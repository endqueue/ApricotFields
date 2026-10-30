
#include "render/aprendbuffers.h"
#include "aprend_internal.hpp"
#include "aprendreflect_internal.hpp"

#include <spudgpu.h>

#include <new>

// Each part is destroyed only as far as it was made: a buffer that failed
// to create may have parts with no view or an empty store. Views go before
// the buffer they are of.
aprend_buffer_t::~aprend_buffer_t() {
	for (aprend_buffer_part &part : this->parts) {
		for (const aprend_buffer_part::stride_view &known : part.vertex_views)
			spudgpu_destroy_buffer_view(known.view);
		if (part.view)
			spudgpu_destroy_buffer_view(part.view);
		aprend_buffer_store_destroy(&part.store);
	}
	// The desc's name is the copy made at creation; the handle's may have
	// been set by the caller since (APRI_SET_DEBUG_NAME), and isn't ours.
	free((char *)this->desc.debug_name);
}

SPUDRESULT aprend_buffer_vertex_view(
    aprend_buffer_part *part,
    uint32_t stride,
    spudgpu_buffer_view *out_view) {
	for (const aprend_buffer_part::stride_view &known : part->vertex_views) {
		if (known.stride != stride)
			continue;
		*out_view = known.view;
		return SPUD_SUCCESS;
	}
	// Room first: with none, no view is made.
	try {
		part->vertex_views.reserve(part->vertex_views.size() + 1);
	} catch (const std::bad_alloc &) {
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	spudgpu_buffer_view_desc vd{};
	vd.base.type                 = SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	vd.parent_buffer             = part->store.buffer;
	vd.offset_from_parent_buffer = 0;
	vd.stride                    = stride;
	vd.size                      = part->store.size;
	spudgpu_buffer_view view     = nullptr;
	SPUDRESULT sr                = spudgpu_create_buffer_view(part->store.buffer, &vd, &view);
	if (SPUDFAIL(sr))
		return sr;
	part->vertex_views.push_back({stride, view});
	*out_view = view;
	return SPUD_SUCCESS;
}

SPUDRESULT aprend_buffer_store_create(
    aprend_buffer_store *store,
    aprend_instance instance,
    uint64_t size,
    SPUDGPU_BUFFER_USAGE usage,
    SPUDGPU_RESOURCE_STATE use_state,
    APREND_BUFFER_WRITES writes) {
	store->instance     = instance;
	store->size         = size;
	store->use_state    = use_state;
	store->written_once = writes == APREND_BUFFER_WRITES_ONCE;

	// A uniform buffer is UPLOAD memory written directly on every device. Any
	// other is written directly on a device whose memory is the system's, and
	// gets the device's memory, written through staging, on one with its own.
	//
	// Written directly, a buffer the GPU only reads is UPLOAD memory. One a
	// shader may write can't be: it needs DEVICE_MAPPABLE, which a device
	// reports having and which is never a device with memory of its own.
	const bool uniform         = (usage & SPUDGPU_BUFFER_USAGE_UNIFORM) != 0;
	const bool shader_writable = (usage & SPUDGPU_BUFFER_USAGE_STORAGE) != 0;
	bool staged                = false;
	if (!uniform)
		staged = shader_writable ? !instance->device_mappable_memory : !instance->unified_memory;

	spudgpu_buffer_desc bd{};
	bd.base.type = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	bd.size      = size;
	if (staged) {
		bd.memory_kind = SPUDGPU_MEMORY_KIND_DEVICE;
		bd.usage       = usage | SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
	} else {
		bd.memory_kind = shader_writable ? SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE : SPUDGPU_MEMORY_KIND_UPLOAD;
		bd.usage       = usage;
	}
	SPUDRESULT sr = spudgpu_create_buffer(instance->desc.device, &bd, &store->buffer);
	if (SPUDFAIL(sr)) {
		store->buffer = nullptr;
		return sr;
	}
	if (!staged) {
		// Mapped once, for life: every write goes through the one mapping.
		sr = spudgpu_map_buffer(store->buffer, 0, size, &store->mapped);
		if (SPUDFAIL(sr)) {
			store->mapped = nullptr;
			spudgpu_destroy_buffer(store->buffer);
			store->buffer = nullptr;
			return sr;
		}
		return SPUD_SUCCESS;
	}
	store->staged = true;
	// A ONCE store stages each write in a buffer of its own, made by the
	// write; only a MANY store keeps one for life.
	if (store->written_once)
		return SPUD_SUCCESS;

	spudgpu_buffer_desc sd{};
	sd.base.type   = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	sd.memory_kind = SPUDGPU_MEMORY_KIND_UPLOAD;
	sd.size        = size;
	sd.usage       = SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	sr             = spudgpu_create_buffer(instance->desc.device, &sd, &store->staging);
	if (SPUDFAIL(sr)) {
		spudgpu_destroy_buffer(store->buffer);
		store->buffer  = nullptr;
		store->staged  = false;
		store->staging = nullptr;
		return sr;
	}
	return SPUD_SUCCESS;
}

void aprend_buffer_store_destroy(aprend_buffer_store *store) {
	// Called once the GPU has finished with the store, so an upload's own
	// buffer, never submitted or long since run, goes at once, and the
	// retire its write promised is given back.
	size_t own = 0;
	for (const aprend_buffer_upload &upload : store->uploads) {
		if (upload.source == store->staging)
			continue;
		spudgpu_destroy_buffer(upload.source);
		++own;
	}
	store->uploads.clear();
	if (own > 0)
		aprend_instance_unreserve_release(store->instance, own);
	if (store->staging)
		spudgpu_destroy_buffer(store->staging);
	if (store->mapped)
		spudgpu_unmap_buffer(store->buffer);
	if (store->buffer)
		spudgpu_destroy_buffer(store->buffer);
	store->mapped  = nullptr;
	store->staging = nullptr;
	store->buffer  = nullptr;
	store->staged  = false;
}

// Copies [size] bytes of [data] to [offset] of [target], through a mapping
// made for the write, and flushes them.
static SPUDRESULT aprend_buffer_write_mapped(spudgpu_buffer target, uint64_t offset, uint64_t size, const void *data) {
	void *mapped  = nullptr;
	SPUDRESULT sr = spudgpu_map_buffer(target, offset, size, &mapped);
	if (SPUDFAIL(sr))
		return sr;
	memcpy(mapped, data, (size_t)size);
	// Every write through a mapping is flushed, on every device: what is
	// written is UPLOAD or DEVICE_MAPPABLE memory, both of which take it.
	sr = spudgpu_flush_buffer(target, offset, size);
	spudgpu_unmap_buffer(target);
	return sr;
}

// A ONCE store's write: an UPLOAD buffer of the write's own, noted to be
// copied and then retired. Its retire is promised here, so the submission
// that copies it can't find the release queue full.
static SPUDRESULT aprend_buffer_store_write_own(
    aprend_buffer_store *store,
    uint64_t offset,
    uint64_t size,
    const void *data) {
	// The note's room first: with no memory for it, nothing is made.
	try {
		store->uploads.reserve(store->uploads.size() + 1);
	} catch (const std::bad_alloc &) {
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	if (aprend_instance_reserve_release(store->instance, 1) != APRESULT_OK)
		return SPUDRESULT_OUT_OF_MEMORY;
	spudgpu_buffer_desc sd{};
	sd.base.type          = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	sd.memory_kind        = SPUDGPU_MEMORY_KIND_UPLOAD;
	sd.size               = size;
	sd.usage              = SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	spudgpu_buffer source = nullptr;
	SPUDRESULT sr         = spudgpu_create_buffer(store->instance->desc.device, &sd, &source);
	if (SPUDFAIL(sr)) {
		aprend_instance_unreserve_release(store->instance, 1);
		return sr;
	}
	sr = aprend_buffer_write_mapped(source, 0, size, data);
	if (SPUDFAIL(sr)) {
		spudgpu_destroy_buffer(source);
		aprend_instance_unreserve_release(store->instance, 1);
		return sr;
	}
	aprend_buffer_upload upload{};
	upload.source        = source;
	upload.source_offset = 0;
	upload.offset        = offset;
	upload.size          = size;
	store->uploads.push_back(upload);
	return SPUD_SUCCESS;
}

// A MANY store's write, into [staging], merged into the ranges it overlaps
// or touches. The list stays sorted and disjoint, so it never holds more
// than one range for each gap between writes, and a range never covers a
// byte nothing wrote.
static SPUDRESULT aprend_buffer_store_write_staging(
    aprend_buffer_store *store,
    uint64_t offset,
    uint64_t size,
    const void *data) {
	// At most one range more: with no memory for it, nothing is written.
	// Written bytes that no range named would never be copied.
	try {
		store->uploads.reserve(store->uploads.size() + 1);
	} catch (const std::bad_alloc &) {
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	SPUDRESULT sr = aprend_buffer_write_mapped(store->staging, offset, size, data);
	if (SPUDFAIL(sr))
		return sr;

	std::vector<aprend_buffer_upload> &uploads = store->uploads;
	uint64_t begin                             = offset;
	uint64_t end                               = offset + size;
	// The first range that ends at or after [begin]: every one before it
	// ends short of the write and is left alone.
	size_t first = 0;
	while (first < uploads.size() && uploads[first].offset + uploads[first].size < begin)
		++first;
	// Every range from [first] that starts at or before [end] overlaps or
	// touches the write, and becomes part of it.
	size_t last = first;
	while (last < uploads.size() && uploads[last].offset <= end) {
		if (uploads[last].offset < begin)
			begin = uploads[last].offset;
		if (uploads[last].offset + uploads[last].size > end)
			end = uploads[last].offset + uploads[last].size;
		++last;
	}
	aprend_buffer_upload merged{};
	merged.source        = store->staging;
	merged.source_offset = begin;
	merged.offset        = begin;
	merged.size          = end - begin;
	if (first == last) {
		// Room was reserved above, so this doesn't allocate.
		uploads.insert(uploads.begin() + (std::ptrdiff_t)first, merged);
		return SPUD_SUCCESS;
	}
	uploads[first] = merged;
	uploads.erase(uploads.begin() + (std::ptrdiff_t)first + 1, uploads.begin() + (std::ptrdiff_t)last);
	return SPUD_SUCCESS;
}

SPUDRESULT aprend_buffer_store_write(
    aprend_buffer_store *store,
    uint64_t offset,
    uint64_t size,
    const void *data) {
	if (store->mapped) {
		memcpy((uint8_t *)store->mapped + offset, data, (size_t)size);
		// The buffer stays mapped; each write is flushed all the same.
		return spudgpu_flush_buffer(store->buffer, offset, size);
	}
	if (store->staging)
		return aprend_buffer_store_write_staging(store, offset, size, data);
	return aprend_buffer_store_write_own(store, offset, size, data);
}

void aprend_buffer_store_uploads_submitted(aprend_buffer_store *store) {
	// Each retire was promised by the write that made the buffer.
	for (const aprend_buffer_upload &upload : store->uploads)
		if (upload.source != store->staging)
			aprend_instance_retire(store->instance, upload.source, &aprend_release_spudgpu_buffer);
	store->uploads.clear();
}

// The SpudGPU usage and the state a buffer of [usage] is used in. A
// storage buffer is in UNORDERED_ACCESS, read and written by shaders; one
// that is also an argument buffer is in that state only while a storage
// slot uses it, and a command list moves it.
static void aprend_buffer_spud_usage(APREND_BUFFER_USAGE usage, SPUDGPU_BUFFER_USAGE *out_usage, SPUDGPU_RESOURCE_STATE *out_state) {
	SPUDGPU_BUFFER_USAGE spud_usage = 0;
	SPUDGPU_RESOURCE_STATE state    = SPUDGPU_RESOURCE_STATE_COMMON;
	if (usage & APREND_BUFFER_USAGE_INDIRECT_ARGUMENTS) {
		spud_usage |= SPUDGPU_BUFFER_USAGE_INDIRECT;
		state = SPUDGPU_RESOURCE_STATE_INDIRECT_ARGUMENT;
	}
	if (usage & APREND_BUFFER_USAGE_VERTEX) {
		spud_usage |= SPUDGPU_BUFFER_USAGE_VERTEX;
		state = SPUDGPU_RESOURCE_STATE_VERTEX_BUFFER;
	}
	if (usage & APREND_BUFFER_USAGE_INDEX) {
		spud_usage |= SPUDGPU_BUFFER_USAGE_INDEX;
		state = SPUDGPU_RESOURCE_STATE_INDEX_BUFFER;
	}
	if (usage & APREND_BUFFER_USAGE_UNIFORM) {
		spud_usage |= SPUDGPU_BUFFER_USAGE_UNIFORM;
		// Read as a uniform buffer: SpudGPU's VERTEX_BUFFER state.
		state = SPUDGPU_RESOURCE_STATE_VERTEX_BUFFER;
	}
	if (usage & APREND_BUFFER_USAGE_STORAGE) {
		spud_usage |= SPUDGPU_BUFFER_USAGE_STORAGE;
		state = SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	*out_usage = spud_usage;
	*out_state = state;
}

// Makes [part]'s store and its view, and writes [data] if there is any.
static APRESULT aprend_buffer_part_create(
    aprend_buffer buffer,
    aprend_buffer_part *part,
    const void *data,
    APREND_ERROR *out_error) {
	const APREND_BUFFER_DESC &desc = buffer->desc;
	SPUDGPU_BUFFER_USAGE spud_usage{};
	SPUDGPU_RESOURCE_STATE use_state{};
	aprend_buffer_spud_usage(desc.usage, &spud_usage, &use_state);
	SPUDRESULT sr = aprend_buffer_store_create(&part->store, buffer->instance, desc.size, spud_usage, use_state, desc.writes);
	if (SPUDFAIL(sr))
		return aprend_fail_spud(out_error, sr, "a buffer of %llu bytes can't be created", (unsigned long long)desc.size);
	part->store.multi_state = (desc.usage & APREND_BUFFER_USAGE_STORAGE) && (desc.usage & APREND_BUFFER_USAGE_INDIRECT_ARGUMENTS);

	// An index buffer is bound through a view at its index stride, a storage
	// buffer through one of no stride.
	if (desc.usage & (APREND_BUFFER_USAGE_INDEX | APREND_BUFFER_USAGE_STORAGE)) {
		spudgpu_buffer_view_desc vd{};
		vd.base.type                 = SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
		vd.parent_buffer             = part->store.buffer;
		vd.offset_from_parent_buffer = 0;
		vd.stride                    = (desc.usage & APREND_BUFFER_USAGE_INDEX) ? (uint64_t)desc.index_stride : 0;
		vd.size                      = desc.size;
		sr                           = spudgpu_create_buffer_view(part->store.buffer, &vd, &part->view);
		if (SPUDFAIL(sr)) {
			part->view = nullptr;
			return aprend_fail_spud(out_error, sr, "a view of the buffer can't be created");
		}
	}

	if (data) {
		sr = aprend_buffer_store_write(&part->store, 0, desc.size, data);
		if (SPUDFAIL(sr))
			return aprend_fail_spud(out_error, sr, "the buffer's initial_data can't be written");
	} else if (part->store.mapped && (desc.usage & APREND_BUFFER_USAGE_UNIFORM)) {
		// A new SpudGPU buffer's contents are undefined, and a block is
		// usually written one uniform at a time: zero it, so a uniform the
		// caller has not written yet reads 0 on every device.
		memset(part->store.mapped, 0, (size_t)desc.size);
		sr = spudgpu_flush_buffer(part->store.buffer, 0, desc.size);
		if (SPUDFAIL(sr))
			return aprend_fail_spud(out_error, sr, "the zeroed block can't be flushed");
	}
	return APRESULT_OK;
}

// The message's name for [buffer]: its debug name in quotes, or "buffer".
static const char *aprend_buffer_label(aprend_buffer buffer, char (&label)[96]) {
	if (!buffer->desc.debug_name)
		return "buffer";
	snprintf(label, sizeof(label), "buffer \"%s\"", buffer->desc.debug_name);
	return label;
}

// A buffer handle with nothing in it yet, named [debug_name], and promised
// its retire if [promise]. [out_buffer] is the caller's, checked.
static APRESULT aprend_buffer_begin(
    aprend_instance instance,
    const char *debug_name,
    bool promise,
    aprend_buffer *out_buffer,
    APREND_ERROR *out_error) {
	char *name = nullptr;
	if (debug_name) {
		const size_t length = strlen(debug_name) + 1;
		name                = (char *)malloc(length);
		if (!name)
			return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the buffer's name");
		memcpy(name, debug_name, length);
	}
	aprend_buffer_t *result = (aprend_buffer_t *)malloc(sizeof(aprend_buffer_t));
	if (!result) {
		free(name);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the buffer");
	}
	result                  = new (result) aprend_buffer_t();
	result->instance        = instance;
	result->desc.debug_name = name;
	result->debug_name      = name;
	// Its destroy's retire, promised now so the destroy can't fail.
	if (promise && aprend_instance_reserve_release(instance, 1) != APRESULT_OK) {
		APREND_DESTRUCT__T(result, aprend_buffer_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the buffer's release");
	}
	*out_buffer = result;
	return APRESULT_OK;
}

// A buffer that failed after aprend_buffer_begin: its promise given back if
// it had one, and nothing on the GPU yet, so freed at once.
static void aprend_buffer_abandon(aprend_buffer buffer, bool promised) {
	if (promised)
		aprend_instance_unreserve_release(buffer->instance, 1);
	APREND_DESTRUCT__T(buffer, aprend_buffer_t);
}

APRESULT aprend_buffer_create_device_uniform(
    aprend_instance instance,
    uint32_t size,
    aprend_buffer *out_buffer,
    APREND_ERROR *out_error) {
	*out_buffer          = nullptr;
	aprend_buffer result = nullptr;
	APRESULT ar          = aprend_buffer_begin(instance, nullptr, false, &result, out_error);
	if (ar != APRESULT_OK)
		return ar;
	result->desc.base         = apri_struct_base(APRI_STRUCT_TYPE_APREND_BUFFER_DESC);
	result->desc.size         = size;
	result->desc.usage        = APREND_BUFFER_USAGE_UNIFORM;
	result->desc.writes       = APREND_BUFFER_WRITES_ONCE;
	result->part_count        = 1;
	aprend_buffer_store &store = result->parts[0].store;
	store.instance            = instance;
	store.size                = size;
	store.use_state           = SPUDGPU_RESOURCE_STATE_VERTEX_BUFFER;
	store.written_once        = true;

	spudgpu_buffer_desc bd{};
	bd.base.type = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	// Read by shaders, written by the copies a submission records ahead of
	// its list: never by the CPU, so in the device's own memory, unmapped.
	bd.memory_kind = SPUDGPU_MEMORY_KIND_DEVICE;
	bd.size        = size;
	bd.usage       = SPUDGPU_BUFFER_USAGE_UNIFORM | SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
	SPUDRESULT sr  = spudgpu_create_buffer(instance->desc.device, &bd, &store.buffer);
	if (SPUDFAIL(sr)) {
		store.buffer = nullptr;
		aprend_buffer_abandon(result, false);
		return aprend_fail_spud(out_error, sr, "a uniform buffer of %u bytes in the device's memory can't be created", size);
	}
	*out_buffer = result;
	return APRESULT_OK;
}

extern "C" {

uint32_t aprend_uniform_type_get_size(APREND_UNIFORM_TYPE type) {
	switch (type) {
	case APREND_UNIFORM_TYPE_BOOL:
	case APREND_UNIFORM_TYPE_INT:
	case APREND_UNIFORM_TYPE_UINT:
	case APREND_UNIFORM_TYPE_FLOAT:
		return 4;
	case APREND_UNIFORM_TYPE_INT2:
	case APREND_UNIFORM_TYPE_UINT2:
	case APREND_UNIFORM_TYPE_VEC2:
		return 8;
	case APREND_UNIFORM_TYPE_INT3:
	case APREND_UNIFORM_TYPE_UINT3:
	case APREND_UNIFORM_TYPE_VEC3:
		return 12;
	case APREND_UNIFORM_TYPE_INT4:
	case APREND_UNIFORM_TYPE_UINT4:
	case APREND_UNIFORM_TYPE_VEC4:
		return 16;
	case APREND_UNIFORM_TYPE_MAT4:
		return 64;
	case APREND_UNIFORM_TYPE_MAT2:
		return 16;
	case APREND_UNIFORM_TYPE_MAT3:
		return 36;
	default:
		return 0;
	}
}
// What one [type] takes in a std140 block: its size, except that a matrix
// keeps each column 16 bytes apart.
static uint32_t aprend_uniform_std140_size(APREND_UNIFORM_TYPE type) {
	const uint32_t columns = aprend_uniform_type_columns(type);
	return columns > 1 ? columns * 16 : aprend_uniform_type_get_size(type);
}
// The distance between two elements of an array of [type] in a std140 block:
// what one takes, rounded up to 16.
static uint32_t aprend_uniform_array_stride(APREND_UNIFORM_TYPE type) { return (aprend_uniform_std140_size(type) + 15) & ~15u; }

// The bytes [u] takes in a std140 block from its offset. 0 and 1 are both
// "not an array".
static uint64_t aprend_uniform_footprint(const APREND_UNIFORM &u) {
	if (u.array_count <= 1)
		return aprend_uniform_std140_size(u.type);
	return (uint64_t)aprend_uniform_array_stride(u.type) * u.array_count;
}

// [u] as a block member laid out by std140, for aprend_write_block_member.
static APREND_SHADER_MEMBER aprend_uniform_as_member(const APREND_UNIFORM &u) {
	APREND_SHADER_MEMBER member{};
	member.type          = u.type;
	member.offset        = u.offset;
	member.array_count   = u.array_count > 1 ? u.array_count : 1;
	member.array_stride  = u.array_count > 1 ? aprend_uniform_array_stride(u.type) : 0;
	member.matrix_stride = aprend_uniform_type_columns(u.type) > 1 ? 16 : 0;
	member.size          = (uint32_t)aprend_uniform_footprint(u);
	return member;
}

APRESULT aprend_uniform_layout_get_uniform_index(const APREND_UNIFORM_LAYOUT *layout, const char *name, uint32_t *out_index) {
	if (out_index)
		*out_index = UINT32_MAX;
	if (!layout)
		return APRESULT_INVALID_ARGUMENT;
	if (!name)
		return APRESULT_INVALID_ARGUMENT;
	if (!out_index)
		return APRESULT_INVALID_ARGUMENT;
	if (layout->count > 0 && !layout->uniforms)
		return APRESULT_INVALID_ARGUMENT;
	for (uint32_t i = 0; i < layout->count; ++i) {
		const char *uniform_name = layout->uniforms[i].name;
		if (uniform_name && strcmp(uniform_name, name) == 0) {
			*out_index = i;
			return APRESULT_OK;
		}
	}
	return APRESULT_NOT_FOUND;
}

APRESULT aprend_uniform_layout_get_size(const APREND_UNIFORM_LAYOUT *layout, uint32_t *out_size, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_size)
		*out_size = 0;
	if (!layout)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "layout is NULL");
	if (!out_size)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_size is NULL");
	if (layout->count > 0 && !layout->uniforms)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "layout's uniforms is NULL with a count of %u", layout->count);
	// The block ends where its furthest uniform does, rounded up to 16 as
	// std140 rounds a block. Offsets are the caller's; nothing is placed here.
	uint64_t size = 0;
	for (uint32_t i = 0; i < layout->count; ++i) {
		const APREND_UNIFORM &u = layout->uniforms[i];
		if (aprend_uniform_type_get_size(u.type) == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "uniform %u has type %u, which is not an APREND_UNIFORM_TYPE", i, (unsigned)u.type);
		const uint64_t uniform_end = (uint64_t)u.offset + aprend_uniform_footprint(u);
		if (uniform_end > size)
			size = uniform_end;
	}
	// The block as the shader declares it, where the layout says.
	if (layout->size > size)
		size = layout->size;
	if (size == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the layout has no uniforms and a size of 0");
	if (size > UINT32_MAX - 15)
		return aprend_fail(out_error, APRESULT_LIMIT, "the block ends past what 32 bits can address");
	*out_size = ((uint32_t)size + 15) & ~15u;
	return APRESULT_OK;
}

APRESULT aprend_buffer_create(aprend_instance instance, const APREND_BUFFER_DESC *desc, aprend_buffer *out_buffer, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_buffer)
		*out_buffer = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_buffer_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	APRESULT ar = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_BUFFER_DESC);
	if (ar != APRESULT_OK)
		return ar;
	if (!out_buffer)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_buffer is NULL");
	if (desc->size == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "size is 0");
	if (desc->usage == APREND_BUFFER_USAGE_NONE)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "usage is APREND_BUFFER_USAGE_NONE");
	const APREND_BUFFER_USAGE every_usage = APREND_BUFFER_USAGE_VERTEX | APREND_BUFFER_USAGE_INDEX | APREND_BUFFER_USAGE_UNIFORM |
	                                        APREND_BUFFER_USAGE_STORAGE | APREND_BUFFER_USAGE_INDIRECT_ARGUMENTS;
	if (desc->usage & ~every_usage)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "usage 0x%x has a bit that is not an APREND_BUFFER_USAGE", (unsigned)desc->usage);
	// One bit, or storage with indirect arguments: the states a command list
	// knows how to move a buffer between.
	const bool one_bit = (desc->usage & (desc->usage - 1)) == 0;
	if (!one_bit && desc->usage != (APREND_BUFFER_USAGE_STORAGE | APREND_BUFFER_USAGE_INDIRECT_ARGUMENTS))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "usage 0x%x combines uses a buffer can't have together (one bit, or STORAGE with INDIRECT_ARGUMENTS)", (unsigned)desc->usage);
	if (desc->writes != APREND_BUFFER_WRITES_ONCE && desc->writes != APREND_BUFFER_WRITES_MANY)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "writes %u is not APREND_BUFFER_WRITES_ONCE or _MANY", (unsigned)desc->writes);
	if (desc->usage & APREND_BUFFER_USAGE_INDEX) {
		if (desc->index_stride != APREND_INDEX_STRIDE_UINT16 && desc->index_stride != APREND_INDEX_STRIDE_UINT32)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "index_stride %u of an index buffer is not UINT16 or UINT32", (unsigned)desc->index_stride);
		if (desc->size % (uint64_t)desc->index_stride != 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "size %llu is not a multiple of index_stride %u", (unsigned long long)desc->size, (unsigned)desc->index_stride);
	} else if (desc->index_stride != APREND_INDEX_STRIDE_NONE) {
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "index_stride %u of a buffer without APREND_BUFFER_USAGE_INDEX is not NONE", (unsigned)desc->index_stride);
	}

	aprend_buffer result = nullptr;
	ar                   = aprend_buffer_begin(instance, desc->debug_name, true, &result, out_error);
	if (ar != APRESULT_OK)
		return ar;
	const char *name           = result->desc.debug_name;
	result->desc               = *desc;
	result->desc.base          = apri_struct_base(APRI_STRUCT_TYPE_APREND_BUFFER_DESC);
	result->desc.initial_data  = nullptr;
	result->desc.debug_name    = name;
	result->part_count         = desc->per_frame ? instance->desc.frames_in_flight : 1;
	for (uint32_t i = 0; i < result->part_count; ++i) {
		ar = aprend_buffer_part_create(result, &result->parts[i], desc->initial_data, out_error);
		if (ar != APRESULT_OK) {
			aprend_buffer_abandon(result, true);
			return ar;
		}
	}
	*out_buffer = result;
	return APRESULT_OK;
}

void aprend_buffer_destroy(aprend_buffer buffer) {
	if (!buffer)
		return;
	// A list compiled against any copy and not submitted would copy its
	// writes, or bind it: refused at submit.
	for (uint32_t i = 0; i < buffer->part_count; ++i)
		aprend_instance_forget(buffer->instance, &buffer->parts[i].store);
	aprend_instance_forget(buffer->instance, buffer);
	// Released once the GPU has finished everything submitted so far, with
	// the retire its create promised.
	aprend_instance_retire(buffer->instance, buffer, &aprend_release_handle<aprend_buffer_t>);
}

APRESULT aprend_buffer_get_desc(aprend_buffer buffer, APREND_BUFFER_DESC *out_desc) {
	if (!buffer)
		return APRESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return APRESULT_INVALID_ARGUMENT;
	*out_desc = buffer->desc;
	return APRESULT_OK;
}

APRESULT aprend_buffer_write(aprend_buffer buffer, uint64_t offset, const void *data, uint64_t data_size, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!buffer)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "buffer is NULL");
	APREND_ENTER(buffer->instance, "aprend_buffer_write");
	char label[96];
	if (!data)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "data is NULL");
	if (data_size == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "data_size is 0");
	const uint64_t size = buffer->desc.size;
	if (offset > size)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "offset %llu is past the %s's %llu bytes", (unsigned long long)offset, aprend_buffer_label(buffer, label), (unsigned long long)size);
	if (data_size > size - offset)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "%llu bytes at offset %llu run past the %s's %llu", (unsigned long long)data_size, (unsigned long long)offset, aprend_buffer_label(buffer, label), (unsigned long long)size);
	if (buffer->desc.writes == APREND_BUFFER_WRITES_ONCE)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the %s was created APREND_BUFFER_WRITES_ONCE", aprend_buffer_label(buffer, label));
	aprend_buffer_part *part = aprend_buffer_part_of(buffer, buffer->instance->frame_index);
	const SPUDRESULT sr      = aprend_buffer_store_write(&part->store, offset, data_size, data);
	if (SPUDFAIL(sr))
		return aprend_fail_spud(out_error, sr, "the bytes can't be written to the %s", aprend_buffer_label(buffer, label));
	return APRESULT_OK;
}

APRESULT aprend_buffer_write_uniform(aprend_buffer buffer, const APREND_UNIFORM_LAYOUT *layout, uint32_t uniform_index, const void *data, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!buffer)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "buffer is NULL");
	APREND_ENTER(buffer->instance, "aprend_buffer_write_uniform");
	char label[96];
	if (!(buffer->desc.usage & APREND_BUFFER_USAGE_UNIFORM))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the %s has no APREND_BUFFER_USAGE_UNIFORM", aprend_buffer_label(buffer, label));
	if (buffer->desc.writes == APREND_BUFFER_WRITES_ONCE)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the %s was created APREND_BUFFER_WRITES_ONCE", aprend_buffer_label(buffer, label));
	if (!layout)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "layout is NULL");
	if (!layout->uniforms)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "layout's uniforms is NULL");
	if (uniform_index >= layout->count)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "uniform_index %u, and the layout has %u uniforms", uniform_index, layout->count);
	if (!data)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "data is NULL");
	const APREND_UNIFORM &uniform = layout->uniforms[uniform_index];
	if (aprend_uniform_type_get_size(uniform.type) == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "uniform %u has type %u, which is not an APREND_UNIFORM_TYPE", uniform_index, (unsigned)uniform.type);
	const APREND_SHADER_MEMBER member = aprend_uniform_as_member(uniform);
	if ((uint64_t)member.offset + member.size > buffer->desc.size)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "uniform %u ends at byte %llu, past the %s's %llu", uniform_index, (unsigned long long)member.offset + member.size, aprend_buffer_label(buffer, label), (unsigned long long)buffer->desc.size);
	// A uniform buffer is mapped for life on every device. The caller's
	// elements and columns are tightly packed, the block's are where std140
	// puts them.
	aprend_buffer_part *part = aprend_buffer_part_of(buffer, buffer->instance->frame_index);
	aprend_write_block_member(member, (uint8_t *)part->store.mapped, data);
	const SPUDRESULT sr = spudgpu_flush_buffer(part->store.buffer, member.offset, member.size);
	if (SPUDFAIL(sr))
		return aprend_fail_spud(out_error, sr, "the uniform written to the %s can't be flushed", aprend_buffer_label(buffer, label));
	return APRESULT_OK;
}
}
