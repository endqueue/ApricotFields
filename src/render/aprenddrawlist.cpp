
#include "render/aprenddrawlist.h"
#include "aprend_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// Whether draws [a] and [b] are of one mesh and one material, and can be one
// instanced draw. The meshes were stored with their unused slots zeroed.
static bool aprend_draw_same_batch(const aprend_draw_list_t::draw &a, const aprend_draw_list_t::draw &b) {
	return a.material == b.material && memcmp(&a.mesh, &b.mesh, sizeof(APREND_DRAW_MESH)) == 0;
}

// The order of APREND_DRAW_LIST_ORDER_BATCHED: by material, then by mesh.
// Neither means anything to the GPU; the point is that equal draws end up
// next to each other, and draws of one material together, in an order that
// is the same on every run. A key's handles are the first draws naming them
// (aprend_draw_list_t::sort_key), so no address decides it.
static bool aprend_draw_key_less(const aprend_draw_list_t::sort_key &a, const aprend_draw_list_t::sort_key &b) {
	if (a.material != b.material)
		return a.material < b.material;
	if (a.vertex_buffers != b.vertex_buffers)
		return a.vertex_buffers < b.vertex_buffers;
	if (a.index_buffer != b.index_buffer)
		return a.index_buffer < b.index_buffer;
	if (a.first != b.first)
		return a.first < b.first;
	if (a.count != b.count)
		return a.count < b.count;
	if (a.base_vertex != b.base_vertex)
		return a.base_vertex < b.base_vertex;
	return a.draw < b.draw;
}

// 64 bits of a handle, for a table that finds its first draw. Only finds:
// the hash orders nothing.
static uint64_t aprend_draw_hash(const void *handle) {
	return (uint64_t)(uintptr_t)handle;
}

static uint64_t aprend_draw_hash_vertex_buffers(const APREND_DRAW_MESH &mesh) {
	uint64_t hash = mesh._vertex_buffer_count;
	for (uint32_t i = 0; i < mesh._vertex_buffer_count; ++i)
		hash = (hash ^ (uint64_t)(uintptr_t)mesh._vertex_buffers[i]) * 0x100000001B3ull;
	return hash;
}

// The index of the first draw before [index] in [table] that [same] says
// names the same handle as draw [index], adding [index] if none does.
// Throws std::bad_alloc.
template <typename Same>
static uint32_t aprend_draw_first(apri_hash_index &table, uint64_t hash, uint32_t index, Same &&same) {
	if (const uint32_t *found = table.find(hash, same))
		return *found;
	table.add(hash, index);
	return index;
}

// Fills [list]'s sort keys, one a draw, and sorts them. Throws
// std::bad_alloc.
static void aprend_draw_sort_batched(aprend_draw_list list) {
	const std::vector<aprend_draw_list_t::draw> &draws = list->draws;
	list->first_material.clear();
	list->first_vertex_buffers.clear();
	list->first_index_buffer.clear();
	list->sort_keys.resize(draws.size());
	for (uint32_t i = 0; i < (uint32_t)draws.size(); ++i) {
		const aprend_draw_list_t::draw &draw = draws[i];
		const APREND_DRAW_MESH &mesh         = draw.mesh;
		aprend_draw_list_t::sort_key &key    = list->sort_keys[i];
		key.material = aprend_draw_first(list->first_material, aprend_draw_hash(draw.material), i,
		                                 [&](uint32_t at) { return draws[at].material == draw.material; });
		// The slots past the count are zero (aprend_draw_list_add), so the
		// whole array compares.
		key.vertex_buffers = aprend_draw_first(list->first_vertex_buffers, aprend_draw_hash_vertex_buffers(mesh), i, [&](uint32_t at) {
			return draws[at].mesh._vertex_buffer_count == mesh._vertex_buffer_count &&
			       memcmp(draws[at].mesh._vertex_buffers, mesh._vertex_buffers, sizeof(mesh._vertex_buffers)) == 0;
		});
		key.index_buffer = aprend_draw_first(list->first_index_buffer, aprend_draw_hash(mesh._index_buffer), i,
		                                     [&](uint32_t at) { return draws[at].mesh._index_buffer == mesh._index_buffer; });
		key.first       = mesh._first;
		key.count       = mesh._count;
		key.base_vertex = mesh._base_vertex;
		key.draw        = i;
	}
	// The key ends in the draw's index, so no two are equal: std::sort's
	// order is the only one, and needs no buffer as std::stable_sort does.
	std::sort(list->sort_keys.begin(), list->sort_keys.end(), aprend_draw_key_less);
	for (uint32_t i = 0; i < (uint32_t)draws.size(); ++i)
		list->order[i] = list->sort_keys[i].draw;
}

bool aprend_draw_list_expand(aprend_command_list cmd_list, aprend_draw_list list, uint32_t material_slot) {
	if (!list->built) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_LIST with a list that hasn't been built since it was last changed (aprend_draw_list_build)");
		return false;
	}
	// Checked whole before anything is sent, so a list that can't be drawn
	// records nothing.
	if (material_slot != APREND_DRAW_LIST_NO_MATERIAL) {
		for (const aprend_draw_list_t::batch &batch : list->batches) {
			if (!list->draws[batch.draw].material) {
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_LIST with a _material_slot and a draw that has no material");
				return false;
			}
		}
	}
	if (list->batches.empty())
		return true;

	APREND_COMMAND cmd{};
	if (list->desc._instance_stride > 0) {
		cmd._type                                        = APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER;
		cmd._params._set_transient_vertex_buffer._span = list->span;
		cmd._params._set_transient_vertex_buffer._slot = list->desc._instance_slot;
		aprend_command_list_send(cmd_list, cmd);
	}

	const aprend_draw_list_t::draw *last = nullptr;
	for (const aprend_draw_list_t::batch &batch : list->batches) {
		const aprend_draw_list_t::draw &draw = list->draws[batch.draw];
		const APREND_DRAW_MESH &mesh         = draw.mesh;

		if (material_slot != APREND_DRAW_LIST_NO_MATERIAL && (!last || last->material != draw.material)) {
			cmd                                  = {};
			cmd._type                            = APREND_COMMAND_SET_MATERIAL;
			cmd._params._set_material._slot      = material_slot;
			cmd._params._set_material._material  = draw.material;
			aprend_command_list_send(cmd_list, cmd);
		}
		const bool same_vertex_buffers = last && last->mesh._vertex_buffer_count == mesh._vertex_buffer_count &&
		                                 memcmp(last->mesh._vertex_buffers, mesh._vertex_buffers, sizeof(mesh._vertex_buffers)) == 0;
		if (!same_vertex_buffers) {
			cmd                                                 = {};
			cmd._type                                           = APREND_COMMAND_SET_VERTEX_BUFFERS;
			// Copied by aprend_command_list_send, as any caller's array.
			cmd._params._set_vertex_buffers._vertex_buffers      = const_cast<aprend_buffer *>(mesh._vertex_buffers);
			cmd._params._set_vertex_buffers._vertex_buffer_count = mesh._vertex_buffer_count;
			cmd._params._set_vertex_buffers._start_slot          = 0;
			aprend_command_list_send(cmd_list, cmd);
		}
		if (mesh._index_buffer && (!last || last->mesh._index_buffer != mesh._index_buffer)) {
			cmd                                       = {};
			cmd._type                                 = APREND_COMMAND_SET_INDEX_BUFFER;
			cmd._params._set_index_buffer._index_buffer = mesh._index_buffer;
			aprend_command_list_send(cmd_list, cmd);
		}

		cmd = {};
		if (mesh._index_buffer) {
			cmd._type                                                  = APREND_COMMAND_DRAW_INSTANCED_INDEXED;
			cmd._params._draw_indexed_instanced._index_count_per_instance = mesh._count;
			cmd._params._draw_indexed_instanced._instance_count           = batch.instance_count;
			cmd._params._draw_indexed_instanced._start_index_location     = mesh._first;
			cmd._params._draw_indexed_instanced._base_vertex_location     = mesh._base_vertex;
			cmd._params._draw_indexed_instanced._start_instance_location  = batch.first_instance;
		} else {
			cmd._type                                            = APREND_COMMAND_DRAW_INSTANCED;
			cmd._params._draw_instanced._vertex_count_per_instance = mesh._count;
			cmd._params._draw_instanced._instance_count            = batch.instance_count;
			cmd._params._draw_instanced._start_vertex_location     = mesh._first;
			cmd._params._draw_instanced._start_instance_location   = batch.first_instance;
		}
		aprend_command_list_send(cmd_list, cmd);
		last = &draw;
	}
	return true;
}

extern "C" {

APRESULT aprend_draw_list_create(
    aprend_instance instance,
    const APREND_DRAW_LIST_DESC *desc,
    aprend_draw_list *out_list,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_list)
		*out_list = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_draw_list_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_DRAW_LIST_DESC))
		return base;
	if (desc->_order != APREND_DRAW_LIST_ORDER_BATCHED && desc->_order != APREND_DRAW_LIST_ORDER_SUBMITTED)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_order %u is not an APREND_DRAW_LIST_ORDER", desc->_order);
	if (desc->_instance_stride % 4 != 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_instance_stride %u is not a multiple of 4", desc->_instance_stride);
	if (desc->_instance_stride > 0 && desc->_instance_slot >= APREND_MAX_VERTEX_BINDINGS)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_instance_slot %u is at or above APREND_MAX_VERTEX_BINDINGS", desc->_instance_slot);
	if (desc->_instance_stride > 0 && instance->desc.transient_chunk_size == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the list has instance bytes, and the instance has no transient memory");
	if (!out_list)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_list is NULL");

	APREND_MALLOC__T(result, aprend_draw_list_t);
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the list");
	APREND_CONSTRUCT__T(result, aprend_draw_list_t);
	result->instance = instance;
	result->desc     = *desc;
	*out_list        = result;
	return APRESULT_OK;
}

void aprend_draw_list_destroy(aprend_draw_list list) {
	if (!list)
		return;
	// The span it wrote is the instance's, taken back with its frame; the
	// rest is the CPU's.
	APREND_DESTRUCT__T(list, aprend_draw_list_t);
}

void aprend_draw_list_reset(aprend_draw_list list) {
	if (!list)
		return;
	APREND_ENTER_LIST(list, "aprend_draw_list_reset");
	// clear() keeps the vectors' memory, so a list refilled each frame stops
	// allocating once it has seen its largest frame.
	list->draws.clear();
	list->instance_bytes.clear();
	list->batches.clear();
	list->instance_count = 0;
	list->span           = {};
	list->built          = false;
}

APRESULT aprend_draw_list_add(
    aprend_draw_list list,
    const APREND_DRAW_MESH *mesh,
    aprend_material material,
    uint32_t instance_count,
    const void *instance_data,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!list)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "list is NULL");
	APREND_ENTER_LIST(list, "aprend_draw_list_add");
	if (!mesh)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "mesh is NULL");
	if (instance_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance_count is 0");
	const uint32_t stride = list->desc._instance_stride;
	if (stride > 0 && !instance_data)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance_data is NULL, and the list has instance bytes");
	if (mesh->_vertex_buffer_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh has no vertex buffers");
	if (mesh->_vertex_buffer_count > APREND_MAX_VERTEX_BINDINGS)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh has %u vertex buffers, above APREND_MAX_VERTEX_BINDINGS", mesh->_vertex_buffer_count);
	if (stride > 0 && mesh->_vertex_buffer_count > list->desc._instance_slot)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh's vertex buffers reach slot %u, which is the list's _instance_slot", list->desc._instance_slot);
	for (uint32_t i = 0; i < mesh->_vertex_buffer_count; ++i) {
		if (!mesh->_vertex_buffers[i])
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh's vertex buffer %u is NULL", i);
		if (mesh->_vertex_buffers[i]->instance != list->instance)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh's vertex buffer %u is of another instance", i);
		if (!(mesh->_vertex_buffers[i]->desc.usage & APREND_BUFFER_USAGE_VERTEX))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh's vertex buffer %u was created without APREND_BUFFER_USAGE_VERTEX", i);
	}
	if (mesh->_index_buffer && mesh->_index_buffer->instance != list->instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh's index buffer is of another instance");
	if (mesh->_index_buffer && !(mesh->_index_buffer->desc.usage & APREND_BUFFER_USAGE_INDEX))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh's index buffer was created without APREND_BUFFER_USAGE_INDEX");
	if (!mesh->_index_buffer && mesh->_base_vertex != 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh isn't indexed and has a _base_vertex of %d", mesh->_base_vertex);
	if (mesh->_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the mesh has a _count of 0");
	if (material && material->instance != list->instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "material is of another instance");
	if (list->instance_count + instance_count > UINT32_MAX)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the list would hold more instances than 32 bits count");

	aprend_draw_list_t::draw draw{};
	// Field by field into a zeroed struct, so the slots past the count, and
	// any padding, are zero: two draws of one mesh are then the same bytes.
	memset(&draw.mesh, 0, sizeof(draw.mesh));
	for (uint32_t i = 0; i < mesh->_vertex_buffer_count; ++i)
		draw.mesh._vertex_buffers[i] = mesh->_vertex_buffers[i];
	draw.mesh._vertex_buffer_count = mesh->_vertex_buffer_count;
	draw.mesh._index_buffer        = mesh->_index_buffer;
	draw.mesh._first               = mesh->_first;
	draw.mesh._count               = mesh->_count;
	draw.mesh._base_vertex         = mesh->_base_vertex;
	draw.material                  = material;
	draw.instance_count            = instance_count;
	draw.first_instance            = (uint32_t)list->instance_count;

	// Both or neither: the bytes go in first, and are taken out again if the
	// draw can't follow them. A vector's insert and push_back add nothing
	// when they throw.
	const size_t old_bytes = list->instance_bytes.size();
	try {
		if (stride > 0) {
			const uint8_t *bytes = (const uint8_t *)instance_data;
			list->instance_bytes.insert(list->instance_bytes.end(), bytes, bytes + (size_t)instance_count * stride);
		}
		list->draws.push_back(draw);
	} catch (const std::bad_alloc &) {
		list->instance_bytes.resize(old_bytes);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the draw");
	}
	list->instance_count += instance_count;
	list->built = false;
	return APRESULT_OK;
}

APRESULT aprend_draw_list_build(aprend_draw_list list, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!list)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "list is NULL");
	APREND_ENTER(list->instance, "aprend_draw_list_build");
	list->built = false;
	list->span  = {};
	list->batches.clear();
	if (list->draws.empty()) {
		list->built = true;
		return APRESULT_OK;
	}

	const uint32_t stride = list->desc._instance_stride;
	try {
		list->order.resize(list->draws.size());
		if (list->desc._order == APREND_DRAW_LIST_ORDER_BATCHED) {
			aprend_draw_sort_batched(list);
		} else {
			for (uint32_t i = 0; i < (uint32_t)list->draws.size(); ++i)
				list->order[i] = i;
		}

		// Batches in order, each draw's instances following the one before
		// it in the batch: the instance a batch starts at is where the
		// bytes below put it.
		uint32_t next_instance = 0;
		for (uint32_t index : list->order) {
			const aprend_draw_list_t::draw &draw = list->draws[index];
			if (!list->batches.empty() && aprend_draw_same_batch(list->draws[list->batches.back().draw], draw)) {
				list->batches.back().instance_count += draw.instance_count;
			} else {
				list->batches.push_back({index, next_instance, draw.instance_count});
			}
			next_instance += draw.instance_count;
		}
	} catch (const std::bad_alloc &) {
		list->batches.clear();
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the list's batches");
	}

	if (stride > 0) {
		const uint64_t size = list->instance_count * stride;
		if (const APRESULT taken = aprend_transient_allocate(list->instance, size, 16, &list->span, out_error)) {
			list->batches.clear();
			return taken;
		}
		// In batch order: each draw's bytes, where its batch puts them.
		uint8_t *out = (uint8_t *)list->span.data;
		for (uint32_t index : list->order) {
			const aprend_draw_list_t::draw &draw = list->draws[index];
			const size_t bytes                   = (size_t)draw.instance_count * stride;
			memcpy(out, list->instance_bytes.data() + (size_t)draw.first_instance * stride, bytes);
			out += bytes;
		}
	}
	list->built = true;
	return APRESULT_OK;
}

APRESULT aprend_draw_list_get_counts(
    aprend_draw_list list,
    uint32_t *out_draw_count,
    uint32_t *out_batch_count,
    uint32_t *out_instance_count) {
	if (!list)
		return APRESULT_INVALID_ARGUMENT;
	if (out_draw_count)
		*out_draw_count = (uint32_t)list->draws.size();
	if (out_batch_count)
		*out_batch_count = list->built ? (uint32_t)list->batches.size() : 0;
	if (out_instance_count)
		*out_instance_count = (uint32_t)list->instance_count;
	return APRESULT_OK;
}

} // extern "C"
