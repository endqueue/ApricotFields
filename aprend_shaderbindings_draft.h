/*
 * aprend_shaderbindings_draft.h - what SpudGPU's rule that a module declares
 * every binding of each set it reads (card S27, option a, decided 2026-11-01
 * with the user; the workspace's DECISIONS.md, "Decided") changes in
 * Aprend's contract.
 *
 * APPROVED 2026-11-01 by the user, as written here, with spudlib's
 * spudgpu_shaderbindings_draft.h. To be built into aprendpipeline.h; where
 * building finds this draft wrong or incomplete, a card says so and this
 * file is not edited to match.
 *
 * DRAFT, 2026-11-01. Not compilable: it repeats parts of
 * aprendpipeline.h's comments. Every change below is marked [ADDED],
 * [CHANGED] or [REMOVED]; nothing unmarked changes. No struct, enum or
 * signature changes, and no new APRESULT: SpudGPU's refusal reaches the
 * caller as "the code for SpudGPU's result" the two creates already list.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * aprend_graphics_pipeline_create() says "A layout may declare more than
 * the shaders use." On a device that takes a .metallib that is true only of
 * bindings the module declares: SpudGPU now refuses, there, a module whose
 * declarations for a set it reads aren't the layout's. A layout made by
 * aprend_binding_layout_create_from_shaders() holds every binding one of its
 * shaders uses, so a shader of the group that declares the set whole (a
 * shared bindings module) keeps the rule, and one that declares only what
 * it reads doesn't.
 *
 * Aprend doesn't check it itself: its reflection is the SPIR-V's
 * (aprendshader.h), which holds only what the entry point reads once a
 * compiler drops the rest, so Aprend can't see what the .metallib declares.
 * A package carrying the declared list would let it check on every backend,
 * and is not proposed: SpudGPU's check covers the backend where it matters,
 * and Aprend's own shaders are kept to the rule by its build.
 *
 * ---------------------------------------------------------------------------
 * aprendpipeline.h
 * ---------------------------------------------------------------------------
 */

/**
 * [CHANGED] aprend_graphics_pipeline_create()'s second checked rule:
 *
 * - Every binding a shader uses is declared by the binding layout at its
 *   set, with the same type, at least as many descriptors, and the shader's
 *   stage among its `_stages`. A layout may declare more than the shaders
 *   use, as long as each shader's module declares every binding of each set
 *   it reads, with the layout's type and count, and no other: SpudGPU's
 *   rule (`spudgpu_shader_module_desc`, "Every binding of a set"). A device
 *   that takes a .metallib refuses a module that doesn't, as SpudGPU's
 *   result; the others don't check it. Aprend's reflection doesn't see the
 *   bindings a compiler left out, so Aprend doesn't check it either.
 */
APRESULT aprend_graphics_pipeline_create(
    aprend_instance instance,
    const APREND_GRAPHICS_PIPELINE_DESC *desc,
    aprend_graphics_pipeline *out_pipeline,
    APREND_ERROR *out_error);

/**
 * [CHANGED] aprend_compute_pipeline_create(): "the shader doesn't match the
 * desc as aprend_graphics_pipeline_create() checks" now includes the rule
 * above, by reference; no new text of its own.
 */
APRESULT aprend_compute_pipeline_create(
    aprend_instance instance,
    const APREND_COMPUTE_PIPELINE_DESC *desc,
    aprend_compute_pipeline *out_pipeline,
    APREND_ERROR *out_error);

/**
 * [CHANGED] aprend_binding_layout_create_from_shaders(), after "A binding
 * declared and not used is left out, as the pipeline doesn't need it.":
 *
 * A module that declares such a binding is then refused on a device that
 * takes a .metallib, where a module declares exactly its sets' layouts
 * (aprend_graphics_pipeline_create()): declare in a set only what one of
 * the layout's shaders reads.
 */
APRESULT aprend_binding_layout_create_from_shaders(
    aprend_instance instance,
    const aprend_shader *shaders,
    uint32_t shader_count,
    uint32_t set,
    aprend_binding_layout *out_layout,
    APREND_ERROR *out_error);

/*
 * ---------------------------------------------------------------------------
 * ABI breaks
 * ---------------------------------------------------------------------------
 *
 * None. Behaviour: a pipeline on Metal that was created and read the wrong
 * members is refused instead.
 *
 * ---------------------------------------------------------------------------
 * Tests (apricottests)
 * ---------------------------------------------------------------------------
 *
 * - test_lit_sky, test_auto_exposure and test_batch_aptext_glyph pass on
 *   Metal (22 checks, both devices), unchanged.
 * - A caller's pair of shaders sharing a set made from shaders, one
 *   declaring only what it reads: refused on Metal with SpudGPU's code,
 *   created on Vulkan and D3D12.
 * - Every one of Aprend's own pipelines is created on Metal (each is, on
 *   first use, in the existing tests).
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 * Hazel builds its binding sets from each shader's reflection through
 * NVRHI, over D3D12 and Vulkan, which bind per binding: it never meets a
 * module shorter than its set. The difference is Metal's argument buffers
 * under SpudGPU's rule of no compiler in the library (card A9), and Aprend
 * only states it.
 */
