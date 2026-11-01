#include "render/aprendraytracing.h"
#include "aprendraytracing_internal.hpp"
#include "aprendlit_internal.hpp"
#include "aprendimages_internal.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

/* Aprend's ray tracing (aprendraytracing.h; aprend_raytracing_draft.h, the
 * approved eleventh version, with cards A40-A42): the instance's BLAS and
 * lit-material tables, the geometry arena, BLASes and TLASes and their
 * instance sets. The four commands' compile functions are in
 * aprendcommands.cpp, beside the barriers they record, and call the
 * functions at the end of this file. */

/* ---- The instance's state ---------------------------------------------- */

// A device buffer of [size] bytes for [rt], named [name], with [extra]
// usage. NULL, [error] saying why, if it can't be made.
static aprend_buffer aprend_rt_make_buffer(aprend_instance instance, uint64_t size, SPUDGPU_BUFFER_USAGE extra, const char *name, APREND_ERROR *error) {
	aprend_buffer buffer = nullptr;
	if (aprend_buffer_create_device_storage(instance, size, extra, name, &buffer, error) != APRESULT_OK)
		return nullptr;
	return buffer;
}

// Replaces [*buffer] with one of [size] bytes, what it held copied across
// ahead of everything submitted after this (the upload batch), and the old
// one released once that has run. With nothing submitted on the instance
// yet, nothing in the old one was ever written by the GPU, and nothing is
// copied. APRESULT_OK, or the failure with [*buffer] as it was.
static APRESULT aprend_rt_grow_buffer(aprend_instance instance, aprend_buffer *buffer, uint64_t size, const char *name, APREND_ERROR *error) {
	aprend_buffer grown = aprend_rt_make_buffer(instance, size, 0, name, error);
	if (!grown)
		return error && error->message[0] ? APRESULT_OUT_OF_MEMORY : APRESULT_OUT_OF_MEMORY;
	aprend_buffer old = *buffer;
	if (old && instance->submit_queue) {
		spudgpu_command_list cmd = nullptr;
		if (const APRESULT opened = aprend_instance_open_upload_batch(instance, instance->submit_queue, &cmd, error)) {
			aprend_buffer_destroy(grown);
			return opened;
		}
		spudgpu_buffer from = old->parts[0].store.buffer;
		spudgpu_buffer to   = grown->parts[0].store.buffer;
		spudgpu_buffer_barrier before[2] = {
		    {{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, from, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_SOURCE},
		    {{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, to, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST},
		};
		spudgpu_cmd_pipeline_barrier(cmd, before, 2, nullptr, 0);
		spudgpu_cmd_copy_buffer(cmd, from, to, 0, 0, old->desc.size);
		spudgpu_buffer_barrier after[2] = {
		    {{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, from, SPUDGPU_RESOURCE_STATE_COPY_SOURCE, SPUDGPU_RESOURCE_STATE_COMMON},
		    {{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, to, SPUDGPU_RESOURCE_STATE_COPY_DEST, SPUDGPU_RESOURCE_STATE_COMMON},
		};
		spudgpu_cmd_pipeline_barrier(cmd, after, 2, nullptr, 0);
	}
	// A list compiled against the old one and not yet submitted is refused at
	// submit, and compiled again against this one.
	aprend_buffer_destroy(old);
	*buffer = grown;
	++instance->rt->buffers_version;
	return APRESULT_OK;
}

aprend_rt_state *aprend_rt_state_get(aprend_instance instance, APREND_ERROR *error) {
	if (instance->rt)
		return instance->rt;
	aprend_rt_state *rt = (aprend_rt_state *)malloc(sizeof(aprend_rt_state));
	if (!rt) {
		aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the instance's ray tracing tables");
		return nullptr;
	}
	rt = new (rt) aprend_rt_state();
	// The limits from the device's adapter, chained onto its properties.
	SPUDGPU_ADAPTER_PROPERTIES properties{};
	properties.base.type = SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
	rt->limits.base.type = SPUD_STRUCT_TYPE_GPU_RAY_QUERY_LIMITS;
	properties.base.next = (SPUD_STRUCT_BASE_OUT *)&rt->limits;
	SPUDRESULT sr        = spudgpu_get_adapter_properties(spudgpu_get_device_adapter(instance->desc.device), &properties);
	if (SPUD_SUCCESS == sr)
		sr = spudgpu_get_acceleration_structure_instance_layout(instance->desc.device, &rt->instance_layout);
	if (SPUDFAIL(sr)) {
		rt->~aprend_rt_state();
		free(rt);
		aprend_fail_spud(error, sr, "reading the device's ray query limits and instance layout failed");
		return nullptr;
	}
	instance->rt = rt;
	// Index 0 of each table is never handed out: 0 is never a slot.
	bool ok = rt->blas_generation.push(0) == EQ_SUCCESS && rt->blas_live.push(0) == EQ_SUCCESS && rt->blas_built.push(0) == EQ_SUCCESS &&
	          rt->blas_generation_debug.push(0) == EQ_SUCCESS && rt->blas_structure.push(nullptr) == EQ_SUCCESS &&
	          rt->blas_geometry_count.push(0) == EQ_SUCCESS && rt->lit_generation.push(0) == EQ_SUCCESS && rt->lit_live.push(0) == EQ_SUCCESS &&
	          rt->lit_entries.push(aprend_rt_lit_entry{}) == EQ_SUCCESS && rt->lit_dirty.push(0) == EQ_SUCCESS &&
	          rt->blas_free.reserve(APREND_RT_FIRST_TABLE_ENTRIES) == EQ_SUCCESS && rt->lit_free.reserve(APREND_RT_FIRST_TABLE_ENTRIES) == EQ_SUCCESS &&
	          rt->lit_dirty_list.reserve(APREND_RT_FIRST_TABLE_ENTRIES) == EQ_SUCCESS;
	if (ok) {
		try {
			rt->arena_ranges.reset(APREND_RT_FIRST_ARENA_UNITS);
		} catch (const std::bad_alloc &) {
			ok = false;
		}
	}
	if (ok) {
		rt->blas_table = aprend_rt_make_buffer(instance, (uint64_t)APREND_RT_FIRST_TABLE_ENTRIES * sizeof(aprend_rt_blas_entry), 0, "aprend BLAS table", error);
		rt->lit_table  = aprend_rt_make_buffer(instance, (uint64_t)APREND_RT_FIRST_TABLE_ENTRIES * sizeof(aprend_rt_lit_entry), 0, "aprend lit-material table", error);
		rt->arena      = aprend_rt_make_buffer(instance, (uint64_t)APREND_RT_FIRST_ARENA_UNITS * APREND_RT_ARENA_UNIT, 0, "aprend geometry arena", error);
		ok             = rt->blas_table && rt->lit_table && rt->arena;
		if (!ok) {
			aprend_rt_state_destroy(instance);
			aprend_rt_state_free(instance);
			return nullptr;
		}
	}
	if (!ok) {
		aprend_rt_state_destroy(instance);
		aprend_rt_state_free(instance);
		aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the instance's ray tracing tables");
		return nullptr;
	}
	return rt;
}

void aprend_rt_state_destroy(aprend_instance instance) {
	aprend_rt_state *rt = instance->rt;
	if (!rt)
		return;
	// Pipelines before layouts, as the builtins go.
	aprend_compute_pipeline_destroy(rt->writer_pipeline);
	aprend_compute_pipeline_destroy(rt->emitter_pipeline);
	aprend_compute_pipeline_destroy(rt->mask_pipelines[0]);
	aprend_compute_pipeline_destroy(rt->mask_pipelines[1]);
	for (aprend_compute_pipeline &pipeline : rt->path_pipelines) {
		aprend_compute_pipeline_destroy(pipeline);
		pipeline = nullptr;
	}
	aprend_binding_layout_destroy(rt->writer_layout);
	aprend_binding_layout_destroy(rt->emitter_layout);
	aprend_binding_layout_destroy(rt->scene_layout);
	aprend_binding_layout_destroy(rt->trace_layout);
	aprend_binding_layout_destroy(rt->mask_output_layout);
	aprend_binding_layout_destroy(rt->path_output_layout);
	aprend_shader_destroy(rt->writer_row64);
	aprend_shader_destroy(rt->writer_col72);
	aprend_shader_destroy(rt->emitter_comp);
	aprend_shader_destroy(rt->mask_comps[0]);
	aprend_shader_destroy(rt->mask_comps[1]);
	for (aprend_shader &shader : rt->path_comps) {
		aprend_shader_destroy(shader);
		shader = nullptr;
	}
	aprend_texture_view_destroy(rt->no_guide_view);
	aprend_texture2d_destroy(rt->no_guide);
	aprend_buffer_destroy(rt->blas_table);
	aprend_buffer_destroy(rt->lit_table);
	aprend_buffer_destroy(rt->arena);
	aprend_buffer_destroy(rt->scratch);
	rt->writer_pipeline = rt->emitter_pipeline = nullptr;
	rt->mask_pipelines[0] = rt->mask_pipelines[1] = nullptr;
	rt->writer_layout = rt->emitter_layout = rt->scene_layout = rt->trace_layout = rt->mask_output_layout = rt->path_output_layout = nullptr;
	rt->writer_row64 = rt->writer_col72 = rt->emitter_comp = nullptr;
	rt->mask_comps[0] = rt->mask_comps[1] = nullptr;
	rt->no_guide_view = nullptr;
	rt->no_guide      = nullptr;
	rt->blas_table = rt->lit_table = rt->arena = rt->scratch = nullptr;
}

void aprend_rt_state_free(aprend_instance instance) {
	aprend_rt_state *rt = instance->rt;
	if (!rt)
		return;
	instance->rt = nullptr;
	rt->~aprend_rt_state();
	free(rt);
}

// Takes a free index of a table whose per-index arrays are [arrays] (each
// pushed one element for a new index, [grow] then called for the GPU
// table's room), from [free_list] first. The generation stays what it is.
// 0, with nothing taken, if there is no memory.

/* ---- Lit materials (D4) ------------------------------------------------- */

static aprend_rt_lit_entry aprend_rt_lit_entry_of(const aprend_lit_material_block *block) {
	aprend_rt_lit_entry entry{};
	memcpy(entry.base_color, block->base_color, sizeof(entry.base_color));
	memcpy(entry.emission, block->emission, sizeof(entry.emission));
	entry.surface[0] = block->roughness;
	entry.surface[1] = block->metalness;
	entry.surface[2] = block->specular;
	entry.surface[3] = 0.0f;
	return entry;
}

APRESULT aprend_rt_lit_take(aprend_instance instance, const aprend_lit_material_block *block, uint32_t *out_slot, APREND_ERROR *error) {
	*out_slot = 0;
	if (!aprend_rt_supported(instance))
		return APRESULT_OK;
	aprend_rt_state *rt = aprend_rt_state_get(instance, error);
	if (!rt)
		return APRESULT_OUT_OF_MEMORY;
	uint32_t index = 0;
	if (!rt->lit_free.empty()) {
		index = rt->lit_free[rt->lit_free.size() - 1];
		(void)rt->lit_free.remove(rt->lit_free.size() - 1);
	} else {
		index = (uint32_t)rt->lit_generation.size();
		if (index > APREND_RT_SLOT_INDEX_MASK)
			return aprend_fail(error, APRESULT_LIMIT, "the lit-material table holds 2^24 materials");
		// Room for the new index everywhere first, so a failure leaves every
		// array as it was; the dirty list keeps room for every index.
		const size_t count = (size_t)index + 1;
		if (rt->lit_generation.reserve(count) != EQ_SUCCESS || rt->lit_live.reserve(count) != EQ_SUCCESS ||
		    rt->lit_entries.reserve(count) != EQ_SUCCESS || rt->lit_dirty.reserve(count) != EQ_SUCCESS ||
		    rt->lit_dirty_list.reserve(count) != EQ_SUCCESS || rt->lit_free.reserve(count) != EQ_SUCCESS)
			return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the lit-material table");
		// The table doubles when it is full: an allocation then and only then.
		const uint64_t capacity = rt->lit_table->desc.size / sizeof(aprend_rt_lit_entry);
		if (count > capacity) {
			if (const APRESULT grown = aprend_rt_grow_buffer(instance, &rt->lit_table, capacity * 2 * sizeof(aprend_rt_lit_entry), "aprend lit-material table", error))
				return grown;
		}
		(void)rt->lit_generation.push(0);
		(void)rt->lit_live.push(0);
		(void)rt->lit_entries.push(aprend_rt_lit_entry{});
		(void)rt->lit_dirty.push(0);
	}
	rt->lit_live[index]    = 1;
	rt->lit_entries[index] = aprend_rt_lit_entry_of(block);
	if (!rt->lit_dirty[index]) {
		rt->lit_dirty[index] = 1;
		(void)rt->lit_dirty_list.push(index);
	}
	++rt->lit_version;
	*out_slot = aprend_rt_slot(index, rt->lit_generation[index]);
	return APRESULT_OK;
}

void aprend_rt_lit_stage(aprend_instance instance, uint32_t slot, const aprend_lit_material_block *block) {
	aprend_rt_state *rt = instance->rt;
	if (!rt || slot == 0)
		return;
	const uint32_t index   = aprend_rt_slot_index(slot);
	rt->lit_entries[index] = aprend_rt_lit_entry_of(block);
	// The list has room for every index: this never allocates.
	if (!rt->lit_dirty[index]) {
		rt->lit_dirty[index] = 1;
		(void)rt->lit_dirty_list.push(index);
	}
	++rt->lit_version;
}

void aprend_rt_lit_give(aprend_instance instance, uint32_t slot) {
	aprend_rt_state *rt = instance->rt;
	if (!rt || slot == 0)
		return;
	const uint32_t index = aprend_rt_slot_index(slot);
	rt->lit_live[index]  = 0;
	rt->lit_generation[index] = (uint8_t)(rt->lit_generation[index] + 1);
	// Room for every index was made when it was taken.
	(void)rt->lit_free.push(index);
}

bool aprend_rt_lit_slot_live(aprend_instance instance, uint32_t slot) {
	aprend_rt_state *rt = instance->rt;
	if (!rt || slot == 0)
		return false;
	const uint32_t index = aprend_rt_slot_index(slot);
	return index < rt->lit_live.size() && rt->lit_live[index] && rt->lit_generation[index] == aprend_rt_slot_generation(slot);
}

/* ---- BLAS ---------------------------------------------------------------- */

aprend_blas_t::~aprend_blas_t() {
	// Released through the queue (aprend_blas_destroy), once the GPU has
	// finished everything submitted while the BLAS lived; or a failed
	// create's, before anything of it reached the GPU.
	if (this->full)
		spudgpu_destroy_acceleration_structure(this->full);
	if (this->compact)
		spudgpu_destroy_acceleration_structure(this->compact);
	if (this->retiring)
		spudgpu_destroy_acceleration_structure(this->retiring);
	if (this->size_readback)
		spudgpu_destroy_buffer(this->size_readback);
	aprend_buffer_destroy(this->size_device);
	if (this->arena_units && this->instance->rt) {
		try {
			(void)this->instance->rt->arena_ranges.give(this->arena_offset, this->arena_units);
		} catch (const std::bad_alloc &) {
			// The range stays taken: memory lost to the arena, not a fault.
		}
	}
	free(this->geometries);
	free(this->debug_name);
}

// [blas]'s inputs for SpudGPU into [geometries] (geometry_count of them) and
// [inputs], as its builds and the size query read them.
static void aprend_rt_blas_inputs(const aprend_blas_t *blas, spudgpu_acceleration_structure_geometry *geometries, spudgpu_acceleration_structure_inputs *inputs) {
	for (uint32_t g = 0; g < blas->geometry_count; ++g) {
		const aprend_rt_geometry &geometry       = blas->geometries[g];
		spudgpu_acceleration_structure_geometry &out = geometries[g];
		out                                      = {};
		out.kind                                 = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES;
		// Opaque to rays in this version (aprendraytracing.h, T2).
		out.flags       = SPUDGPU_ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE;
		out.data        = geometry.vertices->parts[0].store.buffer;
		out.data_offset = geometry.vertex_offset;
		out.data_stride = APREND_RT_VERTEX_STRIDE;
		out.data_count  = geometry.vertex_count;
		if (geometry.indices) {
			out.index_format = SPUDGPU_FORMAT_R32_UINT;
			out.indices      = geometry.indices->parts[0].store.buffer;
			out.index_offset = geometry.index_offset;
			out.index_count  = geometry.index_count;
		} else {
			out.index_format = SPUDGPU_FORMAT_UNKNOWN;
		}
	}
	*inputs                = {};
	inputs->base.type      = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	inputs->level          = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	inputs->flags          = aprend_rt_blas_flags(blas->deforms);
	inputs->geometries     = geometries;
	inputs->geometry_count = blas->geometry_count;
}

SPUDGPU_ACCELERATION_STRUCTURE_BUILD_FLAGS aprend_rt_blas_flags(bool deforms) {
	// T3: a deforming BLAS built fast and refitted; a static one built for
	// fast tracing, and compactable.
	return deforms ? (SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD | SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_UPDATE)
	               : (SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_TRACE | SPUDGPU_ACCELERATION_STRUCTURE_BUILD_ALLOW_COMPACTION);
}

void aprend_rt_blas_describe(const aprend_blas_t *blas, spudgpu_acceleration_structure_geometry *geometries, spudgpu_acceleration_structure_inputs *inputs) {
	aprend_rt_blas_inputs(blas, geometries, inputs);
}

// Whether [buffer] is one of [instance]'s, and one copy.
static const char *aprend_rt_buffer_fault(aprend_instance instance, aprend_buffer buffer) {
	if (!buffer)
		return "is NULL";
	if (buffer->instance != instance)
		return "is of another instance";
	if (buffer->desc.per_frame)
		return "is one of per-frame copies, which a build can't name";
	return nullptr;
}

extern "C" {

APRESULT aprend_blas_create(aprend_instance instance, const APREND_BLAS_DESC *desc, aprend_blas *out_blas, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_blas)
		*out_blas = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_blas_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_blas_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_BLAS_DESC))
		return base;
	if (!out_blas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_blas is NULL");
	if (!aprend_rt_supported(instance))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "aprend_blas_create: " APREND_RT_UNSUPPORTED_MESSAGE);
	if (!desc->_geometries)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc->_geometries is NULL");
	if (desc->_geometry_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc->_geometry_count is 0");
	aprend_rt_state *rt = aprend_rt_state_get(instance, out_error);
	if (!rt)
		return APRESULT_OUT_OF_MEMORY;
	aprend_rt_drain(instance);
	if (desc->_geometry_count > rt->limits.max_geometries)
		return aprend_fail(out_error, APRESULT_LIMIT, "desc->_geometry_count %u is past the device's %llu geometries", desc->_geometry_count,
		                   (unsigned long long)rt->limits.max_geometries);
	uint64_t primitives = 0;
	uint64_t units      = (uint64_t)desc->_geometry_count * sizeof(aprend_rt_geometry_header) / APREND_RT_ARENA_UNIT;
	for (uint32_t g = 0; g < desc->_geometry_count; ++g) {
		const APREND_DRAW_MESH &mesh = desc->_geometries[g];
		if (mesh._vertex_buffer_count == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u has no vertex buffer", g);
		aprend_buffer vertices = mesh._vertex_buffers[0];
		if (const char *fault = aprend_rt_buffer_fault(instance, vertices))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's first vertex buffer %s", g, fault);
		if (!(vertices->desc.usage & APREND_BUFFER_USAGE_VERTEX))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's first vertex buffer lacks APREND_BUFFER_USAGE_VERTEX", g);
		aprend_buffer indices = mesh._index_buffer;
		if (indices) {
			if (const char *fault = aprend_rt_buffer_fault(instance, indices))
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's index buffer %s", g, fault);
			if (!(indices->desc.usage & APREND_BUFFER_USAGE_INDEX))
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's index buffer lacks APREND_BUFFER_USAGE_INDEX", g);
			if (indices->desc.index_stride != APREND_INDEX_STRIDE_UINT32)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's index buffer is not of 32-bit indices", g);
		}
		if (mesh._count == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's _count is 0", g);
		if (mesh._count % 3 != 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's _count %u is not a multiple of 3", g, mesh._count);
		const uint64_t vertex_capacity = vertices->desc.size / APREND_RT_VERTEX_STRIDE;
		uint64_t vertex_count          = 0;
		if (indices) {
			if ((uint64_t)mesh._first + mesh._count > indices->desc.size / 4)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's indices run past its index buffer", g);
			// Every vertex from _base_vertex to the buffer's end may be named.
			if (mesh._base_vertex < 0 || (uint64_t)mesh._base_vertex >= vertex_capacity)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's _base_vertex %d is outside its vertex buffer", g, mesh._base_vertex);
			vertex_count = vertex_capacity - (uint64_t)mesh._base_vertex;
		} else {
			if ((uint64_t)mesh._first + mesh._count > vertex_capacity)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "geometry %u's vertices run past its vertex buffer", g);
			vertex_count = mesh._count;
		}
		if (vertex_count > UINT32_MAX)
			return aprend_fail(out_error, APRESULT_LIMIT, "geometry %u reaches more than 2^32 vertices", g);
		primitives += mesh._count / 3;
		units += (vertex_count * APREND_RT_VERTEX_STRIDE + APREND_RT_ARENA_UNIT - 1) / APREND_RT_ARENA_UNIT;
		if (indices)
			units += ((uint64_t)mesh._count * 4 + APREND_RT_ARENA_UNIT - 1) / APREND_RT_ARENA_UNIT;
	}
	if (primitives > rt->limits.max_primitives)
		return aprend_fail(out_error, APRESULT_LIMIT, "the geometries' %llu triangles are past the device's %llu", (unsigned long long)primitives,
		                   (unsigned long long)rt->limits.max_primitives);
	if (units > APREND_RT_ARENA_MAX_UNITS / 2)
		return aprend_fail(out_error, APRESULT_LIMIT, "the geometries are past what the geometry arena holds");

	// Everything checked: the handle, its geometries, its structure, its
	// slot and its range of the arena.
	APREND_MALLOC__T(result, aprend_blas_t);
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the BLAS");
	APREND_CONSTRUCT__T(result, aprend_blas_t);
	result->instance       = instance;
	result->deforms        = desc->_deforms;
	result->geometry_count = desc->_geometry_count;
	result->primitive_count = primitives;
	result->geometries     = (aprend_rt_geometry *)calloc(desc->_geometry_count, sizeof(aprend_rt_geometry));
	if (desc->debug_name) {
		const size_t length = strlen(desc->debug_name) + 1;
		result->debug_name  = (char *)malloc(length);
		if (result->debug_name)
			memcpy(result->debug_name, desc->debug_name, length);
	}
	if (!result->geometries || (desc->debug_name && !result->debug_name)) {
		APREND_DESTRUCT__T(result, aprend_blas_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the BLAS's geometries");
	}
	for (uint32_t g = 0; g < desc->_geometry_count; ++g) {
		const APREND_DRAW_MESH &mesh   = desc->_geometries[g];
		aprend_rt_geometry &geometry   = result->geometries[g];
		geometry.vertices              = mesh._vertex_buffers[0];
		geometry.indices               = mesh._index_buffer;
		geometry.triangle_count        = mesh._count / 3;
		const uint64_t vertex_capacity = geometry.vertices->desc.size / APREND_RT_VERTEX_STRIDE;
		if (geometry.indices) {
			geometry.vertex_offset = (uint64_t)mesh._base_vertex * APREND_RT_VERTEX_STRIDE;
			geometry.vertex_count  = (uint32_t)(vertex_capacity - (uint64_t)mesh._base_vertex);
			geometry.index_offset  = (uint64_t)mesh._first * 4;
			geometry.index_count   = mesh._count;
		} else {
			geometry.vertex_offset = (uint64_t)mesh._first * APREND_RT_VERTEX_STRIDE;
			geometry.vertex_count  = mesh._count;
		}
	}

	spudgpu_acceleration_structure_geometry *spud_geometries =
	    (spudgpu_acceleration_structure_geometry *)calloc(desc->_geometry_count, sizeof(spudgpu_acceleration_structure_geometry));
	if (!spud_geometries) {
		APREND_DESTRUCT__T(result, aprend_blas_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the BLAS's geometries");
	}
	spudgpu_acceleration_structure_inputs inputs{};
	aprend_rt_blas_inputs(result, spud_geometries, &inputs);
	spudgpu_acceleration_structure_sizes sizes{};
	sizes.base.type = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES;
	SPUDRESULT sr   = spudgpu_get_acceleration_structure_build_sizes(instance->desc.device, &inputs, &sizes);
	free(spud_geometries);
	if (SPUDFAIL(sr)) {
		APREND_DESTRUCT__T(result, aprend_blas_t);
		return aprend_fail_spud(out_error, sr, "the BLAS's sizes can't be read");
	}
	result->full_size      = sizes.storage_size;
	result->build_scratch  = sizes.build_scratch_size;
	result->update_scratch = sizes.update_scratch_size;
	spudgpu_acceleration_structure_desc ad{};
	ad.base.type        = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	ad.level            = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	ad.size             = sizes.storage_size;
	ad.allow_compaction = !desc->_deforms;
	ad.debug_name       = desc->debug_name;
	sr                  = spudgpu_create_acceleration_structure(instance->desc.device, &ad, &result->full);
	if (SPUDFAIL(sr)) {
		result->full = nullptr;
		APREND_DESTRUCT__T(result, aprend_blas_t);
		return aprend_fail_spud(out_error, sr, "the BLAS's structure of %llu bytes can't be created", (unsigned long long)sizes.storage_size);
	}
	if (!desc->_deforms) {
		// Where its compacted size is written after a build (a storage
		// buffer the device writes it into), and read back from.
		result->size_device = aprend_rt_make_buffer(instance, 16, 0, "aprend BLAS compacted size", out_error);
		if (!result->size_device) {
			APREND_DESTRUCT__T(result, aprend_blas_t);
			return APRESULT_OUT_OF_MEMORY;
		}
		spudgpu_buffer_desc rd{};
		rd.base.type   = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
		rd.memory_kind = SPUDGPU_MEMORY_KIND_READBACK;
		rd.size        = 16;
		rd.usage       = SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
		sr             = spudgpu_create_buffer(instance->desc.device, &rd, &result->size_readback);
		if (SPUD_SUCCESS == sr)
			sr = spudgpu_map_buffer(result->size_readback, 0, 16, &result->size_mapped);
		if (SPUDFAIL(sr)) {
			APREND_DESTRUCT__T(result, aprend_blas_t);
			return aprend_fail_spud(out_error, sr, "the BLAS's compacted size buffer can't be created");
		}
	}

	// The arena range, the arena doubling while it is too small (an
	// allocation then and only then).
	uint32_t offset = 0;
	while (!rt->arena_ranges.take((uint32_t)units, &offset)) {
		const uint32_t old_units = rt->arena_ranges.size;
		if ((uint64_t)old_units * 2 > APREND_RT_ARENA_MAX_UNITS) {
			APREND_DESTRUCT__T(result, aprend_blas_t);
			return aprend_fail(out_error, APRESULT_LIMIT, "the geometry arena holds no more");
		}
		if (const APRESULT grown = aprend_rt_grow_buffer(instance, &rt->arena, (uint64_t)old_units * 2 * APREND_RT_ARENA_UNIT, "aprend geometry arena", out_error)) {
			APREND_DESTRUCT__T(result, aprend_blas_t);
			return grown;
		}
		try {
			rt->arena_ranges.size = old_units * 2;
			(void)rt->arena_ranges.give(old_units, old_units);
		} catch (const std::bad_alloc &) {
			rt->arena_ranges.size = old_units;
			APREND_DESTRUCT__T(result, aprend_blas_t);
			return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the geometry arena's ranges");
		}
	}
	result->arena_offset = offset;
	result->arena_units  = (uint32_t)units;
	// Where each geometry's vertices and indices go, after the headers.
	uint64_t at = (uint64_t)offset * APREND_RT_ARENA_UNIT + (uint64_t)desc->_geometry_count * sizeof(aprend_rt_geometry_header);
	for (uint32_t g = 0; g < desc->_geometry_count; ++g) {
		aprend_rt_geometry &geometry = result->geometries[g];
		at                           = (at + APREND_RT_ARENA_UNIT - 1) / APREND_RT_ARENA_UNIT * APREND_RT_ARENA_UNIT;
		geometry.arena_vertices      = at;
		at += (uint64_t)geometry.vertex_count * APREND_RT_VERTEX_STRIDE;
		if (geometry.indices) {
			at                     = (at + APREND_RT_ARENA_UNIT - 1) / APREND_RT_ARENA_UNIT * APREND_RT_ARENA_UNIT;
			geometry.arena_indices = at;
			at += (uint64_t)geometry.index_count * 4;
		}
	}

	// The slot: a free index, or a new one, the table doubling when full.
	// The table's arrays are read and changed under the instance's
	// release_mutex, since a send on another list (the seal, T6) reads them
	// meanwhile; the GPU table grows outside it (its old buffer's retire
	// takes the lock).
	{
		const uint64_t capacity = rt->blas_table->desc.size / sizeof(aprend_rt_blas_entry);
		bool grow               = false;
		{
			std::lock_guard<std::mutex> lock(instance->release_mutex);
			grow = rt->blas_free.empty() && rt->blas_generation.size() + 1 > capacity;
		}
		if (grow) {
			if (const APRESULT grown = aprend_rt_grow_buffer(instance, &rt->blas_table, capacity * 2 * sizeof(aprend_rt_blas_entry), "aprend BLAS table", out_error)) {
				APREND_DESTRUCT__T(result, aprend_blas_t);
				return grown;
			}
		}
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (aprend_instance_reserve_release(instance, 1) != APRESULT_OK) {
		APREND_DESTRUCT__T(result, aprend_blas_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to release the BLAS later");
	}
	uint32_t index = 0;
	bool room      = true;
	{
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		if (!rt->blas_free.empty()) {
			index = rt->blas_free[rt->blas_free.size() - 1];
			(void)rt->blas_free.remove(rt->blas_free.size() - 1);
		} else {
			index              = (uint32_t)rt->blas_generation.size();
			const size_t count = (size_t)index + 1;
			room = index <= APREND_RT_SLOT_INDEX_MASK && rt->blas_generation.reserve(count) == EQ_SUCCESS && rt->blas_live.reserve(count) == EQ_SUCCESS &&
			       rt->blas_built.reserve(count) == EQ_SUCCESS && rt->blas_generation_debug.reserve(count) == EQ_SUCCESS &&
			       rt->blas_structure.reserve(count) == EQ_SUCCESS && rt->blas_geometry_count.reserve(count) == EQ_SUCCESS &&
			       rt->blas_free.reserve(count) == EQ_SUCCESS && rt->blas_freed.reserve(count) == EQ_SUCCESS;
			if (room) {
				(void)rt->blas_generation.push(0);
				(void)rt->blas_live.push(0);
				(void)rt->blas_built.push(0);
				(void)rt->blas_generation_debug.push(0);
				(void)rt->blas_structure.push(nullptr);
				(void)rt->blas_geometry_count.push(0);
			}
		}
		if (room) {
			rt->blas_live[index]           = 1;
			rt->blas_built[index]          = 0;
			rt->blas_structure[index]      = result->full;
			rt->blas_geometry_count[index] = desc->_geometry_count;
			result->slot                   = aprend_rt_slot(index, rt->blas_generation[index]);
		}
	}
	if (!room) {
		aprend_instance_unreserve_release(instance, 1);
		APREND_DESTRUCT__T(result, aprend_blas_t);
		return aprend_fail(out_error, index > APREND_RT_SLOT_INDEX_MASK ? APRESULT_LIMIT : APRESULT_OUT_OF_MEMORY, "no room for the BLAS's slot");
	}
	*out_blas = result;
	return APRESULT_OK;
}

void aprend_blas_destroy(aprend_blas blas) {
	if (!blas)
		return;
	if (APRI_STALE(blas, "aprend_blas_destroy", "blas"))
		return;
	aprend_instance instance = blas->instance;
	// A list compiled with it and not yet submitted is refused at submit.
	aprend_instance_forget(instance, blas);
	// Its index is free for the next BLAS at once, the generation advanced,
	// so a record still holding the old slot is refused (D1, T6): taken
	// into the table by the next call on the instance (aprend_rt_drain),
	// which a destroy from any thread may not touch. The structures go once
	// the GPU has finished with them.
	{
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		// Room for every index was made when it was taken.
		(void)instance->rt->blas_freed.push(aprend_rt_slot_index(blas->slot));
	}
	APRI_KILL(blas);
	aprend_instance_retire(instance, blas, &aprend_release_handle<aprend_blas_t>);
}

} // extern "C"

void aprend_rt_drain(aprend_instance instance) {
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	aprend_rt_drain_locked(instance->rt);
}

void aprend_rt_drain_locked(aprend_rt_state *rt) {
	for (uint32_t index : rt->blas_freed) {
		rt->blas_live[index]       = 0;
		rt->blas_built[index]      = 0;
		rt->blas_structure[index]  = nullptr;
		rt->blas_generation[index] = (uint8_t)(rt->blas_generation[index] + 1);
		// The Debug build's check of an 8-bit generation that has wrapped
		// (T6) reads this; Release keeps it unread.
		++rt->blas_generation_debug[index];
		// Room for every index was made when it was taken.
		(void)rt->blas_free.push(index);
	}
	rt->blas_freed.clear();
}

extern "C" {

uint32_t aprend_blas_get_slot(aprend_blas blas) {
	if (!blas)
		return 0;
	if (APRI_STALE(blas, "aprend_blas_get_slot", "blas"))
		return 0;
	APREND_ENTER_BLAS(blas, "aprend_blas_get_slot");
	return blas->slot;
}

APRESULT aprend_blas_get_info(aprend_blas blas, APREND_BLAS_INFO *out_info, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!blas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "blas is NULL");
	if (APRI_STALE(blas, "aprend_blas_get_info", "blas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "blas is destroyed");
	APREND_ENTER_BLAS(blas, "aprend_blas_get_info");
	if (!out_info)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_info is NULL");
	if (const APRESULT base = aprend_base_out_check(out_error, "out_info", &out_info->base, APRI_STRUCT_TYPE_APREND_BLAS_INFO))
		return base;
	aprend_rt_blas_settle(blas);
	out_info->_storage_bytes   = (blas->full ? blas->full_size : 0) + (blas->compact ? blas->compacted_size : 0) +
	                           (blas->retiring ? (blas->retiring == blas->compact ? 0 : (blas->full ? blas->compacted_size : blas->full_size)) : 0);
	out_info->_compacted_bytes = blas->deforms ? 0 : blas->compacted_size;
	out_info->_built           = blas->built;
	out_info->_compacted       = blas->compact != nullptr && blas->full == nullptr;
	return APRESULT_OK;
}

} // extern "C"

void aprend_rt_blas_settle(aprend_blas blas) {
	aprend_instance instance = blas->instance;
	const uint64_t completed = spudgpu_get_fence_value(instance->frame_fence);
	// A build whose submission has completed, in a frame
	// aprend_instance_next_frame() has moved on from: built, and a static
	// one's compacted size read back.
	if (blas->built_serial != 0 && completed >= blas->built_serial && instance->frame_number > blas->built_frame) {
		blas->built = true;
		if (blas->size_mapped && blas->compacted_size == 0 && blas->full) {
			if (spudgpu_invalidate_buffer(blas->size_readback, 0, 16) == SPUD_SUCCESS) {
				uint64_t size = 0;
				memcpy(&size, blas->size_mapped, sizeof(size));
				blas->compacted_size = size;
			}
		}
		blas->built_serial = 0;
	}
	// A compaction's or a rebuild's frame has completed: the structure it
	// gave up goes (T4).
	if (blas->retiring && blas->retiring_serial != 0 && completed >= blas->retiring_serial && instance->frame_number > blas->retiring_frame) {
		spudgpu_destroy_acceleration_structure(blas->retiring);
		blas->retiring        = nullptr;
		blas->retiring_serial = 0;
	}
}

/* ---- TLAS ---------------------------------------------------------------- */

aprend_tlas_t::~aprend_tlas_t() {
	if (this->structure)
		spudgpu_destroy_acceleration_structure(this->structure);
	aprend_binding_set_destroy(this->trace_set);
	aprend_binding_set_destroy(this->writer_set);
	aprend_binding_set_destroy(this->emitter_set);
	aprend_buffer_destroy(this->records);
	aprend_buffer_destroy(this->materials);
	aprend_buffer_destroy(this->device_instances);
	aprend_buffer_destroy(this->emitters);
	if (this->sets) {
		for (uint32_t s = 0; s < this->set_count; ++s) {
			if (this->sets[s].records)
				spudgpu_destroy_buffer(this->sets[s].records);
			if (this->sets[s].materials)
				spudgpu_destroy_buffer(this->sets[s].materials);
		}
	}
	free(this->sets);
	free(this->record_blocks);
	free(this->material_blocks);
	free(this->bottom_levels);
	free(this->latest);
	free(this->latest_materials);
	free(this->reaches_null);
	free(this->debug_generations);
	free(this->build_levels);
	free(this->debug_name);
}

static void aprend_rt_mark_blocks(uint64_t *blocks, void *set_bytes, const void *latest, size_t element, uint32_t total, uint32_t first, uint32_t count);

// Frees every set of [tlas] whose build has completed (T5).
static void aprend_rt_tlas_settle(aprend_tlas tlas) {
	const uint64_t completed = spudgpu_get_fence_value(tlas->instance->frame_fence);
	for (uint32_t s = 0; s < tlas->set_count; ++s) {
		aprend_rt_set &set = tlas->sets[s];
		if (set.state == APREND_RT_SET_SUBMITTED && completed >= set.serial)
			set.state = APREND_RT_SET_FREE;
	}
}

// The set being prepared, a free one taken if there is none. UINT32_MAX,
// reported through [error] naming the count, if every set is sealed and in
// use.
static uint32_t aprend_rt_tlas_preparing(aprend_tlas tlas, APREND_ERROR *error) {
	if (tlas->preparing != UINT32_MAX)
		return tlas->preparing;
	aprend_rt_tlas_settle(tlas);
	for (uint32_t s = 0; s < tlas->set_count; ++s) {
		if (tlas->sets[s].state != APREND_RT_SET_FREE)
			continue;
		tlas->sets[s].state = APREND_RT_SET_PREPARING;
		memset(tlas->record_blocks + (size_t)s * tlas->record_words, 0, sizeof(uint64_t) * tlas->record_words);
		memset(tlas->material_blocks + (size_t)s * tlas->material_words, 0, sizeof(uint64_t) * tlas->material_words);
		tlas->preparing = s;
		// The first set ever writes every block, so its build fills the
		// device's copies whole: a record the count reaches that was never
		// written reads as slot 0 (no BLAS, an inactive instance), and a
		// material table entry as NULL, never what the memory held.
		if (tlas->next_sequence == 1) {
			aprend_rt_set &first = tlas->sets[s];
			aprend_rt_mark_blocks(tlas->record_blocks + (size_t)s * tlas->record_words, first.records_mapped, tlas->latest, sizeof(APREND_RAY_INSTANCE),
			                      tlas->max_instances, 0, tlas->max_instances);
			aprend_rt_mark_blocks(tlas->material_blocks + (size_t)s * tlas->material_words, first.materials_mapped, tlas->latest_materials,
			                      sizeof(uint32_t), tlas->max_materials, 0, tlas->max_materials);
		}
		return s;
	}
	aprend_fail(error, APRESULT_LIMIT, "every one of the TLAS's %u instance sets is sealed and in use until its build has completed", tlas->set_count);
	return UINT32_MAX;
}

// Whether record [r] of [tlas]'s latest reaches a NULL material table entry
// (or past the table, which reads as one): T7.
static bool aprend_rt_record_reaches_null(const aprend_tlas_t *tlas, const APREND_RAY_INSTANCE &r) {
	const aprend_rt_state *rt = tlas->instance->rt;
	const uint32_t index      = aprend_rt_slot_index(r._blas);
	const uint32_t geometries = index < rt->blas_geometry_count.size() ? rt->blas_geometry_count[index] : 1;
	for (uint32_t g = 0; g < geometries; ++g) {
		const uint64_t entry = (uint64_t)r._material_base + g;
		if (entry >= tlas->max_materials)
			return true;
		if (tlas->latest_materials[entry] == 0)
			return true;
	}
	return false;
}

// Counts record [i] of [tlas]'s latest in or out (by [delta], +1 or -1):
// its slot's count (T6) and its NULL reach by mask bit (T7).
static void aprend_rt_count_record(aprend_tlas tlas, uint32_t i, int delta) {
	const APREND_RAY_INSTANCE &r = tlas->latest[i];
	const uint32_t index         = aprend_rt_slot_index(r._blas);
	aprend_rt_slot_count &count  = tlas->slot_counts[index];
	const uint8_t generation     = (uint8_t)aprend_rt_slot_generation(r._blas);
	if (delta > 0) {
		if (count.current == 0 && count.other == 0)
			count.generation = generation;
		if (generation == count.generation)
			++count.current;
		else
			++count.other;
	} else {
		if (generation == count.generation)
			--count.current;
		else
			--count.other;
	}
	if (tlas->reaches_null[i]) {
		for (uint32_t bit = 0; bit < 8; ++bit)
			if (r._mask & (1u << bit))
				tlas->null_by_bit[bit] = delta > 0 ? tlas->null_by_bit[bit] + 1 : tlas->null_by_bit[bit] - 1;
	}
}

// Recounts every index's records from nothing: what a count whose
// generation has gone (count.current 0, others left) needs. Rare.
static void aprend_rt_recount(aprend_tlas tlas) {
	for (size_t i = 0; i < tlas->slot_counts.size(); ++i)
		tlas->slot_counts[i] = aprend_rt_slot_count{};
	memset(tlas->null_by_bit, 0, sizeof(tlas->null_by_bit));
	for (uint32_t i = 0; i < tlas->instance_count; ++i)
		aprend_rt_count_record(tlas, i, +1);
}

// The slot counts' room for every BLAS index of the instance. False if there
// is no memory (grown only when the BLAS table has grown).
static bool aprend_rt_tlas_room(aprend_tlas tlas) {
	const size_t indices = tlas->instance->rt->blas_generation.size();
	if (tlas->slot_counts.size() >= indices)
		return true;
	return tlas->slot_counts.resize(indices) == EQ_SUCCESS;
}

// Copies the blocks [first, first + count) of [latest] (of [element] bytes,
// [total] in all) touches into [set_bytes], marking them in [blocks]: a
// block a set writes holds every element as the writes left it.
static void aprend_rt_mark_blocks(uint64_t *blocks, void *set_bytes, const void *latest, size_t element, uint32_t total, uint32_t first, uint32_t count) {
	const uint32_t first_block = first / APREND_RT_BLOCK;
	const uint32_t last_block  = (first + count - 1) / APREND_RT_BLOCK;
	for (uint32_t b = first_block; b <= last_block; ++b) {
		blocks[b / 64] |= 1ull << (b % 64);
		const uint32_t begin = b * APREND_RT_BLOCK;
		const uint32_t end   = begin + APREND_RT_BLOCK < total ? begin + APREND_RT_BLOCK : total;
		memcpy((uint8_t *)set_bytes + (size_t)begin * element, (const uint8_t *)latest + (size_t)begin * element, (size_t)(end - begin) * element);
	}
}

extern "C" {

APRESULT aprend_tlas_create(aprend_instance instance, const APREND_TLAS_DESC *desc, aprend_tlas *out_tlas, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_tlas)
		*out_tlas = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_tlas_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_tlas_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_TLAS_DESC))
		return base;
	if (!out_tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_tlas is NULL");
	if (!aprend_rt_supported(instance))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "aprend_tlas_create: " APREND_RT_UNSUPPORTED_MESSAGE);
	if (desc->_max_instances == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc->_max_instances is 0");
	aprend_rt_state *rt = aprend_rt_state_get(instance, out_error);
	if (!rt)
		return APRESULT_OUT_OF_MEMORY;
	if (desc->_max_instances > rt->limits.max_instances)
		return aprend_fail(out_error, APRESULT_LIMIT, "desc->_max_instances %u is past the device's %llu", desc->_max_instances,
		                   (unsigned long long)rt->limits.max_instances);
	if (desc->_max_materials == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc->_max_materials is 0");
	if (desc->_max_instance_sets < 2)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc->_max_instance_sets %u is below 2", desc->_max_instance_sets);
	// Every size the sets take, checked before anything sizes memory.
	const uint64_t record_bytes   = (uint64_t)desc->_max_instances * sizeof(APREND_RAY_INSTANCE);
	const uint64_t material_bytes = (uint64_t)desc->_max_materials * sizeof(uint32_t);
	const uint64_t per_set        = record_bytes + material_bytes;
	if (per_set > UINT64_MAX / desc->_max_instance_sets)
		return aprend_fail(out_error, APRESULT_LIMIT, "the TLAS's %u sets of %llu bytes overflow 64 bits", desc->_max_instance_sets, (unsigned long long)per_set);
	const uint64_t instance_bytes = (uint64_t)desc->_max_instances * rt->instance_layout.size;

	APREND_MALLOC__T(result, aprend_tlas_t);
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the TLAS");
	APREND_CONSTRUCT__T(result, aprend_tlas_t);
	result->instance       = instance;
	result->serial         = aprend_instance_next_serial(instance);
	result->max_instances  = desc->_max_instances;
	result->max_materials  = desc->_max_materials;
	result->set_count      = desc->_max_instance_sets;
	result->record_words   = (desc->_max_instances / APREND_RT_BLOCK + 63) / 64 + 1;
	result->material_words = (desc->_max_materials / APREND_RT_BLOCK + 63) / 64 + 1;
	if (desc->debug_name) {
		const size_t length = strlen(desc->debug_name) + 1;
		result->debug_name  = (char *)malloc(length);
		if (result->debug_name)
			memcpy(result->debug_name, desc->debug_name, length);
	}
	result->sets             = (aprend_rt_set *)calloc(desc->_max_instance_sets, sizeof(aprend_rt_set));
	result->record_blocks    = (uint64_t *)calloc((size_t)desc->_max_instance_sets * result->record_words, sizeof(uint64_t));
	result->material_blocks  = (uint64_t *)calloc((size_t)desc->_max_instance_sets * result->material_words, sizeof(uint64_t));
	result->bottom_levels    = (uint32_t *)calloc((size_t)desc->_max_instance_sets * desc->_max_instances, sizeof(uint32_t));
	result->latest           = (APREND_RAY_INSTANCE *)calloc(desc->_max_instances, sizeof(APREND_RAY_INSTANCE));
	result->latest_materials = (uint32_t *)calloc(desc->_max_materials, sizeof(uint32_t));
	result->reaches_null     = (uint8_t *)calloc(desc->_max_instances, 1);
#if _DEBUG
	result->debug_generations = (uint32_t *)calloc(desc->_max_instances, sizeof(uint32_t));
	const bool debug_ok       = result->debug_generations != nullptr;
#else
	const bool debug_ok = true;
#endif
	result->build_levels = (spudgpu_acceleration_structure *)calloc(desc->_max_instances, sizeof(spudgpu_acceleration_structure));
	if (!result->sets || !result->record_blocks || !result->material_blocks || !result->bottom_levels || !result->latest || !result->latest_materials ||
	    !result->reaches_null || !result->build_levels || (desc->debug_name && !result->debug_name) || !debug_ok || !aprend_rt_tlas_room(result)) {
		APREND_DESTRUCT__T(result, aprend_tlas_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the TLAS's sets");
	}
	SPUDRESULT sr = SPUD_SUCCESS;
	for (uint32_t s = 0; s < desc->_max_instance_sets && SPUD_SUCCESS == sr; ++s) {
		aprend_rt_set &set = result->sets[s];
		spudgpu_buffer_desc bd{};
		bd.base.type   = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
		bd.memory_kind = SPUDGPU_MEMORY_KIND_UPLOAD;
		bd.usage       = SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
		bd.size        = record_bytes;
		sr             = spudgpu_create_buffer(instance->desc.device, &bd, &set.records);
		if (SPUD_SUCCESS == sr)
			sr = spudgpu_map_buffer(set.records, 0, record_bytes, (void **)&set.records_mapped);
		bd.size = material_bytes;
		if (SPUD_SUCCESS == sr)
			sr = spudgpu_create_buffer(instance->desc.device, &bd, &set.materials);
		if (SPUD_SUCCESS == sr)
			sr = spudgpu_map_buffer(set.materials, 0, material_bytes, (void **)&set.materials_mapped);
	}
	if (SPUDFAIL(sr)) {
		APREND_DESTRUCT__T(result, aprend_tlas_t);
		return aprend_fail_spud(out_error, sr, "the TLAS's instance sets can't be created");
	}
	// The structure, at the size the device reports for _max_instances.
	spudgpu_acceleration_structure_inputs inputs{};
	inputs.base.type      = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
	inputs.level          = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
	inputs.flags          = SPUDGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
	result->device_instances = aprend_rt_make_buffer(instance, instance_bytes, SPUDGPU_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT, "aprend TLAS instances", out_error);
	result->records          = aprend_rt_make_buffer(instance, record_bytes, 0, "aprend TLAS records", out_error);
	result->materials        = aprend_rt_make_buffer(instance, material_bytes, 0, "aprend TLAS material table", out_error);
	result->emitters         = aprend_rt_make_buffer(instance, ((uint64_t)desc->_max_instances + 1) * sizeof(uint32_t), 0, "aprend TLAS emitters", out_error);
	if (!result->device_instances || !result->records || !result->materials || !result->emitters) {
		APREND_DESTRUCT__T(result, aprend_tlas_t);
		return APRESULT_OUT_OF_MEMORY;
	}
	inputs.instances      = result->device_instances->parts[0].store.buffer;
	inputs.instance_count = desc->_max_instances;
	spudgpu_acceleration_structure_sizes sizes{};
	sizes.base.type = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_SIZES;
	sr              = spudgpu_get_acceleration_structure_build_sizes(instance->desc.device, &inputs, &sizes);
	if (SPUD_SUCCESS == sr) {
		spudgpu_acceleration_structure_desc ad{};
		ad.base.type         = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
		ad.level             = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
		ad.size              = sizes.storage_size;
		ad.max_bottom_levels = desc->_max_instances;
		ad.debug_name        = desc->debug_name;
		sr                   = spudgpu_create_acceleration_structure(instance->desc.device, &ad, &result->structure);
	}
	if (SPUDFAIL(sr)) {
		result->structure = nullptr;
		APREND_DESTRUCT__T(result, aprend_tlas_t);
		return aprend_fail_spud(out_error, sr, "the TLAS's structure can't be created");
	}
	result->structure_size = sizes.storage_size;
	result->build_scratch  = sizes.build_scratch_size;
	// Listed among the live ones, with room made first.
	{
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		if (rt->live_tlases.push({result, result->serial}) != EQ_SUCCESS) {
			APREND_DESTRUCT__T(result, aprend_tlas_t);
			return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to list the TLAS");
		}
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (aprend_instance_reserve_release(instance, 1) != APRESULT_OK) {
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		(void)rt->live_tlases.remove(rt->live_tlases.size() - 1);
		APREND_DESTRUCT__T(result, aprend_tlas_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to release the TLAS later");
	}
	*out_tlas = result;
	return APRESULT_OK;
}

void aprend_tlas_destroy(aprend_tlas tlas) {
	if (!tlas)
		return;
	if (APRI_STALE(tlas, "aprend_tlas_destroy", "tlas"))
		return;
	// A list compiled with it and not yet submitted is refused at submit; a
	// list holding a seal of it no longer reaches it (aprend_rt_abandon).
	aprend_instance_forget(tlas->instance, tlas);
	{
		std::lock_guard<std::mutex> lock(tlas->instance->release_mutex);
		eq::array<aprend_rt_state::live_tlas> &live = tlas->instance->rt->live_tlases;
		for (size_t i = 0; i < live.size(); ++i) {
			if (live[i].tlas == tlas) {
				(void)live.remove_swap(i);
				break;
			}
		}
	}
	APRI_KILL(tlas);
	aprend_instance_retire(tlas->instance, tlas, &aprend_release_handle<aprend_tlas_t>);
}

APRESULT aprend_tlas_write_instances(aprend_tlas tlas, uint32_t first, const APREND_RAY_INSTANCE *instances, uint32_t count, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is NULL");
	if (APRI_STALE(tlas, "aprend_tlas_write_instances", "tlas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is destroyed");
	APREND_ENTER_TLAS(tlas, "aprend_tlas_write_instances");
	APREND_ENTER(tlas->instance, "aprend_tlas_write_instances");
	if (!aprend_rt_supported(tlas->instance))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "aprend_tlas_write_instances: " APREND_RT_UNSUPPORTED_MESSAGE);
	if (!instances)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instances is NULL");
	if (count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "count is 0");
	if ((uint64_t)first + count > tlas->max_instances)
		return aprend_fail(out_error, APRESULT_LIMIT, "first %u + count %u is past the TLAS's %u instances", first, count, tlas->max_instances);
	const uint32_t set = aprend_rt_tlas_preparing(tlas, out_error);
	if (set == UINT32_MAX)
		return APRESULT_LIMIT;
	// The BLAS table is read under the instance's release_mutex: a send on
	// another list may take destroyed BLASes into it meanwhile.
	std::lock_guard<std::mutex> table_lock(tlas->instance->release_mutex);
	aprend_rt_drain_locked(tlas->instance->rt);
	// Each record's slot names a live BLAS (D1, T6): two dense arrays read.
	const aprend_rt_state *rt = tlas->instance->rt;
	for (uint32_t i = 0; i < count; ++i) {
		const APREND_RAY_INSTANCE &r = instances[i];
		const uint32_t index         = aprend_rt_slot_index(r._blas);
		if (index == 0 || index >= rt->blas_live.size() || !rt->blas_live[index] || rt->blas_generation[index] != aprend_rt_slot_generation(r._blas))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instances[%u]'s _blas 0x%08x names no live BLAS", i, r._blas);
		if (r._material_base >= APREND_RT_MATERIAL_BASE_LIMIT)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instances[%u]'s _material_base %u is not below 2^24", i, r._material_base);
		if (r._mask > APREND_RT_MASK_LIMIT)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instances[%u]'s _mask 0x%x is above 255", i, r._mask);
		if (r._reserved != 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instances[%u]'s _reserved is not 0", i);
	}
	if (!aprend_rt_tlas_room(tlas))
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the TLAS's slot counts");
	// The records below the count leave their counts, are copied, and come
	// back in with their new slots.
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t at = first + i;
		if (at < tlas->instance_count)
			aprend_rt_count_record(tlas, at, -1);
		tlas->latest[at]       = instances[i];
		tlas->reaches_null[at] = aprend_rt_record_reaches_null(tlas, instances[i]);
#if _DEBUG
		tlas->debug_generations[at] = rt->blas_generation_debug[aprend_rt_slot_index(instances[i]._blas)];
#endif
		if (at < tlas->instance_count)
			aprend_rt_count_record(tlas, at, +1);
	}
	aprend_rt_set &prepared = tlas->sets[set];
	aprend_rt_mark_blocks(tlas->record_blocks + (size_t)set * tlas->record_words, prepared.records_mapped, tlas->latest, sizeof(APREND_RAY_INSTANCE),
	                      tlas->max_instances, first, count);
	return APRESULT_OK;
}

APRESULT aprend_tlas_set_instance_count(aprend_tlas tlas, uint32_t count, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is NULL");
	if (APRI_STALE(tlas, "aprend_tlas_set_instance_count", "tlas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is destroyed");
	APREND_ENTER_TLAS(tlas, "aprend_tlas_set_instance_count");
	if (!aprend_rt_supported(tlas->instance))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "aprend_tlas_set_instance_count: " APREND_RT_UNSUPPORTED_MESSAGE);
	if (count > tlas->max_instances)
		return aprend_fail(out_error, APRESULT_LIMIT, "count %u is past the TLAS's %u instances", count, tlas->max_instances);
	if (aprend_rt_tlas_preparing(tlas, out_error) == UINT32_MAX)
		return APRESULT_LIMIT;
	if (!aprend_rt_tlas_room(tlas))
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the TLAS's slot counts");
	// The records that come in or go out with the count.
	for (uint32_t i = count; i < tlas->instance_count; ++i)
		aprend_rt_count_record(tlas, i, -1);
	for (uint32_t i = tlas->instance_count; i < count; ++i)
		aprend_rt_count_record(tlas, i, +1);
	tlas->instance_count = count;
	return APRESULT_OK;
}

APRESULT aprend_tlas_set_materials(aprend_tlas tlas, uint32_t first, const aprend_material *materials, uint32_t count, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is NULL");
	if (APRI_STALE(tlas, "aprend_tlas_set_materials", "tlas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is destroyed");
	APREND_ENTER_TLAS(tlas, "aprend_tlas_set_materials");
	APREND_ENTER(tlas->instance, "aprend_tlas_set_materials");
	if (!aprend_rt_supported(tlas->instance))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "aprend_tlas_set_materials: " APREND_RT_UNSUPPORTED_MESSAGE);
	if (!materials)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "materials is NULL");
	if (count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "count is 0");
	if ((uint64_t)first + count > tlas->max_materials)
		return aprend_fail(out_error, APRESULT_LIMIT, "first %u + count %u is past the TLAS's %u material table entries", first, count, tlas->max_materials);
	const uint32_t set = aprend_rt_tlas_preparing(tlas, out_error);
	if (set == UINT32_MAX)
		return APRESULT_LIMIT;
	for (uint32_t i = 0; i < count; ++i) {
		aprend_material material = materials[i];
		if (!material)
			continue;
		if (APRI_STALE(material, "aprend_tlas_set_materials", "materials[i]"))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "materials[%u] is destroyed", i);
		if (material->instance != tlas->instance)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "materials[%u] is of another instance", i);
		if (material->rt_lit_slot == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "materials[%u] is not a lit material (aprend_lit_material_create)", i);
	}
	// The counts' room first: nothing is written without it.
	if (!aprend_rt_tlas_room(tlas))
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the TLAS's slot counts");
	// Each entry the material's slot in the lit-material table, read now;
	// whether any entry's NULL-ness changed, which T7's counts follow.
	bool nulls_changed = false;
	for (uint32_t i = 0; i < count; ++i) {
		const uint32_t slot = materials[i] ? materials[i]->rt_lit_slot : 0;
		if ((tlas->latest_materials[first + i] == 0) != (slot == 0))
			nulls_changed = true;
		tlas->latest_materials[first + i] = slot;
	}
	if (nulls_changed) {
		for (uint32_t i = 0; i < tlas->max_instances; ++i)
			tlas->reaches_null[i] = tlas->latest[i]._blas != 0 && aprend_rt_record_reaches_null(tlas, tlas->latest[i]);
		aprend_rt_recount(tlas);
	}
	aprend_rt_set &prepared = tlas->sets[set];
	aprend_rt_mark_blocks(tlas->material_blocks + (size_t)set * tlas->material_words, prepared.materials_mapped, tlas->latest_materials,
	                      sizeof(uint32_t), tlas->max_materials, first, count);
	return APRESULT_OK;
}

APRESULT aprend_tlas_get_info(aprend_tlas tlas, APREND_TLAS_INFO *out_info, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!tlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is NULL");
	if (APRI_STALE(tlas, "aprend_tlas_get_info", "tlas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "tlas is destroyed");
	APREND_ENTER_TLAS(tlas, "aprend_tlas_get_info");
	if (!out_info)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_info is NULL");
	if (const APRESULT base = aprend_base_out_check(out_error, "out_info", &out_info->base, APRI_STRUCT_TYPE_APREND_TLAS_INFO))
		return base;
	aprend_rt_tlas_settle(tlas);
	uint32_t in_use = 0;
	for (uint32_t s = 0; s < tlas->set_count; ++s)
		if (tlas->sets[s].state == APREND_RT_SET_SEALED || tlas->sets[s].state == APREND_RT_SET_SUBMITTED)
			++in_use;
	const uint64_t record_bytes   = (uint64_t)tlas->max_instances * sizeof(APREND_RAY_INSTANCE);
	const uint64_t material_bytes = (uint64_t)tlas->max_materials * sizeof(uint32_t);
	out_info->_storage_bytes      = tlas->structure_size + (record_bytes + material_bytes) * (tlas->set_count + 1) + tlas->device_instances->desc.size +
	                           tlas->emitters->desc.size;
	out_info->_sets_in_use = in_use;
	return APRESULT_OK;
}

} // extern "C"

/* ---- The seal, and the order of builds (T5, T6) ------------------------- */

bool aprend_rt_seal_set(aprend_command_list list, APREND_COMMAND *command) {
	aprend_tlas tlas = command->_params._build_tlas._tlas;
	APREND_ERROR error;
	const uint32_t set = aprend_rt_tlas_preparing(tlas, &error);
	if (set == UINT32_MAX) {
		aprend_cmd_report(list, APRESULT_LIMIT, "BUILD_TLAS: %s", error.message);
		return false;
	}
	aprend_rt_state *rt = tlas->instance->rt;
	// A send may overlap calls on other handles of the instance: the BLAS
	// table is read under its release_mutex.
	std::lock_guard<std::mutex> table_lock(tlas->instance->release_mutex);
	aprend_rt_drain_locked(rt);
	if (!aprend_rt_tlas_room(tlas)) {
		aprend_cmd_report(list, APRESULT_OUT_OF_MEMORY, "BUILD_TLAS: no memory for the TLAS's slot counts");
		return false;
	}
	// T6: every slot the set names within its count names a live BLAS, built
	// or sent for building earlier (the BUILD_BLAS's send marks it).
	aprend_rt_set &sealed = tlas->sets[set];
	uint32_t *levels      = tlas->bottom_levels + (size_t)set * tlas->max_instances;
	uint32_t level_count  = 0;
	for (size_t index = 1; index < tlas->slot_counts.size(); ++index) {
		aprend_rt_slot_count &count = tlas->slot_counts[index];
		if (count.current == 0 && count.other != 0) {
			aprend_rt_recount(tlas);
			break;
		}
	}
	for (size_t index = 1; index < tlas->slot_counts.size(); ++index) {
		const aprend_rt_slot_count &count = tlas->slot_counts[index];
		if (count.current == 0 && count.other == 0)
			continue;
		const uint32_t slot = aprend_rt_slot((uint32_t)index, count.generation);
		if (count.other != 0 || !rt->blas_live[index] || rt->blas_generation[index] != count.generation) {
			aprend_cmd_report(list, APRESULT_INVALID_ARGUMENT, "BUILD_TLAS: the set names BLAS slot 0x%08x, whose BLAS is destroyed", slot);
			return false;
		}
		if (!rt->blas_built[index]) {
			aprend_cmd_report(list, APRESULT_INVALID_ARGUMENT, "BUILD_TLAS: the set names BLAS slot 0x%08x, which is neither built nor built by a BUILD_BLAS earlier in the list",
			                  slot);
			return false;
		}
		levels[level_count++] = (uint32_t)index;
	}
#if _DEBUG
	// T6: an index reused 256 times since a record named it passes the 8-bit
	// check; its 32-bit generation names it.
	for (uint32_t i = 0; i < tlas->instance_count; ++i) {
		const uint32_t index = aprend_rt_slot_index(tlas->latest[i]._blas);
		if (rt->blas_generation_debug[index] != tlas->debug_generations[i]) {
			aprend_cmd_report(list, APRESULT_INVALID_ARGUMENT,
			                  "BUILD_TLAS: record %u names BLAS slot 0x%08x, whose BLAS was destroyed and its index reused 256 times since (the 8-bit generation wrapped)",
			                  i, tlas->latest[i]._blas);
			return false;
		}
	}
#endif
	sealed.state              = APREND_RT_SET_SEALED;
	sealed.instance_count     = tlas->instance_count;
	// D6: another count is another set of emitters.
	if (tlas->built_count != tlas->instance_count)
		++tlas->content_version;
	tlas->built_count         = tlas->instance_count;
	sealed.sequence           = tlas->next_sequence++;
	sealed.sealed_by          = list;
	sealed.bottom_level_count = level_count;
	tlas->preparing           = UINT32_MAX;
	tlas->sent_build          = true;
	// D6: a set with writes changes what the emitter table reads.
	bool written = false;
	for (uint32_t w = 0; w < tlas->record_words && !written; ++w)
		written = tlas->record_blocks[(size_t)set * tlas->record_words + w] != 0;
	for (uint32_t w = 0; w < tlas->material_words && !written; ++w)
		written = tlas->material_blocks[(size_t)set * tlas->material_words + w] != 0;
	if (written)
		++tlas->content_version;
	aprend_rt_seal seal{};
	seal.set         = set;
	seal.sequence    = sealed.sequence;
	seal.tlas_serial = tlas->serial;
	memcpy(command->_params._padding + APREND_RT_SEAL_OFFSET, &seal, sizeof(seal));
	return true;
}

// Whether [command]'s seal still holds its set, unsubmitted.
static bool aprend_rt_seal_held(const APREND_COMMAND &command, aprend_rt_seal *out_seal) {
	if (command._type != APREND_COMMAND_BUILD_TLAS)
		return false;
	const aprend_rt_seal seal = aprend_rt_seal_of(command);
	aprend_tlas tlas          = command._params._build_tlas._tlas;
	if (seal.sequence == 0 || tlas->serial != seal.tlas_serial)
		return false;
	const aprend_rt_set &set = tlas->sets[seal.set];
	if (set.state != APREND_RT_SET_SEALED || set.sequence != seal.sequence)
		return false;
	*out_seal = seal;
	return true;
}

void aprend_rt_abandon(aprend_command_list list) {
	aprend_instance instance = list->instance;
	if (!instance->rt)
		return;
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	for (const APREND_COMMAND &command : list->commands) {
		aprend_rt_seal seal{};
		if (command._type != APREND_COMMAND_BUILD_TLAS)
			continue;
		// A destroyed TLAS's seals go with it: it is read only while listed.
		const aprend_rt_seal kept = aprend_rt_seal_of(command);
		bool alive                = false;
		for (const aprend_rt_state::live_tlas &live : instance->rt->live_tlases)
			if (live.tlas == command._params._build_tlas._tlas && live.serial == kept.tlas_serial)
				alive = true;
		if (!alive)
			continue;
		if (!aprend_rt_seal_held(command, &seal))
			continue;
		aprend_tlas tlas      = command._params._build_tlas._tlas;
		aprend_rt_set &gone   = tlas->sets[seal.set];
		// The set sealed next after it, or the one being prepared: its
		// blocks are carried into that one where it didn't write its own.
		uint32_t next    = UINT32_MAX;
		uint64_t nearest = UINT64_MAX;
		for (uint32_t s = 0; s < tlas->set_count; ++s) {
			const aprend_rt_set &other = tlas->sets[s];
			if (other.state == APREND_RT_SET_SEALED && other.sequence > gone.sequence && other.sequence < nearest) {
				nearest = other.sequence;
				next    = s;
			}
		}
		if (next == UINT32_MAX)
			next = tlas->preparing;
		if (next == UINT32_MAX) {
			// Nothing after it: it is the set being prepared again.
			gone.state      = APREND_RT_SET_PREPARING;
			tlas->preparing = seal.set;
			continue;
		}
		aprend_rt_set &into = tlas->sets[next];
		const uint64_t *from_records = tlas->record_blocks + (size_t)seal.set * tlas->record_words;
		uint64_t *to_records         = tlas->record_blocks + (size_t)next * tlas->record_words;
		for (uint32_t b = 0; b < tlas->record_words * 64; ++b) {
			const bool had = (from_records[b / 64] >> (b % 64)) & 1;
			const bool has = (to_records[b / 64] >> (b % 64)) & 1;
			if (!had || has)
				continue;
			const uint32_t begin = b * APREND_RT_BLOCK;
			if (begin >= tlas->max_instances)
				break;
			const uint32_t end = begin + APREND_RT_BLOCK < tlas->max_instances ? begin + APREND_RT_BLOCK : tlas->max_instances;
			memcpy(into.records_mapped + begin, gone.records_mapped + begin, sizeof(APREND_RAY_INSTANCE) * (end - begin));
			to_records[b / 64] |= 1ull << (b % 64);
		}
		const uint64_t *from_materials = tlas->material_blocks + (size_t)seal.set * tlas->material_words;
		uint64_t *to_materials         = tlas->material_blocks + (size_t)next * tlas->material_words;
		for (uint32_t b = 0; b < tlas->material_words * 64; ++b) {
			const bool had = (from_materials[b / 64] >> (b % 64)) & 1;
			const bool has = (to_materials[b / 64] >> (b % 64)) & 1;
			if (!had || has)
				continue;
			const uint32_t begin = b * APREND_RT_BLOCK;
			if (begin >= tlas->max_materials)
				break;
			const uint32_t end = begin + APREND_RT_BLOCK < tlas->max_materials ? begin + APREND_RT_BLOCK : tlas->max_materials;
			memcpy(into.materials_mapped + begin, gone.materials_mapped + begin, sizeof(uint32_t) * (end - begin));
			to_materials[b / 64] |= 1ull << (b % 64);
		}
		gone.state = APREND_RT_SET_FREE;
	}
}

bool aprend_rt_submit_order_ok(aprend_command_list list) {
	for (const APREND_COMMAND &command : list->commands) {
		aprend_rt_seal seal{};
		if (!aprend_rt_seal_held(command, &seal))
			continue;
		aprend_tlas tlas = command._params._build_tlas._tlas;
		// An earlier seal of the same TLAS that another list holds, not yet
		// submitted, goes first (T5).
		for (uint32_t s = 0; s < tlas->set_count; ++s) {
			const aprend_rt_set &other = tlas->sets[s];
			if (other.state != APREND_RT_SET_SEALED || other.sequence >= seal.sequence || other.sealed_by == list)
				continue;
			// Both lists by name, or by address where they have none.
			const aprend_command_list_t *holder = (const aprend_command_list_t *)other.sealed_by;
			char this_name[96];
			char that_name[96];
			if (list->debug_name)
				snprintf(this_name, sizeof(this_name), "list \"%s\"", list->debug_name);
			else
				snprintf(this_name, sizeof(this_name), "list %p", (const void *)list);
			if (holder->debug_name)
				snprintf(that_name, sizeof(that_name), "list \"%s\"", holder->debug_name);
			else
				snprintf(that_name, sizeof(that_name), "list %p", (const void *)holder);
			aprend_cmd_report(list, APRESULT_INVALID_ARGUMENT,
			                  "submit: %s's BUILD_TLAS of TLAS \"%s\" was sent after %s's, which is not yet submitted: builds of one TLAS are submitted in the order they were sent",
			                  this_name, tlas->debug_name ? tlas->debug_name : "", that_name);
			return false;
		}
	}
	return true;
}

void aprend_rt_submitted(aprend_command_list list, uint64_t serial) {
	aprend_instance instance = list->instance;
	for (const APREND_COMMAND &command : list->commands) {
		if (command._type == APREND_COMMAND_BUILD_BLAS) {
			// Built (or refitted) once this submission completes; what a
			// compaction or a rebuild gave up goes then too.
			aprend_blas blas = command._params._build_blas._blas;
			if (command._params._build_blas._mode != APREND_BUILD_MODE_COMPACT) {
				blas->built_serial = serial;
				blas->built_frame  = instance->frame_number;
			}
			if (blas->retiring && blas->retiring_serial == 0) {
				blas->retiring_serial = serial;
				blas->retiring_frame  = instance->frame_number;
			}
			continue;
		}
		aprend_rt_seal seal{};
		if (!aprend_rt_seal_held(command, &seal))
			continue;
		aprend_rt_set &set = command._params._build_tlas._tlas->sets[seal.set];
		set.state          = APREND_RT_SET_SUBMITTED;
		set.serial         = serial;
	}
	// The emitter tables it built are current now (D6).
	for (const APREND_COMMAND &command : list->commands) {
		if (command._type != APREND_COMMAND_LIT_PATH_TRACE)
			continue;
		aprend_tlas tlas = command._params._lit_path_trace._tlas;
		if (tlas->emitters_pending_list != list)
			continue;
		tlas->emitters_content      = tlas->emitters_pending_content;
		tlas->emitters_lit          = tlas->emitters_pending_lit;
		tlas->emitters_pending_list = nullptr;
	}
	// The lit-material entries staged since the last copy are copied by it.
	if (list->rt_reads_lit) {
		aprend_rt_state *rt = instance->rt;
		for (uint32_t index : rt->lit_dirty_list)
			rt->lit_dirty[index] = 0;
		rt->lit_dirty_list.clear();
	}
}

/* ---- The commands' sends (aprend_command_list_send) --------------------- */

// Whether [view] is an [type] 2D view of one mip.
static bool aprend_rt_view_is(const aprend_texture_view_t *view, APREND_TEXTURE_VIEW_TYPE type) {
	return view->view_type == type && view->desc.shape == APREND_TEXTURE_VIEW_SHAPE_2D && view->desc.mip_count == 1;
}

// [view]'s size at its mip.
static void aprend_rt_view_size(const aprend_texture_view_t *view, uint32_t *out_width, uint32_t *out_height) {
	*out_width  = aprend_mip_size(view->texture._t2d->desc.width, view->desc.base_mip);
	*out_height = aprend_mip_size(view->texture._t2d->desc.height, view->desc.base_mip);
}

bool aprend_rt_send_checks(aprend_command_list list, const APREND_COMMAND &command) {
	aprend_instance instance = list->instance;
	const char *fault        = nullptr;
	switch (command._type) {
	case APREND_COMMAND_BUILD_BLAS: {
		const auto &p = command._params._build_blas;
		if (!p._blas)
			fault = "a NULL BLAS";
		else if (p._blas->instance != instance)
			fault = "a BLAS of another instance";
		else if (p._mode != APREND_BUILD_MODE_BUILD && p._mode != APREND_BUILD_MODE_REFIT && p._mode != APREND_BUILD_MODE_COMPACT)
			fault = "a _mode that is not an APREND_BUILD_MODE";
		else if (p._mode == APREND_BUILD_MODE_REFIT && !p._blas->deforms)
			fault = "APREND_BUILD_MODE_REFIT of a BLAS that does not deform";
		else if (p._mode == APREND_BUILD_MODE_COMPACT && p._blas->deforms)
			fault = "APREND_BUILD_MODE_COMPACT of a BLAS that deforms";
		break;
	}
	case APREND_COMMAND_BUILD_TLAS: {
		const auto &p = command._params._build_tlas;
		if (!p._tlas)
			fault = "a NULL TLAS";
		else if (p._tlas->instance != instance)
			fault = "a TLAS of another instance";
		break;
	}
	case APREND_COMMAND_LIT_SHADOW_MASK: {
		const auto &p = command._params._lit_shadow_mask;
		if (!p._scene || !p._tlas || !p._depth || !p._target) {
			fault = "a NULL scene, TLAS, depth or target";
			break;
		}
		if (p._scene->instance != instance || p._tlas->instance != instance || p._depth->instance != instance || p._target->instance != instance) {
			fault = "a scene, TLAS, depth or target of another instance";
			break;
		}
		// Today the scene's one directional light, the sun (card A35).
		if (p._light != 0) {
			aprend_cmd_report(list, APRESULT_INVALID_ARGUMENT,
			                  "LIT_SHADOW_MASK with a _light of %u, which names no directional light of the scene (only light 0, the sun, until card A35)",
			                  p._light);
			return false;
		}
		if (!aprend_rt_view_is(p._depth, APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) || p._depth->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_DEPTH) {
			fault = "a depth that is not a SHADER_RESOURCE 2D view of depth";
			break;
		}
		const SPUDGPU_FORMAT format = p._target->texture._t2d->desc.format;
		if (!aprend_rt_view_is(p._target, APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS) || (format != SPUDGPU_FORMAT_R8_UNORM && format != SPUDGPU_FORMAT_R16_FLOAT)) {
			fault = "a target that is not an UNORDERED_ACCESS 2D view of R8_UNORM or R16_FLOAT";
			break;
		}
		uint32_t dw = 0, dh = 0, tw = 0, th = 0;
		aprend_rt_view_size(p._depth, &dw, &dh);
		aprend_rt_view_size(p._target, &tw, &th);
		if (dw != tw || dh != th)
			fault = "a depth and a target of different sizes";
		else if (p._ray_count == 0)
			fault = "a _ray_count of 0";
		else if (p._filter_radius > APREND_RT_MAX_FILTER_RADIUS)
			fault = "a _filter_radius past 8";
		else if (p._cull_mask == 0 || p._cull_mask > APREND_RT_MASK_LIMIT)
			fault = "a _cull_mask outside 1 to 255";
		else if (!(std::isfinite(p._normal_offset) && p._normal_offset >= 0.0f))
			fault = "a _normal_offset that is negative or not a number";
		else if (!(std::isfinite(p._max_distance) && p._max_distance > 0.0f))
			fault = "a _max_distance that is not above 0, or not a number";
		break;
	}
	case APREND_COMMAND_LIT_PATH_TRACE: {
		const auto &p = command._params._lit_path_trace;
		if (!p._scene || !p._tlas || !p._target) {
			fault = "a NULL scene, TLAS or target";
			break;
		}
		if (p._scene->instance != instance || p._tlas->instance != instance || p._target->instance != instance ||
		    (p._albedo && p._albedo->instance != instance) || (p._normal && p._normal->instance != instance)) {
			fault = "a scene, TLAS, target or guide of another instance";
			break;
		}
		if (!aprend_rt_view_is(p._target, APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS) ||
		    p._target->texture._t2d->desc.format != SPUDGPU_FORMAT_R32G32B32A32_FLOAT) {
			fault = "a target that is not an UNORDERED_ACCESS 2D view of R32G32B32A32_FLOAT";
			break;
		}
		uint32_t tw = 0, th = 0;
		aprend_rt_view_size(p._target, &tw, &th);
		const aprend_texture_view_t *guides[2] = {p._albedo, p._normal};
		for (const aprend_texture_view_t *guide : guides) {
			if (!guide || fault)
				continue;
			const SPUDGPU_FORMAT format = guide->texture._t2d->desc.format;
			uint32_t gw = 0, gh = 0;
			if (!aprend_rt_view_is(guide, APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS) ||
			    (format != SPUDGPU_FORMAT_R32G32B32A32_FLOAT && format != SPUDGPU_FORMAT_R16G16B16A16_FLOAT)) {
				fault = "an _albedo or _normal that is not an UNORDERED_ACCESS 2D view of R32G32B32A32_FLOAT or R16G16B16A16_FLOAT";
				continue;
			}
			aprend_rt_view_size(guide, &gw, &gh);
			if (gw != tw || gh != th)
				fault = "a guide of another size than the target";
		}
		if (fault)
			break;
		if (p._samples == 0)
			fault = "_samples of 0";
		else if (p._max_bounces == 0)
			fault = "_max_bounces of 0";
		else if (p._cull_mask == 0 || p._cull_mask > APREND_RT_MASK_LIMIT)
			fault = "a _cull_mask outside 1 to 255";
		else if (!(std::isfinite(p._clamp) && p._clamp >= 0.0f))
			fault = "a _clamp that is negative or not a number";
		else if (p._accumulated > UINT32_MAX - p._samples)
			fault = "an _accumulated + _samples that overflows";
		break;
	}
	default:
		return true;
	}
	if (fault) {
		aprend_cmd_report(list, APRESULT_INVALID_ARGUMENT, "%s with %s", aprend_rt_command_name(command._type), fault);
		return false;
	}
	return true;
}

bool aprend_rt_send_commit(aprend_command_list list, APREND_COMMAND *command) {
	if (command->_type == APREND_COMMAND_BUILD_TLAS)
		return aprend_rt_seal_set(list, command);
	if (command->_type == APREND_COMMAND_BUILD_BLAS && command->_params._build_blas._mode == APREND_BUILD_MODE_BUILD) {
		// T6: a set sent after this in the list may name it.
		aprend_blas blas = command->_params._build_blas._blas;
		std::lock_guard<std::mutex> lock(blas->instance->release_mutex);
		blas->instance->rt->blas_built[aprend_rt_slot_index(blas->slot)] = 1;
	}
	return true;
}

/* ---- What the builds' compile reads (aprendcommands.cpp) --------------- */

#include "aprend_rt_instances_row64_comp.package.inc"
#include "aprend_rt_instances_col72_comp.package.inc"

aprend_compute_pipeline aprend_rt_writer_pipeline(aprend_instance instance) {
	aprend_rt_state *rt = instance->rt;
	if (rt->writer_pipeline)
		return rt->writer_pipeline;
	// One writer per SpudGPU instance layout kind (card S14), chosen once.
	const bool column = rt->instance_layout.kind == SPUDGPU_ACCELERATION_STRUCTURE_INSTANCE_LAYOUT_COLUMN_MAJOR_72;
	aprend_shader &shader = column ? rt->writer_col72 : rt->writer_row64;
	if (!shader) {
		(void)aprend_shader_create_own(instance, column ? &aprend_rt_instances_col72_comp_package : &aprend_rt_instances_row64_comp_package, &shader, nullptr);
		if (!shader) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's instance writer can't be created");
			return nullptr;
		}
	}
	if (!rt->writer_layout) {
		(void)aprend_binding_layout_create_from_shaders(instance, &shader, 1, 0, &rt->writer_layout, nullptr);
		if (!rt->writer_layout) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the layout of Aprend's instance writer can't be created");
			return nullptr;
		}
	}
	APREND_COMPUTE_PIPELINE_DESC desc{};
	desc.base                  = apri_struct_base(APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC);
	desc._debug_name           = "aprend instance writer";
	desc.compute_shader        = shader;
	desc._entry_point          = column ? "aprend_rt_instances_col72_comp" : "aprend_rt_instances_row64_comp";
	desc._binding_layouts[0]   = rt->writer_layout;
	desc._binding_layout_count = 1;
	desc._push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE;
	desc._push_constant_ranges[0].offset      = 0;
	desc._push_constant_ranges[0].size        = sizeof(aprend_rt_writer_params);
	desc._push_constant_range_count           = 1;
	APREND_ERROR error;
	(void)aprend_compute_pipeline_create(instance, &desc, &rt->writer_pipeline, &error);
	if (!rt->writer_pipeline)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's instance writer pipeline can't be created: %s", error.message);
	return rt->writer_pipeline;
}

// A storage entry of [buffer] at [binding].
static APREND_BINDING_SET_ENTRY aprend_rt_storage_entry(uint32_t binding, aprend_buffer buffer) {
	APREND_BINDING_SET_ENTRY entry{};
	entry._binding                    = binding;
	entry._type                       = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	entry._resource._storage._buffer  = buffer;
	return entry;
}

// Makes [*set] again if it was made against older tables, or not at all, of
// [layout] from [entries]. False, reported, if it can't be.
static bool aprend_rt_keep_set(aprend_tlas tlas, aprend_binding_set *set, uint64_t *version, aprend_binding_layout layout, const APREND_BINDING_SET_ENTRY *entries,
                               uint32_t count, spudgpu_acceleration_structure top_level, const char *what) {
	aprend_rt_state *rt = tlas->instance->rt;
	if (*set && *version == rt->buffers_version)
		return true;
	aprend_binding_set made = nullptr;
	APREND_ERROR error;
	const APRESULT result = top_level ? aprend_binding_set_create_traced(layout, entries, count, top_level, &made, &error)
	                                  : aprend_binding_set_create(layout, entries, count, &made, &error);
	if (result != APRESULT_OK) {
		aprend_report(tlas->instance, result, "%s's set can't be made: %s", what, error.message);
		return false;
	}
	aprend_binding_set_destroy(*set);
	*set     = made;
	*version = rt->buffers_version;
	return true;
}

aprend_binding_set aprend_rt_tlas_writer_set(aprend_tlas tlas) {
	aprend_rt_state *rt = tlas->instance->rt;
	const APREND_BINDING_SET_ENTRY entries[3] = {aprend_rt_storage_entry(0, tlas->records), aprend_rt_storage_entry(1, rt->blas_table),
	                                             aprend_rt_storage_entry(2, tlas->device_instances)};
	if (!aprend_rt_keep_set(tlas, &tlas->writer_set, &tlas->writer_set_version, rt->writer_layout, entries, 3, nullptr, "the instance writer"))
		return nullptr;
	return tlas->writer_set;
}

bool aprend_rt_prepare_scratch(aprend_instance instance, uint64_t size) {
	aprend_rt_state *rt = instance->rt;
	if (rt->scratch && rt->scratch->desc.size >= size)
		return true;
	// Grown to the largest a compiled list needs (T8): an allocation at that
	// compile and only then, before anything of the list is recorded, so the
	// buffer it replaces is used by no part of it.
	APREND_ERROR error;
	aprend_buffer grown = aprend_rt_make_buffer(instance, size, 0, "aprend build scratch", &error);
	if (!grown) {
		aprend_report(instance, APRESULT_OUT_OF_MEMORY, "the builds' scratch of %llu bytes can't be made: %s", (unsigned long long)size, error.message);
		return false;
	}
	aprend_buffer_destroy(rt->scratch);
	rt->scratch = grown;
	return true;
}

bool aprend_rt_blas_retire_pending(aprend_blas blas) {
	if (!blas->retiring)
		return true;
	// Given up by a compaction or a rebuild whose frame hasn't completed:
	// released once everything submitted so far has.
	if (aprend_instance_reserve_release(blas->instance, 1) != APRESULT_OK) {
		aprend_report(blas->instance, APRESULT_OUT_OF_MEMORY, "BUILD_BLAS: no memory to release the BLAS's earlier structure");
		return false;
	}
	aprend_instance_retire(blas->instance, blas->retiring, &aprend_release_spudgpu_acceleration_structure);
	blas->retiring        = nullptr;
	blas->retiring_serial = 0;
	return true;
}

bool aprend_rt_blas_make_compact(aprend_blas blas) {
	spudgpu_acceleration_structure_desc ad{};
	ad.base.type  = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	ad.level      = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	ad.size       = blas->compacted_size;
	ad.debug_name = blas->debug_name;
	const SPUDRESULT sr = spudgpu_create_acceleration_structure(blas->instance->desc.device, &ad, &blas->compact);
	if (SPUDFAIL(sr)) {
		blas->compact = nullptr;
		aprend_report(blas->instance, aprend_result_from_spud(sr), "BUILD_BLAS: the compacted structure of %llu bytes can't be made", (unsigned long long)blas->compacted_size);
		return false;
	}
	return true;
}

bool aprend_rt_blas_make_full(aprend_blas blas) {
	spudgpu_acceleration_structure_desc ad{};
	ad.base.type        = SPUD_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_DESC;
	ad.level            = SPUDGPU_ACCELERATION_STRUCTURE_LEVEL_BOTTOM;
	ad.size             = blas->full_size;
	ad.allow_compaction = !blas->deforms;
	ad.debug_name       = blas->debug_name;
	const SPUDRESULT sr = spudgpu_create_acceleration_structure(blas->instance->desc.device, &ad, &blas->full);
	if (SPUDFAIL(sr)) {
		blas->full = nullptr;
		aprend_report(blas->instance, aprend_result_from_spud(sr), "BUILD_BLAS: the BLAS's full-size structure of %llu bytes can't be made", (unsigned long long)blas->full_size);
		return false;
	}
	return true;
}

bool aprend_rt_stage_lit(aprend_instance instance, eq::array<aprend_upload_copy> &copies) {
	aprend_rt_state *rt = instance->rt;
	aprend_buffer_store *table = &rt->lit_table->parts[0].store;
	for (uint32_t index : rt->lit_dirty_list) {
		// The entry as it is now, in this frame's upload memory, which
		// outlives the submission's reading of it.
		APREND_TRANSIENT_SPAN span{};
		if (aprend_transient_allocate(instance, sizeof(aprend_rt_lit_entry), 16, &span, nullptr) != APRESULT_OK) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "submit: transient memory can't be taken for the lit-material table");
			return false;
		}
		memcpy(span.data, &rt->lit_entries[index], sizeof(aprend_rt_lit_entry));
		const aprend_transient_chunk *chunk = (const aprend_transient_chunk *)span._chunk;
		aprend_push(copies, aprend_upload_copy{chunk->buffer, span._offset, table->buffer, (uint64_t)index * sizeof(aprend_rt_lit_entry),
		                                       sizeof(aprend_rt_lit_entry), &rt->lit_copied, table->use_state});
	}
	return true;
}

/* ---- The traces' pipelines and sets (LIT_SHADOW_MASK, LIT_PATH_TRACE) ---- */

#include "aprend_rt_shadow_mask_r8_comp.package.inc"
#include "aprend_rt_shadow_mask_r16f_comp.package.inc"

// The layout of set [index] of [shader], made into [*layout] if it isn't.
// False, reported naming [what], if it can't be.
static bool aprend_rt_layout(aprend_instance instance, aprend_shader shader, uint32_t index, aprend_binding_layout *layout, const char *what) {
	if (*layout)
		return true;
	APREND_ERROR error;
	(void)aprend_binding_layout_create_from_shaders(instance, &shader, 1, index, layout, &error);
	if (!*layout) {
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the layout of %s can't be created: %s", what, error.message);
		return false;
	}
	return true;
}

// A traced compute pipeline of [shader] at [entry], sets 0 to 2 [layouts],
// [push] bytes of push constants. NULL, reported, if it can't be made.
static aprend_compute_pipeline aprend_rt_traced_pipeline(aprend_instance instance, aprend_shader shader, const char *entry, const aprend_binding_layout *layouts,
                                                         uint32_t push, const char *what) {
	APREND_COMPUTE_PIPELINE_DESC desc{};
	desc.base                  = apri_struct_base(APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC);
	desc._debug_name           = what;
	desc.compute_shader        = shader;
	desc._entry_point          = entry;
	desc._binding_layouts[0]   = layouts[0];
	desc._binding_layouts[1]   = layouts[1];
	desc._binding_layouts[2]   = layouts[2];
	desc._binding_layout_count = 3;
	desc._push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE;
	desc._push_constant_ranges[0].offset      = 0;
	desc._push_constant_ranges[0].size        = push;
	desc._push_constant_range_count           = 1;
	aprend_compute_pipeline pipeline          = nullptr;
	APREND_ERROR error;
	(void)aprend_compute_pipeline_create(instance, &desc, &pipeline, &error);
	if (!pipeline)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "%s can't be created: %s", what, error.message);
	return pipeline;
}

aprend_compute_pipeline aprend_rt_mask_pipeline(aprend_instance instance, SPUDGPU_FORMAT format) {
	aprend_rt_state *rt = instance->rt;
	const uint32_t kind = format == SPUDGPU_FORMAT_R16_FLOAT ? 1u : 0u;
	if (rt->mask_pipelines[kind])
		return rt->mask_pipelines[kind];
	static const APREND_SHADER_PACKAGE *const packages[2] = {&aprend_rt_shadow_mask_r8_comp_package, &aprend_rt_shadow_mask_r16f_comp_package};
	static const char *const entries[2]                   = {"aprend_rt_shadow_mask_r8_comp", "aprend_rt_shadow_mask_r16f_comp"};
	if (!rt->mask_comps[kind]) {
		APREND_ERROR error;
		(void)aprend_shader_create_own(instance, packages[kind], &rt->mask_comps[kind], &error);
		if (!rt->mask_comps[kind]) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's shadow mask shader can't be created: %s", error.message);
			return nullptr;
		}
	}
	aprend_shader shader = rt->mask_comps[kind];
	if (!aprend_rt_trace_layouts(instance) || !aprend_rt_layout(instance, shader, 2, &rt->mask_output_layout, "the shadow mask's output set"))
		return nullptr;
	const aprend_binding_layout layouts[3] = {rt->scene_layout, rt->trace_layout, rt->mask_output_layout};
	rt->mask_pipelines[kind] = aprend_rt_traced_pipeline(instance, shader, entries[kind], layouts, sizeof(aprend_rt_mask_params), "Aprend's shadow mask pipeline");
	return rt->mask_pipelines[kind];
}

aprend_binding_set aprend_rt_tlas_trace_set(aprend_tlas tlas) {
	aprend_rt_state *rt               = tlas->instance->rt;
	const aprend_binding_layout_t *layout = rt->trace_layout;
	// Every binding the layout has, from the shaders that read it: the TLAS
	// at 0, its tables at 1 to 6.
	const aprend_buffer buffers[7] = {nullptr, tlas->records, tlas->materials, rt->blas_table, rt->arena, rt->lit_table, tlas->emitters};
	APREND_BINDING_SET_ENTRY entries[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	uint32_t count = 0;
	for (uint32_t i = 0; i < layout->binding_count; ++i) {
		const APREND_BINDING_DESC &binding = layout->bindings[i];
		APREND_BINDING_SET_ENTRY &entry    = entries[count++];
		entry._binding                     = binding._binding;
		entry._type                        = binding._type;
		if (binding._type == SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER && binding._binding < 7)
			entry._resource._storage._buffer = buffers[binding._binding];
	}
	if (!aprend_rt_keep_set(tlas, &tlas->trace_set, &tlas->trace_set_version, rt->trace_layout, entries, count, tlas->structure, "the traces' TLAS set"))
		return nullptr;
	return tlas->trace_set;
}

aprend_binding_set aprend_rt_output_set(aprend_texture_view target, aprend_binding_layout layout, const aprend_texture_view *others, const APREND_BINDING_SET_ENTRY *entries,
                                        uint32_t count) {
	// Kept by the target, made again only when what it holds besides the
	// target changes (a view destroyed and another at its address has
	// another serial).
	bool same = target->rt_set != nullptr && target->rt_set_layout == layout;
	for (uint32_t i = 0; i < 2 && same; ++i) {
		const aprend_texture_view other = others[i];
		same = target->rt_set_views[i] == other && target->rt_set_serials[i] == (other ? other->serial : 0);
	}
	if (same)
		return target->rt_set;
	aprend_binding_set made = nullptr;
	APREND_ERROR error;
	if (aprend_binding_set_create(layout, entries, count, &made, &error) != APRESULT_OK) {
		aprend_report(target->instance, APRESULT_APREND_GPU_FAILURE, "a trace's output set can't be made: %s", error.message);
		return nullptr;
	}
	aprend_binding_set_destroy(target->rt_set);
	target->rt_set        = made;
	target->rt_set_layout = layout;
	for (uint32_t i = 0; i < 2; ++i) {
		target->rt_set_views[i]   = others[i];
		target->rt_set_serials[i] = others[i] ? others[i]->serial : 0;
	}
	return made;
}

/* ---- The path tracer and its emitter table (LIT_PATH_TRACE) ---------------- */

#include "aprend_rt_emitters_comp.package.inc"
#include "aprend_rt_path_trace_f32_f32_comp.package.inc"
#include "aprend_rt_path_trace_f32_f16_comp.package.inc"
#include "aprend_rt_path_trace_f16_f32_comp.package.inc"
#include "aprend_rt_path_trace_f16_f16_comp.package.inc"

// The path tracer's variant of [index] (bit 1 an albedo guide of
// R16G16B16A16_FLOAT, bit 0 a normal guide of it), made if it isn't.
static aprend_shader aprend_rt_path_shader(aprend_instance instance, uint32_t index) {
	aprend_rt_state *rt = instance->rt;
	if (rt->path_comps[index])
		return rt->path_comps[index];
	static const APREND_SHADER_PACKAGE *const packages[4] = {&aprend_rt_path_trace_f32_f32_comp_package, &aprend_rt_path_trace_f32_f16_comp_package,
	                                                         &aprend_rt_path_trace_f16_f32_comp_package, &aprend_rt_path_trace_f16_f16_comp_package};
	APREND_ERROR error;
	(void)aprend_shader_create_own(instance, packages[index], &rt->path_comps[index], &error);
	if (!rt->path_comps[index])
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's path tracer can't be created: %s", error.message);
	return rt->path_comps[index];
}

bool aprend_rt_trace_layouts(aprend_instance instance) {
	aprend_rt_state *rt = instance->rt;
	if (rt->scene_layout && rt->trace_layout && rt->path_output_layout)
		return true;
	// From the path tracer, which reads every binding of set 1; the mask
	// declares the same set and reads its first.
	aprend_shader shader = aprend_rt_path_shader(instance, 0);
	return shader && aprend_rt_layout(instance, shader, 0, &rt->scene_layout, "the traces' scene set") &&
	       aprend_rt_layout(instance, shader, 1, &rt->trace_layout, "the traces' TLAS set") &&
	       aprend_rt_layout(instance, shader, 2, &rt->path_output_layout, "the path tracer's output set");
}

aprend_compute_pipeline aprend_rt_path_pipeline(aprend_instance instance, SPUDGPU_FORMAT albedo, SPUDGPU_FORMAT normal) {
	aprend_rt_state *rt  = instance->rt;
	const uint32_t index = (albedo == SPUDGPU_FORMAT_R16G16B16A16_FLOAT ? 2u : 0u) | (normal == SPUDGPU_FORMAT_R16G16B16A16_FLOAT ? 1u : 0u);
	if (rt->path_pipelines[index])
		return rt->path_pipelines[index];
	static const char *const entries[4] = {"aprend_rt_path_trace_f32_f32_comp", "aprend_rt_path_trace_f32_f16_comp", "aprend_rt_path_trace_f16_f32_comp",
	                                       "aprend_rt_path_trace_f16_f16_comp"};
	aprend_shader shader = aprend_rt_path_shader(instance, index);
	if (!shader || !aprend_rt_trace_layouts(instance))
		return nullptr;
	const aprend_binding_layout layouts[3] = {rt->scene_layout, rt->trace_layout, rt->path_output_layout};
	rt->path_pipelines[index] = aprend_rt_traced_pipeline(instance, shader, entries[index], layouts, sizeof(aprend_rt_trace_params), "Aprend's path tracer pipeline");
	return rt->path_pipelines[index];
}

aprend_compute_pipeline aprend_rt_emitter_pipeline(aprend_instance instance) {
	aprend_rt_state *rt = instance->rt;
	if (rt->emitter_pipeline)
		return rt->emitter_pipeline;
	if (!rt->emitter_comp) {
		APREND_ERROR error;
		(void)aprend_shader_create_own(instance, &aprend_rt_emitters_comp_package, &rt->emitter_comp, &error);
		if (!rt->emitter_comp) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's emitter pass can't be created: %s", error.message);
			return nullptr;
		}
	}
	if (!aprend_rt_layout(instance, rt->emitter_comp, 0, &rt->emitter_layout, "the emitter pass's set"))
		return nullptr;
	APREND_COMPUTE_PIPELINE_DESC desc{};
	desc.base                                 = apri_struct_base(APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC);
	desc._debug_name                          = "aprend emitter table";
	desc.compute_shader                       = rt->emitter_comp;
	desc._entry_point                         = "aprend_rt_emitters_comp";
	desc._binding_layouts[0]                  = rt->emitter_layout;
	desc._binding_layout_count                = 1;
	desc._push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE;
	desc._push_constant_ranges[0].offset      = 0;
	desc._push_constant_ranges[0].size        = sizeof(aprend_rt_emitter_params);
	desc._push_constant_range_count           = 1;
	APREND_ERROR error;
	(void)aprend_compute_pipeline_create(instance, &desc, &rt->emitter_pipeline, &error);
	if (!rt->emitter_pipeline)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's emitter pass pipeline can't be created: %s", error.message);
	return rt->emitter_pipeline;
}

aprend_binding_set aprend_rt_tlas_emitter_set(aprend_tlas tlas) {
	aprend_rt_state *rt = tlas->instance->rt;
	const APREND_BINDING_SET_ENTRY entries[6] = {aprend_rt_storage_entry(0, tlas->records), aprend_rt_storage_entry(1, tlas->materials),
	                                             aprend_rt_storage_entry(2, rt->blas_table),  aprend_rt_storage_entry(3, rt->arena),
	                                             aprend_rt_storage_entry(4, rt->lit_table),   aprend_rt_storage_entry(5, tlas->emitters)};
	if (!aprend_rt_keep_set(tlas, &tlas->emitter_set, &tlas->emitter_set_version, rt->emitter_layout, entries, 6, nullptr, "the emitter pass"))
		return nullptr;
	return tlas->emitter_set;
}

aprend_texture_view aprend_rt_no_guide(aprend_instance instance) {
	aprend_rt_state *rt = instance->rt;
	if (rt->no_guide_view)
		return rt->no_guide_view;
	APREND_TEXTURE2D_DESC td{};
	td.base         = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC);
	td.width        = 1;
	td.height       = 1;
	td.format       = SPUDGPU_FORMAT_R32G32B32A32_FLOAT;
	td.usage        = APREND_TEXTURE_USAGE_BIT_STORAGE;
	td.mip_levels   = 1;
	td.array_layers = 1;
	td.sample_count = 1;
	td.debug_name   = "aprend absent guide";
	APREND_ERROR error;
	if (aprend_texture2d_create(instance, &td, &rt->no_guide, &error) != APRESULT_OK) {
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the path tracer's stand-in for an absent guide can't be made: %s", error.message);
		return nullptr;
	}
	APREND_TEXTURE_VIEW_DESC vd{};
	vd.base        = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
	vd.type        = APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS;
	vd.shape       = APREND_TEXTURE_VIEW_SHAPE_2D;
	vd.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
	vd.mip_count   = 1;
	vd.layer_count = 1;
	if (aprend_texture_view_create_2d(rt->no_guide, &vd, &rt->no_guide_view, &error) != APRESULT_OK) {
		aprend_texture2d_destroy(rt->no_guide);
		rt->no_guide = nullptr;
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the path tracer's stand-in for an absent guide can't be viewed: %s", error.message);
		return nullptr;
	}
	return rt->no_guide_view;
}

uint32_t aprend_rt_null_instance(aprend_tlas tlas, uint32_t cull_mask) {
	// T7's counts say whether one exists; the record is found only to name
	// it.
	bool any = false;
	for (uint32_t bit = 0; bit < 8; ++bit)
		if ((cull_mask & (1u << bit)) && tlas->null_by_bit[bit] > 0)
			any = true;
	if (!any)
		return UINT32_MAX;
	for (uint32_t i = 0; i < tlas->instance_count; ++i)
		if (tlas->reaches_null[i] && (tlas->latest[i]._mask & cull_mask))
			return i;
	return UINT32_MAX;
}
