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
 * aprendcommands.h, or aprend_render_pass_desc's): the GPU writes the time
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
 * The rule is in aprendcontext.h ("Threads"): a timer is used by one thread
 * at a time with everything else of its instance.
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
typedef struct aprend_gpu_timer_desc {
	/** Number of slots: passes that can be timed in one frame. Not 0. */
	uint32_t _slot_count;
	/**
	 * The queue the passes are submitted on: the instance's one queue
	 * (aprendcontext.h). D3D12 counts time per queue.
	 */
	spudgpu_command_queue _queue;
} aprend_gpu_timer_desc;

/**
 * @brief Creates a GPU timer.
 *
 * @param[in] instance Instance whose passes it times.
 * @param[in] desc     Timer configuration. Only read during the call.
 *
 * @return The timer, or NULL if @p instance or @p desc is NULL,
 *         `_slot_count` is 0, `_queue` is NULL, or the device can't write
 *         timestamps at a pass's start and end
 *         (`SPUDGPU_DEVICE_PROPERTIES::pass_timestamps`) or refuses the
 *         timer.
 *
 * @see aprend_gpu_timer_destroy()
 */
aprend_gpu_timer aprend_gpu_timer_create(
    aprend_instance instance,
    const aprend_gpu_timer_desc *desc);

/**
 * @brief Destroys a GPU timer.
 *
 * The GPU doesn't have to be finished with it (aprendcontext.h,
 * "Destroying"). A command list compiled with a pass that names it must not
 * be submitted afterwards.
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
 *
 * @return false, with nothing written, if an argument is NULL or out of
 *         range, if no submitted pass timed the slot the last time the
 *         current frame index was current, if this frame has already
 *         submitted a pass that times it, or if the times can't be read.
 */
bool aprend_gpu_timer_get_ms(
    aprend_gpu_timer timer,
    uint32_t slot,
    double *out_ms);

#if __cplusplus
}
#endif // __cplusplus

#endif // APREND_TIMER_H
