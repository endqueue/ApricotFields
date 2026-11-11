
#include "rbtrend_internal.hpp"
#include "rbtrendbatch_internal.hpp"
#include "rbtrendimages_internal.hpp"
#include "rbtrendlit_internal.hpp"
#include "rbtrendraytracing_internal.hpp"

#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

rbtrend_command_list_t::~rbtrend_command_list_t() {
	// The Carrottus GPU lists it recorded into are the instance's.
	commands.clear();
}

// [format] and [args] into [out], after "command N: " where [send] isn't
// UINT32_MAX.
static void rbtrend_cmd_format(char *out, size_t size, uint32_t send, const char *format, va_list args) {
	int written = 0;
	if (send != UINT32_MAX)
		written = snprintf(out, size, "command %u: ", send);
	if (written >= 0 && (size_t)written < size)
		vsnprintf(out + written, size - (size_t)written, format, args);
}

bool rbtrend_cmd_report(rbtrend_command_list cmd_list, RBT_RESULT result, const char *format, ...) {
	va_list args;
	va_start(args, format);
	if (cmd_list->reporting) {
		if (cmd_list->report_result == RBT_RESULT_OK) {
			cmd_list->report_result = result;
			if (cmd_list->report)
				rbtrend_cmd_format(cmd_list->report->message, sizeof(cmd_list->report->message), cmd_list->report_send, format, args);
		}
	} else if (cmd_list->recording_result == RBT_RESULT_OK) {
		// A recording's: the send being made.
		cmd_list->recording_result = result;
		cmd_list->recording_send   = cmd_list->send_count ? cmd_list->send_count - 1 : 0;
		vsnprintf(cmd_list->recording_message, sizeof(cmd_list->recording_message), format, args);
	}
	va_end(args);
	return false;
}

bool rbtrend_report(rbtrend_instance instance, RBT_RESULT result, const char *format, ...) {
	rbtrend_command_list cmd_list = instance->reporting_list;
	if (!cmd_list || !cmd_list->reporting || cmd_list->report_result != RBT_RESULT_OK)
		return false;
	cmd_list->report_result = result;
	if (cmd_list->report) {
		va_list args;
		va_start(args, format);
		rbtrend_cmd_format(cmd_list->report->message, sizeof(cmd_list->report->message), cmd_list->report_send, format, args);
		va_end(args);
	}
	return false;
}

/* For the length of a compile or submit of [cmd_list]: what fails in it is
 * reported to [error] (rbtrend_cmd_report, rbtrend_report). */
struct rbtrend_cmd_report_scope {
	rbtrend_command_list cmd_list;
	rbtrend_cmd_report_scope(rbtrend_command_list list, RBTREND_ERROR *error) : cmd_list(list) {
		cmd_list->reporting                 = true;
		cmd_list->report                    = error;
		cmd_list->report_result             = RBT_RESULT_OK;
		cmd_list->report_send               = UINT32_MAX;
		cmd_list->instance->reporting_list = cmd_list;
	}
	~rbtrend_cmd_report_scope() {
		cmd_list->reporting                 = false;
		cmd_list->report                    = nullptr;
		cmd_list->instance->reporting_list = nullptr;
	}
	// The result of a call that failed: the first failure reported, or
	// [fallback] where nothing was.
	RBT_RESULT failed(RBT_RESULT fallback, const char *message) {
		if (cmd_list->report_result != RBT_RESULT_OK)
			return cmd_list->report_result;
		rbtrend_cmd_report(cmd_list, fallback, "%s", message);
		return fallback;
	}
};

/* Adds [store] to the stores [cmd_list] uses, and to its staged stores if
 * it is staged, once each. A store that is written directly has nothing to
 * copy. Throws std::bad_alloc. */
static void rbtrend_cmd_use_store(
    rbtrend_command_list cmd_list,
    rbtrend_buffer_store *store) {
	for (rbtrend_buffer_store *known : cmd_list->used_stores)
		if (known == store)
			return;
	cmd_list->used_stores.push_back(store);
	if (store->staged)
		cmd_list->staged_stores.push_back(store);
}

/* Adds [handle] to what [cmd_list]'s compile names (used_handles), for
 * rbtrend_instance_forget. NULL is ignored. Throws std::bad_alloc. */
static void rbtrend_cmd_use_handle(rbtrend_command_list cmd_list, const void *handle) {
	if (!handle)
		return;
	const uint64_t key = (uint64_t)(uintptr_t)handle;
	// The hash is the address: a match is the same handle.
	if (cmd_list->used_handles.find(key, [](uint32_t) { return true; }))
		return;
	cmd_list->used_handles.add(key, 0);
}

/* The fit of a draw-mode PRESENT_TEXTURE or a DRAW_TEXTURE, checked where
 * the command is sent: [scaling] is an RBTREND_TEXTURE_SCALING, [filter] is
 * LINEAR or NEAREST, and [border] is finite where it is read
 * ([border_read]). False, reported with [who] (the command's name), if not. */
static bool rbtrend_texture_scaling_valid(
    rbtrend_command_list cmd_list,
    const char *who,
    RBTREND_TEXTURE_SCALING scaling,
    CRGPU_FILTER filter,
    const float border[4],
    bool border_read) {
	if (scaling > RBTREND_TEXTURE_SCALING_ASPECT_FIT) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _scaling of %u, which is not an RBTREND_TEXTURE_SCALING", who, (unsigned)scaling);
		return false;
	}
	if (filter != CRGPU_FILTER_LINEAR && filter != CRGPU_FILTER_NEAREST) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _filter of %u, which is not CRGPU_FILTER_LINEAR or _NEAREST", who, (unsigned)filter);
		return false;
	}
	if (!border_read)
		return true;
	for (int i = 0; i < 4; ++i) {
		if (!std::isfinite(border[i])) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _border_color that is not finite", who);
			return false;
		}
	}
	return true;
}

/* The width and height of the first mip a 2D view of a 2D texture is of. */
static void rbtrend_view_size(rbtrend_texture_view view, uint32_t *out_width, uint32_t *out_height) {
	*out_width  = rbtrend_mip_size(view->texture._t2d->desc.width, view->desc.base_mip);
	*out_height = rbtrend_mip_size(view->texture._t2d->desc.height, view->desc.base_mip);
}

/* Whether DRAW_TEXTURE's [rect] (its [what]) is zeroed or inside [view]'s
 * mip, its width and height both 0 or both above 0. Prints what is wrong. */
static bool rbtrend_texel_rect_valid(rbtrend_command_list cmd_list, const char *what, const RBTREND_TEXEL_RECT &rect, rbtrend_texture_view view) {
	if ((rect.width == 0) != (rect.height == 0)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a %s of %ux%u: its width and height are both 0 or both above 0", what, (unsigned)rect.width, (unsigned)rect.height);
		return false;
	}
	if (rect.width == 0) {
		if (rect.x != 0 || rect.y != 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a %s of no size at (%u, %u): a whole one is zeroed", what, (unsigned)rect.x, (unsigned)rect.y);
			return false;
		}
		return true;
	}
	uint32_t width = 0, height = 0;
	rbtrend_view_size(view, &width, &height);
	if (rect.x >= width || rect.width > width - rect.x) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a %s reaching past its view's width of %u", what, (unsigned)width);
		return false;
	}
	if (rect.y >= height || rect.height > height - rect.y) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a %s reaching past its view's height of %u", what, (unsigned)height);
		return false;
	}
	return true;
}

/* Whether 2D views [a] and [b] are of mips of one size. */
static bool rbtrend_views_one_size(rbtrend_texture_view a, rbtrend_texture_view b) {
	uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
	rbtrend_view_size(a, &aw, &ah);
	rbtrend_view_size(b, &bw, &bh);
	return aw == bw && ah == bh;
}

/* Whether [view], a BLOOM's [what] (its _mask or _lens_dirt), is one it can
 * read: of [cmd_list]'s instance, a SHADER_RESOURCE 2D view of color, in a
 * format a shader reads as floats. Prints what is wrong. */
static bool rbtrend_bloom_read_view_valid(rbtrend_command_list cmd_list, rbtrend_texture_view view, const char *what) {
	if (view->instance != cmd_list->instance) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a %s of another instance", what);
		return false;
	}
	if (view->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a %s that is not a SHADER_RESOURCE view", what);
		return false;
	}
	if (view->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a %s that is not a 2D view", what);
		return false;
	}
	if (view->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_COLOR) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a %s that is not a view of color", what);
		return false;
	}
	// A 2D view is of a 2D texture.
	if (rbtrend_format_scalar_type(view->texture._t2d->desc.format) != RBTREND_SHADER_SCALAR_TYPE_FLOAT) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a %s of format %u, which a shader doesn't read as floats", what, (unsigned)view->texture._t2d->desc.format);
		return false;
	}
	return true;
}

/* The most levels a bloom of [source] spreads through: its level 0 is half
 * the size of the source's first mip, and each level halves again down to
 * 1 texel. */
static uint32_t rbtrend_bloom_max_levels(rbtrend_texture_view source) {
	const rbtrend_texture2d texture = source->texture._t2d;
	const uint32_t width           = rbtrend_mip_size(rbtrend_mip_size(texture->desc.width, source->desc.base_mip), 1);
	const uint32_t height          = rbtrend_mip_size(rbtrend_mip_size(texture->desc.height, source->desc.base_mip), 1);
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
static CR_RESULT rbtrend_cmd_record_uploads(
    rbtrend_command_list cmd_list,
    const eq::array<rbtrend_upload_copy> &copies,
    crgpu_command_list *out_upload) {
	*out_upload                                 = nullptr;
	eq::array<crgpu_buffer_barrier> &before = cmd_list->upload_before;
	eq::array<crgpu_buffer_barrier> &after  = cmd_list->upload_after;
	before.clear();
	after.clear();
	for (const rbtrend_upload_copy &copy : copies) {
		bool known = false;
		for (const crgpu_buffer_barrier &barrier : before)
			if (barrier.buffer == copy.destination)
				known = true;
		if (known)
			continue;
		rbtrend_push(before, crgpu_buffer_barrier{{CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, copy.destination, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_COPY_DEST});
		rbtrend_push(after, crgpu_buffer_barrier{{CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, copy.destination, CRGPU_RESOURCE_STATE_COPY_DEST, copy.after});
	}
	for (rbtrend_buffer_store *store : cmd_list->staged_stores) {
		if (store->uploads.empty())
			continue;
		rbtrend_push(before, crgpu_buffer_barrier{{CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, CRGPU_RESOURCE_STATE_COMMON, CRGPU_RESOURCE_STATE_COPY_DEST});
		rbtrend_push(after, crgpu_buffer_barrier{{CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, CRGPU_RESOURCE_STATE_COPY_DEST, store->use_state});
	}
	// A store whose state the list's barriers track must start the list in
	// its use_state, which the copy above leaves it in. One with nothing to
	// copy is put there directly, so the list starts the same either way.
	for (const auto &tracked : cmd_list->buffer_states) {
		rbtrend_buffer_store *store = tracked.store;
		if (store->staged && !store->uploads.empty())
			continue;
		// A copy into it leaves it in its use_state already.
		bool copied = false;
		for (const rbtrend_upload_copy &copy : copies)
			if (copy.destination == store->buffer)
				copied = true;
		if (copied)
			continue;
		rbtrend_push(before, crgpu_buffer_barrier{{CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, CRGPU_RESOURCE_STATE_COMMON, store->use_state});
	}
	if (before.empty())
		return CR_SUCCESS;

	crgpu_command_list upload = nullptr;
	CR_RESULT sr               = rbtrend_instance_take_command_list(cmd_list->instance, &upload);
	if (CR_FAILED(sr))
		return sr;
	crgpu_command_list_begin(upload);
	crgpu_command_list_pipeline_barrier(upload, before.data, (uint32_t)before.size(), nullptr, 0);
	// In each store's order: a ONCE store's uploads may overlap, and the
	// later write is the one that has to land.
	for (rbtrend_buffer_store *store : cmd_list->staged_stores)
		for (const rbtrend_buffer_upload &staged : store->uploads)
			crgpu_command_list_copy_buffer(upload, staged.source, store->buffer, staged.source_offset, staged.offset, staged.size);
	for (const rbtrend_upload_copy &copy : copies)
		crgpu_command_list_copy_buffer(upload, copy.source, copy.destination, copy.source_offset, copy.destination_offset, copy.size);
	if (!after.empty())
		crgpu_command_list_pipeline_barrier(upload, after.data, (uint32_t)after.size(), nullptr, 0);
	crgpu_command_list_end(upload);
	*out_upload = upload;
	return CR_SUCCESS;
}

/* Takes [cmd_list] off its instance's compiled lists, if it is on them, and
 * clears its stale mark. Before anything changes what the list uses. */
static void rbtrend_cmd_list_unlist(rbtrend_command_list cmd_list) {
	rbtrend_instance instance = cmd_list->instance;
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	cmd_list->stale = false;
	if (!cmd_list->listed)
		return;
	cmd_list->listed = false;
	std::vector<rbtrend_command_list> &lists = instance->compiled_lists;
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
static bool rbtrend_cmd_list_enlist(rbtrend_command_list cmd_list) {
	rbtrend_instance instance = cmd_list->instance;
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	try {
		instance->compiled_lists.push_back(cmd_list);
	} catch (const std::bad_alloc &) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "compile: out of memory");
		return false;
	}
	cmd_list->listed = true;
	cmd_list->stale  = false;
	return true;
}

/* Whether the compile of [cmd_list] reads [used] at submit. */
static bool rbtrend_cmd_list_uses(rbtrend_command_list cmd_list, const void *used) {
	// A texture's tracking is in used_handles from its first layout_use.
	for (const rbtrend_buffer_store *store : cmd_list->used_stores)
		if (store == used)
			return true;
	for (const auto &tracked : cmd_list->buffer_states)
		if (tracked.store == used)
			return true;
	for (rbtrend_material material : cmd_list->materials)
		if (material == used)
			return true;
	for (const auto &timer_use : cmd_list->timer_uses)
		if (timer_use.timer == used)
			return true;
	if (cmd_list->present_swap_chain == used)
		return true;
	return cmd_list->used_handles.find((uint64_t)(uintptr_t)used, [](uint32_t) { return true; }) != nullptr;
}

void rbtrend_instance_forget(
    rbtrend_instance instance,
    const void *used) {
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	for (rbtrend_command_list listed : instance->compiled_lists)
		if (!listed->stale && rbtrend_cmd_list_uses(listed, used))
			listed->stale = true;
}

extern "C" {

RBT_RESULT rbtrend_command_list_create(rbtrend_instance instance, rbtrend_command_list *out_list, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_list)
		*out_list = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_command_list_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_command_list_create");
	if (!out_list)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_list is NULL");
	rbtrend_command_list_t *result = (rbtrend_command_list_t *)malloc(sizeof(rbtrend_command_list_t));
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the list");
	result = new (result) rbtrend_command_list_t();

	// What it records into is taken from the instance at each compile.
	result->instance = instance;
	// Its destroy's retire, promised now so the destroy can't fail.
	if (rbtrend_instance_reserve_release(instance, 1) != RBT_RESULT_OK) {
		result->~rbtrend_command_list_t();
		free(result);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the list later");
	}
	*out_list = result;
	return RBT_RESULT_OK;
}
void rbtrend_command_list_destroy(rbtrend_command_list cmd_list) {
	if (!cmd_list)
		return;
	if (RBT_STALE(cmd_list, "rbtrend_command_list_destroy", "cmd_list"))
		return;
	// Its BUILD_TLAS seals, never submitted, are given back (T5).
	rbtrend_rt_abandon(cmd_list);
	// A compile it never submitted gives back what it took from the render
	// target pool. Here and not in its release, which may come after the
	// instance's destructor has freed the pool.
	rbtrend_render_target_give_back(cmd_list);
	// No destroy call may find it among the compiled lists once it is gone.
	rbtrend_cmd_list_unlist(cmd_list);
	// Released once the GPU has finished everything submitted so far.
	RBT_KILL(cmd_list);
	rbtrend_instance_retire(cmd_list->instance, cmd_list, &rbtrend_release_handle<rbtrend_command_list_t>);
}

void rbtrend_command_list_reset(rbtrend_command_list cmd_list) {
	if (!cmd_list)
		return;
	if (RBT_STALE(cmd_list, "rbtrend_command_list_reset", "cmd_list"))
		return;
	RBTREND_ENTER_LIST(cmd_list, "rbtrend_command_list_reset");
	// Off the compiled lists before what it uses is cleared: a destroy call
	// on another thread may be reading it.
	rbtrend_cmd_list_unlist(cmd_list);
	// Its BUILD_TLAS seals, never submitted, are given back (T5).
	rbtrend_rt_abandon(cmd_list);
	const size_t preserve_size = cmd_list->commands.size();
	cmd_list->commands.clear();
	cmd_list->commands.reserve(preserve_size);
	cmd_list->side_bytes.clear();
	cmd_list->side_refs.clear();
	cmd_list->recording_error  = false;
	cmd_list->command_sends.clear();
	cmd_list->send_count       = 0;
	cmd_list->recording_result = RBT_RESULT_OK;
	cmd_list->recording_message[0] = '\0';
	cmd_list->layout_uses.clear();
	cmd_list->layout_index.clear();
	cmd_list->storage_hazards.clear();
	cmd_list->staged_stores.clear();
	cmd_list->used_stores.clear();
	cmd_list->used_handles.clear();
	cmd_list->buffer_states.clear();
	cmd_list->materials.clear();
	rbtrend_render_target_give_back(cmd_list);
	cmd_list->compiled           = false;
	cmd_list->present_swap_chain = nullptr;
}
/* Copies [size] bytes from [bytes] to the end of [cmd_list]'s side_bytes,
 * at an offset aligned for anything a command points at, and returns that
 * offset. Throws std::bad_alloc. */
static uint32_t rbtrend_cmd_keep(rbtrend_command_list cmd_list, const void *bytes, size_t size) {
	constexpr size_t align = alignof(std::max_align_t);
	const size_t offset    = (cmd_list->side_bytes.size() + align - 1) & ~(align - 1);
	cmd_list->side_bytes.resize(offset + size);
	memcpy(cmd_list->side_bytes.data() + offset, bytes, size);
	return (uint32_t)offset;
}

/* Points each command that has a copy in [cmd_list]'s side_bytes at it:
 * after recording, when side_bytes no longer grows, and before anything
 * reads a command's array. */
static void rbtrend_cmd_point_at_copies(rbtrend_command_list cmd_list) {
	uint8_t *bytes = cmd_list->side_bytes.data();
	for (const rbtrend_command_list_t::side_ref &ref : cmd_list->side_refs) {
		RBTREND_COMMAND &cmd = cmd_list->commands[ref.command];
		void *copy          = bytes + ref.offset;
		switch (cmd._type) {
		case RBTREND_COMMAND_BEGIN_RENDERING:
			cmd._params._begin_rendering._color_targets = (RBTREND_COLOR_TARGET *)copy;
			break;
		case RBTREND_COMMAND_BEGIN_LABEL:
			cmd._params._begin_label._label = (const char *)copy;
			break;
		case RBTREND_COMMAND_SET_VERTEX_BUFFERS:
			cmd._params._set_vertex_buffers._vertex_buffers = (rbtrend_buffer *)copy;
			break;
		case RBTREND_COMMAND_SET_VIEWPORTS:
			cmd._params._set_viewports._viewports = (CRGPU_VIEWPORT *)copy;
			break;
		case RBTREND_COMMAND_SET_SCISSOR_RECTS:
			cmd._params._set_scissor_rects._scissor_rects = (CRGPU_SCISSOR_RECT *)copy;
			break;
		case RBTREND_COMMAND_PUSH_CONSTANTS:
			cmd._params._push_constants._data = copy;
			break;
		default:
			break;
		}
	}
}

/* A view a reflection command names ([who], its member [member]), checked
 * at the send: NULL, another instance's, a view of another type than
 * [type], not 2D, of another aspect than [aspect], of more than one mip
 * where [one_mip], or of another format than [format] (UNKNOWN for any;
 * [format_name] its name): each its own refusal. False, reported, at the
 * first. */
static bool rbtrend_reflect_view_ok(rbtrend_command_list cmd_list, const char *who, const char *member, rbtrend_texture_view view,
                                   RBTREND_TEXTURE_VIEW_TYPE type, const char *type_name, RBTREND_TEXTURE_VIEW_ASPECT aspect, bool one_mip,
                                   CRGPU_FORMAT format, const char *format_name) {
	if (!view)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a NULL %s", who, member);
	if (view->instance != cmd_list->instance)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a %s of another instance", who, member);
	if (view->view_type != type)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a %s that is not a %s view", who, member, type_name);
	if (view->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a %s that is not a 2D view", who, member);
	if (view->desc.aspect != aspect)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a %s that is not a view of %s", who, member,
		                          aspect == RBTREND_TEXTURE_VIEW_ASPECT_DEPTH ? "depth" : "color");
	if (one_mip && view->desc.mip_count != 1)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a %s of more than one mip", who, member);
	if (format != CRGPU_FORMAT_UNKNOWN && view->texture._t2d->desc.format != format)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a %s that is not of %s", who, member, format_name);
	return true;
}

/* [view]'s size at its mip. */
static void rbtrend_reflect_view_size(rbtrend_texture_view view, uint32_t *out_width, uint32_t *out_height) {
	*out_width  = rbtrend_mip_size(view->texture._t2d->desc.width, view->desc.base_mip);
	*out_height = rbtrend_mip_size(view->texture._t2d->desc.height, view->desc.base_mip);
}

/* Whether [format] is one LIT_REFLECT draws onto: a float format, the
 * picture linear. */
static bool rbtrend_reflect_target_format(CRGPU_FORMAT format) {
	return format == CRGPU_FORMAT_R16G16B16A16_FLOAT || format == CRGPU_FORMAT_R32G32B32A32_FLOAT || format == CRGPU_FORMAT_R11G11B10_FLOAT;
}

/* The checks the reflection commands make at the send (rbtrendcommands.h:
 * LIT_SSR, LIT_TRACE_REFLECTIONS, LIT_REFLECT), in their members' order,
 * each its own refusal. False, reported, at the first that fails. */
static bool rbtrend_reflect_send_checks(rbtrend_command_list cmd_list, const RBTREND_COMMAND &cmd) {
	rbtrend_instance instance = cmd_list->instance;
	const auto SR            = RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
	const auto UA            = RBTREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS;
	const auto RT            = RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET;
	const auto DEPTH         = RBTREND_TEXTURE_VIEW_ASPECT_DEPTH;
	const auto COLOR         = RBTREND_TEXTURE_VIEW_ASPECT_COLOR;
	const CRGPU_FORMAT any = CRGPU_FORMAT_UNKNOWN;
	const CRGPU_FORMAT f16 = RBTREND_REFLECTION_FORMAT;
	uint32_t width = 0, height = 0, w = 0, h = 0;
	if (cmd._type == RBTREND_COMMAND_LIT_SSR) {
		const auto &p   = cmd._params._lit_ssr;
		const char *who = "LIT_SSR";
		if (!p._scene)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a NULL scene", who);
		if (p._scene->instance != instance)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a scene of another instance", who);
		if (!rbtrend_reflect_view_ok(cmd_list, who, "_depth", p._depth, SR, "SHADER_RESOURCE", DEPTH, true, any, nullptr))
			return false;
		rbtrend_reflect_view_size(p._depth, &width, &height);
		if (!rbtrend_reflect_view_ok(cmd_list, who, "_normal_roughness", p._normal_roughness, SR, "SHADER_RESOURCE", COLOR, true, f16, "R16G16B16A16_FLOAT"))
			return false;
		rbtrend_reflect_view_size(p._normal_roughness, &w, &h);
		if (w != width || h != height)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _normal_roughness of %ux%u, and the depth %ux%u", who, w, h, width, height);
		if (!rbtrend_reflect_view_ok(cmd_list, who, "_color", p._color, SR, "SHADER_RESOURCE", COLOR, false, any, nullptr))
			return false;
		if (!rbtrend_reflect_view_ok(cmd_list, who, "_reflection", p._reflection, UA, "UNORDERED_ACCESS", COLOR, true, f16, "R16G16B16A16_FLOAT"))
			return false;
		rbtrend_reflect_view_size(p._reflection, &w, &h);
		if (w != (width + 1) / 2 || h != (height + 1) / 2)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _reflection of %ux%u, not half the depth's %ux%u rounded up", who, w, h, width,
			                          height);
		if (p._max_steps == 0)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with _max_steps of 0", who);
		if (p._max_steps > RBTREND_SSR_MAX_STEPS)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with _max_steps of %u, past %u", who, p._max_steps, RBTREND_SSR_MAX_STEPS);
		if (!(std::isfinite(p._max_distance) && p._max_distance > 0.0f))
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _max_distance that is not above 0, or not a number", who);
		if (!(std::isfinite(p._thickness) && p._thickness > 0.0f))
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _thickness that is not above 0, or not a number", who);
		if (!(p._edge_fade >= 0.0f && p._edge_fade <= 0.5f))
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with an _edge_fade outside 0 to 0.5, or not a number", who);
		if (!(p._max_roughness >= 0.0f && p._max_roughness <= 1.0f))
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _max_roughness outside 0 to 1, or not a number", who);
		return true;
	}
	if (cmd._type == RBTREND_COMMAND_LIT_TRACE_REFLECTIONS) {
		const auto &p   = cmd._params._lit_trace_reflections;
		const char *who = "LIT_TRACE_REFLECTIONS";
		if (!p._scene)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a NULL scene", who);
		if (p._scene->instance != instance)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a scene of another instance", who);
		if (!p._tlas)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a NULL TLAS", who);
		if (p._tlas->instance != instance)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a TLAS of another instance", who);
		if (!rbtrend_reflect_view_ok(cmd_list, who, "_depth", p._depth, SR, "SHADER_RESOURCE", DEPTH, true, any, nullptr))
			return false;
		rbtrend_reflect_view_size(p._depth, &width, &height);
		if (!rbtrend_reflect_view_ok(cmd_list, who, "_normal_roughness", p._normal_roughness, SR, "SHADER_RESOURCE", COLOR, true, f16, "R16G16B16A16_FLOAT"))
			return false;
		rbtrend_reflect_view_size(p._normal_roughness, &w, &h);
		if (w != width || h != height)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _normal_roughness of %ux%u, and the depth %ux%u", who, w, h, width, height);
		if (!rbtrend_reflect_view_ok(cmd_list, who, "_reflection", p._reflection, UA, "UNORDERED_ACCESS", COLOR, true, f16, "R16G16B16A16_FLOAT"))
			return false;
		rbtrend_reflect_view_size(p._reflection, &w, &h);
		if (w != (width + 1) / 2 || h != (height + 1) / 2)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _reflection of %ux%u, not half the depth's %ux%u rounded up", who, w, h, width,
			                          height);
		if (p._ray_count == 0)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _ray_count of 0", who);
		if (p._cull_mask == 0)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _cull_mask of 0", who);
		if (p._cull_mask > RBTREND_RT_MASK_LIMIT)
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _cull_mask of 0x%x, past 255", who, p._cull_mask);
		if (!(std::isfinite(p._max_distance) && p._max_distance > 0.0f))
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _max_distance that is not above 0, or not a number", who);
		if (!(p._max_roughness >= 0.0f && p._max_roughness <= 1.0f))
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _max_roughness outside 0 to 1, or not a number", who);
		return true;
	}
	const auto &p   = cmd._params._lit_reflect;
	const char *who = "LIT_REFLECT";
	if (!p._scene)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a NULL scene", who);
	if (p._scene->instance != instance)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a scene of another instance", who);
	if (!rbtrend_reflect_view_ok(cmd_list, who, "_depth", p._depth, SR, "SHADER_RESOURCE", DEPTH, true, any, nullptr))
		return false;
	rbtrend_reflect_view_size(p._depth, &width, &height);
	if (!rbtrend_reflect_view_ok(cmd_list, who, "_normal_roughness", p._normal_roughness, SR, "SHADER_RESOURCE", COLOR, true, f16, "R16G16B16A16_FLOAT"))
		return false;
	rbtrend_reflect_view_size(p._normal_roughness, &w, &h);
	if (w != width || h != height)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _normal_roughness of %ux%u, and the depth %ux%u", who, w, h, width, height);
	if (!rbtrend_reflect_view_ok(cmd_list, who, "_specular", p._specular, SR, "SHADER_RESOURCE", COLOR, true, RBTREND_SPECULAR_FORMAT, "R8G8B8A8_UNORM"))
		return false;
	rbtrend_reflect_view_size(p._specular, &w, &h);
	if (w != width || h != height)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _specular of %ux%u, and the depth %ux%u", who, w, h, width, height);
	if (!rbtrend_reflect_view_ok(cmd_list, who, "_reflection", p._reflection, SR, "SHADER_RESOURCE", COLOR, true, f16, "R16G16B16A16_FLOAT"))
		return false;
	rbtrend_reflect_view_size(p._reflection, &w, &h);
	if (w != (width + 1) / 2 || h != (height + 1) / 2)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _reflection of %ux%u, not half the depth's %ux%u rounded up", who, w, h, width, height);
	if (!rbtrend_reflect_view_ok(cmd_list, who, "_target", p._target, RT, "RENDER_TARGET", COLOR, true, any, nullptr))
		return false;
	if (!rbtrend_reflect_target_format(p._target->texture._t2d->desc.format))
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _target not of R16G16B16A16_FLOAT, R32G32B32A32_FLOAT or R11G11B10_FLOAT", who);
	rbtrend_reflect_view_size(p._target, &w, &h);
	if (w != width || h != height)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s with a _target of %ux%u, and the depth %ux%u", who, w, h, width, height);
	return true;
}

/* rbtrend_command_list_send's checks and copies. Throws std::bad_alloc from the
 * list's vectors, which rbtrend_command_list_send catches. */
static void rbtrend_send_command_recorded(
    rbtrend_command_list cmd_list,
    RBTREND_COMMAND cmd) {
	// Where this command's copy is in side_bytes, if it has one: recorded
	// in side_refs with the command, once every check has passed.
	bool has_copy        = false;
	uint32_t copy_offset = 0;
	auto keep            = [&](const void *bytes, size_t size) {
		copy_offset = rbtrend_cmd_keep(cmd_list, bytes, size);
		has_copy    = true;
		return (void *)(cmd_list->side_bytes.data() + copy_offset);
	};

	// Ray tracing's commands (rbtrendraytracing.h): every one needs ray
	// queries, refused before anything else.
	if (cmd._type == RBTREND_COMMAND_BUILD_BLAS || cmd._type == RBTREND_COMMAND_BUILD_TLAS || cmd._type == RBTREND_COMMAND_LIT_SHADOW_MASK ||
	    cmd._type == RBTREND_COMMAND_LIT_PATH_TRACE || cmd._type == RBTREND_COMMAND_LIT_TRACE_REFLECTIONS) {
		if (!(cmd_list->instance->features & CRGPU_FEATURE_RAY_QUERY)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_UNSUPPORTED, "%s: the device was not created with CRGPU_FEATURE_RAY_QUERY", rbtrend_rt_command_name(cmd._type));
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_rt_send_checks(cmd_list, cmd)) {
			cmd_list->recording_error = true;
			return;
		}
	}

	// The three commands that stand for others are expanded into them here,
	// from the pass or list as it is now, and are not recorded themselves.
	if (cmd._type == RBTREND_COMMAND_BEGIN_RENDER_PASS) {
		rbtrend_render_pass pass = cmd._params._begin_render_pass._pass;
		if (!pass) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDER_PASS with a NULL pass");
			cmd_list->recording_error = true;
			return;
		}
		if (pass->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDER_PASS with a pass of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_render_pass_expand(cmd_list, pass))
			cmd_list->recording_error = true;
		return;
	}
	if (cmd._type == RBTREND_COMMAND_SET_COMPUTE_PASS) {
		rbtrend_compute_pass pass = cmd._params._set_compute_pass._pass;
		if (!pass) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_COMPUTE_PASS with a NULL pass");
			cmd_list->recording_error = true;
			return;
		}
		if (pass->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_COMPUTE_PASS with a pass of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_compute_pass_expand(cmd_list, pass))
			cmd_list->recording_error = true;
		return;
	}
	if (cmd._type == RBTREND_COMMAND_DRAW_BATCH) {
		const auto &p = cmd._params._draw_batch;
		if (!p._batch) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_BATCH with a NULL batch");
			cmd_list->recording_error = true;
			return;
		}
		if (p._batch->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_BATCH with a batch of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_batch_expand(cmd_list, p._batch))
			cmd_list->recording_error = true;
		return;
	}
	if (cmd._type == RBTREND_COMMAND_DRAW_LIST) {
		const auto &p = cmd._params._draw_list;
		if (!p._list) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_LIST with a NULL list");
			cmd_list->recording_error = true;
			return;
		}
		if (p._list->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_LIST with a list of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._material_slot != RBTREND_DRAW_LIST_NO_MATERIAL && p._material_slot >= RBTREND_MAX_BINDING_LAYOUTS) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_LIST with _material_slot %u, at or above RBTREND_MAX_BINDING_LAYOUTS", p._material_slot);
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_draw_list_expand(cmd_list, p._list, p._material_slot))
			cmd_list->recording_error = true;
		return;
	}

	if (cmd._type == RBTREND_COMMAND_SET_MATERIAL) {
		const auto &p = cmd._params._set_material;
		if (!p._material) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_MATERIAL with a NULL material");
			cmd_list->recording_error = true;
			return;
		}
		if (p._material->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_MATERIAL with a material of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._slot >= RBTREND_MAX_BINDING_LAYOUTS) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_MATERIAL with _slot %u, at or above RBTREND_MAX_BINDING_LAYOUTS", p._slot);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == RBTREND_COMMAND_BEGIN_RENDERING) {
		auto &p = cmd._params._begin_rendering;
		if (p._color_target_count > RBTREND_MAX_COLOR_TARGETS || (p._color_target_count > 0 && !p._color_targets)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING with invalid color targets (count %u)", p._color_target_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._timer && p._timer->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING with a _timer of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._timer && p._timer_slot >= p._timer->slot_count) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING with _timer_slot %u, at or above the timer's %u slots", p._timer_slot, p._timer->slot_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._color_target_count > 0) {
			p._color_targets = (RBTREND_COLOR_TARGET *)keep(p._color_targets, sizeof(RBTREND_COLOR_TARGET) * p._color_target_count);
		} else {
			p._color_targets = nullptr;
		}
	}

	if (cmd._type == RBTREND_COMMAND_BEGIN_LABEL) {
		auto &p = cmd._params._begin_label;
		const char *label = p._label ? p._label : "";
		p._label          = (const char *)keep(label, strlen(label) + 1);
	}

	// The array commands below are copied the same way: the caller's array
	// only has to live for this call.
	if (cmd._type == RBTREND_COMMAND_SET_VERTEX_BUFFERS) {
		auto &p = cmd._params._set_vertex_buffers;
		if (p._vertex_buffer_count > 0 && !p._vertex_buffers) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS with a NULL array (count %u)", p._vertex_buffer_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._start_slot > RBTREND_MAX_VERTEX_BINDINGS) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS with _start_slot %u, above RBTREND_MAX_VERTEX_BINDINGS", p._start_slot);
			cmd_list->recording_error = true;
			return;
		}
		if (p._vertex_buffer_count > RBTREND_MAX_VERTEX_BINDINGS - p._start_slot) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS of %u buffers from slot %u runs past the last vertex buffer slot", p._vertex_buffer_count, p._start_slot);
			cmd_list->recording_error = true;
			return;
		}
		for (uint32_t i = 0; i < p._vertex_buffer_count; ++i) {
			if (!p._vertex_buffers[i]) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS vertex buffer %u is NULL", i);
				cmd_list->recording_error = true;
				return;
			}
			if (p._vertex_buffers[i]->instance != cmd_list->instance) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS vertex buffer %u is of another instance", i);
				cmd_list->recording_error = true;
				return;
			}
			if (!(p._vertex_buffers[i]->desc.usage & RBTREND_BUFFER_USAGE_VERTEX)) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_VERTEX_BUFFERS vertex buffer %u was created without RBTREND_BUFFER_USAGE_VERTEX", i);
				cmd_list->recording_error = true;
				return;
			}
		}
		if (p._vertex_buffer_count > 0) {
			p._vertex_buffers = (rbtrend_buffer *)keep(p._vertex_buffers, sizeof(rbtrend_buffer) * p._vertex_buffer_count);
		} else {
			p._vertex_buffers = nullptr;
		}
	}

	if (cmd._type == RBTREND_COMMAND_SET_INDEX_BUFFER) {
		const auto &p = cmd._params._set_index_buffer;
		if (!p._index_buffer) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_INDEX_BUFFER with a NULL buffer");
			cmd_list->recording_error = true;
			return;
		}
		if (p._index_buffer->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_INDEX_BUFFER with a buffer of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!(p._index_buffer->desc.usage & RBTREND_BUFFER_USAGE_INDEX)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_INDEX_BUFFER with a buffer created without RBTREND_BUFFER_USAGE_INDEX");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == RBTREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER) {
		const auto &p = cmd._params._set_transient_vertex_buffer;
		if (p._slot >= RBTREND_MAX_VERTEX_BINDINGS) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_TRANSIENT_VERTEX_BUFFER with _slot %u, at or above RBTREND_MAX_VERTEX_BINDINGS", p._slot);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == RBTREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER) {
		const auto &p = cmd._params._set_transient_index_buffer;
		if (p._index_size != 2 && p._index_size != 4) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_TRANSIENT_INDEX_BUFFER with an _index_size of %u, which is not 2 or 4", p._index_size);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == RBTREND_COMMAND_SET_VIEWPORTS) {
		auto &p = cmd._params._set_viewports;
		if (p._viewport_count > 0 && !p._viewports) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_VIEWPORTS with a NULL array (count %u)", p._viewport_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._viewport_count > 0) {
			p._viewports = (CRGPU_VIEWPORT *)keep(p._viewports, sizeof(CRGPU_VIEWPORT) * p._viewport_count);
		} else {
			p._viewports = nullptr;
		}
	}

	if (cmd._type == RBTREND_COMMAND_SET_SCISSOR_RECTS) {
		auto &p = cmd._params._set_scissor_rects;
		if (p._scissor_rect_count > 0 && !p._scissor_rects) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_SCISSOR_RECTS with a NULL array (count %u)", p._scissor_rect_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._scissor_rect_count > 0) {
			p._scissor_rects = (CRGPU_SCISSOR_RECT *)keep(p._scissor_rects, sizeof(CRGPU_SCISSOR_RECT) * p._scissor_rect_count);
		} else {
			p._scissor_rects = nullptr;
		}
	}

	if (cmd._type == RBTREND_COMMAND_DRAW_INDIRECT || cmd._type == RBTREND_COMMAND_DRAW_INDEXED_INDIRECT) {
		const auto &p = cmd._params._draw_indirect;
		// What one entry is depends on which of the two commands this is.
		const uint64_t entry_size = cmd._type == RBTREND_COMMAND_DRAW_INDIRECT ? sizeof(crgpu_draw_indirect_args)
		                                                                       : sizeof(crgpu_draw_indexed_indirect_args);
		if (!p._buffer) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indirect draw with a NULL _buffer");
			cmd_list->recording_error = true;
			return;
		}
		if (p._buffer->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indirect draw with a _buffer of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!(p._buffer->desc.usage & RBTREND_BUFFER_USAGE_INDIRECT_ARGUMENTS)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indirect draw with a buffer created without RBTREND_BUFFER_USAGE_INDIRECT_ARGUMENTS");
			cmd_list->recording_error = true;
			return;
		}
		// Every copy has one size.
		const uint64_t arguments_size = p._buffer->desc.size;
		if (p._draw_count == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indirect draw with a _draw_count of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset % 4 != 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indirect draw with an _offset that is not a multiple of 4");
			cmd_list->recording_error = true;
			return;
		}
		if (p._stride % 4 != 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indirect draw with _stride %u, not a multiple of 4", p._stride);
			cmd_list->recording_error = true;
			return;
		}
		if (p._stride < entry_size) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indirect draw with _stride %u, smaller than the %u bytes of one entry", p._stride, (unsigned)entry_size);
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset > arguments_size) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indirect draw with an _offset past the end of the buffer");
			cmd_list->recording_error = true;
			return;
		}
		// Where the last entry ends: every one before it is a whole stride.
		const uint64_t span = (uint64_t)(p._draw_count - 1) * p._stride + entry_size;
		if (span > arguments_size - p._offset) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indirect draw of %u entries runs past the end of the buffer", p._draw_count);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == RBTREND_COMMAND_DISPATCH_MESH) {
		const auto &p = cmd._params._dispatch_mesh;
		if (p._group_count_x == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DISPATCH_MESH with a _group_count_x of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_y == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DISPATCH_MESH with a _group_count_y of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_z == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DISPATCH_MESH with a _group_count_z of 0");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == RBTREND_COMMAND_DISPATCH) {
		const auto &p = cmd._params._dispatch;
		if (p._group_count_x == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DISPATCH with a _group_count_x of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_y == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DISPATCH with a _group_count_y of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_z == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DISPATCH with a _group_count_z of 0");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == RBTREND_COMMAND_PUSH_CONSTANTS) {
		auto &p = cmd._params._push_constants;
		if (!p._data) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS with NULL _data");
			cmd_list->recording_error = true;
			return;
		}
		if (p._size == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS with a _size of 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset % 4 != 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS with _offset %u, not a multiple of 4", p._offset);
			cmd_list->recording_error = true;
			return;
		}
		if (p._size % 4 != 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS with _size %u, not a multiple of 4", p._size);
			cmd_list->recording_error = true;
			return;
		}
		p._data = keep(p._data, p._size);
	}
	if (cmd._type == RBTREND_COMMAND_BLOOM) {
		const auto &p = cmd._params._bloom;
		if (!p._source) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a NULL source");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a source of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a source that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a source that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_COLOR) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a source that is not a view of color");
			cmd_list->recording_error = true;
			return;
		}
		if (!p._target) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a NULL target");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a target of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->view_type != RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a target that is not a RENDER_TARGET view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a target that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		// A 2D render target view is of a 2D texture.
		if (!rbtrend_bloom_target_format(p._target->texture._t2d->desc.format)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a target in format %d, which a bloom doesn't render onto", (int)p._target->texture._t2d->desc.format);
			cmd_list->recording_error = true;
			return;
		}
		if (rbtrend_texture_ranges_overlap(rbtrend_texture_view_range(p._source), rbtrend_texture_view_range(p._target))) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a source and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._threshold) && p._threshold >= 0.0f)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a _threshold that is negative or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._knee) && p._knee >= 0.0f)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a _knee that is negative or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._radius) && p._radius > 0.0f)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a _radius that is not above 0, or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._intensity) && p._intensity >= 0.0f)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with an _intensity that is negative or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (p._levels == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with _levels 0");
			cmd_list->recording_error = true;
			return;
		}
		const uint32_t most = rbtrend_bloom_max_levels(p._source);
		if (p._levels > most) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with _levels %u, and its source's size allows at most %u", p._levels, most);
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask && !rbtrend_bloom_read_view_valid(cmd_list, p._mask, "_mask")) {
			cmd_list->recording_error = true;
			return;
		}
		if (p._lens_dirt && !rbtrend_bloom_read_view_valid(cmd_list, p._lens_dirt, "_lens_dirt")) {
			cmd_list->recording_error = true;
			return;
		}
		if (p._lens_dirt && rbtrend_texture_ranges_overlap(rbtrend_texture_view_range(p._lens_dirt), rbtrend_texture_view_range(p._target))) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a _lens_dirt and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		if (p._lens_dirt && !(std::isfinite(p._lens_dirt_intensity) && p._lens_dirt_intensity >= 0.0f)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM with a _lens_dirt_intensity that is negative or not a number");
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_GENERATE_MIPS) {
		const auto &p = cmd._params._generate_mips;
		if (!p._texture) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "GENERATE_MIPS with a NULL texture");
			cmd_list->recording_error = true;
			return;
		}
		if (p._texture->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "GENERATE_MIPS with a texture of another instance");
			cmd_list->recording_error = true;
			return;
		}
		RBTREND_ERROR error{};
		if (rbtrend_generate_mips_valid(p._texture, p._base_mip, p._filter, &error) != RBT_RESULT_OK) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "GENERATE_MIPS: %s", error.message);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == RBTREND_COMMAND_PRESENT_TEXTURE) {
		const auto &p = cmd._params._present_texture;
		// What the draw modes read; NONE, the copy, reads neither.
		if (p._scaling != RBTREND_TEXTURE_SCALING_NONE &&
		    !rbtrend_texture_scaling_valid(cmd_list, "PRESENT_TEXTURE", p._scaling, p._filter, p._border_color,
		                                  p._scaling == RBTREND_TEXTURE_SCALING_ASPECT_FIT)) {
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_DRAW_TEXTURE) {
		const auto &p = cmd._params._draw_texture;
		if (!p._source) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a NULL source");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_COLOR) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source that is not a view of color");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.mip_count != 1) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source of %u mips; it reads one", p._source->desc.mip_count);
			cmd_list->recording_error = true;
			return;
		}
		if (!p._target) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a NULL target");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a target of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->view_type != RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a target that is not a RENDER_TARGET view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a target that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		// A 2D render target view is of a 2D texture. The shader writes
		// floats, which an integer target doesn't hold.
		const CRGPU_FORMAT target_format = p._target->texture._t2d->desc.format;
		if (rbtrend_format_scalar_type(target_format) != RBTREND_SHADER_SCALAR_TYPE_FLOAT) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a target of format %u, which doesn't hold floats", (unsigned)target_format);
			cmd_list->recording_error = true;
			return;
		}
		if (rbtrend_texture_ranges_overlap(rbtrend_texture_view_range(p._source), rbtrend_texture_view_range(p._target))) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE with a source and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_texture_scaling_valid(cmd_list, "DRAW_TEXTURE", p._scaling, p._filter, p._border_color, p._scaling != RBTREND_TEXTURE_SCALING_STRETCH)) {
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_texel_rect_valid(cmd_list, "_source_rect", p._source_rect, p._source)) {
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_texel_rect_valid(cmd_list, "_target_rect", p._target_rect, p._target)) {
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_COPY_SCENE_COLOR) {
		const auto &p = cmd._params._copy_scene_color;
		const char *wrong = nullptr;
		if (!p._source)
			wrong = "a NULL source";
		else if (p._source->instance != cmd_list->instance)
			wrong = "a source of another instance";
		else if (p._source->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE)
			wrong = "a source that is not a SHADER_RESOURCE view";
		else if (p._source->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D)
			wrong = "a source that is not a 2D view";
		else if (p._source->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_COLOR)
			wrong = "a source that is not a view of color";
		else if (p._source->desc.mip_count != 1)
			wrong = "a source of more than one mip";
		else if (!p._target)
			wrong = "a NULL target";
		else if (p._target->instance != cmd_list->instance)
			wrong = "a target of another instance";
		else if (p._target->view_type != RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET)
			wrong = "a target that is not a RENDER_TARGET view";
		else if (p._target->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D)
			wrong = "a target that is not a 2D view";
		else if (rbtrend_format_scalar_type(p._target->texture._t2d->desc.format) != RBTREND_SHADER_SCALAR_TYPE_FLOAT)
			wrong = "a target in a format that doesn't hold floats";
		else if (rbtrend_texture_ranges_overlap(rbtrend_texture_view_range(p._source), rbtrend_texture_view_range(p._target)))
			wrong = "a source and a target that share a mip of a layer";
		if (wrong) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "COPY_SCENE_COLOR with %s", wrong);
			cmd_list->recording_error = true;
			return;
		}
		RBTREND_ERROR error{};
		if (rbtrend_generate_mips_valid(p._target->texture._t2d, p._target->desc.base_mip, CRGPU_FILTER_LINEAR, &error) != RBT_RESULT_OK) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "COPY_SCENE_COLOR: %s", error.message);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_BLUR) {
		const auto &p = cmd._params._blur;
		if (!p._source) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a NULL source");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a source of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a source that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a source that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_COLOR) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a source that is not a view of color");
			cmd_list->recording_error = true;
			return;
		}
		if (p._source->desc.mip_count != 1) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a source of %u mips; it reads one", p._source->desc.mip_count);
			cmd_list->recording_error = true;
			return;
		}
		if (!p._target) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a NULL target");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a target of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->view_type != RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a target that is not a RENDER_TARGET view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a target that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		// A 2D render target view is of a 2D texture.
		if (!rbtrend_builtin_downsample_format(p._target->texture._t2d->desc.format)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a target in format %d, which a blur doesn't render onto", (int)p._target->texture._t2d->desc.format);
			cmd_list->recording_error = true;
			return;
		}
		// A paired tap reads two texels only on the target's own grid.
		uint32_t source_width = 0, source_height = 0, target_width = 0, target_height = 0;
		rbtrend_view_size(p._source, &source_width, &source_height);
		rbtrend_view_size(p._target, &target_width, &target_height);
		if (source_width != target_width) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a source %u wide and a target %u wide; they are one size", source_width, target_width);
			cmd_list->recording_error = true;
			return;
		}
		if (source_height != target_height) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a source %u high and a target %u high; they are one size", source_height, target_height);
			cmd_list->recording_error = true;
			return;
		}
		if (rbtrend_texture_ranges_overlap(rbtrend_texture_view_range(p._source), rbtrend_texture_view_range(p._target))) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a source and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._sigma) && p._sigma > 0.0f)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a _sigma that is not above 0, or not a number");
			cmd_list->recording_error = true;
			return;
		}
		if (p._sigma > RBTREND_BLUR_MAX_SIGMA) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR with a _sigma of %g, above RBTREND_BLUR_MAX_SIGMA", (double)p._sigma);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_OUTLINE) {
		const auto &p = cmd._params._outline;
		if (!p._mask) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a NULL mask");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a mask of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a mask that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a mask that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_COLOR) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a mask that is not a view of color");
			cmd_list->recording_error = true;
			return;
		}
		if (p._mask->desc.mip_count != 1) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a mask of %u mips; it reads one", p._mask->desc.mip_count);
			cmd_list->recording_error = true;
			return;
		}
		if (rbtrend_format_scalar_type(p._mask->texture._t2d->desc.format) != RBTREND_SHADER_SCALAR_TYPE_FLOAT) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a mask of format %u, which a shader doesn't read as floats", (unsigned)p._mask->texture._t2d->desc.format);
			cmd_list->recording_error = true;
			return;
		}
		if (!p._target) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a NULL target");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a target of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->view_type != RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a target that is not a RENDER_TARGET view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._target->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a target that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_builtin_downsample_format(p._target->texture._t2d->desc.format)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a target in format %d, which an outline doesn't render onto", (int)p._target->texture._t2d->desc.format);
			cmd_list->recording_error = true;
			return;
		}
		if (!rbtrend_views_one_size(p._mask, p._target)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a mask and a target of different sizes");
			cmd_list->recording_error = true;
			return;
		}
		if (rbtrend_texture_ranges_overlap(rbtrend_texture_view_range(p._mask), rbtrend_texture_view_range(p._target))) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a mask and a target that share a mip of a layer");
			cmd_list->recording_error = true;
			return;
		}
		for (int i = 0; i < 4; ++i) {
			if (!std::isfinite(p._color[i])) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a _color[%d] that is not finite", i);
				cmd_list->recording_error = true;
				return;
			}
		}
		for (int i = 0; i < 3; ++i) {
			if (p._color[i] < 0.0f) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a negative _color[%d]", i);
				cmd_list->recording_error = true;
				return;
			}
		}
		if (p._color[3] < 0.0f) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a _color alpha below 0");
			cmd_list->recording_error = true;
			return;
		}
		if (p._color[3] > 1.0f) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a _color alpha above 1");
			cmd_list->recording_error = true;
			return;
		}
		if (!(std::isfinite(p._width) && p._width > 0.0f)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE with a _width that is not above 0, or not a number");
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_LIT_CULL_LIGHTS) {
		const auto &p = cmd._params._lit_cull_lights;
		if (!p._scene) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a NULL scene");
			cmd_list->recording_error = true;
			return;
		}
		if (p._scene->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a scene of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (!p._depth) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a NULL depth");
			cmd_list->recording_error = true;
			return;
		}
		if (p._depth->instance != cmd_list->instance) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a depth of another instance");
			cmd_list->recording_error = true;
			return;
		}
		if (p._depth->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a depth that is not a SHADER_RESOURCE view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._depth->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a depth that is not a 2D view");
			cmd_list->recording_error = true;
			return;
		}
		if (p._depth->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_DEPTH) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS with a depth that is not a view of depth");
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_TONE_MAP) {
		const auto &p = cmd._params._tone_map;
		const char *fault = nullptr;
		if (!p._source || !p._target)
			fault = "a NULL source or target";
		else if (p._source->instance != cmd_list->instance || p._target->instance != cmd_list->instance)
			fault = "a source or target of another instance";
		else if (p._source->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || p._source->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D ||
		         p._source->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_COLOR)
			fault = "a source that is not a SHADER_RESOURCE 2D view of color";
		else if (p._source->desc.mip_count != 1)
			fault = "a source of more than one mip";
		else if (p._target->view_type != RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET || p._target->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D)
			fault = "a target that is not a RENDER_TARGET 2D view";
		else if (!rbtrend_builtin_downsample_format(p._target->texture._t2d->desc.format))
			fault = "a target in a format it doesn't render onto";
		else if (rbtrend_texture_ranges_overlap(rbtrend_texture_view_range(p._source), rbtrend_texture_view_range(p._target)))
			fault = "a source and a target that share a mip of a layer";
		else if (!(std::isfinite(p._exposure) && p._exposure >= 0.0f))
			fault = "an _exposure that is negative or not a number";
		else if (p._curve > RBTREND_TONE_CURVE_AGX)
			fault = "a _curve that is not an RBTREND_TONE_CURVE";
		else if (p._encoding > RBTREND_COLOR_ENCODING_SRGB)
			fault = "an _encoding that is not an RBTREND_COLOR_ENCODING";
		else if (p._auto_exposure && p._auto_exposure->instance != cmd_list->instance)
			fault = "an _auto_exposure of another instance";
		if (fault) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "TONE_MAP with %s", fault);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_LIT_ENVIRONMENT) {
		const auto &p = cmd._params._lit_environment;
		const char *fault = nullptr;
		if (!p._environment || !p._source)
			fault = "a NULL environment or source";
		else if (p._environment->instance != cmd_list->instance || p._source->instance != cmd_list->instance)
			fault = "an environment or source of another instance";
		else if (p._source->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || p._source->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_COLOR ||
		         (p._source->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_CUBE && p._source->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D))
			fault = "a source that is not a SHADER_RESOURCE view of color shaped CUBE or 2D";
		if (fault) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_ENVIRONMENT with %s", fault);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_LIT_SKY) {
		const auto &p = cmd._params._lit_sky;
		const char *fault = nullptr;
		const float *d    = p._sun_direction;
		const float *g    = p._ground_albedo;
		const bool hosek  = p._model == RBTREND_LIT_SKY_MODEL_HOSEK_WILKIE;
		if (!p._environment)
			fault = "a NULL environment";
		else if (p._environment->instance != cmd_list->instance)
			fault = "an environment of another instance";
		else if (p._model != RBTREND_LIT_SKY_MODEL_PREETHAM && !hosek)
			fault = "a _model that is not an RBTREND_LIT_SKY_MODEL";
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
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_SKY with %s", fault);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_LIT_SSR || cmd._type == RBTREND_COMMAND_LIT_TRACE_REFLECTIONS || cmd._type == RBTREND_COMMAND_LIT_REFLECT) {
		if (!rbtrend_reflect_send_checks(cmd_list, cmd)) {
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_LIT_AMBIENT_OCCLUSION) {
		const auto &p = cmd._params._lit_ambient_occlusion;
		const char *fault = nullptr;
		if (!p._scene || !p._depth || !p._target)
			fault = "a NULL scene, depth or target";
		else if (p._scene->instance != cmd_list->instance || p._depth->instance != cmd_list->instance || p._target->instance != cmd_list->instance)
			fault = "a scene, depth or target of another instance";
		else if (p._depth->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || p._depth->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D ||
		         p._depth->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_DEPTH)
			fault = "a depth that is not a SHADER_RESOURCE 2D view of depth";
		else if (p._target->view_type != RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET || p._target->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D ||
		         !rbtrend_builtin_downsample_format(p._target->texture._t2d->desc.format))
			fault = "a target that is not a RENDER_TARGET 2D view of a format it renders onto";
		else if (rbtrend_mip_size(p._depth->texture._t2d->desc.width, p._depth->desc.base_mip) !=
		             rbtrend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip) ||
		         rbtrend_mip_size(p._depth->texture._t2d->desc.height, p._depth->desc.base_mip) !=
		             rbtrend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip))
			fault = "a depth and a target of different sizes";
		else if (!(std::isfinite(p._radius) && p._radius > 0.0f) || !(std::isfinite(p._intensity) && p._intensity >= 0.0f))
			fault = "a _radius that is not above 0, or an _intensity below 0, or either not a number";
		if (fault) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_AMBIENT_OCCLUSION with %s", fault);
			cmd_list->recording_error = true;
			return;
		}
	}
	if (cmd._type == RBTREND_COMMAND_AUTO_EXPOSURE) {
		const auto &p = cmd._params._auto_exposure;
		const char *fault = nullptr;
		if (!p._exposure || !p._source)
			fault = "a NULL exposure or source";
		else if (p._exposure->instance != cmd_list->instance || p._source->instance != cmd_list->instance)
			fault = "an exposure or source of another instance";
		else if (p._source->view_type != RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || p._source->desc.shape != RBTREND_TEXTURE_VIEW_SHAPE_2D ||
		         p._source->desc.aspect != RBTREND_TEXTURE_VIEW_ASPECT_COLOR || p._source->desc.mip_count != 1)
			fault = "a source that is not a SHADER_RESOURCE 2D view of color of one mip";
		else if (!(std::isfinite(p._delta_time) && p._delta_time >= 0.0f))
			fault = "a _delta_time that is negative or not a number";
		if (fault) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "AUTO_EXPOSURE with %s", fault);
			cmd_list->recording_error = true;
			return;
		}
	}

	// Ray tracing's commands change what they name only once the command can
	// be kept: room is made first, so nothing below can fail after the seal.
	if (cmd._type == RBTREND_COMMAND_BUILD_BLAS || cmd._type == RBTREND_COMMAND_BUILD_TLAS) {
		cmd_list->commands.reserve(cmd_list->commands.size() + 1);
		cmd_list->command_sends.reserve(cmd_list->command_sends.size() + 1);
		if (!rbtrend_rt_send_commit(cmd_list, &cmd)) {
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
void rbtrend_command_list_send(
    rbtrend_command_list cmd_list,
    RBTREND_COMMAND cmd) {
	if (!cmd_list)
		return;
	if (RBT_STALE(cmd_list, "rbtrend_command_list_send", "cmd_list"))
		return;
	RBTREND_ENTER_LIST(cmd_list, "rbtrend_command_list_send");
	if (cmd._type == RBTREND_COMMAND_NONE)
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
		rbtrend_send_command_recorded(cmd_list, cmd);
	} catch (const std::bad_alloc &) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "no memory to keep the command; the list fails its compile until it is reset");
		cmd_list->recording_error = true;
	}
	--cmd_list->send_depth;
}

/* Where [cmd_list] keeps the layout of (mip, layer) of [range]'s texture, as
 * an index into layout_uses, adding it the first time the list uses that
 * subresource: it starts from the layout everything submitted so far leaves
 * it in. An index, not a pointer: adding grows the vector. */
static size_t rbtrend_cmd_layout_use(
    rbtrend_command_list cmd_list,
    const rbtrend_texture_range &range,
    uint32_t mip,
    uint32_t layer) {
	const uint64_t hash = rbtrend_subresource_hash(range.image, mip, layer);
	const uint32_t *found = cmd_list->layout_index.find(hash, [&](uint32_t at) {
		const rbtrend_command_list_t::layout_use &use = cmd_list->layout_uses[at];
		return use.image == range.image && use.mip == mip && use.layer == layer;
	});
	if (found)
		return *found;
	CRGPU_IMAGE_LAYOUT *tracked = range.tracking->at(mip, layer);
	cmd_list->layout_uses.push_back({range.image, mip, layer, tracked, range.tracking, *tracked, *tracked, true, true});
	const size_t index = cmd_list->layout_uses.size() - 1;
	cmd_list->layout_index.add(hash, (uint32_t)index);
	// What rbtrend_instance_forget looks for when the texture goes.
	rbtrend_cmd_use_handle(cmd_list, range.tracking);
	return index;
}

/* Moves every mip of every layer in [range] into [layout]. Unless
 * [skip_if_same], a barrier is recorded even for one already there:
 * back-to-back passes on the same attachment (a LOAD after a previous pass's
 * STORE) still need the barrier's memory dependency, and for a storage image
 * a barrier from GENERAL to GENERAL is what orders one shader's writes before
 * the next one's accesses (crgpu.h). Two reads in a row need nothing, which
 * is what [skip_if_same] is for.
 *
 * The subresources of a range are usually all in one layout, and then it is
 * one barrier over the range; where they differ, each run of mips of a layer
 * that share a layout is one barrier from it.
 *
 * Tracks the layouts in [cmd_list]'s own layout_uses, never on the texture:
 * a texture's layouts only change once the list is actually submitted.
 * Recorded outside any pass. */
static void rbtrend_cmd_transition_range(
    rbtrend_command_list cmd_list,
    const rbtrend_texture_range &range,
    CRGPU_IMAGE_LAYOUT layout,
    bool skip_if_same) {
	// Make sure the list has an entry for each subresource, and see whether
	// they agree on where they are now.
	bool uniform                  = true;
	CRGPU_IMAGE_LAYOUT from     = CRGPU_IMAGE_LAYOUT_UNDEFINED;
	for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
		for (uint32_t mip = 0; mip < range.mip_count; ++mip) {
			const size_t index                = rbtrend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer);
			const CRGPU_IMAGE_LAYOUT current = cmd_list->layout_uses[index].final_layout;
			if (layer == 0 && mip == 0)
				from = current;
			else if (current != from)
				uniform = false;
		}
	}

	if (uniform) {
		if (skip_if_same && from == layout)
			return;
		const crgpu_image_view_desc_subresource_range cr_range = rbtrend_texture_range_crgpu(range);
		crgpu_command_list_image_barrier_subresource(cmd_list->cmd_list, range.image, &cr_range, from, layout);
	}
	for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
		uint32_t mip = 0;
		while (mip < range.mip_count) {
			// The run of mips from [mip] in one layout: one barrier from it,
			// or none where it is there already and that is allowed.
			const size_t first             = rbtrend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer);
			const CRGPU_IMAGE_LAYOUT run = cmd_list->layout_uses[first].final_layout;
			uint32_t end                   = mip + 1;
			while (!uniform && end < range.mip_count &&
			       cmd_list->layout_uses[rbtrend_cmd_layout_use(cmd_list, range, range.base_mip + end, range.base_layer + layer)].final_layout == run)
				++end;
			bool barrier = uniform;
			if (!uniform && !(skip_if_same && run == layout)) {
				crgpu_image_view_desc_subresource_range runs{};
				runs.base_mip_level    = range.base_mip + mip;
				runs.mip_level_count   = end - mip;
				runs.base_array_layer  = range.base_layer + layer;
				runs.array_layer_count = 1;
				crgpu_command_list_image_barrier_subresource(cmd_list->cmd_list, range.image, &runs, run, layout);
				barrier = true;
			}
			for (; mip < end; ++mip) {
				auto &use = cmd_list->layout_uses[rbtrend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer)];
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
static void rbtrend_cmd_transition_view(
    rbtrend_command_list cmd_list,
    rbtrend_texture_view view,
    CRGPU_IMAGE_LAYOUT layout) {
	rbtrend_cmd_transition_range(cmd_list, rbtrend_texture_view_range(view), layout, false);
}

/* PRESENT_TEXTURE: copies the source into the acquired back buffer and moves
 * the back buffer to PRESENT_SRC. The back buffer's previous contents are
 * discarded (UNDEFINED old layout) since the copy overwrites the region that
 * gets shown. */
static bool rbtrend_cmd_range_written(rbtrend_command_list cmd_list, const rbtrend_texture_range &range);

/* [rect] made whole: a zeroed one is all of a mip [width] by [height]. */
static RBTREND_TEXEL_RECT rbtrend_texel_rect_resolved(const RBTREND_TEXEL_RECT *rect, uint32_t width, uint32_t height) {
	if (rect && rect->width != 0)
		return *rect;
	RBTREND_TEXEL_RECT whole{};
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
static bool rbtrend_cmd_draw_fitted(
    rbtrend_command_list cmd_list,
    const char *who,
    rbtrend_texture_view source,
    const RBTREND_TEXEL_RECT *source_rect,
    crgpu_image_view target,
    CRGPU_FORMAT format,
    uint32_t width,
    uint32_t height,
    const RBTREND_TEXEL_RECT *target_rect,
    RBTREND_TEXTURE_SCALING scaling,
    CRGPU_FILTER filter,
    const float border[4]) {
	const rbtrend_texture_range range = rbtrend_texture_view_range(source);
	if (!rbtrend_cmd_range_written(cmd_list, range)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s: the source has nothing in it, from before the list or earlier in it", who);
		return false;
	}
	rbtrend_graphics_pipeline pipeline = rbtrend_builtin_present_pipeline(cmd_list->instance, format);
	if (!pipeline)
		return false;
	rbtrend_binding_set set = rbtrend_texture_view_present_set(source, filter);
	if (!set)
		return false;
	uint32_t source_width = 0, source_height = 0;
	rbtrend_view_size(source, &source_width, &source_height);
	const RBTREND_TEXEL_RECT from = rbtrend_texel_rect_resolved(source_rect, source_width, source_height);
	const RBTREND_TEXEL_RECT to   = rbtrend_texel_rect_resolved(target_rect, width, height);
	rbtrend_present_params params{};
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
	rbtrend_cmd_transition_range(cmd_list, range, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	// Every texel of the target rectangle is written: what the target held
	// is loaded only where the rectangle leaves some of it.
	const bool whole                 = to.x == 0 && to.y == 0 && to.width == width && to.height == height;
	const uint32_t region[4]         = {to.x, to.y, to.width, to.height};
	const rbtrend_binding_set sets[1] = {set};
	rbtrend_record_fullscreen_pass_region(cmd_list->cmd_list, pipeline, target, width, height, region,
	                                     whole ? CRGPU_LOAD_OP_DONT_CARE : CRGPU_LOAD_OP_LOAD, sets, 1, 0, &params, sizeof(params));
	return true;
}

/* DRAW_TEXTURE, outside a pass: the source drawn into the target, left in
 * COLOR_ATTACHMENT_OPTIMAL. */
static bool rbtrend_cmd_draw_texture(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p          = command._params._draw_texture;
	const rbtrend_texture2d target = p._target->texture._t2d;
	const uint32_t width   = rbtrend_mip_size(target->desc.width, p._target->desc.base_mip);
	const uint32_t height  = rbtrend_mip_size(target->desc.height, p._target->desc.base_mip);
	rbtrend_cmd_transition_view(cmd_list, p._target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	return rbtrend_cmd_draw_fitted(cmd_list, "DRAW_TEXTURE", p._source, &p._source_rect, p._target->image_view, target->desc.format, width,
	                              height, &p._target_rect, p._scaling, p._filter, p._border_color);
}

/* The view a draw-mode PRESENT_TEXTURE reads [texture] through: mip 0 of
 * layer 0, made the first time and kept by the texture
 * (rbtrend_texture2d_t::present_view). NULL, reported, if it can't be. */
static rbtrend_texture_view rbtrend_texture2d_present_view(rbtrend_texture2d texture) {
	if (texture->present_view)
		return texture->present_view;
	RBTREND_TEXTURE_VIEW_DESC desc{};
	desc.base        = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC);
	desc.type        = RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
	desc.shape       = RBTREND_TEXTURE_VIEW_SHAPE_2D;
	desc.aspect      = RBTREND_TEXTURE_VIEW_ASPECT_COLOR;
	desc.base_mip    = 0;
	desc.mip_count   = 1;
	desc.base_layer  = 0;
	desc.layer_count = 1;
	(void)rbtrend_texture_view_create_2d(texture, &desc, &texture->present_view, nullptr);
	if (!texture->present_view)
		rbtrend_report(texture->instance, RBT_RESULT_REND_GPU_FAILURE, "PRESENT_TEXTURE: the view the source is drawn through can't be created");
	return texture->present_view;
}

static bool rbtrend_cmd_present_texture(
    rbtrend_command_list cmd_list,
    const RBTREND_COMMAND &command) {
	const auto &p         = command._params._present_texture;
	rbtrend_swap_chain sc  = p._swap_chain;
	rbtrend_texture2d src  = p._source;
	if (!sc || !sc->swap_chain || !src) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE with a NULL source or swap chain");
		return false;
	}
	if (sc->acquired_image == RBTREND_SWAP_CHAIN_NO_IMAGE || sc->submitted) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE without a freshly acquired back buffer (call rbtrend_swap_chain_acquire first)");
		return false;
	}
	if (cmd_list->present_swap_chain) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "more than one PRESENT_TEXTURE in one command list");
		return false;
	}

	crgpu_image_view back_view = crgpu_swap_chain_get_image_view(sc->swap_chain, sc->acquired_image);
	crgpu_image_view_desc back_view_desc{};
	back_view_desc.base.type = CR_STRUCT_TYPE_GPU_IMAGE_VIEW_DESC;
	if (!back_view || CR_FAILED(crgpu_image_view_get_desc(back_view, &back_view_desc)) || !back_view_desc.parent_image) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE couldn't get the acquired back buffer");
		return false;
	}
	// The real back-buffer size, which the surface can override on Vulkan -
	// not necessarily the size the swap chain was requested at.
	crgpu_swap_chain_desc actual{};
	actual.base.type = CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	if (CR_FAILED(crgpu_swap_chain_get_desc(sc->swap_chain, &actual)) || !actual.width || !actual.height) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE couldn't get the back buffer size");
		return false;
	}

	// The draw modes: the source fitted into the back buffer, any size and
	// format of it taken, the back buffers' written.
	if (p._scaling != RBTREND_TEXTURE_SCALING_NONE) {
		if (!(src->desc.usage & RBTREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE with a _scaling that draws, and a source without RBTREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE");
			return false;
		}
		rbtrend_texture_view view = rbtrend_texture2d_present_view(src);
		if (!view)
			return false;
		rbtrend_cmd_use_handle(cmd_list, view);
		crgpu_command_list_image_barrier_view(cmd_list->cmd_list, back_view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		if (!rbtrend_cmd_draw_fitted(cmd_list, "PRESENT_TEXTURE", view, NULL, back_view, actual.format, actual.width, actual.height, NULL,
		                            p._scaling, p._filter, p._border_color))
			return false;
		crgpu_command_list_image_barrier_view(cmd_list->cmd_list, back_view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, CRGPU_IMAGE_LAYOUT_PRESENT_SRC);
		cmd_list->present_swap_chain  = sc;
		cmd_list->present_image_index = sc->acquired_image;
		cmd_list->present_generation  = sc->generation;
		return true;
	}

	// The copy converts nothing on D3D12 and Metal, so a source of another
	// format would be undefined there; and a smaller one would leave part of
	// what is shown undefined, from a back buffer moved out of UNDEFINED.
	if (src->desc.format != actual.format) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE with a source of format %u, and the back buffers are of format %u", (unsigned)src->desc.format, (unsigned)actual.format);
		return false;
	}
	if (src->desc.width < actual.width || src->desc.height < actual.height) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE with a %ux%u source, smaller than the %ux%u back buffer (rbtrend_swap_chain_get_back_buffer_size)", src->desc.width, src->desc.height, actual.width, actual.height);
		return false;
	}

	crgpu_command_list cmd = cmd_list->cmd_list;
	// What is presented is mip 0 of layer 0, and only that is moved.
	rbtrend_texture_range presented{};
	presented.image       = src->image;
	presented.tracking    = &src->tracking;
	presented.base_mip    = 0;
	presented.mip_count   = 1;
	presented.base_layer  = 0;
	presented.layer_count = 1;
	rbtrend_cmd_transition_range(cmd_list, presented, CRGPU_IMAGE_LAYOUT_TRANSFER_SRC, false);
	crgpu_command_list_image_barrier_view(cmd, back_view, CRGPU_IMAGE_LAYOUT_UNDEFINED, CRGPU_IMAGE_LAYOUT_TRANSFER_DST);

	// The source covers the back buffer: the back buffer's size is copied.
	const uint32_t width  = actual.width;
	const uint32_t height = actual.height;
	crgpu_image_blit_desc blit{};
	blit.src_array_layer_count = 1;
	blit.src_x1                = width;
	blit.src_y1                = height;
	blit.src_z1                = 1;
	blit.dst_array_layer_count = 1;
	blit.dst_x1                = width;
	blit.dst_y1                = height;
	blit.dst_z1                = 1;
	blit.filter                = CRGPU_FILTER_NEAREST;
	crgpu_command_list_blit_image(cmd, src->image, back_view_desc.parent_image, &blit);

	crgpu_command_list_image_barrier_view(cmd, back_view, CRGPU_IMAGE_LAYOUT_TRANSFER_DST, CRGPU_IMAGE_LAYOUT_PRESENT_SRC);

	cmd_list->present_swap_chain  = sc;
	cmd_list->present_image_index = sc->acquired_image;
	cmd_list->present_generation  = sc->generation;
	return true;
}

static bool rbtrend_render_view_valid(rbtrend_texture_view view) {
	if (!view)
		return false;
	return view->image_view && view->dimension == RBTREND_TEXTURE_DIMENSION_2D && view->texture._t2d;
}

static bool rbtrend_begin_rendering_valid(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p = command._params._begin_rendering;
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		if (!rbtrend_render_view_valid(p._color_targets[i].view)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING color target %u is not a view of a 2D texture", i);
			return false;
		}
		/* crgpu_color_attachment_desc has no resolve attachment yet, so a
		 * resolve can't be honored — fail rather than silently skip it. */
		if (p._color_targets[i].resolve_view) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING color target %u has a resolve_view, which Carrottus GPU doesn't support yet", i);
			return false;
		}
	}
	if (p._depth_target.view && !rbtrend_render_view_valid(p._depth_target.view)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING depth target is not a view of a 2D texture");
		return false;
	}
	if (p._color_target_count == 0 && !p._depth_target.view) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING has no color or depth target");
		return false;
	}
	// The targets of one pass are all one layer, or all the same number of
	// layers of 2D array views: the pass is layered or it isn't.
	rbtrend_texture_view first = p._color_target_count > 0 ? p._color_targets[0].view : p._depth_target.view;
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		if (p._color_targets[i].view->desc.shape != first->desc.shape) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING color target %u is not the shape of the pass's first target", i);
			return false;
		}
		if (p._color_targets[i].view->desc.layer_count != first->desc.layer_count) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING color target %u covers %u layers, and the pass's first target covers %u", i, p._color_targets[i].view->desc.layer_count, first->desc.layer_count);
			return false;
		}
	}
	if (p._depth_target.view && p._depth_target.view->desc.shape != first->desc.shape) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING depth target is not the shape of the pass's first target");
		return false;
	}
	if (p._depth_target.view && p._depth_target.view->desc.layer_count != first->desc.layer_count) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING depth target covers %u layers, and the pass's first target covers %u", p._depth_target.view->desc.layer_count, first->desc.layer_count);
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
static bool rbtrend_pipeline_matches_pass(
    rbtrend_command_list cmd_list,
    rbtrend_graphics_pipeline pipeline,
    const RBTREND_COMMAND &begin) {
	const auto &p                           = begin._params._begin_rendering;
	const RBTREND_GRAPHICS_PIPELINE_DESC &pd = pipeline->desc;
	if (pd.color_attachment_count != p._color_target_count) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE: the pipeline renders to %u color targets, and the pass has %u", pd.color_attachment_count, p._color_target_count);
		return false;
	}
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		const CRGPU_FORMAT format = p._color_targets[i].view->texture._t2d->desc.format;
		if (pd.color_attachment_formats[i] != format) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE: the pipeline renders format %u to color target %u, which is of format %u", (unsigned)pd.color_attachment_formats[i], i, (unsigned)format);
			return false;
		}
	}
	const CRGPU_FORMAT depth = p._depth_target.view ? p._depth_target.view->texture._t2d->desc.format : CRGPU_FORMAT_UNKNOWN;
	if (pd.depth_format != depth) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE: the pipeline's depth format is %u, and the pass's depth target is of format %u", (unsigned)pd.depth_format, (unsigned)depth);
		return false;
	}
	return true;
}

/* Before a BEGIN_RENDERING [begin] that names a timer: resets the two
 * timestamps of its slot in this frame's set, outside the pass, and notes
 * the slot for submit to mark. False, recording nothing, without memory. */
static bool rbtrend_cmd_begin_timed(
    rbtrend_command_list cmd_list,
    const RBTREND_COMMAND &begin) {
	const auto &p = begin._params._begin_rendering;
	if (!p._timer)
		return true;
	try {
		cmd_list->timer_uses.push_back({p._timer, p._timer_slot});
	} catch (const std::bad_alloc &) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "compile: out of memory");
		return false;
	}
	crgpu_command_list_reset_timestamps(cmd_list->cmd_list, p._timer->sets[cmd_list->compiled_frame], p._timer_slot * 2, 2);
	return true;
}

/* After the END_RENDERING of the pass [begin] opened: makes its slot's two
 * timestamps readable, outside the pass. */
static void rbtrend_cmd_end_timed(
    rbtrend_command_list cmd_list,
    const RBTREND_COMMAND &begin) {
	const auto &p = begin._params._begin_rendering;
	if (!p._timer)
		return;
	crgpu_command_list_resolve_timestamps(cmd_list->cmd_list, p._timer->sets[cmd_list->compiled_frame], p._timer_slot * 2, 2);
}

/* Opens an explicit BEGIN_RENDERING pass: transitions every attachment into
 * its attachment layout (barriers only happen outside a pass), then begins
 * dynamic rendering with each attachment's own load/store ops and clear
 * values. Assumes rbtrend_begin_rendering_valid passed. */
static void rbtrend_cmd_begin_rendering(
    rbtrend_command_list cmd_list,
    const RBTREND_COMMAND &command) {
	const auto &p            = command._params._begin_rendering;
	crgpu_command_list cmd = cmd_list->cmd_list;

	crgpu_rendering_begin_desc desc{};
	desc.base.type = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	desc.color_attachment_count = p._color_target_count;
	desc.x                      = p._x;
	desc.y                      = p._y;
	desc.width                  = p._width;
	desc.height                 = p._height;
	// The slot's start at 2 * slot and its end after it (rbtrend_gpu_timer_t).
	if (p._timer) {
		desc.timestamp_set         = p._timer->sets[cmd_list->compiled_frame];
		desc.timestamp_begin_index = p._timer_slot * 2;
		desc.timestamp_end_index   = p._timer_slot * 2 + 1;
	}

	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		const RBTREND_COLOR_TARGET &target = p._color_targets[i];
		rbtrend_cmd_transition_view(cmd_list, target.view, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		rbtrend_cmd_use_handle(cmd_list, target.view);

		crgpu_color_attachment_desc &color = desc.color_attachments[i];
		color.image_view                     = target.view->image_view;
		color.load_op                        = target.load_op;
		color.store_op                       = target.store_op;
		memcpy(color.clear_color, target.clear_color, sizeof(color.clear_color));
	}

	const RBTREND_DEPTH_TARGET &depth = p._depth_target;
	if (depth.view) {
		rbtrend_cmd_transition_view(cmd_list, depth.view, CRGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		rbtrend_cmd_use_handle(cmd_list, depth.view);

		desc.depth_attachment.image_view       = depth.view->image_view;
		desc.depth_attachment.depth_load_op    = depth.depth_load_op;
		desc.depth_attachment.depth_store_op   = depth.depth_store_op;
		desc.depth_attachment.stencil_load_op  = depth.stencil_load_op;
		desc.depth_attachment.stencil_store_op = depth.stencil_store_op;
		desc.depth_attachment.clear_depth      = depth.clear_depth;
		desc.depth_attachment.clear_stencil    = depth.clear_stencil;
	}

	// A pass over 2D array views is layered, over as many layers as each
	// covers (rbtrend_begin_rendering_valid made them agree); one over 2D
	// views is not, which Carrottus GPU spells 0. An array view of one layer is
	// still a layered pass.
	rbtrend_texture_view sizing = p._color_target_count > 0 ? p._color_targets[0].view : depth.view;
	if (sizing->desc.shape == RBTREND_TEXTURE_VIEW_SHAPE_2D_ARRAY)
		desc.layer_count = sizing->desc.layer_count;

	if (desc.width == 0 || desc.height == 0) {
		// The size of the mip the view covers, not of the texture's mip 0.
		desc.width                 = rbtrend_mip_size(sizing->texture._t2d->desc.width, sizing->desc.base_mip);
		desc.height                = rbtrend_mip_size(sizing->texture._t2d->desc.height, sizing->desc.base_mip);
	}

	crgpu_command_list_begin_rendering(cmd, &desc);
}

/* Adds the storage buffers [set], at [slot], holds for frame [frame] to
 * [stores], each once, with whether [writes] (the pipeline's) writes the
 * binding each is in. */
static void rbtrend_storage_stores_add_set(
    eq::array<rbtrend_pass_store> &stores,
    rbtrend_binding_set set,
    uint32_t frame,
    uint32_t slot,
    const std::vector<rbtrend_storage_write> &writes) {
	const uint32_t frame_slot                               = rbtrend_binding_set_frame_slot(set, frame);
	const rbtrend_binding_set_t::storage_store *frame_stores = set->storage_stores + (size_t)frame_slot * set->storage_store_capacity;
	for (uint32_t i = 0; i < set->storage_store_counts[frame_slot]; ++i) {
		const bool written = rbtrend_storage_writes_hold(writes, slot, frame_stores[i].binding);
		bool known         = false;
		for (rbtrend_pass_store &listed : stores) {
			if (listed.store != frame_stores[i].store)
				continue;
			listed.written = listed.written || written;
			known          = true;
		}
		if (!known)
			rbtrend_push(stores, rbtrend_pass_store{frame_stores[i].store, written});
	}
}

/* Adds what [set], at [slot], has its views cover to [images], each storage
 * image with whether [writes] (the pipeline's) writes its binding. False if
 * any mip of any layer is already there in another layout: sampled through
 * one slot and a storage image through another. Two views of one texture
 * that don't share a mip of a layer are no conflict, which is what lets one
 * mip be read while another is written. */
static bool rbtrend_pass_images_add_set(
    rbtrend_command_list cmd_list,
    eq::array<rbtrend_pass_image> &images,
    rbtrend_binding_set set,
    uint32_t slot,
    const std::vector<rbtrend_storage_write> &writes) {
	for (uint32_t i = 0; i < set->image_use_count; ++i) {
		const auto &use                  = set->image_uses[i];
		const rbtrend_texture_range range = rbtrend_texture_view_range(use.view);
		const bool written = use.layout == CRGPU_IMAGE_LAYOUT_GENERAL && rbtrend_storage_writes_hold(writes, slot, use.binding);

		bool known = false;
		for (rbtrend_pass_image &pass_image : images) {
			if (!rbtrend_texture_ranges_overlap(pass_image.range, range))
				continue;
			if (pass_image.layout != use.layout) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "a mip of a texture is in a sampled slot and a storage image slot of the binding sets of one pass or dispatch");
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
			rbtrend_push(images, rbtrend_pass_image{range, use.layout, written});
	}
	return true;
}

/* The argument buffer of the indirect draw [command], for a list compiled in
 * frame [frame]: that frame's copy of its _buffer. rbtrend_command_list_send
 * refused a command with none. */
static rbtrend_buffer_part *rbtrend_cmd_indirect_arguments(
    const RBTREND_COMMAND &command,
    uint32_t frame) {
	return rbtrend_buffer_part_of(command._params._draw_indirect._buffer, frame);
}

/* Moves a multi-state [store] into [state], tracking it in [cmd_list]'s
 * buffer_states. The first time the list uses a store it is in its
 * use_state, which rbtrend_command_list_submit sees to. Recorded outside any
 * pass. */
static void rbtrend_cmd_transition_store(
    rbtrend_command_list cmd_list,
    rbtrend_buffer_store *store,
    CRGPU_RESOURCE_STATE state) {
	rbtrend_command_list_t::buffer_state *tracked = nullptr;
	for (auto &known : cmd_list->buffer_states)
		if (known.store == store)
			tracked = &known;
	if (!tracked) {
		cmd_list->buffer_states.push_back({store, store->use_state});
		tracked = &cmd_list->buffer_states.back();
	}
	if (tracked->state == state)
		return;
	crgpu_buffer_barrier barrier{{CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, tracked->state, state};
	crgpu_command_list_pipeline_barrier(cmd_list->cmd_list, &barrier, 1, nullptr, 0);
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
static rbtrend_command_list_t::storage_hazard &rbtrend_cmd_store_hazard(
    rbtrend_command_list cmd_list,
    rbtrend_buffer_store *store) {
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
 * The ordering is Carrottus GPU's barrier that moves nothing: from GENERAL to
 * GENERAL over the range for an image, from UNORDERED_ACCESS to
 * UNORDERED_ACCESS for a buffer (crgpu.h). It is needed before a use if a
 * shader may have written the resource since its last barrier, or if this
 * use writes it and a shader may have used it since: what each pipeline
 * writes is its storage_writes, and the list's layout_uses and
 * storage_hazards keep what happened since each barrier. The first use in a
 * list always gets one, since a submission before it may have written the
 * resource. Recorded outside any pass. Throws std::bad_alloc. */
static void rbtrend_cmd_ready_set_resources(
    rbtrend_command_list cmd_list,
    const eq::array<rbtrend_pass_image> &images,
    const eq::array<rbtrend_pass_store> &stores) {
	for (const rbtrend_pass_image &pass_image : images) {
		const rbtrend_texture_range &range = pass_image.range;
		if (pass_image.layout != CRGPU_IMAGE_LAYOUT_GENERAL) {
			// Sampled: a barrier only to move it.
			rbtrend_cmd_transition_range(cmd_list, range, pass_image.layout, true);
			continue;
		}
		// GENERAL is the layout of a storage image slot. The barrier is over
		// the range, so the texture's other mips are left where they are.
		bool order = false;
		for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
			for (uint32_t mip = 0; mip < range.mip_count; ++mip) {
				const auto &use = cmd_list->layout_uses[rbtrend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer)];
				if (use.storage_written || (pass_image.written && use.storage_accessed))
					order = true;
			}
		}
		// With [order], one from GENERAL to GENERAL where it is there
		// already; without, only a move into GENERAL where it is not.
		rbtrend_cmd_transition_range(cmd_list, range, CRGPU_IMAGE_LAYOUT_GENERAL, !order);
		for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
			for (uint32_t mip = 0; mip < range.mip_count; ++mip) {
				auto &use            = cmd_list->layout_uses[rbtrend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer)];
				use.storage_accessed = true;
				use.storage_written  = use.storage_written || pass_image.written;
			}
		}
	}
	eq::array<crgpu_buffer_barrier> &buffer_barriers = cmd_list->set_barriers;
	buffer_barriers.clear();
	for (const rbtrend_pass_store &pass_store : stores) {
		rbtrend_buffer_store *store = pass_store.store;
		// One that was last an indirect argument buffer comes back to the
		// state a storage slot uses it in.
		if (store->multi_state)
			rbtrend_cmd_transition_store(cmd_list, store, store->use_state);
		rbtrend_command_list_t::storage_hazard &hazard = rbtrend_cmd_store_hazard(cmd_list, store);
		if (hazard.written || (pass_store.written && hazard.accessed)) {
			rbtrend_push(buffer_barriers, crgpu_buffer_barrier{{CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, store->buffer, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS});
			hazard.written = false;
		}
		hazard.accessed = true;
		hazard.written  = hazard.written || pass_store.written;
	}
	if (buffer_barriers.empty())
		return;
	crgpu_command_list_pipeline_barrier(cmd_list->cmd_list, buffer_barriers.data, (uint32_t)buffer_barriers.size(), nullptr, 0);
}

/* Before the BEGIN_RENDERING at [begin_index] opens its pass: moves every
 * texture of the pass's binding sets into the layout its slot reads it in,
 * since a barrier can't be issued once the pass is open. The pass's sets are
 * the ones a draw in it reads: the pass is walked as the compile will walk
 * it, from the sets in [slots] now, and at each draw the set in every slot
 * the pipeline bound then declares is taken. A set that is only left in a
 * slot, with no draw reading that slot, isn't the pass's.
 * Assumes rbtrend_begin_rendering_valid passed. */
static bool rbtrend_cmd_prepare_pass_images(
    rbtrend_command_list cmd_list,
    size_t begin_index,
    const rbtrend_binding_set *slots) {
	eq::array<rbtrend_pass_image> &images = cmd_list->pass_images;
	eq::array<rbtrend_pass_store> &stores = cmd_list->pass_stores;
	/* The argument buffers of the pass's indirect draws, each once. */
	eq::array<rbtrend_buffer_store *> &argument_stores = cmd_list->pass_arguments;
	images.clear();
	stores.clear();
	argument_stores.clear();
	rbtrend_binding_set pass_slots[RBTREND_MAX_BINDING_LAYOUTS];
	for (uint32_t slot = 0; slot < RBTREND_MAX_BINDING_LAYOUTS; ++slot)
		pass_slots[slot] = slots[slot];
	rbtrend_graphics_pipeline pass_pipeline = nullptr;
	bool pass_ended                        = false;
	for (size_t i = begin_index + 1; i < cmd_list->commands.size() && !pass_ended; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		switch (command._type) {
		case RBTREND_COMMAND_END_RENDERING:
			pass_ended = true;
			break;
		case RBTREND_COMMAND_SET_GRAPHICS_PIPELINE:
			// A NULL pipeline is refused when the command itself is reached.
			pass_pipeline = command._params._set_shader_pipeline._pipeline;
			break;
		case RBTREND_COMMAND_SET_BINDING_SET:
			// As is a slot out of range. A NULL set is refused there too, and
			// until then leaves the slot empty here.
			if (command._params._set_binding_set._slot < RBTREND_MAX_BINDING_LAYOUTS)
				pass_slots[command._params._set_binding_set._slot] = command._params._set_binding_set._set;
			break;
		case RBTREND_COMMAND_SET_MATERIAL:
			// The material's set was built before the compile began, and its
			// slot was checked when the command was sent.
			pass_slots[command._params._set_material._slot] = command._params._set_material._material->set;
			break;
		case RBTREND_COMMAND_DRAW:
		case RBTREND_COMMAND_DRAW_INDEXED:
		case RBTREND_COMMAND_DRAW_INSTANCED:
		case RBTREND_COMMAND_DRAW_INSTANCED_INDEXED:
		case RBTREND_COMMAND_DRAW_INDIRECT:
		case RBTREND_COMMAND_DRAW_INDEXED_INDIRECT:
		case RBTREND_COMMAND_DISPATCH_MESH:
			if (command._type == RBTREND_COMMAND_DRAW_INDIRECT || command._type == RBTREND_COMMAND_DRAW_INDEXED_INDIRECT) {
				rbtrend_buffer_store *arguments = &rbtrend_cmd_indirect_arguments(command, cmd_list->compiled_frame)->store;
				bool known                     = false;
				for (rbtrend_buffer_store *store : argument_stores)
					if (store == arguments)
						known = true;
				if (!known)
					rbtrend_push(argument_stores, arguments);
			}
			// A draw with no pipeline, or with an empty slot, fails the
			// compile when it is reached.
			if (!pass_pipeline)
				break;
			for (uint32_t slot = 0; slot < pass_pipeline->desc._binding_layout_count; ++slot) {
				if (!pass_slots[slot])
					continue;
				if (!rbtrend_pass_images_add_set(cmd_list, images, pass_slots[slot], slot, pass_pipeline->storage_writes))
					return false;
				rbtrend_storage_stores_add_set(stores, pass_slots[slot], cmd_list->compiled_frame, slot, pass_pipeline->storage_writes);
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
	for (const rbtrend_pass_image &pass_image : images) {
		for (uint32_t i = 0; i < p._color_target_count; ++i) {
			if (rbtrend_texture_ranges_overlap(rbtrend_texture_view_range(p._color_targets[i].view), pass_image.range)) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "color target %u of a pass is also in one of the pass's binding sets", i);
				return false;
			}
		}
		if (p._depth_target.view && rbtrend_texture_ranges_overlap(rbtrend_texture_view_range(p._depth_target.view), pass_image.range)) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "the depth target of a pass is also in one of the pass's binding sets");
			return false;
		}
	}

	// A buffer is in one state for the whole pass: it can't be read as draw
	// arguments and be in a storage slot the pass's draws read.
	for (rbtrend_buffer_store *arguments : argument_stores) {
		for (const rbtrend_pass_store &pass_store : stores) {
			if (pass_store.store == arguments) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "the argument buffer of an indirect draw is also in one of the pass's binding sets");
				return false;
			}
		}
	}

	rbtrend_cmd_ready_set_resources(cmd_list, images, stores);
	// After the sets, so a buffer a set brought back to a storage slot's
	// state in an earlier pass ends this one's preparation as arguments.
	for (rbtrend_buffer_store *arguments : argument_stores)
		rbtrend_cmd_transition_store(cmd_list, arguments, CRGPU_RESOURCE_STATE_INDIRECT_ARGUMENT);
	return true;
}

/* Before a dispatch: checks the set in each slot [pipeline] declares, makes
 * what those sets hold ready (rbtrend_cmd_ready_set_resources) and binds
 * each set that isn't the one already bound there. There is no pass to move
 * the textures ahead of, so this is done right at the dispatch. False if a
 * slot holds no set or one of another layout, or a texture is in a sampled
 * slot and a storage image slot of these sets. */
static bool rbtrend_cmd_prepare_dispatch(
    rbtrend_command_list cmd_list,
    rbtrend_compute_pipeline pipeline,
    const rbtrend_binding_set *slots,
    rbtrend_binding_set *applied) {
	eq::array<rbtrend_pass_image> &images = cmd_list->pass_images;
	eq::array<rbtrend_pass_store> &stores = cmd_list->pass_stores;
	images.clear();
	stores.clear();
	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		rbtrend_binding_set set = slots[slot];
		if (!set) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "dispatch with a pipeline that declares set %u, but no SET_BINDING_SET has put a set there", slot);
			return false;
		}
		if (!rbtrend_binding_set_fits(set, pipeline->desc._binding_layouts[slot])) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "dispatch with a set at slot %u that isn't of the layout the pipeline declares there", slot);
			return false;
		}
		if (!rbtrend_pass_images_add_set(cmd_list, images, set, slot, pipeline->storage_writes))
			return false;
		rbtrend_storage_stores_add_set(stores, set, cmd_list->compiled_frame, slot, pipeline->storage_writes);
	}
	rbtrend_cmd_ready_set_resources(cmd_list, images, stores);

	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		rbtrend_binding_set set = slots[slot];
		if (applied[slot] == set)
			continue;
		// Checked as it is bound for this pipeline: the views the set holds
		// against the shapes the shader declares.
		if (!rbtrend_binding_set_matches_images(set, slot, pipeline->images, "dispatch"))
			return false;
		crgpu_command_list_bind_compute_descriptor_sets(
		    cmd_list->cmd_list, pipeline->pipeline, slot, &set->sets[rbtrend_binding_set_frame_slot(set, cmd_list->compiled_frame)], 1);
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
struct rbtrend_cmd_vertex_slot {
	/* The copy of the buffer of the frame the list is compiled in. */
	rbtrend_buffer_part *buffer;
	bool has_span;
	RBTREND_TRANSIENT_SPAN span;
	crgpu_buffer_view span_view;
	uint32_t span_view_stride;
};

/* The elements [draw] reads from a binding that steps per instance (if
 * [per_instance]) or per vertex: the first and how many, false where the
 * compile can't know (an indexed draw's vertices, an indirect draw's
 * anything). */
static bool rbtrend_cmd_draw_reads(const RBTREND_COMMAND &draw, bool per_instance, uint64_t *out_first, uint64_t *out_count) {
	switch (draw._type) {
	case RBTREND_COMMAND_DRAW:
		*out_first = per_instance ? 0 : draw._params._draw._start_vertex_location;
		*out_count = per_instance ? 1 : draw._params._draw._vertex_count;
		return true;
	case RBTREND_COMMAND_DRAW_INDEXED:
		// Instance 0 of a draw that is not instanced.
		*out_first = 0;
		*out_count = 1;
		return per_instance;
	case RBTREND_COMMAND_DRAW_INSTANCED:
		*out_first = per_instance ? draw._params._draw_instanced._start_instance_location : draw._params._draw_instanced._start_vertex_location;
		*out_count = per_instance ? draw._params._draw_instanced._instance_count : draw._params._draw_instanced._vertex_count_per_instance;
		return true;
	case RBTREND_COMMAND_DRAW_INSTANCED_INDEXED:
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
 * the slot has something else. A Carrottus GPU vertex view carries its stride, so
 * the view is the buffer's for that stride (rbtrend_buffer_vertex_view) or
 * the span's. False, reported, if not. */
static bool rbtrend_cmd_apply_vertex_buffers(
    rbtrend_command_list cmd_list,
    rbtrend_graphics_pipeline pipeline,
    rbtrend_cmd_vertex_slot *slots,
    crgpu_buffer_view *applied,
    const RBTREND_COMMAND &draw) {
	for (uint32_t slot = 0; slot < pipeline->desc._vertex_binding_count; ++slot) {
		rbtrend_cmd_vertex_slot &held = slots[slot];
		if (!held.buffer && !held.has_span) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "draw with a pipeline that reads vertex buffer slot %u, and nothing has been put there", slot);
			return false;
		}
		const uint32_t stride  = pipeline->vertex_strides[slot];
		const uint64_t size    = held.buffer ? held.buffer->store.size : held.span.size;
		uint64_t first         = 0;
		uint64_t count         = 0;
		const bool per_instance = pipeline->desc._vertex_bindings[slot]._per_instance;
		if (rbtrend_cmd_draw_reads(draw, per_instance, &first, &count) && count > 0) {
			// The last element read ends where its furthest attribute does.
			const uint64_t end = (first + count - 1) * stride + pipeline->vertex_extents[slot];
			if (end > size) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "draw reads %s %llu to %llu at vertex buffer slot %u, %llu bytes at a stride of %u, and the %s holds %llu", per_instance ? "instances" : "vertices", (unsigned long long)first, (unsigned long long)(first + count - 1), slot, (unsigned long long)end, stride, held.buffer ? "buffer" : "span", (unsigned long long)size);
				return false;
			}
		}
		crgpu_buffer_view view = nullptr;
		CR_RESULT sr            = CR_SUCCESS;
		if (held.buffer) {
			sr = rbtrend_buffer_vertex_view(held.buffer, stride, &view);
		} else if (held.span_view && held.span_view_stride == stride) {
			view = held.span_view;
		} else {
			sr = rbtrend_transient_span_view(cmd_list->instance, &held.span, stride, &view);
			if (!CR_FAILED(sr)) {
				held.span_view        = view;
				held.span_view_stride = stride;
			}
		}
		if (CR_FAILED(sr)) {
			rbtrend_cmd_report(cmd_list, rbtrend_result_from_cr(sr), "draw: a view of vertex buffer slot %u at a stride of %u can't be created: %s", slot, stride, cr_result_str(sr));
			return false;
		}
		if (applied[slot] == view)
			continue;
		crgpu_command_list_set_vertex_buffers(cmd_list->cmd_list, slot, 1, &view);
		applied[slot] = view;
	}
	return true;
}

static bool rbtrend_cmd_apply_binding_sets(
    rbtrend_command_list cmd_list,
    crgpu_command_list cmd,
    rbtrend_graphics_pipeline pipeline,
    const rbtrend_binding_set *slots,
    rbtrend_binding_set *applied,
    uint32_t frame) {
	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		rbtrend_binding_set set = slots[slot];
		if (!set) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "draw with a pipeline that declares set %u, but no SET_BINDING_SET has put a set there", slot);
			return false;
		}
		if (!rbtrend_binding_set_fits(set, pipeline->desc._binding_layouts[slot])) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "draw with a set at slot %u that isn't of the layout the pipeline declares there", slot);
			return false;
		}
		if (applied[slot] == set)
			continue;
		if (!rbtrend_binding_set_matches_images(set, slot, pipeline->images, "draw"))
			return false;
		crgpu_command_list_bind_graphics_descriptor_sets(cmd, pipeline->pipeline, slot, &set->sets[rbtrend_binding_set_frame_slot(set, frame)], 1);
		applied[slot] = set;
	}
	return true;
}

/* SET_BINDING_SET, and SET_MATERIAL with its material's set: puts [set] in
 * [slot] of [slot_sets] and takes the storage buffers its frame's
 * descriptor set holds. False if [set] is NULL or [slot] is out of range. */
static bool rbtrend_cmd_put_binding_set(
    rbtrend_command_list cmd_list,
    uint32_t slot,
    rbtrend_binding_set set,
    rbtrend_binding_set *slot_sets) {
	if (!set) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_BINDING_SET with a NULL set");
		return false;
	}
	if (slot >= RBTREND_MAX_BINDING_LAYOUTS) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_BINDING_SET with _slot %u, at or above RBTREND_MAX_BINDING_LAYOUTS", slot);
		return false;
	}
	slot_sets[slot] = set;
	// The set, and what its descriptors point at: a destroy of any of them,
	// or an update of the set, refuses the list at submit.
	rbtrend_cmd_use_handle(cmd_list, set);
	for (uint32_t i = 0; i < set->entry_count; ++i) {
		const RBTREND_BINDING_SET_ENTRY &entry = set->entries[i];
		switch (entry._type) {
		case CRGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
			rbtrend_cmd_use_handle(cmd_list, entry._resource._uniform._buffer);
			break;
		case CRGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER:
			// Its store is taken below, which a destroy looks for.
			break;
		default:
			rbtrend_cmd_use_handle(cmd_list, entry._resource._image._view);
			rbtrend_cmd_use_handle(cmd_list, entry._resource._image._sampler);
			break;
		}
	}
	// The stores of the descriptor set this frame binds. Taken whether or not
	// a draw reads the slot: copying a buffer nothing reads yet is harmless.
	const uint32_t frame_slot                               = rbtrend_binding_set_frame_slot(set, cmd_list->compiled_frame);
	const rbtrend_binding_set_t::storage_store *frame_stores = set->storage_stores + (size_t)frame_slot * set->storage_store_capacity;
	for (uint32_t i = 0; i < set->storage_store_counts[frame_slot]; ++i)
		rbtrend_cmd_use_store(cmd_list, frame_stores[i].store);
	return true;
}

/* GENERATE_MIPS, outside a pass: for each layer, the base level into
 * SHADER_READ_ONLY (nothing if a read left it there), then each level below
 * rendered from the one above and moved to SHADER_READ_ONLY in turn, all
 * through the list's own layout tracking. False, reported, if the base level
 * of a layer has nothing in it as the list reaches the command, or the
 * texture's chain or pipeline can't be made. */
static bool rbtrend_cmd_generate_mips(
    rbtrend_command_list cmd_list,
    rbtrend_texture2d texture,
    uint32_t base_mip,
    CRGPU_FILTER filter) {
	// Checked again: a resize since the command was sent may have changed
	// the texture's levels.
	RBTREND_ERROR error{};
	if (rbtrend_generate_mips_valid(texture, base_mip, filter, &error) != RBT_RESULT_OK) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "GENERATE_MIPS: %s", error.message);
		return false;
	}
	const uint32_t layer_count = texture->desc.array_layers;
	const uint32_t last_mip    = texture->desc.mip_levels - 1;
	rbtrend_texture_range one{};
	one.image       = texture->image;
	one.tracking    = &texture->tracking;
	one.mip_count   = 1;
	one.layer_count = 1;
	for (uint32_t layer = 0; layer < layer_count; ++layer) {
		const size_t index = rbtrend_cmd_layout_use(cmd_list, one, base_mip, layer);
		if (cmd_list->layout_uses[index].final_layout == CRGPU_IMAGE_LAYOUT_UNDEFINED) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "GENERATE_MIPS: level %u of layer %u has nothing in it, from before the list or earlier in it", base_mip, layer);
			return false;
		}
	}
	if (base_mip == last_mip)
		return true;

	const rbtrend_mip_chain *chain     = nullptr;
	rbtrend_graphics_pipeline pipeline = nullptr;
	if (rbtrend_texture2d_mip_chain(texture, base_mip, filter, &chain, &pipeline, &error) != RBT_RESULT_OK) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "GENERATE_MIPS: %s", error.message);
		return false;
	}
	const uint32_t levels = last_mip - base_mip;
	for (uint32_t layer = 0; layer < layer_count; ++layer) {
		one.base_layer = layer;
		one.base_mip   = base_mip;
		rbtrend_cmd_transition_range(cmd_list, one, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
		for (uint32_t i = 0; i < levels; ++i) {
			const uint32_t mip = base_mip + i + 1;
			one.base_mip       = mip;
			// A barrier whether or not it is there already, as for a pass's
			// targets.
			rbtrend_cmd_transition_range(cmd_list, one, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
			rbtrend_record_mip_pass(cmd_list->cmd_list, pipeline, chain->steps[(size_t)layer * levels + i], chain->filter, rbtrend_mip_size(texture->desc.width, mip),
			                       rbtrend_mip_size(texture->desc.height, mip));
			rbtrend_cmd_transition_range(cmd_list, one, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, false);
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
static bool rbtrend_cmd_bloom(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p            = command._params._bloom;
	rbtrend_instance instance = cmd_list->instance;
	crgpu_command_list cmd = cmd_list->cmd_list;

	// Checked again: a resize since the command was sent may have changed
	// the source's size.
	if (p._levels > rbtrend_bloom_max_levels(p._source)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM: the source's size no longer allows _levels %u", p._levels);
		return false;
	}
	const rbtrend_texture_range source = rbtrend_texture_view_range(p._source);
	if (!rbtrend_cmd_range_written(cmd_list, source)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM: the source has nothing in it, from before the list or earlier in it");
		return false;
	}
	if (p._mask && !rbtrend_cmd_range_written(cmd_list, rbtrend_texture_view_range(p._mask))) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM: the _mask has nothing in it, from before the list or earlier in it");
		return false;
	}
	if (p._lens_dirt && !rbtrend_cmd_range_written(cmd_list, rbtrend_texture_view_range(p._lens_dirt))) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM: the _lens_dirt has nothing in it, from before the list or earlier in it");
		return false;
	}

	// The chain: level 0 half the source's first mip.
	const CRGPU_FORMAT chain_format  = CRGPU_FORMAT_R16G16B16A16_FLOAT;
	const CRGPU_FORMAT target_format = p._target->texture._t2d->desc.format;
	const rbtrend_texture2d source_tex  = p._source->texture._t2d;
	const uint32_t chain_width         = rbtrend_mip_size(rbtrend_mip_size(source_tex->desc.width, p._source->desc.base_mip), 1);
	const uint32_t chain_height        = rbtrend_mip_size(rbtrend_mip_size(source_tex->desc.height, p._source->desc.base_mip), 1);
	const uint32_t levels              = p._levels;

	rbtrend_graphics_pipeline prefilter  = rbtrend_builtin_bloom_pipeline(instance, RBTREND_BLOOM_STAGE_PREFILTER, chain_format);
	rbtrend_graphics_pipeline downsample = rbtrend_builtin_bloom_pipeline(instance, RBTREND_BLOOM_STAGE_DOWNSAMPLE, chain_format);
	rbtrend_graphics_pipeline upsample   = rbtrend_builtin_bloom_pipeline(instance, RBTREND_BLOOM_STAGE_UPSAMPLE, chain_format);
	rbtrend_graphics_pipeline composite  = rbtrend_builtin_bloom_pipeline(instance, RBTREND_BLOOM_STAGE_COMPOSITE, target_format);
	if (!prefilter || !downsample || !upsample || !composite)
		return false;
	rbtrend_pooled_target *chain = rbtrend_render_target_take(cmd_list, chain_width, chain_height, chain_format, levels);
	if (!chain)
		return false;
	rbtrend_binding_set source_set = rbtrend_texture_view_bloom_set(p._source);
	if (!source_set)
		return false;
	for (uint32_t level = 0; level < levels; ++level)
		if (!rbtrend_texture_view_bloom_set(chain->level_sources[level]))
			return false;
	// Without a mask or dirt, the pass's set 1 is the set it reads at set 0
	// again, which the shader then doesn't read: a pipeline's every set is
	// bound at a draw.
	rbtrend_binding_set mask_set = p._mask ? rbtrend_texture_view_bloom_set(p._mask) : source_set;
	rbtrend_binding_set dirt_set = p._lens_dirt ? rbtrend_texture_view_bloom_set(p._lens_dirt) : chain->level_sources[0]->bloom_set;
	if (!mask_set || !dirt_set)
		return false;

	rbtrend_bloom_params params{};
	params.threshold  = p._threshold;
	params.knee       = p._knee;
	params.radius     = p._radius;
	params.intensity  = p._intensity;
	params.use_mask   = p._mask ? 1.0f : 0.0f;
	params.lens_dirt_intensity = p._lens_dirt ? p._lens_dirt_intensity : 0.0f;
	auto level_width  = [&](uint32_t level) { return rbtrend_mip_size(chain_width, level); };
	auto level_height = [&](uint32_t level) { return rbtrend_mip_size(chain_height, level); };
	auto set_target   = [&](uint32_t width, uint32_t height) {
		params.target_size[0] = (float)width;
		params.target_size[1] = (float)height;
	};

	rbtrend_cmd_transition_range(cmd_list, source, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	if (p._mask)
		rbtrend_cmd_transition_range(cmd_list, rbtrend_texture_view_range(p._mask), CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	// Down: level 0 from the source, then each level from the one above.
	for (uint32_t level = 0; level < levels; ++level) {
		rbtrend_texture_view target = chain->level_targets[level];
		rbtrend_cmd_transition_view(cmd_list, target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		set_target(level_width(level), level_height(level));
		if (level == 0) {
			const rbtrend_binding_set sets[2] = {source_set, mask_set};
			rbtrend_record_fullscreen_pass_sets(cmd, prefilter, target->image_view, level_width(0), level_height(0), CRGPU_LOAD_OP_DONT_CARE, sets, 2,
			                                   cmd_list->compiled_frame, &params, sizeof(params));
		} else
			rbtrend_record_fullscreen_pass(cmd, downsample, target->image_view, level_width(level), level_height(level), CRGPU_LOAD_OP_DONT_CARE,
			                              chain->level_sources[level - 1]->bloom_set, nullptr, 0);
		rbtrend_cmd_transition_view(cmd_list, target, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	}
	// Up: each level added into the one above, from the smallest.
	for (uint32_t level = levels - 1; level > 0; --level) {
		rbtrend_texture_view target = chain->level_targets[level - 1];
		rbtrend_cmd_transition_view(cmd_list, target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		set_target(level_width(level - 1), level_height(level - 1));
		rbtrend_record_fullscreen_pass(cmd, upsample, target->image_view, level_width(level - 1), level_height(level - 1), CRGPU_LOAD_OP_LOAD,
		                              chain->level_sources[level]->bloom_set, &params, sizeof(params));
		rbtrend_cmd_transition_view(cmd_list, target, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	}
	// Onto the target, at its own size: the size of the mip its view is of.
	const uint32_t target_width  = rbtrend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip);
	const uint32_t target_height = rbtrend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip);
	if (p._lens_dirt)
		rbtrend_cmd_transition_range(cmd_list, rbtrend_texture_view_range(p._lens_dirt), CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	rbtrend_cmd_transition_view(cmd_list, p._target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	set_target(target_width, target_height);
	const rbtrend_binding_set composite_sets[2] = {chain->level_sources[0]->bloom_set, dirt_set};
	rbtrend_record_fullscreen_pass_sets(cmd, composite, p._target->image_view, target_width, target_height, CRGPU_LOAD_OP_LOAD, composite_sets, 2,
	                                   cmd_list->compiled_frame, &params, sizeof(params));
	return true;
}

/* Whether every mip and layer of [range] has something in it as the list
 * reaches this point: written before the list, or earlier in it. */
static bool rbtrend_cmd_range_written(rbtrend_command_list cmd_list, const rbtrend_texture_range &range) {
	for (uint32_t layer = 0; layer < range.layer_count; ++layer) {
		for (uint32_t mip = 0; mip < range.mip_count; ++mip) {
			const size_t index = rbtrend_cmd_layout_use(cmd_list, range, range.base_mip + mip, range.base_layer + layer);
			if (cmd_list->layout_uses[index].final_layout == CRGPU_IMAGE_LAYOUT_UNDEFINED)
				return false;
		}
	}
	return true;
}

/* Adds [material] to the materials [cmd_list] uploads at its submission,
 * once, building its binding set first: for a material Rend binds itself,
 * which no SET_MATERIAL names. False if its set can't be built. */
static bool rbtrend_cmd_use_material(rbtrend_command_list cmd_list, rbtrend_material material) {
	for (rbtrend_material listed : cmd_list->materials)
		if (listed == material)
			return true;
	if (!rbtrend_material_prepare(material))
		return false;
	try {
		cmd_list->materials.push_back(material);
	} catch (const std::bad_alloc &) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "compile: out of memory");
		return false;
	}
	return true;
}

/* One full-target pass of Rend's own into [target], a view [width] by
 * [height], reading [sets]. The target is moved to COLOR_ATTACHMENT_OPTIMAL
 * first and to SHADER_READ_ONLY after; what the sets read is the caller's to
 * have moved. */
static void rbtrend_cmd_own_pass(rbtrend_command_list cmd_list, rbtrend_graphics_pipeline pipeline, rbtrend_texture_view target, uint32_t width,
                                uint32_t height, const rbtrend_binding_set *sets, uint32_t set_count, const void *push, uint32_t push_size) {
	rbtrend_cmd_transition_view(cmd_list, target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	rbtrend_record_fullscreen_pass_sets(cmd_list->cmd_list, pipeline, target->image_view, width, height, CRGPU_LOAD_OP_DONT_CARE, sets,
	                                   set_count, cmd_list->compiled_frame, push, push_size);
	rbtrend_cmd_transition_view(cmd_list, target, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
}

/* BLUR, outside a pass: the source into SHADER_READ_ONLY; a pooled texture
 * of the target's size rendered from it across, moved to SHADER_READ_ONLY;
 * the target rendered from that down, left in COLOR_ATTACHMENT_OPTIMAL.
 * False, reported, if the source has nothing in it, it and the target are no
 * longer one size, or a pipeline, set or the pooled texture can't be made. */
static bool rbtrend_cmd_blur(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p                     = command._params._blur;
	rbtrend_instance instance          = cmd_list->instance;
	const rbtrend_texture_range source = rbtrend_texture_view_range(p._source);
	if (!rbtrend_cmd_range_written(cmd_list, source)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR: the source has nothing in it, from before the list or earlier in it");
		return false;
	}
	// Checked again: a resize since the command was sent may have changed
	// either size.
	if (!rbtrend_views_one_size(p._source, p._target)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR: the source and the target are no longer one size");
		return false;
	}
	const CRGPU_FORMAT between_format = CRGPU_FORMAT_R16G16B16A16_FLOAT;
	const CRGPU_FORMAT target_format  = p._target->texture._t2d->desc.format;
	const uint32_t width                = rbtrend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip);
	const uint32_t height               = rbtrend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip);
	rbtrend_graphics_pipeline across     = rbtrend_builtin_blur_pipeline(instance, between_format);
	rbtrend_graphics_pipeline down       = rbtrend_builtin_blur_pipeline(instance, target_format);
	if (!across || !down)
		return false;
	rbtrend_pooled_target *between = rbtrend_render_target_take(cmd_list, width, height, between_format, 1);
	if (!between)
		return false;
	rbtrend_binding_set source_set  = rbtrend_texture_view_bloom_set(p._source);
	rbtrend_binding_set between_set = rbtrend_texture_view_bloom_set(between->level_sources[0]);
	if (!source_set || !between_set)
		return false;

	rbtrend_blur_params params{};
	params.target_size[0] = (float)width;
	params.target_size[1] = (float)height;
	params.sigma          = p._sigma;
	params.radius         = std::ceil(3.0f * p._sigma);
	rbtrend_cmd_transition_range(cmd_list, source, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	rbtrend_texture_view between_target = between->level_targets[0];
	rbtrend_cmd_transition_view(cmd_list, between_target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	params.step[0] = 1.0f / (float)width;
	params.step[1] = 0.0f;
	rbtrend_record_fullscreen_pass(cmd_list->cmd_list, across, between_target->image_view, width, height, CRGPU_LOAD_OP_DONT_CARE, source_set, &params,
	                              sizeof(params));
	rbtrend_cmd_transition_view(cmd_list, between_target, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	rbtrend_cmd_transition_view(cmd_list, p._target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	params.step[0] = 0.0f;
	params.step[1] = 1.0f / (float)height;
	rbtrend_record_fullscreen_pass(cmd_list->cmd_list, down, p._target->image_view, width, height, CRGPU_LOAD_OP_DONT_CARE, between_set, &params,
	                              sizeof(params));
	return true;
}

/* OUTLINE, outside a pass: the mask into SHADER_READ_ONLY; its seeds into
 * one pooled texture of places, each jump from one pooled texture into the
 * other, each moved to SHADER_READ_ONLY once rendered; then the outline
 * blended over the target, left in COLOR_ATTACHMENT_OPTIMAL. False,
 * reported, if the mask has nothing in it, it and the target are no longer
 * one size, or a pipeline, set or pooled texture can't be made. */
static bool rbtrend_cmd_outline(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p                   = command._params._outline;
	rbtrend_instance instance        = cmd_list->instance;
	const rbtrend_texture_range mask = rbtrend_texture_view_range(p._mask);
	if (!rbtrend_cmd_range_written(cmd_list, mask)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE: the mask has nothing in it, from before the list or earlier in it");
		return false;
	}
	// Checked again: a resize since the command was sent may have changed
	// either size.
	if (!rbtrend_views_one_size(p._mask, p._target)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE: the mask and the target are no longer one size");
		return false;
	}
	uint32_t width = 0, height = 0;
	rbtrend_view_size(p._target, &width, &height);
	const CRGPU_FORMAT places_format = CRGPU_FORMAT_R32G32_FLOAT;
	rbtrend_graphics_pipeline flood     = rbtrend_builtin_outline_pipeline(instance, places_format, false);
	rbtrend_graphics_pipeline draw      = rbtrend_builtin_outline_pipeline(instance, p._target->texture._t2d->desc.format, true);
	if (!flood || !draw)
		return false;
	// Two takes of one key in one compile are two textures.
	rbtrend_pooled_target *places[2] = {rbtrend_render_target_take(cmd_list, width, height, places_format, 1), nullptr};
	if (!places[0])
		return false;
	places[1] = rbtrend_render_target_take(cmd_list, width, height, places_format, 1);
	if (!places[1])
		return false;
	rbtrend_binding_set mask_set     = rbtrend_texture_view_bloom_set(p._mask);
	rbtrend_binding_set place_set[2] = {rbtrend_texture_view_bloom_set(places[0]->level_sources[0]),
	                                   rbtrend_texture_view_bloom_set(places[1]->level_sources[0])};
	if (!mask_set || !place_set[0] || !place_set[1])
		return false;

	rbtrend_outline_params params{};
	for (int i = 0; i < 4; ++i)
		params.color[i] = p._color[i];
	params.target_size[0] = (float)width;
	params.target_size[1] = (float)height;
	params.width          = p._width;
	// One pass into [to] from [from_set], its places left readable.
	auto flood_pass = [&](uint32_t to, rbtrend_binding_set from_set) {
		rbtrend_texture_view target = places[to]->level_targets[0];
		rbtrend_cmd_transition_view(cmd_list, target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		rbtrend_record_fullscreen_pass(cmd_list->cmd_list, flood, target->image_view, width, height, CRGPU_LOAD_OP_DONT_CARE, from_set, &params,
		                              sizeof(params));
		rbtrend_cmd_transition_view(cmd_list, target, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	};

	rbtrend_cmd_transition_range(cmd_list, mask, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	params.mode = RBTREND_OUTLINE_MODE_SEED;
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
	params.mode      = RBTREND_OUTLINE_MODE_JUMP;
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

	params.mode = RBTREND_OUTLINE_MODE_COMPOSITE;
	rbtrend_cmd_transition_view(cmd_list, p._target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	rbtrend_record_fullscreen_pass(cmd_list->cmd_list, draw, p._target->image_view, width, height, CRGPU_LOAD_OP_LOAD, place_set[current], &params,
	                              sizeof(params));
	return true;
}

/* TONE_MAP, outside a pass: the source into SHADER_READ_ONLY, the target
 * into COLOR_ATTACHMENT_OPTIMAL and every texel of it replaced. */
static bool rbtrend_cmd_tone_map(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p                     = command._params._tone_map;
	rbtrend_instance instance          = cmd_list->instance;
	const rbtrend_texture_range source = rbtrend_texture_view_range(p._source);
	if (!rbtrend_cmd_range_written(cmd_list, source)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "TONE_MAP: the source has nothing in it, from before the list or earlier in it");
		return false;
	}
	rbtrend_graphics_pipeline pipeline = rbtrend_lit_tone_map_pipeline(instance, p._target->texture._t2d->desc.format);
	if (!pipeline)
		return false;
	// The exposure's last value, or a placeholder that isn't read.
	rbtrend_texture_view exposure = p._auto_exposure ? p._auto_exposure->value_views[p._auto_exposure->current]
	                                                : instance->builtins.lit.placeholder_2d_view;
	if (p._auto_exposure && !p._auto_exposure->adapted) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "TONE_MAP: its _auto_exposure has never adapted (AUTO_EXPOSURE)");
		return false;
	}
	rbtrend_binding_set sets[2] = {rbtrend_texture_view_bloom_set(p._source), rbtrend_texture_view_environment_set(exposure)};
	if (!sets[0] || !sets[1])
		return false;
	const uint32_t width  = rbtrend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip);
	const uint32_t height = rbtrend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip);
	rbtrend_tone_map_params params{};
	params.target_size[0] = (float)width;
	params.target_size[1] = (float)height;
	params.exposure       = p._exposure;
	params.curve          = p._curve;
	params.encoding       = p._encoding;
	params.automatic      = p._auto_exposure ? 1u : 0u;
	rbtrend_cmd_transition_range(cmd_list, source, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	// The placeholder's texels aren't read, but its binding is, and a
	// binding's image must be in the layout its descriptor names: the
	// placeholder is never written, so until something moves it, it is
	// still in UNDEFINED.
	if (p._auto_exposure)
		rbtrend_cmd_transition_view(cmd_list, exposure, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	else
		rbtrend_cmd_transition_range(cmd_list, rbtrend_texture_view_range(exposure), CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	rbtrend_cmd_transition_view(cmd_list, p._target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	rbtrend_record_fullscreen_pass_sets(cmd_list->cmd_list, pipeline, p._target->image_view, width, height, CRGPU_LOAD_OP_DONT_CARE, sets, 2,
	                                   cmd_list->compiled_frame, &params, sizeof(params));
	return true;
}

/* What both fills of an environment end with, once the radiance cube's
 * first mip has the picture: its other mips generated; each mip of the
 * specular chain prefiltered from it, face by face; and the irradiance
 * cube. Everything it renders is left in SHADER_READ_ONLY. */
static bool rbtrend_cmd_prefilter_environment(rbtrend_command_list cmd_list, rbtrend_lit_environment env) {
	rbtrend_instance instance            = cmd_list->instance;
	rbtrend_graphics_pipeline prefilter  = rbtrend_lit_pass_pipeline(instance, RBTREND_LIT_PASS_PREFILTER);
	rbtrend_graphics_pipeline irradiance = rbtrend_lit_pass_pipeline(instance, RBTREND_LIT_PASS_IRRADIANCE);
	rbtrend_binding_set radiance_set     = rbtrend_texture_view_environment_set(env->radiance_cube);
	if (!prefilter || !irradiance || !radiance_set)
		return false;
	if (!rbtrend_cmd_generate_mips(cmd_list, env->radiance, 0, CRGPU_FILTER_LINEAR))
		return false;
	rbtrend_cmd_transition_range(cmd_list, rbtrend_texture_view_range(env->radiance_cube), CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	const uint32_t size = env->desc.specular_size;
	rbtrend_environment_params params{};
	params.source_size = (float)size;
	params.source_mips = (float)env->radiance->desc.mip_levels;
	for (uint32_t mip = 0; mip < env->specular_mips; ++mip) {
		const uint32_t side = rbtrend_mip_size(size, mip);
		params.roughness    = env->specular_mips > 1 ? (float)mip / (float)(env->specular_mips - 1) : 0.0f;
		params.sample_count = mip == 0 ? 1u : 256u;
		params.target_size[0] = params.target_size[1] = (float)side;
		for (uint32_t face = 0; face < 6; ++face) {
			params.face = face;
			rbtrend_cmd_own_pass(cmd_list, prefilter, env->specular_faces[(size_t)mip * 6 + face], side, side, &radiance_set, 1, &params, sizeof(params));
		}
	}
	params.roughness      = 0.0f;
	params.sample_count   = 512u;
	params.target_size[0] = params.target_size[1] = (float)env->desc.irradiance_size;
	for (uint32_t face = 0; face < 6; ++face) {
		params.face = face;
		rbtrend_cmd_own_pass(cmd_list, irradiance, env->irradiance_faces[face], env->desc.irradiance_size, env->desc.irradiance_size, &radiance_set, 1,
		                    &params, sizeof(params));
	}
	return true;
}

/* LIT_ENVIRONMENT, outside a pass: the source copied into the radiance
 * cube's first mip, face by face, then prefiltered. */
static bool rbtrend_cmd_lit_environment(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p                     = command._params._lit_environment;
	rbtrend_lit_environment env        = p._environment;
	const rbtrend_texture_range source = rbtrend_texture_view_range(p._source);
	if (!rbtrend_cmd_range_written(cmd_list, source)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_ENVIRONMENT: the source has nothing in it, from before the list or earlier in it");
		return false;
	}
	const bool cube               = p._source->desc.shape == RBTREND_TEXTURE_VIEW_SHAPE_CUBE;
	rbtrend_graphics_pipeline copy = rbtrend_lit_pass_pipeline(cmd_list->instance, cube ? RBTREND_LIT_PASS_FROM_CUBE : RBTREND_LIT_PASS_FROM_EQUIRECT);
	rbtrend_binding_set source_set = copy ? rbtrend_texture_view_environment_set(p._source) : nullptr;
	if (!source_set)
		return false;
	const uint32_t size = env->desc.specular_size;
	rbtrend_environment_params params{};
	params.target_size[0] = params.target_size[1] = (float)size;
	rbtrend_cmd_transition_range(cmd_list, source, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	for (uint32_t face = 0; face < 6; ++face) {
		params.face = face;
		rbtrend_cmd_own_pass(cmd_list, copy, env->radiance_faces[face], size, size, &source_set, 1, &params, sizeof(params));
	}
	return rbtrend_cmd_prefilter_environment(cmd_list, env);
}

/* LIT_SKY, outside a pass: Preetham's or Hosek and Wilkie's sky into the radiance cube's first
 * mip, face by face, then prefiltered. */
static bool rbtrend_cmd_lit_sky(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p                = command._params._lit_sky;
	rbtrend_lit_environment env   = p._environment;
	const bool hosek             = p._model == RBTREND_LIT_SKY_MODEL_HOSEK_WILKIE;
	rbtrend_graphics_pipeline sky = rbtrend_lit_pass_pipeline(cmd_list->instance, hosek ? RBTREND_LIT_PASS_HOSEK_SKY : RBTREND_LIT_PASS_SKY);
	if (!sky)
		return false;
	const uint32_t size = env->desc.specular_size;
	const float l       = std::sqrt(p._sun_direction[0] * p._sun_direction[0] + p._sun_direction[1] * p._sun_direction[1] +
	                                p._sun_direction[2] * p._sun_direction[2]);
	rbtrend_sky_params params{};
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
	rbtrend_binding_set sky_set = nullptr;
	if (hosek) {
		rbtrend_material material = rbtrend_lit_environment_sky_material(env);
		if (!material)
			return false;
		rbtrend_hosek_sky_block block{};
		rbtrend_lit_hosek_cook(params.sun, p._turbidity, p._ground_albedo, p._intensity, &block);
		for (uint32_t i = 0; i < material->slot_count; ++i) {
			if (material->slots[i].bytes) {
				memcpy(material->slots[i].bytes, &block, sizeof(block));
				material->slots[i].dirty = true;
			}
		}
		if (!rbtrend_cmd_use_material(cmd_list, material))
			return false;
		sky_set = material->set;
	}
	for (uint32_t face = 0; face < 6; ++face) {
		params.face = face;
		rbtrend_cmd_own_pass(cmd_list, sky, env->radiance_faces[face], size, size, hosek ? &sky_set : nullptr, hosek ? 1 : 0, &params, sizeof(params));
	}
	return rbtrend_cmd_prefilter_environment(cmd_list, env);
}

/* LIT_AMBIENT_OCCLUSION, outside a pass: the raw estimate into a pooled
 * texture the depth's size, then smoothed onto the target, left in
 * SHADER_READ_ONLY for the scene to read. */
static bool rbtrend_cmd_lit_ambient_occlusion(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p                    = command._params._lit_ambient_occlusion;
	rbtrend_instance instance         = cmd_list->instance;
	const rbtrend_texture_range depth = rbtrend_texture_view_range(p._depth);
	if (!rbtrend_cmd_range_written(cmd_list, depth)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_AMBIENT_OCCLUSION: the depth has nothing in it, from before the list or earlier in it");
		return false;
	}
	const uint32_t width            = rbtrend_mip_size(p._target->texture._t2d->desc.width, p._target->desc.base_mip);
	const uint32_t height           = rbtrend_mip_size(p._target->texture._t2d->desc.height, p._target->desc.base_mip);
	rbtrend_graphics_pipeline raw    = rbtrend_lit_pass_pipeline(instance, RBTREND_LIT_PASS_OCCLUSION);
	rbtrend_graphics_pipeline smooth = rbtrend_lit_blur_pipeline(instance, p._target->texture._t2d->desc.format);
	if (!raw || !smooth)
		return false;
	// The view's matrices and size, and the command's settings, in the
	// scene's occlusion material for this list.
	const rbtrend_lit_scene_block &b = p._scene->block;
	rbtrend_occlusion_block block{};
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
	rbtrend_material material = p._scene->occlusion_material;
	for (uint32_t i = 0; i < material->slot_count; ++i) {
		if (material->slots[i].bytes) {
			memcpy(material->slots[i].bytes, &block, sizeof(block));
			material->slots[i].dirty = true;
		}
	}
	if (!rbtrend_cmd_use_material(cmd_list, material))
		return false;
	rbtrend_pooled_target *pooled = rbtrend_render_target_take(cmd_list, width, height, RBTREND_OCCLUSION_FORMAT, 1);
	if (!pooled)
		return false;
	rbtrend_binding_set raw_sets[2] = {material->set, rbtrend_texture_view_environment_set(p._depth)};
	rbtrend_binding_set blur_set    = rbtrend_texture_view_environment_set(pooled->level_sources[0]);
	if (!raw_sets[1] || !blur_set)
		return false;
	rbtrend_cmd_transition_range(cmd_list, depth, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	rbtrend_cmd_own_pass(cmd_list, raw, pooled->level_targets[0], width, height, raw_sets, 2, nullptr, 0);
	rbtrend_size_params size{};
	size.target_size[0] = (float)width;
	size.target_size[1] = (float)height;
	rbtrend_cmd_own_pass(cmd_list, smooth, p._target, width, height, &blur_set, 1, &size, sizeof(size));
	return true;
}

/* AUTO_EXPOSURE, outside a pass: the source's log luminance into the
 * exposure's luminance texture, its mips averaged down, and the adapted
 * exposure into the value texture not last written, which becomes the
 * current one. */
static bool rbtrend_cmd_auto_exposure(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p                     = command._params._auto_exposure;
	rbtrend_instance instance          = cmd_list->instance;
	rbtrend_exposure exposure          = p._exposure;
	const rbtrend_texture_range source = rbtrend_texture_view_range(p._source);
	if (!rbtrend_cmd_range_written(cmd_list, source)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "AUTO_EXPOSURE: the source has nothing in it, from before the list or earlier in it");
		return false;
	}
	const bool histogram               = exposure->histogram_set != nullptr;
	rbtrend_graphics_pipeline luminance = rbtrend_lit_pass_pipeline(instance, RBTREND_LIT_PASS_LUMINANCE);
	rbtrend_graphics_pipeline adapt     = rbtrend_lit_pass_pipeline(instance, histogram ? RBTREND_LIT_PASS_ADAPT_HISTOGRAM : RBTREND_LIT_PASS_ADAPT);
	rbtrend_compute_pipeline count      = histogram ? rbtrend_lit_histogram_pipeline(instance) : nullptr;
	if (!luminance || !adapt || (histogram && !count))
		return false;
	const uint32_t next          = exposure->current ^ 1u;
	rbtrend_binding_set source_set = rbtrend_texture_view_environment_set(p._source);
	// Set 0 reads the luminance texture's mips, or the histogram's bins.
	rbtrend_binding_set adapt_sets[2] = {histogram ? exposure->histogram_set : rbtrend_texture_view_environment_set(exposure->luminance_view),
	                                    rbtrend_texture_view_environment_set(exposure->value_views[exposure->current])};
	if (!source_set || !adapt_sets[0] || !adapt_sets[1])
		return false;
	rbtrend_cmd_transition_range(cmd_list, source, CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	rbtrend_size_params size{};
	size.target_size[0] = size.target_size[1] = (float)RBTREND_EXPOSURE_LUMINANCE_SIZE;
	rbtrend_cmd_own_pass(cmd_list, luminance, exposure->luminance_target, RBTREND_EXPOSURE_LUMINANCE_SIZE, RBTREND_EXPOSURE_LUMINANCE_SIZE,
	                    &source_set, 1, &size, sizeof(size));
	if (!rbtrend_cmd_generate_mips(cmd_list, exposure->luminance, 0, CRGPU_FILTER_LINEAR))
		return false;
	rbtrend_cmd_transition_range(cmd_list, rbtrend_texture_view_range(exposure->luminance_view), CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	// Read only once it has been adapted: the first takes its target.
	rbtrend_cmd_transition_view(cmd_list, exposure->value_views[exposure->current], CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY);
	if (histogram) {
		// The bins emptied, then counted, then read by the pass: each use
		// ordered after the last as any dispatch's set is (the first after
		// what an earlier submission of the frame left of this copy).
		rbtrend_binding_set set                = exposure->histogram_set;
		eq::array<rbtrend_pass_image> &images = cmd_list->pass_images;
		eq::array<rbtrend_pass_store> &stores = cmd_list->pass_stores;
		images.clear();
		stores.clear();
		try {
			if (!rbtrend_pass_images_add_set(cmd_list, images, set, 0, count->storage_writes))
				return false;
			rbtrend_storage_stores_add_set(stores, set, cmd_list->compiled_frame, 0, count->storage_writes);
		} catch (const std::bad_alloc &) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "compile: out of memory");
			return false;
		}
		crgpu_command_list cmd = cmd_list->cmd_list;
		rbtrend_histogram_params hp{};
		hp.min_log = exposure->desc.min_log;
		hp.max_log = exposure->desc.max_log;
		hp.size    = RBTREND_EXPOSURE_LUMINANCE_SIZE;
		for (uint32_t clear = 1;; clear = 0) {
			rbtrend_cmd_ready_set_resources(cmd_list, images, stores);
			crgpu_command_list_bind_compute_pipeline(cmd, count->pipeline);
			crgpu_command_list_bind_compute_descriptor_sets(cmd, count->pipeline, 0, &set->sets[rbtrend_binding_set_frame_slot(set, cmd_list->compiled_frame)], 1);
			hp.clear = clear;
			crgpu_command_list_push_compute_constants(cmd, count->pipeline, 0, sizeof(hp), &hp);
			const uint32_t groups = clear ? 1u : (RBTREND_EXPOSURE_LUMINANCE_SIZE + 15u) / 16u;
			crgpu_command_list_dispatch(cmd, groups, groups, 1);
			if (!clear)
				break;
		}
		// Read by the pass now, not written.
		for (rbtrend_pass_store &store : stores)
			store.written = false;
		rbtrend_cmd_ready_set_resources(cmd_list, images, stores);
		rbtrend_histogram_adapt_params params{};
		params.key        = exposure->desc.key;
		params.min_log    = exposure->desc.min_log;
		params.max_log    = exposure->desc.max_log;
		params.up_rate    = exposure->desc.up_rate;
		params.down_rate  = exposure->desc.down_rate;
		params.delta_time = p._delta_time;
		params.low        = exposure->desc.low_share;
		params.high       = exposure->desc.high_share;
		params.first      = exposure->adapted ? 0u : 1u;
		rbtrend_cmd_own_pass(cmd_list, adapt, exposure->value_targets[next], 1, 1, adapt_sets, 2, &params, sizeof(params));
		exposure->current = next;
		exposure->adapted = true;
		return true;
	}
	rbtrend_adapt_params params{};
	params.key        = exposure->desc.key;
	params.min_log    = exposure->desc.min_log;
	params.max_log    = exposure->desc.max_log;
	params.up_rate    = exposure->desc.up_rate;
	params.down_rate  = exposure->desc.down_rate;
	params.delta_time = p._delta_time;
	params.last_mip   = (float)(exposure->luminance->desc.mip_levels - 1);
	params.first      = exposure->adapted ? 0u : 1u;
	rbtrend_cmd_own_pass(cmd_list, adapt, exposure->value_targets[next], 1, 1, adapt_sets, 2, &params, sizeof(params));
	exposure->current = next;
	exposure->adapted = true;
	return true;
}


/* ---- Ray tracing's commands (rbtrendraytracing.h) -------------------------- */

/* Moves [structure] to [after] in [cmd_list], with Carrottus GPU's barrier from
 * where the walk has it (NONE before its first use in the list). A write
 * after a write gets one too. Throws std::bad_alloc. */
static void rbtrend_cmd_rt_access(rbtrend_command_list cmd_list, crgpu_acceleration_structure structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS after) {
	CRGPU_ACCELERATION_STRUCTURE_ACCESS before = CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE;
	rbtrend_rt_access *known                      = nullptr;
	for (rbtrend_rt_access &access : cmd_list->rt_accesses)
		if (access.structure == structure)
			known = &access;
	if (known)
		before = known->access;
	else
		rbtrend_push(cmd_list->rt_accesses, rbtrend_rt_access{structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS_NONE});
	if (before == after && after != CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE) {
		return;
	}
	crgpu_acceleration_structure_barrier barrier{{CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BARRIER, nullptr}, structure, before, after};
	crgpu_command_list_acceleration_structure_barrier(cmd_list->cmd_list, &barrier, 1);
	for (rbtrend_rt_access &access : cmd_list->rt_accesses)
		if (access.structure == structure)
			access.access = after;
}

/* A geometry's buffer moved to [state] for a build or a copy: a buffer in
 * UPLOAD memory (one written directly) is never given a barrier, and reads
 * as it is (crgpu.h). */
static void rbtrend_cmd_rt_source(rbtrend_command_list cmd_list, rbtrend_buffer_store *store, CRGPU_RESOURCE_STATE state) {
	rbtrend_cmd_use_store(cmd_list, store);
	if (store->staged)
		rbtrend_cmd_transition_store(cmd_list, store, state);
}

/* [bytes] of [data] copied by the GPU, in the list's place, into [store] at
 * [offset], through the current frame's transient memory. [store] is in
 * COPY_DEST. False, reported, if the memory can't be taken. */
static bool rbtrend_cmd_rt_put(rbtrend_command_list cmd_list, rbtrend_buffer_store *store, uint64_t offset, const void *data, uint64_t bytes) {
	RBTREND_TRANSIENT_SPAN span{};
	if (rbtrend_transient_allocate(cmd_list->instance, bytes, 16, &span, nullptr) != RBT_RESULT_OK) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_REND_GPU_FAILURE, "BUILD_BLAS: transient memory can't be taken for the BLAS table");
		return false;
	}
	memcpy(span.data, data, (size_t)bytes);
	const rbtrend_transient_chunk *chunk = (const rbtrend_transient_chunk *)span._chunk;
	crgpu_command_list_copy_buffer(cmd_list->cmd_list, chunk->buffer, store->buffer, span._offset, offset, bytes);
	return true;
}

/* The scratch a run of build commands needs, [first] to [last] (exclusive),
 * each build's range at the device's alignment. */
static uint64_t rbtrend_cmd_rt_run_scratch(rbtrend_command_list cmd_list, size_t first, size_t last) {
	const rbtrend_rt_state *rt = cmd_list->instance->rt;
	const uint64_t alignment  = rt->limits.scratch_offset_alignment ? rt->limits.scratch_offset_alignment : 256;
	uint64_t total            = 0;
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		uint64_t size                 = 0;
		if (command._type == RBTREND_COMMAND_BUILD_BLAS) {
			const auto &p = command._params._build_blas;
			if (p._mode == RBTREND_BUILD_MODE_BUILD)
				size = p._blas->build_scratch;
			else if (p._mode == RBTREND_BUILD_MODE_REFIT)
				size = p._blas->update_scratch;
		} else {
			size = command._params._build_tlas._tlas->build_scratch;
		}
		total = (total + alignment - 1) / alignment * alignment + size;
	}
	return total;
}

/* Where the run of build commands that starts at [first] ends: at the
 * first command that isn't one, or a BUILD_BLAS of a BLAS the run already
 * builds, whose second build goes after the first in a run of its own. */
static size_t rbtrend_cmd_rt_run_end(rbtrend_command_list cmd_list, size_t first) {
	size_t last = first;
	while (last < cmd_list->commands.size() &&
	       (cmd_list->commands[last]._type == RBTREND_COMMAND_BUILD_BLAS || cmd_list->commands[last]._type == RBTREND_COMMAND_BUILD_TLAS)) {
		const RBTREND_COMMAND &command = cmd_list->commands[last];
		bool repeated                 = false;
		for (size_t k = first; k < last && command._type == RBTREND_COMMAND_BUILD_BLAS; ++k)
			if (cmd_list->commands[k]._type == RBTREND_COMMAND_BUILD_BLAS && cmd_list->commands[k]._params._build_blas._blas == command._params._build_blas._blas)
				repeated = true;
		if (repeated)
			break;
		++last;
	}
	return last;
}

/* Before anything of a list is recorded: the builds' scratch grown to what
 * its largest run needs. False, reported, if it can't be. */
static bool rbtrend_cmd_rt_prepare(rbtrend_command_list cmd_list) {
	uint64_t largest = 0;
	for (size_t i = 0; i < cmd_list->commands.size();) {
		const RBTREND_COMMAND_TYPE type = cmd_list->commands[i]._type;
		if (type != RBTREND_COMMAND_BUILD_BLAS && type != RBTREND_COMMAND_BUILD_TLAS) {
			++i;
			continue;
		}
		const size_t last    = rbtrend_cmd_rt_run_end(cmd_list, i);
		const uint64_t total = rbtrend_cmd_rt_run_scratch(cmd_list, i, last);
		largest              = total > largest ? total : largest;
		i                    = last;
	}
	if (largest == 0)
		return true;
	return rbtrend_rt_prepare_scratch(cmd_list->instance, largest);
}

/* The BUILD_TLAS commands of [first] to [last] (exclusive), none of one
 * TLAS twice: each set's written blocks copied into its TLAS's current
 * records and material table, the instance writers, then the builds in one
 * call, each its own range of the scratch from [*scratch_at]. False,
 * reported, if one can't be recorded. Throws std::bad_alloc. */
static bool rbtrend_cmd_rt_tlas_group(rbtrend_command_list cmd_list, size_t first, size_t last, uint64_t *scratch_at) {
	rbtrend_instance instance = cmd_list->instance;
	rbtrend_rt_state *rt      = instance->rt;
	crgpu_command_list cmd = cmd_list->cmd_list;
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_TLAS)
			continue;
		rbtrend_tlas tlas = command._params._build_tlas._tlas;
		rbtrend_cmd_use_store(cmd_list, &tlas->records->parts[0].store);
		rbtrend_cmd_use_store(cmd_list, &tlas->materials->parts[0].store);
		rbtrend_cmd_transition_store(cmd_list, &tlas->records->parts[0].store, CRGPU_RESOURCE_STATE_COPY_DEST);
		rbtrend_cmd_transition_store(cmd_list, &tlas->materials->parts[0].store, CRGPU_RESOURCE_STATE_COPY_DEST);
	}
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type == RBTREND_COMMAND_BUILD_TLAS) {
			// The set's written blocks, a copy region per run of them (D3);
			// the rest stay as the builds before left them (carried over).
			rbtrend_tlas tlas          = command._params._build_tlas._tlas;
			const rbtrend_rt_seal seal = rbtrend_rt_seal_of(command);
			const rbtrend_rt_set &set  = tlas->sets[seal.set];
			const struct {
				const uint64_t *blocks;
				uint32_t words;
				uint32_t total;
				size_t element;
				crgpu_buffer from;
				crgpu_buffer to;
			} parts[2] = {
			    {tlas->record_blocks + (size_t)seal.set * tlas->record_words, tlas->record_words, tlas->max_instances, sizeof(RBTREND_RAY_INSTANCE), set.records,
			     tlas->records->parts[0].store.buffer},
			    {tlas->material_blocks + (size_t)seal.set * tlas->material_words, tlas->material_words, tlas->max_materials, sizeof(uint32_t), set.materials,
			     tlas->materials->parts[0].store.buffer},
			};
			for (const auto &part : parts) {
				const uint32_t blocks = (part.total + RBTREND_RT_BLOCK - 1) / RBTREND_RT_BLOCK;
				uint32_t b            = 0;
				while (b < blocks) {
					if (!((part.blocks[b / 64] >> (b % 64)) & 1)) {
						++b;
						continue;
					}
					uint32_t end = b;
					while (end < blocks && ((part.blocks[end / 64] >> (end % 64)) & 1))
						++end;
					const uint64_t begin_element = (uint64_t)b * RBTREND_RT_BLOCK;
					uint64_t end_element         = (uint64_t)end * RBTREND_RT_BLOCK;
					if (end_element > part.total)
						end_element = part.total;
					crgpu_command_list_copy_buffer(cmd, part.from, part.to, begin_element * part.element, begin_element * part.element,
					                        (end_element - begin_element) * part.element);
					b = end;
				}
			}
			continue;
		}
	}
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_TLAS)
			continue;
		rbtrend_tlas tlas = command._params._build_tlas._tlas;
		rbtrend_cmd_transition_store(cmd_list, &tlas->records->parts[0].store, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS);
		rbtrend_cmd_transition_store(cmd_list, &tlas->materials->parts[0].store, CRGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	}
	// The instance writers: each TLAS's current records in the device's
	// layout, a slot resolved to its BLAS's current structure (D3).
	rbtrend_compute_pipeline writer = rbtrend_rt_writer_pipeline(instance);
	if (!writer)
		return false;
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_TLAS)
			continue;
		cmd_list->report_send     = cmd_list->command_sends[i];
		rbtrend_tlas tlas          = command._params._build_tlas._tlas;
		const rbtrend_rt_seal seal = rbtrend_rt_seal_of(command);
		const rbtrend_rt_set &set  = tlas->sets[seal.set];
		if (set.instance_count == 0)
			continue;
		rbtrend_binding_set writer_set = rbtrend_rt_tlas_writer_set(tlas);
		if (!writer_set)
			return false;
		eq::array<rbtrend_pass_image> &images = cmd_list->pass_images;
		eq::array<rbtrend_pass_store> &stores = cmd_list->pass_stores;
		images.clear();
		stores.clear();
		if (!rbtrend_pass_images_add_set(cmd_list, images, writer_set, 0, writer->storage_writes))
			return false;
		rbtrend_storage_stores_add_set(stores, writer_set, cmd_list->compiled_frame, 0, writer->storage_writes);
		rbtrend_cmd_ready_set_resources(cmd_list, images, stores);
		rbtrend_cmd_use_handle(cmd_list, writer_set);
		crgpu_command_list_bind_compute_pipeline(cmd, writer->pipeline);
		crgpu_command_list_bind_compute_descriptor_sets(cmd, writer->pipeline, 0, &writer_set->sets[0], 1);
		rbtrend_rt_writer_params params{};
		params.count         = set.instance_count;
		params.instance_size = rt->instance_layout.size;
		crgpu_command_list_push_compute_constants(cmd, writer->pipeline, 0, sizeof(params), &params);
		crgpu_command_list_dispatch(cmd, (set.instance_count + RBTREND_RT_WRITER_GROUP - 1) / RBTREND_RT_WRITER_GROUP, 1, 1);
	}
	// The builds, over each set's count and the BLASes its set named at the
	// seal (residency, D6), each TLAS's from its own array.
	rbtrend_buffer_store *scratch = &rt->scratch->parts[0].store;
	const uint64_t alignment     = rt->limits.scratch_offset_alignment ? rt->limits.scratch_offset_alignment : 256;
	eq::array<crgpu_acceleration_structure_build> &builds  = cmd_list->rt_builds;
	eq::array<crgpu_acceleration_structure_inputs> &inputs = cmd_list->rt_inputs;
	builds.clear();
	inputs.clear();
	// The builds point into [inputs]: room for all of them first.
	if (inputs.reserve(last - first) != EQ_SUCCESS || builds.reserve(last - first) != EQ_SUCCESS)
		throw std::bad_alloc();
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_TLAS)
			continue;
		rbtrend_tlas tlas                      = command._params._build_tlas._tlas;
		const rbtrend_rt_seal seal             = rbtrend_rt_seal_of(command);
		const rbtrend_rt_set &set              = tlas->sets[seal.set];
		rbtrend_buffer_store *device_instances = &tlas->device_instances->parts[0].store;
		rbtrend_cmd_use_store(cmd_list, device_instances);
		rbtrend_cmd_transition_store(cmd_list, device_instances, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT);
		const uint32_t *levels = tlas->bottom_levels + (size_t)seal.set * tlas->max_instances;
		for (uint32_t l = 0; l < set.bottom_level_count; ++l)
			tlas->build_levels[l] = rt->blas_structure[levels[l]];
		crgpu_acceleration_structure_inputs in{};
		in.base.type          = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_INPUTS;
		in.level              = CRGPU_ACCELERATION_STRUCTURE_LEVEL_TOP;
		in.flags              = CRGPU_ACCELERATION_STRUCTURE_BUILD_PREFER_FAST_BUILD;
		in.instances          = device_instances->buffer;
		in.instance_count     = set.instance_count;
		in.bottom_levels      = set.bottom_level_count ? tlas->build_levels : nullptr;
		in.bottom_level_count = set.bottom_level_count;
		rbtrend_push(inputs, in);
		rbtrend_cmd_rt_access(cmd_list, tlas->structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
		*scratch_at = (*scratch_at + alignment - 1) / alignment * alignment;
		crgpu_acceleration_structure_build build{};
		build.base.type      = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
		build.mode           = CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
		build.inputs         = &inputs[inputs.size() - 1];
		build.destination    = tlas->structure;
		build.scratch        = scratch->buffer;
		build.scratch_offset = *scratch_at;
		rbtrend_push(builds, build);
		*scratch_at += tlas->build_scratch;
	}
	if (builds.empty())
		return true;
	const CR_RESULT sr = crgpu_command_list_build_acceleration_structures(cmd, builds.data, (uint32_t)builds.size());
	if (CR_FAILED(sr)) {
		rbtrend_cmd_report(cmd_list, rbtrend_result_from_cr(sr), "BUILD_TLAS: the builds can't be recorded (%s)", cr_result_str(sr));
		return false;
	}
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type == RBTREND_COMMAND_BUILD_TLAS)
			rbtrend_cmd_rt_access(cmd_list, command._params._build_tlas._tlas->structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	}
	return true;
}

/* One run of BUILD_BLAS and BUILD_TLAS commands, [first] to [last]
 * (exclusive), outside a pass, as one build call to the device (T8): the
 * copies first (each BLAS's table entry and arena range, each TLAS's written
 * blocks), the BLASes' builds and compactions, one barrier, the instance
 * writers, the TLASes' builds. False, reported, if a command can't run. */
static bool rbtrend_cmd_rt_builds(rbtrend_command_list cmd_list, size_t first, size_t last) {
	rbtrend_instance instance = cmd_list->instance;
	rbtrend_rt_state *rt      = instance->rt;
	crgpu_command_list cmd = cmd_list->cmd_list;
	rbtrend_buffer_store *table = &rt->blas_table->parts[0].store;
	rbtrend_buffer_store *arena = &rt->arena->parts[0].store;

	// What each command needs before anything is recorded: a REFIT of a
	// BLAS built, a COMPACT of one whose compacted size is known, a
	// BUILD_TLAS whose seal still holds its set.
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		cmd_list->report_send         = cmd_list->command_sends[i];
		if (command._type == RBTREND_COMMAND_BUILD_TLAS) {
			rbtrend_tlas tlas         = command._params._build_tlas._tlas;
			const rbtrend_rt_seal seal = rbtrend_rt_seal_of(command);
			const rbtrend_rt_set &set  = tlas->sets[seal.set];
			if (set.state != RBTREND_RT_SET_SEALED || set.sequence != seal.sequence) {
				rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT,
				                  "BUILD_TLAS's list was submitted since it was sent: its set is built and free; send BUILD_TLAS again, which seals the next set");
				return false;
			}
			rbtrend_cmd_use_handle(cmd_list, tlas);
			continue;
		}
		rbtrend_blas blas = command._params._build_blas._blas;
		rbtrend_cmd_use_handle(cmd_list, blas);
		rbtrend_rt_blas_settle(blas);
		const RBTREND_BUILD_MODE mode = command._params._build_blas._mode;
		if (mode == RBTREND_BUILD_MODE_REFIT && !blas->built && blas->built_serial == 0 &&
		    !rt->blas_built[rbtrend_rt_slot_index(blas->slot)]) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BUILD_BLAS: RBTREND_BUILD_MODE_REFIT of a BLAS never built");
			return false;
		}
		if (mode == RBTREND_BUILD_MODE_COMPACT && blas->compacted_size == 0) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BUILD_BLAS: RBTREND_BUILD_MODE_COMPACT of a BLAS whose compacted size isn't known yet (rbtrend_blas_get_info)");
			return false;
		}
		if (mode == RBTREND_BUILD_MODE_COMPACT && !blas->full) {
			rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BUILD_BLAS: RBTREND_BUILD_MODE_COMPACT of a BLAS already compacted");
			return false;
		}
	}

	// The structures each command writes: a COMPACT's compacted one, and a
	// full-size one again for a BUILD of a compacted BLAS (T4), made now.
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_BLAS)
			continue;
		cmd_list->report_send = cmd_list->command_sends[i];
		rbtrend_blas blas      = command._params._build_blas._blas;
		const uint32_t index  = rbtrend_rt_slot_index(blas->slot);
		switch (command._params._build_blas._mode) {
		case RBTREND_BUILD_MODE_COMPACT:
			if (!rbtrend_rt_blas_retire_pending(blas) || !rbtrend_rt_blas_make_compact(blas))
				return false;
			blas->retiring            = blas->full;
			blas->retiring_serial     = 0;
			blas->full                = nullptr;
			rt->blas_structure[index] = blas->compact;
			break;
		case RBTREND_BUILD_MODE_BUILD:
			// Its compacted size is this build's, read back once it has run.
			if (!blas->deforms)
				blas->compacted_size = 0;
			if (!blas->full) {
				if (!rbtrend_rt_blas_retire_pending(blas) || !rbtrend_rt_blas_make_full(blas))
					return false;
				blas->retiring            = blas->compact;
				blas->retiring_serial     = 0;
				blas->compact             = nullptr;
				rt->blas_structure[index] = blas->full;
			}
			break;
		default:
			break;
		}
	}

	// The copies: the BLAS table and the arena, and each TLAS's current
	// records and material table, into COPY_DEST; the geometries' buffers
	// into COPY_SOURCE.
	rbtrend_cmd_use_store(cmd_list, table);
	rbtrend_cmd_use_store(cmd_list, arena);
	rbtrend_cmd_transition_store(cmd_list, table, CRGPU_RESOURCE_STATE_COPY_DEST);
	rbtrend_cmd_transition_store(cmd_list, arena, CRGPU_RESOURCE_STATE_COPY_DEST);
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type == RBTREND_COMMAND_BUILD_TLAS)
			continue;
		const auto &p = command._params._build_blas;
		if (p._mode == RBTREND_BUILD_MODE_COMPACT)
			continue;
		for (uint32_t g = 0; g < p._blas->geometry_count; ++g) {
			const rbtrend_rt_geometry &geometry = p._blas->geometries[g];
			rbtrend_cmd_rt_source(cmd_list, &geometry.vertices->parts[0].store, CRGPU_RESOURCE_STATE_COPY_SOURCE);
			if (geometry.indices)
				rbtrend_cmd_rt_source(cmd_list, &geometry.indices->parts[0].store, CRGPU_RESOURCE_STATE_COPY_SOURCE);
		}
	}
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		cmd_list->report_send         = cmd_list->command_sends[i];
		if (command._type == RBTREND_COMMAND_BUILD_TLAS)
			continue;
		const auto &p = command._params._build_blas;
		rbtrend_blas blas     = p._blas;
		const uint32_t index = rbtrend_rt_slot_index(blas->slot);
		// The table entry: the structure every TLAS built after this reads,
		// and where the BLAS's geometries are in the arena (D5).
		if (p._mode != RBTREND_BUILD_MODE_REFIT) {
			uint64_t reference = 0;
			(void)crgpu_acceleration_structure_get_reference(rt->blas_structure[index], &reference);
			rbtrend_rt_blas_entry entry{};
			entry.reference[0]   = (uint32_t)reference;
			entry.reference[1]   = (uint32_t)(reference >> 32);
			entry.arena_word     = (uint32_t)((uint64_t)blas->arena_offset * RBTREND_RT_ARENA_UNIT / 4);
			entry.geometry_count = blas->geometry_count;
			if (!rbtrend_cmd_rt_put(cmd_list, table, (uint64_t)index * sizeof(entry), &entry, sizeof(entry)))
				return false;
		}
		if (p._mode == RBTREND_BUILD_MODE_COMPACT)
			continue;
		// The arena (card A41): the headers at a BUILD, the vertices at a BUILD
		// or a REFIT, the indices at a BUILD.
		if (p._mode == RBTREND_BUILD_MODE_BUILD) {
			rbtrend_rt_geometry_header headers[16];
			const uint64_t header_base = (uint64_t)blas->arena_offset * RBTREND_RT_ARENA_UNIT;
			for (uint32_t g = 0; g < blas->geometry_count; g += 16) {
				const uint32_t n = blas->geometry_count - g < 16 ? blas->geometry_count - g : 16;
				for (uint32_t k = 0; k < n; ++k) {
					const rbtrend_rt_geometry &geometry = blas->geometries[g + k];
					headers[k].vertex_word             = (uint32_t)(geometry.arena_vertices / 4);
					headers[k].index_word              = geometry.indices ? (uint32_t)(geometry.arena_indices / 4) : UINT32_MAX;
					headers[k].triangle_count          = geometry.triangle_count;
					headers[k].indexed                 = geometry.indices ? 1u : 0u;
				}
				if (!rbtrend_cmd_rt_put(cmd_list, arena, header_base + (uint64_t)g * sizeof(rbtrend_rt_geometry_header), headers, n * sizeof(rbtrend_rt_geometry_header)))
					return false;
			}
		}
		for (uint32_t g = 0; g < blas->geometry_count; ++g) {
			const rbtrend_rt_geometry &geometry = blas->geometries[g];
			crgpu_command_list_copy_buffer(cmd, geometry.vertices->parts[0].store.buffer, arena->buffer, geometry.vertex_offset, geometry.arena_vertices,
			                        (uint64_t)geometry.vertex_count * RBTREND_RT_VERTEX_STRIDE);
			if (geometry.indices && p._mode == RBTREND_BUILD_MODE_BUILD)
				crgpu_command_list_copy_buffer(cmd, geometry.indices->parts[0].store.buffer, arena->buffer, geometry.index_offset, geometry.arena_indices,
				                        (uint64_t)geometry.index_count * 4);
		}
	}

	// The builds read the geometries; scratch is a build's.
	rbtrend_buffer_store *scratch = &rt->scratch->parts[0].store;
	rbtrend_cmd_use_store(cmd_list, scratch);
	// A run before this one in the list wrote the same scratch ranges: a
	// SCRATCH to SCRATCH barrier orders the two (card S25).
	bool scratch_tracked = false;
	for (const auto &tracked : cmd_list->buffer_states)
		if (tracked.store == scratch && tracked.state == CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH)
			scratch_tracked = true;
	if (scratch_tracked) {
		crgpu_buffer_barrier barrier{{CR_STRUCT_TYPE_GPU_BUFFER_BARRIER, nullptr}, scratch->buffer, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH,
		                               CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH};
		crgpu_command_list_pipeline_barrier(cmd, &barrier, 1, nullptr, 0);
	}
	rbtrend_cmd_transition_store(cmd_list, scratch, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SCRATCH);
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_BLAS || command._params._build_blas._mode == RBTREND_BUILD_MODE_COMPACT)
			continue;
		const rbtrend_blas blas = command._params._build_blas._blas;
		for (uint32_t g = 0; g < blas->geometry_count; ++g) {
			rbtrend_cmd_rt_source(cmd_list, &blas->geometries[g].vertices->parts[0].store, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT);
			if (blas->geometries[g].indices)
				rbtrend_cmd_rt_source(cmd_list, &blas->geometries[g].indices->parts[0].store, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_BUILD_INPUT);
		}
	}

	// The compactions, from structures built by earlier submissions.
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_BLAS || command._params._build_blas._mode != RBTREND_BUILD_MODE_COMPACT)
			continue;
		rbtrend_blas blas = command._params._build_blas._blas;
		rbtrend_cmd_rt_access(cmd_list, blas->retiring, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
		rbtrend_cmd_rt_access(cmd_list, blas->compact, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
		cmd_list->report_send = cmd_list->command_sends[i];
		const CR_RESULT sr = crgpu_command_list_copy_acceleration_structure(cmd, blas->retiring, blas->compact, CRGPU_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
		if (CR_FAILED(sr)) {
			rbtrend_cmd_report(cmd_list, rbtrend_result_from_cr(sr), "BUILD_BLAS: the compaction can't be recorded (%s)", cr_result_str(sr));
			return false;
		}
	}

	// Every BLAS build and refit of the run in one call (T8), each its own
	// range of the scratch.
	const uint64_t alignment = rt->limits.scratch_offset_alignment ? rt->limits.scratch_offset_alignment : 256;
	uint64_t scratch_at      = 0;
	eq::array<crgpu_acceleration_structure_build> &builds     = cmd_list->rt_builds;
	eq::array<crgpu_acceleration_structure_inputs> &inputs    = cmd_list->rt_inputs;
	eq::array<crgpu_acceleration_structure_geometry> &geometries = cmd_list->rt_geometries;
	builds.clear();
	inputs.clear();
	geometries.clear();
	// Room for every input and geometry of the run first: the builds point
	// into these arrays.
	size_t geometry_total = 0;
	for (size_t i = first; i < last; ++i)
		if (cmd_list->commands[i]._type == RBTREND_COMMAND_BUILD_BLAS)
			geometry_total += cmd_list->commands[i]._params._build_blas._blas->geometry_count;
	if (inputs.reserve(last - first) != EQ_SUCCESS || builds.reserve(last - first) != EQ_SUCCESS || geometries.reserve(geometry_total) != EQ_SUCCESS)
		throw std::bad_alloc();
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_BLAS || command._params._build_blas._mode == RBTREND_BUILD_MODE_COMPACT)
			continue;
		rbtrend_blas blas         = command._params._build_blas._blas;
		const bool refit         = command._params._build_blas._mode == RBTREND_BUILD_MODE_REFIT;
		const size_t first_geom  = geometries.size();
		(void)geometries.resize(first_geom + blas->geometry_count);
		crgpu_acceleration_structure_inputs in{};
		rbtrend_rt_blas_describe(blas, geometries.data + first_geom, &in);
		rbtrend_push(inputs, in);
		rbtrend_cmd_rt_access(cmd_list, blas->full, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_WRITE);
		scratch_at = (scratch_at + alignment - 1) / alignment * alignment;
		crgpu_acceleration_structure_build build{};
		build.base.type      = CR_STRUCT_TYPE_GPU_ACCELERATION_STRUCTURE_BUILD;
		build.mode           = refit ? CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_UPDATE : CRGPU_ACCELERATION_STRUCTURE_BUILD_MODE_BUILD;
		build.inputs         = &inputs[inputs.size() - 1];
		build.source         = refit ? blas->full : nullptr;
		build.destination    = blas->full;
		build.scratch        = scratch->buffer;
		build.scratch_offset = scratch_at;
		rbtrend_push(builds, build);
		scratch_at += refit ? blas->update_scratch : blas->build_scratch;
	}
	if (!builds.empty()) {
		const CR_RESULT sr = crgpu_command_list_build_acceleration_structures(cmd, builds.data, (uint32_t)builds.size());
		if (CR_FAILED(sr)) {
			rbtrend_cmd_report(cmd_list, rbtrend_result_from_cr(sr), "BUILD_BLAS: the builds can't be recorded (%s)", cr_result_str(sr));
			return false;
		}
	}
	const size_t blas_build_count = builds.size();

	// A static BLAS's compacted size, written once its build is done and
	// read back once its submission has completed (T4).
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_BLAS || command._params._build_blas._mode != RBTREND_BUILD_MODE_BUILD)
			continue;
		rbtrend_blas blas = command._params._build_blas._blas;
		if (blas->deforms)
			continue;
		rbtrend_buffer_store *sizes = &blas->size_device->parts[0].store;
		rbtrend_cmd_use_store(cmd_list, sizes);
		rbtrend_cmd_rt_access(cmd_list, blas->full, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
		rbtrend_cmd_transition_store(cmd_list, sizes, CRGPU_RESOURCE_STATE_ACCELERATION_STRUCTURE_SIZE_DEST);
		CR_RESULT sr = crgpu_command_list_write_acceleration_structure_compacted_sizes(cmd, &blas->full, 1, sizes->buffer, 0);
		if (CR_FAILED(sr)) {
			rbtrend_cmd_report(cmd_list, rbtrend_result_from_cr(sr), "BUILD_BLAS: the compacted size can't be written (%s)", cr_result_str(sr));
			return false;
		}
		rbtrend_cmd_transition_store(cmd_list, sizes, CRGPU_RESOURCE_STATE_COPY_SOURCE);
		crgpu_command_list_copy_buffer(cmd, sizes->buffer, blas->size_readback, 0, 0, sizeof(uint64_t));
	}

	// The tables are read now: by the writers, and by later traces.
	rbtrend_cmd_transition_store(cmd_list, table, table->use_state);
	rbtrend_cmd_transition_store(cmd_list, arena, arena->use_state);
	bool any_tlas = false;
	for (size_t i = first; i < last; ++i)
		if (cmd_list->commands[i]._type == RBTREND_COMMAND_BUILD_TLAS)
			any_tlas = true;
	if (any_tlas) {
		// A structure built earlier in the list and traced since is read by
		// these builds as a bottom level: ordered after its build.
		for (size_t k = 0; k < cmd_list->rt_accesses.size(); ++k)
			if (cmd_list->rt_accesses[k].access == CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE)
				rbtrend_cmd_rt_access(cmd_list, cmd_list->rt_accesses[k].structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
		// One barrier between the BLASes and the TLASes (T8).
		for (size_t i = first; i < last; ++i) {
			const RBTREND_COMMAND &command = cmd_list->commands[i];
			if (command._type == RBTREND_COMMAND_BUILD_BLAS) {
				rbtrend_blas blas = command._params._build_blas._blas;
				rbtrend_cmd_rt_access(cmd_list, blas->full ? blas->full : blas->compact, CRGPU_ACCELERATION_STRUCTURE_ACCESS_BUILD_READ);
			}
		}
		// The TLASes in groups in which none repeats: a TLAS built twice in
		// a run writes its current records and its structure twice, in the
		// order sent, so its second build is a group of its own. A group is
		// one build call (T8).
		size_t at = first;
		while (at < last) {
			size_t end = at;
			for (; end < last; ++end) {
				const RBTREND_COMMAND &command = cmd_list->commands[end];
				if (command._type != RBTREND_COMMAND_BUILD_TLAS)
					continue;
				bool repeated = false;
				for (size_t k = at; k < end; ++k)
					if (cmd_list->commands[k]._type == RBTREND_COMMAND_BUILD_TLAS &&
					    cmd_list->commands[k]._params._build_tlas._tlas == command._params._build_tlas._tlas)
						repeated = true;
				if (repeated)
					break;
			}
			if (!rbtrend_cmd_rt_tlas_group(cmd_list, at, end, &scratch_at))
				return false;
			at = end;
		}
	}
	// What was built is traced from here on; the geometries' buffers go
	// back to the state their draws read them in.
	for (size_t i = first; i < last; ++i) {
		const RBTREND_COMMAND &command = cmd_list->commands[i];
		if (command._type != RBTREND_COMMAND_BUILD_BLAS)
			continue;
		rbtrend_blas blas = command._params._build_blas._blas;
		rbtrend_cmd_rt_access(cmd_list, blas->full ? blas->full : blas->compact, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
		if (command._params._build_blas._mode == RBTREND_BUILD_MODE_COMPACT)
			continue;
		for (uint32_t g = 0; g < blas->geometry_count; ++g) {
			rbtrend_buffer_store *vertices = &blas->geometries[g].vertices->parts[0].store;
			rbtrend_cmd_rt_source(cmd_list, vertices, vertices->use_state);
			if (blas->geometries[g].indices) {
				rbtrend_buffer_store *indices = &blas->geometries[g].indices->parts[0].store;
				rbtrend_cmd_rt_source(cmd_list, indices, indices->use_state);
			}
		}
	}
	(void)blas_build_count;
	return true;
}


/* [view]'s size at its mip. */
static void rbtrend_cmd_rt_view_size(rbtrend_texture_view view, uint32_t *out_width, uint32_t *out_height) {
	*out_width  = rbtrend_mip_size(view->texture._t2d->desc.width, view->desc.base_mip);
	*out_height = rbtrend_mip_size(view->texture._t2d->desc.height, view->desc.base_mip);
}

/* What a trace of [scene] through [tlas] at [width] by [height] needs before
 * it is recorded, [who] the command: a TLAS built, the scene's view of that
 * target size with a view_projection that can be inverted, and the scene's
 * block copied by the submission. False, reported, if not. */
static bool rbtrend_cmd_rt_trace_ready(rbtrend_command_list cmd_list, const char *who, rbtrend_lit_scene scene, rbtrend_tlas tlas, uint32_t width, uint32_t height) {
	if (!tlas->sent_build) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s: the TLAS has never been built (no BUILD_TLAS of it sent)", who);
		return false;
	}
	if ((float)width != scene->block.target_size[0] || (float)height != scene->block.target_size[1]) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s: the target is %ux%u, and the scene's view's target %gx%g", who, width, height,
		                  (double)scene->block.target_size[0], (double)scene->block.target_size[1]);
		return false;
	}
	// rbtrend_lit_scene_set_view() leaves the inverse 0 where the matrix has
	// none.
	bool inverse = false;
	for (float v : scene->block.inverse_view_projection)
		inverse = inverse || v != 0.0f;
	if (!inverse) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s: the scene's view_projection can't be inverted", who);
		return false;
	}
	rbtrend_cmd_use_handle(cmd_list, tlas);
	return rbtrend_cmd_use_material(cmd_list, scene->material);
}

/* One traced dispatch of [pipeline] over [sets] (the scene's, the TLAS's,
 * the outputs'), [push] its parameters, [groups] its groups: each set's
 * resources made ready as any dispatch's are, the TLAS moved to be traced.
 * False, reported, if a set can't be read. Throws std::bad_alloc. */
static bool rbtrend_cmd_rt_dispatch(rbtrend_command_list cmd_list, rbtrend_compute_pipeline pipeline, rbtrend_tlas tlas, rbtrend_binding_set const (&sets)[3],
                                   const void *push, uint32_t push_size, uint32_t groups_x, uint32_t groups_y) {
	eq::array<rbtrend_pass_image> &images = cmd_list->pass_images;
	eq::array<rbtrend_pass_store> &stores = cmd_list->pass_stores;
	images.clear();
	stores.clear();
	for (uint32_t slot = 0; slot < 3; ++slot) {
		if (!rbtrend_pass_images_add_set(cmd_list, images, sets[slot], slot, pipeline->storage_writes))
			return false;
		rbtrend_storage_stores_add_set(stores, sets[slot], cmd_list->compiled_frame, slot, pipeline->storage_writes);
		rbtrend_cmd_use_handle(cmd_list, sets[slot]);
	}
	for (const rbtrend_pass_store &store : stores)
		rbtrend_cmd_use_store(cmd_list, store.store);
	rbtrend_cmd_ready_set_resources(cmd_list, images, stores);
	rbtrend_cmd_rt_access(cmd_list, tlas->structure, CRGPU_ACCELERATION_STRUCTURE_ACCESS_TRACE);
	crgpu_command_list cmd = cmd_list->cmd_list;
	crgpu_command_list_bind_compute_pipeline(cmd, pipeline->pipeline);
	for (uint32_t slot = 0; slot < 3; ++slot)
		crgpu_command_list_bind_compute_descriptor_sets(cmd, pipeline->pipeline, slot, &sets[slot]->sets[rbtrend_binding_set_frame_slot(sets[slot], cmd_list->compiled_frame)], 1);
	crgpu_command_list_push_compute_constants(cmd, pipeline->pipeline, 0, push_size, push);
	crgpu_command_list_dispatch(cmd, groups_x, groups_y, 1);
	return true;
}

/* LIT_SHADOW_MASK, outside a pass (T9): the mask traced over the depth and
 * written into the target, its write ordered before a later read of it.
 * False, reported, if the depth has nothing in it, the scene's view doesn't
 * fit it, the TLAS was never built, or a pipeline or set can't be made. */
static bool rbtrend_cmd_rt_shadow_mask(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p            = command._params._lit_shadow_mask;
	rbtrend_instance instance = cmd_list->instance;
	rbtrend_rt_state *rt      = instance->rt;
	if (!rbtrend_cmd_range_written(cmd_list, rbtrend_texture_view_range(p._depth))) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_SHADOW_MASK: the depth has nothing in it, from before the list or earlier in it");
		return false;
	}
	uint32_t width = 0, height = 0;
	rbtrend_cmd_rt_view_size(p._depth, &width, &height);
	if (!rbtrend_cmd_rt_trace_ready(cmd_list, "LIT_SHADOW_MASK", p._scene, p._tlas, width, height))
		return false;
	// Its place among the scene's directional lights, as the scene holds
	// them now: the lights may have changed since the send.
	const uint32_t slot = rbtrend_lit_scene_directional_slot(p._scene, p._light);
	if (slot == UINT32_MAX) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_SHADOW_MASK: _light %u names no directional light of the scene at the compile", p._light);
		return false;
	}
	rbtrend_compute_pipeline pipeline = rbtrend_rt_mask_pipeline(instance, p._target->texture._t2d->desc.format);
	if (!pipeline)
		return false;
	RBTREND_BINDING_SET_ENTRY outputs[2]{};
	outputs[0]._binding                 = 0;
	outputs[0]._type                    = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	outputs[0]._resource._image._view   = p._depth;
	outputs[1]._binding                 = 1;
	outputs[1]._type                    = CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	outputs[1]._resource._image._view   = p._target;
	const rbtrend_texture_view others[2] = {p._depth, nullptr};
	rbtrend_binding_set sets[3]          = {rbtrend_lit_scene_rt_set(p._scene, rt->scene_layout), rbtrend_rt_tlas_trace_set(p._tlas),
	                                       rbtrend_rt_output_set(p._target, rt->mask_output_layout, others, outputs, 2)};
	if (!sets[0] || !sets[1] || !sets[2])
		return false;
	rbtrend_rt_mask_params params{};
	params.size[0]       = width;
	params.size[1]       = height;
	params.ray_count     = p._ray_count;
	params.filter_radius = p._filter_radius;
	params.cull_mask     = p._cull_mask;
	params.normal_offset = p._normal_offset;
	params.max_distance  = p._max_distance;
	params.light         = slot;
	return rbtrend_cmd_rt_dispatch(cmd_list, pipeline, p._tlas, sets, &params, sizeof(params), (width + RBTREND_RT_TRACE_GROUP - 1) / RBTREND_RT_TRACE_GROUP,
	                              (height + RBTREND_RT_TRACE_GROUP - 1) / RBTREND_RT_TRACE_GROUP);
}


/* ---- Reflections (LIT_SSR, LIT_TRACE_REFLECTIONS, LIT_REFLECT) ---------- */

/* A binding entry of [view] at [binding], of [type]. */
static RBTREND_BINDING_SET_ENTRY rbtrend_reflect_entry(uint32_t binding, CRGPU_DESCRIPTOR_TYPE type, rbtrend_texture_view view) {
	RBTREND_BINDING_SET_ENTRY entry{};
	entry._binding               = binding;
	entry._type                  = type;
	entry._resource._image._view = view;
	return entry;
}

/* A sampler entry of [sampler] at [binding]. */
static RBTREND_BINDING_SET_ENTRY rbtrend_reflect_sampler_entry(uint32_t binding, rbtrend_sampler sampler) {
	RBTREND_BINDING_SET_ENTRY entry{};
	entry._binding                  = binding;
	entry._type                     = CRGPU_DESCRIPTOR_TYPE_SAMPLER;
	entry._resource._image._sampler = sampler;
	return entry;
}

/* One dispatch of Rend's own [pipeline] over [count] [sets], untraced:
 * each set's resources made ready as any dispatch's are. False, reported,
 * if a set can't be read. Throws std::bad_alloc. */
static bool rbtrend_cmd_own_dispatch(rbtrend_command_list cmd_list, rbtrend_compute_pipeline pipeline, const rbtrend_binding_set *sets, uint32_t count,
                                    const void *push, uint32_t push_size, uint32_t groups_x, uint32_t groups_y) {
	eq::array<rbtrend_pass_image> &images = cmd_list->pass_images;
	eq::array<rbtrend_pass_store> &stores = cmd_list->pass_stores;
	images.clear();
	stores.clear();
	for (uint32_t slot = 0; slot < count; ++slot) {
		if (!rbtrend_pass_images_add_set(cmd_list, images, sets[slot], slot, pipeline->storage_writes))
			return false;
		rbtrend_storage_stores_add_set(stores, sets[slot], cmd_list->compiled_frame, slot, pipeline->storage_writes);
		rbtrend_cmd_use_handle(cmd_list, sets[slot]);
	}
	for (const rbtrend_pass_store &store : stores)
		rbtrend_cmd_use_store(cmd_list, store.store);
	rbtrend_cmd_ready_set_resources(cmd_list, images, stores);
	crgpu_command_list cmd = cmd_list->cmd_list;
	crgpu_command_list_bind_compute_pipeline(cmd, pipeline->pipeline);
	for (uint32_t slot = 0; slot < count; ++slot)
		crgpu_command_list_bind_compute_descriptor_sets(cmd, pipeline->pipeline, slot, &sets[slot]->sets[rbtrend_binding_set_frame_slot(sets[slot], cmd_list->compiled_frame)], 1);
	crgpu_command_list_push_compute_constants(cmd, pipeline->pipeline, 0, push_size, push);
	crgpu_command_list_dispatch(cmd, groups_x, groups_y, 1);
	return true;
}

/* What every reflection command needs of its views and scene before it is
 * recorded, [who] the command: [views] (NULL-terminated, [names] theirs)
 * each written earlier in the list or before it; the scene's view of
 * [depth]'s size with a view_projection that can be inverted; [reflection]
 * still half [depth]'s size, rounded up (a resize since the send may have
 * changed either). Its size through [out_width] and [out_height]. False,
 * reported, if not. */
static bool rbtrend_cmd_reflect_ready(rbtrend_command_list cmd_list, const char *who, rbtrend_lit_scene scene, const rbtrend_texture_view *views,
                                     const char *const *names, rbtrend_texture_view depth, rbtrend_texture_view reflection, uint32_t *out_width,
                                     uint32_t *out_height) {
	for (uint32_t i = 0; views[i]; ++i) {
		if (!rbtrend_cmd_range_written(cmd_list, rbtrend_texture_view_range(views[i])))
			return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s: the %s has nothing in it, from before the list or earlier in it", who, names[i]);
	}
	uint32_t width = 0, height = 0, w = 0, h = 0;
	rbtrend_reflect_view_size(depth, &width, &height);
	rbtrend_reflect_view_size(reflection, &w, &h);
	if (w != (width + 1) / 2 || h != (height + 1) / 2)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s: the reflection is %ux%u, no longer half the depth's %ux%u rounded up", who, w, h, width, height);
	if ((float)width != scene->block.target_size[0] || (float)height != scene->block.target_size[1])
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s: the depth is %ux%u, and the scene's view's target %gx%g", who, width, height,
		                         (double)scene->block.target_size[0], (double)scene->block.target_size[1]);
	// rbtrend_lit_scene_set_view() leaves the inverse 0 where the matrix has
	// none.
	bool inverse = false;
	for (float v : scene->block.inverse_view_projection)
		inverse = inverse || v != 0.0f;
	if (!inverse)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s: the scene's view_projection can't be inverted", who);
	*out_width  = width;
	*out_height = height;
	return true;
}

/* LIT_SSR, outside a pass: the Hi-Z pyramid of the depth into a pooled
 * texture, level 0 the depth's nearness and each level below the least of
 * the one above, then one dispatch marching a texel of the reflection a
 * thread. False, reported, if a view has nothing in it, the scene's view
 * doesn't fit the depth, or a pipeline, set or the pooled texture can't be
 * made. */
static bool rbtrend_cmd_lit_ssr(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p            = command._params._lit_ssr;
	rbtrend_instance instance = cmd_list->instance;
	rbtrend_lit_builtins &lit = instance->builtins.lit;
	const rbtrend_texture_view read[4] = {p._depth, p._normal_roughness, p._color, nullptr};
	const char *const names[3]        = {"depth", "normal and roughness", "color"};
	uint32_t width = 0, height = 0;
	if (!rbtrend_cmd_reflect_ready(cmd_list, "LIT_SSR", p._scene, read, names, p._depth, p._reflection, &width, &height))
		return false;
	rbtrend_compute_pipeline pipeline = rbtrend_lit_ssr_pipeline(instance);
	rbtrend_graphics_pipeline first   = rbtrend_lit_pass_pipeline(instance, RBTREND_LIT_PASS_HIZ);
	rbtrend_graphics_pipeline down    = rbtrend_lit_pass_pipeline(instance, RBTREND_LIT_PASS_HIZ_DOWN);
	if (!pipeline || !first || !down)
		return false;
	if (!rbtrend_cmd_use_material(cmd_list, p._scene->material))
		return false;
	// Every level down to a texel: the march climbs as far as it can.
	uint32_t levels = 1;
	while ((width >> levels) > 0 || (height >> levels) > 0)
		++levels;
	rbtrend_pooled_target *pyramid = rbtrend_render_target_take(cmd_list, width, height, RBTREND_HIZ_FORMAT, levels);
	if (!pyramid)
		return false;
	rbtrend_texture_view whole     = rbtrend_pooled_target_whole(instance, pyramid);
	rbtrend_binding_set depth_set  = rbtrend_texture_view_environment_set(p._depth);
	rbtrend_binding_set scene_set  = rbtrend_lit_scene_ssr_set(p._scene);
	if (!whole || !depth_set || !scene_set)
		return false;
	const bool reversed = rbtrend_lit_scene_depth_reversed(p._scene);

	rbtrend_cmd_transition_range(cmd_list, rbtrend_texture_view_range(p._depth), CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, true);
	rbtrend_hiz_params hiz{};
	hiz.target_size[0] = (float)width;
	hiz.target_size[1] = (float)height;
	hiz.reversed       = reversed ? 1u : 0u;
	rbtrend_cmd_own_pass(cmd_list, first, pyramid->level_targets[0], width, height, &depth_set, 1, &hiz, sizeof(hiz));
	for (uint32_t level = 1; level < levels; ++level) {
		rbtrend_binding_set above = rbtrend_texture_view_environment_set(pyramid->level_sources[level - 1]);
		if (!above)
			return false;
		rbtrend_size_params size{};
		size.target_size[0] = (float)rbtrend_mip_size(width, level);
		size.target_size[1] = (float)rbtrend_mip_size(height, level);
		rbtrend_cmd_own_pass(cmd_list, down, pyramid->level_targets[level], rbtrend_mip_size(width, level), rbtrend_mip_size(height, level), &above, 1, &size,
		                    sizeof(size));
	}

	const rbtrend_texture_view views[5]         = {p._depth, p._normal_roughness, p._color, whole, p._reflection};
	const RBTREND_BINDING_SET_ENTRY entries[6] = {rbtrend_reflect_entry(0, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, p._depth),
	                                             rbtrend_reflect_entry(1, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, p._normal_roughness),
	                                             rbtrend_reflect_entry(2, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, p._color),
	                                             rbtrend_reflect_entry(3, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, whole),
	                                             rbtrend_reflect_sampler_entry(4, lit.environment_sampler),
	                                             rbtrend_reflect_entry(5, CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE, p._reflection)};
	rbtrend_binding_set inputs = rbtrend_view_set_get(instance, lit.view_sets, lit.ssr_input_layout, views, 5, entries, 6, "screen-space reflections");
	if (!inputs)
		return false;
	rbtrend_ssr_params params{};
	rbtrend_reflect_view_size(p._reflection, &params.size[0], &params.size[1]);
	params.full[0]       = width;
	params.full[1]       = height;
	params.max_steps     = p._max_steps;
	params.max_distance  = p._max_distance;
	params.thickness     = p._thickness;
	params.edge_fade     = p._edge_fade;
	params.max_roughness = p._max_roughness;
	params.hiz_levels    = levels;
	params.reversed      = reversed ? 1u : 0u;
	params.color_mips    = (float)p._color->desc.mip_count;
	const rbtrend_binding_set sets[2] = {scene_set, inputs};
	return rbtrend_cmd_own_dispatch(cmd_list, pipeline, sets, 2, &params, sizeof(params), (params.size[0] + 7) / 8, (params.size[1] + 7) / 8);
}

/* LIT_TRACE_REFLECTIONS, outside a pass: one traced dispatch, a texel of the
 * reflection a thread. False, reported, if a view has nothing in it, the
 * TLAS was never built, the scene's view doesn't fit the depth, or a
 * pipeline or set can't be made. */
static bool rbtrend_cmd_rt_reflections(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p            = command._params._lit_trace_reflections;
	rbtrend_instance instance = cmd_list->instance;
	rbtrend_rt_state *rt      = instance->rt;
	rbtrend_lit_builtins &lit = instance->builtins.lit;
	const rbtrend_texture_view read[4] = {p._depth, p._normal_roughness, p._reflection, nullptr};
	const char *const names[3]        = {"depth", "normal and roughness", "reflection"};
	uint32_t width = 0, height = 0;
	if (!rbtrend_cmd_reflect_ready(cmd_list, "LIT_TRACE_REFLECTIONS", p._scene, read, names, p._depth, p._reflection, &width, &height))
		return false;
	if (!rbtrend_cmd_rt_trace_ready(cmd_list, "LIT_TRACE_REFLECTIONS", p._scene, p._tlas, width, height))
		return false;
	rbtrend_compute_pipeline pipeline = rbtrend_rt_reflection_pipeline(instance);
	if (!pipeline)
		return false;
	// A hit reads the lit-material table: the submission copies the
	// entries staged since (D4).
	cmd_list->rt_reads_lit = true;
	rbtrend_cmd_use_store(cmd_list, &rt->lit_table->parts[0].store);
	// The scene's environment, or the placeholder cube where it has none
	// (the block's flag says so); the BRDF lookup texture, which the
	// compile's preparation of the scene has filled.
	rbtrend_lit_environment env      = p._scene->environment;
	rbtrend_texture_view specular    = env ? env->specular_cube : lit.placeholder_cube_view;
	rbtrend_texture_view irradiance  = env ? env->irradiance_cube : lit.placeholder_cube_view;
	const rbtrend_texture_view views[6]         = {p._depth, p._normal_roughness, p._reflection, specular, irradiance, lit.brdf_lut_view};
	const RBTREND_BINDING_SET_ENTRY entries[7] = {rbtrend_reflect_entry(0, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, p._depth),
	                                             rbtrend_reflect_entry(1, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, p._normal_roughness),
	                                             rbtrend_reflect_entry(2, CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE, p._reflection),
	                                             rbtrend_reflect_entry(3, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, specular),
	                                             rbtrend_reflect_entry(4, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, irradiance),
	                                             rbtrend_reflect_entry(5, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, lit.brdf_lut_view),
	                                             rbtrend_reflect_sampler_entry(6, lit.environment_sampler)};
	rbtrend_binding_set sets[3] = {rbtrend_lit_scene_rt_set(p._scene, rt->scene_layout), rbtrend_rt_tlas_trace_set(p._tlas),
	                              rbtrend_view_set_get(instance, rt->reflection_sets, rt->reflection_output_layout, views, 6, entries, 7, "the traced reflections")};
	if (!sets[0] || !sets[1] || !sets[2])
		return false;
	rbtrend_rt_reflection_params params{};
	rbtrend_reflect_view_size(p._reflection, &params.size[0], &params.size[1]);
	params.full[0]        = width;
	params.full[1]        = height;
	params.ray_count      = p._ray_count;
	params.cull_mask      = p._cull_mask;
	params.max_distance   = p._max_distance;
	params.max_roughness  = p._max_roughness;
	params.material_count = p._tlas->max_materials;
	params.reversed       = rbtrend_lit_scene_depth_reversed(p._scene) ? 1u : 0u;
	return rbtrend_cmd_rt_dispatch(cmd_list, pipeline, p._tlas, sets, &params, sizeof(params), (params.size[0] + RBTREND_RT_TRACE_GROUP - 1) / RBTREND_RT_TRACE_GROUP,
	                              (params.size[1] + RBTREND_RT_TRACE_GROUP - 1) / RBTREND_RT_TRACE_GROUP);
}

/* LIT_REFLECT, outside a pass: one full-target pass over the picture, added
 * onto it, reading the scene's own set and the targets. False, reported, if
 * a view has nothing in it, the scene's view doesn't fit or writes encoded
 * color, or a pipeline or set can't be made. */
static bool rbtrend_cmd_lit_reflect(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p            = command._params._lit_reflect;
	rbtrend_instance instance = cmd_list->instance;
	rbtrend_lit_builtins &lit = instance->builtins.lit;
	const rbtrend_texture_view read[6] = {p._depth, p._normal_roughness, p._specular, p._reflection, p._target, nullptr};
	const char *const names[5]        = {"depth", "normal and roughness", "specular", "reflection", "target"};
	uint32_t width = 0, height = 0;
	if (!rbtrend_cmd_reflect_ready(cmd_list, "LIT_REFLECT", p._scene, read, names, p._depth, p._reflection, &width, &height))
		return false;
	if (p._scene->block.output_encoding != RBTREND_COLOR_ENCODING_LINEAR)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_REFLECT: the scene writes its color encoded, and the picture it reflects onto is linear");
	uint32_t tw = 0, th = 0;
	rbtrend_reflect_view_size(p._target, &tw, &th);
	if (tw != width || th != height)
		return rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_REFLECT: the target is %ux%u, no longer the depth's %ux%u", tw, th, width, height);
	rbtrend_graphics_pipeline pipeline = rbtrend_lit_reflect_pipeline(instance, p._target->texture._t2d->desc.format);
	if (!pipeline)
		return false;
	if (!rbtrend_cmd_use_material(cmd_list, p._scene->material))
		return false;
	const rbtrend_texture_view views[4]         = {p._depth, p._normal_roughness, p._specular, p._reflection};
	const RBTREND_BINDING_SET_ENTRY entries[4] = {rbtrend_reflect_entry(0, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, p._depth),
	                                             rbtrend_reflect_entry(1, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, p._normal_roughness),
	                                             rbtrend_reflect_entry(2, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, p._specular),
	                                             rbtrend_reflect_entry(3, CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE, p._reflection)};
	rbtrend_binding_set inputs = rbtrend_view_set_get(instance, lit.view_sets, lit.reflect_input_layout, views, 4, entries, 4, "the reflection composite");
	if (!inputs)
		return false;
	// What both sets hold made ready to be read, as a pass's are: the
	// scene's textures and the reflection the march or the trace wrote.
	const rbtrend_binding_set sets[2]     = {p._scene->material->set, inputs};
	eq::array<rbtrend_pass_image> &images = cmd_list->pass_images;
	eq::array<rbtrend_pass_store> &stores = cmd_list->pass_stores;
	images.clear();
	stores.clear();
	for (uint32_t slot = 0; slot < 2; ++slot) {
		if (!rbtrend_pass_images_add_set(cmd_list, images, sets[slot], slot, pipeline->storage_writes))
			return false;
		rbtrend_storage_stores_add_set(stores, sets[slot], cmd_list->compiled_frame, slot, pipeline->storage_writes);
		rbtrend_cmd_use_handle(cmd_list, sets[slot]);
	}
	for (const rbtrend_pass_store &store : stores)
		rbtrend_cmd_use_store(cmd_list, store.store);
	rbtrend_cmd_ready_set_resources(cmd_list, images, stores);
	rbtrend_reflect_params params{};
	params.target_size[0] = (float)width;
	params.target_size[1] = (float)height;
	params.reversed       = rbtrend_lit_scene_depth_reversed(p._scene) ? 1u : 0u;
	rbtrend_cmd_transition_view(cmd_list, p._target, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	rbtrend_record_fullscreen_pass_sets(cmd_list->cmd_list, pipeline, p._target->image_view, width, height, CRGPU_LOAD_OP_LOAD, sets, 2,
	                                   cmd_list->compiled_frame, &params, sizeof(params));
	return true;
}

/* LIT_PATH_TRACE, outside a pass (T10, T11): the TLAS's emitter table built
 * first if anything it reads changed since it was (D6), then one dispatch
 * adding _samples paths a pixel to the target's running mean, and the
 * guides'. False, reported, if the cull mask lets in an instance that
 * reaches a NULL material table entry (T7), the scene's view doesn't fit
 * the target, the TLAS was never built, or a pipeline or set can't be
 * made. */
static bool rbtrend_cmd_rt_path_trace(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p            = command._params._lit_path_trace;
	rbtrend_instance instance = cmd_list->instance;
	rbtrend_rt_state *rt      = instance->rt;
	rbtrend_tlas tlas         = p._tlas;
	uint32_t width = 0, height = 0;
	rbtrend_cmd_rt_view_size(p._target, &width, &height);
	if (!rbtrend_cmd_rt_trace_ready(cmd_list, "LIT_PATH_TRACE", p._scene, tlas, width, height))
		return false;
	const uint32_t null_instance = rbtrend_rt_null_instance(tlas, p._cull_mask);
	if (null_instance != UINT32_MAX) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT,
		                  "LIT_PATH_TRACE: instance %u reaches a NULL entry of the TLAS's material table (one only shadows see), and _cull_mask 0x%x lets it in",
		                  null_instance, p._cull_mask);
		return false;
	}
	rbtrend_texture_view albedo = p._albedo ? p._albedo : rbtrend_rt_no_guide(instance);
	rbtrend_texture_view normal = p._normal ? p._normal : rbtrend_rt_no_guide(instance);
	if (!albedo || !normal)
		return false;
	rbtrend_compute_pipeline pipeline = rbtrend_rt_path_pipeline(instance, albedo->texture._t2d->desc.format, normal->texture._t2d->desc.format);
	if (!pipeline)
		return false;
	// A path trace reads the lit-material table: its submission copies the
	// entries staged since (D4).
	cmd_list->rt_reads_lit = true;
	rbtrend_cmd_use_store(cmd_list, &rt->lit_table->parts[0].store);

	// The emitter table, rebuilt only when what it reads changed (D6); taken
	// as current once this list is submitted.
	if (tlas->emitters_content != tlas->content_version || tlas->emitters_lit != rt->lit_version) {
		rbtrend_compute_pipeline emitters = rbtrend_rt_emitter_pipeline(instance);
		if (!emitters)
			return false;
		rbtrend_binding_set set = rbtrend_rt_tlas_emitter_set(tlas);
		if (!set)
			return false;
		eq::array<rbtrend_pass_image> &images = cmd_list->pass_images;
		eq::array<rbtrend_pass_store> &stores = cmd_list->pass_stores;
		images.clear();
		stores.clear();
		rbtrend_storage_stores_add_set(stores, set, cmd_list->compiled_frame, 0, emitters->storage_writes);
		for (const rbtrend_pass_store &store : stores)
			rbtrend_cmd_use_store(cmd_list, store.store);
		rbtrend_cmd_ready_set_resources(cmd_list, images, stores);
		rbtrend_cmd_use_handle(cmd_list, set);
		crgpu_command_list cmd = cmd_list->cmd_list;
		crgpu_command_list_bind_compute_pipeline(cmd, emitters->pipeline);
		crgpu_command_list_bind_compute_descriptor_sets(cmd, emitters->pipeline, 0, &set->sets[0], 1);
		rbtrend_rt_emitter_params params{};
		params.count          = tlas->built_count;
		params.material_count = tlas->max_materials;
		crgpu_command_list_push_compute_constants(cmd, emitters->pipeline, 0, sizeof(params), &params);
		crgpu_command_list_dispatch(cmd, 1, 1, 1);
		tlas->emitters_pending_content = tlas->content_version;
		tlas->emitters_pending_lit     = rt->lit_version;
		tlas->emitters_pending_list    = cmd_list;
	}

	RBTREND_BINDING_SET_ENTRY outputs[3]{};
	const rbtrend_texture_view views[3] = {p._target, albedo, normal};
	for (uint32_t i = 0; i < 3; ++i) {
		outputs[i]._binding               = i;
		outputs[i]._type                  = CRGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		outputs[i]._resource._image._view = views[i];
	}
	const rbtrend_texture_view others[2] = {albedo, normal};
	rbtrend_binding_set sets[3]          = {rbtrend_lit_scene_rt_set(p._scene, rt->scene_layout), rbtrend_rt_tlas_trace_set(tlas),
	                                       rbtrend_rt_output_set(p._target, rt->path_output_layout, others, outputs, 3)};
	if (!sets[0] || !sets[1] || !sets[2])
		return false;
	rbtrend_rt_trace_params params{};
	params.size[0]        = width;
	params.size[1]        = height;
	params.samples        = p._samples;
	params.accumulated    = p._accumulated;
	params.seed           = p._seed;
	params.max_bounces    = p._max_bounces;
	params.cull_mask      = p._cull_mask;
	params.clamp          = p._clamp;
	params.guides         = (p._albedo ? 1u : 0u) | (p._normal ? 2u : 0u);
	params.instance_count = tlas->built_count;
	params.material_count = tlas->max_materials;
	return rbtrend_cmd_rt_dispatch(cmd_list, pipeline, tlas, sets, &params, sizeof(params), (width + RBTREND_RT_TRACE_GROUP - 1) / RBTREND_RT_TRACE_GROUP,
	                              (height + RBTREND_RT_TRACE_GROUP - 1) / RBTREND_RT_TRACE_GROUP);
}

/* Whether [cmd_list] holds a LIT_CULL_LIGHTS of [scene], which then culls
 * its lights where it stands, in place of the cull at the list's top. */
static bool rbtrend_cmd_list_culls(rbtrend_command_list cmd_list, rbtrend_lit_scene scene) {
	for (const RBTREND_COMMAND &command : cmd_list->commands)
		if (command._type == RBTREND_COMMAND_LIT_CULL_LIGHTS && command._params._lit_cull_lights._scene == scene)
			return true;
	return false;
}

/* LIT_CULL_LIGHTS, outside a pass: the scene's tiles culled against the
 * depth's bounds, as the cull at the top of a list is recorded, with the
 * depth view's cull depth set at set 1. False, reported, if the depth has
 * nothing in it, the scene's view has no target size or another than the
 * depth's, or the pipeline or a set can't be made. */
static bool rbtrend_cmd_lit_cull_lights(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	const auto &p                    = command._params._lit_cull_lights;
	rbtrend_lit_scene scene           = p._scene;
	const rbtrend_texture_range depth = rbtrend_texture_view_range(p._depth);
	if (!rbtrend_cmd_range_written(cmd_list, depth)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS: the depth has nothing in it, from before the list or earlier in it");
		return false;
	}
	if (!(scene->block.flags & RBTREND_LIT_SCENE_FLAG_TILES) || !scene->tiles) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS: the scene's view has no target size and view_projection to cull for");
		return false;
	}
	const uint32_t width  = rbtrend_mip_size(p._depth->texture._t2d->desc.width, p._depth->desc.base_mip);
	const uint32_t height = rbtrend_mip_size(p._depth->texture._t2d->desc.height, p._depth->desc.base_mip);
	if ((float)width != scene->block.target_size[0] || (float)height != scene->block.target_size[1]) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS: the depth is %ux%u, and the scene's view's target %gx%g", width, height, (double)scene->block.target_size[0], (double)scene->block.target_size[1]);
		return false;
	}
	rbtrend_compute_pipeline pipeline = rbtrend_lit_cull_bounded_pipeline(cmd_list->instance);
	if (!pipeline || !rbtrend_cmd_use_material(cmd_list, scene->cull_material))
		return false;
	rbtrend_binding_set sets[2] = {scene->cull_material->set, rbtrend_texture_view_cull_depth_set(p._depth)};
	if (!sets[1])
		return false;
	// Each set's resources ordered as any dispatch's are: the depth moved to
	// be sampled, the tiles after what read or wrote them before.
	eq::array<rbtrend_pass_image> &images = cmd_list->pass_images;
	eq::array<rbtrend_pass_store> &stores = cmd_list->pass_stores;
	images.clear();
	stores.clear();
	try {
		for (uint32_t slot = 0; slot < 2; ++slot) {
			if (!rbtrend_pass_images_add_set(cmd_list, images, sets[slot], slot, pipeline->storage_writes))
				return false;
			rbtrend_storage_stores_add_set(stores, sets[slot], cmd_list->compiled_frame, slot, pipeline->storage_writes);
		}
	} catch (const std::bad_alloc &) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "compile: out of memory");
		return false;
	}
	rbtrend_cmd_ready_set_resources(cmd_list, images, stores);
	crgpu_command_list cmd = cmd_list->cmd_list;
	crgpu_command_list_bind_compute_pipeline(cmd, pipeline->pipeline);
	for (uint32_t slot = 0; slot < 2; ++slot)
		crgpu_command_list_bind_compute_descriptor_sets(cmd, pipeline->pipeline, slot,
		                                         &sets[slot]->sets[rbtrend_binding_set_frame_slot(sets[slot], cmd_list->compiled_frame)], 1);
	const uint32_t count = scene->block.tiles_x * scene->block.tiles_y;
	crgpu_command_list_dispatch(cmd, (count + 63) / 64, 1, 1);
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
static bool rbtrend_cmd_prepare_lit_scenes(rbtrend_command_list cmd_list) {
	rbtrend_instance instance = cmd_list->instance;
	rbtrend_lit_builtins &lit = instance->builtins.lit;
	// A copy: culling adds materials to the list.
	eq::array<rbtrend_material> &materials = cmd_list->lit_materials;
	materials.clear();
	if (materials.append(cmd_list->materials.data(), cmd_list->materials.size()) != EQ_SUCCESS) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "compile: out of memory");
		return false;
	}
	bool lut_checked = false;
	for (rbtrend_material material : materials) {
		rbtrend_lit_scene_t *scene = material->lit_scene;
		if (!scene)
			continue;
		if (!lut_checked) {
			lut_checked                    = true;
			const rbtrend_texture_range lut = rbtrend_texture_view_range(lit.brdf_lut_view);
			if (!rbtrend_cmd_range_written(cmd_list, lut)) {
				rbtrend_graphics_pipeline pipeline = rbtrend_lit_pass_pipeline(instance, RBTREND_LIT_PASS_BRDF_LUT);
				if (!pipeline)
					return false;
				rbtrend_size_params size{};
				size.target_size[0] = size.target_size[1] = (float)RBTREND_BRDF_LUT_SIZE;
				rbtrend_cmd_own_pass(cmd_list, pipeline, lit.brdf_lut_target, RBTREND_BRDF_LUT_SIZE, RBTREND_BRDF_LUT_SIZE, nullptr, 0, &size, sizeof(size));
			}
		}
		if (rbtrend_lit_environment env = scene->environment) {
			const rbtrend_texture_range specular   = rbtrend_texture_view_range(env->specular_cube);
			const rbtrend_texture_range irradiance = rbtrend_texture_view_range(env->irradiance_cube);
			if (!rbtrend_cmd_range_written(cmd_list, specular) || !rbtrend_cmd_range_written(cmd_list, irradiance)) {
				rbtrend_cmd_transition_range(cmd_list, specular, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
				rbtrend_cmd_transition_range(cmd_list, irradiance, CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
				// Cleared to 0 in every channel: the clear value every texture is
				// made with (rbtrend_texture2d_create leaves the image's clear_value
				// zeroed), so D3D12 clears at its fast rate and its debug layer
				// doesn't warn of a mismatch. Only .rgb of either cube is read.
				for (uint32_t mip = 0; mip < env->specular_mips; ++mip) {
					const uint32_t side = rbtrend_mip_size(env->desc.specular_size, mip);
					for (uint32_t face = 0; face < 6; ++face)
						crgpu_command_list_clear_color_attachment(cmd_list->cmd_list, env->specular_faces[(size_t)mip * 6 + face]->image_view, 0.0f, 0.0f, 0.0f,
						                                   0.0f, side, side);
				}
				for (uint32_t face = 0; face < 6; ++face)
					crgpu_command_list_clear_color_attachment(cmd_list->cmd_list, env->irradiance_faces[face]->image_view, 0.0f, 0.0f, 0.0f, 0.0f,
					                                   env->desc.irradiance_size, env->desc.irradiance_size);
			}
		}
		if ((scene->block.flags & RBTREND_LIT_SCENE_FLAG_TILES) && scene->tiles && !rbtrend_cmd_list_culls(cmd_list, scene)) {
			rbtrend_compute_pipeline pipeline = rbtrend_lit_cull_pipeline(instance);
			if (!pipeline || !rbtrend_cmd_use_material(cmd_list, scene->cull_material))
				return false;
			// What the cull's set holds is ordered as any dispatch's is, so
			// the tiles it writes are known written to what comes after.
			// The copy this frame writes may still be read by a submission
			// of this frame made earlier: its first use in the list gets a
			// barrier for that.
			rbtrend_binding_set set = scene->cull_material->set;
			eq::array<rbtrend_pass_image> &cull_images = cmd_list->pass_images;
			eq::array<rbtrend_pass_store> &cull_stores = cmd_list->pass_stores;
			cull_images.clear();
			cull_stores.clear();
			if (!rbtrend_pass_images_add_set(cmd_list, cull_images, set, 0, pipeline->storage_writes))
				return false;
			rbtrend_storage_stores_add_set(cull_stores, set, cmd_list->compiled_frame, 0, pipeline->storage_writes);
			rbtrend_cmd_ready_set_resources(cmd_list, cull_images, cull_stores);
			crgpu_command_list_bind_compute_pipeline(cmd_list->cmd_list, pipeline->pipeline);
			crgpu_command_list_bind_compute_descriptor_sets(cmd_list->cmd_list, pipeline->pipeline, 0,
			                                         &set->sets[rbtrend_binding_set_frame_slot(set, cmd_list->compiled_frame)], 1);
			const uint32_t count = scene->block.tiles_x * scene->block.tiles_y;
			crgpu_command_list_dispatch(cmd_list->cmd_list, (count + 63) / 64, 1, 1);
		}
	}
	return true;
}

/* Adds the views a built-in effect's [command] names to what [cmd_list]'s
 * compile names: they are read only through the recording, as a pass's
 * targets are. What the effect's own objects hold (an environment's cubes,
 * an exposure's textures) are the objects', and go with them. Throws
 * std::bad_alloc. */
static void rbtrend_cmd_use_command_views(rbtrend_command_list cmd_list, const RBTREND_COMMAND &command) {
	switch (command._type) {
	case RBTREND_COMMAND_BLOOM:
		rbtrend_cmd_use_handle(cmd_list, command._params._bloom._source);
		rbtrend_cmd_use_handle(cmd_list, command._params._bloom._target);
		if (command._params._bloom._mask)
			rbtrend_cmd_use_handle(cmd_list, command._params._bloom._mask);
		if (command._params._bloom._lens_dirt)
			rbtrend_cmd_use_handle(cmd_list, command._params._bloom._lens_dirt);
		break;
	case RBTREND_COMMAND_BLUR:
		rbtrend_cmd_use_handle(cmd_list, command._params._blur._source);
		rbtrend_cmd_use_handle(cmd_list, command._params._blur._target);
		break;
	case RBTREND_COMMAND_OUTLINE:
		rbtrend_cmd_use_handle(cmd_list, command._params._outline._mask);
		rbtrend_cmd_use_handle(cmd_list, command._params._outline._target);
		break;
	case RBTREND_COMMAND_LIT_CULL_LIGHTS:
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_cull_lights._depth);
		break;
	case RBTREND_COMMAND_TONE_MAP:
		rbtrend_cmd_use_handle(cmd_list, command._params._tone_map._source);
		rbtrend_cmd_use_handle(cmd_list, command._params._tone_map._target);
		break;
	case RBTREND_COMMAND_LIT_ENVIRONMENT:
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_environment._source);
		break;
	case RBTREND_COMMAND_LIT_AMBIENT_OCCLUSION:
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_ambient_occlusion._depth);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_ambient_occlusion._target);
		break;
	case RBTREND_COMMAND_AUTO_EXPOSURE:
		rbtrend_cmd_use_handle(cmd_list, command._params._auto_exposure._source);
		break;
	case RBTREND_COMMAND_DRAW_TEXTURE:
		rbtrend_cmd_use_handle(cmd_list, command._params._draw_texture._source);
		rbtrend_cmd_use_handle(cmd_list, command._params._draw_texture._target);
		break;
	case RBTREND_COMMAND_COPY_SCENE_COLOR:
		rbtrend_cmd_use_handle(cmd_list, command._params._copy_scene_color._source);
		rbtrend_cmd_use_handle(cmd_list, command._params._copy_scene_color._target);
		break;
	case RBTREND_COMMAND_LIT_SSR:
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_ssr._depth);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_ssr._normal_roughness);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_ssr._color);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_ssr._reflection);
		break;
	case RBTREND_COMMAND_LIT_TRACE_REFLECTIONS:
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_trace_reflections._depth);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_trace_reflections._normal_roughness);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_trace_reflections._reflection);
		break;
	case RBTREND_COMMAND_LIT_REFLECT:
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_reflect._depth);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_reflect._normal_roughness);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_reflect._specular);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_reflect._reflection);
		rbtrend_cmd_use_handle(cmd_list, command._params._lit_reflect._target);
		break;
	default:
		break;
	}
}

RBT_RESULT rbtrend_command_list_compile(rbtrend_command_list cmd_list, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!cmd_list)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "cmd_list is NULL");
	if (RBT_STALE(cmd_list, "rbtrend_command_list_compile", "cmd_list"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "cmd_list is destroyed");
	RBTREND_ENTER_LIST(cmd_list, "rbtrend_command_list_compile");
	RBTREND_ENTER(cmd_list->instance, "rbtrend_command_list_compile");
	rbtrend_cmd_report_scope scope(cmd_list, out_error);
	cmd_list->compiled = false;
	rbtrend_cmd_list_unlist(cmd_list);
	// A recording of this frame that was never submitted is recorded over;
	// otherwise a list of this frame that nothing is running.
	rbtrend_instance instance = cmd_list->instance;
	if (!cmd_list->cmd_list || cmd_list->compiled_frame_number != instance->frame_number) {
		cmd_list->cmd_list = nullptr;
		CR_RESULT sr      = rbtrend_instance_take_command_list(instance, &cmd_list->cmd_list);
		if (CR_FAILED(sr)) {
			rbtrend_cmd_report(cmd_list, rbtrend_result_from_cr(sr), "compile: no Carrottus GPU command list to record into: %s", cr_result_str(sr));
			cmd_list->cmd_list = nullptr;
			return scope.failed(RBT_RESULT_REND_GPU_FAILURE, "the compile failed");
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
	rbtrend_render_target_give_back(cmd_list);
	// Every buffer set this compile reads is read at this frame's copy, and
	// it is recorded into this frame's allocator.
	cmd_list->compiled_frame        = instance->frame_index;
	cmd_list->compiled_frame_number = instance->frame_number;
	cmd_list->timer_uses.clear();
	cmd_list->materials.clear();
	if (cmd_list->recording_error) {
		// What the recording found, at the send that found it.
		cmd_list->report_send = cmd_list->recording_send;
		rbtrend_cmd_report(cmd_list, cmd_list->recording_result != RBT_RESULT_OK ? cmd_list->recording_result : RBT_RESULT_INVALID_ARGUMENT, "%s",
		                  cmd_list->recording_message[0] ? cmd_list->recording_message : "a command was refused when it was sent");
		return cmd_list->report_result;
	}
	rbtrend_cmd_point_at_copies(cmd_list);

	// Every material's binding set is built before anything is recorded: a
	// pass looks ahead at the sets its draws read (rbtrend_cmd_prepare_pass_
	// images), and a SET_MATERIAL inside it has to have one by then.
	try {
		for (const RBTREND_COMMAND &command : cmd_list->commands) {
			// A material set for a draw; and the scene of a reflection
			// command that reads its environment and lookup texture, which
			// the scenes' preparation below fills.
			rbtrend_material material = nullptr;
			if (command._type == RBTREND_COMMAND_SET_MATERIAL)
				material = command._params._set_material._material;
			else if (command._type == RBTREND_COMMAND_LIT_TRACE_REFLECTIONS)
				material = command._params._lit_trace_reflections._scene->material;
			else if (command._type == RBTREND_COMMAND_LIT_REFLECT)
				material = command._params._lit_reflect._scene->material;
			if (!material)
				continue;
			bool known               = false;
			for (rbtrend_material listed : cmd_list->materials)
				if (listed == material)
					known = true;
			if (known)
				continue;
			if (!rbtrend_material_prepare(material)) {
				cmd_list->materials.clear();
				return scope.failed(RBT_RESULT_REND_GPU_FAILURE, "the compile failed");
			}
			cmd_list->materials.push_back(material);
		}
	} catch (const std::bad_alloc &) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "compile: out of memory");
		cmd_list->materials.clear();
		return scope.failed(RBT_RESULT_REND_GPU_FAILURE, "the compile failed");
	}

	crgpu_command_list cmd = cmd_list->cmd_list;
	crgpu_command_list_begin(cmd);
	bool ok = false;

	/* Whether a BEGIN_RENDERING pass is currently open. */
	bool in_pass = false;
	/* The BEGIN_RENDERING of the open pass, which every pipeline set in it
	 * must match, and which may time the pass into a timer. */
	const RBTREND_COMMAND *open_pass = nullptr;
	/* The open labelled regions, innermost last: whether each was opened
	 * inside a pass. */
	eq::array<uint8_t> &open_labels = cmd_list->open_labels;
	open_labels.clear();
	/* The pipeline bound in the open pass; none until the pass sets one. */
	rbtrend_graphics_pipeline bound_pipeline = nullptr;
	/* The stride of the buffer or transient span last put in each vertex
	 * buffer slot; 0 for a slot nothing has been put in. */
	rbtrend_cmd_vertex_slot vertex_slots[RBTREND_MAX_VERTEX_BINDINGS]{};
	/* The view bound at each vertex buffer slot in the open pass for the
	 * bound pipeline: a draw binds whatever differs. */
	crgpu_buffer_view applied_vertex_views[RBTREND_MAX_VERTEX_BINDINGS]{};
	/* Whether an index buffer or a transient index span has been set. */
	bool index_buffer_bound = false;
	/* The set SET_BINDING_SET last put in each slot, and the one actually
	 * bound there for the bound pipeline in the open pass. A draw binds
	 * whatever differs. */
	rbtrend_binding_set slot_sets[RBTREND_MAX_BINDING_LAYOUTS]{};
	rbtrend_binding_set applied_sets[RBTREND_MAX_BINDING_LAYOUTS]{};
	/* The compute pipeline set outside a pass, until the next pass or
	 * present, and the set bound for it in each slot. */
	rbtrend_compute_pipeline bound_compute_pipeline = nullptr;
	rbtrend_binding_set applied_compute_sets[RBTREND_MAX_BINDING_LAYOUTS]{};


	// Every vector of the list and of this walk can throw std::bad_alloc,
	// which must not leave the C entry point: the list is closed below and
	// the compile fails.
	cmd_list->rt_accesses.clear();
	cmd_list->rt_reads_lit = false;
	try {
		ok = (!instance->rt || rbtrend_cmd_rt_prepare(cmd_list)) && rbtrend_cmd_prepare_lit_scenes(cmd_list);

		// Nothing more is recorded once something has failed, the top of the
		// list (rbtrend_cmd_prepare_lit_scenes) included.
		for (size_t command_index = 0; ok && command_index < cmd_list->commands.size(); ++command_index) {
			const RBTREND_COMMAND &command = cmd_list->commands[command_index];
			cmd_list->report_send         = cmd_list->command_sends[command_index];
			rbtrend_cmd_use_command_views(cmd_list, command);
			switch (command._type) {
			case RBTREND_COMMAND_DRAW:
			case RBTREND_COMMAND_DRAW_INDEXED:
			case RBTREND_COMMAND_DRAW_INSTANCED:
			case RBTREND_COMMAND_DRAW_INSTANCED_INDEXED:
			case RBTREND_COMMAND_DRAW_INDIRECT:
			case RBTREND_COMMAND_DRAW_INDEXED_INDIRECT:
			case RBTREND_COMMAND_DISPATCH_MESH:
				if (!in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "draw command recorded outside a rendering pass");
					ok = false;
				} else if (!bound_pipeline) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "draw command in a pass that hasn't set a pipeline");
					ok = false;
				} else if (bound_pipeline->is_mesh && command._type != RBTREND_COMMAND_DISPATCH_MESH) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "draw command with a mesh shader pipeline, which is run with DISPATCH_MESH");
					ok = false;
				} else if (!bound_pipeline->is_mesh && command._type == RBTREND_COMMAND_DISPATCH_MESH) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DISPATCH_MESH with a pipeline that has no mesh shader");
					ok = false;
				} else if (!index_buffer_bound && (command._type == RBTREND_COMMAND_DRAW_INDEXED ||
				                                   command._type == RBTREND_COMMAND_DRAW_INSTANCED_INDEXED ||
				                                   command._type == RBTREND_COMMAND_DRAW_INDEXED_INDIRECT)) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "indexed draw with no SET_INDEX_BUFFER or SET_TRANSIENT_INDEX_BUFFER before it");
					ok = false;
				} else if (!rbtrend_cmd_apply_vertex_buffers(cmd_list, bound_pipeline, vertex_slots, applied_vertex_views, command)) {
					ok = false;
				} else {
					ok = rbtrend_cmd_apply_binding_sets(cmd_list, cmd, bound_pipeline, slot_sets, applied_sets, cmd_list->compiled_frame);
				}
				break;
			default:
				break;
			}
			if (!ok)
				break;

			switch (command._type) {
			case RBTREND_COMMAND_SET_VERTEX_BUFFERS: {
				const auto &p = command._params._set_vertex_buffers;
				// Bound at each draw, at the stride of its pipeline's binding.
				for (uint32_t i = 0; i < p._vertex_buffer_count; ++i) {
					// rbtrend_command_list_send refused a command that runs past the last slot.
					rbtrend_buffer_part *part               = rbtrend_buffer_part_of(p._vertex_buffers[i], cmd_list->compiled_frame);
					vertex_slots[p._start_slot + i]        = {};
					vertex_slots[p._start_slot + i].buffer = part;
					rbtrend_cmd_use_store(cmd_list, &part->store);
				}
				break;
			}
			case RBTREND_COMMAND_SET_INDEX_BUFFER: {
				const auto &p = command._params._set_index_buffer;
				if (!p._index_buffer) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_INDEX_BUFFER with a NULL buffer");
					ok = false;
					break;
				}
				rbtrend_buffer_part *part = rbtrend_buffer_part_of(p._index_buffer, cmd_list->compiled_frame);
				crgpu_command_list_set_index_buffer(cmd, part->view);
				index_buffer_bound = true;
				rbtrend_cmd_use_store(cmd_list, &part->store);
				break;
			}
			case RBTREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER: {
				const auto &p = command._params._set_transient_vertex_buffer;
				if (!rbtrend_transient_span_valid(cmd_list->instance, &p._span, "SET_TRANSIENT_VERTEX_BUFFER")) {
					ok = false;
					break;
				}
				// Bound at each draw, as a buffer is. rbtrend_command_list_send
				// refused a slot past the last.
				vertex_slots[p._slot]          = {};
				vertex_slots[p._slot].has_span = true;
				vertex_slots[p._slot].span     = p._span;
				break;
			}
			case RBTREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER: {
				const auto &p = command._params._set_transient_index_buffer;
				if (!rbtrend_transient_span_valid(cmd_list->instance, &p._span, "SET_TRANSIENT_INDEX_BUFFER")) {
					ok = false;
					break;
				}
				if (p._span._offset % p._index_size != 0) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_TRANSIENT_INDEX_BUFFER with a span not aligned to its _index_size of %u; allocate it with that alignment", p._index_size);
					ok = false;
					break;
				}
				crgpu_buffer_view view = nullptr;
				CR_RESULT sr            = rbtrend_transient_span_view(cmd_list->instance, &p._span, p._index_size, &view);
				if (CR_FAILED(sr)) {
					rbtrend_cmd_report(cmd_list, rbtrend_result_from_cr(sr), "SET_TRANSIENT_INDEX_BUFFER: a view of the span can't be created: %s", cr_result_str(sr));
					ok = false;
					break;
				}
				crgpu_command_list_set_index_buffer(cmd, view);
				index_buffer_bound = true;
				break;
			}
			case RBTREND_COMMAND_SET_GRAPHICS_PIPELINE: {
				const auto &p = command._params._set_shader_pipeline;
				if (!p._pipeline) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE with a NULL pipeline");
					ok = false;
					break;
				}
				if (!in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_SHADER_PIPELINE outside a rendering pass");
					ok = false;
					break;
				}
				if (!rbtrend_pipeline_matches_pass(cmd_list, p._pipeline, *open_pass)) {
					ok = false;
					break;
				}
				crgpu_command_list_bind_graphics_pipeline(cmd, p._pipeline->pipeline);
				bound_pipeline = p._pipeline;
				rbtrend_cmd_use_handle(cmd_list, p._pipeline);
				// Not guaranteed to survive a pipeline change on every backend:
				// the next draw binds them again. Vertex buffers do survive one
				// on every API; a draw binds a slot again only where the new
				// pipeline's stride makes its view another.
				for (rbtrend_binding_set &applied : applied_sets)
					applied = nullptr;
				break;
			}
			case RBTREND_COMMAND_SET_BINDING_SET: {
				const auto &p = command._params._set_binding_set;
				ok            = rbtrend_cmd_put_binding_set(cmd_list, p._slot, p._set, slot_sets);
				break;
			}
			case RBTREND_COMMAND_SET_MATERIAL: {
				// Its set was built at the top of the compile.
				const auto &p = command._params._set_material;
				ok            = rbtrend_cmd_put_binding_set(cmd_list, p._slot, p._material->set, slot_sets);
				break;
			}
			case RBTREND_COMMAND_PUSH_CONSTANTS: {
				const auto &p = command._params._push_constants;
				if (!in_pass) {
					// Outside a pass the block written is the compute pipeline's.
					if (!bound_compute_pipeline) {
						rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS outside a pass with no compute pipeline set");
						ok = false;
						break;
					}
					if (p._offset > bound_compute_pipeline->push_constant_end) {
						rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS at offset %u, past the compute pipeline's push constant ranges (%u bytes)", p._offset, bound_compute_pipeline->push_constant_end);
						ok = false;
						break;
					}
					if (p._size > bound_compute_pipeline->push_constant_end - p._offset) {
						rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS of %u bytes at offset %u runs past the compute pipeline's push constant ranges (%u bytes)", p._size, p._offset, bound_compute_pipeline->push_constant_end);
						ok = false;
						break;
					}
					crgpu_command_list_push_compute_constants(cmd, bound_compute_pipeline->pipeline, p._offset, p._size, p._data);
					break;
				}
				if (!bound_pipeline) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS in a pass that hasn't set a pipeline");
					ok = false;
					break;
				}
				if (p._offset > bound_pipeline->push_constant_end) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS at offset %u, past the pipeline's push constant ranges (%u bytes)", p._offset, bound_pipeline->push_constant_end);
					ok = false;
					break;
				}
				if (p._size > bound_pipeline->push_constant_end - p._offset) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PUSH_CONSTANTS of %u bytes at offset %u runs past the pipeline's push constant ranges (%u bytes)", p._size, p._offset, bound_pipeline->push_constant_end);
					ok = false;
					break;
				}
				crgpu_command_list_push_graphics_constants(cmd, bound_pipeline->pipeline, p._offset, p._size, p._data);
				break;
			}
			case RBTREND_COMMAND_SET_COMPUTE_PIPELINE: {
				const auto &p = command._params._set_compute_pipeline;
				if (!p._pipeline) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_COMPUTE_PIPELINE with a NULL pipeline");
					ok = false;
					break;
				}
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "SET_COMPUTE_PIPELINE inside a rendering pass");
					ok = false;
					break;
				}
				crgpu_command_list_bind_compute_pipeline(cmd, p._pipeline->pipeline);
				bound_compute_pipeline = p._pipeline;
				rbtrend_cmd_use_handle(cmd_list, p._pipeline);
				// Not guaranteed to survive a pipeline change on every backend:
				// the next dispatch binds them again.
				for (rbtrend_binding_set &applied : applied_compute_sets)
					applied = nullptr;
				break;
			}
			case RBTREND_COMMAND_DISPATCH: {
				const auto &p = command._params._dispatch;
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DISPATCH inside a rendering pass");
					ok = false;
					break;
				}
				if (!bound_compute_pipeline) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DISPATCH with no compute pipeline set since the last pass or present");
					ok = false;
					break;
				}
				if (!rbtrend_cmd_prepare_dispatch(cmd_list, bound_compute_pipeline, slot_sets, applied_compute_sets)) {
					ok = false;
					break;
				}
				crgpu_command_list_dispatch(cmd, p._group_count_x, p._group_count_y, p._group_count_z);
				break;
			}
			case RBTREND_COMMAND_PRESENT_TEXTURE: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "PRESENT_TEXTURE inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_present_texture(cmd_list, command);
				// The copy ends compute recording on a backend that keeps it
				// apart from copies: a compute pipeline is set again after it.
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_DRAW: {
				const auto &p = command._params._draw;
				crgpu_command_list_draw(cmd, p._vertex_count, p._start_vertex_location);
				break;
			}
			case RBTREND_COMMAND_DRAW_INDEXED: {
				const auto &p = command._params._draw_indexed;
				crgpu_command_list_draw_indexed(cmd, p._index_count, p._start_index_location, p._base_vertex_location);
				break;
			}
			case RBTREND_COMMAND_DRAW_INSTANCED: {
				const auto &p = command._params._draw_instanced;
				crgpu_command_list_draw_instanced(cmd, p._vertex_count_per_instance, p._instance_count, p._start_vertex_location, p._start_instance_location);
				break;
			}
			case RBTREND_COMMAND_DRAW_INSTANCED_INDEXED: {
				const auto &p = command._params._draw_indexed_instanced;
				crgpu_command_list_draw_indexed_instanced(
				    cmd, p._index_count_per_instance, p._instance_count, p._start_index_location, p._base_vertex_location,
				    p._start_instance_location);
				break;
			}
			case RBTREND_COMMAND_DRAW_INDIRECT: {
				const auto &p                   = command._params._draw_indirect;
				rbtrend_buffer_part *arguments   = rbtrend_cmd_indirect_arguments(command, cmd_list->compiled_frame);
				// What the CPU wrote to it is copied at submit if it is staged.
				// It was moved to the indirect argument state before the pass.
				rbtrend_cmd_use_store(cmd_list, &arguments->store);
				// In calls of at most the device's count, at advancing offsets:
				// a Carrottus GPU call with more records nothing.
				const uint32_t most = cmd_list->instance->max_draw_indirect_count;
				uint64_t offset     = p._offset;
				for (uint32_t left = p._draw_count; left > 0;) {
					const uint32_t count = std::min(most, left);
					crgpu_command_list_draw_indirect(cmd, arguments->store.buffer, offset, count, p._stride);
					offset += (uint64_t)count * p._stride;
					left -= count;
				}
				break;
			}
			case RBTREND_COMMAND_DRAW_INDEXED_INDIRECT: {
				const auto &p                   = command._params._draw_indirect;
				rbtrend_buffer_part *arguments   = rbtrend_cmd_indirect_arguments(command, cmd_list->compiled_frame);
				rbtrend_cmd_use_store(cmd_list, &arguments->store);
				const uint32_t most = cmd_list->instance->max_draw_indirect_count;
				uint64_t offset     = p._offset;
				for (uint32_t left = p._draw_count; left > 0;) {
					const uint32_t count = std::min(most, left);
					crgpu_command_list_draw_indexed_indirect(cmd, arguments->store.buffer, offset, count, p._stride);
					offset += (uint64_t)count * p._stride;
					left -= count;
				}
				break;
			}
			case RBTREND_COMMAND_DISPATCH_MESH: {
	#if CRGPU_EXT_MESH_SHADING
				const auto &p = command._params._dispatch_mesh;
				crgpu_command_list_dispatch_mesh(cmd, p._group_count_x, p._group_count_y, p._group_count_z);
	#else
				// No mesh shader pipeline can exist in such a build, so the
				// pipeline check above has already refused this.
				ok = false;
	#endif
				break;
			}
			case RBTREND_COMMAND_SET_VIEWPORTS: {
				const auto &p = command._params._set_viewports;
				crgpu_command_list_set_viewports(cmd, p._first_viewport, p._viewport_count, p._viewports);
				break;
			}
			case RBTREND_COMMAND_SET_SCISSOR_RECTS: {
				const auto &p = command._params._set_scissor_rects;
				crgpu_command_list_set_scissor_rects(cmd, p._first_scissor_rect, p._scissor_rect_count, p._scissor_rects);
				break;
			}
			case RBTREND_COMMAND_BEGIN_RENDERING: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING while another rendering pass is open");
					ok = false;
					break;
				}
				if (!rbtrend_begin_rendering_valid(cmd_list, command)) {
					ok = false;
					break;
				}
				if (!rbtrend_cmd_prepare_pass_images(cmd_list, command_index, slot_sets)) {
					ok = false;
					break;
				}
				if (!rbtrend_cmd_begin_timed(cmd_list, command)) {
					ok = false;
					break;
				}
				rbtrend_cmd_begin_rendering(cmd_list, command);
				in_pass   = true;
				open_pass = &command;
				// A pipeline is bound inside a pass and doesn't outlive it, and
				// bound sets aren't guaranteed to carry over from one pass to the
				// next either.
				bound_pipeline = nullptr;
				for (rbtrend_binding_set &applied : applied_sets)
					applied = nullptr;
				for (crgpu_buffer_view &applied : applied_vertex_views)
					applied = nullptr;
				// Nor does a compute pipeline outlive the pass that follows it.
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_END_RENDERING: {
				if (!in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "END_RENDERING without an open BEGIN_RENDERING pass");
					ok = false;
					break;
				}
				for (bool opened_in_pass : open_labels) {
					if (opened_in_pass) {
						rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "END_RENDERING with a label opened inside the pass still open");
						ok = false;
					}
				}
				if (!ok)
					break;
				crgpu_command_list_end_rendering(cmd);
				// The pass's times are written; made readable outside it.
				rbtrend_cmd_end_timed(cmd_list, *open_pass);
				in_pass   = false;
				open_pass = nullptr;
				break;
			}
			case RBTREND_COMMAND_BLOOM: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLOOM inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_bloom(cmd_list, command);
				// Its passes bind pipelines of their own: a compute pipeline is
				// set again before the next dispatch, as after a pass.
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_BLUR: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BLUR inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_blur(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_OUTLINE: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "OUTLINE inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_outline(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_LIT_CULL_LIGHTS: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_CULL_LIGHTS inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_lit_cull_lights(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_TONE_MAP: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "TONE_MAP inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_tone_map(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_LIT_ENVIRONMENT: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_ENVIRONMENT inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_lit_environment(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_LIT_SKY:
			case RBTREND_COMMAND_LIT_AMBIENT_OCCLUSION:
			case RBTREND_COMMAND_AUTO_EXPOSURE: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s inside a rendering pass", command._type == RBTREND_COMMAND_LIT_SKY             ? "LIT_SKY"
					                                               : command._type == RBTREND_COMMAND_AUTO_EXPOSURE ? "AUTO_EXPOSURE"
					                                                                                                : "LIT_AMBIENT_OCCLUSION");
					ok = false;
					break;
				}
				if (command._type == RBTREND_COMMAND_LIT_SKY)
					ok = rbtrend_cmd_lit_sky(cmd_list, command);
				else if (command._type == RBTREND_COMMAND_AUTO_EXPOSURE)
					ok = rbtrend_cmd_auto_exposure(cmd_list, command);
				else
					ok = rbtrend_cmd_lit_ambient_occlusion(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_DRAW_TEXTURE: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "DRAW_TEXTURE inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_draw_texture(cmd_list, command);
				// Its pass binds a pipeline of its own: a compute pipeline is
				// set again before the next dispatch, as after a pass.
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_COPY_SCENE_COLOR: {
				const auto &p = command._params._copy_scene_color;
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "COPY_SCENE_COLOR inside a rendering pass");
					ok = false;
					break;
				}
				// DRAW_TEXTURE's stretch into the level, then GENERATE_MIPS
				// below it.
				RBTREND_COMMAND draw{};
				draw._type                        = RBTREND_COMMAND_DRAW_TEXTURE;
				draw._params._draw_texture._source = p._source;
				draw._params._draw_texture._target = p._target;
				draw._params._draw_texture._scaling = RBTREND_TEXTURE_SCALING_STRETCH;
				draw._params._draw_texture._filter  = CRGPU_FILTER_LINEAR;
				ok = rbtrend_cmd_draw_texture(cmd_list, draw) &&
				     rbtrend_cmd_generate_mips(cmd_list, p._target->texture._t2d, p._target->desc.base_mip, CRGPU_FILTER_LINEAR);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_GENERATE_MIPS: {
				const auto &p = command._params._generate_mips;
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "GENERATE_MIPS inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_generate_mips(cmd_list, p._texture, p._base_mip, p._filter);
				// Its passes bind a pipeline of their own: a compute pipeline is
				// set again before the next dispatch, as after a pass.
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_BUILD_BLAS:
			case RBTREND_COMMAND_BUILD_TLAS: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s inside a rendering pass", rbtrend_rt_command_name(command._type));
					ok = false;
					break;
				}
				// The run of build commands that starts here, as one build call
				// (T8); the walk goes on after it.
				const size_t run_end = rbtrend_cmd_rt_run_end(cmd_list, command_index);
				ok                   = rbtrend_cmd_rt_builds(cmd_list, command_index, run_end);
				command_index        = run_end - 1;
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_LIT_SSR:
			case RBTREND_COMMAND_LIT_REFLECT: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s inside a rendering pass",
					                  command._type == RBTREND_COMMAND_LIT_SSR ? "LIT_SSR" : "LIT_REFLECT");
					ok = false;
					break;
				}
				if (command._type == RBTREND_COMMAND_LIT_SSR)
					ok = rbtrend_cmd_lit_ssr(cmd_list, command);
				else
					ok = rbtrend_cmd_lit_reflect(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_LIT_TRACE_REFLECTIONS: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "LIT_TRACE_REFLECTIONS inside a rendering pass");
					ok = false;
					break;
				}
				ok = rbtrend_cmd_rt_reflections(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_LIT_SHADOW_MASK:
			case RBTREND_COMMAND_LIT_PATH_TRACE: {
				if (in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%s inside a rendering pass", rbtrend_rt_command_name(command._type));
					ok = false;
					break;
				}
				if (command._type == RBTREND_COMMAND_LIT_SHADOW_MASK)
					ok = rbtrend_cmd_rt_shadow_mask(cmd_list, command);
				else
					ok = rbtrend_cmd_rt_path_trace(cmd_list, command);
				bound_compute_pipeline = nullptr;
				break;
			}
			case RBTREND_COMMAND_BEGIN_LABEL:
				try {
					rbtrend_push(open_labels, (uint8_t)in_pass);
				} catch (const std::bad_alloc &) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "compile: out of memory");
					ok = false;
					break;
				}
				crgpu_command_list_begin_debug_label(cmd, command._params._begin_label._label);
				break;
			case RBTREND_COMMAND_END_LABEL:
				if (open_labels.empty()) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "END_LABEL with no label open");
					ok = false;
					break;
				}
				if ((open_labels[open_labels.size() - 1] != 0) != in_pass) {
					rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "END_LABEL %s a pass, and its BEGIN_LABEL was %s one", in_pass ? "inside" : "outside", open_labels[open_labels.size() - 1] ? "inside" : "outside");
					ok = false;
					break;
				}
				(void)open_labels.remove(open_labels.size() - 1);
				crgpu_command_list_end_debug_label(cmd);
				break;
			default:
				break;
			}
			if (!ok)
				break;
		}
	} catch (const std::bad_alloc &) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_OUT_OF_MEMORY, "compile: out of memory");
		ok = false;
	}

	cmd_list->report_send = UINT32_MAX;
	if (ok && in_pass) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDERING pass left open at the end of the command list");
		ok = false;
	}
	if (ok && !open_labels.empty()) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "%zu labels left open at the end of the command list", open_labels.size());
		ok = false;
	}

	/* Always leave the crgpu list closed and balanced, even on failure —
	 * rbtrend_command_list_submit refuses it when this returns false. The
	 * labels opened inside the pass close before it does, and the rest
	 * after. */
	while (in_pass && !open_labels.empty() && open_labels[open_labels.size() - 1]) {
		crgpu_command_list_end_debug_label(cmd);
		(void)open_labels.remove(open_labels.size() - 1);
	}
	if (in_pass)
		crgpu_command_list_end_rendering(cmd);
	while (!open_labels.empty()) {
		crgpu_command_list_end_debug_label(cmd);
		(void)open_labels.remove(open_labels.size() - 1);
	}

	crgpu_command_list_end(cmd);
	if (ok)
		ok = rbtrend_cmd_list_enlist(cmd_list);
	if (!ok) {
		rbtrend_render_target_give_back(cmd_list);
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
	return ok ? RBT_RESULT_OK : scope.failed(RBT_RESULT_REND_GPU_FAILURE, "the compile failed");
}

RBT_RESULT rbtrend_command_list_submit(rbtrend_command_list cmd_list, crgpu_command_queue queue, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!cmd_list)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "cmd_list is NULL");
	if (RBT_STALE(cmd_list, "rbtrend_command_list_submit", "cmd_list"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "cmd_list is destroyed");
	RBTREND_ENTER_LIST(cmd_list, "rbtrend_command_list_submit");
	if (!queue)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "queue is NULL");
	RBTREND_ENTER(cmd_list->instance, "rbtrend_command_list_submit");
	rbtrend_cmd_report_scope scope(cmd_list, out_error);
	if (!cmd_list->compiled)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the list has not compiled successfully since its last submit");
	// Before anything the list uses is read: something it was compiled
	// against may be gone (rbtrend_instance_forget).
	{
		std::lock_guard<std::mutex> lock(cmd_list->instance->release_mutex);
		if (cmd_list->stale) {
			return rbtrend_fail(out_error, RBT_RESULT_REND_STALE, "something the list uses was destroyed, resized or updated since it was compiled; compile it again");
		}
	}
	// Every texture must still be in the layout this list's barriers were
	// compiled against; if another submission moved it since, the barriers
	// would lie to the GPU.
	for (const auto &use : cmd_list->layout_uses) {
		if (*use.tracked != use.initial_layout) {
			return rbtrend_fail(out_error, RBT_RESULT_REND_STALE, "a texture's layout changed since the list was compiled; compile it again");
		}
	}

	// The recording is in the allocator of the frame it was compiled in,
	// which the next turn of that frame index resets. A list that binds a
	// per-frame set or a transient span reads that frame's copies and
	// memory as well.
	if (cmd_list->compiled_frame_number != cmd_list->instance->frame_number) {
		return rbtrend_fail(out_error, RBT_RESULT_REND_STALE, "the list was compiled in an earlier frame (rbtrend_instance_next_frame has been called since); compile it again");
	}

	rbtrend_swap_chain sc = cmd_list->present_swap_chain;
	if (sc) {
		// The list copies into one specific acquired back buffer; it's only
		// valid while that same one is still acquired and unsubmitted.
		if (sc->generation != cmd_list->present_generation) {
			return rbtrend_fail(out_error, RBT_RESULT_REND_STALE, "the swap chain the list presents into was resized since it was compiled; compile it again");
		}
		if (sc->acquired_image != cmd_list->present_image_index || sc->submitted) {
			return rbtrend_fail(out_error, RBT_RESULT_REND_STALE, "the back buffer the list presents into is no longer the acquired one; compile it again");
		}
		if (queue != sc->desc.queue) {
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "a list presenting into a swap chain goes on that swap chain's queue");
		}
	}

	// Builds of one TLAS go in the order they were sent (T5).
	if (cmd_list->instance->rt && !rbtrend_rt_submit_order_ok(cmd_list))
		return scope.failed(RBT_RESULT_INVALID_ARGUMENT, "the list's BUILD_TLAS was sent after another list's, not yet submitted");

	// Whatever was written to the list's staged buffers since they were last
	// copied goes across first, in the same submission, so the list's own
	// commands read what the caller last wrote; and so do the uniform blocks
	// of its materials that changed since they were last copied, through
	// this frame's transient memory.
	crgpu_command_list upload = nullptr;
	eq::array<rbtrend_upload_copy> &copies = cmd_list->upload_copies;
	copies.clear();
	try {
		for (rbtrend_material material : cmd_list->materials)
			if (!rbtrend_material_stage_uploads(material, copies))
				return scope.failed(RBT_RESULT_REND_GPU_FAILURE, "a material's uniform blocks can't be staged");
		// A path trace reads the lit-material table: the entries staged since
		// it was last copied go across ahead of it (D4).
		if (cmd_list->rt_reads_lit && !rbtrend_rt_stage_lit(cmd_list->instance, copies))
			return scope.failed(RBT_RESULT_REND_GPU_FAILURE, "the lit-material table's entries can't be staged");
		CR_RESULT sr = rbtrend_cmd_record_uploads(cmd_list, copies, &upload);
		if (CR_FAILED(sr)) {
			return rbtrend_fail_cr(out_error, sr, "taking a Carrottus GPU command list for the list's uploads failed");
		}
	} catch (const std::bad_alloc &) {
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the list's uploads");
	}

	crgpu_command_list lists[RBTREND_SUBMIT_MAX_LISTS];
	uint32_t list_count = 0;
	if (upload)
		lists[list_count++] = upload;
	lists[list_count++] = cmd_list->cmd_list;

	// Every submission signals the instance's frame fence to the next
	// serial, so rbtrend_instance_next_frame can wait for the last one of a
	// frame without being told which that was. With a swap chain the
	// submission also waits for the acquire to finish and signals
	// presentation.
	uint64_t serial = 0;
	if (const RBT_RESULT submitted = rbtrend_instance_submit(cmd_list->instance, queue, lists, list_count, sc ? sc->swap_chain : nullptr, &serial, out_error))
		return submitted;
	if (sc)
		sc->submitted = true;
	// Its builds' sets, BLASes and staged lit-material entries are this
	// submission's now.
	if (cmd_list->instance->rt)
		rbtrend_rt_submitted(cmd_list, serial);

	// Copied only now that the submission is in: a failed one leaves the
	// uploads for the next. A buffer an upload owns is retired, released
	// once this submission has run.
	for (rbtrend_buffer_store *store : cmd_list->staged_stores)
		rbtrend_buffer_store_uploads_submitted(store);
	for (const rbtrend_upload_copy &copy : copies)
		*copy.dirty = false;

	for (const auto &use : cmd_list->layout_uses)
		*use.tracked = use.final_layout;

	// Each pass timed in this submission is the slot's time for this frame.
	for (const auto &use : cmd_list->timer_uses)
		use.timer->written_frame[(size_t)cmd_list->compiled_frame * use.timer->slot_count + use.slot] = cmd_list->compiled_frame_number;

	// A compile is one recording of the Carrottus GPU list, and Carrottus GPU submits a
	// recording once. The recording may now be running, so the next compile
	// records into another list.
	// What it took from the pool is the submission's now, free again once
	// this frame has finished.
	cmd_list->pool_takes.clear();
	cmd_list->compiled = false;
	cmd_list->cmd_list = nullptr;
	rbtrend_cmd_list_unlist(cmd_list);
	return RBT_RESULT_OK;
}
}
