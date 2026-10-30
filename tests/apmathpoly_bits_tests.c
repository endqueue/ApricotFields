// The same bits on every platform (include/math/apmathpoly.h's promise): a
// fixed run of booleans, offsets (mitred and round), skeletons, point
// location and triangulations, each output hashed, against values recorded
// on macOS (Apple M5 Pro, 2026-10-30). Another platform passes only if
// every call gives exactly what macOS gave.
//
// If a change to apmathpoly.cpp is meant to change a result, run with
// --print, check the change is the intended one, and record the new values
// here with what changed and why: the hashes are the record, not the code.
//
// Written in C, as the other tests of a C header are.

#include "math/apmathpoly.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static bool print_only = false;

// FNV-1a over bytes, 64 bits.
static uint64_t hash_bytes(uint64_t hash, const void *bytes, size_t size)
{
    const unsigned char *p = (const unsigned char *)bytes;
    for (size_t i = 0; i < size; i++)
    {
        hash ^= p[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

// A set's arrays, each element written out field by field so padding
// never counts.
static uint64_t hash_set(const APMATH_POLY_SET *set)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    hash = hash_bytes(hash, &set->point_count, sizeof(set->point_count));
    hash = hash_bytes(hash, &set->ring_count, sizeof(set->ring_count));
    for (uint32_t i = 0; i < set->point_count; i++)
    {
        hash = hash_bytes(hash, &set->points[i].x, sizeof(int64_t));
        hash = hash_bytes(hash, &set->points[i].y, sizeof(int64_t));
        if (set->edge_tags)
            hash = hash_bytes(hash, &set->edge_tags[i], sizeof(uint64_t));
    }
    for (uint32_t i = 0; i < set->ring_count; i++)
    {
        hash = hash_bytes(hash, &set->ring_ends[i], sizeof(uint32_t));
        if (set->ring_outers)
            hash = hash_bytes(hash, &set->ring_outers[i], sizeof(uint32_t));
    }
    return hash;
}

static void expect(const char *what, uint64_t got, uint64_t recorded)
{
    if (print_only)
    {
        printf("    {\"%s\", 0x%016llXull},\n", what, (unsigned long long)got);
        return;
    }
    if (got != recorded)
    {
        printf("FAIL %s: 0x%016llX, recorded 0x%016llX\n", what, (unsigned long long)got, (unsigned long long)recorded);
        failures++;
    }
}

// Repeatable numbers, the same on every platform.
static uint64_t state = 0x2545F4914F6CDD1Dull;
static int64_t next_int(int64_t lo, int64_t hi)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return lo + (int64_t)((state >> 33) % (uint64_t)(hi - lo + 1));
}

#define MAX 64

typedef struct ring_set
{
    APMATH_POLY_POINT points[MAX];
    uint64_t tags[MAX];
    uint32_t ends[8];
    uint32_t outers[8];
    uint32_t point_count;
    uint32_t ring_count;
} ring_set;

static void add(ring_set *s, const int64_t *xy, uint32_t count, uint32_t outer)
{
    for (uint32_t i = 0; i < count; i++)
    {
        s->points[s->point_count].x = xy[2 * i];
        s->points[s->point_count].y = xy[2 * i + 1];
        s->tags[s->point_count] = 100 + s->point_count;
        s->point_count++;
    }
    s->ends[s->ring_count] = s->point_count;
    s->outers[s->ring_count] = outer;
    s->ring_count++;
}

static APMATH_POLY_SET view(const ring_set *s)
{
    APMATH_POLY_SET v;
    memset(&v, 0, sizeof(v));
    v.points = s->points;
    v.edge_tags = s->tags;
    v.point_count = s->point_count;
    v.ring_ends = s->ends;
    v.ring_outers = s->outers;
    v.ring_count = s->ring_count;
    return v;
}

// Recorded on macOS; see the top of this file.
typedef struct recorded_value
{
    const char *what;
    uint64_t hash;
} recorded_value;

static const recorded_value recorded[] = {
#include "data/apmathpoly/bits.inc"
};

static uint64_t recorded_hash(const char *what)
{
    for (size_t i = 0; i < sizeof(recorded) / sizeof(recorded[0]); i++)
        if (strcmp(recorded[i].what, what) == 0)
            return recorded[i].hash;
    return 0;
}

static void check(const char *what, uint64_t got)
{
    expect(what, got, recorded_hash(what));
}

int main(int argc, char **argv)
{
    print_only = argc > 1 && strcmp(argv[1], "--print") == 0;

    // Booleans of random overlapping polygons, every operation and fill.
    for (int round = 0; round < 8; round++)
    {
        ring_set a = {0}, b = {0};
        int64_t xy[24];
        uint32_t count = (uint32_t)next_int(3, 12);
        for (uint32_t i = 0; i < 2 * count; i++)
            xy[i] = next_int(-100000, 100000);
        add(&a, xy, count, APMATH_POLY_NO_RING);
        count = (uint32_t)next_int(3, 12);
        for (uint32_t i = 0; i < 2 * count; i++)
            xy[i] = next_int(-100000, 100000);
        add(&b, xy, count, APMATH_POLY_NO_RING);
        for (int op = 0; op < 4; op++)
        {
            APMATH_POLY_BOOLEAN_DESC desc;
            memset(&desc, 0, sizeof(desc));
            desc.base.type = APRI_STRUCT_TYPE_APMATH_POLY_BOOLEAN_DESC;
            desc.op = (APMATH_POLY_OP)op;
            desc.fill = (APMATH_POLY_FILL)(round % 3);
            desc.subject = view(&a);
            desc.clip = view(&b);
            apmath_poly poly = NULL;
            char what[64];
            snprintf(what, sizeof(what), "boolean %d op %d", round, op);
            if (apmath_poly_boolean(&desc, &poly, NULL) != APRESULT_OK)
            {
                printf("FAIL %s: the call failed\n", what);
                failures++;
                continue;
            }
            APMATH_POLY_SET result = apmath_poly_get_set(poly);
            check(what, hash_set(&result));

            // Point location against the result, and its triangulation.
            uint64_t places = 0xCBF29CE484222325ull;
            for (int k = 0; k < 64; k++)
            {
                APMATH_POLY_POINT p = {next_int(-110000, 110000), next_int(-110000, 110000)};
                APMATH_POLY_PLACE place = APMATH_POLY_PLACE_OUTSIDE;
                apmath_poly_locate_point(&result, APMATH_POLY_FILL_NON_ZERO, p, &place, NULL);
                uint32_t value = (uint32_t)place;
                places = hash_bytes(places, &value, sizeof(value));
            }
            snprintf(what, sizeof(what), "locate %d op %d", round, op);
            check(what, places);
            uint32_t capacity = apmath_poly_get_triangulation_capacity(&result);
            uint32_t indices[1024];
            uint32_t index_count = 0;
            if (capacity <= 1024 && apmath_poly_triangulate(&result, indices, capacity, &index_count, NULL) == APRESULT_OK)
            {
                uint64_t h = hash_bytes(0xCBF29CE484222325ull, &index_count, sizeof(index_count));
                h = hash_bytes(h, indices, sizeof(uint32_t) * index_count);
                snprintf(what, sizeof(what), "triangles %d op %d", round, op);
                check(what, h);
            }
            apmath_poly_destroy(poly);
        }
    }

    // Offsets: an outline with a hole, moved out and in, mitred and round.
    ring_set outline = {0};
    int64_t outer_xy[] = {0, 0, 50000, 0, 50000, 20000, 30000, 20000, 25000, 35000, 0, 30000};
    int64_t hole_xy[] = {10000, 5000, 10000, 15000, 20000, 15000, 20000, 5000};
    add(&outline, outer_xy, 6, APMATH_POLY_NO_RING);
    add(&outline, hole_xy, 4, 0);
    const double moves[4] = {1700.25, -1300.5, 650.0, -2900.75};
    for (int m = 0; m < 4; m++)
    {
        for (int join = 0; join < 2; join++)
        {
            double distances[MAX];
            for (uint32_t i = 0; i < outline.point_count; i++)
                distances[i] = moves[m] * (1.0 + 0.1 * (double)(i % 3));
            APMATH_POLY_OFFSET_DESC desc;
            memset(&desc, 0, sizeof(desc));
            desc.base.type = APRI_STRUCT_TYPE_APMATH_POLY_OFFSET_DESC;
            desc.set = view(&outline);
            desc.distances = distances;
            desc.miter_limit = 2.0;
            desc.join = (APMATH_POLY_JOIN)join;
            desc.arc_tolerance = 3.0;
            apmath_poly poly = NULL;
            char what[64];
            snprintf(what, sizeof(what), "offset %d join %d", m, join);
            if (apmath_poly_offset_edges(&desc, &poly, NULL) != APRESULT_OK)
            {
                printf("FAIL %s: the call failed\n", what);
                failures++;
                continue;
            }
            APMATH_POLY_SET result = apmath_poly_get_set(poly);
            check(what, hash_set(&result));
            apmath_poly_destroy(poly);
        }
    }

    // Skeletons: the outline with its hole at weight 1, and at mixed
    // weights; a step that merges on one line.
    for (int w = 0; w < 2; w++)
    {
        double weights[MAX];
        for (uint32_t i = 0; i < outline.point_count; i++)
            weights[i] = w == 0 ? 1.0 : 0.5 + 0.25 * (double)(i % 4);
        APMATH_POLY_SKELETON_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.base.type = APRI_STRUCT_TYPE_APMATH_POLY_SKELETON_DESC;
        desc.set = view(&outline);
        desc.weights = weights;
        apmath_poly poly = NULL;
        char what[64];
        snprintf(what, sizeof(what), "skeleton %d", w);
        if (apmath_poly_skeleton(&desc, &poly, NULL) != APRESULT_OK)
        {
            printf("FAIL %s: the call failed\n", what);
            failures++;
            continue;
        }
        APMATH_POLY_SET result = apmath_poly_get_set(poly);
        check(what, hash_set(&result));
        apmath_poly_destroy(poly);
    }
    {
        ring_set step = {0};
        int64_t step_xy[] = {0, 0, 4000, 0, 4000, 1000, 10000, 1000, 10000, 8000, 0, 8000};
        add(&step, step_xy, 6, APMATH_POLY_NO_RING);
        const double weights[6] = {2, 1, 1, 1, 1, 1};
        APMATH_POLY_SKELETON_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.base.type = APRI_STRUCT_TYPE_APMATH_POLY_SKELETON_DESC;
        desc.set = view(&step);
        desc.weights = weights;
        apmath_poly poly = NULL;
        if (apmath_poly_skeleton(&desc, &poly, NULL) != APRESULT_OK)
        {
            printf("FAIL skeleton step: the call failed\n");
            failures++;
        }
        else
        {
            APMATH_POLY_SET result = apmath_poly_get_set(poly);
            check("skeleton step", hash_set(&result));
            apmath_poly_destroy(poly);
        }
    }

    if (print_only)
        return 0;
    if (failures)
    {
        printf("apmathpoly_bits_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("apmathpoly_bits_tests: all passed\n");
    return 0;
}
