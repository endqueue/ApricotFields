// Headless tests for the open-addressing table Aprend's command lists look
// handles and subresources up in (src/aprihashindex.hpp). It is an internal
// header, which this one test includes on purpose.

#include "aprihashindex.hpp"
#include <map>
#include <stdint.h>
#include <stdio.h>
#include <utility>
#include <vector>

static int failures = 0;
static const char *current_test = "";

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// The same sequence on every platform and every run.
static uint32_t random_state = 0x2545F491u;
static uint32_t next_random()
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void test_empty()
{
    current_test = "empty";
    apri_hash_index index;
    CHECK(index.find(0, [](uint32_t) { return true; }) == nullptr);
    index.clear();
    CHECK(index.count == 0);
}

// Keys as a command list's layout_index has them: the value is where the key
// is in an array, and keys that share a hash are told apart by the test.
// Checked against a map, with few hashes for many keys so they collide.
static void test_against_brute_force()
{
    current_test = "against brute force";
    apri_hash_index index;
    for (int round = 0; round < 3; round++)
    {
        std::vector<std::pair<uint64_t, uint32_t>> keys; // (hash, key)
        std::map<uint32_t, uint32_t> reference;          // key -> its place in keys
        for (int i = 0; i < 2000; i++)
        {
            uint32_t key = next_random() % 5000;
            // Aligned like pointers, and only 64 of them: many keys a hash.
            uint64_t hash = (uint64_t)(key % 64) * 16;
            auto same = [&](uint32_t at) { return keys[at].second == key; };
            const uint32_t *found = index.find(hash, same);
            auto known = reference.find(key);
            CHECK((found != nullptr) == (known != reference.end()));
            if (found && known != reference.end())
                CHECK(*found == known->second);
            if (!found)
            {
                keys.push_back({hash, key});
                index.add(hash, (uint32_t)keys.size() - 1);
                reference[key] = (uint32_t)keys.size() - 1;
            }
        }
        CHECK(index.count == reference.size());
        // Cleared between rounds, as a compile clears it: the slots stay.
        size_t slots = index.slots.size();
        index.clear();
        CHECK(index.count == 0 && index.slots.size() == slots);
        CHECK(index.find(16, [](uint32_t) { return true; }) == nullptr);
    }
}

int main()
{
    test_empty();
    test_against_brute_force();

    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all hash index tests passed\n");
    return 0;
}
