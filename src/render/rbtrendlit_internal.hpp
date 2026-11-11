#ifndef RBTREND_LIT_INTERNAL_HPP
#define RBTREND_LIT_INTERNAL_HPP

/* Internal header - never included outside rabbittus/src/render/.
 *
 * Rend's lit surfaces (rbtrendlit.h) and adapting exposure (rbtrendpost.h):
 * the blocks of their shaders as C++ structs, the instance's objects for
 * them, and what a scene, an environment and an exposure hold. */

#include "render/rbtrendlit.h"
#include "render/rbtrendpost.h"
#include "../rbtplaindata.hpp"
#include "../rbtstale.hpp"

#include <cstdint>
#include <vector>

/* ---- The shaders' blocks ----------------------------------------------- */

/* RbtrendLitLight in shaders/rbtrend_lit_scene.slang: a point or spot light. */
struct rbtrend_lit_light_block {
	float position_range[4];
	float radiance_type[4];
	float direction_inner[4];
	/* x the cos of a spot's outer angle; y its shadow in shadows[], or -1. */
	float outer_shadow[4];
};

/* RbtrendLitDirectional: a directional light. */
struct rbtrend_lit_directional_block {
	/* xyz unit, towards the light; w 1 - cos of its radius. */
	float direction_versine[4];
	/* rgb its radiance; w 1 if the sky draws its disc. */
	float radiance_disc[4];
	float splits[4];
	/* bias, normal offset, softness, strength. */
	float shadow_params[4];
	/* x its shadow's kind (RBTREND_LIT_DIRECTIONAL_*), y its first view, z
	 * how many. */
	uint32_t shadow_views[4];
};

/* RbtrendLitDirectional's kinds of shadow, as the shader has them. */
enum : uint32_t {
	RBTREND_LIT_DIRECTIONAL_NO_SHADOW = 0u,
	RBTREND_LIT_DIRECTIONAL_MASK      = 1u,
	RBTREND_LIT_DIRECTIONAL_CASCADES  = 2u,
};

/* RbtrendLitShadow: a point or spot light's map shadow. */
struct rbtrend_lit_shadow_block {
	/* bias, normal offset, softness, strength. */
	float params[4];
	/* x its first view, y how many. */
	uint32_t views[4];
};

/* RbtrendLitShadowView: one view of a shadow, its rectangle in texels. */
struct rbtrend_lit_shadow_view_block {
	float view_projection[16];
	float rect[4];
};

/* RbtrendLitScene in shaders/rbtrend_lit_scene.slang, as std140 lays it out.
 * Checked member by member against the shader's reflection when the scene
 * layout is made. */
struct rbtrend_lit_scene_block {
	float view_projection[16];
	float inverse_view_projection[16];
	rbtrend_lit_shadow_view_block shadow_views[RBTREND_LIT_MAX_SHADOW_VIEWS];
	rbtrend_lit_directional_block directional[RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS];
	rbtrend_lit_shadow_block shadows[RBTREND_LIT_MAX_SHADOW_VIEWS];
	float eye_position[4];
	float view_direction[4];
	float ambient_up[4];
	float ambient_sky[4];
	float ambient_ground[4];
	float environment_rotation[12];
	float target_size[4];
	float shadow_atlas_size[4];
	float environment_intensity;
	float environment_mips;
	float exposure;
	uint32_t output_encoding;
	uint32_t flags;
	/* Point and spot lights. */
	uint32_t light_count;
	uint32_t directional_count;
	uint32_t tile_size;
	uint32_t tiles_x;
	uint32_t tiles_y;
	float occlusion_strength;
	float scene_color_mips;
	rbtrend_lit_light_block lights[RBTREND_LIT_MAX_LIGHTS];
};

/* Bits of rbtrend_lit_scene_block::flags, as the shader has them. */
enum : uint32_t {
	RBTREND_LIT_SCENE_FLAG_SHADOW_ATLAS    = 1u,
	RBTREND_LIT_SCENE_FLAG_SHADOW_REVERSED = 2u,
	RBTREND_LIT_SCENE_FLAG_ENVIRONMENT     = 4u,
	RBTREND_LIT_SCENE_FLAG_TILES           = 8u,
	RBTREND_LIT_SCENE_FLAG_OCCLUSION       = 16u,
	RBTREND_LIT_SCENE_FLAG_BENT_NORMALS    = 64u,
	RBTREND_LIT_SCENE_FLAG_SCENE_COLOR     = 256u,
};

/* RbtrendLitShadowPass in shaders/rbtrend_lit_shadow_pass.slang. */
struct rbtrend_lit_shadow_pass_block {
	float view_projection[16];
};

/* RbtrendLitMaterial in shaders/rbtrend_lit_material.slang. */
struct rbtrend_lit_material_block {
	float base_color[4];
	float emission[4];
	float sheen[4];
	float roughness;
	float metalness;
	float specular;
	float normal_scale;
	float occlusion_strength;
	float alpha_cutoff;
	float clearcoat;
	float clearcoat_roughness;
	uint32_t maps;
	float padding[3];
	float uv_transform[8];
	/* Each map's own, after uv_transform: two rows (vec4s) a map, in
	 * RBTREND_LIT_MAP order. */
	float map_uv_transform[4][8];
	/* x transmission, y index of refraction, z thickness, w attenuation
	 * distance (0: never). */
	float transmission[4];
	float attenuation[4];
};

/* RbtrendOcclusionPass in shaders/rbtrend_lit_occlusion_frag.slang. */
struct rbtrend_occlusion_block {
	float view_projection[16];
	float inverse_view_projection[16];
	float target_size[4];
	float eye_position[4];
	float params[4];
};

/* EnvironmentParams in shaders/rbtrend_environment.slang. Metal aligns a
 * block with a vec2 to 8 bytes: every push here is a multiple of 8. */
struct rbtrend_environment_params {
	float target_size[2];
	uint32_t face;
	uint32_t sample_count;
	float roughness;
	float source_size;
	float source_mips;
	float padding;
};

/* SkyParams in shaders/rbtrend_environment_sky_frag.slang. */
struct rbtrend_sky_params {
	float target_size[2];
	uint32_t face;
	float turbidity;
	float sun[4];
	float ground[4];
};

/* RbtrendHosekSky in shaders/rbtrend_environment_hosek_frag.slang, std140: each
 * channel's nine coefficients (in the dataset's order) and radiance, in a
 * vec4's red, green and blue. */
struct rbtrend_hosek_sky_block {
	float config[9][4];
	float radiance[4];
};

/* Cooks Hosek and Wilkie's RGB sky for a sun towards [sun] (unit, +Y up; a
 * sun below the horizon is taken at it), the air's [turbidity] (1 to 10) and
 * the ground's [albedo] (each 0 to 1), from the authors' dataset, its
 * radiance scaled so the zenith's luminance is [intensity]. */
void rbtrend_lit_hosek_cook(const float sun[3], float turbidity, const float albedo[3], float intensity, rbtrend_hosek_sky_block *out);

/* The material of [environment]'s Hosek-Wilkie coefficients, made if it
 * isn't (the passes prepared). NULL, reported, if it can't be. */
rbtrend_material rbtrend_lit_environment_sky_material(rbtrend_lit_environment environment);

/* ToneMapParams in shaders/rbtrend_tone_map_frag.slang. */
struct rbtrend_tone_map_params {
	float target_size[2];
	float exposure;
	uint32_t curve;
	uint32_t encoding;
	uint32_t automatic;
};

/* AdaptParams in shaders/rbtrend_exposure_adapt_frag.slang. */
struct rbtrend_adapt_params {
	float key;
	float min_log;
	float max_log;
	float up_rate;
	float down_rate;
	float delta_time;
	float last_mip;
	uint32_t first;
};

/* HistogramParams in shaders/rbtrend_exposure_histogram_comp.slang. */
struct rbtrend_histogram_params {
	float min_log;
	float max_log;
	uint32_t clear;
	uint32_t size;
};

/* AdaptParams in shaders/rbtrend_exposure_adapt_histogram_frag.slang, 48 bytes. */
struct rbtrend_histogram_adapt_params {
	float key;
	float min_log;
	float max_log;
	float up_rate;
	float down_rate;
	float delta_time;
	float low;
	float high;
	uint32_t first;
	uint32_t unused[3];
};

/* The bins of an exposure's histogram. */
#define RBTREND_EXPOSURE_BINS 64u

/* The push of the passes that take only their target's size. */
struct rbtrend_size_params {
	float target_size[2];
};

/* HizParams in shaders/rbtrend_lit_hiz_frag.slang: level 0 of LIT_SSR's
 * pyramid, from a depth buffer near at 1 when [reversed]. */
struct rbtrend_hiz_params {
	float target_size[2];
	uint32_t reversed;
	uint32_t padding;
};

/* SsrParams in shaders/rbtrend_lit_ssr_comp.slang. */
struct rbtrend_ssr_params {
	uint32_t size[2];
	uint32_t full[2];
	uint32_t max_steps;
	float max_distance;
	float thickness;
	float edge_fade;
	float max_roughness;
	uint32_t hiz_levels;
	uint32_t reversed;
	float color_mips;
};

/* ReflectParams in shaders/rbtrend_lit_reflect_frag.slang. */
struct rbtrend_reflect_params {
	float target_size[2];
	uint32_t reversed;
	uint32_t padding;
};

/* Whether [scene]'s view renders depth near at 1: its view_projection's
 * inverse takes depth 1 nearer the eye than depth 0. A view with no inverse
 * is near at 0. */
bool rbtrend_lit_scene_depth_reversed(rbtrend_lit_scene scene);

/* ---- The instance's lit objects ---------------------------------------- */

/* The full-target passes Rend's lit and post work is made of, each a
 * fragment shader over rbtrend_fullscreen_vert.slang. */
enum {
	RBTREND_LIT_PASS_FROM_CUBE     = 0, /* a picture's cube into a radiance cube face */
	RBTREND_LIT_PASS_FROM_EQUIRECT = 1, /* an equirectangular picture likewise */
	RBTREND_LIT_PASS_PREFILTER     = 2, /* a specular chain mip face */
	RBTREND_LIT_PASS_IRRADIANCE    = 3, /* an irradiance cube face */
	RBTREND_LIT_PASS_SKY           = 4, /* Preetham's sky into a radiance cube face */
	RBTREND_LIT_PASS_BRDF_LUT      = 5, /* the BRDF lookup texture */
	RBTREND_LIT_PASS_OCCLUSION     = 6, /* raw ambient occlusion */
	RBTREND_LIT_PASS_BLUR          = 7, /* ambient occlusion smoothed */
	RBTREND_LIT_PASS_LUMINANCE     = 8, /* log luminance for an exposure */
	RBTREND_LIT_PASS_ADAPT         = 9, /* an exposure adapted */
	RBTREND_LIT_PASS_HOSEK_SKY     = 10, /* Hosek and Wilkie's sky into a radiance cube face */
	RBTREND_LIT_PASS_ADAPT_HISTOGRAM = 11, /* an exposure adapted from a histogram */
	RBTREND_LIT_PASS_HIZ           = 12, /* LIT_SSR's pyramid, level 0 from a depth buffer */
	RBTREND_LIT_PASS_HIZ_DOWN      = 13, /* a level of it from the level above */
	RBTREND_LIT_PASS_COUNT         = 14,
};

/* The format of every texture of an environment. */
#define RBTREND_ENVIRONMENT_FORMAT CRGPU_FORMAT_R16G16B16A16_FLOAT
/* The BRDF lookup texture's size and format. */
#define RBTREND_BRDF_LUT_SIZE 128u
#define RBTREND_BRDF_LUT_FORMAT CRGPU_FORMAT_R16G16_FLOAT
/* An exposure's luminance texture: its side, a power of 2, and format; and
 * the format of its adapted value. */
#define RBTREND_EXPOSURE_LUMINANCE_SIZE 256u
#define RBTREND_EXPOSURE_LUMINANCE_FORMAT CRGPU_FORMAT_R16_FLOAT
#define RBTREND_EXPOSURE_FORMAT CRGPU_FORMAT_R32_FLOAT
/* Raw ambient occlusion, before the blur: the openness and the bent normal. */
#define RBTREND_OCCLUSION_FORMAT CRGPU_FORMAT_R16G16B16A16_FLOAT
/* LIT_SSR's `_max_steps` is at most this (rbtrendcommands.h). */
#define RBTREND_SSR_MAX_STEPS 256u
/* LIT_SSR's Hi-Z pyramid: each texel's nearness (rbtrend_lit_hiz_frag.slang). */
#define RBTREND_HIZ_FORMAT CRGPU_FORMAT_R32_FLOAT
/* The reflection target and the reflecting surface's normal and roughness,
 * and its specular weight (rbtrendlit.h, rbtrendcommands.h). */
#define RBTREND_REFLECTION_FORMAT CRGPU_FORMAT_R16G16B16A16_FLOAT
#define RBTREND_SPECULAR_FORMAT CRGPU_FORMAT_R8G8B8A8_UNORM

/* A binding set Rend makes over views a command names, kept for the next
 * compile that names the same ones: the layout it is of, and the views and
 * their serials it holds (a view destroyed and another made at its address
 * has another serial). One of a fixed number, the least recently handed
 * out made again when none matches, so a frame that names the views it
 * named before makes none. */
#define RBTREND_VIEW_SET_VIEWS 6u
#define RBTREND_VIEW_SETS 16u
struct rbtrend_view_set {
	rbtrend_binding_set set;
	const void *layout;
	const void *views[RBTREND_VIEW_SET_VIEWS];
	uint64_t serials[RBTREND_VIEW_SET_VIEWS];
	/* The instance's frame_number when last handed out. */
	uint64_t used;
};
RBT_PLAIN_DATA(rbtrend_view_set);

/* The set of [layout] holding [entries] over [views] (at most
 * RBTREND_VIEW_SET_VIEWS, which the entries name), from [sets] if one
 * matches, else made into the least recently used. NULL, reported naming
 * [what], if it can't be made. */
rbtrend_binding_set rbtrend_view_set_get(rbtrend_instance instance, rbtrend_view_set (&sets)[RBTREND_VIEW_SETS], rbtrend_binding_layout layout,
                                       const rbtrend_texture_view *views, uint32_t view_count, const RBTREND_BINDING_SET_ENTRY *entries,
                                       uint32_t entry_count, const char *what);

/* Destroys every set of [sets]. */
void rbtrend_view_sets_destroy(rbtrend_view_set (&sets)[RBTREND_VIEW_SETS]);

/* What an instance makes for lit surfaces, each NULL until first needed
 * (rbtrend_builtins::lit). */
struct rbtrend_lit_builtins {
	/* rbtrend_lit_get_shader()'s, by RBTREND_LIT_SHADER. */
	rbtrend_shader shaders[9]{};
	rbtrend_binding_layout scene_layout{nullptr};
	rbtrend_binding_layout shadow_layout{nullptr};
	rbtrend_binding_layout material_layout{nullptr};
	/* Set 0 of the light culling shader: the scene's block, and its tiles. */
	rbtrend_shader cull_comp{nullptr};
	rbtrend_binding_layout cull_layout{nullptr};
	rbtrend_compute_pipeline cull_pipeline{nullptr};
	/* Culling with depth bounds (RBTREND_COMMAND_LIT_CULL_LIGHTS): set 0 the
	 * cull layout, set 1 a depth view's environment set (the pass layout). */
	rbtrend_shader cull_bounded_comp{nullptr};
	rbtrend_binding_layout cull_depth_layout{nullptr};
	rbtrend_compute_pipeline cull_bounded_pipeline{nullptr};
	/* Set 0 of the occlusion pass: its block. Its set 1 is a view's
	 * environment set (a texture and a sampler). */
	rbtrend_binding_layout occlusion_layout{nullptr};
	/* Set 0 of the Hosek-Wilkie sky pass: its cooked coefficients
	 * (rbtrend_hosek_sky_block). */
	rbtrend_binding_layout hosek_layout{nullptr};
	/* An exposure's histogram: the compute shader that counts it, the
	 * layout of set 0 it shares with the pass that adapts from it (the
	 * luminance texture, a sampler, the bins), and its pipeline. */
	rbtrend_shader histogram_comp{nullptr};
	rbtrend_binding_layout histogram_layout{nullptr};
	rbtrend_compute_pipeline histogram_pipeline{nullptr};
	/* What an empty binding holds: a placeholder of the binding's shape,
	 * never written and never read (a flag in the block says the binding is
	 * empty). A depth one for the shadow atlas, which a comparison sampler
	 * reads; a small storage buffer for the tiles. */
	rbtrend_texture2d placeholder_2d{nullptr};
	rbtrend_texture_view placeholder_2d_view{nullptr};
	rbtrend_texture2d placeholder_cube{nullptr};
	rbtrend_texture_view placeholder_cube_view{nullptr};
	rbtrend_texture2d placeholder_depth{nullptr};
	rbtrend_texture_view placeholder_depth_view{nullptr};
	rbtrend_buffer placeholder_tiles{nullptr};
	/* A material's maps, read linear, mipmapped and repeating. */
	rbtrend_sampler material_sampler{nullptr};
	/* Environments, lookup textures and depth, linear, mipmapped, clamped. */
	rbtrend_sampler environment_sampler{nullptr};
	/* The shadow maps' comparisons: reference <= texel, and >= for maps
	 * rendered with depth reversed. */
	rbtrend_sampler shadow_sampler{nullptr};
	rbtrend_sampler shadow_sampler_reversed{nullptr};
	/* The BRDF lookup texture, made by the first compile that binds a lit
	 * scene, and its render target view. */
	rbtrend_texture2d brdf_lut{nullptr};
	rbtrend_texture_view brdf_lut_view{nullptr};
	rbtrend_texture_view brdf_lut_target{nullptr};

	/* The full-target passes: fragment shaders by RBTREND_LIT_PASS_, the
	 * layout of their set 0 (a texture at 0, a sampler at 1; the occlusion
	 * pass's own layout instead), and a pipeline each, rendering to the
	 * format of what they make. */
	rbtrend_shader pass_frags[RBTREND_LIT_PASS_COUNT]{};
	rbtrend_binding_layout pass_layout{nullptr};
	rbtrend_graphics_pipeline pass_pipelines[RBTREND_LIT_PASS_COUNT]{};
	/* The occlusion blur renders to the caller's format: one for each. */
	struct format_pipeline {
		CRGPU_FORMAT format;
		rbtrend_graphics_pipeline pipeline;
	};
	std::vector<format_pipeline> blur_pipelines{};

	/* RBTREND_COMMAND_TONE_MAP's shader, its layout, and a pipeline for each
	 * target format tone mapped onto. */
	rbtrend_shader tone_map_frag{nullptr};
	rbtrend_binding_layout tone_map_layout{nullptr};
	std::vector<format_pipeline> tone_map_pipelines{};

	/* RBTREND_COMMAND_LIT_SSR's march: its shader, the layout of its set 0
	 * (the scene's block alone; rbtrend_lit_scene_t::ssr_set) and set 1 (the
	 * targets it reads and writes), and its pipeline. */
	rbtrend_shader ssr_comp{nullptr};
	rbtrend_binding_layout ssr_scene_layout{nullptr};
	rbtrend_binding_layout ssr_input_layout{nullptr};
	rbtrend_compute_pipeline ssr_pipeline{nullptr};
	/* RBTREND_COMMAND_LIT_REFLECT's composite: its shader, the layout of its
	 * set 1 (set 0 is the scene's own), and a pipeline for each target
	 * format, blending additively. */
	rbtrend_shader reflect_frag{nullptr};
	rbtrend_binding_layout reflect_input_layout{nullptr};
	std::vector<format_pipeline> reflect_pipelines{};
	/* The sets of the two layouts over the commands' views. */
	rbtrend_view_set view_sets[RBTREND_VIEW_SETS]{};
};

/* Destroys what rbtrend_lit_builtins holds, pipelines before layouts, from
 * rbtrend_builtins_destroy(). */
void rbtrend_lit_builtins_destroy(rbtrend_instance instance);

/* The placeholders and samplers, made if they aren't. False, reported, if one
 * can't be. */
bool rbtrend_lit_prepare_resources(rbtrend_instance instance);

/* The pass of [pass], made if it isn't. NULL, reported, if it can't be. */
rbtrend_graphics_pipeline rbtrend_lit_pass_pipeline(rbtrend_instance instance, uint32_t pass);

/* The occlusion blur rendering to [format], made if it isn't. */
rbtrend_graphics_pipeline rbtrend_lit_blur_pipeline(rbtrend_instance instance, CRGPU_FORMAT format);

/* The tone map pipeline rendering to [format], made if it isn't. NULL,
 * reported, if it can't be. */
rbtrend_graphics_pipeline rbtrend_lit_tone_map_pipeline(rbtrend_instance instance, CRGPU_FORMAT format);

/* LIT_SSR's compute pipeline, made if it isn't (its layouts with it). NULL,
 * reported, if it can't be. */
rbtrend_compute_pipeline rbtrend_lit_ssr_pipeline(rbtrend_instance instance);

/* The set of LIT_SSR's set 0 for [scene]: its block alone, made by the
 * first march of it (rbtrend_lit_scene_t::ssr_set). The pipeline has been
 * made. NULL, reported, if it can't be. */
rbtrend_binding_set rbtrend_lit_scene_ssr_set(rbtrend_lit_scene scene);

/* LIT_REFLECT's pipeline rendering onto [format], made if it isn't (its
 * layout with it). NULL, reported, if it can't be. */
rbtrend_graphics_pipeline rbtrend_lit_reflect_pipeline(rbtrend_instance instance, CRGPU_FORMAT format);

/* The exposure histogram's compute pipeline, made if it isn't (the passes
 * prepared). NULL, reported, if it can't be. */
rbtrend_compute_pipeline rbtrend_lit_histogram_pipeline(rbtrend_instance instance);

/* The light culling pipeline, made if it isn't. */
rbtrend_compute_pipeline rbtrend_lit_cull_pipeline(rbtrend_instance instance);

/* The light culling pipeline with depth bounds, made if it isn't (the
 * passes prepared). NULL, reported, if it can't be. */
rbtrend_compute_pipeline rbtrend_lit_cull_bounded_pipeline(rbtrend_instance instance);

/* The BRDF lookup texture and its views, made if they aren't. */
bool rbtrend_lit_prepare_brdf_lut(rbtrend_instance instance);

/* The set of the pass layout reading [view] through the environment
 * sampler, kept by the view (rbtrend_texture_view_t::environment_set). NULL,
 * reported, if it can't be made. */
rbtrend_binding_set rbtrend_texture_view_environment_set(rbtrend_texture_view view);

/* The set of the bounded culling's depth layout (its set 1) reading [view]
 * through the environment sampler, kept by the view
 * (rbtrend_texture_view_t::cull_depth_set). The bounded culling pipeline has
 * been made. NULL, reported, if it can't be made. */
rbtrend_binding_set rbtrend_texture_view_cull_depth_set(rbtrend_texture_view view);

/* ---- Environments, scenes and exposures --------------------------------- */

typedef struct rbtrend_lit_environment_t {
	char *debug_name{nullptr}; /* first, as every handle's: rbt_debug_name_set() writes it (rbtcore.h) */
	rbtrend_lit_environment_t() = default;
	~rbtrend_lit_environment_t();
	rbtrend_instance instance{nullptr};
	RBTREND_LIT_ENVIRONMENT_DESC desc{};
	/* The picture, copied in at specular_size with every mip down to 1
	 * texel: what the prefilter and irradiance passes sample. Its faces'
	 * first mip as render targets, and the whole cube as one view. */
	rbtrend_texture2d radiance{nullptr};
	rbtrend_texture_view radiance_faces[6]{};
	rbtrend_texture_view radiance_cube{nullptr};
	/* The specular chain: specular_mips mips down to 8 texels, each face of
	 * each mip a render target (mip after mip, face after face), and the
	 * whole as one cube view for the surface shader. */
	rbtrend_texture2d specular{nullptr};
	uint32_t specular_mips{0};
	std::vector<rbtrend_texture_view> specular_faces{};
	rbtrend_texture_view specular_cube{nullptr};
	/* The irradiance cube, likewise. */
	rbtrend_texture2d irradiance{nullptr};
	rbtrend_texture_view irradiance_faces[6]{};
	rbtrend_texture_view irradiance_cube{nullptr};
	/* Of the Hosek-Wilkie layout: the coefficients the last
	 * RBTREND_COMMAND_LIT_SKY of that model cooked, made by the first. */
	rbtrend_material sky_material{nullptr};
	RBT_STALE_TAG
} rbtrend_lit_environment_t;

/* What light [i] of a scene's list is: its type, and its place among the
 * block's directional lights or its point and spot lights. */
struct rbtrend_lit_light_entry {
	uint8_t type;
	uint8_t slot;
};
RBT_PLAIN_DATA(rbtrend_lit_light_entry);

/* One light's shadow as it was set, by the light's place in the list: its
 * kind's fields, and for a map kind the scene's shadow pass materials its
 * views hold (indices into shadow_pass_materials). Walked whole each time
 * the scene's block is written, into the block's view table, shadows and
 * directional lights; not read by a frame. */
struct rbtrend_lit_shadow_record {
	uint32_t light;
	RBTREND_LIT_SHADOW_KIND kind;
	float strength;
	/* MASK: borrowed, as RBTREND_LIT_LIGHT_SHADOW::_mask says. */
	rbtrend_texture_view mask;
	uint32_t view_count;
	uint8_t materials[6];
	float view_projection[6][16];
	RBTREND_LIT_SHADOW_RECT rects[6];
	float split_depths[RBTREND_LIT_MAX_CASCADES];
	float depth_bias;
	float normal_offset;
	float softness;
};
RBT_PLAIN_DATA(rbtrend_lit_shadow_record);

/* The most shadows a scene holds: a map shadow takes a view at least, and
 * a mask none, which only a directional light has. */
#define RBTREND_LIT_MAX_SHADOWS (RBTREND_LIT_MAX_SHADOW_VIEWS + RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS)

typedef struct rbtrend_lit_scene_t {
	char *debug_name{nullptr}; /* first, as every handle's: rbt_debug_name_set() writes it (rbtcore.h) */
	rbtrend_instance instance{nullptr};
	/* Of the scene layout: the block and every texture set 0 reads. Its
	 * lit_scene points back here. */
	rbtrend_material material{nullptr};
	/* Of the shadow layout: one matrix each, for the passes that render the
	 * shadows' views, made with the scene and handed out as shadows take
	 * views: the free ones' indices are free_pass_materials[0, free_pass_count). */
	rbtrend_material shadow_pass_materials[RBTREND_LIT_MAX_SHADOW_VIEWS]{};
	uint8_t free_pass_materials[RBTREND_LIT_MAX_SHADOW_VIEWS]{};
	uint32_t free_pass_count{0};
	/* Of the cull layout: the block, and the tiles to write. */
	rbtrend_material cull_material{nullptr};
	/* Of the occlusion layout: the matrices and the command's settings. */
	rbtrend_material occlusion_material{nullptr};
	/* The tiles, a copy for each frame in flight, written by the culling
	 * pass and read by the surface; NULL with no target size. Sized for
	 * tiles_x by tiles_y. */
	rbtrend_buffer tiles{nullptr};
	/* The block as last set, written whole into the materials that hold it
	 * each time. */
	rbtrend_lit_scene_block block{};
	/* The lights' list as last set: each one's type and slot. */
	rbtrend_lit_light_entry lights[RBTREND_LIT_MAX_LIGHTS + RBTREND_LIT_MAX_DIRECTIONAL_LIGHTS]{};
	uint32_t light_count{0};
	/* The shadows as set, in the order they were first set; resolved into
	 * the block each time it is written. */
	rbtrend_lit_shadow_record shadows[RBTREND_LIT_MAX_SHADOWS]{};
	uint32_t shadow_count{0};
	/* The atlas as last set (borrowed), and its size in texels. */
	rbtrend_texture_view atlas{nullptr};
	uint32_t atlas_width{0};
	uint32_t atlas_height{0};
	/* What the ambient light names, or NULL. */
	rbtrend_lit_environment environment{nullptr};
	/* The scene's block alone, as Rend's traced passes read it (their set
	 * 0; rbtrendraytracing_internal.hpp): made by the first trace of the
	 * scene. */
	rbtrend_binding_set rt_set{nullptr};
	/* The same for LIT_SSR's set 0, which isn't traced: made by the first
	 * march of the scene. */
	rbtrend_binding_set ssr_set{nullptr};
	RBT_STALE_TAG
} rbtrend_lit_scene_t;

/* The directional light light [light] of [scene]'s list is, by its place
 * among the scene's directional lights; UINT32_MAX if it names no light or
 * one of another type. */
uint32_t rbtrend_lit_scene_directional_slot(rbtrend_lit_scene scene, uint32_t light);

typedef struct rbtrend_exposure_t {
	char *debug_name{nullptr}; /* first, as every handle's: rbt_debug_name_set() writes it (rbtcore.h) */
	rbtrend_exposure_t() = default;
	~rbtrend_exposure_t();
	rbtrend_instance instance{nullptr};
	RBTREND_EXPOSURE_DESC desc{};
	/* The picture's log luminance, with every mip down to 1 texel; its
	 * first mip as a render target, the whole as one view. */
	rbtrend_texture2d luminance{nullptr};
	rbtrend_texture_view luminance_target{nullptr};
	rbtrend_texture_view luminance_view{nullptr};
	/* Two 1x1 exposures, written in turn: [current] holds the last one
	 * adapted, the other the one before. */
	rbtrend_texture2d values[2]{};
	rbtrend_texture_view value_targets[2]{};
	rbtrend_texture_view value_views[2]{};
	uint32_t current{0};
	/* Whether an AUTO_EXPOSURE has been compiled: before, there is no
	 * exposure to ease from. */
	bool adapted{false};
	/* For an exposure that leaves some of the picture out (low above 0 or
	 * high below 1): its bins, a copy for each frame, and the set of the
	 * histogram layout that reads the luminance texture and writes them. */
	rbtrend_buffer histogram{nullptr};
	rbtrend_binding_set histogram_set{nullptr};
	RBT_STALE_TAG
} rbtrend_exposure_t;

#endif // RBTREND_LIT_INTERNAL_HPP
