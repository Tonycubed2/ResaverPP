// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "LoadOrder.h"
#include "PluginDB.h"
#include "Text.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <set>
#include <vector>

namespace ess::loadorder
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		std::string Trim(std::string s)
		{
			while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
				s.pop_back();
			}
			std::size_t a = 0;
			while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) {
				++a;
			}
			// a UTF-8 byte order mark at the start of the file
			if (s.size() >= a + 3 && static_cast<unsigned char>(s[a]) == 0xEF && static_cast<unsigned char>(s[a + 1]) == 0xBB &&
				static_cast<unsigned char>(s[a + 2]) == 0xBF) {
				a += 3;
			}
			return s.substr(a);
		}
	}

	const std::vector<std::string>& Fallout4Masters()
	{
		// the base game and its DLC, loaded in this order whether plugins.txt lists them or not
		static const std::vector<std::string> v = { "fallout4.esm", "dlcrobot.esm", "dlcworkshop01.esm", "dlccoast.esm", "dlcworkshop02.esm",
			"dlcworkshop03.esm", "dlcnukaworld.esm", "dlcultrahighresolution.esm" };
		return v;
	}

	std::filesystem::path GamePluginsTxt(int a_game, bool a_le)
	{
		// (under Proton in a prefix of its own: the game's prefix)
		const auto gu = text::GameUserFolder(a_game);
		const auto base = !gu.empty() ? gu / "AppData" / "Local" : text::EnvPath("LOCALAPPDATA");
		if (base.empty()) {
			return {};
		}
		if (a_game == 2) {
			return base / "Fallout4" / "plugins.txt";
		}
		if (a_game == 3) {
			return base / "Starfield" / "plugins.txt";
		}
		const bool a_vr = a_game == 1;
		if (a_le) {
			// a Legendary Edition save: its own plugins.txt, even when Special Edition is installed too
			std::error_code ec;
			if (const auto le = base / "Skyrim" / "plugins.txt"; std::filesystem::exists(le, ec)) {
				return le;
			}
		}
		if (a_vr) {
			return base / "Skyrim VR" / "plugins.txt";
		}
		// Steam, GOG, Epic and Microsoft Store editions each keep their own folder; with more than one,
		// the plugins.txt written last is the one in use
		std::filesystem::path           best = base / "Skyrim Special Edition" / "plugins.txt";
		std::filesystem::file_time_type bestTime{};
		bool                            found = false;
		for (const char* d : { "Skyrim Special Edition", "Skyrim Special Edition GOG", "Skyrim Special Edition EPIC", "Skyrim Special Edition MS" }) {
			std::error_code ec;
			const auto      f = base / d / "plugins.txt";
			const auto      t = std::filesystem::last_write_time(f, ec);
			if (!ec && (!found || t > bestTime)) {
				best = f;
				bestTime = t;
				found = true;
			}
		}
		if (!found) {
			// Skyrim Legendary Edition keeps its plugins.txt in %LOCALAPPDATA%\Skyrim
			std::error_code ec;
			if (const auto le = base / "Skyrim" / "plugins.txt"; std::filesystem::exists(le, ec)) {
				return le;
			}
		}
		return best;
	}

	Active Read(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_gameFolder, int a_game)
	{
		const bool a_vr = a_game == 1, fo4 = a_game == 2;
		Active a;
		std::ifstream f(a_pluginsTxt);
		if (!f) {
			return a;
		}
		a.known = true;
		a.file = a_pluginsTxt;
		// SE/AE (and Mod Organizer for them): enabled lines start with '*', the others are disabled.
		// Legendary Edition (and Mod Organizer for LE) writes no marks: every plugin listed is enabled.
		std::vector<std::string> lines;
		bool                     marked = false;
		for (std::string line; std::getline(f, line);) {
			line = Trim(line);
			if (line.empty() || line[0] == '#') {
				continue;
			}
			marked = marked || line[0] == '*';
			lines.push_back(line);
		}
		for (const auto& line : lines) {
			if (line[0] == '*') {
				a.plugins.insert(Lower(Trim(line.substr(1))));
			} else if (!marked) {
				a.plugins.insert(Lower(line));
			}
		}
		// always loaded, never listed in plugins.txt
		if (a_game == 3) {
			// Starfield: the game's own files (StarfieldBaseFiles, PluginDB.cpp)
			for (const auto& p : StarfieldBaseFiles()) {
				a.plugins.insert(p);
			}
			return a;
		}
		if (fo4) {
			for (const auto& p : Fallout4Masters()) {
				a.plugins.insert(p);
			}
		} else {
			for (const char* p : { "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm", "_resourcepack.esl" }) {
				a.plugins.insert(p);
			}
		}
		if (a_vr) {
			a.plugins.insert("skyrimvr.esm");
		}
		// Creation Club content of the installed edition (Anniversary Edition: Skyrim.ccc lists it)
		std::ifstream ccc(a_gameFolder / (fo4 ? "Fallout4.ccc" : "Skyrim.ccc"));
		for (std::string line; std::getline(ccc, line);) {
			line = Trim(line);
			if (!line.empty()) {
				a.plugins.insert(Lower(line));
			}
		}
		return a;
	}

	std::vector<std::filesystem::path> StarfieldDataDirs(const std::filesystem::path& a_gameFolder, const std::filesystem::path& a_myGames)
	{
		std::vector<std::filesystem::path> v;
		std::error_code                    ec;
		if (!a_gameFolder.empty()) {
			const auto d = std::filesystem::is_directory(a_gameFolder / "Data", ec) ? a_gameFolder / "Data" : a_gameFolder;
			v.push_back(d);
		}
		// (Creations installed from the game's menu; the place is the community's, not seen on Tony's install: [U])
		auto myGames = a_myGames;
		if (myGames.empty()) {
			auto home = text::GameUserFolder(3);
			if (home.empty()) {
				home = text::EnvPath("USERPROFILE");
			}
			if (!home.empty()) {
				myGames = home / "Documents" / "My Games" / "Starfield";
			}
		}
		if (!myGames.empty() && std::filesystem::is_directory(myGames / "Data", ec)) {
			v.push_back(myGames / "Data");
		}
		return v;
	}

	int StarfieldPluginKind(const std::filesystem::path& a_file)
	{
		std::ifstream f(a_file, std::ios::binary);
		unsigned char h[12]{};
		if (!f.read(reinterpret_cast<char*>(h), sizeof(h)) || std::string(reinterpret_cast<char*>(h), 4) != "TES4") {
			return -1;
		}
		const std::uint32_t flags = h[8] | (h[9] << 8) | (h[10] << 16) | (static_cast<std::uint32_t>(h[11]) << 24);
		if ((flags & 0x200) && !(flags & 0x500)) {
			return -1;
		}
		if (flags & 0x100) {
			return 1;
		}
		if (flags & 0x400) {
			return 2;
		}
		const auto u = a_file.extension().u8string();
		return Lower(std::string(u.begin(), u.end())) == ".esl" ? 1 : 0;
	}

	std::vector<SfKindChange> StarfieldKindChanges(const std::vector<std::string>& a_full, const std::vector<std::string>& a_lite,
		const std::vector<std::string>& a_medium, const std::vector<std::uint32_t>& a_formIDs, const StarfieldOrder& a_order)
	{
		std::vector<SfKindChange> out;
		// per list of the save: its plugins whose file has another kind now (-1 = cannot be told: left out)
		std::vector<int> nowFull(a_full.size(), -2), nowLite(a_lite.size(), -2), nowMed(a_medium.size(), -2);
		auto kindNow = [&](const std::string& a_name) {
			const auto it = a_order.files.find(Lower(a_name));
			return it == a_order.files.end() ? -1 : StarfieldPluginKind(it->second);
		};
		std::unordered_map<std::string, std::size_t> slot;  // "kind:index" -> out index
		auto entry = [&](int a_was, std::size_t a_idx, const std::string& a_name, std::vector<int>& a_cache) -> SfKindChange* {
			if (a_cache[a_idx] == -2) {
				a_cache[a_idx] = kindNow(a_name);
			}
			const int now = a_cache[a_idx];
			if (now < 0 || now == a_was) {
				return nullptr;
			}
			const auto key = std::to_string(a_was) + ":" + std::to_string(a_idx);
			auto       it = slot.find(key);
			if (it == slot.end()) {
				it = slot.emplace(key, out.size()).first;
				SfKindChange k;
				k.plugin = a_name;
				k.was = a_was;
				k.now = now;
				out.push_back(std::move(k));
			}
			return &out[it->second];
		};
		for (const auto id : a_formIDs) {
			const std::uint32_t top = id >> 24;
			SfKindChange*       k = nullptr;
			std::uint32_t       local = 0;
			if (top == 0xFF || id == 0) {
				continue;
			}
			if (top == 0xFE) {
				const auto i = (id >> 12) & 0xFFF;
				k = i < a_lite.size() ? entry(1, i, a_lite[i], nowLite) : nullptr;
				local = id & 0xFFF;
			} else if (top == 0xFD) {
				const auto i = (id >> 16) & 0xFF;
				k = i < a_medium.size() ? entry(2, i, a_medium[i], nowMed) : nullptr;
				local = id & 0xFFFF;
			} else {
				k = top < a_full.size() ? entry(0, top, a_full[top], nowFull) : nullptr;
				local = id & 0xFFFFFF;
			}
			if (!k) {
				continue;
			}
			++k->ids;
			const std::uint32_t limit = k->now == 1 ? 0xFFF : k->now == 2 ? 0xFFFF : 0xFFFFFF;
			if (local > limit) {
				if (k->outOfRange++ == 0) {
					k->example = id;
				}
			}
		}
		return out;
	}

	StarfieldOrder ReadStarfield(const std::filesystem::path& a_pluginsTxt, const std::vector<std::filesystem::path>& a_dataDirs)
	{
		StarfieldOrder o;
		for (const auto& dir : a_dataDirs) {
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				const auto u = e.path().filename().u8string();
				const auto name = Lower(std::string(u.begin(), u.end()));
				if ((name.ends_with(".esm") || name.ends_with(".esp") || name.ends_with(".esl")) && e.is_regular_file(ec)) {
					o.files.emplace(name, e.path());
				}
			}
		}
		for (const auto& b : StarfieldBaseFiles()) {
			if (o.installed(b)) {
				o.active.insert(b);
			}
		}
		// Starfield.ccc in the Data folder (the exe reads it: 0x142cc7d10; one plugin a line like Skyrim.ccc [U: format])
		for (const auto& dir : a_dataDirs) {
			std::ifstream ccc(dir / "Starfield.ccc");
			for (std::string line; std::getline(ccc, line);) {
				line = Lower(Trim(line));
				if (!line.empty() && line[0] != '#' && o.installed(line)) {
					o.active.insert(line);
				}
			}
			break;  // (the game's Data folder only)
		}
		// plugins.txt: the exe's reader 0x140425310 skips '#' lines, a leading '*' marks active
		std::ifstream f(a_pluginsTxt);
		if (f) {
			o.pluginsTxtFound = true;
			o.pluginsTxt = a_pluginsTxt;
			for (std::string line; std::getline(f, line);) {
				line = Trim(line);
				if (!line.empty() && line[0] == '*') {
					const auto n = Lower(Trim(line.substr(1)));
					if (o.installed(n)) {
						o.active.insert(n);
					}
				}
			}
		}
		return o;
	}

	std::vector<std::string> StarfieldListed(const std::filesystem::path& a_pluginsTxt, const std::vector<std::filesystem::path>& a_dataDirs)
	{
		const auto o = ReadStarfield(a_pluginsTxt, a_dataDirs);
		std::vector<std::string> out;
		std::set<std::string>    seen;
		auto add = [&](const std::string& a_name) {
			if (seen.insert(Lower(a_name)).second) {
				out.push_back(a_name);
			}
		};
		for (const auto& b : StarfieldBaseFiles()) {
			if (o.installed(b)) {
				add(o.files.at(b).filename().string());
			}
		}
		for (const auto& dir : a_dataDirs) {
			std::ifstream ccc(dir / "Starfield.ccc");
			for (std::string line; std::getline(ccc, line);) {
				line = Trim(line);
				if (!line.empty() && line[0] != '#' && o.installed(Lower(line))) {
					add(line);
				}
			}
			break;
		}
		std::ifstream f(a_pluginsTxt);
		for (std::string line; std::getline(f, line);) {
			line = Trim(line);
			if (!line.empty() && line[0] == '*') {
				add(Trim(line.substr(1)));
			}
		}
		return out;
	}
}

