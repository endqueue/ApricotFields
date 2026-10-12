
#ifndef APREND_COMMANDS_H
#define APREND_COMMANDS_H

#include "aprendcontext.h"
#include "aprendimages.h"
#include "aprendtransient.h"

/**
 * @file aprendcommands.h
 * @brief The command list: commands are recorded into it, compiled into a
 * SpudGPU command list, and submitted.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). What is particular to a
 * command list:
 *
 * - aprend_send_command() and aprend_command_list_reset() write the list and
 *   nothing else, so different lists may be recorded from different threads
 *   at once, one thread to a list. What the commands name must not be
 *   destroyed by another thread meanwhile.
 * - aprend_command_list_create(), aprend_command_list_compile(),
 *   aprend_command_list_submit() and aprend_command_list_destroy() are not
 *   part of that. Every list of an instance is created from and compiled
 *   through the instance's one SpudGPU command allocator; compiling reads
 *   the layouts a submission writes; and a submission takes the next place
 *   in the instance's order. They are made by one thread at a time across
 *   the whole instance, not only one list.
 */

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_vertex_buffer_t *aprend_vertex_buffer;
typedef struct aprend_index_buffer_t *aprend_index_buffer;
typedef struct aprend_storage_buffer_t *aprend_storage_buffer;
typedef struct aprend_storage_buffer_set_t *aprend_storage_buffer_set;
typedef struct aprend_graphics_pipeline_t *aprend_graphics_pipeline;
typedef struct aprend_compute_pipeline_t *aprend_compute_pipeline;
typedef struct aprend_binding_set_t *aprend_binding_set;
typedef struct aprend_swap_chain_t *aprend_swap_chain;
typedef struct aprend_material_t *aprend_material;
typedef struct aprend_render_pass_t *aprend_render_pass;
typedef struct aprend_compute_pass_t *aprend_compute_pass;
typedef struct aprend_draw_list_t *aprend_draw_list;

/**
 * @brief Upper bound on color targets in one BEGIN_RENDERING pass.
 *
 * aprend_color_target and aprend_depth_target (aprendimages.h) describe how
 * one pass uses each attachment view. Zero-initialized ops mean LOAD and
 * STORE.
 */
#define APREND_MAX_COLOR_TARGETS SPUDGPU_MAX_COLOR_ATTACHMENTS

/**
 * @brief Which command an APREND_COMMAND is, and so which member of its
 * `_params` is read.
 *
 * The names below are short for the values: SET_VERTEX_BUFFERS is
 * APREND_COMMAND_SET_VERTEX_BUFFERS.
 */
typedef uint32_t APREND_COMMAND_TYPE;

enum {
	/** No command. */
	APREND_COMMAND_NONE                   = 0,
	/**
	 * Puts `_vertex_buffers[i]` at vertex buffer slot `_start_slot` + i.
	 * Slot N is what a pipeline's `_vertex_bindings[N]` reads
	 * (aprendpipeline.h), per vertex or per instance as that binding says.
	 * At each draw, every slot the bound pipeline reads must hold a buffer
	 * whose stride is the binding's.
	 */
	APREND_COMMAND_SET_VERTEX_BUFFERS     = 1,
	/**
	 * The index buffer of the following indexed draws (DRAW_INDEXED,
	 * DRAW_INSTANCED_INDEXED, DRAW_INDEXED_INDIRECT), until another is set.
	 * An indexed draw with none set fails the compile.
	 */
	APREND_COMMAND_SET_INDEX_BUFFER       = 2,
	/**
	 * The graphics pipeline of the following draws. Inside a pass only, and
	 * it lasts until the pass ends: each pass sets its own pipeline before
	 * it draws.
	 */
	APREND_COMMAND_SET_SHADER_PIPELINE    = 3,
	/**
	 * Draws `_vertex_count` vertices, the first at `_start_vertex_location`
	 * of the vertex buffers set.
	 */
	APREND_COMMAND_DRAW                   = 4,
	/**
	 * Draws `_index_count` indices of the index buffer set, the first at
	 * `_start_index_location`, each with `_base_vertex_location` added to it
	 * before it picks a vertex.
	 */
	APREND_COMMAND_DRAW_INDEXED           = 5,
	/**
	 * As DRAW, `_instance_count` times: `_vertex_count_per_instance`
	 * vertices each, the first instance being `_start_instance_location`.
	 */
	APREND_COMMAND_DRAW_INSTANCED         = 6,
	/**
	 * As DRAW_INDEXED, `_instance_count` times: `_index_count_per_instance`
	 * indices each, the first instance being `_start_instance_location`.
	 */
	APREND_COMMAND_DRAW_INSTANCED_INDEXED = 7,
	/**
	 * Sets `_viewport_count` viewports, the first of them viewport
	 * `_first_viewport`.
	 */
	APREND_COMMAND_SET_VIEWPORTS          = 8,
	/**
	 * Sets `_scissor_rect_count` scissor rectangles, the first of them
	 * rectangle `_first_scissor_rect`.
	 */
	APREND_COMMAND_SET_SCISSOR_RECTS      = 9,
	/**
	 * Begins a pass. Draws are only valid inside a pass; BEGIN_RENDERING
	 * can't be nested and must be closed by END_RENDERING before the list is
	 * compiled. aprend_command_list_compile() fails on any violation.
	 *
	 * The targets of one pass are all 2D views, each one layer, or all 2D
	 * array views of the same number of layers, which makes it a layered
	 * pass: every layer is rendered to, cleared and stored together, and the
	 * pass's vertex or mesh shader writes the layer each primitive is drawn
	 * to (aprend_texture_view_desc, aprendimages.h). Mixing the two shapes
	 * or two layer counts in one pass fails the compile.
	 */
	APREND_COMMAND_BEGIN_RENDERING        = 10,
	/** Ends the pass BEGIN_RENDERING began. */
	APREND_COMMAND_END_RENDERING          = 11,
	/**
	 * Puts an aprend_binding_set (aprendpipeline.h) at set slot `_slot` for
	 * the following draws, until another is put there. It stays through
	 * SET_SHADER_PIPELINE and from one pass to the next, so it may be sent
	 * before or after the pipeline, inside a pass or outside. At each draw,
	 * every slot the bound pipeline declares must hold a set created from
	 * the layout the pipeline declares there, or a layout of the same
	 * bindings (aprendpipeline.h, "Layouts of the same bindings").
	 *
	 * Before a pass begins, every texture in a set that a draw of the pass
	 * reads (one in a slot the pipeline bound at that draw declares) is
	 * moved into the layout its binding reads it in. A set left in a slot no
	 * draw of the pass reads is not touched. Layouts are kept for each mip
	 * of each layer, and a view is moved over the range it covers: the same
	 * mip of the same layer can't be a target of the pass and in one of its
	 * sets, nor in a sampled slot and a storage image slot of one pass, but
	 * two views of one texture that share no mip of a layer can, so a mip
	 * can be rendered from the one above it.
	 */
	APREND_COMMAND_SET_BINDING_SET        = 12,
	/**
	 * Copies `_source` into `_swap_chain`'s acquired back buffer
	 * (aprendswapchain.h) and leaves it ready to present. Outside any pass;
	 * at most one per list. Where the sizes differ, only the overlapping
	 * top-left region is copied. The back buffer must have been acquired
	 * before compiling.
	 */
	APREND_COMMAND_PRESENT_TEXTURE        = 13,
	/**
	 * Writes `_size` bytes at byte `_offset` of the bound pipeline's push
	 * constant block, for the following draws. After SET_SHADER_PIPELINE, in
	 * the same pass: the bytes are written for that pipeline and are not
	 * carried to the next one set. `_offset` and `_size` are multiples of 4,
	 * `_size` is not 0, and the bytes must lie inside the ranges the
	 * pipeline declares (`_push_constant_ranges`). What a draw reads from a
	 * byte no PUSH_CONSTANTS has written since its pipeline was set is
	 * undefined.
	 *
	 * Sent outside a pass it writes the set compute pipeline's block, under
	 * the same rules: after SET_COMPUTE_PIPELINE, and not carried to the
	 * next one set.
	 */
	APREND_COMMAND_PUSH_CONSTANTS         = 14,
	/**
	 * Sets the aprend_compute_pipeline (aprendpipeline.h) the following
	 * dispatches run. Compute is in the same list as rendering, and both
	 * this and DISPATCH are valid only outside a pass. It lasts until the
	 * next BEGIN_RENDERING or PRESENT_TEXTURE: after either, it is set again
	 * before a dispatch.
	 */
	APREND_COMMAND_SET_COMPUTE_PIPELINE   = 15,
	/**
	 * Runs the set compute pipeline over `_group_count_x` by `_y` by `_z`
	 * workgroups, none of them 0. Outside a pass only. The counts are in
	 * workgroups, not threads: a workgroup's size is the shader's own
	 * (`local_size` in GLSL).
	 *
	 * Binding sets are the same slots SET_BINDING_SET fills for draws: at a
	 * dispatch, every slot the compute pipeline declares must hold a set
	 * created from the layout the pipeline declares there. Right before each
	 * dispatch every texture in those sets is moved into the layout its
	 * binding reads it in, and every storage buffer and storage image in
	 * them is ordered after the writes of earlier dispatches and draws, so a
	 * dispatch reads what the one before it wrote. The same mip of the same
	 * layer can't be in a sampled slot and a storage image slot of one
	 * dispatch; two different mips of one texture can, so a dispatch can
	 * read one mip and write the next.
	 */
	APREND_COMMAND_DISPATCH               = 16,
	/**
	 * Draws whose arguments the GPU reads from a storage buffer created
	 * with APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS (aprendbuffers.h):
	 * `_draw_count` draws, the first from the entry at byte `_offset` of the
	 * buffer and each next one `_stride` bytes on. The buffer is `_buffer`,
	 * or the current frame's copy of `_set`; exactly one of the two is
	 * given. A compute shader may have written the entries through a
	 * storage slot earlier in the list, or the CPU with
	 * aprend_storage_buffer_update(). Before the pass the buffer is moved
	 * from the state a storage slot uses it in to the one a draw reads it
	 * in, and back before the next dispatch or pass that has it in a binding
	 * set; it can't be both an argument buffer and in a binding set of one
	 * pass. An entry is a spudgpu_draw_indirect_args (spudgpu.h).
	 * `_draw_count` is not 0, `_offset` and `_stride` are multiples of 4,
	 * `_stride` is at least the size of one entry, and the last entry ends
	 * inside the buffer. They are draws in every other respect: inside a
	 * pass, with a pipeline set and its vertex buffers and binding sets in
	 * place, and not with a mesh shader pipeline.
	 */
	APREND_COMMAND_DRAW_INDIRECT          = 17,
	/**
	 * As DRAW_INDIRECT, for indexed draws: an entry is a
	 * spudgpu_draw_indexed_indirect_args (spudgpu.h).
	 */
	APREND_COMMAND_DRAW_INDEXED_INDIRECT  = 18,
	/**
	 * Runs the set mesh shader pipeline
	 * (aprend_graphics_pipeline_desc::mesh_shader) over `_group_count_x` by
	 * `_y` by `_z` workgroups, none of them 0. It is that pipeline's draw:
	 * inside a pass, with the pipeline set and its binding sets in place.
	 * The draw commands above are refused with a mesh shader pipeline, and
	 * this one with any other.
	 */
	APREND_COMMAND_DISPATCH_MESH          = 19,
	/**
	 * Puts a span of transient memory (aprendtransient.h) in vertex buffer
	 * slot `_slot` for the following draws, as SET_VERTEX_BUFFERS puts a
	 * buffer there, until either puts something else. `_stride` is the size
	 * of one vertex, not 0, and has to be the stride of the pipeline's
	 * binding at that slot at each draw. The span was taken in the frame the
	 * list is compiled in, and its bytes are written before the list is
	 * submitted, in any order with recording and compiling.
	 */
	APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER = 20,
	/**
	 * Makes a span of transient memory the index buffer of the following
	 * indexed draws, as SET_INDEX_BUFFER does a buffer. `_index_size` is the
	 * size of one index, 2 or 4, and the span was allocated with an
	 * alignment of at least that. The rest is as
	 * SET_TRANSIENT_VERTEX_BUFFER.
	 */
	APREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER  = 21,
	/**
	 * Puts an aprend_material (aprendmaterial.h) at set slot `_slot`, as
	 * SET_BINDING_SET puts a set there, and under the same rules: the slot is
	 * one where the pipeline of the following draws or dispatches declares
	 * the material's layout. At the compile the material builds its binding
	 * set if it hasn't, or again if a resource was set since; at the
	 * submission it writes its uniform values into the copy of the frame
	 * (aprendmaterial.h, "When uniform values take effect").
	 */
	APREND_COMMAND_SET_MATERIAL                = 22,
	/**
	 * Begins an aprend_render_pass (aprendpass.h). Sent, it is recorded as
	 * BEGIN_RENDERING over the pass's targets (its framebuffer's attachments
	 * as they are now), SET_SHADER_PIPELINE with its pipeline, a viewport and
	 * a scissor rectangle over the whole of the first target (depth 0 to 1),
	 * and SET_BINDING_SET for each of its fixed sets. It is ended with
	 * END_RENDERING, and is a pass in every other respect.
	 */
	APREND_COMMAND_BEGIN_RENDER_PASS           = 23,
	/**
	 * Sets an aprend_compute_pass (aprendpass.h) for the following
	 * dispatches: recorded as SET_COMPUTE_PIPELINE with its pipeline and
	 * SET_BINDING_SET for each of its fixed sets. Outside a pass, as
	 * SET_COMPUTE_PIPELINE.
	 */
	APREND_COMMAND_SET_COMPUTE_PASS            = 24,
	/**
	 * Draws an aprend_draw_list (aprenddrawlist.h), built in the frame the
	 * list is compiled in. Recorded as, for each of its batches in order:
	 * SET_MATERIAL with the batch's material at `_material_slot` where it
	 * differs from the last batch's, SET_VERTEX_BUFFERS with its mesh's
	 * buffers and SET_INDEX_BUFFER with its index buffer where they differ,
	 * and DRAW_INSTANCED or DRAW_INSTANCED_INDEXED; with one
	 * SET_TRANSIENT_VERTEX_BUFFER of the list's instance bytes first, for a
	 * list that has them. `_material_slot` APREND_DRAW_LIST_NO_MATERIAL binds
	 * no materials. Inside a pass whose pipeline is set, as the draws are.
	 * What it leaves in the vertex and index buffer slots, and the material
	 * slot, stays there after it.
	 */
	APREND_COMMAND_DRAW_LIST                   = 25,
};

/**
 * @brief One command of a command list.
 *
 * Every array a command points at (`_vertex_buffers`, `_viewports`,
 * `_scissor_rects`, `_color_targets`, the `_data` of PUSH_CONSTANTS) is
 * copied into the command list by aprend_send_command(), so the caller's
 * array only has to live for that call. What the entries refer to - buffers,
 * views - must stay alive until the list's submission has completed.
 */
typedef struct APREND_COMMAND {
	/** Which command this is: the member of #_params that is read. */
	APREND_COMMAND_TYPE _type;
	/** The command's parameters, one member for each APREND_COMMAND_TYPE. */
	union {
		/** APREND_COMMAND_SET_VERTEX_BUFFERS. */
		struct {
			aprend_vertex_buffer *_vertex_buffers;
			uint32_t _vertex_buffer_count;
			uint32_t _start_slot;
		} _set_vertex_buffers;
		/** APREND_COMMAND_SET_INDEX_BUFFER. */
		struct {
			aprend_index_buffer _index_buffer;
		} _set_index_buffer;
		/** APREND_COMMAND_SET_SHADER_PIPELINE. */
		struct {
			aprend_graphics_pipeline _pipeline;
		} _set_shader_pipeline;
		/** APREND_COMMAND_DRAW. */
		struct {
			uint32_t _vertex_count;
			uint32_t _start_vertex_location;
		} _draw;
		/** APREND_COMMAND_DRAW_INDEXED. */
		struct {
			uint32_t _index_count;
			uint32_t _start_index_location;
			int32_t _base_vertex_location;
		} _draw_indexed;
		/** APREND_COMMAND_DRAW_INSTANCED. */
		struct {
			uint32_t _vertex_count_per_instance;
			uint32_t _instance_count;
			uint32_t _start_vertex_location;
			uint32_t _start_instance_location;
		} _draw_instanced;
		/** APREND_COMMAND_DRAW_INSTANCED_INDEXED. */
		struct {
			uint32_t _index_count_per_instance;
			uint32_t _instance_count;
			uint32_t _start_index_location;
			int32_t _base_vertex_location;
			uint32_t _start_instance_location;
		} _draw_indexed_instanced;
		/** APREND_COMMAND_SET_VIEWPORTS. */
		struct {
			const SPUDGPU_VIEWPORT *_viewports;
			uint32_t _first_viewport;
			uint32_t _viewport_count;
		} _set_viewports;
		/** APREND_COMMAND_SET_SCISSOR_RECTS. */
		struct {
			const SPUDGPU_SCISSOR_RECT *_scissor_rects;
			uint32_t _first_scissor_rect;
			uint32_t _scissor_rect_count;
		} _set_scissor_rects;
		/** APREND_COMMAND_BEGIN_RENDERING. END_RENDERING reads nothing. */
		struct {
			const aprend_color_target *_color_targets;
			uint32_t _color_target_count;
			aprend_depth_target _depth_target;
			/**
			 * Render area. A `_width` or `_height` of 0 means the full
			 * size of the first color target (or the depth target if
			 * there's none).
			 */
			int32_t _x, _y;
			uint32_t _width, _height;
		} _begin_rendering;
		/** APREND_COMMAND_SET_BINDING_SET. */
		struct {
			uint32_t _slot;
			aprend_binding_set _set;
		} _set_binding_set;
		/** APREND_COMMAND_PRESENT_TEXTURE. */
		struct {
			aprend_texture2d _source;
			aprend_swap_chain _swap_chain;
		} _present_texture;
		/** APREND_COMMAND_PUSH_CONSTANTS. */
		struct {
			const void *_data;
			uint32_t _offset;
			uint32_t _size;
		} _push_constants;
		/** APREND_COMMAND_SET_COMPUTE_PIPELINE. */
		struct {
			aprend_compute_pipeline _pipeline;
		} _set_compute_pipeline;
		/** APREND_COMMAND_DISPATCH. */
		struct {
			uint32_t _group_count_x;
			uint32_t _group_count_y;
			uint32_t _group_count_z;
		} _dispatch;
		/** APREND_COMMAND_DRAW_INDIRECT and APREND_COMMAND_DRAW_INDEXED_INDIRECT. */
		struct {
			aprend_storage_buffer _buffer;
			uint64_t _offset;
			uint32_t _draw_count;
			uint32_t _stride;
			/** A storage buffer set in place of `_buffer`. */
			aprend_storage_buffer_set _set;
		} _draw_indirect;
		/** APREND_COMMAND_DISPATCH_MESH. */
		struct {
			uint32_t _group_count_x;
			uint32_t _group_count_y;
			uint32_t _group_count_z;
		} _dispatch_mesh;
		/** APREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER. */
		struct {
			aprend_transient_span _span;
			uint32_t _slot;
			uint32_t _stride;
		} _set_transient_vertex_buffer;
		/** APREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER. */
		struct {
			aprend_transient_span _span;
			uint32_t _index_size;
		} _set_transient_index_buffer;
		/** APREND_COMMAND_SET_MATERIAL. */
		struct {
			uint32_t _slot;
			aprend_material _material;
		} _set_material;
		/** APREND_COMMAND_BEGIN_RENDER_PASS. */
		struct {
			aprend_render_pass _pass;
		} _begin_render_pass;
		/** APREND_COMMAND_SET_COMPUTE_PASS. */
		struct {
			aprend_compute_pass _pass;
		} _set_compute_pass;
		/** APREND_COMMAND_DRAW_LIST. */
		struct {
			aprend_draw_list _list;
			uint32_t _material_slot;
		} _draw_list;
	} _params;
} APREND_COMMAND;

/**
 * @brief Opaque handle to a command list.
 */
typedef struct aprend_command_list_t *aprend_command_list;

/**
 * @brief Creates an empty command list.
 *
 * @param[in] instance Instance the list records, compiles and submits
 *                     through.
 *
 * @return The list, or NULL if @p instance is NULL or the device refuses
 *         it.
 *
 * @see aprend_command_list_destroy()
 */
aprend_command_list aprend_command_list_create(aprend_instance instance);

/**
 * @brief Destroys a command list.
 *
 * The GPU doesn't have to be finished with what it submitted
 * (aprendcontext.h, "Destroying").
 *
 * @param[in] cmd_list List to destroy. NULL is accepted and does nothing.
 */
void aprend_command_list_destroy(aprend_command_list cmd_list);

/**
 * @brief Empties a command list: every recorded command, a recording error,
 * and the last compile are gone.
 *
 * @param[in] cmd_list List to reset. NULL is accepted and does nothing.
 */
void aprend_command_list_reset(aprend_command_list cmd_list);

/**
 * @brief Records one command at the end of a command list.
 *
 * A malformed command isn't recorded and makes the next
 * aprend_command_list_compile() fail, until aprend_command_list_reset():
 *
 * - a BEGIN_RENDERING with more than APREND_MAX_COLOR_TARGETS color
 *   targets;
 * - a NULL array with a nonzero count in BEGIN_RENDERING,
 *   SET_VERTEX_BUFFERS, SET_VIEWPORTS or SET_SCISSOR_RECTS;
 * - a NULL entry in `_vertex_buffers`;
 * - a SET_VERTEX_BUFFERS that runs past slot
 *   APREND_MAX_VERTEX_BINDINGS - 1;
 * - a PUSH_CONSTANTS with NULL `_data`, a `_size` of 0, or an `_offset` or
 *   `_size` that is not a multiple of 4;
 * - a DISPATCH or DISPATCH_MESH with a group count of 0;
 * - a DRAW_INDIRECT or DRAW_INDEXED_INDIRECT with neither or both of
 *   `_buffer` and `_set`, a `_set` of another instance, a buffer created
 *   without APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS, a `_draw_count`
 *   of 0, an `_offset` or `_stride` that is not a multiple of 4, a
 *   `_stride` smaller than one entry, or entries that run past the end of
 *   the buffer;
 * - a SET_TRANSIENT_VERTEX_BUFFER with a `_slot` at or above
 *   APREND_MAX_VERTEX_BINDINGS or a `_stride` of 0;
 * - a SET_TRANSIENT_INDEX_BUFFER with an `_index_size` that is not 2 or 4;
 * - a SET_MATERIAL with a NULL material, one of another instance, or a
 *   `_slot` at or above APREND_MAX_BINDING_LAYOUTS;
 * - a BEGIN_RENDER_PASS or SET_COMPUTE_PASS with a NULL pass or one of
 *   another instance, or a BEGIN_RENDER_PASS whose framebuffer has lost an
 *   attachment to a failed resize;
 * - a DRAW_LIST with a NULL list, one of another instance, one not built
 *   since it was last changed, or one with a draw that has no material when
 *   `_material_slot` is not APREND_DRAW_LIST_NO_MATERIAL, or a
 *   `_material_slot` at or above APREND_MAX_BINDING_LAYOUTS that isn't
 *   APREND_DRAW_LIST_NO_MATERIAL.
 *
 * BEGIN_RENDER_PASS, SET_COMPUTE_PASS and DRAW_LIST are expanded here into
 * the commands they stand for, read from the pass or list as it is now (see
 * each). What those commands are is checked as if they had been sent one by
 * one.
 *
 * @param[in] cmd_list List to record into. NULL is accepted and does
 *                     nothing.
 * @param[in] cmd      The command. Copied, with every array it points at.
 */
void aprend_send_command(
    aprend_command_list cmd_list,
    APREND_COMMAND cmd);

/**
 * @brief Compiles a command list.
 *
 * Translates every command recorded since the last
 * aprend_command_list_reset() into the underlying spudgpu_command_list and
 * closes it for submission (spudgpu_begin_command_list() / ... /
 * spudgpu_end_command_list()). Only compiles; submit with
 * aprend_command_list_submit(). Compiling has no effect on any texture's
 * tracked layout, so a list that's compiled but never submitted (or fails
 * to compile) leaves nothing out of sync.
 *
 * @param[in] cmd_list List to compile.
 *
 * @return false, without a submittable result, if a malformed command was
 *         sent or the pass scope is violated: a draw outside any pass, a
 *         nested BEGIN_RENDERING, END_RENDERING with no open
 *         BEGIN_RENDERING, or a BEGIN_RENDERING left open at the end of the
 *         list. Also fails on:
 *         - a SET_SHADER_PIPELINE outside a pass or with a NULL pipeline;
 *         - a draw in a pass that hasn't set a pipeline;
 *         - an indexed draw with no index buffer set;
 *         - a SET_INDEX_BUFFER with a NULL buffer;
 *         - a pass with an indirect draw whose argument buffer is also in a
 *           binding set the pass's draws read;
 *         - a draw command with a mesh shader pipeline, or a DISPATCH_MESH
 *           with any other;
 *         - a draw whose pipeline reads a vertex buffer slot that is empty
 *           or holds a buffer of another stride;
 *         - a PUSH_CONSTANTS with no pipeline set in its pass (or, outside
 *           a pass, no compute pipeline set) or with bytes outside the
 *           pipeline's push constant ranges;
 *         - a SET_COMPUTE_PIPELINE or DISPATCH inside a pass, a
 *           SET_COMPUTE_PIPELINE with a NULL pipeline, a DISPATCH with no
 *           compute pipeline set, a DISPATCH whose pipeline declares a set
 *           slot that holds no set or a set of another layout, or whose
 *           sets hold one texture in a sampled slot and a storage image
 *           slot;
 *         - a SET_BINDING_SET with a NULL set or a `_slot` of
 *           APREND_MAX_BINDING_LAYOUTS or more;
 *         - a draw whose pipeline declares a set slot that holds no set or
 *           a set of another layout;
 *         - a pass whose binding sets break the texture rule of
 *           APREND_COMMAND_SET_BINDING_SET;
 *         - a draw or dispatch whose binding sets hold a texture view of
 *           another shape than the one the pipeline's shader declares at its
 *           binding (a 2D view for a `samplerCube`, a 2D array view for a
 *           `sampler2D`), or a stencil view in a binding the shader doesn't
 *           read as unsigned integers, or a depth view in one it doesn't
 *           read as floats (aprendshader.h);
 *         - a SET_MATERIAL whose material is not complete
 *           (aprend_material_is_complete()) or whose binding set can't be
 *           built.
 *
 *         Compiling builds the binding set of every material a SET_MATERIAL
 *         names, when it has none or a resource was set since its last
 *         build (aprendmaterial.h). That is the one thing it changes outside
 *         the list, and it is done whether or not the rest of the compile
 *         succeeds.
 */
bool aprend_command_list_compile(aprend_command_list cmd_list);

/**
 * @brief Submits the last successful compile of a command list and commits
 * the image layouts it leaves its textures in.
 *
 * The submission counts towards the current frame, which
 * aprend_instance_next_frame() (aprendcontext.h) waits for.
 *
 * @p queue is the instance's one queue (aprendcontext.h): the queue of the
 * instance's first submission, and every one after it. Another is refused.
 * Submissions run in submit order on it, which is what the tracked layouts
 * and the frame pacing assume.
 *
 * Whatever was written to the vertex, index and storage buffers the list
 * uses, and has not reached the device yet, is copied there first in the
 * same submission (aprendbuffers.h). So are the uniform blocks of every
 * material the list binds that changed since they were last copied, through
 * the current frame's transient memory (aprendmaterial.h, "When uniform
 * values take effect"). Those buffers must still exist:
 * destroying one and then submitting a list compiled against it is a use
 * after free.
 *
 * A compile is submitted once. To run the same recorded commands again,
 * compile the list again and submit that: a SpudGPU command list is
 * submitted once for each time it is recorded (spudgpu.h), and a compile is
 * one recording. The recorded commands stay until
 * aprend_command_list_reset(), so nothing has to be sent again. The caller
 * decides when to wait for the GPU.
 *
 * A list with a PRESENT_TEXTURE must be submitted on its swap chain's
 * queue, while the back buffer it was compiled against is still the
 * acquired one; follow it with aprend_swap_chain_present().
 *
 * @param[in] cmd_list List to submit.
 * @param[in] queue    Queue to submit on: the instance's one queue.
 *
 * @return false without submitting if the list hasn't compiled
 *         successfully since it was last submitted, if @p queue is not the queue the instance already
 *         submits on, if a texture it uses has changed layout since it was
 *         compiled (another list touching the same texture was submitted in
 *         between), or if it binds a set holding a buffer set and
 *         aprend_instance_next_frame() has moved the frame on since it was
 *         compiled. In the last two cases, recompile it and submit again.
 *         false too if transient memory can't be taken for a material's
 *         block.
 */
bool aprend_command_list_submit(aprend_command_list cmd_list, spudgpu_command_queue queue);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_COMMANDS_H
