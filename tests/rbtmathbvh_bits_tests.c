// The same bits on every platform (include/math/rbtmathbvh.h, "Same bits"):
// a fixed run of casts, overlaps and sweeps against an instance BVH and a
// box BVH, each answer hashed field by field, against values recorded on
// the first machine to run it (Windows 11, ThinkPad P1 Gen 5, MSVC,
// 2026-11-06). Another platform passes only if every query gives exactly
// what that one gave.
//
// The inputs are made from integers over powers of two and square roots
// (correctly rounded everywhere), so they are the same bits everywhere too.
//
// If a change to rbtmathbvh.cpp is meant to change a result, run with
// --print, check the change is the intended one, and record the new values
// in tests/data/rbtmathbvh/bits.inc with what changed and why: the hashes are
// the record, not the code.
//
// Written in C, as the other tests of a C header are.

#include "math/rbtmathbvh.h"
#include <math.h>
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

#define HASH(h, value) h = hash_bytes(h, &(value), sizeof(value))

static uint64_t hash_vec(uint64_t h, RbtDVec3 v)
{
    HASH(h, v.x);
    HASH(h, v.y);
    HASH(h, v.z);
    return h;
}

typedef struct recorded_value
{
    const char *what;
    uint64_t hash;
} recorded_value;

static const recorded_value recorded[] = {
#include "data/rbtmathbvh/bits.inc"
};

static void check(const char *what, uint64_t got)
{
    if (print_only)
    {
        printf("    {\"%s\", 0x%016llXull},\n", what, (unsigned long long)got);
        return;
    }
    for (size_t i = 0; i < sizeof(recorded) / sizeof(recorded[0]); i++)
        if (strcmp(recorded[i].what, what) == 0)
        {
            if (got != recorded[i].hash)
            {
                printf("FAIL %s: 0x%016llX, recorded 0x%016llX\n", what, (unsigned long long)got,
                       (unsigned long long)recorded[i].hash);
                failures++;
            }
            return;
        }
    printf("FAIL %s: nothing recorded\n", what);
    failures++;
}

// Repeatable numbers, the same on every platform: integers over 1024.
static uint64_t state = 0x2545F4914F6CDD1Dull;
static double next(int lo, int hi)
{
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    int64_t span = (int64_t)(hi - lo) * 1024 + 1;
    return (double)lo + (double)(int64_t)((state >> 33) % (uint64_t)span) / 1024.0;
}

static RbtDVec4 next_rotation(void)
{
    double x = next(-4, 4), y = next(-4, 4), z = next(-4, 4), w = next(-4, 4) + 0.5;
    double l = sqrt(x * x + y * y + z * z + w * w);
    RbtDVec4 q = {x / l, y / l, z / l, w / l};
    return q;
}

static const RbtDVec3 cube_positions[8] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                            {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
static const uint32_t cube_indices[36] = {0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4,
                                          2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};

static RBTMATH_SHAPE next_shape(int i)
{
    RBTMATH_SHAPE s;
    memset(&s, 0, sizeof(s));
    s.kind = (RBTMATH_SHAPE_KIND)(1 + i % 3);
    s.center.x = next(-2, 18);
    s.center.y = next(-2, 18);
    s.center.z = next(-2, 18);
    s.rotation = next_rotation();
    if (s.kind == RBTMATH_SHAPE_KIND_BOX)
    {
        s.half_extents.x = next(0, 1) + 0.0625;
        s.half_extents.y = next(0, 1) + 0.0625;
        s.half_extents.z = next(0, 1) + 0.0625;
    }
    else
        s.radius = next(0, 1) + 0.0625;
    if (s.kind == RBTMATH_SHAPE_KIND_CAPSULE)
        s.half_height = next(0, 1);
    return s;
}

int main(int argc, char **argv)
{
    print_only = argc > 1 && strcmp(argv[1], "--print") == 0;

    // A cube and a soup of 24 triangles, placed 64 times: turned, scaled,
    // one in four mirrored.
    RBTMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_MATH_MESH_BVH_DESC;
    desc.positions = cube_positions;
    desc.position_count = 8;
    desc.indices = cube_indices;
    desc.index_count = 36;
    rbtmath_mesh_bvh cube = NULL, soup = NULL;
    if (rbtmath_mesh_bvh_create(&desc, &cube) != RBT_RESULT_OK)
        return 1;
    RbtDVec3 soup_positions[72];
    for (int i = 0; i < 72; i++)
    {
        soup_positions[i].x = next(-1, 1);
        soup_positions[i].y = next(-1, 1);
        soup_positions[i].z = next(-1, 1);
    }
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_MATH_MESH_BVH_DESC;
    desc.positions = soup_positions;
    desc.position_count = 72;
    if (rbtmath_mesh_bvh_create(&desc, &soup) != RBT_RESULT_OK)
        return 1;
    enum
    {
        COUNT = 64
    };
    RbtDMat4 matrices[COUNT];
    RBTMATH_BVH_INSTANCE instances[COUNT];
    for (int i = 0; i < COUNT; i++)
    {
        RbtDVec4 q = next_rotation();
        double s[3] = {next(1, 2), next(1, 2), next(1, 2)};
        if (i % 4 == 3)
            s[0] = -s[0];
        double x = q.x, y = q.y, z = q.z, w = q.w;
        double r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y),
                       2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x),
                       2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
        memset(&matrices[i], 0, sizeof(RbtDMat4));
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++)
                matrices[i].m[c * 4 + k] = r[c * 3 + k] * s[c];
        matrices[i].m[12] = next(0, 16);
        matrices[i].m[13] = next(0, 16);
        matrices[i].m[14] = next(0, 16);
        matrices[i].m[15] = 1.0;
        instances[i].mesh = i % 2 ? soup : cube;
        instances[i].transform = &matrices[i];
        instances[i].id = (uint64_t)i;
        instances[i].mask = 1;
    }
    RBTMATH_INSTANCE_BVH_DESC instance_desc;
    memset(&instance_desc, 0, sizeof(instance_desc));
    instance_desc.base.type = RBT_STRUCT_TYPE_MATH_INSTANCE_BVH_DESC;
    instance_desc.instances = instances;
    instance_desc.instance_count = COUNT;
    rbtmath_instance_bvh bvh = NULL;
    if (rbtmath_instance_bvh_create(&instance_desc, &bvh) != RBT_RESULT_OK)
        return 1;

    // Casts.
    uint64_t h = 0xCBF29CE484222325ull, h_all = h;
    for (int i = 0; i < 256; i++)
    {
        RBTMATH_RAY ray;
        ray.origin.x = next(-4, 20);
        ray.origin.y = next(-4, 20);
        ray.origin.z = next(-4, 20);
        ray.direction.x = next(-1, 1);
        ray.direction.y = next(-1, 1);
        ray.direction.z = next(-1, 1);
        ray.t_min = 0.0;
        ray.t_max = 64.0;
        RBTMATH_INSTANCE_HIT hit;
        memset(&hit, 0, sizeof(hit));
        bool got = rbtmath_instance_bvh_cast(bvh, &ray, NULL, &hit);
        HASH(h, got);
        HASH(h, hit.instance);
        HASH(h, hit.triangle);
        HASH(h, hit.back_face);
        HASH(h, hit.t);
        h = hash_vec(h, hit.point);
        h = hash_vec(h, hit.normal);
        RBTMATH_INSTANCE_HIT all[8];
        uint32_t count = rbtmath_instance_bvh_cast_all(bvh, &ray, NULL, all, 8);
        HASH(h_all, count);
        for (uint32_t k = 0; k < count && k < 8; k++)
        {
            HASH(h_all, all[k].instance);
            HASH(h_all, all[k].t);
            h_all = hash_vec(h_all, all[k].normal);
        }
    }
    check("instance casts", h);
    check("instance cast_all", h_all);

    // Overlaps and sweeps.
    uint64_t h_overlap = 0xCBF29CE484222325ull, h_sweep = h_overlap, h_sweep_all = h_overlap;
    for (int i = 0; i < 256; i++)
    {
        RBTMATH_SHAPE s = next_shape(i);
        uint32_t list[16];
        uint32_t count = rbtmath_instance_bvh_overlap_all(bvh, &s, NULL, list, 16);
        HASH(h_overlap, count);
        for (uint32_t k = 0; k < count && k < 16; k++)
            HASH(h_overlap, list[k]);
        RBTMATH_SWEEP sweep;
        memset(&sweep, 0, sizeof(sweep));
        sweep.shape = s;
        sweep.direction.x = next(-1, 1);
        sweep.direction.y = next(-1, 1);
        sweep.direction.z = next(-1, 1);
        sweep.t_max = next(1, 16);
        RBTMATH_SWEEP_HIT hit;
        memset(&hit, 0, sizeof(hit));
        bool got = rbtmath_instance_bvh_sweep(bvh, &sweep, NULL, &hit);
        HASH(h_sweep, got);
        HASH(h_sweep, hit.instance);
        HASH(h_sweep, hit.triangle);
        HASH(h_sweep, hit.started_overlapping);
        HASH(h_sweep, hit.t);
        h_sweep = hash_vec(h_sweep, hit.point);
        h_sweep = hash_vec(h_sweep, hit.contact_normal);
        h_sweep = hash_vec(h_sweep, hit.face_normal);
        RBTMATH_SWEEP_HIT all[8];
        count = rbtmath_instance_bvh_sweep_all(bvh, &sweep, NULL, all, 8);
        HASH(h_sweep_all, count);
        for (uint32_t k = 0; k < count && k < 8; k++)
        {
            HASH(h_sweep_all, all[k].instance);
            HASH(h_sweep_all, all[k].t);
            h_sweep_all = hash_vec(h_sweep_all, all[k].contact_normal);
        }
    }
    check("instance overlap_all", h_overlap);
    check("instance sweeps", h_sweep);
    check("instance sweep_all", h_sweep_all);

    // Moved in place and refitted: the same queries, their own hash; and
    // the cost after a rebuild.
    uint32_t indices[COUNT];
    for (int i = 0; i < COUNT; i++)
    {
        matrices[i].m[12] += next(-4, 4);
        matrices[i].m[13] += next(-4, 4);
        indices[i] = (uint32_t)i;
    }
    if (rbtmath_instance_bvh_set_instances(bvh, indices, instances, COUNT, NULL) != RBT_RESULT_OK)
        return 1;
    rbtmath_instance_bvh_refit(bvh);
    uint64_t h_moved = 0xCBF29CE484222325ull;
    for (int i = 0; i < 128; i++)
    {
        RBTMATH_SHAPE s = next_shape(i);
        RBTMATH_SWEEP sweep;
        memset(&sweep, 0, sizeof(sweep));
        sweep.shape = s;
        sweep.direction.y = -1.0;
        sweep.t_max = 32.0;
        RBTMATH_SWEEP_HIT hit;
        memset(&hit, 0, sizeof(hit));
        bool got = rbtmath_instance_bvh_sweep(bvh, &sweep, NULL, &hit);
        HASH(h_moved, got);
        HASH(h_moved, hit.instance);
        HASH(h_moved, hit.t);
        h_moved = hash_vec(h_moved, hit.point);
    }
    check("moved sweeps", h_moved);
    rbtmath_instance_bvh_rebuild(bvh);
    double cost = rbtmath_instance_bvh_get_cost(bvh);
    uint64_t h_cost = 0xCBF29CE484222325ull;
    HASH(h_cost, cost);
    check("rebuilt cost", h_cost);

    // A box BVH: rays, overlaps and sweeps against solid boxes.
    RBTMATH_BVH_BOX boxes[128];
    for (int i = 0; i < 128; i++)
    {
        boxes[i].box.lo.x = next(0, 16);
        boxes[i].box.lo.y = next(0, 16);
        boxes[i].box.lo.z = next(0, 16);
        boxes[i].box.hi.x = boxes[i].box.lo.x + next(0, 2);
        boxes[i].box.hi.y = boxes[i].box.lo.y + next(0, 2);
        boxes[i].box.hi.z = boxes[i].box.lo.z + next(0, 2);
        boxes[i].id = (uint64_t)i;
        boxes[i].mask = 1;
    }
    RBTMATH_BOX_BVH_DESC box_desc;
    memset(&box_desc, 0, sizeof(box_desc));
    box_desc.base.type = RBT_STRUCT_TYPE_MATH_BOX_BVH_DESC;
    box_desc.boxes = boxes;
    box_desc.box_count = 128;
    rbtmath_box_bvh box_bvh = NULL;
    if (rbtmath_box_bvh_create(&box_desc, &box_bvh, NULL) != RBT_RESULT_OK)
        return 1;
    uint64_t h_box = 0xCBF29CE484222325ull;
    for (int i = 0; i < 256; i++)
    {
        RBTMATH_RAY ray;
        ray.origin.x = next(-2, 18);
        ray.origin.y = next(-2, 18);
        ray.origin.z = next(-2, 18);
        ray.direction.x = next(-1, 1);
        ray.direction.y = next(-1, 1);
        ray.direction.z = next(-1, 1);
        ray.t_min = 0.0;
        ray.t_max = 64.0;
        RBTMATH_BOX_HIT hit;
        memset(&hit, 0, sizeof(hit));
        bool got = rbtmath_box_bvh_cast(box_bvh, &ray, NULL, &hit);
        HASH(h_box, got);
        HASH(h_box, hit.box);
        HASH(h_box, hit.started_inside);
        HASH(h_box, hit.t);
        h_box = hash_vec(h_box, hit.point);
        h_box = hash_vec(h_box, hit.face_normal);
        RBTMATH_SHAPE s = next_shape(i);
        uint32_t list[16];
        uint32_t count = rbtmath_box_bvh_overlap_all(box_bvh, &s, NULL, list, 16);
        HASH(h_box, count);
        for (uint32_t k = 0; k < count && k < 16; k++)
            HASH(h_box, list[k]);
        RBTMATH_SWEEP sweep;
        memset(&sweep, 0, sizeof(sweep));
        sweep.shape = s;
        sweep.direction.x = next(-1, 1);
        sweep.direction.y = next(-1, 1);
        sweep.direction.z = next(-1, 1);
        sweep.t_max = next(1, 16);
        memset(&hit, 0, sizeof(hit));
        got = rbtmath_box_bvh_sweep(box_bvh, &sweep, NULL, &hit);
        HASH(h_box, got);
        HASH(h_box, hit.box);
        HASH(h_box, hit.started_inside);
        HASH(h_box, hit.t);
        h_box = hash_vec(h_box, hit.point);
        h_box = hash_vec(h_box, hit.contact_normal);
        h_box = hash_vec(h_box, hit.face_normal);
    }
    check("box queries", h_box);

    rbtmath_box_bvh_destroy(box_bvh);
    rbtmath_instance_bvh_destroy(bvh);
    rbtmath_mesh_bvh_destroy(soup);
    rbtmath_mesh_bvh_destroy(cube);
    if (print_only)
        return 0;
    if (failures)
    {
        printf("rbtmathbvh_bits_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("rbtmathbvh_bits_tests: all passed\n");
    return 0;
}
