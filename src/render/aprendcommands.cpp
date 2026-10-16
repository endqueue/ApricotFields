
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

#include <cstdio>

aprend_command_list_t::~aprend_command_list_t() {
	// The SpudGPU lists it recorded into are the instance's.
	commands.clear();
}

/* Adds [store] to the staged stores [cmd_list] uses, once. A store that is
 * written directly has nothing to copy and isn't added. */
static void aprend_cmd_use_store(
    aprend_command_list cmd_list,
    aprend_buffer_store *store) {
	if (!store->staging)
		return;
	for (aprend_buffer_store *known : cmd_list->staged_stores)
		if (known == store)
			return;
	cmd_list->staged_stores.push_back(store);
}

/* Records into an upload list, taken from the instance's current frame,
 * what has to happen ahead of the list's own commands: a copy of the dirty range of every staged store the
 * list uses, each buffer moved into COPY_DEST for the copy and into the state
 * it is used in after it; every store whose state the list tracks put in its
 * use_state; and [copies], the materials' blocks, each uniform buffer they
 * land in moved into COPY_DEST and back to the uniform state once. A copy is
 * ordered after every earlier submission's reading of what it overwrites,
 * by the barrier ahead of it. [out_upload] is the upload list, or NULL if
 * there was nothing to record. Fails, recording nothing, if no list can be
 * taken. Throws std::bad_alloc, recording nothing. */
static SPUDRESULT aprend_cmd_record_uploads(
    aprend_command_list cmd_list,
    const std::vector<aprend_upload_copy> &copies,
    spudgpu_command_list *out_upload) {
	*out_upload = nullptr;
	std::vector<spudgpu_buffer_barrier> before;
	std::vector<spudgpu_buffer_barrier> after;
	for (const aprend_upload_copy &copy : copies) {
		bool known = false;
		for (const spudgpu_buffer_barrier &barrier : before)
			if (barrier.buffer == copy.destination)
				known = true;
		if (known)
			continue;
		before.push_back({copy.destination, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST});
		after.push_back({copy.destination, SPUDGPU_RESOURCE_STATE_COPY_DEST, SPUDGPU_RESOURCE_STATE_VERTEX_BUFFER});
	}
	for (aprend_buffer_store *store : cmd_list->staged_stores) {
		if (store->dirty_begin == store->dirty_end)
			continue;
		before.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST});
		after.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_COPY_DEST, store->use_state});
	}
	// A store whose state the list's barriers track must start the list in
	// its use_state, which the copy above leaves it in. One with nothing to
	// copy is put there directly, so the list starts the same either way.
	for (const auto &tracked : cmd_list->buffer_states) {
		aprend_buffer_store *store = tracked.store;
		if (store->staging && store->dirty_begin != store->dirty_end)
			continue;
		before.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_COMMON, store->use_state});
	}
	if (before.empty())
		return SPUD_SUCCESS;

	spudgpu_command_list upload = nullptr;
	SPUDRESULT sr               = aprend_instance_take_command_list(cmd_list->instance, &upload);
	if (SPUDFAIL(sr))
		return sr;
	spudgpu_begin_command_list(upload);
	spudgpu_cmd_pipeline_barrier(upload, before.data(), (uint32_t)before.size(), nullptr, 0);
	for (aprend_buffer_store *store : cmd_list->staged_stores) {
		if (store->dirty_begin == store->dirty_end)
			continue;
		spudgpu_cmd_copy_buffer(upload, store->staging, store->buffer, store->dirty_begin, store->dirty_begin, store->dirty_end - store->dirty_begin);
	}
	for (const aprend_upload_copy &copy : copies)
		spudgpu_cmd_copy_buffer(upload, copy.source, copy.destination, copy.source_offset, copy.destination_offset, copy.size);
	if (!after.empty())
		spudgpu_cmd_pipeline_barrier(upload, after.data(), (uint32_t)after.size(), nullptr, 0);
	spudgpu_end_command_list(upload);
	*out_upload = upload;
	return SPUD_SUCCESS;
}

extern "C" {

aprend_command_list aprend_command_list_create(aprend_instance instance) {
	if (!instance)
		return nullptr;
	aprend_command_list_t *result = (aprend_command_list_t *)malloc(sizeof(aprend_command_list_t));
	if (!result)
		return nullptr;
	result = new (result) aprend_command_list_t();

	// What it records into is taken from the instance at each compile.
	result->instance = instance;
	return result;
}
void aprend_command_list_destroy(aprend_command_list cmd_list) {
	if (!cmd_list)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(cmd_list->instance, cmd_list, &aprend_release_handle<aprend_command_list_t>);
}

void aprend_command_list_reset(aprend_command_list cmd_list) {
	if (!cmd_list)
		return;
	const size_t preserve_size = cmd_list->commands.size();
	cmd_list->commands.clear();
	cmd_list->commands.reserve(preserve_size);
	cmd_list->color_target_storage.clear();
	cmd_list->vertex_buffer_storage.clear();
	cmd_list->viewport_storage.clear();
	cmd_list->scissor_rect_storage.clear();
	cmd_list->push_constant_storage.clear();
	cmd_list->label_storage.clear();
	cmd_list->recording_error = false;
	cmd_list->layout_uses.clear();
	cmd_list->staged_stores.clear();
	cmd_list->buffer_states.clear();
	cmd_list->materials.clear();
	cmd_list->compiled           = false;
	cmd_list->present_swap_chain = nullptr;
}
void aprend_send_command(
    aprend_command_list cmd_list,
    APREND_COMMAND cmd) {
	if (!cmd_list)
		return;
	if (cmd._type == APREND_COMMAND_NONE)
		return;

	// The three commands that stand for others are expanded into them here,
	// from the pass or list as it is now, and are not recorded themselves.
	if (cmd._type == APREND_COMMAND_BEGIN_RENDER_PASS) {
		aprend_render_pass pass = cmd._params._begin_render_pass._pass;
		if (!pass) {
			printf("aprend: BEGIN_RENDER_PASS with a NULL pass\n");
			cmd_list->recording_error = true;
			return;
		}
		if (pass->instance != cmd_list->instance) {
			printf("aprend: BEGIN_RENDER_PASS with a pass of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_render_pass_expand(cmd_list, pass))
			cmd_list->recording_error = true;
		return;
	}
	if (cmd._type == APREND_COMMAND_SET_COMPUTE_PASS) {
		aprend_compute_pass pass = cmd._params._set_compute_pass._pass;
		if (!pass) {
			printf("aprend: SET_COMPUTE_PASS with a NULL pass\n");
			cmd_list->recording_error = true;
			return;
		}
		if (pass->instance != cmd_list->instance) {
			printf("aprend: SET_COMPUTE_PASS with a pass of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_compute_pass_expand(cmd_list, pass))
			cmd_list->recording_error = true;
		return;
	}
	if (cmd._type == APREND_COMMAND_DRAW_LIST) {
		const auto &p = cmd._params._draw_list;
		if (!p._list) {
			printf("aprend: DRAW_LIST with a NULL list\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._list->instance != cmd_list->instance) {
			printf("aprend: DRAW_LIST with a list of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._material_slot != APREND_DRAW_LIST_NO_MATERIAL && p._material_slot >= APREND_MAX_BINDING_LAYOUTS) {
			printf("aprend: DRAW_LIST with _material_slot %u, at or above APREND_MAX_BINDING_LAYOUTS\n", p._material_slot);
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_draw_list_expand(cmd_list, p._list, p._material_slot))
			cmd_list->recording_error = true;
		return;
	}

	if (cmd._type == APREND_COMMAND_SET_MATERIAL) {
		const auto &p = cmd._params._set_material;
		if (!p._material) {
			printf("aprend: SET_MATERIAL with a NULL material\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._material->instance != cmd_list->instance) {
			printf("aprend: SET_MATERIAL with a material of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._slot >= APREND_MAX_BINDING_LAYOUTS) {
			printf("aprend: SET_MATERIAL with _slot %u, at or above APREND_MAX_BINDING_LAYOUTS\n", p._slot);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_BEGIN_RENDERING) {
		auto &p = cmd._params._begin_rendering;
		if (p._color_target_count > APREND_MAX_COLOR_TARGETS || (p._color_target_count > 0 && !p._color_targets)) {
			printf("aprend: BEGIN_RENDERING with invalid color targets (count %u)\n", p._color_target_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._timer && p._timer->instance != cmd_list->instance) {
			printf("aprend: BEGIN_RENDERING with a _timer of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._timer && p._timer_slot >= p._timer->slot_count) {
			printf("aprend: BEGIN_RENDERING with _timer_slot %u, at or above the timer's %u slots\n", p._timer_slot, p._timer->slot_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._color_target_count > 0) {
			cmd_list->color_target_storage.emplace_back(p._color_targets, p._color_targets + p._color_target_count);
			p._color_targets = cmd_list->color_target_storage.back().data();
		} else {
			p._color_targets = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_BEGIN_LABEL) {
		auto &p = cmd._params._begin_label;
		cmd_list->label_storage.emplace_back(p._label ? p._label : "");
		p._label = cmd_list->label_storage.back().c_str();
	}

	// The array commands below are copied the same way: the caller's array
	// only has to live for this call.
	if (cmd._type == APREND_COMMAND_SET_VERTEX_BUFFERS) {
		auto &p = cmd._params._set_vertex_buffers;
		if (p._vertex_buffer_count > 0 && !p._vertex_buffers) {
			printf("aprend: SET_VERTEX_BUFFERS with a NULL array (count %u)\n", p._vertex_buffer_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._start_slot > APREND_MAX_VERTEX_BINDINGS) {
			printf("aprend: SET_VERTEX_BUFFERS with _start_slot %u, above APREND_MAX_VERTEX_BINDINGS\n", p._start_slot);
			cmd_list->recording_error = true;
			return;
		}
		if (p._vertex_buffer_count > APREND_MAX_VERTEX_BINDINGS - p._start_slot) {
			printf("aprend: SET_VERTEX_BUFFERS of %u buffers from slot %u runs past the last vertex buffer slot\n", p._vertex_buffer_count,
			       p._start_slot);
			cmd_list->recording_error = true;
			return;
		}
		for (uint32_t i = 0; i < p._vertex_buffer_count; ++i) {
			if (!p._vertex_buffers[i]) {
				printf("aprend: SET_VERTEX_BUFFERS vertex buffer %u is NULL\n", i);
				cmd_list->recording_error = true;
				return;
			}
		}
		if (p._vertex_buffer_count > 0) {
			cmd_list->vertex_buffer_storage.emplace_back(p._vertex_buffers, p._vertex_buffers + p._vertex_buffer_count);
			p._vertex_buffers = cmd_list->vertex_buffer_storage.back().data();
		} else {
			p._vertex_buffers = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER) {
		const auto &p = cmd._params._set_transient_vertex_buffer;
		if (p._slot >= APREND_MAX_VERTEX_BINDINGS) {
			printf("aprend: SET_TRANSIENT_VERTEX_BUFFER with _slot %u, at or above APREND_MAX_VERTEX_BINDINGS\n", p._slot);
			cmd_list->recording_error = true;
			return;
		}
		if (p._stride == 0) {
			printf("aprend: SET_TRANSIENT_VERTEX_BUFFER with a _stride of 0\n");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER) {
		const auto &p = cmd._params._set_transient_index_buffer;
		if (p._index_size != 2 && p._index_size != 4) {
			printf("aprend: SET_TRANSIENT_INDEX_BUFFER with an _index_size of %u, which is not 2 or 4\n", p._index_size);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_VIEWPORTS) {
		auto &p = cmd._params._set_viewports;
		if (p._viewport_count > 0 && !p._viewports) {
			printf("aprend: SET_VIEWPORTS with a NULL array (count %u)\n", p._viewport_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._viewport_count > 0) {
			cmd_list->viewport_storage.emplace_back(p._viewports, p._viewports + p._viewport_count);
			p._viewports = cmd_list->viewport_storage.back().data();
		} else {
			p._viewports = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_SCISSOR_RECTS) {
		auto &p = cmd._params._set_scissor_rects;
		if (p._scissor_rect_count > 0 && !p._scissor_rects) {
			printf("aprend: SET_SCISSOR_RECTS with a NULL array (count %u)\n", p._scissor_rect_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._scissor_rect_count > 0) {
			cmd_list->scissor_rect_storage.emplace_back(p._scissor_rects, p._scissor_rects + p._scissor_rect_count);
			p._scissor_rects = cmd_list->scissor_rect_storage.back().data();
		} else {
			p._scissor_rects = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_DRAW_INDIRECT || cmd._type == APREND_COMMAND_DRAW_INDEXED_INDIRECT) {
		const auto &p = cmd._params._draw_indirect;
		// What one entry is depends on which of the two commands this is.
		const uint64_t entry_size = cmd._type == APREND_COMMAND_DRAW_INDIRECT ? sizeof(spudgpu_draw_indirect_args)
		                                                                       : sizeof(spudgpu_draw_indexed_indirect_args);
		if (!p._buffer && !p._set) {
			printf("aprend: indirect draw with neither a _buffer nor a _set\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._buffer && p._set) {
			printf("aprend: indirect draw with both a _buffer and a _set\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._set && p._set->instance != cmd_list->instance) {
			printf("aprend: indirect draw with a _set of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		// Every copy of a set has one size and one usage.
		const aprend_storage_buffer arguments = p._set ? p._set->buffers[0] : p._buffer;
		if (!(arguments->usage & APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS)) {
			printf("aprend: indirect draw with a buffer created without APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._draw_count == 0) {
			printf("aprend: indirect draw with a _draw_count of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset % 4 != 0) {
			printf("aprend: indirect draw with an _offset that is not a multiple of 4\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._stride % 4 != 0) {
			printf("aprend: indirect draw with _stride %u, not a multiple of 4\n", p._stride);
			cmd_list->recording_error = true;
			return;
		}
		if (p._stride < entry_size) {
			printf("aprend: indirect draw with _stride %u, smaller than the %u bytes of one entry\n", p._stride, (unsigned)entry_size);
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset > arguments->size) {
			printf("aprend: indirect draw with an _offset past the end of the buffer\n");
			cmd_list->recording_error = true;
			return;
		}
		// Where the last entry ends: every one before it is a whole stride.
		const uint64_t span = (uint64_t)(p._draw_count - 1) * p._stride + entry_size;
		if (span > arguments->size - p._offset) {
			printf("aprend: indirect draw of %u entries runs past the end of the buffer\n", p._draw_count);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_DISPATCH_MESH) {
		const auto &p = cmd._params._dispatch_mesh;
		if (p._group_count_x == 0) {
			printf("aprend: DISPATCH_MESH with a _group_count_x of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_y == 0) {
			printf("aprend: DISPATCH_MESH with a _group_count_y of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_z == 0) {
			printf("aprend: DISPATCH_MESH with a _group_count_z of 0\n");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_DISPATCH) {
		const auto &p = cmd._params._dispatch;
		if (p._group_count_x == 0) {
			printf("aprend: DISPATCH with a _group_count_x of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_y == 0) {
			printf("aprend: DISPATCH with a _group_count_y of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_z == 0) {
			printf("aprend: DISPATCH with a _group_count_z of 0\n");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_PUSH_CONSTANTS) {
		auto &p = cmd._params._push_constants;
		if (!p._data) {
			printf("aprend: PUSH_CONSTANTS with NULL _data\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._size == 0) {
			printf("aprend: PUSH_CONSTANTS with a _size of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset % 4 != 0) {
			printf("aprend: PUSH_CONSTANTS with _offset %u, not a multiple of 4\n", p._offset);
			cmd_list->recording_error = true;
			return;
		}
		if (p._size % 4 != 0) {
			printf("aprend: PUSH_CONSTANTS with _size %u, not a multiple of 4\n", p._size);
			cmd_list->recording_error = true;
			return;
		}
		const uint8_t *bytes = (const uint8_t *)p._data;
		cmd_list->push_constant_storage.emplace_back(bytes, bytes + p._size);
		p._data = cmd_list->push_constant_storage.back().data();
	}
	if (cmd._type == APREND_COMMAND_GENERATE_MIPS) {
		const auto &p = cmd._params._generate_mips;
		if (!p._texture) {
			printf("aprend: GENERATE_MIPS with a NULL texture\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._texture->instance != cmd_list->instance) {
			printf("aprend: GENERATE_MIPS with a texture of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_generate_mips_valid(p._texture, p._base_mip, p._filter, "aprend: GENERATE_MIPS")) {
			cmd_list->recording_error = true;
			return;
		}
	}

	cmd_list->commands.emplace_back(cmd);
}

/* Where [cmd_list] keeps the layout of (mip, layer) of [range]'s texture, as
 * an index into layout_uses, adding it the first time the list uses that
 * subresource: it starts from the layout everything submitted so far leaves
 * it in. An index, not a pointer: adding grows the vector. */
static size_t aprend_cmd_layout_use(
    aprend_command_list cmd_list,
    const aprend_texture_range &range,
    uint32_t mip,
    uint32_t layer) {
	for (size_t i = 0; i < cmd_list->layout_uses.size(); ++i) {
		const auto &use = cmd_list->layout_uses[i];
		if (use.image == range.image && use.mip == mip && use.layer == layer)
			return i;
	}
	SPUDGPU_IMAGE_LAYOUT *tracked = range.tracking->at(mip, layer);
	cmd_list->layout_uses.push_back({range.image, mip, layer, tracked, *tracked, *tracked});
	return cmd_list->layout_uses.size() - 1;
}

/* Moves every mip of every layer in [range] into [layout]. Unless
 * [skip_if_same], a barrier is recorded even for one already there:
 * back-to-back passes on the same attachment (a LOAD after a previous pass's
 * STORE) still need the barrier's memory dependency, and for a storage image
 * a barrier from GENERAL to GENERAL is what orders one shader's writes before
 * the next one's accesses (spudgpu.h). Two reads in a row need nothing, which
 * is what [skip_if_same] is for.
 *
 * The subresources of a range are usually all in one layout, and then it is
 * one barrier over the range; where they differ, each is moved from its own.
 *
 * Tracks the layouts in [cmd_list]'s own layout_uses, never on the texture:
 * a texture's layouts only change once the list is actually submitted.
 * Recorded outside any pass. */
static void aprend_cmd_transition_range(
    aprend_command_list cmd_list,
    const aprend_texture_range &range,
    SPUDGPU_IMAGE_LAYOUT layout,
    bool skip_if_same) {
	// Make sure the list has an entry for each subresource, and see whether
	// they agree on where they are now.
	bool uniform                  = true;
	SPUDGPU_IMAGE_LAYOUT from     = SPUDGPU_IMAGE_LAYOUT_UNDEFINED;
	for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
		for (uint32_t mip = 0; mip < range.mip_count; ++mip) {
			const size_t index                = aprend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer);
			const SPUDGPU_IMAGE_LAYOUT current = cmd_list->layout_uses[index].final_layout;
			if (layer == 0 && mip == 0)
				from = current;
			else if (current != from)
				uniform = false;
		}
	}

	if (uniform) {
		if (skip_if_same && from == layout)
			return;
		const spudgpu_image_view_desc_subresource_range spud_range = aprend_texture_range_spudgpu(range);
		spudgpu_cmd_image_barrier_subresource(cmd_list->cmd_list, range.image, &spud_range, from, layout);
	}
	for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
		for (uint32_t mip = 0; mip < range.mip_count; ++mip) {
			const size_t index = aprend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer);
			auto &use          = cmd_list->layout_uses[index];
			if (!uniform && !(skip_if_same && use.final_layout == layout)) {
				spudgpu_image_view_desc_subresource_range one{};
				one.base_mip_level    = use.mip;
				one.mip_level_count   = 1;
				one.base_array_layer  = use.layer;
				one.array_layer_count = 1;
				spudgpu_cmd_image_barrier_subresource(cmd_list->cmd_list, range.image, &one, use.final_layout, layout);
			}
			use.final_layout = layout;
		}
	}
}
/* A view's whole range, with a barrier whether or not it is already there:
 * for a pass's targets. */
static void aprend_cmd_transition_view(
    aprend_command_list cmd_list,
    aprend_texture_view view,
    SPUDGPU_IMAGE_LAYOUT layout) {
	aprend_cmd_transition_range(cmd_list, aprend_texture_view_range(view), layout, false);
}

/* PRESENT_TEXTURE: copies the source into the acquired back buffer and moves
 * the back buffer to PRESENT_SRC. The back buffer's previous contents are
 * discarded (UNDEFINED old layout) since the copy overwrites the region that
 * gets shown. */
static bool aprend_cmd_present_texture(
    aprend_command_list cmd_list,
    const APREND_COMMAND &command) {
	const auto &p         = command._params._present_texture;
	aprend_swap_chain sc  = p._swap_chain;
	aprend_texture2d src  = p._source;
	if (!sc || !sc->swap_chain || !src) {
		printf("aprend: PRESENT_TEXTURE with a NULL source or swap chain\n");
		return false;
	}
	if (sc->acquired_image == APREND_SWAP_CHAIN_NO_IMAGE || sc->submitted) {
		printf("aprend: PRESENT_TEXTURE without a freshly acquired back buffer (call aprend_swap_chain_acquire first)\n");
		return false;
	}
	if (cmd_list->present_swap_chain) {
		printf("aprend: more than one PRESENT_TEXTURE in one command list\n");
		return false;
	}

	spudgpu_image_view back_view = spudgpu_get_swap_chain_image_view(sc->swap_chain, sc->acquired_image);
	spudgpu_image_view_desc back_view_desc{};
	if (!back_view || SPUDFAIL(spudgpu_get_image_view_desc(back_view, &back_view_desc)) || !back_view_desc.parent_image) {
		printf("aprend: PRESENT_TEXTURE couldn't get the acquired back buffer\n");
		return false;
	}
	// The real back-buffer size, which the surface can override on Vulkan -
	// not necessarily the size the swap chain was requested at.
	spudgpu_swap_chain_desc actual{};
	if (SPUDFAIL(spudgpu_get_swap_chain_desc(sc->swap_chain, &actual)) || !actual.width || !actual.height) {
		printf("aprend: PRESENT_TEXTURE couldn't get the back buffer size\n");
		return false;
	}

	spudgpu_command_list cmd = cmd_list->cmd_list;
	// What is presented is mip 0 of layer 0, and only that is moved.
	aprend_texture_range presented{};
	presented.image       = src->image;
	presented.tracking    = &src->tracking;
	presented.base_mip    = 0;
	presented.mip_count   = 1;
	presented.base_layer  = 0;
	presented.layer_count = 1;
	aprend_cmd_transition_range(cmd_list, presented, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC, false);
	spudgpu_cmd_image_barrier_view(cmd, back_view, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST);

	uint32_t width  = src->desc.width < actual.width ? src->desc.width : actual.width;
	uint32_t height = src->desc.height < actual.height ? src->desc.height : actual.height;
	spudgpu_image_blit_desc blit{};
	blit.src_array_layer_count = 1;
	blit.src_x1                = width;
	blit.src_y1                = height;
	blit.src_z1                = 1;
	blit.dst_array_layer_count = 1;
	blit.dst_x1                = width;
	blit.dst_y1                = height;
	blit.dst_z1                = 1;
	blit.filter                = SPUDGPU_FILTER_NEAREST;
	spudgpu_cmd_blit_image(cmd, src->image, back_view_desc.parent_image, &blit);

	spudgpu_cmd_image_barrier_view(cmd, back_view, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST, SPUDGPU_IMAGE_LAYOUT_PRESENT_SRC);

	cmd_list->present_swap_chain  = sc;
	cmd_list->present_image_index = sc->acquired_image;
	return true;
}

static bool aprend_render_view_valid(aprend_texture_view view) {
	if (!view)
		return false;
	return view->image_view && view->dimension == APREND_TEXTURE_DIMENSION_2D && view->texture._t2d;
}

static bool aprend_begin_rendering_valid(const APREND_COMMAND &command) {
	const auto &p = command._params._begin_rendering;
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		if (!aprend_render_view_valid(p._color_targets[i].view)) {
			printf("aprend: BEGIN_RENDERING color target %u is not a view of a 2D texture\n", i);
			return false;
		}
		/* spudgpu_color_attachment_desc has no resolve attachment yet, so a
		 * resolve can't be honored — fail rather than silently skip it. */
		if (p._color_targets[i].resolve_view) {
			printf("aprend: BEGIN_RENDERING color target %u has a resolve_view, which SpudGPU doesn't support yet\n", i);
			return false;
		}
	}
	if (p._depth_target.view && !aprend_render_view_valid(p._depth_target.view)) {
		printf("aprend: BEGIN_RENDERING depth target is not a view of a 2D texture\n");
		return false;
	}
	if (p._color_target_count == 0 && !p._depth_target.view) {
		printf("aprend: BEGIN_RENDERING has no color or depth target\n");
		return false;
	}
	// The targets of one pass are all one layer, or all the same number of
	// layers of 2D array views: the pass is layered or it isn't.
	aprend_texture_view first = p._color_target_count > 0 ? p._color_targets[0].view : p._depth_target.view;
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		if (p._color_targets[i].view->desc.shape != first->desc.shape) {
			printf("aprend: BEGIN_RENDERING color target %u is not the shape of the pass's first target\n", i);
			return false;
		}
		if (p._color_targets[i].view->desc.layer_count != first->desc.layer_count) {
			printf("aprend: BEGIN_RENDERING color target %u covers %u layers, and the pass's first target covers %u\n", i,
			       p._color_targets[i].view->desc.layer_count, first->desc.layer_count);
			return false;
		}
	}
	if (p._depth_target.view && p._depth_target.view->desc.shape != first->desc.shape) {
		printf("aprend: BEGIN_RENDERING depth target is not the shape of the pass's first target\n");
		return false;
	}
	if (p._depth_target.view && p._depth_target.view->desc.layer_count != first->desc.layer_count) {
		printf("aprend: BEGIN_RENDERING depth target covers %u layers, and the pass's first target covers %u\n",
		       p._depth_target.view->desc.layer_count, first->desc.layer_count);
		return false;
	}
	return true;
}

/* Whether [pipeline] renders to the targets of the pass [begin] opened: as
 * many color targets, each of the format the pipeline has at its index, and
 * a depth target of its depth format, or none for a pipeline without depth.
 * Every graphics API requires a pipeline to match the pass it draws in;
 * one that doesn't is undefined on some and an error on others. Prints
 * what differs. */
static bool aprend_pipeline_matches_pass(
    aprend_graphics_pipeline pipeline,
    const APREND_COMMAND &begin) {
	const auto &p                           = begin._params._begin_rendering;
	const aprend_graphics_pipeline_desc &pd = pipeline->desc;
	if (pd.color_attachment_count != p._color_target_count) {
		printf("aprend: SET_SHADER_PIPELINE: the pipeline renders to %u color targets, and the pass has %u\n", pd.color_attachment_count,
		       p._color_target_count);
		return false;
	}
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		const SPUDGPU_FORMAT format = p._color_targets[i].view->texture._t2d->desc.format;
		if (pd.color_attachment_formats[i] != format) {
			printf("aprend: SET_SHADER_PIPELINE: the pipeline renders format %u to color target %u, which is of format %u\n",
			       (unsigned)pd.color_attachment_formats[i], i, (unsigned)format);
			return false;
		}
	}
	const SPUDGPU_FORMAT depth = p._depth_target.view ? p._depth_target.view->texture._t2d->desc.format : SPUDGPU_FORMAT_UNKNOWN;
	if (pd.depth_format != depth) {
		printf("aprend: SET_SHADER_PIPELINE: the pipeline's depth format is %u, and the pass's depth target is of format %u\n",
		       (unsigned)pd.depth_format, (unsigned)depth);
		return false;
	}
	return true;
}

/* Before a BEGIN_RENDERING [begin] that names a timer: resets the two
 * timestamps of its slot in this frame's set, outside the pass, and notes
 * the slot for submit to mark. False, recording nothing, without memory. */
static bool aprend_cmd_begin_timed(
    aprend_command_list cmd_list,
    const APREND_COMMAND &begin) {
	const auto &p = begin._params._begin_rendering;
	if (!p._timer)
		return true;
	try {
		cmd_list->timer_uses.push_back({p._timer, p._timer_slot});
	} catch (const std::bad_alloc &) {
		printf("aprend: compile: out of memory\n");
		return false;
	}
	spudgpu_cmd_reset_timestamps(cmd_list->cmd_list, p._timer->sets[cmd_list->compiled_frame], p._timer_slot * 2, 2);
	return true;
}

/* After the END_RENDERING of the pass [begin] opened: makes its slot's two
 * timestamps readable, outside the pass. */
static void aprend_cmd_end_timed(
    aprend_command_list cmd_list,
    const APREND_COMMAND &begin) {
	const auto &p = begin._params._begin_rendering;
	if (!p._timer)
		return;
	spudgpu_cmd_resolve_timestamps(cmd_list->cmd_list, p._timer->sets[cmd_list->compiled_frame], p._timer_slot * 2, 2);
}

/* Opens an explicit BEGIN_RENDERING pass: transitions every attachment into
 * its attachment layout (barriers only happen outside a pass), then begins
 * dynamic rendering with each attachment's own load/store ops and clear
 * values. Assumes aprend_begin_rendering_valid passed. */
static void aprend_cmd_begin_rendering(
    aprend_command_list cmd_list,
    const APREND_COMMAND &command) {
	const auto &p            = command._params._begin_rendering;
	spudgpu_command_list cmd = cmd_list->cmd_list;

	spudgpu_rendering_begin_desc desc{};
	desc.color_attachment_count = p._color_target_count;
	desc.x                      = p._x;
	desc.y                      = p._y;
	desc.width                  = p._width;
	desc.height                 = p._height;
	// The slot's start at 2 * slot and its end after it (aprend_gpu_timer_t).
	if (p._timer) {
		desc.timestamp_set         = p._timer->sets[cmd_list->compiled_frame];
		desc.timestamp_begin_index = p._timer_slot * 2;
		desc.timestamp_end_index   = p._timer_slot * 2 + 1;
	}

	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		const aprend_color_target &target = p._color_targets[i];
		aprend_cmd_transition_view(cmd_list, target.view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		spudgpu_color_attachment_desc &color = desc.color_attachments[i];
		color.image_view                     = target.view->image_view;
		color.load_op                        = target.load_op;
		color.store_op                       = target.store_op;
		memcpy(color.clear_color, target.clear_color, sizeof(color.clear_color));
	}

	const aprend_depth_target &depth = p._depth_target;
	if (depth.view) {
		aprend_cmd_transition_view(cmd_list, depth.view, SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

		desc.depth_attachment.image_view       = depth.view->image_view;
		desc.depth_attachment.depth_load_op    = depth.depth_load_op;
		desc.depth_attachment.depth_store_op   = depth.depth_store_op;
		desc.depth_attachment.stencil_load_op  = depth.stencil_load_op;
		desc.depth_attachment.stencil_store_op = depth.stencil_store_op;
		desc.depth_attachment.clear_depth      = depth.clear_depth;
		desc.depth_attachment.clear_stencil    = depth.clear_stencil;
	}

	// A pass over 2D array views is layered, over as many layers as each
	// covers (aprend_begin_rendering_valid made them agree); one over 2D
	// views is not, which SpudGPU spells 0. An array view of one layer is
	// still a layered pass.
	aprend_texture_view sizing = p._color_target_count > 0 ? p._color_targets[0].view : depth.view;
	if (sizing->desc.shape == APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY)
		desc.layer_count = sizing->desc.layer_count;

	if (desc.width == 0 || desc.height == 0) {
		// The size of the mip the view covers, not of the texture's mip 0.
		desc.width                 = aprend_mip_size(sizing->texture._t2d->desc.width, sizing->desc.base_mip);
		desc.height                = aprend_mip_size(sizing->texture._t2d->desc.height, sizing->desc.base_mip);
	}

	spudgpu_cmd_begin_rendering(cmd, &desc);
}

/* A texture one pass's binding sets read or write, and the layout that takes. */
struct aprend_pass_image {
	/* The mips and layers the view in the set covers. */
	aprend_texture_range range;
	SPUDGPU_IMAGE_LAYOUT layout;
};

/* Adds the storage buffers [set] holds for frame [frame] to [stores], each
 * once. */
static void aprend_storage_stores_add_set(
    std::vector<aprend_buffer_store *> &stores,
    aprend_binding_set set,
    uint32_t frame) {
	const uint32_t frame_slot          = aprend_binding_set_frame_slot(set, frame);
	aprend_buffer_store **frame_stores = set->storage_stores + (size_t)frame_slot * set->storage_store_capacity;
	for (uint32_t i = 0; i < set->storage_store_counts[frame_slot]; ++i) {
		bool known = false;
		for (aprend_buffer_store *store : stores)
			if (store == frame_stores[i])
				known = true;
		if (!known)
			stores.push_back(frame_stores[i]);
	}
}

/* Adds what [set]'s views cover to [images]. False if any mip of any layer
 * is already there in another layout: sampled through one slot and a storage
 * image through another. Two views of one texture that don't share a mip of
 * a layer are no conflict, which is what lets one mip be read while another
 * is written. */
static bool aprend_pass_images_add_set(
    std::vector<aprend_pass_image> &images,
    aprend_binding_set set) {
	for (uint32_t i = 0; i < set->image_use_count; ++i) {
		const auto &use                  = set->image_uses[i];
		const aprend_texture_range range = aprend_texture_view_range(use.view);

		bool known = false;
		for (const aprend_pass_image &pass_image : images) {
			if (!aprend_texture_ranges_overlap(pass_image.range, range))
				continue;
			if (pass_image.layout != use.layout) {
				printf("aprend: a mip of a texture is in a sampled slot and a storage image slot of the binding sets of one pass or dispatch\n");
				return false;
			}
			// The same range again adds nothing. One that only overlaps is
			// kept: it has subresources the other doesn't.
			if (pass_image.range.base_mip == range.base_mip && pass_image.range.mip_count == range.mip_count &&
			    pass_image.range.base_layer == range.base_layer && pass_image.range.layer_count == range.layer_count)
				known = true;
		}
		if (!known)
			images.push_back({range, use.layout});
	}
	return true;
}

/* The argument buffer of the indirect draw [command], for a list compiled in
 * frame [frame]: its _buffer, or that frame's copy of its _set.
 * aprend_send_command refused a command with neither or both. */
static aprend_storage_buffer aprend_cmd_indirect_arguments(
    const APREND_COMMAND &command,
    uint32_t frame) {
	const auto &p = command._params._draw_indirect;
	return p._set ? p._set->buffers[frame] : p._buffer;
}

/* Moves a multi-state [store] into [state], tracking it in [cmd_list]'s
 * buffer_states. The first time the list uses a store it is in its
 * use_state, which aprend_command_list_submit sees to. Recorded outside any
 * pass. */
static void aprend_cmd_transition_store(
    aprend_command_list cmd_list,
    aprend_buffer_store *store,
    SPUDGPU_RESOURCE_STATE state) {
	aprend_command_list_t::buffer_state *tracked = nullptr;
	for (auto &known : cmd_list->buffer_states)
		if (known.store == store)
			tracked = &known;
	if (!tracked) {
		cmd_list->buffer_states.push_back({store, store->use_state});
		tracked = &cmd_list->buffer_states.back();
	}
	if (tracked->state == state)
		return;
	spudgpu_buffer_barrier barrier{store->buffer, tracked->state, state};
	spudgpu_cmd_pipeline_barrier(cmd_list->cmd_list, &barrier, 1, nullptr, 0);
	tracked->state = state;
}

/* Makes what a pass's or a dispatch's binding sets hold ready to be read:
 * each texture in [images] is moved into the layout its slot reads it in,
 * and every storage image among them and every storage buffer in [stores] is
 * ordered after the storage writes made before this point, since a shader of
 * an earlier dispatch or draw may have written it. Reads after reads need no
 * barrier, so a sampled texture already in its layout gets none.
 *
 * The ordering is SpudGPU's barrier that moves nothing: from GENERAL to
 * GENERAL over the range for an image, from UNORDERED_ACCESS to
 * UNORDERED_ACCESS for a buffer (spudgpu.h). Recording it is the caller's,
 * and Aprend can't know which of these a shader wrote, so it records it for
 * all of them. Recorded outside any pass. */
static void aprend_cmd_ready_set_resources(
    aprend_command_list cmd_list,
    const std::vector<aprend_pass_image> &images,
    const std::vector<aprend_buffer_store *> &stores) {
	for (const aprend_pass_image &pass_image : images) {
		// GENERAL is the layout of a storage image slot. Such a range gets
		// its barrier even when it is there already: from GENERAL to GENERAL
		// is SpudGPU's ordering barrier for a storage image, and it is over
		// the range, so the texture's other mips are left where they are. A
		// sampled range already in its layout gets none.
		const bool is_storage = pass_image.layout == SPUDGPU_IMAGE_LAYOUT_GENERAL;
		aprend_cmd_transition_range(cmd_list, pass_image.range, pass_image.layout, !is_storage);
	}
	std::vector<spudgpu_buffer_barrier> buffer_barriers;
	for (aprend_buffer_store *store : stores) {
		// One that was last an indirect argument buffer comes back to the
		// state a storage slot uses it in.
		if (store->multi_state)
			aprend_cmd_transition_store(cmd_list, store, store->use_state);
		buffer_barriers.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS});
	}
	if (buffer_barriers.empty())
		return;
	spudgpu_cmd_pipeline_barrier(cmd_list->cmd_list, buffer_barriers.data(), (uint32_t)buffer_barriers.size(), nullptr, 0);
}

/* Before the BEGIN_RENDERING at [begin_index] opens its pass: moves every
 * texture of the pass's binding sets into the layout its slot reads it in,
 * since a barrier can't be issued once the pass is open. The pass's sets are
 * the ones a draw in it reads: the pass is walked as the compile will walk
 * it, from the sets in [slots] now, and at each draw the set in every slot
 * the pipeline bound then declares is taken. A set that is only left in a
 * slot, with no draw reading that slot, isn't the pass's.
 * Assumes aprend_begin_rendering_valid passed. */
static bool aprend_cmd_prepare_pass_images(
    aprend_command_list cmd_list,
    size_t begin_index,
    const aprend_binding_set *slots) {
	std::vector<aprend_pass_image> images;
	std::vector<aprend_buffer_store *> stores;
	/* The argument buffers of the pass's indirect draws, each once. */
	std::vector<aprend_buffer_store *> argument_stores;
	aprend_binding_set pass_slots[APREND_MAX_BINDING_LAYOUTS];
	for (uint32_t slot = 0; slot < APREND_MAX_BINDING_LAYOUTS; ++slot)
		pass_slots[slot] = slots[slot];
	aprend_graphics_pipeline pass_pipeline = nullptr;
	bool pass_ended                        = false;
	for (size_t i = begin_index + 1; i < cmd_list->commands.size() && !pass_ended; ++i) {
		const APREND_COMMAND &command = cmd_list->commands[i];
		switch (command._type) {
		case APREND_COMMAND_END_RENDERING:
			pass_ended = true;
			break;
		case APREND_COMMAND_SET_SHADER_PIPELINE:
			// A NULL pipeline is refused when the command itself is reached.
			pass_pipeline = command._params._set_shader_pipeline._pipeline;
			break;
		case APREND_COMMAND_SET_BINDING_SET:
			// As is a slot out of range. A NULL set is refused there too, and
			// until then leaves the slot empty here.
			if (command._params._set_binding_set._slot < APREND_MAX_BINDING_LAYOUTS)
				pass_slots[command._params._set_binding_set._slot] = command._params._set_binding_set._set;
			break;
		case APREND_COMMAND_SET_MATERIAL:
			// The material's set was built before the compile began, and its
			// slot was checked when the command was sent.
			pass_slots[command._params._set_material._slot] = command._params._set_material._material->set;
			break;
		case APREND_COMMAND_DRAW:
		case APREND_COMMAND_DRAW_INDEXED:
		case APREND_COMMAND_DRAW_INSTANCED:
		case APREND_COMMAND_DRAW_INSTANCED_INDEXED:
		case APREND_COMMAND_DRAW_INDIRECT:
		case APREND_COMMAND_DRAW_INDEXED_INDIRECT:
		case APREND_COMMAND_DISPATCH_MESH:
			if (command._type == APREND_COMMAND_DRAW_INDIRECT || command._type == APREND_COMMAND_DRAW_INDEXED_INDIRECT) {
				aprend_buffer_store *arguments = &aprend_cmd_indirect_arguments(command, cmd_list->compiled_frame)->store;
				bool known                     = false;
				for (aprend_buffer_store *store : argument_stores)
					if (store == arguments)
						known = true;
				if (!known)
					argument_stores.push_back(arguments);
			}
			// A draw with no pipeline, or with an empty slot, fails the
			// compile when it is reached.
			if (!pass_pipeline)
				break;
			for (uint32_t slot = 0; slot < pass_pipeline->desc._binding_layout_count; ++slot) {
				if (!pass_slots[slot])
					continue;
				if (!aprend_pass_images_add_set(images, pass_slots[slot]))
					return false;
				aprend_storage_stores_add_set(stores, pass_slots[slot], cmd_list->compiled_frame);
			}
			break;
		default:
			break;
		}
	}

	// A texture can't be read through a set and rendered to at once.
	const auto &p = cmd_list->commands[begin_index]._params._begin_rendering;
	// The same mip of the same layer, that is: another mip of the target's
	// texture may be in a set, which is how a mip is made from the one above.
	for (const aprend_pass_image &pass_image : images) {
		for (uint32_t i = 0; i < p._color_target_count; ++i) {
			if (aprend_texture_ranges_overlap(aprend_texture_view_range(p._color_targets[i].view), pass_image.range)) {
				printf("aprend: color target %u of a pass is also in one of the pass's binding sets\n", i);
				return false;
			}
		}
		if (p._depth_target.view && aprend_texture_ranges_overlap(aprend_texture_view_range(p._depth_target.view), pass_image.range)) {
			printf("aprend: the depth target of a pass is also in one of the pass's binding sets\n");
			return false;
		}
	}

	// A buffer is in one state for the whole pass: it can't be read as draw
	// arguments and be in a storage slot the pass's draws read.
	for (aprend_buffer_store *arguments : argument_stores) {
		for (aprend_buffer_store *store : stores) {
			if (store == arguments) {
				printf("aprend: the argument buffer of an indirect draw is also in one of the pass's binding sets\n");
				return false;
			}
		}
	}

	aprend_cmd_ready_set_resources(cmd_list, images, stores);
	// After the sets, so a buffer a set brought back to a storage slot's
	// state in an earlier pass ends this one's preparation as arguments.
	for (aprend_buffer_store *arguments : argument_stores)
		aprend_cmd_transition_store(cmd_list, arguments, SPUDGPU_RESOURCE_STATE_INDIRECT_ARGUMENT);
	return true;
}

/* Before a dispatch: checks the set in each slot [pipeline] declares, makes
 * what those sets hold ready (aprend_cmd_ready_set_resources) and binds
 * each set that isn't the one already bound there. There is no pass to move
 * the textures ahead of, so this is done right at the dispatch. False if a
 * slot holds no set or one of another layout, or a texture is in a sampled
 * slot and a storage image slot of these sets. */
static bool aprend_cmd_prepare_dispatch(
    aprend_command_list cmd_list,
    aprend_compute_pipeline pipeline,
    const aprend_binding_set *slots,
    aprend_binding_set *applied) {
	std::vector<aprend_pass_image> images;
	std::vector<aprend_buffer_store *> stores;
	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		aprend_binding_set set = slots[slot];
		if (!set) {
			printf("aprend: dispatch with a pipeline that declares set %u, but no SET_BINDING_SET has put a set there\n", slot);
			return false;
		}
		if (!aprend_binding_set_fits(set, pipeline->desc._binding_layouts[slot])) {
			printf("aprend: dispatch with a set at slot %u that isn't of the layout the pipeline declares there\n", slot);
			return false;
		}
		if (!aprend_pass_images_add_set(images, set))
			return false;
		aprend_storage_stores_add_set(stores, set, cmd_list->compiled_frame);
	}
	aprend_cmd_ready_set_resources(cmd_list, images, stores);

	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		aprend_binding_set set = slots[slot];
		if (applied[slot] == set)
			continue;
		// Checked as it is bound for this pipeline: the views the set holds
		// against the shapes the shader declares.
		if (!aprend_binding_set_matches_images(set, slot, pipeline->images, "dispatch"))
			return false;
		spudgpu_cmd_bind_descriptor_sets_compute(
		    cmd_list->cmd_list, pipeline->pipeline, slot, &set->sets[aprend_binding_set_frame_slot(set, cmd_list->compiled_frame)], 1);
		applied[slot] = set;
	}
	return true;
}

/* Before a draw: binds the set in each slot [pipeline] declares, where it
 * isn't the one already bound there. False if a slot holds no set, or one of
 * another layout. */
/* Before a draw: every vertex buffer slot [pipeline] reads holds a buffer or
 * a transient span of the stride its binding has. [strides] is the stride
 * of what each slot holds, 0 for an empty one. */
static bool aprend_cmd_vertex_buffers_valid(
    aprend_graphics_pipeline pipeline,
    const uint32_t *strides) {
	for (uint32_t slot = 0; slot < pipeline->desc._vertex_binding_count; ++slot) {
		if (strides[slot] == 0) {
			printf("aprend: draw with a pipeline that reads vertex buffer slot %u, and nothing has been put there\n", slot);
			return false;
		}
		if (strides[slot] != pipeline->vertex_strides[slot]) {
			printf("aprend: draw with vertices of stride %u at slot %u, where the pipeline's binding has stride %u\n", strides[slot], slot,
			       pipeline->vertex_strides[slot]);
			return false;
		}
	}
	return true;
}

static bool aprend_cmd_apply_binding_sets(
    spudgpu_command_list cmd,
    aprend_graphics_pipeline pipeline,
    const aprend_binding_set *slots,
    aprend_binding_set *applied,
    uint32_t frame) {
	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		aprend_binding_set set = slots[slot];
		if (!set) {
			printf("aprend: draw with a pipeline that declares set %u, but no SET_BINDING_SET has put a set there\n", slot);
			return false;
		}
		if (!aprend_binding_set_fits(set, pipeline->desc._binding_layouts[slot])) {
			printf("aprend: draw with a set at slot %u that isn't of the layout the pipeline declares there\n", slot);
			return false;
		}
		if (applied[slot] == set)
			continue;
		if (!aprend_binding_set_matches_images(set, slot, pipeline->images, "draw"))
			return false;
		spudgpu_cmd_bind_descriptor_sets(cmd, pipeline->pipeline, slot, &set->sets[aprend_binding_set_frame_slot(set, frame)], 1);
		applied[slot] = set;
	}
	return true;
}

/* SET_BINDING_SET, and SET_MATERIAL with its material's set: puts [set] in
 * [slot] of [slot_sets] and takes the storage buffers its frame's
 * descriptor set holds. False if [set] is NULL or [slot] is out of range. */
static bool aprend_cmd_put_binding_set(
    aprend_command_list cmd_list,
    uint32_t slot,
    aprend_binding_set set,
    aprend_binding_set *slot_sets) {
	if (!set) {
		printf("aprend: SET_BINDING_SET with a NULL set\n");
		return false;
	}
	if (slot >= APREND_MAX_BINDING_LAYOUTS) {
		printf("aprend: SET_BINDING_SET with _slot %u, at or above APREND_MAX_BINDING_LAYOUTS\n", slot);
		return false;
	}
	slot_sets[slot] = set;
	// The stores of the descriptor set this frame binds. Taken whether or not
	// a draw reads the slot: copying a buffer nothing reads yet is harmless.
	const uint32_t frame_slot          = aprend_binding_set_frame_slot(set, cmd_list->compiled_frame);
	aprend_buffer_store **frame_stores = set->storage_stores + (size_t)frame_slot * set->storage_store_capacity;
	for (uint32_t i = 0; i < set->storage_store_counts[frame_slot]; ++i)
		aprend_cmd_use_store(cmd_list, frame_stores[i]);
	return true;
}

/* GENERATE_MIPS, outside a pass: for each layer, the base level into
 * SHADER_READ_ONLY (nothing if a read left it there), then each level below
 * rendered from the one above and moved to SHADER_READ_ONLY in turn, all
 * through the list's own layout tracking. False, printed, if the base level
 * of a layer has nothing in it as the list reaches the command, or the
 * texture's chain or pipeline can't be made. */
static bool aprend_cmd_generate_mips(
    aprend_command_list cmd_list,
    aprend_texture2d texture,
    uint32_t base_mip,
    SPUDGPU_FILTER filter) {
	// Checked again: a resize since the command was sent may have changed
	// the texture's levels.
	if (!aprend_generate_mips_valid(texture, base_mip, filter, "aprend: GENERATE_MIPS"))
		return false;
	const uint32_t layer_count = texture->desc.array_layers;
	const uint32_t last_mip    = texture->desc.mip_levels - 1;
	aprend_texture_range one{};
	one.image       = texture->image;
	one.tracking    = &texture->tracking;
	one.mip_count   = 1;
	one.layer_count = 1;
	for (uint32_t layer = 0; layer < layer_count; ++layer) {
		const size_t index = aprend_cmd_layout_use(cmd_list, one, base_mip, layer);
		if (cmd_list->layout_uses[index].final_layout == SPUDGPU_IMAGE_LAYOUT_UNDEFINED) {
			printf("aprend: GENERATE_MIPS: level %u of layer %u has nothing in it, from before the list or earlier in it\n", base_mip, layer);
			return false;
		}
	}
	if (base_mip == last_mip)
		return true;

	const aprend_mip_chain *chain     = nullptr;
	aprend_graphics_pipeline pipeline = nullptr;
	if (!aprend_texture2d_mip_chain(texture, base_mip, filter, &chain, &pipeline))
		return false;
	const uint32_t levels = last_mip - base_mip;
	for (uint32_t layer = 0; layer < layer_count; ++layer) {
		one.base_layer = layer;
		one.base_mip   = base_mip;
		aprend_cmd_transition_range(cmd_list, one, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
		for (uint32_t i = 0; i < levels; ++i) {
			const uint32_t mip = base_mip + i + 1;
			one.base_mip       = mip;
			// A barrier whether or not it is there already, as for a pass's
			// targets.
			aprend_cmd_transition_range(cmd_list, one, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
			aprend_record_mip_pass(cmd_list->cmd_list, pipeline, chain->steps[(size_t)layer * levels + i], aprend_mip_size(texture->desc.width, mip),
			                       aprend_mip_size(texture->desc.height, mip));
			aprend_cmd_transition_range(cmd_list, one, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, false);
		}
	}
	return true;
}

bool aprend_command_list_compile(aprend_command_list cmd_list) {
	if (!cmd_list)
		return false;
	cmd_list->compiled = false;
	// A recording of this frame that was never submitted is recorded over;
	// otherwise a list of this frame that nothing is running.
	aprend_instance instance = cmd_list->instance;
	if (!cmd_list->cmd_list || cmd_list->compiled_frame_number != instance->frame_number) {
		cmd_list->cmd_list = nullptr;
		SPUDRESULT sr      = aprend_instance_take_command_list(instance, &cmd_list->cmd_list);
		if (SPUDFAIL(sr)) {
			printf("aprend: compile: no SpudGPU command list to record into: %s\n", spudresult_str(sr));
			cmd_list->cmd_list = nullptr;
			return false;
		}
	}
	cmd_list->layout_uses.clear();
	cmd_list->staged_stores.clear();
	cmd_list->buffer_states.clear();
	cmd_list->present_swap_chain = nullptr;
	// Every buffer set this compile reads is read at this frame's copy, and
	// it is recorded into this frame's allocator.
	cmd_list->compiled_frame        = instance->frame_index;
	cmd_list->compiled_frame_number = instance->frame_number;
	cmd_list->timer_uses.clear();
	cmd_list->materials.clear();
	if (cmd_list->recording_error)
		return false;

	// Every material's binding set is built before anything is recorded: a
	// pass looks ahead at the sets its draws read (aprend_cmd_prepare_pass_
	// images), and a SET_MATERIAL inside it has to have one by then.
	try {
		for (const APREND_COMMAND &command : cmd_list->commands) {
			if (command._type != APREND_COMMAND_SET_MATERIAL)
				continue;
			aprend_material material = command._params._set_material._material;
			bool known               = false;
			for (aprend_material listed : cmd_list->materials)
				if (listed == material)
					known = true;
			if (known)
				continue;
			if (!aprend_material_prepare(material)) {
				cmd_list->materials.clear();
				return false;
			}
			cmd_list->materials.push_back(material);
		}
	} catch (const std::bad_alloc &) {
		printf("aprend: compile: out of memory\n");
		cmd_list->materials.clear();
		return false;
	}

	spudgpu_command_list cmd = cmd_list->cmd_list;
	spudgpu_begin_command_list(cmd);

	/* Whether a BEGIN_RENDERING pass is currently open. */
	bool in_pass = false;
	/* The BEGIN_RENDERING of the open pass, which every pipeline set in it
	 * must match, and which may time the pass into a timer. */
	const APREND_COMMAND *open_pass = nullptr;
	/* The open labelled regions, innermost last: whether each was opened
	 * inside a pass. */
	std::vector<bool> open_labels;
	bool ok      = true;
	/* The pipeline bound in the open pass; none until the pass sets one. */
	aprend_graphics_pipeline bound_pipeline = nullptr;
	/* The stride of the buffer or transient span last put in each vertex
	 * buffer slot; 0 for a slot nothing has been put in. */
	uint32_t slot_vertex_strides[APREND_MAX_VERTEX_BINDINGS]{};
	/* Whether an index buffer or a transient index span has been set. */
	bool index_buffer_bound = false;
	/* The set SET_BINDING_SET last put in each slot, and the one actually
	 * bound there for the bound pipeline in the open pass. A draw binds
	 * whatever differs. */
	aprend_binding_set slot_sets[APREND_MAX_BINDING_LAYOUTS]{};
	aprend_binding_set applied_sets[APREND_MAX_BINDING_LAYOUTS]{};
	/* The compute pipeline set outside a pass, until the next pass or
	 * present, and the set bound for it in each slot. */
	aprend_compute_pipeline bound_compute_pipeline = nullptr;
	aprend_binding_set applied_compute_sets[APREND_MAX_BINDING_LAYOUTS]{};

	std::vector<spudgpu_buffer_view> vertex_buffer_views;

	for (size_t command_index = 0; command_index < cmd_list->commands.size(); ++command_index) {
		const APREND_COMMAND &command = cmd_list->commands[command_index];
		switch (command._type) {
		case APREND_COMMAND_DRAW:
		case APREND_COMMAND_DRAW_INDEXED:
		case APREND_COMMAND_DRAW_INSTANCED:
		case APREND_COMMAND_DRAW_INSTANCED_INDEXED:
		case APREND_COMMAND_DRAW_INDIRECT:
		case APREND_COMMAND_DRAW_INDEXED_INDIRECT:
		case APREND_COMMAND_DISPATCH_MESH:
			if (!in_pass) {
				printf("aprend: draw command recorded outside a rendering pass\n");
				ok = false;
			} else if (!bound_pipeline) {
				printf("aprend: draw command in a pass that hasn't set a pipeline\n");
				ok = false;
			} else if (bound_pipeline->is_mesh && command._type != APREND_COMMAND_DISPATCH_MESH) {
				printf("aprend: draw command with a mesh shader pipeline, which is run with DISPATCH_MESH\n");
				ok = false;
			} else if (!bound_pipeline->is_mesh && command._type == APREND_COMMAND_DISPATCH_MESH) {
				printf("aprend: DISPATCH_MESH with a pipeline that has no mesh shader\n");
				ok = false;
			} else if (!aprend_cmd_vertex_buffers_valid(bound_pipeline, slot_vertex_strides)) {
				ok = false;
			} else if (!index_buffer_bound && (command._type == APREND_COMMAND_DRAW_INDEXED ||
			                                   command._type == APREND_COMMAND_DRAW_INSTANCED_INDEXED ||
			                                   command._type == APREND_COMMAND_DRAW_INDEXED_INDIRECT)) {
				printf("aprend: indexed draw with no SET_INDEX_BUFFER or SET_TRANSIENT_INDEX_BUFFER before it\n");
				ok = false;
			} else {
				ok = aprend_cmd_apply_binding_sets(cmd, bound_pipeline, slot_sets, applied_sets, cmd_list->compiled_frame);
			}
			break;
		default:
			break;
		}
		if (!ok)
			break;

		switch (command._type) {
		case APREND_COMMAND_SET_VERTEX_BUFFERS: {
			const auto &p = command._params._set_vertex_buffers;
			vertex_buffer_views.clear();
			vertex_buffer_views.reserve(p._vertex_buffer_count);
			for (uint32_t i = 0; i < p._vertex_buffer_count; ++i) {
				vertex_buffer_views.push_back(p._vertex_buffers[i]->buffer_view);
				// aprend_send_command refused a command that runs past the last slot.
				slot_vertex_strides[p._start_slot + i] = p._vertex_buffers[i]->vertex_stride;
				aprend_cmd_use_store(cmd_list, &p._vertex_buffers[i]->store);
			}
			spudgpu_cmd_set_vertex_buffers(cmd, p._start_slot, p._vertex_buffer_count, vertex_buffer_views.data());
			break;
		}
		case APREND_COMMAND_SET_INDEX_BUFFER: {
			const auto &p = command._params._set_index_buffer;
			if (!p._index_buffer) {
				printf("aprend: SET_INDEX_BUFFER with a NULL buffer\n");
				ok = false;
				break;
			}
			spudgpu_cmd_set_index_buffer(cmd, p._index_buffer->buffer_view);
			index_buffer_bound = true;
			aprend_cmd_use_store(cmd_list, &p._index_buffer->store);
			break;
		}
		case APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER: {
			const auto &p = command._params._set_transient_vertex_buffer;
			if (!aprend_transient_span_valid(cmd_list->instance, &p._span, "SET_TRANSIENT_VERTEX_BUFFER")) {
				ok = false;
				break;
			}
			spudgpu_buffer_view view = nullptr;
			SPUDRESULT sr            = aprend_transient_span_view(cmd_list->instance, &p._span, p._stride, &view);
			if (SPUDFAIL(sr)) {
				printf("aprend: SET_TRANSIENT_VERTEX_BUFFER: a view of the span can't be created: %s\n", spudresult_str(sr));
				ok = false;
				break;
			}
			spudgpu_cmd_set_vertex_buffers(cmd, p._slot, 1, &view);
			// aprend_send_command refused a slot past the last.
			slot_vertex_strides[p._slot] = p._stride;
			break;
		}
		case APREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER: {
			const auto &p = command._params._set_transient_index_buffer;
			if (!aprend_transient_span_valid(cmd_list->instance, &p._span, "SET_TRANSIENT_INDEX_BUFFER")) {
				ok = false;
				break;
			}
			if (p._span._offset % p._index_size != 0) {
				printf("aprend: SET_TRANSIENT_INDEX_BUFFER with a span not aligned to its _index_size of %u; allocate it with that alignment\n",
				       p._index_size);
				ok = false;
				break;
			}
			spudgpu_buffer_view view = nullptr;
			SPUDRESULT sr            = aprend_transient_span_view(cmd_list->instance, &p._span, p._index_size, &view);
			if (SPUDFAIL(sr)) {
				printf("aprend: SET_TRANSIENT_INDEX_BUFFER: a view of the span can't be created: %s\n", spudresult_str(sr));
				ok = false;
				break;
			}
			spudgpu_cmd_set_index_buffer(cmd, view);
			index_buffer_bound = true;
			break;
		}
		case APREND_COMMAND_SET_SHADER_PIPELINE: {
			const auto &p = command._params._set_shader_pipeline;
			if (!p._pipeline) {
				printf("aprend: SET_SHADER_PIPELINE with a NULL pipeline\n");
				ok = false;
				break;
			}
			if (!in_pass) {
				printf("aprend: SET_SHADER_PIPELINE outside a rendering pass\n");
				ok = false;
				break;
			}
			if (!aprend_pipeline_matches_pass(p._pipeline, *open_pass)) {
				ok = false;
				break;
			}
			spudgpu_cmd_bind_pipeline(cmd, p._pipeline->pipeline);
			bound_pipeline = p._pipeline;
			// Not guaranteed to survive a pipeline change on every backend:
			// the next draw binds them again.
			for (aprend_binding_set &applied : applied_sets)
				applied = nullptr;
			break;
		}
		case APREND_COMMAND_SET_BINDING_SET: {
			const auto &p = command._params._set_binding_set;
			ok            = aprend_cmd_put_binding_set(cmd_list, p._slot, p._set, slot_sets);
			break;
		}
		case APREND_COMMAND_SET_MATERIAL: {
			// Its set was built at the top of the compile.
			const auto &p = command._params._set_material;
			ok            = aprend_cmd_put_binding_set(cmd_list, p._slot, p._material->set, slot_sets);
			break;
		}
		case APREND_COMMAND_PUSH_CONSTANTS: {
			const auto &p = command._params._push_constants;
			if (!in_pass) {
				// Outside a pass the block written is the compute pipeline's.
				if (!bound_compute_pipeline) {
					printf("aprend: PUSH_CONSTANTS outside a pass with no compute pipeline set\n");
					ok = false;
					break;
				}
				if (p._offset > bound_compute_pipeline->push_constant_end) {
					printf("aprend: PUSH_CONSTANTS at offset %u, past the compute pipeline's push constant ranges (%u bytes)\n", p._offset,
					       bound_compute_pipeline->push_constant_end);
					ok = false;
					break;
				}
				if (p._size > bound_compute_pipeline->push_constant_end - p._offset) {
					printf("aprend: PUSH_CONSTANTS of %u bytes at offset %u runs past the compute pipeline's push constant ranges (%u bytes)\n",
					       p._size, p._offset, bound_compute_pipeline->push_constant_end);
					ok = false;
					break;
				}
				spudgpu_cmd_push_constants_compute(cmd, bound_compute_pipeline->pipeline, p._offset, p._size, p._data);
				break;
			}
			if (!bound_pipeline) {
				printf("aprend: PUSH_CONSTANTS in a pass that hasn't set a pipeline\n");
				ok = false;
				break;
			}
			if (p._offset > bound_pipeline->push_constant_end) {
				printf("aprend: PUSH_CONSTANTS at offset %u, past the pipeline's push constant ranges (%u bytes)\n", p._offset,
				       bound_pipeline->push_constant_end);
				ok = false;
				break;
			}
			if (p._size > bound_pipeline->push_constant_end - p._offset) {
				printf("aprend: PUSH_CONSTANTS of %u bytes at offset %u runs past the pipeline's push constant ranges (%u bytes)\n", p._size,
				       p._offset, bound_pipeline->push_constant_end);
				ok = false;
				break;
			}
			spudgpu_cmd_push_constants(cmd, bound_pipeline->pipeline, p._offset, p._size, p._data);
			break;
		}
		case APREND_COMMAND_SET_COMPUTE_PIPELINE: {
			const auto &p = command._params._set_compute_pipeline;
			if (!p._pipeline) {
				printf("aprend: SET_COMPUTE_PIPELINE with a NULL pipeline\n");
				ok = false;
				break;
			}
			if (in_pass) {
				printf("aprend: SET_COMPUTE_PIPELINE inside a rendering pass\n");
				ok = false;
				break;
			}
			spudgpu_cmd_bind_compute_pipeline(cmd, p._pipeline->pipeline);
			bound_compute_pipeline = p._pipeline;
			// Not guaranteed to survive a pipeline change on every backend:
			// the next dispatch binds them again.
			for (aprend_binding_set &applied : applied_compute_sets)
				applied = nullptr;
			break;
		}
		case APREND_COMMAND_DISPATCH: {
			const auto &p = command._params._dispatch;
			if (in_pass) {
				printf("aprend: DISPATCH inside a rendering pass\n");
				ok = false;
				break;
			}
			if (!bound_compute_pipeline) {
				printf("aprend: DISPATCH with no compute pipeline set since the last pass or present\n");
				ok = false;
				break;
			}
			if (!aprend_cmd_prepare_dispatch(cmd_list, bound_compute_pipeline, slot_sets, applied_compute_sets)) {
				ok = false;
				break;
			}
			spudgpu_cmd_dispatch(cmd, p._group_count_x, p._group_count_y, p._group_count_z);
			break;
		}
		case APREND_COMMAND_PRESENT_TEXTURE: {
			if (in_pass) {
				printf("aprend: PRESENT_TEXTURE inside a rendering pass\n");
				ok = false;
				break;
			}
			ok = aprend_cmd_present_texture(cmd_list, command);
			// The copy ends compute recording on a backend that keeps it
			// apart from copies: a compute pipeline is set again after it.
			bound_compute_pipeline = nullptr;
			break;
		}
		case APREND_COMMAND_DRAW: {
			const auto &p = command._params._draw;
			spudgpu_cmd_draw(cmd, p._vertex_count, p._start_vertex_location);
			break;
		}
		case APREND_COMMAND_DRAW_INDEXED: {
			const auto &p = command._params._draw_indexed;
			spudgpu_cmd_draw_indexed(cmd, p._index_count, p._start_index_location, p._base_vertex_location);
			break;
		}
		case APREND_COMMAND_DRAW_INSTANCED: {
			const auto &p = command._params._draw_instanced;
			spudgpu_cmd_draw_instanced(cmd, p._vertex_count_per_instance, p._instance_count, p._start_vertex_location, p._start_instance_location);
			break;
		}
		case APREND_COMMAND_DRAW_INSTANCED_INDEXED: {
			const auto &p = command._params._draw_indexed_instanced;
			spudgpu_cmd_draw_indexed_instanced(
			    cmd, p._index_count_per_instance, p._instance_count, p._start_index_location, p._base_vertex_location,
			    p._start_instance_location);
			break;
		}
		case APREND_COMMAND_DRAW_INDIRECT: {
			const auto &p                   = command._params._draw_indirect;
			aprend_storage_buffer arguments = aprend_cmd_indirect_arguments(command, cmd_list->compiled_frame);
			// What the CPU wrote to it is copied at submit if it is staged.
			// It was moved to the indirect argument state before the pass.
			aprend_cmd_use_store(cmd_list, &arguments->store);
			// In calls of at most the device's count, at advancing offsets:
			// a SpudGPU call with more records nothing.
			const uint32_t most = cmd_list->instance->max_draw_indirect_count;
			uint64_t offset     = p._offset;
			for (uint32_t left = p._draw_count; left > 0;) {
				const uint32_t count = std::min(most, left);
				spudgpu_cmd_draw_indirect(cmd, arguments->store.buffer, offset, count, p._stride);
				offset += (uint64_t)count * p._stride;
				left -= count;
			}
			break;
		}
		case APREND_COMMAND_DRAW_INDEXED_INDIRECT: {
			const auto &p                   = command._params._draw_indirect;
			aprend_storage_buffer arguments = aprend_cmd_indirect_arguments(command, cmd_list->compiled_frame);
			aprend_cmd_use_store(cmd_list, &arguments->store);
			const uint32_t most = cmd_list->instance->max_draw_indirect_count;
			uint64_t offset     = p._offset;
			for (uint32_t left = p._draw_count; left > 0;) {
				const uint32_t count = std::min(most, left);
				spudgpu_cmd_draw_indexed_indirect(cmd, arguments->store.buffer, offset, count, p._stride);
				offset += (uint64_t)count * p._stride;
				left -= count;
			}
			break;
		}
		case APREND_COMMAND_DISPATCH_MESH: {
#if SPUDGPU_EXT_MESH_SHADING
			const auto &p = command._params._dispatch_mesh;
			spudgpu_cmd_dispatch_mesh(cmd, p._group_count_x, p._group_count_y, p._group_count_z);
#else
			// No mesh shader pipeline can exist in such a build, so the
			// pipeline check above has already refused this.
			ok = false;
#endif
			break;
		}
		case APREND_COMMAND_SET_VIEWPORTS: {
			const auto &p = command._params._set_viewports;
			spudgpu_cmd_set_viewports(cmd, p._first_viewport, p._viewport_count, p._viewports);
			break;
		}
		case APREND_COMMAND_SET_SCISSOR_RECTS: {
			const auto &p = command._params._set_scissor_rects;
			spudgpu_cmd_set_scissor_rects(cmd, p._first_scissor_rect, p._scissor_rect_count, p._scissor_rects);
			break;
		}
		case APREND_COMMAND_BEGIN_RENDERING: {
			if (in_pass) {
				printf("aprend: BEGIN_RENDERING while another rendering pass is open\n");
				ok = false;
				break;
			}
			if (!aprend_begin_rendering_valid(command)) {
				ok = false;
				break;
			}
			if (!aprend_cmd_prepare_pass_images(cmd_list, command_index, slot_sets)) {
				ok = false;
				break;
			}
			if (!aprend_cmd_begin_timed(cmd_list, command)) {
				ok = false;
				break;
			}
			aprend_cmd_begin_rendering(cmd_list, command);
			in_pass   = true;
			open_pass = &command;
			// A pipeline is bound inside a pass and doesn't outlive it, and
			// bound sets aren't guaranteed to carry over from one pass to the
			// next either.
			bound_pipeline = nullptr;
			for (aprend_binding_set &applied : applied_sets)
				applied = nullptr;
			// Nor does a compute pipeline outlive the pass that follows it.
			bound_compute_pipeline = nullptr;
			break;
		}
		case APREND_COMMAND_END_RENDERING: {
			if (!in_pass) {
				printf("aprend: END_RENDERING without an open BEGIN_RENDERING pass\n");
				ok = false;
				break;
			}
			for (bool opened_in_pass : open_labels) {
				if (opened_in_pass) {
					printf("aprend: END_RENDERING with a label opened inside the pass still open\n");
					ok = false;
				}
			}
			if (!ok)
				break;
			spudgpu_cmd_end_rendering(cmd);
			// The pass's times are written; made readable outside it.
			aprend_cmd_end_timed(cmd_list, *open_pass);
			in_pass   = false;
			open_pass = nullptr;
			break;
		}
		case APREND_COMMAND_GENERATE_MIPS: {
			const auto &p = command._params._generate_mips;
			if (in_pass) {
				printf("aprend: GENERATE_MIPS inside a rendering pass\n");
				ok = false;
				break;
			}
			ok = aprend_cmd_generate_mips(cmd_list, p._texture, p._base_mip, p._filter);
			// Its passes bind a pipeline of their own: a compute pipeline is
			// set again before the next dispatch, as after a pass.
			bound_compute_pipeline = nullptr;
			break;
		}
		case APREND_COMMAND_BEGIN_LABEL:
			try {
				open_labels.push_back(in_pass);
			} catch (const std::bad_alloc &) {
				printf("aprend: compile: out of memory\n");
				ok = false;
				break;
			}
			spudgpu_cmd_begin_debug_label(cmd, command._params._begin_label._label);
			break;
		case APREND_COMMAND_END_LABEL:
			if (open_labels.empty()) {
				printf("aprend: END_LABEL with no label open\n");
				ok = false;
				break;
			}
			if (open_labels.back() != in_pass) {
				printf("aprend: END_LABEL %s a pass, and its BEGIN_LABEL was %s one\n", in_pass ? "inside" : "outside",
				       open_labels.back() ? "inside" : "outside");
				ok = false;
				break;
			}
			open_labels.pop_back();
			spudgpu_cmd_end_debug_label(cmd);
			break;
		default:
			break;
		}
		if (!ok)
			break;
	}

	if (ok && in_pass) {
		printf("aprend: BEGIN_RENDERING pass left open at the end of the command list\n");
		ok = false;
	}
	if (ok && !open_labels.empty()) {
		printf("aprend: %zu labels left open at the end of the command list\n", open_labels.size());
		ok = false;
	}

	/* Always leave the spudgpu list closed and balanced, even on failure —
	 * aprend_command_list_submit refuses it when this returns false. The
	 * labels opened inside the pass close before it does, and the rest
	 * after. */
	while (in_pass && !open_labels.empty() && open_labels.back()) {
		spudgpu_cmd_end_debug_label(cmd);
		open_labels.pop_back();
	}
	if (in_pass)
		spudgpu_cmd_end_rendering(cmd);
	while (!open_labels.empty()) {
		spudgpu_cmd_end_debug_label(cmd);
		open_labels.pop_back();
	}

	spudgpu_end_command_list(cmd);
	if (!ok) {
		cmd_list->timer_uses.clear();
		cmd_list->layout_uses.clear();
		cmd_list->staged_stores.clear();
		cmd_list->buffer_states.clear();
		cmd_list->materials.clear();
		cmd_list->present_swap_chain = nullptr;
	}
	cmd_list->compiled = ok;
	return ok;
}

bool aprend_command_list_submit(aprend_command_list cmd_list, spudgpu_command_queue queue) {
	if (!cmd_list || !queue)
		return false;
	if (!cmd_list->compiled) {
		printf("aprend: submit of a command list without a successful compile since its last submit\n");
		return false;
	}
	// Every texture must still be in the layout this list's barriers were
	// compiled against; if another submission moved it since, the barriers
	// would lie to the GPU.
	for (const auto &use : cmd_list->layout_uses) {
		if (*use.tracked != use.initial_layout) {
			printf("aprend: submit refused: a texture's layout changed since this list was compiled; recompile it\n");
			return false;
		}
	}

	// The recording is in the allocator of the frame it was compiled in,
	// which the next turn of that frame index resets. A list that binds a
	// per-frame set or a transient span reads that frame's copies and
	// memory as well.
	if (cmd_list->compiled_frame_number != cmd_list->instance->frame_number) {
		printf("aprend: submit refused: the list was compiled in an earlier frame (aprend_instance_next_frame has been called since); recompile it\n");
		return false;
	}

	aprend_swap_chain sc = cmd_list->present_swap_chain;
	if (sc) {
		// The list copies into one specific acquired back buffer; it's only
		// valid while that same one is still acquired and unsubmitted.
		if (sc->acquired_image != cmd_list->present_image_index || sc->submitted) {
			printf("aprend: submit refused: the back buffer this list presents into is no longer the acquired one; recompile it\n");
			return false;
		}
		if (queue != sc->desc.queue) {
			printf("aprend: submit refused: a list presenting into a swap chain must go on that swap chain's queue\n");
			return false;
		}
	}

	// Whatever was written to the list's staged buffers since they were last
	// copied goes across first, in the same submission, so the list's own
	// commands read what the caller last wrote; and so do the uniform blocks
	// of its materials that changed since they were last copied, through
	// this frame's transient memory.
	spudgpu_command_list upload = nullptr;
	std::vector<aprend_upload_copy> copies;
	try {
		for (aprend_material material : cmd_list->materials)
			if (!aprend_material_stage_uploads(material, copies))
				return false;
		SPUDRESULT sr = aprend_cmd_record_uploads(cmd_list, copies, &upload);
		if (SPUDFAIL(sr)) {
			printf("aprend: submit: no SpudGPU command list for the list's uploads: %s\n", spudresult_str(sr));
			return false;
		}
	} catch (const std::bad_alloc &) {
		printf("aprend: submit: out of memory\n");
		return false;
	}

	spudgpu_command_list lists[APREND_SUBMIT_MAX_LISTS];
	uint32_t list_count = 0;
	if (upload)
		lists[list_count++] = upload;
	lists[list_count++] = cmd_list->cmd_list;

	// Every submission signals the instance's frame fence to the next
	// serial, so aprend_instance_next_frame can wait for the last one of a
	// frame without being told which that was. With a swap chain the
	// submission also waits for the acquire to finish and signals
	// presentation.
	if (SPUDFAIL(aprend_instance_submit(cmd_list->instance, queue, lists, list_count, sc ? sc->swap_chain : nullptr, nullptr)))
		return false;
	if (sc)
		sc->submitted = true;

	// Copied only now that the submission is in: a failed one leaves the
	// ranges dirty for the next.
	for (aprend_buffer_store *store : cmd_list->staged_stores) {
		store->dirty_begin = 0;
		store->dirty_end   = 0;
	}
	for (const aprend_upload_copy &copy : copies)
		*copy.dirty = false;

	for (const auto &use : cmd_list->layout_uses)
		*use.tracked = use.final_layout;

	// Each pass timed in this submission is the slot's time for this frame.
	for (const auto &use : cmd_list->timer_uses)
		use.timer->written_frame[(size_t)cmd_list->compiled_frame * use.timer->slot_count + use.slot] = cmd_list->compiled_frame_number;

	// A compile is one recording of the SpudGPU list, and SpudGPU submits a
	// recording once. The recording may now be running, so the next compile
	// records into another list.
	cmd_list->compiled = false;
	cmd_list->cmd_list = nullptr;
	return true;
}
}
