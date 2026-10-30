#ifndef APRENDFRAMES_H
#define APRENDFRAMES_H

#include "aprendimages.h"

/**
 * @file aprendframes.h
 * @brief Framebuffers: a set of color attachments and an optional depth
 * attachment of one size, created and resized together.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A framebuffer is used by one
 * caller at a time with everything else of its instance. Its clear and
 * resize calls act on its attachments as the texture calls of
 * aprendimages.h do, and are bound by the same rule as those.
 */

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @brief One color attachment of a framebuffer.
 */
/* Frozen: an array element of APREND_FRAMEBUFFER_DESC. It never changes; a new need gets a new type. */
typedef struct APREND_FRAMEBUFFER_TEXTURE_SPEC {
	/**
	 * A texture of the caller's to use as the attachment, or NULL for the
	 * framebuffer to create one. If it is not NULL, #format is ignored, and
	 * the texture stays the caller's: the framebuffer never destroys or
	 * resizes it.
	 */
	aprend_texture2d existing_texture;
	/** Texel format of the texture the framebuffer creates. */
	SPUDGPU_FORMAT format;
	/** Whether a shader samples the texture the framebuffer creates. */
	bool shader_resource;
} APREND_FRAMEBUFFER_TEXTURE_SPEC;

/**
 * @brief Configuration descriptor for a framebuffer.
 */
typedef struct APREND_FRAMEBUFFER_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_FRAMEBUFFER_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Width of the framebuffer in texels. Never 0. */
	uint32_t width;
	/** Height of the framebuffer in texels. Never 0. */
	uint32_t height;
	/**
	 * The color attachments, #attachment_count of them. Never NULL. Copied
	 * at creation: the array only has to live for the call.
	 */
	const APREND_FRAMEBUFFER_TEXTURE_SPEC *attachments;
	/** How many color attachments. Never 0. */
	uint32_t attachment_count;
	/**
	 * Format of the depth and stencil attachment, which the framebuffer
	 * creates. SPUDGPU_FORMAT_UNKNOWN for no depth attachment.
	 */
	SPUDGPU_FORMAT depth_format;
	/** Sample count of the textures the framebuffer creates. 0 is taken as 1. */
	uint32_t sample_count;
} APREND_FRAMEBUFFER_DESC;

/**
 * @brief Opaque handle to a framebuffer.
 */
typedef struct aprend_framebuffer_t *aprend_framebuffer;

/**
 * @brief Creates a framebuffer and the attachments it doesn't take from the
 * caller.
 *
 * Each attachment is a view of mip 0 of layer 0 of its texture. A texture
 * the framebuffer creates has one mip and one layer.
 *
 * @param[in]  instance        Instance the framebuffer's textures are
 *                             created on.
 * @param[in]  desc            Framebuffer configuration. Only read during
 *                             the call.
 * @param[out] out_framebuffer Receives the framebuffer; NULL on failure.
 * @param[out] out_error       Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance, @p desc or
 *         @p out_framebuffer is NULL, `width`, `height` or
 *         `attachment_count` is 0, or `attachments` is NULL; the struct
 *         codes for `desc->base`; or the code a texture or view of it
 *         couldn't be created with (aprendimages.h).
 *
 * @see aprend_framebuffer_destroy()
 */
APRESULT aprend_framebuffer_create(
    aprend_instance instance,
    const APREND_FRAMEBUFFER_DESC *desc,
    aprend_framebuffer *out_framebuffer,
    APREND_ERROR *out_error);

/**
 * @brief Destroys a framebuffer, its views, and the textures it created.
 *
 * The GPU doesn't have to be finished with them (aprendcontext.h,
 * "Destroying"). No later submission may use one of its attachments.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p framebuffer itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] framebuffer Framebuffer to destroy. NULL is accepted and does
 *                        nothing.
 */
void aprend_framebuffer_destroy(aprend_framebuffer framebuffer);

/**
 * @brief Reads a framebuffer's descriptor.
 *
 * @param[in]  framebuffer Framebuffer to read.
 * @param[out] out_desc    Receives the descriptor it was created with,
 *                         except that `width` and `height` follow
 *                         aprend_framebuffer_resize(). Its `attachments`
 *                         points at the framebuffer's own copy, which lasts
 *                         as long as the framebuffer.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p framebuffer or @p out_desc is NULL.
 */
APRESULT aprend_framebuffer_get_desc(
    aprend_framebuffer framebuffer,
    APREND_FRAMEBUFFER_DESC *out_desc);

/**
 * @brief Recreates a framebuffer's attachments at a new size.
 *
 * Every view the framebuffer handed out is destroyed and made again, and
 * so is every texture it created: fetch the attachments again afterwards.
 * A texture given as `existing_texture` is not resized; it is the caller's
 * to resize first.
 *
 * @param[in] framebuffer Framebuffer to resize.
 * @param[in] width       New width in texels. Never 0.
 * @param[in] height      New height in texels. Never 0.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK if the framebuffer is that size now, which it is at
 *         once if it already was; APRESULT_INVALID_ARGUMENT if
 *         @p framebuffer is NULL or @p width or @p height is 0; or the code
 *         an attachment couldn't be created with, which leaves the
 *         framebuffer with only the attachments made before the failure.
 */
APRESULT aprend_framebuffer_resize(
    aprend_framebuffer framebuffer,
    uint32_t width,
    uint32_t height,
    APREND_ERROR *out_error);

/**
 * @brief Clears every color attachment of a framebuffer to one color.
 *
 * Recorded into the frame's batch of updates and clears and not waited
 * for, under the same rules as aprend_texture2d_update() (aprendimages.h;
 * aprendcontext.h, "Texture updates and clears"): the clear runs after
 * everything submitted before it and ahead of everything after, and the
 * attachments are left in the image layout they were found in.
 *
 * @param[in] framebuffer Framebuffer to clear.
 * @param[in] queue       Queue to submit on. Never NULL.
 * @param[in] r           Red.
 * @param[in] g           Green.
 * @param[in] b           Blue.
 * @param[in] a           Alpha.
 * @param[out] out_error  Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p framebuffer or
 *         @p queue is NULL or @p queue is not the instance's; or the code
 *         for SpudGPU's result if the clears can't be recorded.
 */
APRESULT aprend_framebuffer_clear_colors(
    aprend_framebuffer framebuffer,
    spudgpu_command_queue queue,
    float r,
    float g,
    float b,
    float a,
    APREND_ERROR *out_error);

/**
 * @brief Clears the depth, the stencil or both of a framebuffer's depth
 * attachment.
 *
 * Recorded as aprend_framebuffer_clear_colors() is.
 *
 * @param[in] framebuffer   Framebuffer to clear.
 * @param[in] queue         Queue to submit on. Never NULL.
 * @param[in] clear_depth   Whether the depth is cleared, to @p depth.
 * @param[in] clear_stencil Whether the stencil is cleared, to @p stencil.
 * @param[in] depth         Depth value.
 * @param[in] stencil       Stencil value.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK, with nothing done if neither @p clear_depth nor
 *         @p clear_stencil is set; APRESULT_INVALID_ARGUMENT if
 *         @p framebuffer or @p queue is NULL, @p queue is not the
 *         instance's, or the framebuffer has no depth attachment; or the
 *         code for SpudGPU's result if the clear can't be recorded.
 */
APRESULT aprend_framebuffer_clear_depth(
    aprend_framebuffer framebuffer,
    spudgpu_command_queue queue,
    bool clear_depth,
    bool clear_stencil,
    float depth,
    uint32_t stencil,
    APREND_ERROR *out_error);

/**
 * @brief Gets one color attachment's view, for a pass's color target.
 *
 * @param[in] framebuffer Framebuffer to read.
 * @param[in] index       Which attachment, from 0.
 *
 * @return The view, owned by the framebuffer: not to be destroyed, and no
 *         longer valid after aprend_framebuffer_resize(). NULL if
 *         @p framebuffer is NULL or @p index is not below its attachment
 *         count.
 */
aprend_texture_view aprend_framebuffer_get_color_attachment(
    aprend_framebuffer framebuffer,
    uint32_t index);

/**
 * @brief Gets the depth attachment's view, for a pass's depth target.
 *
 * @param[in] framebuffer Framebuffer to read.
 *
 * @return The view, owned by the framebuffer: not to be destroyed, and no
 *         longer valid after aprend_framebuffer_resize(). NULL if
 *         @p framebuffer is NULL or it has no depth attachment.
 */
aprend_texture_view aprend_framebuffer_get_depth_attachment(aprend_framebuffer framebuffer);

#if __cplusplus
}
#endif // __cplusplus

#endif // APRENDFRAMES_H
