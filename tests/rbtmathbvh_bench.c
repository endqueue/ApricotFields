// Numbers for Rabbittus Math's BVHs (include/math/rbtmathbvh.h), in Release:
// casts, sweeps and overlaps a second against a scene of placed meshes,
// and an instance BVH's upkeep - refit against rebuild against a fresh
// create - at 100, 10,000 and 100,000 instances, all moving and 1% moving.
// The numbers go into RBTMATH_TODO.md, beside each machine's.
//
// Not a ctest: it times, and a time is no pass or fail. Run it by hand,
// Release, with nothing else running: rbtmathbvh_bench [--quick] [--casts]
// (--casts: the casts alone, as they were measured before the shapes).
//
// Written in C, as the tests of a C header are.

#include "math/rbtmathbvh.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_seconds(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

// Repeatable random numbers in [0, 1).
static uint64_t random_state = 0x9E3779B97F4A7C15ull;
static double random_unit(void)
{
    random_state = random_state * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(random_state >> 11) / 9007199254740992.0;
}
static double random_range(double lo, double hi)
{
    return lo + (hi - lo) * random_unit();
}

// A sphere of 8 x 16 quads: 224 triangles, a mesh a little like a prop's.
static rbtmath_mesh_bvh make_ball(void)
{
    enum
    {
        RINGS = 8,
        SEGMENTS = 16
    };
    static RbtDVec3 positions[(RINGS + 1) * (SEGMENTS + 1)];
    static uint32_t indices[RINGS * SEGMENTS * 6];
    uint32_t p = 0, i = 0;
    for (uint32_t r = 0; r <= RINGS; r++)
        for (uint32_t s = 0; s <= SEGMENTS; s++)
        {
            double theta = 3.14159265358979323846 * r / RINGS, phi = 2.0 * 3.14159265358979323846 * s / SEGMENTS;
            RbtDVec3 v = {sin(theta) * cos(phi) * 0.5, cos(theta) * 0.5, sin(theta) * sin(phi) * 0.5};
            positions[p++] = v;
        }
    for (uint32_t r = 0; r < RINGS; r++)
        for (uint32_t s = 0; s < SEGMENTS; s++)
        {
            uint32_t a = r * (SEGMENTS + 1) + s, b = a + SEGMENTS + 1;
            if (r != 0)
            {
                indices[i++] = a;
                indices[i++] = b;
                indices[i++] = a + 1;
            }
            if (r != RINGS - 1)
            {
                indices[i++] = a + 1;
                indices[i++] = b;
                indices[i++] = b + 1;
            }
        }
    RBTMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_MATH_MESH_BVH_DESC;
    desc.positions = positions;
    desc.position_count = p;
    desc.indices = indices;
    desc.index_count = i;
    rbtmath_mesh_bvh bvh = NULL;
    if (rbtmath_mesh_bvh_create(&desc, &bvh) != RBT_RESULT_OK)
    {
        printf("couldn't make the mesh\n");
        exit(1);
    }
    return bvh;
}

// Instances scattered through a cube of side [extent]: turned about y and
// scaled, as props are.
static void place(RbtDMat4 *m, double extent)
{
    double angle = random_range(0.0, 6.283185307179586), scale = random_range(0.5, 2.0);
    double c = cos(angle) * scale, s = sin(angle) * scale;
    memset(m, 0, sizeof(*m));
    m->m[0] = c;
    m->m[2] = -s;
    m->m[5] = scale;
    m->m[8] = s;
    m->m[10] = c;
    m->m[12] = random_range(0.0, extent);
    m->m[13] = random_range(0.0, extent);
    m->m[14] = random_range(0.0, extent);
    m->m[15] = 1.0;
}

static RBTMATH_RAY random_ray(double extent)
{
    RBTMATH_RAY ray;
    ray.origin.x = random_range(0.0, extent);
    ray.origin.y = random_range(0.0, extent);
    ray.origin.z = random_range(0.0, extent);
    double x = random_range(-1, 1), y = random_range(-1, 1), z = random_range(-1, 1);
    double length = sqrt(x * x + y * y + z * z);
    ray.direction.x = x / length;
    ray.direction.y = y / length;
    ray.direction.z = z / length;
    ray.t_min = 0.0;
    ray.t_max = INFINITY;
    return ray;
}

// Rays a second, over [count] rays, repeated until a quarter second passes.
static void time_casts(rbtmath_instance_bvh bvh, const RBTMATH_RAY *rays, uint32_t count)
{
    RBTMATH_INSTANCE_HIT hit;
    uint32_t hits = 0;
    double start = now_seconds(), elapsed = 0.0;
    uint64_t cast = 0;
    while (elapsed < 0.25)
    {
        for (uint32_t i = 0; i < count; i++)
            hits += rbtmath_instance_bvh_cast(bvh, &rays[i], NULL, &hit);
        cast += count;
        elapsed = now_seconds() - start;
    }
    printf("  cast, nearest hit:  %10.0f rays/s (%u of %u hit)\n", (double)cast / elapsed, hits / (uint32_t)(cast / count),
           count);
    hits = 0;
    start = now_seconds();
    elapsed = 0.0;
    cast = 0;
    while (elapsed < 0.25)
    {
        for (uint32_t i = 0; i < count; i++)
            hits += rbtmath_instance_bvh_cast(bvh, &rays[i], NULL, NULL);
        cast += count;
        elapsed = now_seconds() - start;
    }
    printf("  cast, any hit:      %10.0f rays/s\n", (double)cast / elapsed);
    static RBTMATH_INSTANCE_HIT all[64];
    start = now_seconds();
    elapsed = 0.0;
    cast = 0;
    while (elapsed < 0.25)
    {
        for (uint32_t i = 0; i < count; i++)
            hits += rbtmath_instance_bvh_cast_all(bvh, &rays[i], NULL, all, 64);
        cast += count;
        elapsed = now_seconds() - start;
    }
    printf("  cast_all:           %10.0f rays/s\n", (double)cast / elapsed);
}

// Shapes a second: overlaps (any and all), sweeps of a set length, and
// sweeps of t_max INFINITY beside rays along the same lines.
static void time_shapes(rbtmath_instance_bvh bvh, double extent)
{
    enum
    {
        SHAPES = 1024
    };
    static RBTMATH_SHAPE shapes[SHAPES];
    static RBTMATH_SWEEP sweeps[SHAPES], infinite[SHAPES];
    static RBTMATH_RAY lines[SHAPES];
    for (uint32_t i = 0; i < SHAPES; i++)
    {
        RBTMATH_SHAPE *s = &shapes[i];
        memset(s, 0, sizeof(*s));
        s->kind = (RBTMATH_SHAPE_KIND)(1 + i % 3);
        s->center.x = random_range(0.0, extent);
        s->center.y = random_range(0.0, extent);
        s->center.z = random_range(0.0, extent);
        s->rotation.w = 1.0;
        s->radius = s->kind == RBTMATH_SHAPE_KIND_BOX ? 0.0 : 0.4;
        s->half_height = 0.5;
        s->half_extents.x = s->half_extents.y = s->half_extents.z = 0.4;
        RBTMATH_RAY r = random_ray(extent);
        sweeps[i].shape = *s;
        sweeps[i].direction = r.direction;
        sweeps[i].t_max = 10.0;
        infinite[i] = sweeps[i];
        infinite[i].t_max = INFINITY;
        lines[i] = r;
        lines[i].origin = s->center;
    }
    uint32_t list[64];
    RBTMATH_SWEEP_HIT hit;
    RBTMATH_INSTANCE_HIT ray_hit;
    const char *names[5] = {"overlaps_shape:", "overlap_all:", "sweep, 10 m:", "sweep, INFINITY:", "ray, same lines:"};
    for (int kind = 0; kind < 5; kind++)
    {
        uint32_t found = 0;
        double start = now_seconds(), elapsed = 0.0;
        uint64_t done = 0;
        while (elapsed < 0.25)
        {
            for (uint32_t i = 0; i < SHAPES; i++)
                switch (kind)
                {
                case 0:
                    found += rbtmath_instance_bvh_overlaps_shape(bvh, &shapes[i], NULL);
                    break;
                case 1:
                    found += rbtmath_instance_bvh_overlap_all(bvh, &shapes[i], NULL, list, 64);
                    break;
                case 2:
                    found += rbtmath_instance_bvh_sweep(bvh, &sweeps[i], NULL, &hit);
                    break;
                case 3:
                    found += rbtmath_instance_bvh_sweep(bvh, &infinite[i], NULL, &hit);
                    break;
                default:
                    found += rbtmath_instance_bvh_cast(bvh, &lines[i], NULL, &ray_hit);
                    break;
                }
            done += SHAPES;
            elapsed = now_seconds() - start;
        }
        printf("  %-19s %10.0f a second\n", names[kind], (double)done / elapsed);
    }
}

// Refit against rebuild against a fresh create, all moving and 1% moving:
// a set and a refit, a set and a rebuild, and a create, each a step, and
// the refitted tree's cost against a rebuilt one's after 60 steps.
static void time_upkeep(rbtmath_mesh_bvh mesh, uint32_t count, double extent)
{
    RbtDMat4 *matrices = (RbtDMat4 *)malloc(sizeof(RbtDMat4) * count);
    RBTMATH_BVH_INSTANCE *instances = (RBTMATH_BVH_INSTANCE *)malloc(sizeof(RBTMATH_BVH_INSTANCE) * count);
    uint32_t *indices = (uint32_t *)malloc(sizeof(uint32_t) * count);
    double *velocity = (double *)malloc(sizeof(double) * count * 3);
    for (uint32_t i = 0; i < count; i++)
    {
        place(&matrices[i], extent);
        instances[i].mesh = mesh;
        instances[i].transform = &matrices[i];
        instances[i].id = i;
        instances[i].mask = 1;
        indices[i] = i;
        velocity[i * 3 + 0] = random_range(-0.1, 0.1);
        velocity[i * 3 + 1] = random_range(-0.1, 0.1);
        velocity[i * 3 + 2] = random_range(-0.1, 0.1);
    }
    RBTMATH_INSTANCE_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_MATH_INSTANCE_BVH_DESC;
    desc.instances = instances;
    desc.instance_count = count;
    for (int share = 0; share < 2; share++)
    {
        // All moving, or the first 1%.
        uint32_t moving = share == 0 ? count : (count / 100 ? count / 100 : 1);
        rbtmath_instance_bvh refitted = NULL, rebuilt = NULL;
        if (rbtmath_instance_bvh_create(&desc, &refitted) != RBT_RESULT_OK ||
            rbtmath_instance_bvh_create(&desc, &rebuilt) != RBT_RESULT_OK)
        {
            printf("couldn't make the instances\n");
            exit(1);
        }
        double cost0 = rbtmath_instance_bvh_get_cost(refitted);
        double refit_time = 0.0, rebuild_time = 0.0, create_time = 0.0;
        const int STEPS = 60;
        for (int step = 0; step < STEPS; step++)
        {
            for (uint32_t i = 0; i < moving; i++)
            {
                matrices[i].m[12] += velocity[i * 3 + 0];
                matrices[i].m[13] += velocity[i * 3 + 1];
                matrices[i].m[14] += velocity[i * 3 + 2];
            }
            double start = now_seconds();
            rbtmath_instance_bvh_set_instances(refitted, indices, instances, moving, NULL);
            rbtmath_instance_bvh_refit(refitted);
            refit_time += now_seconds() - start;
            start = now_seconds();
            rbtmath_instance_bvh_set_instances(rebuilt, indices, instances, moving, NULL);
            rbtmath_instance_bvh_rebuild(rebuilt);
            rebuild_time += now_seconds() - start;
            if (step % 10 == 0)
            {
                rbtmath_instance_bvh fresh = NULL;
                start = now_seconds();
                rbtmath_instance_bvh_create(&desc, &fresh);
                create_time += now_seconds() - start;
                rbtmath_instance_bvh_destroy(fresh);
            }
        }
        printf("  %s moving: set + refit %9.1f us, set + rebuild %9.1f us, create %9.1f us a step; cost %.2f built, "
               "%.2f refitted after %d steps, %.2f rebuilt\n",
               share == 0 ? "all" : "1% ", refit_time / STEPS * 1e6, rebuild_time / STEPS * 1e6,
               create_time / (STEPS / 10) * 1e6, cost0, rbtmath_instance_bvh_get_cost(refitted), STEPS,
               rbtmath_instance_bvh_get_cost(rebuilt));
        rbtmath_instance_bvh_destroy(refitted);
        rbtmath_instance_bvh_destroy(rebuilt);
    }
    free(velocity);
    free(indices);
    free(instances);
    free(matrices);
}

int main(int argc, char **argv)
{
    bool quick = false, casts_only = false;
    for (int i = 1; i < argc; i++)
    {
        quick |= strcmp(argv[i], "--quick") == 0;
        casts_only |= strcmp(argv[i], "--casts") == 0;
    }
    rbtmath_mesh_bvh ball = make_ball();
    static const uint32_t counts[3] = {100, 10000, 100000};
    for (int c = 0; c < (quick ? 2 : 3); c++)
    {
        uint32_t count = counts[c];
        // The density of 10,000 in a 100 m cube, kept as the count changes.
        double extent = 100.0 * cbrt((double)count / 10000.0);
        RbtDMat4 *matrices = (RbtDMat4 *)malloc(sizeof(RbtDMat4) * count);
        RBTMATH_BVH_INSTANCE *instances = (RBTMATH_BVH_INSTANCE *)malloc(sizeof(RBTMATH_BVH_INSTANCE) * count);
        for (uint32_t i = 0; i < count; i++)
        {
            place(&matrices[i], extent);
            instances[i].mesh = ball;
            instances[i].transform = &matrices[i];
            instances[i].id = i;
            instances[i].mask = 1;
        }
        RBTMATH_INSTANCE_BVH_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.base.type = RBT_STRUCT_TYPE_MATH_INSTANCE_BVH_DESC;
        desc.instances = instances;
        desc.instance_count = count;
        rbtmath_instance_bvh bvh = NULL;
        double start = now_seconds();
        if (rbtmath_instance_bvh_create(&desc, &bvh) != RBT_RESULT_OK)
        {
            printf("couldn't make the instances\n");
            return 1;
        }
        double create = now_seconds() - start;
        printf("%u instances of a %u-triangle mesh, in a %.0f m cube: create %.3f ms\n", count,
               rbtmath_mesh_bvh_get_triangle_count(ball), extent, create * 1e3);

        enum
        {
            RAYS = 4096
        };
        static RBTMATH_RAY rays[RAYS];
        for (uint32_t i = 0; i < RAYS; i++)
            rays[i] = random_ray(extent);
        time_casts(bvh, rays, RAYS);
        // Their own random numbers, so each count's scene and rays are what
        // the casts' numbers before these were taken against.
        uint64_t saved = random_state;
        random_state = 0x5851F42D4C957F2Dull + count;
        if (!casts_only)
        {
            time_shapes(bvh, extent);
            time_upkeep(ball, count, extent);
        }
        random_state = saved;

        rbtmath_instance_bvh_destroy(bvh);
        free(instances);
        free(matrices);
    }
    rbtmath_mesh_bvh_destroy(ball);
    return 0;
}
