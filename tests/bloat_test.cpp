// Save bloat by mod and the load order check.
//  - load order check: synthetic plugins written with MergedPatch::WritePlugin (light ranges, masters, header versions,
//    the 254 full plugin limit), then the real Fallout 4 Data folder in RESAVERPP_FO4_DATA (if set);
//  - save bloat: every .fos under RESAVERPP_FO4_SAVES and every .ess under RESAVERPP_SKYRIM_SAVES (folders separated
//    by ';'): every byte of the Papyrus block and of the change forms is either in a row or counted as shared Papyrus
//    data, every instance/thread/array/struct is in exactly one row; growth between consecutive saves of a folder.
#include "core/AssetConflicts.h"
#include "core/LoadOrder.h"
#include "core/LoadOrderCheck.h"
#include "core/LoadOrderFix.h"
#include "core/MergedPatch.h"
#include "core/Save.h"
#include "core/SaveBloat.h"
#include "core/ScriptSources.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace ess;
namespace fs = std::filesystem;

namespace
{
	int g_fail = 0, g_pass = 0;
	void Check(bool a_ok, const std::string& a_what)
	{
		if (a_ok) {
			++g_pass;
		} else {
			++g_fail;
			std::printf("FAIL: %s\n", a_what.c_str());
		}
	}

	merge::Record Misc(std::uint32_t a_id)
	{
		merge::Record r;
		std::memcpy(r.sig, "MISC", 4);
		r.formID = a_id;
		r.version = 131;
		merge::Sub s;
		std::memcpy(s.sig, "EDID", 4);
		const std::string e = "Rec" + std::to_string(a_id);
		s.data.assign(e.begin(), e.end());
		s.data.push_back(0);
		r.subs.push_back(s);
		return r;
	}

	bool Has(const locheck::Result& r, locheck::Severity s, const std::string& plugin, const std::string& text)
	{
		for (const auto& f : r.findings) {
			if (f.severity == s && f.plugin == plugin && f.text.find(text) != std::string::npos) {
				return true;
			}
		}
		return false;
	}

	std::vector<fs::path> Files(const char* a_env, const char* a_ext)
	{
		std::vector<fs::path> out;
		const char*           env = std::getenv(a_env);
		if (!env || !*env) {
			return out;
		}
		std::string all = env;
		for (std::size_t p = 0; p <= all.size();) {
			auto e = all.find(';', p);
			if (e == std::string::npos) {
				e = all.size();
			}
			const fs::path  dir = all.substr(p, e - p);
			std::error_code ec;
			if (!dir.empty() && fs::is_directory(dir, ec)) {
				std::vector<fs::path> here;
				for (const auto& f : fs::directory_iterator(dir, ec)) {
					if (f.path().extension() == a_ext) {
						here.push_back(f.path());
					}
				}
				std::sort(here.begin(), here.end(), [](const fs::path& a, const fs::path& b) { return fs::last_write_time(a) < fs::last_write_time(b); });
				out.insert(out.end(), here.begin(), here.end());
			} else if (!dir.empty() && fs::is_regular_file(dir, ec)) {
				out.push_back(dir);
			}
			p = e + 1;
		}
		return out;
	}

	// A Wabbajack list can store Mod Organizer's game folder relative to Mod Organizer's own folder (".\\StockGame");
	// Mod Organizer reads it from there, so must ReSaver Plus (its Skyrim.ccc lists the Creation Club plugins)
	void TestMo2RelativeGamePath()
	{
		const auto root = fs::temp_directory_path() / "rs_mo2_rel";
		fs::remove_all(root);
		fs::create_directories(root / "StockGame");
		fs::create_directories(root / "profiles" / "Main");
		{
			std::ofstream f(root / "ModOrganizer.ini");
			f << "[General]\ngameName=Skyrim Special Edition\ngamePath=@ByteArray(.\\\\StockGame)\nselected_profile=@ByteArray(Main)\n[Settings]\n";
		}
		const auto m = ReadMo2Ini(root / "ModOrganizer.ini");
		Check(m && m->gamePath.is_absolute() && m->gamePath.string().starts_with(root.string()) && m->gamePath.string().ends_with("StockGame"), "mo2: a relative gamePath (.\\StockGame) is read from Mod Organizer's folder");
		fs::remove_all(root);
	}

	// Starfield under Mod Organizer: its setup (gameName=Starfield), the profile's enabled mods, its plugins.txt (the game's
	// own files are always loaded), the plugins found in the mods' folders and the loose-file conflicts between two mods
	void TestMo2Starfield()
	{
		const auto root = fs::temp_directory_path() / "rs_mo2_sf";
		fs::remove_all(root);
		const auto game = root / "Starfield";
		fs::create_directories(game / "Data");
		fs::create_directories(root / "MO2" / "profiles" / "Default");
		fs::create_directories(root / "MO2" / "profiles" / "Other");
		fs::create_directories(root / "MO2" / "mods" / "ModA" / "textures");
		fs::create_directories(root / "MO2" / "mods" / "ModB" / "textures");
		fs::create_directories(root / "MO2" / "mods" / "Disabled");
		fs::create_directories(root / "MO2" / "overwrite");
		auto put = [](const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; };
		std::string tes4 = "TES4";
		tes4 += std::string("\0\0\0\0\0\0\0\0", 8);
		put(game / "Data" / "Starfield.esm", tes4);
		put(root / "MO2" / "mods" / "ModA" / "ModA.esm", tes4);
		put(root / "MO2" / "mods" / "Disabled" / "Gone.esm", tes4);
		put(root / "MO2" / "mods" / "ModA" / "textures" / "same.dds", "a");
		put(root / "MO2" / "mods" / "ModB" / "textures" / "same.dds", "b");
		{
			std::ofstream f(root / "MO2" / "ModOrganizer.ini");
			f << "[General]\ngameName=Starfield\ngamePath=@ByteArray(" << game.string() << ")\nselected_profile=@ByteArray(Default)\n[Settings]\n";
		}
		put(root / "MO2" / "profiles" / "Default" / "modlist.txt", "# This file was automatically generated by Mod Organizer.\n+ModB\n+ModA\n-Disabled\n");
		put(root / "MO2" / "profiles" / "Default" / "plugins.txt", "# This file was automatically generated by Mod Organizer.\n*ModA.esm\nGone.esm\n");
		const auto m = ReadMo2Ini(root / "MO2" / "ModOrganizer.ini");
		Check(m && m->gameName == "Starfield" && m->selectedProfile == "Default" && m->profiles.size() == 2, "mo2 starfield: its setup, game and profiles are read");
		if (!m) {
			return;
		}
		const auto mods = LoadModList(m->profilesDir / "Default");
		Check(mods.size() == 2 && mods[0] == "ModB" && mods[1] == "ModA", "mo2 starfield: the profile's enabled mods, highest priority first");
		const auto active = loadorder::Read(m->profilesDir / "Default" / "plugins.txt", m->gamePath, 3);
		Check(active.known && active.plugins.contains("moda.esm") && active.plugins.contains("starfield.esm") && !active.plugins.contains("gone.esm"),
			"mo2 starfield: the profile's plugins.txt plus the game's own files; a disabled plugin is not active");
		std::vector<fs::path> dirs{ game / "Data", m->overwriteDir };
		for (const auto& mod : mods) {
			dirs.push_back(m->modsDir / mod);
		}
		const auto listed = loadorder::StarfieldListed(m->profilesDir / "Default" / "plugins.txt", dirs);
		const bool hasA = std::any_of(listed.begin(), listed.end(), [](const std::string& x) { return x == "ModA.esm" || x == "moda.esm"; });
		Check(hasA, "mo2 starfield: a plugin inside a mod's folder is in the load order");
		assets::Input in;
		in.sf = true;
		in.modsDir = m->modsDir;
		in.overwrite = m->overwriteDir;
		in.modsLowFirst = { "ModA", "ModB" };
		in.gameData = game / "Data";
		in.loadOrder = listed;
		const auto r = assets::Find(in);
		const bool pair = std::any_of(r.pairs.begin(), r.pairs.end(), [](const assets::Pair& p) { return p.winner == "ModB" && p.loser == "ModA"; });
		Check(pair, "mo2 starfield: the higher mod's loose file overrides the lower one's");
		fs::remove_all(root);
	}
	// Vortex keeps every installed mod's files in its staging folder: when two mods ship the same plugin (a mod
	// and an ESL-flagged replacement of it), the load order check must read the copy Vortex deployed.
	void TestVortexPluginWinner()
	{
		const auto root = fs::temp_directory_path() / "rs_vortex_winner";
		fs::remove_all(root);
		const auto staging = root / "staging", data = root / "Data";
		fs::create_directories(staging / "Original Mod");
		fs::create_directories(staging / "ESL Replacement");
		fs::create_directories(data);
		auto save = [&](const fs::path& p, const std::vector<std::uint8_t>& bytes) {
			std::ofstream f(p, std::ios::binary);
			f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		};
		using merge::WritePlugin;
		save(staging / "Original Mod" / "Foo.esp", WritePlugin({ "Skyrim.esm" }, 0, { Misc(0x01012345) }, "t", {}, 1.71f, 44, true));
		save(staging / "ESL Replacement" / "Foo.esp", WritePlugin({ "Skyrim.esm" }, 0x200, { Misc(0x01000801) }, "t", {}, 1.71f, 44, true));
		save(data / "Foo.esp", WritePlugin({ "Skyrim.esm" }, 0x200, { Misc(0x01000801) }, "t", {}, 1.71f, 44, true));
		VortexDeployment v;
		v.stagingPath = staging;
		v.dataPath = data;
		v.mods = { "Original Mod", "ESL Replacement" };  // the order the manifest lists them in, not the winner's
		v.winner["foo.esp"] = "ESL Replacement";
		ScriptSources src;
		src.scanVortex(v, nullptr);
		const auto p = src.pluginPath("Foo.esp");
		Check(p && p->parent_path().filename() == "ESL Replacement", "vortex: the deployed copy of a plugin two mods ship is the one read");
		locheck::Options o;
		o.game = locheck::Game::kSkyrimSE;
		o.loadOrder = { "Foo.esp" };
		o.findPlugin = [&](const std::string& n) { return src.pluginPath(n); };
		const auto r = locheck::Check(o);
		Check(r.light == 1 && r.full == 0 && !Has(r, locheck::Severity::kError, "Foo.esp", "above 0xFFF"),
			"vortex: the ESL-flagged replacement counts as light, the original's form IDs are not reported");
		fs::remove_all(root);
	}

	void TestLoadOrderSynthetic()
	{
		const auto dir = fs::temp_directory_path() / "rs_locheck_test";
		fs::remove_all(dir);
		fs::create_directories(dir);
		auto save = [&](const std::string& name, const std::vector<std::uint8_t>& bytes) {
			std::ofstream f(dir / name, std::ios::binary);
			f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		};
		using merge::WritePlugin;
		save("Fallout4.esm", WritePlugin({}, 1, { Misc(0x000801) }, "t", {}, 1.0f, 131, true));
		// light, ids fine (0x001 allowed: version 1.0 with a master)
		save("Good.esp", WritePlugin({ "Fallout4.esm" }, 0x200, { Misc(0x01000001), Misc(0x01000FFF), Misc(0x00000801) }, "t", {}, 1.0f, 131, true));
		// light by flag, two new forms over 0xFFF
		save("Over.esp", WritePlugin({ "Fallout4.esm" }, 0x200, { Misc(0x01000800), Misc(0x01001000), Misc(0x01012345) }, "t", {}, 1.0f, 131, true));
		// light by extension (no flag), no master: below 0x800 is the xEdit/CK floor
		save("Ext.esl", WritePlugin({}, 0, { Misc(0x00000010) }, "t", {}, 1.0f, 131, true));
		// needs Late.esp, which loads after it
		save("Early.esp", WritePlugin({ "Fallout4.esm", "Late.esp" }, 0, { Misc(0x02000801) }, "t", {}, 1.0f, 131, true));
		save("Late.esp", WritePlugin({ "Fallout4.esm" }, 0, { Misc(0x01000801) }, "t", {}, 1.0f, 131, true));
		// needs a master that is not in the load order
		save("Orphan.esp", WritePlugin({ "Fallout4.esm", "Gone.esm" }, 0, {}, "t", {}, 1.0f, 131, true));
		// header version from a newer game
		save("Newer.esp", WritePlugin({ "Fallout4.esm" }, 0, {}, "t", {}, 1.1f, 131, true));
		// a .esp flagged light but as an ESM too
		save("Both.esm", WritePlugin({ "Fallout4.esm" }, 0x201, { Misc(0x01000900) }, "t", {}, 1.0f, 131, true));

		locheck::Options o;
		o.game = locheck::Game::kFallout4;
		o.loadOrder = { "Fallout4.esm", "Good.esp", "Over.esp", "Ext.esl", "Early.esp", "Late.esp", "Orphan.esp", "Newer.esp", "Both.esm", "NotThere.esp" };
		o.findPlugin = [&](const std::string& n) -> std::optional<fs::path> {
			const auto p = dir / n;
			return fs::exists(p) ? std::optional<fs::path>(p) : std::nullopt;
		};
		const auto r = locheck::Check(o);
		for (const auto& l : locheck::Describe(r)) {
			std::printf("  %s\n", l.c_str());
		}
		Check(r.light == 4 && r.full == 5 && r.missing == 1, "locheck: Good/Over (flag), Ext.esl (extension), Both.esm (flag) light; 5 full, the missing one takes no slot");
		Check(!Has(r, locheck::Severity::kError, "Good.esp", "above 0xFFF") && !Has(r, locheck::Severity::kWarning, "Good.esp", "below 0x800"),
			"locheck: 0x001..0xFFF fine in a light plugin with header 1.0 and a master");
		Check(Has(r, locheck::Severity::kError, "Over.esp", "numbers 2 of its 3 new forms above 0xFFF"), "locheck: light plugin over 0xFFF");
		Check(Has(r, locheck::Severity::kWarning, "Ext.esl", "below 0x800"), "locheck: no master: below 0x800 warned");
		Check(Has(r, locheck::Severity::kError, "Early.esp", "loads after it"), "locheck: master after its dependant");
		Check(Has(r, locheck::Severity::kError, "Orphan.esp", "Gone.esm, which is not in the load order"), "locheck: missing master");
		Check(Has(r, locheck::Severity::kError, "Newer.esp", "header version 1.10"), "locheck: header version above 1.0");
		Check(Has(r, locheck::Severity::kWarning, "NotThere.esp", "not found"), "locheck: plugin not found");
		{
			// the master tree
			const auto g = locheck::BuildMasterGraph(r);
			auto idx = [&](const char* n) {
				for (std::size_t i = 0; i < r.plugins.size(); ++i) {
					if (r.plugins[i].name == n) return i;
				}
				return SIZE_MAX;
			};
			const auto base = idx("Fallout4.esm"), early = idx("Early.esp"), late = idx("Late.esp"), orphan = idx("Orphan.esp");
			Check(g.dependents[base].size() == 7, "master tree: 7 plugins need Fallout4.esm directly");
			bool lateMarked = false;
			for (const auto& l : g.masters[early]) {
				lateMarked = lateMarked || (l.name == "Late.esp" && l.late && l.index == static_cast<int>(late));
			}
			Check(lateMarked, "master tree: Early.esp's master Late.esp marked as loading late");
			Check(g.absent.size() == 1 && g.absent[0].first == "Gone.esm" && g.absent[0].second == std::vector<std::size_t>{ orphan },
				"master tree: Gone.esm not in the load order, needed by Orphan.esp");
			const auto all = locheck::AllDependents(g, late);
			Check(all == std::vector<std::size_t>{ early }, "master tree: removing Late.esp breaks Early.esp only");
			Check(locheck::AllDependents(g, base).size() == 7, "master tree: everything but the base needs Fallout4.esm (Early also through Late)");
			Check(locheck::AllDependentsOfAbsent(g, 0) == std::vector<std::size_t>{ orphan }, "master tree: Gone.esm's dependants");
		}
		{
			// a patch of a patch, and a cycle (A needs B, B needs A) does not loop
			locheck::Result t;
			t.game = locheck::Game::kSkyrimSE;
			auto add = [&](const char* n, std::vector<std::string> m) {
				locheck::PluginInfo p;
				p.name = n;
				p.path = n;
				p.masters = std::move(m);
				p.loadPos = static_cast<int>(t.plugins.size());
				t.plugins.push_back(p);
			};
			add("Skyrim.esm", {});
			add("Unofficial Skyrim Special Edition Patch.esp", { "Skyrim.esm" });
			add("ModA.esp", { "Skyrim.esm", "Unofficial Skyrim Special Edition Patch.esp" });
			add("ModA - ModB Patch.esp", { "ModA.esp", "ModB.esp" });
			add("ModB.esp", { "Skyrim.esm" });
			add("Patch of patch.esp", { "ModA - ModB Patch.esp" });
			add("X.esp", { "Y.esp" });
			add("Y.esp", { "X.esp" });
			const auto g = locheck::BuildMasterGraph(t);
			Check(locheck::AllDependents(g, 1) == std::vector<std::size_t>{ 2, 3, 5 }, "master tree: removing USSEP breaks ModA, its patch and the patch of the patch");
			Check(locheck::AllDependents(g, 4) == std::vector<std::size_t>{ 3, 5 }, "master tree: removing ModB breaks the patch and the patch of the patch");
			bool bLate = false;
			for (const auto& l : g.masters[3]) {
				bLate = bLate || (l.name == "ModB.esp" && l.late);
			}
			Check(bLate, "master tree: ModB.esp loads after the patch that needs it");
			Check(locheck::AllDependents(g, 6) == std::vector<std::size_t>{ 7 } && locheck::AllDependents(g, 7) == std::vector<std::size_t>{ 6 },
				"master tree: a cycle ends");
		}
		Check(!Has(r, locheck::Severity::kError, "", "full plugins"), "locheck: within the full plugin limit");
		const auto* both = [&]() -> const locheck::PluginInfo* {
			for (const auto& p : r.plugins) {
				if (p.name == "Both.esm") return &p;
			}
			return nullptr;
		}();
		// light indexes follow the order the game loads: the masters Ext.esl (extension) and Both.esm first, then Good, Over
		Check(both && both->light && both->esm && both->slot == 1, "locheck: ESL-flagged ESM is light, light index 1 (masters load first)");

		// 254 full plugins fit, 255 do not
		locheck::Options big;
		big.game = locheck::Game::kFallout4;
		for (int i = 0; i < 254; ++i) {
			big.loadOrder.push_back("Mod" + std::to_string(i) + ".esp");
		}
		// every name reads the same small plugin file (a missing file takes no slot: the game skips it)
		save("StubFull.esp", WritePlugin({}, 0, {}, "t", {}, 1.0f, 131, true));
		save("StubLight.esl", WritePlugin({}, 0, {}, "t", {}, 1.0f, 131, true));
		big.findPlugin = [&](const std::string& n) -> std::optional<fs::path> {
			return dir / (n.ends_with(".esl") ? "StubLight.esl" : "StubFull.esp");
		};
		Check(!locheck::Check(big).hasErrors(), "locheck: 254 full plugins are within the limit");
		big.loadOrder.push_back("Mod254.esp");
		const auto r255 = locheck::Check(big);
		Check(Has(r255, locheck::Severity::kError, "", "at most 254 full plugins") && Has(r255, locheck::Severity::kError, "", "Mod254.esp"),
			"locheck: 255 full plugins: error naming the one beyond");
		big.loadOrder.resize(254);
		for (int i = 0; i < 4096; ++i) {
			big.loadOrder.push_back("L" + std::to_string(i) + ".esl");
		}
		Check(!locheck::Check(big).hasErrors(), "locheck: 254 full + 4096 light plugins are within the limits");
		big.loadOrder.push_back("L4096.esl");
		const auto rl = locheck::Check(big);
		Check(rl.light == 4097 && Has(rl, locheck::Severity::kError, "", "at most 4096 light plugins"), "locheck: 4097 light plugins: error");

		// Skyrim LE: light plugins and SE-format plugins are warned about
		save("Skyrim.esm", WritePlugin({}, 1, {}, "t", {}, 0.94f, 43));
		save("SeMod.esp", WritePlugin({ "Skyrim.esm" }, 0, {}, "t", {}, 1.7f, 44));
		locheck::Options le;
		le.game = locheck::Game::kSkyrimLE;
		le.loadOrder = { "Skyrim.esm", "SeMod.esp", "Ext.esl" };
		le.findPlugin = o.findPlugin;
		const auto rle = locheck::Check(le);
		Check(rle.light == 0 && Has(rle, locheck::Severity::kWarning, "SeMod.esp", "Special Edition's format") &&
				  Has(rle, locheck::Severity::kWarning, "Ext.esl", "no light plugins"),
			"locheck: Legendary Edition");
		fs::remove_all(dir);
	}

	std::vector<std::string> Names(const std::vector<locheck::OrderEntry>& a_e, const std::vector<std::size_t>& a_order)
	{
		std::vector<std::string> v;
		for (const auto i : a_order) {
			v.push_back(a_e[i].name);
		}
		return v;
	}

	// the order the game loads (SkyrimSE.exe 0x1ba4d0 + 0x1b8f50), late masters and Fix the order
	void TestFixLoadOrder()
	{
		using E = locheck::OrderEntry;
		using V = std::vector<std::string>;
		// masters before plain plugins, each group in the listed order
		{
			const std::vector<E> e = { { "Skyrim.esm", true, {} }, { "A.esp", false, { "Skyrim.esm" } }, { "M.esm", true, { "Skyrim.esm" } },
				{ "B.esp", false, { "M.esm" } } };
			Check(Names(e, locheck::GameOrder(e)) == V{ "Skyrim.esm", "M.esm", "A.esp", "B.esp" }, "order: masters load before plain plugins");
			Check(locheck::LateMasters(e).empty(), "order: an ESM-flagged master listed after its plugin is not late");
		}
		// a master's master further down is moved just before it, also a plain plugin, and the moved one's own masters
		{
			const std::vector<E> e = { { "Skyrim.esm", true, {} }, { "X.esm", true, { "Skyrim.esm", "P.esp" } }, { "Q.esm", true, {} },
				{ "P.esp", false, { "Skyrim.esm" } }, { "R.esp", false, {} } };
			Check(Names(e, locheck::GameOrder(e)) == V{ "Skyrim.esm", "P.esp", "X.esm", "Q.esm", "R.esp" }, "order: a master's plain master is hoisted before it");
			const std::vector<E> e2 = { { "X.esm", true, { "Y.esm", "Z.esm" } }, { "Y.esm", true, { "W.esm" } }, { "Z.esm", true, {} }, { "W.esm", true, {} } };
			// X: Y moved before X, then Z before Y; the walk stays on Z (nothing), then Y: W before Y
			Check(Names(e2, locheck::GameOrder(e2)) == V{ "Z.esm", "W.esm", "Y.esm", "X.esm" }, "order: hoisting as the game walks it");
			Check(locheck::LateMasters(e2).empty(), "order: hoisted masters are not late");
		}
		// plain plugin needs a plain plugin listed after it: late, and the fix moves it (and its own late master) up
		{
			const std::vector<E> e = { { "Skyrim.esm", true, {} }, { "A.esp", false, {} }, { "Ward.esp", false, { "Skyrim.esm", "Myst.esp" } },
				{ "Tweak.esp", false, { "Ward.esp" } }, { "B.esp", false, {} }, { "Myst.esp", false, { "Skyrim.esm", "Lib.esp" } }, { "C.esp", false, {} },
				{ "Lib.esp", false, {} }, { "Myst2.esp", false, { "Myst.esp" } } };
			const auto late = locheck::LateMasters(e);
			Check(late.size() == 2, "fix: Ward needs Myst, Myst needs Lib: two late masters");
			const auto plan = locheck::PlanFix(e);
			Check(plan.ok && plan.moves.size() == 2, "fix: two moves");
			Check(plan.order == V{ "Skyrim.esm", "A.esp", "Lib.esp", "Myst.esp", "Ward.esp", "Tweak.esp", "B.esp", "C.esp", "Myst2.esp" },
				"fix: Myst just before Ward, then Lib just before Myst; the rest keeps its order");
			Check(plan.moves.size() == 2 && locheck::MoveText(plan.moves[0]) == "Myst.esp: #6 -> before Ward.esp #3", "fix: move text");
			std::vector<E> after;
			for (const auto& n : plan.order) {
				for (const auto& x : e) {
					if (x.name == n) after.push_back(x);
				}
			}
			Check(locheck::LateMasters(after).empty(), "fix: nothing late afterwards");
		}
		// a master needed by two plugins goes before the first of them
		{
			const std::vector<E> e = { { "P1.esp", false, { "M.esp" } }, { "X.esp", false, {} }, { "P2.esp", false, { "M.esp" } }, { "M.esp", false, {} } };
			const auto plan = locheck::PlanFix(e);
			Check(plan.ok && plan.order == V{ "M.esp", "P1.esp", "X.esp", "P2.esp" } && plan.moves.size() == 1, "fix: before the first plugin that needs it");
		}
		// a cycle is refused
		{
			const std::vector<E> e = { { "A.esp", false, { "B.esp" } }, { "B.esp", false, { "C.esp" } }, { "C.esp", false, { "A.esp" } } };
			const auto plan = locheck::PlanFix(e);
			Check(!plan.ok && plan.moves.empty() && plan.error.find("need each other") != std::string::npos &&
					  plan.error.find("A.esp needs B.esp needs C.esp needs A.esp") != std::string::npos,
				"fix: a cycle of masters is refused, naming it");
		}
		// a master's master the game hoists itself, whose own plain master is listed before it: no listed order helps
		{
			const std::vector<E> e = { { "N.esp", false, {} }, { "X.esm", true, { "H.esp" } }, { "H.esp", false, { "N.esp" } } };
			// game order: H (hoisted before X), X, N: H loads before its master N whatever the list says
			Check(locheck::LateMasters(e).size() == 1, "fix: hoisted plain master with a plain master is late");
			const auto plan = locheck::PlanFix(e);
			Check(!plan.ok && plan.error.find("wherever it is listed") != std::string::npos, "fix: refused when no listed order helps");
		}
		// nothing late: an empty plan that is ok
		{
			const std::vector<E> e = { { "A.esp", false, {} }, { "B.esp", false, { "A.esp" } } };
			const auto plan = locheck::PlanFix(e);
			Check(plan.ok && plan.moves.empty() && plan.order == V{ "A.esp", "B.esp" }, "fix: nothing to do");
		}

		// the files: every other line, the '*' marks, comments, CRLF / LF, a byte order mark and a missing last newline stay
		{
			locheck::Move mv;
			mv.plugin = "MysticismMagic.esp";
			mv.before = "360 Ward.esp";
			const std::string mo2 = "# This file was automatically generated by Mod Organizer.\r\n*SkyUI_SE.esp\r\nDisabled.esp\r\n*360 Ward.esp\r\n"
			                        "*360WardTweaksSOT.esp\r\n*mysticismmagic.esp\r\n*Last.esp\r\n";
			const auto out = lofix::ApplyMoves(mo2, { mv });
			Check(out == "# This file was automatically generated by Mod Organizer.\r\n*SkyUI_SE.esp\r\nDisabled.esp\r\n*mysticismmagic.esp\r\n"
			             "*360 Ward.esp\r\n*360WardTweaksSOT.esp\r\n*Last.esp\r\n",
				"files: MO2 plugins.txt (CRLF, '*', a disabled line, name case kept)");
			const std::string game = "\xEF\xBB\xBF# This file is used by Skyrim to keep track of your downloaded content.\n# Please do not modify this file.\n"
			                         "*360 Ward.esp\nOther.esp\n*MysticismMagic.esp";
			Check(lofix::ApplyMoves(game, { mv }) == "\xEF\xBB\xBF# This file is used by Skyrim to keep track of your downloaded content.\n# Please do not "
			                                          "modify this file.\n*MysticismMagic.esp\n*360 Ward.esp\nOther.esp",
				"files: game plugins.txt (LF, BOM, no newline at the end: the new last line has none either)");
			const std::string le = "360 Ward.esp\r\nMysticismMagic.esp\r\n";
			Check(lofix::ApplyMoves(le, { mv }) == "MysticismMagic.esp\r\n360 Ward.esp\r\n", "files: Legendary Edition plugins.txt (no marks)");
			std::vector<std::string> skipped;
			Check(lofix::ApplyMoves("*A.esp\n", { mv }, &skipped) == "*A.esp\n" && skipped.size() == 1, "files: a move not in the file is skipped");
			Check(lofix::ApplyMoves("MysticismMagic.esp\n360 Ward.esp\n", { mv }) == "MysticismMagic.esp\n360 Ward.esp\n", "files: already in order");
		}
		// MO2 lockedorder.txt: a locked plugin whose load index changes refuses the fix
		{
			const V oldO = { "Skyrim.esm", "W.esp", "T.esp", "M.esp" }, newO = { "Skyrim.esm", "M.esp", "W.esp", "T.esp" };
			Check(lofix::LockedMoved("# This file was automatically generated by Mod Organizer.\r\n", oldO, newO).empty(), "locked: header only");
			const auto l = lofix::LockedMoved("# x\r\nT.esp|2\r\nSkyrim.esm|0\r\nbad line\r\n", oldO, newO);
			Check(l.size() == 1 && l[0].plugin == "T.esp" && l[0].index == 2 && l[0].newIndex == 3, "locked: T.esp locked at 2 would move to 3");
		}
		// writing: backups next to the files, the change read back, a dry run writes nothing, locked plugins refuse
		{
			const auto dir = fs::temp_directory_path() / "rs_lofix_test";
			fs::remove_all(dir);
			fs::create_directories(dir);
			auto put = [&](const char* n, const std::string& t) {
				std::ofstream f(dir / n, std::ios::binary);
				f << t;
			};
			auto get = [&](const fs::path& p) {
				std::ifstream f(p, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(f), {});
			};
			const std::string pl = "# MO2\r\n*W.esp\r\n*T.esp\r\nOff.esp\r\n*M.esp\r\n", lo = "# MO2\r\nSkyrim.esm\r\nW.esp\r\nT.esp\r\nOff.esp\r\nM.esp\r\n";
			put("plugins.txt", pl);
			put("loadorder.txt", lo);
			put("lockedorder.txt", "# MO2\r\n");
			const std::vector<E> e = { { "Skyrim.esm", true, {} }, { "W.esp", false, { "M.esp" } }, { "T.esp", false, {} }, { "M.esp", false, {} } };
			const auto plan = locheck::PlanFix(e);
			const auto t = lofix::TargetsFor(dir / "plugins.txt", lofix::Manager::kMo2);
			Check(t.files.size() == 2 && t.files[1].filename() == "loadorder.txt" && t.lockedOrder.filename() == "lockedorder.txt", "write: MO2 targets");
			const V oldO = { "Skyrim.esm", "W.esp", "T.esp", "M.esp" };
			const auto dry = lofix::Write(t, plan, oldO, true);
			Check(dry.ok && get(dir / "plugins.txt") == pl && dry.files.size() == 2 && dry.files[0].changed && dry.files[0].backup.empty(),
				"write: dry run shows the change, writes nothing");
			const auto w = lofix::Write(t, plan, oldO, false);
			Check(w.ok && get(dir / "plugins.txt") == "# MO2\r\n*M.esp\r\n*W.esp\r\n*T.esp\r\nOff.esp\r\n" &&
					  get(dir / "loadorder.txt") == "# MO2\r\nSkyrim.esm\r\nM.esp\r\nW.esp\r\nT.esp\r\nOff.esp\r\n",
				"write: both files changed");
			Check(w.files.size() == 2 && fs::exists(w.files[0].backup) && get(w.files[0].backup) == pl && get(w.files[1].backup) == lo &&
					  w.files[0].backup.parent_path() == dir && w.files[0].backup.filename().string().ends_with(".resaverpp.bak") &&
					  w.message.find(w.files[0].backup.filename().string()) != std::string::npos,
				"write: originals kept next to them, named in the message");
			Check(lofix::Diff(pl, get(dir / "plugins.txt"), "plugins.txt").find("+*M.esp") != std::string::npos, "write: diff");
			put("plugins.txt", pl);
			put("loadorder.txt", lo);
			put("lockedorder.txt", "# MO2\r\nT.esp|2\r\n");
			const auto locked = lofix::Write(t, plan, oldO, false);
			Check(!locked.ok && locked.message.find("T.esp") != std::string::npos && get(dir / "plugins.txt") == pl, "write: a locked plugin refuses, nothing written");
			fs::remove_all(dir);
		}
		// the check: an ESM-flagged master after its plugin is not reported, a plain one is (with lateMaster set)
		{
			const auto dir = fs::temp_directory_path() / "rs_lofix_check";
			fs::remove_all(dir);
			fs::create_directories(dir);
			auto save = [&](const std::string& name, const std::vector<std::uint8_t>& bytes) {
				std::ofstream f(dir / name, std::ios::binary);
				f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			};
			using merge::WritePlugin;
			save("Skyrim.esm", WritePlugin({}, 1, {}, "t", {}, 1.71f, 44, true));
			save("UsesFlagged.esp", WritePlugin({ "Skyrim.esm", "Flagged.esp" }, 0, {}, "t", {}, 1.71f, 44, true));
			save("UsesPlain.esp", WritePlugin({ "Skyrim.esm", "Plain.esp" }, 0x200, {}, "t", {}, 1.71f, 44, true));
			save("Flagged.esp", WritePlugin({ "Skyrim.esm" }, 1, {}, "t", {}, 1.71f, 44, true));
			save("Plain.esp", WritePlugin({ "Skyrim.esm" }, 0x200, {}, "t", {}, 1.71f, 44, true));
			locheck::Options o;
			o.game = locheck::Game::kSkyrimSE;
			o.loadOrder = { "Skyrim.esm", "UsesFlagged.esp", "UsesPlain.esp", "Flagged.esp", "Plain.esp" };
			o.findPlugin = [&](const std::string& n) -> std::optional<fs::path> { return dir / n; };
			const auto r = locheck::Check(o);
			std::size_t late = 0;
			for (const auto& f : r.findings) {
				late += f.lateMaster.empty() ? 0 : 1;
			}
			Check(late == 1 && Has(r, locheck::Severity::kError, "UsesPlain.esp", "Plain.esp loads after it") &&
					  !Has(r, locheck::Severity::kError, "UsesFlagged.esp", "loads after it") &&
					  Has(r, locheck::Severity::kInfo, "UsesFlagged.esp", "masters load before plain plugins"),
				"check: ESM-flagged master listed late is fine (info), a plain one is an error");
			const auto* fl = &r.plugins[3];
			Check(fl->name == "Flagged.esp" && fl->slot == 1 && fl->loadPos == 1 && r.plugins[1].slot == 2, "check: slots in the order the game loads");
			const auto plan = locheck::PlanFix(r);
			Check(plan.ok && plan.moves.size() == 1 && locheck::MoveText(plan.moves[0]) == "Plain.esp: #FE 001 -> before UsesPlain.esp #FE 000",
				"check: plan from the result, labelled with the check's slots");
			{
				std::ofstream pt(dir / "plugins.txt", std::ios::binary);
				pt << "# This file is used by Skyrim to keep track of your downloaded content.\n*UsesFlagged.esp\nOff.esp\n*UsesPlain.esp\n*Flagged.esp\n*Plain.esp\n";
			}
			const auto lines = lofix::EnabledLines(dir / "plugins.txt");
			Check(lines == std::vector<std::string>{ "UsesFlagged.esp", "UsesPlain.esp", "Flagged.esp", "Plain.esp" }, "check: enabled lines of plugins.txt");
			const auto plan2 = locheck::PlanFix(r, lines);
			Check(plan2.ok && plan2.moves.size() == 1 && locheck::MoveText(plan2.moves[0]) == "Plain.esp: #3 -> before UsesPlain.esp #1",
				"check: moves numbered by their place among plugins.txt's enabled lines (from 0)");
			// Legendary Edition: the extension does not make a master (TESV.exe IsMaster = header flag)
			save("Ext.esm", WritePlugin({}, 0, {}, "t", {}, 0.94f, 43, true));
			Check(!locheck::ReadHeader(dir / "Ext.esm", false, locheck::Game::kSkyrimLE).esm && locheck::ReadHeader(dir / "Ext.esm", false, locheck::Game::kSkyrimSE).esm,
				"check: .esm without the flag is a master in SE, not in LE");
			fs::remove_all(dir);
		}
	}

	void TestLoadOrderRealFO4()
	{
		const char* env = std::getenv("RESAVERPP_FO4_DATA");
		if (!env || !*env) {
			std::printf("(load order: RESAVERPP_FO4_DATA not set, real plugins skipped)\n");
			return;
		}
		const fs::path   data = env;
		locheck::Options o;
		o.game = locheck::Game::kFallout4;
		for (const char* p : { "Fallout4.esm", "DLCRobot.esm", "DLCworkshop01.esm", "DLCCoast.esm", "DLCworkshop02.esm", "DLCworkshop03.esm",
				 "DLCNukaWorld.esm", "ccBGSFO4115-X02.esl" }) {
			if (fs::exists(data / p)) {
				o.loadOrder.push_back(p);
			}
		}
		o.findPlugin = [&](const std::string& n) -> std::optional<fs::path> {
			const auto p = data / n;
			return fs::exists(p) ? std::optional<fs::path>(p) : std::nullopt;
		};
		const auto t0 = std::chrono::steady_clock::now();
		const auto r = locheck::Check(o);
		std::printf("real Fallout 4 Data (%.1f s):\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
		for (const auto& l : locheck::Describe(r)) {
			std::printf("  %s\n", l.c_str());
		}
		Check(!r.hasErrors(), "locheck: the game's own plugins have no errors");
		for (const auto& p : r.plugins) {
			if (p.name == "Fallout4.esm") {
				Check(p.esm && !p.light && p.hedr > 0.99f && p.hedr < 1.01f, "locheck: Fallout4.esm master, HEDR 1.0");
			}
			if (p.name == "ccBGSFO4115-X02.esl") {
				Check(p.light && p.newRecords > 0 && p.overRange == 0, "locheck: Creation Club .esl is light and in range");
			}
		}
	}

	void CheckReport(const SaveFile& s, const bloat::Report& r, const std::string& name)
	{
		const auto* p = s.papyrus();
		std::size_t inst = 0, thr = 0, arr = 0, st = 0;
		for (const auto& m : r.mods) {
			inst += m.instances.count;
			thr += m.threads.count;
			arr += m.arrays.count;
			st += m.structs.count;
		}
		Check(p && inst == p->instances.size() && thr == p->activeScripts.size() && arr == p->arrays.size() && st == p->structInstances.size(),
			name + ": every instance, thread, array and struct is in one row");
		Check(r.attributed + r.papyrusOther == r.papyrusBytes + r.changeFormBytes,
			name + ": rows + shared Papyrus data = Papyrus block + change forms (" + std::to_string(r.attributed + r.papyrusOther) + " vs " +
				std::to_string(r.papyrusBytes + r.changeFormBytes) + ")");
		Check(r.papyrusBytes > 0 && r.papyrusBytes < r.bodyBytes && r.changeFormBytes < r.bodyBytes, name + ": sizes within the body");
		Check(!r.causes.empty(), name + ": causes listed");
	}

	void TestBloat()
	{
		auto fo4 = Files("RESAVERPP_FO4_SAVES", ".fos");
		auto sk = Files("RESAVERPP_SKYRIM_SAVES", ".ess");
		if (fo4.empty() && sk.empty()) {
			std::printf("(bloat: RESAVERPP_FO4_SAVES / RESAVERPP_SKYRIM_SAVES not set, real saves skipped)\n");
			return;
		}
		std::vector<fs::path> all = fo4;
		all.insert(all.end(), sk.begin(), sk.end());
		std::unique_ptr<SaveFile> prev;
		bloat::Report             prevRep;
		fs::path                  prevPath;
		for (const auto& f : all) {
			std::unique_ptr<SaveFile> s;
			try {
				s = SaveFile::load(f);
			} catch (const std::exception& e) {
				// a save the reader refuses (a cut-off file) is not this test's business
				std::printf("(bloat: %s skipped: %s)\n", f.filename().string().c_str(), e.what());
				continue;
			}
			const auto t0 = std::chrono::steady_clock::now();
			const auto r = bloat::Build(*s);
			const auto secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			std::printf("== %s (%s, %.1f s): %zu rows, created base unknown %zu, decode failures %zu\n", f.filename().string().c_str(), s->gameTitle(), secs,
				r.mods.size(), r.createdUnknown, r.decodeFailures);
			const auto lines = bloat::Describe(r, 8);
			for (std::size_t i = 0; i < lines.size() && i < 24; ++i) {
				std::printf("  %s\n", lines[i].c_str());
			}
			CheckReport(*s, r, f.filename().string());
			if (prev && prev->isFO4() == s->isFO4() && prev->header.playerName == s->header.playerName && prevPath.parent_path() == f.parent_path()) {
				const auto g = bloat::Compare(prevRep, r);
				std::printf("  growth since %s:\n", prevPath.filename().string().c_str());
				for (const auto& l : bloat::DescribeGrowth(g, 5)) {
					std::printf("  %s\n", l.c_str());
				}
				std::int64_t sum = 0;
				for (const auto& x : g) {
					sum += x.delta();
				}
				Check(sum == static_cast<std::int64_t>(r.attributed) - static_cast<std::int64_t>(prevRep.attributed),
					f.filename().string() + ": growth rows add up to the change in attributed bytes");
			}
			prevRep = r;
			prevPath = f;
			prev = std::move(s);
		}
	}
}

int main()
{
	TestLoadOrderSynthetic();
	TestFixLoadOrder();
	TestVortexPluginWinner();
	TestMo2RelativeGamePath();
	TestMo2Starfield();
	TestLoadOrderRealFO4();
	TestBloat();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
