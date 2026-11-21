// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_units.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <source_location>
#include <stdexcept>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <x264.h>
}

static void require(bool condition, std::source_location location = std::source_location::current())
{
	if (!condition)
		throw std::runtime_error("slice decode regression at line " + std::to_string(location.line()));
}

struct shard
{
	wivrn::video_unit_info unit;
	std::span<uint8_t> payload;
};

int main()
{
	x264_param_t params;
	x264_param_default_preset(&params, "ultrafast", "zerolatency");
	params.i_width = 640;
	params.i_height = 480;
	params.i_csp = X264_CSP_I420;
	params.i_slice_count = 4;
	params.i_threads = 4;
	params.i_keyint_max = 30;
	params.b_repeat_headers = 1;
	params.b_annexb = 1;
	params.i_log_level = X264_LOG_ERROR;
	auto encoder = x264_encoder_open(&params);
	require(encoder);
	x264_picture_t input, output;
	require(x264_picture_alloc(&input, X264_CSP_I420, 640, 480) == 0);

	auto codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	auto decoder = avcodec_alloc_context3(codec);
	decoder->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
	decoder->thread_count = 1;
	require(avcodec_open2(decoder, codec, nullptr) == 0);
	auto picture = av_frame_alloc();
	auto packet = av_packet_alloc();
	int decoded = 0;
	bool concealed_frame_seen = false;
	bool symbol_dropped = false;
	auto drain = [&]() {
		int result;
		while ((result = avcodec_receive_frame(decoder, picture)) == 0)
		{
			require(picture->pts == decoded);
			++decoded;
			concealed_frame_seen |= picture->pts == 40;
			av_frame_unref(picture);
		}
		require(result == AVERROR(EAGAIN) || result == AVERROR_EOF);
	};
	for (int frame = 0; frame < 120; ++frame)
	{
		for (int plane = 0; plane < 3; ++plane)
		{
			const int width = plane ? 320 : 640;
			const int height = plane ? 240 : 480;
			for (int y = 0; y < height; ++y)
				for (int x = 0; x < width; ++x)
					input.img.plane[plane][y * input.img.i_stride[plane] + x] = (x * 7 + y * 11 + frame * 13 + plane * 53) % 256;
		}
		input.i_pts = frame;
		x264_nal_t * nals;
		int nal_count;
		require(x264_encoder_encode(encoder, &nals, &nal_count, &input, &output) > 0);
		std::vector<uint8_t> encoded;
		for (int n = 0; n < nal_count; ++n)
			encoded.insert(encoded.end(), nals[n].p_payload, nals[n].p_payload + nals[n].i_payload);

		std::vector<std::optional<shard>> symbols;
		int slices = 0;
		uint16_t unit_index = 0;
		for (auto nal: wivrn::annex_b_units(encoded))
		{
			const size_t prefix = wivrn::annex_b_prefix(nal);
			require(prefix < nal.size());
			const auto type = nal[prefix] & 0x1f;
			const bool vcl = type == 1 || type == 5;
			if (vcl)
				++slices;
			const uint16_t count = (nal.size() + 199) / 200;
			wivrn::video_unit_info unit{unit_index++, uint16_t(symbols.size()), count, uint32_t(nal.size()), 200, vcl};
			for (unsigned i = 0; i < count; ++i)
			{
				const auto payload = nal.subspan(i * 200, std::min<size_t>(200, nal.size() - i * 200));
				if (frame == 40 && vcl && slices == 2 && i == count / 2)
				{
					symbols.emplace_back();
					symbol_dropped = true;
				}
				else
					symbols.emplace_back(shard{unit, payload});
			}
		}
		require(slices == 4);
		auto selected = wivrn::complete_video_units(symbols);
		require(selected.has_vcl);
		std::vector<uint8_t> reconstructed;
		for (auto span: selected.spans)
			reconstructed.insert(reconstructed.end(), span.begin(), span.end());
		if (frame != 40)
			require(reconstructed == encoded);
		else
			require(reconstructed.size() < encoded.size());
		require(av_new_packet(packet, reconstructed.size()) == 0);
		std::memcpy(packet->data, reconstructed.data(), reconstructed.size());
		packet->pts = frame;
		require(avcodec_send_packet(decoder, packet) == 0);
		av_packet_unref(packet);
		drain();
	}
	require(avcodec_send_packet(decoder, nullptr) == 0);
	drain();
	require(symbol_dropped && concealed_frame_seen && decoded == 120);
	av_packet_free(&packet);
	av_frame_free(&picture);
	avcodec_free_context(&decoder);
	x264_picture_clean(&input);
	x264_encoder_close(encoder);
	std::cout << "4-slice H.264: one UDP symbol lost, 120/120 frames decoded\n";
}
