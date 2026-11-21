// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string_view>

namespace wivrn
{
// Milliseconds; -1 preserves the existing prediction policy.
inline int64_t pose_prediction_limit(const nlohmann::json & config, std::string_view key)
{
	auto it = config.find(key);
	if (it == config.end() || !it->is_number_integer())
		return -1;
	if (it->is_number_unsigned())
	{
		auto value = it->get<uint64_t>();
		return value <= 100 ? static_cast<int64_t>(value) : -1;
	}
	auto value = it->get<int64_t>();
	return value >= -1 && value <= 100 ? value : -1;
}
} // namespace wivrn
