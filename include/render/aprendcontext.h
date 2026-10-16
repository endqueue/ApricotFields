
#ifndef APREND_BASE_H
#define APREND_BASE_H

#include <spudgpu.h>

/**
 * @file aprendcontext.h
 * @brief The Aprend instance: what everything else of Aprend is created
 * from, its frames, and the two rules every Aprend header rests on.
 *
 * @par Threads
 * This is the rule for every Aprend header (render/aprend*.h). Each of the
 * others says only what is particular to it.
 *
 * Aprend takes no lock and starts no thread. An instance, and everything
 * created from it, is used by one thread at a time, and keeping it so is the
 * caller's. It may be any thread, and a different one from call to call, as
 * long as two are never inside Aprend for the same instance at once.
 *
 * That is every call that takes the instance or a handle created from it,
 * the create calls included and the destroy calls excepted (below), because
 * they meet in state the instance holds:
 *
 * - the count of submissions and the release of what was destroyed: every
 *   call that submits, aprend_instance_next_frame() and
 *   aprend_instance_wait_idle();
 * - the frame index: aprend_instance_next_frame(), a buffer set's update
 *   calls, aprend_command_list_compile() and aprend_command_list_submit();
 * - the command memory of the current frame (a SpudGPU allocator and the
 *   lists recorded from it), which every recording of the instance is made
 *   in: aprend_command_list_compile() and _submit(), the texture update,
 *   readback and framebuffer clear calls, and aprend_instance_next_frame();
 * - the upload batch those update, readback and clear calls record into,
 *   which the next submission carries;
 * - a texture's tracked image layout: read by aprend_command_list_compile(),
 *   written by aprend_command_list_submit() and by the texture update,
 *   readback, resize and framebuffer clear calls;
 * - a buffer's writes not yet on the device: written by its update call,
 *   read by aprend_command_list_submit();
 * - a binding layout's descriptor pools: aprend_binding_set_create(), and
 *   the release of a destroyed set, which happens inside whichever later
 *   call drains the release queue.
 *
 * What several threads may do at once:
 *
 * - Call the functions that take no handle: aprend_uniform_type_get_size(),
 *   aprend_buffer_element_type_get_size(),
 *   aprend_buffer_layout_get_element_index(),
 *   aprend_buffer_layout_get_total_size() and the aprend_blend_ presets.
 * - Read a handle, with its get_ calls, while no thread is in a call that
 *   changes or destroys that handle. aprend_instance_get_frame_index() is
 *   not one of these while another thread may call
 *   aprend_instance_next_frame().
 * - Destroy. A destroy call may be made from any thread, at the same time
 *   as other threads use the instance, for every handle but the instance
 *   itself and a swap chain. It only queues the object: nothing is released
 *   on the thread that destroys (see "Destroying"). What it asks is that no
 *   other thread is using that same handle at that moment - inside a call
 *   that takes it, or recording a command that names it - and that the
 *   order the destroy calls ask for (a view before its texture, a set
 *   before its layout) is kept by whoever makes them.
 * - Record into different command lists, one thread to a list, with
 *   aprend_send_command() and aprend_command_list_reset(). They write the
 *   list and nothing else. No thread may destroy what the commands name
 *   meanwhile. Compiling is not included: every list compiles into the
 *   instance's command memory for the frame.
 *
 * Two instances share nothing of Aprend's, which keeps no state outside an
 * instance. Whether they may then be used from two threads at once is
 * SpudGPU's to say, since two instances created on one device call into the
 * same device, and spudgpu.h promises nothing about that. Until it does,
 * treat every instance on one device as one instance for this rule.
 *
 * A call that blocks, blocks the thread that made it and holds the instance
 * for as long: aprend_instance_next_frame(), aprend_instance_wait_idle(),
 * aprend_instance_destroy(), a texture readback, and a swap chain's resize
 * and destroy.
 *
 * Aprend never calls back. No code of the caller's runs inside it, on any
 * thread, and what a destroy call leaves to be released later is released
 * inside a later call the caller makes, never behind its back.
 *
 * None of this is checked. A call from a second thread is a data race, not
 * a failure the call reports.
 *
 * @par Destroying
 * A destroy call of anything the GPU uses - a buffer, a texture or a view of
 * one, a sampler, a shader, a pipeline, a binding layout or set, a command
 * list - does not need the GPU to be finished with it. The caller's handle
 * is gone when the call returns; the GPU's side is released once everything
 * submitted through the instance up to that moment has finished, which is
 * at once if it already has. The same holds for the old image of a resized
 * texture. Things are released in the order they were destroyed, so the
 * orders the destroy calls ask for (a view before its texture, a set before
 * its layout) still stand.
 *
 * Two things this does not cover:
 *
 * - A command list compiled against an object and submitted after the
 *   object was destroyed. Compiling a list records nothing about what it
 *   uses for this purpose: only a submission does. So destroy an object
 *   after the last submission that uses it, not after the last compile, and
 *   reset or recompile any list that still names it.
 * - Work the caller submits to SpudGPU itself, which the instance never
 *   sees.
 *
 * What is waiting is released as the instance goes: after each submission
 * and in aprend_instance_next_frame() and aprend_instance_wait_idle(), on
 * the thread that makes those calls. A destroy call releases nothing itself,
 * which is what lets it come from any thread; with no later submission,
 * what was destroyed waits for aprend_instance_wait_idle() or
 * aprend_instance_destroy().
 *
 * @par Texture updates and clears
 * A texture update or readback and a framebuffer clear are not each a
 * submission. They are recorded, in order, into one batch for the frame,
 * which goes ahead of the command lists of the next
 * aprend_command_list_submit(), in that same submission.
 * aprend_instance_next_frame() and aprend_instance_wait_idle() submit it on
 * its own if nothing else has. So each runs after everything submitted
 * before it and ahead of everything submitted after, as if it were
 * submitted at once, and a frame's updates cost one SpudGPU list between
 * them. A readback submits the batch itself, since it waits for the result.
 * Destroying a texture the batch writes is safe as anything else is: it is
 * released once the submission that carries the batch has finished.
 */

#if __cplusplus
extern "C" {
#endif

/**
 * @brief Upper bound on aprend_instance_desc::frames_in_flight.
 */
#define APREND_MAX_FRAMES_IN_FLIGHT 4

/**
 * @brief Configuration descriptor for an instance.
 */
typedef struct aprend_instance_desc {
	/**
	 * The device everything of this instance is created on. Never NULL. It
	 * is the caller's: it must outlive the instance and is not destroyed
	 * with it.
	 */
	spudgpu_device device;
	/**
	 * How many frames the caller lets the GPU hold at once: how many it
	 * submits before it has to know the oldest has finished. 1 to
	 * APREND_MAX_FRAMES_IN_FLIGHT, and 0 is refused. A buffer set
	 * (aprendbuffers.h) keeps this many copies of its buffer, one for each
	 * frame, so the frame being written never shares a copy with one the
	 * GPU is still reading. 1 is a caller that waits for each frame to
	 * finish before starting the next.
	 */
	uint32_t frames_in_flight;
	/**
	 * Bytes in one chunk of the instance's transient memory
	 * (aprendtransient.h), which grows a chunk at a time. 0 is an instance
	 * with no transient memory: aprend_transient_allocate() fails on it, and
	 * #transient_memory_limit must be 0 too. A span larger than this gets a
	 * chunk of its own for the frame, so this is the size of the usual
	 * frame's worth, not of the largest span. No memory is taken until the
	 * first span is. Aprend takes spans of it too: a material's changed
	 * uniform blocks go to the GPU through it (aprendmaterial.h), and a
	 * material that owns a block can't be created on an instance without
	 * it.
	 */
	uint64_t transient_chunk_size;
	/**
	 * The most bytes the instance's transient memory may hold, over every
	 * frame in flight; aprend_transient_allocate() fails when a chunk more
	 * would pass it. 0 is no limit. If not 0, at least
	 * #transient_chunk_size.
	 */
	uint64_t transient_memory_limit;
} aprend_instance_desc;

/**
 * @brief Opaque handle to an instance.
 */
typedef struct aprend_instance_t *aprend_instance;

/**
 * @brief Creates an instance on a device.
 *
 * @param[in] desc Instance configuration. Copied: only read during the call.
 *
 * @return The instance, or NULL if @p desc is NULL, `device` is NULL,
 *         `frames_in_flight` is 0 or above APREND_MAX_FRAMES_IN_FLIGHT,
 *         `transient_memory_limit` is set with a `transient_chunk_size` of
 *         0 or is below it, or the device refuses what the instance needs
 *         of it.
 *
 * @see aprend_instance_destroy()
 */
aprend_instance aprend_instance_create(const aprend_instance_desc *desc);

/**
 * @brief Moves the instance to its next frame and returns the new frame
 * index.
 *
 * The frame index runs 0 to `frames_in_flight` - 1 and starts at 0. It says
 * which copy of every buffer set is the current one: the one the update
 * calls write, and the one a command list compiled now reads.
 *
 * This moves to the next index, wrapping to 0. The caller calls it once at
 * the start of each frame after the first, before writing any buffer set or
 * compiling any command list for that frame.
 *
 * First it submits the frame's texture updates and framebuffer clears, if
 * no submission has carried them yet ("Texture updates and clears", at the
 * top of this header). If that submission fails they are dropped, and that
 * is printed.
 *
 * It blocks until everything submitted the last time this index was
 * current has finished: the frame from `frames_in_flight` frames ago. So
 * at most `frames_in_flight` frames are ever on the GPU, and the caller
 * needs no wait of its own between frames. With `frames_in_flight` 1 it
 * waits for the frame before. If the wait fails the index has still moved,
 * and the failure is printed.
 *
 * What it waits for is what went through Aprend:
 * aprend_command_list_submit() and the texture updates, readbacks and
 * framebuffer clears. Work the caller submits to SpudGPU itself is not
 * counted.
 *
 * Once that wait is over, the memory of every recording made in that frame
 * comes back: the compiles of command lists and the update batch, recorded
 * through a SpudGPU command allocator of that frame index, which this
 * resets, and the frame's transient memory (aprendtransient.h). A caller
 * that never calls this never gets either back. After a failed wait
 * neither is taken back, and both go on growing until a wait succeeds.
 *
 * An instance submits on one queue. The queue is the caller's to choose,
 * and it is chosen by the first call that submits: every later one is
 * refused unless it is given the same queue. Completion is counted in the
 * order of submission, which is only the order things finish in on a single
 * queue.
 *
 * It changes which copy every buffer set writes and every compile reads, so
 * under the rule at the top of this header ("Threads") no other thread is
 * in the instance while it runs.
 *
 * @param[in] instance Instance to move on.
 *
 * @return The new frame index, or 0 if @p instance is NULL.
 */
uint32_t aprend_instance_next_frame(aprend_instance instance);

/**
 * @brief Gets the current frame index.
 *
 * @param[in] instance Instance to read.
 *
 * @return The frame index, or 0 if @p instance is NULL.
 */
uint32_t aprend_instance_get_frame_index(aprend_instance instance);

/**
 * @brief Blocks until everything submitted through an instance so far has
 * finished.
 *
 * That is every aprend_command_list_submit(), and every texture update,
 * readback and framebuffer clear: a batch of them not yet submitted is
 * submitted first. Whatever was destroyed and waiting on
 * that work is released before it returns. Destroying and resizing don't
 * need it (see "Destroying" at the top of this header); it is for a caller
 * that wants the result of submitted work, or wants memory back now. It
 * waits for Aprend's own submissions and nothing else on the queue: work
 * the caller submits to SpudGPU itself is the caller's to wait for.
 *
 * It holds the instance while it blocks ("Threads", at the top of this
 * header).
 *
 * @param[in] instance Instance to wait for.
 *
 * @return false if @p instance is NULL, the batch of texture updates and
 *         clears can't be submitted, or the wait fails.
 */
bool aprend_instance_wait_idle(aprend_instance instance);

/**
 * @brief Reads an instance's descriptor.
 *
 * @param[in]  instance Instance to read.
 * @param[out] out_desc Receives the descriptor the instance was created
 *                      with.
 *
 * @return false, with nothing written, if @p instance or @p out_desc is
 *         NULL.
 */
bool aprend_instance_get_desc(
    aprend_instance instance,
    aprend_instance_desc *out_desc);

/**
 * @brief Destroys an instance.
 *
 * Waits for everything submitted through @p instance to finish and releases
 * whatever is still waiting to be released ("Destroying", at the top of
 * this header).
 *
 * @warning Every handle of the instance is destroyed before it. A swap
 * chain is not deferred: it waits for its queue itself.
 *
 * @param[in] instance Instance to destroy. NULL is accepted and does
 *                     nothing.
 */
void aprend_instance_destroy(aprend_instance instance);

#if __cplusplus
}
#endif

#endif // APREND_BASE_H
