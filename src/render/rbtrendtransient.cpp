
#include "render/rbtrendtransient.h"
#include "rbtrend_internal.hpp"

#include <new>

// Destroys [chunk]'s views, which nothing reads any more.
static void rbtrend_transient_chunk_destroy_views(rbtrend_transient_chunk *chunk) {
	for (const rbtrend_transient_view &kept : chunk->views)
		crgpu_buffer_view_destroy(kept.view);
	chunk->views.clear();
}

// At a rewind of [chunk]'s frame, which nothing still reads: destroys the
// views the turn that just ended didn't ask for, and marks the rest unused
// for the next.
static void rbtrend_transient_chunk_rewind_views(rbtrend_transient_chunk *chunk) {
	size_t kept = 0;
	for (size_t i = 0; i < chunk->views.size(); ++i) {
		rbtrend_transient_view v = chunk->views[i];
		if (!v.used) {
			crgpu_buffer_view_destroy(v.view);
			continue;
		}
		v.used                = false;
		chunk->views[kept++] = v;
	}
	(void)chunk->views.resize(kept); // smaller: nothing to allocate
}

// Destroys [chunk] and everything it holds. The GPU has finished with it.
static void rbtrend_transient_chunk_destroy(rbtrend_transient_chunk *chunk) {
	rbtrend_transient_chunk_destroy_views(chunk);
	if (chunk->mapped)
		crgpu_buffer_unmap(chunk->buffer);
	if (chunk->buffer)
		crgpu_buffer_destroy(chunk->buffer);
	chunk->~rbtrend_transient_chunk();
	free(chunk);
}

// A new chunk of [size] bytes, mapped, through [out_chunk]. Fails, [error]
// saying why, if the device refuses it.
static RBT_RESULT rbtrend_transient_chunk_create(
    rbtrend_instance instance,
    uint64_t size,
    rbtrend_transient_chunk **out_chunk,
    RBTREND_ERROR *error) {
	rbtrend_transient_chunk *chunk = (rbtrend_transient_chunk *)malloc(sizeof(rbtrend_transient_chunk));
	if (!chunk)
		return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory for a chunk of transient memory");
	chunk       = new (chunk) rbtrend_transient_chunk();
	chunk->size = size;

	// What a span is bound as, and a copy source so the same memory can
	// later stage writes to buffers in the device's memory.
	crgpu_buffer_desc bd{};
	bd.base.type   = CR_STRUCT_TYPE_GPU_BUFFER_DESC;
	bd.memory_kind = CRGPU_MEMORY_KIND_UPLOAD;
	bd.size        = size;
	bd.usage       = CRGPU_BUFFER_USAGE_VERTEX | CRGPU_BUFFER_USAGE_INDEX | CRGPU_BUFFER_USAGE_TRANSFER_SRC;
	CR_RESULT sr  = crgpu_buffer_create(instance->desc.device, &bd, &chunk->buffer);
	if (CR_FAILED(sr)) {
		chunk->buffer = nullptr;
		rbtrend_transient_chunk_destroy(chunk);
		return rbtrend_fail_cr(error, sr, "creating a chunk of %llu bytes failed", (unsigned long long)size);
	}
	void *mapped = nullptr;
	sr           = crgpu_buffer_map(chunk->buffer, 0, size, &mapped);
	if (CR_FAILED(sr)) {
		rbtrend_transient_chunk_destroy(chunk);
		return rbtrend_fail_cr(error, sr, "mapping a chunk of %llu bytes failed", (unsigned long long)size);
	}
	chunk->mapped = (uint8_t *)mapped;
	*out_chunk    = chunk;
	return RBT_RESULT_OK;
}

// [offset] rounded up to a multiple of [alignment], a power of two.
static uint64_t rbtrend_transient_align(uint64_t offset, uint64_t alignment) { return (offset + alignment - 1) & ~(alignment - 1); }

CR_RESULT rbtrend_transient_arena_flush(rbtrend_instance instance) {
	rbtrend_transient_frame &frame = instance->transient.frames[instance->frame_index];
	for (rbtrend_transient_chunk *chunk : frame.chunks) {
		if (chunk->write_offset == 0)
			continue;
		CR_RESULT sr = crgpu_buffer_flush(chunk->buffer, 0, chunk->write_offset);
		if (CR_FAILED(sr))
			return sr;
	}
	return CR_SUCCESS;
}

void rbtrend_transient_arena_begin_frame(rbtrend_instance instance) {
	rbtrend_transient_arena &arena = instance->transient;
	rbtrend_transient_frame &frame = arena.frames[instance->frame_index];
	size_t kept                   = 0;
	for (rbtrend_transient_chunk *chunk : frame.chunks) {
		if (chunk->dedicated) {
			arena.allocated -= chunk->size;
			rbtrend_transient_chunk_destroy(chunk);
			continue;
		}
		rbtrend_transient_chunk_rewind_views(chunk);
		chunk->write_offset  = 0;
		frame.chunks[kept++] = chunk;
	}
	frame.chunks.resize(kept);
	frame.current = 0;
	++frame.epoch;
}

void rbtrend_transient_arena_destroy(rbtrend_instance instance) {
	rbtrend_transient_arena &arena = instance->transient;
	for (rbtrend_transient_frame &frame : arena.frames) {
		for (rbtrend_transient_chunk *chunk : frame.chunks)
			rbtrend_transient_chunk_destroy(chunk);
		frame.chunks.clear();
	}
	arena.allocated = 0;
}

bool rbtrend_transient_span_valid(
    rbtrend_instance instance,
    const RBTREND_TRANSIENT_SPAN *span,
    const char *who) {
	if (!span->_chunk) {
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "%s with a span that was never allocated", who);
		return false;
	}
	if (span->_frame != instance->frame_index) {
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "%s with a span taken in frame %u, and the frame is now %u", who, span->_frame, instance->frame_index);
		return false;
	}
	const rbtrend_transient_frame &frame = instance->transient.frames[span->_frame];
	if (span->_epoch != frame.epoch) {
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "%s with a span of an earlier turn of frame %u, which rbtrend_instance_next_frame has taken back", who, span->_frame);
		return false;
	}
	// The chunk is one of this frame's, and the span lies in what has been
	// handed out of it: a span whose private fields were changed fails here.
	const rbtrend_transient_chunk *chunk = nullptr;
	for (const rbtrend_transient_chunk *known : frame.chunks)
		if (known == span->_chunk)
			chunk = known;
	if (!chunk) {
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "%s with a span that names no chunk of frame %u", who, span->_frame);
		return false;
	}
	if (span->_offset > chunk->write_offset) {
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "%s with a span that starts past what its chunk has handed out", who);
		return false;
	}
	if (span->size > chunk->write_offset - span->_offset) {
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "%s with a span that runs past what its chunk has handed out", who);
		return false;
	}
	return true;
}

CR_RESULT rbtrend_transient_span_view(
    rbtrend_instance instance,
    const RBTREND_TRANSIENT_SPAN *span,
    uint64_t stride,
    crgpu_buffer_view *out_view) {
	(void)instance;
	rbtrend_transient_chunk *chunk = (rbtrend_transient_chunk *)span->_chunk;
	for (rbtrend_transient_view &kept : chunk->views) {
		if (kept.offset == span->_offset && kept.stride == stride) {
			kept.used = true;
			*out_view = kept.view;
			return CR_SUCCESS;
		}
	}
	crgpu_buffer_view_desc vd{};
	vd.base.type = CR_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	vd.parent_buffer             = chunk->buffer;
	vd.offset_from_parent_buffer = span->_offset;
	vd.stride                    = stride;
	// To the end of the chunk, not the span: a span at this offset next
	// frame may be another size (a draw list's instances, as many as are
	// visible), and the view reaches it too. Binding a vertex or index
	// buffer reads only what the draw names.
	vd.size                      = chunk->size - span->_offset;
	crgpu_buffer_view view     = nullptr;
	CR_RESULT sr                = crgpu_buffer_view_create(chunk->buffer, &vd, &view);
	if (CR_FAILED(sr))
		return sr;
	// The push can only fail for want of memory.
	if (chunk->views.push(rbtrend_transient_view{view, span->_offset, stride, chunk->size - span->_offset, true}) != EQ_SUCCESS) {
		crgpu_buffer_view_destroy(view);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	*out_view = view;
	return CR_SUCCESS;
}

/* rbtrend_transient_allocate, and rbtrend_transient_allocate_staging, which
 * passes no [error]: an instance with no transient memory, or a span that
 * would pass its limit, is a failure it takes another way after. */
static RBT_RESULT rbtrend_transient_take(
    rbtrend_instance instance,
    uint64_t size,
    uint64_t alignment,
    RBTREND_TRANSIENT_SPAN *out_span,
    RBTREND_ERROR *error) {
	if (out_span)
		*out_span = {};
	if (!instance)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (size == 0)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "size is 0");
	if (alignment == 0)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "alignment is 0");
	if (alignment & (alignment - 1))
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "alignment %llu is not a power of two", (unsigned long long)alignment);
	if (!out_span)
		return rbtrend_fail(error, RBT_RESULT_INVALID_ARGUMENT, "out_span is NULL");
	const uint64_t chunk_size = instance->desc.transient_chunk_size;
	if (chunk_size == 0)
		return rbtrend_fail(error, RBT_RESULT_LIMIT, "the instance was created with no transient memory (transient_chunk_size 0)");

	rbtrend_transient_arena &arena = instance->transient;
	rbtrend_transient_frame &frame = arena.frames[instance->frame_index];

	// The current chunk, or the first after it with room. The ones after it
	// are empty, and a chunk passed over stays passed over until the frame
	// is rewound: nothing goes back to fill its tail.
	rbtrend_transient_chunk *chunk = nullptr;
	uint64_t offset               = 0;
	while (frame.current < frame.chunks.size()) {
		rbtrend_transient_chunk *candidate = frame.chunks[frame.current];
		const uint64_t aligned            = rbtrend_transient_align(candidate->write_offset, alignment);
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
		if (over_limit)
			return rbtrend_fail(error, RBT_RESULT_LIMIT, "%llu more bytes would pass transient_memory_limit (%llu of %llu in use)", (unsigned long long)new_size,
			                   (unsigned long long)arena.allocated, (unsigned long long)limit);
		if (const RBT_RESULT made = rbtrend_transient_chunk_create(instance, new_size, &chunk, error))
			return made;
		chunk->dedicated = dedicated;
		try {
			frame.chunks.push_back(chunk);
		} catch (...) {
			rbtrend_transient_chunk_destroy(chunk);
			return rbtrend_fail(error, RBT_RESULT_OUT_OF_MEMORY, "no memory to keep a chunk of transient memory");
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
	return RBT_RESULT_OK;
}

bool rbtrend_transient_allocate_staging(
    rbtrend_instance instance,
    uint64_t size,
    uint64_t alignment,
    RBTREND_TRANSIENT_SPAN *out_span) {
	return rbtrend_transient_take(instance, size, alignment, out_span, nullptr) == RBT_RESULT_OK;
}

extern "C" {

RBT_RESULT rbtrend_transient_allocate(
    rbtrend_instance instance,
    uint64_t size,
    uint64_t alignment,
    RBTREND_TRANSIENT_SPAN *out_span,
    RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (instance && RBT_STALE(instance, "rbtrend_transient_allocate", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	return rbtrend_transient_take(instance, size, alignment, out_span, out_error);
}

uint64_t rbtrend_transient_get_frame_usage(rbtrend_instance instance) {
	if (!instance)
		return 0;
	if (RBT_STALE(instance, "rbtrend_transient_get_frame_usage", "instance"))
		return 0;
	uint64_t used = 0;
	for (const rbtrend_transient_chunk *chunk : instance->transient.frames[instance->frame_index].chunks)
		used += chunk->write_offset;
	return used;
}

} // Extern "C"
