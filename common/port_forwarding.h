// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <vector>

namespace wivrn
{
// Kept separate from the VR protocol and rendering code on purpose.
struct forwarding_rule
{
	enum class protocol : uint8_t
	{
		tcp,
		udp
	};
	enum class endpoint : uint8_t
	{
		headset,
		pc
	};
	protocol transport = protocol::udp;
	endpoint source = endpoint::headset;
	uint16_t listen_port = 9000;
	uint16_t destination_port = 9000;
	bool bidirectional = false;
};

struct forwarding_setup
{
	uint16_t port = 0; // zero disables forwarding
	std::array<uint8_t, 32> to_headset_key{};
	std::array<uint8_t, 32> to_pc_key{};
	std::vector<forwarding_rule> rules;
};

// Throws on invalid/overlapping rules. All application sockets use IPv4 loopback.
void validate_forwarding_rules(const std::vector<forwarding_rule> & rules);

class port_forwarder
{
	struct impl;
	std::unique_ptr<impl> self;
	port_forwarder(std::unique_ptr<impl>);

public:
	using log_function = std::function<void(const std::string &)>;
	// Binds a separate TCP/UDP port before advertising it in the WiVRn handshake.
	static std::unique_ptr<port_forwarder> listen(forwarding_setup & setup, log_function log = {});
	static std::unique_ptr<port_forwarder> connect(const forwarding_setup & setup, sockaddr_in6 server, log_function log = {});
	// Start the server worker after WiVRn forks its runtime process.
	void start();
	~port_forwarder();
};
} // namespace wivrn
