// Tests of the Debug builds' overlap check every module uses
// (src/aprioverlap.hpp; apricore.h, "Threads"): a second thread entering
// while one is inside is reported, and nothing else is - not the same
// thread entering again, not a call from another thread once the last has
// returned, not two objects at once. Release builds check nothing, and this
// test says so and passes.

#include "../src/aprioverlap.hpp"

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

int main()
{
#if !_DEBUG
    printf("aprioverlap_tests: Release build, which checks nothing: skipped\n");
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
    if (failures)
    {
        printf("aprioverlap_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("aprioverlap_tests: all passed\n");
    return 0;
#endif
}
