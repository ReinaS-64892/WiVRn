// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <type_traits>
#include <utility>
#include <variant>

namespace wivrn
{
template <typename Visitor, typename Packet>
void dispatch_packet(Visitor & visitor, Packet && packet, bool reliable)
{
	std::visit([&](auto && value) {
		if constexpr (std::is_invocable_v<Visitor &, decltype(value), bool>)
			visitor(std::forward<decltype(value)>(value), reliable);
		else
			visitor(std::forward<decltype(value)>(value));
	},
	           std::forward<Packet>(packet));
}
} // namespace wivrn
