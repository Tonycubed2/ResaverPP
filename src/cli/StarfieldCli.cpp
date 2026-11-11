// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// resaverpp-cli for Starfield saves (.sfs): what the save is made of, its change forms, deleting change forms, and
// writing it back. The Skyrim / Fallout 4 options do not apply to these saves and are refused.
#include "core/LoadOrder.h"
#include "core/ModRemove.h"
#include "core/Mods.h"
#include "core/Ops.h"
#include "core/PluginDB.h"
#include "core/SaveBloat.h"
#include "core/ScriptFlood.h"
#include "core/Cosave.h"
#include "core/Save.h"
#include "core/Text.h"

#include <chrono>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>

using namespace ess;

namespace
{
	bool IsSfs(const char* a_path)
	{
		std::string low = a_path;
		for (auto& c : low) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		if (low.size() > 4 && low.ends_with(".sfs")) {
			return true;
		}
		std::FILE* f = std::fopen(a_path, "rb");
		if (!f) {
			return false;
		}
		char       m[12]{};
		const auto n = std::fread(m, 1, sizeof(m), f);
		std::fclose(f);
		return (n >= 4 && std::memcmp(m, "BCPS", 4) == 0) || (n == 12 && std::memcmp(m, "SFS_SAVEGAME", 12) == 0);
	}
}

namespace
{
	std::string LowerSF(std::string a_s)
	{
		for (auto& c : a_s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return a_s;
	}
}

// -1: not a Starfield save (the caller goes on); else the exit code
int StarfieldMain(int argc, char** argv)
{
	if (argc < 2 || argv[1][0] == '-' || !IsSfs(argv[1])) {
		return -1;
	}
	const std::string in = argv[1];
	std::string       out, formsType;
	bool              quests = false, components = false;
	std::uint32_t     inventoryOf = 0, faceOf = 0;
	std::string       ofType;
	bool              actorValues = false;
	// edits (each needs -o): inventory, the player's name, level / XP, actor values, face
	std::vector<std::string> sfAddItem, sfSetCount, sfSetAv, sfSetAvMod, sfSetFace;
	std::string              sfRename, sfSetLevel;
	bool              describe = false, forms = false, report = false, unattached = false, undefined = false, termUndef = false;
	std::set<std::uint32_t> deleteForms;
	// names (--game / --data) and mods no longer installed
	std::string                 gameDir, pluginsTxt, language = "en";
	std::vector<std::string>    dataDirs;
	std::vector<std::uint32_t>  nameIDs;
	bool                        names = false, notInstalled = false, removeNotInstalled = false, alsoDisabled = false;
	bool                        check = false, removeStranded = false, clearFloods = false, removeRetyped = false;
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "-o") {
			out = next();
		} else if (a == "--describe") {
			describe = true;
		} else if (a == "--actor-values") {
			actorValues = true;
		} else if (a == "--of-type") {
			ofType = next();
		} else if (a == "--components") {
			components = true;
		} else if (a == "--sf-add-item") {
			sfAddItem.push_back(next());
		} else if (a == "--sf-set-count") {
			sfSetCount.push_back(next());
		} else if (a == "--sf-rename") {
			sfRename = next();
		} else if (a == "--sf-set-level") {
			sfSetLevel = next();
			names = true;
		} else if (a == "--sf-set-av") {
			sfSetAv.push_back(next());
			names = true;
		} else if (a == "--sf-set-av-mod") {
			sfSetAvMod.push_back(next());
			names = true;
		} else if (a == "--sf-set-face") {
			sfSetFace.push_back(next());
		} else if (a == "--face") {
			faceOf = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 16));
		} else if (a == "--inventory") {
			inventoryOf = static_cast<std::uint32_t>(std::strtoul(next().c_str(), nullptr, 16));
		} else if (a == "--quests") {
			quests = true;
		} else if (a == "--forms") {
			forms = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				formsType = argv[++i];
			}
		} else if (a == "--report") {
			report = true;
		} else if (a == "--remove-unattached") {
			unattached = true;
		} else if (a == "--remove-undefined") {
			undefined = true;
		} else if (a == "--terminate-undefined") {
			termUndef = true;
		} else if (a == "--game") {
			gameDir = next();
		} else if (a == "--data") {
			dataDirs.push_back(next());
		} else if (a == "--plugins-txt") {
			pluginsTxt = next();
		} else if (a == "--language") {
			language = next();
		} else if (a == "--names") {
			names = true;
		} else if (a == "--name") {
			names = true;
			nameIDs.push_back(static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16)));
		} else if (a == "--mods-not-installed") {
			notInstalled = true;
		} else if (a == "--remove-not-installed") {
			notInstalled = removeNotInstalled = true;
		} else if (a == "--also-disabled") {
			alsoDisabled = true;
		} else if (a == "--check") {
			check = true;
		} else if (a == "--remove-stranded-threads") {
			removeStranded = true;
		} else if (a == "--clear-floods") {
			clearFloods = true;
		} else if (a == "--remove-retyped") {
			removeRetyped = names = true;
		} else if (a == "--help-sf") {
			std::printf("Starfield (.sfs) options:\n"
						"  --describe, --report, --forms [TYPE], --delete-form <id>   what the save is made of\n"
						"  --check                         the Save check in text\n"
						"  --remove-unattached, --remove-undefined, --terminate-undefined, --remove-stranded-threads, --clear-floods\n"
						"  --game <folder> | --data <Data> [--language en] --names   name the forms from the game's plugins\n"
						"  --remove-retyped                delete change forms whose type in the plugins changed (the game skips them)\n"
						"  --mods-not-installed [--plugins-txt f], --remove-not-installed [--also-disabled]\n"
						"  --inventory <ref>, --face <NPC_>, --components, --actor-values, --quests   read a reference's cargo, a face, ships, the\n"
						"                                  player's actor values, the quests\n"
						"  --sf-add-item <ref>:<item>:<count>     add an item to a reference's inventory (player 14, a ship's reference)\n"
						"  --sf-set-count <ref>:<item>:<count>    set an item's count (its first stack)\n"
						"  --sf-rename <name>                     rename the player (the load menu's name too)\n"
						"  --sf-set-level <level>[:<xp into it>]  the player's level and XP (needs --game / --data)\n"
						"  --sf-set-av <AVIF id>:<value>          a saved base actor value of the player (needs --game / --data)\n"
						"  --sf-set-av-mod <AVIF id>:<permanent|temporary|damage>:<value>   a saved modifier (needs --game / --data)\n"
						"  --sf-set-face <NPC_ id>:<#n>:<value>   a face number, #n as --face lists it (player: 7)\n"
						"  -o <out.sfs>                    write (an .sfse next to the input is written next to the output)\n");
			return 0;
		} else if (a == "--delete-form") {
			deleteForms.insert(static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16)));
		} else {
			std::fprintf(stderr,
				"%s: not available for Starfield saves (they support: -o <out.sfs>, --describe, --report, --check, --forms [TYPE], --delete-form <form id>, "
				"--remove-unattached, --remove-undefined, --terminate-undefined, --remove-stranded-threads, --clear-floods, --remove-retyped, "
				"--game <Starfield folder> | --data <Data folder>, --names, "
				"--name <form id>, --inventory <form id>, --face <NPC_ form id>, --components, --of-type <TYPE>, --actor-values, --language <en>, --plugins-txt <file>, --mods-not-installed, --remove-not-installed [--also-disabled], "
				"--sf-add-item, --sf-set-count, --sf-rename, --sf-set-level, --sf-set-av, --sf-set-av-mod, --sf-set-face; --help-sf explains them)\n",
				a.c_str());
			return 1;
		}
	}
	try {
		auto save = SaveFile::load(in);
		if (!save->problems.empty()) {
			std::fprintf(stderr, "warning: this save has structural problems; it may not be written back faithfully:\n");
			for (const auto& pr : save->problems) {
				std::fprintf(stderr, "  - %s\n", pr.c_str());
			}
		}
		for (const auto& l : DescribeSave(*save, describe)) {
			std::printf("%s\n", l.c_str());
		}
		// the Data folders: --data, else <--game>\Data (+ My Games\Starfield\Data)
		std::vector<std::filesystem::path> dirs;
		for (const auto& d : dataDirs) {
			dirs.emplace_back(text::PathFrom(d));
		}
		if (dirs.empty() && !gameDir.empty()) {
			dirs = loadorder::StarfieldDataDirs(text::PathFrom(gameDir));
		}
		if ((names || notInstalled) && dirs.empty()) {
			std::fprintf(stderr, "--names / --mods-not-installed need --game <Starfield folder> or --data <Data folder>\n");
			return 1;
		}
		std::unique_ptr<PluginDB> db;
		if (names) {
			PluginDB::Input in;
			PrepareInput(*save, in);
			in.sf = true;
			in.medium = save->plugins.medium;
			in.sfDataDirs = dirs;
			in.language = language;
			if (const char* full = std::getenv("RESAVERPP_SF_FULL_RECORDS"); full && *full) {
				in.sfPrefix = SIZE_MAX;  // (check: every record unpacked whole)
			}
			PluginDB::Progress prog;
			db = std::make_unique<PluginDB>();
			const auto t0 = std::chrono::steady_clock::now();
			db->load(in, prog);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			std::printf("names: %zu of %zu plugins read, %zu forms named, %zu records, %.1f MB in %.0f ms (strings files %zu from %zu archives: %.0f ms; "
						"plugins %.0f ms); ids not placed %zu, overrides of unknown forms %zu\n",
				db->pluginsRead, db->plugins().size(), db->size(), db->recordsRead, db->sfBytes / 1048576.0, ms, db->sfStringsFiles, db->sfArchivesRead,
				db->sfMsStrings, db->sfMsPlugins, db->sfUnresolved, db->sfOrphanOverrides);
			std::printf("names: %zu compressed records inflated in %.0f ms\n", db->sfInflated, db->sfMsInflate);
			for (const auto& m : db->sfSamples) {
				std::printf("names: check: %s\n", m.c_str());
			}
			for (const auto& m : db->missing) {
				std::printf("names: not found: %s\n", m.c_str());
			}
			for (const auto& e : db->errors) {
				std::printf("names: error: %s\n", e.c_str());
			}
			std::size_t named = 0, cfs = 0;
			for (const auto& c : save->changeForms) {
				if (c.refID.type() == RefID::Type::kCreated) {
					continue;
				}
				++cfs;
				named += db->find(save->formIDOf(c.refID)) != nullptr;
			}
			std::printf("names: %zu of %zu change forms of plugins are found in the plugins\n", named, cfs);
			for (const auto id : nameIDs) {
				const auto* f = db->find(id);
				std::printf("%08X  %s  (%s)\n", id, db->describe(id).c_str(), f ? db->pluginName(*f).c_str() : save->pluginOf(id).c_str());
			}
		}
		auto baseOf = [&](std::uint32_t a_fid, const ChangeForm& c) -> std::uint32_t {
			if (db) {
				if (const auto* f = db->find(a_fid); f && f->base) {
					return f->base;
				}
			}
			for (const auto& b : BaseObjectsSF(a_fid, c)) {
				return save->formIDOf(b);
			}
			return 0;
		};
		if (actorValues && db) {
			for (const auto& c : save->changeForms) {
				if (c.typeCode() != 1 || save->formIDOf(c.refID) != 0x14) {
					continue;
				}
				const auto av = FindActorValuesSF(*save, c, [&](std::uint32_t f) {
					if (const auto* d = db->find(f)) {
						return db->type(*d) == "AVIF";
					}
					return f > 0 && f < 0x800;
				});
				std::printf("player actor values: %s (%zu base, %zu modified)\n", av.ok ? "found" : av.error.c_str(), av.base.size(), av.modifiers.size());
				auto val = [&](std::size_t o) {
					float v = 0;
					if (o != SIZE_MAX) {
						std::memcpy(&v, c.data.data() + o, 4);
					}
					return v;
				};
				for (const auto& e : av.base) {
					const auto f = save->formIDOf(c.sfRefs[e.refIndex]);
					if (e.value[0] != SIZE_MAX) {
						std::printf("  base %08X %-40s %.3f\n", f, db->describe(f).c_str(), val(e.value[0]));
					}
				}
				for (const auto& e : av.modifiers) {
					const auto f = save->formIDOf(c.sfRefs[e.refIndex]);
					std::printf("  mod  %08X %-40s permanent %.3f temporary %.3f damage %.3f\n", f, db->describe(f).c_str(), val(e.value[0]), val(e.value[1]), val(e.value[2]));
				}
			}
		}
		if (!ofType.empty() && db) {
			for (const auto id : db->ofType(ofType)) {
				std::printf("%08X  %s\n", id, db->describe(id).c_str());
			}
		}
		if (components) {
			// every reference with component data (ships, outposts...): its base and the base's record type
			const auto shipNames = ShipNamesSF(*save);
			for (const auto& c : save->changeForms) {
				if (c.typeCode() > 1 || c.version != 153) {
					continue;
				}
				const auto fid = save->formIDOf(c.refID);
				const auto r = DecodeReferenceSF(fid, c);
				if (!r.ok || !r.components) {
					continue;
				}
				const auto  b = baseOf(fid, c);
				const auto* bf = db && b ? db->find(b) : nullptr;
				std::printf("%08X  %s  components %u  items %zu  base %08X %s  %s\n", fid, save->cfTypeName(c), r.components == 0xFFFFFFFFu ? 0u : r.components,
					r.items.size(), b, bf ? db->type(*bf).c_str() : "????", db ? (db->describe(fid) + " / " + db->describe(b)).c_str() : "");
				if (const auto it = shipNames.find(fid); it != shipNames.end()) {
					std::printf("          named \"%s\"\n", it->second.c_str());
				}
			}
		}
		if (inventoryOf) {
			for (const auto& c : save->changeForms) {
				if (c.typeCode() > 1 || save->formIDOf(c.refID) != inventoryOf) {
					continue;
				}
				const auto r = DecodeReferenceSF(inventoryOf, c);
				std::printf("%08X: %s, %zu items\n", inventoryOf, r.ok ? "read" : ("not read: " + r.error).c_str(), r.items.size());
				for (const auto& it : r.items) {
					const auto f = it.refIndex < c.sfRefs.size() ? save->formIDOf(c.sfRefs[it.refIndex]) : 0;
					std::printf("  %08X  x%llu  (%zu stacks)  %s\n", f, static_cast<unsigned long long>(it.total()), it.stacks.size(), db ? db->describe(f).c_str() : "");
				}
			}
		}
		if (faceOf) {
			// an NPC_'s saved face numbers (DecodeNpcSF): group, key, name, value
			for (const auto& c : save->changeForms) {
				if (c.typeCode() != 9 || save->formIDOf(c.refID) != faceOf) {
					continue;
				}
				const auto n = DecodeNpcSF(c);
				std::printf("%08X: %s, %zu face numbers\n", faceOf, n.ok ? "read" : ("not read: " + n.error).c_str(), n.face.size());
				for (std::size_t k = 0; k < n.face.size(); ++k) {
					const auto& v = n.face[k];
					float       x;
					std::memcpy(&x, c.data.data() + v.offset, 4);
					const bool named = v.name < c.sfStrings.size() && c.sfStrings[v.name] < save->sfStrings.size();
					std::printf("  #%-3zu %s  %6u  %-40s %g\n", k, v.group == 1 ? "slider" : v.group == 2 ? "blend " : "shape ", v.key, named ? save->sfStrings[c.sfStrings[v.name]].c_str() : "-", x);
				}
			}
		}
		if (quests) {
			// quests: enabled / completed and the stages done (DecodeQuestSF)
			for (const auto& c : save->changeForms) {
				if (c.typeCode() != 8) {
					continue;
				}
				const auto fid = save->formIDOf(c.refID);
				const auto q = DecodeQuestSF(c);
				std::string done;
				for (const auto& st : q.stages) {
					if (st.done) {
						done += (done.empty() ? "" : ",") + std::to_string(st.index);
					}
				}
				std::printf("%08X  %-24s %s%s  stages done: %s%s%s\n", fid, save->pluginOf(fid).c_str(), !q.hasFlags ? "-        " : q.completed() ? "completed" : q.enabled() ? "running  " : "stopped  ",
					q.ok ? "" : " (data short)", done.empty() ? "none" : done.c_str(), db ? "  " : "", db ? db->describe(fid).c_str() : "");
			}
		}
		if (forms) {
			for (const auto& c : save->changeForms) {
				const char* t = save->cfTypeName(c);
				if (!formsType.empty() && formsType != t) {
					continue;
				}
				const auto  fid = save->formIDOf(c.refID);
				std::string name = db ? db->describe(fid) : std::string();
				if (db && name.empty()) {
					// a reference made in game (or one whose base changed): named by its base object
					for (const auto& b : BaseObjectsSF(fid, c)) {
						const auto bf = save->formIDOf(b);
						if (const auto d = db->describe(bf); !d.empty()) {
							name = "base " + d;
							break;
						}
					}
				}
				std::printf("%06X  form %08X  %-4s  %-28s flags %08X  version %u  refs %zu  strings %zu  data %zu%s%s\n", c.refID.data, fid, t,
					save->pluginOf(fid).c_str(), c.changeFlags, c.version, c.sfRefs.size(), c.sfStrings.size(), c.data.size(), name.empty() ? "" : "  ",
					name.c_str());
			}
		}
		if (report) {
			auto r = ops::Analyze(*save);
			std::printf("%s", ops::FormatReport(r).c_str());
			if (const auto* p = save->papyrus(); p && !save->sfPapyrusRaw()) {
				for (const auto& sc : p->scripts) {
					if (ops::IsScriptUndefined(*p, sc)) {
						std::printf("undefined script: %s\n", p->str(sc.name).c_str());
					}
				}
				// references to the unattached instances from any variable (what removing them would leave dangling)
				std::set<std::uint64_t> un;
				for (const auto& i : p->instances) {
					if (ops::IsUnattached(i)) {
						un.insert(i.id);
					}
				}
				std::size_t refsTo = 0;
				for (const auto id : ops::NamedObjectIDs(*p)) {
					refsTo += un.contains(id);
				}
				std::printf("unattached instances a variable names: %zu\n", refsTo);
				for (const auto& i : p->instances) {
					if (ops::IsUnattached(i)) {
						std::printf("unattached: %016llX %s (handle kind %u, %u, data %s)\n", static_cast<unsigned long long>(i.id), p->str(i.script).c_str(),
							static_cast<unsigned>(i.unknown2bits), static_cast<unsigned>(i.unknown), i.data ? "yes" : "no");
					}
				}
			}
		}
		std::vector<ops::SfTypeMismatch> retyped;
		if (db) {
			retyped = ops::SfTypeMismatches(*save, [&](std::uint32_t a_id) -> std::string {
				const auto* f = db->find(a_id);
				return f ? db->type(*f) : std::string();
			});
		}
		if (check) {
			const auto* p = save->papyrus();
			const bool  pap = p && !save->sfPapyrusRaw();
			std::printf("check: script data %s\n", !p ? "missing" : save->sfPapyrusRaw() ? ("not read: " + save->sfPapyrusError).c_str()
																   : p->invalid ? "INVALID (the game reports such a save as corrupt)" : "read");
			if (pap && !p->invalid) {
				const auto r = ops::Analyze(*save);
				std::printf("check: %zu threads, %zu suspended stacks, %zu queued calls, %zu pending latent results\n", r.threads, r.suspended,
					r.functionMessages, p->pendingLatents.size());
				std::printf("check: stranded threads %zu\n", r.stranded);
				std::printf("check: script floods (over %zu calls of one event) %zu\n", flood::kOver, r.floods.size());
				for (const auto& f : r.floods) {
					std::printf("check:   %s: %zu calls, %zu not started\n", f.key().c_str(), f.total(), f.notStarted);
				}
				std::printf("check: scripts in wait loops %zu\n", r.loopingScripts.size());
				std::printf("check: undefined scripts %zu, instances %zu, threads %zu\n", r.undefinedScripts, r.undefinedInstances, r.undefinedThreads);
				std::printf("check: unattached instances %zu%s\n", r.unattached, r.unattached >= 100 ? " (many)" : "");
				std::size_t memberless = 0;
				for (const auto& i : p->instances) {
					memberless += ops::IsMemberless(*p, i);
				}
				std::printf("check: memberless instances %zu\n", memberless);
			}
			if (db) {
				std::printf("check: change forms whose type changed in the plugins (the game skips them) %zu\n", retyped.size());
				for (std::size_t i = 0; i < retyped.size() && i < 20; ++i) {
					std::printf("check:   %08X saved %s, now %s  %s\n", retyped[i].formID, retyped[i].saved.c_str(), retyped[i].now.c_str(),
						db->describe(retyped[i].formID).c_str());
				}
			} else {
				std::printf("check: change form types not checked (needs --names with --game / --data)\n");
			}
			std::printf("check: .sfse next to it: %s\n", save->cosave.empty() ? "no" : (std::to_string(save->cosave.size()) + " bytes, kept as is").c_str());
			if (notInstalled) {
				const auto rep = bloat::Build(*save);
				std::printf("check: size by plugin (biggest first; * = not loaded):\n");
				const auto pt = !pluginsTxt.empty() ? text::PathFrom(pluginsTxt) : loadorder::GamePluginsTxt(3);
				const auto order = loadorder::ReadStarfield(pt, dirs);
				const auto left = modremove::StarfieldNotLoaded(*save, order, nullptr);
				std::set<std::string> gone;
				for (const auto& l : left) {
					gone.insert(LowerSF(l.plugin));
				}
				std::size_t shown = 0;
				for (const auto& m : rep.mods) {
					if (shown++ >= 25) {
						break;
					}
					std::printf("check:   %c %-50s %10s  instances %zu, threads %zu, change forms %zu\n", gone.contains(LowerSF(m.name)) ? '*' : ' ',
						m.name.c_str(), bloat::Bytes(m.total()).c_str(), m.instances.count, m.threads.count, m.changeFormCount());
				}
			}
		}
		const bool papyrusEdits = unattached || undefined || termUndef || removeNotInstalled || removeStranded || clearFloods;
		if (papyrusEdits && (!save->papyrus() || save->sfPapyrusRaw())) {
			std::fprintf(stderr, "the script data of this save could not be read: %s\n", save->sfPapyrusError.c_str());
			return 2;
		}
		bool edited = false;
		if (notInstalled) {
			const auto pt = !pluginsTxt.empty() ? text::PathFrom(pluginsTxt) : loadorder::GamePluginsTxt(3);
			const auto order = loadorder::ReadStarfield(pt, dirs);
			const auto table = save->papyrus() && !save->sfPapyrusRaw() ? mods::Build(*save, nullptr) : mods::Table{};
			const auto left = modremove::StarfieldNotLoaded(*save, order, &table);
			std::printf("load order: %zu plugin(s) active (%s), %zu plugin file(s) in the Data folders\n", order.active.size(),
				order.pluginsTxtFound ? ("plugins.txt " + order.pluginsTxt.string()).c_str() : "no plugins.txt: the game's own files", order.files.size());
			std::size_t gone = 0;
			for (const auto& l : left) {
				gone += !l.installed;
				std::printf("not loaded: %-50s %-22s %zu forms, %zu scripts, %zu instances, %zu threads, %zu instances on its forms\n", l.plugin.c_str(),
					l.installed ? "installed, not enabled" : "not installed", l.changeForms, l.scripts, l.instances, l.threads, l.ownInstances);
			}
			std::printf("%zu plugin(s) of the save are not loaded: %zu not installed\n", left.size(), gone);
			if (removeNotInstalled) {
				modremove::Options opt;
				opt.sfFiles = &order.files;
				std::size_t done = 0, refused = 0;
				for (const auto& l : left) {
					if (l.nothing() || (l.installed && !alsoDisabled)) {
						continue;
					}
					const auto plan = modremove::MakePlan(*save, l.plugin, nullptr, opt);
					if (!plan.canApply()) {
						++refused;
						std::printf("refused %s:\n%s\n", l.plugin.c_str(), plan.summary().c_str());
						continue;
					}
					const auto r = modremove::Apply(*save, plan);
					std::printf("removed %s: %s\n", l.plugin.c_str(), r.text().c_str());
					++done;
				}
				std::printf("removed the leftovers of %zu plugin(s), %zu refused\n", done, refused);
				edited = edited || done;
			}
		}
		if (removeRetyped) {
			std::set<std::uint32_t> raws;
			for (const auto& m : retyped) {
				raws.insert(save->changeForms[m.index].refID.data);
			}
			std::printf("removed %zu change form(s) whose type changed\n", ops::DeleteChangeForms(*save, raws));
			edited = edited || !raws.empty();
		}
		if (removeStranded) {
			const auto r = ops::RemoveStrandedThreads(*save->papyrus());
			std::printf("removed %zu stranded thread(s)\n", r.threads);
			edited = true;
		}
		if (clearFloods) {
			const auto r = flood::Clear(*save);
			const auto back = flood::ReadBack(*save);
			std::printf("cleared %zu queued call(s) that never started (%zu threads); read back %s: %zu threads, %zu stranded\n", r.calls, r.threads,
				back.ok ? "ok" : back.error.c_str(), back.threads, back.stranded);
			edited = true;
		}
		if (termUndef) {
			std::printf("terminated %zu undefined thread(s)\n", ops::TerminateUndefinedThreads(*save->papyrus()));
			edited = true;
		}
		if (unattached) {
			const auto r = ops::RemoveUnattachedInstances(*save);
			std::printf("removed %zu unattached instance(s)%s; %zu kept because a variable names them\n", r.removed,
				r.kept.empty() ? "" : (", kept " + std::to_string(r.kept.size())).c_str(), r.keptNamed);
			edited = true;
		}
		if (undefined) {
			const auto r = ops::RemoveUndefinedElements(*save);
			std::printf("removed/terminated %zu undefined element(s)%s\n", r.removed, r.kept.empty() ? "" : (", kept " + std::to_string(r.kept.size())).c_str());
			edited = true;
		}
		{
			// Starfield edits (the same code as the app's windows)
			auto split = [](const std::string& a_s) {
				std::vector<std::string> v;
				std::size_t              p = 0;
				for (;;) {
					const auto e = a_s.find(':', p);
					v.push_back(a_s.substr(p, e == std::string::npos ? std::string::npos : e - p));
					if (e == std::string::npos) {
						break;
					}
					p = e + 1;
				}
				return v;
			};
			auto hex = [](const std::string& a_s) { return static_cast<std::uint32_t>(std::stoul(a_s, nullptr, 16)); };
			auto cfOf = [&](std::uint32_t a_fid, int a_code) -> ChangeForm* {
				for (auto& c : save->changeForms) {
					if (save->formIDOf(c.refID) == a_fid && (a_code < 0 ? c.typeCode() <= 1 : c.typeCode() == a_code)) {
						return &c;
					}
				}
				return nullptr;
			};
			auto bad = [&](const std::string& a_what) {
				std::fprintf(stderr, "%s\n", a_what.c_str());
				return 1;
			};
			auto isAV = [&](std::uint32_t f) {
				if (db) {
					if (const auto* d = db->find(f)) {
						return db->type(*d) == "AVIF";
					}
				}
				return f > 0 && f < 0x800;
			};
			for (const auto& e : sfAddItem) {
				const auto v = split(e);
				if (v.size() != 3) {
					return bad("--sf-add-item <ref>:<item>:<count>");
				}
				auto* c = cfOf(hex(v[0]), -1);
				std::string why;
				if (!c) {
					return bad(v[0] + ": no reference change form in this save");
				}
				if (!AddInventoryItemSF(*save, *c, hex(v[1]), static_cast<std::uint32_t>(std::stoul(v[2])), &why)) {
					return bad("not added: " + why);
				}
				std::printf("added %s x%s to %s\n", v[1].c_str(), v[2].c_str(), v[0].c_str());
				edited = true;
			}
			for (const auto& e : sfSetCount) {
				const auto v = split(e);
				if (v.size() != 3) {
					return bad("--sf-set-count <ref>:<item>:<count>");
				}
				const auto fid = hex(v[0]), item = hex(v[1]);
				auto*      c = cfOf(fid, -1);
				if (!c) {
					return bad(v[0] + ": no reference change form in this save");
				}
				const auto r = DecodeReferenceSF(fid, *c);
				bool       done = false;
				for (const auto& it : r.items) {
					if (it.refIndex < c->sfRefs.size() && save->formIDOf(c->sfRefs[it.refIndex]) == item && !it.stacks.empty()) {
						done = SetInventoryCountSF(*c, it.stacks.front(), static_cast<std::uint32_t>(std::stoul(v[2])));
						break;
					}
				}
				if (!done) {
					return bad(v[1] + ": not in " + v[0] + "'s saved inventory (--inventory " + v[0] + " lists it)");
				}
				std::printf("%s: %s count set to %s\n", v[0].c_str(), v[1].c_str(), v[2].c_str());
				edited = true;
			}
			if (!sfRename.empty()) {
				auto* c = cfOf(0x7, 9);
				if (!c || !RenameNpcSF(*save, *c, sfRename)) {
					return bad("not renamed: the save keeps no name for the player");
				}
				std::printf("player renamed to %s\n", sfRename.c_str());
				edited = true;
			}
			if (!sfSetLevel.empty()) {
				const auto  v = split(sfSetLevel);
				std::string why;
				if (!SetPlayerLevelSF(*save, static_cast<std::uint16_t>(std::stoul(v[0])), v.size() > 1 ? std::stof(v[1]) : 0.0f, isAV, &why)) {
					return bad("level not set: " + why);
				}
				std::printf("player level set to %s%s\n", v[0].c_str(), v.size() > 1 ? (", XP into it " + v[1]).c_str() : "");
				edited = true;
			}
			if (!sfSetAv.empty() || !sfSetAvMod.empty()) {
				auto* pc = cfOf(0x14, 1);
				if (!pc) {
					return bad("the player (14) has no actor record in this save");
				}
				const auto av = FindActorValuesSF(*save, *pc, isAV);
				if (!av.ok) {
					return bad("actor values not found: " + av.error);
				}
				auto find = [&](const std::vector<SfActorValue>& a_list, std::uint32_t a_f) -> const SfActorValue* {
					for (const auto& x : a_list) {
						if (x.refIndex < pc->sfRefs.size() && save->formIDOf(pc->sfRefs[x.refIndex]) == a_f) {
							return &x;
						}
					}
					return nullptr;
				};
				for (const auto& e : sfSetAv) {
					const auto v = split(e);
					const auto* x = v.size() == 2 ? find(av.base, hex(v[0])) : nullptr;
					if (!x || x->value[0] == SIZE_MAX || !SetActorValueSF(*pc, x->value[0], std::stof(v[1]))) {
						return bad(e + ": not a saved base value of the player (--actor-values lists them)");
					}
					std::printf("%s base set to %s\n", v[0].c_str(), v[1].c_str());
					edited = true;
				}
				for (const auto& e : sfSetAvMod) {
					const auto v = split(e);
					const int  k = v.size() == 3 ? (v[1] == "permanent" ? 0 : v[1] == "temporary" ? 1 : v[1] == "damage" ? 2 : -1) : -1;
					const auto* x = k >= 0 ? find(av.modifiers, hex(v[0])) : nullptr;
					if (!x || x->value[k] == SIZE_MAX || !SetActorValueSF(*pc, x->value[k], std::stof(v[2]))) {
						return bad(e + ": not a saved modifier of the player (--actor-values lists them; a modifier of 0 is not saved)");
					}
					std::printf("%s %s modifier set to %s\n", v[0].c_str(), v[1].c_str(), v[2].c_str());
					edited = true;
				}
			}
			for (const auto& e : sfSetFace) {
				const auto v = split(e);
				if (v.size() != 3 || v[1].empty() || v[1][0] != '#') {
					return bad("--sf-set-face <NPC_ id>:<#n>:<value>");
				}
				auto* c = cfOf(hex(v[0]), 9);
				const auto n = c ? DecodeNpcSF(*c) : SfNpcData{};
				const auto k = static_cast<std::size_t>(std::stoul(v[1].substr(1)));
				if (!n.ok || k >= n.face.size() || !SetFaceValueSF(*c, n.face[k].offset, std::stof(v[2]))) {
					return bad(e + ": no such face number (--face " + v[0] + " lists them)");
				}
				std::printf("%s face %s set to %s\n", v[0].c_str(), v[1].c_str(), v[2].c_str());
				edited = true;
			}
		}
		std::size_t removed = 0;
		if (!deleteForms.empty()) {
			std::vector<ChangeForm> keep;
			keep.reserve(save->changeForms.size());
			for (auto& c : save->changeForms) {
				if (deleteForms.contains(save->formIDOf(c.refID))) {
					std::printf("deleted change form %08X (%s)\n", save->formIDOf(c.refID), save->cfTypeName(c));
					++removed;
					continue;
				}
				keep.push_back(std::move(c));
			}
			save->changeForms = std::move(keep);
			save->reindexChangeForms();
			if (removed != deleteForms.size()) {
				std::fprintf(stderr, "warning: %zu of the form ids have no change form in this save\n", deleteForms.size() - removed);
			}
		}
		if (!out.empty()) {
			if (!save->problems.empty()) {
				std::fprintf(stderr, "not written: the save has structural problems\n");
				return 2;
			}
			std::error_code ec;
			const auto      wr = save->write(out, std::filesystem::exists(out, ec));
			std::printf("written %s%s%s\n", out.c_str(), wr.backup.empty() ? "" : " (backup ", wr.backup.empty() ? "" : (wr.backup.string() + ")").c_str());
		} else if (removed || edited) {
			std::fprintf(stderr, "nothing written: add -o <out.sfs>\n");
		}
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
	return 0;
}
