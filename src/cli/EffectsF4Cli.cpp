// ReSaver++ command line: Fallout 4 active effects of an actor (core/ChangeFormFO4.h, the same functions as Edit player >
// Active effects and the AI's fo4_edit_player).
//   resaverpp-cli <save.fos> --fo4-effects <actor> [--sources <Data or MO2 mods folder>]
//   resaverpp-cli <save.fos> --fo4-remove-effects <actor>:<what> [--sources <folder>] -o <out.fos>
//     <actor>: player, or the actor's reference form id (hex)
//     <what>:  #n[,#m...] (numbers as --fo4-effects lists them, from 0), all, from:<magic item form id hex>, from:<plugin file>
// Without --sources the plugins are not read: running effects that change a stat are refused (which stat is in the plugins).
#include "core/ChangeFormFO4.h"
#include "core/PluginDB.h"
#include "core/Save.h"
#include "core/ScriptSources.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace ess;

namespace
{
	bool Hex(const std::string& a_text, std::uint32_t& a_out)
	{
		if (a_text.empty() || a_text.size() > 8) {
			return false;
		}
		char*      end = nullptr;
		const auto v = std::strtoul(a_text.c_str(), &end, 16);
		if (*end) {
			return false;
		}
		a_out = static_cast<std::uint32_t>(v);
		return true;
	}

	// the actor's change form: "player" or its reference's form id
	ChangeForm* FindActor(SaveFile& a_save, const std::string& a_actor, std::string& a_why)
	{
		std::uint32_t id = 0;
		if (a_actor == "player" || a_actor == "Player" || a_actor == "PLAYER") {
			id = 0x14;
		} else if (!Hex(a_actor, id)) {
			a_why = "not an actor: " + a_actor + " (player or a reference form id in hex)";
			return nullptr;
		}
		for (auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR && a_save.formIDOf(c.refID) == id) {
				return &c;
			}
		}
		char b[96];
		std::snprintf(b, sizeof(b), "the save has no saved actor data (ACHR change form) for %08X", id);
		a_why = b;
		return nullptr;
	}
}

bool IsFo4EffectsCommand(int argc, char** argv)
{
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--fo4-effects" || a == "--fo4-remove-effects") {
			return true;
		}
	}
	return false;
}

int Fo4EffectsMain(int argc, char** argv)
{
	std::string list, remove, sources, out;
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "--fo4-effects") {
			list = next();
		} else if (a == "--fo4-remove-effects") {
			remove = next();
		} else if (a == "--sources") {
			sources = next();
		} else if (a == "-o") {
			out = next();
		} else {
			std::fprintf(stderr, "unknown option %s (with --fo4-effects / --fo4-remove-effects: --sources, -o)\n", a.c_str());
			return 1;
		}
	}
	std::string actorText = list, what;
	if (!remove.empty()) {
		const auto c = remove.find(':');
		if (c == std::string::npos || c == 0 || c + 1 >= remove.size()) {
			std::fprintf(stderr, "--fo4-remove-effects needs <actor>:<#n[,#m...]|all|from:<item or plugin>>\n");
			return 1;
		}
		actorText = remove.substr(0, c);
		what = remove.substr(c + 1);
		if (out.empty()) {
			std::fprintf(stderr, "--fo4-remove-effects needs -o <out.fos>\n");
			return 1;
		}
	}
	try {
		auto save = SaveFile::load(argv[1]);
		if (!save->isFO4()) {
			std::fprintf(stderr, "--fo4-effects / --fo4-remove-effects need a Fallout 4 save\n");
			return 1;
		}
		std::string why;
		auto*       cf = FindActor(*save, actorText, why);
		if (!cf) {
			std::fprintf(stderr, "error: %s\n", why.c_str());
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
		const auto actor = fo4cf::DecodeActor(*save, *cf);
		if (!actor.decoded) {
			std::fprintf(stderr, "error: the actor's saved data could not be read: %s\n", actor.error.c_str());
			return 2;
		}
		const auto fxSources = fo4cf::EffectSources(*save, actor, db.get());
		const auto actorID = save->formIDOf(cf->refID);
		auto       name = [&](std::uint32_t a_id) -> std::string { return db && a_id ? db->describe(a_id) : std::string(); };

		if (!list.empty()) {
			std::printf("actor %08X %s: %zu active effects%s%s\n", actorID, name(actorID).c_str(), actor.effects.size(),
				actor.complete ? "" : " (its saved data does not read completely: nothing can be removed)",
				db ? "" : " (no --sources: running effects that change a stat cannot be removed)");
			for (std::size_t i = 0; i < actor.effects.size(); ++i) {
				const auto& e = actor.effects[i];
				const auto  id = save->formIDOf(e.item);
				const auto  plan = fo4cf::PlanEffectRemoval(actor, e, &fxSources[i]);
				std::string state = e.data.size() < 16 ? "not readable"
				                    : e.started()      ? "running"
				                    : (e.flags & 0x20000u) ? "ended"
				                                           : "not started";
				char b[64];
				if (e.started()) {
					std::snprintf(b, sizeof(b), ", strength %g", e.magnitude);
					state += b;
				}
				std::string when;
				if (!plan.ok) {
					when = "cannot be removed: " + plan.why;
				} else if (plan.modifier && plan.amount != 0) {
					std::snprintf(b, sizeof(b), "actor value %08X %+g (modifier slot %d)", plan.actorValue, -plan.amount, plan.slot);
					when = b;
				} else {
					when = "no stat changes";
				}
				const auto scripts = fo4cf::EffectScriptInstances(*save, actorID, e);
				std::printf("#%zu %08X %s [%s] effect %u, archetype %u, %s; when removed: %s%s\n", i, id, name(id).c_str(),
					save->pluginOf(id).c_str(), e.effectIndex, e.archetype, state.c_str(), when.c_str(),
					scripts.empty() ? "" : (", " + std::to_string(scripts.size()) + " script instance(s) go with it").c_str());
			}
		}
		if (!remove.empty()) {
			std::vector<std::size_t> pick;
			if (what == "all") {
				for (std::size_t i = 0; i < actor.effects.size(); ++i) {
					pick.push_back(i);
				}
			} else if (what.rfind("from:", 0) == 0) {
				const auto    x = what.substr(5);
				std::uint32_t id = 0;
				pick = x.find('.') == std::string::npos && Hex(x, id) ? fo4cf::EffectsFrom(*save, actor, id) : fo4cf::EffectsFromPlugin(*save, actor, x);
				if (pick.empty()) {
					std::fprintf(stderr, "error: the actor has no active effect from \"%s\"\n", x.c_str());
					return 1;
				}
			} else {
				std::size_t at = 0;
				while (at <= what.size()) {
					auto comma = what.find(',', at);
					if (comma == std::string::npos) {
						comma = what.size();
					}
					auto t = what.substr(at, comma - at);
					if (!t.empty() && t[0] == '#') {
						t = t.substr(1);
					}
					char*      end = nullptr;
					const auto n = std::strtoul(t.c_str(), &end, 10);
					if (t.empty() || *end || n >= actor.effects.size()) {
						std::fprintf(stderr, "error: there is no active effect #%s (the actor has %zu)\n", t.c_str(), actor.effects.size());
						return 1;
					}
					pick.push_back(n);
					at = comma + 1;
				}
			}
			if (pick.empty()) {
				std::fprintf(stderr, "error: the actor has no active effects\n");
				return 1;
			}
			if (!save->problems.empty()) {
				std::fprintf(stderr, "error: not editing a damaged save\n");
				return 3;
			}
			std::vector<std::string> changes;
			const auto               e = fo4cf::RemoveActiveEffects(*save, *cf, pick, fxSources, &changes);
			if (!e.empty()) {
				std::fprintf(stderr, "could not remove the effects: %s\n", e.c_str());
				return 1;
			}
			std::printf("removed %zu active effect(s) of %08X\n", pick.size(), actorID);
			for (const auto& c : changes) {
				std::printf("    %s\n", c.c_str());
			}
			std::error_code ec;
			save->write(out, std::filesystem::exists(out, ec));
			auto        check = SaveFile::load(out);
			std::string why2;
			auto*       c2 = FindActor(*check, actorText, why2);
			const auto  again = c2 ? fo4cf::DecodeActor(*check, *c2) : fo4cf::Actor{};
			if (!c2 || !again.complete || again.effects.size() != actor.effects.size() - pick.size()) {
				std::fprintf(stderr, "error: the written save does not read back with the effects removed\n");
				return 2;
			}
			std::printf("written %s and verified (%zu active effects left, the actor reads completely)\n", out.c_str(), again.effects.size());
		}
	} catch (const std::exception& ex) {
		std::fprintf(stderr, "error: %s\n", ex.what());
		return 2;
	}
	return 0;
}
