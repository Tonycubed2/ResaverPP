// ReSaver++ command line: the player's effects grouped by source, what is worn, and the stuck effects
// (core/CharEffects.h, the same code as the Character effects window). Skyrim and Fallout 4.
//   resaverpp-cli <save> --char-effects [--sources <Data or MO2 mods folder>] [--plugins-txt <file> [--game <dir>]] [--all]
//   resaverpp-cli <save> --remove-stuck [--sources <folder>] [--plugins-txt <file>] -o <out>
// --plugins-txt: the load order to check gone plugins against (without it that check is not done); --game <game folder>: its
// Skyrim.ccc / Fallout4.ccc and masters count as loaded (the game loads them without listing them in plugins.txt).
#include "core/CharEffects.h"
#include "core/LoadOrder.h"
#include "core/PluginDB.h"
#include "core/Save.h"
#include "core/ScriptSources.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>

using namespace ess;

bool IsCharEffectsCommand(int argc, char** argv)
{
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--char-effects" || a == "--remove-stuck") {
			return true;
		}
	}
	return false;
}

int CharEffectsMain(int argc, char** argv)
{
	std::string sources, pluginsTxt, out, game;
	bool        removeStuck = false, all = false;
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "--char-effects") {
		} else if (a == "--remove-stuck") {
			removeStuck = true;
		} else if (a == "--all") {
			all = true;
		} else if (a == "--sources") {
			sources = next();
		} else if (a == "--game") {
			game = next();
		} else if (a == "--plugins-txt") {
			pluginsTxt = next();
		} else if (a == "-o") {
			out = next();
		} else {
			std::fprintf(stderr, "unknown option %s (with --char-effects / --remove-stuck: --sources, --plugins-txt, --game, --all, -o)\n", a.c_str());
			return 1;
		}
	}
	if (removeStuck && out.empty()) {
		std::fprintf(stderr, "--remove-stuck needs -o <out>\n");
		return 1;
	}
	try {
		auto save = SaveFile::load(argv[1]);
		std::unique_ptr<PluginDB> db;
		if (!sources.empty()) {
			ScriptSources src;
			src.scan(sources);
			PluginDB::Input in;
			PrepareInput(*save, in);
			in.sources = src;
			in.fo4 = save->isFO4();
			if (in.fo4) {
				in.language = "en";
			}
			PluginDB::Progress prog;
			db = std::make_unique<PluginDB>();
			db->load(in, prog);
			std::printf("plugins read: %zu, not found: %zu\n", db->pluginsRead, db->missing.size());
		}
		loadorder::Active lo;
		if (!pluginsTxt.empty()) {
			lo = loadorder::Read(pluginsTxt, game, save->isFO4() ? 2 : 0);
			if (!lo.known) {
				std::fprintf(stderr, "could not read %s\n", pluginsTxt.c_str());
				return 1;
			}
		}
		chfx::Options opt;
		opt.db = db.get();
		opt.loadOrder = lo.known ? &lo.plugins : nullptr;
		auto v = chfx::Build(*save, opt);
		if (!v.error.empty()) {
			std::fprintf(stderr, "error: %s\n", v.error.c_str());
			return 2;
		}
		std::printf("%zu active effect(s) on the player from %zu source(s)\n", v.effects.size(), v.cards.size());
		std::printf("worn (%zu):\n", v.worn.size());
		for (const auto& w : v.worn) {
			std::printf("  %-14s %s (%s, %08X)%s x%d, enchantment %08X, %zu effect(s)\n", chfx::SlotName(w.slot), v.name(w.item).c_str(),
				save->pluginOf(w.item).c_str(), w.item, w.left ? " left hand" : "", w.count, w.enchantment, w.effects);
		}
		std::printf("stuck (%zu):%s\n", v.stuck.size(), v.loadOrderChecked ? "" : (" [load order part not checked: " + v.loadOrderNote + "]").c_str());
		for (const auto& f : v.stuck) {
			std::printf("  %s\n", f.text.c_str());
		}
		for (int g = 0; g < static_cast<int>(chfx::Group::kCount); ++g) {
			std::size_t n = 0, fx = 0;
			for (const auto& c : v.cards) {
				if (static_cast<int>(c.group) == g) {
					++n;
					fx += c.effects.size();
				}
			}
			if (!n) {
				continue;
			}
			std::printf("%s: %zu source(s), %zu effect(s)\n", chfx::GroupName(static_cast<chfx::Group>(g), v.fo4), n, fx);
			if (!all && (static_cast<chfx::Group>(g) != chfx::Group::kWorn && static_cast<chfx::Group>(g) != chfx::Group::kNotWorn)) {
				continue;
			}
			for (const auto& c : v.cards) {
				if (static_cast<int>(c.group) != g) {
					continue;
				}
				std::printf("  %s (%s, %08X)%s: %zu effect(s), %zu running%s\n", v.name(c.item).c_str(), c.plugin.c_str(), c.item,
					c.source ? (" on " + v.name(c.source) + (c.sourceWorn ? " (worn)" : " (not worn)")).c_str() : "", c.effects.size(), c.running,
					c.removable ? "" : ", some cannot be removed");
			}
		}
		if (!removeStuck) {
			return 0;
		}
		std::set<std::size_t> pick;
		for (const auto& f : v.stuck) {
			for (const auto i : f.effects) {
				if (v.effects[i].canRemove) {
					pick.insert(i);
				} else {
					std::printf("left (cannot be removed): %s effect %u: %s\n", v.name(v.effects[i].item).c_str(), v.effects[i].effectIndex,
						v.effects[i].why.c_str());
				}
			}
		}
		if (pick.empty()) {
			std::printf("nothing stuck to remove\n");
			return 0;
		}
		std::vector<std::string> changes;
		const auto               why = chfx::Remove(*save, v, { pick.begin(), pick.end() }, &changes);
		if (!why.empty()) {
			std::fprintf(stderr, "could not remove: %s\n", why.c_str());
			return 3;
		}
		std::printf("removed %zu effect(s)\n", pick.size());
		for (const auto& c : changes) {
			std::printf("  %s\n", c.c_str());
		}
		std::error_code ec;
		save->write(out, std::filesystem::exists(out, ec));
		auto back = SaveFile::load(out);
		const auto again = chfx::Build(*back, opt);
		std::printf("written %s, read back: %zu effect(s), %zu stuck\n", out.c_str(), again.effects.size(), again.stuck.size());
		return again.error.empty() ? 0 : 2;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
}
