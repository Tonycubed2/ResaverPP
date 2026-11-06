// resaverpp-cli: Fallout 4 dropped / created items in the world and reset an area (core/WorldItemsFO4, core/CellResetFO4).
//   <save.fos> --fo4-world-items [--all-objects] [--sources <mods> [--game <dir>] [--profile <dir>]]
//   <save.fos> --fo4-remove-world-items <ids|all> [--all-objects] -o <out.fos>
//   <save.fos> --fo4-cells [--sources ...]
//   <save.fos> --fo4-reset-cell <cell id>[,<cell id>...] [--sources ...] -o <out.fos>   (any cell of the save or, with
//              --sources, of the plugins: indoors / outdoors and an outdoor cell's place come from the plugins' CELL records)
#include "core/CellResetFO4.h"
#include "core/PluginDB.h"
#include "core/ScriptSources.h"
#include "core/WorldItemsFO4.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace ess;

namespace
{
	std::vector<std::uint32_t> ParseIds(const std::string& a_text)
	{
		std::vector<std::uint32_t> ids;
		std::size_t                p = 0;
		while (p < a_text.size()) {
			auto e = a_text.find(',', p);
			if (e == std::string::npos) e = a_text.size();
			const auto t = a_text.substr(p, e - p);
			if (!t.empty()) ids.push_back(static_cast<std::uint32_t>(std::stoul(t, nullptr, 16)));
			p = e + 1;
		}
		return ids;
	}
}

// Returns -1 when none of these options is given (main goes on), else the exit code.
int Fo4WorldMain(int argc, char** argv)
{
	bool        listItems = false, listCells = false, allObjects = false;
	std::string removeIds, resetIds, out, sources, game, profile;
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
		if (a == "--fo4-world-items") listItems = true;
		else if (a == "--fo4-cells") listCells = true;
		else if (a == "--all-objects") allObjects = true;
		else if (a == "--fo4-remove-world-items") removeIds = next();
		else if (a == "--fo4-reset-cell") resetIds = next();
		else if (a == "-o") out = next();
		else if (a == "--sources") sources = next();
		else if (a == "--game") game = next();
		else if (a == "--profile") profile = next();
	}
	if (!listItems && !listCells && removeIds.empty() && resetIds.empty()) {
		return -1;
	}
	if ((!removeIds.empty() || !resetIds.empty()) && out.empty()) {
		std::fprintf(stderr, "--fo4-remove-world-items / --fo4-reset-cell need -o <out.fos>\n");
		return 1;
	}
	try {
		auto save = SaveFile::load(argv[1]);
		if (!save->isFO4()) {
			std::fprintf(stderr, "these options are for Fallout 4 saves\n");
			return 1;
		}
		if (!save->problems.empty() && (!removeIds.empty() || !resetIds.empty())) {
			std::fprintf(stderr, "error: not editing a damaged save\n");
			return 3;
		}
		// names and record types from the plugins
		std::unique_ptr<PluginDB> db;
		if (!sources.empty()) {
			ScriptSources         src;
			std::filesystem::path profDir = profile, overwrite, gameData;
			if (auto mo2 = FindMo2Instance(sources)) {
				if (profDir.empty()) profDir = mo2->profilesDir / mo2->selectedProfile;
				overwrite = mo2->overwriteDir;
				if (!mo2->gamePath.empty()) gameData = mo2->gamePath / "Data";
			}
			if (!game.empty()) gameData = std::filesystem::path(game) / "Data";
			const auto order = profDir.empty() ? std::vector<std::string>{} : LoadModList(profDir);
			src.scan(sources, order.empty() ? nullptr : &order, overwrite, gameData);
			PluginDB::Input in;
			PrepareInput(*save, in);
			in.sources = src;
			in.fo4 = true;
			in.language = "en";
			PluginDB::Progress prog;
			db = std::make_unique<PluginDB>();
			db->load(in, prog);
			std::printf("plugins read %zu, %zu missing\n", db->pluginsRead, db->missing.size());
		}
		const auto look = fo4world::LookupFrom(db.get());
		bool edited = false;
		if (listItems || !removeIds.empty()) {
			const auto l = fo4world::List(*save, look);
			std::map<std::string, std::vector<const fo4world::Item*>> byPlace;
			std::map<std::string, std::pair<std::size_t, long long>> byMod;
			std::size_t shown = 0, removable = 0;
			for (const auto& it : l.items) {
				if (!allObjects && db && !it.item) continue;
				byPlace[fo4world::PlaceOf(it, look)].push_back(&it);
				auto& m = byMod[it.plugin];
				++m.first;
				m.second += it.count;
				++shown;
				removable += it.removable;
			}
			if (listItems) {
				for (const auto& [place, items] : byPlace) {
					long long total = 0;
					for (const auto* it : items) total += it->count;
					std::printf("%s: %zu object(s), %lld in all\n", place.c_str(), items.size(), total);
					for (const auto* it : items) {
						std::printf("  %08X x%-4d %-20s %-30s %08X [%s] %s\n", it->formID, it->count, it->kind.c_str(),
							(it->name.empty() ? "-" : it->name).substr(0, 30).c_str(), it->base, it->plugin.c_str(),
							it->removable ? "removable" : ("kept: " + it->why).c_str());
					}
				}
				std::printf("by mod of the base object:\n");
				for (const auto& [mod, n] : byMod) std::printf("  %-40s %zu object(s), %lld in all\n", mod.c_str(), n.first, n.second);
				std::printf("%zu created object(s) in the world%s, %zu removable; %zu more are in inventories (not listed)%s\n", shown,
					(!allObjects && db) ? " (items only; --all-objects for the rest)" : "", removable, l.held,
					db ? "" : "; kinds and names need --sources");
			}
			if (!removeIds.empty()) {
				std::set<std::uint32_t> ids;
				if (removeIds == "all") {
					for (const auto& it : l.items) {
						if (it.removable && (allObjects || !db || it.item)) ids.insert(it.formID);
					}
				} else {
					for (auto id : ParseIds(removeIds)) ids.insert(id);
				}
				const auto r = fo4world::Remove(*save, ids);
				for (const auto& why : r.refused) std::printf("kept %s\n", why.c_str());
				std::printf("removed %zu created object(s) from the world\n", r.removed);
				edited = edited || r.removed > 0;
			}
		}
		if (listCells || !resetIds.empty()) {
			const auto cells = fo4cell::ListCells(*save, look);
			if (listCells) {
				std::size_t can = 0;
				for (const auto& c : cells) {
					char where[64] = "interior";
					if (!c.interiorKnown) std::snprintf(where, sizeof where, "indoors or outdoors?");
					else if (c.exterior && c.hasGrid) std::snprintf(where, sizeof where, "%08X (%d, %d)", c.worldspace, c.x, c.y);
					else if (c.exterior) std::snprintf(where, sizeof where, "outdoors (place?)");
					std::printf("%08X %-28s %-22s %s%-9s created %zu (%zu go on reset), other changed %zu  %s\n", c.formID,
						(c.name.empty() ? "-" : c.name).substr(0, 28).c_str(), where, c.hasChangeForm ? "" : "(no cell data) ",
						c.pending ? "PENDING" : c.hasDetach ? ("t=" + std::to_string(c.detach)).c_str() : "", c.createdItems, c.createdRemovable, c.placed,
						c.why.empty() ? "can reset" : c.why.c_str());
					can += c.why.empty();
				}
				std::printf("%zu cell(s), %zu can be reset. A reset does what the console's ResetInterior does: the game resets the cell the "
							"next time it loads it.%s\n",
					cells.size(), can, db ? "" : " (without --sources, indoors / outdoors comes from the save only)");
			}
			for (const auto id : ParseIds(resetIds)) {
				const auto why = fo4cell::ResetCell(*save, id, look);
				if (why.empty()) {
					std::printf("cell %08X: reset set (detach time 0xFFFFFFFF): the game resets it the next time it loads it\n", id);
					edited = true;
				} else {
					std::printf("cell %s\n", why.c_str());
				}
			}
		}
		if (!out.empty()) {
			if (!edited) {
				std::printf("nothing changed; %s not written\n", out.c_str());
				return 0;
			}
			std::error_code ec;
			const auto      wr = save->write(out, std::filesystem::exists(out, ec));
			if (!wr.backup.empty()) std::printf("previous file kept as %s\n", wr.backup.filename().string().c_str());
			auto check = SaveFile::load(out);
			std::printf("written %s and verified (re-read OK: %zu change forms)\n", out.c_str(), check->changeForms.size());
		}
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
	return 0;
}
