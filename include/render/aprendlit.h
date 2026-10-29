#ifndef APREND_LIT_H
#define APREND_LIT_H

#include "aprendcommands.h"
#include "aprendmaterial.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @file aprendlit.h
 * @brief Aprend's lit surfaces: physically based shading of metal/roughness
 * materials, with a clear coat and sheen, under a sun with cascaded
 * shadows, point and spot lights (spots with shadows of their own), a
 * hemisphere of ambient light, an environment (a picture or a sky) and
 * ambient occlusion - shaders Aprend ships, and the objects that feed them.
 * Point lights may have shadows too, from six faces each.
 *
 * @par What is whose
 * Aprend ships the shading, never the geometry. A surface is drawn by a
 * pipeline the caller makes, with Aprend's surface fragment shader
 * (APREND_LIT_SHADER_SURFACE_FRAGMENT) and a vertex shader: the caller's
 * own, for geometry of its own kind, or Aprend's
 * (APREND_LIT_SHADER_SURFACE_VERTEX), for meshes of aprend_lit_vertex with
 * a transform per instance. Unreal splits a surface the same way (a vertex
 * factory per kind of geometry, a shading model per kind of surface); Hazel
 * ships one shader with both in it, for its one vertex layout. There is no
 * scene graph and no list of what to draw: what is drawn, and in what
 * order, is the caller's (a draw list, aprenddrawlist.h, fits). What Aprend
 * does decide is what the light does once a surface is drawn.
 *
 * @par Sets
 * The surface fragment shader reads two sets, whose layouts are Aprend's:
 * - set APREND_LIT_SCENE_SET (0), aprend_lit_get_scene_layout(): what one
 *   view of the world has in common - the eye, the lights, the ambient light
 *   and the shadows. It is an aprend_lit_scene's material
 *   (aprend_lit_scene_get_material()), bound once per pass with
 *   APREND_COMMAND_SET_MATERIAL.
 * - set APREND_LIT_MATERIAL_SET (1), aprend_lit_get_material_layout(): one
 *   surface - an aprend_material from aprend_lit_material_create(), bound
 *   per draw (a draw list binds it, at slot 1).
 * Sets 2 and 3 are the caller's, for its vertex shader (a camera, say).
 * A pipeline names Aprend's two layouts at slots 0 and 1, and its own after
 * them; a caller's vertex shader declares nothing in sets 0 and 1. A pass
 * that renders a shadow map binds instead, at set 0, one of the scene's
 * shadow pass materials (aprend_lit_get_shadow_layout()), which hold the
 * matrix the map is rendered through and leave out the maps themselves: a
 * pass can't read what it renders into.
 *
 * @par A caller's vertex shader
 * It writes, as outputs the fragment shader reads, every one of:
 * - `layout(location = 0) out vec3`: the surface's position;
 * - `layout(location = 1) out vec3`: its normal, of any length (0 faces the
 *   eye);
 * - `layout(location = 2) out vec2`: its texture coordinate (0 if it has
 *   none: every map then reads its one texel at the corner);
 * - `layout(location = 3) out vec4`: its tangent, xyz along the texture's
 *   u, w +1 or -1 for which way v runs (glTF's and MikkTSpace's), or 0 for
 *   none, which turns normal mapping off.
 * All four are written even where the geometry has no use for them: Metal
 * refuses a pipeline whose fragment shader reads what its vertex shader
 * doesn't write. Positions and directions are in any one space of the
 * caller's choosing - world space, the eye at the origin, any other - and
 * every position, direction and matrix an aprend_lit_scene is given is in
 * that same space. The push constants are all the caller's; Aprend's
 * shaders declare none.
 *
 * @par Colors and light
 * Every color in is linear, and so is all light. A color picked as sRGB (as
 * most are) is the caller's to convert; a texture of colors is read through
 * an `_SRGB` format, which converts as it is sampled. A light's radiance is
 * scaled so that a white, matte face square to it reads the light's color:
 * the sun at radiance 1 lights a face of albedo 0.5 to 0.5; a point light
 * does so at distance 1, falling off with the square of the distance. What
 * the surface writes is linear radiance times the view's exposure, for a
 * float target and then APREND_COMMAND_TONE_MAP (aprendcommands.h), or
 * already clamped and sRGB-encoded for an 8-bit UNORM target shown as it is
 * (aprend_lit_view::output_encoding). It is premultiplied by the opacity:
 * blending is the pipeline's, and an opaque surface writes alpha 1.
 *
 * @par Shading
 * Direct light: Cook-Torrance with GGX, Smith-Schlick (k = (r + 1)^2 / 8,
 * for analytic lights) and Schlick's Fresnel, as Unreal's default lit model
 * and Hazel's. A clear coat is a second, smooth GGX lobe of 4% over the base
 * (Filament's, glTF's KHR_materials_clearcoat), a sheen the Charlie
 * distribution (KHR_materials_sheen). Point and spot lights fall off by the
 * inverse square, windowed to 0 at their range (Karis's, Unreal's). Ambient
 * light: the hemisphere and the environment's irradiance for the diffuse
 * part, the hemisphere at the reflection and the environment's prefiltered
 * mips for the specular part, weighted by the split sum's scale and bias
 * from Aprend's BRDF lookup texture, as Unreal and Hazel do. A face seen
 * from behind is lit as its front.
 *
 * @par What a compile does for a scene
 * The first compile that binds a lit scene makes the instance's BRDF lookup
 * texture, once; one that binds a scene whose environment has never been
 * filled clears it to black; and one that binds a scene with a target size
 * (aprend_lit_view) culls its lights for each 16-texel tile of the target
 * with a compute pass, so a fragment reads its tile's lights, not all of
 * them. Each is recorded at the top of the list, before anything else.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A scene, an environment and a
 * material are each used by one caller at a time with everything else of
 * their instance; every set call writes the object and nothing else. The
 * fitting functions read their arguments and write their results only, so
 * they take no object and may run at once.
 */

/** @brief The set slot of a lit pipeline's scene. */
#define APREND_LIT_SCENE_SET 0u
/** @brief The set slot of a lit pipeline's material. */
#define APREND_LIT_MATERIAL_SET 1u
/** @brief The most point and spot lights a scene holds. */
#define APREND_LIT_MAX_LIGHTS 128u
/** @brief The most cascades a sun shadow has. */
#define APREND_LIT_MAX_CASCADES 4u
/** @brief The most spot lights with a shadow a scene holds. */
#define APREND_LIT_MAX_SPOT_SHADOWS 4u
/** @brief The most point lights with a shadow a scene holds. */
#define APREND_LIT_MAX_POINT_SHADOWS 4u
/** @brief The side of a tile of a view's target that lights are culled for, in texels. */
#define APREND_LIT_TILE_SIZE 16u
/** @brief The most lights one tile keeps; beyond them a tile drops the rest, in the order they were set. */
#define APREND_LIT_MAX_TILE_LIGHTS 63u

/* ---- Shaders and layouts ------------------------------------------------ */

/**
 * @brief The shaders Aprend ships for lit surfaces.
 */
typedef uint32_t APREND_LIT_SHADER;
enum {
	/** The surface, shaded: what the top of this header describes. */
	APREND_LIT_SHADER_SURFACE_FRAGMENT = 0,
	/**
	 * A fragment shader that writes nothing, for a pass that renders depth
	 * alone (a shadow map, a depth pre-pass) with no color target: a
	 * pipeline needs a fragment shader.
	 */
	APREND_LIT_SHADER_DEPTH_FRAGMENT = 1,
	/**
	 * Meshes of aprend_lit_vertex at vertex buffer slot 0, with an
	 * aprend_lit_instance per instance at slot 1 (aprend_lit_vertex_layout()
	 * and aprend_lit_instance_layout() for the pipeline's vertex bindings),
	 * through the view's view_projection; positions out in the space the
	 * instance's transform takes them to.
	 */
	APREND_LIT_SHADER_SURFACE_VERTEX = 2,
	/**
	 * The same meshes through a shadow pass material's matrix
	 * (aprend_lit_get_shadow_layout() at set 0), for a pass that renders a
	 * sun cascade's or a spot's shadow map; it hands the texture coordinate
	 * on at location 2, for APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT.
	 */
	APREND_LIT_SHADER_SHADOW_VERTEX = 3,
	/**
	 * Depth only, as APREND_LIT_SHADER_DEPTH_FRAGMENT, for cut-out
	 * materials: a fragment whose opacity is below the material's
	 * alpha_cutoff is discarded, so the holes in leaves or a perforated
	 * screen let light through. Reads the lit material at set 1 and the
	 * texture coordinate at location 2.
	 */
	APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT = 4,
	/**
	 * The scene's environment as a background, for a pass over the whole
	 * target with aprend_get_fullscreen_vertex_shader() (aprendshader.h),
	 * drawn before the surfaces: each texel looks along its ray through the
	 * view's view_projection and reads the environment there, times its
	 * intensity and the exposure, written out as a surface is. Black with
	 * no environment. With a sun disc (aprend_lit_sun::disc_angle), the
	 * sun is drawn over it, its edge softened over a texel. Reads set 0
	 * and needs the view's target size.
	 */
	APREND_LIT_SHADER_SKY_FRAGMENT = 5,
};

/**
 * @brief One of Aprend's lit shaders, as APREND_LIT_SHADER describes it. Its
 * entry point is `main`.
 *
 * The shader is the instance's, made on the first call and destroyed with
 * the instance. Don't pass it to aprend_shader_destroy().
 *
 * @param[in] instance Instance the shader is of.
 * @param[in] shader   Which.
 *
 * @return The shader, or NULL if @p instance is NULL, @p shader is not an
 *         APREND_LIT_SHADER, or it can't be created (printed).
 */
aprend_shader aprend_lit_get_shader(aprend_instance instance, APREND_LIT_SHADER shader);

/**
 * @brief The layout of set APREND_LIT_SCENE_SET, for slot 0 of a lit
 * pipeline: what an aprend_lit_scene fills. Made from every one of Aprend's
 * shaders that read it - the surface, its vertex shader and the sky - so one
 * layout serves them all.
 *
 * The instance's, as aprend_lit_get_shader()'s are: don't destroy it.
 *
 * @param[in] instance Instance the layout is of.
 *
 * @return The layout, or NULL if @p instance is NULL or it can't be made
 *         (printed).
 */
aprend_binding_layout aprend_lit_get_scene_layout(aprend_instance instance);

/**
 * @brief The layout of set 0 of a pass that renders a shadow map with
 * APREND_LIT_SHADER_SHADOW_VERTEX: one matrix, the one the map is rendered
 * through. Filled by a scene's shadow pass materials
 * (aprend_lit_scene_get_sun_shadow_material(),
 * aprend_lit_scene_get_spot_shadow_material()). The instance's: don't
 * destroy it.
 *
 * @param[in] instance Instance the layout is of.
 *
 * @return The layout, or NULL if @p instance is NULL or it can't be made
 *         (printed).
 */
aprend_binding_layout aprend_lit_get_shadow_layout(aprend_instance instance);

/**
 * @brief The layout of set APREND_LIT_MATERIAL_SET, for slot 1 of a lit
 * pipeline (and of a cut-out shadow pass): what a lit material fills. The
 * instance's: don't destroy it.
 *
 * @param[in] instance Instance the layout is of.
 *
 * @return The layout, or NULL if @p instance is NULL or it can't be made
 *         (printed).
 */
aprend_binding_layout aprend_lit_get_material_layout(aprend_instance instance);

/**
 * @brief One vertex of a mesh for Aprend's lit vertex shaders: 48 bytes,
 * tightly packed.
 */
/* Frozen: a vertex in GPU memory, laid out as the lit shaders read it. It never changes; a new need gets a new type. */
typedef struct aprend_lit_vertex {
	/** The position, in the mesh's own space. */
	float position[3];
	/** The normal, in the mesh's own space. */
	float normal[3];
	/** The texture coordinate, v down the image (its first row at 0). */
	float uv[2];
	/** xyz along u, w +1 or -1 for which way v runs; all 0 for none. */
	float tangent[4];
} aprend_lit_vertex;

/**
 * @brief One instance for Aprend's lit vertex shaders: a draw list's
 * instance bytes (aprenddrawlist.h, a stride of 64), or a vertex buffer that
 * steps per instance.
 */
/* Frozen: an instance in GPU memory, laid out as the lit shaders read it. It never changes; a new need gets a new type. */
typedef struct aprend_lit_instance {
	/**
	 * The mesh's transform into the space the scene is in, column after
	 * column. Normals go through its inverse transpose, so a scale that
	 * isn't the same along every axis, or a mirror, keeps them square.
	 */
	float transform[16];
} aprend_lit_instance;

/**
 * @brief The vertex layout of aprend_lit_vertex, for vertex binding 0 of a
 * pipeline with Aprend's lit vertex shaders. Its elements are Aprend's own,
 * static, so the layout may be kept as long as anything likes.
 *
 * @return The layout: four elements, a stride of 48.
 */
aprend_vertex_layout aprend_lit_vertex_layout(void);

/**
 * @brief The vertex layout of aprend_lit_instance (four VEC4 columns), for
 * vertex binding 1, per instance. Static, as aprend_lit_vertex_layout()'s.
 *
 * @return The layout: four elements, a stride of 64.
 */
aprend_vertex_layout aprend_lit_instance_layout(void);

/* ---- Materials ---------------------------------------------------------- */

/**
 * @brief A lit material's values: the metal/roughness parameters Unreal's
 * default lit shading takes and glTF's are, each multiplied by its map, and
 * glTF's clear coat and sheen.
 */
typedef struct aprend_lit_material_values {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_MATERIAL_VALUES, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** rgb: the surface's color, linear: what a non-metal scatters and a
	 * metal reflects. a: its opacity, 0 to 1. Times the base color map. */
	float base_color[4];
	/** Light the surface gives off of itself, linear, times the emission map. */
	float emission[3];
	/** 0 (polished) to 1 (matte), as perceived; times the ORM map's green.
	 * Below 0.045 it is taken as 0.045. */
	float roughness;
	/** 0 (a dielectric) to 1 (a metal); times the ORM map's blue. */
	float metalness;
	/** A dielectric's reflectance, 0 to 1: 0.5 is 4% head on, which suits
	 * most materials. A metal's comes from its base color. Unreal's Specular. */
	float specular;
	/** How strongly the normal map bends the normal: 1 as the map is, 0 not at all. */
	float normal_scale;
	/** How much of the ORM map's red (ambient occlusion) is applied, 0 to 1. */
	float occlusion_strength;
	/**
	 * 0 for a surface blended by its opacity. Above 0, a cut-out: a
	 * fragment whose opacity is below this is discarded and the rest are
	 * opaque (glTF's MASK), in its shadows too when they are rendered with
	 * APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT.
	 */
	float alpha_cutoff;
	/**
	 * The texture coordinate's transform before any map is read, as two
	 * rows of a 2x3 matrix: u' = [0]u + [1]v + [2], v' = [3]u + [4]v + [5].
	 * Identity is {1, 0, 0, 0, 1, 0}: what a texture's size in the world
	 * comes to, for a caller whose coordinates are distances. Each map has a
	 * transform of its own after it (aprend_lit_material_set_map_uv_transform()).
	 */
	float uv_transform[6];
	/** A clear coat over the surface, 0 (none) to 1: lacquer, car paint, a
	 * varnished floor. It reflects 4% head on, on the face's own normal. */
	float clearcoat;
	/** The coat's roughness, as #roughness. */
	float clearcoat_roughness;
	/** A sheen's color, linear, 0 for none: the soft rim of fabric and
	 * velvet. */
	float sheen_color[3];
	/** The sheen's roughness, 0.07 to 1. */
	float sheen_roughness;
} aprend_lit_material_values;

/**
 * @brief The maps of a lit material.
 */
typedef uint32_t APREND_LIT_MAP;
enum {
	/** rgb times the base color, a times the opacity: read through an `_SRGB`
	 * format for colors stored as sRGB. White where none is given. */
	APREND_LIT_MAP_BASE_COLOR = 0,
	/** A tangent-space normal map, stored as 0 to 1 (z up). Flat where none
	 * is given. */
	APREND_LIT_MAP_NORMAL = 1,
	/** Ambient occlusion in red, roughness in green, metalness in blue: glTF's
	 * and Unreal's packing. White where none is given. */
	APREND_LIT_MAP_ORM = 2,
	/** rgb times the emission. White where none is given. */
	APREND_LIT_MAP_EMISSION = 3,
};

/**
 * @brief Creates a lit material: an aprend_material (aprendmaterial.h) of
 * aprend_lit_get_material_layout(), bound at set APREND_LIT_MATERIAL_SET.
 *
 * It starts white, opaque, half rough (0.5), a dielectric of specular 0.5,
 * with no emission, normal scale 1, occlusion strength 1, no cut-out, the
 * identity uv transform (the material's and each map's), no clear coat and
 * no sheen; every map is Aprend's
 * neutral one, read through Aprend's sampler (linear, mipmapped,
 * repeating). It is an aprend_material in every other respect: destroyed
 * with aprend_material_destroy(), and its values may also be set one by one
 * by their names in the block (`base_color`, `roughness`, ...) with
 * aprend_material_set_uniform().
 *
 * @param[in] instance Instance the material is of.
 *
 * @return The material, or NULL if @p instance is NULL or it can't be made
 *         (printed).
 */
aprend_material aprend_lit_material_create(aprend_instance instance);

/**
 * @brief Sets every value of a lit material at once.
 *
 * @param[in] material Material to change.
 * @param[in] values   The values. Only read during the call.
 *
 * @return false, with nothing changed, if an argument is NULL, @p material
 *         is not of aprend_lit_get_material_layout(), or a value is not
 *         finite.
 */
bool aprend_lit_material_set_values(aprend_material material, const aprend_lit_material_values *values);

/**
 * @brief Reads a lit material's values back.
 *
 * @param[in]  material   Material to read.
 * @param[out] out_values Receives the values.
 *
 * @return false, with nothing written, if an argument is NULL or @p material
 *         is not of aprend_lit_get_material_layout().
 */
bool aprend_lit_material_get_values(aprend_material material, aprend_lit_material_values *out_values);

/**
 * @brief Puts a texture in one of a lit material's maps, or puts back
 * Aprend's neutral one.
 *
 * @param[in] material Material to change.
 * @param[in] map      Which map.
 * @param[in] view     A SHADER_RESOURCE view of a 2D texture of a format a
 *                     shader reads as floats, or NULL for the neutral map. It
 *                     must outlive the material, or be replaced before the
 *                     next compile after it goes.
 *
 * @return false, with nothing changed, if @p material is NULL or not of
 *         aprend_lit_get_material_layout(), @p map is not an APREND_LIT_MAP,
 *         or @p view is of the wrong type or instance.
 */
bool aprend_lit_material_set_map(aprend_material material, APREND_LIT_MAP map, aprend_texture_view view);

/**
 * @brief Sets the transform of one map's texture coordinate, applied after
 * the material's own (aprend_lit_material_values::uv_transform), so each
 * map can be tiled, moved or turned on its own: glTF's
 * KHR_texture_transform, whose offset, rotation and scale make the matrix
 * that extension gives, and which the caller writes here.
 *
 * Every map's starts as the identity. aprend_lit_material_set_values()
 * leaves them as they are; aprend_material_copy() copies them.
 *
 * @param[in] material  Material to change.
 * @param[in] map       Which map.
 * @param[in] transform Two rows of a 2x3 matrix, as `uv_transform`'s:
 *                      u' = [0]u + [1]v + [2], v' = [3]u + [4]v + [5].
 *                      Identity is {1, 0, 0, 0, 1, 0}. Only read during the
 *                      call.
 *
 * @return false, with nothing changed, if @p material is NULL or not of
 *         aprend_lit_get_material_layout(), @p map is not an APREND_LIT_MAP,
 *         @p transform is NULL, or a value of it is not finite.
 */
bool aprend_lit_material_set_map_uv_transform(aprend_material material, APREND_LIT_MAP map, const float transform[6]);

/**
 * @brief Reads the transform of one map's texture coordinate back.
 *
 * @param[in]  material      Material to read.
 * @param[in]  map           Which map.
 * @param[out] out_transform Receives the two rows, as
 *                           aprend_lit_material_set_map_uv_transform() takes
 *                           them.
 *
 * @return false, with nothing written, if @p material is NULL or not of
 *         aprend_lit_get_material_layout(), @p map is not an APREND_LIT_MAP,
 *         or @p out_transform is NULL.
 */
bool aprend_lit_material_get_map_uv_transform(aprend_material material, APREND_LIT_MAP map, float out_transform[6]);

/**
 * @brief Puts the sampler every map of a lit material is read through, or
 * puts back Aprend's (linear, mipmapped, repeating).
 *
 * @param[in] material Material to change.
 * @param[in] sampler  The sampler, or NULL for Aprend's. It must outlive
 *                     the material, or be replaced before the next compile
 *                     after it goes.
 *
 * @return false, with nothing changed, if @p material is NULL or not of
 *         aprend_lit_get_material_layout(), or @p sampler is of another
 *         instance.
 */
bool aprend_lit_material_set_sampler(aprend_material material, aprend_sampler sampler);

/* ---- Environments -------------------------------------------------------- */

/**
 * @brief Opaque handle to an environment: the light all round a scene,
 * prefiltered for shading - its irradiance for diffuse light, and a chain of
 * mips each blurred for a roughness for specular light.
 *
 * An environment is filled in a command list: from a picture with
 * APREND_COMMAND_LIT_ENVIRONMENT (a cube or an equirectangular image), or
 * from a sky with APREND_COMMAND_LIT_SKY (aprendcommands.h); filling it
 * again (a sky that changes) is the same command again. It is used by
 * naming it in an aprend_lit_scene's ambient light. The handle is declared
 * in aprendcommands.h, beside those commands.
 */

/**
 * @brief Configuration descriptor for an environment. No field has a
 * default.
 */
typedef struct aprend_lit_environment_desc {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_ENVIRONMENT_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * The width of a face of the specular chain's first mip, which is the
	 * picture itself, for mirrors: a power of 2 from 16 to 2048. 256 suits
	 * most uses. The chain runs down to 8 texels a face.
	 */
	uint32_t specular_size;
	/** The width of a face of the irradiance cube: a power of 2 from 8 to 128.
	 * Irradiance is smooth, so 32 is plenty. */
	uint32_t irradiance_size;
} aprend_lit_environment_desc;

/**
 * @brief Creates an environment, black until it is filled.
 *
 * @param[in] instance Instance the environment is of.
 * @param[in] desc     The environment's sizes. Only read during the call.
 *
 * @return The environment, or NULL if an argument is NULL, a size is not a
 *         power of 2 in its range, or a texture can't be created (printed).
 */
aprend_lit_environment aprend_lit_environment_create(aprend_instance instance, const aprend_lit_environment_desc *desc);

/**
 * @brief Destroys an environment. The GPU doesn't have to be finished with
 * it (aprendcontext.h, "Destroying"); no scene may name it afterwards.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p environment itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] environment Environment to destroy. NULL is accepted and does
 *                        nothing.
 */
void aprend_lit_environment_destroy(aprend_lit_environment environment);

/**
 * @brief How many mips an environment's specular chain has: the roughness of
 * mip m is m / (count - 1).
 *
 * @param[in] environment Environment to read.
 *
 * @return The count, or 0 if @p environment is NULL.
 */
uint32_t aprend_lit_environment_get_specular_mip_count(aprend_lit_environment environment);

/* ---- Scenes ------------------------------------------------------------- */

/**
 * @brief Opaque handle to a scene: what one view of the world shades with.
 *
 * A scene is a value store, not a world: it holds the view, the sun and its
 * shadow, the point and spot lights and their shadows, the ambient light and
 * the ambient occlusion as they were last set, and is drawn with by binding
 * its material (aprend_lit_scene_get_material()) at set
 * APREND_LIT_SCENE_SET. Values set take effect as an aprend_material's do
 * (aprendmaterial.h, "When uniform values take effect"): at the next
 * submission of a list that binds it, and kept until set again. One scene
 * serves every pass of a view; two views of one world (two viewports) are
 * two scenes.
 */
typedef struct aprend_lit_scene_t *aprend_lit_scene;

/**
 * @brief Creates a scene. It starts with exposure 1 and LINEAR output and
 * everything else 0: no view, no sun, no lights, no ambient light, no
 * shadows, so a surface drawn with it is black but for its emission.
 *
 * @param[in] instance Instance the scene is of.
 *
 * @return The scene, or NULL if @p instance is NULL or it can't be made
 *         (printed).
 */
aprend_lit_scene aprend_lit_scene_create(aprend_instance instance);

/**
 * @brief Destroys a scene and its materials. What it names (shadow maps, an
 * environment, an occlusion texture) is the caller's. The GPU doesn't have
 * to be finished with it.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p scene itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] scene Scene to destroy. NULL is accepted and does nothing.
 */
void aprend_lit_scene_destroy(aprend_lit_scene scene);

/**
 * @brief The material a scene is drawn with, for APREND_COMMAND_SET_MATERIAL
 * at slot APREND_LIT_SCENE_SET. The scene's: set it through the scene's
 * calls, not the material's, and don't destroy it.
 *
 * @param[in] scene Scene to read.
 *
 * @return The material, or NULL if @p scene is NULL.
 */
aprend_material aprend_lit_scene_get_material(aprend_lit_scene scene);

/**
 * @brief The material a pass that renders cascade @p cascade of the sun's
 * shadow binds at set 0: of aprend_lit_get_shadow_layout(), holding that
 * cascade's matrix from aprend_lit_scene_set_sun_shadow(). The scene's:
 * don't destroy it.
 *
 * @param[in] scene   Scene to read.
 * @param[in] cascade Which cascade.
 *
 * @return The material, or NULL if @p scene is NULL or @p cascade is not
 *         below APREND_LIT_MAX_CASCADES.
 */
aprend_material aprend_lit_scene_get_sun_shadow_material(aprend_lit_scene scene, uint32_t cascade);

/**
 * @brief The material a pass that renders spot shadow @p index binds at set
 * 0, as aprend_lit_scene_get_sun_shadow_material() is for a cascade.
 *
 * @param[in] scene Scene to read.
 * @param[in] index Which spot shadow: its place in the shadows
 *                  aprend_lit_scene_set_spot_shadows() was given.
 *
 * @return The material, or NULL if @p scene is NULL or @p index is not below
 *         APREND_LIT_MAX_SPOT_SHADOWS.
 */
aprend_material aprend_lit_scene_get_spot_shadow_material(aprend_lit_scene scene, uint32_t index);

/**
 * @brief Where a scene is seen from.
 */
typedef struct aprend_lit_view {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_VIEW, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Where the eye is: what view-dependent shading (highlights, Fresnel)
	 * is worked out from. */
	float eye_position[3];
	/** Which way the view looks: what the sun's cascades are measured along.
	 * Any length; 0 only for a scene with no sun shadow. */
	float view_direction[3];
	/**
	 * Positions to clip space, column after column: what Aprend's surface
	 * vertex shader draws through, the sky's rays are cast through, ambient
	 * occlusion reads depth through, and lights are culled for. A caller
	 * with a vertex shader of its own gives it too if it uses any of those.
	 * SpudGPU's clip space: depth 0 to 1, +Y down the target.
	 */
	float view_projection[16];
	/**
	 * The target the view is drawn into, in texels; 0 by 0 for none. With
	 * a size and a view_projection, a compile that binds the scene culls
	 * its lights for each APREND_LIT_TILE_SIZE-texel tile of it; the sky and
	 * the ambient occlusion are read by it.
	 */
	uint32_t target_width;
	/** The target's height, in texels, as #target_width. */
	uint32_t target_height;
	/** What the color is multiplied by before it is written out. 1 leaves it. */
	float exposure;
	/** How the surface writes its color. */
	APREND_COLOR_ENCODING output_encoding;
} aprend_lit_view;

/**
 * @brief Sets where a scene is seen from.
 *
 * @param[in] scene Scene to change.
 * @param[in] view  Where from. Only read during the call.
 *
 * @return false, with nothing changed, if an argument is NULL, a value is not
 *         finite, the exposure is negative, only one of the target's sides
 *         is 0, the target is larger than 16384 texels a side, or the
 *         encoding is not an APREND_COLOR_ENCODING.
 */
bool aprend_lit_scene_set_view(aprend_lit_scene scene, const aprend_lit_view *view);

/**
 * @brief The sun: a light from one direction, everywhere alike.
 */
typedef struct aprend_lit_sun {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_SUN, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** From the surface towards the sun; any length but 0. */
	float direction[3];
	/** Its color times its strength, linear; 0 for no sun. */
	float radiance[3];
	/**
	 * The angle from the disc's centre to its edge, in radians, as
	 * APREND_LIT_SHADER_SKY_FRAGMENT draws the sun on the sky: 0 for no
	 * disc, up to 0.5. The sun seen from the earth is 0.00465. The disc is
	 * the sun's own light spread over it, so it lights nothing more than
	 * #radiance does: each texel of it reads pi times #radiance over its
	 * solid angle, 2 pi (1 - cos(disc_angle)). A larger disc is dimmer.
	 */
	float disc_angle;
} aprend_lit_sun;

/**
 * @brief Sets a scene's sun.
 *
 * @param[in] scene Scene to change.
 * @param[in] sun   The sun. Only read during the call.
 *
 * @return false, with nothing changed, if an argument is NULL, a value is not
 *         finite or a radiance negative, the direction is 0 while the
 *         radiance isn't, or the disc angle is out of 0 to 0.5.
 */
bool aprend_lit_scene_set_sun(aprend_lit_scene scene, const aprend_lit_sun *sun);

/**
 * @brief The sun's shadow: cascaded shadow maps the caller rendered, each
 * covering a stretch of the view's depth, nearer stretches in finer detail.
 *
 * The caller renders each cascade into a layer of one 2D array depth
 * texture: a depth-only pass over that layer's view, binding
 * aprend_lit_scene_get_sun_shadow_material() for the cascade at set 0 with
 * Aprend's shadow shaders, or through its own shaders with the same matrix.
 * aprend_lit_fit_sun_cascades() works out the matrices and splits from the
 * view; or the caller frames them itself. A fragment is shadowed from the
 * first cascade whose split is beyond it along the view direction, and over
 * the last tenth of that cascade's stretch (from the split before it, or the
 * eye for the first) it is blended into the next by a smoothstep, so no seam
 * shows where one cascade's detail gives way to the next's. The last fades
 * out to no shadow over its own last tenth; past the last split a fragment
 * is lit. A caller framing its own cascades lets each after the first reach
 * back over the last tenth of the one before, as
 * aprend_lit_fit_sun_cascades() does: where a cascade's map doesn't reach,
 * it reads as lit.
 */
typedef struct aprend_lit_sun_shadow {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_SUN_SHADOW, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * A SHADER_RESOURCE view, DEPTH aspect, shaped 2D_ARRAY, of a depth
	 * texture with a layer for each cascade.
	 */
	aprend_texture_view map;
	/** How many cascades: 1 to APREND_LIT_MAX_CASCADES. */
	uint32_t cascade_count;
	/** Each cascade's matrix from the scene's space to its layer's clip
	 * space, column after column, as it was rendered with. */
	float view_projection[APREND_LIT_MAX_CASCADES][16];
	/** How far along the view direction from the eye each cascade reaches:
	 * increasing, and above 0. */
	float split_depths[APREND_LIT_MAX_CASCADES];
	/** Depth added in the light's favour before the test, in the map's depth
	 * units: against acne. */
	float depth_bias;
	/** How far a surface's position is moved along its normal before it is
	 * looked up, in the scene's units: against acne on slopes. */
	float normal_offset;
	/** The shadow's edge, in texels of the map: 0 for the hardware's own
	 * four-texel filter alone, 1 and up for a softer one. Up to 4. */
	float softness;
	/** How dark the shadow is: 1 takes all the sun away, 0 none of it. */
	float strength;
	/** The maps were rendered with depth reversed: near at 1, far at 0. */
	bool reversed_depth;
} aprend_lit_sun_shadow;

/**
 * @brief Sets the sun's shadow, or takes it away.
 *
 * The shadow pass materials of the cascades get their matrices here too.
 *
 * @param[in] scene  Scene to change.
 * @param[in] shadow The shadow, or NULL for none. Its map must outlive its
 *                   use by the scene.
 *
 * @return false, with nothing changed, if @p scene is NULL, the map is NULL,
 *         of another instance, not a depth view as above or has fewer layers
 *         than cascades, the cascade count is 0 or too many, a value is not
 *         finite, the splits are not increasing above 0, the softness is
 *         out of range, or the strength is out of 0 to 1.
 */
bool aprend_lit_scene_set_sun_shadow(aprend_lit_scene scene, const aprend_lit_sun_shadow *shadow);

/**
 * @brief How aprend_lit_fit_sun_cascades() frames the sun's cascades.
 */
typedef struct aprend_lit_cascade_fit {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_CASCADE_FIT, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The view's eye, as aprend_lit_view has it. */
	float eye_position[3];
	/** Which way the view looks; any length but 0. */
	float view_direction[3];
	/** Which way is up on the view's target; not along view_direction. */
	float view_up[3];
	/** true for a parallel projection, false for a perspective one. */
	bool orthographic;
	/** A perspective view's vertical field of view, in radians. */
	float fov_y;
	/** A parallel view's half height, in the scene's units. */
	float half_height;
	/** The target's width over its height. */
	float aspect;
	/** The stretch of the view's depth the cascades cover: near_depth above 0
	 * for a perspective view, far_depth beyond it. */
	float near_depth;
	/** The far end of that stretch: beyond #near_depth. */
	float far_depth;
	/** 1 to APREND_LIT_MAX_CASCADES. */
	uint32_t cascade_count;
	/** How the stretch is split, 0 to 1: 0 evenly, 1 by a constant ratio
	 * (logarithmically); between, a mix of the two (Zhang's practical split
	 * scheme). 0.75 suits most views. */
	float split_lambda;
	/** From the scene towards the sun, as aprend_lit_sun has it. */
	float sun_direction[3];
	/** How far beyond a cascade towards the sun its map still catches what
	 * casts a shadow into it, in the scene's units: the height of the
	 * tallest thing that can stand between a surface and the sun. */
	float caster_distance;
	/** The width of a layer of the map, in texels: what each cascade is
	 * snapped to, so its shadows don't shimmer as the view moves. */
	uint32_t map_size;
	/** Whether the maps will be rendered with depth reversed. */
	bool reversed_depth;
} aprend_lit_cascade_fit;

/**
 * @brief Frames the sun's cascades for a view: for each, the matrix its layer
 * is rendered through and how far along the view it reaches, into
 * @p out_shadow's cascade_count, view_projection and split_depths (nothing
 * else of it is written: its base, which the caller sets as for
 * aprend_lit_scene_set_sun_shadow(), is checked and left as it is).
 *
 * Each cascade bounds its stretch of the view's frustum with a sphere, so
 * its size doesn't change as the view turns (a cascade after the first from
 * the last tenth of the stretch before it, which the surface blends over),
 * looks at it down the sun's
 * direction with a parallel projection, and is moved by whole texels of the
 * map, so as the view moves its shadows stay still: the stable cascades of
 * Valient's "Stable Rendering of Cascaded Shadow Maps", as Unreal and Hazel
 * have them. Depth runs 0 at the sun's end to 1 (or reversed).
 *
 * @param[in]  fit        The view and how its cascades are framed. Only read
 *                        during the call.
 * @param[out] out_shadow Receives the cascades, as above.
 *
 * @return false, with nothing written, if an argument is NULL, a value is not
 *         finite, a direction is 0, up runs along the view, the depths or the
 *         field of view are out of their ranges, the cascade count is 0 or
 *         too many, the map size is 0, or either struct's base is wrong.
 */
bool aprend_lit_fit_sun_cascades(const aprend_lit_cascade_fit *fit, aprend_lit_sun_shadow *out_shadow);

/**
 * @brief Point and spot lights.
 */
typedef uint32_t APREND_LIT_LIGHT_TYPE;
enum {
	/** Light from one point, every way. */
	APREND_LIT_LIGHT_TYPE_POINT = 0,
	/** Light from one point, in a cone. */
	APREND_LIT_LIGHT_TYPE_SPOT = 1,
};

/**
 * @brief One point or spot light.
 */
/* Frozen: an array element, set every frame. It never changes; a new need gets a new type. */
typedef struct aprend_lit_light {
	/** A point light or a spot. */
	APREND_LIT_LIGHT_TYPE type;
	/** Where the light is, in the scene's space. */
	float position[3];
	/** A spot's axis, from the light outwards; any length but 0. Not read
	 * for a point light. */
	float direction[3];
	/** Its color times its strength, linear: what a white matte face
	 * square to it reads at distance 1. */
	float radiance[3];
	/** Where its light has fallen to 0, smoothly: above 0. Also what light
	 * culling bounds it by. */
	float range;
	/** A spot's full light within this angle of its axis, in radians... */
	float inner_angle;
	/** ...fading to none at this one: inner_angle to pi / 2. */
	float outer_angle;
} aprend_lit_light;

/**
 * @brief Sets a scene's point and spot lights, replacing the ones it had.
 * Spot shadows name lights by their place in this list.
 *
 * @param[in] scene  Scene to change.
 * @param[in] lights The lights; NULL with a count of 0 for none.
 * @param[in] count  How many: up to APREND_LIT_MAX_LIGHTS.
 *
 * @return false, with nothing changed, if @p scene is NULL, @p lights is NULL
 *         with a count above 0, @p count is above APREND_LIT_MAX_LIGHTS, or a
 *         light has a type that is not an APREND_LIT_LIGHT_TYPE, a value that
 *         is not finite, a negative radiance, a range that is not above 0, or
 *         (a spot) a direction of 0 or angles out of order or past pi / 2.
 */
bool aprend_lit_scene_set_lights(aprend_lit_scene scene, const aprend_lit_light *lights, uint32_t count);

/**
 * @brief The shadow of one spot light, from a layer of a depth map the
 * caller rendered, as a sun cascade is.
 */
/* Frozen: an array element, set every frame. It never changes; a new need gets a new type. */
typedef struct aprend_lit_spot_shadow {
	/** The spot it is the shadow of: its place in the scene's lights. A
	 * light that is not a spot casts no shadow from it. */
	uint32_t light;
	/** The scene's space to its layer's clip space, as rendered:
	 * aprend_lit_spot_shadow_view_projection() frames one. */
	float view_projection[16];
	/** As aprend_lit_sun_shadow's. */
	float depth_bias;
	/** As aprend_lit_sun_shadow's. */
	float normal_offset;
	/** As aprend_lit_sun_shadow's. */
	float softness;
	/** As aprend_lit_sun_shadow's. */
	float strength;
} aprend_lit_spot_shadow;

/**
 * @brief Sets the shadows of a scene's spot lights, replacing any it had.
 *
 * Shadow @p i is rendered into layer i of @p map, binding
 * aprend_lit_scene_get_spot_shadow_material() for it at set 0.
 *
 * @param[in] scene          Scene to change.
 * @param[in] map            A SHADER_RESOURCE view, DEPTH aspect, shaped
 *                           2D_ARRAY, with a layer for each shadow; NULL with
 *                           a count of 0.
 * @param[in] shadows        The shadows; NULL with a count of 0.
 * @param[in] count          Up to APREND_LIT_MAX_SPOT_SHADOWS.
 * @param[in] reversed_depth The maps were rendered near at 1.
 *
 * @return false, with nothing changed, if @p scene is NULL, @p map or
 *         @p shadows is NULL with a count above 0, there are too many, the
 *         map is not a depth view as above or has too few layers, two
 *         shadows name one light, or a value is out of its range.
 */
bool aprend_lit_scene_set_spot_shadows(aprend_lit_scene scene, aprend_texture_view map, const aprend_lit_spot_shadow *shadows,
                                       uint32_t count, bool reversed_depth);

/**
 * @brief The shadow of one point light, from six layers of a depth map the
 * caller rendered, one for each face of a cube round the light, as a spot's
 * is from one.
 */
/* Frozen: an array element, set every frame. It never changes; a new need gets a new type. */
typedef struct aprend_lit_point_shadow {
	/** The point light it is the shadow of: its place in the scene's lights.
	 * A light that is not a point light casts no shadow from it. */
	uint32_t light;
	/**
	 * Each face's matrix, from the scene's space to its layer's clip space,
	 * as rendered: faces +X, -X, +Y, -Y, +Z, -Z, in that order, each
	 * looking from the light along its axis and covering at least the
	 * quarter of space around it (a 90 degree square frustum).
	 * aprend_lit_point_shadow_view_projection() frames each. A fragment is
	 * looked up in the face its offset from the light is largest along.
	 */
	float view_projection[6][16];
	/** As aprend_lit_sun_shadow's. */
	float depth_bias;
	/** As aprend_lit_sun_shadow's. */
	float normal_offset;
	/** As aprend_lit_sun_shadow's. */
	float softness;
	/** As aprend_lit_sun_shadow's. */
	float strength;
} aprend_lit_point_shadow;

/**
 * @brief Sets the shadows of a scene's point lights, replacing any it had.
 *
 * Face f of shadow @p i is rendered into layer 6i + f of @p map, binding
 * aprend_lit_scene_get_point_shadow_material() for it at set 0: six
 * depth-only passes a light. A 2D array, not a cube array, so a device with
 * no cube array views takes them.
 *
 * @param[in] scene          Scene to change.
 * @param[in] map            A SHADER_RESOURCE view, DEPTH aspect, shaped
 *                           2D_ARRAY, with six layers for each shadow; NULL
 *                           with a count of 0.
 * @param[in] shadows        The shadows; NULL with a count of 0.
 * @param[in] count          Up to APREND_LIT_MAX_POINT_SHADOWS.
 * @param[in] reversed_depth The maps were rendered near at 1.
 *
 * @return false, with nothing changed, if @p scene is NULL, @p map or
 *         @p shadows is NULL with a count above 0, there are too many, the
 *         map is not a depth view as above or has too few layers, two
 *         shadows name one light, or a value is out of its range.
 */
bool aprend_lit_scene_set_point_shadows(aprend_lit_scene scene, aprend_texture_view map, const aprend_lit_point_shadow *shadows,
                                        uint32_t count, bool reversed_depth);

/**
 * @brief The material a pass that renders face @p face of point shadow
 * @p index binds at set 0, as aprend_lit_scene_get_spot_shadow_material() is
 * for a spot.
 *
 * @param[in] scene Scene to read.
 * @param[in] index Which point shadow: its place in the shadows
 *                  aprend_lit_scene_set_point_shadows() was given.
 * @param[in] face  Which face, 0 to 5, in the order of
 *                  aprend_lit_point_shadow::view_projection.
 *
 * @return The material, or NULL if @p scene is NULL, @p index is not below
 *         APREND_LIT_MAX_POINT_SHADOWS or @p face is not below 6.
 */
aprend_material aprend_lit_scene_get_point_shadow_material(aprend_lit_scene scene, uint32_t index, uint32_t face);

/**
 * @brief The matrix face @p face of a point light's shadow is rendered
 * through: a perspective from the light along the face's axis (+X, -X, +Y,
 * -Y, +Z, -Z for faces 0 to 5), from @p near_depth out to the light's range,
 * a little wider than 90 degrees (the tangent of its half angle is 1.05),
 * so a soft shadow's taps near a face's edge stay inside the face's map.
 *
 * @param[in]  light               The point light.
 * @param[in]  face                Which face, 0 to 5.
 * @param[in]  near_depth          The near plane's distance from the light.
 * @param[in]  reversed_depth      Whether the map is rendered with depth
 *                                 reversed: near at 1, far at 0.
 * @param[out] out_view_projection Receives the matrix, column after column.
 *
 * @return false, with nothing written, if an argument is NULL, the light is
 *         not a valid point light (as aprend_lit_scene_set_lights() checks),
 *         @p face is not below 6, or @p near_depth is not between 0 and the
 *         range.
 */
bool aprend_lit_point_shadow_view_projection(const aprend_lit_light *light, uint32_t face, float near_depth, bool reversed_depth,
                                             float out_view_projection[16]);

/**
 * @brief The matrix a spot light's shadow is rendered through: a perspective
 * from the light along its axis, wide enough for its cone, from
 * @p near_depth out to its range.
 *
 * @param[in]  light               The spot.
 * @param[in]  near_depth          The near plane's distance from the light.
 * @param[in]  reversed_depth      Whether the map is rendered with depth
 *                                 reversed: near at 1, far at 0.
 * @param[out] out_view_projection Receives the matrix, column after column.
 *
 * @return false, with nothing written, if an argument is NULL, the light is
 *         not a valid spot (as aprend_lit_scene_set_lights() checks), or
 *         @p near_depth is not between 0 and the range.
 */
bool aprend_lit_spot_shadow_view_projection(const aprend_lit_light *light, float near_depth, bool reversed_depth, float out_view_projection[16]);

/**
 * @brief The light that comes from all round: a hemisphere of two colors,
 * and an environment, added together.
 */
typedef struct aprend_lit_ambient {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_AMBIENT, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Which way is up: the sky is that way, the ground the other. Any length
	 * but 0 while either color isn't 0. */
	float up[3];
	/** The light from straight up, linear. */
	float sky[3];
	/** The light from straight down, linear. */
	float ground[3];
	/** An environment, or NULL for none. It must outlive its use by the
	 * scene: until another is set, or the scene goes. */
	aprend_lit_environment environment;
	/** What the environment's light is multiplied by. */
	float environment_intensity;
	/**
	 * Directions in the scene's space into the environment's, row after row
	 * of a 3x3 rotation: the environment's +Y is its up, and an
	 * equirectangular picture's middle column faces its +Z. The identity
	 * {1,0,0, 0,1,0, 0,0,1} is right for a scene that is Y up; a Z-up scene
	 * takes {1,0,0, 0,0,1, 0,-1,0}.
	 */
	float environment_rotation[9];
} aprend_lit_ambient;

/**
 * @brief Sets a scene's ambient light.
 *
 * @param[in] scene   Scene to change.
 * @param[in] ambient The ambient light. Only read during the call.
 *
 * @return false, with nothing changed, if an argument is NULL, a value is not
 *         finite, a color or the intensity is negative, up is 0 while a
 *         color isn't, or the environment is of another instance.
 */
bool aprend_lit_scene_set_ambient(aprend_lit_scene scene, const aprend_lit_ambient *ambient);

/**
 * @brief Sets the ambient occlusion of the view, or takes it away: a texture
 * the size of the view's target whose red is how open each texel is to the
 * sky (APREND_COMMAND_LIT_AMBIENT_OCCLUSION makes one from a depth buffer),
 * which darkens the ambient light there.
 *
 * @param[in] scene     Scene to change.
 * @param[in] occlusion A SHADER_RESOURCE 2D view of color, or NULL for none.
 *                      It must outlive its use by the scene.
 * @param[in] strength  How much of it is applied, 0 to 1.
 *
 * @return false, with nothing changed, if @p scene is NULL, the view is of
 *         another instance or of the wrong type or shape, or @p strength is
 *         out of 0 to 1.
 */
bool aprend_lit_scene_set_ambient_occlusion(aprend_lit_scene scene, aprend_texture_view occlusion, float strength);

/**
 * @brief Sets whether the surface reads a bent normal from the ambient
 * occlusion's green, blue and alpha (APREND_COMMAND_LIT_AMBIENT_OCCLUSION
 * writes it into a target that has them): the average direction that is
 * open over each texel, along which the diffuse ambient light (the
 * hemisphere and the environment's irradiance) is then looked up, in place
 * of the surface's normal, so light comes from where the surface is open.
 * Specular light keeps the surface's normal. Not read without an
 * occlusion, or with this false, as a scene starts.
 *
 * @param[in] scene Scene to change.
 * @param[in] read  true to read the bent normal.
 *
 * @return false, with nothing changed, if @p scene is NULL.
 */
bool aprend_lit_scene_set_bent_normals(aprend_lit_scene scene, bool read);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_LIT_H
