#include "PCH.h"
#include "Settings.h"

namespace Settings
{
	namespace
	{
		Values g_values;

		std::string Lower(std::string s)
		{
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		std::string Trim(std::string s)
		{
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
				s.pop_back();
			}
			std::size_t a = 0;
			while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) {
				++a;
			}
			return s.substr(a);
		}

		bool ParseBool(const std::string& v) { return v == "1" || Lower(v) == "true"; }

		const char* kDefaultIni =
			"; ReSaverHelper - lets ReSaver++ (ReSaver Plus) act in the running game.\n"
			"; ReSaver++ can change these settings itself while the game runs (Quests page / Settings).\n"
			"[General]\n"
			"; How the helper confirms what it did: hud = top-left notification, box = message box, none\n"
			"sNotify=hud\n"
			"; 1 = refresh the quest journal after every save load (brings back quests and objectives the\n"
			"; journal stopped showing). 0 = only when you ask for it in ReSaver++.\n"
			"bRefreshJournalOnLoad=0\n"
			"; seconds to wait after a load before that refresh starts\n"
			"iRefreshDelaySeconds=5\n"
			"; 0 = do not accept commands from ReSaver++ at all\n"
			"bListen=1\n"
			"; Hold Ctrl in the game for iRescueHoldSeconds: the game is saved (a new save, ReSaverPP_Rescue_<date>),\n"
			"; your controls are switched back on and every follower is brought next to you. 0 = off.\n"
			"bRescueHotkey=1\n"
			"; seconds Ctrl must be held (3 to 60)\n"
			"iRescueHoldSeconds=8\n"
			"; characters Gather followers leaves alone: form ids as the console shows them, separated by commas\n"
			"sGatherSkip=\n"
			"; Menu load guard (read when the game starts): when a menu's interface file (Interface\\<name>.swf) does not load,\n"
			"; the helper logs why, tries again, and if it still fails the menu does not open (with a notice) instead of\n"
			"; crashing the game. 0 = off (no hooks at all).\n"
			"bMenuLoadGuard=1\n"
			"; how many more tries after a failed load (0 to 5; the second and later wait 50 ms)\n"
			"iMenuLoadRetries=2\n"
			"; 1 = one log line for every interface file loaded (else only failures, with the last 50 loads)\n"
			"bLogAllMovieLoads=0\n"
			"; 1 = Scaleform's own error messages and its file opening are written to the log when a load fails (version 17)\n"
			"bMenuLoadDiagnostics=1\n"
			"; Linux / Steam Deck (Proton) only: when another program is started into the game's Proton prefix, Proton removes the\n"
			"; drive the game runs from (S:) and menus crash. 1 = the helper puts it back at once (version 18). 0 = off.\n"
			"bFixProtonGameDrive=1\n"
			"; TEST ONLY - leave empty. Interface file names whose load is made to fail on purpose (e.g. Map,Quest_Journal,\n"
			"; StatsMenu - StatsMenu is the Skills menu, which loads its file in the background),\n"
			"; to try the guard. sTestFailMode: first = only the first try fails (the retry works), all = every try fails.\n"
			"sTestFailMovies=\n"
			"sTestFailMode=first\n";
	}

	Values& Get() { return g_values; }

	std::filesystem::path IniPath()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&IniPath), &self);
		wchar_t buf[MAX_PATH * 2]{};
		GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
		return std::filesystem::path(buf).parent_path() / L"ReSaverHelper.ini";
	}

	bool SetOption(const std::string& a_name, const std::string& a_value)
	{
		const auto n = Lower(a_name);
		const auto v = Trim(a_value);
		if (n == "snotify") {
			const auto m = Lower(v);
			if (m != "hud" && m != "box" && m != "none") {
				return false;
			}
			g_values.notify = m;
		} else if (n == "brefreshjournalonload") {
			g_values.refreshOnLoad = ParseBool(v);
		} else if (n == "irefreshdelayseconds") {
			try {
				g_values.refreshDelay = std::clamp(std::stoi(v), 0, 120);
			} catch (...) {
				return false;
			}
		} else if (n == "blisten") {
			g_values.listen = ParseBool(v);
		} else if (n == "brescuehotkey") {
			g_values.rescueHotkey = ParseBool(v);
		} else if (n == "sgatherskip") {
			g_values.gatherSkip = v;
		} else if (n == "irescueholdseconds") {
			try {
				g_values.rescueHoldSeconds = std::clamp(std::stoi(v), 3, 60);
			} catch (...) {
				return false;
			}
		} else if (n == "bmenuloadguard") {
			g_values.menuLoadGuard = ParseBool(v);
		} else if (n == "imenuloadretries") {
			try {
				g_values.menuLoadRetries = std::clamp(std::stoi(v), 0, 5);
			} catch (...) {
				return false;
			}
		} else if (n == "blogallmovieloads") {
			g_values.logAllMovieLoads = ParseBool(v);
		} else if (n == "bmenuloaddiagnostics") {
			g_values.menuLoadDiagnostics = ParseBool(v);
		} else if (n == "bfixprotongamedrive") {
			g_values.fixProtonGameDrive = ParseBool(v);
		} else if (n == "stestfailmovies") {
			g_values.testFailMovies = v;
		} else if (n == "stestfailmode") {
			const auto m = Lower(v);
			if (m != "first" && m != "all") {
				return false;
			}
			g_values.testFailMode = m;
		} else {
			return false;
		}
		return true;
	}

	std::string GetOption(const std::string& a_name)
	{
		const auto n = Lower(a_name);
		if (n == "snotify") {
			return g_values.notify;
		}
		if (n == "brefreshjournalonload") {
			return g_values.refreshOnLoad ? "1" : "0";
		}
		if (n == "irefreshdelayseconds") {
			return std::to_string(g_values.refreshDelay);
		}
		if (n == "blisten") {
			return g_values.listen ? "1" : "0";
		}
		if (n == "brescuehotkey") {
			return g_values.rescueHotkey ? "1" : "0";
		}
		if (n == "irescueholdseconds") {
			return std::to_string(g_values.rescueHoldSeconds);
		}
		if (n == "sgatherskip") {
			return g_values.gatherSkip;
		}
		if (n == "bmenuloadguard") {
			return g_values.menuLoadGuard ? "1" : "0";
		}
		if (n == "imenuloadretries") {
			return std::to_string(g_values.menuLoadRetries);
		}
		if (n == "blogallmovieloads") {
			return g_values.logAllMovieLoads ? "1" : "0";
		}
		if (n == "bmenuloaddiagnostics") {
			return g_values.menuLoadDiagnostics ? "1" : "0";
		}
		if (n == "bfixprotongamedrive") {
			return g_values.fixProtonGameDrive ? "1" : "0";
		}
		if (n == "stestfailmovies") {
			return g_values.testFailMovies;
		}
		if (n == "stestfailmode") {
			return g_values.testFailMode;
		}
		return {};
	}

	void Load()
	{
		const auto path = IniPath();
		std::error_code ec;
		if (!std::filesystem::exists(path, ec)) {
			std::ofstream(path) << kDefaultIni;
			spdlog::info("Settings: wrote default {}", path.string());
		}
		std::ifstream f(path);
		for (std::string line; std::getline(f, line);) {
			line = Trim(line);
			if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') {
				continue;
			}
			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			auto value = Trim(line.substr(eq + 1));
			if (const auto sc = value.find(';'); sc != std::string::npos) {
				value = Trim(value.substr(0, sc));
			}
			if (!SetOption(Trim(line.substr(0, eq)), value)) {
				spdlog::warn("Settings: ignored '{}'", line);
			}
		}
		spdlog::info("Settings: sNotify={} bRefreshJournalOnLoad={} iRefreshDelaySeconds={} bListen={} bRescueHotkey={} iRescueHoldSeconds={}",
			g_values.notify, g_values.refreshOnLoad, g_values.refreshDelay, g_values.listen, g_values.rescueHotkey, g_values.rescueHoldSeconds);
		spdlog::info("Settings: bMenuLoadGuard={} iMenuLoadRetries={} bLogAllMovieLoads={} bMenuLoadDiagnostics={} bFixProtonGameDrive={}{}", g_values.menuLoadGuard,
			g_values.menuLoadRetries, g_values.logAllMovieLoads, g_values.menuLoadDiagnostics, g_values.fixProtonGameDrive, g_values.testFailMovies.empty() ? std::string() :
		                                   std::format(" TEST: sTestFailMovies={} sTestFailMode={}", g_values.testFailMovies, g_values.testFailMode));
	}

	bool Save()
	{
		const auto path = IniPath();
		// rewrite the known keys in place, keep everything else (comments, unknown keys)
		std::vector<std::string> lines;
		{
			std::ifstream f(path);
			for (std::string line; std::getline(f, line);) {
				lines.push_back(line);
			}
		}
		if (lines.empty()) {
			std::string d = kDefaultIni;
			for (std::size_t a = 0, b; a < d.size(); a = b + 1) {
				b = d.find('\n', a);
				if (b == std::string::npos) {
					b = d.size();
				}
				lines.push_back(d.substr(a, b - a));
			}
		}
		for (const char* key : { "sNotify", "bRefreshJournalOnLoad", "iRefreshDelaySeconds", "bListen", "bRescueHotkey", "iRescueHoldSeconds", "sGatherSkip", "bMenuLoadGuard", "iMenuLoadRetries",
				 "bLogAllMovieLoads", "bMenuLoadDiagnostics", "bFixProtonGameDrive", "sTestFailMovies", "sTestFailMode" }) {
			bool found = false;
			for (auto& l : lines) {
				const auto t = Trim(l);
				const auto eq = t.find('=');
				if (!t.empty() && t[0] != ';' && eq != std::string::npos && Lower(Trim(t.substr(0, eq))) == Lower(key)) {
					l = std::string(key) + "=" + GetOption(key);
					found = true;
				}
			}
			if (!found) {
				lines.push_back(std::string(key) + "=" + GetOption(key));
			}
		}
		std::ofstream f(path, std::ios::trunc);
		for (const auto& l : lines) {
			f << l << "\n";
		}
		return static_cast<bool>(f);
	}

	namespace
	{
		int g_startupStage = -1;

		// the line holds iDiagnosticStage (the same test as main.cpp's ReadDiagnosticStage: a commented line has ';' in its key)
		bool IsStageLine(const std::string& a_line)
		{
			const auto eq = a_line.find('=');
			if (eq == std::string::npos) {
				return false;
			}
			std::string key = a_line.substr(0, eq);
			std::erase_if(key, [](unsigned char c) { return std::isspace(c) != 0; });
			return Lower(key) == "idiagnosticstage";
		}
	}

	int  StartupDiagnosticStage() { return g_startupStage; }
	void SetStartupDiagnosticStage(int a_stage) { g_startupStage = a_stage; }

	int DiagnosticStageInIni()
	{
		std::ifstream f(IniPath());
		for (std::string line; std::getline(f, line);) {
			if (IsStageLine(line)) {
				try {
					const int v = std::stoi(line.substr(line.find('=') + 1));
					return v >= 0 && v < 5 ? v : -1;
				} catch (...) {
					return -1;
				}
			}
		}
		return -1;
	}

	bool WriteDiagnosticStage(int a_stage)
	{
		const auto               path = IniPath();
		std::vector<std::string> lines;
		{
			std::ifstream f(path);
			for (std::string line; std::getline(f, line);) {
				lines.push_back(line);
			}
		}
		if (lines.empty()) {  // no INI (deleted while the game runs): start from the default one, as Save() does
			const std::string d = kDefaultIni;
			for (std::size_t a = 0, b; a < d.size(); a = b + 1) {
				b = d.find('\n', a);
				if (b == std::string::npos) {
					b = d.size();
				}
				lines.push_back(d.substr(a, b - a));
			}
		}
		std::erase_if(lines, IsStageLine);  // every iDiagnosticStage line goes; at most one comes back
		if (a_stage >= 0 && a_stage < 5) {
			const auto line = "iDiagnosticStage=" + std::to_string(a_stage);
			auto       general = std::ranges::find_if(lines, [](const std::string& l) { return Lower(Trim(l)) == "[general]"; });
			if (general != lines.end()) {
				lines.insert(general + 1, line);
			} else {
				lines.push_back(line);
			}
		}
		std::ofstream f(path, std::ios::trunc);
		for (const auto& l : lines) {
			f << l << "\n";
		}
		const bool ok = static_cast<bool>(f);
		spdlog::info("Settings: iDiagnosticStage {} in {} ({})", a_stage >= 0 && a_stage < 5 ? std::to_string(a_stage) : std::string("removed"), path.string(),
			ok ? "written" : "could not write");
		return ok;
	}
}
