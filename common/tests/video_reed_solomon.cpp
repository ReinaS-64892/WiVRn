// SPDX-License-Identifier: GPL-3.0-or-later
#include "crypto.h"
#include "video_fec_interleave.h"

#include <bit>
#include <iostream>
#include <map>
#include <source_location>

using namespace wivrn;
using source_type = to_headset::video_stream_data_shard;
using repair_type = to_headset::video_stream_repair_shard;
static void require(bool condition, std::source_location where = std::source_location::current())
{
	if (!condition)
		throw std::runtime_error("Video RS regression at line " + std::to_string(where.line()));
}
// Independent polynomial multiplication oracle, without logarithm tables.
static uint8_t polynomial_multiply(unsigned a, unsigned b)
{
	unsigned result = 0;
	while (b)
	{
		if (b & 1)
			result ^= a;
		b >>= 1;
		a <<= 1;
		if (a & 256)
			a ^= 0x11d;
	}
	return result;
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
	for (unsigned a = 0; a < 256; ++a)
		for (unsigned b = 0; b < 256; ++b)
			require(video_rs::multiply(a, b) == polynomial_multiply(a, b));
	for (unsigned a = 1; a < 256; ++a)
		require(polynomial_multiply(a, video_rs::inverse(a)) == 1);
	size_t checked = 0;
	for (size_t count: {1, 7, 16})
		for (uint8_t repair_count = 1; repair_count <= 4; ++repair_count)
		{
			std::vector<std::vector<uint8_t>> storage(count);
			std::vector<source_type> originals;
			std::vector<std::vector<uint8_t>> expected;
			for (size_t i = 0; i < count; ++i)
			{
				storage[i].resize(19 + (i * 113) % 800, uint8_t(i + 37));
				source_type s{};
				s.stream_item_idx = 1;
				s.frame_idx = 876;
				s.shard_idx = i + 5;
				s.payload = storage[i];
				if (!i)
				{
					s.view_info.emplace();
					s.view_info->display_time = 12345;
				}
				if (i + 1 == count)
					s.timing_info.emplace();
				expected.push_back(video_fec_source_bytes(s));
				originals.push_back(s);
			}
			// Build ALL parity first, then exercise in-place transport encryption.
			auto repairs = encode_video_rs(originals, repair_count);
			for (auto & s: originals)
				s = wire_roundtrip(s);
			for (auto & repair: repairs)
				repair = wire_roundtrip(std::move(repair));
			// Exhaust every erasure combination of up to R data/repair symbols.
			// Also reject R+1 lost data where enough sources exist.
			const size_t total = count + repair_count;
			for (unsigned mask = 0; mask < (1u << total); ++mask)
			{
				if (unsigned(std::popcount(mask)) > repair_count)
					continue;
				std::vector<std::optional<source_type>> available(count + 5);
				std::vector<repair_type> surviving;
				for (size_t i = 0; i < count; ++i)
					if (!(mask & (1u << i)))
						available[i + 5] = originals[i];
				for (size_t i = 0; i < repair_count; ++i)
					if (!(mask & (1u << (count + i))))
						surviving.push_back(repairs[i]);
				std::reverse(surviving.begin(), surviving.end());
				for (auto & packet: recover_video_packets(surviving, available))
					available[packet.shard_idx] = std::move(packet);
				for (size_t i = 0; i < count; ++i)
					require(available[i + 5] && video_fec_source_bytes(*available[i + 5]) == expected[i]);
				++checked;
			}
			std::vector<std::optional<source_type>> absent(count + 5);
			for (size_t i = repair_count + 1; i < count; ++i)
				absent[i + 5] = originals[i];
			if (count > repair_count)
				require(recover_video_packets(repairs, absent).empty());
			// Duplicated, conflicting or invalid equations must never add rank.
			auto invalid = repairs;
			invalid.push_back(repairs.front());
			require(recover_video_packets(invalid, absent).empty());
			invalid = repairs;
			invalid.front().repair_index = repair_count;
			require(!valid_video_repair(invalid.front()));
			invalid = repairs;
			invalid.front().source_count = 17;
			require(!valid_video_repair(invalid.front()));
			if (repairs.size() > 1)
			{
				invalid = repairs;
				++invalid.back().frame_idx;
				require(recover_video_packets(invalid, absent).empty());
				invalid = repairs;
				--invalid.back().source_sizes[0];
				require(recover_video_packets(invalid, absent).empty());
			}
			// A malformed recovered metadata/header rejects the entire block.
			if (count == 1)
			{
				auto broken = repairs.front();
				broken.data.c.reset(new uint8_t[broken.payload.size()]);
				std::copy(broken.payload.begin(), broken.payload.end(), broken.data.c.get());
				broken.payload = {broken.data.c.get(), broken.payload.size()};
				broken.payload[0] ^= 1;
				require(recover_video_packets(std::span(&broken, 1), absent).empty());
			}
		}
	// Encrypted sender schedules: short buffers, group transitions, every burst
	// position. An independent per-block erasure count determines recoverability.
	for (size_t count: {1, 7, 16, 17, 26, 32, 33, 63, 64})
		for (uint8_t r: {2, 4})
		{
			std::vector<std::vector<uint8_t>> storage(count, std::vector<uint8_t>(90, 51));
			std::vector<source_type> originals;
			for (size_t i = 0; i < count; ++i)
			{
				source_type s{};
				s.frame_idx = 111;
				s.shard_idx = i;
				s.payload = storage[i];
				originals.push_back(s);
			}
			std::vector<to_headset::packets> wire;
			auto emit = [&](auto packet) { wire.emplace_back(wire_roundtrip(std::move(packet))); };
			send_interleaved_video(originals, emit, emit, r);
			for (size_t burst = 1; burst <= 10 && burst <= wire.size(); ++burst)
				for (size_t start = 0; start + burst <= wire.size(); ++start)
				{
					std::vector<std::optional<source_type>> available(count);
					std::map<uint16_t, std::vector<repair_type>> blocks;
					for (size_t i = 0; i < wire.size(); ++i)
						if (auto repair = std::get_if<repair_type>(&wire[i]))
							blocks.try_emplace(repair->first_shard);
					for (size_t i = 0; i < wire.size(); ++i)
					{
						if (i >= start && i < start + burst)
							continue;
						if (auto s = std::get_if<source_type>(&wire[i]))
							available[s->shard_idx] = *s;
						else
						{
							const auto & repair = std::get<repair_type>(wire[i]);
							blocks[repair.first_shard].push_back(repair);
						}
					}
					for (const auto & [first, symbols]: blocks)
					{
						if (symbols.empty())
							continue;
						size_t holes = 0;
						for (size_t i = first; i < size_t(first) + symbols.front().source_count; ++i)
							holes += !available[i];
						auto recovered = recover_video_packets(symbols, available);
						require(recovered.size() == (holes <= symbols.size() ? holes : 0));
						for (const auto & packet: recovered)
							require(std::ranges::all_of(packet.payload, [](uint8_t b) { return b == 51; }));
					}
					++checked;
				}
		}
	// Maximal body with both metadata fields must fit the repair datagram.
	source_type s{};
	s.view_info.emplace();
	s.timing_info.emplace();
	std::vector<uint8_t> bytes(video_fec_symbol_capacity() - serialized_size(s), 37);
	s.payload = bytes;
	auto repairs = encode_video_rs(std::span(&s, 1), 4);
	for (const auto & repair: repairs)
		require(serialized_size(repair) + 1 == source_type::max_payload_size);
	std::cout << checked << " RS erasure/burst cases, GF oracle, metadata, encryption and wire bounds passed\n";
}
