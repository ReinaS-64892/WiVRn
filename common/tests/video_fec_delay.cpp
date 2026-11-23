// SPDX-License-Identifier: GPL-3.0-or-later
#include "crypto.h"
#include "video_fec_interleave.h"
#include "video_fec_repair_queue.h"

#include <atomic>
#include <future>
#include <iostream>
#include <map>
#include <source_location>

using namespace wivrn;
using source_type = to_headset::video_stream_data_shard;
using repair_type = to_headset::video_stream_repair_shard;
using queue_type = video_fec_repair_queue;
using namespace std::chrono_literals;

static void require(bool condition, std::source_location where = std::source_location::current())
{
	if (!condition)
		throw std::runtime_error("Delayed FEC regression at line " + std::to_string(where.line()));
}

template <typename T>
static T wire_roundtrip(T source)
{
	to_headset::packets value{std::move(source)};
	serialization_packet packet;
	packet.serialize(value);
	auto & spans = static_cast<std::vector<std::span<uint8_t>> &>(packet);
	std::array<uint8_t, 16> key{}, iv{};
	crypto::encrypt_context encrypt(EVP_aes_128_ctr());
	encrypt.set_key_and_iv(key, iv);
	encrypt.encrypt_in_place(spans);
	size_t length = 0;
	for (auto span: spans)
		length += span.size();
	require(length <= source_type::max_payload_size);
	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[length]);
	size_t offset = 0;
	for (auto span: spans)
	{
		std::copy(span.begin(), span.end(), memory.get() + offset);
		offset += span.size();
	}
	crypto::decrypt_context decrypt(EVP_aes_128_ctr());
	decrypt.set_key_and_iv(key, iv);
	decrypt.decrypt_in_place({memory.get(), length});
	deserialization_packet input(memory, {memory.get(), length});
	return std::get<T>(input.deserialize<to_headset::packets>());
}

int main()
{
	// Follow the encoder path: capture parity before in-place encryption, send
	// sources immediately, then enqueue owned repairs 500 us after the last send.
	{
		queue_type queue;
		std::vector<std::vector<uint8_t>> storage(26);
		std::vector<source_type> originals;
		std::vector<std::vector<uint8_t>> expected;
		for (size_t i = 0; i < 26; ++i)
		{
			storage[i].resize(50 + i * 7, uint8_t(i + 11));
			source_type source{};
			source.frame_idx = 81;
			source.shard_idx = i;
			source.payload = storage[i];
			if (!i)
			{
				source.view_info.emplace();
				source.view_info->display_time = 12345;
			}
			if (i == 25)
				source.timing_info.emplace();
			expected.push_back(video_fec_source_bytes(source));
			originals.push_back(source);
		}
		std::vector<std::optional<source_type>> available(26);
		std::vector<repair_type> deferred;
		std::map<uint16_t, std::vector<repair_type>> received;
		size_t source_sends = 0;
		auto emit_source = [&](const auto & source) {
			auto packet = wire_roundtrip(source);
			++source_sends;
			// Erase the first and final metadata, plus several other sources.
			if (packet.shard_idx % 3 == 1 && packet.shard_idx != 25)
				available[packet.shard_idx] = std::move(packet);
		};
		auto hold_repair = [&](auto repair) { deferred.push_back(std::move(repair)); };
		send_interleaved_video(originals, emit_source, hold_repair, 8, 8, 8);
		require(source_sends == 26 && deferred.size() == 26);
		const auto due = queue_type::clock::now() + 500us;
		std::atomic<bool> early = false;
		queue.push({due, std::move(deferred), [&](repair_type && repair) {
			            if (queue_type::clock::now() < due)
				            early = true;
			            auto packet = wire_roundtrip(std::move(repair));
			            received[packet.first_shard].push_back(std::move(packet));
		            }});
		// Reusing/freed encoder buffers must not affect delayed parity.
		for (auto & bytes: storage)
			std::fill(bytes.begin(), bytes.end(), 0xcc);
		originals.clear();
		storage.clear();
		queue.wait_idle();
		require(!early);
		for (const auto & [first, repairs]: received)
			for (auto & source: recover_video_packets(repairs, available))
				available[source.shard_idx] = std::move(source);
		for (size_t i = 0; i < available.size(); ++i)
			require(available[i] && video_fec_source_bytes(*available[i]) == expected[i]);
	}
	// A long future job must not block the producer or an earlier-deadline job.
	// Cancelling it also tests prompt wakeup from a timed wait during teardown.
	{
		queue_type queue;
		std::atomic<bool> future_sent = false;
		queue.push({queue_type::clock::now() + 1h, {repair_type{}}, [&](auto &&) { future_sent = true; }});
		std::promise<void> sent;
		auto done = sent.get_future();
		queue.push({queue_type::clock::now(), {repair_type{}}, [&](auto &&) { sent.set_value(); }});
		require(done.wait_for(2s) == std::future_status::ready);
		queue.cancel();
		require(!future_sent);
		std::promise<void> restarted;
		auto ready = restarted.get_future();
		queue.push({queue_type::clock::now(), {repair_type{}}, [&](auto &&) { restarted.set_value(); }});
		require(ready.wait_for(2s) == std::future_status::ready);
		queue.wait_idle();
	}
	// Bounded overload discards old pending repairs rather than waiting on the
	// video sender. A send exception must not terminate the worker.
	{
		queue_type queue;
		std::atomic<size_t> sends = 0;
		for (size_t i = 0; i < queue_type::capacity + 5; ++i)
			queue.push({queue_type::clock::now() + 1h, {repair_type{}}, [&](auto &&) { ++sends; }});
		require(queue.overflow_count() == 5);
		queue.cancel();
		require(sends == 0);
		queue.push({queue_type::clock::now(), {repair_type{}, repair_type{}}, [&](auto &&) { ++sends; throw std::runtime_error("send failed"); }});
		queue.wait_idle();
		require(sends == 2);
	}
	// Cancellation cannot return while the callback still uses a connection.
	// Captures must be destroyed before idle is signalled to the owner.
	{
		queue_type queue;
		std::promise<void> entered, release;
		auto started = entered.get_future();
		auto gate = release.get_future().share();
		auto connection = std::make_shared<int>(42);
		std::weak_ptr<int> weak = connection;
		queue.push({queue_type::clock::now(), {repair_type{}}, [&, connection](auto &&) {
			            entered.set_value();
			            gate.wait_for(2s);
			            require(*connection == 42);
		            }});
		connection.reset();
		require(started.wait_for(2s) == std::future_status::ready);
		auto cancelled = std::async(std::launch::async, [&] { queue.pause(); });
		require(cancelled.wait_for(10ms) != std::future_status::ready);
		release.set_value();
		require(cancelled.wait_for(2s) == std::future_status::ready);
		cancelled.get();
		require(weak.expired());
	}
	// A paused/disconnected owner rejects future work until explicitly resumed.
	// Permanent stop remains closed even if a later reset attempts to resume.
	{
		queue_type queue;
		std::atomic<size_t> sends = 0;
		queue.pause();
		queue.push({queue_type::clock::now(), {repair_type{}}, [&](auto &&) { ++sends; }});
		queue.wait_idle();
		require(sends == 0);
		queue.resume();
		queue.push({queue_type::clock::now(), {repair_type{}}, [&](auto &&) { ++sends; }});
		queue.wait_idle();
		require(sends == 1);
		queue.stop();
		queue.resume();
		queue.push({queue_type::clock::now(), {repair_type{}}, [&](auto &&) { ++sends; }});
		queue.wait_idle();
		require(sends == 1);
		queue.stop();
	}
	std::atomic<bool> sent_at_shutdown = false;
	{
		queue_type queue;
		queue.push({queue_type::clock::now() + 1h, {repair_type{}}, [&](auto &&) { sent_at_shutdown = true; }});
	}
	require(!sent_at_shutdown);
	std::cout << "500 us repair timing, encrypted recovery, buffer ownership, deadline wakeup, bounded overload, pause/resume, cancellation and teardown passed\n";
}
