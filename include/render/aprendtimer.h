#ifndef APREND_TIMER_H
#define APREND_TIMER_H

#include "aprendcontext.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @file aprendtimer.h
 * @brief GPU timers: how long rendering passes took on the GPU, a frame
 * later.
 *
 * A timer has slots. A pass names a timer and a slot
 * (APREND_COMMAND_BEGIN_RENDERING's `_timer` and `_timer_slot`,
 * aprendcommands.h, or APREND_RENDER_PASS_DESC's): the GPU writes the time
 * as the pass's first work starts and as its last work ends, and the slot
 * holds the difference. Only a whole pass is timed, because that is what
 * every backend can measure: Apple GPUs sample their clocks at the
 * boundaries of a pass's stages and nowhere in between (spudgpu.h,
 * "Timestamps"). Time spent between passes, and compute dispatches, are not
 * in any slot.
 *
 * The time is read a frame late, the way the frames already work
 * (aprendcontext.h): a timer keeps its slots once for each frame in flight,
 * and aprend_gpu_timer_get_ms() reads the copy of the current frame index,
 * which holds what was written the last time that index was current,
 * `frames_in_flight` frames ago. aprend_instance_next_frame() has waited for
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
 * The rule is in aprendcontext.h ("Threads"): a timer is used by one
 * caller at a time with everything else of its instance.
 */

/**
 * @brief Opaque handle to a GPU timer.
 */
typedef struct aprend_gpu_timer_t *aprend_gpu_timer;

/**
 * @brief Configuration descriptor for a GPU timer.
 *
 * No field has a default.
 */
typedef struct APREND_GPU_TIMER_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_GPU_TIMER_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Number of slots: passes that can be timed in one frame. Not 0. */
	uint32_t _slot_count;
	/**
	 * The queue the passes are submitted on: the instance's one queue
	 * (aprendcontext.h). D3D12 counts time per queue.
	 */
	spudgpu_command_queue _queue;
} APREND_GPU_TIMER_DESC;

/**
 * @brief Creates a GPU timer.
 *
 * @param[in]  instance  Instance whose passes it times.
 * @param[in]  desc      Timer configuration. Only read during the call.
 * @param[out] out_timer Receives the timer; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance, @p desc
 *         or @p out_timer is NULL, `_slot_count` is 0 or `_queue` is NULL;
 *         APRESULT_LIMIT if `_slot_count` is above UINT32_MAX / 2;
 *         APRESULT_UNSUPPORTED if the device was created without
 *         SPUDGPU_FEATURE_PASS_TIMESTAMPS (timestamps at a pass's start and
 *         end); the struct codes for `desc->base`; APRESULT_OUT_OF_MEMORY;
 *         or the code for SpudGPU's result if it refuses the timer.
 *
 * @see aprend_gpu_timer_destroy()
 */
APRESULT aprend_gpu_timer_create(
    aprend_instance instance,
    const APREND_GPU_TIMER_DESC *desc,
    aprend_gpu_timer *out_timer,
    APREND_ERROR *out_error);

/**
 * @brief Destroys a GPU timer.
 *
 * The GPU doesn't have to be finished with it (aprendcontext.h,
 * "Destroying"). A command list compiled with a pass that names it must not
 * be submitted afterwards.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p timer itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] timer Timer to destroy. NULL is accepted and does nothing.
 */
void aprend_gpu_timer_destroy(aprend_gpu_timer timer);

/**
 * @brief Reads how long the pass timed in a slot took, `frames_in_flight`
 * frames ago.
 *
 * @param[in]  timer  Timer to read.
 * @param[in]  slot   Slot, below the timer's `_slot_count`.
 * @param[out] out_ms Receives the pass's GPU time in milliseconds.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; with nothing written, APRESULT_INVALID_ARGUMENT if
 *         @p timer or @p out_ms is NULL or @p slot is out of range;
 *         APRESULT_NOT_FOUND if no submitted pass timed the slot the last
 *         time the current frame index was current, or this frame has
 *         already submitted a pass that times it; or the code for
 *         SpudGPU's result if the times can't be read.
 */
APRESULT aprend_gpu_timer_get_ms(
    aprend_gpu_timer timer,
    uint32_t slot,
    double *out_ms,
    APREND_ERROR *out_error);

#if __cplusplus
}
#endif // __cplusplus

#endif // APREND_TIMER_H
