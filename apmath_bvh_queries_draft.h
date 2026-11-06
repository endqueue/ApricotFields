/*
 * apmath_bvh_queries_draft.h - shape queries on ApMath's BVHs, and an
 * instance BVH that changes in place (apmathbvh.h).
 *
 * DRAFT, 2026-11-06, third version (the first 2026-11-05), revised after
 * two reviews: a sweep walks the tree as a ray against boxes grown by the
 * shape's, nearest first, not by the box around its whole path; what GJK
 * reports when it reaches its count, a near miss reported as a touch its
 * stated price and bound; overlap_all ordered by a heap, not by
 * insertion; set_instances' marks kept apart from refit's; a new shape
 * with data of its own is a new type; a box BVH over solid boxes the
 * caller gives. APPROVED by the user 2026-11-06, as it stands after two
 * reviews.
 * Not built, not in any target. Not compilable: it repeats declarations of
 * include/math/apmathbvh.h, include/math/apmathpoly.h and apricore.h to show
 * them changed. Every declaration below is marked [ADDED], [CHANGED] or
 * [REMOVED]; nothing unmarked changes. Each says why it earns its place, its
 * ABI break, and its tests. Written against apmathbvh.h and
 * src/math/apmathbvh.cpp as read on 2026-11-05.
 *
 * ---------------------------------------------------------------------------
 * What apmathbvh.h lacks, and what this adds
 * ---------------------------------------------------------------------------
 *
 * apmathbvh.h casts rays at meshes and at many placed meshes, and tests a
 * mesh against a convex set of planes. A caller that keeps many moving
 * things in an instance BVH, and asks what a moving shape meets, can't:
 *
 * 1. No shape but a ray. Asking what a sphere, a capsule or a box touches
 *    (an overlap), or where one moved along a line first touches something
 *    (a sweep), is the next thing every caller of a ray cast asks: a
 *    character moved without falling through a floor, what is within reach,
 *    whether a box fits where it is put. A plane volume covers a box at
 *    rest, not a sphere, a capsule or anything moving.
 * 2. No change in place. An instance BVH is created and never changed, so a
 *    caller whose instances move remakes it whole: a binned surface-area
 *    build over every box, allocating, each time. Refitting (moving the
 *    boxes, keeping the tree) is what every engine with moving things does
 *    between rebuilds (Embree's refit build quality, PhysX's PxBVH::refit
 *    and its scene query update modes, Bullet's dbvt, Box2D v3's dynamic
 *    tree with its enlarged boxes, Jolt's broad phase quad tree).
 *
 * Added, for approval:
 *
 * - APMATH_SHAPE: a sphere, a capsule or a box, placed and turned, in
 *   double precision; APMATH_SWEEP: one moved along a line.
 * - Overlaps: apmath_mesh_bvh_overlaps_shape() (does a placed mesh touch the
 *   shape), apmath_instance_bvh_overlaps_shape() (does any instance) and
 *   apmath_instance_bvh_overlap_all() (which instances do).
 * - Sweeps: apmath_mesh_bvh_sweep(), apmath_instance_bvh_sweep() (the first
 *   touch) and apmath_instance_bvh_sweep_all() (each instance's first).
 * - Changing an instance BVH in place: apmath_instance_bvh_set_instances()
 *   replaces some instances (mesh, transform, id, mask) and computes their
 *   boxes; apmath_instance_bvh_refit() carries those boxes up the tree;
 *   apmath_instance_bvh_rebuild() builds the tree again over the instances
 *   as they are, in the memory it has. None of them allocates.
 *   apmath_instance_bvh_get_cost() is the tree's surface-area cost, so the
 *   caller can tell when a refitted tree has grown worse than a rebuild.
 * - A box BVH (apmath_box_bvh): the same queries and the same changes in
 *   place over solid axis-aligned boxes the caller gives, for a caller
 *   that asks about regions, not meshes (below, "Box BVH: solid boxes").
 * - APMATH_ERROR moves out of apmathpoly.h into a header of its own,
 *   math/apmatherror.h, so apmathbvh.h's new call can take one without
 *   including the polygons.
 *
 * Unchanged: every existing call and struct. The mesh BVH stays immutable.
 *
 * ---------------------------------------------------------------------------
 * How the shapes are tested: one algorithm
 * ---------------------------------------------------------------------------
 *
 * Every shape is a core - a point, a segment or a box - grown by a radius:
 * a sphere is a point grown by its radius, a capsule a segment grown by
 * its, a box a box grown by 0. A triangle is convex too. So an overlap is
 * the distance between two convex sets compared with the radius, and a
 * sweep is the first t at which that distance reaches the radius: GJK for
 * the distance (Gilbert, Johnson and Keerthi) and GJK's ray cast for the
 * sweep (van den Bergen, "Ray Casting against General Convex Objects with
 * Application to Continuous Collision Detection", 2004). Jolt's CastShape
 * and CollideShape do the same, with the same "convex radius" split. One
 * algorithm for three shapes, both questions, and any shape a later need
 * adds that has a support function (a cylinder, a convex hull): no table
 * of shape pairs. The algorithm extends so; APMATH_SHAPE doesn't. It is
 * frozen, so a shape that needs data of its own (a hull's points) is a new
 * struct type and new calls, sharing the algorithm; a cylinder, which a
 * radius and a half height describe, would only append a kind.
 *
 * How a sweep finds its triangles: as a ray, against every box grown by
 * the shape's own box (its box at t = 0, about its center): the shape's
 * center moved along the sweep enters a grown box exactly when the shape
 * enters the box. Nodes are visited nearest entry first, and a node whose
 * entry is past the nearest touch found so far is never opened: the walk
 * a ray cast makes, so a sweep costs about what a ray does whatever
 * t_max is, INFINITY included. Not the box around the whole path (start
 * and end), which for a long sweep holds most of the scene and opens every
 * node in it. The _any form stops at the first touch, nearest or not; the
 * _all form keeps opening nodes up to t_max. Overlaps, which don't move,
 * walk the nodes the shape's box meets. Jolt's broad phase and PhysX's
 * sweeps walk their trees the same way.
 *
 * When GJK reaches its iteration count (64) without converging - a
 * near-degenerate triangle, a shape grazing an edge at a tolerance's
 * width - its answer is conservative, never optimistic: a sweep reports a
 * touch at the t it has reached, which is never past the true first touch
 * (each step of the ray cast advances only as far as the distance found
 * proves clear), so a shape moved to it is never inside what it meets; an
 * overlap reports touching. The price, stated in the header: a capped run
 * can report a touch that never comes, a near miss counted as a hit. It is
 * small, since GJK fails to converge only where the distance is already
 * at its tolerance's scale (elsewhere it ends in a few iterations): a
 * capped touch is never farther than 1e-9 of the shape's and the
 * triangle's larger size from the surface. A sweep that must not stop
 * early on a graze can't exist at that scale in double precision anyway.
 * The same input reaches the count on every machine (the steps are fixed,
 * contraction off), so the answer is the same bits everywhere.
 *
 * Compared: Jolt (CastRay, CastShape, CollideShape, CollidePoint; GJK and
 * EPA with a convex radius), PhysX (PxScene raycast, sweep, overlap; its
 * sphere, capsule and box geometries; initial overlap reported as a hit at
 * distance 0 with eINITIAL_OVERLAP), Unreal (LineTrace, SweepSingle /
 * SweepMulti with FCollisionShape's sphere, capsule and box, OverlapMulti;
 * bStartPenetrating), Godot (intersect_ray, intersect_shape, cast_motion),
 * Bullet (convexSweepTest), and Hazel (Physics/SceneQueries.h: CastRay,
 * CastShape and OverlapShape for a box, a sphere and a capsule, through
 * Jolt; its shapes are not turned, these are).
 *
 * Not here: depth of penetration and the way out (that is a physics
 * engine's contact solving, EPA's, and the caller's physics); shapes
 * against shapes (a caller's instances are meshes); a sweep that turns as
 * it moves (each sweep moves the shape without turning it, as every engine
 * compared does).
 *
 * ---------------------------------------------------------------------------
 * The same bits everywhere
 * ---------------------------------------------------------------------------
 *
 * [CHANGED, build] src/math/apmathbvh.cpp is built with floating-point
 * contraction off (-ffp-contract=off on Clang and GCC, as
 * src/math/apmathpoly.cpp is; MSVC contracts only under /fp:contract), so a
 * cast, an overlap and a sweep give the same bits on every platform, and a
 * caller replaying a recorded run on another machine meets the same
 * geometry. Today's casts are built with contraction on: Clang on Apple
 * silicon fuses the Moller-Trumbore products, so a cast's t can differ in
 * its last bits from MSVC's. Measured before and after (Release, rays a
 * second against the tests' 10,000-instance scene). GJK's iteration ends
 * on a fixed tolerance and a fixed count (below), never on timing.
 *
 * Results never depend on the tree's shape: of hits at the same t, the
 * earlier instance in the caller's order and then the earlier triangle win,
 * as today; overlaps are listed in instance order. So a refit, a rebuild or
 * a fresh create over the same instances give the same answers, bit for
 * bit - which is what lets a caller choose between them by cost alone.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * No break. Every addition is new (the box BVH's struct type is 4006,
 * after APRI_STRUCT_TYPE_APMATH_POLY_SKELETON_DESC); APMATH_ERROR keeps its
 * name, layout and size and moves header (apmathpoly.h includes apmatherror.h, so a caller
 * that included apmathpoly.h for it still compiles). The header's
 * "immutable once created" sentence changes for the instance BVH: a
 * caller that relied on an instance BVH never changing is unaffected unless
 * it calls the new calls. ./check.sh --update-abi lists the additions.
 *
 * ---------------------------------------------------------------------------
 * Tests (tests/apmathbvh_tests.c unless named)
 * ---------------------------------------------------------------------------
 *
 * - Against brute force, as the casts' are (test_mesh_against_brute_force):
 *   1,000 random shapes of each kind, turned, against random meshes and
 *   placed instances; an overlap against every triangle's exact distance
 *   (a slow closest-point-of-triangle and of-segment computation); a sweep
 *   against a march of overlaps along the motion, bisected to 1e-9 of the
 *   motion's length, its t within that of the march's, and its point on
 *   the surface hit within it.
 * - Exact cases: a sphere resting on a plane's triangle (touching counts),
 *   a capsule lying along an edge two triangles share (one contact, the
 *   earlier triangle), a box turned 45 degrees onto a corner, a sweep that
 *   starts overlapping (t 0, started_overlapping), one parallel to a face
 *   it grazes, one that misses by 1e-9, a zero motion (an overlap test), a
 *   shape of radius 0 (a point, a segment).
 * - GJK's count: sliver triangles (one edge 1e-12 of the others) and
 *   capsules grazing them along that edge, 1,000 of them, the cases that
 *   reach the count (found while building it, with a local log, and kept
 *   as the test's inputs; the tests have no hook into src/): each sweep's
 *   t no later than the brute-force march's, the shape at that t not
 *   inside the triangle (the exact distance at least 0); each overlap
 *   true where the march's says touching; and the other side, every
 *   touch reported (a sweep's at its t, an overlap's) within 1e-9 of the
 *   larger size of the exact surface, so no capped run reports a touch
 *   farther off than the header states. The property is checked
 *   whichever path an input takes, so a later change to GJK can't make a
 *   capped answer optimistic unnoticed.
 * - The sweep's walk: a sweep of t_max INFINITY through the
 *   100,000-instance scene, its answer the brute force's; and in
 *   apmathbvh_bench, sweeps of t_max INFINITY against rays along the same
 *   lines, a second each, the sweep within a small multiple of the ray
 *   (a walk opening every node would be thousands of times slower).
 * - Tree independence: the same queries against an instance BVH created,
 *   then every instance moved by apmath_instance_bvh_set_instances() and
 *   refitted, then rebuilt, then created fresh over the moved instances:
 *   every answer the same bits all four ways, casts included.
 * - In place: set then refit then cast finds the moved instance where it
 *   went and not where it was; set of mesh NULL hides one; a set of a mesh
 *   with no triangles; the cost rising as instances scatter and falling
 *   back on a rebuild to the fresh create's cost exactly.
 * - Refusals: each of set's checks in parameter order, each its own
 *   result and message, each leaving the BVH as it was (checked by
 *   casting); a NULL or stale BVH for every call; a shape whose kind is
 *   NONE or out of range, a negative or non-finite radius, a quaternion
 *   not of unit length, each answering as for nothing there.
 * - aprinoalloc_tests: overlaps, sweeps, set, refit and rebuild of a
 *   10,000-instance BVH allocate nothing.
 * - aprioverlap_tests: casts, overlaps and sweeps from two threads on one
 *   BVH; set on disjoint instances from two threads, then refit; and, in
 *   Debug, a cast overlapping a set reported as an overlap (APRI_ENTER).
 * - The misuse harness, regenerated from abi.txt.
 * - apmathbvh_bits_tests (new, as apmathpoly_bits_tests): a fixed set of
 *   casts, overlaps and sweeps hashed, compared with the values recorded on
 *   the first machine; the same on every other.
 * - Numbers (Release, tests/apmathbvh_bench.c, new, not a ctest): refit
 *   against rebuild against create, at 100, 10,000 and 100,000 instances,
 *   all moving and 1% moving; casts, sweeps and overlaps a second.
 */

/* ===========================================================================
 * math/apmatherror.h  [ADDED, header]
 * ========================================================================= */

/*
 * Why it earns its place: apmath_instance_bvh_set_instances() fails for a
 * reason worth a sentence (which instance, and what of it), and card A2's
 * policy is a per-call APMATH_ERROR. The struct lives in apmathpoly.h
 * today; apmathbvh.h including the polygons for it would drag Clipper2's
 * world in by name. Moved, not copied: one definition, both headers
 * include it.
 */

/** @brief The size of APMATH_ERROR's message buffer, its NUL included. */
#define APMATH_ERROR_MESSAGE_SIZE 256 /* [CHANGED: moved from apmathpoly.h] */

/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct APMATH_ERROR { /* [CHANGED: moved from apmathpoly.h] */
	/** NUL-terminated, truncated to fit. Empty on success. */
	char message[APMATH_ERROR_MESSAGE_SIZE];
} APMATH_ERROR;

/* ===========================================================================
 * apmathbvh.h
 * ========================================================================= */

/*
 * [CHANGED] The file comment: "Both are immutable once created" becomes:
 *
 *   A mesh BVH is immutable once created. An instance BVH changes only
 *   through apmath_instance_bvh_set_instances(), _refit() and _rebuild(),
 *   which keep its instance count and its memory: a caller whose instances
 *   move changes them in place and refits, and rebuilds when the refitted
 *   tree has grown too costly (apmath_instance_bvh_get_cost()), with no
 *   allocation either way. A change of instance count is a new BVH.
 *
 * and "Out of scope: ... refitting a BVH in place ..." loses "refitting".
 * It gains: "Shapes: a sphere, a capsule and a box can be tested against a
 * mesh or against instances (overlaps) and moved along a line to where they
 * first touch one (sweeps); see APMATH_SHAPE."
 *
 * [CHANGED] @par Threads gains: set_instances changes the BVH: it may
 * overlap other set_instances calls on the same BVH whose indices are all
 * different (each writes only its instances' records), and nothing else on
 * it - no cast, test, get_, refit or rebuild. refit and rebuild may overlap
 * nothing on the BVH. Debug builds check both (APRI_ENTER on the BVH for
 * refit and rebuild; set_instances opens a shared section that refit,
 * rebuild and the casts check against). A cast, test or sweep made after a
 * set_instances and before the refit or rebuild that follows it is the
 * caller's error: the tree's boxes are the old ones, so it may miss what
 * moved.
 *
 * [CHANGED] @par Results gains: set_instances checks every instance before
 * changing any; on failure the BVH is as it was. refit, rebuild and the
 * queries can't fail.
 */

/* ---- Shapes ---------------------------------------------------------- */

/**
 * [ADDED] Which shape an APMATH_SHAPE is.
 *
 * Why: the three every engine compared offers for sweeps and overlaps
 * (Jolt, PhysX, Unreal, Godot, Hazel), and the three a support function
 * and a radius describe. 0 is "not chosen" and refused.
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
 * [ADDED] A sphere, a capsule or a box, in the coordinates of whatever it is
 * tested against.
 *
 * Why: the one description an overlap and a sweep take, in double precision
 * as the rays are. A struct for all three, the fields a kind doesn't use
 * left 0, rather than one struct per kind: one signature per call, and
 * bindable (no union). Turned by a unit quaternion, not a matrix: a shape
 * neither shears nor scales, and a matrix would make the caller prove it
 * doesn't.
 *
 * A shape is refused - meets nothing, as a NULL one - if its kind is none of
 * the enum's, a value it uses isn't finite, `radius`, `half_height` or a
 * `half_extents` is negative, a box's `radius` isn't 0, or `rotation` (for a
 * capsule or a box) is further than 1e-6 from unit length. A sphere ignores
 * `rotation`, `half_height` and `half_extents`; a capsule `half_extents`; a
 * box `half_height`. A sphere or capsule of radius 0 is a point or a segment,
 * and valid.
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
 * [ADDED] A shape moved along a line, without turning: the shape with its
 * center at `shape.center` + t * `direction`, for t from 0 to `t_max`.
 *
 * Why: what a sweep asks. t counts in lengths of `direction`, as a ray's
 * does, so a unit direction makes it a distance. A zero `direction` asks
 * only whether the shape overlaps at t = 0. A `t_max` below 0 or not a
 * number meets nothing; it may be INFINITY.
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
 * [ADDED] Where a sweep first touched a triangle.
 *
 * Why not APMATH_MESH_HIT or APMATH_INSTANCE_HIT: a swept shape touches at a
 * point beside its path, and the direction it is pushed back along (the
 * contact normal) is not the triangle's face normal where it touches an
 * edge or a corner. A caller sliding a character along a wall needs the
 * contact normal; one reading what material was hit needs the face. Both
 * engines compared that report one report the other too (Unreal's Normal
 * and ImpactNormal; Jolt's penetration axis and the face's normal).
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
	 * True if the shape overlapped the triangle at t = 0: then #t is 0, and
	 * #point and #contact_normal are those of the deepest point found, not a
	 * first touch (there was none). As PhysX's eINITIAL_OVERLAP and Unreal's
	 * bStartPenetrating.
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

/* ---- Mesh BVH: shapes ------------------------------------------------ */

/**
 * [ADDED] Whether a placed mesh touches a shape: some triangle within the
 * shape or on its surface. Exact against the triangles, to GJK's tolerance
 * (1e-12 of the larger of the shape's and the triangle's sizes), in double
 * precision. Stops at the first triangle found. Only the surface counts, as
 * apmath_mesh_bvh_overlaps_volume(): a shape wholly inside a closed mesh,
 * touching none of its triangles, is not found.
 *
 * Why: the per-mesh form of apmath_instance_bvh_overlaps_shape(), as
 * apmath_mesh_bvh_cast() is of the instance cast; and what a caller with
 * one mesh asks.
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
 *
 * Tests: brute force (above); a shape inside a closed box mesh, touching
 * nothing, is false; the transform's refusals.
 */
bool apmath_mesh_bvh_overlaps_shape(apmath_mesh_bvh bvh, const ApriDMat4 *transform, const APMATH_SHAPE *shape);

/**
 * [ADDED] Sweeps a shape against a placed mesh: the smallest t at which it
 * touches a triangle. Of triangles touched at the same t, the one earliest
 * in the desc's order.
 *
 * t is exact to GJK's tolerance; a touch is reported at the first t whose
 * distance is within it, so the shape stops at most that far short of the
 * surface, never inside it.
 *
 * Why: what moves something without passing through what it meets.
 *
 * Thread: as apmath_mesh_bvh_overlaps_shape(). Allocates nothing.
 *
 * @param[in]  bvh       The mesh. NULL is a mesh with no triangles.
 * @param[in]  transform As apmath_mesh_bvh_overlaps_shape().
 * @param[in]  sweep     The sweep, in the coordinates the transform carries
 *                       the mesh into.
 * @param[out] out_hit   The touch. May be NULL, to ask only whether there is
 *                       one; left alone when there is none. Its instance and
 *                       id are 0.
 *
 * @return true if the shape touches the mesh within the sweep.
 *
 * Tests: brute force; started_overlapping; a graze; a zero direction.
 */
bool apmath_mesh_bvh_sweep(apmath_mesh_bvh bvh, const ApriDMat4 *transform, const APMATH_SWEEP *sweep,
                           APMATH_SWEEP_HIT *out_hit);

/* ---- Instance BVH: shapes -------------------------------------------- */

/**
 * [ADDED] Whether any instance @p desc lets through touches a shape.
 *
 * Stops at the first found: the cheap form, for "is anything there".
 *
 * Thread: as apmath_instance_bvh_cast(). Allocates nothing.
 *
 * @param[in] bvh   The instances.
 * @param[in] shape The shape, in the instance BVH's coordinates.
 * @param[in] desc  Which instances to consider; NULL for all. Its filter is
 *                  asked about each instance whose box the shape's box
 *                  meets, at most once.
 *
 * @return true if one does.
 *
 * Tests: brute force; a mask and a filter that leave out the only one.
 */
bool apmath_instance_bvh_overlaps_shape(apmath_instance_bvh bvh, const APMATH_SHAPE *shape,
                                        const APMATH_CAST_DESC *desc);

/**
 * [ADDED] Which instances touch a shape, among those @p desc lets through,
 * in the desc's order: the lowest positions first, whatever the tree.
 *
 * Why the order: so the answer depends on the instances alone, never on
 * how the tree was built, refitted or rebuilt; a caller replays it.
 *
 * Thread: as apmath_instance_bvh_cast(). Allocates nothing: @p
 * out_instances is the working space. While there is room, positions are
 * appended as found; once it is full, it is kept as a max-heap of the
 * lowest @p instance_capacity found, each new position replacing the
 * heap's top if lower. At the end it is sorted. So n found cost about
 * n log(capacity), not the n times capacity an insertion into a sorted
 * buffer would (an explosion touching 1,000 instances).
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
 *
 * Tests: brute force; the order against a tree built from shuffled boxes; a
 * capacity of 0, 1 and one short of the count; 10,000 found into a
 * capacity of 100, the lowest 100 in order.
 */
uint32_t apmath_instance_bvh_overlap_all(apmath_instance_bvh bvh, const APMATH_SHAPE *shape,
                                         const APMATH_CAST_DESC *desc, uint32_t *out_instances,
                                         uint32_t instance_capacity);

/**
 * [ADDED] Sweeps a shape against an instance BVH: the smallest t at which it
 * touches an instance @p desc lets through. Of touches at the same t, the
 * earliest instance in the desc's order, then the earliest triangle.
 *
 * Thread: as apmath_instance_bvh_cast(). Allocates nothing.
 *
 * @param[in]  bvh     The instances.
 * @param[in]  sweep   The sweep, in the instance BVH's coordinates.
 * @param[in]  desc    Which instances to consider; NULL for all.
 * @param[out] out_hit The touch, its point and normals in the instance BVH's
 *                     coordinates. May be NULL; left alone when there is
 *                     none.
 *
 * @return true if the shape touches an instance within the sweep.
 *
 * Tests: brute force; tree independence; a mirrored instance's normals.
 */
bool apmath_instance_bvh_sweep(apmath_instance_bvh bvh, const APMATH_SWEEP *sweep, const APMATH_CAST_DESC *desc,
                               APMATH_SWEEP_HIT *out_hit);

/**
 * [ADDED] Sweeps a shape against an instance BVH, for every instance it
 * touches: one touch each, the instance's own first, ordered as
 * apmath_instance_bvh_sweep() would choose between them.
 *
 * Why: as apmath_instance_bvh_cast_all() for rays - what a caller asks to
 * move through some things and stop at others, or to list what a moving
 * shape passes through.
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
 *
 * Tests: brute force; the order; capacities as overlap_all's.
 */
uint32_t apmath_instance_bvh_sweep_all(apmath_instance_bvh bvh, const APMATH_SWEEP *sweep,
                                       const APMATH_CAST_DESC *desc, APMATH_SWEEP_HIT *out_hits,
                                       uint32_t hit_capacity);

/* ---- Instance BVH: changing it in place ------------------------------ */

/**
 * [ADDED] Replaces some of an instance BVH's instances: each named position
 * gets a new mesh, transform, id and mask, and its box is worked out again.
 * The tree is not changed: call apmath_instance_bvh_refit() or
 * apmath_instance_bvh_rebuild() before the next query.
 *
 * Why: the change a moving instance needs, without a new BVH. The count
 * stays the BVH's: a caller that has things come and go keeps positions
 * for them with a NULL mesh (never hit, no box) and sets one when it is
 * used. Taking positions, not a range, so a caller whose few movers are
 * scattered sets them alone; disjoint calls may run at once (Threads), so a
 * caller with many spreads them over its own threads.
 *
 * Every instance is checked before any is changed: on failure the BVH is
 * as it was.
 *
 * Thread: may overlap other calls of this function on @p bvh whose
 * @p indices are all different from this one's; nothing else on @p bvh.
 *
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
 * @param[out] error     Why it failed; NULL to skip.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p bvh,
 *         @p indices or @p instances (with @p count above 0), an index past
 *         the instance count, an index given twice, or an instance whose
 *         transform isn't finite, can't be inverted or whose last row isn't
 *         0, 0, 0, 1 - the message names which, and the position in
 *         @p indices.
 *
 * Two marks per instance, in two arrays: the check's (an index given
 * twice is found by a mark per instance, set as each index is read and
 * cleared after, so it costs the count, not its square) and refit's (the
 * instance moved since the last refit or rebuild, which refit reads and
 * clears). Kept apart, so a set clearing its check's marks never clears
 * what refit must carry up. Each call sets and clears only its own
 * indices' marks, one byte each, so calls on disjoint indices at once
 * never write the same byte.
 *
 * Tests: each refusal in parameter order, the BVH unchanged after each;
 * an index twice found at any distance apart; a set refused for an index
 * given twice after an earlier set of other instances, then refit: the
 * earlier set's instances carried up (its refit marks kept).
 */
APRESULT apmath_instance_bvh_set_instances(apmath_instance_bvh bvh, const uint32_t *indices,
                                           const APMATH_BVH_INSTANCE *instances, uint32_t count,
                                           APMATH_ERROR *error);

/**
 * [ADDED] Carries the boxes of the instances set since the last refit or
 * rebuild up the tree: every node above one gets the box around its
 * children again. The tree's shape is kept, so as instances move far from
 * where it was built it grows worse to walk (apmath_instance_bvh_get_cost()),
 * never wrong.
 *
 * Costs the nodes above the instances set: about their count times the
 * tree's depth when few were set, every node once when most were.
 *
 * Why: the cheap half of a moving scene's upkeep, as every engine compared
 * does between rebuilds.
 *
 * Thread: may overlap nothing on @p bvh. Allocates nothing.
 *
 * @param[in] bvh The BVH. NULL does nothing.
 *
 * Tests: a cast after set and refit finds the moved instance; nothing set
 * since leaves every node's bits as they were.
 */
void apmath_instance_bvh_refit(apmath_instance_bvh bvh);

/**
 * [ADDED] Builds the tree again over the instances as they are, as
 * apmath_instance_bvh_create() would build it over the same instances,
 * node for node, in the memory the BVH already has.
 *
 * Why: the dear half of the upkeep, for when refitting has let the tree
 * grow too costly - without a new BVH, and so without allocating: the
 * build's scratch is made with the BVH (its memory, stated at create,
 * grows by about 40 bytes an instance).
 *
 * Thread: may overlap nothing on @p bvh. Allocates nothing.
 *
 * @param[in] bvh The BVH. NULL does nothing.
 *
 * Tests: node for node the same as a fresh create over the same instances;
 * its cost the same bits.
 */
void apmath_instance_bvh_rebuild(apmath_instance_bvh bvh);

/**
 * [ADDED] The tree's surface-area cost: the sum, over its inner nodes, of
 * each node's box's area, over the root's area - the expected number of
 * boxes a ray through the root tests, by the surface-area heuristic the
 * build uses. Kept up to date by refit and rebuild, so reading it costs
 * nothing.
 *
 * Why: the one number a caller needs to choose between refit and rebuild by
 * cost, not by a rule of thumb or a count of steps: rebuild when it has
 * grown past some ratio of what it was after the last rebuild. The ratio
 * is the caller's (no default here); the numbers measured with this draft
 * go in APMATH_TODO.md to choose it by.
 *
 * Thread: as apmath_instance_bvh_get_bounds().
 *
 * @param[in] bvh The BVH.
 *
 * @return The cost, at least 0; 0 for a BVH with fewer than two instances
 *         with triangles, and for a NULL @p bvh.
 *
 * Tests: rises as refitted instances scatter; equal to a fresh create's
 * after a rebuild.
 */
double apmath_instance_bvh_get_cost(apmath_instance_bvh bvh);

/* ---- Box BVH: solid boxes -------------------------------------------- */

/*
 * [ADDED] A BVH over axis-aligned boxes, each a solid: what is inside a box
 * is in it. Queried by rays, shapes and sweeps as the instance BVH is, and
 * changed in place the same way, but each entry is a box the caller gives,
 * not a mesh placed.
 *
 * Why it earns its place: a caller that asks about boxes (what is near a
 * point, what a moving shape's region meets, which bounds a ray enters)
 * has no mesh to give an instance BVH, and a box made into twelve
 * triangles would be hollow (a shape wholly inside meets nothing), cost a
 * mesh walk per box, and answer with triangles that mean nothing. Every
 * engine compared keeps such a tree beside its meshes: Box2D v3's dynamic
 * tree, Jolt's and PhysX's broad phases, Bullet's dbvt. It is the instance
 * BVH's own tree, over boxes the caller gives: one tree code for both
 * (internal), so the build, refit, rebuild, cost and the sweep's walk are
 * the same and give the same bits.
 *
 * Solid: a ray whose start (t_min) is inside a box meets it at t_min; a
 * shape that overlaps a box's inside overlaps it, touching a face or not;
 * a sweep that starts overlapping one touches it at 0, started_overlapping.
 * A shape against a box is GJK's distance between the box and the shape's
 * core, as against a triangle (one algorithm, "How the shapes are tested").
 *
 * Results, ties and order as the instance BVH's: of hits at the same t, the
 * earliest position; lists in position order (overlaps) or by t then
 * position (sweep_all, cast_all); never the tree's order.
 *
 * Threads, as the instance BVH's (its @par Threads as this draft changes
 * it): queries may overlap each other; set_boxes calls on disjoint
 * positions may overlap each other and nothing else; refit and rebuild
 * overlap nothing. Debug checks it the same way.
 *
 * Memory: about 160 bytes a box (its box and id in doubles and words, its
 * place in the order, about one node a box at 56 bytes, the rebuild's
 * scratch and the two marks), stated in apmath_box_bvh_create's comment.
 *
 * ABI: additions only - a handle, two structs (frozen), a desc (based,
 * APRI_STRUCT_TYPE_APMATH_BOX_BVH_DESC = 4006 in ApMath's block), a hit
 * (frozen) and the calls below. The misuse factories gain the handle,
 * with APRI_STALE_TAG on its struct.
 *
 * Tests (tests/apmathbvh_tests.c): every query against brute force over
 * every box (a slow exact test per box: slab for rays, GJK's brute
 * distance for shapes, a bisected march for sweeps); starts inside (a ray
 * from a box's middle meets it at t_min, normal -direction; a sphere
 * wholly inside overlaps; a sweep from inside at 0, started_overlapping);
 * an empty box never met; tree independence (create, set and refit,
 * rebuild, fresh create: the same bits); set_boxes' refusals in parameter
 * order, nothing changed; aprinoalloc_tests (queries, set, refit and
 * rebuild of 100,000 boxes); aprioverlap_tests (queries from two threads;
 * set on disjoint positions from two); the misuse harness regenerated;
 * apmathbvh_bench beside the instance BVH's.
 */

/** @brief [ADDED] Opaque handle to a box BVH: solid axis-aligned boxes. */
typedef struct apmath_box_bvh_t *apmath_box_bvh;

/**
 * @brief [ADDED] One box of a box BVH.
 */
/* Frozen: an array element. It never changes; a new need gets a new type. */
typedef struct APMATH_BVH_BOX {
	/** The box, in the BVH's coordinates. One whose lo is above its hi on any axis is empty: never met, no place in the tree. */
	APMATH_AABB box;
	/** The caller's, handed back in hits and to the filter. Needn't be unique. */
	uint64_t id;
	/** Which queries consider the box, as APMATH_BVH_INSTANCE::mask. */
	uint32_t mask;
} APMATH_BVH_BOX;

/**
 * @brief [ADDED] Configuration descriptor for a box BVH.
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
 * @brief [ADDED] Where a ray or a sweep met a box, in the BVH's
 * coordinates.
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
 * @brief [ADDED] Creates a box BVH over the desc's boxes. Allocates: about
 * 160 bytes a box. Nothing it does after allocates.
 *
 * @param[in]  desc    The boxes. Required; its base checked.
 * @param[out] out_bvh Receives the BVH; NULL on failure. Required.
 * @param[out] error   Why it failed; NULL to skip.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT for a NULL @p desc or
 *         @p out_bvh, NULL boxes with a count above 0, or a box not finite
 *         (the message names its position); the struct codes for its base;
 *         APRESULT_OUT_OF_MEMORY.
 */
APRESULT apmath_box_bvh_create(const APMATH_BOX_BVH_DESC *desc, apmath_box_bvh *out_bvh, APMATH_ERROR *error);

/** @brief [ADDED] Destroys a box BVH. NULL is ignored. */
void apmath_box_bvh_destroy(apmath_box_bvh bvh);

/**
 * @brief [ADDED] Replaces some boxes, as apmath_instance_bvh_set_instances()
 * replaces instances: checked before any changes, the tree changed only by
 * the refit or rebuild that follows. Thread and marks as set_instances'.
 *
 * @return As apmath_instance_bvh_set_instances(), a box not finite in
 *         place of a transform's refusals.
 */
APRESULT apmath_box_bvh_set_boxes(apmath_box_bvh bvh, const uint32_t *indices, const APMATH_BVH_BOX *boxes,
                                  uint32_t count, APMATH_ERROR *error);

/** @brief [ADDED] As apmath_instance_bvh_refit(), for boxes. */
void apmath_box_bvh_refit(apmath_box_bvh bvh);

/** @brief [ADDED] As apmath_instance_bvh_rebuild(), for boxes. */
void apmath_box_bvh_rebuild(apmath_box_bvh bvh);

/** @brief [ADDED] As apmath_instance_bvh_get_cost(), for boxes. */
double apmath_box_bvh_get_cost(apmath_box_bvh bvh);

/**
 * @brief [ADDED] The nearest box a ray meets, a box its start is inside
 * met at t_min. Of boxes met at the same t, the earliest position.
 *
 * @param[out] out_hit The meeting; may be NULL, to ask only whether there
 *                     is one (the walk then stops at the first found).
 * @return true if the ray meets a box @p desc lets through.
 */
bool apmath_box_bvh_cast(apmath_box_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                         APMATH_BOX_HIT *out_hit);

/**
 * @brief [ADDED] Every box a ray meets, one meeting each, by t then
 * position; the caller's buffer kept as overlap_all's.
 *
 * @return How many boxes the ray meets, whatever @p hit_capacity is.
 */
uint32_t apmath_box_bvh_cast_all(apmath_box_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                                 APMATH_BOX_HIT *out_hits, uint32_t hit_capacity);

/** @brief [ADDED] Whether any box overlaps a shape (a solid: inside counts). */
bool apmath_box_bvh_overlaps_shape(apmath_box_bvh bvh, const APMATH_SHAPE *shape, const APMATH_CAST_DESC *desc);

/**
 * @brief [ADDED] Which boxes overlap a shape, in position order; the
 * caller's buffer kept as apmath_instance_bvh_overlap_all()'s.
 *
 * @return How many boxes overlap it, whatever @p box_capacity is.
 */
uint32_t apmath_box_bvh_overlap_all(apmath_box_bvh bvh, const APMATH_SHAPE *shape, const APMATH_CAST_DESC *desc,
                                    uint32_t *out_boxes, uint32_t box_capacity);

/** @brief [ADDED] The first box a sweep touches; one it starts in at 0, started_inside. */
bool apmath_box_bvh_sweep(apmath_box_bvh bvh, const APMATH_SWEEP *sweep, const APMATH_CAST_DESC *desc,
                          APMATH_BOX_HIT *out_hit);

/**
 * @brief [ADDED] Every box a sweep touches, one touch each, by t then
 * position; the caller's buffer kept as overlap_all's.
 *
 * @return How many boxes the sweep touches, whatever @p hit_capacity is.
 */
uint32_t apmath_box_bvh_sweep_all(apmath_box_bvh bvh, const APMATH_SWEEP *sweep, const APMATH_CAST_DESC *desc,
                                  APMATH_BOX_HIT *out_hits, uint32_t hit_capacity);

/*
 * [CHANGED] apmath_instance_bvh_cast(): with out_hit NULL ("to ask only
 * whether the ray hits") the cast now stops at the first hit it finds,
 * rather than finding the nearest and discarding it. The contract already
 * said this is all such a call learns; no ABI change. Measured before and
 * after.
 *
 * [CHANGED] apmath_instance_bvh_create(): its memory grows by the rebuild's
 * scratch (about 40 bytes an instance), the refit's marks (1 byte a node
 * and an instance) and set_instances' check's marks (1 byte an instance).
 * Stated in its comment, with the whole: about 245 bytes
 * an instance (its record in doubles and its place in the order) and 56 a
 * node, about one node an instance, today; so about 350 bytes an instance
 * in all.
 */

/* ===========================================================================
 * apmathpoly.h  [CHANGED]
 * ========================================================================= */

/*
 * APMATH_ERROR_MESSAGE_SIZE and APMATH_ERROR leave it for apmatherror.h,
 * which it includes. Nothing else changes.
 */

/* ===========================================================================
 * apricore.h  [CHANGED]
 * ========================================================================= */

/*
 * APRI_STRUCT_TYPE, in ApMath's block, after
 * APRI_STRUCT_TYPE_APMATH_POLY_SKELETON_DESC:
 *
 *     [ADDED] APRI_STRUCT_TYPE_APMATH_BOX_BVH_DESC = 4006,
 *         APMATH_BOX_BVH_DESC.
 */
