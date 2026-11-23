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

// Include first because of incompatibility between Eigen and X includes
#include "driver/wivrn_session.h"

#include "video_encoder.h"
#include "video_fec_interleave.h"
#include "video_fec_repair_queue.h"

#include "encoder_settings.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "utils/wivrn_trace.h"
#include "wivrn_config.h"

#include <charconv>
#include <string>
#include <string_view>

#if WIVRN_USE_NVENC
#include "video_encoder_nvenc.h"
#endif
#if WIVRN_USE_VAAPI
#include "ffmpeg/video_encoder_va.h"
#endif
#if WIVRN_USE_X264
#include "video_encoder_x264.h"
#endif
#if WIVRN_USE_VULKAN_ENCODE
#include "video_encoder_vulkan_av1.h"
#include "video_encoder_vulkan_h264.h"
#include "video_encoder_vulkan_h265.h"
#endif
#include "video_encoder_raw.h"

namespace wivrn
{

video_encoder::sender::sender() :
        thread([this](std::stop_token t) {
	        while (not t.stop_requested())
	        {
		        data * d = nullptr;
		        {
			        std::unique_lock lock(mutex);
			        if (pending.empty())
				        cv.wait_for(lock, std::chrono::milliseconds(100));
			        else
				        d = &pending.front();
		        }
		        if (d and not d->span.empty())
		        {
			        d->encoder->SendData(d->span, true, d->prefer_control);
			        std::unique_lock lock(mutex);
			        pending.pop_front();
			        cv.notify_all();
		        }
	        }
	        std::unique_lock lock(mutex);
	        pending.clear();
	        cv.notify_all();
        })
{
}

void video_encoder::sender::push(data && d)
{
	std::unique_lock lock(mutex);
	pending.push_back(std::move(d));
	cv.notify_all();
}

void video_encoder::sender::wait_idle(video_encoder * encoder)
{
	std::unique_lock lock(mutex);
	while (std::ranges::any_of(pending, [=](auto & data) { return data.encoder == encoder; }))
		cv.wait_for(lock, std::chrono::milliseconds(100));
}

std::shared_ptr<video_encoder::sender> video_encoder::sender::get()
{
	static std::weak_ptr<video_encoder::sender> instance;
	static std::mutex m;
	std::unique_lock lock(m);
	auto s = instance.lock();
	if (s)
		return s;
	s.reset(new video_encoder::sender());
	instance = s;
	return s;
}

std::unique_ptr<video_encoder> video_encoder::create(
        wivrn::vk_bundle & wivrn_vk,
        const encoder_settings & settings,
        uint8_t stream_idx)
{
	using namespace std::string_literals;
	std::unique_ptr<video_encoder> res;
	if (settings.encoder_name == encoder_vulkan)
	{
#if WIVRN_USE_VULKAN_ENCODE
		switch (settings.codec)
		{
			case video_codec::h264:
				res = video_encoder_vulkan_h264::create(wivrn_vk, settings, stream_idx);
				break;
			case video_codec::h265:
				res = video_encoder_vulkan_h265::create(wivrn_vk, settings, stream_idx);
				break;
			case video_codec::av1:
				res = video_encoder_vulkan_av1::create(wivrn_vk, settings, stream_idx);
				break;
			case video_codec::raw:
				throw std::runtime_error("raw codec only supported on raw encoder");
		}
#else
		throw std::runtime_error("Vulkan video encode not enabled");
#endif
	}
	if (settings.encoder_name == encoder_x264)
	{
#if WIVRN_USE_X264
		res = std::make_unique<video_encoder_x264>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("x264 encoder not enabled");
#endif
	}
	if (settings.encoder_name == encoder_nvenc)
	{
#if WIVRN_USE_NVENC
		res = std::make_unique<video_encoder_nvenc>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("nvenc support not enabled");
#endif
	}
	if (settings.encoder_name == encoder_vaapi)
	{
#if WIVRN_USE_VAAPI
		res = std::make_unique<video_encoder_va>(wivrn_vk, settings, stream_idx);
#else
		throw std::runtime_error("vaapi support not enabled");
#endif
	}

	if (settings.encoder_name == encoder_raw)
	{
		res = std::make_unique<video_encoder_raw>(wivrn_vk, settings, stream_idx);
	}

	if (not res)
		throw std::runtime_error("Failed to create encoder " + settings.encoder_name);

	res->video_fec = settings.video_fec.enabled;
	res->video_fec_sources = settings.video_fec.sources;
	res->video_fec_repairs = settings.video_fec.repairs;
	res->video_fec_repair_delay_us = settings.video_fec.repair_delay_us;

	if (auto value = std::getenv("WIVRN_VIDEO_FEC"))
		res->video_fec = std::string_view(value) != "0";
	if (auto value = std::getenv("WIVRN_VIDEO_FEC_SCHEME"))
	{
		const std::string_view scheme(value);
		if (scheme == "xor" || scheme == "rs")
			res->video_fec_rs = scheme == "rs";
		else
			U_LOG_W("Invalid WIVRN_VIDEO_FEC_SCHEME '%s'; using rs", value);
	}
	if (auto value = std::getenv("WIVRN_VIDEO_FEC_SOURCES"))
	{
		const std::string_view text(value);
		unsigned count = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
		if (error == std::errc{} && end == text.data() + text.size() && count >= 1 && count <= video_rs_group_size)
			res->video_fec_sources = count;
		else
			U_LOG_W("Invalid WIVRN_VIDEO_FEC_SOURCES '%s'; keeping configured value", value);
	}
	if (auto value = std::getenv("WIVRN_VIDEO_FEC_REPAIRS"))
	{
		const std::string_view text(value);
		unsigned count = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
		if (error == std::errc{} && end == text.data() + text.size() && count >= 1 && count <= video_rs_max_repairs)
			res->video_fec_repairs = count;
		else
			U_LOG_W("Invalid WIVRN_VIDEO_FEC_REPAIRS '%s'; keeping configured value", value);
	}
	// Derive depth after the legacy source-count override, too.
	auto effective_fec = settings.video_fec;
	effective_fec.sources = res->video_fec_rs ? res->video_fec_sources : video_fec_group_size;
	res->video_fec_interleave = effective_fec.interleave();
	if (auto value = std::getenv("WIVRN_VIDEO_FEC_INTERLEAVE"))
	{
		const std::string_view text(value);
		unsigned depth = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), depth);
		if (error == std::errc{} && end == text.data() + text.size() && depth >= 1 && depth <= video_fec_max_interleave_depth)
			res->video_fec_interleave = depth;
		else
			U_LOG_W("Invalid WIVRN_VIDEO_FEC_INTERLEAVE '%s'; keeping configured value", value);
	}
	if (auto value = std::getenv("WIVRN_VIDEO_FEC_REPAIR_DELAY_US"))
	{
		const std::string_view text(value);
		unsigned delay = 0;
		const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), delay);
		if (error == std::errc{} && end == text.data() + text.size() && delay <= 5000)
			res->video_fec_repair_delay_us = delay;
		else
			U_LOG_W("Invalid WIVRN_VIDEO_FEC_REPAIR_DELAY_US '%s'; keeping configured value", value);
	}
	if (res->video_fec && res->video_fec_repair_delay_us)
		res->delayed_repairs = std::make_unique<video_fec_repair_queue>();
	U_LOG_I("Video FEC stream %u: %s, %u sources + %u repairs per full block, interleave %u, repair delay %u us, enabled %d",
	        stream_idx,
	        res->video_fec_rs ? "Reed-Solomon" : "XOR",
	        unsigned(res->video_fec_rs ? res->video_fec_sources : video_fec_group_size),
	        unsigned(res->video_fec_rs ? res->video_fec_repairs : 1),
	        unsigned(res->video_fec_interleave),
	        res->video_fec_repair_delay_us,
	        int(res->video_fec));
	auto wivrn_dump_video = std::getenv("WIVRN_DUMP_VIDEO");
	if (wivrn_dump_video)
	{
		std::string file(wivrn_dump_video);
		file += "-" + std::to_string(stream_idx);
		switch (settings.codec)
		{
			case h264:
				file += ".h264";
				break;
			case h265:
				file += ".h265";
				break;
			case av1:
				file += ".av1";
				break;
			case raw:
				file += ".yuv";
				break;
		}
		res->video_dump.open(file);
	}
	return res;
}

video_encoder::video_encoder(vk_bundle & vk,
                             uint8_t stream_idx,
                             uint32_t target_queue,
                             const encoder_settings & settings,
                             std::unique_ptr<idr_handler> idr,
                             bool async_send) :
        stream_idx(stream_idx),
        target_queue(target_queue),
        need_transfer(not vk.optimal_transfer(vk.queue.family_index, target_queue)),
        bitrate_multiplier(settings.bitrate_multiplier),
        shared_sender(async_send ? sender::get() : nullptr),
        idr(std::move(idr)),
        extent{
                .width = settings.width,
                .height = settings.height,
        }
{
	assert(this->idr);
}

video_encoder::~video_encoder()
{
	if (shared_sender)
		shared_sender->wait_idle(this);
	if (delayed_repairs)
	{
		delayed_repairs->cancel();
		if (auto dropped = delayed_repairs->overflow_count())
			U_LOG_W("Video FEC stream %u: dropped %llu delayed repair batches on queue overflow", stream_idx, (unsigned long long)dropped);
		delayed_repairs.reset();
	}
}

void video_encoder::on_feedback(const from_headset::feedback & feedback)
{
	assert(feedback.stream_index == stream_idx);
	idr->on_feedback(feedback);
}

void video_encoder::reset()
{
	if (delayed_repairs)
	{
		delayed_repairs->cancel();
		delayed_repairs->resume();
	}
	idr->reset();
}

void video_encoder::pause_delayed_repairs()
{
	if (delayed_repairs)
		delayed_repairs->pause();
}

void video_encoder::stop_delayed_repairs()
{
	if (delayed_repairs)
		delayed_repairs->stop();
}

void video_encoder::set_bitrate(uint32_t bitrate_bps)
{
	pending_bitrate = bitrate_bps * bitrate_multiplier;
}

void video_encoder::set_framerate(float framerate)
{
	pending_framerate = framerate;
}

void video_encoder::present_image(vk::Image y_cbcr, vk::SemaphoreSubmitInfo info, uint64_t frame_index)
{
	wivrn::trace::scope trace_present(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "present_image");
	// Wait for encoder to be done
	present_slot = (present_slot + 1) % num_slots;
	state[present_slot].wait(busy);
	if (idr->should_skip(frame_index, stream_idx))
	{
		state[present_slot] = skip;
		return;
	}
	state[present_slot] = busy;
	return present_image(y_cbcr, info, present_slot, frame_index);
}

void video_encoder::encode(wivrn_session & cnx,
                           const to_headset::video_stream_data_shard::view_info_t & view_info,
                           uint64_t frame_index)
{
	encode_slot = (encode_slot + 1) % num_slots;

	struct idle_setter
	{
		std::atomic_unsigned_lock_free & state;
		~idle_setter()
		{
			state = idle;
			state.notify_all();
		}
	};
	idle_setter i{state[encode_slot]};

	if (state[encode_slot] == skip)
		return;

	if (shared_sender)
		shared_sender->wait_idle(this);
	if (delayed_repairs && this->cnx != &cnx)
		delayed_repairs->cancel();
	this->cnx = &cnx;
	clock = cnx.get_offset();

	wivrn::trace::scope trace_encode(wivrn::trace::cpu_track::encoder, stream_idx, frame_index, "encode");
	auto encode_begin = os_monotonic_get_ns();
	timing_info = {
	        .encode_begin = clock.to_headset(encode_begin),
	};

	// Prepare the video shard template
	shard.stream_item_idx = stream_idx;
	shard.frame_idx = frame_index;
	shard.shard_idx = 0;
	shard.view_info = view_info;
	shard.timing_info.reset();

	auto data = encode(encode_slot, frame_index);
	if (data)
	{
		timing_info.encode_end = clock.to_headset(os_monotonic_get_ns());
		assert(shared_sender);
		shared_sender->push(std::move(*data));
	}
}

void video_encoder::SendData(std::span<uint8_t> data, bool end_of_frame, bool control)
{
	std::lock_guard lock(mutex);
	if (shard.shard_idx == 0)
	{
		// One SendData call per NAL; span the whole frame, not each call.
		wivrn::trace::cpu_begin(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
		timing_info.send_begin = clock.to_headset(os_monotonic_get_ns());
	}
	if (end_of_frame)
	{
		timing_info.send_end = clock.to_headset(os_monotonic_get_ns());
		if (not timing_info.encode_end)
			timing_info.encode_end = timing_info.send_end;
	}
	if (video_dump)
		video_dump.write((char *)data.data(), data.size());

	ssize_t max_payload_size = (cnx->has_stream() and not control) ? to_headset::video_stream_data_shard::max_payload_size : std::numeric_limits<uint32_t>::max();

	const bool udp = cnx->has_stream() && !control;
	const bool protect = udp && video_fec;
	std::vector<to_headset::video_stream_data_shard> window;
	const size_t window_size = video_fec_interleave * (video_fec_rs ? video_fec_sources : video_fec_group_size);
	if (protect)
		window.reserve(window_size);
	auto send_source = [&](const auto & source) {
		try
		{
			if (control)
				cnx->send_control(to_headset::video_stream_data_shard{source});
			else
				cnx->send_stream(to_headset::video_stream_data_shard{source});
		}
		catch (...)
		{
			// Ignore network errors, as for the original source path.
		}
	};
	auto send_repair = [session = cnx](auto repair) {
		try
		{
			session->send_stream(std::move(repair));
		}
		catch (...)
		{
			// Repairs use the same best-effort transport.
		}
	};
	auto begin = data.begin();
	auto end = data.end();
	while (begin != end)
	{
		size_t payload_size;
		if (protect)
		{
			shard.payload = {};
			shard.timing_info = timing_info; // Reserve the worst-case final packet.
			const auto limit = video_fec_symbol_capacity();
			payload_size = limit - serialized_size(shard);
			shard.timing_info.reset();
		}
		else
			payload_size = std::max(0z, max_payload_size - ssize_t(serialized_size(shard.view_info)));
		auto next = std::min(end, begin + payload_size);
		if (next == end)
		{
			if (end_of_frame)
			{
				if (serialized_size(timing_info) + std::distance(begin, next) <= max_payload_size)
					shard.timing_info = timing_info;
				else
					--next;
			}
		}
		shard.payload = {begin, next};
		if (protect)
			window.push_back(shard);
		else
			send_source(shard);
		++shard.shard_idx;
		shard.view_info.reset();
		begin = next;
		if (protect && (window.size() == window_size || begin == end))
		{
			if (delayed_repairs)
			{
				std::vector<to_headset::video_stream_repair_shard> repairs;
				auto hold_repair = [&](auto repair) { repairs.push_back(std::move(repair)); };
				send_interleaved_video(window, send_source, hold_repair, video_fec_rs ? video_fec_repairs : 0, video_fec_sources, video_fec_interleave);
				// All original sends have returned. Coded bodies own their memory;
				// the worker retains no encoder buffer or mutable frame metadata.
				delayed_repairs->push({video_fec_repair_queue::clock::now() + std::chrono::microseconds(video_fec_repair_delay_us),
				                       std::move(repairs),
				                       send_repair});
			}
			else
				send_interleaved_video(window, send_source, send_repair, video_fec_rs ? video_fec_repairs : 0, video_fec_sources, video_fec_interleave);
			window.clear();
		}
	}
	if (end_of_frame)
		wivrn::trace::cpu_end(wivrn::trace::cpu_track::network, stream_idx, shard.frame_idx, "SendData");
}

} // namespace wivrn
