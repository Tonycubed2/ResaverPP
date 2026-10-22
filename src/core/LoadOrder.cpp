// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "LoadOrder.h"

#include <cctype>
#include <cstdlib>
#include <fstream>

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

	std::filesystem::path GamePluginsTxt(bool a_vr)
	{
		if (const char* la = std::getenv("LOCALAPPDATA")) {
			return std::filesystem::path(la) / (a_vr ? "Skyrim VR" : "Skyrim Special Edition") / "plugins.txt";
		}
		return {};
	}

	Active Read(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_gameFolder, bool a_vr)
	{
		Active a;
		std::ifstream f(a_pluginsTxt);
		if (!f) {
			return a;
		}
		a.known = true;
		a.file = a_pluginsTxt;
		for (std::string line; std::getline(f, line);) {
			line = Trim(line);
			if (line.empty() || line[0] == '#') {
				continue;
			}
			if (line[0] == '*') {
				a.plugins.insert(Lower(Trim(line.substr(1))));
			}
		}
		// always loaded, never listed in plugins.txt
		for (const char* p : { "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm", "dragonborn.esm", "_resourcepack.esl" }) {
			a.plugins.insert(p);
		}
		if (a_vr) {
			a.plugins.insert("skyrimvr.esm");
		}
		// Creation Club content of the installed edition (Anniversary Edition: Skyrim.ccc lists it)
		std::ifstream ccc(a_gameFolder / "Skyrim.ccc");
		for (std::string line; std::getline(ccc, line);) {
			line = Trim(line);
			if (!line.empty()) {
				a.plugins.insert(Lower(line));
			}
		}
		return a;
	}
}
