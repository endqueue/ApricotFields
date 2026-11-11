#ifndef RBTSTRUCTBASE_HPP
#define RBTSTRUCTBASE_HPP

// The checks every call makes of a caller's based struct (RBT_STRUCT_BASE,
// rbtcore.h) before it reads any other field, written once so that every
// module returns the same result for the same struct.
//
// These refuse any chain: for every struct but RBTJOBS_SCHEDULER, anything in
// a chain is a struct this build doesn't take there. A call that takes a
// chained struct walks its own chain instead (rbtjobs_scheduler_check, which
// takes RBTJOBS_SCHEDULER_GROUPS).

#include "rbtcore.h"
#include <stddef.h>

// A struct the caller filled in: [type] is the type the call takes there.
static inline RBT_RESULT rbt_struct_check(const RBT_STRUCT_BASE *base, RBT_STRUCT_TYPE type)
{
    if (base->type != type)
        return RBT_RESULT_STRUCT_WRONG_TYPE;
    if (base->next)
        return RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN;
    return RBT_RESULT_OK;
}

// A struct the caller hands over for Rabbittus to fill in: the same
// checks, made before anything is written to it.
static inline RBT_RESULT rbt_struct_out_check(const RBT_STRUCT_BASE_OUT *base, RBT_STRUCT_TYPE type)
{
    if (base->type != type)
        return RBT_RESULT_STRUCT_WRONG_TYPE;
    if (base->next)
        return RBT_RESULT_STRUCT_UNKNOWN_IN_CHAIN;
    return RBT_RESULT_OK;
}

// What a failed check found, for a call's error message: "<the struct's
// name> <this>".
static inline const char *rbt_struct_check_message(RBT_RESULT result)
{
    if (result == RBT_RESULT_STRUCT_WRONG_TYPE)
        return "has a base.type other than its own";
    return "has a struct chained on, and nothing can be chained onto it in this version";
}

// The base a struct Rabbittus makes for itself has: its own type, and no
// chain.
static inline RBT_STRUCT_BASE rbt_struct_base(RBT_STRUCT_TYPE type)
{
    RBT_STRUCT_BASE base;
    base.type = type;
    base.next = NULL;
    return base;
}

#endif // RBTSTRUCTBASE_HPP
