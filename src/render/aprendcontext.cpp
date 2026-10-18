
#include "render/aprendcontext.h"
#include "aprend_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

// The lists go before their allocator.
static void aprend_command_pool_destroy(aprend_instance_t::command_pool &pool) {
	for (spudgpu_command_list list : pool.lists)
		spudgpu_destroy_command_list(list);
	if (pool.allocator)
		spudgpu_destroy_command_allocator(pool.allocator);
}

static SPUDRESULT aprend_command_pool_take(
    aprend_instance_t::command_pool &pool,
    spudgpu_command_list *out_list) {
	if (pool.used == pool.lists.size()) {
		spudgpu_command_list list = nullptr;
		SPUDRESULT sr             = spudgpu_create_command_list(pool.allocator, &list);
		if (SPUDFAIL(sr))
			return sr;
		// The push can only fail for want of memory. No exception leaves a
		// C entry point, and this is reached from one.
		try {
			pool.lists.push_back(list);
		} catch (...) {
			spudgpu_destroy_command_list(list);
			return SPUDRESULT_OUT_OF_MEMORY;
		}
	}
	*out_list = pool.lists[pool.used++];
	return SPUD_SUCCESS;
}

// Once nothing recorded into the pool runs: its memory comes back, and every
// list is free again.
static void aprend_command_pool_reset(
    aprend_instance_t::command_pool &pool,
    uint32_t frame_index) {
	SPUDRESULT reset = spudgpu_reset_command_allocator(pool.allocator);
	if (SPUDFAIL(reset))
		printf("apricot: aprend_instance_next_frame: resetting a command allocator of frame %u failed: %s\n", frame_index, spudresult_str(reset));
	pool.used = 0;
}

aprend_instance_t::~aprend_instance_t() {
	// Onto the release queue first, so the loop below releases them in
	// order with everything else.
	aprend_builtins_destroy(this);
	aprend_render_target_pool_destroy(this);
	// Whatever is still queued goes now: the instance is the last thing that
	// could release it. aprend_instance_destroy has waited for the GPU. A
	// release may retire something more, which this loop then reaches.
	for (;;) {
		release_entry entry{};
		{
			std::lock_guard<std::mutex> lock(this->release_mutex);
			if (this->release_head == this->release_queue.size())
				break;
			entry = this->release_queue[this->release_head++];
		}
		entry.release(entry.object);
	}
	// Every material has been released with the queue, and given its range
	// back: the arena's buffers are all that is left of it.
	aprend_material_arena_destroy(this);
	// Nothing reads a transient span any more.
	aprend_transient_arena_destroy(this);
	// An instance that failed to create may have neither. A batch that was
	// never submitted is dropped with the lists, which go before their
	// allocators.
	if (this->frame_fence)
		spudgpu_destroy_fence(this->frame_fence);
	// Closed first: a Metal encoder is ended before it goes.
	if (this->upload_batch && this->upload_batch_open)
		spudgpu_end_command_list(this->upload_batch);
	for (command_frame &frame : this->frame_commands) {
		aprend_command_pool_destroy(frame.recordings);
		aprend_command_pool_destroy(frame.batches);
	}
}

SPUDRESULT aprend_instance_submit(
    aprend_instance instance,
    spudgpu_command_queue queue,
    spudgpu_command_list *lists,
    uint32_t list_count,
    spudgpu_swap_chain swap_chain,
    uint64_t *out_serial) {
	// One queue for the life of the instance. On a second queue a serial
	// could be reached while work submitted before it, on the first, was
	// still running, and everything that waits on the fence would be told
	// that work had finished.
	if (instance->submit_queue && queue != instance->submit_queue) {
		printf("apricot: submission refused: this instance submits on one queue, and this is not the queue of its first submission\n");
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	}
	// The batch was recorded for its queue, and is the instance's first
	// submission when no queue is chosen yet.
	if (instance->upload_batch && queue != instance->upload_batch_queue) {
		printf("apricot: submission refused: texture updates or clears are waiting to go on another queue, and this instance submits on one\n");
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	}
	assert(list_count <= APREND_SUBMIT_MAX_LISTS);
	assert(list_count > 0 || instance->upload_batch);

	// The batch goes first, in this submission: what it writes is what the
	// lists after it read.
	spudgpu_command_list all[APREND_SUBMIT_MAX_LISTS + 1];
	uint32_t all_count = 0;
	if (instance->upload_batch) {
		if (instance->upload_batch_open) {
			spudgpu_end_command_list(instance->upload_batch);
			instance->upload_batch_open = false;
		}
		all[all_count++] = instance->upload_batch;
	}
	for (uint32_t i = 0; i < list_count; ++i)
		all[all_count++] = lists[i];

	// What was written into the frame's transient spans, which the lists
	// may read. A failed flush fails the submission: the GPU would read
	// bytes that may not have reached it.
	SPUDRESULT flushed = aprend_transient_arena_flush(instance);
	if (SPUDFAIL(flushed))
		return flushed;
	const uint64_t serial = instance->submit_serial.load() + 1;
	spudgpu_submit_desc submit{};
	submit.cmd_lists          = all;
	submit.cmd_list_count     = all_count;
	submit.signal_fence       = instance->frame_fence;
	submit.signal_fence_value = serial;
	submit.swap_chain         = swap_chain;
	SPUDRESULT sr             = spudgpu_queue_submit(queue, &submit);
	if (SPUDFAIL(sr))
		return sr;
	// Taken only by a submission that went in, so the fence's value never
	// has a gap a wait could hang on. With the batch gone in, what is
	// retired from now on waits for this serial and no later one.
	{
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		instance->submit_serial        = serial;
		instance->upload_batch_waiting = false;
	}
	instance->upload_batch                             = nullptr;
	instance->upload_batch_queue                       = nullptr;
	instance->frame_last_serial[instance->frame_index] = serial;
	// Set by a submission that went in: a failed first one binds nothing.
	instance->submit_queue = queue;
	if (out_serial)
		*out_serial = serial;
	// A point the GPU has usually moved past something by.
	aprend_instance_collect(instance);
	return SPUD_SUCCESS;
}

SPUDRESULT aprend_instance_wait_serial(
    aprend_instance instance,
    uint64_t serial) {
	return spudgpu_wait_for_fences(instance->desc.device, &instance->frame_fence, &serial, 1, true, UINT64_MAX);
}

SPUDRESULT aprend_instance_take_command_list(
    aprend_instance instance,
    spudgpu_command_list *out_list) {
	return aprend_command_pool_take(instance->frame_commands[instance->frame_index].recordings, out_list);
}

SPUDRESULT aprend_instance_flush_upload_batch(
    aprend_instance instance,
    uint64_t *out_serial) {
	if (!instance->upload_batch) {
		*out_serial = instance->submit_serial.load();
		return SPUD_SUCCESS;
	}
	return aprend_instance_submit(instance, instance->upload_batch_queue, nullptr, 0, nullptr, out_serial);
}

SPUDRESULT aprend_instance_open_upload_batch(
    aprend_instance instance,
    spudgpu_command_queue queue,
    spudgpu_command_list *out_list) {
	if (instance->submit_queue && queue != instance->submit_queue) {
		printf("apricot: refused: this instance submits on one queue, and this is not the queue of its first submission\n");
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	}
	if (instance->upload_batch && queue != instance->upload_batch_queue) {
		printf("apricot: refused: texture updates or clears are waiting to go on another queue, and this instance submits on one\n");
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	}
	// Closed by a submission that failed: nothing more can be recorded into
	// it, so it goes on its own now.
	if (instance->upload_batch && !instance->upload_batch_open) {
		uint64_t unused = 0;
		SPUDRESULT sr   = aprend_instance_flush_upload_batch(instance, &unused);
		if (SPUDFAIL(sr))
			return sr;
	}
	if (!instance->upload_batch) {
		spudgpu_command_list list = nullptr;
		SPUDRESULT sr             = aprend_command_pool_take(instance->frame_commands[instance->frame_index].batches, &list);
		if (SPUDFAIL(sr))
			return sr;
		spudgpu_begin_command_list(list);
		instance->upload_batch       = list;
		instance->upload_batch_open  = true;
		instance->upload_batch_queue = queue;
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		instance->upload_batch_waiting = true;
	}
	*out_list = instance->upload_batch;
	return SPUD_SUCCESS;
}

void aprend_instance_collect(aprend_instance instance) {
	{
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		if (instance->release_head == instance->release_queue.size())
			return;
	}
	// 0 if the value can't be read, which releases nothing. Read once: what
	// finishes while this runs is collected by the next call.
	const uint64_t completed = spudgpu_get_fence_value(instance->frame_fence);
	for (;;) {
		aprend_instance_t::release_entry entry{};
		{
			// Another thread may retire something at any moment, so the
			// queue is only touched under the lock, and the entry is taken
			// by value with the head moved past it before the lock goes.
			std::lock_guard<std::mutex> lock(instance->release_mutex);
			if (instance->release_head == instance->release_queue.size()) {
				// Everything taken: start the queue again from empty.
				instance->release_queue.clear();
				instance->release_head = 0;
				return;
			}
			entry = instance->release_queue[instance->release_head];
			if (entry.serial > completed) {
				// Stopping with entries still waiting. A caller that always
				// has work in flight never empties the queue, so the taken
				// entries at its front are dropped here once they are most
				// of it, or it would grow for as long as the instance lives.
				if (instance->release_head >= 64 && instance->release_head * 2 >= instance->release_queue.size()) {
					instance->release_queue.erase(
					    instance->release_queue.begin(), instance->release_queue.begin() + (std::ptrdiff_t)instance->release_head);
					instance->release_head = 0;
				}
				return;
			}
			++instance->release_head;
		}
		// Outside the lock: releasing calls into SpudGPU, and may itself
		// retire something.
		entry.release(entry.object);
	}
}

void aprend_instance_retire(
    aprend_instance instance,
    void *object,
    void (*release)(void *object)) {
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	// Read under the lock, so entries go on in the order of their serials
	// whichever threads they come from. A waiting upload batch will take the
	// next serial, and may use the object.
	const uint64_t serial = instance->submit_serial + (instance->upload_batch_waiting ? 1 : 0);
	try {
		instance->release_queue.push_back({release, object, serial});
	} catch (const std::bad_alloc &) {
		printf("apricot: release queue out of memory: an object destroyed now is left unreleased\n");
	}
}

extern "C" {
aprend_instance aprend_instance_create(const aprend_instance_desc *desc) {
	if (!desc)
		return nullptr;
	if (!desc->device) {
		printf("apricot: aprend_instance_create: device is NULL\n");
		return nullptr;
	}
	if (desc->frames_in_flight == 0) {
		printf("apricot: aprend_instance_create: frames_in_flight is 0\n");
		return nullptr;
	}
	if (desc->frames_in_flight > APREND_MAX_FRAMES_IN_FLIGHT) {
		printf("apricot: aprend_instance_create: frames_in_flight %u is above APREND_MAX_FRAMES_IN_FLIGHT\n", desc->frames_in_flight);
		return nullptr;
	}
	if (desc->transient_chunk_size == 0 && desc->transient_memory_limit != 0) {
		printf("apricot: aprend_instance_create: transient_memory_limit is set and transient_chunk_size is 0\n");
		return nullptr;
	}
	if (desc->transient_memory_limit != 0 && desc->transient_memory_limit < desc->transient_chunk_size) {
		printf("apricot: aprend_instance_create: transient_memory_limit is below transient_chunk_size\n");
		return nullptr;
	}
	aprend_instance_t *result = (aprend_instance_t *)malloc(sizeof(aprend_instance_t));
	if (!result)
		return nullptr;
	result = new (result) aprend_instance_t();

	result->desc = *desc;

	SPUDRESULT sr = SPUD_SUCCESS;

	// Allocators for each frame index, so that one index's can be reset
	// while the GPU still runs what the others recorded.
	for (uint32_t i = 0; i < desc->frames_in_flight; ++i) {
		spudgpu_command_allocator_desc aDesc = {};
		aDesc.flags                          = 0;
		aDesc.type                           = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;

		sr = spudgpu_create_command_allocator(desc->device, &aDesc, &result->frame_commands[i].recordings.allocator);
		if (SPUDFAIL(sr)) {
			result->frame_commands[i].recordings.allocator = nullptr;
			goto failedattempt;
		}
		sr = spudgpu_create_command_allocator(desc->device, &aDesc, &result->frame_commands[i].batches.allocator);
		if (SPUDFAIL(sr)) {
			result->frame_commands[i].batches.allocator = nullptr;
			goto failedattempt;
		}
	}

	{
		SPUDGPU_DEVICE_PROPERTIES properties{};
		sr = spudgpu_get_device_properties(desc->device, &properties);
		if (SPUDFAIL(sr))
			goto failedattempt;
		result->unified_memory         = properties.unified_memory;
		result->device_mappable_memory = properties.device_mappable_memory;
		result->uniform_buffer_offset_alignment = properties.uniform_buffer_offset_alignment;
		result->storage_buffer_offset_alignment = properties.storage_buffer_offset_alignment;
		// 0 from a device that reported nothing is taken as 1: any offset.
		if (result->uniform_buffer_offset_alignment == 0)
			result->uniform_buffer_offset_alignment = 1;
		if (result->storage_buffer_offset_alignment == 0)
			result->storage_buffer_offset_alignment = 1;
		result->material_block_alignment = result->uniform_buffer_offset_alignment > 16 ? result->uniform_buffer_offset_alignment : 16;
		result->layered_rendering      = properties.layered_rendering;
		result->max_draw_indirect_count = properties.max_draw_indirect_count;
	}

	sr = spudgpu_create_fence(desc->device, SPUDGPU_FENCE_FLAG_NONE, 0, &result->frame_fence);
	if (SPUDFAIL(sr)) {
		result->frame_fence = nullptr;
		goto failedattempt;
	}

	return result;
failedattempt:
	printf("apricot: aprend_instance_create failed: %s\n", spudresult_str(sr));
	result->~aprend_instance_t();
	free(result);
	return nullptr;
}
bool aprend_instance_get_desc(
    aprend_instance instance,
    aprend_instance_desc *out_desc) {
	if (instance && out_desc) {
		*out_desc = instance->desc;
		return true;
	} else
		return false;
}

uint32_t aprend_instance_next_frame(aprend_instance instance) {
	if (!instance)
		return 0;
	// The batch is the frame's, recorded into its allocator and reading its
	// transient memory: it goes before the frame ends, if nothing has taken
	// it. If it can't, it is dropped, since the next turn of this index
	// resets what it was recorded into, and what it would have done is lost.
	{
		uint64_t unused = 0;
		SPUDRESULT sr   = aprend_instance_flush_upload_batch(instance, &unused);
		if (SPUDFAIL(sr)) {
			printf("apricot: aprend_instance_next_frame: texture updates or clears waiting to be submitted could not be, and are dropped: %s\n",
			       spudresult_str(sr));
			if (instance->upload_batch_open)
				spudgpu_end_command_list(instance->upload_batch);
			instance->upload_batch       = nullptr;
			instance->upload_batch_open  = false;
			instance->upload_batch_queue = nullptr;
			// What was retired while it waited waits a serial longer than
			// it needs: never too short.
			std::lock_guard<std::mutex> lock(instance->release_mutex);
			instance->upload_batch_waiting = false;
		}
	}

	instance->frame_index = (instance->frame_index + 1) % instance->desc.frames_in_flight;
	++instance->frame_number;

	// Everything submitted the last time this index was current must have
	// finished before its buffer set copies are written again.
	SPUDRESULT sr = aprend_instance_wait_serial(instance, instance->frame_last_serial[instance->frame_index]);
	aprend_instance_collect(instance);
	if (SPUDFAIL(sr))
		printf("apricot: aprend_instance_next_frame: waiting for frame %u failed: %s\n", instance->frame_index, spudresult_str(sr));
	// The transient spans taken the last time this index was current are
	// read by nothing now, and nothing recorded into the index's allocator
	// runs. After a failed wait that isn't known, and both are left as they
	// are: the frame's spans go on from where they were, and its recordings
	// take lists it hasn't handed out yet, so none still running is
	// reopened. Its command memory then grows until a wait succeeds.
	if (!SPUDFAIL(sr)) {
		aprend_transient_arena_begin_frame(instance);
		aprend_render_target_pool_begin_frame(instance);
		aprend_instance_t::command_frame &frame = instance->frame_commands[instance->frame_index];
		aprend_command_pool_reset(frame.recordings, instance->frame_index);
		aprend_command_pool_reset(frame.batches, instance->frame_index);
	}
	return instance->frame_index;
}
uint32_t aprend_instance_get_frame_index(aprend_instance instance) { return instance ? instance->frame_index : 0; }
bool aprend_instance_wait_idle(aprend_instance instance) {
	if (!instance)
		return false;
	// Waiting for everything includes what is recorded and not yet
	// submitted.
	uint64_t serial = 0;
	SPUDRESULT sr   = aprend_instance_flush_upload_batch(instance, &serial);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_instance_wait_idle: texture updates or clears waiting to be submitted could not be: %s\n", spudresult_str(sr));
		return false;
	}
	sr = aprend_instance_wait_serial(instance, serial);
	aprend_instance_collect(instance);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_instance_wait_idle failed: %s\n", spudresult_str(sr));
		return false;
	}
	return true;
}

void aprend_instance_destroy(aprend_instance instance) {
	if (instance) {
		// Everything retired is released by the destructor, so the GPU must
		// be done with all of it first.
		SPUDRESULT sr = aprend_instance_wait_serial(instance, instance->submit_serial.load());
		if (SPUDFAIL(sr))
			printf("apricot: aprend_instance_destroy: waiting for the GPU failed: %s\n", spudresult_str(sr));
		instance->~aprend_instance_t();
		free(instance);
	}
}
} // Extern "C"
