#pragma once
// A small log for the LE helper, with the same calls as spdlog (spdlog::info("... {}", x)), so the code
// shared with the SE helper (Settings.cpp) builds unchanged. Writes
// Documents\My Games\Skyrim\SKSE\ReSaverHelper.log, one line per call, flushed at once.

#include <filesystem>
#include <format>
#include <string>
#include <utility>

namespace rshlog
{
	// Documents\My Games\Skyrim\SKSE (created if missing); empty if Documents cannot be found
	std::filesystem::path Dir();
	void                  Open();
	void                  Write(const char* a_level, const std::string& a_text);
}

namespace spdlog
{
	template <class... Args>
	void info(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		rshlog::Write("info", std::format(a_fmt, std::forward<Args>(a_args)...));
	}
	template <class... Args>
	void warn(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		rshlog::Write("warning", std::format(a_fmt, std::forward<Args>(a_args)...));
	}
	template <class... Args>
	void error(std::format_string<Args...> a_fmt, Args&&... a_args)
	{
		rshlog::Write("error", std::format(a_fmt, std::forward<Args>(a_args)...));
	}
}
