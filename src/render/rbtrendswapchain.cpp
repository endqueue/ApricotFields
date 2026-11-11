#include "render/rbtrendswapchain.h"
#include "rbtrend_internal.hpp"

#include <cstdio>

rbtrend_swap_chain_t::~rbtrend_swap_chain_t() {
	if (swap_chain) {
		crgpu_command_queue_wait_idle(desc.queue);
		crgpu_swap_chain_destroy(swap_chain);
	}
}

static CR_RESULT rbtrend_swap_chain_create_native(rbtrend_swap_chain_t *sc) {
	if (!sc)
		return CR_RESULT_GENERAL_FAILURE;
	crgpu_swap_chain_desc scd{};
	scd.base.type = CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	scd.surface         = sc->desc.surface;
	scd.queue           = sc->desc.queue;
	scd.width           = sc->desc.width;
	scd.height          = sc->desc.height;
	scd.buffer_count    = sc->desc.buffer_count;
	scd.format          = sc->desc.format;
	scd.present_mode    = sc->desc.present_mode;
	// Rend's back buffers are the window's size and it scales with a draw
	// of its own (RBTREND_TODO.md item 40), so the compositor has nothing to
	// scale: one to one, which every surface takes.
	scd.present_scaling = CRGPU_PRESENT_SCALING_ONE_TO_ONE;
	scd.fullscreen_mode = CRGPU_FULLSCREEN_MODE_WINDOWED;
	// RBTREND_COMMAND_PRESENT_TEXTURE copies into the back buffer.
	scd.usage = CRGPU_IMAGE_USAGE_TRANSFER_DST;
	const CR_RESULT sr = crgpu_swap_chain_create(sc->instance->desc.device, &scd, &sc->swap_chain);
	// New back buffers: a list compiled against the old ones is refused.
	++sc->generation;
	return sr;
}

extern "C" {

RBT_RESULT rbtrend_swap_chain_create(
	rbtrend_instance instance,
	const RBTREND_SWAP_CHAIN_DESC *desc,
	rbtrend_swap_chain *out_swap_chain,
	RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (out_swap_chain)
		*out_swap_chain = nullptr;
	if (!instance)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is NULL");
	if (RBT_STALE(instance, "rbtrend_swap_chain_create", "instance"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "instance is destroyed");
	RBTREND_ENTER(instance, "rbtrend_swap_chain_create");
	if (!desc)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "desc is NULL");
	if (const RBT_RESULT base = rbtrend_base_check(out_error, "desc", &desc->base, RBT_STRUCT_TYPE_REND_SWAP_CHAIN_DESC))
		return base;
	if (!desc->surface)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "surface is NULL");
	if (!desc->queue)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "queue is NULL");
	if (desc->width == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "width is 0");
	if (desc->height == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "height is 0");
	if (!(instance->features & CRGPU_FEATURE_PRESENT))
		return rbtrend_fail(out_error, RBT_RESULT_UNSUPPORTED, "the device was created without CRGPU_FEATURE_PRESENT");
	if (!out_swap_chain)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "out_swap_chain is NULL");

	RBTREND_MALLOC__T(result, rbtrend_swap_chain_t);
	if (!result)
		return rbtrend_fail(out_error, RBT_RESULT_OUT_OF_MEMORY, "no memory for the swap chain");
	RBTREND_CONSTRUCT__T(result, rbtrend_swap_chain_t);
	result->instance = instance;
	result->desc     = *desc;

	CR_RESULT sr = rbtrend_swap_chain_create_native(result);
	if (CR_FAILED(sr)) {
		result->swap_chain = nullptr;
		RBTREND_DESTRUCT__T(result, rbtrend_swap_chain_t);
		return rbtrend_fail_cr(out_error, sr, "creating the swap chain failed");
	}
	*out_swap_chain = result;
	return RBT_RESULT_OK;
}
void rbtrend_swap_chain_destroy(rbtrend_swap_chain swap_chain) {
	if (!swap_chain)
		return;
	if (RBT_STALE(swap_chain, "rbtrend_swap_chain_destroy", "swap_chain"))
		return;
	// A compiled list that presents into it would read it at submit.
	rbtrend_instance_forget(swap_chain->instance, swap_chain);
	RBT_RETIRE(swap_chain->instance, swap_chain, rbtrend_swap_chain_t);
}
RBT_RESULT rbtrend_swap_chain_resize(
	rbtrend_swap_chain swap_chain,
	uint32_t width,
	uint32_t height,
	RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!swap_chain)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "swap_chain is NULL");
	if (RBT_STALE(swap_chain, "rbtrend_swap_chain_resize", "swap_chain"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "swap_chain is destroyed");
	RBTREND_ENTER(swap_chain->instance, "rbtrend_swap_chain_resize");
	if (width == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "width is 0");
	if (height == 0)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "height is 0");
	if (swap_chain->swap_chain && width == swap_chain->desc.width && height == swap_chain->desc.height)
		return RBT_RESULT_OK;

	if (swap_chain->swap_chain) {
		crgpu_command_queue_wait_idle(swap_chain->desc.queue);
		crgpu_swap_chain_destroy(swap_chain->swap_chain);
		swap_chain->swap_chain = nullptr;
	}
	swap_chain->acquired_image = RBTREND_SWAP_CHAIN_NO_IMAGE;
	swap_chain->submitted      = false;
	swap_chain->desc.width     = width;
	swap_chain->desc.height    = height;

	CR_RESULT sr = rbtrend_swap_chain_create_native(swap_chain);
	if (CR_FAILED(sr)) {
		swap_chain->swap_chain = nullptr;
		return rbtrend_fail_cr(out_error, sr, "creating the swap chain at %ux%u failed", width, height);
	}
	return RBT_RESULT_OK;
}
RBT_RESULT rbtrend_swap_chain_get_desc(
	rbtrend_swap_chain swap_chain,
	RBTREND_SWAP_CHAIN_DESC *out_desc) {
	if (!swap_chain)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(swap_chain, "rbtrend_swap_chain_get_desc", "swap_chain"))
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_desc)
		return RBT_RESULT_INVALID_ARGUMENT;
	*out_desc           = swap_chain->desc;
	out_desc->base.type = RBT_STRUCT_TYPE_REND_SWAP_CHAIN_DESC;
	out_desc->base.next = nullptr;
	return RBT_RESULT_OK;
}

RBT_RESULT rbtrend_swap_chain_acquire(rbtrend_swap_chain swap_chain, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!swap_chain)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "swap_chain is NULL");
	if (RBT_STALE(swap_chain, "rbtrend_swap_chain_acquire", "swap_chain"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "swap_chain is destroyed");
	RBTREND_ENTER(swap_chain->instance, "rbtrend_swap_chain_acquire");
	if (!swap_chain->swap_chain)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the swap chain has no back buffers since a resize failed");
	if (swap_chain->acquired_image != RBTREND_SWAP_CHAIN_NO_IMAGE) {
		if (swap_chain->submitted)
			return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the submitted back buffer has not been presented yet");
		// Acquired but never submitted (e.g. that frame's list failed to
		// compile): it's still ours to use, so keep it.
		return RBT_RESULT_OK;
	}
	const uint32_t image = crgpu_swap_chain_acquire_next_image(swap_chain->swap_chain);
	// Carrottus GPU's acquire hands back UINT32_MAX when no image could be had.
	if (image == CR_UINT32_MAX)
		return rbtrend_fail(out_error, RBT_RESULT_REND_GPU_FAILURE, "Carrottus GPU acquired no back buffer");
	swap_chain->acquired_image = image;
	swap_chain->submitted      = false;
	return RBT_RESULT_OK;
}
RBT_RESULT rbtrend_swap_chain_get_back_buffer_size(
	rbtrend_swap_chain swap_chain,
	uint32_t *out_width,
	uint32_t *out_height) {
	if (!swap_chain)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (RBT_STALE(swap_chain, "rbtrend_swap_chain_get_back_buffer_size", "swap_chain"))
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_width)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!out_height)
		return RBT_RESULT_INVALID_ARGUMENT;
	if (!swap_chain->swap_chain)
		return RBT_RESULT_INVALID_ARGUMENT;
	crgpu_swap_chain_desc actual{};
	actual.base.type = CR_STRUCT_TYPE_GPU_SWAP_CHAIN_DESC;
	const CR_RESULT sr = crgpu_swap_chain_get_desc(swap_chain->swap_chain, &actual);
	if (CR_FAILED(sr))
		return rbtrend_result_from_cr(sr);
	*out_width  = actual.width;
	*out_height = actual.height;
	return RBT_RESULT_OK;
}
RBT_RESULT rbtrend_swap_chain_present(rbtrend_swap_chain swap_chain, RBTREND_ERROR *out_error) {
	rbtrend_error_clear(out_error);
	if (!swap_chain)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "swap_chain is NULL");
	if (RBT_STALE(swap_chain, "rbtrend_swap_chain_present", "swap_chain"))
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "swap_chain is destroyed");
	RBTREND_ENTER(swap_chain->instance, "rbtrend_swap_chain_present");
	if (!swap_chain->swap_chain)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "the swap chain has no back buffers since a resize failed");
	if (swap_chain->acquired_image == RBTREND_SWAP_CHAIN_NO_IMAGE)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "no back buffer is acquired");
	if (!swap_chain->submitted)
		return rbtrend_fail(out_error, RBT_RESULT_INVALID_ARGUMENT, "no list presenting into the acquired back buffer has been submitted");
	crgpu_swap_chain_present(swap_chain->swap_chain);
	swap_chain->acquired_image = RBTREND_SWAP_CHAIN_NO_IMAGE;
	swap_chain->submitted      = false;
	return RBT_RESULT_OK;
}

} // extern "C"
