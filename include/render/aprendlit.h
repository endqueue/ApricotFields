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
 * materials, with a clear coat and sheen, under directional lights with
 * cascaded shadows, point and spot lights, each with a shadow of its own, a
 * hemisphere of ambient light, an environment (a picture or a sky) and
 * ambient occlusion - shaders Aprend ships, and the objects that feed them.
 *
 * @par What is whose
 * Aprend ships the shading, never the geometry. A surface is drawn by a
 * pipeline the caller makes, with Aprend's surface fragment shader
 * (APREND_LIT_SHADER_SURFACE_FRAGMENT) and a vertex shader: the caller's
 * own, for geometry of its own kind, or Aprend's
 * (APREND_LIT_SHADER_SURFACE_VERTEX), for meshes of APREND_LIT_VERTEX with
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
 * that renders a shadow binds, at set 0, the material
 * aprend_lit_scene_get_shadow_pass_material() gives for the light and view
 * it renders (of aprend_lit_get_shadow_layout()), which holds the matrix the
 * view is rendered through and leaves out the atlas itself: a pass can't
 * read what it renders into.
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
 *   none, which turns normal mapping off;
 * - `layout(location = 4) out vec4`: its color, linear RGBA, which
 *   multiplies the base color and the opacity, as glTF 2.0's COLOR_0 does
 *   (the base color factor times the base color map times the vertex
 *   color); (1, 1, 1, 1) for geometry with none. Emission is not
 *   multiplied by it.
 * All five are written even where the geometry has no use for them: Metal
 * refuses a pipeline whose fragment shader reads what its vertex shader
 * doesn't write, and D3D12 matches the stages by register in order
 * (SpudGPU's rule: every output from location 0 up to the last one read).
 *
 * The same holds for a caller's vertex shader in a pass that renders a
 * shadow map or a depth pre-pass with APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT:
 * it writes locations 0 to 4 as above (the cut-out fragment shader reads the
 * texture coordinate and the color).
 *
 * Positions and directions are in any one space of the
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
 * a directional light at radiance 1 lights a face of albedo 0.5 to 0.5; a point light
 * does so at distance 1, falling off with the square of the distance. What
 * the surface writes is linear radiance times the view's exposure, for a
 * float target and then APREND_COMMAND_TONE_MAP (aprendcommands.h), or
 * already clamped and sRGB-encoded for an 8-bit UNORM target shown as it is
 * (APREND_LIT_VIEW::output_encoding). It is premultiplied by the opacity:
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
 * (APREND_LIT_VIEW) culls its lights for each 16-texel tile of the target
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
/** @brief The most point and spot lights a scene holds: 128. */
#define APREND_LIT_MAX_LIGHTS 128u
/** @brief The most directional lights a scene holds, beside its point and spot lights (Hazel's 4). */
#define APREND_LIT_MAX_DIRECTIONAL_LIGHTS 4u
/** @brief The most cascades a directional light's shadow has. */
#define APREND_LIT_MAX_CASCADES 4u
/**
 * @brief The most views a scene's shadows draw from its atlas, together: a
 * cascade, a spot's map and each of a point light's six faces are one each.
 */
#define APREND_LIT_MAX_SHADOW_VIEWS 64u
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
	/**
	 * The surface, shaded: what the top of this header describes. Reads
	 * locations 0 to 4; its color at 4 multiplies the base color and the
	 * opacity.
	 */
	APREND_LIT_SHADER_SURFACE_FRAGMENT = 0,
	/**
	 * A fragment shader that writes nothing, for a pass that renders depth
	 * alone (a shadow map, a depth pre-pass) with no color target: a
	 * pipeline needs a fragment shader. Reads no input.
	 */
	APREND_LIT_SHADER_DEPTH_FRAGMENT = 1,
	/**
	 * Meshes of APREND_LIT_VERTEX at vertex buffer slot 0, with an
	 * APREND_LIT_INSTANCE per instance at slot 1 (aprend_lit_vertex_layout()
	 * and aprend_lit_instance_layout() for the pipeline's vertex bindings),
	 * through the view's view_projection; positions out in the space the
	 * instance's transform takes them to. Writes the color at location 4
	 * as (1, 1, 1, 1).
	 */
	APREND_LIT_SHADER_SURFACE_VERTEX = 2,
	/**
	 * The same meshes through a shadow pass material's matrix
	 * (aprend_lit_get_shadow_layout() at set 0), for a pass that renders a
	 * view of a shadow; it writes locations 0 to 4 (the position, normal and
	 * tangent as 0, the texture coordinate, and the color as white), for
	 * APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT.
	 */
	APREND_LIT_SHADER_SHADOW_VERTEX = 3,
	/**
	 * Depth only, as APREND_LIT_SHADER_DEPTH_FRAGMENT, for cut-out
	 * materials: a fragment whose opacity (the material's base color alpha,
	 * times the base color map's alpha, times the color's alpha at location
	 * 4) is below the material's alpha_cutoff is discarded, so the holes in
	 * leaves or a perforated screen let light through. Reads the lit
	 * material at set 1 and locations 0 to 4 (it uses 2 and 4).
	 */
	APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT = 4,
	/**
	 * The scene's environment as a background, for a pass over the whole
	 * target with aprend_get_fullscreen_vertex_shader() (aprendshader.h),
	 * drawn before the surfaces: each texel looks along its ray through the
	 * view's view_projection and reads the environment there, times its
	 * intensity and the exposure, written out as a surface is. Black with
	 * no environment. With directional lights whose `_sky_disc` is true,
	 * each disc is drawn over it at its `_radius`, its edge softened over a
	 * texel. Reads set 0 and needs the view's target size.
	 */
	APREND_LIT_SHADER_SKY_FRAGMENT = 5,
	/**
	 * Meshes of APREND_LIT_VERTEX at vertex buffer slot 0 with a color per
	 * vertex at slot 1 (aprend_lit_vertex_color_layout()), and an
	 * APREND_LIT_INSTANCE per instance at slot 2: as
	 * APREND_LIT_SHADER_SURFACE_VERTEX, but the instance a slot later and
	 * the color at location 4 the vertex's own. A pipeline's vertex bindings
	 * are aprend_lit_vertex_layout(), aprend_lit_vertex_color_layout() and
	 * aprend_lit_instance_layout(), the last per instance; a draw list's
	 * `_instance_slot` is 2.
	 *
	 * Meshes whose colors are per vertex (glTF's COLOR_0, a painted
	 * terrain, a scan) keep them under lit shading.
	 */
	APREND_LIT_SHADER_SURFACE_VERTEX_COLOR = 6,
	/**
	 * The shadow pass of the same colored meshes, as
	 * APREND_LIT_SHADER_SHADOW_VERTEX with the bindings of
	 * APREND_LIT_SHADER_SURFACE_VERTEX_COLOR, handing the color on at
	 * location 4 for APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT. A colored mesh
	 * whose material isn't cut out may instead be drawn with
	 * APREND_LIT_SHADER_SHADOW_VERTEX, named by its first vertex buffer
	 * alone in a draw list whose `_instance_slot` is 1.
	 *
	 * A cut-out's holes come from its opacity, the color's alpha included,
	 * and the shadow's are the same holes.
	 */
	APREND_LIT_SHADER_SHADOW_VERTEX_COLOR = 7,
	/**
	 * The surface, as APREND_LIT_SHADER_SURFACE_FRAGMENT, writing two more
	 * color targets for reflections (aprendcommands.h: LIT_SSR,
	 * LIT_TRACE_REFLECTIONS, LIT_REFLECT). At color target 1, the shading
	 * normal (xyz, unit, in the scene's space, the normal map applied) and
	 * the roughness (w, after the ORM map), for an
	 * SPUDGPU_FORMAT_R16G16B16A16_FLOAT target. At color target 2, the
	 * specular weight the environment's specular light is multiplied by
	 * (the split sum's f0 x scale + bias, times the occlusion, and under a
	 * sheen or a clear coat the share of the light they leave the surface
	 * below them), linear, in rgb of an SPUDGPU_FORMAT_R8G8B8A8_UNORM
	 * target; its alpha is written 0 and read by nothing. A pipeline with
	 * it has three color targets, target 0 the picture as with
	 * APREND_LIT_SHADER_SURFACE_FRAGMENT. A cut-out discards in all three.
	 *
	 * A forward renderer has no G-buffer: a pipeline with this variant pays
	 * for the two targets where a caller reflects, and nowhere else. The
	 * caller clears both to 0 before the surfaces: a normal of 0 is a texel
	 * with no surface, which nothing reflects.
	 */
	APREND_LIT_SHADER_SURFACE_REFLECT_FRAGMENT = 8,
};

/**
 * @brief One of Aprend's lit shaders, as APREND_LIT_SHADER describes it. Its
 * entry point is `main`.
 *
 * The shader is the instance's, made on the first call and destroyed with
 * the instance. Don't pass it to aprend_shader_destroy().
 *
 * @param[in]  instance   Instance the shader is of.
 * @param[in]  shader     Which.
 * @param[out] out_shader Receives the shader; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance or
 *         @p out_shader is NULL or @p shader is not an APREND_LIT_SHADER;
 *         or APRESULT_APREND_GPU_FAILURE if it can't be created.
 */
APRESULT aprend_lit_get_shader(aprend_instance instance, APREND_LIT_SHADER shader, aprend_shader *out_shader, APREND_ERROR *out_error);

/**
 * @brief The layout of set APREND_LIT_SCENE_SET, for slot 0 of a lit
 * pipeline: what an aprend_lit_scene fills. Made from every one of Aprend's
 * shaders that read it - the surface, its two vertex shaders and the sky -
 * so one layout serves them all.
 *
 * The instance's, as aprend_lit_get_shader()'s are: don't destroy it.
 *
 * @param[in]  instance   Instance the layout is of.
 * @param[out] out_layout Receives the layout; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance or
 *         @p out_layout is NULL; or APRESULT_APREND_GPU_FAILURE if it can't
 *         be made.
 */
APRESULT aprend_lit_get_scene_layout(aprend_instance instance, aprend_binding_layout *out_layout, APREND_ERROR *out_error);

/**
 * @brief The layout of set 0 of a pass that renders a view of a shadow
 * with APREND_LIT_SHADER_SHADOW_VERTEX or
 * APREND_LIT_SHADER_SHADOW_VERTEX_COLOR (both read it): one matrix, the one
 * the view is rendered through. Filled by a scene's shadow pass materials
 * (aprend_lit_scene_get_shadow_pass_material()). The instance's: don't
 * destroy it.
 *
 * @param[in]  instance   Instance the layout is of.
 * @param[out] out_layout Receives the layout; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance or
 *         @p out_layout is NULL; or APRESULT_APREND_GPU_FAILURE if it can't
 *         be made.
 */
APRESULT aprend_lit_get_shadow_layout(aprend_instance instance, aprend_binding_layout *out_layout, APREND_ERROR *out_error);

/**
 * @brief The layout of set APREND_LIT_MATERIAL_SET, for slot 1 of a lit
 * pipeline (and of a cut-out shadow pass): what a lit material fills. The
 * instance's: don't destroy it.
 *
 * @param[in]  instance   Instance the layout is of.
 * @param[out] out_layout Receives the layout; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance or
 *         @p out_layout is NULL; or APRESULT_APREND_GPU_FAILURE if it can't
 *         be made.
 */
APRESULT aprend_lit_get_material_layout(aprend_instance instance, aprend_binding_layout *out_layout, APREND_ERROR *out_error);

/**
 * @brief One vertex of a mesh for Aprend's lit vertex shaders: 48 bytes,
 * tightly packed.
 */
/* Frozen: a vertex in GPU memory, laid out as the lit shaders read it. It never changes; a new need gets a new type. */
typedef struct APREND_LIT_VERTEX {
	/** The position, in the mesh's own space. */
	float position[3];
	/** The normal, in the mesh's own space. */
	float normal[3];
	/** The texture coordinate, v down the image (its first row at 0). */
	float uv[2];
	/** xyz along u, w +1 or -1 for which way v runs; all 0 for none. */
	float tangent[4];
} APREND_LIT_VERTEX;

/**
 * @brief One instance for Aprend's lit vertex shaders: a draw list's
 * instance bytes (aprenddrawlist.h, a stride of 64), or a vertex buffer that
 * steps per instance.
 */
/* Frozen: an instance in GPU memory, laid out as the lit shaders read it. It never changes; a new need gets a new type. */
typedef struct APREND_LIT_INSTANCE {
	/**
	 * The mesh's transform into the space the scene is in, column after
	 * column. Normals go through its inverse transpose, so a scale that
	 * isn't the same along every axis, or a mirror, keeps them square.
	 */
	float transform[16];
} APREND_LIT_INSTANCE;

/**
 * @brief The vertex layout of APREND_LIT_VERTEX, for vertex binding 0 of a
 * pipeline with Aprend's lit vertex shaders. Its elements are Aprend's own,
 * static, so the layout may be kept as long as anything likes.
 *
 * @return The layout: four elements, a stride of 48.
 */
APREND_VERTEX_LAYOUT aprend_lit_vertex_layout(void);

/**
 * @brief The vertex layout of APREND_LIT_INSTANCE (four VEC4 columns), for
 * vertex binding 1, per instance. Static, as aprend_lit_vertex_layout()'s.
 *
 * @return The layout: four elements, a stride of 64.
 */
APREND_VERTEX_LAYOUT aprend_lit_instance_layout(void);

/**
 * @brief The vertex layout of a per-vertex color, for vertex binding 1 of a
 * pipeline with APREND_LIT_SHADER_SURFACE_VERTEX_COLOR or
 * APREND_LIT_SHADER_SHADOW_VERTEX_COLOR: one APREND_VERTEX_ELEMENT_TYPE_VEC4
 * element named `color` at offset 0, linear RGBA. Static, as
 * aprend_lit_vertex_layout()'s, so it may be kept as long as anything likes.
 *
 * A caller whose colors are interleaved in a buffer of its own gives its
 * own layout instead: one VEC4 element at its offset, at its stride.
 *
 * @return The layout: one element, a stride of 16.
 */
APREND_VERTEX_LAYOUT aprend_lit_vertex_color_layout(void);

/* ---- Materials ---------------------------------------------------------- */

/**
 * @brief A lit material's values: the metal/roughness parameters Unreal's
 * default lit shading takes and glTF's are, each multiplied by its map, and
 * glTF's clear coat and sheen.
 */
typedef struct APREND_LIT_MATERIAL_VALUES {
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
	/** How much light passes through the surface instead of scattering
	 * back, 0 (none) to 1: glTF's KHR_materials_transmission. That share
	 * of the diffuse light is replaced by the opaque picture behind, bent
	 * by #ior through #thickness and blurred by the roughness, read from
	 * the scene's color (aprend_lit_scene_set_scene_color()); without one,
	 * nothing passes. A metal transmits nothing. What is behind other
	 * transmitting surfaces, or off the screen, doesn't show. */
	float transmission;
	/** The index of refraction, from 1 (no bending): 1.5 for glass, 1.33
	 * for water (glTF's KHR_materials_ior). Below 1 is taken as 1. */
	float ior;
	/** How thick the surface is, in the scene's units: how far the bent
	 * view ray travels inside it, so how far what is behind seems to move,
	 * and how far light travels for #attenuation_distance (glTF's
	 * KHR_materials_volume). 0 for a thin sheet, which shifts nothing. */
	float thickness;
	/** The color light passing through is tinted to after
	 * #attenuation_distance, linear. */
	float attenuation_color[3];
	/** How far light travels inside before it is tinted to
	 * #attenuation_color (Beer's law); 0 for never. */
	float attenuation_distance;
} APREND_LIT_MATERIAL_VALUES;

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
 * It also takes an entry in the instance's lit-material table, which it
 * keeps for its life: one buffer of every lit material's values that a
 * path trace reads (APREND_COMMAND_LIT_PATH_TRACE, aprendcommands.h), the
 * entry named from a TLAS's material table (aprendraytracing.h). The table
 * grows by doubling when it is full, an allocation then and only then.
 * Destroying the material frees the entry, its generation advanced, once
 * the frames using it have completed.
 *
 * @param[in]  instance     Instance the material is of.
 * @param[out] out_material Receives the material; NULL on failure, with no
 *                          table entry taken.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance or
 *         @p out_material is NULL; APRESULT_LIMIT if the lit-material table
 *         holds 2^24 materials; or the code it can't be made with.
 */
APRESULT aprend_lit_material_create(aprend_instance instance, aprend_material *out_material, APREND_ERROR *out_error);

/**
 * @brief Sets every value of a lit material at once.
 *
 * The values reach a path trace through the material's entry in the
 * instance's lit-material table (aprend_lit_material_create()): staged
 * here, and copied into the table in the next submission, ahead of what
 * reads it, so a frame already submitted reads the values it was submitted
 * with.
 *
 * @param[in] material Material to change.
 * @param[in] values   The values. Only read during the call.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         changed, if an argument is NULL, @p material is not of
 *         aprend_lit_get_material_layout(), or a value is not finite.
 */
APRESULT aprend_lit_material_set_values(aprend_material material, const APREND_LIT_MATERIAL_VALUES *values, APREND_ERROR *out_error);

/**
 * @brief Reads a lit material's values back.
 *
 * @param[in]  material   Material to read.
 * @param[out] out_values Receives the values.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         written, if an argument is NULL or @p material is not of
 *         aprend_lit_get_material_layout().
 */
APRESULT aprend_lit_material_get_values(aprend_material material, APREND_LIT_MATERIAL_VALUES *out_values, APREND_ERROR *out_error);

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
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         changed, if @p material is NULL or not of
 *         aprend_lit_get_material_layout(), @p map is not an APREND_LIT_MAP,
 *         or @p view is of the wrong type or instance.
 */
APRESULT aprend_lit_material_set_map(aprend_material material, APREND_LIT_MAP map, aprend_texture_view view, APREND_ERROR *out_error);

/**
 * @brief Sets the transform of one map's texture coordinate, applied after
 * the material's own (APREND_LIT_MATERIAL_VALUES::uv_transform), so each
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
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         changed, if @p material is NULL or not of
 *         aprend_lit_get_material_layout(), @p map is not an APREND_LIT_MAP,
 *         @p transform is NULL, or a value of it is not finite.
 */
APRESULT aprend_lit_material_set_map_uv_transform(aprend_material material, APREND_LIT_MAP map, const float transform[6], APREND_ERROR *out_error);

/**
 * @brief Reads the transform of one map's texture coordinate back.
 *
 * @param[in]  material      Material to read.
 * @param[in]  map           Which map.
 * @param[out] out_transform Receives the two rows, as
 *                           aprend_lit_material_set_map_uv_transform() takes
 *                           them.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         written, if @p material is NULL or not of
 *         aprend_lit_get_material_layout(), @p map is not an APREND_LIT_MAP,
 *         or @p out_transform is NULL.
 */
APRESULT aprend_lit_material_get_map_uv_transform(aprend_material material, APREND_LIT_MAP map, float out_transform[6], APREND_ERROR *out_error);

/**
 * @brief Puts the sampler every map of a lit material is read through, or
 * puts back Aprend's (linear, mipmapped, repeating).
 *
 * @param[in] material Material to change.
 * @param[in] sampler  The sampler, or NULL for Aprend's. It must outlive
 *                     the material, or be replaced before the next compile
 *                     after it goes.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         changed, if @p material is NULL or not of
 *         aprend_lit_get_material_layout(), or @p sampler is of another
 *         instance.
 */
APRESULT aprend_lit_material_set_sampler(aprend_material material, aprend_sampler sampler, APREND_ERROR *out_error);

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
typedef struct APREND_LIT_ENVIRONMENT_DESC {
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
} APREND_LIT_ENVIRONMENT_DESC;

/**
 * @brief Creates an environment, black until it is filled.
 *
 * @param[in]  instance        Instance the environment is of.
 * @param[in]  desc            The environment's sizes. Only read during the
 *                             call.
 * @param[out] out_environment Receives the environment; NULL on failure.
 * @param[out] out_error       Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if an argument is NULL or a
 *         size is not a power of 2 in its range; the struct codes for
 *         `desc->base`; APRESULT_OUT_OF_MEMORY; or
 *         APRESULT_APREND_GPU_FAILURE if a texture can't be created.
 */
APRESULT aprend_lit_environment_create(
    aprend_instance instance,
    const APREND_LIT_ENVIRONMENT_DESC *desc,
    aprend_lit_environment *out_environment,
    APREND_ERROR *out_error);

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
 * A scene is a value store, not a world: it holds the view, the lights and
 * their shadows, the shadow atlas, the ambient light and the ambient
 * occlusion as they were last set, and is drawn with by binding
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
 * everything else 0: no view, no lights, no shadow atlas, no ambient light,
 * no shadows, so a surface drawn with it is black but for its emission.
 *
 * It makes, with its own material, APREND_LIT_MAX_SHADOW_VIEWS shadow pass
 * materials (aprend_lit_scene_get_shadow_pass_material()), handed out to
 * shadows' views as they are set: what setting a shadow needs is allocated
 * here, once.
 *
 * @param[in]  instance  Instance the scene is of.
 * @param[out] out_scene Receives the scene; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance or
 *         @p out_scene is NULL; APRESULT_OUT_OF_MEMORY; or
 *         APRESULT_APREND_GPU_FAILURE if what it is drawn with can't be made.
 */
APRESULT aprend_lit_scene_create(aprend_instance instance, aprend_lit_scene *out_scene, APREND_ERROR *out_error);

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
 * @brief Where a scene is seen from.
 */
typedef struct APREND_LIT_VIEW {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_VIEW, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Where the eye is: what view-dependent shading (highlights, Fresnel)
	 * is worked out from. */
	float eye_position[3];
	/** Which way the view looks: what directional lights' cascades are
	 * measured along. Any length; 0 only for a scene with no cascades. */
	float view_direction[3];
	/**
	 * Positions to clip space, column after column: what Aprend's surface
	 * vertex shader draws through, the sky's rays are cast through, ambient
	 * occlusion reads depth through, and lights are culled for. A caller
	 * with a vertex shader of its own gives it too if it uses any of those.
	 * SpudGPU's clip space: depth 0 to 1, +Y up the target (card S19).
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
} APREND_LIT_VIEW;

/**
 * @brief Sets where a scene is seen from.
 *
 * @param[in] scene Scene to change.
 * @param[in] view  Where from. Only read during the call.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         changed, if an argument is NULL, a value is not finite, the
 *         exposure is negative, only one of the target's sides is 0, the
 *         target is larger than 16384 texels a side, or the encoding is not
 *         an APREND_COLOR_ENCODING.
 */
APRESULT aprend_lit_scene_set_view(aprend_lit_scene scene, const APREND_LIT_VIEW *view, APREND_ERROR *out_error);

/**
 * @brief The kinds of light.
 */
typedef uint32_t APREND_LIT_LIGHT_TYPE;
enum {
	/** Light from one point, every way. */
	APREND_LIT_LIGHT_TYPE_POINT = 0,
	/** Light from one point, in a cone. */
	APREND_LIT_LIGHT_TYPE_SPOT = 1,
	/** Light from one direction, everywhere alike: a sun, a moon. */
	APREND_LIT_LIGHT_TYPE_DIRECTIONAL = 2,
};

/**
 * @brief One light of a scene, of any type.
 */
/* Frozen: an array element, set every frame. It never changes; a new need gets a new type. */
typedef struct APREND_LIT_SCENE_LIGHT {
	/** Which type. */
	APREND_LIT_LIGHT_TYPE _type;
	/** Where a point or spot light is, in the scene's space. Not read for a
	 * directional light. */
	float _position[3];
	/**
	 * A spot's axis, from the light outwards; a directional light's
	 * direction, from the surface towards the light. Any length but 0. Not
	 * read for a point light.
	 */
	float _direction[3];
	/**
	 * Its color times its strength, linear: what a white matte face square
	 * to it reads (a point or spot light at distance 1). 0 is a light that
	 * gives nothing.
	 */
	float _radiance[3];
	/** Where a point or spot light's light has fallen to 0, smoothly: above
	 * 0. What tiled culling bounds it by. Not read for a directional light. */
	float _range;
	/** A spot's full light within this angle of its axis, in radians... */
	float _inner_angle;
	/** ...fading to none at this one: #_inner_angle to pi / 2. */
	float _outer_angle;
	/**
	 * A directional light's angular radius, in radians, 0 to 0.5: the
	 * angle from its disc's centre to its edge (0.00465 for the sun seen
	 * from the earth). Its light is spread over the disc, so it lights
	 * nothing more than #_radiance; a traced shadow and the path trace
	 * read it as the light's size. A point or spot light's: 0 (card A38).
	 */
	float _radius;
	/**
	 * A directional light only: whether APREND_LIT_SHADER_SKY_FRAGMENT
	 * draws its disc, at #_radius. Each texel of a disc reads pi times
	 * #_radiance over its solid angle, 2 pi (1 - cos(_radius)). Must be
	 * false for the other types.
	 */
	bool _sky_disc;
} APREND_LIT_SCENE_LIGHT;

/**
 * @brief Sets a scene's lights, every type, replacing the ones it had.
 * Shadows, the traced mask command and the path trace name a light by its
 * place in this list.
 *
 * A shadow set for a light stays while its index names a light of a type
 * its kind fits; the call takes away the rest (an index past the new
 * count, or a kind the new light's type can't have). So a list resent each
 * frame keeps its shadows. Directional lights aren't culled per tile; point
 * and spot lights are.
 *
 * @param[in] scene  Scene to change.
 * @param[in] lights The lights; NULL with a count of 0 for none. Only read
 *                   during the call.
 * @param[in] count  How many: up to APREND_LIT_MAX_LIGHTS plus
 *                   APREND_LIT_MAX_DIRECTIONAL_LIGHTS.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, with nothing changed:
 *         APRESULT_INVALID_ARGUMENT if @p scene is NULL, @p lights is NULL
 *         with a count above 0, or a light (named by its index) has a type
 *         that is not an APREND_LIT_LIGHT_TYPE, a value not finite, a
 *         negative radiance, a direction of 0 where it is read, a range not
 *         above 0 (point, spot), angles out of order or past pi / 2
 *         (spot), a radius out of 0 to 0.5 (directional) or not 0 (point,
 *         spot), or `_sky_disc` true on a point or spot light;
 *         APRESULT_LIMIT if more than APREND_LIT_MAX_LIGHTS are point and
 *         spot lights, or more than APREND_LIT_MAX_DIRECTIONAL_LIGHTS are
 *         directional.
 */
APRESULT aprend_lit_scene_set_lights(aprend_lit_scene scene, const APREND_LIT_SCENE_LIGHT *lights, uint32_t count,
                                     APREND_ERROR *out_error);

/**
 * @brief Sets the depth atlas every shadow drawn from maps reads, or takes
 * it away.
 *
 * The caller renders each view of each shadow into its rectangle of the
 * atlas (APREND_LIT_SHADOW_MAPS::_rects): a depth-only pass over the
 * atlas, its viewport and scissor the rectangle
 * (APREND_COMMAND_SET_VIEWPORTS, APREND_COMMAND_SET_SCISSOR_RECTS), binding
 * aprend_lit_scene_get_shadow_pass_material() for that light and view at
 * set 0. Which rectangle each view has, and how large, is the caller's:
 * rectangles may differ in size, and nothing checks two don't overlap.
 * Every view's depth is rendered with the same direction of depth.
 *
 * One binding serves any number of shadows of any size, the caps then the
 * view table's; a small or distant light's shadow costs what the caller
 * gives it (Unreal's, Godot's and Unity's atlases).
 *
 * @param[in] scene          Scene to change.
 * @param[in] atlas          A SHADER_RESOURCE view, DEPTH aspect, shaped
 *                           2D, of a depth texture; NULL for none, which
 *                           every map shadow then reads as lit. Borrowed:
 *                           it outlives its use by the scene, until set
 *                           again or the scene goes.
 * @param[in] reversed_depth The views are rendered near at 1, far at 0.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, with nothing changed:
 *         APRESULT_INVALID_ARGUMENT if @p scene is NULL, or @p atlas is of
 *         another instance or not a depth view shaped as above; or
 *         APRESULT_INVALID_ARGUMENT, naming the light, if a shadow already
 *         set has a rectangle past the atlas's size.
 */
APRESULT aprend_lit_scene_set_shadow_atlas(aprend_lit_scene scene, aprend_texture_view atlas, bool reversed_depth,
                                           APREND_ERROR *out_error);

/**
 * @brief What kind of shadow a light has.
 */
typedef enum APREND_LIT_SHADOW_KIND APRI_ENUM_BASE {
	/** Not a kind: a desc that has not chosen, and is refused. */
	APREND_LIT_SHADOW_KIND_NONE      = 0,
	/** A traced mask, a directional light's, written by
	 * APREND_COMMAND_LIT_SHADOW_MASK (aprendcommands.h) for the light. */
	APREND_LIT_SHADOW_KIND_MASK      = 1,
	/** A directional light's cascades, from the atlas. Its fields are an
	 * APREND_LIT_SHADOW_MAPS chained on the shadow's base. */
	APREND_LIT_SHADOW_KIND_CASCADES  = 2,
	/** A spot's map, one view in the atlas; an APREND_LIT_SHADOW_MAPS
	 * chained on. */
	APREND_LIT_SHADOW_KIND_SPOT_MAP  = 3,
	/** A point light's six faces, +X, -X, +Y, -Y, +Z, -Z, each a view in the
	 * atlas; an APREND_LIT_SHADOW_MAPS chained on. A fragment is looked up
	 * in the face its offset from the light is largest along. */
	APREND_LIT_SHADOW_KIND_POINT_MAP = 4,
} APREND_LIT_SHADOW_KIND;

/**
 * @brief One light's shadow: its kind, its strength and, for a mask, the
 * mask. A map kind's fields are an APREND_LIT_SHADOW_MAPS chained on `base`
 * (its `next`); a mask has none chained.
 */
typedef struct APREND_LIT_LIGHT_SHADOW {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_LIGHT_SHADOW, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Which kind. */
	APREND_LIT_SHADOW_KIND _kind;
	/** How dark the shadow is: 1 takes all the light away, 0 none of it. */
	float _strength;
	/**
	 * APREND_LIT_SHADOW_KIND_MASK: a SHADER_RESOURCE 2D view of color of
	 * the mask APREND_COMMAND_LIT_SHADOW_MASK writes for this light, at the
	 * scene's view's target size, read at each fragment's place on the
	 * target. Borrowed: it outlives its use by the scene, until the light's
	 * shadow is set again or the scene goes. Not read for the other kinds.
	 */
	aprend_texture_view _mask;
} APREND_LIT_LIGHT_SHADOW;

/**
 * @brief A rectangle of a scene's shadow atlas, in texels.
 */
/* Frozen: an array element of APREND_LIT_SHADOW_MAPS. It never changes; a new need gets a new type. */
typedef struct APREND_LIT_SHADOW_RECT {
	/** Its left column. */
	uint32_t _x;
	/** Its top row. */
	uint32_t _y;
	/** Its width: above 0. */
	uint32_t _width;
	/** Its height: above 0. */
	uint32_t _height;
} APREND_LIT_SHADOW_RECT;

/**
 * @brief A map shadow's views: each one's matrix and rectangle of the
 * atlas, and the filtering. Chained on APREND_LIT_LIGHT_SHADOW's base.
 *
 * Cascades: a fragment is shadowed from the first cascade whose split is
 * beyond it along the view direction, and over the last tenth of that
 * cascade's stretch (from the split before it, or the eye for the first) it
 * is blended into the next by a smoothstep, so no seam shows where one
 * cascade's detail gives way to the next's. The last fades out to no shadow
 * over its own last tenth; past the last split a fragment is lit. A caller
 * framing its own cascades lets each after the first reach back over the
 * last tenth of the one before, as aprend_lit_fit_cascades() does: where a
 * cascade's view doesn't reach, it reads as lit.
 */
typedef struct APREND_LIT_SHADOW_MAPS {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_SHADOW_MAPS, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** How many views: 1 to APREND_LIT_MAX_CASCADES for CASCADES, 1 for a
	 * SPOT_MAP, 6 for a POINT_MAP. */
	uint32_t _view_count;
	/**
	 * Each view's matrix from the scene's space to its clip space, column
	 * after column, as rendered into its rectangle (the rectangle is the
	 * whole of that clip space). aprend_lit_fit_cascades(),
	 * aprend_lit_spot_shadow_view_projection() and
	 * aprend_lit_point_shadow_view_projection() frame them.
	 */
	float _view_projection[6][16];
	/** Each view's rectangle of the atlas, inside it. Filtering reads within
	 * a rectangle only, clamped at its edge. */
	APREND_LIT_SHADOW_RECT _rects[6];
	/**
	 * CASCADES only: how far along the view direction from the eye each
	 * cascade reaches, increasing and above 0, blended as above (the last
	 * tenth of each into the next; the last fades out). Not read for the
	 * other kinds.
	 */
	float _split_depths[APREND_LIT_MAX_CASCADES];
	/** Depth added in the light's favour before the test, in the map's
	 * depth units: against acne. */
	float _depth_bias;
	/** How far a surface's position is moved along its normal before it is
	 * looked up, in the scene's units: against acne on slopes. */
	float _normal_offset;
	/** The edge, in texels: 0 for the hardware's own filter alone, up to 4. */
	float _softness;
} APREND_LIT_SHADOW_MAPS;

/**
 * @brief Sets light @p light's one shadow, of any kind its type can have,
 * replacing what it had, or takes it away.
 *
 * A directional light takes a MASK or CASCADES; a spot a SPOT_MAP; a point
 * light a POINT_MAP. A map shadow's rectangles must lie inside the scene's
 * atlas (aprend_lit_scene_set_shadow_atlas()), and the scene's views
 * together stay within APREND_LIT_MAX_SHADOW_VIEWS. The shadow pass
 * materials of its views get their matrices here.
 *
 * A mask is read by the lit surface for that light in place of maps: the
 * light at a fragment is multiplied by 1 - `_strength` x (1 - m), m the
 * mask's red at the fragment's place on the view's target. The mask is
 * written by APREND_COMMAND_LIT_SHADOW_MASK earlier in the list that draws
 * with it, or before; a surface drawn in a list with no mask written yet
 * reads whatever the texture holds. A map shadow multiplies the light by
 * 1 - `_strength` x (1 - v), v the share of the filtered comparisons that
 * pass. Either takes effect as the scene's other values do, at the next
 * submission of a list that binds the scene.
 *
 * A light has one shadow, set by naming the light; a change to one light's
 * shadow touches that light alone (Unreal, Godot and Unity set shadows on
 * each light).
 *
 * @param[in] scene     Scene to change.
 * @param[in] light     Which light: its place in the scene's lights.
 * @param[in] shadow    The shadow, or NULL for none. Only read during the
 *                      call; a mask view it names is borrowed, as `_mask`
 *                      says.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, checked in this order, with nothing
 *         changed: APRESULT_INVALID_ARGUMENT if @p scene is NULL;
 *         APRESULT_INVALID_ARGUMENT if @p light names no light of the
 *         scene; the struct codes for `shadow->base`;
 *         APRESULT_INVALID_ARGUMENT if `_kind` is NONE or not an
 *         APREND_LIT_SHADOW_KIND, or a kind the light's type can't have, or
 *         `_strength` is not finite or out of 0 to 1; for a mask, if
 *         `_mask` is NULL, of another instance, or not a SHADER_RESOURCE 2D
 *         view of color; for a map kind, the struct codes for the chained
 *         APREND_LIT_SHADOW_MAPS (missing: APRESULT_INVALID_ARGUMENT), then
 *         APRESULT_INVALID_ARGUMENT if `_view_count` is wrong for the kind,
 *         a value is not finite, a rectangle is empty or past the atlas (or
 *         there is no atlas), the splits are not increasing above 0
 *         (CASCADES), the softness is out of 0 to 4 or a bias negative;
 *         APRESULT_LIMIT if the scene's views would pass
 *         APREND_LIT_MAX_SHADOW_VIEWS.
 */
APRESULT aprend_lit_scene_set_light_shadow(aprend_lit_scene scene, uint32_t light, const APREND_LIT_LIGHT_SHADOW *shadow,
                                           APREND_ERROR *out_error);

/**
 * @brief The material a pass that renders view @p view of light @p light's
 * map shadow binds at set 0: of aprend_lit_get_shadow_layout(), holding
 * that view's matrix. The scene's: don't destroy it. It changes with the
 * light's shadow; one taken before a later
 * aprend_lit_scene_set_light_shadow() for the light holds the new matrix.
 *
 * The scene makes APREND_LIT_MAX_SHADOW_VIEWS such materials when it is
 * created (aprend_lit_scene_create()) and hands them out as shadows take
 * views: nothing is allocated here or by aprend_lit_scene_set_light_shadow().
 *
 * @param[in] scene Scene to read.
 * @param[in] light Which light.
 * @param[in] view  Which view of its shadow: a cascade, or a face.
 *
 * @return The material, or NULL if @p scene is NULL, @p light has no map
 *         shadow, or @p view is not below its view count.
 */
aprend_material aprend_lit_scene_get_shadow_pass_material(aprend_lit_scene scene, uint32_t light, uint32_t view);

/**
 * @brief How aprend_lit_fit_cascades() frames a directional light's
 * cascades.
 */
typedef struct APREND_LIT_CASCADE_FIT {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_LIT_CASCADE_FIT, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The view's eye, as APREND_LIT_VIEW has it. */
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
	/** From the scene towards the light: the directional light's
	 * APREND_LIT_SCENE_LIGHT::_direction. */
	float light_direction[3];
	/** How far beyond a cascade towards the light its view still catches
	 * what casts a shadow into it, in the scene's units: the height of the
	 * tallest thing that can stand between a surface and the light. */
	float caster_distance;
	/** The width in texels of each cascade's rectangle of the atlas: what
	 * each cascade is snapped to, so its shadows don't shimmer as the view
	 * moves. */
	uint32_t map_size;
	/** Whether the views will be rendered with depth reversed. */
	bool reversed_depth;
} APREND_LIT_CASCADE_FIT;

/**
 * @brief Frames a directional light's cascades for a view: for each, the
 * matrix its view is rendered through and how far along the view it
 * reaches, into @p out_maps's `_view_count`, `_view_projection` and
 * `_split_depths` (the views past the count, and the splits past it, are
 * zeroed). Its rectangles, biases and softness are left as they are, the
 * caller's; its base, which the caller sets as for
 * aprend_lit_scene_set_light_shadow(), is checked and left as it is.
 *
 * Each cascade bounds its stretch of the view's frustum with a sphere, so
 * its size doesn't change as the view turns (a cascade after the first from
 * the last tenth of the stretch before it, which the surface blends over),
 * looks at it down the light's direction with a parallel projection (its
 * clip space's y negated, as aprend_lit_point_shadow_view_projection()'s),
 * and is moved by whole texels of its rectangle, so as the view moves its
 * shadows stay still: the stable cascades of Valient's "Stable Rendering of
 * Cascaded Shadow Maps", as Unreal and Hazel have them. Depth runs 0 at the
 * light's end to 1 (or reversed).
 *
 * @param[in]  fit       The view and how its cascades are framed. Only read
 *                       during the call.
 * @param[out] out_maps  Receives the cascades, as above.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         written, if an argument is NULL, a value is not finite, a direction
 *         is 0, up runs along the view, the depths or the field of view are
 *         out of their ranges, the cascade count is 0 or too many, the map
 *         size is 0, or either struct's base is wrong.
 */
APRESULT aprend_lit_fit_cascades(const APREND_LIT_CASCADE_FIT *fit, APREND_LIT_SHADOW_MAPS *out_maps, APREND_ERROR *out_error);

/**
 * @brief The matrix face @p face of a point light's shadow is rendered
 * through: a perspective from the light along the face's axis (+X, -X, +Y,
 * -Y, +Z, -Z for faces 0 to 5), from @p near_depth out to the light's range,
 * a little wider than 90 degrees (the tangent of its half angle is 1.05),
 * so a soft shadow's taps near a face's edge stay inside the face's view.
 * Its clip space's y is negated, as a directional light's cascades' and a
 * spot's are: the view is the picture it was when clip space was +Y down
 * the target, and the surface reads it through the same matrix, so either
 * way works.
 *
 * @param[in]  light               The point light (of type
 *                                 APREND_LIT_LIGHT_TYPE_POINT).
 * @param[in]  face                Which face, 0 to 5.
 * @param[in]  near_depth          The near plane's distance from the light.
 * @param[in]  reversed_depth      Whether the view is rendered with depth
 *                                 reversed: near at 1, far at 0.
 * @param[out] out_view_projection Receives the matrix, column after column.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         written, if an argument is NULL, the light is not a valid point
 *         light (as aprend_lit_scene_set_lights() checks), @p face is not
 *         below 6, or @p near_depth is not between 0 and the range.
 */
APRESULT aprend_lit_point_shadow_view_projection(const APREND_LIT_SCENE_LIGHT *light, uint32_t face, float near_depth, bool reversed_depth,
                                                 float out_view_projection[16], APREND_ERROR *out_error);

/**
 * @brief The matrix a spot light's shadow is rendered through: a perspective
 * from the light along its axis, wide enough for its cone, from
 * @p near_depth out to its range, its clip space's y negated (as
 * aprend_lit_point_shadow_view_projection()'s).
 *
 * @param[in]  light               The spot (of type
 *                                 APREND_LIT_LIGHT_TYPE_SPOT).
 * @param[in]  near_depth          The near plane's distance from the light.
 * @param[in]  reversed_depth      Whether the view is rendered with depth
 *                                 reversed: near at 1, far at 0.
 * @param[out] out_view_projection Receives the matrix, column after column.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         written, if an argument is NULL, the light is not a valid spot (as
 *         aprend_lit_scene_set_lights() checks), or @p near_depth is not
 *         between 0 and the range.
 */
APRESULT aprend_lit_spot_shadow_view_projection(const APREND_LIT_SCENE_LIGHT *light, float near_depth, bool reversed_depth,
                                                float out_view_projection[16], APREND_ERROR *out_error);

/**
 * @brief The light that comes from all round: a hemisphere of two colors,
 * and an environment, added together.
 */
typedef struct APREND_LIT_AMBIENT {
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
} APREND_LIT_AMBIENT;

/**
 * @brief Sets a scene's ambient light.
 *
 * @param[in] scene   Scene to change.
 * @param[in] ambient The ambient light. Only read during the call.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         changed, if an argument is NULL, a value is not finite, a color or
 *         the intensity is negative, up is 0 while a color isn't, or the
 *         environment is of another instance.
 */
APRESULT aprend_lit_scene_set_ambient(aprend_lit_scene scene, const APREND_LIT_AMBIENT *ambient, APREND_ERROR *out_error);

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
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         changed, if @p scene is NULL, the view is of another instance or of
 *         the wrong type or shape, or @p strength is out of 0 to 1.
 */
APRESULT aprend_lit_scene_set_ambient_occlusion(aprend_lit_scene scene, aprend_texture_view occlusion, float strength, APREND_ERROR *out_error);

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
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         changed, if @p scene is NULL.
 */
APRESULT aprend_lit_scene_set_bent_normals(aprend_lit_scene scene, bool read, APREND_ERROR *out_error);

/**
 * @brief Sets the opaque picture surfaces with transmission show through,
 * or takes it away: what APREND_COMMAND_COPY_SCENE_COLOR (aprendcommands.h)
 * copied from the view's color target into a mipped texture, between the
 * opaque draws and the transmitting ones. It is read exposed and, if the
 * scene's output encoding is sRGB, encoded, as the surfaces wrote it.
 *
 * @param[in] scene       Scene to change.
 * @param[in] scene_color A SHADER_RESOURCE 2D view of color holding every
 *                        mip the copy fills, or NULL for none. It must
 *                        outlive its use by the scene.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         changed, if @p scene is NULL, or the view is of another instance
 *         or of the wrong type, shape or aspect.
 */
APRESULT aprend_lit_scene_set_scene_color(aprend_lit_scene scene, aprend_texture_view scene_color, APREND_ERROR *out_error);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_LIT_H
