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
	bool              quests = false;
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
						"  -o <out.sfs>                    write (an .sfse next to the input is written next to the output)\n");
			return 0;
		} else if (a == "--delete-form") {
			deleteForms.insert(static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16)));
		} else {
			std::fprintf(stderr,
				"%s: not available for Starfield saves (they support: -o <out.sfs>, --describe, --report, --check, --forms [TYPE], --delete-form <form id>, "
				"--remove-unattached, --remove-undefined, --terminate-undefined, --remove-stranded-threads, --clear-floods, --remove-retyped, "
				"--game <Starfield folder> | --data <Data folder>, --names, "
				"--name <form id>, --language <en>, --plugins-txt <file>, --mods-not-installed, --remove-not-installed [--also-disabled])\n",
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
