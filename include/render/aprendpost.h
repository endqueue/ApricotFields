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
 * Unreal measures with a histogram of luminance, which can leave the
 * darkest and brightest few percent out; a log average is Reinhard's, and
 * is what this is.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads").
 */

/**
 * @brief Configuration descriptor for an exposure. No field has a default.
 */
typedef struct aprend_exposure_desc {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_EXPOSURE_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The luminance the picture's log-average is brought to: 0.18 is middle
	 * grey. Above 0. */
	float key;
	/** The range of log-average luminance exposed for, as base 2 logs
	 * (stops): darker than min_log is exposed as min_log, brighter than
	 * max_log as max_log. min_log below max_log. */
	float min_log;
	float max_log;
	/** How fast the exposure moves, in stops a second (or rather: the share
	 * of the way it closes in a second is 1 - e^-rate): brightening a dark
	 * picture, and darkening a bright one. Eyes adapt to the dark slower:
	 * 3 and 1 are usual. 0 never moves; above 0. */
	float up_rate;
	float down_rate;
} aprend_exposure_desc;

/**
 * @brief Creates an exposure. Its first AUTO_EXPOSURE takes the exposure it
 * measures at once, with nothing to ease from.
 *
 * @return The exposure, or NULL if an argument is NULL, a value is out of
 *         its range, or a texture can't be created (printed).
 */
aprend_exposure aprend_exposure_create(aprend_instance instance, const aprend_exposure_desc *desc);

/**
 * @brief Destroys an exposure. The GPU doesn't have to be finished with it.
 *
 * @param[in] exposure Exposure to destroy. NULL is accepted and does nothing.
 */
void aprend_exposure_destroy(aprend_exposure exposure);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_POST_H
