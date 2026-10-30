#include "render/aprendtimer.h"
#include "aprend_internal.hpp"

#include <cstdio>
#include <new>

// The sets go once the GPU has finished with them (aprend_instance_retire).
aprend_gpu_timer_t::~aprend_gpu_timer_t() {
	for (spudgpu_timestamp_set set : sets)
		if (set)
			spudgpu_destroy_timestamp_set(set);
}

extern "C" {

APRESULT aprend_gpu_timer_create(
    aprend_instance instance,
    const APREND_GPU_TIMER_DESC *desc,
    aprend_gpu_timer *out_timer,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_timer)
		*out_timer = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_gpu_timer_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_GPU_TIMER_DESC))
		return base;
	if (desc->_slot_count == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_slot_count is 0");
	// Two entries a slot, counted in 32 bits.
	if (desc->_slot_count > UINT32_MAX / 2)
		return aprend_fail(out_error, APRESULT_LIMIT, "_slot_count %u is above %u", desc->_slot_count, UINT32_MAX / 2);
	if (!desc->_queue)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "_queue is NULL");
	if (!(instance->features & SPUDGPU_FEATURE_PASS_TIMESTAMPS))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "the device was created without SPUDGPU_FEATURE_PASS_TIMESTAMPS");
	if (!out_timer)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_timer is NULL");

	aprend_gpu_timer_t *result = (aprend_gpu_timer_t *)malloc(sizeof(aprend_gpu_timer_t));
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the timer");
	result             = new (result) aprend_gpu_timer_t();
	result->instance   = instance;
	result->slot_count = desc->_slot_count;
	try {
		result->written_frame.assign((size_t)instance->desc.frames_in_flight * desc->_slot_count, UINT64_MAX);
	} catch (...) {
		result->~aprend_gpu_timer_t();
		free(result);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the timer's %u slots", desc->_slot_count);
	}
	for (uint32_t i = 0; i < instance->desc.frames_in_flight; ++i) {
		SPUDRESULT sr = spudgpu_create_timestamp_set(instance->desc.device, desc->_queue, desc->_slot_count * 2, &result->sets[i]);
		if (SPUDFAIL(sr)) {
			result->sets[i] = nullptr;
			result->~aprend_gpu_timer_t();
			free(result);
			return aprend_fail_spud(out_error, sr, "creating the timestamps failed");
		}
	}
	// Its destroy's retire, promised now so the destroy can't fail.
	if (aprend_instance_reserve_release(instance, 1) != APRESULT_OK) {
		result->~aprend_gpu_timer_t();
		free(result);
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to release the timer later");
	}
	*out_timer = result;
	return APRESULT_OK;
}

void aprend_gpu_timer_destroy(aprend_gpu_timer timer) {
	if (!timer)
		return;
	// A list compiled against it and not submitted would mark its slots.
	aprend_instance_forget(timer->instance, timer);
	aprend_instance_retire(timer->instance, timer, &aprend_release_handle<aprend_gpu_timer_t>);
}

APRESULT aprend_gpu_timer_get_ms(
    aprend_gpu_timer timer,
    uint32_t slot,
    double *out_ms,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!timer)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "timer is NULL");
	if (slot >= timer->slot_count)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "slot %u is at or above the timer's %u slots", slot, timer->slot_count);
	if (!out_ms)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_ms is NULL");
	aprend_instance instance = timer->instance;
	const uint32_t frames    = instance->desc.frames_in_flight;
	// What the current index's copy holds is from frames_in_flight frames
	// ago, which aprend_instance_next_frame has waited for; anything else
	// is either nothing or still being written.
	if (instance->frame_number < frames)
		return aprend_fail(out_error, APRESULT_NOT_FOUND, "no frame %u frames ago has finished yet", frames);
	const uint64_t wanted = instance->frame_number - frames;
	if (timer->written_frame[(size_t)instance->frame_index * timer->slot_count + slot] != wanted)
		return aprend_fail(out_error, APRESULT_NOT_FOUND, "no pass submitted %u frames ago timed slot %u, or this frame has already timed it", frames, slot);
	uint64_t ns[2] = {};
	SPUDRESULT sr  = spudgpu_get_timestamps(timer->sets[instance->frame_index], slot * 2, 2, ns);
	if (SPUDFAIL(sr))
		return aprend_fail_spud(out_error, sr, "reading the timestamps failed");
	// A clock that went backwards is no time.
	*out_ms = ns[1] > ns[0] ? (double)(ns[1] - ns[0]) / 1.0e6 : 0.0;
	return APRESULT_OK;
}

} // extern "C"
