// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Platform.h"
#include "core/Text.h"

#ifdef _WIN32
#	include <windows.h>
#	include <shellapi.h>
#	include <shlobj.h>
#	include <shellapi.h>
#	include <commdlg.h>
#endif

#include <cstdlib>
#include <fstream>
#include <vector>

namespace platform
{
	std::string ToUtf8(const std::filesystem::path& a_path)
	{
		try {
			const auto u8 = a_path.u8string();
			return std::string(u8.begin(), u8.end());
		} catch (...) {  // a name with broken UTF-16 (rare): shown with ? for what cannot be converted
			std::string out;
			for (wchar_t c : a_path.native()) {
				out += c < 0x80 ? static_cast<char>(c) : '?';
			}
			return out;
		}
	}

	// never throws: text that is not UTF-8 (a value from an INI written in the Windows code page, an
	// environment variable from getenv) is read in the Windows code page instead
	std::filesystem::path FromUtf8(const std::string& a_utf8) { return ess::text::PathFrom(a_utf8); }

	const char* MyGamesName(int a_game) { return a_game == 2 ? "Fallout4" : a_game == 1 ? "Skyrim VR" : "Skyrim Special Edition"; }

	namespace
	{
		int g_game = 0;  // which game AddUnique / AddSteamLibraries look for: 0 Skyrim SE, 1 Skyrim VR, 2 Fallout 4

		const char* GameExe() { return g_game == 2 ? "Fallout4.exe" : g_game == 1 ? "SkyrimVR.exe" : "SkyrimSE.exe"; }
		const char* SteamDir() { return g_game == 2 ? "Fallout 4" : g_game == 1 ? "SkyrimVR" : "Skyrim Special Edition"; }

		bool IsGame(const std::filesystem::path& p)
		{
			std::error_code ec;
			return !p.empty() && std::filesystem::exists(p / GameExe(), ec);
		}

		void AddUnique(std::vector<std::filesystem::path>& v, const std::filesystem::path& p)
		{
			if (!IsGame(p)) {
				return;
			}
			std::error_code ec;
			for (const auto& x : v) {
				if (std::filesystem::equivalent(x, p, ec)) {
					return;
				}
			}
			v.push_back(p);
		}

		// Steam's libraryfolders.vdf: lines like  "path"  "D:\\SteamLibrary"
		void AddSteamLibraries(std::vector<std::filesystem::path>& v, const std::filesystem::path& steam)
		{
			AddUnique(v, steam / "steamapps" / "common" / SteamDir());
			std::ifstream f(steam / "steamapps" / "libraryfolders.vdf");
			for (std::string line; std::getline(f, line);) {
				const auto k = line.find("\"path\"");
				if (k == std::string::npos) {
					continue;
				}
				const auto a = line.find('"', k + 6);
				const auto b = a == std::string::npos ? a : line.rfind('"');
				if (a == std::string::npos || b <= a) {
					continue;
				}
				std::string val, raw = line.substr(a + 1, b - a - 1);
				for (std::size_t i = 0; i < raw.size(); ++i) {
					if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') {
						++i;
					}
					val.push_back(raw[i]);
				}
				AddUnique(v, FromUtf8(val) / "steamapps" / "common" / SteamDir());
			}
		}
	}

#ifdef _WIN32
	namespace
	{
		std::filesystem::path RegString(HKEY root, const wchar_t* key, const wchar_t* value)
		{
			wchar_t buf[2048];
			DWORD   size = sizeof(buf);
			if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
				return std::filesystem::path(buf);
			}
			return {};
		}
	}

	std::vector<std::filesystem::path> SkyrimInstallFolders(int a_game)
	{
		g_game = a_game;
		const bool a_vr = a_game == 1;
		std::vector<std::filesystem::path> v;
		if (a_game == 2) {
			AddUnique(v, RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Fallout4", L"installed path"));
			AddUnique(v, RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Bethesda Softworks\\Fallout4", L"installed path"));
			if (auto steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"); !steam.empty()) {
				AddSteamLibraries(v, steam);
			}
			AddSteamLibraries(v, "C:\\Program Files (x86)\\Steam");
			return v;
		}
		if (a_vr) {
			AddUnique(v, RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim VR", L"installed path"));
			AddUnique(v, RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Bethesda Softworks\\Skyrim VR", L"installed path"));
		}
		AddUnique(v, RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim Special Edition", L"installed path"));
		AddUnique(v, RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Bethesda Softworks\\Skyrim Special Edition", L"installed path"));
		AddUnique(v, RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\GOG.com\\Games\\1711230643", L"path"));  // GOG Anniversary Edition
		if (auto steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"); !steam.empty()) {
			AddSteamLibraries(v, steam);
		}
		AddSteamLibraries(v, "C:\\Program Files (x86)\\Steam");
		return v;
	}

	std::filesystem::path ExeFolder()
	{
		wchar_t exe[MAX_PATH * 4] = L"";
		GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
		return std::filesystem::path(exe).parent_path();
	}

	bool HasNativeDialogs() { return true; }

	bool RunningUnderMO2() { return GetModuleHandleW(L"usvfs_x64.dll") != nullptr || GetModuleHandleW(L"usvfs_x86.dll") != nullptr; }

	std::filesystem::path DefaultSavesFolder(int a_game)
	{
		PWSTR docs = nullptr;
		std::filesystem::path p;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			p = std::filesystem::path(docs) / "My Games" / MyGamesName(a_game) / "Saves";
			CoTaskMemFree(docs);
		}
		return p;
	}

	std::optional<std::filesystem::path> OpenSaveDialog(const std::filesystem::path& a_startDir)
	{
		wchar_t      file[MAX_PATH * 4] = L"";
		std::wstring dir = a_startDir.wstring();
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetActiveWindow();
		ofn.lpstrFilter = L"Skyrim and Fallout 4 saves (*.ess, *.fos)\0*.ess;*.fos\0Skyrim saves (*.ess)\0*.ess\0Fallout 4 saves (*.fos)\0*.fos\0All files (*.*)\0*.*\0";
		ofn.lpstrFile = file;
		ofn.nMaxFile = static_cast<DWORD>(std::size(file));
		ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
		ofn.lpstrTitle = L"Open Skyrim save";
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetOpenFileNameW(&ofn)) {
			return std::filesystem::path(file);
		}
		return std::nullopt;
	}

	std::optional<std::filesystem::path> PickFolderDialog(const std::string& a_title)
	{
		std::wstring title = FromUtf8(a_title).wstring();
		BROWSEINFOW  bi{};
		bi.hwndOwner = GetActiveWindow();
		bi.lpszTitle = title.c_str();
		bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
		PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
		if (!pidl) {
			return std::nullopt;
		}
		wchar_t path[MAX_PATH * 4] = L"";
		const bool ok = SHGetPathFromIDListW(pidl, path);
		CoTaskMemFree(pidl);
		if (!ok) {
			return std::nullopt;
		}
		return std::filesystem::path(path);
	}

	std::filesystem::path SettingsFile()
	{
		wchar_t exe[MAX_PATH * 4] = L"";
		GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
		return std::filesystem::path(exe).parent_path() / "resaverpp.ini";
	}

	std::optional<std::filesystem::path> SaveAsDialog(const std::filesystem::path& a_suggested)
	{
		wchar_t file[MAX_PATH * 4] = L"";
		wcsncpy_s(file, a_suggested.filename().wstring().c_str(), _TRUNCATE);
		std::wstring  dir = a_suggested.parent_path().wstring();
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetActiveWindow();
		// the same kind of save as the one suggested (Skyrim .ess or Fallout 4 .fos)
		const bool fos = _wcsicmp(a_suggested.extension().wstring().c_str(), L".fos") == 0;
		ofn.lpstrFilter = fos ? L"Fallout 4 saves (*.fos)\0*.fos\0" : L"Skyrim saves (*.ess)\0*.ess\0";
		ofn.lpstrDefExt = fos ? L"fos" : L"ess";
		ofn.lpstrFile = file;
		ofn.nMaxFile = static_cast<DWORD>(std::size(file));
		ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
		ofn.lpstrTitle = L"Save edited save as";
		ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetSaveFileNameW(&ofn)) {
			return std::filesystem::path(file);
		}
		return std::nullopt;
	}
	std::optional<std::filesystem::path> SaveFileDialog(const std::filesystem::path& a_suggested, const std::string& a_ext, const std::string& a_title)
	{
		wchar_t file[MAX_PATH * 4] = L"";
		wcsncpy_s(file, a_suggested.filename().wstring().c_str(), _TRUNCATE);
		std::wstring dir = a_suggested.parent_path().wstring();
		std::wstring ext = FromUtf8(a_ext).wstring();
		std::wstring title = FromUtf8(a_title).wstring();
		// a_ext may name several, separated by ';' ("bsa;ba2")
		std::wstring pattern;
		for (std::size_t a = 0; a <= ext.size();) {
			const auto b = std::min(ext.find(L';', a), ext.size());
			if (b > a) {
				pattern += (pattern.empty() ? L"*." : L";*.") + ext.substr(a, b - a);
			}
			a = b + 1;
		}
		std::wstring filter = title + L" (" + pattern + L")";
		filter.push_back(L'\0');
		filter += pattern;
		filter.push_back(L'\0');
		filter.push_back(L'\0');
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetActiveWindow();
		ofn.lpstrFilter = filter.c_str();
		ofn.lpstrDefExt = ext.c_str();
		ofn.lpstrFile = file;
		ofn.nMaxFile = static_cast<DWORD>(std::size(file));
		ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
		ofn.lpstrTitle = title.c_str();
		ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetSaveFileNameW(&ofn)) {
			return std::filesystem::path(file);
		}
		return std::nullopt;
	}

	std::optional<std::filesystem::path> OpenFileDialog(const std::filesystem::path& a_startDir, const std::string& a_ext, const std::string& a_title)
	{
		wchar_t      file[MAX_PATH * 4] = L"";
		std::wstring dir = a_startDir.wstring();
		std::wstring ext = FromUtf8(a_ext).wstring();
		std::wstring title = FromUtf8(a_title).wstring();
		// a_ext may name several, separated by ';' ("bsa;ba2")
		std::wstring pattern;
		for (std::size_t a = 0; a <= ext.size();) {
			const auto b = std::min(ext.find(L';', a), ext.size());
			if (b > a) {
				pattern += (pattern.empty() ? L"*." : L";*.") + ext.substr(a, b - a);
			}
			a = b + 1;
		}
		std::wstring filter = title + L" (" + pattern + L")";
		filter.push_back(L'\0');
		filter += pattern;
		filter.push_back(L'\0');
		filter.push_back(L'\0');
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = GetActiveWindow();
		ofn.lpstrFilter = filter.c_str();
		ofn.lpstrFile = file;
		ofn.nMaxFile = static_cast<DWORD>(std::size(file));
		ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
		ofn.lpstrTitle = title.c_str();
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
		if (GetOpenFileNameW(&ofn)) {
			return std::filesystem::path(file);
		}
		return std::nullopt;
	}

	void OpenWithDefaultApp(const std::filesystem::path& a_path)
	{
		ShellExecuteW(nullptr, L"open", a_path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}
#else
	bool HasNativeDialogs() { return false; }
	std::optional<std::filesystem::path> SaveFileDialog(const std::filesystem::path&, const std::string&, const std::string&) { return std::nullopt; }
	void                                 OpenWithDefaultApp(const std::filesystem::path&) {}
	std::optional<std::filesystem::path> OpenFileDialog(const std::filesystem::path&, const std::string&, const std::string&) { return std::nullopt; }

	std::filesystem::path DefaultSavesFolder(int)
	{
		const char* home = std::getenv("HOME");
		return home ? std::filesystem::path(home) : std::filesystem::path();
	}

	std::optional<std::filesystem::path> OpenSaveDialog(const std::filesystem::path&) { return std::nullopt; }
	std::optional<std::filesystem::path> SaveAsDialog(const std::filesystem::path&) { return std::nullopt; }
	std::optional<std::filesystem::path> PickFolderDialog(const std::string&) { return std::nullopt; }
	std::filesystem::path                SettingsFile() { return std::filesystem::current_path() / "resaverpp.ini"; }
	std::vector<std::filesystem::path>   SkyrimInstallFolders(int a_game)
	{
		g_game = a_game;
		std::vector<std::filesystem::path> v;
		if (const char* home = std::getenv("HOME")) {
			AddSteamLibraries(v, std::filesystem::path(home) / ".steam" / "steam");
		}
		return v;
	}
	std::filesystem::path ExeFolder() { return std::filesystem::current_path(); }
	bool                  RunningUnderMO2() { return std::getenv("RESAVERPP_TEST_MO2") != nullptr; }
#endif

	bool MoveToRecycleBin(const std::vector<std::filesystem::path>& a_files)
	{
		if (a_files.empty()) {
			return true;
		}
#ifdef _WIN32
		// a double-null-terminated list of full paths
		std::wstring list;
		for (const auto& f : a_files) {
			list += std::filesystem::absolute(f).wstring();
			list.push_back(L'\0');
		}
		list.push_back(L'\0');
		SHFILEOPSTRUCTW op{};
		op.wFunc = FO_DELETE;
		op.pFrom = list.c_str();
		op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
		return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
#else
		bool ok = true;
		for (const auto& f : a_files) {
			std::error_code ec;
			ok = std::filesystem::remove(f, ec) && ok;
		}
		return ok;
#endif
	}
}
