// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_fec.h"

namespace wivrn
{
namespace video_rs
{
// GF(256), polynomial x^8+x^4+x^3+x^2+1 (0x11d), primitive element 2.
struct field_tables
{
	std::array<uint8_t, 512> exp{};
	std::array<uint8_t, 256> log{};
	constexpr field_tables()
	{
		unsigned value = 1;
		for (unsigned i = 0; i < 255; ++i)
		{
			exp[i] = value;
			log[value] = i;
			value <<= 1;
			if (value & 256)
				value ^= 0x11d;
		}
		for (size_t i = 255; i < exp.size(); ++i)
			exp[i] = exp[i - 255];
	}
};
inline constexpr field_tables field{};
inline uint8_t multiply(uint8_t a, uint8_t b)
{
	return a && b ? field.exp[unsigned(field.log[a]) + field.log[b]] : 0;
}
inline uint8_t inverse(uint8_t a)
{
	if (!a)
		throw std::runtime_error("Singular video RS matrix");
	return field.exp[255 - field.log[a]];
}
// Systematic generator [I; C], with C[r,c] = 1/(x[r]+y[c]).
// Disjoint x=16..23 and y=0..15 make every square Cauchy submatrix
// invertible: any K of K+R wire symbols recover the K original packets.
inline uint8_t coefficient(size_t repair, size_t source)
{
	return inverse(uint8_t((16 + repair) ^ source));
}
} // namespace video_rs

inline std::vector<to_headset::video_stream_repair_shard> encode_video_rs(
        std::span<const to_headset::video_stream_data_shard> sources,
        uint8_t repair_count)
{
	if (sources.empty() || sources.size() > video_rs_group_size || !repair_count || repair_count > video_rs_max_repairs)
		throw std::runtime_error("Invalid video RS block");
	to_headset::video_stream_repair_shard header{};
	header.stream_item_idx = sources.front().stream_item_idx;
	header.frame_idx = sources.front().frame_idx;
	header.first_shard = sources.front().shard_idx;
	header.source_count = sources.size();
	header.scheme = 1;
	header.repair_count = repair_count;
	std::vector<std::vector<uint8_t>> bytes;
	size_t length = 0;
	for (size_t i = 0; i < sources.size(); ++i)
	{
		if (sources[i].frame_idx != header.frame_idx || sources[i].stream_item_idx != header.stream_item_idx ||
		    size_t(header.first_shard) + i != sources[i].shard_idx)
			throw std::runtime_error("Nonconsecutive video RS sources");
		bytes.push_back(video_fec_source_bytes(sources[i]));
		if (bytes.back().size() > video_fec_symbol_capacity())
			throw std::runtime_error("Video RS source exceeds wire budget");
		header.source_sizes[i] = bytes.back().size();
		length = std::max(length, bytes.back().size());
	}
	std::vector<to_headset::video_stream_repair_shard> result;
	for (size_t r = 0; r < repair_count; ++r)
	{
		auto repair = header;
		repair.repair_index = r;
		repair.data.c.reset(new uint8_t[length]{});
		repair.payload = {repair.data.c.get(), length};
		for (size_t i = 0; i < bytes.size(); ++i)
		{
			const auto factor = video_rs::coefficient(r, i);
			for (size_t j = 0; j < bytes[i].size(); ++j)
				repair.payload[j] ^= video_rs::multiply(factor, bytes[i][j]);
		}
		result.push_back(std::move(repair));
	}
	return result;
}

inline bool same_video_repair_block(const to_headset::video_stream_repair_shard & a,
                                    const to_headset::video_stream_repair_shard & b)
{
	return a.stream_item_idx == b.stream_item_idx && a.frame_idx == b.frame_idx && a.first_shard == b.first_shard &&
	       a.source_count == b.source_count && a.scheme == b.scheme && a.repair_count == b.repair_count &&
	       a.source_sizes == b.source_sizes && a.payload.size() == b.payload.size();
}

// Return all recovered packets atomically, or none. Never expose a partially
// validated reconstruction to the incremental video decoder.
inline std::vector<to_headset::video_stream_data_shard> recover_video_packets(
        std::span<const to_headset::video_stream_repair_shard> repairs,
        const std::vector<std::optional<to_headset::video_stream_data_shard>> & sources)
{
	if (repairs.empty() || !valid_video_repair(repairs.front()))
		return {};
	const auto & header = repairs.front();
	if (!header.scheme)
	{
		if (auto restored = recover_video_packet(header, sources))
			return {std::move(*restored)};
		return {};
	}
	std::array<bool, video_rs_max_repairs> seen{};
	for (const auto & repair: repairs)
	{
		if (!valid_video_repair(repair) || !same_video_repair_block(header, repair) || seen[repair.repair_index])
			return {};
		seen[repair.repair_index] = true;
	}
	std::vector<size_t> missing;
	for (size_t i = 0; i < header.source_count; ++i)
	{
		const auto index = size_t(header.first_shard) + i;
		if (index >= sources.size() || !sources[index])
			missing.push_back(i);
		else if (sources[index]->frame_idx != header.frame_idx || sources[index]->stream_item_idx != header.stream_item_idx ||
		         sources[index]->shard_idx != index || serialized_size(*sources[index]) != header.source_sizes[i])
			return {};
	}
	const size_t n = missing.size();
	if (!n || n > repairs.size())
		return {};
	std::array<std::array<uint8_t, 2 * video_rs_max_repairs>, video_rs_max_repairs> matrix{};
	std::vector<std::vector<uint8_t>> rhs;
	for (size_t r = 0; r < n; ++r)
	{
		for (size_t c = 0; c < n; ++c)
			matrix[r][c] = video_rs::coefficient(repairs[r].repair_index, missing[c]);
		matrix[r][n + r] = 1;
		rhs.emplace_back(repairs[r].payload.begin(), repairs[r].payload.end());
	}
	for (size_t i = 0; i < header.source_count; ++i)
	{
		const auto index = size_t(header.first_shard) + i;
		if (index >= sources.size() || !sources[index])
			continue;
		const auto bytes = video_fec_source_bytes(*sources[index]);
		for (size_t r = 0; r < n; ++r)
		{
			const auto factor = video_rs::coefficient(repairs[r].repair_index, i);
			for (size_t j = 0; j < bytes.size(); ++j)
				rhs[r][j] ^= video_rs::multiply(factor, bytes[j]);
		}
	}
	// Gauss-Jordan inversion, at most 8x8. All missing positions are known.
	for (size_t c = 0; c < n; ++c)
	{
		size_t pivot = c;
		while (pivot < n && !matrix[pivot][c])
			++pivot;
		if (pivot == n)
			return {};
		std::swap(matrix[c], matrix[pivot]);
		const auto scale = video_rs::inverse(matrix[c][c]);
		for (size_t j = 0; j < 2 * n; ++j)
			matrix[c][j] = video_rs::multiply(matrix[c][j], scale);
		for (size_t r = 0; r < n; ++r)
			if (r != c)
			{
				const auto factor = matrix[r][c];
				for (size_t j = 0; j < 2 * n; ++j)
					matrix[r][j] ^= video_rs::multiply(factor, matrix[c][j]);
			}
	}
	std::vector<to_headset::video_stream_data_shard> result;
	try
	{
		for (size_t c = 0; c < n; ++c)
		{
			auto memory = std::shared_ptr<uint8_t[]>(new uint8_t[header.payload.size()]{});
			for (size_t r = 0; r < n; ++r)
				for (size_t j = 0; j < header.payload.size(); ++j)
					memory[j] ^= video_rs::multiply(matrix[c][n + r], rhs[r][j]);
			const auto length = header.source_sizes[missing[c]];
			if (!std::all_of(memory.get() + length, memory.get() + header.payload.size(), [](uint8_t v) { return v == 0; }))
				return {};
			deserialization_packet packet(memory, {memory.get(), length});
			auto restored = packet.deserialize<to_headset::video_stream_data_shard>();
			if (restored.frame_idx != header.frame_idx || restored.stream_item_idx != header.stream_item_idx ||
			    restored.shard_idx != size_t(header.first_shard) + missing[c] || serialized_size(restored) != length)
				return {};
			result.push_back(std::move(restored));
		}
	}
	catch (const std::exception &)
	{
		return {};
	}
	return result;
}
} // namespace wivrn
