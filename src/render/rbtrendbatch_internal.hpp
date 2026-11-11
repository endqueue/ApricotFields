#ifndef RBTREND_BATCH_INTERNAL_HPP
#define RBTREND_BATCH_INTERNAL_HPP

/* Internal header - never included outside rabbittus/src/render/.
 *
 * The batcher (rbtrendbatch.h): a frame's records, written straight into a
 * transient span, and the runs of one kind that become one instanced draw
 * each when RBTREND_COMMAND_DRAW_BATCH is sent. */

#include "rbtrend_internal.hpp"
#include "render/rbtrendbatch.h"
#include "../rbtplaindata.hpp"

#include <eqstd.hpp>

/* One shape, as rbtrend_batch_vert.slang reads it per instance: eight vec4s. The
 * kinds' use of them is in rbtrendbatch.cpp, beside the writers. */
struct rbtrend_batch_record {
	float r[8][4];
};
RBT_PLAIN_DATA(rbtrend_batch_record);
RBT_HOT_RECORD(rbtrend_batch_record, 128);

/* Consecutive records of one kind: one instanced draw. */
struct rbtrend_batch_run {
	uint32_t kind;
	uint32_t first;
	uint32_t count;
};
RBT_PLAIN_DATA(rbtrend_batch_run);

/* RbtrendBatchPush in shaders/rbtrend_batch.slang. */
struct rbtrend_batch_push {
	float view_projection[16];
	float viewport[4];
	float params[4];
};

/* The depth modes a batch has pipelines for: over, tested, tested reversed. */
enum { RBTREND_BATCH_DEPTH_MODES = 3, RBTREND_BATCH_KINDS = 4 };

/* A segment of a line after the near plane: its ends (world or screen
 * points), and whether each touches the next or previous visible segment. */
struct rbtrend_batch_segment {
	float a[3];
	float b[3];
	float a_px[2];
	float b_px[2];
	bool joined_before; /* continues the visible segment before it */
};
RBT_PLAIN_DATA(rbtrend_batch_segment);

typedef struct rbtrend_batch_t {
	const char *debug_name{nullptr}; /* first, as every handle's (rbtcore.h) */
	RBT_STALE_TAG
	rbtrend_instance instance{nullptr};
	char *name_copy{nullptr};
	CRGPU_FORMAT color_format{};
	CRGPU_FORMAT depth_format{};
	uint32_t max_shapes{0};
	rbtrend_shader vert{nullptr};
	rbtrend_shader frags[RBTREND_BATCH_KINDS]{};
	rbtrend_binding_layout layout{nullptr};
	rbtrend_graphics_pipeline pipelines[RBTREND_BATCH_DEPTH_MODES][RBTREND_BATCH_KINDS]{};
	rbtrend_material material{nullptr};
	rbtrend_sampler sampler{nullptr};
	/* Bound where a frame has no texture or atlas: never written, never
	 * sampled (the add calls refuse what would read them). */
	rbtrend_texture2d placeholder{nullptr};
	rbtrend_texture_view placeholder_2d{nullptr};
	rbtrend_texture_view placeholder_array{nullptr};

	/* The frame. */
	bool begun{false};
	RBTREND_BATCH_FRAME frame{};
	rbtrend_batch_push push{};
	RBTREND_TRANSIENT_SPAN span{};
	uint32_t count{0};
	uint32_t atlas_layers{0};
	eq::array<rbtrend_batch_run> runs;
	/* rbtrend_batch_add_line's working array, the line it is adding cut at
	 * the near plane; kept with its capacity from line to line. */
	eq::array<rbtrend_batch_segment> segments;
} rbtrend_batch_t;

/* RBTREND_COMMAND_DRAW_BATCH, at send: the batch's runs as pipelines, its
 * material, its push constants, its span and an instanced draw a run.
 * False, reported, if the batch can't be drawn. */
bool rbtrend_batch_expand(rbtrend_command_list cmd_list, rbtrend_batch batch);

#endif // RBTREND_BATCH_INTERNAL_HPP
