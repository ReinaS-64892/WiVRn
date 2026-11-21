/*
 * WiVRn VR streaming
 * Copyright (C) 2022-2023  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022-2023  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "shard_accumulator.h"
#include "scenes/stream.h" // IWYU pragma: keep
#include "spdlog/spdlog.h"
#include "xr/instance.h"

namespace wivrn
{

using namespace wivrn::to_headset;
using shard_set = shard_accumulator::shard_set;
using data_shard = shard_accumulator::data_shard;

shard_set::shard_set(uint8_t stream_index)
{
	feedback.stream_index = stream_index;
}

void shard_set::reset(uint64_t frame_index)
{
	data.clear();

	uint8_t stream_index = feedback.stream_index;
	feedback = {};
	feedback.frame_index = frame_index;
	feedback.stream_index = stream_index;
}

bool shard_set::empty() const
{
	return data.empty();
}

static bool is_complete(const shard_set & shards)
{
	const auto & frame = shards.data;
	if (frame.empty())
		return false;
	if (not(frame.back() and frame.back()->timing_info))
		return false;
	for (const auto & shard: frame)
		if (not shard)
			return false;
	return true;
}

std::optional<uint16_t> shard_set::insert(data_shard && shard, xr::instance & instance)
{
	// A source symbol must belong to the range advertised by its unit.
	const auto & unit = shard.unit;
	if (!valid_video_symbol(unit, shard.shard_idx, shard.payload.size()))
		return {};
	if (empty())
		feedback.received_first_packet = instance.now();

	auto idx = shard.shard_idx;
	if (idx >= data.size())
		data.resize(idx + 1);
	if (data[idx])
		return {};
	data[idx] = std::move(shard);
	return idx;
}

static void debug_why_not_sent(const shard_set & shards)
{
	const auto & frame = shards.data;
	if (frame.empty())
	{
		spdlog::info("frame {} was not sent because no shard was received", shards.frame_index());
		return;
	}
	int frame_idx = -1;
	size_t data = 0;
	size_t missing = 0;
	for (const auto & shard: frame)
	{
		if (shard)
		{
			frame_idx = shard->frame_idx;
			++data;
		}
		else
			++missing;
	}

	bool end = frame.back() and frame.back()->timing_info;
	spdlog::info("frame {} was not sent with {} data shards, {}{} missing", frame_idx, data, end ? "" : "at least ", missing);
}

void shard_accumulator::advance()
{
	std::swap(current, next);
	next.reset(current.frame_index() + 1);
}

void shard_accumulator::push_shard(video_stream_data_shard && shard)
{
	assert(current.frame_index() + 1 == next.frame_index());

	auto frame_diff = shard.frame_idx - current.frame_index();
	if (shard.frame_idx < current.frame_index())
	{
		// frame is in the past, drop it
		spdlog::info("Drop shard for old frame {} (current {})", shard.frame_idx, current.frame_index());
	}
	else if (frame_diff == 0)
	{
		if (current.insert(std::move(shard), instance))
			try_submit_frame();
	}
	else if (frame_diff == 1)
	{
		next.insert(std::move(shard), instance);
		if (is_complete(next))
		{
			finish_current();
			advance();
			try_submit_frame();
		}
	}
	else if (frame_diff == 2)
	{
		finish_current();
		advance();
		try_submit_frame();
		push_shard(std::move(shard));
	}
	else
	{
		// We have lost more than one frame
		finish_current();
		advance();
		finish_current();

		current.reset(shard.frame_idx);
		next.reset(shard.frame_idx + 1);

		push_shard(std::move(shard));
	}
}

void shard_accumulator::try_submit_frame()
{
	// Submit each access unit exactly once; never leave a truncated NAL buffered
	// in a decoder while waiting for a lost UDP packet.
	if (!is_complete(current))
		return;
	finish_current();
	advance();
	if (is_complete(current))
	{
		finish_current();
		advance();
	}
}

void shard_accumulator::finish_current()
{
	auto payload = complete_video_units(current.data);
	const bool complete = is_complete(current) && payload.spans.size() == current.data.size();
	if (!complete)
		debug_why_not_sent(current);

	std::optional<data_shard::view_info_t> view;
	for (const auto & shard: current.data)
	{
		if (!shard)
			continue;
		if (shard->view_info)
			view = shard->view_info;
		if (shard->timing_info)
		{
			const auto & timing = *shard->timing_info;
			current.feedback.encode_begin = timing.encode_begin;
			current.feedback.encode_end = timing.encode_end;
			current.feedback.send_begin = timing.send_begin;
			current.feedback.send_end = timing.send_end;
		}
	}
	current.feedback.stream_corrupted = !complete;
	if (complete)
		current.feedback.received_last_packet = instance.now();

	if (view && !payload.spans.empty() && (complete || (allow_partial && payload.has_vcl)))
	{
		current.feedback.sent_to_decoder = instance.now();
		decoder_->push_data(payload.spans, current.frame_index(), false);
		decoder_->frame_completed(current.feedback, *view);
	}
	send_feedback(current.feedback);
}

void shard_accumulator::send_feedback(wivrn::from_headset::feedback & feedback)
{
	if (not feedback.received_first_packet)
		feedback.received_first_packet = instance.now();
	auto scene = weak_scene.lock();
	if (scene)
		scene->send_feedback(feedback);
}
} // namespace wivrn
