
#include "render/rbtrendcontext.h"
#include "rbtrend_internal.hpp"
#include "rbtrendraytracing_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

// The lists go before their allocator.
static void rbtrend_command_pool_destroy(rbtrend_instance_t::command_pool &pool) {
	for (crgpu_command_list list : pool.lists)
		crgpu_command_list_destroy(list);
	if (pool.allocator)
		crgpu_command_allocator_destroy(pool.allocator);
}

static CR_RESULT rbtrend_command_pool_take(
    rbtrend_instance_t::command_pool &pool,
    crgpu_command_list *out_list) {
	if (pool.used == pool.lists.size()) {
		crgpu_command_list list = nullptr;
		CR_RESULT sr             = crgpu_command_list_create(pool.allocator, &list);
		if (CR_FAILED(sr))
			return sr;
		// The push can only fail for want of memory. No exception leaves a
		// C entry point, and this is reached from one.
		try {
			pool.lists.push_back(list);
		} catch (...) {
			crgpu_command_list_destroy(list);
			return CR_RESULT_OUT_OF_MEMORY;
		}
	}
	*out_list = pool.lists[pool.used++];
	return CR_SUCCESS;
}

// Once nothing recorded into the pool runs: its memory comes back, and every
// list is free again. A failed reset leaves the pool as it was.
static CR_RESULT rbtrend_command_pool_reset(rbtrend_instance_t::command_pool &pool) {
	const CR_RESULT reset = crgpu_command_allocator_reset(pool.allocator);
	if (CR_FAILED(reset))
		return reset;
	pool.used = 0;
	return CR_SUCCESS;
}

rbtrend_instance_t::~rbtrend_instance_t() {
	// A batch of updates and clears never submitted is closed before
	// anything it recorded is released: a list still recording when an
	// object it uses goes can't be ended (Vulkan's command buffer is then
	// invalid), and a Metal encoder is ended before it goes. It is dropped
	// with the lists, which go before their allocators.
	if (this->upload_batch && this->upload_batch_open) {
		crgpu_command_list_end(this->upload_batch);
		this->upload_batch_open = false;
	}
	// Onto the release queue first, so the loop below releases them in
	// order with everything else.
	rbtrend_rt_state_destroy(this);
	rbtrend_builtins_destroy(this);
	rbtrend_render_target_pool_destroy(this);
	// Whatever is still queued goes now: the instance is the last thing that
	// could release it. rbtrend_instance_destroy has waited for the GPU. A
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
	// back: the arena's buffers are all that is left of it. Each lit material
	// has given its slot back to ray tracing's tables, which go now.
	rbtrend_rt_state_free(this);
	rbtrend_material_arena_destroy(this);
	// Nothing reads a transient span any more.
	rbtrend_transient_arena_destroy(this);
	// An instance that failed to create may have neither.
	if (this->frame_fence)
		crgpu_fence_destroy(this->frame_fence);
	for (command_frame &frame : this->frame_commands) {
		rbtrend_command_pool_destroy(frame.recordings);
		rbtrend_command_pool_destroy(frame.batches);
	}
}

RBT_RESULT rbtrend_instance_submit(
    rbtrend_instance instance,
    crgpu_command_queue queue,
    crgpu_command_list *lists,
    uint32_t list_count,
    crgpu_swap_chain swap_chain,
    uint64_t *out_serial,
    RBTREND_ERROR *error) {
	// One queue for the life of the instance. On a second queue a serial
	// could be reached while work submitted before it, on the first, was
	// still running, and everything that waits on the fence would be told
	// that work had finished.
	if (instance->submit_queue && queue != instance->submit_queue)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "the instance submits on one queue, and this is not the queue of its first submission");
	// The batch was recorded for its queue, and is the instance's first
	// submission when no queue is chosen yet.
	if (instance->upload_batch && queue != instance->upload_batch_queue)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "updates or clears are waiting to go on another queue, and the instance submits on one");
	assert(list_count <= RBTREND_SUBMIT_MAX_LISTS);
	assert(list_count > 0 || instance->upload_batch);

	// The batch goes first, in this submission: what it writes is what the
	// lists after it read.
	crgpu_command_list all[RBTREND_SUBMIT_MAX_LISTS + 1];
	uint32_t all_count = 0;
	if (instance->upload_batch) {
		if (instance->upload_batch_open) {
			crgpu_command_list_end(instance->upload_batch);
			instance->upload_batch_open = false;
		}
		all[all_count++] = instance->upload_batch;
	}
	for (uint32_t i = 0; i < list_count; ++i)
		all[all_count++] = lists[i];

	// What was written into the frame's transient spans, which the lists
	// may read. A failed flush fails the submission: the GPU would read
	// bytes that may not have reached it.
	CR_RESULT flushed = rbtrend_transient_arena_flush(instance);
	if (CR_FAILED(flushed))
		return rbtrend_fail_cr(error, flushed, "flushing the frame's transient memory failed");
	const uint64_t serial = instance->submit_serial.load() + 1;
	crgpu_submit_desc submit{};
	submit.base.type = CR_STRUCT_TYPE_GPU_SUBMIT_DESC;
	submit.cmd_lists          = all;
	submit.cmd_list_count     = all_count;
	submit.signal_fence       = instance->frame_fence;
	submit.signal_fence_value = serial;
	submit.swap_chain         = swap_chain;
	CR_RESULT sr             = crgpu_command_queue_submit(queue, &submit);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(error, sr, "submitting failed");
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
	rbtrend_instance_collect(instance);
	return RBT_RESULT_OK;
}

CR_RESULT rbtrend_instance_wait_serial(
    rbtrend_instance instance,
    uint64_t serial,
    uint64_t timeout_ns) {
	// 0 is no submission: the fence started there, so there is nothing to
	// wait for, and a wait for it is one a backend may warn about.
	if (serial == 0)
		return CR_SUCCESS;
	return crgpu_fence_wait(instance->desc.device, &instance->frame_fence, &serial, 1, true, timeout_ns);
}

CR_RESULT rbtrend_instance_take_command_list(
    rbtrend_instance instance,
    crgpu_command_list *out_list) {
	return rbtrend_command_pool_take(instance->frame_commands[instance->frame_index].recordings, out_list);
}

RBT_RESULT rbtrend_instance_flush_upload_batch(
    rbtrend_instance instance,
    uint64_t *out_serial,
    RBTREND_ERROR *error) {
	if (!instance->upload_batch) {
		*out_serial = instance->submit_serial.load();
		return RBT_RESULT_OK;
	}
	return rbtrend_instance_submit(instance, instance->upload_batch_queue, nullptr, 0, nullptr, out_serial, error);
}

RBT_RESULT rbtrend_instance_open_upload_batch(
    rbtrend_instance instance,
    crgpu_command_queue queue,
    crgpu_command_list *out_list,
    RBTREND_ERROR *error) {
	if (instance->submit_queue && queue != instance->submit_queue)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "the instance submits on one queue, and this is not the queue of its first submission");
	if (instance->upload_batch && queue != instance->upload_batch_queue)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "updates or clears are waiting to go on another queue, and the instance submits on one");
	// Closed by a submission that failed: nothing more can be recorded into
	// it, so it goes on its own now.
	if (instance->upload_batch && !instance->upload_batch_open) {
		uint64_t unused = 0;
		if (const RBT_RESULT flushed = rbtrend_instance_flush_upload_batch(instance, &unused, error))
			return flushed;
	}
	if (!instance->upload_batch) {
		crgpu_command_list list = nullptr;
		CR_RESULT sr             = rbtrend_command_pool_take(instance->frame_commands[instance->frame_index].batches, &list);
		if (CR_FAILED(sr))
			return rbtrend_fail_cr(error, sr, "taking a command list for the batch of updates and clears failed");
		crgpu_command_list_begin(list);
		instance->upload_batch       = list;
		instance->upload_batch_open  = true;
		instance->upload_batch_queue = queue;
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		instance->upload_batch_waiting = true;
	}
	*out_list = instance->upload_batch;
	return RBT_RESULT_OK;
}

void rbtrend_instance_collect(rbtrend_instance instance) {
	{
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		if (instance->release_head == instance->release_queue.size())
			return;
	}
	// 0 if the value can't be read, which releases nothing. Read once: what
	// finishes while this runs is collected by the next call.
	const uint64_t completed = crgpu_fence_get_value(instance->frame_fence);
	for (;;) {
		rbtrend_instance_t::release_entry entry{};
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
		// Outside the lock: releasing calls into Carrottus GPU, and may itself
		// retire something.
		entry.release(entry.object);
	}
}

RBT_RESULT rbtrend_fail(RBTREND_ERROR *error, RBT_RESULT result, const char *format, ...) {
	if (error) {
		va_list args;
		va_start(args, format);
		vsnprintf(error->message, sizeof(error->message), format, args);
		va_end(args);
	}
	return result;
}

RBT_RESULT rbtrend_result_from_cr(CR_RESULT sr) {
	switch (sr) {
	case CR_SUCCESS:
		return RBT_RESULT_OK;
	case CR_RESULT_OUT_OF_MEMORY:
		return RBT_RESULT_OUT_OF_MEMORY;
	case CR_RESULT_GPU_FEATURE_NOT_SUPPORTED:
	case CR_RESULT_GPU_EXT_BINDLESS_NOT_SUPPORTED:
	case CR_RESULT_GPU_EXT_DEPTH_BOUNDS_TEST_NOT_SUPPORTED:
	case CR_RESULT_GPU_EXT_MESH_SHADING_NOT_SUPPORTED:
	case CR_RESULT_GPU_DEPTH_BIAS_CLAMP_NOT_SUPPORTED:
	case CR_RESULT_GPU_INDEPENDENT_BLEND_NOT_SUPPORTED:
	case CR_RESULT_GPU_MEMORY_KIND_NOT_SUPPORTED:
	case CR_RESULT_GPU_PRESENT_SCALING_NOT_SUPPORTED:
	case CR_RESULT_GPU_TIMESTAMPS_NOT_SUPPORTED:
		return RBT_RESULT_UNSUPPORTED;
	case CR_RESULT_GPU_OUT_OF_DESCRIPTORS:
	case CR_RESULT_GPU_BINDLESS_OUT_OF_SLOTS:
		return RBT_RESULT_LIMIT;
	case CR_RESULT_GPU_FENCE_WAIT_TIMED_OUT:
		return RBT_RESULT_TIMEOUT;
	case CR_RESULT_GPU_DEVICE_LOST:
		return RBT_RESULT_REND_DEVICE_LOST;
	default:
		return RBT_RESULT_REND_GPU_FAILURE;
	}
}

RBT_RESULT rbtrend_fail_cr(RBTREND_ERROR *error, CR_RESULT sr, const char *format, ...) {
	if (error) {
		va_list args;
		va_start(args, format);
		const int written = vsnprintf(error->message, sizeof(error->message), format, args);
		va_end(args);
		// Truncated already: Carrottus GPU's name would not fit after it.
		if (written >= 0 && (size_t)written < sizeof(error->message))
			snprintf(error->message + written, sizeof(error->message) - (size_t)written, ": %s", cr_result_str(sr));
	}
	return rbtrend_result_from_cr(sr);
}

RBT_RESULT rbtrend_instance_reserve_release(
    rbtrend_instance instance,
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
			return RBT_RESULT_OUT_OF_MEMORY;
		}
	}
	instance->release_promised += count;
	return RBT_RESULT_OK;
}

void rbtrend_instance_unreserve_release(
    rbtrend_instance instance,
    uint32_t count) {
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	instance->release_promised -= count < instance->release_promised ? count : instance->release_promised;
}

void rbtrend_instance_retire(
    rbtrend_instance instance,
    void *object,
    void (*release)(void *object)) {
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	// Read under the lock, so entries go on in the order of their serials
	// whichever threads they come from. A waiting upload batch will take the
	// next serial, and may use the object.
	const uint64_t serial = instance->submit_serial + (instance->upload_batch_waiting ? 1 : 0);
	// Every retire was promised by the create, resize, upload or write that
	// made what it releases (rbtrend_instance_reserve_release), so the queue
	// has room and push_back below doesn't allocate. One that wasn't is a
	// bug in Rend: the Debug build stops on it.
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
CRGPU_FEATURES rbtrend_get_optional_gpu_features(void) {
	// The features only Rend's own code uses, each with a path for a
	// device without it (rbtrend_internal.hpp, rbtrend_instance_t::features).
	return CRGPU_FEATURE_MULTI_DRAW_INDIRECT | CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY;
}
RBT_RESULT rbtrend_instance_create(
    const RBTREND_INSTANCE_DESC *desc,
    rbtrend_instance *out_instance,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_instance)
		*out_instance = nullptr;
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_INSTANCE_DESC))
		return base;
	if (!desc->device)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "device is NULL");
	if (desc->frames_in_flight == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "frames_in_flight is 0");
	if (desc->frames_in_flight > RBTREND_MAX_FRAMES_IN_FLIGHT)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "frames_in_flight %u is above RBTREND_MAX_FRAMES_IN_FLIGHT (%u)", desc->frames_in_flight,
		                   (unsigned)RBTREND_MAX_FRAMES_IN_FLIGHT);
	if (desc->transient_chunk_size == 0 && desc->transient_memory_limit != 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "transient_memory_limit is set and transient_chunk_size is 0");
	if (desc->transient_memory_limit != 0 && desc->transient_memory_limit < desc->transient_chunk_size)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "transient_memory_limit %llu is below transient_chunk_size %llu",
		                   (unsigned long long)desc->transient_memory_limit, (unsigned long long)desc->transient_chunk_size);
	if (!out_instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_instance is NULL");

	rbtrend_instance_t *result = (rbtrend_instance_t *)malloc(sizeof(rbtrend_instance_t));
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the instance");
	result = new (result) rbtrend_instance_t();

	result->desc = *desc;

	CR_RESULT sr     = CR_SUCCESS;
	const char *doing = "";

	// Allocators for each frame index, so that one index's can be reset
	// while the GPU still runs what the others recorded.
	for (uint32_t i = 0; i < desc->frames_in_flight; ++i) {
		crgpu_command_allocator_desc aDesc = {};
		aDesc.base.type = CR_STRUCT_TYPE_GPU_COMMAND_ALLOCATOR_DESC;
		aDesc.flags                          = 0;
		aDesc.type                           = CRGPU_COMMAND_LIST_TYPE_DIRECT;

		sr = crgpu_command_allocator_create(desc->device, &aDesc, &result->frame_commands[i].recordings.allocator);
		if (CR_FAILED(sr)) {
			result->frame_commands[i].recordings.allocator = nullptr;
			doing = "creating a direct command allocator";
			goto failedattempt;
		}
		sr = crgpu_command_allocator_create(desc->device, &aDesc, &result->frame_commands[i].batches.allocator);
		if (CR_FAILED(sr)) {
			result->frame_commands[i].batches.allocator = nullptr;
			doing = "creating a direct command allocator";
			goto failedattempt;
		}
	}

	{
		// Features from the device, which has only those it was created
		// with; limits and properties from its adapter.
		result->features = crgpu_device_get_features(desc->device);
		CRGPU_ADAPTER_PROPERTIES properties{};
		properties.base.type = CR_STRUCT_TYPE_GPU_ADAPTER_PROPERTIES;
		sr = crgpu_adapter_get_properties(crgpu_device_get_adapter(desc->device), &properties);
		if (CR_FAILED(sr)) {
			doing = "reading the device's adapter properties";
			goto failedattempt;
		}
		result->unified_memory         = properties.unified_memory;
		for (CRGPU_SHADER_FORMAT format = CRGPU_SHADER_FORMAT_SPIRV; format <= CRGPU_SHADER_FORMAT_METALLIB; ++format)
			if (properties.shader_formats & (1u << format))
				result->shader_format = format;
		result->device_mappable_memory = (result->features & CRGPU_FEATURE_DEVICE_MAPPABLE_MEMORY) != 0;
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
		result->max_draw_indirect_count = (result->features & CRGPU_FEATURE_MULTI_DRAW_INDIRECT) ? properties.max_draw_indirect_count : 1;
	}

	sr = crgpu_fence_create(desc->device, CRGPU_FENCE_FLAG_NONE, 0, &result->frame_fence);
	if (CR_FAILED(sr)) {
		result->frame_fence = nullptr;
		doing = "creating the frame fence";
		goto failedattempt;
	}

	*out_instance = result;
	return RBT_RESULT_OK;
failedattempt:
	result->~rbtrend_instance_t();
	free(result);
	return rbtrend_fail_cr(out_error, sr, "%s failed", doing);
}
RBT_RESULT rbtrend_instance_get_desc(
    rbtrend_instance instance,
    RBTREND_INSTANCE_DESC *out_desc) {
	if (!instance)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(instance, "rbtrend_instance_get_desc", "instance"))
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return RBT_RESULT_INVALID_ARGUMENT;
	*out_desc           = instance->desc;
	out_desc->base.type = RBT_STRUCT_TYPE_REND_INSTANCE_DESC;
	out_desc->base.next = nullptr;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_instance_next_frame(
    rbtrend_instance instance,
    uint64_t timeout_ns,
    uint32_t *out_frame_index,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_instance_next_frame", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_instance_next_frame");
	// The batch is the frame's, recorded into its allocator and reading its
	// transient memory: it goes before the frame ends, if nothing has taken
	// it. If it can't, nothing else is done: the frame stays, and the batch
	// waits, closed, for the next submission.
	{
		uint64_t unused = 0;
		if (const RBT_RESULT flushed = rbtrend_instance_flush_upload_batch(instance, &unused, out_error))
			return flushed;
	}

	// Everything submitted the last time the next index was current must
	// have finished before its buffer set copies are written again. Waited
	// for before anything changes, so a wait that runs out or fails leaves
	// the instance in this frame.
	const uint32_t next = (instance->frame_index + 1) % instance->desc.frames_in_flight;
	CR_RESULT sr       = rbtrend_instance_wait_serial(instance, instance->frame_last_serial[next], timeout_ns);
	rbtrend_instance_collect(instance);
	if (sr == CR_RESULT_GPU_FENCE_WAIT_TIMED_OUT)
		return rbtrend_fail(out_error, RBT_RESULT_TIMEOUT, "frame %u's work had not finished within the time limit", next);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(out_error, sr, "waiting for frame %u failed", next);

	// The memory of every recording made the last time the index was
	// current comes back: nothing recorded into its allocators runs.
	rbtrend_instance_t::command_frame &frame = instance->frame_commands[next];
	sr                                      = rbtrend_command_pool_reset(frame.recordings);
	if (!CR_FAILED(sr))
		sr = rbtrend_command_pool_reset(frame.batches);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(out_error, sr, "resetting a command allocator of frame %u failed", next);

	instance->frame_index = next;
	++instance->frame_number;
	// The transient spans and pooled targets taken the last time this index
	// was current are read by nothing now.
	rbtrend_transient_arena_begin_frame(instance);
	rbtrend_render_target_pool_begin_frame(instance);
	if (out_frame_index)
		*out_frame_index = next;
	return RBT_RESULT_OK;
}
uint32_t rbtrend_instance_get_frame_index(rbtrend_instance instance) {
	if (!instance)
		return 0;
	if (RBT_STALE(instance, "rbtrend_instance_get_frame_index", "instance"))
		return 0;
	return instance->frame_index;
}
RBT_RESULT rbtrend_instance_wait_idle(
    rbtrend_instance instance,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_instance_wait_idle", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_instance_wait_idle");
	// Waiting for everything includes what is recorded and not yet
	// submitted.
	uint64_t serial = 0;
	if (const RBT_RESULT flushed = rbtrend_instance_flush_upload_batch(instance, &serial, out_error))
		return flushed;
	const CR_RESULT sr = rbtrend_instance_wait_serial(instance, serial, UINT64_MAX);
	rbtrend_instance_collect(instance);
	if (CR_FAILED(sr))
		return rbtrend_fail_cr(out_error, sr, "waiting for submission %llu failed", (unsigned long long)serial);
	return RBT_RESULT_OK;
}

void rbtrend_instance_destroy(rbtrend_instance instance) {
	if (instance && RBT_STALE(instance, "rbtrend_instance_destroy", "instance"))
		return;
	if (instance) {
		// Everything retired is released by the destructor, so the GPU must
		// be done with all of it first.
		// A wait that fails (a lost device) has nothing left to wait for:
		// what the device won't run, it won't read.
		(void)rbtrend_instance_wait_serial(instance, instance->submit_serial.load(), UINT64_MAX);
		instance->~rbtrend_instance_t();
		free(instance);
	}
}
} // Extern "C"
