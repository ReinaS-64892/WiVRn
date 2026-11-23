// SPDX-License-Identifier: GPL-3.0-or-later
#include "shard_accumulator.h"
#include <algorithm>
#include <bit>
#include <spdlog/fmt/fmt.h>

namespace wivrn
{
void shard_accumulator::observe_packet(shard_set & shards, bool repair, uint16_t index, size_t bytes)
{
	auto generation = video_fec_diagnostics::get().recording_id();
	if (!generation)
		return;
	auto now = video_fec_diagnostics::now();
	auto & d = shards.diagnostics;
	if (d.generation != generation)
	{
		d = {};
		d.generation = generation;
		d.first_ns = now;
		d.partial_capture = !shards.empty();
	}
	if (d.last_ns)
		d.max_gap_ns = std::max(d.max_gap_ns, now - d.last_ns);
	d.last_ns = now;
	d.payload_bytes += bytes;
	if (repair)
		++d.repairs;
	else
	{
		++d.sources;
		d.duplicate_sources += index < shards.data.size() && shards.data[index].has_value();
		d.reordered_sources += d.previous_source && index < *d.previous_source;
		d.previous_source = index;
	}
}

void shard_accumulator::record_retirement(const shard_set & shards, bool complete, const char * reason)
{
	auto & recorder = video_fec_diagnostics::get();
	auto generation = recorder.recording_id();
	if (!generation)
		return;
	const auto now = video_fec_diagnostics::now();
	const auto & d = shards.diagnostics;
	const bool partial_capture = d.generation != generation || d.partial_capture;
	size_t expected = shards.data.size(), present = 0, xor_blocks = 0;
	std::array<size_t, 4> rs_blocks_by_repairs{};
	for (const auto & [first, symbols]: shards.repairs)
	{
		const auto & repair = symbols.front();
		expected = std::max(expected, size_t(first) + repair.source_count);
		if (repair.scheme == 1)
			++rs_blocks_by_repairs[repair.repair_count - 1];
		else
			++xor_blocks;
	}
	for (const auto & packet: shards.data)
		present += packet.has_value();
	const bool end_known = expected == shards.data.size() && !shards.data.empty() && shards.data.back() && shards.data.back()->timing_info.has_value();

	// Counts are exhaustive. Only the detailed lists are capped for bounded logs.
	std::string missing = "[", groups = "[";
	size_t missing_listed = 0, groups_listed = 0, without_repair = 0, multiple_groups = 0, single_groups = 0;
	for (size_t i = 0; i < expected; ++i)
	{
		if (i < shards.data.size() && shards.data[i])
			continue;
		if (missing_listed < 64)
		{
			missing += fmt::format("{}{}", missing_listed ? "," : "", i);
			++missing_listed;
		}
		auto group = shards.repairs.upper_bound(i);
		bool covered = false;
		if (group != shards.repairs.begin())
		{
			--group;
			covered = i < size_t(group->first) + group->second.front().source_count;
		}
		without_repair += !covered;
	}
	size_t over_capacity = 0, insufficient_repairs = 0, recoverable_unrecovered = 0;
	for (const auto & [first, symbols]: shards.repairs)
	{
		const auto & repair = symbols.front();
		unsigned mask = 0;
		for (size_t i = 0; i < repair.source_count; ++i)
			if (size_t(first) + i >= shards.data.size() || !shards.data[first + i])
				mask |= 1u << i;
		if (!mask)
			continue;
		const auto holes = size_t(std::popcount(mask));
		over_capacity += holes > repair.repair_count;
		insufficient_repairs += holes <= repair.repair_count && holes > symbols.size();
		recoverable_unrecovered += holes <= symbols.size();
		const bool single = holes == 1;
		single_groups += single;
		multiple_groups += !single;
		if (groups_listed < 64)
		{
			unsigned repair_mask = 0;
			for (const auto & symbol: symbols)
				repair_mask |= 1u << symbol.repair_index;
			groups += fmt::format("{}{{\"first\":{},\"count\":{},\"missing_mask\":{},\"scheme\":{},\"repair_count\":{},\"repairs_present\":{},\"repair_mask\":{}}}", groups_listed ? "," : "", first, repair.source_count, mask, repair.scheme, repair.repair_count, symbols.size(), repair_mask);
			++groups_listed;
		}
	}
	missing += ']';
	groups += ']';
	recorder.record(generation, fmt::format("{{\"kind\":\"frame\",\"mono_ns\":{},\"receiver\":{},\"stream\":{},\"frame\":{},\"complete\":{},\"reason\":\"{}\",\"partial_capture\":{},\"sources_rx\":{},\"repairs_rx\":{},\"duplicate_sources\":{},\"reordered_sources\":{},\"payload_bytes\":{},\"recovered\":{},\"present\":{},\"expected_min\":{},\"end_known\":{},\"missing_min\":{},\"missing_without_repair\":{},\"multiple_missing_groups\":{},\"unrecovered_single_groups\":{},\"repair_groups\":{},\"xor_blocks\":{},\"rs_blocks_by_repairs\":[{},{},{},{}],\"over_capacity_groups\":{},\"insufficient_repair_groups\":{},\"recoverable_unrecovered_groups\":{},\"missing\":{},\"damaged_groups\":{},\"lists_truncated\":{},\"first_ns\":{},\"last_ns\":{},\"max_gap_ns\":{},\"receive_span_ns\":{},\"age_ns\":{}}}", now, diagnostics_id, shards.feedback.stream_index, shards.frame_index(), complete, reason, partial_capture, d.generation == generation ? d.sources : 0, d.generation == generation ? d.repairs : 0, d.generation == generation ? d.duplicate_sources : 0, d.generation == generation ? d.reordered_sources : 0, d.generation == generation ? d.payload_bytes : 0, shards.recovered_packets, present, expected, end_known, expected - present, without_repair, multiple_groups, single_groups, shards.repairs.size(), xor_blocks, rs_blocks_by_repairs[0], rs_blocks_by_repairs[1], rs_blocks_by_repairs[2], rs_blocks_by_repairs[3], over_capacity, insufficient_repairs, recoverable_unrecovered, missing, groups, missing_listed < expected - present || groups_listed < multiple_groups + single_groups, d.generation == generation ? d.first_ns : 0, d.generation == generation ? d.last_ns : 0, d.generation == generation ? d.max_gap_ns : 0, d.generation == generation ? d.last_ns - d.first_ns : 0, d.generation == generation ? now - d.first_ns : 0));
	if (!complete || shards.recovered_packets)
	{
		recently_damaged.push_back({shards.frame_index(), generation, now, complete});
		if (recently_damaged.size() > 32)
			recently_damaged.pop_front();
	}
}

void shard_accumulator::record_late(uint64_t frame, bool repair, uint16_t index, uint8_t count, uint8_t scheme, uint8_t repair_index, uint8_t repair_count)
{
	auto & recorder = video_fec_diagnostics::get();
	auto generation = recorder.recording_id();
	if (!generation)
		return;
	for (const auto & retired: recently_damaged)
		if (retired.generation == generation && retired.frame == frame)
		{
			auto now = video_fec_diagnostics::now();
			recorder.record(generation, fmt::format("{{\"kind\":\"late\",\"mono_ns\":{},\"receiver\":{},\"frame\":{},\"complete_at_retire\":{},\"repair\":{},\"index\":{},\"source_count\":{},\"scheme\":{},\"repair_index\":{},\"repair_count\":{},\"after_retire_ns\":{}}}", now, diagnostics_id, frame, retired.complete, repair, index, count, scheme, repair_index, repair_count, now - retired.retired_ns));
			break;
		}
}
} // namespace wivrn
