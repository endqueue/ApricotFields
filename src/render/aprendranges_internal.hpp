#ifndef APREND_RANGES_INTERNAL_HPP
#define APREND_RANGES_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/, but for
 * tests/aprendranges_tests.cpp.
 *
 * Which parts of a run of elements are free: the bookkeeping of one block of
 * a vertex or index pool (render/aprendbuffers.h, aprendpools_internal.hpp).
 * It counts elements and knows nothing of what they are or where they are
 * kept, so it includes nothing of Aprend or SpudGPU and runs with no device.
 * All of it is in this header for that reason. */

#include <stdint.h>

#include <cstddef>
#include <vector>

struct aprend_range_span {
	uint32_t offset;
	uint32_t count;
};

struct aprend_range_list {
	/* Elements in the run; 0 for a list that holds nothing. */
	uint32_t size{0};
	/* The free spans, by offset, none empty and no two touching: two free
	 * spans side by side are always one span. */
	std::vector<aprend_range_span> free{};

	/* Makes the list a run of [new_size] elements, all of them free. Throws
	 * std::bad_alloc, leaving a list that holds nothing. */
	void reset(uint32_t new_size) {
		size = 0;
		free.clear();
		if (new_size == 0)
			return;
		free.push_back({0, new_size});
		size = new_size;
	}

	/* Takes [count] elements from the lowest free span that holds them. False,
	 * with nothing taken, if [count] is 0 or no span is that long. Never
	 * allocates. */
	bool take(uint32_t count, uint32_t *out_offset) {
		if (count == 0)
			return false;
		for (size_t i = 0; i < free.size(); ++i) {
			if (free[i].count < count)
				continue;
			*out_offset = free[i].offset;
			free[i].offset += count;
			free[i].count -= count;
			if (free[i].count == 0)
				free.erase(free.begin() + (std::ptrdiff_t)i);
			return true;
		}
		return false;
	}

	/* The first free span that starts at or after [offset]: an index into
	 * free, free.size() if there is none. */
	size_t first_at_or_after(uint32_t offset) const {
		size_t low  = 0;
		size_t high = free.size();
		while (low < high) {
			const size_t middle = low + (high - low) / 2;
			if (free[middle].offset < offset)
				low = middle + 1;
			else
				high = middle;
		}
		return low;
	}

	/* Whether [offset, offset + count) lies inside the run. */
	bool contains(uint32_t offset, uint32_t count) const {
		if (count == 0)
			return false;
		if (offset > size)
			return false;
		if (count > size - offset)
			return false;
		return true;
	}

	/* Whether any element of [offset, offset + count), a span inside the
	 * run, is free. */
	bool overlaps_free(uint32_t offset, uint32_t count) const {
		const size_t next = first_at_or_after(offset);
		if (next > 0 && free[next - 1].offset + free[next - 1].count > offset)
			return true;
		if (next < free.size() && free[next].offset - offset < count)
			return true;
		return false;
	}

	/* Makes [offset, offset + count) free again, joining it to the free spans
	 * it touches. False, changing nothing, if the span is empty, runs outside
	 * the run, or holds an element that is already free. Throws
	 * std::bad_alloc, changing nothing, when it touches no free span and
	 * there is no memory to list it. */
	bool give(uint32_t offset, uint32_t count) {
		if (!contains(offset, count))
			return false;
		if (overlaps_free(offset, count))
			return false;
		const size_t next       = first_at_or_after(offset);
		const bool touches_prev = next > 0 && free[next - 1].offset + free[next - 1].count == offset;
		const bool touches_next = next < free.size() && offset + count == free[next].offset;
		if (touches_prev && touches_next) {
			free[next - 1].count += count + free[next].count;
			/* free.erase(free.begin() + next), written out: GCC 16 at -O3,
			 * inlining erase where it knows the vector's contents, warns of
			 * an overflowing memmove (-Wstringop-overflow) on a path that
			 * can't run. */
			for (size_t i = next; i + 1 < free.size(); ++i)
				free[i] = free[i + 1];
			free.pop_back();
			return true;
		}
		if (touches_prev) {
			free[next - 1].count += count;
			return true;
		}
		if (touches_next) {
			free[next].offset = offset;
			free[next].count += count;
			return true;
		}
		free.insert(free.begin() + (std::ptrdiff_t)next, {offset, count});
		return true;
	}

	/* Whether every element is free. False for a list that holds nothing. */
	bool is_whole() const { return size != 0 && free.size() == 1 && free[0].count == size; }

	/* How many elements are free. */
	uint32_t free_count() const {
		uint32_t total = 0;
		for (const aprend_range_span &span : free)
			total += span.count;
		return total;
	}
};

#endif // APREND_RANGES_INTERNAL_HPP
