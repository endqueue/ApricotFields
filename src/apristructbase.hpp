#ifndef APRISTRUCTBASE_HPP
#define APRISTRUCTBASE_HPP

// The checks every call makes of a caller's based struct (APRI_STRUCT_BASE,
// apricore.h) before it reads any other field, written once so that every
// module returns the same result for the same struct.
//
// No struct can be chained onto any ApricotFields struct yet, so anything in
// a chain is a struct this build doesn't take there. When the first chained
// struct is added, the call that takes it walks its own chain instead.

#include "apricore.h"
#include <stddef.h>

// A struct the caller filled in: [type] is the type the call takes there.
static inline APRESULT apri_struct_check(const APRI_STRUCT_BASE *base, APRI_STRUCT_TYPE type)
{
    if (base->type != type)
        return APRESULT_STRUCT_WRONG_TYPE;
    if (base->next)
        return APRESULT_STRUCT_UNKNOWN_IN_CHAIN;
    return APRESULT_OK;
}

// A struct the caller hands over for ApricotFields to fill in: the same
// checks, made before anything is written to it.
static inline APRESULT apri_struct_out_check(const APRI_STRUCT_BASE_OUT *base, APRI_STRUCT_TYPE type)
{
    if (base->type != type)
        return APRESULT_STRUCT_WRONG_TYPE;
    if (base->next)
        return APRESULT_STRUCT_UNKNOWN_IN_CHAIN;
    return APRESULT_OK;
}

// What a failed check found, for a call's error message: "<the struct's
// name> <this>".
static inline const char *apri_struct_check_message(APRESULT result)
{
    if (result == APRESULT_STRUCT_WRONG_TYPE)
        return "has a base.type other than its own";
    return "has a struct chained on, and nothing can be chained onto it in this version";
}

// The base a struct ApricotFields makes for itself has: its own type, and no
// chain.
static inline APRI_STRUCT_BASE apri_struct_base(APRI_STRUCT_TYPE type)
{
    APRI_STRUCT_BASE base;
    base.type = type;
    base.next = NULL;
    return base;
}

#endif // APRISTRUCTBASE_HPP
