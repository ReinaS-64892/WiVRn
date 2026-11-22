// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wivrn
{
// Keep network observations in wall-clock windows, independently of frame rate.
// Only the applied budget affects new frames; an armed deadline never moves.
class receive_time_budget
{
	static constexpr int64_t second = 1'000'000'000;
	struct window
	{
		bool valid = false;
		int64_t epoch = 0;
		std::array<int64_t, 256> samples{};
		size_t count = 0, cursor = 0;
		int64_t late_peak = 0, recovery_target = 0;
	};
	std::array<window, 4> windows{};
	unsigned consecutive_losses = 0;
	const int64_t limit;
	int64_t budget;
	std::optional<int64_t> last_update;

	window & observation(int64_t now)
	{
		const auto epoch = now / second;
		auto & result = windows[static_cast<uint64_t>(epoch) % windows.size()];
		if (!result.valid || result.epoch != epoch)
			result = {.valid = true, .epoch = epoch};
		return result;
	}
	void update(int64_t now)
	{
		if (!last_update)
			last_update = now;
		if (now - *last_update < second)
			return;
		// No catch-up jump after an idle interval: apply one bounded step.
		last_update = now;
		std::array<int64_t, 1024> samples{};
		size_t count = 0;
		int64_t tail = 0, recovery = 0;
		const auto epoch = now / second;
		for (const auto & item: windows)
		{
			if (!item.valid || item.epoch > epoch || epoch - item.epoch >= int64_t(windows.size()))
				continue;
			std::copy_n(item.samples.begin(), item.count, samples.begin() + count);
			count += item.count;
			tail = std::max(tail, item.late_peak);
			recovery = std::max(recovery, item.recovery_target);
		}
		int64_t target = budget;
		if (count >= 8)
		{
			std::sort(samples.begin(), samples.begin() + count);
			const auto fast_count = (count * 3 + 3) / 4;
			int64_t total = 0;
			for (size_t i = 0; i < fast_count; ++i)
				total += samples[i];
			target = std::clamp<int64_t>((total / fast_count) * 7 / 4, 2'000'000, limit);
		}
		target = std::max({target, tail, recovery});
		// A small deadband avoids chasing measurement noise. Rise by at most
		// 0.5 ms/s; fall by at most 0.1 ms/s after the observation window clears.
		if (target > budget + 50'000)
			budget += std::min<int64_t>(target - budget, 500'000);
		else if (target < budget - 50'000)
			budget -= std::min<int64_t>(budget - target, 100'000);
	}

public:
	explicit receive_time_budget(int64_t limit) : limit(limit), budget(std::min<int64_t>(4'000'000, limit)) {}
	int64_t value() const
	{
		return budget;
	}
	void complete(int64_t duration, int64_t now)
	{
		consecutive_losses = 0;
		auto & item = observation(now);
		item.samples[item.cursor] = std::clamp<int64_t>(duration, 0, limit);
		item.cursor = (item.cursor + 1) % item.samples.size();
		item.count = std::min(item.count + 1, item.samples.size());
		update(now);
	}
	void late(int64_t duration, int64_t now)
	{
		auto & item = observation(now);
		item.late_peak = std::max(item.late_peak, std::clamp<int64_t>(duration + 500'000, 2'000'000, std::min<int64_t>(4'000'000, limit)));
		update(now);
	}
	void incomplete(int64_t now)
	{
		auto & item = observation(now);
		if (++consecutive_losses >= 8)
		{
			item.recovery_target = std::min(limit, std::max<int64_t>(4'000'000, budget * 3 / 2));
			consecutive_losses = 0;
		}
		update(now);
	}
};
} // namespace wivrn
