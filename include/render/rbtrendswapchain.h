
#ifndef RBTREND_SWAP_CHAIN_H
#define RBTREND_SWAP_CHAIN_H

#include "rbtrendcontext.h"

/**
 * @file rbtrendswapchain.h
 * @brief The swap chain: presents finished images to a window.
 *
 * A frame that presents goes:
 *
 * 1. rbtrend_swap_chain_acquire()
 * 2. record and compile a command list ending in
 *    RBTREND_COMMAND_PRESENT_TEXTURE
 * 3. rbtrend_command_list_submit(), on the swap chain's queue
 * 4. rbtrend_swap_chain_present()
 *
 * Each step fails (returns false) when called out of that order.
 *
 * @par Threads
 * The rule is in rbtrendcontext.h ("Threads"). A swap chain is used by one
 * caller at a time with everything else of its instance, its destroy
 * included: acquire, the submission and present are steps of one frame,
 * made one after another. Its resize and destroy block their caller until
 * the swap chain's queue is idle.
 */

#if __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration descriptor for a swap chain.
 */
typedef struct RBTREND_SWAP_CHAIN_DESC {
	/** This struct's type, RBT_STRUCT_TYPE_REND_SWAP_CHAIN_DESC, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/**
	 * Created by the caller from its window (crgpu_surface_create()), and
	 * still owned by it: it must outlive the swap chain. Never NULL.
	 */
	crgpu_surface surface;
	/**
	 * The queue that presents, and that lists presenting into this swap
	 * chain must be submitted on. Never NULL.
	 */
	crgpu_command_queue queue;
	/** Width of the back buffers in pixels. Never 0. */
	uint32_t width;
	/** Height of the back buffers in pixels. Never 0. */
	uint32_t height;
	/** How many back buffers. Passed to Carrottus GPU as it is. */
	uint32_t buffer_count;
	/** Pixel format of the back buffers. Passed to Carrottus GPU as it is. */
	CRGPU_FORMAT format;
	/** How presenting is paced. Passed to Carrottus GPU as it is. */
	CRGPU_PRESENT_MODE present_mode;
} RBTREND_SWAP_CHAIN_DESC;

/**
 * @brief Opaque handle to a swap chain.
 */
typedef struct rbtrend_swap_chain_t *rbtrend_swap_chain;

/**
 * @brief Creates a swap chain on a surface. It is windowed, its back
 * buffers are copy destinations (RBTREND_COMMAND_PRESENT_TEXTURE copies into
 * one), and its present scaling is CRGPU_PRESENT_SCALING_ONE_TO_ONE: a
 * back buffer of another size than the window is shown pixel for pixel from
 * the top left, never scaled by the compositor. The caller resizes the swap
 * chain to the window's size (rbtrend_swap_chain_resize()).
 *
 * @param[in]  instance       Instance whose device the swap chain is
 *                            created on.
 * @param[in]  desc           Swap chain configuration. Copied: only read
 *                            during the call.
 * @param[out] out_swap_chain Receives the swap chain; NULL on failure.
 * @param[out] out_error      Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p instance, @p desc
 *         or @p out_swap_chain is NULL, `surface` or `queue` is NULL, or
 *         `width` or `height` is 0; RBT_RESULT_UNSUPPORTED if the device was
 *         created without CRGPU_FEATURE_PRESENT; the struct codes for
 *         `desc->base`; RBT_RESULT_OUT_OF_MEMORY; or the code for Carrottus GPU's
 *         result if it refuses the swap chain.
 *
 * @see rbtrend_swap_chain_destroy()
 */
RBT_RESULT rbtrend_swap_chain_create(
    rbtrend_instance instance,
    const RBTREND_SWAP_CHAIN_DESC *desc,
    rbtrend_swap_chain *out_swap_chain,
    RBTREND_ERROR *out_error);

/**
 * @brief Destroys a swap chain.
 *
 * Waits for the queue to go idle first, so nothing in flight still uses it.
 * Unlike the other destroy calls it is not deferred (rbtrendcontext.h,
 * "Destroying"). A compiled, unsubmitted command list that presents into it
 * is refused at submit.
 *
 * @param[in] swap_chain Swap chain to destroy. NULL is accepted and does
 *                       nothing.
 */
void rbtrend_swap_chain_destroy(rbtrend_swap_chain swap_chain);

/**
 * @brief Recreates the back buffers at a new size.
 *
 * Waits for the queue to go idle first. Drops any acquired, not yet
 * presented back buffer. A command list compiled to present into the old
 * back buffers is refused at submit (rbtrend_command_list_submit()), even
 * when the new swap chain's acquired back buffer has the same index.
 *
 * @param[in] swap_chain Swap chain to resize.
 * @param[in] width      New width in pixels. Never 0.
 * @param[in] height     New height in pixels. Never 0.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK if the swap chain is that size now, which it is at
 *         once if it already was; RBT_RESULT_INVALID_ARGUMENT if
 *         @p swap_chain is NULL or @p width or @p height is 0; or the code
 *         for Carrottus GPU's result if the device refuses the new swap chain,
 *         which leaves it with no back buffers until a resize succeeds.
 */
RBT_RESULT rbtrend_swap_chain_resize(
    rbtrend_swap_chain swap_chain,
    uint32_t width,
    uint32_t height,
    RBTREND_ERROR *out_error);

/**
 * @brief Reads a swap chain's descriptor.
 *
 * @param[in]  swap_chain Swap chain to read.
 * @param[out] out_desc   Receives the descriptor it was created with,
 *                        except that `width` and `height` follow
 *                        rbtrend_swap_chain_resize().
 *
 * @return RBT_RESULT_OK, or RBT_RESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p swap_chain or @p out_desc is NULL.
 */
RBT_RESULT rbtrend_swap_chain_get_desc(
    rbtrend_swap_chain swap_chain,
    RBTREND_SWAP_CHAIN_DESC *out_desc);

/**
 * @brief Acquires the next back buffer.
 *
 * Call before compiling the list that presents into it. If a back buffer is
 * already acquired but nothing presenting into it was submitted, that one
 * is kept.
 *
 * @param[in]  swap_chain Swap chain to acquire from.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p swap_chain is NULL,
 *         it has no back buffers (a failed resize), or a list presenting
 *         into the acquired back buffer was submitted and it has not been
 *         presented yet; RBT_RESULT_REND_GPU_FAILURE if Carrottus GPU acquired no
 *         image. No back buffer is acquired on a failure.
 */
RBT_RESULT rbtrend_swap_chain_acquire(rbtrend_swap_chain swap_chain, RBTREND_ERROR *out_error);

/**
 * @brief Reads the size of the back buffers as they were made.
 *
 * Usually the size asked for at creation or the last resize, but the surface
 * can dictate another on some backends (on Vulkan, a window not yet laid
 * out). RBTREND_COMMAND_PRESENT_TEXTURE needs a source at least this size.
 *
 * @param[in]  swap_chain Swap chain to read.
 * @param[out] out_width  Receives the back buffers' width in pixels.
 * @param[out] out_height Receives their height in pixels.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT, with nothing written, if
 *         @p swap_chain, @p out_width or @p out_height is NULL or the swap
 *         chain has no back buffers (a failed resize); or the code for
 *         Carrottus GPU's result if it can't say.
 *
 * @see rbtrend_swap_chain_get_desc()
 */
RBT_RESULT rbtrend_swap_chain_get_back_buffer_size(
    rbtrend_swap_chain swap_chain,
    uint32_t *out_width,
    uint32_t *out_height);

/**
 * @brief Presents the acquired back buffer, after the list that presents
 * into it has been submitted.
 *
 * @param[in]  swap_chain Swap chain to present.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK, or RBT_RESULT_INVALID_ARGUMENT if @p swap_chain is
 *         NULL, it has no back buffers, or no list presenting into an
 *         acquired back buffer has been submitted.
 */
RBT_RESULT rbtrend_swap_chain_present(rbtrend_swap_chain swap_chain, RBTREND_ERROR *out_error);

#if __cplusplus
}
#endif

#endif // RBTREND_SWAP_CHAIN_H
