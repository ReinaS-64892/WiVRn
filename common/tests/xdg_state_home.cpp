// SPDX-License-Identifier: GPL-3.0-or-later
#include "utils/xdg_base_directory.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

int main()
{
	std::optional<std::string> original;
	if (const char * value = std::getenv("XDG_STATE_HOME"))
		original = value;
	int result = 0;
	try
	{
		setenv("XDG_STATE_HOME", "/tmp/wivrn-state-test", 1);
		if (xdg_state_home() != "/tmp/wivrn-state-test")
			throw std::runtime_error("absolute XDG_STATE_HOME");
		if (const char * home = std::getenv("HOME"); home and std::filesystem::path(home).is_absolute())
		{
			auto fallback = std::filesystem::path(home) / ".local/state";
			for (const char * value: {"", "relative/state"})
			{
				setenv("XDG_STATE_HOME", value, 1);
				if (xdg_state_home() != fallback)
					throw std::runtime_error("empty/relative state directory must use default");
			}
			unsetenv("XDG_STATE_HOME");
			if (xdg_state_home() != fallback)
				throw std::runtime_error("unset state directory must use default");
		}
		std::cout << "XDG state directory tests passed\n";
	}
	catch (const std::exception & error)
	{
		std::cerr << error.what() << '\n';
		result = 1;
	}
	if (original)
		setenv("XDG_STATE_HOME", original->c_str(), 1);
	else
		unsetenv("XDG_STATE_HOME");
	return result;
}
