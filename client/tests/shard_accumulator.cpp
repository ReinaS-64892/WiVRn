// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder/shard_accumulator.h"
#include "scenes/stream.h"
#include "xr/instance.h"

#include <iostream>
#include <stdexcept>

namespace
{
struct submission
{
	wivrn::from_headset::feedback feedback;
	XrTime display_time;
	std::vector<uint8_t> bytes;
};

class recording_decoder : public wivrn::decoder
{
public:
	std::vector<submission> frames;
	std::vector<uint8_t> bytes;
	void push_data(std::span<std::span<const uint8_t>> spans, uint64_t, bool partial) override
	{
		if (partial || !bytes.empty())
			throw std::runtime_error("Access unit was submitted more than once");
		for (auto span: spans)
			bytes.insert(bytes.end(), span.begin(), span.end());
	}
	void frame_completed(const wivrn::from_headset::feedback & feedback,
	                     const wivrn::to_headset::video_stream_data_shard::view_info_t & view) override
	{
		frames.push_back({feedback, view.display_time, std::move(bytes)});
		bytes.clear();
	}
	vk::Sampler sampler() override
	{
		return nullptr;
	}
};

std::shared_ptr<recording_decoder> recording;

void require(bool condition)
{
	if (!condition)
		throw std::runtime_error("Frame accumulator regression");
}

wivrn::to_headset::video_stream_data_shard packet(uint64_t frame, uint16_t index)
{
	static std::array<uint8_t, 6> data{0, 1, 2, 3, 4, 5};
	wivrn::to_headset::video_stream_data_shard result{};
	result.frame_idx = frame;
	result.shard_idx = index;
	result.unit = {uint16_t(index / 3), uint16_t(index / 3 * 3), 3, 3, 1, true};
	result.payload = std::span(data).subspan(index, 1);
	result.view_info.emplace();
	result.view_info->display_time = frame + 100;
	if (index == 5)
		result.timing_info.emplace();
	return result;
}
} // namespace

// Replace the device-facing factory and clock, exercising the real accumulator
// without starting OpenXR, a GPU, a network connection or a codec driver.
wivrn::decoder::~decoder() = default;
std::shared_ptr<wivrn::decoder> wivrn::decoder::make(
        vk::raii::Device &,
        vk::raii::PhysicalDevice &,
        uint32_t,
        const to_headset::video_stream_description &,
        uint8_t,
        std::weak_ptr<scenes::stream>,
        shard_accumulator *)
{
	return recording;
}
XrTime xr::instance::now()
{
	static XrTime time = 1;
	return time++;
}
void scenes::stream::send_feedback(const wivrn::from_headset::feedback &) {}

int main()
{
	vk::raii::Device device(nullptr);
	vk::raii::PhysicalDevice physical_device(nullptr);
	xr::instance instance;
	wivrn::to_headset::video_stream_description description{};
	description.codec.fill(wivrn::h264);
	recording = std::make_shared<recording_decoder>();
	wivrn::shard_accumulator receiver(device, physical_device, instance, 0, description, {}, 0);
	for (int i: {5, 4, 3, 3, 2, 1, 0})
		receiver.push_shard(packet(0, i));
	require(recording->frames.size() == 1 && !recording->frames[0].feedback.stream_corrupted);
	require(recording->frames[0].bytes == std::vector<uint8_t>({0, 1, 2, 3, 4, 5}));
	// Missing the first packet still leaves view metadata on other symbols.
	for (int i: {1, 2, 3, 4, 5})
		receiver.push_shard(packet(1, i));
	require(recording->frames.size() == 1);
	for (int i = 0; i < 6; ++i)
		receiver.push_shard(packet(2, i));
	require(recording->frames.size() == 3);
	const auto & concealed = recording->frames[1];
	require(concealed.feedback.frame_index == 1 && concealed.feedback.stream_corrupted && concealed.feedback.sent_to_decoder);
	require(!concealed.feedback.received_last_packet && concealed.display_time == 101);
	require(concealed.bytes == std::vector<uint8_t>({3, 4, 5}));
	receiver.push_shard(packet(1, 0));
	require(recording->frames.size() == 3);
	// Two successive damaged frames, including a lost frame-end marker.
	for (int i: {0, 1, 2, 3, 4})
		receiver.push_shard(packet(3, i));
	for (int i: {0, 2, 3, 4, 5})
		receiver.push_shard(packet(4, i));
	receiver.push_shard(packet(5, 0));
	require(recording->frames.size() == 4 && recording->frames[3].feedback.frame_index == 3);
	require(recording->frames[3].bytes == std::vector<uint8_t>({0, 1, 2}));
	for (int i = 1; i < 6; ++i)
		receiver.push_shard(packet(5, i));
	require(recording->frames.size() == 6 && recording->frames[4].feedback.stream_corrupted);
	require(recording->frames[4].bytes == std::vector<uint8_t>({3, 4, 5}));
	require(!recording->frames[5].feedback.stream_corrupted);
	for (int i = 0; i < 6; ++i)
		receiver.push_shard(packet(9, i));
	require(recording->frames.size() == 7 && recording->frames.back().feedback.frame_index == 9);

	// Opaque codecs keep the complete-frame policy.
	for (auto codec: {wivrn::av1, wivrn::raw})
	{
		description.codec.fill(codec);
		recording = std::make_shared<recording_decoder>();
		wivrn::shard_accumulator opaque(device, physical_device, instance, 0, description, {}, 0);
		for (int i: {0, 2, 3, 4, 5})
			opaque.push_shard(packet(0, i));
		for (int i = 0; i < 6; ++i)
			opaque.push_shard(packet(1, i));
		require(recording->frames.size() == 1 && recording->frames[0].feedback.frame_index == 1);
	}
	std::cout << "Accumulator: reordering, duplicates, first/final loss, retirement, frame gaps and opaque codecs passed\n";
}
