// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace wivrn
{
// Observes application video shards, not Wi-Fi retries. No payloads are logged.
// Call from the owning encoder/network thread; reset must be serialized with send.
class video_packet_audit
{
	struct bitmap
	{
		std::vector<uint64_t> words;
		void set(uint16_t index)
		{
			words.resize(std::max(words.size(), size_t(index / 64 + 1)));
			words[index / 64] |= uint64_t(1) << (index % 64);
		}
		std::string hex(size_t first) const
		{
			std::ostringstream out;
			out << std::hex;
			for (size_t i = std::min(first + 8, words.size()); i > first; --i)
			{
				out.width(16);
				out.fill('0');
				out << words[i - 1];
			}
			return out.str();
		}
	};
	struct frame
	{
		bitmap seen, on_time, udp, accepted;
		uint64_t packets = 0, bytes = 0, reordered = 0;
		int64_t first_ns = 0, last_ns = 0;
		bool complete = false;
	};
	inline static std::atomic<uint64_t> next_capture = 0;
	const bool enabled;
	const std::string side;
	const unsigned stream;
	const std::optional<uint64_t> first_frame;
	std::function<void(const std::string &)> sink;
	uint64_t capture = 0;
	mutable uint64_t sequence = 0;
	uint64_t retired_before = 0;
	std::pair<uint64_t, uint16_t> high_water{};
	std::map<uint64_t, frame> frames;
	std::chrono::steady_clock::time_point start;

	std::ostringstream prefix(std::string_view event) const
	{
		std::ostringstream out;
		out << "WIVRN_PACKET_AUDIT {\"v\":" << (first_frame ? 2 : 1) << ",\"side\":\"" << side
		    << "\",\"stream\":" << stream << ",\"capture\":" << capture
		    << ",\"seq\":" << sequence++ << ",\"event\":\"" << event << '"';
		return out;
	}
	void emit(std::ostringstream && out) const noexcept
	{
		try
		{
			out << '}';
			sink(out.str());
		}
		catch (...)
		{
			// Diagnostic output must not tear down streaming.
		}
	}
	void flush(uint64_t index, const frame & f)
	{
		// Bound each line for Android logcat (even at shard index 65535).
		for (size_t first = 0; first < f.seen.words.size(); first += 8)
		{
			auto out = prefix("frame");
			out << ",\"frame\":" << index << ",\"offset\":" << first * 64
			    << ",\"seen\":\"" << f.seen.hex(first) << "\",\"on_time\":\"" << f.on_time.hex(first)
			    << "\",\"udp\":\"" << f.udp.hex(first) << "\",\"accepted\":\"" << f.accepted.hex(first) << '"';
			if (first == 0)
				out << ",\"packets\":" << f.packets << ",\"bytes\":" << f.bytes
				    << ",\"reordered\":" << f.reordered << ",\"complete\":" << (f.complete ? "true" : "false")
				    << ",\"first_ns\":" << f.first_ns << ",\"last_ns\":" << f.last_ns;
			emit(std::move(out));
		}
	}
	frame & observe(uint64_t index, uint16_t shard, size_t bytes)
	{
		auto & f = frames[index];
		auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		if (f.packets++ == 0)
			f.first_ns = now;
		f.last_ns = now;
		f.bytes += bytes;
		f.seen.set(shard);
		return f;
	}
	void trim()
	{
		// At most 128 observed frames per stream. Very late arrivals can create
		// another fragment; the offline tool unions bitmaps instead of overwriting.
		while (frames.size() > 128)
		{
			auto it = frames.begin();
			flush(it->first, it->second);
			frames.erase(it);
		}
	}
	void finish()
	{
		for (const auto & [index, f]: frames)
			flush(index, f);
		frames.clear();
		auto out = prefix("end");
		out << ",\"retired_before\":" << retired_before;
		if (first_frame)
			out << ",\"frame_limit\":" << retired_before;
		emit(std::move(out));
	}

public:
	static bool requested()
	{
		if (const char * value = std::getenv("WIVRN_PACKET_AUDIT"); value and std::string_view(value) == "1")
			return true;
#ifdef __ANDROID__
		char value[PROP_VALUE_MAX]{};
		__system_property_get("debug.wivrn.packet_audit", value);
		return std::string_view(value) == "1";
#else
		return false;
#endif
	}
	video_packet_audit(std::string_view side, unsigned stream, std::function<void(const std::string &)> sink, bool enabled = requested(), std::optional<uint64_t> first_frame = {}) :
	        enabled(enabled), side(side), stream(stream), first_frame(first_frame), sink(std::move(sink))
	{
		if (enabled)
			reset();
	}
	video_packet_audit(const video_packet_audit &) = delete;
	video_packet_audit & operator=(const video_packet_audit &) = delete;
	~video_packet_audit()
	{
		if (enabled)
			finish();
	}
	void reset()
	{
		if (not enabled)
			return;
		if (capture)
			finish();
		capture = ++next_capture;
		sequence = 0;
		retired_before = first_frame.value_or(0);
		high_water = {};
		start = std::chrono::steady_clock::now();
		auto out = prefix("begin");
		if (first_frame)
			out << ",\"first_frame\":" << *first_frame;
		emit(std::move(out));
	}
	void send(uint64_t index, uint16_t shard, size_t bytes, bool udp, bool accepted)
	{
		if (not enabled)
			return;
		auto & f = observe(index, shard, bytes);
		if (udp)
			f.udp.set(shard);
		if (accepted)
			f.accepted.set(shard);
		trim();
	}
	void sent_frame(uint64_t index)
	{
		if (not enabled)
			return;
		if (auto it = frames.find(index); it != frames.end())
		{
			it->second.complete = true;
			flush(index, it->second);
			frames.erase(it);
		}
	}
	void receive(uint64_t index, uint16_t shard, size_t bytes)
	{
		if (not enabled or (first_frame and index < *first_frame))
			return;
		auto & f = observe(index, shard, bytes);
		if (index >= retired_before)
			f.on_time.set(shard);
		auto key = std::pair(index, shard);
		if (key < high_water)
			++f.reordered;
		high_water = std::max(high_water, key);
		trim();
	}
	void completed_frame(uint64_t index)
	{
		if (enabled)
			if (auto it = frames.find(index); it != frames.end())
				it->second.complete = true;
	}
	void retire_before(uint64_t index)
	{
		if (enabled)
			retired_before = std::max(retired_before, index);
	}
};
} // namespace wivrn
