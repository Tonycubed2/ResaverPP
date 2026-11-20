#include "PCH.h"
#include "Collect.h"

#include "LiveInfo.h"
#include "Protocol.h"
#include "SaveDialog.h"
#include "Zip.h"

#include <ShlObj.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <winreg.h>

#include <cctype>
#include <ctime>
#include <set>

namespace Collect
{
	namespace
	{
		std::mutex        g_lock;
		Progress          g_progress;
		std::atomic<bool> g_busy{ false };

		struct Item
		{
			std::filesystem::path file;
			std::string           name;  // the path inside the zip
		};

		std::string U8(const std::filesystem::path& a_path)
		{
			const auto s = a_path.u8string();
			return std::string(s.begin(), s.end());
		}

		std::string Lower(std::string a_s)
		{
			std::transform(a_s.begin(), a_s.end(), a_s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_s;
		}

		std::filesystem::path KnownFolder(const KNOWNFOLDERID& a_id)
		{
			PWSTR                 p = nullptr;
			std::filesystem::path out;
			if (SUCCEEDED(SHGetKnownFolderPath(a_id, 0, nullptr, &p)) && p) {
				out = p;
			}
			if (p) {
				CoTaskMemFree(p);
			}
			return out;
		}

		std::filesystem::path GameFolder()
		{
			wchar_t buf[MAX_PATH * 2]{};
			const auto n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
			return n ? std::filesystem::path(buf).parent_path() : std::filesystem::path();
		}

		// ------------------------------------------------------------ Mod Organizer: the instance that started the game
		struct Mo2
		{
			std::filesystem::path base, profilesDir, gamePath;
			std::string           profile;
		};

		std::string IniValue(std::string v)
		{
			if (v.rfind("@ByteArray(", 0) == 0 && !v.empty() && v.back() == ')') {
				v = v.substr(11, v.size() - 12);
			}
			std::string out;
			for (std::size_t i = 0; i < v.size(); ++i) {
				if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') {
					++i;
				}
				out.push_back(v[i]);
			}
			return out;
		}

		bool Relative(const std::string& a_v)
		{
			if (a_v.size() >= 2 && std::isalpha(static_cast<unsigned char>(a_v[0])) && a_v[1] == ':') {
				return false;
			}
			return a_v.empty() || (a_v[0] != '\\' && a_v[0] != '/');
		}

		std::filesystem::path FromU8(const std::string& a_s) { return std::filesystem::path(std::u8string(a_s.begin(), a_s.end())); }

		std::optional<Mo2> ReadMo2Ini(const std::filesystem::path& a_ini)
		{
			std::ifstream f(a_ini);
			if (!f) {
				return std::nullopt;
			}
			Mo2         m;
			std::string profiles, game, line;
			m.base = a_ini.parent_path();
			bool general = false, settings = false;
			while (std::getline(f, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (!line.empty() && line.front() == '[') {
					general = line == "[General]";
					settings = line == "[Settings]";
					continue;
				}
				const auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				const auto key = line.substr(0, eq);
				const auto val = IniValue(line.substr(eq + 1));
				if (general && key == "selected_profile") {
					m.profile = val;
				} else if (general && key == "gamePath") {
					game = val;
				} else if (settings && key == "base_directory" && !val.empty()) {
					m.base = Relative(val) ? a_ini.parent_path() / FromU8(val) : FromU8(val);
				} else if (settings && key == "profiles_directory") {
					profiles = val;
				}
			}
			auto resolve = [&](std::string v, const char* a_def) {
				if (v.empty()) {
					return m.base / a_def;
				}
				if (const auto p = Lower(v).find("%base_dir%"); p != std::string::npos) {
					v.replace(p, 10, U8(m.base));
				}
				return Relative(v) ? m.base / FromU8(v) : FromU8(v);
			};
			m.profilesDir = resolve(profiles, "profiles");
			if (!game.empty()) {
				if (const auto p = Lower(game).find("%base_dir%"); p != std::string::npos) {
					game.replace(p, 10, U8(m.base));
				}
				m.gamePath = Relative(game) ? (a_ini.parent_path() / FromU8(game)).lexically_normal() : FromU8(game);
			}
			return m;
		}

		bool SameFolder(const std::filesystem::path& a, const std::filesystem::path& b)
		{
			std::error_code ec;
			if (a.empty() || b.empty()) {
				return false;
			}
			if (std::filesystem::equivalent(a, b, ec)) {
				return true;
			}
			return Lower(U8(a.lexically_normal())) == Lower(U8(b.lexically_normal()));
		}

		// the running ModOrganizer.exe (the game is started through it); its ModOrganizer.ini is next to it (portable
		// instance) or under %LOCALAPPDATA%\ModOrganizer\<instance> with this game's folder as gamePath
		std::optional<Mo2> FindMo2()
		{
			std::vector<std::filesystem::path> exes;
			if (const auto snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
				PROCESSENTRY32W pe{ sizeof(pe) };
				for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
					if (_wcsicmp(pe.szExeFile, L"ModOrganizer.exe") != 0) {
						continue;
					}
					if (const auto h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
						wchar_t buf[MAX_PATH * 2]{};
						DWORD   n = static_cast<DWORD>(std::size(buf));
						if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
							exes.emplace_back(buf);
						}
						CloseHandle(h);
					}
				}
				CloseHandle(snap);
			}
			if (exes.empty()) {
				return std::nullopt;
			}
			const auto game = GameFolder();
			std::vector<std::filesystem::path> inis;
			for (const auto& e : exes) {
				inis.push_back(e.parent_path() / "ModOrganizer.ini");
			}
			std::error_code ec;
			if (const auto la = KnownFolder(FOLDERID_LocalAppData); !la.empty()) {
				for (std::filesystem::directory_iterator it(la / "ModOrganizer", ec), end; !ec && it != end; it.increment(ec)) {
					inis.push_back(it->path() / "ModOrganizer.ini");
				}
			}
			std::optional<Mo2> any;
			for (const auto& ini : inis) {
				if (!std::filesystem::is_regular_file(ini, ec)) {
					continue;
				}
				auto m = ReadMo2Ini(ini);
				if (!m) {
					continue;
				}
				if (SameFolder(m->gamePath, game)) {
					return m;
				}
				if (!any) {
					any = m;
				}
			}
			return any;
		}

		// ------------------------------------------------------------ system info for the README
		std::string SystemText()
		{
			std::string t;
			using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
			if (const auto ntdll = GetModuleHandleW(L"ntdll.dll")) {
				if (const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
					RTL_OSVERSIONINFOW v{ sizeof(v) };
					if (fn(&v) == 0) {
						t += std::format("Windows: {}.{} build {}\r\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
					}
				}
			}
			MEMORYSTATUSEX ms{ sizeof(ms) };
			if (GlobalMemoryStatusEx(&ms)) {
				t += std::format("Memory: {} MB in all, {} MB free\r\n", ms.ullTotalPhys / (1024 * 1024), ms.ullAvailPhys / (1024 * 1024));
			}
			HKEY k = nullptr;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
				wchar_t name[256]{};
				DWORD   size = sizeof(name);
				if (RegQueryValueExW(k, L"ProcessorNameString", nullptr, nullptr, reinterpret_cast<LPBYTE>(name), &size) == ERROR_SUCCESS) {
					t += "Processor: " + U8(std::filesystem::path(name)) + "\r\n";
				}
				RegCloseKey(k);
			}
			return t;
		}

		void Set(const std::function<void(Progress&)>& a_fn)
		{
			std::scoped_lock l(g_lock);
			a_fn(g_progress);
		}
	}

	Progress Get()
	{
		std::scoped_lock l(g_lock);
		return g_progress;
	}

	bool Busy() { return g_busy.load(); }

	void ShowInFolder(const std::filesystem::path& a_zip)
	{
		std::thread([a_zip] {
			const auto arg = L"/select,\"" + a_zip.wstring() + L"\"";
			ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
		}).detach();
	}

	bool Start(const std::filesystem::path& a_gameReport, const std::string& a_gameReportNote, std::string a_helperReport)
	{
		if (g_busy.exchange(true)) {
			return false;
		}
		Set([](Progress& p) { p = Progress{ true, 0, 0, {}, "Collecting the game data..." }; });

		// what the game knows, read here (the caller runs on the main thread): the save folder from the game's INI
		std::string savePath = "Saves\\";
		if (auto* ini = RE::INISettingCollection::GetSingleton()) {
			if (auto* s = ini->GetSetting("sLocalSavePath:General"); s && s->GetType() == RE::Setting::Type::kString && s->GetString() &&
																		*s->GetString()) {
				savePath = s->GetString();
			}
		}
		const auto gameVersion = std::format("Skyrim {} {}", REL::Module::IsVR() ? "VR" : REL::Module::IsAE() ? "AE" : "SE",
			REL::Module::get().version().string());

		std::thread([a_gameReport, a_gameReportNote, helperReport = std::move(a_helperReport), savePath, gameVersion] {
			std::vector<Item>        items;
			std::vector<std::string> notes;
			std::set<std::filesystem::path> seen;
			std::error_code          ec;
			if (!a_gameReportNote.empty()) {
				notes.push_back(a_gameReportNote);
			}
			auto add = [&](const std::filesystem::path& a_file, const std::string& a_name) {
				const auto size = std::filesystem::file_size(a_file, ec);
				if (ec) {
					ec.clear();
					return;
				}
				if (size > 200ull * 1024 * 1024) {
					notes.push_back("left out (over 200 MB): " + U8(a_file));
					return;
				}
				if (seen.insert(a_file.lexically_normal()).second) {
					items.push_back({ a_file, a_name });
				}
			};
			auto addTree = [&](const std::filesystem::path& a_dir, const std::string& a_prefix) {
				if (!std::filesystem::is_directory(a_dir, ec)) {
					ec.clear();
					return;
				}
				for (std::filesystem::recursive_directory_iterator it(a_dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
					 !ec && it != end; it.increment(ec)) {
					if (it->is_regular_file(ec)) {
						auto rel = U8(std::filesystem::relative(it->path(), a_dir, ec));
						std::replace(rel.begin(), rel.end(), '\\', '/');
						add(it->path(), a_prefix + "/" + rel);
					}
				}
				ec.clear();
			};

			// the reports first, at the top of the zip
			if (!a_gameReport.empty()) {
				add(a_gameReport, "Game state report.txt");
			}
			const auto skse = LiveInfo::LogFolder();                                  // Documents\My Games\<game>\SKSE
			const auto myGame = skse.empty() ? skse : skse.parent_path();             // Documents\My Games\<game>
			const auto gameName = myGame.empty() ? std::string("Skyrim") : U8(myGame.filename());
			if (!skse.empty()) {
				addTree(skse, gameName + "/SKSE");
			}
			if (!myGame.empty()) {
				addTree(myGame / "Logs", gameName + "/Logs");
				for (std::filesystem::directory_iterator it(myGame, ec), end; !ec && it != end; it.increment(ec)) {
					if (it->is_regular_file(ec) && Lower(U8(it->path().extension())) == ".ini") {
						add(it->path(), gameName + "/" + U8(it->path().filename()));
					}
				}
				ec.clear();
			}
			// the load order the game reads (with Mod Organizer: the profile's, through its virtual folders)
			if (const auto la = KnownFolder(FOLDERID_LocalAppData); !la.empty() && !myGame.empty()) {
				for (const char* f : { "plugins.txt", "loadorder.txt", "DLCList.txt" }) {
					const auto p = la / myGame.filename() / f;
					if (std::filesystem::is_regular_file(p, ec)) {
						add(p, "Load order (AppData)/" + gameName + "/" + f);
					}
				}
			}
			const auto game = GameFolder();
			if (!game.empty()) {
				addTree(game / "Data" / "NetScriptFramework" / "Crash", "NetScriptFramework crash logs");
			}
			// Mod Organizer
			std::string manager = "Mod manager: none found (not Mod Organizer)\r\n";
			if (const auto mo2 = FindMo2()) {
				manager = "Mod manager: Mod Organizer 2 (" + U8(mo2->base) + "), profile " + mo2->profile + "\r\n";
				addTree(mo2->base / "logs", "Mod Organizer/logs");
				if (!mo2->profile.empty()) {
					const auto prof = mo2->profilesDir / FromU8(mo2->profile);
					for (const char* f : { "modlist.txt", "plugins.txt", "loadorder.txt", "settings.ini" }) {
						if (std::filesystem::is_regular_file(prof / f, ec)) {
							add(prof / f, "Mod Organizer/profile " + mo2->profile + "/" + f);
						}
					}
				}
			}
			// the last 3 saves, each with its co-save
			if (!myGame.empty()) {
				auto dir = std::filesystem::path(savePath);
				if (dir.is_relative()) {
					dir = myGame / dir;
				}
				std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> saves;
				for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
					if (it->is_regular_file(ec) && Lower(U8(it->path().extension())) == ".ess") {
						saves.emplace_back(it->last_write_time(ec), it->path());
					}
				}
				ec.clear();
				std::sort(saves.begin(), saves.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
				for (std::size_t i = 0; i < saves.size() && i < 3; ++i) {
					const auto& s = saves[i].second;
					add(s, "Saves/" + U8(s.filename()));
					auto co = s;
					co.replace_extension(".skse");
					if (std::filesystem::is_regular_file(co, ec)) {
						add(co, "Saves/" + U8(co.filename()));
					}
				}
				if (saves.empty()) {
					notes.push_back("no saves found in " + U8(dir));
				}
			}
			// the SKSE plugin DLLs the game sees (with Mod Organizer: from every mod)
			std::string dllList;
			if (!game.empty()) {
				std::vector<std::string> dlls;
				const auto               dir = game / "Data" / "SKSE" / "Plugins";
				for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
					if (it->is_regular_file(ec) && Lower(U8(it->path().extension())) == ".dll") {
						dlls.push_back(std::format("{:<56} {:>12} bytes", U8(it->path().filename()), it->file_size(ec)));
					}
				}
				ec.clear();
				std::sort(dlls.begin(), dlls.end());
				dllList = std::format("SKSE plugins in {} ({}):\r\n", U8(dir), dlls.size());
				for (const auto& d : dlls) {
					dllList += "  " + d + "\r\n";
				}
			}

			Set([&](Progress& p) { p.total = items.size(); });

			// the zip, on the Desktop
			const auto now = std::time(nullptr);
			std::tm    tm{};
			localtime_s(&tm, &now);
			char stamp[64], when[64];
			std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H%M", &tm);
			std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
			auto desk = SaveDialog::Desktop();
			if (desk.empty() || !std::filesystem::is_directory(desk, ec)) {
				desk = skse;
			}
			auto zipPath = desk / (std::string("SkyrimGameData_") + stamp + ".zip");
			for (int n = 2; std::filesystem::exists(zipPath, ec) && n < 100; ++n) {
				zipPath = desk / std::format("SkyrimGameData_{}_{}.zip", stamp, n);
			}
			try {
				ess::zip::Writer z(zipPath);
				std::string readme = std::format("Skyrim game data saved from the game (ReSaver Plus F1 page, ReSaverHelper {}) on {}\r\n\r\n",
					proto::kHelperVersion, when);
				readme += gameVersion + "\r\n" + manager + SystemText() + "\r\nFiles:\r\n";
				if (!helperReport.empty()) {
					z.add("ReSaverHelper report.txt", helperReport);
					readme += "  ReSaverHelper report.txt\r\n";
				}
				for (const auto& it : items) {
					if (z.addFile(it.name, it.file)) {
						readme += "  " + it.name + "\r\n";
					} else {
						readme += "  could not be read: " + U8(it.file) + "\r\n";
					}
					Set([](Progress& p) { ++p.done; });
				}
				if (!dllList.empty()) {
					z.add("SKSE plugins (game Data folder).txt", dllList);
					readme += "  SKSE plugins (game Data folder).txt\r\n";
				}
				if (!notes.empty()) {
					readme += "\r\nNotes:\r\n";
					for (const auto& n : notes) {
						readme += "  " + n + "\r\n";
					}
				}
				z.add("README.txt", readme);
				z.close();
				const auto mb = static_cast<double>(std::filesystem::file_size(zipPath, ec)) / (1024.0 * 1024.0);
				spdlog::info("Save all game data: {} ({} files, {:.1f} MB)", zipPath.string(), items.size(), mb);
				Set([&](Progress& p) {
					p.zip = zipPath;
					p.message = std::format("Saved on your Desktop: {} ({} files, {:.1f} MB). Attach this file when you ask for help.",
						U8(zipPath.filename()), items.size(), mb);
				});
			} catch (const std::exception& e) {
				std::filesystem::remove(zipPath, ec);
				spdlog::error("Save all game data failed: {}", e.what());
				Set([&](Progress& p) { p.message = std::string("Could not write the zip: ") + e.what(); });
			}
			Set([](Progress& p) { p.busy = false; });
			g_busy = false;
		}).detach();
		return true;
	}
}
