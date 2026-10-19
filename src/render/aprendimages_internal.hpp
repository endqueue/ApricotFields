
#include "render/aprendimages.h"
#include "render/aprendframes.h"
#include "aprend_internal.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* Records `record` into the instance's upload batch for [queue]
 * (aprend_instance_open_upload_batch): it goes ahead of the lists of the
 * next submission, in that submission, and aprend_instance_next_frame or
 * aprend_instance_wait_idle submits it if nothing has by then. So it runs
 * after everything submitted before it and ahead of everything submitted
 * after, as a submission of its own would, and a frame's updates and clears
 * cost one list between them. What the commands read (a staging buffer) is
 * the caller's to retire after this returns, which holds it until the batch
 * has run. False, with nothing recorded, if no batch can be opened.
 *
 * Used for texture updates, readbacks and framebuffer clears. Shared by
 * aprendimages.cpp and aprendframes.cpp. */
template <typename Fn> static bool aprend_record_immediate(aprend_instance instance, spudgpu_command_queue queue, Fn &&record) {
	spudgpu_command_list cmd = nullptr;
	SPUDRESULT sr            = aprend_instance_open_upload_batch(instance, queue, &cmd);
	if (SPUDFAIL(sr)) {
		printf("apricot: no upload batch to record into: %s\n", spudresult_str(sr));
		return false;
	}
	record(cmd);
	return true;
}

/* Submits the upload batch now and blocks until it has run: a wait on the
 * instance's fence for that submission's serial, not on the whole queue. For
 * work whose result the caller reads back. If it fails, what was recorded
 * may still run with a later submission. */
static inline bool aprend_wait_immediate(aprend_instance instance) {
	uint64_t serial = 0;
	if (SPUDFAIL(aprend_instance_flush_upload_batch(instance, &serial)))
		return false;
	return !SPUDFAIL(aprend_instance_wait_serial(instance, serial));
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

/* The layout an upload leaves a texture with no layout yet in: one its usage
 * allows a shader to reach it through. SHADER_READ_ONLY needs a sampled
 * image, so a storage-only texture rests in GENERAL, the storage layout; a
 * texture with neither stays in TRANSFER_DST, where the copy left it. */
inline SPUDGPU_IMAGE_LAYOUT aprend_texture_upload_rest_layout(APREND_TEXTURE_USAGE_BITS usage) {
	if (usage & APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE)
		return SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY;
	if (usage & APREND_TEXTURE_USAGE_BIT_STORAGE)
		return SPUDGPU_IMAGE_LAYOUT_GENERAL;
	return SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST;
}

/* aprend_record_immediate for work on one mip of one layer of a texture
 * (aprend_texture2d_t or aprend_texture3d_t): moves that subresource into
 * [working], runs [record], and moves it to aprend_immediate_final_layout.
 * The rest of the texture is not touched. The tracked layout is written once
 * the barriers are recorded, since the batch that holds them goes ahead of
 * every later submission. With [wait], aprend_wait_immediate follows. */
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

	if (!aprend_record_immediate(texture->instance, queue, [&](spudgpu_command_list cmd) {
		    spudgpu_cmd_image_barrier_subresource(cmd, texture->image, &range, found, working);
		    record(cmd);
		    if (final_layout != working)
			    spudgpu_cmd_image_barrier_subresource(cmd, texture->image, &range, working, final_layout);
	    }))
		return false;
	*tracked = final_layout;
	return !wait || aprend_wait_immediate(texture->instance);
}

/* One level of one layer of a mip generation: a view of the level above to
 * sample (a 2D array view of its one layer, as aprend_downsample.frag
 * declares it), a view of the level to render (a 2D view), and the set that
 * binds the first with the chain's sampler. */
struct aprend_mip_step {
	aprend_texture_view source{nullptr};
	aprend_texture_view target{nullptr};
	aprend_binding_set set{nullptr};
};

/* What renders every level below [base_mip] of every layer of a 2D texture
 * through [filter]: steps[layer * levels + i], levels being the texture's
 * mip count - 1 - base_mip, renders level base_mip + 1 + i of that layer.
 * Kept by the texture (aprend_texture2d_t::mip_chains) from the first
 * generation that asks for it, by either form, so a list compiled every
 * frame makes nothing new; released with the texture, before its image,
 * and retired when a resize replaces the image. */
struct aprend_mip_chain {
	uint32_t base_mip{0};
	SPUDGPU_FILTER filter{SPUDGPU_FILTER_NEAREST};
	std::vector<aprend_mip_step> steps{};
};

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
    // The mip generations made so far; see aprend_mip_chain.
    std::vector<aprend_mip_chain> mip_chains{};
#if _DEBUG
    // Views made of it and not yet destroyed, its mip chains' included, so
    // a resize can refuse while one is left (aprend_texture2d_resize).
    uint32_t live_views{0};
#endif
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
#if _DEBUG
    // As aprend_texture2d_t::live_views.
    uint32_t live_views{0};
#endif
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
	/* The set of Aprend's bloom layout that reads this view
	 * (aprend_texture_view_bloom_set), made the first time a bloom reads
	 * it; NULL until then. Kept by the view, not by a bloom or a list, so
	 * it can't outlive the view or be confused with a new view made at the
	 * same address. Released with the view, before its image view. */
	aprend_binding_set bloom_set{nullptr};
	/* The same for Aprend's environment passes (aprendlit.cpp,
	 * aprend_texture_view_environment_set): this view and a mipmapped
	 * sampler, made the first time one reads it. */
	aprend_binding_set environment_set{nullptr};
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

/* Whether [texture] can have the levels below [base_mip] generated through
 * [filter], by either form: the level exists, the filter is a
 * SPUDGPU_FILTER, the texture has shader resource and render target usage,
 * and its format is one aprend_builtin_downsample_format takes. False,
 * printed with [who] in front, if not. Whether the level has contents is
 * each form's own check. */
bool aprend_generate_mips_valid(aprend_texture2d texture, uint32_t base_mip, SPUDGPU_FILTER filter, const char *who);

/* The chain of [texture] for [base_mip] and [filter], made if it has none,
 * and the downsample pipeline for its format. False, printed, if either
 * can't be made, with nothing kept. The pointer is valid until the next
 * call for this texture. Not for the last level, which has no chain. */
bool aprend_texture2d_mip_chain(
    aprend_texture2d texture,
    uint32_t base_mip,
    SPUDGPU_FILTER filter,
    const aprend_mip_chain **out_chain,
    aprend_graphics_pipeline *out_pipeline);

/* Records the pass that renders [step]'s target, a level [width] by
 * [height], from its source with [pipeline]. Both views are already in the
 * layouts the pass needs: the source in SHADER_READ_ONLY, the target in
 * COLOR_ATTACHMENT_OPTIMAL. */
void aprend_record_mip_pass(
    spudgpu_command_list cmd,
    aprend_graphics_pipeline pipeline,
    const aprend_mip_step &step,
    uint32_t width,
    uint32_t height);

