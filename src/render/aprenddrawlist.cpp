
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
	return a.material == b.material && memcmp(&a.mesh, &b.mesh, sizeof(aprend_draw_mesh)) == 0;
}

// The order of APREND_DRAW_LIST_ORDER_BATCHED: by material, then by mesh.
// Neither means anything to the GPU; the point is that equal draws end up
// next to each other, and draws of one material together.
static bool aprend_draw_batch_less(const aprend_draw_list_t::draw &a, const aprend_draw_list_t::draw &b) {
	if (a.material != b.material)
		return (uintptr_t)a.material < (uintptr_t)b.material;
	return memcmp(&a.mesh, &b.mesh, sizeof(aprend_draw_mesh)) < 0;
}

bool aprend_draw_list_expand(aprend_command_list cmd_list, aprend_draw_list list, uint32_t material_slot) {
	if (!list->built) {
		printf("aprend: DRAW_LIST with a list that hasn't been built since it was last changed (aprend_draw_list_build)\n");
		return false;
	}
	// Checked whole before anything is sent, so a list that can't be drawn
	// records nothing.
	if (material_slot != APREND_DRAW_LIST_NO_MATERIAL) {
		for (const aprend_draw_list_t::batch &batch : list->batches) {
			if (!list->draws[batch.draw].material) {
				printf("aprend: DRAW_LIST with a _material_slot and a draw that has no material\n");
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
		aprend_send_command(cmd_list, cmd);
	}

	const aprend_draw_list_t::draw *last = nullptr;
	for (const aprend_draw_list_t::batch &batch : list->batches) {
		const aprend_draw_list_t::draw &draw = list->draws[batch.draw];
		const aprend_draw_mesh &mesh         = draw.mesh;

		if (material_slot != APREND_DRAW_LIST_NO_MATERIAL && (!last || last->material != draw.material)) {
			cmd                                  = {};
			cmd._type                            = APREND_COMMAND_SET_MATERIAL;
			cmd._params._set_material._slot      = material_slot;
			cmd._params._set_material._material  = draw.material;
			aprend_send_command(cmd_list, cmd);
		}
		const bool same_vertex_buffers = last && last->mesh._vertex_buffer_count == mesh._vertex_buffer_count &&
		                                 memcmp(last->mesh._vertex_buffers, mesh._vertex_buffers, sizeof(mesh._vertex_buffers)) == 0;
		if (!same_vertex_buffers) {
			cmd                                                 = {};
			cmd._type                                           = APREND_COMMAND_SET_VERTEX_BUFFERS;
			// Copied by aprend_send_command, as any caller's array.
			cmd._params._set_vertex_buffers._vertex_buffers      = const_cast<aprend_vertex_buffer *>(mesh._vertex_buffers);
			cmd._params._set_vertex_buffers._vertex_buffer_count = mesh._vertex_buffer_count;
			cmd._params._set_vertex_buffers._start_slot          = 0;
			aprend_send_command(cmd_list, cmd);
		}
		if (mesh._index_buffer && (!last || last->mesh._index_buffer != mesh._index_buffer)) {
			cmd                                       = {};
			cmd._type                                 = APREND_COMMAND_SET_INDEX_BUFFER;
			cmd._params._set_index_buffer._index_buffer = mesh._index_buffer;
			aprend_send_command(cmd_list, cmd);
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
		aprend_send_command(cmd_list, cmd);
		last = &draw;
	}
	return true;
}

extern "C" {

aprend_draw_list aprend_draw_list_create(
    aprend_instance instance,
    const aprend_draw_list_desc *desc) {
	static const char *who = "aprend_draw_list_create";
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return nullptr;
	}
	if (!desc) {
		printf("apricot: %s: desc is NULL\n", who);
		return nullptr;
	}
	if (desc->_order != APREND_DRAW_LIST_ORDER_BATCHED && desc->_order != APREND_DRAW_LIST_ORDER_SUBMITTED) {
		printf("apricot: %s: _order %u is not an APREND_DRAW_LIST_ORDER\n", who, desc->_order);
		return nullptr;
	}
	if (desc->_instance_stride % 4 != 0) {
		printf("apricot: %s: _instance_stride %u is not a multiple of 4\n", who, desc->_instance_stride);
		return nullptr;
	}
	if (desc->_instance_stride > 0 && desc->_instance_slot >= APREND_MAX_VERTEX_BINDINGS) {
		printf("apricot: %s: _instance_slot %u is at or above APREND_MAX_VERTEX_BINDINGS\n", who, desc->_instance_slot);
		return nullptr;
	}
	if (desc->_instance_stride > 0 && instance->desc.transient_chunk_size == 0) {
		printf("apricot: %s: the list has instance bytes, and the instance has no transient memory\n", who);
		return nullptr;
	}

	APREND_MALLOC__T(result, aprend_draw_list_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_draw_list_t);
	result->instance = instance;
	result->desc     = *desc;
	return result;
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
	// clear() keeps the vectors' memory, so a list refilled each frame stops
	// allocating once it has seen its largest frame.
	list->draws.clear();
	list->instance_bytes.clear();
	list->batches.clear();
	list->instance_count = 0;
	list->span           = {};
	list->built          = false;
}

bool aprend_draw_list_add(
    aprend_draw_list list,
    const aprend_draw_mesh *mesh,
    aprend_material material,
    uint32_t instance_count,
    const void *instance_data) {
	static const char *who = "aprend_draw_list_add";
	if (!list) {
		printf("apricot: %s: list is NULL\n", who);
		return false;
	}
	if (!mesh) {
		printf("apricot: %s: mesh is NULL\n", who);
		return false;
	}
	if (instance_count == 0) {
		printf("apricot: %s: instance_count is 0\n", who);
		return false;
	}
	const uint32_t stride = list->desc._instance_stride;
	if (stride > 0 && !instance_data) {
		printf("apricot: %s: instance_data is NULL, and the list has instance bytes\n", who);
		return false;
	}
	if (mesh->_vertex_buffer_count == 0) {
		printf("apricot: %s: the mesh has no vertex buffers\n", who);
		return false;
	}
	if (mesh->_vertex_buffer_count > APREND_MAX_VERTEX_BINDINGS) {
		printf("apricot: %s: the mesh has %u vertex buffers, above APREND_MAX_VERTEX_BINDINGS\n", who, mesh->_vertex_buffer_count);
		return false;
	}
	if (stride > 0 && mesh->_vertex_buffer_count > list->desc._instance_slot) {
		printf("apricot: %s: the mesh's vertex buffers reach slot %u, which is the list's _instance_slot\n", who, list->desc._instance_slot);
		return false;
	}
	for (uint32_t i = 0; i < mesh->_vertex_buffer_count; ++i) {
		if (!mesh->_vertex_buffers[i]) {
			printf("apricot: %s: the mesh's vertex buffer %u is NULL\n", who, i);
			return false;
		}
		if (mesh->_vertex_buffers[i]->instance != list->instance) {
			printf("apricot: %s: the mesh's vertex buffer %u is of another instance\n", who, i);
			return false;
		}
	}
	if (mesh->_index_buffer && mesh->_index_buffer->instance != list->instance) {
		printf("apricot: %s: the mesh's index buffer is of another instance\n", who);
		return false;
	}
	if (!mesh->_index_buffer && mesh->_base_vertex != 0) {
		printf("apricot: %s: the mesh isn't indexed and has a _base_vertex of %d\n", who, mesh->_base_vertex);
		return false;
	}
	if (mesh->_count == 0) {
		printf("apricot: %s: the mesh has a _count of 0\n", who);
		return false;
	}
	if (material && material->instance != list->instance) {
		printf("apricot: %s: material is of another instance\n", who);
		return false;
	}
	if (list->instance_count + instance_count > UINT32_MAX) {
		printf("apricot: %s: the list would hold more instances than 32 bits count\n", who);
		return false;
	}

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
		printf("apricot: %s: out of memory\n", who);
		return false;
	}
	list->instance_count += instance_count;
	list->built = false;
	return true;
}

bool aprend_draw_list_build(aprend_draw_list list) {
	static const char *who = "aprend_draw_list_build";
	if (!list) {
		printf("apricot: %s: list is NULL\n", who);
		return false;
	}
	list->built = false;
	list->span  = {};
	list->batches.clear();
	if (list->draws.empty()) {
		list->built = true;
		return true;
	}

	const uint32_t stride = list->desc._instance_stride;
	try {
		list->order.resize(list->draws.size());
		for (uint32_t i = 0; i < (uint32_t)list->draws.size(); ++i)
			list->order[i] = i;
		// Stable, so instances of one batch stay in the order they were
		// added in.
		if (list->desc._order == APREND_DRAW_LIST_ORDER_BATCHED)
			std::stable_sort(list->order.begin(), list->order.end(),
			                 [list](uint32_t a, uint32_t b) { return aprend_draw_batch_less(list->draws[a], list->draws[b]); });

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
		printf("apricot: %s: out of memory\n", who);
		return false;
	}

	if (stride > 0) {
		const uint64_t size = list->instance_count * stride;
		if (!aprend_transient_allocate(list->instance, size, 16, &list->span)) {
			list->batches.clear();
			printf("apricot: %s: %llu bytes of transient memory can't be taken for the instances\n", who, (unsigned long long)size);
			return false;
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
	return true;
}

bool aprend_draw_list_get_counts(
    aprend_draw_list list,
    uint32_t *out_draw_count,
    uint32_t *out_batch_count,
    uint32_t *out_instance_count) {
	if (!list)
		return false;
	if (out_draw_count)
		*out_draw_count = (uint32_t)list->draws.size();
	if (out_batch_count)
		*out_batch_count = list->built ? (uint32_t)list->batches.size() : 0;
	if (out_instance_count)
		*out_instance_count = (uint32_t)list->instance_count;
	return true;
}

} // extern "C"
