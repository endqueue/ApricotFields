
#ifndef RBTREND_BASE_H
#define RBTREND_BASE_H

#include "../rbtcore.h"
#include <crgpu.h>

/**
 * @file rbtrendcontext.h
 * @brief The Rend instance: what everything else of Rend is created
 * from, its frames, and the rules every Rend header rests on.
 *
 * @par Threads
 * rbtcore.h's rule ("Threads") holds for every Rend header
 * (render/rbtrend*.h), with one difference: for Rend, the object of that
 * rule is the instance together with everything created from it. Its
 * handles meet in state the instance holds, so they are not independent of
 * each other: an instance and all its handles are used by one caller at a
 * time. Each of the other headers says only what is particular to it.
 *
 * Rend takes no lock for this and starts no thread. The caller may call
 * from any thread, and a different one from call to call, as long as no
 * two calls are inside Rend for the same instance at once.
 *
 * That is every call that takes the instance or a handle created from it,
 * the create calls included and the destroy calls mostly excepted (below),
 * because they meet in state the instance holds:
 *
 * - the count of submissions and the release of what was destroyed: every
 *   call that submits, rbtrend_instance_next_frame() and
 *   rbtrend_instance_wait_idle();
 * - the frame index: rbtrend_instance_next_frame(), the buffer writes (of a
 *   buffer of per-frame copies), rbtrend_command_list_compile() and
 *   rbtrend_command_list_submit();
 * - the command memory of the current frame (a Carrottus GPU allocator and the
 *   lists recorded from it), which every recording of the instance is made
 *   in: rbtrend_command_list_compile() and _submit(), the texture update,
 *   readback and framebuffer clear calls, and rbtrend_instance_next_frame();
 * - the upload batch those update, readback and clear calls record into,
 *   which the next submission carries;
 * - a texture's tracked image layout: read by rbtrend_command_list_compile(),
 *   written by rbtrend_command_list_submit() and by the texture update,
 *   readback, resize and framebuffer clear calls;
 * - a buffer's writes not yet on the device: written by its update call,
 *   read by rbtrend_command_list_submit();
 * - a binding layout's descriptor pools: rbtrend_binding_set_create(), and
 *   the release of a destroyed set, which happens inside whichever later
 *   call drains the release queue;
 * - the render target pool ("Textures a command uses in between"):
 *   rbtrend_command_list_compile() and _submit(), rbtrend_command_list_destroy()
 *   (it gives back what an unsubmitted compile took),
 *   rbtrend_instance_next_frame(), rbtrend_instance_get_render_target_pool(),
 *   rbtrend_instance_trim_render_targets() and
 *   rbtrend_instance_set_render_target_retirement().
 *
 * What may overlap, by exception (the calls are marked "Thread:"):
 *
 * - The functions that take no handle: rbtrend_uniform_type_get_size(),
 *   rbtrend_uniform_layout_get_uniform_index(),
 *   rbtrend_vertex_element_type_get_size(),
 *   rbtrend_vertex_layout_get_element_index(), the rbtrend_blend_ presets and
 *   rbtrend_get_optional_gpu_features(). They take no object, so the rule
 *   doesn't reach them.
 * - Reading a handle, with its get_ calls, while no call that changes or
 *   destroys that handle runs. rbtrend_instance_get_frame_index() is not one
 *   of these while rbtrend_instance_next_frame() may run.
 * - Destroying. A destroy call may overlap other calls on the instance, for
 *   every handle but the instance itself, a swap chain and a command list.
 *   It only queues the object: nothing is released by the destroy call
 *   itself (see "Destroying"). What it asks is that no other call is using
 *   that same handle at that moment - a call that takes it, or the
 *   recording of a command that names it - and that the order the destroy
 *   calls ask for (a view before its texture, a set before its layout) is
 *   kept by whoever makes them.
 * - Recording into different command lists, one caller to a list, with
 *   rbtrend_command_list_send() and rbtrend_command_list_reset(), and filling
 *   different draw lists with rbtrend_draw_list_add() and
 *   rbtrend_draw_list_reset() (rbtrenddrawlist.h). They write the list and
 *   nothing else. Nothing may destroy what the commands name meanwhile.
 *   Compiling is not included: every list compiles into the instance's
 *   command memory for the frame.
 *
 * Two instances share nothing of Rend's, which keeps no state outside an
 * instance. Whether two callers may then use them at once is Carrottus GPU's to
 * say, since two instances created on one device call into the same
 * device, and crgpu.h promises nothing about that. Until it does, treat
 * every instance on one device as one instance for this rule.
 *
 * A call that blocks, blocks its caller and holds the instance for as long:
 * rbtrend_instance_next_frame(), rbtrend_instance_wait_idle(),
 * rbtrend_instance_destroy(), a texture readback, and a swap chain's resize
 * and destroy.
 *
 * Rend never calls back. No code of the caller's runs inside it, and what
 * a destroy call leaves to be released later is released inside a later
 * call the caller makes, never behind its back.
 *
 * Two overlapping calls are a data race, not a failure the call reports. A
 * Debug build of Rend checks the rule (rbtcore.h, "Threads") on every
 * call that takes the instance or a handle of it but the exceptions above:
 * one that finds a call from another thread still inside the instance
 * prints both calls' names on stderr, and goes on. The recordings, which
 * are one caller to a list, are checked on their list, and so are a list's
 * compile and submit, which check the instance as well. The get_ and find
 * calls and the destroys are not checked, being the exceptions. It checks
 * overlap, not which thread calls. A Release build checks nothing.
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
 *   object was destroyed. The release waits for submissions, not compiles,
 *   so rbtrend_command_list_submit() refuses such a list instead: one that
 *   names a destroyed texture (2D or 3D) or view of one, a vertex, index,
 *   uniform or storage buffer, a sampler, a pipeline, a binding set or
 *   anything a set it binds holds, a material, a GPU timer or a swap chain;
 *   one compiled before a texture was resized, a binding set updated or a
 *   swap chain resized. Compiling it again is what the caller does then.
 *   Destroy an object after the last submission that uses it, not after
 *   the last compile, to keep a list submittable.
 * - Work the caller submits to Carrottus GPU itself, which the instance never
 *   sees.
 *
 * What is waiting is released as the instance goes: after each submission
 * and in rbtrend_instance_next_frame() and rbtrend_instance_wait_idle(),
 * inside those calls. A destroy call releases nothing itself, which is what
 * lets it overlap other calls on the instance; with no later submission,
 * what was destroyed waits for rbtrend_instance_wait_idle() or
 * rbtrend_instance_destroy().
 *
 * @par Texture updates and clears
 * A texture update or readback and a framebuffer clear are not each a
 * submission. They are recorded, in order, into one batch for the frame,
 * which goes ahead of the command lists of the next
 * rbtrend_command_list_submit(), in that same submission.
 * rbtrend_instance_next_frame() and rbtrend_instance_wait_idle() submit it on
 * its own if nothing else has. So each runs after everything submitted
 * before it and ahead of everything submitted after, as if it were
 * submitted at once, and a frame's updates cost one Carrottus GPU list between
 * them. A readback submits the batch itself, since it waits for the result.
 * Destroying a texture the batch writes is safe as anything else is: it is
 * released once the submission that carries the batch has finished.
 *
 * @par Textures a command uses in between
 * A command that records several passes through textures of its own
 * (RBTREND_COMMAND_BLOOM, rbtrendcommands.h) takes them from a pool the
 * instance keeps, when the list holding it is compiled. Nothing is made or
 * sized by the caller: a source of a new size takes textures of the new
 * size at the next compile. A texture taken in one frame is taken again
 * only once that frame has finished on the GPU, which
 * rbtrend_instance_next_frame() waits for, and two commands compiled in one
 * frame never share one. A compile that is not submitted (compiled again,
 * failed, reset, or its list destroyed, in the same frame) gives back what
 * it took. The pool holds a texture for every size taken, for each command
 * compiled in a frame and each frame in flight, until the caller says
 * otherwise: rbtrend_instance_set_render_target_retirement() has
 * rbtrend_instance_next_frame() destroy what has gone a number of frames
 * untaken while the pool is over a size, so sizes no longer used don't
 * stay, and rbtrend_instance_trim_render_targets() destroys what the GPU is
 * done with at once, all of it or what has sat untaken for a number of
 * frames: after a window is resized, or when memory is wanted elsewhere.
 * Without either, the pool destroys nothing until the instance goes.
 * rbtrend_instance_get_render_target_pool() reports what it holds.
 *
 * @par Results and errors
 * This is the rule for every Rend header. A call that can fail returns
 * an RBT_RESULT (rbtcore.h), and Rend prints nothing for it. A command
 * list's recording calls (rbtrend_command_list_send(),
 * rbtrend_command_list_reset()) return nothing, as Vulkan's do: a command
 * refused when it is sent is reported by the next compile. A creation hands its handle back through an `out_`
 * parameter, set to NULL on any failure, the `out_` parameter itself
 * included when it can be written. A call that can fail for a reason worth
 * a sentence also takes an RBTREND_ERROR pointer, last (NULL to skip it): on
 * failure it says what was wrong in developer English, naming the argument
 * or the object (by its debug name, where it has one); on success its
 * message is empty. Being per call, not kept on the instance, it never
 * outlives the call that filled it. A call whose only failures are NULL
 * arguments returns the code alone. What Carrottus GPU refuses becomes
 * RBT_RESULT_OUT_OF_MEMORY, RBT_RESULT_UNSUPPORTED or RBT_RESULT_LIMIT where one
 * of those says it, and RBT_RESULT_REND_GPU_FAILURE otherwise, its message
 * naming Carrottus GPU's result. The Debug build's check of "Threads" still
 * prints, as a validation layer would; it refuses nothing.
 *
 * @par Struct bases
 * This too is the rule for every Rend header. A desc, and each of the lit
 * scene's settings, starts with an RBT_STRUCT_BASE named `base`
 * (rbtcore.h, "Struct bases"): the caller sets `base.type` to the struct's
 * own RBT_STRUCT_TYPE_REND_* and `base.next` to NULL, since nothing
 * chains onto any of them yet. A call given a wrong type or a chained
 * struct refuses it with RBT_RESULT_STRUCT_WRONG_TYPE or
 * RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN; a command checked at a compile is
 * reported by that compile. A get_ call that hands a
 * desc or settings back writes the whole struct, `base` included: its own
 * type and no chain, so the caller sets nothing first. Every other struct
 * has a "Frozen" line above it and never changes.
 */

#if __cplusplus
extern "C" {
#endif

/**
 * @brief Upper bound on RBTREND_INSTANCE_DESC::frames_in_flight.
 */
#define RBTREND_MAX_FRAMES_IN_FLIGHT 4

/** @brief The size of RBTREND_ERROR's `message`, its NUL included. */
#define RBTREND_ERROR_MESSAGE_SIZE 256

/**
 * @brief Why a call failed, filled per call ("Results and errors", at the
 * top of this header).
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct RBTREND_ERROR {
	char message[RBTREND_ERROR_MESSAGE_SIZE]; /**< NUL-terminated, truncated to fit; empty on success. */
} RBTREND_ERROR;

/**
 * @brief Configuration descriptor for an instance.
 */
typedef struct RBTREND_INSTANCE_DESC {
	/** This struct's type, RBT_STRUCT_TYPE_REND_INSTANCE_DESC, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/**
	 * The device everything of this instance is created on. Never NULL. It
	 * is the caller's: it must outlive the instance and is not destroyed
	 * with it. Rend uses the features the device was created with
	 * (crgpu_device_get_features()), never the adapter's: the paths that
	 * rbtrend_get_optional_gpu_features() names are taken only on a device
	 * that has their feature, and a call that needs a feature names it and
	 * refuses a device without it. The device needs a direct queue: the
	 * instance's command allocators are direct.
	 */
	crgpu_device device;
	/**
	 * How many frames the caller lets the GPU hold at once: how many it
	 * submits before it has to know the oldest has finished. 1 to
	 * RBTREND_MAX_FRAMES_IN_FLIGHT, and 0 is refused. A buffer of per-frame
	 * copies (rbtrendbuffers.h) keeps this many, one for each frame, so the
	 * frame being written never shares a copy with one the GPU is still
	 * reading. 1 is a caller that waits for each frame to
	 * finish before starting the next.
	 */
	uint32_t frames_in_flight;
	/**
	 * Bytes in one chunk of the instance's transient memory
	 * (rbtrendtransient.h), which grows a chunk at a time. 0 is an instance
	 * with no transient memory: rbtrend_transient_allocate() fails on it, and
	 * #transient_memory_limit must be 0 too. A span larger than this gets a
	 * chunk of its own for the frame, so this is the size of the usual
	 * frame's worth, not of the largest span. No memory is taken until the
	 * first span is. Rend takes spans of it too: a material's changed
	 * uniform blocks go to the GPU through it (rbtrendmaterial.h), and a
	 * material that owns a block can't be created on an instance without
	 * it.
	 */
	uint64_t transient_chunk_size;
	/**
	 * The most bytes the instance's transient memory may hold, over every
	 * frame in flight; rbtrend_transient_allocate() fails when a chunk more
	 * would pass it. 0 is no limit. If not 0, at least
	 * #transient_chunk_size.
	 */
	uint64_t transient_memory_limit;
} RBTREND_INSTANCE_DESC;

/**
 * @brief Opaque handle to an instance.
 */
typedef struct rbtrend_instance_t *rbtrend_instance;

/**
 * @brief The Carrottus GPU features Rend's own code takes a better path with
 * when the device has them, and works without.
 *
 * For the caller to ask for, where the adapter supports them, when it
 * creates the device it gives rbtrend_instance_create(): Rend reads the
 * device's features, so one the device wasn't created with is a path not
 * taken. Today these are CRGPU_FEATURE_MULTI_DRAW_INDIRECT (an indirect
 * draw command is one Carrottus GPU call, not one a draw) and
 * CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY (a storage buffer is written
 * directly, not staged). Features that one of Rend's calls needs are not
 * here: each such call names its feature, and the caller asks for it if it
 * makes the call.
 *
 * A function, not a macro, so the value is the linked library's, not the
 * header's the caller compiled against.
 *
 * Thread: takes no object, so it may overlap any call, from any thread.
 *
 * @return The features, CRGPU_FEATURE_* bits.
 */
CRGPU_FEATURES rbtrend_get_optional_gpu_features(void);

/**
 * @brief Creates an instance on a device.
 *
 * @param[in]  desc         Instance configuration. Copied: only read during
 *                          the call.
 * @param[out] out_instance Receives the instance; NULL on failure.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p desc or
 *         @p out_instance is NULL, `device` is NULL, `frames_in_flight` is
 *         0 or above RBTREND_MAX_FRAMES_IN_FLIGHT, or
 *         `transient_memory_limit` is set with a `transient_chunk_size` of
 *         0 or is below it; RBT_RESULT_STRUCT_WRONG_TYPE or
 *         RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN for `desc->base`;
 *         RBT_RESULT_OUT_OF_MEMORY; or, if the device refuses what the
 *         instance needs of it, the code for Carrottus GPU's result ("Results and
 *         errors", at the top of this header).
 *
 * @see rbtrend_instance_destroy()
 */
RBT_RESULT rbtrend_instance_create(
    const RBTREND_INSTANCE_DESC *desc,
    rbtrend_instance *out_instance,
    RBTREND_ERROR *out_error);

/**
 * @brief Moves the instance to its next frame, once the GPU has finished
 * what the instance submitted the last time that frame index was current.
 *
 * The frame index runs 0 to `frames_in_flight` - 1 and starts at 0. It says
 * which copy of every buffer of per-frame copies is the current one: the
 * one a write writes, and the one a command list compiled now reads.
 *
 * This moves to the next index, wrapping to 0. The caller calls it once at
 * the start of each frame after the first, before writing any buffer or
 * compiling any command list for that frame.
 *
 * First it submits the frame's texture updates and framebuffer clears, if
 * no submission has carried them yet ("Texture updates and clears", at the
 * top of this header). If that submission fails, nothing else is done.
 *
 * Then it waits, up to @p timeout_ns, until everything submitted the last
 * time the next index was current has finished: the frame from
 * `frames_in_flight` frames ago. So at most `frames_in_flight` frames are
 * ever on the GPU, and the caller needs no wait of its own between frames.
 * With `frames_in_flight` 1 it waits for the frame before. A wait that runs
 * out, or fails, leaves the instance in the frame it was in: the call may
 * be made again.
 *
 * What it waits for is what went through Rend:
 * rbtrend_command_list_submit() and the texture updates, readbacks and
 * framebuffer clears. Work the caller submits to Carrottus GPU itself is not
 * counted.
 *
 * Once that wait is over, the memory of every recording made in that frame
 * comes back: the compiles of command lists and the update batch, recorded
 * through a Carrottus GPU command allocator of that frame index, which this
 * resets, and the frame's transient memory (rbtrendtransient.h). A caller
 * that never calls this never gets either back.
 *
 * An instance submits on one queue. The queue is the caller's to choose,
 * and it is chosen by the first call that submits: every later one is
 * refused unless it is given the same queue. Completion is counted in the
 * order of submission, which is only the order things finish in on a single
 * queue.
 *
 * It changes which copy every buffer write writes and every compile reads, so
 * under the rule at the top of this header ("Threads") no other call is in
 * the instance while it runs.
 *
 * @param[in]  instance        Instance to move on.
 * @param[in]  timeout_ns      The longest to wait for the frame, in
 *                             nanoseconds. UINT64_MAX waits without limit;
 *                             0 only looks.
 * @param[out] out_frame_index Receives the new frame index. May be NULL;
 *                             rbtrend_instance_get_frame_index() reads it
 *                             too.
 * @param[out] out_error       Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p instance is NULL;
 *         RBT_RESULT_TIMEOUT if the frame had not finished within
 *         @p timeout_ns; RBT_RESULT_REND_DEVICE_LOST if the device is lost,
 *         after which nothing the instance submits runs; or the code for
 *         Carrottus GPU's result if the updates and clears can't be submitted, or
 *         the wait or a command allocator's reset fails. On every failure the
 *         frame index is as it was.
 */
RBT_RESULT rbtrend_instance_next_frame(
    rbtrend_instance instance,
    uint64_t timeout_ns,
    uint32_t *out_frame_index,
    RBTREND_ERROR *out_error);

/**
 * @brief Gets the current frame index.
 *
 * @param[in] instance Instance to read.
 *
 * @return The frame index, or 0 if @p instance is NULL.
 */
uint32_t rbtrend_instance_get_frame_index(rbtrend_instance instance);

/**
 * @brief Blocks until everything submitted through an instance so far has
 * finished.
 *
 * That is every rbtrend_command_list_submit(), and every texture update,
 * readback and framebuffer clear: a batch of them not yet submitted is
 * submitted first. Whatever was destroyed and waiting on
 * that work is released before it returns. Destroying and resizing don't
 * need it (see "Destroying" at the top of this header); it is for a caller
 * that wants the result of submitted work, or wants memory back now. It
 * waits for Rend's own submissions and nothing else on the queue: work
 * the caller submits to Carrottus GPU itself is the caller's to wait for.
 *
 * It holds the instance while it blocks ("Threads", at the top of this
 * header).
 *
 * @param[in]  instance  Instance to wait for.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p instance is NULL;
 *         RBT_RESULT_REND_DEVICE_LOST if the device is lost; or the code for
 *         Carrottus GPU's result if the batch of texture updates and clears can't
 *         be submitted or the wait fails ("Results and errors", at the top
 *         of this header). What was destroyed and is
 *         known to be finished with is released either way.
 */
RBT_RESULT rbtrend_instance_wait_idle(
    rbtrend_instance instance,
    RBTREND_ERROR *out_error);

/**
 * @brief Reads an instance's descriptor.
 *
 * The whole struct is written, `base` included ("Struct bases", at the top
 * of this header): the caller sets nothing in it first.
 *
 * @param[in]  instance Instance to read.
 * @param[out] out_desc Receives the descriptor the instance was created
 *                      with.
 *
 * @return RBT_RESULT_OK, or RBT_RESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p instance or @p out_desc is NULL.
 */
RBT_RESULT rbtrend_instance_get_desc(
    rbtrend_instance instance,
    RBTREND_INSTANCE_DESC *out_desc);

/**
 * @brief What an instance's render target pool holds
 * (rbtrend_instance_get_render_target_pool()).
 *
 * Sizes are the bytes of the textures' texels, every mip of each, as their
 * formats count them: what the device allocates is that at least, and more
 * by whatever alignment and padding it adds.
 */
typedef struct RBTREND_RENDER_TARGET_POOL_INFO {
	/** This struct's type, RBT_STRUCT_TYPE_REND_RENDER_TARGET_POOL_INFO, set by the caller (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE_OUT base;
	/** How many textures the pool holds. */
	uint32_t texture_count;
	/**
	 * How many of them were taken in a frame the GPU may still be running,
	 * or by a compile of this frame: what a trim can't destroy yet.
	 */
	uint32_t busy_count;
	/** The texel bytes of every texture the pool holds. */
	uint64_t bytes;
	/** The texel bytes of the busy ones. */
	uint64_t busy_bytes;
} RBTREND_RENDER_TARGET_POOL_INFO;

/**
 * @brief Reports what an instance's render target pool holds ("Textures a
 * command uses in between", at the top of this header).
 *
 * It reads the pool, which compiles and rbtrend_instance_next_frame()
 * change, so it is one of the calls of the rule at the top of this header
 * ("Threads").
 *
 * @param[in]  instance Instance to read.
 * @param[out] out_info Receives the report; its `base.type` is set by the
 *                      caller, as for every struct Rend fills in.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT, with nothing written, if
 *         @p instance or @p out_info is NULL; RBT_RESULT_STRUCT_WRONG_TYPE or
 *         RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN, with nothing written, for
 *         `out_info->base`.
 */
RBT_RESULT rbtrend_instance_get_render_target_pool(
    rbtrend_instance instance,
    RBTREND_RENDER_TARGET_POOL_INFO *out_info);

/**
 * @brief Destroys the textures of an instance's render target pool that the
 * GPU is done with and nothing has taken for at least @p idle_frames frames.
 *
 * A texture taken in a frame the GPU may still be running, or by a compile
 * of this frame, is kept whatever @p idle_frames is. With @p idle_frames 0,
 * every other texture goes: the pool then holds only what is busy, and the
 * next compile of a command that needs one makes it again. Without this
 * call the pool destroys a texture only as
 * rbtrend_instance_set_render_target_retirement() says, at
 * rbtrend_instance_next_frame(); this does it now, by the caller's count, as
 * after a window is resized through many sizes, or when memory is wanted
 * for something else.
 *
 * Destroyed as every Rend object is ("Destroying", at the top of this
 * header): released once the GPU has finished what it was submitted with.
 * A list compiled in an earlier frame that took one is refused at submit,
 * as it would be anyway: the frame has moved on since its compile.
 *
 * It changes the pool, under the rule at the top of this header
 * ("Threads").
 *
 * @param[in]  instance        Instance whose pool is trimmed.
 * @param[in]  idle_frames     The frames a texture must have gone untaken to
 *                             be destroyed; 0 for every texture the GPU is
 *                             done with.
 * @param[out] out_trim_count  Receives how many textures were destroyed.
 *                             May be NULL.
 *
 * @return RBT_RESULT_OK, or RBT_RESULT_INVALID_ARGUMENT, with nothing destroyed
 *         and 0 written, if @p instance is NULL.
 */
RBT_RESULT rbtrend_instance_trim_render_targets(
    rbtrend_instance instance,
    uint32_t idle_frames,
    uint32_t *out_trim_count);

/**
 * @brief Sets how an instance's render target pool destroys textures by
 * itself: at each rbtrend_instance_next_frame(), while the texel bytes it
 * holds are above @p keep_bytes, each texture the GPU is done with that has
 * gone @p idle_frames frames untaken is destroyed, in the pool's order,
 * until it is at or under them.
 *
 * Until this is called the pool destroys nothing by itself (only
 * rbtrend_instance_trim_render_targets() does). Called again, it replaces
 * what it set. A busy texture is kept whatever it says, as for a trim, and
 * destroyed as every Rend object is ("Destroying", at the top of this
 * header). It changes the pool, under the rule at the top of this header
 * ("Threads").
 *
 * @param[in] instance    Instance whose pool it is.
 * @param[in] idle_frames The frames a texture must have gone untaken before
 *                        it may be destroyed; above 0.
 * @param[in] keep_bytes  The pool's size in texel bytes, as
 *                        RBTREND_RENDER_TARGET_POOL_INFO counts them, at or
 *                        under which nothing is destroyed; 0 for none kept.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK, or RBT_RESULT_INVALID_ARGUMENT, with nothing changed,
 *         if @p instance is NULL or @p idle_frames is 0.
 */
RBT_RESULT rbtrend_instance_set_render_target_retirement(
    rbtrend_instance instance,
    uint32_t idle_frames,
    uint64_t keep_bytes,
    RBTREND_ERROR *out_error);

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
void rbtrend_instance_destroy(rbtrend_instance instance);

#if __cplusplus
}
#endif

#endif // RBTREND_BASE_H
