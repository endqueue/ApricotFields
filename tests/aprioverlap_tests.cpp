// Tests of the Debug builds' overlap check every module uses
// (src/aprioverlap.hpp; apricore.h, "Threads"): a second thread entering
// while one is inside is reported, and nothing else is - not the same
// thread entering again, not a call from another thread once the last has
// returned, not two objects at once. Release builds check nothing, and this
// test says so and passes.
//
// Before those, in every build so ThreadSanitizer sees it: reads a header
// lets overlap, from two threads at once.

#include "../src/aprioverlap.hpp"
#include "../src/audio/apaudio_internal.hpp"
#include "math/apmathbvh.h"

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
static void hold(apri_overlap *guard, std::atomic<bool> *inside, std::atomic<bool> *release)
{
    APRI_ENTER(*guard, "test", "hold", "object");
    inside->store(true);
    while (!release->load())
        std::this_thread::yield();
}

static void enter_once(apri_overlap *guard)
{
    APRI_ENTER(*guard, "test", "enter_once", "object");
}

// ApMath's volume tests and casts on one mesh BVH from two threads at once
// (apmathbvh.h, "Threads"): each gets the answers the main thread got.
static void test_volume_reads()
{
    const ApriDVec3 positions[8] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    const uint32_t indices[36] = {0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4,
                                  2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};
    APMATH_MESH_BVH_DESC desc{};
    desc.base.type = APRI_STRUCT_TYPE_APMATH_MESH_BVH_DESC;
    desc.positions = positions;
    desc.position_count = 8;
    desc.indices = indices;
    desc.index_count = 36;
    apmath_mesh_bvh bvh = nullptr;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
    if (!bvh)
        return;
    // x >= edge, for edges across and beside the cube.
    auto answers = [bvh](int step, bool &overlaps, bool &within, bool &hit) {
        APMATH_PLANE plane = {{1, 0, 0}, -(double)(step % 7) * 0.25};
        APMATH_VOLUME volume = {&plane, 1, nullptr, 0};
        overlaps = apmath_mesh_bvh_overlaps_volume(bvh, nullptr, &volume);
        within = apmath_mesh_bvh_within_volume(bvh, nullptr, &volume);
        APMATH_RAY ray = {{0.5, 0.5, 5.0}, {0.0, 0.0, -1.0}, 0.0, 10.0};
        hit = apmath_mesh_bvh_cast(bvh, &ray, nullptr);
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
    apmath_mesh_bvh_destroy(bvh);
}

int main()
{
    test_volume_reads();
#if !_DEBUG
    printf("aprioverlap_tests: Release build, which checks no overlaps: those skipped\n");
    if (failures)
    {
        printf("aprioverlap_tests: %d failure(s)\n", failures);
        return 1;
    }
    return 0;
#else
    // Two threads inside one object at once: one report, by the second.
    {
        apri_overlap guard;
        std::atomic<bool> inside{false};
        std::atomic<bool> release{false};
        std::thread holder(hold, &guard, &inside, &release);
        while (!inside.load())
            std::this_thread::yield();
        printf("aprioverlap_tests: one overlap report is expected next\n");
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
        apri_overlap guard;
        {
            APRI_ENTER(guard, "test", "outer", "object");
            APRI_ENTER(guard, "test", "inner", "object");
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
        apri_overlap guard;
        for (int i = 0; i < 50; ++i)
        {
            std::thread t(enter_once, &guard);
            t.join();
        }
        CHECK(guard.reports.load() == 0);
    }
    // Two objects, a thread inside each at once: independent.
    {
        apri_overlap a;
        apri_overlap b;
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
    // A mixer's control calls (apaudio.h, "Threads"): an update while
    // another control call is inside is reported; the underrun count, an
    // "any" call, isn't.
    {
        const SPUDAUDIO_CHANNEL_POSITION stereo[2] = {SPUDAUDIO_CHANNEL_POSITION_FL, SPUDAUDIO_CHANNEL_POSITION_FR};
        APAUDIO_MIXER_DESC desc{};
        desc.base.type = APRI_STRUCT_TYPE_APAUDIO_MIXER_DESC;
        desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
        desc.format.sample_rate = 48000;
        desc.format.channel_count = 2;
        desc.format.interleaved = true;
        desc.format.positions = stereo;
        desc.max_frames = 256;
        desc.max_voices = 4;
        desc.max_real_voices = 2;
        desc.max_sounds = 2;
        desc.queue_capacity = 4;
        desc.resampler = APAUDIO_RESAMPLER_LINEAR;
        desc.limiter = APAUDIO_LIMITER_OFF;
        desc.gain = 1.0f;
        apaudio_mixer mixer = nullptr;
        CHECK(apaudio_mixer_create(&desc, &mixer, nullptr) == APRESULT_OK);
        if (mixer)
        {
            apri_overlap &guard = apaudio_mixer_control_guard(mixer);
            std::atomic<bool> inside{false};
            std::atomic<bool> release{false};
            std::thread holder(hold, &guard, &inside, &release);
            while (!inside.load())
                std::this_thread::yield();
            std::thread reader([mixer] { (void)apaudio_sound_get_underrun_frames(mixer, 0); });
            reader.join();
            CHECK(guard.reports.load() == 0);
            printf("aprioverlap_tests: one overlap report is expected next\n");
            fflush(stdout);
            std::thread updater([mixer] { (void)apaudio_mixer_update(mixer, nullptr); });
            updater.join();
            release.store(true);
            holder.join();
            CHECK(guard.reports.load() == 1);
            apaudio_mixer_destroy(mixer);
        }
    }
    if (failures)
    {
        printf("aprioverlap_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("aprioverlap_tests: all passed\n");
    return 0;
#endif
}
