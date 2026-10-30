
#ifndef APREND_BUFFERS_H
#define APREND_BUFFERS_H

#include "aprendcontext.h"
#include "stdint.h"

/**
 * @file aprendbuffers.h
 * @brief Buffers over SpudGPU buffers, uniform block layouts, and vertex and
 * index pools.
 *
 * One kind of buffer, aprend_buffer, for every use: a usage mask says what
 * it is bound as (vertex, index, uniform, storage, indirect arguments), and
 * #APREND_BUFFER_WRITES how often the CPU writes it. A buffer rewritten every
 * frame can be made of one copy per frame in flight ("Per-frame copies",
 * below). Nothing here knows what the bytes are: a mesh's layout is the
 * pipeline's, a uniform block's the shader's.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A buffer is used by one
 * caller at a time with everything else of its instance. A write and a
 * submission of a list that uses the buffer never overlap: the write
 * changes what the submission reads. A write to a buffer of per-frame
 * copies also reads the instance's frame index. The calls that take no
 * buffer or pool (aprend_uniform_type_get_size(),
 * aprend_uniform_layout_get_uniform_index() and
 * aprend_uniform_layout_get_size()) take no object, so they may run at once
 * with anything.
 *
 * @par Where a buffer is kept
 * A buffer with #APREND_BUFFER_USAGE_UNIFORM is kept in memory the CPU
 * writes and the GPU reads (SPUDGPU_MEMORY_KIND_UPLOAD), mapped for its
 * life, on every device: a write goes straight into what the GPU reads.
 *
 * Any other buffer follows the device's memory
 * (SPUDGPU_ADAPTER_PROPERTIES::unified_memory of its adapter, and
 * SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY if the device was created with it,
 * read when the instance is created), and the calls behave the same either
 * way. On a device whose memory is the system's, the buffer is mapped and a
 * write goes straight into it; a storage buffer needs the device to have
 * SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE for that. On a device with memory of
 * its own, or for a storage buffer without that kind, the buffer is in the
 * device's memory and a write goes into a staging copy; the next
 * aprend_command_list_submit() of a list that uses the buffer copies the
 * bytes written since the last such submission across, ahead of the list's
 * own commands. Only those bytes are copied: two writes far apart are two
 * copies, not one over everything between them.
 *
 * What the staging copy is depends on #APREND_BUFFER_WRITES. A buffer
 * written many times keeps a staging buffer of its own size for life. A
 * buffer written once, at creation, has the bytes it was created with
 * staged in a buffer of their own, which is released once the submission
 * that copies them has run; one created with no bytes has no staging at
 * all.
 *
 * The rule for the caller is one rule: a write (at creation or by
 * aprend_buffer_write()) is seen by any submission made after it, and must
 * not be made while submitted work that uses the buffer has yet to finish.
 * A buffer written and never used by a submitted list is never copied, and
 * keeps its staging copy until it is. A uniform buffer's write lands in the
 * memory the GPU reads, so the rule holds more sharply there: written while
 * a submitted frame still reads it, it changes what that frame sees. A
 * buffer rewritten every frame is made of per-frame copies.
 *
 * @par Per-frame copies
 * A buffer that is rewritten every frame can only be written once the GPU
 * has finished the last frame that reads it. Created with
 * APREND_BUFFER_DESC::per_frame, it holds
 * APREND_INSTANCE_DESC::frames_in_flight copies instead, behind the one
 * handle. aprend_buffer_write() writes the copy of the current frame
 * (aprend_instance_next_frame(), aprendcontext.h), and a binding set or a
 * command that holds the buffer makes a command list read the copy of the
 * frame it is compiled in. So the frame being written and the frames still
 * on the GPU never share a copy. Each copy keeps what was last written to
 * it, which was `frames_in_flight` frames ago, not last frame: a write is
 * not carried from one copy to the next. Write, each frame, everything that
 * frame reads. With `frames_in_flight` 1 there is one copy.
 */

#if __cplusplus
extern "C" {
#endif

/** @brief Opaque handle to a buffer. */
typedef struct aprend_buffer_t *aprend_buffer;

/**
 * @brief What a buffer is bound as, a mask given when it is created.
 *
 * A buffer is in one state for the GPU at a time, and a command list moves
 * it between the states its uses need only where it knows how: so a mask
 * is one bit, or #APREND_BUFFER_USAGE_STORAGE with
 * #APREND_BUFFER_USAGE_INDIRECT_ARGUMENTS. Any other combination is refused
 * with APRESULT_UNSUPPORTED.
 */
typedef uint32_t APREND_BUFFER_USAGE;
enum {
	/** No use: refused, a buffer is made for something. */
	APREND_BUFFER_USAGE_NONE = 0,
	/**
	 * Read by a pipeline as vertices or instances: a buffer of
	 * APREND_COMMAND_SET_VERTEX_BUFFERS (aprendcommands.h). It holds no
	 * layout and no stride: what its bytes are is the pipeline's to say
	 * (APREND_VERTEX_BINDING_DESC, aprendpipeline.h), so one buffer can be
	 * read by pipelines that read different parts of the same vertices.
	 */
	APREND_BUFFER_USAGE_VERTEX = 1,
	/**
	 * Read as indices of APREND_BUFFER_DESC::index_stride each: the buffer
	 * of APREND_COMMAND_SET_INDEX_BUFFER.
	 */
	APREND_BUFFER_USAGE_INDEX = 2,
	/**
	 * A uniform slot of a binding set (aprendpipeline.h). Kept mapped, and
	 * written directly on every device ("Where a buffer is kept", at the
	 * top of this header).
	 */
	APREND_BUFFER_USAGE_UNIFORM = 4,
	/** A storage slot of a binding set, read and written by shaders. */
	APREND_BUFFER_USAGE_STORAGE = 8,
	/**
	 * The argument buffer of APREND_COMMAND_DRAW_INDIRECT and
	 * APREND_COMMAND_DRAW_INDEXED_INDIRECT (aprendcommands.h). With
	 * #APREND_BUFFER_USAGE_STORAGE, the buffer a compute shader fills
	 * through a storage slot is then read by the draws, with no trip
	 * through the CPU; a command list moves it between the two uses itself.
	 */
	APREND_BUFFER_USAGE_INDIRECT_ARGUMENTS = 16
};

/**
 * @brief How often the CPU writes a buffer, given when it is created.
 *
 * It decides what a write is staged in on a device with memory of its own
 * ("Where a buffer is kept", at the top of this header); on a device whose
 * memory is the system's, and for a uniform buffer on any device, the
 * buffer is written directly either way. A GPU that writes the buffer (a
 * storage buffer written by a shader) is not counted: only the CPU's writes
 * are.
 */
typedef uint32_t APREND_BUFFER_WRITES;
enum {
	/** Not chosen: refused, there is no default. */
	APREND_BUFFER_WRITES_NONE = 0,
	/**
	 * Written at creation, with APREND_BUFFER_DESC::initial_data, and never
	 * again: aprend_buffer_write() refuses it. The staging copy of those
	 * bytes is released once the submission that copies them has run, so a
	 * buffer of meshes loaded once costs its own size and no more. Created
	 * with no bytes, the buffer is never staged: right for one only a
	 * shader writes.
	 */
	APREND_BUFFER_WRITES_ONCE = 1,
	/**
	 * Written at creation and by aprend_buffer_write(), as often as needed.
	 * The buffer keeps a staging buffer of its own size for life, where it
	 * needs one.
	 */
	APREND_BUFFER_WRITES_MANY = 2
};

/**
 * @brief The size of one index of an index buffer, in bytes.
 */
typedef enum APREND_INDEX_STRIDE APRI_ENUM_BASE {
	APREND_INDEX_STRIDE_NONE   = 0, /**< Not an index buffer. */
	APREND_INDEX_STRIDE_UINT16 = 2, /**< 16-bit indices. */
	APREND_INDEX_STRIDE_UINT32 = 4  /**< 32-bit indices. */
} APREND_INDEX_STRIDE;

/**
 * @brief Configuration descriptor for a buffer.
 */
typedef struct APREND_BUFFER_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_BUFFER_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * The buffer's size in bytes, each copy's for a buffer of per-frame
	 * copies. Not 0. A uniform buffer's is usually its block's,
	 * aprend_uniform_layout_get_size().
	 */
	uint64_t size;
	/** What the buffer is bound as. @see APREND_BUFFER_USAGE */
	APREND_BUFFER_USAGE usage;
	/** How often the CPU writes it: ONCE or MANY. @see APREND_BUFFER_WRITES */
	APREND_BUFFER_WRITES writes;
	/**
	 * The size of one index, UINT16 or UINT32, for a buffer with
	 * #APREND_BUFFER_USAGE_INDEX; NONE for any other. #size is then a
	 * multiple of it.
	 */
	APREND_INDEX_STRIDE index_stride;
	/**
	 * One copy for each frame in flight behind the one handle ("Per-frame
	 * copies", at the top of this header), or false for one copy.
	 */
	bool per_frame;
	/**
	 * #size bytes every copy starts with, or NULL. Only read during the
	 * call. With none, a uniform buffer starts as zeroes and any other is
	 * undefined until written.
	 */
	const void *initial_data;
	/** A name for diagnostics and messages, copied. May be NULL. */
	const char *debug_name;
} APREND_BUFFER_DESC;

/**
 * @brief Creates a buffer.
 *
 * @param[in]  instance   Instance whose device the buffer is created on.
 * @param[in]  desc       Buffer configuration. Only read during the call.
 * @param[out] out_buffer Receives the buffer; NULL on failure.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance, @p desc or
 *         @p out_buffer is NULL, `size` is 0, `usage` is NONE or has a bit
 *         that is not an APREND_BUFFER_USAGE, `writes` is not ONCE or MANY,
 *         `index_stride` is not UINT16 or UINT32 for an index buffer or not
 *         NONE for any other, or `size` is not a multiple of it;
 *         APRESULT_UNSUPPORTED for a combination of usages the header
 *         doesn't allow (#APREND_BUFFER_USAGE); the struct codes for
 *         `desc->base`; APRESULT_OUT_OF_MEMORY; or the code for SpudGPU's
 *         result if a buffer can't be created or written (aprendcontext.h,
 *         "Results and errors").
 *
 * @see aprend_buffer_destroy()
 */
APRESULT aprend_buffer_create(
	aprend_instance instance,
	const APREND_BUFFER_DESC *desc,
	aprend_buffer *out_buffer,
	APREND_ERROR *out_error);

/**
 * @brief Destroys a buffer, every copy of it.
 *
 * The GPU doesn't have to be finished with it (aprendcontext.h,
 * "Destroying"). Every binding set holding @p buffer goes first, and no
 * later submission may use it; a command list compiled with it and not yet
 * submitted is refused at submit (aprendcommands.h).
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p buffer itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] buffer Buffer to destroy. NULL is accepted and does nothing.
 */
void aprend_buffer_destroy(aprend_buffer buffer);

/**
 * @brief Reads a buffer's descriptor: the one it was created with, except
 * that `initial_data` is NULL and `debug_name` is the buffer's own copy,
 * valid as long as the buffer.
 *
 * The whole struct is written, `base` included.
 *
 * @param[in]  buffer   Buffer to read.
 * @param[out] out_desc Receives the descriptor.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p buffer or @p out_desc is NULL.
 */
APRESULT aprend_buffer_get_desc(
	aprend_buffer buffer,
	APREND_BUFFER_DESC *out_desc);

/**
 * @brief Writes bytes into a buffer: the current frame's copy, for a buffer
 * of per-frame copies.
 *
 * Not while a submission that reads that copy is unfinished ("Where a
 * buffer is kept", at the top of this header).
 *
 * @param[in]  buffer    Buffer to write.
 * @param[in]  offset    Where in the buffer the bytes go.
 * @param[in]  data      The bytes.
 * @param[in]  data_size How many bytes. Not 0.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT, with nothing written, if
 *         @p buffer or @p data is NULL, @p data_size is 0, the bytes run
 *         past the end of the buffer, or the buffer was created
 *         APREND_BUFFER_WRITES_ONCE; APRESULT_OUT_OF_MEMORY if there is no
 *         memory to note the write; or the code for SpudGPU's result if it
 *         can't be flushed or staged.
 */
APRESULT aprend_buffer_write(
	aprend_buffer buffer,
	uint64_t offset,
	const void *data,
	uint64_t data_size,
	APREND_ERROR *out_error);

/**
 * @name Uniform blocks
 *
 * A uniform buffer's bytes are a std140 block the shader declares. A layout
 * names the block's members, so one can be written by name or by index
 * with aprend_buffer_write_uniform(). Written by hand, or read from a
 * shader: aprend_binding_layout_get_uniform_layout() (aprendpipeline.h)
 * gives the layout of a block of a layout made from shaders, with every
 * offset and the block's size as the SPIR-V has them, and its structs and
 * arrays of structs listed member by member under names such as
 * `lights[2].color`. A buffer keeps no layout: the caller passes the one it
 * wrote the block by.
 * @{
 */

/**
 * @brief The type of one uniform of a uniform block.
 */
typedef uint32_t APREND_UNIFORM_TYPE;
enum {
	/** A `bool`: 4 bytes, given as a `uint32_t` (a C `bool` is one byte). */
	APREND_UNIFORM_TYPE_BOOL  = 0,
	/** An `int`: 4 bytes. */
	APREND_UNIFORM_TYPE_INT   = 1,
	/** An `ivec2`: 8 bytes. */
	APREND_UNIFORM_TYPE_INT2  = 2,
	/** An `ivec3`: 12 bytes. */
	APREND_UNIFORM_TYPE_INT3  = 3,
	/** An `ivec4`: 16 bytes. */
	APREND_UNIFORM_TYPE_INT4  = 4,
	/** A `uint`: 4 bytes. */
	APREND_UNIFORM_TYPE_UINT  = 5,
	/** A `uvec2`: 8 bytes. */
	APREND_UNIFORM_TYPE_UINT2 = 6,
	/** A `uvec3`: 12 bytes. */
	APREND_UNIFORM_TYPE_UINT3 = 7,
	/** A `uvec4`: 16 bytes. */
	APREND_UNIFORM_TYPE_UINT4 = 8,
	/** A `float`: 4 bytes. */
	APREND_UNIFORM_TYPE_FLOAT = 9,
	/** A `vec2`: 8 bytes. */
	APREND_UNIFORM_TYPE_VEC2  = 10,
	/** A `vec3`: 12 bytes. */
	APREND_UNIFORM_TYPE_VEC3  = 11,
	/** A `vec4`: 16 bytes. */
	APREND_UNIFORM_TYPE_VEC4  = 12,
	/** A `mat4`: 64 bytes, four columns of 16. */
	APREND_UNIFORM_TYPE_MAT4  = 13,
	/**
	 * A `mat2`: 16 bytes tightly packed, two columns of 8. In a std140 block
	 * each column takes 16, so it takes 32 there.
	 */
	APREND_UNIFORM_TYPE_MAT2  = 14,
	/**
	 * A `mat3`: 36 bytes tightly packed, three columns of 12. In a std140
	 * block each column takes 16, so it takes 48 there.
	 */
	APREND_UNIFORM_TYPE_MAT3  = 15
};

/**
 * @brief Gets the size of one uniform of a type.
 *
 * @param[in] type The type.
 *
 * @return Its size in bytes, tightly packed, or 0 for a value that isn't an
 *         APREND_UNIFORM_TYPE. A matrix's is its columns' tightly packed (a
 *         `mat3`'s is 36), which is what aprend_buffer_write_uniform()
 *         takes; a block keeps each column 16 bytes apart.
 */
uint32_t aprend_uniform_type_get_size(APREND_UNIFORM_TYPE type);

/**
 * @brief One uniform of a uniform block.
 */
/* Frozen: an array element of APREND_UNIFORM_LAYOUT. It never changes; a new need gets a new type. */
typedef struct APREND_UNIFORM {
	/**
	 * The uniform's name, for aprend_uniform_layout_get_uniform_index(). May
	 * be NULL.
	 */
	const char *name;
	/** @see APREND_UNIFORM_TYPE */
	APREND_UNIFORM_TYPE type;
	/**
	 * How many elements, for an array; 1 (or 0) for a uniform that is not
	 * one. Not a size in bytes.
	 */
	uint32_t array_count;
	/** Where the uniform starts in the block, in bytes. The caller's. */
	uint32_t offset;
} APREND_UNIFORM;

/**
 * @brief The uniforms of one uniform block.
 *
 * Nothing is placed: each uniform's `offset` is the caller's and must be
 * where the shader's block has it, which a layout from
 * aprend_binding_layout_get_uniform_layout() always is. std140 is assumed
 * for what takes room: an array (an `array_count` above 1) takes 16 bytes
 * an element, or the type's size rounded up to 16 if that is larger, and a
 * matrix takes 16 bytes a column.
 */
/* Frozen: a block's layout as a shader declares it, written by hand or read back. It never changes; a new need gets a new type. */
typedef struct APREND_UNIFORM_LAYOUT {
	/** How many uniforms. */
	uint32_t count;
	/** The uniforms, #count of them. Only read. */
	const APREND_UNIFORM *uniforms;
	/**
	 * The block's size in bytes as the shader declares it, or 0 to take it
	 * from where the furthest uniform ends. A block with members that have no
	 * APREND_UNIFORM_TYPE (a double, a non-square matrix) has them left out
	 * of #uniforms, and needs this to be sized right.
	 */
	uint32_t size;
} APREND_UNIFORM_LAYOUT;

/**
 * @brief Finds a uniform of a layout by name, once: the index
 * aprend_buffer_write_uniform() takes.
 *
 * The index is the uniform's place in @p layout's `uniforms`, so a caller
 * that wrote the layout knows it already. A uniform written every frame is
 * found once, when its layout is made, rather than by name at every write.
 *
 * @param[in]  layout    Layout to search.
 * @param[in]  name      The uniform's name, case-sensitive.
 * @param[out] out_index Receives the index of the first uniform of
 *                       @p layout named @p name; UINT32_MAX if there is
 *                       none.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p layout, @p name or
 *         @p out_index is NULL, or the layout's `uniforms` is NULL with a
 *         `count` above 0; APRESULT_NOT_FOUND if no uniform has the name.
 *
 * @see aprend_vertex_layout_get_element_index(), the same for a vertex
 *      layout.
 */
APRESULT aprend_uniform_layout_get_uniform_index(
	const APREND_UNIFORM_LAYOUT *layout,
	const char *name,
	uint32_t *out_index);

/**
 * @brief Gets the size of a uniform layout's block: where it ends, rounded
 * up to 16, the size of a buffer that holds it.
 *
 * The block ends at the layout's `size`, or where the furthest uniform
 * does if that is further.
 *
 * @param[in]  layout    The block's uniforms.
 * @param[out] out_size  Receives the size in bytes; 0 on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p layout or
 *         @p out_size is NULL, the layout's `uniforms` is NULL with a
 *         `count` above 0, a uniform's `type` is not an
 *         APREND_UNIFORM_TYPE, or the layout has no uniforms and a `size`
 *         of 0; APRESULT_LIMIT if the block ends past what 32 bits can
 *         address.
 */
APRESULT aprend_uniform_layout_get_size(
	const APREND_UNIFORM_LAYOUT *layout,
	uint32_t *out_size,
	APREND_ERROR *out_error);

/**
 * @brief Writes one uniform of a block, found beforehand with
 * aprend_uniform_layout_get_uniform_index(), into a buffer: the current
 * frame's copy, as aprend_buffer_write().
 *
 * Writes the uniform at the offset its layout entry gives, with no search.
 * An array's elements are written where std140 puts them, 16 bytes apart or
 * the type's size rounded up to 16 if that is larger, and a matrix's
 * columns 16 bytes apart; the padding between them is left as it was.
 *
 * @param[in]  buffer        Buffer to write: one with
 *                           #APREND_BUFFER_USAGE_UNIFORM, holding the block
 *                           from its first byte.
 * @param[in]  layout        The block's layout. Only read during the call.
 * @param[in]  uniform_index The uniform's index in @p layout.
 * @param[in]  data          The whole uniform: the type's size times its
 *                           element count, tightly packed. A `bool` is 4
 *                           bytes: pass a `uint32_t`, never a C `bool`.
 * @param[out] out_error     Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT, with nothing written, if
 *         @p buffer, @p layout or @p data is NULL, the buffer has no
 *         #APREND_BUFFER_USAGE_UNIFORM or was created
 *         APREND_BUFFER_WRITES_ONCE, the layout's `uniforms` is NULL,
 *         @p uniform_index is not below its `count`, the uniform's type is
 *         not an APREND_UNIFORM_TYPE, or the uniform doesn't fit inside the
 *         buffer; or the code for SpudGPU's result if the write can't be
 *         flushed.
 */
APRESULT aprend_buffer_write_uniform(
	aprend_buffer buffer,
	const APREND_UNIFORM_LAYOUT *layout,
	uint32_t uniform_index,
	const void *data,
	APREND_ERROR *out_error);

/** @} */

/**
 * @name Pools: many small meshes in a few large buffers
 *
 * A buffer is one allocation of the device's, and a device has a limit on
 * how many it gives out and a smallest size for each. A caller with
 * thousands of small meshes that each last a while doesn't make a buffer
 * for each: it makes a pool, which owns a few large buffers, the blocks,
 * and hands out ranges of them. A range is kept until it is freed.
 * (Geometry rebuilt every frame is what transient memory is for,
 * aprendtransient.h.)
 *
 * A range names its block's buffer and where in it the range starts. The
 * caller draws from it as from any buffer: the block goes in
 * SET_VERTEX_BUFFERS or SET_INDEX_BUFFER, and the range's start is added to
 * the draw's `_start_vertex_location` or `_start_index_location`
 * (aprendcommands.h). Draws from ranges of one block need the block bound
 * once.
 *
 * A mesh that is indexed takes a range from a vertex pool and one from an
 * index pool. Its indices count from its own first vertex, 0 for the first,
 * as they would in a buffer of its own, and the draw passes the vertex
 * range's `first_vertex` as `_base_vertex_location`. Neither pool knows of
 * the other.
 *
 * The rules:
 *
 * - A range is written once, by the allocate call, and never again. To
 *   change a mesh, free its range and allocate another. The block's buffer
 *   is the pool's: it is never passed to aprend_buffer_write() or
 *   aprend_buffer_destroy().
 * - Allocating is safe while submitted work is drawing other ranges of the
 *   same block.
 * - Freeing a range doesn't need the GPU to be finished with it. The range
 *   is handed out again only once everything submitted through the instance
 *   up to the free has finished, as a destroyed buffer is released
 *   (aprendcontext.h, "Destroying"), and with the same gap: a command list
 *   compiled against a range and submitted after its free may draw another
 *   mesh's data. Free a range after the last submission that draws it.
 * - A range never moves, and nothing is compacted. A pool whose blocks have
 *   become mostly holes is the caller's to make again.
 * - A pool grows a block at a time and gives a block back only in
 *   aprend_vertex_pool_trim() or aprend_index_pool_trim(). A range longer
 *   than a block gets a block of its own, which goes when the range is
 *   freed.
 *
 * A block is a buffer written once ("Where a buffer is kept", at the top of
 * this header): on a device with memory of its own it keeps no staging
 * copy, and each range's vertices or indices are staged in a buffer of
 * their own until the submission that copies them has run. `memory_limit`
 * does not count that staging.
 *
 * Thread: a pool is used by one caller at a time with everything else of
 * its instance (aprendcontext.h, "Threads"). That includes the free calls,
 * which are not destroy calls: unlike a destroy call, a free may not
 * overlap any other call on the instance. The pool's destroy call is a
 * destroy call, and may.
 * @{
 */

/** @brief Opaque handle to a vertex pool. */
typedef struct aprend_vertex_pool_t *aprend_vertex_pool;
/** @brief Opaque handle to an index pool. */
typedef struct aprend_index_pool_t *aprend_index_pool;

/**
 * @brief Configuration descriptor for a vertex pool.
 */
typedef struct APREND_VERTEX_POOL_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_VERTEX_POOL_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * The size of one vertex in bytes: what the pool counts its ranges in.
	 * Not 0. The vertices' layout is the pipeline's that reads them
	 * (aprendpipeline.h), with this as its binding's `stride`.
	 */
	uint32_t vertex_stride;
	/** Vertices in one block. Not 0. */
	uint32_t block_vertex_count;
	/**
	 * The most bytes the pool's blocks may hold, the blocks of ranges
	 * longer than a block included; an allocation that needs a block more
	 * fails. 0 is no limit. If not 0, at least one block's bytes.
	 */
	uint64_t memory_limit;
	/** A name for diagnostics, copied; each block is named after it. May be NULL. */
	const char *debug_name;
} APREND_VERTEX_POOL_DESC;

/**
 * @brief A range of a vertex pool.
 *
 * A value: copy it freely. Zeroed, it is no range.
 */
/* Frozen: a value, copied freely. It never changes; a new need gets a new type. */
typedef struct APREND_VERTEX_RANGE {
	/**
	 * The block the range is in, for SET_VERTEX_BUFFERS. The pool's: read by
	 * draws and by aprend_buffer_get_desc(), never written or destroyed.
	 */
	aprend_buffer buffer;
	/**
	 * Where the range starts in the block: a draw's
	 * `_start_vertex_location`, or an indexed draw's
	 * `_base_vertex_location`.
	 */
	uint32_t first_vertex;
	/** How many vertices the range holds. */
	uint32_t vertex_count;
	/**
	 * Which allocation this is. Aprend's: not for the caller to read or
	 * change.
	 */
	uint32_t _id;
	/** Aprend's, as #_id. */
	uint32_t _generation;
} APREND_VERTEX_RANGE;

/**
 * @brief Creates a vertex pool with no blocks: the first is made by the
 * first allocation.
 *
 * @param[in]  instance  Instance whose device the blocks are created on.
 * @param[in]  desc      Pool configuration. Only read during the call.
 * @param[out] out_pool  Receives the pool; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance, @p desc
 *         or @p out_pool is NULL, `vertex_stride` or `block_vertex_count`
 *         is 0, or `memory_limit` is not 0 and below one block's bytes;
 *         the struct codes for `desc->base`; or APRESULT_OUT_OF_MEMORY.
 *
 * @see aprend_vertex_pool_destroy()
 */
APRESULT aprend_vertex_pool_create(
	aprend_instance instance,
	const APREND_VERTEX_POOL_DESC *desc,
	aprend_vertex_pool *out_pool,
	APREND_ERROR *out_error);

/**
 * @brief Destroys a vertex pool and its blocks; every range of it is gone.
 *
 * The GPU doesn't have to be finished with the blocks (aprendcontext.h,
 * "Destroying"), and no later submission may draw from one.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p pool itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] pool Pool to destroy. NULL is accepted and does nothing.
 */
void aprend_vertex_pool_destroy(aprend_vertex_pool pool);

/**
 * @brief Takes a range of a vertex pool and writes vertices into it.
 *
 * @param[in]  pool         Pool to take from.
 * @param[in]  vertex_count How many vertices. Not 0.
 * @param[in]  data         @p vertex_count vertices of the pool's
 *                          `vertex_stride` each.
 * @param[out] out_range    Receives the range. Zeroed on failure.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT, with nothing taken, if
 *         @p pool, @p data or @p out_range is NULL or @p vertex_count is 0;
 *         APRESULT_LIMIT if a block is needed and `memory_limit` doesn't
 *         allow it; APRESULT_OUT_OF_MEMORY; or the code for SpudGPU's
 *         result if a block can't be created or the write staged.
 */
APRESULT aprend_vertex_pool_allocate(
	aprend_vertex_pool pool,
	uint32_t vertex_count,
	const void *data,
	APREND_VERTEX_RANGE *out_range,
	APREND_ERROR *out_error);

/**
 * @brief Gives a range of a vertex pool back.
 *
 * It is handed out again once the GPU has finished what was submitted up to
 * now.
 *
 * @param[in]  pool      Pool the range is of.
 * @param[in]  range     Range to free.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT, with nothing freed, if
 *         @p pool or @p range is NULL, or @p range is not a range of
 *         @p pool that is still allocated: a zeroed range, a range of
 *         another pool, one whose fields were changed, or a copy of a range
 *         that was freed before, whatever has been allocated in its place
 *         since. A free needs no memory.
 */
APRESULT aprend_vertex_pool_free(
	aprend_vertex_pool pool,
	const APREND_VERTEX_RANGE *range,
	APREND_ERROR *out_error);

/**
 * @brief Destroys every block of a vertex pool that holds no range.
 *
 * A block holding a freed range the GPU hasn't finished with is kept, and
 * goes at a later call. The pool never does this by itself: a block that
 * empties is kept for the next allocation.
 *
 * @param[in] pool Pool to trim. NULL is accepted and does nothing.
 */
void aprend_vertex_pool_trim(aprend_vertex_pool pool);

/**
 * @brief Reads how much of a vertex pool is in use.
 *
 * The difference between the two is what the next allocations can take
 * without a new block, if it is in long enough pieces.
 *
 * @param[in]  pool            Pool to read.
 * @param[out] out_used_bytes  Receives the bytes of every range allocated,
 *                             a freed one counted until the GPU has
 *                             finished with it. May be NULL.
 * @param[out] out_block_bytes Receives the bytes of every block. May be
 *                             NULL.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p pool is NULL.
 */
APRESULT aprend_vertex_pool_get_usage(
	aprend_vertex_pool pool,
	uint64_t *out_used_bytes,
	uint64_t *out_block_bytes);

/**
 * @brief Configuration descriptor for an index pool.
 */
typedef struct APREND_INDEX_POOL_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_INDEX_POOL_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The size of every index in the pool: UINT16 or UINT32. */
	APREND_INDEX_STRIDE stride;
	/** Indices in one block. Not 0. */
	uint32_t block_index_count;
	/** As APREND_VERTEX_POOL_DESC::memory_limit. */
	uint64_t memory_limit;
	/** A name for diagnostics, copied; each block is named after it. May be NULL. */
	const char *debug_name;
} APREND_INDEX_POOL_DESC;

/**
 * @brief A range of an index pool.
 *
 * A value: copy it freely. Zeroed, it is no range.
 */
/* Frozen: a value, copied freely. It never changes; a new need gets a new type. */
typedef struct APREND_INDEX_RANGE {
	/**
	 * The block the range is in, for SET_INDEX_BUFFER. The pool's: read by
	 * draws and by aprend_buffer_get_desc(), never written or destroyed.
	 */
	aprend_buffer buffer;
	/**
	 * Where the range starts in the block: an indexed draw's
	 * `_start_index_location`.
	 */
	uint32_t first_index;
	/** How many indices the range holds. */
	uint32_t index_count;
	/**
	 * Which allocation this is. Aprend's: not for the caller to read or
	 * change.
	 */
	uint32_t _id;
	/** Aprend's, as #_id. */
	uint32_t _generation;
} APREND_INDEX_RANGE;

/**
 * @brief Creates an index pool with no blocks, as
 * aprend_vertex_pool_create() does a vertex pool.
 *
 * @param[in]  instance  Instance whose device the blocks are created on.
 * @param[in]  desc      Pool configuration. Only read during the call.
 * @param[out] out_pool  Receives the pool; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance, @p desc
 *         or @p out_pool is NULL, `stride` is not UINT16 or UINT32,
 *         `block_index_count` is 0, or `memory_limit` is not 0 and below one
 *         block's bytes; the struct codes for `desc->base`; or
 *         APRESULT_OUT_OF_MEMORY.
 *
 * @see aprend_index_pool_destroy()
 */
APRESULT aprend_index_pool_create(
	aprend_instance instance,
	const APREND_INDEX_POOL_DESC *desc,
	aprend_index_pool *out_pool,
	APREND_ERROR *out_error);

/**
 * @brief Destroys an index pool and its blocks, as
 * aprend_vertex_pool_destroy() does a vertex pool.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p pool itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] pool Pool to destroy. NULL is accepted and does nothing.
 */
void aprend_index_pool_destroy(aprend_index_pool pool);

/**
 * @brief Takes a range of an index pool and writes indices into it, as
 * aprend_vertex_pool_allocate() does vertices.
 *
 * The indices are written as they are given: one that counts from its
 * mesh's first vertex is drawn with that vertex's place as
 * `_base_vertex_location`.
 *
 * @param[in]  pool        Pool to take from.
 * @param[in]  index_count How many indices. Not 0.
 * @param[in]  data        @p index_count indices of the pool's size.
 * @param[out] out_range   Receives the range. Zeroed on failure.
 * @param[out] out_error   Why it failed, when it did. May be NULL.
 *
 * @return As aprend_vertex_pool_allocate().
 */
APRESULT aprend_index_pool_allocate(
	aprend_index_pool pool,
	uint32_t index_count,
	const void *data,
	APREND_INDEX_RANGE *out_range,
	APREND_ERROR *out_error);

/**
 * @brief Gives a range of an index pool back, as aprend_vertex_pool_free()
 * does a vertex range.
 *
 * @param[in]  pool      Pool the range is of.
 * @param[in]  range     Range to free.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return As aprend_vertex_pool_free().
 */
APRESULT aprend_index_pool_free(
	aprend_index_pool pool,
	const APREND_INDEX_RANGE *range,
	APREND_ERROR *out_error);

/**
 * @brief Destroys every block of an index pool that holds no range, as
 * aprend_vertex_pool_trim() does.
 *
 * @param[in] pool Pool to trim. NULL is accepted and does nothing.
 */
void aprend_index_pool_trim(aprend_index_pool pool);

/**
 * @brief Reads how much of an index pool is in use, as
 * aprend_vertex_pool_get_usage() does.
 *
 * @param[in]  pool            Pool to read.
 * @param[out] out_used_bytes  Receives the bytes of every range allocated.
 *                             May be NULL.
 * @param[out] out_block_bytes Receives the bytes of every block. May be
 *                             NULL.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p pool is NULL.
 */
APRESULT aprend_index_pool_get_usage(
	aprend_index_pool pool,
	uint64_t *out_used_bytes,
	uint64_t *out_block_bytes);

/** @} */

#if __cplusplus
}
#endif

#endif // APREND_BUFFERS_H
