// Headless tests for Rabbittus Jobs' groups (include/jobs/rbtjobs.h,
// RBTJOBS_SCHEDULER_GROUPS): the contract, as the pool and a caller's own
// scheduler have to meet it.
//
// C++ only for std::atomic and std::thread, which C has no portable form of
// on every compiler the workspace builds with; it calls the C API only, as
// rbtjobs_tests.c does.
//
// Written before the pool's groups (rbtjobs_groups_draft.h, approved
// 2026-11-06) and not to be edited by them: a pool whose scheduler has no
// groups chained on marks each test pending and ends it.
//
// It also holds the reference scheduler over a pool with no worker indices
// (rbtjobs.h, RBTJOBS_SCHEDULER_GROUPS): the shape a caller's scheduler over
// Grand Central Dispatch or the .NET thread pool takes, written here over an
// executor that starts a thread for every block submitted, as an
// overcommitting pool may.

#include "jobs/rbtjobs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

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

static void begin(const char *name)
{
    current_test = name;
    test_pending = false;
}

// The groups chained on a scheduler, or NULL (and the test pending).
static const RBTJOBS_SCHEDULER_GROUPS *groups_of(const RBTJOBS_SCHEDULER *scheduler)
{
    for (const RBT_STRUCT_BASE *b = scheduler->base.next; b; b = b->next)
        if (b->type == RBT_STRUCT_TYPE_JOBS_SCHEDULER_GROUPS)
            return (const RBTJOBS_SCHEDULER_GROUPS *)b;
    if (!test_pending)
        printf("PENDING %s: the scheduler has no groups\n", current_test);
    test_pending = true;
    return nullptr;
}

static RBTJOBS_POOL_DESC pool_desc(uint32_t thread_count, uint32_t max_tasks)
{
    RBTJOBS_POOL_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.base.type = RBT_STRUCT_TYPE_JOBS_POOL_DESC;
    desc.thread_count = thread_count;
    desc.max_tasks = max_tasks;
    desc.debug_name = "rbtjobs_groups_tests";
    return desc;
}

// A pool and its scheduler, for one test.
struct Pool
{
    rbtjobs_pool pool = nullptr;
    RBTJOBS_SCHEDULER scheduler{};
    const RBTJOBS_SCHEDULER_GROUPS *groups = nullptr;

    Pool(uint32_t thread_count, uint32_t max_tasks)
    {
        RBTJOBS_POOL_DESC desc = pool_desc(thread_count, max_tasks);
        CHECK(rbtjobs_pool_create(&desc, &pool, nullptr) == RBT_RESULT_OK);
        if (pool)
            CHECK(rbtjobs_pool_get_scheduler(pool, &scheduler) == RBT_RESULT_OK);
        if (pool)
            groups = groups_of(&scheduler);
    }
    ~Pool() { rbtjobs_pool_destroy(pool); }
};

// ---- The worker-index check ----
//
// Each range marks its index busy, holding its task's token, and finds it
// free or held by the task that enqueued it (whose range is paused while it
// runs nested), never by another.

#define MAX_WORKERS (RBTJOBS_POOL_MAX_THREADS + 1)

struct IndexCheck
{
    std::atomic<uint32_t> held[MAX_WORKERS];
    std::atomic<uint32_t> bad{0};
    std::atomic<uint32_t> nested{0};
    std::atomic<uint32_t> out_of_range{0};
    uint32_t worker_count = 0;

    void reset(uint32_t workers)
    {
        for (auto &h : held)
            h.store(0);
        bad.store(0);
        nested.store(0);
        out_of_range.store(0);
        worker_count = workers;
    }
    // Returns what the range must restore on leaving.
    uint32_t enter(uint32_t worker, uint32_t token, uint32_t parent_token)
    {
        if (worker >= worker_count)
        {
            out_of_range.fetch_add(1);
            return UINT32_MAX;
        }
        const uint32_t before = held[worker].exchange(token);
        if (before != 0)
        {
            nested.fetch_add(1);
            if (before != parent_token)
                bad.fetch_add(1);
        }
        return before;
    }
    void leave(uint32_t worker, uint32_t restore)
    {
        if (restore != UINT32_MAX)
            held[worker].store(restore);
    }
};

// ---- A tree: each task enqueues two until a depth ----

struct Tree;

struct TreeNode
{
    Tree *tree;
    uint32_t id;
};

struct Tree
{
    const RBTJOBS_SCHEDULER *scheduler = nullptr;
    const RBTJOBS_SCHEDULER_GROUPS *groups = nullptr;
    void *group = nullptr;
    uint32_t count = 0;
    std::vector<TreeNode> nodes;
    std::vector<uint32_t> hits; // each node's own slot, written by it alone
    IndexCheck *check = nullptr;
};

static void tree_task(uint32_t first, uint32_t end, uint32_t worker, void *data)
{
    TreeNode *node = (TreeNode *)data;
    Tree *t = node->tree;
    const uint32_t parent = node->id == 0 ? 0 : (node->id - 1) / 2 + 1;
    uint32_t restore = 0;
    if (t->check)
        restore = t->check->enter(worker, node->id + 1, parent);
    for (uint32_t i = first; i < end; ++i)
        t->hits[node->id] += 1;
    // Its children, after its own work: it keeps no scratch in use across
    // the calls (a range run there runs nested on this index). Its mark
    // stays, so a nested child finds its parent's.
    for (uint32_t c = node->id * 2 + 1; c <= node->id * 2 + 2; ++c)
        if (c < t->count)
            t->groups->group_enqueue(t->group, worker, tree_task, 1, 1, &t->nodes[c], t->scheduler->user);
    if (t->check)
        t->check->leave(worker, restore);
}

static void tree_init(Tree &t, const RBTJOBS_SCHEDULER *scheduler, const RBTJOBS_SCHEDULER_GROUPS *groups, void *group, uint32_t depth)
{
    t.scheduler = scheduler;
    t.groups = groups;
    t.group = group;
    // The root at depth 0: depth 12 is 8,191 tasks.
    t.count = (1u << (depth + 1)) - 1;
    t.nodes.resize(t.count);
    t.hits.assign(t.count, 0);
    for (uint32_t i = 0; i < t.count; ++i)
        t.nodes[i] = TreeNode{&t, i};
}

// Enqueues the root, waits, and checks every node ran once.
static void tree_run(Tree &t)
{
    for (auto &h : t.hits)
        h = 0;
    t.groups->group_enqueue(t.group, 0, tree_task, 1, 1, &t.nodes[0], t.scheduler->user);
    t.groups->group_wait(t.group, t.scheduler->user);
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < t.count; ++i)
        if (t.hits[i] != 1)
            ++wrong;
    CHECK(wrong == 0);
}

static void test_tree(void)
{
    begin("tree");
    const uint32_t threads[] = {0, 1, 3, 7};
    for (uint32_t th : threads)
    {
        Pool p(th, 64);
        if (!p.groups)
            return;
        void *group = p.groups->group_create(p.scheduler.user);
        CHECK(group != nullptr);
        if (!group)
            return;
        Tree t;
        tree_init(t, &p.scheduler, p.groups, group, 12);
        CHECK(t.count == 8191);
        tree_run(t);
        p.groups->group_destroy(group, p.scheduler.user);
    }
}

// ---- A chain: each task enqueues the next ----

struct Chain
{
    const RBTJOBS_SCHEDULER *scheduler = nullptr;
    const RBTJOBS_SCHEDULER_GROUPS *groups = nullptr;
    void *group = nullptr;
    uint32_t length = 0;
    std::atomic<uint32_t> ran{0};
    std::atomic<uint32_t> last{0};
    // Per worker: how deep the ranges on it run (a range run nested in
    // another for want of a slot is 2).
    std::atomic<uint32_t> depth[MAX_WORKERS];
    std::atomic<uint32_t> deepest{0};
};

struct ChainLink
{
    Chain *chain;
    uint32_t id;
};

static void chain_link_task(uint32_t, uint32_t, uint32_t worker, void *data)
{
    ChainLink *link = (ChainLink *)data;
    Chain *c = link->chain;
    const uint32_t d = c->depth[worker].fetch_add(1) + 1;
    uint32_t seen = c->deepest.load();
    while (d > seen && !c->deepest.compare_exchange_weak(seen, d))
    {
    }
    c->ran.fetch_add(1);
    c->last.store(link->id);
    c->depth[worker].fetch_sub(1);
    if (link->id + 1 < c->length)
        c->groups->group_enqueue(c->group, worker, chain_link_task, 1, 1, link + 1, c->scheduler->user);
}

static void chain_run(Chain &c, std::vector<ChainLink> &links)
{
    c.ran.store(0);
    c.last.store(0);
    c.deepest.store(0);
    for (auto &d : c.depth)
        d.store(0);
    c.groups->group_enqueue(c.group, 0, chain_link_task, 1, 1, &links[0], c.scheduler->user);
    c.groups->group_wait(c.group, c.scheduler->user);
    CHECK(c.ran.load() == c.length);
    CHECK(c.last.load() == c.length - 1);
}

static void chain_init(Chain &c, std::vector<ChainLink> &links, const RBTJOBS_SCHEDULER *s, const RBTJOBS_SCHEDULER_GROUPS *g, void *group,
                       uint32_t length)
{
    c.scheduler = s;
    c.groups = g;
    c.group = group;
    c.length = length;
    links.resize(length);
    for (uint32_t i = 0; i < length; ++i)
        links[i] = ChainLink{&c, i};
}

static void test_chain(void)
{
    begin("chain");
    Pool p(3, 16);
    if (!p.groups)
        return;
    void *group = p.groups->group_create(p.scheduler.user);
    Chain c;
    std::vector<ChainLink> links;
    chain_init(c, links, &p.scheduler, p.groups, group, 10000);
    chain_run(c, links);
    p.groups->group_destroy(group, p.scheduler.user);
}

// Slots come back: each task's slot is free by the time its successor's
// successor needs one, so with max_tasks 4 nothing runs nested.
static void test_slots_come_back(void)
{
    begin("slots_come_back");
    Pool p(3, 4);
    if (!p.groups)
        return;
    void *group = p.groups->group_create(p.scheduler.user);
    Chain c;
    std::vector<ChainLink> links;
    chain_init(c, links, &p.scheduler, p.groups, group, 10000);
    chain_run(c, links);
    CHECK(c.deepest.load() == 1);
    p.groups->group_destroy(group, p.scheduler.user);
}

// ---- A graph: layers of nodes, each depending on two of the layer before ----
//
// The test keeps the dependency counts; a node that ends lowers each
// dependent's, and enqueues the one it brings to zero. Each node's value is
// worked out from its dependencies' in a fixed order, so the bits are the
// same whatever ran where.

#define GRAPH_WIDTH 64
#define GRAPH_LAYERS 48

struct Graph;

struct GraphNode
{
    Graph *graph;
    uint32_t layer;
    uint32_t index;
};

struct Graph
{
    const RBTJOBS_SCHEDULER *scheduler = nullptr;
    const RBTJOBS_SCHEDULER_GROUPS *groups = nullptr;
    void *group = nullptr;
    GraphNode nodes[GRAPH_LAYERS][GRAPH_WIDTH];
    double value[GRAPH_LAYERS][GRAPH_WIDTH];
    std::atomic<uint32_t> waiting[GRAPH_LAYERS][GRAPH_WIDTH];
};

static void graph_task(uint32_t, uint32_t, uint32_t worker, void *data)
{
    GraphNode *n = (GraphNode *)data;
    Graph *g = n->graph;
    if (n->layer == 0)
    {
        g->value[0][n->index] = 1.0 / (double)(n->index + 3);
    }
    else
    {
        const double a = g->value[n->layer - 1][n->index];
        const double b = g->value[n->layer - 1][(n->index + 1) % GRAPH_WIDTH];
        g->value[n->layer][n->index] = a * 0.7 + b * 0.3 + (a * b) / 7.0;
    }
    if (n->layer + 1 == GRAPH_LAYERS)
        return;
    // Layer k+1's node j waits on layer k's j and j+1: this node is waited
    // on by j and j-1.
    const uint32_t dependents[2] = {n->index, (n->index + GRAPH_WIDTH - 1) % GRAPH_WIDTH};
    for (uint32_t d : dependents)
        if (g->waiting[n->layer + 1][d].fetch_sub(1, std::memory_order_acq_rel) == 1)
            g->groups->group_enqueue(g->group, worker, graph_task, 1, 1, &g->nodes[n->layer + 1][d], g->scheduler->user);
}

// Runs the graph; returns the last layer summed in item order.
static double graph_run(Graph &g)
{
    for (uint32_t l = 0; l < GRAPH_LAYERS; ++l)
        for (uint32_t i = 0; i < GRAPH_WIDTH; ++i)
        {
            g.nodes[l][i] = GraphNode{&g, l, i};
            g.value[l][i] = 0.0;
            g.waiting[l][i].store(2);
        }
    for (uint32_t i = 0; i < GRAPH_WIDTH; ++i)
        g.groups->group_enqueue(g.group, 0, graph_task, 1, 1, &g.nodes[0][i], g.scheduler->user);
    g.groups->group_wait(g.group, g.scheduler->user);
    double sum = 0.0;
    for (uint32_t i = 0; i < GRAPH_WIDTH; ++i)
        sum += g.value[GRAPH_LAYERS - 1][i];
    return sum;
}

static uint64_t bits_of(double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    return b;
}

static void test_graph(void)
{
    begin("graph");
    const uint32_t threads[] = {0, 1, 3, 7};
    uint64_t first = 0;
    bool have = false;
    for (uint32_t th : threads)
    {
        Pool p(th, 128);
        if (!p.groups)
            return;
        Graph *g = new Graph();
        g->scheduler = &p.scheduler;
        g->groups = p.groups;
        g->group = p.groups->group_create(p.scheduler.user);
        const uint64_t bits = bits_of(graph_run(*g));
        if (!have)
            first = bits;
        have = true;
        CHECK(bits == first);
        p.groups->group_destroy(g->group, p.scheduler.user);
        delete g;
    }
}

// ---- Reuse, two groups at once ----

static void test_reuse_and_two_groups(void)
{
    begin("reuse_and_two_groups");
    Pool p(3, 64);
    if (!p.groups)
        return;
    void *a = p.groups->group_create(p.scheduler.user);
    void *b = p.groups->group_create(p.scheduler.user);
    Tree ta, tb;
    tree_init(ta, &p.scheduler, p.groups, a, 6);
    tree_init(tb, &p.scheduler, p.groups, b, 6);
    for (uint32_t i = 0; i < 1000; ++i)
        tree_run(ta);
    // Both in flight, waited for in either order.
    for (uint32_t i = 0; i < 200; ++i)
    {
        for (auto &h : ta.hits)
            h = 0;
        for (auto &h : tb.hits)
            h = 0;
        p.groups->group_enqueue(a, 0, tree_task, 1, 1, &ta.nodes[0], p.scheduler.user);
        p.groups->group_enqueue(b, 0, tree_task, 1, 1, &tb.nodes[0], p.scheduler.user);
        if (i % 2)
        {
            p.groups->group_wait(a, p.scheduler.user);
            p.groups->group_wait(b, p.scheduler.user);
        }
        else
        {
            p.groups->group_wait(b, p.scheduler.user);
            p.groups->group_wait(a, p.scheduler.user);
        }
        uint32_t wrong = 0;
        for (uint32_t n = 0; n < ta.count; ++n)
            wrong += (ta.hits[n] != 1) + (tb.hits[n] != 1);
        CHECK(wrong == 0);
    }
    // A group with nothing enqueued returns at once.
    p.groups->group_wait(a, p.scheduler.user);
    p.groups->group_destroy(a, p.scheduler.user);
    p.groups->group_destroy(b, p.scheduler.user);
}

// ---- Past max_tasks: run where enqueued, as the index given ----

static void test_past_max_tasks(void)
{
    begin("past_max_tasks");
    static IndexCheck check;
    Pool p(7, 1);
    if (!p.groups)
        return;
    void *group = p.groups->group_create(p.scheduler.user);
    check.reset(p.scheduler.worker_count);
    Tree t;
    tree_init(t, &p.scheduler, p.groups, group, 10);
    t.check = &check;
    for (uint32_t i = 0; i < 20; ++i)
        tree_run(t);
    CHECK(check.bad.load() == 0);
    CHECK(check.out_of_range.load() == 0);
    // Most ran nested for want of the one slot.
    CHECK(check.nested.load() > 0);
    p.groups->group_destroy(group, p.scheduler.user);
}

// ---- The holder's calls overlapping a group's tasks ----

static void sum_task(uint32_t first, uint32_t end, uint32_t, void *data)
{
    uint64_t *out = (uint64_t *)data;
    for (uint32_t i = first; i < end; ++i)
        out[i] = (uint64_t)i * 3u + 1u;
}

static void test_overlap(void)
{
    begin("overlap");
    Pool p(3, 64);
    if (!p.groups)
        return;
    void *a = p.groups->group_create(p.scheduler.user);
    void *b = p.groups->group_create(p.scheduler.user);
    Tree ta, tb;
    tree_init(ta, &p.scheduler, p.groups, a, 8);
    tree_init(tb, &p.scheduler, p.groups, b, 5);
    std::vector<uint64_t> out(2000);
    for (uint32_t i = 0; i < 1000; ++i)
    {
        for (auto &h : ta.hits)
            h = 0;
        for (auto &h : tb.hits)
            h = 0;
        p.groups->group_enqueue(a, 0, tree_task, 1, 1, &ta.nodes[0], p.scheduler.user);
        // Plain tasks while the group's run.
        void *task = p.scheduler.enqueue(sum_task, (uint32_t)out.size(), 16, out.data(), p.scheduler.user);
        if (task)
            p.scheduler.wait(task, p.scheduler.user);
        uint32_t wrong = 0;
        for (uint32_t k = 0; k < out.size(); ++k)
            wrong += out[k] != (uint64_t)k * 3u + 1u;
        CHECK(wrong == 0);
        // A second group, enqueued and waited for, while the first runs.
        p.groups->group_enqueue(b, 0, tree_task, 1, 1, &tb.nodes[0], p.scheduler.user);
        p.groups->group_wait(b, p.scheduler.user);
        p.groups->group_wait(a, p.scheduler.user);
        wrong = 0;
        for (uint32_t n = 0; n < ta.count; ++n)
            wrong += ta.hits[n] != 1;
        for (uint32_t n = 0; n < tb.count; ++n)
            wrong += tb.hits[n] != 1;
        CHECK(wrong == 0);
    }
    p.groups->group_destroy(a, p.scheduler.user);
    p.groups->group_destroy(b, p.scheduler.user);
}

// ---- No thread: every task inside the wait, in the order enqueued ----

struct Order
{
    const RBTJOBS_SCHEDULER *scheduler;
    const RBTJOBS_SCHEDULER_GROUPS *groups;
    void *group;
    std::vector<uint32_t> ran;
    uint32_t next_id;
    uint32_t limit;
};

struct OrderNode
{
    Order *order;
    uint32_t id;
};

static void order_node_task(uint32_t, uint32_t, uint32_t worker, void *data)
{
    OrderNode *n = (OrderNode *)data;
    Order *o = n->order;
    CHECK(worker == 0);
    o->ran.push_back(n->id);
    // Each enqueues two more, numbered in the order they are enqueued.
    for (int k = 0; k < 2; ++k)
        if (o->next_id < o->limit)
        {
            OrderNode *child = n + (o->next_id - n->id);
            o->next_id += 1;
            o->groups->group_enqueue(o->group, worker, order_node_task, 1, 1, child, o->scheduler->user);
        }
}

static void test_no_thread_order(void)
{
    begin("no_thread_order");
    // A slot for every task, so none runs where it was enqueued.
    Pool p(0, 256);
    if (!p.groups)
        return;
    Order o{};
    o.scheduler = &p.scheduler;
    o.groups = p.groups;
    o.group = p.groups->group_create(p.scheduler.user);
    o.limit = 200;
    std::vector<OrderNode> nodes(o.limit);
    for (uint32_t i = 0; i < o.limit; ++i)
        nodes[i] = OrderNode{&o, i};
    o.next_id = 1;
    o.ran.reserve(o.limit);
    p.groups->group_enqueue(o.group, 0, order_node_task, 1, 1, &nodes[0], p.scheduler.user);
    // Nothing runs before the wait: there is no thread.
    CHECK(o.ran.empty());
    p.groups->group_wait(o.group, p.scheduler.user);
    CHECK(o.ran.size() == o.limit);
    bool in_order = o.ran.size() == o.limit;
    for (uint32_t i = 0; in_order && i < o.limit; ++i)
        in_order = o.ran[i] == i;
    CHECK(in_order);
    p.groups->group_destroy(o.group, p.scheduler.user);
}

// ---- The reference scheduler over a pool with no worker indices ----
//
// The executor below starts a thread for every block submitted, as a pool
// that adds threads may. The scheduler over it never lets more than
// worker_count - 1 blocks run at once, and each takes an index from 1 up;
// the waiting thread runs ranges as 0. Written as a caller's scheduler over
// Grand Central Dispatch or the .NET thread pool would be (rbtjobs.h,
// RBTJOBS_SCHEDULER_GROUPS), with dispatch_async_f / UnsafeQueueUserWorkItem
// where it calls submit().

struct RefTask
{
    RBTJOBS_TASK_FUNCTION function;
    void *task_data;
    struct RefGroup *group; // null for a plain task
    std::atomic<uint32_t> ranges_left;
};

struct RefRange
{
    RefTask *task;
    uint32_t first;
    uint32_t end;
};

struct RefGroup
{
    std::atomic<uint32_t> tasks_left{0};
};

struct RefScheduler
{
    uint32_t worker_count = 0;
    uint32_t start_delay_us = 0;
    std::mutex mutex;
    std::condition_variable changed;
    // The queue of ranges not yet run (a ring under the mutex).
    std::vector<RefRange> queue;
    size_t head = 0;
    size_t size = 0;
    // Blocks submitted and not finished; never above worker_count - 1.
    std::atomic<uint32_t> blocks{0};
    std::atomic<uint64_t> free_indices{0}; // bit i: index i free (1 and up)
    // Tasks, allocated by the test up front (a real one would pool them).
    std::unique_ptr<RefTask[]> tasks;
    std::atomic<uint32_t> next_task{0};
    // The executor's threads, joined at the end.
    std::mutex threads_mutex;
    std::vector<std::thread> threads;
    std::atomic<uint32_t> most_blocks{0};
};

static void ref_block(RefScheduler *s);

static void ref_submit(RefScheduler *s)
{
    std::lock_guard<std::mutex> lock(s->threads_mutex);
    s->threads.emplace_back(ref_block, s);
}

// Raises the count of blocks if below the limit; says whether it did.
static bool ref_take_block(RefScheduler *s)
{
    uint32_t n = s->blocks.load();
    while (n < s->worker_count - 1)
        if (s->blocks.compare_exchange_weak(n, n + 1))
        {
            uint32_t most = s->most_blocks.load();
            while (n + 1 > most && !s->most_blocks.compare_exchange_weak(most, n + 1))
            {
            }
            return true;
        }
    return false;
}

static bool ref_pop(RefScheduler *s, RefRange *out)
{
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->size == 0)
        return false;
    *out = s->queue[s->head];
    s->head = (s->head + 1) % s->queue.size();
    s->size -= 1;
    return true;
}

static void ref_run(RefScheduler *s, const RefRange &r, uint32_t index)
{
    r.task->function(r.first, r.end, index, r.task->task_data);
    if (r.task->ranges_left.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        RefGroup *g = r.task->group;
        std::lock_guard<std::mutex> lock(s->mutex);
        if (g)
            g->tasks_left.fetch_sub(1, std::memory_order_acq_rel);
        s->changed.notify_all();
    }
}

static void ref_block(RefScheduler *s)
{
    if (s->start_delay_us)
        std::this_thread::sleep_for(std::chrono::microseconds(s->start_delay_us));
    for (;;)
    {
        // A free index: there is always one, since there are never more
        // blocks than indices.
        uint64_t mask = s->free_indices.load();
        uint32_t index = 0;
        for (;;)
        {
            CHECK(mask != 0);
            if (mask == 0)
                return;
            index = 0;
            while (!(mask & (1ull << index)))
                ++index;
            if (s->free_indices.compare_exchange_weak(mask, mask & ~(1ull << index)))
                break;
        }
        RefRange r;
        while (ref_pop(s, &r))
            ref_run(s, r, index);
        s->free_indices.fetch_or(1ull << index);
        s->blocks.fetch_sub(1);
        // A second look: a range pushed after the last pop, while no block
        // was taking it, would otherwise wait for the waiter alone.
        bool more;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            more = s->size > 0;
        }
        if (!more || !ref_take_block(s))
            return;
    }
}

static RefTask *ref_push(RefScheduler *s, RBTJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t min_range, void *task_data,
                         RefGroup *group)
{
    RefTask *t = &s->tasks[s->next_task.fetch_add(1)];
    t->function = function;
    t->task_data = task_data;
    t->group = group;
    const uint32_t ranges = (item_count + min_range - 1) / min_range;
    t->ranges_left.store(ranges);
    if (group)
        group->tasks_left.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        for (uint32_t first = 0; first < item_count; first += min_range)
        {
            const uint32_t end = item_count - first < min_range ? item_count : first + min_range;
            s->queue[(s->head + s->size) % s->queue.size()] = RefRange{t, first, end};
            s->size += 1;
        }
        s->changed.notify_all();
    }
    for (uint32_t i = 0; i < ranges; ++i)
        if (ref_take_block(s))
            ref_submit(s);
        else
            break;
    return t;
}

// Runs ranges from the queue as index 0 until [done] says so.
template <typename Done> static void ref_wait_until(RefScheduler *s, Done done)
{
    for (;;)
    {
        if (done())
            return;
        RefRange r;
        if (ref_pop(s, &r))
        {
            ref_run(s, r, 0);
            continue;
        }
        std::unique_lock<std::mutex> lock(s->mutex);
        s->changed.wait(lock, [&] { return s->size > 0 || done(); });
    }
}

static void *ref_enqueue(RBTJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t min_range, void *task_data, void *user)
{
    return ref_push((RefScheduler *)user, function, item_count, min_range, task_data, nullptr);
}

static void ref_wait(void *task, void *user)
{
    RefTask *t = (RefTask *)task;
    ref_wait_until((RefScheduler *)user, [t] { return t->ranges_left.load(std::memory_order_acquire) == 0; });
}

static void *ref_group_create(void *)
{
    return new RefGroup();
}

static void ref_group_destroy(void *group, void *)
{
    delete (RefGroup *)group;
}

static void ref_group_enqueue(void *group, uint32_t, RBTJOBS_TASK_FUNCTION function, uint32_t item_count, uint32_t min_range, void *task_data,
                              void *user)
{
    // Never runs a range here, so the worker index is never needed.
    ref_push((RefScheduler *)user, function, item_count, min_range, task_data, (RefGroup *)group);
}

static void ref_group_wait(void *group, void *user)
{
    RefGroup *g = (RefGroup *)group;
    ref_wait_until((RefScheduler *)user, [g] { return g->tasks_left.load(std::memory_order_acquire) == 0; });
}

struct RefSetup
{
    RefScheduler s;
    RBTJOBS_SCHEDULER_GROUPS groups{};
    RBTJOBS_SCHEDULER scheduler{};

    RefSetup(uint32_t worker_count, uint32_t max_tasks, uint32_t delay_us)
    {
        s.worker_count = worker_count;
        s.start_delay_us = delay_us;
        s.queue.resize(max_tasks + 1);
        s.tasks.reset(new RefTask[max_tasks]);
        uint64_t mask = 0;
        for (uint32_t i = 1; i < worker_count; ++i)
            mask |= 1ull << i;
        s.free_indices.store(mask);
        groups.base.type = RBT_STRUCT_TYPE_JOBS_SCHEDULER_GROUPS;
        groups.group_create = ref_group_create;
        groups.group_destroy = ref_group_destroy;
        groups.group_enqueue = ref_group_enqueue;
        groups.group_wait = ref_group_wait;
        scheduler.base.type = RBT_STRUCT_TYPE_JOBS_SCHEDULER;
        scheduler.base.next = &groups.base;
        scheduler.worker_count = worker_count;
        scheduler.user = &s;
        scheduler.enqueue = ref_enqueue;
        scheduler.wait = ref_wait;
    }
    ~RefSetup()
    {
        // Every block's thread has ended, or is ending; join them all.
        std::lock_guard<std::mutex> lock(s.threads_mutex);
        for (auto &t : s.threads)
            t.join();
    }
};

static void test_reference_scheduler(void)
{
    begin("reference_scheduler");
    const uint32_t workers[] = {2, 4, 8};
    for (uint32_t w : workers)
    {
        static IndexCheck check;
        check.reset(w);
        {
            RefSetup r(w, 8191 + 16, 0);
            CHECK(rbtjobs_scheduler_check(&r.scheduler, nullptr) == RBT_RESULT_OK);
            void *group = r.groups.group_create(&r.s);
            Tree t;
            tree_init(t, &r.scheduler, &r.groups, group, 12);
            t.check = &check;
            tree_run(t);
            CHECK(check.nested.load() == 0);
            CHECK(check.bad.load() == 0);
            CHECK(check.out_of_range.load() == 0);
            CHECK(r.s.most_blocks.load() <= w - 1);
            r.groups.group_destroy(group, &r.s);
        }
        {
            RefSetup r(w, 10000 + 16, 0);
            void *group = r.groups.group_create(&r.s);
            Chain c;
            std::vector<ChainLink> links;
            chain_init(c, links, &r.scheduler, &r.groups, group, 10000);
            chain_run(c, links);
            CHECK(c.deepest.load() == 1);
            r.groups.group_destroy(group, &r.s);
        }
        {
            RefSetup r(w, GRAPH_LAYERS * GRAPH_WIDTH + 16, 0);
            Graph *g = new Graph();
            g->scheduler = &r.scheduler;
            g->groups = &r.groups;
            g->group = r.groups.group_create(&r.s);
            const uint64_t bits = bits_of(graph_run(*g));
            Pool none(0, 4);
            if (none.groups)
            {
                Graph *h = new Graph();
                h->scheduler = &none.scheduler;
                h->groups = none.groups;
                h->group = none.groups->group_create(none.scheduler.user);
                CHECK(bits == bits_of(graph_run(*h)));
                none.groups->group_destroy(h->group, none.scheduler.user);
                delete h;
            }
            r.groups.group_destroy(g->group, &r.s);
            delete g;
        }
    }
    // Blocks that start late, so ranges come in while every block is
    // leaving: the second look picks them up.
    {
        RefSetup r(4, 2000 + 16, 200);
        void *group = r.groups.group_create(&r.s);
        Chain c;
        std::vector<ChainLink> links;
        chain_init(c, links, &r.scheduler, &r.groups, group, 2000);
        chain_run(c, links);
        r.groups.group_destroy(group, &r.s);
    }
}

int main(void)
{
    test_tree();
    test_chain();
    test_slots_come_back();
    test_graph();
    test_reuse_and_two_groups();
    test_past_max_tasks();
    test_overlap();
    test_no_thread_order();
    test_reference_scheduler();
    if (failures)
    {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("rbtjobs_groups_tests: all passed\n");
    return 0;
}
