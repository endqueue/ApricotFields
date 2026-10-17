
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
 *   from the shader's input names (aprend_graphics_pipeline_desc::
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
 * (aprend_shader_reflection::entry_point is then NULL).
 *
 * Nothing here compiles a shader or reads GLSL: the input is SPIR-V
 * (APREND_TODO.md, item 22).
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A shader's reflection never
 * changes after creation, so it may be read from any number of threads at
 * once while no thread destroys the shader.
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
 * @brief Creates a shader from a SPIR-V file.
 *
 * The file is read whole during the call and not kept open. The SPIR-V is
 * reflected as aprend_shader_create_spirv() does.
 *
 * @param[in] instance     Instance whose device the shader is created on.
 * @param[in] filename     Path of the SPIR-V file.
 * @param[in] shader_stage The one stage the code is for. Only
 *                         SPUDGPU_SHADER_STAGE_VERTEX,
 *                         SPUDGPU_SHADER_STAGE_FRAGMENT,
 *                         SPUDGPU_SHADER_STAGE_COMPUTE and
 *                         SPUDGPU_SHADER_STAGE_MESH are accepted.
 *
 * @return The shader, or NULL if @p instance or @p filename is NULL,
 *         @p shader_stage is not accepted, the file doesn't exist or can't be
 *         read, the SPIR-V can't be reflected (see
 *         aprend_shader_create_spirv()), or the shader module can't be
 *         created.
 *
 * @see aprend_shader_create_spirv()
 * @see aprend_shader_destroy()
 */
aprend_shader aprend_shader_read_from_file_spirv(
    aprend_instance instance,
    const char *filename,
    SPUDGPU_SHADER_STAGE shader_stage);

/**
 * @brief Creates a shader from SPIR-V already in memory, such as code embedded
 * in the caller's binary, and reflects it.
 *
 * The code is copied, so @p spirv_code only has to live for this call.
 *
 * @param[in] instance     Instance whose device the shader is created on.
 * @param[in] spirv_code   The SPIR-V words.
 * @param[in] spirv_size   Size of @p spirv_code in bytes. A multiple of 4, and
 *                         not 0.
 * @param[in] shader_stage The one stage the code is for. Only
 *                         SPUDGPU_SHADER_STAGE_VERTEX,
 *                         SPUDGPU_SHADER_STAGE_FRAGMENT,
 *                         SPUDGPU_SHADER_STAGE_COMPUTE and
 *                         SPUDGPU_SHADER_STAGE_MESH are accepted.
 *
 * @return The shader, or NULL if @p instance or @p spirv_code is NULL,
 *         @p spirv_size is 0 or not a multiple of 4, @p shader_stage is not
 *         accepted, the shader module can't be created, or the SPIR-V can't
 *         be reflected: it isn't valid SPIR-V, it has no entry point for
 *         @p shader_stage, or its entry point uses a resource Aprend has no
 *         binding for (a texel buffer, an input attachment, an acceleration
 *         structure, an array of descriptors without a size, an image of a
 *         dimension no texture view has). Each is printed.
 *
 * @see aprend_shader_destroy()
 */
aprend_shader aprend_shader_create_spirv(
    aprend_instance instance,
    const void *spirv_code,
    uint64_t spirv_size,
    SPUDGPU_SHADER_STAGE shader_stage);

/**
 * @brief Destroys a shader.
 *
 * A pipeline or binding layout made from it doesn't need it afterwards: what
 * they took from its reflection is theirs.
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
 * 0), its entry point `main`. It writes no output but the position. The
 * fragment shader takes its place from `gl_FragCoord.xy`, in pixels of the
 * target with the first row at the top on every backend, divided by the
 * target's size for a place in [0, 1] to sample at; a texture's rows start
 * at the top as well. An interpolated coordinate would not read the same
 * way up on every backend, which is why there is none.
 *
 * The shader is the instance's, made on the first call (or the first time
 * Aprend needs it itself) and destroyed with the instance. Don't pass it to
 * aprend_shader_destroy().
 *
 * @param[in] instance Instance the shader is of.
 *
 * @return The shader, or NULL if @p instance is NULL or it can't be
 *         created (printed).
 */
aprend_shader aprend_get_fullscreen_vertex_shader(aprend_instance instance);

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
 * @brief For aprend_shader_member::type: a member whose type has no
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
 * #array_count elements.
 */
typedef struct aprend_shader_member {
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
	 * entry point can't be told (aprend_shader_reflection::entry_point is
	 * NULL).
	 */
	bool used;
} aprend_shader_member;

/**
 * @brief One descriptor binding a shader reads or writes: `layout(set = S,
 * binding = B)` in GLSL.
 */
typedef struct aprend_shader_resource {
	/**
	 * The variable's name: `u_Albedo`, or a block's instance name
	 * (`uniform Material { ... } u_Material` is `u_Material`). A block with no
	 * instance name has its block name here. The shader's own string; not
	 * NULL, and empty only if the SPIR-V was stripped of names.
	 */
	const char *name;
	/**
	 * For a uniform or storage buffer, the block's name (`Material` above);
	 * NULL for anything else.
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
	/** For a block, where its members start in aprend_shader_reflection::members. */
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
	 * (`sampler2DShadow`): the sampler given with it then compares.
	 */
	bool image_depth_compare;
	/** For a storage buffer or image, whether the shader only reads it (`readonly`). */
	bool read_only;
	/** Whether the entry point uses the binding (see "used" at the top of this header). */
	bool used;
} aprend_shader_resource;

/**
 * @brief One input of a vertex shader: `layout(location = L) in ...`.
 */
typedef struct aprend_shader_input {
	/** The variable's name. The shader's own string; not NULL. */
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
} aprend_shader_input;

/**
 * @brief What one shader's SPIR-V declares. Read when the shader is created
 * and the shader's for its lifetime.
 */
typedef struct aprend_shader_reflection {
	/** The shader's stage. */
	SPUDGPU_SHADER_STAGE stage;
	/**
	 * The name of the SPIR-V's one entry point for #stage, which is the one
	 * a pipeline has to name. NULL if the SPIR-V has more than one for the
	 * stage: then every declared resource and input counts as used.
	 */
	const char *entry_point;
	/** The descriptor bindings, ordered by set and then binding. */
	const aprend_shader_resource *resources;
	/** Number of elements in #resources. */
	uint32_t resource_count;
	/** The members of every block, the resources' and the push constant block's. */
	const aprend_shader_member *members;
	/** Number of elements in #members. */
	uint32_t member_count;
	/** A vertex shader's inputs, ordered by location; none for any other stage. */
	const aprend_shader_input *inputs;
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
	 * only if the member is used (aprend_shader_member::used), which is what
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
} aprend_shader_reflection;

/**
 * @brief Gets what a shader's SPIR-V declares.
 *
 * @param[in] shader Shader to read.
 *
 * @return The reflection, owned by the shader and valid until it is
 *         destroyed; NULL if @p shader is NULL.
 */
const aprend_shader_reflection *aprend_shader_get_reflection(aprend_shader shader);

/**
 * @brief Finds one of a shader's resources by name.
 *
 * @param[in] shader Shader to search.
 * @param[in] name   The resource's name (aprend_shader_resource::name), or
 *                   a block's name (aprend_shader_resource::block_name).
 *                   Case-sensitive.
 *
 * @return The first resource of that name, owned by the shader; NULL if
 *         there is none, or if an argument is NULL.
 */
const aprend_shader_resource *aprend_shader_find_resource(
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
