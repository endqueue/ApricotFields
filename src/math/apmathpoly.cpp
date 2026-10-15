#include "math/apmathpoly.h"
#include "apmathpoly_internal.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "clipper2/clipper.engine.h"
#include "mapbox/earcut.hpp"

// Booleans are Clipper2's, on its 64-bit integer paths; triangulation is
// earcut's. Everything around them - checks, orientation, where rings start,
// tags - is exact integer arithmetic here, through the 128-bit products in
// "Exact predicates", so it can't disagree between platforms.
//
// Both libraries compute some things in doubles (Clipper2 an intersection
// point before rounding it, earcut its z-order hash). Doubles give the same
// bits everywhere only if no compiler fuses a multiply and an add, so this
// file and Clipper2 are built with contraction off (CMakeLists.txt). Don't
// build either with fast-math.

namespace
{

// ---- Errors ---------------------------------------------------------------

APRESULT fail(APMATH_ERROR *error, APRESULT result, const char *format, ...)
{
    if (error)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return result;
}

void clear(APMATH_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// Copies a caller's versioned input struct into [out] at the current version:
// the fields its struct_size covers, zeros past them. False if it's smaller
// than the struct's first version.
template <typename T> bool read_versioned(const T *in, size_t v1_size, T &out)
{
    if (!in || in->struct_size < v1_size)
        return false;
    memset(&out, 0, sizeof(T));
    memcpy(&out, in, in->struct_size < sizeof(T) ? in->struct_size : sizeof(T));
    out.struct_size = (uint32_t)sizeof(T);
    return true;
}

const size_t APMATH_POLY_BOOLEAN_DESC_V1_SIZE = sizeof(APMATH_POLY_BOOLEAN_DESC);

// ---- Exact predicates -----------------------------------------------------
//
// Coordinates are within 2^40, so a difference of two is within 2^41 and a
// product of two differences within 2^82: past int64, inside 128 bits. These
// are written out rather than using __int128, which MSVC doesn't have.

// A signed 128-bit value: hi * 2^64 + lo.
struct wide_t
{
    int64_t hi;
    uint64_t lo;
};

wide_t multiply(int64_t a, int64_t b)
{
    bool negative = (a < 0) != (b < 0);
    uint64_t ua = a < 0 ? 0 - (uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? 0 - (uint64_t)b : (uint64_t)b;

    uint64_t a_lo = ua & 0xFFFFFFFFu, a_hi = ua >> 32;
    uint64_t b_lo = ub & 0xFFFFFFFFu, b_hi = ub >> 32;
    uint64_t lo_lo = a_lo * b_lo;
    uint64_t lo_hi = a_lo * b_hi;
    uint64_t hi_lo = a_hi * b_lo;
    uint64_t hi_hi = a_hi * b_hi;
    // At most (2^32 - 1)^2 + 2 (2^32 - 1): no carry out of 64 bits.
    uint64_t middle = (lo_lo >> 32) + (lo_hi & 0xFFFFFFFFu) + hi_lo;
    uint64_t lo = (middle << 32) | (lo_lo & 0xFFFFFFFFu);
    uint64_t hi = hi_hi + (lo_hi >> 32) + (middle >> 32);

    if (negative)
    {
        lo = ~lo + 1;
        hi = ~hi + (lo == 0 ? 1 : 0);
    }
    return {(int64_t)hi, lo};
}

int compare(const wide_t &a, const wide_t &b)
{
    if (a.hi != b.hi)
        return a.hi < b.hi ? -1 : 1;
    if (a.lo != b.lo)
        return a.lo < b.lo ? -1 : 1;
    return 0;
}

// The sign of (b - a) x (c - a): 1 if a, b, c turn left (counter-clockwise),
// -1 if they turn right, 0 if they're on one line.
int orient(const APMATH_POLY_POINT &a, const APMATH_POLY_POINT &b, const APMATH_POLY_POINT &c)
{
    return compare(multiply(b.x - a.x, c.y - a.y), multiply(b.y - a.y, c.x - a.x));
}

// The sign of (b - a) . (d - c).
int dot_sign(const APMATH_POLY_POINT &a, const APMATH_POLY_POINT &b, const APMATH_POLY_POINT &c,
             const APMATH_POLY_POINT &d)
{
    // x x' + y y' against 0 is x x' against -y y'.
    return compare(multiply(b.x - a.x, d.x - c.x), multiply(-(b.y - a.y), d.y - c.y));
}

bool same_point(const APMATH_POLY_POINT &a, const APMATH_POLY_POINT &b)
{
    return a.x == b.x && a.y == b.y;
}

// Whether [p], known to be on the line through [a] and [b], is between them,
// ends included.
bool within(const APMATH_POLY_POINT &a, const APMATH_POLY_POINT &b, const APMATH_POLY_POINT &p)
{
    return (a.x <= b.x ? a.x <= p.x && p.x <= b.x : b.x <= p.x && p.x <= a.x) &&
           (a.y <= b.y ? a.y <= p.y && p.y <= b.y : b.y <= p.y && p.y <= a.y);
}

// Whether segments a-b and c-d share any point, ends included.
bool segments_meet(const APMATH_POLY_POINT &a, const APMATH_POLY_POINT &b, const APMATH_POLY_POINT &c,
                   const APMATH_POLY_POINT &d)
{
    int abc = orient(a, b, c);
    int abd = orient(a, b, d);
    int cda = orient(c, d, a);
    int cdb = orient(c, d, b);
    if (abc * abd < 0 && cda * cdb < 0)
        return true;
    if (abc == 0 && within(a, b, c))
        return true;
    if (abd == 0 && within(a, b, d))
        return true;
    if (cda == 0 && within(c, d, a))
        return true;
    if (cdb == 0 && within(c, d, b))
        return true;
    return false;
}

// Whether [p] is inside [ring] by the even-odd rule. [p] must not be on the
// ring's boundary.
bool inside_ring(const APMATH_POLY_POINT *ring, uint32_t count, const APMATH_POLY_POINT &p)
{
    bool inside = false;
    for (uint32_t i = 0, j = count - 1; i < count; j = i++)
    {
        const APMATH_POLY_POINT &a = ring[j];
        const APMATH_POLY_POINT &b = ring[i];
        if ((a.y > p.y) != (b.y > p.y))
        {
            // The ray from [p] towards +x crosses an upward edge with [p] on
            // its left, or a downward one with [p] on its right.
            int side = orient(a, b, p);
            if (b.y > a.y ? side > 0 : side < 0)
                inside = !inside;
        }
    }
    return inside;
}

// The position in [ring] of its lowest point, the leftmost of those.
uint32_t lowest_point(const APMATH_POLY_POINT *ring, uint32_t count)
{
    uint32_t lowest = 0;
    for (uint32_t i = 1; i < count; i++)
    {
        if (ring[i].y < ring[lowest].y || (ring[i].y == ring[lowest].y && ring[i].x < ring[lowest].x))
            lowest = i;
    }
    return lowest;
}

// 1 if [ring] runs counter-clockwise, -1 if clockwise: the turn at its lowest
// point, where a simple ring always turns its own way. 0 for a ring whose
// points are all on one line.
int winding(const APMATH_POLY_POINT *ring, uint32_t count)
{
    if (count < 3)
        return 0;
    uint32_t i = lowest_point(ring, count);
    return orient(ring[(i + count - 1) % count], ring[i], ring[(i + 1) % count]);
}

// ---- Reading sets -----------------------------------------------------------

uint32_t ring_begin(const APMATH_POLY_SET &set, uint32_t ring)
{
    return ring ? set.ring_ends[ring - 1] : 0;
}

uint32_t ring_size(const APMATH_POLY_SET &set, uint32_t ring)
{
    return set.ring_ends[ring] - ring_begin(set, ring);
}

bool is_outer(const APMATH_POLY_SET &set, uint32_t ring)
{
    return !set.ring_outers || set.ring_outers[ring] == APMATH_POLY_NO_RING;
}

// Checks that [set] can be read: arrays where its counts need them, ring ends
// in order and ending at the point count, coordinates within the limit - and,
// with [check_outers], ring outers naming outer rings. [what] names it in
// messages.
APRESULT check_set(const APMATH_POLY_SET &set, const char *what, bool check_outers, APMATH_ERROR *error)
{
    if (set.point_count > 0 && !set.points)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s has %u points but no point array", what, set.point_count);
    if (set.ring_count > 0 && !set.ring_ends)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s has %u rings but no ring ends", what, set.ring_count);
    if (set.ring_count == 0 && set.point_count > 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s has %u points but no rings", what, set.point_count);
    for (uint32_t i = 1; i < set.ring_count; i++)
    {
        if (set.ring_ends[i] < set.ring_ends[i - 1])
            return fail(error, APRESULT_INVALID_ARGUMENT, "%s's ring %u ends before ring %u does", what, i, i - 1);
    }
    if (set.ring_count > 0 && set.ring_ends[set.ring_count - 1] != set.point_count)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%s's last ring ends at point %u, not at its point count, %u",
                    what, set.ring_ends[set.ring_count - 1], set.point_count);
    for (uint32_t i = 0; i < set.point_count; i++)
    {
        const APMATH_POLY_POINT &p = set.points[i];
        if (p.x > APMATH_POLY_COORD_LIMIT)
            return fail(error, APRESULT_INVALID_ARGUMENT, "%s's point %u has x above the coordinate limit", what, i);
        if (p.x < -APMATH_POLY_COORD_LIMIT)
            return fail(error, APRESULT_INVALID_ARGUMENT, "%s's point %u has x below the coordinate limit", what, i);
        if (p.y > APMATH_POLY_COORD_LIMIT)
            return fail(error, APRESULT_INVALID_ARGUMENT, "%s's point %u has y above the coordinate limit", what, i);
        if (p.y < -APMATH_POLY_COORD_LIMIT)
            return fail(error, APRESULT_INVALID_ARGUMENT, "%s's point %u has y below the coordinate limit", what, i);
    }
    if (check_outers && set.ring_outers)
    {
        for (uint32_t i = 0; i < set.ring_count; i++)
        {
            uint32_t outer = set.ring_outers[i];
            if (outer == APMATH_POLY_NO_RING)
                continue;
            if (outer >= set.ring_count)
                return fail(error, APRESULT_INVALID_ARGUMENT, "%s's ring %u names ring %u as its outer ring, past the last",
                            what, i, outer);
            if (set.ring_outers[outer] != APMATH_POLY_NO_RING)
                return fail(error, APRESULT_INVALID_ARGUMENT, "%s's ring %u names ring %u as its outer ring, a hole",
                            what, i, outer);
        }
    }
    return APRESULT_OK;
}

// ---- Validation ---------------------------------------------------------------

struct edge_t
{
    uint32_t ring, edge; // the edge's position in its ring
    uint32_t a, b;       // its points' indices in the set
};

APMATH_POLY_DEFECT_INFO defect(APMATH_POLY_DEFECT kind, uint32_t ring, uint32_t edge = 0, uint32_t other_ring = 0,
                               uint32_t other_edge = 0)
{
    return {kind, ring, edge, other_ring, other_edge};
}

// Whether edges [e] and [f], which follow each other in one ring - [f]
// starting where [e] ends - meet anywhere but there: they double back along
// one line.
bool folds_back(const APMATH_POLY_SET &set, const edge_t &e, const edge_t &f)
{
    const APMATH_POLY_POINT &p = set.points[e.a];
    const APMATH_POLY_POINT &q = set.points[e.b];
    const APMATH_POLY_POINT &r = set.points[f.b];
    return orient(p, q, r) == 0 && dot_sign(p, q, q, r) < 0;
}

// The first defect of [set], which check_set() has passed. Throws
// std::bad_alloc.
APMATH_POLY_DEFECT_INFO find_defect(const APMATH_POLY_SET &set)
{
    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        if (ring_size(set, ring) < 3)
            return defect(APMATH_POLY_DEFECT_TOO_FEW_POINTS, ring);
    }

    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        uint32_t begin = ring_begin(set, ring), count = ring_size(set, ring);
        for (uint32_t i = 0; i < count; i++)
        {
            if (same_point(set.points[begin + i], set.points[begin + (i + 1) % count]))
                return defect(APMATH_POLY_DEFECT_ZERO_LENGTH_EDGE, ring, i);
        }
    }

    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        uint32_t begin = ring_begin(set, ring), count = ring_size(set, ring);
        const APMATH_POLY_POINT *points = set.points + begin;
        bool flat = true;
        for (uint32_t i = 2; i < count && flat; i++)
            flat = orient(points[0], points[1], points[i]) == 0;
        if (flat)
            return defect(APMATH_POLY_DEFECT_NO_AREA, ring);
    }

    std::vector<edge_t> edges;
    edges.reserve(set.point_count);
    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        uint32_t begin = ring_begin(set, ring), count = ring_size(set, ring);
        for (uint32_t i = 0; i < count; i++)
            edges.push_back({ring, i, begin + i, begin + (i + 1) % count});
    }
    for (size_t i = 0; i < edges.size(); i++)
    {
        const edge_t &e = edges[i];
        for (size_t j = i + 1; j < edges.size(); j++)
        {
            const edge_t &f = edges[j];
            bool meet;
            if (f.ring == e.ring && f.a == e.b)
                meet = folds_back(set, e, f);
            else if (f.ring == e.ring && f.b == e.a)
                meet = folds_back(set, f, e);
            else
                meet = segments_meet(set.points[e.a], set.points[e.b], set.points[f.a], set.points[f.b]);
            if (meet)
                return defect(APMATH_POLY_DEFECT_EDGES_MEET, e.ring, e.edge, f.ring, f.edge);
        }
    }

    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        int turn = winding(set.points + ring_begin(set, ring), ring_size(set, ring));
        if (turn != (is_outer(set, ring) ? 1 : -1))
            return defect(APMATH_POLY_DEFECT_WRONG_WINDING, ring);
    }

    // From here no two edges meet, so a ring's first point is inside another
    // ring exactly when the whole ring is.
    auto contains = [&](uint32_t ring, uint32_t other) {
        return inside_ring(set.points + ring_begin(set, other), ring_size(set, other),
                           set.points[ring_begin(set, ring)]);
    };

    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        if (is_outer(set, ring))
            continue;
        uint32_t outer = set.ring_outers[ring];
        if (!contains(ring, outer))
            return defect(APMATH_POLY_DEFECT_HOLE_OUTSIDE, ring, 0, outer);
    }

    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        for (uint32_t other = 0; other < set.ring_count; other++)
        {
            if (other == ring)
                continue;
            if (is_outer(set, ring) && is_outer(set, other))
            {
                // Inside the other polygon unless inside one of its holes.
                if (!contains(ring, other))
                    continue;
                bool in_hole = false;
                for (uint32_t hole = 0; hole < set.ring_count && !in_hole; hole++)
                    in_hole = !is_outer(set, hole) && set.ring_outers[hole] == other && contains(ring, hole);
                if (!in_hole)
                    return defect(APMATH_POLY_DEFECT_OVERLAP, ring, 0, other);
            }
            else if (!is_outer(set, ring) && !is_outer(set, other) && set.ring_outers[ring] == set.ring_outers[other])
            {
                if (contains(ring, other))
                    return defect(APMATH_POLY_DEFECT_OVERLAP, ring, 0, other);
            }
        }
    }

    return defect(APMATH_POLY_DEFECT_NONE, 0);
}

// ---- Booleans -------------------------------------------------------------------

// An input edge, for tagging the result's edges.
struct tagged_edge_t
{
    APMATH_POLY_POINT a, b;
    uint64_t tag;
};

// Appends [set]'s edges to [out], in ring and edge order.
void add_tagged_edges(const APMATH_POLY_SET &set, std::vector<tagged_edge_t> &out)
{
    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        uint32_t begin = ring_begin(set, ring), count = ring_size(set, ring);
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t a = begin + i, b = begin + (i + 1) % count;
            out.push_back({set.points[a], set.points[b], set.edge_tags ? set.edge_tags[a] : 0});
        }
    }
}

// How a result edge p-q sits against an input edge. In doubles: a coordinate
// difference is exact in one, and these are only compared with each other
// and with the unit tolerance, computed the same way everywhere.
struct fit_t
{
    double line_distance2;    // the farther of p and q from the input edge's line, squared
    double segment_distance2; // the farther of p and q from the input edge itself, squared
    bool overlaps;            // p-q and the input edge share a stretch along that line
};

fit_t fit(const APMATH_POLY_POINT &p, const APMATH_POLY_POINT &q, const tagged_edge_t &e)
{
    fit_t out{0.0, 0.0, false};
    double dx = (double)(e.b.x - e.a.x), dy = (double)(e.b.y - e.a.y);
    double length2 = dx * dx + dy * dy;
    double t[2];
    const APMATH_POLY_POINT *ends[2] = {&p, &q};
    for (int k = 0; k < 2; k++)
    {
        double px = (double)(ends[k]->x - e.a.x), py = (double)(ends[k]->y - e.a.y);
        if (!(length2 > 0.0))
        {
            // An input edge with no length is a point: no line to be on.
            double d2 = px * px + py * py;
            out.line_distance2 = d2 > out.line_distance2 ? d2 : out.line_distance2;
            out.segment_distance2 = out.line_distance2;
            t[k] = 0.0;
            continue;
        }
        double cross = dx * py - dy * px;
        double line2 = cross * cross / length2;
        t[k] = (px * dx + py * dy) / length2;
        double clamped = t[k] < 0.0 ? 0.0 : (t[k] > 1.0 ? 1.0 : t[k]);
        double ex = px - clamped * dx, ey = py - clamped * dy;
        double segment2 = ex * ex + ey * ey;
        if (line2 > out.line_distance2)
            out.line_distance2 = line2;
        if (segment2 > out.segment_distance2)
            out.segment_distance2 = segment2;
    }
    if (length2 > 0.0)
    {
        double lo = t[0] < t[1] ? t[0] : t[1];
        double hi = t[0] < t[1] ? t[1] : t[0];
        out.overlaps = (hi < 1.0 ? hi : 1.0) > (lo > 0.0 ? lo : 0.0);
    }
    return out;
}

// How far off its input edge's line rounding can move a result edge's end: an
// intersection is rounded to the nearest grid point, at most half a unit out
// in each axis.
const double ON_LINE_DISTANCE2 = 1.0;

// The tag of the input edge [p]-[q] lies along: one it overlaps with both
// ends on its line, give or take rounding; of those, the one whose line is
// nearest, then the first. Rounding can't leave an edge along none, but if
// one were, it takes the input edge its farther end is nearest, then the
// first - so every edge gets a tag, and the same one everywhere.
uint64_t find_tag(const APMATH_POLY_POINT &p, const APMATH_POLY_POINT &q, const std::vector<tagged_edge_t> &edges)
{
    bool found = false;
    uint64_t tag = 0;
    double best = 0.0;
    uint64_t fallback_tag = 0;
    double fallback_best = 0.0;
    for (size_t i = 0; i < edges.size(); i++)
    {
        fit_t f = fit(p, q, edges[i]);
        if (f.overlaps && f.line_distance2 <= ON_LINE_DISTANCE2 && (!found || f.line_distance2 < best))
        {
            found = true;
            best = f.line_distance2;
            tag = edges[i].tag;
        }
        if (i == 0 || f.segment_distance2 < fallback_best)
        {
            fallback_best = f.segment_distance2;
            fallback_tag = edges[i].tag;
        }
    }
    return found ? tag : fallback_tag;
}

Clipper2Lib::Paths64 to_paths(const APMATH_POLY_SET &set)
{
    Clipper2Lib::Paths64 paths(set.ring_count);
    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        uint32_t begin = ring_begin(set, ring), count = ring_size(set, ring);
        paths[ring].reserve(count);
        for (uint32_t i = 0; i < count; i++)
            paths[ring].emplace_back(set.points[begin + i].x, set.points[begin + i].y);
    }
    return paths;
}

// Appends [path] to [poly] as a ring: an outer ring with [outer] NO_RING,
// else a hole in [outer]. Turned to run counter-clockwise for an outer ring
// and clockwise for a hole, and to start at its lowest point. Returns the
// ring's index, or NO_RING for a path with no area, which isn't added.
uint32_t add_ring(apmath_poly_t &poly, const Clipper2Lib::Path64 &path, uint32_t outer)
{
    std::vector<APMATH_POLY_POINT> ring;
    ring.reserve(path.size());
    for (const Clipper2Lib::Point64 &p : path)
        ring.push_back({p.x, p.y});
    int turn = winding(ring.data(), (uint32_t)ring.size());
    if (turn == 0)
        return APMATH_POLY_NO_RING;
    if (turn != (outer == APMATH_POLY_NO_RING ? 1 : -1))
        std::reverse(ring.begin(), ring.end());
    uint32_t start = lowest_point(ring.data(), (uint32_t)ring.size());
    std::rotate(ring.begin(), ring.begin() + start, ring.end());

    poly.points.insert(poly.points.end(), ring.begin(), ring.end());
    poly.ring_ends.push_back((uint32_t)poly.points.size());
    poly.ring_outers.push_back(outer);
    return (uint32_t)poly.ring_ends.size() - 1;
}

// Fills [poly] from [tree]: each outer ring followed by its holes, and the
// polygons standing in a hole after everything before them.
void flatten(apmath_poly_t &poly, const Clipper2Lib::PolyTree64 &tree)
{
    std::vector<const Clipper2Lib::PolyPath64 *> outers;
    for (size_t i = 0; i < tree.Count(); i++)
        outers.push_back(tree.Child(i));
    for (size_t k = 0; k < outers.size(); k++)
    {
        const Clipper2Lib::PolyPath64 *outer = outers[k];
        uint32_t outer_ring = add_ring(poly, outer->Polygon(), APMATH_POLY_NO_RING);
        for (size_t h = 0; h < outer->Count(); h++)
        {
            const Clipper2Lib::PolyPath64 *hole = outer->Child(h);
            if (outer_ring != APMATH_POLY_NO_RING)
                add_ring(poly, hole->Polygon(), outer_ring);
            for (size_t i = 0; i < hole->Count(); i++)
                outers.push_back(hole->Child(i));
        }
    }
}

// The boolean of [subject] and [clip], both read already (check_set()), into
// a new result: Clipper2's answer, flattened and normalised (flatten()), each
// edge tagged with the input edge it lies along (find_tag()).
APRESULT compute_boolean(const APMATH_POLY_SET &subject, const APMATH_POLY_SET &clip, Clipper2Lib::ClipType clip_type,
                         Clipper2Lib::FillRule fill_rule, apmath_poly *out_poly, APMATH_ERROR *out_error)
{
    apmath_poly_t *poly = (apmath_poly_t *)malloc(sizeof(apmath_poly_t));
    if (!poly)
        return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory for the result");
    poly = new (poly) apmath_poly_t();

    try
    {
        Clipper2Lib::Clipper64 clipper;
        clipper.PreserveCollinear(false);
        clipper.AddSubject(to_paths(subject));
        if (clip.ring_count > 0)
            clipper.AddClip(to_paths(clip));
        Clipper2Lib::PolyTree64 tree;
        if (!clipper.Execute(clip_type, fill_rule, tree))
        {
            poly->~apmath_poly_t();
            free(poly);
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "the boolean couldn't be computed from these sets");
        }
        flatten(*poly, tree);

        std::vector<tagged_edge_t> edges;
        edges.reserve((size_t)subject.point_count + clip.point_count);
        add_tagged_edges(subject, edges);
        add_tagged_edges(clip, edges);
        poly->edge_tags.resize(poly->points.size());
        APMATH_POLY_SET out = apmath_poly_get_set(poly);
        for (uint32_t ring = 0; ring < out.ring_count; ring++)
        {
            uint32_t begin = ring_begin(out, ring), count = ring_size(out, ring);
            for (uint32_t i = 0; i < count; i++)
                poly->edge_tags[begin + i] =
                    find_tag(poly->points[begin + i], poly->points[begin + (i + 1) % count], edges);
        }
    }
    catch (const std::bad_alloc &)
    {
        poly->~apmath_poly_t();
        free(poly);
        return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory computing the boolean");
    }
    catch (...)
    {
        // Clipper2 throws on input it can't take; the checks above should
        // leave none.
        poly->~apmath_poly_t();
        free(poly);
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "the boolean couldn't be computed from these sets");
    }

    *out_poly = poly;
    return APRESULT_OK;
}

// ---- Offsetting -------------------------------------------------------------------

const size_t APMATH_POLY_OFFSET_DESC_V1_SIZE = sizeof(APMATH_POLY_OFFSET_DESC);

// [set]'s rings with each edge moved by its distance and the corners re-met
// (apmath_poly_offset_edges()), into [out_points], [out_tags] and
// [out_ring_ends]: rings that may cross themselves or each other, for a
// union to resolve. Every point emitted at a corner starts an edge along the
// moved edge after the corner, or a bevel or step leading to it, so it takes
// that edge's tag. False, with [error] set, if a corner lands past the
// coordinate limit. Throws std::bad_alloc.
bool move_edges(const APMATH_POLY_SET &set, const double *distances, double miter_limit,
                std::vector<APMATH_POLY_POINT> &out_points, std::vector<uint64_t> &out_tags,
                std::vector<uint32_t> &out_ring_ends, APMATH_ERROR *error)
{
    const double limit = (double)APMATH_POLY_COORD_LIMIT;
    std::vector<uint32_t> at;
    for (uint32_t ring = 0; ring < set.ring_count; ring++)
    {
        // The ring without repeated points. Of a run of equal points the
        // last is kept: the edge that leaves the run starts there.
        uint32_t begin = ring_begin(set, ring), count = ring_size(set, ring);
        at.clear();
        for (uint32_t i = begin; i < begin + count; i++)
        {
            if (!at.empty() && same_point(set.points[i], set.points[at.back()]))
                at.back() = i;
            else
                at.push_back(i);
        }
        // Round the end: the front comes after the back.
        if (at.size() > 1 && same_point(set.points[at.front()], set.points[at.back()]))
            at.pop_back();
        size_t n = at.size();
        if (n < 3)
            continue;

        auto emit = [&](const APMATH_POLY_POINT &corner, double dx, double dy, uint64_t tag) {
            double x = (double)corner.x + dx;
            double y = (double)corner.y + dy;
            if (!(std::fabs(x) <= limit) || !(std::fabs(y) <= limit))
                return false;
            out_points.push_back({(int64_t)std::llround(x), (int64_t)std::llround(y)});
            out_tags.push_back(tag);
            return true;
        };

        for (size_t k = 0; k < n; k++)
        {
            uint32_t in = at[(k + n - 1) % n], out = at[k], next = at[(k + 1) % n];
            const APMATH_POLY_POINT &prev_point = set.points[in];
            const APMATH_POLY_POINT &corner = set.points[out];
            const APMATH_POLY_POINT &next_point = set.points[next];
            uint64_t tag = set.edge_tags ? set.edge_tags[out] : 0;

            // Unit directions of the edge into the corner and out of it, and
            // each one's move: along its right-hand normal, which points away
            // from what the ring encloses on either winding.
            double ux = (double)(corner.x - prev_point.x), uy = (double)(corner.y - prev_point.y);
            double vx = (double)(next_point.x - corner.x), vy = (double)(next_point.y - corner.y);
            double u_length = std::sqrt(ux * ux + uy * uy), v_length = std::sqrt(vx * vx + vy * vy);
            ux /= u_length;
            uy /= u_length;
            vx /= v_length;
            vy /= v_length;
            double d1 = distances[in], d2 = distances[out];
            double q1x = uy * d1, q1y = -ux * d1;
            double q2x = vy * d2, q2y = -vx * d2;

            double cross = ux * vy - uy * vx;
            double dot = ux * vx + uy * vy;
            bool one_point = false;
            double x = 0.0, y = 0.0;
            if (cross != 0.0)
            {
                // Where the two moved lines meet, unless that's past the
                // mitre limit.
                double s = ((q2x - q1x) * vy - (q2y - q1y) * vx) / cross;
                x = q1x + ux * s;
                y = q1y + uy * s;
                double reach = std::fabs(d1) > std::fabs(d2) ? std::fabs(d1) : std::fabs(d2);
                one_point = std::sqrt(x * x + y * y) <= miter_limit * reach;
            }
            else if (dot > 0.0 && d1 == d2)
            {
                // Running on in one line, moved as one.
                x = q1x;
                y = q1y;
                one_point = true;
            }

            bool ok = one_point ? emit(corner, x, y, tag)
                                : emit(corner, q1x, q1y, tag) && emit(corner, q2x, q2y, tag);
            if (!ok)
            {
                fail(error, APRESULT_INVALID_ARGUMENT, "a moved corner of ring %u lands past the coordinate limit", ring);
                return false;
            }
        }
        out_ring_ends.push_back((uint32_t)out_points.size());
    }
    return true;
}

// ---- Straight skeleton ------------------------------------------------------------
//
// A kinetic simulation of the wavefront. Every active vertex moves at a
// constant velocity between events - the point where its two edges' moving
// lines meet - so its position is a line in time. Each step finds the
// earliest event among every loop, processes it, and looks again from
// scratch: no event queue to keep valid, at a cost of about the square of
// the vertex count a step, which is nothing at the sizes this is for.
//
//   collapse  two neighbours meet: the edge between them is gone, and one
//             vertex carries on between the edges either side.
//   split     a reflex vertex reaches an edge across the loop: the loop
//             splits in two there, each with a new vertex at the point.
//   finish    a loop down to two vertices has no area left: the segment
//             between them (a ridge) closes the two faces either side.
//
// Every vertex leaves a trace from where it started to where it ended,
// between the faces of its two edges; the faces are walked from those.

const size_t APMATH_POLY_SKELETON_DESC_V1_SIZE = sizeof(APMATH_POLY_SKELETON_DESC);

// Unit cross products below this count two edges as parallel (the header's
// rule).
const double SKELETON_PARALLEL = 1e-12;

struct sk_edge_t
{
    double dx, dy; // unit direction
    double nx, ny; // unit inward normal
    double c;      // the line at time t: n . x = c + w t
    double w;
};

struct sk_vertex_t
{
    double ox, oy, t0; // where it was, and when
    double vx, vy;
    uint32_t in, out; // the input edges either side
    uint32_t node;    // where it started
    uint32_t prev, next;
    bool alive, reflex;
};

struct sk_trace_t
{
    uint32_t a, b;           // nodes
    uint32_t face_a, face_b; // the input edges whose faces it divides
};

struct skeleton_t
{
    std::vector<sk_edge_t> edges;
    std::vector<sk_vertex_t> vertices;
    std::vector<double> node_x, node_y;
    std::vector<sk_trace_t> traces;
    double eps_length{0.0};

    double x_at(uint32_t v, double t) const
    {
        return vertices[v].ox + vertices[v].vx * (t - vertices[v].t0);
    }
    double y_at(uint32_t v, double t) const
    {
        return vertices[v].oy + vertices[v].vy * (t - vertices[v].t0);
    }
    uint32_t add_node(double x, double y)
    {
        node_x.push_back(x);
        node_y.push_back(y);
        return (uint32_t)node_x.size() - 1;
    }
    // Where [in]'s and [out]'s moving lines meet moves at this velocity.
    void set_velocity(sk_vertex_t &v) const
    {
        const sk_edge_t &a = edges[v.in];
        const sk_edge_t &b = edges[v.out];
        double det = a.nx * b.ny - a.ny * b.nx;
        double turn = a.dx * b.dy - a.dy * b.dx;
        v.reflex = turn < -SKELETON_PARALLEL;
        if (std::fabs(det) >= SKELETON_PARALLEL)
        {
            v.vx = (a.w * b.ny - b.w * a.ny) / det;
            v.vy = (a.nx * b.w - b.nx * a.w) / det;
        }
        else if (a.nx * b.nx + a.ny * b.ny > 0.0)
        {
            // One line: equal weights in valid input. Edges that come to
            // follow each other later with different weights are a
            // degenerate meeting; the faster one leads.
            double w = a.w > b.w ? a.w : b.w;
            v.vx = a.nx * w;
            v.vy = a.ny * w;
        }
        else
        {
            // Facing each other on one line: there's nothing between them.
            v.vx = 0.0;
            v.vy = 0.0;
        }
    }
    // Throws std::bad_alloc.
    uint32_t add_vertex(double x, double y, double t, uint32_t in, uint32_t out, uint32_t node)
    {
        sk_vertex_t v{};
        v.ox = x;
        v.oy = y;
        v.t0 = t;
        v.in = in;
        v.out = out;
        v.node = node;
        v.prev = v.next = UINT32_MAX;
        v.alive = true;
        set_velocity(v);
        vertices.push_back(v);
        return (uint32_t)vertices.size() - 1;
    }
    // [v] stops at [node], leaving its trace.
    void end_vertex(uint32_t v, uint32_t node)
    {
        sk_vertex_t &vertex = vertices[v];
        if (node != vertex.node && vertex.in != vertex.out)
            traces.push_back({vertex.node, node, vertex.in, vertex.out});
        vertex.alive = false;
    }
    uint32_t loop_size(uint32_t v) const
    {
        uint32_t count = 1;
        for (uint32_t at = vertices[v].next; at != v && count <= vertices.size(); at = vertices[at].next)
            count++;
        return count;
    }
    // A loop of two vertices or one, at time [t]: nothing left inside it.
    void finish_small(uint32_t v, double t)
    {
        uint32_t other = vertices[v].next;
        if (other == v)
        {
            end_vertex(v, add_node(x_at(v, t), y_at(v, t)));
            return;
        }
        uint32_t end_v = add_node(x_at(v, t), y_at(v, t));
        uint32_t end_other = add_node(x_at(other, t), y_at(other, t));
        uint32_t p = vertices[v].out, q = vertices[v].in;
        end_vertex(v, end_v);
        end_vertex(other, end_other);
        if (p != q)
            traces.push_back({end_v, end_other, p, q});
    }
};

// Runs the wavefront of [sk], whose vertices are the input polygon's, to
// the end. False, with [problem] set, if it can't close. Throws
// std::bad_alloc.
bool run_skeleton(skeleton_t &sk, const char *&problem)
{
    const double INF = std::numeric_limits<double>::infinity();
    double now = 0.0;
    size_t limit = 10 * sk.vertices.size() + 100;
    for (size_t step = 0; step < limit; step++)
    {
        // The earliest event of any loop: kind 0 a collapse of [v] with its
        // next, kind 1 [v] splitting the edge from [x] to its next.
        double best = INF;
        int kind = -1;
        uint32_t best_v = 0, best_x = 0;
        bool any = false;
        for (uint32_t u = 0; u < sk.vertices.size(); u++)
        {
            if (!sk.vertices[u].alive)
                continue;
            any = true;
            uint32_t w = sk.vertices[u].next;
            const sk_edge_t &e = sk.edges[sk.vertices[u].out];
            double ux = sk.x_at(u, now), uy = sk.y_at(u, now);
            double s0 = (sk.x_at(w, now) - ux) * e.dx + (sk.y_at(w, now) - uy) * e.dy;
            double rate = (sk.vertices[w].vx - sk.vertices[u].vx) * e.dx + (sk.vertices[w].vy - sk.vertices[u].vy) * e.dy;
            double t = INF;
            if (s0 <= sk.eps_length)
                t = now;
            else if (rate < 0.0)
                t = now + s0 / -rate;
            if (t < best)
            {
                best = t;
                kind = 0;
                best_v = u;
            }

            if (!sk.vertices[u].reflex)
                continue;
            const sk_vertex_t &vu = sk.vertices[u];
            for (uint32_t x = vu.next; x != u; x = sk.vertices[x].next)
            {
                uint32_t y = sk.vertices[x].next;
                if (y == u)
                    break;
                uint32_t edge = sk.vertices[x].out;
                if (edge == vu.in || edge == vu.out)
                    continue;
                const sk_edge_t &f = sk.edges[edge];
                double approach = f.nx * vu.vx + f.ny * vu.vy - f.w;
                if (approach >= -1e-12)
                    continue;
                double distance = f.nx * ux + f.ny * uy - (f.c + f.w * now);
                if (distance < -sk.eps_length)
                    continue;
                double tau = now + (distance > 0.0 ? distance : 0.0) / -approach;
                if (!(tau < best))
                    continue;
                double px = sk.x_at(u, tau), py = sk.y_at(u, tau);
                double ax = sk.x_at(x, tau), ay = sk.y_at(x, tau);
                double length = (sk.x_at(y, tau) - ax) * f.dx + (sk.y_at(y, tau) - ay) * f.dy;
                double along = (px - ax) * f.dx + (py - ay) * f.dy;
                if (length < -sk.eps_length)
                    continue;
                if (along < -sk.eps_length)
                    continue;
                if (along > length + sk.eps_length)
                    continue;
                best = tau;
                kind = 1;
                best_v = u;
                best_x = x;
            }
        }
        if (!any)
            return true;
        if (kind < 0)
        {
            problem = "part of the polygon never closes: its edges all have weight 0";
            return false;
        }
        if (best > now)
            now = best;

        if (kind == 0)
        {
            uint32_t u = best_v, w = sk.vertices[u].next;
            double px = (sk.x_at(u, now) + sk.x_at(w, now)) * 0.5;
            double py = (sk.y_at(u, now) + sk.y_at(w, now)) * 0.5;
            uint32_t node = sk.add_node(px, py);
            uint32_t before = sk.vertices[u].prev, after = sk.vertices[w].next;
            uint32_t in = sk.vertices[u].in, out = sk.vertices[w].out;
            bool alone = before == w; // a loop of two: u and w
            sk.end_vertex(u, node);
            sk.end_vertex(w, node);
            if (alone)
                continue;
            uint32_t m = sk.add_vertex(px, py, now, in, out, node);
            sk.vertices[m].prev = before;
            sk.vertices[m].next = after;
            sk.vertices[before].next = m;
            sk.vertices[after].prev = m;
            if (sk.loop_size(m) <= 2)
                sk.finish_small(m, now);
        }
        else
        {
            uint32_t u = best_v, x = best_x, y = sk.vertices[x].next;
            double px = sk.x_at(u, now), py = sk.y_at(u, now);
            uint32_t node = sk.add_node(px, py);
            uint32_t before = sk.vertices[u].prev, after = sk.vertices[u].next;
            uint32_t in = sk.vertices[u].in, out = sk.vertices[u].out, edge = sk.vertices[x].out;
            sk.end_vertex(u, node);
            uint32_t first = sk.add_vertex(px, py, now, in, edge, node);
            uint32_t second = sk.add_vertex(px, py, now, edge, out, node);
            sk.vertices[before].next = first;
            sk.vertices[first].prev = before;
            sk.vertices[first].next = y;
            sk.vertices[y].prev = first;
            sk.vertices[x].next = second;
            sk.vertices[second].prev = x;
            sk.vertices[second].next = after;
            sk.vertices[after].prev = second;
            if (sk.loop_size(first) <= 2)
                sk.finish_small(first, now);
            if (sk.vertices[second].alive && sk.loop_size(second) <= 2)
                sk.finish_small(second, now);
        }
    }
    problem = "the skeleton took more events than a polygon of this size can have";
    return false;
}

} // namespace

// ---- Checking -----------------------------------------------------------------------

APRESULT apmath_poly_validate(const APMATH_POLY_SET *set, APMATH_POLY_DEFECT_INFO *out_defect, APMATH_ERROR *out_error)
{
    clear(out_error);
    if (out_defect)
        *out_defect = defect(APMATH_POLY_DEFECT_NONE, 0);
    if (!set)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "set is null");
    if (!out_defect)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_defect is null");
    APRESULT result = check_set(*set, "the set", true, out_error);
    if (result != APRESULT_OK)
        return result;

    try
    {
        *out_defect = find_defect(*set);
    }
    catch (const std::bad_alloc &)
    {
        return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory checking the set");
    }
    return APRESULT_OK;
}

// ---- Booleans -----------------------------------------------------------------------

APRESULT apmath_poly_boolean(const APMATH_POLY_BOOLEAN_DESC *desc, apmath_poly *out_poly, APMATH_ERROR *out_error)
{
    clear(out_error);
    if (out_poly)
        *out_poly = nullptr;
    APMATH_POLY_BOOLEAN_DESC d;
    if (!read_versioned(desc, APMATH_POLY_BOOLEAN_DESC_V1_SIZE, d))
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null or its struct_size is too small");
    if (!out_poly)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_poly is null");

    Clipper2Lib::ClipType clip_type;
    switch (d.op)
    {
    case APMATH_POLY_OP_UNION:
        clip_type = Clipper2Lib::ClipType::Union;
        break;
    case APMATH_POLY_OP_INTERSECTION:
        clip_type = Clipper2Lib::ClipType::Intersection;
        break;
    case APMATH_POLY_OP_DIFFERENCE:
        clip_type = Clipper2Lib::ClipType::Difference;
        break;
    case APMATH_POLY_OP_XOR:
        clip_type = Clipper2Lib::ClipType::Xor;
        break;
    default:
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "op %d isn't an APMATH_POLY_OP", (int)d.op);
    }
    Clipper2Lib::FillRule fill_rule;
    switch (d.fill)
    {
    case APMATH_POLY_FILL_EVEN_ODD:
        fill_rule = Clipper2Lib::FillRule::EvenOdd;
        break;
    case APMATH_POLY_FILL_NON_ZERO:
        fill_rule = Clipper2Lib::FillRule::NonZero;
        break;
    case APMATH_POLY_FILL_POSITIVE:
        fill_rule = Clipper2Lib::FillRule::Positive;
        break;
    default:
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "fill %d isn't an APMATH_POLY_FILL", (int)d.fill);
    }
    APRESULT result = check_set(d.subject, "the subject", false, out_error);
    if (result != APRESULT_OK)
        return result;
    result = check_set(d.clip, "the clip", false, out_error);
    if (result != APRESULT_OK)
        return result;

    return compute_boolean(d.subject, d.clip, clip_type, fill_rule, out_poly, out_error);
}

APRESULT apmath_poly_offset_edges(const APMATH_POLY_OFFSET_DESC *desc, apmath_poly *out_poly, APMATH_ERROR *out_error)
{
    clear(out_error);
    if (out_poly)
        *out_poly = nullptr;
    APMATH_POLY_OFFSET_DESC d;
    if (!read_versioned(desc, APMATH_POLY_OFFSET_DESC_V1_SIZE, d))
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null or its struct_size is too small");
    if (!out_poly)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_poly is null");
    APRESULT result = check_set(d.set, "the set", false, out_error);
    if (result != APRESULT_OK)
        return result;
    if (d.set.point_count > 0 && !d.distances)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "distances is null");
    for (uint32_t i = 0; i < d.set.point_count; i++)
    {
        if (!std::isfinite(d.distances[i]))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "distance %u isn't finite", i);
    }
    if (!(d.miter_limit >= 1.0))
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "miter_limit is below 1");
    if (!std::isfinite(d.miter_limit))
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "miter_limit isn't finite");

    try
    {
        std::vector<APMATH_POLY_POINT> points;
        std::vector<uint64_t> tags;
        std::vector<uint32_t> ring_ends;
        points.reserve((size_t)d.set.point_count * 2);
        tags.reserve((size_t)d.set.point_count * 2);
        if (!move_edges(d.set, d.distances, d.miter_limit, points, tags, ring_ends, out_error))
            return APRESULT_INVALID_ARGUMENT;
        APMATH_POLY_SET moved{};
        moved.points = points.data();
        moved.edge_tags = tags.data();
        moved.point_count = (uint32_t)points.size();
        moved.ring_ends = ring_ends.data();
        moved.ring_count = (uint32_t)ring_ends.size();
        APMATH_POLY_SET none{};
        return compute_boolean(moved, none, Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::Positive, out_poly,
                               out_error);
    }
    catch (const std::bad_alloc &)
    {
        return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory moving the edges");
    }
}

APRESULT apmath_poly_skeleton(const APMATH_POLY_SKELETON_DESC *desc, apmath_poly *out_poly, APMATH_ERROR *out_error)
{
    clear(out_error);
    if (out_poly)
        *out_poly = nullptr;
    APMATH_POLY_SKELETON_DESC d;
    if (!read_versioned(desc, APMATH_POLY_SKELETON_DESC_V1_SIZE, d))
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is null or its struct_size is too small");
    if (!out_poly)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_poly is null");
    APRESULT result = check_set(d.set, "the set", false, out_error);
    if (result != APRESULT_OK)
        return result;
    if (d.set.ring_count == 0)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "the set has no ring");
    if (d.set.ring_count > 1)
        return fail(out_error, APRESULT_UNSUPPORTED, "the set has holes, which the skeleton doesn't take");
    if (!d.weights)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "weights is null");
    bool moving = false;
    for (uint32_t i = 0; i < d.set.point_count; i++)
    {
        if (!std::isfinite(d.weights[i]))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "weight %u isn't finite", i);
        if (d.weights[i] < 0.0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "weight %u is negative", i);
        moving = moving || d.weights[i] > 0.0;
    }
    if (!moving)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "every weight is 0");

    apmath_poly_t *poly = nullptr;
    try
    {
        APMATH_POLY_SET ring = d.set;
        ring.ring_outers = nullptr;
        APMATH_POLY_DEFECT_INFO defect = find_defect(ring);
        if (defect.defect != APMATH_POLY_DEFECT_NONE)
            return fail(out_error, APRESULT_INVALID_ARGUMENT,
                        "the set isn't a simple counter-clockwise ring (defect %d at edge %u)", (int)defect.defect,
                        defect.edge);

        // Relative to the first point, so the doubles stay small; exact,
        // since coordinates are within 2^40.
        uint32_t n = d.set.point_count;
        const APMATH_POLY_POINT &origin = d.set.points[0];
        auto px = [&](uint32_t i) { return (double)(d.set.points[i].x - origin.x); };
        auto py = [&](uint32_t i) { return (double)(d.set.points[i].y - origin.y); };

        skeleton_t sk;
        double lo_x = 0.0, hi_x = 0.0, lo_y = 0.0, hi_y = 0.0;
        sk.edges.resize(n);
        for (uint32_t i = 0; i < n; i++)
        {
            uint32_t j = (i + 1) % n;
            double dx = px(j) - px(i), dy = py(j) - py(i);
            double length = std::sqrt(dx * dx + dy * dy);
            sk_edge_t &e = sk.edges[i];
            e.dx = dx / length;
            e.dy = dy / length;
            e.nx = -e.dy;
            e.ny = e.dx;
            e.c = e.nx * px(i) + e.ny * py(i);
            e.w = d.weights[i];
            lo_x = px(i) < lo_x ? px(i) : lo_x;
            hi_x = px(i) > hi_x ? px(i) : hi_x;
            lo_y = py(i) < lo_y ? py(i) : lo_y;
            hi_y = py(i) > hi_y ? py(i) : hi_y;
        }
        for (uint32_t i = 0; i < n; i++)
        {
            const sk_edge_t &a = sk.edges[(i + n - 1) % n];
            const sk_edge_t &b = sk.edges[i];
            bool parallel = std::fabs(a.dx * b.dy - a.dy * b.dx) < SKELETON_PARALLEL && a.dx * b.dx + a.dy * b.dy > 0.0;
            if (parallel && a.w != b.w)
                return fail(out_error, APRESULT_INVALID_ARGUMENT,
                            "edges %u and %u follow each other on one line with different weights", (i + n - 1) % n, i);
        }
        double extent = hi_x - lo_x > hi_y - lo_y ? hi_x - lo_x : hi_y - lo_y;
        sk.eps_length = (extent > 1.0 ? extent : 1.0) * 1e-9;

        // The input corners: node i, and the vertex between edge i - 1 and
        // edge i.
        for (uint32_t i = 0; i < n; i++)
            sk.add_node(px(i), py(i));
        for (uint32_t i = 0; i < n; i++)
            sk.add_vertex(px(i), py(i), 0.0, (i + n - 1) % n, i, i);
        for (uint32_t i = 0; i < n; i++)
        {
            sk.vertices[i].prev = (i + n - 1) % n;
            sk.vertices[i].next = (i + 1) % n;
        }

        const char *problem = nullptr;
        if (!run_skeleton(sk, problem))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "the skeleton couldn't be traced: %s", problem);

        // Each face's boundary: its own edge, and the traces it lies beside,
        // each with the face across it.
        struct side_t
        {
            uint32_t a, b, across;
        };
        std::vector<std::vector<side_t>> sides(n);
        for (const sk_trace_t &trace : sk.traces)
        {
            sides[trace.face_a].push_back({trace.a, trace.b, trace.face_b});
            sides[trace.face_b].push_back({trace.a, trace.b, trace.face_a});
        }

        poly = (apmath_poly_t *)malloc(sizeof(apmath_poly_t));
        if (!poly)
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory for the result");
        poly = new (poly) apmath_poly_t();
        const double limit = (double)APMATH_POLY_COORD_LIMIT;
        std::vector<uint32_t> nodes;
        std::vector<uint64_t> tags;
        std::vector<APMATH_POLY_POINT> points;
        std::vector<bool> used;
        for (uint32_t face = 0; face < n; face++)
        {
            // From the edge's end back round to its start.
            uint32_t start = face, end = (face + 1) % n;
            nodes.assign({start, end});
            tags.assign({d.set.edge_tags ? d.set.edge_tags[face] : 0});
            used.assign(sides[face].size(), false);
            uint32_t at = end;
            bool closed = false;
            for (size_t walked = 0; walked <= sides[face].size() && !closed; walked++)
            {
                size_t found = SIZE_MAX;
                for (size_t k = 0; k < sides[face].size() && found == SIZE_MAX; k++)
                {
                    if (!used[k] && (sides[face][k].a == at || sides[face][k].b == at))
                        found = k;
                }
                if (found == SIZE_MAX)
                    break;
                used[found] = true;
                const side_t &side = sides[face][found];
                uint32_t next = side.a == at ? side.b : side.a;
                tags.push_back(d.set.edge_tags ? d.set.edge_tags[side.across] : 0);
                if (next == start)
                    closed = true;
                else
                    nodes.push_back(next);
                at = next;
            }
            if (!closed)
            {
                poly->~apmath_poly_t();
                free(poly);
                return fail(out_error, APRESULT_INVALID_ARGUMENT,
                            "the skeleton couldn't be traced: the face of edge %u doesn't close", face);
            }

            // On the grid, without points rounded onto their neighbours: of
            // two equal points the earlier goes, with the empty edge it
            // started, except the edge's own start.
            points.clear();
            for (uint32_t node : nodes)
            {
                double x = sk.node_x[node] + (double)origin.x, y = sk.node_y[node] + (double)origin.y;
                if (!(std::fabs(x) <= limit) || !(std::fabs(y) <= limit))
                {
                    poly->~apmath_poly_t();
                    free(poly);
                    return fail(out_error, APRESULT_INVALID_ARGUMENT, "a skeleton point lands past the coordinate limit");
                }
                points.push_back({(int64_t)std::llround(x), (int64_t)std::llround(y)});
            }
            for (size_t i = points.size(); i-- > 1;)
            {
                size_t next = (i + 1) % points.size();
                if (points.size() > 2 && same_point(points[i], points[next]))
                {
                    points.erase(points.begin() + (long)i);
                    tags.erase(tags.begin() + (long)i);
                }
            }
            poly->points.insert(poly->points.end(), points.begin(), points.end());
            poly->edge_tags.insert(poly->edge_tags.end(), tags.begin(), tags.end());
            poly->ring_ends.push_back((uint32_t)poly->points.size());
            poly->ring_outers.push_back(APMATH_POLY_NO_RING);
        }
    }
    catch (const std::bad_alloc &)
    {
        if (poly)
        {
            poly->~apmath_poly_t();
            free(poly);
        }
        return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory building the skeleton");
    }

    *out_poly = poly;
    return APRESULT_OK;
}

void apmath_poly_destroy(apmath_poly poly)
{
    if (!poly)
        return;
    poly->~apmath_poly_t();
    free(poly);
}

APMATH_POLY_SET apmath_poly_get_set(apmath_poly poly)
{
    APMATH_POLY_SET set;
    memset(&set, 0, sizeof(set));
    if (!poly)
        return set;
    set.points = poly->points.data();
    set.edge_tags = poly->edge_tags.data();
    set.point_count = (uint32_t)poly->points.size();
    set.ring_ends = poly->ring_ends.data();
    set.ring_outers = poly->ring_outers.data();
    set.ring_count = (uint32_t)poly->ring_ends.size();
    return set;
}

// ---- Triangulation ------------------------------------------------------------------

uint32_t apmath_poly_get_triangulation_capacity(const APMATH_POLY_SET *set)
{
    if (!set)
        return 0;
    if (!set->ring_ends)
        return 0;
    // Earcut joins each hole to its outer ring with a bridge there and back,
    // two more points, then clips n - 2 ears from n points.
    uint64_t triangles = 0;
    for (uint32_t ring = 0; ring < set->ring_count; ring++)
    {
        uint64_t count = ring_size(*set, ring);
        if (!is_outer(*set, ring))
            triangles += count + 2;
        else if (count > 2)
            triangles += count - 2;
    }
    uint64_t indices = triangles * 3;
    return indices > UINT32_MAX ? UINT32_MAX : (uint32_t)indices;
}

APRESULT apmath_poly_triangulate(const APMATH_POLY_SET *set, uint32_t *out_indices, uint32_t index_capacity,
                                 uint32_t *out_index_count, APMATH_ERROR *out_error)
{
    clear(out_error);
    if (out_index_count)
        *out_index_count = 0;
    if (!set)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "set is null");
    APRESULT result = check_set(*set, "the set", true, out_error);
    if (result != APRESULT_OK)
        return result;
    uint32_t capacity = apmath_poly_get_triangulation_capacity(set);
    if (capacity > 0 && !out_indices)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_indices is null");
    if (index_capacity < capacity)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "index_capacity is %u; the set can need %u", index_capacity,
                    capacity);
    if (!out_index_count)
        return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_index_count is null");

    try
    {
        // Each outer ring's holes, in ring order: hole_list[hole_first[r] ..
        // hole_first[r + 1]) for ring r.
        std::vector<uint32_t> hole_first(set->ring_count + 1, 0);
        for (uint32_t ring = 0; ring < set->ring_count; ring++)
        {
            if (!is_outer(*set, ring))
                hole_first[set->ring_outers[ring] + 1]++;
        }
        for (uint32_t ring = 0; ring < set->ring_count; ring++)
            hole_first[ring + 1] += hole_first[ring];
        std::vector<uint32_t> hole_list(hole_first[set->ring_count]);
        std::vector<uint32_t> filled(set->ring_count, 0);
        for (uint32_t ring = 0; ring < set->ring_count; ring++)
        {
            if (is_outer(*set, ring))
                continue;
            uint32_t outer = set->ring_outers[ring];
            hole_list[hole_first[outer] + filled[outer]++] = ring;
        }

        using point_t = std::array<double, 2>;
        std::vector<std::vector<point_t>> polygon;
        std::vector<uint32_t> point_index; // earcut's vertex -> the set's point
        uint32_t written = 0;
        for (uint32_t outer = 0; outer < set->ring_count; outer++)
        {
            if (!is_outer(*set, outer))
                continue;
            polygon.clear();
            point_index.clear();
            auto add = [&](uint32_t ring) {
                uint32_t begin = ring_begin(*set, ring), count = ring_size(*set, ring);
                polygon.emplace_back();
                polygon.back().reserve(count);
                for (uint32_t i = 0; i < count; i++)
                {
                    // Exact: coordinates are within 2^40.
                    polygon.back().push_back({(double)set->points[begin + i].x, (double)set->points[begin + i].y});
                    point_index.push_back(begin + i);
                }
            };
            add(outer);
            for (uint32_t h = hole_first[outer]; h < hole_first[outer + 1]; h++)
                add(hole_list[h]);

            std::vector<uint32_t> triangles = mapbox::earcut<uint32_t>(polygon);
            for (size_t t = 0; t + 2 < triangles.size(); t += 3)
            {
                uint32_t a = point_index[triangles[t]];
                uint32_t b = point_index[triangles[t + 1]];
                uint32_t c = point_index[triangles[t + 2]];
                int turn = orient(set->points[a], set->points[b], set->points[c]);
                if (turn == 0)
                    continue;
                if (turn < 0)
                    std::swap(b, c);
                // The capacity bounds what earcut makes; this only guards it.
                if (index_capacity - written < 3)
                    return fail(out_error, APRESULT_INVALID_ARGUMENT, "the triangulation came to more than its capacity");
                out_indices[written++] = a;
                out_indices[written++] = b;
                out_indices[written++] = c;
            }
        }
        *out_index_count = written;
    }
    catch (const std::bad_alloc &)
    {
        return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory triangulating the set");
    }
    return APRESULT_OK;
}
