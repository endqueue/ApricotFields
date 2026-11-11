# Rabbittus Jobs

Parallel work through a scheduler the caller supplies (`include/jobs/rbtjobs.h`),
and a reference pool a caller may create. Decided in the workspace's
`DECISIONS.md` (2026-10-29); the workspace's threading rule is the root
`CLAUDE.md`, "Threading" (card X3).

## What is built (2026-10-30)

- **The interface** (`RBTJOBS_SCHEDULER`, struct type 7000): a worker count,
  the caller's `user`, and two callbacks, `enqueue` (a function over
  `[0, item_count)` in ranges of at least `min_range`, returning a handle or
  NULL if it already ran) and `wait`. Box2D v3's shape (`b2EnqueueTaskCallback`
  / `b2FinishTaskCallback`), chosen over Jolt's job graph because it is the
  smallest C interface every host's pool can implement: Grand Central
  Dispatch's `dispatch_apply`, the .NET thread pool's `Parallel.For`, an
  engine's workers. A range's call gets its worker index, so a task can keep
  scratch memory per worker. Enqueue never fails: a scheduler that can't
  queue runs the task before returning, so a library has no error path for
  it. Callbacks are called by the scheduler's one caller, never from inside a
  task: no task waits for another, which keeps every pool deadlock-free
  without work stealing.
- `rbtjobs_scheduler_check` (what a library runs on the scheduler it is
  given) and `rbtjobs_run` (enqueue, then wait; with no scheduler, every range
  on the calling thread in item order, so a library given none is serial).
- **The reference pool** (`rbtjobs_pool`, `RBTJOBS_POOL_DESC`, struct type
  7001): `thread_count` threads (0 to `RBTJOBS_POOL_MAX_THREADS`, 256) beside
  the waiting thread, which runs ranges of the task it waits for as worker 0;
  `max_tasks` slots, allocated at creation, so enqueue allocates nothing,
  and a task past them runs in enqueue. About four ranges a worker, none
  under `min_range`. Items are claimed with atomics on plain slot fields
  (`std::atomic_ref`); the mutex is taken to find a task, to leave it, and
  once for its last range. A slot is reused only when no worker is inside it.
  `rbtjobs_get_processor_count` for a caller choosing `thread_count`.
- **Tests** (`tests/rbtjobs_tests.c`, C, written before the code and reporting
  pending until it existed): the scheduler check's refusals; serial runs;
  pool refusals; tasks of 1 to 100,000 items on 0 to 4 threads, every item
  covered once, no range under `min_range` but the last, no worker running
  two ranges at once, and the merged result the serial one's; two tasks in
  flight waited in the other order, a third past `max_tasks` run in
  enqueue, a slot freed and taken again; 500 tasks in a row.
- **The Debug build's overlap check** (`src/rbtoverlap.hpp`, every
  module's, 2026-10-30) on the pool's enqueue, wait and get_scheduler.
- **Groups** (`RBTJOBS_SCHEDULER_GROUPS`, struct type 7002, chained onto
  `RBTJOBS_SCHEDULER`; `rbtjobs_groups_draft.h`, approved 2026-11-06, built
  the same day on Windows): tasks that may enqueue tasks into their own
  group, the one call a task may make, naming its worker index; a wait for
  every task of the group, those enqueued while it waits included. Grand
  Central Dispatch's `dispatch_group`. `rbtjobs_scheduler_check` walks the
  chain (the groups struct once, nothing else, each of its four callbacks
  checked in field order). The pool's groups: a group is a count on a
  cache line of its own, made in `group_create`; a group's task takes a
  slot as any task does, the slot freed by whoever leaves it last once its
  last range has run, and only then the count lowered; past `max_tasks`
  the task runs in its enqueue as the worker index given, nested in the
  calling range; a group's wait runs its own group's tasks, oldest first,
  and sleeps on the pool's `finished` until the count is 0 or a task of
  its group has a range to claim. No allocation in a group's enqueue or
  wait. The overlap check on `group_create`, `_destroy` and `_wait`, not on
  `group_enqueue` (a `Thread:` exception). Rabbittus Audio's mixer now drops the
  chain from its copy of the scheduler, keeping no pointer into the
  caller's struct.
  Tests: `rbtjobs_tests` (the chain check's refusals, the pool's groups
  chained on) and `rbtjobs_groups_tests` (C++ for `std::thread` and
  `std::atomic`, the C API only: a tree of 8,191 tasks at 0, 1, 3 and 7
  threads, a chain of 10,000, slots coming back with `max_tasks` 4, a
  layered graph the same bits at every thread count, a group reused 1,000
  times and two in flight, past `max_tasks` each nested range finding its
  index held only by its enqueuer, the holder's plain tasks and a second
  group overlapping a group's tree 1,000 times, no thread in enqueue order,
  and the reference scheduler over an executor with no worker indices: at
  most `worker_count - 1` blocks, each with an index from 1 up, at 2, 4 and
  8 workers, and with blocks that start late); `rbtnoalloc_tests` (a tree
  of 1,000 tasks in one group a hundred times: none of ours).

## What has run where

- macOS (Apple M5 Pro), 2026-10-30: the tests plain, under ASan, and under
  TSan twenty times in a row, clean; every worker ran ranges of the large
  tasks. **Linux never built:** plain, `-asan` and `-tsan` there.
- Windows 11 (ThinkPad P1 Gen 5, MSVC 14.51), 2026-10-30: `rbtjobs_tests`
  passes on `windows-d3d12` and `windows-vulkan` in Debug, Release and
  RelWithDebInfo, and under both `-asan` presets (MSVC has no TSan); the
  pool's waiting thread makes no allocation (`rbtnoalloc_tests`).
- Windows 11 (the same ThinkPad, MSVC 14.51), 2026-11-06, the groups:
  `rbtjobs_tests` and `rbtjobs_groups_tests` on `rabbittus-windows-d3d12`
  in Debug and Release (every other ctest too, A49's `rbtrend_contract_tests`
  the only failure, as before), `rbtjobs_groups_tests` 100 times in Release
  and 30 in Debug without a failure, `rbtnoalloc_tests`' group tree none
  of ours in both; `rbtjobs_tests` and `rbtjobs_groups_tests` under
  `rabbittus-windows-d3d12-asan`, no report. **Not run with the
  groups:** macOS and Linux, and TSan anywhere (Windows has none).

## Not done

- **No caller yet.** A caller takes an `RBTJOBS_SCHEDULER *` in its desc
  and copies it; whoever creates that caller implements the scheduler over
  its platform's pool (Grand Central Dispatch, the .NET thread pool) or
  creates the reference pool. Which callers come first is the workspace's
  `ROADMAP.md`.
- **Dependencies, priorities, a task that waits for another**: none. A
  caller orders tasks by waiting. If one is ever needed (a physics engine's
  job graph through this interface), it is a new callback chained on, and
  the pool learns to help with other tasks while it waits.
  **Drafted 2026-11-05** (the ThinkPad P1, Windows 11), revised
  2026-11-06 after two reviews, approved by the user 2026-11-06, not
  built:
  `rbtjobs_groups_draft.h`. Groups, Grand Central Dispatch's
  `dispatch_group`: an `RBTJOBS_SCHEDULER_GROUPS` chained onto the
  scheduler's base (struct type 7002), its four callbacks making a group,
  letting one go, enqueuing a task into one (from the holder, or from
  inside a task of the group: the one call a task may make) and waiting
  for all of it, those its tasks enqueued included. Dependencies stay the
  caller's to count; a task still never waits for another, so the pool
  stays free of deadlock without work stealing. No layout changes, so every
  scheduler and every caller today (Rabbittus Audio's streams) is untouched;
  `rbtjobs_scheduler_check` accepts the new chained struct, and
  `rbtjobs_pool_get_scheduler` points its base at the pool's own.
