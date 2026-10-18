
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"
#include "aprendlit_internal.hpp"

#include <cmath>
#include <cstdio>

aprend_command_list_t::~aprend_command_list_t() {
	// The SpudGPU lists it recorded into are the instance's.
	commands.clear();
}

/* Adds [store] to the stores [cmd_list] uses, and to its staged stores if
 * it is staged, once each. A store that is written directly has nothing to
 * copy. Throws std::bad_alloc. */
static void aprend_cmd_use_store(
    aprend_command_list cmd_list,
    aprend_buffer_store *store) {
	for (aprend_buffer_store *known : cmd_list->used_stores)
		if (known == store)
			return;
	cmd_list->used_stores.push_back(store);
	if (store->staged)
		cmd_list->staged_stores.push_back(store);
}

/* The most levels a bloom of [source] spreads through: its level 0 is half
 * the size of the source's first mip, and each level halves again down to
 * 1 texel. */
static uint32_t aprend_bloom_max_levels(aprend_texture_view source) {
	const aprend_texture2d texture = source->texture._t2d;
	const uint32_t width           = aprend_mip_size(aprend_mip_size(texture->desc.width, source->desc.base_mip), 1);
	const uint32_t height          = aprend_mip_size(aprend_mip_size(texture->desc.height, source->desc.base_mip), 1);
	uint32_t side                  = width > height ? width : height;
	uint32_t levels                = 1;
	while (side > 1) {
		side >>= 1;
		++levels;
	}
	return levels;
}

/* Records into an upload list, taken from the instance's current frame,
 * what has to happen ahead of the list's own commands: a copy of each upload
 * of every staged store the list uses, each buffer moved into COPY_DEST for the copy and into the state
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
		if (store->uploads.empty())
			continue;
		before.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST});
		after.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_COPY_DEST, store->use_state});
	}
	// A store whose state the list's barriers track must start the list in
	// its use_state, which the copy above leaves it in. One with nothing to
	// copy is put there directly, so the list starts the same either way.
	for (const auto &tracked : cmd_list->buffer_states) {
		aprend_buffer_store *store = tracked.store;
		if (store->staged && !store->uploads.empty())
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
	// In each store's order: a ONCE store's uploads may overlap, and the
	// later write is the one that has to land.
	for (aprend_buffer_store *store : cmd_list->staged_stores)
		for (const aprend_buffer_upload &staged : store->uploads)
			spudgpu_cmd_copy_buffer(upload, staged.source, store->buffer, staged.source_offset, staged.offset, staged.size);
	for (const aprend_upload_copy &copy : copies)
		spudgpu_cmd_copy_buffer(upload, copy.source, copy.destination, copy.source_offset, copy.destination_offset, copy.size);
	if (!after.empty())
		spudgpu_cmd_pipeline_barrier(upload, after.data(), (uint32_t)after.size(), nullptr, 0);
	spudgpu_end_command_list(upload);
	*out_upload = upload;
	return SPUD_SUCCESS;
}

/* Takes [cmd_list] off its instance's compiled lists, if it is on them, and
 * clears its stale mark. Before anything changes what the list uses. */
static void aprend_cmd_list_unlist(aprend_command_list cmd_list) {
	aprend_instance instance = cmd_list->instance;
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	cmd_list->stale = false;
	if (!cmd_list->listed)
		return;
	cmd_list->listed = false;
	std::vector<aprend_command_list> &lists = instance->compiled_lists;
	for (size_t i = 0; i < lists.size(); ++i) {
		if (lists[i] != cmd_list)
			continue;
		lists[i] = lists.back();
		lists.pop_back();
		return;
	}
}

/* Puts [cmd_list], just compiled, on its instance's compiled lists. False,
 * printed, with no memory for it. */
static bool aprend_cmd_list_enlist(aprend_command_list cmd_list) {
	aprend_instance instance = cmd_list->instance;
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	try {
		instance->compiled_lists.push_back(cmd_list);
	} catch (const std::bad_alloc &) {
		printf("aprend: compile: out of memory\n");
		return false;
	}
	cmd_list->listed = true;
	cmd_list->stale  = false;
	return true;
}

/* Whether the compile of [cmd_list] reads [used] at submit. */
static bool aprend_cmd_list_uses(aprend_command_list cmd_list, const void *used) {
	for (const auto &use : cmd_list->layout_uses)
		if (use.tracking == used)
			return true;
	for (const aprend_buffer_store *store : cmd_list->used_stores)
		if (store == used)
			return true;
	for (const auto &tracked : cmd_list->buffer_states)
		if (tracked.store == used)
			return true;
	for (aprend_material material : cmd_list->materials)
		if (material == used)
			return true;
	for (const auto &timer_use : cmd_list->timer_uses)
		if (timer_use.timer == used)
			return true;
	return false;
}

void aprend_instance_forget(
    aprend_instance instance,
    const void *used) {
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	for (aprend_command_list listed : instance->compiled_lists)
		if (!listed->stale && aprend_cmd_list_uses(listed, used))
			listed->stale = true;
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
	// A compile it never submitted gives back what it took from the render
	// target pool. Here and not in its release, which may come after the
	// instance's destructor has freed the pool.
	aprend_render_target_give_back(cmd_list);
	// No destroy call may find it among the compiled lists once it is gone.
	aprend_cmd_list_unlist(cmd_list);
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(cmd_list->instance, cmd_list, &aprend_release_handle<aprend_command_list_t>);
}

void aprend_command_list_reset(aprend_command_list cmd_list) {
	if (!cmd_list)
		return;
	// Off the compiled lists before what it uses is cleared: a destroy call
	// on another thread may be reading it.
	aprend_cmd_list_unlist(cmd_list);
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
	cmd_list->storage_hazards.clear();
	cmd_list->staged_stores.clear();
	cmd_list->used_stores.clear();
	cmd_list->buffer_states.clear();
	cmd_list->materials.clear();
	aprend_render_target_give_back(cmd_list);
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
	if (cmd._type == APREND_COMMAND_BLOOM) {
		const auto &p = cmd._params._bloom;
		if (!p._source) {
			printf("aprend: BLOOM with a NULL source\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->instance != cmd_list->instance) {
			printf("aprend: BLOOM with a source of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			printf("aprend: BLOOM with a source that is not a SHADER_RESOURCE view\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			printf("aprend: BLOOM with a source that is not a 2D view\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR) {
			printf("aprend: BLOOM with a source that is not a view of color\n");
			cmd_list->recording_error = true;
			return;
		}
		if (!p._target) {
			printf("aprend: BLOOM with a NULL target\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->instance != cmd_list->instance) {
			printf("aprend: BLOOM with a target of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET) {
			printf("aprend: BLOOM with a target that is not a RENDER_TAGET view\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			printf("aprend: BLOOM with a target that is not a 2D view\n");
			cmd_list->recording_error = true;
			return;
		}
		// A 2D render target view is of a 2D texture.
		if (!aprend_bloom_target_format(p._target->texture._t2d->desc.format)) {
			printf("aprend: BLOOM with a target in format %d, which a bloom doesn't render onto\n", (int)p._target->texture._t2d->desc.format);
			cmd_list->recording_error = true;
			return;
		}
		if (aprend_texture_ranges_overlap(aprend_texture_view_range(p._source), aprend_texture_view_range(p._target))) {
			printf("aprend: BLOOM with a source and a target that share a mip of a layer\n");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._threshold) && p._threshold >= 0.0f)) {
			printf("aprend: BLOOM with a _threshold that is negative or not a number\n");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._knee) && p._knee >= 0.0f)) {
			printf("aprend: BLOOM with a _knee that is negative or not a number\n");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._radius) && p._radius > 0.0f)) {
			printf("aprend: BLOOM with a _radius that is not above 0, or not a number\n");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._intensity) && p._intensity >= 0.0f)) {
			printf("aprend: BLOOM with an _intensity that is negative or not a number\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._levels == 0) {
			printf("aprend: BLOOM with _levels 0\n");
			cmd_list->recording_error = true;
			return;
		}
		const uint32_t most = aprend_bloom_max_levels(p._source);
		if (p._levels > most) {
			printf("aprend: BLOOM with _levels %u, and its source's size allows at most %u\n", p._levels, most);
			cmd_list->recording_error = true;
			return;
		}
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

	if (cmd._type == APREND_COMMAND_TONE_MAP) {
		const auto &p = cmd._params._tone_map;
		const char *fault = nullptr;
		if (!p._source || !p._target)
			fault = "a NULL source or target";
		else if (p._source->instance != cmd_list->instance || p._target->instance != cmd_list->instance)
			fault = "a source or target of another instance";
		else if (p._source->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || p._source->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D ||
		         p._source->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR)
			fault = "a source that is not a SHADER_RESOURCE 2D view of color";
		else if (p._source->desc.mip_count != 1)
			fault = "a source of more than one mip";
		else if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET || p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D)
			fault = "a target that is not a RENDER_TAGET 2D view";
		else if (!aprend_builtin_downsample_format(p._target->texture._t2d->desc.format))
			fault = "a target in a format it doesn't render onto";
		else if (aprend_texture_ranges_overlap(aprend_texture_view_range(p._source), aprend_texture_view_range(p._target)))
			fault = "a source and a target that share a mip of a layer";
		else if (!(std::isfinite(p._exposure) && p._exposure >= 0.0f))
			fault = "an _exposure that is negative or not a number";
		else if (p._curve > APREND_TONE_CURVE_AGX)
			fault = "a _curve that is not an APREND_TONE_CURVE";
		else if (p._encoding > APREND_COLOR_ENCODING_SRGB)
			fault = "an _encoding that is not an APREND_COLOR_ENCODING";
		else if (p._auto_exposure && p._auto_exposure->instance != cmd_list->instance)
			fault = "an _auto_exposure of another instance";
		if (fault) {
			printf("aprend: TONE_MAP with %s\n", fault);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_LIT_ENVIRONMENT) {
		const auto &p = cmd._params._lit_environment;
		const char *fault = nullptr;
		if (!p._environment || !p._source)
			fault = "a NULL environment or source";
		else if (p._environment->instance != cmd_list->instance || p._source->instance != cmd_list->instance)
			fault = "an environment or source of another instance";
		else if (p._source->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || p._source->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR ||
		         (p._source->desc.shape != APREND_TEXTURE_VIEW_SHAPE_CUBE && p._source->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D))
			fault = "a source that is not a SHADER_RESOURCE view of color shaped CUBE or 2D";
		if (fault) {
			printf("aprend: LIT_ENVIRONMENT with %s\n", fault);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_LIT_SKY) {
		const auto &p = cmd._params._lit_sky;
		const char *fault = nullptr;
		const float *d    = p._sun_direction;
		const float *g    = p._ground_albedo;
		if (!p._environment)
			fault = "a NULL environment";
		else if (p._environment->instance != cmd_list->instance)
			fault = "an environment of another instance";
		else if (!(std::isfinite(d[0]) && std::isfinite(d[1]) && std::isfinite(d[2])) || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] == 0.0f)
			fault = "a _sun_direction of 0, or not a number";
		else if (!(p._turbidity >= 1.7f && p._turbidity <= 10.0f))
			fault = "a _turbidity outside 1.7 to 10";
		else if (!(std::isfinite(p._intensity) && p._intensity >= 0.0f))
			fault = "an _intensity that is negative or not a number";
		else if (!(g[0] >= 0.0f && g[1] >= 0.0f && g[2] >= 0.0f && std::isfinite(g[0]) && std::isfinite(g[1]) && std::isfinite(g[2])))
			fault = "a _ground_albedo that is negative or not a number";
		if (fault) {
			printf("aprend: LIT_SKY with %s\n", fault);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_LIT_AMBIENT_OCCLUSION) {
		const auto &p = cmd._params._lit_ambient_occlusion;
		const char *fault = nullptr;
		if (!p._scene || !p._depth || !p._target)
			fault = "a NULL scene, depth or target";
		else if (p._scene->instance != cmd_list->instance || p._depth->instance != cmd_list->instance || p._target->instance != cmd_list->instance)
			fault = "a scene, depth or target of another instance";
		else if (p._depth->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || p._depth->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D ||
		         p._depth->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_DEPTH)
			fault = "a depth that is not a SHADER_RESOURCE 2D view of depth";
		else if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET || p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D ||
		         !aprend_builtin_downsample_format(p._target->texture._t2d->desc.format))
			fault = "a target that is not a RENDER_TAGET 2D view of a format it renders onto";
		else if (aprend_mip_size(p._depth->texture._t2d->desc.width, p._depth->desc.base_mip) !=
		             aprend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip) ||
		         aprend_mip_size(p._depth->texture._t2d->desc.height, p._depth->desc.base_mip) !=
		             aprend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip))
			fault = "a depth and a target of different sizes";
		else if (!(std::isfinite(p._radius) && p._radius > 0.0f) || !(std::isfinite(p._intensity) && p._intensity >= 0.0f))
			fault = "a _radius that is not above 0, or an _intensity below 0, or either not a number";
		if (fault) {
			printf("aprend: LIT_AMBIENT_OCCLUSION with %s\n", fault);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_AUTO_EXPOSURE) {
		const auto &p = cmd._params._auto_exposure;
		const char *fault = nullptr;
		if (!p._exposure || !p._source)
			fault = "a NULL exposure or source";
		else if (p._exposure->instance != cmd_list->instance || p._source->instance != cmd_list->instance)
			fault = "an exposure or source of another instance";
		else if (p._source->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || p._source->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D ||
		         p._source->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR || p._source->desc.mip_count != 1)
			fault = "a source that is not a SHADER_RESOURCE 2D view of color of one mip";
		else if (!(std::isfinite(p._delta_time) && p._delta_time >= 0.0f))
			fault = "a _delta_time that is negative or not a number";
		if (fault) {
			printf("aprend: AUTO_EXPOSURE with %s\n", fault);
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
	cmd_list->layout_uses.push_back({range.image, mip, layer, tracked, range.tracking, *tracked, *tracked, true, true});
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
			bool barrier       = uniform;
			if (!uniform && !(skip_if_same && use.final_layout == layout)) {
				spudgpu_image_view_desc_subresource_range one{};
				one.base_mip_level    = use.mip;
				one.mip_level_count   = 1;
				one.base_array_layer  = use.layer;
				one.array_layer_count = 1;
				spudgpu_cmd_image_barrier_subresource(cmd_list->cmd_list, range.image, &one, use.final_layout, layout);
				barrier = true;
			}
			// A barrier orders everything before it, storage writes included.
			if (barrier) {
				use.storage_accessed = false;
				use.storage_written  = false;
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
	/* A storage image a pipeline of the pass or dispatch writes. */
	bool written;
};

/* A storage buffer one pass's or dispatch's binding sets hold. */
struct aprend_pass_store {
	aprend_buffer_store *store;
	/* A pipeline of the pass or dispatch writes it. */
	bool written;
};

/* Adds the storage buffers [set], at [slot], holds for frame [frame] to
 * [stores], each once, with whether [writes] (the pipeline's) writes the
 * binding each is in. */
static void aprend_storage_stores_add_set(
    std::vector<aprend_pass_store> &stores,
    aprend_binding_set set,
    uint32_t frame,
    uint32_t slot,
    const std::vector<aprend_storage_write> &writes) {
	const uint32_t frame_slot                               = aprend_binding_set_frame_slot(set, frame);
	const aprend_binding_set_t::storage_store *frame_stores = set->storage_stores + (size_t)frame_slot * set->storage_store_capacity;
	for (uint32_t i = 0; i < set->storage_store_counts[frame_slot]; ++i) {
		const bool written = aprend_storage_writes_hold(writes, slot, frame_stores[i].binding);
		bool known         = false;
		for (aprend_pass_store &listed : stores) {
			if (listed.store != frame_stores[i].store)
				continue;
			listed.written = listed.written || written;
			known          = true;
		}
		if (!known)
			stores.push_back({frame_stores[i].store, written});
	}
}

/* Adds what [set], at [slot], has its views cover to [images], each storage
 * image with whether [writes] (the pipeline's) writes its binding. False if
 * any mip of any layer is already there in another layout: sampled through
 * one slot and a storage image through another. Two views of one texture
 * that don't share a mip of a layer are no conflict, which is what lets one
 * mip be read while another is written. */
static bool aprend_pass_images_add_set(
    std::vector<aprend_pass_image> &images,
    aprend_binding_set set,
    uint32_t slot,
    const std::vector<aprend_storage_write> &writes) {
	for (uint32_t i = 0; i < set->image_use_count; ++i) {
		const auto &use                  = set->image_uses[i];
		const aprend_texture_range range = aprend_texture_view_range(use.view);
		const bool written = use.layout == SPUDGPU_IMAGE_LAYOUT_GENERAL && aprend_storage_writes_hold(writes, slot, use.binding);

		bool known = false;
		for (aprend_pass_image &pass_image : images) {
			if (!aprend_texture_ranges_overlap(pass_image.range, range))
				continue;
			if (pass_image.layout != use.layout) {
				printf("aprend: a mip of a texture is in a sampled slot and a storage image slot of the binding sets of one pass or dispatch\n");
				return false;
			}
			// The same range again adds nothing. One that only overlaps is
			// kept: it has subresources the other doesn't.
			if (pass_image.range.base_mip == range.base_mip && pass_image.range.mip_count == range.mip_count &&
			    pass_image.range.base_layer == range.base_layer && pass_image.range.layer_count == range.layer_count) {
				pass_image.written = pass_image.written || written;
				known              = true;
			}
		}
		if (!known)
			images.push_back({range, use.layout, written});
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
	// A barrier orders everything before it, storage writes included.
	for (auto &hazard : cmd_list->storage_hazards) {
		if (hazard.store != store)
			continue;
		hazard.accessed = false;
		hazard.written  = false;
	}
}

/* [cmd_list]'s storage hazard of [store], added the first time: used and
 * written, for all the list knows, by a submission before it. Throws
 * std::bad_alloc. */
static aprend_command_list_t::storage_hazard &aprend_cmd_store_hazard(
    aprend_command_list cmd_list,
    aprend_buffer_store *store) {
	for (auto &hazard : cmd_list->storage_hazards)
		if (hazard.store == store)
			return hazard;
	cmd_list->storage_hazards.push_back({store, true, true});
	return cmd_list->storage_hazards.back();
}

/* Makes what a pass's or a dispatch's binding sets hold ready to be read:
 * each texture in [images] is moved into the layout its slot reads it in,
 * and every storage image among them and every storage buffer in [stores] is
 * ordered after the storage accesses before this point that need it. Reads
 * after reads need no barrier, so a sampled texture already in its layout
 * gets none, and nor does a storage resource no pipeline has written since
 * its last barrier, used by one that only reads it.
 *
 * The ordering is SpudGPU's barrier that moves nothing: from GENERAL to
 * GENERAL over the range for an image, from UNORDERED_ACCESS to
 * UNORDERED_ACCESS for a buffer (spudgpu.h). It is needed before a use if a
 * shader may have written the resource since its last barrier, or if this
 * use writes it and a shader may have used it since: what each pipeline
 * writes is its storage_writes, and the list's layout_uses and
 * storage_hazards keep what happened since each barrier. The first use in a
 * list always gets one, since a submission before it may have written the
 * resource. Recorded outside any pass. Throws std::bad_alloc. */
static void aprend_cmd_ready_set_resources(
    aprend_command_list cmd_list,
    const std::vector<aprend_pass_image> &images,
    const std::vector<aprend_pass_store> &stores) {
	for (const aprend_pass_image &pass_image : images) {
		const aprend_texture_range &range = pass_image.range;
		if (pass_image.layout != SPUDGPU_IMAGE_LAYOUT_GENERAL) {
			// Sampled: a barrier only to move it.
			aprend_cmd_transition_range(cmd_list, range, pass_image.layout, true);
			continue;
		}
		// GENERAL is the layout of a storage image slot. The barrier is over
		// the range, so the texture's other mips are left where they are.
		bool order = false;
		for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
			for (uint32_t mip = 0; mip < range.mip_count; ++mip) {
				const auto &use = cmd_list->layout_uses[aprend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer)];
				if (use.storage_written || (pass_image.written && use.storage_accessed))
					order = true;
			}
		}
		// With [order], one from GENERAL to GENERAL where it is there
		// already; without, only a move into GENERAL where it is not.
		aprend_cmd_transition_range(cmd_list, range, SPUDGPU_IMAGE_LAYOUT_GENERAL, !order);
		for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
			for (uint32_t mip = 0; mip < range.mip_count; ++mip) {
				auto &use            = cmd_list->layout_uses[aprend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer)];
				use.storage_accessed = true;
				use.storage_written  = use.storage_written || pass_image.written;
			}
		}
	}
	std::vector<spudgpu_buffer_barrier> buffer_barriers;
	for (const aprend_pass_store &pass_store : stores) {
		aprend_buffer_store *store = pass_store.store;
		// One that was last an indirect argument buffer comes back to the
		// state a storage slot uses it in.
		if (store->multi_state)
			aprend_cmd_transition_store(cmd_list, store, store->use_state);
		aprend_command_list_t::storage_hazard &hazard = aprend_cmd_store_hazard(cmd_list, store);
		if (hazard.written || (pass_store.written && hazard.accessed)) {
			buffer_barriers.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS});
			hazard.written = false;
		}
		hazard.accessed = true;
		hazard.written  = hazard.written || pass_store.written;
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
	std::vector<aprend_pass_store> stores;
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
				if (!aprend_pass_images_add_set(images, pass_slots[slot], slot, pass_pipeline->storage_writes))
					return false;
				aprend_storage_stores_add_set(stores, pass_slots[slot], cmd_list->compiled_frame, slot, pass_pipeline->storage_writes);
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
		for (const aprend_pass_store &pass_store : stores) {
			if (pass_store.store == arguments) {
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
	std::vector<aprend_pass_store> stores;
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
		if (!aprend_pass_images_add_set(images, set, slot, pipeline->storage_writes))
			return false;
		aprend_storage_stores_add_set(stores, set, cmd_list->compiled_frame, slot, pipeline->storage_writes);
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
/* What a vertex buffer slot holds, as the compile walks the list: a vertex
 * buffer, a transient span, or nothing. A span's view is made at the stride
 * of the first draw that reads it and kept for the draws after at that
 * stride. */
struct aprend_cmd_vertex_slot {
	aprend_vertex_buffer buffer;
	bool has_span;
	aprend_transient_span span;
	spudgpu_buffer_view span_view;
	uint32_t span_view_stride;
};

/* The elements [draw] reads from a binding that steps per instance (if
 * [per_instance]) or per vertex: the first and how many, false where the
 * compile can't know (an indexed draw's vertices, an indirect draw's
 * anything). */
static bool aprend_cmd_draw_reads(const APREND_COMMAND &draw, bool per_instance, uint64_t *out_first, uint64_t *out_count) {
	switch (draw._type) {
	case APREND_COMMAND_DRAW:
		*out_first = per_instance ? 0 : draw._params._draw._start_vertex_location;
		*out_count = per_instance ? 1 : draw._params._draw._vertex_count;
		return true;
	case APREND_COMMAND_DRAW_INDEXED:
		// Instance 0 of a draw that is not instanced.
		*out_first = 0;
		*out_count = 1;
		return per_instance;
	case APREND_COMMAND_DRAW_INSTANCED:
		*out_first = per_instance ? draw._params._draw_instanced._start_instance_location : draw._params._draw_instanced._start_vertex_location;
		*out_count = per_instance ? draw._params._draw_instanced._instance_count : draw._params._draw_instanced._vertex_count_per_instance;
		return true;
	case APREND_COMMAND_DRAW_INSTANCED_INDEXED:
		*out_first = draw._params._draw_indexed_instanced._start_instance_location;
		*out_count = draw._params._draw_indexed_instanced._instance_count;
		return per_instance;
	default:
		return false;
	}
}

/* Before [draw]: every vertex buffer slot [pipeline] reads holds a buffer or
 * a span, what the draw is known to read of each lies inside it at the
 * binding's stride, and each is bound at that stride where [applied] says
 * the slot has something else. A SpudGPU vertex view carries its stride, so
 * the view is the buffer's for that stride (aprend_vertex_buffer_view) or
 * the span's. False, printed, if not. */
static bool aprend_cmd_apply_vertex_buffers(
    aprend_command_list cmd_list,
    aprend_graphics_pipeline pipeline,
    aprend_cmd_vertex_slot *slots,
    spudgpu_buffer_view *applied,
    const APREND_COMMAND &draw) {
	for (uint32_t slot = 0; slot < pipeline->desc._vertex_binding_count; ++slot) {
		aprend_cmd_vertex_slot &held = slots[slot];
		if (!held.buffer && !held.has_span) {
			printf("aprend: draw with a pipeline that reads vertex buffer slot %u, and nothing has been put there\n", slot);
			return false;
		}
		const uint32_t stride  = pipeline->vertex_strides[slot];
		const uint64_t size    = held.buffer ? held.buffer->size : held.span.size;
		uint64_t first         = 0;
		uint64_t count         = 0;
		const bool per_instance = pipeline->desc._vertex_bindings[slot]._per_instance;
		if (aprend_cmd_draw_reads(draw, per_instance, &first, &count) && count > 0) {
			// The last element read ends where its furthest attribute does.
			const uint64_t end = (first + count - 1) * stride + pipeline->vertex_extents[slot];
			if (end > size) {
				printf("aprend: draw reads %s %llu to %llu at vertex buffer slot %u, %llu bytes at a stride of %u, and the %s holds %llu\n",
				       per_instance ? "instances" : "vertices", (unsigned long long)first, (unsigned long long)(first + count - 1), slot,
				       (unsigned long long)end, stride, held.buffer ? "buffer" : "span", (unsigned long long)size);
				return false;
			}
		}
		spudgpu_buffer_view view = nullptr;
		SPUDRESULT sr            = SPUD_SUCCESS;
		if (held.buffer) {
			sr = aprend_vertex_buffer_view(held.buffer, stride, &view);
		} else if (held.span_view && held.span_view_stride == stride) {
			view = held.span_view;
		} else {
			sr = aprend_transient_span_view(cmd_list->instance, &held.span, stride, &view);
			if (!SPUDFAIL(sr)) {
				held.span_view        = view;
				held.span_view_stride = stride;
			}
		}
		if (SPUDFAIL(sr)) {
			printf("aprend: draw: a view of vertex buffer slot %u at a stride of %u can't be created: %s\n", slot, stride, spudresult_str(sr));
			return false;
		}
		if (applied[slot] == view)
			continue;
		spudgpu_cmd_set_vertex_buffers(cmd_list->cmd_list, slot, 1, &view);
		applied[slot] = view;
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
	const uint32_t frame_slot                               = aprend_binding_set_frame_slot(set, cmd_list->compiled_frame);
	const aprend_binding_set_t::storage_store *frame_stores = set->storage_stores + (size_t)frame_slot * set->storage_store_capacity;
	for (uint32_t i = 0; i < set->storage_store_counts[frame_slot]; ++i)
		aprend_cmd_use_store(cmd_list, frame_stores[i].store);
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

/* BLOOM, outside a pass: the source into SHADER_READ_ONLY; the chain's
 * level 0 prefiltered from it, each further level downsampled from the one
 * above, each level but the last added back up into the one above; then
 * level 0 added onto the target, left in COLOR_ATTACHMENT_OPTIMAL. Each
 * level is moved to SHADER_READ_ONLY once rendered. False, printed, if the
 * source has nothing in it as the list reaches the command, or a pipeline
 * or set can't be made. */
static bool aprend_cmd_bloom(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p            = command._params._bloom;
	aprend_instance instance = cmd_list->instance;
	spudgpu_command_list cmd = cmd_list->cmd_list;

	// Checked again: a resize since the command was sent may have changed
	// the source's size.
	if (p._levels > aprend_bloom_max_levels(p._source)) {
		printf("aprend: BLOOM: the source's size no longer allows _levels %u\n", p._levels);
		return false;
	}
	const aprend_texture_range source = aprend_texture_view_range(p._source);
	for (uint32_t layer = 0; layer < source.layer_count; ++layer) {
		for (uint32_t mip = 0; mip < source.mip_count; ++mip) {
			const size_t index = aprend_cmd_layout_use(cmd_list, source, source.base_mip + mip, source.base_layer + layer);
			if (cmd_list->layout_uses[index].final_layout == SPUDGPU_IMAGE_LAYOUT_UNDEFINED) {
				printf("aprend: BLOOM: the source has nothing in it, from before the list or earlier in it\n");
				return false;
			}
		}
	}

	// The chain: level 0 half the source's first mip.
	const SPUDGPU_FORMAT chain_format  = SPUDGPU_FORMAT_R16G16B16A16_FLOAT;
	const SPUDGPU_FORMAT target_format = p._target->texture._t2d->desc.format;
	const aprend_texture2d source_tex  = p._source->texture._t2d;
	const uint32_t chain_width         = aprend_mip_size(aprend_mip_size(source_tex->desc.width, p._source->desc.base_mip), 1);
	const uint32_t chain_height        = aprend_mip_size(aprend_mip_size(source_tex->desc.height, p._source->desc.base_mip), 1);
	const uint32_t levels              = p._levels;

	aprend_graphics_pipeline prefilter  = aprend_builtin_bloom_pipeline(instance, APREND_BLOOM_STAGE_PREFILTER, chain_format);
	aprend_graphics_pipeline downsample = aprend_builtin_bloom_pipeline(instance, APREND_BLOOM_STAGE_DOWNSAMPLE, chain_format);
	aprend_graphics_pipeline upsample   = aprend_builtin_bloom_pipeline(instance, APREND_BLOOM_STAGE_UPSAMPLE, chain_format);
	aprend_graphics_pipeline composite  = aprend_builtin_bloom_pipeline(instance, APREND_BLOOM_STAGE_COMPOSITE, target_format);
	if (!prefilter || !downsample || !upsample || !composite)
		return false;
	aprend_pooled_target *chain = aprend_render_target_take(cmd_list, chain_width, chain_height, chain_format, levels);
	if (!chain)
		return false;
	aprend_binding_set source_set = aprend_texture_view_bloom_set(p._source);
	if (!source_set)
		return false;
	for (uint32_t level = 0; level < levels; ++level)
		if (!aprend_texture_view_bloom_set(chain->level_sources[level]))
			return false;

	aprend_bloom_params params{};
	params.threshold  = p._threshold;
	params.knee       = p._knee;
	params.radius     = p._radius;
	params.intensity  = p._intensity;
	auto level_width  = [&](uint32_t level) { return aprend_mip_size(chain_width, level); };
	auto level_height = [&](uint32_t level) { return aprend_mip_size(chain_height, level); };
	auto set_target   = [&](uint32_t width, uint32_t height) {
		params.target_size[0] = (float)width;
		params.target_size[1] = (float)height;
	};

	aprend_cmd_transition_range(cmd_list, source, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	// Down: level 0 from the source, then each level from the one above.
	for (uint32_t level = 0; level < levels; ++level) {
		aprend_texture_view target = chain->level_targets[level];
		aprend_cmd_transition_view(cmd_list, target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		set_target(level_width(level), level_height(level));
		if (level == 0)
			aprend_record_fullscreen_pass(cmd, prefilter, target->image_view, level_width(0), level_height(0), SPUDGPU_LOAD_OP_DONT_CARE, source_set,
			                              &params, sizeof(params));
		else
			aprend_record_fullscreen_pass(cmd, downsample, target->image_view, level_width(level), level_height(level), SPUDGPU_LOAD_OP_DONT_CARE,
			                              chain->level_sources[level - 1]->bloom_set, nullptr, 0);
		aprend_cmd_transition_view(cmd_list, target, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	}
	// Up: each level added into the one above, from the smallest.
	for (uint32_t level = levels - 1; level > 0; --level) {
		aprend_texture_view target = chain->level_targets[level - 1];
		aprend_cmd_transition_view(cmd_list, target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		set_target(level_width(level - 1), level_height(level - 1));
		aprend_record_fullscreen_pass(cmd, upsample, target->image_view, level_width(level - 1), level_height(level - 1), SPUDGPU_LOAD_OP_LOAD,
		                              chain->level_sources[level]->bloom_set, &params, sizeof(params));
		aprend_cmd_transition_view(cmd_list, target, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	}
	// Onto the target, at its own size: the size of the mip its view is of.
	const uint32_t target_width  = aprend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip);
	const uint32_t target_height = aprend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip);
	aprend_cmd_transition_view(cmd_list, p._target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	set_target(target_width, target_height);
	aprend_record_fullscreen_pass(cmd, composite, p._target->image_view, target_width, target_height, SPUDGPU_LOAD_OP_LOAD,
	                              chain->level_sources[0]->bloom_set, &params, sizeof(params));
	return true;
}

/* Whether every mip and layer of [range] has something in it as the list
 * reaches this point: written before the list, or earlier in it. */
static bool aprend_cmd_range_written(aprend_command_list cmd_list, const aprend_texture_range &range) {
	for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
		for (uint32_t mip = 0; mip < range.mip_count; ++mip) {
			const size_t index = aprend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer);
			if (cmd_list->layout_uses[index].final_layout == SPUDGPU_IMAGE_LAYOUT_UNDEFINED)
				return false;
		}
	}
	return true;
}

/* Adds [material] to the materials [cmd_list] uploads at its submission,
 * once, building its binding set first: for a material Aprend binds itself,
 * which no SET_MATERIAL names. False if its set can't be built. */
static bool aprend_cmd_use_material(aprend_command_list cmd_list, aprend_material material) {
	for (aprend_material listed : cmd_list->materials)
		if (listed == material)
			return true;
	if (!aprend_material_prepare(material))
		return false;
	try {
		cmd_list->materials.push_back(material);
	} catch (const std::bad_alloc &) {
		printf("aprend: compile: out of memory\n");
		return false;
	}
	return true;
}

/* One full-target pass of Aprend's own into [target], a view [width] by
 * [height], reading [sets]. The target is moved to COLOR_ATTACHMENT_OPTIMAL
 * first and to SHADER_READ_ONLY after; what the sets read is the caller's to
 * have moved. */
static void aprend_cmd_own_pass(aprend_command_list cmd_list, aprend_graphics_pipeline pipeline, aprend_texture_view target, uint32_t width,
                                uint32_t height, const aprend_binding_set *sets, uint32_t set_count, const void *push, uint32_t push_size) {
	aprend_cmd_transition_view(cmd_list, target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	aprend_record_fullscreen_pass_sets(cmd_list->cmd_list, pipeline, target->image_view, width, height, SPUDGPU_LOAD_OP_DONT_CARE, sets,
	                                   set_count, cmd_list->compiled_frame, push, push_size);
	aprend_cmd_transition_view(cmd_list, target, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
}

/* TONE_MAP, outside a pass: the source into SHADER_READ_ONLY, the target
 * into COLOR_ATTACHMENT_OPTIMAL and every texel of it replaced. */
static bool aprend_cmd_tone_map(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                     = command._params._tone_map;
	aprend_instance instance          = cmd_list->instance;
	const aprend_texture_range source = aprend_texture_view_range(p._source);
	if (!aprend_cmd_range_written(cmd_list, source)) {
		printf("aprend: TONE_MAP: the source has nothing in it, from before the list or earlier in it\n");
		return false;
	}
	aprend_graphics_pipeline pipeline = aprend_lit_tone_map_pipeline(instance, p._target->texture._t2d->desc.format);
	if (!pipeline)
		return false;
	// The exposure's last value, or a placeholder that isn't read.
	aprend_texture_view exposure = p._auto_exposure ? p._auto_exposure->value_views[p._auto_exposure->current]
	                                                : instance->builtins.lit.placeholder_2d_view;
	if (p._auto_exposure && !p._auto_exposure->adapted) {
		printf("aprend: TONE_MAP: its _auto_exposure has never adapted (AUTO_EXPOSURE)\n");
		return false;
	}
	aprend_binding_set sets[2] = {aprend_texture_view_bloom_set(p._source), aprend_texture_view_environment_set(exposure)};
	if (!sets[0] || !sets[1])
		return false;
	const uint32_t width  = aprend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip);
	const uint32_t height = aprend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip);
	aprend_tone_map_params params{};
	params.target_size[0] = (float)width;
	params.target_size[1] = (float)height;
	params.exposure       = p._exposure;
	params.curve          = p._curve;
	params.encoding       = p._encoding;
	params.automatic      = p._auto_exposure ? 1u : 0u;
	aprend_cmd_transition_range(cmd_list, source, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	if (p._auto_exposure)
		aprend_cmd_transition_view(cmd_list, exposure, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	aprend_cmd_transition_view(cmd_list, p._target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	aprend_record_fullscreen_pass_sets(cmd_list->cmd_list, pipeline, p._target->image_view, width, height, SPUDGPU_LOAD_OP_DONT_CARE, sets, 2,
	                                   cmd_list->compiled_frame, &params, sizeof(params));
	return true;
}

/* What both fills of an environment end with, once the radiance cube's
 * first mip has the picture: its other mips generated; each mip of the
 * specular chain prefiltered from it, face by face; and the irradiance
 * cube. Everything it renders is left in SHADER_READ_ONLY. */
static bool aprend_cmd_prefilter_environment(aprend_command_list cmd_list, aprend_lit_environment env) {
	aprend_instance instance            = cmd_list->instance;
	aprend_graphics_pipeline prefilter  = aprend_lit_pass_pipeline(instance, APREND_LIT_PASS_PREFILTER);
	aprend_graphics_pipeline irradiance = aprend_lit_pass_pipeline(instance, APREND_LIT_PASS_IRRADIANCE);
	aprend_binding_set radiance_set     = aprend_texture_view_environment_set(env->radiance_cube);
	if (!prefilter || !irradiance || !radiance_set)
		return false;
	if (!aprend_cmd_generate_mips(cmd_list, env->radiance, 0, SPUDGPU_FILTER_LINEAR))
		return false;
	aprend_cmd_transition_range(cmd_list, aprend_texture_view_range(env->radiance_cube), SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	const uint32_t size = env->desc.specular_size;
	aprend_environment_params params{};
	params.source_size = (float)size;
	params.source_mips = (float)env->radiance->desc.mip_levels;
	for (uint32_t mip = 0; mip < env->specular_mips; ++mip) {
		const uint32_t side = aprend_mip_size(size, mip);
		params.roughness    = env->specular_mips > 1 ? (float)mip / (float)(env->specular_mips - 1) : 0.0f;
		params.sample_count = mip == 0 ? 1u : 256u;
		params.target_size[0] = params.target_size[1] = (float)side;
		for (uint32_t face = 0; face < 6; ++face) {
			params.face = face;
			aprend_cmd_own_pass(cmd_list, prefilter, env->specular_faces[(size_t)mip * 6 + face], side, side, &radiance_set, 1, &params, sizeof(params));
		}
	}
	params.roughness      = 0.0f;
	params.sample_count   = 512u;
	params.target_size[0] = params.target_size[1] = (float)env->desc.irradiance_size;
	for (uint32_t face = 0; face < 6; ++face) {
		params.face = face;
		aprend_cmd_own_pass(cmd_list, irradiance, env->irradiance_faces[face], env->desc.irradiance_size, env->desc.irradiance_size, &radiance_set, 1,
		                    &params, sizeof(params));
	}
	return true;
}

/* LIT_ENVIRONMENT, outside a pass: the source copied into the radiance
 * cube's first mip, face by face, then prefiltered. */
static bool aprend_cmd_lit_environment(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                     = command._params._lit_environment;
	aprend_lit_environment env        = p._environment;
	const aprend_texture_range source = aprend_texture_view_range(p._source);
	if (!aprend_cmd_range_written(cmd_list, source)) {
		printf("aprend: LIT_ENVIRONMENT: the source has nothing in it, from before the list or earlier in it\n");
		return false;
	}
	const bool cube               = p._source->desc.shape == APREND_TEXTURE_VIEW_SHAPE_CUBE;
	aprend_graphics_pipeline copy = aprend_lit_pass_pipeline(cmd_list->instance, cube ? APREND_LIT_PASS_FROM_CUBE : APREND_LIT_PASS_FROM_EQUIRECT);
	aprend_binding_set source_set = copy ? aprend_texture_view_environment_set(p._source) : nullptr;
	if (!source_set)
		return false;
	const uint32_t size = env->desc.specular_size;
	aprend_environment_params params{};
	params.target_size[0] = params.target_size[1] = (float)size;
	aprend_cmd_transition_range(cmd_list, source, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	for (uint32_t face = 0; face < 6; ++face) {
		params.face = face;
		aprend_cmd_own_pass(cmd_list, copy, env->radiance_faces[face], size, size, &source_set, 1, &params, sizeof(params));
	}
	return aprend_cmd_prefilter_environment(cmd_list, env);
}

/* LIT_SKY, outside a pass: Preetham's sky into the radiance cube's first
 * mip, face by face, then prefiltered. */
static bool aprend_cmd_lit_sky(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                = command._params._lit_sky;
	aprend_lit_environment env   = p._environment;
	aprend_graphics_pipeline sky = aprend_lit_pass_pipeline(cmd_list->instance, APREND_LIT_PASS_SKY);
	if (!sky)
		return false;
	const uint32_t size = env->desc.specular_size;
	const float l       = std::sqrt(p._sun_direction[0] * p._sun_direction[0] + p._sun_direction[1] * p._sun_direction[1] +
	                                p._sun_direction[2] * p._sun_direction[2]);
	aprend_sky_params params{};
	params.target_size[0] = params.target_size[1] = (float)size;
	params.turbidity                              = p._turbidity;
	for (int i = 0; i < 3; ++i) {
		params.sun[i]    = p._sun_direction[i] / l;
		params.ground[i] = p._ground_albedo[i];
	}
	params.sun[3]    = p._intensity;
	params.ground[3] = 0.0f;
	for (uint32_t face = 0; face < 6; ++face) {
		params.face = face;
		aprend_cmd_own_pass(cmd_list, sky, env->radiance_faces[face], size, size, nullptr, 0, &params, sizeof(params));
	}
	return aprend_cmd_prefilter_environment(cmd_list, env);
}

/* LIT_AMBIENT_OCCLUSION, outside a pass: the raw estimate into a pooled
 * texture the depth's size, then smoothed onto the target, left in
 * SHADER_READ_ONLY for the scene to read. */
static bool aprend_cmd_lit_ambient_occlusion(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                    = command._params._lit_ambient_occlusion;
	aprend_instance instance         = cmd_list->instance;
	const aprend_texture_range depth = aprend_texture_view_range(p._depth);
	if (!aprend_cmd_range_written(cmd_list, depth)) {
		printf("aprend: LIT_AMBIENT_OCCLUSION: the depth has nothing in it, from before the list or earlier in it\n");
		return false;
	}
	const uint32_t width            = aprend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip);
	const uint32_t height           = aprend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip);
	aprend_graphics_pipeline raw    = aprend_lit_pass_pipeline(instance, APREND_LIT_PASS_OCCLUSION);
	aprend_graphics_pipeline smooth = aprend_lit_blur_pipeline(instance, p._target->texture._t2d->desc.format);
	if (!raw || !smooth)
		return false;
	// The view's matrices and size, and the command's settings, in the
	// scene's occlusion material for this list.
	const aprend_lit_scene_block &b = p._scene->block;
	aprend_occlusion_block block{};
	memcpy(block.view_projection, b.view_projection, sizeof(block.view_projection));
	memcpy(block.inverse_view_projection, b.inverse_view_projection, sizeof(block.inverse_view_projection));
	block.target_size[0] = (float)width;
	block.target_size[1] = (float)height;
	block.target_size[2] = 1.0f / (float)width;
	block.target_size[3] = 1.0f / (float)height;
	memcpy(block.eye_position, b.eye_position, sizeof(block.eye_position));
	block.params[0]      = p._radius;
	block.params[1]      = p._intensity;
	block.params[2]      = 0.1f;
	aprend_material material = p._scene->occlusion_material;
	for (uint32_t i = 0; i < material->slot_count; ++i) {
		if (material->slots[i].bytes) {
			memcpy(material->slots[i].bytes, &block, sizeof(block));
			material->slots[i].dirty = true;
		}
	}
	if (!aprend_cmd_use_material(cmd_list, material))
		return false;
	aprend_pooled_target *pooled = aprend_render_target_take(cmd_list, width, height, APREND_OCCLUSION_FORMAT, 1);
	if (!pooled)
		return false;
	aprend_binding_set raw_sets[2] = {material->set, aprend_texture_view_environment_set(p._depth)};
	aprend_binding_set blur_set    = aprend_texture_view_environment_set(pooled->level_sources[0]);
	if (!raw_sets[1] || !blur_set)
		return false;
	aprend_cmd_transition_range(cmd_list, depth, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	aprend_cmd_own_pass(cmd_list, raw, pooled->level_targets[0], width, height, raw_sets, 2, nullptr, 0);
	aprend_size_params size{};
	size.target_size[0] = (float)width;
	size.target_size[1] = (float)height;
	aprend_cmd_own_pass(cmd_list, smooth, p._target, width, height, &blur_set, 1, &size, sizeof(size));
	return true;
}

/* AUTO_EXPOSURE, outside a pass: the source's log luminance into the
 * exposure's luminance texture, its mips averaged down, and the adapted
 * exposure into the value texture not last written, which becomes the
 * current one. */
static bool aprend_cmd_auto_exposure(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                     = command._params._auto_exposure;
	aprend_instance instance          = cmd_list->instance;
	aprend_exposure exposure          = p._exposure;
	const aprend_texture_range source = aprend_texture_view_range(p._source);
	if (!aprend_cmd_range_written(cmd_list, source)) {
		printf("aprend: AUTO_EXPOSURE: the source has nothing in it, from before the list or earlier in it\n");
		return false;
	}
	aprend_graphics_pipeline luminance = aprend_lit_pass_pipeline(instance, APREND_LIT_PASS_LUMINANCE);
	aprend_graphics_pipeline adapt     = aprend_lit_pass_pipeline(instance, APREND_LIT_PASS_ADAPT);
	if (!luminance || !adapt)
		return false;
	const uint32_t next          = exposure->current ^ 1u;
	aprend_binding_set source_set = aprend_texture_view_environment_set(p._source);
	aprend_binding_set adapt_sets[2] = {aprend_texture_view_environment_set(exposure->luminance_view),
	                                    aprend_texture_view_environment_set(exposure->value_views[exposure->current])};
	if (!source_set || !adapt_sets[0] || !adapt_sets[1])
		return false;
	aprend_cmd_transition_range(cmd_list, source, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	aprend_size_params size{};
	size.target_size[0] = size.target_size[1] = (float)APREND_EXPOSURE_LUMINANCE_SIZE;
	aprend_cmd_own_pass(cmd_list, luminance, exposure->luminance_target, APREND_EXPOSURE_LUMINANCE_SIZE, APREND_EXPOSURE_LUMINANCE_SIZE,
	                    &source_set, 1, &size, sizeof(size));
	if (!aprend_cmd_generate_mips(cmd_list, exposure->luminance, 0, SPUDGPU_FILTER_LINEAR))
		return false;
	aprend_cmd_transition_range(cmd_list, aprend_texture_view_range(exposure->luminance_view), SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	// Read only once it has been adapted: the first takes its target.
	aprend_cmd_transition_view(cmd_list, exposure->value_views[exposure->current], SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	aprend_adapt_params params{};
	params.key        = exposure->desc.key;
	params.min_log    = exposure->desc.min_log;
	params.max_log    = exposure->desc.max_log;
	params.up_rate    = exposure->desc.up_rate;
	params.down_rate  = exposure->desc.down_rate;
	params.delta_time = p._delta_time;
	params.last_mip   = (float)(exposure->luminance->desc.mip_levels - 1);
	params.first      = exposure->adapted ? 0u : 1u;
	aprend_cmd_own_pass(cmd_list, adapt, exposure->value_targets[next], 1, 1, adapt_sets, 2, &params, sizeof(params));
	exposure->current = next;
	exposure->adapted = true;
	return true;
}

/* For each lit scene [cmd_list] binds, at the top of the list:
 * - the instance's BRDF lookup texture made, if nothing has written it;
 * - its environment's cubes cleared to black, if it names one that has
 *   never been written (not filled before the list, and not yet in it), so
 *   an environment reads as black until it is filled;
 * - its lights culled for its view's tiles, if it has a target size.
 * Recorded before anything else. False if a pipeline or set can't be made. */
static bool aprend_cmd_prepare_lit_scenes(aprend_command_list cmd_list) {
	aprend_instance instance = cmd_list->instance;
	aprend_lit_builtins &lit = instance->builtins.lit;
	// A copy: culling adds materials to the list.
	std::vector<aprend_material> materials;
	try {
		materials = cmd_list->materials;
	} catch (const std::bad_alloc &) {
		printf("aprend: compile: out of memory\n");
		return false;
	}
	bool lut_checked = false;
	for (aprend_material material : materials) {
		aprend_lit_scene_t *scene = material->lit_scene;
		if (!scene)
			continue;
		if (!lut_checked) {
			lut_checked                    = true;
			const aprend_texture_range lut = aprend_texture_view_range(lit.brdf_lut_view);
			if (!aprend_cmd_range_written(cmd_list, lut)) {
				aprend_graphics_pipeline pipeline = aprend_lit_pass_pipeline(instance, APREND_LIT_PASS_BRDF_LUT);
				if (!pipeline)
					return false;
				aprend_size_params size{};
				size.target_size[0] = size.target_size[1] = (float)APREND_BRDF_LUT_SIZE;
				aprend_cmd_own_pass(cmd_list, pipeline, lit.brdf_lut_target, APREND_BRDF_LUT_SIZE, APREND_BRDF_LUT_SIZE, nullptr, 0, &size, sizeof(size));
			}
		}
		if (aprend_lit_environment env = scene->environment) {
			const aprend_texture_range specular   = aprend_texture_view_range(env->specular_cube);
			const aprend_texture_range irradiance = aprend_texture_view_range(env->irradiance_cube);
			if (!aprend_cmd_range_written(cmd_list, specular) || !aprend_cmd_range_written(cmd_list, irradiance)) {
				aprend_cmd_transition_range(cmd_list, specular, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
				aprend_cmd_transition_range(cmd_list, irradiance, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
				// Cleared to 0 in every channel: the clear value every texture is
				// made with (aprend_texture2d_create leaves the image's clear_value
				// zeroed), so D3D12 clears at its fast rate and its debug layer
				// doesn't warn of a mismatch. Only .rgb of either cube is read.
				for (uint32_t mip = 0; mip < env->specular_mips; ++mip) {
					const uint32_t side = aprend_mip_size(env->desc.specular_size, mip);
					for (uint32_t face = 0; face < 6; ++face)
						spudgpu_cmd_clear_color_attachment(cmd_list->cmd_list, env->specular_faces[(size_t)mip * 6 + face]->image_view, 0.0f, 0.0f, 0.0f,
						                                   0.0f, side, side);
				}
				for (uint32_t face = 0; face < 6; ++face)
					spudgpu_cmd_clear_color_attachment(cmd_list->cmd_list, env->irradiance_faces[face]->image_view, 0.0f, 0.0f, 0.0f, 0.0f,
					                                   env->desc.irradiance_size, env->desc.irradiance_size);
			}
		}
		if ((scene->block.flags & APREND_LIT_SCENE_FLAG_TILES) && scene->tiles) {
			aprend_compute_pipeline pipeline = aprend_lit_cull_pipeline(instance);
			if (!pipeline || !aprend_cmd_use_material(cmd_list, scene->cull_material))
				return false;
			// What the cull's set holds is ordered as any dispatch's is, so
			// the tiles it writes are known written to what comes after.
			// The copy this frame writes may still be read by a submission
			// of this frame made earlier: its first use in the list gets a
			// barrier for that.
			aprend_binding_set set = scene->cull_material->set;
			std::vector<aprend_pass_image> cull_images;
			std::vector<aprend_pass_store> cull_stores;
			if (!aprend_pass_images_add_set(cull_images, set, 0, pipeline->storage_writes))
				return false;
			aprend_storage_stores_add_set(cull_stores, set, cmd_list->compiled_frame, 0, pipeline->storage_writes);
			aprend_cmd_ready_set_resources(cmd_list, cull_images, cull_stores);
			spudgpu_cmd_bind_compute_pipeline(cmd_list->cmd_list, pipeline->pipeline);
			spudgpu_cmd_bind_descriptor_sets_compute(cmd_list->cmd_list, pipeline->pipeline, 0,
			                                         &set->sets[aprend_binding_set_frame_slot(set, cmd_list->compiled_frame)], 1);
			const uint32_t count = scene->block.tiles_x * scene->block.tiles_y;
			spudgpu_cmd_dispatch(cmd_list->cmd_list, (count + 63) / 64, 1, 1);
		}
	}
	return true;
}

bool aprend_command_list_compile(aprend_command_list cmd_list) {
	if (!cmd_list)
		return false;
	cmd_list->compiled = false;
	aprend_cmd_list_unlist(cmd_list);
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
	cmd_list->storage_hazards.clear();
	cmd_list->staged_stores.clear();
	cmd_list->used_stores.clear();
	cmd_list->buffer_states.clear();
	cmd_list->present_swap_chain = nullptr;
	// The last compile was not submitted, or this one would find nothing:
	// what it took from the render target pool goes back first.
	aprend_render_target_give_back(cmd_list);
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
	bool ok = false;

	/* Whether a BEGIN_RENDERING pass is currently open. */
	bool in_pass = false;
	/* The BEGIN_RENDERING of the open pass, which every pipeline set in it
	 * must match, and which may time the pass into a timer. */
	const APREND_COMMAND *open_pass = nullptr;
	/* The open labelled regions, innermost last: whether each was opened
	 * inside a pass. */
	std::vector<bool> open_labels;
	/* The pipeline bound in the open pass; none until the pass sets one. */
	aprend_graphics_pipeline bound_pipeline = nullptr;
	/* The stride of the buffer or transient span last put in each vertex
	 * buffer slot; 0 for a slot nothing has been put in. */
	aprend_cmd_vertex_slot vertex_slots[APREND_MAX_VERTEX_BINDINGS]{};
	/* The view bound at each vertex buffer slot in the open pass for the
	 * bound pipeline: a draw binds whatever differs. */
	spudgpu_buffer_view applied_vertex_views[APREND_MAX_VERTEX_BINDINGS]{};
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


	// Every vector of the list and of this walk can throw std::bad_alloc,
	// which must not leave the C entry point: the list is closed below and
	// the compile fails.
	try {
		ok = aprend_cmd_prepare_lit_scenes(cmd_list);

		// Nothing more is recorded once something has failed, the top of the
		// list (aprend_cmd_prepare_lit_scenes) included.
		for (size_t command_index = 0; ok && command_index < cmd_list->commands.size(); ++command_index) {
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
				} else if (!index_buffer_bound && (command._type == APREND_COMMAND_DRAW_INDEXED ||
				                                   command._type == APREND_COMMAND_DRAW_INSTANCED_INDEXED ||
				                                   command._type == APREND_COMMAND_DRAW_INDEXED_INDIRECT)) {
					printf("aprend: indexed draw with no SET_INDEX_BUFFER or SET_TRANSIENT_INDEX_BUFFER before it\n");
					ok = false;
				} else if (!aprend_cmd_apply_vertex_buffers(cmd_list, bound_pipeline, vertex_slots, applied_vertex_views, command)) {
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
				// Bound at each draw, at the stride of its pipeline's binding.
				for (uint32_t i = 0; i < p._vertex_buffer_count; ++i) {
					// aprend_send_command refused a command that runs past the last slot.
					vertex_slots[p._start_slot + i]        = {};
					vertex_slots[p._start_slot + i].buffer = p._vertex_buffers[i];
					aprend_cmd_use_store(cmd_list, &p._vertex_buffers[i]->store);
				}
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
				// Bound at each draw, as a buffer is. aprend_send_command
				// refused a slot past the last.
				vertex_slots[p._slot]          = {};
				vertex_slots[p._slot].has_span = true;
				vertex_slots[p._slot].span     = p._span;
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
				// the next draw binds them again. Vertex buffers do survive one
				// on every API; a draw binds a slot again only where the new
				// pipeline's stride makes its view another.
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
				for (spudgpu_buffer_view &applied : applied_vertex_views)
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
			case APREND_COMMAND_BLOOM: {
				if (in_pass) {
					printf("aprend: BLOOM inside a rendering pass\n");
					ok = false;
					break;
				}
				ok = aprend_cmd_bloom(cmd_list, command);
				// Its passes bind pipelines of their own: a compute pipeline is
				// set again before the next dispatch, as after a pass.
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_TONE_MAP: {
				if (in_pass) {
					printf("aprend: TONE_MAP inside a rendering pass\n");
					ok = false;
					break;
				}
				ok = aprend_cmd_tone_map(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_LIT_ENVIRONMENT: {
				if (in_pass) {
					printf("aprend: LIT_ENVIRONMENT inside a rendering pass\n");
					ok = false;
					break;
				}
				ok = aprend_cmd_lit_environment(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_LIT_SKY:
			case APREND_COMMAND_LIT_AMBIENT_OCCLUSION:
			case APREND_COMMAND_AUTO_EXPOSURE: {
				if (in_pass) {
					printf("aprend: %s inside a rendering pass\n", command._type == APREND_COMMAND_LIT_SKY             ? "LIT_SKY"
					                                               : command._type == APREND_COMMAND_AUTO_EXPOSURE ? "AUTO_EXPOSURE"
					                                                                                                : "LIT_AMBIENT_OCCLUSION");
					ok = false;
					break;
				}
				if (command._type == APREND_COMMAND_LIT_SKY)
					ok = aprend_cmd_lit_sky(cmd_list, command);
				else if (command._type == APREND_COMMAND_AUTO_EXPOSURE)
					ok = aprend_cmd_auto_exposure(cmd_list, command);
				else
					ok = aprend_cmd_lit_ambient_occlusion(cmd_list, command);
				bound_compute_pipeline = nullptr;
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
	} catch (const std::bad_alloc &) {
		printf("aprend: compile: out of memory\n");
		ok = false;
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
	if (ok)
		ok = aprend_cmd_list_enlist(cmd_list);
	if (!ok) {
		aprend_render_target_give_back(cmd_list);
		cmd_list->timer_uses.clear();
		cmd_list->layout_uses.clear();
		cmd_list->storage_hazards.clear();
		cmd_list->staged_stores.clear();
		cmd_list->used_stores.clear();
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
	// Before anything the list uses is read: a texture, buffer, material or
	// GPU timer it was compiled against may be gone.
	{
		std::lock_guard<std::mutex> lock(cmd_list->instance->release_mutex);
		if (cmd_list->stale) {
			printf("aprend: submit refused: a texture, buffer, material or GPU timer the list uses was destroyed or resized since it was compiled; recompile it\n");
			return false;
		}
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
	// uploads for the next. A buffer an upload owns is retired, released
	// once this submission has run.
	for (aprend_buffer_store *store : cmd_list->staged_stores)
		aprend_buffer_store_uploads_submitted(store);
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
	// What it took from the pool is the submission's now, free again once
	// this frame has finished.
	cmd_list->pool_takes.clear();
	cmd_list->compiled = false;
	cmd_list->cmd_list = nullptr;
	aprend_cmd_list_unlist(cmd_list);
	return true;
}
}
