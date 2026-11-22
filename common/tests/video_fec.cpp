// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_fec.h"
#include "protocol_version.h"

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
	output.serialize(to_headset::packets{*repair});
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
