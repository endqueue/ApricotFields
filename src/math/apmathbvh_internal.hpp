#ifndef APMATHBVH_INTERNAL_HPP
#define APMATHBVH_INTERNAL_HPP

// What tests/aprioverlap_tests.cpp reaches inside ApMath's BVHs: the guard
// each instance and box BVH's calls enter in Debug (apmathbvh.h, "Threads"),
// so a test can hold it in one role and see another call reported.

#include "../aprioverlap.hpp"
#include "math/apmathbvh.h"

apri_shared_overlap &apmath_instance_bvh_guard(apmath_instance_bvh bvh);
apri_shared_overlap &apmath_box_bvh_guard(apmath_box_bvh bvh);

#endif // APMATHBVH_INTERNAL_HPP
