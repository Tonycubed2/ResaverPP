// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "AssetConflicts.h"

#include "ScriptSources.h"  // ListBSA

#include <algorithm>
#include <fstream>
#include <map>
#include <cctype>
#include <set>
#include <sstream>
#include <unordered_map>

namespace ess::assets
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
		std::string U8(const std::filesystem::path& p)
		{
			const auto s = p.u8string();
			return std::string(s.begin(), s.end());
		}

		struct Slot
		{
			std::uint32_t                first = 0;  // the first source that has it
			std::int32_t                 conflict = -1;
		};
	}

	Result Find(const Input& a_in, Progress* a_progress)
	{
		Result r;
		std::unordered_map<std::uint64_t, Slot> seen;
		seen.reserve(1 << 20);
		std::hash<std::string> hasher;
		auto add = [&](const std::string& a_path, std::uint32_t a_source) {
			++r.files;
			auto [it, fresh] = seen.try_emplace(static_cast<std::uint64_t>(hasher(a_path)), Slot{ a_source, -1 });
			if (fresh) {
				return;
			}
			auto& slot = it->second;
			if (slot.conflict < 0) {
				slot.conflict = static_cast<std::int32_t>(r.conflicts.size());
				r.conflicts.push_back({ a_path, { slot.first } });
			}
			auto& c = r.conflicts[static_cast<std::size_t>(slot.conflict)];
			if (c.sources.back() != a_source) {
				c.sources.push_back(a_source);
			}
		};
		auto cancelled = [&] { return a_progress && a_progress->cancel; };

		// where each archive is: the highest-priority folder that has it
		std::vector<std::pair<std::string, std::filesystem::path>> folders;  // low priority first
		if (!a_in.gameData.empty()) {
			folders.push_back({ "Game Data", a_in.gameData });
		}
		for (const auto& m : a_in.modsLowFirst) {
			folders.push_back({ m, a_in.modsDir / std::filesystem::path(reinterpret_cast<const char8_t*>(m.c_str())) });
		}
		if (!a_in.overwrite.empty()) {
			folders.push_back({ "Overwrite", a_in.overwrite });
		}
		std::unordered_map<std::string, std::pair<std::string, std::filesystem::path>> archiveAt;  // lowercase name -> (mod, file)
		for (const auto& [mod, dir] : folders) {
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (e.is_regular_file(ec) && Lower(U8(e.path().extension())) == (a_in.fo4 ? ".ba2" : ".bsa")) {
					archiveAt[Lower(U8(e.path().filename()))] = { mod, e.path() };
				}
			}
		}
		// the archives in the order the game loads them
		std::vector<std::string> archives;
		{
			std::vector<std::string> vanilla;
			for (const auto& [name, at] : archiveAt) {
				if (name.rfind(a_in.fo4 ? "fallout4 - " : "skyrim - ", 0) == 0) vanilla.push_back(name);
			}
			std::sort(vanilla.begin(), vanilla.end());
			if (a_in.fo4 && !a_in.gameData.empty()) {
				// Fallout 4 loads its own archives in the order its ini lists them ([Archive] in Fallout4_Default.ini, the
				// file the game's Fallout4.ini starts from): the start-up archives, then the archive lists, then the
				// texture index; the base game archives no list names come after them
				std::vector<std::string> listed;
				std::ifstream            ini(a_in.gameData.parent_path() / "Fallout4_Default.ini");
				std::map<std::string, std::string> lists;
				for (std::string line; std::getline(ini, line);) {
					const auto eq = line.find('=');
					if (eq != std::string::npos) {
						lists[Lower(line.substr(0, eq))] = line.substr(eq + 1);
					}
				}
				for (const char* key : { "sresourcestartuparchivelist", "sresourcearchivelist", "sresourcearchivelist2", "sresourceindexfilelist" }) {
					std::stringstream ss(lists[key]);
					for (std::string n; std::getline(ss, n, ',');) {
						while (!n.empty() && (n.front() == ' ' || n.front() == '\t')) n.erase(n.begin());
						while (!n.empty() && (n.back() == ' ' || n.back() == '\r' || n.back() == '\t')) n.pop_back();
						n = Lower(n);
						if (archiveAt.contains(n) && std::find(listed.begin(), listed.end(), n) == listed.end()) {
							listed.push_back(n);
						}
					}
				}
				for (const auto& v : vanilla) {
					if (std::find(listed.begin(), listed.end(), v) == listed.end()) {
						listed.push_back(v);
					}
				}
				vanilla = listed;
			}
			archives = vanilla;
			for (const auto& p : a_in.loadOrder) {
				const auto stem = Lower(U8(std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str())).stem()));
				std::vector<std::string> names;
				if (a_in.fo4) {
					names = { stem + " - main.ba2", stem + " - textures.ba2" };
					std::vector<std::string> voices;  // "<plugin> - Voices_en.ba2" (the DLC's way)
					for (const auto& [name, at] : archiveAt) {
						if (name.rfind(stem + " - voices_", 0) == 0) voices.push_back(name);
					}
					std::sort(voices.begin(), voices.end());
					names.insert(names.end(), voices.begin(), voices.end());
				} else {
					names = { stem + ".bsa", stem + " - textures.bsa" };
				}
				for (const auto& n : names) {
					if (archiveAt.contains(n) && std::find(archives.begin(), archives.end(), n) == archives.end()) {
						archives.push_back(n);
					}
				}
			}
		}
		if (a_progress) {
			a_progress->total = archives.size() + folders.size();
			a_progress->done = 0;
		}
		for (const auto& n : archives) {
			if (cancelled()) return r;
			const auto& [mod, file] = archiveAt[n];
			if (a_progress) a_progress->set(U8(file.filename()));
			const auto src = static_cast<std::uint32_t>(r.sources.size());
			r.sources.push_back({ mod, U8(file.filename()) });
			try {
				for (const auto& path : ListBSA(file)) {
					add(path, src);
				}
				++r.archives;
			} catch (const std::exception& e) {
				r.errors.push_back(U8(file) + ": " + e.what());
			}
			if (a_progress) ++a_progress->done;
		}
		// loose files above every archive, by mod priority
		for (const auto& [mod, dir] : folders) {
			if (cancelled()) return r;
			if (a_progress) a_progress->set(mod);
			const auto src = static_cast<std::uint32_t>(r.sources.size());
			r.sources.push_back({ mod, {} });
			std::error_code ec;
			for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
				if (it.depth() == 0 && it->is_directory(ec) && Lower(U8(it->path().filename())) == "fomod") {
					it.disable_recursion_pending();
					continue;
				}
				if (it.depth() == 0 || !it->is_regular_file(ec)) {
					continue;  // plugins, archives and readme files at the top are not game files
				}
				auto rel = Lower(U8(it->path().lexically_relative(dir)));
				std::replace(rel.begin(), rel.end(), '/', '\\');
				add(rel, src);
			}
			++r.modsRead;
			if (a_progress) ++a_progress->done;
		}

		// who overrides whom
		std::map<std::pair<std::string, std::string>, std::size_t> pairIndex;
		for (std::size_t ci = 0; ci < r.conflicts.size(); ++ci) {
			const auto& c = r.conflicts[ci];
			const bool  script = c.path.size() > 4 && c.path.ends_with(".pex");
			r.scriptConflicts += script ? 1 : 0;
			const auto& win = r.sources[c.sources.back()].mod;
			std::set<std::string> losers;
			for (std::size_t i = 0; i + 1 < c.sources.size(); ++i) {
				const auto& l = r.sources[c.sources[i]].mod;
				if (l != win) losers.insert(l);
			}
			for (const auto& l : losers) {
				auto [it, fresh] = pairIndex.try_emplace({ win, l }, r.pairs.size());
				if (fresh) {
					r.pairs.push_back({ win, l });
				}
				auto& p = r.pairs[it->second];
				++p.files;
				p.scripts += script ? 1 : 0;
				p.conflicts.push_back(ci);
			}
		}
		// mod against mod first (overriding the base game's own files is what most mods are for), scripts first
		std::sort(r.pairs.begin(), r.pairs.end(), [](const Pair& a, const Pair& b) {
			const bool ga = a.loser == "Game Data", gb = b.loser == "Game Data";
			if (ga != gb) return gb;
			return a.scripts != b.scripts ? a.scripts > b.scripts : a.files > b.files;
		});
		return r;
	}

	std::string Result::report(std::size_t a_maxFiles) const
	{
		std::ostringstream o;
		o << "ReSaver Plus file conflicts (loose files and archives)\n";
		o << "======================================================\n\n";
		o << files << " files in " << modsRead << " folders and " << archives << " archives; " << conflicts.size()
		  << " paths are provided more than once (" << scriptConflicts << " of them scripts).\n";
		o << "Loose files beat every archive; among loose files the mod order decides; among archives the plugin load order.\n\n";
		for (const auto& e : errors) o << "could not read: " << e << "\n";
		o << "Mod overriding mod (mod against mod first, scripts first, then by number of files). A script overridden by\n"
			 "another mod's copy is worth a look: one of the two mods may not work as its author meant.\n";
		for (const auto& p : pairs) {
			o << "\n" << p.winner << "  overrides  " << p.loser << ": " << p.files << " file(s)" << (p.scripts ? ", " + std::to_string(p.scripts) + " script(s)" : "") << "\n";
			std::size_t shown = 0;
			for (const auto ci : p.conflicts) {
				const auto& c = conflicts[ci];
				const bool script = c.path.ends_with(".pex");
				if (shown >= a_maxFiles && !script) continue;
				o << "    " << c.path << "   winner " << sources[c.sources.back()].label() << "\n";
				++shown;
			}
			if (p.conflicts.size() > shown) o << "    ... and " << p.conflicts.size() - shown << " more\n";
		}
		return o.str();
	}
}
