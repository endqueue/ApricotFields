
#include "render/aprendpass.h"
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

#include <cstdio>
#include <cstdlib>

// Whether [view] can be a target of a pass of [instance] as [type], of a
// texture of [format]. Fails, [error] naming [what], if not.
static APRESULT aprend_pass_target_valid(
    APREND_ERROR *error,
    const char *what,
    aprend_instance instance,
    aprend_texture_view view,
    APREND_TEXTURE_VIEW_TYPE type,
    SPUDGPU_FORMAT format) {
	if (view->instance != instance)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "%s is of another instance", what);
	if (view->view_type != type)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "%s is a view of type %u, and a pass takes %u there", what, (unsigned)view->view_type, (unsigned)type);
	if (view->dimension != APREND_TEXTURE_DIMENSION_2D)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "%s is not a view of a 2D texture", what);
	if (view->texture._t2d->desc.format != format)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "%s is of format %u, and the pipeline renders to %u", what, (unsigned)view->texture._t2d->desc.format, (unsigned)format);
	return APRESULT_OK;
}

// The format of attachment [index] of [framebuffer]: its texture's, where
// the caller gave one, else the one the framebuffer creates it with.
static SPUDGPU_FORMAT aprend_framebuffer_attachment_format(aprend_framebuffer framebuffer, uint32_t index) {
	const APREND_FRAMEBUFFER_TEXTURE_SPEC &spec = framebuffer->desc.attachments[index];
	return spec.existing_texture ? spec.existing_texture->desc.format : spec.format;
}

// Checks [sets] against the layouts [layouts] declares at each slot, the
// first [layout_count] of them. Fails, [error] saying why, if one doesn't
// match.
static APRESULT aprend_pass_sets_valid(
    APREND_ERROR *error,
    const aprend_binding_set *sets,
    const aprend_binding_layout *layouts,
    uint32_t layout_count) {
	for (uint32_t slot = 0; slot < APREND_MAX_BINDING_LAYOUTS; ++slot) {
		if (!sets[slot])
			continue;
		if (slot >= layout_count)
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "a binding set is given at slot %u, and the pipeline declares %u slots", slot, layout_count);
		if (!aprend_binding_set_fits(sets[slot], layouts[slot]))
			return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the binding set at slot %u is not of the layout the pipeline declares there", slot);
	}
	return APRESULT_OK;
}

// The views [pass] renders to now: its own, or its framebuffer's
// attachments. False if the framebuffer has lost one.
static bool aprend_render_pass_views(
    aprend_render_pass pass,
    aprend_texture_view *out_color,
    aprend_texture_view *out_depth) {
	const APREND_RENDER_PASS_DESC &desc = pass->desc;
	if (!desc._framebuffer) {
		for (uint32_t i = 0; i < desc._color_target_count; ++i)
			out_color[i] = desc._color_targets[i].view;
		*out_depth = desc._depth_target.view;
		return true;
	}
	for (uint32_t i = 0; i < desc._color_target_count; ++i) {
		out_color[i] = aprend_framebuffer_get_color_attachment(desc._framebuffer, i);
		if (!out_color[i])
			return false;
	}
	*out_depth = nullptr;
	if (desc._framebuffer->desc.depth_format != SPUDGPU_FORMAT_UNKNOWN) {
		*out_depth = aprend_framebuffer_get_depth_attachment(desc._framebuffer);
		if (!*out_depth)
			return false;
	}
	return true;
}

// The size of the mip of [view]'s texture the view covers.
static void aprend_pass_view_size(aprend_texture_view view, uint32_t *out_width, uint32_t *out_height) {
	*out_width  = aprend_mip_size(view->texture._t2d->desc.width, view->desc.base_mip);
	*out_height = aprend_mip_size(view->texture._t2d->desc.height, view->desc.base_mip);
}

bool aprend_render_pass_expand(aprend_command_list cmd_list, aprend_render_pass pass) {
	const APREND_RENDER_PASS_DESC &desc = pass->desc;
	aprend_texture_view color_views[APREND_RENDER_PASS_MAX_COLOR_TARGETS]{};
	aprend_texture_view depth_view = nullptr;
	if (!aprend_render_pass_views(pass, color_views, &depth_view)) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "BEGIN_RENDER_PASS with a framebuffer that has lost an attachment to a failed resize");
		return false;
	}

	APREND_COMMAND cmd{};
	APREND_COLOR_TARGET targets[APREND_RENDER_PASS_MAX_COLOR_TARGETS]{};
	for (uint32_t i = 0; i < desc._color_target_count; ++i) {
		targets[i]      = desc._color_targets[i];
		targets[i].view = color_views[i];
	}
	cmd._type                                       = APREND_COMMAND_BEGIN_RENDERING;
	cmd._params._begin_rendering._color_targets     = desc._color_target_count ? targets : nullptr;
	cmd._params._begin_rendering._color_target_count = desc._color_target_count;
	cmd._params._begin_rendering._depth_target      = desc._depth_target;
	cmd._params._begin_rendering._depth_target.view = depth_view;
	// 0 is the whole of the first target, which is the area the viewport
	// below covers too.
	cmd._params._begin_rendering._width  = 0;
	cmd._params._begin_rendering._height = 0;
	cmd._params._begin_rendering._timer      = desc._timer;
	cmd._params._begin_rendering._timer_slot = desc._timer_slot;
	aprend_command_list_send(cmd_list, cmd);

	cmd                                        = {};
	cmd._type                                  = APREND_COMMAND_SET_SHADER_PIPELINE;
	cmd._params._set_shader_pipeline._pipeline = desc._pipeline;
	aprend_command_list_send(cmd_list, cmd);

	uint32_t width  = 0;
	uint32_t height = 0;
	aprend_pass_view_size(desc._color_target_count ? color_views[0] : depth_view, &width, &height);
	const SPUDGPU_VIEWPORT viewport{0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
	cmd                                     = {};
	cmd._type                               = APREND_COMMAND_SET_VIEWPORTS;
	cmd._params._set_viewports._viewports      = &viewport;
	cmd._params._set_viewports._viewport_count = 1;
	aprend_command_list_send(cmd_list, cmd);

	const SPUDGPU_SCISSOR_RECT scissor{0.0f, 0.0f, (float)width, (float)height};
	cmd                                              = {};
	cmd._type                                        = APREND_COMMAND_SET_SCISSOR_RECTS;
	cmd._params._set_scissor_rects._scissor_rects      = &scissor;
	cmd._params._set_scissor_rects._scissor_rect_count = 1;
	aprend_command_list_send(cmd_list, cmd);

	for (uint32_t slot = 0; slot < APREND_MAX_BINDING_LAYOUTS; ++slot) {
		if (!desc._binding_sets[slot])
			continue;
		cmd                               = {};
		cmd._type                         = APREND_COMMAND_SET_BINDING_SET;
		cmd._params._set_binding_set._slot = slot;
		cmd._params._set_binding_set._set  = desc._binding_sets[slot];
		aprend_command_list_send(cmd_list, cmd);
	}
	return true;
}

bool aprend_compute_pass_expand(aprend_command_list cmd_list, aprend_compute_pass pass) {
	APREND_COMMAND cmd{};
	cmd._type                                   = APREND_COMMAND_SET_COMPUTE_PIPELINE;
	cmd._params._set_compute_pipeline._pipeline = pass->desc._pipeline;
	aprend_command_list_send(cmd_list, cmd);
	for (uint32_t slot = 0; slot < APREND_MAX_BINDING_LAYOUTS; ++slot) {
		if (!pass->desc._binding_sets[slot])
			continue;
		cmd                               = {};
		cmd._type                         = APREND_COMMAND_SET_BINDING_SET;
		cmd._params._set_binding_set._slot = slot;
		cmd._params._set_binding_set._set  = pass->desc._binding_sets[slot];
		aprend_command_list_send(cmd_list, cmd);
	}
	return true;
}

extern "C" {

APRESULT aprend_render_pass_create(
    aprend_instance instance,
    const APREND_RENDER_PASS_DESC *desc,
    aprend_render_pass *out_pass,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_pass)
		*out_pass = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_render_pass_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_render_pass_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_RENDER_PASS_DESC))
		return base;
	aprend_graphics_pipeline pipeline = desc->_pipeline;
	if (!pipeline)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_pipeline is NULL");
	if (pipeline->instance != instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_pipeline is of another instance");
	if (desc->_timer && desc->_timer->instance != instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_timer is of another instance");
	if (desc->_timer && desc->_timer_slot >= desc->_timer->slot_count)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_timer_slot %u is at or above the timer's %u slots", desc->_timer_slot, desc->_timer->slot_count);
	if (desc->_color_target_count > APREND_RENDER_PASS_MAX_COLOR_TARGETS)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_color_target_count %u is above APREND_RENDER_PASS_MAX_COLOR_TARGETS", desc->_color_target_count);
	const SPUDGPU_FORMAT *color_formats = pipeline->desc.color_attachment_formats;
	const SPUDGPU_FORMAT depth_format   = pipeline->desc.depth_format;
	const uint32_t wanted_colors        = pipeline->desc.color_attachment_count;
	if (desc->_color_target_count != wanted_colors)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_color_target_count is %u, and the pipeline renders to %u color targets", desc->_color_target_count, wanted_colors);
	if (desc->_color_target_count == 0 && depth_format == SPUDGPU_FORMAT_UNKNOWN)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the pipeline renders to no color target and no depth target");
	for (uint32_t i = 0; i < desc->_color_target_count; ++i) {
		if (desc->_color_targets[i].resolve_view)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_color_targets[%u] has a resolve_view, which SpudGPU doesn't support yet", i);
	}

	if (desc->_framebuffer) {
		aprend_framebuffer framebuffer = desc->_framebuffer;
		if (framebuffer->instance != instance)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_framebuffer is of another instance");
		if (framebuffer->desc.attachment_count != desc->_color_target_count)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_framebuffer has %u color attachments, and _color_target_count is %u", framebuffer->desc.attachment_count, desc->_color_target_count);
		for (uint32_t i = 0; i < desc->_color_target_count; ++i) {
			if (desc->_color_targets[i].view)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_color_targets[%u] has a view, and the pass renders to _framebuffer", i);
			if (aprend_framebuffer_attachment_format(framebuffer, i) != color_formats[i])
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "attachment %u of _framebuffer is of format %u, and the pipeline renders %u there", i, (unsigned)aprend_framebuffer_attachment_format(framebuffer, i), (unsigned)color_formats[i]);
		}
		if (desc->_depth_target.view)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_depth_target has a view, and the pass renders to _framebuffer");
		if (framebuffer->desc.depth_format != depth_format)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the depth attachment of _framebuffer is of format %u, and the pipeline's depth is %u", (unsigned)framebuffer->desc.depth_format, (unsigned)depth_format);
	} else {
		for (uint32_t i = 0; i < desc->_color_target_count; ++i) {
			if (!desc->_color_targets[i].view)
				return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_color_targets[%u] has no view", i);
			if (const APRESULT valid = aprend_pass_target_valid(out_error, "a color target", instance, desc->_color_targets[i].view,
			                                                    APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET, color_formats[i]))
				return valid;
		}
		if (depth_format == SPUDGPU_FORMAT_UNKNOWN && desc->_depth_target.view)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_depth_target has a view, and the pipeline has no depth");
		if (depth_format != SPUDGPU_FORMAT_UNKNOWN && !desc->_depth_target.view)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_depth_target has no view, and the pipeline has depth");
		if (desc->_depth_target.view)
			if (const APRESULT valid = aprend_pass_target_valid(out_error, "the depth target", instance, desc->_depth_target.view,
			                                                    APREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL, depth_format))
				return valid;
		// One shape and one layer count for every target: a pass is layered
		// or it isn't (APREND_COMMAND_BEGIN_RENDERING).
		aprend_texture_view first = desc->_color_target_count ? desc->_color_targets[0].view : desc->_depth_target.view;
		aprend_texture_view depth = desc->_depth_target.view;
		if (depth && depth->desc.shape != first->desc.shape)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the depth target is not the shape of the color target");
		if (depth && depth->desc.layer_count != first->desc.layer_count)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the depth target covers %u layers, and the color target %u", depth->desc.layer_count, first->desc.layer_count);
	}
	if (const APRESULT valid = aprend_pass_sets_valid(out_error, desc->_binding_sets, pipeline->desc._binding_layouts, pipeline->desc._binding_layout_count))
		return valid;
	if (!out_pass)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_pass is NULL");

	APREND_MALLOC__T(result, aprend_render_pass_t);
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the pass");
	APREND_CONSTRUCT__T(result, aprend_render_pass_t);
	result->instance = instance;
	result->desc     = *desc;
	*out_pass        = result;
	return APRESULT_OK;
}

void aprend_render_pass_destroy(aprend_render_pass pass) {
	if (!pass)
		return;
	if (APRI_STALE(pass, "aprend_render_pass_destroy", "pass"))
		return;
	// Nothing of the GPU's: every command that named the pass recorded what
	// it held, so it can go now.
	APREND_DESTRUCT__T(pass, aprend_render_pass_t);
}

APRESULT aprend_render_pass_set_binding_set(
    aprend_render_pass pass,
    uint32_t slot,
    aprend_binding_set set,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!pass)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "pass is NULL");
	if (APRI_STALE(pass, "aprend_render_pass_set_binding_set", "pass"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "pass is destroyed");
	APREND_ENTER(pass->instance, "aprend_render_pass_set_binding_set");
	const APREND_GRAPHICS_PIPELINE_DESC &pipeline = pass->desc._pipeline->desc;
	if (slot >= pipeline._binding_layout_count)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "slot %u, and the pipeline declares %u slots", slot, pipeline._binding_layout_count);
	if (set && APRI_STALE(set, "aprend_render_pass_set_binding_set", "set"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "set is destroyed");
	if (set && !aprend_binding_set_fits(set, pipeline._binding_layouts[slot]))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the set is not of the layout the pipeline declares at slot %u", slot);
	pass->desc._binding_sets[slot] = set;
	return APRESULT_OK;
}

aprend_graphics_pipeline aprend_render_pass_get_pipeline(aprend_render_pass pass) {
	if (!pass)
		return nullptr;
	if (APRI_STALE(pass, "aprend_render_pass_get_pipeline", "pass"))
		return nullptr;
	return pass->desc._pipeline;
}

APRESULT aprend_render_pass_get_size(
    aprend_render_pass pass,
    uint32_t *out_width,
    uint32_t *out_height) {
	if (!pass)
		return APRESULT_INVALID_ARGUMENT;
	if (APRI_STALE(pass, "aprend_render_pass_get_size", "pass"))
		return APRESULT_INVALID_ARGUMENT;
	uint32_t width  = 0;
	uint32_t height = 0;
	if (pass->desc._framebuffer) {
		width  = pass->desc._framebuffer->desc.width;
		height = pass->desc._framebuffer->desc.height;
	} else {
		aprend_texture_view first = pass->desc._color_target_count ? pass->desc._color_targets[0].view : pass->desc._depth_target.view;
		aprend_pass_view_size(first, &width, &height);
	}
	if (out_width)
		*out_width = width;
	if (out_height)
		*out_height = height;
	return APRESULT_OK;
}

APRESULT aprend_compute_pass_create(
    aprend_instance instance,
    const APREND_COMPUTE_PASS_DESC *desc,
    aprend_compute_pass *out_pass,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_pass)
		*out_pass = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_compute_pass_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_compute_pass_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_COMPUTE_PASS_DESC))
		return base;
	if (!desc->_pipeline)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_pipeline is NULL");
	if (desc->_pipeline->instance != instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_pipeline is of another instance");
	const APREND_COMPUTE_PIPELINE_DESC &pipeline = desc->_pipeline->desc;
	if (const APRESULT valid = aprend_pass_sets_valid(out_error, desc->_binding_sets, pipeline._binding_layouts, pipeline._binding_layout_count))
		return valid;
	if (!out_pass)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_pass is NULL");

	APREND_MALLOC__T(result, aprend_compute_pass_t);
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the pass");
	APREND_CONSTRUCT__T(result, aprend_compute_pass_t);
	result->instance = instance;
	result->desc     = *desc;
	*out_pass        = result;
	return APRESULT_OK;
}

void aprend_compute_pass_destroy(aprend_compute_pass pass) {
	if (!pass)
		return;
	if (APRI_STALE(pass, "aprend_compute_pass_destroy", "pass"))
		return;
	APREND_DESTRUCT__T(pass, aprend_compute_pass_t);
}

APRESULT aprend_compute_pass_set_binding_set(
    aprend_compute_pass pass,
    uint32_t slot,
    aprend_binding_set set,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!pass)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "pass is NULL");
	if (APRI_STALE(pass, "aprend_compute_pass_set_binding_set", "pass"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "pass is destroyed");
	APREND_ENTER(pass->instance, "aprend_compute_pass_set_binding_set");
	const APREND_COMPUTE_PIPELINE_DESC &pipeline = pass->desc._pipeline->desc;
	if (slot >= pipeline._binding_layout_count)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "slot %u, and the pipeline declares %u slots", slot, pipeline._binding_layout_count);
	if (set && APRI_STALE(set, "aprend_compute_pass_set_binding_set", "set"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "set is destroyed");
	if (set && !aprend_binding_set_fits(set, pipeline._binding_layouts[slot]))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the set is not of the layout the pipeline declares at slot %u", slot);
	pass->desc._binding_sets[slot] = set;
	return APRESULT_OK;
}

} // extern "C"
