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

// ---- Shapes, sweeps and the BVHs changed in place ------------------------
//
// The brute force below is slow and obvious: exact distances (a point's
// closest point on a triangle, segments' closest points) for spheres and
// capsules, separating axes for boxes, and a sweep's first touch by a march
// of those along the motion, bisected. Every answer of the BVHs is compared
// with it.

// Pending while the calls aren't built: set_instances and the box BVH's
// create then return APRESULT_NOT_IMPLEMENTED.
static bool shapes_pending = false;

typedef struct
{
    double x, y, z;
} v3;

static v3 V(double x, double y, double z)
{
    v3 r = {x, y, z};
    return r;
}
static v3 vadd(v3 a, v3 b)
{
    return V(a.x + b.x, a.y + b.y, a.z + b.z);
}
static v3 vsub(v3 a, v3 b)
{
    return V(a.x - b.x, a.y - b.y, a.z - b.z);
}
static v3 vmul(v3 a, double s)
{
    return V(a.x * s, a.y * s, a.z * s);
}
static double vdot(v3 a, v3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static v3 vcross(v3 a, v3 b)
{
    return V(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static double vlen(v3 a)
{
    return sqrt(vdot(a, a));
}
static v3 from_d(ApriDVec3 a)
{
    return V(a.x, a.y, a.z);
}
static ApriDVec3 to_d(v3 a)
{
    ApriDVec3 r = {a.x, a.y, a.z};
    return r;
}

// p turned by the unit quaternion q (x, y, z, w).
static v3 rotate(ApriDVec4 q, v3 p)
{
    v3 u = V(q.x, q.y, q.z);
    v3 t = vmul(vcross(u, p), 2.0);
    return vadd(vadd(p, vmul(t, q.w)), vcross(u, t));
}

static ApriDVec4 random_rotation(void)
{
    double x = random_range(-1, 1), y = random_range(-1, 1), z = random_range(-1, 1), w = random_range(-1, 1);
    double l = sqrt(x * x + y * y + z * z + w * w);
    ApriDVec4 q = {x / l, y / l, z / l, w / l};
    return q;
}

// Ericson, "Real-Time Collision Detection", 5.1.5.
static v3 closest_on_triangle(v3 p, v3 a, v3 b, v3 c)
{
    v3 ab = vsub(b, a), ac = vsub(c, a), ap = vsub(p, a);
    double d1 = vdot(ab, ap), d2 = vdot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return a;
    v3 bp = vsub(p, b);
    double d3 = vdot(ab, bp), d4 = vdot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return b;
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return vadd(a, vmul(ab, d1 / (d1 - d3)));
    v3 cp = vsub(p, c);
    double d5 = vdot(ab, cp), d6 = vdot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return c;
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return vadd(a, vmul(ac, d2 / (d2 - d6)));
    double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return vadd(b, vmul(vsub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    double denom = 1.0 / (va + vb + vc);
    return vadd(a, vadd(vmul(ab, vb * denom), vmul(ac, vc * denom)));
}

static double clamp01(double x)
{
    return x < 0 ? 0 : x > 1 ? 1 : x;
}

// Ericson 5.1.9: the distance between segments p1q1 and p2q2.
static double segment_segment(v3 p1, v3 q1, v3 p2, v3 q2)
{
    v3 d1 = vsub(q1, p1), d2 = vsub(q2, p2), r = vsub(p1, p2);
    double a = vdot(d1, d1), e = vdot(d2, d2), f = vdot(d2, r), s, t;
    if (a <= 1e-300 && e <= 1e-300)
        return vlen(r);
    if (a <= 1e-300)
    {
        s = 0;
        t = clamp01(f / e);
    }
    else
    {
        double c = vdot(d1, r);
        if (e <= 1e-300)
        {
            t = 0;
            s = clamp01(-c / a);
        }
        else
        {
            double b = vdot(d1, d2), denom = a * e - b * b;
            s = denom != 0 ? clamp01((b * f - c * e) / denom) : 0;
            t = (b * s + f) / e;
            if (t < 0)
            {
                t = 0;
                s = clamp01(-c / a);
            }
            else if (t > 1)
            {
                t = 1;
                s = clamp01((b - c) / a);
            }
        }
    }
    return vlen(vsub(vadd(p1, vmul(d1, s)), vadd(p2, vmul(d2, t))));
}

// Whether segment pq crosses triangle abc (both sides).
static bool segment_crosses(v3 p, v3 q, v3 a, v3 b, v3 c)
{
    v3 d = vsub(q, p), e1 = vsub(b, a), e2 = vsub(c, a);
    v3 h = vcross(d, e2);
    double det = vdot(e1, h);
    if (fabs(det) < 1e-300)
        return false;
    v3 s = vsub(p, a);
    double u = vdot(s, h) / det;
    v3 qq = vcross(s, e1);
    double v = vdot(d, qq) / det, t = vdot(e2, qq) / det;
    return u >= 0 && v >= 0 && u + v <= 1 && t >= 0 && t <= 1;
}

static double segment_triangle(v3 p, v3 q, v3 a, v3 b, v3 c)
{
    if (segment_crosses(p, q, a, b, c))
        return 0.0;
    double d = vlen(vsub(p, closest_on_triangle(p, a, b, c)));
    double e = vlen(vsub(q, closest_on_triangle(q, a, b, c)));
    d = e < d ? e : d;
    double edges[3] = {segment_segment(p, q, a, b), segment_segment(p, q, b, c), segment_segment(p, q, c, a)};
    for (int i = 0; i < 3; i++)
        d = edges[i] < d ? edges[i] : d;
    return d;
}

// The largest separation along any axis between two convex point sets: >
// 0 when apart (a lower bound on their distance), <= 0 when they meet.
static double separation(const v3 *a, int an, const v3 *b, int bn, const v3 *axes, int axis_count)
{
    double best = -INFINITY;
    for (int i = 0; i < axis_count; i++)
    {
        double l = vlen(axes[i]);
        if (l < 1e-9)
            continue;
        v3 n = vmul(axes[i], 1.0 / l);
        double alo = INFINITY, ahi = -INFINITY, blo = INFINITY, bhi = -INFINITY;
        for (int k = 0; k < an; k++)
        {
            double x = vdot(n, a[k]);
            alo = x < alo ? x : alo;
            ahi = x > ahi ? x : ahi;
        }
        for (int k = 0; k < bn; k++)
        {
            double x = vdot(n, b[k]);
            blo = x < blo ? x : blo;
            bhi = x > bhi ? x : bhi;
        }
        double s = blo - ahi > alo - bhi ? blo - ahi : alo - bhi;
        best = s > best ? s : best;
    }
    return best;
}

// A shape's box core: its 8 corners and its 3 axes.
static void box_corners(const APMATH_SHAPE *s, v3 center, v3 *corners, v3 *axes)
{
    v3 h = from_d(s->half_extents);
    axes[0] = rotate(s->rotation, V(1, 0, 0));
    axes[1] = rotate(s->rotation, V(0, 1, 0));
    axes[2] = rotate(s->rotation, V(0, 0, 1));
    for (int i = 0; i < 8; i++)
        corners[i] = vadd(center, vadd(vmul(axes[0], i & 1 ? h.x : -h.x),
                                       vadd(vmul(axes[1], i & 2 ? h.y : -h.y), vmul(axes[2], i & 4 ? h.z : -h.z))));
}

// The gap between a shape centered at [center] and triangle abc: > 0 apart,
// <= 0 touching or crossing. Exact for spheres and capsules (distance less
// radius); for a box the separating-axis bound, exact in its sign.
static double shape_triangle_gap(const APMATH_SHAPE *s, v3 center, v3 a, v3 b, v3 c)
{
    if (s->kind == APMATH_SHAPE_KIND_SPHERE)
        return vlen(vsub(center, closest_on_triangle(center, a, b, c))) - s->radius;
    if (s->kind == APMATH_SHAPE_KIND_CAPSULE)
    {
        v3 axis = vmul(rotate(s->rotation, V(0, 1, 0)), s->half_height);
        return segment_triangle(vsub(center, axis), vadd(center, axis), a, b, c) - s->radius;
    }
    v3 corners[8], axes[13], tri[3] = {a, b, c};
    box_corners(s, center, corners, axes);
    v3 e[3] = {vsub(b, a), vsub(c, b), vsub(a, c)};
    axes[3] = vcross(e[0], e[1]);
    int n = 4;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            axes[n++] = vcross(axes[i], e[j]);
    return separation(corners, 8, tri, 3, axes, n);
}

// The same against a solid axis-aligned box.
static double shape_box_gap(const APMATH_SHAPE *s, v3 center, APMATH_AABB box)
{
    v3 lo = from_d(box.lo), hi = from_d(box.hi);
    if (s->kind == APMATH_SHAPE_KIND_SPHERE || s->kind == APMATH_SHAPE_KIND_CAPSULE)
    {
        // The distance from a point of the segment to the box is convex
        // along it: a ternary search finds its least.
        v3 axis = s->kind == APMATH_SHAPE_KIND_CAPSULE ? vmul(rotate(s->rotation, V(0, 1, 0)), s->half_height) : V(0, 0, 0);
        v3 p = vsub(center, axis), d = vmul(axis, 2.0);
        double l = 0, r = 1, best = INFINITY;
        for (int i = 0; i < 80; i++)
        {
            double m1 = l + (r - l) / 3, m2 = r - (r - l) / 3, f[2];
            for (int k = 0; k < 2; k++)
            {
                v3 q = vadd(p, vmul(d, k ? m2 : m1));
                v3 c = V(q.x < lo.x ? lo.x : q.x > hi.x ? hi.x : q.x, q.y < lo.y ? lo.y : q.y > hi.y ? hi.y : q.y,
                         q.z < lo.z ? lo.z : q.z > hi.z ? hi.z : q.z);
                f[k] = vlen(vsub(q, c));
                best = f[k] < best ? f[k] : best;
            }
            if (f[0] < f[1])
                r = m2;
            else
                l = m1;
        }
        return best - s->radius;
    }
    v3 corners[8], axes[15], bc[8];
    box_corners(s, center, corners, axes);
    for (int i = 0; i < 8; i++)
        bc[i] = V(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
    v3 world[3] = {V(1, 0, 0), V(0, 1, 0), V(0, 0, 1)};
    int n = 3;
    for (int i = 0; i < 3; i++)
        axes[n++] = world[i];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            axes[n++] = vcross(axes[i], world[j]);
    return separation(corners, 8, bc, 8, axes, n);
}

// What a brute-force scene holds: its triangles in world space, each with
// its instance and its triangle's position in its mesh.
typedef struct
{
    v3 p[3];
    uint32_t instance, triangle;
} world_triangle;

static world_triangle *scene_triangles = NULL;
static uint32_t scene_triangle_count = 0;
// Each instance's triangles are a run of them, and the box around the run.
enum
{
    SCENE_MAX_INSTANCES = 64
};
static uint32_t scene_first[SCENE_MAX_INSTANCES], scene_count[SCENE_MAX_INSTANCES];
static v3 scene_lo[SCENE_MAX_INSTANCES], scene_hi[SCENE_MAX_INSTANCES];

static void add_scene_mesh(const ApriDVec3 *positions, const uint32_t *indices, uint32_t triangle_count,
                           const ApriDMat4 *m, uint32_t instance)
{
    scene_first[instance] = scene_triangle_count;
    scene_count[instance] = triangle_count;
    scene_lo[instance] = V(INFINITY, INFINITY, INFINITY);
    scene_hi[instance] = V(-INFINITY, -INFINITY, -INFINITY);
    for (uint32_t t = 0; t < triangle_count; t++)
    {
        world_triangle *w = &scene_triangles[scene_triangle_count++];
        w->instance = instance;
        w->triangle = t;
        for (int k = 0; k < 3; k++)
        {
            v3 p = from_d(positions[indices[t * 3 + k]]);
            w->p[k] = m ? V(m->m[0] * p.x + m->m[4] * p.y + m->m[8] * p.z + m->m[12],
                            m->m[1] * p.x + m->m[5] * p.y + m->m[9] * p.z + m->m[13],
                            m->m[2] * p.x + m->m[6] * p.y + m->m[10] * p.z + m->m[14])
                        : p;
            v3 *lo = &scene_lo[instance], *hi = &scene_hi[instance];
            *lo = V(w->p[k].x < lo->x ? w->p[k].x : lo->x, w->p[k].y < lo->y ? w->p[k].y : lo->y,
                    w->p[k].z < lo->z ? w->p[k].z : lo->z);
            *hi = V(w->p[k].x > hi->x ? w->p[k].x : hi->x, w->p[k].y > hi->y ? w->p[k].y : hi->y,
                    w->p[k].z > hi->z ? w->p[k].z : hi->z);
        }
    }
}

// The least gap between a shape at [center] and any triangle of [instance]
// (all instances for UINT32_MAX).
static double scene_gap(const APMATH_SHAPE *s, v3 center, uint32_t instance)
{
    double best = INFINITY;
    uint32_t first = instance == UINT32_MAX ? 0 : scene_first[instance];
    uint32_t end = instance == UINT32_MAX ? scene_triangle_count : first + scene_count[instance];
    for (uint32_t i = first; i < end; i++)
    {
        const world_triangle *w = &scene_triangles[i];
        double g = shape_triangle_gap(s, center, w->p[0], w->p[1], w->p[2]);
        best = g < best ? g : best;
    }
    return best;
}

// A sweep's first touch of [instance] by a march of gaps, bisected to 1e-9
// of the motion: its t, or -1 for none. [out_min_gap] is the least gap the
// march saw, so a graze can be told from a clear miss.
static double march_sweep(const APMATH_SWEEP *sweep, uint32_t instance, double *out_min_gap)
{
    enum
    {
        STEPS = 400
    };
    v3 c = from_d(sweep->shape.center), d = from_d(sweep->direction);
    double previous = 0.0, min_gap = INFINITY;
    // Nothing to march when the box around the whole sweep, grown by the
    // shape's reach, misses the instance's: the gap is at least the boxes'.
    if (instance != UINT32_MAX)
    {
        double r = sweep->shape.radius + sweep->shape.half_height + vlen(from_d(sweep->shape.half_extents)) + 1e-6;
        v3 e = vadd(c, vmul(d, sweep->t_max));
        v3 lo = V(fmin(c.x, e.x) - r, fmin(c.y, e.y) - r, fmin(c.z, e.z) - r);
        v3 hi = V(fmax(c.x, e.x) + r, fmax(c.y, e.y) + r, fmax(c.z, e.z) + r);
        if (lo.x > scene_hi[instance].x || lo.y > scene_hi[instance].y || lo.z > scene_hi[instance].z ||
            hi.x < scene_lo[instance].x || hi.y < scene_lo[instance].y || hi.z < scene_lo[instance].z)
        {
            *out_min_gap = INFINITY;
            return -1.0;
        }
    }
    for (int i = 0; i <= STEPS; i++)
    {
        double t = sweep->t_max * i / STEPS;
        double g = scene_gap(&sweep->shape, vadd(c, vmul(d, t)), instance);
        min_gap = g < min_gap ? g : min_gap;
        if (g <= 0.0)
        {
            if (i == 0)
            {
                *out_min_gap = min_gap;
                return 0.0;
            }
            double lo = previous, hi = t;
            while (hi - lo > 1e-9 * sweep->t_max)
            {
                double mid = 0.5 * (lo + hi);
                if (scene_gap(&sweep->shape, vadd(c, vmul(d, mid)), instance) <= 0.0)
                    hi = mid;
                else
                    lo = mid;
            }
            *out_min_gap = min_gap;
            return hi;
        }
        previous = t;
    }
    *out_min_gap = min_gap;
    return -1.0;
}

// A mesh of [count] random triangles in [-1, 1]^3, each 0.3 to 1.2 across.
static ApriDVec3 soup_positions[3 * 40];
static uint32_t soup_indices[3 * 40];
static apmath_mesh_bvh make_soup(uint32_t count)
{
    for (uint32_t t = 0; t < count; t++)
    {
        v3 c = V(random_range(-0.6, 0.6), random_range(-0.6, 0.6), random_range(-0.6, 0.6));
        for (int k = 0; k < 3; k++)
        {
            soup_positions[t * 3 + k] =
                to_d(vadd(c, V(random_range(-0.5, 0.5), random_range(-0.5, 0.5), random_range(-0.5, 0.5))));
            soup_indices[t * 3 + k] = t * 3 + k;
        }
    }
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    desc.positions = soup_positions;
    desc.position_count = count * 3;
    desc.indices = soup_indices;
    desc.index_count = count * 3;
    apmath_mesh_bvh bvh = NULL;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
    return bvh;
}

// A turn, a scale on each axis (one mirrored now and then) and a move.
static ApriDMat4 random_placement(double reach)
{
    ApriDVec4 q = random_rotation();
    v3 axes[3] = {rotate(q, V(1, 0, 0)), rotate(q, V(0, 1, 0)), rotate(q, V(0, 0, 1))};
    double scale[3] = {random_range(0.6, 1.8), random_range(0.6, 1.8), random_range(0.6, 1.8)};
    if (random_unit() < 0.2)
        scale[0] = -scale[0];
    ApriDMat4 m;
    memset(&m, 0, sizeof(m));
    for (int c = 0; c < 3; c++)
    {
        m.m[c * 4 + 0] = axes[c].x * scale[c];
        m.m[c * 4 + 1] = axes[c].y * scale[c];
        m.m[c * 4 + 2] = axes[c].z * scale[c];
    }
    m.m[12] = random_range(-reach, reach);
    m.m[13] = random_range(-reach, reach);
    m.m[14] = random_range(-reach, reach);
    m.m[15] = 1.0;
    return m;
}

static APMATH_SHAPE random_shape(APMATH_SHAPE_KIND kind, double reach)
{
    APMATH_SHAPE s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.center = to_d(V(random_range(-reach, reach), random_range(-reach, reach), random_range(-reach, reach)));
    s.rotation = random_rotation();
    if (kind == APMATH_SHAPE_KIND_BOX)
        s.half_extents = to_d(V(random_range(0.1, 0.7), random_range(0.1, 0.7), random_range(0.1, 0.7)));
    else
        s.radius = random_range(0.1, 0.8);
    if (kind == APMATH_SHAPE_KIND_CAPSULE)
        s.half_height = random_range(0.0, 0.8);
    return s;
}

static APMATH_SHAPE sphere_at(double x, double y, double z, double r)
{
    APMATH_SHAPE s;
    memset(&s, 0, sizeof(s));
    s.kind = APMATH_SHAPE_KIND_SPHERE;
    s.center = vec3(x, y, z);
    s.rotation.w = 1.0;
    s.radius = r;
    return s;
}

static APMATH_SWEEP sweep_of(APMATH_SHAPE s, double dx, double dy, double dz, double t_max)
{
    APMATH_SWEEP w;
    memset(&w, 0, sizeof(w));
    w.shape = s;
    w.direction = vec3(dx, dy, dz);
    w.t_max = t_max;
    return w;
}

// Pending probe: whether the in-place calls exist yet.
static void probe_shapes(void)
{
    apmath_mesh_bvh cube = make_cube();
    APMATH_BVH_INSTANCE instance;
    memset(&instance, 0, sizeof(instance));
    instance.mesh = cube;
    apmath_instance_bvh bvh = make_instances(&instance, 1);
    uint32_t index = 0;
    APRESULT result = apmath_instance_bvh_set_instances(bvh, &index, &instance, 1, NULL);
    shapes_pending = result == APRESULT_NOT_IMPLEMENTED;
    if (shapes_pending)
        printf("apmathbvh_tests: shapes, sweeps and the in-place calls pending (not implemented)\n");
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(cube);
}

// Exact cases against the unit cube and two triangles.
static void test_shape_cases(void)
{
    current_test = "shape_cases";
    if (shapes_pending)
        return;
    apmath_mesh_bvh cube = make_cube();
    APMATH_SWEEP_HIT hit;

    // A sphere resting on the cube's top: touching counts.
    APMATH_SHAPE s = sphere_at(0.5, 1.5, 0.5, 0.5);
    CHECK(apmath_mesh_bvh_overlaps_shape(cube, NULL, &s));
    s.radius = 0.5 - 1e-9;
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &s));
    // Wholly inside the closed cube, touching none of it: not found.
    s = sphere_at(0.5, 0.5, 0.5, 0.25);
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &s));

    // Dropped onto the top from above: t is the gap, the normal +Y.
    APMATH_SWEEP w = sweep_of(sphere_at(0.5, 3.0, 0.5, 0.5), 0, -1, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    CHECK(near(hit.t, 1.5) && !hit.started_overlapping && near3(hit.contact_normal, 0, 1, 0) &&
          near3(hit.face_normal, 0, 1, 0) && near(hit.point.y, 1.0) && hit.instance == 0 && hit.id == 0);
    // Short of it: no touch; a NULL hit asks only whether.
    w.t_max = 1.4;
    CHECK(!apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    w.t_max = INFINITY;
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, NULL));
    // Missing by 1e-9 beside the cube.
    w = sweep_of(sphere_at(1.5 + 1e-9, 3.0, 0.5, 0.5), 0, -1, 0, 10);
    CHECK(!apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    // Past a corner: the contact normal along the closest points, not a face's.
    // Radius 0.8, its center 0.5 from the corner in x and z: it touches the
    // corner (1, 1, 1) when 0.5 + (y - 1)^2 = 0.64.
    w = sweep_of(sphere_at(1.5, 3.0, 1.5, 0.8), 0, -1, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    CHECK(fabs(hit.t - (2.0 - sqrt(0.14))) < 1e-9);
    CHECK(near(hit.point.x, 1.0) && near(hit.point.z, 1.0) && near(hit.point.y, 1.0));
    CHECK(near(hit.contact_normal.x, hit.contact_normal.z) && hit.contact_normal.x > 0.1 && hit.contact_normal.y > 0.1);
    // Starting overlapping: t 0.
    w = sweep_of(sphere_at(0.5, 1.2, 0.5, 0.5), 1, 0, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    CHECK(hit.t == 0.0 && hit.started_overlapping && near3(hit.contact_normal, 0, 1, 0));
    // A zero motion is an overlap test.
    w = sweep_of(sphere_at(0.5, 1.2, 0.5, 0.5), 0, 0, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, &hit) && hit.t == 0.0 && hit.started_overlapping);
    w = sweep_of(sphere_at(0.5, 2.0, 0.5, 0.5), 0, 0, 0, 10);
    CHECK(!apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    // Parallel to the top face's plane, grazing it: it first touches the
    // top's near edge, from above, when its center is over it.
    w = sweep_of(sphere_at(-2.0, 1.5, 0.5, 0.5), 1, 0, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    CHECK(fabs(hit.t - 2.0) < 1e-6 && !hit.started_overlapping && hit.t <= 2.0);
    CHECK(fabs(hit.contact_normal.y - 1.0) < 1e-3);

    // A capsule lying along the edge the top's two triangles share (from
    // (0, 1, 0) to (1, 1, 1)): one contact, the earlier triangle.
    APMATH_SHAPE capsule;
    memset(&capsule, 0, sizeof(capsule));
    capsule.kind = APMATH_SHAPE_KIND_CAPSULE;
    capsule.center = vec3(0.5, 3.0, 0.5);
    // The capsule's own y along (1, 0, 1): a turn of -90 degrees about
    // (1, 0, -1)/sqrt2 takes +Y there... built as a quaternion directly.
    {
        v3 axis = vmul(V(1, 0, -1), 1.0 / sqrt(2.0));
        double half = 0.25 * 3.14159265358979323846;
        capsule.rotation.x = axis.x * sin(half);
        capsule.rotation.y = axis.y * sin(half);
        capsule.rotation.z = axis.z * sin(half);
        capsule.rotation.w = cos(half);
        v3 y = rotate(capsule.rotation, V(0, 1, 0));
        CHECK(near(y.x, sqrt(0.5)) && near(y.z, sqrt(0.5)) && fabs(y.y) < 1e-12);
    }
    capsule.half_height = 0.3;
    capsule.radius = 0.25;
    w = sweep_of(capsule, 0, -1, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    CHECK(near(hit.t, 1.75) && near3(hit.contact_normal, 0, 1, 0));
    // The +Y face's triangles are 6 and 7 (2, 6, 7 and 2, 7, 3): the earlier.
    CHECK(hit.triangle == 6);

    // A box turned 45 degrees about Z, dropped onto the top: its lower
    // edge touches first, at its half diagonal.
    APMATH_SHAPE box;
    memset(&box, 0, sizeof(box));
    box.kind = APMATH_SHAPE_KIND_BOX;
    box.center = vec3(0.5, 3.0, 0.5);
    box.rotation.z = sin(0.125 * 3.14159265358979323846);
    box.rotation.w = cos(0.125 * 3.14159265358979323846);
    box.half_extents = vec3(0.2, 0.2, 0.2);
    w = sweep_of(box, 0, -1, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    CHECK(fabs(hit.t - (2.0 - 0.2 * sqrt(2.0))) < 1e-9 && near(hit.point.y, 1.0));
    CHECK(fabs(hit.contact_normal.y - 1.0) < 1e-6);
    // Turned onto a corner: still stops on the face, never inside.
    box.rotation = random_rotation();
    w = sweep_of(box, 0, -1, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    {
        v3 corners[8], axes[3];
        box_corners(&box, vadd(from_d(box.center), V(0, -hit.t, 0)), corners, axes);
        double lowest = INFINITY;
        for (int i = 0; i < 8; i++)
            lowest = corners[i].y < lowest ? corners[i].y : lowest;
        CHECK(lowest >= 1.0 - 1e-12 && lowest <= 1.0 + 1e-9);
    }

    // Radius 0: a point and a segment.
    s = sphere_at(0.5, 1.0, 0.5, 0.0);
    CHECK(apmath_mesh_bvh_overlaps_shape(cube, NULL, &s));
    w = sweep_of(sphere_at(0.5, 5.0, 0.5, 0.0), 0, -2, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, NULL, &w, &hit) && near(hit.t, 2.0));
    // A segment lying in the top face's plane touches it; just above, not.
    capsule.radius = 0.0;
    capsule.center = vec3(0.5, 1.0, 0.5);
    CHECK(apmath_mesh_bvh_overlaps_shape(cube, NULL, &capsule));
    capsule.center = vec3(0.5, 1.0 + 1e-9, 0.5);
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &capsule));

    // Placed: the mesh's transform carries it into the sweep's coordinates.
    ApriDMat4 m = placement(2, 2, 2, 10, 0, 0);
    w = sweep_of(sphere_at(11, 5.0, 1, 0.5), 0, -1, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, &m, &w, &hit) && near(hit.t, 2.5) && near3(hit.face_normal, 0, 1, 0));
    // Mirrored: the face normal is still out of the mesh's front.
    ApriDMat4 mirror = placement(-1, 1, 1, 0, 0, 0);
    w = sweep_of(sphere_at(-3.0, 0.5, 0.5, 0.5), 1, 0, 0, 10);
    CHECK(apmath_mesh_bvh_sweep(cube, &mirror, &w, &hit) && near(hit.t, 1.5) && near3(hit.face_normal, -1, 0, 0));

    // Refusals: each answers as for nothing there.
    s = sphere_at(0.5, 1.2, 0.5, 0.5);
    CHECK(apmath_mesh_bvh_overlaps_shape(cube, NULL, &s));
    APMATH_SHAPE bad = s;
    bad.kind = APMATH_SHAPE_KIND_NONE;
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    bad.kind = (APMATH_SHAPE_KIND)7;
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    bad = s;
    bad.radius = -0.1;
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    bad.radius = NAN;
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    bad = s;
    bad.center.x = INFINITY;
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    bad = box;
    bad.center = s.center;
    CHECK(apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    bad.rotation.w *= 1.01;
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    bad = box;
    bad.center = s.center;
    bad.radius = 0.1;
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    bad = box;
    bad.center = s.center;
    bad.half_extents.y = -0.1;
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    // A sphere ignores its rotation, even one not of unit length.
    bad = s;
    bad.rotation.w = 5.0;
    CHECK(apmath_mesh_bvh_overlaps_shape(cube, NULL, &bad));
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, NULL, NULL));
    CHECK(!apmath_mesh_bvh_overlaps_shape(NULL, NULL, &s));
    ApriDMat4 flat = placement(1, 0, 1, 0, 0, 0);
    CHECK(!apmath_mesh_bvh_overlaps_shape(cube, &flat, &s));
    w = sweep_of(s, 1, 0, 0, -1.0);
    CHECK(!apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    w.t_max = NAN;
    CHECK(!apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    w = sweep_of(s, INFINITY, 0, 0, 1.0);
    CHECK(!apmath_mesh_bvh_sweep(cube, NULL, &w, &hit));
    CHECK(!apmath_mesh_bvh_sweep(cube, NULL, NULL, &hit));
    apmath_mesh_bvh_destroy(cube);
}

// A scene of placed cubes and triangle soups, and the brute force's world
// triangles for it.
enum
{
    SHAPE_SCENE_INSTANCES = 14,
    SOUP_TRIANGLES = 30
};
static ApriDMat4 shape_scene_matrices[SHAPE_SCENE_INSTANCES];
static APMATH_BVH_INSTANCE shape_scene_instances[SHAPE_SCENE_INSTANCES];

static apmath_instance_bvh make_shape_scene(apmath_mesh_bvh cube, apmath_mesh_bvh soup)
{
    free(scene_triangles);
    scene_triangles = (world_triangle *)malloc(sizeof(world_triangle) * SHAPE_SCENE_INSTANCES * SOUP_TRIANGLES);
    scene_triangle_count = 0;
    for (uint32_t i = 0; i < SHAPE_SCENE_INSTANCES; i++)
    {
        shape_scene_matrices[i] = random_placement(4.0);
        APMATH_BVH_INSTANCE *in = &shape_scene_instances[i];
        memset(in, 0, sizeof(*in));
        in->mesh = i % 2 ? soup : cube;
        in->transform = &shape_scene_matrices[i];
        in->id = 1000 + i;
        in->mask = 1;
        if (i % 2)
            add_scene_mesh(soup_positions, soup_indices, SOUP_TRIANGLES, &shape_scene_matrices[i], i);
        else
            add_scene_mesh(cube_positions, cube_indices, 12, &shape_scene_matrices[i], i);
    }
    return make_instances(shape_scene_instances, SHAPE_SCENE_INSTANCES);
}

static const APMATH_SHAPE_KIND shape_kinds[3] = {APMATH_SHAPE_KIND_SPHERE, APMATH_SHAPE_KIND_CAPSULE,
                                                 APMATH_SHAPE_KIND_BOX};

// Overlaps: each shape's instances against every triangle's exact gap.
static void test_overlaps_against_brute_force(void)
{
    current_test = "overlaps_against_brute_force";
    if (shapes_pending)
        return;
    apmath_mesh_bvh cube = make_cube(), soup = make_soup(SOUP_TRIANGLES);
    apmath_instance_bvh bvh = make_shape_scene(cube, soup);
    int found = 0, borderline = 0, checked = 0;
    for (int k = 0; k < 3; k++)
        for (int n = 0; n < 1000; n++)
        {
            APMATH_SHAPE s = random_shape(shape_kinds[k], 5.0);
            uint32_t expected[SHAPE_SCENE_INSTANCES], expected_count = 0;
            bool unsure = false;
            for (uint32_t i = 0; i < SHAPE_SCENE_INSTANCES; i++)
            {
                double g = scene_gap(&s, from_d(s.center), i);
                if (fabs(g) < 1e-7)
                    unsure = true;
                if (g <= 0.0)
                    expected[expected_count++] = i;
                // The per-mesh call says the same of each instance.
                bool one = apmath_mesh_bvh_overlaps_shape(shape_scene_instances[i].mesh, &shape_scene_matrices[i], &s);
                if (fabs(g) >= 1e-7)
                    CHECK(one == (g <= 0.0));
            }
            if (unsure)
            {
                borderline++;
                continue;
            }
            checked++;
            uint32_t got[SHAPE_SCENE_INSTANCES];
            uint32_t count = apmath_instance_bvh_overlap_all(bvh, &s, NULL, got, SHAPE_SCENE_INSTANCES);
            CHECK(count == expected_count);
            for (uint32_t i = 0; i < count && i < expected_count; i++)
                CHECK(got[i] == expected[i]);
            CHECK(apmath_instance_bvh_overlaps_shape(bvh, &s, NULL) == (expected_count > 0));
            CHECK(apmath_instance_bvh_overlap_all(bvh, &s, NULL, NULL, 0) == expected_count);
            found += expected_count > 0;
        }
    printf("overlaps_against_brute_force: %d of %d shapes overlap something, %d borderline skipped\n", found, checked,
           borderline);
    CHECK(found > 100 && found < checked);
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(soup);
    apmath_mesh_bvh_destroy(cube);
}

// Sweeps: each against a march of gaps along its motion, bisected.
static void test_sweeps_against_brute_force(void)
{
    current_test = "sweeps_against_brute_force";
    if (shapes_pending)
        return;
    apmath_mesh_bvh cube = make_cube(), soup = make_soup(SOUP_TRIANGLES);
    apmath_instance_bvh bvh = make_shape_scene(cube, soup);
    int hits = 0, grazes = 0, sweeps = 0;
    for (int k = 0; k < 3; k++)
        for (int n = 0; n < 1000; n++)
        {
            APMATH_SHAPE s = random_shape(shape_kinds[k], 6.0);
            v3 d = V(random_range(-1, 1), random_range(-1, 1), random_range(-1, 1));
            d = vmul(d, 1.0 / vlen(d));
            APMATH_SWEEP w = sweep_of(s, d.x, d.y, d.z, random_range(0.5, 4.0));
            sweeps++;
            // Each instance's own first touch.
            double t_each[SHAPE_SCENE_INSTANCES];
            bool graze = false;
            uint32_t touched = 0;
            double best_t = INFINITY;
            uint32_t best = UINT32_MAX;
            for (uint32_t i = 0; i < SHAPE_SCENE_INSTANCES; i++)
            {
                double min_gap;
                t_each[i] = march_sweep(&w, i, &min_gap);
                if (t_each[i] < 0.0 && min_gap < 1e-7)
                    graze = true;
                if (t_each[i] >= 0.0)
                {
                    touched++;
                    if (t_each[i] < best_t)
                    {
                        best_t = t_each[i];
                        best = i;
                    }
                }
            }
            APMATH_SWEEP_HIT hit;
            bool got = apmath_instance_bvh_sweep(bvh, &w, NULL, &hit);
            if (got)
            {
                // Never inside what it met, at the t reported.
                v3 at = vadd(from_d(s.center), vmul(d, hit.t));
                double gap = scene_gap(&s, at, hit.instance);
                if (!hit.started_overlapping)
                    CHECK(gap >= -1e-9);
                CHECK(hit.id == 1000 + hit.instance);
                CHECK(fabs(vlen(from_d(hit.contact_normal)) - 1.0) < 1e-9 && fabs(vlen(from_d(hit.face_normal)) - 1.0) < 1e-9);
            }
            if (graze)
            {
                grazes++;
                continue;
            }
            CHECK(got == (best != UINT32_MAX));
            if (got && best != UINT32_MAX)
            {
                hits++;
                double tol = 1e-6 * w.t_max;
                if (!(fabs(hit.t - best_t) <= tol))
                {
                    v3 at = vadd(from_d(s.center), vmul(d, hit.t));
                    CHECK(fabs(scene_gap(&s, at, hit.instance)) <= 1e-7);
                }
                if (fabs(t_each[hit.instance] - best_t) > tol)
                    CHECK(hit.instance == best);
                // The per-mesh call agrees on its instance.
                APMATH_SWEEP_HIT one;
                CHECK(apmath_mesh_bvh_sweep(shape_scene_instances[hit.instance].mesh, &shape_scene_matrices[hit.instance],
                                            &w, &one));
                CHECK(one.t == hit.t && one.triangle == hit.triangle);
                // A NULL hit agrees.
                CHECK(apmath_instance_bvh_sweep(bvh, &w, NULL, NULL));
            }
            // Every instance's own first touch, in order of t then position.
            APMATH_SWEEP_HIT all[SHAPE_SCENE_INSTANCES];
            uint32_t count = apmath_instance_bvh_sweep_all(bvh, &w, NULL, all, SHAPE_SCENE_INSTANCES);
            CHECK(count == touched);
            for (uint32_t i = 0; i < count && i < SHAPE_SCENE_INSTANCES; i++)
            {
                CHECK(fabs(all[i].t - t_each[all[i].instance]) <= 1e-6 * w.t_max ||
                      fabs(scene_gap(&s, vadd(from_d(s.center), vmul(d, all[i].t)), all[i].instance)) <= 1e-7);
                if (i)
                    CHECK(all[i - 1].t < all[i].t || (all[i - 1].t == all[i].t && all[i - 1].instance < all[i].instance));
            }
            if (count)
                CHECK(all[0].instance == hit.instance && all[0].t == hit.t);
        }
    printf("sweeps_against_brute_force: %d of %d sweeps touch, %d grazes skipped\n", hits, sweeps - grazes, grazes);
    CHECK(hits > 200);
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(soup);
    apmath_mesh_bvh_destroy(cube);
}

// GJK's count: capsules grazing slivers - triangles one of whose edges is
// 1e-12 of the others - along that edge. Whichever path GJK takes, a sweep
// stops no later than the march and never inside, and a touch it reports is
// within 1e-9 of the larger size of the exact surface.
static void test_gjk_slivers(void)
{
    current_test = "gjk_slivers";
    if (shapes_pending)
        return;
    int touches = 0;
    for (int n = 0; n < 1000; n++)
    {
        v3 a = V(random_range(-1, 1), random_range(-1, 1), random_range(-1, 1));
        v3 b = vadd(a, V(random_range(0.5, 1.5), random_range(-0.2, 0.2), random_range(-0.2, 0.2)));
        v3 off = V(random_range(-1, 1), random_range(-1, 1), random_range(-1, 1));
        v3 c = vadd(a, vmul(off, 1e-12 / vlen(off)));
        ApriDVec3 positions[3] = {to_d(a), to_d(b), to_d(c)};
        APMATH_MESH_BVH_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
        desc.positions = positions;
        desc.position_count = 3;
        apmath_mesh_bvh sliver = NULL;
        CHECK(apmath_mesh_bvh_create(&desc, &sliver) == APRESULT_OK);
        free(scene_triangles);
        scene_triangles = (world_triangle *)malloc(sizeof(world_triangle));
        scene_triangle_count = 1;
        scene_triangles[0].p[0] = a;
        scene_triangles[0].p[1] = b;
        scene_triangles[0].p[2] = c;
        scene_triangles[0].instance = 0;
        scene_triangles[0].triangle = 0;
        scene_first[0] = 0;
        scene_count[0] = 1;
        scene_lo[0] = V(-INFINITY, -INFINITY, -INFINITY);
        scene_hi[0] = V(INFINITY, INFINITY, INFINITY);
        // A capsule along the long edge ab, offset sideways by its radius
        // plus a little, falling onto it.
        v3 ab = vsub(b, a);
        v3 side = vcross(ab, V(0, 0, 1));
        side = vmul(side, 1.0 / vlen(side));
        APMATH_SHAPE s;
        memset(&s, 0, sizeof(s));
        s.kind = APMATH_SHAPE_KIND_CAPSULE;
        s.radius = random_range(0.05, 0.3);
        s.half_height = random_range(0.0, 0.4);
        s.rotation = random_rotation();
        v3 mid = vmul(vadd(a, b), 0.5);
        s.center = to_d(vadd(mid, vmul(side, s.radius + random_range(-1e-9, 1e-9) + 1.0)));
        v3 d = vmul(side, -1.0);
        APMATH_SWEEP w = sweep_of(s, d.x, d.y, d.z, 2.0);
        APMATH_SWEEP_HIT hit;
        double min_gap;
        double t = march_sweep(&w, 0, &min_gap);
        bool got = apmath_mesh_bvh_sweep(sliver, NULL, &w, &hit);
        double size = 2.0 * (s.half_height + s.radius) > vlen(ab) ? 2.0 * (s.half_height + s.radius) : vlen(ab);
        if (got)
        {
            touches++;
            v3 at = vadd(from_d(s.center), vmul(d, hit.t));
            double gap = scene_gap(&s, at, 0);
            CHECK(gap >= -1e-12 * size); // not inside
            CHECK(gap <= 1e-9 * size);   // a touch near the surface
            if (t >= 0.0)
                CHECK(hit.t <= t + 1e-9 * w.t_max);
        }
        else
            CHECK(t < 0.0 || min_gap > -1e-9 * size);
        // An overlap at the march's touch says touching.
        if (t >= 0.0)
        {
            APMATH_SHAPE at = s;
            at.center = to_d(vadd(from_d(s.center), vmul(d, t)));
            CHECK(apmath_mesh_bvh_overlaps_shape(sliver, NULL, &at));
        }
        apmath_mesh_bvh_destroy(sliver);
    }
    printf("gjk_slivers: %d of 1000 sweeps touch\n", touches);
    CHECK(touches > 900);
}

// The sweep's walk: t_max INFINITY through 100,000 instances, against the
// per-mesh sweep of each instance alone.
static void test_long_sweeps(void)
{
    current_test = "long_sweeps";
    if (shapes_pending)
        return;
    enum
    {
        COUNT = 100000
    };
    apmath_mesh_bvh cube = make_cube();
    ApriDMat4 *matrices = (ApriDMat4 *)malloc(sizeof(ApriDMat4) * COUNT);
    APMATH_BVH_INSTANCE *instances = (APMATH_BVH_INSTANCE *)malloc(sizeof(APMATH_BVH_INSTANCE) * COUNT);
    for (uint32_t i = 0; i < COUNT; i++)
    {
        matrices[i] = placement(1, 1, 1, random_range(0, 200), random_range(0, 200), random_range(0, 200));
        instances[i].mesh = cube;
        instances[i].transform = &matrices[i];
        instances[i].id = i;
        instances[i].mask = 1;
    }
    apmath_instance_bvh bvh = make_instances(instances, COUNT);
    for (int n = 0; n < 4; n++)
    {
        APMATH_SHAPE s = random_shape(shape_kinds[n % 3], 1.0);
        s.center.x += 100;
        s.center.y += 100;
        s.center.z += 100;
        v3 d = V(random_range(-1, 1), random_range(-1, 1), random_range(-1, 1));
        d = vmul(d, 1.0 / vlen(d));
        APMATH_SWEEP w = sweep_of(s, d.x, d.y, d.z, INFINITY);
        APMATH_SWEEP_HIT hit, one;
        bool got = apmath_instance_bvh_sweep(bvh, &w, NULL, &hit);
        double best = INFINITY;
        uint32_t best_index = UINT32_MAX;
        for (uint32_t i = 0; i < COUNT; i++)
            if (apmath_mesh_bvh_sweep(cube, &matrices[i], &w, &one) && one.t < best)
            {
                best = one.t;
                best_index = i;
            }
        CHECK(got == (best_index != UINT32_MAX));
        if (got)
            CHECK(hit.instance == best_index && hit.t == best);
    }
    apmath_instance_bvh_destroy(bvh);
    free(instances);
    free(matrices);
    apmath_mesh_bvh_destroy(cube);
}

// Every answer of a BVH hashed: rays, overlaps and sweeps, each in full.
static uint64_t hash_answers(apmath_instance_bvh bvh, uint64_t seed)
{
    uint64_t saved = random_state;
    random_state = seed;
    uint64_t h = 0xCBF29CE484222325ull;
#define HASH(value)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        const unsigned char *bytes_ = (const unsigned char *)&(value);                                                 \
        for (size_t i_ = 0; i_ < sizeof(value); i_++)                                                                  \
        {                                                                                                              \
            h ^= bytes_[i_];                                                                                           \
            h *= 0x100000001B3ull;                                                                                     \
        }                                                                                                              \
    } while (0)
    for (int n = 0; n < 200; n++)
    {
        APMATH_RAY ray = random_ray(8.0);
        APMATH_INSTANCE_HIT hit;
        memset(&hit, 0, sizeof(hit));
        bool got = apmath_instance_bvh_cast(bvh, &ray, NULL, &hit);
        HASH(got);
        HASH(hit.instance);
        HASH(hit.triangle);
        HASH(hit.t);
        HASH(hit.point);
        HASH(hit.normal);
        APMATH_SHAPE s = random_shape(shape_kinds[n % 3], 6.0);
        uint32_t list[64];
        uint32_t count = apmath_instance_bvh_overlap_all(bvh, &s, NULL, list, 64);
        HASH(count);
        for (uint32_t i = 0; i < count && i < 64; i++)
            HASH(list[i]);
        v3 d = V(random_range(-1, 1), random_range(-1, 1), random_range(-1, 1));
        APMATH_SWEEP w = sweep_of(s, d.x, d.y, d.z, 3.0);
        APMATH_SWEEP_HIT sh;
        memset(&sh, 0, sizeof(sh));
        got = apmath_instance_bvh_sweep(bvh, &w, NULL, &sh);
        HASH(got);
        HASH(sh.instance);
        HASH(sh.triangle);
        HASH(sh.t);
        HASH(sh.point);
        HASH(sh.contact_normal);
        HASH(sh.face_normal);
        HASH(sh.started_overlapping);
    }
#undef HASH
    random_state = saved;
    return h;
}

// Created, then every instance moved and refitted, then rebuilt, then
// created fresh over the moved instances: the same bits all four ways.
static void test_tree_independence(void)
{
    current_test = "tree_independence";
    if (shapes_pending)
        return;
    apmath_mesh_bvh cube = make_cube(), soup = make_soup(SOUP_TRIANGLES);
    enum
    {
        COUNT = 300
    };
    ApriDMat4 before[COUNT], after[COUNT];
    APMATH_BVH_INSTANCE instances[COUNT];
    uint32_t indices[COUNT];
    for (uint32_t i = 0; i < COUNT; i++)
    {
        before[i] = random_placement(7.0);
        after[i] = random_placement(7.0);
        instances[i].mesh = i % 3 ? cube : soup;
        instances[i].transform = &before[i];
        instances[i].id = i;
        instances[i].mask = 1;
        indices[i] = i;
    }
    apmath_instance_bvh bvh = make_instances(instances, COUNT);
    double built_cost = apmath_instance_bvh_get_cost(bvh);
    CHECK(built_cost > 1.0);
    for (uint32_t i = 0; i < COUNT; i++)
        instances[i].transform = &after[i];
    apmath_instance_bvh fresh = make_instances(instances, COUNT);
    uint64_t want = hash_answers(fresh, 77);
    CHECK(apmath_instance_bvh_set_instances(bvh, indices, instances, COUNT, NULL) == APRESULT_OK);
    apmath_instance_bvh_refit(bvh);
    CHECK(hash_answers(bvh, 77) == want);
    // The refitted tree is worse than one built for where things are.
    CHECK(apmath_instance_bvh_get_cost(bvh) > apmath_instance_bvh_get_cost(fresh));
    apmath_instance_bvh_rebuild(bvh);
    CHECK(hash_answers(bvh, 77) == want);
    // Rebuilt node for node as a create: the same cost, bit for bit.
    CHECK(apmath_instance_bvh_get_cost(bvh) == apmath_instance_bvh_get_cost(fresh));
    // A refit with nothing set changes nothing.
    double cost = apmath_instance_bvh_get_cost(bvh);
    apmath_instance_bvh_refit(bvh);
    CHECK(apmath_instance_bvh_get_cost(bvh) == cost);
    CHECK(hash_answers(bvh, 77) == want);
    apmath_instance_bvh_destroy(fresh);
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(soup);
    apmath_mesh_bvh_destroy(cube);
}

// Setting instances in place: moved, hidden, given a mesh, refused.
static void test_set_instances(void)
{
    current_test = "set_instances";
    if (shapes_pending)
        return;
    apmath_mesh_bvh cube = make_cube();
    apmath_mesh_bvh empty_mesh = NULL;
    {
        APMATH_MESH_BVH_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
        CHECK(apmath_mesh_bvh_create(&desc, &empty_mesh) == APRESULT_OK);
    }
    // Four cubes along X at 0, 3, 6 and 9, and a free position (no mesh).
    ApriDMat4 at[5] = {placement(1, 1, 1, 0, 0, 0), placement(1, 1, 1, 3, 0, 0), placement(1, 1, 1, 6, 0, 0),
                       placement(1, 1, 1, 9, 0, 0), placement(1, 1, 1, 0, 0, 0)};
    APMATH_BVH_INSTANCE instances[5];
    memset(instances, 0, sizeof(instances));
    for (int i = 0; i < 5; i++)
    {
        instances[i].mesh = i == 4 ? NULL : cube;
        instances[i].transform = &at[i];
        instances[i].id = 100 + (uint64_t)i;
        instances[i].mask = 1;
    }
    apmath_instance_bvh bvh = make_instances(instances, 5);
    APMATH_INSTANCE_HIT hit;
    APMATH_RAY ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit) && hit.instance == 0);

    // Moved: found where it went and not where it was.
    ApriDMat4 up = placement(1, 1, 1, 0, 10, 0);
    APMATH_BVH_INSTANCE moved = instances[0];
    moved.transform = &up;
    uint32_t index = 0;
    APMATH_ERROR error;
    CHECK(apmath_instance_bvh_set_instances(bvh, &index, &moved, 1, &error) == APRESULT_OK);
    CHECK(error.message[0] == 0);
    apmath_instance_bvh_refit(bvh);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit) && hit.instance == 1 && near(hit.t, 5.0));
    APMATH_RAY down = make_ray(0.5, 20, 0.5, 0, -1, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &down, NULL, &hit) && hit.instance == 0 && near(hit.t, 9.0));
    APMATH_AABB bounds = apmath_instance_bvh_get_bounds(bvh);
    CHECK(near3(bounds.lo, 0, 0, 0) && near3(bounds.hi, 10, 11, 1));

    // The free position given a mesh, where the first was.
    APMATH_BVH_INSTANCE placed = instances[4];
    placed.mesh = cube;
    index = 4;
    CHECK(apmath_instance_bvh_set_instances(bvh, &index, &placed, 1, NULL) == APRESULT_OK);
    apmath_instance_bvh_refit(bvh);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit) && hit.instance == 4 && hit.id == 104 && near(hit.t, 2.0));

    // Hidden by a NULL mesh, and by a mesh with no triangles.
    placed.mesh = NULL;
    CHECK(apmath_instance_bvh_set_instances(bvh, &index, &placed, 1, NULL) == APRESULT_OK);
    apmath_instance_bvh_refit(bvh);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit) && hit.instance == 1);
    placed.mesh = empty_mesh;
    CHECK(apmath_instance_bvh_set_instances(bvh, &index, &placed, 1, NULL) == APRESULT_OK);
    apmath_instance_bvh_refit(bvh);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit) && hit.instance == 1);

    // Refusals, in parameter order, each its own message, each leaving the
    // BVH as it was.
    uint32_t two[2] = {1, 1};
    APMATH_BVH_INSTANCE pair[2] = {instances[1], instances[1]};
    ApriDMat4 far_away = placement(1, 1, 1, 50, 0, 0);
    pair[0].transform = &far_away;
    pair[1].transform = &far_away;
    CHECK(apmath_instance_bvh_set_instances(NULL, two, pair, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(error.message[0] != 0);
    CHECK(apmath_instance_bvh_set_instances(bvh, NULL, pair, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "indices") != NULL);
    CHECK(apmath_instance_bvh_set_instances(bvh, two, NULL, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "instances") != NULL);
    uint32_t past = 5;
    CHECK(apmath_instance_bvh_set_instances(bvh, &past, pair, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "past") != NULL);
    CHECK(apmath_instance_bvh_set_instances(bvh, two, pair, 2, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "twice") != NULL && strstr(error.message, "1") != NULL);
    ApriDMat4 flat = placement(1, 0, 1, 0, 0, 0), projective = placement(1, 1, 1, 0, 0, 0),
              not_finite = placement(1, 1, 1, NAN, 0, 0);
    projective.m[7] = 0.5;
    const ApriDMat4 *bad[3] = {&flat, &projective, &not_finite};
    for (int b = 0; b < 3; b++)
    {
        uint32_t ok_then_bad[2] = {2, 3};
        APMATH_BVH_INSTANCE ib[2] = {instances[2], instances[3]};
        ib[0].transform = &far_away;
        ib[1].transform = bad[b];
        CHECK(apmath_instance_bvh_set_instances(bvh, ok_then_bad, ib, 2, &error) == APRESULT_INVALID_ARGUMENT);
        CHECK(strstr(error.message, "transform") != NULL);
    }
    CHECK(apmath_instance_bvh_set_instances(bvh, NULL, NULL, 0, &error) == APRESULT_OK);
    // Nothing changed by any of them: instance 2 not moved, 1 still where it was.
    apmath_instance_bvh_refit(bvh);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit) && hit.instance == 1 && near(hit.t, 5.0));
    APMATH_RAY at6 = make_ray(6.5, 20, 0.5, 0, -1, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &at6, NULL, &hit) && hit.instance == 2);

    // A set refused for an index given twice after an earlier set of other
    // instances: the earlier set's marks kept, so the refit carries it up.
    uint32_t three = 3;
    APMATH_BVH_INSTANCE moved3 = instances[3];
    ApriDMat4 at20 = placement(1, 1, 1, 20, 0, 0);
    moved3.transform = &at20;
    CHECK(apmath_instance_bvh_set_instances(bvh, &three, &moved3, 1, NULL) == APRESULT_OK);
    uint32_t dup[3] = {2, 0, 2};
    APMATH_BVH_INSTANCE dups[3] = {instances[2], instances[0], instances[2]};
    CHECK(apmath_instance_bvh_set_instances(bvh, dup, dups, 3, &error) == APRESULT_INVALID_ARGUMENT);
    apmath_instance_bvh_refit(bvh);
    APMATH_RAY at20ray = make_ray(20.5, 20, 0.5, 0, -1, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &at20ray, NULL, &hit) && hit.instance == 3);

    // The cost rises as instances scatter and falls back on a rebuild to a
    // fresh create's, exactly.
    enum
    {
        COUNT = 400
    };
    ApriDMat4 grid[COUNT], scattered[COUNT];
    APMATH_BVH_INSTANCE many[COUNT];
    uint32_t all[COUNT];
    for (uint32_t i = 0; i < COUNT; i++)
    {
        grid[i] = placement(1, 1, 1, (i % 20) * 2.0, 0, (i / 20) * 2.0);
        scattered[i] = placement(1, 1, 1, random_range(0, 40), random_range(0, 40), random_range(0, 40));
        many[i].mesh = cube;
        many[i].transform = &grid[i];
        many[i].id = i;
        many[i].mask = 1;
        all[i] = i;
    }
    apmath_instance_bvh big = make_instances(many, COUNT);
    double cost0 = apmath_instance_bvh_get_cost(big);
    for (uint32_t i = 0; i < COUNT; i++)
        many[i].transform = &scattered[i];
    CHECK(apmath_instance_bvh_set_instances(big, all, many, COUNT, NULL) == APRESULT_OK);
    apmath_instance_bvh_refit(big);
    double refitted = apmath_instance_bvh_get_cost(big);
    apmath_instance_bvh_rebuild(big);
    double rebuilt = apmath_instance_bvh_get_cost(big);
    apmath_instance_bvh fresh = make_instances(many, COUNT);
    CHECK(rebuilt == apmath_instance_bvh_get_cost(fresh));
    CHECK(refitted > rebuilt * 1.5);
    CHECK(cost0 > 1.0);
    apmath_instance_bvh_destroy(fresh);
    apmath_instance_bvh_destroy(big);

    // Fewer than two instances with triangles: cost 0.
    apmath_instance_bvh one = make_instances(instances, 1);
    CHECK(apmath_instance_bvh_get_cost(one) == 0.0);
    apmath_instance_bvh_destroy(one);
    CHECK(apmath_instance_bvh_get_cost(NULL) == 0.0);
    apmath_instance_bvh_refit(NULL);
    apmath_instance_bvh_rebuild(NULL);

    // Shapes on a BVH with a desc: masks and the filter leave out the only one.
    APMATH_SHAPE s = sphere_at(6.5, 1.2, 0.5, 0.5);
    CHECK(apmath_instance_bvh_overlaps_shape(bvh, &s, NULL));
    APMATH_CAST_DESC cast;
    memset(&cast, 0, sizeof(cast));
    cast.base.type = APRI_STRUCT_TYPE_APMATH_CAST_DESC;
    cast.mask = 2;
    CHECK(!apmath_instance_bvh_overlaps_shape(bvh, &s, &cast));
    cast.mask = 1;
    uint64_t skip = 102;
    cast.filter = filter_skip_one;
    cast.user = &skip;
    memset(filter_calls, 0, sizeof(filter_calls));
    CHECK(!apmath_instance_bvh_overlaps_shape(bvh, &s, &cast));
    CHECK(filter_calls[2] == 1);
    APMATH_SWEEP w = sweep_of(sphere_at(6.5, 5, 0.5, 0.5), 0, -1, 0, 10);
    CHECK(!apmath_instance_bvh_sweep(bvh, &w, &cast, NULL));
    CHECK(apmath_instance_bvh_sweep_all(bvh, &w, &cast, NULL, 0) == 0);
    CHECK(apmath_instance_bvh_overlap_all(bvh, &s, &cast, NULL, 0) == 0);
    APMATH_CAST_DESC wrong = cast;
    wrong.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    CHECK(!apmath_instance_bvh_sweep(NULL, &w, NULL, NULL));
    CHECK(!apmath_instance_bvh_overlaps_shape(bvh, &s, &wrong));

    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(empty_mesh);
    apmath_mesh_bvh_destroy(cube);
}

// overlap_all and sweep_all's buffers: capacities 0, 1 and one short; and
// the lowest 100 of 10,000 found, in order, against a tree built from
// shuffled boxes.
static void test_lists(void)
{
    current_test = "lists";
    if (shapes_pending)
        return;
    enum
    {
        COUNT = 10000
    };
    apmath_mesh_bvh cube = make_cube();
    ApriDMat4 *matrices = (ApriDMat4 *)malloc(sizeof(ApriDMat4) * COUNT);
    APMATH_BVH_INSTANCE *instances = (APMATH_BVH_INSTANCE *)malloc(sizeof(APMATH_BVH_INSTANCE) * COUNT);
    for (uint32_t i = 0; i < COUNT; i++)
    {
        matrices[i] = placement(0.1, 0.1, 0.1, random_range(0, 10), random_range(0, 10), random_range(0, 10));
        instances[i].mesh = cube;
        instances[i].transform = &matrices[i];
        instances[i].id = i;
        instances[i].mask = 1;
    }
    apmath_instance_bvh bvh = make_instances(instances, COUNT);
    APMATH_SHAPE s;
    memset(&s, 0, sizeof(s));
    s.kind = APMATH_SHAPE_KIND_BOX;
    s.center = vec3(5, 5, 5);
    s.rotation.w = 1;
    s.half_extents = vec3(6, 6, 6);
    uint32_t total = apmath_instance_bvh_overlap_all(bvh, &s, NULL, NULL, 0);
    CHECK(total == COUNT);
    uint32_t lowest[100];
    CHECK(apmath_instance_bvh_overlap_all(bvh, &s, NULL, lowest, 100) == COUNT);
    for (uint32_t i = 0; i < 100; i++)
        CHECK(lowest[i] == i);
    uint32_t onefound;
    CHECK(apmath_instance_bvh_overlap_all(bvh, &s, NULL, &onefound, 1) == COUNT && onefound == 0);
    // A sweep through a line of them, by t then position.
    APMATH_SHAPE ball = sphere_at(-1, 5, 5, 3);
    APMATH_SWEEP w = sweep_of(ball, 1, 0, 0, 12);
    uint32_t touched = apmath_instance_bvh_sweep_all(bvh, &w, NULL, NULL, 0);
    CHECK(touched > 100);
    APMATH_SWEEP_HIT *hits = (APMATH_SWEEP_HIT *)malloc(sizeof(APMATH_SWEEP_HIT) * touched);
    CHECK(apmath_instance_bvh_sweep_all(bvh, &w, NULL, hits, touched) == touched);
    for (uint32_t i = 1; i < touched; i++)
        CHECK(hits[i - 1].t < hits[i].t || (hits[i - 1].t == hits[i].t && hits[i - 1].instance < hits[i].instance));
    APMATH_SWEEP_HIT short_list[64];
    CHECK(apmath_instance_bvh_sweep_all(bvh, &w, NULL, short_list, 64) == touched);
    for (uint32_t i = 0; i < 64; i++)
        CHECK(short_list[i].instance == hits[i].instance && short_list[i].t == hits[i].t);
    APMATH_SWEEP_HIT one_short[1];
    CHECK(apmath_instance_bvh_sweep_all(bvh, &w, NULL, one_short, 1) == touched &&
          one_short[0].instance == hits[0].instance);
    CHECK(apmath_instance_bvh_sweep_all(bvh, &w, NULL, hits, touched - 1) == touched);
    free(hits);
    apmath_instance_bvh_destroy(bvh);
    free(instances);
    free(matrices);
    apmath_mesh_bvh_destroy(cube);
}

static apmath_box_bvh make_boxes(const APMATH_BVH_BOX *boxes, uint32_t count)
{
    APMATH_BOX_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_BOX_BVH_DESC;
    desc.boxes = boxes;
    desc.box_count = count;
    apmath_box_bvh bvh = NULL;
    CHECK(apmath_box_bvh_create(&desc, &bvh, NULL) == APRESULT_OK);
    return bvh;
}

static APMATH_BVH_BOX box_entry(double lx, double ly, double lz, double hx, double hy, double hz, uint64_t id)
{
    APMATH_BVH_BOX b;
    memset(&b, 0, sizeof(b));
    b.box.lo = vec3(lx, ly, lz);
    b.box.hi = vec3(hx, hy, hz);
    b.id = id;
    b.mask = 1;
    return b;
}

// Slab exact: a ray against a solid box, its t from t_min on.
static bool brute_ray_box(const APMATH_RAY *ray, APMATH_AABB box, double *out_t)
{
    double lo = ray->t_min, hi = ray->t_max;
    double o[3] = {ray->origin.x, ray->origin.y, ray->origin.z}, d[3] = {ray->direction.x, ray->direction.y, ray->direction.z};
    double bl[3] = {box.lo.x, box.lo.y, box.lo.z}, bh[3] = {box.hi.x, box.hi.y, box.hi.z};
    if (bl[0] > bh[0] || bl[1] > bh[1] || bl[2] > bh[2])
        return false;
    for (int a = 0; a < 3; a++)
    {
        if (d[a] == 0)
        {
            if (o[a] < bl[a] || o[a] > bh[a])
                return false;
            continue;
        }
        double t1 = (bl[a] - o[a]) / d[a], t2 = (bh[a] - o[a]) / d[a];
        if (t1 > t2)
        {
            double x = t1;
            t1 = t2;
            t2 = x;
        }
        lo = t1 > lo ? t1 : lo;
        hi = t2 < hi ? t2 : hi;
        if (lo > hi)
            return false;
    }
    *out_t = lo;
    return true;
}

static void test_box_bvh(void)
{
    current_test = "box_bvh";
    if (shapes_pending)
        return;
    // Arguments.
    APMATH_ERROR error;
    APMATH_BOX_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APMATH_BOX_BVH_DESC;
    apmath_box_bvh bvh = (apmath_box_bvh)1;
    CHECK(apmath_box_bvh_create(NULL, &bvh, &error) == APRESULT_INVALID_ARGUMENT && bvh == NULL);
    CHECK(apmath_box_bvh_create(&desc, NULL, &error) == APRESULT_INVALID_ARGUMENT);
    desc.box_count = 1;
    CHECK(apmath_box_bvh_create(&desc, &bvh, &error) == APRESULT_INVALID_ARGUMENT && bvh == NULL);
    APMATH_BVH_BOX not_finite = box_entry(0, 0, 0, INFINITY, 1, 1, 0);
    desc.boxes = &not_finite;
    CHECK(apmath_box_bvh_create(&desc, &bvh, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "0") != NULL);
    desc.base.type = APRI_STRUCT_TYPE_APMATH_CAST_DESC;
    CHECK(apmath_box_bvh_create(&desc, &bvh, &error) == APRESULT_STRUCT_WRONG_TYPE);
    desc.base.type = APRI_STRUCT_TYPE_APMATH_BOX_BVH_DESC;
    desc.box_count = 0;
    CHECK(apmath_box_bvh_create(&desc, &bvh, &error) == APRESULT_OK);
    APMATH_RAY ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(!apmath_box_bvh_cast(bvh, &ray, NULL, NULL));
    CHECK(apmath_box_bvh_get_cost(bvh) == 0.0);
    apmath_box_bvh_destroy(bvh);
    apmath_box_bvh_destroy(NULL);

    // Three boxes in a row, an empty one, and solid answers.
    APMATH_BVH_BOX boxes[4] = {box_entry(0, 0, 0, 1, 1, 1, 10), box_entry(3, 0, 0, 4, 1, 1, 11),
                               box_entry(6, 0, 0, 7, 1, 1, 12), box_entry(1, 1, 1, 0, 0, 0, 13)};
    bvh = make_boxes(boxes, 4);
    APMATH_BOX_HIT hit;
    CHECK(apmath_box_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(hit.box == 0 && hit.id == 10 && near(hit.t, 2.0) && !hit.started_inside && near3(hit.face_normal, -1, 0, 0) &&
          near3(hit.contact_normal, -1, 0, 0) && near3(hit.point, 0, 0.5, 0.5));
    // From inside the second box: met at t_min, normals against the ray.
    ray = make_ray(3.5, 0.5, 0.5, 2, 0, 0);
    CHECK(apmath_box_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(hit.box == 1 && hit.t == 0.0 && hit.started_inside && near3(hit.face_normal, -1, 0, 0) &&
          near3(hit.point, 3.5, 0.5, 0.5));
    APMATH_BOX_HIT all[4];
    CHECK(apmath_box_bvh_cast_all(bvh, &ray, NULL, all, 4) == 2);
    CHECK(all[0].box == 1 && all[1].box == 2 && near(all[1].t, 1.25));
    CHECK(apmath_box_bvh_cast_all(bvh, &ray, NULL, NULL, 0) == 2);
    // A sphere wholly inside a box overlaps it; the empty box is never met.
    APMATH_SHAPE s = sphere_at(0.5, 0.5, 0.5, 0.1);
    CHECK(apmath_box_bvh_overlaps_shape(bvh, &s, NULL));
    uint32_t list[4];
    CHECK(apmath_box_bvh_overlap_all(bvh, &s, NULL, list, 4) == 1 && list[0] == 0);
    s = sphere_at(2, 0.5, 0.5, 1.0);
    CHECK(apmath_box_bvh_overlap_all(bvh, &s, NULL, list, 4) == 2 && list[0] == 0 && list[1] == 1);
    // A sweep from inside touches at 0; one from outside at its gap.
    APMATH_SWEEP w = sweep_of(sphere_at(0.5, 0.5, 0.5, 0.1), 1, 0, 0, 10);
    APMATH_BOX_HIT bh;
    CHECK(apmath_box_bvh_sweep(bvh, &w, NULL, &bh) && bh.box == 0 && bh.t == 0.0 && bh.started_inside);
    w = sweep_of(sphere_at(1.5, 0.5, 0.5, 0.25), 1, 0, 0, 10);
    CHECK(apmath_box_bvh_sweep(bvh, &w, NULL, &bh) && bh.box == 1 && near(bh.t, 1.25) && !bh.started_inside);
    CHECK(near3(bh.contact_normal, -1, 0, 0) && near3(bh.face_normal, -1, 0, 0) && near3(bh.point, 3, 0.5, 0.5));
    CHECK(apmath_box_bvh_sweep_all(bvh, &w, NULL, all, 4) == 2 && all[0].box == 1 && all[1].box == 2);

    // set_boxes: refusals in parameter order, nothing changed; then moved.
    uint32_t idx = 1, past = 4, twice[2] = {2, 2};
    APMATH_BVH_BOX moved = box_entry(3, 10, 0, 4, 11, 1, 11);
    APMATH_BVH_BOX pair[2] = {moved, moved};
    CHECK(apmath_box_bvh_set_boxes(NULL, &idx, &moved, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(apmath_box_bvh_set_boxes(bvh, NULL, &moved, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(apmath_box_bvh_set_boxes(bvh, &idx, NULL, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(apmath_box_bvh_set_boxes(bvh, &past, &moved, 1, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(apmath_box_bvh_set_boxes(bvh, twice, pair, 2, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strstr(error.message, "twice") != NULL);
    APMATH_BVH_BOX nan_box = box_entry(3, NAN, 0, 4, 11, 1, 11);
    CHECK(apmath_box_bvh_set_boxes(bvh, &idx, &nan_box, 1, &error) == APRESULT_INVALID_ARGUMENT);
    apmath_box_bvh_refit(bvh);
    ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(apmath_box_bvh_cast_all(bvh, &ray, NULL, all, 4) == 3);
    CHECK(apmath_box_bvh_set_boxes(bvh, &idx, &moved, 1, &error) == APRESULT_OK && error.message[0] == 0);
    // The empty one given a box.
    uint32_t idx3 = 3;
    APMATH_BVH_BOX filled = box_entry(9, 0, 0, 10, 1, 1, 13);
    CHECK(apmath_box_bvh_set_boxes(bvh, &idx3, &filled, 1, NULL) == APRESULT_OK);
    apmath_box_bvh_refit(bvh);
    CHECK(apmath_box_bvh_cast_all(bvh, &ray, NULL, all, 4) == 3);
    CHECK(all[0].box == 0 && all[1].box == 2 && all[2].box == 3);
    apmath_box_bvh_rebuild(bvh);
    CHECK(apmath_box_bvh_cast_all(bvh, &ray, NULL, all, 4) == 3 && all[2].box == 3);
    // Masks and filters as the instance BVH's.
    APMATH_CAST_DESC cast;
    memset(&cast, 0, sizeof(cast));
    cast.base.type = APRI_STRUCT_TYPE_APMATH_CAST_DESC;
    cast.mask = 2;
    CHECK(!apmath_box_bvh_cast(bvh, &ray, &cast, NULL));
    apmath_box_bvh_destroy(bvh);

    // Brute force over 2,000 random boxes, and tree independence.
    enum
    {
        COUNT = 2000
    };
    APMATH_BVH_BOX *many = (APMATH_BVH_BOX *)malloc(sizeof(APMATH_BVH_BOX) * COUNT);
    APMATH_BVH_BOX *later = (APMATH_BVH_BOX *)malloc(sizeof(APMATH_BVH_BOX) * COUNT);
    uint32_t *indices = (uint32_t *)malloc(sizeof(uint32_t) * COUNT);
    for (uint32_t i = 0; i < COUNT; i++)
    {
        double x = random_range(0, 30), y = random_range(0, 30), z = random_range(0, 30);
        many[i] = box_entry(x, y, z, x + random_range(0.1, 2), y + random_range(0.1, 2), z + random_range(0.1, 2), i);
        x = random_range(0, 30), y = random_range(0, 30), z = random_range(0, 30);
        later[i] = box_entry(x, y, z, x + random_range(0.1, 2), y + random_range(0.1, 2), z + random_range(0.1, 2), i);
        if (i % 50 == 0)
            later[i] = box_entry(1, 1, 1, 0, 0, 0, i); // emptied
        indices[i] = i;
    }
    bvh = make_boxes(many, COUNT);
    for (int n = 0; n < 500; n++)
    {
        APMATH_RAY r = random_ray(15.0);
        r.origin.x += 15;
        r.origin.y += 15;
        r.origin.z += 15;
        double best = INFINITY, t;
        uint32_t best_box = UINT32_MAX, count = 0;
        for (uint32_t i = 0; i < COUNT; i++)
            if (brute_ray_box(&r, many[i].box, &t))
            {
                count++;
                if (t < best)
                {
                    best = t;
                    best_box = i;
                }
            }
        bool got = apmath_box_bvh_cast(bvh, &r, NULL, &hit);
        CHECK(got == (best_box != UINT32_MAX));
        if (got)
            CHECK(hit.box == best_box && near(hit.t, best));
        CHECK(apmath_box_bvh_cast_all(bvh, &r, NULL, NULL, 0) == count);
        APMATH_SHAPE shape = random_shape(shape_kinds[n % 3], 15.0);
        shape.center.x += 15;
        shape.center.y += 15;
        shape.center.z += 15;
        uint32_t expected = 0;
        bool unsure = false;
        double reach = shape.radius + shape.half_height + vlen(from_d(shape.half_extents)) + 1e-6;
        for (uint32_t i = 0; i < COUNT; i++)
        {
            APMATH_AABB b = many[i].box;
            if (shape.center.x - reach > b.hi.x || shape.center.y - reach > b.hi.y || shape.center.z - reach > b.hi.z ||
                shape.center.x + reach < b.lo.x || shape.center.y + reach < b.lo.y || shape.center.z + reach < b.lo.z)
                continue;
            double g = shape_box_gap(&shape, from_d(shape.center), many[i].box);
            unsure |= fabs(g) < 1e-7;
            expected += g <= 0.0;
        }
        if (!unsure)
        {
            CHECK(apmath_box_bvh_overlap_all(bvh, &shape, NULL, NULL, 0) == expected);
            CHECK(apmath_box_bvh_overlaps_shape(bvh, &shape, NULL) == (expected > 0));
        }
    }
    // Tree independence: moved and refitted, rebuilt, created fresh.
    apmath_box_bvh fresh = make_boxes(later, COUNT);
    CHECK(apmath_box_bvh_set_boxes(bvh, indices, later, COUNT, NULL) == APRESULT_OK);
    apmath_box_bvh_refit(bvh);
    apmath_box_bvh rebuilt = make_boxes(many, COUNT);
    CHECK(apmath_box_bvh_set_boxes(rebuilt, indices, later, COUNT, NULL) == APRESULT_OK);
    apmath_box_bvh_rebuild(rebuilt);
    CHECK(apmath_box_bvh_get_cost(rebuilt) == apmath_box_bvh_get_cost(fresh));
    for (int n = 0; n < 300; n++)
    {
        APMATH_RAY r = random_ray(15.0);
        r.origin.x += 15;
        r.origin.y += 15;
        r.origin.z += 15;
        APMATH_BOX_HIT a, b, c;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        memset(&c, 0, sizeof(c));
        bool ga = apmath_box_bvh_cast(bvh, &r, NULL, &a), gb = apmath_box_bvh_cast(rebuilt, &r, NULL, &b),
             gc = apmath_box_bvh_cast(fresh, &r, NULL, &c);
        CHECK(ga == gc && gb == gc && memcmp(&a, &c, sizeof(a)) == 0 && memcmp(&b, &c, sizeof(b)) == 0);
        APMATH_SHAPE shape = random_shape(shape_kinds[n % 3], 15.0);
        shape.center.x += 15;
        shape.center.y += 15;
        shape.center.z += 15;
        v3 d = V(random_range(-1, 1), random_range(-1, 1), random_range(-1, 1));
        APMATH_SWEEP sw = sweep_of(shape, d.x, d.y, d.z, 5.0);
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        memset(&c, 0, sizeof(c));
        ga = apmath_box_bvh_sweep(bvh, &sw, NULL, &a);
        gb = apmath_box_bvh_sweep(rebuilt, &sw, NULL, &b);
        gc = apmath_box_bvh_sweep(fresh, &sw, NULL, &c);
        CHECK(ga == gc && gb == gc && memcmp(&a, &c, sizeof(a)) == 0 && memcmp(&b, &c, sizeof(b)) == 0);
        // The sweep against the brute force's march over every box.
        double first = INFINITY;
        uint32_t first_box = UINT32_MAX;
        bool graze = false;
        double reach = shape.radius + shape.half_height + vlen(from_d(shape.half_extents)) + 1e-6;
        v3 c0 = from_d(shape.center), c1 = vadd(c0, vmul(d, 5.0));
        for (uint32_t i = 0; i < COUNT; i++)
        {
            double t = -1;
            APMATH_AABB b = later[i].box;
            if (fmin(c0.x, c1.x) - reach > b.hi.x || fmin(c0.y, c1.y) - reach > b.hi.y ||
                fmin(c0.z, c1.z) - reach > b.hi.z || fmax(c0.x, c1.x) + reach < b.lo.x ||
                fmax(c0.y, c1.y) + reach < b.lo.y || fmax(c0.z, c1.z) + reach < b.lo.z)
                continue; // out of the sweep's reach (an emptied box too)
            for (int k = 0; k <= 200; k++)
            {
                double tk = 5.0 * k / 200;
                double g = shape_box_gap(&shape, vadd(from_d(shape.center), vmul(d, tk)), later[i].box);
                if (fabs(g) < 1e-7)
                    graze = true;
                if (g <= 0)
                {
                    t = tk;
                    break;
                }
            }
            if (t >= 0 && t < first)
            {
                first = t;
                first_box = i;
            }
        }
        if (!graze)
        {
            CHECK(gc == (first_box != UINT32_MAX));
            if (gc && first_box != UINT32_MAX)
                CHECK(c.t <= first + 1e-9 && c.t >= first - 5.0 / 200 - 1e-9);
        }
    }
    apmath_box_bvh_destroy(rebuilt);
    apmath_box_bvh_destroy(fresh);
    apmath_box_bvh_destroy(bvh);
    free(indices);
    free(later);
    free(many);
}

// The early stop of a cast asked only whether: the same answer.
static void test_cast_any(void)
{
    current_test = "cast_any";
    apmath_mesh_bvh cube = make_cube();
    enum
    {
        COUNT = 500
    };
    ApriDMat4 m[COUNT];
    APMATH_BVH_INSTANCE instances[COUNT];
    for (uint32_t i = 0; i < COUNT; i++)
    {
        m[i] = placement(1, 1, 1, random_range(0, 30), random_range(0, 30), random_range(0, 30));
        instances[i].mesh = cube;
        instances[i].transform = &m[i];
        instances[i].id = i;
        instances[i].mask = 1;
    }
    apmath_instance_bvh bvh = make_instances(instances, COUNT);
    for (int n = 0; n < 2000; n++)
    {
        APMATH_RAY r = random_ray(15.0);
        r.origin.x += 15;
        r.origin.y += 15;
        r.origin.z += 15;
        APMATH_INSTANCE_HIT hit;
        CHECK(apmath_instance_bvh_cast(bvh, &r, NULL, &hit) == apmath_instance_bvh_cast(bvh, &r, NULL, NULL));
    }
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(cube);
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
    test_cast_any();
    probe_shapes();
    test_shape_cases();
    test_overlaps_against_brute_force();
    test_sweeps_against_brute_force();
    test_gjk_slivers();
    test_long_sweeps();
    test_tree_independence();
    test_set_instances();
    test_lists();
    test_box_bvh();
    free(scene_triangles);
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("apmathbvh_tests: all passed\n");
    return 0;
}
