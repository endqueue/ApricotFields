
#ifndef RBTREND_COMMANDS_H
#define RBTREND_COMMANDS_H

#include "rbtrendcontext.h"
#include "rbtrendimages.h"
#include "rbtrendtransient.h"
#include "rbtrendtimer.h"

/**
 * @file rbtrendcommands.h
 * @brief The command list: commands are recorded into it, compiled into a
 * Carrottus GPU command list, and submitted.
 *
 * @par Threads
 * The rule is in rbtrendcontext.h ("Threads"). What is particular to a
 * command list:
 *
 * - rbtrend_command_list_send() and rbtrend_command_list_reset() write the list and
 *   nothing else, so different lists may be recorded at once, one caller to
 *   a list, overlapping other calls on the instance (both are marked). What
 *   the commands name must not be destroyed meanwhile.
 * - rbtrend_command_list_create(), rbtrend_command_list_compile(),
 *   rbtrend_command_list_submit() and rbtrend_command_list_destroy() are not
 *   part of that. Every list of an instance compiles into the instance's
 *   command memory for the frame (rbtrendcontext.h); compiling reads the
 *   layouts a submission writes; and a submission takes the next place in
 *   the instance's order and carries the instance's batch of updates. They
 *   are made by one caller at a time across the whole instance, not only
 *   one list. rbtrend_command_list_destroy() is among them, unlike most
 *   destroy calls: it gives back to the instance's render target pool what
 *   an unsubmitted compile took.
 */

#if __cplusplus
extern "C" {
#endif // __cplusplus

/** Opaque handle to a buffer (rbtrendbuffers.h). */
typedef struct rbtrend_buffer_t *rbtrend_buffer;
/** Opaque handle to a graphics pipeline (rbtrendpipeline.h). */
typedef struct rbtrend_graphics_pipeline_t *rbtrend_graphics_pipeline;
/** Opaque handle to a compute pipeline (rbtrendpipeline.h). */
typedef struct rbtrend_compute_pipeline_t *rbtrend_compute_pipeline;
/** Opaque handle to a binding set (rbtrendpipeline.h). */
typedef struct rbtrend_binding_set_t *rbtrend_binding_set;
/** Opaque handle to a swap chain (rbtrendswapchain.h). */
typedef struct rbtrend_swap_chain_t *rbtrend_swap_chain;
/** Opaque handle to a material (rbtrendmaterial.h). */
typedef struct rbtrend_material_t *rbtrend_material;
/** Opaque handle to a render pass (rbtrendpass.h). */
typedef struct rbtrend_render_pass_t *rbtrend_render_pass;
/** Opaque handle to a compute pass (rbtrendpass.h). */
typedef struct rbtrend_compute_pass_t *rbtrend_compute_pass;
/** Opaque handle to a draw list (rbtrenddrawlist.h). */
typedef struct rbtrend_draw_list_t *rbtrend_draw_list;
/** An environment for lit surfaces (rbtrendlit.h), filled by
 * RBTREND_COMMAND_LIT_ENVIRONMENT. */
typedef struct rbtrend_lit_environment_t *rbtrend_lit_environment;
/** A lit scene (rbtrendlit.h), read by RBTREND_COMMAND_LIT_AMBIENT_OCCLUSION. */
typedef struct rbtrend_lit_scene_t *rbtrend_lit_scene;
/** An adapting exposure (rbtrendpost.h), for RBTREND_COMMAND_AUTO_EXPOSURE and
 * RBTREND_COMMAND_TONE_MAP. */
typedef struct rbtrend_exposure_t *rbtrend_exposure;
/** A bottom-level acceleration structure (rbtrendraytracing.h), built by
 * RBTREND_COMMAND_BUILD_BLAS. */
typedef struct rbtrend_blas_t *rbtrend_blas;
/** A top-level acceleration structure (rbtrendraytracing.h), built by
 * RBTREND_COMMAND_BUILD_TLAS and traced by RBTREND_COMMAND_LIT_SHADOW_MASK and
 * RBTREND_COMMAND_LIT_PATH_TRACE. */
typedef struct rbtrend_tlas_t *rbtrend_tlas;

/**
 * @brief What RBTREND_COMMAND_BUILD_BLAS does to its BLAS
 * (rbtrendraytracing.h).
 */
typedef enum RBTREND_BUILD_MODE RBT_ENUM_BASE {
	/** Not a mode: a zeroed command has not chosen, and is refused. */
	RBTREND_BUILD_MODE_NONE    = 0,
	/**
	 * Builds the BLAS from its geometries as they are now: the first build,
	 * a deforming BLAS's quality brought back after refits, or a static one
	 * built again. A compacted BLAS gets full-size storage again first, an
	 * allocation at the compile that records it.
	 */
	RBTREND_BUILD_MODE_BUILD   = 1,
	/**
	 * Refits a deforming BLAS (`_deforms`) to its geometries' new
	 * positions, in place: the same triangles, moved. The BLAS has been
	 * built, by a build that has completed or one sent earlier in the same
	 * list.
	 */
	RBTREND_BUILD_MODE_REFIT   = 2,
	/**
	 * Compacts a static BLAS whose compacted size is known
	 * (rbtrend_blas_get_info(), `_compacted_bytes`): a structure of that
	 * size is made at the compile that records it, an allocation, the BLAS
	 * copied into it, and the full-size one released once that frame has
	 * completed. Its slot is unchanged; every TLAS built after it reads the
	 * compacted one.
	 */
	RBTREND_BUILD_MODE_COMPACT = 3
} RBTREND_BUILD_MODE;

/**
 * @brief How RBTREND_COMMAND_PRESENT_TEXTURE and RBTREND_COMMAND_DRAW_TEXTURE
 * fit a source into a target of another size.
 *
 * Every mode but PRESENT_TEXTURE's NONE is a full-target draw: the target is
 * rendered whole, each texel from the source through a linear or nearest
 * sampler (the command's `_filter`) or the command's `_border_color` where
 * no source covers it. The draw writes the target's own format, so a source
 * of another (a float picture onto an 8-bit target, sRGB encoded or not)
 * is taken.
 */
typedef uint32_t RBTREND_TEXTURE_SCALING;
enum {
	/**
	 * No scaling. PRESENT_TEXTURE: a copy, not a draw: the source in the back
	 * buffers' format and at least their size, a larger one cropped to its
	 * top-left region. DRAW_TEXTURE: drawn texel for texel from the target's
	 * top-left corner, the border color past the source, a larger source
	 * cropped.
	 */
	RBTREND_TEXTURE_SCALING_NONE       = 0,
	/** The source filtered to fill the target, its aspect not kept. */
	RBTREND_TEXTURE_SCALING_STRETCH    = 1,
	/**
	 * The source filtered to the largest size that fits inside the target
	 * with its aspect kept, centred; the border color on the two sides it
	 * doesn't reach.
	 */
	RBTREND_TEXTURE_SCALING_ASPECT_FIT = 2,
};

/**
 * @brief A rectangle of a view's mip, in texels from its top-left corner:
 * the part of a texture RBTREND_COMMAND_DRAW_TEXTURE reads or writes. A
 * width and height both 0 are the whole mip.
 */
/* Frozen: a value, in a command's params. It never changes; a new need gets a new type. */
typedef struct RBTREND_TEXEL_RECT {
	/** The left edge. */
	uint32_t x;
	/** The top edge. */
	uint32_t y;
	/** The width; 0, with #height 0, for the whole mip. */
	uint32_t width;
	/** The height; 0, with #width 0, for the whole mip. */
	uint32_t height;
} RBTREND_TEXEL_RECT;

/**
 * @brief How color is written out, by RBTREND_COMMAND_TONE_MAP and by lit
 * surfaces (rbtrendlit.h).
 */
typedef uint32_t RBTREND_COLOR_ENCODING;
enum {
	/** Linear: for a float target, an `_SRGB` target that encodes as it is
	 * written, or a pass that tone maps after. */
	RBTREND_COLOR_ENCODING_LINEAR = 0,
	/** Clamped to 0 to 1 and encoded as sRGB: for an 8-bit UNORM target
	 * whose values are shown as they are. */
	RBTREND_COLOR_ENCODING_SRGB = 1,
};

/**
 * @brief The curve RBTREND_COMMAND_TONE_MAP brings linear radiance, of any
 * brightness, into 0 to 1 with.
 */
typedef uint32_t RBTREND_TONE_CURVE;
enum {
	/** None: clamped at 1. What is brighter than white is lost. */
	RBTREND_TONE_CURVE_CLAMP = 0,
	/**
	 * Stephen Hill's fit of ACES's reference rendering and sRGB output
	 * transforms: the filmic look most engines default to (Unreal's
	 * is ACES-based). Shifts saturated colors as they brighten.
	 */
	RBTREND_TONE_CURVE_ACES = 1,
	/**
	 * Khronos PBR Neutral (2024): base colors under even light come out as
	 * they went in, and only highlights are compressed. For product and
	 * architectural pictures, where a material's color must read true.
	 */
	RBTREND_TONE_CURVE_KHRONOS_PBR_NEUTRAL = 2,
	/**
	 * AgX (Troy Sobotka's; Blender's default since 4.0), in Benjamin
	 * Wrensch's minimal fit: bright, saturated colors go towards white as
	 * film does, with no hue skew, over 16.5 stops.
	 */
	RBTREND_TONE_CURVE_AGX = 3,
};

/**
 * @brief The analytic sky RBTREND_COMMAND_LIT_SKY fills an environment from.
 */
typedef uint32_t RBTREND_LIT_SKY_MODEL;
enum {
	/**
	 * Preetham, Shirley and Smits' ("A Practical Analytic Model for
	 * Daylight", 1999), Hazel's sky: Perez's distribution of luminance and
	 * chromaticity. Turbidity 1.7 to 10.
	 */
	RBTREND_LIT_SKY_MODEL_PREETHAM     = 0,
	/**
	 * Hosek and Wilkie's ("An Analytic Model for Full Spectral Sky-Dome
	 * Radiance", 2012), in its native RGB: fitted to brute-force
	 * simulations of the atmosphere, with a truer horizon and glow round
	 * the sun than Preetham's, and the ground's albedo lighting the sky
	 * from below. Turbidity 1 to 10, the ground's albedo 0 to 1.
	 */
	RBTREND_LIT_SKY_MODEL_HOSEK_WILKIE = 1,
};

/**
 * @brief Upper bound on color targets in one BEGIN_RENDERING pass.
 *
 * RBTREND_COLOR_TARGET and RBTREND_DEPTH_TARGET (rbtrendimages.h) describe how
 * one pass uses each attachment view. Zero-initialized ops mean LOAD and
 * STORE.
 */
#define RBTREND_MAX_COLOR_TARGETS CRGPU_MAX_COLOR_ATTACHMENTS

/**
 * @brief The largest `_sigma` of RBTREND_COMMAND_BLUR, in texels: a blur
 * reaches three times its sigma each way, so at most 96 texels.
 *
 * A wider blur is the source drawn down to a smaller texture, blurred there
 * and drawn up again (RBTREND_COMMAND_DRAW_TEXTURE), at a fraction of the
 * cost.
 */
#define RBTREND_BLUR_MAX_SIGMA 32.0f

/**
 * @brief Which command an RBTREND_COMMAND is, and so which member of its
 * `_params` is read.
 *
 * The names below are short for the values: SET_VERTEX_BUFFERS is
 * RBTREND_COMMAND_SET_VERTEX_BUFFERS.
 */
typedef uint32_t RBTREND_COMMAND_TYPE;

enum {
	/** No command. */
	RBTREND_COMMAND_NONE                   = 0,
	/**
	 * Puts `_vertex_buffers[i]` at vertex buffer slot `_start_slot` + i.
	 * Slot N is what a pipeline's `_vertex_bindings[N]` reads
	 * (rbtrendpipeline.h), per vertex or per instance as that binding says,
	 * at the binding's `stride`: a buffer is bytes, and the pipeline of
	 * each draw says what they are. At each draw, every slot the bound
	 * pipeline reads must hold a buffer or a span, and a draw whose
	 * vertices or instances are known (DRAW, DRAW_INSTANCED, and the
	 * instances of DRAW_INSTANCED_INDEXED) must read inside it at that
	 * stride. An indexed draw's vertices, and an indirect draw's, are read
	 * where the indices or arguments say, which the compile can't see.
	 */
	RBTREND_COMMAND_SET_VERTEX_BUFFERS     = 1,
	/**
	 * The index buffer of the following indexed draws (DRAW_INDEXED,
	 * DRAW_INSTANCED_INDEXED, DRAW_INDEXED_INDIRECT), until another is set.
	 * An indexed draw with none set fails the compile.
	 */
	RBTREND_COMMAND_SET_INDEX_BUFFER       = 2,
	/**
	 * The graphics pipeline of the following draws. Inside a pass only, and
	 * it lasts until the pass ends: each pass sets its own pipeline before
	 * it draws.
	 */
	RBTREND_COMMAND_SET_GRAPHICS_PIPELINE    = 3,
	/**
	 * Draws `_vertex_count` vertices, the first at `_start_vertex_location`
	 * of the vertex buffers set.
	 */
	RBTREND_COMMAND_DRAW                   = 4,
	/**
	 * Draws `_index_count` indices of the index buffer set, the first at
	 * `_start_index_location`, each with `_base_vertex_location` added to it
	 * before it picks a vertex.
	 */
	RBTREND_COMMAND_DRAW_INDEXED           = 5,
	/**
	 * As DRAW, `_instance_count` times: `_vertex_count_per_instance`
	 * vertices each, the first instance being `_start_instance_location`.
	 */
	RBTREND_COMMAND_DRAW_INSTANCED         = 6,
	/**
	 * As DRAW_INDEXED, `_instance_count` times: `_index_count_per_instance`
	 * indices each, the first instance being `_start_instance_location`.
	 */
	RBTREND_COMMAND_DRAW_INSTANCED_INDEXED = 7,
	/**
	 * Sets `_viewport_count` viewports, the first of them viewport
	 * `_first_viewport`.
	 */
	RBTREND_COMMAND_SET_VIEWPORTS          = 8,
	/**
	 * Sets `_scissor_rect_count` scissor rectangles, the first of them
	 * rectangle `_first_scissor_rect`.
	 */
	RBTREND_COMMAND_SET_SCISSOR_RECTS      = 9,
	/**
	 * Begins a pass. Draws are only valid inside a pass; BEGIN_RENDERING
	 * can't be nested and must be closed by END_RENDERING before the list is
	 * compiled. rbtrend_command_list_compile() fails on any violation.
	 *
	 * The targets of one pass are all 2D views, each one layer, or all 2D
	 * array views of the same number of layers, which makes it a layered
	 * pass: every layer is rendered to, cleared and stored together, and the
	 * pass's vertex or mesh shader writes the layer each primitive is drawn
	 * to (RBTREND_TEXTURE_VIEW_DESC, rbtrendimages.h). Mixing the two shapes
	 * or two layer counts in one pass fails the compile.
	 */
	RBTREND_COMMAND_BEGIN_RENDERING        = 10,
	/** Ends the pass BEGIN_RENDERING began. */
	RBTREND_COMMAND_END_RENDERING          = 11,
	/**
	 * Puts an rbtrend_binding_set (rbtrendpipeline.h) at set slot `_slot` for
	 * the following draws, until another is put there. It stays through
	 * SET_SHADER_PIPELINE and from one pass to the next, so it may be sent
	 * before or after the pipeline, inside a pass or outside. At each draw,
	 * every slot the bound pipeline declares must hold a set created from
	 * the layout the pipeline declares there, or a layout of the same
	 * bindings (rbtrendpipeline.h, "Layouts of the same bindings").
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
	RBTREND_COMMAND_SET_BINDING_SET        = 12,
	/**
	 * Puts `_source`, mip 0 of layer 0, into `_swap_chain`'s acquired back
	 * buffer (rbtrendswapchain.h) and leaves it ready to present. Outside any
	 * pass; at most one per list. The back buffer must have been acquired
	 * before compiling. `_scaling` (RBTREND_TEXTURE_SCALING) says how:
	 *
	 * - RBTREND_TEXTURE_SCALING_NONE (0): a copy. `_source` is in the back
	 *   buffers' format (the copy converts nothing on D3D12 and Metal) and
	 *   at least their size (rbtrend_swap_chain_get_back_buffer_size()):
	 *   every texel shown is one of the source's, and a larger source is
	 *   cropped to its top-left region. The compile fails otherwise.
	 *   `_filter` and `_border_color` are not read.
	 * - STRETCH and ASPECT_FIT: a full-target draw of the source into the
	 *   back buffer, as RBTREND_COMMAND_DRAW_TEXTURE draws: of any size and
	 *   any format the source has, the back buffers' written. `_source` has
	 *   RBTREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE; it is sampled through a
	 *   view the texture keeps (made at the first such present, released
	 *   with it or at its resize), and moved to SHADER_READ_ONLY. `_filter`
	 *   is CRGPU_FILTER_LINEAR or _NEAREST; ASPECT_FIT's `_border_color`
	 *   is finite. The first such present onto a back buffer format makes
	 *   its pipeline.
	 *
	 * Rend's swap chains are made with Carrottus GPU's one-to-one scaling, so
	 * what reaches the back buffer is what is shown.
	 */
	RBTREND_COMMAND_PRESENT_TEXTURE        = 13,
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
	RBTREND_COMMAND_PUSH_CONSTANTS         = 14,
	/**
	 * Sets the rbtrend_compute_pipeline (rbtrendpipeline.h) the following
	 * dispatches run. Compute is in the same list as rendering, and both
	 * this and DISPATCH are valid only outside a pass. It lasts until the
	 * next BEGIN_RENDERING or PRESENT_TEXTURE: after either, it is set again
	 * before a dispatch.
	 */
	RBTREND_COMMAND_SET_COMPUTE_PIPELINE   = 15,
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
	 * dispatch reads what the one before it wrote, and a write after earlier
	 * reads. What a shader writes is what it declares without `readonly`
	 * (GLSL) and its entry point uses; two dispatches or passes that only
	 * read a storage resource, with no write between them, get no barrier
	 * between them. The first use of one in a list always gets one, for
	 * what a submission before it may have written. The same mip of the same
	 * layer can't be in a sampled slot and a storage image slot of one
	 * dispatch; two different mips of one texture can, so a dispatch can
	 * read one mip and write the next.
	 */
	RBTREND_COMMAND_DISPATCH               = 16,
	/**
	 * Draws whose arguments the GPU reads from `_buffer`, a buffer created
	 * with RBTREND_BUFFER_USAGE_INDIRECT_ARGUMENTS (rbtrendbuffers.h):
	 * `_draw_count` draws, the first from the entry at byte `_offset` of the
	 * buffer and each next one `_stride` bytes on; of a buffer of per-frame
	 * copies, the copy of the frame the list is compiled in. A compute
	 * shader may have written the entries through a storage slot earlier in
	 * the list, or the CPU with rbtrend_buffer_write(). Before the pass a
	 * buffer that is also RBTREND_BUFFER_USAGE_STORAGE is moved from the
	 * state a storage slot uses it in to the one a draw reads it in, and back before the next dispatch or pass that has it in a binding
	 * set; it can't be both an argument buffer and in a binding set of one
	 * pass. An entry is a crgpu_draw_indirect_args (crgpu.h).
	 * `_draw_count` is not 0, `_offset` and `_stride` are multiples of 4,
	 * `_stride` is at least the size of one entry, and the last entry ends
	 * inside the buffer. They are draws in every other respect: inside a
	 * pass, with a pipeline set and its vertex buffers and binding sets in
	 * place, and not with a mesh shader pipeline.
	 */
	RBTREND_COMMAND_DRAW_INDIRECT          = 17,
	/**
	 * As DRAW_INDIRECT, for indexed draws: an entry is a
	 * crgpu_draw_indexed_indirect_args (crgpu.h).
	 */
	RBTREND_COMMAND_DRAW_INDEXED_INDIRECT  = 18,
	/**
	 * Runs the set mesh shader pipeline
	 * (RBTREND_GRAPHICS_PIPELINE_DESC::mesh_shader) over `_group_count_x` by
	 * `_y` by `_z` workgroups, none of them 0. It is that pipeline's draw:
	 * inside a pass, with the pipeline set and its binding sets in place.
	 * The draw commands above are refused with a mesh shader pipeline, and
	 * this one with any other.
	 */
	RBTREND_COMMAND_DISPATCH_MESH          = 19,
	/**
	 * Puts a span of transient memory (rbtrendtransient.h) in vertex buffer
	 * slot `_slot` for the following draws, as SET_VERTEX_BUFFERS puts a
	 * buffer there, until either puts something else, and read as a buffer
	 * is: at the stride of the pipeline's binding at that slot, and inside
	 * the span's `size`. The span was taken in the frame the
	 * list is compiled in, and its bytes are written before the list is
	 * submitted, in any order with recording and compiling.
	 */
	RBTREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER = 20,
	/**
	 * Makes a span of transient memory the index buffer of the following
	 * indexed draws, as SET_INDEX_BUFFER does a buffer. `_index_size` is the
	 * size of one index, 2 or 4, and the span was allocated with an
	 * alignment of at least that. The rest is as
	 * SET_TRANSIENT_VERTEX_BUFFER.
	 */
	RBTREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER  = 21,
	/**
	 * Puts an rbtrend_material (rbtrendmaterial.h) at set slot `_slot`, as
	 * SET_BINDING_SET puts a set there, and under the same rules: the slot is
	 * one where the pipeline of the following draws or dispatches declares
	 * the material's layout. At the compile the material builds its binding
	 * set if it hasn't, or again if a resource was set since; at the
	 * submission it writes its uniform values into the copy of the frame
	 * (rbtrendmaterial.h, "When uniform values take effect").
	 */
	RBTREND_COMMAND_SET_MATERIAL                = 22,
	/**
	 * Begins an rbtrend_render_pass (rbtrendpass.h). Sent, it is recorded as
	 * BEGIN_RENDERING over the pass's targets (its framebuffer's attachments
	 * as they are now), SET_SHADER_PIPELINE with its pipeline, a viewport and
	 * a scissor rectangle over the whole of the first target (depth 0 to 1),
	 * and SET_BINDING_SET for each of its fixed sets. It is ended with
	 * END_RENDERING, and is a pass in every other respect.
	 */
	RBTREND_COMMAND_BEGIN_RENDER_PASS           = 23,
	/**
	 * Sets an rbtrend_compute_pass (rbtrendpass.h) for the following
	 * dispatches: recorded as SET_COMPUTE_PIPELINE with its pipeline and
	 * SET_BINDING_SET for each of its fixed sets. Outside a pass, as
	 * SET_COMPUTE_PIPELINE.
	 */
	RBTREND_COMMAND_SET_COMPUTE_PASS            = 24,
	/**
	 * Draws an rbtrend_draw_list (rbtrenddrawlist.h), built in the frame the
	 * list is compiled in. Recorded as, for each of its batches in order:
	 * SET_MATERIAL with the batch's material at `_material_slot` where it
	 * differs from the last batch's, SET_VERTEX_BUFFERS with its mesh's
	 * buffers and SET_INDEX_BUFFER with its index buffer where they differ,
	 * and DRAW_INSTANCED or DRAW_INSTANCED_INDEXED; with one
	 * SET_TRANSIENT_VERTEX_BUFFER of the list's instance bytes first, for a
	 * list that has them. `_material_slot` RBTREND_DRAW_LIST_NO_MATERIAL binds
	 * no materials. Inside a pass whose pipeline is set, as the draws are.
	 * What it leaves in the vertex and index buffer slots, and the material
	 * slot, stays there after it.
	 */
	RBTREND_COMMAND_DRAW_LIST                   = 25,
	/**
	 * Opens a labelled region of the list, for a capture or profiling tool
	 * to show around the commands until the matching END_LABEL. It changes
	 * nothing the GPU does. The text is copied. Regions nest, and one
	 * opened inside a pass closes inside the same pass, one opened outside
	 * every pass closes outside every pass; every region is closed by the
	 * end of the list. rbtrend_command_list_compile() fails on any
	 * violation.
	 */
	RBTREND_COMMAND_BEGIN_LABEL                 = 26,
	/** Closes the region the last BEGIN_LABEL opened. Reads nothing. */
	RBTREND_COMMAND_END_LABEL                   = 27,
	/**
	 * Fills every level of `_texture` below `_base_mip`, in every array
	 * layer, from the level above, as rbtrend_texture2d_generate_mips()
	 * (rbtrendimages.h) does and under its rules for the texture's usage, its
	 * format and `_filter`: a pass per level, sampling the one above. It is
	 * that call's form for a texture whose base level is written earlier in
	 * the same list, by a pass or a dispatch, or by anything submitted
	 * before it: a scene rendered and then reduced, in one list.
	 *
	 * Outside a pass. Every level it fills is left in
	 * CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, and the base level too, ready
	 * for a later pass to sample; layouts are tracked as for a pass. It
	 * binds a pipeline and sets of its own, so a compute pipeline is set
	 * again after it before a dispatch, as after a pass; the binding set
	 * slots are not touched.
	 *
	 * The views and sets it renders through are kept by the texture from
	 * the first generation of the same base level and filter, by this
	 * command or the call, and released with it. A texture's first
	 * generation in a format also makes Rend's pipeline for that format.
	 */
	RBTREND_COMMAND_GENERATE_MIPS               = 28,
	/**
	 * Blooms `_source` onto `_target`: a soft glow spread out from the
	 * source and added to what the target holds, color and alpha alike.
	 * Outside a pass.
	 *
	 * What glows is the caller's. Every texel of the source glows unless
	 * `_threshold` is set: a caller that wants only some things to glow
	 * draws those alone into a source of their own and blooms that with a
	 * threshold of 0, and one blooming a whole HDR picture sets the
	 * brightness that should start to glow. Rend never decides what is
	 * worth glowing.
	 *
	 * It records a chain of passes at falling sizes through a
	 * CRGPU_FORMAT_R16G16B16A16_FLOAT texture of `_levels` levels, taken
	 * from the instance's pool (rbtrendcontext.h, "Textures a command uses
	 * in between"), its level 0 half the size of the source's first mip, so
	 * that sums don't clip or band:
	 *
	 * 1. the source, read through 13 taps and weighted by `_threshold` and
	 *    `_knee`, into level 0;
	 * 2. each further level from the one above, through the same 13 taps;
	 * 3. each level read back up through a tent of taps `_radius` texels
	 *    apart and added into the level above, from the smallest up, so
	 *    level 0 ends holding the sum of them all;
	 * 4. level 0 spread over the target through the same tent, times
	 *    `_intensity`, and added to what the target holds.
	 *
	 * With a premultiplied source the glow carries the source's coverage,
	 * so a glow over a clear target is seen once the target is composited
	 * over something else.
	 *
	 * `_mask`, if not NULL, weights what glows texel by texel: each tap of
	 * step 1 is the source times the mask's red at the same place in
	 * [0, 1], so a masked texel adds nothing to the glow, bright or not,
	 * and the rest of the source glows as without a mask. It picks what
	 * glows without a source of its own.
	 *
	 * `_lens_dirt`, if not NULL, is laid over the whole target (stretched
	 * to it, read at the same place in [0, 1]) and brightens the glow in
	 * step 4 where it falls: what is added is the glow times
	 * (1 + dirt * `_lens_dirt_intensity`) in color; alpha is the glow's
	 * alone. Dirt shows only where there is glow, as smudges on a lens
	 * light up only where light crosses them.
	 *
	 * The mask and the dirt are each a SHADER_RESOURCE view, 2D, of color,
	 * in a format a shader reads as floats, of any size, written by
	 * anything submitted before or earlier in the list; the first mip of
	 * each is read through a linear filter, and moved into
	 * CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY. The dirt shares no mip of a
	 * layer with the target. `_lens_dirt_intensity` is not negative and is
	 * a number, and is not read without `_lens_dirt`.
	 *
	 * `_source` is a SHADER_RESOURCE view, 2D, of color, written by
	 * anything submitted before or earlier in the list; its first mip is
	 * read, and moved into CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY. `_target`
	 * is a RENDER_TARGET view, 2D, in a format a bloom renders onto (the
	 * formats rbtrend_texture2d_generate_mips() takes), of any size; it is
	 * left in CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, as a pass's
	 * target is. The two share no mip of a layer.
	 *
	 * `_levels` is from 1 to as many as halving the source's size twice
	 * over allows (a level's sides stop at 1 texel); more is a wider glow
	 * and two more passes a level. `_threshold` is the brightness (the
	 * largest of red, green and blue) a source texel needs before it glows,
	 * and `_knee` how far below it the glow fades in rather than starting
	 * at once; 0 and 0 make every texel glow. `_radius` is how far apart, in
	 * texels of each level, the taps reading a level back up are: 1 is the
	 * usual, more is a wider and fainter glow. `_intensity` scales what is
	 * added to the target. None is negative or not a number, and `_radius`
	 * is above 0.
	 *
	 * It binds pipelines and sets of its own, so a compute pipeline is set
	 * again after it before a dispatch, as after a pass; the binding set
	 * slots are not touched. The first bloom of an instance makes its bloom
	 * pipelines, the first onto a target format the composite for it, the
	 * first of a size a pooled texture, and the first that reads a view the
	 * set that reads it, which the view keeps.
	 */
	RBTREND_COMMAND_BLOOM                       = 29,
	/**
	 * Tone maps `_source` onto `_target`, outside a pass: each texel times
	 * `_exposure`, through `_curve`, then written as `_encoding` says,
	 * replacing what the target held, alpha kept as it is. The source is a
	 * SHADER_RESOURCE view of one mip of a 2D color texture (a float target
	 * that lit surfaces were drawn into, with a bloom over it, say); the
	 * target a RENDER_TARGET view of a 2D texture of a color format, of any
	 * size: it is read at the same place in [0, 1], filtered. `_exposure` is
	 * not negative and is a number.
	 *
	 * With `_auto_exposure` (rbtrendpost.h) each texel is also multiplied by
	 * the exposure it last adapted to (RBTREND_COMMAND_AUTO_EXPOSURE, earlier
	 * in this list or in one submitted before); `_exposure` is then a
	 * compensation on top of it.
	 *
	 * It binds a pipeline and a set of its own, as BLOOM does, and leaves
	 * the binding set slots alone. The first onto a target format makes the
	 * pipeline for it.
	 */
	RBTREND_COMMAND_TONE_MAP                    = 30,
	/**
	 * Fills `_environment` (rbtrendlit.h) from `_source`, outside a pass: the
	 * picture copied into the environment's specular chain's first mip, its
	 * other mips prefiltered for rougher and rougher surfaces (GGX, importance
	 * sampled from the picture's own mips), and its irradiance worked out.
	 * The source is a SHADER_RESOURCE view, of color, of a CUBE (six faces,
	 * any size) or of a 2D equirectangular picture (longitude across, the
	 * north pole at the top row), linear. It is read, not kept: it may be
	 * destroyed once the list has been submitted.
	 *
	 * It binds pipelines and sets of its own, as BLOOM does. Its cost is a
	 * few hundred samples a texel of the chain, so it is for a sky that
	 * changes now and then, not every frame.
	 */
	RBTREND_COMMAND_LIT_ENVIRONMENT             = 31,
	/**
	 * Fills `_environment` (rbtrendlit.h) from a daylight sky, outside a
	 * pass: the analytic model `_model` names (RBTREND_LIT_SKY_MODEL:
	 * Preetham's, Hazel's sky too, or Hosek and Wilkie's) at `_turbidity`
	 * (2, clear, to 10, hazy) with the sun towards `_sun_direction` in the
	 * environment's space (+Y up), scaled so the zenith's luminance is
	 * `_intensity`; below the horizon, the horizon's light times
	 * `_ground_albedo`, which Hosek and Wilkie's model also takes as the
	 * light the ground sends back up into the sky. Then prefiltered as
	 * LIT_ENVIRONMENT's picture is. The sun's disc is not drawn here: a
	 * directional light's disc is the scene's
	 * (RBTREND_LIT_SCENE_LIGHT::_sky_disc), drawn by the sky shader, as a
	 * disc here would only be blurred into the rougher mips.
	 * `_sun_direction` is which way the sky's own model has its sun, the
	 * caller's (one of its directional lights, usually). Hosek and Wilkie's
	 * model is defined for a sun at or above the horizon, and one below is
	 * taken at it.
	 *
	 * Hosek and Wilkie's model is cooked on the CPU at the compile, from
	 * the authors' dataset, into a material the environment keeps; the
	 * environment holds one sky at a time, so two fills of one environment
	 * in one list leave the last.
	 */
	RBTREND_COMMAND_LIT_SKY                     = 32,
	/**
	 * Ambient occlusion from a depth buffer, outside a pass: how open each
	 * texel of `_depth` is to the sky, from the depth round it, out to
	 * `_radius` in the scene's units, raised to the power `_intensity` (1 as
	 * measured), then smoothed over 4x4 texels, written into `_target`'s red;
	 * and the bent normal, the average direction that is open (unit, in the
	 * scene's space, as 0.5 + 0.5 b), into its green, blue and alpha, for a
	 * target that has them (rbtrend_lit_scene_set_bent_normals() reads it).
	 * `_depth` is a SHADER_RESOURCE view, DEPTH aspect, 2D, of a depth buffer
	 * rendered through `_scene`'s view_projection (a depth pre-pass), at the
	 * view's target size; `_target` a RENDER_TARGET 2D view of a color format
	 * at that size, which rbtrend_lit_scene_set_ambient_occlusion() then
	 * names. Positions come back through the view's view_projection, so it
	 * must be invertible. Ground-truth ambient occlusion (Jimenez et al.,
	 * 2016; Hazel's XeGTAO): four slices through each texel's view ray, two
	 * horizons in each found by four steps either side (a sample fading out
	 * over the outer 0.615 of the radius), the visible arc between them
	 * integrated with the cosine, so a texel nothing stands over reads 1.
	 */
	RBTREND_COMMAND_LIT_AMBIENT_OCCLUSION       = 33,
	/**
	 * Adapts `_exposure` (rbtrendpost.h) to `_source`, outside a pass: the
	 * source's log-average luminance, brought to the exposure's key, eased
	 * towards from the exposure it last adapted to over `_delta_time`
	 * seconds. `_source` is a SHADER_RESOURCE view of one mip of a 2D color
	 * texture: what lit surfaces were drawn into, before the tone map that
	 * then reads the exposure. `_delta_time` is not negative.
	 */
	RBTREND_COMMAND_AUTO_EXPOSURE               = 34,
	/**
	 * Draws `_source_rect` of `_source` into `_target_rect` of `_target`,
	 * fitted as `_scaling` says (RBTREND_TEXTURE_SCALING, with the two
	 * rectangles as the source and the target): every texel of the target
	 * rectangle is written, from the source rectangle through `_filter`'s
	 * sampler or, where it doesn't reach, `_border_color` (linear RGBA,
	 * written as the target's format writes it), and the target outside it
	 * keeps what it held. A zeroed rectangle is the whole mip, so with both
	 * zeroed the whole source fills the whole target. Outside a pass.
	 *
	 * A source rectangle reads only its own texels: a linear filter at its
	 * edge takes the texel outside it into the mix, as any sampled edge
	 * inside a texture does. A rectangle is inside its view's mip, its width
	 * and height both 0 or both above 0.
	 *
	 * `_source` is a SHADER_RESOURCE 2D view of color of one mip, moved to
	 * SHADER_READ_ONLY. `_target` is a RENDER_TARGET 2D view of a format
	 * that holds floats (float, normalized or sRGB), left in
	 * COLOR_ATTACHMENT_OPTIMAL; it may be of another size and format than
	 * the source. The two share no mip of a layer. `_filter` is
	 * CRGPU_FILTER_LINEAR or _NEAREST. `_border_color` is finite, and not
	 * read for STRETCH.
	 *
	 * It is what PRESENT_TEXTURE's STRETCH and ASPECT_FIT do onto a back
	 * buffer, onto a texture: a picture scaled to a thumbnail or a window, a
	 * lower render size brought up to the target's. It binds a pipeline and
	 * a set of its own, so a compute pipeline is set again after it before a
	 * dispatch; the binding set slots are not touched. The first draw onto a
	 * target format makes its pipeline, and the first that reads a view
	 * with a filter the set that reads it, which the view keeps.
	 */
	RBTREND_COMMAND_DRAW_TEXTURE                = 35,
	/**
	 * Blurs `_source` into `_target`: a Gaussian of `_sigma` texels of the
	 * target, replacing what the target held, color and alpha alike.
	 * Outside a pass.
	 *
	 * Separable, as a Gaussian is: one pass across, from the source into a
	 * CRGPU_FORMAT_R16G16B16A16_FLOAT texture of the target's size taken
	 * from the instance's pool (rbtrendcontext.h, "Textures a command uses
	 * in between"), and one down, from it into the target. Each texel is
	 * the sum of its neighbours out to three sigma each way, weighted by
	 * exp(-d^2 / (2 sigma^2)) for a neighbour d texels off and normalized
	 * to sum to 1; past an edge, the edge texel stands in. The taps are
	 * read in pairs through a linear filter, one sample for two texels.
	 *
	 * `_source` is a SHADER_RESOURCE view, 2D, of color, of one mip,
	 * written by anything submitted before or earlier in the list, and
	 * moved into CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY. `_target` is a
	 * RENDER_TARGET view, 2D, in a format a blur renders onto (the formats
	 * rbtrend_texture2d_generate_mips() takes), of the size of the source's
	 * mip: a paired tap reads two texels only where the texture it reads
	 * has the target's texels. A blur at a smaller size is the source drawn
	 * down to it first (RBTREND_COMMAND_DRAW_TEXTURE). It is left in
	 * CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL. The two share no mip of
	 * a layer. `_sigma` is above 0, a number, and at most
	 * RBTREND_BLUR_MAX_SIGMA.
	 *
	 * It binds pipelines and sets of its own, as BLOOM does. The first blur
	 * onto a target format makes its pipeline, the first of a size a pooled
	 * texture, and the first that reads a view the set that reads it,
	 * which the view keeps (the set a BLOOM of the view reads through).
	 */
	RBTREND_COMMAND_BLUR                        = 36,
	/**
	 * Draws an outline `_width` texels wide round what `_mask` covers, in
	 * `_color`, over what `_target` holds. Outside a pass.
	 *
	 * A texel of the mask is inside where its red is above 0.5. Each texel
	 * outside is covered by the outline as far as its centre is within
	 * `_width` + 1 of the nearest inside texel's centre: by `_width` + 1
	 * less that distance, held to [0, 1]. So the texels along a straight
	 * edge are covered by `_width` (up to 1), and an outline of a whole
	 * `_width` covers that many texels out from the edge, its outer side
	 * softened by the distance's fraction. Texels inside the mask are left
	 * as they are. The outline is laid over the target as `_color` at its
	 * alpha times the coverage would be (the target's color times one less
	 * that, plus `_color` times it; alpha likewise).
	 *
	 * The distances come from a jump flood (Rong and Tan, 2006): a pass
	 * marking the mask's texels, a pass for each halving of a jump from
	 * the first power of two that reaches `_width` + 1 down to 1, one more
	 * of 1, and the pass that draws, ping-ponging between two
	 * CRGPU_FORMAT_R32G32_FLOAT textures of the target's size taken from
	 * the instance's pool (rbtrendcontext.h, "Textures a command uses in
	 * between"). A wide outline costs a pass more for each doubling of its
	 * width, not a pass more a texel.
	 *
	 * `_mask` is a SHADER_RESOURCE view, 2D, of color, of one mip, in a
	 * format a shader reads as floats, written by anything submitted before
	 * or earlier in the list, and moved into
	 * CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY. `_target` is a RENDER_TARGET
	 * view, 2D, of the size of the mask's mip, in a format an outline
	 * renders onto (the formats rbtrend_texture2d_generate_mips() takes),
	 * left in CRGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL. The two share
	 * no mip of a layer. `_color` is linear RGBA, its alpha not
	 * premultiplied: each finite, red, green and blue not negative, alpha
	 * from 0 to 1. `_width` is above 0 and a number.
	 *
	 * It binds pipelines and sets of its own, as BLOOM does. The first
	 * outline makes its pipelines, the first onto a target format the
	 * drawing pass's for it, the first of a size two pooled textures, and
	 * the first that reads a view the set that reads it, which the view
	 * keeps.
	 */
	RBTREND_COMMAND_OUTLINE                     = 37,
	/**
	 * Culls `_scene`'s point and spot lights (rbtrendlit.h) for each tile of
	 * its view with depth bounds, outside a pass: as the cull a compile
	 * records at the top of a list for a scene with a target size, each
	 * tile's list holding the lights whose range reaches its frustum, but
	 * with the frustum's near and far planes at the least and most depth
	 * the tile's texels hold in `_depth`, so a light wholly in front of or
	 * behind what a tile shows (behind a wall, under a floor) is left out
	 * of its list (Hazel's forward+ culls against its pre-depth the same
	 * way). A tile keeps RBTREND_LIT_MAX_TILE_LIGHTS lights at most, so fewer
	 * lights to a tile is also fewer dropped.
	 *
	 * `_depth` is a SHADER_RESOURCE view, DEPTH aspect, 2D, of a depth
	 * buffer rendered through the scene's view_projection (a depth
	 * pre-pass) at the view's target size, written before the command,
	 * earlier in the list or before it; it is moved into
	 * CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY. The scene's view has a target
	 * size and a view_projection.
	 *
	 * A list that holds this command for a scene has no cull at its top for
	 * that scene: the surfaces that read the tiles are drawn after the
	 * command. It binds a pipeline and sets of its own, so a compute
	 * pipeline is set again after it before a dispatch, as after a pass;
	 * the binding set slots are not touched.
	 */
	RBTREND_COMMAND_LIT_CULL_LIGHTS             = 38,
	/**
	 * Copies the opaque picture into a mipped texture for surfaces with
	 * transmission to show through (rbtrendlit.h,
	 * rbtrend_lit_scene_set_scene_color()), outside a pass, recorded
	 * between the opaque draws and the transmitting ones: `_source`
	 * stretched onto `_target`'s level as DRAW_TEXTURE does (linear
	 * filtering), then every level below made from it as GENERATE_MIPS
	 * does (linear), the rougher surfaces reading the blurrier levels.
	 * Unreal's scene color and Godot's screen texture are the same copy.
	 *
	 * `_source` is a SHADER_RESOURCE 2D view of one mip of color: what the
	 * opaque surfaces were drawn into. `_target` is a RENDER_TARGET 2D view
	 * of a level of a texture made for mips, in a format a shader writes
	 * floats to; any size, the view's target size usually, or half of it.
	 * Every level it fills is left in CRGPU_IMAGE_LAYOUT_SHADER_READ_ONLY.
	 * It binds pipelines and sets of its own, as DRAW_TEXTURE and
	 * GENERATE_MIPS do.
	 */
	RBTREND_COMMAND_COPY_SCENE_COLOR            = 39,
	/**
	 * Draws `_batch`'s shapes (rbtrendbatch.h) in the order they were added,
	 * inside a pass whose color target is of the batch's color format and
	 * whose depth is of its depth format: an instanced draw a run of one
	 * kind. The batch was begun in the frame the list is compiled in; one
	 * begun with no shapes draws nothing.
	 *
	 * It stands for the commands it sends (rbtrend_command_list_send()
	 * expands it there): it sets a pipeline, binding set slot 0, the push
	 * constants and vertex buffer slot 0, which are the caller's to set
	 * again after it.
	 */
	RBTREND_COMMAND_DRAW_BATCH                  = 40,
	/**
	 * Builds, refits or compacts `_blas` (rbtrendraytracing.h), as `_mode`
	 * (RBTREND_BUILD_MODE) says, outside a pass. The device has
	 * CRGPU_FEATURE_RAY_QUERY.
	 *
	 * Built from its geometries' buffers as they are when the command runs,
	 * written by anything submitted before or earlier in the list. A static
	 * BLAS is built for fast tracing, and allows compaction; a deforming
	 * one for fast building, and allows refitting. Once a static BLAS's
	 * build has completed, its compacted size is read back
	 * (rbtrend_blas_get_info()); compacting it is the caller's choice, a
	 * BUILD_BLAS with RBTREND_BUILD_MODE_COMPACT.
	 *
	 * Consecutive BUILD_BLAS and BUILD_TLAS commands of a list are one
	 * build call to the device: every BLAS of the run first, then one
	 * barrier, then every TLAS. A BLAS built in a list is ordered before
	 * every BUILD_TLAS after it in that list. The build's scratch is the
	 * instance's, one buffer grown to the largest a compiled list needs,
	 * an allocation at that compile and only then.
	 */
	RBTREND_COMMAND_BUILD_BLAS                  = 41,
	/**
	 * Builds `_tlas` (rbtrendraytracing.h) from its set being prepared,
	 * outside a pass. The device has CRGPU_FEATURE_RAY_QUERY.
	 *
	 * Sending it seals the set: the record keeps which set it builds from,
	 * and the next write to the TLAS goes to the next set, so a write after
	 * the send changes no build already sent. Sending checks every BLAS slot
	 * the set names: each names a live BLAS that has been built, or that a
	 * BUILD_BLAS earlier in the same list builds; the first that doesn't is
	 * refused, naming the slot, and nothing is sealed.
	 *
	 * Running, it copies the blocks of 64 records the set didn't write from
	 * the set sealed before it, writes every instance in the device's own
	 * layout with each BLAS's current structure (a compute pass of
	 * Rend's), and builds the TLAS for fast building, over the set's
	 * `_instance_count` instances, each BLAS ordered before it. The TLAS is
	 * rebuilt every time, never refitted. Builds of one TLAS run in the
	 * order they were sent, across lists too: a list holding one is
	 * refused at submit while a list that sent an earlier build of the same
	 * TLAS is not yet submitted (rbtrend_command_list_submit()). A trace of
	 * the TLAS later in the list, or in a later list, reads this build.
	 */
	RBTREND_COMMAND_BUILD_TLAS                  = 42,
	/**
	 * Traces the shadow of directional light `_light` of `_scene`
	 * (rbtrendlit.h; its place in the scene's lights) over a depth pre-pass into a mask, outside a pass. The
	 * device has CRGPU_FEATURE_RAY_QUERY. The mask is what the lit
	 * surface reads for that light instead of shadow maps, once
	 * rbtrend_lit_scene_set_light_shadow() names it.
	 *
	 * Each texel of `_depth` is brought back into the scene's space
	 * through the scene's view_projection, moved `_normal_offset` along
	 * the normal its depth's slopes give (the depth's own normals: no
	 * normal target is read), and `_ray_count` rays are traced from it
	 * through `_tlas` towards the light, over the light's disc (its
	 * RBTREND_LIT_SCENE_LIGHT `_direction` and `_radius` as the scene has
	 * them; a radius of 0 is a hard shadow),
	 * in blue-noise directions, up to `_max_distance`, against the
	 * instances whose mask shares a bit with `_cull_mask`. The fraction that
	 * reach the light is filtered over `_filter_radius` texels each way,
	 * weighted by depth and normal so an edge is kept, and written into
	 * `_target`'s red: 1 lit, 0 in shadow. There is no filter over time.
	 *
	 * `_scene`'s view has a target size and a view_projection that can be
	 * inverted, and `_light` names a directional light of it (one that
	 * names no light, or a light that isn't directional, is refused at
	 * send, and at the compile if the scene's lights changed in between).
	 * `_depth` is a SHADER_RESOURCE
	 * view, DEPTH aspect, 2D, of a depth buffer rendered through the
	 * view's view_projection at its target size, written earlier in the
	 * list or before it. `_target` is an UNORDERED_ACCESS 2D view of an
	 * CRGPU_FORMAT_R8_UNORM or CRGPU_FORMAT_R16_FLOAT texture of the
	 * same size, which the scene then reads; its write is ordered before
	 * any later read of it. `_tlas` has been built, by a BUILD_TLAS earlier
	 * in the list or submitted before. `_ray_count` is 1 or more,
	 * `_filter_radius` 0 (no filter) to 8, `_cull_mask` 1 to 255,
	 * `_normal_offset` 0 or more and `_max_distance` above 0, both finite.
	 */
	RBTREND_COMMAND_LIT_SHADOW_MASK             = 43,
	/**
	 * Adds `_samples` path-traced samples a pixel of `_scene` (rbtrendlit.h)
	 * through `_tlas` to the accumulation in `_target`, outside a pass, in
	 * one dispatch. The device has CRGPU_FEATURE_RAY_QUERY.
	 *
	 * `_target` holds the running mean of `_accumulated` samples before the
	 * step and of `_accumulated` + `_samples` after it; `_accumulated` 0
	 * starts over, and the caller sets it back to 0 after any change to the
	 * view, the instances, the materials, the environment or the lights:
	 * accumulating is right only over an unchanged scene. The same `_seed`,
	 * counts and scene give the same bits on one device. A step is one
	 * dispatch, which an operating system ends past its time limit (about
	 * two seconds on Windows): the caller sizes `_samples` from the samples
	 * a second it measures.
	 *
	 * Each path starts from the view's eye through a jittered point of its
	 * pixel (the view_projection's inverse), and bounces at most
	 * `_max_bounces` times, by each lit material's values
	 * (RBTREND_LIT_MATERIAL_VALUES' base color, emission, roughness,
	 * metalness and specular: every surface opaque; opacity, transmission,
	 * the coat, the sheen and the maps are not read).
	 * Light comes from the environment (importance-sampled), every
	 * directional light over its disc (each one's `_radius`), emissive triangles (with multiple
	 * importance sampling; a surface gives off its emission from both its
	 * faces, as the lit surface lights a face seen from behind as its
	 * front), and point and spot lights (without). The hemisphere of
	 * RBTREND_LIT_AMBIENT is part of the environment. Paths end
	 * by Russian roulette from half the bounces; the light a bounce after
	 * the first brings is clamped to `_clamp` (0 for no clamp). Instances
	 * are seen by the paths where their mask shares a bit with
	 * `_cull_mask`, which leaves out every instance whose geometries reach
	 * a NULL entry of the TLAS's material table (an entry only shadows
	 * see): one that doesn't is refused at the compile, naming the
	 * instance.
	 *
	 * `_target` is an UNORDERED_ACCESS 2D view of an CRGPU_FORMAT_R32G32B32A32_FLOAT
	 * texture at the view's target size. `_albedo` and `_normal`, each NULL
	 * or an UNORDERED_ACCESS 2D view of an R32G32B32A32_FLOAT or R16G16B16A16_FLOAT
	 * texture of that size, receive the first hit's albedo and normal (unit,
	 * in the scene's space), averaged as the target is: the guides a
	 * denoiser reads. `_tlas` has been built, as for LIT_SHADOW_MASK.
	 * `_samples` and `_max_bounces` are 1 or more, `_cull_mask` 1 to 255,
	 * `_clamp` 0 or more and finite, and `_accumulated` + `_samples` does
	 * not overflow.
	 */
	RBTREND_COMMAND_LIT_PATH_TRACE              = 44,
	/**
	 * Screen-space reflections into `_reflection`, outside a pass, over
	 * the targets of surfaces drawn with
	 * RBTREND_LIT_SHADER_SURFACE_REFLECT_FRAGMENT (rbtrendlit.h).
	 *
	 * Each texel of `_reflection` (half `_depth`'s size each way, rounded
	 * up) stands for the 2x2 texels of `_depth` under it (fewer at the
	 * right and bottom edges of an odd size) and takes the one of them
	 * nearest the eye: its depth, its normal and roughness from
	 * `_normal_roughness`, its place. It reflects the view ray about that
	 * normal and marches the reflected ray over a Hi-Z pyramid of `_depth`
	 * (built here, from the render target pool) for at most `_max_steps`
	 * steps, up to `_max_distance` in the scene's units. A hit within
	 * `_thickness` behind the depth it passes reads `_color` at the hit's
	 * texel, at a mip chosen from the roughness and the distance travelled
	 * (a cone over the mips). The texel is written rgb the reflected
	 * radiance (unexposed: `_color` divided by the view's exposure, as
	 * transmission reads it) and a the confidence: 1 for a hit well inside
	 * the screen, fading to 0 within `_edge_fade` of its edge and as the
	 * ray turns back towards the eye; 0 for a miss, for a texel with no
	 * surface, and for a surface rougher than `_max_roughness`, where
	 * nothing is marched and the environment stays.
	 *
	 * `_scene` gives the view: its view_projection (which can be
	 * inverted), target size, eye and exposure. `_depth` is a
	 * SHADER_RESOURCE 2D view, DEPTH aspect, of a depth buffer rendered
	 * through that view_projection at the view's target size;
	 * `_normal_roughness` a SHADER_RESOURCE 2D view of R16G16B16A16_FLOAT
	 * of the same size, the reflecting surfaces' target 1 (cleared to 0
	 * where nothing was drawn); `_color` a SHADER_RESOURCE 2D view of a
	 * color texture with its mips (COPY_SCENE_COLOR's target, from the
	 * opaque surfaces); `_reflection` an UNORDERED_ACCESS 2D view of one
	 * mip of R16G16B16A16_FLOAT, half the view's target size each way,
	 * rounded up. Each view but `_reflection` holds what was written
	 * earlier in the list or before it. `_max_steps` is 1 to 256;
	 * `_max_distance` and `_thickness` above 0; `_edge_fade` 0 to 0.5;
	 * `_max_roughness` 0 to 1; all finite. Its write is ordered before
	 * any later read of `_reflection`.
	 *
	 * The frame's order is the caller's to record: the opaque surfaces
	 * (with the reflecting variant where reflections are wanted),
	 * COPY_SCENE_COLOR, LIT_SSR, LIT_TRACE_REFLECTIONS, LIT_REFLECT, then
	 * the transparent surfaces, then the tone map. Transparent surfaces
	 * write none of the variant's targets: they are drawn after LIT_REFLECT
	 * and are not reflected.
	 *
	 * It binds pipelines and sets of its own: a compute pipeline is set
	 * again after it before a dispatch.
	 */
	RBTREND_COMMAND_LIT_SSR                     = 45,
	/**
	 * Ray-traced reflections into `_reflection` where its confidence is
	 * short of 1, outside a pass. The device has CRGPU_FEATURE_RAY_QUERY.
	 *
	 * Each texel of `_reflection` whose a is below 1, and whose surface
	 * (from `_depth` and `_normal_roughness`, the texel of the 2x2 under it
	 * nearest the eye, as LIT_SSR takes it) is at most `_max_roughness`
	 * rough, traces `_ray_count` rays from the surface along its
	 * reflection (a rough surface's over its GGX lobe, in blue-noise
	 * directions as LIT_SHADOW_MASK's) through `_tlas`, against the
	 * instances whose mask shares a bit with `_cull_mask`, up to
	 * `_max_distance`. A hit is shaded with its lit material's values
	 * (RBTREND_LIT_MATERIAL_VALUES' base color, emission, roughness,
	 * metalness and specular: no maps, opaque), every directional light of
	 * `_scene` (unshadowed), its point and spot lights and its ambient
	 * light; a miss reads the scene's ambient light and environment along
	 * the ray. A hit on an entry of the TLAS's material table that is NULL
	 * (one only shadows see) is black. The texel becomes the traced
	 * radiance mixed under what was there by its confidence,
	 * lerp(traced, rgb, a), with a 1: where the march was sure, its hit is
	 * kept. A texel with no surface, or a rougher one, is left as it was.
	 *
	 * Nothing is denoised: over a rough lobe the result is as noisy as
	 * `_ray_count` rays make it, so `_max_roughness` is the caller's bound
	 * on that noise (one ray is noisy above a roughness of about 0.3).
	 *
	 * `_scene`, `_depth` and `_normal_roughness` are as LIT_SSR's;
	 * `_reflection` an UNORDERED_ACCESS 2D view of one mip of
	 * R16G16B16A16_FLOAT, half the view's target size each way, rounded
	 * up, holding a march's result or cleared to 0 (a trace alone).
	 * `_tlas` has been built, by a BUILD_TLAS earlier in the list or
	 * submitted before. `_ray_count` is 1 or more, `_cull_mask` 1 to 255,
	 * `_max_distance` above 0 and `_max_roughness` 0 to 1, both finite.
	 * Its write is ordered before any later read of `_reflection`.
	 */
	RBTREND_COMMAND_LIT_TRACE_REFLECTIONS       = 46,
	/**
	 * Puts `_reflection` on `_target`, outside a pass. At each texel of
	 * `_target` (the view's target size), the reflection is upsampled from
	 * the four texels of `_reflection` round it, weighted by their
	 * distance and by how near each one's depth and normal are to the
	 * texel's (as GTAO's blur is), and added as
	 * specular x a x (reflection - environment specular) x exposure: the
	 * specular weight from `_specular`, the environment specular worked
	 * out again from `_scene`'s ambient light and environment at the
	 * texel's normal and roughness, as the surface shaded it.
	 *
	 * Worked out again from an 8-bit specular weight and a 16-bit float
	 * normal and roughness, the environment specular cancels what the
	 * surface added only to within their precision: at a of 1 the picture
	 * is the reflection to within that, not bit for bit. Where a is 0, or
	 * no surface was drawn (a normal of 0), nothing is added and the
	 * picture is unchanged bit for bit.
	 *
	 * `_scene` is as LIT_SSR's, its output encoding linear
	 * (RBTREND_COLOR_ENCODING_LINEAR): the picture is linear, tone mapped
	 * after. `_depth` and `_normal_roughness` are as LIT_SSR's;
	 * `_specular` a SHADER_RESOURCE 2D view of R8G8B8A8_UNORM of the same
	 * size, the reflecting surfaces' target 2; `_reflection` a
	 * SHADER_RESOURCE 2D view of one mip of R16G16B16A16_FLOAT, half the
	 * view's target size each way, rounded up; `_target` a RENDER_TARGET 2D
	 * view of the picture the surfaces wrote, of a float format
	 * (R16G16B16A16_FLOAT, R32G32B32A32_FLOAT or R11G11B10_FLOAT) at the
	 * view's target size, left in COLOR_ATTACHMENT_OPTIMAL. Each view but
	 * `_target` holds what was written earlier in the list or before it.
	 *
	 * It binds pipelines and sets of its own, as BLOOM does.
	 */
	RBTREND_COMMAND_LIT_REFLECT                 = 47,
};

/**
 * @brief One command of a command list.
 *
 * Every array a command points at (`_vertex_buffers`, `_viewports`,
 * `_scissor_rects`, `_color_targets`, the `_data` of PUSH_CONSTANTS, the
 * `_label` of BEGIN_LABEL) is
 * copied into the command list by rbtrend_command_list_send(), so the caller's
 * array only has to live for that call. What the entries refer to - buffers,
 * views - must stay alive until the list's submission has completed.
 */
/* Frozen: an array element, recorded every frame. A new command is a new RBTREND_COMMAND_TYPE inside the fixed size #_params is padded to. It never changes; a new need gets a new type. */
typedef struct RBTREND_COMMAND {
	/** Which command this is: the member of #_params that is read. */
	RBTREND_COMMAND_TYPE _type;
	union {
		struct {
			/** The buffers, `_vertex_buffer_count` of them; copied at the send. */
			rbtrend_buffer *_vertex_buffers;
			/** How many buffers `_vertex_buffers` holds. */
			uint32_t _vertex_buffer_count;
			/** The slot the first buffer goes in; the rest follow it. */
			uint32_t _start_slot;
		} _set_vertex_buffers; /**< RBTREND_COMMAND_SET_VERTEX_BUFFERS. */
		struct {
			/** The index buffer. */
			rbtrend_buffer _index_buffer;
		} _set_index_buffer; /**< RBTREND_COMMAND_SET_INDEX_BUFFER. */
		struct {
			/** The graphics pipeline. */
			rbtrend_graphics_pipeline _pipeline;
		} _set_shader_pipeline; /**< RBTREND_COMMAND_SET_GRAPHICS_PIPELINE. */
		struct {
			/** How many vertices are drawn. */
			uint32_t _vertex_count;
			/** The first vertex. */
			uint32_t _start_vertex_location;
		} _draw; /**< RBTREND_COMMAND_DRAW. */
		struct {
			/** How many indices are drawn. */
			uint32_t _index_count;
			/** The first index. */
			uint32_t _start_index_location;
			/** Added to each index before it picks a vertex. */
			int32_t _base_vertex_location;
		} _draw_indexed; /**< RBTREND_COMMAND_DRAW_INDEXED. */
		struct {
			/** How many vertices each instance draws. */
			uint32_t _vertex_count_per_instance;
			/** How many instances are drawn. */
			uint32_t _instance_count;
			/** The first vertex. */
			uint32_t _start_vertex_location;
			/** The first instance. */
			uint32_t _start_instance_location;
		} _draw_instanced; /**< RBTREND_COMMAND_DRAW_INSTANCED. */
		struct {
			/** How many indices each instance draws. */
			uint32_t _index_count_per_instance;
			/** How many instances are drawn. */
			uint32_t _instance_count;
			/** The first index. */
			uint32_t _start_index_location;
			/** Added to each index before it picks a vertex. */
			int32_t _base_vertex_location;
			/** The first instance. */
			uint32_t _start_instance_location;
		} _draw_indexed_instanced; /**< RBTREND_COMMAND_DRAW_INSTANCED_INDEXED. */
		struct {
			/** The viewports, `_viewport_count` of them; copied at the send. */
			const CRGPU_VIEWPORT *_viewports;
			/** The index of the first viewport set. */
			uint32_t _first_viewport;
			/** How many viewports `_viewports` holds. */
			uint32_t _viewport_count;
		} _set_viewports; /**< RBTREND_COMMAND_SET_VIEWPORTS. */
		struct {
			/** The rectangles, `_scissor_rect_count` of them; copied at the send. */
			const CRGPU_SCISSOR_RECT *_scissor_rects;
			/** The index of the first rectangle set. */
			uint32_t _first_scissor_rect;
			/** How many rectangles `_scissor_rects` holds. */
			uint32_t _scissor_rect_count;
		} _set_scissor_rects; /**< RBTREND_COMMAND_SET_SCISSOR_RECTS. */
		struct {
			/** The color targets, `_color_target_count` of them; copied at the send. */
			const RBTREND_COLOR_TARGET *_color_targets;
			/** How many color targets `_color_targets` holds, at most RBTREND_MAX_COLOR_TARGETS. */
			uint32_t _color_target_count;
			/** The depth target. */
			RBTREND_DEPTH_TARGET _depth_target;
			/** The render area's left edge. */
			int32_t _x;
			/** The render area's top edge. */
			int32_t _y;
			/**
			 * The render area's width. 0 means the full width of the first
			 * color target (or the depth target if there's none).
			 */
			uint32_t _width;
			/** The render area's height. 0 means the full height, as `_width`. */
			uint32_t _height;
			/**
			 * A GPU timer (rbtrendtimer.h) the pass is timed into, at slot
			 * `_timer_slot`; NULL to time nothing. Of the list's instance.
			 */
			rbtrend_gpu_timer _timer;
			/** The slot of `_timer` the pass is timed into, below its slot count. */
			uint32_t _timer_slot;
		} _begin_rendering; /**< RBTREND_COMMAND_BEGIN_RENDERING. END_RENDERING reads nothing. */
		struct {
			/** The set slot. */
			uint32_t _slot;
			/** The binding set. */
			rbtrend_binding_set _set;
		} _set_binding_set; /**< RBTREND_COMMAND_SET_BINDING_SET. */
		struct {
			/** The texture presented, mip 0 of layer 0. */
			rbtrend_texture2d _source;
			/** The swap chain whose acquired back buffer it goes into. */
			rbtrend_swap_chain _swap_chain;
			/** NONE (0) a copy; STRETCH and ASPECT_FIT a draw. */
			RBTREND_TEXTURE_SCALING _scaling;
			/** STRETCH and ASPECT_FIT: CRGPU_FILTER_LINEAR or _NEAREST. */
			CRGPU_FILTER _filter;
			/** ASPECT_FIT's bars, linear RGBA. */
			float _border_color[4];
		} _present_texture; /**< RBTREND_COMMAND_PRESENT_TEXTURE. */
		struct {
			/** The bytes, `_size` of them; copied at the send. */
			const void *_data;
			/** Where in the block the bytes go, a multiple of 4. */
			uint32_t _offset;
			/** How many bytes, a multiple of 4 and not 0. */
			uint32_t _size;
		} _push_constants; /**< RBTREND_COMMAND_PUSH_CONSTANTS. */
		struct {
			/** The compute pipeline. */
			rbtrend_compute_pipeline _pipeline;
		} _set_compute_pipeline; /**< RBTREND_COMMAND_SET_COMPUTE_PIPELINE. */
		struct {
			/** Workgroups along x, not 0. */
			uint32_t _group_count_x;
			/** Workgroups along y, not 0. */
			uint32_t _group_count_y;
			/** Workgroups along z, not 0. */
			uint32_t _group_count_z;
		} _dispatch; /**< RBTREND_COMMAND_DISPATCH. */
		struct {
			/** The argument buffer. */
			rbtrend_buffer _buffer;
			/** The byte the first entry starts at, a multiple of 4. */
			uint64_t _offset;
			/** How many draws, not 0. */
			uint32_t _draw_count;
			/** Bytes from one entry to the next, a multiple of 4, at least an entry. */
			uint32_t _stride;
		} _draw_indirect; /**< RBTREND_COMMAND_DRAW_INDIRECT and RBTREND_COMMAND_DRAW_INDEXED_INDIRECT. */
		struct {
			/** Workgroups along x, not 0. */
			uint32_t _group_count_x;
			/** Workgroups along y, not 0. */
			uint32_t _group_count_y;
			/** Workgroups along z, not 0. */
			uint32_t _group_count_z;
		} _dispatch_mesh; /**< RBTREND_COMMAND_DISPATCH_MESH. */
		struct {
			/** The span, taken in the frame the list is compiled in. */
			RBTREND_TRANSIENT_SPAN _span;
			/** The vertex buffer slot. */
			uint32_t _slot;
		} _set_transient_vertex_buffer; /**< RBTREND_COMMAND_SET_TRANSIENT_VERTEX_BUFFER. */
		struct {
			/** The span, taken in the frame the list is compiled in. */
			RBTREND_TRANSIENT_SPAN _span;
			/** The size of one index: 2 or 4. */
			uint32_t _index_size;
		} _set_transient_index_buffer; /**< RBTREND_COMMAND_SET_TRANSIENT_INDEX_BUFFER. */
		struct {
			/** The set slot. */
			uint32_t _slot;
			/** The material. */
			rbtrend_material _material;
		} _set_material; /**< RBTREND_COMMAND_SET_MATERIAL. */
		struct {
			/** The render pass. */
			rbtrend_render_pass _pass;
		} _begin_render_pass; /**< RBTREND_COMMAND_BEGIN_RENDER_PASS. */
		struct {
			/** The compute pass. */
			rbtrend_compute_pass _pass;
		} _set_compute_pass; /**< RBTREND_COMMAND_SET_COMPUTE_PASS. */
		struct {
			/** The draw list, built in the frame the list is compiled in. */
			rbtrend_draw_list _list;
			/** The set slot materials go in; RBTREND_DRAW_LIST_NO_MATERIAL for none. */
			uint32_t _material_slot;
		} _draw_list; /**< RBTREND_COMMAND_DRAW_LIST. */
		struct {
			/** Null-terminated text. NULL is an empty label. */
			const char *_label;
		} _begin_label; /**< RBTREND_COMMAND_BEGIN_LABEL. END_LABEL reads nothing. */
		struct {
			/** What glows. */
			rbtrend_texture_view _source;
			/** What the glow is added onto. */
			rbtrend_texture_view _target;
			/** The brightness a source texel needs before it glows; 0 for every texel. */
			float _threshold;
			/** How far below `_threshold` the glow fades in. */
			float _knee;
			/** How far apart the taps reading a level back up are, in texels; above 0. */
			float _radius;
			/** Scales what is added to the target. */
			float _intensity;
			/** How many levels the glow is spread through. */
			uint32_t _levels;
			/**
			 * NULL, or a SHADER_RESOURCE 2D view of color whose red weights
			 * how much each texel of the source glows, read at the same
			 * place in [0, 1]: 1 as without it, 0 not at all.
			 */
			rbtrend_texture_view _mask;
			/**
			 * NULL, or a SHADER_RESOURCE 2D view of color laid over the
			 * whole target, whose color times `_lens_dirt_intensity`
			 * brightens the glow where it falls: lens dirt.
			 */
			rbtrend_texture_view _lens_dirt;
			/** How much `_lens_dirt` adds to the glow. Not read without `_lens_dirt`. */
			float _lens_dirt_intensity;
		} _bloom; /**< RBTREND_COMMAND_BLOOM. */
		struct {
			/** What is tone mapped. */
			rbtrend_texture_view _source;
			/** What the result replaces. */
			rbtrend_texture_view _target;
			/** What each texel is multiplied by first; with `_auto_exposure`, a compensation. */
			float _exposure;
			/** The curve. */
			RBTREND_TONE_CURVE _curve;
			/** How the result is written. */
			RBTREND_COLOR_ENCODING _encoding;
			/** NULL for `_exposure` alone. */
			rbtrend_exposure _auto_exposure;
		} _tone_map; /**< RBTREND_COMMAND_TONE_MAP. */
		struct {
			/** The environment filled. */
			rbtrend_lit_environment _environment;
			/** Towards the sun, in the environment's space (+Y up). */
			float _sun_direction[3];
			/** How hazy: 2 clear to 10 hazy. */
			float _turbidity;
			/** The zenith's luminance. */
			float _intensity;
			/** What the ground below the horizon reflects, linear RGB. */
			float _ground_albedo[3];
			/** Which sky: RBTREND_LIT_SKY_MODEL_PREETHAM (0) or _HOSEK_WILKIE. */
			RBTREND_LIT_SKY_MODEL _model;
		} _lit_sky; /**< RBTREND_COMMAND_LIT_SKY. */
		struct {
			/** The scene whose view the depth was rendered through. */
			rbtrend_lit_scene _scene;
			/** The depth pre-pass. */
			rbtrend_texture_view _depth;
			/** The occlusion in red, the bent normal in green, blue and alpha. */
			rbtrend_texture_view _target;
			/** How far out occlusion is looked for, in the scene's units. */
			float _radius;
			/** The power the occlusion is raised to; 1 as measured. */
			float _intensity;
		} _lit_ambient_occlusion; /**< RBTREND_COMMAND_LIT_AMBIENT_OCCLUSION. */
		struct {
			/** The exposure adapted. */
			rbtrend_exposure _exposure;
			/** What it adapts to. */
			rbtrend_texture_view _source;
			/** Seconds since the last adaptation, not negative. */
			float _delta_time;
		} _auto_exposure; /**< RBTREND_COMMAND_AUTO_EXPOSURE. */
		struct {
			/** The environment filled. */
			rbtrend_lit_environment _environment;
			/** A cube or equirectangular picture, read and not kept. */
			rbtrend_texture_view _source;
		} _lit_environment; /**< RBTREND_COMMAND_LIT_ENVIRONMENT. */
		struct {
			/** What is drawn. */
			rbtrend_texture_view _source;
			/** What it is drawn into, every texel written. */
			rbtrend_texture_view _target;
			/** How the source is fitted to the target. */
			RBTREND_TEXTURE_SCALING _scaling;
			/** CRGPU_FILTER_LINEAR or CRGPU_FILTER_NEAREST. */
			CRGPU_FILTER _filter;
			/** Where the source doesn't reach, linear RGBA. Not read for STRETCH. */
			float _border_color[4];
			/** The part of the source drawn; zeroed for all of it. */
			RBTREND_TEXEL_RECT _source_rect;
			/** The part of the target drawn into; zeroed for all of it. */
			RBTREND_TEXEL_RECT _target_rect;
		} _draw_texture; /**< RBTREND_COMMAND_DRAW_TEXTURE. */
		struct {
			/** What is blurred. */
			rbtrend_texture_view _source;
			/** What the blur replaces. */
			rbtrend_texture_view _target;
			/** The Gaussian's sigma, in texels of the target. */
			float _sigma;
		} _blur; /**< RBTREND_COMMAND_BLUR. */
		struct {
			/** What the outline goes round: red above 0.5 is inside. */
			rbtrend_texture_view _mask;
			/** What the outline is drawn over. */
			rbtrend_texture_view _target;
			/** Linear RGBA, alpha not premultiplied. */
			float _color[4];
			/** How far out from the mask the outline reaches, in texels. */
			float _width;
		} _outline; /**< RBTREND_COMMAND_OUTLINE. */
		struct {
			/** The scene whose lights are culled. */
			rbtrend_lit_scene _scene;
			/** The view's depth, the tiles' bounds. */
			rbtrend_texture_view _depth;
		} _lit_cull_lights; /**< RBTREND_COMMAND_LIT_CULL_LIGHTS. */
		struct {
			/** The texture, of the list's instance. */
			rbtrend_texture2d _texture;
			/** The level the others are made from; below the texture's mip count. */
			uint32_t _base_mip;
			/** CRGPU_FILTER_LINEAR or CRGPU_FILTER_NEAREST. */
			CRGPU_FILTER _filter;
		} _generate_mips; /**< RBTREND_COMMAND_GENERATE_MIPS. */
		struct {
			/** The opaque picture. */
			rbtrend_texture_view _source;
			/** The level filled; those below are made from it. */
			rbtrend_texture_view _target;
		} _copy_scene_color; /**< RBTREND_COMMAND_COPY_SCENE_COLOR. */
		struct {
			/** The batch, begun this frame. */
			struct rbtrend_batch_t *_batch;
		} _draw_batch; /**< RBTREND_COMMAND_DRAW_BATCH. */
		struct {
			/** The BLAS, of the list's instance. */
			rbtrend_blas _blas;
			/** Build, refit or compact. */
			RBTREND_BUILD_MODE _mode;
		} _build_blas; /**< RBTREND_COMMAND_BUILD_BLAS. */
		struct {
			/** The TLAS, of the list's instance; its set being prepared is sealed at the send. */
			rbtrend_tlas _tlas;
		} _build_tlas; /**< RBTREND_COMMAND_BUILD_TLAS. */
		struct {
			/** The scene whose view the depth was rendered through, and whose light is traced. */
			rbtrend_lit_scene _scene;
			/** What the rays are traced through. */
			rbtrend_tlas _tlas;
			/** Which directional light of the scene: its place in the scene's
			 * lights (rbtrend_lit_scene_set_lights()). */
			uint32_t _light;
			/** The depth pre-pass. */
			rbtrend_texture_view _depth;
			/** The mask, in red: 1 lit, 0 in shadow. */
			rbtrend_texture_view _target;
			/** Rays a texel, 1 or more. */
			uint32_t _ray_count;
			/** Texels each way the mask is filtered over, 0 to 8. */
			uint32_t _filter_radius;
			/** Which instances cast shadows: 1 to 255, against each instance's `_mask`. */
			uint32_t _cull_mask;
			/** How far a ray starts along the surface's normal, in the scene's units. */
			float _normal_offset;
			/** How far a ray looks for what stands in the light, in the scene's units. */
			float _max_distance;
		} _lit_shadow_mask; /**< RBTREND_COMMAND_LIT_SHADOW_MASK. */
		struct {
			/** The scene: its view, lights and environment. */
			rbtrend_lit_scene _scene;
			/** What the paths are traced through. */
			rbtrend_tlas _tlas;
			/** The accumulation: the running mean, RGBA32F. */
			rbtrend_texture_view _target;
			/** NULL, or where the first hit's albedo is averaged. */
			rbtrend_texture_view _albedo;
			/** NULL, or where the first hit's normal is averaged. */
			rbtrend_texture_view _normal;
			/** Samples a pixel this step, 1 or more. */
			uint32_t _samples;
			/** Samples the target holds before the step; 0 starts over. */
			uint32_t _accumulated;
			/** What the step's random numbers are made from. */
			uint32_t _seed;
			/** The most bounces a path takes, 1 or more. */
			uint32_t _max_bounces;
			/** Which instances the paths see: 1 to 255, against each instance's `_mask`. */
			uint32_t _cull_mask;
			/** The most light a bounce after the first brings; 0 for no clamp. */
			float _clamp;
		} _lit_path_trace; /**< RBTREND_COMMAND_LIT_PATH_TRACE. */
		struct {
			/** The scene: its view and exposure. */
			rbtrend_lit_scene _scene;
			/** The depth pre-pass: a SHADER_RESOURCE 2D view of depth at the view's target size. */
			rbtrend_texture_view _depth;
			/** The surfaces' normal and roughness: a SHADER_RESOURCE 2D view of R16G16B16A16_FLOAT, that size. */
			rbtrend_texture_view _normal_roughness;
			/** The mipped copy of the opaque picture: a SHADER_RESOURCE 2D view of color with its mips. */
			rbtrend_texture_view _color;
			/** What is written: an UNORDERED_ACCESS 2D view of R16G16B16A16_FLOAT, half the size rounded up. */
			rbtrend_texture_view _reflection;
			/** The most steps a ray marches, 1 to 256. */
			uint32_t _max_steps;
			/** How far a ray reaches, in the scene's units; above 0. */
			float _max_distance;
			/** How far behind a depth a ray still hits it, in the scene's units; above 0. */
			float _thickness;
			/** The share of the screen at each edge a hit fades out over, 0 to 0.5. */
			float _edge_fade;
			/** The roughest surface marched, 0 to 1; rougher keeps the environment. */
			float _max_roughness;
		} _lit_ssr; /**< RBTREND_COMMAND_LIT_SSR. */
		struct {
			/** The scene: its view, lights and environment. */
			rbtrend_lit_scene _scene;
			/** What the rays are traced through. */
			rbtrend_tlas _tlas;
			/** The depth pre-pass, as LIT_SSR's. */
			rbtrend_texture_view _depth;
			/** The surfaces' normal and roughness, as LIT_SSR's. */
			rbtrend_texture_view _normal_roughness;
			/** What is traced into where its confidence is short of 1, as LIT_SSR writes it. */
			rbtrend_texture_view _reflection;
			/** Rays a texel, 1 or more. */
			uint32_t _ray_count;
			/** Which instances the rays see: 1 to 255, against each instance's `_mask`. */
			uint32_t _cull_mask;
			/** How far a ray reaches, in the scene's units; above 0. */
			float _max_distance;
			/** The roughest surface traced, 0 to 1. */
			float _max_roughness;
		} _lit_trace_reflections; /**< RBTREND_COMMAND_LIT_TRACE_REFLECTIONS. */
		struct {
			/** The scene: its view, ambient light, environment and exposure. */
			rbtrend_lit_scene _scene;
			/** The depth pre-pass, as LIT_SSR's. */
			rbtrend_texture_view _depth;
			/** The surfaces' normal and roughness, as LIT_SSR's. */
			rbtrend_texture_view _normal_roughness;
			/** The surfaces' specular weight: a SHADER_RESOURCE 2D view of R8G8B8A8_UNORM at the view's target size. */
			rbtrend_texture_view _specular;
			/** The reflection: a SHADER_RESOURCE 2D view of R16G16B16A16_FLOAT, half the size rounded up. */
			rbtrend_texture_view _reflection;
			/** The picture: a RENDER_TARGET 2D view of a float format at the view's target size. */
			rbtrend_texture_view _target;
		} _lit_reflect; /**< RBTREND_COMMAND_LIT_REFLECT. */
		/**
		 * Never read. Fixes `_params` at 120 bytes, so the command's size
		 * stays 128 as commands are added: a new command's parameters fit
		 * in it or come through a pointer.
		 */
		uint8_t _padding[120];
	} _params; /**< The command's parameters, one member for each RBTREND_COMMAND_TYPE. */
} RBTREND_COMMAND;

/**
 * @brief Opaque handle to a command list.
 */
typedef struct rbtrend_command_list_t *rbtrend_command_list;

/**
 * @brief Creates an empty command list.
 *
 * It holds no Carrottus GPU list of its own: each compile records into one of
 * the instance's for the frame (rbtrendcontext.h).
 *
 * @param[in]  instance  Instance the list records, compiles and submits
 *                       through.
 * @param[out] out_list  Receives the list; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p instance or
 *         @p out_list is NULL; or RBT_RESULT_OUT_OF_MEMORY.
 *
 * @see rbtrend_command_list_destroy()
 */
RBT_RESULT rbtrend_command_list_create(rbtrend_instance instance, rbtrend_command_list *out_list, RBTREND_ERROR *out_error);

/**
 * @brief Destroys a command list.
 *
 * The GPU doesn't have to be finished with what it submitted
 * (rbtrendcontext.h, "Destroying").
 *
 * @param[in] cmd_list List to destroy. NULL is accepted and does nothing.
 */
void rbtrend_command_list_destroy(rbtrend_command_list cmd_list);

/**
 * @brief Empties a command list: every recorded command, a recording error,
 * and the last compile are gone.
 *
 * Thread: one caller to the list; it may overlap calls on other handles of
 * the instance, other lists' recording included, from any thread - except
 * when the list holds a compile not yet submitted. Then it gives back to
 * the instance's render target pool what that compile took, and is one of
 * the calls made by one caller at a time across the instance
 * (rbtrendcontext.h, "Threads").
 *
 * @param[in] cmd_list List to reset. NULL is accepted and does nothing.
 */
void rbtrend_command_list_reset(rbtrend_command_list cmd_list);

/**
 * @brief Records one command at the end of a command list.
 *
 * A malformed command isn't recorded and makes the next
 * rbtrend_command_list_compile() fail, until rbtrend_command_list_reset():
 *
 * - a BEGIN_RENDERING with more than RBTREND_MAX_COLOR_TARGETS color
 *   targets;
 * - a NULL array with a nonzero count in BEGIN_RENDERING,
 *   SET_VERTEX_BUFFERS, SET_VIEWPORTS or SET_SCISSOR_RECTS;
 * - a NULL entry in `_vertex_buffers`, one of another instance, or one
 *   created without RBTREND_BUFFER_USAGE_VERTEX;
 * - a SET_INDEX_BUFFER with a NULL buffer, one of another instance, or one
 *   created without RBTREND_BUFFER_USAGE_INDEX;
 * - a SET_VERTEX_BUFFERS that runs past slot
 *   RBTREND_MAX_VERTEX_BINDINGS - 1;
 * - a PUSH_CONSTANTS with NULL `_data`, a `_size` of 0, or an `_offset` or
 *   `_size` that is not a multiple of 4;
 * - a DISPATCH or DISPATCH_MESH with a group count of 0;
 * - a DRAW_INDIRECT or DRAW_INDEXED_INDIRECT with a NULL `_buffer`, one of
 *   another instance, or one created without
 *   RBTREND_BUFFER_USAGE_INDIRECT_ARGUMENTS, a `_draw_count`
 *   of 0, an `_offset` or `_stride` that is not a multiple of 4, a
 *   `_stride` smaller than one entry, or entries that run past the end of
 *   the buffer;
 * - a SET_TRANSIENT_VERTEX_BUFFER with a `_slot` at or above
 *   RBTREND_MAX_VERTEX_BINDINGS;
 * - a SET_TRANSIENT_INDEX_BUFFER with an `_index_size` that is not 2 or 4;
 * - a SET_MATERIAL with a NULL material, one of another instance, or a
 *   `_slot` at or above RBTREND_MAX_BINDING_LAYOUTS;
 * - a BEGIN_RENDER_PASS or SET_COMPUTE_PASS with a NULL pass or one of
 *   another instance, or a BEGIN_RENDER_PASS whose framebuffer has lost an
 *   attachment to a failed resize;
 * - a DRAW_LIST with a NULL list, one of another instance, one not built
 *   since it was last changed, or one with a draw that has no material when
 *   `_material_slot` is not RBTREND_DRAW_LIST_NO_MATERIAL, or a
 *   `_material_slot` at or above RBTREND_MAX_BINDING_LAYOUTS that isn't
 *   RBTREND_DRAW_LIST_NO_MATERIAL;
 * - a GENERATE_MIPS with a NULL texture or one of another instance, or one
 *   rbtrend_texture2d_generate_mips() refuses for its usage, its format, its
 *   level or its filter;
 * - a BLOOM with a NULL source or target, one of another instance, a
 *   source or target view of the wrong type, shape or aspect, a target in
 *   a format a bloom doesn't render onto, a source and target that share a
 *   mip of a layer, `_levels` 0 or more than the source's size allows, a
 *   setting that is negative or not a number, or a `_radius` of 0; or a
 *   `_mask` or `_lens_dirt` of another instance, not a SHADER_RESOURCE 2D
 *   view of color, or in a format a shader doesn't read as floats, a
 *   `_lens_dirt` that shares a mip of a layer with the target, or, with a
 *   `_lens_dirt`, a `_lens_dirt_intensity` that is negative or not a
 *   number;
 * - a TONE_MAP with a NULL source or target, one of another instance, a view
 *   of the wrong type, shape or aspect, a source of more than one mip, a
 *   source and target that share a mip of a layer, a target in a format it
 *   doesn't render onto, an `_exposure` that is negative or not a number,
 *   or a `_curve` or `_encoding` that is not one;
 * - a LIT_ENVIRONMENT with a NULL environment or source, one of another
 *   instance, or a source that is not a SHADER_RESOURCE view of color
 *   shaped CUBE or 2D;
 * - a TONE_MAP whose `_auto_exposure` is of another instance;
 * - a LIT_SKY with a NULL environment or one of another instance, a
 *   `_model` that is not an RBTREND_LIT_SKY_MODEL, a sun direction of 0, a
 *   turbidity outside its model's range, a negative intensity or albedo, an
 *   albedo above 1 for HOSEK_WILKIE, or a value that is not a number;
 * - a LIT_AMBIENT_OCCLUSION with a NULL scene, depth or target, one of
 *   another instance, a depth that is not a SHADER_RESOURCE 2D view of
 *   depth, a target that is not a RENDER_TARGET 2D view of a format it
 *   renders onto, the two of different sizes, or a radius that is not above
 *   0 or an intensity below 0;
 * - an AUTO_EXPOSURE with a NULL exposure or source, one of another
 *   instance, a source that is not a SHADER_RESOURCE 2D view of color of one
 *   mip, or a `_delta_time` that is negative or not a number;
 * - a DRAW_TEXTURE with a NULL source or target, one of another instance,
 *   a source that is not a SHADER_RESOURCE 2D view of color of one mip, a
 *   target that is not a RENDER_TARGET 2D view of a format that holds
 *   floats, the two sharing a mip of a layer, a `_scaling` that is not an
 *   RBTREND_TEXTURE_SCALING, a `_filter` that is not LINEAR or NEAREST, a
 *   `_border_color` it reads that is not finite, or a rectangle with one of
 *   its width and height 0 and not the other, with no size and a corner
 *   other than (0, 0), or not inside its view's mip;
 * - a BLUR with a NULL source or target, one of another instance, a source
 *   that is not a SHADER_RESOURCE 2D view of color of one mip, a target
 *   that is not a RENDER_TARGET 2D view in a format a blur renders onto, a
 *   target of another size than the source, a source and target that
 *   share a mip of a layer, or a `_sigma` that is
 *   not above 0, not a number or above RBTREND_BLUR_MAX_SIGMA;
 * - an OUTLINE with a NULL mask or target, one of another instance, a mask
 *   that is not a SHADER_RESOURCE 2D view of color of one mip in a format a
 *   shader reads as floats, a target that is not a RENDER_TARGET 2D view in
 *   a format an outline renders onto, a target of another size than the
 *   mask, a mask and target that share a mip of a layer, a `_color` with a
 *   value that is not finite, a negative red, green or blue or an alpha
 *   outside 0 to 1, or a `_width` that is not above 0 or not a number;
 * - a LIT_CULL_LIGHTS with a NULL scene or depth, one of another instance,
 *   or a depth that is not a SHADER_RESOURCE 2D view of depth;
 * - a DRAW_BATCH with a NULL batch, one of another instance, or one not
 *   begun;
 * - a COPY_SCENE_COLOR with a NULL source or target, one of another
 *   instance, a source that is not a SHADER_RESOURCE 2D view of one mip of
 *   color, a target that is not a RENDER_TARGET 2D view of one mip in a
 *   format a shader writes floats to, a source and target that share a mip
 *   of a layer, or a target whose texture GENERATE_MIPS refuses;
 * - a BUILD_BLAS, BUILD_TLAS, LIT_SHADOW_MASK or LIT_PATH_TRACE on a
 *   device without CRGPU_FEATURE_RAY_QUERY (RBT_RESULT_UNSUPPORTED, naming
 *   the feature);
 * - a BUILD_BLAS with a NULL BLAS or one of another instance, a `_mode`
 *   that is not an RBTREND_BUILD_MODE, a REFIT of a BLAS that doesn't
 *   deform, or a COMPACT of one that does;
 * - a BUILD_TLAS with a NULL TLAS or one of another instance, or whose set
 *   being prepared names a BLAS slot that is not live, or a BLAS neither
 *   built nor built by a BUILD_BLAS earlier in the list (naming the slot);
 *   the set is not sealed then;
 * - a LIT_SHADOW_MASK with a NULL scene, TLAS, depth or target, one of
 *   another instance, a `_light` that names no directional light of the
 *   scene, a depth that is not a SHADER_RESOURCE 2D view of depth, a
 *   target that is not an UNORDERED_ACCESS 2D view of R8_UNORM or R16_FLOAT, the two
 *   of different sizes, or a `_ray_count`, `_filter_radius`, `_cull_mask`,
 *   `_normal_offset` or `_max_distance` out of its range;
 * - a LIT_PATH_TRACE with a NULL scene, TLAS or target, one of another
 *   instance, a target that is not an UNORDERED_ACCESS 2D view of R32G32B32A32_FLOAT,
 *   an `_albedo` or `_normal` that is not NULL or an UNORDERED_ACCESS 2D view of
 *   R32G32B32A32_FLOAT or R16G16B16A16_FLOAT, a guide of another size than
 *   the target, or a `_samples`, `_max_bounces`, `_cull_mask` or `_clamp`
 *   out of its range, or an `_accumulated` + `_samples` that overflows.
 *
 * BEGIN_RENDER_PASS, SET_COMPUTE_PASS and DRAW_LIST are expanded here into
 * the commands they stand for, read from the pass or list as it is now (see
 * each). What those commands are is checked as if they had been sent one by
 * one.
 *
 * Thread: one caller to the list; it may overlap calls on other handles of
 * the instance, other lists' recording included, from any thread, as long
 * as nothing the command names is changed or destroyed meanwhile
 * (rbtrendcontext.h, "Threads").
 *
 * @param[in] cmd_list List to record into. NULL is accepted and does
 *                     nothing.
 * @param[in] cmd      The command. Copied, with every array it points at.
 */
void rbtrend_command_list_send(
    rbtrend_command_list cmd_list,
    RBTREND_COMMAND cmd);

/**
 * @brief Compiles a command list.
 *
 * Translates every command recorded since the last
 * rbtrend_command_list_reset() into a crgpu_command_list and closes it for
 * submission (crgpu_command_list_begin() / ... /
 * crgpu_command_list_end()). Only compiles; submit with
 * rbtrend_command_list_submit(), in the same frame. The Carrottus GPU list is one
 * of the instance's for the current frame, recorded through that frame's
 * command allocator: a compile after the list's last submit takes a fresh
 * one, since the last may still be running, and a compile in the same
 * frame with no submit in between records over its own. Compiling has no effect on any texture's
 * tracked layout, so a list that's compiled but never submitted (or fails
 * to compile) leaves nothing out of sync.
 *
 * A command refused when it was sent (rbtrend_command_list_send() keeps no
 * result of its own) fails the compile, reported here. Every failure's
 * message starts with the index of the send it is about, counted from the
 * last rbtrend_command_list_reset() from 0, the commands a BEGIN_RENDER_PASS,
 * SET_COMPUTE_PASS or DRAW_LIST stands for counting as that one send:
 * "command 412: BLUR source has nothing in it". A failure at the end of the
 * list (a pass left open) names no command.
 *
 * @param[in]  cmd_list  List to compile.
 * @param[out] out_error Why it failed, when it did, naming the command. May
 *                       be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p cmd_list is NULL;
 *         otherwise the code of the first failure the compile meets
 *         (RBT_RESULT_INVALID_ARGUMENT for a command that breaks a rule,
 *         RBT_RESULT_OUT_OF_MEMORY, RBT_RESULT_UNSUPPORTED, or the code for
 *         Carrottus GPU's result where something Rend makes for a command can't
 *         be), without a submittable result, if a malformed command was
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
 *         - a draw whose pipeline reads a vertex buffer slot that is
 *           empty, or whose known vertices or instances run past the end
 *           of a buffer or span at the binding's stride;
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
 *           RBTREND_MAX_BINDING_LAYOUTS or more;
 *         - a draw whose pipeline declares a set slot that holds no set or
 *           a set of another layout;
 *         - a pass whose binding sets break the texture rule of
 *           RBTREND_COMMAND_SET_BINDING_SET;
 *         - a draw or dispatch whose binding sets hold a texture view of
 *           another shape than the one the pipeline's shader declares at its
 *           binding (a 2D view for a `samplerCube`, a 2D array view for a
 *           `sampler2D`), or a stencil view in a binding the shader doesn't
 *           read as unsigned integers, or a depth view in one it doesn't
 *           read as floats (rbtrendshader.h);
 *         - a SET_MATERIAL whose material is not complete
 *           (rbtrend_material_is_complete()) or whose binding set can't be
 *           built;
 *         - a BLOOM inside a pass, whose source, mask or lens dirt has
 *           nothing in it (written neither before the list nor earlier
 *           in it), whose source no longer holds `_levels` (resized since
 *           it was sent), or whose
 *           pipelines, sets or pooled texture can't be made;
 *         - a TONE_MAP inside a pass, whose source has nothing in it, or
 *           whose pipeline or set can't be made;
 *         - a LIT_ENVIRONMENT inside a pass, whose source has nothing in it,
 *           or whose pipelines, views or sets can't be made; a LIT_SKY,
 *           LIT_AMBIENT_OCCLUSION or AUTO_EXPOSURE inside a pass, whose
 *           source or depth has nothing in it, or whose pipelines, views,
 *           sets or pooled textures can't be made;
 *         - a BLUR inside a pass, whose source has nothing in it, whose
 *           source and target are no longer one size (resized), or
 *           whose pipelines, sets or pooled texture can't be made;
 *         - an OUTLINE inside a pass, whose mask has nothing in it, whose
 *           mask and target are no longer one size (resized), or whose
 *           pipelines, sets or pooled textures can't be made;
 *         - a LIT_CULL_LIGHTS inside a pass, whose depth has nothing in
 *           it, whose scene's view has no target size or one other than
 *           the depth's size, or whose pipeline or sets can't be made;
 *         - a COPY_SCENE_COLOR inside a pass, or whose views, sets or
 *           pipelines can't be made;
 *         - a BUILD_BLAS, BUILD_TLAS, LIT_SHADOW_MASK or LIT_PATH_TRACE
 *           inside a pass; a REFIT of a BLAS never built, by a build that
 *           has completed or one earlier in the list; a COMPACT of a BLAS
 *           whose compacted size isn't known yet, or already compacted; a
 *           trace of a TLAS never built; a BUILD_TLAS whose list was
 *           submitted since it was sent (sent again, it seals the next
 *           set); a LIT_SHADOW_MASK whose depth has
 *           nothing in it or whose scene's view has no target size of the
 *           depth's or a view_projection that can't be inverted; a
 *           LIT_PATH_TRACE whose `_cull_mask` lets in an instance that
 *           reaches a NULL material table entry (naming the instance), or
 *           whose scene's view has no target size of the target's; or
 *           whose structures, scratch, pipelines, views or sets can't be
 *           made;
 *         - a GENERATE_MIPS inside a pass, or whose base level has nothing
 *           in some layer: written neither by anything submitted before nor
 *           earlier in the list; or whose views, sets or pipeline can't be
 *           made;
 *         - no Carrottus GPU command list to record into, if one has to be made
 *           and the device refuses it.
 *
 *         Compiling builds the binding set of every material a SET_MATERIAL
 *         names, when it has none or a resource was set since its last
 *         build (rbtrendmaterial.h). That is the one thing it changes outside
 *         the list, and it is done whether or not the rest of the compile
 *         succeeds.
 */
RBT_RESULT rbtrend_command_list_compile(rbtrend_command_list cmd_list, RBTREND_ERROR *out_error);

/**
 * @brief Submits the last successful compile of a command list and commits
 * the image layouts it leaves its textures in.
 *
 * The submission counts towards the current frame, which
 * rbtrend_instance_next_frame() (rbtrendcontext.h) waits for. A list is
 * submitted in the frame it was compiled in: the next
 * rbtrend_instance_next_frame() refuses every compile made before it. Texture
 * updates and framebuffer clears recorded since the last submission go
 * first, in the same submission (rbtrendcontext.h, "Texture updates and
 * clears").
 *
 * @p queue is the instance's one queue (rbtrendcontext.h): the queue of the
 * instance's first submission, and every one after it. Another is refused.
 * Submissions run in submit order on it, which is what the tracked layouts
 * and the frame pacing assume.
 *
 * Whatever was written to the vertex, index and storage buffers the list
 * uses, and has not reached the device yet, is copied there first in the
 * same submission (rbtrendbuffers.h). So are the uniform blocks of every
 * material the list binds that changed since they were last copied, through
 * the current frame's transient memory (rbtrendmaterial.h, "When uniform
 * values take effect").
 *
 * A list is refused if anything it was compiled against has been destroyed
 * since, by any call - a texture or view, a buffer, a sampler, a
 * pipeline, a binding set or anything one it binds holds, a material, a GPU
 * timer, a swap chain - or a texture it uses resized, a binding set it
 * binds updated (rbtrend_binding_set_update()), or the swap chain it
 * presents into resized: its barriers, copies and recorded commands name
 * what is gone. Recompile it once what it names exists again. Nothing of a
 * refused list is read (rbtrendcontext.h, "Destroying").
 *
 * A compile is submitted once. To run the same recorded commands again,
 * compile the list again and submit that: a Carrottus GPU command list is
 * submitted once for each time it is recorded (crgpu.h), and a compile is
 * one recording. The recorded commands stay until
 * rbtrend_command_list_reset(), so nothing has to be sent again. The caller
 * decides when to wait for the GPU.
 *
 * A list with a PRESENT_TEXTURE must be submitted on its swap chain's
 * queue, while the back buffer it was compiled against is still the
 * acquired one of the same back buffers (no resize since); follow it with
 * rbtrend_swap_chain_present().
 *
 * Builds of one TLAS run in the order they were sent (rbtrendraytracing.h,
 * "Instance sets"): a list holding a BUILD_TLAS is refused while another
 * list holding an earlier-sent BUILD_TLAS of the same TLAS has not been
 * submitted, the narrowest rule all three graphics APIs keep. Submit the
 * lists in the order their builds were sent.
 *
 * @param[in]  cmd_list  List to submit.
 * @param[in]  queue     Queue to submit on: the instance's one queue.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return RBT_RESULT_OK; without submitting, RBT_RESULT_INVALID_ARGUMENT if
 *         @p cmd_list or @p queue is NULL, the list hasn't compiled
 *         successfully since it was last submitted, or @p queue is not the
 *         queue the instance already submits on (or the swap chain's, for
 *         a list that presents), or the list holds a BUILD_TLAS sent after
 *         another unsubmitted list's BUILD_TLAS of the same TLAS (naming
 *         both lists); RBT_RESULT_REND_STALE if a texture it uses
 *         has changed layout since it was compiled (another list touching
 *         the same texture was submitted in between),
 *         rbtrend_instance_next_frame() has moved the frame on since it was
 *         compiled, or something it uses was destroyed or resized since it
 *         was compiled (above): compile it again and submit that;
 *         RBT_RESULT_OUT_OF_MEMORY; or another code if transient memory can't
 *         be taken for a material's block, or a Carrottus GPU list for the list's
 *         buffer copies can't be made, or the submission itself fails.
 */
RBT_RESULT rbtrend_command_list_submit(rbtrend_command_list cmd_list, crgpu_command_queue queue, RBTREND_ERROR *out_error);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // RBTREND_COMMANDS_H
