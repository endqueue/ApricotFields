
#include "render/rbtrendbuffers.h"
#include "rbtrend_internal.hpp"
#include "rbtrendreflect_internal.hpp"

#include <crgpu.h>

#include <new>

// Each part is destroyed only as far as it was made: a buffer that failed
// to create may have parts with no view or an empty store. Views go before
// the buffer they are of.
rbtrend_buffer_t::~rbtrend_buffer_t() {
	for (rbtrend_buffer_part &part : this->parts) {
		for (const rbtrend_buffer_part::stride_view &known : part.vertex_views)
			crgpu_buffer_view_destroy(known.view);
		if (part.view)
			crgpu_buffer_view_destroy(part.view);
		rbtrend_buffer_store_destroy(&part.store);
	}
	// The desc's name is the copy made at creation; the handle's may have
	// been set by the caller since (RBT_SET_DEBUG_NAME), and isn't ours.
	free((char *)this->desc.debug_name);
}

CR_RESULT rbtrend_buffer_vertex_view(
    rbtrend_buffer_part *part,
    uint32_t stride,
    crgpu_buffer_view *out_view) {
	for (const rbtrend_buffer_part::stride_view &known : part->vertex_views) {
		if (known.stride != stride)
			continue;
		*out_view = known.view;
		return CR_SUCCESS;
	}
	// Room first: with none, no view is made.
	try {
		part->vertex_views.reserve(part->vertex_views.size() + 1);
	} catch (const std::bad_alloc &) {
		return CR_RESULT_OUT_OF_MEMORY;
	}
	crgpu_buffer_view_desc vd{};
	vd.base.type                 = CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	vd.parent_buffer             = part->store.buffer;
	vd.offset_from_parent_buffer = 0;
	vd.stride                    = stride;
	vd.size                      = part->store.size;
	crgpu_buffer_view view     = nullptr;
	CR_RESULT sr                = crgpu_buffer_view_create(part->store.buffer, &vd, &view);
	if (CR_FAILED(sr))
		return sr;
	part->vertex_views.push_back({stride, view});
	*out_view = view;
	return CR_SUCCESS;
}

CR_RESULT rbtrend_buffer_store_create(
    rbtrend_buffer_store *store,
    rbtrend_instance instance,
    uint64_t size,
    CRGPU_BUFFER_USAGE usage,
    CRGPU_RESOURCE_STATE use_state,
    RBTREND_BUFFER_WRITES writes) {
	store->instance     = instance;
	store->size         = size;
	store->use_state    = use_state;
	store->written_once = writes == RBTREND_BUFFER_WRITES_ONCE;

	// A uniform buffer is UPLOAD memory written directly on every device. Any
	// other is written directly on a device whose memory is the system's, and
	// gets the device's memory, written through staging, on one with its own.
	//
	// Written directly, a buffer the GPU only reads is UPLOAD memory. One a
	// shader may write can't be: it needs DEVICE_MAPPABLE, which a device
	// reports having and which is never a device with memory of its own.
	const bool uniform         = (usage & CRGPU_BUFFER_USAGE_UNIFORM) != 0;
	const bool shader_writable = (usage & CRGPU_BUFFER_USAGE_STORAGE) != 0;
	bool staged                = false;
	if (!uniform)
		staged = shader_writable ? !instance->device_mappable_memory : !instance->unified_memory;

	crgpu_buffer_desc bd{};
	bd.base.type = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	bd.size      = size;
	if (staged) {
		bd.memory_kind = CRGPU_MEMORY_KIND_DEVICE;
		bd.usage       = usage | CRGPU_BUFFER_USAGE_TRANSFER_DST;
	} else {
		bd.memory_kind = shader_writable ? CRGPU_MEMORY_KIND_DEVICE_MAPPABLE : CRGPU_MEMORY_KIND_UPLOAD;
		bd.usage       = usage;
	}
	CR_RESULT sr = crgpu_buffer_create(instance->desc.device, &bd, &store->buffer);
	if (CR_FAILED(sr)) {
		store->buffer = nullptr;
		return sr;
	}
	if (!staged) {
		// Mapped once, for life: every write goes through the one mapping.
		sr = crgpu_buffer_map(store->buffer, 0, size, &store->mapped);
		if (CR_FAILED(sr)) {
			store->mapped = nullptr;
			crgpu_buffer_destroy(store->buffer);
			store->buffer = nullptr;
			return sr;
		}
		return CR_SUCCESS;
	}
	store->staged = true;
	// A ONCE store stages each write in a buffer of its own, made by the
	// write; only a MANY store keeps one for life.
	if (store->written_once)
		return CR_SUCCESS;

	crgpu_buffer_desc sd{};
	sd.base.type   = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	sd.memory_kind = CRGPU_MEMORY_KIND_UPLOAD;
	sd.size        = size;
	sd.usage       = CRGPU_BUFFER_USAGE_TRANSFER_SRC;
	sr             = crgpu_buffer_create(instance->desc.device, &sd, &store->staging);
	if (CR_FAILED(sr)) {
		crgpu_buffer_destroy(store->buffer);
		store->buffer  = nullptr;
		store->staged  = false;
		store->staging = nullptr;
		return sr;
	}
	return CR_SUCCESS;
}

void rbtrend_buffer_store_destroy(rbtrend_buffer_store *store) {
	// Called once the GPU has finished with the store, so an upload's own
	// buffer, never submitted or long since run, goes at once, and the
	// retire its write promised is given back.
	size_t own = 0;
	for (const rbtrend_buffer_upload &upload : store->uploads) {
		if (upload.source == store->staging)
			continue;
		crgpu_buffer_destroy(upload.source);
		++own;
	}
	store->uploads.clear();
	if (own > 0)
		rbtrend_instance_unreserve_release(store->instance, own);
	if (store->staging)
		crgpu_buffer_destroy(store->staging);
	if (store->mapped)
		crgpu_buffer_unmap(store->buffer);
	if (store->buffer)
		crgpu_buffer_destroy(store->buffer);
	store->mapped  = nullptr;
	store->staging = nullptr;
	store->buffer  = nullptr;
	store->staged  = false;
}

// Copies [size] bytes of [data] to [offset] of [target], through a mapping
// made for the write, and flushes them.
static CR_RESULT rbtrend_buffer_write_mapped(crgpu_buffer target, uint64_t offset, uint64_t size, const void *data) {
	void *mapped  = nullptr;
	CR_RESULT sr = crgpu_buffer_map(target, offset, size, &mapped);
	if (CR_FAILED(sr))
		return sr;
	memcpy(mapped, data, (size_t)size);
	// Every write through a mapping is flushed, on every device: what is
	// written is UPLOAD or DEVICE_MAPPABLE memory, both of which take it.
	sr = crgpu_buffer_flush(target, offset, size);
	crgpu_buffer_unmap(target);
	return sr;
}

// A ONCE store's write: an UPLOAD buffer of the write's own, noted to be
// copied and then retired. Its retire is promised here, so the submission
// that copies it can't find the release queue full.
static CR_RESULT rbtrend_buffer_store_write_own(
    rbtrend_buffer_store *store,
    uint64_t offset,
    uint64_t size,
    const void *data) {
	// The note's room first: with no memory for it, nothing is made.
	try {
		store->uploads.reserve(store->uploads.size() + 1);
	} catch (const std::bad_alloc &) {
		return CR_RESULT_OUT_OF_MEMORY;
	}
	if (rbtrend_instance_reserve_release(store->instance, 1) != RBT_RESULT_OK)
		return CR_RESULT_OUT_OF_MEMORY;
	crgpu_buffer_desc sd{};
	sd.base.type          = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	sd.memory_kind        = CRGPU_MEMORY_KIND_UPLOAD;
	sd.size               = size;
	sd.usage              = CRGPU_BUFFER_USAGE_TRANSFER_SRC;
	crgpu_buffer source = nullptr;
	CR_RESULT sr         = crgpu_buffer_create(store->instance->desc.device, &sd, &source);
	if (CR_FAILED(sr)) {
		rbtrend_instance_unreserve_release(store->instance, 1);
		return sr;
	}
	sr = rbtrend_buffer_write_mapped(source, 0, size, data);
	if (CR_FAILED(sr)) {
		crgpu_buffer_destroy(source);
		rbtrend_instance_unreserve_release(store->instance, 1);
		return sr;
	}
	rbtrend_buffer_upload upload{};
	upload.source        = source;
	upload.source_offset = 0;
	upload.offset        = offset;
	upload.size          = size;
	store->uploads.push_back(upload);
	return CR_SUCCESS;
}

// A MANY store's write, into [staging], merged into the ranges it overlaps
// or touches. The list stays sorted and disjoint, so it never holds more
// than one range for each gap between writes, and a range never covers a
// byte nothing wrote.
static CR_RESULT rbtrend_buffer_store_write_staging(
    rbtrend_buffer_store *store,
    uint64_t offset,
    uint64_t size,
    const void *data) {
	// At most one range more: with no memory for it, nothing is written.
	// Written bytes that no range named would never be copied.
	try {
		store->uploads.reserve(store->uploads.size() + 1);
	} catch (const std::bad_alloc &) {
		return CR_RESULT_OUT_OF_MEMORY;
	}
	CR_RESULT sr = rbtrend_buffer_write_mapped(store->staging, offset, size, data);
	if (CR_FAILED(sr))
		return sr;

	std::vector<rbtrend_buffer_upload> &uploads = store->uploads;
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
	rbtrend_buffer_upload merged{};
	merged.source        = store->staging;
	merged.source_offset = begin;
	merged.offset        = begin;
	merged.size          = end - begin;
	if (first == last) {
		// Room was reserved above, so this doesn't allocate.
		uploads.insert(uploads.begin() + (std::ptrdiff_t)first, merged);
		return CR_SUCCESS;
	}
	uploads[first] = merged;
	uploads.erase(uploads.begin() + (std::ptrdiff_t)first + 1, uploads.begin() + (std::ptrdiff_t)last);
	return CR_SUCCESS;
}

CR_RESULT rbtrend_buffer_store_write(
    rbtrend_buffer_store *store,
    uint64_t offset,
    uint64_t size,
    const void *data) {
	if (store->mapped) {
		memcpy((uint8_t *)store->mapped + offset, data, (size_t)size);
		// The buffer stays mapped; each write is flushed all the same.
		return crgpu_buffer_flush(store->buffer, offset, size);
	}
	if (store->staging)
		return rbtrend_buffer_store_write_staging(store, offset, size, data);
	return rbtrend_buffer_store_write_own(store, offset, size, data);
}

void rbtrend_buffer_store_uploads_submitted(rbtrend_buffer_store *store) {
	// Each retire was promised by the write that made the buffer.
	for (const rbtrend_buffer_upload &upload : store->uploads)
		if (upload.source != store->staging)
			rbtrend_instance_retire(store->instance, upload.source, &rbtrend_release_crgpu_buffer);
	store->uploads.clear();
}

// The Carrottus GPU usage and the state a buffer of [usage] is used in. A
// storage buffer is in UNORDERED_ACCESS, read and written by shaders; one
// that is also an argument buffer is in that state only while a storage
// slot uses it, and a command list moves it.
static void rbtrend_buffer_cr_usage(RBTREND_BUFFER_USAGE usage, CRGPU_BUFFER_USAGE *out_usage, CRGPU_RESOURCE_STATE *out_state) {
	CRGPU_BUFFER_USAGE cr_usage = 0;
	CRGPU_RESOURCE_STATE state    = CRGPU_RESOURCE_STATE_COMMON;
	if (usage & RBTREND_BUFFER_USAGE_INDIRECT_ARGUMENTS) {
		cr_usage |= CRGPU_BUFFER_USAGE_INDIRECT;
		state = CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT;
	}
	// A vertex or index buffer may also be a BLAS's geometry
	// (rbtrendraytracing.h): read by its builds, and copied from into the
	// geometry arena a path trace reads (card A41). Carrottus GPU allows the build
	// input usage on every device (card S23).
	if (usage & RBTREND_BUFFER_USAGE_VERTEX) {
		cr_usage |= CRGPU_BUFFER_USAGE_VERTEX | CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT | CRGPU_BUFFER_USAGE_TRANSFER_SRC;
		state = CRGPU_RESOURCE_STATE_VERTEX_BUFFER;
	}
	if (usage & RBTREND_BUFFER_USAGE_INDEX) {
		cr_usage |= CRGPU_BUFFER_USAGE_INDEX | CRGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT | CRGPU_BUFFER_USAGE_TRANSFER_SRC;
		state = CRGPU_RESOURCE_STATE_INDEX_BUFFER;
	}
	if (usage & RBTREND_BUFFER_USAGE_UNIFORM) {
		cr_usage |= CRGPU_BUFFER_USAGE_UNIFORM;
		// Read as a uniform buffer: Carrottus GPU's VERTEX_BUFFER state.
		state = CRGPU_RESOURCE_STATE_VERTEX_BUFFER;
	}
	if (usage & RBTREND_BUFFER_USAGE_STORAGE) {
		cr_usage |= CRGPU_BUFFER_USAGE_STORAGE;
		state = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	*out_usage = cr_usage;
	*out_state = state;
}

// Makes [part]'s store and its view, and writes [data] if there is any.
static RBT_RESULT rbtrend_buffer_part_create(
    rbtrend_buffer buffer,
    rbtrend_buffer_part *part,
    const void *data,
    RBTREND_ERROR *out_error) {
	const RBTREND_BUFFER_DESC &desc = buffer->desc;
	CRGPU_BUFFER_USAGE cr_usage{};
	CRGPU_RESOURCE_STATE use_state{};
	rbtrend_buffer_cr_usage(desc.usage, &cr_usage, &use_state);
	CR_RESULT sr = rbtrend_buffer_store_create(&part->store, buffer->instance, desc.size, cr_usage, use_state, desc.writes);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(out_error, sr, "a buffer of %llu bytes can't be created", (unsigned long long)desc.size);
	part->store.multi_state = (desc.usage & RBTREND_BUFFER_USAGE_STORAGE) && (desc.usage & RBTREND_BUFFER_USAGE_INDIRECT_ARGUMENTS);

	// An index buffer is bound through a view at its index stride, a storage
	// buffer through one of no stride.
	if (desc.usage & (RBTREND_BUFFER_USAGE_INDEX | RBTREND_BUFFER_USAGE_STORAGE)) {
		crgpu_buffer_view_desc vd{};
		vd.base.type                 = CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
		vd.parent_buffer             = part->store.buffer;
		vd.offset_from_parent_buffer = 0;
		vd.stride                    = (desc.usage & RBTREND_BUFFER_USAGE_INDEX) ? (uint64_t)desc.index_stride : 0;
		vd.size                      = desc.size;
		sr                           = crgpu_buffer_view_create(part->store.buffer, &vd, &part->view);
		if (CR_FAILED(sr)) {
			part->view = nullptr;
			return rbtrend_fail_cr(out_error, sr, "a view of the buffer can't be created");
		}
	}

	if (data) {
		sr = rbtrend_buffer_store_write(&part->store, 0, desc.size, data);
		if (CR_FAILED(sr))
			return rbtrend_fail_cr(out_error, sr, "the buffer's initial_data can't be written");
	} else if (part->store.mapped && (desc.usage & RBTREND_BUFFER_USAGE_UNIFORM)) {
		// A new Carrottus GPU buffer's contents are undefined, and a block is
		// usually written one uniform at a time: zero it, so a uniform the
		// caller has not written yet reads 0 on every device.
		memset(part->store.mapped, 0, (size_t)desc.size);
		sr = crgpu_buffer_flush(part->store.buffer, 0, desc.size);
		if (CR_FAILED(sr))
			return rbtrend_fail_cr(out_error, sr, "the zeroed block can't be flushed");
	}
	return RBT_RESULT_OK;
}

// The message's name for [buffer]: its debug name in quotes, or "buffer".
static const char *rbtrend_buffer_label(rbtrend_buffer buffer, char (&label)[96]) {
	if (!buffer->desc.debug_name)
		return "buffer";
	snprintf(label, sizeof(label), "buffer \"%s\"", buffer->desc.debug_name);
	return label;
}

// A buffer handle with nothing in it yet, named [debug_name], and promised
// its retire if [promise]. [out_buffer] is the caller's, checked.
static RBT_RESULT rbtrend_buffer_begin(
    rbtrend_instance instance,
    const char *debug_name,
    bool promise,
    rbtrend_buffer *out_buffer,
    RBTREND_ERROR *out_error) {
	char *name = nullptr;
	if (debug_name) {
		const size_t length = strlen(debug_name) + 1;
		name                = (char *)malloc(length);
		if (!name)
			return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the buffer's name");
		memcpy(name, debug_name, length);
	}
	rbtrend_buffer_t *result = (rbtrend_buffer_t *)malloc(sizeof(rbtrend_buffer_t));
	if (!result) {
		free(name);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the buffer");
	}
	result                  = new (result) rbtrend_buffer_t();
	result->instance        = instance;
	result->serial          = rbtrend_instance_next_serial(instance);
	result->desc.debug_name = name;
	result->debug_name      = name;
	// Its destroy's retire, promised now so the destroy can't fail.
	if (promise && rbtrend_instance_reserve_release(instance, 1) != RBT_RESULT_OK) {
		RBTREND_DESTRUCT__T(result, rbtrend_buffer_t);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the buffer's release");
	}
	*out_buffer = result;
	return RBT_RESULT_OK;
}

// A buffer that failed after rbtrend_buffer_begin: its promise given back if
// it had one, and nothing on the GPU yet, so freed at once.
static void rbtrend_buffer_abandon(rbtrend_buffer buffer, bool promised) {
	if (promised)
		rbtrend_instance_unreserve_release(buffer->instance, 1);
	RBTREND_DESTRUCT__T(buffer, rbtrend_buffer_t);
}

RBT_RESULT rbtrend_buffer_create_device_uniform(
    rbtrend_instance instance,
    uint32_t size,
    rbtrend_buffer *out_buffer,
    RBTREND_ERROR *out_error) {
	*out_buffer          = nullptr;
	rbtrend_buffer result = nullptr;
	RBT_RESULT ar          = rbtrend_buffer_begin(instance, nullptr, false, &result, out_error);
	if (ar != RBT_RESULT_OK)
		return ar;
	result->desc.base         = rbt_struct_base(RBT_STRUCT_TYPE_REND_BUFFER_DESC);
	result->desc.size         = size;
	result->desc.usage        = RBTREND_BUFFER_USAGE_UNIFORM;
	result->desc.writes       = RBTREND_BUFFER_WRITES_ONCE;
	result->part_count        = 1;
	rbtrend_buffer_store &store = result->parts[0].store;
	store.instance            = instance;
	store.size                = size;
	store.use_state           = CRGPU_RESOURCE_STATE_VERTEX_BUFFER;
	store.written_once        = true;

	crgpu_buffer_desc bd{};
	bd.base.type = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	// Read by shaders, written by the copies a submission records ahead of
	// its list: never by the CPU, so in the device's own memory, unmapped.
	bd.memory_kind = CRGPU_MEMORY_KIND_DEVICE;
	bd.size        = size;
	bd.usage       = CRGPU_BUFFER_USAGE_UNIFORM | CRGPU_BUFFER_USAGE_TRANSFER_DST;
	CR_RESULT sr  = crgpu_buffer_create(instance->desc.device, &bd, &store.buffer);
	if (CR_FAILED(sr)) {
		store.buffer = nullptr;
		rbtrend_buffer_abandon(result, false);
		return rbtrend_fail_cr(out_error, sr, "a uniform buffer of %u bytes in the device's memory can't be created", size);
	}
	*out_buffer = result;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_buffer_create_device_storage(
    rbtrend_instance instance,
    uint64_t size,
    CRGPU_BUFFER_USAGE extra_usage,
    const char *debug_name,
    rbtrend_buffer *out_buffer,
    RBTREND_ERROR *out_error) {
	*out_buffer          = nullptr;
	rbtrend_buffer result = nullptr;
	RBT_RESULT ar          = rbtrend_buffer_begin(instance, debug_name, true, &result, out_error);
	if (ar != RBT_RESULT_OK)
		return ar;
	result->desc.base          = rbt_struct_base(RBT_STRUCT_TYPE_REND_BUFFER_DESC);
	result->desc.size          = size;
	result->desc.usage         = RBTREND_BUFFER_USAGE_STORAGE;
	result->desc.writes        = RBTREND_BUFFER_WRITES_ONCE;
	result->part_count         = 1;
	rbtrend_buffer_store &store = result->parts[0].store;
	store.instance             = instance;
	store.size                 = size;
	store.use_state            = CRGPU_RESOURCE_STATE_UNORDERED_ACCESS;
	store.written_once         = true;
	// Copied into, built from, read as a size's destination as well as a
	// storage slot: a command list tracks its state and moves it.
	store.multi_state = true;

	crgpu_buffer_desc bd{};
	bd.base.type = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	// Written by the GPU alone (copies and Rend's shaders), never mapped.
	bd.memory_kind = CRGPU_MEMORY_KIND_DEVICE;
	bd.size        = size;
	bd.usage       = CRGPU_BUFFER_USAGE_STORAGE | CRGPU_BUFFER_USAGE_TRANSFER_DST | CRGPU_BUFFER_USAGE_TRANSFER_SRC | extra_usage;
	CR_RESULT sr  = crgpu_buffer_create(instance->desc.device, &bd, &store.buffer);
	if (CR_FAILED(sr)) {
		store.buffer = nullptr;
		rbtrend_buffer_abandon(result, true);
		return rbtrend_fail_cr(out_error, sr, "a storage buffer of %llu bytes in the device's memory can't be created", (unsigned long long)size);
	}
	*out_buffer = result;
	return RBT_RESULT_OK;
}

extern "C" {

uint32_t rbtrend_uniform_type_get_size(RBTREND_UNIFORM_TYPE type) {
	switch (type) {
	case RBTREND_UNIFORM_TYPE_BOOL:
	case RBTREND_UNIFORM_TYPE_INT:
	case RBTREND_UNIFORM_TYPE_UINT:
	case RBTREND_UNIFORM_TYPE_FLOAT:
		return 4;
	case RBTREND_UNIFORM_TYPE_INT2:
	case RBTREND_UNIFORM_TYPE_UINT2:
	case RBTREND_UNIFORM_TYPE_VEC2:
		return 8;
	case RBTREND_UNIFORM_TYPE_INT3:
	case RBTREND_UNIFORM_TYPE_UINT3:
	case RBTREND_UNIFORM_TYPE_VEC3:
		return 12;
	case RBTREND_UNIFORM_TYPE_INT4:
	case RBTREND_UNIFORM_TYPE_UINT4:
	case RBTREND_UNIFORM_TYPE_VEC4:
		return 16;
	case RBTREND_UNIFORM_TYPE_MAT4:
		return 64;
	case RBTREND_UNIFORM_TYPE_MAT2:
		return 16;
	case RBTREND_UNIFORM_TYPE_MAT3:
		return 36;
	default:
		return 0;
	}
}
// What one [type] takes in a std140 block: its size, except that a matrix
// keeps each column 16 bytes apart.
static uint32_t rbtrend_uniform_std140_size(RBTREND_UNIFORM_TYPE type) {
	const uint32_t columns = rbtrend_uniform_type_columns(type);
	return columns > 1 ? columns * 16 : rbtrend_uniform_type_get_size(type);
}
// The distance between two elements of an array of [type] in a std140 block:
// what one takes, rounded up to 16.
static uint32_t rbtrend_uniform_array_stride(RBTREND_UNIFORM_TYPE type) { return (rbtrend_uniform_std140_size(type) + 15) & ~15u; }

// The bytes [u] takes in a std140 block from its offset. 0 and 1 are both
// "not an array".
static uint64_t rbtrend_uniform_footprint(const RBTREND_UNIFORM &u) {
	if (u.array_count <= 1)
		return rbtrend_uniform_std140_size(u.type);
	return (uint64_t)rbtrend_uniform_array_stride(u.type) * u.array_count;
}

// [u] as a block member laid out by std140, for rbtrend_write_block_member.
static RBTREND_SHADER_MEMBER rbtrend_uniform_as_member(const RBTREND_UNIFORM &u) {
	RBTREND_SHADER_MEMBER member{};
	member.type          = u.type;
	member.offset        = u.offset;
	member.array_count   = u.array_count > 1 ? u.array_count : 1;
	member.array_stride  = u.array_count > 1 ? rbtrend_uniform_array_stride(u.type) : 0;
	member.matrix_stride = rbtrend_uniform_type_columns(u.type) > 1 ? 16 : 0;
	member.size          = (uint32_t)rbtrend_uniform_footprint(u);
	return member;
}

RBT_RESULT rbtrend_uniform_layout_get_uniform_index(const RBTREND_UNIFORM_LAYOUT *layout, const char *name, uint32_t *out_index) {
	if (out_index)
		*out_index = UINT32_MAX;
	if (!layout)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!name)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_index)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (layout->count > 0 && !layout->uniforms)
		return RBT_RESULT_INVALID_ARGUMENT;
	for (uint32_t i = 0; i < layout->count; ++i) {
		const char *uniform_name = layout->uniforms[i].name;
		if (uniform_name && strcmp(uniform_name, name) == 0) {
			*out_index = i;
			return RBT_RESULT_OK;
		}
	}
	return RBT_RESULT_NOT_FOUND;
}

RBT_RESULT rbtrend_uniform_layout_get_size(const RBTREND_UNIFORM_LAYOUT *layout, uint32_t *out_size, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_size)
		*out_size = 0;
	if (!layout)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layout is NULL");
	if (!out_size)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_size is NULL");
	if (layout->count > 0 && !layout->uniforms)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layout's uniforms is NULL with a count of %u", layout->count);
	// The block ends where its furthest uniform does, rounded up to 16 as
	// std140 rounds a block. Offsets are the caller's; nothing is placed here.
	uint64_t size = 0;
	for (uint32_t i = 0; i < layout->count; ++i) {
		const RBTREND_UNIFORM &u = layout->uniforms[i];
		if (rbtrend_uniform_type_get_size(u.type) == 0)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "uniform %u has type %u, which is not an RBTREND_UNIFORM_TYPE", i, (unsigned)u.type);
		const uint64_t uniform_end = (uint64_t)u.offset + rbtrend_uniform_footprint(u);
		if (uniform_end > size)
			size = uniform_end;
	}
	// The block as the shader declares it, where the layout says.
	if (layout->size > size)
		size = layout->size;
	if (size == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the layout has no uniforms and a size of 0");
	if (size > UINT32_MAX - 15)
		return rbtrend_fail(out_error, RBT_RESULT_LIMIT, "the block ends past what 32 bits can address");
	*out_size = ((uint32_t)size + 15) & ~15u;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_buffer_create(rbtrend_instance instance, const RBTREND_BUFFER_DESC *desc, rbtrend_buffer *out_buffer, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_buffer)
		*out_buffer = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_buffer_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_buffer_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	RBT_RESULT ar = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_BUFFER_DESC);
	if (ar != RBT_RESULT_OK)
		return ar;
	if (!out_buffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_buffer is NULL");
	if (desc->size == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "size is 0");
	if (desc->usage == RBTREND_BUFFER_USAGE_NONE)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "usage is RBTREND_BUFFER_USAGE_NONE");
	const RBTREND_BUFFER_USAGE every_usage = RBTREND_BUFFER_USAGE_VERTEX | RBTREND_BUFFER_USAGE_INDEX | RBTREND_BUFFER_USAGE_UNIFORM |
	                                        RBTREND_BUFFER_USAGE_STORAGE | RBTREND_BUFFER_USAGE_INDIRECT_ARGUMENTS;
	if (desc->usage & ~every_usage)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "usage 0x%x has a bit that is not an RBTREND_BUFFER_USAGE", (unsigned)desc->usage);
	// One bit, or storage with indirect arguments: the states a command list
	// knows how to move a buffer between.
	const bool one_bit = (desc->usage & (desc->usage - 1)) == 0;
	if (!one_bit && desc->usage != (RBTREND_BUFFER_USAGE_STORAGE | RBTREND_BUFFER_USAGE_INDIRECT_ARGUMENTS))
		return rbtrend_fail(out_error, RBT_RESULT_UNSUPPORTED, "usage 0x%x combines uses a buffer can't have together (one bit, or STORAGE with INDIRECT_ARGUMENTS)", (unsigned)desc->usage);
	if (desc->writes != RBTREND_BUFFER_WRITES_ONCE && desc->writes != RBTREND_BUFFER_WRITES_MANY)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "writes %u is not RBTREND_BUFFER_WRITES_ONCE or _MANY", (unsigned)desc->writes);
	if (desc->usage & RBTREND_BUFFER_USAGE_INDEX) {
		if (desc->index_stride != RBTREND_INDEX_STRIDE_UINT16 && desc->index_stride != RBTREND_INDEX_STRIDE_UINT32)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "index_stride %u of an index buffer is not UINT16 or UINT32", (unsigned)desc->index_stride);
		if (desc->size % (uint64_t)desc->index_stride != 0)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "size %llu is not a multiple of index_stride %u", (unsigned long long)desc->size, (unsigned)desc->index_stride);
	} else if (desc->index_stride != RBTREND_INDEX_STRIDE_NONE) {
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "index_stride %u of a buffer without RBTREND_BUFFER_USAGE_INDEX is not NONE", (unsigned)desc->index_stride);
	}

	rbtrend_buffer result = nullptr;
	ar                   = rbtrend_buffer_begin(instance, desc->debug_name, true, &result, out_error);
	if (ar != RBT_RESULT_OK)
		return ar;
	const char *name           = result->desc.debug_name;
	result->desc               = *desc;
	result->desc.base          = rbt_struct_base(RBT_STRUCT_TYPE_REND_BUFFER_DESC);
	result->desc.initial_data  = nullptr;
	result->desc.debug_name    = name;
	result->part_count         = desc->per_frame ? instance->desc.frames_in_flight : 1;
	for (uint32_t i = 0; i < result->part_count; ++i) {
		ar = rbtrend_buffer_part_create(result, &result->parts[i], desc->initial_data, out_error);
		if (ar != RBT_RESULT_OK) {
			rbtrend_buffer_abandon(result, true);
			return ar;
		}
	}
	*out_buffer = result;
	return RBT_RESULT_OK;
}

void rbtrend_buffer_destroy(rbtrend_buffer buffer) {
	if (!buffer)
		return;
	if (RBT_STALE(buffer, "rbtrend_buffer_destroy", "buffer"))
		return;
	// A list compiled against any copy and not submitted would copy its
	// writes, or bind it: refused at submit.
	for (uint32_t i = 0; i < buffer->part_count; ++i)
		rbtrend_instance_forget(buffer->instance, &buffer->parts[i].store);
	rbtrend_instance_forget(buffer->instance, buffer);
	// Released once the GPU has finished everything submitted so far, with
	// the retire its create promised.
	RBT_KILL(buffer);
	rbtrend_instance_retire(buffer->instance, buffer, &rbtrend_release_handle<rbtrend_buffer_t>);
}

RBT_RESULT rbtrend_buffer_get_desc(rbtrend_buffer buffer, RBTREND_BUFFER_DESC *out_desc) {
	if (!buffer)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(buffer, "rbtrend_buffer_get_desc", "buffer"))
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return RBT_RESULT_INVALID_ARGUMENT;
	*out_desc = buffer->desc;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_buffer_write(rbtrend_buffer buffer, uint64_t offset, const void *data, uint64_t data_size, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!buffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is NULL");
	if (RBT_STALE(buffer, "rbtrend_buffer_write", "buffer"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is destroyed");
	RBTREND_ENTER(buffer->instance, "rbtrend_buffer_write");
	char label[96];
	if (!data)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "data is NULL");
	if (data_size == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "data_size is 0");
	const uint64_t size = buffer->desc.size;
	if (offset > size)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "offset %llu is past the %s's %llu bytes", (unsigned long long)offset, rbtrend_buffer_label(buffer, label), (unsigned long long)size);
	if (data_size > size - offset)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "%llu bytes at offset %llu run past the %s's %llu", (unsigned long long)data_size, (unsigned long long)offset, rbtrend_buffer_label(buffer, label), (unsigned long long)size);
	if (buffer->desc.writes == RBTREND_BUFFER_WRITES_ONCE)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the %s was created RBTREND_BUFFER_WRITES_ONCE", rbtrend_buffer_label(buffer, label));
	rbtrend_buffer_part *part = rbtrend_buffer_part_of(buffer, buffer->instance->frame_index);
	const CR_RESULT sr      = rbtrend_buffer_store_write(&part->store, offset, data_size, data);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(out_error, sr, "the bytes can't be written to the %s", rbtrend_buffer_label(buffer, label));
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_buffer_write_uniform(rbtrend_buffer buffer, const RBTREND_UNIFORM_LAYOUT *layout, uint32_t uniform_index, const void *data, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!buffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is NULL");
	if (RBT_STALE(buffer, "rbtrend_buffer_write_uniform", "buffer"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "buffer is destroyed");
	RBTREND_ENTER(buffer->instance, "rbtrend_buffer_write_uniform");
	char label[96];
	if (!(buffer->desc.usage & RBTREND_BUFFER_USAGE_UNIFORM))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the %s has no RBTREND_BUFFER_USAGE_UNIFORM", rbtrend_buffer_label(buffer, label));
	if (buffer->desc.writes == RBTREND_BUFFER_WRITES_ONCE)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the %s was created RBTREND_BUFFER_WRITES_ONCE", rbtrend_buffer_label(buffer, label));
	if (!layout)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layout is NULL");
	if (!layout->uniforms)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "layout's uniforms is NULL");
	if (uniform_index >= layout->count)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "uniform_index %u, and the layout has %u uniforms", uniform_index, layout->count);
	if (!data)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "data is NULL");
	const RBTREND_UNIFORM &uniform = layout->uniforms[uniform_index];
	if (rbtrend_uniform_type_get_size(uniform.type) == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "uniform %u has type %u, which is not an RBTREND_UNIFORM_TYPE", uniform_index, (unsigned)uniform.type);
	const RBTREND_SHADER_MEMBER member = rbtrend_uniform_as_member(uniform);
	if ((uint64_t)member.offset + member.size > buffer->desc.size)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "uniform %u ends at byte %llu, past the %s's %llu", uniform_index, (unsigned long long)member.offset + member.size, rbtrend_buffer_label(buffer, label), (unsigned long long)buffer->desc.size);
	// A uniform buffer is mapped for life on every device. The caller's
	// elements and columns are tightly packed, the block's are where std140
	// puts them.
	rbtrend_buffer_part *part = rbtrend_buffer_part_of(buffer, buffer->instance->frame_index);
	rbtrend_write_block_member(member, (uint8_t *)part->store.mapped, data);
	const CR_RESULT sr = crgpu_buffer_flush(part->store.buffer, member.offset, member.size);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(out_error, sr, "the uniform written to the %s can't be flushed", rbtrend_buffer_label(buffer, label));
	return RBT_RESULT_OK;
}
}
