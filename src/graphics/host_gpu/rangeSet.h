#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_

#include "common/assert.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class RangeSet final {
public:
	void Add(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		if (m_ranges.empty()) {
			m_ranges.emplace_back(address, end);
			return;
		}
		// Fast path for monotonically increasing or repeated tail additions.
		if (auto& back = m_ranges.back(); address >= back.first) {
			if (address <= back.second) {
				if (end > back.second) {
					back.second = end;
				}
				return;
			}
			m_ranges.emplace_back(address, end);
			return;
		}

		// First range whose end >= address (can merge with [address, end)).
		auto it = std::lower_bound(m_ranges.begin(), m_ranges.end(), address,
		                           [](const Range& r, uint64_t addr) { return r.second < addr; });
		if (it == m_ranges.end() || it->first > end) {
			m_ranges.emplace(it, address, end);
			return;
		}
		if (it->first <= address && it->second >= end) {
			return;
		}
		it->first  = std::min(it->first, address);
		it->second = std::max(it->second, end);

		auto merge_end = it + 1;
		while (merge_end != m_ranges.end() && merge_end->first <= it->second) {
			it->second = std::max(it->second, merge_end->second);
			++merge_end;
		}
		if (merge_end != it + 1) {
			m_ranges.erase(it + 1, merge_end);
		}
	}

	void Subtract(uint64_t address, uint64_t size) {
		const auto end = End(address, size);
		if (m_ranges.empty() || end <= m_ranges.front().first || address >= m_ranges.back().second) {
			return;
		}
		auto it = std::lower_bound(m_ranges.begin(), m_ranges.end(), address,
		                           [](const Range& r, uint64_t addr) { return r.second <= addr; });
		if (it == m_ranges.end() || it->first >= end) {
			return;
		}
		// Single interval strictly split into two pieces.
		if (it->first < address && it->second > end) {
			const auto tail_end = it->second;
			it->second          = address;
			m_ranges.emplace(it + 1, end, tail_end);
			return;
		}
		// Trim left partial overlap in-place.
		if (it->first < address) {
			it->second = address;
			++it;
		}
		auto erase_begin = it;
		while (it != m_ranges.end() && it->second <= end) {
			++it;
		}
		// Trim right partial overlap in-place.
		if (it != m_ranges.end() && it->first < end) {
			it->first = end;
		}
		if (erase_begin != it) {
			m_ranges.erase(erase_begin, it);
		}
	}

	template <typename Func>
	void ForEach(Func&& func) const {
		for (const auto& [begin, end]: m_ranges) {
			func(begin, end);
		}
	}

	[[nodiscard]] bool Intersects(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		if (m_ranges.empty() || end <= m_ranges.front().first || address >= m_ranges.back().second) {
			return false;
		}
		const auto it =
		    std::lower_bound(m_ranges.begin(), m_ranges.end(), address,
		                     [](const Range& r, uint64_t addr) { return r.second <= addr; });
		return it != m_ranges.end() && it->first < end;
	}

	[[nodiscard]] bool Contains(uint64_t address, uint64_t size) const {
		const auto end = End(address, size);
		if (m_ranges.empty() || address < m_ranges.front().first || end > m_ranges.back().second) {
			return false;
		}
		const auto it =
		    std::lower_bound(m_ranges.begin(), m_ranges.end(), address,
		                     [](const Range& r, uint64_t addr) { return r.second <= addr; });
		return it != m_ranges.end() && it->first <= address && it->second >= end;
	}

	template <typename Func>
	void ForEachInRange(uint64_t address, uint64_t size, Func&& func) const {
		const auto end = End(address, size);
		if (m_ranges.empty() || end <= m_ranges.front().first || address >= m_ranges.back().second) {
			return;
		}
		auto it = std::lower_bound(m_ranges.begin(), m_ranges.end(), address,
		                           [](const Range& r, uint64_t addr) { return r.second <= addr; });
		for (; it != m_ranges.end() && it->first < end; ++it) {
			const auto begin = std::max(address, it->first);
			const auto last  = std::min(end, it->second);
			if (begin < last) {
				func(begin, last);
			}
		}
	}

	[[nodiscard]] bool Empty() const { return m_ranges.empty(); }

private:
	using Range = std::pair<uint64_t, uint64_t>;

	static uint64_t End(uint64_t address, uint64_t size) {
		if (size == 0 || size > UINT64_MAX - address) {
			EXIT("invalid range-set address or size\n");
		}
		return address + size;
	}

	std::vector<Range> m_ranges;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RANGESET_H_
