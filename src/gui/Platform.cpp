// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Platform.h"
#include "core/Text.h"

#ifdef _WIN32
#	include <windows.h>
#	include <shlobj.h>
#	include <shellapi.h>
#	include <commdlg.h>
#	include <tlhelp32.h>
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

	const char* MyGamesName(int a_game) { return a_game == 3 ? "Starfield" : a_game == 2 ? "Fallout4" : a_game == 1 ? "Skyrim VR" : "Skyrim Special Edition"; }

	namespace
	{
		int g_game = 0;  // which game AddUnique / AddSteamLibraries look for: 0 Skyrim SE, 1 Skyrim VR, 2 Fallout 4, 3 Starfield

		const char* GameExe() { return g_game == 3 ? "Starfield.exe" : g_game == 2 ? "Fallout4.exe" : g_game == 1 ? "SkyrimVR.exe" : "SkyrimSE.exe"; }
		const char* SteamDir() { return g_game == 3 ? "Starfield" : g_game == 2 ? "Fallout 4" : g_game == 1 ? "SkyrimVR" : "Skyrim Special Edition"; }

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

		// Steam's libraryfolders.vdf: lines like  "path"  "D:\\SteamLibrary"; the Steam folder itself first
		std::vector<std::filesystem::path> SteamLibraryRoots(const std::filesystem::path& steam)
		{
			std::vector<std::filesystem::path> out{ steam };
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
#ifdef _WIN32
				if (!val.empty() && val[0] == '/') {
					val = "Z:" + val;  // Steam for Linux (read under Wine / Proton): its Unix paths are on Wine's Z: drive
				}
#endif
				out.push_back(FromUtf8(val).make_preferred());
			}
			return out;
		}

		void AddSteamLibraries(std::vector<std::filesystem::path>& v, const std::filesystem::path& steam)
		{
			for (const auto& lib : SteamLibraryRoots(steam)) {
				AddUnique(v, lib / "steamapps" / "common" / SteamDir());
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

	// ---- Wine / Proton -----------------------------------------------------------------------------------------------
	// Started as a non-Steam game, ReSaver++ gets a Proton prefix of its own: no Steam in its registry and no My Games of
	// the game in its Documents. The game is then found in Steam for Linux's libraries (through Wine's Z: drive) and its
	// saves, ini files and plugins.txt in the game's own prefix (steamapps\compatdata\<app id>\pfx).
	namespace
	{
		bool UnderWine()
		{
			static const bool wine = [] {
				const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
				return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
			}();
			return wine;
		}

		// Steam for Linux's own folders (each holds steamapps\libraryfolders.vdf), as Z:\ paths
		std::vector<std::filesystem::path> LinuxSteamRoots()
		{
			std::vector<std::filesystem::path> homes, roots;
			std::error_code                    ec;
			auto zpath = [](const std::filesystem::path& a_unix) {
				const auto u = a_unix.generic_wstring();
				return !u.empty() && u[0] == L'/' ? std::filesystem::path(L"Z:" + u) : std::filesystem::path();
			};
			for (const char* e : { "STEAM_COMPAT_CLIENT_INSTALL_PATH" }) {
				if (const auto p = zpath(ess::text::EnvPath(e)); !p.empty()) {
					roots.push_back(p);
				}
			}
			if (const auto h = zpath(ess::text::EnvPath("HOME")); !h.empty()) {
				homes.push_back(h);
			}
			for (const auto& e : std::filesystem::directory_iterator(L"Z:\\home", ec)) {
				homes.push_back(e.path());
			}
			for (const auto& h : homes) {
				for (const char* r : { ".steam/steam", ".steam/root", ".steam/debian-installation", ".local/share/Steam",
						 ".var/app/com.valvesoftware.Steam/.local/share/Steam", "snap/steam/common/.local/share/Steam" }) {
					roots.push_back(h / std::filesystem::path(r).make_preferred());
				}
			}
			std::vector<std::filesystem::path> out;
			for (const auto& r : roots) {
				if (std::filesystem::is_directory(r / "steamapps", ec)) {
					bool dup = false;
					for (const auto& o : out) {
						dup = dup || std::filesystem::equivalent(o, r, ec);
					}
					if (!dup) {
						out.push_back(r);
					}
				}
			}
			return out;
		}

		// every Steam library of Steam for Linux (the roots and the libraries their libraryfolders.vdf lists)
		std::vector<std::filesystem::path> LinuxSteamLibraries()
		{
			std::vector<std::filesystem::path> libs;
			for (const auto& r : LinuxSteamRoots()) {
				for (const auto& l : SteamLibraryRoots(r)) {
					libs.push_back(l);
				}
			}
			return libs;
		}

		// The game's own Proton prefix user folder when ReSaver++ runs in another prefix (see text::GameUserFolder)
		std::filesystem::path WineGameUserFolder(int a_game)
		{
			if (!UnderWine() || a_game < 0 || a_game > 3) {
				return {};
			}
			static std::filesystem::path found[4];
			static bool                  looked[4] = {};
			if (looked[a_game]) {
				return found[a_game];
			}
			looked[a_game] = true;
			std::error_code ec;
			// run inside the game's prefix (launched with the game): its own Documents has the game's My Games
			PWSTR docs = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				const auto mine = std::filesystem::path(docs) / "My Games" / MyGamesName(a_game);
				CoTaskMemFree(docs);
				if (std::filesystem::is_directory(mine, ec)) {
					return {};
				}
			}
			static const char* const kAppId[4] = { "489830", "611670", "377160", "1716740" };
			for (const auto& lib : LinuxSteamLibraries()) {
				const auto users = lib / "steamapps" / "compatdata" / kAppId[a_game] / "pfx" / "drive_c" / "users";
				for (const auto& u : std::filesystem::directory_iterator(users, ec)) {
					if (std::filesystem::is_directory(u.path() / "Documents" / "My Games" / MyGamesName(a_game), ec)) {
						return found[a_game] = u.path();
					}
				}
			}
			return {};
		}

		const bool g_wineFinder = (ess::text::SetGameUserFolderFinder(&WineGameUserFolder), true);
	}

	std::vector<std::filesystem::path> SkyrimInstallFolders(int a_game)
	{
		g_game = a_game;
		const bool a_vr = a_game == 1;
		std::vector<std::filesystem::path> v;
		if (UnderWine()) {
			for (const auto& r : LinuxSteamRoots()) {
				AddSteamLibraries(v, r);
			}
		}
		if (a_game == 3) {
			// Starfield: Steam (Steam ...\steamapps\common\Starfield, in any Steam library)
			if (auto steam = RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"); !steam.empty()) {
				AddSteamLibraries(v, steam);
			}
			AddSteamLibraries(v, "C:\\Program Files (x86)\\Steam");
			return v;
		}
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

	std::optional<bool> Mo2ProcessRunning()
	{
		const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE) {
			return std::nullopt;
		}
		PROCESSENTRY32W e{};
		e.dwSize = sizeof(e);
		bool found = false;
		for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
			found = _wcsicmp(e.szExeFile, L"ModOrganizer.exe") == 0;
		}
		CloseHandle(snap);
		return found;
	}

	std::filesystem::path DefaultSavesFolder(int a_game)
	{
		if (const auto gu = WineGameUserFolder(a_game); !gu.empty()) {
			return gu / "Documents" / "My Games" / MyGamesName(a_game) / "Saves";  // (the game's Proton prefix)
		}
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
		ofn.lpstrFilter = L"Skyrim, Fallout 4 and Starfield saves (*.ess, *.fos, *.sfs)\0*.ess;*.fos;*.sfs\0Skyrim saves (*.ess)\0*.ess\0Fallout 4 saves (*.fos)\0*.fos\0Starfield saves (*.sfs)\0*.sfs\0All files (*.*)\0*.*\0";
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
		const bool sfs = _wcsicmp(a_suggested.extension().wstring().c_str(), L".sfs") == 0;
		ofn.lpstrFilter = sfs ? L"Starfield saves (*.sfs)\0*.sfs\0" : fos ? L"Fallout 4 saves (*.fos)\0*.fos\0" : L"Skyrim saves (*.ess)\0*.ess\0";
		ofn.lpstrDefExt = sfs ? L"sfs" : fos ? L"fos" : L"ess";
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
	std::optional<bool>   Mo2ProcessRunning() { return std::nullopt; }
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
