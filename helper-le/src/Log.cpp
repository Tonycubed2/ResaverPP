#include "PCH.h"

namespace rshlog
{
	namespace
	{
		std::mutex    g_lock;
		std::ofstream g_file;
	}

	std::filesystem::path Dir()
	{
		wchar_t docs[MAX_PATH]{};
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_MYDOCUMENTS | CSIDL_FLAG_CREATE, nullptr, SHGFP_TYPE_CURRENT, docs))) {
			return {};
		}
		auto dir = std::filesystem::path(docs) / L"My Games" / L"Skyrim" / L"SKSE";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		return dir;
	}

	void Open()
	{
		std::scoped_lock lock(g_lock);
		const auto       dir = Dir();
		if (!dir.empty()) {
			g_file.open(dir / L"ReSaverHelper.log", std::ios::out | std::ios::trunc);
		}
	}

	void Write(const char* a_level, const std::string& a_text)
	{
		SYSTEMTIME t{};
		GetLocalTime(&t);
		std::scoped_lock lock(g_lock);
		if (!g_file.is_open()) {
			return;
		}
		g_file << std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}] [{}] {}\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
					  t.wMilliseconds, a_level, a_text);
		g_file.flush();
	}
}
