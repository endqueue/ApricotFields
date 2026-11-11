/*
 * rbtjobs_groups_draft.h - groups: tasks that may enqueue tasks, waited for
 * together.
 *
 * DRAFT, 2026-11-06, third version (the first 2026-11-05), revised after
 * two reviews: a group's enqueue takes the calling range's worker index,
 * and a nested range's sharing of it is stated; a library copies the
 * groups struct with the scheduler; whether a scheduler allocates is its
 * own, the pool's groups allocating nothing after they are made; the pool
 * frees a group task's slot when its last range ends, and wakes a group's
 * waiter on new work; a group's enqueue may overlap any call on the
 * scheduler; how a scheduler over a pool with no worker indices gives
 * them; the waiting thread counted in a slot it runs, and nothing touching
 * a group once its count is 0. APPROVED by the user 2026-11-06, as it
 * stands. Written on the ThinkPad P1
 * Gen 5 (Windows 11) against include/jobs/rbtjobs.h and src/jobs/rbtjobs.cpp
 * as read that day. Not built, not in any target. Not compilable: it
 * repeats declarations of rbtjobs.h and rbtcore.h to show them changed.
 * Every declaration is marked [ADDED] or [CHANGED]; nothing unmarked
 * changes.
 *
 * ---------------------------------------------------------------------------
 * Why
 * ---------------------------------------------------------------------------
 *
 * Rabbittus Jobs' scheduler runs tasks a caller enqueues and waits for, and a task
 * may not call the scheduler (rbtjobs.h, "Threads"). Work shaped as a graph
 * can't be put on it without holding workers: each task, when it ends,
 * makes ready the tasks that waited on it, and they must be queued then,
 * from inside the task that ended. A library with such work (a physics
 * step is the case at hand; an animation graph or an audio graph are the
 * next) today has to enqueue one long task per worker that loops over a
 * queue of its own and waits when nothing is ready, holding every worker
 * for the whole of the work. On a pool that adds threads when its threads
 * block (Grand Central Dispatch, the .NET thread pool) that grows the pool.
 *
 * A group is a set of tasks waited for together: the caller makes one, the
 * caller or any task of the group enqueues into it, and the caller waits
 * for the group, which returns when every task enqueued into it, those its
 * tasks enqueued included, has run. That is all a graph needs: its
 * dependency counting is the library's, and a task is queued the moment
 * its last dependency ends. It is Grand Central Dispatch's dispatch_group,
 * whose wait counts work added while it waits.
 *
 * General purpose: a group knows nothing of what its tasks compute or
 * which depends on which.
 *
 * ---------------------------------------------------------------------------
 * The shape, and why it is a chained struct
 * ---------------------------------------------------------------------------
 *
 * Four callbacks in a struct chained onto RBTJOBS_SCHEDULER's base
 * (rbtcore.h, "Struct bases": a released layout never changes; what a
 * struct gains is a struct chained on). So:
 * - Every scheduler written today stays valid as it is, and every library
 *   that takes one (Rabbittus Audio's streamed decoding) is untouched: nothing in
 *   RBTJOBS_SCHEDULER moves.
 * - A library that needs groups finds the chained struct (walking
 *   base.next for RBT_STRUCT_TYPE_JOBS_SCHEDULER_GROUPS, as every chain
 *   is walked) and refuses a scheduler without it, in its own words.
 * - rbtjobs_scheduler_check, which refuses any chained struct today,
 *   accepts this one and checks it.
 * - A library keeps a copy of the scheduler (rbtjobs.h: the struct needn't
 *   outlive the call that took it), so it copies the groups struct with
 *   it, its base.next set to NULL; the caller's groups struct needn't
 *   outlive that call either. Without the copy, a library would keep a
 *   pointer into a struct a caller had on its stack.
 *
 * A group's enqueue takes the worker index of the range calling it. A
 * scheduler that can't queue the task runs its ranges there and then, and
 * must run them as some index; it holds no per-thread state to find the
 * caller's, so the caller names it. Those ranges run nested inside the
 * calling range, on its index, while it is paused, so a task keeps no
 * per-worker scratch in use across the call.
 *
 * A group's enqueue from a task may overlap any call on the scheduler:
 * other tasks' group enqueues, and the holder's enqueue and wait, of
 * plain tasks or of other groups. One caller at a time still holds for
 * everything else. A scheduler is often lent to several libraries, one
 * of which may keep a plain task in flight while another's group runs;
 * a rule that the holder call nothing else while a group's tasks run
 * would be a rule over every library at once, which none could keep.
 *
 * ---------------------------------------------------------------------------
 * A scheduler over a pool with no worker indices
 * ---------------------------------------------------------------------------
 *
 * Grand Central Dispatch and the .NET thread pool give a block no worker
 * index, and either may run more blocks at once than a scheduler's
 * `worker_count` (both add threads when theirs block). A scheduler over
 * one gives indices this way, for plain tasks and groups alike:
 * - Its own queue of ranges (a task's ranges, pushed by enqueue or a
 *   group's enqueue), and a count of blocks submitted and not finished,
 *   never above `worker_count - 1`.
 * - Enqueue pushes the ranges, then submits blocks while the count is
 *   below its limit and the queue holds more ranges than blocks running
 *   (dispatch_async_f; ThreadPool.UnsafeQueueUserWorkItem).
 * - A block takes a free index from 1 up (an atomic mask: one is always
 *   free, since there are never more blocks than indices), runs ranges
 *   from the queue as that index until it is empty, gives the index back,
 *   lowers the count, and then looks at the queue once more: if a range
 *   came in after it looked, it raises the count again (if below the
 *   limit) and goes on, so no range is left with no block to run it.
 * - The waiting thread is index 0 and runs ranges from the queue too.
 * - A group is a count raised by its enqueue and lowered by a task's last
 *   range (dispatch_group_enter and dispatch_group_leave, then
 *   dispatch_group_wait; a CountdownEvent or a count and a
 *   ManualResetEventSlim on .NET).
 * No block waits for an index or for anything else, since no task waits,
 * so the pool never sees a blocked thread to add one for.
 * dispatch_apply is not the mapping: it blocks its caller until every
 * iteration is done (a GCD thread, when called from inside a task), and
 * its iteration number counts iterations, not threads.
 *
 * Rejected:
 * - New fields at RBTJOBS_SCHEDULER's end: a changed released layout, and
 *   every scheduler a caller has written would have to fill them.
 * - Letting the existing enqueue be called from inside a task, with wait
 *   taking several handles: the handles of tasks enqueued from inside tasks
 *   are never seen by the one waiting, so it can't name them; a group is
 *   the name.
 * - Dependencies in the scheduler (a task names the tasks it waits for, as
 *   Unity's JobHandle, Unreal's FGraphEvent prerequisites, enkiTS'
 *   dependencies): the scheduler a caller writes over its own threads then
 *   has to implement a graph; GCD and the .NET pool have no such thing to
 *   map it to. A group asks of a scheduler only a count, which every pool
 *   has, and leaves the graph to the library that has one.
 *
 * ---------------------------------------------------------------------------
 * Compared
 * ---------------------------------------------------------------------------
 *
 * - Grand Central Dispatch: dispatch_group_create, dispatch_group_async
 *   (from anywhere, a block of the group included), dispatch_group_wait.
 *   The model taken for the group; worker indices, which GCD has none of,
 *   are the scheduler's (the section above).
 * - .NET: no group object; a CountdownEvent or Task.WhenAll over tasks
 *   started with ThreadPool.UnsafeQueueUserWorkItem, a count raised on
 *   enqueue and lowered on finish, is how a host implements it.
 * - Box2D v3 (RBTJOBS_SCHEDULER's shape): enqueueTask and finishTask, no
 *   enqueue from a task; its solver's graph runs inside long tasks per
 *   worker, as the holding loop above. What this removes.
 * - Jolt's JobSystem: jobs queue their dependents as they end, a barrier
 *   waited for; JobSystemThreadPool implements it with its own threads.
 *   Jolt's barrier is a group.
 * - enkiTS: a task set may add task sets from inside one, and dependencies.
 * - Unity's job system and Unreal's task graph: dependencies in the
 *   scheduler (rejected above, for what a caller's scheduler would owe).
 * - Hazel: no job system of its own (src/Hazel/Core has a render thread
 *   and a thread wrapper); nothing to measure against.
 *
 * ---------------------------------------------------------------------------
 * ABI
 * ---------------------------------------------------------------------------
 *
 * Additions only: a struct type, four function types, a struct, an added
 * sentence on RBTJOBS_SCHEDULER (a library copies a chained groups struct
 * with it), and a new meaning for rbtjobs_pool_get_scheduler's base.next
 * (it pointed nowhere; now at the pool's groups struct).
 * rbtjobs_scheduler_check accepts a chain it refused: a caller that relied
 * on the refusal of this one type sees success. No layout changes. ./check.sh --update-abi lists them; the
 * misuse generator is rerun.
 *
 * ---------------------------------------------------------------------------
 * Tests (tests/rbtjobs_tests.c, through the C API only; pending while the
 * pool's group callbacks are absent)
 * ---------------------------------------------------------------------------
 *
 * - A tree: one task enqueued by the caller, each task enqueuing two until
 *   a depth of 12 (8,191 tasks), each counting itself into its own slot;
 *   one wait; every slot counted once. At 0, 1, 3 and 7 threads.
 * - A chain: each task enqueuing the next, 10,000 long; the wait returns
 *   after the last.
 * - A graph: a diamond of tasks whose dependency counts the test keeps
 *   (atomics), each enqueuing a dependent when its count reaches zero;
 *   results summed in item order, the same bits at every thread count.
 * - A group reused for 1,000 waits; two groups in flight, waited for in
 *   either order.
 * - Past max_tasks: enqueue from inside a task runs the task there, and
 *   the tree still completes (depth bounded by the test).
 * - Slots come back: a chain of 10,000 tasks on a pool with max_tasks 4
 *   and 3 threads, each enqueuing the next as it ends, so no more than two
 *   are in flight at once; no range ever runs nested (each range records
 *   how deep it runs, from a counter per worker index in the test's
 *   data), so no task was run inline for want of a slot, and every slot a
 *   task held was freed by the time its successor needed one.
 * - Overlap: a group's tree on a pool of 3 threads while the holder
 *   enqueues and waits plain tasks, and enqueues into and waits for a
 *   second group, 1,000 times: every count right, no overlap reported in
 *   Debug.
 * - The scheduler over a pool with no worker indices, as the section
 *   above describes it, written in the test over an executor that starts
 *   a thread for every block submitted (as an overcommitting pool may):
 *   the tree, the chain and the graph at worker_count 2, 4 and 8, with
 *   the worker-index check below; and the chain with the executor
 *   delaying each block's start, so ranges come in while every block is
 *   leaving (the second look). It is the reference a caller's GCD or .NET
 *   scheduler is written from.
 * - Worker indices: a tree on a pool with max_tasks 1 and 7 threads, so
 *   most enqueues from inside tasks run inline; each range marks its index
 *   busy on entry (an atomic per index holding the range's token) and
 *   finds it free or held by the range that enqueued it (named in its
 *   task data), never by another.
 * - No thread (thread_count 0): every task runs inside the wait, in the
 *   order enqueued.
 * - rbtjobs_scheduler_check: the groups struct accepted; each of its four
 *   NULL refused, one line each, in field order; its wrong type refused.
 * - rbtnoalloc_tests: a scenario enqueuing and waiting a tree of 1,000
 *   tasks into one group made beforehand, 100 times: none of ours.
 * - The two-thread test: the tree on a pool of 7 threads in every build,
 *   so -tsan sees it on macOS and Linux.
 * - The misuse harness regenerated and run ten times in Debug.
 */

/* ===========================================================================
 * rbtcore.h  [CHANGED]
 * ========================================================================= */

/*
 * RBT_STRUCT_TYPE, in Rabbittus Jobs' block (7000+), after
 * RBT_STRUCT_TYPE_JOBS_POOL_DESC:
 *
 *     [ADDED] RBT_STRUCT_TYPE_JOBS_SCHEDULER_GROUPS, a chained struct
 *     (rbtcore.h, "Struct bases": chained onto RBTJOBS_SCHEDULER).
 */

/* ===========================================================================
 * rbtjobs.h  [CHANGED]
 * ========================================================================= */

/*
 * [CHANGED] The file comment: "Not built here: dependencies between
 * tasks..." becomes: "Not built here: dependencies between tasks (a caller
 * that has them counts them, and enqueues a task into a group when its
 * last one ends: RBTJOBS_SCHEDULER_GROUPS), priorities, and a task that
 * waits for another."
 *
 * [CHANGED] "Threads": "...enqueue and wait are called by the code that
 * holds the scheduler, never from inside a task" gains: "A group's enqueue
 * is the one call a task may make (RBTJOBS_SCHEDULER_GROUPS), and it may
 * overlap any call on the scheduler."
 *
 * [CHANGED] "Struct bases": "nothing chains onto either yet" becomes
 * "RBTJOBS_SCHEDULER_GROUPS may be chained onto RBTJOBS_SCHEDULER; nothing
 * chains onto RBTJOBS_POOL_DESC".
 *
 * [CHANGED] RBTJOBS_SCHEDULER's comment: "A library that takes one keeps a
 * copy, so the struct needn't outlive the call that took it" gains: "and
 * a copy of an RBTJOBS_SCHEDULER_GROUPS chained on, which needn't outlive
 * it either". Its "Thread:" paragraph gains: "A group's enqueue is the
 * exception: see RBTJOBS_GROUP_ENQUEUE_FUNCTION."
 */

/* ---- Groups ---------------------------------------------------------- */

/**
 * @brief [ADDED] Makes a group: a set of tasks waited for together.
 *
 * Called by the code that holds the scheduler, at setup, never in a task.
 * A group is reused for wait after wait. Making one may allocate; whether
 * a group's enqueue or wait allocates is the scheduler's own, as for
 * #RBTJOBS_ENQUEUE_FUNCTION (Grand Central Dispatch's dispatch_group_async
 * does; the pool's groups don't, rbtjobs_pool_get_scheduler()).
 *
 * @param[in] user The scheduler's RBTJOBS_SCHEDULER::user.
 *
 * @return The group, or NULL if the scheduler couldn't make one (out of
 *         memory), which the caller reports as RBT_RESULT_OUT_OF_MEMORY.
 */
typedef void *(*RBTJOBS_GROUP_CREATE_FUNCTION)(void *user);

/**
 * @brief [ADDED] Lets go of a group no task is in.
 *
 * Called by the code that holds the scheduler, never in a task.
 *
 * @param[in] group A group #RBTJOBS_GROUP_CREATE_FUNCTION made, waited for
 *                  since its last enqueue; never NULL.
 * @param[in] user  The scheduler's RBTJOBS_SCHEDULER::user.
 */
typedef void (*RBTJOBS_GROUP_DESTROY_FUNCTION)(void *group, void *user);

/**
 * @brief [ADDED] Starts a task in a group: @p function over
 * `[0, item_count)`, split into ranges of at least @p min_range items, as
 * #RBTJOBS_ENQUEUE_FUNCTION does.
 *
 * It never fails: a scheduler that can't queue the task (past its own
 * limit) runs every range before returning, on the calling thread, as
 * @p worker_index, from inside a task too, so a caller's graph is as deep
 * as it lets the calls nest. A range run so from inside a task runs nested
 * in the calling range, on its index, while that range is paused: no two
 * ranges run at once with one index, but the calling range keeps no
 * per-worker scratch in use across the call.
 *
 * Thread: by the code that holds the scheduler, or from inside a task of
 * the same group (the one call a task may make to the scheduler), from
 * several tasks at once. Not from a task of another group. From a task it
 * may overlap any call on the scheduler: other group enqueues, and the
 * holder's enqueue and wait, of plain tasks or of any group.
 *
 * @param[in] group        The group; never NULL.
 * @param[in] worker_index The `worker_index` of the range calling, or 0
 *                         from the code that holds the scheduler; below
 *                         the scheduler's `worker_count`. A range the call
 *                         runs itself runs as this index.
 * @param[in] function     The task's function. Never NULL.
 * @param[in] item_count   How many items; above 0.
 * @param[in] min_range    The fewest items worth a call; above 0.
 * @param[in] task_data    Passed to every call of @p function.
 * @param[in] user         The scheduler's RBTJOBS_SCHEDULER::user.
 */
typedef void (*RBTJOBS_GROUP_ENQUEUE_FUNCTION)(void *group, uint32_t worker_index, RBTJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t min_range, void *task_data, void *user);

/**
 * @brief [ADDED] Waits until every task enqueued into a group has run every
 * range: those enqueued before the wait and those its tasks enqueued while
 * it waited.
 *
 * The thread that waits may run ranges of the group's tasks itself, as
 * worker 0. Once it returns, every write the tasks made is visible to the
 * caller, and the group is empty and may be enqueued into again. A group
 * with nothing enqueued returns at once.
 *
 * Thread: by the code that holds the scheduler, never from inside a task.
 *
 * @param[in] group The group; never NULL.
 * @param[in] user  The scheduler's RBTJOBS_SCHEDULER::user.
 */
typedef void (*RBTJOBS_GROUP_WAIT_FUNCTION)(void *group, void *user);

/**
 * @brief [ADDED] What a scheduler offers to run groups, chained onto its
 * RBTJOBS_SCHEDULER (`base.next`).
 *
 * Why: work shaped as a graph queues each task when the tasks it waited for
 * end, from inside them; without groups it can only hold workers in loops
 * of its own (rbtjobs_groups_draft.h, "Why"). A library that needs groups
 * finds this struct in the scheduler's chain and says so when it is
 * missing; one that doesn't ignores it. A library that keeps the
 * scheduler keeps a copy of this struct too, its `base.next` set to NULL,
 * so the caller's needn't outlive the call that took it.
 *
 * A scheduler a caller writes over Grand Central Dispatch or the .NET
 * thread pool keeps a group as a count raised on enqueue, lowered when a
 * task's last range ends, and waited on (dispatch_group_enter, _leave and
 * _wait on GCD), and runs the ranges with worker indices of its own:
 * rbtjobs_groups_draft.h, "A scheduler over a pool with no worker
 * indices".
 */
typedef struct RBTJOBS_SCHEDULER_GROUPS {
	/** This struct's type, RBT_STRUCT_TYPE_JOBS_SCHEDULER_GROUPS, and its chain (rbtcore.h, "Struct bases"). */
	RBT_STRUCT_BASE base;
	/** @see RBTJOBS_GROUP_CREATE_FUNCTION. Not NULL. */
	RBTJOBS_GROUP_CREATE_FUNCTION group_create;
	/** @see RBTJOBS_GROUP_DESTROY_FUNCTION. Not NULL. */
	RBTJOBS_GROUP_DESTROY_FUNCTION group_destroy;
	/** @see RBTJOBS_GROUP_ENQUEUE_FUNCTION. Not NULL. */
	RBTJOBS_GROUP_ENQUEUE_FUNCTION group_enqueue;
	/** @see RBTJOBS_GROUP_WAIT_FUNCTION. Not NULL. */
	RBTJOBS_GROUP_WAIT_FUNCTION group_wait;
} RBTJOBS_SCHEDULER_GROUPS;

/**
 * @brief [CHANGED] Checks a scheduler as a library that takes one does.
 *
 * Accepts an RBTJOBS_SCHEDULER_GROUPS chained on, and checks it; any other
 * chained struct is refused, as before.
 *
 * @param[in]  scheduler The scheduler.
 * @param[out] out_error Why it was refused, when it was. May be NULL.
 *
 * @return RBT_RESULT_OK; RBT_RESULT_INVALID_ARGUMENT if @p scheduler is NULL,
 *         its `worker_count` is 0, its `enqueue` or `wait` is NULL, or a
 *         chained RBTJOBS_SCHEDULER_GROUPS has any of its four NULL (each its
 *         own message, in field order); or the struct codes for
 *         `scheduler->base` and for the groups struct's base.
 */
RBT_RESULT rbtjobs_scheduler_check(
	const RBTJOBS_SCHEDULER *scheduler,
	RBTJOBS_ERROR *out_error);

/**
 * @brief [CHANGED] Fills a scheduler that runs tasks on a pool.
 *
 * Its `worker_count` is the pool's `thread_count` plus 1, and its `user`
 * the pool. The whole struct is written, `base` included: `base.next`
 * points at the pool's own RBTJOBS_SCHEDULER_GROUPS, alive as long as the
 * pool. A wait runs ranges of the task it waits for on the waiting thread,
 * as worker 0; a group's wait, of the group's tasks.
 *
 * The pool's groups: a group is a small record the pool allocates in
 * group_create; enqueuing into one and waiting for it allocate nothing.
 * Tasks enqueued into groups take the pool's task slots as any task does
 * (RBTJOBS_POOL_DESC::max_tasks), each slot free again as soon as its
 * task's last range ends; past them, a task runs in its enqueue, on the
 * thread enqueuing it, as the worker index it was given.
 *
 * @param[in]  pool          The pool.
 * @param[out] out_scheduler Receives the scheduler.
 *
 * @return RBT_RESULT_OK, or RBT_RESULT_INVALID_ARGUMENT, with nothing written,
 *         if @p pool or @p out_scheduler is NULL.
 */
RBT_RESULT rbtjobs_pool_get_scheduler(
	rbtjobs_pool pool,
	RBTJOBS_SCHEDULER *out_scheduler);

/*
 * Implementation notes for the pool (src/jobs/rbtjobs.cpp, internal, for the
 * reviewer):
 * - A group is a record of one atomic count of tasks enqueued and not ended
 *   (its own cache line) and the pool's slot for each task records its
 *   group. group_enqueue raises the count (release) before the task is
 *   visible; a task's last range lowers it (acq_rel) after its writes; the
 *   wait runs claimable ranges of its group's tasks, and when none is left
 *   to claim sleeps on the pool's `finished` condition until the count is 0
 *   or the group has a range to claim again.
 * - A group task has no wait of its own, so its slot is freed by whoever
 *   leaves it last: the worker whose range ends the task frees it when the
 *   slot's `inside` count reaches 0 (the waiting thread, as worker 0, when
 *   it is that one), and only then lowers the group's count. The waiting
 *   thread raises and lowers `inside` around the ranges it runs of a
 *   group's slot, as the threads do (today's plain wait doesn't, since it
 *   frees its own slot after): otherwise a thread could free the slot and
 *   another task take it over while the waiter was still in a range.
 * - Nothing touches a group once its count reaches 0: from then on the
 *   wait may return and the holder destroy it. Whoever lowered the count
 *   to 0 then only takes the pool's mutex and notifies `finished`, the
 *   pool's own; the wait checks the count under that mutex, so the
 *   notification can't be lost. The tests' tree, destroyed right after
 *   each wait, runs under ASan and TSan for it. A plain task's
 *   slot is still freed by its wait. Without this, a group's slots would be
 *   held until its wait, and a graph of more tasks than max_tasks would run
 *   most of them inline, serially and nested.
 * - group_enqueue notifies `work`, for the threads, and `finished`, for a
 *   wait on the same group sleeping with nothing to claim: otherwise the
 *   waiting thread, worker 0, sits out every task enqueued after it ran out
 *   of ranges.
 * - Inline (no slot free), group_enqueue runs the ranges as the
 *   worker_index it was given, not as 0 (today's rbtjobs_run_inline takes
 *   the index as a parameter).
 * - group_enqueue from inside a task takes the pool's mutex to claim a
 *   slot, as enqueue does; the mutex is the pool's existing one. A one-item
 *   task takes it about four times (claim the slot, a worker's scan for
 *   it, the last range, the end), which is acceptable for a first build. A
 *   test times a graph of one-item tasks on the pool against the same
 *   graph run by holding workers in loops (the "Why" above), and a
 *   lock-free ready list replaces the mutex only if that shows it.
 * - The Debug overlap check (RBT_ENTER) is not on group_enqueue (a
 *   Thread: exception) but is on group_wait, create and destroy; plain
 *   enqueue and wait keep theirs, which a task's group_enqueue never
 *   enters, so overlapping it is never reported.
 * - A group's wait never runs a task of another group, so a library's
 *   waits never interleave another's work into its own.
 * - Nothing is allocated by group_enqueue or group_wait.
 */
