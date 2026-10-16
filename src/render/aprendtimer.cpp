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

aprend_gpu_timer aprend_gpu_timer_create(
    aprend_instance instance,
    const aprend_gpu_timer_desc *desc) {
	static const char *who = "aprend_gpu_timer_create";
	if (!instance)
		return nullptr;
	if (!desc)
		return nullptr;
	if (desc->_slot_count == 0) {
		printf("apricot: %s: _slot_count is 0\n", who);
		return nullptr;
	}
	if (!desc->_queue) {
		printf("apricot: %s: _queue is NULL\n", who);
		return nullptr;
	}
	// Two entries a slot, counted in 32 bits.
	if (desc->_slot_count > UINT32_MAX / 2) {
		printf("apricot: %s: _slot_count %u is too large\n", who, desc->_slot_count);
		return nullptr;
	}

	aprend_gpu_timer_t *result = (aprend_gpu_timer_t *)malloc(sizeof(aprend_gpu_timer_t));
	if (!result)
		return nullptr;
	result             = new (result) aprend_gpu_timer_t();
	result->instance   = instance;
	result->slot_count = desc->_slot_count;
	try {
		result->written_frame.assign((size_t)instance->desc.frames_in_flight * desc->_slot_count, UINT64_MAX);
	} catch (...) {
		printf("apricot: %s: out of memory\n", who);
		result->~aprend_gpu_timer_t();
		free(result);
		return nullptr;
	}
	for (uint32_t i = 0; i < instance->desc.frames_in_flight; ++i) {
		SPUDRESULT sr = spudgpu_create_timestamp_set(instance->desc.device, desc->_queue, desc->_slot_count * 2, &result->sets[i]);
		if (SPUDFAIL(sr)) {
			printf("apricot: %s: the timestamps can't be created: %s\n", who, spudresult_str(sr));
			result->sets[i] = nullptr;
			result->~aprend_gpu_timer_t();
			free(result);
			return nullptr;
		}
	}
	return result;
}

void aprend_gpu_timer_destroy(aprend_gpu_timer timer) {
	if (!timer)
		return;
	aprend_instance_retire(timer->instance, timer, &aprend_release_handle<aprend_gpu_timer_t>);
}

bool aprend_gpu_timer_get_ms(
    aprend_gpu_timer timer,
    uint32_t slot,
    double *out_ms) {
	if (!timer || !out_ms)
		return false;
	if (slot >= timer->slot_count)
		return false;
	aprend_instance instance = timer->instance;
	const uint32_t frames    = instance->desc.frames_in_flight;
	// What the current index's copy holds is from frames_in_flight frames
	// ago, which aprend_instance_next_frame has waited for; anything else
	// is either nothing or still being written.
	if (instance->frame_number < frames)
		return false;
	const uint64_t wanted = instance->frame_number - frames;
	if (timer->written_frame[(size_t)instance->frame_index * timer->slot_count + slot] != wanted)
		return false;
	uint64_t ns[2] = {};
	SPUDRESULT sr  = spudgpu_get_timestamps(timer->sets[instance->frame_index], slot * 2, 2, ns);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_gpu_timer_get_ms: the timestamps can't be read: %s\n", spudresult_str(sr));
		return false;
	}
	// A clock that went backwards is no time.
	*out_ms = ns[1] > ns[0] ? (double)(ns[1] - ns[0]) / 1.0e6 : 0.0;
	return true;
}

} // extern "C"
