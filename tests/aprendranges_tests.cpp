// Headless tests for the range list under Aprend's vertex and index pools
// (src/render/aprendranges_internal.hpp).
//
// The pools themselves need a device and aren't tested here. The list is the
// part of them that is only arithmetic: which elements of a block are free.
// It is an internal header, which this one test includes on purpose.

#include "aprendranges_internal.hpp"
#include <stdint.h>
#include <stdio.h>
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

// ---- Helpers ------------------------------------------------------------------

// The same sequence on every platform and every run.
static uint32_t random_state = 0x2545F491u;
static uint32_t next_random()
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

// The slow obvious list: one flag an element.
struct reference_t
{
    std::vector<bool> used;

    // The lowest run of [count] free elements, or false.
    bool take(uint32_t count, uint32_t *out_offset)
    {
        uint32_t run = 0;
        for (uint32_t i = 0; i < (uint32_t)used.size(); i++)
        {
            run = used[i] ? 0 : run + 1;
            if (run < count)
                continue;
            *out_offset = i + 1 - count;
            for (uint32_t j = *out_offset; j <= i; j++)
                used[j] = true;
            return true;
        }
        return false;
    }

    // False if any of it is free already.
    bool give(uint32_t offset, uint32_t count)
    {
        for (uint32_t i = offset; i < offset + count; i++)
        {
            if (!used[i])
                return false;
        }
        for (uint32_t i = offset; i < offset + count; i++)
            used[i] = false;
        return true;
    }

    // The free elements as maximal runs, lowest first: what the list holds.
    std::vector<aprend_range_span> spans() const
    {
        std::vector<aprend_range_span> result;
        for (uint32_t i = 0; i < (uint32_t)used.size(); i++)
        {
            if (used[i])
                continue;
            if (!result.empty() && result.back().offset + result.back().count == i)
                result.back().count++;
            else
                result.push_back({i, 1});
        }
        return result;
    }
};

static bool same_spans(const std::vector<aprend_range_span> &a, const std::vector<aprend_range_span> &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
    {
        if (a[i].offset != b[i].offset)
            return false;
        if (a[i].count != b[i].count)
            return false;
    }
    return true;
}

// ---- Tests --------------------------------------------------------------------

static void test_empty_list()
{
    current_test = "empty list";
    aprend_range_list list;
    uint32_t offset = 99;
    CHECK(!list.is_whole());
    CHECK(list.free_count() == 0);
    CHECK(!list.take(1, &offset));
    CHECK(!list.give(0, 1));
    CHECK(!list.contains(0, 1));

    list.reset(8);
    CHECK(list.is_whole());
    list.reset(0);
    CHECK(!list.is_whole());
    CHECK(list.size == 0);
    CHECK(list.free.empty());
}

static void test_take_and_give()
{
    current_test = "take and give";
    aprend_range_list list;
    list.reset(10);
    CHECK(list.is_whole());
    CHECK(list.free_count() == 10);

    uint32_t a = 99, b = 99, c = 99;
    CHECK(!list.take(0, &a));
    CHECK(!list.take(11, &a));
    CHECK(list.take(4, &a) && a == 0);
    CHECK(list.take(3, &b) && b == 4);
    CHECK(list.take(3, &c) && c == 7);
    CHECK(list.free.empty());
    CHECK(!list.is_whole());
    CHECK(!list.take(1, &a));

    // The middle one back: a hole neither end touches.
    CHECK(list.give(4, 3));
    CHECK(list.free.size() == 1 && list.free[0].offset == 4 && list.free[0].count == 3);
    // The lowest span that holds it.
    uint32_t d = 99;
    CHECK(list.take(2, &d) && d == 4);
    CHECK(list.give(4, 2));

    // The first: joins the hole after it.
    CHECK(list.give(0, 4));
    CHECK(list.free.size() == 1 && list.free[0].offset == 0 && list.free[0].count == 7);
    // The last: joins the span before it, and the list is whole again.
    CHECK(list.give(7, 3));
    CHECK(list.is_whole());
}

static void test_give_joins_both_sides()
{
    current_test = "give joins both sides";
    aprend_range_list list;
    list.reset(9);
    uint32_t a = 0, b = 0, c = 0;
    CHECK(list.take(3, &a) && list.take(3, &b) && list.take(3, &c));
    CHECK(list.give(a, 3));
    CHECK(list.give(c, 3));
    CHECK(list.free.size() == 2);
    CHECK(list.give(b, 3));
    CHECK(list.is_whole());
}

static void test_give_refused()
{
    current_test = "give refused";
    aprend_range_list list;
    list.reset(10);
    uint32_t a = 0;
    CHECK(list.take(6, &a) && a == 0);
    // Free is [6, 10).
    CHECK(!list.give(0, 0));
    CHECK(!list.give(10, 1));
    CHECK(!list.give(8, 3));
    CHECK(!list.give(UINT32_MAX, 2));
    CHECK(!list.give(6, 1));  // free already
    CHECK(!list.give(5, 2));  // runs into the free span after it
    CHECK(!list.give(9, 1));  // inside it
    CHECK(list.give(2, 2));   // free is now [2, 4) and [6, 10)
    CHECK(!list.give(1, 2));  // runs into the one after
    CHECK(!list.give(3, 2));  // starts in the one before
    CHECK(!list.give(0, 10)); // covers both
    CHECK(list.overlaps_free(3, 1) && list.overlaps_free(0, 3) && !list.overlaps_free(4, 2));
    // Nothing above changed it.
    CHECK(list.free.size() == 2 && list.free_count() == 6);
    CHECK(list.give(4, 2));
    CHECK(list.give(0, 2));
    CHECK(list.is_whole());
}

static void test_against_brute_force()
{
    current_test = "against brute force";
    const uint32_t size = 257;
    aprend_range_list list;
    list.reset(size);
    reference_t reference;
    reference.used.assign(size, false);
    std::vector<aprend_range_span> taken;

    for (uint32_t step = 0; step < 20000; step++)
    {
        const uint32_t roll = next_random() % 100;
        if (roll < 55 || taken.empty())
        {
            const uint32_t count = 1 + next_random() % 24;
            uint32_t offset = 0, expected = 0;
            const bool got = list.take(count, &offset);
            const bool want = reference.take(count, &expected);
            CHECK(got == want);
            if (got && want)
            {
                CHECK(offset == expected);
                taken.push_back({offset, count});
            }
        }
        else if (roll < 95)
        {
            const size_t which = next_random() % taken.size();
            const aprend_range_span span = taken[which];
            taken[which] = taken.back();
            taken.pop_back();
            CHECK(reference.give(span.offset, span.count));
            CHECK(list.give(span.offset, span.count));
        }
        else
        {
            // Something that isn't all taken: refused, and nothing changes.
            const uint32_t offset = next_random() % size;
            const uint32_t count = 1 + next_random() % 8;
            bool all_taken = offset + count <= size;
            for (uint32_t i = offset; all_taken && i < offset + count; i++)
                all_taken = reference.used[i];
            if (!all_taken)
                CHECK(!list.give(offset, count));
        }
        if (!same_spans(list.free, reference.spans()))
        {
            printf("FAIL %s: the list and the reference disagree at step %u\n", current_test, step);
            failures++;
            return;
        }
    }

    for (const aprend_range_span &span : taken)
        CHECK(list.give(span.offset, span.count));
    CHECK(list.is_whole());
}

int main()
{
    test_empty_list();
    test_take_and_give();
    test_give_joins_both_sides();
    test_give_refused();
    test_against_brute_force();

    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all range list tests passed\n");
    return 0;
}
