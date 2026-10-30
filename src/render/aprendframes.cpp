
#include "aprendimages_internal.hpp"

/* Populates framebuffer->color_attachments / depth_attachment from
 * framebuffer->desc. On partial failure, whatever was already created is left
 * in place for the caller to clean up (via aprend_framebuffer_destroy_attachments),
 * and [error] says what failed.
 * Throws nothing: the vectors are reserved first, and no push_back below
 * grows them past that. */
static APRESULT aprend_framebuffer_create_attachments(aprend_framebuffer_t *framebuffer, APREND_ERROR *error) {
	const APREND_FRAMEBUFFER_DESC &desc = framebuffer->desc;

	try {
		framebuffer->color_attachments.reserve(desc.attachment_count);
		framebuffer->owned_color_textures.reserve(desc.attachment_count);
	} catch (const std::bad_alloc &) {
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for %u attachments", desc.attachment_count);
	}
	for (uint32_t i = 0; i < desc.attachment_count; ++i) {
		const APREND_FRAMEBUFFER_TEXTURE_SPEC &spec = desc.attachments[i];

		aprend_texture2d texture;
		bool owns_texture;
		if (spec.existing_texture) {
			texture      = spec.existing_texture;
			owns_texture = false;
		} else {
			APREND_TEXTURE2D_DESC tex_desc{};
			tex_desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC);
			tex_desc.width  = desc.width;
			tex_desc.height = desc.height;
			tex_desc.format = spec.format;
			tex_desc.usage  = APREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
			if (spec.shader_resource)
				tex_desc.usage |= APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE;
			tex_desc.mip_levels   = 1; // attachments are single-mip; nothing generates a chain for them
			tex_desc.array_layers = 1;
			tex_desc.sample_count = desc.sample_count ? desc.sample_count : 1;

			if (const APRESULT result = aprend_texture2d_create(framebuffer->instance, &tex_desc, &texture, error))
				return result;
			owns_texture = true;
		}

		// An attachment is mip 0 of layer 0, whatever else the texture has.
		APREND_TEXTURE_VIEW_DESC view_desc{};
		view_desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
		view_desc.type        = APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET;
		view_desc.shape       = APREND_TEXTURE_VIEW_SHAPE_2D;
		view_desc.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
		view_desc.base_mip    = 0;
		view_desc.mip_count   = 1;
		view_desc.base_layer  = 0;
		view_desc.layer_count = 1;
		aprend_texture_view view = nullptr;
		if (const APRESULT result = aprend_texture_view_create_2d(texture, &view_desc, &view, error)) {
			if (owns_texture)
				aprend_texture2d_destroy(texture);
			return result;
		}
		framebuffer->color_attachments.push_back(view);
		framebuffer->owned_color_textures.push_back(owns_texture ? texture : nullptr);
	}

	if (desc.depth_format != SPUDGPU_FORMAT_UNKNOWN) {
		APREND_TEXTURE2D_DESC depth_desc{};
		depth_desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC);
		depth_desc.width        = desc.width;
		depth_desc.height       = desc.height;
		depth_desc.format       = desc.depth_format;
		depth_desc.usage        = APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL;
		depth_desc.mip_levels   = 1;
		depth_desc.array_layers = 1;
		depth_desc.sample_count = desc.sample_count ? desc.sample_count : 1;

		aprend_texture2d depth_texture = nullptr;
		if (const APRESULT result = aprend_texture2d_create(framebuffer->instance, &depth_desc, &depth_texture, error))
			return result;

		APREND_TEXTURE_VIEW_DESC depth_view_desc{};
		depth_view_desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
		depth_view_desc.type        = APREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL;
		depth_view_desc.shape       = APREND_TEXTURE_VIEW_SHAPE_2D;
		// Every aspect of the format: the depth, and the stencil if it has one.
		depth_view_desc.aspect      = aprend_format_has_stencil(desc.depth_format) ? APREND_TEXTURE_VIEW_ASPECT_DEPTH_STENCIL : APREND_TEXTURE_VIEW_ASPECT_DEPTH;
		depth_view_desc.base_mip    = 0;
		depth_view_desc.mip_count   = 1;
		depth_view_desc.base_layer  = 0;
		depth_view_desc.layer_count = 1;
		if (const APRESULT result = aprend_texture_view_create_2d(depth_texture, &depth_view_desc, &framebuffer->depth_attachment, error)) {
			aprend_texture2d_destroy(depth_texture);
			return result;
		}
		framebuffer->has_depth_attachment = true;
	}

	return APRESULT_OK;
}

static void aprend_framebuffer_destroy_attachments(aprend_framebuffer_t *framebuffer) {
	for (size_t i = 0; i < framebuffer->color_attachments.size(); ++i) {
		aprend_texture_view_destroy(framebuffer->color_attachments[i]);
		if (framebuffer->owned_color_textures[i])
			aprend_texture2d_destroy(framebuffer->owned_color_textures[i]);
	}
	framebuffer->color_attachments.clear();
	framebuffer->owned_color_textures.clear();

	if (framebuffer->has_depth_attachment) {
		aprend_texture2d depth_texture = framebuffer->depth_attachment->texture._t2d;
		aprend_texture_view_destroy(framebuffer->depth_attachment);
		aprend_texture2d_destroy(depth_texture);
		framebuffer->depth_attachment     = nullptr;
		framebuffer->has_depth_attachment = false;
	}
}

aprend_framebuffer_t::~aprend_framebuffer_t() { aprend_framebuffer_destroy_attachments(this); }

extern "C" {
APRESULT aprend_framebuffer_create(
    aprend_instance instance,
    const APREND_FRAMEBUFFER_DESC *desc,
    aprend_framebuffer *out_framebuffer,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_framebuffer)
		*out_framebuffer = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_framebuffer_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_framebuffer_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_FRAMEBUFFER_DESC))
		return base;
	if (desc->width == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "width is 0");
	if (desc->height == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "height is 0");
	if (desc->attachment_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "attachment_count is 0");
	if (!desc->attachments)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "attachments is NULL");
	if (!out_framebuffer)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_framebuffer is NULL");

	aprend_framebuffer_t *result = (aprend_framebuffer_t *)malloc(sizeof(aprend_framebuffer_t));
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the framebuffer");
	result = new (result) aprend_framebuffer_t();

	result->instance = instance;
	result->desc     = *desc;

	// Copy the attachment specs out of the caller's array — desc->attachments
	// may point at a local of the caller's, which
	// won't survive until the next aprend_framebuffer_resize() call.
	APRESULT made = APRESULT_OK;
	try {
		result->attachment_specs.assign(desc->attachments, desc->attachments + desc->attachment_count);
	} catch (const std::bad_alloc &) {
		made = aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the attachments' specs");
	}
	if (made == APRESULT_OK) {
		result->desc.attachments = result->attachment_specs.data();
		made                     = aprend_framebuffer_create_attachments(result, out_error);
	}
	if (made != APRESULT_OK) {
		result->~aprend_framebuffer_t();
		free(result);
		return made;
	}
	*out_framebuffer = result;
	return APRESULT_OK;
}
void aprend_framebuffer_destroy(aprend_framebuffer framebuffer) {
	if (!framebuffer)
		return;
	if (APRI_STALE(framebuffer, "aprend_framebuffer_destroy", "framebuffer"))
		return;
	framebuffer->~aprend_framebuffer_t();
	free(framebuffer);
}
APRESULT aprend_framebuffer_get_desc(
    aprend_framebuffer framebuffer,
    APREND_FRAMEBUFFER_DESC *out_desc) {
	if (!framebuffer)
		return APRESULT_INVALID_ARGUMENT;
	if (APRI_STALE(framebuffer, "aprend_framebuffer_get_desc", "framebuffer"))
		return APRESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return APRESULT_INVALID_ARGUMENT;
	*out_desc           = framebuffer->desc;
	out_desc->base.type = APRI_STRUCT_TYPE_APREND_FRAMEBUFFER_DESC;
	out_desc->base.next = nullptr;
	return APRESULT_OK;
}
APRESULT aprend_framebuffer_resize(
    aprend_framebuffer framebuffer,
    uint32_t width,
    uint32_t height,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!framebuffer)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "framebuffer is NULL");
	if (APRI_STALE(framebuffer, "aprend_framebuffer_resize", "framebuffer"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "framebuffer is destroyed");
	APREND_ENTER(framebuffer->instance, "aprend_framebuffer_resize");
	if (width == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "width is 0");
	if (height == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "height is 0");
	if (framebuffer->desc.width == width && framebuffer->desc.height == height)
		return APRESULT_OK;

	aprend_framebuffer_destroy_attachments(framebuffer);
	framebuffer->desc.width  = width;
	framebuffer->desc.height = height;
	return aprend_framebuffer_create_attachments(framebuffer, out_error);
}
APRESULT aprend_framebuffer_clear_colors(
    aprend_framebuffer framebuffer,
    spudgpu_command_queue queue,
    float r,
    float g,
    float b,
    float a,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!framebuffer)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "framebuffer is NULL");
	if (APRI_STALE(framebuffer, "aprend_framebuffer_clear_colors", "framebuffer"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "framebuffer is destroyed");
	APREND_ENTER(framebuffer->instance, "aprend_framebuffer_clear_colors");
	if (!queue)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "queue is NULL");
	if (framebuffer->color_attachments.empty())
		return APRESULT_OK; // Nothing to clear.

	// Each attachment goes back to the layout it was found in (see
	// aprend_immediate_final_layout), and the tracked layouts are written
	// once the clears are recorded into the upload batch.
	const SPUDGPU_IMAGE_LAYOUT working = SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	const APRESULT recorded            = aprend_record_immediate(framebuffer->instance, queue, out_error, [&](spudgpu_command_list cmd) {
		for (aprend_texture_view view : framebuffer->color_attachments) {
			// The one mip of one layer the attachment's view covers.
			aprend_texture2d attachment                     = view->texture._t2d;
			const aprend_texture_range covered              = aprend_texture_view_range(view);
			const spudgpu_image_view_desc_subresource_range range = aprend_texture_range_spudgpu(covered);
			const SPUDGPU_IMAGE_LAYOUT found                = *attachment->tracking.at(covered.base_mip, covered.base_layer);
			const SPUDGPU_IMAGE_LAYOUT final_layout         = aprend_immediate_final_layout(found, working);
			spudgpu_cmd_image_barrier_subresource(cmd, attachment->image, &range, found, working);
			spudgpu_cmd_clear_color_attachment(
			    cmd, aprend_texture_view_get_spudgpu_image_view(view), r, g, b, a, attachment->desc.width, attachment->desc.height);
			if (final_layout != working)
				spudgpu_cmd_image_barrier_subresource(cmd, attachment->image, &range, working, final_layout);
		}
	});
	if (recorded != APRESULT_OK)
		return recorded;
	for (aprend_texture_view view : framebuffer->color_attachments) {
		aprend_texture2d attachment   = view->texture._t2d;
		SPUDGPU_IMAGE_LAYOUT *tracked = attachment->tracking.at(view->desc.base_mip, view->desc.base_layer);
		*tracked                      = aprend_immediate_final_layout(*tracked, working);
	}
	return APRESULT_OK;
}
APRESULT aprend_framebuffer_clear_depth(
    aprend_framebuffer framebuffer,
    spudgpu_command_queue queue,
    bool clear_depth,
    bool clear_stencil,
    float depth,
    uint32_t stencil,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!framebuffer)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "framebuffer is NULL");
	if (APRI_STALE(framebuffer, "aprend_framebuffer_clear_depth", "framebuffer"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "framebuffer is destroyed");
	APREND_ENTER(framebuffer->instance, "aprend_framebuffer_clear_depth");
	if (!queue)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "queue is NULL");
	if (!framebuffer->has_depth_attachment)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the framebuffer has no depth attachment");
	if (!clear_depth && !clear_stencil)
		return APRESULT_OK; // Nothing to clear.

	aprend_texture_view view            = framebuffer->depth_attachment;
	aprend_texture2d attachment_texture = view->texture._t2d;

	return aprend_immediate_on_texture(
	    attachment_texture, queue, false, view->desc.base_mip, view->desc.base_layer, SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
	    SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, out_error,
	    [&](spudgpu_command_list cmd) {
		    spudgpu_cmd_clear_depth_attachment(
		        cmd, aprend_texture_view_get_spudgpu_image_view(view), clear_depth, clear_stencil, depth, stencil, attachment_texture->desc.width,
		        attachment_texture->desc.height);
	    });
}
aprend_texture_view aprend_framebuffer_get_color_attachment(
    aprend_framebuffer framebuffer,
    uint32_t index) {
	if (!framebuffer)
		return nullptr;
	if (APRI_STALE(framebuffer, "aprend_framebuffer_get_color_attachment", "framebuffer"))
		return nullptr;
	if (index < framebuffer->color_attachments.size())
		return framebuffer->color_attachments[index];
	return nullptr;
}
aprend_texture_view aprend_framebuffer_get_depth_attachment(aprend_framebuffer framebuffer) {
	if (!framebuffer)
		return nullptr;
	if (APRI_STALE(framebuffer, "aprend_framebuffer_get_depth_attachment", "framebuffer"))
		return nullptr;
	if (framebuffer->has_depth_attachment)
		return framebuffer->depth_attachment;
	else
		return nullptr;
}
}
