
#include "render/aprendbuffers.h"
#include "aprend_internal.hpp"
#include "aprendreflect_internal.hpp"

#include <spudgpu.h>

#include <new>

aprend_uniform_buffer_t::~aprend_uniform_buffer_t() {
	// One in the device's memory (aprend_uniform_buffer_create_device) is
	// never mapped, and one that failed to create may have no buffer.
	if (this->mapped)
		spudgpu_unmap_buffer(this->buffer);
	if (this->buffer)
		spudgpu_destroy_buffer(this->buffer);
	free(this->layout.uniforms);
}

aprend_uniform_buffer aprend_uniform_buffer_create_device(aprend_instance instance, uint32_t size) {
	APREND_MALLOC__T(result, aprend_uniform_buffer_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_uniform_buffer_t);
	result->instance = instance;
	result->size     = size;

	spudgpu_buffer_desc bd{};
	// Read by shaders, written by the copies a submission records ahead of
	// its list: never by the CPU, so in the device's own memory, unmapped.
	bd.memory_kind = SPUDGPU_MEMORY_KIND_DEVICE;
	bd.size        = size;
	bd.usage       = SPUDGPU_BUFFER_USAGE_UNIFORM | SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
	SPUDRESULT sr  = spudgpu_create_buffer(instance->desc.device, &bd, &result->buffer);
	if (SPUDFAIL(sr)) {
		result->buffer = nullptr;
		printf("apricot: a uniform buffer of %u bytes in the device's memory can't be created: %s\n", size, spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_uniform_buffer_t);
		return nullptr;
	}
	return result;
}

// The CPU writes [buffer] through its mapping: false, printed for [who],
// for one of the device's memory (the material arena's), which has none.
static bool aprend_uniform_buffer_writable(aprend_uniform_buffer buffer, const char *who) {
	if (buffer->mapped)
		return true;
	printf("apricot: %s: the buffer is in the device's memory and is not written by the CPU\n", who);
	return false;
}

// A buffer that failed to create may have no view, and its store may hold
// nothing: each is destroyed only if it exists, the view before its buffer.
aprend_vertex_buffer_t::~aprend_vertex_buffer_t() {
	// Views before the buffer they are of.
	for (const stride_view &known : this->views)
		spudgpu_destroy_buffer_view(known.view);
	aprend_buffer_store_destroy(&this->store);
}

SPUDRESULT aprend_vertex_buffer_view(
    aprend_vertex_buffer buffer,
    uint32_t stride,
    spudgpu_buffer_view *out_view) {
	for (const aprend_vertex_buffer_t::stride_view &known : buffer->views) {
		if (known.stride != stride)
			continue;
		*out_view = known.view;
		return SPUD_SUCCESS;
	}
	// Room first: with none, no view is made.
	try {
		buffer->views.reserve(buffer->views.size() + 1);
	} catch (const std::bad_alloc &) {
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	spudgpu_buffer_view_desc vd{};
	vd.parent_buffer             = buffer->store.buffer;
	vd.offset_from_parent_buffer = 0;
	vd.stride                    = stride;
	vd.size                      = buffer->size;
	spudgpu_buffer_view view     = nullptr;
	SPUDRESULT sr                = spudgpu_create_buffer_view(buffer->store.buffer, &vd, &view);
	if (SPUDFAIL(sr))
		return sr;
	buffer->views.push_back({stride, view});
	*out_view = view;
	return SPUD_SUCCESS;
}

aprend_index_buffer_t::~aprend_index_buffer_t() {
	if (this->buffer_view)
		spudgpu_destroy_buffer_view(this->buffer_view);
	aprend_buffer_store_destroy(&this->store);
}

aprend_storage_buffer_t::~aprend_storage_buffer_t() {
	if (this->buffer_view)
		spudgpu_destroy_buffer_view(this->buffer_view);
	aprend_buffer_store_destroy(&this->store);
}

// A set that failed part-way holds only the buffers it got as far as making.
aprend_uniform_buffer_set_t::~aprend_uniform_buffer_set_t() {
	for (uint32_t i = 0; i < this->buffer_count; ++i)
		aprend_uniform_buffer_destroy(this->buffers[i]);
}

aprend_storage_buffer_set_t::~aprend_storage_buffer_set_t() {
	for (uint32_t i = 0; i < this->buffer_count; ++i)
		aprend_storage_buffer_destroy(this->buffers[i]);
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

	// A device whose memory is the system's is written directly. One with
	// its own gets the buffer there, written through staging.
	//
	// Written directly, a buffer the GPU only reads is UPLOAD memory. One a
	// shader may write can't be: it needs DEVICE_MAPPABLE, which a device
	// reports having and which is never a device with memory of its own.
	const bool shader_writable = (usage & SPUDGPU_BUFFER_USAGE_STORAGE) != 0;
	const bool staged          = shader_writable ? !instance->device_mappable_memory : !instance->unified_memory;

	spudgpu_buffer_desc bd{};
	bd.size         = size;
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
	store->staged = staged;
	// A ONCE store stages each write in a buffer of its own, made by the
	// write; only a MANY store keeps one for life.
	if (!staged || store->written_once)
		return SPUD_SUCCESS;

	spudgpu_buffer_desc sd{};
	sd.memory_kind  = SPUDGPU_MEMORY_KIND_UPLOAD;
	sd.size         = size;
	sd.usage        = SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	sr              = spudgpu_create_buffer(instance->desc.device, &sd, &store->staging);
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
	// buffer, never submitted or long since run, goes at once.
	for (const aprend_buffer_upload &upload : store->uploads)
		if (upload.source != store->staging)
			spudgpu_destroy_buffer(upload.source);
	store->uploads.clear();
	if (store->staging)
		spudgpu_destroy_buffer(store->staging);
	if (store->buffer)
		spudgpu_destroy_buffer(store->buffer);
	store->staging = nullptr;
	store->buffer  = nullptr;
	store->staged  = false;
}

// Copies [size] bytes of [data] to [offset] of [target], through a mapping,
// and flushes them.
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
// copied and then retired.
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
	spudgpu_buffer_desc sd{};
	sd.memory_kind        = SPUDGPU_MEMORY_KIND_UPLOAD;
	sd.size               = size;
	sd.usage              = SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	spudgpu_buffer source = nullptr;
	SPUDRESULT sr         = spudgpu_create_buffer(store->instance->desc.device, &sd, &source);
	if (SPUDFAIL(sr))
		return sr;
	sr = aprend_buffer_write_mapped(source, 0, size, data);
	if (SPUDFAIL(sr)) {
		spudgpu_destroy_buffer(source);
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
	uint64_t begin = offset;
	uint64_t end   = offset + size;
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
	if (!store->staged)
		return aprend_buffer_write_mapped(store->buffer, offset, size, data);
	if (store->staging)
		return aprend_buffer_store_write_staging(store, offset, size, data);
	return aprend_buffer_store_write_own(store, offset, size, data);
}

void aprend_buffer_store_uploads_submitted(aprend_buffer_store *store) {
	for (const aprend_buffer_upload &upload : store->uploads)
		if (upload.source != store->staging)
			aprend_instance_retire(store->instance, upload.source, &aprend_release_spudgpu_buffer);
	store->uploads.clear();
}

// [writes] is one of the two APREND_BUFFER_WRITES; [who] names the call.
static bool aprend_buffer_writes_valid(APREND_BUFFER_WRITES writes, const char *who) {
	if (writes == APREND_BUFFER_WRITES_ONCE || writes == APREND_BUFFER_WRITES_MANY)
		return true;
	printf("apricot: %s: writes %u is not APREND_BUFFER_WRITES_ONCE or _MANY\n", who, (unsigned)writes);
	return false;
}

// The update calls' refusal of a buffer written once; [who] names the call.
static bool aprend_buffer_store_updatable(const aprend_buffer_store &store, const char *who) {
	if (!store.written_once)
		return true;
	printf("apricot: %s: the buffer was created APREND_BUFFER_WRITES_ONCE\n", who);
	return false;
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
static uint64_t aprend_uniform_footprint(const aprend_uniform &u) {
	if (u.array_count <= 1)
		return aprend_uniform_std140_size(u.type);
	return (uint64_t)aprend_uniform_array_stride(u.type) * u.array_count;
}

// [u] as a block member laid out by std140, for aprend_write_block_member.
static aprend_shader_member aprend_uniform_as_member(const aprend_uniform &u) {
	aprend_shader_member member{};
	member.type          = u.type;
	member.offset        = u.offset;
	member.array_count   = u.array_count > 1 ? u.array_count : 1;
	member.array_stride  = u.array_count > 1 ? aprend_uniform_array_stride(u.type) : 0;
	member.matrix_stride = aprend_uniform_type_columns(u.type) > 1 ? 16 : 0;
	member.size          = (uint32_t)aprend_uniform_footprint(u);
	return member;
}

aprend_uniform_buffer aprend_uniform_buffer_create(aprend_instance instance, const aprend_uniform_layout *layout) {
	static const char who[] = "aprend_uniform_buffer_create";
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return nullptr;
	}
	if (!layout) {
		printf("apricot: %s: layout is NULL\n", who);
		return nullptr;
	}
	if (layout->count > 0 && !layout->uniforms) {
		printf("apricot: %s: layout's uniforms is NULL with a count of %u\n", who, layout->count);
		return nullptr;
	}
	// The block ends where its furthest uniform does, rounded up to 16 as
	// std140 rounds a block. Offsets are the caller's; nothing is placed here.
	uint64_t size = 0;
	for (uint32_t i = 0; i < layout->count; ++i) {
		const aprend_uniform &u = layout->uniforms[i];
		if (aprend_uniform_type_get_size(u.type) == 0) {
			printf("apricot: %s: uniform %u has type %u, which is not an APREND_UNIFORM_TYPE\n", who, i, (unsigned)u.type);
			return nullptr;
		}
		const uint64_t uniform_end = (uint64_t)u.offset + aprend_uniform_footprint(u);
		if (uniform_end > size)
			size = uniform_end;
	}
	// The block as the shader declares it, where the layout says.
	if (layout->size > size)
		size = layout->size;
	if (size == 0) {
		printf("apricot: %s: the layout has no uniforms and a size of 0\n", who);
		return nullptr;
	}
	if (size > UINT32_MAX - 15) {
		printf("apricot: %s: the block ends past what 32 bits can address\n", who);
		return nullptr;
	}

	APREND_MALLOC__T(result, aprend_uniform_buffer_t);
	if (!result) {
		printf("apricot: %s: out of memory\n", who);
		return nullptr;
	}
	APREND_CONSTRUCT__T(result, aprend_uniform_buffer_t);
	result->instance = instance;
	result->size     = ((uint32_t)size + 15) & ~15u;

	// One block: the uniforms, then a copy of each name, so a name is never
	// read from a string the caller has since released. The destructor frees
	// it in one go. A layout of no uniforms has none to copy.
	result->layout.size = layout->size;
	if (layout->count > 0) {
		size_t name_bytes = 0;
		for (uint32_t i = 0; i < layout->count; ++i)
			if (layout->uniforms[i].name)
				name_bytes += strlen(layout->uniforms[i].name) + 1;
		const size_t uniform_bytes = sizeof(aprend_uniform) * layout->count;
		result->layout.uniforms    = (aprend_uniform *)malloc(uniform_bytes + name_bytes);
		if (!result->layout.uniforms) {
			printf("apricot: %s: out of memory\n", who);
			APREND_DESTRUCT__T(result, aprend_uniform_buffer_t);
			return nullptr;
		}
		result->layout.count = layout->count;
		memcpy(result->layout.uniforms, layout->uniforms, uniform_bytes);
		char *names = (char *)result->layout.uniforms + uniform_bytes;
		for (uint32_t i = 0; i < layout->count; ++i) {
			const char *name = layout->uniforms[i].name;
			if (!name)
				continue;
			const size_t length = strlen(name) + 1;
			memcpy(names, name, length);
			result->layout.uniforms[i].name = names;
			names += length;
		}
	}

	spudgpu_buffer_desc bd{};
	// Written by the CPU and only read by the GPU, on every device.
	bd.memory_kind = SPUDGPU_MEMORY_KIND_UPLOAD;
	bd.size        = result->size;
	bd.usage       = SPUDGPU_BUFFER_USAGE_UNIFORM;
	SPUDRESULT sr  = spudgpu_create_buffer(instance->desc.device, &bd, &result->buffer);
	if (SPUDFAIL(sr)) {
		result->buffer = nullptr;
		printf("apricot: %s: a buffer of %u bytes can't be created: %s\n", who, result->size, spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_uniform_buffer_t);
		return nullptr;
	}
	sr = spudgpu_map_buffer(result->buffer, 0, result->size, &result->mapped);
	if (SPUDFAIL(sr)) {
		result->mapped = nullptr;
		printf("apricot: %s: the buffer can't be mapped: %s\n", who, spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_uniform_buffer_t);
		return nullptr;
	}
	// A new SpudGPU buffer's contents are undefined, and a block is usually
	// written one uniform at a time: zero it, so a uniform the caller has not
	// written yet reads 0 on every device.
	memset(result->mapped, 0, result->size);
	sr = spudgpu_flush_buffer(result->buffer, 0, result->size);
	if (SPUDFAIL(sr)) {
		printf("apricot: %s: the zeroed block can't be flushed: %s\n", who, spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_uniform_buffer_t);
		return nullptr;
	}
	return result;
}
void aprend_uniform_buffer_destroy(aprend_uniform_buffer buffer) {
	if (!buffer)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(buffer->instance, buffer, &aprend_release_handle<aprend_uniform_buffer_t>);
}
void aprend_uniform_buffer_get_layout(aprend_uniform_buffer buffer, aprend_uniform_layout *out_layout) {
	if (!buffer)
		return;
	if (!out_layout)
		return;
	*out_layout = buffer->layout;
}
uint32_t aprend_uniform_buffer_get_size(aprend_uniform_buffer buffer) { return buffer ? buffer->size : 0; }
bool aprend_uniform_buffer_update(aprend_uniform_buffer buffer, uint32_t local_offset, uint32_t size, const void *pData) {
	static const char who[] = "aprend_uniform_buffer_update";
	if (!buffer) {
		printf("apricot: %s: buffer is NULL\n", who);
		return false;
	}
	if (size == 0) {
		printf("apricot: %s: size is 0\n", who);
		return false;
	}
	if (!pData) {
		printf("apricot: %s: pData is NULL\n", who);
		return false;
	}
	if ((uint64_t)local_offset + size > buffer->size) {
		printf("apricot: %s: %u bytes at offset %u run past the buffer's %u\n", who, size, local_offset, buffer->size);
		return false;
	}
	if (!aprend_uniform_buffer_writable(buffer, who))
		return false;
	memcpy((uint8_t *)buffer->mapped + local_offset, pData, size);
	// The buffer stays mapped; each write is flushed all the same.
	return !SPUDFAIL(spudgpu_flush_buffer(buffer->buffer, local_offset, size));
}
uint32_t aprend_uniform_layout_get_uniform_index(const aprend_uniform_layout *layout, const char *name) {
	if (!layout)
		return UINT32_MAX;
	if (!layout->uniforms)
		return UINT32_MAX;
	if (!name)
		return UINT32_MAX;
	for (uint32_t i = 0; i < layout->count; ++i) {
		const char *uniform_name = layout->uniforms[i].name;
		if (uniform_name && strcmp(uniform_name, name) == 0)
			return i;
	}
	return UINT32_MAX;
}
bool aprend_uniform_buffer_update_uniform(aprend_uniform_buffer buffer, uint32_t uniform_index, const void *pData) {
	static const char who[] = "aprend_uniform_buffer_update_uniform";
	if (!buffer) {
		printf("apricot: %s: buffer is NULL\n", who);
		return false;
	}
	if (!pData) {
		printf("apricot: %s: pData is NULL\n", who);
		return false;
	}
	if (uniform_index >= buffer->layout.count) {
		printf("apricot: %s: uniform_index %u, and the buffer's layout has %u uniforms\n", who, uniform_index, buffer->layout.count);
		return false;
	}
	if (!aprend_uniform_buffer_writable(buffer, who))
		return false;
	// Every uniform's type was checked and the block sized to hold it at
	// creation, so it fits. The caller's elements and columns are tightly
	// packed, the block's are where std140 puts them.
	const aprend_shader_member member = aprend_uniform_as_member(buffer->layout.uniforms[uniform_index]);
	aprend_write_block_member(member, (uint8_t *)buffer->mapped, pData);
	return !SPUDFAIL(spudgpu_flush_buffer(buffer->buffer, member.offset, member.size));
}
bool aprend_uniform_buffer_update_by_name(aprend_uniform_buffer buffer, const char *name, const void *pData) {
	static const char who[] = "aprend_uniform_buffer_update_by_name";
	if (!buffer) {
		printf("apricot: %s: buffer is NULL\n", who);
		return false;
	}
	if (!name) {
		printf("apricot: %s: name is NULL\n", who);
		return false;
	}
	if (!pData) {
		printf("apricot: %s: pData is NULL\n", who);
		return false;
	}
	const uint32_t uniform_index = aprend_uniform_layout_get_uniform_index(&buffer->layout, name);
	if (uniform_index == UINT32_MAX) {
		printf("apricot: %s: the buffer's layout has no uniform named \"%s\"\n", who, name);
		return false;
	}
	return aprend_uniform_buffer_update_uniform(buffer, uniform_index, pData);
}

aprend_vertex_buffer aprend_vertex_buffer_create(aprend_instance instance, uint64_t size, APREND_BUFFER_WRITES writes, const void *pData) {
	static const char who[] = "aprend_vertex_buffer_create";
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return nullptr;
	}
	if (size == 0) {
		printf("apricot: %s: size is 0\n", who);
		return nullptr;
	}
	if (!aprend_buffer_writes_valid(writes, who))
		return nullptr;

	APREND_MALLOC__T(result, aprend_vertex_buffer_t);
	if (!result) {
		printf("apricot: %s: out of memory\n", who);
		return nullptr;
	}
	APREND_CONSTRUCT__T(result, aprend_vertex_buffer_t);
	result->instance = instance;
	result->size     = size;

	SPUDRESULT sr = aprend_buffer_store_create(&result->store, instance, size, SPUDGPU_BUFFER_USAGE_VERTEX, SPUDGPU_RESOURCE_STATE_VERTEX_BUFFER, writes);
	if (SPUDFAIL(sr)) {
		printf("apricot: %s: a buffer of %llu bytes can't be created: %s\n", who, (unsigned long long)size, spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_vertex_buffer_t);
		return nullptr;
	}
	if (pData) {
		sr = aprend_buffer_store_write(&result->store, 0, size, pData);
		if (SPUDFAIL(sr)) {
			printf("apricot: %s: the bytes can't be written: %s\n", who, spudresult_str(sr));
			APREND_DESTRUCT__T(result, aprend_vertex_buffer_t);
			return nullptr;
		}
	}
	return result;
}

void aprend_vertex_buffer_destroy(aprend_vertex_buffer buffer) {
	if (!buffer)
		return;
	// A list compiled against it and not submitted would copy its writes.
	aprend_instance_forget(buffer->instance, &buffer->store);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(buffer->instance, buffer, &aprend_release_handle<aprend_vertex_buffer_t>);
}
uint64_t aprend_vertex_buffer_get_size(aprend_vertex_buffer buffer) { return buffer ? buffer->size : 0; }
bool aprend_vertex_buffer_update(aprend_vertex_buffer buffer, uint64_t offset, uint64_t size, const void *pData) {
	static const char who[] = "aprend_vertex_buffer_update";
	if (!buffer) {
		printf("apricot: %s: buffer is NULL\n", who);
		return false;
	}
	if (size == 0) {
		printf("apricot: %s: size is 0\n", who);
		return false;
	}
	if (!pData) {
		printf("apricot: %s: pData is NULL\n", who);
		return false;
	}
	if (offset > buffer->size) {
		printf("apricot: %s: offset %llu is past the buffer's %llu bytes\n", who, (unsigned long long)offset, (unsigned long long)buffer->size);
		return false;
	}
	if (size > buffer->size - offset) {
		printf("apricot: %s: %llu bytes at offset %llu run past the buffer's %llu\n", who, (unsigned long long)size, (unsigned long long)offset,
		       (unsigned long long)buffer->size);
		return false;
	}
	if (!aprend_buffer_store_updatable(buffer->store, who))
		return false;
	const SPUDRESULT sr = aprend_buffer_store_write(&buffer->store, offset, size, pData);
	if (SPUDFAIL(sr)) {
		printf("apricot: %s: the bytes can't be written: %s\n", who, spudresult_str(sr));
		return false;
	}
	return true;
}

aprend_index_buffer aprend_index_buffer_create(aprend_instance instance, APREND_INDEX_STRIDE stride, uint32_t index_count, APREND_BUFFER_WRITES writes, const void *pData) {
	if (!instance)
		return nullptr;
	if (stride != APREND_INDEX_STRIDE_UINT16 && stride != APREND_INDEX_STRIDE_UINT32)
		return nullptr;
	if (index_count == 0)
		return nullptr;
	if (!aprend_buffer_writes_valid(writes, "aprend_index_buffer_create"))
		return nullptr;

	aprend_index_buffer_t *result = (aprend_index_buffer_t *)malloc(sizeof(aprend_index_buffer_t));
	if (result)
		result = new (result) aprend_index_buffer_t();
	else
		return nullptr;
	result->instance = instance;

	result->index_count  = index_count;
	result->index_stride = stride;

	const uint64_t byte_size = (uint64_t)stride * index_count;

	SPUDRESULT sr = aprend_buffer_store_create(&result->store, instance, byte_size, SPUDGPU_BUFFER_USAGE_INDEX, SPUDGPU_RESOURCE_STATE_INDEX_BUFFER, writes);
	if (SPUDFAIL(sr))
		goto failedattempt;

	result->buffer_view_desc.parent_buffer             = result->store.buffer;
	result->buffer_view_desc.offset_from_parent_buffer = 0;
	result->buffer_view_desc.stride                    = (uint64_t)result->index_stride;
	result->buffer_view_desc.size                      = byte_size;
	sr = spudgpu_create_buffer_view(result->store.buffer, &result->buffer_view_desc, &result->buffer_view);
	if (SPUDFAIL(sr)) {
		result->buffer_view = nullptr;
		goto failedattempt;
	}

	if (pData) {
		sr = aprend_buffer_store_write(&result->store, 0, byte_size, pData);
		if (SPUDFAIL(sr))
			goto failedattempt;
	}

	return result;

failedattempt:
	printf("apricot: aprend_index_buffer_create failed: %s\n", spudresult_str(sr));
	result->~aprend_index_buffer_t();
	free(result);
	return nullptr;
}
void aprend_index_buffer_destroy(aprend_index_buffer buffer) {
	if (!buffer)
		return;
	// As aprend_vertex_buffer_destroy.
	aprend_instance_forget(buffer->instance, &buffer->store);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(buffer->instance, buffer, &aprend_release_handle<aprend_index_buffer_t>);
}
spudgpu_buffer_view aprend_index_buffer_get_spudgpu_buffer_view(aprend_index_buffer buffer) { return buffer ? buffer->buffer_view : nullptr; }
bool aprend_index_buffer_update(aprend_index_buffer buffer, uint32_t index_offset, uint32_t index_count, const void *pData) {
	if (!buffer)
		return false;
	if (index_count == 0)
		return false;
	if (!pData)
		return false;
	if (index_offset > buffer->index_count)
		return false;
	if (index_count > buffer->index_count - index_offset)
		return false;
	if (!aprend_buffer_store_updatable(buffer->store, "aprend_index_buffer_update"))
		return false;
	const uint64_t index_size  = (uint64_t)buffer->index_stride;
	const uint64_t byte_offset = index_offset * index_size;
	const uint64_t byte_size   = index_count * index_size;
	return !SPUDFAIL(aprend_buffer_store_write(&buffer->store, byte_offset, byte_size, pData));
}
APREND_INDEX_STRIDE aprend_index_buffer_get_stride(aprend_index_buffer buffer) { return buffer ? buffer->index_stride : APREND_INDEX_STRIDE_NONE; }
uint32_t aprend_index_buffer_get_index_count(aprend_index_buffer buffer) { return buffer ? buffer->index_count : 0; }

aprend_storage_buffer aprend_storage_buffer_create(aprend_instance instance, uint64_t size, APREND_STORAGE_BUFFER_USAGE usage, APREND_BUFFER_WRITES writes, const void *pData) {
	if (!instance)
		return nullptr;
	if (size == 0)
		return nullptr;
	if (usage & ~(APREND_STORAGE_BUFFER_USAGE)APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS) {
		printf("apricot: aprend_storage_buffer_create: usage %u has a bit that is not an APREND_STORAGE_BUFFER_USAGE\n", (unsigned)usage);
		return nullptr;
	}
	if (!aprend_buffer_writes_valid(writes, "aprend_storage_buffer_create"))
		return nullptr;
	aprend_storage_buffer_t *result = (aprend_storage_buffer_t *)malloc(sizeof(aprend_storage_buffer_t));
	if (result)
		result = new (result) aprend_storage_buffer_t();
	else
		return nullptr;
	result->instance = instance;

	result->size  = size;
	result->usage = usage;

	// UNORDERED_ACCESS: a storage buffer slot may be written by the shader as
	// well as read. One that is also an indirect argument buffer is in that
	// state only while a storage slot uses it, and a command list moves it.
	SPUDGPU_BUFFER_USAGE spud_usage = SPUDGPU_BUFFER_USAGE_STORAGE;
	if (usage & APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS)
		spud_usage |= SPUDGPU_BUFFER_USAGE_INDIRECT;
	SPUDRESULT sr = aprend_buffer_store_create(&result->store, instance, size, spud_usage, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, writes);
	result->store.multi_state = (usage & APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS) != 0;
	if (SPUDFAIL(sr))
		goto failedattempt;

	result->buffer_view_desc.parent_buffer             = result->store.buffer;
	result->buffer_view_desc.offset_from_parent_buffer = 0;
	result->buffer_view_desc.stride                    = 0;
	result->buffer_view_desc.size                      = size;
	sr = spudgpu_create_buffer_view(result->store.buffer, &result->buffer_view_desc, &result->buffer_view);
	if (SPUDFAIL(sr)) {
		result->buffer_view = nullptr;
		goto failedattempt;
	}

	if (pData) {
		sr = aprend_buffer_store_write(&result->store, 0, size, pData);
		if (SPUDFAIL(sr))
			goto failedattempt;
	}

	return result;
failedattempt:
	printf("apricot: aprend_storage_buffer_create failed: %s\n", spudresult_str(sr));
	result->~aprend_storage_buffer_t();
	free(result);
	return nullptr;
}
APREND_STORAGE_BUFFER_USAGE aprend_storage_buffer_get_usage(aprend_storage_buffer buffer) {
	return buffer ? buffer->usage : (APREND_STORAGE_BUFFER_USAGE)APREND_STORAGE_BUFFER_USAGE_NONE;
}
uint64_t aprend_storage_buffer_get_size(aprend_storage_buffer buffer) { return buffer ? buffer->size : 0; }
void aprend_storage_buffer_destroy(aprend_storage_buffer buffer) {
	if (!buffer)
		return;
	// As aprend_vertex_buffer_destroy.
	aprend_instance_forget(buffer->instance, &buffer->store);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(buffer->instance, buffer, &aprend_release_handle<aprend_storage_buffer_t>);
}
bool aprend_storage_buffer_update(aprend_storage_buffer buffer, uint64_t local_offset, uint64_t size, const void *pData) {
	if (!buffer)
		return false;
	if (size == 0)
		return false;
	if (!pData)
		return false;
	if (local_offset > buffer->size)
		return false;
	if (size > buffer->size - local_offset)
		return false;
	if (!aprend_buffer_store_updatable(buffer->store, "aprend_storage_buffer_update"))
		return false;
	return !SPUDFAIL(aprend_buffer_store_write(&buffer->store, local_offset, size, pData));
}

aprend_uniform_buffer_set aprend_uniform_buffer_set_create(aprend_instance instance, const aprend_uniform_layout *layout) {
	static const char who[] = "aprend_uniform_buffer_set_create";
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return nullptr;
	}
	if (!layout) {
		printf("apricot: %s: layout is NULL\n", who);
		return nullptr;
	}
	aprend_uniform_buffer_set_t *result = (aprend_uniform_buffer_set_t *)malloc(sizeof(aprend_uniform_buffer_set_t));
	if (!result) {
		printf("apricot: %s: out of memory\n", who);
		return nullptr;
	}
	result           = new (result) aprend_uniform_buffer_set_t();
	result->instance = instance;

	for (uint32_t i = 0; i < instance->desc.frames_in_flight; ++i) {
		result->buffers[i] = aprend_uniform_buffer_create(instance, layout);
		if (!result->buffers[i]) {
			result->~aprend_uniform_buffer_set_t();
			free(result);
			return nullptr;
		}
		++result->buffer_count;
	}
	return result;
}
void aprend_uniform_buffer_set_destroy(aprend_uniform_buffer_set set) {
	if (set) {
		set->~aprend_uniform_buffer_set_t();
		free(set);
	}
}
aprend_uniform_buffer aprend_uniform_buffer_set_get_buffer(aprend_uniform_buffer_set set, uint32_t frame_index) {
	if (!set)
		return nullptr;
	if (frame_index >= set->buffer_count)
		return nullptr;
	return set->buffers[frame_index];
}
bool aprend_uniform_buffer_set_update(aprend_uniform_buffer_set set, uint32_t local_offset, uint32_t size, const void *pData) {
	if (!set) {
		printf("apricot: aprend_uniform_buffer_set_update: set is NULL\n");
		return false;
	}
	return aprend_uniform_buffer_update(set->buffers[set->instance->frame_index], local_offset, size, pData);
}
bool aprend_uniform_buffer_set_update_by_name(aprend_uniform_buffer_set set, const char *name, const void *pData) {
	if (!set) {
		printf("apricot: aprend_uniform_buffer_set_update_by_name: set is NULL\n");
		return false;
	}
	return aprend_uniform_buffer_update_by_name(set->buffers[set->instance->frame_index], name, pData);
}
bool aprend_uniform_buffer_set_update_uniform(aprend_uniform_buffer_set set, uint32_t uniform_index, const void *pData) {
	if (!set) {
		printf("apricot: aprend_uniform_buffer_set_update_uniform: set is NULL\n");
		return false;
	}
	return aprend_uniform_buffer_update_uniform(set->buffers[set->instance->frame_index], uniform_index, pData);
}

aprend_storage_buffer_set aprend_storage_buffer_set_create(aprend_instance instance, uint64_t size, APREND_STORAGE_BUFFER_USAGE usage, APREND_BUFFER_WRITES writes, const void *pData) {
	if (!instance)
		return nullptr;
	if (size == 0)
		return nullptr;
	aprend_storage_buffer_set_t *result = (aprend_storage_buffer_set_t *)malloc(sizeof(aprend_storage_buffer_set_t));
	if (!result)
		return nullptr;
	result           = new (result) aprend_storage_buffer_set_t();
	result->instance = instance;

	for (uint32_t i = 0; i < instance->desc.frames_in_flight; ++i) {
		result->buffers[i] = aprend_storage_buffer_create(instance, size, usage, writes, pData);
		if (!result->buffers[i]) {
			result->~aprend_storage_buffer_set_t();
			free(result);
			return nullptr;
		}
		++result->buffer_count;
	}
	return result;
}
void aprend_storage_buffer_set_destroy(aprend_storage_buffer_set set) {
	if (set) {
		set->~aprend_storage_buffer_set_t();
		free(set);
	}
}
aprend_storage_buffer aprend_storage_buffer_set_get_buffer(aprend_storage_buffer_set set, uint32_t frame_index) {
	if (!set)
		return nullptr;
	if (frame_index >= set->buffer_count)
		return nullptr;
	return set->buffers[frame_index];
}
bool aprend_storage_buffer_set_update(aprend_storage_buffer_set set, uint64_t local_offset, uint64_t size, const void *pData) {
	if (!set)
		return false;
	return aprend_storage_buffer_update(set->buffers[set->instance->frame_index], local_offset, size, pData);
}
}
