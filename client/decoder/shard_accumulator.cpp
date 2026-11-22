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
#include "video_diagnostics.h"
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
	reliable = false;
	assembled_at = 0;
	receive_limit = 0;
	hard_deadline.reset();
	grace_used = false;

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

std::optional<uint16_t> shard_set::insert(data_shard && shard, XrTime now, XrDuration max_wait, XrDuration reserve, bool reliable_transport, XrDuration hard_wait)
{
	// A source symbol must belong to the range advertised by its unit.
	const auto & unit = shard.unit;
	if (!valid_video_symbol(unit, shard.shard_idx, shard.payload.size()))
		return {};
	const bool first = empty();
	if (first)
	{
		feedback.received_first_packet = now;
		receive_limit = max_wait;
		deadline = now + max_wait;
		hard_deadline = now + (hard_wait > 0 ? hard_wait : max_wait);
	}
	if (reliable_transport && !reliable)
	{
		// Recovery IDRs and parameter sets travel over control/TCP. Cutting
		// those at the short UDP loss timeout can prevent decoder startup.
		reliable = true;
		deadline = feedback.received_first_packet + 100'000'000;
		hard_deadline = deadline;
	}
	if (!reliable && shard.view_info && shard.view_info->display_time > 0)
	{
		const auto predicted_deadline = shard.view_info->display_time - reserve;
		// The server pacer starts without decoded/displayed timing samples.
		// An already-unusable prediction must not discard every fragment of the
		// first IDR and prevent the feedback needed to calibrate that pacer.
		if (predicted_deadline > feedback.received_first_packet)
		{
			hard_deadline = std::min(*hard_deadline, predicted_deadline);
			deadline = std::min(*deadline, *hard_deadline);
		}
	}
	if (first)
		video_diagnostics::get().record({.kind = "frame_first_packet", .xr_ns = now, .frame = shard.frame_idx, .stream = shard.stream_item_idx, .deadline_ns = *deadline, .display_ns = shard.view_info ? shard.view_info->display_time : 0, .budget_ns = reliable ? 100'000'000 : max_wait, .reliable = reliable});

	auto idx = shard.shard_idx;
	if (idx >= data.size())
		data.resize(idx + 1);
	if (data[idx])
		return {};
	data[idx] = std::move(shard);
	if (is_complete(*this))
		assembled_at = now;
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
	normal_wait_samples += other.normal_wait_samples;
	normal_wait_sum += other.normal_wait_sum;
	normal_wait_max = std::max(normal_wait_max, other.normal_wait_max);
	reliable_wait_max = std::max(reliable_wait_max, other.reliable_wait_max);
	no_slice += other.no_slice;
	partial_unsupported += other.partial_unsupported;
	for (size_t i = 0; i < deadline_slice_frames.size(); ++i)
		deadline_slice_frames[i] += other.deadline_slice_frames[i];
	deadline_intact_slices += other.deadline_intact_slices;
	deadline_damaged_slices += other.deadline_damaged_slices;
	grace_frames += other.grace_frames;
	grace_completed += other.grace_completed;
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
		// Both slots can have reached their soft cutoff. Extend only once,
		// anchored to the original limit, never relative to a late wakeup.
		const bool extended_current = extend_for_late_symbols(current, now);
		const bool extended_next = extend_for_late_symbols(next, now);
		if (extended_current || extended_next)
			continue;
		finish_current(true);
		advance();
		try_submit_frame();
	}
}

bool shard_accumulator::extend_for_late_symbols(shard_set & frame, XrTime now)
{
	const auto grace = std::clamp<XrDuration>(decoder_->incomplete_frame_grace(), 0, 2'000'000);
	if (!grace || frame.reliable || frame.grace_used || !frame.deadline || now < *frame.deadline ||
	    *frame.deadline != frame.feedback.received_first_packet + frame.receive_limit || !frame.hard_deadline)
		return false;
	const auto extended = std::min(*frame.deadline + grace, *frame.hard_deadline);
	if (extended <= now)
		return false;
	frame.grace_used = true;
	frame.deadline = extended;
	{
		std::lock_guard lock(statistics_mutex);
		++pending_statistics.grace_frames;
	}
	video_diagnostics::get().record({.kind = "receive_grace", .detail = fmt::format("hard_deadline_ns={}", *frame.hard_deadline), .xr_ns = now, .frame = frame.frame_index(), .stream = frame.feedback.stream_index, .first_ns = frame.feedback.received_first_packet, .deadline_ns = extended, .budget_ns = frame.receive_limit});
	return true;
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

void shard_accumulator::push_shard(video_stream_data_shard && shard, bool reliable)
{
	assert(current.frame_index() + 1 == next.frame_index());
	if (!valid_video_symbol(shard.unit, shard.shard_idx, shard.payload.size()))
		return;
	if (!started)
	{
		current.reset(shard.frame_idx);
		next.reset(shard.frame_idx + 1);
		started = true;
		published_receive_budget.store(assembly_budget.value(), std::memory_order_relaxed);
	}
	const auto now = instance.now();
	expire(now);
	const auto reserve = decode_budget.load(std::memory_order_relaxed) + 2'000'000; // render/submit headroom

	auto frame_diff = shard.frame_idx - current.frame_index();
	if (shard.frame_idx < current.frame_index())
	{
		const auto & retired = retired_frames[shard.frame_idx % retired_frames.size()];
		if (!reliable && retired.adaptive_expiry && retired.frame == shard.frame_idx)
		{
			assembly_budget.late(std::max<XrDuration>(0, now - retired.first_packet), now);
			published_receive_budget.store(assembly_budget.value(), std::memory_order_relaxed);
		}
		// frame is in the past, drop it
		std::lock_guard lock(statistics_mutex);
		++pending_statistics.late_packets;
		video_diagnostics::get().record({.kind = "late_packet", .xr_ns = now, .frame = shard.frame_idx, .stream = shard.stream_item_idx, .received = shard.shard_idx, .reliable = reliable});
	}
	else if (frame_diff == 0)
	{
		if (current.insert(std::move(shard), now, assembly_budget.value(), reserve, reliable, max_wait))
			try_submit_frame();
	}
	else if (frame_diff == 1)
	{
		next.insert(std::move(shard), now, assembly_budget.value(), reserve, reliable, max_wait);
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
		push_shard(std::move(shard), reliable);
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

		push_shard(std::move(shard), reliable);
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
		current.feedback.received_last_packet = current.assembled_at;

	const bool submit = view && !payload.spans.empty() && (complete || (allow_partial && payload.has_vcl));
	const auto wait = std::max<XrDuration>(0, now - current.feedback.received_first_packet);
	const auto budget = assembly_budget.value();
	if (!current.reliable)
	{
		retired_frames[current.frame_index() % retired_frames.size()] = {
		        .frame = current.frame_index(),
		        .first_packet = current.feedback.received_first_packet,
		        .adaptive_expiry = !complete && expired && (current.grace_used || current.deadline == current.feedback.received_first_packet + current.receive_limit),
		};
		if (complete)
		{
			assembly_budget.complete(std::max<XrDuration>(0, current.assembled_at - current.feedback.received_first_packet), now);
			if (current.grace_used)
				assembly_budget.late(std::max<XrDuration>(0, current.assembled_at - current.feedback.received_first_packet), now);
		}
		else
			assembly_budget.incomplete(now);
		published_receive_budget.store(assembly_budget.value(), std::memory_order_relaxed);
	}
	// Count each advertised VCL unit once, even if its first source symbol was
	// lost. Entirely unseen units cannot be inferred from a source descriptor.
	uint64_t observed_vcl = 0;
	std::vector<bool> observed_units(current.data.size());
	for (const auto & shard: current.data)
		if (shard && !observed_units[shard->unit.first_shard])
		{
			observed_units[shard->unit.first_shard] = true;
			observed_vcl += shard->unit.vcl;
		}
	const auto damaged_vcl = observed_vcl - payload.vcl_count;
	if (video_diagnostics::get().active())
	{
		uint64_t received = 0, expected = 0;
		for (const auto & shard: current.data)
			if (shard)
			{
				++received;
				expected = std::max<uint64_t>(expected, uint64_t(shard->unit.first_shard) + shard->unit.shard_count);
			}
		video_diagnostics::get().record({.kind = "frame", .detail = complete ? "complete" : !view      ? "no_view"
		                                                                            : !allow_partial   ? "partial_unsupported"
		                                                                            : !payload.has_vcl ? "no_intact_slice"
		                                                                                               : "partial",
		                                 .xr_ns = now,
		                                 .frame = current.frame_index(),
		                                 .stream = current.feedback.stream_index,
		                                 .first_ns = current.feedback.received_first_packet,
		                                 .deadline_ns = current.deadline.value_or(0),
		                                 .display_ns = view ? view->display_time : 0,
		                                 .wait_ns = wait,
		                                 .budget_ns = budget,
		                                 .received = received,
		                                 .expected_min = expected,
		                                 .units = payload.unit_count,
		                                 .vcl = payload.vcl_count,
		                                 .vcl_observed = observed_vcl,
		                                 .vcl_damaged = damaged_vcl,
		                                 .reliable = current.reliable,
		                                 .complete = complete,
		                                 .submitted = submit,
		                                 .expired = expired});
	}
	{
		std::lock_guard lock(statistics_mutex);
		pending_statistics.complete += complete;
		pending_statistics.incomplete += !complete;
		pending_statistics.partial += submit && !complete;
		pending_statistics.dropped += !submit;
		pending_statistics.expired += expired;
		pending_statistics.grace_completed += complete && current.grace_used && !current.reliable;
		++pending_statistics.wait_samples;
		pending_statistics.wait_sum += wait;
		pending_statistics.wait_max = std::max(pending_statistics.wait_max, wait);
		if (current.reliable)
			pending_statistics.reliable_wait_max = std::max(pending_statistics.reliable_wait_max, wait);
		else
		{
			++pending_statistics.normal_wait_samples;
			pending_statistics.normal_wait_sum += wait;
			pending_statistics.normal_wait_max = std::max(pending_statistics.normal_wait_max, wait);
		}
		pending_statistics.no_slice += !complete && allow_partial && !payload.has_vcl;
		pending_statistics.partial_unsupported += !complete && !allow_partial;
		if (expired && !complete && allow_partial)
		{
			++pending_statistics.deadline_slice_frames[std::min<size_t>(payload.vcl_count, 4)];
			pending_statistics.deadline_intact_slices += payload.vcl_count;
			pending_statistics.deadline_damaged_slices += damaged_vcl;
		}
	}
	if (complete && current.grace_used && !current.reliable)
		video_diagnostics::get().record({.kind = "grace_completed", .xr_ns = now, .frame = current.frame_index(), .stream = current.feedback.stream_index, .wait_ns = wait});
	if (submit)
	{
		current.feedback.sent_to_decoder = now;
		decoder_->push_data(payload.spans, current.frame_index(), false);
		decoder_->frame_completed(current.feedback, *view);
		video_diagnostics::get().record({.kind = "decoder_submit_complete", .xr_ns = instance.now(), .frame = current.frame_index(), .stream = current.feedback.stream_index});
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
