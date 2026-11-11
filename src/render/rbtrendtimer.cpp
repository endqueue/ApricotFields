#include "render/rbtrendtimer.h"
#include "rbtrend_internal.hpp"

#include <cstdio>
#include <new>

// The sets go once the GPU has finished with them (rbtrend_instance_retire).
rbtrend_gpu_timer_t::~rbtrend_gpu_timer_t() {
	for (crgpu_timestamp_set set : sets)
		if (set)
			crgpu_timestamp_set_destroy(set);
}

extern "C" {

RBT_RESULT rbtrend_gpu_timer_create(
    rbtrend_instance instance,
    const RBTREND_GPU_TIMER_DESC *desc,
    rbtrend_gpu_timer *out_timer,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_timer)
		*out_timer = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_gpu_timer_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_gpu_timer_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_GPU_TIMER_DESC))
		return base;
	if (desc->_slot_count == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_slot_count is 0");
	// Two entries a slot, counted in 32 bits.
	if (desc->_slot_count > UINT32_MAX / 2)
		return rbtrend_fail(out_error, RBT_RESULT_LIMIT, "_slot_count %u is above %u", desc->_slot_count, UINT32_MAX / 2);
	if (!desc->_queue)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "_queue is NULL");
	if (!(instance->features & CRGPU_FEATURE_PASS_TIMESTAMPS))
		return rbtrend_fail(out_error, RBT_RESULT_UNSUPPORTED, "the device was created without CRGPU_FEATURE_PASS_TIMESTAMPS");
	if (!out_timer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_timer is NULL");

	rbtrend_gpu_timer_t *result = (rbtrend_gpu_timer_t *)malloc(sizeof(rbtrend_gpu_timer_t));
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the timer");
	result             = new (result) rbtrend_gpu_timer_t();
	result->instance   = instance;
	result->slot_count = desc->_slot_count;
	try {
		result->written_frame.assign((size_t)instance->desc.frames_in_flight * desc->_slot_count, UINT64_MAX);
	} catch (...) {
		result->~rbtrend_gpu_timer_t();
		free(result);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the timer's %u slots", desc->_slot_count);
	}
	for (uint32_t i = 0; i < instance->desc.frames_in_flight; ++i) {
		CR_RESULT sr = crgpu_timestamp_set_create(instance->desc.device, desc->_queue, desc->_slot_count * 2, &result->sets[i]);
		if (CR_FAILED(sr)) {
			result->sets[i] = nullptr;
			result->~rbtrend_gpu_timer_t();
			free(result);
			return rbtrend_fail_cr(out_error, sr, "creating the timestamps failed");
		}
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (rbtrend_instance_reserve_release(instance, 1) != RBT_RESULT_OK) {
		result->~rbtrend_gpu_timer_t();
		free(result);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory to release the timer later");
	}
	*out_timer = result;
	return RBT_RESULT_OK;
}

void rbtrend_gpu_timer_destroy(rbtrend_gpu_timer timer) {
	if (!timer)
		return;
	if (RBT_STALE(timer, "rbtrend_gpu_timer_destroy", "timer"))
		return;
	// A list compiled against it and not submitted would mark its slots.
	rbtrend_instance_forget(timer->instance, timer);
	RBT_KILL(timer);
	rbtrend_instance_retire(timer->instance, timer, &rbtrend_release_handle<rbtrend_gpu_timer_t>);
}

RBT_RESULT rbtrend_gpu_timer_get_ms(
    rbtrend_gpu_timer timer,
    uint32_t slot,
    double *out_ms,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!timer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "timer is NULL");
	if (RBT_STALE(timer, "rbtrend_gpu_timer_get_ms", "timer"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "timer is destroyed");
	if (slot >= timer->slot_count)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "slot %u is at or above the timer's %u slots", slot, timer->slot_count);
	if (!out_ms)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_ms is NULL");
	rbtrend_instance instance = timer->instance;
	const uint32_t frames    = instance->desc.frames_in_flight;
	// What the current index's copy holds is from frames_in_flight frames
	// ago, which rbtrend_instance_next_frame has waited for; anything else
	// is either nothing or still being written.
	if (instance->frame_number < frames)
		return rbtrend_fail(out_error, RBT_RESULT_NOT_FOUND, "no frame %u frames ago has finished yet", frames);
	const uint64_t wanted = instance->frame_number - frames;
	if (timer->written_frame[(size_t)instance->frame_index * timer->slot_count + slot] != wanted)
		return rbtrend_fail(out_error, RBT_RESULT_NOT_FOUND, "no pass submitted %u frames ago timed slot %u, or this frame has already timed it", frames, slot);
	uint64_t ns[2] = {};
	CR_RESULT sr  = crgpu_timestamp_set_get(timer->sets[instance->frame_index], slot * 2, 2, ns);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(out_error, sr, "reading the timestamps failed");
	// A clock that went backwards is no time.
	*out_ms = ns[1] > ns[0] ? (double)(ns[1] - ns[0]) / 1.0e6 : 0.0;
	return RBT_RESULT_OK;
}

} // extern "C"
