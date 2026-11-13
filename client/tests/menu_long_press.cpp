// SPDX-License-Identifier: GPL-3.0-or-later
#include "scenes/menu_long_press.h"
#include <stdexcept>

namespace
{
void require(bool value)
{
	if (not value)
		throw std::runtime_error("menu long press regression");
}
} // namespace

int main()
{
	scenes::menu_long_press press;
	// A button already held on connection or when the setting changes must not fire.
	require(not press.check(true, 0));
	require(not press.check(true, 2'000'000'000));
	require(not press.check(false, 3'000'000'000));
	require(not press.check(true, 4'000'000'000));
	require(not press.check(true, 4'999'999'999));
	require(press.check(true, 5'000'000'000));
	// Exactly one toggle per hold, even across many frames.
	require(not press.check(true, 6'000'000'000));
	require(not press.check(true, 7'000'000'000));
	require(not press.check(false, 8'000'000'000));
	require(not press.check(true, 9'000'000'000));
	require(press.check(true, 10'000'000'000));

	// Short taps do not accumulate into a long press.
	require(not press.check(false, 11'000'000'000));
	require(not press.check(true, 12'000'000'000));
	require(not press.check(false, 12'500'000'000));
	require(not press.check(true, 13'000'000'000));
	require(not press.check(true, 13'500'000'000));
	require(press.check(true, 14'000'000'000));

	// Lost focus or inactive input cancels the hold and requires a new release.
	require(not press.check(false, 15'000'000'000));
	require(not press.check(true, 16'000'000'000));
	require(not press.check(std::nullopt, 16'500'000'000));
	require(not press.check(true, 18'000'000'000));
	require(not press.check(false, 19'000'000'000));
	require(not press.check(true, 20'000'000'000));
	require(press.check(true, 21'000'000'000));

	// Switching shortcuts or disabling the GUI also cancels a pending hold.
	require(not press.check(false, 22'000'000'000));
	require(not press.check(true, 23'000'000'000));
	press.reset();
	require(not press.check(true, 25'000'000'000));
	require(not press.check(false, 26'000'000'000));
	require(not press.check(true, 27'000'000'000));
	require(press.check(true, 28'000'000'000));
}
