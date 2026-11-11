#ifndef RBTREND_TIMER_H
#define RBTREND_TIMER_H

#include "rbtrendcontext.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @file rbtrendtimer.h
 * @brief GPU timers: how long rendering passes took on the GPU, a frame
 * later.
 *
 * A timer has slots. A pass names a timer and a slot
 * (RBTREND_COMMAND_BEGIN_RENDERING's `_timer` and `_timer_slot`,
 * rbtrendcommands.h, or RBTREND_RENDER_PASS_DESC's): the GPU writes the time
 * as the pass's first work starts and as its last work ends, and the slot
 * holds the difference. Only a whole pass is timed, because that is what
 * every backend can measure: Apple GPUs sample their clocks at the
 * boundaries of a pass's stages and nowhere in between (crgpu.h,
 * "Timestamps"). Time spent between passes, and compute dispatches, are not
 * in any slot.
 *
 * The time is read a frame late, the way the frames already work
 * (rbtrendcontext.h): a timer keeps its slots once for each frame in flight,
 * and rbtrend_gpu_timer_get_ms() reads the copy of the current frame index,
 * which holds what was written the last time that index was current,
 * `frames_in_flight` frames ago. rbtrend_instance_next_frame() has waited for
 * that frame, so the read doesn't wait. Read a slot before this frame's
 * submission that times it again.
 *
 * One pass to a slot in a frame: a second pass that names the same slot in
 * the same frame replaces the first's time.
 *
 * The times are the GPU's own and cover what the GPU did between the
 * pass's start and end, including waiting on other work it overlapped.
 *
 * @par Threads
 * The rule is in rbtrendcontext.h ("Threads"): a timer is used by one
 * caller at a time with everything else of its instance.
 */

/**
 * @brief Opaque handle to a GPU timer.
 */
typedef struct rbtrend_gpu_timer_t *rbtrend_gpu_timer;

/**
 * @brief Configuration descriptor for a GPU timer.
 *
 * No field has a default.
 */
typedef struct RBTREND_GPU_TIMER_DESC {
	/** This struct's type, RBT_STRUCT_TYPE_REND_GPU_TIMER_DESC, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/** Number of slots: passes that can be timed in one frame. Not 0. */
	uint32_t _slot_count;
	/**
	 * The queue the passes are submitted on: the instance's one queue
	 * (rbtrendcontext.h). D3D12 counts time per queue.
	 */
	crgpu_command_queue _queue;
} RBTREND_GPU_TIMER_DESC;

/**
 * @brief Creates a GPU timer.
 *
 * @param[in]  instance  Instance whose passes it times.
 * @param[in]  desc      Timer configuration. Only read during the call.
 * @param[out] out_timer Receives the timer; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p instance, @p desc
 *         or @p out_timer is NULL, `_slot_count` is 0 or `_queue` is NULL;
 *         RBT_RESULT_LIMIT if `_slot_count` is above UINT32_MAX / 2;
 *         RBT_RESULT_UNSUPPORTED if the device was created without
 *         CRGPU_FEATURE_PASS_TIMESTAMPS (timestamps at a pass's start and
 *         end); the struct codes for `desc->base`; RBT_RESULT_OUT_OF_MEMORY;
 *         or the code for Carrottus GPU's result if it refuses the timer.
 *
 * @see rbtrend_gpu_timer_destroy()
 */
RBT_RESULT rbtrend_gpu_timer_create(
    rbtrend_instance instance,
    const RBTREND_GPU_TIMER_DESC *desc,
    rbtrend_gpu_timer *out_timer,
    RBTREND_ERROR *out_error);

/**
 * @brief Destroys a GPU timer.
 *
 * The GPU doesn't have to be finished with it (rbtrendcontext.h,
 * "Destroying"). A command list compiled with a pass that names it must not
 * be submitted afterwards.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p timer itself meanwhile (rbtrendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] timer Timer to destroy. NULL is accepted and does nothing.
 */
void rbtrend_gpu_timer_destroy(rbtrend_gpu_timer timer);

/**
 * @brief Reads how long the pass timed in a slot took, `frames_in_flight`
 * frames ago.
 *
 * @param[in]  timer  Timer to read.
 * @param[in]  slot   Slot, below the timer's `_slot_count`.
 * @param[out] out_ms Receives the pass's GPU time in milliseconds.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; with nothing written, RBT_RESULT_INVALID_ARGUMENT if
 *         @p timer or @p out_ms is NULL or @p slot is out of range;
 *         RBT_RESULT_NOT_FOUND if no submitted pass timed the slot the last
 *         time the current frame index was current, or this frame has
 *         already submitted a pass that times it; or the code for
 *         Carrottus GPU's result if the times can't be read.
 */
RBT_RESULT rbtrend_gpu_timer_get_ms(
    rbtrend_gpu_timer timer,
    uint32_t slot,
    double *out_ms,
    RBTREND_ERROR *out_error);

#if __cplusplus
}
#endif // __cplusplus

#endif // RBTREND_TIMER_H
