// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder/shard_accumulator.h"
#include "scenes/stream.h"
#include "video_fec.h"
#include "xr/instance.h"

#include <iostream>
#include <source_location>
#include <stdexcept>

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
		std::array<uint8_t, 24> bytes{};
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
	std::cout << "First/middle/final recovery, reordered/duplicate repairs, two-frame buffering and original discard policy passed\n";
}
