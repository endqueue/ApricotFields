#ifndef APMATHPOLY_INTERNAL_HPP
#define APMATHPOLY_INTERNAL_HPP

#include "math/apmathpoly.h"
#include "../apristale.hpp"
#include <vector>

// A boolean's result: an APMATH_POLY_SET's arrays, owned.
struct apmath_poly_t
{
    char *debug_name{nullptr}; // first, as every handle's: apri_debug_name_set() writes it (apricore.h)
    std::vector<APMATH_POLY_POINT> points;
    std::vector<uint64_t> edge_tags;
    std::vector<uint32_t> ring_ends;
    std::vector<uint32_t> ring_outers;
    APRI_STALE_TAG
};

#endif // APMATHPOLY_INTERNAL_HPP
