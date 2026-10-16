
#include "render/aprendtransient.h"
#include "aprend_internal.hpp"

#include <new>

// Destroys [chunk]'s views, which nothing reads any more.
static void aprend_transient_chunk_destroy_views(aprend_transient_chunk *chunk) {
	for (spudgpu_buffer_view view : chunk->views)
		spudgpu_destroy_buffer_view(view);
	chunk->views.clear();
}

// Destroys [chunk] and everything it holds. The GPU has finished with it.
static void aprend_transient_chunk_destroy(aprend_transient_chunk *chunk) {
	aprend_transient_chunk_destroy_views(chunk);
	if (chunk->mapped)
		spudgpu_unmap_buffer(chunk->buffer);
	if (chunk->buffer)
		spudgpu_destroy_buffer(chunk->buffer);
	chunk->~aprend_transient_chunk();
	free(chunk);
}

// A new chunk of [size] bytes, mapped. NULL if the device refuses it.
static aprend_transient_chunk *aprend_transient_chunk_create(
    aprend_instance instance,
    uint64_t size) {
	aprend_transient_chunk *chunk = (aprend_transient_chunk *)malloc(sizeof(aprend_transient_chunk));
	if (!chunk)
		return nullptr;
	chunk       = new (chunk) aprend_transient_chunk();
	chunk->size = size;

	// What a span is bound as, and a copy source so the same memory can
	// later stage writes to buffers in the device's memory.
	spudgpu_buffer_desc bd{};
	bd.memory_kind = SPUDGPU_MEMORY_KIND_UPLOAD;
	bd.size        = size;
	bd.usage       = SPUDGPU_BUFFER_USAGE_VERTEX | SPUDGPU_BUFFER_USAGE_INDEX | SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	SPUDRESULT sr  = spudgpu_create_buffer(instance->desc.device, &bd, &chunk->buffer);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_transient_allocate: a chunk of %llu bytes can't be created: %s\n", (unsigned long long)size, spudresult_str(sr));
		chunk->buffer = nullptr;
		aprend_transient_chunk_destroy(chunk);
		return nullptr;
	}
	void *mapped = nullptr;
	sr           = spudgpu_map_buffer(chunk->buffer, 0, size, &mapped);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_transient_allocate: a chunk of %llu bytes can't be mapped: %s\n", (unsigned long long)size, spudresult_str(sr));
		aprend_transient_chunk_destroy(chunk);
		return nullptr;
	}
	chunk->mapped = (uint8_t *)mapped;
	return chunk;
}

// [offset] rounded up to a multiple of [alignment], a power of two.
static uint64_t aprend_transient_align(uint64_t offset, uint64_t alignment) { return (offset + alignment - 1) & ~(alignment - 1); }

SPUDRESULT aprend_transient_arena_flush(aprend_instance instance) {
	aprend_transient_frame &frame = instance->transient.frames[instance->frame_index];
	for (aprend_transient_chunk *chunk : frame.chunks) {
		if (chunk->write_offset == 0)
			continue;
		SPUDRESULT sr = spudgpu_flush_buffer(chunk->buffer, 0, chunk->write_offset);
		if (SPUDFAIL(sr))
			return sr;
	}
	return SPUD_SUCCESS;
}

void aprend_transient_arena_begin_frame(aprend_instance instance) {
	aprend_transient_arena &arena = instance->transient;
	aprend_transient_frame &frame = arena.frames[instance->frame_index];
	size_t kept                   = 0;
	for (aprend_transient_chunk *chunk : frame.chunks) {
		if (chunk->dedicated) {
			arena.allocated -= chunk->size;
			aprend_transient_chunk_destroy(chunk);
			continue;
		}
		aprend_transient_chunk_destroy_views(chunk);
		chunk->write_offset  = 0;
		frame.chunks[kept++] = chunk;
	}
	frame.chunks.resize(kept);
	frame.current = 0;
	++frame.epoch;
}

void aprend_transient_arena_destroy(aprend_instance instance) {
	aprend_transient_arena &arena = instance->transient;
	for (aprend_transient_frame &frame : arena.frames) {
		for (aprend_transient_chunk *chunk : frame.chunks)
			aprend_transient_chunk_destroy(chunk);
		frame.chunks.clear();
	}
	arena.allocated = 0;
}

bool aprend_transient_span_valid(
    aprend_instance instance,
    const aprend_transient_span *span,
    const char *who) {
	if (!span->_chunk) {
		printf("aprend: %s with a span that was never allocated\n", who);
		return false;
	}
	if (span->_frame != instance->frame_index) {
		printf("aprend: %s with a span taken in frame %u, and the frame is now %u\n", who, span->_frame, instance->frame_index);
		return false;
	}
	const aprend_transient_frame &frame = instance->transient.frames[span->_frame];
	if (span->_epoch != frame.epoch) {
		printf("aprend: %s with a span of an earlier turn of frame %u, which aprend_instance_next_frame has taken back\n", who, span->_frame);
		return false;
	}
	// The chunk is one of this frame's, and the span lies in what has been
	// handed out of it: a span whose private fields were changed fails here.
	const aprend_transient_chunk *chunk = nullptr;
	for (const aprend_transient_chunk *known : frame.chunks)
		if (known == span->_chunk)
			chunk = known;
	if (!chunk) {
		printf("aprend: %s with a span that names no chunk of frame %u\n", who, span->_frame);
		return false;
	}
	if (span->_offset > chunk->write_offset) {
		printf("aprend: %s with a span that starts past what its chunk has handed out\n", who);
		return false;
	}
	if (span->size > chunk->write_offset - span->_offset) {
		printf("aprend: %s with a span that runs past what its chunk has handed out\n", who);
		return false;
	}
	return true;
}

SPUDRESULT aprend_transient_span_view(
    aprend_instance instance,
    const aprend_transient_span *span,
    uint64_t stride,
    spudgpu_buffer_view *out_view) {
	(void)instance;
	aprend_transient_chunk *chunk = (aprend_transient_chunk *)span->_chunk;
	spudgpu_buffer_view_desc vd{};
	vd.parent_buffer             = chunk->buffer;
	vd.offset_from_parent_buffer = span->_offset;
	vd.stride                    = stride;
	vd.size                      = span->size;
	spudgpu_buffer_view view     = nullptr;
	SPUDRESULT sr                = spudgpu_create_buffer_view(chunk->buffer, &vd, &view);
	if (SPUDFAIL(sr))
		return sr;
	// The push can only fail for want of memory. No exception leaves a C
	// entry point, and this is reached from one.
	try {
		chunk->views.push_back(view);
	} catch (...) {
		spudgpu_destroy_buffer_view(view);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	*out_view = view;
	return SPUD_SUCCESS;
}

/* aprend_transient_allocate, and aprend_transient_allocate_staging with
 * [staging] set: then an instance with no transient memory, or a span that
 * would pass its limit, fails without a word, since the caller has another
 * way. */
static bool aprend_transient_take(
    aprend_instance instance,
    uint64_t size,
    uint64_t alignment,
    aprend_transient_span *out_span,
    bool staging) {
	static const char *who = "aprend_transient_allocate";
	if (!out_span) {
		printf("apricot: %s: out_span is NULL\n", who);
		return false;
	}
	*out_span = {};
	if (!instance) {
		printf("apricot: %s: instance is NULL\n", who);
		return false;
	}
	if (size == 0) {
		printf("apricot: %s: size is 0\n", who);
		return false;
	}
	if (alignment == 0) {
		printf("apricot: %s: alignment is 0\n", who);
		return false;
	}
	if (alignment & (alignment - 1)) {
		printf("apricot: %s: alignment %llu is not a power of two\n", who, (unsigned long long)alignment);
		return false;
	}
	const uint64_t chunk_size = instance->desc.transient_chunk_size;
	if (chunk_size == 0) {
		if (!staging)
			printf("apricot: %s: the instance was created with no transient memory (transient_chunk_size 0)\n", who);
		return false;
	}

	aprend_transient_arena &arena = instance->transient;
	aprend_transient_frame &frame = arena.frames[instance->frame_index];

	// The current chunk, or the first after it with room. The ones after it
	// are empty, and a chunk passed over stays passed over until the frame
	// is rewound: nothing goes back to fill its tail.
	aprend_transient_chunk *chunk = nullptr;
	uint64_t offset               = 0;
	while (frame.current < frame.chunks.size()) {
		aprend_transient_chunk *candidate = frame.chunks[frame.current];
		const uint64_t aligned            = aprend_transient_align(candidate->write_offset, alignment);
		if (aligned <= candidate->size && size <= candidate->size - aligned) {
			chunk  = candidate;
			offset = aligned;
			break;
		}
		++frame.current;
	}

	if (!chunk) {
		// One of the usual size, or one made for this span alone if it is
		// larger than that.
		const bool dedicated      = size > chunk_size;
		const uint64_t new_size   = dedicated ? size : chunk_size;
		const uint64_t limit      = instance->desc.transient_memory_limit;
		const bool over_limit     = limit != 0 && (arena.allocated > limit || new_size > limit - arena.allocated);
		if (over_limit) {
			if (staging)
				return false;
			printf("apricot: %s: %llu more bytes would pass transient_memory_limit (%llu of %llu in use)\n", who, (unsigned long long)new_size,
			       (unsigned long long)arena.allocated, (unsigned long long)limit);
			return false;
		}
		chunk = aprend_transient_chunk_create(instance, new_size);
		if (!chunk)
			return false;
		chunk->dedicated = dedicated;
		try {
			frame.chunks.push_back(chunk);
		} catch (...) {
			aprend_transient_chunk_destroy(chunk);
			return false;
		}
		arena.allocated += new_size;
		frame.current    = frame.chunks.size() - 1;
		offset           = 0;
	}

	chunk->write_offset = offset + size;

	out_span->data    = chunk->mapped + offset;
	out_span->size    = size;
	out_span->_chunk  = chunk;
	out_span->_offset = offset;
	out_span->_epoch  = frame.epoch;
	out_span->_frame  = instance->frame_index;
	return true;
}

bool aprend_transient_allocate_staging(
    aprend_instance instance,
    uint64_t size,
    uint64_t alignment,
    aprend_transient_span *out_span) {
	return aprend_transient_take(instance, size, alignment, out_span, true);
}

extern "C" {

bool aprend_transient_allocate(
    aprend_instance instance,
    uint64_t size,
    uint64_t alignment,
    aprend_transient_span *out_span) {
	return aprend_transient_take(instance, size, alignment, out_span, false);
}

uint64_t aprend_transient_get_frame_usage(aprend_instance instance) {
	if (!instance)
		return 0;
	uint64_t used = 0;
	for (const aprend_transient_chunk *chunk : instance->transient.frames[instance->frame_index].chunks)
		used += chunk->write_offset;
	return used;
}

} // Extern "C"
