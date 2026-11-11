/*
 * rbtrend_shaderpackage_draft.h - Rend takes a shader package (card A9,
 * revised 2026-10-28; RBTREND_TODO.md item 22), the Rabbittus half of
 * the native shader formats (carrottus/crgpu_shaderformat_draft.h, presented
 * with this one). For review: prompts/ray_tracing.md, Phase 2.
 *
 * APPROVED 2026-10-31 by the user, as written here, with cards S17, S18,
 * S19 and S20 as recommended (the workspace's DECISIONS.md, "Decided").
 * Built into rbtrendshader.h and rbtcore.h in Phase 3; where building finds this draft wrong
 * or incomplete, a card says so and this file is not edited to match
 * (card S21: F4's sentence on Slang's own Metal output).
 *
 * DRAFT, 2026-10-31. Not compilable: it declares a struct and
 * a call beside rbtrendshader.h's, and names two it removes.
 *
 * ---------------------------------------------------------------------------
 * The shape, in one paragraph
 * ---------------------------------------------------------------------------
 *
 * A shader is made from a package: one entry point's code in the formats a
 * build made for it (SPIR-V always; DXIL on Windows; a .metallib on Apple
 * platforms), as Rabbittus' build step and every caller's write them.
 * Rend reflects the SPIR-V, as it does today, so RBTREND_SHADER_REFLECTION
 * is the same on every backend and needs no DXC or Metal reflection at run
 * time; it hands Carrottus GPU the format the device takes
 * (CRGPU_ADAPTER_PROPERTIES::shader_formats), with the entry point and the
 * workgroup size from that reflection. A package without the device's
 * format is refused, naming the format. Rend's own shaders are packages
 * embedded at build time; nothing is compiled or read from a file at run
 * time.
 *
 * ---------------------------------------------------------------------------
 * What this decides that no card has
 * ---------------------------------------------------------------------------
 *
 *  P1. SPIR-V is in every package, as the one source of Rend's
 *      reflection (A29: SPIRV-Cross stays for reflection where the native
 *      format has none). DXIL's own reflection is DXC's, a run-time
 *      dependency A9 removes; a .metallib has none of what
 *      RBTREND_SHADER_REFLECTION holds. It costs each package its SPIR-V's
 *      bytes on D3D12 and Metal builds (Rend's 45: about 400 KiB, to be
 *      measured in Phase 3).
 *  P2. One entry point name for every format in a package. Build steps
 *      name entry points other than "main", which Metal reserves (Slang
 *      renames it main_0 there): Rend's are named for their shader.
 *  P3. The workgroup size Carrottus GPU needs (card S17) is the SPIR-V's
 *      LocalSize, from the reflection Rend already makes; a shader whose
 *      size is a specialization constant is refused, as reflection reports
 *      0 today.
 *  P4. rbtrend_shader_read_from_file_spirv() goes. A package is several
 *      blobs, and reading files is the caller's (Carrottus Files); its only
 *      callers are Rend's own contract and misuse tests.
 *
 * ---------------------------------------------------------------------------
 * ABI breaks (abi.txt)
 * ---------------------------------------------------------------------------
 *
 *  - rbtrend_shader_create_spirv() and rbtrend_shader_read_from_file_spirv()
 *    removed; rbtrend_shader_create() and RBTREND_SHADER_PACKAGE added, with
 *    RBT_STRUCT_TYPE_REND_SHADER_PACKAGE.
 *  - Every caller: Rend's own (rbtrendbuiltin.cpp, rbtrendbatch.cpp,
 *    rbtrendlit.cpp) and every caller of rbtrend_shader_create_spirv() above
 *    Rabbittus (the workspace's ROADMAP.md lists them).
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 *  - rbtrend_contract_tests and rbt_misuse_tests: each refusal below, in
 *    parameter order; the misuse harness regenerated.
 *  - rabbittustests, every existing read-back test on D3D12 and Vulkan (and
 *    Metal on the Mac): each Slang shader's image against its GLSL
 *    predecessor's, within the tests' tolerances.
 *  - A package without the device's format: RBT_RESULT_UNSUPPORTED, the
 *    message naming the format.
 *  - Reflection: a package's reflection equals the old SPIR-V path's for
 *    every built-in shader (resources, members, inputs, workgroup size).
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 *  Hazel (.refs/HazelRenderer2025): its shader compiler keeps SPIR-V per
 *  stage and reflects it with SPIRV-Cross; NVRHI is handed the platform's
 *  binary. Unreal's FShaderCode carries the platform's bytecode and a
 *  parameter map made at cook time; bgfx's .bin carries the platform's
 *  binary with a uniform table. Each keeps one reflection for every
 *  platform, as P1 does.
 */

#ifndef RBTREND_SHADERPACKAGE_DRAFT_H
#define RBTREND_SHADERPACKAGE_DRAFT_H

#include "include/render/rbtrendshader.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief [ADDED] One entry point's code in the formats a build made for it.
 *
 * Every pointer is borrowed for rbtrend_shader_create(): the shader keeps
 * what it needs. A format the build didn't make is NULL with a size of 0.
 * Earns its place: Carrottus GPU takes one format per backend (A9), so a shader
 * made on any backend needs the code for that backend, and Rend's
 * reflection needs the SPIR-V (P1).
 */
typedef struct RBTREND_SHADER_PACKAGE {
	/** `type` is RBT_STRUCT_TYPE_REND_SHADER_PACKAGE; `next` is NULL.
	 * See RBT_STRUCT_BASE. */
	RBT_STRUCT_BASE base;
	/** The stage the entry point is for: CRGPU_SHADER_STAGE_VERTEX,
	 * _FRAGMENT, _COMPUTE or _MESH (the last only on a device created with
	 * CRGPU_FEATURE_MESH_SHADING), as today. */
	CRGPU_SHADER_STAGE stage;
	/** The entry point's name in every format here (P2). Required; not
	 * "main" where a .metallib is given. */
	const char *entry_point;
	/** SPIR-V words, 4-byte aligned. Required: Rend reflects it on every
	 * backend, and it is the module on Vulkan. */
	const void *spirv;
	/** Bytes in #spirv: not 0, a multiple of 4. */
	uint64_t spirv_size;
	/** DXIL, as DXC writes it (D3D12), 4-byte aligned, or NULL. */
	const void *dxil;
	/** Bytes in #dxil: 0 with NULL, else a multiple of 4. */
	uint64_t dxil_size;
	/** A .metallib (Metal), 4-byte aligned, or NULL. */
	const void *metallib;
	/** Bytes in #metallib: 0 with NULL. */
	uint64_t metallib_size;
} RBTREND_SHADER_PACKAGE;

/**
 * @brief [ADDED] Creates a shader from a package, and reflects its SPIR-V.
 *
 * Replaces rbtrend_shader_create_spirv(). Thread: as that call (rbtrendcontext.h,
 * "Threads").
 *
 * @param[in]  instance   Instance whose device the shader is created on.
 * @param[in]  package    The shader's code. Borrowed for the call.
 * @param[out] out_shader Receives the shader; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK. RBT_RESULT_INVALID_ARGUMENT, one check each in this
 *         order: @p instance NULL; @p package NULL or its base wrong;
 *         @p out_shader NULL; the stage not accepted; the entry point NULL
 *         or empty; the SPIR-V NULL, empty or not a multiple of 4; a
 *         native format's pointer and size disagreeing (one NULL or 0 and
 *         not the other) or its size not a multiple of 4 (DXIL); the SPIR-V
 *         can't be reflected (as today), or a compute or mesh shader's
 *         workgroup size is a specialization constant (P3).
 *         RBT_RESULT_UNSUPPORTED: a mesh shader on a device without the
 *         feature; the package has no code in the device's format (the
 *         message names it). RBT_RESULT_OUT_OF_MEMORY. Or the code for
 *         Carrottus GPU's result if the module can't be created (an entry point
 *         the native code lacks: CR_RESULT_GPU_SHADER_ENTRY_POINT_NOT_FOUND).
 */
RBT_RESULT rbtrend_shader_create(
    rbtrend_instance instance,
    const RBTREND_SHADER_PACKAGE *package,
    rbtrend_shader *out_shader,
    RBTREND_ERROR *out_error);

/* ---------------------------------------------------------------------------
 * [REMOVED] rbtrend_shader_create_spirv(): SPIR-V alone makes no module on
 *   D3D12 or Metal any more; a package with only SPIR-V is its Vulkan form.
 * [REMOVED] rbtrend_shader_read_from_file_spirv() (P4).
 * [ADDED] rbtcore.h: RBT_STRUCT_TYPE_REND_SHADER_PACKAGE = 5029 (the
 *   next free value after 5028, to be checked when built).
 * Unchanged: rbtrend_shader_destroy(), rbtrend_instance_get_fullscreen_vertex_shader(),
 *   RBTREND_SHADER_REFLECTION and its getters.
 * ------------------------------------------------------------------------- */

#ifdef __cplusplus
}
#endif

#endif /* RBTREND_SHADERPACKAGE_DRAFT_H */
