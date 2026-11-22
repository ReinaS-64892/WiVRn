// SPDX-License-Identifier: GPL-3.0-or-later
#include "utils/ranges.h"

#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

template <typename T>
concept can_enumerate = requires(T && range) { utils::enumerate(std::forward<T>(range)); };

// A borrowed range must never outlive an unnamed source, even if it is a view.
static_assert(can_enumerate<std::vector<int> &>);
static_assert(can_enumerate<const std::vector<int> &>);
static_assert(can_enumerate<std::span<int> &>);
static_assert(!can_enumerate<std::vector<int>>);
static_assert(!can_enumerate<std::span<int>>);
static_assert(!can_enumerate<const std::span<int>>);

int main()
{
	std::vector<std::vector<float>> metrics(2, std::vector<float>(300, 1.f));
	for (bool fec_only: {false, true})
	{
		auto plotted_decoders = std::span(metrics).first(fec_only ? 0 : metrics.size());
		size_t visited = 0;
		for (auto && [index, values]: utils::enumerate(plotted_decoders))
		{
			if (index != visited || values.size() != 300 || &values != &metrics[index])
				throw std::runtime_error("Invalid borrowed statistics range");
			values[0] = float(index + 2);
			++visited;
		}
		if (visited != (fec_only ? 0 : metrics.size()))
			throw std::runtime_error("Incorrect statistics page selection");
	}
	const auto & const_metrics = metrics;
	for (auto && [index, values]: utils::enumerate(const_metrics))
		if (values[0] != float(index + 2))
			throw std::runtime_error("Enumeration lost its reference");
}
