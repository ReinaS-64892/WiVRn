// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <vector>

namespace wivrn
{
inline constexpr size_t video_fec_group_size = 8;
inline constexpr size_t video_rs_group_size = 16;
inline constexpr size_t video_rs_max_repairs = 8;

// Protect the serialized source body, including optional first/last metadata.
// Source datagrams keep their original layout. Leave room for the larger repair
// header and the outer packet variant tag within the same 1400-byte wire budget.
inline size_t video_fec_symbol_capacity()
{
	const to_headset::video_stream_repair_shard repair{};
	return to_headset::video_stream_data_shard::max_payload_size - serialized_size(repair) - 1;
}

inline std::vector<uint8_t> video_fec_source_bytes(const to_headset::video_stream_data_shard & source)
{
	serialization_packet packet;
	packet.serialize(source);
	std::vector<uint8_t> bytes;
	bytes.reserve(serialized_size(source));
	for (auto span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		bytes.insert(bytes.end(), span.begin(), span.end());
	return bytes;
}

inline bool valid_video_repair(const to_headset::video_stream_repair_shard & repair)
{
	if (!repair.source_count || repair.source_count > video_rs_group_size ||
	    repair.scheme > 1 || !repair.repair_count || repair.repair_count > video_rs_max_repairs ||
	    repair.repair_index >= repair.repair_count ||
	    (!repair.scheme && (repair.source_count > video_fec_group_size || repair.repair_count != 1)) ||
	    size_t(repair.first_shard) + repair.source_count > size_t(UINT16_MAX) + 1 ||
	    repair.payload.empty() || repair.payload.size() > video_fec_symbol_capacity())
		return false;
	size_t maximum = 0;
	for (size_t i = 0; i < video_rs_group_size; ++i)
	{
		if (i < repair.source_count)
		{
			if (!repair.source_sizes[i] || repair.source_sizes[i] > repair.payload.size())
				return false;
			maximum = std::max<size_t>(maximum, repair.source_sizes[i]);
		}
		else if (repair.source_sizes[i])
			return false;
	}
	return maximum == repair.payload.size();
}

class video_fec_encoder
{
	to_headset::video_stream_repair_shard pending{};
	std::vector<uint8_t> parity;

public:
	// Call before the transport mutates source bytes during encryption. Flush at
	// eight packets or an encoder output-buffer boundary, without waiting for more
	// encoded data or changing source order. A short group uses zero padding.
	std::optional<to_headset::video_stream_repair_shard> push(const to_headset::video_stream_data_shard & source, bool flush)
	{
		const auto bytes = video_fec_source_bytes(source);
		if (bytes.size() > video_fec_symbol_capacity())
			throw std::runtime_error("Video FEC source exceeds wire budget");
		if (!pending.source_count)
		{
			pending = {};
			pending.stream_item_idx = source.stream_item_idx;
			pending.frame_idx = source.frame_idx;
			pending.first_shard = source.shard_idx;
			parity.clear();
		}
		if (source.stream_item_idx != pending.stream_item_idx || source.frame_idx != pending.frame_idx ||
		    source.shard_idx != size_t(pending.first_shard) + pending.source_count)
			throw std::runtime_error("Nonconsecutive video FEC sources");
		pending.source_sizes[pending.source_count++] = bytes.size();
		parity.resize(std::max(parity.size(), bytes.size()), 0);
		for (size_t i = 0; i < bytes.size(); ++i)
			parity[i] ^= bytes[i];
		if (!flush && pending.source_count < video_fec_group_size)
			return {};
		auto result = pending;
		result.data.c.reset(new uint8_t[parity.size()]);
		std::copy(parity.begin(), parity.end(), result.data.c.get());
		result.payload = {result.data.c.get(), parity.size()};
		pending.source_count = 0;
		return result;
	}
};

// Return a single missing source with its original metadata and an owned byte
// buffer. Never feed parity or an incomplete reconstruction into a decoder.
inline std::optional<to_headset::video_stream_data_shard> recover_video_packet(
        const to_headset::video_stream_repair_shard & repair,
        const std::vector<std::optional<to_headset::video_stream_data_shard>> & sources)
{
	if (!valid_video_repair(repair) || repair.scheme != 0)
		return {};
	std::optional<uint16_t> missing;
	const size_t end = size_t(repair.first_shard) + repair.source_count;
	for (size_t i = repair.first_shard; i < end; ++i)
	{
		if (i >= sources.size() || !sources[i])
		{
			if (missing)
				return {};
			missing = i;
		}
		else if (sources[i]->frame_idx != repair.frame_idx || sources[i]->stream_item_idx != repair.stream_item_idx ||
		         sources[i]->shard_idx != i || serialized_size(*sources[i]) != repair.source_sizes[i - repair.first_shard])
			return {};
	}
	if (!missing)
		return {};
	auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[repair.payload.size()]);
	std::copy(repair.payload.begin(), repair.payload.end(), memory.get());
	for (size_t i = repair.first_shard; i < end; ++i)
		if (i != *missing)
		{
			const auto bytes = video_fec_source_bytes(*sources[i]);
			for (size_t j = 0; j < bytes.size(); ++j)
				memory[j] ^= bytes[j];
		}
	try
	{
		const auto length = repair.source_sizes[*missing - repair.first_shard];
		deserialization_packet packet(memory, {memory.get(), length});
		auto result = packet.deserialize<to_headset::video_stream_data_shard>();
		if (result.frame_idx != repair.frame_idx || result.stream_item_idx != repair.stream_item_idx ||
		    result.shard_idx != *missing || serialized_size(result) != length)
			return {};
		return result;
	}
	catch (const std::exception &)
	{
		return {};
	}
}
} // namespace wivrn
