// SPDX-License-Identifier: GPL-3.0-or-later
#include "port_forwarding.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <map>
#include <netinet/tcp.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>
#include <set>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace wivrn
{
namespace
{
using bytes = std::vector<uint8_t>;
using clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr size_t max_datagram = 65472;
constexpr size_t max_buffer = 4 * 1024 * 1024; // memory bound, not a bandwidth limit
constexpr size_t max_flows = 256;
constexpr size_t header_size = 7;
enum message : uint8_t
{
	hello,
	open,
	data,
	fin,
	close_flow,
	datagram
};

struct fd
{
	int value = -1;
	fd() = default;
	explicit fd(int value) : value(value) {}
	fd(const fd &) = delete;
	fd & operator=(const fd &) = delete;
	fd(fd && other) noexcept : value(std::exchange(other.value, -1)) {}
	fd & operator=(fd && other) noexcept
	{
		fd old(std::move(other));
		std::swap(value, old.value);
		return *this;
	}
	~fd()
	{
		if (value >= 0)
			::close(value);
	}
};

void check(bool ok, const char * what)
{
	if (!ok)
		throw std::system_error(errno, std::generic_category(), what);
}

fd socket_for(int family, int type)
{
	fd result(::socket(family, type, 0));
	check(result.value >= 0, "forwarding socket");
	check(fcntl(result.value, F_SETFD, FD_CLOEXEC) >= 0, "forwarding close-on-exec");
	check(fcntl(result.value, F_SETFL, O_NONBLOCK) >= 0, "forwarding nonblocking");
	if (type == SOCK_STREAM)
	{
		int on = 1;
		check(setsockopt(result.value, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0, "forwarding TCP_NODELAY");
	}
	return result;
}

sockaddr_in loopback(uint16_t port)
{
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	return addr;
}

void put(bytes & out, uint64_t value, size_t count)
{
	for (size_t i = count; i; --i)
		out.push_back(value >> ((i - 1) * 8));
}
uint64_t get(std::span<const uint8_t> in)
{
	uint64_t result = 0;
	for (auto b: in)
		result = (result << 8) | b;
	return result;
}

// Independently authenticated records. TCP and UDP use distinct nonce prefixes;
// the two directions have independent random keys, renewed on every VR session.
struct cipher
{
	std::array<uint8_t, 32> key;
	uint64_t sequence = 0;
	uint64_t highest = 0, seen = 0;
	uint8_t domain;
	std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};

	bytes seal(std::span<const uint8_t> plain)
	{
		if (!ctx || sequence == std::numeric_limits<uint64_t>::max())
			throw std::runtime_error("forwarding cipher exhausted");
		bytes out{domain, 0, 0, 0};
		put(out, sequence++, 8);
		out.resize(12 + plain.size() + 16);
		int size = 0, tail = 0;
		if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(), out.data()) != 1 ||
		    EVP_EncryptUpdate(ctx.get(), out.data() + 12, &size, plain.data(), plain.size()) != 1 ||
		    EVP_EncryptFinal_ex(ctx.get(), out.data() + 12 + size, &tail) != 1 ||
		    EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, out.data() + out.size() - 16) != 1)
			throw std::runtime_error("forwarding encryption failed");
		return out;
	}
	bytes unseal(std::span<const uint8_t> in)
	{
		if (!ctx || in.size() < 28 || in[0] != domain || in[1] || in[2] || in[3])
			return {};
		auto seq = get(in.subspan(4, 8));
		if (seen && seq <= highest && (highest - seq >= 64 || (seen & (uint64_t(1) << (highest - seq)))))
			return {};
		bytes out(in.size() - 28);
		int size = 0, tail = 0;
		if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(), in.data()) != 1 ||
		    EVP_DecryptUpdate(ctx.get(), out.data(), &size, in.data() + 12, out.size()) != 1 ||
		    EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t *>(in.data() + in.size() - 16)) != 1 ||
		    EVP_DecryptFinal_ex(ctx.get(), out.data() + size, &tail) != 1)
			return {};
		if (!seen || seq > highest)
		{
			seen = !seen || seq - highest >= 64 ? 1 : (seen << (seq - highest)) | 1;
			highest = seq;
		}
		else
			seen |= uint64_t(1) << (highest - seq);
		return out;
	}
};

struct buffer
{
	bytes content;
	size_t offset = 0;
	bool empty() const
	{
		return offset == content.size();
	}
	void append(std::span<const uint8_t> value)
	{
		if (content.size() - offset + value.size() > max_buffer)
			throw std::runtime_error("forwarding buffer limit reached");
		if (offset)
		{
			content.erase(content.begin(), content.begin() + offset);
			offset = 0;
		}
		content.insert(content.end(), value.begin(), value.end());
	}
	bool flush(int socket)
	{
		if (empty())
			return true;
		auto n = send(socket, content.data() + offset, content.size() - offset, MSG_NOSIGNAL);
		if (n < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
		if (n == 0)
			return false;
		offset += n;
		if (empty())
		{
			content.clear();
			offset = 0;
		}
		return true;
	}
};

bytes packet(message kind, uint16_t rule, uint32_t flow, std::span<const uint8_t> payload = {})
{
	bytes out{kind};
	put(out, rule, 2);
	put(out, flow, 4);
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}
} // namespace

void validate_forwarding_rules(const std::vector<forwarding_rule> & rules)
{
	if (rules.size() > 64)
		throw std::runtime_error("At most 64 forwarding rules are supported");
	std::set<std::tuple<forwarding_rule::protocol, forwarding_rule::endpoint, uint16_t>> ports;
	for (const auto & rule: rules)
	{
		if ((rule.transport != forwarding_rule::protocol::tcp && rule.transport != forwarding_rule::protocol::udp) ||
		    (rule.source != forwarding_rule::endpoint::headset && rule.source != forwarding_rule::endpoint::pc) ||
		    !rule.listen_port || !rule.destination_port)
			throw std::runtime_error("Invalid port forwarding rule");
		if (!ports.emplace(rule.transport, rule.source, rule.listen_port).second)
			throw std::runtime_error("Forwarding rules cannot share a listening port on the same endpoint and protocol");
	}
}

struct port_forwarder::impl
{
	forwarding_setup setup;
	bool server;
	log_function log;
	fd listener, udp;
	sockaddr_in6 remote{};
	bool udp_ready = false;
	clock::time_point next_hello{};
	std::jthread thread;

	struct channel
	{
		fd socket;
		bool connecting = false, ready = false;
		clock::time_point started = clock::now();
		bytes input;
		buffer output;
		cipher tx, rx;
		uint64_t expected = 0;
		channel(fd socket, const forwarding_setup & s, bool server) :
		        socket(std::move(socket)), tx{server ? s.to_headset_key : s.to_pc_key, 0, 0, 0, 0}, rx{server ? s.to_pc_key : s.to_headset_key, 0, 0, 0, 0} {}
		void queue(const bytes & plain)
		{
			auto record = tx.seal(plain);
			bytes length;
			put(length, record.size(), 4);
			output.append(length);
			output.append(record);
		}
	};
	std::unique_ptr<channel> tunnel;
	std::map<int, std::unique_ptr<channel>> candidates;
	cipher udp_tx, udp_rx;

	struct bound_rule
	{
		uint16_t index;
		fd socket;
	};
	std::vector<bound_rule> bound;
	struct flow
	{
		uint16_t rule;
		fd socket; // destination socket, or accepted TCP socket; UDP sources use bound_rule's socket
		bool origin, connecting = false, eof = false, peer_eof = false, write_closed = false;
		buffer output;
		sockaddr_in sender{};
		clock::time_point last = clock::now();
	};
	std::map<uint32_t, flow> flows;
	uint32_t next_id;

	impl(const forwarding_setup & setup, bool server, log_function log) :
	        setup(setup), server(server), log(std::move(log)), udp_tx{server ? setup.to_headset_key : setup.to_pc_key, 0, 0, 0, 1}, udp_rx{server ? setup.to_pc_key : setup.to_headset_key, 0, 0, 0, 1}, next_id(server ? 2 : 1)
	{
		validate_forwarding_rules(setup.rules);
	}
	~impl()
	{
		if (thread.joinable())
		{
			thread.request_stop();
			thread.join();
		}
	}
	void report(const std::string & message)
	{
		if (log)
			log(message);
		else
			std::fprintf(stderr, "Port forwarding: %s\n", message.c_str());
	}
	bool local(const forwarding_rule & rule) const
	{
		return (rule.source == forwarding_rule::endpoint::pc) == server;
	}
	uint32_t allocate()
	{
		for (size_t i = 0; i <= max_flows; ++i)
		{
			auto id = next_id;
			next_id += 2;
			if (next_id < 2)
				next_id = server ? 2 : 1;
			if (!flows.contains(id))
				return id;
		}
		throw std::runtime_error("forwarding flow limit reached");
	}
	void bind_rules()
	{
		for (uint16_t i = 0; i < setup.rules.size(); ++i)
		{
			const auto & rule = setup.rules[i];
			if (!local(rule))
				continue;
			int type = rule.transport == forwarding_rule::protocol::tcp ? SOCK_STREAM : SOCK_DGRAM;
			auto socket = socket_for(AF_INET, type);
			if (type == SOCK_STREAM)
			{
				int on = 1;
				check(setsockopt(socket.value, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0, "forwarding reuse address");
			}
			auto addr = loopback(rule.listen_port);
			check(::bind(socket.value, (sockaddr *)&addr, sizeof(addr)) == 0, "forwarding local port bind");
			if (type == SOCK_STREAM)
				check(::listen(socket.value, 16) == 0, "forwarding local listen");
			bound.push_back({i, std::move(socket)});
		}
	}
	void queue(message kind, uint16_t rule, uint32_t id, std::span<const uint8_t> payload = {})
	{
		if (tunnel && tunnel->ready)
			tunnel->queue(packet(kind, rule, id, payload));
	}
	void send_udp(uint16_t rule, uint32_t id, std::span<const uint8_t> payload = {}, bool greeting = false)
	{
		if (!udp_ready && !greeting)
			return;
		auto record = udp_tx.seal(packet(greeting ? hello : datagram, rule, id, payload));
		sendto(udp.value, record.data(), record.size(), MSG_NOSIGNAL, (sockaddr *)&remote, sizeof(remote));
	}
	void disconnect()
	{
		tunnel.reset();
		flows.clear();
		udp_ready = false;
		// Per-session UDP counters deliberately survive auxiliary TCP reconnects.
	}
	void start_connect()
	{
		auto socket = socket_for(AF_INET6, SOCK_STREAM);
		auto addr = remote;
		addr.sin6_port = htons(setup.port);
		int r = ::connect(socket.value, (sockaddr *)&addr, sizeof(addr));
		if (r < 0 && errno != EINPROGRESS)
			return;
		tunnel = std::make_unique<channel>(std::move(socket), setup, server);
		// Only one auxiliary TCP connection is established per WiVRn session.
		// A failed tunnel requires a new VR handshake and fresh keys before reconnecting.
		tunnel->connecting = r < 0;
		tunnel->queue(packet(hello, 0, 0));
	}
	bool receive_channel(channel & c, const std::function<void(const bytes &)> & handle)
	{
		uint8_t scratch[65536];
		auto n = recv(c.socket.value, scratch, sizeof(scratch), 0);
		if (n == 0)
			return false;
		if (n < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
		c.input.insert(c.input.end(), scratch, scratch + n);
		while (c.input.size() >= 4)
		{
			size_t size = get(std::span(c.input).first(4));
			if (size < 28 + header_size || size > 65536 + 28 + header_size)
				return false;
			if (c.input.size() < size + 4)
				break;
			auto record = std::span(c.input).subspan(4, size);
			if (get(record.subspan(4, 8)) != c.expected++)
				return false;
			auto plain = c.rx.unseal(record);
			if (plain.size() < header_size)
				return false;
			handle(plain);
			c.input.erase(c.input.begin(), c.input.begin() + 4 + size);
		}
		return true;
	}
	void handle(message kind, uint16_t rule_index, uint32_t id, std::span<const uint8_t> payload)
	{
		if (rule_index >= setup.rules.size())
			return;
		const auto & rule = setup.rules[rule_index];
		bool is_udp = rule.transport == forwarding_rule::protocol::udp;
		if ((kind == datagram) != is_udp)
			return;
		auto it = flows.find(id);
		if (it == flows.end())
		{
			if (local(rule) || (kind != open && kind != datagram) || !id || (id % 2 == (server ? 0 : 1)) || flows.size() >= max_flows)
			{
				if (kind == open)
					queue(close_flow, rule_index, id);
				return;
			}
			auto socket = socket_for(AF_INET, is_udp ? SOCK_DGRAM : SOCK_STREAM);
			auto addr = loopback(rule.destination_port);
			int r = ::connect(socket.value, (sockaddr *)&addr, sizeof(addr));
			if (r < 0 && errno != EINPROGRESS)
			{
				if (!is_udp)
					queue(close_flow, rule_index, id);
				return;
			}
			it = flows.emplace(id, flow{rule_index, std::move(socket), false, r < 0}).first;
		}
		auto & f = it->second;
		if (f.rule != rule_index || (f.origin && !rule.bidirectional && (kind == data || kind == datagram)))
			return;
		f.last = clock::now();
		if (kind == datagram)
		{
			if (f.origin)
			{
				auto b = std::find_if(bound.begin(), bound.end(), [&](auto & b) { return b.index == rule_index; });
				if (b != bound.end())
					sendto(b->socket.value, payload.data(), payload.size(), MSG_NOSIGNAL, (sockaddr *)&f.sender, sizeof(f.sender));
			}
			else
				send(f.socket.value, payload.data(), payload.size(), MSG_NOSIGNAL);
		}
		else if (kind == data)
		{
			if (f.peer_eof)
			{
				queue(close_flow, rule_index, id);
				flows.erase(it);
			}
			else
				f.output.append(payload);
		}
		else if (kind == fin)
			f.peer_eof = true;
		else if (kind == close_flow)
			flows.erase(it);
	}
	void tcp_message(const bytes & plain)
	{
		auto kind = message(plain[0]);
		if (kind == datagram || kind == hello)
			return;
		handle(kind, get(std::span(plain).subspan(1, 2)), get(std::span(plain).subspan(3, 4)), std::span(plain).subspan(header_size));
	}
	void read_udp()
	{
		uint8_t scratch[65536];
		sockaddr_in6 from{};
		socklen_t len = sizeof(from);
		auto n = recvfrom(udp.value, scratch, sizeof(scratch), 0, (sockaddr *)&from, &len);
		if (n <= 0)
			return;
		auto plain = udp_rx.unseal({scratch, size_t(n)});
		if (plain.size() < header_size)
			return;
		if (plain[0] == hello)
		{
			if (server)
			{
				remote = from;
				udp_ready = true;
				send_udp(0, 0, {}, true);
			}
			else
				udp_ready = true;
		}
		else if (plain[0] == datagram && udp_ready)
			handle(datagram, get(std::span(plain).subspan(1, 2)), get(std::span(plain).subspan(3, 4)), std::span(plain).subspan(header_size));
	}
	void read_bound(bound_rule & b)
	{
		const auto & rule = setup.rules[b.index];
		if (rule.transport == forwarding_rule::protocol::tcp)
		{
			fd accepted(::accept(b.socket.value, nullptr, nullptr));
			if (accepted.value < 0 || flows.size() >= max_flows)
				return;
			check(fcntl(accepted.value, F_SETFD, FD_CLOEXEC) >= 0, "forwarding accept close-on-exec");
			check(fcntl(accepted.value, F_SETFL, O_NONBLOCK) >= 0, "forwarding accept nonblocking");
			int on = 1;
			setsockopt(accepted.value, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
			auto id = allocate();
			flows.emplace(id, flow{b.index, std::move(accepted), true});
			queue(open, b.index, id);
			return;
		}
		uint8_t scratch[max_datagram + 1];
		sockaddr_in sender{};
		socklen_t len = sizeof(sender);
		auto n = recvfrom(b.socket.value, scratch, sizeof(scratch), 0, (sockaddr *)&sender, &len);
		if (n < 0 || size_t(n) > max_datagram || !udp_ready)
			return;
		auto it = std::find_if(flows.begin(), flows.end(), [&](auto & entry) {
			auto & f = entry.second;
			return f.origin && f.rule == b.index && f.sender.sin_port == sender.sin_port && f.sender.sin_addr.s_addr == sender.sin_addr.s_addr;
		});
		if (it == flows.end())
		{
			if (flows.size() >= max_flows)
				return;
			it = flows.emplace(allocate(), flow{b.index, {}, true}).first;
			it->second.sender = sender;
		}
		it->second.last = clock::now();
		send_udp(b.index, it->first, {scratch, size_t(n)});
	}
	bool service_flow(uint32_t id, flow & f, short events)
	{
		const auto & rule = setup.rules[f.rule];
		bool is_udp = rule.transport == forwarding_rule::protocol::udp;
		if (f.connecting && (events & (POLLOUT | POLLERR | POLLHUP)))
		{
			int error = 0;
			socklen_t len = sizeof(error);
			if (getsockopt(f.socket.value, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error)
				return false;
			f.connecting = false;
		}
		if (f.connecting)
			return clock::now() - f.last < 10s;
		if (events & POLLOUT)
			if (!f.output.flush(f.socket.value))
				return false;
		if ((events & (POLLIN | POLLHUP)) && !f.eof)
		{
			uint8_t scratch[max_datagram + 1];
			auto n = recv(f.socket.value, scratch, sizeof(scratch), 0);
			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				return false;
			if (n == 0 && !is_udp)
			{
				f.eof = true;
				queue(fin, f.rule, id);
			}
			else if (n >= 0 && (!is_udp || size_t(n) <= max_datagram) && (f.origin || rule.bidirectional))
			{
				f.last = clock::now();
				if (is_udp)
					send_udp(f.rule, id, {scratch, size_t(n)});
				else
					queue(data, f.rule, id, {scratch, size_t(n)});
			}
		}
		if (f.peer_eof && f.output.empty() && !f.write_closed)
		{
			::shutdown(f.socket.value, SHUT_WR);
			f.write_closed = true;
		}
		return !(events & (POLLERR | POLLNVAL)) && !(f.eof && f.write_closed);
	}
	void run(std::stop_token stop)
	{
		try
		{
			if (!server)
				start_connect();
			while (!stop.stop_requested())
			{
				struct item
				{
					pollfd poll;
					uint32_t id;
					int kind;
				};
				std::vector<item> items;
				auto add = [&](int socket, short events, uint32_t id, int kind) { items.push_back({{socket, events, 0}, id, kind}); };
				if (server && !tunnel)
					add(listener.value, POLLIN, 0, 0);
				if (tunnel)
					add(tunnel->socket.value, short(POLLIN | ((!tunnel->output.empty() || tunnel->connecting) ? POLLOUT : 0)), 0, 1);
				for (auto & [id, c]: candidates)
					add(id, POLLIN, id, 2);
				add(udp.value, POLLIN, 0, 3);
				if (tunnel && tunnel->ready)
				{
					for (size_t i = 0; i < bound.size(); ++i)
						add(bound[i].socket.value, POLLIN, i, 4);
					for (auto & [id, f]: flows)
						if (f.socket.value >= 0 && (!f.eof || f.connecting || !f.output.empty()))
							add(f.socket.value, short((f.eof ? 0 : POLLIN) | ((f.connecting || !f.output.empty()) ? POLLOUT : 0)), id, 5);
				}
				std::vector<pollfd> polls;
				for (auto & i: items)
					polls.push_back(i.poll);
				int r = ::poll(polls.data(), polls.size(), 50);
				if (r < 0)
				{
					if (errno == EINTR)
						continue;
					check(false, "forwarding poll");
				}
				for (size_t i = 0; i < items.size(); ++i)
				{
					auto events = polls[i].revents;
					if (!events)
						continue;
					auto id = items[i].id;
					switch (items[i].kind)
					{
						case 0: {
							fd socket(::accept(listener.value, nullptr, nullptr));
							if (socket.value >= 0 && candidates.size() < 8)
							{
								check(fcntl(socket.value, F_SETFD, FD_CLOEXEC) >= 0, "forwarding tunnel close-on-exec");
								check(fcntl(socket.value, F_SETFL, O_NONBLOCK) >= 0, "forwarding tunnel nonblocking");
								int on = 1;
								setsockopt(socket.value, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
								int key = socket.value;
								candidates.emplace(key, std::make_unique<channel>(std::move(socket), setup, server));
							}
							break;
						}
						case 1: {
							if (!tunnel)
								break;
							bool ok = !(events & (POLLERR | POLLNVAL));
							if (tunnel->connecting && (events & POLLOUT))
							{
								int error = 0;
								socklen_t len = sizeof(error);
								ok &= getsockopt(tunnel->socket.value, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && !error;
								tunnel->connecting = false;
							}
							if (ok && !tunnel->connecting && (events & POLLOUT))
								ok = tunnel->output.flush(tunnel->socket.value);
							if (ok && (events & (POLLIN | POLLHUP)))
								ok = receive_channel(*tunnel, [&](const bytes & plain) {
									if (!tunnel->ready)
									{
										if (plain != packet(hello, 0, 0))
											throw std::runtime_error("invalid forwarding hello");
										tunnel->ready = true;
										report("Connected");
									}
									else
										tcp_message(plain);
								});
							if (!ok)
								throw std::runtime_error("forwarding connection closed; reconnect WiVRn to resume");
							break;
						}
						case 2: {
							auto it = candidates.find(id);
							if (it == candidates.end())
								break;
							bool authenticated = false;
							bool ok = receive_channel(*it->second, [&](const bytes & plain) {
								if (plain == packet(hello, 0, 0))
									authenticated = true;
							});
							if (ok && authenticated && !tunnel)
							{
								tunnel = std::move(it->second);
								tunnel->ready = true;
								tunnel->queue(packet(hello, 0, 0));
								candidates.clear();
								report("Connected");
							}
							else if (!ok || authenticated)
								candidates.erase(it);
							break;
						}
						case 3:
							read_udp();
							break;
						case 4:
							if (tunnel && tunnel->ready)
								read_bound(bound[id]);
							break;
						case 5: {
							auto it = flows.find(id);
							if (it != flows.end() && !service_flow(id, it->second, events))
							{
								if (!(it->second.eof && it->second.write_closed))
									queue(close_flow, it->second.rule, id);
								flows.erase(it);
							}
							break;
						}
					}
				}
				const auto now = clock::now();
				// FIN may arrive after the local read side has already reached EOF,
				// in which case there are no socket events left to trigger shutdown.
				for (auto & [id, f]: flows)
					if (f.peer_eof && f.output.empty() && !f.write_closed && !f.connecting)
					{
						::shutdown(f.socket.value, SHUT_WR);
						f.write_closed = true;
					}
				std::erase_if(flows, [](auto & entry) { return entry.second.eof && entry.second.write_closed; });
				if (tunnel && !tunnel->ready && now - tunnel->started > 30s)
					throw std::runtime_error("forwarding connection timed out");
				std::erase_if(candidates, [&](auto & entry) { return now - entry.second->started > 5s; });
				std::erase_if(flows, [&](auto & entry) { return setup.rules[entry.second.rule].transport == forwarding_rule::protocol::udp && now - entry.second.last > 60s; });
				if (!server && tunnel && tunnel->ready && now >= next_hello)
				{
					send_udp(0, 0, {}, true);
					next_hello = now + 1s;
				}
			}
		}
		catch (const std::exception & e)
		{
			report(e.what());
		}
		disconnect();
		bound.clear();
		candidates.clear();
		listener = fd{};
		udp = fd{};
	}
	void start()
	{
		if (!thread.joinable())
			thread = std::jthread([this](std::stop_token stop) { run(stop); });
	}
};

port_forwarder::port_forwarder(std::unique_ptr<impl> self) : self(std::move(self)) {}
port_forwarder::~port_forwarder() = default;
void port_forwarder::start()
{
	self->start();
}

std::unique_ptr<port_forwarder> port_forwarder::listen(forwarding_setup & setup, log_function log)
{
	validate_forwarding_rules(setup.rules);
	if (RAND_bytes(setup.to_headset_key.data(), setup.to_headset_key.size()) != 1 || RAND_bytes(setup.to_pc_key.data(), setup.to_pc_key.size()) != 1)
		throw std::runtime_error("forwarding key generation failed");
	auto self = std::make_unique<impl>(setup, true, std::move(log));
	self->listener = socket_for(AF_INET6, SOCK_STREAM);
	int on = 1;
	check(setsockopt(self->listener.value, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0, "forwarding tunnel reuse address");
	sockaddr_in6 addr{};
	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(setup.port);
	check(::bind(self->listener.value, (sockaddr *)&addr, sizeof(addr)) == 0, "forwarding tunnel bind");
	check(::listen(self->listener.value, 8) == 0, "forwarding tunnel listen");
	socklen_t len = sizeof(addr);
	check(getsockname(self->listener.value, (sockaddr *)&addr, &len) == 0, "forwarding tunnel port");
	setup.port = ntohs(addr.sin6_port);
	self->setup.port = setup.port;
	self->udp = socket_for(AF_INET6, SOCK_DGRAM);
	check(::bind(self->udp.value, (sockaddr *)&addr, sizeof(addr)) == 0, "forwarding UDP tunnel bind");
	self->bind_rules();
	return std::unique_ptr<port_forwarder>(new port_forwarder(std::move(self)));
}

std::unique_ptr<port_forwarder> port_forwarder::connect(const forwarding_setup & setup, sockaddr_in6 server, log_function log)
{
	if (!setup.port || setup.rules.empty())
		return {};
	auto self = std::make_unique<impl>(setup, false, std::move(log));
	server.sin6_port = htons(setup.port);
	self->remote = server;
	self->udp = socket_for(AF_INET6, SOCK_DGRAM);
	self->bind_rules();
	self->start();
	return std::unique_ptr<port_forwarder>(new port_forwarder(std::move(self)));
}
} // namespace wivrn
