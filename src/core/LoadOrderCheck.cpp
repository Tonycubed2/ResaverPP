// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "LoadOrderCheck.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
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
			// checked in SkyrimSE.exe 1.6.1170 (decrypted): TESDataHandler::AddCompiledFile 0x1b8580 puts a light file
			// (IsLight 0x1c8d50 = TESFile flags +0x438 bit 0x200) in the small list (compile index 0xFE, small index u16 at
			// +0x47a), any other in the full list (compile index = position as a byte); GetFileForFormID 0x1bdfe0 reads top
			// byte 0xFE as "light, index = (id >> 12) & 0xFFF", any other top byte as the full list position (0xFF =
			// created): 254 full (00..FD) + 4096 light. A light file's form ids are FE000000 | index << 12 | (id & 0xFFF)
			// (0x1c8d70, 0x1c9b91).
			l.maxFull = 254;
			l.maxLight = 4096;
			l.light = true;
			l.maxHedr = 1.71f;  // 0x1c6220: loaded only when 1.7101 >= HEDR version ("File %s is a higher version"), else the
			                    // file's active bits (4, 8) are cleared (0x1c6fcf): not loaded
			l.extendedRangeHedr = 1.709f;  // xEdit wbDefinitionsTES5: HardcodedRangeMinVersion 1.709 (a Creation Kit / xEdit
			                               // rule: the game's loader takes any 12-bit id of a light file)
			break;
		case Game::kStarfield:
			// the save's three plugin lists (FORMAT 2.2) and TESFile::AdjustFormID (Starfield.exe 0x1405b78c0, LOADER.md 2.5):
			// a light plugin keeps the low 12 bits of a form id (FE xxx yyy), a medium one the low 16 (FD xx yyyy), a full one
			// 24 (00..FC; FD medium, FE light, FF made in game)
			l.maxFull = 253;
			l.maxLight = 4096;
			l.maxMedium = 256;
			l.light = true;
			l.maxHedr = 1000.0f;           // (no header version limit checked for Starfield)
			l.extendedRangeHedr = 0.0f;    // (no low-id rule checked for Starfield)
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
		// (Fallout4.exe 0x2f6217..0x2f62ba; SkyrimSE.exe 1.6.1170 0x1c5c4f..0x1c5cf2: header flags & 0x291 kept, then
		// "esm" or "esl" -> | 1, "esl" -> | 0x200)
		// Legendary Edition: the header flag alone (TESV.exe 1.9.32.0 IsMaster 0x4446d0 = flags & 1; its file opener does not
		// look at the extension)
		p.esm = (flags & 1) || (a_game != Game::kSkyrimLE && (low.ends_with(".esm") || p.eslExt));
		if (a_game == Game::kStarfield) {
			// Starfield.exe 0x1405b70ac (StarfieldPluginKind): light = flag 0x100 or .esl, medium = flag 0x400
			p.eslFlag = (flags & 0x100) != 0;
			p.light = p.eslFlag || p.eslExt;
			p.medium = !p.light && (flags & 0x400) != 0;
		} else {
			p.eslFlag = (flags & 0x200) != 0;
			p.light = limits.light && (p.eslFlag || p.eslExt);
		}
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
		std::set<std::uint32_t> kept;  // Starfield: the low bits the game keeps (AdjustFormID)
		while (f.read(reinterpret_cast<char*>(h), 24)) {
			if (std::memcmp(h, "GRUP", 4) == 0) {
				continue;  // step into the group
			}
			const auto id = U32(h + 12);
			if ((id >> 24) >= self) {
				const auto obj = id & 0xFFFFFF;
				++p.newRecords;
				p.maxObjectID = std::max(p.maxObjectID, obj);
				p.overRange += obj > (p.medium ? 0xFFFFu : 0xFFFu) ? 1 : 0;
				if (a_game == Game::kStarfield && !kept.insert(obj & (p.medium ? 0xFFFFu : 0xFFFu)).second) {
					++p.collisions;
				}
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
		const bool  sf = a_opt.game == Game::kStarfield;
		const char* gameName = sf ? "Starfield" : fo4 ? "Fallout 4" : a_opt.game == Game::kSkyrimLE ? "Skyrim Legendary Edition" : "Skyrim Special Edition";
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
					if (p.light || p.medium) {
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
			r.plugins.push_back(std::move(p));
		}

		// slots in the order the game loads (masters first, a master's masters moved up before it): GameOrder
		std::vector<OrderEntry> entries;
		entries.reserve(r.plugins.size());
		for (const auto& p : r.plugins) {
			entries.push_back({ p.name, !p.path.empty() && p.error.empty() && p.esm, p.masters });
		}
		const auto order = GameOrder(entries);
		for (std::size_t k = 0; k < order.size(); ++k) {
			auto& p = r.plugins[order[k]];
			p.loadPos = static_cast<int>(k);
			// a plugin whose file is missing is not loaded (the game skips it), so it takes no slot
			if (p.path.empty()) {
				p.slot = -1;
			} else {
				auto& n = p.light ? r.light : p.medium ? r.medium : r.full;
				p.slot = static_cast<int>(n);
				++n;
			}
		}

		// limits
		if (r.full > L.maxFull) {
			std::string over;
			std::size_t k = 0;
			for (const auto& p : r.plugins) {
				if (!p.light && !p.medium && p.slot >= static_cast<int>(L.maxFull)) {
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
		if (L.maxMedium && r.medium > L.maxMedium) {
			add(Severity::kError, "", std::string(gameName) + " loads at most " + std::to_string(L.maxMedium) + " medium plugins, and this load order has " +
			                          std::to_string(r.medium) + ".");
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
					                                  (sf    ? " (Starfield leaves it out)."
					                                   : fo4 ? " (Fallout 4 leaves it out, or stops at the loading screen)."
					                                         : " (Skyrim crashes at the start)."));
				} else if (sf) {
					// (the order Starfield loads in, masters first or not, is not checked in Starfield.exe: no late-master finding)
				} else if (r.plugins[it->second].loadPos > p.loadPos) {
					r.findings.push_back({ Severity::kError, p.name,
						p.name + " needs " + m + ", but " + m + " loads after it. A master must load before the plugins that use it: move " + m +
							" up (or " + p.name + " down).",
						r.plugins[it->second].name });
				} else if (it->second > i) {
					// listed after it, but the game loads it first anyway (a master, or a master's master moved up)
					add(Severity::kInfo, p.name, p.name + " needs " + m + ", which is listed after it, but the game loads " + m + " first (" +
					                                 (r.plugins[it->second].esm ? "it is a master: masters load before plain plugins"
					                                                            : std::string("it is a master of the master ") + p.name) +
					                                 "). Nothing to fix.");
				}
			}
			// header version
			if (p.hedr > L.maxHedr + 1e-4f) {
				if (fo4) {
					add(Severity::kError, p.name, p.name + " has header version " + Ver(p.hedr) + "; Fallout 4 loads plugins of version 1.0 at most "
					                                  "(0.95 is Fallout 4 VR's). The game does not load it, nor the plugins that need it. It was saved by a "
					                                  "newer Creation Kit (or another game's); get the version made for your game.");
				} else if (a_opt.game == Game::kSkyrimSE) {
					add(Severity::kError, p.name, p.name + " has header version " + Ver(p.hedr) + "; Skyrim Special Edition loads 1.71 at most. "
					                                  "The game does not load it, nor the plugins that need it. It was not made for this game.");
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
			// Starfield: a medium plugin's new forms must fit in 16 bits, a light one's in 12
			if (sf && (p.light || p.medium)) {
				// the game keeps the low bits only (TESFile::AdjustFormID): numbers above the range are harmless as long as no two
				// new forms keep the same low bits (Bethesda's own SFBGS047.esm numbers 0x1000..0x102A)
				if (p.collisions) {
					add(Severity::kError, p.name, p.name + " is " + (p.medium ? "medium (flag 0x400)" : p.eslExt ? "light (.esl)" : "light (flag 0x100)") +
					                                  " and " + std::to_string(p.collisions) + " of its " + std::to_string(p.newRecords) +
					                                  " new forms end in the same last " + (p.medium ? "four" : "three") +
					                                  " hex digits as another of its new forms (highest " + Hex(p.maxObjectID) +
					                                  "). The game keeps only those digits, so these forms land on each other. Compact it in xEdit "
					                                  "or remove the flag.");
				}
				continue;
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
		if (sf) {
			std::snprintf(sum, sizeof(sum), "%zu plugins: %zu full of %zu, %zu medium of %zu, %zu light of %zu%s.", r.plugins.size(), r.full, L.maxFull,
				r.medium, L.maxMedium, r.light, L.maxLight, r.missing ? (", " + std::to_string(r.missing) + " not found").c_str() : "");
		} else if (L.light) {
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

	std::vector<std::size_t> GameOrder(const std::vector<OrderEntry>& a_listed)
	{
		const auto n = a_listed.size();
		std::map<std::string, std::size_t> byName;
		for (std::size_t i = 0; i < n; ++i) {
			byName.emplace(Lower(a_listed[i].name), i);
		}
		// masters first, plain plugins after, each in the listed order (SkyrimSE.exe 0x1ba8fc / 0x1baa7a)
		std::vector<std::size_t> L;
		L.reserve(n);
		for (std::size_t i = 0; i < n; ++i) {
			if (a_listed[i].master) L.push_back(i);
		}
		for (std::size_t i = 0; i < n; ++i) {
			if (!a_listed[i].master) L.push_back(i);
		}
		// each master's masters found further down are moved just before it (SkyrimSE.exe 0x1b8f50): the search starts at
		// the current node; a moved master is inserted at the current node, the next one of the same file before it, and
		// the walk stays on the node (now the last moved file) until a pass moves nothing
		std::size_t guard = n * n + 16;  // the game itself would never finish on a cycle of masters
		for (std::size_t i = 0; i < L.size() && guard; --guard) {
			const auto f = L[i];
			bool       moved = false;
			if (a_listed[f].master) {
				for (const auto& m : a_listed[f].masters) {
					const auto it = byName.find(Lower(m));
					if (it == byName.end()) {
						continue;
					}
					const auto j = std::find(L.begin() + static_cast<std::ptrdiff_t>(i), L.end(), it->second);
					if (j == L.end() || j == L.begin() + static_cast<std::ptrdiff_t>(i)) {
						continue;
					}
					const auto v = *j;
					L.erase(j);
					L.insert(L.begin() + static_cast<std::ptrdiff_t>(i), v);
					moved = true;
				}
			}
			if (!moved) {
				++i;
			}
		}
		return L;
	}

	std::vector<std::pair<std::size_t, std::size_t>> LateMasters(const std::vector<OrderEntry>& a_listed)
	{
		const auto order = GameOrder(a_listed);
		std::vector<std::size_t> pos(a_listed.size());
		for (std::size_t k = 0; k < order.size(); ++k) {
			pos[order[k]] = k;
		}
		std::map<std::string, std::size_t> byName;
		for (std::size_t i = 0; i < a_listed.size(); ++i) {
			byName.emplace(Lower(a_listed[i].name), i);
		}
		std::vector<std::pair<std::size_t, std::size_t>> out;
		for (std::size_t i = 0; i < a_listed.size(); ++i) {
			for (const auto& m : a_listed[i].masters) {
				const auto it = byName.find(Lower(m));
				if (it != byName.end() && it->second != i && pos[it->second] > pos[i]) {
					out.emplace_back(i, it->second);
				}
			}
		}
		return out;
	}

	FixPlan PlanFix(const std::vector<OrderEntry>& a_listed)
	{
		FixPlan    plan;
		const auto n = a_listed.size();
		std::map<std::string, std::size_t> byName;
		for (std::size_t i = 0; i < n; ++i) {
			byName.emplace(Lower(a_listed[i].name), i);
		}
		// a cycle of masters (A needs B, B needs A): no order can load each before the other
		{
			std::vector<int>         state(n, 0);  // 0 new, 1 on the path, 2 done
			std::vector<std::size_t> path;
			std::string              cycle;
			std::function<bool(std::size_t)> dfs = [&](std::size_t a_i) -> bool {
				state[a_i] = 1;
				path.push_back(a_i);
				for (const auto& m : a_listed[a_i].masters) {
					const auto it = byName.find(Lower(m));
					if (it == byName.end() || it->second == a_i) {
						continue;
					}
					if (state[it->second] == 1) {
						const auto from = std::find(path.begin(), path.end(), it->second);
						for (auto k = from; k != path.end(); ++k) {
							cycle += a_listed[*k].name + " needs ";
						}
						cycle += a_listed[it->second].name;
						return true;
					}
					if (state[it->second] == 0 && dfs(it->second)) {
						return true;
					}
				}
				path.pop_back();
				state[a_i] = 2;
				return false;
			};
			for (std::size_t i = 0; i < n && cycle.empty(); ++i) {
				if (state[i] == 0) {
					dfs(i);
				}
			}
			if (!cycle.empty()) {
				plan.error = "These plugins need each other (" + cycle + "), so no order loads each master first. Nothing was changed: one "
				             "of them has to be fixed (or removed) in xEdit or by its author.";
				return plan;
			}
		}

		std::vector<std::size_t> L(n);  // listed order, as indices into a_listed
		for (std::size_t i = 0; i < n; ++i) {
			L[i] = i;
		}
		auto entriesOf = [&]() {
			std::vector<OrderEntry> e;
			e.reserve(n);
			for (const auto i : L) {
				e.push_back(a_listed[i]);
			}
			return e;
		};
		std::set<std::pair<std::size_t, std::size_t>> done;  // (master, plugin) pairs already moved for
		for (std::size_t guard = n * n + 16; guard; --guard) {
			const auto late = LateMasters(entriesOf());  // indices into L
			if (late.empty()) {
				plan.ok = true;
				break;
			}
			// the first plugin (listed order) with a late master; its master goes before the first plugin that needs it late
			std::size_t first = n, master = n;
			for (const auto& [p, m] : late) {
				if (p < first) {
					first = p;
					master = m;
				}
			}
			std::size_t target = first;
			for (const auto& [p, m] : late) {
				if (m == master && p < target) {
					target = p;
				}
			}
			const auto mi = L[master], ti = L[target];
			if (!done.insert({ mi, ti }).second) {
				plan.error = a_listed[ti].name + " needs " + a_listed[mi].name + ", and the game still loads it after " + a_listed[ti].name +
				             " wherever it is listed (a master's master the game moves up itself). Nothing was changed.";
				return plan;
			}
			Move mv;
			mv.plugin = a_listed[mi].name;
			mv.from = master;
			mv.before = a_listed[ti].name;
			mv.beforeAt = target;
			plan.moves.push_back(mv);
			L.erase(L.begin() + static_cast<std::ptrdiff_t>(master));
			L.insert(L.begin() + static_cast<std::ptrdiff_t>(target < master ? target : target - 1), mi);
		}
		if (!plan.ok) {
			plan.error = "The masters could not be put in order (too many moves). Nothing was changed.";
			plan.moves.clear();
			return plan;
		}
		for (const auto i : L) {
			plan.order.push_back(a_listed[i].name);
		}
		return plan;
	}

	namespace
	{
		std::string SlotLabel(const PluginInfo& p, std::size_t a_listed)
		{
			char b[32];
			if (p.slot < 0) {
				std::snprintf(b, sizeof(b), "#%zu", a_listed + 1);
			} else if (p.light) {
				std::snprintf(b, sizeof(b), "#FE %03X", p.slot);
			} else {
				std::snprintf(b, sizeof(b), "#%02X", p.slot);
			}
			return b;
		}
	}

	FixPlan PlanFix(const Result& a_result, const std::vector<std::string>& a_enabledLines)
	{
		std::vector<OrderEntry> entries;
		std::map<std::string, std::string> label;
		for (std::size_t i = 0; i < a_enabledLines.size(); ++i) {
			label.emplace(Lower(a_enabledLines[i]), "#" + std::to_string(i));
		}
		for (std::size_t i = 0; i < a_result.plugins.size(); ++i) {
			const auto& p = a_result.plugins[i];
			const bool  read = !p.path.empty() && p.error.empty();
			entries.push_back({ p.name, read && p.esm, read ? p.masters : std::vector<std::string>{} });
			label.emplace(Lower(p.name), SlotLabel(p, i));  // kept when plugins.txt numbered it
		}
		auto plan = PlanFix(entries);
		for (auto& m : plan.moves) {
			m.fromLabel = label[Lower(m.plugin)];
			m.beforeLabel = label[Lower(m.before)];
		}
		return plan;
	}

	std::string MoveText(const Move& a_move)
	{
		const auto from = a_move.fromLabel.empty() ? "#" + std::to_string(a_move.from + 1) : a_move.fromLabel;
		const auto at = a_move.beforeLabel.empty() ? "#" + std::to_string(a_move.beforeAt + 1) : a_move.beforeLabel;
		return a_move.plugin + ": " + from + " -> before " + a_move.before + " " + at;
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
			if (p.slot < 0) {
				std::snprintf(slot, sizeof(slot), "--");
			} else if (p.light) {
				std::snprintf(slot, sizeof(slot), "FE %03X", p.slot);
			} else if (p.medium) {
				std::snprintf(slot, sizeof(slot), "FD %02X", p.slot);
			} else {
				std::snprintf(slot, sizeof(slot), "%02X", p.slot);
			}
			std::snprintf(b, sizeof(b), "  %-7s %-50s %s%s%s HEDR %.2f%s", slot, p.name.c_str(), p.esm ? "M" : "-", p.light ? "L" : "-",
				p.eslFlag ? "f" : "-", p.hedr, p.path.empty() ? "  (not found)" : "");
			out.push_back(b);
		}
		return out;
	}

	MasterGraph BuildMasterGraph(const Result& a_result)
	{
		MasterGraph g;
		const auto& P = a_result.plugins;
		const bool  sf = a_result.game == Game::kStarfield;
		g.masters.resize(P.size());
		g.dependents.resize(P.size());
		std::map<std::string, std::size_t> pos;
		for (std::size_t i = 0; i < P.size(); ++i) {
			pos.emplace(Lower(P[i].name), i);
		}
		std::map<std::string, std::pair<std::string, std::vector<std::size_t>>> absent;  // lower name -> (name, plugins)
		for (std::size_t i = 0; i < P.size(); ++i) {
			std::set<std::size_t> seen;  // a master named twice counts once
			for (const auto& m : P[i].masters) {
				MasterLink l;
				l.name = m;
				const auto it = pos.find(Lower(m));
				if (it == pos.end()) {
					auto& a = absent[Lower(m)];
					if (a.first.empty()) {
						a.first = m;
					}
					if (a.second.empty() || a.second.back() != i) {
						a.second.push_back(i);
					}
				} else if (it->second != i) {
					l.index = static_cast<int>(it->second);
					l.late = !sf && P[it->second].loadPos > P[i].loadPos && P[i].loadPos >= 0;
					if (seen.insert(it->second).second) {
						g.dependents[it->second].push_back(i);  // (i grows: load order kept)
					}
				}
				g.masters[i].push_back(std::move(l));
			}
		}
		for (auto& [low, a] : absent) {
			g.absent.push_back(std::move(a));
		}
		return g;
	}

	namespace
	{
		std::vector<std::size_t> Closure(const MasterGraph& a_graph, std::vector<std::size_t> a_todo, std::size_t a_skip)
		{
			std::vector<char> in(a_graph.dependents.size(), 0);
			if (a_skip < in.size()) {
				in[a_skip] = 1;  // (a cycle back to the plugin itself)
			}
			std::vector<std::size_t> out;
			while (!a_todo.empty()) {
				const auto d = a_todo.back();
				a_todo.pop_back();
				if (d >= in.size() || in[d]) {
					continue;
				}
				in[d] = 1;
				out.push_back(d);
				for (const auto e : a_graph.dependents[d]) {
					a_todo.push_back(e);
				}
			}
			std::sort(out.begin(), out.end());
			return out;
		}
	}

	std::vector<std::size_t> AllDependents(const MasterGraph& a_graph, std::size_t a_plugin)
	{
		if (a_plugin >= a_graph.dependents.size()) {
			return {};
		}
		return Closure(a_graph, a_graph.dependents[a_plugin], a_plugin);
	}

	std::vector<std::size_t> AllDependentsOfAbsent(const MasterGraph& a_graph, std::size_t a_absent)
	{
		if (a_absent >= a_graph.absent.size()) {
			return {};
		}
		return Closure(a_graph, a_graph.absent[a_absent].second, SIZE_MAX);
	}
}
