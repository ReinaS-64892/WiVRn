// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder/shard_accumulator.h"
#include "packet_dispatch.h"
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
XrTime test_time = 1'000'000;

XrTime display_time(uint64_t frame)
{
	return 1'000'000'000 + frame * 11'111'111;
}

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
	result.view_info->display_time = display_time(frame);
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
	return test_time;
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
	require(!concealed.feedback.received_last_packet && concealed.display_time == display_time(1));
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
	// Wall-clock retirement with no subsequent packets or frame-end marker.
	description.codec.fill(wivrn::h264);
	description.frame_rate = 90;
	recording = std::make_shared<recording_decoder>();
	wivrn::shard_accumulator timed(device, physical_device, instance, 0, description, {}, 0);
	test_time = 10'000'000;
	for (int i: {0, 1, 2, 3})
	{
		auto data = packet(0, i);
		data.view_info->display_time = 22'000'000;
		timed.push_shard(std::move(data));
	}
	require(timed.next_deadline() == 14'000'000); // Initial 4 ms assembly budget wins over the prediction
	test_time = 13'999'999;
	timed.expire(test_time);
	require(recording->frames.empty());
	timed.push_shard(packet(0, 0)); // duplicate/far prediction cannot extend the timer
	require(timed.next_deadline() == 14'000'000);
	test_time = 14'000'000;
	timed.expire(test_time);
	require(recording->frames.size() == 1 && recording->frames[0].feedback.stream_corrupted);
	require(recording->frames[0].bytes == std::vector<uint8_t>({0, 1, 2}));
	require(!timed.next_deadline());
	auto stats = timed.take_statistics();
	require(stats.incomplete == 1 && stats.partial == 1 && stats.expired == 1 && stats.dropped == 0);
	require(stats.wait_samples == 1 && stats.wait_sum == 4'000'000 && stats.wait_max == 4'000'000);
	timed.expire(test_time + 100'000'000);
	timed.push_shard(packet(0, 4));
	timed.push_shard(packet(0, 5));
	stats = timed.take_statistics();
	require(stats.late_packets == 2 && stats.incomplete == 0 && stats.wait_samples == 0);
	require(recording->frames.size() == 1); // no second submission after expiry

	// All NALs damaged: count a complete discard even without a decoded handle.
	for (int i: {0, 3})
	{
		auto data = packet(1, i);
		data.view_info->display_time = test_time + 10'000'000;
		timed.push_shard(std::move(data));
	}
	test_time += 4'000'000;
	timed.expire(test_time);
	stats = timed.take_statistics();
	require(stats.incomplete == 1 && stats.dropped == 1 && stats.partial == 0 && stats.expired == 1);
	require(recording->frames.size() == 1);

	// No prediction metadata: use the initial 4 ms assembly budget.
	const auto started = test_time;
	auto no_view = packet(2, 0);
	no_view.view_info.reset();
	timed.push_shard(std::move(no_view));
	require(timed.next_deadline() == started + 4'000'000);
	test_time = *timed.next_deadline();
	timed.expire(test_time);
	stats = timed.take_statistics();
	require(stats.dropped == 1 && stats.expired == 1);

	// A slow decoder increases the reserve used for the following frame.
	timed.record_decode_time(8'000'000);
	auto slow = packet(3, 0);
	slow.view_info->display_time = test_time + 15'000'000;
	timed.push_shard(std::move(slow));
	require(timed.next_deadline() == test_time + 3'000'000); // 10 ms + 2 ms
	test_time += 3'000'000;
	timed.expire(test_time);
	timed.take_statistics();

	// A complete frame is submitted immediately rather than waiting for its deadline.
	for (int i = 0; i < 6; ++i)
		timed.push_shard(packet(4, i));
	require(recording->frames.size() == 2 && !timed.next_deadline());
	stats = timed.take_statistics();
	require(stats.complete == 1 && stats.expired == 0 && stats.wait_sum == 0);

	// Already-past predictions at startup must not prevent IDR/clock calibration.
	auto stale = packet(5, 0);
	stale.view_info->display_time = test_time;
	timed.push_shard(std::move(stale));
	require(timed.next_deadline() == test_time + 4'000'000);
	test_time = *timed.next_deadline();
	timed.expire(test_time);
	require(!timed.next_deadline());
	stats = timed.take_statistics();
	require(stats.dropped == 1 && stats.expired == 1);
	for (int i = 0; i < 6; ++i)
	{
		auto data = packet(6, i);
		data.view_info->display_time = test_time;
		timed.push_shard(std::move(data));
	}
	require(recording->frames.size() == 3 && !timed.next_deadline());
	require(timed.take_statistics().complete == 1);

	// A missing current frame cannot hide the observed successor's timer.
	for (int i: {0, 1, 2})
		timed.push_shard(packet(8, i));
	test_time = *timed.next_deadline();
	timed.expire(test_time);
	require(recording->frames.size() == 4 && recording->frames.back().feedback.frame_index == 8);
	stats = timed.take_statistics();
	require(stats.unobserved_frames == 1 && stats.expired == 1 && stats.partial == 1);

	// The final source symbol at the boundary is late, even without an explicit tick.
	description.frame_rate = 90;
	recording = std::make_shared<recording_decoder>();
	wivrn::shard_accumulator boundary(device, physical_device, instance, 0, description, {}, 0);
	for (int i: {0, 1, 2, 3, 4})
	{
		auto data = packet(10, i);
		data.view_info->display_time = test_time + 10'000'000;
		boundary.push_shard(std::move(data));
	}
	test_time += 4'000'000;
	boundary.push_shard(packet(10, 5));
	require(recording->frames.size() == 1 && recording->frames[0].feedback.stream_corrupted);
	stats = boundary.take_statistics();
	require(stats.partial == 1 && stats.expired == 1 && stats.late_packets == 1 && stats.unobserved_frames == 0);

	// Bound the fallback on both slow and fast video streams.
	for (const auto [rate, limit]: {std::pair{30.f, XrDuration(4'000'000)}, std::pair{1000.f, XrDuration(2'000'000)}})
	{
		description.frame_rate = rate;
		wivrn::shard_accumulator bounded(device, physical_device, instance, 0, description, {}, 0);
		auto data = packet(50, 0);
		data.view_info.reset();
		bounded.push_shard(std::move(data));
		require(bounded.next_deadline() == test_time + limit);
	}

	// Normal reception still records its actual assembly time for the wait plot.
	description.frame_rate = 90;
	recording = std::make_shared<recording_decoder>();
	wivrn::shard_accumulator complete(device, physical_device, instance, 0, description, {}, 0);
	for (int i = 0; i < 6; ++i)
	{
		test_time += 200'000;
		complete.push_shard(packet(20, i));
	}
	stats = complete.take_statistics();
	require(stats.complete == 1 && stats.incomplete == 0 && stats.expired == 0);
	require(stats.wait_samples == 1 && stats.wait_sum == 1'000'000 && stats.wait_max == 1'000'000);
	require(!complete.next_deadline());
	// Fast intact UDP samples learn a 1.3x mean; slow outliers are excluded.
	wivrn::receive_time_budget learned(11'111'111);
	for (int i = 0; i < 24; ++i)
		learned.complete(2'000'000);
	for (int i = 0; i < 8; ++i)
		learned.complete(8'000'000);
	require(learned.value() == 2'600'000);
	for (int i = 0; i < 8; ++i)
		learned.incomplete();
	require(learned.value() == 4'000'000);
	for (int i = 0; i < 64; ++i)
		learned.incomplete();
	require(learned.value() == 11'111'111);
	for (int i = 0; i < 32; ++i)
		learned.complete(2'000'000);
	require(learned.value() == 2'600'000);

	// Actual assembler follows the learned budget, not just the estimator.
	recording = std::make_shared<recording_decoder>();
	wivrn::shard_accumulator adaptive(device, physical_device, instance, 0, description, {}, 0);
	for (uint64_t f = 100; f < 132; ++f)
	{
		for (int i = 0; i < 6; ++i)
		{
			if (i)
				test_time += 400'000;
			adaptive.push_shard(packet(f, i));
		}
		test_time += 9'000'000;
	}
	require(adaptive.current_receive_budget() == 2'600'000);
	adaptive.push_shard(packet(132, 0));
	require(adaptive.next_deadline() == test_time + 2'600'000);
	test_time = *adaptive.next_deadline();
	adaptive.expire(test_time);

	// TCP recovery IDRs can finish beyond both the UDP and display deadlines.
	// A fresh receiver accepts high server frame IDs after reconnect.
	recording = std::make_shared<recording_decoder>();
	wivrn::shard_accumulator recovery(device, physical_device, instance, 0, description, {}, 0);
	const auto recovery_start = test_time;
	for (int i = 0; i < 6; ++i)
	{
		auto data = packet(5000, i);
		data.view_info->display_time = recovery_start + 8'000'000;
		recovery.push_shard(std::move(data), true);
		if (i != 5)
			test_time += 6'000'000;
	}
	require(recording->frames.size() == 1 && !recording->frames[0].feedback.stream_corrupted);
	require(!recovery.next_deadline() && recovery.current_receive_budget() == 4'000'000);
	stats = recovery.take_statistics();
	require(stats.reliable_wait_max == 30'000'000 && stats.normal_wait_samples == 0 && stats.unobserved_frames == 0);
	recovery.push_shard(packet(5001, 0), true);
	require(recovery.next_deadline() == test_time + 100'000'000);
	test_time = *recovery.next_deadline();
	recovery.expire(test_time);
	require(recovery.take_statistics().expired == 1);
	// Advancing over a gap must preserve the packet's reliable provenance.
	recovery.push_shard(packet(5004, 0), true);
	require(recovery.next_deadline() == test_time + 100'000'000);
	test_time = *recovery.next_deadline();
	recovery.expire(test_time);
	recovery.push_shard(packet(5010, 0), true);
	require(recovery.next_deadline() == test_time + 100'000'000);

	// Capture assembly completion before any subsequent decoder submission work.
	wivrn::shard_accumulator::shard_set assembled(0);
	for (int i = 0; i < 6; ++i)
		assembled.insert(packet(0, i), 1'000'000 + i * 100'000, 4'000'000, 0, false);
	require(assembled.assembled_at == 1'500'000);
	assembled.insert(packet(0, 5), 3'000'000, 4'000'000, 0, false);
	require(assembled.assembled_at == 1'500'000);
	assembled.reset(1);
	require(assembled.assembled_at == 0 && !assembled.reliable);

	// Existing visitors stay single-argument; video gets transport provenance.
	struct visitor
	{
		bool reliable = false;
		int other = 0;
		void operator()(int, bool value)
		{
			reliable = value;
		}
		void operator()(std::unique_ptr<int> value)
		{
			other = *value;
		}
	} dispatch;
	std::variant<int, std::unique_ptr<int>> data(1);
	wivrn::dispatch_packet(dispatch, std::move(data), true);
	require(dispatch.reliable);
	wivrn::dispatch_packet(dispatch, std::move(data), false);
	require(!dispatch.reliable);
	data = std::make_unique<int>(42);
	wivrn::dispatch_packet(dispatch, std::move(data), true);
	require(dispatch.other == 42);
	std::cout << "Accumulator: deadlines, silence, late packets, statistics, decode reserve and frame assembly passed\n";
}
