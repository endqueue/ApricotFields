
#ifndef APREND_MATERIAL_H
#define APREND_MATERIAL_H

/**
 * @file aprendmaterial.h
 * @brief Materials: the resources one aprend_binding_layout's slots hold,
 * set by name, and the bytes of the uniform blocks among them.
 *
 * A material fills one set slot of a pipeline. It is a binding layout and
 * what each of the layout's bindings holds, each binding found by its name
 * in the layout (aprend_binding_layout_find(), aprendpipeline.h):
 *
 * - A uniform block the material owns: every uniform buffer binding whose
 *   block the layout knows (a layout made from shaders,
 *   aprend_binding_layout_create_from_shaders()) and that is one buffer, not
 *   an array, unless the desc names it external. Its members are written
 *   by name with aprend_material_set_uniform(), at the offsets, strides and
 *   matrix layout the SPIR-V gives them, or as bytes with
 *   aprend_material_set_uniform_bytes(). The material keeps a copy of the
 *   block's bytes, and the block itself at a fixed range of the instance's
 *   material arena: uniform buffers in the device's own memory, shared by
 *   every material of the instance, each range 256-byte aligned. A value
 *   set once is kept until it is set again; there is one copy of the block
 *   on the GPU, not one for each frame, and one descriptor set for the
 *   material.
 * - Anything else a binding set holds, given by the caller: a uniform
 *   buffer or buffer set it keeps itself (an external block), a storage
 *   buffer or buffer set, a texture view, a sampler.
 *
 * It is bound with APREND_COMMAND_SET_MATERIAL (aprendcommands.h), in place
 * of an aprend_binding_set, at the slot where the pipeline declares the
 * material's layout or a layout of the same bindings (aprendpipeline.h,
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
 * aprend_material_set_uniform() searches the layout's members by name at
 * every call. A value set every frame, or in many materials, is found once
 * instead: aprend_material_find_uniform() turns a name into an
 * aprend_material_uniform, a small value that names the member within the
 * layout, and aprend_material_set_uniform_value() writes it with no search.
 * One is found from the layout, not from a material, so it serves every
 * material of that layout: found when the layout is made, used for each
 * material after. aprend_material_set_uniform() is a find and a set.
 *
 * @par When resources take effect
 * The material builds its aprend_binding_set (aprendpipeline.h) when a command
 * list that binds it is compiled, from what it holds then, and builds it
 * again at the next compile after any of its resources is set to something
 * else. Building it again is aprend_binding_set_update(): it costs what
 * creating a set does, and a command list compiled before the change and
 * submitted after it still reads what the material held at its compile. So
 * a resource is set before the compile of every list that is to see it.
 * Every binding must hold something by then: aprend_material_is_complete().
 *
 * @par Copying, and what an unset binding holds
 * aprend_material_copy() makes a material that starts as another is: its
 * values, and the same resources. Aprend puts nothing in a binding the
 * caller leaves empty, and a material with one can't be drawn: what an
 * empty slot should read (white, black, a flat normal, a texture of the
 * right shape and type) depends on what the slot is for, which the caller
 * knows and Aprend doesn't. A caller that wants every material of a layout
 * to start from defaults of its choosing makes one material with them and
 * copies it. (aprend_lit_material_create(), aprendlit.h, is such a start for
 * Aprend's own lit layout, whose slots Aprend does know.)
 *
 * @par When uniform values take effect
 * A value set is seen by every submission made after it of a list that
 * binds the material, and by none made before. At the submission of such a
 * list, the blocks the material owns that have changed since they were last
 * copied are written to a span of the current frame's transient memory
 * (aprendtransient.h), and copied from there into their ranges by the GPU,
 * in the same submission and ahead of the list's own commands. The copy
 * waits for the earlier submissions that read the range, so work already on
 * the GPU keeps the values it was submitted with, and a value may be set
 * between any two submissions, of one frame or of two. A material whose
 * values don't change costs nothing per frame.
 *
 * So an instance with materials that own a block has transient memory
 * (aprend_instance_desc::transient_chunk_size): a copy takes the block's
 * size of it, rounded up to 16, in the frame it is made.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A material is used by one
 * thread at a time with everything else of its instance: its set calls,
 * and the compile and submission of every list that binds it, which build
 * its binding set and copy its blocks. aprend_material_destroy() may be
 * called from any thread, as any destroy call.
 */

#include "aprendpipeline.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @brief Opaque handle to a material.
 */
typedef struct aprend_material_t *aprend_material;

/**
 * @brief Configuration descriptor for a material.
 *
 * No field has a default.
 */
typedef struct aprend_material_desc {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_MATERIAL_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * The layout whose slots the material fills: the one the pipelines it is
	 * drawn with declare at the slot it is bound to, usually made from their
	 * shaders. Not NULL, and of the instance the material is created on.
	 * Every binding has a name. It must outlive the material.
	 */
	aprend_binding_layout _layout;
	/**
	 * The names of uniform buffer bindings the material doesn't own though it
	 * could: blocks the caller fills with a buffer of its own, with
	 * aprend_material_set_uniform_buffer() - one shared by many materials,
	 * say. Each names a uniform buffer binding of #_layout. May be NULL with
	 * a count of 0. Only read during aprend_material_create().
	 */
	const char *const *_external_blocks;
	/** Number of elements in #_external_blocks. */
	uint32_t _external_block_count;
} aprend_material_desc;

/**
 * @brief Creates a material. Its owned blocks start as zeroes; every other
 * binding starts empty.
 *
 * @param[in] instance Instance the material's buffers are created on.
 * @param[in] desc     Material configuration. Only read during the call.
 *
 * @return The material, or NULL if @p instance or @p desc is NULL, `_layout`
 *         is NULL or of another instance, a binding of the layout has no
 *         name, two have the same name, `_external_blocks` is NULL with a
 *         count above 0 or names something that is not a uniform buffer
 *         binding of the layout, the material owns a block and the instance
 *         has no transient memory, or the material arena needs another
 *         buffer and the device refuses it.
 *
 * @see aprend_material_destroy()
 */
aprend_material aprend_material_create(
    aprend_instance instance,
    const aprend_material_desc *desc);

/**
 * @brief Destroys a material and its binding set, and gives its blocks'
 * ranges back to the instance's material arena.
 *
 * What it was given by the caller is the caller's and is not destroyed. The
 * GPU doesn't have to be finished with it (aprendcontext.h, "Destroying"):
 * the ranges are handed out again only once everything submitted up to now
 * has finished.
 *
 * A copy made from it (aprend_material_copy()) is not affected. A command
 * list compiled with it and not yet submitted is refused at submit
 * (aprend_command_list_submit(), aprendcommands.h).
 *
 * @warning No command list that binds @p material may be compiled after
 * this.
 *
 * @param[in] material Material to destroy. NULL is accepted and does nothing.
 */
void aprend_material_destroy(aprend_material material);

/**
 * @brief Creates a material that starts as another is.
 *
 * The new material fills the same layout, owns the same blocks (those the
 * source was not told to leave external), and holds the source's values in
 * them in blocks of its own. Every resource the source holds - a view, a
 * sampler, a uniform or storage buffer or buffer set - it holds too: the
 * same handles, shared, not duplicated, so each must outlive both
 * materials or be replaced in each before its next compile. A binding the
 * source leaves empty is empty in the copy. After the call the two are
 * independent: setting a value or a resource of one does not change the
 * other, and either may be destroyed first.
 *
 * Costs what aprend_material_create() does - a range of the material arena
 * for each owned block - and, at the first submission that binds the copy,
 * the copy of its blocks to the GPU, as for a new material. Its binding set
 * is built at the first compile that binds it. A copy of a lit material
 * (aprend_lit_material_create(), aprendlit.h) is a lit material.
 *
 * @param[in] source Material to copy. Only read during the call.
 *
 * @return The copy, or NULL if @p source is NULL, there is no memory, or the
 *         material arena needs another buffer and the device refuses it.
 *         Each is printed.
 *
 * @see aprend_material_destroy()
 */
aprend_material aprend_material_copy(aprend_material source);

/**
 * @brief Gets the layout a material fills.
 *
 * @param[in] material Material to read.
 *
 * @return The layout, or NULL if @p material is NULL.
 */
aprend_binding_layout aprend_material_get_layout(aprend_material material);

/**
 * @brief Whether every binding of a material holds what its type reads: a
 * command list that binds it can't be compiled until it does.
 *
 * Owned blocks always do. A combined image sampler binding needs a texture
 * and a sampler. A compile that finds one missing prints which binding and
 * element it is.
 *
 * @param[in] material Material to read.
 *
 * @return true if it is complete; false if not, or if @p material is NULL.
 */
bool aprend_material_is_complete(aprend_material material);

/**
 * @brief Sets one uniform of a material's owned blocks, by name.
 *
 * aprend_material_find_uniform() on the material's layout, then
 * aprend_material_set_uniform_value(): see both for what is written and
 * when it is refused. For a value set every frame or in many materials,
 * find it once and call aprend_material_set_uniform_value() (see "Finding
 * a uniform once" at the top of this header).
 *
 * @param[in] material Material to write.
 * @param[in] name     The member's path in its block, or its binding's name,
 *                     a dot and that path (see the top of this header).
 *                     Case-sensitive.
 * @param[in] data     The whole member, tightly packed, as
 *                     aprend_material_set_uniform_value() takes it.
 *
 * @return false, with nothing written, if an argument is NULL, or either
 *         call it makes would return false.
 */
bool aprend_material_set_uniform(
    aprend_material material,
    const char *name,
    const void *data);

/**
 * @brief One member of a uniform block of a binding layout, as
 * aprend_material_find_uniform() found it: what
 * aprend_material_set_uniform_value() and
 * aprend_material_get_uniform_value() write and read with no search.
 *
 * A value, copied and kept as the caller likes, and valid as long as its
 * layout. Its fields are Aprend's: they are filled by
 * aprend_material_find_uniform() and read only by the calls that take one,
 * never by the caller. All zeroes is no uniform, and every call that takes
 * it refuses it.
 */
/* Frozen: a value, Aprend's to fill in. It never changes; a new need gets a new type. */
typedef struct aprend_material_uniform {
	/** The layout the member was found in. */
	aprend_binding_layout _layout;
	/** The block's binding: its place in the layout's bindings. */
	uint32_t _binding_index;
	/** The member's place in its block's members. */
	uint32_t _member_index;
} aprend_material_uniform;

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
 * A layout of the same bindings made separately (aprendpipeline.h, "Layouts
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
 *
 * @return false, with nothing written, if an argument but @p out_member is
 *         NULL, no member of those blocks has that name, or more than one
 *         does and the name isn't qualified by its binding. Each is printed.
 */
bool aprend_material_find_uniform(
    aprend_binding_layout layout,
    const char *name,
    aprend_material_uniform *out_uniform,
    aprend_shader_member *out_member);

/**
 * @brief Sets one uniform of a material's owned blocks, found beforehand
 * with aprend_material_find_uniform().
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
 *                     (aprend_uniform_type_get_size()) for each element, a
 *                     matrix column after column. A `mat3` is 36 bytes, as
 *                     glm keeps one. A `bool` is 4 bytes, as the shader
 *                     keeps it: pass a `uint32_t`, never a C `bool`, whose
 *                     one byte would be read as four.
 *
 * @return false, with nothing written, if @p material or @p data is NULL,
 *         @p uniform is no uniform or was found in another layout than the
 *         material's, its block is one the material doesn't own (named in
 *         `_external_blocks`), the member has no APREND_UNIFORM_TYPE (write
 *         it with aprend_material_set_uniform_bytes() at the offset
 *         aprend_material_find_uniform() gives), or it is an array without
 *         a size. Each is printed.
 */
bool aprend_material_set_uniform_value(
    aprend_material material,
    aprend_material_uniform uniform,
    const void *data);

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
 *                      aprend_material_set_uniform_value() takes it.
 *
 * @return false, with nothing written, under the conditions of
 *         aprend_material_set_uniform_value().
 */
bool aprend_material_get_uniform_value(
    aprend_material material,
    aprend_material_uniform uniform,
    void *out_data);

/**
 * @brief Writes bytes into one of a material's owned blocks.
 *
 * As aprend_material_set_uniform(), for a caller that lays the block out
 * itself.
 *
 * @param[in] material Material to write.
 * @param[in] name     The binding's name in the layout, or its block
 *                     name.
 * @param[in] offset   Where in the block the bytes go.
 * @param[in] size     How many bytes. Not 0.
 * @param[in] data     The bytes.
 *
 * @return false, with nothing written, if an argument is NULL, @p size is 0,
 *         no binding has that name or its block is not owned, or the bytes
 *         run past the end of the block.
 */
bool aprend_material_set_uniform_bytes(
    aprend_material material,
    const char *name,
    uint32_t offset,
    uint32_t size,
    const void *data);

/**
 * @brief Puts a uniform buffer the caller keeps in a uniform buffer binding
 * the material doesn't own, the whole buffer.
 *
 * Exactly one of @p buffer and @p set is given: a single buffer, or a buffer
 * set read at the copy of the frame a list is compiled in.
 *
 * @param[in] material      Material to change.
 * @param[in] name          The binding's name in the layout, or for a
 *                          uniform buffer binding its block name.
 * @param[in] array_element Index into the binding's array; 0 where its
 *                          `_count` is 1.
 * @param[in] buffer        The buffer, or NULL.
 * @param[in] set           The buffer set, or NULL.
 *
 * @return false, with nothing changed, if @p material or @p name is NULL, no
 *         binding has that name, it is not a uniform buffer binding or is
 *         an owned block, @p array_element is not below the binding's
 *         `_count`, or not exactly one of @p buffer and @p set is given or
 *         it is of another instance.
 */
bool aprend_material_set_uniform_buffer(
    aprend_material material,
    const char *name,
    uint32_t array_element,
    aprend_uniform_buffer buffer,
    aprend_uniform_buffer_set set);

/**
 * @brief Puts a storage buffer in a storage buffer binding, the whole
 * buffer.
 *
 * As aprend_material_set_uniform_buffer(), for a storage buffer binding.
 *
 * @param[in] material      Material to change.
 * @param[in] name          The binding's name in the layout.
 * @param[in] array_element Index into the binding's array.
 * @param[in] buffer        The buffer, or NULL.
 * @param[in] set           The buffer set, or NULL.
 *
 * @return false, with nothing changed, under the conditions of
 *         aprend_material_set_uniform_buffer(), for a binding that is not a
 *         storage buffer binding.
 */
bool aprend_material_set_storage_buffer(
    aprend_material material,
    const char *name,
    uint32_t array_element,
    aprend_storage_buffer buffer,
    aprend_storage_buffer_set set);

/**
 * @brief Puts a texture view in a sampled image, storage image or combined
 * image sampler binding.
 *
 * @param[in] material      Material to change.
 * @param[in] name          The binding's name in the layout.
 * @param[in] array_element Index into the binding's array.
 * @param[in] view          The view: of type
 *                          APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS for a
 *                          storage image binding,
 *                          APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE for the
 *                          other two. It must outlive every material that
 *                          holds it, or be replaced in each before its next
 *                          compile after it goes.
 *
 * @return false, with nothing changed, if an argument is NULL, no binding has
 *         that name, the binding is of none of those types,
 *         @p array_element is not below its `_count`, or @p view is of
 *         another instance or of the wrong type.
 */
bool aprend_material_set_texture(
    aprend_material material,
    const char *name,
    uint32_t array_element,
    aprend_texture_view view);

/**
 * @brief Puts a sampler in a sampler or combined image sampler binding.
 *
 * @param[in] material      Material to change.
 * @param[in] name          The binding's name in the layout.
 * @param[in] array_element Index into the binding's array.
 * @param[in] sampler       The sampler. As @p view of
 *                          aprend_material_set_texture().
 *
 * @return false, with nothing changed, if an argument is NULL, no binding has
 *         that name, the binding is of neither type, @p array_element is not
 *         below its `_count`, or @p sampler is of another instance.
 */
bool aprend_material_set_sampler(
    aprend_material material,
    const char *name,
    uint32_t array_element,
    aprend_sampler sampler);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_MATERIAL_H
