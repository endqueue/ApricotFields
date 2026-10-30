
#ifndef APREND_DRAW_LIST_H
#define APREND_DRAW_LIST_H

/**
 * @file aprenddrawlist.h
 * @brief Draw lists: draws handed over by the caller, batched by mesh and
 * material into instanced draws, and replayed into any pass that needs them.
 *
 * The caller adds what to draw each frame: a mesh, a material, and the bytes
 * of each instance of it (a transform, a color, an id - what they are is the
 * caller's). aprend_draw_list_build() sorts the draws, merges those of one
 * mesh and one material into one instanced draw, and writes every instance's
 * bytes into one span of the instance's transient memory (aprendtransient.h).
 * APREND_COMMAND_DRAW_LIST (aprendcommands.h) then records the draws into a
 * pass, as many times and into as many passes and command lists as the frame
 * needs: a depth pre-pass and the main pass, say, from one list.
 *
 * There is no scene here: no camera, no culling, no knowledge of what a mesh
 * or an instance is. What is in the list is what is drawn.
 *
 * @par Instances
 * Instance bytes are read by the pipeline as a vertex binding that steps per
 * instance (APREND_VERTEX_BINDING_DESC::_per_instance, aprendpipeline.h) at
 * the slot APREND_DRAW_LIST_DESC::_instance_slot, with the stride
 * APREND_DRAW_LIST_DESC::_instance_stride. A 4x4 matrix is four VEC4
 * elements, at four locations. A batch's first instance is its draw's
 * `_start_instance_location`, so `gl_InstanceIndex` counts from there in
 * the shader and the per-instance binding reads the batch's own instances.
 * A list with an `_instance_stride` of 0 has no instance bytes and needs no
 * transient memory: its batches are still instanced draws, told apart in
 * the shader by `gl_InstanceIndex` alone.
 *
 * @par Pipelines
 * A draw list names no pipeline: it is drawn with whatever the pass has set.
 * Draws that need different pipelines (opaque and blended, say) go in
 * different lists. Every pipeline a list is drawn with reads its vertex
 * buffer slots as the list's meshes and `_instance_slot` lay them out.
 *
 * @par A frame
 * aprend_draw_list_reset(), then aprend_draw_list_add() for each draw, then
 * aprend_draw_list_build(), then DRAW_LIST commands, then compile and
 * submit, all in one frame: the span the instances are written to is that
 * frame's (aprendtransient.h), and a list that reads it is refused in any
 * other. Adding after the build needs another build before the next
 * DRAW_LIST.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). aprend_draw_list_add() and
 * aprend_draw_list_reset() write the list and nothing else, so different
 * draw lists may be filled at once, one caller to a list, overlapping
 * other calls on the instance (both are marked). aprend_draw_list_build()
 * takes the instance's transient memory and is one of the calls made by one
 * caller at a time across the instance. Sending a DRAW_LIST reads the list:
 * several command lists may be recorded at once with one draw list, while
 * no call changes it.
 */

#include "aprendmaterial.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * @brief Opaque handle to a draw list.
 */
typedef struct aprend_draw_list_t *aprend_draw_list;

/**
 * @brief The order a draw list's draws are recorded in.
 */
typedef uint32_t APREND_DRAW_LIST_ORDER;
enum {
	/**
	 * Sorted by material and then by mesh, and every draw of one mesh and
	 * one material merged into one instanced draw, wherever it was added.
	 * Each material is bound once. Instances keep the order they were added
	 * in within their batch. For draws whose order doesn't matter: opaque
	 * geometry with depth testing.
	 *
	 * The order is the same on every run for the same draws added in the
	 * same order, whatever order the objects they name were made in: no
	 * address decides it. Materials come in the order each was first
	 * added. Within a material, meshes come grouped by their vertex buffers
	 * and then by their index buffer, each in the order it was first added
	 * (the draws of one block of a pool together, say), and then by
	 * `_first`, `_count` and `_base_vertex`, lowest first.
	 */
	APREND_DRAW_LIST_ORDER_BATCHED   = 0,
	/**
	 * In the order they were added, with only draws added one after the
	 * other of one mesh and one material merged. For draws whose order is
	 * the caller's: blended geometry, sorted back to front before it is
	 * added.
	 */
	APREND_DRAW_LIST_ORDER_SUBMITTED = 1,
};

/**
 * @brief Configuration descriptor for a draw list.
 *
 * No field has a default.
 */
typedef struct APREND_DRAW_LIST_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_DRAW_LIST_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The order draws are recorded in. */
	APREND_DRAW_LIST_ORDER _order;
	/**
	 * The bytes of one instance: the stride of the pipeline's per-instance
	 * binding. 0 for a list with no instance bytes. A multiple of 4.
	 */
	uint32_t _instance_stride;
	/**
	 * The vertex buffer slot the instance bytes are put in. Below
	 * APREND_MAX_VERTEX_BINDINGS, and past every slot a mesh of the list
	 * uses. Not read when #_instance_stride is 0.
	 */
	uint32_t _instance_slot;
} APREND_DRAW_LIST_DESC;

/**
 * @brief What one draw draws: vertex buffers, and an index buffer if it is
 * indexed, and the part of them that is the mesh.
 *
 * A value: it is copied by aprend_draw_list_add(). Two draws are of one
 * mesh when every field read below is the same. A range of a pool
 * (aprendbuffers.h) is a mesh with the pool's block as its buffer and the
 * range's start as `_first` or `_base_vertex`.
 */
/* Frozen: a value, copied by every aprend_draw_list_add(). It never changes; a new need gets a new type. */
typedef struct APREND_DRAW_MESH {
	/**
	 * The vertex buffers: `_vertex_buffers[N]` at vertex buffer slot N. The
	 * first #_vertex_buffer_count are read; none of them NULL.
	 */
	aprend_buffer _vertex_buffers[APREND_MAX_VERTEX_BINDINGS];
	/** How many vertex buffers. Not 0, and below the list's `_instance_slot` if it has instance bytes. */
	uint32_t _vertex_buffer_count;
	/** The index buffer, or NULL for a mesh that isn't indexed. */
	aprend_buffer _index_buffer;
	/** The first index, or the first vertex of a mesh that isn't indexed. */
	uint32_t _first;
	/** How many indices, or vertices. Not 0. */
	uint32_t _count;
	/** Added to each index before it picks a vertex. 0 for a mesh that isn't indexed. */
	int32_t _base_vertex;
} APREND_DRAW_MESH;

/**
 * @brief For APREND_COMMAND_DRAW_LIST's `_material_slot`: bind no
 * materials, and draw with whatever sets the pass holds.
 */
#define APREND_DRAW_LIST_NO_MATERIAL UINT32_MAX

/**
 * @brief Creates an empty draw list.
 *
 * @param[in]  instance  Instance the list's instance bytes are written to
 *                       the transient memory of.
 * @param[in]  desc      List configuration. Only read during the call.
 * @param[out] out_list  Receives the list; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p instance, @p desc or
 *         @p out_list is NULL, `_order` is not an APREND_DRAW_LIST_ORDER,
 *         `_instance_stride` is not a multiple of 4, or it is not 0 and
 *         `_instance_slot` is not below APREND_MAX_VERTEX_BINDINGS or the
 *         instance has no transient memory; the struct codes for
 *         `desc->base`; or APRESULT_OUT_OF_MEMORY.
 *
 * @see aprend_draw_list_destroy()
 */
APRESULT aprend_draw_list_create(
    aprend_instance instance,
    const APREND_DRAW_LIST_DESC *desc,
    aprend_draw_list *out_list,
    APREND_ERROR *out_error);

/**
 * @brief Destroys a draw list. The meshes and materials it names are the
 * caller's.
 *
 * Commands already sent that name it have been expanded and don't read it
 * again.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p list itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] list List to destroy. NULL is accepted and does nothing.
 */
void aprend_draw_list_destroy(aprend_draw_list list);

/**
 * @brief Empties a draw list, for the next frame. Its memory is kept.
 *
 * Thread: one caller to the list; it may overlap calls on other handles of
 * the instance, from any thread (aprendcontext.h, "Threads").
 *
 * @param[in] list List to empty. NULL is accepted and does nothing.
 */
void aprend_draw_list_reset(aprend_draw_list list);

/**
 * @brief Adds a draw: @p instance_count instances of a mesh with a
 * material.
 *
 * Copied, mesh and bytes: what the arguments point at only has to live for
 * this call. What the mesh and material name must live until the
 * submissions that draw them.
 *
 * Thread: one caller to the list; it may overlap calls on other handles of
 * the instance, from any thread, as long as the mesh's buffers and
 * @p material are not destroyed meanwhile (aprendcontext.h, "Threads").
 *
 * @param[in] list           List to add to.
 * @param[in] mesh           The mesh.
 * @param[in] material       The material, or NULL for a draw that is only
 *                           replayed with APREND_DRAW_LIST_NO_MATERIAL.
 * @param[in] instance_count How many instances. Not 0.
 * @param[in] instance_data  @p instance_count instances of the list's
 *                           `_instance_stride` bytes, tightly packed; NULL
 *                           for a list whose stride is 0.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         added, if @p list or @p mesh is NULL, @p instance_count is 0, @p
 *         instance_data is NULL for a list with instance bytes, the mesh has
 *         no vertex buffers or more than APREND_MAX_VERTEX_BINDINGS, one of
 *         them is NULL, a slot it uses is the list's `_instance_slot`, its
 *         `_count` is 0, it is not indexed and has a `_base_vertex`, its
 *         buffers or @p material are of another instance, or memory runs out.
 */
APRESULT aprend_draw_list_add(
    aprend_draw_list list,
    const APREND_DRAW_MESH *mesh,
    aprend_material material,
    uint32_t instance_count,
    const void *instance_data,
    APREND_ERROR *out_error);

/**
 * @brief Sorts and batches a draw list's draws and writes its instance bytes
 * to the current frame's transient memory, for the DRAW_LIST commands that
 * follow.
 *
 * Build again after adding more. An empty list builds, and records nothing.
 *
 * @param[in] list List to build.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), if @p list is
 *         NULL, the transient memory can't be taken
 *         (aprend_transient_allocate()), the instances number more than 32
 *         bits can count, or memory runs out. A failed build leaves the list
 *         unbuilt: a DRAW_LIST of it is refused until it builds.
 */
APRESULT aprend_draw_list_build(aprend_draw_list list, APREND_ERROR *out_error);

/**
 * @brief Reads how many draws were added to a draw list and what they were
 * batched into at its last build.
 *
 * @param[in]  list               List to read.
 * @param[out] out_draw_count     Receives the number of aprend_draw_list_add()
 *                                calls since the reset. May be NULL.
 * @param[out] out_batch_count    Receives the number of instanced draws a
 *                                DRAW_LIST records; 0 before a build. May be
 *                                NULL.
 * @param[out] out_instance_count Receives the instances of every draw. May
 *                                be NULL.
 *
 * @return APRESULT_OK; otherwise APRESULT_INVALID_ARGUMENT or the failure's
 *         own code (aprendcontext.h, "Results and errors"), with nothing
 *         written, if @p list is NULL.
 */
APRESULT aprend_draw_list_get_counts(
    aprend_draw_list list,
    uint32_t *out_draw_count,
    uint32_t *out_batch_count,
    uint32_t *out_instance_count);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_DRAW_LIST_H
