
#ifndef APREND_PASS_H
#define APREND_PASS_H

/**
 * @file aprendpass.h
 * @brief Render passes and compute passes: a pipeline with what it renders
 * to and the binding sets it always reads, checked against each other once
 * and begun with one command.
 *
 * A render pass is what a caller would otherwise send as BEGIN_RENDERING,
 * SET_SHADER_PIPELINE, SET_VIEWPORTS, SET_SCISSOR_RECTS and a SET_BINDING_SET
 * for each fixed input, every frame, with the formats and layouts left to
 * agree by luck. It is begun with APREND_COMMAND_BEGIN_RENDER_PASS and ended
 * with APREND_COMMAND_END_RENDERING (aprendcommands.h); between the two the
 * caller draws, binds materials or other sets in the slots the pass leaves
 * open, and may set the viewport, scissor and push constants as in any
 * pass. A compute pass is a compute pipeline and its fixed binding sets, set
 * with APREND_COMMAND_SET_COMPUTE_PASS before dispatches.
 *
 * What is checked when a pass is created: the pipeline's color and depth
 * formats against the targets', the targets' view types, and each fixed
 * binding set's layout against the one the pipeline declares at its slot.
 * What is checked at every compile is unchanged: the texture layout rules
 * of APREND_COMMAND_SET_BINDING_SET, and that every slot a draw's pipeline
 * declares holds a set.
 *
 * A pass is a description, not a GPU object. A command that names one is
 * expanded into the commands above when it is sent: the views, framebuffer
 * attachments and sets it holds then are what the list records. So a pass
 * may be destroyed, or changed, once the commands are sent, and a list
 * recorded before a change keeps what it recorded.
 *
 * @par Framebuffers
 * A render pass may render to an aprend_framebuffer (aprendframes.h) in place
 * of views. Its attachments are read when the begin command is sent, so a
 * pass made once follows aprend_framebuffer_resize() for every list recorded
 * after the resize. The load and store ops and the clear values are the
 * pass's, one for each of the framebuffer's attachments.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A pass is used by one thread
 * at a time with everything else of its instance, except that sending a
 * command that names it only reads it, and the framebuffer it renders to:
 * lists may be recorded from several threads at once with one pass, as long
 * as no thread changes or destroys the pass, its framebuffer, or what they
 * hold meanwhile. aprend_render_pass_destroy() and
 * aprend_compute_pass_destroy() may be called from any thread, as any
 * destroy call.
 */

#include "aprendframes.h"
#include "aprendpipeline.h"
#include "aprendtimer.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @brief Opaque handle to a render pass.
 */
typedef struct aprend_render_pass_t *aprend_render_pass;

/**
 * @brief Upper bound on the color targets of a render pass: as many as a
 * pipeline can describe (aprend_graphics_pipeline_desc::color_attachment_count).
 */
#define APREND_RENDER_PASS_MAX_COLOR_TARGETS SPUDGPU_MAX_COLOR_ATTACHMENTS

/**
 * @brief Configuration descriptor for a render pass.
 *
 * No field has a default.
 */
typedef struct aprend_render_pass_desc {
	/**
	 * The pipeline the pass draws with, set as the pass begins. Not NULL. It
	 * must outlive the pass. Another may be set inside the pass with
	 * SET_SHADER_PIPELINE, if it renders to the same formats.
	 */
	aprend_graphics_pipeline _pipeline;
	/**
	 * The framebuffer the pass renders to, or NULL to name the target views
	 * in #_color_targets and #_depth_target. With a framebuffer every
	 * `view` there is NULL, #_color_target_count is the framebuffer's
	 * attachment count, and #_depth_target is read for its depth attachment
	 * if it has one. It must outlive the pass.
	 */
	aprend_framebuffer _framebuffer;
	/**
	 * The color targets, as BEGIN_RENDERING takes them (aprendimages.h): the
	 * first #_color_target_count are read. Each `view` is of type
	 * APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET, of a 2D texture whose format is
	 * the pipeline's `color_attachment_formats` at the same index, and
	 * `resolve_view` is NULL.
	 */
	aprend_color_target _color_targets[APREND_RENDER_PASS_MAX_COLOR_TARGETS];
	/**
	 * How many color targets: the pipeline's `color_attachment_count`.
	 */
	uint32_t _color_target_count;
	/**
	 * The depth target. Its `view` is NULL for a pipeline whose
	 * `depth_format` is SPUDGPU_FORMAT_UNKNOWN; otherwise a view of type
	 * APREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL of a 2D texture of that format.
	 */
	aprend_depth_target _depth_target;
	/**
	 * The binding sets the pass always reads: `_binding_sets[N]` is put at
	 * set slot N as the pass begins. NULL leaves slot N to the caller, who
	 * fills it inside the pass with SET_MATERIAL or SET_BINDING_SET before
	 * the first draw that needs it. A set given is of the layout the
	 * pipeline declares at its slot. Slots at or past the pipeline's
	 * `_binding_layout_count` are NULL. The sets must outlive the pass.
	 */
	aprend_binding_set _binding_sets[APREND_MAX_BINDING_LAYOUTS];
	/**
	 * A GPU timer (aprendtimer.h) each recording of the pass is timed into,
	 * at slot #_timer_slot; NULL to time nothing. Of the pass's instance,
	 * with #_timer_slot below its slot count. It must outlive the pass.
	 */
	aprend_gpu_timer _timer;
	/** The slot of #_timer. Read only with a timer. */
	uint32_t _timer_slot;
} aprend_render_pass_desc;

/**
 * @brief Creates a render pass.
 *
 * @param[in] instance Instance the pass belongs to.
 * @param[in] desc     Pass configuration. Only read during the call.
 *
 * @return The pass, or NULL if @p instance or @p desc is NULL, `_pipeline`
 *         is NULL or of another instance, `_color_target_count` is above
 *         APREND_RENDER_PASS_MAX_COLOR_TARGETS or doesn't match the
 *         pipeline's color target count (or, with a framebuffer, its
 *         attachment count), a target has the wrong type, shape or format or a
 *         `resolve_view`, a depth target is given for a pipeline without
 *         depth or missing for one with it, a view is given alongside a
 *         framebuffer, the pass would have no target, the targets differ in
 *         shape or layer count, a binding set is of another layout than
 *         the pipeline's at its slot or is given past the pipeline's last
 *         slot, or `_timer` is of another instance or `_timer_slot` past its
 *         slots.
 *
 * @see aprend_render_pass_destroy()
 */
aprend_render_pass aprend_render_pass_create(
    aprend_instance instance,
    const aprend_render_pass_desc *desc);

/**
 * @brief Destroys a render pass. Nothing it names is destroyed.
 *
 * Commands already sent that name it have been expanded and don't read it
 * again, so this may follow the last send at once.
 *
 * @param[in] pass Pass to destroy. NULL is accepted and does nothing.
 */
void aprend_render_pass_destroy(aprend_render_pass pass);

/**
 * @brief Puts a binding set in one of a render pass's fixed slots, or
 * leaves the slot to the caller.
 *
 * Commands sent before the change keep the set they recorded.
 *
 * @param[in] pass Pass to change.
 * @param[in] slot The set slot. Below the pipeline's
 *                 `_binding_layout_count`.
 * @param[in] set  The set, of the layout the pipeline declares at @p slot,
 *                 or NULL to leave the slot to the caller.
 *
 * @return false, with nothing changed, if @p pass is NULL, @p slot is not
 *         one the pipeline declares, or @p set is of another layout.
 */
bool aprend_render_pass_set_binding_set(
    aprend_render_pass pass,
    uint32_t slot,
    aprend_binding_set set);

/**
 * @brief Gets the pipeline a render pass begins with.
 *
 * @param[in] pass Pass to read.
 *
 * @return The pipeline, or NULL if @p pass is NULL.
 */
aprend_graphics_pipeline aprend_render_pass_get_pipeline(aprend_render_pass pass);

/**
 * @brief Gets the size of what a render pass renders to now: the mip of its
 * first target the view covers, or its framebuffer's size.
 *
 * It is the size the next begin command sent is recorded with, and the
 * viewport and scissor it sets.
 *
 * @param[in]  pass       Pass to read.
 * @param[out] out_width  Receives the width. May be NULL.
 * @param[out] out_height Receives the height. May be NULL.
 *
 * @return false, with nothing written, if @p pass is NULL.
 */
bool aprend_render_pass_get_size(
    aprend_render_pass pass,
    uint32_t *out_width,
    uint32_t *out_height);

/**
 * @brief Opaque handle to a compute pass.
 */
typedef struct aprend_compute_pass_t *aprend_compute_pass;

/**
 * @brief Configuration descriptor for a compute pass.
 *
 * No field has a default.
 */
typedef struct aprend_compute_pass_desc {
	/** The pipeline the pass's dispatches run. Not NULL. It must outlive the pass. */
	aprend_compute_pipeline _pipeline;
	/**
	 * The binding sets put at each slot when the pass is set, as
	 * aprend_render_pass_desc::_binding_sets: NULL leaves a slot to the
	 * caller.
	 */
	aprend_binding_set _binding_sets[APREND_MAX_BINDING_LAYOUTS];
} aprend_compute_pass_desc;

/**
 * @brief Creates a compute pass.
 *
 * @param[in] instance Instance the pass belongs to.
 * @param[in] desc     Pass configuration. Only read during the call.
 *
 * @return The pass, or NULL if @p instance or @p desc is NULL, `_pipeline`
 *         is NULL or of another instance, or a binding set is of another
 *         layout than the pipeline's at its slot or is given past the
 *         pipeline's last slot.
 *
 * @see aprend_compute_pass_destroy()
 */
aprend_compute_pass aprend_compute_pass_create(
    aprend_instance instance,
    const aprend_compute_pass_desc *desc);

/**
 * @brief Destroys a compute pass, as aprend_render_pass_destroy() does a
 * render pass.
 *
 * @param[in] pass Pass to destroy. NULL is accepted and does nothing.
 */
void aprend_compute_pass_destroy(aprend_compute_pass pass);

/**
 * @brief Puts a binding set in one of a compute pass's fixed slots, as
 * aprend_render_pass_set_binding_set() does.
 *
 * @param[in] pass Pass to change.
 * @param[in] slot The set slot.
 * @param[in] set  The set, or NULL to leave the slot to the caller.
 *
 * @return false, with nothing changed, under the conditions of
 *         aprend_render_pass_set_binding_set().
 */
bool aprend_compute_pass_set_binding_set(
    aprend_compute_pass pass,
    uint32_t slot,
    aprend_binding_set set);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_PASS_H
