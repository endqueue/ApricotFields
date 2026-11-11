
#include "rbtrendimages_internal.hpp"

/* Populates framebuffer->color_attachments / depth_attachment from
 * framebuffer->desc. On partial failure, whatever was already created is left
 * in place for the caller to clean up (via rbtrend_framebuffer_destroy_attachments),
 * and [error] says what failed.
 * Throws nothing: the vectors are reserved first, and no push_back below
 * grows them past that. */
static RBT_RESULT rbtrend_framebuffer_create_attachments(rbtrend_framebuffer_t *framebuffer, RBTREND_ERROR *error) {
	const RBTREND_FRAMEBUFFER_DESC &desc = framebuffer->desc;

	try {
		framebuffer->color_attachments.reserve(desc.attachment_count);
		framebuffer->owned_color_textures.reserve(desc.attachment_count);
	} catch (const std::bad_alloc &) {
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory for %u attachments", desc.attachment_count);
	}
	for (uint32_t i = 0; i < desc.attachment_count; ++i) {
		const RBTREND_FRAMEBUFFER_TEXTURE_SPEC &spec = desc.attachments[i];

		rbtrend_texture2d texture;
		bool owns_texture;
		if (spec.existing_texture) {
			texture      = spec.existing_texture;
			owns_texture = false;
		} else {
			RBTREND_TEXTURE2D_DESC tex_desc{};
			tex_desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE2D_DESC);
			tex_desc.width  = desc.width;
			tex_desc.height = desc.height;
			tex_desc.format = spec.format;
			tex_desc.usage  = RBTREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
			if (spec.shader_resource)
				tex_desc.usage |= RBTREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE;
			tex_desc.mip_levels   = 1; // attachments are single-mip; nothing generates a chain for them
			tex_desc.array_layers = 1;
			tex_desc.sample_count = desc.sample_count ? desc.sample_count : 1;

			if (const RBT_RESULT result = rbtrend_texture2d_create(framebuffer->instance, &tex_desc, &texture, error))
				return result;
			owns_texture = true;
		}

		// An attachment is mip 0 of layer 0, whatever else the texture has.
		RBTREND_TEXTURE_VIEW_DESC view_desc{};
		view_desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC);
		view_desc.type        = RBTREND_TEXTURE_VIEW_TYPE_RENDER_TARGET;
		view_desc.shape       = RBTREND_TEXTURE_VIEW_SHAPE_2D;
		view_desc.aspect      = RBTREND_TEXTURE_VIEW_ASPECT_COLOR;
		view_desc.base_mip    = 0;
		view_desc.mip_count   = 1;
		view_desc.base_layer  = 0;
		view_desc.layer_count = 1;
		rbtrend_texture_view view = nullptr;
		if (const RBT_RESULT result = rbtrend_texture_view_create_2d(texture, &view_desc, &view, error)) {
			if (owns_texture)
				rbtrend_texture2d_destroy(texture);
			return result;
		}
		framebuffer->color_attachments.push_back(view);
		framebuffer->owned_color_textures.push_back(owns_texture ? texture : nullptr);
	}

	if (desc.depth_format != CRGPU_FORMAT_UNKNOWN) {
		RBTREND_TEXTURE2D_DESC depth_desc{};
		depth_desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE2D_DESC);
		depth_desc.width        = desc.width;
		depth_desc.height       = desc.height;
		depth_desc.format       = desc.depth_format;
		depth_desc.usage        = RBTREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL;
		depth_desc.mip_levels   = 1;
		depth_desc.array_layers = 1;
		depth_desc.sample_count = desc.sample_count ? desc.sample_count : 1;

		rbtrend_texture2d depth_texture = nullptr;
		if (const RBT_RESULT result = rbtrend_texture2d_create(framebuffer->instance, &depth_desc, &depth_texture, error))
			return result;

		RBTREND_TEXTURE_VIEW_DESC depth_view_desc{};
		depth_view_desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_TEXTURE_VIEW_DESC);
		depth_view_desc.type        = RBTREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL;
		depth_view_desc.shape       = RBTREND_TEXTURE_VIEW_SHAPE_2D;
		// Every aspect of the format: the depth, and the stencil if it has one.
		depth_view_desc.aspect      = rbtrend_format_has_stencil(desc.depth_format) ? RBTREND_TEXTURE_VIEW_ASPECT_DEPTH_STENCIL : RBTREND_TEXTURE_VIEW_ASPECT_DEPTH;
		depth_view_desc.base_mip    = 0;
		depth_view_desc.mip_count   = 1;
		depth_view_desc.base_layer  = 0;
		depth_view_desc.layer_count = 1;
		if (const RBT_RESULT result = rbtrend_texture_view_create_2d(depth_texture, &depth_view_desc, &framebuffer->depth_attachment, error)) {
			rbtrend_texture2d_destroy(depth_texture);
			return result;
		}
		framebuffer->has_depth_attachment = true;
	}

	return RBT_RESULT_OK;
}

static void rbtrend_framebuffer_destroy_attachments(rbtrend_framebuffer_t *framebuffer) {
	for (size_t i = 0; i < framebuffer->color_attachments.size(); ++i) {
		rbtrend_texture_view_destroy(framebuffer->color_attachments[i]);
		if (framebuffer->owned_color_textures[i])
			rbtrend_texture2d_destroy(framebuffer->owned_color_textures[i]);
	}
	framebuffer->color_attachments.clear();
	framebuffer->owned_color_textures.clear();

	if (framebuffer->has_depth_attachment) {
		rbtrend_texture2d depth_texture = framebuffer->depth_attachment->texture._t2d;
		rbtrend_texture_view_destroy(framebuffer->depth_attachment);
		rbtrend_texture2d_destroy(depth_texture);
		framebuffer->depth_attachment     = nullptr;
		framebuffer->has_depth_attachment = false;
	}
}

rbtrend_framebuffer_t::~rbtrend_framebuffer_t() { rbtrend_framebuffer_destroy_attachments(this); }

extern "C" {
RBT_RESULT rbtrend_framebuffer_create(
    rbtrend_instance instance,
    const RBTREND_FRAMEBUFFER_DESC *desc,
    rbtrend_framebuffer *out_framebuffer,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_framebuffer)
		*out_framebuffer = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_framebuffer_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_framebuffer_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_FRAMEBUFFER_DESC))
		return base;
	if (desc->width == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "width is 0");
	if (desc->height == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "height is 0");
	if (desc->attachment_count == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "attachment_count is 0");
	if (!desc->attachments)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "attachments is NULL");
	if (!out_framebuffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_framebuffer is NULL");

	rbtrend_framebuffer_t *result = (rbtrend_framebuffer_t *)malloc(sizeof(rbtrend_framebuffer_t));
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the framebuffer");
	result = new (result) rbtrend_framebuffer_t();

	result->instance = instance;
	result->desc     = *desc;

	// Copy the attachment specs out of the caller's array — desc->attachments
	// may point at a local of the caller's, which
	// won't survive until the next rbtrend_framebuffer_resize() call.
	RBT_RESULT made = RBT_RESULT_OK;
	try {
		result->attachment_specs.assign(desc->attachments, desc->attachments + desc->attachment_count);
	} catch (const std::bad_alloc &) {
		made = rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the attachments' specs");
	}
	if (made == RBT_RESULT_OK) {
		result->desc.attachments = result->attachment_specs.data();
		made                     = rbtrend_framebuffer_create_attachments(result, out_error);
	}
	if (made != RBT_RESULT_OK) {
		result->~rbtrend_framebuffer_t();
		free(result);
		return made;
	}
	*out_framebuffer = result;
	return RBT_RESULT_OK;
}
void rbtrend_framebuffer_destroy(rbtrend_framebuffer framebuffer) {
	if (!framebuffer)
		return;
	if (RBT_STALE(framebuffer, "rbtrend_framebuffer_destroy", "framebuffer"))
		return;
	RBT_RETIRE(framebuffer->instance, framebuffer, rbtrend_framebuffer_t);
}
RBT_RESULT rbtrend_framebuffer_get_desc(
    rbtrend_framebuffer framebuffer,
    RBTREND_FRAMEBUFFER_DESC *out_desc) {
	if (!framebuffer)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(framebuffer, "rbtrend_framebuffer_get_desc", "framebuffer"))
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return RBT_RESULT_INVALID_ARGUMENT;
	*out_desc           = framebuffer->desc;
	out_desc->base.type = RBT_STRUCT_TYPE_REND_FRAMEBUFFER_DESC;
	out_desc->base.next = nullptr;
	return RBT_RESULT_OK;
}
RBT_RESULT rbtrend_framebuffer_resize(
    rbtrend_framebuffer framebuffer,
    uint32_t width,
    uint32_t height,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!framebuffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "framebuffer is NULL");
	if (RBT_STALE(framebuffer, "rbtrend_framebuffer_resize", "framebuffer"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "framebuffer is destroyed");
	RBTREND_ENTER(framebuffer->instance, "rbtrend_framebuffer_resize");
	if (width == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "width is 0");
	if (height == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "height is 0");
	if (framebuffer->desc.width == width && framebuffer->desc.height == height)
		return RBT_RESULT_OK;

	rbtrend_framebuffer_destroy_attachments(framebuffer);
	framebuffer->desc.width  = width;
	framebuffer->desc.height = height;
	return rbtrend_framebuffer_create_attachments(framebuffer, out_error);
}
RBT_RESULT rbtrend_framebuffer_clear_colors(
    rbtrend_framebuffer framebuffer,
    crgpu_command_queue queue,
    float r,
    float g,
    float b,
    float a,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!framebuffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "framebuffer is NULL");
	if (RBT_STALE(framebuffer, "rbtrend_framebuffer_clear_colors", "framebuffer"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "framebuffer is destroyed");
	RBTREND_ENTER(framebuffer->instance, "rbtrend_framebuffer_clear_colors");
	if (!queue)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "queue is NULL");
	if (framebuffer->color_attachments.empty())
		return RBT_RESULT_OK; // Nothing to clear.

	// Each attachment goes back to the layout it was found in (see
	// rbtrend_immediate_final_layout), and the tracked layouts are written
	// once the clears are recorded into the upload batch.
	const CRGPU_IMAGE_LAYOUT working = CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	const RBT_RESULT recorded            = rbtrend_record_immediate(framebuffer->instance, queue, out_error, [&](crgpu_command_list cmd) {
		for (rbtrend_texture_view view : framebuffer->color_attachments) {
			// The one mip of one layer the attachment's view covers.
			rbtrend_texture2d attachment                     = view->texture._t2d;
			const rbtrend_texture_range covered              = rbtrend_texture_view_range(view);
			const crgpu_image_view_desc_subresource_range range = rbtrend_texture_range_crgpu(covered);
			const CRGPU_IMAGE_LAYOUT found                = *attachment->tracking.at(covered.base_mip, covered.base_layer);
			const CRGPU_IMAGE_LAYOUT final_layout         = rbtrend_immediate_final_layout(found, working);
			crgpu_command_list_image_barrier_subresource(cmd, attachment->image, &range, found, working);
			crgpu_command_list_clear_color_attachment(
			    cmd, rbtrend_texture_view_get_crgpu_image_view(view), r, g, b, a, attachment->desc.width, attachment->desc.height);
			if (final_layout != working)
				crgpu_command_list_image_barrier_subresource(cmd, attachment->image, &range, working, final_layout);
		}
	});
	if (recorded != RBT_RESULT_OK)
		return recorded;
	for (rbtrend_texture_view view : framebuffer->color_attachments) {
		rbtrend_texture2d attachment   = view->texture._t2d;
		CRGPU_IMAGE_LAYOUT *tracked = attachment->tracking.at(view->desc.base_mip, view->desc.base_layer);
		*tracked                      = rbtrend_immediate_final_layout(*tracked, working);
	}
	return RBT_RESULT_OK;
}
RBT_RESULT rbtrend_framebuffer_clear_depth(
    rbtrend_framebuffer framebuffer,
    crgpu_command_queue queue,
    bool clear_depth,
    bool clear_stencil,
    float depth,
    uint32_t stencil,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!framebuffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "framebuffer is NULL");
	if (RBT_STALE(framebuffer, "rbtrend_framebuffer_clear_depth", "framebuffer"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "framebuffer is destroyed");
	RBTREND_ENTER(framebuffer->instance, "rbtrend_framebuffer_clear_depth");
	if (!queue)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "queue is NULL");
	if (!framebuffer->has_depth_attachment)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the framebuffer has no depth attachment");
	if (!clear_depth && !clear_stencil)
		return RBT_RESULT_OK; // Nothing to clear.

	rbtrend_texture_view view            = framebuffer->depth_attachment;
	rbtrend_texture2d attachment_texture = view->texture._t2d;

	return rbtrend_immediate_on_texture(
	    attachment_texture, queue, false, view->desc.base_mip, view->desc.base_layer, CRGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	    CRGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, out_error,
	    [&](crgpu_command_list cmd) {
		    crgpu_command_list_clear_depth_attachment(
		        cmd, rbtrend_texture_view_get_crgpu_image_view(view), clear_depth, clear_stencil, depth, stencil, attachment_texture->desc.width,
		        attachment_texture->desc.height);
	    });
}
rbtrend_texture_view rbtrend_framebuffer_get_color_attachment(
    rbtrend_framebuffer framebuffer,
    uint32_t index) {
	if (!framebuffer)
		return nullptr;
	if (RBT_STALE(framebuffer, "rbtrend_framebuffer_get_color_attachment", "framebuffer"))
		return nullptr;
	if (index < framebuffer->color_attachments.size())
		return framebuffer->color_attachments[index];
	return nullptr;
}
rbtrend_texture_view rbtrend_framebuffer_get_depth_attachment(rbtrend_framebuffer framebuffer) {
	if (!framebuffer)
		return nullptr;
	if (RBT_STALE(framebuffer, "rbtrend_framebuffer_get_depth_attachment", "framebuffer"))
		return nullptr;
	if (framebuffer->has_depth_attachment)
		return framebuffer->depth_attachment;
	else
		return nullptr;
}
}
