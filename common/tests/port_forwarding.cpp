// SPDX-License-Identifier: GPL-3.0-or-later
#include "port_forwarding.h"
#include "port_forwarding_config.h"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace wivrn;

namespace
{
void require(bool ok, const char * message)
{
	if (!ok)
		throw std::runtime_error(message);
}
struct socket_fd
{
	int value;
	explicit socket_fd(int type) : value(socket(AF_INET, type, 0))
	{
		require(value >= 0, "socket");
	}
	explicit socket_fd(int value, bool) : value(value)
	{
		require(value >= 0, "accept");
	}
	~socket_fd()
	{
		close(value);
	}
	socket_fd(const socket_fd &) = delete;
};
sockaddr_in address(uint16_t port)
{
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	return addr;
}
uint16_t bind_port(int socket)
{
	auto addr = address(0);
	require(bind(socket, (sockaddr *)&addr, sizeof(addr)) == 0, "bind");
	socklen_t len = sizeof(addr);
	require(getsockname(socket, (sockaddr *)&addr, &len) == 0, "getsockname");
	return ntohs(addr.sin_port);
}
uint16_t unused_port(int type)
{
	socket_fd socket(type);
	return bind_port(socket.value);
}
bool readable(int socket, int timeout = 3000)
{
	pollfd p{socket, POLLIN, 0};
	return poll(&p, 1, timeout) == 1 && (p.revents & (POLLIN | POLLHUP));
}
std::string receive_udp(int socket)
{
	require(readable(socket), "UDP receive timed out");
	std::array<char, 65536> buf{};
	auto n = recv(socket, buf.data(), buf.size(), 0);
	require(n >= 0, "UDP recv");
	return {buf.data(), size_t(n)};
}
void send_all(int socket, const std::string & data)
{
	size_t offset = 0;
	while (offset < data.size())
	{
		auto n = send(socket, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
		require(n > 0, "TCP send");
		offset += n;
	}
}
std::string receive_all(int socket)
{
	std::string result;
	for (;;)
	{
		require(readable(socket, 10000), "TCP receive timed out");
		char buf[8192];
		auto n = recv(socket, buf, sizeof(buf), 0);
		require(n >= 0, "TCP receive");
		if (n == 0)
			return result;
		result.append(buf, n);
	}
}
sockaddr_in6 peer(bool ipv4)
{
	sockaddr_in6 result{};
	result.sin6_family = AF_INET6;
	if (ipv4)
	{
		result.sin6_addr.s6_addr[10] = result.sin6_addr.s6_addr[11] = 0xff;
		result.sin6_addr.s6_addr[12] = 127;
		result.sin6_addr.s6_addr[15] = 1;
	}
	else
		result.sin6_addr = in6addr_loopback;
	return result;
}
void udp_case(bool source_pc, bool bidirectional, bool ipv4)
{
	socket_fd destination(SOCK_DGRAM);
	auto dest = bind_port(destination.value);
	auto source = unused_port(SOCK_DGRAM);
	while (source == dest)
		source = unused_port(SOCK_DGRAM);
	forwarding_setup setup;
	setup.rules.push_back({forwarding_rule::protocol::udp, source_pc ? forwarding_rule::endpoint::pc : forwarding_rule::endpoint::headset, source, dest, bidirectional});
	auto server = port_forwarder::listen(setup);
	server->start();
	auto client = port_forwarder::connect(setup, peer(ipv4));
	std::this_thread::sleep_for(200ms);
	socket_fd a(SOCK_DGRAM), b(SOCK_DGRAM);
	auto addr = address(source);
	require(connect(a.value, (sockaddr *)&addr, sizeof(addr)) == 0, "UDP connect");
	require(connect(b.value, (sockaddr *)&addr, sizeof(addr)) == 0, "UDP connect B");
	// Replies must return to the correct local sender, including empty datagrams.
	for (int i = 0; i < 4; ++i)
	{
		int socket = i % 2 ? a.value : b.value;
		std::string value = i == 0 ? "" : (i == 3 ? std::string(60000, 'x') : std::to_string(i));
		require(send(socket, value.data(), value.size(), 0) == ssize_t(value.size()), "UDP send");
		require(readable(destination.value), "UDP destination timeout");
		std::array<char, 65536> buf{};
		sockaddr_in from{};
		socklen_t len = sizeof(from);
		auto n = recvfrom(destination.value, buf.data(), buf.size(), 0, (sockaddr *)&from, &len);
		require(n == ssize_t(value.size()) && std::string(buf.data(), n) == value, "UDP payload changed");
		require(sendto(destination.value, value.data(), value.size(), 0, (sockaddr *)&from, len) == ssize_t(value.size()), "UDP reply");
		if (bidirectional)
			require(receive_udp(socket) == value, "UDP reply routed incorrectly");
		else
			require(!readable(socket, 100), "Unidirectional UDP returned a reply");
	}
	client.reset();
	server.reset();
	// Listener lifetime is tied to the session.
	socket_fd rebound(SOCK_DGRAM);
	require(bind(rebound.value, (sockaddr *)&addr, sizeof(addr)) == 0, "UDP listener leaked");
}
void tcp_case(bool source_pc, bool bidirectional, bool ipv4)
{
	socket_fd destination(SOCK_STREAM);
	auto dest = bind_port(destination.value);
	require(listen(destination.value, 4) == 0, "TCP listen");
	auto source = unused_port(SOCK_STREAM);
	while (source == dest)
		source = unused_port(SOCK_STREAM);
	forwarding_setup setup;
	setup.rules.push_back({forwarding_rule::protocol::tcp, source_pc ? forwarding_rule::endpoint::pc : forwarding_rule::endpoint::headset, source, dest, bidirectional});
	auto server = port_forwarder::listen(setup);
	server->start();
	auto client = port_forwarder::connect(setup, peer(ipv4));
	std::this_thread::sleep_for(150ms);
	// Concurrent connections, a large transfer, and response only after EOF exercise
	// framing, partial writes, per-flow routing, and TCP half-close semantics.
	auto echo = std::async(std::launch::async, [&] {
		std::vector<std::future<void>> work;
		for (int i = 0; i < 3; ++i)
		{
			require(readable(destination.value), "TCP accept timeout");
			int accepted = accept(destination.value, nullptr, nullptr);
			work.push_back(std::async(std::launch::async, [accepted] {
				socket_fd socket(accepted, true);
				auto data = receive_all(socket.value);
				send_all(socket.value, data);
				shutdown(socket.value, SHUT_WR);
			}));
		}
		for (auto & w: work)
			w.get();
	});
	std::vector<std::future<void>> senders;
	for (int i = 0; i < 3; ++i)
		senders.push_back(std::async(std::launch::async, [&, i] {
			socket_fd socket(SOCK_STREAM);
			auto addr = address(source);
			require(connect(socket.value, (sockaddr *)&addr, sizeof(addr)) == 0, "TCP connect");
			std::string data(1024 * 1024, 'a' + i);
			send_all(socket.value, data);
			shutdown(socket.value, SHUT_WR);
			require(receive_all(socket.value) == (bidirectional ? data : ""), "TCP payload/reply mismatch");
		}));
	for (auto & s: senders)
		s.get();
	echo.get();
}
void fixed_port_case()
{
	forwarding_setup setup;
	setup.rules.push_back({forwarding_rule::protocol::udp, forwarding_rule::endpoint::headset, 9000, 9000, false});
	// Find a number free for both protocols, then explicitly reuse that number.
	auto reservation = port_forwarder::listen(setup);
	auto configured_port = setup.port;
	require(configured_port != 0, "No test port selected");
	reservation.reset();
	auto server = port_forwarder::listen(setup);
	require(setup.port == configured_port, "Configured forwarding port was changed");

	bool rejected = false;
	try
	{
		auto duplicate = port_forwarder::listen(setup);
	}
	catch (const std::system_error &)
	{
		rejected = true;
	}
	require(rejected, "Occupied forwarding port silently changed to another number");
}

void fork_case()
{
	socket_fd destination(SOCK_DGRAM);
	auto dest = bind_port(destination.value);
	auto source = unused_port(SOCK_DGRAM);
	while (source == dest)
		source = unused_port(SOCK_DGRAM);
	forwarding_setup setup;
	setup.rules.push_back({forwarding_rule::protocol::udp, forwarding_rule::endpoint::headset, source, dest, false});
	auto server = port_forwarder::listen(setup);
	int stop[2];
	require(pipe(stop) == 0, "pipe");
	auto pid = fork();
	require(pid >= 0, "fork");
	if (pid == 0)
	{
		close(stop[1]);
		try
		{
			server->start();
			char value;
			(void)read(stop[0], &value, 1);
			server.reset();
			_exit(0);
		}
		catch (...)
		{
			_exit(1);
		}
	}
	close(stop[0]);
	struct child_guard
	{
		pid_t pid;
		int pipe;
		~child_guard()
		{
			close(pipe);
			int status;
			(void)waitpid(pid, &status, 0);
		}
	} child{pid, stop[1]};
	// WiVRn's parent releases its copy after the child starts the runtime.
	server.reset();
	auto client = port_forwarder::connect(setup, peer(false));
	std::this_thread::sleep_for(200ms);
	socket_fd sender(SOCK_DGRAM);
	auto addr = address(source);
	const std::string payload = "after fork";
	require(sendto(sender.value, payload.data(), payload.size(), 0, (sockaddr *)&addr, sizeof(addr)) == ssize_t(payload.size()), "fork UDP send");
	require(receive_udp(destination.value) == payload, "Forwarding failed after fork");
}

void authentication_case()
{
	forwarding_setup setup;
	setup.rules.push_back({forwarding_rule::protocol::udp, forwarding_rule::endpoint::pc, unused_port(SOCK_DGRAM), unused_port(SOCK_DGRAM), true});
	std::atomic<int> connections = 0;
	auto log = [&](const std::string & message) { if (message == "Connected") ++connections; };
	auto server = port_forwarder::listen(setup, log);
	server->start();
	// Invalid length prefixes and an invalid AEAD key must not claim the tunnel
	// or keep the legitimate peer from authenticating afterwards.
	{
		socket_fd raw(SOCK_STREAM);
		auto addr = address(setup.port);
		require(connect(raw.value, (sockaddr *)&addr, sizeof(addr)) == 0, "raw connect");
		send_all(raw.value, std::string(4, char(0xff)));
	}
	auto wrong_setup = setup;
	wrong_setup.to_pc_key[0] ^= 1;
	auto wrong = port_forwarder::connect(wrong_setup, peer(true), log);
	std::this_thread::sleep_for(150ms);
	require(connections == 0, "Unauthenticated tunnel accepted");
	wrong.reset();
	auto client = port_forwarder::connect(setup, peer(true), log);
	for (int i = 0; i < 100 && connections < 2; ++i)
		std::this_thread::sleep_for(10ms);
	require(connections == 2, "Authentication did not recover after a rejected peer");
}

void invalid_rules()
{
	for (const char * value: {R"([{"protocol":"other","source":"pc","listen-port":1,"destination-port":2}])",
	                          R"([{"protocol":"udp","source":"pc","listen-port":65536,"destination-port":2}])",
	                          R"([{"protocol":"udp","source":"pc","listen-port":-1,"destination-port":2}])",
	                          R"([{"protocol":"tcp","source":"pc","listen-port":4294976296,"destination-port":2}])",
	                          R"([{"protocol":"udp","source":"pc","listen-port":9000.5,"destination-port":2}])",
	                          "{}"})
	{
		bool rejected = false;
		try
		{
			parse_forwarding_rules(nlohmann::json::parse(value));
		}
		catch (...)
		{
			rejected = true;
		}
		require(rejected, "Invalid configuration accepted");
	}
	forwarding_rule rule;
	bool rejected = false;
	try
	{
		validate_forwarding_rules({rule, rule});
	}
	catch (...)
	{
		rejected = true;
	}
	require(rejected, "Duplicate listeners accepted");
}
} // namespace
int main()
{
	try
	{
		invalid_rules();
		fixed_port_case();
		authentication_case();
		fork_case();
		for (bool pc: {false, true})
			for (bool replies: {false, true})
			{
				udp_case(pc, replies, pc != replies);
				tcp_case(pc, replies, pc == replies);
			}
		std::cout << "Port forwarding integration tests passed\n";
	}
	catch (const std::exception & e)
	{
		std::cerr << e.what() << '\n';
		return 1;
	}
}
