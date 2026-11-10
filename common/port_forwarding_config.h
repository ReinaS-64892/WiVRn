// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "port_forwarding.h"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace wivrn
{
inline std::vector<forwarding_rule> parse_forwarding_rules(const nlohmann::json & json)
{
	if (!json.is_array())
		throw std::runtime_error("Port forwarding rules must be an array");
	std::vector<forwarding_rule> rules;
	for (const auto & item: json)
	{
		auto protocol = item.at("protocol").get<std::string>();
		auto source = item.at("source").get<std::string>();
		if (!item.at("listen-port").is_number_integer() || !item.at("destination-port").is_number_integer())
			throw std::runtime_error("Forwarding ports must be integers");
		auto listen = item.at("listen-port").get<int64_t>();
		auto destination = item.at("destination-port").get<int64_t>();
		if ((protocol != "tcp" && protocol != "udp") || (source != "headset" && source != "pc") ||
		    listen < 1 || listen > 65535 || destination < 1 || destination > 65535)
			throw std::runtime_error("Invalid port forwarding rule");
		rules.push_back({protocol == "tcp" ? forwarding_rule::protocol::tcp : forwarding_rule::protocol::udp,
		                 source == "headset" ? forwarding_rule::endpoint::headset : forwarding_rule::endpoint::pc,
		                 uint16_t(listen),
		                 uint16_t(destination),
		                 item.value("bidirectional", false)});
	}
	validate_forwarding_rules(rules);
	return rules;
}
} // namespace wivrn
