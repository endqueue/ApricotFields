#ifndef RBTREND_TRANSIENT_INTERNAL_HPP
#define RBTREND_TRANSIENT_INTERNAL_HPP

/* Internal header — never included outside rabbittus/src/render/.
 *
 * An instance's transient memory (render/rbtrendtransient.h): mapped
 * CRGPU_MEMORY_KIND_UPLOAD buffers, handed out front to back, one run of
 * them for each frame in flight. Nothing is tracked for a span. A frame's
 * chunks are rewound together when rbtrend_instance_next_frame comes round to
 * that frame index again, which is after it has waited for everything
 * submitted in that frame. */

#include "render/rbtrendcontext.h"
#include "render/rbtrendtransient.h"

#include "../rbtplaindata.hpp"
#include <eqstd.hpp>
#include <crgpu.h>

#include <vector>

/* A view of a transient chunk from [offset] to its end, for spans that
 * start there with [stride]; [size] is the view's. */
struct rbtrend_transient_view {
	crgpu_buffer_view view;
	uint64_t offset;
	uint64_t stride;
	uint64_t size;
	/* Asked for since the chunk's frame was last rewound. */
	bool used;
};
RBT_PLAIN_DATA(rbtrend_transient_view);

struct rbtrend_transient_chunk {
	/* Vertex, index and transfer source usage, mapped for as long as it
	 * exists. Never NULL in a chunk that is in a frame's list. */
	crgpu_buffer buffer{nullptr};
	uint8_t *mapped{nullptr};
	uint64_t size{0};
	/* The first byte not handed out. */
	uint64_t write_offset{0};
	/* Made for one span larger than the arena's chunk size: destroyed when
	 * its frame is rewound, where an ordinary chunk is kept and used again. */
	bool dedicated{false};
	/* The views command lists bound spans of this chunk through, one per
	 * offset and stride, each reaching the chunk's end. A frame whose spans
	 * start where the last turn of its index's did (whatever their sizes)
	 * asks for the same views, so one asked for again is the one already made, and a
	 * rewind destroys only those the turn before it didn't ask for: a
	 * steady frame makes no view, and one that changes keeps no more than
	 * it uses (no allocation per frame, ROADMAP.md 3.1). */
	eq::array<rbtrend_transient_view> views;
};

struct rbtrend_transient_frame {
	std::vector<rbtrend_transient_chunk *> chunks;
	/* The chunk spans are being taken from: an index into chunks. Chunks
	 * before it are full for this frame, chunks after it untouched. */
	size_t current{0};
	/* Goes up each time the frame is rewound, and a span carries the value
	 * it was taken under: a span of an earlier turn of this frame index is
	 * told apart from a live one. Starts at 1, so a zeroed span is never
	 * live. */
	uint64_t epoch{1};
};

struct rbtrend_transient_arena {
	rbtrend_transient_frame frames[RBTREND_MAX_FRAMES_IN_FLIGHT];
	/* Bytes in every chunk that exists, in every frame. */
	uint64_t allocated{0};
};

/* Before a submission: Carrottus GPU's flush over everything written in the
 * current frame so far. All of it each time, not what is new since the last
 * submission, because a span may be written after a submission that didn't
 * read it. */
CR_RESULT rbtrend_transient_arena_flush(rbtrend_instance instance);

/* From rbtrend_instance_next_frame, once it has waited for the GPU to finish
 * what was submitted the last time the now current frame index was current:
 * takes back every span taken then. */
void rbtrend_transient_arena_begin_frame(rbtrend_instance instance);

/* From the instance's destructor, after the wait for the GPU. */
void rbtrend_transient_arena_destroy(rbtrend_instance instance);

/* Whether [span] was taken in the instance's current frame and is still
 * live. [who] names the command, for the message. */
bool rbtrend_transient_span_valid(
    rbtrend_instance instance,
    const RBTREND_TRANSIENT_SPAN *span,
    const char *who);

/* rbtrend_transient_allocate for staging an upload, which has a buffer of
 * its own to fall back on: false, printing nothing, if the instance has no
 * transient memory or the span would pass transient_memory_limit. The span's
 * chunk is a copy source, and its bytes are flushed by the submission that
 * reads them, which is in the current frame. */
bool rbtrend_transient_allocate_staging(
    rbtrend_instance instance,
    uint64_t size,
    uint64_t alignment,
    RBTREND_TRANSIENT_SPAN *out_span);

/* A view of [span] with elements of [stride] bytes, for a command list to
 * bind. It is the arena's: destroyed when the span's frame is rewound.
 * [span] has passed rbtrend_transient_span_valid. */
CR_RESULT rbtrend_transient_span_view(
    rbtrend_instance instance,
    const RBTREND_TRANSIENT_SPAN *span,
    uint64_t stride,
    crgpu_buffer_view *out_view);

#endif // RBTREND_TRANSIENT_INTERNAL_HPP
