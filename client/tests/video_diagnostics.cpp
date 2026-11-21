// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_diagnostics.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static void require(bool condition)
{
	if (!condition)
		throw std::runtime_error("Diagnostic recorder regression");
}

int main()
{
	namespace fs = std::filesystem;
	const auto root = fs::temp_directory_path() / ("wivrn-diagnostics-test-" + std::to_string(getpid()));
	fs::create_directories(root);
	setenv("XDG_STATE_HOME", root.c_str(), 1);
	require(wivrn::video_diagnostics::default_directory() == root / "wivrn/logs");
	setenv("XDG_STATE_HOME", "relative-path-is-invalid", 1);
	const auto home = fs::path(std::getenv("HOME"));
	require(wivrn::video_diagnostics::default_directory() == home / ".local/state/wivrn/logs");
	setenv("XDG_STATE_HOME", "", 1);
	require(wivrn::video_diagnostics::default_directory() == home / ".local/state/wivrn/logs");

	wivrn::video_diagnostics recorder;
	require(recorder.start(root / "recordings"));
	const auto first = recorder.snapshot().path;
	std::vector<std::thread> producers;
	for (int p = 0; p < 3; ++p)
		producers.emplace_back([&, p] {
			for (int i = 0; i < 200; ++i)
				recorder.record({.kind = "frame", .detail = "quote\" slash\\ newline\n tab\t", .frame = uint64_t(i), .stream = uint32_t(p), .received = 4, .expected_min = 6, .submitted = true});
		});
	for (auto & producer: producers)
		producer.join();
	recorder.record({.kind = "stream_destroyed"});
	recorder.record({.kind = "stream_created"});
	recorder.stop();
	auto status = recorder.snapshot();
	require(!status.recording && status.error.empty() && !status.lost_events);
	struct stat info{};
	require(stat(first.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
	std::ifstream input(first);
	std::string line;
	size_t lines = 0, frames = 0;
	while (std::getline(input, line))
	{
		++lines;
		require(line.front() == '{' && line.back() == '}');
		if (line.find("\"kind\":\"frame\"") != std::string::npos)
		{
			++frames;
			require(line.find("quote\\\" slash\\\\ newline\\u000a tab\\u0009") != std::string::npos);
			require(line.find("\"expected_min\":6") != std::string::npos);
		}
	}
	require(lines == 604 && frames == 600);
	// Stop/start creates a distinct file; inactive events never leak into it.
	recorder.record({.kind = "should_not_appear"});
	require(recorder.start(root / "recordings"));
	require(recorder.snapshot().path != first);
	recorder.stop();
	const auto saved = recorder.snapshot().path;
	std::ifstream second(saved);
	lines = 0;
	while (std::getline(second, line))
		++lines;
	require(lines == 2);
	// A file cannot be used as a parent directory. Failure is reported in UI state.
	require(!recorder.start(saved / "invalid"));
	require(!recorder.snapshot().error.empty() && !recorder.active());
	std::cout << "Recorder: concurrent producers, flush, JSON escaping, reconnect lifetime, restart, permissions, XDG and errors passed: " << root << '\n';
}
