/*
 * rbtrend_shaderreflection_draft.h - a shader package carries its reflection,
 * made at build time, so Rend reads no SPIR-V at run time and SPIRV-Cross
 * leaves the library (card X18, option b; the workspace's DECISIONS.md).
 *
 * APPROVED 2026-11-01 by the user, as written here. To be built after
 * card X18's first step; where building finds this draft wrong or
 * incomplete, a card says so and this file is not edited to match.
 *
 * DRAFT, 2026-11-01. Not compilable: it repeats
 * parts of rbtrendshader.h and rbtcore.h. Every change below is marked
 * [ADDED], [CHANGED] or [REMOVED]; nothing unmarked changes.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * rbtrend_shader_create() reflects the package's SPIR-V with SPIRV-Cross on
 * every backend, so:
 * - SPIRV-Cross's core is linked into Rabbittus, and the library takes
 *   it from whichever repository made the target first
 *   (CMakeLists.txt's `if (NOT TARGET spirv-cross-core)`): Carrottus's
 *   crgpu_msl block where it ran, its own fetch elsewhere. Card X1c stops
 *   that block running on Windows and Linux, which moves the target there
 *   without anyone choosing it.
 * - Every package embeds its SPIR-V, on a device that takes DXIL or a
 *   .metallib too: Rend's 45 shaders, and every caller's, carry two
 *   formats on Apple and Windows where one runs.
 * - The same SPIR-V is reflected again at every run, for every shader, on
 *   the first use of each.
 *
 * The reflection is a function of the SPIR-V alone, and the SPIR-V is a
 * build output, so the reflection can be one too. src/render/rbtrendreflect.cpp
 * (rbtrend_reflect_spirv(), 749 lines over SPIRV-Cross) moves, unchanged in
 * what it computes, into a build-time tool, tools/rbtrend_reflect, which the
 * shader step (cmake/rbtrend_slang.cmake) runs on each SPIR-V it makes and
 * whose output it embeds in the package. Rend reads that output: a flat
 * table of plain data, versioned, checked, never parsed as text.
 *
 * The tool reads SPIR-V, not Slang's -reflection-json: a caller's SPIR-V
 * from any compiler still works, and Rend never depends on Slang's JSON
 * format, which is Slang's and changes with it. (Card S27's padding reads
 * the JSON because it needs bindings the SPIR-V dropped; Rend's
 * reflection is of what the entry point uses, which the SPIR-V has.)
 *
 * What a caller's build changes: nothing, where it uses cmake/rbtrend_slang.cmake
 * (Rabbittus offers it for that). A caller making
 * packages by hand runs tools/rbtrend_reflect beside its compiler.
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 * - Hazel reflects SPIR-V with SPIRV-Cross when compiling
 *   (Platform/Vulkan/ShaderCompiler) and writes the result into its shader
 *   pack beside the code (Renderer/ShaderPack.cpp, ReflectionDataOffset),
 *   which a shipped build reads instead of reflecting. Unreal's cooked
 *   shader maps keep their parameter maps the same way.
 * - D3D12 and Metal have no SPIR-V: a runtime that reflects SPIR-V needs it
 *   shipped beside the native code for no other reason.
 * - SPIRV-Reflect, slang's reflection API, DXC's ID3D12ShaderReflection:
 *   every one of them is a compile-time tool in the engines above.
 *
 * ---------------------------------------------------------------------------
 * ABI breaks (before 1.0, X2; listed by ./check.sh abi)
 * ---------------------------------------------------------------------------
 *
 * - RBTREND_SHADER_PACKAGE gains two members at its end; its size changes.
 * - #spirv becomes optional (NULL with a size of 0 where no Vulkan device
 *   will take the package); a package that gave SPIR-V and no reflection is
 *   now refused.
 * - RBT_RESULT gains RBT_RESULT_REND_REFLECTION_MISMATCH (5004).
 * - rbtrend_shader_get_reflection() and RBTREND_SHADER_REFLECTION are
 *   unchanged: what the call returns is the same view, read from the
 *   package instead of made from the SPIR-V.
 *
 * ---------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------------
 *
 * - tests/rbtrendreflect_tests.cpp keeps its cases, run against the tool's
 *   output read back by Rend's reader: every case's view the same as
 *   rbtrend_reflect_spirv()'s today, field for field (the brute-force
 *   comparison, run once while both exist, then the old path goes).
 * - A package with no reflection; one whose size isn't a multiple of 4; one
 *   whose magic is wrong (RBT_RESULT_CORRUPT_DATA); one from another format
 *   version (RBT_RESULT_REND_REFLECTION_MISMATCH); one whose stage differs
 *   from the package's; one truncated at every offset (CORRUPT_DATA, never a
 *   read past its end, under -asan).
 * - A package with no SPIR-V created on a Metal and a D3D12 device; refused
 *   on a Vulkan one (RBT_RESULT_UNSUPPORTED, as a missing format is today).
 * - The reader is a fuzz target (tests/fuzz/, rbtrend_reflection_fuzz): it
 *   reads bytes a caller vouches for only by having built them.
 * - apricottests' whole suite unchanged, on Metal, D3D12 and Vulkan.
 * - nm on a Release build: no spirv_cross symbol in Rabbittus.
 *
 * ---------------------------------------------------------------------------
 * rbtrendshader.h
 * ---------------------------------------------------------------------------
 */

/**
 * @brief One entry point's code in the formats a build made for it.
 *
 * [CHANGED] The package carries its reflection, made at build time from its
 * SPIR-V by tools/rbtrend_reflect (cmake/rbtrend_slang.cmake runs it), and
 * the code in each format the build made. SPIR-V is given where a Vulkan
 * device may take the package, DXIL where a D3D12 one may, a .metallib
 * where a Metal one may; a format it didn't make is NULL with a size of 0.
 * The format the device takes (CRGPU_ADAPTER_PROPERTIES::shader_formats)
 * becomes the shader's module, with the entry point and, for a compute or
 * mesh shader, the workgroup size the reflection holds. Every pointer is
 * borrowed for rbtrend_shader_create(): the shader keeps what it needs.
 */
typedef struct RBTREND_SHADER_PACKAGE {
	RBT_STRUCT_BASE base;
	CRGPU_SHADER_STAGE stage;
	const char *entry_point;
	/** [CHANGED] SPIR-V words, 4-byte aligned, or NULL. (Was required.) */
	const void *spirv;
	/** [CHANGED] Bytes in #spirv: 0 with NULL, else a multiple of 4. */
	uint64_t spirv_size;
	const void *dxil;
	uint64_t dxil_size;
	const void *metallib;
	uint64_t metallib_size;
	/**
	 * [ADDED] The entry point's reflection, as tools/rbtrend_reflect wrote it
	 * from this package's SPIR-V, 4-byte aligned. Required. Its layout is
	 * Rend's own and versioned: the tool of the same Rabbittus
	 * version writes what this one reads. It is checked whole before it is
	 * read, and nothing in it is a pointer.
	 */
	const void *reflection;
	/** [ADDED] Bytes in #reflection: not 0, a multiple of 4. */
	uint64_t reflection_size;
} RBTREND_SHADER_PACKAGE;

/**
 * @brief Creates a shader from a package.
 *
 * [CHANGED] Nothing is compiled or reflected: the shader's module is the
 * package's code in the device's own format, and its reflection
 * (rbtrend_shader_get_reflection()) is the package's, the same on every
 * backend.
 *
 * @return RBT_RESULT_OK. RBT_RESULT_INVALID_ARGUMENT, checked in this order:
 *         @p instance NULL; @p package NULL (its base wrong: the code
 *         rbt_struct_check() gives); @p out_shader NULL; the stage not one
 *         of the four (then a mesh shader on a device without the feature:
 *         RBT_RESULT_UNSUPPORTED); the entry point NULL or empty;
 *         [CHANGED] a format's pointer and size disagreeing (one NULL or 0
 *         and not the other), or a SPIR-V or DXIL size not a multiple of 4;
 *         [ADDED] the reflection NULL, empty or not a multiple of 4;
 *         a compute or mesh shader whose workgroup size is a
 *         specialization constant.
 *         [ADDED] RBT_RESULT_CORRUPT_DATA: the reflection isn't one
 *         tools/rbtrend_reflect wrote (its magic, a count or an offset
 *         reaching past its end, a string not terminated inside it), or it
 *         is for another stage or entry point than the package names.
 *         [ADDED] RBT_RESULT_REND_REFLECTION_MISMATCH: it was written by a
 *         tool of another format version; the message names both. The
 *         caller builds its shaders again with this version's tool.
 *         [REMOVED] "the SPIR-V can't be reflected (...)": the tool refuses
 *         that SPIR-V when the shader is built, with the same reasons (a
 *         texel buffer, an input attachment, an acceleration structure
 *         outside Rend's own layouts, an array of descriptors without a
 *         size, an image of a dimension no texture view has), so the build
 *         fails, not the run.
 *         RBT_RESULT_UNSUPPORTED: a mesh shader on a device without the
 *         feature, or a package without the device's format (the message
 *         names the format). RBT_RESULT_OUT_OF_MEMORY. Or the code for
 *         Carrottus GPU's result if the module can't be made.
 */
RBT_RESULT rbtrend_shader_create(
    rbtrend_instance instance,
    const RBTREND_SHADER_PACKAGE *package,
    rbtrend_shader *out_shader,
    RBTREND_ERROR *out_error);

/*
 * [CHANGED] The file comment's "Nothing here compiles a shader or reads
 * GLSL: the input is SPIR-V" becomes "Nothing here compiles or reflects a
 * shader: the input is a package a build made (cmake/rbtrend_slang.cmake)."
 *
 * ---------------------------------------------------------------------------
 * rbtcore.h
 * ---------------------------------------------------------------------------
 */

/**
 * [ADDED] A shader package's reflection was written by tools/rbtrend_reflect
 * of another format version than this library reads. Rebuilding the
 * caller's shaders with this version's tool fixes it.
 */
RBT_RESULT_REND_REFLECTION_MISMATCH = 5004,
