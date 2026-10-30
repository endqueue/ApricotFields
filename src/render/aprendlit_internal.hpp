#ifndef APREND_LIT_INTERNAL_HPP
#define APREND_LIT_INTERNAL_HPP

/* Internal header - never included outside ApricotFields/src/render/.
 *
 * Aprend's lit surfaces (aprendlit.h) and adapting exposure (aprendpost.h):
 * the blocks of their shaders as C++ structs, the instance's objects for
 * them, and what a scene, an environment and an exposure hold. */

#include "render/aprendlit.h"
#include "render/aprendpost.h"

#include <cstdint>
#include <vector>

/* ---- The shaders' blocks ----------------------------------------------- */

/* AprendLitLight in shaders/aprend_lit_scene.glslh. */
struct aprend_lit_light_block {
	float position_range[4];
	float radiance_type[4];
	float direction_inner[4];
	float outer_shadow[4];
};

/* AprendLitScene in shaders/aprend_lit_scene.glslh, as std140 lays it out.
 * Checked member by member against the shader's reflection when the scene
 * layout is made. */
struct aprend_lit_scene_block {
	float view_projection[16];
	float inverse_view_projection[16];
	float sun_shadow_view_projection[APREND_LIT_MAX_CASCADES][16];
	float spot_shadow_view_projection[APREND_LIT_MAX_SPOT_SHADOWS][16];
	float point_shadow_view_projection[APREND_LIT_MAX_POINT_SHADOWS * 6][16];
	float eye_position[4];
	float view_direction[4];
	float sun_direction[4];
	float sun_radiance[4];
	float ambient_up[4];
	float ambient_sky[4];
	float ambient_ground[4];
	float environment_rotation[12];
	float sun_shadow_splits[4];
	float sun_shadow_params[4];
	float spot_shadow_params[APREND_LIT_MAX_SPOT_SHADOWS][4];
	float point_shadow_params[APREND_LIT_MAX_POINT_SHADOWS][4];
	float target_size[4];
	float environment_intensity;
	float environment_mips;
	float exposure;
	uint32_t output_encoding;
	uint32_t flags;
	uint32_t light_count;
	uint32_t sun_cascade_count;
	uint32_t tile_size;
	uint32_t tiles_x;
	uint32_t tiles_y;
	float occlusion_strength;
	float scene_color_mips;
	aprend_lit_light_block lights[APREND_LIT_MAX_LIGHTS];
};

/* Bits of aprend_lit_scene_block::flags, as the shader has them. */
enum : uint32_t {
	APREND_LIT_SCENE_FLAG_SUN_SHADOW    = 1u,
	APREND_LIT_SCENE_FLAG_SUN_REVERSED  = 2u,
	APREND_LIT_SCENE_FLAG_ENVIRONMENT   = 4u,
	APREND_LIT_SCENE_FLAG_TILES         = 8u,
	APREND_LIT_SCENE_FLAG_OCCLUSION     = 16u,
	APREND_LIT_SCENE_FLAG_SPOT_REVERSED = 32u,
	APREND_LIT_SCENE_FLAG_BENT_NORMALS  = 64u,
	APREND_LIT_SCENE_FLAG_POINT_REVERSED = 128u,
	APREND_LIT_SCENE_FLAG_SCENE_COLOR   = 256u,
};

/* AprendLitShadowPass in shaders/aprend_lit_shadow_pass.glslh. */
struct aprend_lit_shadow_pass_block {
	float view_projection[16];
};

/* AprendLitMaterial in shaders/aprend_lit_material.glslh. */
struct aprend_lit_material_block {
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
	 * APREND_LIT_MAP order. */
	float map_uv_transform[4][8];
	/* x transmission, y index of refraction, z thickness, w attenuation
	 * distance (0: never). */
	float transmission[4];
	float attenuation[4];
};

/* AprendOcclusionPass in shaders/aprend_lit_occlusion.frag. */
struct aprend_occlusion_block {
	float view_projection[16];
	float inverse_view_projection[16];
	float target_size[4];
	float eye_position[4];
	float params[4];
};

/* EnvironmentParams in shaders/aprend_environment.glslh. Metal aligns a
 * block with a vec2 to 8 bytes: every push here is a multiple of 8. */
struct aprend_environment_params {
	float target_size[2];
	uint32_t face;
	uint32_t sample_count;
	float roughness;
	float source_size;
	float source_mips;
	float padding;
};

/* SkyParams in shaders/aprend_environment_sky.frag. */
struct aprend_sky_params {
	float target_size[2];
	uint32_t face;
	float turbidity;
	float sun[4];
	float ground[4];
};

/* AprendHosekSky in shaders/aprend_environment_hosek.frag, std140: each
 * channel's nine coefficients (in the dataset's order) and radiance, in a
 * vec4's red, green and blue. */
struct aprend_hosek_sky_block {
	float config[9][4];
	float radiance[4];
};

/* Cooks Hosek and Wilkie's RGB sky for a sun towards [sun] (unit, +Y up; a
 * sun below the horizon is taken at it), the air's [turbidity] (1 to 10) and
 * the ground's [albedo] (each 0 to 1), from the authors' dataset, its
 * radiance scaled so the zenith's luminance is [intensity]. */
void aprend_lit_hosek_cook(const float sun[3], float turbidity, const float albedo[3], float intensity, aprend_hosek_sky_block *out);

/* The material of [environment]'s Hosek-Wilkie coefficients, made if it
 * isn't (the passes prepared). NULL, reported, if it can't be. */
aprend_material aprend_lit_environment_sky_material(aprend_lit_environment environment);

/* ToneMapParams in shaders/aprend_tone_map.frag. */
struct aprend_tone_map_params {
	float target_size[2];
	float exposure;
	uint32_t curve;
	uint32_t encoding;
	uint32_t automatic;
};

/* AdaptParams in shaders/aprend_exposure_adapt.frag. */
struct aprend_adapt_params {
	float key;
	float min_log;
	float max_log;
	float up_rate;
	float down_rate;
	float delta_time;
	float last_mip;
	uint32_t first;
};

/* HistogramParams in shaders/aprend_exposure_histogram.comp. */
struct aprend_histogram_params {
	float min_log;
	float max_log;
	uint32_t clear;
	uint32_t size;
};

/* AdaptParams in shaders/aprend_exposure_adapt_histogram.frag, 48 bytes. */
struct aprend_histogram_adapt_params {
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
#define APREND_EXPOSURE_BINS 64u

/* The push of the passes that take only their target's size. */
struct aprend_size_params {
	float target_size[2];
};

/* ---- The instance's lit objects ---------------------------------------- */

/* The full-target passes Aprend's lit and post work is made of, each a
 * fragment shader over aprend_fullscreen.vert. */
enum {
	APREND_LIT_PASS_FROM_CUBE     = 0, /* a picture's cube into a radiance cube face */
	APREND_LIT_PASS_FROM_EQUIRECT = 1, /* an equirectangular picture likewise */
	APREND_LIT_PASS_PREFILTER     = 2, /* a specular chain mip face */
	APREND_LIT_PASS_IRRADIANCE    = 3, /* an irradiance cube face */
	APREND_LIT_PASS_SKY           = 4, /* Preetham's sky into a radiance cube face */
	APREND_LIT_PASS_BRDF_LUT      = 5, /* the BRDF lookup texture */
	APREND_LIT_PASS_OCCLUSION     = 6, /* raw ambient occlusion */
	APREND_LIT_PASS_BLUR          = 7, /* ambient occlusion smoothed */
	APREND_LIT_PASS_LUMINANCE     = 8, /* log luminance for an exposure */
	APREND_LIT_PASS_ADAPT         = 9, /* an exposure adapted */
	APREND_LIT_PASS_HOSEK_SKY     = 10, /* Hosek and Wilkie's sky into a radiance cube face */
	APREND_LIT_PASS_ADAPT_HISTOGRAM = 11, /* an exposure adapted from a histogram */
	APREND_LIT_PASS_COUNT         = 12,
};

/* The format of every texture of an environment. */
#define APREND_ENVIRONMENT_FORMAT SPUDGPU_FORMAT_R16G16B16A16_FLOAT
/* The BRDF lookup texture's size and format. */
#define APREND_BRDF_LUT_SIZE 128u
#define APREND_BRDF_LUT_FORMAT SPUDGPU_FORMAT_R16G16_FLOAT
/* An exposure's luminance texture: its side, a power of 2, and format; and
 * the format of its adapted value. */
#define APREND_EXPOSURE_LUMINANCE_SIZE 256u
#define APREND_EXPOSURE_LUMINANCE_FORMAT SPUDGPU_FORMAT_R16_FLOAT
#define APREND_EXPOSURE_FORMAT SPUDGPU_FORMAT_R32_FLOAT
/* Raw ambient occlusion, before the blur: the openness and the bent normal. */
#define APREND_OCCLUSION_FORMAT SPUDGPU_FORMAT_R16G16B16A16_FLOAT

/* What an instance makes for lit surfaces, each NULL until first needed
 * (aprend_builtins::lit). */
struct aprend_lit_builtins {
	/* aprend_lit_get_shader()'s, by APREND_LIT_SHADER. */
	aprend_shader shaders[6]{};
	aprend_binding_layout scene_layout{nullptr};
	aprend_binding_layout shadow_layout{nullptr};
	aprend_binding_layout material_layout{nullptr};
	/* Set 0 of the light culling shader: the scene's block, and its tiles. */
	aprend_shader cull_comp{nullptr};
	aprend_binding_layout cull_layout{nullptr};
	aprend_compute_pipeline cull_pipeline{nullptr};
	/* Culling with depth bounds (APREND_COMMAND_LIT_CULL_LIGHTS): set 0 the
	 * cull layout, set 1 a depth view's environment set (the pass layout). */
	aprend_shader cull_bounded_comp{nullptr};
	aprend_binding_layout cull_depth_layout{nullptr};
	aprend_compute_pipeline cull_bounded_pipeline{nullptr};
	/* Set 0 of the occlusion pass: its block. Its set 1 is a view's
	 * environment set (a texture and a sampler). */
	aprend_binding_layout occlusion_layout{nullptr};
	/* Set 0 of the Hosek-Wilkie sky pass: its cooked coefficients
	 * (aprend_hosek_sky_block). */
	aprend_binding_layout hosek_layout{nullptr};
	/* An exposure's histogram: the compute shader that counts it, the
	 * layout of set 0 it shares with the pass that adapts from it (the
	 * luminance texture, a sampler, the bins), and its pipeline. */
	aprend_shader histogram_comp{nullptr};
	aprend_binding_layout histogram_layout{nullptr};
	aprend_compute_pipeline histogram_pipeline{nullptr};
	/* What an empty binding holds: a placeholder of the binding's shape,
	 * never written and never read (a flag in the block says the binding is
	 * empty). Depth ones for the shadow maps, which a comparison sampler
	 * reads; a small storage buffer for the tiles. */
	aprend_texture2d placeholder_2d{nullptr};
	aprend_texture_view placeholder_2d_view{nullptr};
	aprend_texture2d placeholder_cube{nullptr};
	aprend_texture_view placeholder_cube_view{nullptr};
	aprend_texture2d placeholder_depth{nullptr};
	aprend_texture_view placeholder_depth_view{nullptr};
	aprend_buffer placeholder_tiles{nullptr};
	/* A material's maps, read linear, mipmapped and repeating. */
	aprend_sampler material_sampler{nullptr};
	/* Environments, lookup textures and depth, linear, mipmapped, clamped. */
	aprend_sampler environment_sampler{nullptr};
	/* The shadow maps' comparisons: reference <= texel, and >= for maps
	 * rendered with depth reversed. */
	aprend_sampler shadow_sampler{nullptr};
	aprend_sampler shadow_sampler_reversed{nullptr};
	/* The BRDF lookup texture, made by the first compile that binds a lit
	 * scene, and its render target view. */
	aprend_texture2d brdf_lut{nullptr};
	aprend_texture_view brdf_lut_view{nullptr};
	aprend_texture_view brdf_lut_target{nullptr};

	/* The full-target passes: fragment shaders by APREND_LIT_PASS_, the
	 * layout of their set 0 (a texture at 0, a sampler at 1; the occlusion
	 * pass's own layout instead), and a pipeline each, rendering to the
	 * format of what they make. */
	aprend_shader pass_frags[APREND_LIT_PASS_COUNT]{};
	aprend_binding_layout pass_layout{nullptr};
	aprend_graphics_pipeline pass_pipelines[APREND_LIT_PASS_COUNT]{};
	/* The occlusion blur renders to the caller's format: one for each. */
	struct format_pipeline {
		SPUDGPU_FORMAT format;
		aprend_graphics_pipeline pipeline;
	};
	std::vector<format_pipeline> blur_pipelines{};

	/* APREND_COMMAND_TONE_MAP's shader, its layout, and a pipeline for each
	 * target format tone mapped onto. */
	aprend_shader tone_map_frag{nullptr};
	aprend_binding_layout tone_map_layout{nullptr};
	std::vector<format_pipeline> tone_map_pipelines{};
};

/* Destroys what aprend_lit_builtins holds, pipelines before layouts, from
 * aprend_builtins_destroy(). */
void aprend_lit_builtins_destroy(aprend_instance instance);

/* The placeholders and samplers, made if they aren't. False, reported, if one
 * can't be. */
bool aprend_lit_prepare_resources(aprend_instance instance);

/* The pass of [pass], made if it isn't. NULL, reported, if it can't be. */
aprend_graphics_pipeline aprend_lit_pass_pipeline(aprend_instance instance, uint32_t pass);

/* The occlusion blur rendering to [format], made if it isn't. */
aprend_graphics_pipeline aprend_lit_blur_pipeline(aprend_instance instance, SPUDGPU_FORMAT format);

/* The tone map pipeline rendering to [format], made if it isn't. NULL,
 * reported, if it can't be. */
aprend_graphics_pipeline aprend_lit_tone_map_pipeline(aprend_instance instance, SPUDGPU_FORMAT format);

/* The exposure histogram's compute pipeline, made if it isn't (the passes
 * prepared). NULL, reported, if it can't be. */
aprend_compute_pipeline aprend_lit_histogram_pipeline(aprend_instance instance);

/* The light culling pipeline, made if it isn't. */
aprend_compute_pipeline aprend_lit_cull_pipeline(aprend_instance instance);

/* The light culling pipeline with depth bounds, made if it isn't (the
 * passes prepared). NULL, reported, if it can't be. */
aprend_compute_pipeline aprend_lit_cull_bounded_pipeline(aprend_instance instance);

/* The BRDF lookup texture and its views, made if they aren't. */
bool aprend_lit_prepare_brdf_lut(aprend_instance instance);

/* The set of the pass layout reading [view] through the environment
 * sampler, kept by the view (aprend_texture_view_t::environment_set). NULL,
 * reported, if it can't be made. */
aprend_binding_set aprend_texture_view_environment_set(aprend_texture_view view);

/* The set of the bounded culling's depth layout (its set 1) reading [view]
 * through the environment sampler, kept by the view
 * (aprend_texture_view_t::cull_depth_set). The bounded culling pipeline has
 * been made. NULL, reported, if it can't be made. */
aprend_binding_set aprend_texture_view_cull_depth_set(aprend_texture_view view);

/* ---- Environments, scenes and exposures --------------------------------- */

typedef struct aprend_lit_environment_t {
	aprend_lit_environment_t() = default;
	~aprend_lit_environment_t();
	aprend_instance instance{nullptr};
	APREND_LIT_ENVIRONMENT_DESC desc{};
	/* The picture, copied in at specular_size with every mip down to 1
	 * texel: what the prefilter and irradiance passes sample. Its faces'
	 * first mip as render targets, and the whole cube as one view. */
	aprend_texture2d radiance{nullptr};
	aprend_texture_view radiance_faces[6]{};
	aprend_texture_view radiance_cube{nullptr};
	/* The specular chain: specular_mips mips down to 8 texels, each face of
	 * each mip a render target (mip after mip, face after face), and the
	 * whole as one cube view for the surface shader. */
	aprend_texture2d specular{nullptr};
	uint32_t specular_mips{0};
	std::vector<aprend_texture_view> specular_faces{};
	aprend_texture_view specular_cube{nullptr};
	/* The irradiance cube, likewise. */
	aprend_texture2d irradiance{nullptr};
	aprend_texture_view irradiance_faces[6]{};
	aprend_texture_view irradiance_cube{nullptr};
	/* Of the Hosek-Wilkie layout: the coefficients the last
	 * APREND_COMMAND_LIT_SKY of that model cooked, made by the first. */
	aprend_material sky_material{nullptr};
} aprend_lit_environment_t;

typedef struct aprend_lit_scene_t {
	aprend_instance instance{nullptr};
	/* Of the scene layout: the block and every texture set 0 reads. Its
	 * lit_scene points back here. */
	aprend_material material{nullptr};
	/* Of the shadow layout: one matrix each, for the passes that render the
	 * sun's cascades and the spots' shadows. */
	aprend_material sun_shadow_materials[APREND_LIT_MAX_CASCADES]{};
	aprend_material spot_shadow_materials[APREND_LIT_MAX_SPOT_SHADOWS]{};
	/* Six a point shadow: [6 * index + face]. */
	aprend_material point_shadow_materials[APREND_LIT_MAX_POINT_SHADOWS * 6]{};
	/* Of the cull layout: the block, and the tiles to write. */
	aprend_material cull_material{nullptr};
	/* Of the occlusion layout: the matrices and the command's settings. */
	aprend_material occlusion_material{nullptr};
	/* The tiles, a copy for each frame in flight, written by the culling
	 * pass and read by the surface; NULL with no target size. Sized for
	 * tiles_x by tiles_y. */
	aprend_buffer tiles{nullptr};
	/* The block as last set, written whole into the materials that hold it
	 * each time. */
	aprend_lit_scene_block block{};
	/* The spot shadows' lights, resolved into the block's lights' shadow
	 * indices each time the lights or the shadows change. */
	uint32_t spot_shadow_lights[APREND_LIT_MAX_SPOT_SHADOWS]{};
	uint32_t spot_shadow_count{0};
	/* The point shadows' lights, likewise. */
	uint32_t point_shadow_lights[APREND_LIT_MAX_POINT_SHADOWS]{};
	uint32_t point_shadow_count{0};
	/* What the ambient light names, or NULL. */
	aprend_lit_environment environment{nullptr};
} aprend_lit_scene_t;

typedef struct aprend_exposure_t {
	aprend_exposure_t() = default;
	~aprend_exposure_t();
	aprend_instance instance{nullptr};
	APREND_EXPOSURE_DESC desc{};
	/* The picture's log luminance, with every mip down to 1 texel; its
	 * first mip as a render target, the whole as one view. */
	aprend_texture2d luminance{nullptr};
	aprend_texture_view luminance_target{nullptr};
	aprend_texture_view luminance_view{nullptr};
	/* Two 1x1 exposures, written in turn: [current] holds the last one
	 * adapted, the other the one before. */
	aprend_texture2d values[2]{};
	aprend_texture_view value_targets[2]{};
	aprend_texture_view value_views[2]{};
	uint32_t current{0};
	/* Whether an AUTO_EXPOSURE has been compiled: before, there is no
	 * exposure to ease from. */
	bool adapted{false};
	/* For an exposure that leaves some of the picture out (low above 0 or
	 * high below 1): its bins, a copy for each frame, and the set of the
	 * histogram layout that reads the luminance texture and writes them. */
	aprend_buffer histogram{nullptr};
	aprend_binding_set histogram_set{nullptr};
} aprend_exposure_t;

#endif // APREND_LIT_INTERNAL_HPP
