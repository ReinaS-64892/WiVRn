// SPDX-License-Identifier: GPL-3.0-or-later
#include "crypto.h"
#include "protocol_version.h"
#include "video_fec_sender.h"

#include <iostream>
#include <random>
#include <source_location>
#include <stdexcept>

static void require(bool condition, std::source_location location = std::source_location::current())
{
	if (!condition)
		throw std::runtime_error("FEC regression at line " + std::to_string(location.line()));
}

int main()
{
	using namespace wivrn;
	using shard = to_headset::video_stream_data_shard;
	std::mt19937 rng(42);
	size_t recovered_count = 0;
	for (unsigned length: {1, 7, 31, 64, 65, 127, 511, 512, 513, 1019, 12000})
	{
		std::vector<uint8_t> original(length);
		for (auto & b: original)
			b = rng();
		const uint16_t count = (length + 63) / 64;
		std::vector<std::optional<shard>> sources(5 + count);
		std::vector<to_headset::video_stream_repair_shard> repairs;
		video_parity_builder builder;
		for (uint16_t i = 0; i < count; ++i)
		{
			shard s{};
			s.stream_item_idx = 1;
			s.frame_idx = 42;
			s.shard_idx = 5 + i;
			s.unit = {2, 5, count, length, 64, true};
			s.view_info.emplace();
			s.view_info->display_time = 12345;
			if (i + 1 == count)
				s.timing_info.emplace();
			s.payload = std::span(original).subspan(i * 64, std::min<size_t>(64, length - i * 64));
			sources[s.shard_idx] = s;
			if (auto repair = builder.push(s))
				repairs.push_back(std::move(*repair));
		}
		require(repairs.size() == (count + 7u) / 8u);
		for (const auto & repair: repairs)
		{
			require(valid_video_repair(repair.source, repair.source_count));
			require(!recover_video_symbol(repair, sources)); // All sources present.
			for (size_t missing = repair.source.shard_idx; missing < size_t(repair.source.shard_idx) + repair.source_count; ++missing)
			{
				auto received = sources;
				received[missing].reset();
				auto recovered = recover_video_symbol(repair, received);
				require(recovered && recovered->shard_idx == missing && recovered->frame_idx == 42 && recovered->stream_item_idx == 1);
				require(recovered->unit == sources[missing]->unit && std::ranges::equal(recovered->payload, sources[missing]->payload));
				require(bool(recovered->timing_info) == bool(sources[missing]->timing_info));
				require(recovered->view_info && recovered->view_info->display_time == 12345);
				++recovered_count;
				if (repair.source_count > 1)
				{
					auto second = missing == repair.source.shard_idx ? missing + 1 : repair.source.shard_idx;
					received[second].reset();
					require(!recover_video_symbol(repair, received)); // Two losses cannot be invented.
				}
			}
			auto invalid = repair;
			++invalid.source_count;
			require(!valid_video_repair(invalid.source, invalid.source_count));
			invalid = repair;
			++invalid.source.shard_idx;
			require(!valid_video_repair(invalid.source, invalid.source_count));
			invalid = repair;
			invalid.source.payload = invalid.source.payload.first(63);
			require(!valid_video_repair(invalid.source, invalid.source_count));
			// Every operand must match frame, stream, descriptor and exact length.
			if (repair.source_count > 1)
			{
				auto received = sources;
				received[repair.source.shard_idx].reset();
				++received[repair.source.shard_idx + 1]->frame_idx;
				require(!recover_video_symbol(repair, received));
				received = sources;
				received[repair.source.shard_idx].reset();
				++received[repair.source.shard_idx + 1]->unit.byte_count;
				require(!recover_video_symbol(repair, received));
			}
		}
	}

	// Exercise the actual sender ordering with the UDP stream cipher. Serialization
	// references payload spans, so encryption overwrites the encoder's source bytes.
	{
		crypto::encrypt_context encrypt(EVP_aes_128_ctr());
		crypto::decrypt_context decrypt(EVP_aes_128_ctr());
		std::array<uint8_t, 16> key{}, iv{};
		uint8_t sequence = 0;
		auto transfer = [&](auto outgoing) {
			serialization_packet packet;
			auto value = to_headset::packets{std::move(outgoing)};
			packet.serialize(value);
			auto & spans = static_cast<std::vector<std::span<uint8_t>> &>(packet);
			iv[0] = ++sequence;
			encrypt.set_key_and_iv(key, iv);
			encrypt.encrypt_in_place(spans);
			size_t size = 0;
			for (auto span: spans)
				size += span.size();
			auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[size]);
			size_t offset = 0;
			for (auto span: spans)
			{
				std::copy(span.begin(), span.end(), memory.get() + offset);
				offset += span.size();
			}
			decrypt.set_key_and_iv(key, iv);
			decrypt.decrypt_in_place({memory.get(), size});
			deserialization_packet input(memory, {memory.get(), size});
			return input.deserialize<to_headset::packets>();
		};
		std::vector<uint8_t> plain(16 * 1233 + 25);
		for (auto & b: plain)
			b = rng();
		auto encoder_bytes = plain;
		std::vector<std::optional<shard>> received(17);
		std::vector<to_headset::video_stream_repair_shard> repairs;
		video_parity_builder builder;
		for (uint16_t i = 0; i < 17; ++i)
		{
			shard s{};
			s.frame_idx = 987;
			s.shard_idx = i;
			s.unit = {0, 0, 17, uint32_t(plain.size()), 1233, true};
			s.view_info.emplace();
			s.view_info->display_time = 12345;
			if (i == 16)
				s.timing_info.emplace();
			s.payload = std::span(encoder_bytes).subspan(i * 1233, i == 16 ? 25 : 1233);
			send_video_source(s, &builder, [&](const auto & source) {
     auto wire = transfer(source);
     if (i % video_fec_group_size) received[i] = std::get<shard>(std::move(wire)); }, [&](auto repair) {
     auto wire = transfer(std::move(repair));
     repairs.push_back(std::get<to_headset::video_stream_repair_shard>(std::move(wire))); });
		}
		require(encoder_bytes != plain); // Confirm actual in-place transport mutation.
		for (const auto & repair: repairs)
		{
			auto recovered = recover_video_symbol(repair, received);
			require(bool(recovered));
			received[recovered->shard_idx] = std::move(*recovered);
		}
		std::vector<uint8_t> restored;
		for (const auto & source: received)
		{
			require(bool(source));
			restored.insert(restored.end(), source->payload.begin(), source->payload.end());
		}
		require(restored == plain && received.back()->timing_info);
	}
	// Worst-case wire metadata is budgeted for both packet types.
	shard s{};
	s.view_info.emplace();
	s.timing_info.emplace();
	const auto capacity = shard::max_payload_size - serialized_size(s) - 2;
	std::vector<uint8_t> payload(capacity, 99);
	s.payload = payload;
	s.unit = {0, 0, 1, uint32_t(capacity), uint16_t(capacity), true};
	video_parity_builder builder;
	auto repair = builder.push(s);
	require(bool(repair));
	serialization_packet output;
	auto value = to_headset::packets{*repair};
	output.serialize(value);
	std::vector<uint8_t> wire;
	for (auto span: static_cast<std::vector<std::span<uint8_t>> &>(output))
		wire.insert(wire.end(), span.begin(), span.end());
	require(wire.size() == shard::max_payload_size);
	auto owned = std::shared_ptr<uint8_t[]>(new uint8_t[wire.size()]);
	std::copy(wire.begin(), wire.end(), owned.get());
	deserialization_packet input(owned, {owned.get(), wire.size()});
	owned.reset();
	auto packet = input.deserialize<to_headset::packets>();
	auto parsed = std::get<to_headset::video_stream_repair_shard>(std::move(packet));
	wire.clear();
	wire.shrink_to_fit(); // Deserializer-owned source payload remains valid.
	std::vector<std::optional<shard>> absent;
	auto recovered = recover_video_symbol(parsed, absent);
	require(recovered && std::ranges::equal(recovered->payload, payload) && recovered->timing_info);
	std::cout << recovered_count << " single-symbol recoveries, multiple losses, metadata validation and 1400-byte repair wire round trip passed\n";
}
