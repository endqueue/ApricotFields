#ifndef APRISTALE_HPP
#define APRISTALE_HPP

// Debug builds' check for a destroyed handle (the workspace's DECISIONS.md,
// card X16, option (a)): a handle used after its destroy call, or destroyed
// twice. Every handle struct (the struct behind a public header's
// `typedef struct <name>_t *<name>;`) holds APRI_STALE_TAG, a word that
// reads APRI_LIVE while the object lives:
//
// - set by its default member initializer when the struct is constructed;
// - written APRI_DEAD by its own destructor, before the memory is freed, and
//   by APRI_KILL at a destroy call that only queues the free (an Aprend
//   object released once the GPU is done with it reads DEAD from its
//   destroy call on, not from its eventual free);
// - checked by APRI_STALE in every public call that takes the handle, right
//   after the handle's null check, as an `if` of its own that refuses as
//   the null check refuses, with "<param> is destroyed" where the null check
//   has a message. A refusal prints one line on stderr, as the overlap check
//   (aprioverlap.hpp) reports, so a stale handle is seen even where the
//   caller ignores the result.
//
// One LIVE and one DEAD value for the library, not one per type: enough for
// a use after destroy and a double destroy. Memory a new object has reused
// can still read LIVE; AddressSanitizer covers that.
//
// Only _DEBUG decides it. A Release build has no field, no write and no
// check: APRI_STALE is the constant false, so the `if` it is in goes.

#include <stdint.h>
#include <stdio.h>

// "APRLIVE!" and "APRDEAD!" in ASCII, in a debugger's memory view.
#define APRI_LIVE UINT64_C(0x214556494C525041)
#define APRI_DEAD UINT64_C(0x2144414544525041)

#if _DEBUG

struct apri_stale_tag
{
    uint64_t word{APRI_LIVE};
    apri_stale_tag() = default;
    apri_stale_tag(const apri_stale_tag &) = default;
    apri_stale_tag &operator=(const apri_stale_tag &) = default;
    ~apri_stale_tag()
    {
        word = APRI_DEAD;
    }
};

// Whether [tag] isn't LIVE; if so, says which call was handed a destroyed
// [param].
inline bool apri_stale(const apri_stale_tag &tag, const char *who, const char *param)
{
    if (tag.word == APRI_LIVE)
        return false;
    fprintf(stderr, "apricotfields: %s: called with a destroyed %s\n", who, param);
    return true;
}

// The member, after a handle struct's debug_name (apricore.h: debug_name
// stays first).
#define APRI_STALE_TAG apri_stale_tag stale_tag;
#define APRI_KILL(obj) ((obj)->stale_tag.word = APRI_DEAD)
#define APRI_STALE(obj, who, param) apri_stale((obj)->stale_tag, (who), (param))

#else

#define APRI_STALE_TAG
#define APRI_KILL(obj) ((void)0)
#define APRI_STALE(obj, who, param) false

#endif

#endif // APRISTALE_HPP
