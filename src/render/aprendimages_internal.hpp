
#include "render/aprendimages.h"
#include "render/aprendframes.h"
#include "aprend_internal.hpp"
#include <cstdlib>
#include <cstring>
#include <vector>

/* Records `record` onto a throwaway command list and submits it on [queue] as
 * one of the instance's submissions (aprend_instance_submit).
 *
 * With [wait], blocks until it has finished: a wait on the instance's fence
 * for this submission's serial, not on the whole queue. For work whose
 * result the caller reads back.
 *
 * Without, returns once it is submitted. The list and its allocator go on
 * the instance's release queue, and whatever the commands read (a staging
 * buffer) is the caller's to retire the same way, after this returns. The
 * work runs in submission order, ahead of anything submitted later.
 *
 * Not for hot paths: it makes an allocator and a list each time. Only used
 * for one-shot texture update/readback/clear operations. Shared by
 * aprendimages.cpp and aprendframes.cpp. */
template <typename Fn> static bool aprend_submit_immediate(aprend_instance instance, spudgpu_command_queue queue, bool wait, Fn &&record) {
	spudgpu_command_allocator_desc alloc_desc{};
	alloc_desc.type = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;

	spudgpu_command_allocator allocator;
	if (spudgpu_create_command_allocator(instance->desc.device, &alloc_desc, &allocator) != SPUD_SUCCESS)
		return false;

	spudgpu_command_list cmd;
	if (spudgpu_create_command_list(allocator, &cmd) != SPUD_SUCCESS) {
		spudgpu_destroy_command_allocator(allocator);
		return false;
	}

	spudgpu_begin_command_list(cmd);
	record(cmd);
	spudgpu_end_command_list(cmd);

	spudgpu_command_list cmd_lists[] = {cmd};
	uint64_t serial                  = 0;
	bool ok                          = !SPUDFAIL(aprend_instance_submit(instance, queue, cmd_lists, 1, nullptr, &serial));
	if (!wait) {
		// Released once the submission has run; at once if it was refused,
		// since nothing was submitted. The list goes before its allocator.
		aprend_instance_retire(instance, cmd, &aprend_release_spudgpu_command_list);
		aprend_instance_retire(instance, allocator, &aprend_release_spudgpu_command_allocator);
		return ok;
	}
	// The list and its allocator are destroyed below, and the caller reads
	// what the commands wrote: both need them to have run.
	if (ok)
		ok = !SPUDFAIL(aprend_instance_wait_serial(instance, serial));

	spudgpu_destroy_command_list(cmd);
	spudgpu_destroy_command_allocator(allocator);
	return ok;
}

/* One side of mip [level] of a texture whose level 0 is [size] along it. */
inline uint32_t aprend_mip_size(uint32_t size, uint32_t level) {
	// A shift by the type's width or more is undefined; such a level is 1.
	const uint32_t mip_size = level < 32 ? size >> level : 0;
	return mip_size ? mip_size : 1;
}

/* The image layout Aprend tracks for a texture: one for each (mip, layer),
 * since a view can cover part of a texture and one mip can be rendered to
 * while another is sampled. It is the layout the GPU will see once
 * everything submitted so far has run. A 3D texture has one layer.
 *
 * Plain storage, zeroed by calloc, and SPUDGPU_IMAGE_LAYOUT_UNDEFINED is 0:
 * a new texture's every subresource starts with no layout. */
struct aprend_texture_layouts {
	SPUDGPU_IMAGE_LAYOUT *layouts{nullptr};
	uint32_t mip_levels{0};
	uint32_t array_layers{0};

	SPUDGPU_IMAGE_LAYOUT *at(uint32_t mip, uint32_t layer) const { return &layouts[mip + (size_t)layer * mip_levels]; }
};

/* Gives [tracking] room for [mip_levels] by [array_layers] layouts, all
 * UNDEFINED. With the counts it already has, the storage is kept and only
 * cleared, so a pointer a compiled command list holds into it stays valid;
 * with others it is replaced. False, with [tracking] as it was, if there is
 * no memory. */
inline bool aprend_texture_layouts_reset(aprend_texture_layouts *tracking, uint32_t mip_levels, uint32_t array_layers) {
	const size_t count = (size_t)mip_levels * array_layers;
	if (tracking->layouts && tracking->mip_levels == mip_levels && tracking->array_layers == array_layers) {
		memset(tracking->layouts, 0, count * sizeof(SPUDGPU_IMAGE_LAYOUT));
		return true;
	}
	SPUDGPU_IMAGE_LAYOUT *layouts = (SPUDGPU_IMAGE_LAYOUT *)calloc(count, sizeof(SPUDGPU_IMAGE_LAYOUT));
	if (!layouts)
		return false;
	free(tracking->layouts);
	tracking->layouts      = layouts;
	tracking->mip_levels   = mip_levels;
	tracking->array_layers = array_layers;
	return true;
}

/* Some mips and layers of one texture: what a view covers, and what a
 * barrier moves. */
struct aprend_texture_range {
	spudgpu_image image;
	aprend_texture_layouts *tracking;
	uint32_t base_mip;
	uint32_t mip_count;
	uint32_t base_layer;
	uint32_t layer_count;
};

/* Whether two ranges share a (mip, layer) of the same image. */
inline bool aprend_texture_ranges_overlap(const aprend_texture_range &a, const aprend_texture_range &b) {
	if (a.image != b.image)
		return false;
	if (a.base_mip >= b.base_mip + b.mip_count || b.base_mip >= a.base_mip + a.mip_count)
		return false;
	if (a.base_layer >= b.base_layer + b.layer_count || b.base_layer >= a.base_layer + a.layer_count)
		return false;
	return true;
}

/* Whether [format] is a depth format that also has a stencil. */
inline bool aprend_format_has_stencil(SPUDGPU_FORMAT format) {
	return format == SPUDGPU_FORMAT_D24_UNORM_S8_UINT || format == SPUDGPU_FORMAT_D32_FLOAT_S8X24_UINT;
}

/* [range] as SpudGPU's barrier and view calls take it. The aspect is the
 * image's own: SpudGPU's barriers read it from the image. */
inline spudgpu_image_view_desc_subresource_range aprend_texture_range_spudgpu(const aprend_texture_range &range) {
	spudgpu_image_view_desc_subresource_range out{};
	out.base_mip_level    = range.base_mip;
	out.mip_level_count   = range.mip_count;
	out.base_array_layer  = range.base_layer;
	out.array_layer_count = range.layer_count;
	return out;
}

/* The layout an immediate operation leaves a texture in: the one it found it
 * in, so the texture's tracked layout doesn't change and command lists already
 * compiled against it stay submittable. A texture with no layout yet
 * (UNDEFINED) can't be put back, and ends in [if_undefined]. */
inline SPUDGPU_IMAGE_LAYOUT aprend_immediate_final_layout(SPUDGPU_IMAGE_LAYOUT found, SPUDGPU_IMAGE_LAYOUT if_undefined) {
	return found == SPUDGPU_IMAGE_LAYOUT_UNDEFINED ? if_undefined : found;
}

/* aprend_submit_immediate for work on one mip of one layer of a texture
 * (aprend_texture2d_t or aprend_texture3d_t): moves that subresource into
 * [working], runs [record], and moves it to aprend_immediate_final_layout.
 * The rest of the texture is not touched. The tracked layout is only written
 * once the submission has succeeded - a failed one ran nothing, so the layout
 * is still what it was. */
template <typename Texture, typename Fn>
static bool aprend_immediate_on_texture(
    Texture *texture,
    spudgpu_command_queue queue,
    bool wait,
    uint32_t mip,
    uint32_t layer,
    SPUDGPU_IMAGE_LAYOUT working,
    SPUDGPU_IMAGE_LAYOUT if_undefined,
    Fn &&record) {
	SPUDGPU_IMAGE_LAYOUT *tracked           = texture->tracking.at(mip, layer);
	const SPUDGPU_IMAGE_LAYOUT found        = *tracked;
	const SPUDGPU_IMAGE_LAYOUT final_layout = aprend_immediate_final_layout(found, if_undefined);

	spudgpu_image_view_desc_subresource_range range{};
	range.base_mip_level    = mip;
	range.mip_level_count   = 1;
	range.base_array_layer  = layer;
	range.array_layer_count = 1;

	bool ok = aprend_submit_immediate(texture->instance, queue, wait, [&](spudgpu_command_list cmd) {
		spudgpu_cmd_image_barrier_subresource(cmd, texture->image, &range, found, working);
		record(cmd);
		if (final_layout != working)
			spudgpu_cmd_image_barrier_subresource(cmd, texture->image, &range, working, final_layout);
	});
	if (ok)
		*tracked = final_layout;
	return ok;
}

typedef struct aprend_texture2d_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_texture2d_t() = default;
    ~aprend_texture2d_t();
    aprend_instance instance;
    aprend_texture2d_desc desc;
    spudgpu_image image{nullptr};
    // One layout for each (mip, layer); see aprend_texture_layouts.
    aprend_texture_layouts tracking{};
    // desc.mip_levels as the caller gave it (0 = full chain). desc holds the
    // count actually allocated, so a resize needs this to know which was asked.
    uint32_t requested_mip_levels{0};
} aprend_texture2d_t;

typedef struct aprend_texture3d_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_texture3d_t() = default;
    ~aprend_texture3d_t();
    aprend_instance instance;
    aprend_texture3d_desc desc;
    spudgpu_image image{nullptr};
    spudgpu_image_view image_view{nullptr};
    // One layout for each mip; a 3D texture has one layer.
    aprend_texture_layouts tracking{};
    // As aprend_texture2d_t::requested_mip_levels.
    uint32_t requested_mip_levels{0};
} aprend_texture3d_t;

typedef struct aprend_texture_view_t
{
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_texture_view_t() = default;
	~aprend_texture_view_t();
	aprend_instance instance{nullptr};
	APREND_TEXTURE_VIEW_TYPE view_type{APREND_TEXTURE_VIEW_TYPE_NONE};
	APREND_TEXTURE_DIMENSION dimension{APREND_TEXTURE_DIMENSION_1D};
	spudgpu_image_view image_view{nullptr};
	union {
		aprend_texture2d _t2d;
		aprend_texture3d _t3d;
	} texture;
	/* The desc the view was created with, its two counts resolved from
	 * APREND_TEXTURE_VIEW_REMAINING to real numbers. */
	aprend_texture_view_desc desc{};
} aprend_texture_view_t;

/* The mips and layers [view] covers, with the image and where that
 * texture's layouts are tracked. Views are of a 2D or a 3D texture, nothing
 * else. */
inline aprend_texture_range aprend_texture_view_range(aprend_texture_view_t *view) {
	aprend_texture_range range{};
	if (view->dimension == APREND_TEXTURE_DIMENSION_3D) {
		range.image    = view->texture._t3d->image;
		range.tracking = &view->texture._t3d->tracking;
	} else {
		range.image    = view->texture._t2d->image;
		range.tracking = &view->texture._t2d->tracking;
	}
	range.base_mip    = view->desc.base_mip;
	range.mip_count   = view->desc.mip_count;
	range.base_layer  = view->desc.base_layer;
	range.layer_count = view->desc.layer_count;
	return range;
}

typedef struct aprend_sampler_t
{
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_sampler_t() = default;
	~aprend_sampler_t();
	aprend_instance instance{nullptr};
	aprend_sampler_desc desc{};
	spudgpu_sampler sampler{nullptr};
} aprend_sampler_t;

typedef struct aprend_framebuffer_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_framebuffer_t() = default;
    ~aprend_framebuffer_t();
    aprend_instance instance;
    aprend_framebuffer_desc desc;
    // Owns the attachment specs desc.attachments points at — the caller's
    // array (see create_offscreen_color_target's stack-local color_spec) is
    // not guaranteed to outlive this framebuffer, but aprend_framebuffer_resize
    // re-reads desc.attachments on every resize, long after creation returns.
    std::vector<aprend_framebuffer_texture_spec> attachment_specs;
    std::vector<aprend_texture_view> color_attachments;
    // Parallel to color_attachments: the underlying texture if we created it
    // (needs destroying), or nullptr if it was borrowed via
    // aprend_framebuffer_texture_spec::existing_texture (caller still owns it).
    std::vector<aprend_texture2d> owned_color_textures;
    aprend_texture_view depth_attachment{nullptr};
    bool has_depth_attachment{false};
} aprend_framebuffer_t;
