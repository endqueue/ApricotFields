#include "math/rbtmathbvh.h"
#include "../rbtoverlap.hpp"
#include "../rbtplaindata.hpp"
#include "../rbtstale.hpp"
#include "../rbtstructbase.hpp"
#include "rbtmathbvh_internal.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <eqstd.hpp>
#include <glm/glm.hpp>
#include <limits>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Every level is the same tree over boxes (bvh_node_t): a mesh's over its
// triangles' boxes, an instance BVH's over its instances', a box BVH's over
// the caller's boxes. Built with a binned surface-area heuristic, walked
// nearest child first. An instance or box BVH keeps its tree's memory and
// the build's scratch for its life, so a refit and a rebuild allocate
// nothing (dyn_tree_t).
//
// Built without floating-point contraction (CMakeLists.txt), so every query
// gives the same bits on every platform.

static const double INF = std::numeric_limits<double>::infinity();

// How far outside a triangle, in barycentric units, a ray still hits it: two
// triangles sharing an edge leave no crack for a ray to slip through.
// Triangles' boxes are padded to match.
static const double EDGE_TOLERANCE = 1e-12;
// Rays' box tests are widened by this, relative to t, so rounding never
// culls a box the ray touches.
static const double BOX_SLACK = 4.0 * DBL_EPSILON;

static const uint32_t MESH_LEAF_SIZE = 4;
static const uint32_t INSTANCE_LEAF_SIZE = 2;
static const uint32_t SAH_BINS = 16;
// Past this depth a build splits at the median instead, which halves a node
// every level: at most 32 more for 2^32 boxes, and one more for the split
// between boxes and empty entries (build_tree()). So no tree is deeper than
// 94, and neither a build's nor a walk's stack holds more than STACK_SIZE.
static const uint32_t SAH_DEPTH_LIMIT = 60;
static const uint32_t STACK_SIZE = 128;
// No node.
static const uint32_t NO_NODE = UINT32_MAX;

// GJK (rbtmathbvh.h, "Shapes"): a shape and a triangle are touching when
// their cores are within the shape's radius plus this, relative to the
// larger of their sizes; and a run that hasn't converged in this many
// iterations answers touching, at the t it has reached.
static const double GJK_TOLERANCE = 1e-12;
static const uint32_t GJK_MAX_ITERATIONS = 64;
// How far from unit length a shape's rotation may be (RBTMATH_SHAPE).
static const double ROTATION_TOLERANCE = 1e-6;

// ---- Boxes --------------------------------------------------------------------

namespace
{
struct box_t
{
    glm::dvec3 lo{INF}, hi{-INF};

    void add(const glm::dvec3 &point)
    {
        lo = glm::min(lo, point);
        hi = glm::max(hi, point);
    }
    void add(const box_t &box)
    {
        lo = glm::min(lo, box.lo);
        hi = glm::max(hi, box.hi);
    }
    bool empty() const
    {
        return lo.x > hi.x || lo.y > hi.y || lo.z > hi.z;
    }
    double area() const
    {
        glm::dvec3 d = hi - lo;
        return 2.0 * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
    double max_extent() const
    {
        glm::dvec3 d = hi - lo;
        return std::max(d.x, std::max(d.y, d.z));
    }
    void pad(double amount)
    {
        lo -= glm::dvec3(amount);
        hi += glm::dvec3(amount);
    }
};
RBT_PLAIN_DATA(box_t);

// A leaf if [count] isn't 0: entries order[first .. first + count).
// Otherwise its children are nodes [first] and [first] + 1, both after it.
struct bvh_node_t
{
    glm::dvec3 lo{0.0}, hi{0.0};
    uint32_t first{0};
    uint32_t count{0};
};
RBT_PLAIN_DATA(bvh_node_t);
RBT_HOT_RECORD(bvh_node_t, 56);

struct ray_t
{
    glm::dvec3 origin, direction, inverse;
};
} // namespace

static bool is_finite(const glm::dvec3 &v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static glm::dvec3 to_glm(const RbtDVec3 &v)
{
    return glm::dvec3(v.x, v.y, v.z);
}

static RbtDVec3 from_glm(const glm::dvec3 &v)
{
    return RbtDVec3{v.x, v.y, v.z};
}

static RBTMATH_AABB to_aabb(const box_t &box)
{
    return RBTMATH_AABB{from_glm(box.lo), from_glm(box.hi)};
}

// The area a tree's cost counts: 0 for an empty box.
static double area_of(const glm::dvec3 &lo, const glm::dvec3 &hi)
{
    if (lo.x > hi.x || lo.y > hi.y || lo.z > hi.z)
        return 0.0;
    glm::dvec3 d = hi - lo;
    return 2.0 * (d.x * d.y + d.y * d.z + d.z * d.x);
}

static ray_t make_ray(const glm::dvec3 &origin, const glm::dvec3 &direction)
{
    // A zero component's inverse is never used (see clip_to_box()).
    return ray_t{origin, direction, 1.0 / direction};
}

// Narrows [t_min, t_max] to the part of [ray] inside the box, widened by
// [slack] relative to t. False if none of it is, and for an empty box.
static bool clip_to_box(const ray_t &ray, const glm::dvec3 &lo, const glm::dvec3 &hi, double slack, double &t_min,
                        double &t_max)
{
    // Empty: an entry with no box, or a subtree of them. Every empty box is
    // box_t's, empty on every axis, so one axis tells.
    if (lo.x > hi.x)
        return false;
    for (int axis = 0; axis < 3; axis++)
    {
        if (ray.direction[axis] == 0.0)
        {
            if (ray.origin[axis] < lo[axis] || ray.origin[axis] > hi[axis])
                return false;
            continue;
        }
        double t1 = (lo[axis] - ray.origin[axis]) * ray.inverse[axis];
        double t2 = (hi[axis] - ray.origin[axis]) * ray.inverse[axis];
        if (t1 > t2)
            std::swap(t1, t2);
        if (slack != 0.0)
        {
            t1 -= std::abs(t1) * slack;
            t2 += std::abs(t2) * slack;
        }
        if (t1 > t_min)
            t_min = t1;
        if (t2 < t_max)
            t_max = t2;
        if (!(t_min <= t_max))
            return false;
    }
    return true;
}

static bool boxes_meet(const glm::dvec3 &lo, const glm::dvec3 &hi, const glm::dvec3 &other_lo,
                       const glm::dvec3 &other_hi)
{
    return lo.x <= other_hi.x && lo.y <= other_hi.y && lo.z <= other_hi.z && other_lo.x <= hi.x &&
           other_lo.y <= hi.y && other_lo.z <= hi.z;
}

// True, with the ray as the casts use it, if [ray] can hit anything.
static bool read_ray(const RBTMATH_RAY *ray, ray_t &out)
{
    if (!ray)
        return false;
    glm::dvec3 origin = to_glm(ray->origin), direction = to_glm(ray->direction);
    if (!is_finite(origin) || !is_finite(direction) || direction == glm::dvec3(0.0))
        return false;
    if (!(ray->t_min <= ray->t_max)) // also either being NaN
        return false;
    out = make_ray(origin, direction);
    return true;
}

// ---- Building -----------------------------------------------------------------

// Reorders order[first .. first + count) so its first part and the rest
// make two good children, and returns where the rest starts - never at
// either end. [centroid_bounds] is around the range's centroids.
static uint32_t split_range(const box_t *boxes, const glm::dvec3 *centroids, uint32_t *order, uint32_t first,
                            uint32_t count, const box_t &centroid_bounds, uint32_t depth)
{
    uint32_t *begin = order + first, *end = begin + count;
    if (depth < SAH_DEPTH_LIMIT)
    {
        double best_cost = INF;
        int best_axis = -1;
        uint32_t best_bin = 0;
        double best_scale = 0.0;
        for (int axis = 0; axis < 3; axis++)
        {
            double extent = centroid_bounds.hi[axis] - centroid_bounds.lo[axis];
            if (!(extent > 0.0) || !std::isfinite(extent))
                continue;
            double scale = SAH_BINS / extent;
            if (!std::isfinite(scale))
                continue;
            box_t bin_boxes[SAH_BINS];
            uint32_t bin_counts[SAH_BINS] = {};
            for (uint32_t *it = begin; it != end; ++it)
            {
                uint32_t bin = std::min(SAH_BINS - 1,
                                        (uint32_t)((centroids[*it][axis] - centroid_bounds.lo[axis]) * scale));
                bin_boxes[bin].add(boxes[*it]);
                bin_counts[bin]++;
            }
            // The cost of splitting before bin i + 1: each side's area times
            // how many boxes it holds.
            double right_areas[SAH_BINS - 1];
            uint32_t right_counts[SAH_BINS - 1];
            box_t side;
            uint32_t side_count = 0;
            for (uint32_t i = SAH_BINS - 1; i > 0; i--)
            {
                side.add(bin_boxes[i]);
                side_count += bin_counts[i];
                right_areas[i - 1] = side_count ? side.area() : 0.0;
                right_counts[i - 1] = side_count;
            }
            side = box_t();
            side_count = 0;
            for (uint32_t i = 0; i < SAH_BINS - 1; i++)
            {
                side.add(bin_boxes[i]);
                side_count += bin_counts[i];
                if (!side_count || !right_counts[i])
                    continue;
                double cost = side_count * side.area() + right_counts[i] * right_areas[i];
                if (cost < best_cost)
                {
                    best_cost = cost;
                    best_axis = axis;
                    best_bin = i + 1;
                    best_scale = scale;
                }
            }
        }
        if (best_axis >= 0)
        {
            double lo = centroid_bounds.lo[best_axis];
            uint32_t *middle = std::partition(begin, end, [&](uint32_t index) {
                return std::min(SAH_BINS - 1, (uint32_t)((centroids[index][best_axis] - lo) * best_scale)) < best_bin;
            });
            return first + (uint32_t)(middle - begin);
        }
    }
    // The centroids coincide, the areas aren't finite, or the tree is deep:
    // half the boxes each side, along the axis they're most spread on.
    glm::dvec3 spread = centroid_bounds.hi - centroid_bounds.lo;
    int axis = spread.y > spread.x ? 1 : 0;
    if (spread.z > spread[axis])
        axis = 2;
    uint32_t *middle = begin + count / 2;
    std::nth_element(begin, middle, end,
                     [&](uint32_t a, uint32_t b) { return centroids[a][axis] < centroids[b][axis]; });
    return first + count / 2;
}

// Builds a tree over boxes[0 .. box_count) into [nodes], which has room for
// 2 * box_count - 1 of them, with [order] (box_count) and [centroids]
// (box_count) as its working space, and returns how many nodes it made.
// Allocates nothing. Empty boxes - an instance with no mesh, a box the
// caller emptied - aren't left out: they go in a subtree of their own, split
// in half by position, so a refit can carry one up the day it gains a box,
// and the rest are built as if they weren't there. The same boxes give the
// same tree, node for node.
static uint32_t build_tree(const box_t *boxes, uint32_t box_count, uint32_t leaf_size, glm::dvec3 *centroids,
                           uint32_t *order, bvh_node_t *nodes)
{
    if (!box_count)
        return 0;
    uint32_t filled = 0;
    for (uint32_t i = 0; i < box_count; i++)
        if (!boxes[i].empty())
            order[filled++] = i;
    uint32_t empties = filled;
    for (uint32_t i = 0; i < box_count; i++)
        if (boxes[i].empty())
            order[empties++] = i;
    for (uint32_t i = 0; i < box_count; i++)
        centroids[i] = boxes[i].empty() ? glm::dvec3(0.0) : boxes[i].lo * 0.5 + boxes[i].hi * 0.5;

    struct task_t
    {
        uint32_t node, first, count, depth;
        bool empty;
    };
    task_t tasks[STACK_SIZE];
    uint32_t top = 0;
    uint32_t node_count = 1;
    nodes[0] = bvh_node_t{};
    if (filled && filled < box_count)
    {
        // Both kinds: the root's children are the boxes and the empties.
        nodes[1] = bvh_node_t{};
        nodes[2] = bvh_node_t{};
        node_count = 3;
        box_t bounds;
        for (uint32_t i = 0; i < filled; i++)
            bounds.add(boxes[order[i]]);
        nodes[0].lo = bounds.lo;
        nodes[0].hi = bounds.hi;
        nodes[0].first = 1;
        nodes[0].count = 0;
        tasks[top++] = {2, filled, box_count - filled, 0, true};
        tasks[top++] = {1, 0, filled, 0, false};
    }
    else
        tasks[top++] = {0, 0, box_count, 0, filled == 0};
    while (top)
    {
        task_t task = tasks[--top];
        box_t bounds, centroid_bounds;
        for (uint32_t i = task.first; i < task.first + task.count; i++)
        {
            bounds.add(boxes[order[i]]);
            if (!task.empty)
                centroid_bounds.add(centroids[order[i]]);
        }
        nodes[task.node].lo = bounds.lo;
        nodes[task.node].hi = bounds.hi;
        if (task.count <= leaf_size)
        {
            nodes[task.node].first = task.first;
            nodes[task.node].count = task.count;
            continue;
        }
        uint32_t middle = task.empty ? task.first + task.count / 2
                                     : split_range(boxes, centroids, order, task.first, task.count, centroid_bounds,
                                                   task.depth);
        uint32_t left = node_count;
        nodes[left] = bvh_node_t{};
        nodes[left + 1] = bvh_node_t{};
        node_count += 2;
        nodes[task.node].first = left;
        nodes[task.node].count = 0;
        tasks[top++] = {left, task.first, middle - task.first, task.depth + 1, task.empty};
        tasks[top++] = {left + 1, middle, task.first + task.count - middle, task.depth + 1, task.empty};
    }
    return node_count;
}

// ---- Walking ------------------------------------------------------------------

// clip_to_box() on a node's box, grown by [grow] on every side with [Grow].
template <bool Grow>
static bool clip_node(const ray_t &ray, const glm::dvec3 &lo, const glm::dvec3 &hi, const glm::dvec3 &grow,
                      double &t_min, double &t_max)
{
    if constexpr (Grow)
        return clip_to_box(ray, lo - grow, hi + grow, BOX_SLACK, t_min, t_max);
    else
        return clip_to_box(ray, lo, hi, BOX_SLACK, t_min, t_max);
}

// Calls leaf(first, count) for each leaf whose box [ray] enters within
// [t_min, t_max], nearest first. [t_max] is read again after every leaf, so
// a leaf that finds a hit shrinks it and the walk skips what's beyond. With
// [Grow], each box is first grown by [grow] on every side: a sweep's walk,
// the ray its shape's center takes against boxes grown by the shape's own.
template <bool Grow, typename Leaf>
static void walk_tree(const bvh_node_t *nodes, uint32_t node_count, const ray_t &ray, const glm::dvec3 &grow,
                      double t_min, const double &t_max, Leaf &&leaf)
{
    if (!node_count)
        return;
    struct entry_t
    {
        uint32_t node;
        double enter;
    };
    entry_t stack[STACK_SIZE];
    uint32_t top = 0;
    double enter = t_min, leave = t_max;
    if (!clip_node<Grow>(ray, nodes[0].lo, nodes[0].hi, grow, enter, leave))
        return;
    stack[top++] = {0, enter};
    while (top)
    {
        entry_t entry = stack[--top];
        if (entry.enter > t_max)
            continue;
        const bvh_node_t &node = nodes[entry.node];
        if (node.count)
        {
            leaf(node.first, node.count);
            continue;
        }
        const bvh_node_t &a = nodes[node.first], &b = nodes[node.first + 1];
        double enter_a = t_min, leave_a = t_max, enter_b = t_min, leave_b = t_max;
        bool hit_a = clip_node<Grow>(ray, a.lo, a.hi, grow, enter_a, leave_a);
        bool hit_b = clip_node<Grow>(ray, b.lo, b.hi, grow, enter_b, leave_b);
        if (hit_a && hit_b)
        {
            // The nearer one last, so it's popped first.
            if (enter_a <= enter_b)
            {
                stack[top++] = {node.first + 1, enter_b};
                stack[top++] = {node.first, enter_a};
            }
            else
            {
                stack[top++] = {node.first, enter_a};
                stack[top++] = {node.first + 1, enter_b};
            }
        }
        else if (hit_a)
            stack[top++] = {node.first, enter_a};
        else if (hit_b)
            stack[top++] = {node.first + 1, enter_b};
    }
}

// Calls node(index) for nodes of a tree, from the root, each node's
// children only if node() returns true for it; stops when [stop] is set.
template <typename Node>
static void walk_nodes(const bvh_node_t *nodes, uint32_t node_count, const bool &stop, Node &&node)
{
    if (!node_count)
        return;
    uint32_t stack[STACK_SIZE];
    uint32_t top = 0;
    stack[top++] = 0;
    while (top && !stop)
    {
        uint32_t index = stack[--top];
        if (!node(index))
            continue;
        const bvh_node_t &n = nodes[index];
        if (n.count)
            continue;
        stack[top++] = n.first + 1;
        stack[top++] = n.first;
    }
}

// ---- A tree that changes in place ---------------------------------------------

namespace
{
// An instance or box BVH's tree over its entries, every array sized at
// creation for the BVH's life: set_*, refit and rebuild allocate nothing.
// Read by the queries (boxes, nodes, order); written by set_* (one entry's
// boxes and marks each), refit and rebuild (the rest).
struct dyn_tree_t
{
    uint32_t entry_count{0};
    // Each entry's box as the tree holds it: padded for rounding, empty for
    // an entry with nothing to meet. Read by every query's leaves.
    eq::array<box_t> boxes;
    eq::array<bvh_node_t> nodes; // room for 2 * entry_count - 1
    uint32_t node_count{0};
    eq::array<uint32_t> order;   // entries, leaf by leaf
    eq::array<uint32_t> parent;  // per node; NO_NODE for the root
    eq::array<uint32_t> leaf_of; // per entry, the leaf holding it
    // The rebuild's scratch.
    eq::array<glm::dvec3> centroids;
    // A byte an entry: set since the last refit or rebuild (refit's marks).
    eq::array<uint8_t> moved;
    // A byte an entry: seen by the set call checking it (its check's marks),
    // kept apart from refit's so a set refused clears only its own.
    eq::array<uint8_t> checking;
    // A byte a node: to be worked out again by the refit under way.
    eq::array<uint8_t> dirty;
    // A byte an entry: whether its box wasn't empty at the last refit or
    // rebuild, and how many weren't.
    eq::array<uint8_t> filled;
    uint32_t filled_count{0};
    // The sum of the inner nodes' areas (area_of()), kept by refit.
    double inner_area{0.0};
};
} // namespace

// Sizes every array of [tree] for [count] entries. False if out of memory.
static bool tree_allocate(dyn_tree_t &tree, uint32_t count)
{
    tree.entry_count = count;
    size_t nodes = count ? (size_t)count * 2 - 1 : 0;
    if (tree.boxes.resize(count) != EQ_SUCCESS)
        return false;
    if (tree.nodes.resize(nodes) != EQ_SUCCESS)
        return false;
    if (tree.order.resize(count) != EQ_SUCCESS)
        return false;
    if (tree.parent.resize(nodes) != EQ_SUCCESS)
        return false;
    if (tree.leaf_of.resize(count) != EQ_SUCCESS)
        return false;
    if (tree.centroids.resize(count) != EQ_SUCCESS)
        return false;
    if (tree.moved.resize(count) != EQ_SUCCESS)
        return false;
    if (tree.checking.resize(count) != EQ_SUCCESS)
        return false;
    if (tree.dirty.resize(nodes) != EQ_SUCCESS)
        return false;
    if (tree.filled.resize(count) != EQ_SUCCESS)
        return false;
    if (count)
    {
        memset(tree.moved.data, 0, count);
        memset(tree.checking.data, 0, count);
        memset(tree.dirty.data, 0, nodes);
    }
    return true;
}

// Builds [tree] again over its boxes as they are: what a create builds.
static void tree_rebuild(dyn_tree_t &tree, uint32_t leaf_size)
{
    tree.node_count = build_tree(tree.boxes.data, tree.entry_count, leaf_size, tree.centroids.data, tree.order.data,
                                 tree.nodes.data);
    tree.inner_area = 0.0;
    tree.filled_count = 0;
    for (uint32_t i = 0; i < tree.entry_count; i++)
    {
        tree.filled[i] = !tree.boxes[i].empty();
        tree.filled_count += tree.filled[i];
        tree.moved[i] = 0;
    }
    if (tree.node_count)
        tree.parent[0] = NO_NODE;
    for (uint32_t n = 0; n < tree.node_count; n++)
    {
        const bvh_node_t &node = tree.nodes[n];
        tree.dirty[n] = 0;
        if (node.count)
        {
            for (uint32_t i = node.first; i < node.first + node.count; i++)
                tree.leaf_of[tree.order[i]] = n;
            continue;
        }
        tree.parent[node.first] = n;
        tree.parent[node.first + 1] = n;
        tree.inner_area += area_of(node.lo, node.hi);
    }
}

// Carries the boxes of the entries set since the last refit or rebuild up
// [tree]: marks every node above one, then works the marked ones out again
// from the last to the first, children (which come after their parent)
// before parents.
static void tree_refit(dyn_tree_t &tree)
{
    bool any = false;
    for (uint32_t i = 0; i < tree.entry_count; i++)
    {
        if (!tree.moved[i])
            continue;
        tree.moved[i] = 0;
        any = true;
        uint8_t now = !tree.boxes[i].empty();
        tree.filled_count = tree.filled_count - tree.filled[i] + now;
        tree.filled[i] = now;
        for (uint32_t n = tree.leaf_of[i]; n != NO_NODE && !tree.dirty[n]; n = tree.parent[n])
            tree.dirty[n] = 1;
    }
    if (!any)
        return;
    for (uint32_t n = tree.node_count; n-- > 0;)
    {
        if (!tree.dirty[n])
            continue;
        tree.dirty[n] = 0;
        bvh_node_t &node = tree.nodes[n];
        box_t box;
        if (node.count)
        {
            for (uint32_t i = node.first; i < node.first + node.count; i++)
                box.add(tree.boxes[tree.order[i]]);
            node.lo = box.lo;
            node.hi = box.hi;
            continue;
        }
        double before = area_of(node.lo, node.hi);
        const bvh_node_t &a = tree.nodes[node.first], &b = tree.nodes[node.first + 1];
        node.lo = glm::min(a.lo, b.lo);
        node.hi = glm::max(a.hi, b.hi);
        tree.inner_area += area_of(node.lo, node.hi) - before;
    }
    if (tree.inner_area < 0.0)
        tree.inner_area = 0.0;
}

static double tree_cost(const dyn_tree_t &tree)
{
    if (tree.filled_count < 2 || !tree.node_count)
        return 0.0;
    double root = area_of(tree.nodes[0].lo, tree.nodes[0].hi);
    if (!(root > 0.0))
        return 0.0;
    return tree.inner_area / root;
}

// ---- Errors ---------------------------------------------------------------------

static RBT_RESULT fail(RBTMATH_ERROR *error, RBT_RESULT result, const char *format, ...)
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

static void clear(RBTMATH_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// ---- Objects ------------------------------------------------------------------

namespace
{
// A triangle as casts read it: a corner and the two edges from it.
struct triangle_t
{
    glm::dvec3 v0, e1, e2;
};
RBT_PLAIN_DATA(triangle_t);

// A hit inside one mesh, before its point and normal are worked out.
struct mesh_hit_t
{
    uint32_t slot{0};     // in triangles
    uint32_t triangle{0}; // in the desc's order
    double t{0.0};
    bool back_face{false};
};
} // namespace

struct rbtmath_mesh_bvh_t
{
    const char *debug_name{nullptr};
    RBT_STALE_TAG
    // The desc's triangles, degenerate ones included.
    uint32_t triangle_count{0};
    box_t bounds;
    // The ones with an area, in the tree's leaf order, and each one's
    // position in the desc's order.
    eq::array<triangle_t> triangles;
    eq::array<uint32_t> triangle_indices;
    // A leaf's range is in slots of [triangles].
    eq::array<bvh_node_t> nodes;
    uint32_t node_count{0};
};

namespace
{
// Mesh coordinates to the instance BVH's, and back.
struct instance_t
{
    const rbtmath_mesh_bvh_t *mesh{nullptr};
    uint64_t id{0};
    uint32_t mask{0};
    bool placed{false}; // has a transform
    // The instance BVH's coordinates to the mesh's.
    glm::dmat3 inverse_linear{1.0};
    glm::dvec3 inverse_translation{0.0};
    // The mesh's to the instance BVH's: a sweep's triangles are placed by it.
    glm::dmat3 linear{1.0};
    glm::dvec3 translation{0.0};
};
RBT_PLAIN_DATA(instance_t);
} // namespace

struct rbtmath_instance_bvh_t
{
    const char *debug_name{nullptr};
    RBT_STALE_TAG
    rbt_shared_overlap guard;
    eq::array<instance_t> instances; // the desc's order
    // Around each placed mesh, unpadded: what get_bounds() reports.
    eq::array<box_t> raw_boxes;
    // Over every instance; its order holds their indices.
    dyn_tree_t tree;
};

namespace
{
struct box_entry_t
{
    box_t box; // exact, as the caller gave it; empty is lo above hi
    uint64_t id;
    uint32_t mask;
};
RBT_PLAIN_DATA(box_entry_t);
} // namespace

struct rbtmath_box_bvh_t
{
    const char *debug_name{nullptr};
    RBT_STALE_TAG
    rbt_shared_overlap guard;
    eq::array<box_entry_t> entries; // the desc's order
    dyn_tree_t tree;
};

rbt_shared_overlap &rbtmath_instance_bvh_guard(rbtmath_instance_bvh bvh)
{
    return bvh->guard;
}

rbt_shared_overlap &rbtmath_box_bvh_guard(rbtmath_box_bvh bvh)
{
    return bvh->guard;
}

// ---- Casting ------------------------------------------------------------------

// True if [candidate] is the hit to keep over [best]: nearer, or as near and
// earlier in the caller's order.
static bool is_better(double t, uint32_t index, double best_t, uint32_t best_index)
{
    return t < best_t || (t == best_t && index < best_index);
}

// The mesh's nearest hit within [t_min, t_max], in its own coordinates.
static bool cast_mesh(const rbtmath_mesh_bvh_t &mesh, const ray_t &ray, double t_min, double t_max, mesh_hit_t &out_hit)
{
    bool found = false;
    const glm::dvec3 no_grow(0.0);
    walk_tree<false>(mesh.nodes.data, mesh.node_count, ray, no_grow, t_min, t_max, [&](uint32_t first, uint32_t count) {
        for (uint32_t slot = first; slot < first + count; slot++)
        {
            // Moller-Trumbore, both sides.
            const triangle_t &triangle = mesh.triangles[slot];
            glm::dvec3 p = glm::cross(ray.direction, triangle.e2);
            double det = glm::dot(triangle.e1, p);
            if (det == 0.0)
                continue; // the ray runs along the triangle's plane
            double inverse_det = 1.0 / det;
            glm::dvec3 s = ray.origin - triangle.v0;
            double u = glm::dot(s, p) * inverse_det;
            glm::dvec3 q = glm::cross(s, triangle.e1);
            double v = glm::dot(ray.direction, q) * inverse_det;
            // Written to fail on NaN too.
            if (!(u >= -EDGE_TOLERANCE && v >= -EDGE_TOLERANCE && u + v <= 1.0 + EDGE_TOLERANCE))
                continue;
            double t = glm::dot(triangle.e2, q) * inverse_det;
            if (!(t >= t_min && t <= t_max))
                continue;
            uint32_t index = mesh.triangle_indices[slot];
            if (found && !is_better(t, index, out_hit.t, out_hit.triangle))
                continue;
            found = true;
            out_hit.slot = slot;
            out_hit.triangle = index;
            out_hit.t = t;
            // det is -direction . normal: positive when the ray meets the front.
            out_hit.back_face = det < 0.0;
            t_max = t;
        }
    });
    return found;
}

// Unit, out of the triangle's front, in the mesh's coordinates.
static glm::dvec3 triangle_normal(const triangle_t &triangle)
{
    return glm::normalize(glm::cross(triangle.e1, triangle.e2));
}

// The instance's nearest hit within [t_min, t_max], with [ray] in the
// instance BVH's coordinates. t is the same in both: the transform is
// applied to the ray's direction as it is, not renormalized.
static bool cast_instance(const instance_t &instance, const ray_t &ray, double t_min, double t_max, mesh_hit_t &out_hit)
{
    if (!instance.placed)
        return cast_mesh(*instance.mesh, ray, t_min, t_max, out_hit);
    glm::dvec3 origin = instance.inverse_linear * ray.origin + instance.inverse_translation;
    glm::dvec3 direction = instance.inverse_linear * ray.direction;
    if (!is_finite(origin) || !is_finite(direction) || direction == glm::dvec3(0.0))
        return false;
    return cast_mesh(*instance.mesh, make_ray(origin, direction), t_min, t_max, out_hit);
}

// A mesh normal carried into the instance BVH's coordinates, unit: by the
// inverse's transpose, so the mesh's front stays its front.
static glm::dvec3 place_normal(const instance_t &instance, const glm::dvec3 &normal)
{
    if (!instance.placed)
        return normal;
    return glm::normalize(glm::transpose(instance.inverse_linear) * normal);
}

static void fill_instance_hit(const rbtmath_instance_bvh_t &bvh, uint32_t index, const mesh_hit_t &hit, const ray_t &ray,
                              RBTMATH_INSTANCE_HIT &out)
{
    const instance_t &instance = bvh.instances[index];
    glm::dvec3 normal = place_normal(instance, triangle_normal(instance.mesh->triangles[hit.slot]));
    out.instance = index;
    out.id = instance.id;
    out.triangle = hit.triangle;
    out.back_face = hit.back_face;
    out.t = hit.t;
    out.point = from_glm(ray.origin + ray.direction * hit.t);
    out.normal = from_glm(normal);
}

// False if [desc] is given and can't be read - the query meets nothing.
static bool read_cast_desc(const RBTMATH_CAST_DESC *desc, RBTMATH_CAST_DESC &out, bool &out_filtered)
{
    out_filtered = desc != nullptr;
    if (!desc)
        return true;
    if (rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_MATH_CAST_DESC) != RBT_RESULT_OK)
        return false;
    out = *desc;
    return true;
}

// Calls hit(index, mesh_hit) for each instance [ray] hits among those the
// desc lets through, nearest boxes first. [t_max] is read again after each;
// below [t_min], the walk stops.
template <typename Hit>
static void walk_instances(const rbtmath_instance_bvh_t &bvh, const ray_t &ray, double t_min, const double &t_max,
                           bool filtered, const RBTMATH_CAST_DESC &desc, Hit &&hit)
{
    const dyn_tree_t &tree = bvh.tree;
    const glm::dvec3 no_grow(0.0);
    walk_tree<false>(tree.nodes.data, tree.node_count, ray, no_grow, t_min, t_max, [&](uint32_t first, uint32_t count) {
        for (uint32_t i = first; i < first + count; i++)
        {
            if (t_max == -INF || t_max < t_min)
                return; // stopped: asked only whether
            uint32_t index = tree.order[i];
            const instance_t &instance = bvh.instances[index];
            if (filtered && !(instance.mask & desc.mask))
                continue;
            double enter = t_min, leave = t_max;
            if (!clip_to_box(ray, tree.boxes[index].lo, tree.boxes[index].hi, BOX_SLACK, enter, leave))
                continue;
            if (filtered && desc.filter && !desc.filter(desc.user, index, instance.id))
                continue;
            mesh_hit_t mesh_hit;
            if (cast_instance(instance, ray, t_min, t_max, mesh_hit))
                hit(index, mesh_hit);
        }
    });
}

// ---- Shapes -------------------------------------------------------------------

namespace
{
// A shape as the queries read it: its core (a point, a segment or a box)
// about its center, and its radius.
struct core_t
{
    RBTMATH_SHAPE_KIND kind{RBTMATH_SHAPE_KIND_NONE};
    glm::dvec3 center{0.0};
    glm::dvec3 axes[3]; // the shape's own x, y and z, turned
    glm::dvec3 half{0.0};
    double half_height{0.0};
    double radius{0.0};
    // Half the size of the box around the whole shape, about its center.
    glm::dvec3 reach{0.0};
    // Its largest extent, for GJK's tolerance.
    double size{0.0};
};

// A sweep's or an overlap's touch, before it's placed.
struct touch_t
{
    double t{0.0};
    glm::dvec3 point{0.0};          // on what was touched, relative to the shape's center
    glm::dvec3 contact_normal{0.0}; // unit
    bool started{false};
};

// The support point of a triangle: its corner furthest along a direction,
// the earliest of equals.
struct triangle_support_t
{
    glm::dvec3 p[3];
    glm::dvec3 operator()(const glm::dvec3 &d) const
    {
        double a = glm::dot(p[0], d), b = glm::dot(p[1], d), c = glm::dot(p[2], d);
        if (a >= b && a >= c)
            return p[0];
        return b >= c ? p[1] : p[2];
    }
};

// And of a solid box.
struct box_support_t
{
    glm::dvec3 lo, hi;
    glm::dvec3 operator()(const glm::dvec3 &d) const
    {
        return glm::dvec3(d.x >= 0.0 ? hi.x : lo.x, d.y >= 0.0 ? hi.y : lo.y, d.z >= 0.0 ? hi.z : lo.z);
    }
};

// GJK's simplex: points of the Minkowski difference (what the shape's core
// is tested against, less the core), and the points of the other set they
// came from.
struct simplex_t
{
    glm::dvec3 p[4];
    glm::dvec3 b[4];
    uint32_t count{0};
};
} // namespace

// The shape as the queries use it; false if RBTMATH_SHAPE refuses it.
static bool read_shape(const RBTMATH_SHAPE *shape, core_t &out)
{
    if (!shape)
        return false;
    if (shape->kind != RBTMATH_SHAPE_KIND_SPHERE && shape->kind != RBTMATH_SHAPE_KIND_CAPSULE &&
        shape->kind != RBTMATH_SHAPE_KIND_BOX)
        return false;
    glm::dvec3 center = to_glm(shape->center);
    if (!is_finite(center))
        return false;
    if (!std::isfinite(shape->radius))
        return false;
    if (shape->radius < 0.0)
        return false;
    out = core_t{};
    out.kind = shape->kind;
    out.center = center;
    out.radius = shape->radius;
    out.axes[0] = glm::dvec3(1.0, 0.0, 0.0);
    out.axes[1] = glm::dvec3(0.0, 1.0, 0.0);
    out.axes[2] = glm::dvec3(0.0, 0.0, 1.0);
    if (shape->kind != RBTMATH_SHAPE_KIND_SPHERE)
    {
        double x = shape->rotation.x, y = shape->rotation.y, z = shape->rotation.z, w = shape->rotation.w;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w))
            return false;
        double length = std::sqrt(x * x + y * y + z * z + w * w);
        if (!(std::abs(length - 1.0) <= ROTATION_TOLERANCE))
            return false;
        x /= length;
        y /= length;
        z /= length;
        w /= length;
        out.axes[0] = glm::dvec3(1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y + w * z), 2.0 * (x * z - w * y));
        out.axes[1] = glm::dvec3(2.0 * (x * y - w * z), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z + w * x));
        out.axes[2] = glm::dvec3(2.0 * (x * z + w * y), 2.0 * (y * z - w * x), 1.0 - 2.0 * (x * x + y * y));
    }
    if (shape->kind == RBTMATH_SHAPE_KIND_CAPSULE)
    {
        if (!std::isfinite(shape->half_height))
            return false;
        if (shape->half_height < 0.0)
            return false;
        out.half_height = shape->half_height;
        out.reach = glm::abs(out.axes[1]) * out.half_height + glm::dvec3(out.radius);
    }
    else if (shape->kind == RBTMATH_SHAPE_KIND_BOX)
    {
        glm::dvec3 half = to_glm(shape->half_extents);
        if (!is_finite(half))
            return false;
        if (half.x < 0.0 || half.y < 0.0 || half.z < 0.0)
            return false;
        if (shape->radius != 0.0)
            return false;
        out.half = half;
        out.reach = glm::abs(out.axes[0]) * half.x + glm::abs(out.axes[1]) * half.y + glm::abs(out.axes[2]) * half.z;
    }
    else
        out.reach = glm::dvec3(out.radius);
    out.size = 2.0 * std::max(out.reach.x, std::max(out.reach.y, out.reach.z));
    return is_finite(out.reach);
}

// The core's point furthest along [d], about its center.
static glm::dvec3 core_support(const core_t &core, const glm::dvec3 &d)
{
    switch (core.kind)
    {
    case RBTMATH_SHAPE_KIND_CAPSULE: {
        glm::dvec3 a = core.axes[1] * core.half_height;
        return glm::dot(d, a) >= 0.0 ? a : -a;
    }
    case RBTMATH_SHAPE_KIND_BOX: {
        glm::dvec3 p(0.0);
        for (int i = 0; i < 3; i++)
            p += core.axes[i] * (glm::dot(d, core.axes[i]) >= 0.0 ? core.half[i] : -core.half[i]);
        return p;
    }
    default:
        return glm::dvec3(0.0);
    }
}

// The point of segment ab closest to the origin, with its weights.
static glm::dvec3 closest_on_segment(const glm::dvec3 &a, const glm::dvec3 &b, double w[2])
{
    glm::dvec3 ab = b - a;
    double length = glm::dot(ab, ab);
    double t = length > 0.0 ? -glm::dot(a, ab) / length : 0.0;
    if (!(t > 0.0))
    {
        w[0] = 1.0;
        w[1] = 0.0;
        return a;
    }
    if (t >= 1.0)
    {
        w[0] = 0.0;
        w[1] = 1.0;
        return b;
    }
    w[0] = 1.0 - t;
    w[1] = t;
    return a + ab * t;
}

// The point of triangle abc closest to the origin, with its weights
// (Ericson, "Real-Time Collision Detection", 5.1.5, with p the origin).
static glm::dvec3 closest_on_triangle(const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c, double w[3])
{
    glm::dvec3 ab = b - a, ac = c - a;
    double d1 = -glm::dot(ab, a), d2 = -glm::dot(ac, a);
    w[0] = w[1] = w[2] = 0.0;
    if (d1 <= 0.0 && d2 <= 0.0)
    {
        w[0] = 1.0;
        return a;
    }
    double d3 = -glm::dot(ab, b), d4 = -glm::dot(ac, b);
    if (d3 >= 0.0 && d4 <= d3)
    {
        w[1] = 1.0;
        return b;
    }
    double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        double v = d1 / (d1 - d3);
        w[0] = 1.0 - v;
        w[1] = v;
        return a + ab * v;
    }
    double d5 = -glm::dot(ab, c), d6 = -glm::dot(ac, c);
    if (d6 >= 0.0 && d5 <= d6)
    {
        w[2] = 1.0;
        return c;
    }
    double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        double v = d2 / (d2 - d6);
        w[0] = 1.0 - v;
        w[2] = v;
        return a + ac * v;
    }
    double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && d4 - d3 >= 0.0 && d5 - d6 >= 0.0)
    {
        double v = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        w[1] = 1.0 - v;
        w[2] = v;
        return b + (c - b) * v;
    }
    double sum = va + vb + vc;
    if (!(sum > 0.0) || !std::isfinite(sum))
    {
        // No area: the nearest of its edges.
        double e[3][2];
        glm::dvec3 p[3] = {closest_on_segment(a, b, e[0]), closest_on_segment(b, c, e[1]),
                           closest_on_segment(c, a, e[2])};
        int best = 0;
        for (int i = 1; i < 3; i++)
            if (glm::dot(p[i], p[i]) < glm::dot(p[best], p[best]))
                best = i;
        w[best] = e[best][0];
        w[(best + 1) % 3] = e[best][1];
        return p[best];
    }
    double v = vb / sum, u = vc / sum;
    w[0] = 1.0 - v - u;
    w[1] = v;
    w[2] = u;
    return a + ab * v + ac * u;
}

// The point of the simplex's hull, its points carried to x - p, closest to
// the origin: GJK's v. Keeps only the points it needs, and their weights.
static glm::dvec3 closest_on_simplex(simplex_t &s, const glm::dvec3 &x, double out_w[4])
{
    glm::dvec3 y[4];
    for (uint32_t i = 0; i < s.count; i++)
        y[i] = x - s.p[i];
    double w[4] = {0.0, 0.0, 0.0, 0.0};
    glm::dvec3 v;
    switch (s.count)
    {
    case 1:
        w[0] = 1.0;
        v = y[0];
        break;
    case 2:
        v = closest_on_segment(y[0], y[1], w);
        break;
    case 3:
        v = closest_on_triangle(y[0], y[1], y[2], w);
        break;
    default: {
        // A tetrahedron: the origin inside it, or the nearest of the faces it
        // lies outside of (and every face of one with no volume).
        static const int faces[4][4] = {{0, 1, 2, 3}, {0, 2, 3, 1}, {0, 3, 1, 2}, {1, 3, 2, 0}};
        bool outside_any = false;
        double best = INF;
        for (const int *f : faces)
        {
            glm::dvec3 n = glm::cross(y[f[1]] - y[f[0]], y[f[2]] - y[f[0]]);
            double origin_side = -glm::dot(n, y[f[0]]), other_side = glm::dot(n, y[f[3]] - y[f[0]]);
            if (other_side != 0.0 && origin_side * other_side >= 0.0)
                continue; // the origin is on the tetrahedron's side of it
            outside_any = true;
            double fw[3];
            glm::dvec3 p = closest_on_triangle(y[f[0]], y[f[1]], y[f[2]], fw);
            double d = glm::dot(p, p);
            if (d < best)
            {
                best = d;
                v = p;
                w[0] = w[1] = w[2] = w[3] = 0.0;
                w[f[0]] = fw[0];
                w[f[1]] = fw[1];
                w[f[2]] = fw[2];
            }
        }
        if (!outside_any)
        {
            // Inside: the weights are the volumes opposite each corner.
            double volume = glm::dot(y[1] - y[0], glm::cross(y[2] - y[0], y[3] - y[0]));
            double sub[4] = {glm::dot(y[1], glm::cross(y[2], y[3])), -glm::dot(y[0], glm::cross(y[2], y[3])),
                             glm::dot(y[0], glm::cross(y[1], y[3])), -glm::dot(y[0], glm::cross(y[1], y[2]))};
            for (int i = 0; i < 4; i++)
                out_w[i] = volume != 0.0 ? sub[i] / volume : 0.25;
            return glm::dvec3(0.0);
        }
        break;
    }
    }
    // Keep the points with weight.
    uint32_t kept = 0;
    for (uint32_t i = 0; i < s.count; i++)
    {
        if (w[i] == 0.0)
            continue;
        s.p[kept] = s.p[i];
        s.b[kept] = s.b[i];
        out_w[kept] = w[i];
        kept++;
    }
    if (!kept)
    {
        // Every weight rounded to 0 (a point): keep the first.
        kept = 1;
        out_w[0] = 1.0;
    }
    s.count = kept;
    return v;
}

// GJK's ray cast (van den Bergen 2004) of a shape's core grown by its
// radius, moved by t * [direction] for t from 0, against a convex set B
// given by its support function, all about the shape's center: the first t
// at which the core is within the radius (plus the tolerance) of B, if it is
// at most [t_max]. Each step moves t only as far as a plane B lies wholly
// behind proves clear, so t never passes the true first touch. [b_size] is
// B's size, for the tolerance; [fallback] the contact normal where the core
// already crosses B and GJK has no direction to give.
template <typename Support>
static bool gjk_cast(const core_t &core, const Support &other, double b_size, const glm::dvec3 &direction,
                     double t_max, const glm::dvec3 &fallback, touch_t &out)
{
    const double radius = core.radius;
    const double tolerance = GJK_TOLERANCE * std::max(core.size, b_size);
    const double reach = radius + tolerance;
    simplex_t s;
    double w[4] = {1.0, 0.0, 0.0, 0.0};
    glm::dvec3 start = direction == glm::dvec3(0.0) ? glm::dvec3(1.0, 0.0, 0.0) : -direction;
    s.b[0] = other(start);
    s.p[0] = s.b[0] - core_support(core, -start);
    s.count = 1;
    double lambda = 0.0;
    glm::dvec3 x(0.0), last_plane(0.0);
    bool advanced = false;
    glm::dvec3 v = x - s.p[0];
    double vv = glm::dot(v, v);
    for (uint32_t iteration = 0; iteration < GJK_MAX_ITERATIONS; iteration++)
    {
        if (vv <= reach * reach)
            break; // touching
        glm::dvec3 b = other(v);
        glm::dvec3 p = b - core_support(core, -v);
        double length = std::sqrt(vv);
        double vw = glm::dot(v, x - p);
        bool moved = false;
        if (vw > radius * length)
        {
            // B lies wholly behind a plane more than the radius from the
            // core: move to that plane, or miss.
            double vr = glm::dot(v, direction);
            if (!(vr < 0.0))
                return false;
            lambda -= (vw - radius * length) / vr;
            if (!(lambda <= t_max))
                return false;
            x = direction * lambda;
            advanced = true;
            moved = true;
            last_plane = v;
        }
        bool duplicate = false;
        for (uint32_t i = 0; i < s.count; i++)
            duplicate |= s.p[i] == p;
        if (duplicate && !moved)
            break; // no point closer: within the radius, to rounding
        if (!duplicate)
        {
            s.p[s.count] = p;
            s.b[s.count] = b;
            s.count++;
        }
        double before = vv;
        v = closest_on_simplex(s, x, w);
        vv = glm::dot(v, v);
        if (!moved && !(vv < before))
            break; // no progress: as close as it gets
    }
    // Touching, or GJK's count reached: touching at the t it has reached.
    out.t = lambda;
    out.started = !advanced;
    out.point = glm::dvec3(0.0);
    for (uint32_t i = 0; i < s.count; i++)
        out.point += s.b[i] * w[i];
    double length = std::sqrt(vv);
    if (length > 1e3 * tolerance)
        out.contact_normal = v / length;
    else if (advanced)
        out.contact_normal = glm::normalize(last_plane);
    else
        out.contact_normal = fallback;
    return true;
}

// The placement a mesh is queried through: none, an instance's, or a
// transform a mesh call was given.
namespace
{
struct placement_t
{
    bool placed{false};
    glm::dmat3 linear{1.0};
    glm::dvec3 translation{0.0};
    glm::dmat3 inverse_linear{1.0};
    glm::dvec3 inverse_translation{0.0};
};

// A touch inside one mesh.
struct shape_hit_t
{
    uint32_t slot{0};
    uint32_t triangle{0};
    touch_t touch;
};
} // namespace

static placement_t instance_placement(const instance_t &instance)
{
    placement_t p;
    p.placed = instance.placed;
    p.linear = instance.linear;
    p.translation = instance.translation;
    p.inverse_linear = instance.inverse_linear;
    p.inverse_translation = instance.inverse_translation;
    return p;
}

// A triangle of [mesh], placed, about the shape's [center].
static triangle_support_t place_triangle(const triangle_t &triangle, const placement_t &place, const glm::dvec3 &center)
{
    triangle_support_t t;
    glm::dvec3 a = triangle.v0, b = triangle.v0 + triangle.e1, c = triangle.v0 + triangle.e2;
    if (place.placed)
    {
        a = place.linear * a + place.translation;
        b = place.linear * b + place.translation;
        c = place.linear * c + place.translation;
    }
    t.p[0] = a - center;
    t.p[1] = b - center;
    t.p[2] = c - center;
    return t;
}

static double triangle_size(const triangle_support_t &t)
{
    box_t box;
    for (const glm::dvec3 &p : t.p)
        box.add(p);
    return box.max_extent();
}

// A shape's reach and center carried into a mesh's coordinates: the box
// around the inverse of the shape's box, padded for rounding.
static void reach_in_mesh(const core_t &core, const placement_t &place, glm::dvec3 &out_center, glm::dvec3 &out_reach)
{
    if (!place.placed)
    {
        out_center = core.center;
        out_reach = core.reach;
    }
    else
    {
        out_center = place.inverse_linear * core.center + place.inverse_translation;
        const glm::dmat3 &m = place.inverse_linear;
        out_reach = glm::dvec3(std::abs(m[0][0]) * core.reach.x + std::abs(m[1][0]) * core.reach.y +
                                   std::abs(m[2][0]) * core.reach.z,
                               std::abs(m[0][1]) * core.reach.x + std::abs(m[1][1]) * core.reach.y +
                                   std::abs(m[2][1]) * core.reach.z,
                               std::abs(m[0][2]) * core.reach.x + std::abs(m[1][2]) * core.reach.y +
                                   std::abs(m[2][2]) * core.reach.z);
    }
    glm::dvec3 scale = glm::max(glm::abs(out_center), out_reach);
    out_reach += glm::dvec3(1e-9) * (out_reach + glm::dvec3(std::max(scale.x, std::max(scale.y, scale.z))));
}

// The contact normal for a core that crosses a triangle: its face turned
// against the motion.
static glm::dvec3 crossing_normal(const triangle_support_t &t, const glm::dvec3 &direction)
{
    glm::dvec3 n = glm::cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
    double length = glm::length(n);
    if (!(length > 0.0))
        return direction == glm::dvec3(0.0) ? glm::dvec3(0.0, 1.0, 0.0) : -glm::normalize(direction);
    n /= length;
    return glm::dot(n, direction) > 0.0 ? -n : n;
}

// The first touch of a shape moved along [direction] (zero: an overlap)
// against a placed mesh, within [t_max]; of touches at the same t, the
// earliest triangle. With [stop_at_first], any touch.
static bool sweep_mesh(const rbtmath_mesh_bvh_t &mesh, const placement_t &place, const core_t &core,
                       const glm::dvec3 &direction, double t_max, bool stop_at_first, shape_hit_t &out)
{
    glm::dvec3 origin, grow;
    reach_in_mesh(core, place, origin, grow);
    glm::dvec3 mesh_direction = place.placed ? place.inverse_linear * direction : direction;
    if (!is_finite(origin) || !is_finite(mesh_direction) || !is_finite(grow))
        return false;
    ray_t ray = make_ray(origin, mesh_direction);
    bool found = false;
    double best_t = t_max;
    walk_tree<true>(mesh.nodes.data, mesh.node_count, ray, grow, 0.0, best_t, [&](uint32_t first, uint32_t count) {
        for (uint32_t slot = first; slot < first + count; slot++)
        {
            if (best_t < 0.0)
                return; // stopped at the first
            triangle_support_t t = place_triangle(mesh.triangles[slot], place, core.center);
            touch_t touch;
            if (!gjk_cast(core, t, triangle_size(t), direction, best_t, crossing_normal(t, direction), touch))
                continue;
            uint32_t index = mesh.triangle_indices[slot];
            if (found && !is_better(touch.t, index, out.touch.t, out.triangle))
                continue;
            found = true;
            out.slot = slot;
            out.triangle = index;
            out.touch = touch;
            best_t = stop_at_first ? -INF : touch.t;
        }
    });
    return found;
}

// Whether a shape at rest touches a placed mesh: the nodes its box meets,
// then GJK per triangle, stopping at the first.
static bool overlap_mesh(const rbtmath_mesh_bvh_t &mesh, const placement_t &place, const core_t &core)
{
    glm::dvec3 origin, grow;
    reach_in_mesh(core, place, origin, grow);
    if (!is_finite(origin) || !is_finite(grow))
        return false;
    glm::dvec3 lo = origin - grow, hi = origin + grow, zero(0.0);
    bool found = false;
    walk_nodes(mesh.nodes.data, mesh.node_count, found, [&](uint32_t index) {
        const bvh_node_t &node = mesh.nodes[index];
        if (!boxes_meet(node.lo, node.hi, lo, hi))
            return false;
        if (!node.count)
            return true;
        for (uint32_t slot = node.first; slot < node.first + node.count && !found; slot++)
        {
            triangle_support_t t = place_triangle(mesh.triangles[slot], place, core.center);
            touch_t touch;
            found = gjk_cast(core, t, triangle_size(t), zero, 0.0, zero, touch);
        }
        return false;
    });
    return found;
}

// A sweep as the queries use it; false if it meets nothing.
static bool read_sweep(const RBTMATH_SWEEP *sweep, core_t &out_core, glm::dvec3 &out_direction)
{
    if (!sweep)
        return false;
    if (!read_shape(&sweep->shape, out_core))
        return false;
    out_direction = to_glm(sweep->direction);
    if (!is_finite(out_direction))
        return false;
    if (!(sweep->t_max >= 0.0)) // also NaN
        return false;
    return true;
}

static void fill_sweep_hit(uint32_t instance, uint64_t id, const shape_hit_t &hit, const core_t &core,
                           const glm::dvec3 &face_normal, RBTMATH_SWEEP_HIT &out)
{
    out.instance = instance;
    out.id = id;
    out.triangle = hit.triangle;
    out.started_overlapping = hit.touch.started;
    out.t = hit.touch.t;
    out.point = from_glm(hit.touch.point + core.center);
    out.contact_normal = from_glm(hit.touch.contact_normal);
    out.face_normal = from_glm(face_normal);
}

// As an instance's transform is checked (rbtmath_instance_bvh_create()):
// finite, its last row 0, 0, 0, 1, and invertible. NULL places nothing.
enum class transform_check_t
{
    ok,
    not_finite,
    last_row,
    not_invertible,
};

static transform_check_t read_transform(const RbtDMat4 *transform, placement_t &out)
{
    out = placement_t{};
    if (!transform)
        return transform_check_t::ok;
    const double *m = transform->m; // column-major
    for (int i = 0; i < 16; i++)
        if (!std::isfinite(m[i]))
            return transform_check_t::not_finite;
    // The last row 0, 0, 0, 1.
    for (int i : {3, 7, 11})
        if (m[i] != 0.0)
            return transform_check_t::last_row;
    if (m[15] != 1.0)
        return transform_check_t::last_row;
    glm::dmat3 linear(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
    double det = glm::determinant(linear);
    if (!(det != 0.0))
        return transform_check_t::not_invertible;
    if (!std::isfinite(det))
        return transform_check_t::not_invertible;
    glm::dmat3 inverse = glm::inverse(linear);
    for (int column = 0; column < 3; column++)
        if (!is_finite(inverse[column]))
            return transform_check_t::not_invertible;
    out.placed = true;
    out.linear = linear;
    out.translation = glm::dvec3(m[12], m[13], m[14]);
    out.inverse_linear = inverse;
    out.inverse_translation = -(inverse * out.translation);
    return transform_check_t::ok;
}

// ---- Volumes ------------------------------------------------------------------

// The most planes a volume test takes, its volume's and clip planes together
// (rbtmathbvh.h, RBTMATH_VOLUME): they're carried into a mesh's coordinates in
// an array on the stack, and a triangle clipped by all of them has at most
// three points more than there are planes.
static const uint32_t VOLUME_MAX_PLANES = 32;
static const uint32_t CLIPPED_MAX_POINTS = 3 + VOLUME_MAX_PLANES;

namespace
{
// Inside where dot(normal, p) + d >= 0.
struct plane_t
{
    glm::dvec3 normal;
    double d;
};

// A volume as the tests read it: the volume's planes, then its clip planes.
struct volume_t
{
    plane_t planes[VOLUME_MAX_PLANES];
    uint32_t volume_count{0};
    uint32_t count{0}; // volume_count and the clip planes after them
};
} // namespace

static bool read_planes(const RBTMATH_PLANE *planes, uint32_t count, volume_t &out)
{
    if (count && !planes)
        return false;
    for (uint32_t i = 0; i < count; i++)
    {
        glm::dvec3 normal = to_glm(planes[i].normal);
        if (!is_finite(normal))
            return false;
        if (normal == glm::dvec3(0.0))
            return false;
        if (!std::isfinite(planes[i].d))
            return false;
        out.planes[out.count++] = plane_t{normal, planes[i].d};
    }
    return true;
}

// False if [volume] can't be read: a test then answers as for no mesh.
static bool read_volume(const RBTMATH_VOLUME *volume, volume_t &out)
{
    if (!volume)
        return false;
    if (volume->plane_count > VOLUME_MAX_PLANES)
        return false;
    if (volume->clip_plane_count > VOLUME_MAX_PLANES - volume->plane_count)
        return false;
    out.count = 0;
    if (!read_planes(volume->planes, volume->plane_count, out))
        return false;
    out.volume_count = out.count;
    return read_planes(volume->clip_planes, volume->clip_plane_count, out);
}

// As an instance's transform is checked: false for any refusal.
static bool read_placement(const RbtDMat4 *transform, placement_t &out)
{
    return read_transform(transform, out) == transform_check_t::ok;
}

static double plane_at(const plane_t &plane, const glm::dvec3 &point)
{
    return glm::dot(plane.normal, point) + plane.d;
}

// [in]'s planes as seen from the coordinates [placement] places: a point p
// there is at linear * p + translation in the volume's, so
// dot(n, linear * p + translation) + d = dot(transpose(linear) * n, p) +
// (dot(n, translation) + d). A linear part that can be inverted never makes
// a normal zero.
static void place_planes(const volume_t &in, const placement_t &placement, volume_t &out)
{
    out.volume_count = in.volume_count;
    out.count = in.count;
    glm::dmat3 transposed = glm::transpose(placement.linear);
    for (uint32_t i = 0; i < in.count; i++)
        out.planes[i] = plane_t{transposed * in.planes[i].normal,
                                glm::dot(in.planes[i].normal, placement.translation) + in.planes[i].d};
}

// Where a box lies against one plane.
enum class side_t
{
    outside,   // every point of it outside
    straddles, // some in, some out
    inside,    // every point of it inside (or on the plane)
};

// From the corners nearest and furthest along the normal, which are the
// box's least and greatest values of the plane's function.
static side_t box_side(const plane_t &plane, const glm::dvec3 &lo, const glm::dvec3 &hi)
{
    glm::dvec3 least, greatest;
    for (int axis = 0; axis < 3; axis++)
    {
        bool rising = plane.normal[axis] >= 0.0;
        least[axis] = rising ? lo[axis] : hi[axis];
        greatest[axis] = rising ? hi[axis] : lo[axis];
    }
    if (plane_at(plane, greatest) < 0.0)
        return side_t::outside;
    if (plane_at(plane, least) >= 0.0)
        return side_t::inside;
    return side_t::straddles;
}

// Where a box lies against planes [first, end) together: outside if it's
// outside any, inside if it's inside all.
static side_t box_side(const volume_t &volume, uint32_t first, uint32_t end, const glm::dvec3 &lo,
                       const glm::dvec3 &hi)
{
    side_t side = side_t::inside;
    for (uint32_t i = first; i < end; i++)
    {
        side_t one = box_side(volume.planes[i], lo, hi);
        if (one == side_t::outside)
            return side_t::outside;
        if (one == side_t::straddles)
            side = side_t::straddles;
    }
    return side;
}

// A convex polygon, as a triangle clipped by planes leaves it.
struct polygon_t
{
    glm::dvec3 points[CLIPPED_MAX_POINTS];
    uint32_t count{0};
};

// [polygon] cut down to its part inside [plane] (Sutherland-Hodgman); points
// on the plane are kept, so a polygon touching it keeps that point or edge.
// A convex polygon gains at most one point.
static void clip_polygon(polygon_t &polygon, const plane_t &plane)
{
    polygon_t out;
    for (uint32_t i = 0; i < polygon.count; i++)
    {
        const glm::dvec3 &a = polygon.points[i];
        const glm::dvec3 &b = polygon.points[(i + 1) % polygon.count];
        double da = plane_at(plane, a), db = plane_at(plane, b);
        // A convex polygon gains at most a point a plane, but rounding can leave one a hair
        // short of convex: never past the array, whatever comes in.
        if (da >= 0.0 && out.count < CLIPPED_MAX_POINTS)
            out.points[out.count++] = a;
        if ((da >= 0.0) != (db >= 0.0) && out.count < CLIPPED_MAX_POINTS)
            out.points[out.count++] = a + (b - a) * (da / (da - db));
    }
    polygon = out;
}

static void triangle_polygon(const triangle_t &triangle, polygon_t &out)
{
    out.points[0] = triangle.v0;
    out.points[1] = triangle.v0 + triangle.e1;
    out.points[2] = triangle.v0 + triangle.e2;
    out.count = 3;
}

// rbtmath_mesh_bvh_overlaps_volume(), with [volume] in the mesh's coordinates.
static bool mesh_overlaps(const rbtmath_mesh_bvh_t &mesh, const volume_t &volume)
{
    bool found = false;
    walk_nodes(mesh.nodes.data, mesh.node_count, found, [&](uint32_t index) {
        const bvh_node_t &node = mesh.nodes[index];
        side_t side = box_side(volume, 0, volume.count, node.lo, node.hi);
        if (side == side_t::outside)
            return false;
        if (side == side_t::inside)
        {
            found = true; // every node holds a triangle
            return false;
        }
        if (!node.count)
            return true;
        for (uint32_t slot = node.first; slot < node.first + node.count && !found; slot++)
        {
            polygon_t polygon;
            triangle_polygon(mesh.triangles[slot], polygon);
            for (uint32_t i = 0; i < volume.count && polygon.count; i++)
                clip_polygon(polygon, volume.planes[i]);
            found = polygon.count != 0;
        }
        return false;
    });
    return found;
}

// rbtmath_mesh_bvh_within_volume(), likewise.
static bool mesh_within(const rbtmath_mesh_bvh_t &mesh, const volume_t &volume)
{
    bool kept = false, outside = false;
    walk_nodes(mesh.nodes.data, mesh.node_count, outside, [&](uint32_t index) {
        const bvh_node_t &node = mesh.nodes[index];
        side_t clip = box_side(volume, volume.volume_count, volume.count, node.lo, node.hi);
        if (clip == side_t::outside)
            return false; // clipped away
        side_t side = box_side(volume, 0, volume.volume_count, node.lo, node.hi);
        if (clip == side_t::inside && side == side_t::inside)
        {
            kept = true;
            return false;
        }
        if (clip == side_t::inside && side == side_t::outside)
        {
            outside = true; // kept whole, and out
            return false;
        }
        if (!node.count)
            return true;
        for (uint32_t slot = node.first; slot < node.first + node.count && !outside; slot++)
        {
            polygon_t polygon;
            triangle_polygon(mesh.triangles[slot], polygon);
            for (uint32_t i = volume.volume_count; i < volume.count && polygon.count; i++)
                clip_polygon(polygon, volume.planes[i]);
            if (!polygon.count)
                continue;
            kept = true;
            for (uint32_t p = 0; p < polygon.count && !outside; p++)
                for (uint32_t i = 0; i < volume.volume_count && !outside; i++)
                    outside = plane_at(volume.planes[i], polygon.points[p]) < 0.0;
        }
        return false;
    });
    return kept && !outside;
}

// ---- Instances ----------------------------------------------------------------

// An instance as the BVH holds it, and its box unpadded and padded, from
// [in]: false (with what was wrong) if its transform is refused or places
// it out of double's range.
static transform_check_t place_instance(const RBTMATH_BVH_INSTANCE &in, instance_t &out, box_t &out_raw,
                                        box_t &out_box, bool &out_out_of_range)
{
    out_out_of_range = false;
    placement_t place;
    transform_check_t check = read_transform(in.transform, place);
    if (check != transform_check_t::ok)
        return check;
    out = instance_t{};
    out.mesh = in.mesh;
    out.id = in.id;
    out.mask = in.mask;
    box_t mesh_bounds = in.mesh ? in.mesh->bounds : box_t();
    out_raw = box_t();
    if (place.placed)
    {
        out.placed = true;
        out.inverse_linear = place.inverse_linear;
        out.inverse_translation = place.inverse_translation;
        out.linear = place.linear;
        out.translation = place.translation;
        if (!mesh_bounds.empty())
        {
            // Around the mesh's box's corners: loose for a rotated mesh, but
            // never too small.
            for (int corner = 0; corner < 8; corner++)
            {
                glm::dvec3 point(corner & 1 ? mesh_bounds.hi.x : mesh_bounds.lo.x,
                                 corner & 2 ? mesh_bounds.hi.y : mesh_bounds.lo.y,
                                 corner & 4 ? mesh_bounds.hi.z : mesh_bounds.lo.z);
                out_raw.add(place.linear * point + place.translation);
            }
        }
    }
    else
        out_raw = mesh_bounds;
    out_box = box_t();
    if (out_raw.empty())
    {
        out_raw = box_t();
        return transform_check_t::ok;
    }
    if (!is_finite(out_raw.lo) || !is_finite(out_raw.hi))
    {
        out_out_of_range = true;
        return transform_check_t::ok;
    }
    out_box = out_raw;
    // As a triangle's box, plus the transform's own rounding.
    glm::dvec3 reach = glm::max(glm::abs(out_box.lo), glm::abs(out_box.hi));
    out_box.pad(4.0 * EDGE_TOLERANCE * out_box.max_extent() +
                16.0 * DBL_EPSILON * std::max(reach.x, std::max(reach.y, reach.z)));
    return transform_check_t::ok;
}

static const char *transform_refusal(transform_check_t check)
{
    switch (check)
    {
    case transform_check_t::not_finite:
        return "isn't finite";
    case transform_check_t::last_row:
        return "has a last row other than 0, 0, 0, 1";
    case transform_check_t::not_invertible:
        return "can't be inverted";
    default:
        return "is fine";
    }
}

// A box BVH's entry from the caller's: its tree box padded for rounding.
static void place_box(const RBTMATH_BVH_BOX &in, box_entry_t &out, box_t &out_box)
{
    glm::dvec3 lo = to_glm(in.box.lo), hi = to_glm(in.box.hi);
    out.id = in.id;
    out.mask = in.mask;
    out.box = box_t();
    out_box = box_t();
    if (lo.x > hi.x || lo.y > hi.y || lo.z > hi.z)
        return; // empty
    out.box.lo = lo;
    out.box.hi = hi;
    out_box = out.box;
    glm::dvec3 reach = glm::max(glm::abs(lo), glm::abs(hi));
    out_box.pad(16.0 * DBL_EPSILON * std::max(reach.x, std::max(reach.y, reach.z)));
}

static void destroy_instance_bvh(rbtmath_instance_bvh_t *bvh)
{
    bvh->~rbtmath_instance_bvh_t();
    free(bvh);
}

static void destroy_box_bvh(rbtmath_box_bvh_t *bvh)
{
    bvh->~rbtmath_box_bvh_t();
    free(bvh);
}

// ---- Instance BVH queries -------------------------------------------------------

// The first touch of a shape moved along [direction] against one instance.
static bool sweep_instance(const instance_t &instance, const core_t &core, const glm::dvec3 &direction, double t_max,
                           bool stop_at_first, shape_hit_t &out)
{
    return sweep_mesh(*instance.mesh, instance_placement(instance), core, direction, t_max, stop_at_first, out);
}

// Calls touch(index, shape_hit) for each instance a sweep touches among
// those the desc lets through, nearest grown boxes first. [t_max] is read
// again after each; below 0, the walk stops.
template <typename Touch>
static void walk_sweep(const rbtmath_instance_bvh_t &bvh, const core_t &core, const glm::dvec3 &direction,
                       const double &t_max, bool stop_at_first, bool filtered, const RBTMATH_CAST_DESC &desc,
                       Touch &&touch)
{
    const dyn_tree_t &tree = bvh.tree;
    glm::dvec3 grow = core.reach * (1.0 + 1e-9) + glm::dvec3(1e-9 * core.size);
    ray_t ray = make_ray(core.center, direction);
    walk_tree<true>(tree.nodes.data, tree.node_count, ray, grow, 0.0, t_max, [&](uint32_t first, uint32_t count) {
        for (uint32_t i = first; i < first + count; i++)
        {
            if (t_max < 0.0)
                return;
            uint32_t index = tree.order[i];
            const instance_t &instance = bvh.instances[index];
            if (filtered && !(instance.mask & desc.mask))
                continue;
            double enter = 0.0, leave = t_max;
            if (!clip_to_box(ray, tree.boxes[index].lo - grow, tree.boxes[index].hi + grow, BOX_SLACK, enter, leave))
                continue;
            if (filtered && desc.filter && !desc.filter(desc.user, index, instance.id))
                continue;
            shape_hit_t hit;
            if (sweep_instance(instance, core, direction, t_max, stop_at_first, hit))
                touch(index, hit);
        }
    });
}

// Calls touch(index) for each instance a shape at rest touches among those
// the desc lets through; stops when [stop] is set.
template <typename Touch>
static void walk_overlap(const rbtmath_instance_bvh_t &bvh, const core_t &core, bool filtered,
                         const RBTMATH_CAST_DESC &desc, const bool &stop, Touch &&touch)
{
    const dyn_tree_t &tree = bvh.tree;
    glm::dvec3 grow = core.reach * (1.0 + 1e-9) + glm::dvec3(1e-9 * core.size);
    glm::dvec3 lo = core.center - grow, hi = core.center + grow;
    walk_nodes(tree.nodes.data, tree.node_count, stop, [&](uint32_t node_index) {
        const bvh_node_t &node = tree.nodes[node_index];
        if (!boxes_meet(node.lo, node.hi, lo, hi))
            return false;
        if (!node.count)
            return true;
        for (uint32_t i = node.first; i < node.first + node.count && !stop; i++)
        {
            uint32_t index = tree.order[i];
            const instance_t &instance = bvh.instances[index];
            if (filtered && !(instance.mask & desc.mask))
                continue;
            if (!boxes_meet(tree.boxes[index].lo, tree.boxes[index].hi, lo, hi))
                continue;
            if (filtered && desc.filter && !desc.filter(desc.user, index, instance.id))
                continue;
            if (overlap_mesh(*instance.mesh, instance_placement(instance), core))
                touch(index);
        }
        return false;
    });
}

// ---- Box BVH queries ------------------------------------------------------------

// A ray against one solid box: where it enters, or t_min if it starts
// inside; the face it enters by.
static bool cast_box(const ray_t &ray, const box_t &box, double t_min, double t_max, double &out_t, int &out_axis,
                     bool &out_inside)
{
    double enter = -INF, leave = INF;
    int axis_in = -1;
    for (int axis = 0; axis < 3; axis++)
    {
        if (box.lo[axis] > box.hi[axis])
            return false;
        if (ray.direction[axis] == 0.0)
        {
            if (ray.origin[axis] < box.lo[axis] || ray.origin[axis] > box.hi[axis])
                return false;
            continue;
        }
        double t1 = (box.lo[axis] - ray.origin[axis]) * ray.inverse[axis];
        double t2 = (box.hi[axis] - ray.origin[axis]) * ray.inverse[axis];
        if (t1 > t2)
            std::swap(t1, t2);
        if (t1 > enter)
        {
            enter = t1;
            axis_in = axis;
        }
        if (t2 < leave)
            leave = t2;
    }
    if (!(enter <= leave))
        return false;
    if (enter < t_min)
    {
        if (!(leave >= t_min))
            return false;
        out_t = t_min;
        out_inside = true;
        out_axis = axis_in;
        return true;
    }
    if (!(enter <= t_max))
        return false;
    out_t = enter;
    out_inside = false;
    out_axis = axis_in;
    return true;
}

static void fill_box_ray_hit(const rbtmath_box_bvh_t &bvh, uint32_t index, const ray_t &ray, double t, int axis,
                             bool inside, RBTMATH_BOX_HIT &out)
{
    out.box = index;
    out.id = bvh.entries[index].id;
    out.started_inside = inside;
    out.t = t;
    out.point = from_glm(ray.origin + ray.direction * t);
    glm::dvec3 normal = -glm::normalize(ray.direction);
    if (!inside && axis >= 0)
    {
        normal = glm::dvec3(0.0);
        normal[axis] = ray.direction[axis] > 0.0 ? -1.0 : 1.0;
    }
    out.face_normal = from_glm(normal);
    out.contact_normal = out.face_normal;
}

// Out of the face of [box] nearest [point]: of equals, the lowest axis, its
// low face first.
static glm::dvec3 nearest_face(const box_t &box, const glm::dvec3 &point)
{
    double best = INF;
    glm::dvec3 normal(0.0);
    for (int axis = 0; axis < 3; axis++)
    {
        double low = std::abs(point[axis] - box.lo[axis]), high = std::abs(box.hi[axis] - point[axis]);
        if (low < best)
        {
            best = low;
            normal = glm::dvec3(0.0);
            normal[axis] = -1.0;
        }
        if (high < best)
        {
            best = high;
            normal = glm::dvec3(0.0);
            normal[axis] = 1.0;
        }
    }
    return normal;
}

// A shape moved along [direction] against one solid box.
static bool sweep_box(const box_t &box, const core_t &core, const glm::dvec3 &direction, double t_max, touch_t &out)
{
    box_support_t support{box.lo - core.center, box.hi - core.center};
    glm::dvec3 against = direction == glm::dvec3(0.0) ? glm::dvec3(0.0, 1.0, 0.0) : -glm::normalize(direction);
    return gjk_cast(core, support, box.max_extent(), direction, t_max, against, out);
}

static void fill_box_sweep_hit(const rbtmath_box_bvh_t &bvh, uint32_t index, const touch_t &touch, const core_t &core,
                               const glm::dvec3 &direction, RBTMATH_BOX_HIT &out)
{
    const box_t &box = bvh.entries[index].box;
    glm::dvec3 point = touch.point + core.center;
    out.box = index;
    out.id = bvh.entries[index].id;
    out.started_inside = touch.started;
    out.t = touch.t;
    out.point = from_glm(point);
    if (touch.started && direction != glm::dvec3(0.0))
    {
        out.contact_normal = from_glm(-glm::normalize(direction));
        out.face_normal = out.contact_normal;
        return;
    }
    glm::dvec3 face = nearest_face(box, point);
    out.face_normal = from_glm(face);
    out.contact_normal = touch.started ? out.face_normal : from_glm(touch.contact_normal);
}

// Calls entry(index) for each box in a leaf the walk reaches that the desc
// lets through; [ray] and [grow] as walk_tree's.
template <bool Grow, typename Entry>
static void walk_boxes(const rbtmath_box_bvh_t &bvh, const ray_t &ray, const glm::dvec3 &grow, double t_min,
                       const double &t_max, bool filtered, const RBTMATH_CAST_DESC &desc, Entry &&entry)
{
    const dyn_tree_t &tree = bvh.tree;
    walk_tree<Grow>(tree.nodes.data, tree.node_count, ray, grow, t_min, t_max, [&](uint32_t first, uint32_t count) {
        for (uint32_t i = first; i < first + count; i++)
        {
            if (t_max == -INF || t_max < t_min)
                return; // stopped: asked only whether
            uint32_t index = tree.order[i];
            const box_entry_t &e = bvh.entries[index];
            if (filtered && !(e.mask & desc.mask))
                continue;
            double enter = t_min, leave = t_max;
            if (!clip_node<Grow>(ray, tree.boxes[index].lo, tree.boxes[index].hi, grow, enter, leave))
                continue;
            if (filtered && desc.filter && !desc.filter(desc.user, index, e.id))
                continue;
            entry(index);
        }
    });
}

template <typename Entry>
static void walk_box_overlap(const rbtmath_box_bvh_t &bvh, const core_t &core, bool filtered,
                             const RBTMATH_CAST_DESC &desc, const bool &stop, Entry &&entry)
{
    const dyn_tree_t &tree = bvh.tree;
    glm::dvec3 grow = core.reach * (1.0 + 1e-9) + glm::dvec3(1e-9 * core.size);
    glm::dvec3 lo = core.center - grow, hi = core.center + grow;
    walk_nodes(tree.nodes.data, tree.node_count, stop, [&](uint32_t node_index) {
        const bvh_node_t &node = tree.nodes[node_index];
        if (!boxes_meet(node.lo, node.hi, lo, hi))
            return false;
        if (!node.count)
            return true;
        for (uint32_t i = node.first; i < node.first + node.count && !stop; i++)
        {
            uint32_t index = tree.order[i];
            const box_entry_t &e = bvh.entries[index];
            if (filtered && !(e.mask & desc.mask))
                continue;
            if (!boxes_meet(tree.boxes[index].lo, tree.boxes[index].hi, lo, hi))
                continue;
            if (filtered && desc.filter && !desc.filter(desc.user, index, e.id))
                continue;
            touch_t touch;
            if (sweep_box(e.box, core, glm::dvec3(0.0), 0.0, touch))
                entry(index);
        }
        return false;
    });
}

// The lowest positions found, in the caller's buffer as a max-heap.
static void keep_lowest(uint32_t index, uint32_t *out, uint32_t capacity, uint32_t &kept)
{
    if (!capacity)
        return;
    if (kept == capacity)
    {
        if (index >= out[0])
            return;
        std::pop_heap(out, out + kept);
        kept--;
    }
    out[kept++] = index;
    std::push_heap(out, out + kept);
}

extern "C"
{
    // ---- Rays and boxes ---------------------------------------------------------

    RBTMATH_OVERLAP rbtmath_aabb_test_volume(const RBTMATH_AABB *box, const RbtDMat4 *transform,
                                           const RBTMATH_VOLUME *volume)
    {
        if (!box)
            return RBTMATH_OVERLAP_NONE;
        glm::dvec3 lo = to_glm(box->lo), hi = to_glm(box->hi);
        if (!is_finite(lo) || !is_finite(hi))
            return RBTMATH_OVERLAP_NONE;
        if (!(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z))
            return RBTMATH_OVERLAP_NONE; // empty
        placement_t placement;
        if (!read_placement(transform, placement))
            return RBTMATH_OVERLAP_NONE;
        volume_t planes;
        if (!read_volume(volume, planes))
            return RBTMATH_OVERLAP_NONE;
        // A placed box is a parallelepiped, whose corners are where a
        // plane's function is least and greatest; a box's are its own.
        glm::dvec3 corners[8];
        for (int corner = 0; corner < 8; corner++)
        {
            glm::dvec3 point(corner & 1 ? hi.x : lo.x, corner & 2 ? hi.y : lo.y, corner & 4 ? hi.z : lo.z);
            corners[corner] = placement.placed ? placement.linear * point + placement.translation : point;
            if (!is_finite(corners[corner]))
                return RBTMATH_OVERLAP_NONE; // placed out of double's range
        }
        RBTMATH_OVERLAP overlap = RBTMATH_OVERLAP_WHOLE;
        for (uint32_t i = 0; i < planes.count; i++)
        {
            double least = INF, greatest = -INF;
            for (const glm::dvec3 &corner : corners)
            {
                double at = plane_at(planes.planes[i], corner);
                least = std::min(least, at);
                greatest = std::max(greatest, at);
            }
            if (greatest < 0.0)
                return RBTMATH_OVERLAP_NONE;
            if (least < 0.0)
                overlap = RBTMATH_OVERLAP_PARTIAL;
        }
        return overlap;
    }

    bool rbtmath_ray_clip_aabb(RBTMATH_RAY *ray, const RBTMATH_AABB *box)
    {
        ray_t r;
        if (!box || !read_ray(ray, r))
            return false;
        glm::dvec3 lo = to_glm(box->lo), hi = to_glm(box->hi);
        if (!(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z)) // empty, or NaN
            return false;
        double t_min = ray->t_min, t_max = ray->t_max;
        if (!clip_to_box(r, lo, hi, 0.0, t_min, t_max))
            return false;
        ray->t_min = t_min;
        ray->t_max = t_max;
        return true;
    }

    // ---- Mesh BVH -----------------------------------------------------------------

    RBT_RESULT rbtmath_mesh_bvh_create(const RBTMATH_MESH_BVH_DESC *desc, rbtmath_mesh_bvh *out_bvh)
    {
        if (!out_bvh)
            return RBT_RESULT_INVALID_ARGUMENT;
        *out_bvh = nullptr;
        if (!desc)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_MATH_MESH_BVH_DESC))
            return result;
        const RBTMATH_MESH_BVH_DESC &d = *desc;
        if (d.position_count && !d.positions)
            return RBT_RESULT_INVALID_ARGUMENT;
        uint32_t corner_count = d.indices ? d.index_count : d.position_count;
        if (corner_count % 3)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (!d.indices && d.index_count)
            return RBT_RESULT_INVALID_ARGUMENT;
        for (uint32_t i = 0; i < d.position_count; i++)
            if (!is_finite(to_glm(d.positions[i])))
                return RBT_RESULT_INVALID_ARGUMENT;
        for (uint32_t i = 0; i < d.index_count; i++)
            if (d.indices[i] >= d.position_count)
                return RBT_RESULT_INVALID_ARGUMENT;

        rbtmath_mesh_bvh_t *bvh = (rbtmath_mesh_bvh_t *)malloc(sizeof(rbtmath_mesh_bvh_t));
        if (!bvh)
            return RBT_RESULT_OUT_OF_MEMORY;
        bvh = new (bvh) rbtmath_mesh_bvh_t();
        bvh->triangle_count = corner_count / 3;
        uint32_t count = bvh->triangle_count;
        // The build's working space, freed when it's done.
        eq::array<triangle_t> triangles;
        eq::array<uint32_t> indices;
        eq::array<box_t> boxes;
        eq::array<glm::dvec3> centroids;
        eq::array<uint32_t> order;
        bool ok = triangles.resize(count) == EQ_SUCCESS && indices.resize(count) == EQ_SUCCESS &&
                  boxes.resize(count) == EQ_SUCCESS;
        uint32_t kept = 0;
        if (ok)
        {
            for (uint32_t triangle = 0; triangle < count; triangle++)
            {
                glm::dvec3 corners[3];
                for (uint32_t corner = 0; corner < 3; corner++)
                {
                    uint32_t at = triangle * 3 + corner;
                    corners[corner] = to_glm(d.positions[d.indices ? d.indices[at] : at]);
                    bvh->bounds.add(corners[corner]);
                }
                glm::dvec3 e1 = corners[1] - corners[0], e2 = corners[2] - corners[0];
                double doubled_area = glm::length(glm::cross(e1, e2));
                if (!(doubled_area > 0.0) || !std::isfinite(doubled_area))
                    continue; // no area: never hit
                box_t box;
                for (const glm::dvec3 &corner : corners)
                    box.add(corner);
                // Covers what EDGE_TOLERANCE lets a ray hit past the edges.
                box.pad(4.0 * EDGE_TOLERANCE * box.max_extent());
                triangles[kept] = triangle_t{corners[0], e1, e2};
                indices[kept] = triangle;
                boxes[kept] = box;
                kept++;
            }
            size_t nodes = kept ? (size_t)kept * 2 - 1 : 0;
            ok = centroids.resize(kept) == EQ_SUCCESS && order.resize(kept) == EQ_SUCCESS &&
                 bvh->nodes.resize(nodes) == EQ_SUCCESS && bvh->triangles.resize(kept) == EQ_SUCCESS &&
                 bvh->triangle_indices.resize(kept) == EQ_SUCCESS;
        }
        if (!ok)
        {
            bvh->~rbtmath_mesh_bvh_t();
            free(bvh);
            return RBT_RESULT_OUT_OF_MEMORY;
        }
        bvh->node_count = build_tree(boxes.data, kept, MESH_LEAF_SIZE, centroids.data, order.data, bvh->nodes.data);
        // Into leaf order, so a leaf's triangles sit together: a leaf's range
        // is in slots now, and the order has done its job.
        for (uint32_t slot = 0; slot < kept; slot++)
        {
            bvh->triangles[slot] = triangles[order[slot]];
            bvh->triangle_indices[slot] = indices[order[slot]];
        }
        *out_bvh = bvh;
        return RBT_RESULT_OK;
    }

    void rbtmath_mesh_bvh_destroy(rbtmath_mesh_bvh bvh)
    {
        if (!bvh)
            return;
        if (RBT_STALE(bvh, "rbtmath_mesh_bvh_destroy", "bvh"))
            return;
        bvh->~rbtmath_mesh_bvh_t();
        free(bvh);
    }

    uint32_t rbtmath_mesh_bvh_get_triangle_count(rbtmath_mesh_bvh bvh)
    {
        if (!bvh)
            return 0;
        if (RBT_STALE(bvh, "rbtmath_mesh_bvh_get_triangle_count", "bvh"))
            return 0;
        return bvh->triangle_count;
    }

    RBTMATH_AABB rbtmath_mesh_bvh_get_bounds(rbtmath_mesh_bvh bvh)
    {
        if (!bvh)
            return to_aabb(box_t());
        if (RBT_STALE(bvh, "rbtmath_mesh_bvh_get_bounds", "bvh"))
            return to_aabb(box_t());
        return to_aabb(bvh->bounds);
    }

    bool rbtmath_mesh_bvh_cast(rbtmath_mesh_bvh bvh, const RBTMATH_RAY *ray, RBTMATH_MESH_HIT *out_hit)
    {
        ray_t r;
        if (!bvh)
            return false;
        if (RBT_STALE(bvh, "rbtmath_mesh_bvh_cast", "bvh"))
            return false;
        if (!read_ray(ray, r))
            return false;
        mesh_hit_t hit;
        if (!cast_mesh(*bvh, r, ray->t_min, ray->t_max, hit))
            return false;
        if (out_hit)
        {
            out_hit->triangle = hit.triangle;
            out_hit->back_face = hit.back_face;
            out_hit->t = hit.t;
            out_hit->point = from_glm(r.origin + r.direction * hit.t);
            out_hit->normal = from_glm(triangle_normal(bvh->triangles[hit.slot]));
        }
        return true;
    }

    bool rbtmath_mesh_bvh_overlaps_volume(rbtmath_mesh_bvh bvh, const RbtDMat4 *transform, const RBTMATH_VOLUME *volume)
    {
        if (!bvh)
            return false; // no triangles
        if (RBT_STALE(bvh, "rbtmath_mesh_bvh_overlaps_volume", "bvh"))
            return false;
        placement_t placement;
        if (!read_placement(transform, placement))
            return false;
        volume_t planes;
        if (!read_volume(volume, planes))
            return false;
        if (!placement.placed)
            return mesh_overlaps(*bvh, planes);
        volume_t placed;
        place_planes(planes, placement, placed);
        return mesh_overlaps(*bvh, placed);
    }

    bool rbtmath_mesh_bvh_within_volume(rbtmath_mesh_bvh bvh, const RbtDMat4 *transform, const RBTMATH_VOLUME *volume)
    {
        if (!bvh)
            return false; // no triangles
        if (RBT_STALE(bvh, "rbtmath_mesh_bvh_within_volume", "bvh"))
            return false;
        placement_t placement;
        if (!read_placement(transform, placement))
            return false;
        volume_t planes;
        if (!read_volume(volume, planes))
            return false;
        if (!placement.placed)
            return mesh_within(*bvh, planes);
        volume_t placed;
        place_planes(planes, placement, placed);
        return mesh_within(*bvh, placed);
    }

    bool rbtmath_mesh_bvh_overlaps_shape(rbtmath_mesh_bvh bvh, const RbtDMat4 *transform, const RBTMATH_SHAPE *shape)
    {
        if (!bvh)
            return false; // no triangles
        if (RBT_STALE(bvh, "rbtmath_mesh_bvh_overlaps_shape", "bvh"))
            return false;
        placement_t placement;
        if (!read_placement(transform, placement))
            return false;
        core_t core;
        if (!read_shape(shape, core))
            return false;
        return overlap_mesh(*bvh, placement, core);
    }

    bool rbtmath_mesh_bvh_sweep(rbtmath_mesh_bvh bvh, const RbtDMat4 *transform, const RBTMATH_SWEEP *sweep,
                               RBTMATH_SWEEP_HIT *out_hit)
    {
        if (!bvh)
            return false; // no triangles
        if (RBT_STALE(bvh, "rbtmath_mesh_bvh_sweep", "bvh"))
            return false;
        placement_t placement;
        if (!read_placement(transform, placement))
            return false;
        core_t core;
        glm::dvec3 direction;
        if (!read_sweep(sweep, core, direction))
            return false;
        shape_hit_t hit;
        if (!sweep_mesh(*bvh, placement, core, direction, sweep->t_max, out_hit == nullptr, hit))
            return false;
        if (out_hit)
        {
            glm::dvec3 face = triangle_normal(bvh->triangles[hit.slot]);
            if (placement.placed)
                face = glm::normalize(glm::transpose(placement.inverse_linear) * face);
            fill_sweep_hit(0, 0, hit, core, face, *out_hit);
        }
        return true;
    }

    // ---- Instance BVH -------------------------------------------------------------

    RBT_RESULT rbtmath_instance_bvh_create(const RBTMATH_INSTANCE_BVH_DESC *desc, rbtmath_instance_bvh *out_bvh)
    {
        if (!out_bvh)
            return RBT_RESULT_INVALID_ARGUMENT;
        *out_bvh = nullptr;
        if (!desc)
            return RBT_RESULT_INVALID_ARGUMENT;
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_MATH_INSTANCE_BVH_DESC))
            return result;
        const RBTMATH_INSTANCE_BVH_DESC &d = *desc;
        if (d.instance_count && !d.instances)
            return RBT_RESULT_INVALID_ARGUMENT;
        // Node indices are 32-bit, and a tree has up to two nodes a box.
        if (d.instance_count > UINT32_MAX / 2)
            return RBT_RESULT_INVALID_ARGUMENT;
        for (uint32_t index = 0; index < d.instance_count; index++)
        {
            instance_t instance;
            box_t raw, box;
            bool out_of_range;
            if (place_instance(d.instances[index], instance, raw, box, out_of_range) != transform_check_t::ok)
                return RBT_RESULT_INVALID_ARGUMENT;
            if (out_of_range)
                return RBT_RESULT_INVALID_ARGUMENT; // placed out of double's range
        }

        rbtmath_instance_bvh_t *bvh = (rbtmath_instance_bvh_t *)malloc(sizeof(rbtmath_instance_bvh_t));
        if (!bvh)
            return RBT_RESULT_OUT_OF_MEMORY;
        bvh = new (bvh) rbtmath_instance_bvh_t();
        if (bvh->instances.resize(d.instance_count) != EQ_SUCCESS ||
            bvh->raw_boxes.resize(d.instance_count) != EQ_SUCCESS || !tree_allocate(bvh->tree, d.instance_count))
        {
            destroy_instance_bvh(bvh);
            return RBT_RESULT_OUT_OF_MEMORY;
        }
        for (uint32_t index = 0; index < d.instance_count; index++)
        {
            bool out_of_range;
            place_instance(d.instances[index], bvh->instances[index], bvh->raw_boxes[index], bvh->tree.boxes[index],
                           out_of_range);
        }
        tree_rebuild(bvh->tree, INSTANCE_LEAF_SIZE);
        *out_bvh = bvh;
        return RBT_RESULT_OK;
    }

    void rbtmath_instance_bvh_destroy(rbtmath_instance_bvh bvh)
    {
        if (!bvh)
            return;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_destroy", "bvh"))
            return;
        destroy_instance_bvh(bvh);
    }

    uint32_t rbtmath_instance_bvh_get_instance_count(rbtmath_instance_bvh bvh)
    {
        if (!bvh)
            return 0;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_get_instance_count", "bvh"))
            return 0;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_instance_bvh_get_instance_count",
                          "instance BVH");
        return (uint32_t)bvh->instances.size();
    }

    RBTMATH_AABB rbtmath_instance_bvh_get_bounds(rbtmath_instance_bvh bvh)
    {
        if (!bvh)
            return to_aabb(box_t());
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_get_bounds", "bvh"))
            return to_aabb(box_t());
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_instance_bvh_get_bounds", "instance BVH");
        box_t bounds;
        for (const box_t &box : bvh->raw_boxes)
            bounds.add(box);
        return to_aabb(bounds);
    }

    bool rbtmath_instance_bvh_cast(rbtmath_instance_bvh bvh, const RBTMATH_RAY *ray, const RBTMATH_CAST_DESC *desc,
                                  RBTMATH_INSTANCE_HIT *out_hit)
    {
        ray_t r;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return false;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_cast", "bvh"))
            return false;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_instance_bvh_cast", "instance BVH");
        if (!read_ray(ray, r))
            return false;
        if (!read_cast_desc(desc, d, filtered))
            return false;
        bool found = false;
        uint32_t best_index = 0;
        mesh_hit_t best;
        double t_max = ray->t_max;
        walk_instances(*bvh, r, ray->t_min, t_max, filtered, d, [&](uint32_t index, const mesh_hit_t &hit) {
            if (found && !is_better(hit.t, index, best.t, best_index))
                return;
            found = true;
            best_index = index;
            best = hit;
            // Asked only whether: the first hit answers it.
            t_max = out_hit ? hit.t : -INF;
        });
        if (found && out_hit)
            fill_instance_hit(*bvh, best_index, best, r, *out_hit);
        return found;
    }

    uint32_t rbtmath_instance_bvh_cast_all(rbtmath_instance_bvh bvh, const RBTMATH_RAY *ray, const RBTMATH_CAST_DESC *desc,
                                          RBTMATH_INSTANCE_HIT *out_hits, uint32_t hit_capacity)
    {
        ray_t r;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return 0;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_cast_all", "bvh"))
            return 0;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_instance_bvh_cast_all", "instance BVH");
        if (!read_ray(ray, r))
            return 0;
        if (!read_cast_desc(desc, d, filtered))
            return 0;
        if (!out_hits)
            hit_capacity = 0;
        // The caller's buffer is a heap of the best hits so far, the worst of
        // them on top to be pushed out - so nothing is allocated.
        auto before = [](const RBTMATH_INSTANCE_HIT &a, const RBTMATH_INSTANCE_HIT &b) {
            return is_better(a.t, a.instance, b.t, b.instance);
        };
        uint32_t total = 0, kept = 0;
        double t_max = ray->t_max;
        walk_instances(*bvh, r, ray->t_min, t_max, filtered, d, [&](uint32_t index, const mesh_hit_t &hit) {
            total++;
            if (!hit_capacity)
                return;
            if (kept == hit_capacity)
            {
                if (!is_better(hit.t, index, out_hits[0].t, out_hits[0].instance))
                    return;
                std::pop_heap(out_hits, out_hits + kept, before);
                kept--;
            }
            fill_instance_hit(*bvh, index, hit, r, out_hits[kept++]);
            std::push_heap(out_hits, out_hits + kept, before);
        });
        std::sort_heap(out_hits, out_hits + kept, before);
        return total;
    }

    bool rbtmath_instance_bvh_overlaps_shape(rbtmath_instance_bvh bvh, const RBTMATH_SHAPE *shape,
                                            const RBTMATH_CAST_DESC *desc)
    {
        core_t core;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return false;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_overlaps_shape", "bvh"))
            return false;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_instance_bvh_overlaps_shape", "instance BVH");
        if (!read_shape(shape, core))
            return false;
        if (!read_cast_desc(desc, d, filtered))
            return false;
        bool found = false;
        walk_overlap(*bvh, core, filtered, d, found, [&](uint32_t) { found = true; });
        return found;
    }

    uint32_t rbtmath_instance_bvh_overlap_all(rbtmath_instance_bvh bvh, const RBTMATH_SHAPE *shape,
                                             const RBTMATH_CAST_DESC *desc, uint32_t *out_instances,
                                             uint32_t instance_capacity)
    {
        core_t core;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return 0;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_overlap_all", "bvh"))
            return 0;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_instance_bvh_overlap_all", "instance BVH");
        if (!read_shape(shape, core))
            return 0;
        if (!read_cast_desc(desc, d, filtered))
            return 0;
        if (!out_instances)
            instance_capacity = 0;
        uint32_t total = 0, kept = 0;
        const bool never = false;
        walk_overlap(*bvh, core, filtered, d, never, [&](uint32_t index) {
            total++;
            keep_lowest(index, out_instances, instance_capacity, kept);
        });
        std::sort_heap(out_instances, out_instances + kept);
        return total;
    }

    bool rbtmath_instance_bvh_sweep(rbtmath_instance_bvh bvh, const RBTMATH_SWEEP *sweep, const RBTMATH_CAST_DESC *desc,
                                   RBTMATH_SWEEP_HIT *out_hit)
    {
        core_t core;
        glm::dvec3 direction;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return false;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_sweep", "bvh"))
            return false;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_instance_bvh_sweep", "instance BVH");
        if (!read_sweep(sweep, core, direction))
            return false;
        if (!read_cast_desc(desc, d, filtered))
            return false;
        bool found = false;
        uint32_t best_index = 0;
        shape_hit_t best;
        double t_max = sweep->t_max;
        walk_sweep(*bvh, core, direction, t_max, out_hit == nullptr, filtered, d,
                   [&](uint32_t index, const shape_hit_t &hit) {
                       if (found && !is_better(hit.touch.t, index, best.touch.t, best_index))
                           return;
                       found = true;
                       best_index = index;
                       best = hit;
                       t_max = out_hit ? hit.touch.t : -INF;
                   });
        if (found && out_hit)
        {
            const instance_t &instance = bvh->instances[best_index];
            glm::dvec3 face = place_normal(instance, triangle_normal(instance.mesh->triangles[best.slot]));
            fill_sweep_hit(best_index, instance.id, best, core, face, *out_hit);
        }
        return found;
    }

    uint32_t rbtmath_instance_bvh_sweep_all(rbtmath_instance_bvh bvh, const RBTMATH_SWEEP *sweep,
                                           const RBTMATH_CAST_DESC *desc, RBTMATH_SWEEP_HIT *out_hits,
                                           uint32_t hit_capacity)
    {
        core_t core;
        glm::dvec3 direction;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return 0;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_sweep_all", "bvh"))
            return 0;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_instance_bvh_sweep_all", "instance BVH");
        if (!read_sweep(sweep, core, direction))
            return 0;
        if (!read_cast_desc(desc, d, filtered))
            return 0;
        if (!out_hits)
            hit_capacity = 0;
        auto before = [](const RBTMATH_SWEEP_HIT &a, const RBTMATH_SWEEP_HIT &b) {
            return is_better(a.t, a.instance, b.t, b.instance);
        };
        uint32_t total = 0, kept = 0;
        const double t_max = sweep->t_max;
        walk_sweep(*bvh, core, direction, t_max, false, filtered, d, [&](uint32_t index, const shape_hit_t &hit) {
            total++;
            if (!hit_capacity)
                return;
            if (kept == hit_capacity)
            {
                if (!is_better(hit.touch.t, index, out_hits[0].t, out_hits[0].instance))
                    return;
                std::pop_heap(out_hits, out_hits + kept, before);
                kept--;
            }
            const instance_t &instance = bvh->instances[index];
            glm::dvec3 face = place_normal(instance, triangle_normal(instance.mesh->triangles[hit.slot]));
            fill_sweep_hit(index, instance.id, hit, core, face, out_hits[kept++]);
            std::push_heap(out_hits, out_hits + kept, before);
        });
        std::sort_heap(out_hits, out_hits + kept, before);
        return total;
    }

    RBT_RESULT rbtmath_instance_bvh_set_instances(rbtmath_instance_bvh bvh, const uint32_t *indices,
                                               const RBTMATH_BVH_INSTANCE *instances, uint32_t count,
                                               RBTMATH_ERROR *error)
    {
        clear(error);
        if (!bvh)
            return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_instance_bvh_set_instances: bvh is NULL");
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_set_instances", "bvh"))
            return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_instance_bvh_set_instances: bvh is destroyed");
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_SHARE, "rbtmath", "rbtmath_instance_bvh_set_instances",
                          "instance BVH");
        if (count && !indices)
            return fail(error, RBT_RESULT_INVALID_ARGUMENT,
                        "rbtmath_instance_bvh_set_instances: indices is NULL with count %u", count);
        if (count && !instances)
            return fail(error, RBT_RESULT_INVALID_ARGUMENT,
                        "rbtmath_instance_bvh_set_instances: instances is NULL with count %u", count);
        dyn_tree_t &tree = bvh->tree;
        RBT_RESULT result = RBT_RESULT_OK;
        uint32_t checked = 0;
        for (; checked < count; checked++)
        {
            uint32_t index = indices[checked];
            if (index >= tree.entry_count)
            {
                result = fail(error, RBT_RESULT_INVALID_ARGUMENT,
                              "rbtmath_instance_bvh_set_instances: indices[%u] is %u, past the instance count %u",
                              checked, index, tree.entry_count);
                break;
            }
            if (tree.checking[index])
            {
                result = fail(error, RBT_RESULT_INVALID_ARGUMENT,
                              "rbtmath_instance_bvh_set_instances: indices[%u] is %u, given twice", checked, index);
                break;
            }
            tree.checking[index] = 1;
            instance_t instance;
            box_t raw, box;
            bool out_of_range;
            transform_check_t check = place_instance(instances[checked], instance, raw, box, out_of_range);
            if (check != transform_check_t::ok)
            {
                result = fail(error, RBT_RESULT_INVALID_ARGUMENT,
                              "rbtmath_instance_bvh_set_instances: instances[%u]'s transform (for index %u) %s",
                              checked, index, transform_refusal(check));
                checked++; // its mark is set
                break;
            }
            if (out_of_range)
            {
                result = fail(error, RBT_RESULT_INVALID_ARGUMENT,
                              "rbtmath_instance_bvh_set_instances: instances[%u]'s transform (for index %u) places "
                              "it out of double's range",
                              checked, index);
                checked++;
                break;
            }
        }
        // This call's check marks, and only those, cleared: set again only
        // by an index of this call's, so a duplicate's was set by an earlier
        // one of them.
        for (uint32_t i = 0; i < checked; i++)
            tree.checking[indices[i]] = 0;
        if (result != RBT_RESULT_OK)
            return result;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t index = indices[i];
            bool out_of_range;
            place_instance(instances[i], bvh->instances[index], bvh->raw_boxes[index], tree.boxes[index], out_of_range);
            tree.moved[index] = 1;
        }
        return RBT_RESULT_OK;
    }

    void rbtmath_instance_bvh_refit(rbtmath_instance_bvh bvh)
    {
        if (!bvh)
            return;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_refit", "bvh"))
            return;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_ALONE, "rbtmath", "rbtmath_instance_bvh_refit", "instance BVH");
        tree_refit(bvh->tree);
    }

    void rbtmath_instance_bvh_rebuild(rbtmath_instance_bvh bvh)
    {
        if (!bvh)
            return;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_rebuild", "bvh"))
            return;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_ALONE, "rbtmath", "rbtmath_instance_bvh_rebuild", "instance BVH");
        tree_rebuild(bvh->tree, INSTANCE_LEAF_SIZE);
    }

    double rbtmath_instance_bvh_get_cost(rbtmath_instance_bvh bvh)
    {
        if (!bvh)
            return 0.0;
        if (RBT_STALE(bvh, "rbtmath_instance_bvh_get_cost", "bvh"))
            return 0.0;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_instance_bvh_get_cost", "instance BVH");
        return tree_cost(bvh->tree);
    }

    // ---- Box BVH ------------------------------------------------------------------

    RBT_RESULT rbtmath_box_bvh_create(const RBTMATH_BOX_BVH_DESC *desc, rbtmath_box_bvh *out_bvh, RBTMATH_ERROR *error)
    {
        clear(error);
        if (!desc)
        {
            if (out_bvh)
                *out_bvh = nullptr;
            return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_box_bvh_create: desc is NULL");
        }
        if (RBT_RESULT result = rbt_struct_check(&desc->base, RBT_STRUCT_TYPE_MATH_BOX_BVH_DESC))
        {
            if (out_bvh)
                *out_bvh = nullptr;
            return fail(error, result, "rbtmath_box_bvh_create: desc's base isn't an RBTMATH_BOX_BVH_DESC's alone");
        }
        if (!out_bvh)
            return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_box_bvh_create: out_bvh is NULL");
        *out_bvh = nullptr;
        const RBTMATH_BOX_BVH_DESC &d = *desc;
        if (d.box_count && !d.boxes)
            return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_box_bvh_create: boxes is NULL with box_count %u",
                        d.box_count);
        if (d.box_count > UINT32_MAX / 2)
            return fail(error, RBT_RESULT_INVALID_ARGUMENT,
                        "rbtmath_box_bvh_create: box_count %u is past UINT32_MAX / 2", d.box_count);
        for (uint32_t i = 0; i < d.box_count; i++)
            if (!is_finite(to_glm(d.boxes[i].box.lo)) || !is_finite(to_glm(d.boxes[i].box.hi)))
                return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_box_bvh_create: boxes[%u] isn't finite", i);

        rbtmath_box_bvh_t *bvh = (rbtmath_box_bvh_t *)malloc(sizeof(rbtmath_box_bvh_t));
        if (!bvh)
            return fail(error, RBT_RESULT_OUT_OF_MEMORY, "rbtmath_box_bvh_create: out of memory");
        bvh = new (bvh) rbtmath_box_bvh_t();
        if (bvh->entries.resize(d.box_count) != EQ_SUCCESS || !tree_allocate(bvh->tree, d.box_count))
        {
            destroy_box_bvh(bvh);
            return fail(error, RBT_RESULT_OUT_OF_MEMORY, "rbtmath_box_bvh_create: out of memory for %u boxes",
                        d.box_count);
        }
        for (uint32_t i = 0; i < d.box_count; i++)
            place_box(d.boxes[i], bvh->entries[i], bvh->tree.boxes[i]);
        tree_rebuild(bvh->tree, INSTANCE_LEAF_SIZE);
        *out_bvh = bvh;
        return RBT_RESULT_OK;
    }

    void rbtmath_box_bvh_destroy(rbtmath_box_bvh bvh)
    {
        if (!bvh)
            return;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_destroy", "bvh"))
            return;
        destroy_box_bvh(bvh);
    }

    RBT_RESULT rbtmath_box_bvh_set_boxes(rbtmath_box_bvh bvh, const uint32_t *indices, const RBTMATH_BVH_BOX *boxes,
                                      uint32_t count, RBTMATH_ERROR *error)
    {
        clear(error);
        if (!bvh)
            return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_box_bvh_set_boxes: bvh is NULL");
        if (RBT_STALE(bvh, "rbtmath_box_bvh_set_boxes", "bvh"))
            return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_box_bvh_set_boxes: bvh is destroyed");
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_SHARE, "rbtmath", "rbtmath_box_bvh_set_boxes", "box BVH");
        if (count && !indices)
            return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_box_bvh_set_boxes: indices is NULL with count %u",
                        count);
        if (count && !boxes)
            return fail(error, RBT_RESULT_INVALID_ARGUMENT, "rbtmath_box_bvh_set_boxes: boxes is NULL with count %u",
                        count);
        dyn_tree_t &tree = bvh->tree;
        RBT_RESULT result = RBT_RESULT_OK;
        uint32_t checked = 0;
        for (; checked < count; checked++)
        {
            uint32_t index = indices[checked];
            if (index >= tree.entry_count)
            {
                result = fail(error, RBT_RESULT_INVALID_ARGUMENT,
                              "rbtmath_box_bvh_set_boxes: indices[%u] is %u, past the box count %u", checked, index,
                              tree.entry_count);
                break;
            }
            if (tree.checking[index])
            {
                result = fail(error, RBT_RESULT_INVALID_ARGUMENT,
                              "rbtmath_box_bvh_set_boxes: indices[%u] is %u, given twice", checked, index);
                break;
            }
            tree.checking[index] = 1;
            if (!is_finite(to_glm(boxes[checked].box.lo)) || !is_finite(to_glm(boxes[checked].box.hi)))
            {
                result = fail(error, RBT_RESULT_INVALID_ARGUMENT,
                              "rbtmath_box_bvh_set_boxes: boxes[%u] (for index %u) isn't finite", checked, index);
                checked++;
                break;
            }
        }
        for (uint32_t i = 0; i < checked; i++)
            tree.checking[indices[i]] = 0;
        if (result != RBT_RESULT_OK)
            return result;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t index = indices[i];
            place_box(boxes[i], bvh->entries[index], tree.boxes[index]);
            tree.moved[index] = 1;
        }
        return RBT_RESULT_OK;
    }

    void rbtmath_box_bvh_refit(rbtmath_box_bvh bvh)
    {
        if (!bvh)
            return;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_refit", "bvh"))
            return;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_ALONE, "rbtmath", "rbtmath_box_bvh_refit", "box BVH");
        tree_refit(bvh->tree);
    }

    void rbtmath_box_bvh_rebuild(rbtmath_box_bvh bvh)
    {
        if (!bvh)
            return;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_rebuild", "bvh"))
            return;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_ALONE, "rbtmath", "rbtmath_box_bvh_rebuild", "box BVH");
        tree_rebuild(bvh->tree, INSTANCE_LEAF_SIZE);
    }

    double rbtmath_box_bvh_get_cost(rbtmath_box_bvh bvh)
    {
        if (!bvh)
            return 0.0;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_get_cost", "bvh"))
            return 0.0;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_box_bvh_get_cost", "box BVH");
        return tree_cost(bvh->tree);
    }

    bool rbtmath_box_bvh_cast(rbtmath_box_bvh bvh, const RBTMATH_RAY *ray, const RBTMATH_CAST_DESC *desc,
                             RBTMATH_BOX_HIT *out_hit)
    {
        ray_t r;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return false;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_cast", "bvh"))
            return false;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_box_bvh_cast", "box BVH");
        if (!read_ray(ray, r))
            return false;
        if (!read_cast_desc(desc, d, filtered))
            return false;
        bool found = false;
        uint32_t best_index = 0;
        double best_t = 0.0;
        int best_axis = -1;
        bool best_inside = false;
        double t_max = ray->t_max;
        const glm::dvec3 no_grow(0.0);
        walk_boxes<false>(*bvh, r, no_grow, ray->t_min, t_max, filtered, d, [&](uint32_t index) {
            double t;
            int axis;
            bool inside;
            if (!cast_box(r, bvh->entries[index].box, ray->t_min, t_max, t, axis, inside))
                return;
            if (found && !is_better(t, index, best_t, best_index))
                return;
            found = true;
            best_index = index;
            best_t = t;
            best_axis = axis;
            best_inside = inside;
            t_max = out_hit ? t : -INF;
        });
        if (found && out_hit)
            fill_box_ray_hit(*bvh, best_index, r, best_t, best_axis, best_inside, *out_hit);
        return found;
    }

    uint32_t rbtmath_box_bvh_cast_all(rbtmath_box_bvh bvh, const RBTMATH_RAY *ray, const RBTMATH_CAST_DESC *desc,
                                     RBTMATH_BOX_HIT *out_hits, uint32_t hit_capacity)
    {
        ray_t r;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return 0;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_cast_all", "bvh"))
            return 0;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_box_bvh_cast_all", "box BVH");
        if (!read_ray(ray, r))
            return 0;
        if (!read_cast_desc(desc, d, filtered))
            return 0;
        if (!out_hits)
            hit_capacity = 0;
        auto before = [](const RBTMATH_BOX_HIT &a, const RBTMATH_BOX_HIT &b) { return is_better(a.t, a.box, b.t, b.box); };
        uint32_t total = 0, kept = 0;
        const double t_max = ray->t_max;
        const glm::dvec3 no_grow(0.0);
        walk_boxes<false>(*bvh, r, no_grow, ray->t_min, t_max, filtered, d, [&](uint32_t index) {
            double t;
            int axis;
            bool inside;
            if (!cast_box(r, bvh->entries[index].box, ray->t_min, t_max, t, axis, inside))
                return;
            total++;
            if (!hit_capacity)
                return;
            if (kept == hit_capacity)
            {
                if (!is_better(t, index, out_hits[0].t, out_hits[0].box))
                    return;
                std::pop_heap(out_hits, out_hits + kept, before);
                kept--;
            }
            fill_box_ray_hit(*bvh, index, r, t, axis, inside, out_hits[kept++]);
            std::push_heap(out_hits, out_hits + kept, before);
        });
        std::sort_heap(out_hits, out_hits + kept, before);
        return total;
    }

    bool rbtmath_box_bvh_overlaps_shape(rbtmath_box_bvh bvh, const RBTMATH_SHAPE *shape, const RBTMATH_CAST_DESC *desc)
    {
        core_t core;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return false;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_overlaps_shape", "bvh"))
            return false;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_box_bvh_overlaps_shape", "box BVH");
        if (!read_shape(shape, core))
            return false;
        if (!read_cast_desc(desc, d, filtered))
            return false;
        bool found = false;
        walk_box_overlap(*bvh, core, filtered, d, found, [&](uint32_t) { found = true; });
        return found;
    }

    uint32_t rbtmath_box_bvh_overlap_all(rbtmath_box_bvh bvh, const RBTMATH_SHAPE *shape, const RBTMATH_CAST_DESC *desc,
                                        uint32_t *out_boxes, uint32_t box_capacity)
    {
        core_t core;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return 0;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_overlap_all", "bvh"))
            return 0;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_box_bvh_overlap_all", "box BVH");
        if (!read_shape(shape, core))
            return 0;
        if (!read_cast_desc(desc, d, filtered))
            return 0;
        if (!out_boxes)
            box_capacity = 0;
        uint32_t total = 0, kept = 0;
        const bool never = false;
        walk_box_overlap(*bvh, core, filtered, d, never, [&](uint32_t index) {
            total++;
            keep_lowest(index, out_boxes, box_capacity, kept);
        });
        std::sort_heap(out_boxes, out_boxes + kept);
        return total;
    }

    bool rbtmath_box_bvh_sweep(rbtmath_box_bvh bvh, const RBTMATH_SWEEP *sweep, const RBTMATH_CAST_DESC *desc,
                              RBTMATH_BOX_HIT *out_hit)
    {
        core_t core;
        glm::dvec3 direction;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return false;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_sweep", "bvh"))
            return false;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_box_bvh_sweep", "box BVH");
        if (!read_sweep(sweep, core, direction))
            return false;
        if (!read_cast_desc(desc, d, filtered))
            return false;
        bool found = false;
        uint32_t best_index = 0;
        touch_t best;
        double t_max = sweep->t_max;
        glm::dvec3 grow = core.reach * (1.0 + 1e-9) + glm::dvec3(1e-9 * core.size);
        ray_t r = make_ray(core.center, direction);
        walk_boxes<true>(*bvh, r, grow, 0.0, t_max, filtered, d, [&](uint32_t index) {
            touch_t touch;
            if (!sweep_box(bvh->entries[index].box, core, direction, t_max, touch))
                return;
            if (found && !is_better(touch.t, index, best.t, best_index))
                return;
            found = true;
            best_index = index;
            best = touch;
            t_max = out_hit ? touch.t : -INF;
        });
        if (found && out_hit)
            fill_box_sweep_hit(*bvh, best_index, best, core, direction, *out_hit);
        return found;
    }

    uint32_t rbtmath_box_bvh_sweep_all(rbtmath_box_bvh bvh, const RBTMATH_SWEEP *sweep, const RBTMATH_CAST_DESC *desc,
                                      RBTMATH_BOX_HIT *out_hits, uint32_t hit_capacity)
    {
        core_t core;
        glm::dvec3 direction;
        RBTMATH_CAST_DESC d;
        bool filtered;
        if (!bvh)
            return 0;
        if (RBT_STALE(bvh, "rbtmath_box_bvh_sweep_all", "bvh"))
            return 0;
        RBT_ENTER_SHARED(bvh->guard, RBT_ROLE_READ, "rbtmath", "rbtmath_box_bvh_sweep_all", "box BVH");
        if (!read_sweep(sweep, core, direction))
            return 0;
        if (!read_cast_desc(desc, d, filtered))
            return 0;
        if (!out_hits)
            hit_capacity = 0;
        auto before = [](const RBTMATH_BOX_HIT &a, const RBTMATH_BOX_HIT &b) { return is_better(a.t, a.box, b.t, b.box); };
        uint32_t total = 0, kept = 0;
        const double t_max = sweep->t_max;
        glm::dvec3 grow = core.reach * (1.0 + 1e-9) + glm::dvec3(1e-9 * core.size);
        ray_t r = make_ray(core.center, direction);
        walk_boxes<true>(*bvh, r, grow, 0.0, t_max, filtered, d, [&](uint32_t index) {
            touch_t touch;
            if (!sweep_box(bvh->entries[index].box, core, direction, t_max, touch))
                return;
            total++;
            if (!hit_capacity)
                return;
            if (kept == hit_capacity)
            {
                if (!is_better(touch.t, index, out_hits[0].t, out_hits[0].box))
                    return;
                std::pop_heap(out_hits, out_hits + kept, before);
                kept--;
            }
            fill_box_sweep_hit(*bvh, index, touch, core, direction, out_hits[kept++]);
            std::push_heap(out_hits, out_hits + kept, before);
        });
        std::sort_heap(out_hits, out_hits + kept, before);
        return total;
    }
}
