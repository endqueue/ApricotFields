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

APRESULT aprend_swap_chain_create(
	aprend_instance instance,
	const APREND_SWAP_CHAIN_DESC *desc,
	aprend_swap_chain *out_swap_chain,
	APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (out_swap_chain)
		*out_swap_chain = nullptr;
	if (!instance)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is NULL");
	if (APRI_STALE(instance, "aprend_swap_chain_create", "instance"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "instance is destroyed");
	APREND_ENTER(instance, "aprend_swap_chain_create");
	if (!desc)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const APRESULT base = aprend_base_check(out_error, "desc", &desc->base, APRI_STRUCT_TYPE_APREND_SWAP_CHAIN_DESC))
		return base;
	if (!desc->surface)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "surface is NULL");
	if (!desc->queue)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "queue is NULL");
	if (desc->width == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "width is 0");
	if (desc->height == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "height is 0");
	if (!(instance->features & SPUDGPU_FEATURE_PRESENT))
		return aprend_fail(out_error, APRESULT_UNSUPPORTED, "the device was created without SPUDGPU_FEATURE_PRESENT");
	if (!out_swap_chain)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_swap_chain is NULL");

	APREND_MALLOC__T(result, aprend_swap_chain_t);
	if (!result)
		return aprend_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the swap chain");
	APREND_CONSTRUCT__T(result, aprend_swap_chain_t);
	result->instance = instance;
	result->desc     = *desc;

	SPUDRESULT sr = aprend_swap_chain_create_native(result);
	if (SPUDFAIL(sr)) {
		result->swap_chain = nullptr;
		APREND_DESTRUCT__T(result, aprend_swap_chain_t);
		return aprend_fail_spud(out_error, sr, "creating the swap chain failed");
	}
	*out_swap_chain = result;
	return APRESULT_OK;
}
void aprend_swap_chain_destroy(aprend_swap_chain swap_chain) {
	if (!swap_chain)
		return;
	if (APRI_STALE(swap_chain, "aprend_swap_chain_destroy", "swap_chain"))
		return;
	// A compiled list that presents into it would read it at submit.
	aprend_instance_forget(swap_chain->instance, swap_chain);
	APREND_DESTRUCT__T(swap_chain, aprend_swap_chain_t);
}
APRESULT aprend_swap_chain_resize(
	aprend_swap_chain swap_chain,
	uint32_t width,
	uint32_t height,
	APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!swap_chain)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "swap_chain is NULL");
	if (APRI_STALE(swap_chain, "aprend_swap_chain_resize", "swap_chain"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "swap_chain is destroyed");
	APREND_ENTER(swap_chain->instance, "aprend_swap_chain_resize");
	if (width == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "width is 0");
	if (height == 0)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "height is 0");
	if (swap_chain->swap_chain && width == swap_chain->desc.width && height == swap_chain->desc.height)
		return APRESULT_OK;

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
		swap_chain->swap_chain = nullptr;
		return aprend_fail_spud(out_error, sr, "creating the swap chain at %ux%u failed", width, height);
	}
	return APRESULT_OK;
}
APRESULT aprend_swap_chain_get_desc(
	aprend_swap_chain swap_chain,
	APREND_SWAP_CHAIN_DESC *out_desc) {
	if (!swap_chain)
		return APRESULT_INVALID_ARGUMENT;
	if (APRI_STALE(swap_chain, "aprend_swap_chain_get_desc", "swap_chain"))
		return APRESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return APRESULT_INVALID_ARGUMENT;
	*out_desc           = swap_chain->desc;
	out_desc->base.type = APRI_STRUCT_TYPE_APREND_SWAP_CHAIN_DESC;
	out_desc->base.next = nullptr;
	return APRESULT_OK;
}

APRESULT aprend_swap_chain_acquire(aprend_swap_chain swap_chain, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!swap_chain)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "swap_chain is NULL");
	if (APRI_STALE(swap_chain, "aprend_swap_chain_acquire", "swap_chain"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "swap_chain is destroyed");
	APREND_ENTER(swap_chain->instance, "aprend_swap_chain_acquire");
	if (!swap_chain->swap_chain)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the swap chain has no back buffers since a resize failed");
	if (swap_chain->acquired_image != APREND_SWAP_CHAIN_NO_IMAGE) {
		if (swap_chain->submitted)
			return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the submitted back buffer has not been presented yet");
		// Acquired but never submitted (e.g. that frame's list failed to
		// compile): it's still ours to use, so keep it.
		return APRESULT_OK;
	}
	const uint32_t image = spudgpu_swap_chain_acquire_next_image(swap_chain->swap_chain);
	// SpudGPU's acquire hands back UINT32_MAX when no image could be had.
	if (image == SPUD_UINT32_MAX)
		return aprend_fail(out_error, APRESULT_APREND_GPU_FAILURE, "SpudGPU acquired no back buffer");
	swap_chain->acquired_image = image;
	swap_chain->submitted      = false;
	return APRESULT_OK;
}
APRESULT aprend_swap_chain_get_back_buffer_size(
	aprend_swap_chain swap_chain,
	uint32_t *out_width,
	uint32_t *out_height) {
	if (!swap_chain)
		return APRESULT_INVALID_ARGUMENT;
	if (APRI_STALE(swap_chain, "aprend_swap_chain_get_back_buffer_size", "swap_chain"))
		return APRESULT_INVALID_ARGUMENT;
	if (!out_width)
		return APRESULT_INVALID_ARGUMENT;
	if (!out_height)
		return APRESULT_INVALID_ARGUMENT;
	if (!swap_chain->swap_chain)
		return APRESULT_INVALID_ARGUMENT;
	spudgpu_swap_chain_desc actual{};
	actual.base.type = SPUD_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	const SPUDRESULT sr = spudgpu_get_swap_chain_desc(swap_chain->swap_chain, &actual);
	if (SPUDFAIL(sr))
		return aprend_result_from_spud(sr);
	*out_width  = actual.width;
	*out_height = actual.height;
	return APRESULT_OK;
}
APRESULT aprend_swap_chain_present(aprend_swap_chain swap_chain, APREND_ERROR *out_error) {
	aprend_error_clear(out_error);
	if (!swap_chain)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "swap_chain is NULL");
	if (APRI_STALE(swap_chain, "aprend_swap_chain_present", "swap_chain"))
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "swap_chain is destroyed");
	APREND_ENTER(swap_chain->instance, "aprend_swap_chain_present");
	if (!swap_chain->swap_chain)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "the swap chain has no back buffers since a resize failed");
	if (swap_chain->acquired_image == APREND_SWAP_CHAIN_NO_IMAGE)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "no back buffer is acquired");
	if (!swap_chain->submitted)
		return aprend_fail(out_error, APRESULT_INVALID_ARGUMENT, "no list presenting into the acquired back buffer has been submitted");
	spudgpu_swap_chain_present(swap_chain->swap_chain);
	swap_chain->acquired_image = APREND_SWAP_CHAIN_NO_IMAGE;
	swap_chain->submitted      = false;
	return APRESULT_OK;
}

} // extern "C"
