#ifndef APJOBS_H
#define APJOBS_H

#include "../apricore.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/**
 * @file apjobs.h
 * @brief ApJobs - parallel work through a scheduler the caller supplies, and
 * a reference pool a caller may create.
 *
 * General purpose: it knows nothing about what a task computes. A library
 * that wants work done in parallel takes an APJOBS_SCHEDULER from its caller
 * and hands it tasks; the caller implements the scheduler over whatever
 * threads it has (Grand Central Dispatch, the .NET thread pool, an engine's
 * own workers), or creates the reference pool here. ApricotFields starts no
 * thread for this anywhere but in the pool, and only when the caller creates
 * one.
 *
 * A task is one function run over a range of items: the scheduler splits
 * `[0, item_count)` into ranges and calls the function once per range, on
 * whatever threads it chooses, with the index of the worker running it. A
 * task writes only its own outputs, so the results don't depend on how the
 * items were split or in what order the ranges ran; a caller that merges
 * them does so in item order, which is how the same inputs give the same
 * results however many threads there are.
 *
 * Not built here: dependencies between tasks (a caller orders tasks by
 * waiting), priorities, and a task that waits for another (see
 * APJOBS_SCHEDULER, which calls may come from where).
 *
 * @par Threads
 * apricore.h's rule ("Threads"): a pool, and a scheduler the caller
 * implements, is used by one caller at a time - enqueue and wait are called
 * by the code that holds the scheduler, never from inside a task. The
 * task's function is the exception: it runs on the scheduler's threads,
 * several ranges of one task at once.
 *
 * @par Results
 * Calls return APRESULT (apricore.h): APRESULT_OK or a general code. The
 * scheduler's own callbacks return nothing and can't fail: a scheduler that
 * can't queue a task runs it before enqueue returns.
 *
 * @par Error messages
 * A call that can fail for a reason worth a sentence takes an APJOBS_ERROR
 * pointer (NULL to skip it): on failure it says what was wrong; on success
 * its message is empty.
 *
 * @par Struct bases
 * APJOBS_SCHEDULER and APJOBS_POOL_DESC start with a `base` (apricore.h,
 * "Struct bases"): the caller sets `base.type` to the struct's own
 * APRI_STRUCT_TYPE_APJOBS_* and `base.next` to NULL. A wrong type is
 * APRESULT_STRUCT_WRONG_TYPE and a chained struct
 * APRESULT_STRUCT_UNKNOWN_IN_CHAIN: nothing chains onto either yet.
 * APJOBS_ERROR is frozen and has none.
 */

/* ---- Conventions ----------------------------------------------------- */

/** @brief The size of APJOBS_ERROR's `message`, its NUL included. */
#define APJOBS_ERROR_MESSAGE_SIZE 256

/**
 * @brief Why a call failed, filled per call (apjobs.h, "Error messages").
 */
/* Frozen: filled per call. It never changes; a new need gets a new type. */
typedef struct APJOBS_ERROR {
	char message[APJOBS_ERROR_MESSAGE_SIZE]; /**< NUL-terminated, truncated to fit; empty on success. */
} APJOBS_ERROR;

/* ---- The scheduler --------------------------------------------------- */

/**
 * @brief A task's work on one range of its items.
 *
 * Thread: runs on the scheduler's threads, several ranges of one task at
 * once; each range is given to one call. It must not call the scheduler.
 *
 * @param[in] first_item   The range's first item.
 * @param[in] end_item     One past its last item; above @p first_item.
 * @param[in] worker_index Which worker runs the range, below the
 *                         scheduler's `worker_count`. No two ranges run at
 *                         once with one index, so per-worker scratch
 *                         memory indexed by it is never shared.
 * @param[in] task_data    The task's data, as given to enqueue.
 */
typedef void (*APJOBS_TASK_FUNCTION)(uint32_t first_item, uint32_t end_item, uint32_t worker_index, void *task_data);

/**
 * @brief Starts a task: @p function over `[0, item_count)`, split into
 * ranges of at least @p min_range items (the last may be shorter).
 *
 * Every item is in exactly one range, and each range is passed to one call
 * of @p function. How the items are split, on which threads and in what
 * order the ranges run, is the scheduler's. It never fails: a scheduler
 * that can't queue the task runs every range itself before returning.
 *
 * @param[in] function   The task's function. Never NULL.
 * @param[in] item_count How many items; above 0.
 * @param[in] min_range  The fewest items worth a call; above 0.
 * @param[in] task_data  Passed to every call of @p function.
 * @param[in] user       The scheduler's APJOBS_SCHEDULER::user.
 *
 * @return A handle for #APJOBS_WAIT_FUNCTION, which must be waited for once;
 *         or NULL if every range has run already, which needs no wait.
 */
typedef void *(*APJOBS_ENQUEUE_FUNCTION)(APJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t min_range, void *task_data, void *user);

/**
 * @brief Waits until every range of a task has run.
 *
 * The thread that waits may run ranges of the task itself, as worker 0.
 * Once it returns, every write the task's calls made is visible to the
 * caller, and the handle is gone.
 *
 * @param[in] task The handle enqueue returned; never NULL.
 * @param[in] user The scheduler's APJOBS_SCHEDULER::user.
 */
typedef void (*APJOBS_WAIT_FUNCTION)(void *task, void *user);

/**
 * @brief A scheduler: what a library is given to run tasks with.
 *
 * Filled by the caller over its own threads, or by
 * apjobs_pool_get_scheduler(). A library that takes one keeps a copy, so
 * the struct needn't outlive the call that took it; `user` must outlive
 * every task the library starts with it.
 *
 * Two tasks may be in flight at once, waited for in any order.
 *
 * Thread: its callbacks are called by the code that holds the scheduler,
 * one call at a time, never from inside a task (a task never waits for
 * another); the tasks' functions run on whatever threads the scheduler
 * runs them on.
 */
typedef struct APJOBS_SCHEDULER {
	/** This struct's type, APRI_STRUCT_TYPE_APJOBS_SCHEDULER, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * How many workers may run ranges at once, the thread that waits
	 * counted as worker 0: every `worker_index` is below it. Not 0; 1 is a
	 * scheduler that runs everything on the caller's thread.
	 */
	uint32_t worker_count;
	/** Passed to #enqueue and #wait as their `user`. The scheduler's. */
	void *user;
	/** @see APJOBS_ENQUEUE_FUNCTION. Not NULL. */
	APJOBS_ENQUEUE_FUNCTION enqueue;
	/** @see APJOBS_WAIT_FUNCTION. Not NULL. */
	APJOBS_WAIT_FUNCTION wait;
} APJOBS_SCHEDULER;

/**
 * @brief Checks a scheduler as a library that takes one does.
 *
 * @param[in]  scheduler The scheduler.
 * @param[out] out_error Why it was refused, when it was. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p scheduler is NULL,
 *         its `worker_count` is 0 or its `enqueue` or `wait` is NULL; or
 *         the struct codes for `scheduler->base`.
 */
APRESULT apjobs_scheduler_check(
	const APJOBS_SCHEDULER *scheduler,
	APJOBS_ERROR *out_error);

/**
 * @brief Runs a task through a scheduler and waits for it: enqueue, then
 * wait if enqueue handed back a task.
 *
 * With no scheduler, every range runs on the calling thread, as worker 0,
 * in item order: a library given none runs its tasks serially.
 *
 * Thread: on the scheduler, as its own callbacks are (one caller at a
 * time); with none, it takes no object and may run at once with anything.
 *
 * @param[in] scheduler  The scheduler, checked beforehand
 *                       (apjobs_scheduler_check()), or NULL.
 * @param[in] function   The task's function.
 * @param[in] item_count How many items; 0 runs nothing.
 * @param[in] min_range  The fewest items worth a call; above 0.
 * @param[in] task_data  Passed to every call of @p function.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT, with nothing run, if
 *         @p function is NULL or @p min_range is 0.
 */
APRESULT apjobs_run(
	const APJOBS_SCHEDULER *scheduler,
	APJOBS_TASK_FUNCTION function,
	uint32_t item_count,
	uint32_t min_range,
	void *task_data);

/* ---- The reference pool ---------------------------------------------- */

/**
 * @brief The number of threads the machine can run at once, for a caller
 * choosing a pool's `thread_count`.
 *
 * @return The count the system reports; 1 if it reports none.
 */
uint32_t apjobs_get_processor_count(void);

/** @brief Opaque handle to a reference pool: threads that run tasks. */
typedef struct apjobs_pool_t *apjobs_pool;

/**
 * @brief Configuration descriptor for a pool.
 */
typedef struct APJOBS_POOL_DESC {
	/** This struct's type, APRI_STRUCT_TYPE_APJOBS_POOL_DESC, and its chain (apricore.h, "Struct bases"). */
	APRI_STRUCT_BASE base;
	/**
	 * Threads the pool starts, each a worker beside the thread that waits:
	 * the scheduler's `worker_count` is this plus 1. 0 starts none, and
	 * every task runs in its wait on the caller's thread. At most
	 * #APJOBS_POOL_MAX_THREADS.
	 */
	uint32_t thread_count;
	/**
	 * Tasks in flight at once: enqueued and not yet waited for. Not 0. A
	 * task past it runs in enqueue, on the caller's thread.
	 */
	uint32_t max_tasks;
	/** A name for diagnostics, copied. May be NULL. */
	const char *debug_name;
} APJOBS_POOL_DESC;

/** @brief The most threads one pool starts. */
#define APJOBS_POOL_MAX_THREADS 256

/**
 * @brief Creates a pool and starts its threads.
 *
 * Its threads sleep while no task is in flight, and take no work but the
 * ranges of its tasks.
 *
 * @param[in]  desc      Pool configuration. Only read during the call.
 * @param[out] out_pool  Receives the pool; NULL on failure.
 * @param[out] out_error Why it failed, when it did. May be NULL.
 *
 * @return APRESULT_OK; APRESULT_INVALID_ARGUMENT if @p desc or @p out_pool
 *         is NULL, `max_tasks` is 0 or `thread_count` is above
 *         APJOBS_POOL_MAX_THREADS; the struct codes for `desc->base`;
 *         APRESULT_OUT_OF_MEMORY; or APRESULT_UNSUPPORTED if the system
 *         won't start a thread, with nothing left running.
 *
 * @see apjobs_pool_destroy()
 */
APRESULT apjobs_pool_create(
	const APJOBS_POOL_DESC *desc,
	apjobs_pool *out_pool,
	APJOBS_ERROR *out_error);

/**
 * @brief Stops a pool's threads, waiting for each to end, and destroys it.
 *
 * @warning Every task enqueued through its scheduler is waited for first,
 * and nothing holds the scheduler after.
 *
 * @param[in] pool Pool to destroy. NULL is accepted and does nothing.
 */
void apjobs_pool_destroy(apjobs_pool pool);

/**
 * @brief Fills a scheduler that runs tasks on a pool.
 *
 * Its `worker_count` is the pool's `thread_count` plus 1, and its `user`
 * the pool. The whole struct is written, `base` included. A wait runs
 * ranges of the task it waits for on the waiting thread, as worker 0.
 *
 * @param[in]  pool          The pool.
 * @param[out] out_scheduler Receives the scheduler.
 *
 * @return APRESULT_OK, or APRESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p pool or @p out_scheduler is NULL.
 */
APRESULT apjobs_pool_get_scheduler(
	apjobs_pool pool,
	APJOBS_SCHEDULER *out_scheduler);

#if __cplusplus
}
#endif

#endif // APJOBS_H
