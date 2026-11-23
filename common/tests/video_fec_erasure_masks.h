// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <numeric>
#include <random>
#include <set>
#include <vector>

namespace video_fec_test
{
// Independent channel-loss patterns, shared by the codec and real receiver
// tests. Full enumeration is practical for R<=4; larger budgets use every
// contiguous burst, every erased-repair subset and deterministic random data
// erasures, including repeated maximum-rank recovery with all repairs intact.
inline std::vector<unsigned> erasure_masks(size_t sources, unsigned repairs, bool exhaustive)
{
	const auto total = sources + repairs;
	if (exhaustive)
	{
		std::vector<unsigned> result;
		for (unsigned mask = 0; mask < (1u << total); ++mask)
			if (unsigned(std::popcount(mask)) <= repairs)
				result.push_back(mask);
		return result;
	}
	std::set<unsigned> result{0};
	for (unsigned count = 1; count <= repairs; ++count)
		for (size_t first = 0; first + count <= total; ++first)
			result.insert(((1u << count) - 1) << first);
	std::mt19937 random(0x52530000u + sources * 31 + repairs);
	std::vector<unsigned> positions(sources);
	std::iota(positions.begin(), positions.end(), 0);
	auto add = [&](unsigned data_losses, unsigned repair_mask) {
		std::shuffle(positions.begin(), positions.end(), random);
		unsigned mask = repair_mask << sources;
		for (unsigned i = 0; i < data_losses; ++i)
			mask |= 1u << positions[i];
		result.insert(mask);
	};
	for (unsigned repair_mask = 0; repair_mask < (1u << repairs); ++repair_mask)
		for (unsigned count = 0; count <= std::min(sources, size_t(repairs - std::popcount(repair_mask))); ++count)
			add(count, repair_mask);
	for (unsigned count = 1; count <= std::min(sources, size_t(repairs)); ++count)
		for (unsigned sample = 0; sample < 64; ++sample)
			add(count, 0);
	return {result.begin(), result.end()};
}
} // namespace video_fec_test
