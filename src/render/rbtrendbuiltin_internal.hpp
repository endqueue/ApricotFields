#ifndef RBTREND_BUILTIN_INTERNAL_HPP
#define RBTREND_BUILTIN_INTERNAL_HPP

/* Internal header - never included outside rabbittus/src/render/.
 *
 * What Rend draws with itself: shaders built into the library (Slang in
 * shaders/, compiled to each API's own format and embedded by the build as
 * packages), and the objects
 * made of them. Each is made through the same create calls a caller uses,
 * the first time something needs it, and kept by the instance until it is
 * destroyed. Made and used on the thread using the instance, under the
 * one-thread rule (rbtrendcontext.h, "Threads"), so nothing here is locked. */

#include "render/rbtrendimages.h"
#include "render/rbtrendpipeline.h"
#include "render/rbtrendshader.h"
#include "rbtrendlit_internal.hpp"

#include <vector>

/* An instance's built-in objects; each NULL, or empty, until first needed. */
struct rbtrend_builtins {
	/* rbtrend_fullscreen_vert.slang: one triangle over the whole target, drawn with
	 * 3 vertices and no vertex buffer. */
	rbtrend_shader fullscreen_vert{nullptr};
	/* rbtrend_downsample_frag.slang: a mip level from the one above, through a
	 * view of that level (binding 0) and a sampler (binding 1). */
	rbtrend_shader downsample_frag{nullptr};
	/* Set 0 of downsample_frag. */
	rbtrend_binding_layout downsample_layout{nullptr};
	/* Clamped to the edge, one for each filter: [CRGPU_FILTER_NEAREST]
	 * and [CRGPU_FILTER_LINEAR]. */
	rbtrend_sampler downsample_samplers[2]{};
	/* A pipeline renders to targets of the formats it was made for, so
	 * there is one for each format a mip chain has been generated in. */
	struct downsample_pipeline {
		CRGPU_FORMAT format;
		rbtrend_graphics_pipeline pipeline;
	};
	std::vector<downsample_pipeline> downsample_pipelines{};

	/* Bloom's shaders (rbtrend_bloom_*.frag), one per stage, and the layout
	 * of their set 0, which all four share: a texture at binding 0 and a
	 * sampler at binding 1. The prefilter's set 1 (the mask) and the
	 * composite's (the lens dirt) are of the same layout. */
	rbtrend_shader bloom_frags[4]{};
	rbtrend_binding_layout bloom_layout{nullptr};
	/* A pipeline for each stage and target format used. */
	struct bloom_pipeline {
		uint32_t stage;
		CRGPU_FORMAT format;
		rbtrend_graphics_pipeline pipeline;
	};
	std::vector<bloom_pipeline> bloom_pipelines{};

	/* rbtrend_present_frag.slang: a source fitted into a whole target
	 * (RBTREND_COMMAND_DRAW_TEXTURE, PRESENT_TEXTURE's draw modes), and the
	 * layout of its set 0: a texture at binding 0, a sampler at binding 1. */
	rbtrend_shader present_frag{nullptr};
	rbtrend_binding_layout present_layout{nullptr};
	/* A pipeline for each target format drawn onto. */
	struct present_pipeline {
		CRGPU_FORMAT format;
		rbtrend_graphics_pipeline pipeline;
	};
	std::vector<present_pipeline> present_pipelines{};

	/* rbtrend_blur_frag.slang: one direction of a separable Gaussian blur
	 * (RBTREND_COMMAND_BLUR), whose set 0 is the bloom layout's shape, and a
	 * pipeline for each target format it renders to. */
	rbtrend_shader blur_frag{nullptr};
	struct blur_pipeline {
		CRGPU_FORMAT format;
		rbtrend_graphics_pipeline pipeline;
	};
	std::vector<blur_pipeline> blur_pipelines{};

	/* rbtrend_outline_frag.slang: the seed, jump and composite passes of an outline
	 * (RBTREND_COMMAND_OUTLINE), set 0 of the bloom layout's shape, and a
	 * pipeline for each target format and whether it blends (the
	 * composite) or not (the seed and jumps, into places). */
	rbtrend_shader outline_frag{nullptr};
	struct outline_pipeline {
		CRGPU_FORMAT format;
		bool blend;
		rbtrend_graphics_pipeline pipeline;
	};
	std::vector<outline_pipeline> outline_pipelines{};

	/* Lit surfaces' (rbtrendlit.h, rbtrendlit_internal.hpp). */
	rbtrend_lit_builtins lit{};
};

/* The stages of a bloom, each a pass with its own fragment shader. */
enum {
	/* The source into level 0 of the chain, with the threshold. */
	RBTREND_BLOOM_STAGE_PREFILTER = 0,
	/* A level of the chain from the one above. */
	RBTREND_BLOOM_STAGE_DOWNSAMPLE = 1,
	/* A level read up into the one above, added to it. */
	RBTREND_BLOOM_STAGE_UPSAMPLE = 2,
	/* Level 0 added onto the caller's target. */
	RBTREND_BLOOM_STAGE_COMPOSITE = 3
};

/* The push constant block every bloom shader declares (BloomParams), whole
 * or in part: the bytes a bloom pass pushes. */
struct rbtrend_bloom_params {
	float threshold;
	float knee;
	float radius;
	float intensity;
	float target_size[2];
	/* 1 for the prefilter to read the mask at its set 1, 0 for none. */
	float use_mask;
	/* What the composite's lens dirt at its set 1 adds; 0 for none. */
	float lens_dirt_intensity;
};

/* The full-target vertex shader, made if it doesn't exist. NULL, reported,
 * if it can't be. */
rbtrend_shader rbtrend_builtin_fullscreen_vert(rbtrend_instance instance);

/* The bloom pipeline of [stage] rendering to [format], made if it doesn't
 * exist, with the bloom shaders and layout. The upsample and composite
 * stages blend additively. NULL, reported, if it can't be made. */
rbtrend_graphics_pipeline rbtrend_builtin_bloom_pipeline(rbtrend_instance instance, uint32_t stage, CRGPU_FORMAT format);
/* The bloom shaders and layout, made if they aren't: what
 * rbtrend_builtin_bloom_pipeline() and rbtrend_texture_view_bloom_set() need.
 * False, reported, if one can't be. */
bool rbtrend_builtin_bloom_prepare(rbtrend_instance instance);
/* The bloom layout; made by rbtrend_builtin_bloom_prepare, NULL before it. */
rbtrend_binding_layout rbtrend_builtin_bloom_layout(rbtrend_instance instance);

/* The push constants of Rend's blur shader (BlurParams), 24 bytes: one
 * texel of the target along the pass's direction in uv, the target's size,
 * sigma and the radius in whole texels. */
struct rbtrend_blur_params {
	float step[2];
	float target_size[2];
	float sigma;
	float radius;
};

/* The blur pipeline rendering to [format], made if it doesn't exist, with
 * its shader and the bloom layout it reads through. NULL, reported, if it
 * can't be made. */
rbtrend_graphics_pipeline rbtrend_builtin_blur_pipeline(rbtrend_instance instance, CRGPU_FORMAT format);

/* The passes of an outline, rbtrend_outline_params::mode. */
enum {
	RBTREND_OUTLINE_MODE_SEED      = 0,
	RBTREND_OUTLINE_MODE_JUMP      = 1,
	RBTREND_OUTLINE_MODE_COMPOSITE = 2
};

/* The push constants of Rend's outline shader (OutlineParams), 48 bytes:
 * the color (linear RGBA, alpha not premultiplied), the target's size, the
 * width and the jump in texels, and the pass. */
struct rbtrend_outline_params {
	float color[4];
	float target_size[2];
	float width;
	float jump;
	uint32_t mode;
	uint32_t unused[3];
};

/* The outline pipeline rendering to [format], blending premultiplied over
 * the target if [blend] (the composite), made if it doesn't exist, with its
 * shader and the bloom layout it reads through. NULL, reported, if it can't
 * be made. */
rbtrend_graphics_pipeline rbtrend_builtin_outline_pipeline(rbtrend_instance instance, CRGPU_FORMAT format, bool blend);

/* The push constants of Rend's present shader (PresentParams), 32 bytes:
 * the border color (linear RGBA), the target's size in texels, and how the
 * source fits: 0 its own size from the top left, 1 stretched over the
 * target, 2 fitted with its aspect kept, centred (RBTREND_TEXTURE_SCALING's
 * values, NONE drawn rather than copied). */
struct rbtrend_present_params {
	float border[4];
	/* The target rectangle: its top-left corner and size, in target texels. */
	float target_offset[2];
	float target_size[2];
	/* The source rectangle, in source texels. */
	float source_offset[2];
	float source_size[2];
	uint32_t mode;
	uint32_t unused[3];
};

/* The present pipeline rendering to [format], made if it doesn't exist, with
 * its shaders and layout. NULL, reported, if it can't be made. */
rbtrend_graphics_pipeline rbtrend_builtin_present_pipeline(rbtrend_instance instance, CRGPU_FORMAT format);
/* The set of the present layout that reads [view] through the downsample
 * sampler of [filter] (clamped to the edge), kept by the view
 * (rbtrend_texture_view_t::present_sets) from the first call and released
 * with it. rbtrend_builtin_present_pipeline has made the layout. NULL,
 * reported, if the set can't be made. */
rbtrend_binding_set rbtrend_texture_view_present_set(rbtrend_texture_view view, CRGPU_FILTER filter);

/* The push constants of Rend's downsample shader: [mode][0] is 1 for an
 * exact box filter (CRGPU_FILTER_LINEAR), 0 for a nearest one. 16 bytes,
 * as a uvec4. */
struct rbtrend_downsample_params {
	uint32_t mode[4];
};

/* The downsample pipeline rendering to [format], made if it doesn't exist,
 * with its shaders and layout. NULL, reported, if one can't be made. */
rbtrend_graphics_pipeline rbtrend_builtin_downsample_pipeline(rbtrend_instance instance, CRGPU_FORMAT format);
/* The downsample layout; made by rbtrend_builtin_downsample_pipeline, NULL
 * before it. */
rbtrend_binding_layout rbtrend_builtin_downsample_layout(rbtrend_instance instance);
/* The downsample sampler for [filter], made if it doesn't exist; bloom
 * reads through the linear one. NULL, reported, if [filter] is not a
 * CRGPU_FILTER or it can't be made. */
rbtrend_sampler rbtrend_builtin_downsample_sampler(rbtrend_instance instance, CRGPU_FILTER filter);

/* Whether a mip can be rendered from the one above in [format]: a color
 * format every backend renders to and filters, as a float, unorm or sRGB
 * value. */
bool rbtrend_builtin_downsample_format(CRGPU_FORMAT format);

/* Records a pass over the whole of [target], a view [width] by [height],
 * drawing the full-target triangle with [pipeline], [set] at set slot 0,
 * and [push_size] bytes of [push] as its push constants (none if [push] is
 * NULL). The target is in COLOR_ATTACHMENT_OPTIMAL and what [set] reads in
 * SHADER_READ_ONLY already; layouts are the caller's. [load_op] is
 * CRGPU_LOAD_OP_LOAD to draw over what the target holds (a blending
 * pipeline), CRGPU_LOAD_OP_DONT_CARE when every texel is replaced. */
void rbtrend_record_fullscreen_pass(
    crgpu_command_list cmd,
    rbtrend_graphics_pipeline pipeline,
    crgpu_image_view target,
    uint32_t width,
    uint32_t height,
    CRGPU_LOAD_OP load_op,
    rbtrend_binding_set set,
    const void *push,
    uint32_t push_size);

/* As rbtrend_record_fullscreen_pass, with [set_count] sets at slots 0 on (a
 * NULL one left unbound, for a pipeline that declares none there), each read
 * at its copy for [frame] where it is a set of buffer sets. */
void rbtrend_record_fullscreen_pass_sets(
    crgpu_command_list cmd,
    rbtrend_graphics_pipeline pipeline,
    crgpu_image_view target,
    uint32_t width,
    uint32_t height,
    CRGPU_LOAD_OP load_op,
    const rbtrend_binding_set *sets,
    uint32_t set_count,
    uint32_t frame,
    const void *push,
    uint32_t push_size);

/* As rbtrend_record_fullscreen_pass_sets, drawn only inside [region] (x, y,
 * width, height in texels, inside the target), by the pass's scissor: with
 * CRGPU_LOAD_OP_LOAD, the target outside it keeps what it held. */
void rbtrend_record_fullscreen_pass_region(
    crgpu_command_list cmd,
    rbtrend_graphics_pipeline pipeline,
    crgpu_image_view target,
    uint32_t width,
    uint32_t height,
    const uint32_t region[4],
    CRGPU_LOAD_OP load_op,
    const rbtrend_binding_set *sets,
    uint32_t set_count,
    uint32_t frame,
    const void *push,
    uint32_t push_size);

/* From the instance's destructor, before its release queue is emptied:
 * destroys every built-in object, pipelines before the layout they
 * declare. Each destroy call goes through the release queue, which the
 * destructor then empties in order. */
void rbtrend_builtins_destroy(rbtrend_instance instance);

#endif // RBTREND_BUILTIN_INTERNAL_HPP
