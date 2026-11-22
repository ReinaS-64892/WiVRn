// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace wivrn
{
// Prefer a synchronized frame near the prediction, but never undo progress
// made by an earlier per-stream fallback. Retain displayed images if caches
// have since wrapped and contain only older frames.
template <typename Frame, size_t Streams>
std::array<std::shared_ptr<Frame>, Streams> select_video_frames(
        const std::array<std::span<const std::shared_ptr<Frame>>, Streams> & caches,
        const std::array<std::shared_ptr<Frame>, Streams> & previous,
        size_t active_streams,
        int64_t display_time)
{
	std::array<std::shared_ptr<Frame>, Streams> result{};
	if (!active_streams || active_streams > Streams)
		return result;
	auto eligible = [&](const auto & frame, size_t stream) {
		return frame && (!previous[stream] || frame->feedback.frame_index >= previous[stream]->feedback.frame_index);
	};
	auto distance = [&](const auto & frame) {
		const auto time = frame->view_info.display_time;
		return time >= display_time ? uint64_t(time) - uint64_t(display_time) : uint64_t(display_time) - uint64_t(time);
	};
	auto find = [&](size_t stream, uint64_t id) -> const std::shared_ptr<Frame> * {
		for (const auto & frame: caches[stream])
			if (eligible(frame, stream) && frame->feedback.frame_index == id)
				return &frame;
		return nullptr;
	};
	const Frame * best = nullptr;
	uint64_t best_distance = std::numeric_limits<uint64_t>::max();
	for (const auto & frame: caches[0])
	{
		if (!eligible(frame, 0))
			continue;
		bool common = true;
		for (size_t stream = 1; stream < active_streams; ++stream)
			common &= find(stream, frame->feedback.frame_index) != nullptr;
		const auto delta = distance(frame);
		if (common && (!best || delta < best_distance || (delta == best_distance && frame->feedback.frame_index > best->feedback.frame_index)))
		{
			best = frame.get();
			best_distance = delta;
		}
	}
	if (best)
	{
		for (size_t stream = 0; stream < active_streams; ++stream)
			result[stream] = *find(stream, best->feedback.frame_index);
		return result;
	}
	for (size_t stream = 0; stream < active_streams; ++stream)
	{
		result[stream] = previous[stream];
		for (const auto & frame: caches[stream])
			if (eligible(frame, stream) && (!result[stream] || distance(frame) < distance(result[stream]) ||
			                                (distance(frame) == distance(result[stream]) && frame->feedback.frame_index > result[stream]->feedback.frame_index)))
				result[stream] = frame;
	}
	return result;
}
} // namespace wivrn
