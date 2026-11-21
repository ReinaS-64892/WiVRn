// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace wivrn
{
// Learn network assembly time only from intact datagram frames. Decoder time,
// reliable recovery pictures and deadline-truncated samples must not train it.
class receive_time_budget
{
	std::array<int64_t, 32> samples{};
	size_t count = 0;
	size_t cursor = 0;
	unsigned consecutive_losses = 0;
	const int64_t limit;
	int64_t budget;

public:
	explicit receive_time_budget(int64_t limit) : limit(limit), budget(std::min<int64_t>(4'000'000, limit)) {}
	int64_t value() const
	{
		return budget;
	}
	void complete(int64_t duration)
	{
		consecutive_losses = 0;
		samples[cursor] = std::clamp<int64_t>(duration, 0, limit);
		cursor = (cursor + 1) % samples.size();
		count = std::min(count + 1, samples.size());
		if (count < 8)
			return;
		auto sorted = samples;
		std::sort(sorted.begin(), sorted.begin() + count);
		// Ignore the slowest quarter: sporadic successful-but-late frames must
		// not turn a 1-2 ms network path into a whole-period loss wait again.
		const size_t fast_count = (count * 3 + 3) / 4;
		int64_t total = 0;
		for (size_t i = 0; i < fast_count; ++i)
			total += sorted[i];
		budget = std::clamp<int64_t>((total / fast_count) * 13 / 10, 1'000'000, limit);
	}
	void incomplete()
	{
		if (++consecutive_losses < 8)
			return;
		// If the network changes, do not remain stuck censoring every slower
		// frame forever. Relearn after sustained loss, still within one period.
		budget = std::min(limit, std::max<int64_t>(4'000'000, budget * 3 / 2));
		count = cursor = consecutive_losses = 0;
	}
};
} // namespace wivrn
