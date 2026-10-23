#include "render/aprendswapchain.h"
#include "aprend_internal.hpp"

#include <cstdio>

aprend_swap_chain_t::~aprend_swap_chain_t() {
	if (swap_chain) {
		spudgpu_queue_wait_idle(desc.queue);
		spudgpu_destroy_swap_chain(swap_chain);
	}
}

static SPUDRESULT aprend_swap_chain_create_native(aprend_swap_chain_t *sc) {
	if (!sc)
		return SPUDRESULT_GENERAL_FAILURE;
	spudgpu_swap_chain_desc scd{};
	scd.base.type = SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	scd.surface         = sc->desc.surface;
	scd.queue           = sc->desc.queue;
	scd.width           = sc->desc.width;
	scd.height          = sc->desc.height;
	scd.buffer_count    = sc->desc.buffer_count;
	scd.format          = sc->desc.format;
	scd.present_mode    = sc->desc.present_mode;
	// Aprend's back buffers are the window's size and it scales with a draw
	// of its own (APREND_TODO.md item 40), so the compositor has nothing to
	// scale: one to one, which every surface takes.
	scd.present_scaling = SPUDGPU_PRESENT_SCALING_ONE_TO_ONE;
	scd.fullscreen_mode = SPUDGPU_FULLSCREEN_MODE_WINDOWED;
	// APREND_COMMAND_PRESENT_TEXTURE copies into the back buffer.
	scd.usage = SPUDGPU_IMAGE_USAGE_TRANSFER_DST;
	const SPUDRESULT sr = spudgpu_create_swap_chain(sc->instance->desc.device, &scd, &sc->swap_chain);
	// New back buffers: a list compiled against the old ones is refused.
	++sc->generation;
	return sr;
}

extern "C" {

aprend_swap_chain aprend_swap_chain_create(
	aprend_instance instance,
	const aprend_swap_chain_desc *desc) {
	if (!instance || !desc)
		return nullptr;
	if (!aprend_base_ok("aprend_swap_chain_create", "desc", &desc->base, APRI_STRUCT_TYPE_APREND_SWAP_CHAIN_DESC))
		return nullptr;
	if (!desc->surface || !desc->queue || !desc->width || !desc->height)
		return nullptr;
	if (!(instance->features & SPUDGPU_FEATURE_PRESENT)) {
		printf("apricot: aprend_swap_chain_create: the device was created without SPUDGPU_FEATURE_PRESENT\n");
		return nullptr;
	}

	APREND_MALLOC__T(result, aprend_swap_chain_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_swap_chain_t);
	result->instance = instance;
	result->desc     = *desc;

	SPUDRESULT sr = aprend_swap_chain_create_native(result);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_swap_chain_create failed: %s\n", spudresult_str(sr));
		result->swap_chain = nullptr;
		APREND_DESTRUCT__T(result, aprend_swap_chain_t);
		return nullptr;
	}
	return result;
}
void aprend_swap_chain_destroy(aprend_swap_chain swap_chain) {
	if (!swap_chain)
		return;
	// A compiled list that presents into it would read it at submit.
	aprend_instance_forget(swap_chain->instance, swap_chain);
	APREND_DESTRUCT__T(swap_chain, aprend_swap_chain_t);
}
bool aprend_swap_chain_resize(
	aprend_swap_chain swap_chain,
	uint32_t width,
	uint32_t height) {
	if (!swap_chain || !width || !height)
		return false;
	if (swap_chain->swap_chain && width == swap_chain->desc.width && height == swap_chain->desc.height)
		return true;

	if (swap_chain->swap_chain) {
		spudgpu_queue_wait_idle(swap_chain->desc.queue);
		spudgpu_destroy_swap_chain(swap_chain->swap_chain);
		swap_chain->swap_chain = nullptr;
	}
	swap_chain->acquired_image = APREND_SWAP_CHAIN_NO_IMAGE;
	swap_chain->submitted      = false;
	swap_chain->desc.width     = width;
	swap_chain->desc.height    = height;

	SPUDRESULT sr = aprend_swap_chain_create_native(swap_chain);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_swap_chain_resize to %ux%u failed: %s\n", width, height, spudresult_str(sr));
		swap_chain->swap_chain = nullptr;
		return false;
	}
	return true;
}
bool aprend_swap_chain_get_desc(
	aprend_swap_chain swap_chain,
	aprend_swap_chain_desc *out_desc) {
	if (!swap_chain || !out_desc)
		return false;
	*out_desc      = swap_chain->desc;
	out_desc->base.type = APRI_STRUCT_TYPE_APREND_SWAP_CHAIN_DESC;
	out_desc->base.next = nullptr;
	return true;
}

bool aprend_swap_chain_acquire(aprend_swap_chain swap_chain) {
	if (!swap_chain)
		return false;
	if (!swap_chain->swap_chain)
		return false;
	if (swap_chain->acquired_image != APREND_SWAP_CHAIN_NO_IMAGE) {
		if (swap_chain->submitted) {
			printf("aprend: aprend_swap_chain_acquire before presenting the submitted back buffer\n");
			return false;
		}
		// Acquired but never submitted (e.g. that frame's list failed to
		// compile): it's still ours to use, so keep it.
		return true;
	}
	const uint32_t image = spudgpu_swap_chain_acquire_next_image(swap_chain->swap_chain);
	// SpudGPU's acquire hands back UINT32_MAX when no image could be had.
	if (image == SPUD_UINT32_MAX) {
		printf("aprend: aprend_swap_chain_acquire: SpudGPU acquired no back buffer\n");
		return false;
	}
	swap_chain->acquired_image = image;
	swap_chain->submitted      = false;
	return true;
}
bool aprend_swap_chain_get_back_buffer_size(
	aprend_swap_chain swap_chain,
	uint32_t *out_width,
	uint32_t *out_height) {
	if (!swap_chain)
		return false;
	if (!out_width)
		return false;
	if (!out_height)
		return false;
	if (!swap_chain->swap_chain)
		return false;
	spudgpu_swap_chain_desc actual{};
	actual.base.type = SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	if (SPUDFAIL(spudgpu_get_swap_chain_desc(swap_chain->swap_chain, &actual)))
		return false;
	*out_width  = actual.width;
	*out_height = actual.height;
	return true;
}
bool aprend_swap_chain_present(aprend_swap_chain swap_chain) {
	if (!swap_chain)
		return false;
	if (!swap_chain->swap_chain)
		return false;
	if (swap_chain->acquired_image == APREND_SWAP_CHAIN_NO_IMAGE || !swap_chain->submitted) {
		printf("aprend: aprend_swap_chain_present without a submitted list presenting into the acquired back buffer\n");
		return false;
	}
	spudgpu_swap_chain_present(swap_chain->swap_chain);
	swap_chain->acquired_image = APREND_SWAP_CHAIN_NO_IMAGE;
	swap_chain->submitted      = false;
	return true;
}

} // extern "C"
