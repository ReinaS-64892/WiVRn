// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wivrn_packets.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace wivrn
{
// One queue per encoder, created only for opt-in delayed FEC. Jobs own coded
// payloads; neither source buffers nor references to encoder state may escape.
class video_fec_repair_queue
{
public:
	using repair = to_headset::video_stream_repair_shard;
	using clock = std::chrono::steady_clock;
	struct batch
	{
		clock::time_point due;
		std::vector<repair> repairs;
		std::function<void(repair &&)> send;
	};
	static constexpr size_t capacity = 64;

	video_fec_repair_queue();
	~video_fec_repair_queue();
	// Never wait for queue space: retire the oldest pending batch on overflow.
	void push(batch job);
	// Cancel pending work and wait for an in-flight callback to return. The
	// caller must keep the callback's connection alive until this returns.
	void cancel();
	void pause();
	void resume();
	void stop();
	void wait_idle();
	uint64_t overflow_count();

private:
	void run(std::stop_token stop);
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<batch> pending;
	bool active = false;
	bool cancelling = false;
	bool accepting = true;
	uint64_t overflows = 0;
	std::jthread worker;
};
} // namespace wivrn
