// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <type_traits>

namespace wivrn
{
struct video_fec_settings
{
	bool enabled = false;
	uint8_t sources = 8;
	uint8_t repairs = 8;
	uint32_t repair_delay_us = 1000;

	uint8_t interleave() const
	{
		// Keep roughly 64 sources per window, within the scheduler's 16-group bound.
		return std::min(16, (64 + int(sources) - 1) / int(sources));
	}
};

// Parse each field independently so a malformed field cannot discard other settings.
inline video_fec_settings parse_video_fec_settings(const nlohmann::json & json)
{
	video_fec_settings result;
	auto it = json.find("video-fec");
	if (it == json.end() || !it->is_object())
		return result;
	if (auto enabled = it->find("enabled"); enabled != it->end() && enabled->is_boolean())
		result.enabled = enabled->get<bool>();
	auto count = [&](const char * name, auto & output, int minimum, int maximum) {
		auto value = it->find(name);
		if (value == it->end() || !value->is_number_integer())
			return;
		// Compare before narrowing; JSON may contain integers larger than int64_t.
		if (*value >= minimum && *value <= maximum)
			output = value->get<std::remove_reference_t<decltype(output)>>();
	};
	count("sources", result.sources, 1, 16);
	count("repairs", result.repairs, 1, 8);
	count("repair-delay-us", result.repair_delay_us, 0, 5000);
	return result;
}
} // namespace wivrn
