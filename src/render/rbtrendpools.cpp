
#include "render/rbtrendbuffers.h"
#include "rbtrend_internal.hpp"

#include <crgpu.h>

#include <assert.h>
#include <new>
#include <stdlib.h>

// Vertex and index pools (render/rbtrendbuffers.h). Both are an
// rbtrend_pool_core (rbtrendpools_internal.hpp); everything up to the C entry
// points is written against that and asks the core which kind it is only
// where a block's buffer is made, destroyed or written.

static const char *rbtrend_pool_element_name(const rbtrend_pool_core *core) { return core->is_index ? "indices" : "vertices"; }

// A block's buffer of [count] elements, unwritten, named after the pool.
// Written once a range, by the pool alone: a ONCE buffer stages each range
// in a buffer of its own, keeps no staging copy of the block, and refuses
// the caller's writes.
static RBT_RESULT rbtrend_pool_buffer_create(rbtrend_pool_core *core, uint32_t count, rbtrend_buffer *out_buffer, RBTREND_ERROR *out_error) {
	RBTREND_BUFFER_DESC desc{};
	desc.base         = rbt_struct_base(RBT_STRUCT_TYPE_REND_BUFFER_DESC);
	desc.size         = (uint64_t)count * core->element_size;
	desc.usage        = core->is_index ? RBTREND_BUFFER_USAGE_INDEX : RBTREND_BUFFER_USAGE_VERTEX;
	desc.writes       = RBTREND_BUFFER_WRITES_ONCE;
	desc.index_stride = core->index_stride;
	desc.debug_name   = core->name;
	return rbtrend_buffer_create(core->instance, &desc, out_buffer, out_error);
}

// Makes a block of [count] elements, all free, in the first empty slot, and
// hands its slot back through [out_slot]. RBT_RESULT_LIMIT with nothing made
// if the pool's limit doesn't allow it, or the buffer's failure. Throws
// std::bad_alloc, with nothing made.
static RBT_RESULT rbtrend_pool_add_block(rbtrend_pool_core *core, uint32_t count, bool dedicated, uint32_t *out_slot, RBTREND_ERROR *out_error) {
	const uint64_t bytes = (uint64_t)count * core->element_size;
	// block_bytes never passes the limit, so the subtraction can't wrap.
	if (core->memory_limit != 0 && bytes > core->memory_limit - core->block_bytes)
		return rbtrend_fail(out_error, RBT_RESULT_LIMIT, "a block of %llu bytes more would pass the pool's memory_limit", (unsigned long long)bytes);

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
	rbtrend_pool_block &block = core->blocks[slot];
	block.ranges.reset(count);

	const RBT_RESULT ar = rbtrend_pool_buffer_create(core, count, &block.buffer, out_error);
	if (ar != RBT_RESULT_OK) {
		block.ranges.reset(0);
		return ar;
	}
	block.dedicated = dedicated;
	core->block_bytes += bytes;
	*out_slot = slot;
	return RBT_RESULT_OK;
}

// Destroys the block in [slot] and leaves the slot empty. Nothing of it is
// handed out or waiting to be taken back.
static void rbtrend_pool_remove_block(rbtrend_pool_core *core, uint32_t slot) {
	rbtrend_pool_block &block = core->blocks[slot];
	core->block_bytes -= (uint64_t)block.ranges.size * core->element_size;
	// Released once the GPU has finished everything submitted so far, as
	// any buffer is.
	rbtrend_buffer_destroy(block.buffer);
	block.buffer    = nullptr;
	block.dedicated = false;
	block.ranges.reset(0);
}

// An entry for a new range: an unused one, or one more. Its generation is
// already the new range's. Throws std::bad_alloc.
static uint32_t rbtrend_pool_allocation_take(rbtrend_pool_core *core) {
	uint32_t id = core->unused_first;
	if (id != RBTREND_POOL_NO_ALLOCATION) {
		core->unused_first = core->allocations[id].next;
	} else {
		core->allocations.emplace_back();
		id = (uint32_t)core->allocations.size() - 1;
	}
	rbtrend_pool_allocation &allocation = core->allocations[id];
	allocation.next = RBTREND_POOL_NO_ALLOCATION;
	// 0 is no generation: after four billion uses of one entry it is
	// stepped over.
	allocation.generation++;
	if (allocation.generation == 0)
		allocation.generation = 1;
	return id;
}

// Puts entry [id] back on the unused list. It keeps its generation, which
// the next range to use it moves on.
static void rbtrend_pool_allocation_release(rbtrend_pool_core *core, uint32_t id) {
	rbtrend_pool_allocation &allocation = core->allocations[id];
	allocation.state   = rbtrend_pool_allocation::UNUSED;
	allocation.next    = core->unused_first;
	core->unused_first = id;
}

// Takes back every freed range the GPU has finished with: its elements are
// free again, its entry unused, and a block made for it alone is destroyed.
static void rbtrend_pool_reclaim(rbtrend_pool_core *core) {
	if (core->pending_first == RBTREND_POOL_NO_ALLOCATION)
		return;
	// 0 if the value can't be read, which takes back only what was freed
	// before anything was submitted.
	const uint64_t completed = crgpu_fence_get_value(core->instance->frame_fence);
	while (core->pending_first != RBTREND_POOL_NO_ALLOCATION) {
		const uint32_t id                  = core->pending_first;
		rbtrend_pool_allocation &allocation = core->allocations[id];
		if (allocation.serial > completed)
			break;
		rbtrend_pool_block &block = core->blocks[allocation.block];
		bool given               = false;
		try {
			given = block.ranges.give(allocation.offset, allocation.count);
		} catch (const std::bad_alloc &) {
			// No memory to list it as free. It stays where it is and is
			// tried again at the next call.
			break;
		}
		core->pending_first = allocation.next;
		if (core->pending_first == RBTREND_POOL_NO_ALLOCATION)
			core->pending_last = RBTREND_POOL_NO_ALLOCATION;
		// An entry is a range's elements for as long as it is LIVE or
		// FREED, so they can't have been free: a range list out of step
		// with the entries would hand one range's elements to another.
		assert(given);
		if (given)
			core->used_bytes -= (uint64_t)allocation.count * core->element_size;
		if (block.dedicated)
			rbtrend_pool_remove_block(core, allocation.block);
		rbtrend_pool_allocation_release(core, id);
	}
}

// Takes [count] elements from a block that has them, or from a new one.
// The block's failure, with nothing taken, if a block is needed and can't
// be made. Throws std::bad_alloc, with nothing taken.
static RBT_RESULT rbtrend_pool_take_elements(
    rbtrend_pool_core *core,
    uint32_t count,
    uint32_t *out_slot,
    uint32_t *out_offset,
    RBTREND_ERROR *out_error) {
	uint32_t slot = UINT32_MAX;
	if (count > core->block_element_count) {
		// Longer than a block: a block of its own, exactly that long.
		const RBT_RESULT ar = rbtrend_pool_add_block(core, count, true, &slot, out_error);
		if (ar != RBT_RESULT_OK)
			return ar;
		core->blocks[slot].ranges.take(count, out_offset);
		*out_slot = slot;
		return RBT_RESULT_OK;
	}
	for (uint32_t i = 0; i < (uint32_t)core->blocks.size(); ++i) {
		rbtrend_pool_block &block = core->blocks[i];
		if (!block.buffer)
			continue;
		if (block.dedicated)
			continue;
		if (!block.ranges.take(count, out_offset))
			continue;
		*out_slot = i;
		return RBT_RESULT_OK;
	}
	const RBT_RESULT ar = rbtrend_pool_add_block(core, core->block_element_count, false, &slot, out_error);
	if (ar != RBT_RESULT_OK)
		return ar;
	core->blocks[slot].ranges.take(count, out_offset);
	*out_slot = slot;
	return RBT_RESULT_OK;
}

// Takes [count] elements, writes [data] into them and makes them a range:
// entry [out_id], of generation [out_generation]. The failure, with nothing
// taken, otherwise. Throws std::bad_alloc, with nothing taken.
static RBT_RESULT rbtrend_pool_allocate(
    rbtrend_pool_core *core,
    uint32_t count,
    const void *data,
    rbtrend_buffer *out_buffer,
    uint32_t *out_offset,
    uint32_t *out_id,
    uint32_t *out_generation,
    RBTREND_ERROR *out_error) {
	rbtrend_pool_reclaim(core);

	// The entry first: with no memory for it, no elements have been taken.
	const uint32_t id = rbtrend_pool_allocation_take(core);
	uint32_t slot     = UINT32_MAX;
	uint32_t offset   = 0;
	RBT_RESULT ar       = RBT_RESULT_OK;
	try {
		ar = rbtrend_pool_take_elements(core, count, &slot, &offset, out_error);
	} catch (const std::bad_alloc &) {
		rbtrend_pool_allocation_release(core, id);
		throw;
	}
	if (ar != RBT_RESULT_OK) {
		rbtrend_pool_allocation_release(core, id);
		return ar;
	}

	// Nothing submitted reads these elements: they were free, which a freed
	// range only becomes once the GPU has finished with it. So this is the
	// one write a store allows while its buffer is in use, to bytes nothing
	// in flight reads.
	rbtrend_pool_block &block   = core->blocks[slot];
	const uint64_t byte_offset = (uint64_t)offset * core->element_size;
	const uint64_t byte_size   = (uint64_t)count * core->element_size;
	CR_RESULT sr = rbtrend_buffer_store_write(&block.buffer->parts[0].store, byte_offset, byte_size, data);
	if (CR_FAILED(sr)) {
		rbtrend_pool_allocation_release(core, id);
		ar = rbtrend_fail_cr(out_error, sr, "writing %u %s failed", count, rbtrend_pool_element_name(core));
		if (block.dedicated) {
			rbtrend_pool_remove_block(core, slot);
			return ar;
		}
		try {
			block.ranges.give(offset, count);
		} catch (const std::bad_alloc &) {
			// The elements stay taken and are never handed out: lost to the
			// pool, and nothing reads them. The write's failure is what the
			// caller hears of.
		}
		return ar;
	}

	rbtrend_pool_allocation &allocation = core->allocations[id];
	allocation.block  = slot;
	allocation.offset = offset;
	allocation.count  = count;
	allocation.state  = rbtrend_pool_allocation::LIVE;

	core->used_bytes += byte_size;
	*out_buffer     = block.buffer;
	*out_offset     = offset;
	*out_id         = id;
	*out_generation = allocation.generation;
	return RBT_RESULT_OK;
}

// Marks the range of entry [id] freed, to be taken back once the GPU has
// finished with it. [generation], [buffer], [offset] and [count] are what
// the caller's copy of the range says, and all of it has to be what the
// entry says: a copy of a range freed earlier, a range of another pool and a
// range that was never one are each refused. Needs no memory.
static RBT_RESULT rbtrend_pool_free(
    rbtrend_pool_core *core,
    uint32_t id,
    uint32_t generation,
    rbtrend_buffer buffer,
    uint32_t offset,
    uint32_t count,
    RBTREND_ERROR *out_error) {
	if (id >= core->allocations.size())
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the range is not one of this pool");
	rbtrend_pool_allocation &allocation = core->allocations[id];
	// The entry has been a later range since, or the range is no range
	// (generation 0).
	if (allocation.generation != generation)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the range is not allocated: it was freed before, or is not one of this pool");
	// Freed and waiting for the GPU, or taken back since and its entry not
	// yet used again.
	if (allocation.state != rbtrend_pool_allocation::LIVE)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the range was freed already");
	// Two pools number their entries alike, so a range of another pool can
	// match an entry here by id and generation. Its block's buffer is its
	// own pool's, which no block of this one is.
	if (core->blocks[allocation.block].buffer != buffer)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the range is not one of this pool");
	if (allocation.offset != offset)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the range's start is not the one it was allocated with");
	if (allocation.count != count)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the range's length is not the one it was allocated with");

	allocation.state  = rbtrend_pool_allocation::FREED;
	allocation.serial = core->instance->submit_serial.load();
	allocation.next   = RBTREND_POOL_NO_ALLOCATION;
	if (core->pending_last == RBTREND_POOL_NO_ALLOCATION)
		core->pending_first = id;
	else
		core->allocations[core->pending_last].next = id;
	core->pending_last = id;
	return RBT_RESULT_OK;
}

// Destroys every ordinary block all of whose elements are free.
static void rbtrend_pool_trim(rbtrend_pool_core *core) {
	rbtrend_pool_reclaim(core);
	for (uint32_t i = 0; i < (uint32_t)core->blocks.size(); ++i) {
		rbtrend_pool_block &block = core->blocks[i];
		if (!block.buffer)
			continue;
		// A range freed and not yet taken back is not in the free list, so
		// a block the GPU may still read is never whole.
		if (!block.ranges.is_whole())
			continue;
		rbtrend_pool_remove_block(core, i);
	}
}

// Destroys every block. The pool's ranges are gone with them.
static void rbtrend_pool_destroy_blocks(rbtrend_pool_core *core) {
	for (uint32_t i = 0; i < (uint32_t)core->blocks.size(); ++i) {
		if (core->blocks[i].buffer)
			rbtrend_pool_remove_block(core, i);
	}
	core->allocations.clear();
	core->unused_first  = RBTREND_POOL_NO_ALLOCATION;
	core->pending_first = RBTREND_POOL_NO_ALLOCATION;
	core->pending_last  = RBTREND_POOL_NO_ALLOCATION;
	core->used_bytes    = 0;
}

rbtrend_vertex_pool_t::~rbtrend_vertex_pool_t() {
	rbtrend_pool_destroy_blocks(&this->core);
	free(this->core.name);
}

rbtrend_index_pool_t::~rbtrend_index_pool_t() {
	rbtrend_pool_destroy_blocks(&this->core);
	free(this->core.name);
}

// Copies [name] into [core] and points the handle's debug name at it.
// False if there is no memory for it; a NULL name is no name.
static bool rbtrend_pool_name(rbtrend_pool_core *core, char **debug_name, const char *name) {
	if (!name)
		return true;
	const size_t length = strlen(name) + 1;
	core->name          = (char *)malloc(length);
	if (!core->name)
		return false;
	memcpy(core->name, name, length);
	*debug_name = core->name;
	return true;
}

extern "C" {

RBT_RESULT rbtrend_vertex_pool_create(rbtrend_instance instance, const RBTREND_VERTEX_POOL_DESC *desc, rbtrend_vertex_pool *out_pool, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_pool)
		*out_pool = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_vertex_pool_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_vertex_pool_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	const RBT_RESULT ar = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_VERTEX_POOL_DESC);
	if (ar != RBT_RESULT_OK)
		return ar;
	if (!out_pool)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_pool is NULL");
	if (desc->vertex_stride == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "vertex_stride is 0");
	if (desc->block_vertex_count == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "block_vertex_count is 0");
	const uint64_t block_bytes = (uint64_t)desc->block_vertex_count * desc->vertex_stride;
	if (desc->memory_limit != 0 && desc->memory_limit < block_bytes)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "memory_limit is below one block's %llu bytes", (unsigned long long)block_bytes);

	rbtrend_vertex_pool_t *result = (rbtrend_vertex_pool_t *)malloc(sizeof(rbtrend_vertex_pool_t));
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the pool");
	result = new (result) rbtrend_vertex_pool_t();
	if (!rbtrend_pool_name(&result->core, &result->debug_name, desc->debug_name)) {
		RBTREND_DESTRUCT__T(result, rbtrend_vertex_pool_t);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the pool's name");
	}

	result->core.instance            = instance;
	result->core.is_index            = false;
	result->core.element_size        = desc->vertex_stride;
	result->core.block_element_count = desc->block_vertex_count;
	result->core.memory_limit        = desc->memory_limit;
	*out_pool                        = result;
	return RBT_RESULT_OK;
}

void rbtrend_vertex_pool_destroy(rbtrend_vertex_pool pool) {
	if (!pool)
		return;
	if (RBT_STALE(pool, "rbtrend_vertex_pool_destroy", "pool"))
		return;
	// The pool itself is nothing the GPU uses. Its blocks are buffers, each
	// released once the GPU has finished with it.
	RBT_RETIRE(pool->core.instance, pool, rbtrend_vertex_pool_t);
}

RBT_RESULT rbtrend_vertex_pool_allocate(rbtrend_vertex_pool pool, uint32_t vertex_count, const void *data, RBTREND_VERTEX_RANGE *out_range, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_range)
		*out_range = {};
	if (!pool)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pool is NULL");
	if (RBT_STALE(pool, "rbtrend_vertex_pool_allocate", "pool"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pool is destroyed");
	RBTREND_ENTER(pool->core.instance, "rbtrend_vertex_pool_allocate");
	if (vertex_count == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "vertex_count is 0");
	if (!data)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "data is NULL");
	if (!out_range)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_range is NULL");

	rbtrend_buffer buffer = nullptr;
	uint32_t offset      = 0;
	uint32_t id          = 0;
	uint32_t generation  = 0;
	try {
		const RBT_RESULT ar = rbtrend_pool_allocate(&pool->core, vertex_count, data, &buffer, &offset, &id, &generation, out_error);
		if (ar != RBT_RESULT_OK)
			return ar;
	} catch (const std::bad_alloc &) {
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the range");
	}
	out_range->buffer       = buffer;
	out_range->first_vertex = offset;
	out_range->vertex_count = vertex_count;
	out_range->_id          = id;
	out_range->_generation  = generation;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_vertex_pool_free(rbtrend_vertex_pool pool, const RBTREND_VERTEX_RANGE *range, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!pool)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pool is NULL");
	if (RBT_STALE(pool, "rbtrend_vertex_pool_free", "pool"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pool is destroyed");
	RBTREND_ENTER(pool->core.instance, "rbtrend_vertex_pool_free");
	if (!range)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "range is NULL");
	return rbtrend_pool_free(&pool->core, range->_id, range->_generation, range->buffer, range->first_vertex, range->vertex_count, out_error);
}

void rbtrend_vertex_pool_trim(rbtrend_vertex_pool pool) {
	if (!pool)
		return;
	if (RBT_STALE(pool, "rbtrend_vertex_pool_trim", "pool"))
		return;
	RBTREND_ENTER(pool->core.instance, "rbtrend_vertex_pool_trim");
	rbtrend_pool_trim(&pool->core);
}

RBT_RESULT rbtrend_vertex_pool_get_usage(rbtrend_vertex_pool pool, uint64_t *out_used_bytes, uint64_t *out_block_bytes) {
	if (!pool)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(pool, "rbtrend_vertex_pool_get_usage", "pool"))
		return RBT_RESULT_INVALID_ARGUMENT;
	rbtrend_pool_reclaim(&pool->core);
	if (out_used_bytes)
		*out_used_bytes = pool->core.used_bytes;
	if (out_block_bytes)
		*out_block_bytes = pool->core.block_bytes;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_index_pool_create(rbtrend_instance instance, const RBTREND_INDEX_POOL_DESC *desc, rbtrend_index_pool *out_pool, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_pool)
		*out_pool = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_index_pool_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_index_pool_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	const RBT_RESULT ar = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_INDEX_POOL_DESC);
	if (ar != RBT_RESULT_OK)
		return ar;
	if (!out_pool)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_pool is NULL");
	if (desc->stride != RBTREND_INDEX_STRIDE_UINT16 && desc->stride != RBTREND_INDEX_STRIDE_UINT32)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "stride %u is not UINT16 or UINT32", (unsigned)desc->stride);
	if (desc->block_index_count == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "block_index_count is 0");
	const uint64_t block_bytes = (uint64_t)desc->block_index_count * (uint64_t)desc->stride;
	if (desc->memory_limit != 0 && desc->memory_limit < block_bytes)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "memory_limit is below one block's %llu bytes", (unsigned long long)block_bytes);

	rbtrend_index_pool_t *result = (rbtrend_index_pool_t *)malloc(sizeof(rbtrend_index_pool_t));
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the pool");
	result = new (result) rbtrend_index_pool_t();
	if (!rbtrend_pool_name(&result->core, &result->debug_name, desc->debug_name)) {
		RBTREND_DESTRUCT__T(result, rbtrend_index_pool_t);
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the pool's name");
	}

	result->core.instance            = instance;
	result->core.is_index            = true;
	result->core.index_stride        = desc->stride;
	result->core.element_size        = (uint32_t)desc->stride;
	result->core.block_element_count = desc->block_index_count;
	result->core.memory_limit        = desc->memory_limit;
	*out_pool                        = result;
	return RBT_RESULT_OK;
}

void rbtrend_index_pool_destroy(rbtrend_index_pool pool) {
	if (!pool)
		return;
	if (RBT_STALE(pool, "rbtrend_index_pool_destroy", "pool"))
		return;
	// As rbtrend_vertex_pool_destroy.
	RBT_RETIRE(pool->core.instance, pool, rbtrend_index_pool_t);
}

RBT_RESULT rbtrend_index_pool_allocate(rbtrend_index_pool pool, uint32_t index_count, const void *data, RBTREND_INDEX_RANGE *out_range, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_range)
		*out_range = {};
	if (!pool)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pool is NULL");
	if (RBT_STALE(pool, "rbtrend_index_pool_allocate", "pool"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pool is destroyed");
	RBTREND_ENTER(pool->core.instance, "rbtrend_index_pool_allocate");
	if (index_count == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "index_count is 0");
	if (!data)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "data is NULL");
	if (!out_range)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_range is NULL");

	rbtrend_buffer buffer = nullptr;
	uint32_t offset      = 0;
	uint32_t id          = 0;
	uint32_t generation  = 0;
	try {
		const RBT_RESULT ar = rbtrend_pool_allocate(&pool->core, index_count, data, &buffer, &offset, &id, &generation, out_error);
		if (ar != RBT_RESULT_OK)
			return ar;
	} catch (const std::bad_alloc &) {
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the range");
	}
	out_range->buffer      = buffer;
	out_range->first_index = offset;
	out_range->index_count = index_count;
	out_range->_id         = id;
	out_range->_generation = generation;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_index_pool_free(rbtrend_index_pool pool, const RBTREND_INDEX_RANGE *range, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!pool)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pool is NULL");
	if (RBT_STALE(pool, "rbtrend_index_pool_free", "pool"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "pool is destroyed");
	RBTREND_ENTER(pool->core.instance, "rbtrend_index_pool_free");
	if (!range)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "range is NULL");
	return rbtrend_pool_free(&pool->core, range->_id, range->_generation, range->buffer, range->first_index, range->index_count, out_error);
}

void rbtrend_index_pool_trim(rbtrend_index_pool pool) {
	if (!pool)
		return;
	if (RBT_STALE(pool, "rbtrend_index_pool_trim", "pool"))
		return;
	RBTREND_ENTER(pool->core.instance, "rbtrend_index_pool_trim");
	rbtrend_pool_trim(&pool->core);
}

RBT_RESULT rbtrend_index_pool_get_usage(rbtrend_index_pool pool, uint64_t *out_used_bytes, uint64_t *out_block_bytes) {
	if (!pool)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(pool, "rbtrend_index_pool_get_usage", "pool"))
		return RBT_RESULT_INVALID_ARGUMENT;
	rbtrend_pool_reclaim(&pool->core);
	if (out_used_bytes)
		*out_used_bytes = pool->core.used_bytes;
	if (out_block_bytes)
		*out_block_bytes = pool->core.block_bytes;
	return RBT_RESULT_OK;
}
}
