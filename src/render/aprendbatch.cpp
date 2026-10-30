#include "aprendbatch_internal.hpp"
#include "aprendimages_internal.hpp"
#include "render/aprendmaterial.h"
#include "render/aprendpipeline.h"
#include "render/aprendshader.h"
#include "render/aprendtransient.h"

#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

// The batcher (aprendbatch.h). A frame's records go straight into a
// transient span taken at aprend_batch_begin(); the add calls check
// everything first, then write, so a refused call adds nothing. What each
// kind keeps in a record's eight vec4s (aprend_batch.vert reads them):
//
//   quad:   r0 origin, r1 edge x, r2 edge y, r3.w flags, r4 color, r5 uv rect
//   circle: r0 centre, r1 radius x, r2 radius y, r3.w flags, r4 color,
//           r5 (inner, start, sweep)
//   line:   r0 start (w: the dash phase there), r1 end (w: width in
//           pixels), r2 previous point (w: mitre limit), r3 next point
//           (w: flags), r4 color, r5 and r6 the dashes in pixels, r7.z the
//           pattern's length
//   glyph:  r0 origin, r1 edge x, r2 edge y, r3.w flags, r4 color, r5 uv
//           rect, r6 outline color, r7 (outline width, layer)
//
// Flags are a float holding a small integer: the kind in the low two bits,
// then the kind's own (shaders/aprend_batch.glslh).

static const char *const s_name = "aprend_batch";

static const uint32_t s_batch_vert_spv[] =
#include "aprend_batch.vert.spv.inc"
    ;
static const uint32_t s_batch_quad_frag_spv[] =
#include "aprend_batch_quad.frag.spv.inc"
    ;
static const uint32_t s_batch_circle_frag_spv[] =
#include "aprend_batch_circle.frag.spv.inc"
    ;
static const uint32_t s_batch_line_frag_spv[] =
#include "aprend_batch_line.frag.spv.inc"
    ;
static const uint32_t s_batch_glyph_frag_spv[] =
#include "aprend_batch_glyph.frag.spv.inc"
    ;

enum : uint32_t {
	KIND_QUAD   = 0,
	KIND_CIRCLE = 1,
	KIND_LINE   = 2,
	KIND_GLYPH  = 3,
	// Above the kind (shaders/aprend_batch.glslh).
	QUAD_TEXTURED   = 4,
	LINE_HAS_PREV   = 4,
	LINE_HAS_NEXT   = 8,
	LINE_CAP_SHIFT  = 4,
	LINE_JOIN_SHIFT = 6,
};

static const APREND_VERTEX_ELEMENT s_record_elements[] = {
    {"in_r0", APREND_VERTEX_ELEMENT_TYPE_VEC4, 0},   {"in_r1", APREND_VERTEX_ELEMENT_TYPE_VEC4, 16},
    {"in_r2", APREND_VERTEX_ELEMENT_TYPE_VEC4, 32},  {"in_r3", APREND_VERTEX_ELEMENT_TYPE_VEC4, 48},
    {"in_r4", APREND_VERTEX_ELEMENT_TYPE_VEC4, 64},  {"in_r5", APREND_VERTEX_ELEMENT_TYPE_VEC4, 80},
    {"in_r6", APREND_VERTEX_ELEMENT_TYPE_VEC4, 96},  {"in_r7", APREND_VERTEX_ELEMENT_TYPE_VEC4, 112},
};
static_assert(sizeof(aprend_batch_record) == 128, "aprend_batch.vert reads eight vec4s a record");
static_assert(sizeof(aprend_batch_push) == 96, "AprendBatchPush's size");

static bool finite_all(const float *v, size_t n) {
	for (size_t i = 0; i < n; ++i)
		if (!isfinite(v[i]))
			return false;
	return true;
}

static bool is_depth_format(SPUDGPU_FORMAT format) {
	switch (format) {
	case SPUDGPU_FORMAT_D16_UNORM:
	case SPUDGPU_FORMAT_D32_FLOAT:
	case SPUDGPU_FORMAT_D24_UNORM_S8_UINT:
	case SPUDGPU_FORMAT_D32_FLOAT_S8X24_UINT:
		return true;
	default:
		return false;
	}
}

static void set4(float *out, float x, float y, float z, float w) {
	out[0] = x;
	out[1] = y;
	out[2] = z;
	out[3] = w;
}

// Everything the batch made, in reverse, for its destroy and a failed
// create alike.
static void aprend_batch_free(aprend_batch batch) {
	aprend_material_destroy(batch->material);
	for (auto &mode : batch->pipelines)
		for (aprend_graphics_pipeline pipeline : mode)
			aprend_graphics_pipeline_destroy(pipeline);
	aprend_binding_layout_destroy(batch->layout);
	for (aprend_shader shader : batch->frags)
		aprend_shader_destroy(shader);
	aprend_shader_destroy(batch->vert);
	aprend_sampler_destroy(batch->sampler);
	aprend_texture_view_destroy(batch->placeholder_array);
	aprend_texture_view_destroy(batch->placeholder_2d);
	aprend_texture2d_destroy(batch->placeholder);
	free(batch->name_copy);
	batch->~aprend_batch_t();
	free(batch);
}

// The pipeline for a kind and a depth mode (0 over, 1 tested, 2 tested
// reversed). NULL if it can't be made.
static aprend_graphics_pipeline aprend_batch_pipeline(aprend_batch batch, uint32_t kind, uint32_t mode, APREND_ERROR *error) {
	APREND_GRAPHICS_PIPELINE_DESC desc{};
	desc.base                                  = apri_struct_base(APRI_STRUCT_TYPE_APREND_GRAPHICS_PIPELINE_DESC);
	desc._debug_name                           = batch->debug_name;
	desc._vertex_bindings[0]._layout.elements  = s_record_elements;
	desc._vertex_bindings[0]._layout.count     = 8;
	desc._vertex_bindings[0]._layout.stride    = (uint32_t)sizeof(aprend_batch_record);
	desc._vertex_bindings[0]._per_instance     = true;
	desc._vertex_binding_count                 = 1;
	desc._vertex_inputs_by_name                = true;
	desc._topology                             = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc._cull_mode                            = SPUDGPU_CULL_MODE_NONE;
	desc._front_face_ccw                       = true;
	desc._depth_test                           = mode != 0;
	desc._depth_write                          = false;
	desc._depth_compare_op                     = mode == 0 ? SPUDGPU_COMPARE_OP_NEVER : mode == 1 ? SPUDGPU_COMPARE_OP_LESS_OR_EQUAL : SPUDGPU_COMPARE_OP_GREATER_OR_EQUAL;
	desc._blends[0]                            = aprend_blend_premultiplied();
	desc.vertex_shader                         = batch->vert;
	desc.fragment_shader                       = batch->frags[kind];
	desc._vertex_entry_point                   = "main";
	desc._fragment_entry_point                 = "main";
	desc.color_attachment_formats[0]           = batch->color_format;
	desc.color_attachment_count                = 1;
	desc.depth_format                          = batch->depth_format;
	desc._binding_layouts[0]                   = batch->layout;
	desc._binding_layout_count                 = 1;
	// Only the glyphs' fragment shader reads them (the atlas's range);
	// Metal's validation refuses bytes set for a stage that doesn't.
	desc._push_constant_ranges[0].stage_flags  = kind == KIND_GLYPH ? SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_FRAGMENT : SPUDGPU_SHADER_STAGE_VERTEX;
	desc._push_constant_ranges[0].offset       = 0;
	desc._push_constant_ranges[0].size         = sizeof(aprend_batch_push);
	desc._push_constant_range_count            = 1;
	aprend_graphics_pipeline pipeline          = nullptr;
	(void)aprend_graphics_pipeline_create(batch->instance, &desc, &pipeline, error);
	return pipeline;
}

// Room for [count] more records in the frame: false, with the reason, if
// the batch isn't begun or would pass its limit.
static APRESULT aprend_batch_room(aprend_batch batch, uint64_t count, APREND_ERROR *error) {
	if (!batch->begun)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the batch isn't begun this frame (aprend_batch_begin)");
	if (count > batch->max_shapes - batch->count)
		return aprend_fail(error, APRESULT_LIMIT, "%llu more shapes pass the batch's max_shapes of %u (%u taken)", (unsigned long long)count, batch->max_shapes,
		                   batch->count);
	return APRESULT_OK;
}

// The next record, its run extended or begun. The room was checked; the
// runs array may still fail to grow.
static APRESULT aprend_batch_next(aprend_batch batch, uint32_t kind, aprend_batch_record **out_record, APREND_ERROR *error) {
	if (batch->runs.empty() || batch->runs[batch->runs.size() - 1].kind != kind) {
		aprend_batch_run run{kind, batch->count, 0};
		if (batch->runs.push(run) != EQ_SUCCESS)
			return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "out of memory for the batch's runs");
	}
	batch->runs[batch->runs.size() - 1].count++;
	aprend_batch_record *record = (aprend_batch_record *)batch->span.data + batch->count;
	memset(record, 0, sizeof(*record));
	batch->count++;
	*out_record = record;
	return APRESULT_OK;
}

static void put3(float *out, const float *in, float w) {
	set4(out, in[0], in[1], in[2], w);
}

// ---- Lines ----------------------------------------------------------------

// A point projected to the screen: its clip position and pixel.
struct projected {
	float clip[4];
	float px[2];
};

static projected aprend_batch_project(const aprend_batch_t &batch, const float *p) {
	projected out{};
	const float *m = batch.push.view_projection;
	if (batch.frame.space == APREND_BATCH_SPACE_WORLD) {
		for (int r = 0; r < 4; ++r)
			out.clip[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
		const float w = out.clip[3] != 0.0f ? out.clip[3] : 1e-30f;
		out.px[0]     = (out.clip[0] / w * 0.5f + 0.5f) * batch.frame.viewport_size[0];
		out.px[1]     = (out.clip[1] / w * 0.5f + 0.5f) * batch.frame.viewport_size[1];
	} else {
		set4(out.clip, p[0], p[1], 0.0f, 1.0f);
		out.px[0] = p[0];
		out.px[1] = p[1];
	}
	return out;
}

// A segment of a line after the near plane: its ends (world or screen
// points), and whether each touches the next or previous visible segment.
struct segment {
	float a[3];
	float b[3];
	float a_px[2];
	float b_px[2];
	bool joined_before; // continues the visible segment before it
};
APRI_PLAIN_DATA(segment);

static float distance2(const float *a, const float *b) {
	const float dx = a[0] - b[0], dy = a[1] - b[1];
	return sqrtf(dx * dx + dy * dy);
}

// The line's segments, cut at the near plane in world space (clip z from
// 0 to w is what SpudGPU draws): what is behind the eye goes, and the line
// is broken there. May fail to grow [out].
static APRESULT aprend_batch_segments(const aprend_batch_t &batch, const APREND_BATCH_LINE &line, eq::array<segment> &out, APREND_ERROR *error) {
	const uint32_t n     = line.point_count;
	const uint32_t edges = line.closed ? n : n - 1;
	bool last_visible    = false;
	for (uint32_t i = 0; i < edges; ++i) {
		const float *p0 = line.points + 3 * (size_t)i;
		const float *p1 = line.points + 3 * (size_t)((i + 1) % n);
		float a[3] = {p0[0], p0[1], p0[2]}, b[3] = {p1[0], p1[1], p1[2]};
		if (a[0] == b[0] && a[1] == b[1] && (batch.frame.space == APREND_BATCH_SPACE_SCREEN || a[2] == b[2]))
			continue; // two equal points are one
		bool joined = last_visible;
		if (batch.frame.space == APREND_BATCH_SPACE_WORLD) {
			const projected pa = aprend_batch_project(batch, a), pb = aprend_batch_project(batch, b);
			const float za = pa.clip[2], zb = pb.clip[2];
			if (za < 0.0f && zb < 0.0f) {
				last_visible = false;
				continue;
			}
			if (za < 0.0f || zb < 0.0f) {
				const float t = za / (za - zb); // where z crosses 0
				float cut[3];
				for (int k = 0; k < 3; ++k)
					cut[k] = a[k] + (b[k] - a[k]) * t;
				if (za < 0.0f) {
					memcpy(a, cut, sizeof(a));
					joined = false;
				} else {
					memcpy(b, cut, sizeof(b));
				}
			}
		}
		segment s{};
		memcpy(s.a, a, sizeof(a));
		memcpy(s.b, b, sizeof(b));
		const projected pa = aprend_batch_project(batch, a), pb = aprend_batch_project(batch, b);
		memcpy(s.a_px, pa.px, sizeof(s.a_px));
		memcpy(s.b_px, pb.px, sizeof(s.b_px));
		s.joined_before = joined;
		if (out.push(s) != EQ_SUCCESS)
			return aprend_fail(error, APRESULT_OUT_OF_MEMORY, "out of memory laying out the line");
		// A segment cut at its end doesn't reach the next.
		last_visible = batch.frame.space == APREND_BATCH_SPACE_SCREEN || aprend_batch_project(batch, p1).clip[2] >= 0.0f;
	}
	// A closed line whose first and last segments both survive meets
	// itself.
	if (line.closed && out.size() > 1 && last_visible && edges == out.size())
		out[0].joined_before = true;
	return APRESULT_OK;
}

// ---- Debug ------------------------------------------------------------------

static APRESULT aprend_batch_debug_polyline(aprend_batch batch, const float *points, uint32_t count, bool closed, const float color[4], float width,
                                            APREND_ERROR *error) {
	APREND_BATCH_LINE line{};
	line.base        = apri_struct_base(APRI_STRUCT_TYPE_APREND_BATCH_LINE);
	line.points      = points;
	line.point_count = count;
	line.closed      = closed;
	line.width       = width;
	line.unit        = APREND_BATCH_UNIT_PIXELS;
	line.cap         = APREND_BATCH_CAP_ROUND;
	line.join        = APREND_BATCH_JOIN_ROUND;
	line.miter_limit = 1.0f;
	memcpy(line.color, color, sizeof(line.color));
	return aprend_batch_add_line(batch, &line, error);
}

static APRESULT aprend_batch_debug_check(aprend_batch batch, const float *color, float width, APREND_ERROR *error) {
	if (!batch->begun)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "the batch isn't begun this frame (aprend_batch_begin)");
	if (batch->frame.space != APREND_BATCH_SPACE_WORLD)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "debug drawing is in world space; the frame is in screen space");
	if (!color)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "color is NULL");
	if (!isfinite(width) || width <= 0.0f)
		return aprend_fail(error, APRESULT_INVALID_ARGUMENT, "width is not above 0");
	return APRESULT_OK;
}

// ---- Drawing ------------------------------------------------------------------

bool aprend_batch_expand(aprend_command_list cmd_list, aprend_batch batch) {
	if (!batch->begun) {
		aprend_cmd_report(cmd_list, APRESULT_INVALID_ARGUMENT, "DRAW_BATCH with a batch not begun this frame");
		return false;
	}
	if (batch->count == 0)
		return true;
	const uint32_t mode = batch->frame.depth == APREND_BATCH_DEPTH_TESTED ? 1 : batch->frame.depth == APREND_BATCH_DEPTH_TESTED_REVERSED ? 2 : 0;
	APREND_COMMAND cmd{};
	cmd._type                                       = APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER;
	cmd._params._set_transient_vertex_buffer._span = batch->span;
	cmd._params._set_transient_vertex_buffer._slot = 0;
	aprend_command_list_send(cmd_list, cmd);
	cmd                                 = {};
	cmd._type                           = APREND_COMMAND_SET_MATERIAL;
	cmd._params._set_material._slot     = 0;
	cmd._params._set_material._material = batch->material;
	aprend_command_list_send(cmd_list, cmd);
	uint32_t kind = UINT32_MAX;
	for (const aprend_batch_run &run : batch->runs) {
		if (run.kind != kind) {
			kind                                = run.kind;
			cmd                                 = {};
			cmd._type                           = APREND_COMMAND_SET_SHADER_PIPELINE;
			cmd._params._set_shader_pipeline._pipeline = batch->pipelines[mode][kind];
			aprend_command_list_send(cmd_list, cmd);
			// Copied by aprend_command_list_send.
			cmd                                    = {};
			cmd._type                              = APREND_COMMAND_PUSH_CONSTANTS;
			cmd._params._push_constants._data      = &batch->push;
			cmd._params._push_constants._offset    = 0;
			cmd._params._push_constants._size      = sizeof(batch->push);
			aprend_command_list_send(cmd_list, cmd);
		}
		cmd                                                    = {};
		cmd._type                                              = APREND_COMMAND_DRAW_INSTANCED;
		cmd._params._draw_instanced._vertex_count_per_instance = 6;
		cmd._params._draw_instanced._instance_count            = run.count;
		cmd._params._draw_instanced._start_vertex_location     = 0;
		cmd._params._draw_instanced._start_instance_location   = run.first;
		aprend_command_list_send(cmd_list, cmd);
	}
	return true;
}

extern "C" {

APRESULT aprend_batch_create(aprend_instance instance, const APREND_BATCH_DESC *desc, aprend_batch *out_batch, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_batch)
		*out_batch = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	APREND_ENTER(instance, "aprend_batch_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_BATCH_DESC))
		return base;
	if (!out_batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_batch is NULL");
	if (aprend_format_scalar_type(desc->color_format) != APREND_SHADER_SCALAR_TYPE_FLOAT)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "color_format %u doesn't hold floats", (unsigned)desc->color_format);
	if (is_depth_format(desc->color_format))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "color_format %u is a depth format", (unsigned)desc->color_format);
	if (desc->depth_format != SPUDGPU_FORMAT_UNKNOWN && !is_depth_format(desc->depth_format))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "depth_format %u is not a depth format", (unsigned)desc->depth_format);
	if (desc->max_shapes == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "max_shapes is 0");

	aprend_batch_t *batch = (aprend_batch_t *)malloc(sizeof(aprend_batch_t));
	if (!batch)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory creating the batch");
	batch = new (batch) aprend_batch_t();
	batch->instance     = instance;
	batch->color_format = desc->color_format;
	batch->depth_format = desc->depth_format;
	batch->max_shapes   = desc->max_shapes;
	if (desc->debug_name) {
		const size_t size = strlen(desc->debug_name) + 1;
		batch->name_copy  = (char *)malloc(size);
		if (!batch->name_copy) {
			aprend_batch_free(batch);
			return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory creating the batch");
		}
		memcpy(batch->name_copy, desc->debug_name, size);
	}
	batch->debug_name = batch->name_copy ? batch->name_copy : s_name;

	struct shader_source {
		const uint32_t *code;
		size_t size;
	};
	const shader_source frags[APREND_BATCH_KINDS] = {{s_batch_quad_frag_spv, sizeof(s_batch_quad_frag_spv)},
	                                                 {s_batch_circle_frag_spv, sizeof(s_batch_circle_frag_spv)},
	                                                 {s_batch_line_frag_spv, sizeof(s_batch_line_frag_spv)},
	                                                 {s_batch_glyph_frag_spv, sizeof(s_batch_glyph_frag_spv)}};
	APRESULT result = aprend_shader_create_spirv(instance, s_batch_vert_spv, sizeof(s_batch_vert_spv), SPUDGPU_SHADER_STAGE_VERTEX, &batch->vert, out_error);
	for (uint32_t k = 0; k < APREND_BATCH_KINDS && result == APRESULT_OK; ++k)
		result = aprend_shader_create_spirv(instance, frags[k].code, frags[k].size, SPUDGPU_SHADER_STAGE_FRAGMENT, &batch->frags[k], out_error);
	if (result == APRESULT_OK)
		result = aprend_binding_layout_create_from_shaders(instance, batch->frags, APREND_BATCH_KINDS, 0, &batch->layout, out_error);
	const uint32_t modes = desc->depth_format == SPUDGPU_FORMAT_UNKNOWN ? 1 : APREND_BATCH_DEPTH_MODES;
	for (uint32_t mode = 0; mode < modes && result == APRESULT_OK; ++mode)
		for (uint32_t k = 0; k < APREND_BATCH_KINDS && result == APRESULT_OK; ++k)
			if (!(batch->pipelines[mode][k] = aprend_batch_pipeline(batch, k, mode, out_error)))
				result = APRESULT_APREND_GPU_FAILURE;
	if (result == APRESULT_OK) {
		APREND_SAMPLER_DESC sd{};
		sd.base           = apri_struct_base(APRI_STRUCT_TYPE_APREND_SAMPLER_DESC);
		sd.mag_filter     = SPUDGPU_FILTER_LINEAR;
		sd.min_filter     = SPUDGPU_FILTER_LINEAR;
		sd.mipmap_filter  = SPUDGPU_FILTER_LINEAR;
		sd.address_mode_u = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
		sd.address_mode_v = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
		sd.address_mode_w = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
		sd.max_lod        = 1000.0f;
		sd.max_anisotropy = 1.0f;
		sd.debug_name     = batch->debug_name;
		result            = aprend_sampler_create(instance, &sd, &batch->sampler, out_error);
	}
	if (result == APRESULT_OK) {
		APREND_TEXTURE2D_DESC td{};
		td.base         = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE2D_DESC);
		td.width        = 1;
		td.height       = 1;
		td.format       = SPUDGPU_FORMAT_R8G8B8A8_UNORM;
		td.usage        = APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE;
		td.mip_levels   = 1;
		td.array_layers = 1;
		td.sample_count = 1;
		result          = aprend_texture2d_create(instance, &td, &batch->placeholder, out_error);
	}
	for (int array = 0; array < 2 && result == APRESULT_OK; ++array) {
		APREND_TEXTURE_VIEW_DESC vd{};
		vd.base        = apri_struct_base(APRI_STRUCT_TYPE_APREND_TEXTURE_VIEW_DESC);
		vd.type        = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
		vd.shape       = array ? APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY : APREND_TEXTURE_VIEW_SHAPE_2D;
		vd.aspect      = APREND_TEXTURE_VIEW_ASPECT_COLOR;
		vd.mip_count   = 1;
		vd.layer_count = 1;
		result         = aprend_texture_view_create_2d(batch->placeholder, &vd, array ? &batch->placeholder_array : &batch->placeholder_2d, out_error);
	}
	if (result == APRESULT_OK) {
		APREND_MATERIAL_DESC md{};
		md.base    = apri_struct_base(APRI_STRUCT_TYPE_APREND_MATERIAL_DESC);
		md._layout = batch->layout;
		result     = aprend_material_create(instance, &md, &batch->material, out_error);
	}
	if (result == APRESULT_OK)
		result = aprend_material_set_texture(batch->material, "batch_texture", 0, batch->placeholder_2d, out_error);
	if (result == APRESULT_OK)
		result = aprend_material_set_texture(batch->material, "batch_atlas", 0, batch->placeholder_array, out_error);
	if (result == APRESULT_OK)
		result = aprend_material_set_sampler(batch->material, "batch_sampler", 0, batch->sampler, out_error);
	if (result != APRESULT_OK) {
		aprend_batch_free(batch);
		return result;
	}
	*out_batch = batch;
	return APRESULT_OK;
}

void aprend_batch_destroy(aprend_batch batch) {
	if (!batch)
		return;
	aprend_batch_free(batch);
}

APRESULT aprend_batch_begin(aprend_batch batch, const APREND_BATCH_FRAME *frame, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "batch is NULL");
	APREND_ENTER(batch->instance, "aprend_batch_begin");
	if (!frame)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "frame is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "frame", &frame->base, APRI_STRUCT_TYPE_APREND_BATCH_FRAME))
		return base;
	const APREND_BATCH_FRAME &f = *frame;
	if (f.space != APREND_BATCH_SPACE_SCREEN && f.space != APREND_BATCH_SPACE_WORLD)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "space %u is not a space", (unsigned)f.space);
	if (f.depth != APREND_BATCH_DEPTH_OVER && f.depth != APREND_BATCH_DEPTH_TESTED && f.depth != APREND_BATCH_DEPTH_TESTED_REVERSED)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "depth %u is not a depth mode", (unsigned)f.depth);
	if (f.depth != APREND_BATCH_DEPTH_OVER && f.space == APREND_BATCH_SPACE_SCREEN)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a screen-space frame is drawn over: its depth is APREND_BATCH_DEPTH_OVER");
	if (f.depth != APREND_BATCH_DEPTH_OVER && batch->depth_format == SPUDGPU_FORMAT_UNKNOWN)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the frame is depth-tested and the batch was made with no depth_format");
	if (f.space == APREND_BATCH_SPACE_WORLD && !finite_all(f.view_projection, 16))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "view_projection has a value that is not finite");
	if (!finite_all(f.viewport_size, 2) || f.viewport_size[0] <= 0.0f || f.viewport_size[1] <= 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "viewport_size is not above 0");
	if (!isfinite(f.pixels_per_mm) || f.pixels_per_mm < 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "pixels_per_mm is negative or not a number");
	if (f.texture && f.texture->instance != batch->instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "texture is of another instance");
	if (f.texture && (f.texture->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || f.texture->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D ||
	                  f.texture->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "texture is not a SHADER_RESOURCE 2D view of color");
	if (f.atlas && f.atlas->instance != batch->instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is of another instance");
	if (f.atlas && (f.atlas->view_type != APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE || f.atlas->desc.shape != APREND_TEXTURE_VIEW_SHAPE_2D_ARRAY ||
	                f.atlas->desc.aspect != APREND_TEXTURE_VIEW_ASPECT_COLOR))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas is not a SHADER_RESOURCE 2D_ARRAY view of color");
	if (f.atlas && (!isfinite(f.atlas_distance_range) || f.atlas_distance_range <= 0.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "atlas_distance_range is not above 0");

	// The material's views first: a failure there leaves the last frame's
	// shapes as they were.
	if (const APRESULT set = aprend_material_set_texture(batch->material, "batch_texture", 0, f.texture ? f.texture : batch->placeholder_2d, out_error))
		return set;
	if (const APRESULT set = aprend_material_set_texture(batch->material, "batch_atlas", 0, f.atlas ? f.atlas : batch->placeholder_array, out_error))
		return set;
	APREND_TRANSIENT_SPAN span{};
	if (const APRESULT taken = aprend_transient_allocate(batch->instance, (uint64_t)batch->max_shapes * sizeof(aprend_batch_record), 16, &span, out_error))
		return taken;
	batch->begun = true;
	batch->frame = f;
	batch->span  = span;
	batch->count = 0;
	batch->runs.clear();
	batch->atlas_layers = f.atlas ? f.atlas->desc.layer_count : 0;
	memcpy(batch->push.view_projection, f.view_projection, sizeof(batch->push.view_projection));
	set4(batch->push.viewport, f.viewport_size[0], f.viewport_size[1], 1.0f / f.viewport_size[0], 1.0f / f.viewport_size[1]);
	set4(batch->push.params, f.space == APREND_BATCH_SPACE_WORLD ? 2.0f : 1.0f, f.atlas_distance_range, 0.0f, 0.0f);
	return APRESULT_OK;
}

APRESULT aprend_batch_add_quads(aprend_batch batch, const APREND_BATCH_QUAD *quads, uint32_t count, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "batch is NULL");
	APREND_ENTER(batch->instance, "aprend_batch_add_quads");
	if (!quads && count != 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "quads is NULL with a count");
	if (const APRESULT room = aprend_batch_room(batch, count, out_error))
		return room;
	for (uint32_t i = 0; i < count; ++i) {
		const APREND_BATCH_QUAD &q = quads[i];
		if (!finite_all(q.origin, 3) || !finite_all(q.edge_x, 3) || !finite_all(q.edge_y, 3) || !finite_all(q.color, 4) || !finite_all(q.uv_rect, 4))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "quad %u has a value that is not finite", i);
		if (q.textured && !batch->frame.texture)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "quad %u is textured and the frame has no texture", i);
	}
	if (batch->runs.reserve(batch->runs.size() + 1) != EQ_SUCCESS)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory for the batch's runs");
	for (uint32_t i = 0; i < count; ++i) {
		const APREND_BATCH_QUAD &q = quads[i];
		aprend_batch_record *r     = nullptr;
		if (const APRESULT next = aprend_batch_next(batch, KIND_QUAD, &r, out_error))
			return next;
		put3(r->r[0], q.origin, 0.0f);
		put3(r->r[1], q.edge_x, 0.0f);
		put3(r->r[2], q.edge_y, 0.0f);
		r->r[3][3] = (float)(KIND_QUAD | (q.textured ? QUAD_TEXTURED : 0u));
		memcpy(r->r[4], q.color, sizeof(q.color));
		memcpy(r->r[5], q.uv_rect, sizeof(q.uv_rect));
	}
	return APRESULT_OK;
}

APRESULT aprend_batch_add_circles(aprend_batch batch, const APREND_BATCH_CIRCLE *circles, uint32_t count, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "batch is NULL");
	APREND_ENTER(batch->instance, "aprend_batch_add_circles");
	if (!circles && count != 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "circles is NULL with a count");
	if (const APRESULT room = aprend_batch_room(batch, count, out_error))
		return room;
	for (uint32_t i = 0; i < count; ++i) {
		const APREND_BATCH_CIRCLE &c = circles[i];
		if (!finite_all(c.center, 3) || !finite_all(c.radius_x, 3) || !finite_all(c.radius_y, 3) || !finite_all(c.color, 4) || !isfinite(c.inner) ||
		    !isfinite(c.start_angle) || !isfinite(c.sweep_angle))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "circle %u has a value that is not finite", i);
		if (c.inner < 0.0f || c.inner > 1.0f)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "circle %u's inner is not from 0 to 1", i);
	}
	if (batch->runs.reserve(batch->runs.size() + 1) != EQ_SUCCESS)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory for the batch's runs");
	for (uint32_t i = 0; i < count; ++i) {
		const APREND_BATCH_CIRCLE &c = circles[i];
		aprend_batch_record *r       = nullptr;
		if (const APRESULT next = aprend_batch_next(batch, KIND_CIRCLE, &r, out_error))
			return next;
		put3(r->r[0], c.center, 0.0f);
		put3(r->r[1], c.radius_x, 0.0f);
		put3(r->r[2], c.radius_y, 0.0f);
		r->r[3][3] = (float)KIND_CIRCLE;
		memcpy(r->r[4], c.color, sizeof(c.color));
		set4(r->r[5], c.inner, c.start_angle, c.sweep_angle, 0.0f);
	}
	return APRESULT_OK;
}

APRESULT aprend_batch_add_line(aprend_batch batch, const APREND_BATCH_LINE *line, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "batch is NULL");
	APREND_ENTER(batch->instance, "aprend_batch_add_line");
	if (!line)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "line is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "line", &line->base, APRI_STRUCT_TYPE_APREND_BATCH_LINE))
		return base;
	const APREND_BATCH_LINE &l = *line;
	if (!batch->begun)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the batch isn't begun this frame (aprend_batch_begin)");
	if (!l.points)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "points is NULL");
	if (l.point_count < 2)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "point_count %u is under 2", l.point_count);
	if (!finite_all(l.points, 3 * (size_t)l.point_count))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "a point has a value that is not finite");
	if (!isfinite(l.width) || l.width <= 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "width is not above 0");
	if (l.unit != APREND_BATCH_UNIT_PIXELS && l.unit != APREND_BATCH_UNIT_PAPER_MM)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "unit %u is not a unit", (unsigned)l.unit);
	if (l.unit == APREND_BATCH_UNIT_PAPER_MM && batch->frame.pixels_per_mm <= 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the line is in paper millimetres and the frame's pixels_per_mm is 0");
	if (l.cap != APREND_BATCH_CAP_BUTT && l.cap != APREND_BATCH_CAP_SQUARE && l.cap != APREND_BATCH_CAP_ROUND)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "cap %u is not a cap", (unsigned)l.cap);
	if (l.join != APREND_BATCH_JOIN_MITER && l.join != APREND_BATCH_JOIN_BEVEL && l.join != APREND_BATCH_JOIN_ROUND)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "join %u is not a join", (unsigned)l.join);
	if (l.join == APREND_BATCH_JOIN_MITER && (!isfinite(l.miter_limit) || l.miter_limit < 1.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "miter_limit is under 1");
	if (!finite_all(l.color, 4))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "color has a value that is not finite");
	if (l.dash_count % 2 != 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "dash_count %u is odd: dashes and gaps come in pairs", l.dash_count);
	if (l.dash_count > 8)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "dash_count %u is over 8", l.dash_count);
	if (l.dash_count > 0 && !l.dashes)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "dashes is NULL with a dash_count");
	float pattern = 0.0f;
	for (uint32_t i = 0; i < l.dash_count; ++i) {
		if (!isfinite(l.dashes[i]) || l.dashes[i] < 0.0f)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "dash %u is negative or not a number", i);
		pattern += l.dashes[i];
	}
	if (l.dash_count > 0 && pattern <= 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the dash pattern has no length");
	if (!isfinite(l.dash_offset))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "dash_offset is not a number");

	eq::array<segment> segments;
	if (const APRESULT laid = aprend_batch_segments(*batch, l, segments, out_error))
		return laid;
	if (const APRESULT room = aprend_batch_room(batch, segments.size(), out_error))
		return room;
	if (batch->runs.reserve(batch->runs.size() + 1) != EQ_SUCCESS)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory for the batch's runs");

	const float scale = l.unit == APREND_BATCH_UNIT_PAPER_MM ? batch->frame.pixels_per_mm : 1.0f;
	float dashes[8]   = {};
	for (uint32_t i = 0; i < l.dash_count; ++i)
		dashes[i] = l.dashes[i] * scale;
	// The phase runs on along the line as it is on the screen, from the
	// offset, kept within one pattern.
	const float pattern_px = pattern * scale;
	float phase            = pattern_px > 0.0f ? fmodf(l.dash_offset * scale, pattern_px) : 0.0f;
	if (phase < 0.0f)
		phase += pattern_px;
	const size_t n = segments.size();
	const uint32_t cap_join = ((uint32_t)l.cap << LINE_CAP_SHIFT) | ((uint32_t)l.join << LINE_JOIN_SHIFT);
	for (size_t i = 0; i < n; ++i) {
		const segment &s     = segments[i];
		const bool has_prev  = s.joined_before && (i > 0 || l.closed);
		const size_t next_i  = (i + 1) % n;
		const bool has_next  = (i + 1 < n || l.closed) && segments[next_i].joined_before && next_i != i;
		const segment &prev  = segments[(i + n - 1) % n];
		const segment &after = segments[next_i];
		aprend_batch_record *r = nullptr;
		if (const APRESULT next = aprend_batch_next(batch, KIND_LINE, &r, out_error))
			return next;
		put3(r->r[0], s.a, phase);
		put3(r->r[1], s.b, l.width * scale);
		put3(r->r[2], has_prev ? prev.a : s.a, l.join == APREND_BATCH_JOIN_MITER ? l.miter_limit : 1.0f);
		put3(r->r[3], has_next ? after.b : s.b, (float)(KIND_LINE | cap_join | (has_prev ? LINE_HAS_PREV : 0u) | (has_next ? LINE_HAS_NEXT : 0u)));
		memcpy(r->r[4], l.color, sizeof(l.color));
		memcpy(r->r[5], dashes, sizeof(float) * 4);
		memcpy(r->r[6], dashes + 4, sizeof(float) * 4);
		set4(r->r[7], 0.0f, 0.0f, pattern_px, 0.0f);
		if (pattern_px > 0.0f)
			phase = fmodf(phase + distance2(s.a_px, s.b_px), pattern_px);
	}
	return APRESULT_OK;
}

APRESULT aprend_batch_add_glyphs(aprend_batch batch, const APREND_BATCH_GLYPH *glyphs, uint32_t count, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "batch is NULL");
	APREND_ENTER(batch->instance, "aprend_batch_add_glyphs");
	if (!glyphs && count != 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "glyphs is NULL with a count");
	if (const APRESULT room = aprend_batch_room(batch, count, out_error))
		return room;
	if (count > 0 && !batch->frame.atlas)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the frame has no atlas");
	for (uint32_t i = 0; i < count; ++i) {
		const APREND_BATCH_GLYPH &g = glyphs[i];
		if (!finite_all(g.origin, 3) || !finite_all(g.edge_x, 3) || !finite_all(g.edge_y, 3) || !finite_all(g.color, 4) ||
		    !finite_all(g.outline_color, 4) || !isfinite(g.outline_width) || !finite_all(g.uv_rect, 4))
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "glyph %u has a value that is not finite", i);
		if (g.outline_width < 0.0f || g.outline_width > 1.0f)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "glyph %u's outline_width is not from 0 to 1", i);
		if (g.layer >= batch->atlas_layers)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "glyph %u's layer %u is past the atlas's %u", i, g.layer, batch->atlas_layers);
	}
	if (batch->runs.reserve(batch->runs.size() + 1) != EQ_SUCCESS)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory for the batch's runs");
	for (uint32_t i = 0; i < count; ++i) {
		const APREND_BATCH_GLYPH &g = glyphs[i];
		aprend_batch_record *r      = nullptr;
		if (const APRESULT next = aprend_batch_next(batch, KIND_GLYPH, &r, out_error))
			return next;
		put3(r->r[0], g.origin, 0.0f);
		put3(r->r[1], g.edge_x, 0.0f);
		put3(r->r[2], g.edge_y, 0.0f);
		r->r[3][3] = (float)KIND_GLYPH;
		memcpy(r->r[4], g.color, sizeof(g.color));
		memcpy(r->r[5], g.uv_rect, sizeof(g.uv_rect));
		memcpy(r->r[6], g.outline_color, sizeof(g.outline_color));
		set4(r->r[7], g.outline_width, (float)g.layer, 0.0f, 0.0f);
	}
	return APRESULT_OK;
}

uint32_t aprend_batch_get_shape_count(aprend_batch batch) {
	return batch ? batch->count : 0;
}

APRESULT aprend_batch_debug_box(aprend_batch batch, const float center[3], const float half_x[3], const float half_y[3], const float half_z[3],
                                const float color[4], float width, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "batch is NULL");
	APREND_ENTER(batch->instance, "aprend_batch_debug_box");
	if (!center)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "center is NULL");
	if (!half_x)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "half_x is NULL");
	if (!half_y)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "half_y is NULL");
	if (!half_z)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "half_z is NULL");
	if (const APRESULT ok = aprend_batch_debug_check(batch, color, width, out_error))
		return ok;
	// Corner k is centre + sx half_x + sy half_y + sz half_z, the signs from
	// k's bits.
	float corners[8][3];
	for (int k = 0; k < 8; ++k) {
		const float sx = (k & 1) ? 1.0f : -1.0f, sy = (k & 2) ? 1.0f : -1.0f, sz = (k & 4) ? 1.0f : -1.0f;
		for (int a = 0; a < 3; ++a)
			corners[k][a] = center[a] + sx * half_x[a] + sy * half_y[a] + sz * half_z[a];
	}
	// Two faces as closed loops, then the four edges between them: twelve
	// records, checked for room together.
	if (const APRESULT room = aprend_batch_room(batch, 12, out_error))
		return room;
	const int loops[2][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}};
	for (const auto &loop : loops) {
		float points[12];
		for (int i = 0; i < 4; ++i)
			memcpy(points + 3 * i, corners[loop[i]], sizeof(float) * 3);
		if (const APRESULT added = aprend_batch_debug_polyline(batch, points, 4, true, color, width, out_error))
			return added;
	}
	for (int k = 0; k < 4; ++k) {
		float points[6];
		memcpy(points, corners[k], sizeof(float) * 3);
		memcpy(points + 3, corners[k + 4], sizeof(float) * 3);
		if (const APRESULT added = aprend_batch_debug_polyline(batch, points, 2, false, color, width, out_error))
			return added;
	}
	return APRESULT_OK;
}

APRESULT aprend_batch_debug_sphere(aprend_batch batch, const float center[3], float radius, const float color[4], float width,
                                   APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "batch is NULL");
	APREND_ENTER(batch->instance, "aprend_batch_debug_sphere");
	if (!center)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "center is NULL");
	if (!isfinite(radius) || radius <= 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "radius is not above 0");
	if (const APRESULT ok = aprend_batch_debug_check(batch, color, width, out_error))
		return ok;
	// Three great circles of 48 segments, round x, y and z.
	enum { SEGMENTS = 48 };
	if (const APRESULT room = aprend_batch_room(batch, 3 * SEGMENTS, out_error))
		return room;
	for (int axis = 0; axis < 3; ++axis) {
		float points[3 * SEGMENTS];
		for (int i = 0; i < SEGMENTS; ++i) {
			const float a = 6.28318530718f * (float)i / (float)SEGMENTS;
			float local[3] = {0.0f, 0.0f, 0.0f};
			local[(axis + 1) % 3] = cosf(a) * radius;
			local[(axis + 2) % 3] = sinf(a) * radius;
			for (int k = 0; k < 3; ++k)
				points[3 * i + k] = center[k] + local[k];
		}
		if (const APRESULT added = aprend_batch_debug_polyline(batch, points, SEGMENTS, true, color, width, out_error))
			return added;
	}
	return APRESULT_OK;
}

APRESULT aprend_batch_debug_arrow(aprend_batch batch, const float from[3], const float to[3], float head, const float color[4], float width,
                                  APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "batch is NULL");
	APREND_ENTER(batch->instance, "aprend_batch_debug_arrow");
	if (!from)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "from is NULL");
	if (!to)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "to is NULL");
	if (!isfinite(head) || head <= 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "head is not above 0");
	if (const APRESULT ok = aprend_batch_debug_check(batch, color, width, out_error))
		return ok;
	float d[3]   = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
	const float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (!(length > 0.0f))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "from and to are the same point");
	for (float &v : d)
		v /= length;
	// A side at right angles to the shaft: across the axis it is least
	// along.
	const float ax[3] = {fabsf(d[0]) < 0.9f ? 1.0f : 0.0f, fabsf(d[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
	float side[3]     = {d[1] * ax[2] - d[2] * ax[1], d[2] * ax[0] - d[0] * ax[2], d[0] * ax[1] - d[1] * ax[0]};
	const float sl    = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
	for (float &v : side)
		v /= sl;
	if (const APRESULT room = aprend_batch_room(batch, 3, out_error))
		return room;
	float shaft[6] = {from[0], from[1], from[2], to[0], to[1], to[2]};
	if (const APRESULT added = aprend_batch_debug_polyline(batch, shaft, 2, false, color, width, out_error))
		return added;
	float barbs[9];
	for (int k = 0; k < 3; ++k) {
		barbs[k]     = to[k] - d[k] * head + side[k] * head * 0.5f;
		barbs[3 + k] = to[k];
		barbs[6 + k] = to[k] - d[k] * head - side[k] * head * 0.5f;
	}
	return aprend_batch_debug_polyline(batch, barbs, 3, false, color, width, out_error);
}

APRESULT aprend_batch_debug_axes(aprend_batch batch, const float origin[3], float length, float width, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!batch)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "batch is NULL");
	APREND_ENTER(batch->instance, "aprend_batch_debug_axes");
	if (!origin)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "origin is NULL");
	if (!isfinite(length) || length <= 0.0f)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "length is not above 0");
	const float colors[3][4] = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 1.0f}};
	if (const APRESULT ok = aprend_batch_debug_check(batch, colors[0], width, out_error))
		return ok;
	if (const APRESULT room = aprend_batch_room(batch, 3, out_error))
		return room;
	for (int axis = 0; axis < 3; ++axis) {
		float points[6] = {origin[0], origin[1], origin[2], origin[0], origin[1], origin[2]};
		points[3 + axis] += length;
		if (const APRESULT added = aprend_batch_debug_polyline(batch, points, 2, false, colors[axis], width, out_error))
			return added;
	}
	return APRESULT_OK;
}

} // extern "C"
