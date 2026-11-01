/**
 * @file apmathbvh.h
 * @brief ApricotMath's ray casting - which triangle a ray hits first, among
 * many meshes, in double precision.
 *
 * Built in src/math/apmathbvh.cpp, with headless tests in
 * tests/apmathbvh_tests.c. It is not part of
 * aprend.h, which is Aprend's alone.
 *
 * Two levels of bounding volume hierarchy (BVH), so a cast costs about the
 * logarithm of the triangle count and of the mesh count rather than their
 * sum:
 *
 * - mesh BVH: over one mesh's triangles. Built once per mesh.
 * - instance BVH: over the bounds of many instances - each a mesh BVH
 *   placed by a transform, with an id and a mask that are the caller's.
 *   Built once per set of instances; a mesh used a thousand times is built
 *   once and instanced.
 *
 * Beside casts, a mesh can be tested against a convex volume - a set of
 * planes, such as a camera's frustum or a box: whether any of it, or all of
 * it, lies inside (apmath_mesh_bvh_overlaps_volume(),
 * apmath_mesh_bvh_within_volume()), with apmath_aabb_test_volume() as the
 * quick test on bounds before it.
 *
 * Both are immutable once created: a change is a new BVH. Rebuilding an
 * instance BVH doesn't rebuild its meshes, so changing one mesh among many
 * costs that mesh's build plus a build over boxes.
 *
 * General purpose: it knows nothing about what a mesh is or why a ray is
 * cast. What an instance means is the caller's, through its id; which
 * instances a cast considers is the caller's, through masks and a filter
 * callback.
 *
 * Out of scope: curved primitives, motion, refitting a BVH in place, and
 * anything GPU - this is CPU geometry, unrelated to Aprend.
 *
 * @par Threads
 * apricore.h's rule ("Threads"), with one exception: a BVH never changes
 * once created, so the casts, volume tests and get_ calls on one BVH may
 * overlap each other, from any number of threads, and never block or
 * allocate; the casts and tests are marked. Destroying a BVH may overlap none of them, nor the
 * creation of an instance BVH over it, which only reads it. A create call
 * takes no other object, so creates may run at once. A filter callback runs
 * inside the cast, on the thread that made it.
 *
 * @par Results
 * Calls return APRESULT (apricore.h). The general codes mean, here:
 * - APRESULT_INVALID_ARGUMENT: also a position or transform that isn't
 *   finite, an index past the positions, a count that isn't a multiple of
 *   three, a transform that can't be inverted or whose last row isn't
 *   0, 0, 0, 1.
 * - APRESULT_OUT_OF_MEMORY: only when creating.
 *
 * @par
 * Casts can't fail: they return whether, or how many times, the ray hit. A
 * cast given a NULL BVH, a NULL ray or a ray that isn't finite hits nothing.
 * Volume tests can't fail either: given arguments they can't use, they
 * answer as for a mesh that isn't there.
 *
 * @par Struct bases
 * Descs start with a `base` (apricore.h, "Struct bases"): the caller sets
 * `base.type` to the desc's own APRI_STRUCT_TYPE_APMATH_* and `base.next` to
 * NULL. A create given a wrong type returns APRESULT_STRUCT_WRONG_TYPE, and
 * one given a chained struct APRESULT_STRUCT_UNKNOWN_IN_CHAIN: nothing
 * chains onto any of them yet. A cast given either hits nothing.
 * APMATH_AABB, APMATH_RAY, APMATH_PLANE, APMATH_VOLUME, APMATH_BVH_INSTANCE
 * and the hit structs are frozen: they never grow (a new need gets a new type).
 *
 * @par Geometry
 * Right-handed coordinates; a triangle's front is the side its points run
 * counter-clockwise from. A triangle with no area is never hit. Rays hit
 * both sides of a triangle.
 */

#ifndef APMATHBVH_H
#define APMATHBVH_H

#include "../apricore.h"
#include "../aprimath.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/* ---- Rays and boxes -------------------------------------------------- */

/**
 * @brief An axis-aligned box. Empty if any of `lo` is above its `hi`.
 */
/* Frozen: a value. It never changes; a new need gets a new type. */
typedef struct APMATH_AABB {
	/** The low corner: the smallest x, y and z inside. */
	ApriDVec3 lo;
	/** The high corner: the largest x, y and z inside. */
	ApriDVec3 hi;
} APMATH_AABB;

/**
 * @brief The points `origin` + t * `direction` for t from `t_min` to `t_max`,
 * both included.
 *
 * `direction` needn't be unit length: t counts in its lengths, so with a
 * unit direction it's a distance. `t_max` may be INFINITY for a ray with no
 * far end; `t_min` may be negative. A zero `direction`, or a `t_min` above
 * `t_max`, hits nothing.
 */
/* Frozen: a value, passed on every cast. It never changes; a new need gets a new type. */
typedef struct APMATH_RAY {
	/** The point at t = 0. */
	ApriDVec3 origin;
	/** How far the ray moves per unit of t. A zero direction hits nothing. */
	ApriDVec3 direction;
	/** The nearest t the ray covers. May be negative. */
	double t_min;
	/** The furthest t the ray covers. May be INFINITY. */
	double t_max;
} APMATH_RAY;

/**
 * @brief Clips a ray to a box.
 *
 * Narrows @p ray's `t_min` and `t_max` to the part of it inside @p box. A
 * box side may be infinite: a slab between two planes is a box infinite
 * along the other two axes.
 *
 * @param[in,out] ray The ray to narrow. Left as it was if none of it is
 *                    inside @p box.
 * @param[in]     box The box to clip to.
 *
 * @return true if part of @p ray is inside @p box, false if none of it is.
 */
bool apmath_ray_clip_aabb(APMATH_RAY *ray, const APMATH_AABB *box);

/**
 * @brief A plane, and the side of it that is inside: the points p with
 * dot(`normal`, p) + `d` >= 0.
 *
 * `normal` needn't be unit length; it mustn't be zero. Points on the plane
 * are inside.
 */
/* Frozen: a value, an array element. It never changes; a new need gets a new type. */
typedef struct APMATH_PLANE {
	/** Points into the inside. Not zero. */
	ApriDVec3 normal;
	/** The plane's offset: dot(`normal`, p) + `d` is 0 on it. */
	double d;
} APMATH_PLANE;

/**
 * @brief A convex volume - the points inside every one of `planes` - and
 * the part of what is tested that counts: what lies inside every one of
 * `clip_planes`.
 *
 * Tested against a mesh, the mesh is first cut down to its part inside the
 * clip planes, and that part is what lies inside, outside or across the
 * volume. With no clip planes the whole mesh counts. A clip plane only
 * cuts: what it removes is neither inside nor outside, so a mesh entirely
 * clipped away overlaps nothing and is within nothing.
 *
 * Neither array may hold a plane with a zero or non-finite normal, or a
 * non-finite `d`, and the two may hold at most 32 planes between them; a
 * test given one that breaks this answers as for a mesh that isn't there
 * (APMATH_OVERLAP_NONE, false). No planes at all is all of space.
 */
/* Frozen: a value, passed on every test. It never changes; a new need gets a new type. */
typedef struct APMATH_VOLUME {
	/** The volume's planes. Only read during the call. NULL when #plane_count is 0. */
	const APMATH_PLANE *planes;
	/** How many #planes holds. */
	uint32_t plane_count;
	/** Planes the mesh is cut by first. Only read during the call. NULL when #clip_plane_count is 0. */
	const APMATH_PLANE *clip_planes;
	/** How many #clip_planes holds. */
	uint32_t clip_plane_count;
} APMATH_VOLUME;

/**
 * @brief How much of something lies in a volume.
 */
typedef enum APMATH_OVERLAP APRI_ENUM_BASE {
	/** None of it. */
	APMATH_OVERLAP_NONE = 0,
	/** Some of it, or, for a box, maybe some: see apmath_aabb_test_volume(). */
	APMATH_OVERLAP_PARTIAL = 1,
	/** All of it - all of what the clip planes keep, and that isn't empty. */
	APMATH_OVERLAP_WHOLE = 2,
} APMATH_OVERLAP;

/**
 * @brief Tests a box against a volume, from its corners: the quick test
 * before a mesh's.
 *
 * NONE and WHOLE are exact: NONE when the box lies outside one of the
 * volume's planes or one of its clip planes (nothing of a mesh inside it
 * can count), WHOLE when it lies inside every plane of both (everything
 * inside it is in the volume). Otherwise PARTIAL, which only says the box
 * straddles a plane: the mesh inside it needs its own test.
 *
 * Thread: any - it reads only its arguments.
 *
 * @param[in] box       The box. An empty one is NONE.
 * @param[in] transform Box coordinates to the volume's, as an instance's
 *                      (APMATH_BVH_INSTANCE::transform); NULL for none. The
 *                      box is placed by it, and tested by its eight corners.
 * @param[in] volume    The volume, in its own coordinates.
 *
 * @return NONE, PARTIAL or WHOLE as above. NONE for a NULL @p box or
 *         @p volume, a box that isn't finite, a transform that isn't finite
 *         or can't be inverted or whose last row isn't 0, 0, 0, 1, or a bad
 *         plane.
 */
APMATH_OVERLAP apmath_aabb_test_volume(const APMATH_AABB *box, const ApriDMat4 *transform,
                                       const APMATH_VOLUME *volume);

/* ---- Mesh BVH -------------------------------------------------------- */

/**
 * @brief Opaque handle to a mesh BVH: one mesh's triangles, immutable once
 * created.
 */
typedef struct apmath_mesh_bvh_t *apmath_mesh_bvh;

/**
 * @brief Configuration descriptor for a mesh BVH.
 */
typedef struct APMATH_MESH_BVH_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * The mesh's points, in its own coordinates. Copied: the array needn't
	 * outlive the call.
	 */
	const ApriDVec3 *positions;
	/** How many points #positions holds. */
	uint32_t position_count;
	/**
	 * Three per triangle, each a position's index. NULL for a mesh with no
	 * index list: then every three positions are a triangle, in order, and
	 * #position_count must be a multiple of three.
	 */
	const uint32_t *indices;
	/** A multiple of three; 0 when #indices is NULL. */
	uint32_t index_count;
} APMATH_MESH_BVH_DESC;

/**
 * @brief Builds a mesh BVH.
 *
 * A mesh with no triangles is valid, and never hit.
 *
 * @param[in]  desc    The mesh. Only read during the call.
 * @param[out] out_bvh The BVH, or NULL on failure.
 *
 * @return APRESULT_OK, or a failure as in "Results" above.
 *
 * @see apmath_mesh_bvh_destroy()
 */
APRESULT apmath_mesh_bvh_create(const APMATH_MESH_BVH_DESC *desc, apmath_mesh_bvh *out_bvh);
/**
 * @brief Destroys a mesh BVH.
 *
 * @warning Destroy a mesh only after every instance BVH holding it.
 *
 * @param[in] bvh The BVH to destroy. NULL is fine.
 */
void apmath_mesh_bvh_destroy(apmath_mesh_bvh bvh);

/**
 * @brief Counts a mesh BVH's triangles.
 *
 * @param[in] bvh The BVH.
 *
 * @return How many triangles it was built from, or 0 if @p bvh is NULL.
 */
uint32_t apmath_mesh_bvh_get_triangle_count(apmath_mesh_bvh bvh);
/**
 * @brief Reads a mesh BVH's bounds.
 *
 * @param[in] bvh The BVH.
 *
 * @return The box around its triangles, in the mesh's coordinates. Empty
 *         for a mesh with no triangles, and for a NULL @p bvh.
 */
APMATH_AABB apmath_mesh_bvh_get_bounds(apmath_mesh_bvh bvh);

/**
 * @brief Where a ray hit a mesh, in the mesh's coordinates.
 */
/* Frozen: filled on every cast. It never changes; a new need gets a new type. */
typedef struct APMATH_MESH_HIT {
	/** The triangle's position in the desc's order - never the BVH's own. */
	uint32_t triangle;
	/**
	 * True if the ray hit the triangle's back - e.g. leaving a closed solid
	 * it started inside.
	 */
	bool back_face;
	/** The ray's t at the hit (APMATH_RAY). */
	double t;
	/** The point hit. */
	ApriDVec3 point;
	/** Unit, out of the triangle's front - whichever side was hit. */
	ApriDVec3 normal;
} APMATH_MESH_HIT;

/**
 * @brief Casts a ray against a mesh BVH.
 *
 * Finds the hit with the smallest t. Of triangles hit at the same t, the
 * one earliest in the desc's order - so a cast is repeatable.
 *
 * Thread: may overlap any other cast or get_ call on @p bvh, from any
 * thread; not its destroy.
 *
 * @param[in]  bvh     The mesh.
 * @param[in]  ray     The ray, in the mesh's coordinates.
 * @param[out] out_hit The hit. May be NULL, to ask only whether the ray
 *                     hits; it's left alone on a miss.
 *
 * @return true if the ray hits the mesh.
 */
bool apmath_mesh_bvh_cast(apmath_mesh_bvh bvh, const APMATH_RAY *ray, APMATH_MESH_HIT *out_hit);

/**
 * @brief Whether any part of a mesh lies in a volume: a triangle, cut down to
 * the clip planes, with a point inside the volume or on its boundary.
 *
 * Exact against the triangles, in double precision, walking the BVH: a node
 * outside a plane is skipped, a node inside every plane answers at once.
 * Stops at the first triangle found. A triangle with no area counts, as a
 * segment or a point. Only the mesh's surface is tested: a volume wholly
 * inside a closed mesh, touching none of its triangles, is not found.
 *
 * Thread: may overlap any other test, cast or get_ call on @p bvh, from any
 * thread; not its destroy. Allocates nothing.
 *
 * @param[in] bvh       The mesh. NULL is a mesh with no triangles.
 * @param[in] transform Mesh coordinates to the volume's, as an instance's
 *                      (APMATH_BVH_INSTANCE::transform); NULL for none.
 * @param[in] volume    The volume, in its own coordinates.
 *
 * @return true if some part of the mesh the clip planes keep is in the
 *         volume. false for a NULL @p volume, a transform that isn't finite
 *         or can't be inverted or whose last row isn't 0, 0, 0, 1, or a bad
 *         plane.
 */
bool apmath_mesh_bvh_overlaps_volume(apmath_mesh_bvh bvh, const ApriDMat4 *transform, const APMATH_VOLUME *volume);

/**
 * @brief Whether all of a mesh lies in a volume: every triangle, cut down to
 * the clip planes, inside the volume or on its boundary, and at least one
 * left after the cut.
 *
 * Exact, as apmath_mesh_bvh_overlaps_volume(). A node inside every plane
 * passes without its triangles being read; a node wholly clipped away is
 * skipped. Stops at the first point found outside.
 *
 * Thread: as apmath_mesh_bvh_overlaps_volume(). Allocates nothing.
 *
 * @param[in] bvh       The mesh. NULL is a mesh with no triangles.
 * @param[in] transform As apmath_mesh_bvh_overlaps_volume().
 * @param[in] volume    The volume, in its own coordinates.
 *
 * @return true if the part of the mesh the clip planes keep is not empty and
 *         lies wholly in the volume. false for a mesh with no triangles, or
 *         none left by the clip planes, and in every failure case
 *         apmath_mesh_bvh_overlaps_volume() names.
 */
bool apmath_mesh_bvh_within_volume(apmath_mesh_bvh bvh, const ApriDMat4 *transform, const APMATH_VOLUME *volume);

/* ---- Instance BVH ---------------------------------------------------- */

/**
 * @brief Opaque handle to an instance BVH: placements of mesh BVHs,
 * immutable once created.
 */
typedef struct apmath_instance_bvh_t *apmath_instance_bvh;

/**
 * @brief One placement of a mesh.
 */
/* Frozen: an array element. It never changes; a new need gets a new type. */
typedef struct APMATH_BVH_INSTANCE {
	/**
	 * Not owned, and not copied: it must outlive the instance BVH. NULL is
	 * an instance that's never hit.
	 */
	apmath_mesh_bvh mesh;
	/**
	 * Mesh coordinates to the instance BVH's: any transform that can be
	 * inverted and keeps straight lines straight and parallel ones parallel
	 * (rotation, translation, scale, shear, mirror) - its last row is
	 * 0, 0, 0, 1. Copied. NULL places the mesh as it is.
	 */
	const ApriDMat4 *transform;
	/** The caller's, handed back in hits and to the filter. Needn't be unique. */
	uint64_t id;
	/**
	 * Which kinds of cast consider the instance - see APMATH_CAST_DESC. An
	 * instance with no bits set is only hit by casts that pass no desc.
	 */
	uint32_t mask;
} APMATH_BVH_INSTANCE;

/**
 * @brief Configuration descriptor for an instance BVH.
 */
typedef struct APMATH_INSTANCE_BVH_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APMATH_INSTANCE_BVH_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The instances. Copied. */
	const APMATH_BVH_INSTANCE *instances;
	/** How many #instances holds. */
	uint32_t instance_count;
} APMATH_INSTANCE_BVH_DESC;

/**
 * @brief Builds an instance BVH.
 *
 * No instances is valid, and never hit.
 *
 * @param[in]  desc    The instances. Only read during the call.
 * @param[out] out_bvh The BVH, or NULL on failure.
 *
 * @return APRESULT_OK, or a failure as in "Results" above.
 *
 * @see apmath_instance_bvh_destroy()
 */
APRESULT apmath_instance_bvh_create(const APMATH_INSTANCE_BVH_DESC *desc, apmath_instance_bvh *out_bvh);
/**
 * @brief Destroys an instance BVH. Leaves the meshes alone.
 *
 * @param[in] bvh The BVH to destroy. NULL is fine.
 */
void apmath_instance_bvh_destroy(apmath_instance_bvh bvh);

/**
 * @brief Counts an instance BVH's instances.
 *
 * @param[in] bvh The BVH.
 *
 * @return How many instances it was built from, or 0 if @p bvh is NULL.
 */
uint32_t apmath_instance_bvh_get_instance_count(apmath_instance_bvh bvh);
/**
 * @brief Reads an instance BVH's bounds.
 *
 * @param[in] bvh The BVH.
 *
 * @return The box around every instance, in the instance BVH's
 *         coordinates. Empty if there are none with triangles, and for a
 *         NULL @p bvh.
 */
APMATH_AABB apmath_instance_bvh_get_bounds(apmath_instance_bvh bvh);

/**
 * @brief Decides whether a cast may hit an instance.
 *
 * Called for instances whose bounds the ray enters and whose mask matched,
 * at most once per instance per cast, in no particular order, on the thread
 * that called the cast.
 *
 * @warning It must not create or destroy BVHs.
 *
 * @param[in] user     APMATH_CAST_DESC's `user`, unchanged.
 * @param[in] instance The instance's position in the desc's order.
 * @param[in] id       The instance's id.
 *
 * @return true to let the ray hit the instance, false to pass through it as
 *         if it weren't there.
 */
typedef bool (*APMATH_CAST_FILTER)(void *user, uint32_t instance, uint64_t id);

/**
 * @brief Which instances a cast considers. A NULL desc considers them all.
 */
typedef struct APMATH_CAST_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APMATH_CAST_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** Only instances sharing a bit with #mask are considered. */
	uint32_t mask;
	/** Asked about each of those the ray might hit. NULL lets them all through. */
	APMATH_CAST_FILTER filter;
	/** The caller's, handed to #filter unchanged. */
	void *user;
} APMATH_CAST_DESC;

/**
 * @brief Where a ray hit an instance, in the instance BVH's coordinates.
 */
/* Frozen: filled on every cast. It never changes; a new need gets a new type. */
typedef struct APMATH_INSTANCE_HIT {
	/** The instance's position in the desc's order. */
	uint32_t instance;
	/** The instance's id. */
	uint64_t id;
	/**
	 * As APMATH_MESH_HIT's, as are #back_face, #t, #point and #normal. A
	 * triangle's front is the mesh's, wherever the transform puts it - so a
	 * closed solid's outside stays its outside when an instance mirrors it.
	 */
	uint32_t triangle;
	/** As APMATH_MESH_HIT's. */
	bool back_face;
	/** As APMATH_MESH_HIT's. */
	double t;
	/** As APMATH_MESH_HIT's. */
	ApriDVec3 point;
	/** As APMATH_MESH_HIT's. */
	ApriDVec3 normal;
} APMATH_INSTANCE_HIT;

/**
 * @brief Casts a ray against an instance BVH.
 *
 * Finds the hit with the smallest t among the instances @p desc lets
 * through. Of instances hit at the same t, the one earliest in the desc's
 * order; of triangles, likewise - so a cast is repeatable.
 *
 * Thread: may overlap any other cast or get_ call on @p bvh or on the mesh
 * BVHs it holds, from any thread; not a destroy of either. @p desc's filter
 * runs on this call's thread.
 *
 * @param[in]  bvh     The instances.
 * @param[in]  ray     The ray, in the instance BVH's coordinates.
 * @param[in]  desc    Which instances to consider. NULL considers them all.
 * @param[out] out_hit The hit. May be NULL; it's left alone on a miss.
 *
 * @return true if the ray hits an instance.
 */
bool apmath_instance_bvh_cast(apmath_instance_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                              APMATH_INSTANCE_HIT *out_hit);

/**
 * @brief Casts a ray against an instance BVH, for every instance it hits.
 *
 * One hit each - the instance's own nearest - ordered as
 * apmath_instance_bvh_cast() would choose between them.
 *
 * Thread: as apmath_instance_bvh_cast().
 *
 * @param[in]  bvh          The instances.
 * @param[in]  ray          The ray, in the instance BVH's coordinates.
 * @param[in]  desc         Which instances to consider. NULL considers them
 *                          all.
 * @param[out] out_hits     Receives the first @p hit_capacity hits. May be
 *                          NULL, to ask how big a buffer to pass.
 * @param[in]  hit_capacity How many hits @p out_hits has room for.
 *
 * @return How many instances the ray hits, whatever @p hit_capacity is.
 */
uint32_t apmath_instance_bvh_cast_all(apmath_instance_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                                      APMATH_INSTANCE_HIT *out_hits, uint32_t hit_capacity);

#if __cplusplus
}
#endif

#endif // APMATHBVH_H
