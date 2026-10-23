#ifndef APREND_POOLS_INTERNAL_HPP
#define APREND_POOLS_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/.
 *
 * Vertex and index pools (render/aprendbuffers.h): a few large vertex or
 * index buffers, the blocks, and which elements of each are handed out. The
 * two pools are one thing over two kinds of buffer, so both handles hold an
 * aprend_pool_core and aprendpools.cpp is written against that.
 *
 * A block is an ordinary aprend_vertex_buffer or aprend_index_buffer: a
 * command list binds it, finds its staged writes and orders them as it does
 * any other, and nothing in aprendcommands.cpp knows a pool exists. */

#include "render/aprendbuffers.h"
#include "render/aprendcontext.h"

#include "aprendranges_internal.hpp"
#include "../apriplaindata.hpp"

#include <vector>

struct aprend_pool_block {
	/* An aprend_vertex_buffer or an aprend_index_buffer, by the pool's kind.
	 * NULL in a slot that holds no block: slots are never removed, so the
	 * slot number a range carries stays good, and an empty one is the first
	 * to be filled. */
	void *buffer{nullptr};
	/* Which of the block's elements are free. A freed range is not among
	 * them until the GPU has finished with it (aprend_pool_allocation). */
	aprend_range_list ranges{};
	/* Made for one range longer than a block: it holds that range and
	 * nothing else, and goes when the range does. */
	bool dedicated{false};
};

/* No entry: the end of a list of allocations. */
#define APREND_POOL_NO_ALLOCATION 0xFFFFFFFFu

/* One range the pool has handed out: what a range a caller holds is checked
 * against, since the caller's copy is only a value and may be stale. A range
 * names its entry and the generation the entry had when the range was made;
 * an entry is used again for later ranges and its generation goes up each
 * time, so an old copy of a range never matches a newer one, wherever the
 * newer one's elements are. */
struct aprend_pool_allocation {
	enum : uint8_t {
		/* Not a range. On the unused list. */
		UNUSED = 0,
		/* Handed out and not freed. */
		LIVE = 1,
		/* Freed, and the GPU may still read it. On the pending list. */
		FREED = 2,
	};
	uint32_t block{0};
	uint32_t offset{0};
	uint32_t count{0};
	/* Never 0 in an entry that has been used, so a zeroed range matches
	 * nothing. */
	uint32_t generation{0};
	/* FREED: the instance's submit serial at the free. Everything submitted
	 * up to then may read the range, and nothing submitted after may, so
	 * its elements are free once the instance's fence has reached this. */
	uint64_t serial{0};
	/* The entry after this one on the list it is on, unused or pending. */
	uint32_t next{APREND_POOL_NO_ALLOCATION};
	uint8_t state{UNUSED};
};
APRI_PLAIN_DATA(aprend_pool_allocation);

struct aprend_pool_core {
	aprend_instance instance{nullptr};
	/* An index pool; a vertex pool otherwise. */
	bool is_index{false};
	/* An index pool's index size. */
	APREND_INDEX_STRIDE index_stride{APREND_INDEX_STRIDE_NONE};
	/* Bytes in one vertex or one index. */
	uint32_t element_size{0};
	/* Elements in an ordinary block. */
	uint32_t block_element_count{0};
	/* The most block_bytes may be; 0 for no limit. */
	uint64_t memory_limit{0};
	/* Bytes of every block that exists. */
	uint64_t block_bytes{0};
	/* Bytes of every range handed out and not yet taken back: one that was
	 * freed counts until the GPU has finished with it. */
	uint64_t used_bytes{0};
	std::vector<aprend_pool_block> blocks{};
	/* Every range handed out since the pool was made has an entry here, by
	 * the id it carries. Entries are never removed, only used again. */
	std::vector<aprend_pool_allocation> allocations{};
	/* The entries that are no range, to be used before a new one is added. */
	uint32_t unused_first{APREND_POOL_NO_ALLOCATION};
	/* The freed ranges the GPU may still read, in the order they were freed,
	 * so their serials never go down: taken back strictly from the front,
	 * added at the back. Both lists run through the entries themselves, so
	 * neither ever needs memory and a free can't fail for want of it. */
	uint32_t pending_first{APREND_POOL_NO_ALLOCATION};
	uint32_t pending_last{APREND_POOL_NO_ALLOCATION};
};

typedef struct aprend_vertex_pool_t {
	char *debug_name{nullptr};
	aprend_vertex_pool_t() = default;
	~aprend_vertex_pool_t();
	aprend_pool_core core{};
} aprend_vertex_pool_t;

typedef struct aprend_index_pool_t {
	char *debug_name{nullptr};
	aprend_index_pool_t() = default;
	~aprend_index_pool_t();
	aprend_pool_core core{};
} aprend_index_pool_t;

#endif // APREND_POOLS_INTERNAL_HPP
