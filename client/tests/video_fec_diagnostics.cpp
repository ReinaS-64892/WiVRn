// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_fec_diagnostics.h"
#include <cstdlib>
#include <fstream>
#include <simdjson.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static void require(bool condition)
{
	if (!condition)
		throw std::runtime_error("FEC recorder regression");
}

int main()
{
	namespace fs = std::filesystem;
	const auto root = fs::temp_directory_path() / ("wivrn-fec-diagnostics-test-" + std::to_string(getpid()));
	fs::create_directories(root);
	setenv("XDG_STATE_HOME", root.c_str(), 1);
	require(wivrn::video_fec_diagnostics::default_directory() == root / "wivrn/logs");
	setenv("XDG_STATE_HOME", "relative-path-is-invalid", 1);
	require(wivrn::video_fec_diagnostics::default_directory() == fs::path(std::getenv("HOME")) / ".local/state/wivrn/logs");

	wivrn::video_fec_diagnostics recorder;
	require(recorder.start(root / "recordings"));
	const auto first = recorder.snapshot().path;
	const auto generation = recorder.recording_id();
	std::vector<std::thread> producers;
	for (int p = 0; p < 3; ++p)
		producers.emplace_back([&] {
			for (int i = 0; i < 200; ++i)
				recorder.record(generation, "{\"kind\":\"test\"}");
		});
	for (auto & producer: producers)
		producer.join();
	recorder.stop();
	require(!recorder.recording_id() && recorder.snapshot().error.empty() && !recorder.snapshot().lost_events);
	struct stat info{};
	require(stat(first.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
	std::ifstream input(first);
	std::string line;
	size_t lines = 0, events = 0;
	simdjson::dom::parser parser;
	while (std::getline(input, line))
	{
		++lines;
		auto json = parser.parse(line);
		events += json["kind"].get_string().value() == "test";
	}
	require(lines == 602 && events == 600);
	require(recorder.start(root / "recordings"));
	require(recorder.recording_id() != generation && recorder.snapshot().path != first);
	recorder.record(generation, "{\"kind\":\"stale\"}");               // Previous recording cannot leak into a restart.
	recorder.record(recorder.recording_id(), std::string(16385, 'x')); // Bounded memory, no invalid truncated JSON.
	recorder.stop();
	require(recorder.snapshot().lost_events == 1);
	const auto second = recorder.snapshot().path;
	std::ifstream restarted(second);
	lines = 0;
	while (std::getline(restarted, line))
	{
		++lines;
		auto json = parser.parse(line);
		require(json["kind"].get_string().value() != "stale");
	}
	require(lines == 2);
	require(!recorder.start(second / "invalid"));
	require(!recorder.snapshot().error.empty() && !recorder.recording_id());
}
