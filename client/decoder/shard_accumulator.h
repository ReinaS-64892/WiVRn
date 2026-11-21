/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
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

#pragma once

#include "decoder.h"
#include "receive_time_budget.h"
#include "wivrn_packets.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

namespace xr
{
class instance;
}

namespace scenes
{
class stream;
}

namespace wivrn
{

class shard_accumulator
{
	std::shared_ptr<decoder> decoder_;
	bool started = false;

public:
	using data_shard = wivrn::to_headset::video_stream_data_shard;
	struct shard_set
	{
		std::vector<std::optional<data_shard>> data;
		std::optional<XrTime> deadline;
		bool reliable = false;
		XrTime assembled_at = 0;
		XrDuration receive_limit = 0;
		void reset(uint64_t frame_index);
		bool empty() const;

		std::optional<uint16_t> insert(data_shard &&, XrTime now, XrDuration max_wait, XrDuration reserve, bool reliable);

		wivrn::from_headset::feedback feedback{};

		explicit shard_set(uint8_t stream_index);
		shard_set(const shard_set &) = default;
		shard_set(shard_set &&) = default;
		shard_set & operator=(const shard_set &) = default;
		shard_set & operator=(shard_set &&) = default;

		uint64_t frame_index() const
		{
			return feedback.frame_index;
		}
	};

	// Receiver decisions, including frames that never produce a decoded image.
	struct statistics
	{
		uint64_t complete = 0;
		uint64_t incomplete = 0;
		uint64_t partial = 0;
		uint64_t dropped = 0;
		uint64_t expired = 0;
		uint64_t late_packets = 0;
		uint64_t unobserved_frames = 0;
		uint64_t wait_samples = 0;
		XrDuration wait_sum = 0;
		XrDuration wait_max = 0;
		uint64_t normal_wait_samples = 0;
		XrDuration normal_wait_sum = 0;
		XrDuration normal_wait_max = 0;
		XrDuration reliable_wait_max = 0;
		uint64_t no_slice = 0;
		uint64_t partial_unsupported = 0;
		std::array<uint64_t, 5> deadline_slice_frames{}; // 0, 1, 2, 3, 4+ intact VCL NALs
		uint64_t deadline_intact_slices = 0;
		uint64_t deadline_damaged_slices = 0; // Lower bound: wholly unseen NALs are unknown.
		statistics & operator+=(const statistics &);
	};

private:
	shard_set current;
	shard_set next;
	std::weak_ptr<scenes::stream> weak_scene;
	xr::instance & instance;
	const bool allow_partial;
	const XrDuration max_wait;
	receive_time_budget assembly_budget;
	std::atomic<XrDuration> published_receive_budget;
	std::atomic<XrDuration> decode_budget = 4'000'000;
	std::mutex statistics_mutex;
	statistics pending_statistics;
	struct retired_frame
	{
		uint64_t frame = 0;
		XrTime first_packet = 0;
		bool adaptive_expiry = false;
	};
	std::array<retired_frame, 64> retired_frames{};

public:
	explicit shard_accumulator(
	        vk::raii::Device & device,
	        vk::raii::PhysicalDevice & physical_device,
	        xr::instance & instance,
	        uint32_t vk_queue_family_index,
	        const wivrn::to_headset::video_stream_description & description,
	        std::weak_ptr<scenes::stream> scene,
	        uint8_t stream_index) :
	        decoder_(decoder::make(device, physical_device, vk_queue_family_index, description, stream_index, scene, this)),
	        current(stream_index),
	        next(stream_index),
	        weak_scene(scene),
	        instance(instance),
	        allow_partial(description.codec[stream_index] == h264 || description.codec[stream_index] == h265),
	        max_wait(receive_budget(description.frame_rate)),
	        assembly_budget(max_wait),
	        published_receive_budget(0)
	{
		next.reset(1);
	}

	void push_shard(wivrn::to_headset::video_stream_data_shard &&, bool reliable = false);
	XrDuration current_receive_budget() const
	{
		return published_receive_budget.load(std::memory_order_relaxed);
	}
	// Called on the network thread, even when no packets arrive.
	void expire(XrTime now);
	std::optional<XrTime> next_deadline() const;
	// Decoder callback and render thread respectively; both may run concurrently.
	void record_decode_time(XrDuration duration);
	statistics take_statistics();

	vk::Sampler sampler()
	{
		return decoder_->sampler();
	}
	bool needs_manual_linear_filtering() const
	{
		return decoder_->needs_manual_linear_filtering();
	}

	using blit_handle = decoder::blit_handle;

private:
	void try_submit_frame();
	void send_feedback(wivrn::from_headset::feedback & feedback);
	void advance();
	void finish_current(bool expired = false);
	static XrDuration receive_budget(float frame_rate);
};
} // namespace wivrn
