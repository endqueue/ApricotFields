#ifndef APREND_RAYTRACING_H
#define APREND_RAYTRACING_H

#include "aprendcommands.h"
#include "aprenddrawlist.h"

/**
 * @file aprendraytracing.h
 * @brief Acceleration structures for Aprend's ray-traced commands: a bottom
 * level (BLAS) for each mesh, a top level (TLAS) for each placement of
 * meshes, and the 64-byte instance records a top level is written in.
 *
 * A BLAS (bottom-level acceleration structure) is one mesh's geometry in its
 * own space: one or more ranges, each a material slot, static or deforming,
 * built once and shared by every instance of the mesh. A TLAS (top-level
 * acceleration structure) is one placement of meshes: the instances rays are
 * traced against, rebuilt when the placement changes. Both are resources,
 * at the tier of an aprend_buffer, not a manager: what is built, and when,
 * is the caller's, through commands it sends (aprendcommands.h):
 *
 * - APREND_COMMAND_BUILD_BLAS builds, refits or compacts a BLAS;
 * - APREND_COMMAND_BUILD_TLAS seals the TLAS's instance set being prepared
 *   and builds from it;
 * - APREND_COMMAND_LIT_SHADOW_MASK traces a directional light's shadow into
 *   a mask the lit scene reads (aprendlit.h,
 *   aprend_lit_scene_set_light_shadow());
 * - APREND_COMMAND_LIT_PATH_TRACE adds samples of a path-traced picture to
 *   the caller's accumulation.
 *
 * @par A device without ray queries
 * Every call here and every one of those commands needs a device created
 * with SPUDGPU_FEATURE_RAY_QUERY (aprendcontext.h,
 * APREND_INSTANCE_DESC::device); on any other each refuses with
 * APRESULT_UNSUPPORTED and a message naming the feature, before SpudGPU
 * would. There is no software traversal.
 *
 * @par Slots, not handles, in instances
 * An instance names its BLAS by the BLAS's slot (aprend_blas_get_slot()),
 * read once when the BLAS is made, never per frame: a 32-bit value whose
 * low 24 bits are an index into the instance's table of BLASes and whose
 * high 8 bits are a generation. When a BLAS is destroyed its index is free
 * for the next one, with the generation advanced, so a record holding the
 * old slot is refused, not silently another mesh. The generation is 8 bits:
 * an index reused 256 times reads as its first BLAS again, so a slot kept
 * that long past its BLAS's destruction can pass the check, as any
 * generational id can. Keep slots no longer than the BLAS they name.
 *
 * @par Instance sets
 * A TLAS holds `_max_instance_sets` sets of `_max_instances` records.
 * Writes (aprend_tlas_write_instances(), aprend_tlas_set_materials(),
 * aprend_tlas_set_instance_count()) go to the set being prepared. Sending
 * APREND_COMMAND_BUILD_TLAS seals it, so a later write changes no build
 * already sent, and the next write starts the next set. Only what is
 * written is copied: a set's records are tracked in blocks of 64, and the
 * blocks a set doesn't write are carried over from the set sealed before it
 * when its build runs, on the GPU. So the builds of one TLAS run in the
 * order they were sent, and aprend_command_list_submit() refuses a list
 * whose BUILD_TLAS was sent after another unsubmitted list's build of the
 * same TLAS. A sealed set is free again once the frame that built from it
 * has completed (aprend_instance_next_frame() has come round to it); with
 * every set sealed and in use, a write is refused with APRESULT_LIMIT.
 *
 * @par Materials
 * A record's `_material_base` is where its BLAS's geometries' materials
 * start in the TLAS's material table: geometry g of the BLAS uses entry
 * `_material_base + g`. The table holds lit materials (aprendlit.h), written
 * into each set as instances are (aprend_tlas_set_materials()), or NULL for
 * an entry only shadows see. A path trace reads every lit material's values
 * from the instance's lit-material table, where each lit material keeps one
 * entry for its life (aprend_lit_material_create()).
 *
 * @par Culling and winding
 * The same for every instance, and Aprend's: no triangle is culled by
 * facing (a mirrored instance's winding flips, and thin geometry still
 * stops a shadow ray), and the side a ray hit is found from the geometric
 * normal against the ray, not from the winding. A record carries no flags.
 *
 * @par What the steady state costs
 * A frame that writes a range of records, sends BUILD_TLAS, builds and
 * traces allocates nothing of Aprend's and touches no aprend_blas: writing
 * records copies them as they are, checking each slot against two dense
 * arrays, and the device's own instance layout is written from them on the
 * GPU at each build, where every slot finds its BLAS's current structure.
 * What allocates is said where it does: creating a BLAS or a TLAS, a
 * compaction's second structure, a BUILD of a compacted BLAS, the
 * lit-material table's growth, and the build scratch's growth.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A BLAS and a TLAS are each
 * used by one caller at a time with everything else of their instance, as a
 * buffer is: through their own calls and through the command lists that
 * build and read them. Creating and destroying a BLAS, and creating a lit
 * material, take a slot from a table of the instance, and writing records
 * and materials reads those tables, so they are calls on the instance too.
 * No call here is an exception, the get_ calls included: what they read
 * changes as builds complete. A Debug build checks each call on the BLAS or
 * TLAS it takes, and on the instance where it reads or writes the
 * instance's tables (aprendcontext.h, "Threads").
 */

#if __cplusplus
extern "C" {
#endif // __cplusplus

/* ---- Handles ------------------------------------------------------------ */

/**
 * @brief Opaque handle to a bottom-level acceleration structure (BLAS): one
 * mesh's geometry, in its own space.
 */
typedef struct aprend_blas_t *aprend_blas;

/**
 * @brief Opaque handle to a top-level acceleration structure (TLAS): one
 * placement of meshes, the instances rays are traced against.
 */
typedef struct aprend_tlas_t *aprend_tlas;

/* ---- BLAS --------------------------------------------------------------- */

/**
 * @brief What a BLAS is made from: its geometries, and whether it deforms.
 *
 * Each geometry is a mesh's buffers and range as a draw names them
 * (APREND_DRAW_MESH, aprenddrawlist.h), and one material slot: geometry g
 * of the BLAS uses entry `_material_base + g` of a TLAS's material table.
 * Its vertices are APREND_LIT_VERTEX (aprendlit.h), 48 bytes apart in
 * `_vertex_buffers[0]` (the other vertex buffers are not read), the
 * position each one's first three floats; its indices are 32-bit, from
 * `_index_buffer`, or none (three vertices a triangle, in order). The
 * mesh's `_first`, `_count` and `_base_vertex` are read as a draw reads
 * them. A path trace reads normals and texture coordinates at a hit:
 * each build and refit copies the BLAS's vertices and indices into the
 * instance's geometry arena, where a path trace reads them, a range the
 * BLAS takes at creation for its life (the arena grows by doubling when it
 * is full, an allocation then and only then).
 *
 * Geometry is opaque to rays in this version: a cut-out material's
 * `alpha_cutoff` is not yet read by a ray.
 */
typedef struct APREND_BLAS_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_BLAS_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * Its geometries, #_geometry_count of them, one per material slot.
	 * Copied by aprend_blas_create(); the buffers they name are borrowed,
	 * and outlive every build of the BLAS and every path trace of a TLAS
	 * built over it.
	 */
	const APREND_DRAW_MESH *_geometries;
	/** How many geometries: 1 to the device's limit. */
	uint32_t _geometry_count;
	/**
	 * Whether the geometry is rewritten while the BLAS lives. A deforming
	 * BLAS is built for fast building and refitting and refitted
	 * (APREND_BUILD_MODE_REFIT) after each rewrite, rebuilt
	 * (APREND_BUILD_MODE_BUILD) when the caller wants its quality back, and
	 * never compacted. A static one is built for fast tracing, and may be
	 * compacted once (APREND_BUILD_MODE_COMPACT).
	 */
	bool _deforms;
	/** A name for diagnostics, or NULL. Copied. */
	const char *debug_name;
} APREND_BLAS_DESC;

/**
 * @brief Creates a BLAS, not yet built, and takes its slot in the
 * instance's table of BLASes.
 *
 * Nothing is built here: APREND_COMMAND_BUILD_BLAS with
 * APREND_BUILD_MODE_BUILD builds it. Allocates the handle, the geometries'
 * copy and the structure's storage, at the size the device reports for the
 * geometries; the table of BLASes grows by doubling when it is full, an
 * allocation then and only then.
 *
 * @param[in]  instance  Instance the BLAS is of. Its device has
 *                       SPUDGPU_FEATURE_RAY_QUERY.
 * @param[in]  desc      The geometries. Only read during the call.
 * @param[out] out_blas  Receives the BLAS; NULL on failure, with no slot
 *                       taken.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, checked in this order, with nothing made:
 *         APRESULT_INVALID_ARGUMENT if @p instance is NULL; the struct codes
 *         for `desc->base` (with @p desc NULL, APRESULT_INVALID_ARGUMENT);
 *         APRESULT_INVALID_ARGUMENT if @p out_blas is NULL;
 *         APRESULT_UNSUPPORTED on a device without SPUDGPU_FEATURE_RAY_QUERY;
 *         APRESULT_INVALID_ARGUMENT if `_geometries` is NULL or
 *         `_geometry_count` is 0; APRESULT_LIMIT if `_geometry_count` is past
 *         the device's limit; APRESULT_INVALID_ARGUMENT, naming the geometry,
 *         if a buffer of one is NULL or of another instance, its first
 *         vertex buffer lacks APREND_BUFFER_USAGE_VERTEX or its index buffer
 *         APREND_BUFFER_USAGE_INDEX or 32-bit indices, a buffer is one of
 *         per-frame copies, its count is 0 or not a multiple of 3, or its
 *         range runs past its buffers; APRESULT_LIMIT if the
 *         triangles summed are past the device's limit; then
 *         APRESULT_OUT_OF_MEMORY, or the code for SpudGPU's result where the
 *         structure can't be created.
 *
 * @see aprend_blas_destroy()
 */
APRESULT aprend_blas_create(aprend_instance instance, const APREND_BLAS_DESC *desc, aprend_blas *out_blas,
                            APREND_ERROR *out_error);

/**
 * @brief Destroys a BLAS and frees its slot.
 *
 * The GPU doesn't have to be finished with it (aprendcontext.h,
 * "Destroying"): its structures are released once every submission made so
 * far has completed. Its slot's index is free for the next BLAS at once,
 * with the generation advanced, so a record still holding the old slot is
 * refused by aprend_tlas_write_instances() and by the seal of a set that
 * names it.
 *
 * @warning A BLAS outlives every instance set that names it until the
 * builds from that set have completed: destroying it while a set being
 * prepared still names it makes that set's BUILD_TLAS refused at its send.
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p blas itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] blas BLAS to destroy. NULL is accepted and does nothing.
 */
void aprend_blas_destroy(aprend_blas blas);

/**
 * @brief The BLAS's slot, what an APREND_RAY_INSTANCE names it by
 * (`_blas`): bits 0 to 23 its index in the instance's table of BLASes,
 * bits 24 to 31 its generation. Constant for the BLAS's life, through every
 * build and compaction. Read it once, when the BLAS is made.
 *
 * @param[in] blas The BLAS.
 *
 * @return The slot; 0, which is never a slot, if @p blas is NULL.
 */
uint32_t aprend_blas_get_slot(aprend_blas blas);

/**
 * @brief What a BLAS holds.
 */
typedef struct APREND_BLAS_INFO {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_BLAS_INFO, set by the caller (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE_OUT base;
	/** GPU bytes of its structures now: both while a compaction's frame
	 * runs, one after. */
	uint64_t _storage_bytes;
	/** The size its structure compacts to, once a build of a static BLAS has
	 * completed and the size has been read back; 0 before, and always for a
	 * deforming one. */
	uint64_t _compacted_bytes;
	/** A build has completed. */
	bool _built;
	/** Its storage is the compacted structure. */
	bool _compacted;
} APREND_BLAS_INFO;

/**
 * @brief Reports what a BLAS holds: its sizes, and whether it is built and
 * compacted.
 *
 * Compaction is the caller's to start: once `_compacted_bytes` is not 0,
 * APREND_COMMAND_BUILD_BLAS with APREND_BUILD_MODE_COMPACT compacts it.
 *
 * @param[in]  blas      BLAS to read.
 * @param[out] out_info  Receives the report; its `base.type` is set by the
 *                       caller, as for every struct Aprend fills in.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, with nothing written:
 *         APRESULT_INVALID_ARGUMENT if @p blas is NULL; the struct codes for
 *         `out_info->base` (with @p out_info NULL,
 *         APRESULT_INVALID_ARGUMENT).
 */
APRESULT aprend_blas_get_info(aprend_blas blas, APREND_BLAS_INFO *out_info, APREND_ERROR *out_error);

/* ---- TLAS --------------------------------------------------------------- */

/**
 * @brief Creates a TLAS: its structure, its instance sets and their
 * material tables, sized here once.
 */
typedef struct APREND_TLAS_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_TLAS_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The most instances a set holds: not 0, and within the device's limit. */
	uint32_t _max_instances;
	/** The length of each set's material table: not 0. */
	uint32_t _max_materials;
	/**
	 * How many instance sets: at least 2. A set is busy from the send of the
	 * BUILD_TLAS that seals it until the frame that built from it has
	 * completed, so one build a frame needs the instance's
	 * `frames_in_flight` + 1.
	 */
	uint32_t _max_instance_sets;
	/** A name for diagnostics, or NULL. Copied. */
	const char *debug_name;
} APREND_TLAS_DESC;

/**
 * @brief Creates a TLAS, with no instances (`_instance_count` 0) and every
 * material table entry NULL.
 *
 * Allocates the handle, `_max_instance_sets` sets of `_max_instances`
 * records and `_max_materials` table entries each, their written-block
 * bitsets (`_max_instances` / 64 bits a set, rounded up), the bitset of
 * BLAS slots each set names, the structure's storage at the size the device
 * reports for `_max_instances`, the buffer the device's instance layout is
 * written into, and the emitter table a path trace builds. Nothing a set
 * holds is allocated again in its life.
 *
 * @param[in]  instance  Instance the TLAS is of. Its device has
 *                       SPUDGPU_FEATURE_RAY_QUERY.
 * @param[in]  desc      Its sizes. Only read during the call.
 * @param[out] out_tlas  Receives the TLAS; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, checked in this order, with nothing made:
 *         APRESULT_INVALID_ARGUMENT if @p instance is NULL; the struct codes
 *         for `desc->base` (with @p desc NULL, APRESULT_INVALID_ARGUMENT);
 *         APRESULT_INVALID_ARGUMENT if @p out_tlas is NULL;
 *         APRESULT_UNSUPPORTED on a device without SPUDGPU_FEATURE_RAY_QUERY;
 *         APRESULT_INVALID_ARGUMENT if `_max_instances` is 0; APRESULT_LIMIT
 *         if it is past the device's limit; APRESULT_INVALID_ARGUMENT if
 *         `_max_materials` is 0 or `_max_instance_sets` is below 2;
 *         APRESULT_LIMIT if the sets' sizes overflow 64 bits; then
 *         APRESULT_OUT_OF_MEMORY, or the code for SpudGPU's result where a
 *         buffer or the structure can't be created.
 *
 * @see aprend_tlas_destroy()
 */
APRESULT aprend_tlas_create(aprend_instance instance, const APREND_TLAS_DESC *desc, aprend_tlas *out_tlas,
                            APREND_ERROR *out_error);

/**
 * @brief Destroys a TLAS: its structure, sets and tables.
 *
 * The GPU doesn't have to be finished with it (aprendcontext.h,
 * "Destroying"); a command list compiled with it and not yet submitted is
 * refused at submit (aprendcommands.h).
 *
 * Thread: may overlap calls on other handles of its instance, from any
 * thread; no call may be using @p tlas itself meanwhile (aprendcontext.h,
 * "Threads" and "Destroying").
 *
 * @param[in] tlas TLAS to destroy. NULL is accepted and does nothing.
 */
void aprend_tlas_destroy(aprend_tlas tlas);

/**
 * @brief One instance of a TLAS: 64 bytes, one cache line.
 *
 * The transform is in the form every device's instances take (DXR's
 * D3D12_RAYTRACING_INSTANCE_DESC::Transform, Vulkan's
 * VkTransformMatrixKHR); the rest is Aprend's, translated on the GPU at each
 * build into the device's own layout. From a column-major 4x4 `m`
 * (`m[column][row]`, as APREND_LIT_INSTANCE has it), row r of
 * `_transform` is (`m[0][r]`, `m[1][r]`, `m[2][r]`, `m[3][r]`) for r = 0, 1,
 * 2: its last row dropped, once, where the caller makes the transform.
 */
/* Frozen: an array element of aprend_tlas_write_instances(); its transform in the devices' order, the rest Aprend's. */
typedef struct APREND_RAY_INSTANCE {
	/** Object to world, row-major 3x4: rows of (x, y, z, translation). */
	float _transform[12];
	/** aprend_blas_get_slot() of a BLAS of the same instance. */
	uint32_t _blas;
	/** Below 2^24: geometry g of the BLAS uses the TLAS's material table's
	 * entry `_material_base + g`. */
	uint32_t _material_base;
	/** Which rays see it: its low 8 bits, against each ray command's
	 * `_cull_mask`, the instance skipped where the two share no bit. The
	 * bits above are 0. */
	uint32_t _mask;
	/** 0. */
	uint32_t _reserved;
} APREND_RAY_INSTANCE;

/**
 * @brief Copies @p count records into the set being prepared, from
 * instance @p first, as they are.
 *
 * Each record's slot is checked against the instance's table of BLASes
 * (it names a live BLAS: its index is in use and its generation is the
 * BLAS's), and the blocks of 64 records the range touches are marked
 * written; nothing else is done per record. Blocks the set doesn't write
 * carry over from the set sealed before it, at its build. A record past
 * the set's `_instance_count` is kept but not built until the count reaches
 * it.
 *
 * @param[in]  tlas      TLAS whose set being prepared is written.
 * @param[in]  first     The first instance written.
 * @param[in]  instances @p count records. Only read during the call.
 * @param[in]  count     1 or more; `first + count` at most `_max_instances`.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, checked in this order, with no record
 *         written and no block marked: APRESULT_INVALID_ARGUMENT if @p tlas
 *         is NULL; APRESULT_UNSUPPORTED on a device without
 *         SPUDGPU_FEATURE_RAY_QUERY; APRESULT_INVALID_ARGUMENT if
 *         @p instances is NULL or @p count is 0; APRESULT_LIMIT if
 *         `first + count` is past `_max_instances`; APRESULT_LIMIT, naming
 *         the count, if every set is sealed and in use;
 *         APRESULT_INVALID_ARGUMENT, naming the record (its index in
 *         @p instances), for a slot that names no live BLAS, a
 *         `_material_base` of 2^24 or more, a `_mask` above 255, or a
 *         `_reserved` that is not 0. The transform is copied unread: one
 *         that is not finite gives undefined hits for its instance.
 */
APRESULT aprend_tlas_write_instances(aprend_tlas tlas, uint32_t first, const APREND_RAY_INSTANCE *instances,
                                     uint32_t count, APREND_ERROR *out_error);

/**
 * @brief Sets how many instances the sets hold, from the set being prepared
 * on: the first @p count records are built, the rest left out. It stays
 * until set again.
 *
 * @param[in]  tlas      TLAS to change.
 * @param[in]  count     0 to `_max_instances`.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, with nothing changed:
 *         APRESULT_INVALID_ARGUMENT if @p tlas is NULL; APRESULT_UNSUPPORTED
 *         on a device without SPUDGPU_FEATURE_RAY_QUERY; APRESULT_LIMIT if
 *         @p count is past `_max_instances`; APRESULT_LIMIT, naming the
 *         count, if every set is sealed and in use.
 */
APRESULT aprend_tlas_set_instance_count(aprend_tlas tlas, uint32_t count, APREND_ERROR *out_error);

/**
 * @brief Writes @p count entries of the material table, from @p first,
 * into the set being prepared: each a lit material (aprendlit.h), read into
 * its slot of the instance's lit-material table at the call, or NULL for an
 * entry only shadows see.
 *
 * Carried over by block when not written, as records are. Seen by the
 * BUILD_TLAS that seals the set and every trace of it. A path trace reads a
 * material's values as they are at the trace, from the lit-material table;
 * the TLAS holds only slots, so changing a material's values is not a call
 * here.
 *
 * @warning A NULL entry is one only shadows see: a path trace's
 * `_cull_mask` leaves out every instance whose geometries reach one
 * (APREND_COMMAND_LIT_PATH_TRACE).
 *
 * @param[in]  tlas      TLAS whose set being prepared is written.
 * @param[in]  first     The first entry written.
 * @param[in]  materials @p count lit materials or NULLs. Only read during
 *                       the call.
 * @param[in]  count     1 or more; `first + count` at most `_max_materials`.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, checked in this order, with nothing
 *         written: APRESULT_INVALID_ARGUMENT if @p tlas is NULL;
 *         APRESULT_UNSUPPORTED on a device without
 *         SPUDGPU_FEATURE_RAY_QUERY; APRESULT_INVALID_ARGUMENT if
 *         @p materials is NULL or @p count is 0; APRESULT_LIMIT if
 *         `first + count` is past `_max_materials`; APRESULT_LIMIT, naming
 *         the count, if every set is sealed and in use;
 *         APRESULT_INVALID_ARGUMENT, naming the entry, for a material of
 *         another instance or one that is not a lit material.
 */
APRESULT aprend_tlas_set_materials(aprend_tlas tlas, uint32_t first, const aprend_material *materials, uint32_t count,
                                   APREND_ERROR *out_error);

/**
 * @brief What a TLAS holds.
 */
typedef struct APREND_TLAS_INFO {
	/** This struct's type, APRI_STRUCT_TYPE_APREND_TLAS_INFO, set by the caller (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE_OUT base;
	/** GPU bytes: the structure, its sets and their tables, the device's
	 * instance layout and its emitter table. */
	uint64_t _storage_bytes;
	/** Sealed sets whose builds have not completed. */
	uint32_t _sets_in_use;
} APREND_TLAS_INFO;

/**
 * @brief Reports what a TLAS holds: its GPU bytes and how many sets are in
 * use.
 *
 * @param[in]  tlas      TLAS to read.
 * @param[out] out_info  Receives the report; its `base.type` is set by the
 *                       caller.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK. Otherwise, with nothing written:
 *         APRESULT_INVALID_ARGUMENT if @p tlas is NULL; the struct codes for
 *         `out_info->base` (with @p out_info NULL,
 *         APRESULT_INVALID_ARGUMENT).
 */
APRESULT aprend_tlas_get_info(aprend_tlas tlas, APREND_TLAS_INFO *out_info, APREND_ERROR *out_error);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_RAYTRACING_H
