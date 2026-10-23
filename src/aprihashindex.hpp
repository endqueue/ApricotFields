#ifndef APRIHASHINDEX_HPP
#define APRIHASHINDEX_HPP

// A table from a 64-bit hash to a 32-bit value - usually an index into an
// array the caller keeps, where the key itself is - for lookups made per
// command or per draw. Open addressing: one array of slots, probed in order
// from where the hash lands, so a find reads a cache line or two and follows
// no node pointer. Two keys may share a hash: find() takes a test of the
// key, called on each value whose hash matches. clear() keeps the slots, so
// a table filled again every frame stops allocating once it has grown.
//
// There is no erase: the tables it serves are filled, read and cleared.

#include "apriplaindata.hpp"
#include <stddef.h>
#include <stdint.h>
#include <vector>

struct apri_hash_index
{
    struct slot
    {
        uint64_t hash;
        uint32_t value;
        uint32_t used; // 1 if [hash] and [value] hold an entry
    };

    // A power of two long, or empty.
    std::vector<slot> slots;
    // Entries held.
    uint32_t count = 0;

    // Where [hash] lands in a table of [slot_count] slots (a power of two).
    static size_t home(uint64_t hash, size_t slot_count)
    {
        // Fibonacci hashing: the high bits of the product mix every bit of
        // the hash, so pointers (aligned, low bits zero) spread too.
        return (size_t)((hash * 0x9E3779B97F4A7C15ull) >> 32) & (slot_count - 1);
    }

    // The value of the first entry with [hash] that [same] (a test of a
    // value: bool same(uint32_t)) accepts, or null. Good until the next
    // add() or clear().
    template <typename Same>
    const uint32_t *find(uint64_t hash, Same &&same) const
    {
        if (count == 0)
            return nullptr;
        const size_t mask = slots.size() - 1;
        for (size_t i = home(hash, slots.size());; i = (i + 1) & mask)
        {
            const slot &at = slots[i];
            if (!at.used)
                return nullptr;
            if (at.hash == hash && same(at.value))
                return &at.value;
        }
    }

    // Adds an entry, without looking for one with the same key: find()
    // first. Grows the table when it is three quarters full, the one call
    // that allocates. Throws std::bad_alloc.
    void add(uint64_t hash, uint32_t value)
    {
        if (((size_t)count + 1) * 4 > slots.size() * 3)
            grow();
        const size_t mask = slots.size() - 1;
        size_t i = home(hash, slots.size());
        while (slots[i].used)
            i = (i + 1) & mask;
        slots[i] = slot{hash, value, 1};
        count++;
    }

    // Empties the table, keeping its slots.
    void clear()
    {
        if (count == 0)
            return;
        for (slot &at : slots)
            at = slot{0, 0, 0};
        count = 0;
    }

private:
    // Twice the slots (16 the first time), every entry placed again. Throws
    // std::bad_alloc, and the table is then as it was.
    void grow()
    {
        const size_t wanted = slots.empty() ? 16 : slots.size() * 2;
        std::vector<slot> grown(wanted, slot{0, 0, 0});
        const size_t mask = wanted - 1;
        for (const slot &at : slots)
        {
            if (!at.used)
                continue;
            size_t i = home(at.hash, wanted);
            while (grown[i].used)
                i = (i + 1) & mask;
            grown[i] = at;
        }
        slots.swap(grown);
    }
};
APRI_PLAIN_DATA(apri_hash_index::slot);

#endif // APRIHASHINDEX_HPP
