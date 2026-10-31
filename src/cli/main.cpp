#include <cctype>
#include <tuple>
// ReSaver++ command line - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "core/Batch.h"
#include "core/CliHelp.h"
#include "core/SkyrimLogs.h"
#include "core/ChangeFormData.h"
#include "core/CrashLog.h"
#include "core/Compare.h"
#include "core/ModRemove.h"
#include "core/Report.h"
#include "core/Pex.h"
#include "core/PluginDB.h"

#include <chrono>
#include "core/AssetConflicts.h"
#include "core/LoadOrder.h"
#include "core/MergedPatch.h"
#include "core/Mods.h"
#include "core/Ops.h"
#include "core/ScriptSources.h"
#include "core/Orphans.h"
#include "core/PropertyEdit.h"
#include "core/PluginEdit.h"
#include "core/Quests.h"
#include "core/Transplant.h"
#include <functional>
#include <map>
#include <set>

#include <algorithm>
#include <fstream>
#include <memory>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace ess;

namespace
{
	template <class... A>
	std::string Fmt(const char* a_fmt, A... a_args)
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf), a_fmt, a_args...);
		return buf;
	}
}

static void Usage()
{
	std::fputs(ess::CliUsage(), stdout);
}

static bool _stricmp_portable(const std::string& a_a, const std::string& a_b)
{
	return a_a.size() == a_b.size() && std::equal(a_a.begin(), a_a.end(), a_b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

// resaverpp-cli --merged-patch <out.esp> --sources <mods> [--profile <dir>] [--game <dir>] [--plugins-txt <file>] [--language <lang>]
static int MergedPatchMain(int argc, char** argv)
{
	if (argc < 3) {
		Usage();
		return 1;
	}
	const std::filesystem::path out = argv[2];
	std::filesystem::path       mods, profile, game, pluginsTxt, overwrite;
	std::string                 language = "english";
	int                         maxRisk = 2;
	for (int i = 3; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "--sources") mods = next();
		else if (a == "--profile") profile = next();
		else if (a == "--game") game = next();
		else if (a == "--plugins-txt") pluginsTxt = next();
		else if (a == "--overwrite") overwrite = next();
		else if (a == "--language") language = next();
		else if (a == "--only-safe" || a == "--no-risky") maxRisk = a == "--only-safe" ? 0 : 1;
		else {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return 1;
		}
	}
	if (mods.empty()) {
		std::fprintf(stderr, "--sources <MO2 mods folder or game Data folder> is needed\n");
		return 1;
	}
	if (auto mo2 = FindMo2Instance(mods)) {
		if (profile.empty()) profile = mo2->profilesDir / mo2->selectedProfile;
		if (overwrite.empty()) overwrite = mo2->overwriteDir;
		if (game.empty()) game = mo2->gamePath;
	}
	if (pluginsTxt.empty()) {
		pluginsTxt = profile.empty() ? loadorder::GamePluginsTxt() : profile / "plugins.txt";
	}
	const auto order = profile.empty() ? std::vector<std::string>{} : LoadModList(profile);
	ScriptSources src;
	const auto    t0 = std::chrono::steady_clock::now();
	src.scan(mods, order.empty() ? nullptr : &order, overwrite, game.empty() ? std::filesystem::path{} : game / "Data");
	std::printf("scanned %zu mod folders, %zu strings files\n", src.modsScanned, src.stringsFiles.size());
	merge::Options opt;
	opt.loadOrder = merge::ReadLoadOrder(pluginsTxt, game);
	opt.language = language;
	opt.patchName = out.filename().string();
	for (int c = 0; c < static_cast<int>(merge::Category::kCount); ++c) {
		opt.enabled[c] = merge::CategoryRisk(static_cast<merge::Category>(c)) <= maxRisk;
	}
	merge::UseSources(opt, src);
	std::printf("load order: %zu plugins (%s)\n", opt.loadOrder.size(), pluginsTxt.string().c_str());
	const auto r = merge::Build(opt);
	const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	if (!r.ok) {
		std::fprintf(stderr, "error: %s\n", r.error.c_str());
		return 2;
	}
	{
		std::ofstream f(out, std::ios::binary);
		f.write(reinterpret_cast<const char*>(r.file.data()), static_cast<std::streamsize>(r.file.size()));
	}
	for (const auto& w : r.fixes) {
		std::printf("fixed: %s\n", w.c_str());
	}
	for (const auto& w : r.warnings) {
		std::printf("warning: %s\n", w.c_str());
	}
	auto rep = out;
	rep.replace_extension(".txt");
	std::ofstream(rep) << r.report();
	std::printf("%zu plugins read, %zu conflicting records, %zu merged, %zu not merged, %zu masters; %.1f s\nwrote %s and %s\n", r.pluginsRead,
		r.conflicts, r.merged.size(), r.skipped.size(), r.masters.size(), secs, out.string().c_str(), rep.string().c_str());
	return 0;
}

// resaverpp-cli --file-conflicts <report.txt> --sources <mods> [--profile <dir>] [--game <dir>] [--overwrite <dir>]
static int FileConflictsMain(int argc, char** argv)
{
	if (argc < 3) {
		Usage();
		return 1;
	}
	const std::filesystem::path out = argv[2];
	assets::Input               in;
	std::filesystem::path       profile, game;
	for (int i = 3; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "--sources") in.modsDir = next();
		else if (a == "--profile") profile = next();
		else if (a == "--game") game = next();
		else if (a == "--overwrite") in.overwrite = next();
		else {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return 1;
		}
	}
	if (auto mo2 = in.modsDir.empty() ? std::nullopt : FindMo2Instance(in.modsDir)) {
		if (profile.empty()) profile = mo2->profilesDir / mo2->selectedProfile;
		if (in.overwrite.empty()) in.overwrite = mo2->overwriteDir;
		if (game.empty()) game = mo2->gamePath;
	}
	if (!profile.empty()) {
		in.modsLowFirst = LoadModList(profile);
		std::reverse(in.modsLowFirst.begin(), in.modsLowFirst.end());
	}
	if (!game.empty()) in.gameData = game / "Data";
	in.loadOrder = merge::ReadLoadOrder(profile.empty() ? loadorder::GamePluginsTxt() : profile / "plugins.txt", game);
	const auto t0 = std::chrono::steady_clock::now();
	const auto r = assets::Find(in);
	std::ofstream(out) << r.report();
	std::printf("%zu files, %zu archives, %zu mod folders; %zu conflicting paths (%zu scripts), %zu mod pairs; %.1f s\nwrote %s\n", r.files,
		r.archives, r.modsRead, r.conflicts.size(), r.scriptConflicts, r.pairs.size(),
		std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), out.string().c_str());
	return 0;
}

// resaverpp-cli --analyze-logs [<My Games folder>] [--days <n>]
static int AnalyzeLogsMain(int argc, char** argv)
{
	std::vector<std::filesystem::path> myGames;
	logs::Options                      opt;
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--days" && i + 1 < argc) {
			opt.days = std::atoi(argv[++i]);
		} else {
			myGames.emplace_back(a);
		}
	}
	if (myGames.empty()) {
		if (const char* up = std::getenv("USERPROFILE")) {
			myGames.push_back(std::filesystem::path(up) / "Documents" / "My Games");
			myGames.push_back(std::filesystem::path(up) / "OneDrive" / "Documents" / "My Games");
		}
		if (const char* home = std::getenv("HOME")) {
			myGames.push_back(std::filesystem::path(home) / "Documents" / "My Games");
		}
	}
	const auto files = logs::Find(logs::Folders(myGames, {}));
	std::fputs(logs::ToText(logs::Analyze(files, opt)).c_str(), stdout);
	return 0;
}

int main(int argc, char** argv)
{
	if (argc < 2 || !std::strcmp(argv[1], "-h") || !std::strcmp(argv[1], "--help")) {
		Usage();
		return argc < 2 ? 1 : 0;
	}
	std::string              in = argv[1], out;
	{
		std::string low = in;
		for (auto& c : low) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		if (low.size() > 4 && low.ends_with(".pex")) {
			try {
				const auto file = pex::ReadFile(in);
				const bool psc = argc > 2 && std::string(argv[2]) == "--psc";
				std::vector<std::string> warnings;
				for (const auto& l : psc ? pex::Decompile(file, &warnings) : pex::Disassemble(file)) {
					std::puts(l.c_str());
				}
				for (const auto& w : warnings) {
					std::fprintf(stderr, "warning: %s\n", w.c_str());
				}
				return 0;
			} catch (const std::exception& e) {
				std::fprintf(stderr, "error: %s\n", e.what());
				return 2;
			}
		}
	}
	if (std::string(argv[1]) == "--plugin-property" && (argc == 6 || argc == 7)) {
		// resaverpp-cli --plugin-property <plugin> <record id> <script> <property> [<value id>]: read (or set) an object
		// property in a plugin's script data; ids as the plugin writes them (master index in the top byte)
		const std::filesystem::path plugin = argv[2];
		const auto rec = static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 16));
		if (argc == 6) {
			std::printf("%08X\n", ess::plugedit::GetObjectProperty(plugin, rec, argv[4], argv[5]));
			return 0;
		}
		const auto r = ess::plugedit::SetObjectProperty(plugin, rec, argv[4], argv[5], static_cast<std::uint32_t>(std::stoul(argv[6], nullptr, 16)));
		std::printf("%s\n", r.message.c_str());
		return r.ok ? 0 : 1;
	}
	if (std::string(argv[1]) == "--merged-patch") {
		return MergedPatchMain(argc, argv);
	}
	if (std::string(argv[1]) == "--file-conflicts") {
		return FileConflictsMain(argc, argv);
	}
	if (std::string(argv[1]) == "--analyze-logs") {
		return AnalyzeLogsMain(argc, argv);
	}
	std::vector<std::string> purge;
	bool                     fixMcm = false, delForms = false, keepInst = false, unattached = false, undefined = false, termUndef = false, nonexist = false;
	std::size_t              top = 20;
	bool                     modsView = false;
	std::string              sources, profile, removeMod, game, findText;
	bool                     names = false, havok = false, purify = false, batchAll = false, repMods = false, repAnom = false;
	std::string              batchFile, compareWith, reportFile, transplantFrom;
	bool                     dryRun = false, spawned = false, spawnedGone = false, charged = false, allowDamaged = false;
	bool                     pathIdles = false, fixPathIdles = false;
	int                      fixCharges = 0;
	bool                     questsView = false;
	bool journalOnly = false;
	std::uint32_t removeFlagForm = 0;
	int removeFlagBit = -1;
	std::uint32_t removeExtraForm = 0, resetAiForm = 0;
	std::vector<std::tuple<std::string, std::string, std::string>> setProps;
	std::vector<std::uint32_t> moveToPlayer;
	bool          npcsView = false;
	std::string   crashLogFile;
	bool          removeCrashActors = false;
	std::vector<std::uint32_t> deleteForms;
	std::string   npcFilter;
	std::vector<std::pair<std::string, std::string>> renamePlugins;
	bool          playerView = false;
	int           playerGold = -1;
	std::vector<std::pair<std::string, float>> playerSkills;
	std::vector<std::tuple<std::uint32_t, int, int>> playerBounties;
	std::vector<std::pair<std::string, std::vector<std::string>>> playerEdits;  // the other player edits, in order
	std::string   removeExtraName;
	std::string              questFilter;
	modremove::Options       removeOpt;
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "--purge-script") {
			purge.push_back(next());
		} else if (a == "--delete-forms") {
			delForms = true;
		} else if (a == "--fix-mcm") {
			fixMcm = true;
		} else if (a == "--keep-instances") {
			keepInst = true;
		} else if (a == "--remove-unattached") {
			unattached = true;
		} else if (a == "--remove-undefined") {
			undefined = true;
		} else if (a == "--terminate-undefined") {
			termUndef = true;
		} else if (a == "--remove-nonexistent") {
			nonexist = true;
		} else if (a == "--set-property") {
			const auto obj = next();
			const auto prop = next();
			setProps.emplace_back(obj, prop, next());
		} else if (a == "--remove-extra") {
			removeExtraForm = static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16));
			removeExtraName = next();
		} else if (a == "--player") {
			playerView = true;
		} else if (a == "--set-gold") {
			playerGold = std::atoi(next().c_str());
		} else if (a == "--set-skill") {
			const auto name = next();
			playerSkills.emplace_back(name, static_cast<float>(std::atof(next().c_str())));
		} else if (a == "--set-level" || a == "--set-xp" || a == "--set-perks" || a == "--set-souls" || a == "--reset-av" ||
				   a == "--clear-av-mods" || a == "--remove-item" || a == "--remove-spell" || a == "--clear-crimes") {
			playerEdits.push_back({ a, { next() } });
		} else if (a == "--set-av" || a == "--set-item" || a == "--set-legendary") {
			const auto x = next();
			playerEdits.push_back({ a, { x, next() } });
		} else if (a == "--set-bounty") {
			const auto fac = static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16));
			const int  v = std::atoi(next().c_str());
			const int  nv = std::atoi(next().c_str());
			playerBounties.emplace_back(fac, v, nv);
		} else if (a == "--move-to-player") {
			moveToPlayer.push_back(static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16)));
		} else if (a == "--crash-log") {
			crashLogFile = next();
		} else if (a == "--remove-crash-actors") {
			removeCrashActors = true;
		} else if (a == "--delete-form") {
			deleteForms.push_back(static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16)));
		} else if (a == "--npcs") {
			npcsView = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				npcFilter = argv[++i];
			}
		} else if (a == "--rename-plugin") {
			const auto from = next();
			renamePlugins.emplace_back(from, next());
		} else if (a == "--reset-ai") {
			resetAiForm = static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16));
		} else if (a == "--remove-flag") {
			removeFlagForm = static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16));
			removeFlagBit = std::atoi(next().c_str());
		} else if (a == "--journal") {
			questsView = true;
			journalOnly = true;
		} else if (a == "--quests") {
			questsView = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				questFilter = argv[++i];
			}
		} else if (a == "--allow-damaged") {
			allowDamaged = true;
		} else if (a == "--path-idles") {
			pathIdles = true;
		} else if (a == "--fix-path-idles") {
			fixPathIdles = true;
		} else if (a == "--charged") {
			charged = true;
		} else if (a == "--fix-charges") {
			fixCharges = std::atoi(next().c_str());
		} else if (a == "--spawned") {
			spawned = true;
		} else if (a == "--remove-spawned-gone") {
			spawnedGone = true;
		} else if (a == "-o") {
			out = next();
		} else if (a == "--mods") {
			modsView = true;
		} else if (a == "--sources") {
			sources = next();
		} else if (a == "--profile") {
			profile = next();
		} else if (a == "--remove-mod") {
			removeMod = next();
		} else if (a == "--keep-other-objects") {
			removeOpt.otherObjects = false;
		} else if (a == "--keep-spawned") {
			removeOpt.spawnedObjects = false;
		} else if (a == "--reset-havok") {
			havok = true;
		} else if (a == "--purify-formlists") {
			purify = true;
		} else if (a == "--batch") {
			batchFile = next();
		} else if (a == "--batch-all") {
			batchAll = true;
		} else if (a == "--transplant") {
			transplantFrom = next();
		} else if (a == "--compare") {
			compareWith = next();
		} else if (a == "--report") {
			reportFile = next();
		} else if (a == "--report-mods") {
			repMods = true;
		} else if (a == "--report-anomalies") {
			repAnom = true;
		} else if (a == "--names") {
			names = true;
		} else if (a == "--find") {
			findText = next();
			names = true;
		} else if (a == "--game") {
			game = next();
		} else if (a == "--dry-run") {
			dryRun = true;
		} else if (a == "--top") {
			top = std::stoul(next());
		} else {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			Usage();
			return 1;
		}
	}

	// two snapshots: resaverpp-cli good.txt --transplant bad.txt
	if (!transplantFrom.empty() && std::filesystem::path(in).extension() == ".txt") {
		using namespace ess::transplant;
		const auto good = ReadSnapshot(in);
		Snapshot bad;
		if (std::filesystem::path(transplantFrom).extension() == ".txt") {
			bad = ReadSnapshot(transplantFrom);
		} else if (auto bs = ess::SaveFile::load(transplantFrom)) {
			bad = FromSave(*bs, std::filesystem::path(transplantFrom).filename().string());  // partial: read without the game
		}
		if (!good.valid || !bad.valid) {
			std::fprintf(stderr, "%s%s\n", good.error.c_str(), bad.error.c_str());
			return 1;
		}
		std::size_t journal = 0, background = 0;
		for (const auto& ch : Diff(good, bad)) {
			if (ch.cat != Category::kQuests) {
				continue;
			}
			(ch.background ? background : journal) += 1;
			std::printf("  %s %s\n", ch.background ? "[background, unticked]" : "[journal, ticked]   ", ch.label.c_str());
		}
		std::printf("quests: %zu journal, %zu background\n", journal, background);
		return 0;
	}
	try {
		auto save = SaveFile::load(in);
		auto* p = save->papyrus();
		if (!save->problems.empty()) {
			std::fprintf(stderr, "warning: this save has structural problems; it may not be written back faithfully:\n");
			for (const auto& pr : save->problems) {
				std::fprintf(stderr, "  - %s\n", pr.c_str());
			}
		}
		const bool edits = !purge.empty() || unattached || undefined || termUndef || nonexist || !removeMod.empty() || fixMcm || havok || purify || !batchFile.empty() || spawnedGone || fixCharges > 0 || fixPathIdles || removeFlagBit >= 0 || !removeExtraName.empty() || resetAiForm != 0 || !moveToPlayer.empty() || !renamePlugins.empty() || playerGold >= 0 || !playerSkills.empty() || !playerBounties.empty() || !playerEdits.empty() || !setProps.empty() || removeCrashActors || !deleteForms.empty();
		std::vector<crashlog::Finding> crashFindings;
		if (!crashLogFile.empty()) {
			const auto log = crashlog::Read(crashLogFile);
			if (!log.ok) {
				std::fprintf(stderr, "could not read %s: %s\n", crashLogFile.c_str(), log.error.c_str());
				return 1;
			}
			std::printf("%s%s%s\n", log.tool.empty() ? "crash log" : log.tool.c_str(), log.time.empty() ? "" : ", ", log.time.c_str());
			if (!log.exception.empty()) {
				std::printf("%s\n", log.exception.c_str());
			}
			crashFindings = crashlog::Match(log, *save, crashlog::LeveledBases(*save));
			for (const auto& f : crashFindings) {
				std::string what;
				switch (f.kind) {
				case crashlog::Kind::kChanged:
					what = Fmt("in the save: %s change form%s", f.cfType.c_str(), f.created ? " (spawned/created)" : "");
					break;
				case crashlog::Kind::kRebuiltBase:
					what = f.refRaw ? Fmt("not in the save: the base NPC the game rebuilds for leveled actor %08X (%s change form)", f.target, f.cfType.c_str())
					                : std::string("not in the save: a leveled actor's rebuilt base NPC");
					break;
				case crashlog::Kind::kTemporary: what = "not in the save (made by the game after loading)"; break;
				case crashlog::Kind::kPlugin: what = "a plugin's form the save did not change"; break;
				}
				std::printf("%s %08X  %-24s %-34s x%-3zu %s%s\n", f.suspect ? "**" : "  ", f.form.id, f.form.type.substr(0, 24).c_str(),
					("\"" + f.form.name.substr(0, 32) + "\"").c_str(), f.form.hits, what.c_str(), f.scripts ? Fmt(", %zu scripts", f.scripts).c_str() : "");
			}
			std::size_t sus = 0;
			for (const auto& f : crashFindings) {
				sus += f.suspect ? 1 : 0;
			}
			std::printf(sus ? "** = a spawned or leveled actor the game was loading when it crashed: the likely cause\n"
			                : "no spawned or leveled actor of this save was being loaded when it crashed\n");
			if (!edits) {
				return 0;
			}
		} else if (removeCrashActors) {
			std::fputs("--remove-crash-actors needs --crash-log\n", stderr);
			return 1;
		}
		if (npcsView) {
			std::size_t shown = 0, total = 0, placed = 0;
			std::string flt = npcFilter;
			for (auto& ch : flt) {
				ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
			}
			for (const auto& a : cf::ListActors(*save)) {
				++total;
				placed += a.hasPosition ? 1 : 0;
				char id[16];
				std::snprintf(id, sizeof(id), "%08X", a.formID);
				if (!flt.empty() && std::string(id).find(flt) == std::string::npos) {
					continue;
				}
				++shown;
				if (!a.hasPosition) {
					std::printf("%08X  %s  at its plugin's position (never moved)\n", a.formID, a.isPlayer ? "PLAYER" : "      ");
				} else {
					std::printf("%08X  %s  in %08X  at %.0f, %.0f, %.0f%s\n", a.formID, a.isPlayer ? "PLAYER" : "      ", a.cell, a.pos[0],
						a.pos[1], a.pos[2], a.startCell ? (" (started in " + [&] { char b[16]; std::snprintf(b, 16, "%08X", a.startCell); return std::string(b); }() + ")").c_str() : "");
				}
			}
			std::printf("%zu actors, %zu with a saved position, %zu listed\n", total, placed, shown);
			return 0;
		}
		if (playerView) {
			const auto pv = cf::ReadPlayer(*save);
			if (!pv.error.empty()) {
				std::fprintf(stderr, "%s\n", pv.error.c_str());
				return 1;
			}
			std::printf("level: %d  level-up XP: %g of %g  perk points: %d\n", pv.level, pv.xp, pv.xpNext, pv.perkPoints);
			std::printf("gold: %d%s\n", pv.gold, pv.hasGold ? "" : " (none carried)");
			for (const auto& av : pv.avs) {
				if (av.id < 6 || av.id > 23) {
					std::printf("av %-24s base %s  permanent %g  temporary %g  damage %g\n", cf::ActorValueName(av.id),
						av.hasBase ? std::to_string(av.base).c_str() : "(plugin)", av.permanent, av.temporary, av.damage);
				}
			}
			std::printf("items: %zu kinds, spells added in game: %zu\n", pv.items.size(), pv.spells.size());
			for (int i = 0; i < 18; ++i) {
				std::printf("skill %-12s %g\n", cf::kSkillNames[i], pv.skills[i]);
			}
			for (const auto& b : pv.bounties) {
				if (b.violent || b.nonViolent || b.violentTotal || b.nonViolentTotal) {
					std::printf("faction %08X: bounty %d violent + %d non-violent (crime totals %d violent, %d non-violent)\n", b.faction,
						b.violent, b.nonViolent, b.violentTotal, b.nonViolentTotal);
				}
			}
			return 0;
		}
		if (edits && !save->problems.empty() && !allowDamaged) {
			std::fprintf(stderr, "error: not editing a damaged save (use --allow-damaged to write it anyway, at your own risk)\n");
			return 3;
		}
		ScriptSources src;
		if (!sources.empty() && (modsView || !removeMod.empty() || names || !reportFile.empty() || spawned || spawnedGone || questsView)) {
			std::filesystem::path profDir = profile, overwrite, gameData;
			if (auto mo2 = FindMo2Instance(sources)) {
				if (profDir.empty()) {
					profDir = mo2->profilesDir / mo2->selectedProfile;
				}
				overwrite = mo2->overwriteDir;
				if (!mo2->gamePath.empty()) {
					gameData = mo2->gamePath / "Data";
				}
				std::printf("MO2 instance %s, profile %s\n", mo2->ini.string().c_str(), profDir.string().c_str());
			}
			if (!game.empty()) {
				gameData = std::filesystem::path(game) / "Data";
			}
			const auto order = profDir.empty() ? std::vector<std::string>{} : LoadModList(profDir);
			if (auto vx = FindMo2Instance(sources) ? std::nullopt : FindVortexDeployment(sources)) {
				std::printf("Vortex: staging %s, %zu deployed mods\n", vx->stagingPath.string().c_str(), vx->mods.size());
				src.scanVortex(*vx);
			} else {
				src.scan(sources, order.empty() ? nullptr : &order, overwrite, gameData);
			}
			std::printf("scanned %zu enabled + %zu disabled mod folders%s, %zu scripts, %zu BSAs, %zu errors\n", src.modsScanned, src.disabledScanned,
				src.gameDataScanned ? " + game Data" : "", src.pexFound, src.bsasRead, src.errors.size());
		}
		if (pathIdles) {
			const auto stuck = cf::FindStuckPathIdles(*save);
			std::printf("%zu character(s) saved waiting for an idle the save does not keep (this save crashes on load)\n", stuck.size());
			for (const auto& s : stuck) {
				std::printf("  %08X  %zu path follower(s) in state 14\n", s.formID, s.count);
			}
		}
		if (charged) {
			const auto items = cf::FindChargedItems(*save);
			std::size_t bad = 0;
			for (const auto& c : items) {
				bad += cf::NeedsChargeFix(c.inv) ? 1 : 0;
			}
			std::printf("%zu enchanted/charged inventory item(s), %zu cannot be recharged\n", items.size(), bad);
			for (std::size_t i = 0; i < items.size() && i < top; ++i) {
				const auto& c = items[i];
				std::printf("  holder %08X  item %08X x%d  ench %08X  max %u  charge %s%s\n", c.holder, c.inv.item, c.inv.count, c.inv.ench, c.inv.maxCharge,
					c.inv.hasCharge ? std::to_string(c.inv.charge).c_str() : "(full)", cf::NeedsChargeFix(c.inv) ? "  <- NEEDS FIX" : "");
			}
			if (!edits) {
				return 0;
			}
		}
		if (spawned) {
			const auto groups = orphans::Find(*save, src.scanned() ? &src : nullptr);
			std::size_t total = 0;
			for (const auto& g : groups) {
				total += g.instances;
			}
			std::printf("%zu instance(s) on missing spawned objects in %zu group(s)\n", total, groups.size());
			for (const auto& g : groups) {
				std::printf("\n%-40s %-28s %6zu instance(s), %zu held\n", g.mod.c_str(), orphans::StatusText(g.status), g.instances, g.held);
				for (std::size_t i = 0; i < g.scripts.size() && i < top; ++i) {
					std::printf("    %-50s %6zu  held %zu\n", g.scripts[i].script.c_str(), g.scripts[i].ids.size(), g.scripts[i].held.size());
				}
				if (g.scripts.size() > top) {
					std::printf("    ... %zu more script(s)\n", g.scripts.size() - top);
				}
			}
			if (!edits) {
				return 0;
			}
		}
		if (!transplantFrom.empty()) {
			using namespace ess::transplant;
			const auto good = FromSave(*save, std::filesystem::path(in).filename().string());
			Snapshot   bad;
			if (std::filesystem::path(transplantFrom).extension() == ".txt") {
				bad = ReadSnapshot(transplantFrom);
			} else {
				auto other = SaveFile::load(transplantFrom);
				bad = FromSave(*other, std::filesystem::path(transplantFrom).filename().string());
			}
			if (!bad.valid) {
				std::fprintf(stderr, "%s\n", bad.error.c_str());
				return 1;
			}
			const auto changes = Diff(good, bad);
			std::printf("%zu thing(s) the bad save has and this one does not:\n", changes.size());
			for (int c = 0; c < static_cast<int>(Category::kCount); ++c) {
				bool head = false;
				for (const auto& ch : changes) {
					if (static_cast<int>(ch.cat) != c) {
						continue;
					}
					if (!head) {
						std::printf("\n%s\n", CategoryName(static_cast<Category>(c)));
						head = true;
					}
					std::printf("  [%s] %s%s%s\n", ch.risk == 0 ? "safe" : ch.risk == 1 ? "caution" : "risky", ch.label.c_str(), ch.detail.empty() ? "" : "   ",
						ch.detail.c_str());
				}
			}
			return 0;
		}
		if (!compareWith.empty()) {
			auto other = SaveFile::load(compareWith);
			for (const auto& l : compare::Compare(*save, *other).text()) {
				std::puts(l.c_str());
			}
			return 0;
		}
		if (!reportFile.empty()) {
			report::Options ro;
			if (repMods || repAnom) {
				ro.mods = repMods;
				ro.anomalies = repAnom;
			}
			std::unique_ptr<PluginDB> db;
			if (!src.empty()) {
				PluginDB::Input in;
				PrepareInput(*save, in);
				in.sources = src;
				PluginDB::Progress prog;
				db = std::make_unique<PluginDB>();
				db->load(in, prog);
			}
			const auto table = mods::Build(*save, src.empty() ? nullptr : &src);
			const auto lines = report::Build(*save, table, db.get(), ro, std::filesystem::path(in).filename().string());
			std::filesystem::path rp = reportFile;
			std::string ext = rp.extension().string();
			for (auto& c : ext) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (ext == ".pdf") {
				report::WritePdf(rp, "ReSaver++ report - " + std::filesystem::path(in).filename().string(), lines);
			} else {
				report::WriteText(rp, lines);
			}
			std::printf("report written: %s (%zu lines)\n", reportFile.c_str(), lines.size());
			return 0;
		}
		if (questsView) {
			std::unique_ptr<PluginDB> db;
			if (!src.empty()) {
				PluginDB::Input in2;
				PrepareInput(*save, in2);
				in2.sources = src;
				PluginDB::Progress prog;
				db = std::make_unique<PluginDB>();
				db->load(in2, prog);
				std::printf("plugins read %zu, quests defined %zu\n", db->pluginsRead, db->questDefs().size());
			}
			auto        states = quests::ReadStates(*save);
			if (db && !questFilter.empty()) {
				// with a search text, quests of the load order that this save never started are included
				std::set<std::uint32_t> have;
				for (const auto& q : states) {
					have.insert(q.formID);
				}
				for (const auto& [id, d] : db->questDefs()) {
					if (!have.contains(id)) {
						quests::QuestState q;
						q.formID = id;
						states.push_back(q);
					}
				}
			}
			std::size_t running = 0, shown = 0;
			for (const auto& q : states) {
				running += q.running() && !q.completed() ? 1 : 0;
				shown += q.shownObjectives();
			}
			std::printf("%zu quests in the save, %zu running, %zu objectives shown in the journal\n", states.size(), running, shown);
			{
				bool        found = false;
				const auto  j = quests::ReadJournal(*save, &found);
				std::size_t inJournal = 0;
				for (const auto& q : states) {
					inJournal += q.inJournal() ? 1 : 0;
				}
				if (found) {
					std::printf("journal: %zu entries, %zu quests listed (read from the player's record)\n", j.size(), inJournal);
				} else {
					std::printf("journal: not found in the player's record (objective counts come from the quests' own records)\n");
				}
			}
			auto lower = [](std::string x) {
				for (auto& c : x) {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				return x;
			};
			const auto needle = lower(questFilter);
			std::size_t listed = 0;
			for (const auto& q : states) {
				const auto* def = db ? db->quest(q.formID) : nullptr;
				const std::string edid = def ? def->edid : "", name = def ? def->name : "";
				if (!needle.empty() && lower(edid).find(needle) == std::string::npos && lower(name).find(needle) == std::string::npos) {
					continue;
				}
				if (needle.empty() && (journalOnly ? !q.inJournal() : !(q.running() && !q.completed()))) {
					continue;
				}
				if (++listed > top && needle.empty()) {
					continue;
				}
				std::printf("%08X  %-32s %-30s %-24s stage %d  shown %zu", q.formID, edid.c_str(), name.c_str(), q.statusText(), q.highestDoneStage(),
					q.shownObjectives());
				if (def) {
					std::string cs;
					for (auto st : def->completionStages()) {
						cs += (cs.empty() ? "" : ",") + std::to_string(st);
					}
					std::printf("  completes at %s", cs.empty() ? "-" : cs.c_str());
				}
				std::printf("\n");
				if (!needle.empty() && def) {
					std::printf("    plugin %s, fragment script %s (%zu fragments, %zu properties)\n", db->plugins()[def->plugin].c_str(),
						def->fragmentScript.c_str(), def->fragments.size(), def->properties.size());
					for (const auto& sg : def->stages) {
						bool done = false;
						for (const auto& [st, d] : q.stages) {
							done |= st == sg.index && d;
						}
						const auto* fr = def->fragmentFor(sg.index);
						std::printf("    stage %4u %s%s%s%s\n", sg.index, done ? "[done] " : "", sg.completes() ? "[COMPLETES] " : "", sg.fails() ? "[FAILS] " : "",
							fr ? fr->function.c_str() : "");
						for (const auto& l : sg.logs) {
							if (!l.text.empty()) {
								std::printf("        \"%s\"\n", l.text.substr(0, 140).c_str());
							}
						}
					}
					for (const auto& o : def->objectives) {
						int state = 0;
						for (const auto& os : q.objectives) {
							if (os.index == o.index) {
								state = os.state;
							}
						}
						std::printf("    objective %u (%s): %s\n", o.index, quests::ObjectiveStateText(state), o.text.c_str());
					}
					if (q.journalKnown) {
						std::string js;
						for (const auto& o : q.journal) {
							js += (js.empty() ? "" : ", ") + std::to_string(o.index) + " " + quests::ObjectiveStateText(o.state);
						}
						std::printf("    in the journal: %s\n", js.empty() ? "no" : js.c_str());
					}
					for (const auto& pr : def->properties) {
						std::printf("    property %s = %s\n", pr.name.c_str(), pr.type == 1 ? db->describe(pr.form).c_str() : pr.text.c_str());
					}
					// earlier quests of its quest line
					{
						std::map<std::uint32_t, std::vector<std::pair<std::uint16_t, std::vector<std::string>>>> cache;
						auto bodies = [&](std::uint32_t qid) {
							auto it = cache.find(qid);
							if (it != cache.end()) {
								return it->second;
							}
							std::vector<std::pair<std::uint16_t, std::vector<std::string>>> out;
							const auto* d = db->quest(qid);
							try {
								if (d && !d->fragmentScript.empty()) {
									if (auto px = src.findPex(d->fragmentScript)) {
										const auto code = pex::Decompile(pex::Read(ScriptSources::readPex(*px)));
										for (auto st : d->completionStages()) {
											if (const auto* fr = d->fragmentFor(st)) {
												out.push_back({ st, quests::FunctionBody(code, fr->function) });
											}
										}
									}
								}
							} catch (...) {
							}
							cache[qid] = out;
							return out;
						};
						std::set<std::uint32_t> seen{ q.formID };
						std::function<void(std::uint32_t, int)> walk = [&](std::uint32_t t, int depth) {
							if (depth > 12) {
								return;
							}
							const auto pre = quests::LeadsInto(t, db->questDefs(), bodies);
							for (auto id : pre) {
								if (!seen.insert(id).second) {
									continue;
								}
								bool done = false;
								for (const auto& s2 : states) {
									done |= s2.formID == id && s2.completed();
								}
								const auto* d = db->quest(id);
								std::printf("    %*sleads here: %08X %s '%s' %s%s\n", depth * 2, "", id, d->edid.c_str(), d->name.c_str(), done ? "[completed]" : "",
									pre.size() > 1 ? " (one of several)" : "");
								if (!done) {
									walk(id, depth + 1);
								}
							}
						};
						walk(q.formID, 0);
					}
					// what the completion stage's script does
					if (!def->fragmentScript.empty()) {
						try {
							if (auto px = src.findPex(def->fragmentScript)) {
								const auto file = pex::Read(ScriptSources::readPex(*px));
								const auto code = pex::Decompile(file);
								for (auto st : def->completionStages()) {
									const auto* fr = def->fragmentFor(st);
									if (!fr) {
										continue;
									}
									std::printf("    stage %u script (%s):\n", st, fr->function.c_str());
									const auto body = quests::FunctionBody(code, fr->function);
									for (const auto& l : body) {
										std::printf("        %s\n", l.c_str());
									}
									for (const auto& e : quests::ScanEffects(body)) {
										std::printf("      %s: %s\n", e.kind == quests::Effect::Kind::kReward ? "REWARD" : e.kind == quests::Effect::Kind::kQuest ? "QUEST" : "CALLS",
											e.line.c_str());
									}
								}
							} else {
								std::printf("    (fragment script %s not found)\n", def->fragmentScript.c_str());
							}
						} catch (const std::exception& e) {
							std::printf("    (fragment script: %s)\n", e.what());
						}
					}
				}
			}
			return 0;
		}
		if (names) {
			PluginDB::Input in;
			PrepareInput(*save, in);
			in.sources = src;
			PluginDB::Progress prog;
			PluginDB          db;
			const auto        t0 = std::chrono::steady_clock::now();
			db.load(in, prog);
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			std::printf("plugins read %zu of %zu in %lld ms, %zu records kept, %zu forms, %zu missing, %zu errors\n", db.pluginsRead, db.plugins().size(),
				static_cast<long long>(ms), db.recordsRead, db.size(), db.missing.size(), db.errors.size());
			for (std::size_t i = 0; i < db.missing.size() && i < 10; ++i) {
				std::printf("  missing: %s\n", db.missing[i].c_str());
			}
			for (std::size_t i = 0; i < db.errors.size() && i < 10; ++i) {
				std::printf("  error: %s\n", db.errors[i].c_str());
			}
			for (std::uint32_t id : { 0x00000007u, 0x00000014u, 0x0001A66Au, 0x00013BBFu, 0x0003BC26u }) {
				std::printf("  %08X %s\n", id, db.describe(id).c_str());
			}
			if (!findText.empty()) {
				for (auto id : db.search(findText, 40)) {
					std::printf("  %08X %s (%s)\n", id, db.describe(id).c_str(), db.pluginName(*db.find(id)).c_str());
				}
			}
			if (p) {
				std::size_t shown = 0;
				for (const auto& i : p->instances) {
					if (shown >= 8) {
						break;
					}
					if (const auto id = save->formIDOf(i.refID); id && (id >> 24) != 0xFF) {
						const auto d = db.describe(id);
						if (!d.empty() && d.find('\'') != std::string::npos) {
							std::printf("  instance %s on %08X %s\n", p->str(i.script).c_str(), id, d.c_str());
							++shown;
						}
					}
				}
			}
			return 0;
		}
		if (!removeMod.empty()) {
			if (!p) {
				std::fputs("save has no Papyrus section\n", stderr);
				return 1;
			}
			const auto plan = modremove::MakePlan(*save, removeMod, src.empty() ? nullptr : &src, removeOpt);
			std::fputs(plan.summary().c_str(), stdout);
			if (!plan.canApply()) {
				std::fputs("\nnot removed.\n", stdout);
				return 3;
			}
			if (dryRun) {
				return 0;
			}
			std::error_code ec;
			if (out.empty() || std::filesystem::equivalent(in, out, ec)) {
				std::fputs("\n--remove-mod writes a new file: give -o <new.ess> (not the input file)\n", stderr);
				return 1;
			}
			std::printf("\nremoved: %s\n", modremove::Apply(*save, plan).text().c_str());
		}
		if (modsView) {
			auto t = mods::Build(*save, src.empty() ? nullptr : &src);
			std::vector<const mods::ModRow*> rows;
			for (const auto& r : t.rows) {
				if (r.scriptCount || r.threadCount || !r.provided.empty()) {
					rows.push_back(&r);
				}
			}
			std::sort(rows.begin(), rows.end(), [](auto a, auto b) { return a->threadCount != b->threadCount ? a->threadCount > b->threadCount : a->instanceCount > b->instanceCount; });
			std::printf("%-50s %8s %10s %8s %9s\n", "mod", "scripts", "instances", "running", "provides");
			for (std::size_t i = 0; i < rows.size() && i < top; ++i) {
				const auto* r = rows[i];
				std::printf("%-50.50s %8zu %10zu %8zu %9zu\n", r->name.c_str(), r->scriptCount, r->instanceCount, r->threadCount, r->providedInSave);
			}
			return 0;
		}
		if (!edits) {
			std::fputs(ops::FormatReport(ops::Analyze(*save), top).c_str(), stdout);
			return 0;
		}
		if (out.empty()) {
			std::fputs("an output file (-o) is required when editing\n", stderr);
			return 1;
		}
		if (!p) {
			std::fputs("save has no Papyrus section\n", stderr);
			return 1;
		}
		if (removeFlagBit >= 0) {
			ChangeForm* target = nullptr;
			for (auto& c : save->changeForms) {
				if (save->formIDOf(c.refID) == removeFlagForm) {
					target = &c;
					break;
				}
			}
			if (!target) {
				std::fprintf(stderr, "no change form for %08X in this save\n", removeFlagForm);
				return 1;
			}
			const auto why = cf::RemoveFlag(*save, *target, removeFlagBit);
			if (!why.empty()) {
				std::fprintf(stderr, "could not remove flag %d from %08X: %s\n", removeFlagBit, removeFlagForm, why.c_str());
				return 1;
			}
			std::printf("removed change flag %d from %08X\n", removeFlagBit, removeFlagForm);
		}
		auto findCf = [&](std::uint32_t a_form) -> ChangeForm* {
			for (auto& c : save->changeForms) {
				if (save->formIDOf(c.refID) == a_form) {
					return &c;
				}
			}
			return nullptr;
		};
		for (const auto& [objText, propName, formText] : setProps) {
			auto* pp = save->papyrus();
			if (!pp) {
				std::fprintf(stderr, "this save has no Papyrus data\n");
				return 1;
			}
			auto&            p = *pp;
			ScriptInstance*  inst = nullptr;
			const auto       at = objText.find('@');
			auto lower = [](std::string x) {
				for (auto& c : x) {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				return x;
			};
			if (at == std::string::npos) {
				inst = p.findInstance(std::stoull(objText, nullptr, 16));
			} else {
				const auto sc = lower(objText.substr(0, at));
				const auto fid = props::ParseFormID(*save, objText.substr(at + 1));
				for (auto& i : p.instances) {
					if (lower(p.str(i.script)) == sc && save->formIDOf(i.refID) == fid) {
						inst = &i;
						break;
					}
				}
			}
			if (!inst || !inst->data) {
				std::fprintf(stderr, "no script instance %s in this save\n", objText.c_str());
				return 1;
			}
			// the script's members, parents first (the order of the instance's variables)
			std::vector<const Script*> chain;
			for (const Script* sc = p.findScript(inst->script); sc && chain.size() < 32; sc = p.findScript(sc->parent)) {
				chain.push_back(sc);
				if (p.str(sc->parent).empty()) {
					break;
				}
			}
			std::vector<std::pair<std::string, std::string>> members;  // name, type
			for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
				for (const auto& m : (*it)->members) {
					members.emplace_back(p.str(m.name), p.str(m.type));
				}
			}
			std::size_t idx = members.size();
			for (std::size_t k = 0; k < members.size(); ++k) {
				const auto n = lower(members[k].first);
				if (n == lower(propName) || n == "::" + lower(propName) + "_var") {
					idx = k;
					break;
				}
			}
			if (idx >= members.size() || idx >= inst->data->vars.size()) {
				std::fprintf(stderr, "%s has no property %s; its variables:\n", p.str(inst->script).c_str(), propName.c_str());
				for (const auto& [n, t] : members) {
					std::fprintf(stderr, "  %s %s\n", t.c_str(), n.c_str());
				}
				return 1;
			}
			const auto fid = props::ParseFormID(*save, formText);
			if (!fid) {
				std::fprintf(stderr, "not a form id of a plugin in this save: %s\n", formText.c_str());
				return 1;
			}
			auto&      var = inst->data->vars[idx];
			std::string type = members[idx].second;
			if (var.type == VType::kRef) {
				type = p.str(var.refType);
			}
			const auto scriptName = p.str(inst->script);  // inst can move when an object is added
			const auto o = props::PointAt(*save, var, type, fid);
			if (!o.ok) {
				std::fprintf(stderr, "could not set %s: %s\n", members[idx].first.c_str(), o.message.c_str());
				return 1;
			}
			std::printf("%s.%s now points at %08X: %s\n", scriptName.c_str(), members[idx].first.c_str(), fid, o.message.c_str());
		}
		if (!removeExtraName.empty()) {
			auto* target = findCf(removeExtraForm);
			if (!target) {
				std::fprintf(stderr, "no change form for %08X in this save\n", removeExtraForm);
				return 1;
			}
			const auto d = cf::Decode(*save, *target);
			std::size_t idx = d.extras.size();
			for (std::size_t k = 0; k < d.extras.size(); ++k) {
				if (d.extras[k].name == removeExtraName) {
					idx = k;
					break;
				}
			}
			if (idx == d.extras.size()) {
				std::fprintf(stderr, "%08X has no \"%s\" extra data\n", removeExtraForm, removeExtraName.c_str());
				return 1;
			}
			const auto why = cf::RemoveExtra(*save, *target, idx);
			if (!why.empty()) {
				std::fprintf(stderr, "could not remove %s from %08X: %s\n", removeExtraName.c_str(), removeExtraForm, why.c_str());
				return 1;
			}
			std::printf("removed %s from %08X\n", removeExtraName.c_str(), removeExtraForm);
		}
		if (playerGold >= 0) {
			const auto why = cf::SetPlayerGold(*save, playerGold);
			if (!why.empty()) {
				std::fprintf(stderr, "could not set gold: %s\n", why.c_str());
				return 1;
			}
			std::printf("set the player's gold to %d\n", playerGold);
		}
		for (const auto& [name, level] : playerSkills) {
			int idx = -1;
			for (int i = 0; i < 18; ++i) {
				std::string a = cf::kSkillNames[i], b = name;
				std::erase_if(a, [](char ch) { return ch == ' ' || ch == '-'; });
				std::erase_if(b, [](char ch) { return ch == ' ' || ch == '-'; });
				if (_stricmp_portable(a, b)) {
					idx = i;
				}
			}
			if (idx < 0) {
				std::fprintf(stderr, "no skill called %s\n", name.c_str());
				return 1;
			}
			const auto why = cf::SetPlayerSkill(*save, idx, level);
			if (!why.empty()) {
				std::fprintf(stderr, "could not set %s: %s\n", cf::kSkillNames[idx], why.c_str());
				return 1;
			}
			std::printf("set the player's %s to %g\n", cf::kSkillNames[idx], level);
		}
		for (const auto& [op, args] : playerEdits) {
			auto avId = [](const std::string& a_s) -> std::uint32_t {
				for (std::uint32_t i = 0; i < 164; ++i) {
					if (_stricmp_portable(cf::ActorValueName(i), a_s)) {
						return i;
					}
				}
				return static_cast<std::uint32_t>(std::stoul(a_s));
			};
			auto hex = [](const std::string& a_s) { return static_cast<std::uint32_t>(std::stoul(a_s, nullptr, 16)); };
			auto skill = [](const std::string& a_s) {
				for (int i = 0; i < 18; ++i) {
					std::string a = cf::kSkillNames[i], b = a_s;
					std::erase_if(a, [](char ch) { return ch == ' ' || ch == '-'; });
					std::erase_if(b, [](char ch) { return ch == ' ' || ch == '-'; });
					if (_stricmp_portable(a, b)) {
						return i;
					}
				}
				return -1;
			};
			std::string why;
			try {
				if (op == "--set-level") {
					why = cf::SetPlayerLevel(*save, std::stoi(args[0]));
				} else if (op == "--set-xp") {
					why = cf::SetPlayerXP(*save, std::stof(args[0]));
				} else if (op == "--set-perks") {
					why = cf::SetPlayerPerkPoints(*save, std::stoi(args[0]));
				} else if (op == "--set-souls") {
					why = cf::SetPlayerDragonSouls(*save, std::stoi(args[0]));
				} else if (op == "--reset-av") {
					why = cf::ResetPlayerAV(*save, avId(args[0]));
				} else if (op == "--clear-av-mods") {
					why = cf::ClearPlayerAVModifiers(*save, avId(args[0]), 1 | 4);
				} else if (op == "--remove-item") {
					why = cf::RemovePlayerItem(*save, hex(args[0]));
				} else if (op == "--remove-spell") {
					why = cf::RemovePlayerSpell(*save, hex(args[0]));
				} else if (op == "--clear-crimes") {
					why = cf::ClearPlayerCrimes(*save, hex(args[0]));
				} else if (op == "--set-av") {
					why = cf::SetPlayerAV(*save, avId(args[0]), std::stof(args[1]));
				} else if (op == "--set-item") {
					why = cf::SetPlayerItemCount(*save, hex(args[0]), std::stoi(args[1]));
				} else if (op == "--set-legendary") {
					why = cf::SetPlayerLegendary(*save, skill(args[0]), std::stoi(args[1]));
				}
			} catch (const std::exception&) {
				why = "bad value";
			}
			if (!why.empty()) {
				std::fprintf(stderr, "%s %s: %s\n", op.c_str(), args[0].c_str(), why.c_str());
				return 1;
			}
			std::printf("%s %s: done\n", op.c_str(), args[0].c_str());
		}
		for (const auto& [fac, v, nv] : playerBounties) {
			const auto why = cf::SetPlayerBounty(*save, fac, v, nv);
			if (!why.empty()) {
				std::fprintf(stderr, "could not set the bounty with %08X: %s\n", fac, why.c_str());
				return 1;
			}
			std::printf("set the player's bounty with %08X to %d violent + %d non-violent\n", fac, v, nv);
		}
		for (const auto& [from, to] : renamePlugins) {
			if (const auto why = ops::RenamePlugin(*save, from, to); !why.empty()) {
				std::fprintf(stderr, "could not rename %s: %s\n", from.c_str(), why.c_str());
				return 1;
			}
			std::printf("the save now loads %s where it had %s\n", to.c_str(), from.c_str());
		}
		for (const auto fid : moveToPlayer) {
			auto* target = findCf(fid);
			if (!target) {
				std::fprintf(stderr, "no change form for %08X in this save\n", fid);
				return 1;
			}
			if (const auto why = cf::MoveActorToPlayer(*save, *target); !why.empty()) {
				std::fprintf(stderr, "could not move %08X: %s\n", fid, why.c_str());
				return 1;
			}
			std::printf("moved %08X next to the player\n", fid);
		}
		{
			std::set<std::uint32_t> del;
			for (const auto fid : deleteForms) {
				auto* target = findCf(fid);
				if (!target) {
					std::fprintf(stderr, "no change form for %08X in this save\n", fid);
					return 1;
				}
				del.insert(target->refID.data);
			}
			if (removeCrashActors) {
				for (const auto& f : crashFindings) {
					if (f.suspect) {
						del.insert(f.refRaw);
					}
				}
				if (del.empty()) {
					std::fputs("the crash log names no spawned or leveled actor of this save at the crash: nothing to remove\n", stderr);
					return 1;
				}
			}
			if (!del.empty()) {
				std::printf("deleted %zu change form(s)\n", ops::DeleteChangeForms(*save, del));
			}
		}
		if (resetAiForm != 0) {
			auto* target = findCf(resetAiForm);
			if (!target) {
				std::fprintf(stderr, "no change form for %08X in this save\n", resetAiForm);
				return 1;
			}
			const auto why = cf::ResetAI(*save, *target);
			if (!why.empty()) {
				std::fprintf(stderr, "could not reset the AI of %08X: %s\n", resetAiForm, why.c_str());
				return 1;
			}
			std::printf("reset the AI of %08X\n", resetAiForm);
		}
		for (const auto& name : purge) {
			const auto r = ops::PurgeScript(*save, name, delForms, keepInst);
			std::printf("purge %s: %zu threads, %zu suspended stacks, %zu messages, %zu instances, %zu created forms\n", name.c_str(), r.threads,
				r.stacks, r.messages, r.instances, r.forms);
		}
		if (!batchFile.empty()) {
			std::ifstream bf(batchFile);
			std::string   list((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
			auto          matches = batch::Resolve(*save, list);
			for (auto& mt : matches) {
				if (batchAll) {
					mt.selected = true;
				}
				std::printf("  %s %s (%zu instances, %zu threads)%s\n", mt.selected ? "purge" : "skip ", mt.script.c_str(), mt.instances, mt.threads,
					mt.prompt.empty() ? "" : ("  @@ " + mt.prompt).c_str());
			}
			const auto br = batch::Apply(*save, matches);
			std::printf("batch clean: %zu scripts, %zu threads, %zu stacks, %zu messages, %zu instances\n", br.scripts, br.threads, br.stacks,
				br.messages, br.instances);
		}
		if (havok) {
			const auto hr = cf::ResetHavok(*save);
			std::printf("reset havok: %zu references reset, %zu could not be decoded (left alone)\n", hr.reset, hr.failed);
		}
		if (purify) {
			const auto pr = cf::PurifyFormLists(*save);
			std::printf("purify formlists: %zu null entries removed from %zu lists (%zu lists not decodable)\n", pr.entries, pr.forms, pr.failed);
		}
		if (fixMcm) {
			std::printf("fixed MCM jam: %s\n", ops::FixMcmJam(*p).text().c_str());
		}
		if (termUndef) {
			std::printf("terminated undefined threads: %zu\n", ops::TerminateUndefinedThreads(*p));
		}
		if (undefined) {
			std::printf("removed undefined elements: %zu\n", ops::RemoveUndefinedElements(*p));
		}
		if (unattached) {
			std::printf("removed unattached instances: %zu\n", ops::RemoveUnattachedInstances(*p));
		}
		if (nonexist) {
			std::printf("removed instances on non-existent created forms: %zu\n", ops::RemoveNonexistentCreated(*save));
		}
		if (fixPathIdles) {
			std::printf("fixed the path of %zu character(s) (state 14 -> 1)\n", cf::FixStuckPathIdles(*save));
		}
		if (fixCharges > 0) {
			std::printf("fixed charges of %zu item(s)\n", cf::FixCharges(*save, static_cast<std::uint16_t>(std::min(fixCharges, 65535)), {}));
		}
		if (spawnedGone) {
			if (!src.scanned()) {
				std::fprintf(stderr, "--remove-spawned-gone needs --sources (to tell which mods are installed)\n");
				return 1;
			}
			std::set<EID> ids;
			for (const auto& g : orphans::Find(*save, &src)) {
				if (g.status != orphans::Status::kNotInstalled && g.status != orphans::Status::kDisabled && g.status != orphans::Status::kEngine) {
					continue;
				}
				for (const auto& sg : g.scripts) {
					for (EID id : sg.ids) {
						if (!sg.held.contains(id)) {
							ids.insert(id);
						}
					}
				}
			}
			std::printf("removed instances on missing spawned objects of mods no longer installed: %zu\n", orphans::Remove(*save, ids));
		}
		{
			std::error_code ec;
			const auto      wr = save->write(out, std::filesystem::exists(out, ec));
			if (!wr.backup.empty()) {
				std::printf("previous file kept as %s%s\n", wr.backup.filename().string().c_str(),
					wr.cosaveBackup.empty() ? "" : (" + " + wr.cosaveBackup.filename().string()).c_str());
			}
		}
		// verify by reading it back
		auto check = SaveFile::load(out);
		std::printf("written %s and verified (re-read OK: %zu instances, %zu threads)\n", out.c_str(), check->papyrus()->instances.size(),
			check->papyrus()->activeScripts.size());
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
	return 0;
}
