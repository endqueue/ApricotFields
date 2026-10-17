
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

#include <cstdio>

/* The chain's format: float, so the sums of the upsample don't clip or band,
 * and one every backend renders to, filters and blends. */
#define APREND_BLOOM_CHAIN_FORMAT SPUDGPU_FORMAT_R16G16B16A16_FLOAT

/* The most levels a chain for a source [width] by [height] holds: its level
 * 0 is half the source, and each level halves again down to 1 texel. */
static uint32_t aprend_bloom_max_levels(uint32_t width, uint32_t height) {
	uint32_t side   = aprend_mip_size(width, 1) > aprend_mip_size(height, 1) ? aprend_mip_size(width, 1) : aprend_mip_size(height, 1);
	uint32_t levels = 1;
	while (side > 1) {
		side >>= 1;
		++levels;
	}
	return levels;
}

/* Retires [bloom]'s views and then its chain, so the views go first. */
static void aprend_bloom_release_chain(aprend_bloom bloom) {
	for (aprend_texture_view view : bloom->level_targets)
		aprend_destroy_texture_view(view);
	for (aprend_texture_view view : bloom->level_sources)
		aprend_destroy_texture_view(view);
	bloom->level_targets.clear();
	bloom->level_sources.clear();
	aprend_texture2d_destroy(bloom->chain);
	bloom->chain = nullptr;
}

/* Makes a chain for a source [width] by [height] of [levels] levels into
 * [bloom], which holds none. False, printed, with nothing kept, if it can't
 * be made. */
static bool aprend_bloom_make_chain(aprend_bloom bloom, uint32_t width, uint32_t height, uint32_t levels) {
	aprend_texture2d_desc texture_desc{};
	texture_desc.width        = aprend_mip_size(width, 1);
	texture_desc.height       = aprend_mip_size(height, 1);
	texture_desc.format       = APREND_BLOOM_CHAIN_FORMAT;
	texture_desc.usage        = APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE | APREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
	texture_desc.mip_levels   = levels;
	texture_desc.array_layers = 1;
	texture_desc.sample_count = 1;
#if _DEBUG
	texture_desc.debug_name = bloom->desc.debug_name ? bloom->desc.debug_name : "aprend bloom chain";
#endif
	bloom->chain = aprend_texture2d_create(bloom->instance, &texture_desc);
	if (!bloom->chain) {
		printf("apricot: a bloom's chain of %u levels for %ux%u can't be created\n", levels, width, height);
		return false;
	}
	try {
		bloom->level_targets.assign(levels, nullptr);
		bloom->level_sources.assign(levels, nullptr);
	} catch (...) {
		printf("apricot: no memory for a bloom's views\n");
		aprend_bloom_release_chain(bloom);
		return false;
	}
	for (uint32_t level = 0; level < levels; ++level) {
		aprend_texture_view_desc view_desc{};
		view_desc.shape          = APREND_TEXTURE_VIEW_SHAPE_2D;
		view_desc.aspect         = APREND_TEXTURE_VIEW_ASPECT_COLOR;
		view_desc.base_mip       = level;
		view_desc.mip_count      = 1;
		view_desc.base_layer     = 0;
		view_desc.layer_count    = 1;
		view_desc.type           = APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET;
		bloom->level_targets[level] = aprend_texture_view_create_2d(bloom->chain, &view_desc);
		view_desc.type           = APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE;
		bloom->level_sources[level] = aprend_texture_view_create_2d(bloom->chain, &view_desc);
		if (!bloom->level_targets[level] || !bloom->level_sources[level]) {
			printf("apricot: the views of level %u of a bloom's chain can't be created\n", level);
			aprend_bloom_release_chain(bloom);
			return false;
		}
	}
	return true;
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

extern "C" {

aprend_bloom aprend_bloom_create(aprend_instance instance, const aprend_bloom_desc *desc) {
	if (!instance) {
		printf("apricot: aprend_bloom_create: instance is NULL\n");
		return nullptr;
	}
	if (!desc) {
		printf("apricot: aprend_bloom_create: desc is NULL\n");
		return nullptr;
	}
	if (desc->width == 0) {
		printf("apricot: aprend_bloom_create: width is 0\n");
		return nullptr;
	}
	if (desc->height == 0) {
		printf("apricot: aprend_bloom_create: height is 0\n");
		return nullptr;
	}
	if (desc->levels == 0) {
		printf("apricot: aprend_bloom_create: levels is 0\n");
		return nullptr;
	}
	const uint32_t most = aprend_bloom_max_levels(desc->width, desc->height);
	if (desc->levels > most) {
		printf("apricot: aprend_bloom_create: %u levels, and a bloom for %ux%u has at most %u\n", desc->levels, desc->width, desc->height, most);
		return nullptr;
	}

	aprend_bloom_t *bloom = (aprend_bloom_t *)malloc(sizeof(aprend_bloom_t));
	if (!bloom) {
		printf("apricot: aprend_bloom_create: out of memory\n");
		return nullptr;
	}
	bloom           = new (bloom) aprend_bloom_t();
	bloom->instance = instance;
	bloom->desc     = *desc;
#if _DEBUG
	// The caller's string is not kept.
	bloom->desc.debug_name = nullptr;
#endif
	if (!aprend_bloom_make_chain(bloom, desc->width, desc->height, desc->levels)) {
		bloom->~aprend_bloom_t();
		free(bloom);
		return nullptr;
	}
	return bloom;
}

bool aprend_bloom_resize(aprend_bloom bloom, uint32_t width, uint32_t height) {
	if (!bloom)
		return false;
	if (width == 0)
		return false;
	if (height == 0)
		return false;
	if (width == bloom->desc.width && height == bloom->desc.height)
		return true;
	const uint32_t most = aprend_bloom_max_levels(width, height);
	if (bloom->desc.levels > most) {
		printf("apricot: aprend_bloom_resize: the bloom has %u levels, and one for %ux%u has at most %u\n", bloom->desc.levels, width, height, most);
		return false;
	}

	// The new chain is made before the old goes, so a failure leaves the
	// bloom as it was.
	aprend_bloom_t old{};
	old.instance      = bloom->instance;
	old.chain         = bloom->chain;
	old.level_targets = std::move(bloom->level_targets);
	old.level_sources = std::move(bloom->level_sources);
	bloom->chain      = nullptr;
	bloom->level_targets.clear();
	bloom->level_sources.clear();
	if (!aprend_bloom_make_chain(bloom, width, height, bloom->desc.levels)) {
		bloom->chain         = old.chain;
		bloom->level_targets = std::move(old.level_targets);
		bloom->level_sources = std::move(old.level_sources);
		return false;
	}
	// Submitted work may still be rendering into it.
	aprend_bloom_release_chain(&old);
	bloom->desc.width  = width;
	bloom->desc.height = height;
	return true;
}

void aprend_bloom_destroy(aprend_bloom bloom) {
	if (!bloom)
		return;
	// The views and the chain are retired, and go when the GPU has finished
	// with them; the handle holds nothing the GPU reads, and goes now.
	aprend_bloom_release_chain(bloom);
	bloom->~aprend_bloom_t();
	free(bloom);
}

aprend_bloom_desc aprend_bloom_get_desc(aprend_bloom bloom) { return bloom ? bloom->desc : aprend_bloom_desc{}; }

} // extern "C"
