# ApJobs

Parallel work through a scheduler the caller supplies (`include/jobs/apjobs.h`),
and a reference pool a caller may create. Decided in the workspace's
`DECISIONS.md` (2026-10-29); the workspace's threading rule is the root
`CLAUDE.md`, "Threading" (card X3).

## What is built (2026-10-30)

- **The interface** (`APJOBS_SCHEDULER`, struct type 7000): a worker count,
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
- `apjobs_scheduler_check` (what a library runs on the scheduler it is
  given) and `apjobs_run` (enqueue, then wait; with no scheduler, every range
  on the calling thread in item order, so a library given none is serial).
- **The reference pool** (`apjobs_pool`, `APJOBS_POOL_DESC`, struct type
  7001): `thread_count` threads (0 to `APJOBS_POOL_MAX_THREADS`, 256) beside
  the waiting thread, which runs ranges of the task it waits for as worker 0;
  `max_tasks` slots, allocated at creation, so enqueue allocates nothing,
  and a task past them runs in enqueue. About four ranges a worker, none
  under `min_range`. Items are claimed with atomics on plain slot fields
  (`std::atomic_ref`); the mutex is taken to find a task, to leave it, and
  once for its last range. A slot is reused only when no worker is inside it.
  `apjobs_get_processor_count` for a caller choosing `thread_count`.
- **Tests** (`tests/apjobs_tests.c`, C, written before the code and reporting
  pending until it existed): the scheduler check's refusals; serial runs;
  pool refusals; tasks of 1 to 100,000 items on 0 to 4 threads, every item
  covered once, no range under `min_range` but the last, no worker running
  two ranges at once, and the merged result the serial one's; two tasks in
  flight waited in the other order, a third past `max_tasks` run in
  enqueue, a slot freed and taken again; 500 tasks in a row.
- **The Debug build's overlap check** (`src/aprioverlap.hpp`, every
  module's, 2026-10-30) on the pool's enqueue, wait and get_scheduler.

## What has run where

- macOS (Apple M5 Pro), 2026-10-30: the tests plain, under ASan, and under
  TSan twenty times in a row, clean; every worker ran ranges of the large
  tasks. **Linux never built:** plain, `-asan` and `-tsan` there.
- Windows 11 (ThinkPad P1 Gen 5, MSVC 14.51), 2026-10-30: `apjobs_tests`
  passes on `windows-d3d12` and `windows-vulkan` in Debug, Release and
  RelWithDebInfo, and under both `-asan` presets (MSVC has no TSan); the
  pool's waiting thread makes no allocation (`aprinoalloc_tests`).

## Not done

- **No caller yet.** A caller takes an `APJOBS_SCHEDULER *` in its desc
  and copies it; whoever creates that caller implements the scheduler over
  its platform's pool (Grand Central Dispatch, the .NET thread pool) or
  creates the reference pool. Which callers come first is the workspace's
  `ROADMAP.md`.
- **Dependencies, priorities, a task that waits for another**: none. A
  caller orders tasks by waiting. If one is ever needed (a physics engine's
  job graph through this interface), it is a new callback chained on, and
  the pool learns to help with other tasks while it waits.
