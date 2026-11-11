#ifndef RBTSTALE_HPP
#define RBTSTALE_HPP

// Debug builds' check for a destroyed handle (the workspace's DECISIONS.md,
// card X16, option (a)): a handle used after its destroy call, or destroyed
// twice. Every handle struct (the struct behind a public header's
// `typedef struct <name>_t *<name>;`) holds RBT_STALE_TAG, a word that
// reads RBT_LIVE while the object lives:
//
// - set by its default member initializer when the struct is constructed;
// - written RBT_DEAD by its own destructor, before the memory is freed, and
//   by RBT_KILL at a destroy call that only queues the free (a Rend
//   object released once the GPU is done with it reads DEAD from its
//   destroy call on, not from its eventual free);
// - checked by RBT_STALE in every public call that takes the handle, right
//   after the handle's null check, as an `if` of its own that refuses as
//   the null check refuses, with "<param> is destroyed" where the null check
//   has a message. A refusal prints one line on stderr, as the overlap check
//   (rbtoverlap.hpp) reports, so a stale handle is seen even where the
//   caller ignores the result.
//
// One LIVE and one DEAD value for the library, not one per type: enough for
// a use after destroy and a double destroy. Anything but LIVE refuses, so
// memory written over since is caught too.
//
// Only _DEBUG decides it. A Release build has no field, no write and no
// check: RBT_STALE is the constant false, so the `if` it is in goes.
//
// The quarantine (card X27): the tag is only worth reading while the memory
// under it is still the dead handle's; a block the heap gave to a new
// object of the same size can read LIVE again. So in a Debug build a
// handle with an owner is not freed by its destroy: the destroy does
// everything it did before (releases what the object holds, its destructor
// writes the tag DEAD), then RBT_RETIRE hands the object's block to its
// owner's quarantine (RBT_QUARANTINE, a member of the owner), and the
// owner frees every block it holds when it is destroyed itself, with
// free() as the destroy did. Until then nothing new is made in that memory,
// so the tag stays DEAD. A destroy that defers its free (Rend's release
// queue, rbtrend_instance_retire) keeps doing so: the quarantine is the
// last step, taken by the release function once the GPU is done, after the
// destructor. Release frees at once, as before: no quarantine, no member,
// no call.
//
// The owner is the object at the root of the handle's tree that the caller
// must destroy after it, one rule per kind:
//
// - Rend: everything made on an instance (a batch, buffer, vertex or
//   index pool, texture (2D or 3D), texture view, sampler, framebuffer,
//   shader, pipeline (graphics or compute), binding layout or set,
//   material, render or compute pass, draw list, lit environment, lit
//   scene, exposure, BLAS, TLAS, command list, GPU timer, swap chain), and
//   what Rend makes on it for itself: the instance (rbtrendcontext.h:
//   every handle of the instance is destroyed before it).
// - Rabbittus Archive: an entry reader: its reader (rbtarchive.h: every entry
//   reader is destroyed before the reader).
// - None, freed at once as before: the roots, which are every other handle
//   of the library (a Rend instance; an Rabbittus Archive codec table, writer
//   and reader; an Rabbittus Audio mixer and cue table; an Rabbittus Jobs pool; Rabbittus Math's
//   BVHs and polygons; an Rabbittus Sync session; Rabbittus Text's faces, chains, layouts
//   and atlases). Each is made from a desc alone; one that must outlive
//   another (a face its chain, a mesh BVH its instance BVH) is borrowed
//   by it, not its owner. And a block a create frees on its failure path,
//   which no caller ever held.
//
// The quarantine allocates nothing and can't fail: a dead block's first
// bytes, its debug_name's (rbtcore.h: debug_name is first in every handle
// struct), which the destructor has done with, hold its link; the tag
// after them stays DEAD. Blocks are pushed with a compare-and-swap, since
// destroys may come from any thread at once; the list is emptied by the
// owner's destructor, when nothing else may push. rbtarchive_tests counts,
// through the CRT's debug heap on Windows, that a reader holds its entry
// readers' blocks until it goes and that none is left after;
// rabbittustests' test_destroyed_quarantine does the same for a Rend
// instance. AddressSanitizer's leak check (Linux, macOS) covers the rest.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if _DEBUG
#include <assert.h>
#include <atomic>
#endif

// "APRLIVE!" and "APRDEAD!" in ASCII, in a debugger's memory view.
#define RBT_LIVE UINT64_C(0x214556494C525041)
#define RBT_DEAD UINT64_C(0x2144414544525041)

#if _DEBUG

struct rbt_stale_tag
{
    uint64_t word{RBT_LIVE};
    rbt_stale_tag() = default;
    rbt_stale_tag(const rbt_stale_tag &) = default;
    rbt_stale_tag &operator=(const rbt_stale_tag &) = default;
    ~rbt_stale_tag()
    {
        word = RBT_DEAD;
    }
};

// Whether [tag] isn't LIVE; if so, says which call was handed a destroyed
// [param].
inline bool rbt_stale(const rbt_stale_tag &tag, const char *who, const char *param)
{
    if (tag.word == RBT_LIVE)
        return false;
    fprintf(stderr, "rabbittus: %s: called with a destroyed %s\n", who, param);
    return true;
}

// The member, after a handle struct's debug_name (rbtcore.h: debug_name
// stays first).
#define RBT_STALE_TAG rbt_stale_tag stale_tag;
#define RBT_KILL(obj) ((obj)->stale_tag.word = RBT_DEAD)
#define RBT_STALE(obj, who, param) rbt_stale((obj)->stale_tag, (who), (param))

// A dead handle's block, linked through its own first bytes.
struct rbt_quarantined
{
    rbt_quarantined *next;
};

// The blocks an owner holds until it is destroyed: a member of the owner
// (RBT_QUARANTINE), which frees them all in its destructor.
struct rbt_quarantine
{
    std::atomic<rbt_quarantined *> first{nullptr};
    rbt_quarantine() = default;
    rbt_quarantine(const rbt_quarantine &) = delete;
    rbt_quarantine &operator=(const rbt_quarantine &) = delete;
    ~rbt_quarantine()
    {
        rbt_quarantined *block = first.exchange(nullptr, std::memory_order_acquire);
        while (block)
        {
            rbt_quarantined *next = block->next;
            free(block);
            block = next;
        }
    }
};

// Puts [block], an object whose destructor has run and whose stale tag is
// at [tag], first in [quarantine], whatever another thread pushes at once.
inline void rbt_quarantine_push(rbt_quarantine &quarantine, void *block, const void *tag)
{
    assert((const char *)tag >= (const char *)block + sizeof(rbt_quarantined));
    (void)tag;
    rbt_quarantined *dead = (rbt_quarantined *)block;
    rbt_quarantined *head = quarantine.first.load(std::memory_order_relaxed);
    do
        dead->next = head;
    while (!quarantine.first.compare_exchange_weak(head, dead, std::memory_order_release, std::memory_order_relaxed));
}

// The member an owner holds its dead handles' blocks in.
#define RBT_QUARANTINE rbt_quarantine stale_quarantine;
// A destroy's last step for a handle [obj] of type T made on [owner]: runs
// its destructor and keeps its block in [owner]'s quarantine, where
// Release frees it. [owner] is read before the destructor runs, so it may
// be a member of [obj].
#define RBT_RETIRE(owner, obj, T)                                                 \
    do                                                                             \
    {                                                                              \
        rbt_quarantine &rbt_retire_quarantine_ = (owner)->stale_quarantine;     \
        (obj)->~T();                                                               \
        rbt_quarantine_push(rbt_retire_quarantine_, (obj), &(obj)->stale_tag);  \
    } while (0)

#else

#define RBT_STALE_TAG
#define RBT_KILL(obj) ((void)0)
#define RBT_STALE(obj, who, param) false
#define RBT_QUARANTINE
#define RBT_RETIRE(owner, obj, T) \
    do                             \
    {                              \
        (obj)->~T();               \
        free(obj);                 \
    } while (0)

#endif

#endif // RBTSTALE_HPP
