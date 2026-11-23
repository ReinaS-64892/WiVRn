// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder/shard_accumulator.h"
#include "scenes/stream.h"
#include "tests/video_fec_erasure_masks.h"
#include "video_fec_interleave.h"
#include "xr/instance.h"

#include <bit>
#include <fstream>
#include <iostream>
#include <numeric>
#include <simdjson.h>
#include <source_location>
#include <stdexcept>
#include <unistd.h>

namespace
{
class recording_decoder : public wivrn::decoder
{
	uint64_t frame = UINT64_MAX;
	std::vector<uint8_t> bytes;

public:
	std::vector<std::vector<uint8_t>> completed;
	void push_data(std::span<std::span<const uint8_t>> spans, uint64_t index, bool) override
	{
		if (frame != index)
		{
			bytes.clear();
			frame = index;
		}
		for (auto span: spans)
			bytes.insert(bytes.end(), span.begin(), span.end());
	}
	void frame_completed(const wivrn::from_headset::feedback & feedback, const wivrn::to_headset::video_stream_data_shard::view_info_t & view) override
	{
		if (feedback.frame_index != frame || view.display_time != 12345)
			throw std::runtime_error("Incorrect recovered frame metadata");
		completed.push_back(std::move(bytes));
		bytes.clear();
	}
	vk::Sampler sampler() override
	{
		return nullptr;
	}
};
std::shared_ptr<recording_decoder> recording;
void require(bool condition, std::source_location where = std::source_location::current())
{
	if (!condition)
		throw std::runtime_error("Video FEC receiver regression at line " + std::to_string(where.line()));
}
wivrn::to_headset::video_stream_data_shard source(uint64_t frame, uint16_t index, uint16_t count = 6)
{
	static auto data = [] {
		std::array<uint8_t, 64> bytes{};
		for (size_t i = 0; i < bytes.size(); ++i)
			bytes[i] = i;
		return bytes;
	}();
	wivrn::to_headset::video_stream_data_shard s{};
	s.frame_idx = frame;
	s.shard_idx = index;
	s.payload = std::span(data).subspan(index, 1);
	if (!index)
	{
		s.view_info.emplace();
		s.view_info->display_time = 12345;
	}
	if (index + 1 == count)
		s.timing_info.emplace();
	return s;
}
wivrn::to_headset::video_stream_repair_shard parity(uint64_t frame)
{
	wivrn::video_fec_encoder encoder;
	std::optional<wivrn::to_headset::video_stream_repair_shard> result;
	for (int i = 0; i < 6; ++i)
		result = encoder.push(source(frame, i), i == 5);
	return std::move(*result);
}
} // namespace
wivrn::decoder::~decoder() = default;
std::shared_ptr<wivrn::decoder> wivrn::decoder::make(vk::raii::Device &, vk::raii::PhysicalDevice &, uint32_t, const to_headset::video_stream_description &, uint8_t, std::weak_ptr<scenes::stream>, shard_accumulator *)
{
	return recording;
}
XrTime xr::instance::now()
{
	return 1000000;
}
void scenes::stream::send_feedback(const wivrn::from_headset::feedback &) {}

int main()
{
	auto & recorder = wivrn::video_fec_diagnostics::get();
	require(recorder.start(std::filesystem::temp_directory_path() / ("wivrn-fec-receiver-" + std::to_string(getpid()))));
	vk::raii::Device device(nullptr);
	vk::raii::PhysicalDevice physical(nullptr);
	xr::instance instance;
	wivrn::to_headset::video_stream_description description{};
	for (int missing = 0; missing < 6; ++missing)
		for (bool repair_first: {false, true})
		{
			recording = std::make_shared<recording_decoder>();
			wivrn::shard_accumulator receiver(device, physical, instance, 0, description, {}, 0);
			if (repair_first)
				receiver.push_repair(parity(100));
			for (int i = 5; i >= 0; --i)
				if (i != missing)
					receiver.push_shard(source(100, i));
			if (!repair_first)
				receiver.push_repair(parity(100));
			require(recording->completed.size() == 1 && recording->completed[0] == std::vector<uint8_t>({0, 1, 2, 3, 4, 5}));
			receiver.push_repair(parity(100));
			receiver.push_shard(source(100, missing));
			require(recording->completed.size() == 1);
			auto stats = receiver.take_statistics();
			require(stats.source_packets == 6 && stats.repair_packets == 2);
			require(stats.recovered_packets == 1 && stats.completed_frames == 1 && stats.rescued_frames == 1);
			require(stats.incomplete_frames == 0);
			stats = receiver.take_statistics();
			require(stats.source_packets == 0 && stats.repair_packets == 0 && stats.recovered_packets == 0);
			require(stats.completed_frames == 0 && stats.rescued_frames == 0 && stats.incomplete_frames == 0);
		}
	for (bool with_repair: {false, true})
	{
		recording = std::make_shared<recording_decoder>();
		wivrn::shard_accumulator receiver(device, physical, instance, 0, description, {}, 0);
		for (int i: {0, 1, 4, 5})
			receiver.push_shard(source(100, i));
		if (with_repair)
			receiver.push_repair(parity(100));
		require(recording->completed.empty()); // Two losses remain unrepairable.
		for (int i = 0; i < 6; ++i)
			receiver.push_shard(source(101, i));
		require(recording->completed.size() == 1 && recording->completed[0] == std::vector<uint8_t>({0, 1, 2, 3, 4, 5}));
		receiver.push_repair(parity(100));
		require(recording->completed.size() == 1);
		receiver.push_shard(source(100, 2));
		auto stats = receiver.take_statistics();
		require(stats.source_packets == 11 && stats.repair_packets == (with_repair ? 2 : 1));
		require(stats.recovered_packets == 0 && stats.completed_frames == 1 && stats.rescued_frames == 0);
		require(stats.incomplete_frames == 1); // Exclude unobserved startup frame IDs.
	}
	// Independent groups can recover out of order, while incremental decoder
	// input must still be delivered once and in source order.
	for (bool lose_sources: {false, true})
	{
		recording = std::make_shared<recording_decoder>();
		wivrn::shard_accumulator grouped_receiver(device, physical, instance, 0, description, {}, 0);
		wivrn::video_fec_encoder encoder;
		std::vector<wivrn::to_headset::video_stream_repair_shard> repairs;
		for (uint16_t i = 0; i < 19; ++i)
		{
			if (auto repair = encoder.push(source(200, i, 19), i == 18))
				repairs.push_back(std::move(*repair));
			if (!lose_sources || i % 8 != 2)
				grouped_receiver.push_shard(source(200, i, 19));
		}
		for (auto it = repairs.rbegin(); it != repairs.rend(); ++it)
			grouped_receiver.push_repair(std::move(*it));
		std::vector<uint8_t> expected(19);
		for (size_t i = 0; i < expected.size(); ++i)
			expected[i] = i;
		require(recording->completed.size() == 1 && recording->completed[0] == expected);
		auto stats = grouped_receiver.take_statistics();
		require(stats.completed_frames == 1 && stats.incomplete_frames == 0);
		require(stats.recovered_packets == (lose_sources ? 3 : 0));
		require(stats.rescued_frames == (lose_sources ? 1 : 0));
	}
	// A real receiver must recover each 1..4-packet burst at every position of
	// two consecutive full windows, including the boundary and parity packets.
	std::vector<wivrn::to_headset::packets> interleaved;
	for (uint16_t first: {0, 32})
	{
		std::vector<wivrn::to_headset::video_stream_data_shard> originals;
		for (uint16_t i = first; i < first + 32; ++i)
			originals.push_back(source(300, i, 64));
		wivrn::send_interleaved_video(originals, [&](const auto & s) { interleaved.emplace_back(s); }, [&](auto repair) { interleaved.emplace_back(std::move(repair)); });
	}
	require(interleaved.size() == 72);
	for (size_t burst = 1; burst <= 4; ++burst)
		for (size_t start = 0; start + burst <= interleaved.size(); ++start)
		{
			recording = std::make_shared<recording_decoder>();
			wivrn::shard_accumulator burst_receiver(device, physical, instance, 0, description, {}, 0);
			for (size_t i = 0; i < interleaved.size(); ++i)
			{
				if (i >= start && i < start + burst)
					continue;
				if (auto s = std::get_if<wivrn::to_headset::video_stream_data_shard>(&interleaved[i]))
					burst_receiver.push_shard(wivrn::to_headset::video_stream_data_shard(*s));
				else
					burst_receiver.push_repair(wivrn::to_headset::video_stream_repair_shard(std::get<wivrn::to_headset::video_stream_repair_shard>(interleaved[i])));
			}
			std::vector<uint8_t> expected(64);
			for (size_t i = 0; i < expected.size(); ++i)
				expected[i] = i;
			require(recording->completed.size() == 1 && recording->completed[0] == expected);
		}
	// Both frame slots can wait for independent repairs; completing the next
	// frame retains the original policy of retiring an incomplete predecessor.
	recording = std::make_shared<recording_decoder>();
	wivrn::shard_accumulator receiver(device, physical, instance, 0, description, {}, 0);
	for (int i: {0, 1, 3, 4, 5})
		receiver.push_shard(source(100, i));
	for (int i: {0, 1, 2, 3, 4})
		receiver.push_shard(source(101, i));
	receiver.push_repair(parity(100));
	receiver.push_repair(parity(101));
	require(recording->completed.size() == 2);
	auto stats = receiver.take_statistics();
	require(stats.completed_frames == 2 && stats.rescued_frames == 2 && stats.recovered_packets == 2 && stats.incomplete_frames == 0);
	// Recovery in one group need not rescue a frame with losses in another.
	recording = std::make_shared<recording_decoder>();
	wivrn::shard_accumulator partial(device, physical, instance, 0, description, {}, 0);
	wivrn::video_fec_encoder encoder;
	for (uint16_t i = 0; i < 16; ++i)
	{
		auto repair = encoder.push(source(400, i, 16), i == 15);
		if (i != 2 && i != 9 && i != 10)
			partial.push_shard(source(400, i, 16));
		if (repair)
			partial.push_repair(std::move(*repair));
	}
	for (uint16_t i = 0; i < 6; ++i)
		partial.push_shard(source(401, i));
	stats = partial.take_statistics();
	require(stats.recovered_packets == 1 && stats.rescued_frames == 0);
	require(stats.completed_frames == 1 && stats.incomplete_frames == 1);
	// Invalid repair packets are excluded from the valid repair receive count.
	auto invalid = parity(402);
	invalid.source_count = 0;
	partial.push_repair(std::move(invalid));
	require(partial.take_statistics().repair_packets == 0);
	// A syntactically valid repair with inconsistent source length must remain
	// distinguishable from two losses in a group.
	recording = std::make_shared<recording_decoder>();
	wivrn::shard_accumulator mismatch(device, physical, instance, 0, description, {}, 0);
	for (int i: {0, 1, 3, 4, 5})
		mismatch.push_shard(source(500, i));
	auto broken = parity(500);
	++broken.source_sizes[2];
	mismatch.push_repair(std::move(broken));
	for (int i = 0; i < 6; ++i)
		mismatch.push_shard(source(501, i));
	// Missing end metadata means the number of absent tail packets is unknown.
	mismatch.push_shard(source(600, 0));
	for (int i = 0; i < 6; ++i)
		mismatch.push_shard(source(601, i));
	recorder.stop();
	require(recorder.snapshot().error.empty() && !recorder.snapshot().lost_events);
	std::ifstream log(recorder.snapshot().path);
	std::string line;
	simdjson::dom::parser parser;
	size_t rescued = 0, incomplete = 0, late_sources = 0, late_repairs = 0;
	while (std::getline(log, line))
	{
		auto json = parser.parse(line);
		auto kind = json["kind"].get_string().value();
		if (kind == "frame")
		{
			if (json["complete"].get_bool().value())
				rescued += json["recovered"].get_uint64().value() > 0;
			else
			{
				++incomplete;
				if (json["frame"].get_uint64().value() == 500)
				{
					require(json["missing_min"].get_uint64().value() == 1);
					require(json["unrecovered_single_groups"].get_uint64().value() == 1);
					continue;
				}
				if (json["frame"].get_uint64().value() == 600)
				{
					require(!json["end_known"].get_bool().value());
					require(json["missing_min"].get_uint64().value() == 0);
					continue;
				}
				require(json["missing_min"].get_uint64().value() == 2);
				if (json["repair_groups"].get_uint64().value())
				{
					require(json["multiple_missing_groups"].get_uint64().value() == 1);
					require(json["missing_without_repair"].get_uint64().value() == 0);
				}
				else
					require(json["missing_without_repair"].get_uint64().value() == 2);
			}
		}
		else if (kind == "late")
		{
			if (json["complete_at_retire"].get_bool().value())
				continue;
			if (json["repair"].get_bool().value())
				++late_repairs;
			else
				++late_sources;
		}
	}
	require(rescued > 12 && incomplete == 5 && late_sources == 2 && late_repairs == 2);
	// Starting in the middle of a frame must flag incomplete capture rather
	// than presenting its receive counters as a complete observation.
	mismatch.push_shard(source(700, 0));
	require(recorder.start(std::filesystem::temp_directory_path() / ("wivrn-fec-receiver-" + std::to_string(getpid()))));
	for (int i = 1; i < 6; ++i)
		mismatch.push_shard(source(700, i));
	recorder.stop();
	std::ifstream mid_frame(recorder.snapshot().path);
	bool found_partial_capture = false;
	while (std::getline(mid_frame, line))
	{
		auto json = parser.parse(line);
		if (json["kind"].get_string().value() == "frame")
		{
			require(json["partial_capture"].get_bool().value());
			require(json["sources_rx"].get_uint64().value() == 5);
			require(json["present"].get_uint64().value() == 6);
			found_partial_capture = true;
		}
	}
	require(found_partial_capture);
	// Real accumulator/decoder integration: exhaustive budgets up to four and
	// sampled larger budgets, including first/last metadata, arbitrary parity IDs,
	// repair-first arrival, duplicates and conflicting block metadata.
	for (const auto & [count, r]: std::vector<std::pair<uint8_t, uint8_t>>{{4, 2}, {8, 2}, {8, 4}, {16, 2}, {16, 4}, {16, 5}, {16, 6}, {16, 7}, {16, 8}})
	{
		std::vector<wivrn::to_headset::video_stream_data_shard> originals;
		for (uint16_t i = 0; i < count; ++i)
			originals.push_back(source(800, i, count));
		auto repairs = wivrn::encode_video_rs(originals, r);
		for (unsigned mask: video_fec_test::erasure_masks(count, r, r <= 4))
		{
			for (bool repair_first: {false, true})
			{
				recording = std::make_shared<recording_decoder>();
				wivrn::shard_accumulator rs_receiver(device, physical, instance, 0, description, {}, 0);
				auto send_repairs = [&] {
					for (int i = r - 1; i >= 0; --i)
						if (!(mask & (1u << (count + i))))
						{
							rs_receiver.push_repair(wivrn::to_headset::video_stream_repair_shard(repairs[i]));
							rs_receiver.push_repair(wivrn::to_headset::video_stream_repair_shard(repairs[i]));
							auto conflict = repairs[i];
							--conflict.source_sizes[0];
							rs_receiver.push_repair(std::move(conflict));
						}
				};
				if (repair_first)
					send_repairs();
				for (int i = count - 1; i >= 0; --i)
					if (!(mask & (1u << i)))
						rs_receiver.push_shard(source(800, i, count));
				if (!repair_first)
					send_repairs();
				require(recording->completed.size() == 1);
				std::vector<uint8_t> expected(count);
				std::iota(expected.begin(), expected.end(), 0);
				require(recording->completed[0] == expected);
				const auto stats = rs_receiver.take_statistics();
				require(stats.completed_frames == 1 && stats.incomplete_frames == 0);
				if (!repair_first)
					require(stats.recovered_packets == unsigned(std::popcount(mask & ((1u << count) - 1))));
			}
		}
	}
	// Full 64-source windows with smaller blocks and deeper interleaving must
	// reconstruct the frame in order through the actual decoder submission path.
	struct interleave_configuration
	{
		uint8_t sources, repairs, depth;
	};
	for (auto config: {interleave_configuration{4, 2, 16}, {8, 4, 8}, {8, 2, 8}})
	{
		std::vector<wivrn::to_headset::video_stream_data_shard> originals;
		for (uint16_t i = 0; i < 64; ++i)
			originals.push_back(source(850, i, 64));
		std::vector<wivrn::to_headset::packets> wire;
		auto emit = [&](auto packet) { wire.emplace_back(std::move(packet)); };
		wivrn::send_interleaved_video(originals, emit, emit, config.repairs, config.sources, config.depth);
		for (size_t burst = 1; burst <= size_t(config.repairs) * config.depth; ++burst)
			for (size_t first = 0; first + burst <= wire.size(); ++first)
			{
				recording = std::make_shared<recording_decoder>();
				wivrn::shard_accumulator rs_receiver(device, physical, instance, 0, description, {}, 0);
				for (size_t i = 0; i < wire.size(); ++i)
				{
					if (i >= first && i < first + burst)
						continue;
					if (auto packet = std::get_if<wivrn::to_headset::video_stream_data_shard>(&wire[i]))
						rs_receiver.push_shard(wivrn::to_headset::video_stream_data_shard(*packet));
					else
						rs_receiver.push_repair(wivrn::to_headset::video_stream_repair_shard(std::get<wivrn::to_headset::video_stream_repair_shard>(wire[i])));
				}
				require(recording->completed.size() == 1);
				std::vector<uint8_t> expected(64);
				std::iota(expected.begin(), expected.end(), 0);
				require(recording->completed[0] == expected);
				require(rs_receiver.take_statistics().incomplete_frames == 0);
			}
	}
	// Distinguish a code-capacity failure from missing repair packets in logs.
	require(recorder.start(std::filesystem::temp_directory_path() / ("wivrn-fec-receiver-" + std::to_string(getpid()))));
	for (uint8_t r: {2, 8})
		for (bool over_capacity: {false, true})
		{
			recording = std::make_shared<recording_decoder>();
			wivrn::shard_accumulator rs_receiver(device, physical, instance, 0, description, {}, 0);
			std::vector<wivrn::to_headset::video_stream_data_shard> originals;
			for (uint16_t i = 0; i < 16; ++i)
				originals.push_back(source(900, i, 16));
			auto repairs = wivrn::encode_video_rs(originals, r);
			auto late_repair = repairs[r - 1];
			for (int i = over_capacity ? r + 1 : r; i < 16; ++i)
				rs_receiver.push_shard(source(900, i, 16));
			for (int i = r - 1; i >= (over_capacity ? 0 : 1); --i)
				rs_receiver.push_repair(std::move(repairs[i]));
			require(recording->completed.empty());
			for (uint16_t i = 0; i < 16; ++i)
				rs_receiver.push_shard(source(901, i, 16));
			require(rs_receiver.take_statistics().incomplete_frames == 1);
			rs_receiver.push_repair(std::move(late_repair));
		}
	recorder.stop();
	std::ifstream rs_log(recorder.snapshot().path);
	size_t capacity_failures = 0, repair_failures = 0, rs_late = 0;
	while (std::getline(rs_log, line))
	{
		auto json = parser.parse(line);
		if (json["kind"].get_string().value() == "late")
		{
			require(json["scheme"].get_uint64().value() == 1);
			const auto count = json["repair_count"].get_uint64().value();
			require(count == 2 || count == 8);
			require(json["repair_index"].get_uint64().value() == count - 1);
			++rs_late;
			continue;
		}
		if (json["kind"].get_string().value() != "frame" || json["complete"].get_bool().value())
			continue;
		require(json["rs_blocks_by_repairs"].get_array().value().size() == 8);
		capacity_failures += json["over_capacity_groups"].get_uint64().value();
		repair_failures += json["insufficient_repair_groups"].get_uint64().value();
		require(json["recoverable_unrecovered_groups"].get_uint64().value() == 0);
		auto group = json["damaged_groups"].get_array().value().at(0);
		require(group["scheme"].get_uint64().value() == 1);
		const auto count = group["repair_count"].get_uint64().value();
		require(count == 2 || count == 8);
		require(json["rs_blocks_by_repairs"].get_array().value().at(count - 1).get_uint64().value() == 1);
	}
	require(capacity_failures == 2 && repair_failures == 2 && rs_late == 4);
	std::cout << "First/middle/final recovery, reordered/duplicate repairs, two-frame buffering and original discard policy passed\n";
}
