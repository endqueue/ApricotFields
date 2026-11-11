#ifndef RBTREND_POOLS_INTERNAL_HPP
#define RBTREND_POOLS_INTERNAL_HPP

/* Internal header — never included outside rabbittus/src/render/.
 *
 * Vertex and index pools (render/rbtrendbuffers.h): a few large vertex or
 * index buffers, the blocks, and which elements of each are handed out. The
 * two pools are one thing over two kinds of buffer, so both handles hold an
 * rbtrend_pool_core and rbtrendpools.cpp is written against that.
 *
 * A block is an ordinary rbtrend_buffer, of VERTEX or INDEX usage: a
 * command list binds it, finds its staged writes and orders them as it does
 * any other, and nothing in rbtrendcommands.cpp knows a pool exists. */

#include "render/rbtrendbuffers.h"
#include "render/rbtrendcontext.h"

#include "rbtrendranges_internal.hpp"
#include "../rbtplaindata.hpp"
#include "../rbtstale.hpp"

#include <vector>

struct rbtrend_pool_block {
	/* A VERTEX or INDEX buffer, by the pool's kind, written once a range.
	 * NULL in a slot that holds no block: slots are never removed, so the
	 * slot number a range carries stays good, and an empty one is the first
	 * to be filled. */
	rbtrend_buffer buffer{nullptr};
	/* Which of the block's elements are free. A freed range is not among
	 * them until the GPU has finished with it (rbtrend_pool_allocation). */
	rbtrend_range_list ranges{};
	/* Made for one range longer than a block: it holds that range and
	 * nothing else, and goes when the range does. */
	bool dedicated{false};
};

/* No entry: the end of a list of allocations. */
#define RBTREND_POOL_NO_ALLOCATION 0xFFFFFFFFu

/* One range the pool has handed out: what a range a caller holds is checked
 * against, since the caller's copy is only a value and may be stale. A range
 * names its entry and the generation the entry had when the range was made;
 * an entry is used again for later ranges and its generation goes up each
 * time, so an old copy of a range never matches a newer one, wherever the
 * newer one's elements are. */
struct rbtrend_pool_allocation {
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
	uint32_t next{RBTREND_POOL_NO_ALLOCATION};
	uint8_t state{UNUSED};
};
RBT_PLAIN_DATA(rbtrend_pool_allocation);

struct rbtrend_pool_core {
	rbtrend_instance instance{nullptr};
	/* The pool's own copy of its desc's debug_name, which its blocks are
	 * named after; NULL for none. */
	char *name{nullptr};
	/* An index pool; a vertex pool otherwise. */
	bool is_index{false};
	/* An index pool's index size. */
	RBTREND_INDEX_STRIDE index_stride{RBTREND_INDEX_STRIDE_NONE};
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
	std::vector<rbtrend_pool_block> blocks{};
	/* Every range handed out since the pool was made has an entry here, by
	 * the id it carries. Entries are never removed, only used again. */
	std::vector<rbtrend_pool_allocation> allocations{};
	/* The entries that are no range, to be used before a new one is added. */
	uint32_t unused_first{RBTREND_POOL_NO_ALLOCATION};
	/* The freed ranges the GPU may still read, in the order they were freed,
	 * so their serials never go down: taken back strictly from the front,
	 * added at the back. Both lists run through the entries themselves, so
	 * neither ever needs memory and a free can't fail for want of it. */
	uint32_t pending_first{RBTREND_POOL_NO_ALLOCATION};
	uint32_t pending_last{RBTREND_POOL_NO_ALLOCATION};
};

typedef struct rbtrend_vertex_pool_t {
	char *debug_name{nullptr};
	RBT_STALE_TAG
	rbtrend_vertex_pool_t() = default;
	~rbtrend_vertex_pool_t();
	rbtrend_pool_core core{};
} rbtrend_vertex_pool_t;

typedef struct rbtrend_index_pool_t {
	char *debug_name{nullptr};
	RBT_STALE_TAG
	rbtrend_index_pool_t() = default;
	~rbtrend_index_pool_t();
	rbtrend_pool_core core{};
} rbtrend_index_pool_t;

#endif // RBTREND_POOLS_INTERNAL_HPP
