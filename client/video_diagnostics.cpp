// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_diagnostics.h"

#ifdef __ANDROID__
#include "application.h"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <spdlog/fmt/fmt.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace
{
std::string json_string(const std::string & input)
{
	std::string result = "\"";
	for (unsigned char c: input)
	{
		if (c == '"' || c == '\\')
		{
			result += '\\';
			result += c;
		}
		else if (c < 0x20)
			result += fmt::format("\\u{:04x}", c);
		else
			result += c;
	}
	return result + '"';
}

void write_all(int fd, std::string_view bytes)
{
	while (!bytes.empty())
	{
		auto n = ::write(fd, bytes.data(), bytes.size());
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			throw std::system_error(n < 0 ? errno : EIO, std::generic_category(), "Writing diagnostic recording");
		bytes.remove_prefix(n);
	}
}
} // namespace

namespace wivrn
{
video_diagnostics & video_diagnostics::get()
{
	static video_diagnostics recorder;
	return recorder;
}

std::filesystem::path video_diagnostics::default_directory()
{
#ifdef __ANDROID__
	return application::get_config_path() / "logs";
#else
	// XDG requires absolute paths and places persistent logs in STATE_HOME.
	if (const char * state = std::getenv("XDG_STATE_HOME"); state && std::filesystem::path(state).is_absolute())
		return std::filesystem::path(state) / "wivrn/logs";
	if (const char * home = std::getenv("HOME"); home && std::filesystem::path(home).is_absolute())
		return std::filesystem::path(home) / ".local/state/wivrn/logs";
	return {}; // start() reports an unavailable state directory in the UI.
#endif
}

video_diagnostics::~video_diagnostics()
{
	stop();
}

bool video_diagnostics::start(const std::filesystem::path & directory)
{
	// UI thread owns start/stop. The writer only touches the queue and status.
	stop();
	std::lock_guard lock(mutex);
	error.clear();
	lost_events = 0;
	path.clear();
	int fd = -1;
	try
	{
		if (!directory.is_absolute())
			throw std::runtime_error("Diagnostic log directory must be absolute");
		if (std::filesystem::create_directories(directory))
			std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
		static uint64_t sequence = 0;
		const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		path = directory / fmt::format("video-{}-{}-{}.jsonl", now, getpid(), sequence++);
		fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(), "Creating diagnostic recording");
		write_all(fd, fmt::format("{{\"kind\":\"start\",\"schema\":1,\"unix_ns\":{},\"time_unit\":\"ns\"}}\n", now));
		stopping = false;
		writer = std::thread(&video_diagnostics::write_events, this, fd);
		recording.store(true, std::memory_order_relaxed);
		return true;
	}
	catch (const std::exception & e)
	{
		if (fd >= 0)
			::close(fd);
		error = e.what();
		return false;
	}
}

void video_diagnostics::stop()
{
	{
		std::lock_guard lock(mutex);
		recording.store(false, std::memory_order_relaxed);
		stopping = true;
	}
	ready.notify_one();
	if (writer.joinable())
		writer.join();
}

void video_diagnostics::record(event value)
{
	if (!active())
		return;
	{
		std::lock_guard lock(mutex);
		if (!active())
			return;
		if (queue.size() >= 4096)
		{
			++lost_events;
			return;
		}
		// Error text is bounded too, so a stalled disk cannot grow memory use.
		if (value.detail.size() > 1024)
		{
			size_t end = 1024;
			while (end && (static_cast<unsigned char>(value.detail[end]) & 0xc0) == 0x80)
				--end; // Preserve a UTF-8 codepoint at the truncation boundary.
			value.detail.resize(end);
		}
		value.mono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		queue.push_back(std::move(value));
	}
	ready.notify_one();
}

video_diagnostics::status video_diagnostics::snapshot()
{
	std::lock_guard lock(mutex);
	return {active(), path, error, lost_events};
}

void video_diagnostics::write_events(int fd)
{
	try
	{
		for (;;)
		{
			std::deque<event> batch;
			uint64_t lost;
			{
				std::unique_lock lock(mutex);
				ready.wait(lock, [&] { return stopping || !queue.empty(); });
				if (queue.empty() && stopping)
				{
					lost = lost_events;
					lock.unlock();
					write_all(fd, fmt::format("{{\"kind\":\"stop\",\"lost_events\":{}}}\n", lost));
					break;
				}
				batch.swap(queue);
			}
			std::string bytes;
			for (const auto & e: batch)
			{
				bytes += fmt::format("{{\"kind\":{},\"detail\":{},\"mono_ns\":{},\"xr_ns\":{},\"frame\":{},\"stream\":{},\"first_ns\":{},\"deadline_ns\":{},\"display_ns\":{},\"wait_ns\":{},\"budget_ns\":{},\"received\":{},\"expected_min\":{},\"units\":{},\"vcl\":{},\"vcl_observed\":{},\"vcl_damaged\":{},\"reliable\":{},\"complete\":{},\"submitted\":{},\"expired\":{}}}\n",
				                     json_string(e.kind),
				                     json_string(e.detail),
				                     e.mono_ns,
				                     e.xr_ns,
				                     e.frame,
				                     e.stream,
				                     e.first_ns,
				                     e.deadline_ns,
				                     e.display_ns,
				                     e.wait_ns,
				                     e.budget_ns,
				                     e.received,
				                     e.expected_min,
				                     e.units,
				                     e.vcl,
				                     e.vcl_observed,
				                     e.vcl_damaged,
				                     e.reliable,
				                     e.complete,
				                     e.submitted,
				                     e.expired);
			}
			write_all(fd, bytes);
		}
		const auto result = ::close(fd);
		fd = -1;
		if (result < 0)
			throw std::system_error(errno, std::generic_category(), "Closing diagnostic recording");
	}
	catch (const std::exception & e)
	{
		if (fd >= 0)
			::close(fd);
		std::lock_guard lock(mutex);
		error = e.what();
		recording.store(false, std::memory_order_relaxed);
		lost_events += queue.size();
		queue.clear();
	}
}
} // namespace wivrn
