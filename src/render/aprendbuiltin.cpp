
#include "aprend_internal.hpp"
#include "aprendbuiltin_internal.hpp"

#include <cstdio>

// The SPIR-V of shaders/, embedded by the build (CMakeLists.txt) as
// brace-enclosed uint32_t initializer lists.
static const uint32_t s_fullscreen_vert_spv[] =
#include "aprend_fullscreen.vert.spv.inc"
    ;
static const uint32_t s_downsample_frag_spv[] =
#include "aprend_downsample.frag.spv.inc"
    ;

// The shaders and layout of the downsample pipelines, made together the
// first time a pipeline is asked for. False, printed, if any can't be;
// what was made is kept for the next try.
static bool aprend_builtin_downsample_prepare(aprend_instance instance) {
	aprend_builtins &builtins = instance->builtins;
	if (!builtins.fullscreen_vert) {
		builtins.fullscreen_vert = aprend_shader_create_spirv(instance, s_fullscreen_vert_spv, sizeof(s_fullscreen_vert_spv), SPUDGPU_SHADER_STAGE_VERTEX);
		if (!builtins.fullscreen_vert) {
			printf("apricot: Aprend's full-target vertex shader can't be created\n");
			return false;
		}
	}
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
#ifdef _DEBUG
	desc._debug_name = "aprend downsample";
#endif
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
#if _DEBUG
	desc.debug_name = "aprend downsample";
#endif
	sampler = aprend_sampler_create(instance, &desc);
	if (!sampler)
		printf("apricot: Aprend's downsample sampler can't be created\n");
	return sampler;
}

void aprend_builtins_destroy(aprend_instance instance) {
	aprend_builtins &builtins = instance->builtins;
	// Pipelines go before the layout they declare, and the release queue
	// keeps that order.
	for (const aprend_builtins::downsample_pipeline &entry : builtins.downsample_pipelines)
		aprend_graphics_pipeline_destroy(entry.pipeline);
	builtins.downsample_pipelines.clear();
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
}
