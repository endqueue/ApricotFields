#include "jobs/apjobs.h"
#include "../apriplaindata.hpp"
#include "../aprioverlap.hpp"
#include "../apristale.hpp"
#include "../apristructbase.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system_error>
#include <thread>

// The reference pool (apjobs.h). One mutex guards which tasks are in flight
// and who is inside them; the items of a task are claimed range by range
// with atomics, so running a task takes the lock only to find it, to leave
// it, and once to say its last range has been claimed.
//
// Groups (APJOBS_SCHEDULER_GROUPS): a task enqueued into a group takes a
// slot as any task does, its slot naming the group. It has no wait of its
// own, so whoever leaves it last, once its last range has run, frees the
// slot and only then lowers the group's count; the group's wait runs
// ranges of its group's tasks and sleeps on `finished` until the count is
// 0 or it has a range to claim again. Nothing touches a group once its
// count is 0: whoever lowered it there holds the mutex while it notifies,
// and the wait reads the count under it.

namespace
{

// [result], with [error]'s message made from [format].
APRESULT apjobs_fail(APJOBS_ERROR *error, APRESULT result, const char *format, ...)
{
    if (error)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return result;
}

void apjobs_error_clear(APJOBS_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// One task in flight. Its function, data, count and range size are written
// by enqueue under the pool's mutex while the slot is free, and only read
// while it is in use; next_item and done_items are reached through
// std::atomic_ref while it runs.
struct apjobs_task_slot
{
    APJOBS_TASK_FUNCTION function;
    void *task_data;
    uint32_t item_count;
    uint32_t range_size;
    // The first item no range has claimed yet; claims add range_size, and
    // may pass item_count, which is why it is 64 bits.
    uint64_t next_item;
    // Items whose range has run.
    uint64_t done_items;
    // The group the task was enqueued into, or null for a plain task.
    // Written with the rest while the slot is free.
    struct apjobs_group_t *group;
    // When it was enqueued, counted by the pool: a group's wait takes the
    // oldest of its tasks first. Under the mutex.
    uint64_t sequence;
    // From enqueue until its wait returns, or for a group's task until its
    // last worker leaves. Under the mutex.
    uint32_t in_use;
    // Workers between taking the task and leaving it, a group's waiting
    // thread included. Under the mutex. A slot is freed only when none is
    // inside, so a worker never claims items of a later task that took the
    // slot over.
    uint32_t inside;
};
APRI_PLAIN_DATA(apjobs_task_slot);

// A group: the count of its tasks enqueued and not ended, on a cache line
// of its own (the pool's threads lower it while its wait reads it, and
// nothing else is on it).
struct apjobs_group_t
{
    char before[64];
    // Raised under the pool's mutex by group_enqueue before the task is
    // visible, lowered under it by the task's last worker, read by the wait
    // under it.
    uint64_t pending;
    char after[64 - sizeof(uint64_t)];
};
APRI_PLAIN_DATA(apjobs_group_t);

} // namespace

typedef struct apjobs_pool_t
{
    char *debug_name{nullptr};
    APRI_STALE_TAG
    apjobs_pool_t() = default;
    ~apjobs_pool_t();
    std::mutex mutex{};
    // Workers wait here for a task with items to claim, or for the stop.
    std::condition_variable work{};
    // A wait waits here for its task's last range and last worker.
    std::condition_variable finished{};
    apjobs_task_slot *slots{nullptr};
    uint32_t slot_count{0};
    // Tasks with items left to claim. Under the mutex.
    uint32_t open_tasks{0};
    // The sequence the next task enqueued is given. Under the mutex.
    uint64_t next_sequence{0};
    bool stopping{false};
    // The threads started, thread_count of them once creation succeeds.
    std::thread *threads{nullptr};
    uint32_t thread_count{0};
    // The desc's debug_name, the pool's own copy.
    char *name{nullptr};
    // The groups struct the scheduler's base.next points at.
    APJOBS_SCHEDULER_GROUPS groups{};
    // Debug builds' check of apricore.h's "Threads" rule (../aprioverlap.hpp).
    apri_overlap overlap{};
} apjobs_pool_t;

namespace
{

// Claims and runs ranges of [slot] as [worker] until none is left to claim.
// The claimer of the last range closes the task to new workers.
void apjobs_run_ranges(apjobs_pool_t *pool, apjobs_task_slot *slot, APJOBS_TASK_FUNCTION function, void *task_data,
                       uint32_t item_count, uint32_t range_size, uint32_t worker)
{
    std::atomic_ref<uint64_t> next(slot->next_item);
    std::atomic_ref<uint64_t> done(slot->done_items);
    for (;;)
    {
        const uint64_t first = next.fetch_add(range_size, std::memory_order_relaxed);
        if (first >= item_count)
            return;
        const uint64_t end = first + range_size < item_count ? first + range_size : item_count;
        if (end == item_count)
        {
            // The last range: nothing more to hand out.
            std::lock_guard<std::mutex> lock(pool->mutex);
            --pool->open_tasks;
        }
        function((uint32_t)first, (uint32_t)end, worker, task_data);
        // Release: the range's writes come before the count a wait reads.
        if (done.fetch_add(end - first, std::memory_order_acq_rel) + (end - first) == item_count)
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            pool->finished.notify_all();
        }
    }
}

// Leaves [slot], the pool's mutex held. A group's task, once every range
// has run and nobody is left inside, is freed here and its group's count
// lowered, waking a wait when it reaches 0; after that the group is not
// touched, and the wait reads the count under this same mutex.
void apjobs_leave(apjobs_pool_t *pool, apjobs_task_slot *slot)
{
    --slot->inside;
    if (slot->inside != 0)
        return;
    apjobs_group_t *group = slot->group;
    if (!group)
    {
        // A plain task's wait frees it.
        pool->finished.notify_all();
        return;
    }
    if (std::atomic_ref<uint64_t>(slot->done_items).load(std::memory_order_acquire) != slot->item_count)
        return;
    slot->in_use = 0;
    slot->group  = nullptr;
    --group->pending;
    if (group->pending == 0)
        pool->finished.notify_all();
}

// A worker thread, [worker] of the pool's scheduler (1 and up).
void apjobs_worker(apjobs_pool_t *pool, uint32_t worker)
{
    std::unique_lock<std::mutex> lock(pool->mutex);
    for (;;)
    {
        pool->work.wait(lock, [pool] { return pool->stopping || pool->open_tasks > 0; });
        if (pool->stopping)
            return;
        // A task with items left; open_tasks says one exists.
        apjobs_task_slot *slot = nullptr;
        for (uint32_t i = 0; i < pool->slot_count; ++i)
        {
            apjobs_task_slot &candidate = pool->slots[i];
            if (!candidate.in_use)
                continue;
            if (std::atomic_ref<uint64_t>(candidate.next_item).load(std::memory_order_relaxed) >= candidate.item_count)
                continue;
            slot = &candidate;
            break;
        }
        if (!slot)
        {
            // Its last range was claimed between the count and the look,
            // and open_tasks is about to say so.
            lock.unlock();
            std::this_thread::yield();
            lock.lock();
            continue;
        }
        ++slot->inside;
        const APJOBS_TASK_FUNCTION function = slot->function;
        void *task_data                     = slot->task_data;
        const uint32_t item_count           = slot->item_count;
        const uint32_t range_size           = slot->range_size;
        lock.unlock();
        apjobs_run_ranges(pool, slot, function, task_data, item_count, range_size, worker);
        lock.lock();
        apjobs_leave(pool, slot);
    }
}

// Every range of a task run here, in order, as [worker].
void apjobs_run_inline(APJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t range_size, void *task_data, uint32_t worker)
{
    for (uint64_t first = 0; first < item_count; first += range_size)
    {
        const uint64_t end = first + range_size < item_count ? first + range_size : item_count;
        function((uint32_t)first, (uint32_t)end, worker, task_data);
    }
}

// About four ranges a worker, so a worker that finishes early finds more,
// and none smaller than the caller says is worth a call.
uint32_t apjobs_range_size(const apjobs_pool_t *pool, uint32_t item_count, uint32_t min_range)
{
    const uint64_t workers = (uint64_t)pool->thread_count + 1;
    const uint64_t share   = ((uint64_t)item_count + workers * 4 - 1) / (workers * 4);
    return (uint32_t)(share > min_range ? share : min_range);
}

// A free slot taken for a task, the mutex held; null when every slot is in
// flight.
apjobs_task_slot *apjobs_claim_slot(apjobs_pool_t *pool, APJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t range_size,
                                    void *task_data, apjobs_group_t *group)
{
    for (uint32_t i = 0; i < pool->slot_count; ++i)
    {
        apjobs_task_slot &slot = pool->slots[i];
        if (slot.in_use)
            continue;
        slot.function   = function;
        slot.task_data  = task_data;
        slot.item_count = item_count;
        slot.range_size = range_size;
        slot.next_item  = 0;
        slot.done_items = 0;
        slot.group      = group;
        slot.sequence   = pool->next_sequence++;
        slot.inside     = 0;
        slot.in_use     = 1;
        ++pool->open_tasks;
        return &slot;
    }
    return nullptr;
}

void *apjobs_pool_enqueue(APJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t min_range, void *task_data, void *user)
{
    apjobs_pool_t *pool = (apjobs_pool_t *)user;
    APRI_ENTER(pool->overlap, "apjobs", "the pool's enqueue", "pool");
    // Outside the contract; nothing is run, and nothing needs a wait.
    if (!function)
        return nullptr;
    if (item_count == 0)
        return nullptr;
    if (min_range == 0)
        return nullptr;
    const uint32_t range_size = apjobs_range_size(pool, item_count, min_range);
    // With no thread, a task waits in its slot for the wait to run it.
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        apjobs_task_slot *slot = apjobs_claim_slot(pool, function, item_count, range_size, task_data, nullptr);
        if (slot)
        {
            if (pool->thread_count > 0)
                pool->work.notify_all();
            return slot;
        }
    }
    // Every slot is in flight: run it now.
    apjobs_run_inline(function, item_count, range_size, task_data, 0);
    return nullptr;
}

void apjobs_pool_wait(void *task, void *user)
{
    apjobs_pool_t *pool    = (apjobs_pool_t *)user;
    apjobs_task_slot *slot = (apjobs_task_slot *)task;
    APRI_ENTER(pool->overlap, "apjobs", "the pool's wait", "pool");
    // The task's fields don't change while it is in use, and this is its
    // one caller: read without the lock.
    apjobs_run_ranges(pool, slot, slot->function, slot->task_data, slot->item_count, slot->range_size, 0);
    std::unique_lock<std::mutex> lock(pool->mutex);
    pool->finished.wait(lock, [slot] {
        return std::atomic_ref<uint64_t>(slot->done_items).load(std::memory_order_acquire) == slot->item_count && slot->inside == 0;
    });
    slot->in_use = 0;
}

void *apjobs_pool_group_create(void *user)
{
    apjobs_pool_t *pool = (apjobs_pool_t *)user;
    APRI_ENTER(pool->overlap, "apjobs", "the pool's group_create", "pool");
    // No constructor: zeroed is a group with nothing in it.
    return calloc(1, sizeof(apjobs_group_t));
}

void apjobs_pool_group_destroy(void *group, void *user)
{
    apjobs_pool_t *pool = (apjobs_pool_t *)user;
    APRI_ENTER(pool->overlap, "apjobs", "the pool's group_destroy", "pool");
    free(group);
}

// No overlap check: the one call a task may make, overlapping any other
// (apjobs.h, APJOBS_GROUP_ENQUEUE_FUNCTION).
void apjobs_pool_group_enqueue(void *group_handle, uint32_t worker_index, APJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t min_range,
                               void *task_data, void *user)
{
    apjobs_pool_t *pool   = (apjobs_pool_t *)user;
    apjobs_group_t *group = (apjobs_group_t *)group_handle;
    // Outside the contract; nothing is run.
    if (!group)
        return;
    if (!function)
        return;
    if (item_count == 0)
        return;
    if (min_range == 0)
        return;
    if (worker_index > pool->thread_count)
        return;
    const uint32_t range_size = apjobs_range_size(pool, item_count, min_range);
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        apjobs_task_slot *slot = apjobs_claim_slot(pool, function, item_count, range_size, task_data, group);
        if (slot)
        {
            // Raised before the task is visible to anyone: the mutex is held
            // until both are written.
            ++group->pending;
            if (pool->thread_count > 0)
                pool->work.notify_all();
            // A wait on this group, sleeping with nothing to claim.
            pool->finished.notify_all();
            return;
        }
    }
    // Every slot is in flight: run it here, nested in the caller's range
    // when called from a task, as the index the caller runs as.
    apjobs_run_inline(function, item_count, range_size, task_data, worker_index);
}

// The oldest task of [group] with a range to claim, the mutex held; null if
// none has.
apjobs_task_slot *apjobs_find_group_task(apjobs_pool_t *pool, const apjobs_group_t *group)
{
    apjobs_task_slot *found = nullptr;
    for (uint32_t i = 0; i < pool->slot_count; ++i)
    {
        apjobs_task_slot &slot = pool->slots[i];
        if (!slot.in_use)
            continue;
        if (slot.group != group)
            continue;
        if (std::atomic_ref<uint64_t>(slot.next_item).load(std::memory_order_relaxed) >= slot.item_count)
            continue;
        if (found && found->sequence < slot.sequence)
            continue;
        found = &slot;
    }
    return found;
}

void apjobs_pool_group_wait(void *group_handle, void *user)
{
    apjobs_pool_t *pool   = (apjobs_pool_t *)user;
    apjobs_group_t *group = (apjobs_group_t *)group_handle;
    APRI_ENTER(pool->overlap, "apjobs", "the pool's group_wait", "pool");
    if (!group)
        return;
    std::unique_lock<std::mutex> lock(pool->mutex);
    for (;;)
    {
        if (group->pending == 0)
            return;
        apjobs_task_slot *slot = apjobs_find_group_task(pool, group);
        if (!slot)
        {
            // Every task in flight is being run elsewhere: sleep until one
            // ends or another is enqueued.
            pool->finished.wait(lock, [pool, group] { return group->pending == 0 || apjobs_find_group_task(pool, group) != nullptr; });
            continue;
        }
        // Inside, as the threads are, so the slot isn't freed and taken over
        // while this thread is still in one of its ranges.
        ++slot->inside;
        const APJOBS_TASK_FUNCTION function = slot->function;
        void *task_data                     = slot->task_data;
        const uint32_t item_count           = slot->item_count;
        const uint32_t range_size           = slot->range_size;
        lock.unlock();
        apjobs_run_ranges(pool, slot, function, task_data, item_count, range_size, 0);
        lock.lock();
        apjobs_leave(pool, slot);
    }
}

// Stops and joins the threads started so far.
void apjobs_pool_stop(apjobs_pool_t *pool)
{
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->stopping = true;
    }
    pool->work.notify_all();
    for (uint32_t i = 0; i < pool->thread_count; ++i)
    {
        if (pool->threads[i].joinable())
            pool->threads[i].join();
        pool->threads[i].~thread();
    }
    pool->thread_count = 0;
}

} // namespace

apjobs_pool_t::~apjobs_pool_t()
{
    apjobs_pool_stop(this);
    free(this->threads);
    free(this->slots);
    free(this->name);
}

extern "C"
{

APRESULT apjobs_scheduler_check(const APJOBS_SCHEDULER *scheduler, APJOBS_ERROR *out_error)
{
    apjobs_error_clear(out_error);
    if (!scheduler)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "scheduler is NULL");
    if (scheduler->base.type != APRI_STRUCT_TYPE_APJOBS_SCHEDULER)
        return apjobs_fail(out_error, APRESULT_STRUCT_WRONG_TYPE, "scheduler %s", apri_struct_check_message(APRESULT_STRUCT_WRONG_TYPE));
    // Its chain: the groups struct, once, and nothing else.
    const APJOBS_SCHEDULER_GROUPS *groups = nullptr;
    for (const APRI_STRUCT_BASE *link = scheduler->base.next; link; link = link->next)
    {
        if (link->type != APRI_STRUCT_TYPE_APJOBS_SCHEDULER_GROUPS)
            return apjobs_fail(out_error, APRESULT_STRUCT_UNKNOWN_IN_CHAIN,
                               "scheduler has a struct chained on other than APJOBS_SCHEDULER_GROUPS, the one it takes");
        if (groups)
            return apjobs_fail(out_error, APRESULT_STRUCT_DUPLICATE_IN_CHAIN, "scheduler has APJOBS_SCHEDULER_GROUPS chained on twice");
        groups = (const APJOBS_SCHEDULER_GROUPS *)link;
    }
    if (scheduler->worker_count == 0)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "scheduler's worker_count is 0");
    if (!scheduler->enqueue)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "scheduler's enqueue is NULL");
    if (!scheduler->wait)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "scheduler's wait is NULL");
    if (!groups)
        return APRESULT_OK;
    if (!groups->group_create)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "scheduler's groups' group_create is NULL");
    if (!groups->group_destroy)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "scheduler's groups' group_destroy is NULL");
    if (!groups->group_enqueue)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "scheduler's groups' group_enqueue is NULL");
    if (!groups->group_wait)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "scheduler's groups' group_wait is NULL");
    return APRESULT_OK;
}

APRESULT apjobs_run(const APJOBS_SCHEDULER *scheduler, APJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t min_range, void *task_data)
{
    if (!function)
        return APRESULT_INVALID_ARGUMENT;
    if (min_range == 0)
        return APRESULT_INVALID_ARGUMENT;
    if (item_count == 0)
        return APRESULT_OK;
    if (!scheduler)
    {
        apjobs_run_inline(function, item_count, min_range, task_data, 0);
        return APRESULT_OK;
    }
    void *task = scheduler->enqueue(function, item_count, min_range, task_data, scheduler->user);
    if (task)
        scheduler->wait(task, scheduler->user);
    return APRESULT_OK;
}

uint32_t apjobs_get_processor_count(void)
{
    const unsigned int count = std::thread::hardware_concurrency();
    return count > 0 ? (uint32_t)count : 1;
}

APRESULT apjobs_pool_create(const APJOBS_POOL_DESC *desc, apjobs_pool *out_pool, APJOBS_ERROR *out_error)
{
    apjobs_error_clear(out_error);
    if (out_pool)
        *out_pool = nullptr;
    if (!desc)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "desc is NULL");
    const APRESULT base = apri_struct_check(&desc->base, APRI_STRUCT_TYPE_APJOBS_POOL_DESC);
    if (base != APRESULT_OK)
        return apjobs_fail(out_error, base, "desc %s", apri_struct_check_message(base));
    if (!out_pool)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "out_pool is NULL");
    if (desc->thread_count > APJOBS_POOL_MAX_THREADS)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "thread_count %u is above APJOBS_POOL_MAX_THREADS (%u)", desc->thread_count,
                           (unsigned)APJOBS_POOL_MAX_THREADS);
    if (desc->max_tasks == 0)
        return apjobs_fail(out_error, APRESULT_INVALID_ARGUMENT, "max_tasks is 0");

    apjobs_pool_t *pool = (apjobs_pool_t *)malloc(sizeof(apjobs_pool_t));
    if (!pool)
        return apjobs_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the pool");
    pool = new (pool) apjobs_pool_t();
    pool->slots = (apjobs_task_slot *)calloc(desc->max_tasks, sizeof(apjobs_task_slot));
    // Room for every thread up front, so starting them allocates nothing
    // here that could fail half-way.
    pool->threads = (std::thread *)malloc(sizeof(std::thread) * (desc->thread_count > 0 ? desc->thread_count : 1));
    if (desc->debug_name)
    {
        const size_t length = strlen(desc->debug_name) + 1;
        pool->name          = (char *)malloc(length);
        if (pool->name)
            memcpy(pool->name, desc->debug_name, length);
    }
    if (!pool->slots || !pool->threads || (desc->debug_name && !pool->name))
    {
        pool->~apjobs_pool_t();
        free(pool);
        return apjobs_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory for the pool's tasks or threads");
    }
    pool->debug_name           = pool->name;
    pool->slot_count           = desc->max_tasks;
    pool->groups.base          = apri_struct_base(APRI_STRUCT_TYPE_APJOBS_SCHEDULER_GROUPS);
    pool->groups.group_create  = apjobs_pool_group_create;
    pool->groups.group_destroy = apjobs_pool_group_destroy;
    pool->groups.group_enqueue = apjobs_pool_group_enqueue;
    pool->groups.group_wait    = apjobs_pool_group_wait;

    for (uint32_t i = 0; i < desc->thread_count; ++i)
    {
        try
        {
            new (&pool->threads[i]) std::thread(apjobs_worker, pool, i + 1);
        }
        catch (const std::system_error &e)
        {
            // Stops the ones started; the destructor joins them.
            pool->~apjobs_pool_t();
            free(pool);
            return apjobs_fail(out_error, APRESULT_UNSUPPORTED, "the system won't start thread %u of %u: %s", i + 1, desc->thread_count, e.what());
        }
        catch (const std::bad_alloc &)
        {
            pool->~apjobs_pool_t();
            free(pool);
            return apjobs_fail(out_error, APRESULT_OUT_OF_MEMORY, "no memory to start thread %u of %u", i + 1, desc->thread_count);
        }
        ++pool->thread_count;
    }
    *out_pool = pool;
    return APRESULT_OK;
}

void apjobs_pool_destroy(apjobs_pool pool)
{
    if (!pool)
        return;
    if (APRI_STALE(pool, "apjobs_pool_destroy", "pool"))
        return;
    pool->~apjobs_pool_t();
    free(pool);
}

APRESULT apjobs_pool_get_scheduler(apjobs_pool pool, APJOBS_SCHEDULER *out_scheduler)
{
    if (!pool)
        return APRESULT_INVALID_ARGUMENT;
    if (APRI_STALE(pool, "apjobs_pool_get_scheduler", "pool"))
        return APRESULT_INVALID_ARGUMENT;
    APRI_ENTER(pool->overlap, "apjobs", "apjobs_pool_get_scheduler", "pool");
    if (!out_scheduler)
        return APRESULT_INVALID_ARGUMENT;
    APJOBS_SCHEDULER scheduler{};
    scheduler.base         = apri_struct_base(APRI_STRUCT_TYPE_APJOBS_SCHEDULER);
    scheduler.base.next    = &pool->groups.base;
    scheduler.worker_count = pool->thread_count + 1;
    scheduler.user         = pool;
    scheduler.enqueue      = apjobs_pool_enqueue;
    scheduler.wait         = apjobs_pool_wait;
    *out_scheduler         = scheduler;
    return APRESULT_OK;
}
}
