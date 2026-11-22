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

#include "stream.h"

#include "application.h"
#include "utils/i18n.h"
#include "utils/named_thread.h"
#include "video_diagnostics.h"

#include <spdlog/spdlog.h>
#include <uni_algo/case.h>

void scenes::stream::process_packets()
{
#ifdef __ANDROID__
	application::instance().setup_jni();
#endif
	XrTime last_diagnostic_heartbeat = 0;
	while (state_ != state::shutdown)
	{
		try
		{
			auto timeout = std::chrono::milliseconds(500);
			{
				std::shared_lock lock(decoder_mutex);
				for (auto & item: decoders)
				{
					if (item.decoder)
						item.decoder->expire(instance.now());
				}
				const auto now = instance.now();
				for (auto & item: decoders)
				{
					if (!item.decoder)
						continue;
					if (auto deadline = item.decoder->next_deadline())
					{
						const auto remaining = std::chrono::nanoseconds(std::max<XrDuration>(0, *deadline - now));
						timeout = std::min(timeout, std::chrono::ceil<std::chrono::milliseconds>(remaining));
					}
				}
			}
			// A timeout returns to the deadline service even if no more video arrives.
			if (wivrn::video_diagnostics::get().active())
			{
				const auto now = instance.now();
				if (now - last_diagnostic_heartbeat >= 1'000'000'000)
				{
					last_diagnostic_heartbeat = now;
					wivrn::video_diagnostics::get().record({.kind = "network_heartbeat", .detail = fmt::format("state={}", int(state_.load())), .xr_ns = now, .budget_ns = timeout.count() * 1'000'000, .received = network_session->bytes_received()});
				}
			}
			network_session->poll(*this, timeout);
		}
		catch (std::exception & e)
		{
			spdlog::info("Exception in network thread, exiting: {}", e.what());
			wivrn::video_diagnostics::get().record({.kind = "network_error", .detail = e.what(), .xr_ns = instance.now()});
			exit();
		}
	}
}

void scenes::stream::operator()(to_headset::server_message && message)
{
	switch (message.kind)
	{
		case to_headset::server_message::kind::toast:
		case to_headset::server_message::kind::toast_urgent: {
			auto toast = gui_toast.lock();
			toast->emplace(message.msg, message.kind == to_headset::server_message::kind::toast_urgent);

			gui_status_last_change = instance.now();
			break;
		}

		case to_headset::server_message::kind::error: {
			wivrn::video_diagnostics::get().record({.kind = "server_error", .detail = message.msg, .xr_ns = instance.now()});
			auto queue = stream_error_queue.lock();
			queue->emplace(std::move(message.msg));

			break;
		}
	}
}

void scenes::stream::operator()(to_headset::video_stream_data_shard && shard, bool reliable)
{
	std::shared_lock lock(decoder_mutex);
	uint8_t idx = shard.stream_item_idx;
	if (idx >= decoders.size() || !decoders[idx].decoder)
	{
		// We don't know (yet?) about this stream, ignore packet
		wivrn::video_diagnostics::get().record({.kind = "unconfigured_video_packet", .xr_ns = instance.now(), .frame = shard.frame_idx, .stream = idx, .reliable = reliable});
		return;
	}
	decoders[idx].decoder->push_shard(std::move(shard), reliable);
}

void scenes::stream::operator()(to_headset::video_stream_repair_shard && repair)
{
	std::shared_lock lock(decoder_mutex);
	const auto idx = repair.source.stream_item_idx;
	if (idx < decoders.size() && decoders[idx].decoder)
		decoders[idx].decoder->push_repair(std::move(repair));
}

void scenes::stream::operator()(to_headset::feature_control && control)
{
	switch (control.f)
	{
		case wivrn::to_headset::feature_control::hid_input:
			hid_forwarding = control.state;
			if (not control.state and (application::get_config().forward_keyboard or application::get_config().forward_mouse or application::get_config().forward_gamepad))
			{
				auto toast = gui_toast.lock();
				toast->emplace(_("The server does not allow forwarded input devices"), true);
				gui_status_last_change = instance.now();
			}
			return;
		case wivrn::to_headset::feature_control::microphone:
			if (audio_handle)
				audio_handle->set_mic_state(control.state);
			return;
	}
}

void scenes::stream::operator()(to_headset::audio_stream_description && desc)
{
	audio_handle.emplace(desc, *network_session, instance);
}

void scenes::stream::operator()(to_headset::video_stream_description && desc)
{
	wivrn::video_diagnostics::get().record({.kind = "video_description", .detail = fmt::format("{}x{} fps={} codec={}/{}", desc.width, desc.height, desc.frame_rate, int(desc.codec[0]), int(desc.codec[1])), .xr_ns = instance.now()});
	setup(desc);
	wivrn::video_diagnostics::get().record({.kind = "decoders_ready", .xr_ns = instance.now()});

	if (not tracking_thread)
	{
		tracking_thread = utils::named_thread("tracking_thread", &stream::tracking, this);
	}
}

void scenes::stream::operator()(to_headset::refresh_rate_change && rate)
{
	spdlog::info("refresh rate change request: {}", rate.hz);
	session.set_refresh_rate(rate.hz);
	std::shared_lock lock(decoder_mutex);
	if (video_stream_description)
		video_stream_description->refresh_rate = rate.hz;
}

void scenes::stream::operator()(to_headset::stream_tab_change && tab)
{
	next_gui_status = tab.tab;
}

void scenes::stream::operator()(to_headset::timesync_query && query)
{
	network_session->send_stream(from_headset::timesync_response{
	        .query = query.query,
	        .response = instance.now(),
	});
}

void scenes::stream::operator()(audio_data && data)
{
	if (audio_handle)
		(*audio_handle)(std::move(data));
}

void scenes::stream::send_feedback(const wivrn::from_headset::feedback & feedback)
{
	try
	{
		network_session->send_control(wivrn::from_headset::feedback{feedback});
	}
	catch (std::exception & e)
	{
		spdlog::warn("Exception while sending feedback packet: {}", e.what());
		wivrn::video_diagnostics::get().record({.kind = "feedback_error", .detail = e.what(), .xr_ns = instance.now(), .frame = feedback.frame_index, .stream = feedback.stream_index});
	}
}

void scenes::stream::operator()(to_headset::application_list && l)
{
	apps(std::move(l));
}

void scenes::stream::operator()(to_headset::application_icon && icon)
{
	apps(std::move(icon));
}

void scenes::stream::operator()(to_headset::running_applications && apps)
{
	*running_applications.lock() = std::move(apps);
}

void scenes::stream::start_application(std::string appid)
{
	wivrn::video_diagnostics::get().record({.kind = "start_application", .xr_ns = instance.now()});
	network_session->send_control(wivrn::from_headset::start_app{
	        .app_id = std::move(appid),
	});
}
