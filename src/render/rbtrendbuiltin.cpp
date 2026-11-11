
#include "rbtrend_internal.hpp"
#include "rbtrendbuiltin_internal.hpp"
#include "rbtrendimages_internal.hpp"
#include "rbtrendlit_internal.hpp"

#include <cstdio>

// The packages of shaders/, embedded by the build (cmake/rbtrend_slang.cmake):
// each <stem>_package, its entry point named <stem>.
#include "rbtrend_fullscreen_vert.package.inc"
#include "rbtrend_downsample_frag.package.inc"
#include "rbtrend_present_frag.package.inc"
#include "rbtrend_bloom_prefilter_frag.package.inc"
#include "rbtrend_bloom_downsample_frag.package.inc"
#include "rbtrend_bloom_upsample_frag.package.inc"
#include "rbtrend_bloom_composite_frag.package.inc"
#include "rbtrend_blur_frag.package.inc"
#include "rbtrend_outline_frag.package.inc"

// The bloom stages' shaders, by RBTREND_BLOOM_STAGE.
static const RBTREND_SHADER_PACKAGE *const s_bloom_packages[4] = {
    &rbtrend_bloom_prefilter_frag_package,
    &rbtrend_bloom_downsample_frag_package,
    &rbtrend_bloom_upsample_frag_package,
    &rbtrend_bloom_composite_frag_package,
};

extern "C" RBT_RESULT rbtrend_instance_get_fullscreen_vertex_shader(rbtrend_instance instance, rbtrend_shader *out_shader, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_shader)
		*out_shader = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_instance_get_fullscreen_vertex_shader", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	if (!out_shader)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_shader is NULL");
	rbtrend_builtins &builtins = instance->builtins;
	if (!builtins.fullscreen_vert)
		if (const RBT_RESULT result = rbtrend_shader_create(instance, &rbtrend_fullscreen_vert_package, &builtins.fullscreen_vert, out_error))
			return result;
	*out_shader = builtins.fullscreen_vert;
	return RBT_RESULT_OK;
}

rbtrend_shader rbtrend_builtin_fullscreen_vert(rbtrend_instance instance) {
	rbtrend_builtins &builtins = instance->builtins;
	if (!builtins.fullscreen_vert) {
		(void)rbtrend_shader_create(instance, &rbtrend_fullscreen_vert_package, &builtins.fullscreen_vert, nullptr);
		if (!builtins.fullscreen_vert)
			rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's full-target vertex shader can't be created");
	}
	return builtins.fullscreen_vert;
}

// The shaders and layout of the downsample pipelines, made together the
// first time a pipeline is asked for. False, reported, if any can't be;
// what was made is kept for the next try.
static bool rbtrend_builtin_downsample_prepare(rbtrend_instance instance) {
	rbtrend_builtins &builtins = instance->builtins;
	if (!rbtrend_builtin_fullscreen_vert(instance))
		return false;
	if (!builtins.downsample_frag) {
		(void)rbtrend_shader_create(instance, &rbtrend_downsample_frag_package, &builtins.downsample_frag, nullptr);
		if (!builtins.downsample_frag) {
			rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's downsample shader can't be created");
			return false;
		}
	}
	if (!builtins.downsample_layout) {
		(void)rbtrend_binding_layout_create_from_shaders(instance, &builtins.downsample_frag, 1, 0, &builtins.downsample_layout, nullptr);
		if (!builtins.downsample_layout) {
			rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "the binding layout of Rend's downsample shader can't be created");
			return false;
		}
	}
	return true;
}

bool rbtrend_builtin_downsample_format(CRGPU_FORMAT format) {
	// What Vulkan requires of every device as a color attachment and
	// linearly filtered, D3D12 of every device of feature level 12_0, and
	// Metal of every Apple GPU. A 32-bit float channel is not filterable on
	// every device, and a snorm one not renderable on every one, so both are
	// left out; an integer format can't be filtered at all.
	switch (format) {
	case CRGPU_FORMAT_R8_UNORM:
	case CRGPU_FORMAT_R8G8_UNORM:
	case CRGPU_FORMAT_R8G8B8A8_UNORM:
	case CRGPU_FORMAT_R8G8B8A8_UNORM_SRGB:
	case CRGPU_FORMAT_B8G8R8A8_UNORM:
	case CRGPU_FORMAT_B8G8R8A8_UNORM_SRGB:
	case CRGPU_FORMAT_R10G10B10A2_UNORM:
	case CRGPU_FORMAT_R11G11B10_FLOAT:
	case CRGPU_FORMAT_R16_FLOAT:
	case CRGPU_FORMAT_R16G16_FLOAT:
	case CRGPU_FORMAT_R16G16B16A16_FLOAT:
		return true;
	default:
		return false;
	}
}

rbtrend_graphics_pipeline rbtrend_builtin_downsample_pipeline(rbtrend_instance instance, CRGPU_FORMAT format) {
	rbtrend_builtins &builtins = instance->builtins;
	for (const rbtrend_builtins::downsample_pipeline &entry : builtins.downsample_pipelines)
		if (entry.format == format)
			return entry.pipeline;
	if (!rbtrend_builtin_downsample_prepare(instance))
		return nullptr;

	RBTREND_GRAPHICS_PIPELINE_DESC desc{};
	desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_GRAPHICS_PIPELINE_DESC);
	desc._debug_name = "rbtrend downsample";
	desc._vertex_binding_count        = 0;
	desc._topology                    = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc._cull_mode                   = CRGPU_CULL_MODE_NONE;
	desc._front_face_ccw              = true;
	desc._depth_test                  = false;
	desc._depth_write                 = false;
	desc._depth_compare_op            = CRGPU_COMPARE_OP_NEVER;
	desc.vertex_shader                = builtins.fullscreen_vert;
	desc.fragment_shader              = builtins.downsample_frag;
	desc._vertex_entry_point          = "rbtrend_fullscreen_vert";
	desc._fragment_entry_point        = "rbtrend_downsample_frag";
	desc.color_attachment_formats[0]  = format;
	desc.color_attachment_count       = 1;
	desc.depth_format                 = CRGPU_FORMAT_UNKNOWN;
	desc._binding_layouts[0]          = builtins.downsample_layout;
	desc._binding_layout_count        = 1;
	desc._push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_FRAGMENT;
	desc._push_constant_ranges[0].offset      = 0;
	desc._push_constant_ranges[0].size        = sizeof(rbtrend_downsample_params);
	desc._push_constant_range_count           = 1;

	rbtrend_graphics_pipeline pipeline = nullptr;
	(void)rbtrend_graphics_pipeline_create(instance, &desc, &pipeline, nullptr);
	if (!pipeline) {
		rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's downsample pipeline can't be created for format %d", (int)format);
		return nullptr;
	}
	try {
		builtins.downsample_pipelines.push_back({format, pipeline});
	} catch (...) {
		rbtrend_graphics_pipeline_destroy(pipeline);
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "no memory to keep Rend's downsample pipeline");
		return nullptr;
	}
	return pipeline;
}

rbtrend_binding_layout rbtrend_builtin_downsample_layout(rbtrend_instance instance) { return instance->builtins.downsample_layout; }

// The bloom shaders and their layout, made together the first time a bloom
// pipeline is asked for, as rbtrend_builtin_downsample_prepare.
bool rbtrend_builtin_bloom_prepare(rbtrend_instance instance) {
	rbtrend_builtins &builtins = instance->builtins;
	if (!rbtrend_builtin_fullscreen_vert(instance))
		return false;
	for (uint32_t stage = 0; stage < 4; ++stage) {
		if (builtins.bloom_frags[stage])
			continue;
		(void)rbtrend_shader_create(instance, s_bloom_packages[stage], &builtins.bloom_frags[stage], nullptr);
		if (!builtins.bloom_frags[stage]) {
			rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's bloom shader for stage %u can't be created", stage);
			return false;
		}
	}
	if (!builtins.bloom_layout) {
		(void)rbtrend_binding_layout_create_from_shaders(instance, builtins.bloom_frags, 4, 0, &builtins.bloom_layout, nullptr);
		if (!builtins.bloom_layout) {
			rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "the binding layout of Rend's bloom shaders can't be created");
			return false;
		}
	}
	return true;
}

rbtrend_graphics_pipeline rbtrend_builtin_bloom_pipeline(rbtrend_instance instance, uint32_t stage, CRGPU_FORMAT format) {
	rbtrend_builtins &builtins = instance->builtins;
	for (const rbtrend_builtins::bloom_pipeline &entry : builtins.bloom_pipelines)
		if (entry.stage == stage && entry.format == format)
			return entry.pipeline;
	if (!rbtrend_builtin_bloom_prepare(instance))
		return nullptr;

	RBTREND_GRAPHICS_PIPELINE_DESC desc{};
	desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_GRAPHICS_PIPELINE_DESC);
	desc._debug_name = "rbtrend bloom";
	desc._vertex_binding_count       = 0;
	desc._topology                   = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc._cull_mode                  = CRGPU_CULL_MODE_NONE;
	desc._front_face_ccw             = true;
	desc._depth_test                 = false;
	desc._depth_write                = false;
	desc._depth_compare_op           = CRGPU_COMPARE_OP_NEVER;
	desc.vertex_shader               = builtins.fullscreen_vert;
	desc.fragment_shader             = builtins.bloom_frags[stage];
	desc._vertex_entry_point         = "rbtrend_fullscreen_vert";
	desc._fragment_entry_point       = s_bloom_packages[stage]->entry_point;
	desc.color_attachment_formats[0] = format;
	desc.color_attachment_count      = 1;
	desc.depth_format                = CRGPU_FORMAT_UNKNOWN;
	// The upsample adds into the level it renders, and the composite onto
	// the caller's target: color and alpha alike.
	if (stage == RBTREND_BLOOM_STAGE_UPSAMPLE || stage == RBTREND_BLOOM_STAGE_COMPOSITE)
		desc._blends[0] = rbtrend_get_blend_additive();
	desc._binding_layouts[0]                   = builtins.bloom_layout;
	desc._binding_layout_count                 = 1;
	// The prefilter's mask and the composite's lens dirt, each a texture
	// and a sampler as set 0 is.
	if (stage == RBTREND_BLOOM_STAGE_PREFILTER || stage == RBTREND_BLOOM_STAGE_COMPOSITE) {
		desc._binding_layouts[1]   = builtins.bloom_layout;
		desc._binding_layout_count = 2;
	}
	desc._push_constant_ranges[0].stage_flags  = CRGPU_SHADER_STAGE_FRAGMENT;
	desc._push_constant_ranges[0].offset       = 0;
	desc._push_constant_ranges[0].size         = sizeof(rbtrend_bloom_params);
	desc._push_constant_range_count            = 1;

	rbtrend_graphics_pipeline pipeline = nullptr;
	(void)rbtrend_graphics_pipeline_create(instance, &desc, &pipeline, nullptr);
	if (!pipeline) {
		rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's bloom pipeline for stage %u can't be created for format %d", stage, (int)format);
		return nullptr;
	}
	try {
		builtins.bloom_pipelines.push_back({stage, format, pipeline});
	} catch (...) {
		rbtrend_graphics_pipeline_destroy(pipeline);
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "no memory to keep Rend's bloom pipeline");
		return nullptr;
	}
	return pipeline;
}

rbtrend_binding_layout rbtrend_builtin_bloom_layout(rbtrend_instance instance) { return instance->builtins.bloom_layout; }

rbtrend_graphics_pipeline rbtrend_builtin_blur_pipeline(rbtrend_instance instance, CRGPU_FORMAT format) {
	rbtrend_builtins &builtins = instance->builtins;
	for (const rbtrend_builtins::blur_pipeline &entry : builtins.blur_pipelines)
		if (entry.format == format)
			return entry.pipeline;
	// Its set is the bloom layout's shape, so a view's bloom set serves it.
	if (!rbtrend_builtin_bloom_prepare(instance))
		return nullptr;
	if (!builtins.blur_frag) {
		(void)rbtrend_shader_create(instance, &rbtrend_blur_frag_package, &builtins.blur_frag, nullptr);
		if (!builtins.blur_frag) {
			rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's blur shader can't be created");
			return nullptr;
		}
	}

	RBTREND_GRAPHICS_PIPELINE_DESC desc{};
	desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_GRAPHICS_PIPELINE_DESC);
	desc._debug_name = "rbtrend blur";
	desc._vertex_binding_count                = 0;
	desc._topology                            = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc._cull_mode                           = CRGPU_CULL_MODE_NONE;
	desc._front_face_ccw                      = true;
	desc._depth_test                          = false;
	desc._depth_write                         = false;
	desc._depth_compare_op                    = CRGPU_COMPARE_OP_NEVER;
	desc.vertex_shader                        = builtins.fullscreen_vert;
	desc.fragment_shader                      = builtins.blur_frag;
	desc._vertex_entry_point                  = "rbtrend_fullscreen_vert";
	desc._fragment_entry_point                = "rbtrend_blur_frag";
	desc.color_attachment_formats[0]          = format;
	desc.color_attachment_count               = 1;
	desc.depth_format                         = CRGPU_FORMAT_UNKNOWN;
	desc._binding_layouts[0]                  = builtins.bloom_layout;
	desc._binding_layout_count                = 1;
	desc._push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_FRAGMENT;
	desc._push_constant_ranges[0].offset      = 0;
	desc._push_constant_ranges[0].size        = sizeof(rbtrend_blur_params);
	desc._push_constant_range_count           = 1;

	rbtrend_graphics_pipeline pipeline = nullptr;
	(void)rbtrend_graphics_pipeline_create(instance, &desc, &pipeline, nullptr);
	if (!pipeline) {
		rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's blur pipeline can't be created for format %d", (int)format);
		return nullptr;
	}
	try {
		builtins.blur_pipelines.push_back({format, pipeline});
	} catch (...) {
		rbtrend_graphics_pipeline_destroy(pipeline);
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "no memory to keep Rend's blur pipeline");
		return nullptr;
	}
	return pipeline;
}

rbtrend_graphics_pipeline rbtrend_builtin_outline_pipeline(rbtrend_instance instance, CRGPU_FORMAT format, bool blend) {
	rbtrend_builtins &builtins = instance->builtins;
	for (const rbtrend_builtins::outline_pipeline &entry : builtins.outline_pipelines)
		if (entry.format == format && entry.blend == blend)
			return entry.pipeline;
	// Its set is the bloom layout's shape, so a view's bloom set serves it.
	if (!rbtrend_builtin_bloom_prepare(instance))
		return nullptr;
	if (!builtins.outline_frag) {
		(void)rbtrend_shader_create(instance, &rbtrend_outline_frag_package, &builtins.outline_frag, nullptr);
		if (!builtins.outline_frag) {
			rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's outline shader can't be created");
			return nullptr;
		}
	}

	RBTREND_GRAPHICS_PIPELINE_DESC desc{};
	desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_GRAPHICS_PIPELINE_DESC);
	desc._debug_name = "rbtrend outline";
	desc._vertex_binding_count                = 0;
	desc._topology                            = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc._cull_mode                           = CRGPU_CULL_MODE_NONE;
	desc._front_face_ccw                      = true;
	desc._depth_test                          = false;
	desc._depth_write                         = false;
	desc._depth_compare_op                    = CRGPU_COMPARE_OP_NEVER;
	desc.vertex_shader                        = builtins.fullscreen_vert;
	desc.fragment_shader                      = builtins.outline_frag;
	desc._vertex_entry_point                  = "rbtrend_fullscreen_vert";
	desc._fragment_entry_point                = "rbtrend_outline_frag";
	desc.color_attachment_formats[0]          = format;
	desc.color_attachment_count               = 1;
	desc.depth_format                         = CRGPU_FORMAT_UNKNOWN;
	// The composite lays its premultiplied outline over the target.
	if (blend)
		desc._blends[0] = rbtrend_get_blend_premultiplied();
	desc._binding_layouts[0]                  = builtins.bloom_layout;
	desc._binding_layout_count                = 1;
	desc._push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_FRAGMENT;
	desc._push_constant_ranges[0].offset      = 0;
	desc._push_constant_ranges[0].size        = sizeof(rbtrend_outline_params);
	desc._push_constant_range_count           = 1;

	rbtrend_graphics_pipeline pipeline = nullptr;
	(void)rbtrend_graphics_pipeline_create(instance, &desc, &pipeline, nullptr);
	if (!pipeline) {
		rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's outline pipeline can't be created for format %d", (int)format);
		return nullptr;
	}
	try {
		builtins.outline_pipelines.push_back({format, blend, pipeline});
	} catch (...) {
		rbtrend_graphics_pipeline_destroy(pipeline);
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "no memory to keep Rend's outline pipeline");
		return nullptr;
	}
	return pipeline;
}

rbtrend_graphics_pipeline rbtrend_builtin_present_pipeline(rbtrend_instance instance, CRGPU_FORMAT format) {
	rbtrend_builtins &builtins = instance->builtins;
	for (const rbtrend_builtins::present_pipeline &entry : builtins.present_pipelines)
		if (entry.format == format)
			return entry.pipeline;
	if (!rbtrend_builtin_fullscreen_vert(instance))
		return nullptr;
	if (!builtins.present_frag) {
		(void)rbtrend_shader_create(instance, &rbtrend_present_frag_package, &builtins.present_frag, nullptr);
		if (!builtins.present_frag) {
			rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's present shader can't be created");
			return nullptr;
		}
	}
	if (!builtins.present_layout) {
		(void)rbtrend_binding_layout_create_from_shaders(instance, &builtins.present_frag, 1, 0, &builtins.present_layout, nullptr);
		if (!builtins.present_layout) {
			rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "the binding layout of Rend's present shader can't be created");
			return nullptr;
		}
	}

	RBTREND_GRAPHICS_PIPELINE_DESC desc{};
	desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_GRAPHICS_PIPELINE_DESC);
	desc._debug_name = "rbtrend present";
	desc._vertex_binding_count               = 0;
	desc._topology                           = CRGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc._cull_mode                          = CRGPU_CULL_MODE_NONE;
	desc._front_face_ccw                     = true;
	desc._depth_test                         = false;
	desc._depth_write                        = false;
	desc._depth_compare_op                   = CRGPU_COMPARE_OP_NEVER;
	desc.vertex_shader                       = builtins.fullscreen_vert;
	desc.fragment_shader                     = builtins.present_frag;
	desc._vertex_entry_point                 = "rbtrend_fullscreen_vert";
	desc._fragment_entry_point               = "rbtrend_present_frag";
	desc.color_attachment_formats[0]         = format;
	desc.color_attachment_count              = 1;
	desc.depth_format                        = CRGPU_FORMAT_UNKNOWN;
	desc._binding_layouts[0]                 = builtins.present_layout;
	desc._binding_layout_count               = 1;
	desc._push_constant_ranges[0].stage_flags = CRGPU_SHADER_STAGE_FRAGMENT;
	desc._push_constant_ranges[0].offset      = 0;
	desc._push_constant_ranges[0].size        = sizeof(rbtrend_present_params);
	desc._push_constant_range_count           = 1;

	rbtrend_graphics_pipeline pipeline = nullptr;
	(void)rbtrend_graphics_pipeline_create(instance, &desc, &pipeline, nullptr);
	if (!pipeline) {
		rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's present pipeline can't be created for format %d", (int)format);
		return nullptr;
	}
	try {
		builtins.present_pipelines.push_back({format, pipeline});
	} catch (...) {
		rbtrend_graphics_pipeline_destroy(pipeline);
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "no memory to keep Rend's present pipeline");
		return nullptr;
	}
	return pipeline;
}

rbtrend_binding_set rbtrend_texture_view_present_set(rbtrend_texture_view view, CRGPU_FILTER filter) {
	rbtrend_sampler sampler = rbtrend_builtin_downsample_sampler(view->instance, filter);
	if (!sampler)
		return nullptr;
	rbtrend_binding_set &set = view->present_sets[filter];
	if (set)
		return set;
	RBTREND_BINDING_SET_ENTRY entries[2]{};
	entries[0]._binding                  = 0;
	entries[0]._type                     = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	entries[0]._resource._image._view    = view;
	entries[1]._binding                  = 1;
	entries[1]._type                     = CRGPU_DESCRIPTOR_TYPE_SAMPLER;
	entries[1]._resource._image._sampler = sampler;
	(void)rbtrend_binding_set_create(view->instance->builtins.present_layout, entries, 2, &set, nullptr);
	if (!set)
		rbtrend_report(view->instance, RBT_RESULT_REND_GPU_FAILURE, "present: the binding set that reads a view can't be created");
	return set;
}

rbtrend_sampler rbtrend_builtin_downsample_sampler(rbtrend_instance instance, CRGPU_FILTER filter) {
	if (filter != CRGPU_FILTER_NEAREST && filter != CRGPU_FILTER_LINEAR) {
		rbtrend_report(instance, RBT_RESULT_INVALID_ARGUMENT, "%d is not a CRGPU_FILTER", (int)filter);
		return nullptr;
	}
	rbtrend_sampler &sampler = instance->builtins.downsample_samplers[filter];
	if (sampler)
		return sampler;

	RBTREND_SAMPLER_DESC desc{};
	desc.base = rbt_struct_base(RBT_STRUCT_TYPE_REND_SAMPLER_DESC);
	desc.mag_filter     = filter;
	desc.min_filter     = filter;
	desc.mipmap_filter  = CRGPU_FILTER_NEAREST;
	desc.address_mode_u = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.address_mode_v = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.address_mode_w = CRGPU_ADDRESS_MODE_CLAMP_TO_EDGE;
	desc.mip_lod_bias   = 0.0f;
	// The view it reads through has one level.
	desc.min_lod        = 0.0f;
	desc.max_lod        = 0.0f;
	desc.max_anisotropy = 1.0f;
	desc.debug_name = "rbtrend downsample";
	(void)rbtrend_sampler_create(instance, &desc, &sampler, nullptr);
	if (!sampler)
		rbtrend_report(instance, RBT_RESULT_REND_GPU_FAILURE, "Rend's downsample sampler can't be created");
	return sampler;
}

rbtrend_binding_set rbtrend_texture_view_bloom_set(rbtrend_texture_view view) {
	if (view->bloom_set)
		return view->bloom_set;
	rbtrend_sampler sampler = rbtrend_builtin_downsample_sampler(view->instance, CRGPU_FILTER_LINEAR);
	if (!sampler)
		return nullptr;
	RBTREND_BINDING_SET_ENTRY entries[2]{};
	entries[0]._binding                  = 0;
	entries[0]._type                     = CRGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	entries[0]._resource._image._view    = view;
	entries[1]._binding                  = 1;
	entries[1]._type                     = CRGPU_DESCRIPTOR_TYPE_SAMPLER;
	entries[1]._resource._image._sampler = sampler;
	(void)rbtrend_binding_set_create(rbtrend_builtin_bloom_layout(view->instance), entries, 2, &view->bloom_set, nullptr);
	if (!view->bloom_set)
		rbtrend_report(view->instance, RBT_RESULT_REND_GPU_FAILURE, "bloom: the binding set that reads a view can't be created");
	return view->bloom_set;
}

void rbtrend_record_fullscreen_pass_sets(
    crgpu_command_list cmd,
    rbtrend_graphics_pipeline pipeline,
    crgpu_image_view target,
    uint32_t width,
    uint32_t height,
    CRGPU_LOAD_OP load_op,
    const rbtrend_binding_set *sets,
    uint32_t set_count,
    uint32_t frame,
    const void *push,
    uint32_t push_size) {
	const uint32_t whole[4] = {0, 0, width, height};
	rbtrend_record_fullscreen_pass_region(cmd, pipeline, target, width, height, whole, load_op, sets, set_count, frame, push, push_size);
}

void rbtrend_record_fullscreen_pass_region(
    crgpu_command_list cmd,
    rbtrend_graphics_pipeline pipeline,
    crgpu_image_view target,
    uint32_t width,
    uint32_t height,
    const uint32_t region[4],
    CRGPU_LOAD_OP load_op,
    const rbtrend_binding_set *sets,
    uint32_t set_count,
    uint32_t frame,
    const void *push,
    uint32_t push_size) {
	crgpu_rendering_begin_desc pass{};
	pass.base.type = CR_STRUCT_TYPE_GPU_RENDERING_BEGIN_DESC;
	pass.color_attachments[0].image_view = target;
	pass.color_attachments[0].load_op    = load_op;
	pass.color_attachments[0].store_op   = CRGPU_STORE_OP_STORE;
	pass.color_attachment_count          = 1;
	pass.width                           = width;
	pass.height                          = height;
	crgpu_command_list_begin_rendering(cmd, &pass);
	CRGPU_VIEWPORT viewport{};
	viewport.width    = (float)width;
	viewport.height   = (float)height;
	viewport.maxDepth = 1.0f;
	crgpu_command_list_set_viewports(cmd, 0, 1, &viewport);
	CRGPU_SCISSOR_RECT scissor{};
	scissor.x      = (float)region[0];
	scissor.y      = (float)region[1];
	scissor.width  = (float)region[2];
	scissor.height = (float)region[3];
	crgpu_command_list_set_scissor_rects(cmd, 0, 1, &scissor);
	crgpu_command_list_bind_graphics_pipeline(cmd, pipeline->pipeline);
	for (uint32_t i = 0; i < set_count; ++i)
		if (sets[i])
			crgpu_command_list_bind_graphics_descriptor_sets(cmd, pipeline->pipeline, i, &sets[i]->sets[rbtrend_binding_set_frame_slot(sets[i], frame)], 1);
	if (push)
		crgpu_command_list_push_graphics_constants(cmd, pipeline->pipeline, 0, push_size, push);
	crgpu_command_list_draw(cmd, 3, 0);
	crgpu_command_list_end_rendering(cmd);
}

void rbtrend_record_fullscreen_pass(
    crgpu_command_list cmd,
    rbtrend_graphics_pipeline pipeline,
    crgpu_image_view target,
    uint32_t width,
    uint32_t height,
    CRGPU_LOAD_OP load_op,
    rbtrend_binding_set set,
    const void *push,
    uint32_t push_size) {
	rbtrend_record_fullscreen_pass_sets(cmd, pipeline, target, width, height, load_op, &set, 1, 0, push, push_size);
}

void rbtrend_builtins_destroy(rbtrend_instance instance) {
	rbtrend_builtins &builtins = instance->builtins;
	// Pipelines go before the layout they declare, and the release queue
	// keeps that order.
	for (const rbtrend_builtins::downsample_pipeline &entry : builtins.downsample_pipelines)
		rbtrend_graphics_pipeline_destroy(entry.pipeline);
	builtins.downsample_pipelines.clear();
	for (const rbtrend_builtins::bloom_pipeline &entry : builtins.bloom_pipelines)
		rbtrend_graphics_pipeline_destroy(entry.pipeline);
	builtins.bloom_pipelines.clear();
	for (const rbtrend_builtins::present_pipeline &entry : builtins.present_pipelines)
		rbtrend_graphics_pipeline_destroy(entry.pipeline);
	builtins.present_pipelines.clear();
	for (const rbtrend_builtins::blur_pipeline &entry : builtins.blur_pipelines)
		rbtrend_graphics_pipeline_destroy(entry.pipeline);
	builtins.blur_pipelines.clear();
	rbtrend_shader_destroy(builtins.blur_frag);
	builtins.blur_frag = nullptr;
	for (const rbtrend_builtins::outline_pipeline &entry : builtins.outline_pipelines)
		rbtrend_graphics_pipeline_destroy(entry.pipeline);
	builtins.outline_pipelines.clear();
	rbtrend_shader_destroy(builtins.outline_frag);
	builtins.outline_frag = nullptr;
	rbtrend_binding_layout_destroy(builtins.present_layout);
	builtins.present_layout = nullptr;
	rbtrend_shader_destroy(builtins.present_frag);
	builtins.present_frag = nullptr;
	rbtrend_binding_layout_destroy(builtins.bloom_layout);
	builtins.bloom_layout = nullptr;
	for (rbtrend_shader &shader : builtins.bloom_frags) {
		rbtrend_shader_destroy(shader);
		shader = nullptr;
	}
	rbtrend_binding_layout_destroy(builtins.downsample_layout);
	builtins.downsample_layout = nullptr;
	for (rbtrend_sampler &sampler : builtins.downsample_samplers) {
		rbtrend_sampler_destroy(sampler);
		sampler = nullptr;
	}
	rbtrend_shader_destroy(builtins.downsample_frag);
	builtins.downsample_frag = nullptr;
	rbtrend_shader_destroy(builtins.fullscreen_vert);
	builtins.fullscreen_vert = nullptr;
	rbtrend_lit_builtins_destroy(instance);
}
