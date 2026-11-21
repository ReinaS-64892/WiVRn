// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wivrn
{
// One Annex B NAL, or one opaque access unit for codecs without NAL framing.
// Repeated on every source shard so losing its first packet loses no boundaries.
struct video_unit_info
{
	uint16_t unit_idx;
	uint16_t first_shard;
	uint16_t shard_count;
	uint32_t byte_count;
	uint16_t symbol_size;
	bool vcl;
	bool operator==(const video_unit_info &) const = default;
};

inline bool valid_video_symbol(const video_unit_info & unit, size_t index, size_t bytes)
{
	const size_t end = size_t(unit.first_shard) + unit.shard_count;
	if (!unit.shard_count || !unit.symbol_size || !unit.byte_count || end > UINT16_MAX ||
	    index < unit.first_shard || index >= end ||
	    (uint64_t(unit.byte_count) + unit.symbol_size - 1) / unit.symbol_size != unit.shard_count)
		return false;
	const size_t expected = index + 1 == end ? unit.byte_count - size_t(unit.shard_count - 1) * unit.symbol_size : unit.symbol_size;
	return bytes == expected;
}

// Preserve the start codes, including any leading zero bytes. Never scan raw/AV1.
inline std::vector<std::span<uint8_t>> annex_b_units(std::span<uint8_t> data)
{
	std::vector<std::span<uint8_t>> result;
	size_t begin = 0;
	bool found = false;
	for (size_t i = 0; i + 3 <= data.size(); ++i)
	{
		size_t prefix = 0;
		if (data[i] == 0 && data[i + 1] == 0)
		{
			if (data[i + 2] == 1)
				prefix = 3;
			else if (i + 4 <= data.size() && data[i + 2] == 0 && data[i + 3] == 1)
				prefix = 4;
		}
		if (!prefix)
			continue;
		if (found)
		{
			result.push_back(data.subspan(begin, i - begin));
			begin = i;
		}
		found = true;
		i += prefix - 1;
	}
	if (!data.empty())
		result.push_back(data.subspan(begin));
	return result;
}

inline size_t annex_b_prefix(std::span<const uint8_t> data)
{
	for (size_t i = 2; i < data.size(); ++i)
	{
		if (data[i] == 1)
			return i + 1;
		if (data[i] != 0)
			break;
	}
	return data.size();
}

struct video_unit_payload
{
	std::vector<std::span<const uint8_t>> spans;
	bool has_vcl = false;
};

// Select only whole, consistently described units. No bytes from a damaged NAL
// may enter a decoder. Shards are indexed by their frame-wide source index.
template <typename Shard>
video_unit_payload complete_video_units(const std::vector<std::optional<Shard>> & shards)
{
	video_unit_payload result;
	for (size_t i = 0; i < shards.size();)
	{
		if (!shards[i])
		{
			++i;
			continue;
		}
		const auto & unit = shards[i]->unit;
		const size_t end = size_t(unit.first_shard) + unit.shard_count;
		if (!unit.shard_count || unit.first_shard != i || end > shards.size())
		{
			++i;
			continue;
		}
		size_t bytes = 0;
		bool complete = true;
		for (size_t j = i; j < end; ++j)
		{
			if (!shards[j] || shards[j]->unit != unit || !valid_video_symbol(unit, j, shards[j]->payload.size()))
			{
				complete = false;
				break;
			}
			bytes += shards[j]->payload.size();
		}
		if (complete && bytes == unit.byte_count)
		{
			for (size_t j = i; j < end; ++j)
				result.spans.emplace_back(shards[j]->payload);
			result.has_vcl |= unit.vcl;
			i = end;
		}
		else
			++i;
	}
	return result;
}
} // namespace wivrn
