// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_reed_solomon.h"

namespace wivrn
{
inline constexpr size_t video_fec_interleave_depth = 4;
inline constexpr size_t video_fec_interleave_window = video_fec_group_size * video_fec_interleave_depth;

// Reorder only already encoded data, without crossing a buffer, stream or frame.
// rs_repairs=0 selects XOR; otherwise it is the repair count per 16 sources.
template <typename SendSource, typename SendRepair>
void send_interleaved_video(std::span<const to_headset::video_stream_data_shard> sources,
                            SendSource && send_source,
                            SendRepair && send_repair,
                            uint8_t rs_repairs = 0)
{
	if (sources.empty())
		return;
	if (rs_repairs > video_rs_max_repairs)
		throw std::runtime_error("Invalid video RS repair count");
	const auto group_size = rs_repairs ? video_rs_group_size : video_fec_group_size;
	if (sources.size() > group_size * video_fec_interleave_depth)
		throw std::runtime_error("Video FEC interleave window too large");
	for (size_t i = 0; i < sources.size(); ++i)
		if (sources[i].frame_idx != sources[0].frame_idx || sources[i].stream_item_idx != sources[0].stream_item_idx ||
		    size_t(sources[0].shard_idx) + i != sources[i].shard_idx)
			throw std::runtime_error("Nonconsecutive video FEC interleave sources");

	// Balance short windows and round up their proportional repair count.
	// Include repairs in the round-robin schedule to separate tail symbols.
	const size_t groups = (sources.size() + group_size - 1) / group_size;
	std::array<size_t, video_fec_interleave_depth> offsets{}, counts{};
	std::array<std::vector<to_headset::video_stream_repair_shard>, video_fec_interleave_depth> repairs{};
	size_t offset = 0;
	for (size_t group = 0; group < groups; ++group)
	{
		offsets[group] = offset;
		counts[group] = sources.size() / groups + (group < sources.size() % groups);
		if (rs_repairs)
		{
			const auto count = (counts[group] * rs_repairs + video_rs_group_size - 1) / video_rs_group_size;
			repairs[group] = encode_video_rs(sources.subspan(offset, counts[group]), count);
		}
		else
		{
			video_fec_encoder encoder;
			for (size_t i = 0; i < counts[group]; ++i)
				if (auto repair = encoder.push(sources[offset + i], i + 1 == counts[group]))
					repairs[group].push_back(std::move(*repair));
		}
		offset += counts[group];
	}
	// All parity must be captured before the first send: transport encryption
	// mutates the payload spans referenced by sources in place.
	for (size_t row = 0; row < group_size + (rs_repairs ? rs_repairs : 1); ++row)
		for (size_t group = 0; group < groups; ++group)
			if (row < counts[group])
				send_source(sources[offsets[group] + row]);
			else if (row - counts[group] < repairs[group].size())
				send_repair(std::move(repairs[group][row - counts[group]]));
}
} // namespace wivrn
