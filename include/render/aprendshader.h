
#ifndef APREND_SHADER_H
#define APREND_SHADER_H

/**
 * @file aprendshader.h
 * @brief Shaders, and what their SPIR-V declares: the reflection read from
 * it when a shader is created.
 *
 * A shader is one stage's SPIR-V. When it is created the SPIR-V is read for
 * what the stage declares - the descriptor sets and bindings it reads and
 * writes, the members of each uniform, storage and push constant block with
 * their offsets, the vertex inputs of a vertex shader, a compute or mesh
 * shader's workgroup size - and the shader keeps that for its lifetime.
 *
 * Aprend uses it in three places:
 *
 * - aprend_graphics_pipeline_create() and aprend_compute_pipeline_create()
 *   (aprendpipeline.h) check the binding layouts, push constant ranges and
 *   vertex bindings they are given against what the shaders declare, and
 *   refuse a pipeline that doesn't match, where before a mismatch was found
 *   on screen. A graphics pipeline can also take its vertex input locations
 *   from the shader's input names (APREND_GRAPHICS_PIPELINE_DESC::
 *   _vertex_inputs_by_name).
 * - aprend_binding_layout_create_from_shaders() (aprendpipeline.h) builds a
 *   layout from them, with the names and block members of its bindings,
 *   which is what lets a material (aprendmaterial.h) and a uniform buffer
 *   (aprend_binding_layout_get_uniform_layout()) be addressed by name.
 * - A command list checks the shape of each texture view a draw or dispatch
 *   reads (2D, 2D array, cube, cube array, 3D) against the one the shader
 *   declares at that binding.
 *
 * What is "used": a shader's SPIR-V may declare resources its entry point
 * never touches (shared headers do this). A resource or input is used if the
 * entry point reaches it, which is what the checks above hold a pipeline to:
 * a pipeline needn't declare what its shaders declare and don't use. When
 * the SPIR-V has more than one entry point for the shader's stage this can't
 * be told from the shader alone, and everything it declares counts as used
 * (APREND_SHADER_REFLECTION::entry_point is then NULL).
 *
 * Nothing here compiles or reflects a shader: the input is a package a
 * build made (cmake/aprend_slang.cmake).
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"): a shader is used by one
 * caller at a time with everything else of its instance. A shader's
 * reflection never changes after creation, so its get_ calls may overlap
 * each other, from any number of threads, while no call destroys the
 * shader.
 */

#include "aprendbuffers.h"
#include "aprendcontext.h"
#include "aprendimages.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @brief Opaque handle to one compiled shader stage.
 */
typedef struct aprend_shader_t *aprend_shader;

/**
 * @brief One entry point's code in the formats a build made for it.
 *
 * The package carries its reflection, made at build time from its SPIR-V by
 * tools/aprend_reflect (cmake/aprend_slang.cmake runs it), and the code in
 * each format the build made. SPIR-V is given where a Vulkan device may
 * take the package, DXIL where a D3D12 one may, a .metallib where a Metal
 * one may; a format it didn't make is NULL with a size of 0. The format the
 * device takes (SPUDGPU_ADAPTER_PROPERTIES::shader_formats) becomes the
 * shader's module, with the entry point and, for a compute or mesh shader,
 * the workgroup size the reflection holds. Every pointer is borrowed for
 * aprend_shader_create(): the shader keeps what it needs.
 */
typedef struct APREND_SHADER_PACKAGE {
	/** `type` is APRI_STRUCT_TYPE_APREND_SHADER_PACKAGE; `next` is NULL.
	 * See APRI_STRUCT_BASE. */
	APRI_STRUCT_BASE base;
	/** The stage the entry point is for: SPUDGPU_SHADER_STAGE_VERTEX,
	 * SPUDGPU_SHADER_STAGE_FRAGMENT, SPUDGPU_SHADER_STAGE_COMPUTE or
	 * SPUDGPU_SHADER_STAGE_MESH, the last only on a device created with
	 * SPUDGPU_FEATURE_MESH_SHADING. */
	SPUDGPU_SHADER_STAGE stage;
	/** The entry point's name in every format here, null-terminated.
	 * Required; not "main" where a .metallib is given (Metal reserves it). */
	const char *entry_point;
	/** SPIR-V words, 4-byte aligned, or NULL. */
	const void *spirv;
	/** Bytes in #spirv: 0 with NULL, else a multiple of 4. */
	uint64_t spirv_size;
	/** DXIL as DXC writes it, 4-byte aligned, or NULL. */
	const void *dxil;
	/** Bytes in #dxil: 0 with NULL, else a multiple of 4. */
	uint64_t dxil_size;
	/** A .metallib, 4-byte aligned, or NULL. */
	const void *metallib;
	/** Bytes in #metallib: 0 with NULL. */
	uint64_t metallib_size;
	/**
	 * The entry point's reflection, as tools/aprend_reflect wrote it from
	 * this package's SPIR-V, 4-byte aligned. Required. Its layout is
	 * Aprend's own and versioned: the tool of the same ApricotFields
	 * version writes what this one reads. It is checked whole before it is
	 * read, and nothing in it is a pointer.
	 */
	const void *reflection;
	/** Bytes in #reflection: not 0, a multiple of 4. */
	uint64_t reflection_size;
} APREND_SHADER_PACKAGE;

/**
 * @brief Creates a shader from a package.
 *
 * Nothing is compiled or reflected: the shader's module is the package's
 * code in the device's own format, and its reflection
 * (aprend_shader_get_reflection()) is the package's, the same on every
 * backend.
 *
 * @param[in]  instance   Instance whose device the shader is created on.
 * @param[in]  package    The shader's code and reflection. Borrowed for the call.
 * @param[out] out_shader Receives the shader; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. APRESULT_INVALID_ARGUMENT, checked in this order:
 *         @p instance NULL; @p package NULL (its base wrong: the code
 *         apri_struct_check() gives); @p out_shader NULL; the stage not one
 *         of the four (then a mesh shader on a device without the feature:
 *         APRESULT_UNSUPPORTED); the entry point NULL or empty; a format's
 *         pointer and size disagreeing (one NULL or 0 and not the other),
 *         or a SPIR-V or DXIL size not a multiple of 4; the reflection
 *         NULL, empty or not a multiple of 4; the entry point using an
 *         acceleration structure (Aprend's own traced passes alone bind
 *         one); a compute or mesh shader whose workgroup size is a
 *         specialization constant.
 *         APRESULT_CORRUPT_DATA: the reflection isn't one
 *         tools/aprend_reflect wrote (its magic, a count or an offset
 *         reaching past its end, a string not terminated inside it), or it
 *         is for another stage or entry point than the package names.
 *         APRESULT_APREND_REFLECTION_MISMATCH: it was written by a tool of
 *         another format version; the message names both. The caller
 *         builds its shaders again with this version's tool.
 *         APRESULT_UNSUPPORTED: a mesh shader on a device without the
 *         feature, or a package without the device's format (the message
 *         names the format). APRESULT_OUT_OF_MEMORY. Or the code for
 *         SpudGPU's result if the module can't be made.
 *
 * @see aprend_shader_destroy()
 */
APRESULT aprend_shader_create(
    aprend_instance instance,
    const APREND_SHADER_PACKAGE *package,
    aprend_shader *out_shader,
    APREND_ERROR *out_error);

/**
 * @brief Destroys a shader.
 *
 * A pipeline or binding layout made from it doesn't need it afterwards: what
 * they took from its reflection is theirs.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p shader itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] shader Shader to destroy. NULL is accepted and does nothing.
 */
void aprend_shader_destroy(aprend_shader shader);

/**
 * @brief Aprend's own full-target vertex shader, for a pass over the whole
 * of a target with a fragment shader of the caller's: a blur, a tone curve,
 * an outline, any pass that makes one picture from another. Such a pass is
 * the commands any pass is made of (aprendcommands.h): BEGIN_RENDERING on
 * the destination, a pipeline made with this shader and the caller's
 * fragment shader, a binding set of the source, a DRAW of 3 vertices.
 *
 * It draws one triangle that covers the target from the vertex index alone,
 * so a pipeline made with it has no vertex input (`_vertex_binding_count`
 * 0), its entry point `aprend_fullscreen_vert`. It writes no output but the
 * position. The fragment shader takes its place from its pixel position
 * (`SV_Position`, `gl_FragCoord` in GLSL), in pixels of the
 * target with the first row at the top on every backend, divided by the
 * target's size for a place in [0, 1] to sample at; a texture's rows start
 * at the top as well. An interpolated coordinate would not read the same
 * way up on every backend, which is why there is none.
 *
 * The shader is the instance's, made on the first call (or the first time
 * Aprend needs it itself) and destroyed with the instance. Don't pass it to
 * aprend_shader_destroy().
 *
 * @param[in]  instance   Instance the shader is of.
 * @param[out] out_shader Receives the shader; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance or
 *         @p out_shader is NULL; or the code it couldn't be created with.
 */
APRESULT aprend_get_fullscreen_vertex_shader(
    aprend_instance instance,
    aprend_shader *out_shader,
    APREND_ERROR *out_error);

/**
 * @brief The kind of number a shader reads or writes.
 */
typedef uint32_t APREND_SHADER_SCALAR_TYPE;
enum {
	/** Not a number of a kind below, or not read at all. */
	APREND_SHADER_SCALAR_TYPE_NONE   = 0,
	/** 32-bit float. */
	APREND_SHADER_SCALAR_TYPE_FLOAT  = 1,
	/** 32-bit signed integer. */
	APREND_SHADER_SCALAR_TYPE_INT    = 2,
	/** 32-bit unsigned integer. */
	APREND_SHADER_SCALAR_TYPE_UINT   = 3,
	/** Boolean. */
	APREND_SHADER_SCALAR_TYPE_BOOL   = 4,
	/** Anything else: a double, a 16- or 64-bit integer, a half. */
	APREND_SHADER_SCALAR_TYPE_OTHER  = 5
};

/**
 * @brief For APREND_SHADER_MEMBER::type: a member whose type has no
 * APREND_UNIFORM_TYPE, such as a double, a non-square matrix or a 16-bit
 * value. It is still listed, with its offset and size, and is written as
 * bytes (aprend_material_set_uniform_bytes()).
 */
#define APREND_UNIFORM_TYPE_UNKNOWN UINT32_MAX

/**
 * @brief One member of a uniform, storage or push constant block, as the
 * SPIR-V lays it out.
 *
 * A block's members are listed flat: a member that is a struct is listed as
 * its members, and an array of structs as the members of each element. A
 * member that is an array of a scalar, vector or matrix is one entry with
 * `array_count` elements; so is one Slang's SPIR-V wraps in a struct of its
 * own (`_Array_std140_...`, member `data`), which is listed by the member's
 * name as the source declares it.
 */
/* Frozen: an array element of APREND_SHADER_REFLECTION. It never changes; a new need gets a new type. */
typedef struct APREND_SHADER_MEMBER {
	/**
	 * The member's path inside its block, as GLSL would write it after the
	 * block's name: `color`, `light.position`, `lights[2].position`. The
	 * shader's own string; not NULL. An element of an array of arrays of
	 * scalars or vectors (`m[2][3]`) is listed as its outer elements, each an
	 * array of the innermost dimension (`m[0]`, `m[1]`).
	 */
	const char *name;
	/** The member's type, or APREND_UNIFORM_TYPE_UNKNOWN. */
	APREND_UNIFORM_TYPE type;
	/** Where it starts, in bytes from the start of the block. */
	uint32_t offset;
	/**
	 * Bytes it takes in the block, padding inside it included: an array's
	 * last element ends at #offset + #size. 0 for an array without a size.
	 */
	uint32_t size;
	/**
	 * Elements in the array; 1 for a member that is not an array. 0 for the
	 * array without a size that may end a storage block.
	 */
	uint32_t array_count;
	/** Bytes from one element to the next; 0 for a member that is not an array. */
	uint32_t array_stride;
	/** Bytes from one column of a matrix to the next (from one row, if #row_major); 0 for a member that is not a matrix. */
	uint32_t matrix_stride;
	/** The matrix is stored row by row (`layout(row_major)`). */
	bool row_major;
	/**
	 * Whether the entry point reads or writes the member: some part of the
	 * top-level member of its block it belongs to. Every member, if the
	 * entry point can't be told (APREND_SHADER_REFLECTION::entry_point is
	 * NULL).
	 */
	bool used;
} APREND_SHADER_MEMBER;

/**
 * @brief One descriptor binding a shader reads or writes: `layout(set = S,
 * binding = B)` in GLSL.
 */
/* Frozen: an array element of APREND_SHADER_REFLECTION. It never changes; a new need gets a new type. */
typedef struct APREND_SHADER_RESOURCE {
	/**
	 * The variable's name: `u_Albedo`, or a block's instance name
	 * (`uniform Material { ... } u_Material` is `u_Material`). A block with no
	 * instance name has its block name here. The shader's own string; not
	 * NULL, and empty only if the SPIR-V was stripped of names.
	 */
	const char *name;
	/**
	 * For a uniform or storage buffer, the block's name (`Material` above;
	 * `ConstantBuffer<Material>` in Slang, without the `_std140` Slang's
	 * SPIR-V adds); for a Slang structured or byte address buffer, whose
	 * block is the buffer type's own, the variable's name. NULL for
	 * anything else.
	 */
	const char *block_name;
	/** The descriptor set: `set = S`. */
	uint32_t set;
	/** The binding: `binding = B`. */
	uint32_t binding;
	/** What the binding holds. */
	SPUDGPU_DESCRIPTOR_TYPE type;
	/** Descriptors in the binding: 1, or N for an array of N (`u_Textures[N]`). */
	uint32_t count;
	/**
	 * For a uniform or storage buffer, the bytes of the block as declared;
	 * for a storage block that ends in an array without a size, the bytes
	 * before that array. 0 for anything else.
	 */
	uint32_t block_size;
	/**
	 * For a storage block that ends in an array without a size, the bytes of
	 * one of its elements; 0 otherwise.
	 */
	uint32_t runtime_array_stride;
	/** For a block, where its members start in APREND_SHADER_REFLECTION::members. */
	uint32_t first_member;
	/** For a block, how many members it has there; 0 for anything else. */
	uint32_t member_count;
	/**
	 * For an image (sampled, storage, or combined with a sampler), the
	 * shape of the view the binding takes: APREND_TEXTURE_VIEW_SHAPE_2D for a
	 * `sampler2D`, _2D_ARRAY for a `sampler2DArray`, _CUBE, _CUBE_ARRAY or
	 * _3D. APREND_TEXTURE_VIEW_SHAPE_NONE for anything else.
	 */
	APREND_TEXTURE_VIEW_SHAPE image_shape;
	/**
	 * For an image, what a read gives: FLOAT for a `sampler2D`, INT for an
	 * `isampler2D`, UINT for a `usampler2D` (which is how a stencil is
	 * read). NONE for anything else.
	 */
	APREND_SHADER_SCALAR_TYPE image_scalar_type;
	/**
	 * For a sampled image, whether it is read with a depth comparison
	 * (`sampler2DShadow`; in Slang, whose SPIR-V leaves an image's depth
	 * unknown, a texture its code samples with `SampleCmp`): the sampler
	 * given with it then compares.
	 */
	bool image_depth_compare;
	/**
	 * For a storage buffer or image, whether the shader only reads it: it
	 * is declared `readonly`, or no code of the module writes through it
	 * (a Slang RW type not declared `readonly` is writable in its SPIR-V).
	 */
	bool read_only;
	/** Whether the entry point uses the binding (see "used" at the top of this header). */
	bool used;
} APREND_SHADER_RESOURCE;

/**
 * @brief One input of a vertex shader: `layout(location = L) in ...`.
 */
/* Frozen: an array element of APREND_SHADER_REFLECTION. It never changes; a new need gets a new type. */
typedef struct APREND_SHADER_INPUT {
	/**
	 * The variable's name, or for a field of a struct parameter (Slang's
	 * `VertexInput input`, `input.position`) the field's (`position`). The
	 * shader's own string; not NULL.
	 */
	const char *name;
	/** Its first location. */
	uint32_t location;
	/**
	 * Locations it takes: 1, or the columns of a matrix (a `mat4` takes four,
	 * one `vec4` each).
	 */
	uint32_t location_count;
	/** The kind of number it reads. */
	APREND_SHADER_SCALAR_TYPE scalar_type;
	/** Components in one location: 1 for a `float`, 3 for a `vec3`. */
	uint32_t components;
	/** Whether the entry point reads it. */
	bool used;
} APREND_SHADER_INPUT;

/**
 * @brief What one shader's SPIR-V declares. Read when the shader is created
 * and the shader's for its lifetime.
 */
/* Frozen: Aprend's own, read through the pointer aprend_shader_get_reflection() returns; a new need gets a new getter. */
typedef struct APREND_SHADER_REFLECTION {
	/** The shader's stage. */
	SPUDGPU_SHADER_STAGE stage;
	/**
	 * The name of the SPIR-V's one entry point for #stage, which is the one
	 * a pipeline has to name. NULL if the SPIR-V has more than one for the
	 * stage: then every declared resource and input counts as used.
	 */
	const char *entry_point;
	/** The descriptor bindings, ordered by set and then binding. */
	const APREND_SHADER_RESOURCE *resources;
	/** Number of elements in #resources. */
	uint32_t resource_count;
	/** The members of every block, the resources' and the push constant block's. */
	const APREND_SHADER_MEMBER *members;
	/** Number of elements in #members. */
	uint32_t member_count;
	/** A vertex shader's inputs, ordered by location; none for any other stage. */
	const APREND_SHADER_INPUT *inputs;
	/** Number of elements in #inputs. */
	uint32_t input_count;
	/**
	 * Whether the shader has a push constant block (`layout(push_constant)`)
	 * its entry point uses. One it only declares is not reflected.
	 */
	bool has_push_constants;
	/** The push constant block's instance name, or its block name; NULL without one. */
	const char *push_constant_name;
	/**
	 * The first byte of a member the entry point uses: a push constant range
	 * for this stage has to start there or before.
	 */
	uint32_t push_constant_begin;
	/**
	 * Where the last member the entry point uses ends: a range for this
	 * stage has to reach there. Each member's bytes in between are covered
	 * only if the member is used (APREND_SHADER_MEMBER::used), which is what
	 * lets two stages share one block and each take part of it.
	 */
	uint32_t push_constant_end;
	/** Where the block's members start in #members. */
	uint32_t push_constant_first_member;
	/** How many members the block has. */
	uint32_t push_constant_member_count;
	/**
	 * A fragment shader's color outputs: one past the highest location it
	 * writes, 0 for a shader that writes none. 0 for any other stage.
	 */
	uint32_t color_output_count;
	/**
	 * A compute or mesh shader's workgroup size (`local_size_x` and so on).
	 * 0 in each for any other stage, or one whose size is a specialization
	 * constant.
	 */
	uint32_t workgroup_size[3];
} APREND_SHADER_REFLECTION;

/**
 * @brief Gets what a shader's SPIR-V declares.
 *
 * @param[in] shader Shader to read.
 *
 * @return The reflection, owned by the shader and valid until it is
 *         destroyed; NULL if @p shader is NULL.
 */
const APREND_SHADER_REFLECTION *aprend_shader_get_reflection(aprend_shader shader);

/**
 * @brief Finds one of a shader's resources by name.
 *
 * @param[in] shader Shader to search.
 * @param[in] name   The resource's name (APREND_SHADER_RESOURCE::name), or
 *                   a block's name (APREND_SHADER_RESOURCE::block_name).
 *                   Case-sensitive.
 *
 * @return The first resource of that name, owned by the shader; NULL if
 *         there is none, or if an argument is NULL.
 */
const APREND_SHADER_RESOURCE *aprend_shader_find_resource(
    aprend_shader shader,
    const char *name);

/**
 * @brief Gets the stage a shader was created for.
 *
 * @param[in] shader Shader to read.
 *
 * @return The stage, or SPUDGPU_SHADER_STAGE_NONE if @p shader is NULL.
 */
SPUDGPU_SHADER_STAGE aprend_shader_get_stage(aprend_shader shader);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_SHADER_H
