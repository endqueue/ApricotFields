// Headless tests for ApricotMath's polygons (include/math/apmathpoly.h).
//
// Written in C on purpose: apmathpoly.h is a C front end, and this proves it
// compiles and links as one.

#include "math/apmathpoly.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static const char *current_test = "";

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Repeatable random numbers.
static uint64_t random_state = 0x9E3779B97F4A7C15ull;
static int64_t random_int(int64_t lo, int64_t hi)
{
    random_state = random_state * 6364136223846793005ull + 1442695040888963407ull;
    return lo + (int64_t)((random_state >> 33) % (uint64_t)(hi - lo + 1));
}

// ---- Building sets ----------------------------------------------------------

#define MAX_POINTS 64
#define MAX_RINGS 8

// A set with room of its own, filled ring by ring.
typedef struct test_set
{
    APMATH_POLY_POINT points[MAX_POINTS];
    uint64_t tags[MAX_POINTS];
    uint32_t ring_ends[MAX_RINGS];
    uint32_t ring_outers[MAX_RINGS];
    uint32_t point_count;
    uint32_t ring_count;
} test_set;

// Adds a ring of [count] xy pairs, its edges tagged [first_tag], [first_tag]
// + 1, ... - a hole in ring [outer], or an outer ring for APMATH_POLY_NO_RING.
static void add_ring(test_set *set, const int64_t *xy, uint32_t count, uint64_t first_tag, uint32_t outer)
{
    for (uint32_t i = 0; i < count; i++)
    {
        set->points[set->point_count].x = xy[2 * i];
        set->points[set->point_count].y = xy[2 * i + 1];
        set->tags[set->point_count] = first_tag + i;
        set->point_count++;
    }
    set->ring_ends[set->ring_count] = set->point_count;
    set->ring_outers[set->ring_count] = outer;
    set->ring_count++;
}

// A counter-clockwise rectangle from (x0, y0) to (x1, y1), its edges tagged
// from [first_tag]: bottom, right, top, left.
static void add_rect(test_set *set, int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint64_t first_tag)
{
    int64_t xy[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
    add_ring(set, xy, 4, first_tag, APMATH_POLY_NO_RING);
}

static APMATH_POLY_SET view(const test_set *set)
{
    APMATH_POLY_SET view;
    memset(&view, 0, sizeof(view));
    view.points = set->points;
    view.edge_tags = set->tags;
    view.point_count = set->point_count;
    view.ring_ends = set->ring_ends;
    view.ring_outers = set->ring_outers;
    view.ring_count = set->ring_count;
    return view;
}

static uint32_t ring_begin(const APMATH_POLY_SET *set, uint32_t ring)
{
    return ring ? set->ring_ends[ring - 1] : 0;
}

// Twice a ring's signed area: positive counter-clockwise. Exact for the small
// coordinates these tests use.
static double ring_area2(const APMATH_POLY_SET *set, uint32_t ring)
{
    uint32_t begin = ring_begin(set, ring), end = set->ring_ends[ring];
    double area = 0.0;
    for (uint32_t i = begin; i < end; i++)
    {
        const APMATH_POLY_POINT *a = &set->points[i];
        const APMATH_POLY_POINT *b = &set->points[i + 1 < end ? i + 1 : begin];
        area += (double)a->x * (double)b->y - (double)b->x * (double)a->y;
    }
    return area;
}

// Twice the area a set encloses: outer rings less holes, given their windings.
static double set_area2(const APMATH_POLY_SET *set)
{
    double area = 0.0;
    for (uint32_t ring = 0; ring < set->ring_count; ring++)
        area += ring_area2(set, ring);
    return area;
}

static APMATH_POLY_DEFECT_INFO validate(const APMATH_POLY_SET *set)
{
    APMATH_POLY_DEFECT_INFO info;
    APRESULT result = apmath_poly_validate(set, &info, NULL);
    CHECK(result == APRESULT_OK);
    return info;
}

static apmath_poly run_boolean(APMATH_POLY_OP op, const test_set *subject, const test_set *clip)
{
    APMATH_POLY_BOOLEAN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.op = op;
    desc.fill = APMATH_POLY_FILL_NON_ZERO;
    if (subject)
        desc.subject = view(subject);
    if (clip)
        desc.clip = view(clip);
    apmath_poly poly = NULL;
    APMATH_ERROR error;
    APRESULT result = apmath_poly_boolean(&desc, &poly, &error);
    CHECK(result == APRESULT_OK);
    CHECK(poly != NULL);
    if (result != APRESULT_OK)
        printf("  %s\n", error.message);
    return poly;
}

// Whether ring [ring] of [set] is exactly [xy] ([count] points) with edge tags
// [tags].
static bool ring_is(const APMATH_POLY_SET *set, uint32_t ring, const int64_t *xy, const uint64_t *tags, uint32_t count)
{
    uint32_t begin = ring_begin(set, ring);
    if (set->ring_ends[ring] - begin != count)
        return false;
    for (uint32_t i = 0; i < count; i++)
    {
        if (set->points[begin + i].x != xy[2 * i] || set->points[begin + i].y != xy[2 * i + 1])
            return false;
        if (tags && set->edge_tags[begin + i] != tags[i])
            return false;
    }
    return true;
}

// Triangulates [set] and returns twice the triangles' total area, checking
// each runs counter-clockwise, has area and indexes the set's points.
static double triangulated_area2(const APMATH_POLY_SET *set, uint32_t *out_triangle_count)
{
    uint32_t capacity = apmath_poly_get_triangulation_capacity(set);
    uint32_t *indices = (uint32_t *)malloc(sizeof(uint32_t) * (capacity ? capacity : 1));
    uint32_t count = 0;
    APRESULT result = apmath_poly_triangulate(set, indices, capacity, &count, NULL);
    CHECK(result == APRESULT_OK);
    CHECK(count % 3 == 0);
    CHECK(count <= capacity);
    double area = 0.0;
    for (uint32_t t = 0; t + 2 < count; t += 3)
    {
        CHECK(indices[t] < set->point_count);
        CHECK(indices[t + 1] < set->point_count);
        CHECK(indices[t + 2] < set->point_count);
        const APMATH_POLY_POINT *a = &set->points[indices[t]];
        const APMATH_POLY_POINT *b = &set->points[indices[t + 1]];
        const APMATH_POLY_POINT *c = &set->points[indices[t + 2]];
        double twice = (double)(b->x - a->x) * (double)(c->y - a->y) - (double)(b->y - a->y) * (double)(c->x - a->x);
        CHECK(twice > 0.0);
        area += twice;
    }
    free(indices);
    if (out_triangle_count)
        *out_triangle_count = count / 3;
    return area;
}

// ---- Validation -------------------------------------------------------------

static void test_validate_simple_rings(void)
{
    current_test = "validate_simple_rings";
    test_set set = {0};
    add_rect(&set, 0, 0, 10, 10, 1);
    APMATH_POLY_SET s = view(&set);
    CHECK(validate(&s).defect == APMATH_POLY_DEFECT_NONE);

    // An L, with a point in the middle of a straight run: still fine.
    test_set l = {0};
    int64_t l_xy[] = {0, 0, 5, 0, 10, 0, 10, 4, 4, 4, 4, 10, 0, 10};
    add_ring(&l, l_xy, 7, 1, APMATH_POLY_NO_RING);
    s = view(&l);
    CHECK(validate(&s).defect == APMATH_POLY_DEFECT_NONE);

    // NULL tags and NULL outers are allowed.
    s.edge_tags = NULL;
    s.ring_outers = NULL;
    CHECK(validate(&s).defect == APMATH_POLY_DEFECT_NONE);
}

static void test_validate_defects(void)
{
    current_test = "validate_defects";
    APMATH_POLY_SET s;
    APMATH_POLY_DEFECT_INFO info;

    test_set few = {0};
    int64_t few_xy[] = {0, 0, 10, 0};
    add_ring(&few, few_xy, 2, 0, APMATH_POLY_NO_RING);
    s = view(&few);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_TOO_FEW_POINTS);
    CHECK(info.ring == 0);

    test_set repeated = {0};
    int64_t repeated_xy[] = {0, 0, 10, 0, 10, 0, 10, 10};
    add_ring(&repeated, repeated_xy, 4, 0, APMATH_POLY_NO_RING);
    s = view(&repeated);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_ZERO_LENGTH_EDGE);
    CHECK(info.edge == 1);

    test_set flat = {0};
    int64_t flat_xy[] = {0, 0, 5, 5, 10, 10};
    add_ring(&flat, flat_xy, 3, 0, APMATH_POLY_NO_RING);
    s = view(&flat);
    CHECK(validate(&s).defect == APMATH_POLY_DEFECT_NO_AREA);

    // A bow tie: edge 0 crosses edge 2.
    test_set bow = {0};
    int64_t bow_xy[] = {0, 0, 10, 10, 10, 0, 0, 10};
    add_ring(&bow, bow_xy, 4, 0, APMATH_POLY_NO_RING);
    s = view(&bow);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_EDGES_MEET);
    CHECK(info.ring == 0);
    CHECK(info.edge == 0);
    CHECK(info.other_edge == 2);

    // A spike doubling back along itself: edges 1 and 2 follow each other
    // and overlap.
    test_set spike = {0};
    int64_t spike_xy[] = {0, 0, 10, 0, 15, 0, 12, 0, 10, 10};
    add_ring(&spike, spike_xy, 5, 0, APMATH_POLY_NO_RING);
    s = view(&spike);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_EDGES_MEET);
    CHECK(info.edge == 1);
    CHECK(info.other_edge == 2);

    // A ring touching itself: its point (5, 0) lies on edge 0.
    test_set touch = {0};
    int64_t touch_xy[] = {0, 0, 10, 0, 10, 10, 5, 0, 0, 10};
    add_ring(&touch, touch_xy, 5, 0, APMATH_POLY_NO_RING);
    s = view(&touch);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_EDGES_MEET);
    CHECK(info.edge == 0);
    CHECK(info.other_edge == 2);

    // Clockwise outer ring.
    test_set cw = {0};
    int64_t cw_xy[] = {0, 0, 0, 10, 10, 10, 10, 0};
    add_ring(&cw, cw_xy, 4, 0, APMATH_POLY_NO_RING);
    s = view(&cw);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_WRONG_WINDING);
    CHECK(info.ring == 0);

    // Two rings that share an edge's stretch.
    test_set touching = {0};
    add_rect(&touching, 0, 0, 10, 10, 0);
    add_rect(&touching, 10, 2, 20, 8, 0);
    s = view(&touching);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_EDGES_MEET);
    CHECK(info.ring == 0);
    CHECK(info.other_ring == 1);
}

static void test_validate_holes(void)
{
    current_test = "validate_holes";
    APMATH_POLY_SET s;
    APMATH_POLY_DEFECT_INFO info;
    int64_t hole_xy[] = {3, 3, 3, 6, 6, 6, 6, 3}; // clockwise

    test_set holed = {0};
    add_rect(&holed, 0, 0, 10, 10, 0);
    add_ring(&holed, hole_xy, 4, 0, 0);
    s = view(&holed);
    CHECK(validate(&s).defect == APMATH_POLY_DEFECT_NONE);

    // An island standing in the hole: its own polygon, and fine.
    test_set island = holed;
    add_rect(&island, 4, 4, 5, 5, 0);
    s = view(&island);
    CHECK(validate(&s).defect == APMATH_POLY_DEFECT_NONE);

    // The hole run counter-clockwise.
    test_set wrong = {0};
    add_rect(&wrong, 0, 0, 10, 10, 0);
    add_rect(&wrong, 3, 3, 6, 6, 0);
    wrong.ring_outers[1] = 0;
    s = view(&wrong);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_WRONG_WINDING);
    CHECK(info.ring == 1);

    // A hole outside its outer ring.
    test_set outside = {0};
    int64_t far_xy[] = {20, 20, 20, 30, 30, 30, 30, 20};
    add_rect(&outside, 0, 0, 10, 10, 0);
    add_ring(&outside, far_xy, 4, 0, 0);
    s = view(&outside);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_HOLE_OUTSIDE);
    CHECK(info.ring == 1);
    CHECK(info.other_ring == 0);

    // One polygon inside another, not in a hole of it.
    test_set nested = {0};
    add_rect(&nested, 0, 0, 10, 10, 0);
    add_rect(&nested, 2, 2, 4, 4, 0);
    s = view(&nested);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_OVERLAP);
    CHECK(info.ring == 1);
    CHECK(info.other_ring == 0);

    // A hole inside another hole of the same polygon.
    test_set double_hole = {0};
    int64_t inner_xy[] = {4, 4, 4, 5, 5, 5, 5, 4};
    add_rect(&double_hole, 0, 0, 10, 10, 0);
    add_ring(&double_hole, hole_xy, 4, 0, 0);
    add_ring(&double_hole, inner_xy, 4, 0, 0);
    s = view(&double_hole);
    info = validate(&s);
    CHECK(info.defect == APMATH_POLY_DEFECT_OVERLAP);
    CHECK(info.ring == 2);
    CHECK(info.other_ring == 1);
}

static void test_validate_arguments(void)
{
    current_test = "validate_arguments";
    APMATH_POLY_DEFECT_INFO info;
    APMATH_ERROR error;
    test_set set = {0};
    add_rect(&set, 0, 0, 10, 10, 0);
    APMATH_POLY_SET s = view(&set);

    CHECK(apmath_poly_validate(NULL, &info, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != '\0');
    CHECK(apmath_poly_validate(&s, NULL, NULL) == APRESULT_INVALID_ARGUMENT);

    APMATH_POLY_SET bad = s;
    uint32_t short_ends[1] = {3};
    bad.ring_ends = short_ends;
    CHECK(apmath_poly_validate(&bad, &info, NULL) == APRESULT_INVALID_ARGUMENT);

    test_set far = set;
    far.points[2].x = APMATH_POLY_COORD_LIMIT + 1;
    bad = view(&far);
    CHECK(apmath_poly_validate(&bad, &info, NULL) == APRESULT_INVALID_ARGUMENT);

    // A ring naming a hole as its outer ring.
    test_set chain = {0};
    add_rect(&chain, 0, 0, 10, 10, 0);
    add_rect(&chain, 1, 1, 2, 2, 0);
    add_rect(&chain, 3, 3, 4, 4, 0);
    chain.ring_outers[1] = 0;
    chain.ring_outers[2] = 1;
    bad = view(&chain);
    CHECK(apmath_poly_validate(&bad, &info, NULL) == APRESULT_INVALID_ARGUMENT);

    // The empty set is valid.
    APMATH_POLY_SET empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(validate(&empty).defect == APMATH_POLY_DEFECT_NONE);
}

// ---- Booleans -----------------------------------------------------------------

// Two overlapping squares, every edge of every result traced to its input.
static void test_boolean_tags(void)
{
    current_test = "boolean_tags";
    test_set a = {0}, b = {0};
    add_rect(&a, 0, 0, 10, 10, 1);   // 1 bottom, 2 right, 3 top, 4 left
    add_rect(&b, 5, 5, 15, 15, 11);  // 11 bottom, 12 right, 13 top, 14 left

    apmath_poly u = run_boolean(APMATH_POLY_OP_UNION, &a, &b);
    APMATH_POLY_SET s = apmath_poly_get_set(u);
    CHECK(s.ring_count == 1);
    int64_t u_xy[] = {0, 0, 10, 0, 10, 5, 15, 5, 15, 15, 5, 15, 5, 10, 0, 10};
    uint64_t u_tags[] = {1, 2, 11, 12, 13, 14, 3, 4};
    CHECK(s.ring_count == 1 && ring_is(&s, 0, u_xy, u_tags, 8));
    CHECK(s.ring_outers[0] == APMATH_POLY_NO_RING);
    CHECK(validate(&s).defect == APMATH_POLY_DEFECT_NONE);
    apmath_poly_destroy(u);

    apmath_poly d = run_boolean(APMATH_POLY_OP_DIFFERENCE, &a, &b);
    s = apmath_poly_get_set(d);
    int64_t d_xy[] = {0, 0, 10, 0, 10, 5, 5, 5, 5, 10, 0, 10};
    uint64_t d_tags[] = {1, 2, 11, 14, 3, 4};
    CHECK(s.ring_count == 1 && ring_is(&s, 0, d_xy, d_tags, 6));
    apmath_poly_destroy(d);

    apmath_poly i = run_boolean(APMATH_POLY_OP_INTERSECTION, &a, &b);
    s = apmath_poly_get_set(i);
    int64_t i_xy[] = {5, 5, 10, 5, 10, 10, 5, 10};
    uint64_t i_tags[] = {11, 2, 3, 14};
    CHECK(s.ring_count == 1 && ring_is(&s, 0, i_xy, i_tags, 4));
    apmath_poly_destroy(i);

    // XOR: two L shapes, touching at two corners - however they come out,
    // they cover the right area.
    apmath_poly x = run_boolean(APMATH_POLY_OP_XOR, &a, &b);
    s = apmath_poly_get_set(x);
    CHECK(set_area2(&s) == 2.0 * (100.0 + 100.0 - 2.0 * 25.0));
    apmath_poly_destroy(x);
}

// A hole comes out as a hole: clockwise, after its outer ring, named by it.
static void test_boolean_hole(void)
{
    current_test = "boolean_hole";
    test_set outer = {0}, inner = {0};
    add_rect(&outer, 0, 0, 10, 10, 1);
    add_rect(&inner, 3, 3, 6, 6, 11);
    apmath_poly d = run_boolean(APMATH_POLY_OP_DIFFERENCE, &outer, &inner);
    APMATH_POLY_SET s = apmath_poly_get_set(d);
    CHECK(s.ring_count == 2);
    if (s.ring_count == 2)
    {
        CHECK(s.ring_outers[0] == APMATH_POLY_NO_RING);
        CHECK(s.ring_outers[1] == 0);
        int64_t hole_xy[] = {3, 3, 3, 6, 6, 6, 6, 3};
        uint64_t hole_tags[] = {14, 13, 12, 11};
        CHECK(ring_is(&s, 1, hole_xy, hole_tags, 4));
        CHECK(validate(&s).defect == APMATH_POLY_DEFECT_NONE);
        CHECK(set_area2(&s) == 2.0 * (100.0 - 9.0));
    }
    apmath_poly_destroy(d);
}

// A union with no clip cleans the subject up: a clockwise ring turned round,
// a point in a straight run gone, the start moved to the lowest point.
static void test_boolean_cleanup(void)
{
    current_test = "boolean_cleanup";
    test_set set = {0};
    int64_t xy[] = {10, 10, 10, 0, 5, 0, 0, 0, 0, 10};
    add_ring(&set, xy, 5, 1, APMATH_POLY_NO_RING);
    apmath_poly u = run_boolean(APMATH_POLY_OP_UNION, &set, NULL);
    APMATH_POLY_SET s = apmath_poly_get_set(u);
    int64_t out_xy[] = {0, 0, 10, 0, 10, 10, 0, 10};
    // Edges 2 and 3 ran (5,0)-(0,0) and (10,0)-(5,0); the bottom lies
    // along both, equally near, so it takes the first: 2.
    uint64_t out_tags[] = {2, 1, 5, 4};
    CHECK(s.ring_count == 1 && ring_is(&s, 0, out_xy, out_tags, 4));
    apmath_poly_destroy(u);

    // Nothing in, nothing out.
    apmath_poly empty = run_boolean(APMATH_POLY_OP_UNION, NULL, NULL);
    s = apmath_poly_get_set(empty);
    CHECK(s.ring_count == 0);
    CHECK(s.point_count == 0);
    apmath_poly_destroy(empty);
}

// Random rectangles: every intersection is on the grid, so areas add up
// exactly - |A u B| + |A n B| = |A| + |B|, and A - B is A less A n B.
static void test_boolean_against_areas(void)
{
    current_test = "boolean_against_areas";
    for (int round = 0; round < 200; round++)
    {
        test_set a = {0}, b = {0};
        int rects = (int)random_int(1, 3);
        for (int r = 0; r < rects; r++)
        {
            int64_t x = random_int(-50, 50), y = random_int(-50, 50);
            add_rect(&a, x, y, x + random_int(1, 40), y + random_int(1, 40), 100 + 4 * r);
            x = random_int(-50, 50);
            y = random_int(-50, 50);
            add_rect(&b, x, y, x + random_int(1, 40), y + random_int(1, 40), 200 + 4 * r);
        }
        apmath_poly ua = run_boolean(APMATH_POLY_OP_UNION, &a, NULL);
        apmath_poly ub = run_boolean(APMATH_POLY_OP_UNION, &b, NULL);
        apmath_poly u = run_boolean(APMATH_POLY_OP_UNION, &a, &b);
        apmath_poly n = run_boolean(APMATH_POLY_OP_INTERSECTION, &a, &b);
        apmath_poly d = run_boolean(APMATH_POLY_OP_DIFFERENCE, &a, &b);
        APMATH_POLY_SET sa = apmath_poly_get_set(ua), sb = apmath_poly_get_set(ub);
        APMATH_POLY_SET su = apmath_poly_get_set(u), sn = apmath_poly_get_set(n), sd = apmath_poly_get_set(d);

        CHECK(set_area2(&su) + set_area2(&sn) == set_area2(&sa) + set_area2(&sb));
        CHECK(set_area2(&sd) == set_area2(&sa) - set_area2(&sn));

        // Outer rings counter-clockwise, holes clockwise, holes after an
        // outer ring, each tag one of the inputs'.
        APMATH_POLY_SET results[3] = {su, sn, sd};
        for (int k = 0; k < 3; k++)
        {
            const APMATH_POLY_SET *s = &results[k];
            for (uint32_t ring = 0; ring < s->ring_count; ring++)
            {
                bool outer = s->ring_outers[ring] == APMATH_POLY_NO_RING;
                CHECK(outer ? ring_area2(s, ring) > 0.0 : ring_area2(s, ring) < 0.0);
                CHECK(outer || s->ring_outers[ring] < ring);
            }
            for (uint32_t i = 0; i < s->point_count; i++)
            {
                uint64_t tag = s->edge_tags[i];
                CHECK((tag >= 100 && tag < 112) || (tag >= 200 && tag < 212));
            }
            // And they triangulate to the same area.
            CHECK(triangulated_area2(s, NULL) == set_area2(s));
        }

        // The same desc twice, the same result.
        apmath_poly again = run_boolean(APMATH_POLY_OP_UNION, &a, &b);
        APMATH_POLY_SET sagain = apmath_poly_get_set(again);
        CHECK(sagain.point_count == su.point_count);
        CHECK(sagain.ring_count == su.ring_count);
        if (sagain.point_count == su.point_count && sagain.ring_count == su.ring_count)
        {
            CHECK(memcmp(sagain.points, su.points, sizeof(APMATH_POLY_POINT) * su.point_count) == 0);
            CHECK(memcmp(sagain.edge_tags, su.edge_tags, sizeof(uint64_t) * su.point_count) == 0);
            CHECK(memcmp(sagain.ring_ends, su.ring_ends, sizeof(uint32_t) * su.ring_count) == 0);
        }

        apmath_poly_destroy(again);
        apmath_poly_destroy(ua);
        apmath_poly_destroy(ub);
        apmath_poly_destroy(u);
        apmath_poly_destroy(n);
        apmath_poly_destroy(d);
    }
}

// Slanted edges: intersections round onto the grid, and each result edge
// still takes the tag of the input edge it runs along.
static void test_boolean_rounding(void)
{
    current_test = "boolean_rounding";
    test_set a = {0}, b = {0};
    int64_t tri_xy[] = {0, 0, 1000003, 0, 0, 999983};
    add_ring(&a, tri_xy, 3, 1, APMATH_POLY_NO_RING); // 1 bottom, 2 hypotenuse, 3 left
    add_rect(&b, 1, -10, 500001, 700001, 11);        // 11 bottom, 12 right, 13 top, 14 left
    apmath_poly n = run_boolean(APMATH_POLY_OP_INTERSECTION, &a, &b);
    APMATH_POLY_SET s = apmath_poly_get_set(n);
    CHECK(s.ring_count == 1);
    // The cut runs: along the bottom (1), up the rect's right side (12), back
    // along the hypotenuse (2), across the rect's top (13), down its left (14).
    for (uint32_t i = 0; i < s.point_count; i++)
    {
        const APMATH_POLY_POINT *p = &s.points[i];
        const APMATH_POLY_POINT *q = &s.points[(i + 1) % s.point_count];
        uint64_t expected = 0;
        if (p->y == 0 && q->y == 0)
            expected = 1;
        else if (p->x == 500001 && q->x == 500001)
            expected = 12;
        else if (p->y == 700001 && q->y == 700001)
            expected = 13;
        else if (p->x == 1 && q->x == 1)
            expected = 14;
        else
            expected = 2;
        CHECK(s.edge_tags[i] == expected);
    }
    apmath_poly_destroy(n);
}

static void test_boolean_arguments(void)
{
    current_test = "boolean_arguments";
    test_set a = {0};
    add_rect(&a, 0, 0, 10, 10, 0);
    APMATH_POLY_BOOLEAN_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.subject = view(&a);
    apmath_poly poly = (apmath_poly)1;
    APMATH_ERROR error;

    CHECK(apmath_poly_boolean(NULL, &poly, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(poly == NULL);
    CHECK(apmath_poly_boolean(&desc, NULL, NULL) == APRESULT_INVALID_ARGUMENT);

    APMATH_POLY_BOOLEAN_DESC bad = desc;
    bad.op = (APMATH_POLY_OP)99;
    CHECK(apmath_poly_boolean(&bad, &poly, NULL) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.fill = (APMATH_POLY_FILL)99;
    CHECK(apmath_poly_boolean(&bad, &poly, NULL) == APRESULT_INVALID_ARGUMENT);
    bad = desc;
    bad.struct_size = 4;
    CHECK(apmath_poly_boolean(&bad, &poly, NULL) == APRESULT_INVALID_ARGUMENT);

    test_set far = a;
    far.points[0].y = -APMATH_POLY_COORD_LIMIT - 1;
    bad = desc;
    bad.clip = view(&far);
    CHECK(apmath_poly_boolean(&bad, &poly, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "clip") != NULL);

    // At the limit is fine.
    test_set edge = {0};
    add_rect(&edge, -APMATH_POLY_COORD_LIMIT, -APMATH_POLY_COORD_LIMIT, APMATH_POLY_COORD_LIMIT,
             APMATH_POLY_COORD_LIMIT, 0);
    desc.subject = view(&edge);
    CHECK(apmath_poly_boolean(&desc, &poly, NULL) == APRESULT_OK);
    apmath_poly_destroy(poly);
    apmath_poly_destroy(NULL);

    APMATH_POLY_SET none = apmath_poly_get_set(NULL);
    CHECK(none.point_count == 0);
    CHECK(none.points == NULL);
}

// ---- Offsetting -------------------------------------------------------------------

static apmath_poly run_offset(const test_set *set, const double *distances, double miter_limit)
{
    APMATH_POLY_OFFSET_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.set = view(set);
    desc.distances = distances;
    desc.miter_limit = miter_limit;
    apmath_poly poly = NULL;
    APMATH_ERROR error;
    APRESULT result = apmath_poly_offset_edges(&desc, &poly, &error);
    CHECK(result == APRESULT_OK);
    if (result != APRESULT_OK)
        printf("  %s\n", error.message);
    return poly;
}

// Every edge moved, one edge moved, holes, and an inward move that leaves
// nothing.
static void test_offset_moves(void)
{
    current_test = "offset_moves";
    test_set square = {0};
    add_rect(&square, 0, 0, 10, 10, 1);

    const double all_one[4] = {1, 1, 1, 1};
    apmath_poly grown = run_offset(&square, all_one, 2.0);
    APMATH_POLY_SET s = apmath_poly_get_set(grown);
    int64_t grown_xy[] = {-1, -1, 11, -1, 11, 11, -1, 11};
    uint64_t grown_tags[] = {1, 2, 3, 4};
    CHECK(s.ring_count == 1 && ring_is(&s, 0, grown_xy, grown_tags, 4));
    apmath_poly_destroy(grown);

    // Only the right edge, by 3: its neighbours stretch to meet it.
    const double right_only[4] = {0, 3, 0, 0};
    apmath_poly wider = run_offset(&square, right_only, 2.0);
    s = apmath_poly_get_set(wider);
    int64_t wider_xy[] = {0, 0, 13, 0, 13, 10, 0, 10};
    CHECK(s.ring_count == 1 && ring_is(&s, 0, wider_xy, grown_tags, 4));
    apmath_poly_destroy(wider);

    // Nothing moved: the same square.
    const double none[4] = {0, 0, 0, 0};
    apmath_poly same = run_offset(&square, none, 1.0);
    s = apmath_poly_get_set(same);
    int64_t same_xy[] = {0, 0, 10, 0, 10, 10, 0, 10};
    CHECK(s.ring_count == 1 && ring_is(&s, 0, same_xy, grown_tags, 4));
    apmath_poly_destroy(same);

    // A hole's edges move into it: the outline grows by 1, the hole shrinks
    // by 1 on each side.
    test_set holed = {0};
    int64_t hole_xy[] = {3, 3, 3, 6, 6, 6, 6, 3};
    add_rect(&holed, 0, 0, 10, 10, 1);
    add_ring(&holed, hole_xy, 4, 11, 0);
    const double holed_one[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    apmath_poly holed_grown = run_offset(&holed, holed_one, 2.0);
    s = apmath_poly_get_set(holed_grown);
    CHECK(s.ring_count == 2);
    CHECK(set_area2(&s) == 2.0 * (144.0 - 1.0));
    apmath_poly_destroy(holed_grown);

    // Moved in by more than half its width, the square turns inside out,
    // and nothing is left.
    const double inward[4] = {-6, -6, -6, -6};
    apmath_poly gone = run_offset(&square, inward, 2.0);
    s = apmath_poly_get_set(gone);
    CHECK(s.ring_count == 0);
    apmath_poly_destroy(gone);
}

// Mitres within the limit, bevels past it, and a step where two edges in
// one line move apart.
static void test_offset_corners(void)
{
    current_test = "offset_corners";
    test_set sliver = {0};
    int64_t sliver_xy[] = {0, 0, 100, 0, 0, 10};
    add_ring(&sliver, sliver_xy, 3, 1, APMATH_POLY_NO_RING);
    const double one[3] = {1, 1, 1};

    // A limit of 1 bevels every corner: none mitres within 1.
    apmath_poly bevelled = run_offset(&sliver, one, 1.0);
    APMATH_POLY_SET s = apmath_poly_get_set(bevelled);
    CHECK(s.ring_count == 1 && s.point_count == 6);
    apmath_poly_destroy(bevelled);

    // A limit of 10 mitres the right angle and the corner at (0, 10), and
    // bevels the sharp one at (100, 0), whose mitre is about 20 out.
    apmath_poly mixed = run_offset(&sliver, one, 10.0);
    s = apmath_poly_get_set(mixed);
    CHECK(s.ring_count == 1 && s.point_count == 4);
    CHECK(s.ring_count == 1 && s.points[0].x == -1 && s.points[0].y == -1);
    apmath_poly_destroy(mixed);

    // The bottom's second half moved down 2: a step down at (5, 0), tagged
    // as the half after it.
    test_set stepped = {0};
    int64_t stepped_xy[] = {0, 0, 5, 0, 10, 0, 10, 10, 0, 10};
    add_ring(&stepped, stepped_xy, 5, 1, APMATH_POLY_NO_RING);
    const double second_half[5] = {0, 2, 0, 0, 0};
    apmath_poly step = run_offset(&stepped, second_half, 2.0);
    s = apmath_poly_get_set(step);
    int64_t step_xy[] = {5, -2, 10, -2, 10, 10, 0, 10, 0, 0, 5, 0};
    uint64_t step_tags[] = {2, 3, 4, 5, 1, 2};
    CHECK(s.ring_count == 1 && ring_is(&s, 0, step_xy, step_tags, 6));
    CHECK(set_area2(&s) == 2.0 * 110.0);
    apmath_poly_destroy(step);

    // The same desc twice, the same points.
    apmath_poly a = run_offset(&sliver, one, 10.0);
    apmath_poly b = run_offset(&sliver, one, 10.0);
    APMATH_POLY_SET sa = apmath_poly_get_set(a), sb = apmath_poly_get_set(b);
    CHECK(sa.point_count == sb.point_count);
    if (sa.point_count == sb.point_count)
        CHECK(memcmp(sa.points, sb.points, sizeof(APMATH_POLY_POINT) * sa.point_count) == 0);
    apmath_poly_destroy(a);
    apmath_poly_destroy(b);
}

static void test_offset_arguments(void)
{
    current_test = "offset_arguments";
    test_set square = {0};
    add_rect(&square, 0, 0, 10, 10, 1);
    const double one[4] = {1, 1, 1, 1};
    APMATH_POLY_OFFSET_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.set = view(&square);
    desc.distances = one;
    desc.miter_limit = 2.0;
    apmath_poly poly = (apmath_poly)1;
    APMATH_ERROR error;

    CHECK(apmath_poly_offset_edges(NULL, &poly, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(poly == NULL);
    CHECK(apmath_poly_offset_edges(&desc, NULL, NULL) == APRESULT_INVALID_ARGUMENT);
    APMATH_POLY_OFFSET_DESC bad = desc;
    bad.distances = NULL;
    CHECK(apmath_poly_offset_edges(&bad, &poly, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "distances") != NULL);
    const double not_finite[4] = {1, NAN, 1, 1};
    bad = desc;
    bad.distances = not_finite;
    CHECK(apmath_poly_offset_edges(&bad, &poly, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "distance 1") != NULL);
    bad = desc;
    bad.miter_limit = 0.5;
    CHECK(apmath_poly_offset_edges(&bad, &poly, NULL) == APRESULT_INVALID_ARGUMENT);
    bad.miter_limit = NAN;
    CHECK(apmath_poly_offset_edges(&bad, &poly, NULL) == APRESULT_INVALID_ARGUMENT);
    bad.miter_limit = INFINITY;
    CHECK(apmath_poly_offset_edges(&bad, &poly, NULL) == APRESULT_INVALID_ARGUMENT);

    // A corner moved past the coordinate limit.
    const double huge[4] = {2e12, 2e12, 2e12, 2e12};
    bad = desc;
    bad.distances = huge;
    CHECK(apmath_poly_offset_edges(&bad, &poly, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "coordinate limit") != NULL);
    CHECK(poly == NULL);
}

// ---- Triangulation ----------------------------------------------------------------

static void test_triangulate_shapes(void)
{
    current_test = "triangulate_shapes";
    uint32_t triangles = 0;

    test_set square = {0};
    add_rect(&square, 0, 0, 10, 10, 0);
    APMATH_POLY_SET s = view(&square);
    CHECK(apmath_poly_get_triangulation_capacity(&s) == 6);
    CHECK(triangulated_area2(&s, &triangles) == 200.0);
    CHECK(triangles == 2);

    // A U shape, which a fan from any corner would get wrong.
    test_set u = {0};
    int64_t u_xy[] = {0, 0, 9, 0, 9, 9, 6, 9, 6, 3, 3, 3, 3, 9, 0, 9};
    add_ring(&u, u_xy, 8, 0, APMATH_POLY_NO_RING);
    s = view(&u);
    CHECK(triangulated_area2(&s, &triangles) == 2.0 * (81.0 - 18.0));
    CHECK(triangles == 6);

    // A square with a hole: 8 points + 2 for the bridge, 8 triangles.
    test_set holed = {0};
    int64_t hole_xy[] = {3, 3, 3, 6, 6, 6, 6, 3};
    add_rect(&holed, 0, 0, 10, 10, 0);
    add_ring(&holed, hole_xy, 4, 0, 0);
    s = view(&holed);
    CHECK(apmath_poly_get_triangulation_capacity(&s) == 3 * 8);
    CHECK(triangulated_area2(&s, &triangles) == 2.0 * (100.0 - 9.0));
    CHECK(triangles == 8);

    // Two polygons in one set, each triangulated on its own.
    test_set two = {0};
    add_rect(&two, 0, 0, 1, 1, 0);
    add_rect(&two, 5, 5, 7, 7, 0);
    s = view(&two);
    CHECK(triangulated_area2(&s, &triangles) == 2.0 * (1.0 + 4.0));
    CHECK(triangles == 4);

    // A point in a straight run leaves a triangle with no area, which
    // isn't written.
    test_set run = {0};
    int64_t run_xy[] = {0, 0, 5, 0, 10, 0, 10, 10, 0, 10};
    add_ring(&run, run_xy, 5, 0, APMATH_POLY_NO_RING);
    s = view(&run);
    CHECK(triangulated_area2(&s, &triangles) == 200.0);
    CHECK(triangles <= 3);
}

static void test_triangulate_arguments(void)
{
    current_test = "triangulate_arguments";
    test_set square = {0};
    add_rect(&square, 0, 0, 10, 10, 0);
    APMATH_POLY_SET s = view(&square);
    uint32_t indices[6];
    uint32_t count = 99;

    CHECK(apmath_poly_triangulate(NULL, indices, 6, &count, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(count == 0);
    CHECK(apmath_poly_triangulate(&s, NULL, 6, &count, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(apmath_poly_triangulate(&s, indices, 5, &count, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(apmath_poly_triangulate(&s, indices, 6, NULL, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(apmath_poly_triangulate(&s, indices, 6, &count, NULL) == APRESULT_OK);
    CHECK(count == 6);

    // The empty set needs nothing.
    APMATH_POLY_SET empty;
    memset(&empty, 0, sizeof(empty));
    CHECK(apmath_poly_get_triangulation_capacity(&empty) == 0);
    CHECK(apmath_poly_triangulate(&empty, NULL, 0, &count, NULL) == APRESULT_OK);
    CHECK(count == 0);
    CHECK(apmath_poly_get_triangulation_capacity(NULL) == 0);
}

int main(void)
{
    test_validate_simple_rings();
    test_validate_defects();
    test_validate_holes();
    test_validate_arguments();
    test_boolean_tags();
    test_boolean_hole();
    test_boolean_cleanup();
    test_boolean_against_areas();
    test_boolean_rounding();
    test_boolean_arguments();
    test_offset_moves();
    test_offset_corners();
    test_offset_arguments();
    test_triangulate_shapes();
    test_triangulate_arguments();

    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("apmathpoly_tests: all passed\n");
    return 0;
}
