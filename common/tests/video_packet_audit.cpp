// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_packet_audit.h"
#include "video_packet_recording.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using wivrn::video_packet_audit;

int main()
{
	try
	{
		auto sink = [](const std::string & line) {
			if (line.size() > 2048)
				throw std::runtime_error("audit line exceeds logcat budget");
			std::cout << line << '\n';
		};
		unsigned disabled_calls = 0;
		{
			video_packet_audit disabled("tx", 0, [&](auto &) { ++disabled_calls; }, false);
			disabled.send(0, 0, 100, true, true);
			disabled.sent_frame(0);
			disabled.receive(0, 0, 100);
			disabled.completed_frame(0);
			disabled.retire_before(1);
			disabled.reset();
		}
		if (disabled_calls)
			throw std::runtime_error("disabled audit emitted records");
		{
			video_packet_audit tx("tx", 0, sink, true), rx("rx", 0, sink, true);
			// A skipped source frame (12) must not count as loss. Frame 11 is
			// entirely lost; frame 14 is still pending at observation shutdown.
			for (uint64_t index: {10, 11, 13, 14})
			{
				for (uint16_t shard = 0; shard < 8; ++shard)
					tx.send(index, shard, 100, index != 10 or shard != 7, index != 10 or shard != 6);
				tx.sent_frame(index);
			}
			for (uint16_t shard: {0, 2, 2, 1, 7, 3})
				rx.receive(10, shard, 100);
			rx.retire_before(11);
			rx.receive(10, 4, 100);
			for (uint16_t shard = 0; shard < 8; ++shard)
				rx.receive(13, shard, 100);
			rx.completed_frame(13);
			rx.retire_before(14);
			rx.receive(14, 0, 100);
		}
		{
			// Missing first and terminal shards both have a known denominator
			// when compared with the transmitter, even without RX frame completion.
			video_packet_audit tx("tx", 3, sink, true), rx("rx", 3, sink, true);
			for (uint64_t index: {0, 1})
			{
				for (uint16_t shard = 0; shard < 4; ++shard)
				{
					tx.send(index, shard, 100, true, true);
					if ((index == 0 and shard != 0) or (index == 1 and shard != 3))
						rx.receive(index, shard, 100);
				}
				tx.sent_frame(index);
				rx.retire_before(index + 1);
			}
		}
		{
			video_packet_audit tx("tx", 4, sink, true), rx("rx", 4, sink, true);
			tx.send(0, 0, 100, false, true);
			tx.sent_frame(0);
			rx.receive(0, 0, 100);
			rx.completed_frame(0);
			rx.retire_before(1);
		}
		{
			video_packet_audit tx("tx", 5, sink, true), rx("rx", 5, sink, true);
			for (uint16_t shard = 0; shard < 8; ++shard)
				tx.send(0, shard, 100, true, true);
			tx.sent_frame(0);
			// No packet reached RX; its end marker still defines a valid capture.
		}
		{
			// Exercise the same runtime start/stop and shared file path as the UI.
			char temporary[] = "/tmp/wivrn-packet-recording-XXXXXX";
			if (not mkdtemp(temporary))
				throw std::runtime_error("mkdtemp");
			struct cleanup
			{
				std::filesystem::path path;
				~cleanup()
				{
					std::error_code error;
					std::filesystem::remove_all(path, error);
				}
			} cleanup{temporary};
			auto read = [](const auto & path) {
				std::ifstream input(path);
				return std::string(std::istreambuf_iterator<char>(input), {});
			};
			auto file = std::make_shared<wivrn::packet_audit_file>(cleanup.path / "logs");
			if ((std::filesystem::status(file->path()).permissions() & std::filesystem::perms::all) != (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write))
				throw std::runtime_error("recording file permissions");
			video_packet_audit tx("tx", 6, sink, true);
			for (uint64_t frame = 0; frame < 15; ++frame)
			{
				for (uint16_t shard = 0; shard < 2; ++shard)
					tx.send(frame, shard, 100, true, true);
				tx.sent_frame(frame);
			}
			wivrn::video_packet_recording recording(6);
			recording.receive(7, 0, 100); // Before start; never appears in the file.
			recording.start(file);
			recording.receive(3, 0, 100); // Old packet must not move start backwards.
			for (uint64_t frame: {8, 9, 11})
			{
				if (frame != 9)
					recording.receive(frame, 0, 100);
				recording.receive(frame, 1, 100);
				if (frame != 9)
					recording.completed_frame(frame);
				recording.retire_before(frame + 1);
			}
			recording.receive(12, 0, 100); // Pending at stop: excluded by frame_limit.
			recording.stop();
			file->close();
			if (not file->error().empty())
				throw std::runtime_error(file->error());
			auto first_log = read(file->path());
			std::cout << first_log;
			auto second = std::make_shared<wivrn::packet_audit_file>(cleanup.path / "logs");
			recording.start(second);
			recording.receive(13, 0, 100);
			recording.receive(14, 0, 100);
			recording.completed_frame(14);
			recording.retire_before(15);
			recording.stop();
			second->close();
			if (second->path() == file->path() or read(file->path()) != first_log or read(second->path()).find("\"first_frame\":14") == std::string::npos)
				throw std::runtime_error("restart must use a new file and window");
			bool failed = false;
			try
			{
				wivrn::packet_audit_file invalid(file->path()); // Parent is a file.
			}
			catch (const std::exception &)
			{
				failed = true;
			}
			if (not failed)
				throw std::runtime_error("directory creation failure not reported");
			second->write("closed");
			if (second->error().empty())
				throw std::runtime_error("write failure not reported");
			auto shared = std::make_shared<wivrn::packet_audit_file>(cleanup.path / "logs");
			for (unsigned stream: {7, 8})
			{
				video_packet_audit tx("tx", stream, sink, true);
				for (uint64_t frame = 100; frame < 113; ++frame)
				{
					for (uint16_t shard = 0; shard < 2; ++shard)
						tx.send(frame, shard, 100, true, true);
					tx.sent_frame(frame);
				}
			}
			auto receive = [shared](unsigned stream) {
				wivrn::video_packet_recording recording(stream);
				recording.start(shared);
				recording.receive(100, 0, 100);
				for (uint64_t frame = 101; frame < 111; ++frame)
				{
					recording.receive(frame, 0, 100);
					recording.receive(frame, 1, 100);
					recording.completed_frame(frame);
					recording.retire_before(frame + 1);
				}
				recording.stop();
			};
			std::jthread left(receive, 7), right(receive, 8);
			left.join();
			right.join();
			shared->close();
			if (not shared->error().empty())
				throw std::runtime_error(shared->error());
			std::cout << read(shared->path());
		}
		{
			// Exercise chunk boundaries and the full uint16_t shard range.
			video_packet_audit tx("tx", 1, sink, true), rx("rx", 1, sink, true);
			for (uint16_t shard: {0, 511, 512, 65535})
			{
				tx.send(20, shard, 100, true, true);
				rx.receive(20, shard, 100);
			}
			tx.sent_frame(20);
			rx.completed_frame(20);
			rx.retire_before(21);
		}
		{
			// Late duplicate after history eviction must merge with the original.
			video_packet_audit tx("tx", 2, sink, true), rx("rx", 2, sink, true);
			for (uint64_t index = 0; index < 130; ++index)
			{
				tx.send(index, 0, 100, true, true);
				tx.sent_frame(index);
				rx.receive(index, 0, 100);
				rx.completed_frame(index);
				rx.retire_before(index + 1);
			}
			rx.receive(0, 0, 100);
			tx.reset();
			rx.reset();
		}
	}
	catch (const std::exception & error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
