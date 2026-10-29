#ifndef APTEXT_INTERNAL_HPP
#define APTEXT_INTERNAL_HPP

#include "text/aptext.h"

// ApText's objects. The contract pass (APTEXT_TODO.md) gives each only its
// debug name, the first member of every opaque handle (apricore.h); the
// implementation pass adds what each holds. Nothing creates one yet.

// ---- Objects ----------------------------------------------------------------

struct aptext_face_t
{
    const char *debug_name;
};

struct aptext_chain_t
{
    const char *debug_name;
};

struct aptext_layout_t
{
    const char *debug_name;
};

struct aptext_atlas_t
{
    const char *debug_name;
};

#endif // APTEXT_INTERNAL_HPP
