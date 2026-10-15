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
