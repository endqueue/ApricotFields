
#include "aprend_internal.hpp"
#include "aprendbuiltin_internal.hpp"
#include "aprendimages_internal.hpp"
#include "aprendlit_internal.hpp"

#include <cstdio>

// The SPIR-V of shaders/, embedded by the build (CMakeLists.txt) as
// brace-enclosed uint32_t initializer lists.
static const uint32_t s_fullscreen_vert_spv[] =
#include "aprend_fullscreen.vert.spv.inc"
    ;
static const uint32_t s_downsample_frag_spv[] =
#include "aprend_downsample.frag.spv.inc"
    ;
static const uint32_t s_bloom_prefilter_frag_spv[] =
#include "aprend_bloom_prefilter.frag.spv.inc"
    ;
static const uint32_t s_bloom_downsample_frag_spv[] =
#include "aprend_bloom_downsample.frag.spv.inc"
    ;
static const uint32_t s_bloom_upsample_frag_spv[] =
#include "aprend_bloom_upsample.frag.spv.inc"
    ;
static const uint32_t s_bloom_composite_frag_spv[] =
#include "aprend_bloom_composite.frag.spv.inc"
    ;

extern "C" aprend_shader aprend_get_fullscreen_vertex_shader(aprend_instance instance) {
	if (!instance)
		return nullptr;
	return aprend_builtin_fullscreen_vert(instance);
}

aprend_shader aprend_builtin_fullscreen_vert(aprend_instance instance) {
	aprend_builtins &builtins = instance->builtins;
	if (!builtins.fullscreen_vert) {
		builtins.fullscreen_vert = aprend_shader_create_spirv(instance, s_fullscreen_vert_spv, sizeof(s_fullscreen_vert_spv), SPUDGPU_SHADER_STAGE_VERTEX);
		if (!builtins.fullscreen_vert)
			printf("apricot: Aprend's full-target vertex shader can't be created\n");
	}
	return builtins.fullscreen_vert;
}

// The shaders and layout of the downsample pipelines, made together the
// first time a pipeline is asked for. False, printed, if any can't be;
// what was made is kept for the next try.
static bool aprend_builtin_downsample_prepare(aprend_instance instance) {
	aprend_builtins &builtins = instance->builtins;
	if (!aprend_builtin_fullscreen_vert(instance))
		return false;
	if (!builtins.downsample_frag) {
		builtins.downsample_frag = aprend_shader_create_spirv(instance, s_downsample_frag_spv, sizeof(s_downsample_frag_spv), SPUDGPU_SHADER_STAGE_FRAGMENT);
		if (!builtins.downsample_frag) {
			printf("apricot: Aprend's downsample shader can't be created\n");
			return false;
		}
	}
	if (!builtins.downsample_layout) {
		builtins.downsample_layout = aprend_binding_layout_create_from_shaders(instance, &builtins.downsample_frag, 1, 0);
		if (!builtins.downsample_layout) {
			printf("apricot: the binding layout of Aprend's downsample shader can't be created\n");
			return false;
		}
	}
	return true;
}

bool aprend_builtin_downsample_format(SPUDGPU_FORMAT format) {
	// What Vulkan requires of every device as a color attachment and
	// linearly filtered, D3D12 of every device of feature level 12_0, and
	// Metal of every Apple GPU. A 32-bit float channel is not filterable on
	// every device, and a snorm one not renderable on every one, so both are
	// left out; an integer format can't be filtered at all.
	switch (format) {
	case SPUDGPU_FORMAT_R8_UNORM:
	case SPUDGPU_FORMAT_R8G8_UNORM:
	case SPUDGPU_FORMAT_R8G8B8A8_UNORM:
	case SPUDGPU_FORMAT_R8G8B8A8_UNORM_SRGB:
	case SPUDGPU_FORMAT_B8G8R8A8_UNORM:
	case SPUDGPU_FORMAT_B8G8R8A8_UNORM_SRGB:
	case SPUDGPU_FORMAT_R10G10B10A2_UNORM:
	case SPUDGPU_FORMAT_R11G11B10_FLOAT:
	case SPUDGPU_FORMAT_R16_FLOAT:
	case SPUDGPU_FORMAT_R16G16_FLOAT:
	case SPUDGPU_FORMAT_R16G16B16A16_FLOAT:
		return true;
	default:
		return false;
	}
}

aprend_graphics_pipeline aprend_builtin_downsample_pipeline(aprend_instance instance, SPUDGPU_FORMAT format) {
	aprend_builtins &builtins = instance->builtins;
	for (const aprend_builtins::downsample_pipeline &entry : builtins.downsample_pipelines)
		if (entry.format == format)
			return entry.pipeline;
	if (!aprend_builtin_downsample_prepare(instance))
		return nullptr;

	aprend_graphics_pipeline_desc desc{};
	desc._debug_name = "aprend downsample";
	desc._vertex_binding_count        = 0;
	desc._topology                    = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc._cull_mode                   = SPUDGPU_CULL_MODE_NONE;
	desc._front_face_ccw              = true;
	desc._depth_test                  = false;
	desc._depth_write                 = false;
	desc._depth_compare_op            = SPUDGPU_COMPARE_OP_NEVER;
	desc.vertex_shader                = builtins.fullscreen_vert;
	desc.fragment_shader              = builtins.downsample_frag;
	desc._vertex_entry_point          = "main";
	desc._fragment_entry_point        = "main";
	desc.color_attachment_formats[0]  = format;
	desc.color_attachment_count       = 1;
	desc.depth_format                 = SPUDGPU_FORMAT_UNKNOWN;
	desc._binding_layouts[0]          = builtins.downsample_layout;
	desc._binding_layout_count        = 1;
	desc._push_constant_range_count   = 0;

	aprend_graphics_pipeline pipeline = aprend_graphics_pipeline_create(instance, desc);
	if (!pipeline) {
		printf("apricot: Aprend's downsample pipeline can't be created for format %d\n", (int)format);
		return nullptr;
	}
	try {
		builtins.downsample_pipelines.push_back({format, pipeline});
	} catch (...) {
		aprend_graphics_pipeline_destroy(pipeline);
		printf("apricot: no memory to keep Aprend's downsample pipeline\n");
		return nullptr;
	}
	return pipeline;
}

aprend_binding_layout aprend_builtin_downsample_layout(aprend_instance instance) { return instance->builtins.downsample_layout; }

// The bloom shaders and their layout, made together the first time a bloom
// pipeline is asked for, as aprend_builtin_downsample_prepare.
bool aprend_builtin_bloom_prepare(aprend_instance instance) {
	aprend_builtins &builtins = instance->builtins;
	if (!aprend_builtin_fullscreen_vert(instance))
		return false;
	static const struct {
		const uint32_t *code;
		uint64_t size;
	} spirv[4] = {
	    {s_bloom_prefilter_frag_spv, sizeof(s_bloom_prefilter_frag_spv)},
	    {s_bloom_downsample_frag_spv, sizeof(s_bloom_downsample_frag_spv)},
	    {s_bloom_upsample_frag_spv, sizeof(s_bloom_upsample_frag_spv)},
	    {s_bloom_composite_frag_spv, sizeof(s_bloom_composite_frag_spv)},
	};
	for (uint32_t stage = 0; stage < 4; ++stage) {
		if (builtins.bloom_frags[stage])
			continue;
		builtins.bloom_frags[stage] = aprend_shader_create_spirv(instance, spirv[stage].code, spirv[stage].size, SPUDGPU_SHADER_STAGE_FRAGMENT);
		if (!builtins.bloom_frags[stage]) {
			printf("apricot: Aprend's bloom shader for stage %u can't be created\n", stage);
			return false;
		}
	}
	if (!builtins.bloom_layout) {
		builtins.bloom_layout = aprend_binding_layout_create_from_shaders(instance, builtins.bloom_frags, 4, 0);
		if (!builtins.bloom_layout) {
			printf("apricot: the binding layout of Aprend's bloom shaders can't be created\n");
			return false;
		}
	}
	return true;
}

aprend_graphics_pipeline aprend_builtin_bloom_pipeline(aprend_instance instance, uint32_t stage, SPUDGPU_FORMAT format) {
	aprend_builtins &builtins = instance->builtins;
	for (const aprend_builtins::bloom_pipeline &entry : builtins.bloom_pipelines)
		if (entry.stage == stage && entry.format == format)
			return entry.pipeline;
	if (!aprend_builtin_bloom_prepare(instance))
		return nullptr;

	aprend_graphics_pipeline_desc desc{};
	desc._debug_name = "aprend bloom";
	desc._vertex_binding_count       = 0;
	desc._topology                   = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc._cull_mode                  = SPUDGPU_CULL_MODE_NONE;
	desc._front_face_ccw             = true;
	desc._depth_test                 = false;
	desc._depth_write                = false;
	desc._depth_compare_op           = SPUDGPU_COMPARE_OP_NEVER;
	desc.vertex_shader               = builtins.fullscreen_vert;
	desc.fragment_shader             = builtins.bloom_frags[stage];
	desc._vertex_entry_point         = "main";
	desc._fragment_entry_point       = "main";
	desc.color_attachment_formats[0] = format;
	desc.color_attachment_count      = 1;
	desc.depth_format                = SPUDGPU_FORMAT_UNKNOWN;
	// The upsample adds into the level it renders, and the composite onto
	// the caller's target: color and alpha alike.
	if (stage == APREND_BLOOM_STAGE_UPSAMPLE || stage == APREND_BLOOM_STAGE_COMPOSITE)
		desc._blends[0] = aprend_blend_additive();
	desc._binding_layouts[0]                   = builtins.bloom_layout;
	desc._binding_layout_count                 = 1;
	desc._push_constant_ranges[0].stage_flags  = SPUDGPU_SHADER_STAGE_FRAGMENT;
	desc._push_constant_ranges[0].offset       = 0;
	desc._push_constant_ranges[0].size         = sizeof(aprend_bloom_params);
	desc._push_constant_range_count            = 1;

	aprend_graphics_pipeline pipeline = aprend_graphics_pipeline_create(instance, desc);
	if (!pipeline) {
		printf("apricot: Aprend's bloom pipeline for stage %u can't be created for format %d\n", stage, (int)format);
		return nullptr;
	}
	try {
		builtins.bloom_pipelines.push_back({stage, format, pipeline});
	} catch (...) {
		aprend_graphics_pipeline_destroy(pipeline);
		printf("apricot: no memory to keep Aprend's bloom pipeline\n");
		return nullptr;
	}
	return pipeline;
}

aprend_binding_layout aprend_builtin_bloom_layout(aprend_instance instance) { return instance->builtins.bloom_layout; }

aprend_sampler aprend_builtin_downsample_sampler(aprend_instance instance, SPUDGPU_FILTER filter) {
	if (filter != SPUDGPU_FILTER_NEAREST && filter != SPUDGPU_FILTER_LINEAR) {
		printf("apricot: %d is not a SPUDGPU_FILTER\n", (int)filter);
		return nullptr;
	}
	aprend_sampler &sampler = instance->builtins.downsample_samplers[filter];
	if (sampler)
		return sampler;

	aprend_sampler_desc desc{};
	desc.mag_filter     = filter;
	desc.min_filter     = filter;
	desc.mipmap_filter  = SPUDGPU_FILTER_NEAREST;
	desc.address_mode_u = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.address_mode_v = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.address_mode_w = SPUDGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.mip_lod_bias   = 0.0f;
	// The view it reads through has one level.
	desc.min_lod        = 0.0f;
	desc.max_lod        = 0.0f;
	desc.max_anisotropy = 1.0f;
	desc.debug_name = "aprend downsample";
	sampler = aprend_sampler_create(instance, &desc);
	if (!sampler)
		printf("apricot: Aprend's downsample sampler can't be created\n");
	return sampler;
}

aprend_binding_set aprend_texture_view_bloom_set(aprend_texture_view view) {
	if (view->bloom_set)
		return view->bloom_set;
	aprend_sampler sampler = aprend_builtin_downsample_sampler(view->instance, SPUDGPU_FILTER_LINEAR);
	if (!sampler)
		return nullptr;
	aprend_binding_set_entry entries[2]{};
	entries[0]._binding                  = 0;
	entries[0]._type                     = SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	entries[0]._resource._image._view    = view;
	entries[1]._binding                  = 1;
	entries[1]._type                     = SPUDGPU_DESCRIPTOR_TYPE_SAMPLER;
	entries[1]._resource._image._sampler = sampler;
	view->bloom_set                      = aprend_binding_set_create(aprend_builtin_bloom_layout(view->instance), entries, 2);
	if (!view->bloom_set)
		printf("apricot: bloom: the binding set that reads a view can't be created\n");
	return view->bloom_set;
}

void aprend_record_fullscreen_pass_sets(
    spudgpu_command_list cmd,
    aprend_graphics_pipeline pipeline,
    spudgpu_image_view target,
    uint32_t width,
    uint32_t height,
    SPUDGPU_LOAD_OP load_op,
    const aprend_binding_set *sets,
    uint32_t set_count,
    uint32_t frame,
    const void *push,
    uint32_t push_size) {
	spudgpu_rendering_begin_desc pass{};
	pass.base.type = SPUD_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	pass.color_attachments[0].image_view = target;
	pass.color_attachments[0].load_op    = load_op;
	pass.color_attachments[0].store_op   = SPUDGPU_STORE_OP_STORE;
	pass.color_attachment_count          = 1;
	pass.width                           = width;
	pass.height                          = height;
	spudgpu_cmd_begin_rendering(cmd, &pass);
	SPUDGPU_VIEWPORT viewport{};
	viewport.width    = (float)width;
	viewport.height   = (float)height;
	viewport.maxDepth = 1.0f;
	spudgpu_cmd_set_viewports(cmd, 0, 1, &viewport);
	SPUDGPU_SCISSOR_RECT scissor{};
	scissor.width  = (float)width;
	scissor.height = (float)height;
	spudgpu_cmd_set_scissor_rects(cmd, 0, 1, &scissor);
	spudgpu_cmd_bind_pipeline(cmd, pipeline->pipeline);
	for (uint32_t i = 0; i < set_count; ++i)
		if (sets[i])
			spudgpu_cmd_bind_descriptor_sets(cmd, pipeline->pipeline, i, &sets[i]->sets[aprend_binding_set_frame_slot(sets[i], frame)], 1);
	if (push)
		spudgpu_cmd_push_constants(cmd, pipeline->pipeline, 0, push_size, push);
	spudgpu_cmd_draw(cmd, 3, 0);
	spudgpu_cmd_end_rendering(cmd);
}

void aprend_record_fullscreen_pass(
    spudgpu_command_list cmd,
    aprend_graphics_pipeline pipeline,
    spudgpu_image_view target,
    uint32_t width,
    uint32_t height,
    SPUDGPU_LOAD_OP load_op,
    aprend_binding_set set,
    const void *push,
    uint32_t push_size) {
	aprend_record_fullscreen_pass_sets(cmd, pipeline, target, width, height, load_op, &set, 1, 0, push, push_size);
}

void aprend_builtins_destroy(aprend_instance instance) {
	aprend_builtins &builtins = instance->builtins;
	// Pipelines go before the layout they declare, and the release queue
	// keeps that order.
	for (const aprend_builtins::downsample_pipeline &entry : builtins.downsample_pipelines)
		aprend_graphics_pipeline_destroy(entry.pipeline);
	builtins.downsample_pipelines.clear();
	for (const aprend_builtins::bloom_pipeline &entry : builtins.bloom_pipelines)
		aprend_graphics_pipeline_destroy(entry.pipeline);
	builtins.bloom_pipelines.clear();
	aprend_binding_layout_destroy(builtins.bloom_layout);
	builtins.bloom_layout = nullptr;
	for (aprend_shader &shader : builtins.bloom_frags) {
		aprend_shader_destroy(shader);
		shader = nullptr;
	}
	aprend_binding_layout_destroy(builtins.downsample_layout);
	builtins.downsample_layout = nullptr;
	for (aprend_sampler &sampler : builtins.downsample_samplers) {
		aprend_sampler_destroy(sampler);
		sampler = nullptr;
	}
	aprend_shader_destroy(builtins.downsample_frag);
	builtins.downsample_frag = nullptr;
	aprend_shader_destroy(builtins.fullscreen_vert);
	builtins.fullscreen_vert = nullptr;
	aprend_lit_builtins_destroy(instance);
}
