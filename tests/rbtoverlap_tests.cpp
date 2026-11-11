// Tests of the Debug builds' overlap check every module uses
// (src/rbtoverlap.hpp; rbtcore.h, "Threads"): a second thread entering
// while one is inside is reported, and nothing else is - not the same
// thread entering again, not a call from another thread once the last has
// returned, not two objects at once. Release builds check nothing, and this
// test says so and passes.
//
// Before those, in every build so ThreadSanitizer sees it: reads a header
// lets overlap, from two threads at once.

#include "../src/rbtoverlap.hpp"
#include "../src/audio/rbtaudio_internal.hpp"
#include "../src/math/rbtmathbvh_internal.hpp"
#include "math/rbtmathbvh.h"
#include "../src/render/rbtrendlit_internal.hpp"
#include "../src/render/rbtrendraytracing_internal.hpp"

#include <atomic>
#include <stdio.h>
#include <thread>

static int failures = 0;

#define CHECK(condition)                                                              \
    do                                                                                \
    {                                                                                 \
        if (!(condition))                                                             \
        {                                                                             \
            printf("FAIL (%s:%d): %s\n", __FILE__, __LINE__, #condition);            \
            failures++;                                                               \
        }                                                                             \
    } while (0)

// Enters [guard] and stays until [release] is set, saying it is inside.
static void hold(rbt_overlap *guard, std::atomic<bool> *inside, std::atomic<bool> *release)
{
    RBT_ENTER(*guard, "test", "hold", "object");
    inside->store(true);
    while (!release->load())
        std::this_thread::yield();
}

static void enter_once(rbt_overlap *guard)
{
    RBT_ENTER(*guard, "test", "enter_once", "object");
}

// Rabbittus Math's volume tests and casts on one mesh BVH from two threads at once
// (rbtmathbvh.h, "Threads"): each gets the answers the main thread got.
static void test_volume_reads()
{
    const RbtDVec3 positions[8] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    const uint32_t indices[36] = {0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4,
                                  2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};
    RBTMATH_MESH_BVH_DESC desc{};
    desc.base.type = RBT_STRUCT_TYPE_MATH_MESH_BVH_DESC;
    desc.positions = positions;
    desc.position_count = 8;
    desc.indices = indices;
    desc.index_count = 36;
    rbtmath_mesh_bvh bvh = nullptr;
    CHECK(rbtmath_mesh_bvh_create(&desc, &bvh) == RBT_RESULT_OK);
    if (!bvh)
        return;
    // x >= edge, for edges across and beside the cube.
    auto answers = [bvh](int step, bool &overlaps, bool &within, bool &hit) {
        RBTMATH_PLANE plane = {{1, 0, 0}, -(double)(step % 7) * 0.25};
        RBTMATH_VOLUME volume = {&plane, 1, nullptr, 0};
        overlaps = rbtmath_mesh_bvh_overlaps_volume(bvh, nullptr, &volume);
        within = rbtmath_mesh_bvh_within_volume(bvh, nullptr, &volume);
        RBTMATH_RAY ray = {{0.5, 0.5, 5.0}, {0.0, 0.0, -1.0}, 0.0, 10.0};
        hit = rbtmath_mesh_bvh_cast(bvh, &ray, nullptr);
    };
    bool expected[7][3];
    for (int step = 0; step < 7; step++)
        answers(step, expected[step][0], expected[step][1], expected[step][2]);
    std::atomic<int> wrong{0};
    auto run = [&]() {
        for (int i = 0; i < 2000; i++)
        {
            bool got[3];
            answers(i, got[0], got[1], got[2]);
            for (int k = 0; k < 3; k++)
                if (got[k] != expected[i % 7][k])
                    wrong++;
        }
    };
    std::thread a(run), b(run);
    a.join();
    b.join();
    CHECK(wrong.load() == 0);
    // The answers themselves: x >= 0 holds it all, x >= 1.5 none of it.
    CHECK(expected[0][0] && expected[0][1]);
    CHECK(!expected[6][0] && !expected[6][1]);
    rbtmath_mesh_bvh_destroy(bvh);
}

// Rabbittus Math's instance and box BVHs (rbtmathbvh.h, "Threads"), in every build so
// ThreadSanitizer sees it: casts, overlaps and sweeps from two threads on one
// BVH, each getting the main thread's answers; then set_instances on disjoint
// instances from two threads at once, then a refit, and the answers after.
static rbtmath_mesh_bvh make_cube_bvh()
{
    static const RbtDVec3 positions[8] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                           {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static const uint32_t indices[36] = {0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4,
                                         2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};
    RBTMATH_MESH_BVH_DESC desc{};
    desc.base.type = RBT_STRUCT_TYPE_MATH_MESH_BVH_DESC;
    desc.positions = positions;
    desc.position_count = 8;
    desc.indices = indices;
    desc.index_count = 36;
    rbtmath_mesh_bvh bvh = nullptr;
    CHECK(rbtmath_mesh_bvh_create(&desc, &bvh) == RBT_RESULT_OK);
    return bvh;
}

static void test_bvh_reads()
{
    rbtmath_mesh_bvh cube = make_cube_bvh();
    if (!cube)
        return;
    enum
    {
        COUNT = 200
    };
    static RbtDMat4 matrices[COUNT];
    static RBTMATH_BVH_INSTANCE instances[COUNT];
    static uint32_t indices[COUNT];
    for (uint32_t i = 0; i < COUNT; i++)
    {
        matrices[i] = RbtDMat4{};
        matrices[i].m[0] = matrices[i].m[5] = matrices[i].m[10] = matrices[i].m[15] = 1.0;
        matrices[i].m[12] = (double)(i % 20) * 2.0;
        matrices[i].m[14] = (double)(i / 20) * 2.0;
        instances[i] = RBTMATH_BVH_INSTANCE{cube, &matrices[i], i, 1};
        indices[i] = i;
    }
    RBTMATH_INSTANCE_BVH_DESC desc{};
    desc.base.type = RBT_STRUCT_TYPE_MATH_INSTANCE_BVH_DESC;
    desc.instances = instances;
    desc.instance_count = COUNT;
    rbtmath_instance_bvh bvh = nullptr;
    CHECK(rbtmath_instance_bvh_create(&desc, &bvh) == RBT_RESULT_OK);
    if (!bvh)
    {
        rbtmath_mesh_bvh_destroy(cube);
        return;
    }
    auto answer = [bvh](int i) {
        uint64_t a = 0;
        RBTMATH_RAY ray = {{(double)(i % 40) + 0.5, 5.0, (double)(i % 20) + 0.5}, {0.0, -1.0, 0.0}, 0.0, 10.0};
        RBTMATH_INSTANCE_HIT hit{};
        a = a * 31 + rbtmath_instance_bvh_cast(bvh, &ray, nullptr, &hit) + hit.instance;
        RBTMATH_SHAPE shape{};
        shape.kind = RBTMATH_SHAPE_KIND_SPHERE;
        shape.center = {(double)(i % 40) + 0.5, 1.2, (double)(i % 20) + 0.5};
        shape.rotation.w = 1.0;
        shape.radius = 0.4;
        uint32_t list[8];
        a = a * 31 + rbtmath_instance_bvh_overlap_all(bvh, &shape, nullptr, list, 8);
        RBTMATH_SWEEP sweep{};
        sweep.shape = shape;
        sweep.shape.center.y = 5.0;
        sweep.direction.y = -1.0;
        sweep.t_max = 10.0;
        RBTMATH_SWEEP_HIT sh{};
        a = a * 31 + rbtmath_instance_bvh_sweep(bvh, &sweep, nullptr, &sh) + sh.instance;
        return a;
    };
    uint64_t expected[40];
    for (int i = 0; i < 40; i++)
        expected[i] = answer(i);
    std::atomic<int> wrong{0};
    auto run = [&]() {
        for (int i = 0; i < 400; i++)
            if (answer(i) != expected[i % 40])
                wrong++;
    };
    std::thread a(run), b(run);
    a.join();
    b.join();
    CHECK(wrong.load() == 0);
    // Disjoint sets from two threads, then one refit: every instance up 10.
    for (uint32_t i = 0; i < COUNT; i++)
        matrices[i].m[13] = 10.0;
    std::atomic<int> refused{0};
    std::thread low([&] {
        if (rbtmath_instance_bvh_set_instances(bvh, indices, instances, COUNT / 2, nullptr) != RBT_RESULT_OK)
            refused++;
    });
    std::thread high([&] {
        if (rbtmath_instance_bvh_set_instances(bvh, indices + COUNT / 2, instances + COUNT / 2, COUNT / 2, nullptr) !=
            RBT_RESULT_OK)
            refused++;
    });
    low.join();
    high.join();
    CHECK(refused.load() == 0);
    rbtmath_instance_bvh_refit(bvh);
    CHECK(rbtmath_instance_bvh_guard(bvh).reports.load() == 0);
    RBTMATH_RAY ray = {{0.5, 20.0, 0.5}, {0.0, -1.0, 0.0}, 0.0, 100.0};
    RBTMATH_INSTANCE_HIT hit{};
    CHECK(rbtmath_instance_bvh_cast(bvh, &ray, nullptr, &hit) && hit.t > 8.9 && hit.t < 9.1);
    rbtmath_instance_bvh_destroy(bvh);
    rbtmath_mesh_bvh_destroy(cube);
}

// Enters a shared guard in [role] and stays until [release] is set.
static void hold_shared(rbt_shared_overlap *guard, rbt_role role, std::atomic<bool> *inside,
                        std::atomic<bool> *release)
{
    RBT_ENTER_SHARED(*guard, role, "test", "hold_shared", "object");
    inside->store(true);
    while (!release->load())
        std::this_thread::yield();
}

// Calls [call] on another thread while [guard] is held in [role]: [expected]
// reports, by the call.
template <typename F>
static void shared_overlap_once(rbt_shared_overlap &guard, rbt_role role, const char *what, uint32_t expected, F call)
{
    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
    std::thread holder(hold_shared, &guard, role, &inside, &release);
    while (!inside.load())
        std::this_thread::yield();
    const uint32_t before = guard.reports.load();
    std::thread caller(call);
    caller.join();
    release.store(true);
    holder.join();
    if (guard.reports.load() != before + expected)
        printf("FAIL: %s: %u overlap report(s), %u expected\n", what, guard.reports.load() - before, expected);
    CHECK(guard.reports.load() == before + expected);
}

// Calls [call] on another thread while [guard] is held: one overlap report,
// by the call, if it opens with RBTREND_ENTER on that guard.
template <typename F>
static void overlap_once(rbt_overlap &guard, const char *what, F call)
{
    std::atomic<bool> inside{false};
    std::atomic<bool> release{false};
    std::thread holder(hold, &guard, &inside, &release);
    while (!inside.load())
        std::this_thread::yield();
    const uint32_t before = guard.reports.load();
    std::thread caller(call);
    caller.join();
    release.store(true);
    holder.join();
    if (guard.reports.load() != before + 1)
        printf("FAIL: %s: no overlap reported\n", what);
    CHECK(guard.reports.load() == before + 1);
}

int main()
{
    test_volume_reads();
    test_bvh_reads();
#if !_DEBUG
    printf("rbtoverlap_tests: Release build, which checks no overlaps: those skipped\n");
    if (failures)
    {
        printf("rbtoverlap_tests: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
#else
    // Two threads inside one object at once: one report, by the second.
    {
        rbt_overlap guard;
        std::atomic<bool> inside{false};
        std::atomic<bool> release{false};
        std::thread holder(hold, &guard, &inside, &release);
        while (!inside.load())
            std::this_thread::yield();
        printf("rbtoverlap_tests: one overlap report is expected next\n");
        fflush(stdout);
        std::thread second(enter_once, &guard);
        second.join();
        release.store(true);
        holder.join();
        CHECK(guard.reports.load() == 1);
        // Left empty: a later call from any thread is no overlap.
        std::thread later(enter_once, &guard);
        later.join();
        CHECK(guard.reports.load() == 1);
    }
    // The same thread entering again, as a guarded call making another.
    {
        rbt_overlap guard;
        {
            RBT_ENTER(guard, "test", "outer", "object");
            RBT_ENTER(guard, "test", "inner", "object");
            enter_once(&guard);
        }
        CHECK(guard.reports.load() == 0);
        // And empty again after: another thread enters unreported.
        std::thread other(enter_once, &guard);
        other.join();
        CHECK(guard.reports.load() == 0);
    }
    // One call after another, each on its own thread: no overlap.
    {
        rbt_overlap guard;
        for (int i = 0; i < 50; ++i)
        {
            std::thread t(enter_once, &guard);
            t.join();
        }
        CHECK(guard.reports.load() == 0);
    }
    // Two objects, a thread inside each at once: independent.
    {
        rbt_overlap a;
        rbt_overlap b;
        std::atomic<bool> inside_a{false};
        std::atomic<bool> inside_b{false};
        std::atomic<bool> release{false};
        std::thread ta(hold, &a, &inside_a, &release);
        std::thread tb(hold, &b, &inside_b, &release);
        while (!inside_a.load() || !inside_b.load())
            std::this_thread::yield();
        release.store(true);
        ta.join();
        tb.join();
        CHECK(a.reports.load() == 0);
        CHECK(b.reports.load() == 0);
    }
    // Rabbittus Math's instance and box BVHs (rbtmathbvh.h, "Threads"): queries
    // beside queries and sets beside sets go unreported; a cast beside a set,
    // a set beside a cast, and a refit or rebuild beside anything, reported.
    {
        rbtmath_mesh_bvh cube = make_cube_bvh();
        RbtDMat4 m{};
        m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0;
        RBTMATH_BVH_INSTANCE instance = {cube, &m, 1, 1};
        RBTMATH_INSTANCE_BVH_DESC desc{};
        desc.base.type = RBT_STRUCT_TYPE_MATH_INSTANCE_BVH_DESC;
        desc.instances = &instance;
        desc.instance_count = 1;
        rbtmath_instance_bvh bvh = nullptr;
        CHECK(rbtmath_instance_bvh_create(&desc, &bvh) == RBT_RESULT_OK);
        RBTMATH_BVH_BOX box = {{{0, 0, 0}, {1, 1, 1}}, 1, 1};
        RBTMATH_BOX_BVH_DESC box_desc{};
        box_desc.base.type = RBT_STRUCT_TYPE_MATH_BOX_BVH_DESC;
        box_desc.boxes = &box;
        box_desc.box_count = 1;
        rbtmath_box_bvh boxes = nullptr;
        CHECK(rbtmath_box_bvh_create(&box_desc, &boxes, nullptr) == RBT_RESULT_OK);
        if (bvh && boxes)
        {
            rbt_shared_overlap &guard = rbtmath_instance_bvh_guard(bvh);
            rbt_shared_overlap &box_guard = rbtmath_box_bvh_guard(boxes);
            RBTMATH_RAY ray = {{0.5, 5.0, 0.5}, {0.0, -1.0, 0.0}, 0.0, 10.0};
            uint32_t zero = 0;
            shared_overlap_once(guard, RBT_ROLE_READ, "a cast beside a cast", 0,
                                [&] { (void)rbtmath_instance_bvh_cast(bvh, &ray, nullptr, nullptr); });
            shared_overlap_once(guard, RBT_ROLE_SHARE, "a set beside a set", 0,
                                [&] { (void)rbtmath_instance_bvh_set_instances(bvh, &zero, &instance, 1, nullptr); });
            printf("rbtoverlap_tests: six overlap reports are expected next\n");
            fflush(stdout);
            shared_overlap_once(guard, RBT_ROLE_SHARE, "a cast beside a set", 1,
                                [&] { (void)rbtmath_instance_bvh_cast(bvh, &ray, nullptr, nullptr); });
            shared_overlap_once(guard, RBT_ROLE_READ, "a set beside a cast", 1,
                                [&] { (void)rbtmath_instance_bvh_set_instances(bvh, &zero, &instance, 1, nullptr); });
            shared_overlap_once(guard, RBT_ROLE_READ, "a refit beside a cast", 1, [&] { rbtmath_instance_bvh_refit(bvh); });
            shared_overlap_once(guard, RBT_ROLE_SHARE, "a rebuild beside a set", 1,
                                [&] { rbtmath_instance_bvh_rebuild(bvh); });
            shared_overlap_once(box_guard, RBT_ROLE_SHARE, "a box cast beside a set", 1,
                                [&] { (void)rbtmath_box_bvh_cast(boxes, &ray, nullptr, nullptr); });
            rbtmath_instance_bvh_refit(bvh);
        }
        rbtmath_box_bvh_destroy(boxes);
        rbtmath_instance_bvh_destroy(bvh);
        rbtmath_mesh_bvh_destroy(cube);
    }
    // A mixer's control calls (rbtaudio.h, "Threads"): an update while
    // another control call is inside is reported; the underrun count, an
    // "any" call, isn't.
    {
        const CRAUDIO_CHANNEL_POSITION stereo[2] = {CRAUDIO_CHANNEL_POSITION_FL, CRAUDIO_CHANNEL_POSITION_FR};
        RBTAUDIO_MIXER_DESC desc{};
        desc.base.type = RBT_STRUCT_TYPE_AUDIO_MIXER_DESC;
        desc.format.sample_format = CRAUDIO_SAMPLE_FORMAT_F32;
        desc.format.sample_rate = 48000;
        desc.format.channel_count = 2;
        desc.format.interleaved = true;
        desc.format.positions = stereo;
        desc.max_frames = 256;
        desc.max_voices = 4;
        desc.max_real_voices = 2;
        desc.max_sounds = 2;
        desc.queue_capacity = 4;
        desc.resampler = RBTAUDIO_RESAMPLER_LINEAR;
        desc.limiter = RBTAUDIO_LIMITER_OFF;
        desc.gain = 1.0f;
        rbtaudio_mixer mixer = nullptr;
        CHECK(rbtaudio_mixer_create(&desc, &mixer, nullptr) == RBT_RESULT_OK);
        if (mixer)
        {
            rbt_overlap &guard = rbtaudio_mixer_control_guard(mixer);
            std::atomic<bool> inside{false};
            std::atomic<bool> release{false};
            std::thread holder(hold, &guard, &inside, &release);
            while (!inside.load())
                std::this_thread::yield();
            std::thread reader([mixer] { (void)rbtaudio_sound_get_underrun_frames(mixer, 0); });
            reader.join();
            CHECK(guard.reports.load() == 0);
            printf("rbtoverlap_tests: one overlap report is expected next\n");
            fflush(stdout);
            std::thread updater([mixer] { (void)rbtaudio_mixer_update(mixer, nullptr); });
            updater.join();
            release.store(true);
            holder.join();
            CHECK(guard.reports.load() == 1);
            rbtaudio_mixer_destroy(mixer);
        }
    }
    // Rend's ray tracing (rbtrendraytracing.h, "Threads"): each new call
    // opens with the overlap check on the object its header names: a BLAS's
    // and a TLAS's own calls on it, creating either and setting a light's
    // shadow on the instance. Each call is made with an argument it refuses
    // right after the check, so nothing past it runs: an instance with no
    // device and no features, a BLAS and a TLAS of it never made by a create.
    {
        rbtrend_instance_t instance;
        rbtrend_blas_t blas;
        blas.instance = &instance;
        rbtrend_tlas_t tlas;
        tlas.instance = &instance;
        rbtrend_lit_scene_t scene;
        scene.instance = &instance;
        printf("rbtoverlap_tests: nine overlap reports are expected next, one for each of Rend's ray tracing calls\n");
        fflush(stdout);
        overlap_once(instance.entered, "rbtrend_blas_create", [&] { (void)rbtrend_blas_create(&instance, nullptr, nullptr, nullptr); });
        overlap_once(instance.entered, "rbtrend_tlas_create", [&] { (void)rbtrend_tlas_create(&instance, nullptr, nullptr, nullptr); });
        overlap_once(blas.entered, "rbtrend_blas_get_info", [&] { (void)rbtrend_blas_get_info(&blas, nullptr, nullptr); });
        overlap_once(tlas.entered, "rbtrend_tlas_get_info", [&] { (void)rbtrend_tlas_get_info(&tlas, nullptr, nullptr); });
        overlap_once(tlas.entered, "rbtrend_tlas_write_instances", [&] { (void)rbtrend_tlas_write_instances(&tlas, 0, nullptr, 0, nullptr); });
        overlap_once(tlas.entered, "rbtrend_tlas_set_instance_count", [&] { (void)rbtrend_tlas_set_instance_count(&tlas, 0, nullptr); });
        overlap_once(tlas.entered, "rbtrend_tlas_set_materials", [&] { (void)rbtrend_tlas_set_materials(&tlas, 0, nullptr, 0, nullptr); });
        overlap_once(instance.entered, "rbtrend_lit_scene_set_light_shadow", [&] { (void)rbtrend_lit_scene_set_light_shadow(&scene, 1, nullptr, nullptr); });
        // A getter that takes no object's state is no exception: the slot
        // reads only the BLAS, and is checked too.
        overlap_once(blas.entered, "rbtrend_blas_get_slot", [&] { (void)rbtrend_blas_get_slot(&blas); });
    }
    if (failures)
    {
        printf("rbtoverlap_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("rbtoverlap_tests: all passed\n");
    return 0;
#endif
}
