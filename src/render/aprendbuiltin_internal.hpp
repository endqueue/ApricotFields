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
};

/* The downsample pipeline rendering to [format], made if it doesn't exist,
 * with its shaders and layout. NULL, printed, if one can't be made. */
aprend_graphics_pipeline aprend_builtin_downsample_pipeline(aprend_instance instance, SPUDGPU_FORMAT format);
/* The downsample layout; made by aprend_builtin_downsample_pipeline, NULL
 * before it. */
aprend_binding_layout aprend_builtin_downsample_layout(aprend_instance instance);
/* The downsample sampler for [filter], made if it doesn't exist. NULL,
 * printed, if [filter] is not a SPUDGPU_FILTER or it can't be made. */
aprend_sampler aprend_builtin_downsample_sampler(aprend_instance instance, SPUDGPU_FILTER filter);

/* Whether a mip can be rendered from the one above in [format]: a color
 * format every backend renders to and filters, as a float, unorm or sRGB
 * value. */
bool aprend_builtin_downsample_format(SPUDGPU_FORMAT format);

/* From the instance's destructor, before its release queue is emptied:
 * destroys every built-in object, pipelines before the layout they
 * declare. Each destroy call goes through the release queue, which the
 * destructor then empties in order. */
void aprend_builtins_destroy(aprend_instance instance);

#endif // APREND_BUILTIN_INTERNAL_HPP
