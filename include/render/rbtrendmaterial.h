
#ifndef RBTREND_MATERIAL_H
#define RBTREND_MATERIAL_H

/**
 * @file rbtrendmaterial.h
 * @brief Materials: the resources one rbtrend_binding_layout's slots hold,
 * set by name, and the bytes of the uniform blocks among them.
 *
 * A material fills one set slot of a pipeline. It is a binding layout and
 * what each of the layout's bindings holds, each binding found by its name
 * in the layout (rbtrend_binding_layout_find(), rbtrendpipeline.h):
 *
 * - A uniform block the material owns: every uniform buffer binding whose
 *   block the layout knows (a layout made from shaders,
 *   rbtrend_binding_layout_create_from_shaders()) and that is one buffer, not
 *   an array, unless the desc names it external. Its members are written
 *   by name with rbtrend_material_set_uniform(), at the offsets, strides and
 *   matrix layout the SPIR-V gives them, or as bytes with
 *   rbtrend_material_set_uniform_bytes(). The material keeps a copy of the
 *   block's bytes, and the block itself at a fixed range of the instance's
 *   material arena: uniform buffers in the device's own memory, shared by
 *   every material of the instance, each range 256-byte aligned. A value
 *   set once is kept until it is set again; there is one copy of the block
 *   on the GPU, not one for each frame, and one descriptor set for the
 *   material.
 * - Anything else a binding set holds, given by the caller: a uniform
 *   buffer it keeps itself (an external block), a storage buffer, a
 *   texture view, a sampler.
 *
 * It is bound with RBTREND_COMMAND_SET_MATERIAL (rbtrendcommands.h), in place
 * of an rbtrend_binding_set, at the slot where the pipeline declares the
 * material's layout or a layout of the same bindings (rbtrendpipeline.h,
 * "Layouts of the same bindings"), so one material serves every pipeline
 * that reads its set: a depth pre-pass and the main pass, say. What a
 * pipeline draws with is chosen by the material it is given, so one
 * pipeline and many materials of one layout are the usual shape: a material
 * does not name a pipeline or a shader.
 *
 * The names are the layout's: the shader's names for a layout made from
 * shaders, the `_name`s of a layout written by hand. A uniform is named by
 * its path in its block (`roughness`, `lights[2].color`), or by that path
 * after its binding's name (`u_Material.roughness`), which is needed where
 * two of the layout's blocks have a member of one name. A name that matches
 * nothing is a failed call, not a silent one.
 *
 * @par Finding a uniform once
 * rbtrend_material_set_uniform() searches the layout's members by name at
 * every call. A value set every frame, or in many materials, is found once
 * instead: rbtrend_material_find_uniform() turns a name into an
 * RBTREND_MATERIAL_UNIFORM, a small value that names the member within the
 * layout, and rbtrend_material_set_uniform_value() writes it with no search.
 * One is found from the layout, not from a material, so it serves every
 * material of that layout: found when the layout is made, used for each
 * material after. rbtrend_material_set_uniform() is a find and a set.
 *
 * @par When resources take effect
 * The material builds its rbtrend_binding_set (rbtrendpipeline.h) when a command
 * list that binds it is compiled, from what it holds then, and builds it
 * again at the next compile after any of its resources is set to something
 * else. Building it again is rbtrend_binding_set_update(): it costs what
 * creating a set does. A command list compiled before the change and
 * submitted before the set is built again reads what the material held at
 * its compile; once another compile has built it again, the earlier list
 * is refused at submit (its descriptor sets are on their way out) and is
 * compiled again. So a resource is set before the compile of every list
 * that is to see it.
 * Every binding must hold something by then: rbtrend_material_is_complete().
 *
 * @par Copying, and what an unset binding holds
 * rbtrend_material_copy() makes a material that starts as another is: its
 * values, and the same resources. Rend puts nothing in a binding the
 * caller leaves empty, and a material with one can't be drawn: what an
 * empty slot should read (white, black, a flat normal, a texture of the
 * right shape and type) depends on what the slot is for, which the caller
 * knows and Rend doesn't. A caller that wants every material of a layout
 * to start from defaults of its choosing makes one material with them and
 * copies it. (rbtrend_lit_material_create(), rbtrendlit.h, is such a start for
 * Rend's own lit layout, whose slots Rend does know.)
 *
 * @par When uniform values take effect
 * A value set is seen by every submission made after it of a list that
 * binds the material, and by none made before. At the submission of such a
 * list, the blocks the material owns that have changed since they were last
 * copied are written to a span of the current frame's transient memory
 * (rbtrendtransient.h), and copied from there into their ranges by the GPU,
 * in the same submission and ahead of the list's own commands. The copy
 * waits for the earlier submissions that read the range, so work already on
 * the GPU keeps the values it was submitted with, and a value may be set
 * between any two submissions, of one frame or of two. A material whose
 * values don't change costs nothing per frame.
 *
 * So an instance with materials that own a block has transient memory
 * (RBTREND_INSTANCE_DESC::transient_chunk_size): a copy takes the block's
 * size of it, rounded up to 16, in the frame it is made.
 *
 * @par Threads
 * The rule is in rbtrendcontext.h ("Threads"). A material is used by one
 * caller at a time with everything else of its instance: its set calls,
 * and the compile and submission of every list that binds it, which build
 * its binding set and copy its blocks. rbtrend_material_destroy() may
 * overlap other calls on the instance, as most destroy calls may.
 */

#include "rbtrendpipeline.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @brief Opaque handle to a material.
 */
typedef struct rbtrend_material_t *rbtrend_material;

/**
 * @brief Configuration descriptor for a material.
 *
 * No field has a default.
 */
typedef struct RBTREND_MATERIAL_DESC {
	/** This struct's type, RBT_STRUCT_TYPE_REND_MATERIAL_DESC, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/**
	 * The layout whose slots the material fills: the one the pipelines it is
	 * drawn with declare at the slot it is bound to, usually made from their
	 * shaders. Not NULL, and of the instance the material is created on.
	 * Every binding has a name. It must outlive the material.
	 */
	rbtrend_binding_layout _layout;
	/**
	 * The names of uniform buffer bindings the material doesn't own though it
	 * could: blocks the caller fills with a buffer of its own, with
	 * rbtrend_material_set_uniform_buffer() - one shared by many materials,
	 * say. Each names a uniform buffer binding of #_layout. May be NULL with
	 * a count of 0. Only read during rbtrend_material_create().
	 */
	const char *const *_external_blocks;
	/** Number of elements in #_external_blocks. */
	uint32_t _external_block_count;
} RBTREND_MATERIAL_DESC;

/**
 * @brief Creates a material. Its owned blocks start as zeroes; every other
 * binding starts empty.
 *
 * @param[in]  instance     Instance the material's buffers are created on.
 * @param[in]  desc         Material configuration. Only read during the
 *                          call.
 * @param[out] out_material Receives the material; NULL on failure.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p instance, @p desc or
 *         @p out_material is NULL, `_layout` is NULL or of another
 *         instance, a binding of the layout has no name, two have the same
 *         name, or `_external_blocks` is NULL with a count above 0 or names
 *         something that is not a uniform buffer binding of the layout;
 *         RBT_RESULT_UNSUPPORTED if the material owns a block and the instance
 *         has no transient memory; RBT_RESULT_LIMIT for a block past 32 bits;
 *         the struct codes for `desc->base`; RBT_RESULT_OUT_OF_MEMORY; or
 *         RBT_RESULT_REND_GPU_FAILURE if the material arena needs another
 *         buffer and the device refuses it.
 *
 * @see rbtrend_material_destroy()
 */
RBT_RESULT rbtrend_material_create(
    rbtrend_instance instance,
    const RBTREND_MATERIAL_DESC *desc,
    rbtrend_material *out_material,
    RBTREND_ERROR *out_error);

/**
 * @brief Destroys a material and its binding set, and gives its blocks'
 * ranges back to the instance's material arena.
 *
 * What it was given by the caller is the caller's and is not destroyed. The
 * GPU doesn't have to be finished with it (rbtrendcontext.h, "Destroying"):
 * the ranges are handed out again only once everything submitted up to now
 * has finished.
 *
 * A copy made from it (rbtrend_material_copy()) is not affected. A command
 * list compiled with it and not yet submitted is refused at submit
 * (rbtrend_command_list_submit(), rbtrendcommands.h).
 *
 * @warning No command list that binds @p material may be compiled after
 * this.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p material itself meanwhile (rbtrendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] material Material to destroy. NULL is accepted and does nothing.
 */
void rbtrend_material_destroy(rbtrend_material material);

/**
 * @brief Creates a material that starts as another is.
 *
 * The new material fills the same layout, owns the same blocks (those the
 * source was not told to leave external), and holds the source's values in
 * them in blocks of its own. Every resource the source holds - a view, a
 * sampler, a uniform or storage buffer - it holds too: the
 * same handles, shared, not duplicated, so each must outlive both
 * materials or be replaced in each before its next compile. A binding the
 * source leaves empty is empty in the copy. After the call the two are
 * independent: setting a value or a resource of one does not change the
 * other, and either may be destroyed first.
 *
 * Costs what rbtrend_material_create() does - a range of the material arena
 * for each owned block - and, at the first submission that binds the copy,
 * the copy of its blocks to the GPU, as for a new material. Its binding set
 * is built at the first compile that binds it. A copy of a lit material
 * (rbtrend_lit_material_create(), rbtrendlit.h) is a lit material.
 *
 * @param[in]  source       Material to copy. Only read during the call.
 * @param[out] out_material Receives the copy; NULL on failure.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p source or
 *         @p out_material is NULL; RBT_RESULT_OUT_OF_MEMORY; or
 *         RBT_RESULT_REND_GPU_FAILURE if the material arena needs another
 *         buffer and the device refuses it.
 *
 * @see rbtrend_material_destroy()
 */
RBT_RESULT rbtrend_material_copy(
    rbtrend_material source,
    rbtrend_material *out_material,
    RBTREND_ERROR *out_error);

/**
 * @brief Gets the layout a material fills.
 *
 * @param[in] material Material to read.
 *
 * @return The layout, or NULL if @p material is NULL.
 */
rbtrend_binding_layout rbtrend_material_get_layout(rbtrend_material material);

/**
 * @brief Whether every binding of a material holds what its type reads: a
 * command list that binds it can't be compiled until it does.
 *
 * Owned blocks always do. A combined image sampler binding needs a texture
 * and a sampler. A compile that finds one missing fails, its message naming
 * the binding and element.
 *
 * @param[in] material Material to read.
 *
 * @return true if it is complete; false if not, or if @p material is NULL.
 */
bool rbtrend_material_is_complete(rbtrend_material material);

/**
 * @brief Sets one uniform of a material's owned blocks, by name.
 *
 * rbtrend_material_find_uniform() on the material's layout, then
 * rbtrend_material_set_uniform_value(): see both for what is written and
 * when it is refused. For a value set every frame or in many materials,
 * find it once and call rbtrend_material_set_uniform_value() (see "Finding
 * a uniform once" at the top of this header).
 *
 * @param[in] material Material to write.
 * @param[in] name     The member's path in its block, or its binding's name,
 *                     a dot and that path (see the top of this header).
 *                     Case-sensitive.
 * @param[in] data     The whole member, tightly packed, as
 *                     rbtrend_material_set_uniform_value() takes it.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; otherwise RBT_RESULT_INVALID_ARGUMENT or the failure's
 *         own code (rbtrendcontext.h, "Results and errors"), with nothing
 *         written, if an argument is NULL, or the failure's code of either
 *         call it makes.
 */
RBT_RESULT rbtrend_material_set_uniform(
    rbtrend_material material,
    const char *name,
    const void *data,
    RBTREND_ERROR *out_error);

/**
 * @brief One member of a uniform block of a binding layout, as
 * rbtrend_material_find_uniform() found it: what
 * rbtrend_material_set_uniform_value() and
 * rbtrend_material_get_uniform_value() write and read with no search.
 *
 * A value, copied and kept as the caller likes, and valid as long as its
 * layout. Its fields are Rend's: they are filled by
 * rbtrend_material_find_uniform() and read only by the calls that take one,
 * never by the caller. All zeroes is no uniform, and every call that takes
 * it refuses it.
 */
/* Frozen: a value, Rend's to fill in. It never changes; a new need gets a new type. */
typedef struct RBTREND_MATERIAL_UNIFORM {
	/** The layout the member was found in. */
	rbtrend_binding_layout _layout;
	/** The block's binding: its place in the layout's bindings. */
	uint32_t _binding_index;
	/** The member's place in its block's members. */
	uint32_t _member_index;
} RBTREND_MATERIAL_UNIFORM;

/**
 * @brief Finds a member of one of a layout's uniform blocks by name, for
 * every material of that layout.
 *
 * The blocks searched are the ones a material of the layout can own: a
 * uniform buffer binding that is one buffer, not an array, and whose block
 * the layout knows (a layout made from shaders). Whether a given material
 * owns the block, or was told to leave it external, is checked when the
 * uniform is set, so the name means the same member in every material of
 * the layout.
 *
 * A layout of the same bindings made separately (rbtrendpipeline.h, "Layouts
 * of the same bindings") is another layout here: a uniform found in one is
 * refused by a material of the other.
 *
 * @param[in]  layout      Layout to search.
 * @param[in]  name        The member's path in its block, or its binding's
 *                         name (or its block name), a dot and that path, as
 *                         at the top of this header. Case-sensitive.
 * @param[out] out_uniform Receives the uniform. Not NULL.
 * @param[out] out_member  Receives the member: its type, offset, size and
 *                         array count, for a caller that checks them once
 *                         (that `roughness` is a float, say). Its `name` is
 *                         the layout's, valid as long as it. May be NULL.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; otherwise RBT_RESULT_INVALID_ARGUMENT or the failure's
 *         own code (rbtrendcontext.h, "Results and errors"), with nothing
 *         written, if an argument but @p out_member is NULL, no member of
 *         those blocks has that name, or more than one does and the name
 *         isn't qualified by its binding.
 */
RBT_RESULT rbtrend_material_find_uniform(
    rbtrend_binding_layout layout,
    const char *name,
    RBTREND_MATERIAL_UNIFORM *out_uniform,
    RBTREND_SHADER_MEMBER *out_member,
    RBTREND_ERROR *out_error);

/**
 * @brief Sets one uniform of a material's owned blocks, found beforehand
 * with rbtrend_material_find_uniform().
 *
 * Writes the material's copy of the block, where the SPIR-V lays the member
 * out: each array element at its array stride, each matrix column at its
 * matrix stride, and a `row_major` matrix transposed. What a frame reads is
 * under "When uniform values take effect" at the top of this header. It
 * costs the copy of the member's bytes and nothing else: no name is read.
 *
 * @param[in] material Material to write.
 * @param[in] uniform  The uniform, found in the material's layout.
 * @param[in] data     The whole member, tightly packed: its type's size
 *                     (rbtrend_uniform_type_get_size()) for each element, a
 *                     matrix column after column. A `mat3` is 36 bytes, as
 *                     glm keeps one. A `bool` is 4 bytes, as the shader
 *                     keeps it: pass a `uint32_t`, never a C `bool`, whose
 *                     one byte would be read as four.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; otherwise RBT_RESULT_INVALID_ARGUMENT or the failure's
 *         own code (rbtrendcontext.h, "Results and errors"), with nothing
 *         written, if @p material or @p data is NULL, @p uniform is no
 *         uniform or was found in another layout than the material's, its
 *         block is one the material doesn't own (named in
 *         `_external_blocks`), the member has no RBTREND_UNIFORM_TYPE (write
 *         it with rbtrend_material_set_uniform_bytes() at the offset
 *         rbtrend_material_find_uniform() gives), or it is an array without a
 *         size.
 */
RBT_RESULT rbtrend_material_set_uniform_value(
    rbtrend_material material,
    RBTREND_MATERIAL_UNIFORM uniform,
    const void *data,
    RBTREND_ERROR *out_error);

/**
 * @brief Reads one uniform of a material's owned blocks back: the value it
 * was last set to, or zeroes if it never was.
 *
 * The material's own copy of the block, not the GPU's: it is what the next
 * submission that binds the material reads.
 *
 * @param[in]  material Material to read.
 * @param[in]  uniform  The uniform, found in the material's layout.
 * @param[out] out_data Receives the whole member, tightly packed, as
 *                      rbtrend_material_set_uniform_value() takes it.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; otherwise RBT_RESULT_INVALID_ARGUMENT or the failure's
 *         own code (rbtrendcontext.h, "Results and errors"), with nothing
 *         written, under the conditions of
 *         rbtrend_material_set_uniform_value().
 */
RBT_RESULT rbtrend_material_get_uniform_value(
    rbtrend_material material,
    RBTREND_MATERIAL_UNIFORM uniform,
    void *out_data,
    RBTREND_ERROR *out_error);

/**
 * @brief Writes bytes into one of a material's owned blocks.
 *
 * As rbtrend_material_set_uniform(), for a caller that lays the block out
 * itself.
 *
 * @param[in] material Material to write.
 * @param[in] name     The binding's name in the layout, or its block
 *                     name.
 * @param[in] offset   Where in the block the bytes go.
 * @param[in] size     How many bytes. Not 0.
 * @param[in] data     The bytes.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; otherwise RBT_RESULT_INVALID_ARGUMENT or the failure's
 *         own code (rbtrendcontext.h, "Results and errors"), with nothing
 *         written, if an argument is NULL, @p size is 0, no binding has that
 *         name or its block is not owned, or the bytes run past the end of
 *         the block.
 */
RBT_RESULT rbtrend_material_set_uniform_bytes(
    rbtrend_material material,
    const char *name,
    uint32_t offset,
    uint32_t size,
    const void *data,
    RBTREND_ERROR *out_error);

/**
 * @brief Puts a uniform buffer the caller keeps in a uniform buffer binding
 * the material doesn't own, the whole buffer.
 *
 * A buffer of per-frame copies is read at the copy of the frame a list is
 * compiled in.
 *
 * @param[in] material      Material to change.
 * @param[in] name          The binding's name in the layout, or for a
 *                          uniform buffer binding its block name.
 * @param[in] array_element Index into the binding's array; 0 where its
 *                          `_count` is 1.
 * @param[in] buffer        The buffer, of #RBTREND_BUFFER_USAGE_UNIFORM.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; otherwise RBT_RESULT_INVALID_ARGUMENT or the failure's
 *         own code (rbtrendcontext.h, "Results and errors"), with nothing
 *         changed, if @p material, @p name or @p buffer is NULL, no binding
 *         has that name, it is not a uniform buffer binding or is an owned
 *         block, @p array_element is not below the binding's `_count`, or
 *         @p buffer is of another instance or lacks the usage.
 */
RBT_RESULT rbtrend_material_set_uniform_buffer(
    rbtrend_material material,
    const char *name,
    uint32_t array_element,
    rbtrend_buffer buffer,
    RBTREND_ERROR *out_error);

/**
 * @brief Puts a storage buffer in a storage buffer binding, the whole
 * buffer.
 *
 * As rbtrend_material_set_uniform_buffer(), for a storage buffer binding.
 *
 * @param[in] material      Material to change.
 * @param[in] name          The binding's name in the layout.
 * @param[in] array_element Index into the binding's array.
 * @param[in] buffer        The buffer, of #RBTREND_BUFFER_USAGE_STORAGE.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; otherwise RBT_RESULT_INVALID_ARGUMENT or the failure's
 *         own code (rbtrendcontext.h, "Results and errors"), with nothing
 *         changed, under the conditions of
 *         rbtrend_material_set_uniform_buffer(), for a binding that is not a
 *         storage buffer binding.
 */
RBT_RESULT rbtrend_material_set_storage_buffer(
    rbtrend_material material,
    const char *name,
    uint32_t array_element,
    rbtrend_buffer buffer,
    RBTREND_ERROR *out_error);

/**
 * @brief Puts a texture view in a sampled image, storage image or combined
 * image sampler binding.
 *
 * @param[in] material      Material to change.
 * @param[in] name          The binding's name in the layout.
 * @param[in] array_element Index into the binding's array.
 * @param[in] view          The view: of type
 *                          RBTREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS for a
 *                          storage image binding,
 *                          RBTREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE for the
 *                          other two. It must outlive every material that
 *                          holds it, or be replaced in each before its next
 *                          compile after it goes.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; otherwise RBT_RESULT_INVALID_ARGUMENT
 *         (RBT_RESULT_NOT_FOUND where nothing has the name or number) or the
 *         failure's own code (rbtrendcontext.h, "Results and errors"), with
 *         nothing changed, if an argument is NULL, no binding has that name,
 *         the binding is of none of those types, @p array_element is not
 *         below its `_count`, or @p view is of another instance or of the
 *         wrong type.
 */
RBT_RESULT rbtrend_material_set_texture(
    rbtrend_material material,
    const char *name,
    uint32_t array_element,
    rbtrend_texture_view view,
    RBTREND_ERROR *out_error);

/**
 * @brief Puts a sampler in a sampler or combined image sampler binding.
 *
 * @param[in] material      Material to change.
 * @param[in] name          The binding's name in the layout.
 * @param[in] array_element Index into the binding's array.
 * @param[in] sampler       The sampler. As @p view of
 *                          rbtrend_material_set_texture().
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; otherwise RBT_RESULT_INVALID_ARGUMENT
 *         (RBT_RESULT_NOT_FOUND where nothing has the name or number) or the
 *         failure's own code (rbtrendcontext.h, "Results and errors"), with
 *         nothing changed, if an argument is NULL, no binding has that name,
 *         the binding is of neither type, @p array_element is not below its
 *         `_count`, or @p sampler is of another instance.
 */
RBT_RESULT rbtrend_material_set_sampler(
    rbtrend_material material,
    const char *name,
    uint32_t array_element,
    rbtrend_sampler sampler,
    RBTREND_ERROR *out_error);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // RBTREND_MATERIAL_H
