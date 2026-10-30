#ifndef APREND_BUILTIN_INTERNAL_HPP
#define APREND_BUILTIN_INTERNAL_HPP

/* Internal header - never included outside ApricotFields/src/render/.
 *
 * What Aprend draws with itself: shaders built into the library (GLSL in
 * shaders/, compiled to SPIR-V and embedded by the build), and the objects
 * made of them. Each is made through the same create calls a caller uses,
 * the first time something needs it, and kept by the instance until it is
 * destroyed. Made and used on the thread using the instance, under the
 * one-thread rule (aprendcontext.h, "Threads"), so nothing here is locked. */

#include "render/aprendimages.h"
#include "render/aprendpipeline.h"
#include "render/aprendshader.h"
#include "aprendlit_internal.hpp"

#include <vector>

/* An instance's built-in objects; each NULL, or empty, until first needed. */
struct aprend_builtins {
	/* aprend_fullscreen.vert: one triangle over the whole target, drawn with
	 * 3 vertices and no vertex buffer. */
	aprend_shader fullscreen_vert{nullptr};
	/* aprend_downsample.frag: a mip level from the one above, through a
	 * view of that level (binding 0) and a sampler (binding 1). */
	aprend_shader downsample_frag{nullptr};
	/* Set 0 of downsample_frag. */
	aprend_binding_layout downsample_layout{nullptr};
	/* Clamped to the edge, one for each filter: [SPUDGPU_FILTER_NEAREST]
	 * and [SPUDGPU_FILTER_LINEAR]. */
	aprend_sampler downsample_samplers[2]{};
	/* A pipeline renders to targets of the formats it was made for, so
	 * there is one for each format a mip chain has been generated in. */
	struct downsample_pipeline {
		SPUDGPU_FORMAT format;
		aprend_graphics_pipeline pipeline;
	};
	std::vector<downsample_pipeline> downsample_pipelines{};

	/* Bloom's shaders (aprend_bloom_*.frag), one per stage, and the layout
	 * of their set 0, which all four share: a texture at binding 0 and a
	 * sampler at binding 1. The prefilter's set 1 (the mask) and the
	 * composite's (the lens dirt) are of the same layout. */
	aprend_shader bloom_frags[4]{};
	aprend_binding_layout bloom_layout{nullptr};
	/* A pipeline for each stage and target format used. */
	struct bloom_pipeline {
		uint32_t stage;
		SPUDGPU_FORMAT format;
		aprend_graphics_pipeline pipeline;
	};
	std::vector<bloom_pipeline> bloom_pipelines{};

	/* aprend_present.frag: a source fitted into a whole target
	 * (APREND_COMMAND_DRAW_TEXTURE, PRESENT_TEXTURE's draw modes), and the
	 * layout of its set 0: a texture at binding 0, a sampler at binding 1. */
	aprend_shader present_frag{nullptr};
	aprend_binding_layout present_layout{nullptr};
	/* A pipeline for each target format drawn onto. */
	struct present_pipeline {
		SPUDGPU_FORMAT format;
		aprend_graphics_pipeline pipeline;
	};
	std::vector<present_pipeline> present_pipelines{};

	/* aprend_blur.frag: one direction of a separable Gaussian blur
	 * (APREND_COMMAND_BLUR), whose set 0 is the bloom layout's shape, and a
	 * pipeline for each target format it renders to. */
	aprend_shader blur_frag{nullptr};
	struct blur_pipeline {
		SPUDGPU_FORMAT format;
		aprend_graphics_pipeline pipeline;
	};
	std::vector<blur_pipeline> blur_pipelines{};

	/* aprend_outline.frag: the seed, jump and composite passes of an outline
	 * (APREND_COMMAND_OUTLINE), set 0 of the bloom layout's shape, and a
	 * pipeline for each target format and whether it blends (the
	 * composite) or not (the seed and jumps, into places). */
	aprend_shader outline_frag{nullptr};
	struct outline_pipeline {
		SPUDGPU_FORMAT format;
		bool blend;
		aprend_graphics_pipeline pipeline;
	};
	std::vector<outline_pipeline> outline_pipelines{};

	/* Lit surfaces' (aprendlit.h, aprendlit_internal.hpp). */
	aprend_lit_builtins lit{};
};

/* The stages of a bloom, each a pass with its own fragment shader. */
enum {
	/* The source into level 0 of the chain, with the threshold. */
	APREND_BLOOM_STAGE_PREFILTER = 0,
	/* A level of the chain from the one above. */
	APREND_BLOOM_STAGE_DOWNSAMPLE = 1,
	/* A level read up into the one above, added to it. */
	APREND_BLOOM_STAGE_UPSAMPLE = 2,
	/* Level 0 added onto the caller's target. */
	APREND_BLOOM_STAGE_COMPOSITE = 3
};

/* The push constant block every bloom shader declares (BloomParams), whole
 * or in part: the bytes a bloom pass pushes. */
struct aprend_bloom_params {
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
aprend_shader aprend_builtin_fullscreen_vert(aprend_instance instance);

/* The bloom pipeline of [stage] rendering to [format], made if it doesn't
 * exist, with the bloom shaders and layout. The upsample and composite
 * stages blend additively. NULL, reported, if it can't be made. */
aprend_graphics_pipeline aprend_builtin_bloom_pipeline(aprend_instance instance, uint32_t stage, SPUDGPU_FORMAT format);
/* The bloom shaders and layout, made if they aren't: what
 * aprend_builtin_bloom_pipeline() and aprend_texture_view_bloom_set() need.
 * False, reported, if one can't be. */
bool aprend_builtin_bloom_prepare(aprend_instance instance);
/* The bloom layout; made by aprend_builtin_bloom_prepare, NULL before it. */
aprend_binding_layout aprend_builtin_bloom_layout(aprend_instance instance);

/* The push constants of Aprend's blur shader (BlurParams), 24 bytes: one
 * texel of the target along the pass's direction in uv, the target's size,
 * sigma and the radius in whole texels. */
struct aprend_blur_params {
	float step[2];
	float target_size[2];
	float sigma;
	float radius;
};

/* The blur pipeline rendering to [format], made if it doesn't exist, with
 * its shader and the bloom layout it reads through. NULL, reported, if it
 * can't be made. */
aprend_graphics_pipeline aprend_builtin_blur_pipeline(aprend_instance instance, SPUDGPU_FORMAT format);

/* The passes of an outline, aprend_outline_params::mode. */
enum {
	APREND_OUTLINE_MODE_SEED      = 0,
	APREND_OUTLINE_MODE_JUMP      = 1,
	APREND_OUTLINE_MODE_COMPOSITE = 2
};

/* The push constants of Aprend's outline shader (OutlineParams), 48 bytes:
 * the color (linear RGBA, alpha not premultiplied), the target's size, the
 * width and the jump in texels, and the pass. */
struct aprend_outline_params {
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
aprend_graphics_pipeline aprend_builtin_outline_pipeline(aprend_instance instance, SPUDGPU_FORMAT format, bool blend);

/* The push constants of Aprend's present shader (PresentParams), 32 bytes:
 * the border color (linear RGBA), the target's size in texels, and how the
 * source fits: 0 its own size from the top left, 1 stretched over the
 * target, 2 fitted with its aspect kept, centred (APREND_TEXTURE_SCALING's
 * values, NONE drawn rather than copied). */
struct aprend_present_params {
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
aprend_graphics_pipeline aprend_builtin_present_pipeline(aprend_instance instance, SPUDGPU_FORMAT format);
/* The set of the present layout that reads [view] through the downsample
 * sampler of [filter] (clamped to the edge), kept by the view
 * (aprend_texture_view_t::present_sets) from the first call and released
 * with it. aprend_builtin_present_pipeline has made the layout. NULL,
 * reported, if the set can't be made. */
aprend_binding_set aprend_texture_view_present_set(aprend_texture_view view, SPUDGPU_FILTER filter);

/* The push constants of Aprend's downsample shader: [mode][0] is 1 for an
 * exact box filter (SPUDGPU_FILTER_LINEAR), 0 for a nearest one. 16 bytes,
 * as a uvec4. */
struct aprend_downsample_params {
	uint32_t mode[4];
};

/* The downsample pipeline rendering to [format], made if it doesn't exist,
 * with its shaders and layout. NULL, reported, if one can't be made. */
aprend_graphics_pipeline aprend_builtin_downsample_pipeline(aprend_instance instance, SPUDGPU_FORMAT format);
/* The downsample layout; made by aprend_builtin_downsample_pipeline, NULL
 * before it. */
aprend_binding_layout aprend_builtin_downsample_layout(aprend_instance instance);
/* The downsample sampler for [filter], made if it doesn't exist; bloom
 * reads through the linear one. NULL, reported, if [filter] is not a
 * SPUDGPU_FILTER or it can't be made. */
aprend_sampler aprend_builtin_downsample_sampler(aprend_instance instance, SPUDGPU_FILTER filter);

/* Whether a mip can be rendered from the one above in [format]: a color
 * format every backend renders to and filters, as a float, unorm or sRGB
 * value. */
bool aprend_builtin_downsample_format(SPUDGPU_FORMAT format);

/* Records a pass over the whole of [target], a view [width] by [height],
 * drawing the full-target triangle with [pipeline], [set] at set slot 0,
 * and [push_size] bytes of [push] as its push constants (none if [push] is
 * NULL). The target is in COLOR_ATTACHMENT_OPTIMAL and what [set] reads in
 * SHADER_READ_ONLY already; layouts are the caller's. [load_op] is
 * SPUDGPU_LOAD_OP_LOAD to draw over what the target holds (a blending
 * pipeline), SPUDGPU_LOAD_OP_DONT_CARE when every texel is replaced. */
void aprend_record_fullscreen_pass(
    spudgpu_command_list cmd,
    aprend_graphics_pipeline pipeline,
    spudgpu_image_view target,
    uint32_t width,
    uint32_t height,
    SPUDGPU_LOAD_OP load_op,
    aprend_binding_set set,
    const void *push,
    uint32_t push_size);

/* As aprend_record_fullscreen_pass, with [set_count] sets at slots 0 on (a
 * NULL one left unbound, for a pipeline that declares none there), each read
 * at its copy for [frame] where it is a set of buffer sets. */
void aprend_record_fullscreen_pass_sets(
    spudgpu_command_list cmd,
    aprend_graphics_pipeline pipeline,
    spudgpu_image_view target,
    uint32_t width,
    uint32_t height,
    SPUDGPU_LOAD_OP load_op,
    const aprend_binding_set *sets,
    uint32_t set_count,
    uint32_t frame,
    const void *push,
    uint32_t push_size);

/* As aprend_record_fullscreen_pass_sets, drawn only inside [region] (x, y,
 * width, height in texels, inside the target), by the pass's scissor: with
 * SPUDGPU_LOAD_OP_LOAD, the target outside it keeps what it held. */
void aprend_record_fullscreen_pass_region(
    spudgpu_command_list cmd,
    aprend_graphics_pipeline pipeline,
    spudgpu_image_view target,
    uint32_t width,
    uint32_t height,
    const uint32_t region[4],
    SPUDGPU_LOAD_OP load_op,
    const aprend_binding_set *sets,
    uint32_t set_count,
    uint32_t frame,
    const void *push,
    uint32_t push_size);

/* From the instance's destructor, before its release queue is emptied:
 * destroys every built-in object, pipelines before the layout they
 * declare. Each destroy call goes through the release queue, which the
 * destructor then empties in order. */
void aprend_builtins_destroy(aprend_instance instance);

#endif // APREND_BUILTIN_INTERNAL_HPP
