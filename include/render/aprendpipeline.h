
#ifndef APREND_PIPELINE_H
#define APREND_PIPELINE_H

/**
 * @file aprendpipeline.h
 * @brief Graphics and compute pipelines, and the binding layouts and sets
 * that give a pipeline's shaders their buffers, textures and samplers.
 *
 * Shaders, and what their SPIR-V declares, are in aprendshader.h. A
 * pipeline is checked against its shaders when it is created: see
 * aprend_graphics_pipeline_create(). A binding layout can be written by hand
 * or made from shaders (aprend_binding_layout_create_from_shaders()); one
 * made from shaders also knows the names of its bindings and the members of
 * its blocks.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). Shaders, pipelines, binding
 * layouts and binding sets are each used by one caller at a time with
 * everything else of their instance. A binding layout's sets share the
 * layout's descriptor pools, and a destroyed set is given back to them
 * inside a later call on the instance, so sets of different layouts are not
 * independent of each other either. The blend presets,
 * aprend_vertex_element_type_get_size() and
 * aprend_vertex_layout_get_element_index() take no handle, so they may run
 * at once with anything.
 */

#include "aprendbuffers.h"
#include "aprendimages.h"
#include "aprendshader.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @brief Upper bound on the bindings one aprend_binding_layout can declare.
 */
#define APREND_MAX_BINDINGS_PER_LAYOUT SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET

/**
 * @brief Upper bound on the set slots one pipeline can declare.
 */
#define APREND_MAX_BINDING_LAYOUTS SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS

/**
 * @brief One slot shaders read or write: `layout(set = N, binding = _binding)`
 * in GLSL.
 *
 * N is not stated here. It is the index a pipeline puts the layout at, in
 * APREND_GRAPHICS_PIPELINE_DESC::_binding_layouts.
 */
/* Frozen: an array element of a binding layout. It never changes; a new need gets a new type. */
typedef struct APREND_BINDING_DESC {
	/** Slot index, `binding = ...` in GLSL. Unique within a layout. */
	uint32_t _binding;
	/** What the slot holds. */
	SPUDGPU_DESCRIPTOR_TYPE _type;
	/**
	 * 1 for a single resource, N for an array of N (`... name[N]` in GLSL).
	 * Never 0.
	 */
	uint32_t _count;
	/**
	 * Bitmask of the shader stages that use the slot. Never
	 * SPUDGPU_SHADER_STAGE_NONE.
	 * @see SPUDGPU_SHADER_STAGE
	 */
	SPUDGPU_SHADER_STAGE _stages;
	/**
	 * A name the slot is found by (aprend_binding_layout_find()), such as the
	 * shader's variable name. May be NULL. Copied.
	 */
	const char *_name;
} APREND_BINDING_DESC;

/**
 * @brief Opaque handle to the slots of one descriptor set.
 *
 * Shared by every pipeline that declares it and every aprend_binding_set
 * created from it.
 *
 * @par Layouts of the same bindings
 * Two layouts of one instance whose bindings are the same - each binding's
 * number, type, count and stages, in any order - are one layout to
 * everything that takes a layout: a set created from either may be put where
 * a pipeline declares the other, and a material made for one is drawn with
 * a pipeline that declares the other. They share one SpudGPU layout and one
 * set of descriptor pools, so this holds on every backend. Names don't take
 * part (`_name`, and a layout made from shaders' names and blocks): each
 * layout keeps its own, and they are what aprend_binding_layout_find() and a
 * material read. A binding's stages do take part: a layout whose binding is
 * given to the vertex stage alone is another layout from one that gives it
 * to vertex and fragment.
 *
 * So pipelines that bind the same sets share them by declaring layouts of
 * the same bindings, made however the caller likes: the same handle, or
 * layouts made separately. For a layout made from shaders, make it from
 * every shader that will read the set
 * (aprend_binding_layout_create_from_shaders()), and give that layout to
 * each pipeline that binds the set, a pipeline whose shaders read less
 * than it declares included (a depth pre-pass of the main pass's materials,
 * say): a pipeline may declare more than its shaders use.
 */
typedef struct aprend_binding_layout_t *aprend_binding_layout;

/**
 * @brief Creates a binding layout.
 *
 * @p bindings is copied, so it only has to live for this call.
 *
 * @param[in] instance      Instance whose device the layout is created on.
 * @param[in] bindings      The slots, in any order.
 * @param[in] binding_count Number of elements in @p bindings. From 1 to
 *                          APREND_MAX_BINDINGS_PER_LAYOUT.
 * @param[out] out_layout   Receives the layout; NULL on failure.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance,
 *         @p bindings or @p out_layout is NULL, @p binding_count is 0 or
 *         above APREND_MAX_BINDINGS_PER_LAYOUT, two entries share a
 *         `_binding`, or an entry has a `_count` of 0, no `_stages`, or a
 *         `_type` that is not a SPUDGPU_DESCRIPTOR_TYPE;
 *         APRESULT_OUT_OF_MEMORY; or the code for SpudGPU's result if the
 *         layout can't be created.
 *
 * @see aprend_binding_layout_destroy()
 * @see aprend_binding_set_create()
 */
APRESULT aprend_binding_layout_create(
    aprend_instance instance,
    const APREND_BINDING_DESC *bindings,
    uint32_t binding_count,
    aprend_binding_layout *out_layout,
    APREND_ERROR *out_error);

/**
 * @brief Creates the binding layout of one set from what shaders declare
 * there.
 *
 * Every binding of set @p set that the entry point of one of @p shaders uses
 * (aprendshader.h, "used") is a binding of the layout: its type and count as
 * the shaders declare them (the largest count, where two shaders declare one
 * binding with different counts), and the stages of every shader that uses
 * it. A binding declared and not used is left out, as the pipeline doesn't
 * need it.
 *
 * The layout keeps, for each binding, the shader's name for it (the
 * variable's, or a block's instance name, or its block name if it has none)
 * and, for a uniform or storage buffer, its block's size and members. That
 * is what aprend_binding_layout_find(), aprend_binding_layout_get_block(),
 * aprend_binding_layout_get_uniform_layout() and a material
 * (aprendmaterial.h) read. Where two shaders declare one binding the names
 * and members of the first that uses it are kept, and the block of the one
 * that declares it largest. Two declarations of one block may differ in
 * length, each shader declaring the members up to the last it reads, but
 * the shorter's members must be the first of the longer's: the same names,
 * types, offsets, array counts and strides.
 *
 * @param[in] instance     Instance whose device the layout is created on.
 * @param[in] shaders      The shaders whose declarations make the layout:
 *                         every shader that reads set @p set in any
 *                         pipeline that is to share its sets, so one layout
 *                         serves all of those pipelines (see "Layouts of the
 *                         same bindings" at aprend_binding_layout). Not
 *                         NULL, and none of them NULL. The layout doesn't
 *                         keep them.
 * @param[in] shader_count Number of elements in @p shaders. Not 0.
 * @param[in] set          The descriptor set: `set = ...` in GLSL.
 * @param[out] out_layout  Receives the layout; NULL on failure.
 * @param[out] out_error   Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if an argument is NULL or
 *         0, a shader is of another instance, no shader uses a binding of
 *         @p set, two shaders declare one binding with different types, or
 *         one block with members that differ (not one the start of the
 *         other); APRESULT_LIMIT if the set has more than
 *         APREND_MAX_BINDINGS_PER_LAYOUT bindings; APRESULT_OUT_OF_MEMORY;
 *         or the code for SpudGPU's result if the layout can't be created.
 *
 * @see aprend_binding_layout_destroy()
 */
APRESULT aprend_binding_layout_create_from_shaders(
    aprend_instance instance,
    const aprend_shader *shaders,
    uint32_t shader_count,
    uint32_t set,
    aprend_binding_layout *out_layout,
    APREND_ERROR *out_error);

/**
 * @brief Finds one of a layout's bindings by name.
 *
 * @param[in]  layout      Layout to search.
 * @param[in]  name        The binding's name: its `_name`, or for a layout
 *                         made from shaders the shader's name for it or, for
 *                         a block, its block name. Case-sensitive.
 * @param[out] out_binding Receives the binding's number (`binding = ...`).
 *                         May be NULL.
 * @param[out] out_desc    Receives the binding's desc; its `_name` is the
 *                         layout's copy, valid as long as the layout. May be
 *                         NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT, with nothing written, if
 *         @p layout or @p name is NULL; APRESULT_NOT_FOUND, with nothing
 *         written, if no binding has that name.
 */
APRESULT aprend_binding_layout_find(
    aprend_binding_layout layout,
    const char *name,
    uint32_t *out_binding,
    APREND_BINDING_DESC *out_desc);

/**
 * @brief Gets one of a layout's bindings by its place in the layout.
 *
 * @param[in]  layout   Layout to read.
 * @param[in]  index    From 0 to aprend_binding_layout_get_binding_count() - 1.
 * @param[out] out_desc Receives the binding's desc, with its name as
 *                      aprend_binding_layout_find() gives it.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p layout or @p out_desc is NULL or @p index is out of range.
 */
APRESULT aprend_binding_layout_get_binding(
    aprend_binding_layout layout,
    uint32_t index,
    APREND_BINDING_DESC *out_desc);

/**
 * @brief Gets how many bindings a layout has.
 *
 * @param[in] layout Layout to read.
 *
 * @return The count, or 0 if @p layout is NULL.
 */
uint32_t aprend_binding_layout_get_binding_count(aprend_binding_layout layout);

/**
 * @brief Gets the block of a uniform or storage buffer binding of a layout
 * made from shaders: its size and its members as the SPIR-V lays them out.
 *
 * @param[in]  layout           Layout to read.
 * @param[in]  binding          The binding's number.
 * @param[out] out_size         Receives the block's size (for a storage block
 *                              ending in an array without a size, the bytes
 *                              before it). May be NULL.
 * @param[out] out_members      Receives the members, owned by the layout and
 *                              valid as long as it. May be NULL.
 * @param[out] out_member_count Receives how many. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT, with nothing written, if
 *         @p layout is NULL; APRESULT_NOT_FOUND, with nothing written, if it
 *         has no such binding, or the binding has no block it knows: not a
 *         buffer, or a layout written by hand.
 */
APRESULT aprend_binding_layout_get_block(
    aprend_binding_layout layout,
    uint32_t binding,
    uint32_t *out_size,
    const APREND_SHADER_MEMBER **out_members,
    uint32_t *out_member_count);

/**
 * @brief Gets the uniform layout of a uniform buffer binding of a layout
 * made from shaders, for aprend_uniform_layout_get_size() and
 * aprend_buffer_write_uniform() (aprendbuffers.h).
 *
 * Every member with an APREND_UNIFORM_TYPE is a uniform, named by its path
 * (`color`, `lights[2].position`), at its offset, with its array's element
 * count; `size` is the block's. The offsets are the shader's, so nothing
 * placed by hand can be wrong. A member with no type (a double, a
 * non-square matrix) is not listed and can't be written by name.
 *
 * @param[in]  layout  Layout to read.
 * @param[in]  binding The binding's number.
 * @param[out] out     Receives the layout. Its `uniforms` and their names
 *                     are the layout's, valid as long as it.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT, with nothing written, if
 *         @p layout or @p out is NULL; APRESULT_NOT_FOUND, with nothing
 *         written, if it has no such binding, the binding is not a uniform
 *         buffer, or the layout was written by hand.
 */
APRESULT aprend_binding_layout_get_uniform_layout(
    aprend_binding_layout layout,
    uint32_t binding,
    APREND_UNIFORM_LAYOUT *out);

/**
 * @brief Destroys a binding layout. The SpudGPU layout and descriptor pools
 * it shares with layouts of the same bindings go with the last of them and
 * of their sets.
 *
 * @warning Every pipeline declaring @p layout and every aprend_binding_set
 * created from it must be destroyed first.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p layout itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] layout Layout to destroy. NULL is accepted and does nothing.
 */
void aprend_binding_layout_destroy(aprend_binding_layout layout);

/**
 * @brief Upper bound on the vertex bindings of one pipeline, and on the
 * vertex buffer slots of a command list.
 */
#define APREND_MAX_VERTEX_BINDINGS SPUDGPU_MAX_VERTEX_BINDINGS

/**
 * @brief Upper bound on the push constant ranges of one pipeline.
 */
#define APREND_MAX_PUSH_CONSTANT_RANGES SPUDGPU_MAX_PUSH_CONSTANT_RANGES

/**
 * @brief The type of one element of a vertex layout: what a vertex shader
 * input of that element reads.
 *
 * Values are only ever appended.
 */
typedef uint32_t APREND_VERTEX_ELEMENT_TYPE;
enum {
	/** One float: a `float` input. */
	APREND_VERTEX_ELEMENT_TYPE_FLOAT = 0,
	/** Two floats: a `vec2` input. */
	APREND_VERTEX_ELEMENT_TYPE_VEC2  = 1,
	/** Three floats: a `vec3` input. */
	APREND_VERTEX_ELEMENT_TYPE_VEC3  = 2,
	/** Four floats: a `vec4` input, or one column of a `mat4`. */
	APREND_VERTEX_ELEMENT_TYPE_VEC4  = 3,
	/** One signed 32-bit integer: an `int` input. */
	APREND_VERTEX_ELEMENT_TYPE_INT   = 4,
	/** Two signed 32-bit integers: an `ivec2` input. */
	APREND_VERTEX_ELEMENT_TYPE_INT2  = 5,
	/** Three signed 32-bit integers: an `ivec3` input. */
	APREND_VERTEX_ELEMENT_TYPE_INT3  = 6,
	/** Four signed 32-bit integers: an `ivec4` input. */
	APREND_VERTEX_ELEMENT_TYPE_INT4  = 7,
	/** One unsigned 32-bit integer: a `uint` input. */
	APREND_VERTEX_ELEMENT_TYPE_UINT  = 8,
	/** Two unsigned 32-bit integers: a `uvec2` input. */
	APREND_VERTEX_ELEMENT_TYPE_UINT2 = 9,
	/** Three unsigned 32-bit integers: a `uvec3` input. */
	APREND_VERTEX_ELEMENT_TYPE_UINT3 = 10,
	/** Four unsigned 32-bit integers: a `uvec4` input. */
	APREND_VERTEX_ELEMENT_TYPE_UINT4 = 11,
};

/**
 * @brief Gets the size of one element of a type.
 *
 * @param[in] type The type.
 *
 * @return Its size in bytes, or 0 for a value that isn't an
 *         APREND_VERTEX_ELEMENT_TYPE.
 */
uint32_t aprend_vertex_element_type_get_size(APREND_VERTEX_ELEMENT_TYPE type);

/**
 * @brief One element of a vertex layout: one attribute of a vertex.
 */
/* Frozen: an array element of APREND_VERTEX_LAYOUT. It never changes; a new need gets a new type. */
typedef struct APREND_VERTEX_ELEMENT {
	/**
	 * The element's name, for a pipeline that meets the shader's inputs by
	 * name (APREND_GRAPHICS_PIPELINE_DESC::_vertex_inputs_by_name). May be
	 * NULL otherwise. Copied by the pipeline.
	 */
	const char *name;
	/** What the element is; its size follows from it. */
	APREND_VERTEX_ELEMENT_TYPE type;
	/**
	 * Where the element starts in one vertex, in bytes. A multiple of 4,
	 * and the element ends inside the layout's `stride`.
	 */
	uint32_t offset;
} APREND_VERTEX_ELEMENT;

/**
 * @brief How a pipeline reads one vertex of a vertex buffer slot.
 *
 * A vertex buffer is bytes (aprendbuffers.h); this says what they are, for
 * one pipeline. Two pipelines may read one buffer through different
 * layouts, a depth pass reading only the positions of vertices a main pass
 * reads whole, say: the same `stride`, fewer elements.
 */
/* Frozen: held by value in APREND_VERTEX_BINDING_DESC. It never changes; a new need gets a new type. */
typedef struct APREND_VERTEX_LAYOUT {
	/** How many elements. Not 0. */
	uint32_t count;
	/** The elements, #count of them. Only read. */
	const APREND_VERTEX_ELEMENT *elements;
	/**
	 * The distance in bytes from one vertex to the next. Not 0, a multiple
	 * of 4, and no less than where the furthest element ends: bytes past it
	 * are padding the pipeline doesn't read, as a C struct's `sizeof` may
	 * have.
	 */
	uint32_t stride;
} APREND_VERTEX_LAYOUT;

/**
 * @brief Finds an element of a layout by name.
 *
 * @param[in] layout Layout to search.
 * @param[in] name   The element's name, case-sensitive.
 *
 * @return Index of the first element of @p layout named @p name, or
 *         UINT32_MAX if there is none, or if @p layout, its elements or
 *         @p name is NULL.
 */
uint32_t aprend_vertex_layout_get_element_index(
    const APREND_VERTEX_LAYOUT *layout,
    const char *name);

/**
 * @brief The attributes a pipeline reads from one vertex buffer slot, and how
 * often it steps to the next element of that buffer.
 */
/* Frozen: an array element of APREND_GRAPHICS_PIPELINE_DESC. It never changes; a new need gets a new type. */
typedef struct APREND_VERTEX_BINDING_DESC {
	/**
	 * The attributes, with their offsets inside one vertex, and the stride.
	 * Copied by the pipeline, names included:
	 * aprend_graphics_pipeline_get_desc() hands back the pipeline's copy.
	 */
	APREND_VERTEX_LAYOUT _layout;
	/**
	 * false to step once per vertex, true to step once per instance: every
	 * vertex of one instance reads the same element, and the element is
	 * chosen by the instance index (APREND_COMMAND_DRAW_INSTANCED and
	 * APREND_COMMAND_DRAW_INSTANCED_INDEXED, from `_start_instance_location`).
	 */
	bool _per_instance;
} APREND_VERTEX_BINDING_DESC;

/**
 * @brief Configuration descriptor for a graphics pipeline.
 *
 * No field has a default.
 */
typedef struct APREND_GRAPHICS_PIPELINE_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_GRAPHICS_PIPELINE_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** A name for diagnostics. May be NULL. */
	const char *_debug_name;
	/**
	 * The vertex input: `_vertex_bindings[N]` is read from the vertex buffer
	 * at slot N (APREND_COMMAND_SET_VERTEX_BUFFERS). The first
	 * #_vertex_binding_count entries are read; the rest are ignored.
	 *
	 * Shader locations are numbered through the bindings in order: binding
	 * 0's elements are `layout(location = 0)` onwards, binding 1's follow
	 * on from where binding 0's end, and so on. At most
	 * SPUDGPU_MAX_VERTEX_ATTRIBUTES elements in all.
	 */
	APREND_VERTEX_BINDING_DESC _vertex_bindings[APREND_MAX_VERTEX_BINDINGS];
	/**
	 * How many vertex buffer slots the pipeline reads. At most
	 * APREND_MAX_VERTEX_BINDINGS; 0 is a pipeline with no vertex input.
	 */
	uint32_t _vertex_binding_count;
	/** What the vertices are assembled into. */
    SPUDGPU_PRIMITIVE_TOPOLOGY _topology;
	/**
	 * Which faces are discarded before shading: SPUDGPU_CULL_MODE_NONE,
	 * SPUDGPU_CULL_MODE_FRONT or SPUDGPU_CULL_MODE_BACK.
	 */
	SPUDGPU_CULL_MODE _cull_mode;
	/**
	 * Which winding is a front face, judged as the triangle lands in the
	 * target: true for counter-clockwise, false for clockwise. It decides
	 * what #_cull_mode discards and what the fragment shader sees as front
	 * facing. A projection that flips an axis reverses the winding of
	 * everything drawn through it; accounting for that is the caller's.
	 */
	bool _front_face_ccw;
	/** Whether fragments are tested against the depth target. */
	bool _depth_test;
	/** Whether fragments that pass write their depth. */
	bool _depth_write;
	/**
	 * Depth test comparison, required when #_depth_test is set:
	 * SPUDGPU_COMPARE_OP_LESS for standard depth, SPUDGPU_COMPARE_OP_GREATER
	 * for reverse-Z. SPUDGPU_COMPARE_OP_NEVER with #_depth_test set is
	 * refused.
	 */
	SPUDGPU_COMPARE_OP _depth_compare_op;
	/**
	 * Draws triangles as their edges instead of filled. Lines, of a wireframe
	 * or a line topology, are one pixel wide: there is no line width.
	 */
	bool _wireframe;
	/**
	 * Depth bias: `_depth_bias_constant * r + _depth_bias_slope * m`, added
	 * to each fragment's depth before the depth test and write, where m is
	 * the triangle's depth slope and r the depth format's smallest step;
	 * clamped to #_depth_bias_clamp unless that is 0. All three 0 is no
	 * bias. What a shadow map is rendered with, so that a surface doesn't
	 * shadow itself. Bias triangles: what it does to lines and points is
	 * undefined. The full rule, and how a float depth format's step differs
	 * between graphics APIs, is spudgpu_shader_pipeline_desc's.
	 */
	int32_t _depth_bias_constant;
	/** The slope's factor in the bias. See #_depth_bias_constant. */
	float _depth_bias_slope;
	/**
	 * The largest bias, if positive, or the smallest, if negative; 0 for no
	 * clamp. Anything else needs a device that can clamp
	 * (`SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP` on the instance's device).
	 */
	float _depth_bias_clamp;
	/**
	 * How each color target's output combines with what it already holds:
	 * `_blends[i]` for `color_attachment_formats[i]`, the first
	 * #color_attachment_count read. Zeroed (`blend_enable` false) writes the
	 * output as is. The presets fill one for the usual cases. Entries that
	 * differ need a device that blends targets independently
	 * (`SPUDGPU_FEATURE_INDEPENDENT_BLEND` on the instance's device). A blended pipeline
	 * normally clears #_depth_write and keeps #_depth_test, and its draws
	 * are the caller's to order: back to front, or after everything opaque.
	 * @see aprend_blend_premultiplied()
	 * @see aprend_blend_alpha()
	 * @see aprend_blend_additive()
	 */
	spudgpu_blend_attachment_desc _blends[SPUDGPU_MAX_COLOR_ATTACHMENTS];
	/**
	 * The vertex stage. Exactly one of this and #mesh_shader is given: NULL
	 * for a mesh shader pipeline.
	 */
	aprend_shader vertex_shader;
	/** The fragment stage. Required. */
	aprend_shader fragment_shader;
	/**
	 * Name of the function #vertex_shader starts at, `main` for GLSL.
	 * Required, and not an empty string. Copied: the pointer only has to
	 * live for aprend_graphics_pipeline_create().
	 */
	const char *_vertex_entry_point;
	/** The same for #fragment_shader. */
	const char *_fragment_entry_point;
	/**
	 * A mesh shader in place of #vertex_shader: a shader created for
	 * SPUDGPU_SHADER_STAGE_MESH, which emits its own geometry. Such a
	 * pipeline has no vertex input (#_vertex_binding_count is 0, and
	 * #_vertex_entry_point is not read), is run with
	 * APREND_COMMAND_DISPATCH_MESH and not with the draw commands, and reads
	 * what it needs through its binding sets. It can only be created on a
	 * device created with SPUDGPU_FEATURE_MESH_SHADING. There is no task
	 * shader.
	 */
	aprend_shader mesh_shader;
	/** The entry point of #mesh_shader, as #_vertex_entry_point. */
	const char *_mesh_entry_point;

	/**
	 * Pixel format of each color target the pipeline writes, in the order
	 * of the fragment shader's outputs (`location = i` writes target i) and
	 * of a pass's color targets. The first #color_attachment_count are read,
	 * and none is SPUDGPU_FORMAT_UNKNOWN.
	 */
	SPUDGPU_FORMAT color_attachment_formats[SPUDGPU_MAX_COLOR_ATTACHMENTS];
	/**
	 * How many color targets: 0 for a pipeline that renders depth only, at
	 * most APREND_MAX_COLOR_TARGETS (aprendcommands.h), which is
	 * SPUDGPU_MAX_COLOR_ATTACHMENTS.
	 */
	uint32_t color_attachment_count;
	/**
	 * Pixel format of the depth attachment. SPUDGPU_FORMAT_UNKNOWN for no
	 * depth.
	 */
	SPUDGPU_FORMAT depth_format;

	/**
	 * The descriptor sets the shaders use: `_binding_layouts[N]` describes
	 * `set = N`. The first #_binding_layout_count entries must all be set;
	 * the rest are ignored. Draws with this pipeline need an
	 * aprend_binding_set of the same layout at each of those slots first
	 * (APREND_COMMAND_SET_BINDING_SET). The layouts must outlive the
	 * pipeline.
	 */
	aprend_binding_layout _binding_layouts[APREND_MAX_BINDING_LAYOUTS];
	/** How many set slots are declared. At most APREND_MAX_BINDING_LAYOUTS. */
	uint32_t _binding_layout_count;

	/**
	 * The byte ranges of the shaders' push constant block and the stages
	 * that read each (`layout(push_constant)` in GLSL). The first
	 * #_push_constant_range_count entries are read. Each needs at least one
	 * stage, a size that is not 0, and an offset and size that are multiples
	 * of 4. The bytes are written with APREND_COMMAND_PUSH_CONSTANTS.
	 */
	spudgpu_push_constant_range_desc _push_constant_ranges[APREND_MAX_PUSH_CONSTANT_RANGES];
	/**
	 * How many ranges are declared. At most APREND_MAX_PUSH_CONSTANT_RANGES;
	 * 0 is a pipeline with no push constants.
	 */
	uint32_t _push_constant_range_count;

	/**
	 * How the elements of #_vertex_bindings meet the vertex shader's inputs.
	 *
	 * false: by position, as #_vertex_bindings describes: element N across
	 * the bindings is `location = N`.
	 *
	 * true: by name. Each element whose `name` is the name of one of the
	 * vertex shader's inputs is given that input's location; an element whose
	 * name matches none is not read, so one vertex layout can serve shaders
	 * that read different parts of it. Every input the shader uses must be
	 * matched; a matrix input is matched by one element per column, each
	 * named as the input. Every element then has a name.
	 */
	bool _vertex_inputs_by_name;
} APREND_GRAPHICS_PIPELINE_DESC;

/**
 * @brief Opaque handle to a graphics pipeline.
 */
typedef struct aprend_graphics_pipeline_t *aprend_graphics_pipeline;

/**
 * @name Blend presets
 * For an entry of APREND_GRAPHICS_PIPELINE_DESC::_blends.
 *
 * Premultiplied is the convention to prefer: the fragment shader outputs
 * color already multiplied by its alpha, which composites correctly over
 * anything and filters without dark fringes. Use aprend_blend_alpha() for
 * shaders that output straight (unmultiplied) color; it leaves the target's
 * alpha as coverage, so the target can itself be composited afterwards.
 * @{
 */

/**
 * @brief Premultiplied alpha blending.
 * @return `out = src + dst * (1 - src.a)`, color and alpha alike.
 */
spudgpu_blend_attachment_desc aprend_blend_premultiplied(void);

/**
 * @brief Straight (unmultiplied) alpha blending.
 * @return `out.rgb = src.rgb * src.a + dst.rgb * (1 - src.a)`;
 *         `out.a = src.a + dst.a * (1 - src.a)`.
 */
spudgpu_blend_attachment_desc aprend_blend_alpha(void);

/**
 * @brief Additive blending: light adding to light.
 * @return `out = src + dst`, color and alpha alike.
 */
spudgpu_blend_attachment_desc aprend_blend_additive(void);

/** @} */

/**
 * @brief Creates a graphics pipeline.
 *
 * @param[in] instance Instance whose device the pipeline is created on.
 * @param[in]  desc         Pipeline configuration. Only read during the
 *                          call.
 * @param[out] out_pipeline Receives the pipeline; NULL on failure.
 * @param[out] out_error    Why it failed, when it did, naming the field or
 *                          the shader's declaration. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_UNSUPPORTED where a feature the desc needs
 *         is missing (named below); APRESULT_OUT_OF_MEMORY; the struct codes
 *         for `desc->base`; the code for SpudGPU's result if the pipeline
 *         can't be created; otherwise APRESULT_INVALID_ARGUMENT, if
 *         @p instance, @p desc or @p out_pipeline is NULL, `fragment_shader` is
 *         NULL, neither or both of `vertex_shader` and `mesh_shader` are
 *         given, a shader was created for another stage than the one it is
 *         given as, a mesh shader comes with vertex bindings or on a device
 *         without SPUDGPU_FEATURE_MESH_SHADING,
 *         either entry point is NULL or an empty string,
 *         `_vertex_binding_count` is above APREND_MAX_VERTEX_BINDINGS, a
 *         counted vertex binding has no elements, a `stride` of 0 or not a
 *         multiple of 4, or an element whose type is not an
 *         APREND_VERTEX_ELEMENT_TYPE, whose `offset` is not a multiple of
 *         4, or that ends past the `stride`, the
 *         bindings hold more than SPUDGPU_MAX_VERTEX_ATTRIBUTES elements in
 *         all, `_push_constant_range_count` is above
 *         APREND_MAX_PUSH_CONSTANT_RANGES, a counted push constant range has
 *         no stages, a size of 0, or an offset or size that is not a
 *         multiple of 4, `_cull_mode` is not a SPUDGPU_CULL_MODE, `_depth_test` is
 *         set with `_depth_compare_op` SPUDGPU_COMPARE_OP_NEVER,
 *         `color_attachment_count` is above APREND_MAX_COLOR_TARGETS or a
 *         counted format is SPUDGPU_FORMAT_UNKNOWN, `_depth_bias_clamp` is
 *         not 0 on a device without SPUDGPU_FEATURE_DEPTH_BIAS_CLAMP,
 *         `_blends` differ on one without
 *         SPUDGPU_FEATURE_INDEPENDENT_BLEND,
 *         `_binding_layout_count` is above APREND_MAX_BINDING_LAYOUTS, a
 *         counted entry of `_binding_layouts` is NULL or belongs to another
 *         instance, a shader is of another instance, the shaders don't match
 *         the desc (below).
 *
 * Checked against each shader's reflection (aprendshader.h), each failure
 * named in @p out_error:
 *
 * - The entry point named is the SPIR-V's entry point for the stage.
 * - Every binding a shader uses is declared by the binding layout at its
 *   set, with the same type, at least as many descriptors, and the shader's
 *   stage among its `_stages`. A layout may declare more than the shaders
 *   use.
 * - Every byte of a shader's push constant block that a member covers is in
 *   a range whose `stage_flags` include the shader's stage.
 * - Every input the vertex shader uses has a vertex element at each of its
 *   locations, of the same kind of number: a float input from a VEC or
 *   FLOAT element, an int input from an INT element. A `uint` input has no
 *   element type that feeds it and is refused.
 *
 * @see aprend_graphics_pipeline_destroy()
 */
APRESULT aprend_graphics_pipeline_create(
    aprend_instance instance,
    const APREND_GRAPHICS_PIPELINE_DESC *desc,
    aprend_graphics_pipeline *out_pipeline,
    APREND_ERROR *out_error);

/**
 * @brief Destroys a graphics pipeline.
 *
 * Its shaders and binding layouts are the caller's and are not destroyed.
 *
 * @warning No later submission may draw with @p p. A
 * submission already made that uses it is safe: the GPU's side of it is
 * released when that work has finished (aprendcontext.h, "Destroying").
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p p itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] p Pipeline to destroy. NULL is accepted and does nothing.
 */
void aprend_graphics_pipeline_destroy(aprend_graphics_pipeline p);

/**
 * @brief Reads the descriptor a pipeline was created with: as it was passed
 * to aprend_graphics_pipeline_create(), pointers included, except that the
 * vertex layouts' elements and their names, and the entry point names, are
 * the pipeline's own copies, valid until it is destroyed, and the one of
 * `_vertex_entry_point` and `_mesh_entry_point` the pipeline doesn't use is
 * NULL.
 *
 * The whole struct is written, `base` included.
 *
 * @param[in]  p        Pipeline to read.
 * @param[out] out_desc Receives the descriptor.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p p or @p out_desc is NULL.
 */
APRESULT aprend_graphics_pipeline_get_desc(
    aprend_graphics_pipeline p,
    APREND_GRAPHICS_PIPELINE_DESC *out_desc);

/**
 * @brief Configuration descriptor for a compute pipeline.
 *
 * No field has a default.
 */
typedef struct APREND_COMPUTE_PIPELINE_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_COMPUTE_PIPELINE_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** A name for diagnostics. May be NULL. */
	const char *_debug_name;
	/**
	 * The compute stage: a shader created for
	 * SPUDGPU_SHADER_STAGE_COMPUTE. Required.
	 */
	aprend_shader compute_shader;
	/**
	 * Name of the function #compute_shader starts at, `main` for GLSL.
	 * Required, and not an empty string. Copied: the pointer only has to
	 * live for aprend_compute_pipeline_create().
	 */
	const char *_entry_point;
	/**
	 * The descriptor sets the shader uses: `_binding_layouts[N]` describes
	 * `set = N`. The first #_binding_layout_count entries must all be set;
	 * the rest are ignored. They are the same aprend_binding_layout objects
	 * a graphics pipeline declares, so one layout and its sets serve both.
	 * A dispatch needs an aprend_binding_set of the same layout at each of
	 * those slots first (APREND_COMMAND_SET_BINDING_SET). The layouts must
	 * outlive the pipeline.
	 */
	aprend_binding_layout _binding_layouts[APREND_MAX_BINDING_LAYOUTS];
	/** How many set slots are declared. At most APREND_MAX_BINDING_LAYOUTS. */
	uint32_t _binding_layout_count;
	/**
	 * The byte ranges of the shader's push constant block. The first
	 * #_push_constant_range_count entries are read. Each has
	 * SPUDGPU_SHADER_STAGE_COMPUTE as its only stage, a size that is not 0,
	 * and an offset and size that are multiples of 4. The bytes are written
	 * with APREND_COMMAND_PUSH_CONSTANTS, sent outside a pass.
	 */
	spudgpu_push_constant_range_desc _push_constant_ranges[APREND_MAX_PUSH_CONSTANT_RANGES];
	/**
	 * How many ranges are declared. At most APREND_MAX_PUSH_CONSTANT_RANGES;
	 * 0 is a pipeline with no push constants.
	 */
	uint32_t _push_constant_range_count;
} APREND_COMPUTE_PIPELINE_DESC;

/**
 * @brief Opaque handle to a compute pipeline.
 *
 * Set in a command list with APREND_COMMAND_SET_COMPUTE_PIPELINE and run
 * with APREND_COMMAND_DISPATCH, both outside any pass.
 */
typedef struct aprend_compute_pipeline_t *aprend_compute_pipeline;

/**
 * @brief Creates a compute pipeline.
 *
 * @param[in] instance Instance whose device the pipeline is created on.
 * @param[in]  desc         Pipeline configuration. Only read during the
 *                          call.
 * @param[out] out_pipeline Receives the pipeline; NULL on failure.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_OUT_OF_MEMORY; the struct codes for
 *         `desc->base`; the code for SpudGPU's result if the pipeline can't
 *         be created; otherwise APRESULT_INVALID_ARGUMENT, if @p instance,
 *         @p desc or @p out_pipeline is NULL, `compute_shader` is
 *         NULL, belongs to another instance or was not created for the
 *         compute stage, `_entry_point` is NULL or an empty string,
 *         `_binding_layout_count` is above APREND_MAX_BINDING_LAYOUTS, a
 *         counted entry of `_binding_layouts` is NULL or belongs to another
 *         instance, `_push_constant_range_count` is above
 *         APREND_MAX_PUSH_CONSTANT_RANGES, a counted push constant range has
 *         a stage other than compute alone, a size of 0, or an offset or size
 *         that is not a multiple of 4, the shader doesn't match the desc as
 *         aprend_graphics_pipeline_create() checks (entry point, bindings,
 *         push constants).
 *
 * @see aprend_compute_pipeline_destroy()
 */
APRESULT aprend_compute_pipeline_create(
    aprend_instance instance,
    const APREND_COMPUTE_PIPELINE_DESC *desc,
    aprend_compute_pipeline *out_pipeline,
    APREND_ERROR *out_error);

/**
 * @brief Destroys a compute pipeline.
 *
 * Its shader and binding layouts are the caller's and are not destroyed.
 *
 * @warning No later submission may dispatch with @p p. A
 * submission already made that uses it is safe: the GPU's side of it is
 * released when that work has finished (aprendcontext.h, "Destroying").
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p p itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] p Pipeline to destroy. NULL is accepted and does nothing.
 */
void aprend_compute_pipeline_destroy(aprend_compute_pipeline p);

/**
 * @brief Reads the descriptor a compute pipeline was created with: as it
 * was passed to aprend_compute_pipeline_create(), pointers included, except
 * that the entry point name is the pipeline's own copy and is valid until
 * it is destroyed.
 *
 * The whole struct is written, `base` included.
 *
 * @param[in]  p        Pipeline to read.
 * @param[out] out_desc Receives the descriptor.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p p or @p out_desc is NULL.
 */
APRESULT aprend_compute_pipeline_get_desc(
    aprend_compute_pipeline p,
    APREND_COMPUTE_PIPELINE_DESC *out_desc);

/**
 * @brief Opaque handle to the resources filling one aprend_binding_layout's
 * slots.
 *
 * Put at a set slot in a command list with APREND_COMMAND_SET_BINDING_SET.
 * Which resource sits in which slot is fixed at creation; the contents of the
 * buffers and textures can still change. Usable with any pipeline that
 * declares the layout it was created from, or a layout of the same bindings
 * ("Layouts of the same bindings" at aprend_binding_layout), at any set slot
 * where the pipeline declares it.
 */
typedef struct aprend_binding_set_t *aprend_binding_set;

/**
 * @brief One resource for one slot of an aprend_binding_set.
 *
 * `_type` says which member of `_resource` is read:
 *
 * | `_type`                                        | Read                               |
 * |------------------------------------------------|------------------------------------|
 * | SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER         | `_uniform`                         |
 * | SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER         | `_storage`                         |
 * | SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE          | `_image._view`                     |
 * | SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE          | `_image._view`                     |
 * | SPUDGPU_DESCRIPTOR_TYPE_SAMPLER                | `_image._sampler`                  |
 * | SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER | `_image._view`, `_image._sampler`  |
 *
 * A member of `_image` the type doesn't read is ignored.
 */
/* Frozen: an array element of a binding set. It never changes; a new need gets a new type. */
typedef struct APREND_BINDING_SET_ENTRY {
	/** The slot, as APREND_BINDING_DESC::_binding. */
	uint32_t _binding;
	/** Index into the binding's array; 0 where its `_count` is 1. */
	uint32_t _array_element;
	/** Must be the `_type` the layout declares at #_binding. */
	SPUDGPU_DESCRIPTOR_TYPE _type;
	union {
		struct {
			/**
			 * The buffer, of #APREND_BUFFER_USAGE_UNIFORM (aprendbuffers.h).
			 * One of per-frame copies is read in the copy of the frame a
			 * command list is compiled in.
			 */
			aprend_buffer _buffer;
			/**
			 * First byte of the buffer the slot sees. Inside the buffer, and
			 * a multiple of the device's
			 * SPUDGPU_ADAPTER_PROPERTIES::uniform_buffer_offset_alignment
			 * (spudgpu.h); 0 always is.
			 */
			uint64_t _offset;
			/**
			 * How many bytes the slot sees, from `_offset`. 0 is to the end
			 * of the buffer.
			 */
			uint64_t _range;
		} _uniform; /**< For SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER. */
		struct {
			/**
			 * The buffer, of #APREND_BUFFER_USAGE_STORAGE, its copies read
			 * as `_uniform._buffer`'s are.
			 */
			aprend_buffer _buffer;
			/**
			 * First byte of the buffer the slot sees. Inside the buffer, and
			 * a multiple of the device's
			 * SPUDGPU_ADAPTER_PROPERTIES::storage_buffer_offset_alignment;
			 * 0 always is.
			 */
			uint64_t _offset;
			/**
			 * How many bytes the slot sees, from `_offset`. 0 is to the end
			 * of the buffer.
			 */
			uint64_t _range;
		} _storage; /**< For SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER. */
		struct {
			/**
			 * The texture view. Of type
			 * APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE for a sampled image or
			 * combined image sampler slot,
			 * APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS for a storage image
			 * slot.
			 */
			aprend_texture_view _view;
			/** The sampler. */
			aprend_sampler _sampler;
		} _image; /**< For the image and sampler types. */
	} _resource; /**< The resource. Only the member #_type names is read. */
} APREND_BINDING_SET_ENTRY;

/**
 * @brief Creates a binding set from a layout.
 *
 * @p entries is only read during the call. The resources it names must
 * outlive the set, and the set must outlive every submission that binds it.
 *
 * A layout's sets share descriptor pools the layout owns, and a destroyed
 * set's place is reused by the next one created.
 *
 * A set with a buffer of per-frame copies in any entry keeps one descriptor
 * set for each frame in flight, and a command list binds the one of the
 * frame it is compiled in: such a list is submitted in that frame, and compiled again in
 * the next (aprend_command_list_submit() refuses it otherwise). A set of
 * single buffers, textures and samplers alone has no such limit.
 *
 * @note Two of these calls may not overlap, for sets of one layout or of
 * different ones: see "Threads" at the top of this header.
 *
 * @param[in] layout      Layout whose slots the set fills.
 * @param[in] entries     One entry for every array element of every binding
 *                        of @p layout, each exactly once, and nothing else.
 * @param[in] entry_count Number of elements in @p entries.
 * @param[out] out_set    Receives the set; NULL on failure.
 * @param[out] out_error  Why it failed, when it did, naming the entry. May
 *                        be NULL.
 *
 * @return APRESULT_OK; APRESULT_OUT_OF_MEMORY; the code for SpudGPU's result
 *         if the set can't be allocated; otherwise
 *         APRESULT_INVALID_ARGUMENT, if @p layout, @p entries or @p out_set
 *         is NULL, the entries
 *         don't fill the layout exactly, an entry's `_type` is not the
 *         layout's, a handle its `_type` reads is NULL, a buffer is of
 *         another instance or lacks the usage its slot needs
 *         (#APREND_BUFFER_USAGE_UNIFORM or _STORAGE), a view
 *         is of the wrong type for its slot, a buffer's `_offset` is not a
 *         multiple of the device's offset alignment for its type or its
 *         `_offset` and `_range` run past its end.
 *
 * @see aprend_binding_set_destroy()
 */
APRESULT aprend_binding_set_create(
    aprend_binding_layout layout,
    const APREND_BINDING_SET_ENTRY *entries,
    uint32_t entry_count,
    aprend_binding_set *out_set,
    APREND_ERROR *out_error);

/**
 * @brief Points some of a binding set's slots at other resources.
 *
 * Every array element @p entries names is given the resource the entry
 * holds; every other element keeps what it has. The handle stays the same,
 * so command lists that record it need no change.
 *
 * A submission already made that binds @p set is safe, and still reads what
 * the set held: the set is given new descriptor sets, and its old ones go
 * back to the layout when that work has finished (aprendcontext.h,
 * "Destroying"). So an update costs what creating a set does, and is for a
 * slot that changes now and then, not for data that changes every frame,
 * which is the job of a buffer of per-frame copies (aprendbuffers.h).
 *
 * @warning A command list compiled before the update and not yet submitted
 * would bind the old descriptor sets, which may be gone by the time it
 * runs: aprend_command_list_submit() refuses it. Compile it again.
 *
 * Every resource the set holds must still be alive, the ones not named here
 * as well: they are all written again.
 *
 * @note Two of these calls may not overlap, for sets of one layout or of
 * different ones: see "Threads" at the top of this header.
 *
 * @param[in] set         Set to change.
 * @param[in] entries     The elements to change, each at most once. Only
 *                        read during the call.
 * @param[in] entry_count Number of elements in @p entries. From 1 to the
 *                        number of array elements the layout declares.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; on failure, leaving @p set as it was,
 *         APRESULT_INVALID_ARGUMENT if @p set or @p entries is NULL,
 *         @p entry_count is 0 or more than the set has elements, an entry
 *         names an element the layout doesn't declare or one another entry
 *         names, or fails any check aprend_binding_set_create() makes of an
 *         entry; APRESULT_OUT_OF_MEMORY; or the code for SpudGPU's result if
 *         the new descriptor sets can't be allocated.
 */
APRESULT aprend_binding_set_update(
    aprend_binding_set set,
    const APREND_BINDING_SET_ENTRY *entries,
    uint32_t entry_count,
    APREND_ERROR *out_error);

/**
 * @brief Destroys a binding set.
 *
 * The resources it holds are the caller's and are not destroyed.
 *
 * @warning No later submission may bind @p set. A
 * submission already made that uses it is safe: the GPU's side of it is
 * released when that work has finished (aprendcontext.h, "Destroying").
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p set itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] set Set to destroy. NULL is accepted and does nothing.
 */
void aprend_binding_set_destroy(aprend_binding_set set);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_PIPELINE_H
