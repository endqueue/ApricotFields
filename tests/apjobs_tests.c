// Headless tests for ApJobs (include/jobs/apjobs.h): the contract, as the
// implementation has to meet it.
//
// Written in C on purpose: apjobs.h is a C front end, and this proves it
// compiles and links as one.
//
// Written before the implementation (card L12) and not to be edited by it.
// A call that returns APRESULT_NOT_IMPLEMENTED marks its test pending and
// ends it; anything else is checked. Every task here writes only its own
// items' outputs, so a plain array is race-free when the scheduler keeps
// the contract (each item in one range) and a race ThreadSanitizer reports
// when it doesn't.

#include "jobs/apjobs.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static const char *current_test = "";
static bool test_pending = false;

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Marks the current test pending if [result] is APRESULT_NOT_IMPLEMENTED,
// and says whether it is.
static bool pending(APRESULT result)
{
    if (result != APRESULT_NOT_IMPLEMENTED)
        return false;
    if (!test_pending)
        printf("PENDING %s: not implemented yet\n", current_test);
    test_pending = true;
    return true;
}

#define MAX_ITEMS 100000
#define MAX_WORKERS (APJOBS_POOL_MAX_THREADS + 1)

// What a task records: how many calls covered each item, which worker ran
// it, each range's size, and a value per item whose sum is checked.
typedef struct Recorder
{
    uint32_t item_count;
    uint32_t min_range;
    uint32_t worker_count;
    uint8_t *hits;
    uint32_t *worker;
    uint64_t *value;
    // Per worker: a range in progress, and how many it ran. Indexed by the
    // worker, so the scheduler's one-range-at-a-time rule keeps it unshared.
    uint8_t busy[MAX_WORKERS];
    uint32_t ranges[MAX_WORKERS];
    // A range shorter than min_range that isn't the last one.
    uint8_t short_range[MAX_WORKERS];
    uint8_t overlap[MAX_WORKERS];
    uint8_t bad_worker;
} Recorder;

static void record(uint32_t first_item, uint32_t end_item, uint32_t worker_index, void *task_data)
{
    Recorder *r = (Recorder *)task_data;
    if (worker_index >= r->worker_count || worker_index >= MAX_WORKERS)
    {
        // Written by any thread that gets here; only ever set to 1.
        r->bad_worker = 1;
        return;
    }
    if (r->busy[worker_index])
        r->overlap[worker_index] = 1;
    r->busy[worker_index] = 1;
    if (end_item - first_item < r->min_range && end_item != r->item_count)
        r->short_range[worker_index] = 1;
    for (uint32_t i = first_item; i < end_item && i < r->item_count; ++i)
    {
        r->hits[i]++;
        r->worker[i] = worker_index;
        // Some work per item, so ranges overlap in time.
        uint64_t x = i;
        for (int k = 0; k < 20; ++k)
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        r->value[i] = x;
    }
    r->ranges[worker_index]++;
    r->busy[worker_index] = 0;
}

static Recorder *recorder_create(uint32_t item_count, uint32_t min_range, uint32_t worker_count)
{
    Recorder *r = (Recorder *)calloc(1, sizeof(Recorder));
    r->item_count = item_count;
    r->min_range = min_range;
    r->worker_count = worker_count;
    r->hits = (uint8_t *)calloc(item_count ? item_count : 1, 1);
    r->worker = (uint32_t *)calloc(item_count ? item_count : 1, sizeof(uint32_t));
    r->value = (uint64_t *)calloc(item_count ? item_count : 1, sizeof(uint64_t));
    return r;
}

static void recorder_destroy(Recorder *r)
{
    free(r->hits);
    free(r->worker);
    free(r->value);
    free(r);
}

// The sum a serial run gives, to compare with: the same inputs give the
// same result however the items were split.
static uint64_t expected_sum(uint32_t item_count)
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < item_count; ++i)
    {
        uint64_t x = i;
        for (int k = 0; k < 20; ++k)
            x = x * 6364136223846793005ull + 1442695040888963407ull;
        sum += x;
    }
    return sum;
}

// Every item covered once, every range on a valid worker, none shorter than
// min_range but the last, no worker running two ranges at once, and the
// merged result the serial one's. [used_workers] receives how many workers
// ran a range.
static void check_recorder(const Recorder *r, uint32_t *used_workers)
{
    bool once = true;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < r->item_count; ++i)
    {
        if (r->hits[i] != 1)
            once = false;
        sum += r->value[i];
    }
    CHECK(once);
    CHECK(!r->bad_worker);
    CHECK(sum == expected_sum(r->item_count));
    uint32_t used = 0;
    for (uint32_t w = 0; w < MAX_WORKERS; ++w)
    {
        CHECK(!r->short_range[w]);
        CHECK(!r->overlap[w]);
        if (r->ranges[w] > 0)
            ++used;
    }
    if (used_workers)
        *used_workers = used;
}

static APJOBS_POOL_DESC pool_desc(uint32_t thread_count, uint32_t max_tasks)
{
    APJOBS_POOL_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = APRI_STRUCT_TYPE_APJOBS_POOL_DESC;
    desc.thread_count = thread_count;
    desc.max_tasks = max_tasks;
    desc.debug_name = "apjobs_tests";
    return desc;
}

// ---- The scheduler, checked ----

static void *never_enqueue(APJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t min_range, void *task_data, void *user)
{
    (void)function;
    (void)item_count;
    (void)min_range;
    (void)task_data;
    (void)user;
    return NULL;
}

static void never_wait(void *task, void *user)
{
    (void)task;
    (void)user;
}

static void test_scheduler_check(void)
{
    current_test = "scheduler_check";
    test_pending = false;
    APJOBS_SCHEDULER s;
    memset(&s, 0, sizeof(s));
    s.base.type = APRI_STRUCT_TYPE_APJOBS_SCHEDULER;
    s.worker_count = 1;
    s.enqueue = never_enqueue;
    s.wait = never_wait;
    APJOBS_ERROR error;
    APRESULT result = apjobs_scheduler_check(&s, &error);
    if (pending(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(error.message[0] == '\0');

    CHECK(apjobs_scheduler_check(NULL, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "scheduler is NULL") == 0);
    APJOBS_SCHEDULER bad = s;
    bad.worker_count = 0;
    CHECK(apjobs_scheduler_check(&bad, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "scheduler's worker_count is 0") == 0);
    bad = s;
    bad.enqueue = NULL;
    CHECK(apjobs_scheduler_check(&bad, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "scheduler's enqueue is NULL") == 0);
    bad = s;
    bad.wait = NULL;
    CHECK(apjobs_scheduler_check(&bad, NULL) == APRESULT_INVALID_ARGUMENT);
    bad = s;
    bad.base.type = APRI_STRUCT_TYPE_APJOBS_POOL_DESC;
    CHECK(apjobs_scheduler_check(&bad, &error) == APRESULT_STRUCT_WRONG_TYPE);
    bad = s;
    bad.base.next = &s.base;
    CHECK(apjobs_scheduler_check(&bad, &error) == APRESULT_STRUCT_UNKNOWN_IN_CHAIN);
}

// ---- apjobs_run with no scheduler: serial, worker 0, in order ----

static void test_run_serial(void)
{
    current_test = "run_serial";
    test_pending = false;
    Recorder *r = recorder_create(1000, 7, 1);
    APRESULT result = apjobs_run(NULL, record, 1000, 7, r);
    if (pending(result))
    {
        recorder_destroy(r);
        return;
    }
    CHECK(result == APRESULT_OK);
    uint32_t used = 0;
    check_recorder(r, &used);
    CHECK(used == 1);
    CHECK(r->ranges[0] >= 1);
    recorder_destroy(r);

    // Nothing to do, and refusals that run nothing.
    r = recorder_create(10, 1, 1);
    CHECK(apjobs_run(NULL, record, 0, 1, r) == APRESULT_OK);
    CHECK(r->ranges[0] == 0);
    CHECK(apjobs_run(NULL, NULL, 10, 1, r) == APRESULT_INVALID_ARGUMENT);
    CHECK(apjobs_run(NULL, record, 10, 0, r) == APRESULT_INVALID_ARGUMENT);
    CHECK(r->ranges[0] == 0);
    recorder_destroy(r);
}

// ---- The pool ----

static void test_pool_refusals(void)
{
    current_test = "pool_refusals";
    test_pending = false;
    APJOBS_ERROR error;
    apjobs_pool pool = (apjobs_pool)(uintptr_t)0x20;
    APJOBS_POOL_DESC desc = pool_desc(2, 0);
    APRESULT result = apjobs_pool_create(&desc, &pool, &error);
    if (pending(result))
        return;
    CHECK(result == APRESULT_INVALID_ARGUMENT);
    CHECK(pool == NULL);
    CHECK(strcmp(error.message, "max_tasks is 0") == 0);
    desc = pool_desc(APJOBS_POOL_MAX_THREADS + 1, 4);
    CHECK(apjobs_pool_create(&desc, &pool, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "thread_count 257 is above APJOBS_POOL_MAX_THREADS (256)") == 0);
    CHECK(apjobs_pool_create(NULL, &pool, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(strcmp(error.message, "desc is NULL") == 0);
    desc = pool_desc(1, 4);
    CHECK(apjobs_pool_create(&desc, NULL, NULL) == APRESULT_INVALID_ARGUMENT);
    desc.base.type = APRI_STRUCT_TYPE_APJOBS_SCHEDULER;
    CHECK(apjobs_pool_create(&desc, &pool, &error) == APRESULT_STRUCT_WRONG_TYPE);
    CHECK(pool == NULL);
    apjobs_pool_destroy(NULL);
    APJOBS_SCHEDULER s;
    CHECK(apjobs_pool_get_scheduler(NULL, &s) == APRESULT_INVALID_ARGUMENT);
    CHECK(apjobs_get_processor_count() >= 1);
}

// One task of [item_count] items through a pool of [thread_count] threads;
// every item once, the result the serial one's, and the scheduler as the
// header says.
static void run_on_pool(uint32_t thread_count, uint32_t item_count, uint32_t min_range)
{
    APJOBS_POOL_DESC desc = pool_desc(thread_count, 8);
    apjobs_pool pool = NULL;
    APRESULT result = apjobs_pool_create(&desc, &pool, NULL);
    if (pending(result))
        return;
    CHECK(result == APRESULT_OK);
    CHECK(pool != NULL);
    if (!pool)
        return;
    APJOBS_SCHEDULER s;
    memset(&s, 0xFF, sizeof(s));
    CHECK(apjobs_pool_get_scheduler(pool, &s) == APRESULT_OK);
    CHECK(s.base.type == APRI_STRUCT_TYPE_APJOBS_SCHEDULER);
    CHECK(s.base.next == NULL);
    CHECK(s.worker_count == thread_count + 1);
    CHECK(s.user == (void *)pool);
    CHECK(apjobs_scheduler_check(&s, NULL) == APRESULT_OK);

    Recorder *r = recorder_create(item_count, min_range, s.worker_count);
    CHECK(apjobs_run(&s, record, item_count, min_range, r) == APRESULT_OK);
    uint32_t used = 0;
    check_recorder(r, &used);
    // How the work was spread is the scheduler's; said, not checked.
    printf("  %s: %u items on %u thread(s): %u worker(s) ran ranges\n", current_test, item_count, thread_count, used);
    recorder_destroy(r);
    apjobs_pool_destroy(pool);
}

static void test_pool_runs(void)
{
    current_test = "pool_runs";
    test_pending = false;
    // No threads: every range in the wait, as worker 0.
    run_on_pool(0, 5000, 16);
    if (test_pending)
        return;
    run_on_pool(1, 5000, 16);
    run_on_pool(4, MAX_ITEMS, 64);
    // Fewer items than a range, and one item.
    run_on_pool(4, 10, 64);
    run_on_pool(4, 1, 1);
    // Ranges of one item.
    run_on_pool(3, 2000, 1);
}

// Two tasks in flight at once, waited for in the other order; then more
// tasks than max_tasks, the ones past it run in enqueue (handle NULL).
static void test_pool_in_flight(void)
{
    current_test = "pool_in_flight";
    test_pending = false;
    APJOBS_POOL_DESC desc = pool_desc(3, 2);
    apjobs_pool pool = NULL;
    APRESULT result = apjobs_pool_create(&desc, &pool, NULL);
    if (pending(result))
        return;
    CHECK(result == APRESULT_OK);
    if (!pool)
        return;
    APJOBS_SCHEDULER s;
    CHECK(apjobs_pool_get_scheduler(pool, &s) == APRESULT_OK);

    Recorder *a = recorder_create(20000, 32, s.worker_count);
    Recorder *b = recorder_create(30000, 16, s.worker_count);
    Recorder *c = recorder_create(500, 8, s.worker_count);
    void *ta = s.enqueue(record, a->item_count, a->min_range, a, s.user);
    void *tb = s.enqueue(record, b->item_count, b->min_range, b, s.user);
    // A third, past max_tasks: run before enqueue returns.
    void *tc = s.enqueue(record, c->item_count, c->min_range, c, s.user);
    CHECK(tc == NULL);
    check_recorder(c, NULL);
    if (tb)
        s.wait(tb, s.user);
    check_recorder(b, NULL);
    if (ta)
        s.wait(ta, s.user);
    check_recorder(a, NULL);

    // A slot freed by a wait is taken again.
    Recorder *d = recorder_create(4000, 4, s.worker_count);
    void *td = s.enqueue(record, d->item_count, d->min_range, d, s.user);
    if (td)
        s.wait(td, s.user);
    check_recorder(d, NULL);

    recorder_destroy(a);
    recorder_destroy(b);
    recorder_destroy(c);
    recorder_destroy(d);
    apjobs_pool_destroy(pool);
}

// Many short tasks one after another on one pool: what a frame loop does.
static void test_pool_many_tasks(void)
{
    current_test = "pool_many_tasks";
    test_pending = false;
    APJOBS_POOL_DESC desc = pool_desc(4, 4);
    apjobs_pool pool = NULL;
    APRESULT result = apjobs_pool_create(&desc, &pool, NULL);
    if (pending(result))
        return;
    CHECK(result == APRESULT_OK);
    if (!pool)
        return;
    APJOBS_SCHEDULER s;
    CHECK(apjobs_pool_get_scheduler(pool, &s) == APRESULT_OK);
    for (int round = 0; round < 500; ++round)
    {
        Recorder *r = recorder_create(64 + (uint32_t)round, 4, s.worker_count);
        CHECK(apjobs_run(&s, record, r->item_count, r->min_range, r) == APRESULT_OK);
        check_recorder(r, NULL);
        recorder_destroy(r);
    }
    apjobs_pool_destroy(pool);
    // A pool made and destroyed with no task ever run.
    CHECK(apjobs_pool_create(&desc, &pool, NULL) == APRESULT_OK);
    apjobs_pool_destroy(pool);
}

int main(void)
{
    test_scheduler_check();
    test_run_serial();
    test_pool_refusals();
    test_pool_runs();
    test_pool_in_flight();
    test_pool_many_tasks();
    if (failures)
    {
        printf("apjobs_tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("apjobs_tests: all passed\n");
    return 0;
}
