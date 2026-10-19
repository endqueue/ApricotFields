
#include "render/aprendbuffers.h"
#include "aprend_internal.hpp"

#include <spudgpu.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>

// Vertex and index pools (render/aprendbuffers.h). Both are an
// aprend_pool_core (aprendpools_internal.hpp); everything up to the C entry
// points is written against that and asks the core which kind it is only
// where a block's buffer is made, destroyed or written.

static const char *aprend_pool_element_name(const aprend_pool_core *core) { return core->is_index ? "indices" : "vertices"; }

// A block's buffer of [count] elements, unwritten. NULL if the device
// refuses it. Written once a range, by the pool alone: a ONCE store stages
// each range in a buffer of its own and keeps no staging copy of the block,
// and refuses the caller's update calls.
static void *aprend_pool_buffer_create(aprend_pool_core *core, uint32_t count) {
	if (core->is_index)
		return aprend_index_buffer_create(core->instance, core->index_stride, count, APREND_BUFFER_WRITES_ONCE, nullptr);
	return aprend_vertex_buffer_create(core->instance, (uint64_t)count * core->element_size, APREND_BUFFER_WRITES_ONCE, nullptr);
}

// Released once the GPU has finished everything submitted so far, as any
// buffer is.
static void aprend_pool_buffer_destroy(aprend_pool_core *core, void *buffer) {
	if (core->is_index)
		aprend_index_buffer_destroy((aprend_index_buffer)buffer);
	else
		aprend_vertex_buffer_destroy((aprend_vertex_buffer)buffer);
}

static aprend_buffer_store *aprend_pool_buffer_store(aprend_pool_core *core, void *buffer) {
	if (core->is_index)
		return &((aprend_index_buffer)buffer)->store;
	return &((aprend_vertex_buffer)buffer)->store;
}

// Makes a block of [count] elements, all free, in the first empty slot.
// Returns the slot, or UINT32_MAX with nothing made if the pool's limit
// doesn't allow it or the device refuses the buffer. Throws std::bad_alloc,
// with nothing made.
static uint32_t aprend_pool_add_block(aprend_pool_core *core, uint32_t count, bool dedicated, const char *who) {
	const uint64_t bytes = (uint64_t)count * core->element_size;
	// block_bytes never passes the limit, so the subtraction can't wrap.
	if (core->memory_limit != 0 && bytes > core->memory_limit - core->block_bytes) {
		printf("apricot: %s: a block of %llu bytes more would pass the pool's memory_limit\n", who, (unsigned long long)bytes);
		return UINT32_MAX;
	}

	// The slot and its list first: with no memory for either there is no
	// buffer to undo.
	uint32_t slot = UINT32_MAX;
	for (uint32_t i = 0; i < (uint32_t)core->blocks.size(); ++i) {
		if (!core->blocks[i].buffer) {
			slot = i;
			break;
		}
	}
	if (slot == UINT32_MAX) {
		core->blocks.emplace_back();
		slot = (uint32_t)core->blocks.size() - 1;
	}
	aprend_pool_block &block = core->blocks[slot];
	block.ranges.reset(count);

	block.buffer = aprend_pool_buffer_create(core, count);
	if (!block.buffer) {
		printf("apricot: %s: a block of %u %s can't be created\n", who, count, aprend_pool_element_name(core));
		block.ranges.reset(0);
		return UINT32_MAX;
	}
	block.dedicated = dedicated;
	core->block_bytes += bytes;
	return slot;
}

// Destroys the block in [slot] and leaves the slot empty. Nothing of it is
// handed out or waiting to be taken back.
static void aprend_pool_remove_block(aprend_pool_core *core, uint32_t slot) {
	aprend_pool_block &block = core->blocks[slot];
	core->block_bytes -= (uint64_t)block.ranges.size * core->element_size;
	aprend_pool_buffer_destroy(core, block.buffer);
	block.buffer    = nullptr;
	block.dedicated = false;
	block.ranges.reset(0);
}

// An entry for a new range: an unused one, or one more. Its generation is
// already the new range's. Throws std::bad_alloc.
static uint32_t aprend_pool_allocation_take(aprend_pool_core *core) {
	uint32_t id = core->unused_first;
	if (id != APREND_POOL_NO_ALLOCATION) {
		core->unused_first = core->allocations[id].next;
	} else {
		core->allocations.emplace_back();
		id = (uint32_t)core->allocations.size() - 1;
	}
	aprend_pool_allocation &allocation = core->allocations[id];
	allocation.next = APREND_POOL_NO_ALLOCATION;
	// 0 is no generation: after four billion uses of one entry it is
	// stepped over.
	allocation.generation++;
	if (allocation.generation == 0)
		allocation.generation = 1;
	return id;
}

// Puts entry [id] back on the unused list. It keeps its generation, which
// the next range to use it moves on.
static void aprend_pool_allocation_release(aprend_pool_core *core, uint32_t id) {
	aprend_pool_allocation &allocation = core->allocations[id];
	allocation.state   = aprend_pool_allocation::UNUSED;
	allocation.next    = core->unused_first;
	core->unused_first = id;
}

// Takes back every freed range the GPU has finished with: its elements are
// free again, its entry unused, and a block made for it alone is destroyed.
static void aprend_pool_reclaim(aprend_pool_core *core) {
	if (core->pending_first == APREND_POOL_NO_ALLOCATION)
		return;
	// 0 if the value can't be read, which takes back only what was freed
	// before anything was submitted.
	const uint64_t completed = spudgpu_get_fence_value(core->instance->frame_fence);
	while (core->pending_first != APREND_POOL_NO_ALLOCATION) {
		const uint32_t id                  = core->pending_first;
		aprend_pool_allocation &allocation = core->allocations[id];
		if (allocation.serial > completed)
			break;
		aprend_pool_block &block = core->blocks[allocation.block];
		bool given               = false;
		try {
			given = block.ranges.give(allocation.offset, allocation.count);
		} catch (const std::bad_alloc &) {
			// No memory to list it as free. It stays where it is and is
			// tried again at the next call.
			break;
		}
		core->pending_first = allocation.next;
		if (core->pending_first == APREND_POOL_NO_ALLOCATION)
			core->pending_last = APREND_POOL_NO_ALLOCATION;
		// An entry is a range's elements for as long as it is LIVE or
		// FREED, so they can't have been free. Checked all the same: a
		// range list out of step with the entries would hand one range's
		// elements to another.
		if (!given)
			printf("apricot: a pool's free list already held a freed range: %u elements at %u\n", allocation.count, allocation.offset);
		else
			core->used_bytes -= (uint64_t)allocation.count * core->element_size;
		if (block.dedicated)
			aprend_pool_remove_block(core, allocation.block);
		aprend_pool_allocation_release(core, id);
	}
}

// Takes [count] elements from a block that has them, or from a new one.
// False, with nothing taken, if a block is needed and can't be made. Throws
// std::bad_alloc, with nothing taken.
static bool aprend_pool_take_elements(
    aprend_pool_core *core,
    uint32_t count,
    const char *who,
    uint32_t *out_slot,
    uint32_t *out_offset) {
	if (count > core->block_element_count) {
		// Longer than a block: a block of its own, exactly that long.
		const uint32_t slot = aprend_pool_add_block(core, count, true, who);
		if (slot == UINT32_MAX)
			return false;
		core->blocks[slot].ranges.take(count, out_offset);
		*out_slot = slot;
		return true;
	}
	for (uint32_t i = 0; i < (uint32_t)core->blocks.size(); ++i) {
		aprend_pool_block &block = core->blocks[i];
		if (!block.buffer)
			continue;
		if (block.dedicated)
			continue;
		if (!block.ranges.take(count, out_offset))
			continue;
		*out_slot = i;
		return true;
	}
	const uint32_t slot = aprend_pool_add_block(core, core->block_element_count, false, who);
	if (slot == UINT32_MAX)
		return false;
	core->blocks[slot].ranges.take(count, out_offset);
	*out_slot = slot;
	return true;
}

// Takes [count] elements, writes [data] into them and makes them a range:
// entry [out_id], of generation [out_generation]. False with nothing taken
// on any failure. Throws std::bad_alloc, with nothing taken.
static bool aprend_pool_allocate(
    aprend_pool_core *core,
    uint32_t count,
    const void *data,
    const char *who,
    void **out_buffer,
    uint32_t *out_offset,
    uint32_t *out_id,
    uint32_t *out_generation) {
	aprend_pool_reclaim(core);

	// The entry first: with no memory for it, no elements have been taken.
	const uint32_t id = aprend_pool_allocation_take(core);
	uint32_t slot     = UINT32_MAX;
	uint32_t offset   = 0;
	bool taken        = false;
	try {
		taken = aprend_pool_take_elements(core, count, who, &slot, &offset);
	} catch (const std::bad_alloc &) {
		aprend_pool_allocation_release(core, id);
		throw;
	}
	if (!taken) {
		aprend_pool_allocation_release(core, id);
		return false;
	}

	// Nothing submitted reads these elements: they were free, which a freed
	// range only becomes once the GPU has finished with it. So this is the
	// one write a store allows while its buffer is in use, to bytes nothing
	// in flight reads.
	aprend_pool_block &block   = core->blocks[slot];
	const uint64_t byte_offset = (uint64_t)offset * core->element_size;
	const uint64_t byte_size   = (uint64_t)count * core->element_size;
	SPUDRESULT sr = aprend_buffer_store_write(aprend_pool_buffer_store(core, block.buffer), byte_offset, byte_size, data);
	if (SPUDFAIL(sr)) {
		printf("apricot: %s: writing %u %s failed: %s\n", who, count, aprend_pool_element_name(core), spudresult_str(sr));
		aprend_pool_allocation_release(core, id);
		if (block.dedicated) {
			aprend_pool_remove_block(core, slot);
			return false;
		}
		try {
			block.ranges.give(offset, count);
		} catch (const std::bad_alloc &) {
			// The elements stay taken and are never handed out: lost to the
			// pool, and nothing reads them.
			printf("apricot: %s: out of memory giving the range back: %u %s of the pool are lost\n", who, count, aprend_pool_element_name(core));
		}
		return false;
	}

	aprend_pool_allocation &allocation = core->allocations[id];
	allocation.block  = slot;
	allocation.offset = offset;
	allocation.count  = count;
	allocation.state  = aprend_pool_allocation::LIVE;

	core->used_bytes += byte_size;
	*out_buffer     = block.buffer;
	*out_offset     = offset;
	*out_id         = id;
	*out_generation = allocation.generation;
	return true;
}

// Marks the range of entry [id] freed, to be taken back once the GPU has
// finished with it. [generation], [buffer], [offset] and [count] are what
// the caller's copy of the range says, and all of it has to be what the
// entry says: a copy of a range freed earlier, a range of another pool and a
// range that was never one are each refused. [who] names the call, for the
// messages. Needs no memory.
static void aprend_pool_free(
    aprend_pool_core *core,
    uint32_t id,
    uint32_t generation,
    void *buffer,
    uint32_t offset,
    uint32_t count,
    const char *who) {
	if (id >= core->allocations.size()) {
		printf("apricot: %s: the range is not one of this pool\n", who);
		return;
	}
	aprend_pool_allocation &allocation = core->allocations[id];
	if (allocation.generation != generation) {
		// The entry has been a later range since, or the range is no
		// range (generation 0).
		printf("apricot: %s: the range is not allocated: it was freed before, or is not one of this pool\n", who);
		return;
	}
	// Freed and waiting for the GPU, or taken back since and its entry not
	// yet used again.
	if (allocation.state != aprend_pool_allocation::LIVE) {
		printf("apricot: %s: the range was freed already\n", who);
		return;
	}
	// Two pools number their entries alike, so a range of another pool can
	// match an entry here by id and generation. Its block's buffer is its
	// own pool's, which no block of this one is.
	if (core->blocks[allocation.block].buffer != buffer) {
		printf("apricot: %s: the range is not one of this pool\n", who);
		return;
	}
	if (allocation.offset != offset) {
		printf("apricot: %s: the range's start is not the one it was allocated with\n", who);
		return;
	}
	if (allocation.count != count) {
		printf("apricot: %s: the range's length is not the one it was allocated with\n", who);
		return;
	}

	allocation.state  = aprend_pool_allocation::FREED;
	allocation.serial = core->instance->submit_serial.load();
	allocation.next   = APREND_POOL_NO_ALLOCATION;
	if (core->pending_last == APREND_POOL_NO_ALLOCATION)
		core->pending_first = id;
	else
		core->allocations[core->pending_last].next = id;
	core->pending_last = id;
}

// Destroys every ordinary block all of whose elements are free.
static void aprend_pool_trim(aprend_pool_core *core) {
	aprend_pool_reclaim(core);
	for (uint32_t i = 0; i < (uint32_t)core->blocks.size(); ++i) {
		aprend_pool_block &block = core->blocks[i];
		if (!block.buffer)
			continue;
		// A range freed and not yet taken back is not in the free list, so
		// a block the GPU may still read is never whole.
		if (!block.ranges.is_whole())
			continue;
		aprend_pool_remove_block(core, i);
	}
}

// Destroys every block. The pool's ranges are gone with them.
static void aprend_pool_destroy_blocks(aprend_pool_core *core) {
	for (uint32_t i = 0; i < (uint32_t)core->blocks.size(); ++i) {
		if (core->blocks[i].buffer)
			aprend_pool_remove_block(core, i);
	}
	core->allocations.clear();
	core->unused_first  = APREND_POOL_NO_ALLOCATION;
	core->pending_first = APREND_POOL_NO_ALLOCATION;
	core->pending_last  = APREND_POOL_NO_ALLOCATION;
	core->used_bytes    = 0;
}

aprend_vertex_pool_t::~aprend_vertex_pool_t() { aprend_pool_destroy_blocks(&this->core); }

aprend_index_pool_t::~aprend_index_pool_t() { aprend_pool_destroy_blocks(&this->core); }

extern "C" {

aprend_vertex_pool aprend_vertex_pool_create(aprend_instance instance, const aprend_vertex_pool_desc *desc) {
	static const char who[] = "aprend_vertex_pool_create";
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return nullptr;
	}
	if (!desc) {
		printf("apricot: %s: desc is NULL\n", who);
		return nullptr;
	}
	if (desc->vertex_stride == 0) {
		printf("apricot: %s: vertex_stride is 0\n", who);
		return nullptr;
	}
	if (desc->block_vertex_count == 0) {
		printf("apricot: %s: block_vertex_count is 0\n", who);
		return nullptr;
	}
	const uint64_t block_bytes = (uint64_t)desc->block_vertex_count * desc->vertex_stride;
	if (desc->memory_limit != 0 && desc->memory_limit < block_bytes) {
		printf("apricot: %s: memory_limit is below one block's %llu bytes\n", who, (unsigned long long)block_bytes);
		return nullptr;
	}

	aprend_vertex_pool_t *result = (aprend_vertex_pool_t *)malloc(sizeof(aprend_vertex_pool_t));
	if (!result) {
		printf("apricot: %s: out of memory\n", who);
		return nullptr;
	}
	result = new (result) aprend_vertex_pool_t();

	result->core.instance            = instance;
	result->core.is_index            = false;
	result->core.element_size        = desc->vertex_stride;
	result->core.block_element_count = desc->block_vertex_count;
	result->core.memory_limit        = desc->memory_limit;
	return result;
}

void aprend_vertex_pool_destroy(aprend_vertex_pool pool) {
	if (!pool)
		return;
	// The pool itself is nothing the GPU uses. Its blocks are buffers, each
	// released once the GPU has finished with it.
	pool->~aprend_vertex_pool_t();
	free(pool);
}

bool aprend_vertex_pool_allocate(aprend_vertex_pool pool, uint32_t vertex_count, const void *data, aprend_vertex_range *out_range) {
	if (!out_range)
		return false;
	*out_range = {};
	if (!pool)
		return false;
	if (vertex_count == 0)
		return false;
	if (!data)
		return false;

	void *buffer        = nullptr;
	uint32_t offset     = 0;
	uint32_t id         = 0;
	uint32_t generation = 0;
	try {
		if (!aprend_pool_allocate(&pool->core, vertex_count, data, "aprend_vertex_pool_allocate", &buffer, &offset, &id, &generation))
			return false;
	} catch (const std::bad_alloc &) {
		printf("apricot: aprend_vertex_pool_allocate: out of memory\n");
		return false;
	}
	out_range->buffer       = (aprend_vertex_buffer)buffer;
	out_range->first_vertex = offset;
	out_range->vertex_count = vertex_count;
	out_range->_id          = id;
	out_range->_generation  = generation;
	return true;
}

void aprend_vertex_pool_free(aprend_vertex_pool pool, const aprend_vertex_range *range) {
	if (!pool)
		return;
	if (!range)
		return;
	aprend_pool_free(&pool->core, range->_id, range->_generation, range->buffer, range->first_vertex, range->vertex_count, "aprend_vertex_pool_free");
}

void aprend_vertex_pool_trim(aprend_vertex_pool pool) {
	if (!pool)
		return;
	aprend_pool_trim(&pool->core);
}

void aprend_vertex_pool_get_usage(aprend_vertex_pool pool, uint64_t *out_used_bytes, uint64_t *out_block_bytes) {
	if (out_used_bytes)
		*out_used_bytes = 0;
	if (out_block_bytes)
		*out_block_bytes = 0;
	if (!pool)
		return;
	aprend_pool_reclaim(&pool->core);
	if (out_used_bytes)
		*out_used_bytes = pool->core.used_bytes;
	if (out_block_bytes)
		*out_block_bytes = pool->core.block_bytes;
}

aprend_index_pool aprend_index_pool_create(aprend_instance instance, const aprend_index_pool_desc *desc) {
	if (!instance)
		return nullptr;
	if (!desc)
		return nullptr;
	if (desc->stride != APREND_INDEX_STRIDE_UINT16 && desc->stride != APREND_INDEX_STRIDE_UINT32) {
		printf("apricot: aprend_index_pool_create: stride is not UINT16 or UINT32\n");
		return nullptr;
	}
	if (desc->block_index_count == 0) {
		printf("apricot: aprend_index_pool_create: block_index_count is 0\n");
		return nullptr;
	}
	const uint64_t block_bytes = (uint64_t)desc->block_index_count * (uint64_t)desc->stride;
	if (desc->memory_limit != 0 && desc->memory_limit < block_bytes) {
		printf("apricot: aprend_index_pool_create: memory_limit is below one block's %llu bytes\n", (unsigned long long)block_bytes);
		return nullptr;
	}

	aprend_index_pool_t *result = (aprend_index_pool_t *)malloc(sizeof(aprend_index_pool_t));
	if (!result)
		return nullptr;
	result = new (result) aprend_index_pool_t();

	result->core.instance            = instance;
	result->core.is_index            = true;
	result->core.index_stride        = desc->stride;
	result->core.element_size        = (uint32_t)desc->stride;
	result->core.block_element_count = desc->block_index_count;
	result->core.memory_limit        = desc->memory_limit;
	return result;
}

void aprend_index_pool_destroy(aprend_index_pool pool) {
	if (!pool)
		return;
	// As aprend_vertex_pool_destroy.
	pool->~aprend_index_pool_t();
	free(pool);
}

bool aprend_index_pool_allocate(aprend_index_pool pool, uint32_t index_count, const void *data, aprend_index_range *out_range) {
	if (!out_range)
		return false;
	*out_range = {};
	if (!pool)
		return false;
	if (index_count == 0)
		return false;
	if (!data)
		return false;

	void *buffer        = nullptr;
	uint32_t offset     = 0;
	uint32_t id         = 0;
	uint32_t generation = 0;
	try {
		if (!aprend_pool_allocate(&pool->core, index_count, data, "aprend_index_pool_allocate", &buffer, &offset, &id, &generation))
			return false;
	} catch (const std::bad_alloc &) {
		printf("apricot: aprend_index_pool_allocate: out of memory\n");
		return false;
	}
	out_range->buffer      = (aprend_index_buffer)buffer;
	out_range->first_index = offset;
	out_range->index_count = index_count;
	out_range->_id         = id;
	out_range->_generation = generation;
	return true;
}

void aprend_index_pool_free(aprend_index_pool pool, const aprend_index_range *range) {
	if (!pool)
		return;
	if (!range)
		return;
	aprend_pool_free(&pool->core, range->_id, range->_generation, range->buffer, range->first_index, range->index_count, "aprend_index_pool_free");
}

void aprend_index_pool_trim(aprend_index_pool pool) {
	if (!pool)
		return;
	aprend_pool_trim(&pool->core);
}

void aprend_index_pool_get_usage(aprend_index_pool pool, uint64_t *out_used_bytes, uint64_t *out_block_bytes) {
	if (out_used_bytes)
		*out_used_bytes = 0;
	if (out_block_bytes)
		*out_block_bytes = 0;
	if (!pool)
		return;
	aprend_pool_reclaim(&pool->core);
	if (out_used_bytes)
		*out_used_bytes = pool->core.used_bytes;
	if (out_block_bytes)
		*out_block_bytes = pool->core.block_bytes;
}
}
