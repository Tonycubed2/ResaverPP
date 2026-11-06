// ReSaver++ command line: Fallout 4 companions (core/CompanionsFO4.h).
//   resaverpp-cli <save.fos> --fo4-companions [--sources <Data or MO2 mods folder>]
//   resaverpp-cli <save.fos> --fo4-set-affinity <ref hex>:<value> -o <out.fos>
#include "core/CompanionsFO4.h"
#include "core/PluginDB.h"
#include "core/Save.h"
#include "core/ScriptSources.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

using namespace ess;

namespace
{
	const char* LevelWord(fo4comp::Finding::Level l)
	{
		switch (l) {
		case fo4comp::Finding::Level::kProblem: return "PROBLEM";
		case fo4comp::Finding::Level::kCaution: return "CAUTION";
		default: return "INFO";
		}
	}
}

bool IsFo4CompanionsCommand(int argc, char** argv)
{
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--fo4-companions" || a == "--fo4-set-affinity") {
			return true;
		}
	}
	return false;
}

int Fo4CompanionsMain(int argc, char** argv)
{
	std::string sources, out;
	bool        view = false;
	std::uint32_t setRef = 0;
	float       setValue = 0;
	bool        set = false;
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "--fo4-companions") {
			view = true;
		} else if (a == "--sources") {
			sources = next();
		} else if (a == "-o") {
			out = next();
		} else if (a == "--fo4-set-affinity") {
			const auto v = next();
			const auto c = v.find(':');
			if (c == std::string::npos) {
				std::fprintf(stderr, "--fo4-set-affinity needs <ref hex>:<value>\n");
				return 1;
			}
			setRef = static_cast<std::uint32_t>(std::stoul(v.substr(0, c), nullptr, 16));
			setValue = static_cast<float>(std::atof(v.substr(c + 1).c_str()));
			set = true;
		} else {
			std::fprintf(stderr, "unknown option %s (with --fo4-companions: --sources, --fo4-set-affinity, -o)\n", a.c_str());
			return 1;
		}
	}
	try {
		auto save = SaveFile::load(argv[1]);
		if (!save->isFO4()) {
			std::fprintf(stderr, "--fo4-companions / --fo4-set-affinity need a Fallout 4 save\n");
			return 1;
		}
		std::unique_ptr<PluginDB> db;
		if (!sources.empty()) {
			ScriptSources src;
			src.scan(sources);
			PluginDB::Input in;
			PrepareInput(*save, in);
			in.sources = src;
			in.fo4 = true;
			in.language = "en";
			PluginDB::Progress prog;
			db = std::make_unique<PluginDB>();
			db->load(in, prog);
		}
		auto name = [&](std::uint32_t a_id) -> std::string {
			if (!db || !a_id) {
				return {};
			}
			const auto* f = db->find(a_id);
			if (!f) {
				return {};
			}
			auto n = db->name(*f);
			if (n.empty()) {
				n = db->edid(*f);
			}
			// a placed reference: its base object's name
			if (n.empty() && f->base) {
				if (const auto* b = db->find(f->base)) {
					n = db->name(*b).empty() ? db->edid(*b) : db->name(*b);
				}
			}
			return n;
		};
		if (view) {
			const auto r = fo4comp::Read(*save);
			const auto& s = r.system;
			if (s.found) {
				std::printf("follower quest %08X (%s)%s%s, Companion alias %d, DogmeatCompanion alias %d, ActiveCompanions alias %d\n", s.quest,
					s.questScript.c_str(), s.questRunning ? ", running" : "", s.questDecoded ? "" : ", its data not read", s.aliasCompanion,
					s.aliasDogmeat, s.aliasActive);
				std::printf("companion slot: %s %s; Dogmeat slot: %s %s; ActiveCompanions: %zu\n",
					s.companionFill ? [&] { char b[16]; std::snprintf(b, 16, "%08X", s.companionFill); return std::string(b); }().c_str() : "empty",
					name(s.companionFill).c_str(),
					s.dogmeatFill ? [&] { char b[16]; std::snprintf(b, 16, "%08X", s.dogmeatFill); return std::string(b); }().c_str() : "empty",
					name(s.dogmeatFill).c_str(), s.active.size());
				std::printf("forms: CurrentCompanionFaction %08X, HasBeenCompanionFaction %08X, DisallowedCompanionFaction %08X, CA_Affinity %08X, "
							"CA_CurrentThreshold %08X, CA_IsRomantic %08X, FollowerState %08X\n",
					s.factionCurrent, s.factionHasBeen, s.factionDisallowed, s.avAffinity, s.avThreshold, s.avIsRomantic, s.avFollowerState);
			}
			for (const auto& n : r.notes) {
				std::printf("note: %s\n", n.c_str());
			}
			std::printf("%zu companions\n", r.companions.size());
			for (const auto& c : r.companions) {
				std::printf("%08X %-24s %s%s%s%s\n", c.ref, name(c.ref).c_str(), c.script.c_str(), c.current() ? "  CURRENT" : "",
					c.inActive ? "  active" : "", c.hasBeen ? "  has been" : "");
				std::string line = "    ";
				char        b[160];
				if (c.hasAffinity) {
					std::snprintf(b, sizeof(b), "affinity %g", c.affinity);
					line += b;
				} else if (!c.dogmeat) {
					line += "affinity not saved";
				}
				if (c.hasThreshold) {
					std::snprintf(b, sizeof(b), ", threshold %g", c.threshold);
					line += b;
				}
				if (c.hasRange) {
					std::snprintf(b, sizeof(b), " (range %g..%g)", c.minAffinity, c.maxAffinity);
					line += b;
				}
				if (c.hasRomantic && c.romantic == 1.0f) {
					line += ", romanced";
				}
				if (c.infatuated) {
					line += ", infatuated";
				}
				if (c.perkUnlocked) {
					line += ", perk unlocked";
				}
				if (c.hasLeftPermanently) {
					line += ", LEFT FOR GOOD";
				}
				if (c.disallowed) {
					line += ", disallowed";
				}
				if (c.hasFollowerState) {
					std::snprintf(b, sizeof(b), ", FollowerState %g", c.followerState);
					line += b;
				}
				std::puts(line.c_str());
				if (c.refRead) {
					std::printf("    at %08X %s (%.0f, %.0f, %.0f)%s\n", c.cell, name(c.cell).c_str(), c.pos[0], c.pos[1], c.pos[2],
						c.aliasExtraSaysCompanion ? ", its alias list names the companion slot" : "");
				} else if (!c.hasChangeForm) {
					std::printf("    no change form (as in the plugin)\n");
				}
			}
			std::printf("%zu findings\n", r.findings.size());
			for (const auto& f : r.findings) {
				std::printf("[%s] %s\n    %s\n", LevelWord(f.level), f.title.c_str(), f.what.c_str());
				for (const auto& cmd : f.console) {
					std::printf("    console: %s\n", cmd.c_str());
				}
			}
		}
		if (set) {
			if (out.empty()) {
				std::fprintf(stderr, "--fo4-set-affinity needs -o <out.fos>\n");
				return 1;
			}
			if (!save->problems.empty()) {
				std::fprintf(stderr, "error: not editing a damaged save\n");
				return 3;
			}
			const auto why = fo4comp::SetAffinity(*save, setRef, setValue);
			if (!why.empty()) {
				std::fprintf(stderr, "could not set the affinity of %08X: %s\n", setRef, why.c_str());
				return 1;
			}
			std::printf("set the affinity of %08X to %g\n", setRef, setValue);
			std::error_code ec;
			save->write(out, std::filesystem::exists(out, ec));
			auto check = SaveFile::load(out);
			const auto again = fo4comp::Read(*check);
			const auto* c = again.find(setRef);
			if (!c || !c->hasAffinity || c->affinity != setValue) {
				std::fprintf(stderr, "error: the written save does not read back with the new affinity\n");
				return 2;
			}
			std::printf("written %s and verified (affinity reads back as %g)\n", out.c_str(), c->affinity);
		}
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
	return 0;
}
