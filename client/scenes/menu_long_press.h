// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>

namespace scenes
{
// Times are OpenXR nanoseconds. Require a release after enabling or regaining focus.
class menu_long_press
{
	bool released = false;
	std::optional<int64_t> press_started;

public:
	void reset()
	{
		released = false;
		press_started.reset();
	}

	bool check(std::optional<bool> pressed, int64_t now)
	{
		if (not pressed)
		{
			reset();
			return false;
		}
		if (not *pressed)
		{
			released = true;
			press_started.reset();
			return false;
		}
		if (not released)
			return false;
		if (not press_started)
			press_started = now;
		if (now - *press_started < 1'000'000'000)
			return false;

		reset();
		return true;
	}
};
} // namespace scenes
