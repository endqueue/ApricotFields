
#ifndef APREND_BUFFERS_H
#define APREND_BUFFERS_H

#include "aprendcontext.h"
#include "stdint.h"

/**
 * @file aprendbuffers.h
 * @brief Uniform, vertex, index and storage buffers over SpudGPU buffers,
 * buffer sets for frames in flight, and vertex and index pools.
 *
 * Higher-level buffer management on top of SpudGPU buffers. Provides
 * dynamic resizing, memory hints, and uniform buffer field layout
 * management.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A buffer is used by one
 * thread at a time with everything else of its instance. An update call and
 * a submission of a list that uses the buffer are never made from two
 * threads at once: the update writes what the submission reads. A buffer
 * set's update calls also read the instance's frame index. The size and
 * layout functions that take no buffer (aprend_uniform_type_get_size() and
 * aprend_uniform_layout_get_uniform_index()) are safe from any thread.
 *
 * @par Where a buffer is kept
 * Where a vertex, index or storage buffer is kept follows the device's
 * memory (SPUDGPU_ADAPTER_PROPERTIES::unified_memory of its adapter, and
 * SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY if the device was created with it,
 * read when the instance is created), and the
 * calls behave the same either way.
 *
 * On a device whose memory is the system's, the buffer is mapped and a
 * write goes straight into it; a storage buffer needs the device to have
 * SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE for that. On a device with memory of
 * its own, or for a storage buffer without that kind, the buffer is in the
 * device's memory and a write goes into a staging copy; the next
 * aprend_command_list_submit() of a list that uses the buffer copies the
 * bytes written since the last such submission across, ahead of the list's
 * own commands. Only those bytes are copied: two writes far apart are two
 * copies, not one over everything between them.
 *
 * What the staging copy is depends on #APREND_BUFFER_WRITES, given at
 * creation. A buffer written many times keeps a staging buffer of its own
 * size for life. A buffer written once, at creation, has the bytes it was
 * created with staged in a buffer of their own, which is released once the
 * submission that copies them has run; one created with no bytes has no
 * staging at all. Either way the buffer itself is the same.
 *
 * The rule for the caller is one rule: a write (at creation or by an update
 * call) is seen by any submission made after it, and must not be made while
 * submitted work that uses the buffer has yet to finish. A buffer written
 * and never used by a submitted list is never copied, and keeps its staging
 * copy until it is.
 *
 * A uniform buffer is mapped and written directly on every device. The same
 * rule holds, and more sharply: a write lands in the memory the GPU reads,
 * so a uniform buffer written while a submitted frame still reads it
 * changes what that frame sees. A block rewritten every frame belongs in a
 * uniform buffer set ("Buffer sets", below), whose copies the frames in
 * flight never share.
 */

#if __cplusplus
extern "C" {
#endif

/*
 * Rewrite this entire thing
 * Make something like :
 * aprend_static_mesh_buffer_t*
 * aprend_static_mesh_set_data()
 * aprend_dynamic_mesh_buffer_t*
 * aprend_dynamic_mesh_set_vertex_range(data_range, vertices)
 * aprend_dynamic_mesh_set_index_range(data_range, indices)
 */

/**
 * @brief A vertex of position, normal, texture coordinate and color, for a
 * caller that has no layout of its own.
 */
/* Frozen: a vertex in GPU memory, laid out as the shaders that read it. It never changes; a new need gets a new type. */
typedef struct APREND_DEFAULT_VERTEX {
	/** Position. */
	float position[3];
	/** Normal. */
	float normal[3];
	/** Texture coordinate. */
	float uv[2];
	/** Color, with alpha. */
	float color[4];
} APREND_DEFAULT_VERTEX;

/** @brief Opaque handle to a vertex buffer. */
typedef struct aprend_vertex_buffer_t *aprend_vertex_buffer;
/** @brief Opaque handle to an index buffer. */
typedef struct aprend_index_buffer_t *aprend_index_buffer;
/** @brief Opaque handle to a uniform buffer. */
typedef struct aprend_uniform_buffer_t *aprend_uniform_buffer;
/** @brief Opaque handle to a storage buffer. */
typedef struct aprend_storage_buffer_t *aprend_storage_buffer;
/** @brief Opaque handle to a set of uniform buffers, one for each frame in flight. */
typedef struct aprend_uniform_buffer_set_t *aprend_uniform_buffer_set;
/** @brief Opaque handle to a set of storage buffers, one for each frame in flight. */
typedef struct aprend_storage_buffer_set_t *aprend_storage_buffer_set;

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
 *         `mat3`'s is 36), which is what the update calls take; a block
 *         keeps each column 16 bytes apart.
 */
uint32_t aprend_uniform_type_get_size(APREND_UNIFORM_TYPE type);

/**
 * @brief One uniform of a uniform block.
 */
/* Frozen: an array element of aprend_uniform_layout. It never changes; a new need gets a new type. */
typedef struct aprend_uniform {
	/**
	 * The uniform's name, for aprend_uniform_layout_get_uniform_index() and
	 * aprend_uniform_buffer_update_by_name(). May be NULL.
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
} aprend_uniform;

/**
 * @brief The uniforms of one uniform block.
 *
 * Written by hand, or read from a shader:
 * aprend_binding_layout_get_uniform_layout() (aprendpipeline.h) gives the
 * layout of a block of a layout made from shaders, with every offset and the
 * block's size as the SPIR-V has them, and its structs and arrays of structs
 * listed member by member under names such as `lights[2].color`.
 */
/* Frozen: a block's layout as a shader declares it, written by hand or read back. It never changes; a new need gets a new type. */
typedef struct aprend_uniform_layout {
	/** How many uniforms. */
	uint32_t count;
	/** The uniforms, #count of them. */
	aprend_uniform *uniforms;
	/**
	 * The block's size in bytes as the shader declares it, or 0 to take it
	 * from where the furthest uniform ends. A block with members that have no
	 * APREND_UNIFORM_TYPE (a double, a non-square matrix) has them left out
	 * of #uniforms, and needs this to be sized right.
	 */
	uint32_t size;
} aprend_uniform_layout;

/**
 * @brief Finds a uniform of a layout by name, once: the index
 * aprend_uniform_buffer_update_uniform() takes.
 *
 * The index is the uniform's place in @p layout's `uniforms`, so it serves
 * every buffer and buffer set made from that layout, and a caller that wrote
 * the layout knows it already. A uniform written every frame is found once,
 * when its layout is made, rather than by name at every write.
 *
 * @param[in] layout Layout to search.
 * @param[in] name   The uniform's name, case-sensitive.
 *
 * @return Index of the first uniform of @p layout named @p name, or
 *         UINT32_MAX if there is none, or if @p layout, its uniforms or
 *         @p name is NULL.
 *
 * @see aprend_vertex_layout_get_element_index(), the same for a vertex
 *      layout.
 */
uint32_t aprend_uniform_layout_get_uniform_index(
    const aprend_uniform_layout *layout,
    const char *name);

/**
 * @brief Creates a buffer for one std140 uniform block.
 *
 * Nothing is placed: each uniform's `offset` is the caller's and must be
 * where the shader's block has it, which a layout from
 * aprend_binding_layout_get_uniform_layout() always is. The buffer ends at
 * the layout's `size`, or where the furthest uniform does if that is
 * further, rounded up to 16. std140 is assumed for what takes room: an array
 * (an `array_count` above 1) takes 16 bytes an element, or the type's size
 * rounded up to 16 if that is larger, and a matrix takes 16 bytes a column. The
 * block starts as zeroes: a uniform reads 0 until it is written.
 *
 * @param[in] instance Instance whose device the buffer is created on.
 * @param[in] layout   The block's uniforms. Copied, names included: it
 *                     doesn't have to outlive the call.
 *
 * @return The buffer, or NULL if @p instance or @p layout is NULL, the
 *         layout's `uniforms` is NULL with a `count` above 0, a uniform's
 *         `type` is not an APREND_UNIFORM_TYPE, the layout has no uniforms
 *         and a `size` of 0, the block ends past what 32 bits can address,
 *         there is no memory, or the device refuses or can't map the
 *         buffer. Each is printed.
 *
 * @see aprend_uniform_buffer_destroy()
 */
aprend_uniform_buffer aprend_uniform_buffer_create(
    aprend_instance instance,
    const aprend_uniform_layout *layout);

/**
 * @brief Destroys a uniform buffer.
 *
 * @warning Every binding set holding @p buffer goes first, and no later
 * submission may read it. A submission already made is safe
 * (aprendcontext.h, "Destroying").
 *
 * @param[in] buffer Buffer to destroy. NULL is accepted and does nothing.
 */
void aprend_uniform_buffer_destroy(aprend_uniform_buffer buffer);

/**
 * @brief Reads a uniform buffer's layout: the buffer's own copy of the one
 * it was created with, for aprend_uniform_layout_get_uniform_index().
 *
 * Nothing is written if @p buffer or @p out_layout is NULL.
 *
 * @param[in]  buffer     Buffer to read.
 * @param[out] out_layout Receives the layout. Its `uniforms` and their
 *                        names are the buffer's, valid as long as it, and
 *                        not to be written.
 */
void aprend_uniform_buffer_get_layout(
    aprend_uniform_buffer buffer,
    aprend_uniform_layout *out_layout);

/**
 * @brief Gets a uniform buffer's size: its block's, rounded up to 16.
 *
 * @param[in] buffer Buffer to read.
 *
 * @return The size in bytes, or 0 if @p buffer is NULL.
 */
uint32_t aprend_uniform_buffer_get_size(aprend_uniform_buffer buffer);

/**
 * @brief Writes bytes into a uniform buffer's block.
 *
 * Into the memory the GPU reads: not while a submission that reads the
 * buffer is unfinished ("Where a buffer is kept", at the top of this
 * header). The same holds for every uniform buffer update call.
 *
 * @param[in] buffer       Buffer to write.
 * @param[in] local_offset Where in the block the bytes go.
 * @param[in] size         How many bytes. Not 0.
 * @param[in] pData        The bytes.
 *
 * @return false, with nothing written, if @p buffer or @p pData is NULL,
 *         @p size is 0, or the bytes run past the end of the block. false
 *         too if the write can't be flushed to the device.
 */
bool aprend_uniform_buffer_update(
    aprend_uniform_buffer buffer,
    uint32_t local_offset,
    uint32_t size,
    const void *pData);

/**
 * @brief Writes one uniform of a uniform buffer, found beforehand with
 * aprend_uniform_layout_get_uniform_index().
 *
 * Writes the uniform at the offset its layout entry gives, with no search.
 * An array's elements are written where std140 puts them, 16 bytes apart or
 * the type's size rounded up to 16 if that is larger, and a matrix's
 * columns 16 bytes apart.
 *
 * @param[in] buffer        Buffer to write.
 * @param[in] uniform_index The uniform's index in the layout the buffer was
 *                          created with.
 * @param[in] pData         The whole uniform: the type's size times its
 *                          element count, tightly packed. A `bool` is 4
 *                          bytes: pass a `uint32_t`, never a C `bool`.
 *
 * @return false, with nothing written, if @p buffer or @p pData is NULL,
 *         @p uniform_index is not below the layout's `count`, the uniform's
 *         type is not an APREND_UNIFORM_TYPE, or it doesn't fit inside the
 *         buffer. false too if the write can't be flushed to the device.
 */
bool aprend_uniform_buffer_update_uniform(
    aprend_uniform_buffer buffer,
    uint32_t uniform_index,
    const void *pData);

/**
 * @brief Writes one uniform of a uniform buffer, by name.
 *
 * aprend_uniform_layout_get_uniform_index() on the buffer's copy of its
 * layout (case-sensitive; the first uniform of that name), then
 * aprend_uniform_buffer_update_uniform(). Names are copied at
 * aprend_uniform_buffer_create(), so the layout passed there doesn't have to
 * outlive the buffer. For a uniform written every frame, find its index
 * once and use aprend_uniform_buffer_update_uniform().
 *
 * @param[in] buffer Buffer to write.
 * @param[in] name   The uniform's name.
 * @param[in] pData  The whole uniform, as
 *                   aprend_uniform_buffer_update_uniform() takes it.
 *
 * @return false, with nothing written, if an argument is NULL, no uniform
 *         has that name, or aprend_uniform_buffer_update_uniform() would
 *         return false.
 */
bool aprend_uniform_buffer_update_by_name(
    aprend_uniform_buffer buffer,
    const char *name,
    const void *pData);

/**
 * @brief How often the CPU writes a vertex, index or storage buffer, given
 * when it is created.
 *
 * It decides what a write is staged in on a device with memory of its own
 * ("Where a buffer is kept", at the top of this header); on a device whose
 * memory is the system's the buffer is written directly either way. A GPU
 * that writes the buffer (a storage buffer written by a shader) is not
 * counted: only the CPU's writes are.
 */
typedef uint32_t APREND_BUFFER_WRITES;
enum {
	/**
	 * Written at creation, with the bytes the create call is given, and
	 * never again: the buffer's update call refuses it. The staging copy of
	 * those bytes is released once the submission that copies them has
	 * run, so a buffer of meshes loaded once costs its own size and no more.
	 * Created with no bytes, the buffer is never staged: right for one only
	 * a shader writes.
	 */
	APREND_BUFFER_WRITES_ONCE = 1,
	/**
	 * Written at creation and by the update call, as often as needed. The
	 * buffer keeps a staging buffer of its own size for life.
	 */
	APREND_BUFFER_WRITES_MANY = 2
};

/**
 * @brief Creates a vertex buffer: bytes a pipeline reads its vertices or
 * instances from.
 *
 * The buffer holds no layout and no stride. What its bytes are is the
 * pipeline's to say (aprend_vertex_binding_desc, aprendpipeline.h), so one
 * buffer can be read by pipelines that read different parts of the same
 * vertices: a depth pass that reads only positions, say. Where it is kept
 * and when a write is seen are under "Where a buffer is kept" at the top of
 * this header.
 *
 * @param[in] instance Instance whose device the buffer is created on.
 * @param[in] size     The buffer's size in bytes. Not 0.
 * @param[in] writes   How often the CPU writes the buffer: ONCE or MANY.
 *                     There is no default.
 * @param[in] pData    @p size bytes to copy in, or NULL to leave the buffer
 *                     unwritten.
 *
 * @return The buffer, or NULL if @p instance is NULL, @p size is 0,
 *         @p writes is not ONCE or MANY, or the device refuses the buffer.
 *         Each is printed.
 *
 * @see aprend_vertex_buffer_destroy()
 */
aprend_vertex_buffer aprend_vertex_buffer_create(
    aprend_instance instance,
    uint64_t size,
    APREND_BUFFER_WRITES writes,
    const void *pData);

/**
 * @brief Destroys a vertex buffer.
 *
 * The GPU doesn't have to be finished with it (aprendcontext.h,
 * "Destroying"). A command list compiled with it and not yet submitted is
 * refused at submit (aprendcommands.h).
 *
 * @param[in] buffer Buffer to destroy. NULL is accepted and does nothing.
 */
void aprend_vertex_buffer_destroy(aprend_vertex_buffer buffer);

/**
 * @brief Gets a vertex buffer's size.
 *
 * @param[in] buffer Buffer to read.
 *
 * @return The size in bytes, or 0 if @p buffer is NULL.
 */
uint64_t aprend_vertex_buffer_get_size(aprend_vertex_buffer buffer);

/**
 * @brief Writes bytes into a vertex buffer.
 *
 * Not while a submission that reads the buffer is unfinished ("Where a
 * buffer is kept", at the top of this header).
 *
 * @param[in] buffer Buffer to write.
 * @param[in] offset Where in the buffer the bytes go.
 * @param[in] size   How many bytes. Not 0.
 * @param[in] pData  The bytes.
 *
 * @return false, with nothing written, if @p buffer or @p pData is NULL,
 *         @p size is 0, @p offset + @p size runs past the end of the
 *         buffer, the buffer was created APREND_BUFFER_WRITES_ONCE, or there
 *         is no memory to note the write. Each is printed.
 */
bool aprend_vertex_buffer_update(
    aprend_vertex_buffer buffer,
    uint64_t offset,
    uint64_t size,
    const void *pData);

/**
 * @brief The size of one index of an index buffer, in bytes.
 */
typedef enum APREND_INDEX_STRIDE APRI_ENUM_BASE { APREND_INDEX_STRIDE_NONE = 0, APREND_INDEX_STRIDE_UINT16 = 2, APREND_INDEX_STRIDE_UINT32 = 4 } APREND_INDEX_STRIDE;

/**
 * @brief Creates an index buffer.
 *
 * Where it is kept and when a write is seen are as for a vertex buffer.
 *
 * @param[in] instance    Instance whose device the buffer is created on.
 * @param[in] stride      The size of one index: UINT16 or UINT32.
 * @param[in] index_count How many indices the buffer holds. Not 0.
 * @param[in] writes      How often the CPU writes the buffer: ONCE or MANY.
 * @param[in] pData       @p index_count indices to copy in, or NULL to
 *                        leave the buffer unwritten.
 *
 * @return The buffer, or NULL if @p instance is NULL, @p stride is not
 *         UINT16 or UINT32, @p index_count is 0, @p writes is not ONCE or
 *         MANY, or the device refuses the buffer.
 *
 * @see aprend_index_buffer_destroy()
 */
aprend_index_buffer aprend_index_buffer_create(
    aprend_instance instance,
    APREND_INDEX_STRIDE stride,
    uint32_t index_count,
    APREND_BUFFER_WRITES writes,
    const void *pData);

/**
 * @brief Destroys an index buffer.
 *
 * The GPU doesn't have to be finished with it (aprendcontext.h,
 * "Destroying"). No later submission may use it.
 *
 * @param[in] buffer Buffer to destroy. NULL is accepted and does nothing.
 */
void aprend_index_buffer_destroy(aprend_index_buffer buffer);

/**
 * @brief Gets the SpudGPU buffer view of a whole index buffer.
 *
 * @param[in] buffer Buffer to read.
 *
 * @return The view, or NULL if @p buffer is NULL.
 */
spudgpu_buffer_view aprend_index_buffer_get_spudgpu_buffer_view(aprend_index_buffer buffer);

/**
 * @brief Writes indices into an index buffer.
 *
 * @param[in] buffer       Buffer to write.
 * @param[in] index_offset The first index written.
 * @param[in] index_count  How many indices. Not 0.
 * @param[in] pData        The indices.
 *
 * @return false, with nothing written, if @p buffer or @p pData is NULL,
 *         @p index_count is 0, the range runs past the end of the buffer,
 *         the buffer was created APREND_BUFFER_WRITES_ONCE, or there is no
 *         memory to note the write.
 */
bool aprend_index_buffer_update(
    aprend_index_buffer buffer,
    uint32_t index_offset,
    uint32_t index_count,
    const void *pData);

/**
 * @brief Gets the size of one index of an index buffer.
 *
 * @param[in] buffer Buffer to read.
 *
 * @return The index size, or APREND_INDEX_STRIDE_NONE if @p buffer is NULL.
 */
APREND_INDEX_STRIDE aprend_index_buffer_get_stride(aprend_index_buffer buffer);

/**
 * @brief Gets how many indices an index buffer holds.
 *
 * @param[in] buffer Buffer to read.
 *
 * @return The index count, or 0 if @p buffer is NULL.
 */
uint32_t aprend_index_buffer_get_index_count(aprend_index_buffer buffer);

/**
 * @brief What a storage buffer is used for beyond a storage slot of a
 * binding set, which every storage buffer can be put in.
 */
typedef uint32_t APREND_STORAGE_BUFFER_USAGE;
enum {
	/** A storage slot of a binding set and nothing more. */
	APREND_STORAGE_BUFFER_USAGE_NONE = 0,
	/**
	 * The buffer can also be the argument buffer of
	 * APREND_COMMAND_DRAW_INDIRECT and APREND_COMMAND_DRAW_INDEXED_INDIRECT
	 * (aprendcommands.h): the same buffer a compute shader fills through a
	 * storage slot is then read by the draws, with no trip through the CPU.
	 * It can equally be filled from the CPU with
	 * aprend_storage_buffer_update(). A command list moves such a buffer
	 * between the two uses itself.
	 */
	APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS = 1
};

/**
 * @brief Creates a storage buffer.
 *
 * Where it is kept and when a write is seen are as for a vertex buffer.
 *
 * @param[in] instance Instance whose device the buffer is created on.
 * @param[in] size     The buffer's size in bytes. Not 0.
 * @param[in] usage    What the buffer is used for beyond a storage slot.
 * @param[in] writes   How often the CPU writes the buffer: ONCE or MANY.
 *                     A buffer only a shader writes is ONCE, with no
 *                     @p pData.
 * @param[in] pData    @p size bytes to copy in, or NULL to leave the buffer
 *                     unwritten.
 *
 * @return The buffer, or NULL if @p instance is NULL, @p size is 0,
 *         @p usage has a bit that is not an APREND_STORAGE_BUFFER_USAGE,
 *         @p writes is not ONCE or MANY, or the device refuses the buffer.
 *
 * @see aprend_storage_buffer_destroy()
 */
aprend_storage_buffer aprend_storage_buffer_create(
    aprend_instance instance,
    uint64_t size,
    APREND_STORAGE_BUFFER_USAGE usage,
    APREND_BUFFER_WRITES writes,
    const void *pData);

/**
 * @brief Gets the usage a storage buffer was created with.
 *
 * @param[in] buffer Buffer to read.
 *
 * @return The usage, or APREND_STORAGE_BUFFER_USAGE_NONE if @p buffer is
 *         NULL.
 */
APREND_STORAGE_BUFFER_USAGE aprend_storage_buffer_get_usage(aprend_storage_buffer buffer);

/**
 * @brief Gets a storage buffer's size.
 *
 * @param[in] buffer Buffer to read.
 *
 * @return The size in bytes, or 0 if @p buffer is NULL.
 */
uint64_t aprend_storage_buffer_get_size(aprend_storage_buffer buffer);

/**
 * @brief Destroys a storage buffer.
 *
 * @warning Every binding set holding @p buffer goes first, and no later
 * submission may use it. A submission already made is safe
 * (aprendcontext.h, "Destroying").
 *
 * @param[in] buffer Buffer to destroy. NULL is accepted and does nothing.
 */
void aprend_storage_buffer_destroy(aprend_storage_buffer buffer);

/**
 * @brief Writes bytes into a storage buffer.
 *
 * @param[in] buffer       Buffer to write.
 * @param[in] local_offset Where in the buffer the bytes go.
 * @param[in] size         How many bytes. Not 0.
 * @param[in] pData        The bytes.
 *
 * @return false, with nothing written, if @p buffer or @p pData is NULL,
 *         @p size is 0, @p local_offset + @p size runs past the end of the
 *         buffer, the buffer was created APREND_BUFFER_WRITES_ONCE, or there
 *         is no memory to note the write.
 */
bool aprend_storage_buffer_update(
    aprend_storage_buffer buffer,
    uint64_t local_offset,
    uint64_t size,
    const void *pData);

/**
 * @name Buffer sets: one buffer for each frame in flight
 *
 * A single uniform or storage buffer that is rewritten every frame can only
 * be written once the GPU has finished the last frame that reads it. A set
 * holds aprend_instance_desc::frames_in_flight copies instead. The update
 * calls write the copy of the current frame (aprend_instance_next_frame(),
 * aprendcontext.h), and a binding set that holds the buffer set makes a
 * command list read the copy of the frame it is compiled in. So the frame
 * being written and the frames still on the GPU never share a copy.
 *
 * Each copy keeps what was last written to it, which was `frames_in_flight`
 * frames ago, not last frame: an update is not carried from one copy to the
 * next. Write, each frame, everything that frame reads.
 *
 * A set is put in a binding set through the `_set` member of an entry's
 * `_uniform` or `_storage` (aprendpipeline.h). With `frames_in_flight` 1 a
 * set is one buffer and behaves as a single buffer does.
 * @{
 */

/**
 * @brief Creates a set of uniform buffers of one layout, as
 * aprend_uniform_buffer_create() makes each.
 *
 * @param[in] instance Instance whose device the buffers are created on.
 * @param[in] layout   The block's uniforms. Copied.
 *
 * @return The set, or NULL if @p instance or @p layout is NULL or a buffer
 *         can't be created.
 *
 * @see aprend_uniform_buffer_set_destroy()
 */
aprend_uniform_buffer_set aprend_uniform_buffer_set_create(
    aprend_instance instance,
    const aprend_uniform_layout *layout);

/**
 * @brief Destroys a uniform buffer set and its buffers.
 *
 * @warning Every binding set holding @p set goes first, and no later
 * submission may read it; one already made is safe (aprendcontext.h,
 * "Destroying").
 *
 * @param[in] set Set to destroy. NULL is accepted and does nothing.
 */
void aprend_uniform_buffer_set_destroy(aprend_uniform_buffer_set set);

/**
 * @brief Gets one frame's copy of a uniform buffer set.
 *
 * @param[in] set         Set to read.
 * @param[in] frame_index Which frame's copy.
 *
 * @return The copy, owned by the set: not to be destroyed. NULL if @p set
 *         is NULL or @p frame_index is not below `frames_in_flight`.
 */
aprend_uniform_buffer aprend_uniform_buffer_set_get_buffer(
    aprend_uniform_buffer_set set,
    uint32_t frame_index);

/**
 * @brief aprend_uniform_buffer_update() on the current frame's copy.
 *
 * @param[in] set          Set to write.
 * @param[in] local_offset Where in the block the bytes go.
 * @param[in] size         How many bytes. Not 0.
 * @param[in] pData        The bytes.
 *
 * @return false if @p set is NULL, or as aprend_uniform_buffer_update().
 */
bool aprend_uniform_buffer_set_update(
    aprend_uniform_buffer_set set,
    uint32_t local_offset,
    uint32_t size,
    const void *pData);

/**
 * @brief aprend_uniform_buffer_update_by_name() on the current frame's
 * copy.
 *
 * @param[in] set   Set to write.
 * @param[in] name  The uniform's name.
 * @param[in] pData The whole uniform.
 *
 * @return false if @p set is NULL, or as
 *         aprend_uniform_buffer_update_by_name().
 */
bool aprend_uniform_buffer_set_update_by_name(
    aprend_uniform_buffer_set set,
    const char *name,
    const void *pData);

/**
 * @brief aprend_uniform_buffer_update_uniform() on the current frame's copy.
 *
 * Every copy of a set is made from the one layout, so one index serves them
 * all.
 *
 * @param[in] set           Set to write.
 * @param[in] uniform_index The uniform's index in the set's layout.
 * @param[in] pData         The whole uniform.
 *
 * @return false if @p set is NULL, or as
 *         aprend_uniform_buffer_update_uniform().
 */
bool aprend_uniform_buffer_set_update_uniform(
    aprend_uniform_buffer_set set,
    uint32_t uniform_index,
    const void *pData);

/**
 * @brief Creates a set of storage buffers of one size, as
 * aprend_storage_buffer_create() makes each.
 *
 * @param[in] instance Instance whose device the buffers are created on.
 * @param[in] size     Each copy's size in bytes. Not 0.
 * @param[in] usage    Every copy's usage.
 * @param[in] writes   How often the CPU writes each copy. A set the CPU
 *                     rewrites each frame is MANY; one only a shader writes
 *                     is ONCE, with no @p pData.
 * @param[in] pData    @p size bytes every copy starts with, or NULL to
 *                     leave them unwritten.
 *
 * @return The set, or NULL if @p instance is NULL, @p size is 0, or a
 *         buffer can't be created.
 *
 * @see aprend_storage_buffer_set_destroy()
 */
aprend_storage_buffer_set aprend_storage_buffer_set_create(
    aprend_instance instance,
    uint64_t size,
    APREND_STORAGE_BUFFER_USAGE usage,
    APREND_BUFFER_WRITES writes,
    const void *pData);

/**
 * @brief Destroys a storage buffer set and its buffers, as
 * aprend_uniform_buffer_set_destroy() does a uniform buffer set.
 *
 * @param[in] set Set to destroy. NULL is accepted and does nothing.
 */
void aprend_storage_buffer_set_destroy(aprend_storage_buffer_set set);

/**
 * @brief Gets one frame's copy of a storage buffer set, as
 * aprend_uniform_buffer_set_get_buffer() does.
 *
 * @param[in] set         Set to read.
 * @param[in] frame_index Which frame's copy.
 *
 * @return The copy, owned by the set: not to be destroyed. NULL if @p set
 *         is NULL or @p frame_index is not below `frames_in_flight`.
 */
aprend_storage_buffer aprend_storage_buffer_set_get_buffer(
    aprend_storage_buffer_set set,
    uint32_t frame_index);

/**
 * @brief aprend_storage_buffer_update() on the current frame's copy.
 *
 * @param[in] set          Set to write.
 * @param[in] local_offset Where in the buffer the bytes go.
 * @param[in] size         How many bytes. Not 0.
 * @param[in] pData        The bytes.
 *
 * @return false if @p set is NULL, or as aprend_storage_buffer_update().
 */
bool aprend_storage_buffer_set_update(
    aprend_storage_buffer_set set,
    uint64_t local_offset,
    uint64_t size,
    const void *pData);

/** @} */

/**
 * @name Pools: many small meshes in a few large buffers
 *
 * A vertex or index buffer is one allocation of the device's, and a device
 * has a limit on how many it gives out and a smallest size for each. A
 * caller with thousands of small meshes that each last a while doesn't make
 * a buffer for each: it makes a pool, which owns a few large buffers, the
 * blocks, and hands out ranges of them. A range is kept until it is freed.
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
 *   is the pool's: it is never passed to an update or destroy call.
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
 * Where a block is kept and how a write reaches the device are as for any
 * vertex or index buffer ("Where a buffer is kept", at the top of this
 * header), for a buffer written once: on a device with memory of its own a
 * block keeps no staging copy, and each range's vertices or indices are
 * staged in a buffer of their own until the submission that copies them
 * has run. `memory_limit` does not count that staging. The block buffers a
 * range names are written by the pool alone; their update calls refuse.
 *
 * Threads: a pool is used by one thread at a time with everything else of
 * its instance (aprendcontext.h, "Threads"). That includes the free calls,
 * which are not destroy calls: they are not safe from another thread.
 * @{
 */

/** @brief Opaque handle to a vertex pool. */
typedef struct aprend_vertex_pool_t *aprend_vertex_pool;
/** @brief Opaque handle to an index pool. */
typedef struct aprend_index_pool_t *aprend_index_pool;

/**
 * @brief Configuration descriptor for a vertex pool.
 */
typedef struct aprend_vertex_pool_desc {
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
} aprend_vertex_pool_desc;

/**
 * @brief A range of a vertex pool.
 *
 * A value: copy it freely. Zeroed, it is no range.
 */
/* Frozen: a value, copied freely. It never changes; a new need gets a new type. */
typedef struct aprend_vertex_range {
	/**
	 * The block the range is in, for SET_VERTEX_BUFFERS. The pool's: read by
	 * draws and by the get_ calls, never updated or destroyed.
	 */
	aprend_vertex_buffer buffer;
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
} aprend_vertex_range;

/**
 * @brief Creates a vertex pool with no blocks: the first is made by the
 * first allocation.
 *
 * @param[in] instance Instance whose device the blocks are created on.
 * @param[in] desc     Pool configuration. Copied: only read during the
 *                     call.
 *
 * @return The pool, or NULL if @p instance or @p desc is NULL,
 *         `vertex_stride` or `block_vertex_count` is 0, or `memory_limit` is
 *         not 0 and below one block's bytes. Each is printed.
 *
 * @see aprend_vertex_pool_destroy()
 */
aprend_vertex_pool aprend_vertex_pool_create(
    aprend_instance instance,
    const aprend_vertex_pool_desc *desc);

/**
 * @brief Destroys a vertex pool and its blocks; every range of it is gone.
 *
 * The GPU doesn't have to be finished with the blocks (aprendcontext.h,
 * "Destroying"), and no later submission may draw from one.
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
 *
 * @return false, with nothing taken, if @p pool, @p data or @p out_range is
 *         NULL, @p vertex_count is 0, a block is needed and `memory_limit`
 *         doesn't allow it or the device refuses it, the write fails, or
 *         memory runs out.
 */
bool aprend_vertex_pool_allocate(
    aprend_vertex_pool pool,
    uint32_t vertex_count,
    const void *data,
    aprend_vertex_range *out_range);

/**
 * @brief Gives a range of a vertex pool back.
 *
 * It is handed out again once the GPU has finished what was submitted up to
 * now. Nothing happens, and it is printed, if @p range is not a range of
 * @p pool that is still allocated: a zeroed range, a range of another pool,
 * one whose fields were changed, or a copy of a range that was freed
 * before, whatever has been allocated in its place since.
 *
 * @param[in] pool  Pool the range is of. NULL is accepted and does nothing.
 * @param[in] range Range to free. NULL is accepted and does nothing.
 */
void aprend_vertex_pool_free(
    aprend_vertex_pool pool,
    const aprend_vertex_range *range);

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
 *                             finished with it. May be NULL. 0 if @p pool
 *                             is NULL.
 * @param[out] out_block_bytes Receives the bytes of every block. May be
 *                             NULL. 0 if @p pool is NULL.
 */
void aprend_vertex_pool_get_usage(
    aprend_vertex_pool pool,
    uint64_t *out_used_bytes,
    uint64_t *out_block_bytes);

/**
 * @brief Configuration descriptor for an index pool.
 */
typedef struct aprend_index_pool_desc {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_INDEX_POOL_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The size of every index in the pool: UINT16 or UINT32. */
	APREND_INDEX_STRIDE stride;
	/** Indices in one block. Not 0. */
	uint32_t block_index_count;
	/** As aprend_vertex_pool_desc::memory_limit. */
	uint64_t memory_limit;
} aprend_index_pool_desc;

/**
 * @brief A range of an index pool.
 *
 * A value: copy it freely. Zeroed, it is no range.
 */
/* Frozen: a value, copied freely. It never changes; a new need gets a new type. */
typedef struct aprend_index_range {
	/**
	 * The block the range is in, for SET_INDEX_BUFFER. The pool's: read by
	 * draws and by the get_ calls, never updated or destroyed.
	 */
	aprend_index_buffer buffer;
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
} aprend_index_range;

/**
 * @brief Creates an index pool with no blocks, as
 * aprend_vertex_pool_create() does a vertex pool.
 *
 * @param[in] instance Instance whose device the blocks are created on.
 * @param[in] desc     Pool configuration. Only read during the call.
 *
 * @return The pool, or NULL if @p instance or @p desc is NULL, `stride` is
 *         not UINT16 or UINT32, `block_index_count` is 0, or `memory_limit`
 *         is not 0 and below one block's bytes.
 *
 * @see aprend_index_pool_destroy()
 */
aprend_index_pool aprend_index_pool_create(
    aprend_instance instance,
    const aprend_index_pool_desc *desc);

/**
 * @brief Destroys an index pool and its blocks, as
 * aprend_vertex_pool_destroy() does a vertex pool.
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
 *
 * @return false, with nothing taken, under the conditions of
 *         aprend_vertex_pool_allocate().
 */
bool aprend_index_pool_allocate(
    aprend_index_pool pool,
    uint32_t index_count,
    const void *data,
    aprend_index_range *out_range);

/**
 * @brief Gives a range of an index pool back, as aprend_vertex_pool_free()
 * does a vertex range.
 *
 * @param[in] pool  Pool the range is of. NULL is accepted and does nothing.
 * @param[in] range Range to free. NULL is accepted and does nothing.
 */
void aprend_index_pool_free(
    aprend_index_pool pool,
    const aprend_index_range *range);

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
 */
void aprend_index_pool_get_usage(
    aprend_index_pool pool,
    uint64_t *out_used_bytes,
    uint64_t *out_block_bytes);

/** @} */

#if __cplusplus
}
#endif

#endif // APREND_BUFFERS_H
