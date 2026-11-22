// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder/receive_time_budget.h"
#include <iostream>
#include <stdexcept>

static void require(bool condition)
{
	if (!condition)
		throw std::runtime_error("Receive budget wall-clock regression");
}

int main()
{
	constexpr int64_t second = 1'000'000'000;
	// Identical paths have identical behaviour at different stream rates.
	for (int fps: {30, 90, 144})
	{
		wivrn::receive_time_budget budget(11'111'111);
		int64_t previous = budget.value(), changed_at = -second;
		for (int frame = 0; frame <= fps * 25; ++frame)
		{
			const auto now = int64_t(frame) * second / fps;
			budget.complete(600'000, now);
			if (budget.value() != previous)
			{
				require(now - changed_at >= second);
				require(previous - budget.value() <= 100'000);
				changed_at = now;
				previous = budget.value();
			}
			if (frame == fps)
				require(budget.value() == 3'900'000);
			if (frame == fps * 10)
				require(budget.value() == 3'000'000);
		}
		require(budget.value() == 2'000'000);
		budget.late(2'200'000, 25 * second + 1);
		require(budget.value() == 2'000'000); // no packet-by-packet jump
		for (int frame = 1; frame <= fps * 3; ++frame)
			budget.complete(600'000, 25 * second + int64_t(frame) * second / fps);
		require(budget.value() == 2'700'000); // retained over seconds, not frames
		for (int frame = 1; frame <= fps; ++frame)
			budget.complete(600'000, 28 * second + int64_t(frame) * second / fps);
		require(budget.value() == 2'600'000);
		for (int frame = 1; frame <= fps * 10; ++frame)
			budget.complete(600'000, 29 * second + int64_t(frame) * second / fps);
		require(budget.value() == 2'000'000);
		// A long pause permits one bounded increase, not accumulated steps.
		budget.late(35'000'000, 100 * second);
		require(budget.value() == 2'500'000);
		for (int i = 0; i < 1000; ++i)
			budget.late(35'000'000, 100 * second + 1);
		require(budget.value() == 2'500'000);
		for (int i = 101; i <= 110; ++i)
		{
			const auto before = budget.value();
			budget.late(35'000'000, i * second);
			require(budget.value() - before <= 500'000 && budget.value() <= 4'000'000);
		}
	}
	// Slow successful outliers do not dominate the fast mean.
	wivrn::receive_time_budget trimmed(11'111'111);
	for (int frame = 0; frame <= 32 * 12; ++frame)
		trimmed.complete(frame % 4 == 3 ? 8'000'000 : 2'000'000, int64_t(frame) * second / 32);
	require(trimmed.value() == 3'500'000);
	// Persistent loss still increases the target, with the same rate limit.
	wivrn::receive_time_budget loss(11'111'111);
	for (int frame = 0; frame <= 600; ++frame)
	{
		const auto before = loss.value();
		loss.incomplete(int64_t(frame) * second / 10);
		require(loss.value() - before <= 500'000 && loss.value() <= 11'111'111);
	}
	require(loss.value() == 11'111'111);
	wivrn::receive_time_budget high_fps(2'000'000);
	for (int frame = 0; frame < 5000; ++frame)
		high_fps.complete(600'000, int64_t(frame) * 1'000'000); // capped samples in a full bucket
	for (int i = 0; i < 20; ++i)
		high_fps.late(35'000'000, i * second);
	require(high_fps.value() == 2'000'000);
	std::cout << "Receive budget: wall-clock windows, rate limits, jitter retention, idle, outliers and recovery passed\n";
}
