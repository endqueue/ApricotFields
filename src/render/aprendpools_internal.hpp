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

#include <vector>

struct aprend_pool_block {
	/* An aprend_vertex_buffer or an aprend_index_buffer, by the pool's kind.
	 * NULL in a slot that holds no block: slots are never removed, so the
	 * slot number a range carries stays good, and an empty one is the first
	 * to be filled. */
	void *buffer{nullptr};
	/* Which of the block's elements are free. A freed range is not among
	 * them until the GPU has finished with it (aprend_pool_core::pending). */
	aprend_range_list ranges{};
	/* Made for one range longer than a block: it holds that range and
	 * nothing else, and goes when the range does. */
	bool dedicated{false};
};

/* A range that was freed while the GPU might still read it. */
struct aprend_pool_pending {
	uint32_t block;
	/* The block's buffer when the range was freed: the slot may hold another
	 * block by the time this is looked at. */
	void *buffer;
	uint32_t offset;
	uint32_t count;
	/* The instance's submit serial at the free. Everything submitted up to
	 * then may read the range, and nothing submitted after may, so the range
	 * is free once the instance's fence has reached this. */
	uint64_t serial;
};

struct aprend_pool_core {
	aprend_instance instance{nullptr};
	/* An index pool; a vertex pool otherwise. */
	bool is_index{false};
	/* A vertex pool's layout. The elements are the pool's own copy. */
	aprend_buffer_layout vertex_layout{};
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
	/* In the order they were freed, so their serials never go down: taken
	 * back strictly from the front. Entries before pending_head are done. */
	std::vector<aprend_pool_pending> pending{};
	size_t pending_head{0};
};

typedef struct aprend_vertex_pool_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_vertex_pool_t() = default;
	~aprend_vertex_pool_t();
	aprend_pool_core core{};
} aprend_vertex_pool_t;

typedef struct aprend_index_pool_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_index_pool_t() = default;
	~aprend_index_pool_t();
	aprend_pool_core core{};
} aprend_index_pool_t;

#endif // APREND_POOLS_INTERNAL_HPP
