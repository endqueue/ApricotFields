/*
 * aprend_reflections_draft.h - reflections for Aprend's lit surfaces: the
 * surface variant that writes a normal-and-roughness target, screen-space
 * reflections, and ray-traced reflections over the same targets (card A6,
 * decided 2026-10-28; card S12's "ray-traced reflections as an opt-in
 * command over SSR's normal and roughness target"; APREND_TODO.md item 24).
 *
 * DRAFT, 2026-11-01. APPROVED 2026-11-02 by the user, with six fixes
 * written in here that day (the header, the half-resolution texel, the
 * trace's noise, the specular target's alpha, the frame's order, the
 * composite's tolerance). To be built into aprendlit.h and
 * aprendcommands.h; where building finds this draft wrong or incomplete, a
 * card says so and this file is not edited to match. Not compilable: it
 * repeats declarations of aprendlit.h and aprendcommands.h to show them
 * changed. Every change below is marked [ADDED], [CHANGED] or [REMOVED];
 * nothing unmarked changes.
 *
 * Written over card A30, decided 2026-11-02 (c): the reflecting surface
 * variant writes, beside the normal and roughness, a second target holding
 * the surface's specular color, so a reflection is put back exact for
 * metals.
 *
 * ---------------------------------------------------------------------------
 * What it settles that A6 and S12 left open
 * ---------------------------------------------------------------------------
 *
 * 1. Three commands, not one: LIT_SSR marches and writes a reflection with
 *    its confidence into a target of the caller's; LIT_TRACE_REFLECTIONS
 *    traces where that confidence is short of 1 (S12's "where SSR misses or
 *    roughness is low") into the same target; LIT_REFLECT puts the target
 *    on the picture. A caller with no ray queries runs the first and the
 *    last; one that wants only traced reflections the last two, with the
 *    reflection target cleared to 0 confidence. The march's and the
 *    trace's results meet in plain data the caller can see, read and
 *    debug, which one command doing all three would hide.
 * 2. Where the reflection replaces light: the surface's environment
 *    specular, the base lobe's alone (the clear coat keeps the
 *    environment's: its lobe is the face's own normal, which the target
 *    doesn't hold). LIT_REFLECT adds, at each texel,
 *    specular x confidence x (reflection - environment specular), the
 *    environment specular worked out again from the scene, the normal and
 *    the roughness, as the surface shaded it; so a texel of confidence 0
 *    is the picture as it was, bit for bit.
 * 3. The Hi-Z pyramid is the command's, from the render target pool, built
 *    from `_depth` each time (A6's (3)): no public pyramid until a second
 *    command needs one (item 42's edges, GTAO's march), then a card.
 * 4. Half resolution (A6): the march runs at half the target's size in
 *    each axis and the reflection target is that size; LIT_REFLECT
 *    upsamples it, weighted by depth and normal (as GTAO's blur is). A
 *    half-resolution texel stands for the 2x2 full-resolution texels under
 *    it (fewer at the right and bottom edges of an odd size) and takes the
 *    one of them nearest the eye: its depth, its normal and roughness, its
 *    position. Nearest, as GTAO's downsample, so a texel on an edge is the
 *    foreground's and never a blend of two surfaces.
 * 5. Roughness: the reflection is the picture read at a mip chosen from
 *    the roughness and the hit's distance (A6's (4), a cone over the mipped
 *    copy COPY_SCENE_COLOR makes); past `_max_roughness` nothing is marched
 *    and the environment stays.
 * 6. The frame's order, the caller's to record: the opaque surfaces (with
 *    the variant where reflections are wanted), COPY_SCENE_COLOR, LIT_SSR,
 *    LIT_TRACE_REFLECTIONS, LIT_REFLECT, then the transparent surfaces, then
 *    the tone map. Transparent surfaces come after LIT_REFLECT: they write
 *    none of the variant's targets, so a reflection put on the picture
 *    under them is the opaque surface's, and they are not reflected (as
 *    with transmission, which reads the same opaque copy).
 * 7. No denoiser. One trace of `_ray_count` rays per texel, accumulated
 *    over no frames and filtered over no neighbours, is noisy over a rough
 *    lobe: above a roughness of about 0.3 at one ray. `_max_roughness`
 *    bounds it, and a caller wanting rough traced reflections clean waits
 *    for a temporal or spatial filter, a card of its own when one is asked
 *    for. The march has no such noise (one ray, the mip standing for the
 *    lobe).
 *
 * ---------------------------------------------------------------------------
 * What a caller tracing per view, within a distance, needs from it
 * ---------------------------------------------------------------------------
 *
 * - On or off per target, a ray distance: a caller's own choice of which
 *   commands it records, and `_max_distance` on both.
 * - The refusal on a device without ray queries: LIT_TRACE_REFLECTIONS
 *   needs SPUDGPU_FEATURE_RAY_QUERY, as the other traced commands do
 *   (aprendcommands.h; the device's name is the caller's to put in its
 *   message: Aprend's refusal names the feature).
 * - A top level rebuilt each frame from the caller's instances within a
 *   distance: BUILD_TLAS as it is (aprendraytracing.h; measured with no
 *   allocation of Aprend's a frame, apricottests' test_frame_allocations).
 * - Not here: hits of cut-out materials are opaque, and hits read no maps
 *   and no vertex color (aprendraytracing.h, APREND_BLAS_DESC; cards A15's
 *   maps and A43's c); a traced reflection shades a hit with the hit's
 *   material values, the directional lights and the environment, as the
 *   path trace's first bounce does, with no further bounce.
 */

/* =====================================================================
 * aprendlit.h
 * ===================================================================== */

/**
 * [ADDED] APREND_LIT_SHADER_SURFACE_REFLECT_FRAGMENT = 8: the surface, as
 * APREND_LIT_SHADER_SURFACE_FRAGMENT, writing two more targets: at color
 * target 1 the shading normal (xyz, unit, in the scene's space, normal map
 * applied) and the roughness (w, after the ORM map), for an
 * SPUDGPU_FORMAT_R16G16B16A16_FLOAT target; at color target 2 (card A30's
 * c) the specular weight the environment's specular light is multiplied by
 * (f0 x scale + bias of the split sum, times the occlusion), linear, in
 * rgb of an SPUDGPU_FORMAT_R8G8B8A8_UNORM target; its alpha is written 0
 * and read by nothing, kept free for a later use that names it. A pipeline
 * with it has three color targets. A cut-out discards in all three.
 *
 * Why: A6's (1): a forward renderer has no G-buffer; a variant pays for
 * the targets only where a caller reflects. Unreal's G-buffer and Godot 4
 * Forward+'s normal-roughness buffer are the same data.
 */

/* =====================================================================
 * aprendcommands.h
 * ===================================================================== */

/**
 * [ADDED] APREND_COMMAND_LIT_SSR = 45: screen-space reflections, outside a
 * pass, into `_reflection`.
 *
 * Each texel of `_reflection` (half `_depth`'s size each way, rounded up)
 * takes the surface under it from `_depth` and `_normal_roughness` (of the
 * 2x2 texels it stands for, the one nearest the eye), reflects
 * the view ray about its normal, and marches the reflected ray over a Hi-Z
 * pyramid of `_depth` (built here, from the render target pool) for at most
 * `_max_steps` steps, up to `_max_distance` in the scene's units. A hit
 * within `_thickness` of the depth it passes reads `_color` (a mipped copy
 * of the opaque picture, APREND_COMMAND_COPY_SCENE_COLOR's) at the hit's
 * texel, at a mip from the roughness and the distance travelled. The texel
 * is written rgb the reflected radiance (unexposed: `_color` divided by
 * the view's exposure, as transmission reads it), a the confidence: 1 for a
 * hit well inside the screen, fading to 0 within `_edge_fade` of its edge,
 * as the ray faces the eye, and past `_max_roughness`; 0 for a miss.
 *
 * `_scene` gives the view (its view_projection and inverse, target size,
 * eye). `_depth` is a SHADER_RESOURCE 2D view of depth at the view's
 * target size; `_normal_roughness` a SHADER_RESOURCE 2D view of
 * R16G16B16A16_FLOAT of the same size; `_color` a SHADER_RESOURCE 2D view
 * of color with its mips; `_reflection` an UNORDERED_ACCESS 2D view of
 * R16G16B16A16_FLOAT, half the size rounded up. `_max_steps` 1 to 256;
 * `_max_distance` and `_thickness` above 0; `_edge_fade` 0 to 0.5 of the
 * screen; `_max_roughness` 0 to 1; all finite.
 *
 * Why: A6's (2) to (5): an opt-in command as BLOOM is; Hazel's SSR (a
 * compute pass over a Hi-Z buffer, a pre-convolved color, a fade at the
 * edges) and AMD's FidelityFX SSSR are the technique.
 */

/**
 * [ADDED] APREND_COMMAND_LIT_TRACE_REFLECTIONS = 46: ray-traced
 * reflections where `_reflection`'s confidence is short of 1, outside a
 * pass. The device has SPUDGPU_FEATURE_RAY_QUERY.
 *
 * Each texel of `_reflection` whose a is below 1 and whose roughness (from
 * `_normal_roughness`, at the texel nearest the eye of the 2x2 it stands
 * for, as LIT_SSR takes it) is at most `_max_roughness` traces `_ray_count`
 * rays from the surface along its reflection (the rough ones over a
 * GGX lobe, in blue-noise directions as LIT_SHADOW_MASK's), through
 * `_tlas` against the instances whose mask shares a bit with `_cull_mask`,
 * up to `_max_distance`. A hit is shaded with its material's values
 * (APREND_LIT_MATERIAL_VALUES' base color, emission, roughness, metalness,
 * specular: no maps, opaque), every directional light of `_scene`
 * (unshadowed), its point and spot lights, and the scene's ambient light;
 * a miss reads the scene's environment. The texel becomes
 * lerp(traced, rgb, a) with a 1: the march's hit is kept where it was sure.
 * Nothing is denoised: the result over a rough lobe is as noisy as
 * `_ray_count` rays make it (point 7 above), so `_max_roughness` here is
 * the caller's bound on that noise.
 *
 * Why: S12's decision, over the same inputs so a caller adds it to SSR or
 * uses it alone; Unreal's hybrid (screen traces, then hardware rays where
 * they fail) and Hazel's absence of it (Hazel has no hardware ray tracing).
 */

/**
 * [ADDED] APREND_COMMAND_LIT_REFLECT = 47: puts `_reflection` on `_target`,
 * outside a pass: at each texel of `_target` (the view's target size),
 * the reflection upsampled by depth and normal, and added as
 * specular x a x (reflection - the environment specular the surface used),
 * the specular from `_specular` (card A30's c), the environment specular
 * worked out again from `_scene`'s ambient light and environment at the
 * texel's normal and roughness, times the view's exposure. Worked out again
 * from an 8-bit specular and a 16-bit float normal, it cancels what the
 * surface added only to within their precision: at a of 1 the picture is
 * the reflection to within that, not bit for bit (at a of 0 nothing is
 * added, and the picture is unchanged bit for bit). `_target` is a
 * RENDER_TARGET view of the picture the surfaces wrote (linear, a float
 * format: the caller tone maps after, as with BLOOM).
 *
 * Why: card A30's c, the composite. A separate command so traced and marched
 * reflections, or either alone, reach the picture the same way.
 */

/*
 * [ADDED] The commands' parameters, in APREND_COMMAND's union (each within
 * its 120 bytes):
 *
 *   struct {
 *       aprend_lit_scene _scene;
 *       aprend_texture_view _depth;
 *       aprend_texture_view _normal_roughness;
 *       aprend_texture_view _color;
 *       aprend_texture_view _reflection;
 *       uint32_t _max_steps;
 *       float _max_distance;
 *       float _thickness;
 *       float _edge_fade;
 *       float _max_roughness;
 *   } _lit_ssr;                    // 64 bytes
 *
 *   struct {
 *       aprend_lit_scene _scene;
 *       aprend_tlas _tlas;
 *       aprend_texture_view _depth;
 *       aprend_texture_view _normal_roughness;
 *       aprend_texture_view _reflection;
 *       uint32_t _ray_count;
 *       uint32_t _cull_mask;
 *       float _max_distance;
 *       float _max_roughness;
 *   } _lit_trace_reflections;      // 56 bytes
 *
 *   struct {
 *       aprend_lit_scene _scene;
 *       aprend_texture_view _depth;
 *       aprend_texture_view _normal_roughness;
 *       aprend_texture_view _specular;     // card A30's c
 *       aprend_texture_view _reflection;
 *       aprend_texture_view _target;
 *   } _lit_reflect;                // 48 bytes
 *
 * Refused at send, each by its own message, as LIT_SHADOW_MASK's are: a
 * NULL or another instance's handle, a view of the wrong type, aspect,
 * format or size, a value out of its range; LIT_TRACE_REFLECTIONS on a
 * device without ray queries (APRESULT_UNSUPPORTED) or a TLAS never built;
 * any of the three inside a pass.
 */

/*
 * ---------------------------------------------------------------------------
 * ABI breaks (before 1.0; check.sh abi lists them)
 * ---------------------------------------------------------------------------
 *
 * - Added: APREND_LIT_SHADER_SURFACE_REFLECT_FRAGMENT (8),
 *   APREND_COMMAND_LIT_SSR (45), _LIT_TRACE_REFLECTIONS (46),
 *   _LIT_REFLECT (47), their three parameter structs in the union.
 * - Nothing changed or removed; APREND_COMMAND stays 128 bytes.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * ApricotFields (no device): the misuse harness regenerated; the refusals
 * that need no device are none (each command's checks read views).
 *
 * apricottests (Metal under validation with warnings fatal here; D3D12
 * and Vulkan in the handoff):
 * - A6's: a mirror floor (roughness 0, metal) under a known quad standing
 *   on it: the reflection's texels against its place worked out in C, on
 *   every backend; the edge fade, a miss (the sky behind), roughness past
 *   `_max_roughness` (the picture unchanged, bit for bit).
 * - A gold mirror reflects gold (A30's point): the reflected quad's texel
 *   times gold's f0, within the tolerance of test_lit_surface plus the
 *   composite's (the 8-bit specular and 16-bit normal it works the
 *   environment out again from): a tolerance, not an exact match.
 * - Half resolution on an edge: a quad's silhouette over the sky, each
 *   reflection texel along it the quad's (nearest the eye), never a blend.
 * - Confidence 0 everywhere: LIT_REFLECT leaves the picture bit for bit.
 * - LIT_TRACE_REFLECTIONS (ray queries): the quad behind the camera, which
 *   SSR misses, reflected by the trace; the same floor traced and marched
 *   agree within 2 where both see the quad; refused without ray queries.
 * - test_frame_allocations with SSR, the trace and the composite: none of
 *   Aprend's.
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 * - Hazel: SSR as a compute pass (SSR.glsl) over a Hi-Z buffer, a
 *   pre-convolved color and a visibility buffer, its normal and
 *   metalness-roughness from the geometry pass, options for steps, depth
 *   tolerance, fade-in and half resolution; SSR-Composite.glsl adds it.
 *   No hardware ray tracing.
 * - Godot 4 Forward+: a normal-roughness buffer from the depth pre-pass,
 *   SSR over last frame's picture (A30's a).
 * - Unreal: Lumen's screen traces first, hardware rays where they fail,
 *   its G-buffer holding the specular color (A30's c).
 * - AMD FidelityFX SSSR: Hi-Z march, confidence, a ray-traced fallback in
 *   its hybrid sample.
 * - The platform APIs: compute and storage images on all three; inline
 *   ray queries (SPUDGPU_EXT_RAY_QUERY) for the trace.
 */
