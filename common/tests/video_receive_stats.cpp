// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_receive_stats.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

using wivrn::video_receive_stats;

static void require(bool condition, const char * message)
{
	if (not condition)
		throw std::runtime_error(message);
}

int main()
{
	try
	{
		video_receive_stats stats;
		require(stats.read().finalized_frames() == 0, "initial frame count");
		require(stats.read().incomplete_percent() == 0, "initial percentage");
		stats.packet(10, 0, false, false);
		stats.packet(10, 3, false, false);
		stats.packet(10, 3, false, true);
		stats.packet(10, 1, false, false);
		stats.finish_frame(4, 3, true, false); // terminal at index 3; one missing
		stats.packet(10, 0, true, false);      // late duplicate cannot be identified
		stats.packet(13, 0, false, false);     // missing frame IDs are not losses
		stats.finish_frame(1, 1, true, true);
		stats.finish_frame(0, 0, false, false); // unobserved/skipped frame
		stats.packet(14, 2, false, false);
		stats.finish_frame(3, 1, false, false); // 2 holes plus >=1 missing tail
		stats.packet(15, 0, false, false);
		stats.finish_frame(1, 1, false, false); // no holes, but missing terminal
		stats.packet(16, 0, false, false);      // pending: not finalized
		auto s = stats.read();
		require(s.received_packets == 9, "each arrival counted exactly once");
		require(s.buffered_duplicates == 1, "only buffered duplicates counted");
		require(s.late_packets == 1, "late packets");
		require(s.out_of_order_packets == 2, "out of order includes late duplicates");
		require(s.completed_frames == 1 and s.incomplete_frames == 3, "observed finalized frames only");
		require(s.missing_packets_min == 5, "terminal loss contributes at least one missing packet");
		require(s.frames_with_unknown_total == 2, "unknown frame totals");
		require(s.incomplete_percent() == 75, "frame percentage is not packet loss percentage");
		stats.packet(14, 1, true, false);
		require(stats.read().missing_packets_min == 5, "late arrival does not change past discard count");
		video_receive_stats new_stream;
		require(new_stream.read().received_packets == 0, "new stream starts with fresh counters");

		video_receive_stats concurrent;
		std::atomic<bool> done = false;
		std::jthread writer([&] {
			for (uint64_t i = 0; i < 10000; ++i)
			{
				concurrent.packet(i, 0, false, false);
				concurrent.finish_frame(1, 1, true, true);
			}
			done = true;
		});
		do
		{
			auto snapshot = concurrent.read();
			require(snapshot.completed_frames <= snapshot.received_packets, "coherent snapshot");
			require(snapshot.received_packets - snapshot.completed_frames <= 1, "snapshot not torn");
		} while (not done);
		writer.join();
		require(concurrent.read().completed_frames == 10000, "concurrent completion count");
		std::cout << "video receive statistics tests passed\n";
	}
	catch (const std::exception & error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
