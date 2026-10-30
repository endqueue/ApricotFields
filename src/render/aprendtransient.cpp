
#include "render/aprendtransient.h"
#include "aprend_internal.hpp"

#include <new>

// Destroys [chunk]'s views, which nothing reads any more.
static void aprend_transient_chunk_destroy_views(aprend_transient_chunk *chunk) {
	for (const aprend_transient_view &kept : chunk->views)
		spudgpu_destroy_buffer_view(kept.view);
	chunk->views.clear();
}

// At a rewind of [chunk]'s frame, which nothing still reads: destroys the
// views the turn that just ended didn't ask for, and marks the rest unused
// for the next.
static void aprend_transient_chunk_rewind_views(aprend_transient_chunk *chunk) {
	size_t kept = 0;
	for (size_t i = 0; i < chunk->views.size(); ++i) {
		aprend_transient_view v = chunk->views[i];
		if (!v.used) {
			spudgpu_destroy_buffer_view(v.view);
			continue;
		}
		v.used                = false;
		chunk->views[kept++] = v;
	}
	(void)chunk->views.resize(kept); // smaller: nothing to allocate
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

// A new chunk of [size] bytes, mapped, through [out_chunk]. Fails, [error]
// saying why, if the device refuses it.
static APRESULT aprend_transient_chunk_create(
    aprend_instance instance,
    uint64_t size,
    aprend_transient_chunk **out_chunk,
    APREND_ERROR *error) {
	aprend_transient_chunk *chunk = (aprend_transient_chunk *)malloc(sizeof(aprend_transient_chunk));
	if (!chunk)
		return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory for a chunk of transient memory");
	chunk       = new (chunk) aprend_transient_chunk();
	chunk->size = size;

	// What a span is bound as, and a copy source so the same memory can
	// later stage writes to buffers in the device's memory.
	spudgpu_buffer_desc bd{};
	bd.base.type   = SPUD_STRUCT_TYPE_GPU_BUFFER_DESC;
	bd.memory_kind = SPUDGPU_MEMORY_KIND_UPLOAD;
	bd.size        = size;
	bd.usage       = SPUDGPU_BUFFER_USAGE_VERTEX | SPUDGPU_BUFFER_USAGE_INDEX | SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	SPUDRESULT sr  = spudgpu_create_buffer(instance->desc.device, &bd, &chunk->buffer);
	if (SPUDFAIL(sr)) {
		chunk->buffer = nullptr;
		aprend_transient_chunk_destroy(chunk);
		return aprend_fail_spud(error, sr, "creating a chunk of %llu bytes failed", (unsigned long long)size);
	}
	void *mapped = nullptr;
	sr           = spudgpu_map_buffer(chunk->buffer, 0, size, &mapped);
	if (SPUDFAIL(sr)) {
		aprend_transient_chunk_destroy(chunk);
		return aprend_fail_spud(error, sr, "mapping a chunk of %llu bytes failed", (unsigned long long)size);
	}
	chunk->mapped = (uint8_t *)mapped;
	*out_chunk    = chunk;
	return APRESULT_OK;
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
		aprend_transient_chunk_rewind_views(chunk);
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
    const APREND_TRANSIENT_SPAN *span,
    const char *who) {
	if (!span->_chunk) {
		aprend_report(instance, APRESULT_INVALID_ARGUMENT, "%s with a span that was never allocated", who);
		return false;
	}
	if (span->_frame != instance->frame_index) {
		aprend_report(instance, APRESULT_INVALID_ARGUMENT, "%s with a span taken in frame %u, and the frame is now %u", who, span->_frame, instance->frame_index);
		return false;
	}
	const aprend_transient_frame &frame = instance->transient.frames[span->_frame];
	if (span->_epoch != frame.epoch) {
		aprend_report(instance, APRESULT_INVALID_ARGUMENT, "%s with a span of an earlier turn of frame %u, which aprend_instance_next_frame has taken back", who, span->_frame);
		return false;
	}
	// The chunk is one of this frame's, and the span lies in what has been
	// handed out of it: a span whose private fields were changed fails here.
	const aprend_transient_chunk *chunk = nullptr;
	for (const aprend_transient_chunk *known : frame.chunks)
		if (known == span->_chunk)
			chunk = known;
	if (!chunk) {
		aprend_report(instance, APRESULT_INVALID_ARGUMENT, "%s with a span that names no chunk of frame %u", who, span->_frame);
		return false;
	}
	if (span->_offset > chunk->write_offset) {
		aprend_report(instance, APRESULT_INVALID_ARGUMENT, "%s with a span that starts past what its chunk has handed out", who);
		return false;
	}
	if (span->size > chunk->write_offset - span->_offset) {
		aprend_report(instance, APRESULT_INVALID_ARGUMENT, "%s with a span that runs past what its chunk has handed out", who);
		return false;
	}
	return true;
}

SPUDRESULT aprend_transient_span_view(
    aprend_instance instance,
    const APREND_TRANSIENT_SPAN *span,
    uint64_t stride,
    spudgpu_buffer_view *out_view) {
	(void)instance;
	aprend_transient_chunk *chunk = (aprend_transient_chunk *)span->_chunk;
	for (aprend_transient_view &kept : chunk->views) {
		if (kept.offset == span->_offset && kept.stride == stride) {
			kept.used = true;
			*out_view = kept.view;
			return SPUD_SUCCESS;
		}
	}
	spudgpu_buffer_view_desc vd{};
	vd.base.type = SPUD_STRUCT_TYPE_GPU_BUFFER_VIEW_DESC;
	vd.parent_buffer             = chunk->buffer;
	vd.offset_from_parent_buffer = span->_offset;
	vd.stride                    = stride;
	// To the end of the chunk, not the span: a span at this offset next
	// frame may be another size (a draw list's instances, as many as are
	// visible), and the view reaches it too. Binding a vertex or index
	// buffer reads only what the draw names.
	vd.size                      = chunk->size - span->_offset;
	spudgpu_buffer_view view     = nullptr;
	SPUDRESULT sr                = spudgpu_create_buffer_view(chunk->buffer, &vd, &view);
	if (SPUDFAIL(sr))
		return sr;
	// The push can only fail for want of memory.
	if (chunk->views.push(aprend_transient_view{view, span->_offset, stride, chunk->size - span->_offset, true}) != EQ_SUCCESS) {
		spudgpu_destroy_buffer_view(view);
		return SPUDRESULT_OUT_OF_MEMORY;
	}
	*out_view = view;
	return SPUD_SUCCESS;
}

/* aprend_transient_allocate, and aprend_transient_allocate_staging, which
 * passes no [error]: an instance with no transient memory, or a span that
 * would pass its limit, is a failure it takes another way after. */
static APRESULT aprend_transient_take(
    aprend_instance instance,
    uint64_t size,
    uint64_t alignment,
    APREND_TRANSIENT_SPAN *out_span,
    APREND_ERROR *error) {
	if (out_span)
		*out_span = {};
	if (!instance)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (size == 0)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "size is 0");
	if (alignment == 0)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "alignment is 0");
	if (alignment & (alignment - 1))
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "alignment %llu is not a power of two", (unsigned long long)alignment);
	if (!out_span)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "out_span is NULL");
	const uint64_t chunk_size = instance->desc.transient_chunk_size;
	if (chunk_size == 0)
		return aprend_fail(error, APRESULT_LIMIT, "the instance was created with no transient memory (transient_chunk_size 0)");

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
		if (over_limit)
			return aprend_fail(error, APRESULT_LIMIT, "%llu more bytes would pass transient_memory_limit (%llu of %llu in use)", (unsigned long long)new_size,
			                   (unsigned long long)arena.allocated, (unsigned long long)limit);
		if (const APRESULT made = aprend_transient_chunk_create(instance, new_size, &chunk, error))
			return made;
		chunk->dedicated = dedicated;
		try {
			frame.chunks.push_back(chunk);
		} catch (...) {
			aprend_transient_chunk_destroy(chunk);
			return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "no memory to keep a chunk of transient memory");
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
	return APRESULT_OK;
}

bool aprend_transient_allocate_staging(
    aprend_instance instance,
    uint64_t size,
    uint64_t alignment,
    APREND_TRANSIENT_SPAN *out_span) {
	return aprend_transient_take(instance, size, alignment, out_span, nullptr) == APRESULT_OK;
}

extern "C" {

APRESULT aprend_transient_allocate(
    aprend_instance instance,
    uint64_t size,
    uint64_t alignment,
    APREND_TRANSIENT_SPAN *out_span,
    APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (instance && APRI_STALE(instance, "aprend_transient_allocate", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	return aprend_transient_take(instance, size, alignment, out_span, out_error);
}

uint64_t aprend_transient_get_frame_usage(aprend_instance instance) {
	if (!instance)
		return 0;
	if (APRI_STALE(instance, "aprend_transient_get_frame_usage", "instance"))
		return 0;
	uint64_t used = 0;
	for (const aprend_transient_chunk *chunk : instance->transient.frames[instance->frame_index].chunks)
		used += chunk->write_offset;
	return used;
}

} // Extern "C"
