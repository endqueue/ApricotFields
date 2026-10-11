
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
 * two owned blocks have a member of one name. A name that matches nothing
 * is a failed call, not a silent one.
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
 * @warning No later submission may bind @p material, and no command list
 * that binds it may be compiled or submitted again: submitting one copies
 * the material's blocks.
 *
 * @param[in] material Material to destroy. NULL is accepted and does nothing.
 */
void aprend_material_destroy(aprend_material material);

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
 * and a sampler.
 *
 * @param[in] material Material to read.
 *
 * @return true if it is complete; false if not, or if @p material is NULL.
 */
bool aprend_material_is_complete(aprend_material material);

/**
 * @brief Sets one uniform of a material's owned blocks, by name.
 *
 * Writes the material's copy of the block, where the SPIR-V lays the member
 * out: each array element at its array stride, each matrix column at its
 * matrix stride, and a `row_major` matrix transposed. What a frame reads is
 * under "When uniform values take effect" at the top of this header.
 *
 * @param[in] material Material to write.
 * @param[in] name     The member's path in its block, or its binding's name,
 *                     a dot and that path (see the top of this header).
 *                     Case-sensitive.
 * @param[in] data     The whole member, tightly packed: its type's size
 *                     (aprend_uniform_type_get_size()) for each element, a
 *                     matrix column after column. A `mat3` is 36 bytes, as
 *                     glm keeps one. A `bool` is 4 bytes, as the shader
 *                     keeps it: pass a `uint32_t`, never a C `bool`, whose
 *                     one byte would be read as four.
 *
 * @return false, with nothing written, if an argument is NULL, no member of
 *         an owned block has that name, more than one does and the name
 *         isn't qualified by its binding, the member has no
 *         APREND_UNIFORM_TYPE (write it with
 *         aprend_material_set_uniform_bytes()), or it is an array without a
 *         size.
 */
bool aprend_material_set_uniform(
    aprend_material material,
    const char *name,
    const void *data);

/**
 * @brief Writes bytes into one of a material's owned blocks.
 *
 * As aprend_material_set_uniform(), for a caller that lays the block out
 * itself.
 *
 * @param[in] material Material to write.
 * @param[in] name     The binding's name in the layout.
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
 * @param[in] name          The binding's name in the layout.
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
 *                          other two. It must outlive the material, or be
 *                          replaced before the next compile after it goes.
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
 * @param[in] sampler       The sampler. It must outlive the material, or be
 *                          replaced before the next compile after it goes.
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
