#include "render/aprendlit.h"
#include "render/aprendpost.h"
#include "aprend_internal.hpp"
#include "aprendbuiltin_internal.hpp"
#include "aprendimages_internal.hpp"
#include "aprendlit_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The SPIR-V of shaders/, embedded by the build (CMakeLists.txt).
static const uint32_t s_lit_frag_spv[] =
#include "aprend_lit.frag.spv.inc"
    ;
static const uint32_t s_lit_depth_frag_spv[] =
#include "aprend_lit_depth.frag.spv.inc"
    ;
static const uint32_t s_lit_vert_spv[] =
#include "aprend_lit.vert.spv.inc"
    ;
static const uint32_t s_lit_shadow_vert_spv[] =
#include "aprend_lit_shadow.vert.spv.inc"
    ;
static const uint32_t s_lit_depth_cutout_frag_spv[] =
#include "aprend_lit_depth_cutout.frag.spv.inc"
    ;
static const uint32_t s_lit_sky_frag_spv[] =
#include "aprend_lit_sky.frag.spv.inc"
    ;
static const uint32_t s_lit_cull_comp_spv[] =
#include "aprend_lit_cull.comp.spv.inc"
    ;
static const uint32_t s_environment_from_cube_frag_spv[] =
#include "aprend_environment_from_cube.frag.spv.inc"
    ;
static const uint32_t s_environment_from_equirect_frag_spv[] =
#include "aprend_environment_from_equirect.frag.spv.inc"
    ;
static const uint32_t s_environment_prefilter_frag_spv[] =
#include "aprend_environment_prefilter.frag.spv.inc"
    ;
static const uint32_t s_environment_irradiance_frag_spv[] =
#include "aprend_environment_irradiance.frag.spv.inc"
    ;
static const uint32_t s_environment_sky_frag_spv[] =
#include "aprend_environment_sky.frag.spv.inc"
    ;
static const uint32_t s_lit_brdf_lut_frag_spv[] =
#include "aprend_lit_brdf_lut.frag.spv.inc"
    ;
static const uint32_t s_lit_occlusion_frag_spv[] =
#include "aprend_lit_occlusion.frag.spv.inc"
    ;
static const uint32_t s_lit_occlusion_blur_frag_spv[] =
#include "aprend_lit_occlusion_blur.frag.spv.inc"
    ;
static const uint32_t s_exposure_luminance_frag_spv[] =
#include "aprend_exposure_luminance.frag.spv.inc"
    ;
static const uint32_t s_exposure_adapt_frag_spv[] =
#include "aprend_exposure_adapt.frag.spv.inc"
    ;
static const uint32_t s_tone_map_frag_spv[] =
#include "aprend_tone_map.frag.spv.inc"
    ;

static_assert(sizeof(aprend_lit_vertex) == 48, "aprend_lit_vertex is 48 bytes");
static_assert(sizeof(aprend_lit_instance) == 64, "aprend_lit_instance is 64 bytes");
static_assert(sizeof(aprend_lit_scene_block) == 960 + 64 * APREND_LIT_MAX_LIGHTS, "AprendLitScene's std140 size");
static_assert(sizeof(aprend_lit_material_block) == 128, "AprendLitMaterial's std140 size");
static_assert(sizeof(aprend_lit_shadow_pass_block) == 64, "AprendLitShadowPass's std140 size");
static_assert(sizeof(aprend_occlusion_block) == 176, "AprendOcclusionPass's std140 size");
static_assert(sizeof(aprend_environment_params) % 8 == 0 && sizeof(aprend_sky_params) % 8 == 0 && sizeof(aprend_tone_map_params) % 8 == 0 &&
                  sizeof(aprend_adapt_params) % 8 == 0 && sizeof(aprend_size_params) % 8 == 0,
              "a push Metal aligns to 8 bytes is a multiple of 8");

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
	const aprend_shader_member *found = nullptr;
	uint32_t found_count              = 0;
	if (!aprend_binding_layout_get_block(layout, binding, &block_size, &found, &found_count)) {
		printf("apricot: %s has no block at binding %u\n", what, binding);
		return false;
	}
	bool ok = block_size <= size && size - block_size < 16;
	if (!ok)
		printf("apricot: %s is %u bytes in the shader, and its mirror %u\n", what, block_size, size);
	for (size_t i = 0; i < count; ++i) {
		bool here = false;
		for (uint32_t m = 0; m < found_count && !here; ++m)
			here = strcmp(found[m].name, members[i].name) == 0 && found[m].offset == members[i].offset;
		if (!here) {
			printf("apricot: %s's member \"%s\" is not at %u in the shader\n", what, members[i].name, members[i].offset);
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
	const uint32_t last = MEMBER(B, lights) + (uint32_t)sizeof(L) * (APREND_LIT_MAX_LIGHTS - 1);
	const expected_member members[] = {
	    {"view_projection", MEMBER(B, view_projection)},
	    {"inverse_view_projection", MEMBER(B, inverse_view_projection)},
	    {"sun_shadow_view_projection", MEMBER(B, sun_shadow_view_projection)},
	    {"spot_shadow_view_projection", MEMBER(B, spot_shadow_view_projection)},
	    {"eye_position", MEMBER(B, eye_position)},
	    {"view_direction", MEMBER(B, view_direction)},
	    {"sun_direction", MEMBER(B, sun_direction)},
	    {"sun_radiance", MEMBER(B, sun_radiance)},
	    {"ambient_up", MEMBER(B, ambient_up)},
	    {"ambient_sky", MEMBER(B, ambient_sky)},
	    {"ambient_ground", MEMBER(B, ambient_ground)},
	    {"environment_rotation", MEMBER(B, environment_rotation)},
	    {"sun_shadow_splits", MEMBER(B, sun_shadow_splits)},
	    {"sun_shadow_params", MEMBER(B, sun_shadow_params)},
	    {"spot_shadow_params", MEMBER(B, spot_shadow_params)},
	    {"target_size", MEMBER(B, target_size)},
	    {"environment_intensity", MEMBER(B, environment_intensity)},
	    {"environment_mips", MEMBER(B, environment_mips)},
	    {"exposure", MEMBER(B, exposure)},
	    {"output_encoding", MEMBER(B, output_encoding)},
	    {"flags", MEMBER(B, flags)},
	    {"light_count", MEMBER(B, light_count)},
	    {"sun_cascade_count", MEMBER(B, sun_cascade_count)},
	    {"tile_size", MEMBER(B, tile_size)},
	    {"tiles_x", MEMBER(B, tiles_x)},
	    {"tiles_y", MEMBER(B, tiles_y)},
	    {"occlusion_strength", MEMBER(B, occlusion_strength)},
	    {"lights[0].position_range", MEMBER(B, lights)},
	    {"lights[0].outer_shadow", MEMBER(B, lights) + MEMBER(L, outer_shadow)},
	    {"lights[127].outer_shadow", last + MEMBER(L, outer_shadow)},
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
	};
	return aprend_lit_check_block(layout, 0, "AprendLitMaterial", (uint32_t)sizeof(B), members, COUNT_OF(members));
}

static bool aprend_lit_check_shadow_pass_block(aprend_binding_layout layout) {
	const expected_member members[] = {{"view_projection", 0}};
	return aprend_lit_check_block(layout, 0, "AprendLitShadowPass", (uint32_t)sizeof(aprend_lit_shadow_pass_block), members, 1);
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
	const uint32_t *code;
	uint64_t size;
	SPUDGPU_SHADER_STAGE stage;
	const char *name;
} s_lit_shaders[6] = {
    {s_lit_frag_spv, sizeof(s_lit_frag_spv), SPUDGPU_SHADER_STAGE_FRAGMENT, "surface fragment"},
    {s_lit_depth_frag_spv, sizeof(s_lit_depth_frag_spv), SPUDGPU_SHADER_STAGE_FRAGMENT, "depth fragment"},
    {s_lit_vert_spv, sizeof(s_lit_vert_spv), SPUDGPU_SHADER_STAGE_VERTEX, "surface vertex"},
    {s_lit_shadow_vert_spv, sizeof(s_lit_shadow_vert_spv), SPUDGPU_SHADER_STAGE_VERTEX, "shadow vertex"},
    {s_lit_depth_cutout_frag_spv, sizeof(s_lit_depth_cutout_frag_spv), SPUDGPU_SHADER_STAGE_FRAGMENT, "cut-out depth fragment"},
    {s_lit_sky_frag_spv, sizeof(s_lit_sky_frag_spv), SPUDGPU_SHADER_STAGE_FRAGMENT, "sky fragment"},
};

static aprend_shader aprend_lit_shader(aprend_instance instance, uint32_t which) {
	aprend_shader &shader = instance->builtins.lit.shaders[which];
	if (!shader) {
		shader = aprend_shader_create_spirv(instance, s_lit_shaders[which].code, s_lit_shaders[which].size, s_lit_shaders[which].stage);
		if (!shader)
			printf("apricot: Aprend's lit %s shader can't be created\n", s_lit_shaders[which].name);
	}
	return shader;
}

static aprend_sampler aprend_lit_sampler(aprend_instance instance, SPUDGPU_ADDRESS_MODE address, bool compare, SPUDGPU_COMPARE_OP op,
                                         const char *name) {
	aprend_sampler_desc desc{};
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
	aprend_sampler sampler = aprend_sampler_create(instance, &desc);
	if (!sampler)
		printf("apricot: Aprend's %s can't be created\n", name);
	return sampler;
}

// A placeholder texture of 1x1 texels: one layer, six for a cube; of depth
// for a shadow map. Never written. Its view in the binding's shape.
static bool aprend_lit_placeholder(aprend_instance instance, APREND_TEXTURE_VIEW_SHAPE shape, bool depth, aprend_texture2d *out_texture,
                                   aprend_texture_view *out_view) {
	if (*out_view)
		return true;
	const bool cube = shape == APREND_TEXTURE_VIEW_SHAPE_CUBE;
	if (!*out_texture) {
		aprend_texture2d_desc desc{};
		desc.width        = 1;
		desc.height       = 1;
		desc.format       = depth ? SPUDGPU_FORMAT_D32_FLOAT : SPUDGPU_FORMAT_R8G8B8A8_UNORM;
		desc.usage        = APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE | (depth ? APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL : 0);
		desc.mip_levels   = 1;
		desc.array_layers = cube ? 6 : 1;
		desc.sample_count = 1;
		desc.cube_capable = cube;
		*out_texture      = aprend_texture2d_create(instance, &desc);
		if (!*out_texture) {
			printf("apricot: Aprend's lit placeholder texture can't be created\n");
			return false;
		}
	}
	aprend_texture_view_desc vd{};
	vd.type        = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
	vd.shape       = shape;
	vd.aspect      = depth ? APREND_TEXTURE_VIEW_ASPECT_DEPTH : APREND_TEXTURE_VIEW_ASPECT_COLOR;
	vd.base_mip    = 0;
	vd.mip_count   = 1;
	vd.base_layer  = 0;
	vd.layer_count = cube ? 6 : 1;
	*out_view      = aprend_texture_view_create_2d(*out_texture, &vd);
	if (!*out_view)
		printf("apricot: Aprend's lit placeholder view can't be created\n");
	return *out_view != nullptr;
}

bool aprend_lit_prepare_resources(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (!aprend_lit_placeholder(instance, APREND_TEXTURE_VIEW_SHAPE_2D, false, &lit.placeholder_2d, &lit.placeholder_2d_view))
		return false;
	if (!aprend_lit_placeholder(instance, APREND_TEXTURE_VIEW_SHAPE_CUBE, false, &lit.placeholder_cube, &lit.placeholder_cube_view))
		return false;
	if (!aprend_lit_placeholder(instance, APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY, true, &lit.placeholder_depth, &lit.placeholder_depth_view))
		return false;
	if (!lit.placeholder_tiles) {
		lit.placeholder_tiles = aprend_storage_buffer_create(instance, 64, APREND_STORAGE_BUFFER_USAGE_NONE, APREND_BUFFER_WRITES_ONCE, nullptr);
		if (!lit.placeholder_tiles) {
			printf("apricot: Aprend's lit placeholder tiles can't be created\n");
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
		aprend_texture2d_desc desc{};
		desc.width        = APREND_BRDF_LUT_SIZE;
		desc.height       = APREND_BRDF_LUT_SIZE;
		desc.format       = APREND_BRDF_LUT_FORMAT;
		desc.usage        = APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE | APREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
		desc.mip_levels   = 1;
		desc.array_layers = 1;
		desc.sample_count = 1;
		lit.brdf_lut      = aprend_texture2d_create(instance, &desc);
		if (!lit.brdf_lut) {
			printf("apricot: Aprend's BRDF lookup texture can't be created\n");
			return false;
		}
	}
	aprend_texture_view_desc vd{};
	vd.shape       = APREND_TEXTURE_VIEW_SHAPE_2D;
	vd.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
	vd.mip_count   = 1;
	vd.layer_count = 1;
	if (!lit.brdf_lut_view) {
		vd.type           = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
		lit.brdf_lut_view = aprend_texture_view_create_2d(lit.brdf_lut, &vd);
	}
	if (!lit.brdf_lut_target) {
		vd.type             = APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET;
		lit.brdf_lut_target = aprend_texture_view_create_2d(lit.brdf_lut, &vd);
	}
	if (!lit.brdf_lut_view || !lit.brdf_lut_target)
		printf("apricot: the views of Aprend's BRDF lookup texture can't be created\n");
	return lit.brdf_lut_view && lit.brdf_lut_target;
}

// A pipeline over aprend_fullscreen.vert with [fragment], rendering to
// [format], [layouts] at its first slots, [push_size] bytes of push
// constants (none if 0).
static aprend_graphics_pipeline aprend_lit_fullscreen_pipeline(aprend_instance instance, aprend_shader fragment, const aprend_binding_layout *layouts,
                                                               uint32_t layout_count, SPUDGPU_FORMAT format, uint32_t push_size,
                                                               const char *name) {
	aprend_graphics_pipeline_desc desc{};
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
	desc._vertex_entry_point         = "main";
	desc._fragment_entry_point       = "main";
	desc.color_attachment_formats[0] = format;
	desc.color_attachment_count      = 1;
	desc.depth_format                = SPUDGPU_FORMAT_UNKNOWN;
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
	aprend_graphics_pipeline pipeline = aprend_graphics_pipeline_create(instance, desc);
	if (!pipeline)
		printf("apricot: Aprend's %s pipeline can't be created for format %d\n", name, (int)format);
	return pipeline;
}

static const struct {
	const uint32_t *code;
	uint64_t size;
	SPUDGPU_FORMAT format;
	uint32_t push_size;
	const char *name;
} s_passes[APREND_LIT_PASS_COUNT] = {
    {s_environment_from_cube_frag_spv, sizeof(s_environment_from_cube_frag_spv), APREND_ENVIRONMENT_FORMAT, sizeof(aprend_environment_params),
     "environment from a cube"},
    {s_environment_from_equirect_frag_spv, sizeof(s_environment_from_equirect_frag_spv), APREND_ENVIRONMENT_FORMAT,
     sizeof(aprend_environment_params), "environment from a picture"},
    {s_environment_prefilter_frag_spv, sizeof(s_environment_prefilter_frag_spv), APREND_ENVIRONMENT_FORMAT, sizeof(aprend_environment_params),
     "environment prefilter"},
    {s_environment_irradiance_frag_spv, sizeof(s_environment_irradiance_frag_spv), APREND_ENVIRONMENT_FORMAT,
     sizeof(aprend_environment_params), "environment irradiance"},
    {s_environment_sky_frag_spv, sizeof(s_environment_sky_frag_spv), APREND_ENVIRONMENT_FORMAT, sizeof(aprend_sky_params), "sky"},
    {s_lit_brdf_lut_frag_spv, sizeof(s_lit_brdf_lut_frag_spv), APREND_BRDF_LUT_FORMAT, sizeof(aprend_size_params), "BRDF lookup"},
    {s_lit_occlusion_frag_spv, sizeof(s_lit_occlusion_frag_spv), APREND_OCCLUSION_FORMAT, 0, "ambient occlusion"},
    {s_lit_occlusion_blur_frag_spv, sizeof(s_lit_occlusion_blur_frag_spv), SPUDGPU_FORMAT_UNKNOWN, sizeof(aprend_size_params),
     "ambient occlusion blur"},
    {s_exposure_luminance_frag_spv, sizeof(s_exposure_luminance_frag_spv), APREND_EXPOSURE_LUMINANCE_FORMAT, sizeof(aprend_size_params),
     "exposure luminance"},
    {s_exposure_adapt_frag_spv, sizeof(s_exposure_adapt_frag_spv), APREND_EXPOSURE_FORMAT, sizeof(aprend_adapt_params), "exposure adapt"},
};

// Every pass's fragment shader, the pass layout (a texture at 0, a sampler
// at 1), and the occlusion pass's own layout. False, printed, if one can't
// be made.
static bool aprend_lit_prepare_passes(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (!aprend_lit_prepare_resources(instance))
		return false;
	for (uint32_t pass = 0; pass < APREND_LIT_PASS_COUNT; ++pass) {
		if (lit.pass_frags[pass])
			continue;
		lit.pass_frags[pass] = aprend_shader_create_spirv(instance, s_passes[pass].code, s_passes[pass].size, SPUDGPU_SHADER_STAGE_FRAGMENT);
		if (!lit.pass_frags[pass]) {
			printf("apricot: Aprend's %s shader can't be created\n", s_passes[pass].name);
			return false;
		}
	}
	if (!lit.pass_layout) {
		// The passes that read a texture and a sampler at set 0.
		aprend_shader readers[5] = {lit.pass_frags[APREND_LIT_PASS_FROM_CUBE], lit.pass_frags[APREND_LIT_PASS_FROM_EQUIRECT],
		                            lit.pass_frags[APREND_LIT_PASS_PREFILTER], lit.pass_frags[APREND_LIT_PASS_IRRADIANCE],
		                            lit.pass_frags[APREND_LIT_PASS_BLUR]};
		lit.pass_layout = aprend_binding_layout_create_from_shaders(instance, readers, 5, 0);
		if (!lit.pass_layout) {
			printf("apricot: the binding layout of Aprend's passes can't be created\n");
			return false;
		}
	}
	if (!lit.occlusion_layout) {
		lit.occlusion_layout = aprend_binding_layout_create_from_shaders(instance, &lit.pass_frags[APREND_LIT_PASS_OCCLUSION], 1, 0);
		if (!lit.occlusion_layout || !aprend_lit_check_occlusion_block(lit.occlusion_layout)) {
			aprend_binding_layout_destroy(lit.occlusion_layout);
			lit.occlusion_layout = nullptr;
			printf("apricot: the binding layout of Aprend's ambient occlusion can't be created\n");
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
	} else if (pass == APREND_LIT_PASS_ADAPT)
		count = 2;
	lit.pass_pipelines[pass] = aprend_lit_fullscreen_pipeline(instance, lit.pass_frags[pass], layouts, count, s_passes[pass].format,
	                                                         s_passes[pass].push_size, s_passes[pass].name);
	return lit.pass_pipelines[pass];
}

static aprend_graphics_pipeline aprend_lit_format_pipeline(aprend_instance instance, std::vector<aprend_lit_builtins::format_pipeline> &pipelines,
                                                           aprend_shader fragment, const aprend_binding_layout *layouts, uint32_t layout_count,
                                                           SPUDGPU_FORMAT format, uint32_t push_size, const char *name) {
	for (const aprend_lit_builtins::format_pipeline &entry : pipelines)
		if (entry.format == format)
			return entry.pipeline;
	aprend_graphics_pipeline pipeline = aprend_lit_fullscreen_pipeline(instance, fragment, layouts, layout_count, format, push_size, name);
	if (!pipeline)
		return nullptr;
	try {
		pipelines.push_back({format, pipeline});
	} catch (...) {
		aprend_graphics_pipeline_destroy(pipeline);
		printf("apricot: no memory to keep Aprend's %s pipeline\n", name);
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
		lit.tone_map_frag = aprend_shader_create_spirv(instance, s_tone_map_frag_spv, sizeof(s_tone_map_frag_spv), SPUDGPU_SHADER_STAGE_FRAGMENT);
		if (!lit.tone_map_frag) {
			printf("apricot: Aprend's tone map shader can't be created\n");
			return nullptr;
		}
	}
	if (!lit.tone_map_layout) {
		lit.tone_map_layout = aprend_binding_layout_create_from_shaders(instance, &lit.tone_map_frag, 1, 0);
		if (!lit.tone_map_layout) {
			printf("apricot: the binding layout of Aprend's tone map shader can't be created\n");
			return nullptr;
		}
	}
	const aprend_binding_layout layouts[2] = {lit.tone_map_layout, lit.pass_layout};
	return aprend_lit_format_pipeline(instance, lit.tone_map_pipelines, lit.tone_map_frag, layouts, 2, format, sizeof(aprend_tone_map_params),
	                                  "tone map");
}

aprend_compute_pipeline aprend_lit_cull_pipeline(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.cull_pipeline)
		return lit.cull_pipeline;
	if (!lit.cull_comp) {
		lit.cull_comp = aprend_shader_create_spirv(instance, s_lit_cull_comp_spv, sizeof(s_lit_cull_comp_spv), SPUDGPU_SHADER_STAGE_COMPUTE);
		if (!lit.cull_comp) {
			printf("apricot: Aprend's light culling shader can't be created\n");
			return nullptr;
		}
	}
	if (!lit.cull_layout) {
		lit.cull_layout = aprend_binding_layout_create_from_shaders(instance, &lit.cull_comp, 1, 0);
		if (!lit.cull_layout || !aprend_lit_check_scene_block(lit.cull_layout)) {
			aprend_binding_layout_destroy(lit.cull_layout);
			lit.cull_layout = nullptr;
			printf("apricot: the binding layout of Aprend's light culling can't be created\n");
			return nullptr;
		}
	}
	aprend_compute_pipeline_desc desc{};
	desc._debug_name = "aprend light culling";
	desc.compute_shader        = lit.cull_comp;
	desc._entry_point          = "main";
	desc._binding_layouts[0]   = lit.cull_layout;
	desc._binding_layout_count = 1;
	lit.cull_pipeline          = aprend_compute_pipeline_create(instance, desc);
	if (!lit.cull_pipeline)
		printf("apricot: Aprend's light culling pipeline can't be created\n");
	return lit.cull_pipeline;
}

aprend_binding_set aprend_texture_view_environment_set(aprend_texture_view view) {
	if (view->environment_set)
		return view->environment_set;
	aprend_instance instance = view->instance;
	if (!aprend_lit_prepare_passes(instance))
		return nullptr;
	aprend_binding_set_entry entries[2]{};
	entries[0]._binding                  = 0;
	entries[0]._type                     = SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	entries[0]._resource._image._view    = view;
	entries[1]._binding                  = 1;
	entries[1]._type                     = SPUDGPU_DESCRIPTOR_TYPE_SAMPLER;
	entries[1]._resource._image._sampler = instance->builtins.lit.environment_sampler;
	view->environment_set                = aprend_binding_set_create(instance->builtins.lit.pass_layout, entries, 2);
	if (!view->environment_set)
		printf("apricot: the binding set that reads a view can't be created\n");
	return view->environment_set;
}

void aprend_lit_builtins_destroy(aprend_instance instance) {
	aprend_lit_builtins &lit = instance->builtins.lit;
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
	lit.cull_pipeline                = nullptr;
	aprend_binding_layout *layouts[] = {&lit.tone_map_layout, &lit.pass_layout,   &lit.occlusion_layout, &lit.cull_layout,
	                                    &lit.material_layout, &lit.shadow_layout, &lit.scene_layout};
	for (aprend_binding_layout *layout : layouts) {
		aprend_binding_layout_destroy(*layout);
		*layout = nullptr;
	}
	aprend_shader_destroy(lit.tone_map_frag);
	lit.tone_map_frag = nullptr;
	aprend_shader_destroy(lit.cull_comp);
	lit.cull_comp = nullptr;
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
		aprend_destroy_texture_view(*view);
		*view = nullptr;
	}
	aprend_texture2d *textures[] = {&lit.brdf_lut, &lit.placeholder_depth, &lit.placeholder_cube, &lit.placeholder_2d};
	for (aprend_texture2d *texture : textures) {
		aprend_texture2d_destroy(*texture);
		*texture = nullptr;
	}
	aprend_storage_buffer_destroy(lit.placeholder_tiles);
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

static aprend_vertex_element s_lit_vertex_elements[] = {
    {"in_position", APREND_VERTEX_ELEMENT_TYPE_VEC3, (uint32_t)offsetof(aprend_lit_vertex, position)},
    {"in_normal", APREND_VERTEX_ELEMENT_TYPE_VEC3, (uint32_t)offsetof(aprend_lit_vertex, normal)},
    {"in_uv", APREND_VERTEX_ELEMENT_TYPE_VEC2, (uint32_t)offsetof(aprend_lit_vertex, uv)},
    {"in_tangent", APREND_VERTEX_ELEMENT_TYPE_VEC4, (uint32_t)offsetof(aprend_lit_vertex, tangent)},
};
static aprend_vertex_element s_lit_instance_elements[] = {
    {"in_transform0", APREND_VERTEX_ELEMENT_TYPE_VEC4, 0},
    {"in_transform1", APREND_VERTEX_ELEMENT_TYPE_VEC4, 16},
    {"in_transform2", APREND_VERTEX_ELEMENT_TYPE_VEC4, 32},
    {"in_transform3", APREND_VERTEX_ELEMENT_TYPE_VEC4, 48},
};

extern "C" {

aprend_vertex_layout aprend_lit_vertex_layout(void) {
	aprend_vertex_layout layout{};
	layout.count    = 4;
	layout.elements = s_lit_vertex_elements;
	layout.stride   = (uint32_t)sizeof(aprend_lit_vertex);
	return layout;
}

aprend_vertex_layout aprend_lit_instance_layout(void) {
	aprend_vertex_layout layout{};
	layout.count    = 4;
	layout.elements = s_lit_instance_elements;
	layout.stride   = (uint32_t)sizeof(aprend_lit_instance);
	return layout;
}

aprend_shader aprend_lit_get_shader(aprend_instance instance, APREND_LIT_SHADER shader) {
	if (!instance)
		return nullptr;
	if (shader > APREND_LIT_SHADER_SKY_FRAGMENT) {
		printf("apricot: aprend_lit_get_shader: %u is not an APREND_LIT_SHADER\n", shader);
		return nullptr;
	}
	return aprend_lit_shader(instance, shader);
}

aprend_binding_layout aprend_lit_get_scene_layout(aprend_instance instance) {
	if (!instance)
		return nullptr;
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.scene_layout)
		return lit.scene_layout;
	// Every shader that reads set 0, so each binding has every stage that
	// reads it.
	aprend_shader shaders[3] = {aprend_lit_shader(instance, APREND_LIT_SHADER_SURFACE_FRAGMENT),
	                            aprend_lit_shader(instance, APREND_LIT_SHADER_SURFACE_VERTEX),
	                            aprend_lit_shader(instance, APREND_LIT_SHADER_SKY_FRAGMENT)};
	if (!shaders[0] || !shaders[1] || !shaders[2])
		return nullptr;
	aprend_binding_layout layout = aprend_binding_layout_create_from_shaders(instance, shaders, 3, APREND_LIT_SCENE_SET);
	if (!layout) {
		printf("apricot: the binding layout of Aprend's lit scene can't be created\n");
		return nullptr;
	}
	if (!aprend_lit_check_scene_block(layout)) {
		aprend_binding_layout_destroy(layout);
		return nullptr;
	}
	lit.scene_layout = layout;
	return layout;
}

aprend_binding_layout aprend_lit_get_shadow_layout(aprend_instance instance) {
	if (!instance)
		return nullptr;
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.shadow_layout)
		return lit.shadow_layout;
	aprend_shader vertex = aprend_lit_shader(instance, APREND_LIT_SHADER_SHADOW_VERTEX);
	if (!vertex)
		return nullptr;
	aprend_binding_layout layout = aprend_binding_layout_create_from_shaders(instance, &vertex, 1, APREND_LIT_SCENE_SET);
	if (!layout) {
		printf("apricot: the binding layout of Aprend's lit shadow pass can't be created\n");
		return nullptr;
	}
	if (!aprend_lit_check_shadow_pass_block(layout)) {
		aprend_binding_layout_destroy(layout);
		return nullptr;
	}
	lit.shadow_layout = layout;
	return layout;
}

aprend_binding_layout aprend_lit_get_material_layout(aprend_instance instance) {
	if (!instance)
		return nullptr;
	aprend_lit_builtins &lit = instance->builtins.lit;
	if (lit.material_layout)
		return lit.material_layout;
	aprend_shader shaders[2] = {aprend_lit_shader(instance, APREND_LIT_SHADER_SURFACE_FRAGMENT),
	                            aprend_lit_shader(instance, APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT)};
	if (!shaders[0] || !shaders[1])
		return nullptr;
	aprend_binding_layout layout = aprend_binding_layout_create_from_shaders(instance, shaders, 2, APREND_LIT_MATERIAL_SET);
	if (!layout) {
		printf("apricot: the binding layout of Aprend's lit materials can't be created\n");
		return nullptr;
	}
	if (!aprend_lit_check_material_block(layout)) {
		aprend_binding_layout_destroy(layout);
		return nullptr;
	}
	lit.material_layout = layout;
	return layout;
}

// ---- Materials ----------------------------------------------------------------------

static const char *const s_map_bindings[4] = {"base_color_map", "normal_map", "orm_map", "emission_map"};

// [material]'s block, if it is a lit material; NULL, printed for [who], if
// not.
static aprend_material_slot *aprend_lit_material_slot(aprend_material material, const char *who) {
	if (!material) {
		printf("apricot: %s: material is NULL\n", who);
		return nullptr;
	}
	if (!material->layout || material->layout != material->instance->builtins.lit.material_layout) {
		printf("apricot: %s: material is not a lit material (aprend_lit_material_create)\n", who);
		return nullptr;
	}
	aprend_material_slot *slot = aprend_lit_block_slot(material, "u_lit_material");
	if (!slot || slot->block_size < sizeof(aprend_lit_material_block)) {
		printf("apricot: %s: the material has no lit block\n", who);
		return nullptr;
	}
	return slot;
}

aprend_material aprend_lit_material_create(aprend_instance instance) {
	static const char who[] = "aprend_lit_material_create";
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return nullptr;
	}
	aprend_binding_layout layout = aprend_lit_get_material_layout(instance);
	if (!layout || !aprend_lit_prepare_resources(instance))
		return nullptr;
	aprend_material_desc desc{};
	desc._layout             = layout;
	aprend_material material = aprend_material_create(instance, &desc);
	if (!material)
		return nullptr;
	const aprend_lit_builtins &lit = instance->builtins.lit;
	bool ok                        = aprend_material_set_sampler(material, "material_sampler", 0, lit.material_sampler);
	for (const char *binding : s_map_bindings)
		ok = aprend_material_set_texture(material, binding, 0, lit.placeholder_2d_view) && ok;
	aprend_material_slot *slot = aprend_lit_material_slot(material, who);
	if (!ok || !slot) {
		aprend_material_destroy(material);
		return nullptr;
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
	memcpy(slot->bytes, &block, sizeof(block));
	slot->dirty = true;
	return material;
}

bool aprend_lit_material_set_values(aprend_material material, const aprend_lit_material_values *values) {
	static const char who[]    = "aprend_lit_material_set_values";
	aprend_material_slot *slot = aprend_lit_material_slot(material, who);
	if (!slot)
		return false;
	if (!values) {
		printf("apricot: %s: values is NULL\n", who);
		return false;
	}
	const aprend_lit_material_values &v = *values;
	if (!all_finite(v.base_color, 4) || !all_finite(v.emission, 3) || !finite(v.roughness) || !finite(v.metalness) || !finite(v.specular) ||
	    !finite(v.normal_scale) || !finite(v.occlusion_strength) || !finite(v.alpha_cutoff) || !all_finite(v.uv_transform, 6) ||
	    !finite(v.clearcoat) || !finite(v.clearcoat_roughness) || !all_finite(v.sheen_color, 3) || !finite(v.sheen_roughness)) {
		printf("apricot: %s: a value is not a finite number\n", who);
		return false;
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
	// The maps' bits stay: they follow the textures set, not the values.
	const float *t      = v.uv_transform;
	const float rows[8] = {t[0], t[1], t[2], 0.0f, t[3], t[4], t[5], 0.0f};
	memcpy(block.uv_transform, rows, sizeof(rows));
	memcpy(slot->bytes, &block, sizeof(block));
	slot->dirty = true;
	return true;
}

bool aprend_lit_material_get_values(aprend_material material, aprend_lit_material_values *out_values) {
	static const char who[]    = "aprend_lit_material_get_values";
	aprend_material_slot *slot = aprend_lit_material_slot(material, who);
	if (!slot)
		return false;
	if (!out_values) {
		printf("apricot: %s: out_values is NULL\n", who);
		return false;
	}
	aprend_lit_material_block block;
	memcpy(&block, slot->bytes, sizeof(block));
	aprend_lit_material_values &v = *out_values;
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
	const float *r        = block.uv_transform;
	const float t[6]      = {r[0], r[1], r[2], r[4], r[5], r[6]};
	memcpy(v.uv_transform, t, sizeof(t));
	return true;
}

bool aprend_lit_material_set_map(aprend_material material, APREND_LIT_MAP map, aprend_texture_view view) {
	static const char who[]    = "aprend_lit_material_set_map";
	aprend_material_slot *slot = aprend_lit_material_slot(material, who);
	if (!slot)
		return false;
	if (map > APREND_LIT_MAP_EMISSION) {
		printf("apricot: %s: %u is not an APREND_LIT_MAP\n", who, map);
		return false;
	}
	if (view && (view->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D || view->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR)) {
		printf("apricot: %s: view is not a 2D view of color\n", who);
		return false;
	}
	aprend_texture_view given = view ? view : material->instance->builtins.lit.placeholder_2d_view;
	if (!aprend_material_set_texture(material, s_map_bindings[map], 0, given))
		return false;
	aprend_lit_material_block block;
	memcpy(&block, slot->bytes, sizeof(block));
	if (view)
		block.maps |= 1u << map;
	else
		block.maps &= ~(1u << map);
	memcpy(slot->bytes, &block, sizeof(block));
	slot->dirty = true;
	return true;
}

bool aprend_lit_material_set_sampler(aprend_material material, aprend_sampler sampler) {
	static const char who[] = "aprend_lit_material_set_sampler";
	if (!aprend_lit_material_slot(material, who))
		return false;
	return aprend_material_set_sampler(material, "material_sampler", 0, sampler ? sampler : material->instance->builtins.lit.material_sampler);
}

} // extern "C"

// ---- Environments -------------------------------------------------------------------

aprend_lit_environment_t::~aprend_lit_environment_t() {
	// Each goes through the release queue on its own.
	aprend_destroy_texture_view(this->irradiance_cube);
	for (aprend_texture_view view : this->irradiance_faces)
		aprend_destroy_texture_view(view);
	aprend_texture2d_destroy(this->irradiance);
	aprend_destroy_texture_view(this->specular_cube);
	for (aprend_texture_view view : this->specular_faces)
		aprend_destroy_texture_view(view);
	aprend_texture2d_destroy(this->specular);
	aprend_destroy_texture_view(this->radiance_cube);
	for (aprend_texture_view view : this->radiance_faces)
		aprend_destroy_texture_view(view);
	aprend_texture2d_destroy(this->radiance);
}

// A texture of [layers] square layers of [size] and [mips] for Aprend's own
// work, rendered to and sampled; a cube when [layers] is 6.
static aprend_texture2d aprend_lit_work_texture(aprend_instance instance, uint32_t size, uint32_t mips, uint32_t layers, SPUDGPU_FORMAT format) {
	aprend_texture2d_desc desc{};
	desc.width        = size;
	desc.height       = size;
	desc.format       = format;
	desc.usage        = APREND_TEXTURE_USAGE_BIT_RENDER_TARGET | APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE;
	desc.mip_levels   = mips;
	desc.array_layers = layers;
	desc.sample_count = 1;
	desc.cube_capable = layers == 6;
	return aprend_texture2d_create(instance, &desc);
}

static aprend_texture_view aprend_lit_target_view(aprend_texture2d texture, uint32_t mip, uint32_t layer) {
	aprend_texture_view_desc vd{};
	vd.type        = APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET;
	vd.shape       = APREND_TEXTURE_VIEW_SHAPE_2D;
	vd.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
	vd.base_mip    = mip;
	vd.mip_count   = 1;
	vd.base_layer  = layer;
	vd.layer_count = 1;
	return aprend_texture_view_create_2d(texture, &vd);
}

static aprend_texture_view aprend_lit_whole_view(aprend_texture2d texture, bool cube) {
	aprend_texture_view_desc vd{};
	vd.type        = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
	vd.shape       = cube ? APREND_TEXTURE_VIEW_SHAPE_CUBE : APREND_TEXTURE_VIEW_SHAPE_2D;
	vd.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
	vd.base_mip    = 0;
	vd.mip_count   = APREND_TEXTURE_VIEW_REMAINING;
	vd.base_layer  = 0;
	vd.layer_count = cube ? 6 : 1;
	return aprend_texture_view_create_2d(texture, &vd);
}

extern "C" {

aprend_lit_environment aprend_lit_environment_create(aprend_instance instance, const aprend_lit_environment_desc *desc) {
	static const char who[] = "aprend_lit_environment_create";
	if (!instance || !desc) {
		printf("apricot: %s: %s is NULL\n", who, instance ? "desc" : "instance");
		return nullptr;
	}
	if (!power_of_2(desc->specular_size) || desc->specular_size < 16 || desc->specular_size > 2048) {
		printf("apricot: %s: specular_size %u is not a power of 2 from 16 to 2048\n", who, desc->specular_size);
		return nullptr;
	}
	if (!power_of_2(desc->irradiance_size) || desc->irradiance_size < 8 || desc->irradiance_size > 128) {
		printf("apricot: %s: irradiance_size %u is not a power of 2 from 8 to 128\n", who, desc->irradiance_size);
		return nullptr;
	}
	APREND_MALLOC__T(env, aprend_lit_environment_t);
	if (!env) {
		printf("apricot: %s: out of memory\n", who);
		return nullptr;
	}
	APREND_CONSTRUCT__T(env, aprend_lit_environment_t);
	env->instance      = instance;
	env->desc          = *desc;
	env->specular_mips = log2_of(desc->specular_size) - 3 + 1; // down to 8
	try {
		env->specular_faces.resize((size_t)env->specular_mips * 6, nullptr);
	} catch (...) {
		APREND_DESTRUCT__T(env, aprend_lit_environment_t);
		printf("apricot: %s: out of memory\n", who);
		return nullptr;
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
		printf("apricot: %s: a texture or view of the environment can't be created\n", who);
		APREND_DESTRUCT__T(env, aprend_lit_environment_t);
		return nullptr;
	}
	return env;
}

void aprend_lit_environment_destroy(aprend_lit_environment environment) {
	if (!environment)
		return;
	// What it holds on the GPU goes through the release queue, each on its
	// own; the rest is the CPU's.
	APREND_DESTRUCT__T(environment, aprend_lit_environment_t);
}

uint32_t aprend_lit_environment_get_specular_mip_count(aprend_lit_environment environment) {
	return environment ? environment->specular_mips : 0;
}

} // extern "C"

// ---- Scenes -------------------------------------------------------------------------

// The block's lights' spot shadow indices, from the spot shadows' lights.
static void aprend_lit_scene_resolve_shadows(aprend_lit_scene scene) {
	aprend_lit_scene_block &b = scene->block;
	for (uint32_t i = 0; i < APREND_LIT_MAX_LIGHTS; ++i)
		b.lights[i].outer_shadow[1] = -1.0f;
	for (uint32_t s = 0; s < scene->spot_shadow_count; ++s) {
		const uint32_t light = scene->spot_shadow_lights[s];
		if (light < b.light_count && b.lights[light].radiance_type[3] > 0.5f)
			b.lights[light].outer_shadow[1] = (float)s;
	}
}

// The block as the scene holds it, into the materials that read it, for the
// next submission that binds them.
static void aprend_lit_scene_commit(aprend_lit_scene scene) {
	aprend_lit_scene_resolve_shadows(scene);
	aprend_lit_write_block(scene->material, "u_lit_scene", &scene->block, sizeof(scene->block));
	aprend_lit_write_block(scene->cull_material, "u_lit_scene", &scene->block, sizeof(scene->block));
}

static void aprend_lit_scene_free(aprend_lit_scene scene) {
	if (scene->material)
		scene->material->lit_scene = nullptr;
	aprend_material_destroy(scene->material);
	for (aprend_material material : scene->sun_shadow_materials)
		aprend_material_destroy(material);
	for (aprend_material material : scene->spot_shadow_materials)
		aprend_material_destroy(material);
	aprend_material_destroy(scene->cull_material);
	aprend_material_destroy(scene->occlusion_material);
	aprend_storage_buffer_set_destroy(scene->tiles);
	APREND_DESTRUCT__T(scene, aprend_lit_scene_t);
}

extern "C" {

aprend_lit_scene aprend_lit_scene_create(aprend_instance instance) {
	static const char who[] = "aprend_lit_scene_create";
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return nullptr;
	}
	aprend_binding_layout layout        = aprend_lit_get_scene_layout(instance);
	aprend_binding_layout shadow_layout = aprend_lit_get_shadow_layout(instance);
	if (!layout || !shadow_layout || !aprend_lit_prepare_resources(instance) || !aprend_lit_prepare_brdf_lut(instance) ||
	    !aprend_lit_cull_pipeline(instance) || !aprend_lit_prepare_passes(instance))
		return nullptr;
	APREND_MALLOC__T(scene, aprend_lit_scene_t);
	if (!scene) {
		printf("apricot: %s: out of memory\n", who);
		return nullptr;
	}
	APREND_CONSTRUCT__T(scene, aprend_lit_scene_t);
	scene->instance                = instance;
	const aprend_lit_builtins &lit = instance->builtins.lit;
	aprend_material_desc desc{};
	desc._layout    = layout;
	scene->material = aprend_material_create(instance, &desc);
	desc._layout    = shadow_layout;
	for (aprend_material &material : scene->sun_shadow_materials)
		material = aprend_material_create(instance, &desc);
	for (aprend_material &material : scene->spot_shadow_materials)
		material = aprend_material_create(instance, &desc);
	desc._layout              = lit.cull_layout;
	scene->cull_material      = aprend_material_create(instance, &desc);
	desc._layout              = lit.occlusion_layout;
	scene->occlusion_material = aprend_material_create(instance, &desc);

	bool ok = scene->material && scene->cull_material && scene->occlusion_material;
	for (aprend_material material : scene->sun_shadow_materials)
		ok = ok && material;
	for (aprend_material material : scene->spot_shadow_materials)
		ok = ok && material;
	aprend_material m = scene->material;
	ok                = ok && aprend_material_set_storage_buffer(m, "AprendLitTiles", 0, lit.placeholder_tiles, nullptr);
	ok                = ok && aprend_material_set_texture(m, "sun_shadow_map", 0, lit.placeholder_depth_view);
	ok                = ok && aprend_material_set_texture(m, "spot_shadow_map", 0, lit.placeholder_depth_view);
	ok                = ok && aprend_material_set_sampler(m, "shadow_sampler", 0, lit.shadow_sampler);
	ok                = ok && aprend_material_set_sampler(m, "shadow_sampler_reversed", 0, lit.shadow_sampler_reversed);
	ok                = ok && aprend_material_set_texture(m, "environment_specular", 0, lit.placeholder_cube_view);
	ok                = ok && aprend_material_set_texture(m, "environment_irradiance", 0, lit.placeholder_cube_view);
	ok                = ok && aprend_material_set_texture(m, "brdf_lut", 0, lit.brdf_lut_view);
	ok                = ok && aprend_material_set_sampler(m, "environment_sampler", 0, lit.environment_sampler);
	ok                = ok && aprend_material_set_texture(m, "ambient_occlusion", 0, lit.placeholder_2d_view);
	ok                = ok && aprend_material_set_storage_buffer(scene->cull_material, "AprendLitTiles", 0, lit.placeholder_tiles, nullptr);
	ok                = ok && aprend_lit_block_slot(m, "u_lit_scene") && aprend_lit_block_slot(scene->cull_material, "u_lit_scene");
	if (!ok) {
		printf("apricot: %s: the scene's materials can't be made\n", who);
		aprend_lit_scene_free(scene);
		return nullptr;
	}
	scene->material->lit_scene   = scene;
	scene->block.exposure        = 1.0f;
	scene->block.output_encoding = APREND_COLOR_ENCODING_LINEAR;
	aprend_lit_scene_commit(scene);
	return scene;
}

void aprend_lit_scene_destroy(aprend_lit_scene scene) {
	if (!scene)
		return;
	aprend_lit_scene_free(scene);
}

aprend_material aprend_lit_scene_get_material(aprend_lit_scene scene) { return scene ? scene->material : nullptr; }

aprend_material aprend_lit_scene_get_sun_shadow_material(aprend_lit_scene scene, uint32_t cascade) {
	return scene && cascade < APREND_LIT_MAX_CASCADES ? scene->sun_shadow_materials[cascade] : nullptr;
}

aprend_material aprend_lit_scene_get_spot_shadow_material(aprend_lit_scene scene, uint32_t index) {
	return scene && index < APREND_LIT_MAX_SPOT_SHADOWS ? scene->spot_shadow_materials[index] : nullptr;
}

bool aprend_lit_scene_set_view(aprend_lit_scene scene, const aprend_lit_view *view) {
	static const char who[] = "aprend_lit_scene_set_view";
	if (!scene || !view) {
		printf("apricot: %s: %s is NULL\n", who, scene ? "view" : "scene");
		return false;
	}
	const aprend_lit_view &v = *view;
	if (!all_finite(v.eye_position, 3) || !all_finite(v.view_direction, 3) || !all_finite(v.view_projection, 16) || !finite(v.exposure) ||
	    v.exposure < 0.0f) {
		printf("apricot: %s: a value is not a finite number, or the exposure is negative\n", who);
		return false;
	}
	if (v.output_encoding > APREND_COLOR_ENCODING_SRGB) {
		printf("apricot: %s: %u is not an APREND_COLOR_ENCODING\n", who, v.output_encoding);
		return false;
	}
	if ((v.target_width == 0) != (v.target_height == 0) || v.target_width > 16384 || v.target_height > 16384) {
		printf("apricot: %s: a target of %u by %u: both sides 0, or both from 1 to 16384\n", who, v.target_width, v.target_height);
		return false;
	}
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
		aprend_storage_buffer_set tiles = nullptr;
		if (tiled) {
			const uint64_t size = (uint64_t)tiles_x * tiles_y * (APREND_LIT_MAX_TILE_LIGHTS + 1) * sizeof(uint32_t);
			tiles               = aprend_storage_buffer_set_create(scene->instance, size, APREND_STORAGE_BUFFER_USAGE_NONE, APREND_BUFFER_WRITES_ONCE, nullptr);
			if (!tiles) {
				printf("apricot: %s: the view's light tiles can't be created\n", who);
				return false;
			}
		}
		const bool set = tiles ? aprend_material_set_storage_buffer(scene->material, "AprendLitTiles", 0, nullptr, tiles) &&
		                             aprend_material_set_storage_buffer(scene->cull_material, "AprendLitTiles", 0, nullptr, tiles)
		                       : aprend_material_set_storage_buffer(scene->material, "AprendLitTiles", 0, lit.placeholder_tiles, nullptr) &&
		                             aprend_material_set_storage_buffer(scene->cull_material, "AprendLitTiles", 0, lit.placeholder_tiles, nullptr);
		if (!set) {
			aprend_storage_buffer_set_destroy(tiles);
			return false;
		}
		aprend_storage_buffer_set_destroy(scene->tiles);
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
	return true;
}

bool aprend_lit_scene_set_sun(aprend_lit_scene scene, const aprend_lit_sun *sun) {
	static const char who[] = "aprend_lit_scene_set_sun";
	if (!scene || !sun) {
		printf("apricot: %s: %s is NULL\n", who, scene ? "sun" : "scene");
		return false;
	}
	if (!all_finite(sun->direction, 3) || !all_finite(sun->radiance, 3) || sun->radiance[0] < 0.0f || sun->radiance[1] < 0.0f ||
	    sun->radiance[2] < 0.0f) {
		printf("apricot: %s: a value is not a finite number, or a radiance is negative\n", who);
		return false;
	}
	const bool lit = sun->radiance[0] > 0.0f || sun->radiance[1] > 0.0f || sun->radiance[2] > 0.0f;
	if (lit && length3(sun->direction) == 0.0f) {
		printf("apricot: %s: the sun has radiance and no direction\n", who);
		return false;
	}
	aprend_lit_scene_block &b = scene->block;
	unit3(sun->direction, b.sun_direction);
	memcpy(b.sun_radiance, sun->radiance, sizeof(float) * 3);
	b.sun_radiance[3] = 0.0f;
	aprend_lit_scene_commit(scene);
	return true;
}

} // extern "C"

// Whether [map] is a SHADER_RESOURCE view of depth shaped 2D_ARRAY, of
// [scene]'s instance, with at least [layers] layers. Printed for [who].
static bool aprend_lit_shadow_map_valid(aprend_lit_scene scene, aprend_texture_view map, uint32_t layers, const char *who) {
	if (!map) {
		printf("apricot: %s: the map is NULL\n", who);
		return false;
	}
	if (map->instance != scene->instance) {
		printf("apricot: %s: the map is of another instance\n", who);
		return false;
	}
	if (map->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || map->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY ||
	    map->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_DEPTH) {
		printf("apricot: %s: the map is not a SHADER_RESOURCE 2D_ARRAY view of depth\n", who);
		return false;
	}
	if (map->desc.layer_count < layers) {
		printf("apricot: %s: the map has %u layers, and %u are needed\n", who, map->desc.layer_count, layers);
		return false;
	}
	return true;
}

static bool aprend_lit_shadow_params_valid(float bias, float normal_offset, float softness, float strength, const char *who) {
	if (!finite(bias) || !finite(normal_offset) || !finite(softness) || !finite(strength)) {
		printf("apricot: %s: a value is not a finite number\n", who);
		return false;
	}
	if (softness < 0.0f || softness > 4.0f || strength < 0.0f || strength > 1.0f) {
		printf("apricot: %s: softness is outside 0 to 4, or strength outside 0 to 1\n", who);
		return false;
	}
	return true;
}

// Whether [l] is a light aprend_lit_scene_set_lights() takes; printed for
// [who] with its [index].
static bool aprend_lit_light_valid(const aprend_lit_light &l, uint32_t index, const char *who) {
	const float half_pi = 1.57079632679f;
	if (l.type > APREND_LIT_LIGHT_TYPE_SPOT) {
		printf("apricot: %s: light %u has type %u, which is not an APREND_LIT_LIGHT_TYPE\n", who, index, l.type);
		return false;
	}
	if (!all_finite(l.position, 3) || !all_finite(l.direction, 3) || !all_finite(l.radiance, 3) || !finite(l.range) || !finite(l.inner_angle) ||
	    !finite(l.outer_angle)) {
		printf("apricot: %s: light %u has a value that is not a finite number\n", who, index);
		return false;
	}
	if (l.radiance[0] < 0.0f || l.radiance[1] < 0.0f || l.radiance[2] < 0.0f || !(l.range > 0.0f)) {
		printf("apricot: %s: light %u has a negative radiance or a range that is not above 0\n", who, index);
		return false;
	}
	if (l.type == APREND_LIT_LIGHT_TYPE_SPOT &&
	    (length3(l.direction) == 0.0f || l.inner_angle < 0.0f || l.inner_angle > l.outer_angle || l.outer_angle > half_pi)) {
		printf("apricot: %s: spot light %u has no direction, or angles that are not 0 <= inner <= outer <= pi / 2\n", who, index);
		return false;
	}
	return true;
}

extern "C" {

bool aprend_lit_scene_set_sun_shadow(aprend_lit_scene scene, const aprend_lit_sun_shadow *shadow) {
	static const char who[] = "aprend_lit_scene_set_sun_shadow";
	if (!scene) {
		printf("apricot: %s: scene is NULL\n", who);
		return false;
	}
	aprend_lit_scene_block &b = scene->block;
	if (!shadow) {
		if (!aprend_material_set_texture(scene->material, "sun_shadow_map", 0, scene->instance->builtins.lit.placeholder_depth_view))
			return false;
		b.flags &= ~(APREND_LIT_SCENE_FLAG_SUN_SHADOW | APREND_LIT_SCENE_FLAG_SUN_REVERSED);
		b.sun_cascade_count = 0;
		aprend_lit_scene_commit(scene);
		return true;
	}
	const aprend_lit_sun_shadow &s = *shadow;
	if (s.cascade_count == 0 || s.cascade_count > APREND_LIT_MAX_CASCADES) {
		printf("apricot: %s: %u cascades: 1 to %u\n", who, s.cascade_count, APREND_LIT_MAX_CASCADES);
		return false;
	}
	if (!aprend_lit_shadow_map_valid(scene, s.map, s.cascade_count, who) ||
	    !aprend_lit_shadow_params_valid(s.depth_bias, s.normal_offset, s.softness, s.strength, who))
		return false;
	for (uint32_t c = 0; c < s.cascade_count; ++c) {
		if (!all_finite(s.view_projection[c], 16) || !finite(s.split_depths[c]) || !(s.split_depths[c] > 0.0f) ||
		    (c > 0 && !(s.split_depths[c] > s.split_depths[c - 1]))) {
			printf("apricot: %s: cascade %u's matrix is not finite, or its split is not above the one before and 0\n", who, c);
			return false;
		}
	}
	if (!aprend_material_set_texture(scene->material, "sun_shadow_map", 0, s.map))
		return false;
	for (uint32_t c = 0; c < APREND_LIT_MAX_CASCADES; ++c) {
		if (c < s.cascade_count) {
			memcpy(b.sun_shadow_view_projection[c], s.view_projection[c], sizeof(float) * 16);
			b.sun_shadow_splits[c] = s.split_depths[c];
		} else {
			memset(b.sun_shadow_view_projection[c], 0, sizeof(float) * 16);
			b.sun_shadow_splits[c] = 0.0f;
		}
		aprend_lit_shadow_pass_block pass{};
		memcpy(pass.view_projection, b.sun_shadow_view_projection[c], sizeof(pass.view_projection));
		aprend_lit_write_block(scene->sun_shadow_materials[c], "u_lit_shadow_pass", &pass, sizeof(pass));
	}
	b.sun_cascade_count    = s.cascade_count;
	b.sun_shadow_params[0] = s.depth_bias;
	b.sun_shadow_params[1] = s.normal_offset;
	b.sun_shadow_params[2] = s.softness;
	b.sun_shadow_params[3] = s.strength;
	b.flags |= APREND_LIT_SCENE_FLAG_SUN_SHADOW;
	if (s.reversed_depth)
		b.flags |= APREND_LIT_SCENE_FLAG_SUN_REVERSED;
	else
		b.flags &= ~APREND_LIT_SCENE_FLAG_SUN_REVERSED;
	aprend_lit_scene_commit(scene);
	return true;
}

bool aprend_lit_scene_set_lights(aprend_lit_scene scene, const aprend_lit_light *lights, uint32_t count) {
	static const char who[] = "aprend_lit_scene_set_lights";
	if (!scene) {
		printf("apricot: %s: scene is NULL\n", who);
		return false;
	}
	if (count > 0 && !lights) {
		printf("apricot: %s: lights is NULL with a count of %u\n", who, count);
		return false;
	}
	if (count > APREND_LIT_MAX_LIGHTS) {
		printf("apricot: %s: %u lights, and a scene holds %u\n", who, count, APREND_LIT_MAX_LIGHTS);
		return false;
	}
	for (uint32_t i = 0; i < count; ++i)
		if (!aprend_lit_light_valid(lights[i], i, who))
			return false;
	aprend_lit_scene_block &b = scene->block;
	memset(b.lights, 0, sizeof(b.lights));
	for (uint32_t i = 0; i < count; ++i) {
		const aprend_lit_light &l = lights[i];
		aprend_lit_light_block &o = b.lights[i];
		memcpy(o.position_range, l.position, sizeof(float) * 3);
		o.position_range[3] = l.range;
		memcpy(o.radiance_type, l.radiance, sizeof(float) * 3);
		o.radiance_type[3] = l.type == APREND_LIT_LIGHT_TYPE_SPOT ? 1.0f : 0.0f;
		unit3(l.direction, o.direction_inner);
		o.direction_inner[3] = std::cos(l.inner_angle);
		o.outer_shadow[0]    = std::cos(l.outer_angle);
	}
	b.light_count = count;
	aprend_lit_scene_commit(scene);
	return true;
}

bool aprend_lit_scene_set_spot_shadows(aprend_lit_scene scene, aprend_texture_view map, const aprend_lit_spot_shadow *shadows, uint32_t count,
                                       bool reversed_depth) {
	static const char who[] = "aprend_lit_scene_set_spot_shadows";
	if (!scene) {
		printf("apricot: %s: scene is NULL\n", who);
		return false;
	}
	if (count > APREND_LIT_MAX_SPOT_SHADOWS) {
		printf("apricot: %s: %u spot shadows, and a scene holds %u\n", who, count, APREND_LIT_MAX_SPOT_SHADOWS);
		return false;
	}
	if (count > 0 && !shadows) {
		printf("apricot: %s: shadows is NULL with a count of %u\n", who, count);
		return false;
	}
	if (count > 0 && !aprend_lit_shadow_map_valid(scene, map, count, who))
		return false;
	for (uint32_t i = 0; i < count; ++i) {
		const aprend_lit_spot_shadow &s = shadows[i];
		if (!all_finite(s.view_projection, 16) || !aprend_lit_shadow_params_valid(s.depth_bias, s.normal_offset, s.softness, s.strength, who))
			return false;
		for (uint32_t j = 0; j < i; ++j) {
			if (shadows[j].light == s.light) {
				printf("apricot: %s: shadows %u and %u name one light, %u\n", who, j, i, s.light);
				return false;
			}
		}
	}
	const aprend_lit_builtins &lit = scene->instance->builtins.lit;
	if (!aprend_material_set_texture(scene->material, "spot_shadow_map", 0, count ? map : lit.placeholder_depth_view))
		return false;
	aprend_lit_scene_block &b = scene->block;
	for (uint32_t i = 0; i < APREND_LIT_MAX_SPOT_SHADOWS; ++i) {
		if (i < count) {
			const aprend_lit_spot_shadow &s = shadows[i];
			memcpy(b.spot_shadow_view_projection[i], s.view_projection, sizeof(float) * 16);
			b.spot_shadow_params[i][0]   = s.depth_bias;
			b.spot_shadow_params[i][1]   = s.normal_offset;
			b.spot_shadow_params[i][2]   = s.softness;
			b.spot_shadow_params[i][3]   = s.strength;
			scene->spot_shadow_lights[i] = s.light;
		} else {
			memset(b.spot_shadow_view_projection[i], 0, sizeof(float) * 16);
			memset(b.spot_shadow_params[i], 0, sizeof(float) * 4);
		}
		aprend_lit_shadow_pass_block pass{};
		memcpy(pass.view_projection, b.spot_shadow_view_projection[i], sizeof(pass.view_projection));
		aprend_lit_write_block(scene->spot_shadow_materials[i], "u_lit_shadow_pass", &pass, sizeof(pass));
	}
	scene->spot_shadow_count = count;
	if (reversed_depth)
		b.flags |= APREND_LIT_SCENE_FLAG_SPOT_REVERSED;
	else
		b.flags &= ~APREND_LIT_SCENE_FLAG_SPOT_REVERSED;
	aprend_lit_scene_commit(scene);
	return true;
}

bool aprend_lit_scene_set_ambient(aprend_lit_scene scene, const aprend_lit_ambient *ambient) {
	static const char who[] = "aprend_lit_scene_set_ambient";
	if (!scene || !ambient) {
		printf("apricot: %s: %s is NULL\n", who, scene ? "ambient" : "scene");
		return false;
	}
	const aprend_lit_ambient &a = *ambient;
	if (!all_finite(a.up, 3) || !all_finite(a.sky, 3) || !all_finite(a.ground, 3) || !finite(a.environment_intensity) ||
	    !all_finite(a.environment_rotation, 9)) {
		printf("apricot: %s: a value is not a finite number\n", who);
		return false;
	}
	bool colored = false;
	for (int i = 0; i < 3; ++i) {
		if (a.sky[i] < 0.0f || a.ground[i] < 0.0f) {
			printf("apricot: %s: a color is negative\n", who);
			return false;
		}
		colored = colored || a.sky[i] > 0.0f || a.ground[i] > 0.0f;
	}
	if (a.environment_intensity < 0.0f) {
		printf("apricot: %s: environment_intensity is negative\n", who);
		return false;
	}
	if (colored && length3(a.up) == 0.0f) {
		printf("apricot: %s: the hemisphere has colors and no up\n", who);
		return false;
	}
	if (a.environment && a.environment->instance != scene->instance) {
		printf("apricot: %s: environment is of another instance\n", who);
		return false;
	}
	const aprend_lit_builtins &lit = scene->instance->builtins.lit;
	aprend_texture_view specular   = a.environment ? a.environment->specular_cube : lit.placeholder_cube_view;
	aprend_texture_view irradiance = a.environment ? a.environment->irradiance_cube : lit.placeholder_cube_view;
	if (!aprend_material_set_texture(scene->material, "environment_specular", 0, specular) ||
	    !aprend_material_set_texture(scene->material, "environment_irradiance", 0, irradiance))
		return false;
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
	return true;
}

bool aprend_lit_scene_set_ambient_occlusion(aprend_lit_scene scene, aprend_texture_view occlusion, float strength) {
	static const char who[] = "aprend_lit_scene_set_ambient_occlusion";
	if (!scene) {
		printf("apricot: %s: scene is NULL\n", who);
		return false;
	}
	if (!finite(strength) || strength < 0.0f || strength > 1.0f) {
		printf("apricot: %s: strength is not from 0 to 1\n", who);
		return false;
	}
	if (occlusion && (occlusion->instance != scene->instance || occlusion->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE ||
	                  occlusion->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D || occlusion->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR)) {
		printf("apricot: %s: occlusion is not a SHADER_RESOURCE 2D view of color of the scene's instance\n", who);
		return false;
	}
	if (!aprend_material_set_texture(scene->material, "ambient_occlusion", 0,
	                                 occlusion ? occlusion : scene->instance->builtins.lit.placeholder_2d_view))
		return false;
	aprend_lit_scene_block &b = scene->block;
	b.occlusion_strength      = strength;
	if (occlusion)
		b.flags |= APREND_LIT_SCENE_FLAG_OCCLUSION;
	else
		b.flags &= ~APREND_LIT_SCENE_FLAG_OCCLUSION;
	aprend_lit_scene_commit(scene);
	return true;
}

// ---- Fitting shadows --------------------------------------------------------------

bool aprend_lit_fit_sun_cascades(const aprend_lit_cascade_fit *fit, aprend_lit_sun_shadow *out_shadow) {
	static const char who[] = "aprend_lit_fit_sun_cascades";
	if (!fit || !out_shadow) {
		printf("apricot: %s: %s is NULL\n", who, fit ? "out_shadow" : "fit");
		return false;
	}
	const aprend_lit_cascade_fit &f = *fit;
	if (!all_finite(f.eye_position, 3) || !all_finite(f.view_direction, 3) || !all_finite(f.view_up, 3) || !all_finite(f.sun_direction, 3) ||
	    !finite(f.fov_y) || !finite(f.half_height) || !finite(f.aspect) || !finite(f.near_depth) || !finite(f.far_depth) ||
	    !finite(f.split_lambda) || !finite(f.caster_distance)) {
		printf("apricot: %s: a value is not a finite number\n", who);
		return false;
	}
	const glm::vec3 forward_in = vec3_of(f.view_direction), up_in = vec3_of(f.view_up), sun_in = vec3_of(f.sun_direction);
	if (glm::length(forward_in) == 0.0f || glm::length(sun_in) == 0.0f) {
		printf("apricot: %s: the view direction or the sun direction is 0\n", who);
		return false;
	}
	const glm::vec3 forward = glm::normalize(forward_in);
	glm::vec3 right         = glm::cross(forward, up_in);
	if (glm::length(right) <= 1e-6f * std::max(glm::length(up_in), 1e-30f)) {
		printf("apricot: %s: view_up runs along the view direction\n", who);
		return false;
	}
	right              = glm::normalize(right);
	const glm::vec3 up = glm::cross(right, forward);
	if (f.cascade_count == 0 || f.cascade_count > APREND_LIT_MAX_CASCADES || f.map_size == 0) {
		printf("apricot: %s: %u cascades and a map of %u: 1 to %u, and above 0\n", who, f.cascade_count, APREND_LIT_MAX_CASCADES, f.map_size);
		return false;
	}
	if (!(f.far_depth > f.near_depth) || (!f.orthographic && !(f.near_depth > 0.0f)) || !(f.aspect > 0.0f) || f.split_lambda < 0.0f ||
	    f.split_lambda > 1.0f || f.caster_distance < 0.0f || (f.orthographic ? !(f.half_height > 0.0f) : !(f.fov_y > 0.0f && f.fov_y < 3.1f))) {
		printf("apricot: %s: the depths, the field of view or half height, the aspect, the split lambda or the caster distance is out of range\n",
		       who);
		return false;
	}

	const glm::vec3 eye = vec3_of(f.eye_position);
	const glm::vec3 sun = glm::normalize(sun_in);
	const float n = f.near_depth, fa = f.far_depth;
	float start = n;
	for (uint32_t c = 0; c < f.cascade_count; ++c) {
		// Zhang's practical split: a mix of even and logarithmic.
		const float t     = (float)(c + 1) / (float)f.cascade_count;
		const float even  = n + (fa - n) * t;
		const float ratio = n > 0.0f ? n * std::pow(fa / n, t) : even;
		const float end   = c + 1 == f.cascade_count ? fa : f.split_lambda * ratio + (1.0f - f.split_lambda) * even;
		// The stretch's eight corners, and the sphere round them: its
		// radius depends on the stretch's shape alone, so it doesn't change
		// as the view turns.
		glm::vec3 corners[8];
		const float depths[2] = {start, end};
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

		// Looking down the sun's direction at the sphere, from far enough
		// out to catch what stands between it and the sun.
		const glm::vec3 light_up = std::fabs(sun.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
		const float back         = radius + f.caster_distance;
		const glm::mat4 view     = glm::lookAtRH(centre + sun * back, centre, light_up);
		const float depth        = back + radius;
		glm::mat4 projection     = f.reversed_depth ? glm::orthoRH_ZO(-radius, radius, -radius, radius, depth, 0.0f)
		                                            : glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, depth);
		// Moved by whole texels: where the scene's origin lands, rounded.
		glm::mat4 matrix        = projection * view;
		const float half_texels = (float)f.map_size * 0.5f;
		const glm::vec4 origin  = matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		const glm::vec2 at      = glm::vec2(origin.x, origin.y) * half_texels;
		const glm::vec2 offset  = (glm::round(at) - at) / half_texels;
		projection[3][0] += offset.x;
		projection[3][1] += offset.y;
		matrix = projection * view;
		memcpy(out_shadow->view_projection[c], &matrix, sizeof(float) * 16);
		out_shadow->split_depths[c] = end;
		start                       = end;
	}
	for (uint32_t c = f.cascade_count; c < APREND_LIT_MAX_CASCADES; ++c) {
		memset(out_shadow->view_projection[c], 0, sizeof(float) * 16);
		out_shadow->split_depths[c] = 0.0f;
	}
	out_shadow->cascade_count = f.cascade_count;
	return true;
}

bool aprend_lit_spot_shadow_view_projection(const aprend_lit_light *light, float near_depth, bool reversed_depth, float out_view_projection[16]) {
	static const char who[] = "aprend_lit_spot_shadow_view_projection";
	if (!light || !out_view_projection) {
		printf("apricot: %s: %s is NULL\n", who, light ? "out_view_projection" : "light");
		return false;
	}
	if (light->type != APREND_LIT_LIGHT_TYPE_SPOT) {
		printf("apricot: %s: the light is not a spot\n", who);
		return false;
	}
	if (!aprend_lit_light_valid(*light, 0, who))
		return false;
	if (!finite(near_depth) || !(near_depth > 0.0f) || !(near_depth < light->range)) {
		printf("apricot: %s: near_depth is not between 0 and the light's range\n", who);
		return false;
	}
	const glm::vec3 position = vec3_of(light->position), axis = glm::normalize(vec3_of(light->direction));
	const glm::vec3 up       = std::fabs(axis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
	// Wide enough for the cone, with a margin the softness reads into.
	const float fov            = std::min(light->outer_angle * 2.0f * 1.05f, 3.1f);
	const glm::mat4 projection = reversed_depth ? glm::perspectiveRH_ZO(fov, 1.0f, light->range, near_depth)
	                                            : glm::perspectiveRH_ZO(fov, 1.0f, near_depth, light->range);
	const glm::mat4 matrix     = projection * glm::lookAtRH(position, position + axis, up);
	memcpy(out_view_projection, &matrix, sizeof(float) * 16);
	return true;
}

} // extern "C"

// ---- Exposures ----------------------------------------------------------------------

aprend_exposure_t::~aprend_exposure_t() {
	for (int i = 0; i < 2; ++i) {
		aprend_destroy_texture_view(this->value_views[i]);
		aprend_destroy_texture_view(this->value_targets[i]);
		aprend_texture2d_destroy(this->values[i]);
	}
	aprend_destroy_texture_view(this->luminance_view);
	aprend_destroy_texture_view(this->luminance_target);
	aprend_texture2d_destroy(this->luminance);
}

extern "C" {

aprend_exposure aprend_exposure_create(aprend_instance instance, const aprend_exposure_desc *desc) {
	static const char who[] = "aprend_exposure_create";
	if (!instance || !desc) {
		printf("apricot: %s: %s is NULL\n", who, instance ? "desc" : "instance");
		return nullptr;
	}
	const aprend_exposure_desc &d = *desc;
	if (!finite(d.key) || !finite(d.min_log) || !finite(d.max_log) || !finite(d.up_rate) || !finite(d.down_rate) || !(d.key > 0.0f) ||
	    !(d.min_log < d.max_log) || !(d.up_rate > 0.0f) || !(d.down_rate > 0.0f)) {
		printf("apricot: %s: the key and rates are above 0, and min_log below max_log, all finite\n", who);
		return nullptr;
	}
	APREND_MALLOC__T(exposure, aprend_exposure_t);
	if (!exposure) {
		printf("apricot: %s: out of memory\n", who);
		return nullptr;
	}
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
	if (!ok) {
		printf("apricot: %s: a texture or view can't be created\n", who);
		APREND_DESTRUCT__T(exposure, aprend_exposure_t);
		return nullptr;
	}
	return exposure;
}

void aprend_exposure_destroy(aprend_exposure exposure) {
	if (!exposure)
		return;
	APREND_DESTRUCT__T(exposure, aprend_exposure_t);
}

} // extern "C"
