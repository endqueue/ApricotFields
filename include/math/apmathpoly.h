/**
 * @file apmathpoly.h
 * @brief ApricotMath's plane polygons - booleans, checks and triangulation
 * of polygons with holes, exactly, on an integer grid.
 *
 * Built in src/math/apmathpoly.cpp, with headless tests in
 * tests/apmathpoly_tests.c. It is not part of
 * aprend.h, which is Aprend's alone.
 *
 * Coordinates are integers. What one unit is - a micrometre, a pixel - is
 * the caller's: it rounds its own coordinates onto the grid, and every
 * result lands on the same grid. Integers are what make a result the same
 * on every platform and every run, given the same input: booleans compute
 * exactly where they can, and where an intersection falls between grid
 * points it's rounded the same way everywhere.
 *
 * Every edge carries a tag: a 64-bit value that is the caller's, handed
 * through a boolean to the edges of the result that lie along it. That's
 * how a caller knows which of its edges each edge of a result came from,
 * however the boolean cut, merged or reordered them.
 *
 * General purpose: it knows nothing about what a polygon outlines or what
 * a tag names.
 *
 * Out of scope: curves (a caller tessellates them; an offset's round
 * corners are the one arc made here, to the caller's tolerance), polylines
 * that aren't closed, and floating-point coordinates.
 *
 * @par Threads
 * apricore.h's rule ("Threads"), with one exception: a result never
 * changes once made, so reads of one result may overlap, from any number of
 * threads (apmath_poly_get_set() and the calls given its polygons), and it
 * is destroyed once nothing reads it. Every other call reads only its
 * arguments, so any of them may run at once.
 *
 * @par Results
 * Calls return APRESULT (apricore.h). The general codes mean, here:
 * - APRESULT_INVALID_ARGUMENT: also a coordinate past
 *   APMATH_POLY_COORD_LIMIT, ring ends that aren't in order or don't end at
 *   the point count, a ring outer that isn't an outer ring, an operation or
 *   fill rule that isn't one of the enum's.
 * - APRESULT_OUT_OF_MEMORY: an allocation failed.
 *
 * @par
 * A call that can fail for a reason worth a sentence takes an APMATH_ERROR
 * pointer (NULL to skip it): on failure it holds a description; on success
 * its message is empty.
 *
 * @par Struct bases
 * Descs start with a `base` (apricore.h, "Struct bases"): the caller sets
 * `base.type` to the desc's own APRI_STRUCT_TYPE_APMATH_* and `base.next` to
 * NULL. A wrong type is APRESULT_STRUCT_WRONG_TYPE and a chained struct
 * APRESULT_STRUCT_UNKNOWN_IN_CHAIN: nothing chains onto any of them yet.
 * APMATH_POLY_POINT, APMATH_POLY_SET, APMATH_POLY_DEFECT_INFO and
 * APMATH_ERROR are frozen: they never grow (a new need gets a new type).
 *
 * @par Geometry
 * Y up: counter-clockwise turns left. A ring is closed - its last point
 * joins its first - and doesn't repeat its first point at the end. Edge i
 * of a ring runs from its point i to point i + 1, wrapping.
 */

#ifndef APMATHPOLY_H
#define APMATHPOLY_H

#include "../apricore.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/* ---- Conventions ----------------------------------------------------- */

/** @brief The size of APMATH_ERROR's message buffer, its NUL included. */
#define APMATH_ERROR_MESSAGE_SIZE 256

/**
 * @brief Why a call failed, filled by calls that take one (see "Results"
 * above).
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct APMATH_ERROR {
	/** NUL-terminated, truncated to fit. Empty on success. */
	char message[APMATH_ERROR_MESSAGE_SIZE];
} APMATH_ERROR;

/* ---- Polygon sets ---------------------------------------------------- */

/**
 * @brief The largest coordinate magnitude, in either axis: 2^40.
 *
 * Leaves the products exact predicates need within 128 bits.
 */
#define APMATH_POLY_COORD_LIMIT ((int64_t)1 << 40)

/** @brief In APMATH_POLY_SET::ring_outers: the ring is an outer ring. */
#define APMATH_POLY_NO_RING UINT32_MAX

/** @brief A point on the integer grid. */
/* Frozen: a value, an array element. It never changes; a new need gets a new type. */
typedef struct APMATH_POLY_POINT {
	/** In grid units; magnitude at most APMATH_POLY_COORD_LIMIT. */
	int64_t x;
	/** In grid units, up; magnitude at most APMATH_POLY_COORD_LIMIT. */
	int64_t y;
} APMATH_POLY_POINT;

/**
 * @brief Rings of points, read in place - nothing here owns the arrays.
 *
 * Ring i is points [`ring_ends[i - 1]`, `ring_ends[i]`) - from 0 for the
 * first ring - so `ring_ends` runs in order and its last value is
 * `point_count`.
 *
 * `edge_tags` holds one tag per point: the tag of the edge that starts at
 * that point. NULL tags every edge 0.
 *
 * `ring_outers` says how rings group into polygons with holes: for an outer
 * ring APMATH_POLY_NO_RING, for a hole the index of the outer ring it's a
 * hole in. NULL makes every ring an outer ring. A boolean ignores it - its
 * fill rule decides what's inside - and fills it in on what it returns.
 *
 * All zeros is an empty set.
 */
/* Frozen: held by value in the descs. It never changes; a new need gets a new type. */
typedef struct APMATH_POLY_SET {
	/** Every ring's points, ring after ring. */
	const APMATH_POLY_POINT *points;
	/** #point_count of them, or NULL. */
	const uint64_t *edge_tags;
	/** How many #points there are. */
	uint32_t point_count;
	/** #ring_count of them: where each ring's points end. */
	const uint32_t *ring_ends;
	/** #ring_count of them, or NULL. */
	const uint32_t *ring_outers;
	/** How many rings there are. */
	uint32_t ring_count;
} APMATH_POLY_SET;

/* ---- Checking -------------------------------------------------------- */

/**
 * @brief What makes a set something other than polygons with holes.
 *
 * The fields named in each are APMATH_POLY_DEFECT_INFO's.
 */
typedef enum APMATH_POLY_DEFECT APRI_ENUM_BASE {
	/** Nothing: the set is polygons with holes. */
	APMATH_POLY_DEFECT_NONE = 0,
	/** `ring` has fewer than three points. */
	APMATH_POLY_DEFECT_TOO_FEW_POINTS,
	/** Edge `edge` of `ring` has no length: its two points are the same. */
	APMATH_POLY_DEFECT_ZERO_LENGTH_EDGE,
	/** Every point of `ring` is on one line: it encloses nothing. */
	APMATH_POLY_DEFECT_NO_AREA,
	/**
	 * Edge `edge` of `ring` touches or crosses edge `other_edge` of
	 * `other_ring` (which may be the same ring), other than where two edges
	 * that follow each other share their point - so a ring that crosses or
	 * touches itself, a spike that doubles back along itself, and two rings
	 * that touch are all this.
	 */
	APMATH_POLY_DEFECT_EDGES_MEET,
	/** Outer ring `ring` runs clockwise, or hole `ring` counter-clockwise. */
	APMATH_POLY_DEFECT_WRONG_WINDING,
	/** Hole `ring` isn't inside its outer ring, `other_ring`. */
	APMATH_POLY_DEFECT_HOLE_OUTSIDE,
	/**
	 * `ring` is inside `other_ring`'s polygon - two polygons overlap, or a
	 * hole is inside another hole of the same polygon.
	 */
	APMATH_POLY_DEFECT_OVERLAP,
} APMATH_POLY_DEFECT;

/**
 * @brief The first defect apmath_poly_validate() found, and where.
 *
 * Fields a defect doesn't name are 0.
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct APMATH_POLY_DEFECT_INFO {
	/** Which defect, or APMATH_POLY_DEFECT_NONE. */
	APMATH_POLY_DEFECT defect;
	/** The ring, as an index into the set's rings. */
	uint32_t ring;
	/** The edge's position in its ring. */
	uint32_t edge;
	/** The other ring, as an index into the set's rings. */
	uint32_t other_ring;
	/** The other edge's position in #other_ring. */
	uint32_t other_edge;
} APMATH_POLY_DEFECT_INFO;

/**
 * @brief Checks whether a set is polygons with holes.
 *
 * Polygons with holes means: every ring simple (it doesn't touch or cross
 * itself) and enclosing something, outer rings counter-clockwise and holes
 * clockwise, no two rings touching or crossing, each hole inside its own
 * outer ring, and no polygon's inside overlapping another's. Collinear
 * points along an edge are allowed. Costs the square of the edge count.
 *
 * @param[in]  set        The set to check.
 * @param[out] out_defect The first defect found - checked in the order of
 *                        the enum, rings and edges in order - or
 *                        APMATH_POLY_DEFECT_NONE.
 * @param[out] out_error  Why the call failed. May be NULL.
 *
 * @return APRESULT_OK whether or not @p set is polygons with holes. The
 *         other results are for a @p set that can't be read (see "Results"
 *         above).
 */
APRESULT apmath_poly_validate(const APMATH_POLY_SET *set, APMATH_POLY_DEFECT_INFO *out_defect, APMATH_ERROR *out_error);

/* ---- Booleans -------------------------------------------------------- */

/** @brief A boolean operation: which points the result encloses. */
typedef enum APMATH_POLY_OP APRI_ENUM_BASE {
	/** Inside either. */
	APMATH_POLY_OP_UNION = 0,
	/** Inside both. */
	APMATH_POLY_OP_INTERSECTION = 1,
	/** Inside the subject, outside the clip. */
	APMATH_POLY_OP_DIFFERENCE = 2,
	/** Inside exactly one. */
	APMATH_POLY_OP_XOR = 3,
} APMATH_POLY_OP;

/**
 * @brief Which points a set's rings enclose, by the winding number - how
 * many times the rings go round the point counter-clockwise, less
 * clockwise.
 */
typedef enum APMATH_POLY_FILL APRI_ENUM_BASE {
	/** Odd. */
	APMATH_POLY_FILL_EVEN_ODD = 0,
	/** Not zero. */
	APMATH_POLY_FILL_NON_ZERO = 1,
	/** Above zero. */
	APMATH_POLY_FILL_POSITIVE = 2,
} APMATH_POLY_FILL;

/** @brief Configuration descriptor for apmath_poly_boolean(). */
typedef struct APMATH_POLY_BOOLEAN_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APMATH_POLY_BOOLEAN_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/** The operation. */
	APMATH_POLY_OP op;
	/** The fill rule. Applies to both sets. */
	APMATH_POLY_FILL fill;
	/** The first set. */
	APMATH_POLY_SET subject;
	/**
	 * The second set. May be empty: a union with an empty clip is the
	 * subject cleaned up - overlaps merged, self-crossings resolved, rings
	 * oriented.
	 */
	APMATH_POLY_SET clip;
} APMATH_POLY_BOOLEAN_DESC;

/** @brief Opaque handle to a result: polygons that own their arrays. */
typedef struct apmath_poly_t *apmath_poly;

/**
 * @brief Builds the boolean of a desc's two sets.
 *
 * The result is polygons with holes that apmath_poly_validate() finds
 * nothing wrong with, except where rounding an intersection onto the grid
 * has left two rings touching:
 * - outer rings counter-clockwise, holes clockwise, each hole listed
 *   straight after its outer ring (or after an earlier hole of the same
 *   ring); a polygon standing in another's hole comes after that other's
 *   holes
 * - no edge of no length, and no point in the middle of a straight run
 * - each ring starting at its lowest point, the leftmost of those
 * - each edge tagged with the tag of the input edge it lies along: one it
 *   overlaps, with both its ends on that edge's line to within a unit
 *   (rounding an intersection moves a point up to half a unit in each
 *   axis). Where it lies along several - where collinear input edges
 *   merged - the one whose line is nearest, then the first: the subject's
 *   before the clip's, each in ring and edge order
 *
 * An empty result is valid: a set with no rings.
 *
 * The same desc gives the same result, point for point, on every platform.
 * Costs about (n + k) log n for n edges in and k crossings, plus the
 * product of the edge counts in and out for the tags.
 *
 * @param[in]  desc      The operation and its two sets. Only read during
 *                       the call.
 * @param[out] out_poly  The result, or NULL on failure.
 * @param[out] out_error Why the call failed. May be NULL.
 *
 * @return APRESULT_OK, or a failure as in "Results" above.
 *
 * @see apmath_poly_get_set(), apmath_poly_destroy()
 */
APRESULT apmath_poly_boolean(const APMATH_POLY_BOOLEAN_DESC *desc, apmath_poly *out_poly, APMATH_ERROR *out_error);
/**
 * @brief Destroys a result.
 *
 * @param[in] poly The result to destroy. NULL is fine.
 */
void apmath_poly_destroy(apmath_poly poly);

/**
 * @brief Reads a result's polygons.
 *
 * Thread: may overlap any other read of @p poly, from any thread; not its
 * destroy.
 *
 * @param[in] poly The result.
 *
 * @return @p poly's polygons, reading its own arrays: valid until it's
 *         destroyed. All zeros for NULL.
 */
APMATH_POLY_SET apmath_poly_get_set(apmath_poly poly);

/* ---- Locating a point ------------------------------------------------ */

/** @brief Where a point lies against a set: what apmath_poly_locate_point() finds. */
typedef enum APMATH_POLY_PLACE APRI_ENUM_BASE {
	/** Outside what the set encloses, and on none of its edges. */
	APMATH_POLY_PLACE_OUTSIDE = 0,
	/** Inside what the set encloses, and on none of its edges. */
	APMATH_POLY_PLACE_INSIDE = 1,
	/** On an edge of the set, its ends included. */
	APMATH_POLY_PLACE_ON_EDGE = 2,
} APMATH_POLY_PLACE;

/**
 * @brief Finds whether a point is inside what a set's rings enclose under a
 * fill rule, outside it, or on one of its edges.
 *
 * Exact: the point's winding number is counted with integer orientation
 * tests, so the answer is the same on every platform. Any rings are taken -
 * crossing, overlapping, either winding - and `ring_outers` isn't read: the
 * fill rule decides, as in a boolean. Costs the set's edge count.
 *
 * @param[in]  set       The rings.
 * @param[in]  fill      Which winding numbers are inside.
 * @param[in]  point     The point; within APMATH_POLY_COORD_LIMIT.
 * @param[out] out_place Receives where it lies.
 * @param[out] out_error Why the call failed. May be NULL.
 *
 * @return APRESULT_OK, or a failure as in "Results" above.
 *         APRESULT_INVALID_ARGUMENT also if @p out_place is NULL or
 *         @p point is past the coordinate limit.
 */
APRESULT apmath_poly_locate_point(const APMATH_POLY_SET *set, APMATH_POLY_FILL fill, APMATH_POLY_POINT point,
                                  APMATH_POLY_PLACE *out_place, APMATH_ERROR *out_error);

/* ---- Offsetting ------------------------------------------------------ */

/** @brief How apmath_poly_offset_edges() fills a corner the move opens. */
typedef enum APMATH_POLY_JOIN APRI_ENUM_BASE {
	/** Mitred, bevelled past the mitre limit. */
	APMATH_POLY_JOIN_MITER = 0,
	/**
	 * An arc round the original corner, cut into straight edges no more
	 * than `arc_tolerance` short of it.
	 */
	APMATH_POLY_JOIN_ROUND = 1,
} APMATH_POLY_JOIN;

/** @brief Configuration descriptor for apmath_poly_offset_edges(). */
typedef struct APMATH_POLY_OFFSET_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APMATH_POLY_OFFSET_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * Polygons with holes (apmath_poly_validate()); its `ring_outers` isn't
	 * read - what each ring encloses follows from its winding.
	 */
	APMATH_POLY_SET set;
	/**
	 * One per point: how far the edge starting there moves, in grid units,
	 * square to itself. Positive moves it away from what the polygon
	 * encloses - out from an outer ring, into a hole - and negative in; 0
	 * leaves it where it is. Finite.
	 */
	const double *distances;
	/**
	 * Where two moved edges meet at a corner, they're extended to meet
	 * (mitred) unless the meeting point would be further from the corner
	 * than this many times the larger of the two distances; then the corner
	 * is cut across from one moved edge's end to the other's (bevelled). At
	 * least 1. A right angle moved by d on both sides mitres within sqrt(2).
	 * Read for every corner the move doesn't open, and with
	 * APMATH_POLY_JOIN_MITER for every corner.
	 */
	double miter_limit;
	/**
	 * How a corner the move opens is filled: one where the two moved edges
	 * part, as an outer ring's convex corner moved out does. A corner the
	 * move closes is mitred either way. 0 is APMATH_POLY_JOIN_MITER.
	 */
	APMATH_POLY_JOIN join;
	/**
	 * With APMATH_POLY_JOIN_ROUND, the most, in grid units, that an edge of
	 * a round corner may fall short of the true arc. Above 0 and finite
	 * there; not read with APMATH_POLY_JOIN_MITER.
	 */
	double arc_tolerance;
} APMATH_POLY_OFFSET_DESC;

/**
 * @brief Moves each edge of a desc's set by its own distance and re-meets
 * the corners.
 *
 * Corners are mitred, bevelled past the mitre limit, and stepped where two
 * edges that run on in one line move by different distances (a short edge
 * square to them joins the two). With APMATH_POLY_JOIN_ROUND, a corner the
 * move opens is an arc centred on the original corner instead, from the
 * end of one moved edge to the start of the next, its radius going from
 * the one distance to the other where the two differ. An arc is cut by
 * halving its angle as often as the tolerance needs, every piece alike,
 * with nothing but arithmetic and square roots, so it is the same on every
 * platform.
 *
 * An edge the move shortens to nothing is gone: as if the move ran from 0
 * to its whole distance, each edge whose two ends meet on the way is taken
 * out when they meet, and the edges either side of it meet directly from
 * there - mitred whatever the limit, or stepped across where they're
 * parallel. So a ring moved in past its own width leaves nothing, and a
 * notch grown shut leaves no trace. What the moved rings enclose is then
 * taken as by a union with no clip under the POSITIVE rule, so a ring
 * folded over on itself by a large inward move loses the fold, and two
 * polygons grown into each other merge. A ring the move pinches in two (a
 * narrow waist moved in past its half width) is resolved by that union,
 * not by taking edges out.
 *
 * The result is a boolean's (apmath_poly_boolean()): normalised the same
 * way, and each edge tagged with the tag of the input edge it was moved
 * from. A bevel, a step, or an arc's edges, take the tag of the edge after
 * it. The same
 * desc gives the same result on every platform: moved corners are computed
 * in doubles and rounded to the nearest grid point.
 *
 * @param[in]  desc      The set, its distances and the mitre limit. Only
 *                       read during the call.
 * @param[out] out_poly  The result, or NULL on failure.
 * @param[out] out_error Why the call failed. May be NULL.
 *
 * @return APRESULT_OK, or a failure as in "Results" above.
 *         APRESULT_INVALID_ARGUMENT also for a moved corner past the
 *         coordinate limit, a `join` that isn't an APMATH_POLY_JOIN, or an
 *         `arc_tolerance` that isn't above 0 and finite with
 *         APMATH_POLY_JOIN_ROUND.
 */
APRESULT apmath_poly_offset_edges(const APMATH_POLY_OFFSET_DESC *desc, apmath_poly *out_poly, APMATH_ERROR *out_error);

/* ---- Straight skeleton ----------------------------------------------- */

/** @brief Configuration descriptor for apmath_poly_skeleton(). */
typedef struct APMATH_POLY_SKELETON_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APMATH_POLY_SKELETON_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * Polygons with holes (apmath_poly_validate()): outer rings
	 * counter-clockwise, holes clockwise, grouped by `ring_outers`.
	 */
	APMATH_POLY_SET set;
	/**
	 * One per point: how fast the edge starting there moves inward, square
	 * to itself, in grid units per unit of time. At least 0, finite, and not
	 * all 0. An edge with weight 0 stays where it is, and the edges beside
	 * it slide along it. Two edges that follow each other on one line -
	 * parallel by the rule in apmath_poly_skeleton() - must have the same
	 * weight.
	 */
	const double *weights;
} APMATH_POLY_SKELETON_DESC;

/**
 * @brief Builds the weighted straight skeleton of a desc's polygon.
 *
 * Every edge moves inward at its weight - into its polygon, so a hole's
 * edges move out into the polygon round it - corners follow where their two
 * edges meet, and where the moving edges collide the polygon shrinks,
 * splits, joins round a hole and closes up. The region an edge sweeps
 * before it's gone is its face.
 *
 * Where two edges that weren't neighbours come to follow each other on one
 * line mid-way (an edge between them gone), they move on as one edge: the
 * one with the larger weight takes the line, and on equal weights the one
 * earlier in the input; the other's face ends there, along the line. Every
 * platform builds the same skeleton by that rule.
 *
 * The result holds one ring per input edge, in input order, each that
 * edge's face: counter-clockwise, starting with the edge itself (its point
 * 0 and 1 are the edge's start and end), then back round its boundary. The
 * first edge of a face keeps the input edge's tag; every other edge of it
 * is tagged with the tag of the input edge whose face is across it - so a
 * caller knows which two faces meet along it. `ring_outers` is
 * APMATH_POLY_NO_RING for every ring, and the rings aren't a set
 * apmath_poly_validate() accepts: faces share edges, and a face of an edge
 * with weight 0 has no area (it runs along the edge and back).
 *
 * Time t is only the parameter the edges move by: a point's time is when
 * the first edge reached it, so with every weight 1 it's the point's
 * distance from the boundary measured along the skeleton. A caller that
 * reads time as height gets a roof: each face then lies in a plane through
 * its edge, rising at 1 / weight.
 *
 * Corners are found in doubles and rounded to the grid; two edges that
 * follow each other count as parallel when the cross product of their unit
 * directions is less than 1e-12 in magnitude. Where a face's boundary
 * passes a point where more than two of its traces meet, the walk takes
 * the trace that turns most to the right, which keeps the face on its
 * left. The same desc gives the same result on every platform. Costs about
 * the cube of the edge count.
 *
 * @param[in]  desc      The polygon and its weights. Only read during the
 *                       call.
 * @param[out] out_poly  The faces, or NULL on failure.
 * @param[out] out_error Why the call failed. May be NULL.
 *
 * @return APRESULT_OK, or a failure as in "Results" above.
 *         APRESULT_INVALID_ARGUMENT also for a set that isn't polygons
 *         with holes, and for a polygon whose skeleton can't be traced (a
 *         degenerate case the message names).
 */
APRESULT apmath_poly_skeleton(const APMATH_POLY_SKELETON_DESC *desc, apmath_poly *out_poly, APMATH_ERROR *out_error);

/* ---- Triangulation --------------------------------------------------- */

/**
 * @brief The most indices apmath_poly_triangulate() can write for a set.
 *
 * Three for each of a polygon's points, less two for it, plus two for each
 * of its holes.
 *
 * @param[in] set The set.
 *
 * @return The index count to make room for. 0 for NULL.
 */
uint32_t apmath_poly_get_triangulation_capacity(const APMATH_POLY_SET *set);

/**
 * @brief Triangulates each of a set's polygons - an outer ring with its
 * holes, as the set's `ring_outers` groups them.
 *
 * Writes three indices into @p set's points per triangle. Triangles run
 * counter-clockwise; none has no area, so collinear points can leave fewer
 * than the capacity.
 *
 * @p set must be polygons with holes (apmath_poly_validate()). Given one
 * that isn't, it still writes only indices of @p set's points, but which
 * triangles is unspecified. The same @p set gives the same triangles on
 * every platform.
 *
 * @param[in]  set             The polygons.
 * @param[out] out_indices     Receives the indices.
 * @param[in]  index_capacity  How many indices @p out_indices has room for.
 * @param[out] out_index_count How many indices were written.
 * @param[out] out_error       Why the call failed. May be NULL.
 *
 * @return APRESULT_OK, or a failure as in "Results" above.
 *         APRESULT_INVALID_ARGUMENT if @p index_capacity is below
 *         apmath_poly_get_triangulation_capacity().
 */
APRESULT apmath_poly_triangulate(const APMATH_POLY_SET *set, uint32_t *out_indices, uint32_t index_capacity,
                                 uint32_t *out_index_count, APMATH_ERROR *out_error);

#if __cplusplus
}
#endif

#endif // APMATHPOLY_H
