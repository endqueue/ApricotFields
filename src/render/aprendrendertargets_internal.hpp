#ifndef APREND_RENDER_TARGETS_INTERNAL_HPP
#define APREND_RENDER_TARGETS_INTERNAL_HPP

/* Internal header - never included outside ApricotFields/src/render/.
 *
 * The instance's pool of render targets: textures a command takes at
 * compile for the passes it records in between (a bloom's chain), and gives
 * back once the frame it was taken in has finished on the GPU. So a command
 * of a built-in effect needs no texture from its caller, and its caller
 * manages no size or lifetime: an effect asks for a size, a format and a
 * level count each time it is compiled, and gets a texture nobody else is
 * using in this frame or in one the GPU may still be running.
 *
 * Used on the thread using the instance, under the one-thread rule: a
 * compile, a submit, aprend_instance_next_frame and the instance's
 * destructor. Nothing here is locked. */

#include "render/aprendimages.h"

#include <cstdint>
#include <vector>

struct aprend_command_list_t;

/* How many frames an entry nobody has taken is kept before it is retired:
 * long enough that a size used every few frames stays, short enough that
 * the sizes of a window being dragged go. */
#define APREND_RENDER_TARGET_IDLE_FRAMES 120u

/* One texture of the pool, with a 2D view to render and one to read each of
 * its levels. Made by the first take that finds none free of its key. */
struct aprend_pooled_target {
	uint32_t width{0};
	uint32_t height{0};
	SPUDGPU_FORMAT format{SPUDGPU_FORMAT_UNKNOWN};
	uint32_t levels{0};
	aprend_texture2d texture{nullptr};
	std::vector<aprend_texture_view> level_targets{};
	std::vector<aprend_texture_view> level_sources{};
	/* Whether it has been taken, and the instance's frame_number when it
	 * last was. */
	bool taken{false};
	uint64_t taken_frame{0};
	/* The frame it was made or last taken in, given back or not: what
	 * APREND_RENDER_TARGET_IDLE_FRAMES counts from. */
	uint64_t used_frame{0};
};

struct aprend_render_target_pool {
	std::vector<aprend_pooled_target *> targets{};
	/* Every frame numbered below this has finished on the GPU, as far as
	 * aprend_instance_next_frame has waited: an entry taken in one of them
	 * is free. */
	uint64_t free_below{0};
};

/* What a compile took from the pool, kept by the list until it is submitted:
 * an entry, and what it held before the take, to give it back if the
 * compile is never submitted. */
struct aprend_pool_take {
	aprend_pooled_target *target;
	bool was_taken;
	uint64_t was_taken_frame;
	uint64_t frame;
};

/* An entry of the pool of [width] by [height] in [format] with [levels]
 * levels, render target and shader resource, free since the frame it was
 * last taken in finished; made if there is none. Marked taken in the current
 * frame, and the take added to [cmd_list]'s. NULL, printed, if one can't be
 * made. Its contents are undefined. */
aprend_pooled_target *aprend_render_target_take(
    aprend_command_list_t *cmd_list,
    uint32_t width,
    uint32_t height,
    SPUDGPU_FORMAT format,
    uint32_t levels);

/* Gives back what [cmd_list]'s takes hold, for a compile that will not be
 * submitted: compiled again, reset, failed or destroyed. Only takes of the
 * current frame are given back, and only if still the entry's last: one of
 * an earlier frame frees itself once that frame has finished, and putting it
 * back late could undo a take made since. */
void aprend_render_target_give_back(aprend_command_list_t *cmd_list);

/* From aprend_instance_next_frame, once its wait has succeeded: every frame
 * before the one now [frames_in_flight] back has finished, and an entry free
 * and untaken for APREND_RENDER_TARGET_IDLE_FRAMES is retired. */
void aprend_render_target_pool_begin_frame(aprend_instance instance);

/* From the instance's destructor, before its release queue is emptied:
 * retires every entry, views before their texture. */
void aprend_render_target_pool_destroy(aprend_instance instance);

#endif // APREND_RENDER_TARGETS_INTERNAL_HPP
