#ifndef APREND_TRANSIENT_INTERNAL_HPP
#define APREND_TRANSIENT_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/.
 *
 * An instance's transient memory (render/aprendtransient.h): mapped
 * SPUDGPU_MEMORY_KIND_UPLOAD buffers, handed out front to back, one run of
 * them for each frame in flight. Nothing is tracked for a span. A frame's
 * chunks are rewound together when aprend_instance_next_frame comes round to
 * that frame index again, which is after it has waited for everything
 * submitted in that frame. */

#include "render/aprendcontext.h"
#include "render/aprendtransient.h"

#include <spudgpu.h>

#include <vector>

struct aprend_transient_chunk {
	/* Vertex, index and transfer source usage, mapped for as long as it
	 * exists. Never NULL in a chunk that is in a frame's list. */
	spudgpu_buffer buffer{nullptr};
	uint8_t *mapped{nullptr};
	uint64_t size{0};
	/* The first byte not handed out. */
	uint64_t write_offset{0};
	/* Made for one span larger than the arena's chunk size: destroyed when
	 * its frame is rewound, where an ordinary chunk is kept and used again. */
	bool dedicated{false};
	/* The views command lists bound spans of this chunk through, destroyed
	 * when the frame is rewound. */
	std::vector<spudgpu_buffer_view> views;
};

struct aprend_transient_frame {
	std::vector<aprend_transient_chunk *> chunks;
	/* The chunk spans are being taken from: an index into chunks. Chunks
	 * before it are full for this frame, chunks after it untouched. */
	size_t current{0};
	/* Goes up each time the frame is rewound, and a span carries the value
	 * it was taken under: a span of an earlier turn of this frame index is
	 * told apart from a live one. Starts at 1, so a zeroed span is never
	 * live. */
	uint64_t epoch{1};
};

struct aprend_transient_arena {
	aprend_transient_frame frames[APREND_MAX_FRAMES_IN_FLIGHT];
	/* Bytes in every chunk that exists, in every frame. */
	uint64_t allocated{0};
};

/* Before a submission: SpudGPU's flush over everything written in the
 * current frame so far. All of it each time, not what is new since the last
 * submission, because a span may be written after a submission that didn't
 * read it. */
SPUDRESULT aprend_transient_arena_flush(aprend_instance instance);

/* From aprend_instance_next_frame, once it has waited for the GPU to finish
 * what was submitted the last time the now current frame index was current:
 * takes back every span taken then. */
void aprend_transient_arena_begin_frame(aprend_instance instance);

/* From the instance's destructor, after the wait for the GPU. */
void aprend_transient_arena_destroy(aprend_instance instance);

/* Whether [span] was taken in the instance's current frame and is still
 * live. [who] names the command, for the message. */
bool aprend_transient_span_valid(
    aprend_instance instance,
    const aprend_transient_span *span,
    const char *who);

/* aprend_transient_allocate for staging an upload, which has a buffer of
 * its own to fall back on: false, printing nothing, if the instance has no
 * transient memory or the span would pass transient_memory_limit. The span's
 * chunk is a copy source, and its bytes are flushed by the submission that
 * reads them, which is in the current frame. */
bool aprend_transient_allocate_staging(
    aprend_instance instance,
    uint64_t size,
    uint64_t alignment,
    aprend_transient_span *out_span);

/* A view of [span] with elements of [stride] bytes, for a command list to
 * bind. It is the arena's: destroyed when the span's frame is rewound.
 * [span] has passed aprend_transient_span_valid. */
SPUDRESULT aprend_transient_span_view(
    aprend_instance instance,
    const aprend_transient_span *span,
    uint64_t stride,
    spudgpu_buffer_view *out_view);

#endif // APREND_TRANSIENT_INTERNAL_HPP
