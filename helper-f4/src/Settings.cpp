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
			"; ReSaverHelper for Fallout 4 - lets ReSaver++ (ReSaver Plus) act in the running game.\n"
			"; ReSaver++ can change these settings itself while the game runs.\n"
			"[General]\n"
			"; How the helper confirms what it did: hud = top-left notification, box = message box, none\n"
			"sNotify=hud\n"
			"; 0 = do not accept commands from ReSaver++ at all\n"
			"bListen=1\n"
			"; Hold Ctrl in the game for iRescueHoldSeconds: the game is saved (a new save, ReSaverPP_Rescue_<date>),\n"
			"; your controls are forced back on and your companion and Dogmeat are brought to you. 0 = off.\n"
			"bRescueHotkey=1\n"
			"; seconds Ctrl must be held (3 to 60)\n"
			"iRescueHoldSeconds=8\n";
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
		} else if (n == "blisten") {
			g_values.listen = ParseBool(v);
		} else if (n == "brescuehotkey") {
			g_values.rescueHotkey = ParseBool(v);
		} else if (n == "irescueholdseconds") {
			try {
				g_values.rescueHoldSeconds = std::clamp(std::stoi(v), 3, 60);
			} catch (...) {
				return false;
			}
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
		if (n == "blisten") {
			return g_values.listen ? "1" : "0";
		}
		if (n == "brescuehotkey") {
			return g_values.rescueHotkey ? "1" : "0";
		}
		if (n == "irescueholdseconds") {
			return std::to_string(g_values.rescueHoldSeconds);
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
		spdlog::info("Settings: sNotify={} bListen={} bRescueHotkey={} iRescueHoldSeconds={}", g_values.notify, g_values.listen, g_values.rescueHotkey,
			g_values.rescueHoldSeconds);
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
		for (const char* key : { "sNotify", "bListen", "bRescueHotkey", "iRescueHoldSeconds" }) {
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
}
