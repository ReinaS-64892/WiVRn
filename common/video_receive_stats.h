// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace wivrn
{
// Always-on receiver observations. Unseen frames may have been skipped by the
// sender, so they must not be counted as incomplete frames or missing packets.
class video_receive_stats
{
public:
	struct snapshot
	{
		uint64_t received_packets = 0;
		uint64_t late_packets = 0;
		uint64_t buffered_duplicates = 0;
		uint64_t out_of_order_packets = 0;
		uint64_t completed_frames = 0;
		uint64_t incomplete_frames = 0;
		uint64_t missing_packets_min = 0;
		uint64_t frames_with_unknown_total = 0;

		uint64_t finalized_frames() const
		{
			return completed_frames + incomplete_frames;
		}
		double incomplete_percent() const
		{
			return finalized_frames() ? 100.0 * incomplete_frames / finalized_frames() : 0;
		}
	};

private:
	mutable std::mutex mutex;
	snapshot totals;
	std::pair<uint64_t, uint16_t> high_water{};

public:
	void packet(uint64_t frame, uint16_t shard, bool late, bool buffered_duplicate)
	{
		std::lock_guard lock(mutex);
		++totals.received_packets;
		totals.late_packets += late;
		totals.buffered_duplicates += buffered_duplicate;
		auto key = std::pair(frame, shard);
		totals.out_of_order_packets += key < high_water;
		high_water = std::max(high_water, key);
	}

	// Call exactly once when a received frame is completed or abandoned.
	// slots is max observed shard index + 1; received excludes duplicates.
	void finish_frame(size_t slots, size_t received, bool terminal_received, bool complete)
	{
		if (received == 0)
			return;
		std::lock_guard lock(mutex);
		if (complete)
			++totals.completed_frames;
		else
		{
			++totals.incomplete_frames;
			// Without a terminal shard, at least one additional shard is missing
			// beyond the highest observed index. Its full tail length is unknown.
			totals.missing_packets_min += slots - received + !terminal_received;
			totals.frames_with_unknown_total += !terminal_received;
		}
	}

	snapshot read() const
	{
		std::lock_guard lock(mutex);
		return totals;
	}
};
} // namespace wivrn
