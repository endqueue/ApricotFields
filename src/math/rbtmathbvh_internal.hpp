#ifndef RBTMATHBVH_INTERNAL_HPP
#define RBTMATHBVH_INTERNAL_HPP

// What tests/rbtoverlap_tests.cpp reaches inside Rabbittus Math's BVHs: the guard
// each instance and box BVH's calls enter in Debug (rbtmathbvh.h, "Threads"),
// so a test can hold it in one role and see another call reported.

#include "../rbtoverlap.hpp"
#include "math/rbtmathbvh.h"

rbt_shared_overlap &rbtmath_instance_bvh_guard(rbtmath_instance_bvh bvh);
rbt_shared_overlap &rbtmath_box_bvh_guard(rbtmath_box_bvh bvh);

#endif // RBTMATHBVH_INTERNAL_HPP
