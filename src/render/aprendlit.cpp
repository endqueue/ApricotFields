#include "render/aprendlit.h"
#include "render/aprendpost.h"
#include "aprend_internal.hpp"
#include "aprendraytracing_internal.hpp"
#include "aprendbuiltin_internal.hpp"
#include "aprendimages_internal.hpp"
#include "aprendlit_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The packages of shaders/, embedded by the build (cmake/aprend_slang.cmake).
#include "aprend_lit_frag.package.inc"
#include "aprend_lit_depth_frag.package.inc"
#include "aprend_lit_vert.package.inc"
#include "aprend_lit_color_vert.package.inc"
#include "aprend_lit_shadow_vert.package.inc"
#include "aprend_lit_shadow_color_vert.package.inc"
#include "aprend_lit_depth_cutout_frag.package.inc"
#include "aprend_lit_sky_frag.package.inc"
#include "aprend_lit_cull_comp.package.inc"
#include "aprend_lit_cull_bounded_comp.package.inc"
#include "aprend_environment_from_cube_frag.package.inc"
#include "aprend_environment_from_equirect_frag.package.inc"
#include "aprend_environment_prefilter_frag.package.inc"
#include "aprend_environment_irradiance_frag.package.inc"
#include "aprend_environment_sky_frag.package.inc"
#include "aprend_environment_hosek_frag.package.inc"
#include "aprend_lit_brdf_lut_frag.package.inc"
#include "aprend_lit_occlusion_frag.package.inc"
#include "aprend_lit_occlusion_blur_frag.package.inc"
#include "aprend_exposure_luminance_frag.package.inc"
#include "aprend_exposure_adapt_frag.package.inc"
#include "aprend_exposure_histogram_comp.package.inc"
#include "aprend_exposure_adapt_histogram_frag.package.inc"
#include "aprend_tone_map_frag.package.inc"
#include "aprend_lit_surface_reflect_frag.package.inc"
#include "aprend_lit_hiz_frag.package.inc"
#include "aprend_lit_hiz_down_frag.package.inc"
#include "aprend_lit_ssr_comp.package.inc"
#include "aprend_lit_reflect_frag.package.inc"

static_assert(sizeof(APREND_LIT_VERTEX) == 48, "APREND_LIT_VERTEX is 48 bytes");
static_assert(sizeof(APREND_LIT_INSTANCE) == 64, "APREND_LIT_INSTANCE is 64 bytes");
static_assert(sizeof(aprend_lit_scene_block) == 128 + 80 * APREND_LIT_MAX_SHADOW_VIEWS + 80 * APREND_LIT_MAX_DIRECTIONAL_LIGHTS +
                                                 32 * APREND_LIT_MAX_SHADOW_VIEWS + 208 + 64 * APREND_LIT_MAX_LIGHTS,
              "AprendLitScene's std140 size");
// Vulkan's guaranteed maxUniformBufferRange, the smallest any device may
// report; SpudGPU reports no limit of its own for a uniform buffer, so the
// block keeps under this one on every device.
static_assert(sizeof(aprend_lit_scene_block) <= 16384, "AprendLitScene fits Vulkan's guaranteed uniform buffer range");
static_assert(sizeof(aprend_lit_material_block) == 288, "AprendLitMaterial's std140 size");
static_assert(sizeof(aprend_lit_shadow_pass_block) == 64, "AprendLitShadowPass's std140 size");
static_assert(sizeof(aprend_occlusion_block) == 176, "AprendOcclusionPass's std140 size");
static_assert(sizeof(aprend_hosek_sky_block) == 160, "AprendHosekSky's std140 size");
static_assert(sizeof(aprend_environment_params) % 8 == 0 && sizeof(aprend_sky_params) % 8 == 0 && sizeof(aprend_tone_map_params) % 8 == 0 &&
                  sizeof(aprend_adapt_params) % 8 == 0 && sizeof(aprend_size_params) % 8 == 0 &&
                  sizeof(aprend_histogram_params) % 8 == 0 && sizeof(aprend_histogram_adapt_params) % 8 == 0,
              "a push Metal aligns to 8 bytes is a multiple of 8");
static_assert(sizeof(aprend_hiz_params) == 16 && sizeof(aprend_reflect_params) == 16, "HizParams and ReflectParams are 16 bytes");
static_assert(sizeof(aprend_ssr_params) == 48, "SsrParams in aprend_lit_ssr_comp.slang");

// ---- Checks -------------------------------------------------------------------------

static bool finite(float v) { return std::isfinite(v); }
static bool all_finite(const float *v, size_t n) {
	for (size_t i = 0; i < n; ++i)
		if (!finite(v[i]))
			return false;
	return true;
}
static float length3(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static void unit3(const float v[3], float out[4]) {
	const float l = length3(v);
	for (int i = 0; i < 3; ++i)
		out[i] = l > 0.0f ? v[i] / l : 0.0f;
	out[3] = 0.0f;
}
static bool power_of_2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }
static uint32_t log2_of(uint32_t v) {
	uint32_t l = 0;
	while (v > 1) {
		v >>= 1;
		++l;
	}
	return l;
}
static glm::vec3 vec3_of(const float v[3]) { return glm::vec3(v[0], v[1], v[2]); }

// [projection] with clip space's y negated. glm's projections put +Y up the
// view, as SpudGPU's clip space has it on every backend (card S19); each
// shadow map is rendered with y negated all the same, so it holds the image
// (and the winding) it did when clip space was +Y down the target, and the
// surface shader reads it by the same matrix either way.
static glm::mat4 aprend_flip_clip_y(const glm::mat4 &projection) {
	return glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f)) * projection;
}

// One member a block must have, where the C++ mirror has it.
struct expected_member {
	const char *name;
	uint32_t offset;
};

// Whether the block at [binding] of [layout] is [size] bytes, give or take
// std140's rounding, and has each of [members] where the mirror puts it.
// Printed if not: the shader and the mirror have drifted apart.
static bool aprend_lit_check_block(aprend_binding_layout layout, uint32_t binding, const char *what, uint32_t size,
                                   const expected_member *members, size_t count) {
	uint32_t block_size               = 0;
	const APREND_SHADER_MEMBER *found = nullptr;
	uint32_t found_count              = 0;
	if (!(aprend_binding_layout_get_block(layout, binding, &block_size, &found, &found_count) == APRESULT_OK)) {
		aprend_report(layout->instance, APRESULT_INVALID_ARGUMENT, "%s has no block at binding %u", what, binding);
		return false;
	}
	bool ok = block_size <= size && size - block_size < 16;
	if (!ok)
		aprend_report(layout->instance, APRESULT_INVALID_ARGUMENT, "%s is %u bytes in the shader, and its mirror %u", what, block_size, size);
	for (size_t i = 0; i < count; ++i) {
		bool here = false;
		for (uint32_t m = 0; m < found_count && !here; ++m)
			here = strcmp(found[m].name, members[i].name) == 0 && found[m].offset == members[i].offset;
		if (!here) {
			aprend_report(layout->instance, APRESULT_INVALID_ARGUMENT, "%s's member \"%s\" is not at %u in the shader", what, members[i].name, members[i].offset);
			ok = false;
		}
	}
	return ok;
}

#define MEMBER(T, field) (uint32_t) offsetof(T, field)
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

static bool aprend_lit_check_scene_block(aprend_binding_layout layout) {
	typedef aprend_lit_scene_block B;
	typedef aprend_lit_light_block L;
	typedef aprend_lit_directional_block D;
	typedef aprend_lit_shadow_block S;
	typedef aprend_lit_shadow_view_block V;
	const uint32_t last_light       = MEMBER(B, lights) + (uint32_t)sizeof(L) * (APREND_LIT_MAX_LIGHTS - 1);
	const uint32_t last_directional = MEMBER(B, directional) + (uint32_t)sizeof(D) * (APREND_LIT_MAX_DIRECTIONAL_LIGHTS - 1);
	const uint32_t last_shadow      = MEMBER(B, shadows) + (uint32_t)sizeof(S) * (APREND_LIT_MAX_SHADOW_VIEWS - 1);
	const uint32_t last_view        = MEMBER(B, shadow_views) + (uint32_t)sizeof(V) * (APREND_LIT_MAX_SHADOW_VIEWS - 1);
	const expected_member members[] = {
	    {"view_projection", MEMBER(B, view_projection)},
	    {"inverse_view_projection", MEMBER(B, inverse_view_projection)},
	    {"shadow_views[0].view_projection", MEMBER(B, shadow_views)},
	    {"shadow_views[0].rect", MEMBER(B, shadow_views) + MEMBER(V, rect)},
	    {"shadow_views[63].rect", last_view + MEMBER(V, rect)},
	    {"directional[0].direction_versine", MEMBER(B, directional)},
	    {"directional[0].shadow_views", MEMBER(B, directional) + MEMBER(D, shadow_views)},
	    {"directional[3].shadow_views", last_directional + MEMBER(D, shadow_views)},
	    {"shadows[0].params", MEMBER(B, shadows)},
	    {"shadows[63].views", last_shadow + MEMBER(S, views)},
	    {"eye_position", MEMBER(B, eye_position)},
	    {"view_direction", MEMBER(B, view_direction)},
	    {"ambient_up", MEMBER(B, ambient_up)},
	    {"ambient_sky", MEMBER(B, ambient_sky)},
	    {"ambient_ground", MEMBER(B, ambient_ground)},
	    {"environment_rotation", MEMBER(B, environment_rotation)},
	    {"target_size", MEMBER(B, target_size)},
	    {"shadow_atlas_size", MEMBER(B, shadow_atlas_size)},
	    {"environment_intensity", MEMBER(B, environment_intensity)},
	    {"environment_mips", MEMBER(B, environment_mips)},
	    {"exposure", MEMBER(B, exposure)},
	    {"output_encoding", MEMBER(B, output_encoding)},
	    {"flags", MEMBER(B, flags)},
	    {"light_count", MEMBER(B, light_count)},
	    {"directional_count", MEMBER(B, directional_count)},
	    {"tile_size", MEMBER(B, tile_size)},
	    {"tiles_x", MEMBER(B, tiles_x)},
	    {"tiles_y", MEMBER(B, tiles_y)},
	    {"occlusion_strength", MEMBER(B, occlusion_strength)},
	    {"scene_color_mips", MEMBER(B, scene_color_mips)},
	    {"lights[0].position_range", MEMBER(B, lights)},
	    {"lights[0].outer_shadow", MEMBER(B, lights) + MEMBER(L, outer_shadow)},
	    {"lights[127].outer_shadow", last_light + MEMBER(L, outer_shadow)},
	};
	return aprend_lit_check_block(layout, 0, "AprendLitScene", (uint32_t)sizeof(B), members, COUNT_OF(members));
}

static bool aprend_lit_check_material_block(aprend_binding_layout layout) {
	typedef aprend_lit_material_block B;
	const expected_member members[] = {
	    {"base_color", MEMBER(B, base_color)},
	    {"emission", MEMBER(B, emission)},
	    {"sheen", MEMBER(B, sheen)},
	    {"roughness", MEMBER(B, roughness)},
	    {"metalness", MEMBER(B, metalness)},
	    {"specular", MEMBER(B, specular)},
	    {"normal_scale", MEMBER(B, normal_scale)},
	    {"occlusion_strength", MEMBER(B, occlusion_strength)},
	    {"alpha_cutoff", MEMBER(B, alpha_cutoff)},
	    {"clearcoat", MEMBER(B, clearcoat)},
	    {"clearcoat_roughness", MEMBER(B, clearcoat_roughness)},
	    {"maps", MEMBER(B, maps)},
	    {"uv_transform", MEMBER(B, uv_transform)},
	    {"map_uv_transform", MEMBER(B, map_uv_transform)},
	    {"transmission", MEMBER(B, transmission)},
	    {"attenuation", MEMBER(B, attenuation)},
	};
	return aprend_lit_check_block(layout, 0, "AprendLitMaterial", (uint32_t)sizeof(B), members, COUNT_OF(members));
}

static bool aprend_lit_check_shadow_pass_block(aprend_binding_layout layout) {
	const expected_member members[] = {{"view_projection", 0}};
	return aprend_lit_check_block(layout, 0, "AprendLitShadowPass", (uint32_t)sizeof(aprend_lit_shadow_pass_block), members, 1);
}

static bool aprend_lit_check_hosek_block(aprend_binding_layout layout) {
	typedef aprend_hosek_sky_block B;
	const expected_member members[] = {
	    {"config", MEMBER(B, config)},
	    {"radiance", MEMBER(B, radiance)},
	};
	return aprend_lit_check_block(layout, 0, "AprendHosekSky", (uint32_t)sizeof(B), members, COUNT_OF(members));
}

static bool aprend_lit_check_occlusion_block(aprend_binding_layout layout) {
	typedef aprend_occlusion_block B;
	const expected_member members[] = {
	    {"view_projection", MEMBER(B, view_projection)},
	    {"inverse_view_projection", MEMBER(B, inverse_view_projection)},
	    {"target_size", MEMBER(B, target_size)},
	    {"eye_position", MEMBER(B, eye_position)},
	    {"params", MEMBER(B, params)},
	};
	return aprend_lit_check_block(layout, 0, "AprendOcclusionPass", (uint32_t)sizeof(B), members, COUNT_OF(members));
}

// ---- Shaders, layouts and the objects every lit thing shares -------------------------

static const struct {
	const APREND_SHADER_PACKAGE *package;
	const char *name;
} s_lit_shaders[9] = {
    {&aprend_lit_frag_package, "surface fragment"},
    {&aprend_lit_depth_frag_package, "depth fragment"},
    {&aprend_lit_vert_package, "surface vertex"},
    {&aprend_lit_shadow_vert_package, "shadow vertex"},
    {&aprend_lit_depth_cutout_frag_package, "cut-out depth fragment"},
    {&aprend_lit_sky_frag_package, "sky fragment"},
    {&aprend_lit_color_vert_package, "colored surface vertex"},
    {&aprend_lit_shadow_color_vert_package, "colored shadow vertex"},
    {&aprend_lit_surface_reflect_frag_package, "reflecting surface fragment"},
};

static aprend_shader aprend_lit_shader(aprend_instance instance, uint32_t which) {
	aprend_shader &shader = instance->builtins.lit.shaders[which];
	if (!shader) {
		(void)aprend_shader_create(instance, s_lit_shaders[which].package, &shader, nullptr);
		if (!shader)
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's lit %s shader can't be created", s_lit_shaders[which].name);
	}
	return shader;
}

static aprend_sampler aprend_lit_sampler(aprend_instance instance, SPUDGPU_ADDRESS_MODE address, bool compare, SPUDGPU_COMPARE_OP op,
                                         const char *name) {
	APREND_SAMPLER_DESC desc{};
	desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_SAMPLER_DESC);
	desc.mag_filter     = SPUDGPU_FILTER_LINEAR;
	desc.min_filter     = SPUDGPU_FILTER_LINEAR;
	desc.mipmap_filter  = compare ? SPUDGPU_FILTER_NEAREST : SPUDGPU_FILTER_LINEAR;
	desc.address_mode_u = address;
	desc.address_mode_v = address;
	desc.address_mode_w = address;
	desc.mip_lod_bias   = 0.0f;
	desc.min_lod        = 0.0f;
	desc.max_lod        = compare ? 0.0f : 1000.0f;
	desc.max_anisotropy = 1.0f;
	desc.compare_enable = compare;
	desc.compare_op     = op;
	desc.debug_name = name;
	aprend_sampler sampler = nullptr;
	(void)aprend_sampler_create(instance, &desc, &sampler, nullptr);
	if (!sampler)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's %s can't be created", name);
	return sampler;
}

// A storage buffer of [size] bytes only shaders write, one copy or one per
// frame in flight ([per_frame]), named [name]. [error] says why it failed.
static APRESULT aprend_lit_storage_buffer(aprend_instance instance, uint64_t size, bool per_frame, const char *name, aprend_buffer *out_buffer, APREND_ERROR *error) {
	APREND_BUFFER_DESC desc{};
	desc.base       = apri_struct_base(APRI_STRUCT_TYPE_APREND_BUFFER_DESC);
	desc.size       = size;
	desc.usage      = APREND_BUFFER_USAGE_STORAGE;
	desc.writes     = APREND_BUFFER_WRITES_ONCE;
	desc.per_frame  = per_frame;
	desc.debug_name = name;
	return aprend_buffer_create(instance, &desc, out_buffer, error);
}

// A placeholder texture of 1x1 texels: one layer, six for a cube; of depth
// for a shadow map. Never written. Its view in the binding's shape.
static bool aprend_lit_placeholder(aprend_instance instance, APREND_TEXTURE_VIEW_SHAPE shape, bool depth, aprend_texture2d *out_texture,
                                   aprend_texture_view *out_view) {
	if (*out_view)
		return true;
	const bool cube = shape == APREND_TEXTURE_VIEW_SHAPE_CUBE;
	if (!*out_texture) {
		APREND_TEXTURE2D_DESC desc{};
		desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC);
		desc.width        = 1;
		desc.height       = 1;
		desc.format       = depth ? SPUDGPU_FORMAT_D32_FLOAT : SPUDGPU_FORMAT_R8G8B8A8_UNORM;
		desc.usage        = APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE | (depth ? APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL : 0);
		desc.mip_levels   = 1;
		desc.array_layers = cube ? 6 : 1;
		desc.sample_count = 1;
		desc.cube_capable = cube;
		(void)aprend_texture2d_create(instance, &desc, out_texture, nullptr);
		if (!*out_texture) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's lit placeholder texture can't be created");
			return false;
		}
	}
	APREND_TEXTURE_VIEW_DESC vd{};
	vd.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
	vd.type        = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
	vd.shape       = shape;
	vd.aspect      = depth ? APREND_TEXTURE_VIEW_ASPECT_DEPTH : APREND_TEXTURE_VIEW_ASPECT_COLOR;
	vd.base_mip    = 0;
	vd.mip_count   = 1;
	vd.base_layer  = 0;
	vd.layer_count = cube ? 6 : 1;
	(void)aprend_texture_view_create_2d(*out_texture, &vd, out_view, nullptr);
	if (!*out_view)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's lit placeholder view can't be created");
	return *out_view != nullptr;
}

bool aprend_lit_prepare_resources(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (!aprend_lit_placeholder(instance, APREND_TEXTURE_VIEW_SHAPE_2D, false, &lit.placeholder_2d, &lit.placeholder_2d_view))
		return false;
	if (!aprend_lit_placeholder(instance, APREND_TEXTURE_VIEW_SHAPE_CUBE, false, &lit.placeholder_cube, &lit.placeholder_cube_view))
		return false;
	if (!aprend_lit_placeholder(instance, APREND_TEXTURE_VIEW_SHAPE_2D, true, &lit.placeholder_depth, &lit.placeholder_depth_view))
		return false;
	if (!lit.placeholder_tiles) {
		if (aprend_lit_storage_buffer(instance, 64, false, "aprend lit placeholder tiles", &lit.placeholder_tiles, nullptr) != APRESULT_OK) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's lit placeholder tiles can't be created");
			return false;
		}
	}
	if (!lit.material_sampler)
		lit.material_sampler = aprend_lit_sampler(instance, SPUDGPU_ADDRESS_MODE_REPEAT, false, 0, "lit material sampler");
	if (!lit.environment_sampler)
		lit.environment_sampler = aprend_lit_sampler(instance, SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE, false, 0, "lit environment sampler");
	if (!lit.shadow_sampler)
		lit.shadow_sampler =
		    aprend_lit_sampler(instance, SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE, true, SPUDGPU_COMPARE_OP_LESS_OR_EQUAL, "lit shadow sampler");
	if (!lit.shadow_sampler_reversed)
		lit.shadow_sampler_reversed = aprend_lit_sampler(instance, SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE, true, SPUDGPU_COMPARE_OP_GREATER_OR_EQUAL,
		                                                 "lit reversed shadow sampler");
	return lit.material_sampler && lit.environment_sampler && lit.shadow_sampler && lit.shadow_sampler_reversed;
}

bool aprend_lit_prepare_brdf_lut(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.brdf_lut_view && lit.brdf_lut_target)
		return true;
	if (!lit.brdf_lut) {
		APREND_TEXTURE2D_DESC desc{};
		desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC);
		desc.width        = APREND_BRDF_LUT_SIZE;
		desc.height       = APREND_BRDF_LUT_SIZE;
		desc.format       = APREND_BRDF_LUT_FORMAT;
		desc.usage        = APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE | APREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
		desc.mip_levels   = 1;
		desc.array_layers = 1;
		desc.sample_count = 1;
		(void)aprend_texture2d_create(instance, &desc, &lit.brdf_lut, nullptr);
		if (!lit.brdf_lut) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's BRDF lookup texture can't be created");
			return false;
		}
	}
	APREND_TEXTURE_VIEW_DESC vd{};
	vd.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
	vd.shape       = APREND_TEXTURE_VIEW_SHAPE_2D;
	vd.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
	vd.mip_count   = 1;
	vd.layer_count = 1;
	if (!lit.brdf_lut_view) {
		vd.type           = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
		(void)aprend_texture_view_create_2d(lit.brdf_lut, &vd, &lit.brdf_lut_view, nullptr);
	}
	if (!lit.brdf_lut_target) {
		vd.type             = APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET;
		(void)aprend_texture_view_create_2d(lit.brdf_lut, &vd, &lit.brdf_lut_target, nullptr);
	}
	if (!lit.brdf_lut_view || !lit.brdf_lut_target)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the views of Aprend's BRDF lookup texture can't be created");
	return lit.brdf_lut_view && lit.brdf_lut_target;
}

// A pipeline over aprend_fullscreen_vert.slang with [fragment], rendering to
// [format], [layouts] at its first slots, [push_size] bytes of push
// constants (none if 0).
static aprend_graphics_pipeline aprend_lit_fullscreen_pipeline(aprend_instance instance, aprend_shader fragment, const aprend_binding_layout *layouts,
                                                               uint32_t layout_count, SPUDGPU_FORMAT format, uint32_t push_size,
                                                               const char *name, const spudgpu_blend_attachment_desc *blend = nullptr) {
	APREND_GRAPHICS_PIPELINE_DESC desc{};
	desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_GRAPHICS_PIPELINE_DESC);
	desc._debug_name = name;
	desc._vertex_binding_count       = 0;
	desc._topology                   = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc._cull_mode                  = SPUDGPU_CULL_MODE_NONE;
	desc._front_face_ccw             = true;
	desc._depth_test                 = false;
	desc._depth_write                = false;
	desc._depth_compare_op           = SPUDGPU_COMPARE_OP_NEVER;
	desc.vertex_shader               = aprend_builtin_fullscreen_vert(instance);
	desc.fragment_shader             = fragment;
	desc._vertex_entry_point         = "aprend_fullscreen_vert";
	desc._fragment_entry_point       = aprend_shader_get_reflection(fragment)->entry_point;
	desc.color_attachment_formats[0] = format;
	desc.color_attachment_count      = 1;
	desc.depth_format                = SPUDGPU_FORMAT_UNKNOWN;
	if (blend)
		desc._blends[0] = *blend;
	for (uint32_t i = 0; i < layout_count; ++i)
		desc._binding_layouts[i] = layouts[i];
	desc._binding_layout_count = layout_count;
	if (push_size > 0) {
		desc._push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_FRAGMENT;
		desc._push_constant_ranges[0].offset      = 0;
		desc._push_constant_ranges[0].size        = push_size;
		desc._push_constant_range_count           = 1;
	}
	if (!desc.vertex_shader)
		return nullptr;
	aprend_graphics_pipeline pipeline = nullptr;
	(void)aprend_graphics_pipeline_create(instance, &desc, &pipeline, nullptr);
	if (!pipeline)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's %s pipeline can't be created for format %d", name, (int)format);
	return pipeline;
}

static const struct {
	const APREND_SHADER_PACKAGE *package;
	SPUDGPU_FORMAT format;
	uint32_t push_size;
	const char *name;
} s_passes[APREND_LIT_PASS_COUNT] = {
    {&aprend_environment_from_cube_frag_package, APREND_ENVIRONMENT_FORMAT, sizeof(aprend_environment_params),
     "environment from a cube"},
    {&aprend_environment_from_equirect_frag_package, APREND_ENVIRONMENT_FORMAT,
     sizeof(aprend_environment_params), "environment from a picture"},
    {&aprend_environment_prefilter_frag_package, APREND_ENVIRONMENT_FORMAT, sizeof(aprend_environment_params),
     "environment prefilter"},
    {&aprend_environment_irradiance_frag_package, APREND_ENVIRONMENT_FORMAT,
     sizeof(aprend_environment_params), "environment irradiance"},
    {&aprend_environment_sky_frag_package, APREND_ENVIRONMENT_FORMAT, sizeof(aprend_sky_params), "sky"},
    {&aprend_lit_brdf_lut_frag_package, APREND_BRDF_LUT_FORMAT, sizeof(aprend_size_params), "BRDF lookup"},
    {&aprend_lit_occlusion_frag_package, APREND_OCCLUSION_FORMAT, 0, "ambient occlusion"},
    {&aprend_lit_occlusion_blur_frag_package, SPUDGPU_FORMAT_UNKNOWN, sizeof(aprend_size_params),
     "ambient occlusion blur"},
    {&aprend_exposure_luminance_frag_package, APREND_EXPOSURE_LUMINANCE_FORMAT, sizeof(aprend_size_params),
     "exposure luminance"},
    {&aprend_exposure_adapt_frag_package, APREND_EXPOSURE_FORMAT, sizeof(aprend_adapt_params), "exposure adapt"},
    {&aprend_environment_hosek_frag_package, APREND_ENVIRONMENT_FORMAT, sizeof(aprend_sky_params),
     "Hosek-Wilkie sky"},
    {&aprend_exposure_adapt_histogram_frag_package, APREND_EXPOSURE_FORMAT,
     sizeof(aprend_histogram_adapt_params), "exposure adapt from a histogram"},
    {&aprend_lit_hiz_frag_package, APREND_HIZ_FORMAT, sizeof(aprend_hiz_params), "Hi-Z pyramid"},
    {&aprend_lit_hiz_down_frag_package, APREND_HIZ_FORMAT, sizeof(aprend_size_params), "Hi-Z pyramid level"},
};

// Every pass's fragment shader, the pass layout (a texture at 0, a sampler
// at 1), and the occlusion pass's own layout. False, reported, if one can't
// be made.
static bool aprend_lit_prepare_passes(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (!aprend_lit_prepare_resources(instance))
		return false;
	for (uint32_t pass = 0; pass < APREND_LIT_PASS_COUNT; ++pass) {
		if (lit.pass_frags[pass])
			continue;
		(void)aprend_shader_create(instance, s_passes[pass].package, &lit.pass_frags[pass], nullptr);
		if (!lit.pass_frags[pass]) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's %s shader can't be created", s_passes[pass].name);
			return false;
		}
	}
	if (!lit.pass_layout) {
		// The passes that read a texture and a sampler at set 0.
		aprend_shader readers[5] = {lit.pass_frags[APREND_LIT_PASS_FROM_CUBE], lit.pass_frags[APREND_LIT_PASS_FROM_EQUIRECT],
		                            lit.pass_frags[APREND_LIT_PASS_PREFILTER], lit.pass_frags[APREND_LIT_PASS_IRRADIANCE],
		                            lit.pass_frags[APREND_LIT_PASS_BLUR]};
		(void)aprend_binding_layout_create_from_shaders(instance, readers, 5, 0, &lit.pass_layout, nullptr);
		if (!lit.pass_layout) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's passes can't be created");
			return false;
		}
	}
	if (!lit.occlusion_layout) {
		(void)aprend_binding_layout_create_from_shaders(instance, &lit.pass_frags[APREND_LIT_PASS_OCCLUSION], 1, 0, &lit.occlusion_layout, nullptr);
		if (!lit.occlusion_layout || !aprend_lit_check_occlusion_block(lit.occlusion_layout)) {
			aprend_binding_layout_destroy(lit.occlusion_layout);
			lit.occlusion_layout = nullptr;
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's ambient occlusion can't be created");
			return false;
		}
	}
	if (!lit.histogram_comp) {
		(void)aprend_shader_create(instance, &aprend_exposure_histogram_comp_package, &lit.histogram_comp, nullptr);
		if (!lit.histogram_comp) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's exposure histogram shader can't be created");
			return false;
		}
	}
	if (!lit.histogram_layout) {
		// The histogram's dispatch writes the bins and the adapting pass
		// reads them: one layout, so one set serves both.
		aprend_shader readers[2] = {lit.histogram_comp, lit.pass_frags[APREND_LIT_PASS_ADAPT_HISTOGRAM]};
		(void)aprend_binding_layout_create_from_shaders(instance, readers, 2, 0, &lit.histogram_layout, nullptr);
		if (!lit.histogram_layout) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's exposure histogram can't be created");
			return false;
		}
	}
	if (!lit.hosek_layout) {
		(void)aprend_binding_layout_create_from_shaders(instance, &lit.pass_frags[APREND_LIT_PASS_HOSEK_SKY], 1, 0, &lit.hosek_layout, nullptr);
		if (!lit.hosek_layout || !aprend_lit_check_hosek_block(lit.hosek_layout)) {
			aprend_binding_layout_destroy(lit.hosek_layout);
			lit.hosek_layout = nullptr;
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's Hosek-Wilkie sky can't be created");
			return false;
		}
	}
	return true;
}

aprend_graphics_pipeline aprend_lit_pass_pipeline(aprend_instance instance, uint32_t pass) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.pass_pipelines[pass])
		return lit.pass_pipelines[pass];
	if (!aprend_lit_prepare_passes(instance))
		return nullptr;
	// What each reads: a texture at set 0 (and the previous exposure at set
	// 1); nothing; or the occlusion block and a depth buffer.
	aprend_binding_layout layouts[2] = {lit.pass_layout, lit.pass_layout};
	uint32_t count                   = 1;
	if (pass == APREND_LIT_PASS_SKY || pass == APREND_LIT_PASS_BRDF_LUT)
		count = 0;
	else if (pass == APREND_LIT_PASS_OCCLUSION) {
		layouts[0] = lit.occlusion_layout;
		count      = 2;
	} else if (pass == APREND_LIT_PASS_HOSEK_SKY)
		layouts[0] = lit.hosek_layout;
	else if (pass == APREND_LIT_PASS_ADAPT_HISTOGRAM) {
		layouts[0] = lit.histogram_layout;
		count      = 2;
	}
	else if (pass == APREND_LIT_PASS_ADAPT)
		count = 2;
	lit.pass_pipelines[pass] = aprend_lit_fullscreen_pipeline(instance, lit.pass_frags[pass], layouts, count, s_passes[pass].format,
	                                                         s_passes[pass].push_size, s_passes[pass].name);
	return lit.pass_pipelines[pass];
}

static aprend_graphics_pipeline aprend_lit_format_pipeline(aprend_instance instance, std::vector<aprend_lit_builtins::format_pipeline> &pipelines,
                                                           aprend_shader fragment, const aprend_binding_layout *layouts, uint32_t layout_count,
                                                           SPUDGPU_FORMAT format, uint32_t push_size, const char *name,
                                                           const spudgpu_blend_attachment_desc *blend = nullptr) {
	for (const aprend_lit_builtins::format_pipeline &entry : pipelines)
		if (entry.format == format)
			return entry.pipeline;
	aprend_graphics_pipeline pipeline = aprend_lit_fullscreen_pipeline(instance, fragment, layouts, layout_count, format, push_size, name, blend);
	if (!pipeline)
		return nullptr;
	try {
		pipelines.push_back({format, pipeline});
	} catch (...) {
		aprend_graphics_pipeline_destroy(pipeline);
		aprend_report(instance, APRESULT_INVALID_ARGUMENT, "no memory to keep Aprend's %s pipeline", name);
		return nullptr;
	}
	return pipeline;
}

aprend_graphics_pipeline aprend_lit_blur_pipeline(aprend_instance instance, SPUDGPU_FORMAT format) {
	if (!aprend_lit_prepare_passes(instance))
		return nullptr;
	aprend_lit_builtins &lit = instance->builtins.lit;
	return aprend_lit_format_pipeline(instance, lit.blur_pipelines, lit.pass_frags[APREND_LIT_PASS_BLUR], &lit.pass_layout, 1, format,
	                                  sizeof(aprend_size_params), "ambient occlusion blur");
}

aprend_graphics_pipeline aprend_lit_tone_map_pipeline(aprend_instance instance, SPUDGPU_FORMAT format) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	// It reads its source through the view's bloom set, and an exposure
	// through a view's environment set.
	if (!aprend_builtin_bloom_prepare(instance) || !aprend_lit_prepare_passes(instance))
		return nullptr;
	if (!lit.tone_map_frag) {
		(void)aprend_shader_create(instance, &aprend_tone_map_frag_package, &lit.tone_map_frag, nullptr);
		if (!lit.tone_map_frag) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's tone map shader can't be created");
			return nullptr;
		}
	}
	if (!lit.tone_map_layout) {
		(void)aprend_binding_layout_create_from_shaders(instance, &lit.tone_map_frag, 1, 0, &lit.tone_map_layout, nullptr);
		if (!lit.tone_map_layout) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's tone map shader can't be created");
			return nullptr;
		}
	}
	const aprend_binding_layout layouts[2] = {lit.tone_map_layout, lit.pass_layout};
	return aprend_lit_format_pipeline(instance, lit.tone_map_pipelines, lit.tone_map_frag, layouts, 2, format, sizeof(aprend_tone_map_params),
	                                  "tone map");
}

static aprend_binding_layout aprend_lit_scene_layout(aprend_instance instance);

// A shader of Aprend's made from [package] into [*shader] if it isn't, and
// the layout of its set [index] into [*layout] if it isn't. False, reported
// naming [what], if either can't be.
static bool aprend_lit_own_shader(aprend_instance instance, const APREND_SHADER_PACKAGE *package, aprend_shader *shader, uint32_t index,
                                  aprend_binding_layout *layout, const char *what) {
	if (!*shader) {
		(void)aprend_shader_create(instance, package, shader, nullptr);
		if (!*shader) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's %s shader can't be created", what);
			return false;
		}
	}
	if (!*layout) {
		(void)aprend_binding_layout_create_from_shaders(instance, shader, 1, index, layout, nullptr);
		if (!*layout) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's %s shader can't be created", what);
			return false;
		}
	}
	return true;
}

aprend_compute_pipeline aprend_lit_ssr_pipeline(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.ssr_pipeline)
		return lit.ssr_pipeline;
	if (!aprend_lit_prepare_resources(instance))
		return nullptr;
	if (!aprend_lit_own_shader(instance, &aprend_lit_ssr_comp_package, &lit.ssr_comp, 0, &lit.ssr_scene_layout, "screen-space reflections"))
		return nullptr;
	if (!aprend_lit_own_shader(instance, &aprend_lit_ssr_comp_package, &lit.ssr_comp, 1, &lit.ssr_input_layout, "screen-space reflections"))
		return nullptr;
	if (!aprend_lit_check_scene_block(lit.ssr_scene_layout)) {
		aprend_binding_layout_destroy(lit.ssr_scene_layout);
		lit.ssr_scene_layout = nullptr;
		return nullptr;
	}
	APREND_COMPUTE_PIPELINE_DESC desc{};
	desc.base                                 = apri_struct_base(APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC);
	desc._debug_name                          = "aprend screen-space reflections";
	desc.compute_shader                       = lit.ssr_comp;
	desc._entry_point                         = "aprend_lit_ssr_comp";
	desc._binding_layouts[0]                  = lit.ssr_scene_layout;
	desc._binding_layouts[1]                  = lit.ssr_input_layout;
	desc._binding_layout_count                = 2;
	desc._push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE;
	desc._push_constant_ranges[0].offset      = 0;
	desc._push_constant_ranges[0].size        = sizeof(aprend_ssr_params);
	desc._push_constant_range_count           = 1;
	APREND_ERROR error;
	(void)aprend_compute_pipeline_create(instance, &desc, &lit.ssr_pipeline, &error);
	if (!lit.ssr_pipeline)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's screen-space reflections pipeline can't be created: %s", error.message);
	return lit.ssr_pipeline;
}

aprend_graphics_pipeline aprend_lit_reflect_pipeline(aprend_instance instance, SPUDGPU_FORMAT format) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	aprend_binding_layout scene_layout = aprend_lit_scene_layout(instance);
	if (!scene_layout)
		return nullptr;
	if (!aprend_lit_own_shader(instance, &aprend_lit_reflect_frag_package, &lit.reflect_frag, 1, &lit.reflect_input_layout, "reflection composite"))
		return nullptr;
	// Added onto the picture: what the shader writes is the change.
	const spudgpu_blend_attachment_desc blend = aprend_blend_additive();
	const aprend_binding_layout layouts[2]    = {scene_layout, lit.reflect_input_layout};
	return aprend_lit_format_pipeline(instance, lit.reflect_pipelines, lit.reflect_frag, layouts, 2, format, sizeof(aprend_reflect_params),
	                                  "reflection composite", &blend);
}

aprend_binding_set aprend_view_set_get(aprend_instance instance, aprend_view_set (&sets)[APREND_VIEW_SETS], aprend_binding_layout layout,
                                       const aprend_texture_view *views, uint32_t view_count, const APREND_BINDING_SET_ENTRY *entries,
                                       uint32_t entry_count, const char *what) {
	aprend_view_set *oldest = &sets[0];
	for (aprend_view_set &kept : sets) {
		bool same = kept.set != nullptr && kept.layout == layout;
		for (uint32_t i = 0; i < view_count && same; ++i)
			same = kept.views[i] == views[i] && kept.serials[i] == views[i]->serial;
		if (same) {
			kept.used = instance->frame_number;
			return kept.set;
		}
		if (!kept.set || (oldest->set && kept.used < oldest->used))
			oldest = &kept;
	}
	aprend_binding_set made = nullptr;
	APREND_ERROR error;
	if (aprend_binding_set_create(layout, entries, entry_count, &made, &error) != APRESULT_OK) {
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "%s's set can't be made: %s", what, error.message);
		return nullptr;
	}
	aprend_binding_set_destroy(oldest->set);
	*oldest        = aprend_view_set{};
	oldest->set    = made;
	oldest->layout = layout;
	for (uint32_t i = 0; i < view_count; ++i) {
		oldest->views[i]   = views[i];
		oldest->serials[i] = views[i]->serial;
	}
	oldest->used = instance->frame_number;
	return made;
}

void aprend_view_sets_destroy(aprend_view_set (&sets)[APREND_VIEW_SETS]) {
	for (aprend_view_set &kept : sets) {
		aprend_binding_set_destroy(kept.set);
		kept = aprend_view_set{};
	}
}

aprend_compute_pipeline aprend_lit_histogram_pipeline(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.histogram_pipeline)
		return lit.histogram_pipeline;
	if (!aprend_lit_prepare_passes(instance))
		return nullptr;
	APREND_COMPUTE_PIPELINE_DESC desc{};
	desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC);
	desc._debug_name = "aprend exposure histogram";
	desc.compute_shader                       = lit.histogram_comp;
	desc._entry_point                         = "aprend_exposure_histogram_comp";
	desc._binding_layouts[0]                  = lit.histogram_layout;
	desc._binding_layout_count                = 1;
	desc._push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_COMPUTE;
	desc._push_constant_ranges[0].offset      = 0;
	desc._push_constant_ranges[0].size        = sizeof(aprend_histogram_params);
	desc._push_constant_range_count           = 1;
	(void)aprend_compute_pipeline_create(instance, &desc, &lit.histogram_pipeline, nullptr);
	if (!lit.histogram_pipeline)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's exposure histogram pipeline can't be created");
	return lit.histogram_pipeline;
}

aprend_compute_pipeline aprend_lit_cull_pipeline(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.cull_pipeline)
		return lit.cull_pipeline;
	if (!lit.cull_comp) {
		(void)aprend_shader_create(instance, &aprend_lit_cull_comp_package, &lit.cull_comp, nullptr);
		if (!lit.cull_comp) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's light culling shader can't be created");
			return nullptr;
		}
	}
	if (!lit.cull_layout) {
		(void)aprend_binding_layout_create_from_shaders(instance, &lit.cull_comp, 1, 0, &lit.cull_layout, nullptr);
		if (!lit.cull_layout || !aprend_lit_check_scene_block(lit.cull_layout)) {
			aprend_binding_layout_destroy(lit.cull_layout);
			lit.cull_layout = nullptr;
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's light culling can't be created");
			return nullptr;
		}
	}
	APREND_COMPUTE_PIPELINE_DESC desc{};
	desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC);
	desc._debug_name = "aprend light culling";
	desc.compute_shader        = lit.cull_comp;
	desc._entry_point          = "aprend_lit_cull_comp";
	desc._binding_layouts[0]   = lit.cull_layout;
	desc._binding_layout_count = 1;
	(void)aprend_compute_pipeline_create(instance, &desc, &lit.cull_pipeline, nullptr);
	if (!lit.cull_pipeline)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's light culling pipeline can't be created");
	return lit.cull_pipeline;
}

aprend_compute_pipeline aprend_lit_cull_bounded_pipeline(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.cull_bounded_pipeline)
		return lit.cull_bounded_pipeline;
	if (!aprend_lit_cull_pipeline(instance) || !aprend_lit_prepare_passes(instance))
		return nullptr;
	if (!lit.cull_bounded_comp) {
		(void)aprend_shader_create(instance, &aprend_lit_cull_bounded_comp_package, &lit.cull_bounded_comp, nullptr);
		if (!lit.cull_bounded_comp) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's light culling shader with depth bounds can't be created");
			return nullptr;
		}
	}
	// Set 0 as the unbounded culling's, so the scene's cull material serves
	// both; set 1 the depth, in a layout of its own that the compute stage
	// sees (the pass layout's are the fragment stage's).
	if (!lit.cull_depth_layout) {
		(void)aprend_binding_layout_create_from_shaders(instance, &lit.cull_bounded_comp, 1, 1, &lit.cull_depth_layout, nullptr);
		if (!lit.cull_depth_layout) {
			aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the depth layout of Aprend's light culling with depth bounds can't be created");
			return nullptr;
		}
	}
	APREND_COMPUTE_PIPELINE_DESC desc{};
	desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC);
	desc._debug_name = "aprend light culling with depth bounds";
	desc.compute_shader        = lit.cull_bounded_comp;
	desc._entry_point          = "aprend_lit_cull_bounded_comp";
	desc._binding_layouts[0]   = lit.cull_layout;
	desc._binding_layouts[1]   = lit.cull_depth_layout;
	desc._binding_layout_count = 2;
	(void)aprend_compute_pipeline_create(instance, &desc, &lit.cull_bounded_pipeline, nullptr);
	if (!lit.cull_bounded_pipeline)
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "Aprend's light culling pipeline with depth bounds can't be created");
	return lit.cull_bounded_pipeline;
}

aprend_binding_set aprend_texture_view_cull_depth_set(aprend_texture_view view) {
	if (view->cull_depth_set)
		return view->cull_depth_set;
	aprend_instance instance = view->instance;
	// The depth alone: the shader loads texels, with no sampler, so its
	// layout (made from it) has none.
	APREND_BINDING_SET_ENTRY entries[1]{};
	entries[0]._binding                  = 0;
	entries[0]._type                     = SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	entries[0]._resource._image._view    = view;
	(void)aprend_binding_set_create(instance->builtins.lit.cull_depth_layout, entries, 1, &view->cull_depth_set, nullptr);
	if (!view->cull_depth_set)
		aprend_report(view->instance, APRESULT_APREND_GPU_FAILURE, "the binding set that reads a depth view for light culling can't be created");
	return view->cull_depth_set;
}

aprend_binding_set aprend_texture_view_environment_set(aprend_texture_view view) {
	if (view->environment_set)
		return view->environment_set;
	aprend_instance instance = view->instance;
	if (!aprend_lit_prepare_passes(instance))
		return nullptr;
	APREND_BINDING_SET_ENTRY entries[2]{};
	entries[0]._binding                  = 0;
	entries[0]._type                     = SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	entries[0]._resource._image._view    = view;
	entries[1]._binding                  = 1;
	entries[1]._type                     = SPUDGPU_DESCRIPTOR_TYPE_SAMPLER;
	entries[1]._resource._image._sampler = instance->builtins.lit.environment_sampler;
	(void)aprend_binding_set_create(instance->builtins.lit.pass_layout, entries, 2, &view->environment_set, nullptr);
	if (!view->environment_set)
		aprend_report(view->instance, APRESULT_APREND_GPU_FAILURE, "the binding set that reads a view can't be created");
	return view->environment_set;
}

void aprend_lit_builtins_destroy(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	aprend_view_sets_destroy(lit.view_sets);
	for (const aprend_lit_builtins::format_pipeline &entry : lit.reflect_pipelines)
		aprend_graphics_pipeline_destroy(entry.pipeline);
	lit.reflect_pipelines.clear();
	aprend_compute_pipeline_destroy(lit.ssr_pipeline);
	lit.ssr_pipeline = nullptr;
	for (const aprend_lit_builtins::format_pipeline &entry : lit.tone_map_pipelines)
		aprend_graphics_pipeline_destroy(entry.pipeline);
	lit.tone_map_pipelines.clear();
	for (const aprend_lit_builtins::format_pipeline &entry : lit.blur_pipelines)
		aprend_graphics_pipeline_destroy(entry.pipeline);
	lit.blur_pipelines.clear();
	for (aprend_graphics_pipeline &pipeline : lit.pass_pipelines) {
		aprend_graphics_pipeline_destroy(pipeline);
		pipeline = nullptr;
	}
	aprend_compute_pipeline_destroy(lit.cull_pipeline);
	lit.cull_pipeline = nullptr;
	aprend_compute_pipeline_destroy(lit.cull_bounded_pipeline);
	lit.cull_bounded_pipeline = nullptr;
	aprend_compute_pipeline_destroy(lit.histogram_pipeline);
	lit.histogram_pipeline           = nullptr;
	aprend_binding_layout *layouts[] = {&lit.reflect_input_layout, &lit.ssr_input_layout, &lit.ssr_scene_layout,  &lit.tone_map_layout,
	                                    &lit.pass_layout,          &lit.occlusion_layout, &lit.hosek_layout,       &lit.histogram_layout,
	                                    &lit.cull_depth_layout,    &lit.cull_layout,      &lit.material_layout,    &lit.shadow_layout,
	                                    &lit.scene_layout};
	for (aprend_binding_layout *layout : layouts) {
		aprend_binding_layout_destroy(*layout);
		*layout = nullptr;
	}
	aprend_shader_destroy(lit.tone_map_frag);
	lit.tone_map_frag = nullptr;
	aprend_shader_destroy(lit.ssr_comp);
	lit.ssr_comp = nullptr;
	aprend_shader_destroy(lit.reflect_frag);
	lit.reflect_frag = nullptr;
	aprend_shader_destroy(lit.cull_comp);
	lit.cull_comp = nullptr;
	aprend_shader_destroy(lit.cull_bounded_comp);
	lit.cull_bounded_comp = nullptr;
	aprend_shader_destroy(lit.histogram_comp);
	lit.histogram_comp = nullptr;
	for (aprend_shader &shader : lit.pass_frags) {
		aprend_shader_destroy(shader);
		shader = nullptr;
	}
	for (aprend_shader &shader : lit.shaders) {
		aprend_shader_destroy(shader);
		shader = nullptr;
	}
	aprend_sampler *samplers[] = {&lit.shadow_sampler_reversed, &lit.shadow_sampler, &lit.environment_sampler, &lit.material_sampler};
	for (aprend_sampler *sampler : samplers) {
		aprend_sampler_destroy(*sampler);
		*sampler = nullptr;
	}
	aprend_texture_view *views[] = {&lit.brdf_lut_target, &lit.brdf_lut_view, &lit.placeholder_depth_view, &lit.placeholder_cube_view,
	                                &lit.placeholder_2d_view};
	for (aprend_texture_view *view : views) {
		aprend_texture_view_destroy(*view);
		*view = nullptr;
	}
	aprend_texture2d *textures[] = {&lit.brdf_lut, &lit.placeholder_depth, &lit.placeholder_cube, &lit.placeholder_2d};
	for (aprend_texture2d *texture : textures) {
		aprend_texture2d_destroy(*texture);
		*texture = nullptr;
	}
	aprend_buffer_destroy(lit.placeholder_tiles);
	lit.placeholder_tiles = nullptr;
}

// The slot of [material]'s block named [name]: where its bytes are.
static aprend_material_slot *aprend_lit_block_slot(aprend_material material, const char *name) {
	for (uint32_t i = 0; i < material->slot_count; ++i)
		if (material->slots[i].bytes && strcmp(material->slots[i].name, name) == 0)
			return &material->slots[i];
	return nullptr;
}

static void aprend_lit_write_block(aprend_material material, const char *name, const void *bytes, size_t size) {
	aprend_material_slot *slot = aprend_lit_block_slot(material, name);
	memcpy(slot->bytes, bytes, size);
	slot->dirty = true;
}

// ---- Vertex layouts -----------------------------------------------------------------

static const APREND_VERTEX_ELEMENT s_lit_vertex_elements[] = {
    {"in_position", APREND_VERTEX_ELEMENT_TYPE_VEC3, (uint32_t)offsetof(APREND_LIT_VERTEX, position)},
    {"in_normal", APREND_VERTEX_ELEMENT_TYPE_VEC3, (uint32_t)offsetof(APREND_LIT_VERTEX, normal)},
    {"in_uv", APREND_VERTEX_ELEMENT_TYPE_VEC2, (uint32_t)offsetof(APREND_LIT_VERTEX, uv)},
    {"in_tangent", APREND_VERTEX_ELEMENT_TYPE_VEC4, (uint32_t)offsetof(APREND_LIT_VERTEX, tangent)},
};
static const APREND_VERTEX_ELEMENT s_lit_color_elements[] = {
    {"color", APREND_VERTEX_ELEMENT_TYPE_VEC4, 0},
};
static const APREND_VERTEX_ELEMENT s_lit_instance_elements[] = {
    {"in_transform0", APREND_VERTEX_ELEMENT_TYPE_VEC4, 0},
    {"in_transform1", APREND_VERTEX_ELEMENT_TYPE_VEC4, 16},
    {"in_transform2", APREND_VERTEX_ELEMENT_TYPE_VEC4, 32},
    {"in_transform3", APREND_VERTEX_ELEMENT_TYPE_VEC4, 48},
};

// aprend_lit_get_scene_layout's work: the instance's layout, made on
// first use. NULL, reported, if it can't be.
static aprend_binding_layout aprend_lit_scene_layout(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.scene_layout)
		return lit.scene_layout;
	// Every shader that reads set 0, so each binding has every stage that
	// reads it.
	aprend_shader shaders[4] = {aprend_lit_shader(instance, APREND_LIT_SHADER_SURFACE_FRAGMENT),
	                            aprend_lit_shader(instance, APREND_LIT_SHADER_SURFACE_VERTEX),
	                            aprend_lit_shader(instance, APREND_LIT_SHADER_SURFACE_VERTEX_COLOR),
	                            aprend_lit_shader(instance, APREND_LIT_SHADER_SKY_FRAGMENT)};
	for (aprend_shader shader : shaders)
		if (!shader)
			return nullptr;
	aprend_binding_layout layout = nullptr;
	(void)aprend_binding_layout_create_from_shaders(instance, shaders, 4, APREND_LIT_SCENE_SET, &layout, nullptr);
	if (!layout) {
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's lit scene can't be created");
		return nullptr;
	}
	if (!aprend_lit_check_scene_block(layout)) {
		aprend_binding_layout_destroy(layout);
		return nullptr;
	}
	lit.scene_layout = layout;
	return layout;
}

extern "C" {

APREND_VERTEX_LAYOUT aprend_lit_vertex_layout(void) {
	APREND_VERTEX_LAYOUT layout{};
	layout.count    = 4;
	layout.elements = s_lit_vertex_elements;
	layout.stride   = (uint32_t)sizeof(APREND_LIT_VERTEX);
	return layout;
}

APREND_VERTEX_LAYOUT aprend_lit_vertex_color_layout(void) {
	APREND_VERTEX_LAYOUT layout{};
	layout.count    = 1;
	layout.elements = s_lit_color_elements;
	layout.stride   = (uint32_t)(sizeof(float) * 4);
	return layout;
}

APREND_VERTEX_LAYOUT aprend_lit_instance_layout(void) {
	APREND_VERTEX_LAYOUT layout{};
	layout.count    = 4;
	layout.elements = s_lit_instance_elements;
	layout.stride   = (uint32_t)sizeof(APREND_LIT_INSTANCE);
	return layout;
}

APRESULT aprend_lit_get_shader(aprend_instance instance, APREND_LIT_SHADER shader, aprend_shader *out_shader, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_shader)
		*out_shader = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_lit_get_shader", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	if (shader > APREND_LIT_SHADER_SURFACE_REFLECT_FRAGMENT)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "%u is not an APREND_LIT_SHADER", shader);
	if (!out_shader)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_shader is NULL");
	*out_shader = aprend_lit_shader(instance, shader);
	if (!*out_shader)
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "Aprend's lit shader %u can't be made", shader);
	return APRESULT_OK;
}

// aprend_lit_get_shadow_layout's work: the instance's layout, made on
// first use. NULL, reported, if it can't be.
static aprend_binding_layout aprend_lit_shadow_layout(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.shadow_layout)
		return lit.shadow_layout;
	aprend_shader vertices[2] = {aprend_lit_shader(instance, APREND_LIT_SHADER_SHADOW_VERTEX),
	                             aprend_lit_shader(instance, APREND_LIT_SHADER_SHADOW_VERTEX_COLOR)};
	if (!vertices[0])
		return nullptr;
	if (!vertices[1])
		return nullptr;
	aprend_binding_layout layout = nullptr;
	(void)aprend_binding_layout_create_from_shaders(instance, vertices, 2, APREND_LIT_SCENE_SET, &layout, nullptr);
	if (!layout) {
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's lit shadow pass can't be created");
		return nullptr;
	}
	if (!aprend_lit_check_shadow_pass_block(layout)) {
		aprend_binding_layout_destroy(layout);
		return nullptr;
	}
	lit.shadow_layout = layout;
	return layout;
}

// aprend_lit_get_material_layout's work: the instance's layout, made on
// first use. NULL, reported, if it can't be.
static aprend_binding_layout aprend_lit_material_layout(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.material_layout)
		return lit.material_layout;
	aprend_shader shaders[2] = {aprend_lit_shader(instance, APREND_LIT_SHADER_SURFACE_FRAGMENT),
	                            aprend_lit_shader(instance, APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT)};
	if (!shaders[0] || !shaders[1])
		return nullptr;
	aprend_binding_layout layout = nullptr;
	(void)aprend_binding_layout_create_from_shaders(instance, shaders, 2, APREND_LIT_MATERIAL_SET, &layout, nullptr);
	if (!layout) {
		aprend_report(instance, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's lit materials can't be created");
		return nullptr;
	}
	if (!aprend_lit_check_material_block(layout)) {
		aprend_binding_layout_destroy(layout);
		return nullptr;
	}
	lit.material_layout = layout;
	return layout;
}

// A layout getter's public form: the layout through [out_layout], or why
// it can't be made.
static APRESULT aprend_lit_layout_result(
    const char *who,
    aprend_instance instance,
    aprend_binding_layout (*make)(aprend_instance),
    const char *what,
    aprend_binding_layout *out_layout,
    APREND_ERROR *error) {
	aprend_error_clear(error);
	if (out_layout)
		*out_layout = nullptr;
	if (!instance)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, who, "instance"))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	if (!out_layout)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "out_layout is NULL");
	*out_layout = make(instance);
	if (!*out_layout)
		return aprend_fail(error, APRESULT_APREND_GPU_FAILURE, "the binding layout of Aprend's lit %s can't be made", what);
	return APRESULT_OK;
}

APRESULT aprend_lit_get_scene_layout(aprend_instance instance, aprend_binding_layout *out_layout, APREND_ERROR *out_error) {
	return aprend_lit_layout_result("aprend_lit_get_scene_layout", instance, aprend_lit_scene_layout, "scene", out_layout, out_error);
}

APRESULT aprend_lit_get_shadow_layout(aprend_instance instance, aprend_binding_layout *out_layout, APREND_ERROR *out_error) {
	return aprend_lit_layout_result("aprend_lit_get_shadow_layout", instance, aprend_lit_shadow_layout, "shadow pass", out_layout, out_error);
}

APRESULT aprend_lit_get_material_layout(aprend_instance instance, aprend_binding_layout *out_layout, APREND_ERROR *out_error) {
	return aprend_lit_layout_result("aprend_lit_get_material_layout", instance, aprend_lit_material_layout, "materials", out_layout, out_error);
}

// ---- Materials ----------------------------------------------------------------------

static const char *const s_map_bindings[4] = {"base_color_map", "normal_map", "orm_map", "emission_map"};

// [material]'s block, through [out_slot], if it is a lit material;
// APRESULT_INVALID_ARGUMENT, [error] saying why, if not. [who] is the
// public call, for the destroyed-handle check (apristale.hpp).
static APRESULT aprend_lit_material_slot(const char *who, aprend_material material, aprend_material_slot **out_slot, APREND_ERROR *error) {
	if (!material)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "material is NULL");
	if (APRI_STALE(material, who, "material"))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "material is destroyed");
	if (!material->layout || material->layout != material->instance->builtins.lit.material_layout)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "material is not a lit material (aprend_lit_material_create)");
	aprend_material_slot *slot = aprend_lit_block_slot(material, "u_lit_material");
	if (!slot || slot->block_size < sizeof(aprend_lit_material_block))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the material has no lit block");
	*out_slot = slot;
	return APRESULT_OK;
}

APRESULT aprend_lit_material_create(aprend_instance instance, aprend_material *out_material, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_material)
		*out_material = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_lit_material_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_lit_material_create");
	if (!out_material)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_material is NULL");
	aprend_binding_layout layout = nullptr;
	if (const APRESULT made = aprend_lit_get_material_layout(instance, &layout, out_error))
		return made;
	if (!aprend_lit_prepare_resources(instance))
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "Aprend's lit resources can't be made");
	APREND_MATERIAL_DESC desc{};
	desc.base                = apri_struct_base(APRI_STRUCT_TYPE_APREND_MATERIAL_DESC);
	desc._layout             = layout;
	aprend_material material = nullptr;
	if (const APRESULT made = aprend_material_create(instance, &desc, &material, out_error))
		return made;
	const aprend_lit_builtins &lit = instance->builtins.lit;
	APRESULT set                   = aprend_material_set_sampler(material, "material_sampler", 0, lit.material_sampler, out_error);
	for (const char *binding : s_map_bindings)
		if (set == APRESULT_OK)
			set = aprend_material_set_texture(material, binding, 0, lit.placeholder_2d_view, out_error);
	aprend_material_slot *slot = nullptr;
	if (set == APRESULT_OK)
		set = aprend_lit_material_slot("aprend_lit_material_create", material, &slot, out_error);
	if (set != APRESULT_OK) {
		aprend_material_destroy(material);
		return set;
	}
	aprend_lit_material_block block{};
	for (int i = 0; i < 4; ++i)
		block.base_color[i] = 1.0f;
	block.sheen[3]            = 0.5f;
	block.roughness           = 0.5f;
	block.metalness           = 0.0f;
	block.specular            = 0.5f;
	block.normal_scale        = 1.0f;
	block.occlusion_strength  = 1.0f;
	block.alpha_cutoff        = 0.0f;
	block.clearcoat           = 0.0f;
	block.clearcoat_roughness = 0.1f;
	block.maps                = 0;
	block.uv_transform[0]     = 1.0f;
	block.uv_transform[5]     = 1.0f;
	for (float *rows : block.map_uv_transform) {
		rows[0] = 1.0f;
		rows[5] = 1.0f;
	}
	block.transmission[1] = 1.5f; // glTF's default index
	for (int i = 0; i < 3; ++i)
		block.attenuation[i] = 1.0f;
	memcpy(slot->bytes, &block, sizeof(block));
	slot->dirty = true;
	// Its entry in the lit-material table, on a device with ray queries (D4).
	if (const APRESULT taken = aprend_rt_lit_take(instance, &block, &material->rt_lit_slot, out_error)) {
		aprend_material_destroy(material);
		return taken;
	}
	*out_material = material;
	return APRESULT_OK;
}

APRESULT aprend_lit_material_set_values(aprend_material material, const APREND_LIT_MATERIAL_VALUES *values, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	aprend_material_slot *slot = nullptr;
	if (const APRESULT lit = aprend_lit_material_slot("aprend_lit_material_set_values", material, &slot, out_error))
		return lit;
	if (!values)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "values is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "values", &values->base, APRI_STRUCT_TYPE_APREND_LIT_MATERIAL_VALUES))
		return base;
	const APREND_LIT_MATERIAL_VALUES &v = *values;
	if (!all_finite(v.base_color, 4) || !all_finite(v.emission, 3) || !finite(v.roughness) || !finite(v.metalness) || !finite(v.specular) ||
	    !finite(v.normal_scale) || !finite(v.occlusion_strength) || !finite(v.alpha_cutoff) || !all_finite(v.uv_transform, 6) ||
	    !finite(v.clearcoat) || !finite(v.clearcoat_roughness) || !all_finite(v.sheen_color, 3) || !finite(v.sheen_roughness) ||
	    !finite(v.transmission) || !finite(v.ior) || !finite(v.thickness) || !all_finite(v.attenuation_color, 3) || !finite(v.attenuation_distance)) {
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a value is not a finite number");
	}
	aprend_lit_material_block block;
	memcpy(&block, slot->bytes, sizeof(block));
	memcpy(block.base_color, v.base_color, sizeof(block.base_color));
	memcpy(block.emission, v.emission, sizeof(float) * 3);
	block.emission[3] = 0.0f;
	memcpy(block.sheen, v.sheen_color, sizeof(float) * 3);
	block.sheen[3]            = v.sheen_roughness;
	block.roughness           = v.roughness;
	block.metalness           = v.metalness;
	block.specular            = v.specular;
	block.normal_scale        = v.normal_scale;
	block.occlusion_strength  = v.occlusion_strength;
	block.alpha_cutoff        = v.alpha_cutoff;
	block.clearcoat           = v.clearcoat;
	block.clearcoat_roughness = v.clearcoat_roughness;
	const float transmission[4] = {v.transmission, v.ior, v.thickness, v.attenuation_distance};
	memcpy(block.transmission, transmission, sizeof(transmission));
	memcpy(block.attenuation, v.attenuation_color, sizeof(float) * 3);
	block.attenuation[3] = 0.0f;
	// The maps' bits stay: they follow the textures set, not the values.
	const float *t      = v.uv_transform;
	const float rows[8] = {t[0], t[1], t[2], 0.0f, t[3], t[4], t[5], 0.0f};
	memcpy(block.uv_transform, rows, sizeof(rows));
	memcpy(slot->bytes, &block, sizeof(block));
	slot->dirty = true;
	// Staged for the next submission that reads the lit-material table (D4).
	aprend_rt_lit_stage(material->instance, material->rt_lit_slot, &block);
	return APRESULT_OK;
}

APRESULT aprend_lit_material_get_values(aprend_material material, APREND_LIT_MATERIAL_VALUES *out_values, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	aprend_material_slot *slot = nullptr;
	if (const APRESULT lit = aprend_lit_material_slot("aprend_lit_material_get_values", material, &slot, out_error))
		return lit;
	if (!out_values)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_values is NULL");
	aprend_lit_material_block block;
	memcpy(&block, slot->bytes, sizeof(block));
	APREND_LIT_MATERIAL_VALUES &v = *out_values;
	v.base.type = APRI_STRUCT_TYPE_APREND_LIT_MATERIAL_VALUES; // field by field: the caller's padding stays as it was
	v.base.next = nullptr;
	memcpy(v.base_color, block.base_color, sizeof(v.base_color));
	memcpy(v.emission, block.emission, sizeof(v.emission));
	memcpy(v.sheen_color, block.sheen, sizeof(v.sheen_color));
	v.sheen_roughness     = block.sheen[3];
	v.roughness           = block.roughness;
	v.metalness           = block.metalness;
	v.specular            = block.specular;
	v.normal_scale        = block.normal_scale;
	v.occlusion_strength  = block.occlusion_strength;
	v.alpha_cutoff        = block.alpha_cutoff;
	v.clearcoat           = block.clearcoat;
	v.clearcoat_roughness = block.clearcoat_roughness;
	v.transmission         = block.transmission[0];
	v.ior                  = block.transmission[1];
	v.thickness            = block.transmission[2];
	v.attenuation_distance = block.transmission[3];
	memcpy(v.attenuation_color, block.attenuation, sizeof(v.attenuation_color));
	const float *r        = block.uv_transform;
	const float t[6]      = {r[0], r[1], r[2], r[4], r[5], r[6]};
	memcpy(v.uv_transform, t, sizeof(t));
	return APRESULT_OK;
}

APRESULT aprend_lit_material_set_map(aprend_material material, APREND_LIT_MAP map, aprend_texture_view view, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	aprend_material_slot *slot = nullptr;
	if (const APRESULT lit = aprend_lit_material_slot("aprend_lit_material_set_map", material, &slot, out_error))
		return lit;
	if (map > APREND_LIT_MAP_EMISSION)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "%u is not an APREND_LIT_MAP", map);
	if (view && APRI_STALE(view, "aprend_lit_material_set_map", "view"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view is destroyed");
	if (view && (view->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D || view->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view is not a 2D view of color");
	aprend_texture_view given = view ? view : material->instance->builtins.lit.placeholder_2d_view;
	if (const APRESULT set = aprend_material_set_texture(material, s_map_bindings[map], 0, given, out_error))
		return set;
	aprend_lit_material_block block;
	memcpy(&block, slot->bytes, sizeof(block));
	if (view)
		block.maps |= 1u << map;
	else
		block.maps &= ~(1u << map);
	memcpy(slot->bytes, &block, sizeof(block));
	slot->dirty = true;
	return APRESULT_OK;
}

APRESULT aprend_lit_material_set_map_uv_transform(aprend_material material, APREND_LIT_MAP map, const float transform[6], APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	aprend_material_slot *slot = nullptr;
	if (const APRESULT lit = aprend_lit_material_slot("aprend_lit_material_set_map_uv_transform", material, &slot, out_error))
		return lit;
	if (map > APREND_LIT_MAP_EMISSION)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "%u is not an APREND_LIT_MAP", map);
	if (!transform)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "transform is NULL");
	if (!all_finite(transform, 6))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a value of transform is not a finite number");
	aprend_lit_material_block block;
	memcpy(&block, slot->bytes, sizeof(block));
	const float rows[8] = {transform[0], transform[1], transform[2], 0.0f, transform[3], transform[4], transform[5], 0.0f};
	memcpy(block.map_uv_transform[map], rows, sizeof(rows));
	memcpy(slot->bytes, &block, sizeof(block));
	slot->dirty = true;
	return APRESULT_OK;
}

APRESULT aprend_lit_material_get_map_uv_transform(aprend_material material, APREND_LIT_MAP map, float out_transform[6], APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	aprend_material_slot *slot = nullptr;
	if (const APRESULT lit = aprend_lit_material_slot("aprend_lit_material_get_map_uv_transform", material, &slot, out_error))
		return lit;
	if (map > APREND_LIT_MAP_EMISSION)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "%u is not an APREND_LIT_MAP", map);
	if (!out_transform)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_transform is NULL");
	aprend_lit_material_block block;
	memcpy(&block, slot->bytes, sizeof(block));
	const float *r = block.map_uv_transform[map];
	out_transform[0] = r[0];
	out_transform[1] = r[1];
	out_transform[2] = r[2];
	out_transform[3] = r[4];
	out_transform[4] = r[5];
	out_transform[5] = r[6];
	return APRESULT_OK;
}

APRESULT aprend_lit_material_set_sampler(aprend_material material, aprend_sampler sampler, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	aprend_material_slot *slot = nullptr;
	if (const APRESULT lit = aprend_lit_material_slot("aprend_lit_material_set_sampler", material, &slot, out_error))
		return lit;
	if (sampler && APRI_STALE(sampler, "aprend_lit_material_set_sampler", "sampler"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "sampler is destroyed");
	return aprend_material_set_sampler(material, "material_sampler", 0, sampler ? sampler : material->instance->builtins.lit.material_sampler, out_error);
}

} // extern "C"

// ---- Environments -------------------------------------------------------------------

aprend_lit_environment_t::~aprend_lit_environment_t() {
	// Each goes through the release queue on its own.
	aprend_material_destroy(this->sky_material);
	aprend_texture_view_destroy(this->irradiance_cube);
	for (aprend_texture_view view : this->irradiance_faces)
		aprend_texture_view_destroy(view);
	aprend_texture2d_destroy(this->irradiance);
	aprend_texture_view_destroy(this->specular_cube);
	for (aprend_texture_view view : this->specular_faces)
		aprend_texture_view_destroy(view);
	aprend_texture2d_destroy(this->specular);
	aprend_texture_view_destroy(this->radiance_cube);
	for (aprend_texture_view view : this->radiance_faces)
		aprend_texture_view_destroy(view);
	aprend_texture2d_destroy(this->radiance);
}

// A texture of [layers] square layers of [size] and [mips] for Aprend's own
// work, rendered to and sampled; a cube when [layers] is 6.
static aprend_texture2d aprend_lit_work_texture(aprend_instance instance, uint32_t size, uint32_t mips, uint32_t layers, SPUDGPU_FORMAT format) {
	APREND_TEXTURE2D_DESC desc{};
	desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC);
	desc.width        = size;
	desc.height       = size;
	desc.format       = format;
	desc.usage        = APREND_TEXTURE_USAGE_BIT_RENDER_TARGET | APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE;
	desc.mip_levels   = mips;
	desc.array_layers = layers;
	desc.sample_count = 1;
	desc.cube_capable = layers == 6;
	aprend_texture2d texture_made = nullptr;
	(void)aprend_texture2d_create(instance, &desc, &texture_made, nullptr);
	return texture_made;
}

static aprend_texture_view aprend_lit_target_view(aprend_texture2d texture, uint32_t mip, uint32_t layer) {
	APREND_TEXTURE_VIEW_DESC vd{};
	vd.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
	vd.type        = APREND_TEXTURE_VIEW_TYPE_RENDER_TARGET;
	vd.shape       = APREND_TEXTURE_VIEW_SHAPE_2D;
	vd.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
	vd.base_mip    = mip;
	vd.mip_count   = 1;
	vd.base_layer  = layer;
	vd.layer_count = 1;
	aprend_texture_view view_made = nullptr;
	(void)aprend_texture_view_create_2d(texture, &vd, &view_made, nullptr);
	return view_made;
}

static aprend_texture_view aprend_lit_whole_view(aprend_texture2d texture, bool cube) {
	APREND_TEXTURE_VIEW_DESC vd{};
	vd.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
	vd.type        = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
	vd.shape       = cube ? APREND_TEXTURE_VIEW_SHAPE_CUBE : APREND_TEXTURE_VIEW_SHAPE_2D;
	vd.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
	vd.base_mip    = 0;
	vd.mip_count   = APREND_TEXTURE_VIEW_REMAINING;
	vd.base_layer  = 0;
	vd.layer_count = cube ? 6 : 1;
	aprend_texture_view view_made = nullptr;
	(void)aprend_texture_view_create_2d(texture, &vd, &view_made, nullptr);
	return view_made;
}

extern "C" {

APRESULT aprend_lit_environment_create(
    aprend_instance instance,
    const APREND_LIT_ENVIRONMENT_DESC *desc,
    aprend_lit_environment *out_environment,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_environment)
		*out_environment = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_lit_environment_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_lit_environment_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_LIT_ENVIRONMENT_DESC))
		return base;
	if (!power_of_2(desc->specular_size) || desc->specular_size < 16 || desc->specular_size > 2048)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "specular_size %u is not a power of 2 from 16 to 2048", desc->specular_size);
	if (!power_of_2(desc->irradiance_size) || desc->irradiance_size < 8 || desc->irradiance_size > 128)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "irradiance_size %u is not a power of 2 from 8 to 128", desc->irradiance_size);
	if (!out_environment)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_environment is NULL");
	APREND_MALLOC__T(env, aprend_lit_environment_t);
	if (!env)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the environment");
	APREND_CONSTRUCT__T(env, aprend_lit_environment_t);
	env->instance      = instance;
	env->desc          = *desc;
	env->specular_mips = log2_of(desc->specular_size) - 3 + 1; // down to 8
	try {
		env->specular_faces.resize((size_t)env->specular_mips * 6, nullptr);
	} catch (...) {
		APREND_DESTRUCT__T(env, aprend_lit_environment_t);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the environment's views");
	}
	env->radiance   = aprend_lit_work_texture(instance, desc->specular_size, log2_of(desc->specular_size) + 1, 6, APREND_ENVIRONMENT_FORMAT);
	env->specular   = aprend_lit_work_texture(instance, desc->specular_size, env->specular_mips, 6, APREND_ENVIRONMENT_FORMAT);
	env->irradiance = aprend_lit_work_texture(instance, desc->irradiance_size, 1, 6, APREND_ENVIRONMENT_FORMAT);
	bool ok         = env->radiance && env->specular && env->irradiance;
	for (uint32_t face = 0; ok && face < 6; ++face) {
		env->radiance_faces[face]   = aprend_lit_target_view(env->radiance, 0, face);
		env->irradiance_faces[face] = aprend_lit_target_view(env->irradiance, 0, face);
		ok                          = env->radiance_faces[face] && env->irradiance_faces[face];
		for (uint32_t mip = 0; ok && mip < env->specular_mips; ++mip) {
			aprend_texture_view &view = env->specular_faces[(size_t)mip * 6 + face];
			view                      = aprend_lit_target_view(env->specular, mip, face);
			ok                        = view != nullptr;
		}
	}
	if (ok) {
		env->radiance_cube   = aprend_lit_whole_view(env->radiance, true);
		env->specular_cube   = aprend_lit_whole_view(env->specular, true);
		env->irradiance_cube = aprend_lit_whole_view(env->irradiance, true);
		ok                   = env->radiance_cube && env->specular_cube && env->irradiance_cube;
	}
	if (!ok) {
		APREND_DESTRUCT__T(env, aprend_lit_environment_t);
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "a texture or view of the environment can't be created");
	}
	*out_environment = env;
	return APRESULT_OK;
}

void aprend_lit_environment_destroy(aprend_lit_environment environment) {
	if (!environment)
		return;
	if (APRI_STALE(environment, "aprend_lit_environment_destroy", "environment"))
		return;
	// What it holds on the GPU goes through the release queue, each on its
	// own; the rest is the CPU's.
	APREND_DESTRUCT__T(environment, aprend_lit_environment_t);
}

uint32_t aprend_lit_environment_get_specular_mip_count(aprend_lit_environment environment) {
	if (!environment)
		return 0;
	if (APRI_STALE(environment, "aprend_lit_environment_get_specular_mip_count", "environment"))
		return 0;
	return environment->specular_mips;
}

} // extern "C"

aprend_material aprend_lit_environment_sky_material(aprend_lit_environment environment) {
	if (environment->sky_material)
		return environment->sky_material;
	APREND_MATERIAL_DESC desc{};
	desc.base                 = apri_struct_base(APRI_STRUCT_TYPE_APREND_MATERIAL_DESC);
	desc._layout              = environment->instance->builtins.lit.hosek_layout;
	(void)aprend_material_create(environment->instance, &desc, &environment->sky_material, nullptr);
	if (!environment->sky_material)
		aprend_report(environment->instance, APRESULT_APREND_GPU_FAILURE, "LIT_SKY: the material of an environment's Hosek-Wilkie sky can't be made");
	return environment->sky_material;
}

// ---- Hosek and Wilkie's sky -------------------------------------------------------------
//
// The coefficients are Hosek and Wilkie's, version 1.4a, under their
// licence:
//
//   Copyright (c) 2012 - 2013, Lukas Hosek and Alexander Wilkie
//   All rights reserved. Published under the 3-clause BSD license (the
//   full text is at the top of the dataset, aprend_hosek_rgb.inc, which
//   CMakeLists.txt makes from their ArHosekSkyModelData_RGB.h).
//
// The cooking follows their ArHosekSkyModel_CookConfiguration and
// ArHosekSkyModel_CookRadianceConfiguration: each coefficient is a quintic
// Bezier over six control points in the cube root of the sun's elevation,
// mixed between the two turbidities either side and the albedos 0 and 1.

#include "aprend_hosek_rgb.inc"

// The Bezier of control points [points] (each [stride] doubles apart) at
// [e], 0 to 1.
static double aprend_hosek_bezier(const double *points, int stride, double e) {
	const double f    = 1.0 - e;
	const double b[6] = {f * f * f * f * f,         5.0 * f * f * f * f * e, 10.0 * f * f * f * e * e,
	                     10.0 * f * f * e * e * e, 5.0 * f * e * e * e * e, e * e * e * e * e};
	double sum        = 0.0;
	for (int k = 0; k < 6; ++k)
		sum += b[k] * points[k * stride];
	return sum;
}

// One channel's nine coefficients ([dataset]) or radiance ([rad_dataset]),
// cooked: [count] values, each [count] * 6 doubles to an albedo's
// turbidity, 10 turbidities to an albedo.
static void aprend_hosek_cook_channel(const double *dataset, int count, double turbidity, double albedo, double e, double *out) {
	const int lower      = (int)turbidity;
	const double above   = turbidity - (double)lower;
	const int block      = count * 6;
	const double weights[2][2] = {{(1.0 - albedo) * (1.0 - above), (1.0 - albedo) * above}, {albedo * (1.0 - above), albedo * above}};
	for (int i = 0; i < count; ++i) {
		double value = 0.0;
		for (int a = 0; a < 2; ++a) {
			for (int t = 0; t < 2; ++t) {
				// The turbidity above 10 has no data, and no weight there.
				if (lower + t > 10)
					continue;
				const double *points = dataset + block * 10 * a + block * (lower + t - 1) + i;
				value += weights[a][t] * aprend_hosek_bezier(points, count, e);
			}
		}
		out[i] = value;
	}
}

// The extended Perez function of [config] at [theta] and [gamma], as their
// ArHosekSkyModel_GetRadianceInternal has it.
static double aprend_hosek_perez(const double config[9], double theta, double gamma) {
	const double cos_gamma = std::cos(gamma);
	const double exp_m     = std::exp(config[4] * gamma);
	const double ray_m     = cos_gamma * cos_gamma;
	const double mie_m     = (1.0 + cos_gamma * cos_gamma) / std::pow(1.0 + config[8] * config[8] - 2.0 * config[8] * cos_gamma, 1.5);
	const double zenith    = std::sqrt(std::cos(theta));
	return (1.0 + config[0] * std::exp(config[1] / (std::cos(theta) + 0.01))) *
	       (config[2] + config[3] * exp_m + config[5] * ray_m + config[6] * mie_m + config[7] * zenith);
}

void aprend_lit_hosek_cook(const float sun[3], float turbidity, const float albedo[3], float intensity, aprend_hosek_sky_block *out) {
	const double pi        = 3.14159265358979323846;
	const double elevation = std::asin(std::clamp((double)sun[1], 0.0, 1.0));
	const double e         = std::pow(elevation / (pi / 2.0), 1.0 / 3.0);
	double config[3][9];
	double radiance[3];
	for (int c = 0; c < 3; ++c) {
		aprend_hosek_cook_channel(aprend_hosek_datasetsRGB[c], 9, turbidity, albedo[c], e, config[c]);
		aprend_hosek_cook_channel(aprend_hosek_datasetsRGBRad[c], 1, turbidity, albedo[c], e, &radiance[c]);
	}
	// The zenith's luminance (Rec. 709's, the dataset's primaries), at the
	// sun's angle from it.
	const double zenith_gamma = pi / 2.0 - elevation;
	double zenith[3];
	for (int c = 0; c < 3; ++c)
		zenith[c] = aprend_hosek_perez(config[c], 0.0, zenith_gamma) * radiance[c];
	const double luminance = 0.2126 * zenith[0] + 0.7152 * zenith[1] + 0.0722 * zenith[2];
	const double scale     = luminance > 0.0 ? (double)intensity / luminance : 0.0;
	for (int i = 0; i < 9; ++i) {
		for (int c = 0; c < 3; ++c)
			out->config[i][c] = (float)config[c][i];
		out->config[i][3] = 0.0f;
	}
	for (int c = 0; c < 3; ++c)
		out->radiance[c] = (float)(radiance[c] * scale);
	out->radiance[3] = 0.0f;
}

// ---- Scenes -------------------------------------------------------------------------

static const char *const s_directional_masks[APREND_LIT_MAX_DIRECTIONAL_LIGHTS] = {"directional_mask_0", "directional_mask_1", "directional_mask_2",
                                                                                   "directional_mask_3"};

// The shadows as set, into the block: each map shadow's views into the view
// table in record order, a directional light's shadow into its record, a
// point or spot light's into the shadow table it names; and each
// directional light's mask bound at its slot (a placeholder where it has
// none). Every view, name and handle here was checked when it was set, so
// binding a mask can't fail.
static void aprend_lit_scene_resolve_shadows(aprend_lit_scene scene) {
	aprend_lit_scene_block &b = scene->block;
	for (uint32_t i = 0; i < APREND_LIT_MAX_LIGHTS; ++i)
		b.lights[i].outer_shadow[1] = -1.0f;
	aprend_texture_view masks[APREND_LIT_MAX_DIRECTIONAL_LIGHTS];
	for (uint32_t d = 0; d < APREND_LIT_MAX_DIRECTIONAL_LIGHTS; ++d) {
		memset(b.directional[d].shadow_params, 0, sizeof(b.directional[d].shadow_params));
		memset(b.directional[d].shadow_views, 0, sizeof(b.directional[d].shadow_views));
		memset(b.directional[d].splits, 0, sizeof(b.directional[d].splits));
		masks[d] = scene->instance->builtins.lit.placeholder_2d_view;
	}
	uint32_t views   = 0;
	uint32_t shadows = 0;
	for (uint32_t r = 0; r < scene->shadow_count; ++r) {
		const aprend_lit_shadow_record &record = scene->shadows[r];
		const aprend_lit_light_entry entry     = scene->lights[record.light];
		if (record.kind == APREND_LIT_SHADOW_KIND_MASK) {
			aprend_lit_directional_block &d = b.directional[entry.slot];
			d.shadow_views[0]               = APREND_LIT_DIRECTIONAL_MASK;
			d.shadow_params[3]              = record.strength;
			masks[entry.slot]               = record.mask;
			continue;
		}
		const uint32_t first = views;
		for (uint32_t v = 0; v < record.view_count; ++v, ++views) {
			aprend_lit_shadow_view_block &view = b.shadow_views[views];
			memcpy(view.view_projection, record.view_projection[v], sizeof(view.view_projection));
			view.rect[0] = (float)record.rects[v]._x;
			view.rect[1] = (float)record.rects[v]._y;
			view.rect[2] = (float)record.rects[v]._width;
			view.rect[3] = (float)record.rects[v]._height;
		}
		const float params[4] = {record.depth_bias, record.normal_offset, record.softness, record.strength};
		if (record.kind == APREND_LIT_SHADOW_KIND_CASCADES) {
			aprend_lit_directional_block &d = b.directional[entry.slot];
			d.shadow_views[0]               = APREND_LIT_DIRECTIONAL_CASCADES;
			d.shadow_views[1]               = first;
			d.shadow_views[2]               = record.view_count;
			memcpy(d.shadow_params, params, sizeof(params));
			memcpy(d.splits, record.split_depths, sizeof(d.splits));
			continue;
		}
		aprend_lit_shadow_block &shadow = b.shadows[shadows];
		memcpy(shadow.params, params, sizeof(params));
		shadow.views[0]                         = first;
		shadow.views[1]                         = record.view_count;
		shadow.views[2]                         = 0;
		shadow.views[3]                         = 0;
		b.lights[entry.slot].outer_shadow[1] = (float)shadows;
		++shadows;
	}
	for (uint32_t d = 0; d < APREND_LIT_MAX_DIRECTIONAL_LIGHTS; ++d)
		(void)aprend_material_set_texture(scene->material, s_directional_masks[d], 0, masks[d], nullptr);
}

// The block as the scene holds it, into the materials that read it, for the
// next submission that binds them.
static void aprend_lit_scene_commit(aprend_lit_scene scene) {
	aprend_lit_scene_resolve_shadows(scene);
	aprend_lit_write_block(scene->material, "u_lit_scene", &scene->block, sizeof(scene->block));
	aprend_lit_write_block(scene->cull_material, "u_lit_scene", &scene->block, sizeof(scene->block));
}

aprend_binding_set aprend_lit_scene_rt_set(aprend_lit_scene scene, aprend_binding_layout layout) {
	if (scene->rt_set)
		return scene->rt_set;
	// The block's range of the material arena, which is the scene's for its
	// life and copied into by every submission that binds the scene.
	aprend_material_slot *slot = aprend_lit_block_slot(scene->material, "u_lit_scene");
	APREND_BINDING_SET_ENTRY entry{};
	entry._binding                    = 0;
	entry._type                       = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	entry._resource._uniform._buffer  = slot->arena->buffer;
	entry._resource._uniform._offset  = slot->offset;
	entry._resource._uniform._range   = slot->block_size;
	APREND_ERROR error;
	if (aprend_binding_set_create(layout, &entry, 1, &scene->rt_set, &error) != APRESULT_OK) {
		aprend_report(scene->instance, APRESULT_APREND_GPU_FAILURE, "the scene's set for a trace can't be made: %s", error.message);
		return nullptr;
	}
	return scene->rt_set;
}

aprend_binding_set aprend_lit_scene_ssr_set(aprend_lit_scene scene) {
	if (scene->ssr_set)
		return scene->ssr_set;
	// The block's range of the material arena, as aprend_lit_scene_rt_set
	// binds it.
	aprend_material_slot *slot = aprend_lit_block_slot(scene->material, "u_lit_scene");
	APREND_BINDING_SET_ENTRY entry{};
	entry._binding                   = 0;
	entry._type                      = SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	entry._resource._uniform._buffer = slot->arena->buffer;
	entry._resource._uniform._offset = slot->offset;
	entry._resource._uniform._range  = slot->block_size;
	APREND_ERROR error;
	if (aprend_binding_set_create(scene->instance->builtins.lit.ssr_scene_layout, &entry, 1, &scene->ssr_set, &error) != APRESULT_OK) {
		aprend_report(scene->instance, APRESULT_APREND_GPU_FAILURE, "the scene's set for screen-space reflections can't be made: %s", error.message);
		return nullptr;
	}
	return scene->ssr_set;
}

bool aprend_lit_scene_depth_reversed(aprend_lit_scene scene) {
	// Depth 0 and depth 1 at the middle of the view brought back through
	// the inverse: the one nearer the eye is near. A point at infinity (w
	// 0, as a reversed infinite projection's depth 0) is the far one.
	const float *m   = scene->block.inverse_view_projection;
	const float *eye = scene->block.eye_position;
	float distance[2] = {0.0f, 0.0f};
	for (int z = 0; z < 2; ++z) {
		// Column-major: element (row, column) at column * 4 + row.
		const float h[4] = {m[8] * (float)z + m[12], m[9] * (float)z + m[13], m[10] * (float)z + m[14], m[11] * (float)z + m[15]};
		if (std::fabs(h[3]) < 1e-30f) {
			distance[z] = INFINITY;
			continue;
		}
		const float d[3] = {h[0] / h[3] - eye[0], h[1] / h[3] - eye[1], h[2] / h[3] - eye[2]};
		distance[z]      = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	}
	return distance[1] < distance[0];
}

uint32_t aprend_lit_scene_directional_slot(aprend_lit_scene scene, uint32_t light) {
	if (light >= scene->light_count)
		return UINT32_MAX;
	if (scene->lights[light].type != APREND_LIT_LIGHT_TYPE_DIRECTIONAL)
		return UINT32_MAX;
	return scene->lights[light].slot;
}

static void aprend_lit_scene_free(aprend_lit_scene scene) {
	aprend_binding_set_destroy(scene->rt_set);
	aprend_binding_set_destroy(scene->ssr_set);
	if (scene->material)
		scene->material->lit_scene = nullptr;
	aprend_material_destroy(scene->material);
	for (aprend_material material : scene->shadow_pass_materials)
		aprend_material_destroy(material);
	aprend_material_destroy(scene->cull_material);
	aprend_material_destroy(scene->occlusion_material);
	aprend_buffer_destroy(scene->tiles);
	APREND_DESTRUCT__T(scene, aprend_lit_scene_t);
}

// The record of [light]'s shadow in [scene], or NULL if it has none.
static aprend_lit_shadow_record *aprend_lit_scene_find_shadow(aprend_lit_scene scene, uint32_t light) {
	for (uint32_t r = 0; r < scene->shadow_count; ++r)
		if (scene->shadows[r].light == light)
			return &scene->shadows[r];
	return nullptr;
}

// Takes record [r] of [scene]'s shadows away: its pass materials go back to
// the free ones, and the records after it move down, keeping their order.
static void aprend_lit_scene_remove_shadow(aprend_lit_scene scene, uint32_t r) {
	const aprend_lit_shadow_record &record = scene->shadows[r];
	for (uint32_t v = 0; v < record.view_count; ++v)
		scene->free_pass_materials[scene->free_pass_count++] = record.materials[v];
	for (uint32_t i = r + 1; i < scene->shadow_count; ++i)
		scene->shadows[i - 1] = scene->shadows[i];
	--scene->shadow_count;
}

// Whether a shadow of [kind] fits a light of [type].
static bool aprend_lit_kind_fits(APREND_LIT_SHADOW_KIND kind, uint32_t type) {
	switch (kind) {
	case APREND_LIT_SHADOW_KIND_MASK:
	case APREND_LIT_SHADOW_KIND_CASCADES:
		return type == APREND_LIT_LIGHT_TYPE_DIRECTIONAL;
	case APREND_LIT_SHADOW_KIND_SPOT_MAP:
		return type == APREND_LIT_LIGHT_TYPE_SPOT;
	case APREND_LIT_SHADOW_KIND_POINT_MAP:
		return type == APREND_LIT_LIGHT_TYPE_POINT;
	default:
		return false;
	}
}

extern "C" {

APRESULT aprend_lit_scene_create(aprend_instance instance, aprend_lit_scene *out_scene, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_scene)
		*out_scene = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_lit_scene_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_lit_scene_create");
	if (!out_scene)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_scene is NULL");
	aprend_binding_layout layout        = nullptr;
	aprend_binding_layout shadow_layout = nullptr;
	if (const APRESULT made = aprend_lit_get_scene_layout(instance, &layout, out_error))
		return made;
	if (const APRESULT made = aprend_lit_get_shadow_layout(instance, &shadow_layout, out_error))
		return made;
	if (!aprend_lit_prepare_resources(instance))
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "Aprend's lit resources can't be made");
	if (!aprend_lit_prepare_brdf_lut(instance))
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "Aprend's lit BRDF table can't be made");
	if (!aprend_lit_cull_pipeline(instance))
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "Aprend's light culling pipeline can't be made");
	if (!aprend_lit_prepare_passes(instance))
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "Aprend's lit passes can't be made");
	APREND_MALLOC__T(scene, aprend_lit_scene_t);
	if (!scene)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the scene");
	APREND_CONSTRUCT__T(scene, aprend_lit_scene_t);
	scene->instance                = instance;
	const aprend_lit_builtins &lit = instance->builtins.lit;
	APREND_MATERIAL_DESC desc{};
	desc.base = apri_struct_base(APRI_STRUCT_TYPE_APREND_MATERIAL_DESC);
	desc._layout = layout;
	(void)aprend_material_create(instance, &desc, &scene->material, nullptr);
	desc._layout = shadow_layout;
	for (aprend_material &material : scene->shadow_pass_materials)
		(void)aprend_material_create(instance, &desc, &material, nullptr);
	desc._layout = lit.cull_layout;
	(void)aprend_material_create(instance, &desc, &scene->cull_material, nullptr);
	desc._layout = lit.occlusion_layout;
	(void)aprend_material_create(instance, &desc, &scene->occlusion_material, nullptr);

	bool ok = scene->material && scene->cull_material && scene->occlusion_material;
	for (aprend_material material : scene->shadow_pass_materials)
		ok = ok && material;
	aprend_material m = scene->material;
	ok                = ok && aprend_material_set_storage_buffer(m, "AprendLitTiles", 0, lit.placeholder_tiles, nullptr) == APRESULT_OK;
	ok                = ok && aprend_material_set_texture(m, "shadow_atlas", 0, lit.placeholder_depth_view, nullptr) == APRESULT_OK;
	ok                = ok && aprend_material_set_sampler(m, "shadow_sampler", 0, lit.shadow_sampler, nullptr) == APRESULT_OK;
	ok                = ok && aprend_material_set_sampler(m, "shadow_sampler_reversed", 0, lit.shadow_sampler_reversed, nullptr) == APRESULT_OK;
	ok                = ok && aprend_material_set_texture(m, "environment_specular", 0, lit.placeholder_cube_view, nullptr) == APRESULT_OK;
	ok                = ok && aprend_material_set_texture(m, "environment_irradiance", 0, lit.placeholder_cube_view, nullptr) == APRESULT_OK;
	ok                = ok && aprend_material_set_texture(m, "brdf_lut", 0, lit.brdf_lut_view, nullptr) == APRESULT_OK;
	ok                = ok && aprend_material_set_sampler(m, "environment_sampler", 0, lit.environment_sampler, nullptr) == APRESULT_OK;
	ok                = ok && aprend_material_set_texture(m, "ambient_occlusion", 0, lit.placeholder_2d_view, nullptr) == APRESULT_OK;
	ok                = ok && aprend_material_set_texture(m, "scene_color", 0, lit.placeholder_2d_view, nullptr) == APRESULT_OK;
	for (const char *mask : s_directional_masks)
		ok = ok && aprend_material_set_texture(m, mask, 0, lit.placeholder_2d_view, nullptr) == APRESULT_OK;
	ok = ok && aprend_material_set_storage_buffer(scene->cull_material, "AprendLitTiles", 0, lit.placeholder_tiles, nullptr) == APRESULT_OK;
	ok = ok && aprend_lit_block_slot(m, "u_lit_scene") && aprend_lit_block_slot(scene->cull_material, "u_lit_scene");
	if (!ok) {
		aprend_lit_scene_free(scene);
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "the scene's materials can't be made");
	}
	// Every pass material free, handed out from the end down so the first
	// shadow set takes material 0.
	for (uint32_t i = 0; i < APREND_LIT_MAX_SHADOW_VIEWS; ++i)
		scene->free_pass_materials[i] = (uint8_t)(APREND_LIT_MAX_SHADOW_VIEWS - 1 - i);
	scene->free_pass_count       = APREND_LIT_MAX_SHADOW_VIEWS;
	scene->material->lit_scene   = scene;
	scene->block.exposure        = 1.0f;
	scene->block.output_encoding = APREND_COLOR_ENCODING_LINEAR;
	aprend_lit_scene_commit(scene);
	*out_scene = scene;
	return APRESULT_OK;
}

void aprend_lit_scene_destroy(aprend_lit_scene scene) {
	if (!scene)
		return;
	if (APRI_STALE(scene, "aprend_lit_scene_destroy", "scene"))
		return;
	aprend_lit_scene_free(scene);
}

aprend_material aprend_lit_scene_get_material(aprend_lit_scene scene) {
	if (!scene)
		return nullptr;
	if (APRI_STALE(scene, "aprend_lit_scene_get_material", "scene"))
		return nullptr;
	return scene->material;
}

aprend_material aprend_lit_scene_get_shadow_pass_material(aprend_lit_scene scene, uint32_t light, uint32_t view) {
	if (!scene)
		return nullptr;
	if (APRI_STALE(scene, "aprend_lit_scene_get_shadow_pass_material", "scene"))
		return nullptr;
	const aprend_lit_shadow_record *record = aprend_lit_scene_find_shadow(scene, light);
	if (!record)
		return nullptr;
	if (view >= record->view_count)
		return nullptr;
	return scene->shadow_pass_materials[record->materials[view]];
}

APRESULT aprend_lit_scene_set_view(aprend_lit_scene scene, const APREND_LIT_VIEW *view, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!scene)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is NULL");
	if (APRI_STALE(scene, "aprend_lit_scene_set_view", "scene"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is destroyed");
	if (!view)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view is NULL");
	APREND_ENTER(scene->instance, "aprend_lit_scene_set_view");
	if (const APRESULT base = aprend_base_check(out_error, "view", &view->base, APRI_STRUCT_TYPE_APREND_LIT_VIEW))
		return base;
	const APREND_LIT_VIEW &v = *view;
	if (!all_finite(v.eye_position, 3) || !all_finite(v.view_direction, 3) || !all_finite(v.view_projection, 16) || !finite(v.exposure) ||
	    v.exposure < 0.0f) {
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a value is not a finite number, or the exposure is negative");
	}
	if (v.output_encoding > APREND_COLOR_ENCODING_SRGB)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "%u is not an APREND_COLOR_ENCODING", v.output_encoding);
	if ((v.target_width == 0) != (v.target_height == 0) || v.target_width > 16384 || v.target_height > 16384)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a target of %u by %u: both sides 0, or both from 1 to 16384", v.target_width, v.target_height);
	glm::mat4 vp;
	memcpy(&vp, v.view_projection, sizeof(vp));
	bool projected = false;
	for (int i = 0; i < 16 && !projected; ++i)
		projected = v.view_projection[i] != 0.0f;

	// The tiles, for a target with a projection: a buffer of the right
	// size, made again when the number of tiles changes.
	const bool tiled               = projected && v.target_width > 0;
	const uint32_t tiles_x         = tiled ? (v.target_width + APREND_LIT_TILE_SIZE - 1) / APREND_LIT_TILE_SIZE : 0;
	const uint32_t tiles_y         = tiled ? (v.target_height + APREND_LIT_TILE_SIZE - 1) / APREND_LIT_TILE_SIZE : 0;
	aprend_lit_scene_block &b      = scene->block;
	const aprend_lit_builtins &lit = scene->instance->builtins.lit;
	if (tiles_x != b.tiles_x || tiles_y != b.tiles_y || (tiled && !scene->tiles)) {
		aprend_buffer tiles = nullptr;
		if (tiled) {
			const uint64_t size = (uint64_t)tiles_x * tiles_y * (APREND_LIT_MAX_TILE_LIGHTS + 1) * sizeof(uint32_t);
			const APRESULT made = aprend_lit_storage_buffer(scene->instance, size, true, "aprend lit tiles", &tiles, out_error);
			if (made != APRESULT_OK)
				return made;
		}
		aprend_buffer buffer = tiles ? tiles : lit.placeholder_tiles;
		APRESULT set         = aprend_material_set_storage_buffer(scene->material, "AprendLitTiles", 0, buffer, out_error);
		if (set == APRESULT_OK)
			set = aprend_material_set_storage_buffer(scene->cull_material, "AprendLitTiles", 0, buffer, out_error);
		if (set != APRESULT_OK) {
			aprend_buffer_destroy(tiles);
			return set;
		}
		aprend_buffer_destroy(scene->tiles);
		scene->tiles = tiles;
	}
	memcpy(b.eye_position, v.eye_position, sizeof(float) * 3);
	b.eye_position[3] = 1.0f;
	unit3(v.view_direction, b.view_direction);
	memcpy(b.view_projection, v.view_projection, sizeof(b.view_projection));
	// A view_projection that can't be inverted leaves rays and positions
	// at 0: nothing that reads them is right, and nothing is undefined.
	const glm::mat4 inverse = projected && std::fabs(glm::determinant(vp)) > 1e-30f ? glm::inverse(vp) : glm::mat4(0.0f);
	memcpy(b.inverse_view_projection, &inverse, sizeof(b.inverse_view_projection));
	b.target_size[0]  = (float)v.target_width;
	b.target_size[1]  = (float)v.target_height;
	b.target_size[2]  = v.target_width ? 1.0f / (float)v.target_width : 0.0f;
	b.target_size[3]  = v.target_height ? 1.0f / (float)v.target_height : 0.0f;
	b.exposure        = v.exposure;
	b.output_encoding = v.output_encoding;
	b.tile_size       = APREND_LIT_TILE_SIZE;
	b.tiles_x         = tiles_x;
	b.tiles_y         = tiles_y;
	if (tiled)
		b.flags |= APREND_LIT_SCENE_FLAG_TILES;
	else
		b.flags &= ~APREND_LIT_SCENE_FLAG_TILES;
	aprend_lit_scene_commit(scene);
	return APRESULT_OK;
}

} // extern "C"

// Whether any of [v]'s three is below 0.
static bool any_negative3(const float v[3]) {
	bool negative = false;
	for (int i = 0; i < 3; ++i)
		negative = negative || v[i] < 0.0f;
	return negative;
}

// Whether [l] is a light aprend_lit_scene_set_lights() takes; [error] names
// it by its [index] if not.
static APRESULT aprend_lit_light_valid(const APREND_LIT_SCENE_LIGHT &l, uint32_t index, APREND_ERROR *error) {
	const float half_pi = 1.57079632679f;
	if (l._type > APREND_LIT_LIGHT_TYPE_DIRECTIONAL)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u has type %u, which is not an APREND_LIT_LIGHT_TYPE", index, l._type);
	if (!all_finite(l._position, 3))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u's _position is not finite", index);
	if (!all_finite(l._direction, 3))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u's _direction is not finite", index);
	if (!all_finite(l._radiance, 3))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u's _radiance is not finite", index);
	if (!finite(l._range))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u's _range is not finite", index);
	if (!finite(l._inner_angle))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u's _inner_angle is not finite", index);
	if (!finite(l._outer_angle))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u's _outer_angle is not finite", index);
	if (!finite(l._radius))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u's _radius is not finite", index);
	if (any_negative3(l._radiance))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u has a negative _radiance", index);
	if (l._type != APREND_LIT_LIGHT_TYPE_POINT && length3(l._direction) == 0.0f)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u, a spot or directional light, has a _direction of 0", index);
	if (l._type != APREND_LIT_LIGHT_TYPE_DIRECTIONAL && !(l._range > 0.0f))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u, a point or spot light, has a _range that is not above 0", index);
	if (l._type == APREND_LIT_LIGHT_TYPE_SPOT && l._inner_angle < 0.0f)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "spot light %u's _inner_angle is negative", index);
	if (l._type == APREND_LIT_LIGHT_TYPE_SPOT && l._inner_angle > l._outer_angle)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "spot light %u's _inner_angle is past its _outer_angle", index);
	if (l._type == APREND_LIT_LIGHT_TYPE_SPOT && l._outer_angle > half_pi)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "spot light %u's _outer_angle is past pi / 2", index);
	if (l._type == APREND_LIT_LIGHT_TYPE_DIRECTIONAL && l._radius < 0.0f)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "directional light %u's _radius is negative", index);
	if (l._type == APREND_LIT_LIGHT_TYPE_DIRECTIONAL && l._radius > 0.5f)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "directional light %u's _radius %g is above 0.5", index, (double)l._radius);
	if (l._type != APREND_LIT_LIGHT_TYPE_DIRECTIONAL && l._radius != 0.0f)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u, a point or spot light, has a _radius that is not 0 (card A38)", index);
	if (l._type != APREND_LIT_LIGHT_TYPE_DIRECTIONAL && l._sky_disc)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "light %u, a point or spot light, has _sky_disc set", index);
	return APRESULT_OK;
}

// Whether every rectangle of [maps]' first [count] views lies inside an
// atlas of [width] by [height]: their index, or UINT32_MAX if they do.
static uint32_t aprend_lit_rect_outside(const APREND_LIT_SHADOW_RECT *rects, uint32_t count, uint32_t width, uint32_t height) {
	for (uint32_t v = 0; v < count; ++v) {
		const APREND_LIT_SHADOW_RECT &r = rects[v];
		if ((uint64_t)r._x + r._width > width || (uint64_t)r._y + r._height > height)
			return v;
	}
	return UINT32_MAX;
}

extern "C" {

APRESULT aprend_lit_scene_set_lights(aprend_lit_scene scene, const APREND_LIT_SCENE_LIGHT *lights, uint32_t count, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!scene)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is NULL");
	if (APRI_STALE(scene, "aprend_lit_scene_set_lights", "scene"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is destroyed");
	APREND_ENTER(scene->instance, "aprend_lit_scene_set_lights");
	if (count > 0 && !lights)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "lights is NULL with a count of %u", count);
	uint32_t point_or_spot = 0;
	uint32_t directional   = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (const APRESULT valid = aprend_lit_light_valid(lights[i], i, out_error))
			return valid;
		if (lights[i]._type == APREND_LIT_LIGHT_TYPE_DIRECTIONAL)
			++directional;
		else
			++point_or_spot;
	}
	if (point_or_spot > APREND_LIT_MAX_LIGHTS)
		return aprend_fail(out_error, APRESULT_LIMIT, "%u point and spot lights, and a scene holds %u", point_or_spot, APREND_LIT_MAX_LIGHTS);
	if (directional > APREND_LIT_MAX_DIRECTIONAL_LIGHTS)
		return aprend_fail(out_error, APRESULT_LIMIT, "%u directional lights, and a scene holds %u", directional, APREND_LIT_MAX_DIRECTIONAL_LIGHTS);
	aprend_lit_scene_block &b = scene->block;
	memset(b.lights, 0, sizeof(b.lights));
	for (aprend_lit_directional_block &d : b.directional) {
		memset(d.direction_versine, 0, sizeof(d.direction_versine));
		memset(d.radiance_disc, 0, sizeof(d.radiance_disc));
	}
	uint32_t slots[2] = {0, 0};
	for (uint32_t i = 0; i < count; ++i) {
		const APREND_LIT_SCENE_LIGHT &l = lights[i];
		const bool is_directional       = l._type == APREND_LIT_LIGHT_TYPE_DIRECTIONAL;
		const uint32_t slot             = slots[is_directional ? 1 : 0]++;
		scene->lights[i].type           = (uint8_t)l._type;
		scene->lights[i].slot           = (uint8_t)slot;
		if (is_directional) {
			aprend_lit_directional_block &d = b.directional[slot];
			unit3(l._direction, d.direction_versine);
			// 1 - cos of the radius: 0 for none. Worked out in double, since
			// for the earth's sun it is 1e-5.
			d.direction_versine[3] = (float)(1.0 - std::cos((double)l._radius));
			memcpy(d.radiance_disc, l._radiance, sizeof(float) * 3);
			d.radiance_disc[3] = l._sky_disc ? 1.0f : 0.0f;
			continue;
		}
		aprend_lit_light_block &o = b.lights[slot];
		memcpy(o.position_range, l._position, sizeof(float) * 3);
		o.position_range[3] = l._range;
		memcpy(o.radiance_type, l._radiance, sizeof(float) * 3);
		o.radiance_type[3] = l._type == APREND_LIT_LIGHT_TYPE_SPOT ? 1.0f : 0.0f;
		unit3(l._direction, o.direction_inner);
		o.direction_inner[3] = std::cos(l._inner_angle);
		o.outer_shadow[0]    = std::cos(l._outer_angle);
	}
	scene->light_count  = count;
	b.light_count       = point_or_spot;
	b.directional_count = directional;
	// Each shadow whose index still names a light of a type its kind fits
	// stays; the rest go.
	for (uint32_t r = scene->shadow_count; r-- > 0;) {
		const aprend_lit_shadow_record &record = scene->shadows[r];
		if (record.light >= count || !aprend_lit_kind_fits(record.kind, lights[record.light]._type))
			aprend_lit_scene_remove_shadow(scene, r);
	}
	aprend_lit_scene_commit(scene);
	return APRESULT_OK;
}

APRESULT aprend_lit_scene_set_shadow_atlas(aprend_lit_scene scene, aprend_texture_view atlas, bool reversed_depth, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!scene)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is NULL");
	if (APRI_STALE(scene, "aprend_lit_scene_set_shadow_atlas", "scene"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is destroyed");
	APREND_ENTER(scene->instance, "aprend_lit_scene_set_shadow_atlas");
	if (atlas && APRI_STALE(atlas, "aprend_lit_scene_set_shadow_atlas", "atlas"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is destroyed");
	if (atlas && atlas->instance != scene->instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is of another instance");
	if (atlas && atlas->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is not a SHADER_RESOURCE view");
	if (atlas && atlas->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is not a 2D view");
	if (atlas && atlas->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_DEPTH)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is not a view of depth");
	const uint32_t width  = atlas ? aprend_mip_size(atlas->texture._t2d->desc.width, atlas->desc.base_mip) : 0;
	const uint32_t height = atlas ? aprend_mip_size(atlas->texture._t2d->desc.height, atlas->desc.base_mip) : 0;
	for (uint32_t r = 0; atlas && r < scene->shadow_count; ++r) {
		const aprend_lit_shadow_record &record = scene->shadows[r];
		const uint32_t outside                 = aprend_lit_rect_outside(record.rects, record.view_count, width, height);
		if (outside != UINT32_MAX)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "light %u's shadow has view %u's rectangle past an atlas of %u by %u", record.light,
			                   outside, width, height);
	}
	const aprend_lit_builtins &lit = scene->instance->builtins.lit;
	if (const APRESULT set = aprend_material_set_texture(scene->material, "shadow_atlas", 0, atlas ? atlas : lit.placeholder_depth_view, out_error))
		return set;
	scene->atlas        = atlas;
	scene->atlas_width  = width;
	scene->atlas_height = height;
	aprend_lit_scene_block &b = scene->block;
	b.shadow_atlas_size[0]    = (float)width;
	b.shadow_atlas_size[1]    = (float)height;
	b.shadow_atlas_size[2]    = width ? 1.0f / (float)width : 0.0f;
	b.shadow_atlas_size[3]    = height ? 1.0f / (float)height : 0.0f;
	if (atlas)
		b.flags |= APREND_LIT_SCENE_FLAG_SHADOW_ATLAS;
	else
		b.flags &= ~APREND_LIT_SCENE_FLAG_SHADOW_ATLAS;
	if (reversed_depth)
		b.flags |= APREND_LIT_SCENE_FLAG_SHADOW_REVERSED;
	else
		b.flags &= ~APREND_LIT_SCENE_FLAG_SHADOW_REVERSED;
	aprend_lit_scene_commit(scene);
	return APRESULT_OK;
}

APRESULT aprend_lit_scene_set_light_shadow(aprend_lit_scene scene, uint32_t light, const APREND_LIT_LIGHT_SHADOW *shadow,
                                           APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!scene)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is NULL");
	if (APRI_STALE(scene, "aprend_lit_scene_set_light_shadow", "scene"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is destroyed");
	APREND_ENTER(scene->instance, "aprend_lit_scene_set_light_shadow");
	if (light >= scene->light_count)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "light %u names no light of the scene, which has %u", light, scene->light_count);
	aprend_lit_shadow_record *existing = aprend_lit_scene_find_shadow(scene, light);
	if (!shadow) {
		if (existing)
			aprend_lit_scene_remove_shadow(scene, (uint32_t)(existing - scene->shadows));
		aprend_lit_scene_commit(scene);
		return APRESULT_OK;
	}
	// The shadow's own type; what is chained on is its kind's, checked below.
	if (shadow->base.type != APRI_STRUCT_TYPE_APREND_LIT_LIGHT_SHADOW)
		return aprend_fail(out_error, APRESULT_STRUCT_WRONG_TYPE, "shadow %s", apri_struct_check_message(APRESULT_STRUCT_WRONG_TYPE));
	const APREND_LIT_LIGHT_SHADOW &s = *shadow;
	const uint32_t type              = scene->lights[light].type;
	if (s._kind == APREND_LIT_SHADOW_KIND_NONE)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_kind is APREND_LIT_SHADOW_KIND_NONE");
	if (s._kind > APREND_LIT_SHADOW_KIND_POINT_MAP)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_kind %u is not an APREND_LIT_SHADOW_KIND", (unsigned)s._kind);
	if (!aprend_lit_kind_fits(s._kind, type))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_kind %u is not a kind light %u, of type %u, can have", (unsigned)s._kind, light, type);
	if (!finite(s._strength))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_strength is not finite");
	if (s._strength < 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_strength is below 0");
	if (s._strength > 1.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_strength is above 1");
	const uint32_t held = existing ? existing->view_count : 0;
	if (s._kind == APREND_LIT_SHADOW_KIND_MASK) {
		if (s.base.next)
			return aprend_fail(out_error, APRESULT_STRUCT_UNKNOWN_IN_CHAIN, "shadow is a mask, and nothing is chained onto a mask");
		if (!s._mask)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_mask is NULL");
		if (APRI_STALE(s._mask, "aprend_lit_scene_set_light_shadow", "shadow->_mask"))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_mask is destroyed");
		if (s._mask->instance != scene->instance)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_mask is of another instance");
		if (s._mask->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_mask is not a SHADER_RESOURCE view");
		if (s._mask->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_mask is not a 2D view");
		if (s._mask->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow->_mask is not a view of color");
		aprend_lit_shadow_record *record = existing;
		if (!record) {
			record = &scene->shadows[scene->shadow_count++];
		} else {
			for (uint32_t v = 0; v < record->view_count; ++v)
				scene->free_pass_materials[scene->free_pass_count++] = record->materials[v];
		}
		*record          = aprend_lit_shadow_record{};
		record->light    = light;
		record->kind     = s._kind;
		record->strength = s._strength;
		record->mask     = s._mask;
		aprend_lit_scene_commit(scene);
		return APRESULT_OK;
	}
	if (!s.base.next)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "shadow is of a map kind, and has no APREND_LIT_SHADOW_MAPS chained on");
	const APREND_LIT_SHADOW_MAPS &maps = *(const APREND_LIT_SHADOW_MAPS *)s.base.next;
	if (const APRESULT base = aprend_base_check(out_error, "shadow's chained APREND_LIT_SHADOW_MAPS", &maps.base, APRI_STRUCT_TYPE_APREND_LIT_SHADOW_MAPS))
		return base;
	const uint32_t wanted = s._kind == APREND_LIT_SHADOW_KIND_POINT_MAP ? 6u : 1u;
	if (s._kind == APREND_LIT_SHADOW_KIND_CASCADES && (maps._view_count == 0 || maps._view_count > APREND_LIT_MAX_CASCADES))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_view_count %u: cascades are 1 to %u views", maps._view_count, APREND_LIT_MAX_CASCADES);
	if (s._kind != APREND_LIT_SHADOW_KIND_CASCADES && maps._view_count != wanted)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_view_count %u: a %s is %u", maps._view_count,
		                   s._kind == APREND_LIT_SHADOW_KIND_POINT_MAP ? "point light's map" : "spot's map", wanted);
	const uint32_t views = maps._view_count;
	if (!all_finite(&maps._view_projection[0][0], (size_t)views * 16))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a value of _view_projection is not finite");
	if (s._kind == APREND_LIT_SHADOW_KIND_CASCADES && !all_finite(maps._split_depths, views))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a value of _split_depths is not finite");
	if (!finite(maps._depth_bias))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_depth_bias is not finite");
	if (!finite(maps._normal_offset))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_normal_offset is not finite");
	if (!finite(maps._softness))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_softness is not finite");
	for (uint32_t v = 0; v < views; ++v) {
		if (maps._rects[v]._width == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view %u's rectangle has a _width of 0", v);
		if (maps._rects[v]._height == 0)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view %u's rectangle has a _height of 0", v);
	}
	if (!scene->atlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the scene has no shadow atlas (aprend_lit_scene_set_shadow_atlas()) for the rectangles to be in");
	const uint32_t outside = aprend_lit_rect_outside(maps._rects, views, scene->atlas_width, scene->atlas_height);
	if (outside != UINT32_MAX)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view %u's rectangle is past the atlas, %u by %u", outside, scene->atlas_width, scene->atlas_height);
	for (uint32_t c = 0; s._kind == APREND_LIT_SHADOW_KIND_CASCADES && c < views; ++c) {
		if (!(maps._split_depths[c] > 0.0f))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "cascade %u's split is not above 0", c);
		if (c > 0 && !(maps._split_depths[c] > maps._split_depths[c - 1]))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "cascade %u's split is not above the one before", c);
	}
	if (maps._softness < 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_softness is below 0");
	if (maps._softness > 4.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_softness is above 4");
	if (maps._depth_bias < 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_depth_bias is negative");
	if (maps._normal_offset < 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_normal_offset is negative");
	if (views > held && views - held > scene->free_pass_count)
		return aprend_fail(out_error, APRESULT_LIMIT, "the scene's shadows would draw %u views, and a scene draws %u",
		                   APREND_LIT_MAX_SHADOW_VIEWS - scene->free_pass_count - held + views, APREND_LIT_MAX_SHADOW_VIEWS);

	aprend_lit_shadow_record *record = existing;
	uint8_t materials[6]             = {};
	if (record)
		memcpy(materials, record->materials, sizeof(materials));
	else
		record = &scene->shadows[scene->shadow_count++];
	// The views it held keep their materials, so one taken for a view before
	// holds the new matrix; more come from the free ones, and those past the
	// new count go back.
	for (uint32_t v = held; v < views; ++v)
		materials[v] = scene->free_pass_materials[--scene->free_pass_count];
	for (uint32_t v = views; v < held; ++v)
		scene->free_pass_materials[scene->free_pass_count++] = materials[v];
	*record               = aprend_lit_shadow_record{};
	record->light         = light;
	record->kind          = s._kind;
	record->strength      = s._strength;
	record->view_count    = views;
	record->depth_bias    = maps._depth_bias;
	record->normal_offset = maps._normal_offset;
	record->softness      = maps._softness;
	memcpy(record->materials, materials, sizeof(materials));
	memcpy(record->view_projection, maps._view_projection, sizeof(float) * 16 * views);
	memcpy(record->rects, maps._rects, sizeof(APREND_LIT_SHADOW_RECT) * views);
	if (s._kind == APREND_LIT_SHADOW_KIND_CASCADES)
		memcpy(record->split_depths, maps._split_depths, sizeof(float) * views);
	for (uint32_t v = 0; v < views; ++v) {
		aprend_lit_shadow_pass_block pass{};
		memcpy(pass.view_projection, record->view_projection[v], sizeof(pass.view_projection));
		aprend_lit_write_block(scene->shadow_pass_materials[materials[v]], "u_lit_shadow_pass", &pass, sizeof(pass));
	}
	aprend_lit_scene_commit(scene);
	return APRESULT_OK;
}

APRESULT aprend_lit_scene_set_ambient(aprend_lit_scene scene, const APREND_LIT_AMBIENT *ambient, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!scene)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is NULL");
	if (APRI_STALE(scene, "aprend_lit_scene_set_ambient", "scene"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is destroyed");
	if (!ambient)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "ambient is NULL");
	APREND_ENTER(scene->instance, "aprend_lit_scene_set_ambient");
	if (const APRESULT base = aprend_base_check(out_error, "ambient", &ambient->base, APRI_STRUCT_TYPE_APREND_LIT_AMBIENT))
		return base;
	const APREND_LIT_AMBIENT &a = *ambient;
	if (!all_finite(a.up, 3) || !all_finite(a.sky, 3) || !all_finite(a.ground, 3) || !finite(a.environment_intensity) ||
	    !all_finite(a.environment_rotation, 9)) {
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a value is not a finite number");
	}
	bool colored = false;
	for (int i = 0; i < 3; ++i) {
		if (a.sky[i] < 0.0f || a.ground[i] < 0.0f)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a color is negative");
		colored = colored || a.sky[i] > 0.0f || a.ground[i] > 0.0f;
	}
	if (a.environment_intensity < 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "environment_intensity is negative");
	if (colored && length3(a.up) == 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the hemisphere has colors and no up");
	if (a.environment && a.environment->instance != scene->instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "environment is of another instance");
	const aprend_lit_builtins &lit = scene->instance->builtins.lit;
	aprend_texture_view specular   = a.environment ? a.environment->specular_cube : lit.placeholder_cube_view;
	aprend_texture_view irradiance = a.environment ? a.environment->irradiance_cube : lit.placeholder_cube_view;
	if (const APRESULT set = aprend_material_set_texture(scene->material, "environment_specular", 0, specular, out_error))
		return set;
	if (const APRESULT set = aprend_material_set_texture(scene->material, "environment_irradiance", 0, irradiance, out_error))
		return set;
	scene->environment        = a.environment;
	aprend_lit_scene_block &b = scene->block;
	unit3(a.up, b.ambient_up);
	memcpy(b.ambient_sky, a.sky, sizeof(float) * 3);
	b.ambient_sky[3] = 0.0f;
	memcpy(b.ambient_ground, a.ground, sizeof(float) * 3);
	b.ambient_ground[3] = 0.0f;
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column)
			b.environment_rotation[row * 4 + column] = a.environment_rotation[row * 3 + column];
		b.environment_rotation[row * 4 + 3] = 0.0f;
	}
	b.environment_intensity = a.environment_intensity;
	b.environment_mips      = a.environment ? (float)a.environment->specular_mips : 0.0f;
	if (a.environment)
		b.flags |= APREND_LIT_SCENE_FLAG_ENVIRONMENT;
	else
		b.flags &= ~APREND_LIT_SCENE_FLAG_ENVIRONMENT;
	aprend_lit_scene_commit(scene);
	return APRESULT_OK;
}

APRESULT aprend_lit_scene_set_ambient_occlusion(aprend_lit_scene scene, aprend_texture_view occlusion, float strength, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!scene)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is NULL");
	if (APRI_STALE(scene, "aprend_lit_scene_set_ambient_occlusion", "scene"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is destroyed");
	APREND_ENTER(scene->instance, "aprend_lit_scene_set_ambient_occlusion");
	if (!finite(strength) || strength < 0.0f || strength > 1.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "strength is not from 0 to 1");
	if (occlusion && APRI_STALE(occlusion, "aprend_lit_scene_set_ambient_occlusion", "occlusion"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "occlusion is destroyed");
	if (occlusion && (occlusion->instance != scene->instance || occlusion->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE ||
	                  occlusion->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D || occlusion->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR)) {
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "occlusion is not a SHADER_RESOURCE 2D view of color of the scene's instance");
	}
	if (const APRESULT set = aprend_material_set_texture(scene->material, "ambient_occlusion", 0, occlusion ? occlusion : scene->instance->builtins.lit.placeholder_2d_view, out_error))
		return set;
	aprend_lit_scene_block &b = scene->block;
	b.occlusion_strength      = strength;
	if (occlusion)
		b.flags |= APREND_LIT_SCENE_FLAG_OCCLUSION;
	else
		b.flags &= ~APREND_LIT_SCENE_FLAG_OCCLUSION;
	aprend_lit_scene_commit(scene);
	return APRESULT_OK;
}

APRESULT aprend_lit_scene_set_scene_color(aprend_lit_scene scene, aprend_texture_view scene_color, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!scene)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is NULL");
	if (APRI_STALE(scene, "aprend_lit_scene_set_scene_color", "scene"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is destroyed");
	APREND_ENTER(scene->instance, "aprend_lit_scene_set_scene_color");
	if (scene_color && APRI_STALE(scene_color, "aprend_lit_scene_set_scene_color", "scene_color"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene_color is destroyed");
	if (scene_color && scene_color->instance != scene->instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene_color is of another instance");
	if (scene_color && scene_color->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene_color is not a SHADER_RESOURCE view");
	if (scene_color && scene_color->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene_color is not a 2D view");
	if (scene_color && scene_color->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene_color is not a view of color");
	if (const APRESULT set = aprend_material_set_texture(scene->material, "scene_color", 0, scene_color ? scene_color : scene->instance->builtins.lit.placeholder_2d_view, out_error))
		return set;
	aprend_lit_scene_block &b = scene->block;
	if (scene_color) {
		b.flags |= APREND_LIT_SCENE_FLAG_SCENE_COLOR;
		b.scene_color_mips = (float)scene_color->desc.mip_count;
	} else {
		b.flags &= ~APREND_LIT_SCENE_FLAG_SCENE_COLOR;
		b.scene_color_mips = 0.0f;
	}
	aprend_lit_scene_commit(scene);
	return APRESULT_OK;
}

APRESULT aprend_lit_scene_set_bent_normals(aprend_lit_scene scene, bool read, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!scene)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is NULL");
	if (APRI_STALE(scene, "aprend_lit_scene_set_bent_normals", "scene"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "scene is destroyed");
	APREND_ENTER(scene->instance, "aprend_lit_scene_set_bent_normals");
	aprend_lit_scene_block &b = scene->block;
	if (read)
		b.flags |= APREND_LIT_SCENE_FLAG_BENT_NORMALS;
	else
		b.flags &= ~APREND_LIT_SCENE_FLAG_BENT_NORMALS;
	aprend_lit_scene_commit(scene);
	return APRESULT_OK;
}

// ---- Fitting shadows --------------------------------------------------------------

APRESULT aprend_lit_fit_cascades(const APREND_LIT_CASCADE_FIT *fit, APREND_LIT_SHADOW_MAPS *out_maps, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!fit)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "fit is NULL");
	if (!out_maps)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_maps is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "fit", &fit->base, APRI_STRUCT_TYPE_APREND_LIT_CASCADE_FIT))
		return base;
	if (const APRESULT base = aprend_base_check(out_error, "out_maps", &out_maps->base, APRI_STRUCT_TYPE_APREND_LIT_SHADOW_MAPS))
		return base;
	const APREND_LIT_CASCADE_FIT &f = *fit;
	if (!all_finite(f.eye_position, 3))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "eye_position is not finite");
	if (!all_finite(f.view_direction, 3))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view_direction is not finite");
	if (!all_finite(f.view_up, 3))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view_up is not finite");
	if (!finite(f.fov_y))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "fov_y is not finite");
	if (!finite(f.half_height))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "half_height is not finite");
	if (!finite(f.aspect))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "aspect is not finite");
	if (!finite(f.near_depth))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "near_depth is not finite");
	if (!finite(f.far_depth))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "far_depth is not finite");
	if (!finite(f.split_lambda))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "split_lambda is not finite");
	if (!all_finite(f.light_direction, 3))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "light_direction is not finite");
	if (!finite(f.caster_distance))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "caster_distance is not finite");
	const glm::vec3 forward_in = vec3_of(f.view_direction), up_in = vec3_of(f.view_up), sun_in = vec3_of(f.light_direction);
	if (glm::length(forward_in) == 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view_direction is 0");
	const glm::vec3 forward = glm::normalize(forward_in);
	glm::vec3 right         = glm::cross(forward, up_in);
	if (glm::length(right) <= 1e-6f * std::max(glm::length(up_in), 1e-30f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view_up runs along the view direction");
	right              = glm::normalize(right);
	const glm::vec3 up = glm::cross(right, forward);
	if (!f.orthographic && !(f.fov_y > 0.0f && f.fov_y < 3.1f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a perspective view's fov_y is not between 0 and 3.1");
	if (f.orthographic && !(f.half_height > 0.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a parallel view's half_height is not above 0");
	if (!(f.aspect > 0.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "aspect is not above 0");
	if (!f.orthographic && !(f.near_depth > 0.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a perspective view's near_depth is not above 0");
	if (!(f.far_depth > f.near_depth))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "far_depth is not beyond near_depth");
	if (f.cascade_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "cascade_count is 0");
	if (f.cascade_count > APREND_LIT_MAX_CASCADES)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "cascade_count %u is above %u", f.cascade_count, APREND_LIT_MAX_CASCADES);
	if (f.split_lambda < 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "split_lambda is below 0");
	if (f.split_lambda > 1.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "split_lambda is above 1");
	if (glm::length(sun_in) == 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "light_direction is 0");
	if (f.caster_distance < 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "caster_distance is negative");
	if (f.map_size == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "map_size is 0");

	const glm::vec3 eye = vec3_of(f.eye_position);
	const glm::vec3 sun = glm::normalize(sun_in);
	const float n = f.near_depth, fa = f.far_depth;
	float start = n;
	// The split before the last: where the stretch the surface shader
	// blends over at the end of the last cascade began (0 before the first).
	float before = 0.0f;
	for (uint32_t c = 0; c < f.cascade_count; ++c) {
		// Zhang's practical split: a mix of even and logarithmic.
		const float t     = (float)(c + 1) / (float)f.cascade_count;
		const float even  = n + (fa - n) * t;
		const float ratio = n > 0.0f ? n * std::pow(fa / n, t) : even;
		const float end   = c + 1 == f.cascade_count ? fa : f.split_lambda * ratio + (1.0f - f.split_lambda) * even;
		// The stretch's eight corners, and the sphere round them: its
		// radius depends on the stretch's shape alone, so it doesn't change
		// as the view turns.
		// Each cascade after the first reaches back over the last tenth of
		// the one before, which the surface shader blends into it.
		const float reach     = c == 0 ? start : std::max(n, start - 0.1f * (start - before));
		glm::vec3 corners[8];
		const float depths[2] = {reach, end};
		for (int k = 0; k < 2; ++k) {
			const float d          = depths[k];
			const float hh         = f.orthographic ? f.half_height : d * std::tan(f.fov_y * 0.5f);
			const float hw         = hh * f.aspect;
			const glm::vec3 centre = eye + forward * d;
			for (int i = 0; i < 4; ++i)
				corners[k * 4 + i] = centre + right * ((i & 1) ? hw : -hw) + up * ((i & 2) ? hh : -hh);
		}
		glm::vec3 centre(0.0f);
		for (const glm::vec3 &corner : corners)
			centre += corner / 8.0f;
		float radius = 0.0f;
		for (const glm::vec3 &corner : corners)
			radius = std::max(radius, glm::length(corner - centre));
		radius = std::ceil(radius * 16.0f) / 16.0f;

		// Looking down the light's direction at the sphere, from far enough
		// out to catch what stands between it and the light.
		const glm::vec3 light_up = std::fabs(sun.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
		const float back         = radius + f.caster_distance;
		const glm::mat4 view     = glm::lookAtRH(centre + sun * back, centre, light_up);
		const float depth        = back + radius;
		glm::mat4 projection     = aprend_flip_clip_y(f.reversed_depth ? glm::orthoRH_ZO(-radius, radius, -radius, radius, depth, 0.0f)
		                                                               : glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, depth));
		// Moved by whole texels: where the scene's origin lands, rounded.
		glm::mat4 matrix        = projection * view;
		const float half_texels = (float)f.map_size * 0.5f;
		const glm::vec4 origin  = matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		const glm::vec2 at      = glm::vec2(origin.x, origin.y) * half_texels;
		const glm::vec2 offset  = (glm::round(at) - at) / half_texels;
		projection[3][0] += offset.x;
		projection[3][1] += offset.y;
		matrix = projection * view;
		memcpy(out_maps->_view_projection[c], &matrix, sizeof(float) * 16);
		out_maps->_split_depths[c] = end;
		before                      = c == 0 ? 0.0f : start;
		start                       = end;
	}
	for (uint32_t c = f.cascade_count; c < 6; ++c)
		memset(out_maps->_view_projection[c], 0, sizeof(float) * 16);
	for (uint32_t c = f.cascade_count; c < APREND_LIT_MAX_CASCADES; ++c)
		out_maps->_split_depths[c] = 0.0f;
	out_maps->_view_count = f.cascade_count;
	return APRESULT_OK;
}

APRESULT aprend_lit_point_shadow_view_projection(const APREND_LIT_SCENE_LIGHT *light, uint32_t face, float near_depth, bool reversed_depth,
                                                 float out_view_projection[16], APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!light)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "light is NULL");
	if (!out_view_projection)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_view_projection is NULL");
	if (light->_type != APREND_LIT_LIGHT_TYPE_POINT)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the light is not a point light");
	if (const APRESULT valid = aprend_lit_light_valid(*light, 0, out_error))
		return valid;
	if (face >= 6)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "face %u is not below 6", face);
	if (!(near_depth > 0.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "near_depth is not above 0");
	if (!(near_depth < light->_range))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "near_depth is not below the light's range");
	static const glm::vec3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
	const glm::vec3 position = vec3_of(light->_position), axis = axes[face];
	// Any up that isn't the axis: what the matrix renders is what the
	// surface reads through it.
	const glm::vec3 up = face == 2 || face == 3 ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
	// A margin past 90 degrees, as a spot's has past its cone, for the taps
	// of a soft shadow near a face's edge.
	const float fov            = 2.0f * std::atan(1.05f);
	const glm::mat4 projection = aprend_flip_clip_y(reversed_depth ? glm::perspectiveRH_ZO(fov, 1.0f, light->_range, near_depth)
	                                                               : glm::perspectiveRH_ZO(fov, 1.0f, near_depth, light->_range));
	const glm::mat4 matrix     = projection * glm::lookAtRH(position, position + axis, up);
	memcpy(out_view_projection, &matrix, sizeof(float) * 16);
	return APRESULT_OK;
}

APRESULT aprend_lit_spot_shadow_view_projection(const APREND_LIT_SCENE_LIGHT *light, float near_depth, bool reversed_depth, float out_view_projection[16],
                                                APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!light)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "light is NULL");
	if (!out_view_projection)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_view_projection is NULL");
	if (light->_type != APREND_LIT_LIGHT_TYPE_SPOT)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the light is not a spot");
	if (const APRESULT valid = aprend_lit_light_valid(*light, 0, out_error))
		return valid;
	if (!(near_depth > 0.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "near_depth is not above 0");
	if (!(near_depth < light->_range))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "near_depth is not below the light's range");
	const glm::vec3 position = vec3_of(light->_position), axis = glm::normalize(vec3_of(light->_direction));
	const glm::vec3 up       = std::fabs(axis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
	// Wide enough for the cone, with a margin the softness reads into.
	const float fov            = std::min(light->_outer_angle * 2.0f * 1.05f, 3.1f);
	const glm::mat4 projection = aprend_flip_clip_y(reversed_depth ? glm::perspectiveRH_ZO(fov, 1.0f, light->_range, near_depth)
	                                                               : glm::perspectiveRH_ZO(fov, 1.0f, near_depth, light->_range));
	const glm::mat4 matrix     = projection * glm::lookAtRH(position, position + axis, up);
	memcpy(out_view_projection, &matrix, sizeof(float) * 16);
	return APRESULT_OK;
}

} // extern "C"

// ---- Exposures ----------------------------------------------------------------------

aprend_exposure_t::~aprend_exposure_t() {
	aprend_binding_set_destroy(this->histogram_set);
	aprend_buffer_destroy(this->histogram);
	for (int i = 0; i < 2; ++i) {
		aprend_texture_view_destroy(this->value_views[i]);
		aprend_texture_view_destroy(this->value_targets[i]);
		aprend_texture2d_destroy(this->values[i]);
	}
	aprend_texture_view_destroy(this->luminance_view);
	aprend_texture_view_destroy(this->luminance_target);
	aprend_texture2d_destroy(this->luminance);
}

extern "C" {

APRESULT aprend_exposure_create(
    aprend_instance instance,
    const APREND_EXPOSURE_DESC *desc,
    aprend_exposure *out_exposure,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_exposure)
		*out_exposure = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_exposure_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_exposure_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_EXPOSURE_DESC))
		return base;
	const APREND_EXPOSURE_DESC &d = *desc;
	if (!finite(d.key) || !finite(d.min_log) || !finite(d.max_log) || !finite(d.up_rate) || !finite(d.down_rate) || !(d.key > 0.0f) ||
	    !(d.min_log < d.max_log) || !(d.up_rate > 0.0f) || !(d.down_rate > 0.0f)) {
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the key and rates are above 0, and min_log below max_log, all finite");
	}
	if (!(d.low_share >= 0.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "low_share is negative or not a number");
	if (!(d.high_share <= 1.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "high_share is above 1 or not a number");
	if (!(d.low_share < d.high_share))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "low_share %g is not below high_share %g", (double)d.low_share, (double)d.high_share);
	// Every texel measured is the log average, exactly, through the
	// luminance texture's mips; leaving some out takes a histogram.
	const bool histogram = d.low_share > 0.0f || d.high_share < 1.0f;
	if (!out_exposure)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_exposure is NULL");
	if (histogram && !aprend_lit_histogram_pipeline(instance))
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "Aprend's exposure histogram pipeline can't be made");
	APREND_MALLOC__T(exposure, aprend_exposure_t);
	if (!exposure)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the exposure");
	APREND_CONSTRUCT__T(exposure, aprend_exposure_t);
	exposure->instance         = instance;
	exposure->desc             = d;
	exposure->luminance        = aprend_lit_work_texture(instance, APREND_EXPOSURE_LUMINANCE_SIZE, log2_of(APREND_EXPOSURE_LUMINANCE_SIZE) + 1, 1,
	                                                     APREND_EXPOSURE_LUMINANCE_FORMAT);
	exposure->luminance_target = exposure->luminance ? aprend_lit_target_view(exposure->luminance, 0, 0) : nullptr;
	exposure->luminance_view   = exposure->luminance ? aprend_lit_whole_view(exposure->luminance, false) : nullptr;
	bool ok                    = exposure->luminance_target && exposure->luminance_view;
	for (int i = 0; ok && i < 2; ++i) {
		exposure->values[i]        = aprend_lit_work_texture(instance, 1, 1, 1, APREND_EXPOSURE_FORMAT);
		exposure->value_targets[i] = exposure->values[i] ? aprend_lit_target_view(exposure->values[i], 0, 0) : nullptr;
		exposure->value_views[i]   = exposure->values[i] ? aprend_lit_whole_view(exposure->values[i], false) : nullptr;
		ok                         = exposure->value_targets[i] && exposure->value_views[i];
	}
	if (ok && histogram) {
		if (aprend_lit_storage_buffer(instance, APREND_EXPOSURE_BINS * sizeof(uint32_t), true, "aprend exposure histogram", &exposure->histogram, nullptr) == APRESULT_OK) {
			// The luminance and the bins: the histogram loads texels, with
			// no sampler, so its layout (made from its shaders) has none.
			APREND_BINDING_SET_ENTRY entries[2]{};
			entries[0]._binding                   = 0;
			entries[0]._type                      = SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			entries[0]._resource._image._view     = exposure->luminance_view;
			entries[1]._binding                   = 2;
			entries[1]._type                      = SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			entries[1]._resource._storage._buffer = exposure->histogram;
			(void)aprend_binding_set_create(instance->builtins.lit.histogram_layout, entries, 2, &exposure->histogram_set, nullptr);
		}
		ok = exposure->histogram_set != nullptr;
	}
	if (!ok) {
		APREND_DESTRUCT__T(exposure, aprend_exposure_t);
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "a texture, view, buffer or set of the exposure can't be created");
	}
	*out_exposure = exposure;
	return APRESULT_OK;
}

void aprend_exposure_destroy(aprend_exposure exposure) {
	if (!exposure)
		return;
	if (APRI_STALE(exposure, "aprend_exposure_destroy", "exposure"))
		return;
	APREND_DESTRUCT__T(exposure, aprend_exposure_t);
}

} // extern "C"
