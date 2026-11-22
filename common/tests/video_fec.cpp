// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_fec.h"
#include "crypto.h"

#include <iostream>
#include <source_location>

using namespace wivrn;
using source_type = to_headset::video_stream_data_shard;
using repair_type = to_headset::video_stream_repair_shard;

static void require(bool condition, std::source_location where = std::source_location::current())
{
	if (!condition)
		throw std::runtime_error("Video FEC regression at line " + std::to_string(where.line()));
}

template <typename Packet>
static Packet wire_roundtrip(Packet source, bool encrypted = false)
{
	// Keep the variant alive: serialization keeps references to source spans.
	to_headset::packets value{std::move(source)};
	serialization_packet packet;
	packet.serialize(value);
	auto & spans = static_cast<std::vector<std::span<uint8_t>> &>(packet);
	std::array<uint8_t, 16> key{}, iv{};
	if (encrypted)
	{
		crypto::encrypt_context cipher(EVP_aes_128_ctr());
		cipher.set_key_and_iv(key, iv);
		cipher.encrypt_in_place(spans);
	}
	size_t size = 0;
	for (auto span: spans)
		size += span.size();
	require(size <= source_type::max_payload_size);
	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[size]);
	size_t offset = 0;
	for (auto span: spans)
	{
		std::copy(span.begin(), span.end(), memory.get() + offset);
		offset += span.size();
	}
	if (encrypted)
	{
		crypto::decrypt_context cipher(EVP_aes_128_ctr());
		cipher.set_key_and_iv(key, iv);
		cipher.decrypt_in_place({memory.get(), size});
	}
	deserialization_packet input(memory, {memory.get(), size});
	return std::get<Packet>(input.deserialize<to_headset::packets>());
}

int main()
{
	size_t recovered_count = 0;
	for (size_t count: {1, 2, 7, 8, 9, 16, 19})
	{
		std::vector<std::vector<uint8_t>> storage(count);
		std::vector<std::optional<source_type>> sources(count + 5);
		std::vector<repair_type> repairs;
		video_fec_encoder encoder;
		for (size_t i = 0; i < count; ++i)
		{
			storage[i].resize(13 + (i * 173) % 900, uint8_t(i + 17));
			source_type source{};
			source.stream_item_idx = 1;
			source.frame_idx = 42;
			source.shard_idx = i + 5;
			source.payload = storage[i];
			if (!i)
			{
				source.view_info.emplace();
				source.view_info->display_time = 123456;
			}
			if (i + 1 == count)
			{
				source.timing_info.emplace();
				source.timing_info->send_end = 98765;
			}
			sources[i + 5] = source;
			if (auto repair = encoder.push(source, i + 1 == count))
				repairs.push_back(wire_roundtrip(std::move(*repair)));
		}
		require(repairs.size() == (count + 7) / 8);
		for (auto & repair: repairs)
		{
			require(valid_video_repair(repair));
			require(!recover_video_packet(repair, sources)); // A lost repair alone needs no recovery.
			for (size_t missing = repair.first_shard; missing < size_t(repair.first_shard) + repair.source_count; ++missing)
			{
				auto received = sources;
				received[missing].reset();
				auto result = recover_video_packet(repair, received);
				require(result && video_fec_source_bytes(*result) == video_fec_source_bytes(*sources[missing]));
				++recovered_count;
				if (repair.source_count > 1)
				{
					const auto second = missing == repair.first_shard ? missing + 1 : repair.first_shard;
					received[second].reset();
					require(!recover_video_packet(repair, received));
					received = sources;
					received[missing].reset();
					++received[second]->frame_idx;
					require(!recover_video_packet(repair, received));
				}
			}
			auto invalid = repair;
			invalid.source_count = 9;
			require(!valid_video_repair(invalid));
			invalid = repair;
			invalid.source_sizes[0] = 0;
			require(!valid_video_repair(invalid));
			invalid = repair;
			invalid.payload = {};
			require(!valid_video_repair(invalid));
			invalid = repair;
			invalid.first_shard = UINT16_MAX;
			invalid.source_count = 2;
			require(!valid_video_repair(invalid));
			// A syntactically valid XOR body with an invalid recovered header is rejected.
			if (repair.source_count == 1)
			{
				auto broken = repair;
				broken.data.c.reset(new uint8_t[repair.payload.size()]);
				std::copy(repair.payload.begin(), repair.payload.end(), broken.data.c.get());
				broken.data.c[0] ^= 1;
				broken.payload = {broken.data.c.get(), repair.payload.size()};
				std::vector<std::optional<source_type>> absent;
				require(!recover_video_packet(broken, absent));
			}
		}
	}
	// Worst-case first+last metadata must fit both source and repair wire budgets.
	source_type source{};
	source.view_info.emplace();
	source.timing_info.emplace();
	const auto capacity = video_fec_symbol_capacity() - serialized_size(source);
	std::vector<uint8_t> payload(capacity, 101);
	source.payload = payload;
	video_fec_encoder encoder;
	auto repair = encoder.push(source, true);
	require(repair && serialized_size(*repair) + 1 == source_type::max_payload_size);
	require(serialized_size(source) + 1 <= source_type::max_payload_size);
	wire_roundtrip(source);
	// Use the same sequence as the server: capture plaintext parity BEFORE the
	// transport encrypts the original source payload spans in place.
	std::vector<std::vector<uint8_t>> bytes(8, std::vector<uint8_t>(512, 73));
	std::vector<std::optional<source_type>> received(8);
	std::optional<repair_type> protected_group;
	video_fec_encoder encrypted_encoder;
	for (size_t i = 0; i < 8; ++i)
	{
		source_type s{};
		s.frame_idx = 99;
		s.shard_idx = i;
		s.payload = bytes[i];
		if (!i)
		{
			s.view_info.emplace();
			s.view_info->display_time = 4567;
		}
		if (i == 7)
		{
			s.timing_info.emplace();
			s.timing_info->send_end = 8901;
		}
		auto pending = encrypted_encoder.push(s, i == 7);
		auto decoded = wire_roundtrip(s, true);
		require(bytes[i] != std::vector<uint8_t>(512, 73));
		if (i != 0)
			received[i] = std::move(decoded);
		if (pending)
			protected_group = wire_roundtrip(std::move(*pending), true);
	}
	require(bool(protected_group));
	auto restored = recover_video_packet(*protected_group, received);
	require(restored && restored->view_info && restored->view_info->display_time == 4567);
	require(std::ranges::all_of(restored->payload, [](uint8_t b) { return b == 73; }));
	received[0] = std::move(restored);
	received[7].reset();
	restored = recover_video_packet(*protected_group, received);
	require(restored && restored->timing_info && restored->timing_info->send_end == 8901);
	std::cout << recovered_count << " packet recoveries, metadata, malformed groups, encryption and 1400-byte wire limits passed\n";
}
