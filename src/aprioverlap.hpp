#ifndef APRIOVERLAP_HPP
#define APRIOVERLAP_HPP

// Debug builds' check of apricore.h's "Threads" rule: every object is used
// by one caller at a time. Each call that the rule covers (every call on an
// object but its destroy and the calls marked "Thread:") opens with
// APRI_ENTER on the object's guard, or on the guard of the unit it belongs
// to (an Aprend instance). A second thread entering while one is inside
// would otherwise be a silent race; it is printed on stderr with both
// calls' names, as Vulkan's validation layer reports an object used by two
// threads at once, and the call goes on. The same thread entering again (a
// guarded call making another) is not an overlap: the rule is about calls
// overlapping, not about which thread makes them, so a call from a
// different thread once the last has returned is not reported either.
// Nothing is refused, and a Release build checks nothing.

#include <atomic>
#include <stdint.h>
#include <stdio.h>
#include <thread>

// The guard an object holds. Present in every build, read only in Debug.
struct apri_overlap
{
    std::atomic<std::thread::id> thread{};
    uint32_t depth{0};
    // The call inside, for the message: a string literal.
    std::atomic<const char *> call{nullptr};
    // Overlaps found on this object, for tests/aprioverlap_tests.cpp.
    std::atomic<uint32_t> reports{0};
};

class apri_overlap_entry
{
  public:
    // [module] and [who] name the call ("aparchive", "aparchive_writer_add"),
    // [unit] what it is used through ("writer").
    apri_overlap_entry(apri_overlap &guard, const char *module, const char *who, const char *unit)
    {
#if _DEBUG
        const std::thread::id me = std::this_thread::get_id();
        std::thread::id expected{};
        if (guard.thread.compare_exchange_strong(expected, me))
        {
            guard_       = &guard;
            guard.depth  = 1;
            guard.call.store(who);
        }
        else if (expected == me)
        {
            guard_ = &guard;
            ++guard.depth;
        }
        else
        {
            const char *inside = guard.call.load();
            guard.reports.fetch_add(1);
            fprintf(stderr, "%s: %s: called while %s is inside the same %s on another thread (apricore.h, \"Threads\": one caller at a time)\n",
                    module, who, inside ? inside : "another call", unit);
        }
#else
        (void)guard;
        (void)module;
        (void)who;
        (void)unit;
#endif
    }
    ~apri_overlap_entry()
    {
#if _DEBUG
        if (guard_ && --guard_->depth == 0)
        {
            guard_->call.store(nullptr);
            guard_->thread.store(std::thread::id{});
        }
#endif
    }
    apri_overlap_entry(const apri_overlap_entry &)            = delete;
    apri_overlap_entry &operator=(const apri_overlap_entry &) = delete;

  private:
#if _DEBUG
    // NULL if this entry found another thread inside.
    apri_overlap *guard_{nullptr};
#endif
};

// A guard for an object whose header lets some calls overlap: calls that
// only read it may overlap each other (APRI_ROLE_READ), calls that each
// write only their own parts may overlap each other (APRI_ROLE_SHARE, as
// ApMath's set_instances on disjoint instances), and the rest overlap
// nothing (APRI_ROLE_ALONE). A call entering while another of a role it may
// not overlap is inside is reported, naming that role's last call. Counts,
// not thread ids: reads overlap by design, so "which thread" means nothing.
// Present in every build, read only in Debug.
enum apri_role : uint32_t
{
    APRI_ROLE_READ  = 0,
    APRI_ROLE_SHARE = 1,
    APRI_ROLE_ALONE = 2,
};

struct apri_shared_overlap
{
    std::atomic<uint32_t> inside[3]{};
    // Each role's last call to enter, for the message: a string literal.
    std::atomic<const char *> call[3]{};
    // Overlaps found on this object, for tests/aprioverlap_tests.cpp.
    std::atomic<uint32_t> reports{0};
};

class apri_shared_entry
{
  public:
    apri_shared_entry(apri_shared_overlap &guard, apri_role role, const char *module, const char *who, const char *unit)
    {
#if _DEBUG
        guard_ = &guard;
        role_  = role;
        guard.inside[role].fetch_add(1);
        guard.call[role].store(who);
        for (uint32_t other = 0; other < 3; ++other)
        {
            uint32_t count = guard.inside[other].load();
            if (other == (uint32_t)role)
                --count; // this call
            const bool allowed = other == (uint32_t)role && role != APRI_ROLE_ALONE;
            if (count == 0 || allowed)
                continue;
            const char *inside = other == (uint32_t)role ? nullptr : guard.call[other].load();
            guard.reports.fetch_add(1);
            fprintf(stderr, "%s: %s: called while %s is inside the same %s (its header says which calls may overlap)\n",
                    module, who, inside ? inside : "another call", unit);
            break;
        }
#else
        (void)guard;
        (void)role;
        (void)module;
        (void)who;
        (void)unit;
#endif
    }
    ~apri_shared_entry()
    {
#if _DEBUG
        guard_->inside[role_].fetch_sub(1);
#endif
    }
    apri_shared_entry(const apri_shared_entry &)            = delete;
    apri_shared_entry &operator=(const apri_shared_entry &) = delete;

  private:
#if _DEBUG
    apri_shared_overlap *guard_{nullptr};
    apri_role role_{APRI_ROLE_READ};
#endif
};

// One guard a line, so a call may enter two units (an Aprend list and its
// instance).
#define APRI_OVERLAP_NAME_(line) apri_overlap_guard_##line
#define APRI_OVERLAP_NAME(line) APRI_OVERLAP_NAME_(line)
#define APRI_ENTER(guard, module, who, unit) apri_overlap_entry APRI_OVERLAP_NAME(__LINE__)((guard), (module), (who), (unit))
#define APRI_ENTER_SHARED(guard, role, module, who, unit) \
    apri_shared_entry APRI_OVERLAP_NAME(__LINE__)((guard), (role), (module), (who), (unit))

#endif // APRIOVERLAP_HPP
