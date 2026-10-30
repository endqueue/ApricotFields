
#include "render/aprendcontext.h"
#include "aprend_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdarg>
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
// list is free again. A failed reset leaves the pool as it was.
static SPUDRESULT aprend_command_pool_reset(aprend_instance_t::command_pool &pool) {
	const SPUDRESULT reset = spudgpu_reset_command_allocator(pool.allocator);
	if (SPUDFAIL(reset))
		return reset;
	pool.used = 0;
	return SPUD_SUCCESS;
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

APRESULT aprend_instance_submit(
    aprend_instance instance,
    spudgpu_command_queue queue,
    spudgpu_command_list *lists,
    uint32_t list_count,
    spudgpu_swap_chain swap_chain,
    uint64_t *out_serial,
    APREND_ERROR *error) {
	// One queue for the life of the instance. On a second queue a serial
	// could be reached while work submitted before it, on the first, was
	// still running, and everything that waits on the fence would be told
	// that work had finished.
	if (instance->submit_queue && queue != instance->submit_queue)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the instance submits on one queue, and this is not the queue of its first submission");
	// The batch was recorded for its queue, and is the instance's first
	// submission when no queue is chosen yet.
	if (instance->upload_batch && queue != instance->upload_batch_queue)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "updates or clears are waiting to go on another queue, and the instance submits on one");
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
		return aprend_fail_spud(error, flushed, "flushing the frame's transient memory failed");
	const uint64_t serial = instance->submit_serial.load() + 1;
	spudgpu_submit_desc submit{};
	submit.base.type = SPUD_STRUCT_TYPE_GPU_SUBMIT_DESC;
	submit.cmd_lists          = all;
	submit.cmd_list_count     = all_count;
	submit.signal_fence       = instance->frame_fence;
	submit.signal_fence_value = serial;
	submit.swap_chain         = swap_chain;
	SPUDRESULT sr             = spudgpu_queue_submit(queue, &submit);
	if (SPUDFAIL(sr))
		return aprend_fail_spud(error, sr, "submitting failed");
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
	return APRESULT_OK;
}

SPUDRESULT aprend_instance_wait_serial(
    aprend_instance instance,
    uint64_t serial,
    uint64_t timeout_ns) {
	// 0 is no submission: the fence started there, so there is nothing to
	// wait for, and a wait for it is one a backend may warn about.
	if (serial == 0)
		return SPUD_SUCCESS;
	return spudgpu_wait_for_fences(instance->desc.device, &instance->frame_fence, &serial, 1, true, timeout_ns);
}

SPUDRESULT aprend_instance_take_command_list(
    aprend_instance instance,
    spudgpu_command_list *out_list) {
	return aprend_command_pool_take(instance->frame_commands[instance->frame_index].recordings, out_list);
}

APRESULT aprend_instance_flush_upload_batch(
    aprend_instance instance,
    uint64_t *out_serial,
    APREND_ERROR *error) {
	if (!instance->upload_batch) {
		*out_serial = instance->submit_serial.load();
		return APRESULT_OK;
	}
	return aprend_instance_submit(instance, instance->upload_batch_queue, nullptr, 0, nullptr, out_serial, error);
}

APRESULT aprend_instance_open_upload_batch(
    aprend_instance instance,
    spudgpu_command_queue queue,
    spudgpu_command_list *out_list,
    APREND_ERROR *error) {
	if (instance->submit_queue && queue != instance->submit_queue)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the instance submits on one queue, and this is not the queue of its first submission");
	if (instance->upload_batch && queue != instance->upload_batch_queue)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "updates or clears are waiting to go on another queue, and the instance submits on one");
	// Closed by a submission that failed: nothing more can be recorded into
	// it, so it goes on its own now.
	if (instance->upload_batch && !instance->upload_batch_open) {
		uint64_t unused = 0;
		if (const APRESULT flushed = aprend_instance_flush_upload_batch(instance, &unused, error))
			return flushed;
	}
	if (!instance->upload_batch) {
		spudgpu_command_list list = nullptr;
		SPUDRESULT sr             = aprend_command_pool_take(instance->frame_commands[instance->frame_index].batches, &list);
		if (SPUDFAIL(sr))
			return aprend_fail_spud(error, sr, "taking a command list for the batch of updates and clears failed");
		spudgpu_begin_command_list(list);
		instance->upload_batch       = list;
		instance->upload_batch_open  = true;
		instance->upload_batch_queue = queue;
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		instance->upload_batch_waiting = true;
	}
	*out_list = instance->upload_batch;
	return APRESULT_OK;
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

APRESULT aprend_fail(APREND_ERROR *error, APRESULT result, const char *format, ...) {
	if (error) {
		va_list args;
		va_start(args, format);
		vsnprintf(error->message, sizeof(error->message), format, args);
		va_end(args);
	}
	return result;
}

APRESULT aprend_result_from_spud(SPUDRESULT sr) {
	switch (sr) {
	case SPUD_SUCCESS:
		return APRESULT_OK;
	case SPUDRESULT_OUT_OF_MEMORY:
		return APRESULT_OUT_OF_MEMORY;
	case SPUDRESULT_GPU_FEATURE_NOT_SUPPORTED:
	case SPUDRESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED:
	case SPUDRESULT_GPU_EXT_DEPTH_BOUNDS_TEST_NOT_SUPPORTED:
	case SPUDRESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED:
	case SPUDRESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED:
	case SPUDRESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED:
	case SPUDRESULT_GPU_MEMORY_KIND_NOT_SUPPORTED:
	case SPUDRESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED:
	case SPUDRESULT_GPU_TIMESTAMPS_NOT_SUPPORTED:
		return APRESULT_UNSUPPORTED;
	case SPUDRESULT_GPU_OUT_OF_DESCRIPTORS:
	case SPUDRESULT_GPU_BINDLESS_OUT_OF_SLOTS:
		return APRESULT_LIMIT;
	case SPUDRESULT_GPU_FENCE_WAIT_TIMED_OUT:
		return APRESULT_TIMEOUT;
	case SPUDRESULT_GPU_DEVICE_LOST:
		return APRESULT_APREND_DEVICE_LOST;
	default:
		return APRESULT_APREND_GPU_FAILURE;
	}
}

APRESULT aprend_fail_spud(APREND_ERROR *error, SPUDRESULT sr, const char *format, ...) {
	if (error) {
		va_list args;
		va_start(args, format);
		const int written = vsnprintf(error->message, sizeof(error->message), format, args);
		va_end(args);
		// Truncated already: SpudGPU's name would not fit after it.
		if (written >= 0 && (size_t)written < sizeof(error->message))
			snprintf(error->message + written, sizeof(error->message) - (size_t)written, ": %s", spudresult_str(sr));
	}
	return aprend_result_from_spud(sr);
}

APRESULT aprend_instance_reserve_release(
    aprend_instance instance,
    uint32_t count) {
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	const size_t needed = instance->release_queue.size() + instance->release_promised + count;
	if (instance->release_queue.capacity() < needed) {
		// Doubling, so a run of creates grows it a logarithmic number of
		// times, as push_back would.
		const size_t doubled = instance->release_queue.capacity() * 2;
		try {
			instance->release_queue.reserve(needed > doubled ? needed : doubled);
		} catch (const std::bad_alloc &) {
			return APRESULT_OUT_OF_MEMORY;
		}
	}
	instance->release_promised += count;
	return APRESULT_OK;
}

void aprend_instance_unreserve_release(
    aprend_instance instance,
    uint32_t count) {
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	instance->release_promised -= count < instance->release_promised ? count : instance->release_promised;
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
	// Every retire was promised by the create, resize, upload or write that
	// made what it releases (aprend_instance_reserve_release), so the queue
	// has room and push_back below doesn't allocate. One that wasn't is a
	// bug in Aprend: the Debug build stops on it.
	assert(instance->release_promised > 0);
	if (instance->release_promised > 0)
		--instance->release_promised;
	try {
		instance->release_queue.push_back({release, object, serial});
	} catch (const std::bad_alloc &) {
		// Only an unpromised retire can get here, which the assert above
		// has already stopped in Debug: the object is left unreleased.
	}
}

extern "C" {
SPUDGPU_FEATURES aprend_get_optional_gpu_features(void) {
	// The features only Aprend's own code uses, each with a path for a
	// device without it (aprend_internal.hpp, aprend_instance_t::features).
	return SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT | SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY;
}
APRESULT aprend_instance_create(
    const APREND_INSTANCE_DESC *desc,
    aprend_instance *out_instance,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_instance)
		*out_instance = nullptr;
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_INSTANCE_DESC))
		return base;
	if (!desc->device)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "device is NULL");
	if (desc->frames_in_flight == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "frames_in_flight is 0");
	if (desc->frames_in_flight > APREND_MAX_FRAMES_IN_FLIGHT)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "frames_in_flight %u is above APREND_MAX_FRAMES_IN_FLIGHT (%u)", desc->frames_in_flight,
		                   (unsigned)APREND_MAX_FRAMES_IN_FLIGHT);
	if (desc->transient_chunk_size == 0 && desc->transient_memory_limit != 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "transient_memory_limit is set and transient_chunk_size is 0");
	if (desc->transient_memory_limit != 0 && desc->transient_memory_limit < desc->transient_chunk_size)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "transient_memory_limit %llu is below transient_chunk_size %llu",
		                   (unsigned long long)desc->transient_memory_limit, (unsigned long long)desc->transient_chunk_size);
	if (!out_instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_instance is NULL");

	aprend_instance_t *result = (aprend_instance_t *)malloc(sizeof(aprend_instance_t));
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the instance");
	result = new (result) aprend_instance_t();

	result->desc = *desc;

	SPUDRESULT sr     = SPUD_SUCCESS;
	const char *doing = "";

	// Allocators for each frame index, so that one index's can be reset
	// while the GPU still runs what the others recorded.
	for (uint32_t i = 0; i < desc->frames_in_flight; ++i) {
		spudgpu_command_allocator_desc aDesc = {};
		aDesc.base.type = SPUD_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
		aDesc.flags                          = 0;
		aDesc.type                           = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;

		sr = spudgpu_create_command_allocator(desc->device, &aDesc, &result->frame_commands[i].recordings.allocator);
		if (SPUDFAIL(sr)) {
			result->frame_commands[i].recordings.allocator = nullptr;
			doing = "creating a direct command allocator";
			goto failedattempt;
		}
		sr = spudgpu_create_command_allocator(desc->device, &aDesc, &result->frame_commands[i].batches.allocator);
		if (SPUDFAIL(sr)) {
			result->frame_commands[i].batches.allocator = nullptr;
			doing = "creating a direct command allocator";
			goto failedattempt;
		}
	}

	{
		// Features from the device, which has only those it was created
		// with; limits and properties from its adapter.
		result->features = spudgpu_get_device_features(desc->device);
		SPUDGPU_ADAPTER_PROPERTIES properties{};
		properties.base.type = SPUD_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
		sr = spudgpu_get_adapter_properties(spudgpu_get_device_adapter(desc->device), &properties);
		if (SPUDFAIL(sr)) {
			doing = "reading the device's adapter properties";
			goto failedattempt;
		}
		result->unified_memory         = properties.unified_memory;
		result->device_mappable_memory = (result->features & SPUDGPU_FEATURE_DEVICE_MAPPABLE_MEMORY) != 0;
		result->uniform_buffer_offset_alignment = properties.uniform_buffer_offset_alignment;
		result->storage_buffer_offset_alignment = properties.storage_buffer_offset_alignment;
		// 0 from a device that reported nothing is taken as 1: any offset.
		if (result->uniform_buffer_offset_alignment == 0)
			result->uniform_buffer_offset_alignment = 1;
		if (result->storage_buffer_offset_alignment == 0)
			result->storage_buffer_offset_alignment = 1;
		result->material_block_alignment = result->uniform_buffer_offset_alignment > 16 ? result->uniform_buffer_offset_alignment : 16;
		// The adapter's most draws a call counts only on a device created
		// with multi-draw indirect; one a call without it.
		result->max_draw_indirect_count = (result->features & SPUDGPU_FEATURE_MULTI_DRAW_INDIRECT) ? properties.max_draw_indirect_count : 1;
	}

	sr = spudgpu_create_fence(desc->device, SPUDGPU_FENCE_FLAG_NONE, 0, &result->frame_fence);
	if (SPUDFAIL(sr)) {
		result->frame_fence = nullptr;
		doing = "creating the frame fence";
		goto failedattempt;
	}

	*out_instance = result;
	return APRESULT_OK;
failedattempt:
	result->~aprend_instance_t();
	free(result);
	return aprend_fail_spud(out_error, sr, "%s failed", doing);
}
APRESULT aprend_instance_get_desc(
    aprend_instance instance,
    APREND_INSTANCE_DESC *out_desc) {
	if (!instance)
		return APRESULT_INVALID_ARGUMENT;
	if (APRI_STALE(instance, "aprend_instance_get_desc", "instance"))
		return APRESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return APRESULT_INVALID_ARGUMENT;
	*out_desc           = instance->desc;
	out_desc->base.type = APRI_STRUCT_TYPE_APREND_INSTANCE_DESC;
	out_desc->base.next = nullptr;
	return APRESULT_OK;
}

APRESULT aprend_instance_next_frame(
    aprend_instance instance,
    uint64_t timeout_ns,
    uint32_t *out_frame_index,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_instance_next_frame", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_instance_next_frame");
	// The batch is the frame's, recorded into its allocator and reading its
	// transient memory: it goes before the frame ends, if nothing has taken
	// it. If it can't, nothing else is done: the frame stays, and the batch
	// waits, closed, for the next submission.
	{
		uint64_t unused = 0;
		if (const APRESULT flushed = aprend_instance_flush_upload_batch(instance, &unused, out_error))
			return flushed;
	}

	// Everything submitted the last time the next index was current must
	// have finished before its buffer set copies are written again. Waited
	// for before anything changes, so a wait that runs out or fails leaves
	// the instance in this frame.
	const uint32_t next = (instance->frame_index + 1) % instance->desc.frames_in_flight;
	SPUDRESULT sr       = aprend_instance_wait_serial(instance, instance->frame_last_serial[next], timeout_ns);
	aprend_instance_collect(instance);
	if (sr == SPUDRESULT_GPU_FENCE_WAIT_TIMED_OUT)
		return aprend_fail(out_error, APRESULT_TIMEOUT, "frame %u's work had not finished within the time limit", next);
	if (SPUDFAIL(sr))
		return aprend_fail_spud(out_error, sr, "waiting for frame %u failed", next);

	// The memory of every recording made the last time the index was
	// current comes back: nothing recorded into its allocators runs.
	aprend_instance_t::command_frame &frame = instance->frame_commands[next];
	sr                                      = aprend_command_pool_reset(frame.recordings);
	if (!SPUDFAIL(sr))
		sr = aprend_command_pool_reset(frame.batches);
	if (SPUDFAIL(sr))
		return aprend_fail_spud(out_error, sr, "resetting a command allocator of frame %u failed", next);

	instance->frame_index = next;
	++instance->frame_number;
	// The transient spans and pooled targets taken the last time this index
	// was current are read by nothing now.
	aprend_transient_arena_begin_frame(instance);
	aprend_render_target_pool_begin_frame(instance);
	if (out_frame_index)
		*out_frame_index = next;
	return APRESULT_OK;
}
uint32_t aprend_instance_get_frame_index(aprend_instance instance) {
	if (!instance)
		return 0;
	if (APRI_STALE(instance, "aprend_instance_get_frame_index", "instance"))
		return 0;
	return instance->frame_index;
}
APRESULT aprend_instance_wait_idle(
    aprend_instance instance,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_instance_wait_idle", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_instance_wait_idle");
	// Waiting for everything includes what is recorded and not yet
	// submitted.
	uint64_t serial = 0;
	if (const APRESULT flushed = aprend_instance_flush_upload_batch(instance, &serial, out_error))
		return flushed;
	const SPUDRESULT sr = aprend_instance_wait_serial(instance, serial, UINT64_MAX);
	aprend_instance_collect(instance);
	if (SPUDFAIL(sr))
		return aprend_fail_spud(out_error, sr, "waiting for submission %llu failed", (unsigned long long)serial);
	return APRESULT_OK;
}

void aprend_instance_destroy(aprend_instance instance) {
	if (instance && APRI_STALE(instance, "aprend_instance_destroy", "instance"))
		return;
	if (instance) {
		// Everything retired is released by the destructor, so the GPU must
		// be done with all of it first.
		// A wait that fails (a lost device) has nothing left to wait for:
		// what the device won't run, it won't read.
		(void)aprend_instance_wait_serial(instance, instance->submit_serial.load(), UINT64_MAX);
		instance->~aprend_instance_t();
		free(instance);
	}
}
} // Extern "C"
