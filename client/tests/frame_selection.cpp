// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_selection.h"
#include <iostream>
#include <stdexcept>

struct frame
{
	struct
	{
		uint64_t frame_index;
	} feedback;
	struct
	{
		int64_t display_time;
	} view_info;
};
using image = std::shared_ptr<frame>;
static image make(uint64_t id)
{
	return std::make_shared<frame>(frame{{id}, {int64_t(id * 10)}});
}
static void require(bool value)
{
	if (!value)
		throw std::runtime_error("Frame selection regression");
}

int main()
{
	std::array<std::array<image, 3>, 3> storage{};
	std::array<std::span<const image>, 3> caches;
	for (size_t i = 0; i < caches.size(); ++i)
		caches[i] = storage[i];
	std::array<image, 3> shown{};
	storage[0] = {make(10), make(11), make(12)};
	storage[1] = {make(10), make(11), make(12)};
	auto selected = wivrn::select_video_frames(caches, shown, 2, 115);
	require(selected[0]->feedback.frame_index == 12 && selected[1]->feedback.frame_index == 12);
	// A previous fallback showed a newer pair, but only an old common frame
	// remains. Do not jump back to it, even with an old display prediction.
	shown = {make(12), make(12), {}};
	storage[0] = {make(10), make(11), make(13)};
	storage[1] = {make(10), make(11), make(12)};
	selected = wivrn::select_video_frames(caches, shown, 2, 140);
	require(selected[0]->feedback.frame_index == 13 && selected[1]->feedback.frame_index == 12);
	shown = selected;
	selected = wivrn::select_video_frames(caches, shown, 2, 100);
	require(selected[0]->feedback.frame_index == 13 && selected[1]->feedback.frame_index == 12);
	// New synchronized output becomes eligible as soon as it arrives.
	storage[1][0] = make(13);
	selected = wivrn::select_video_frames(caches, shown, 2, 140);
	require(selected[0]->feedback.frame_index == 13 && selected[1]->feedback.frame_index == 13);
	// Held images survive cache wrap, delayed callbacks and temporary emptiness.
	shown = selected;
	for (auto & cache: storage)
		cache = {make(7), make(8), make(9)};
	selected = wivrn::select_video_frames(caches, shown, 2, 200);
	require(selected[0] == shown[0] && selected[1] == shown[1]);
	for (auto & cache: storage)
		cache = {};
	selected = wivrn::select_video_frames(caches, shown, 2, 200);
	require(selected[0] == shown[0] && selected[1] == shown[1]);
	// Alpha participates in common-frame selection when enabled.
	shown = {};
	storage[0] = {make(20), make(21), make(22)};
	storage[1] = storage[0];
	storage[2] = {make(20), make(21), {}};
	selected = wivrn::select_video_frames(caches, shown, 3, 220);
	for (auto & value: selected)
		require(value->feedback.frame_index == 21);
	// Across repeated losses, wraps and late outputs, every eye is monotonic.
	shown = {};
	for (uint64_t tick = 0; tick < 500; ++tick)
	{
		for (size_t stream = 0; stream < 2; ++stream)
			if ((tick + stream * 3) % 7)
				storage[stream][tick % 3] = make(tick > 4 && tick % 11 == 0 ? tick - 4 : tick);
		selected = wivrn::select_video_frames(caches, shown, 2, int64_t(tick * 10));
		for (size_t stream = 0; stream < 2; ++stream)
			if (shown[stream])
				require(selected[stream]->feedback.frame_index >= shown[stream]->feedback.frame_index);
		shown = selected;
	}
	std::cout << "Frame selection: common frames, fallback, monotonicity, held images, loss and alpha passed\n";
}
