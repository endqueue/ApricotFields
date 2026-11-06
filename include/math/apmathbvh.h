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
 * Shapes: a sphere, a capsule and a box can be tested against a mesh or
 * against instances (overlaps) and moved along a line to where they first
 * touch one (sweeps); see APMATH_SHAPE. Every shape is a core - a point, a
 * segment or a box - grown by a radius, and one algorithm answers both
 * questions for all three: GJK's distance between the core and a triangle,
 * compared with the radius, and GJK's ray cast for the first touch. A sweep
 * walks a tree as a ray does, against each box grown by the shape's own,
 * nearest first, so it costs about what a ray does whatever its length.
 *
 * A box BVH (apmath_box_bvh) holds solid axis-aligned boxes the caller
 * gives, not meshes, and answers the same rays, shapes and sweeps.
 *
 * A mesh BVH is immutable once created. An instance BVH changes only
 * through apmath_instance_bvh_set_instances(), _refit() and _rebuild(),
 * which keep its instance count and its memory: a caller whose instances
 * move changes them in place and refits, and rebuilds when the refitted
 * tree has grown too costly (apmath_instance_bvh_get_cost()), with no
 * allocation either way. A change of instance count is a new BVH. A box BVH
 * changes the same way. Rebuilding an instance BVH doesn't rebuild its
 * meshes, so changing one mesh among many costs that mesh's build plus a
 * build over boxes.
 *
 * General purpose: it knows nothing about what a mesh is or why a ray is
 * cast. What an instance means is the caller's, through its id; which
 * instances a cast considers is the caller's, through masks and a filter
 * callback.
 *
 * Out of scope: curved primitives, shapes that turn as they move, the depth
 * of a penetration and the way out of it, and anything GPU - this is CPU
 * geometry, unrelated to Aprend.
 *
 * @par Threads
 * apricore.h's rule ("Threads"), with these exceptions. The casts, volume
 * tests, shape queries and get_ calls on one BVH only read it, so they may
 * overlap each other, from any number of threads, and never block or
 * allocate; they are marked. Destroying a BVH may overlap none of them, nor
 * the creation of an instance BVH over it, which only reads it. A create
 * call takes no other object, so creates may run at once. A filter callback
 * runs inside the call, on the thread that made it.
 *
 * @par
 * apmath_instance_bvh_set_instances() changes the BVH: it may overlap other
 * set_instances calls on the same BVH whose indices are all different (each
 * writes only its instances' records), and nothing else on it - no cast,
 * test, get_, refit or rebuild. apmath_instance_bvh_refit() and
 * apmath_instance_bvh_rebuild() may overlap nothing on the BVH. A box BVH's
 * set_boxes, refit and rebuild are the same. Debug builds check all of it,
 * and report an overlap the rule forbids on stderr, naming both calls. A
 * query made after a set and before the refit or rebuild that follows it is
 * the caller's error: the tree's boxes are the old ones, so it may miss what
 * moved.
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
 * Volume tests and shape queries can't fail either: given arguments they
 * can't use, they answer as for a mesh that isn't there. Refit and rebuild
 * can't fail. apmath_instance_bvh_set_instances() and
 * apmath_box_bvh_set_boxes() check every entry before changing any: on
 * failure the BVH is as it was, and an APMATH_ERROR (apmatherror.h) says
 * which entry and why.
 *
 * @par Same bits
 * Every query gives the same bits on every platform: this is built without
 * floating-point contraction, and GJK ends on a fixed tolerance and a fixed
 * count, never on timing. Results never depend on the tree's shape: of hits
 * at the same t, the earlier instance (or box) in the caller's order and
 * then the earlier triangle win; lists are in the caller's order or by t
 * then position. So a refit, a rebuild or a fresh create over the same
 * instances give the same answers, bit for bit.
 *
 * @par Struct bases
 * Descs start with a `base` (apricore.h, "Struct bases"): the caller sets
 * `base.type` to the desc's own APRI_STRUCT_TYPE_APMATH_* and `base.next` to
 * NULL. A create given a wrong type returns APRESULT_STRUCT_WRONG_TYPE, and
 * one given a chained struct APRESULT_STRUCT_UNKNOWN_IN_CHAIN: nothing
 * chains onto any of them yet. A cast given either hits nothing.
 * APMATH_AABB, APMATH_RAY, APMATH_PLANE, APMATH_VOLUME, APMATH_BVH_INSTANCE,
 * APMATH_SHAPE, APMATH_SWEEP, APMATH_BVH_BOX and the hit structs are
 * frozen: they never grow (a new need gets a new type).
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
#include "apmatherror.h"
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

/* ---- Shapes ---------------------------------------------------------- */

/**
 * @brief Which shape an APMATH_SHAPE is.
 *
 * 0 is "not chosen", and a shape of that kind meets nothing.
 */
typedef enum APMATH_SHAPE_KIND APRI_ENUM_BASE {
	/** None. A shape of this kind meets nothing. */
	APMATH_SHAPE_KIND_NONE = 0,
	/** A ball: every point within `radius` of `center`. */
	APMATH_SHAPE_KIND_SPHERE = 1,
	/**
	 * A segment grown by `radius`: the segment runs from `center` -
	 * `half_height` to `center` + `half_height` along the shape's own y
	 * axis, turned by `rotation`. Its whole length is 2 * (`half_height` +
	 * `radius`).
	 */
	APMATH_SHAPE_KIND_CAPSULE = 2,
	/**
	 * A box: `half_extents` along each of the shape's own axes from
	 * `center`, turned by `rotation`. `radius` must be 0.
	 */
	APMATH_SHAPE_KIND_BOX = 3,
} APMATH_SHAPE_KIND;

/**
 * @brief A sphere, a capsule or a box, in the coordinates of whatever it is
 * tested against.
 *
 * A shape is refused - it meets nothing, as a NULL one - if its kind is none
 * of the enum's, a value it uses isn't finite, `radius`, `half_height` or a
 * `half_extents` is negative, a box's `radius` isn't 0, or `rotation` (for a
 * capsule or a box) is further than 1e-6 from unit length. A sphere ignores
 * `rotation`, `half_height` and `half_extents`; a capsule `half_extents`; a
 * box `half_height`. A sphere or capsule of radius 0 is a point or a
 * segment, and valid.
 */
/* Frozen: a value, passed on every query. It never changes; a new need gets a new type. */
typedef struct APMATH_SHAPE {
	/** Which shape. */
	APMATH_SHAPE_KIND kind;
	/** Its middle. */
	ApriDVec3 center;
	/** Its turn, as a unit quaternion: x, y, z, w. Identity is 0, 0, 0, 1. */
	ApriDVec4 rotation;
	/** A box's half size along its own x, y and z. At least 0 each. */
	ApriDVec3 half_extents;
	/** A capsule's half segment length along its own y. At least 0. */
	double half_height;
	/** How far the shape reaches past its core (sphere, capsule). At least 0; 0 for a box. */
	double radius;
} APMATH_SHAPE;

/**
 * @brief A shape moved along a line, without turning: the shape with its
 * center at `shape.center` + t * `direction`, for t from 0 to `t_max`.
 *
 * t counts in lengths of `direction`, as a ray's does, so a unit direction
 * makes it a distance. A zero `direction` asks only whether the shape
 * overlaps at t = 0. A `t_max` below 0 or not a number meets nothing; it may
 * be INFINITY. A `direction` that isn't finite meets nothing.
 */
/* Frozen: a value, passed on every sweep. It never changes; a new need gets a new type. */
typedef struct APMATH_SWEEP {
	/** The shape at t = 0. */
	APMATH_SHAPE shape;
	/** How far its center moves per unit of t. */
	ApriDVec3 direction;
	/** The furthest t the sweep covers. At least 0. */
	double t_max;
} APMATH_SWEEP;

/**
 * @brief Where a sweep first touched a triangle.
 *
 * A swept shape touches at a point beside its path, and the direction it is
 * pushed back along (the contact normal) is not the triangle's face normal
 * where it touches an edge or a corner: both are given.
 *
 * GJK's answer is conservative: a sweep stops at most its tolerance (1e-12
 * of the larger of the shape's and the triangle's sizes) short of the
 * surface, never inside it. Where GJK reaches its iteration count (64)
 * without converging - a near-degenerate triangle, a shape grazing an edge
 * at a tolerance's width - it reports a touch at the t it has reached,
 * which is never past the true first touch. The price: such a capped run
 * can report a touch that never comes, a near miss counted as a hit, never
 * farther than 1e-9 of the larger size from the surface.
 */
/* Frozen: filled on every sweep. It never changes; a new need gets a new type. */
typedef struct APMATH_SWEEP_HIT {
	/** The instance's position in the desc's order; 0 from a mesh BVH. */
	uint32_t instance;
	/** The instance's id; 0 from a mesh BVH. */
	uint64_t id;
	/** The triangle's position in the mesh desc's order. */
	uint32_t triangle;
	/**
	 * True if the shape overlapped the triangle at t = 0 (touching counts):
	 * then #t is 0, and #point and #contact_normal are those of the closest
	 * points found, not a first touch (there was none). Where the shape's
	 * core itself crosses the triangle, #contact_normal is the face's,
	 * turned against the sweep's direction.
	 */
	bool started_overlapping;
	/** The sweep's t at the touch. */
	double t;
	/** The point touched, on the triangle, in the coordinates queried in. */
	ApriDVec3 point;
	/**
	 * Unit, from the triangle towards the shape at the touch: the way the
	 * shape would be pushed back. The face's normal where a face is
	 * touched; along the closest points where an edge or a corner is.
	 */
	ApriDVec3 contact_normal;
	/** Unit, out of the triangle's front, as APMATH_MESH_HIT::normal. */
	ApriDVec3 face_normal;
} APMATH_SWEEP_HIT;

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

/**
 * @brief Whether a placed mesh touches a shape: some triangle within the
 * shape or on its surface.
 *
 * Exact against the triangles, to GJK's tolerance (1e-12 of the larger of
 * the shape's and the triangle's sizes), in double precision. Stops at the
 * first triangle found. Only the surface counts, as
 * apmath_mesh_bvh_overlaps_volume(): a shape wholly inside a closed mesh,
 * touching none of its triangles, is not found. Where GJK reaches its
 * iteration count it answers touching (APMATH_SWEEP_HIT says what that
 * costs).
 *
 * Thread: may overlap any other query or get_ call on @p bvh, from any
 * thread; not its destroy. Allocates nothing.
 *
 * @param[in] bvh       The mesh. NULL is a mesh with no triangles.
 * @param[in] transform Mesh coordinates to the shape's, as an instance's
 *                      (APMATH_BVH_INSTANCE::transform); NULL for none.
 * @param[in] shape     The shape. A refused one (APMATH_SHAPE) touches
 *                      nothing.
 *
 * @return true if a triangle touches the shape. false for a NULL @p shape,
 *         and a transform that isn't finite, can't be inverted or whose
 *         last row isn't 0, 0, 0, 1.
 */
bool apmath_mesh_bvh_overlaps_shape(apmath_mesh_bvh bvh, const ApriDMat4 *transform, const APMATH_SHAPE *shape);

/**
 * @brief Sweeps a shape against a placed mesh: the smallest t at which it
 * touches a triangle.
 *
 * Of triangles touched at the same t, the one earliest in the desc's order.
 * t is exact to GJK's tolerance: a touch is reported at the first t whose
 * distance is within it, so the shape stops at most that far short of the
 * surface, never inside it (APMATH_SWEEP_HIT).
 *
 * Thread: as apmath_mesh_bvh_overlaps_shape(). Allocates nothing.
 *
 * @param[in]  bvh       The mesh. NULL is a mesh with no triangles.
 * @param[in]  transform As apmath_mesh_bvh_overlaps_shape().
 * @param[in]  sweep     The sweep, in the coordinates the transform carries
 *                       the mesh into. A NULL or refused one touches nothing.
 * @param[out] out_hit   The touch. May be NULL, to ask only whether there is
 *                       one (the walk then stops at the first found); left
 *                       alone when there is none. Its instance and id are 0.
 *
 * @return true if the shape touches the mesh within the sweep. false also
 *         in every case apmath_mesh_bvh_overlaps_shape() names.
 */
bool apmath_mesh_bvh_sweep(apmath_mesh_bvh bvh, const ApriDMat4 *transform, const APMATH_SWEEP *sweep,
                           APMATH_SWEEP_HIT *out_hit);

/* ---- Instance BVH ---------------------------------------------------- */

/**
 * @brief Opaque handle to an instance BVH: placements of mesh BVHs, changed
 * in place only by apmath_instance_bvh_set_instances() and the refit or
 * rebuild that follows it.
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
 * Allocates about 470 bytes an instance: its record (216 bytes, its
 * transform both ways in doubles), its box twice (exact, for
 * apmath_instance_bvh_get_bounds(), and padded, for the tree: 96 bytes),
 * room for the two tree nodes an instance may need at worst (112 bytes)
 * and their parents (8), its place in the order, the rebuild's scratch and
 * the marks the in-place calls keep (about 40). Nothing done to it after
 * allocates.
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
 * @param[out] out_hit The hit. May be NULL, to ask only whether the ray
 *                     hits: the walk then stops at the first hit it finds.
 *                     It's left alone on a miss.
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

/* ---- Instance BVH: shapes -------------------------------------------- */

/**
 * @brief Whether any instance @p desc lets through touches a shape.
 *
 * Stops at the first found: the cheap form, for "is anything there".
 *
 * Thread: as apmath_instance_bvh_cast(). Allocates nothing.
 *
 * @param[in] bvh   The instances.
 * @param[in] shape The shape, in the instance BVH's coordinates. A NULL or
 *                  refused one (APMATH_SHAPE) touches nothing.
 * @param[in] desc  Which instances to consider; NULL for all. Its filter is
 *                  asked about each instance whose box the shape's box
 *                  meets, at most once.
 *
 * @return true if one does.
 */
bool apmath_instance_bvh_overlaps_shape(apmath_instance_bvh bvh, const APMATH_SHAPE *shape,
                                        const APMATH_CAST_DESC *desc);

/**
 * @brief Which instances touch a shape, among those @p desc lets through,
 * in the desc's order: the lowest positions first, whatever the tree.
 *
 * So the answer depends on the instances alone, never on how the tree was
 * built, refitted or rebuilt.
 *
 * Thread: as apmath_instance_bvh_cast(). Allocates nothing: @p
 * out_instances is the working space. While there is room, positions are
 * appended as found; once it is full, it is kept as a max-heap of the
 * lowest @p instance_capacity found, each new position replacing the
 * heap's top if lower. At the end it is sorted. So n found cost about
 * n log(capacity).
 *
 * @param[in]  bvh               The instances.
 * @param[in]  shape             The shape, in the instance BVH's coordinates.
 * @param[in]  desc              Which instances to consider; NULL for all.
 * @param[out] out_instances     Receives the positions of the first
 *                               @p instance_capacity instances, in order. May
 *                               be NULL, to ask how big a buffer to pass.
 * @param[in]  instance_capacity How many @p out_instances has room for.
 *
 * @return How many instances touch the shape, whatever @p instance_capacity is.
 */
uint32_t apmath_instance_bvh_overlap_all(apmath_instance_bvh bvh, const APMATH_SHAPE *shape,
                                         const APMATH_CAST_DESC *desc, uint32_t *out_instances,
                                         uint32_t instance_capacity);

/**
 * @brief Sweeps a shape against an instance BVH: the smallest t at which it
 * touches an instance @p desc lets through.
 *
 * Of touches at the same t, the earliest instance in the desc's order, then
 * the earliest triangle.
 *
 * Thread: as apmath_instance_bvh_cast(). Allocates nothing.
 *
 * @param[in]  bvh     The instances.
 * @param[in]  sweep   The sweep, in the instance BVH's coordinates. A NULL or
 *                     refused one touches nothing.
 * @param[in]  desc    Which instances to consider; NULL for all.
 * @param[out] out_hit The touch, its point and normals in the instance BVH's
 *                     coordinates. May be NULL, to ask only whether there is
 *                     one (the walk then stops at the first found); left
 *                     alone when there is none.
 *
 * @return true if the shape touches an instance within the sweep.
 */
bool apmath_instance_bvh_sweep(apmath_instance_bvh bvh, const APMATH_SWEEP *sweep, const APMATH_CAST_DESC *desc,
                               APMATH_SWEEP_HIT *out_hit);

/**
 * @brief Sweeps a shape against an instance BVH, for every instance it
 * touches: one touch each, the instance's own first, ordered as
 * apmath_instance_bvh_sweep() would choose between them.
 *
 * Thread: as apmath_instance_bvh_cast(). Allocates nothing: @p out_hits is
 * the working space, kept as apmath_instance_bvh_overlap_all() keeps its
 * positions (appended while there is room, then a max-heap of the first
 * @p hit_capacity touches by that order, sorted at the end).
 *
 * @param[in]  bvh          The instances.
 * @param[in]  sweep        The sweep, in the instance BVH's coordinates.
 * @param[in]  desc         Which instances to consider; NULL for all.
 * @param[out] out_hits     Receives the first @p hit_capacity touches. May
 *                          be NULL, to ask how big a buffer to pass.
 * @param[in]  hit_capacity How many @p out_hits has room for.
 *
 * @return How many instances the shape touches, whatever @p hit_capacity is.
 */
uint32_t apmath_instance_bvh_sweep_all(apmath_instance_bvh bvh, const APMATH_SWEEP *sweep,
                                       const APMATH_CAST_DESC *desc, APMATH_SWEEP_HIT *out_hits,
                                       uint32_t hit_capacity);

/* ---- Instance BVH: changing it in place ------------------------------ */

/**
 * @brief Replaces some of an instance BVH's instances: each named position
 * gets a new mesh, transform, id and mask, and its box is worked out again.
 *
 * The tree is not changed: call apmath_instance_bvh_refit() or
 * apmath_instance_bvh_rebuild() before the next query. The count stays the
 * BVH's: a caller that has things come and go keeps positions for them with
 * a NULL mesh (never hit, no box) and sets one when it is used.
 *
 * Every instance is checked before any is changed: on failure the BVH is as
 * it was.
 *
 * Thread: may overlap other calls of this function on @p bvh whose
 * @p indices are all different from this one's; nothing else on @p bvh.
 * Allocates nothing.
 *
 * @param[in]  bvh       The BVH. Required.
 * @param[in]  indices   The positions to replace, each below the BVH's
 *                       instance count and none twice. Required unless
 *                       @p count is 0. Only read during the call.
 * @param[in]  instances The new instances, one per index, as
 *                       APMATH_BVH_INSTANCE says (the mesh not owned, the
 *                       transform copied). Required unless @p count is 0.
 * @param[in]  count     How many to replace. 0 changes nothing.
 * @param[out] error     Why it failed; NULL to skip. Its message is empty on
 *                       success.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p bvh,
 *         @p indices or @p instances (with @p count above 0), an index past
 *         the instance count, an index given twice, or an instance whose
 *         transform isn't finite, can't be inverted or whose last row isn't
 *         0, 0, 0, 1 - the message names which, and the position in
 *         @p indices.
 */
APRESULT apmath_instance_bvh_set_instances(apmath_instance_bvh bvh, const uint32_t *indices,
                                           const APMATH_BVH_INSTANCE *instances, uint32_t count,
                                           APMATH_ERROR *error);

/**
 * @brief Carries the boxes of the instances set since the last refit or
 * rebuild up the tree: every node above one gets the box around its
 * children again.
 *
 * The tree's shape is kept, so as instances move far from where it was
 * built it grows worse to walk (apmath_instance_bvh_get_cost()), never
 * wrong. Costs the nodes above the instances set: about their count times
 * the tree's depth when few were set, every node once when most were.
 *
 * Thread: may overlap nothing on @p bvh. Allocates nothing.
 *
 * @param[in] bvh The BVH. NULL does nothing.
 */
void apmath_instance_bvh_refit(apmath_instance_bvh bvh);

/**
 * @brief Builds the tree again over the instances as they are, as
 * apmath_instance_bvh_create() would build it over the same instances,
 * node for node, in the memory the BVH already has.
 *
 * For when refitting has let the tree grow too costly. The build's scratch
 * is made with the BVH, so this allocates nothing.
 *
 * Thread: may overlap nothing on @p bvh. Allocates nothing.
 *
 * @param[in] bvh The BVH. NULL does nothing.
 */
void apmath_instance_bvh_rebuild(apmath_instance_bvh bvh);

/**
 * @brief The tree's surface-area cost: the sum, over its inner nodes, of
 * each node's box's area, over the root's area.
 *
 * The expected number of boxes a ray through the root tests, by the
 * surface-area heuristic the build uses. Kept up to date by refit and
 * rebuild, so reading it costs nothing. A caller rebuilds when it has grown
 * past some ratio of what it was after the last rebuild; the ratio is the
 * caller's.
 *
 * Thread: as apmath_instance_bvh_get_bounds().
 *
 * @param[in] bvh The BVH.
 *
 * @return The cost, at least 0; 0 for a BVH with fewer than two instances
 *         with triangles, and for a NULL @p bvh.
 */
double apmath_instance_bvh_get_cost(apmath_instance_bvh bvh);

/* ---- Box BVH: solid boxes -------------------------------------------- */

/**
 * @brief Opaque handle to a box BVH: solid axis-aligned boxes the caller
 * gives.
 *
 * Each box is a solid: what is inside it is in it. A ray whose start
 * (t_min) is inside a box meets it at t_min; a shape that overlaps a box's
 * inside overlaps it, touching a face or not; a sweep that starts
 * overlapping one touches it at 0, started_inside. Queried by rays, shapes
 * and sweeps as the instance BVH is, and changed in place the same way:
 * apmath_box_bvh_set_boxes(), then a refit or rebuild. Its threads,
 * results, ties and order are the instance BVH's ("Threads" and "Same bits"
 * above): of meetings at the same t, the earliest position; lists in
 * position order (overlaps) or by t then position (cast_all, sweep_all).
 */
typedef struct apmath_box_bvh_t *apmath_box_bvh;

/**
 * @brief One box of a box BVH.
 */
/* Frozen: an array element. It never changes; a new need gets a new type. */
typedef struct APMATH_BVH_BOX {
	/** The box, in the BVH's coordinates. One whose lo is above its hi on any axis is empty: never met. Finite. */
	APMATH_AABB box;
	/** The caller's, handed back in hits and to the filter. Needn't be unique. */
	uint64_t id;
	/** Which queries consider the box, as APMATH_BVH_INSTANCE::mask. */
	uint32_t mask;
} APMATH_BVH_BOX;

/**
 * @brief Configuration descriptor for a box BVH.
 */
typedef struct APMATH_BOX_BVH_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APMATH_BOX_BVH_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The boxes. Copied. Their count is the BVH's for its life; an entry not in use is an empty box. */
	const APMATH_BVH_BOX *boxes;
	/** How many #boxes holds. */
	uint32_t box_count;
} APMATH_BOX_BVH_DESC;

/**
 * @brief Where a ray or a sweep met a box, in the BVH's coordinates.
 */
/* Frozen: filled on every query. It never changes; a new need gets a new type. */
typedef struct APMATH_BOX_HIT {
	/** The box's position in the desc's order. */
	uint32_t box;
	/** The box's id. */
	uint64_t id;
	/** True if the ray's start or the sweep's shape at t = 0 was already inside the box: #t is then the ray's t_min, or 0. */
	bool started_inside;
	/** The ray's or the sweep's t at the meeting. */
	double t;
	/** The point met: on the box's surface, or the ray's start when it started inside. */
	ApriDVec3 point;
	/** Unit, the way a sweep's shape would be pushed back (as APMATH_SWEEP_HIT::contact_normal); for a ray, the face's. -direction, normalised, when it started inside. */
	ApriDVec3 contact_normal;
	/** Unit, out of the face of the box nearest the meeting. -direction, normalised, when it started inside. */
	ApriDVec3 face_normal;
} APMATH_BOX_HIT;

/**
 * @brief Creates a box BVH over the desc's boxes.
 *
 * Allocates about 270 bytes a box: its box, id and mask (64 bytes), its
 * padded box for the tree (48), room for the two tree nodes a box may need
 * at worst (112) and their parents (8), its place in the order, the
 * rebuild's scratch and the marks (about 40). Nothing done to it after
 * allocates.
 *
 * @param[in]  desc    The boxes. Required; its base checked. Only read
 *                     during the call.
 * @param[out] out_bvh Receives the BVH; NULL on failure. Required.
 * @param[out] error   Why it failed; NULL to skip.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p desc or
 *         @p out_bvh, NULL boxes with a count above 0, a count past
 *         UINT32_MAX / 2, or a box not finite (the message names its
 *         position); the struct codes for its base; APRESULT_OUT_OF_MEMORY.
 */
APRESULT apmath_box_bvh_create(const APMATH_BOX_BVH_DESC *desc, apmath_box_bvh *out_bvh, APMATH_ERROR *error);

/**
 * @brief Destroys a box BVH.
 *
 * @param[in] bvh The BVH to destroy. NULL is fine.
 */
void apmath_box_bvh_destroy(apmath_box_bvh bvh);

/**
 * @brief Replaces some boxes, as apmath_instance_bvh_set_instances()
 * replaces instances: every box checked before any changes, the tree
 * changed only by the refit or rebuild that follows.
 *
 * Thread: as apmath_instance_bvh_set_instances(). Allocates nothing.
 *
 * @param[in]  bvh     The BVH. Required.
 * @param[in]  indices The positions to replace, each below the box count and
 *                     none twice. Required unless @p count is 0.
 * @param[in]  boxes   The new boxes, one per index. Required unless @p count
 *                     is 0.
 * @param[in]  count   How many to replace. 0 changes nothing.
 * @param[out] error   Why it failed; NULL to skip.
 *
 * @return As apmath_instance_bvh_set_instances(), a box not finite in place
 *         of a transform's refusals.
 */
APRESULT apmath_box_bvh_set_boxes(apmath_box_bvh bvh, const uint32_t *indices, const APMATH_BVH_BOX *boxes,
                                  uint32_t count, APMATH_ERROR *error);

/**
 * @brief As apmath_instance_bvh_refit(), for boxes.
 *
 * Thread: may overlap nothing on @p bvh. Allocates nothing.
 *
 * @param[in] bvh The BVH. NULL does nothing.
 */
void apmath_box_bvh_refit(apmath_box_bvh bvh);

/**
 * @brief As apmath_instance_bvh_rebuild(), for boxes: node for node what
 * apmath_box_bvh_create() would build over the same boxes.
 *
 * Thread: may overlap nothing on @p bvh. Allocates nothing.
 *
 * @param[in] bvh The BVH. NULL does nothing.
 */
void apmath_box_bvh_rebuild(apmath_box_bvh bvh);

/**
 * @brief As apmath_instance_bvh_get_cost(), for boxes.
 *
 * Thread: may overlap any query on @p bvh.
 *
 * @param[in] bvh The BVH.
 *
 * @return The cost, at least 0; 0 for fewer than two boxes that aren't
 *         empty, and for a NULL @p bvh.
 */
double apmath_box_bvh_get_cost(apmath_box_bvh bvh);

/**
 * @brief The nearest box a ray meets, a box its start is inside met at
 * t_min.
 *
 * Of boxes met at the same t, the earliest position.
 *
 * Thread: may overlap any other query or get_ call on @p bvh. Allocates
 * nothing.
 *
 * @param[in]  bvh     The boxes.
 * @param[in]  ray     The ray. A NULL one, or one that isn't finite, meets
 *                     nothing.
 * @param[in]  desc    Which boxes to consider; NULL for all. Its filter is
 *                     handed each box's position and id.
 * @param[out] out_hit The meeting; may be NULL, to ask only whether there is
 *                     one (the walk then stops at the first found). Left
 *                     alone when there is none.
 *
 * @return true if the ray meets a box @p desc lets through.
 */
bool apmath_box_bvh_cast(apmath_box_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                         APMATH_BOX_HIT *out_hit);

/**
 * @brief Every box a ray meets, one meeting each, by t then position.
 *
 * Thread: as apmath_box_bvh_cast(). Allocates nothing: @p out_hits is kept
 * as apmath_instance_bvh_overlap_all() keeps its buffer.
 *
 * @param[in]  bvh          The boxes.
 * @param[in]  ray          The ray.
 * @param[in]  desc         Which boxes to consider; NULL for all.
 * @param[out] out_hits     Receives the first @p hit_capacity meetings. May
 *                          be NULL, to ask how big a buffer to pass.
 * @param[in]  hit_capacity How many @p out_hits has room for.
 *
 * @return How many boxes the ray meets, whatever @p hit_capacity is.
 */
uint32_t apmath_box_bvh_cast_all(apmath_box_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                                 APMATH_BOX_HIT *out_hits, uint32_t hit_capacity);

/**
 * @brief Whether any box overlaps a shape (a solid: inside counts).
 *
 * Thread: as apmath_box_bvh_cast(). Allocates nothing.
 *
 * @param[in] bvh   The boxes.
 * @param[in] shape The shape. A NULL or refused one overlaps nothing.
 * @param[in] desc  Which boxes to consider; NULL for all.
 *
 * @return true if one does.
 */
bool apmath_box_bvh_overlaps_shape(apmath_box_bvh bvh, const APMATH_SHAPE *shape, const APMATH_CAST_DESC *desc);

/**
 * @brief Which boxes overlap a shape, in position order.
 *
 * Thread: as apmath_box_bvh_cast(). Allocates nothing: @p out_boxes is kept
 * as apmath_instance_bvh_overlap_all() keeps its buffer.
 *
 * @param[in]  bvh          The boxes.
 * @param[in]  shape        The shape.
 * @param[in]  desc         Which boxes to consider; NULL for all.
 * @param[out] out_boxes    Receives the positions of the first
 *                          @p box_capacity boxes, in order. May be NULL, to
 *                          ask how big a buffer to pass.
 * @param[in]  box_capacity How many @p out_boxes has room for.
 *
 * @return How many boxes overlap it, whatever @p box_capacity is.
 */
uint32_t apmath_box_bvh_overlap_all(apmath_box_bvh bvh, const APMATH_SHAPE *shape, const APMATH_CAST_DESC *desc,
                                    uint32_t *out_boxes, uint32_t box_capacity);

/**
 * @brief The first box a sweep touches; one it starts in at 0,
 * started_inside.
 *
 * Thread: as apmath_box_bvh_cast(). Allocates nothing.
 *
 * @param[in]  bvh     The boxes.
 * @param[in]  sweep   The sweep. A NULL or refused one touches nothing.
 * @param[in]  desc    Which boxes to consider; NULL for all.
 * @param[out] out_hit The touch. May be NULL, to ask only whether there is
 *                     one; left alone when there is none.
 *
 * @return true if the sweep touches a box.
 */
bool apmath_box_bvh_sweep(apmath_box_bvh bvh, const APMATH_SWEEP *sweep, const APMATH_CAST_DESC *desc,
                          APMATH_BOX_HIT *out_hit);

/**
 * @brief Every box a sweep touches, one touch each, by t then position.
 *
 * Thread: as apmath_box_bvh_cast(). Allocates nothing: @p out_hits is kept
 * as apmath_instance_bvh_overlap_all() keeps its buffer.
 *
 * @param[in]  bvh          The boxes.
 * @param[in]  sweep        The sweep.
 * @param[in]  desc         Which boxes to consider; NULL for all.
 * @param[out] out_hits     Receives the first @p hit_capacity touches. May
 *                          be NULL, to ask how big a buffer to pass.
 * @param[in]  hit_capacity How many @p out_hits has room for.
 *
 * @return How many boxes the sweep touches, whatever @p hit_capacity is.
 */
uint32_t apmath_box_bvh_sweep_all(apmath_box_bvh bvh, const APMATH_SWEEP *sweep, const APMATH_CAST_DESC *desc,
                                  APMATH_BOX_HIT *out_hits, uint32_t hit_capacity);

#if __cplusplus
}
#endif

#endif // APMATHBVH_H
