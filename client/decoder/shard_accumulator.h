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
#include "video_packet_audit.h"
#include "video_packet_recording.h"
#include "video_receive_stats.h"
#include "wivrn_packets.h"

#include <memory>
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
	using data_shard = wivrn::to_headset::video_stream_data_shard;
	struct shard_set
	{
		size_t min_for_reconstruction = -1;
		std::vector<std::optional<data_shard>> data;
		void reset(uint64_t frame_index);
		bool empty() const;

		std::optional<uint16_t> insert(data_shard &&, xr::instance & instance);

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
	shard_set current;
	shard_set next;
	std::weak_ptr<scenes::stream> weak_scene;
	xr::instance & instance;
	video_packet_audit packet_audit;
	video_receive_stats receive_stats;
	video_packet_recording file_recording;

public:
	explicit shard_accumulator(
	        vk::raii::Device & device,
	        vk::raii::PhysicalDevice & physical_device,
	        xr::instance & instance,
	        uint32_t vk_queue_family_index,
	        const wivrn::to_headset::video_stream_description & description,
	        std::weak_ptr<scenes::stream> scene,
	        uint8_t stream_index);

	void push_shard(wivrn::to_headset::video_stream_data_shard &&);
	void start_packet_recording(std::shared_ptr<packet_audit_file>);
	void stop_packet_recording();

	video_receive_stats::snapshot reception_metrics() const
	{
		return receive_stats.read();
	}

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
	void push_shard_impl(wivrn::to_headset::video_stream_data_shard &&);
	void try_submit_frame(std::optional<uint16_t> shard_idx);
	void try_submit_frame(uint16_t shard_idx);
	void send_feedback(wivrn::from_headset::feedback & feedback);
	void record_frame(const shard_set &, bool complete);
	void audit_retire_before(uint64_t index);
	void advance(bool complete = false);
};
} // namespace wivrn
