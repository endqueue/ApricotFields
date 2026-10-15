/*
 * Built in src/math/apmathpoly.cpp, with headless tests in
 * tests/apmathpoly_tests.c. apricotfields.h doesn't include it.
 */

#ifndef APMATHPOLY_H
#define APMATHPOLY_H

#include "../apricore.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/*
 * ApricotMath's plane polygons - booleans, checks and triangulation of
 * polygons with holes, exactly, on an integer grid.
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
 * Out of scope: curves (a caller tessellates them), round joins, polylines
 * that aren't closed, and floating-point coordinates.
 */

/* ---- Conventions ----------------------------------------------------- */

/*
 * Threads. Every call is safe from any number of threads at once; a result
 * may be read from any thread, and destroyed once nothing reads it.
 *
 * Results. Calls return APRESULT (apricore.h). The general codes mean, here:
 *   APRESULT_INVALID_ARGUMENT  also a coordinate past APMATH_POLY_COORD_LIMIT,
 *                              ring ends that aren't in order or don't end at
 *                              the point count, a ring outer that isn't an
 *                              outer ring, an operation or fill rule that
 *                              isn't one of the enum's
 *   APRESULT_OUT_OF_MEMORY     an allocation failed
 * A call that can fail for a reason worth a sentence takes an APMATH_ERROR
 * pointer (NULL to skip it): on failure it holds a description; on success
 * its message is empty.
 *
 * Struct versions. Descs start with [struct_size], set to sizeof(the
 * struct) as compiled; fields past it read as zero, as in aparchive.h.
 * APMATH_POLY_POINT, APMATH_POLY_SET, APMATH_POLY_DEFECT_INFO and
 * APMATH_ERROR are frozen: they never grow (a new need gets a new type).
 *
 * Geometry. Y up: counter-clockwise turns left. A ring is closed - its last
 * point joins its first - and doesn't repeat its first point at the end.
 * Edge i of a ring runs from its point i to point i + 1, wrapping.
 */

#define APMATH_ERROR_MESSAGE_SIZE 256

typedef struct APMATH_ERROR {
	char message[APMATH_ERROR_MESSAGE_SIZE]; /* NUL-terminated, truncated to fit */
} APMATH_ERROR;

/* ---- Polygon sets ---------------------------------------------------- */

/* The largest coordinate magnitude, in either axis: 2^40. Leaves the
 * products exact predicates need within 128 bits. */
#define APMATH_POLY_COORD_LIMIT ((int64_t)1 << 40)

/* In APMATH_POLY_SET::ring_outers: the ring is an outer ring. */
#define APMATH_POLY_NO_RING UINT32_MAX

typedef struct APMATH_POLY_POINT {
	int64_t x;
	int64_t y;
} APMATH_POLY_POINT;

/* Rings of points, read in place - nothing here owns the arrays.
 *
 * Ring i is points [ring_ends[i - 1], ring_ends[i]) - from 0 for the first
 * ring - so [ring_ends] runs in order and its last value is [point_count].
 *
 * [edge_tags] holds one tag per point: the tag of the edge that starts at
 * that point. NULL tags every edge 0.
 *
 * [ring_outers] says how rings group into polygons with holes: for an outer
 * ring APMATH_POLY_NO_RING, for a hole the index of the outer ring it's a
 * hole in. NULL makes every ring an outer ring. A boolean ignores it - its
 * fill rule decides what's inside - and fills it in on what it returns.
 *
 * All zeros is an empty set. */
typedef struct APMATH_POLY_SET {
	const APMATH_POLY_POINT *points;
	const uint64_t *edge_tags; /* [point_count] of them, or NULL */
	uint32_t point_count;
	const uint32_t *ring_ends;
	const uint32_t *ring_outers; /* [ring_count] of them, or NULL */
	uint32_t ring_count;
} APMATH_POLY_SET;

/* ---- Checking -------------------------------------------------------- */

/* What makes a set something other than polygons with holes. */
typedef enum APMATH_POLY_DEFECT {
	APMATH_POLY_DEFECT_NONE = 0,
	/* [ring] has fewer than three points. */
	APMATH_POLY_DEFECT_TOO_FEW_POINTS,
	/* Edge [edge] of [ring] has no length: its two points are the same. */
	APMATH_POLY_DEFECT_ZERO_LENGTH_EDGE,
	/* Every point of [ring] is on one line: it encloses nothing. */
	APMATH_POLY_DEFECT_NO_AREA,
	/* Edge [edge] of [ring] touches or crosses edge [other_edge] of
	 * [other_ring] (which may be the same ring), other than where two edges
	 * that follow each other share their point - so a ring that crosses or
	 * touches itself, a spike that doubles back along itself, and two
	 * rings that touch are all this. */
	APMATH_POLY_DEFECT_EDGES_MEET,
	/* Outer ring [ring] runs clockwise, or hole [ring] counter-clockwise. */
	APMATH_POLY_DEFECT_WRONG_WINDING,
	/* Hole [ring] isn't inside its outer ring, [other_ring]. */
	APMATH_POLY_DEFECT_HOLE_OUTSIDE,
	/* [ring] is inside [other_ring]'s polygon - two polygons overlap, or a
	 * hole is inside another hole of the same polygon. */
	APMATH_POLY_DEFECT_OVERLAP,
} APMATH_POLY_DEFECT;

/* Frozen. Fields a defect doesn't name are 0. */
typedef struct APMATH_POLY_DEFECT_INFO {
	APMATH_POLY_DEFECT defect;
	uint32_t ring;
	uint32_t edge; /* the edge's position in its ring */
	uint32_t other_ring;
	uint32_t other_edge;
} APMATH_POLY_DEFECT_INFO;

/* Whether [set] is polygons with holes: every ring simple (it doesn't
 * touch or cross itself) and enclosing something, outer rings
 * counter-clockwise and holes clockwise, no two rings touching or
 * crossing, each hole inside its own outer ring, and no polygon's inside
 * overlapping another's. Collinear points along an edge are allowed.
 *
 * Returns APRESULT_OK whether or not it is, and sets [out_defect] to the
 * first defect found - checked in the order of the enum, rings and edges
 * in order - or to NONE. The other results are for a [set] that can't be
 * read (see "Results"). Costs the square of the edge count. */
APRESULT apmath_poly_validate(const APMATH_POLY_SET *set, APMATH_POLY_DEFECT_INFO *out_defect, APMATH_ERROR *out_error);

/* ---- Booleans -------------------------------------------------------- */

typedef enum APMATH_POLY_OP {
	APMATH_POLY_OP_UNION = 0,        /* inside either */
	APMATH_POLY_OP_INTERSECTION = 1, /* inside both */
	APMATH_POLY_OP_DIFFERENCE = 2,   /* inside the subject, outside the clip */
	APMATH_POLY_OP_XOR = 3,          /* inside exactly one */
} APMATH_POLY_OP;

/* Which points a set's rings enclose, by the winding number - how many
 * times the rings go round the point counter-clockwise, less clockwise. */
typedef enum APMATH_POLY_FILL {
	APMATH_POLY_FILL_EVEN_ODD = 0, /* odd */
	APMATH_POLY_FILL_NON_ZERO = 1, /* not zero */
	APMATH_POLY_FILL_POSITIVE = 2, /* above zero */
} APMATH_POLY_FILL;

typedef struct APMATH_POLY_BOOLEAN_DESC {
	uint32_t struct_size;
	APMATH_POLY_OP op;
	/* Applies to both sets. */
	APMATH_POLY_FILL fill;
	APMATH_POLY_SET subject;
	/* May be empty: a union with an empty clip is the subject cleaned up -
	 * overlaps merged, self-crossings resolved, rings oriented. */
	APMATH_POLY_SET clip;
} APMATH_POLY_BOOLEAN_DESC;

typedef struct apmath_poly_t *apmath_poly;

/* Builds the boolean of [desc]'s two sets. The result is polygons with
 * holes that apmath_poly_validate() finds nothing wrong with, except where
 * rounding an intersection onto the grid has left two rings touching:
 *   - outer rings counter-clockwise, holes clockwise, each hole listed
 *     straight after its outer ring (or after an earlier hole of the same
 *     ring); a polygon standing in another's hole comes after that
 *     other's holes
 *   - no edge of no length, and no point in the middle of a straight run
 *   - each ring starting at its lowest point, the leftmost of those
 *   - each edge tagged with the tag of the input edge it lies along: one
 *     it overlaps, with both its ends on that edge's line to within a unit
 *     (rounding an intersection moves a point up to half a unit in each
 *     axis). Where it lies along several - where collinear input edges
 *     merged - the one whose line is nearest, then the first: the
 *     subject's before the clip's, each in ring and edge order
 * An empty result is valid: a set with no rings.
 *
 * The same desc gives the same result, point for point, on every platform.
 * Costs about (n + k) log n for n edges in and k crossings, plus the
 * product of the edge counts in and out for the tags. [out_poly] is NULL
 * on failure. */
APRESULT apmath_poly_boolean(const APMATH_POLY_BOOLEAN_DESC *desc, apmath_poly *out_poly, APMATH_ERROR *out_error);
/* NULL is fine. */
void apmath_poly_destroy(apmath_poly poly);

/* [poly]'s polygons, reading its own arrays: valid until it's destroyed.
 * All zeros for NULL. */
APMATH_POLY_SET apmath_poly_get_set(apmath_poly poly);

/* ---- Offsetting ------------------------------------------------------ */

typedef struct APMATH_POLY_OFFSET_DESC {
	uint32_t struct_size;
	/* Polygons with holes (apmath_poly_validate()); its ring_outers isn't
	 * read - what each ring encloses follows from its winding. */
	APMATH_POLY_SET set;
	/* One per point: how far the edge starting there moves, in grid units,
	 * square to itself. Positive moves it away from what the polygon
	 * encloses - out from an outer ring, into a hole - and negative in;
	 * 0 leaves it where it is. Finite. */
	const double *distances;
	/* Where two moved edges meet at a corner, they're extended to meet
	 * (mitred) unless the meeting point would be further from the corner
	 * than this many times the larger of the two distances; then the
	 * corner is cut across from one moved edge's end to the other's
	 * (bevelled). At least 1. A right angle moved by d on both sides
	 * mitres within sqrt(2). */
	double miter_limit;
} APMATH_POLY_OFFSET_DESC;

/* Moves each edge of [desc]'s set by its own distance and re-meets the
 * corners: mitred, bevelled past the mitre limit, and stepped where two
 * edges that run on in one line move by different distances (a short edge
 * square to them joins the two). What the moved rings enclose is then
 * taken as by a union with no clip under the POSITIVE rule, so a ring
 * folded over on itself by a large inward move loses the fold, and two
 * polygons grown into each other merge.
 *
 * The result is a boolean's (apmath_poly_boolean()): normalised the same
 * way, and each edge tagged with the tag of the input edge it was moved
 * from. A bevel, or a step, takes the tag of the edge after it. The same
 * desc gives the same result on every platform: moved corners are
 * computed in doubles and rounded to the nearest grid point.
 *
 * APRESULT_INVALID_ARGUMENT also for a moved corner past the coordinate
 * limit. [out_poly] is NULL on failure. */
APRESULT apmath_poly_offset_edges(const APMATH_POLY_OFFSET_DESC *desc, apmath_poly *out_poly, APMATH_ERROR *out_error);

/* ---- Straight skeleton ----------------------------------------------- */

typedef struct APMATH_POLY_SKELETON_DESC {
	uint32_t struct_size;
	/* One ring, simple and counter-clockwise (apmath_poly_validate()); no
	 * holes (APRESULT_UNSUPPORTED). */
	APMATH_POLY_SET set;
	/* One per point: how fast the edge starting there moves inward, square
	 * to itself, in grid units per unit of time. At least 0, finite, and
	 * not all 0. An edge with weight 0 stays where it is, and the edges
	 * beside it slide along it. Two edges that follow each other on one
	 * line - parallel by the rule below - must have the same weight. */
	const double *weights;
} APMATH_POLY_SKELETON_DESC;

/* The weighted straight skeleton of [desc]'s polygon: every edge moves
 * inward at its weight, corners follow where their two edges meet, and
 * where the moving edges collide the polygon shrinks, splits and closes up.
 * The region an edge sweeps before it's gone is its face.
 *
 * [out_poly] holds one ring per input edge, in input order, each that
 * edge's face: counter-clockwise, starting with the edge itself (its point
 * 0 and 1 are the edge's start and end), then back round its boundary. The
 * first edge of a face keeps the input edge's tag; every other edge of it
 * is tagged with the tag of the input edge whose face is across it - so a
 * caller knows which two faces meet along it. ring_outers is NO_RING for
 * every ring, and the rings aren't a set apmath_poly_validate() accepts:
 * faces share edges, and a face of an edge with weight 0 has no area (it
 * runs along the edge and back).
 *
 * Time t is only the parameter the edges move by: a point's time is when
 * the first edge reached it, so with every weight 1 it's the point's
 * distance from the boundary measured along the skeleton. A caller that
 * reads time as height gets a roof: each face then lies in a plane through
 * its edge, rising at 1 / weight.
 *
 * Corners are found in doubles and rounded to the grid; two edges that
 * follow each other count as parallel when the cross product of their unit
 * directions is less than 1e-12 in magnitude. The same desc gives the same
 * result on every platform. Costs about the cube of the edge count.
 *
 * APRESULT_INVALID_ARGUMENT also for a set that isn't one simple
 * counter-clockwise ring, and for a polygon whose skeleton can't be traced
 * (a degenerate case the message names). [out_poly] is NULL on failure. */
APRESULT apmath_poly_skeleton(const APMATH_POLY_SKELETON_DESC *desc, apmath_poly *out_poly, APMATH_ERROR *out_error);

/* ---- Triangulation --------------------------------------------------- */

/* The most indices apmath_poly_triangulate() can write for [set]: three for
 * each of a polygon's points, less two for it, plus two for each of its
 * holes. 0 for NULL. */
uint32_t apmath_poly_get_triangulation_capacity(const APMATH_POLY_SET *set);

/* Triangulates each of [set]'s polygons - an outer ring with its holes, as
 * [set]'s ring_outers groups them - writing three indices into [set]'s
 * points per triangle into [out_indices], and how many it wrote into
 * [out_index_count]. Triangles run counter-clockwise; none has no area, so
 * collinear points can leave fewer than the capacity.
 *
 * [set] must be polygons with holes (apmath_poly_validate()). Given one
 * that isn't, it still writes only indices of [set]'s points, but which
 * triangles is unspecified. The same [set] gives the same triangles on
 * every platform. APRESULT_INVALID_ARGUMENT if [index_capacity] is below
 * apmath_poly_get_triangulation_capacity(). */
APRESULT apmath_poly_triangulate(const APMATH_POLY_SET *set, uint32_t *out_indices, uint32_t index_capacity,
                                 uint32_t *out_index_count, APMATH_ERROR *out_error);

#if __cplusplus
}
#endif

#endif // APMATHPOLY_H
