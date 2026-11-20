// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "packet_audit_file.h"
#include "video_packet_audit.h"

#include <memory>
#include <mutex>

namespace wivrn
{
// The UI can start/stop while the network thread receives packets. Audit calls
// and destruction are serialized so end is written before the file is closed.
class video_packet_recording
{
	const unsigned stream;
	std::mutex mutex;
	std::shared_ptr<packet_audit_file> file;
	std::unique_ptr<video_packet_audit> audit;
	uint64_t highest_received_frame = 0;

public:
	explicit video_packet_recording(unsigned stream) : stream(stream) {}
	void start(std::shared_ptr<packet_audit_file> target)
	{
		std::lock_guard lock(mutex);
		audit.reset();
		file = std::move(target);
	}
	void stop()
	{
		std::lock_guard lock(mutex);
		audit.reset();
		file.reset();
	}
	void receive(uint64_t index, uint16_t shard, size_t bytes)
	{
		std::lock_guard lock(mutex);
		highest_received_frame = std::max(highest_received_frame, index);
		if (file and not audit)
		{
			// Discard every frame that may already have received packets before
			// start, including the packet that triggers creation of this capture.
			auto target = file;
			audit = std::make_unique<video_packet_audit>("rx", stream, [target](const std::string & line) { target->write(line); }, true, highest_received_frame + 1);
		}
		if (audit)
			audit->receive(index, shard, bytes);
	}
	void retire_before(uint64_t index)
	{
		std::lock_guard lock(mutex);
		if (audit)
			audit->retire_before(index);
	}
	void completed_frame(uint64_t index)
	{
		std::lock_guard lock(mutex);
		if (audit)
			audit->completed_frame(index);
	}
};
} // namespace wivrn
