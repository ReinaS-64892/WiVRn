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
#include "video_fec_diagnostics.h"
#include "wivrn_packets.h"

#include <deque>
#include <map>
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

public:
	// Counts are per encoded stream (eyes and alpha are independent).
	struct statistics
	{
		uint64_t source_packets = 0;
		uint64_t repair_packets = 0;
		uint64_t recovered_packets = 0;
		uint64_t completed_frames = 0;
		uint64_t rescued_frames = 0;
		uint64_t incomplete_frames = 0;

		statistics & operator+=(const statistics & other)
		{
			source_packets += other.source_packets;
			repair_packets += other.repair_packets;
			recovered_packets += other.recovered_packets;
			completed_frames += other.completed_frames;
			rescued_frames += other.rescued_frames;
			incomplete_frames += other.incomplete_frames;
			return *this;
		}
	};
	// Drain interval counters safely from the render thread.
	statistics take_statistics();

	using data_shard = wivrn::to_headset::video_stream_data_shard;
	struct shard_set
	{
		struct diagnostic_timing
		{
			uint64_t generation = 0;
			int64_t first_ns = 0, last_ns = 0, max_gap_ns = 0;
			uint64_t sources = 0, repairs = 0, duplicate_sources = 0, reordered_sources = 0, payload_bytes = 0;
			std::optional<uint16_t> previous_source;
			bool partial_capture = false;
		} diagnostics;
		uint64_t recovered_packets = 0;
		std::map<uint16_t, std::vector<to_headset::video_stream_repair_shard>> repairs;
		std::vector<std::optional<data_shard>> data;
		void reset(uint64_t frame_index);
		bool empty() const;

		std::optional<uint16_t> insert(data_shard &&, xr::instance & instance);
		std::optional<uint16_t> insert(to_headset::video_stream_repair_shard &&, xr::instance & instance);
		std::optional<uint16_t> recover(uint16_t first);

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

private:
	const uint64_t diagnostics_id = video_fec_diagnostics::next_receiver_id();
	struct retired_frame
	{
		uint64_t frame, generation;
		int64_t retired_ns;
		bool complete;
	};
	std::deque<retired_frame> recently_damaged;
	void observe_packet(shard_set &, bool repair, uint16_t index, size_t bytes);
	void record_retirement(const shard_set &, bool complete, const char * reason);
	void record_late(uint64_t frame, bool repair, uint16_t index, uint8_t count, uint8_t scheme = 0, uint8_t repair_index = 0, uint8_t repair_count = 0);

	std::mutex statistics_mutex;
	statistics pending_statistics;
	shard_set current;
	shard_set next;
	std::weak_ptr<scenes::stream> weak_scene;
	xr::instance & instance;

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
	        instance(instance)
	{
		next.reset(1);
	}

	void push_shard(wivrn::to_headset::video_stream_data_shard &&);
	void push_repair(to_headset::video_stream_repair_shard &&);

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
	shard_set * select_frame(uint64_t frame);
	void inserted(shard_set &, std::optional<uint16_t>);
	void try_submit_frame(std::optional<uint16_t> shard_idx);
	void try_submit_frame(uint16_t shard_idx);
	void send_feedback(shard_set & shards, bool completed = false, const char * reason = "window_advanced");
	void advance();
};
} // namespace wivrn
