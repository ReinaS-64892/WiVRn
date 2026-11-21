// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace wivrn
{
// Learn network assembly time only from intact datagram frames. Decoder time,
// reliable recovery pictures and truncated waits must not train that mean.
// Late source packets separately provide bounded evidence of network jitter.
class receive_time_budget
{
	std::array<int64_t, 32> samples{};
	size_t count = 0;
	size_t cursor = 0;
	unsigned consecutive_losses = 0;
	const int64_t limit;
	int64_t budget;
	int64_t late_budget = 0;

public:
	explicit receive_time_budget(int64_t limit) : limit(limit), budget(std::min<int64_t>(4'000'000, limit)) {}
	int64_t value() const
	{
		return std::max(budget, late_budget);
	}
	void complete(int64_t duration)
	{
		consecutive_losses = 0;
		late_budget = late_budget * 127 / 128;
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
		budget = std::clamp<int64_t>((total / fast_count) * 3 / 2, 2'000'000, limit);
	}
	void late(int64_t duration)
	{
		// A successful-only average is censored by our own timeout. Packets
		// arriving just after retirement provide evidence of the missing tail.
		// Bound its influence so a stalled packet cannot restore a whole-period
		// wait. Complete frames still leave immediately, regardless of this cap.
		late_budget = std::max(late_budget, std::clamp<int64_t>(duration + 500'000, 2'000'000, std::min<int64_t>(4'000'000, limit)));
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
