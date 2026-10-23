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
	 * sampler at binding 1. */
	aprend_shader bloom_frags[4]{};
	aprend_binding_layout bloom_layout{nullptr};
	/* A pipeline for each stage and target format used. */
	struct bloom_pipeline {
		uint32_t stage;
		SPUDGPU_FORMAT format;
		aprend_graphics_pipeline pipeline;
	};
	std::vector<bloom_pipeline> bloom_pipelines{};

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
};

/* The full-target vertex shader, made if it doesn't exist. NULL, printed,
 * if it can't be. */
aprend_shader aprend_builtin_fullscreen_vert(aprend_instance instance);

/* The bloom pipeline of [stage] rendering to [format], made if it doesn't
 * exist, with the bloom shaders and layout. The upsample and composite
 * stages blend additively. NULL, printed, if it can't be made. */
aprend_graphics_pipeline aprend_builtin_bloom_pipeline(aprend_instance instance, uint32_t stage, SPUDGPU_FORMAT format);
/* The bloom shaders and layout, made if they aren't: what
 * aprend_builtin_bloom_pipeline() and aprend_texture_view_bloom_set() need.
 * False, printed, if one can't be. */
bool aprend_builtin_bloom_prepare(aprend_instance instance);
/* The bloom layout; made by aprend_builtin_bloom_prepare, NULL before it. */
aprend_binding_layout aprend_builtin_bloom_layout(aprend_instance instance);

/* The push constants of Aprend's downsample shader: [mode][0] is 1 for an
 * exact box filter (SPUDGPU_FILTER_LINEAR), 0 for a nearest one. 16 bytes,
 * as a uvec4. */
struct aprend_downsample_params {
	uint32_t mode[4];
};

/* The downsample pipeline rendering to [format], made if it doesn't exist,
 * with its shaders and layout. NULL, printed, if one can't be made. */
aprend_graphics_pipeline aprend_builtin_downsample_pipeline(aprend_instance instance, SPUDGPU_FORMAT format);
/* The downsample layout; made by aprend_builtin_downsample_pipeline, NULL
 * before it. */
aprend_binding_layout aprend_builtin_downsample_layout(aprend_instance instance);
/* The downsample sampler for [filter], made if it doesn't exist; bloom
 * reads through the linear one. NULL, printed, if [filter] is not a
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

/* From the instance's destructor, before its release queue is emptied:
 * destroys every built-in object, pipelines before the layout they
 * declare. Each destroy call goes through the release queue, which the
 * destructor then empties in order. */
void aprend_builtins_destroy(aprend_instance instance);

#endif // APREND_BUILTIN_INTERNAL_HPP
