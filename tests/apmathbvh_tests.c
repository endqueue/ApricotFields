// Headless tests for ApricotMath's ray casting (include/math/apmathbvh.h).
//
// Written in C on purpose: apmathbvh.h is a C front end, and this proves it
// compiles and links as one.

#include "math/apmathbvh.h"
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

static bool near(double a, double b)
{
    return fabs(a - b) <= 1e-9 * (1.0 + fabs(a) + fabs(b));
}

static bool near3(ApriDVec3 v, double x, double y, double z)
{
    return near(v.x, x) && near(v.y, y) && near(v.z, z);
}

static ApriDVec3 vec3(double x, double y, double z)
{
    ApriDVec3 v = {x, y, z};
    return v;
}

static APMATH_RAY make_ray(double ox, double oy, double oz, double dx, double dy, double dz)
{
    APMATH_RAY ray;
    ray.origin = vec3(ox, oy, oz);
    ray.direction = vec3(dx, dy, dz);
    ray.t_min = 0.0;
    ray.t_max = INFINITY;
    return ray;
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

// The unit cube from (0, 0, 0) to (1, 1, 1): 8 corners, 12 triangles
// facing out, two per face in the order -X, +X, -Y, +Y, -Z, +Z.
static const ApriDVec3 cube_positions[8] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                            {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
static const uint32_t cube_indices[36] = {0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4,
                                          2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};

static apmath_mesh_bvh make_cube(void)
{
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    desc.positions = cube_positions;
    desc.position_count = 8;
    desc.indices = cube_indices;
    desc.index_count = 36;
    apmath_mesh_bvh bvh = NULL;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
    return bvh;
}

static void test_ray_clip(void)
{
    current_test = "ray_clip";
    APMATH_AABB box = {{0, 0, 0}, {1, 1, 1}};
    APMATH_RAY ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(apmath_ray_clip_aabb(&ray, &box));
    CHECK(near(ray.t_min, 2.0) && near(ray.t_max, 3.0));
    // Starting inside keeps its start.
    ray = make_ray(0.25, 0.5, 0.5, 1, 0, 0);
    CHECK(apmath_ray_clip_aabb(&ray, &box));
    CHECK(near(ray.t_min, 0.0) && near(ray.t_max, 0.75));
    // A miss leaves the ray alone.
    ray = make_ray(-2, 5, 0.5, 1, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &box));
    CHECK(ray.t_min == 0.0 && ray.t_max == INFINITY);
    // Pointing away.
    ray = make_ray(-2, 0.5, 0.5, -1, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &box));
    // A slab between two planes: infinite along the other axes.
    APMATH_AABB slab = {{-INFINITY, -INFINITY, 1}, {INFINITY, INFINITY, 3}};
    ray = make_ray(100, -50, 10, 0, 0, -2);
    CHECK(apmath_ray_clip_aabb(&ray, &slab));
    CHECK(near(ray.t_min, 3.5) && near(ray.t_max, 4.5));
    // Along the slab, inside it and outside it.
    ray = make_ray(0, 0, 2, 1, 0, 0);
    CHECK(apmath_ray_clip_aabb(&ray, &slab));
    CHECK(ray.t_min == 0.0 && ray.t_max == INFINITY);
    ray = make_ray(0, 0, 5, 1, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &slab));
    // Nothing to clip.
    APMATH_AABB empty = {{1, 1, 1}, {0, 0, 0}};
    ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &empty));
    CHECK(!apmath_ray_clip_aabb(NULL, &box));
    CHECK(!apmath_ray_clip_aabb(&ray, NULL));
    ray = make_ray(0.5, 0.5, 0.5, 0, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &box));
}

static void test_mesh_arguments(void)
{
    current_test = "mesh_arguments";
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    apmath_mesh_bvh bvh = (apmath_mesh_bvh)1;
    CHECK(apmath_mesh_bvh_create(NULL, &bvh) == APRESULT_INVALID_ARGUMENT);
    CHECK(bvh == NULL);
    CHECK(apmath_mesh_bvh_create(&desc, NULL) == APRESULT_INVALID_ARGUMENT);

    // No triangles: valid, never hit.
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
    CHECK(apmath_mesh_bvh_get_triangle_count(bvh) == 0);
    APMATH_AABB bounds = apmath_mesh_bvh_get_bounds(bvh);
    CHECK(bounds.lo.x > bounds.hi.x);
    APMATH_RAY ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(bvh, &ray, NULL));
    apmath_mesh_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(NULL);

    desc.positions = cube_positions;
    desc.position_count = 8; // not a multiple of three, with no indices
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    desc.indices = cube_indices;
    desc.index_count = 35;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    uint32_t past[3] = {0, 1, 8};
    desc.indices = past;
    desc.index_count = 3;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    ApriDVec3 not_finite[3] = {{0, 0, 0}, {1, 0, 0}, {0, NAN, 0}};
    desc.positions = not_finite;
    desc.position_count = 3;
    desc.indices = NULL;
    desc.index_count = 0;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    desc.base.type = APRI_STRUCT_TYPE_APMATH_INSTANCE_BVH_DESC;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_STRUCT_WRONG_TYPE);
    desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    APRI_STRUCT_BASE chained = {APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC, NULL};
    desc.base.next = &chained;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_STRUCT_UNKNOWN_IN_CHAIN);
    CHECK(bvh == NULL);
}

static void test_mesh_cube(void)
{
    current_test = "mesh_cube";
    apmath_mesh_bvh cube = make_cube();
    CHECK(apmath_mesh_bvh_get_triangle_count(cube) == 12);
    APMATH_AABB bounds = apmath_mesh_bvh_get_bounds(cube);
    CHECK(near3(bounds.lo, 0, 0, 0) && near3(bounds.hi, 1, 1, 1));

    // From outside: the -X face, from its front.
    APMATH_RAY ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    APMATH_MESH_HIT hit;
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 2.0) && !hit.back_face && hit.triangle < 2);
    CHECK(near3(hit.point, 0, 0.25, 0.5) && near3(hit.normal, -1, 0, 0));
    CHECK(apmath_mesh_bvh_cast(cube, &ray, NULL));

    // From inside: the +X face, from its back; the normal still points out.
    ray = make_ray(0.5, 0.25, 0.5, 1, 0, 0);
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 0.5) && hit.back_face && (hit.triangle == 2 || hit.triangle == 3));
    CHECK(near3(hit.normal, 1, 0, 0));

    // A direction that isn't unit length: t counts in its lengths.
    ray = make_ray(-2, 0.25, 0.5, 4, 0, 0);
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 0.5) && near3(hit.point, 0, 0.25, 0.5));

    // The range: starting past the near face, ending before any, and a
    // negative start.
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    ray.t_min = 2.5;
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 3.0) && hit.back_face);
    ray.t_min = 0.0;
    ray.t_max = 1.5;
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(3, 0.25, 0.5, 1, 0, 0);
    ray.t_min = -10.0;
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, -3.0));

    // Misses, and rays that can't hit anything.
    ray = make_ray(-2, 5, 0.5, 1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(-2, 0.25, 0.5, -1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(-2, 0.25, 0.5, 0, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(NAN, 0.25, 0.5, 1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    ray.t_min = 5.0;
    ray.t_max = 1.0;
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(!apmath_mesh_bvh_cast(cube, NULL, &hit));
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(NULL, &ray, &hit));

    // Along a face's diagonal, where its two triangles meet: no crack.
    ray = make_ray(0.5, 0.5, 5, 0, 0, -1);
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 4.0) && near3(hit.normal, 0, 0, 1));
    // And through a corner of the cube.
    ray = make_ray(2, 2, 2, -1, -1, -1);
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 1.0));
    apmath_mesh_bvh_destroy(cube);
}

static void test_mesh_ties_and_degenerates(void)
{
    current_test = "mesh_ties_and_degenerates";
    // 0: no area. 1 and 2: the same triangle twice. 3: further away.
    ApriDVec3 positions[12] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1},
                               {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {0, 0, 2}, {1, 0, 2}, {0, 1, 2}};
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    desc.positions = positions;
    desc.position_count = 12;
    apmath_mesh_bvh bvh = NULL;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
    CHECK(apmath_mesh_bvh_get_triangle_count(bvh) == 4);
    APMATH_MESH_HIT hit;
    APMATH_RAY ray = make_ray(0.25, 0.25, 0, 0, 0, 1);
    CHECK(apmath_mesh_bvh_cast(bvh, &ray, &hit));
    CHECK(hit.triangle == 1 && near(hit.t, 1.0) && hit.back_face);
    // From the other end, the far one is nearest; then the tie again.
    ray = make_ray(0.25, 0.25, 5, 0, 0, -1);
    CHECK(apmath_mesh_bvh_cast(bvh, &ray, &hit));
    CHECK(hit.triangle == 3 && near(hit.t, 3.0) && !hit.back_face);
    ray.t_min = 3.5;
    CHECK(apmath_mesh_bvh_cast(bvh, &ray, &hit));
    CHECK(hit.triangle == 1 && near(hit.t, 4.0));
    // The one with no area is never hit, even along it.
    ray = make_ray(0.5, 0, -1, 0, 0, 1);
    ray.t_max = 1.5;
    CHECK(!apmath_mesh_bvh_cast(bvh, &ray, &hit));
    apmath_mesh_bvh_destroy(bvh);
}

// Moller-Trumbore, written apart from the library's. True with [out_t] if
// the ray hits the triangle at or after t = 0.
static bool brute_triangle(const ApriDVec3 *p, const APMATH_RAY *ray, double *out_t)
{
    double e1[3] = {p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
    double e2[3] = {p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
    double d[3] = {ray->direction.x, ray->direction.y, ray->direction.z};
    double s[3] = {ray->origin.x - p[0].x, ray->origin.y - p[0].y, ray->origin.z - p[0].z};
    double h[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    double det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
    if (det == 0.0)
        return false;
    double u = (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]) / det;
    double q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
    if (u < 0.0 || v < 0.0 || u + v > 1.0)
        return false;
    double t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
    if (t < 0.0)
        return false;
    *out_t = t;
    return true;
}

static APMATH_RAY random_ray(double reach)
{
    APMATH_RAY ray = make_ray(random_range(-reach, reach), random_range(-reach, reach), random_range(-reach, reach),
                              random_range(-1, 1), random_range(-1, 1), random_range(-1, 1));
    if (ray.direction.x == 0.0 && ray.direction.y == 0.0 && ray.direction.z == 0.0)
        ray.direction.x = 1.0;
    return ray;
}

// A big mesh agrees with testing every triangle, whatever shape its
// triangles are in: scattered, or all in one spot.
static void test_mesh_against_brute_force(void)
{
    current_test = "mesh_against_brute_force";
    enum
    {
        TRIANGLES = 3000,
        RAYS = 3000
    };
    ApriDVec3 *positions = (ApriDVec3 *)malloc(sizeof(ApriDVec3) * TRIANGLES * 3);
    for (int shape = 0; shape < 2; shape++)
    {
        for (int i = 0; i < TRIANGLES; i++)
        {
            // Scattered small triangles, then large ones sharing a centre.
            double centre = shape == 0 ? 10.0 : 0.0, size = shape == 0 ? 0.6 : 8.0;
            ApriDVec3 c = vec3(random_range(-centre, centre), random_range(-centre, centre),
                               random_range(-centre, centre));
            for (int corner = 0; corner < 3; corner++)
                positions[i * 3 + corner] = vec3(c.x + random_range(-size, size), c.y + random_range(-size, size),
                                                 c.z + random_range(-size, size));
        }
        APMATH_MESH_BVH_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
        desc.positions = positions;
        desc.position_count = TRIANGLES * 3;
        apmath_mesh_bvh bvh = NULL;
        CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
        int hits = 0, mismatches = 0;
        for (int i = 0; i < RAYS; i++)
        {
            APMATH_RAY ray = random_ray(12.0);
            bool expected = false;
            double expected_t = 0.0;
            uint32_t expected_triangle = 0;
            for (uint32_t triangle = 0; triangle < TRIANGLES; triangle++)
            {
                double t;
                if (brute_triangle(positions + triangle * 3, &ray, &t) && (!expected || t < expected_t))
                {
                    expected = true;
                    expected_t = t;
                    expected_triangle = triangle;
                }
            }
            APMATH_MESH_HIT hit;
            bool found = apmath_mesh_bvh_cast(bvh, &ray, &hit);
            if (found != expected || (found && (hit.triangle != expected_triangle || !near(hit.t, expected_t))))
                mismatches++;
            hits += found;
        }
        CHECK(mismatches == 0);
        CHECK(hits > RAYS / 20); // the rays do hit things
        apmath_mesh_bvh_destroy(bvh);
    }
    free(positions);
}

// Column-major: scale, then translate.
static ApriDMat4 placement(double sx, double sy, double sz, double tx, double ty, double tz)
{
    ApriDMat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = sx;
    m.m[5] = sy;
    m.m[10] = sz;
    m.m[12] = tx;
    m.m[13] = ty;
    m.m[14] = tz;
    m.m[15] = 1.0;
    return m;
}

static apmath_instance_bvh make_instances(const APMATH_BVH_INSTANCE *instances, uint32_t count)
{
    APMATH_INSTANCE_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_INSTANCE_BVH_DESC;
    desc.instances = instances;
    desc.instance_count = count;
    apmath_instance_bvh bvh = NULL;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_OK);
    return bvh;
}

static void test_instance_arguments(void)
{
    current_test = "instance_arguments";
    apmath_mesh_bvh cube = make_cube();
    APMATH_INSTANCE_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_INSTANCE_BVH_DESC;
    apmath_instance_bvh bvh = (apmath_instance_bvh)1;
    CHECK(apmath_instance_bvh_create(NULL, &bvh) == APRESULT_INVALID_ARGUMENT);
    CHECK(bvh == NULL);
    CHECK(apmath_instance_bvh_create(&desc, NULL) == APRESULT_INVALID_ARGUMENT);

    // No instances: valid, never hit.
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_OK);
    CHECK(apmath_instance_bvh_get_instance_count(bvh) == 0);
    APMATH_AABB bounds = apmath_instance_bvh_get_bounds(bvh);
    CHECK(bounds.lo.x > bounds.hi.x);
    APMATH_RAY ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(!apmath_instance_bvh_cast(bvh, &ray, NULL, NULL));
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, NULL, 0) == 0);
    apmath_instance_bvh_destroy(bvh);
    apmath_instance_bvh_destroy(NULL);

    // Transforms that can't place a mesh.
    APMATH_BVH_INSTANCE instance;
    memset(&instance, 0, sizeof(instance));
    instance.mesh = cube;
    desc.instances = &instance;
    desc.instance_count = 1;
    ApriDMat4 flat = placement(1, 0, 1, 0, 0, 0);
    instance.transform = &flat;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    ApriDMat4 projective = placement(1, 1, 1, 0, 0, 0);
    projective.m[3] = 0.5;
    instance.transform = &projective;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    ApriDMat4 not_finite = placement(1, 1, 1, INFINITY, 0, 0);
    instance.transform = &not_finite;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    CHECK(bvh == NULL);
    desc.instances = NULL;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    apmath_mesh_bvh_destroy(cube);
}

static uint32_t filter_calls[8];
static bool filter_skip_one(void *user, uint32_t instance, uint64_t id)
{
    filter_calls[instance]++;
    return id != *(const uint64_t *)user;
}

static void test_instances(void)
{
    current_test = "instances";
    apmath_mesh_bvh cube = make_cube();
    // Four cubes in a row along X at 0, 3, 6 and 9, and one with no mesh.
    ApriDMat4 at3 = placement(1, 1, 1, 3, 0, 0), at6 = placement(1, 1, 1, 6, 0, 0), at9 = placement(1, 1, 1, 9, 0, 0);
    APMATH_BVH_INSTANCE instances[5];
    memset(instances, 0, sizeof(instances));
    const ApriDMat4 *transforms[5] = {NULL, &at3, &at6, &at9, &at3};
    for (int i = 0; i < 5; i++)
    {
        instances[i].mesh = i == 4 ? NULL : cube;
        instances[i].transform = transforms[i];
        instances[i].id = 100 + (uint64_t)i;
        instances[i].mask = 1u << i;
    }
    instances[2].mask = 0; // only casts with no desc hit it
    apmath_instance_bvh bvh = make_instances(instances, 5);
    CHECK(apmath_instance_bvh_get_instance_count(bvh) == 5);
    APMATH_AABB bounds = apmath_instance_bvh_get_bounds(bvh);
    CHECK(near3(bounds.lo, 0, 0, 0) && near3(bounds.hi, 10, 1, 1));

    // From each end, the nearest.
    APMATH_INSTANCE_HIT hit;
    APMATH_RAY ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(hit.instance == 0 && hit.id == 100 && near(hit.t, 2.0) && !hit.back_face);
    CHECK(near3(hit.point, 0, 0.25, 0.5) && near3(hit.normal, -1, 0, 0));
    ray = make_ray(20, 0.25, 0.5, -1, 0, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(hit.instance == 3 && hit.id == 103 && near(hit.t, 10.0) && near3(hit.normal, 1, 0, 0));
    CHECK(near3(hit.point, 10, 0.25, 0.5));
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, NULL));
    ray = make_ray(-2, 5, 0.5, 1, 0, 0);
    CHECK(!apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));

    // All of them, in order, one hit each.
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    APMATH_INSTANCE_HIT hits[8];
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, NULL, 0) == 4);
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 8) == 4);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(hits[i].instance == i && near(hits[i].t, 2.0 + 3.0 * i) && near3(hits[i].normal, -1, 0, 0));
    // A buffer too small keeps the nearest.
    memset(hits, 0, sizeof(hits));
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 2) == 4);
    CHECK(hits[0].instance == 0 && hits[1].instance == 1 && hits[2].t == 0.0);
    ray = make_ray(20, 0.25, 0.5, -1, 0, 0);
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 2) == 4);
    CHECK(hits[0].instance == 3 && hits[1].instance == 2);
    // The range applies to each.
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    ray.t_min = 2.5;
    ray.t_max = 8.5;
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 8) == 3);
    CHECK(hits[0].instance == 0 && hits[0].back_face && near(hits[0].t, 3.0));
    CHECK(hits[1].instance == 1 && near(hits[1].t, 5.0) && hits[2].instance == 2 && near(hits[2].t, 8.0));

    // Masks: only instances sharing a bit, and never a maskless one.
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    APMATH_CAST_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_CAST_DESC;
    desc.mask = 1u << 3;
    CHECK(apmath_instance_bvh_cast(bvh, &ray, &desc, &hit));
    CHECK(hit.instance == 3);
    desc.mask = UINT32_MAX;
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, &desc, hits, 8) == 3);
    CHECK(hits[0].instance == 0 && hits[1].instance == 1 && hits[2].instance == 3);
    desc.mask = 0;
    CHECK(!apmath_instance_bvh_cast(bvh, &ray, &desc, &hit));
    desc.base.type = APRI_STRUCT_TYPE_UNKNOWN; /* a wrong base hits nothing */
    desc.mask = UINT32_MAX;
    CHECK(!apmath_instance_bvh_cast(bvh, &ray, &desc, &hit));

    // The filter: passed through as if it weren't there, asked once each.
    uint64_t skipped = 100;
    desc.base.type = APRI_STRUCT_TYPE_APMATH_CAST_DESC;
    desc.filter = filter_skip_one;
    desc.user = &skipped;
    memset(filter_calls, 0, sizeof(filter_calls));
    CHECK(apmath_instance_bvh_cast(bvh, &ray, &desc, &hit));
    CHECK(hit.instance == 1 && near(hit.t, 5.0));
    CHECK(filter_calls[0] == 1 && filter_calls[1] == 1 && filter_calls[2] == 0 && filter_calls[4] == 0);
    memset(filter_calls, 0, sizeof(filter_calls));
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, &desc, hits, 8) == 2);
    CHECK(hits[0].instance == 1 && hits[1].instance == 3);
    CHECK(filter_calls[0] == 1 && filter_calls[1] == 1 && filter_calls[2] == 0 && filter_calls[3] == 1);

    CHECK(!apmath_instance_bvh_cast(NULL, &ray, NULL, &hit));
    CHECK(!apmath_instance_bvh_cast(bvh, NULL, NULL, &hit));
    CHECK(apmath_instance_bvh_cast_all(NULL, &ray, NULL, hits, 8) == 0);
    apmath_instance_bvh_destroy(bvh);

    // Two instances in the same place: the earlier one.
    APMATH_BVH_INSTANCE same[2];
    memset(same, 0, sizeof(same));
    same[0].mesh = same[1].mesh = cube;
    same[0].transform = same[1].transform = &at3;
    bvh = make_instances(same, 2);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(hit.instance == 0 && near(hit.t, 5.0));
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 8) == 2);
    CHECK(hits[0].instance == 0 && hits[1].instance == 1);
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(cube);
}

static void test_instance_transforms(void)
{
    current_test = "instance_transforms";
    apmath_mesh_bvh cube = make_cube();
    APMATH_BVH_INSTANCE instance;
    memset(&instance, 0, sizeof(instance));
    instance.mesh = cube;
    APMATH_INSTANCE_HIT hit;

    // Stretched 4 along X and moved: t stays the ray's own.
    ApriDMat4 stretched = placement(4, 1, 2, 10, 20, 30);
    instance.transform = &stretched;
    apmath_instance_bvh bvh = make_instances(&instance, 1);
    APMATH_AABB bounds = apmath_instance_bvh_get_bounds(bvh);
    CHECK(near3(bounds.lo, 10, 20, 30) && near3(bounds.hi, 14, 21, 32));
    APMATH_RAY ray = make_ray(12, 20.5, 40, 0, 0, -2);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(near(hit.t, 4.0) && near3(hit.point, 12, 20.5, 32) && near3(hit.normal, 0, 0, 1) && !hit.back_face);
    apmath_instance_bvh_destroy(bvh);

    // Mirrored in X: the cube's outside is still its outside.
    ApriDMat4 mirrored = placement(-1, 1, 1, 0, 0, 0);
    instance.transform = &mirrored;
    bvh = make_instances(&instance, 1);
    ray = make_ray(-5, 0.25, 0.5, 1, 0, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(near(hit.t, 4.0) && near3(hit.normal, -1, 0, 0) && !hit.back_face);
    ray = make_ray(-0.5, 0.25, 0.5, 1, 0, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(near(hit.t, 0.5) && near3(hit.normal, 1, 0, 0) && hit.back_face);
    apmath_instance_bvh_destroy(bvh);

    // Turned 45 degrees about Z and sheared: a face's normal follows it.
    double c = sqrt(0.5);
    ApriDMat4 turned = placement(1, 1, 1, 0, 0, 0);
    turned.m[0] = c;
    turned.m[1] = c;
    turned.m[4] = -c;
    turned.m[5] = c;
    instance.transform = &turned;
    bvh = make_instances(&instance, 1);
    // The cube's -Y face now faces (c, -c, 0), from the origin out along (c, c, 0).
    ray = make_ray(1, -1, 0.5, -1, 1, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(near3(hit.normal, c, -c, 0) && !hit.back_face);
    CHECK(near(hit.point.x, hit.point.y) && near(hit.point.z, 0.5));
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(cube);
}

// Many placed cubes agree with casting each one on its own.
static void test_instances_against_brute_force(void)
{
    current_test = "instances_against_brute_force";
    enum
    {
        INSTANCES = 1500,
        RAYS = 2000
    };
    apmath_mesh_bvh cube = make_cube();
    ApriDMat4 *transforms = (ApriDMat4 *)malloc(sizeof(ApriDMat4) * INSTANCES);
    APMATH_BVH_INSTANCE *instances = (APMATH_BVH_INSTANCE *)calloc(INSTANCES, sizeof(APMATH_BVH_INSTANCE));
    apmath_instance_bvh *singles = (apmath_instance_bvh *)malloc(sizeof(apmath_instance_bvh) * INSTANCES);
    for (int i = 0; i < INSTANCES; i++)
    {
        double angle = random_range(0, 6.283185307179586), c = cos(angle), s = sin(angle);
        transforms[i] = placement(1, 1, random_range(0.5, 3), random_range(-40, 40), random_range(-40, 40),
                                  random_range(-40, 40));
        double scale = random_range(0.5, 3);
        transforms[i].m[0] = c * scale;
        transforms[i].m[1] = s * scale;
        transforms[i].m[4] = -s * scale;
        transforms[i].m[5] = c * scale;
        instances[i].mesh = cube;
        instances[i].transform = &transforms[i];
        instances[i].id = (uint64_t)i * 7;
        singles[i] = make_instances(&instances[i], 1);
    }
    apmath_instance_bvh bvh = make_instances(instances, INSTANCES);
    APMATH_INSTANCE_HIT *all = (APMATH_INSTANCE_HIT *)malloc(sizeof(APMATH_INSTANCE_HIT) * INSTANCES);
    int hits = 0, mismatches = 0, all_mismatches = 0;
    for (int i = 0; i < RAYS; i++)
    {
        APMATH_RAY ray = random_ray(45.0);
        bool expected = false;
        uint32_t expected_count = 0;
        APMATH_INSTANCE_HIT nearest;
        memset(&nearest, 0, sizeof(nearest));
        uint32_t nearest_instance = 0;
        for (uint32_t instance = 0; instance < INSTANCES; instance++)
        {
            APMATH_INSTANCE_HIT single;
            if (!apmath_instance_bvh_cast(singles[instance], &ray, NULL, &single))
                continue;
            expected_count++;
            if (!expected || single.t < nearest.t)
            {
                expected = true;
                nearest = single;
                nearest_instance = instance;
            }
        }
        APMATH_INSTANCE_HIT hit;
        bool found = apmath_instance_bvh_cast(bvh, &ray, NULL, &hit);
        if (found != expected || (found && (hit.instance != nearest_instance || hit.id != (uint64_t)nearest_instance * 7 ||
                                            hit.triangle != nearest.triangle || !near(hit.t, nearest.t))))
            mismatches++;
        hits += found;
        uint32_t count = apmath_instance_bvh_cast_all(bvh, &ray, NULL, all, INSTANCES);
        if (count != expected_count || (count && all[0].instance != nearest_instance))
            all_mismatches++;
        for (uint32_t k = 1; k < count; k++)
            if (all[k - 1].t > all[k].t)
                all_mismatches++;
    }
    CHECK(mismatches == 0);
    CHECK(all_mismatches == 0);
    CHECK(hits > RAYS / 20);
    for (int i = 0; i < INSTANCES; i++)
        apmath_instance_bvh_destroy(singles[i]);
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(cube);
    free(all);
    free(singles);
    free(instances);
    free(transforms);
}

// ---- Volumes ------------------------------------------------------------------

static APMATH_PLANE make_plane(double nx, double ny, double nz, double d)
{
    APMATH_PLANE plane;
    plane.normal = vec3(nx, ny, nz);
    plane.d = d;
    return plane;
}

// Six planes holding the box from [lo] to [hi], into [out].
static void box_planes(ApriDVec3 lo, ApriDVec3 hi, APMATH_PLANE *out)
{
    out[0] = make_plane(1, 0, 0, -lo.x);
    out[1] = make_plane(-1, 0, 0, hi.x);
    out[2] = make_plane(0, 1, 0, -lo.y);
    out[3] = make_plane(0, -1, 0, hi.y);
    out[4] = make_plane(0, 0, 1, -lo.z);
    out[5] = make_plane(0, 0, -1, hi.z);
}

static APMATH_VOLUME make_volume(const APMATH_PLANE *planes, uint32_t plane_count, const APMATH_PLANE *clip_planes,
                                 uint32_t clip_plane_count)
{
    APMATH_VOLUME volume;
    volume.planes = planes;
    volume.plane_count = plane_count;
    volume.clip_planes = clip_planes;
    volume.clip_plane_count = clip_plane_count;
    return volume;
}

static ApriDMat4 identity_matrix(void)
{
    ApriDMat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0;
    return m;
}

static void test_volume_box(void)
{
    current_test = "volume_box";
    APMATH_PLANE planes[6];
    box_planes(vec3(0, 0, 0), vec3(10, 10, 10), planes);
    APMATH_VOLUME volume = make_volume(planes, 6, NULL, 0);

    APMATH_AABB inside = {{1, 1, 1}, {2, 2, 2}};
    APMATH_AABB outside = {{11, 1, 1}, {12, 2, 2}};
    APMATH_AABB across = {{9, 1, 1}, {11, 2, 2}};
    APMATH_AABB touching_inside = {{0, 0, 0}, {10, 10, 10}};
    APMATH_AABB touching_outside = {{10, 1, 1}, {11, 2, 2}};
    CHECK(apmath_aabb_test_volume(&inside, NULL, &volume) == APMATH_OVERLAP_WHOLE);
    CHECK(apmath_aabb_test_volume(&outside, NULL, &volume) == APMATH_OVERLAP_NONE);
    CHECK(apmath_aabb_test_volume(&across, NULL, &volume) == APMATH_OVERLAP_PARTIAL);
    CHECK(apmath_aabb_test_volume(&touching_inside, NULL, &volume) == APMATH_OVERLAP_WHOLE);
    // Its face on the plane counts as inside, the rest isn't.
    CHECK(apmath_aabb_test_volume(&touching_outside, NULL, &volume) == APMATH_OVERLAP_PARTIAL);

    // Clip planes: z <= 5 kept.
    APMATH_PLANE clip = make_plane(0, 0, -1, 5);
    APMATH_VOLUME clipped = make_volume(planes, 6, &clip, 1);
    APMATH_AABB above = {{1, 1, 6}, {2, 2, 7}};
    CHECK(apmath_aabb_test_volume(&above, NULL, &clipped) == APMATH_OVERLAP_NONE);
    CHECK(apmath_aabb_test_volume(&inside, NULL, &clipped) == APMATH_OVERLAP_WHOLE);
    // Straddling only the clip plane: some of it is cut, so not WHOLE.
    APMATH_AABB across_clip = {{1, 1, 4}, {2, 2, 6}};
    CHECK(apmath_aabb_test_volume(&across_clip, NULL, &clipped) == APMATH_OVERLAP_PARTIAL);

    // A transform: the box outside, moved by 20 in x, rotated back in.
    ApriDMat4 move = identity_matrix();
    move.m[12] = -10.0;
    CHECK(apmath_aabb_test_volume(&outside, &move, &volume) == APMATH_OVERLAP_WHOLE);
    // A box along x, turned a quarter about z, leaves the thin volume x in [0, 1].
    APMATH_PLANE slab[2] = {make_plane(1, 0, 0, 0), make_plane(-1, 0, 0, 1)};
    APMATH_VOLUME thin = make_volume(slab, 2, NULL, 0);
    APMATH_AABB along_y = {{0.25, 0.5, 0}, {0.75, 5, 1}}; // turned, x from -5 to -0.5
    CHECK(apmath_aabb_test_volume(&along_y, NULL, &thin) == APMATH_OVERLAP_WHOLE);
    ApriDMat4 turn = identity_matrix();
    turn.m[0] = 0.0, turn.m[1] = 1.0, turn.m[4] = -1.0, turn.m[5] = 0.0; // (x, y) -> (-y, x)
    CHECK(apmath_aabb_test_volume(&along_y, &turn, &thin) == APMATH_OVERLAP_NONE);

    // No planes: all of space.
    APMATH_VOLUME everything = make_volume(NULL, 0, NULL, 0);
    CHECK(apmath_aabb_test_volume(&inside, NULL, &everything) == APMATH_OVERLAP_WHOLE);
    // An empty box.
    APMATH_AABB empty = {{2, 2, 2}, {1, 1, 1}};
    CHECK(apmath_aabb_test_volume(&empty, NULL, &everything) == APMATH_OVERLAP_NONE);
}

static void test_volume_arguments(void)
{
    current_test = "volume_arguments";
    apmath_mesh_bvh cube = make_cube();
    APMATH_PLANE planes[6];
    box_planes(vec3(-1, -1, -1), vec3(2, 2, 2), planes);
    APMATH_VOLUME volume = make_volume(planes, 6, NULL, 0);
    APMATH_AABB box = {{0, 0, 0}, {1, 1, 1}};
    // Each valid call first, so a refusal below is the argument's.
    CHECK(apmath_aabb_test_volume(&box, NULL, &volume) == APMATH_OVERLAP_WHOLE);
    CHECK(apmath_mesh_bvh_overlaps_volume(cube, NULL, &volume));
    CHECK(apmath_mesh_bvh_within_volume(cube, NULL, &volume));

    // NULLs.
    CHECK(apmath_aabb_test_volume(NULL, NULL, &volume) == APMATH_OVERLAP_NONE);
    CHECK(apmath_aabb_test_volume(&box, NULL, NULL) == APMATH_OVERLAP_NONE);
    CHECK(!apmath_mesh_bvh_overlaps_volume(NULL, NULL, &volume));
    CHECK(!apmath_mesh_bvh_within_volume(NULL, NULL, &volume));
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, NULL));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, NULL));

    // A box that isn't finite.
    APMATH_AABB nan_box = box;
    nan_box.hi.y = NAN;
    CHECK(apmath_aabb_test_volume(&nan_box, NULL, &volume) == APMATH_OVERLAP_NONE);

    // Transforms: not finite, last row not 0 0 0 1, not invertible.
    ApriDMat4 bad = identity_matrix();
    bad.m[13] = INFINITY;
    CHECK(apmath_aabb_test_volume(&box, &bad, &volume) == APMATH_OVERLAP_NONE);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, &bad, &volume));
    CHECK(!apmath_mesh_bvh_within_volume(cube, &bad, &volume));
    bad = identity_matrix();
    bad.m[3] = 0.5;
    CHECK(apmath_aabb_test_volume(&box, &bad, &volume) == APMATH_OVERLAP_NONE);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, &bad, &volume));
    CHECK(!apmath_mesh_bvh_within_volume(cube, &bad, &volume));
    bad = identity_matrix();
    bad.m[10] = 0.0; // flattens z
    CHECK(apmath_aabb_test_volume(&box, &bad, &volume) == APMATH_OVERLAP_NONE);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, &bad, &volume));
    CHECK(!apmath_mesh_bvh_within_volume(cube, &bad, &volume));

    // Planes: NULL with a count, a zero normal, a NaN normal, a NaN d - in
    // the volume's planes and in the clip planes.
    APMATH_VOLUME broken = make_volume(NULL, 1, NULL, 0);
    CHECK(apmath_aabb_test_volume(&box, NULL, &broken) == APMATH_OVERLAP_NONE);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &broken));
    broken = make_volume(planes, 6, NULL, 1);
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &broken));
    APMATH_PLANE wrong[4] = {make_plane(0, 0, 0, 1), make_plane(NAN, 0, 1, 1), make_plane(0, 0, 1, NAN),
                             make_plane(0, 0, INFINITY, 1)};
    for (int i = 0; i < 4; i++)
    {
        broken = make_volume(&wrong[i], 1, NULL, 0);
        CHECK(apmath_aabb_test_volume(&box, NULL, &broken) == APMATH_OVERLAP_NONE);
        CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &broken));
        CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &broken));
        broken = make_volume(planes, 6, &wrong[i], 1);
        CHECK(apmath_aabb_test_volume(&box, NULL, &broken) == APMATH_OVERLAP_NONE);
        CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &broken));
        CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &broken));
    }

    // At most 32 planes between the two lists.
    APMATH_PLANE many[33];
    for (int i = 0; i < 33; i++)
        many[i] = make_plane(0, 0, 1, 5.0); // z >= -5, holds the cube
    APMATH_VOLUME most = make_volume(many, 20, many + 20, 12);
    CHECK(apmath_aabb_test_volume(&box, NULL, &most) == APMATH_OVERLAP_WHOLE);
    CHECK(apmath_mesh_bvh_within_volume(cube, NULL, &most));
    APMATH_VOLUME too_many = make_volume(many, 20, many + 20, 13);
    CHECK(apmath_aabb_test_volume(&box, NULL, &too_many) == APMATH_OVERLAP_NONE);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &too_many));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &too_many));
    too_many = make_volume(many, 33, NULL, 0);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &too_many));
    apmath_mesh_bvh_destroy(cube);
}

// A frustum from an eye at (0.5, 0.5, 5) looking down -Z, its sides
// widening by [slope] per unit down (x - 0.5 >= -slope (5 - z), and so on),
// near plane z = 4, far plane z = -95.
static void make_frustum(double slope, APMATH_PLANE *out)
{
    out[0] = make_plane(1, 0, -slope, 5 * slope - 0.5);
    out[1] = make_plane(-1, 0, -slope, 5 * slope + 0.5);
    out[2] = make_plane(0, 1, -slope, 5 * slope - 0.5);
    out[3] = make_plane(0, -1, -slope, 5 * slope + 0.5);
    out[4] = make_plane(0, 0, -1, 4.0);
    out[5] = make_plane(0, 0, 1, 95.0);
}

static void test_volume_cube(void)
{
    current_test = "volume_cube";
    apmath_mesh_bvh cube = make_cube();
    APMATH_PLANE planes[6];

    box_planes(vec3(-1, -1, -1), vec3(2, 2, 2), planes);
    APMATH_VOLUME bigger = make_volume(planes, 6, NULL, 0);
    CHECK(apmath_mesh_bvh_overlaps_volume(cube, NULL, &bigger));
    CHECK(apmath_mesh_bvh_within_volume(cube, NULL, &bigger));

    APMATH_PLANE across_planes[6];
    box_planes(vec3(0.5, -1, -1), vec3(2, 2, 2), across_planes);
    APMATH_VOLUME across = make_volume(across_planes, 6, NULL, 0);
    CHECK(apmath_mesh_bvh_overlaps_volume(cube, NULL, &across));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &across));

    APMATH_PLANE outside_planes[6];
    box_planes(vec3(3, 3, 3), vec3(4, 4, 4), outside_planes);
    APMATH_VOLUME outside = make_volume(outside_planes, 6, NULL, 0);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &outside));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &outside));

    // Touching the cube's +X face from outside.
    APMATH_PLANE touching_planes[6];
    box_planes(vec3(1, 0.25, 0.25), vec3(2, 0.75, 0.75), touching_planes);
    APMATH_VOLUME touching = make_volume(touching_planes, 6, NULL, 0);
    CHECK(apmath_mesh_bvh_overlaps_volume(cube, NULL, &touching));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &touching));

    // A volume wholly inside the cube touches none of its triangles: the
    // surface is what's tested.
    APMATH_PLANE inner_planes[6];
    box_planes(vec3(0.25, 0.25, 0.25), vec3(0.75, 0.75, 0.75), inner_planes);
    APMATH_VOLUME inner = make_volume(inner_planes, 6, NULL, 0);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &inner));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &inner));

    // Clip planes keep z <= 0.5: the cube's lower half.
    APMATH_PLANE keep_lower = make_plane(0, 0, -1, 0.5);
    APMATH_PLANE lower_planes[6];
    box_planes(vec3(-1, -1, -1), vec3(2, 2, 0.6), lower_planes);
    APMATH_VOLUME lower = make_volume(lower_planes, 6, &keep_lower, 1);
    CHECK(apmath_mesh_bvh_within_volume(cube, NULL, &lower));
    // Without the clip, the upper half sticks out.
    APMATH_VOLUME lower_unclipped = make_volume(lower_planes, 6, NULL, 0);
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &lower_unclipped));
    // A volume over only the part clipped away finds nothing.
    APMATH_PLANE upper_planes[6];
    box_planes(vec3(-1, -1, 0.7), vec3(2, 2, 2), upper_planes);
    APMATH_VOLUME upper = make_volume(upper_planes, 6, &keep_lower, 1);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &upper));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &upper));
    // A clip that removes everything: nothing overlaps, nothing is within.
    APMATH_PLANE keep_none = make_plane(0, 0, 1, -5.0); // z >= 5
    APMATH_VOLUME none = make_volume(planes, 6, &keep_none, 1);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &none));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &none));

    // A frustum from an eye at (0.5, 0.5, 5) looking down -Z, its sides
    // widening by [slope] per unit down, near plane z = 4, far z = -95.
    APMATH_PLANE frustum[6];
    make_frustum(0.05, frustum); // 0.4 across at the cube's top, 0.5 at its base
    APMATH_VOLUME view = make_volume(frustum, 6, NULL, 0);
    CHECK(apmath_mesh_bvh_overlaps_volume(cube, NULL, &view));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &view));
    make_frustum(0.5, frustum); // 4 across at the top: holds it all
    CHECK(apmath_mesh_bvh_within_volume(cube, NULL, &view));
    // The near plane moved into the cube: its top is outside the volume.
    frustum[4] = make_plane(0, 0, -1, 0.5);
    CHECK(apmath_mesh_bvh_overlaps_volume(cube, NULL, &view));
    CHECK(!apmath_mesh_bvh_within_volume(cube, NULL, &view));

    // Transforms: the cube moved from (10, 0, 0) into the bigger box; and
    // mirrored in x about 0.5.
    APMATH_PLANE far_planes[6];
    box_planes(vec3(9, -1, -1), vec3(12, 2, 2), far_planes);
    APMATH_VOLUME far_box = make_volume(far_planes, 6, NULL, 0);
    CHECK(!apmath_mesh_bvh_overlaps_volume(cube, NULL, &far_box));
    ApriDMat4 move = identity_matrix();
    move.m[12] = 10.0;
    CHECK(apmath_mesh_bvh_within_volume(cube, &move, &far_box));
    ApriDMat4 mirror = identity_matrix();
    mirror.m[0] = -1.0;
    mirror.m[12] = 1.0; // x -> 1 - x: the cube onto itself
    CHECK(apmath_mesh_bvh_within_volume(cube, &mirror, &bigger));
    CHECK(apmath_mesh_bvh_overlaps_volume(cube, &mirror, &across));
    CHECK(!apmath_mesh_bvh_within_volume(cube, &mirror, &across));
    apmath_mesh_bvh_destroy(cube);
}

// The polygon of [count] points clipped by [plane]'s inside into [out]; its
// new count. Points on the plane are inside.
static int clip_polygon(const ApriDVec3 *points, int count, APMATH_PLANE plane, ApriDVec3 *out)
{
    int kept = 0;
    for (int i = 0; i < count; i++)
    {
        ApriDVec3 a = points[i], b = points[(i + 1) % count];
        double da = plane.normal.x * a.x + plane.normal.y * a.y + plane.normal.z * a.z + plane.d;
        double db = plane.normal.x * b.x + plane.normal.y * b.y + plane.normal.z * b.z + plane.d;
        if (da >= 0.0)
            out[kept++] = a;
        if ((da >= 0.0) != (db >= 0.0))
        {
            double t = da / (da - db);
            out[kept++] = vec3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
        }
    }
    return kept;
}

static void test_volume_against_brute_force(void)
{
    current_test = "volume_against_brute_force";
    enum
    {
        TRIANGLES = 2000,
        VOLUMES = 400,
        CLIPPED_POINTS = 3 + 7 + 2 // 7 planes and 2 clip planes add a point each at most
    };
    ApriDVec3 *positions = (ApriDVec3 *)malloc(sizeof(ApriDVec3) * TRIANGLES * 3);
    for (int i = 0; i < TRIANGLES; i++)
    {
        ApriDVec3 c = vec3(random_range(-10, 10), random_range(-10, 10), random_range(-10, 10));
        for (int corner = 0; corner < 3; corner++)
            positions[i * 3 + corner] =
                vec3(c.x + random_range(-0.5, 0.5), c.y + random_range(-0.5, 0.5), c.z + random_range(-0.5, 0.5));
    }
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    desc.positions = positions;
    desc.position_count = TRIANGLES * 3;
    apmath_mesh_bvh bvh = NULL;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);

    int overlaps_found = 0, within_found = 0;
    for (int v = 0; v < VOLUMES; v++)
    {
        // A box anywhere, tilted by a seventh plane; half of them with a
        // slab of clip planes.
        APMATH_PLANE planes[7], clips[2];
        // A quarter of them about the size of the whole mesh, so some hold
        // all of it.
        bool large = v % 4 == 3;
        double size = large ? random_range(21.5, 24) : random_range(1, 25);
        ApriDVec3 lo = large ? vec3(random_range(-12, -10.5), random_range(-12, -10.5), random_range(-12, -10.5))
                             : vec3(random_range(-15, 5), random_range(-15, 5), random_range(-15, 5));
        box_planes(lo, vec3(lo.x + size, lo.y + size, lo.z + size), planes);
        planes[6] = make_plane(random_range(-1, 1), random_range(-1, 1), 1.0, random_range(-5, large ? 40 : 15));
        uint32_t clip_count = v % 2 ? 2 : 0;
        double z = random_range(-10, 8);
        clips[0] = make_plane(0, 0, 1, -z);
        clips[1] = make_plane(0, 0, -1, z + random_range(0.5, 6));
        APMATH_VOLUME volume = make_volume(planes, 7, clips, clip_count);

        bool overlaps = false, any_kept = false, all_inside = true;
        for (int t = 0; t < TRIANGLES; t++)
        {
            ApriDVec3 a[CLIPPED_POINTS], b[CLIPPED_POINTS];
            int count = 3;
            memcpy(a, positions + t * 3, sizeof(ApriDVec3) * 3);
            for (uint32_t c = 0; c < clip_count && count; c++)
            {
                count = clip_polygon(a, count, clips[c], b);
                memcpy(a, b, sizeof(ApriDVec3) * (size_t)count);
            }
            if (!count)
                continue;
            any_kept = true;
            for (int p = 0; p < count; p++)
                for (int k = 0; k < 7; k++)
                    if (planes[k].normal.x * a[p].x + planes[k].normal.y * a[p].y + planes[k].normal.z * a[p].z +
                            planes[k].d <
                        0.0)
                        all_inside = false;
            for (int k = 0; k < 7 && count; k++)
            {
                count = clip_polygon(a, count, planes[k], b);
                memcpy(a, b, sizeof(ApriDVec3) * (size_t)count);
            }
            if (count)
                overlaps = true;
        }
        bool within = any_kept && all_inside;
        overlaps_found += overlaps;
        within_found += within;
        CHECK(apmath_mesh_bvh_overlaps_volume(bvh, NULL, &volume) == overlaps);
        CHECK(apmath_mesh_bvh_within_volume(bvh, NULL, &volume) == within);
    }
    // The volumes are a mix: some find triangles and some don't.
    CHECK(overlaps_found > 0 && overlaps_found < VOLUMES);
    CHECK(within_found > 0);
    printf("volume_against_brute_force: %d of %d overlap, %d within\n", overlaps_found, (int)VOLUMES, within_found);
    apmath_mesh_bvh_destroy(bvh);
    free(positions);
}

int main(void)
{
    test_ray_clip();
    test_mesh_arguments();
    test_mesh_cube();
    test_mesh_ties_and_degenerates();
    test_mesh_against_brute_force();
    test_instance_arguments();
    test_instances();
    test_instance_transforms();
    test_instances_against_brute_force();
    test_volume_box();
    test_volume_arguments();
    test_volume_cube();
    test_volume_against_brute_force();
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("apmathbvh_tests: all passed\n");
    return 0;
}
