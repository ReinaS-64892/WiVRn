// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_fec_repair_queue.h"

#include <algorithm>

namespace wivrn
{
video_fec_repair_queue::video_fec_repair_queue() :
        worker([this](std::stop_token stop) { run(stop); })
{
}

video_fec_repair_queue::~video_fec_repair_queue()
{
	stop();
}

void video_fec_repair_queue::stop()
{
	{
		std::lock_guard lock(mutex);
		worker.request_stop();
	}
	cv.notify_all();
	if (worker.joinable())
		worker.join();
}

void video_fec_repair_queue::push(batch job)
{
	if (job.repairs.empty() || !job.send)
		return;
	{
		std::lock_guard lock(mutex);
		if (!accepting || cancelling || worker.get_stop_token().stop_requested())
			return;
		if (pending.size() == capacity)
		{
			pending.pop_front();
			++overflows;
		}
		auto position = std::upper_bound(pending.begin(), pending.end(), job.due, [](auto due, const auto & item) { return due < item.due; });
		pending.insert(position, std::move(job));
	}
	cv.notify_all();
}

void video_fec_repair_queue::cancel()
{
	std::unique_lock lock(mutex);
	cancelling = true;
	pending.clear();
	cv.notify_all();
	cv.wait(lock, [this] { return !active; });
	cancelling = false;
}

void video_fec_repair_queue::pause()
{
	std::unique_lock lock(mutex);
	accepting = false;
	pending.clear();
	cv.notify_all();
	cv.wait(lock, [this] { return !active; });
}

void video_fec_repair_queue::resume()
{
	std::lock_guard lock(mutex);
	accepting = true;
}

void video_fec_repair_queue::wait_idle()
{
	std::unique_lock lock(mutex);
	cv.wait(lock, [this] { return pending.empty() && !active; });
}

uint64_t video_fec_repair_queue::overflow_count()
{
	std::lock_guard lock(mutex);
	return overflows;
}

void video_fec_repair_queue::run(std::stop_token stop)
{
	std::unique_lock lock(mutex);
	while (!stop.stop_requested())
	{
		if (pending.empty())
		{
			cv.wait(lock, [&] { return stop.stop_requested() || !pending.empty(); });
			continue;
		}
		if (pending.front().due > clock::now())
		{
			// Push/cancel/stop wake this wait so a new earlier deadline wins.
			const auto due = pending.front().due;
			cv.wait_until(lock, due);
			continue;
		}
		auto job = std::move(pending.front());
		pending.pop_front();
		active = true;
		lock.unlock();
		for (auto & packet: job.repairs)
		{
			if (stop.stop_requested())
				break;
			try
			{
				job.send(std::move(packet));
			}
			catch (...)
			{
				// Same best-effort policy as immediate video/repair sends.
			}
		}
		// Destroy the callable before advertising idle: its captured connection
		// may be released immediately by cancel()/wait_idle() callers.
		job = {};
		lock.lock();
		active = false;
		cv.notify_all();
	}
	pending.clear();
	cv.notify_all();
}
} // namespace wivrn
