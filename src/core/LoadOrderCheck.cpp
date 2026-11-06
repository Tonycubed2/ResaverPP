// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "LoadOrderCheck.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>

namespace ess::locheck
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
		std::uint32_t U32(const std::uint8_t* p)
		{
			std::uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
		}
		std::uint16_t U16(const std::uint8_t* p)
		{
			std::uint16_t v;
			std::memcpy(&v, p, 2);
			return v;
		}
		std::string Ver(float v)
		{
			char b[32];
			std::snprintf(b, sizeof(b), "%.2f", v);
			return b;
		}
		std::string Hex(std::uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "0x%03X", v);
			return b;
		}
	}

	Limits LimitsFor(Game a_game)
	{
		Limits l;
		switch (a_game) {
		case Game::kFallout4:
			l.maxFull = 254;  // 0x00..0xFD; 0xFE light block, 0xFF created (Fallout4.exe 0x2e6dc0, 0x2f91c0)
			l.maxLight = 4096;  // 12-bit light index (0x2f91c0: index << 12 inside 0x00FFF000)
			l.light = true;
			l.maxHedr = 1.0f;  // 0x2f67e0 / 0x2f7360
			l.extendedRangeHedr = 1.0f;  // xEdit wbDefinitionsFO4: HardcodedRangeMinVersion 1.0
			break;
		case Game::kSkyrimSE:
			l.maxFull = 254;
			l.maxLight = 4096;
			l.light = true;
			l.maxHedr = 1.71f;  // xEdit wbDefinitionsTES5: SSE 1.71 (game 1.6.1130+)
			l.extendedRangeHedr = 1.709f;  // xEdit: HardcodedRangeMinVersion 1.709
			break;
		case Game::kSkyrimLE:
			l.maxFull = 255;  // 0x00..0xFE; 0xFF created
			l.maxLight = 0;
			l.light = false;
			l.maxHedr = 1.7f;
			l.extendedRangeHedr = 99.0f;
			break;
		}
		return l;
	}

	PluginInfo ReadHeader(const std::filesystem::path& a_file, bool a_scanRecords, Game a_game)
	{
		PluginInfo p;
		p.path = a_file;
		p.name = a_file.filename().string();
		const auto low = Lower(p.name);
		p.eslExt = low.ends_with(".esl");
		std::ifstream f(a_file, std::ios::binary);
		if (!f) {
			throw std::runtime_error("cannot open the file");
		}
		std::uint8_t h[24];
		if (!f.read(reinterpret_cast<char*>(h), 24) || std::memcmp(h, "TES4", 4) != 0) {
			throw std::runtime_error("not a plugin (no TES4 header)");
		}
		const auto flags = U32(h + 8);
		p.formVersion = U16(h + 20);
		const auto limits = LimitsFor(a_game);
		// the game's own reading: the header's master and light flags, plus .esm/.esl (master) and .esl (light)
		p.esm = (flags & 1) || low.ends_with(".esm") || p.eslExt;
		p.eslFlag = (flags & 0x200) != 0;
		p.light = limits.light && (p.eslFlag || p.eslExt);
		std::vector<std::uint8_t> d(U32(h + 4));
		if (!f.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()))) {
			throw std::runtime_error("the plugin header is cut off");
		}
		std::uint32_t big = 0;
		for (std::size_t i = 0; i + 6 <= d.size();) {
			const char* sig = reinterpret_cast<const char*>(d.data() + i);
			std::uint32_t n = U16(d.data() + i + 4);
			i += 6;
			if (std::memcmp(sig, "XXXX", 4) == 0 && n == 4 && i + 4 <= d.size()) {
				big = U32(d.data() + i);
				i += 4;
				continue;
			}
			if (big) {
				n = big;
				big = 0;
			}
			if (i + n > d.size()) {
				break;
			}
			if (std::memcmp(sig, "HEDR", 4) == 0 && n >= 4) {
				std::memcpy(&p.hedr, d.data() + i, 4);
			} else if (std::memcmp(sig, "MAST", 4) == 0) {
				std::string m(reinterpret_cast<const char*>(d.data() + i), n);
				while (!m.empty() && m.back() == '\0') {
					m.pop_back();
				}
				p.masters.push_back(m);
			}
			i += n;
		}
		if (!a_scanRecords) {
			return p;
		}
		const bool extended = p.hedr >= limits.extendedRangeHedr - 1e-4f && !p.masters.empty();
		const auto self = static_cast<std::uint32_t>(p.masters.size());
		while (f.read(reinterpret_cast<char*>(h), 24)) {
			if (std::memcmp(h, "GRUP", 4) == 0) {
				continue;  // step into the group
			}
			const auto id = U32(h + 12);
			if ((id >> 24) >= self) {
				const auto obj = id & 0xFFFFFF;
				++p.newRecords;
				p.maxObjectID = std::max(p.maxObjectID, obj);
				p.overRange += obj > 0xFFF ? 1 : 0;
				p.underFloor += (!extended && obj < 0x800) ? 1 : 0;
			}
			f.seekg(U32(h + 4), std::ios::cur);
		}
		return p;
	}

	bool Result::hasErrors() const { return count(Severity::kError) > 0; }
	std::size_t Result::count(Severity a_s) const
	{
		return static_cast<std::size_t>(std::count_if(findings.begin(), findings.end(), [&](const Finding& f) { return f.severity == a_s; }));
	}

	const char* SeverityWord(Severity a_s)
	{
		switch (a_s) {
		case Severity::kError: return "ERROR";
		case Severity::kWarning: return "WARNING";
		default: return "INFO";
		}
	}

	Result Check(const Options& a_opt)
	{
		Result r;
		r.game = a_opt.game;
		r.limits = LimitsFor(a_opt.game);
		const auto& L = r.limits;
		const bool  fo4 = a_opt.game == Game::kFallout4;
		const char* gameName = fo4 ? "Fallout 4" : a_opt.game == Game::kSkyrimLE ? "Skyrim Legendary Edition" : "Skyrim Special Edition";
		auto add = [&](Severity s, const std::string& plugin, const std::string& text) { r.findings.push_back({ s, plugin, text }); };

		std::map<std::string, std::size_t> pos;
		for (std::size_t i = 0; i < a_opt.loadOrder.size(); ++i) {
			pos.emplace(Lower(a_opt.loadOrder[i]), i);
		}

		for (std::size_t i = 0; i < a_opt.loadOrder.size(); ++i) {
			const auto& name = a_opt.loadOrder[i];
			PluginInfo  p;
			p.name = name;
			const auto path = a_opt.findPlugin ? a_opt.findPlugin(name) : std::nullopt;
			if (!path) {
				++r.missing;
				add(Severity::kWarning, name, name + " is in the load order but its file was not found: the game skips it (and every plugin "
				                                     "that needs it).");
				const auto l = Lower(name);
				p.eslExt = l.ends_with(".esl");
				p.light = L.light && p.eslExt;
			} else {
				const auto l = Lower(name);
				const bool maybeLight = l.ends_with(".esl");
				try {
					// records are read only for light plugins (their numbers matter); a header tells if it is light
					p = ReadHeader(*path, false, a_opt.game);
					if (p.light) {
						p = ReadHeader(*path, true, a_opt.game);
					}
				} catch (const std::exception& e) {
					p = PluginInfo{};
					p.path = *path;
					p.error = e.what();
					p.eslExt = maybeLight;
					p.light = L.light && maybeLight;
					add(Severity::kError, name, name + " cannot be read: " + e.what() + ".");
				}
				p.name = name;
			}
			p.slot = static_cast<int>(p.light ? r.light : r.full);
			(p.light ? r.light : r.full)++;
			r.plugins.push_back(std::move(p));
		}

		// limits
		if (r.full > L.maxFull) {
			std::string over;
			std::size_t k = 0;
			for (const auto& p : r.plugins) {
				if (!p.light && p.slot >= static_cast<int>(L.maxFull)) {
					over += (k++ ? ", " : "") + p.name;
					if (k >= 8) {
						over += ", ...";
						break;
					}
				}
			}
			add(Severity::kError, "", std::string(gameName) + " loads at most " + std::to_string(L.maxFull) + " full plugins (.esm/.esp not flagged light), "
			                          "and this load order has " + std::to_string(r.full) + ". The ones beyond the limit (" + over + ") do not get "
			                          "a slot: make some of them light (ESL flag, after compacting their form IDs in xEdit) or merge them.");
		}
		if (L.light && r.light > L.maxLight) {
			add(Severity::kError, "", std::string(gameName) + " loads at most " + std::to_string(L.maxLight) + " light plugins, and this load order has " +
			                          std::to_string(r.light) + ".");
		}

		for (std::size_t i = 0; i < r.plugins.size(); ++i) {
			const auto& p = r.plugins[i];
			if (p.path.empty() || !p.error.empty()) {
				continue;
			}
			// masters
			for (const auto& m : p.masters) {
				auto it = pos.find(Lower(m));
				if (it == pos.end()) {
					add(Severity::kError, p.name, p.name + " needs " + m + ", which is not in the load order. The game cannot load " + p.name +
					                                  (fo4 ? " (Fallout 4 leaves it out, or stops at the loading screen)." : " (Skyrim crashes at the start)."));
				} else if (it->second > i) {
					add(Severity::kError, p.name, p.name + " needs " + m + ", but " + m + " loads after it. A master must load before the plugins "
					                                  "that use it: move " + m + " up (or " + p.name + " down).");
				}
			}
			// header version
			if (p.hedr > L.maxHedr + 1e-4f) {
				if (fo4) {
					add(Severity::kError, p.name, p.name + " has header version " + Ver(p.hedr) + "; Fallout 4 loads plugins of version 1.0 at most "
					                                  "(0.95 is Fallout 4 VR's). The game does not load it, nor the plugins that need it. It was saved by a "
					                                  "newer Creation Kit (or another game's); get the version made for your game.");
				} else if (a_opt.game == Game::kSkyrimSE) {
					add(Severity::kError, p.name, p.name + " has header version " + Ver(p.hedr) + "; Skyrim Special Edition knows 1.71 at most. It "
					                                  "was not made for this game.");
				}
			} else if (a_opt.game == Game::kSkyrimSE && p.hedr > 1.7f + 1e-4f) {
				add(Severity::kInfo, p.name, p.name + " has header version 1.71: it needs Skyrim 1.6.1130 or newer (older versions do not load it).");
			}
			if (a_opt.game == Game::kSkyrimLE && p.formVersion >= 44) {
				add(Severity::kWarning, p.name, p.name + " is saved in Skyrim Special Edition's format (record version " + std::to_string(p.formVersion) +
				                                    "). Legendary Edition can crash on it: use the mod's Legendary Edition version.");
			}
			if (a_opt.game == Game::kSkyrimLE && (p.eslFlag || p.eslExt)) {
				add(Severity::kWarning, p.name, p.name + " is a light plugin (" + (p.eslExt ? std::string(".esl") : std::string("ESL flag")) +
				                                    "), but Legendary Edition has no light plugins.");
			}
			// light plugins: their new forms must fit in 12 bits
			if (p.light) {
				if (p.overRange) {
					add(Severity::kError, p.name, p.name + " is light (" + (p.eslFlag ? std::string("ESL flag") : std::string(".esl")) + ") but numbers " +
					                                  std::to_string(p.overRange) + " of its " + std::to_string(p.newRecords) +
					                                  " new forms above 0xFFF (highest " + Hex(p.maxObjectID) +
					                                  "). The game keeps only the last three hex digits of a light plugin's form IDs, so those forms "
					                                  "land on other forms or on nothing. Compact it in xEdit (Compact FormIDs for ESL) or remove "
					                                  "the ESL flag.");
				}
				if (p.underFloor) {
					add(Severity::kWarning, p.name, p.name + " is light and numbers " + std::to_string(p.underFloor) +
					                                    " new forms below 0x800, which the Creation Kit and xEdit only allow from header version " +
					                                    Ver(L.extendedRangeHedr) + " with a master. The game itself takes them; check the mod with xEdit.");
				}
			}
		}

		char sum[256];
		if (L.light) {
			std::snprintf(sum, sizeof(sum), "%zu plugins: %zu full of %zu, %zu light of %zu%s.", r.plugins.size(), r.full, L.maxFull, r.light, L.maxLight,
				r.missing ? (", " + std::to_string(r.missing) + " not found").c_str() : "");
		} else {
			std::snprintf(sum, sizeof(sum), "%zu plugins of %zu%s.", r.plugins.size(), L.maxFull,
				r.missing ? (", " + std::to_string(r.missing) + " not found").c_str() : "");
		}
		add(Severity::kInfo, "", sum);
		std::stable_sort(r.findings.begin(), r.findings.end(), [](const Finding& a, const Finding& b) { return a.severity > b.severity; });
		return r;
	}

	std::vector<std::string> Describe(const Result& a_result)
	{
		std::vector<std::string> out;
		for (const auto& f : a_result.findings) {
			out.push_back(std::string(SeverityWord(f.severity)) + ": " + f.text);
		}
		out.push_back("");
		out.push_back("Load order:");
		for (const auto& p : a_result.plugins) {
			char b[512];
			char slot[16];
			if (p.light) {
				std::snprintf(slot, sizeof(slot), "FE %03X", p.slot);
			} else {
				std::snprintf(slot, sizeof(slot), "%02X", p.slot);
			}
			std::snprintf(b, sizeof(b), "  %-7s %-50s %s%s%s HEDR %.2f%s", slot, p.name.c_str(), p.esm ? "M" : "-", p.light ? "L" : "-",
				p.eslFlag ? "f" : "-", p.hedr, p.path.empty() ? "  (not found)" : "");
			out.push_back(b);
		}
		return out;
	}
}
