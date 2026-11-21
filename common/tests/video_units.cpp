// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_units.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace wivrn;

static void require(bool condition)
{
	if (!condition)
		throw std::runtime_error("video unit regression");
}

struct shard
{
	video_unit_info unit;
	std::span<uint8_t> payload;
};

int main()
{
	std::vector<uint8_t> bytes{0, 0, 0, 1, 0x67, 2, 0, 0, 1, 0x65, 3, 4, 0, 0, 0, 1, 0x41, 5};
	auto units = annex_b_units(bytes);
	require(units.size() == 3);
	require(units[0].size() == 6 && units[1].size() == 6 && units[2].size() == 6);
	require(annex_b_prefix(units[0]) == 4 && annex_b_prefix(units[1]) == 3);
	require(annex_b_units({}).empty());
	std::vector<uint8_t> opaque{42, 43};
	require(annex_b_units(opaque).size() == 1);

	// Three units, each in three symbols; metadata and two independently
	// decodable slices. Arrival order and duplicates cannot affect the selection.
	std::vector<std::optional<shard>> frame(9);
	for (unsigned i: {8, 2, 1, 5, 0, 7, 4, 6, 3, 3})
		frame[i] = shard{video_unit_info{uint16_t(i / 3), uint16_t(i / 3 * 3), 3, 6, 2, i >= 3}, std::span(bytes).subspan(i * 2, 2)};
	auto all = complete_video_units(frame);
	require(all.spans.size() == 9 && all.has_vcl);

	// Losing any symbol drops that entire unit, retaining all other units.
	for (size_t i = 0; i < frame.size(); ++i)
	{
		auto damaged = frame;
		damaged[i].reset();
		auto selected = complete_video_units(damaged);
		require(selected.spans.size() == 6 && selected.has_vcl);
		for (const auto & span: selected.spans)
			require(span.data() < bytes.data() + (i / 3) * 6 || span.data() >= bytes.data() + (i / 3 + 1) * 6);
	}
	// Entire missing NAL, missing final frame packet, and multiple damaged NALs.
	auto damaged = frame;
	for (int i: {3, 4, 5, 8})
		damaged[i].reset();
	require(complete_video_units(damaged).spans.size() == 3);
	require(!complete_video_units(damaged).has_vcl);

	// Inconsistent descriptors, truncated bytes, invalid ranges never reach a decoder.
	for (int mode = 0; mode < 4; ++mode)
	{
		auto invalid = frame;
		if (mode == 0)
			invalid[4]->unit.byte_count++;
		if (mode == 1)
			invalid[4]->payload = invalid[4]->payload.first(1);
		if (mode == 2)
			invalid[3]->unit.shard_count = 0;
		if (mode == 3)
			invalid[3]->unit.shard_count = 65535;
		require(complete_video_units(invalid).spans.size() == 6);
	}
	std::cout << "video unit loss/reordering/boundary tests passed\n";
}
