// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol_version.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

int main()
{
	using namespace wivrn;
	using shard_type = to_headset::video_stream_data_shard;
	shard_type shard{};
	shard.stream_item_idx = 1;
	shard.frame_idx = 42;
	shard.view_info.emplace();
	shard.view_info->display_time = 1234;
	shard.timing_info.emplace();
	const auto capacity = shard_type::max_payload_size - serialized_size(shard) - 1;
	std::vector<uint8_t> data(capacity, 0x65);
	shard.payload = data;
	shard.unit = {3, 0, 1, uint32_t(capacity), uint16_t(capacity), true};
	to_headset::packets packet{shard};
	serialization_packet serialized;
	serialized.serialize(packet);
	const auto & spans = static_cast<std::vector<std::span<uint8_t>> &>(serialized);
	std::vector<uint8_t> wire;
	for (auto span: spans)
		wire.insert(wire.end(), span.begin(), span.end());
	if (wire.size() != shard_type::max_payload_size)
		throw std::runtime_error("Video packet exceeded its wire budget");
	deserialization_packet input({}, wire);
	auto decoded = input.deserialize<to_headset::packets>();
	const auto & recovered = std::get<shard_type>(decoded);
	if (recovered.unit != shard.unit || recovered.frame_idx != 42 ||
	    !recovered.view_info || recovered.view_info->display_time != 1234 ||
	    !recovered.timing_info || !std::ranges::equal(recovered.payload, data))
		throw std::runtime_error("Video packet round trip failed");
	from_headset::feedback feedback{};
	feedback.stream_corrupted = true;
	serialization_packet status;
	status.serialize(feedback);
	std::vector<uint8_t> status_wire;
	for (auto span: static_cast<std::vector<std::span<uint8_t>> &>(status))
		status_wire.insert(status_wire.end(), span.begin(), span.end());
	deserialization_packet status_input({}, status_wire);
	if (!status_input.deserialize<from_headset::feedback>().stream_corrupted)
		throw std::runtime_error("Corruption feedback round trip failed");
	std::cout << "Video protocol round trip and 1400-byte wire budget passed; source symbol " << capacity << " bytes\n";
}
