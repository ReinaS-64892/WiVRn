// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_fec_diagnostics.h"

#ifdef __ANDROID__
#include "application.h"
#endif

#include "version.h"
#include "wivrn_packets.h"
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
video_fec_diagnostics & video_fec_diagnostics::get()
{
	static video_fec_diagnostics recorder;
	return recorder;
}

std::filesystem::path video_fec_diagnostics::default_directory()
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

int64_t video_fec_diagnostics::now()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t video_fec_diagnostics::next_receiver_id()
{
	static std::atomic<uint64_t> counter = 0;
	return ++counter;
}

video_fec_diagnostics::~video_fec_diagnostics()
{
	stop();
}

bool video_fec_diagnostics::start(const std::filesystem::path & directory)
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
		write_all(fd, fmt::format("{{\"kind\":\"start\",\"schema\":2,\"purpose\":\"video_fec\",\"unix_ns\":{},\"mono_ns\":{},\"time_unit\":\"ns\",\"client_commit\":\"{}\",\"protocol_revision\":{}}}\n", now, video_fec_diagnostics::now(), wivrn::git_commit, wivrn::protocol_revision));
		stopping = false;
		writer = std::thread(&video_fec_diagnostics::write_events, this, fd);
		generation.store(++next_generation, std::memory_order_relaxed);
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

void video_fec_diagnostics::stop()
{
	{
		std::lock_guard lock(mutex);
		generation.store(0, std::memory_order_relaxed);
		stopping = true;
	}
	ready.notify_one();
	if (writer.joinable())
		writer.join();
}

void video_fec_diagnostics::record(uint64_t id, std::string json)
{
	if (!id || id != recording_id())
		return;
	{
		std::lock_guard lock(mutex);
		if (id != recording_id())
			return;
		if (queue.size() >= 1024 || json.size() > 16384)
		{
			++lost_events;
			return;
		}
		queue.push_back(std::move(json));
	}
	ready.notify_one();
}

video_fec_diagnostics::status video_fec_diagnostics::snapshot()
{
	std::lock_guard lock(mutex);
	return {recording_id() != 0, path, error, lost_events};
}

void video_fec_diagnostics::write_events(int fd)
{
	try
	{
		for (;;)
		{
			std::deque<std::string> batch;
			uint64_t lost;
			{
				std::unique_lock lock(mutex);
				ready.wait(lock, [&] { return stopping || !queue.empty(); });
				if (queue.empty() && stopping)
				{
					lost = lost_events;
					lock.unlock();
					write_all(fd, fmt::format("{{\"kind\":\"stop\",\"mono_ns\":{},\"lost_events\":{}}}\n", now(), lost));
					break;
				}
				batch.swap(queue);
			}
			std::string bytes;
			for (const auto & json: batch)
				bytes += json + '\n';
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
		generation.store(0, std::memory_order_relaxed);
		lost_events += queue.size();
		queue.clear();
	}
}
} // namespace wivrn
