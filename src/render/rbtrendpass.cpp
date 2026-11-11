
#include "render/rbtrendpass.h"
#include "rbtrend_internal.hpp"
#include "rbtrendimages_internal.hpp"

#include <cstdio>
#include <cstdlib>

// Whether [view] can be a target of a pass of [instance] as [type], of a
// texture of [format]. Fails, [error] naming [what], if not.
static RBT_RESULT rbtrend_pass_target_valid(
    RBTREND_ERROR *error,
    const char *what,
    rbtrend_instance instance,
    rbtrend_texture_view view,
    RBTREND_TEXTURE_VIEW_TYPE type,
    CRGPU_FORMAT format) {
	if (view->instance != instance)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%s is of another instance", what);
	if (view->view_type != type)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%s is a view of type %u, and a pass takes %u there", what, (unsigned)view->view_type, (unsigned)type);
	if (view->dimension != RBTREND_TEXTURE_DIMENSION_2D)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%s is not a view of a 2D texture", what);
	if (view->texture._t2d->desc.format != format)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "%s is of format %u, and the pipeline renders to %u", what, (unsigned)view->texture._t2d->desc.format, (unsigned)format);
	return RBT_RESULT_OK;
}

// The format of attachment [index] of [framebuffer]: its texture's, where
// the caller gave one, else the one the framebuffer creates it with.
static CRGPU_FORMAT rbtrend_framebuffer_attachment_format(rbtrend_framebuffer framebuffer, uint32_t index) {
	const RBTREND_FRAMEBUFFER_TEXTURE_SPEC &spec = framebuffer->desc.attachments[index];
	return spec.existing_texture ? spec.existing_texture->desc.format : spec.format;
}

// Checks [sets] against the layouts [layouts] declares at each slot, the
// first [layout_count] of them. Fails, [error] saying why, if one doesn't
// match.
static RBT_RESULT rbtrend_pass_sets_valid(
    RBTREND_ERROR *error,
    const rbtrend_binding_set *sets,
    const rbtrend_binding_layout *layouts,
    uint32_t layout_count) {
	for (uint32_t slot = 0; slot < RBTREND_MAX_BINDING_LAYOUTS; ++slot) {
		if (!sets[slot])
			continue;
		if (slot >= layout_count)
			return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "a binding set is given at slot %u, and the pipeline declares %u slots", slot, layout_count);
		if (!rbtrend_binding_set_fits(sets[slot], layouts[slot]))
			return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "the binding set at slot %u is not of the layout the pipeline declares there", slot);
	}
	return RBT_RESULT_OK;
}

// The views [pass] renders to now: its own, or its framebuffer's
// attachments. False if the framebuffer has lost one.
static bool rbtrend_render_pass_views(
    rbtrend_render_pass pass,
    rbtrend_texture_view *out_color,
    rbtrend_texture_view *out_depth) {
	const RBTREND_RENDER_PASS_DESC &desc = pass->desc;
	if (!desc._framebuffer) {
		for (uint32_t i = 0; i < desc._color_target_count; ++i)
			out_color[i] = desc._color_targets[i].view;
		*out_depth = desc._depth_target.view;
		return true;
	}
	for (uint32_t i = 0; i < desc._color_target_count; ++i) {
		out_color[i] = rbtrend_framebuffer_get_color_attachment(desc._framebuffer, i);
		if (!out_color[i])
			return false;
	}
	*out_depth = nullptr;
	if (desc._framebuffer->desc.depth_format != CRGPU_FORMAT_UNKNOWN) {
		*out_depth = rbtrend_framebuffer_get_depth_attachment(desc._framebuffer);
		if (!*out_depth)
			return false;
	}
	return true;
}

// The size of the mip of [view]'s texture the view covers.
static void rbtrend_pass_view_size(rbtrend_texture_view view, uint32_t *out_width, uint32_t *out_height) {
	*out_width  = rbtrend_mip_size(view->texture._t2d->desc.width, view->desc.base_mip);
	*out_height = rbtrend_mip_size(view->texture._t2d->desc.height, view->desc.base_mip);
}

bool rbtrend_render_pass_expand(rbtrend_command_list cmd_list, rbtrend_render_pass pass) {
	const RBTREND_RENDER_PASS_DESC &desc = pass->desc;
	rbtrend_texture_view color_views[RBTREND_RENDER_PASS_MAX_COLOR_TARGETS]{};
	rbtrend_texture_view depth_view = nullptr;
	if (!rbtrend_render_pass_views(pass, color_views, &depth_view)) {
		rbtrend_cmd_report(cmd_list, RBT_RESULT_INVALID_ARGUMENT, "BEGIN_RENDER_PASS with a framebuffer that has lost an attachment to a failed resize");
		return false;
	}

	RBTREND_COMMAND cmd{};
	RBTREND_COLOR_TARGET targets[RBTREND_RENDER_PASS_MAX_COLOR_TARGETS]{};
	for (uint32_t i = 0; i < desc._color_target_count; ++i) {
		targets[i]      = desc._color_targets[i];
		targets[i].view = color_views[i];
	}
	cmd._type                                       = RBTREND_COMMAND_BEGIN_RENDERING;
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
	rbtrend_command_list_send(cmd_list, cmd);

	cmd                                        = {};
	cmd._type                                  = RBTREND_COMMAND_SET_GRAPHICS_PIPELINE;
	cmd._params._set_shader_pipeline._pipeline = desc._pipeline;
	rbtrend_command_list_send(cmd_list, cmd);

	uint32_t width  = 0;
	uint32_t height = 0;
	rbtrend_pass_view_size(desc._color_target_count ? color_views[0] : depth_view, &width, &height);
	const CRGPU_VIEWPORT viewport{0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
	cmd                                     = {};
	cmd._type                               = RBTREND_COMMAND_SET_VIEWPORTS;
	cmd._params._set_viewports._viewports      = &viewport;
	cmd._params._set_viewports._viewport_count = 1;
	rbtrend_command_list_send(cmd_list, cmd);

	const CRGPU_SCISSOR_RECT scissor{0.0f, 0.0f, (float)width, (float)height};
	cmd                                              = {};
	cmd._type                                        = RBTREND_COMMAND_SET_SCISSOR_RECTS;
	cmd._params._set_scissor_rects._scissor_rects      = &scissor;
	cmd._params._set_scissor_rects._scissor_rect_count = 1;
	rbtrend_command_list_send(cmd_list, cmd);

	for (uint32_t slot = 0; slot < RBTREND_MAX_BINDING_LAYOUTS; ++slot) {
		if (!desc._binding_sets[slot])
			continue;
		cmd                               = {};
		cmd._type                         = RBTREND_COMMAND_SET_BINDING_SET;
		cmd._params._set_binding_set._slot = slot;
		cmd._params._set_binding_set._set  = desc._binding_sets[slot];
		rbtrend_command_list_send(cmd_list, cmd);
	}
	return true;
}

bool rbtrend_compute_pass_expand(rbtrend_command_list cmd_list, rbtrend_compute_pass pass) {
	RBTREND_COMMAND cmd{};
	cmd._type                                   = RBTREND_COMMAND_SET_COMPUTE_PIPELINE;
	cmd._params._set_compute_pipeline._pipeline = pass->desc._pipeline;
	rbtrend_command_list_send(cmd_list, cmd);
	for (uint32_t slot = 0; slot < RBTREND_MAX_BINDING_LAYOUTS; ++slot) {
		if (!pass->desc._binding_sets[slot])
			continue;
		cmd                               = {};
		cmd._type                         = RBTREND_COMMAND_SET_BINDING_SET;
		cmd._params._set_binding_set._slot = slot;
		cmd._params._set_binding_set._set  = pass->desc._binding_sets[slot];
		rbtrend_command_list_send(cmd_list, cmd);
	}
	return true;
}

extern "C" {

RBT_RESULT rbtrend_render_pass_create(
    rbtrend_instance instance,
    const RBTREND_RENDER_PASS_DESC *desc,
    rbtrend_render_pass *out_pass,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_pass)
		*out_pass = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_render_pass_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_render_pass_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_RENDER_PASS_DESC))
		return base;
	rbtrend_graphics_pipeline pipeline = desc->_pipeline;
	if (!pipeline)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_pipeline is NULL");
	if (pipeline->instance != instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_pipeline is of another instance");
	if (desc->_timer && desc->_timer->instance != instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_timer is of another instance");
	if (desc->_timer && desc->_timer_slot >= desc->_timer->slot_count)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_timer_slot %u is at or above the timer's %u slots", desc->_timer_slot, desc->_timer->slot_count);
	if (desc->_color_target_count > RBTREND_RENDER_PASS_MAX_COLOR_TARGETS)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_color_target_count %u is above RBTREND_RENDER_PASS_MAX_COLOR_TARGETS", desc->_color_target_count);
	const CRGPU_FORMAT *color_formats = pipeline->desc.color_attachment_formats;
	const CRGPU_FORMAT depth_format   = pipeline->desc.depth_format;
	const uint32_t wanted_colors        = pipeline->desc.color_attachment_count;
	if (desc->_color_target_count != wanted_colors)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_color_target_count is %u, and the pipeline renders to %u color targets", desc->_color_target_count, wanted_colors);
	if (desc->_color_target_count == 0 && depth_format == CRGPU_FORMAT_UNKNOWN)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the pipeline renders to no color target and no depth target");
	for (uint32_t i = 0; i < desc->_color_target_count; ++i) {
		if (desc->_color_targets[i].resolve_view)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_color_targets[%u] has a resolve_view, which Carrottus GPU doesn't support yet", i);
	}

	if (desc->_framebuffer) {
		rbtrend_framebuffer framebuffer = desc->_framebuffer;
		if (framebuffer->instance != instance)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_framebuffer is of another instance");
		if (framebuffer->desc.attachment_count != desc->_color_target_count)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_framebuffer has %u color attachments, and _color_target_count is %u", framebuffer->desc.attachment_count, desc->_color_target_count);
		for (uint32_t i = 0; i < desc->_color_target_count; ++i) {
			if (desc->_color_targets[i].view)
				return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_color_targets[%u] has a view, and the pass renders to _framebuffer", i);
			if (rbtrend_framebuffer_attachment_format(framebuffer, i) != color_formats[i])
				return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "attachment %u of _framebuffer is of format %u, and the pipeline renders %u there", i, (unsigned)rbtrend_framebuffer_attachment_format(framebuffer, i), (unsigned)color_formats[i]);
		}
		if (desc->_depth_target.view)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_depth_target has a view, and the pass renders to _framebuffer");
		if (framebuffer->desc.depth_format != depth_format)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the depth attachment of _framebuffer is of format %u, and the pipeline's depth is %u", (unsigned)framebuffer->desc.depth_format, (unsigned)depth_format);
	} else {
		for (uint32_t i = 0; i < desc->_color_target_count; ++i) {
			if (!desc->_color_targets[i].view)
				return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_color_targets[%u] has no view", i);
			if (const RBT_RESULT valid = rbtrend_pass_target_valid(out_error, "a color target", instance, desc->_color_targets[i].view,
			                                                    RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET, color_formats[i]))
				return valid;
		}
		if (depth_format == CRGPU_FORMAT_UNKNOWN && desc->_depth_target.view)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_depth_target has a view, and the pipeline has no depth");
		if (depth_format != CRGPU_FORMAT_UNKNOWN && !desc->_depth_target.view)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_depth_target has no view, and the pipeline has depth");
		if (desc->_depth_target.view)
			if (const RBT_RESULT valid = rbtrend_pass_target_valid(out_error, "the depth target", instance, desc->_depth_target.view,
			                                                    RBTREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL, depth_format))
				return valid;
		// One shape and one layer count for every target: a pass is layered
		// or it isn't (RBTREND_COMMAND_BEGIN_RENDERING).
		rbtrend_texture_view first = desc->_color_target_count ? desc->_color_targets[0].view : desc->_depth_target.view;
		rbtrend_texture_view depth = desc->_depth_target.view;
		if (depth && depth->desc.shape != first->desc.shape)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the depth target is not the shape of the color target");
		if (depth && depth->desc.layer_count != first->desc.layer_count)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the depth target covers %u layers, and the color target %u", depth->desc.layer_count, first->desc.layer_count);
	}
	if (const RBT_RESULT valid = rbtrend_pass_sets_valid(out_error, desc->_binding_sets, pipeline->desc._binding_layouts, pipeline->desc._binding_layout_count))
		return valid;
	if (!out_pass)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_pass is NULL");

	RBTREND_MALLOC__T(result, rbtrend_render_pass_t);
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the pass");
	RBTREND_CONSTRUCT__T(result, rbtrend_render_pass_t);
	result->instance = instance;
	result->desc     = *desc;
	*out_pass        = result;
	return RBT_RESULT_OK;
}

void rbtrend_render_pass_destroy(rbtrend_render_pass pass) {
	if (!pass)
		return;
	if (RBT_STALE(pass, "rbtrend_render_pass_destroy", "pass"))
		return;
	// Nothing of the GPU's: every command that named the pass recorded what
	// it held, so it can go now.
	RBT_RETIRE(pass->instance, pass, rbtrend_render_pass_t);
}

RBT_RESULT rbtrend_render_pass_set_binding_set(
    rbtrend_render_pass pass,
    uint32_t slot,
    rbtrend_binding_set set,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!pass)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pass is NULL");
	if (RBT_STALE(pass, "rbtrend_render_pass_set_binding_set", "pass"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pass is destroyed");
	RBTREND_ENTER(pass->instance, "rbtrend_render_pass_set_binding_set");
	const RBTREND_GRAPHICS_PIPELINE_DESC &pipeline = pass->desc._pipeline->desc;
	if (slot >= pipeline._binding_layout_count)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "slot %u, and the pipeline declares %u slots", slot, pipeline._binding_layout_count);
	if (set && RBT_STALE(set, "rbtrend_render_pass_set_binding_set", "set"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "set is destroyed");
	if (set && !rbtrend_binding_set_fits(set, pipeline._binding_layouts[slot]))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the set is not of the layout the pipeline declares at slot %u", slot);
	pass->desc._binding_sets[slot] = set;
	return RBT_RESULT_OK;
}

rbtrend_graphics_pipeline rbtrend_render_pass_get_pipeline(rbtrend_render_pass pass) {
	if (!pass)
		return nullptr;
	if (RBT_STALE(pass, "rbtrend_render_pass_get_pipeline", "pass"))
		return nullptr;
	return pass->desc._pipeline;
}

RBT_RESULT rbtrend_render_pass_get_size(
    rbtrend_render_pass pass,
    uint32_t *out_width,
    uint32_t *out_height) {
	if (!pass)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(pass, "rbtrend_render_pass_get_size", "pass"))
		return RBT_RESULT_INVALID_ARGUMENT;
	uint32_t width  = 0;
	uint32_t height = 0;
	if (pass->desc._framebuffer) {
		width  = pass->desc._framebuffer->desc.width;
		height = pass->desc._framebuffer->desc.height;
	} else {
		rbtrend_texture_view first = pass->desc._color_target_count ? pass->desc._color_targets[0].view : pass->desc._depth_target.view;
		rbtrend_pass_view_size(first, &width, &height);
	}
	if (out_width)
		*out_width = width;
	if (out_height)
		*out_height = height;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_compute_pass_create(
    rbtrend_instance instance,
    const RBTREND_COMPUTE_PASS_DESC *desc,
    rbtrend_compute_pass *out_pass,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_pass)
		*out_pass = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_compute_pass_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_compute_pass_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_COMPUTE_PASS_DESC))
		return base;
	if (!desc->_pipeline)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_pipeline is NULL");
	if (desc->_pipeline->instance != instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_pipeline is of another instance");
	const RBTREND_COMPUTE_PIPELINE_DESC &pipeline = desc->_pipeline->desc;
	if (const RBT_RESULT valid = rbtrend_pass_sets_valid(out_error, desc->_binding_sets, pipeline._binding_layouts, pipeline._binding_layout_count))
		return valid;
	if (!out_pass)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_pass is NULL");

	RBTREND_MALLOC__T(result, rbtrend_compute_pass_t);
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the pass");
	RBTREND_CONSTRUCT__T(result, rbtrend_compute_pass_t);
	result->instance = instance;
	result->desc     = *desc;
	*out_pass        = result;
	return RBT_RESULT_OK;
}

void rbtrend_compute_pass_destroy(rbtrend_compute_pass pass) {
	if (!pass)
		return;
	if (RBT_STALE(pass, "rbtrend_compute_pass_destroy", "pass"))
		return;
	RBT_RETIRE(pass->instance, pass, rbtrend_compute_pass_t);
}

RBT_RESULT rbtrend_compute_pass_set_binding_set(
    rbtrend_compute_pass pass,
    uint32_t slot,
    rbtrend_binding_set set,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!pass)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pass is NULL");
	if (RBT_STALE(pass, "rbtrend_compute_pass_set_binding_set", "pass"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pass is destroyed");
	RBTREND_ENTER(pass->instance, "rbtrend_compute_pass_set_binding_set");
	const RBTREND_COMPUTE_PIPELINE_DESC &pipeline = pass->desc._pipeline->desc;
	if (slot >= pipeline._binding_layout_count)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "slot %u, and the pipeline declares %u slots", slot, pipeline._binding_layout_count);
	if (set && RBT_STALE(set, "rbtrend_compute_pass_set_binding_set", "set"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "set is destroyed");
	if (set && !rbtrend_binding_set_fits(set, pipeline._binding_layouts[slot]))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the set is not of the layout the pipeline declares at slot %u", slot);
	pass->desc._binding_sets[slot] = set;
	return RBT_RESULT_OK;
}

} // extern "C"
