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
// Process-wide recording survives decoder reconfiguration and reconnects.
// Producers enqueue bounded metadata only; the writer owns disk I/O.
class video_fec_diagnostics
{
public:
	struct status
	{
		bool recording;
		std::filesystem::path path;
		std::string error;
		uint64_t lost_events;
	};
	static video_fec_diagnostics & get();
	static std::filesystem::path default_directory();
	static int64_t now();
	static uint64_t next_receiver_id();
	~video_fec_diagnostics();
	bool start(const std::filesystem::path & directory = default_directory());
	void stop();
	uint64_t recording_id() const
	{
		return generation.load(std::memory_order_relaxed);
	}
	void record(uint64_t recording_id, std::string json);
	status snapshot();

private:
	std::atomic<uint64_t> generation = 0;
	uint64_t next_generation = 0;
	std::mutex mutex;
	std::condition_variable ready;
	std::deque<std::string> queue;
	std::thread writer;
	bool stopping = false;
	std::filesystem::path path;
	std::string error;
	uint64_t lost_events = 0;
	void write_events(int fd);
};
} // namespace wivrn
