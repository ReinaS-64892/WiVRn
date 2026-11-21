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
#include <algorithm>
#include <cmath>

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
	deadline.reset();

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

std::optional<uint16_t> shard_set::insert(data_shard && shard, XrTime now, XrDuration max_wait, XrDuration reserve)
{
	// A source symbol must belong to the range advertised by its unit.
	const auto & unit = shard.unit;
	if (!valid_video_symbol(unit, shard.shard_idx, shard.payload.size()))
		return {};
	if (empty())
	{
		feedback.received_first_packet = now;
		deadline = now + max_wait;
	}
	if (shard.view_info && shard.view_info->display_time > 0)
	{
		const auto predicted_deadline = shard.view_info->display_time - reserve;
		// The server pacer starts without decoded/displayed timing samples.
		// An already-unusable prediction must not discard every fragment of the
		// first IDR and prevent the feedback needed to calibrate that pacer.
		if (predicted_deadline > feedback.received_first_packet)
			deadline = std::min(*deadline, predicted_deadline);
	}

	auto idx = shard.shard_idx;
	if (idx >= data.size())
		data.resize(idx + 1);
	if (data[idx])
		return {};
	data[idx] = std::move(shard);
	return idx;
}

shard_accumulator::statistics & shard_accumulator::statistics::operator+=(const statistics & other)
{
	complete += other.complete;
	incomplete += other.incomplete;
	partial += other.partial;
	dropped += other.dropped;
	expired += other.expired;
	late_packets += other.late_packets;
	unobserved_frames += other.unobserved_frames;
	wait_samples += other.wait_samples;
	wait_sum += other.wait_sum;
	wait_max = std::max(wait_max, other.wait_max);
	return *this;
}

XrDuration shard_accumulator::receive_budget(float frame_rate)
{
	if (!std::isfinite(frame_rate) || frame_rate <= 0)
		frame_rate = 90;
	return std::clamp<XrDuration>(1e9 / std::max(1.f, frame_rate), 2'000'000, 20'000'000);
}

void shard_accumulator::record_decode_time(XrDuration duration)
{
	if (duration <= 0)
		return;
	// Follow increases promptly; decay slowly so one fast picture cannot erase
	// the headroom measured on a slower hardware decode or its queue.
	const auto sample = std::clamp<XrDuration>(duration, 1'000'000, 16'000'000);
	auto previous = decode_budget.load(std::memory_order_relaxed);
	while (!decode_budget.compare_exchange_weak(previous, std::max(sample * 5 / 4, previous * 99 / 100), std::memory_order_relaxed))
	{
	}
}

shard_accumulator::statistics shard_accumulator::take_statistics()
{
	std::lock_guard lock(statistics_mutex);
	return std::exchange(pending_statistics, {});
}

std::optional<XrTime> shard_accumulator::next_deadline() const
{
	if (!current.deadline)
		return next.deadline;
	if (!next.deadline)
		return current.deadline;
	return std::min(*current.deadline, *next.deadline);
}

void shard_accumulator::expire(XrTime now)
{
	// There are at most two observed frames. Empty placeholders have no timer,
	// but must advance when an observed successor reaches its deadline.
	while (auto deadline = next_deadline())
	{
		if (now < *deadline)
			break;
		finish_current(true);
		advance();
		try_submit_frame();
	}
}

static void debug_incomplete_frame(const shard_set & shards)
{
	const auto & frame = shards.data;
	if (frame.empty())
	{
		spdlog::debug("frame {} has no received shards", shards.frame_index());
		return;
	}
	size_t data = 0;
	size_t missing = 0;
	for (const auto & shard: frame)
	{
		if (shard)
		{
			++data;
		}
		else
			++missing;
	}

	bool end = frame.back() and frame.back()->timing_info;
	spdlog::debug("frame {} is incomplete with {} data shards, {}{} missing", shards.frame_index(), data, end ? "" : "at least ", missing);
}

void shard_accumulator::advance()
{
	std::swap(current, next);
	next.reset(current.frame_index() + 1);
}

void shard_accumulator::push_shard(video_stream_data_shard && shard)
{
	assert(current.frame_index() + 1 == next.frame_index());
	if (!valid_video_symbol(shard.unit, shard.shard_idx, shard.payload.size()))
		return;
	if (!started)
	{
		current.reset(shard.frame_idx);
		next.reset(shard.frame_idx + 1);
		started = true;
	}
	const auto now = instance.now();
	expire(now);
	const auto reserve = decode_budget.load(std::memory_order_relaxed) + 2'000'000; // render/submit headroom

	auto frame_diff = shard.frame_idx - current.frame_index();
	if (shard.frame_idx < current.frame_index())
	{
		// frame is in the past, drop it
		std::lock_guard lock(statistics_mutex);
		++pending_statistics.late_packets;
	}
	else if (frame_diff == 0)
	{
		if (current.insert(std::move(shard), now, max_wait, reserve))
			try_submit_frame();
	}
	else if (frame_diff == 1)
	{
		next.insert(std::move(shard), now, max_wait, reserve);
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
		{
			std::lock_guard lock(statistics_mutex);
			pending_statistics.unobserved_frames += frame_diff - 2;
		}

		current.reset(shard.frame_idx);
		next.reset(shard.frame_idx + 1);

		push_shard(std::move(shard));
	}
	expire(now);
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

void shard_accumulator::finish_current(bool expired)
{
	const auto now = instance.now();
	if (current.empty())
	{
		{
			std::lock_guard lock(statistics_mutex);
			++pending_statistics.unobserved_frames;
		}
		send_feedback(current.feedback);
		return;
	}
	auto payload = complete_video_units(current.data);
	const bool complete = is_complete(current) && payload.spans.size() == current.data.size();
	if (!complete)
		debug_incomplete_frame(current);

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
		current.feedback.received_last_packet = now;

	const bool submit = view && !payload.spans.empty() && (complete || (allow_partial && payload.has_vcl));
	{
		std::lock_guard lock(statistics_mutex);
		pending_statistics.complete += complete;
		pending_statistics.incomplete += !complete;
		pending_statistics.partial += submit && !complete;
		pending_statistics.dropped += !submit;
		pending_statistics.expired += expired;
		const auto wait = std::max<XrDuration>(0, now - current.feedback.received_first_packet);
		++pending_statistics.wait_samples;
		pending_statistics.wait_sum += wait;
		pending_statistics.wait_max = std::max(pending_statistics.wait_max, wait);
	}
	if (submit)
	{
		current.feedback.sent_to_decoder = now;
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
