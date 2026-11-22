// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_fec.h"

namespace wivrn
{
inline constexpr size_t video_fec_interleave_depth = 4;
inline constexpr size_t video_fec_interleave_window = video_fec_group_size * video_fec_interleave_depth;

// Reorder only an already encoded, bounded window. Source indices and the repair
// packet format stay unchanged; the receiver already accepts out-of-order
// source/repair packets. No window crosses an encoder output-buffer boundary.
template <typename SendSource, typename SendRepair>
void send_interleaved_video(std::span<const to_headset::video_stream_data_shard> sources,
                            SendSource && send_source,
                            SendRepair && send_repair)
{
	if (sources.empty())
		return;
	if (sources.size() > video_fec_interleave_window)
		throw std::runtime_error("Video FEC interleave window too large");
	for (size_t i = 0; i < sources.size(); ++i)
		if (sources[i].frame_idx != sources[0].frame_idx || sources[i].stream_item_idx != sources[0].stream_item_idx ||
		    size_t(sources[0].shard_idx) + i != sources[i].shard_idx)
			throw std::runtime_error("Nonconsecutive video FEC interleave sources");

	// Balance short windows: group lengths differ by at most one. Count remains
	// ceil(N/8), so interleaving adds no repair packets. Treat each group's repair
	// as its last symbol; this also separates a tail source from its own parity.
	const size_t groups = (sources.size() + video_fec_group_size - 1) / video_fec_group_size;
	std::array<size_t, video_fec_interleave_depth> offsets{}, counts{};
	std::array<to_headset::video_stream_repair_shard, video_fec_interleave_depth> repairs{};
	size_t offset = 0;
	for (size_t group = 0; group < groups; ++group)
	{
		offsets[group] = offset;
		counts[group] = sources.size() / groups + (group < sources.size() % groups);
		video_fec_encoder encoder;
		for (size_t i = 0; i < counts[group]; ++i)
			if (auto repair = encoder.push(sources[offset + i], i + 1 == counts[group]))
				repairs[group] = std::move(*repair);
		offset += counts[group];
	}
	// All parity must be captured before the first send: transport encryption
	// mutates the payload spans referenced by sources in place.
	for (size_t row = 0; row <= video_fec_group_size; ++row)
		for (size_t group = 0; group < groups; ++group)
			if (row < counts[group])
				send_source(sources[offsets[group] + row]);
			else if (row == counts[group])
				send_repair(std::move(repairs[group]));
}
} // namespace wivrn
