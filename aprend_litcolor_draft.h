/*
 * aprend_litcolor_draft.h - an optional per-vertex color for Aprend's lit
 * surfaces (card A46, option a, decided 2026-11-01 with the user; the
 * workspace's DECISIONS.md, "Decided"; APREND_TODO.md item 50).
 *
 * APPROVED 2026-11-01 by the user, as written here. To be built into
 * aprendlit.h and aprendraytracing.h; where building finds this draft
 * wrong or incomplete, a card says so and this file is not edited to
 * match.
 *
 * DRAFT, 2026-11-01. Not built, not in any
 * target. Not compilable: it repeats declarations and comments of
 * aprendlit.h and aprendraytracing.h to show them changed. Every change
 * below is marked [ADDED], [CHANGED] or [REMOVED]; nothing unmarked
 * changes. No struct changes and no new APRESULT.
 *
 * ---------------------------------------------------------------------------
 * What it settles (item 50's list)
 * ---------------------------------------------------------------------------
 *
 * 1. The stream's slot and format. Slot 1, the instance moved to slot 2,
 *    for the colored shaders only (not item 50's slot 2 after the instance
 *    at 1; below, "Why slot 1"). Four floats, linear RGBA: one format,
 *    APREND_VERTEX_ELEMENT_TYPE_VEC4, which Aprend's vertex layouts have
 *    today. glTF's other COLOR_0 forms (three floats; RGBA8 or RGBA16
 *    normalized) convert to it exactly; a narrower format is a new element
 *    type appended to APREND_VERTEX_ELEMENT_TYPE, which breaks no caller,
 *    when a caller measures the bytes as a problem.
 * 2. A second shader, not one shader with a neutral white bound. A white
 *    stream for meshes without colors would need a buffer as long as the
 *    longest mesh (no backend takes a stride of 0 at every vertex: Metal
 *    needs a constant step function, which SpudGPU doesn't expose, and
 *    APREND_VERTEX_LAYOUT refuses it), a limit hidden from the caller. A
 *    draw list sorts by pipeline already, so a mesh with colors and one
 *    without are two pipelines either way.
 * 3. The passes. The surface's outputs gain location 4, the color, which
 *    the surface fragment shader and the cut-out depth fragment shader
 *    read: the color multiplies the base color and the opacity, so a
 *    cut-out's holes are the same in the surface and in its shadow.
 *    Aprend's vertex shaders without a color stream write white there. The
 *    shadow pass gets a colored vertex shader too, for cut-outs; a
 *    depth-only pass that isn't cut out reads no color.
 * 4. What a ray reads at a hit: no color in this version. A path trace
 *    shades a hit with the material's base color alone, as it reads no
 *    `alpha_cutoff` today; the color reaches rays with card A43's c (the
 *    caller's vertex streams read through bindless), not through the
 *    geometry arena, which A43 decided is to go.
 *
 * ---------------------------------------------------------------------------
 * Why slot 1
 * ---------------------------------------------------------------------------
 *
 * A draw list puts its instance bytes at `_instance_slot`, which must be
 * past every slot a mesh of the list uses (APREND_DRAW_LIST_DESC; a mesh's
 * `_vertex_buffer_count` is below it). A mesh with its color at slot 2 and
 * an instance at slot 1 would be refused by every draw list. So a colored
 * mesh is `_vertex_buffers[0]` (APREND_LIT_VERTEX) and `_vertex_buffers[1]`
 * (the colors), `_vertex_buffer_count` 2, in a list whose `_instance_slot`
 * is 2. Shader locations follow the bindings in order
 * (APREND_GRAPHICS_PIPELINE_DESC::_vertex_bindings): the vertex at 0 to 3,
 * the color at 4, the instance's four columns at 5 to 8; 9 of the 16
 * SPUDGPU_MAX_VERTEX_ATTRIBUTES.
 *
 * A BLAS reads `_vertex_buffers[0]` alone (aprendraytracing.h), so one
 * APREND_DRAW_MESH serves the draw and the BLAS, colored or not.
 *
 * What it costs: a draw list has one instance slot, so it holds colored
 * meshes (instance at 2) or uncolored ones (at 1), not both. A caller with
 * both makes two lists a pass, or gives its uncolored meshes a white stream
 * of their own and draws everything colored. Mixing them in one list would
 * need the draw list to take an instance slot per draw, a change to
 * aprenddrawlist.h this draft doesn't make (a card if a caller measures
 * the second list as a cost).
 */

/* =====================================================================
 * aprendlit.h
 * ===================================================================== */

/*
 * [CHANGED] "@par A caller's vertex shader": it writes, as outputs the
 * fragment shader reads, every one of:
 * - `layout(location = 0) out vec3`: the surface's position;
 * - `layout(location = 1) out vec3`: its normal, of any length (0 faces
 *   the eye);
 * - `layout(location = 2) out vec2`: its texture coordinate (0 if it has
 *   none: every map then reads its one texel at the corner);
 * - `layout(location = 3) out vec4`: its tangent, xyz along the texture's
 *   u, w +1 or -1 for which way v runs (glTF's and MikkTSpace's), or 0 for
 *   none, which turns normal mapping off;
 * - [ADDED] `layout(location = 4) out vec4`: its color, linear RGBA, which
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
 * [CHANGED] it writes locations 0 to 4 as above (the cut-out fragment
 * shader reads the texture coordinate and the color; before, 0 to 2).
 */

/**
 * @brief The shaders Aprend ships for lit surfaces.
 */
typedef uint32_t APREND_LIT_SHADER;
enum {
	/**
	 * [CHANGED] The surface, shaded: what the top of this header describes.
	 * Reads locations 0 to 4; its color at 4 multiplies the base color and
	 * the opacity.
	 */
	APREND_LIT_SHADER_SURFACE_FRAGMENT = 0,
	/** Unchanged. Depth alone; reads no input. */
	APREND_LIT_SHADER_DEPTH_FRAGMENT = 1,
	/**
	 * [CHANGED] Meshes of APREND_LIT_VERTEX at vertex buffer slot 0, with
	 * an APREND_LIT_INSTANCE per instance at slot 1, as before; writes the
	 * color at location 4 as (1, 1, 1, 1).
	 */
	APREND_LIT_SHADER_SURFACE_VERTEX = 2,
	/**
	 * [CHANGED] The same meshes through a shadow pass material's matrix,
	 * as before; it writes locations 0 to 4 now (the normal, position and
	 * tangent as 0, the texture coordinate, and the color as white), for
	 * APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT.
	 */
	APREND_LIT_SHADER_SHADOW_VERTEX = 3,
	/**
	 * [CHANGED] Depth only, for cut-out materials: a fragment whose opacity
	 * (the material's base color alpha, times the base color map's alpha,
	 * [ADDED] times the color's alpha at location 4) is below the
	 * material's alpha_cutoff is discarded. Reads the lit material at set 1
	 * and locations 0 to 4 (it uses 2 and 4).
	 */
	APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT = 4,
	/** Unchanged. */
	APREND_LIT_SHADER_SKY_FRAGMENT = 5,
	/**
	 * [ADDED] Meshes of APREND_LIT_VERTEX at vertex buffer slot 0 with a
	 * color per vertex at slot 1 (aprend_lit_vertex_color_layout()), and an
	 * APREND_LIT_INSTANCE per instance at slot 2: as
	 * APREND_LIT_SHADER_SURFACE_VERTEX, but the instance a slot later and
	 * the color at location 4 the vertex's own. A pipeline's vertex bindings
	 * are aprend_lit_vertex_layout(), aprend_lit_vertex_color_layout() and
	 * aprend_lit_instance_layout(), the last per instance; a draw list's
	 * `_instance_slot` is 2.
	 *
	 * Why: meshes whose colors are per vertex (glTF's COLOR_0, a painted
	 * terrain, a scan) keep them under lit shading.
	 */
	APREND_LIT_SHADER_SURFACE_VERTEX_COLOR = 6,
	/**
	 * [ADDED] The shadow pass of the same colored meshes, as
	 * APREND_LIT_SHADER_SHADOW_VERTEX with the bindings of
	 * APREND_LIT_SHADER_SURFACE_VERTEX_COLOR, handing the color on at
	 * location 4 for APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT. A colored mesh
	 * whose material isn't cut out may instead be drawn with
	 * APREND_LIT_SHADER_SHADOW_VERTEX, named by its first vertex buffer
	 * alone in a draw list whose `_instance_slot` is 1.
	 *
	 * Why: a cut-out's holes come from its opacity, the color's alpha
	 * included, and the shadow's must be the same holes.
	 */
	APREND_LIT_SHADER_SHADOW_VERTEX_COLOR = 7,
};

/**
 * [ADDED] @brief The vertex layout of a per-vertex color, for vertex
 * binding 1 of a pipeline with APREND_LIT_SHADER_SURFACE_VERTEX_COLOR or
 * APREND_LIT_SHADER_SHADOW_VERTEX_COLOR: one APREND_VERTEX_ELEMENT_TYPE_VEC4
 * element named `color` at offset 0, linear RGBA. Static, as
 * aprend_lit_vertex_layout()'s, so it may be kept as long as anything likes.
 *
 * A caller whose colors are interleaved in a buffer of its own gives its
 * own layout instead: one VEC4 element at its offset, at its stride.
 *
 * Why: aprend_lit_vertex_layout() and aprend_lit_instance_layout() give the
 * other two bindings of these shaders; without this one the caller writes
 * the third by hand from a comment.
 *
 * @return The layout: one element, a stride of 16.
 */
APREND_VERTEX_LAYOUT aprend_lit_vertex_color_layout(void);

/*
 * [CHANGED] aprend_lit_get_scene_layout(): "Made from every one of Aprend's
 * shaders that read it - the surface, its two vertex shaders and the sky".
 * The layout itself doesn't change.
 *
 * [CHANGED] aprend_lit_get_shadow_layout(): read by both shadow vertex
 * shaders. The layout itself doesn't change.
 */

/* =====================================================================
 * aprendraytracing.h
 * ===================================================================== */

/*
 * [CHANGED] APREND_BLAS_DESC's comment, after "Geometry is opaque to rays
 * in this version: a cut-out material's `alpha_cutoff` is not yet read by a
 * ray.": [ADDED] "Nor is a per-vertex color (aprendlit.h,
 * APREND_LIT_SHADER_SURFACE_VERTEX_COLOR): a path trace shades a hit with
 * its material's base color alone. A geometry whose mesh has its colors at
 * `_vertex_buffers[1]` is taken as any other; slot 1 isn't read."
 */

/*
 * ---------------------------------------------------------------------------
 * ABI breaks (before 1.0; check.sh abi lists them)
 * ---------------------------------------------------------------------------
 *
 * - Added: APREND_LIT_SHADER_SURFACE_VERTEX_COLOR (6),
 *   APREND_LIT_SHADER_SHADOW_VERTEX_COLOR (7) and
 *   aprend_lit_vertex_color_layout(). No struct, signature or value
 *   changes.
 * - Contract (not in abi.txt): a caller's own vertex shader for
 *   APREND_LIT_SHADER_SURFACE_FRAGMENT writes location 4 as well, and one
 *   for APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT writes 0 to 4 (0 to 2
 *   before). One that doesn't is refused at pipeline creation on Metal,
 *   and is outside the rule on D3D12 and Vulkan. In the workspace that is
 *   one test shader (apricottests' lit_test_vert.slang), changed with it.
 * - Behaviour: none for a caller on Aprend's vertex shaders. Every lit
 *   surface interpolates one more vec4 (16 bytes of varyings, from 64 to
 *   80 counting the position), whether it has colors or not.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * ApricotFields (no device):
 * - aprend_lit_vertex_color_layout(): one VEC4 element named `color` at 0,
 *   a stride of 16; aprend_vertex_layout_get_element_index() finds it.
 * - The misuse harness: the new call regenerated in.
 *
 * apricottests (a device; Metal here, under API and shader validation with
 * warnings fatal; D3D12 and Vulkan in the handoff):
 * - test_lit_vertex_color: a quad of APREND_LIT_VERTEX drawn with
 *   APREND_LIT_SHADER_SURFACE_VERTEX_COLOR through a draw list (instance at
 *   slot 2), lit by a white hemisphere alone (no sun, roughness 1,
 *   metalness 0), its four corners colored red, green, blue and white:
 *   each corner's texel reads what the same quad with no colors reads
 *   there, times its corner's color, within test_lit_surface's tolerance,
 *   and the centre their bilinear blend; a base color of (0.5, 1, 1, 1)
 *   halves the red corner's red.
 * - The same quad drawn with APREND_LIT_SHADER_SURFACE_VERTEX (no color
 *   stream): the base color alone, as test_lit_surface reads it today.
 * - test_lit_vertex_color_cutout: a cut-out material (alpha_cutoff 0.5) on
 *   a quad whose left half's vertices have alpha 0.25 and right half's 1:
 *   the surface pass shows the right half only; its shadow, rendered with
 *   APREND_LIT_SHADER_SHADOW_VERTEX_COLOR and
 *   APREND_LIT_SHADER_DEPTH_CUTOUT_FRAGMENT onto a lit floor, darkens the
 *   floor under the right half only.
 * - test_lit_mesh_shadow and every existing lit test unchanged (Aprend's
 *   uncolored vertex shaders now writing location 4), and lit_test_vert
 *   writing location 4: test_lit_surface's readings unchanged.
 * - A BLAS over a colored mesh (`_vertex_buffer_count` 2) builds and its
 *   path trace reads as the uncolored mesh's (the color not read by rays),
 *   on the device with ray queries.
 * - test_frame_allocations with the colored quad in the frame: none of
 *   Aprend's.
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 * - glTF 2.0: COLOR_0 multiplies the base color (and its alpha the
 *   opacity, through the alpha mode); VEC3 or VEC4 of float, or normalized
 *   unsigned byte or short. Emission isn't multiplied.
 * - Bevy's StandardMaterial: the mesh's Mesh::ATTRIBUTE_COLOR, when the
 *   mesh has one, multiplies the base color through a shader variant keyed
 *   on the mesh's vertex layout (VERTEX_COLORS): the same split as here, a
 *   variant per layout, not a white stream.
 * - Godot's StandardMaterial3D: `vertex_color_use_as_albedo` (off by
 *   default), a material flag that picks a shader variant. Here the choice
 *   is the caller's pipeline, which is where Aprend's caller already picks
 *   a vertex shader per kind of geometry.
 * - Unity URP's Lit ignores vertex colors; its Particles and Simple Lit
 *   shaders read them. Unreal's materials read Vertex Color as a node the
 *   material graph multiplies in.
 * - Filament: a material that declares the color attribute reads
 *   getColor(); a mesh without the attribute gets (1, 1, 1, 1) from the
 *   engine's default buffer, which needs a buffer as long as the mesh
 *   (the white stream of point 2, refused here).
 * - Hazel: its one Vertex (position, normal, tangent, binormal, texture
 *   coordinate) has no color, and its PBR shader reads none.
 * - The platform APIs: a per-vertex attribute at a binding of its own, on
 *   all three (Vulkan's VkVertexInputBindingDescription, D3D12's input
 *   slot, Metal's vertex buffer layout); none takes a stride of 0 at every
 *   vertex portably.
 */
