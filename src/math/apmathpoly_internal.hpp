#ifndef APMATHPOLY_INTERNAL_HPP
#define APMATHPOLY_INTERNAL_HPP

#include "math/apmathpoly.h"
#include <vector>

// A boolean's result: an APMATH_POLY_SET's arrays, owned.
struct apmath_poly_t
{
    std::vector<APMATH_POLY_POINT> points;
    std::vector<uint64_t> edge_tags;
    std::vector<uint32_t> ring_ends;
    std::vector<uint32_t> ring_outers;
};

#endif // APMATHPOLY_INTERNAL_HPP
