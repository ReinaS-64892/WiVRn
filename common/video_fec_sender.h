// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_fec.h"

namespace wivrn
{
// Transport encryption is allowed to mutate source payload spans. Capture the
// plaintext parity first, then send source and repair in that order. Callbacks
// implement the transport's error policy independently for the two packets.
template <typename SendSource, typename SendRepair>
void send_video_source(const to_headset::video_stream_data_shard & source,
                       video_parity_builder * parity,
                       SendSource && send_source,
                       SendRepair && send_repair)
{
	auto repair = parity ? parity->push(source) : std::nullopt;
	send_source(source);
	if (repair)
		send_repair(std::move(*repair));
}
} // namespace wivrn
