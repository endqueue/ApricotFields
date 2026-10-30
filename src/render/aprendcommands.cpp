
#include "aprend_internal.hpp"
#include "aprendbatch_internal.hpp"
#include "aprendimages_internal.hpp"
#include "aprendlit_internal.hpp"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

aprend_command_list_t::~aprend_command_list_t() {
	// The SpudGPU lists it recorded into are the instance's.
	commands.clear();
}

// [format] and [args] into [out], after "command N: " where [send] isn't
// UINT32_MAX.
static void aprend_cmd_format(char *out, size_t size, uint32_t send, const char *format, va_list args) {
	int written = 0;
	if (send != UINT32_MAX)
		written = snprintf(out, size, "command %u: ", send);
	if (written >= 0 && (size_t)written < size)
		vsnprintf(out + written, size - (size_t)written, format, args);
}

bool aprend_cmd_report(aprend_command_list cmd_list, APRESULT result, const char *format, ...) {
	va_list args;
	va_start(args, format);
	if (cmd_list->reporting) {
		if (cmd_list->report_result == APRESULT_OK) {
			cmd_list->report_result = result;
			if (cmd_list->report)
				aprend_cmd_format(cmd_list->report->message, sizeof(cmd_list->report->message), cmd_list->report_send, format, args);
		}
	} else if (cmd_list->recording_result == APRESULT_OK) {
		// A recording's: the send being made.
		cmd_list->recording_result = result;
		cmd_list->recording_send   = cmd_list->send_count ? cmd_list->send_count - 1 : 0;
		vsnprintf(cmd_list->recording_message, sizeof(cmd_list->recording_message), format, args);
	}
	va_end(args);
	return false;
}

bool aprend_report(aprend_instance instance, APRESULT result, const char *format, ...) {
	aprend_command_list cmd_list = instance->reporting_list;
	if (!cmd_list || !cmd_list->reporting || cmd_list->report_result != APRESULT_OK)
		return false;
	cmd_list->report_result = result;
	if (cmd_list->report) {
		va_list args;
		va_start(args, format);
		aprend_cmd_format(cmd_list->report->message, sizeof(cmd_list->report->message), cmd_list->report_send, format, args);
		va_end(args);
	}
	return false;
}

/* For the length of a compile or submit of [cmd_list]: what fails in it is
 * reported to [error] (aprend_cmd_report, aprend_report). */
struct aprend_cmd_report_scope {
	aprend_command_list cmd_list;
	aprend_cmd_report_scope(aprend_command_list list, APREND_ERROR *error) : cmd_list(list) {
		cmd_list->reporting                 = true;
		cmd_list->report                    = error;
		cmd_list->report_result             = APRESULT_OK;
		cmd_list->report_send               = UINT32_MAX;
		cmd_list->instance->reporting_list = cmd_list;
	}
	~aprend_cmd_report_scope() {
		cmd_list->reporting                 = false;
		cmd_list->report                    = nullptr;
		cmd_list->instance->reporting_list = nullptr;
	}
	// The result of a call that failed: the first failure reported, or
	// [fallback] where nothing was.
	APRESULT failed(APRESULT fallback, const char *message) {
		if (cmd_list->report_result != APRESULT_OK)
			return cmd_list->report_result;
		aprend_cmd_report(cmd_list, fallback, "%s", message);
		return fallback;
	}
};

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

/* Adds [handle] to what [cmd_list]'s compile names (used_handles), for
 * aprend_instance_forget. NULL is ignored. Throws std::bad_alloc. */
static void aprend_cmd_use_handle(aprend_command_list cmd_list, const void *handle) {
	if (!handle)
		return;
	const uint64_t key = (uint64_t)(uintptr_t)handle;
	// The hash is the address: a match is the same handle.
	if (cmd_list->used_handles.find(key, [](uint32_t) { return true; }))
		return;
	cmd_list->used_handles.add(key, 0);
}

/* The fit of a draw-mode PRESENT_TEXTURE or a DRAW_TEXTURE, checked where
 * the command is sent: [scaling] is an APREND_TEXTURE_SCALING, [filter] is
 * LINEAR or NEAREST, and [border] is finite where it is read
 * ([border_read]). False, reported with [who] (the command's name), if not. */
static bool aprend_texture_scaling_valid(
    aprend_command_list cmd_list,
    const char *who,
    APREND_TEXTURE_SCALING scaling,
    SPUDGPU_FILTER filter,
    const float border[4],
    bool border_read) {
	if (scaling > APREND_TEXTURE_SCALING_ASPECT_FIT) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "%s with a _scaling of %u, which is not an APREND_TEXTURE_SCALING", who, (unsigned)scaling);
		return false;
	}
	if (filter != SPUDGPU_FILTER_LINEAR && filter != SPUDGPU_FILTER_NEAREST) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "%s with a _filter of %u, which is not SPUDGPU_FILTER_LINEAR or _NEAREST", who, (unsigned)filter);
		return false;
	}
	if (!border_read)
		return true;
	for (int i = 0; i < 4; ++i) {
		if (!std::isfinite(border[i])) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "%s with a _border_color that is not finite", who);
			return false;
		}
	}
	return true;
}

/* The width and height of the first mip a 2D view of a 2D texture is of. */
static void aprend_view_size(aprend_texture_view view, uint32_t *out_width, uint32_t *out_height) {
	*out_width  = aprend_mip_size(view->texture._t2d->desc.width, view->desc.base_mip);
	*out_height = aprend_mip_size(view->texture._t2d->desc.height, view->desc.base_mip);
}

/* Whether DRAW_TEXTURE's [rect] (its [what]) is zeroed or inside [view]'s
 * mip, its width and height both 0 or both above 0. Prints what is wrong. */
static bool aprend_texel_rect_valid(aprend_command_list cmd_list, const char *what, const APREND_TEXEL_RECT &rect, aprend_texture_view view) {
	if ((rect.width == 0) != (rect.height == 0)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a %s of %ux%u: its width and height are both 0 or both above 0", what, (unsigned)rect.width, (unsigned)rect.height);
		return false;
	}
	if (rect.width == 0) {
		if (rect.x != 0 || rect.y != 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a %s of no size at (%u, %u): a whole one is zeroed", what, (unsigned)rect.x, (unsigned)rect.y);
			return false;
		}
		return true;
	}
	uint32_t width = 0, height = 0;
	aprend_view_size(view, &width, &height);
	if (rect.x >= width || rect.width > width - rect.x) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a %s reaching past its view's width of %u", what, (unsigned)width);
		return false;
	}
	if (rect.y >= height || rect.height > height - rect.y) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a %s reaching past its view's height of %u", what, (unsigned)height);
		return false;
	}
	return true;
}

/* Whether 2D views [a] and [b] are of mips of one size. */
static bool aprend_views_one_size(aprend_texture_view a, aprend_texture_view b) {
	uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
	aprend_view_size(a, &aw, &ah);
	aprend_view_size(b, &bw, &bh);
	return aw == bw && ah == bh;
}

/* Whether [view], a BLOOM's [what] (its _mask or _lens_dirt), is one it can
 * read: of [cmd_list]'s instance, a SHADER_RESOURCE 2D view of color, in a
 * format a shader reads as floats. Prints what is wrong. */
static bool aprend_bloom_read_view_valid(aprend_command_list cmd_list, aprend_texture_view view, const char *what) {
	if (view->instance != cmd_list->instance) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a %s of another instance", what);
		return false;
	}
	if (view->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a %s that is not a SHADER_RESOURCE view", what);
		return false;
	}
	if (view->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a %s that is not a 2D view", what);
		return false;
	}
	if (view->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a %s that is not a view of color", what);
		return false;
	}
	// A 2D view is of a 2D texture.
	if (aprend_format_scalar_type(view->texture._t2d->desc.format) != APREND_SHADER_SCALAR_TYPE_FLOAT) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a %s of format %u, which a shader doesn't read as floats", what, (unsigned)view->texture._t2d->desc.format);
		return false;
	}
	return true;
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
    const eq::array<aprend_upload_copy> &copies,
    spudgpu_command_list *out_upload) {
	*out_upload                                 = nullptr;
	eq::array<spudgpu_buffer_barrier> &before = cmd_list->upload_before;
	eq::array<spudgpu_buffer_barrier> &after  = cmd_list->upload_after;
	before.clear();
	after.clear();
	for (const aprend_upload_copy &copy : copies) {
		bool known = false;
		for (const spudgpu_buffer_barrier &barrier : before)
			if (barrier.buffer == copy.destination)
				known = true;
		if (known)
			continue;
		aprend_push(before, spudgpu_buffer_barrier{{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, copy.destination, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST});
		aprend_push(after, spudgpu_buffer_barrier{{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, copy.destination, SPUDGPU_RESOURCE_STATE_COPY_DEST, SPUDGPU_RESOURCE_STATE_VERTEX_BUFFER});
	}
	for (aprend_buffer_store *store : cmd_list->staged_stores) {
		if (store->uploads.empty())
			continue;
		aprend_push(before, spudgpu_buffer_barrier{{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST});
		aprend_push(after, spudgpu_buffer_barrier{{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, SPUDGPU_RESOURCE_STATE_COPY_DEST, store->use_state});
	}
	// A store whose state the list's barriers track must start the list in
	// its use_state, which the copy above leaves it in. One with nothing to
	// copy is put there directly, so the list starts the same either way.
	for (const auto &tracked : cmd_list->buffer_states) {
		aprend_buffer_store *store = tracked.store;
		if (store->staged && !store->uploads.empty())
			continue;
		aprend_push(before, spudgpu_buffer_barrier{{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, SPUDGPU_RESOURCE_STATE_COMMON, store->use_state});
	}
	if (before.empty())
		return SPUD_SUCCESS;

	spudgpu_command_list upload = nullptr;
	SPUDRESULT sr               = aprend_instance_take_command_list(cmd_list->instance, &upload);
	if (SPUDFAIL(sr))
		return sr;
	spudgpu_begin_command_list(upload);
	spudgpu_cmd_pipeline_barrier(upload, before.data, (uint32_t)before.size(), nullptr, 0);
	// In each store's order: a ONCE store's uploads may overlap, and the
	// later write is the one that has to land.
	for (aprend_buffer_store *store : cmd_list->staged_stores)
		for (const aprend_buffer_upload &staged : store->uploads)
			spudgpu_cmd_copy_buffer(upload, staged.source, store->buffer, staged.source_offset, staged.offset, staged.size);
	for (const aprend_upload_copy &copy : copies)
		spudgpu_cmd_copy_buffer(upload, copy.source, copy.destination, copy.source_offset, copy.destination_offset, copy.size);
	if (!after.empty())
		spudgpu_cmd_pipeline_barrier(upload, after.data, (uint32_t)after.size(), nullptr, 0);
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
 * reported, with no memory for it. */
static bool aprend_cmd_list_enlist(aprend_command_list cmd_list) {
	aprend_instance instance = cmd_list->instance;
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	try {
		instance->compiled_lists.push_back(cmd_list);
	} catch (const std::bad_alloc &) {
		aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "compile: out of memory");
		return false;
	}
	cmd_list->listed = true;
	cmd_list->stale  = false;
	return true;
}

/* Whether the compile of [cmd_list] reads [used] at submit. */
static bool aprend_cmd_list_uses(aprend_command_list cmd_list, const void *used) {
	// A texture's tracking is in used_handles from its first layout_use.
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
	if (cmd_list->present_swap_chain == used)
		return true;
	return cmd_list->used_handles.find((uint64_t)(uintptr_t)used, [](uint32_t) { return true; }) != nullptr;
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

APRESULT aprend_command_list_create(aprend_instance instance, aprend_command_list *out_list, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_list)
		*out_list = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_command_list_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_command_list_create");
	if (!out_list)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_list is NULL");
	aprend_command_list_t *result = (aprend_command_list_t *)malloc(sizeof(aprend_command_list_t));
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the list");
	result = new (result) aprend_command_list_t();

	// What it records into is taken from the instance at each compile.
	result->instance = instance;
	// Its destroy's retire, promised now so the destroy can't fail.
	if (aprend_instance_reserve_release(instance, 1) != APRESULT_OK) {
		result->~aprend_command_list_t();
		free(result);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to release the list later");
	}
	*out_list = result;
	return APRESULT_OK;
}
void aprend_command_list_destroy(aprend_command_list cmd_list) {
	if (!cmd_list)
		return;
	if (APRI_STALE(cmd_list, "aprend_command_list_destroy", "cmd_list"))
		return;
	// A compile it never submitted gives back what it took from the render
	// target pool. Here and not in its release, which may come after the
	// instance's destructor has freed the pool.
	aprend_render_target_give_back(cmd_list);
	// No destroy call may find it among the compiled lists once it is gone.
	aprend_cmd_list_unlist(cmd_list);
	// Released once the GPU has finished everything submitted so far.
	APRI_KILL(cmd_list);
	aprend_instance_retire(cmd_list->instance, cmd_list, &aprend_release_handle<aprend_command_list_t>);
}

void aprend_command_list_reset(aprend_command_list cmd_list) {
	if (!cmd_list)
		return;
	if (APRI_STALE(cmd_list, "aprend_command_list_reset", "cmd_list"))
		return;
	APREND_ENTER_LIST(cmd_list, "aprend_command_list_reset");
	// Off the compiled lists before what it uses is cleared: a destroy call
	// on another thread may be reading it.
	aprend_cmd_list_unlist(cmd_list);
	const size_t preserve_size = cmd_list->commands.size();
	cmd_list->commands.clear();
	cmd_list->commands.reserve(preserve_size);
	cmd_list->side_bytes.clear();
	cmd_list->side_refs.clear();
	cmd_list->recording_error  = false;
	cmd_list->command_sends.clear();
	cmd_list->send_count       = 0;
	cmd_list->recording_result = APRESULT_OK;
	cmd_list->recording_message[0] = '\0';
	cmd_list->layout_uses.clear();
	cmd_list->layout_index.clear();
	cmd_list->storage_hazards.clear();
	cmd_list->staged_stores.clear();
	cmd_list->used_stores.clear();
	cmd_list->used_handles.clear();
	cmd_list->buffer_states.clear();
	cmd_list->materials.clear();
	aprend_render_target_give_back(cmd_list);
	cmd_list->compiled           = false;
	cmd_list->present_swap_chain = nullptr;
}
/* Copies [size] bytes from [bytes] to the end of [cmd_list]'s side_bytes,
 * at an offset aligned for anything a command points at, and returns that
 * offset. Throws std::bad_alloc. */
static uint32_t aprend_cmd_keep(aprend_command_list cmd_list, const void *bytes, size_t size) {
	constexpr size_t align = alignof(std::max_align_t);
	const size_t offset    = (cmd_list->side_bytes.size() + align - 1) & ~(align - 1);
	cmd_list->side_bytes.resize(offset + size);
	memcpy(cmd_list->side_bytes.data() + offset, bytes, size);
	return (uint32_t)offset;
}

/* Points each command that has a copy in [cmd_list]'s side_bytes at it:
 * after recording, when side_bytes no longer grows, and before anything
 * reads a command's array. */
static void aprend_cmd_point_at_copies(aprend_command_list cmd_list) {
	uint8_t *bytes = cmd_list->side_bytes.data();
	for (const aprend_command_list_t::side_ref &ref : cmd_list->side_refs) {
		APREND_COMMAND &cmd = cmd_list->commands[ref.command];
		void *copy          = bytes + ref.offset;
		switch (cmd._type) {
		case APREND_COMMAND_BEGIN_RENDERING:
			cmd._params._begin_rendering._color_targets = (APREND_COLOR_TARGET *)copy;
			break;
		case APREND_COMMAND_BEGIN_LABEL:
			cmd._params._begin_label._label = (const char *)copy;
			break;
		case APREND_COMMAND_SET_VERTEX_BUFFERS:
			cmd._params._set_vertex_buffers._vertex_buffers = (aprend_buffer *)copy;
			break;
		case APREND_COMMAND_SET_VIEWPORTS:
			cmd._params._set_viewports._viewports = (SPUDGPU_VIEWPORT *)copy;
			break;
		case APREND_COMMAND_SET_SCISSOR_RECTS:
			cmd._params._set_scissor_rects._scissor_rects = (SPUDGPU_SCISSOR_RECT *)copy;
			break;
		case APREND_COMMAND_PUSH_CONSTANTS:
			cmd._params._push_constants._data = copy;
			break;
		default:
			break;
		}
	}
}

/* aprend_command_list_send's checks and copies. Throws std::bad_alloc from the
 * list's vectors, which aprend_command_list_send catches. */
static void aprend_send_command_recorded(
    aprend_command_list cmd_list,
    APREND_COMMAND cmd) {
	// Where this command's copy is in side_bytes, if it has one: recorded
	// in side_refs with the command, once every check has passed.
	bool has_copy        = false;
	uint32_t copy_offset = 0;
	auto keep            = [&](const void *bytes, size_t size) {
		copy_offset = aprend_cmd_keep(cmd_list, bytes, size);
		has_copy    = true;
		return (void *)(cmd_list->side_bytes.data() + copy_offset);
	};

	// The three commands that stand for others are expanded into them here,
	// from the pass or list as it is now, and are not recorded themselves.
	if (cmd._type == APREND_COMMAND_BEGIN_RENDER_PASS) {
		aprend_render_pass pass = cmd._params._begin_render_pass._pass;
		if (!pass) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDER_PASS with a NULL pass");
			cmd_list->recording_error = true;
			return;
		}
		if (pass->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDER_PASS with a pass of another instance");
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
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_COMPUTE_PASS with a NULL pass");
			cmd_list->recording_error = true;
			return;
		}
		if (pass->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_COMPUTE_PASS with a pass of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_compute_pass_expand(cmd_list, pass))
			cmd_list->recording_error = true;
		return;
	}
	if (cmd._type == APREND_COMMAND_DRAW_BATCH) {
		const auto &p = cmd._params._draw_batch;
		if (!p._batch) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_BATCH with a NULL batch");
			cmd_list->recording_error = true;
			return;
		}
		if (p._batch->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_BATCH with a batch of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_batch_expand(cmd_list, p._batch))
			cmd_list->recording_error = true;
		return;
	}
	if (cmd._type == APREND_COMMAND_DRAW_LIST) {
		const auto &p = cmd._params._draw_list;
		if (!p._list) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_LIST with a NULL list");
			cmd_list->recording_error = true;
			return;
		}
		if (p._list->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_LIST with a list of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._material_slot != APREND_DRAW_LIST_NO_MATERIAL && p._material_slot >= APREND_MAX_BINDING_LAYOUTS) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_LIST with _material_slot %u, at or above APREND_MAX_BINDING_LAYOUTS", p._material_slot);
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
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_MATERIAL with a NULL material");
			cmd_list->recording_error = true;
			return;
		}
		if (p._material->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_MATERIAL with a material of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._slot >= APREND_MAX_BINDING_LAYOUTS) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_MATERIAL with _slot %u, at or above APREND_MAX_BINDING_LAYOUTS", p._slot);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_BEGIN_RENDERING) {
		auto &p = cmd._params._begin_rendering;
		if (p._color_target_count > APREND_MAX_COLOR_TARGETS || (p._color_target_count > 0 && !p._color_targets)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING with invalid color targets (count %u)", p._color_target_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._timer && p._timer->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING with a _timer of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._timer && p._timer_slot >= p._timer->slot_count) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING with _timer_slot %u, at or above the timer's %u slots", p._timer_slot, p._timer->slot_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._color_target_count > 0) {
			p._color_targets = (APREND_COLOR_TARGET *)keep(p._color_targets, sizeof(APREND_COLOR_TARGET) * p._color_target_count);
		} else {
			p._color_targets = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_BEGIN_LABEL) {
		auto &p = cmd._params._begin_label;
		const char *label = p._label ? p._label : "";
		p._label          = (const char *)keep(label, strlen(label) + 1);
	}

	// The array commands below are copied the same way: the caller's array
	// only has to live for this call.
	if (cmd._type == APREND_COMMAND_SET_VERTEX_BUFFERS) {
		auto &p = cmd._params._set_vertex_buffers;
		if (p._vertex_buffer_count > 0 && !p._vertex_buffers) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS with a NULL array (count %u)", p._vertex_buffer_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._start_slot > APREND_MAX_VERTEX_BINDINGS) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS with _start_slot %u, above APREND_MAX_VERTEX_BINDINGS", p._start_slot);
			cmd_list->recording_error = true;
			return;
		}
		if (p._vertex_buffer_count > APREND_MAX_VERTEX_BINDINGS - p._start_slot) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS of %u buffers from slot %u runs past the last vertex buffer slot", p._vertex_buffer_count, p._start_slot);
			cmd_list->recording_error = true;
			return;
		}
		for (uint32_t i = 0; i < p._vertex_buffer_count; ++i) {
			if (!p._vertex_buffers[i]) {
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS vertex buffer %u is NULL", i);
				cmd_list->recording_error = true;
				return;
			}
			if (p._vertex_buffers[i]->instance != cmd_list->instance) {
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS vertex buffer %u is of another instance", i);
				cmd_list->recording_error = true;
				return;
			}
			if (!(p._vertex_buffers[i]->desc.usage & APREND_BUFFER_USAGE_VERTEX)) {
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS vertex buffer %u was created without APREND_BUFFER_USAGE_VERTEX", i);
				cmd_list->recording_error = true;
				return;
			}
		}
		if (p._vertex_buffer_count > 0) {
			p._vertex_buffers = (aprend_buffer *)keep(p._vertex_buffers, sizeof(aprend_buffer) * p._vertex_buffer_count);
		} else {
			p._vertex_buffers = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_INDEX_BUFFER) {
		const auto &p = cmd._params._set_index_buffer;
		if (!p._index_buffer) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_INDEX_BUFFER with a NULL buffer");
			cmd_list->recording_error = true;
			return;
		}
		if (p._index_buffer->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_INDEX_BUFFER with a buffer of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!(p._index_buffer->desc.usage & APREND_BUFFER_USAGE_INDEX)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_INDEX_BUFFER with a buffer created without APREND_BUFFER_USAGE_INDEX");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER) {
		const auto &p = cmd._params._set_transient_vertex_buffer;
		if (p._slot >= APREND_MAX_VERTEX_BINDINGS) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_TRANSIENT_VERTEX_BUFFER with _slot %u, at or above APREND_MAX_VERTEX_BINDINGS", p._slot);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER) {
		const auto &p = cmd._params._set_transient_index_buffer;
		if (p._index_size != 2 && p._index_size != 4) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_TRANSIENT_INDEX_BUFFER with an _index_size of %u, which is not 2 or 4", p._index_size);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_VIEWPORTS) {
		auto &p = cmd._params._set_viewports;
		if (p._viewport_count > 0 && !p._viewports) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_VIEWPORTS with a NULL array (count %u)", p._viewport_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._viewport_count > 0) {
			p._viewports = (SPUDGPU_VIEWPORT *)keep(p._viewports, sizeof(SPUDGPU_VIEWPORT) * p._viewport_count);
		} else {
			p._viewports = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_SCISSOR_RECTS) {
		auto &p = cmd._params._set_scissor_rects;
		if (p._scissor_rect_count > 0 && !p._scissor_rects) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_SCISSOR_RECTS with a NULL array (count %u)", p._scissor_rect_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._scissor_rect_count > 0) {
			p._scissor_rects = (SPUDGPU_SCISSOR_RECT *)keep(p._scissor_rects, sizeof(SPUDGPU_SCISSOR_RECT) * p._scissor_rect_count);
		} else {
			p._scissor_rects = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_DRAW_INDIRECT || cmd._type == APREND_COMMAND_DRAW_INDEXED_INDIRECT) {
		const auto &p = cmd._params._draw_indirect;
		// What one entry is depends on which of the two commands this is.
		const uint64_t entry_size = cmd._type == APREND_COMMAND_DRAW_INDIRECT ? sizeof(spudgpu_draw_indirect_args)
		                                                                       : sizeof(spudgpu_draw_indexed_indirect_args);
		if (!p._buffer) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indirect draw with a NULL _buffer");
			cmd_list->recording_error = true;
			return;
		}
		if (p._buffer->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indirect draw with a _buffer of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!(p._buffer->desc.usage & APREND_BUFFER_USAGE_INDIRECT_ARGUMENTS)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indirect draw with a buffer created without APREND_BUFFER_USAGE_INDIRECT_ARGUMENTS");
			cmd_list->recording_error = true;
			return;
		}
		// Every copy has one size.
		const uint64_t arguments_size = p._buffer->desc.size;
		if (p._draw_count == 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indirect draw with a _draw_count of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset % 4 != 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indirect draw with an _offset that is not a multiple of 4");
			cmd_list->recording_error = true;
			return;
		}
		if (p._stride % 4 != 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indirect draw with _stride %u, not a multiple of 4", p._stride);
			cmd_list->recording_error = true;
			return;
		}
		if (p._stride < entry_size) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indirect draw with _stride %u, smaller than the %u bytes of one entry", p._stride, (unsigned)entry_size);
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset > arguments_size) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indirect draw with an _offset past the end of the buffer");
			cmd_list->recording_error = true;
			return;
		}
		// Where the last entry ends: every one before it is a whole stride.
		const uint64_t span = (uint64_t)(p._draw_count - 1) * p._stride + entry_size;
		if (span > arguments_size - p._offset) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indirect draw of %u entries runs past the end of the buffer", p._draw_count);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_DISPATCH_MESH) {
		const auto &p = cmd._params._dispatch_mesh;
		if (p._group_count_x == 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DISPATCH_MESH with a _group_count_x of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_y == 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DISPATCH_MESH with a _group_count_y of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_z == 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DISPATCH_MESH with a _group_count_z of 0");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_DISPATCH) {
		const auto &p = cmd._params._dispatch;
		if (p._group_count_x == 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DISPATCH with a _group_count_x of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_y == 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DISPATCH with a _group_count_y of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_z == 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DISPATCH with a _group_count_z of 0");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_PUSH_CONSTANTS) {
		auto &p = cmd._params._push_constants;
		if (!p._data) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS with NULL _data");
			cmd_list->recording_error = true;
			return;
		}
		if (p._size == 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS with a _size of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset % 4 != 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS with _offset %u, not a multiple of 4", p._offset);
			cmd_list->recording_error = true;
			return;
		}
		if (p._size % 4 != 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS with _size %u, not a multiple of 4", p._size);
			cmd_list->recording_error = true;
			return;
		}
		p._data = keep(p._data, p._size);
	}
	if (cmd._type == APREND_COMMAND_BLOOM) {
		const auto &p = cmd._params._bloom;
		if (!p._source) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a NULL source");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a source of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a source that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a source that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a source that is not a view of color");
			cmd_list->recording_error = true;
			return;
		}
		if (!p._target) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a NULL target");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a target of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a target that is not a RENDER_TARGET view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a target that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		// A 2D render target view is of a 2D texture.
		if (!aprend_bloom_target_format(p._target->texture._t2d->desc.format)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a target in format %d, which a bloom doesn't render onto", (int)p._target->texture._t2d->desc.format);
			cmd_list->recording_error = true;
			return;
		}
		if (aprend_texture_ranges_overlap(aprend_texture_view_range(p._source), aprend_texture_view_range(p._target))) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a source and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._threshold) && p._threshold >= 0.0f)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a _threshold that is negative or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._knee) && p._knee >= 0.0f)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a _knee that is negative or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._radius) && p._radius > 0.0f)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a _radius that is not above 0, or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._intensity) && p._intensity >= 0.0f)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with an _intensity that is negative or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (p._levels == 0) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with _levels 0");
			cmd_list->recording_error = true;
			return;
		}
		const uint32_t most = aprend_bloom_max_levels(p._source);
		if (p._levels > most) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with _levels %u, and its source's size allows at most %u", p._levels, most);
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask && !aprend_bloom_read_view_valid(cmd_list, p._mask, "_mask")) {
			cmd_list->recording_error = true;
			return;
		}
		if (p._lens_dirt && !aprend_bloom_read_view_valid(cmd_list, p._lens_dirt, "_lens_dirt")) {
			cmd_list->recording_error = true;
			return;
		}
		if (p._lens_dirt && aprend_texture_ranges_overlap(aprend_texture_view_range(p._lens_dirt), aprend_texture_view_range(p._target))) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a _lens_dirt and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		if (p._lens_dirt && !(std::isfinite(p._lens_dirt_intensity) && p._lens_dirt_intensity >= 0.0f)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM with a _lens_dirt_intensity that is negative or not a number");
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_GENERATE_MIPS) {
		const auto &p = cmd._params._generate_mips;
		if (!p._texture) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "GENERATE_MIPS with a NULL texture");
			cmd_list->recording_error = true;
			return;
		}
		if (p._texture->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "GENERATE_MIPS with a texture of another instance");
			cmd_list->recording_error = true;
			return;
		}
		APREND_ERROR error{};
		if (aprend_generate_mips_valid(p._texture, p._base_mip, p._filter, &error) != APRESULT_OK) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "GENERATE_MIPS: %s", error.message);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_PRESENT_TEXTURE) {
		const auto &p = cmd._params._present_texture;
		// What the draw modes read; NONE, the copy, reads neither.
		if (p._scaling != APREND_TEXTURE_SCALING_NONE &&
		    !aprend_texture_scaling_valid(cmd_list, "PRESENT_TEXTURE", p._scaling, p._filter, p._border_color,
		                                  p._scaling == APREND_TEXTURE_SCALING_ASPECT_FIT)) {
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_DRAW_TEXTURE) {
		const auto &p = cmd._params._draw_texture;
		if (!p._source) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a NULL source");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source that is not a view of color");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.mip_count != 1) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source of %u mips; it reads one", p._source->desc.mip_count);
			cmd_list->recording_error = true;
			return;
		}
		if (!p._target) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a NULL target");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a target of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a target that is not a RENDER_TARGET view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a target that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		// A 2D render target view is of a 2D texture. The shader writes
		// floats, which an integer target doesn't hold.
		const SPUDGPU_FORMAT target_format = p._target->texture._t2d->desc.format;
		if (aprend_format_scalar_type(target_format) != APREND_SHADER_SCALAR_TYPE_FLOAT) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a target of format %u, which doesn't hold floats", (unsigned)target_format);
			cmd_list->recording_error = true;
			return;
		}
		if (aprend_texture_ranges_overlap(aprend_texture_view_range(p._source), aprend_texture_view_range(p._target))) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_texture_scaling_valid(cmd_list, "DRAW_TEXTURE", p._scaling, p._filter, p._border_color, p._scaling != APREND_TEXTURE_SCALING_STRETCH)) {
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_texel_rect_valid(cmd_list, "_source_rect", p._source_rect, p._source)) {
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_texel_rect_valid(cmd_list, "_target_rect", p._target_rect, p._target)) {
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_COPY_SCENE_COLOR) {
		const auto &p = cmd._params._copy_scene_color;
		const char *wrong = nullptr;
		if (!p._source)
			wrong = "a NULL source";
		else if (p._source->instance != cmd_list->instance)
			wrong = "a source of another instance";
		else if (p._source->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE)
			wrong = "a source that is not a SHADER_RESOURCE view";
		else if (p._source->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D)
			wrong = "a source that is not a 2D view";
		else if (p._source->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR)
			wrong = "a source that is not a view of color";
		else if (p._source->desc.mip_count != 1)
			wrong = "a source of more than one mip";
		else if (!p._target)
			wrong = "a NULL target";
		else if (p._target->instance != cmd_list->instance)
			wrong = "a target of another instance";
		else if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET)
			wrong = "a target that is not a RENDER_TARGET view";
		else if (p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D)
			wrong = "a target that is not a 2D view";
		else if (aprend_format_scalar_type(p._target->texture._t2d->desc.format) != APREND_SHADER_SCALAR_TYPE_FLOAT)
			wrong = "a target in a format that doesn't hold floats";
		else if (aprend_texture_ranges_overlap(aprend_texture_view_range(p._source), aprend_texture_view_range(p._target)))
			wrong = "a source and a target that share a mip of a layer";
		if (wrong) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "COPY_SCENE_COLOR with %s", wrong);
			cmd_list->recording_error = true;
			return;
		}
		APREND_ERROR error{};
		if (aprend_generate_mips_valid(p._target->texture._t2d, p._target->desc.base_mip, SPUDGPU_FILTER_LINEAR, &error) != APRESULT_OK) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "COPY_SCENE_COLOR: %s", error.message);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_BLUR) {
		const auto &p = cmd._params._blur;
		if (!p._source) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a NULL source");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a source of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a source that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a source that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a source that is not a view of color");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.mip_count != 1) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a source of %u mips; it reads one", p._source->desc.mip_count);
			cmd_list->recording_error = true;
			return;
		}
		if (!p._target) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a NULL target");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a target of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a target that is not a RENDER_TARGET view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a target that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		// A 2D render target view is of a 2D texture.
		if (!aprend_builtin_downsample_format(p._target->texture._t2d->desc.format)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a target in format %d, which a blur doesn't render onto", (int)p._target->texture._t2d->desc.format);
			cmd_list->recording_error = true;
			return;
		}
		// A paired tap reads two texels only on the target's own grid.
		uint32_t source_width = 0, source_height = 0, target_width = 0, target_height = 0;
		aprend_view_size(p._source, &source_width, &source_height);
		aprend_view_size(p._target, &target_width, &target_height);
		if (source_width != target_width) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a source %u wide and a target %u wide; they are one size", source_width, target_width);
			cmd_list->recording_error = true;
			return;
		}
		if (source_height != target_height) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a source %u high and a target %u high; they are one size", source_height, target_height);
			cmd_list->recording_error = true;
			return;
		}
		if (aprend_texture_ranges_overlap(aprend_texture_view_range(p._source), aprend_texture_view_range(p._target))) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a source and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._sigma) && p._sigma > 0.0f)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a _sigma that is not above 0, or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (p._sigma > APREND_BLUR_MAX_SIGMA) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR with a _sigma of %g, above APREND_BLUR_MAX_SIGMA", (double)p._sigma);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_OUTLINE) {
		const auto &p = cmd._params._outline;
		if (!p._mask) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a NULL mask");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a mask of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a mask that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a mask that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a mask that is not a view of color");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->desc.mip_count != 1) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a mask of %u mips; it reads one", p._mask->desc.mip_count);
			cmd_list->recording_error = true;
			return;
		}
		if (aprend_format_scalar_type(p._mask->texture._t2d->desc.format) != APREND_SHADER_SCALAR_TYPE_FLOAT) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a mask of format %u, which a shader doesn't read as floats", (unsigned)p._mask->texture._t2d->desc.format);
			cmd_list->recording_error = true;
			return;
		}
		if (!p._target) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a NULL target");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a target of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a target that is not a RENDER_TARGET view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a target that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_builtin_downsample_format(p._target->texture._t2d->desc.format)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a target in format %d, which an outline doesn't render onto", (int)p._target->texture._t2d->desc.format);
			cmd_list->recording_error = true;
			return;
		}
		if (!aprend_views_one_size(p._mask, p._target)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a mask and a target of different sizes");
			cmd_list->recording_error = true;
			return;
		}
		if (aprend_texture_ranges_overlap(aprend_texture_view_range(p._mask), aprend_texture_view_range(p._target))) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a mask and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		for (int i = 0; i < 4; ++i) {
			if (!std::isfinite(p._color[i])) {
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a _color[%d] that is not finite", i);
				cmd_list->recording_error = true;
				return;
			}
		}
		for (int i = 0; i < 3; ++i) {
			if (p._color[i] < 0.0f) {
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a negative _color[%d]", i);
				cmd_list->recording_error = true;
				return;
			}
		}
		if (p._color[3] < 0.0f) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a _color alpha below 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._color[3] > 1.0f) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a _color alpha above 1");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._width) && p._width > 0.0f)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE with a _width that is not above 0, or not a number");
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_LIT_CULL_LIGHTS) {
		const auto &p = cmd._params._lit_cull_lights;
		if (!p._scene) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a NULL scene");
			cmd_list->recording_error = true;
			return;
		}
		if (p._scene->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a scene of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!p._depth) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a NULL depth");
			cmd_list->recording_error = true;
			return;
		}
		if (p._depth->instance != cmd_list->instance) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a depth of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._depth->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a depth that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._depth->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a depth that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._depth->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_DEPTH) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a depth that is not a view of depth");
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
		else if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET || p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D)
			fault = "a target that is not a RENDER_TARGET 2D view";
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
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "TONE_MAP with %s", fault);
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
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_ENVIRONMENT with %s", fault);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == APREND_COMMAND_LIT_SKY) {
		const auto &p = cmd._params._lit_sky;
		const char *fault = nullptr;
		const float *d    = p._sun_direction;
		const float *g    = p._ground_albedo;
		const bool hosek  = p._model == APREND_LIT_SKY_MODEL_HOSEK_WILKIE;
		if (!p._environment)
			fault = "a NULL environment";
		else if (p._environment->instance != cmd_list->instance)
			fault = "an environment of another instance";
		else if (p._model != APREND_LIT_SKY_MODEL_PREETHAM && !hosek)
			fault = "a _model that is not an APREND_LIT_SKY_MODEL";
		else if (!(std::isfinite(d[0]) && std::isfinite(d[1]) && std::isfinite(d[2])) || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] == 0.0f)
			fault = "a _sun_direction of 0, or not a number";
		else if (!hosek && !(p._turbidity >= 1.7f && p._turbidity <= 10.0f))
			fault = "a _turbidity outside 1.7 to 10, Preetham's range";
		else if (hosek && !(p._turbidity >= 1.0f && p._turbidity <= 10.0f))
			fault = "a _turbidity outside 1 to 10, Hosek and Wilkie's range";
		else if (hosek && !(g[0] <= 1.0f && g[1] <= 1.0f && g[2] <= 1.0f))
			fault = "a _ground_albedo above 1, past Hosek and Wilkie's range";
		else if (!(std::isfinite(p._intensity) && p._intensity >= 0.0f))
			fault = "an _intensity that is negative or not a number";
		else if (!(g[0] >= 0.0f && g[1] >= 0.0f && g[2] >= 0.0f && std::isfinite(g[0]) && std::isfinite(g[1]) && std::isfinite(g[2])))
			fault = "a _ground_albedo that is negative or not a number";
		if (fault) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_SKY with %s", fault);
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
		else if (p._target->view_type != APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET || p._target->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D ||
		         !aprend_builtin_downsample_format(p._target->texture._t2d->desc.format))
			fault = "a target that is not a RENDER_TARGET 2D view of a format it renders onto";
		else if (aprend_mip_size(p._depth->texture._t2d->desc.width, p._depth->desc.base_mip) !=
		             aprend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip) ||
		         aprend_mip_size(p._depth->texture._t2d->desc.height, p._depth->desc.base_mip) !=
		             aprend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip))
			fault = "a depth and a target of different sizes";
		else if (!(std::isfinite(p._radius) && p._radius > 0.0f) || !(std::isfinite(p._intensity) && p._intensity >= 0.0f))
			fault = "a _radius that is not above 0, or an _intensity below 0, or either not a number";
		if (fault) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_AMBIENT_OCCLUSION with %s", fault);
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
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "AUTO_EXPOSURE with %s", fault);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (has_copy)
		cmd_list->side_refs.push_back({(uint32_t)cmd_list->commands.size(), copy_offset});
	// The send first: if the command can't follow it, it is taken back.
	cmd_list->command_sends.push_back(cmd_list->send_count - 1);
	try {
		cmd_list->commands.emplace_back(cmd);
	} catch (...) {
		cmd_list->command_sends.pop_back();
		throw;
	}
}
void aprend_command_list_send(
    aprend_command_list cmd_list,
    APREND_COMMAND cmd) {
	if (!cmd_list)
		return;
	if (APRI_STALE(cmd_list, "aprend_command_list_send", "cmd_list"))
		return;
	APREND_ENTER_LIST(cmd_list, "aprend_command_list_send");
	if (cmd._type == APREND_COMMAND_NONE)
		return;
	// The caller's sends are counted; what an expansion sends is the send
	// it expands.
	if (cmd_list->send_depth == 0)
		++cmd_list->send_count;
	++cmd_list->send_depth;
	// Every array copy and the command itself grow a vector, and no C++
	// exception leaves a C entry point: a command that can't be kept fails
	// the list's next compile, as a malformed one does. Whatever of it was
	// copied is freed by the reset.
	try {
		aprend_send_command_recorded(cmd_list, cmd);
	} catch (const std::bad_alloc &) {
		aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "no memory to keep the command; the list fails its compile until it is reset");
		cmd_list->recording_error = true;
	}
	--cmd_list->send_depth;
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
	const uint64_t hash = aprend_subresource_hash(range.image, mip, layer);
	const uint32_t *found = cmd_list->layout_index.find(hash, [&](uint32_t at) {
		const aprend_command_list_t::layout_use &use = cmd_list->layout_uses[at];
		return use.image == range.image && use.mip == mip && use.layer == layer;
	});
	if (found)
		return *found;
	SPUDGPU_IMAGE_LAYOUT *tracked = range.tracking->at(mip, layer);
	cmd_list->layout_uses.push_back({range.image, mip, layer, tracked, range.tracking, *tracked, *tracked, true, true});
	const size_t index = cmd_list->layout_uses.size() - 1;
	cmd_list->layout_index.add(hash, (uint32_t)index);
	// What aprend_instance_forget looks for when the texture goes.
	aprend_cmd_use_handle(cmd_list, range.tracking);
	return index;
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
 * one barrier over the range; where they differ, each run of mips of a layer
 * that share a layout is one barrier from it.
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
		uint32_t mip = 0;
		while (mip < range.mip_count) {
			// The run of mips from [mip] in one layout: one barrier from it,
			// or none where it is there already and that is allowed.
			const size_t first             = aprend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer);
			const SPUDGPU_IMAGE_LAYOUT run = cmd_list->layout_uses[first].final_layout;
			uint32_t end                   = mip + 1;
			while (!uniform && end < range.mip_count &&
			       cmd_list->layout_uses[aprend_cmd_layout_use(cmd_list, range, range.base_mip + end, range.base_layer + layer)].final_layout == run)
				++end;
			bool barrier = uniform;
			if (!uniform && !(skip_if_same && run == layout)) {
				spudgpu_image_view_desc_subresource_range runs{};
				runs.base_mip_level    = range.base_mip + mip;
				runs.mip_level_count   = end - mip;
				runs.base_array_layer  = range.base_layer + layer;
				runs.array_layer_count = 1;
				spudgpu_cmd_image_barrier_subresource(cmd_list->cmd_list, range.image, &runs, run, layout);
				barrier = true;
			}
			for (; mip < end; ++mip) {
				auto &use = cmd_list->layout_uses[aprend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer)];
				// A barrier orders everything before it, storage writes included.
				if (barrier) {
					use.storage_accessed = false;
					use.storage_written  = false;
				}
				use.final_layout = layout;
			}
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
static bool aprend_cmd_range_written(aprend_command_list cmd_list, const aprend_texture_range &range);

/* [rect] made whole: a zeroed one is all of a mip [width] by [height]. */
static APREND_TEXEL_RECT aprend_texel_rect_resolved(const APREND_TEXEL_RECT *rect, uint32_t width, uint32_t height) {
	if (rect && rect->width != 0)
		return *rect;
	APREND_TEXEL_RECT whole{};
	whole.width  = width;
	whole.height = height;
	return whole;
}

/* A draw of [source_rect] of [source] (a SHADER_RESOURCE 2D view of one mip
 * of color) into [target_rect] of [target], a view [width] by [height] of
 * [format], fitted as [scaling] says: what DRAW_TEXTURE records, and
 * PRESENT_TEXTURE's draw modes onto a back buffer. A NULL or zeroed
 * rectangle is the whole mip; the target outside its rectangle keeps what
 * it held. Moves the source to SHADER_READ_ONLY; the target is the
 * caller's to have in COLOR_ATTACHMENT_OPTIMAL, and to move after. False,
 * reported with [who], if the pipeline or the set can't be made. Recorded
 * outside any pass. Throws std::bad_alloc. */
static bool aprend_cmd_draw_fitted(
    aprend_command_list cmd_list,
    const char *who,
    aprend_texture_view source,
    const APREND_TEXEL_RECT *source_rect,
    spudgpu_image_view target,
    SPUDGPU_FORMAT format,
    uint32_t width,
    uint32_t height,
    const APREND_TEXEL_RECT *target_rect,
    APREND_TEXTURE_SCALING scaling,
    SPUDGPU_FILTER filter,
    const float border[4]) {
	const aprend_texture_range range = aprend_texture_view_range(source);
	if (!aprend_cmd_range_written(cmd_list, range)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "%s: the source has nothing in it, from before the list or earlier in it", who);
		return false;
	}
	aprend_graphics_pipeline pipeline = aprend_builtin_present_pipeline(cmd_list->instance, format);
	if (!pipeline)
		return false;
	aprend_binding_set set = aprend_texture_view_present_set(source, filter);
	if (!set)
		return false;
	uint32_t source_width = 0, source_height = 0;
	aprend_view_size(source, &source_width, &source_height);
	const APREND_TEXEL_RECT from = aprend_texel_rect_resolved(source_rect, source_width, source_height);
	const APREND_TEXEL_RECT to   = aprend_texel_rect_resolved(target_rect, width, height);
	aprend_present_params params{};
	memcpy(params.border, border, sizeof(params.border));
	params.target_offset[0] = (float)to.x;
	params.target_offset[1] = (float)to.y;
	params.target_size[0]   = (float)to.width;
	params.target_size[1]   = (float)to.height;
	params.source_offset[0] = (float)from.x;
	params.source_offset[1] = (float)from.y;
	params.source_size[0]   = (float)from.width;
	params.source_size[1]   = (float)from.height;
	params.mode             = (uint32_t)scaling;
	aprend_cmd_transition_range(cmd_list, range, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	// Every texel of the target rectangle is written: what the target held
	// is loaded only where the rectangle leaves some of it.
	const bool whole                 = to.x == 0 && to.y == 0 && to.width == width && to.height == height;
	const uint32_t region[4]         = {to.x, to.y, to.width, to.height};
	const aprend_binding_set sets[1] = {set};
	aprend_record_fullscreen_pass_region(cmd_list->cmd_list, pipeline, target, width, height, region,
	                                     whole ? SPUDGPU_LOAD_OP_DONT_CARE : SPUDGPU_LOAD_OP_LOAD, sets, 1, 0, &params, sizeof(params));
	return true;
}

/* DRAW_TEXTURE, outside a pass: the source drawn into the target, left in
 * COLOR_ATTACHMENT_OPTIMAL. */
static bool aprend_cmd_draw_texture(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p          = command._params._draw_texture;
	const aprend_texture2d target = p._target->texture._t2d;
	const uint32_t width   = aprend_mip_size(target->desc.width, p._target->desc.base_mip);
	const uint32_t height  = aprend_mip_size(target->desc.height, p._target->desc.base_mip);
	aprend_cmd_transition_view(cmd_list, p._target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	return aprend_cmd_draw_fitted(cmd_list, "DRAW_TEXTURE", p._source, &p._source_rect, p._target->image_view, target->desc.format, width,
	                              height, &p._target_rect, p._scaling, p._filter, p._border_color);
}

/* The view a draw-mode PRESENT_TEXTURE reads [texture] through: mip 0 of
 * layer 0, made the first time and kept by the texture
 * (aprend_texture2d_t::present_view). NULL, reported, if it can't be. */
static aprend_texture_view aprend_texture2d_present_view(aprend_texture2d texture) {
	if (texture->present_view)
		return texture->present_view;
	APREND_TEXTURE_VIEW_DESC desc{};
	desc.base        = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
	desc.type        = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
	desc.shape       = APREND_TEXTURE_VIEW_SHAPE_2D;
	desc.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
	desc.base_mip    = 0;
	desc.mip_count   = 1;
	desc.base_layer  = 0;
	desc.layer_count = 1;
	(void)aprend_texture_view_create_2d(texture, &desc, &texture->present_view, nullptr);
	if (!texture->present_view)
		aprend_report(texture->instance, APRESULT_APREND_GPU_FAILURE, "PRESENT_TEXTURE: the view the source is drawn through can't be created");
	return texture->present_view;
}

static bool aprend_cmd_present_texture(
    aprend_command_list cmd_list,
    const APREND_COMMAND &command) {
	const auto &p         = command._params._present_texture;
	aprend_swap_chain sc  = p._swap_chain;
	aprend_texture2d src  = p._source;
	if (!sc || !sc->swap_chain || !src) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE with a NULL source or swap chain");
		return false;
	}
	if (sc->acquired_image == APREND_SWAP_CHAIN_NO_IMAGE || sc->submitted) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE without a freshly acquired back buffer (call aprend_swap_chain_acquire first)");
		return false;
	}
	if (cmd_list->present_swap_chain) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "more than one PRESENT_TEXTURE in one command list");
		return false;
	}

	spudgpu_image_view back_view = spudgpu_get_swap_chain_image_view(sc->swap_chain, sc->acquired_image);
	spudgpu_image_view_desc back_view_desc{};
	back_view_desc.base.type = SPUD_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	if (!back_view || SPUDFAIL(spudgpu_get_image_view_desc(back_view, &back_view_desc)) || !back_view_desc.parent_image) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE couldn't get the acquired back buffer");
		return false;
	}
	// The real back-buffer size, which the surface can override on Vulkan -
	// not necessarily the size the swap chain was requested at.
	spudgpu_swap_chain_desc actual{};
	actual.base.type = SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	if (SPUDFAIL(spudgpu_get_swap_chain_desc(sc->swap_chain, &actual)) || !actual.width || !actual.height) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE couldn't get the back buffer size");
		return false;
	}

	// The draw modes: the source fitted into the back buffer, any size and
	// format of it taken, the back buffers' written.
	if (p._scaling != APREND_TEXTURE_SCALING_NONE) {
		if (!(src->desc.usage & APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE with a _scaling that draws, and a source without APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE");
			return false;
		}
		aprend_texture_view view = aprend_texture2d_present_view(src);
		if (!view)
			return false;
		aprend_cmd_use_handle(cmd_list, view);
		spudgpu_cmd_image_barrier_view(cmd_list->cmd_list, back_view, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		if (!aprend_cmd_draw_fitted(cmd_list, "PRESENT_TEXTURE", view, NULL, back_view, actual.format, actual.width, actual.height, NULL,
		                            p._scaling, p._filter, p._border_color))
			return false;
		spudgpu_cmd_image_barrier_view(cmd_list->cmd_list, back_view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, SPUDGPU_IMAGE_LAYOUT_PRESENT_SRC);
		cmd_list->present_swap_chain  = sc;
		cmd_list->present_image_index = sc->acquired_image;
		cmd_list->present_generation  = sc->generation;
		return true;
	}

	// The copy converts nothing on D3D12 and Metal, so a source of another
	// format would be undefined there; and a smaller one would leave part of
	// what is shown undefined, from a back buffer moved out of UNDEFINED.
	if (src->desc.format != actual.format) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE with a source of format %u, and the back buffers are of format %u", (unsigned)src->desc.format, (unsigned)actual.format);
		return false;
	}
	if (src->desc.width < actual.width || src->desc.height < actual.height) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE with a %ux%u source, smaller than the %ux%u back buffer (aprend_swap_chain_get_back_buffer_size)", src->desc.width, src->desc.height, actual.width, actual.height);
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

	// The source covers the back buffer: the back buffer's size is copied.
	const uint32_t width  = actual.width;
	const uint32_t height = actual.height;
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
	cmd_list->present_generation  = sc->generation;
	return true;
}

static bool aprend_render_view_valid(aprend_texture_view view) {
	if (!view)
		return false;
	return view->image_view && view->dimension == APREND_TEXTURE_DIMENSION_2D && view->texture._t2d;
}

static bool aprend_begin_rendering_valid(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p = command._params._begin_rendering;
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		if (!aprend_render_view_valid(p._color_targets[i].view)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING color target %u is not a view of a 2D texture", i);
			return false;
		}
		/* spudgpu_color_attachment_desc has no resolve attachment yet, so a
		 * resolve can't be honored — fail rather than silently skip it. */
		if (p._color_targets[i].resolve_view) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING color target %u has a resolve_view, which SpudGPU doesn't support yet", i);
			return false;
		}
	}
	if (p._depth_target.view && !aprend_render_view_valid(p._depth_target.view)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING depth target is not a view of a 2D texture");
		return false;
	}
	if (p._color_target_count == 0 && !p._depth_target.view) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING has no color or depth target");
		return false;
	}
	// The targets of one pass are all one layer, or all the same number of
	// layers of 2D array views: the pass is layered or it isn't.
	aprend_texture_view first = p._color_target_count > 0 ? p._color_targets[0].view : p._depth_target.view;
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		if (p._color_targets[i].view->desc.shape != first->desc.shape) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING color target %u is not the shape of the pass's first target", i);
			return false;
		}
		if (p._color_targets[i].view->desc.layer_count != first->desc.layer_count) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING color target %u covers %u layers, and the pass's first target covers %u", i, p._color_targets[i].view->desc.layer_count, first->desc.layer_count);
			return false;
		}
	}
	if (p._depth_target.view && p._depth_target.view->desc.shape != first->desc.shape) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING depth target is not the shape of the pass's first target");
		return false;
	}
	if (p._depth_target.view && p._depth_target.view->desc.layer_count != first->desc.layer_count) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING depth target covers %u layers, and the pass's first target covers %u", p._depth_target.view->desc.layer_count, first->desc.layer_count);
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
    aprend_command_list cmd_list,
    aprend_graphics_pipeline pipeline,
    const APREND_COMMAND &begin) {
	const auto &p                           = begin._params._begin_rendering;
	const APREND_GRAPHICS_PIPELINE_DESC &pd = pipeline->desc;
	if (pd.color_attachment_count != p._color_target_count) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE: the pipeline renders to %u color targets, and the pass has %u", pd.color_attachment_count, p._color_target_count);
		return false;
	}
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		const SPUDGPU_FORMAT format = p._color_targets[i].view->texture._t2d->desc.format;
		if (pd.color_attachment_formats[i] != format) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE: the pipeline renders format %u to color target %u, which is of format %u", (unsigned)pd.color_attachment_formats[i], i, (unsigned)format);
			return false;
		}
	}
	const SPUDGPU_FORMAT depth = p._depth_target.view ? p._depth_target.view->texture._t2d->desc.format : SPUDGPU_FORMAT_UNKNOWN;
	if (pd.depth_format != depth) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE: the pipeline's depth format is %u, and the pass's depth target is of format %u", (unsigned)pd.depth_format, (unsigned)depth);
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
		aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "compile: out of memory");
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
	desc.base.type = SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
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
		const APREND_COLOR_TARGET &target = p._color_targets[i];
		aprend_cmd_transition_view(cmd_list, target.view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		aprend_cmd_use_handle(cmd_list, target.view);

		spudgpu_color_attachment_desc &color = desc.color_attachments[i];
		color.image_view                     = target.view->image_view;
		color.load_op                        = target.load_op;
		color.store_op                       = target.store_op;
		memcpy(color.clear_color, target.clear_color, sizeof(color.clear_color));
	}

	const APREND_DEPTH_TARGET &depth = p._depth_target;
	if (depth.view) {
		aprend_cmd_transition_view(cmd_list, depth.view, SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		aprend_cmd_use_handle(cmd_list, depth.view);

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

/* Adds the storage buffers [set], at [slot], holds for frame [frame] to
 * [stores], each once, with whether [writes] (the pipeline's) writes the
 * binding each is in. */
static void aprend_storage_stores_add_set(
    eq::array<aprend_pass_store> &stores,
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
			aprend_push(stores, aprend_pass_store{frame_stores[i].store, written});
	}
}

/* Adds what [set], at [slot], has its views cover to [images], each storage
 * image with whether [writes] (the pipeline's) writes its binding. False if
 * any mip of any layer is already there in another layout: sampled through
 * one slot and a storage image through another. Two views of one texture
 * that don't share a mip of a layer are no conflict, which is what lets one
 * mip be read while another is written. */
static bool aprend_pass_images_add_set(
    aprend_command_list cmd_list,
    eq::array<aprend_pass_image> &images,
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
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "a mip of a texture is in a sampled slot and a storage image slot of the binding sets of one pass or dispatch");
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
			aprend_push(images, aprend_pass_image{range, use.layout, written});
	}
	return true;
}

/* The argument buffer of the indirect draw [command], for a list compiled in
 * frame [frame]: that frame's copy of its _buffer. aprend_command_list_send
 * refused a command with none. */
static aprend_buffer_part *aprend_cmd_indirect_arguments(
    const APREND_COMMAND &command,
    uint32_t frame) {
	return aprend_buffer_part_of(command._params._draw_indirect._buffer, frame);
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
	spudgpu_buffer_barrier barrier{{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, tracked->state, state};
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
    const eq::array<aprend_pass_image> &images,
    const eq::array<aprend_pass_store> &stores) {
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
	eq::array<spudgpu_buffer_barrier> &buffer_barriers = cmd_list->set_barriers;
	buffer_barriers.clear();
	for (const aprend_pass_store &pass_store : stores) {
		aprend_buffer_store *store = pass_store.store;
		// One that was last an indirect argument buffer comes back to the
		// state a storage slot uses it in.
		if (store->multi_state)
			aprend_cmd_transition_store(cmd_list, store, store->use_state);
		aprend_command_list_t::storage_hazard &hazard = aprend_cmd_store_hazard(cmd_list, store);
		if (hazard.written || (pass_store.written && hazard.accessed)) {
			aprend_push(buffer_barriers, spudgpu_buffer_barrier{{SPUD_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS});
			hazard.written = false;
		}
		hazard.accessed = true;
		hazard.written  = hazard.written || pass_store.written;
	}
	if (buffer_barriers.empty())
		return;
	spudgpu_cmd_pipeline_barrier(cmd_list->cmd_list, buffer_barriers.data, (uint32_t)buffer_barriers.size(), nullptr, 0);
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
	eq::array<aprend_pass_image> &images = cmd_list->pass_images;
	eq::array<aprend_pass_store> &stores = cmd_list->pass_stores;
	/* The argument buffers of the pass's indirect draws, each once. */
	eq::array<aprend_buffer_store *> &argument_stores = cmd_list->pass_arguments;
	images.clear();
	stores.clear();
	argument_stores.clear();
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
					aprend_push(argument_stores, arguments);
			}
			// A draw with no pipeline, or with an empty slot, fails the
			// compile when it is reached.
			if (!pass_pipeline)
				break;
			for (uint32_t slot = 0; slot < pass_pipeline->desc._binding_layout_count; ++slot) {
				if (!pass_slots[slot])
					continue;
				if (!aprend_pass_images_add_set(cmd_list, images, pass_slots[slot], slot, pass_pipeline->storage_writes))
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
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "color target %u of a pass is also in one of the pass's binding sets", i);
				return false;
			}
		}
		if (p._depth_target.view && aprend_texture_ranges_overlap(aprend_texture_view_range(p._depth_target.view), pass_image.range)) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "the depth target of a pass is also in one of the pass's binding sets");
			return false;
		}
	}

	// A buffer is in one state for the whole pass: it can't be read as draw
	// arguments and be in a storage slot the pass's draws read.
	for (aprend_buffer_store *arguments : argument_stores) {
		for (const aprend_pass_store &pass_store : stores) {
			if (pass_store.store == arguments) {
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "the argument buffer of an indirect draw is also in one of the pass's binding sets");
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
	eq::array<aprend_pass_image> &images = cmd_list->pass_images;
	eq::array<aprend_pass_store> &stores = cmd_list->pass_stores;
	images.clear();
	stores.clear();
	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		aprend_binding_set set = slots[slot];
		if (!set) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "dispatch with a pipeline that declares set %u, but no SET_BINDING_SET has put a set there", slot);
			return false;
		}
		if (!aprend_binding_set_fits(set, pipeline->desc._binding_layouts[slot])) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "dispatch with a set at slot %u that isn't of the layout the pipeline declares there", slot);
			return false;
		}
		if (!aprend_pass_images_add_set(cmd_list, images, set, slot, pipeline->storage_writes))
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
	/* The copy of the buffer of the frame the list is compiled in. */
	aprend_buffer_part *buffer;
	bool has_span;
	APREND_TRANSIENT_SPAN span;
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
 * the view is the buffer's for that stride (aprend_buffer_vertex_view) or
 * the span's. False, reported, if not. */
static bool aprend_cmd_apply_vertex_buffers(
    aprend_command_list cmd_list,
    aprend_graphics_pipeline pipeline,
    aprend_cmd_vertex_slot *slots,
    spudgpu_buffer_view *applied,
    const APREND_COMMAND &draw) {
	for (uint32_t slot = 0; slot < pipeline->desc._vertex_binding_count; ++slot) {
		aprend_cmd_vertex_slot &held = slots[slot];
		if (!held.buffer && !held.has_span) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "draw with a pipeline that reads vertex buffer slot %u, and nothing has been put there", slot);
			return false;
		}
		const uint32_t stride  = pipeline->vertex_strides[slot];
		const uint64_t size    = held.buffer ? held.buffer->store.size : held.span.size;
		uint64_t first         = 0;
		uint64_t count         = 0;
		const bool per_instance = pipeline->desc._vertex_bindings[slot]._per_instance;
		if (aprend_cmd_draw_reads(draw, per_instance, &first, &count) && count > 0) {
			// The last element read ends where its furthest attribute does.
			const uint64_t end = (first + count - 1) * stride + pipeline->vertex_extents[slot];
			if (end > size) {
				aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "draw reads %s %llu to %llu at vertex buffer slot %u, %llu bytes at a stride of %u, and the %s holds %llu", per_instance ? "instances" : "vertices", (unsigned long long)first, (unsigned long long)(first + count - 1), slot, (unsigned long long)end, stride, held.buffer ? "buffer" : "span", (unsigned long long)size);
				return false;
			}
		}
		spudgpu_buffer_view view = nullptr;
		SPUDRESULT sr            = SPUD_SUCCESS;
		if (held.buffer) {
			sr = aprend_buffer_vertex_view(held.buffer, stride, &view);
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
			aprend_cmd_report(cmd_list, aprend_result_from_spud(sr), "draw: a view of vertex buffer slot %u at a stride of %u can't be created: %s", slot, stride, spudresult_str(sr));
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
    aprend_command_list cmd_list,
    spudgpu_command_list cmd,
    aprend_graphics_pipeline pipeline,
    const aprend_binding_set *slots,
    aprend_binding_set *applied,
    uint32_t frame) {
	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		aprend_binding_set set = slots[slot];
		if (!set) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "draw with a pipeline that declares set %u, but no SET_BINDING_SET has put a set there", slot);
			return false;
		}
		if (!aprend_binding_set_fits(set, pipeline->desc._binding_layouts[slot])) {
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "draw with a set at slot %u that isn't of the layout the pipeline declares there", slot);
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
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_BINDING_SET with a NULL set");
		return false;
	}
	if (slot >= APREND_MAX_BINDING_LAYOUTS) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_BINDING_SET with _slot %u, at or above APREND_MAX_BINDING_LAYOUTS", slot);
		return false;
	}
	slot_sets[slot] = set;
	// The set, and what its descriptors point at: a destroy of any of them,
	// or an update of the set, refuses the list at submit.
	aprend_cmd_use_handle(cmd_list, set);
	for (uint32_t i = 0; i < set->entry_count; ++i) {
		const APREND_BINDING_SET_ENTRY &entry = set->entries[i];
		switch (entry._type) {
		case SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			aprend_cmd_use_handle(cmd_list, entry._resource._uniform._buffer);
			break;
		case SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			// Its store is taken below, which a destroy looks for.
			break;
		default:
			aprend_cmd_use_handle(cmd_list, entry._resource._image._view);
			aprend_cmd_use_handle(cmd_list, entry._resource._image._sampler);
			break;
		}
	}
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
 * through the list's own layout tracking. False, reported, if the base level
 * of a layer has nothing in it as the list reaches the command, or the
 * texture's chain or pipeline can't be made. */
static bool aprend_cmd_generate_mips(
    aprend_command_list cmd_list,
    aprend_texture2d texture,
    uint32_t base_mip,
    SPUDGPU_FILTER filter) {
	// Checked again: a resize since the command was sent may have changed
	// the texture's levels.
	APREND_ERROR error{};
	if (aprend_generate_mips_valid(texture, base_mip, filter, &error) != APRESULT_OK) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "GENERATE_MIPS: %s", error.message);
		return false;
	}
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
			aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "GENERATE_MIPS: level %u of layer %u has nothing in it, from before the list or earlier in it", base_mip, layer);
			return false;
		}
	}
	if (base_mip == last_mip)
		return true;

	const aprend_mip_chain *chain     = nullptr;
	aprend_graphics_pipeline pipeline = nullptr;
	if (aprend_texture2d_mip_chain(texture, base_mip, filter, &chain, &pipeline, &error) != APRESULT_OK) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "GENERATE_MIPS: %s", error.message);
		return false;
	}
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
			aprend_record_mip_pass(cmd_list->cmd_list, pipeline, chain->steps[(size_t)layer * levels + i], chain->filter, aprend_mip_size(texture->desc.width, mip),
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
 * level is moved to SHADER_READ_ONLY once rendered. False, reported, if the
 * source has nothing in it as the list reaches the command, or a pipeline
 * or set can't be made. */
static bool aprend_cmd_bloom(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p            = command._params._bloom;
	aprend_instance instance = cmd_list->instance;
	spudgpu_command_list cmd = cmd_list->cmd_list;

	// Checked again: a resize since the command was sent may have changed
	// the source's size.
	if (p._levels > aprend_bloom_max_levels(p._source)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM: the source's size no longer allows _levels %u", p._levels);
		return false;
	}
	const aprend_texture_range source = aprend_texture_view_range(p._source);
	if (!aprend_cmd_range_written(cmd_list, source)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM: the source has nothing in it, from before the list or earlier in it");
		return false;
	}
	if (p._mask && !aprend_cmd_range_written(cmd_list, aprend_texture_view_range(p._mask))) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM: the _mask has nothing in it, from before the list or earlier in it");
		return false;
	}
	if (p._lens_dirt && !aprend_cmd_range_written(cmd_list, aprend_texture_view_range(p._lens_dirt))) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM: the _lens_dirt has nothing in it, from before the list or earlier in it");
		return false;
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
	// Without a mask or dirt, the pass's set 1 is the set it reads at set 0
	// again, which the shader then doesn't read: a pipeline's every set is
	// bound at a draw.
	aprend_binding_set mask_set = p._mask ? aprend_texture_view_bloom_set(p._mask) : source_set;
	aprend_binding_set dirt_set = p._lens_dirt ? aprend_texture_view_bloom_set(p._lens_dirt) : chain->level_sources[0]->bloom_set;
	if (!mask_set || !dirt_set)
		return false;

	aprend_bloom_params params{};
	params.threshold  = p._threshold;
	params.knee       = p._knee;
	params.radius     = p._radius;
	params.intensity  = p._intensity;
	params.use_mask   = p._mask ? 1.0f : 0.0f;
	params.lens_dirt_intensity = p._lens_dirt ? p._lens_dirt_intensity : 0.0f;
	auto level_width  = [&](uint32_t level) { return aprend_mip_size(chain_width, level); };
	auto level_height = [&](uint32_t level) { return aprend_mip_size(chain_height, level); };
	auto set_target   = [&](uint32_t width, uint32_t height) {
		params.target_size[0] = (float)width;
		params.target_size[1] = (float)height;
	};

	aprend_cmd_transition_range(cmd_list, source, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	if (p._mask)
		aprend_cmd_transition_range(cmd_list, aprend_texture_view_range(p._mask), SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	// Down: level 0 from the source, then each level from the one above.
	for (uint32_t level = 0; level < levels; ++level) {
		aprend_texture_view target = chain->level_targets[level];
		aprend_cmd_transition_view(cmd_list, target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		set_target(level_width(level), level_height(level));
		if (level == 0) {
			const aprend_binding_set sets[2] = {source_set, mask_set};
			aprend_record_fullscreen_pass_sets(cmd, prefilter, target->image_view, level_width(0), level_height(0), SPUDGPU_LOAD_OP_DONT_CARE, sets, 2,
			                                   cmd_list->compiled_frame, &params, sizeof(params));
		} else
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
	if (p._lens_dirt)
		aprend_cmd_transition_range(cmd_list, aprend_texture_view_range(p._lens_dirt), SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	aprend_cmd_transition_view(cmd_list, p._target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	set_target(target_width, target_height);
	const aprend_binding_set composite_sets[2] = {chain->level_sources[0]->bloom_set, dirt_set};
	aprend_record_fullscreen_pass_sets(cmd, composite, p._target->image_view, target_width, target_height, SPUDGPU_LOAD_OP_LOAD, composite_sets, 2,
	                                   cmd_list->compiled_frame, &params, sizeof(params));
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
		aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "compile: out of memory");
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

/* BLUR, outside a pass: the source into SHADER_READ_ONLY; a pooled texture
 * of the target's size rendered from it across, moved to SHADER_READ_ONLY;
 * the target rendered from that down, left in COLOR_ATTACHMENT_OPTIMAL.
 * False, reported, if the source has nothing in it, it and the target are no
 * longer one size, or a pipeline, set or the pooled texture can't be made. */
static bool aprend_cmd_blur(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                     = command._params._blur;
	aprend_instance instance          = cmd_list->instance;
	const aprend_texture_range source = aprend_texture_view_range(p._source);
	if (!aprend_cmd_range_written(cmd_list, source)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR: the source has nothing in it, from before the list or earlier in it");
		return false;
	}
	// Checked again: a resize since the command was sent may have changed
	// either size.
	if (!aprend_views_one_size(p._source, p._target)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR: the source and the target are no longer one size");
		return false;
	}
	const SPUDGPU_FORMAT between_format = SPUDGPU_FORMAT_R16G16B16A16_FLOAT;
	const SPUDGPU_FORMAT target_format  = p._target->texture._t2d->desc.format;
	const uint32_t width                = aprend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip);
	const uint32_t height               = aprend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip);
	aprend_graphics_pipeline across     = aprend_builtin_blur_pipeline(instance, between_format);
	aprend_graphics_pipeline down       = aprend_builtin_blur_pipeline(instance, target_format);
	if (!across || !down)
		return false;
	aprend_pooled_target *between = aprend_render_target_take(cmd_list, width, height, between_format, 1);
	if (!between)
		return false;
	aprend_binding_set source_set  = aprend_texture_view_bloom_set(p._source);
	aprend_binding_set between_set = aprend_texture_view_bloom_set(between->level_sources[0]);
	if (!source_set || !between_set)
		return false;

	aprend_blur_params params{};
	params.target_size[0] = (float)width;
	params.target_size[1] = (float)height;
	params.sigma          = p._sigma;
	params.radius         = std::ceil(3.0f * p._sigma);
	aprend_cmd_transition_range(cmd_list, source, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	aprend_texture_view between_target = between->level_targets[0];
	aprend_cmd_transition_view(cmd_list, between_target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	params.step[0] = 1.0f / (float)width;
	params.step[1] = 0.0f;
	aprend_record_fullscreen_pass(cmd_list->cmd_list, across, between_target->image_view, width, height, SPUDGPU_LOAD_OP_DONT_CARE, source_set, &params,
	                              sizeof(params));
	aprend_cmd_transition_view(cmd_list, between_target, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	aprend_cmd_transition_view(cmd_list, p._target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	params.step[0] = 0.0f;
	params.step[1] = 1.0f / (float)height;
	aprend_record_fullscreen_pass(cmd_list->cmd_list, down, p._target->image_view, width, height, SPUDGPU_LOAD_OP_DONT_CARE, between_set, &params,
	                              sizeof(params));
	return true;
}

/* OUTLINE, outside a pass: the mask into SHADER_READ_ONLY; its seeds into
 * one pooled texture of places, each jump from one pooled texture into the
 * other, each moved to SHADER_READ_ONLY once rendered; then the outline
 * blended over the target, left in COLOR_ATTACHMENT_OPTIMAL. False,
 * reported, if the mask has nothing in it, it and the target are no longer
 * one size, or a pipeline, set or pooled texture can't be made. */
static bool aprend_cmd_outline(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                   = command._params._outline;
	aprend_instance instance        = cmd_list->instance;
	const aprend_texture_range mask = aprend_texture_view_range(p._mask);
	if (!aprend_cmd_range_written(cmd_list, mask)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE: the mask has nothing in it, from before the list or earlier in it");
		return false;
	}
	// Checked again: a resize since the command was sent may have changed
	// either size.
	if (!aprend_views_one_size(p._mask, p._target)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE: the mask and the target are no longer one size");
		return false;
	}
	uint32_t width = 0, height = 0;
	aprend_view_size(p._target, &width, &height);
	const SPUDGPU_FORMAT places_format = SPUDGPU_FORMAT_R32G32_FLOAT;
	aprend_graphics_pipeline flood     = aprend_builtin_outline_pipeline(instance, places_format, false);
	aprend_graphics_pipeline draw      = aprend_builtin_outline_pipeline(instance, p._target->texture._t2d->desc.format, true);
	if (!flood || !draw)
		return false;
	// Two takes of one key in one compile are two textures.
	aprend_pooled_target *places[2] = {aprend_render_target_take(cmd_list, width, height, places_format, 1), nullptr};
	if (!places[0])
		return false;
	places[1] = aprend_render_target_take(cmd_list, width, height, places_format, 1);
	if (!places[1])
		return false;
	aprend_binding_set mask_set     = aprend_texture_view_bloom_set(p._mask);
	aprend_binding_set place_set[2] = {aprend_texture_view_bloom_set(places[0]->level_sources[0]),
	                                   aprend_texture_view_bloom_set(places[1]->level_sources[0])};
	if (!mask_set || !place_set[0] || !place_set[1])
		return false;

	aprend_outline_params params{};
	for (int i = 0; i < 4; ++i)
		params.color[i] = p._color[i];
	params.target_size[0] = (float)width;
	params.target_size[1] = (float)height;
	params.width          = p._width;
	// One pass into [to] from [from_set], its places left readable.
	auto flood_pass = [&](uint32_t to, aprend_binding_set from_set) {
		aprend_texture_view target = places[to]->level_targets[0];
		aprend_cmd_transition_view(cmd_list, target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		aprend_record_fullscreen_pass(cmd_list->cmd_list, flood, target->image_view, width, height, SPUDGPU_LOAD_OP_DONT_CARE, from_set, &params,
		                              sizeof(params));
		aprend_cmd_transition_view(cmd_list, target, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	};

	aprend_cmd_transition_range(cmd_list, mask, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	params.mode = APREND_OUTLINE_MODE_SEED;
	flood_pass(0, mask_set);
	// A jump of k carries a place 2k - 1 texels at most, through every jump
	// after it: the first reaches the farthest centre the outline covers,
	// _width + 1 off. The extra jump of 1 at the end mends most of what a
	// jump flood gets wrong (Rong and Tan's JFA+1).
	const double reach = std::ceil((double)p._width) + 1.0;
	uint32_t first     = 1;
	while (2.0 * first - 1.0 < reach)
		first *= 2;
	uint32_t current = 0;
	params.mode      = APREND_OUTLINE_MODE_JUMP;
	for (uint32_t jump = first;; jump /= 2) {
		params.jump = (float)jump;
		flood_pass(1 - current, place_set[current]);
		current = 1 - current;
		if (jump == 1)
			break;
	}
	params.jump = 1.0f;
	flood_pass(1 - current, place_set[current]);
	current = 1 - current;

	params.mode = APREND_OUTLINE_MODE_COMPOSITE;
	aprend_cmd_transition_view(cmd_list, p._target, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	aprend_record_fullscreen_pass(cmd_list->cmd_list, draw, p._target->image_view, width, height, SPUDGPU_LOAD_OP_LOAD, place_set[current], &params,
	                              sizeof(params));
	return true;
}

/* TONE_MAP, outside a pass: the source into SHADER_READ_ONLY, the target
 * into COLOR_ATTACHMENT_OPTIMAL and every texel of it replaced. */
static bool aprend_cmd_tone_map(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                     = command._params._tone_map;
	aprend_instance instance          = cmd_list->instance;
	const aprend_texture_range source = aprend_texture_view_range(p._source);
	if (!aprend_cmd_range_written(cmd_list, source)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "TONE_MAP: the source has nothing in it, from before the list or earlier in it");
		return false;
	}
	aprend_graphics_pipeline pipeline = aprend_lit_tone_map_pipeline(instance, p._target->texture._t2d->desc.format);
	if (!pipeline)
		return false;
	// The exposure's last value, or a placeholder that isn't read.
	aprend_texture_view exposure = p._auto_exposure ? p._auto_exposure->value_views[p._auto_exposure->current]
	                                                : instance->builtins.lit.placeholder_2d_view;
	if (p._auto_exposure && !p._auto_exposure->adapted) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "TONE_MAP: its _auto_exposure has never adapted (AUTO_EXPOSURE)");
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
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_ENVIRONMENT: the source has nothing in it, from before the list or earlier in it");
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

/* LIT_SKY, outside a pass: Preetham's or Hosek and Wilkie's sky into the radiance cube's first
 * mip, face by face, then prefiltered. */
static bool aprend_cmd_lit_sky(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                = command._params._lit_sky;
	aprend_lit_environment env   = p._environment;
	const bool hosek             = p._model == APREND_LIT_SKY_MODEL_HOSEK_WILKIE;
	aprend_graphics_pipeline sky = aprend_lit_pass_pipeline(cmd_list->instance, hosek ? APREND_LIT_PASS_HOSEK_SKY : APREND_LIT_PASS_SKY);
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
	// Hosek and Wilkie's coefficients, cooked for this sky into the
	// environment's material, which the passes read at set 0.
	aprend_binding_set sky_set = nullptr;
	if (hosek) {
		aprend_material material = aprend_lit_environment_sky_material(env);
		if (!material)
			return false;
		aprend_hosek_sky_block block{};
		aprend_lit_hosek_cook(params.sun, p._turbidity, p._ground_albedo, p._intensity, &block);
		for (uint32_t i = 0; i < material->slot_count; ++i) {
			if (material->slots[i].bytes) {
				memcpy(material->slots[i].bytes, &block, sizeof(block));
				material->slots[i].dirty = true;
			}
		}
		if (!aprend_cmd_use_material(cmd_list, material))
			return false;
		sky_set = material->set;
	}
	for (uint32_t face = 0; face < 6; ++face) {
		params.face = face;
		aprend_cmd_own_pass(cmd_list, sky, env->radiance_faces[face], size, size, hosek ? &sky_set : nullptr, hosek ? 1 : 0, &params, sizeof(params));
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
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_AMBIENT_OCCLUSION: the depth has nothing in it, from before the list or earlier in it");
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
	// The share of the radius a sample's weight fades out over: XeGTAO's.
	block.params[2]      = 0.615f;
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
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "AUTO_EXPOSURE: the source has nothing in it, from before the list or earlier in it");
		return false;
	}
	const bool histogram               = exposure->histogram_set != nullptr;
	aprend_graphics_pipeline luminance = aprend_lit_pass_pipeline(instance, APREND_LIT_PASS_LUMINANCE);
	aprend_graphics_pipeline adapt     = aprend_lit_pass_pipeline(instance, histogram ? APREND_LIT_PASS_ADAPT_HISTOGRAM : APREND_LIT_PASS_ADAPT);
	aprend_compute_pipeline count      = histogram ? aprend_lit_histogram_pipeline(instance) : nullptr;
	if (!luminance || !adapt || (histogram && !count))
		return false;
	const uint32_t next          = exposure->current ^ 1u;
	aprend_binding_set source_set = aprend_texture_view_environment_set(p._source);
	// Set 0 reads the luminance texture's mips, or the histogram's bins.
	aprend_binding_set adapt_sets[2] = {histogram ? exposure->histogram_set : aprend_texture_view_environment_set(exposure->luminance_view),
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
	if (histogram) {
		// The bins emptied, then counted, then read by the pass: each use
		// ordered after the last as any dispatch's set is (the first after
		// what an earlier submission of the frame left of this copy).
		aprend_binding_set set                = exposure->histogram_set;
		eq::array<aprend_pass_image> &images = cmd_list->pass_images;
		eq::array<aprend_pass_store> &stores = cmd_list->pass_stores;
		images.clear();
		stores.clear();
		try {
			if (!aprend_pass_images_add_set(cmd_list, images, set, 0, count->storage_writes))
				return false;
			aprend_storage_stores_add_set(stores, set, cmd_list->compiled_frame, 0, count->storage_writes);
		} catch (const std::bad_alloc &) {
			aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "compile: out of memory");
			return false;
		}
		spudgpu_command_list cmd = cmd_list->cmd_list;
		aprend_histogram_params hp{};
		hp.min_log = exposure->desc.min_log;
		hp.max_log = exposure->desc.max_log;
		hp.size    = APREND_EXPOSURE_LUMINANCE_SIZE;
		for (uint32_t clear = 1;; clear = 0) {
			aprend_cmd_ready_set_resources(cmd_list, images, stores);
			spudgpu_cmd_bind_compute_pipeline(cmd, count->pipeline);
			spudgpu_cmd_bind_descriptor_sets_compute(cmd, count->pipeline, 0, &set->sets[aprend_binding_set_frame_slot(set, cmd_list->compiled_frame)], 1);
			hp.clear = clear;
			spudgpu_cmd_push_constants_compute(cmd, count->pipeline, 0, sizeof(hp), &hp);
			const uint32_t groups = clear ? 1u : (APREND_EXPOSURE_LUMINANCE_SIZE + 15u) / 16u;
			spudgpu_cmd_dispatch(cmd, groups, groups, 1);
			if (!clear)
				break;
		}
		// Read by the pass now, not written.
		for (aprend_pass_store &store : stores)
			store.written = false;
		aprend_cmd_ready_set_resources(cmd_list, images, stores);
		aprend_histogram_adapt_params params{};
		params.key        = exposure->desc.key;
		params.min_log    = exposure->desc.min_log;
		params.max_log    = exposure->desc.max_log;
		params.up_rate    = exposure->desc.up_rate;
		params.down_rate  = exposure->desc.down_rate;
		params.delta_time = p._delta_time;
		params.low        = exposure->desc.low_share;
		params.high       = exposure->desc.high_share;
		params.first      = exposure->adapted ? 0u : 1u;
		aprend_cmd_own_pass(cmd_list, adapt, exposure->value_targets[next], 1, 1, adapt_sets, 2, &params, sizeof(params));
		exposure->current = next;
		exposure->adapted = true;
		return true;
	}
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

/* Whether [cmd_list] holds a LIT_CULL_LIGHTS of [scene], which then culls
 * its lights where it stands, in place of the cull at the list's top. */
static bool aprend_cmd_list_culls(aprend_command_list cmd_list, aprend_lit_scene scene) {
	for (const APREND_COMMAND &command : cmd_list->commands)
		if (command._type == APREND_COMMAND_LIT_CULL_LIGHTS && command._params._lit_cull_lights._scene == scene)
			return true;
	return false;
}

/* LIT_CULL_LIGHTS, outside a pass: the scene's tiles culled against the
 * depth's bounds, as the cull at the top of a list is recorded, with the
 * depth view's cull depth set at set 1. False, reported, if the depth has
 * nothing in it, the scene's view has no target size or another than the
 * depth's, or the pipeline or a set can't be made. */
static bool aprend_cmd_lit_cull_lights(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	const auto &p                    = command._params._lit_cull_lights;
	aprend_lit_scene scene           = p._scene;
	const aprend_texture_range depth = aprend_texture_view_range(p._depth);
	if (!aprend_cmd_range_written(cmd_list, depth)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS: the depth has nothing in it, from before the list or earlier in it");
		return false;
	}
	if (!(scene->block.flags & APREND_LIT_SCENE_FLAG_TILES) || !scene->tiles) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS: the scene's view has no target size and view_projection to cull for");
		return false;
	}
	const uint32_t width  = aprend_mip_size(p._depth->texture._t2d->desc.width, p._depth->desc.base_mip);
	const uint32_t height = aprend_mip_size(p._depth->texture._t2d->desc.height, p._depth->desc.base_mip);
	if ((float)width != scene->block.target_size[0] || (float)height != scene->block.target_size[1]) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS: the depth is %ux%u, and the scene's view's target %gx%g", width, height, (double)scene->block.target_size[0], (double)scene->block.target_size[1]);
		return false;
	}
	aprend_compute_pipeline pipeline = aprend_lit_cull_bounded_pipeline(cmd_list->instance);
	if (!pipeline || !aprend_cmd_use_material(cmd_list, scene->cull_material))
		return false;
	aprend_binding_set sets[2] = {scene->cull_material->set, aprend_texture_view_cull_depth_set(p._depth)};
	if (!sets[1])
		return false;
	// Each set's resources ordered as any dispatch's are: the depth moved to
	// be sampled, the tiles after what read or wrote them before.
	eq::array<aprend_pass_image> &images = cmd_list->pass_images;
	eq::array<aprend_pass_store> &stores = cmd_list->pass_stores;
	images.clear();
	stores.clear();
	try {
		for (uint32_t slot = 0; slot < 2; ++slot) {
			if (!aprend_pass_images_add_set(cmd_list, images, sets[slot], slot, pipeline->storage_writes))
				return false;
			aprend_storage_stores_add_set(stores, sets[slot], cmd_list->compiled_frame, slot, pipeline->storage_writes);
		}
	} catch (const std::bad_alloc &) {
		aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "compile: out of memory");
		return false;
	}
	aprend_cmd_ready_set_resources(cmd_list, images, stores);
	spudgpu_command_list cmd = cmd_list->cmd_list;
	spudgpu_cmd_bind_compute_pipeline(cmd, pipeline->pipeline);
	for (uint32_t slot = 0; slot < 2; ++slot)
		spudgpu_cmd_bind_descriptor_sets_compute(cmd, pipeline->pipeline, slot,
		                                         &sets[slot]->sets[aprend_binding_set_frame_slot(sets[slot], cmd_list->compiled_frame)], 1);
	const uint32_t count = scene->block.tiles_x * scene->block.tiles_y;
	spudgpu_cmd_dispatch(cmd, (count + 63) / 64, 1, 1);
	return true;
}

/* For each lit scene [cmd_list] binds, at the top of the list:
 * - the instance's BRDF lookup texture made, if nothing has written it;
 * - its environment's cubes cleared to black, if it names one that has
 *   never been written (not filled before the list, and not yet in it), so
 *   an environment reads as black until it is filled;
 * - its lights culled for its view's tiles, if it has a target size and the
 *   list holds no LIT_CULL_LIGHTS of it, which culls where it stands.
 * Recorded before anything else. False if a pipeline or set can't be made. */
static bool aprend_cmd_prepare_lit_scenes(aprend_command_list cmd_list) {
	aprend_instance instance = cmd_list->instance;
	aprend_lit_builtins &lit = instance->builtins.lit;
	// A copy: culling adds materials to the list.
	eq::array<aprend_material> &materials = cmd_list->lit_materials;
	materials.clear();
	if (materials.append(cmd_list->materials.data(), cmd_list->materials.size()) != EQ_SUCCESS) {
		aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "compile: out of memory");
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
		if ((scene->block.flags & APREND_LIT_SCENE_FLAG_TILES) && scene->tiles && !aprend_cmd_list_culls(cmd_list, scene)) {
			aprend_compute_pipeline pipeline = aprend_lit_cull_pipeline(instance);
			if (!pipeline || !aprend_cmd_use_material(cmd_list, scene->cull_material))
				return false;
			// What the cull's set holds is ordered as any dispatch's is, so
			// the tiles it writes are known written to what comes after.
			// The copy this frame writes may still be read by a submission
			// of this frame made earlier: its first use in the list gets a
			// barrier for that.
			aprend_binding_set set = scene->cull_material->set;
			eq::array<aprend_pass_image> &cull_images = cmd_list->pass_images;
			eq::array<aprend_pass_store> &cull_stores = cmd_list->pass_stores;
			cull_images.clear();
			cull_stores.clear();
			if (!aprend_pass_images_add_set(cmd_list, cull_images, set, 0, pipeline->storage_writes))
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

/* Adds the views a built-in effect's [command] names to what [cmd_list]'s
 * compile names: they are read only through the recording, as a pass's
 * targets are. What the effect's own objects hold (an environment's cubes,
 * an exposure's textures) are the objects', and go with them. Throws
 * std::bad_alloc. */
static void aprend_cmd_use_command_views(aprend_command_list cmd_list, const APREND_COMMAND &command) {
	switch (command._type) {
	case APREND_COMMAND_BLOOM:
		aprend_cmd_use_handle(cmd_list, command._params._bloom._source);
		aprend_cmd_use_handle(cmd_list, command._params._bloom._target);
		if (command._params._bloom._mask)
			aprend_cmd_use_handle(cmd_list, command._params._bloom._mask);
		if (command._params._bloom._lens_dirt)
			aprend_cmd_use_handle(cmd_list, command._params._bloom._lens_dirt);
		break;
	case APREND_COMMAND_BLUR:
		aprend_cmd_use_handle(cmd_list, command._params._blur._source);
		aprend_cmd_use_handle(cmd_list, command._params._blur._target);
		break;
	case APREND_COMMAND_OUTLINE:
		aprend_cmd_use_handle(cmd_list, command._params._outline._mask);
		aprend_cmd_use_handle(cmd_list, command._params._outline._target);
		break;
	case APREND_COMMAND_LIT_CULL_LIGHTS:
		aprend_cmd_use_handle(cmd_list, command._params._lit_cull_lights._depth);
		break;
	case APREND_COMMAND_TONE_MAP:
		aprend_cmd_use_handle(cmd_list, command._params._tone_map._source);
		aprend_cmd_use_handle(cmd_list, command._params._tone_map._target);
		break;
	case APREND_COMMAND_LIT_ENVIRONMENT:
		aprend_cmd_use_handle(cmd_list, command._params._lit_environment._source);
		break;
	case APREND_COMMAND_LIT_AMBIENT_OCCLUSION:
		aprend_cmd_use_handle(cmd_list, command._params._lit_ambient_occlusion._depth);
		aprend_cmd_use_handle(cmd_list, command._params._lit_ambient_occlusion._target);
		break;
	case APREND_COMMAND_AUTO_EXPOSURE:
		aprend_cmd_use_handle(cmd_list, command._params._auto_exposure._source);
		break;
	case APREND_COMMAND_DRAW_TEXTURE:
		aprend_cmd_use_handle(cmd_list, command._params._draw_texture._source);
		aprend_cmd_use_handle(cmd_list, command._params._draw_texture._target);
		break;
	case APREND_COMMAND_COPY_SCENE_COLOR:
		aprend_cmd_use_handle(cmd_list, command._params._copy_scene_color._source);
		aprend_cmd_use_handle(cmd_list, command._params._copy_scene_color._target);
		break;
	default:
		break;
	}
}

APRESULT aprend_command_list_compile(aprend_command_list cmd_list, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!cmd_list)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "cmd_list is NULL");
	if (APRI_STALE(cmd_list, "aprend_command_list_compile", "cmd_list"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "cmd_list is destroyed");
	APREND_ENTER_LIST(cmd_list, "aprend_command_list_compile");
	APREND_ENTER(cmd_list->instance, "aprend_command_list_compile");
	aprend_cmd_report_scope scope(cmd_list, out_error);
	cmd_list->compiled = false;
	aprend_cmd_list_unlist(cmd_list);
	// A recording of this frame that was never submitted is recorded over;
	// otherwise a list of this frame that nothing is running.
	aprend_instance instance = cmd_list->instance;
	if (!cmd_list->cmd_list || cmd_list->compiled_frame_number != instance->frame_number) {
		cmd_list->cmd_list = nullptr;
		SPUDRESULT sr      = aprend_instance_take_command_list(instance, &cmd_list->cmd_list);
		if (SPUDFAIL(sr)) {
			aprend_cmd_report(cmd_list, aprend_result_from_spud(sr), "compile: no SpudGPU command list to record into: %s", spudresult_str(sr));
			cmd_list->cmd_list = nullptr;
			return scope.failed(APRESULT_APREND_GPU_FAILURE, "the compile failed");
		}
	}
	cmd_list->layout_uses.clear();
	cmd_list->layout_index.clear();
	cmd_list->storage_hazards.clear();
	cmd_list->staged_stores.clear();
	cmd_list->used_stores.clear();
	cmd_list->used_handles.clear();
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
	if (cmd_list->recording_error) {
		// What the recording found, at the send that found it.
		cmd_list->report_send = cmd_list->recording_send;
		aprend_cmd_report(cmd_list, cmd_list->recording_result != APRESULT_OK ? cmd_list->recording_result : APRESULT_INVALID_ARGUMENT, "%s",
		                  cmd_list->recording_message[0] ? cmd_list->recording_message : "a command was refused when it was sent");
		return cmd_list->report_result;
	}
	aprend_cmd_point_at_copies(cmd_list);

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
				return scope.failed(APRESULT_APREND_GPU_FAILURE, "the compile failed");
			}
			cmd_list->materials.push_back(material);
		}
	} catch (const std::bad_alloc &) {
		aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "compile: out of memory");
		cmd_list->materials.clear();
		return scope.failed(APRESULT_APREND_GPU_FAILURE, "the compile failed");
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
	eq::array<uint8_t> &open_labels = cmd_list->open_labels;
	open_labels.clear();
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
			cmd_list->report_send         = cmd_list->command_sends[command_index];
			aprend_cmd_use_command_views(cmd_list, command);
			switch (command._type) {
			case APREND_COMMAND_DRAW:
			case APREND_COMMAND_DRAW_INDEXED:
			case APREND_COMMAND_DRAW_INSTANCED:
			case APREND_COMMAND_DRAW_INSTANCED_INDEXED:
			case APREND_COMMAND_DRAW_INDIRECT:
			case APREND_COMMAND_DRAW_INDEXED_INDIRECT:
			case APREND_COMMAND_DISPATCH_MESH:
				if (!in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "draw command recorded outside a rendering pass");
					ok = false;
				} else if (!bound_pipeline) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "draw command in a pass that hasn't set a pipeline");
					ok = false;
				} else if (bound_pipeline->is_mesh && command._type != APREND_COMMAND_DISPATCH_MESH) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "draw command with a mesh shader pipeline, which is run with DISPATCH_MESH");
					ok = false;
				} else if (!bound_pipeline->is_mesh && command._type == APREND_COMMAND_DISPATCH_MESH) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DISPATCH_MESH with a pipeline that has no mesh shader");
					ok = false;
				} else if (!index_buffer_bound && (command._type == APREND_COMMAND_DRAW_INDEXED ||
				                                   command._type == APREND_COMMAND_DRAW_INSTANCED_INDEXED ||
				                                   command._type == APREND_COMMAND_DRAW_INDEXED_INDIRECT)) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "indexed draw with no SET_INDEX_BUFFER or SET_TRANSIENT_INDEX_BUFFER before it");
					ok = false;
				} else if (!aprend_cmd_apply_vertex_buffers(cmd_list, bound_pipeline, vertex_slots, applied_vertex_views, command)) {
					ok = false;
				} else {
					ok = aprend_cmd_apply_binding_sets(cmd_list, cmd, bound_pipeline, slot_sets, applied_sets, cmd_list->compiled_frame);
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
					// aprend_command_list_send refused a command that runs past the last slot.
					aprend_buffer_part *part               = aprend_buffer_part_of(p._vertex_buffers[i], cmd_list->compiled_frame);
					vertex_slots[p._start_slot + i]        = {};
					vertex_slots[p._start_slot + i].buffer = part;
					aprend_cmd_use_store(cmd_list, &part->store);
				}
				break;
			}
			case APREND_COMMAND_SET_INDEX_BUFFER: {
				const auto &p = command._params._set_index_buffer;
				if (!p._index_buffer) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_INDEX_BUFFER with a NULL buffer");
					ok = false;
					break;
				}
				aprend_buffer_part *part = aprend_buffer_part_of(p._index_buffer, cmd_list->compiled_frame);
				spudgpu_cmd_set_index_buffer(cmd, part->view);
				index_buffer_bound = true;
				aprend_cmd_use_store(cmd_list, &part->store);
				break;
			}
			case APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER: {
				const auto &p = command._params._set_transient_vertex_buffer;
				if (!aprend_transient_span_valid(cmd_list->instance, &p._span, "SET_TRANSIENT_VERTEX_BUFFER")) {
					ok = false;
					break;
				}
				// Bound at each draw, as a buffer is. aprend_command_list_send
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
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_TRANSIENT_INDEX_BUFFER with a span not aligned to its _index_size of %u; allocate it with that alignment", p._index_size);
					ok = false;
					break;
				}
				spudgpu_buffer_view view = nullptr;
				SPUDRESULT sr            = aprend_transient_span_view(cmd_list->instance, &p._span, p._index_size, &view);
				if (SPUDFAIL(sr)) {
					aprend_cmd_report(cmd_list, aprend_result_from_spud(sr), "SET_TRANSIENT_INDEX_BUFFER: a view of the span can't be created: %s", spudresult_str(sr));
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
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE with a NULL pipeline");
					ok = false;
					break;
				}
				if (!in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE outside a rendering pass");
					ok = false;
					break;
				}
				if (!aprend_pipeline_matches_pass(cmd_list, p._pipeline, *open_pass)) {
					ok = false;
					break;
				}
				spudgpu_cmd_bind_pipeline(cmd, p._pipeline->pipeline);
				bound_pipeline = p._pipeline;
				aprend_cmd_use_handle(cmd_list, p._pipeline);
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
						aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS outside a pass with no compute pipeline set");
						ok = false;
						break;
					}
					if (p._offset > bound_compute_pipeline->push_constant_end) {
						aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS at offset %u, past the compute pipeline's push constant ranges (%u bytes)", p._offset, bound_compute_pipeline->push_constant_end);
						ok = false;
						break;
					}
					if (p._size > bound_compute_pipeline->push_constant_end - p._offset) {
						aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS of %u bytes at offset %u runs past the compute pipeline's push constant ranges (%u bytes)", p._size, p._offset, bound_compute_pipeline->push_constant_end);
						ok = false;
						break;
					}
					spudgpu_cmd_push_constants_compute(cmd, bound_compute_pipeline->pipeline, p._offset, p._size, p._data);
					break;
				}
				if (!bound_pipeline) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS in a pass that hasn't set a pipeline");
					ok = false;
					break;
				}
				if (p._offset > bound_pipeline->push_constant_end) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS at offset %u, past the pipeline's push constant ranges (%u bytes)", p._offset, bound_pipeline->push_constant_end);
					ok = false;
					break;
				}
				if (p._size > bound_pipeline->push_constant_end - p._offset) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS of %u bytes at offset %u runs past the pipeline's push constant ranges (%u bytes)", p._size, p._offset, bound_pipeline->push_constant_end);
					ok = false;
					break;
				}
				spudgpu_cmd_push_constants(cmd, bound_pipeline->pipeline, p._offset, p._size, p._data);
				break;
			}
			case APREND_COMMAND_SET_COMPUTE_PIPELINE: {
				const auto &p = command._params._set_compute_pipeline;
				if (!p._pipeline) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_COMPUTE_PIPELINE with a NULL pipeline");
					ok = false;
					break;
				}
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "SET_COMPUTE_PIPELINE inside a rendering pass");
					ok = false;
					break;
				}
				spudgpu_cmd_bind_compute_pipeline(cmd, p._pipeline->pipeline);
				bound_compute_pipeline = p._pipeline;
				aprend_cmd_use_handle(cmd_list, p._pipeline);
				// Not guaranteed to survive a pipeline change on every backend:
				// the next dispatch binds them again.
				for (aprend_binding_set &applied : applied_compute_sets)
					applied = nullptr;
				break;
			}
			case APREND_COMMAND_DISPATCH: {
				const auto &p = command._params._dispatch;
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DISPATCH inside a rendering pass");
					ok = false;
					break;
				}
				if (!bound_compute_pipeline) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DISPATCH with no compute pipeline set since the last pass or present");
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
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE inside a rendering pass");
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
				aprend_buffer_part *arguments   = aprend_cmd_indirect_arguments(command, cmd_list->compiled_frame);
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
				aprend_buffer_part *arguments   = aprend_cmd_indirect_arguments(command, cmd_list->compiled_frame);
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
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING while another rendering pass is open");
					ok = false;
					break;
				}
				if (!aprend_begin_rendering_valid(cmd_list, command)) {
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
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "END_RENDERING without an open BEGIN_RENDERING pass");
					ok = false;
					break;
				}
				for (bool opened_in_pass : open_labels) {
					if (opened_in_pass) {
						aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "END_RENDERING with a label opened inside the pass still open");
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
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLOOM inside a rendering pass");
					ok = false;
					break;
				}
				ok = aprend_cmd_bloom(cmd_list, command);
				// Its passes bind pipelines of their own: a compute pipeline is
				// set again before the next dispatch, as after a pass.
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_BLUR: {
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BLUR inside a rendering pass");
					ok = false;
					break;
				}
				ok = aprend_cmd_blur(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_OUTLINE: {
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "OUTLINE inside a rendering pass");
					ok = false;
					break;
				}
				ok = aprend_cmd_outline(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_LIT_CULL_LIGHTS: {
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS inside a rendering pass");
					ok = false;
					break;
				}
				ok = aprend_cmd_lit_cull_lights(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_TONE_MAP: {
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "TONE_MAP inside a rendering pass");
					ok = false;
					break;
				}
				ok = aprend_cmd_tone_map(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_LIT_ENVIRONMENT: {
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "LIT_ENVIRONMENT inside a rendering pass");
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
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "%s inside a rendering pass", command._type == APREND_COMMAND_LIT_SKY             ? "LIT_SKY"
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
			case APREND_COMMAND_DRAW_TEXTURE: {
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_TEXTURE inside a rendering pass");
					ok = false;
					break;
				}
				ok = aprend_cmd_draw_texture(cmd_list, command);
				// Its pass binds a pipeline of its own: a compute pipeline is
				// set again before the next dispatch, as after a pass.
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_COPY_SCENE_COLOR: {
				const auto &p = command._params._copy_scene_color;
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "COPY_SCENE_COLOR inside a rendering pass");
					ok = false;
					break;
				}
				// DRAW_TEXTURE's stretch into the level, then GENERATE_MIPS
				// below it.
				APREND_COMMAND draw{};
				draw._type                        = APREND_COMMAND_DRAW_TEXTURE;
				draw._params._draw_texture._source = p._source;
				draw._params._draw_texture._target = p._target;
				draw._params._draw_texture._scaling = APREND_TEXTURE_SCALING_STRETCH;
				draw._params._draw_texture._filter  = SPUDGPU_FILTER_LINEAR;
				ok = aprend_cmd_draw_texture(cmd_list, draw) &&
				     aprend_cmd_generate_mips(cmd_list, p._target->texture._t2d, p._target->desc.base_mip, SPUDGPU_FILTER_LINEAR);
				bound_compute_pipeline = nullptr;
				break;
			}
			case APREND_COMMAND_GENERATE_MIPS: {
				const auto &p = command._params._generate_mips;
				if (in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "GENERATE_MIPS inside a rendering pass");
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
					aprend_push(open_labels, (uint8_t)in_pass);
				} catch (const std::bad_alloc &) {
					aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "compile: out of memory");
					ok = false;
					break;
				}
				spudgpu_cmd_begin_debug_label(cmd, command._params._begin_label._label);
				break;
			case APREND_COMMAND_END_LABEL:
				if (open_labels.empty()) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "END_LABEL with no label open");
					ok = false;
					break;
				}
				if ((open_labels[open_labels.size() - 1] != 0) != in_pass) {
					aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "END_LABEL %s a pass, and its BEGIN_LABEL was %s one", in_pass ? "inside" : "outside", open_labels[open_labels.size() - 1] ? "inside" : "outside");
					ok = false;
					break;
				}
				(void)open_labels.remove(open_labels.size() - 1);
				spudgpu_cmd_end_debug_label(cmd);
				break;
			default:
				break;
			}
			if (!ok)
				break;
		}
	} catch (const std::bad_alloc &) {
		aprend_cmd_report(cmd_list, APRESULT_OUT_OF_MEMORY, "compile: out of memory");
		ok = false;
	}

	cmd_list->report_send = UINT32_MAX;
	if (ok && in_pass) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDERING pass left open at the end of the command list");
		ok = false;
	}
	if (ok && !open_labels.empty()) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "%zu labels left open at the end of the command list", open_labels.size());
		ok = false;
	}

	/* Always leave the spudgpu list closed and balanced, even on failure —
	 * aprend_command_list_submit refuses it when this returns false. The
	 * labels opened inside the pass close before it does, and the rest
	 * after. */
	while (in_pass && !open_labels.empty() && open_labels[open_labels.size() - 1]) {
		spudgpu_cmd_end_debug_label(cmd);
		(void)open_labels.remove(open_labels.size() - 1);
	}
	if (in_pass)
		spudgpu_cmd_end_rendering(cmd);
	while (!open_labels.empty()) {
		spudgpu_cmd_end_debug_label(cmd);
		(void)open_labels.remove(open_labels.size() - 1);
	}

	spudgpu_end_command_list(cmd);
	if (ok)
		ok = aprend_cmd_list_enlist(cmd_list);
	if (!ok) {
		aprend_render_target_give_back(cmd_list);
		cmd_list->timer_uses.clear();
		cmd_list->layout_uses.clear();
		cmd_list->layout_index.clear();
		cmd_list->storage_hazards.clear();
		cmd_list->staged_stores.clear();
		cmd_list->used_stores.clear();
		cmd_list->used_handles.clear();
		cmd_list->buffer_states.clear();
		cmd_list->materials.clear();
		cmd_list->present_swap_chain = nullptr;
	}
	cmd_list->compiled = ok;
	return ok ? APRESULT_OK : scope.failed(APRESULT_APREND_GPU_FAILURE, "the compile failed");
}

APRESULT aprend_command_list_submit(aprend_command_list cmd_list, spudgpu_command_queue queue, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!cmd_list)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "cmd_list is NULL");
	if (APRI_STALE(cmd_list, "aprend_command_list_submit", "cmd_list"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "cmd_list is destroyed");
	APREND_ENTER_LIST(cmd_list, "aprend_command_list_submit");
	if (!queue)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "queue is NULL");
	APREND_ENTER(cmd_list->instance, "aprend_command_list_submit");
	aprend_cmd_report_scope scope(cmd_list, out_error);
	if (!cmd_list->compiled)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the list has not compiled successfully since its last submit");
	// Before anything the list uses is read: something it was compiled
	// against may be gone (aprend_instance_forget).
	{
		std::lock_guard<std::mutex> lock(cmd_list->instance->release_mutex);
		if (cmd_list->stale) {
			return aprend_fail(out_error, APRESULT_APREND_STALE, "something the list uses was destroyed, resized or updated since it was compiled; compile it again");
		}
	}
	// Every texture must still be in the layout this list's barriers were
	// compiled against; if another submission moved it since, the barriers
	// would lie to the GPU.
	for (const auto &use : cmd_list->layout_uses) {
		if (*use.tracked != use.initial_layout) {
			return aprend_fail(out_error, APRESULT_APREND_STALE, "a texture's layout changed since the list was compiled; compile it again");
		}
	}

	// The recording is in the allocator of the frame it was compiled in,
	// which the next turn of that frame index resets. A list that binds a
	// per-frame set or a transient span reads that frame's copies and
	// memory as well.
	if (cmd_list->compiled_frame_number != cmd_list->instance->frame_number) {
		return aprend_fail(out_error, APRESULT_APREND_STALE, "the list was compiled in an earlier frame (aprend_instance_next_frame has been called since); compile it again");
	}

	aprend_swap_chain sc = cmd_list->present_swap_chain;
	if (sc) {
		// The list copies into one specific acquired back buffer; it's only
		// valid while that same one is still acquired and unsubmitted.
		if (sc->generation != cmd_list->present_generation) {
			return aprend_fail(out_error, APRESULT_APREND_STALE, "the swap chain the list presents into was resized since it was compiled; compile it again");
		}
		if (sc->acquired_image != cmd_list->present_image_index || sc->submitted) {
			return aprend_fail(out_error, APRESULT_APREND_STALE, "the back buffer the list presents into is no longer the acquired one; compile it again");
		}
		if (queue != sc->desc.queue) {
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a list presenting into a swap chain goes on that swap chain's queue");
		}
	}

	// Whatever was written to the list's staged buffers since they were last
	// copied goes across first, in the same submission, so the list's own
	// commands read what the caller last wrote; and so do the uniform blocks
	// of its materials that changed since they were last copied, through
	// this frame's transient memory.
	spudgpu_command_list upload = nullptr;
	eq::array<aprend_upload_copy> &copies = cmd_list->upload_copies;
	copies.clear();
	try {
		for (aprend_material material : cmd_list->materials)
			if (!aprend_material_stage_uploads(material, copies))
				return scope.failed(APRESULT_APREND_GPU_FAILURE, "a material's uniform blocks can't be staged");
		SPUDRESULT sr = aprend_cmd_record_uploads(cmd_list, copies, &upload);
		if (SPUDFAIL(sr)) {
			return aprend_fail_spud(out_error, sr, "taking a SpudGPU command list for the list's uploads failed");
		}
	} catch (const std::bad_alloc &) {
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the list's uploads");
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
	if (const APRESULT submitted = aprend_instance_submit(cmd_list->instance, queue, lists, list_count, sc ? sc->swap_chain : nullptr, nullptr, out_error))
		return submitted;
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
	return APRESULT_OK;
}
}
