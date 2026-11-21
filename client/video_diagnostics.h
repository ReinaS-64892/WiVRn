// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace wivrn
{
// Process-wide so a recording can include disconnect, lobby and reconnect.
// Network/decoder threads only enqueue; filesystem I/O belongs to the writer.
class video_diagnostics
{
public:
	struct event
	{
		std::string kind = {};
		std::string detail = {};
		int64_t mono_ns = 0;
		int64_t xr_ns = 0;
		uint64_t frame = 0;
		uint32_t stream = 0;
		int64_t first_ns = 0, deadline_ns = 0, display_ns = 0, wait_ns = 0, budget_ns = 0;
		uint64_t received = 0, expected_min = 0, units = 0, vcl = 0;
		uint64_t vcl_observed = 0, vcl_damaged = 0;
		bool reliable = false, complete = false, submitted = false, expired = false;
	};
	struct status
	{
		bool recording;
		std::filesystem::path path;
		std::string error;
		uint64_t lost_events;
	};
	static video_diagnostics & get();
	static std::filesystem::path default_directory();
	~video_diagnostics();
	bool start(const std::filesystem::path & directory = default_directory());
	void stop();
	bool active() const
	{
		return recording.load(std::memory_order_relaxed);
	}
	void record(event value);
	status snapshot();

private:
	std::atomic<bool> recording = false;
	std::mutex mutex;
	std::condition_variable ready;
	std::deque<event> queue;
	std::thread writer;
	bool stopping = false;
	std::filesystem::path path;
	std::string error;
	uint64_t lost_events = 0;
	void write_events(int fd);
};
} // namespace wivrn

namespace wivrn::gui
{
bool video_recording_button();
}
