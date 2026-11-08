// Save bloat by mod and the load order check.
//  - load order check: synthetic plugins written with MergedPatch::WritePlugin (light ranges, masters, header versions,
//    the 254 full plugin limit), then the real Fallout 4 Data folder in RESAVERPP_FO4_DATA (if set);
//  - save bloat: every .fos under RESAVERPP_FO4_SAVES and every .ess under RESAVERPP_SKYRIM_SAVES (folders separated
//    by ';'): every byte of the Papyrus block and of the change forms is either in a row or counted as shared Papyrus
//    data, every instance/thread/array/struct is in exactly one row; growth between consecutive saves of a folder.
#include "core/LoadOrderCheck.h"
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
		Check(!Has(r, locheck::Severity::kError, "", "full plugins"), "locheck: within the full plugin limit");
		const auto* both = [&]() -> const locheck::PluginInfo* {
			for (const auto& p : r.plugins) {
				if (p.name == "Both.esm") return &p;
			}
			return nullptr;
		}();
		Check(both && both->light && both->esm && both->slot == 3, "locheck: ESL-flagged ESM is light, light index 3");

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
	TestVortexPluginWinner();
	TestMo2RelativeGamePath();
	TestLoadOrderRealFO4();
	TestBloat();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
