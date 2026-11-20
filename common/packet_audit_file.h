// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace wivrn
{
// Shared by all video streams in one recording. The file is independent of
// stdout/logcat filtering and is never reused for a later recording.
class packet_audit_file
{
	inline static std::atomic<uint64_t> serial = 0;
	const std::filesystem::path filename;
	mutable std::mutex mutex;
	FILE * file = nullptr;
	std::string error_message;
	std::chrono::steady_clock::time_point last_flush = std::chrono::steady_clock::now();

	void fail()
	{
		error_message = std::error_code(errno ? errno : EIO, std::generic_category()).message();
	}

public:
	explicit packet_audit_file(const std::filesystem::path & directory) :
	        filename(directory / ("video-packets-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "-" + std::to_string(getpid()) + "-" + std::to_string(++serial) + ".log"))
	{
		if (std::filesystem::create_directories(directory))
			std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
		int fd = open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(), "Cannot create packet recording");
		file = fdopen(fd, "w");
		if (not file)
		{
			auto code = errno;
			::close(fd);
			throw std::system_error(code, std::generic_category(), "Cannot open packet recording");
		}
	}
	packet_audit_file(const packet_audit_file &) = delete;
	packet_audit_file & operator=(const packet_audit_file &) = delete;
	~packet_audit_file()
	{
		close();
	}
	const std::filesystem::path & path() const
	{
		return filename;
	}
	void write(std::string_view line)
	{
		std::lock_guard lock(mutex);
		if (not file)
		{
			error_message = "Recording file is closed";
			return;
		}
		if (not error_message.empty())
			return;
		if (fwrite(line.data(), 1, line.size(), file) != line.size() or fputc('\n', file) == EOF)
		{
			fail();
			return;
		}
		auto now = std::chrono::steady_clock::now();
		if (now - last_flush >= std::chrono::seconds(1))
		{
			if (fflush(file) != 0)
				fail();
			last_flush = now;
		}
	}
	void close()
	{
		std::lock_guard lock(mutex);
		if (file)
		{
			if (fclose(file) != 0)
				fail();
			file = nullptr;
		}
	}
	std::string error() const
	{
		std::lock_guard lock(mutex);
		return error_message;
	}
};
} // namespace wivrn
