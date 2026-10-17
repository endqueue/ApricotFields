#ifndef APREND_RENDER_BLOOM_H
#define APREND_RENDER_BLOOM_H

#include "aprendcontext.h"
#include "aprendimages.h"

/**
 * @file aprendbloom.h
 * @brief Bloom: a soft glow spread out from a source texture and added onto
 * a target, recorded into a command list (APREND_COMMAND_BLOOM,
 * aprendcommands.h).
 *
 * What glows is the caller's. The source is any texture the caller gives,
 * and every texel of it glows unless a threshold is set: a caller that
 * wants only some things to glow draws those alone into a source of their
 * own and blooms that with a threshold of 0. With a threshold, only what is
 * brighter than it glows, as in a bloom of a whole HDR picture. Aprend
 * never decides what is worth glowing.
 *
 * A bloom is a chain of passes at falling sizes over a texture the
 * aprend_bloom holds, in SPUDGPU_FORMAT_R16G16B16A16_FLOAT so that the sums
 * don't clip or band:
 *
 * 1. the source, read through 13 taps and weighted by the threshold, into
 *    the chain's level 0, at half the size the bloom was made for;
 * 2. each further level from the one above, through the same 13 taps;
 * 3. each level read back up through a tent of taps `_radius` texels apart
 *    and added into the level above, from the smallest up, so level 0 ends
 *    holding the sum of them all;
 * 4. level 0 spread over the target through the same tent, times
 *    `_intensity`, and added to what the target holds.
 *
 * The glow is added to color and alpha alike. With a premultiplied source,
 * the glow carries the source's coverage, so a glow over a clear target is
 * seen once the target is composited over something else.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A bloom is used by one thread
 * at a time with everything else of its instance: it is read by
 * aprend_command_list_compile(), which is made by one thread at a time
 * across the instance.
 */

#if __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration descriptor for a bloom. No field has a default.
 */
typedef struct aprend_bloom_desc {
	/**
	 * Width of the source the bloom is for, in texels. Never 0. The chain's
	 * level 0 is half of it; a source of another size is read at the same
	 * places in [0, 1], and its glow is that much coarser or finer.
	 */
	uint32_t width;
	/** Height of the source the bloom is for. Never 0. */
	uint32_t height;
	/**
	 * How many levels the chain has, from 1 to as many as halving the size
	 * twice over allows (a level's sides stop at 1 texel). More levels is a
	 * wider glow and more passes: two passes a level, and two more.
	 */
	uint32_t levels;
#if _DEBUG
	/** A name for diagnostics. May be NULL. */
	const char *debug_name;
#endif
} aprend_bloom_desc;

/**
 * @brief Opaque handle to a bloom: its chain of levels, and what it renders
 * them through.
 */
typedef struct aprend_bloom_t *aprend_bloom;

/**
 * @brief Creates a bloom.
 *
 * @param[in] instance Instance whose device the chain is created on.
 * @param[in] desc     Bloom configuration. Only read during the call.
 *
 * @return The bloom, or NULL if @p instance or @p desc is NULL, `width`,
 *         `height` or `levels` is 0, `levels` is more than the size allows,
 *         or the chain can't be created. Each is printed.
 *
 * @see aprend_bloom_destroy()
 */
aprend_bloom aprend_bloom_create(aprend_instance instance, const aprend_bloom_desc *desc);

/**
 * @brief Remakes a bloom's chain for a source of a new size. Its levels stay
 * as many, and the resize fails if the new size can't hold them.
 *
 * Commands that name the bloom are compiled again before they are
 * submitted: what they recorded rendered into the chain that was replaced.
 *
 * @param[in] bloom  Bloom to resize.
 * @param[in] width  The source's new width. Never 0.
 * @param[in] height The source's new height. Never 0.
 *
 * @return true if the chain is the new size, or already was. false, with
 *         the bloom as it was, if @p bloom is NULL, a size is 0, the size
 *         can't hold the levels, or the new chain can't be created.
 */
bool aprend_bloom_resize(aprend_bloom bloom, uint32_t width, uint32_t height);

/**
 * @brief Destroys a bloom.
 *
 * @warning No later submission may use @p bloom. A submission already made
 * that uses it is safe: the GPU's side of it is released when that work has
 * finished (aprendcontext.h, "Destroying").
 *
 * @param[in] bloom Bloom to destroy. NULL is accepted and does nothing.
 */
void aprend_bloom_destroy(aprend_bloom bloom);

/**
 * @brief Reads the descriptor a bloom was created with, at its current size.
 *
 * @param[in] bloom Bloom to read.
 *
 * @return The descriptor, or a zeroed one if @p bloom is NULL.
 */
aprend_bloom_desc aprend_bloom_get_desc(aprend_bloom bloom);

#if __cplusplus
}
#endif

#endif // APREND_RENDER_BLOOM_H
