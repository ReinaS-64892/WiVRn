// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wivrn_packets.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace wivrn
{
inline constexpr uint8_t video_fec_group_size = 8;

inline bool valid_video_repair(const to_headset::video_stream_data_shard & source, uint8_t count)
{
	const auto & unit = source.unit;
	const size_t end = size_t(unit.first_shard) + unit.shard_count;
	if (!count || count > video_fec_group_size || !unit.shard_count || !unit.symbol_size ||
	    unit.symbol_size > to_headset::video_stream_data_shard::max_payload_size ||
	    source.payload.size() != unit.symbol_size || !unit.byte_count || end > UINT16_MAX ||
	    (uint64_t(unit.byte_count) + unit.symbol_size - 1) / unit.symbol_size != unit.shard_count ||
	    source.shard_idx < unit.first_shard || source.shard_idx >= end)
		return false;
	return (source.shard_idx - unit.first_shard) % video_fec_group_size == 0 &&
	       count == std::min<size_t>(video_fec_group_size, end - source.shard_idx) &&
	       (!source.timing_info || size_t(source.shard_idx) + count == end);
}

// Shared by all encoder backends, including opaque AV1 access units. Flush
// immediately at eight symbols or the NAL boundary; never wait across frames.
class video_parity_builder
{
	std::shared_ptr<uint8_t[]> bytes;
	uint8_t count = 0;
	uint16_t first = 0;

public:
	std::optional<to_headset::video_stream_repair_shard> push(const to_headset::video_stream_data_shard & source)
	{
		if (!count)
		{
			bytes.reset(new uint8_t[source.unit.symbol_size]{});
			first = source.shard_idx;
		}
		for (size_t i = 0; i < source.payload.size(); ++i)
			bytes[i] ^= source.payload[i];
		++count;
		if (count != video_fec_group_size && size_t(source.shard_idx) + 1 != size_t(source.unit.first_shard) + source.unit.shard_count)
			return {};
		to_headset::video_stream_repair_shard result{count, source};
		result.source.shard_idx = first;
		result.source.data.c = std::move(bytes);
		result.source.payload = {result.source.data.c.get(), source.unit.symbol_size};
		count = 0;
		return result;
	}
};

// Recover only a single absent source, with the unit descriptor independently
// verified on every operand. Missing tail bytes are truncated to byte_count.
inline std::optional<to_headset::video_stream_data_shard> recover_video_symbol(
        const to_headset::video_stream_repair_shard & repair,
        const std::vector<std::optional<to_headset::video_stream_data_shard>> & sources)
{
	const auto & source = repair.source;
	if (!valid_video_repair(source, repair.source_count))
		return {};
	std::optional<uint16_t> missing;
	const size_t end = size_t(source.shard_idx) + repair.source_count;
	for (size_t i = source.shard_idx; i < end; ++i)
	{
		if (i >= sources.size() || !sources[i])
		{
			if (missing)
				return {};
			missing = uint16_t(i);
		}
		else if (sources[i]->unit != source.unit || sources[i]->frame_idx != source.frame_idx ||
		         sources[i]->stream_item_idx != source.stream_item_idx || sources[i]->shard_idx != i ||
		         !valid_video_symbol(source.unit, i, sources[i]->payload.size()))
			return {};
	}
	if (!missing)
		return {};
	auto result = source;
	result.data.c.reset(new uint8_t[source.unit.symbol_size]);
	std::copy(source.payload.begin(), source.payload.end(), result.data.c.get());
	for (size_t i = source.shard_idx; i < end; ++i)
		if (i != *missing)
			for (size_t j = 0; j < sources[i]->payload.size(); ++j)
				result.data.c[j] ^= sources[i]->payload[j];
	result.shard_idx = *missing;
	const bool last = size_t(*missing) + 1 == size_t(source.unit.first_shard) + source.unit.shard_count;
	result.payload = {result.data.c.get(), last ? source.unit.byte_count - size_t(source.unit.shard_count - 1) * source.unit.symbol_size : source.unit.symbol_size};
	if (!last)
		result.timing_info.reset();
	return result;
}
} // namespace wivrn
