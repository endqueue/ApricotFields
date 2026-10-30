#ifndef APREND_BATCH_INTERNAL_HPP
#define APREND_BATCH_INTERNAL_HPP

/* Internal header - never included outside ApricotFields/src/render/.
 *
 * The batcher (aprendbatch.h): a frame's records, written straight into a
 * transient span, and the runs of one kind that become one instanced draw
 * each when APREND_COMMAND_DRAW_BATCH is sent. */

#include "aprend_internal.hpp"
#include "render/aprendbatch.h"
#include "../apriplaindata.hpp"

#include <eqstd.hpp>

/* One shape, as aprend_batch.vert reads it per instance: eight vec4s. The
 * kinds' use of them is in aprendbatch.cpp, beside the writers. */
struct aprend_batch_record {
	float r[8][4];
};
APRI_PLAIN_DATA(aprend_batch_record);
APRI_HOT_RECORD(aprend_batch_record, 128);

/* Consecutive records of one kind: one instanced draw. */
struct aprend_batch_run {
	uint32_t kind;
	uint32_t first;
	uint32_t count;
};
APRI_PLAIN_DATA(aprend_batch_run);

/* AprendBatchPush in shaders/aprend_batch.glslh. */
struct aprend_batch_push {
	float view_projection[16];
	float viewport[4];
	float params[4];
};

/* The depth modes a batch has pipelines for: over, tested, tested reversed. */
enum { APREND_BATCH_DEPTH_MODES = 3, APREND_BATCH_KINDS = 4 };

typedef struct aprend_batch_t {
	const char *debug_name{nullptr}; /* first, as every handle's (apricore.h) */
	aprend_instance instance{nullptr};
	char *name_copy{nullptr};
	SPUDGPU_FORMAT color_format{};
	SPUDGPU_FORMAT depth_format{};
	uint32_t max_shapes{0};
	aprend_shader vert{nullptr};
	aprend_shader frags[APREND_BATCH_KINDS]{};
	aprend_binding_layout layout{nullptr};
	aprend_graphics_pipeline pipelines[APREND_BATCH_DEPTH_MODES][APREND_BATCH_KINDS]{};
	aprend_material material{nullptr};
	aprend_sampler sampler{nullptr};
	/* Bound where a frame has no texture or atlas: never written, never
	 * sampled (the add calls refuse what would read them). */
	aprend_texture2d placeholder{nullptr};
	aprend_texture_view placeholder_2d{nullptr};
	aprend_texture_view placeholder_array{nullptr};

	/* The frame. */
	bool begun{false};
	APREND_BATCH_FRAME frame{};
	aprend_batch_push push{};
	APREND_TRANSIENT_SPAN span{};
	uint32_t count{0};
	uint32_t atlas_layers{0};
	eq::array<aprend_batch_run> runs;
} aprend_batch_t;

/* APREND_COMMAND_DRAW_BATCH, at send: the batch's runs as pipelines, its
 * material, its push constants, its span and an instanced draw a run.
 * False, reported, if the batch can't be drawn. */
bool aprend_batch_expand(aprend_command_list cmd_list, aprend_batch batch);

#endif // APREND_BATCH_INTERNAL_HPP
