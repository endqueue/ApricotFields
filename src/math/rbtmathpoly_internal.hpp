#ifndef RBTMATHPOLY_INTERNAL_HPP
#define RBTMATHPOLY_INTERNAL_HPP

#include "math/rbtmathpoly.h"
#include "../rbtstale.hpp"
#include <vector>

// A boolean's result: an RBTMATH_POLY_SET's arrays, owned.
struct rbtmath_poly_t
{
    char *debug_name{nullptr}; // first, as every handle's: rbt_debug_name_set() writes it (rbtcore.h)
    std::vector<RBTMATH_POLY_POINT> points;
    std::vector<uint64_t> edge_tags;
    std::vector<uint32_t> ring_ends;
    std::vector<uint32_t> ring_outers;
    RBT_STALE_TAG
};

#endif // RBTMATHPOLY_INTERNAL_HPP
