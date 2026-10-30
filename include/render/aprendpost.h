#ifndef APREND_POST_H
#define APREND_POST_H

#include "aprendcommands.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @file aprendpost.h
 * @brief Adapting exposure: the brightness a tone map exposes a picture at,
 * worked out from the picture on the GPU and eased from frame to frame, as
 * an eye adapts.
 *
 * APREND_COMMAND_AUTO_EXPOSURE (aprendcommands.h) measures a picture and
 * moves the exposure towards what brings its log-average luminance to a key
 * (middle grey, 0.18), within a range of luminance; APREND_COMMAND_TONE_MAP
 * with the exposure named in `_auto_exposure` multiplies by it. The value
 * stays on the GPU: there is nothing to read back and nothing to wait for.
 *
 * What is measured is the picture's log-average luminance (Reinhard's), of
 * every texel, or of those between the darkest and brightest shares the
 * desc leaves out (APREND_EXPOSURE_DESC::low_share and
 * APREND_EXPOSURE_DESC::high_share): a histogram of 64 bins over the range
 * of luminance, as Unreal's eye adaptation measures between its low and
 * high percent, so a bright window or a dark corner taking a few percent of
 * the picture doesn't move the exposure.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads").
 */

/**
 * @brief Configuration descriptor for an exposure. No field has a default.
 */
typedef struct APREND_EXPOSURE_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_EXPOSURE_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The luminance the picture's log-average is brought to: 0.18 is middle
	 * grey. Above 0. */
	float key;
	/** The range of log-average luminance exposed for, as base 2 logs
	 * (stops): darker than min_log is exposed as min_log, brighter than
	 * max_log as max_log. min_log below max_log. */
	float min_log;
	/** The top of that range: brighter than it is exposed as it. Above
	 * #min_log. */
	float max_log;
	/** How fast the exposure moves, in stops a second (or rather: the share
	 * of the way it closes in a second is 1 - e^-rate): brightening a dark
	 * picture, and darkening a bright one. Eyes adapt to the dark slower:
	 * 3 and 1 are usual. 0 never moves; above 0. */
	float up_rate;
	/** As #up_rate, for darkening a bright picture. 1 is usual; above 0. */
	float down_rate;
	/**
	 * The share of the picture's texels, the darkest first, left out of the
	 * measure: 0 to 1, below #high_share. 0 leaves none out.
	 */
	float low_share;
	/**
	 * The share up to which the texels, the darkest first, are measured:
	 * above #low_share, at most 1. 1 leaves none of the brightest out.
	 * With #low_share 0 and this 1 every texel is measured, and the measure
	 * is their exact log average; otherwise it is the log average of those
	 * between the two shares, read from a histogram of 64 bins from min_log
	 * to max_log (each bin standing for its middle, a bin split where a
	 * share's edge falls in it, and luminance past the range in the bin at
	 * its end), worked out with a compute dispatch. Unreal's defaults are
	 * 0.1 and 0.9.
	 */
	float high_share;
} APREND_EXPOSURE_DESC;

/**
 * @brief Creates an exposure. Its first AUTO_EXPOSURE takes the exposure it
 * measures at once, with nothing to ease from.
 *
 * @param[in]  instance     Instance the exposure is of.
 * @param[in]  desc         The exposure's configuration. Only read during
 *                          the call.
 * @param[out] out_exposure Receives the exposure; NULL on failure.
 * @param[out] out_error    Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if an argument is NULL or
 *         a value is out of its range (the shares too); the struct codes for
 *         `desc->base`; APRESULT_OUT_OF_MEMORY; or
 *         APRESULT_APREND_GPU_FAILURE if a texture, buffer or set can't be
 *         created.
 */
APRESULT aprend_exposure_create(
    aprend_instance instance,
    const APREND_EXPOSURE_DESC *desc,
    aprend_exposure *out_exposure,
    APREND_ERROR *out_error);

/**
 * @brief Destroys an exposure. The GPU doesn't have to be finished with it.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p exposure itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] exposure Exposure to destroy. NULL is accepted and does nothing.
 */
void aprend_exposure_destroy(aprend_exposure exposure);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_POST_H
