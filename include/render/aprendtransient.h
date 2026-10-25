
#ifndef APREND_TRANSIENT_H
#define APREND_TRANSIENT_H

#include "aprendcontext.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @file aprendtransient.h
 * @brief Transient memory: bytes the caller writes this frame and the GPU
 * reads this frame, with no buffer to create or destroy.
 *
 * It is for geometry that is built again every frame and has no size worth
 * keeping a buffer for: 2D shapes, lines, text, debug drawing. The caller
 * takes a span, writes vertices or indices straight into it, and names the
 * span in a command list (APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER and
 * APREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER, aprendcommands.h). Nothing is
 * copied: the GPU reads the bytes where they were written.
 *
 * A span lasts for the frame it was taken in. The frames are the instance's
 * (aprendcontext.h): aprend_instance_next_frame() takes back every span
 * taken the last time the frame index it moves to was current, once the GPU
 * has finished what was submitted then. So:
 *
 * - A span is written, compiled into a list and submitted before the next
 *   aprend_instance_next_frame(). Its bytes are written before the
 *   submission that reads them.
 * - A command list that names a span is compiled and submitted in the frame
 *   the span was taken in. aprend_command_list_compile() refuses a span of
 *   another frame, and aprend_command_list_submit() a list compiled in one.
 * - After that aprend_instance_next_frame(), `data` must not be written:
 *   the memory may already be another span's.
 * - A caller that never calls aprend_instance_next_frame() never gets any
 *   of it back.
 *
 * The memory is the CPU's to write and never to read: reading `data` back is
 * outside the contract and slow where it works. It is not zeroed.
 *
 * What it is not for: data kept from frame to frame (a vertex buffer or a
 * pool, aprendbuffers.h), and uniform or storage data, which a binding set
 * reads through a buffer or a buffer set.
 *
 * An instance has transient memory only if it was created with
 * aprend_instance_desc::transient_chunk_size above 0.
 *
 * Aprend takes spans of it as well: a submission copies a material's
 * changed uniform blocks through it (aprendmaterial.h). They count in
 * aprend_transient_get_frame_usage() and against
 * aprend_instance_desc::transient_memory_limit.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). aprend_transient_allocate()
 * takes the instance, so it is one of the calls only one thread makes at a
 * time. Writing into a span's `data` is not a call into Aprend: several
 * threads may fill different spans at once.
 */

/**
 * @brief Some bytes of an instance's transient memory.
 *
 * A value: copy it freely, and record it in as many commands as read it.
 * There is nothing to destroy.
 */
/* Frozen: a value, held by value in APREND_COMMAND. It never changes; a new need gets a new type. */
typedef struct aprend_transient_span {
	/**
	 * Where the caller writes its vertices or indices. #size bytes, at an
	 * address that is a multiple of the alignment asked for. Written, never
	 * read.
	 */
	void *data;
	/** The number of bytes asked for. */
	uint64_t size;
	/** Which memory this is. Aprend's: not for the caller to read or change. */
	void *_chunk;
	/** Aprend's, as #_chunk. */
	uint64_t _offset;
	/** Aprend's, as #_chunk. */
	uint64_t _epoch;
	/** Aprend's, as #_chunk. */
	uint32_t _frame;
} aprend_transient_span;

/**
 * @brief Takes @p size bytes of transient memory for the current frame.
 *
 * @param[in]  instance  Instance whose transient memory is taken from.
 * @param[in]  size      Number of bytes. Not 0.
 * @param[in]  alignment What the span's offset is a multiple of: a power of
 *                       two, not 0. For an index span, at least the size of
 *                       one index. For a vertex span, 4 suits every format.
 * @param[out] out_span  Receives the span. Zeroed on failure.
 *
 * @return false if @p instance or @p out_span is NULL, @p size is 0,
 *         @p alignment is 0 or not a power of two, the instance was created
 *         with no transient memory, the device refuses more memory, or
 *         aprend_instance_desc::transient_memory_limit would be passed.
 */
bool aprend_transient_allocate(
    aprend_instance instance,
    uint64_t size,
    uint64_t alignment,
    aprend_transient_span *out_span);

/**
 * @brief Bytes of transient memory taken in the current frame so far,
 * padding for alignment included.
 *
 * For sizing aprend_instance_desc::transient_chunk_size.
 *
 * @param[in] instance Instance to read.
 *
 * @return The bytes taken, or 0 if @p instance is NULL.
 */
uint64_t aprend_transient_get_frame_usage(aprend_instance instance);

#if __cplusplus
}
#endif // __cplusplus

#endif // APREND_TRANSIENT_H
