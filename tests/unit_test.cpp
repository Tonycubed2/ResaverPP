#include "core/SkyrimLogs.h"
#include "core/AiGame.h"
#include "core/Json.h"
// ReSaver++ unit and regression tests that need no save files: each builds the data it needs in memory.
// Run by CTest (rs_unit). Exit code 0 = all passed.
#include "core/Bsa.h"
#include "core/CrashLog.h"
#include "core/ChangeFormFO4.h"
#include "core/ChangeFormFO4In.h"
#include "core/ScriptSources.h"
#include "core/MergedPatch.h"
#include "core/ModRemove.h"
#include "core/Ops.h"
#include "core/ScriptFlood.h"
#include "core/GhostActors.h"
#include "../helper/src/Protocol.h"  // the helper's id list parser (GHOSTREMOVE, F1 page)
#include "core/Pex.h"
#include "core/Save.h"
#include "core/SaveArchive.h"
#include "core/LoadOrder.h"
#include "core/PluginEdit.h"
#include "core/PluginDB.h"
#include "core/Quests.h"
#include "core/ChangeFormData.h"
#include "core/FollowersSky.h"
#include "core/Transplant.h"
#include "core/TransplantFO4.h"
#include "core/WorkshopFO4.h"
#include "core/WorldItemsFO4.h"
#include "core/CellResetFO4.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include "core/CharEffects.h"
#include "core/SaveBloat.h"
#include "core/CompanionsFO4.h"
#include "core/Compare.h"
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <sstream>
#include <unordered_set>

using namespace ess;

namespace
{
	int g_fail = 0, g_pass = 0;

	void Check(bool a_ok, const char* a_what)
	{
		if (a_ok) {
			++g_pass;
		} else {
			++g_fail;
			std::printf("FAIL: %s\n", a_what);
		}
	}

	Variable Ref(EID a_id, VType a_type = VType::kRef)
	{
		Variable v;
		v.type = a_type;
		v.value = a_id;
		return v;
	}

	// A save with plugins Skyrim.esm, ModA.esp, ModB.esp and an empty Papyrus section.
	std::unique_ptr<SaveFile> MakeSave()
	{
		auto s = std::make_unique<SaveFile>();
		s->plugins.full = { "Skyrim.esm", "ModA.esp", "ModB.esp" };
		s->formIDs = { 0x01000800, 0x02000800 };  // RefID form index 1 -> ModA, 2 -> ModB
		GlobalData g;
		g.type = 1001;
		g.papyrus = std::make_unique<Papyrus>();
		s->table3.push_back(std::move(g));
		auto& p = *s->papyrus();
		p.strings = { "", "ScriptA", "ScriptB", "Quest", "OnUpdate", "x" };
		p.saveFileVersion = 4;  // always present in game saves
		for (TStr n : { 1u, 2u }) {
			Script sc;
			sc.name = n;
			sc.parent = 3;
			p.scripts.push_back(sc);
		}
		return s;
	}

	ScriptInstance Instance(EID a_id, TStr a_script, std::uint32_t a_refData)
	{
		ScriptInstance i;
		i.id = a_id;
		i.script = a_script;
		i.refID.data = a_refData;
		i.data = InstanceData{};
		return i;
	}

	ActiveScript Thread(ThreadID a_id, TStr a_script, const Variable& a_owner, std::vector<Variable> a_vars = {})
	{
		ActiveScript a;
		a.id = a_id;
		a.data = ActiveScriptData{};
		StackFrame f;
		f.scriptName = a_script;
		f.event = 4;
		f.owner = a_owner;
		f.vars = std::move(a_vars);
		a.data->frames.push_back(std::move(f));
		return a;
	}

	FunctionMessageData Call(TStr a_script, const Variable& a_receiver, std::vector<Variable> a_args = {})
	{
		FunctionMessageData d;
		d.scriptName = a_script;
		d.event = 4;
		d.unknownVar = a_receiver;
		d.vars = std::move(a_args);
		return d;
	}

	ArrayInfo Array(EID a_id)
	{
		ArrayInfo a;
		a.id = a_id;
		a.type = VType::kIntArray;
		a.length = 0;
		a.hasData = true;
		return a;
	}

	// ---- tests ------------------------------------------------------------------------------

	void TestFloatLiteral()
	{
		for (float f : { 0.0f, 1.0f, 0.1f, 3.25f, -2.5f, 1e-10f, 1.5e-38f, 1e30f, 123456789.0f, 1.4e-45f, 0.0000001f }) {
			const auto s = pex::FloatLiteral(f);
			char       what[160];
			std::snprintf(what, sizeof(what), "FloatLiteral(%g) = %s reads back exactly, no exponent", f, s.c_str());
			Check(std::strtof(s.c_str(), nullptr) == f && s.find_first_of("eE") == std::string::npos, what);
		}
	}

	void TestCreationClub()
	{
		Check(modremove::IsCreationClubPlugin("ccbgssse001-fish.esm"), "ccbgssse001-fish.esm is Creation Club");
		Check(modremove::IsCreationClubPlugin("ccqdrsse001-survivalmode.esl"), "ccqdrsse001-survivalmode.esl is Creation Club");
		Check(modremove::IsCreationClubPlugin("ccasvsse001-almsivi.esm"), "ccasvsse001-almsivi.esm is Creation Club");
		Check(!modremove::IsCreationClubPlugin("ccuserpatch.esp"), "ccuserpatch.esp is NOT Creation Club");
		Check(!modremove::IsCreationClubPlugin("cc.esp"), "cc.esp is NOT Creation Club");
	}

	void TestCheck()
	{
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		p.instances.push_back(Instance(10, 1, 1));
		p.instanceDataOrder = { 10 };
		p.reindex();
		Check(p.check().empty(), "a sound Papyrus section has no problems");
		p.instances[0].script = 999;
		Check(!p.check().empty(), "a bad string index is reported");
		p.instances[0].script = 1;
		p.instances.push_back(Instance(10, 2, 2));
		p.instanceDataOrder.push_back(10);
		Check(!p.check().empty(), "a duplicate instance id is reported");
	}

	// review item 3: a queued call removed with the purged script takes its waiting thread with it
	void TestPurgeKeepsThreadsConsistent()
	{
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		p.instances.push_back(Instance(10, 1, 1));  // ScriptA on a ModA object
		p.instanceDataOrder = { 10 };
		// thread 7 runs ScriptB and waits on a queued call to ScriptA
		p.activeScripts.push_back(Thread(7, 2, Ref(0)));
		p.activeDataOrder = { 7 };
		FunctionMessage m;
		m.flag = 0;
		m.id = 7;
		m.flags = 1;
		m.data = Call(1, Ref(10));
		p.functionMessages.push_back(m);
		p.reindex();
		const auto r = ops::PurgeScript(*s, "ScriptA", false);
		Check(r.messages == 1, "purge removes the queued call");
		Check(r.threads == 1 && p.activeScripts.empty(), "purge removes the thread waiting on that call (no orphaned thread)");
	}

	// review item 1: an array a surviving waiting/queued call still uses is not deleted
	void TestRemoveKeepsSharedArrays()
	{
		for (int where = 0; where < 2; ++where) {
			auto  s = MakeSave();
			auto& p = *s->papyrus();
			auto  inst = Instance(10, 1, 1);  // ScriptA on a ModA object, holding array 50
			inst.data->vars.push_back(Ref(50, VType::kIntArray));
			p.instances.push_back(inst);
			p.instanceDataOrder = { 10 };
			p.arrays.push_back(Array(50));
			p.arrayDataOrder = { 50 };
			// a ModB call that stays passes the same array
			p.instances.push_back(Instance(11, 2, 2));  // ScriptB on a ModB object
			p.instanceDataOrder.push_back(11);
			if (where == 0) {
				SuspendedStack st;
				st.id = 9;
				st.flag = 1;
				st.data = Call(2, Ref(11), { Ref(50, VType::kIntArray) });
				p.suspended1.push_back(st);
			} else {
				FunctionMessage m;
				m.flag = 3;
				m.flags = 1;
				m.data = Call(2, Ref(11), { Ref(50, VType::kIntArray) });
				p.functionMessages.push_back(m);
			}
			p.reindex();
			modremove::Options o;
			const auto plan = modremove::MakePlan(*s, "ModA.esp", nullptr, o);
			Check(plan.instances.contains(10) && !plan.instances.contains(11), "removing ModA removes its instance only");
			Check(!plan.arrays.contains(50), where == 0 ? "an array a surviving suspended call uses is kept"
														: "an array a surviving queued call uses is kept");
		}
		// and one nothing else uses is removed
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		auto  inst = Instance(10, 1, 1);
		inst.data->vars.push_back(Ref(50, VType::kIntArray));
		p.instances.push_back(inst);
		p.instanceDataOrder = { 10 };
		p.arrays.push_back(Array(50));
		p.arrayDataOrder = { 50 };
		p.reindex();
		const auto plan = modremove::MakePlan(*s, "ModA.esp", nullptr, modremove::Options{});
		Check(plan.arrays.contains(50), "an array only the removed instance used is removed");
	}

	// Papyrus section: write, read back, write again gives the same bytes
	void TestPapyrusRoundTrip()
	{
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		auto  inst = Instance(10, 1, 1);
		inst.data->vars.push_back(Ref(50, VType::kIntArray));
		p.instances.push_back(inst);
		p.instanceDataOrder = { 10 };
		p.arrays.push_back(Array(50));
		p.arrayDataOrder = { 50 };
		p.reindex();
		Writer w1;
		p.write(w1);
		Papyrus q;
		Reader  r(w1.buf.data(), w1.buf.size());
		q.read(r);
		Writer w2;
		q.write(w2);
		Check(w1.buf == w2.buf, "a Papyrus section writes back byte-identical after a read");
		Check(q.check().empty(), "the re-read section has no problems");
	}
	// Transplant: snapshots are read back and compared the way the GUI does
	void TestEditedName()
	{
		const std::filesystem::path dir = std::filesystem::temp_directory_path();
		auto n = [&](const char* a, const char* tag) { return ess::EditedSaveName(dir / a, tag, false).filename().string(); };
		Check(n("Save7120_8606206E_0_4861656C656168_WhiterunBanneredMare_222347_20261025075454_66_1.ess", "edited") ==
				  "Save7120_8606206E_0_4861656C656168_WhiterunBanneredMare_222347_20261025075454edited_66_1.ess",
			"edited save name: tag after the date, the game's fields kept");
		Check(n("Save7120_8606206E_0_4861656C656168_WhiterunBanneredMare_222347_20261025075454edited_66_1.ess", "cleaned") ==
				  "Save7120_8606206E_0_4861656C656168_WhiterunBanneredMare_222347_20261025075454cleaned_66_1.ess",
			"edited save name: an earlier tag is replaced, not stacked");
		Check(n("Save68_00000000_1_416C6C616E646F72_Tamriel_010446_20261024080619_16_1.ess", "without Some_Mod-1") ==
				  "Save68_00000000_1_416C6C616E646F72_Tamriel_010446_20261024080619withoutSomeMod1_16_1.ess",
			"edited save name: letters and digits only");
		Check(n("my save.ess", "edited") == "my save_edited.ess", "edited save name: other names get _tag");
	}


	void TestSaveArchive()
	{
		namespace ar = ess::archive;
		namespace fs = std::filesystem;
		const auto base = fs::temp_directory_path() / "rs_archive_test";
		fs::remove_all(base);
		const auto saves = base / "Saves", arch = base / "Archive";
		fs::create_directories(saves);
		const auto ess = saves / "Save5_ABCDEF01_0_4E616D65_Whiterun_000101_20261025142859_5_1.ess";
		auto co = ess;
		co.replace_extension(".skse");
		auto write = [&](const std::string& a_text) {
			std::ofstream(ess, std::ios::binary) << a_text;
			std::ofstream(co, std::ios::binary) << "co-" + a_text;
		};
		auto read = [](const fs::path& p) {
			std::ifstream f(p, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(f), {});
		};
		write("v0");
		Check(ar::Add(arch, ess, ess, "Saved over it").has_value(), "archive: a save is archived");
		Check(!ar::Add(arch, ess, ess, "Saved over it").has_value(), "archive: the same unchanged save is not archived twice");
		for (int i = 1; i <= 11; ++i) {
			write("v" + std::to_string(i) + std::string(static_cast<std::size_t>(i), 'x'));  // a new size each time
			ar::Add(arch, ess, ess, "Saved over it");
		}
		auto all = ar::List(arch);
		Check(all.size() == ar::kKeep, "archive: only the newest 10 are kept");
		Check(!all.empty() && read(all.front().ess) == "v11" + std::string(11, 'x') && read(all.front().skse) == "co-v11" + std::string(11, 'x'),
			"archive: newest first, with its .skse");
		Check(read(all.back().ess) == "v2xx", "archive: the oldest ones were deleted");
		// put the oldest back: the file there now is archived first and the oldest entry must survive until it is copied
		write("current");
		const auto oldest = all.back();
		const auto p = ar::RestoreInPlace(arch, oldest);
		Check(p == ess && read(ess) == "v2xx" && read(co) == "co-v2xx", "archive: restore in place puts the save and its .skse back");
		all = ar::List(arch);
		Check(all.size() == ar::kKeep && read(all.front().ess) == "current" && all.front().reason == "Replaced by a restore",
			"archive: the replaced save was archived first");
		const auto copy = ar::RestoreAsCopy(all.front());
		Check(copy.filename().string().find("restored") != std::string::npos && read(copy) == "current" && read(ess) == "v2xx",
			"archive: restore as a copy leaves the save alone");
		fs::remove_all(base);
	}


	void TestPluginProperty()
	{
		namespace fs = std::filesystem;
		const auto file = fs::temp_directory_path() / "rs_plugedit_test.esp";
		// a QUST with a script holding one object property, and a second record after it in another group
		auto w16 = [](std::vector<std::uint8_t>& v, std::uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); };
		auto w32 = [&](std::vector<std::uint8_t>& v, std::uint32_t x) { w16(v, x & 0xFFFF); w16(v, x >> 16); };
		auto wstr = [&](std::vector<std::uint8_t>& v, const std::string& t) { w16(v, static_cast<std::uint16_t>(t.size())); v.insert(v.end(), t.begin(), t.end()); };
		std::vector<std::uint8_t> vm;
		w16(vm, 5); w16(vm, 2); w16(vm, 1);
		wstr(vm, "MyQuestScript"); vm.push_back(0);
		w16(vm, 1);
		wstr(vm, "MySpell"); vm.push_back(1); vm.push_back(1);
		w16(vm, 0); w16(vm, 0xFFFF); w32(vm, 0x00012FCD);
		ess::merge::Record q;
		std::memcpy(q.sig, "QUST", 4);
		q.formID = 0x01000800;
		ess::merge::Sub e; std::memcpy(e.sig, "EDID", 4); e.data = { 'Q', 0 };
		ess::merge::Sub v; std::memcpy(v.sig, "VMAD", 4); v.data = vm;
		q.subs = { e, v };
		ess::merge::Record g;
		std::memcpy(g.sig, "GLOB", 4);
		g.formID = 0x01000801;
		g.subs = { e };
		ess::merge::Record q2 = q;  // after the edited record, in the same group, and groups of other types around it
		q2.formID = 0x01000802;
		ess::merge::Record sp;
		std::memcpy(sp.sig, "SPEL", 4);
		sp.formID = 0x01000803;
		sp.subs = { e };
		ess::merge::Record wr;
		std::memcpy(wr.sig, "WEAP", 4);
		wr.formID = 0x01000804;
		wr.subs = { e };
		const auto bytes = ess::merge::WritePlugin({ "Skyrim.esm" }, 0, { g, q, q2, sp, wr });
		std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

		namespace pe = ess::plugedit;
		const auto h = pe::ReadHeader(file);
		Check(h.ok && h.masters.size() == 1 && h.masters[0] == "Skyrim.esm", "plugin edit: header and masters read");
		Check(pe::GetObjectProperty(file, 0x01000800, "myquestscript", "myspell") == 0x00012FCD, "plugin edit: property read");
		auto r = pe::SetObjectProperty(file, 0x01000800, "MyQuestScript", "MySpell", 0x0003AE9F);
		Check(r.ok && pe::GetObjectProperty(file, 0x01000800, "MyQuestScript", "MySpell") == 0x0003AE9F, "plugin edit: object property changed in place");
		Check(!r.backup.empty() && fs::exists(r.backup), "plugin edit: original kept as a backup");
		const auto firstBackup = r.backup;
		r = pe::SetObjectProperty(file, 0x01000800, "MyQuestScript", "OtherSpell", 0x01000801);
		Check(r.backup != firstBackup && fs::exists(firstBackup) && pe::GetObjectProperty(firstBackup, 0x01000800, "MyQuestScript", "MySpell") == 0x00012FCD,
			"plugin edit: a second change keeps the first backup (the true original)");
		Check(r.ok && pe::GetObjectProperty(file, 0x01000800, "MyQuestScript", "OtherSpell") == 0x01000801 &&
				  pe::GetObjectProperty(file, 0x01000800, "MyQuestScript", "MySpell") == 0x0003AE9F,
			"plugin edit: a property not filled yet is added, the others kept");
		const auto back = ess::merge::ReadPlugin(file);
		std::set<std::uint32_t> ids;
		for (const auto& rec : back.records) {
			ids.insert(rec.formID);
		}
		Check(back.records.size() == 5 && ids.size() == 5 && pe::GetObjectProperty(file, 0x01000802, "MyQuestScript", "MySpell") == 0x00012FCD,
			"plugin edit: the file's groups still add up, the other records untouched");
		Check(!pe::SetObjectProperty(file, 0x01000800, "NotThere", "MySpell", 1).ok, "plugin edit: a script not on the record is refused");
		for (const auto& x : fs::directory_iterator(fs::temp_directory_path())) {
			if (x.path().filename().string().starts_with("rs_plugedit_test.esp")) {
				fs::remove(x.path());
			}
		}
	}


	void TestPluginsTxtFormats()
	{
		namespace fs = std::filesystem;
		const auto dir = fs::temp_directory_path();
		const auto se = dir / "rs_plugins_se.txt", le = dir / "rs_plugins_le.txt";
		std::ofstream(se) << "# This file is used by Skyrim to keep track of your downloaded content.\n*ModA.esp\nModB.esp\n*ModC.esm\n";
		std::ofstream(le) << "# This file is used by Skyrim to keep track of your downloaded content.\nDawnguard.esm\nModA.esp\nModB.esp\n";
		const auto a = ess::loadorder::Read(se, dir, false);
		Check(a.plugins.contains("moda.esp") && !a.plugins.contains("modb.esp") && a.plugins.contains("modc.esm"),
			"plugins.txt SE: only the lines marked with * are enabled");
		const auto b = ess::loadorder::Read(le, dir, false);
		Check(b.plugins.contains("moda.esp") && b.plugins.contains("modb.esp") && b.plugins.contains("dawnguard.esm"),
			"plugins.txt LE: no marks, every listed plugin is enabled");
		fs::remove(se);
		fs::remove(le);
	}

	void TestTransplant()
	{
		namespace tp = ess::transplant;
		const auto dir = std::filesystem::temp_directory_path();
		auto write = [&](const char* a_name, const std::string& a_text) {
			const auto p = dir / a_name;
			std::ofstream(p, std::ios::binary) << a_text;
			return p;
		};
		const auto good = write("rs_tp_good.txt",
			"RSH-SNAPSHOT\t1\t1.6.1170.0\tAllandor\n"
			"plugin\tSkyrim.esm\n"
			"player\t10\t50.0\t300.0\t0\n"
			"skill\t0\t20.0\t1.0\t10.0\t0\n"
			"av\tHealth\t150.0\n"
			"perk\tSkyrim.esm\t0BABE4\t000BABE4\tArmsman\n"
			"item\tSkyrim.esm\t00000F\t0000000F\tGold\t100\t\t\t\t\t\t0\n"
			"item\tSkyrim.esm\t012EB7\t00012EB7\tIron Sword\t1\t\t\t\t\t\t0\n"
			"quest\tSkyrim.esm\t03372B\t0003372B\tMQ101|Unbound\t0\t1\t1000\t10,1000\n"
			"quest\tSkyrim.esm\t03372A\t0003372A\tMQ102|Before the Storm\t1\t0\t10\t10\n");
		const auto bad = write("rs_tp_bad.txt",
			"RSH-SNAPSHOT\t1\t1.6.1170.0\tAllandor\n"
			"plugin\tSkyrim.esm\n"
			"player\t15\t80.0\t400.0\t2\n"
			"skill\t0\t35.0\t2.0\t20.0\t0\n"
			"skill\t1\t15.0\t0.0\t10.0\t0\n"
			"av\tHealth\t150.0\n"
			"perk\tSkyrim.esm\t0BABE4\t000BABE4\tArmsman\n"
			"perk\tSkyrim.esm\t079342\t00079342\tBladesman\n"
			"spell\tDawnguard.esm\t00A26E\t0200A26E\tConjure Wrathman\n"
			"marker\tSkyrim.esm\t016E2B\t00016E2B\tRiverwood\t3\n"
			"item\tSkyrim.esm\t00000F\t0000000F\tGold\t350\t\t\t\t\t\t0\n"
			"item\tSkyrim.esm\t012EB7\t00012EB7\tIron Sword\t1\t\t\t\t\t\t0\n"
			"item\tSkyrim.esm\t012EB7\t00012EB7\tIron Sword\t1\tMy Blade\t1.2000\t\tC:w:1500:Skyrim.esm:04605A:20:0:1\t\t0\n"
			"potion\tPotion of Healing\t3\t0\tSkyrim.esm:03EB15:50:0:0\n"
			"quest\tSkyrim.esm\t03372B\t0003372B\tMQ101|Unbound\t0\t1\t1000\t10,1000\n"
			"quest\tSkyrim.esm\t03372A\t0003372A\tMQ102|Before the Storm\t0\t1\t1000\t10,1000\n"
			"quest\tSkyrim.esm\t02610C\t0002610C\tMQ103|Dragon Rising\t1\t0\t80\t10,80\n"
			"equip\tSkyrim.esm\t012EB7\t00012EB7\tIron Sword\tright\tMy Blade\n"
			"equipshout\tSkyrim.esm\t013E07\t00013E07\tUnrelenting Force\tvoice\n"
			"look\tReSaverPP_Allandor_20261024\tRaceMenu\n");
		const auto g = tp::ReadSnapshot(good), b = tp::ReadSnapshot(bad);
		Check(g.valid && b.valid && b.level == 15 && b.perks.size() == 2, "snapshots are read");
		const auto changes = tp::Diff(g, b);
		auto find = [&](const std::string& a_label) -> const tp::Change* {
			for (const auto& c : changes) {
				if (c.label.find(a_label) != std::string::npos) {
					return &c;
				}
			}
			return nullptr;
		};
		Check(find("Level 15 (now 10)") != nullptr, "transplant: a higher level is listed");
		Check(find("One-handed 35") && find("Two-handed 15") && !find("Health"), "transplant: higher skills listed, equal attributes not");
		Check(find("Bladesman") && !find("Armsman"), "transplant: only perks the good save lacks");
		Check(find("250 gold") != nullptr, "transplant: gold is the difference (350 - 100)");
		const auto* sword = find("My Blade");
		Check(sword && sword->cat == tp::Category::kMade && sword->record.size() > 9 && sword->record[9].rfind("C:w:1500:", 0) == 0,
			"transplant: a sword with the player's own enchantment is listed, enchantment kept");
		int plainSwords = 0;
		for (const auto& c : changes) {
			plainSwords += c.label == "Iron Sword x1" ? 1 : 0;
		}
		Check(plainSwords == 0, "transplant: the plain sword the good save has is not listed again");
		Check(find("Potion of Healing x3") != nullptr, "transplant: a potion the player made is listed");
		const auto* conj = find("Conjure Wrathman");
		Check(conj && !conj->available && !conj->tick, "transplant: something from a plugin the good game lacks cannot be ticked");
		const auto* mq102 = find("Before the Storm - complete");
		const auto* mq103 = find("Dragon Rising - stage 80");
		Check(mq102 && mq102->complete && mq102->goodRuntime == 0x3372A && mq103 && !mq103->complete && !find("Unbound"),
			"transplant: quests: completed ones and further stages, not the ones already done");
		const auto* look = find("Your look from the bad save");
		const auto* wear = find("Wear / hold: My Blade");
		Check(look && look->cat == tp::Category::kAppearance && look->record.size() == 2 && look->record[1] == "ReSaverPP_Allandor_20261024" && wear &&
				  wear->record[0] == "equip" && find("Shout selected: Unrelenting Force"),
			"transplant: the RaceMenu look and the worn gear are listed");
		// helper 10: followers
		{
			const auto g10 = write("rs_tp_good10.txt", "RSH-SNAPSHOT\t1\t1-6-1170-0\tAllandor\n"
													   "follower\tInigo.esp\t000D62\t5700D62\tInigo\t1\t0\tInigo.esp*001D8B*0\t\n");
			const auto b10 = write("rs_tp_bad10.txt", "RSH-SNAPSHOT\t1\t1-6-1170-0\tAllandor\n"
													  "follower\tInigo.esp\t000D62\t5700D62\tInigo\t1\t0\tInigo.esp*001D8B*0\tSkyrim.esm*01397E*2\n"
													  "follower\tSkyrim.esm\t0A2C94\t000A2C94\tLydia\t1\t0\tSkyrim.esm*0750BA*0\tSkyrim.esm*013989*1\n");
			const auto c10 = tp::Diff(tp::ReadSnapshot(g10), tp::ReadSnapshot(b10));
			const tp::Change *inigo = nullptr, *lydia = nullptr;
			for (const auto& c : c10) {
				inigo = c.cat == tp::Category::kFollowers && c.label.rfind("Inigo", 0) == 0 ? &c : inigo;
				lydia = c.cat == tp::Category::kFollowers && c.label.rfind("Lydia", 0) == 0 ? &c : lydia;
			}
			Check(lydia && lydia->label.find("bring back") != std::string::npos && lydia->record.size() == 9 && lydia->record[0] == "follower" && inigo &&
					  inigo->label.find("already following") != std::string::npos,
				"transplant: followers - a missing one brought back, one already following only gets its items");
			std::filesystem::remove(g10);
			std::filesystem::remove(b10);
		}
		// helper 9: the journal list decides which quests are journal quests; one the good journal lacks is shown
		{
			const auto g9 = write("rs_tp_good9.txt",
				"RSH-SNAPSHOT\t1\t1-6-1170-0\tAllandor\n"
				"quest\tSkyrim.esm\t025231\t00025231\tCR04|Hired Muscle\t1\t0\t1\t\t\t5\n"
				"quest\tSkyrim.esm\t03F46A\t0003F46A\tFavor018|Some Light Theft\t0\t0\t0\t\t\t6\n"
				"quest\tSkyrim.esm\t0242BA\t000242BA\tMQ105|The Way of the Voice\t0\t0\t0\t\t\t1\n");
			const auto b9 = write("rs_tp_bad9.txt",
				"RSH-SNAPSHOT\t1\t1-6-1170-0\tAllandor\n"
				"quest\tSkyrim.esm\t025231\t00025231\tCR04|Hired Muscle\t1\t0\t1\t\t\t5\n"
				"quest\tSkyrim.esm\t03F46A\t0003F46A\tFavor018|Some Light Theft\t1\t0\t200\t\t10:4,15:4\t6\n"
				"quest\tSkyrim.esm\t0242BA\t000242BA\tMQ105|The Way of the Voice\t1\t0\t10\t\t10:1\t1\n"
				"jobj\tSkyrim.esm\t0242BA\t000242BA\t10\t1\n"
				"jobj\tSkyrim.esm\t025231\t00025231\t10\t3\n");
			std::ofstream(g9, std::ios::app) << "jobj\tSkyrim.esm\t0BBBBB\t000BBBBB\t10\t1\n";
			const auto c9 = tp::Diff(tp::ReadSnapshot(g9), tp::ReadSnapshot(b9));
			const tp::Change *voice = nullptr, *muscle = nullptr, *theft = nullptr;
			for (const auto& c : c9) {
				voice = c.label.rfind("The Way of the Voice", 0) == 0 ? &c : voice;
				muscle = c.label.rfind("Hired Muscle", 0) == 0 ? &c : muscle;
				theft = c.label.rfind("Some Light Theft", 0) == 0 ? &c : theft;
			}
			Check(voice && voice->tick && !voice->background && muscle && muscle->journalOnly && muscle->tick && muscle->quest->objectives == "10:3" &&
					  theft && theft->background && !theft->tick,
				"transplant: helper 9 journal list: journal quests ticked, one only missing from the journal shown, background ones unticked");
			std::filesystem::remove(g9);
			std::filesystem::remove(b9);
		}
		std::filesystem::remove(good);
		std::filesystem::remove(bad);
	}


	// ---- merged patch ---------------------------------------------------------------------------
	merge::Sub MS(const char* a_sig, std::vector<std::uint8_t> a_data)
	{
		merge::Sub s;
		std::memcpy(s.sig, a_sig, 4);
		s.data = std::move(a_data);
		return s;
	}
	std::vector<std::uint8_t> U32v(std::initializer_list<std::uint32_t> a)
	{
		std::vector<std::uint8_t> o;
		for (auto v : a) {
			for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
		}
		return o;
	}
	std::vector<std::uint8_t> Lvlo(std::uint16_t a_level, std::uint32_t a_form, std::uint16_t a_count)
	{
		std::vector<std::uint8_t> o{ static_cast<std::uint8_t>(a_level), static_cast<std::uint8_t>(a_level >> 8), 0, 0 };
		auto f = U32v({ a_form });
		o.insert(o.end(), f.begin(), f.end());
		o.push_back(static_cast<std::uint8_t>(a_count));
		o.push_back(0);
		o.push_back(0);
		o.push_back(0);
		return o;
	}
	merge::Record MR(const char* a_sig, std::uint32_t a_id, std::vector<merge::Sub> a_subs)
	{
		merge::Record r;
		std::memcpy(r.sig, a_sig, 4);
		r.formID = a_id;
		r.subs = std::move(a_subs);
		return r;
	}
	merge::Record Leveled(std::uint32_t a_id, std::vector<std::uint32_t> a_items)
	{
		std::vector<merge::Sub> subs{ MS("EDID", { 'L', 'L', 0 }), MS("LVLD", { 0 }), MS("LVLF", { 1 }),
			MS("LLCT", { static_cast<std::uint8_t>(a_items.size()) }) };
		for (auto i : a_items) {
			subs.push_back(MS("LVLO", Lvlo(1, i, 1)));
		}
		return MR("LVLI", a_id, std::move(subs));
	}

	// Skyrim Legendary Edition: Skyrim.esm's header has record version 43; the patch is a normal plugin in LE's format
	void TestMergedPatchLE()
	{
		namespace fs = std::filesystem;
		const auto dir = fs::temp_directory_path() / "rs_merge_test_le";
		fs::remove_all(dir);
		fs::create_directories(dir);
		auto save = [&](const char* a_name, const std::vector<std::uint8_t>& a_data) {
			std::ofstream(dir / a_name, std::ios::binary).write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
		};
		auto le = [](merge::Record r) {
			r.version = 43;
			return r;
		};
		save("Skyrim.esm", merge::WritePlugin({}, 1, { le(Leveled(0x810, { 0x900, 0x901 })) }, "t", {}, 0.94f, 43));
		save("ModA.esp", merge::WritePlugin({ "Skyrim.esm" }, 0, { le(Leveled(0x810, { 0x900, 0x901, 0x902 })) }, "t", {}, 0.94f, 43));
		// LE ignores the ESL flag: a plugin that carries it is still a normal plugin there
		save("ModB.esp", merge::WritePlugin({ "Skyrim.esm" }, 0x200, { le(Leveled(0x810, { 0x900, 0x901, 0x903 })) }, "t", {}, 0.94f, 43));
		// LE plugins.txt: no '*' marks, every listed plugin is enabled
		{
			std::ofstream(dir / "plugins.txt") << "# This file is used by Skyrim to keep track of your downloaded content.\r\nModA.esp\r\nModB.esp\r\n";
		}
		merge::Options o;
		Check(merge::ReadLoadOrder(dir / "plugins.txt", {}).size() == 5, "merge: an SE plugins.txt without '*' marks has nothing enabled");
		fs::create_directories(dir / "game");
		std::ofstream(dir / "game" / "TESV.exe") << "x";
		o.loadOrder = merge::ReadLoadOrder(dir / "plugins.txt", dir / "game");
		Check(o.loadOrder.size() == 7 && o.loadOrder[5] == "ModA.esp" && o.loadOrder[6] == "ModB.esp",
			"merge LE: plugins.txt without '*' marks reads every listed plugin as enabled");
		o.loadOrder = { "Skyrim.esm", "ModA.esp", "ModB.esp" };
		o.findPlugin = [&](const std::string& n) -> std::optional<fs::path> { return dir / n; };
		const auto r = merge::Build(o);
		Check(r.ok && r.legendary, "merge LE: built, Legendary Edition found from Skyrim.esm's header");
		Check(r.merged.size() == 1, "merge LE: the leveled list is merged");
		save("Patch.esp", r.file);
		const auto pf = merge::ReadPlugin(dir / "Patch.esp");
		Check(pf.flags == 0, "merge LE: the patch is not light-flagged");
		Check(r.file.size() > 36 && (r.file[20] | (r.file[21] << 8)) == 43, "merge LE: header record version 43");
		float hedr = 0;
		std::memcpy(&hedr, r.file.data() + 30, 4);
		Check(std::memcmp(r.file.data() + 24, "HEDR", 4) == 0 && hedr > 0.93f && hedr < 0.95f, "merge LE: HEDR version 0.94");
		bool both = false, v43 = false;
		for (const auto& rec : pf.records) {
			if (std::memcmp(rec.sig, "LVLI", 4) == 0) {
				v43 = rec.version == 43;
				std::vector<std::uint32_t> items;
				for (const auto& sb : rec.subs) {
					if (std::memcmp(sb.sig, "LVLO", 4) == 0 && sb.data.size() >= 8) {
						std::uint32_t v;
						std::memcpy(&v, sb.data.data() + 4, 4);
						items.push_back(v & 0xFFFFFF);
					}
				}
				both = std::find(items.begin(), items.end(), 0x902u) != items.end() && std::find(items.begin(), items.end(), 0x903u) != items.end();
			}
		}
		Check(both, "merge LE: both mods' additions are in the patch");
		Check(v43, "merge LE: records keep LE's record version 43");
		fs::remove_all(dir);
	}

	// Fallout 4 load order: record layouts from xEdit's FO4 definitions, Fallout 4's header, light (ESL) masters
	void TestMergedPatchFO4()
	{
		namespace fs = std::filesystem;
		const auto top = fs::temp_directory_path() / "rs_merge_test_fo4";
		const auto dir = top / "mods" / "Test";
		fs::remove_all(top);
		fs::create_directories(dir);
		auto save = [&](const char* a_name, const std::vector<std::uint8_t>& a_data) {
			std::ofstream(dir / a_name, std::ios::binary).write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
		};
		auto f4 = [](merge::Record r) {
			r.version = 131;
			return r;
		};
		// FO4 LVLO: level u16, unused 2, form, count u16, chance none u8, unused 1
		auto lvlo = [](std::uint16_t a_level, std::uint32_t a_form, std::uint8_t a_chanceNone) {
			auto o = Lvlo(a_level, a_form, 1);
			o[10] = a_chanceNone;
			return o;
		};
		auto lvli = [&](std::vector<std::pair<std::uint16_t, std::uint32_t>> a_items) {
			std::vector<merge::Sub> subs{ MS("EDID", { 'L', 'L', 0 }), MS("OBND", std::vector<std::uint8_t>(12, 0)), MS("LVLD", { 0 }), MS("LVLM", { 0 }),
				MS("LVLF", { 1 }), MS("LLCT", { static_cast<std::uint8_t>(a_items.size()) }) };
			for (auto [lv, id] : a_items) {
				subs.push_back(MS("LVLO", lvlo(lv, id, 0)));
			}
			subs.push_back(MS("LLKC", U32v({ 0x9A0, 50 })));  // keyword chance: keyword form + u32
			return f4(MR("LVLI", 0x810, std::move(subs)));
		};
		// a container: items with COED (owner NPC 0x830 -> a global follows), keywords, a FO4 script with a struct property
		auto vmadStruct = [](std::uint32_t a_obj) {
			std::vector<std::uint8_t> v{ 6, 0, 2, 0, 1, 0 };  // version 6, object format 2, 1 script
			auto wstr = [&](const char* t) {
				const auto n = static_cast<std::uint16_t>(std::strlen(t));
				v.push_back(static_cast<std::uint8_t>(n));
				v.push_back(0);
				v.insert(v.end(), t, t + n);
			};
			wstr("TestScript");
			v.push_back(0);           // script flags
			v.insert(v.end(), { 1, 0 });  // 1 property
			wstr("Data");
			v.push_back(7);           // struct
			v.push_back(1);           // edited
			v.insert(v.end(), { 2, 0, 0, 0 });  // 2 members
			wstr("Target");
			v.push_back(1);
			v.push_back(1);
			v.insert(v.end(), { 0, 0, 0xFF, 0xFF });  // object v2: unused, alias -1, form
			auto f = U32v({ a_obj });
			v.insert(v.end(), f.begin(), f.end());
			wstr("Count");
			v.push_back(3);
			v.push_back(1);
			v.insert(v.end(), { 5, 0, 0, 0 });
			return v;
		};
		auto cont = [&](std::vector<std::uint32_t> a_items, std::vector<std::uint32_t> a_keywords, bool a_script, std::uint32_t a_scriptObj = 0x9B0) {
			std::vector<merge::Sub> subs{ MS("EDID", { 'C', 0 }) };
			if (a_script) subs.push_back(MS("VMAD", vmadStruct(a_scriptObj)));
			subs.push_back(MS("OBND", std::vector<std::uint8_t>(12, 0)));
			subs.push_back(MS("PTRN", U32v({ 0x9C0 })));
			subs.push_back(MS("COCT", U32v({ static_cast<std::uint32_t>(a_items.size()) })));
			for (auto id : a_items) {
				subs.push_back(MS("CNTO", U32v({ id, 1 })));
				if (id == 0x900) {
					auto coed = U32v({ 0x830, 0x840 });  // owner NPC, its global
					coed.insert(coed.end(), { 0, 0, 0x80, 0x3F });
					subs.push_back(MS("COED", coed));
				}
			}
			subs.push_back(MS("DATA", { 0, 0, 0, 0, 0 }));
			subs.push_back(MS("KSIZ", U32v({ static_cast<std::uint32_t>(a_keywords.size()) })));
			subs.push_back(MS("KWDA", [&] { std::vector<std::uint8_t> o; for (auto k : a_keywords) { auto b = U32v({ k }); o.insert(o.end(), b.begin(), b.end()); } return o; }()));
			subs.push_back(MS("PRPS", U32v({ 0x9D0, 0x3F800000 })));
			return f4(MR("CONT", 0x820, std::move(subs)));
		};
		auto flst = [&](std::vector<std::uint32_t> a_forms) {
			std::vector<merge::Sub> subs{ MS("EDID", { 'F', 0 }) };
			for (auto id : a_forms) subs.push_back(MS("LNAM", U32v({ id })));
			return f4(MR("FLST", 0x850, std::move(subs)));
		};
		auto misc = [&](std::vector<std::uint32_t> a_keywords, std::uint32_t a_value) {
			std::vector<std::uint8_t> kw;
			for (auto k : a_keywords) { auto b = U32v({ k }); kw.insert(kw.end(), b.begin(), b.end()); }
			return f4(MR("MISC", 0x870, { MS("EDID", { 'M', 0 }), MS("OBND", std::vector<std::uint8_t>(12, 0)), MS("MODL", { 'm', 0 }), MS("MODS", U32v({ 0x9E0 })),
				MS("KSIZ", U32v({ static_cast<std::uint32_t>(a_keywords.size()) })), MS("KWDA", kw), MS("DATA", U32v({ a_value, 0 })),
				MS("CVPA", U32v({ 0x9F0, 2 })), MS("CDIX", { 0 }) }));
		};
		// an NPC with Fallout 4 parts: ModA and ModB both change its class, so the winner's stays and there is nothing to merge
		// (it is also read to know COED owners)
		auto npc = [&](std::uint32_t a_class) {
			return f4(MR("NPC_", 0x830, { MS("EDID", { 'N', 0 }), MS("ACBS", std::vector<std::uint8_t>(20, 0)), MS("CNAM", U32v({ a_class })),
				MS("MWGT", std::vector<std::uint8_t>(12, 0)) }));
		};
		// a leveled list with a part ReSaver Plus does not know for Fallout 4: left to the winner, with the reason
		auto odd = [&](std::uint32_t a_item) {
			return f4(MR("LVLN", 0x811, { MS("EDID", { 'O', 0 }), MS("LLCT", { 1 }), MS("LVLO", lvlo(1, a_item, 0)), MS("ABCD", { 0 }) }));
		};
		auto esm = merge::WritePlugin({}, 1, { lvli({ { 1, 0x900 }, { 5, 0x901 } }), cont({ 0x900 }, { 0x950 }, false), flst({ 0x900 }),
			misc({ 0x950 }, 10), npc(0x990), odd(0x900) }, "t", {}, 1.0f, 131, true);
		save("Fallout4.esm", esm);
		// DLCRobot.esm: a form of its own (0x01000900)
		save("DLCRobot.esm", merge::WritePlugin({ "Fallout4.esm" }, 1, { f4(MR("MISC", 0x01000900, { MS("EDID", { 'R', 0 }) })) }, "t", {}, 1.0f, 131, true));
		// ModA.esp (Fallout4, DLCRobot): adds DLCRobot's item at level 3, a container item, a FLST entry, a script on the container
		save("ModA.esp", merge::WritePlugin({ "Fallout4.esm", "DLCRobot.esm" }, 0, { lvli({ { 1, 0x900 }, { 5, 0x901 }, { 3, 0x01000900 } }),
			cont({ 0x900, 0x902 }, { 0x950 }, true, 0x01000900), flst({ 0x900, 0x901 }), misc({ 0x950, 0x951 }, 10), npc(0x991), odd(0x901) },
			"t", {}, 1.0f, 131, true));
		// ModB.esp (light, Fallout4): removes 0x900 and adds 0x903 at level 10, its own keyword (0x01000801) on the misc item, a
		// container keyword, value 20
		save("ModB.esp", merge::WritePlugin({ "Fallout4.esm" }, 0x200, { lvli({ { 5, 0x901 }, { 10, 0x903 } }), cont({ 0x900, 0x903 }, { 0x950, 0x952 }, false),
			flst({ 0x900, 0x904 }), misc({ 0x950, 0x01000801 }, 20), npc(0x992), odd(0x902),
			f4(MR("OTFT", 0x860, { MS("EDID", { 'O', 0 }), MS("INAM", U32v({ 0x01000801 })) })) }, "t", {}, 1.0f, 131, true));
		{
			std::ofstream(top / "plugins.txt") << "*ModA.esp\r\n*ModB.esp\r\n";
		}
		ScriptSources src;
		src.scan(top / "mods");
		merge::Options o;
		o.loadOrder = merge::ReadLoadOrder(top / "plugins.txt", {}, 2);
		Check(o.loadOrder.size() >= 3 && o.loadOrder[0] == "Fallout4.esm" && o.loadOrder[1] == "DLCRobot.esm" && o.loadOrder.back() == "ModB.esp",
			"merge FO4: load order starts with Fallout 4's masters");
		o.loadOrder = { "Fallout4.esm", "DLCRobot.esm", "ModA.esp", "ModB.esp" };
		merge::UseSources(o, src);
		const auto r = merge::Build(o);
		Check(r.ok && r.fo4 && r.light, "merge FO4: built for Fallout 4 (found from Fallout4.esm), light");
		std::string report = r.report();
		bool oddSkipped = false;
		for (const auto& sk : r.skipped) oddSkipped = oddSkipped || (sk.find("LVLN") != std::string::npos && sk.find("ABCD") != std::string::npos);
		Check(oddSkipped, "merge FO4: a record with an unknown part is not merged, with the reason");
		bool npcMerged = false;
		for (const auto& m : r.merged) npcMerged = npcMerged || m.type == "NPC_";
		Check(!npcMerged && r.merged.size() == 4, "merge FO4: LVLI, CONT, FLST, MISC merged; the NPC whose class both mods changed stays the winner's");
		Check(r.masters == std::vector<std::string>({ "Fallout4.esm", "DLCRobot.esm", "ModB.esp" }), "merge FO4: masters Fallout4, DLCRobot, ModB");
		save("Patch.esp", r.file);
		// the header: TES4 record version 131, HEDR 1.0, INTV then INCC, ESL flag
		float hedr = 0;
		std::memcpy(&hedr, r.file.data() + 30, 4);
		Check((r.file[20] | (r.file[21] << 8)) == 131 && hedr == 1.0f, "merge FO4: header record version 131, HEDR 1.0");
		const auto hsize = r.file[4] | (r.file[5] << 8);
		const std::string hdr(reinterpret_cast<const char*>(r.file.data()) + 24, hsize);
		Check(hdr.find("INTV") != std::string::npos && hdr.find("INCC") == hdr.find("INTV") + 10, "merge FO4: header has INTV then INCC");
		const auto pf = merge::ReadPlugin(dir / "Patch.esp");
		Check(pf.flags == 0x200 && pf.masters.size() == 3, "merge FO4: patch read back, light, three masters");
		// group order: Fallout 4's (CONT, MISC, LVLI, FLST)
		{
			std::vector<std::string> groups;
			std::size_t p = 24 + hsize;
			while (p + 24 <= r.file.size()) {
				groups.emplace_back(reinterpret_cast<const char*>(r.file.data()) + p + 8, 4);
				std::uint32_t gs;
				std::memcpy(&gs, r.file.data() + p + 4, 4);
				p += gs;
			}
			Check(groups == std::vector<std::string>({ "CONT", "MISC", "LVLI", "FLST" }), "merge FO4: groups in Fallout 4's order");
		}
		auto find = [&](const char* sig) -> const merge::Record* {
			for (const auto& rec : pf.records) {
				if (std::memcmp(rec.sig, sig, 4) == 0) return &rec;
			}
			return nullptr;
		};
		auto u32 = [](const std::vector<std::uint8_t>& d, std::size_t o) { std::uint32_t v; std::memcpy(&v, d.data() + o, 4); return v; };
		if (const auto* l = find("LVLI")) {
			std::vector<std::pair<int, std::uint32_t>> es;
			int llct = -1;
			bool llkc = false;
			for (const auto& sb : l->subs) {
				if (!std::memcmp(sb.sig, "LVLO", 4)) es.push_back({ sb.data[0] | (sb.data[1] << 8), u32(sb.data, 4) });
				if (!std::memcmp(sb.sig, "LLCT", 4)) llct = sb.data[0];
				if (!std::memcmp(sb.sig, "LLKC", 4)) llkc = sb.data.size() == 8 && u32(sb.data, 0) == 0x9A0;
			}
			// 0x900 removed by ModB, DLCRobot's 0x01000900 added by ModA (index 1 in the patch too), 0x903 added by ModB; by level
			Check(es == std::vector<std::pair<int, std::uint32_t>>({ { 3, 0x01000900 }, { 5, 0x901 }, { 10, 0x903 } }) && llct == 3 && llkc,
				"merge FO4: leveled list merged, sorted by level, LLCT updated, LLKC kept");
			Check(l->version == 131, "merge FO4: records keep Fallout 4's record version");
		} else {
			Check(false, "merge FO4: leveled list in the patch");
		}
		if (const auto* c = find("CONT")) {
			std::vector<std::uint32_t> items, kw;
			bool coed = false, vmad = false;
			std::uint32_t coct = 0;
			for (std::size_t i = 0; i < c->subs.size(); ++i) {
				const auto& sb = c->subs[i];
				if (!std::memcmp(sb.sig, "CNTO", 4)) {
					items.push_back(u32(sb.data, 0));
					if (u32(sb.data, 0) == 0x900 && i + 1 < c->subs.size() && !std::memcmp(c->subs[i + 1].sig, "COED", 4)) {
						coed = u32(c->subs[i + 1].data, 0) == 0x830 && u32(c->subs[i + 1].data, 4) == 0x840;
					}
				}
				if (!std::memcmp(sb.sig, "COCT", 4)) coct = u32(sb.data, 0);
				if (!std::memcmp(sb.sig, "KWDA", 4)) for (std::size_t k = 0; k < sb.data.size(); k += 4) kw.push_back(u32(sb.data, k));
				if (!std::memcmp(sb.sig, "VMAD", 4)) {
					// the struct member's object (DLCRobot's form) is at the same place, its index the patch's (1)
					const auto want = vmadStruct(0x01000900);
					vmad = sb.data == want;
				}
			}
			Check(items == std::vector<std::uint32_t>({ 0x900, 0x903, 0x902 }) && coct == 3 && coed, "merge FO4: container items merged, COED with its owner's global kept");
			Check(kw == std::vector<std::uint32_t>({ 0x950, 0x952 }), "merge FO4: container keywords merged");
			Check(vmad, "merge FO4: ModA's script with a struct property is taken (struct members read)");
		} else {
			Check(false, "merge FO4: container in the patch");
		}
		if (const auto* m = find("MISC")) {
			std::vector<std::uint32_t> kw;
			std::uint32_t value = 0;
			for (const auto& sb : m->subs) {
				if (!std::memcmp(sb.sig, "KWDA", 4)) for (std::size_t k = 0; k < sb.data.size(); k += 4) kw.push_back(u32(sb.data, k));
				if (!std::memcmp(sb.sig, "DATA", 4)) value = u32(sb.data, 0);
			}
			// the winner's (ModB) keywords, its own 0x01000801 as 0x02000801 (ModB is the patch's third master), then ModA's addition
			Check(kw == std::vector<std::uint32_t>({ 0x950, 0x02000801, 0x951 }) && value == 20, "merge FO4: misc item keywords merged, ModB's light form renumbered");
		} else {
			Check(false, "merge FO4: misc item in the patch");
		}
		// read back with the app's Fallout 4 plugin reader, the patch as a light plugin after the others
		{
			ScriptSources src2;
			src2.scan(top / "mods");
			PluginDB db;
			PluginDB::Input in;
			in.fo4 = true;
			in.full = { "Fallout4.esm", "DLCRobot.esm", "ModA.esp" };
			in.lite = { "ModB.esp", "Patch.esp" };
			in.sources = src2;
			PluginDB::Progress prog;
			db.load(in, prog);
			const auto* lf = db.find(0x00000810);
			const auto* kf = db.find(0x00000870);
			Check(db.missing.empty() && lf && db.pluginName(*lf) == "Patch.esp" && db.edid(*lf) == "LL" && kf && db.pluginName(*kf) == "Patch.esp",
				"merge FO4: PluginDB (Fallout 4) reads the patch: its records override the masters'");
		}
		// a Fallout 4 without light plugins (before 1.10): a normal plugin
		o.light = false;
		const auto r2 = merge::Build(o);
		Check(r2.ok && !r2.light && r2.file.size() > 12 && (r2.file[8] | (r2.file[9] << 8)) == 0, "merge FO4: --no-esl writes a normal plugin");
		Check(report.find("Fallout 4 load order") != std::string::npos, "merge FO4: the report says what is merged for Fallout 4");
		fs::remove_all(top);
	}

	// Fallout 4's NPCs, armour, weapons, ammunition, books, potions, ingredients, spells and cells (wbDefinitionsFO4 layouts).
	// Fallout4.esm has each record; ModA.esp (master Fallout4.esm) and ModB.esp (light, master Fallout4.esm, the winner) each
	// change different parts. ModA adds one of its own forms, so the patch's masters are Fallout4, ModA, ModB: ModB's own forms
	// (0x01000xxx in ModB) must become 0x02000xxx in the patch, numbers that only look like forms must stay as they are.
	void TestMergedPatchFO4Types()
	{
		namespace fs = std::filesystem;
		const auto dir = fs::temp_directory_path() / "rs_merge_test_fo4types";
		fs::remove_all(dir);
		fs::create_directories(dir);
		auto save = [&](const char* a_name, const std::vector<std::uint8_t>& a_data) {
			std::ofstream(dir / a_name, std::ios::binary).write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
		};
		struct B
		{
			std::vector<std::uint8_t> d;
			B& u8(std::uint8_t v) { d.push_back(v); return *this; }
			B& u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v)); return u8(static_cast<std::uint8_t>(v >> 8)); }
			B& u32(std::uint32_t v) { u16(static_cast<std::uint16_t>(v)); return u16(static_cast<std::uint16_t>(v >> 16)); }
			B& f32(float v) { std::uint32_t x; std::memcpy(&x, &v, 4); return u32(x); }
			B& zero(std::size_t n) { d.insert(d.end(), n, 0); return *this; }
		};
		auto f4 = [](merge::Record r) {
			r.version = 131;
			return r;
		};
		auto u32 = [](const std::vector<std::uint8_t>& d, std::size_t o) { std::uint32_t v = 0; if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4); return v; };
		auto u16 = [](const std::vector<std::uint8_t>& d, std::size_t o) { std::uint16_t v = 0; if (o + 2 <= d.size()) std::memcpy(&v, d.data() + o, 2); return v; };
		auto kw = [](std::vector<std::uint32_t> a) { std::vector<std::uint8_t> o; for (auto k : a) { auto b = U32v({ k }); o.insert(o.end(), b.begin(), b.end()); } return o; };
		auto obnd = [] { return MS("OBND", std::vector<std::uint8_t>(12, 0)); };
		// an object template: one combination with a name and an OBTS with one keyword, one include (an OMOD) and a FormID,Int
		// property (value type 4, value 1 a form), then STOP
		auto obte = [&](std::vector<merge::Sub>& subs, std::uint32_t a_keyword, std::uint32_t a_mod, std::uint32_t a_propForm) {
			subs.push_back(MS("OBTE", U32v({ 1 })));
			subs.push_back(MS("OBTF", {}));
			subs.push_back(MS("FULL", { 'T', 'p', 'l', 0 }));
			B o;
			o.u32(1).u32(1).u8(1).u8(0).u8(10).u8(0).u16(0xFFFF).u8(1);
			o.u8(1).u32(a_keyword).u8(0).u8(0);              // keywords (u8 count), min level for ranks, alt levels per tier
			o.u32(a_mod).u8(2).u8(0).u8(0);                  // include
			o.u8(4).zero(3).u8(0).zero(3).u16(61).zero(2).u32(a_propForm).u32(3).f32(0);  // property (Ammo, FormID,Int)
			subs.push_back(MS("OBTS", o.d));
			subs.push_back(MS("STOP", {}));
		};
		// ---- NPC_
		auto acbs = [](std::uint32_t a_flags, std::uint16_t a_level, std::uint16_t a_tflags) {
			B b;
			b.u32(a_flags).u16(0).u16(a_level).u16(1).u16(0).u16(35).u16(a_tflags).u16(0).u16(0);
			return b.d;
		};
		auto aidt = [](std::uint8_t a_aggression, std::uint8_t a_confidence) {
			B b;
			b.u8(a_aggression).u8(a_confidence).zero(6).u32(500).u32(1000).u32(2000).u8(0).zero(3);
			return b.d;
		};
		struct NpcSpec
		{
			std::vector<std::uint32_t> factions, perks, items, keywords;
			std::uint32_t              level = 5, head = 0xA10, name = 'N';
			std::uint8_t               aggression = 0;
			std::uint32_t              tplt = 0;
		};
		auto npc = [&](const NpcSpec& n) {
			std::vector<merge::Sub> subs{ MS("EDID", { 'N', 'P', 'C', 0 }), obnd(), MS("ACBS", acbs(0, static_cast<std::uint16_t>(n.level), n.tplt ? 1 : 0)) };
			for (auto f : n.factions) subs.push_back(MS("SNAM", B().u32(f).u8(0).d));
			if (n.tplt) {
				subs.push_back(MS("TPLT", U32v({ n.tplt })));
				subs.push_back(MS("TPTA", kw({ n.tplt, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 })));
			}
			subs.push_back(MS("RNAM", U32v({ 0xA00 })));
			subs.push_back(MS("ATKD", B().f32(1).f32(1).u32(0xA20).u32(0).zero(28).d));
			subs.push_back(MS("ATKE", { 'a', 0 }));
			subs.push_back(MS("PRKZ", U32v({ static_cast<std::uint32_t>(n.perks.size()) })));
			for (auto p : n.perks) subs.push_back(MS("PRKR", B().u32(p).u8(1).d));
			subs.push_back(MS("PRPS", U32v({ 0xA30, 0x3F800000 })));
			subs.push_back(MS("COCT", U32v({ static_cast<std::uint32_t>(n.items.size()) })));
			for (auto i : n.items) subs.push_back(MS("CNTO", U32v({ i, 1 })));
			subs.push_back(MS("AIDT", aidt(n.aggression, 1)));
			subs.push_back(MS("PKID", U32v({ 0xA40 })));
			subs.push_back(MS("KSIZ", U32v({ static_cast<std::uint32_t>(n.keywords.size()) })));
			subs.push_back(MS("KWDA", kw(n.keywords)));
			subs.push_back(MS("APPR", U32v({ 0xA50 })));
			obte(subs, 0xA60, 0xA70, 0xA80);
			subs.push_back(MS("CNAM", U32v({ 0xA90 })));
			subs.push_back(MS("FULL", { static_cast<std::uint8_t>(n.name), 0 }));
			subs.push_back(MS("DATA", {}));
			subs.push_back(MS("DNAM", B().u16(100).u16(50).u16(0).u8(0).u8(0).d));
			subs.push_back(MS("PNAM", U32v({ n.head })));
			subs.push_back(MS("NAM5", { 0xFF, 0 }));
			subs.push_back(MS("NAM6", B().f32(1).d));
			subs.push_back(MS("NAM7", B().f32(0).d));
			subs.push_back(MS("NAM4", B().f32(1).d));
			subs.push_back(MS("MWGT", B().f32(0.3f).f32(0.3f).f32(0.3f).d));
			subs.push_back(MS("NAM8", U32v({ 2 })));
			subs.push_back(MS("QNAM", B().f32(1).f32(1).f32(1).f32(0).d));
			subs.push_back(MS("TETI", B().u16(1).u16(1185).d));
			subs.push_back(MS("TEND", { 50, 0, 0, 0, 0, 0xFF, 0xFF }));
			subs.push_back(MS("FMRI", U32v({ 0x10 })));
			subs.push_back(MS("FMRS", std::vector<std::uint8_t>(36, 0)));
			return f4(MR("NPC_", 0x900, std::move(subs)));
		};
		// ---- WEAP: DNAM 132 (ammo @0, skill @40, resist @44, flags @48, value @63, sounds @73..@101), FNAM 41, CRDT 12
		auto weap = [&](std::uint32_t a_ammo, float a_speed, std::uint32_t a_value, std::uint32_t a_sound, std::uint32_t a_flags, std::uint32_t a_crit) {
			B d;
			d.u32(a_ammo).f32(a_speed).zero(28).u32(0).u32(0xB10).u32(0xB11).u32(a_flags).u16(8).u8(9).f32(0).f32(10).u32(a_value).u16(20).u32(0);
			d.u32(a_sound);
			for (int i = 0; i < 7; ++i) d.u32(0);
			d.u8(0).f32(0.3f).zero(2).f32(20).f32(0).f32(0).u32(0).zero(4);
			B fn;
			fn.f32(0).f32(0.5f).f32(1).f32(0.33f).f32(0).zero(4).f32(0.25f).u8(1).u32(0xB20).u32(0).u32(0);
			std::vector<merge::Sub> subs{ MS("EDID", { 'W', 0 }), obnd(), MS("FULL", { 'W', 0 }), MS("MODL", { 'w', 0 }), MS("MODT", std::vector<std::uint8_t>(20, 0)),
				MS("EITM", U32v({ 0xB30 })), MS("EAMT", { 5, 0 }), MS("ETYP", U32v({ 0xB40 })), MS("KSIZ", U32v({ 1 })), MS("KWDA", kw({ 0xB50 })) };
			obte(subs, 0xB60, 0xB70, a_ammo);
			subs.push_back(MS("DNAM", d.d));
			subs.push_back(MS("FNAM", fn.d));
			subs.push_back(MS("CRDT", B().f32(2).f32(0).u32(a_crit).d));
			subs.push_back(MS("INAM", U32v({ 0xB80 })));
			subs.push_back(MS("DAMA", U32v({ 0xB90, 10 })));
			return f4(MR("WEAP", 0x910, std::move(subs)));
		};
		// ---- ARMO: male and female models (MODC in both), armatures, DATA, FNAM armour rating, DAMA
		auto armo = [&](std::uint16_t a_rating, std::vector<std::uint32_t> a_keywords, std::uint32_t a_arma) {
			std::vector<merge::Sub> subs{ MS("EDID", { 'A', 0 }), obnd(), MS("FULL", { 'A', 0 }), MS("MOD2", { 'm', 0 }), MS("MO2T", std::vector<std::uint8_t>(20, 0)),
				MS("MODC", B().f32(1).d), MS("MOD4", { 'f', 0 }), MS("MO4T", std::vector<std::uint8_t>(20, 0)), MS("MODC", B().f32(2).d),
				MS("BOD2", U32v({ 4 })), MS("RNAM", U32v({ 0xC00 })), MS("KSIZ", U32v({ static_cast<std::uint32_t>(a_keywords.size()) })), MS("KWDA", kw(a_keywords)),
				MS("DESC", { 'd', 0 }), MS("INDX", { 0, 0 }), MS("MODL", U32v({ a_arma })), MS("DATA", B().u32(10).f32(1).u32(100).d),
				MS("FNAM", B().u16(a_rating).u16(0).u8(0).zero(3).d), MS("DAMA", U32v({ 0xC10, 5 })) };
			obte(subs, 0xC20, 0xC30, 0xC40);
			return f4(MR("ARMO", 0x920, std::move(subs)));
		};
		// ---- AMMO, BOOK, ALCH (with an effect and conditions), INGR, SPEL (conditions with numbers that look like forms)
		auto ammo = [&](std::uint32_t a_proj, float a_damage) {
			return f4(MR("AMMO", 0x930, { MS("EDID", { 'M', 0 }), obnd(), MS("FULL", { 'M', 0 }), MS("DESC", { 0 }), MS("KSIZ", U32v({ 0 })), MS("KWDA", {}),
				MS("DATA", B().u32(1).f32(0.1f).d), MS("DNAM", B().u32(a_proj).u8(0).zero(3).f32(a_damage).u32(0).d), MS("ONAM", { 'm', 0 }) }));
		};
		auto book = [&](std::uint8_t a_flags, std::uint32_t a_teaches, std::uint32_t a_value) {
			return f4(MR("BOOK", 0x940, { MS("EDID", { 'K', 0 }), obnd(), MS("FULL", { 'K', 0 }), MS("DESC", { 't', 0 }), MS("DATA", B().u32(a_value).f32(1).d),
				MS("DNAM", B().u8(a_flags).u32(a_teaches).u32(0).u32(0).d), MS("CNAM", { 0 }), MS("INAM", U32v({ 0xD10 })) }));
		};
		// CTDA: type, unused 3, comparison value @4, function @8, unused 2, parameter 1 @12, parameter 2 @16, run on @20,
		// reference @24, parameter 3 @28
		auto ctda = [](std::uint8_t a_type, std::uint16_t a_fn, std::uint32_t a_p1, std::uint32_t a_p2, std::uint32_t a_runOn, std::uint32_t a_ref) {
			return B().u8(a_type).zero(3).f32(1).u16(a_fn).zero(2).u32(a_p1).u32(a_p2).u32(a_runOn).u32(a_ref).u32(0xFFFFFFFF).d;
		};
		auto alch = [&](float a_magnitude, std::uint32_t a_flags) {
			return f4(MR("ALCH", 0x950, { MS("EDID", { 'P', 0 }), obnd(), MS("FULL", { 'P', 0 }), MS("KSIZ", U32v({ 0 })), MS("KWDA", {}), MS("DESC", { 0 }),
				MS("DATA", B().f32(0.5f).d), MS("ENIT", B().u32(5).u32(a_flags).u32(0xE10).f32(0).u32(0xE20).d), MS("DNAM", { 0 }),
				MS("EFID", U32v({ 0xE30 })), MS("EFIT", B().f32(a_magnitude).u32(0).u32(10).d), MS("CTDA", ctda(0, 560, 0xE40, 0, 0, 0)) }));
		};
		auto ingr = [&](std::uint32_t a_flags, std::uint32_t a_value) {
			return f4(MR("INGR", 0x960, { MS("EDID", { 'I', 0 }), obnd(), MS("FULL", { 'I', 0 }), MS("DATA", B().u32(a_value).f32(0.1f).d),
				MS("ENIT", B().u32(1).u32(a_flags).d), MS("EFID", U32v({ 0xE30 })), MS("EFIT", B().f32(1).u32(0).u32(0).d) }));
		};
		// the conditions: HasKeyword (560) on ModB's own keyword (a form), GetDistance (1) to an alias (flag 0x02: parameter 1 is
		// a number), GetIsCurrentPackage (161) run on a quest alias with the alias flag (parameter 1 stays a package)
		auto spel = [&](std::uint32_t a_cost, std::uint32_t a_kw, std::uint32_t a_pack) {
			return f4(MR("SPEL", 0x970, { MS("EDID", { 'S', 0 }), obnd(), MS("FULL", { 'S', 0 }), MS("ETYP", U32v({ 0xF00 })), MS("DESC", { 0 }),
				MS("SPIT", B().u32(a_cost).u32(0).u32(4).f32(0).u32(0).u32(0).f32(0).f32(0).u32(0).d), MS("EFID", U32v({ 0xE30 })),
				MS("EFIT", B().f32(1).u32(0).u32(0).d), MS("CTDA", ctda(0, 560, a_kw, 0, 0, 0)), MS("CTDA", ctda(0x02, 1, 0x01000005, 0, 0, 0)),
				MS("CTDA", ctda(0x02, 161, a_pack, 0, 5, 0)) }));
		};
		// ---- CELL (interior): lighting (XCLL + LTMP), regions, previs (VISI, RVIS, PCMB, XPRI, XCRI)
		auto cell = [&](std::uint8_t a_ambient, std::vector<std::uint32_t> a_regions, std::uint32_t a_previsRef, std::uint16_t a_visi) {
			auto c = f4(MR("CELL", 0x980, { MS("EDID", { 'C', 0 }), MS("DATA", { 1, 0 }), MS("VISI", B().u16(a_visi).d), MS("RVIS", U32v({ 0x980 })),
				MS("PCMB", B().u16(a_visi).d), MS("XCLL", B().u8(a_ambient).zero(135).d), MS("LTMP", U32v({ 0xF10 })), MS("XCLW", B().f32(0).d),
				MS("XCLR", kw(a_regions)), MS("XPRI", U32v({ a_previsRef })), MS("XCRI", B().u32(1).u32(2).u32(0x1234).u32(a_previsRef).u32(0x1234).d) }));
			c.groups = { { 2, 0 }, { 3, 0 } };
			return c;
		};
		// Fallout4.esm
		NpcSpec n0;
		n0.factions = { 0xA01 };
		n0.perks = { 0xA02 };
		n0.items = { 0xA03 };
		n0.keywords = { 0xA04 };
		save("Fallout4.esm", merge::WritePlugin({}, 1, { npc(n0), weap(0xB00, 1.0f, 100, 0xB01, 0, 0), armo(10, { 0xC01 }, 0xC02), ammo(0xD00, 5),
			book(0, 0, 10), alch(1, 0), ingr(0, 1), spel(10, 0xA04, 0xA40), cell(10, { 0xF20 }, 0x0985, 1) }, "t", {}, 1.0f, 131, true));
		// ModA: a faction of its own (0x01000900) and a perk, aggression 2, another face (head part 0xA11), a template (both TPLT and
		// TPTA); weapon speed 2.0; armour rating 20 and another armature; projectile and damage; a book that teaches a perk; the
		// effect's magnitude; ingredient flag 0x2 (food); spell cost 20; cell lighting (ambient 20), a region, and its own previs
		NpcSpec na = n0;
		na.factions = { 0xA01, 0x01000900 };
		na.perks = { 0xA02, 0xA05 };
		na.aggression = 2;
		na.head = 0xA11;
		na.tplt = 0xA12;
		save("ModA.esp", merge::WritePlugin({ "Fallout4.esm" }, 0, { npc(na), weap(0xB00, 2.0f, 100, 0xB01, 0, 0), armo(20, { 0xC01 }, 0xC03), ammo(0xD01, 7),
			book(0x10, 0xD20, 10), alch(3, 0), ingr(0x2, 1), spel(20, 0xA04, 0xA40), cell(20, { 0xF20, 0xF21 }, 0x0986, 2),
			f4(MR("MISC", 0x01000900, { MS("EDID", { 'X', 0 }) })) }, "t", {}, 1.0f, 131, true));
		// ModB (light, the winner): an item and a keyword of its own (0x01000801), level 9, its name; ammo, a sound and the crit
		// effect of its own, value 200, flag 0x8000 (automatic); a keyword; value of the book; potion flag 0x10000 (medicine);
		// ingredient flag 0x100 and value 5; the spell's keyword condition on its own keyword and its own package; cell unchanged
		// but its own previs reference
		NpcSpec nb = n0;
		nb.items = { 0xA03, 0x01000801 };
		nb.keywords = { 0xA04, 0x01000801 };
		nb.level = 9;
		nb.name = 'B';
		save("ModB.esp", merge::WritePlugin({ "Fallout4.esm" }, 0x200, { npc(nb), weap(0x01000802, 1.0f, 200, 0x01000803, 0x8000, 0x01000804),
			armo(10, { 0xC01, 0x01000805 }, 0xC02), ammo(0xD00, 5), book(0, 0, 30), alch(1, 0x10000), ingr(0x100, 5), spel(10, 0x01000806, 0x01000807),
			cell(10, { 0xF20 }, 0x01000808, 3) }, "t", {}, 1.0f, 131, true));
		merge::Options o;
		o.loadOrder = { "Fallout4.esm", "ModA.esp", "ModB.esp" };
		o.findPlugin = [&](const std::string& n) -> std::optional<fs::path> { return dir / n; };
		const auto r = merge::Build(o);
		for (const auto& sk : r.skipped) std::printf("  merge FO4 types: not merged: %s\n", sk.c_str());
		std::set<std::string> types;
		for (const auto& m : r.merged) types.insert(m.type);
		Check(r.ok && r.skipped.empty() && types == std::set<std::string>({ "NPC_", "WEAP", "ARMO", "AMMO", "BOOK", "ALCH", "INGR", "SPEL", "CELL" }),
			"merge FO4 types: NPC_, WEAP, ARMO, AMMO, BOOK, ALCH, INGR, SPEL, CELL merged, nothing left out");
		Check(r.masters == std::vector<std::string>({ "Fallout4.esm", "ModA.esp", "ModB.esp" }), "merge FO4 types: masters Fallout4, ModA, ModB");
		save("Patch.esp", r.file);
		const auto pf = merge::ReadPlugin(dir / "Patch.esp");
		auto find = [&](const char* sig) -> const merge::Record* {
			for (const auto& rec : pf.records) {
				if (!std::memcmp(rec.sig, sig, 4)) return &rec;
			}
			return nullptr;
		};
		auto all = [&](const merge::Record* a_r, const char* a_sig) {
			std::vector<const merge::Sub*> out;
			for (const auto& sb : a_r->subs) if (!std::memcmp(sb.sig, a_sig, 4)) out.push_back(&sb);
			return out;
		};
		auto one = [&](const merge::Record* a_r, const char* a_sig) -> std::vector<std::uint8_t> {
			auto v = all(a_r, a_sig);
			return v.empty() ? std::vector<std::uint8_t>{} : v[0]->data;
		};
		auto order = [&](const merge::Record* a_r) {
			std::string s;
			for (const auto& sb : a_r->subs) s += std::string(sb.sig, 4) + " ";
			return s;
		};
		if (const auto* n = find("NPC_")) {
			std::vector<std::uint32_t> fac, perks, items, kws;
			for (auto* sb : all(n, "SNAM")) fac.push_back(u32(sb->data, 0));
			for (auto* sb : all(n, "PRKR")) perks.push_back(u32(sb->data, 0));
			for (auto* sb : all(n, "CNTO")) items.push_back(u32(sb->data, 0));
			const auto kwda = one(n, "KWDA");
			for (std::size_t k = 0; k < kwda.size(); k += 4) kws.push_back(u32(kwda, k));
			Check(fac == std::vector<std::uint32_t>({ 0xA01, 0x01000900 }) && perks == std::vector<std::uint32_t>({ 0xA02, 0xA05 }) &&
					  u32(one(n, "PRKZ"), 0) == 2 && items == std::vector<std::uint32_t>({ 0xA03, 0x02000801 }) && u32(one(n, "COCT"), 0) == 2 &&
					  kws == std::vector<std::uint32_t>({ 0xA04, 0x02000801 }) && u32(one(n, "KSIZ"), 0) == 2,
				"merge FO4 types: NPC factions (ModA's own), perks, items and keywords (ModB's own, renumbered) merged with their counts");
			const auto ac = one(n, "ACBS");
			Check(ac.size() == 20 && u16(ac, 6) == 9 && u16(ac, 14) == 1 && u32(one(n, "TPLT"), 0) == 0xA12 && u32(one(n, "TPTA"), 0) == 0xA12,
				"merge FO4 types: NPC level from ModB (ACBS @6), template flags (@14) with TPLT and TPTA from ModA");
			Check(one(n, "AIDT").size() == 24 && one(n, "AIDT")[0] == 2 && u32(one(n, "PNAM"), 0) == 0xA11 && all(n, "FULL").back()->data == std::vector<std::uint8_t>({ 'B', 0 }),
				"merge FO4 types: NPC aggression and face from ModA, name from ModB");
			// the object template came through whole: its own FULL, the OBTS forms where they were
			const auto obts = one(n, "OBTS");
			Check(all(n, "FULL").size() == 2 && obts.size() == 18 + 4 + 7 + 24 && u32(obts, 16) == 0xA60 && u32(obts, 22) == 0xA70 && u32(obts, 29 + 12) == 0xA80,
				"merge FO4 types: NPC object template (OBTE..STOP) kept whole, with its own name");
			Check(order(n).find("ACBS SNAM SNAM TPLT TPTA RNAM ATKD ATKE PRKZ PRKR PRKR PRPS COCT CNTO CNTO AIDT PKID KSIZ KWDA APPR OBTE OBTF FULL OBTS STOP CNAM FULL") != std::string::npos,
				"merge FO4 types: NPC subrecords in Fallout 4's order");
		} else {
			Check(false, "merge FO4 types: NPC in the patch");
		}
		if (const auto* w = find("WEAP")) {
			const auto d = one(w, "DNAM");
			float speed = 0;
			if (d.size() == 132) std::memcpy(&speed, d.data() + 4, 4);
			Check(d.size() == 132 && u32(d, 0) == 0x02000802 && speed == 2.0f && u32(d, 63) == 200 && u32(d, 73) == 0x02000803 && u32(d, 48) == 0x8000 &&
					  u32(d, 40) == 0xB10 && u32(one(w, "CRDT"), 8) == 0x02000804 && u32(one(w, "FNAM"), 29) == 0xB20,
				"merge FO4 types: weapon DNAM field by field (ModB's ammo, value, sound and flag, ModA's speed), forms renumbered");
			// ModB's own ammo is the object template property's form too
			const auto obts = one(w, "OBTS");
			Check(obts.size() == 53 && u32(obts, 41) == 0x02000802, "merge FO4 types: weapon object template property form renumbered");
		} else {
			Check(false, "merge FO4 types: weapon in the patch");
		}
		if (const auto* a = find("ARMO")) {
			const auto kwda = one(a, "KWDA");
			Check(u16(one(a, "FNAM"), 0) == 20 && u32(one(a, "MODL"), 0) == 0xC03 && kwda.size() == 8 && u32(kwda, 4) == 0x02000805 && all(a, "MODC").size() == 2,
				"merge FO4 types: armour rating and armature from ModA, ModB's keyword, both world models' MODC kept");
			Check(order(a) == "EDID OBND FULL MOD2 MO2T MODC MOD4 MO4T MODC BOD2 RNAM KSIZ KWDA DESC INDX MODL DATA FNAM DAMA OBTE OBTF FULL OBTS STOP ",
				"merge FO4 types: armour subrecords in place");
		} else {
			Check(false, "merge FO4 types: armour in the patch");
		}
		if (const auto* m = find("AMMO")) {
			const auto d = one(m, "DNAM");
			float dmg = 0;
			if (d.size() == 16) std::memcpy(&dmg, d.data() + 8, 4);
			Check(u32(d, 0) == 0xD01 && dmg == 7.0f, "merge FO4 types: ammunition projectile and damage from ModA");
		} else {
			Check(false, "merge FO4 types: ammunition in the patch");
		}
		if (const auto* b = find("BOOK")) {
			const auto d = one(b, "DNAM");
			Check(d.size() == 13 && d[0] == 0x10 && u32(d, 1) == 0xD20 && u32(one(b, "DATA"), 0) == 30, "merge FO4 types: book teaches ModA's perk, value from ModB");
		} else {
			Check(false, "merge FO4 types: book in the patch");
		}
		if (const auto* p = find("ALCH")) {
			float mag = 0;
			const auto efit = one(p, "EFIT");
			if (efit.size() == 12) std::memcpy(&mag, efit.data(), 4);
			Check(mag == 3.0f && u32(one(p, "ENIT"), 4) == 0x10000 && all(p, "CTDA").size() == 1, "merge FO4 types: potion effects from ModA, flags from ModB");
		} else {
			Check(false, "merge FO4 types: potion in the patch");
		}
		if (const auto* i = find("INGR")) {
			Check(u32(one(i, "ENIT"), 4) == 0x102 && u32(one(i, "DATA"), 0) == 5, "merge FO4 types: ingredient flags merged bit by bit, value from ModB");
		} else {
			Check(false, "merge FO4 types: ingredient in the patch");
		}
		if (const auto* s = find("SPEL")) {
			const auto c = all(s, "CTDA");
			Check(u32(one(s, "SPIT"), 0) == 20 && c.size() == 3 && u32(c[0]->data, 12) == 0x02000806 && u32(c[1]->data, 12) == 0x01000005 &&
					  u32(c[2]->data, 12) == 0x02000807,
				"merge FO4 types: spell cost from ModA; conditions: ModB's keyword and package renumbered, an alias number kept");
		} else {
			Check(false, "merge FO4 types: spell in the patch");
		}
		if (const auto* c = find("CELL")) {
			const auto xclr = one(c, "XCLR");
			Check(one(c, "XCLL").size() == 136 && one(c, "XCLL")[0] == 20 && xclr.size() == 8 && u32(xclr, 4) == 0xF21,
				"merge FO4 types: cell lighting and region from ModA");
			Check(u16(one(c, "VISI"), 0) == 3 && u32(one(c, "XPRI"), 0) == 0x02000808 && u32(one(c, "XCRI"), 12) == 0x02000808,
				"merge FO4 types: cell previs and precombined data stay the winner's (ModB's), its references renumbered");
		} else {
			Check(false, "merge FO4 types: cell in the patch");
		}
		// a weapon DNAM of another size than xEdit's 132 bytes: left to the winner, with the reason
		{
			auto bad = weap(0xB00, 1.0f, 100, 0xB01, 0, 0);
			for (auto& sb : bad.subs) if (!std::memcmp(sb.sig, "DNAM", 4)) sb.data.resize(130);
			auto badB = bad;
			for (auto& sb : badB.subs) if (!std::memcmp(sb.sig, "EDID", 4)) sb.data = { 'Z', 0 };
			save("Fallout4.esm", merge::WritePlugin({}, 1, { weap(0xB00, 1.0f, 100, 0xB01, 0, 0) }, "t", {}, 1.0f, 131, true));
			save("ModA.esp", merge::WritePlugin({ "Fallout4.esm" }, 0, { bad }, "t", {}, 1.0f, 131, true));
			save("ModB.esp", merge::WritePlugin({ "Fallout4.esm" }, 0x200, { badB }, "t", {}, 1.0f, 131, true));
			const auto r2 = merge::Build(o);
			Check(r2.ok && r2.merged.empty() && r2.skipped.size() == 1 && r2.skipped[0].find("DNAM has a size (130 bytes)") != std::string::npos,
				"merge FO4 types: a weapon DNAM of an unknown size is left to the winner, with the reason");
		}
		fs::remove_all(dir);
	}

	// Real Fallout 4 plugins (RESAVERPP_FO4_PLUGINS="file;file"): every record of the merged types the plugin defines is
	// overridden by two test plugins (ModA adds a list entry, ModB renames it), so each real record is read with the Fallout 4
	// layouts and merged; nothing may be left out for an unknown part, and the patch must read back.
	void TestMergedPatchFO4Real()
	{
		const char* env = std::getenv("RESAVERPP_FO4_PLUGINS");
		if (!env || !*env) {
			std::printf("(merge FO4: RESAVERPP_FO4_PLUGINS not set, real plugins skipped)\n");
			return;
		}
		namespace fs = std::filesystem;
		std::string list = env;
		for (std::size_t a = 0; a <= list.size();) {
			auto b = list.find(';', a);
			if (b == std::string::npos) b = list.size();
			const fs::path file = list.substr(a, b - a);
			a = b + 1;
			if (file.empty()) continue;
			const auto top = fs::temp_directory_path() / "rs_merge_test_fo4real";
			const auto dir = top / "mods" / "Test";
			fs::remove_all(top);
			fs::create_directories(dir);
			auto save = [&](const std::string& a_name, const std::vector<std::uint8_t>& a_data) {
				std::ofstream(dir / a_name, std::ios::binary).write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
			};
			const auto name = file.filename().string();
			const auto pf = merge::ReadPlugin(file, merge::MergedTypes(true));
			fs::copy_file(file, dir / name);
			// its strings files are in its archives ("<plugin> - Main.ba2"...): linked next to it
			const auto stem = file.stem().string();
			std::string stemLow = stem;
			for (auto& c : stemLow) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			{
				std::error_code ec;
				for (const auto& e : fs::directory_iterator(file.parent_path(), ec)) {
					const auto fn = e.path().filename().string();
					if (fn.rfind(stem + " - ", 0) == 0 && fn.ends_with(".ba2")) fs::create_symlink(e.path(), dir / fn, ec);
				}
			}
			for (const auto& m : pf.masters) {
				save(m, merge::WritePlugin({}, 1, {}, "t", {}, 1.0f, 131, true));
			}
			const auto own = static_cast<std::uint32_t>(pf.masters.size());
			std::vector<merge::Record> a2, b2;
			std::size_t lists = 0, forcedRecs = 0, worlds = 0;
			// what ModA changed in each record: 'L' a list entry added, 'O' object bounds, 'D' a cell flag (DATA bit 0x10, unused)
			std::map<std::uint32_t, char> forced;
			std::set<std::uint32_t> parents;  // worldspaces of the plugin's own exterior cells
			for (const auto& r : pf.records) {
				if ((r.formID >> 24) != own || (r.flags & 0x20)) continue;
				if (!std::memcmp(r.sig, "CELL", 4) && !r.groups.empty() && r.groups[0].first == 1) parents.insert(r.groups[0].second);
			}
			for (const auto& r : pf.records) {
				if (!std::memcmp(r.sig, "WRLD", 4) && (r.formID >> 24) != own && parents.contains(r.formID)) {
					// a master's worldspace holding cells of this plugin: in the test plugins unchanged, as the cells' parent
					a2.push_back(r);
					b2.push_back(r);
					continue;
				}
				if ((r.formID >> 24) != own || (r.flags & 0x20)) continue;  // overrides of a master (not loaded here), deleted ones
				merge::Record ra = r, rb = r;
				const bool world = !std::memcmp(r.sig, "WRLD", 4);
				// ModA: one more entry in the list (a copy of the first one, at level 99 for leveled lists)
				const char* entry = nullptr;
				for (const char* e : { "LVLO", "CNTO", "LNAM" }) {
					for (const auto& sb : r.subs) entry = !entry && !std::memcmp(sb.sig, e, 4) ? e : entry;
				}
				for (const auto& sb : r.subs) {
					if (entry && !std::memcmp(sb.sig, "LLCT", 4) && !sb.data.empty() && sb.data[0] == 255) entry = nullptr;  // full: no room
				}
				if (entry) {
					for (std::size_t i = 0; i < ra.subs.size(); ++i) {
						if (std::memcmp(ra.subs[i].sig, entry, 4) != 0) continue;
						auto copy = ra.subs[i];
						if (!std::strcmp(entry, "LVLO")) {
							copy.data[0] = 99;
							copy.data[1] = 0;
						}
						ra.subs.insert(ra.subs.begin() + static_cast<std::ptrdiff_t>(i) + 1, copy);
						break;
					}
					for (auto& sb : ra.subs) {
						if (!std::memcmp(sb.sig, "LLCT", 4)) ++sb.data[0];
						if (!std::memcmp(sb.sig, "COCT", 4) && sb.data.size() == 4) {
							// a u32: DLCRobot.esm's containers 0x010024/0x010025 hold 320 and 327 items
							std::uint32_t v;
							std::memcpy(&v, sb.data.data(), 4);
							++v;
							std::memcpy(sb.data.data(), &v, 4);
						}
					}
					++lists;
					forced[r.formID] = 'L';
				} else if (!world) {
					// no list: ModA changes one value, the object bounds (or a cell's unused flag 0x10)
					for (auto& sb : ra.subs) {
						if (!std::memcmp(sb.sig, "OBND", 4) && sb.data.size() == 12 && !forced.contains(r.formID)) {
							sb.data[0] ^= 1;
							forced[r.formID] = 'O';
						}
						if (!std::memcmp(r.sig, "CELL", 4) && !std::memcmp(sb.sig, "DATA", 4) && !sb.data.empty() && !forced.contains(r.formID)) {
							sb.data[0] ^= 0x10;
							forced[r.formID] = 'D';
						}
					}
				}
				forcedRecs += forced.contains(r.formID) ? 1 : 0;
				worlds += world ? 1 : 0;
				for (auto& sb : rb.subs) {
					if (!std::memcmp(sb.sig, "EDID", 4) && !sb.data.empty()) sb.data.insert(sb.data.end() - 1, 'X');
				}
				a2.push_back(std::move(ra));
				b2.push_back(std::move(rb));
			}
			auto masters = pf.masters;
			masters.push_back(name);
			// localized like the plugin (its string ids, its strings files)
			save("ModA.esp", merge::WritePlugin(masters, pf.flags & 0x80, a2, "t", {}, 1.0f, 131, true));
			save("ModB.esp", merge::WritePlugin(masters, pf.flags & 0x80, b2, "t", {}, 1.0f, 131, true));
			merge::Options o;
			o.loadOrder = masters;
			o.loadOrder.push_back("ModA.esp");
			o.loadOrder.push_back("ModB.esp");
			ScriptSources src;
			src.scan(top / "mods");
			merge::UseSources(o, src);
			auto strings = o.readStrings;
			// without the plugin's archives (its strings files): a .strings file made up from the string ids its records use,
			// so the rest of the test still runs ("S<id>" for each)
			std::vector<std::uint8_t> madeUp;
			if ((pf.flags & 0x80) && strings(stemLow + "_en.strings").empty()) {
				std::set<std::uint32_t> ids;
				for (const auto& rec : pf.records) {
					for (const auto& sb : rec.subs) {
						// the localized texts of the merged types (wbDefinitionsFO4 wbLStringKC): FULL, DESC, NPC_ SHRT and ATTX, LVLI
						// and AMMO ONAM, BOOK CNAM, ALCH DNAM
						const std::string k = std::string(rec.sig, 4) + "." + std::string(sb.sig, 4);
						if (sb.data.size() == 4 && (!std::memcmp(sb.sig, "FULL", 4) || !std::memcmp(sb.sig, "DESC", 4) || k == "NPC_.SHRT" ||
													   k == "NPC_.ATTX" || k == "LVLI.ONAM" || k == "AMMO.ONAM" || k == "BOOK.CNAM" || k == "ALCH.DNAM")) {
							std::uint32_t id;
							std::memcpy(&id, sb.data.data(), 4);
							if (id) ids.insert(id);
						}
					}
				}
				std::vector<std::uint8_t> dirs, data;
				for (const auto id : ids) {
					auto d = U32v({ id, static_cast<std::uint32_t>(data.size()) });
					dirs.insert(dirs.end(), d.begin(), d.end());
					const auto t = "S" + std::to_string(id);
					data.insert(data.end(), t.begin(), t.end());
					data.push_back(0);
				}
				madeUp = U32v({ static_cast<std::uint32_t>(ids.size()), static_cast<std::uint32_t>(data.size()) });
				madeUp.insert(madeUp.end(), dirs.begin(), dirs.end());
				madeUp.insert(madeUp.end(), data.begin(), data.end());
				std::printf("  merge FO4 real %s: its strings files are not here, %zu made-up names used\n", name.c_str(), ids.size());
			}
			o.readStrings = [&, strings](const std::string& a_file) {
				auto f = a_file;
				if (f.rfind("moda_", 0) == 0 || f.rfind("modb_", 0) == 0) f = stemLow + f.substr(4);
				if (!madeUp.empty()) return f == stemLow + "_en.strings" ? madeUp : std::vector<std::uint8_t>{};
				return strings(f);
			};
			const auto r = merge::Build(o);
			// a partial form (cells) is left to the game on purpose; anything else not merged is a part the layouts do not know
			std::size_t partial = 0, unknown = 0;
			for (const auto& sk : r.skipped) {
				if (sk.find("partial form") != std::string::npos) {
					++partial;
					continue;
				}
				++unknown;
				std::printf("  merge FO4 real %s: not merged: %s\n", name.c_str(), sk.c_str());
			}
			std::map<std::string, std::size_t> byType;
			for (const auto& m : r.merged) ++byType[m.type];
			std::string types;
			for (const auto& [t, n] : byType) types += " " + t + " " + std::to_string(n);
			std::printf("  merge FO4 real %s: %zu records of the merged types (%zu worldspaces), %zu with a list, %zu forced changes, %zu conflicts, "
						"%zu merged (%s ), %zu not merged (%zu partial cells)\n",
				name.c_str(), a2.size(), worlds, lists, forcedRecs, r.conflicts, r.merged.size(), types.c_str(), r.skipped.size(), partial);
			Check(r.ok && r.fo4 && unknown == 0 && r.merged.size() + partial == forcedRecs,
				"merge FO4 real: every record of a real Fallout 4 plugin read with the FO4 layouts, ModA's change merged into each");
			save("Patch.esp", r.file);
			const auto back = merge::ReadPlugin(dir / "Patch.esp");
			std::size_t backRecs = 0;
			for (const auto& rec : back.records) backRecs += std::memcmp(rec.sig, "WRLD", 4) ? 1 : 0;
			bool good = backRecs == r.merged.size();
			if (!good) {
				std::printf("  merge FO4 real %s: the patch has %zu records, %zu expected\n", name.c_str(), backRecs, r.merged.size());
			}
			std::map<std::uint32_t, const merge::Record*> orig;
			for (const auto& rec : pf.records) orig[rec.formID] = &rec;
			std::size_t bad = 0;
			for (const auto& rec : back.records) {
				if (!std::memcmp(rec.sig, "WRLD", 4)) continue;
				// ModB's edits (the winner) and ModA's added entry, the counts matching
				std::size_t lvlo = 0, cnto = 0;
				long long llct = -1, coct = -1;
				bool hasEdid = false, renamed = false, lvl99 = false;
				for (const auto& sb : rec.subs) {
					if (!std::memcmp(sb.sig, "LVLO", 4)) { ++lvlo; lvl99 = lvl99 || sb.data[0] == 99; }
					if (!std::memcmp(sb.sig, "CNTO", 4)) ++cnto;
					if (!std::memcmp(sb.sig, "LLCT", 4)) llct = sb.data[0];
					if (!std::memcmp(sb.sig, "COCT", 4) && sb.data.size() == 4) {
						std::uint32_t v;
						std::memcpy(&v, sb.data.data(), 4);
						coct = v;
					}
					if (!std::memcmp(sb.sig, "EDID", 4)) {
						hasEdid = true;
						renamed = sb.data.size() >= 2 && sb.data[sb.data.size() - 2] == 'X';
					}
				}
				std::string why;
				if (hasEdid && !renamed) why += " ModB's new editor ID is missing;";
				if (lvlo && static_cast<long long>(lvlo) != llct) why += " LLCT " + std::to_string(llct) + " but " + std::to_string(lvlo) + " LVLO;";
				if (lvlo && !lvl99) why += " ModA's level-99 entry is missing;";
				if (cnto && static_cast<long long>(cnto) != coct) why += " COCT " + std::to_string(coct) + " but " + std::to_string(cnto) + " CNTO;";
				// ModA's value change (its own form IDs are the patch's last-but-one master, so compare by local ID)
				const auto low = (rec.formID & 0xFFFFFF) | (own << 24);
				const auto f = forced.find(low);
				const auto* o0 = orig.contains(low) ? orig[low] : nullptr;
				if (f != forced.end() && o0 && f->second != 'L') {
					const char* sig = f->second == 'O' ? "OBND" : "DATA";
					const std::uint8_t mask = f->second == 'O' ? 1 : 0x10;
					std::optional<std::uint8_t> was, now;
					for (const auto& sb : o0->subs) if (!was && !std::memcmp(sb.sig, sig, 4) && !sb.data.empty()) was = sb.data[0];
					for (const auto& sb : rec.subs) if (!now && !std::memcmp(sb.sig, sig, 4) && !sb.data.empty()) now = sb.data[0];
					if (!was || !now || *now != (*was ^ mask)) why += std::string(" ModA's ") + sig + " change is missing;";
				}
				// the patch must hold the same subrecords as the plugin's record (ModB's editor ID aside), in the same order
				if (o0) {
					std::string a, b;
					for (const auto& sb : o0->subs) a.append(sb.sig, 4);
					for (const auto& sb : rec.subs) b.append(sb.sig, 4);
					if (f == forced.end() || f->second != 'L') {
						if (a != b) why += " its subrecords are not the plugin's: " + a.substr(0, 200) + " / " + b.substr(0, 200) + ";";
					}
				}
				if (!why.empty()) {
					good = false;
					if (++bad <= 20) {
						std::printf("  merge FO4 real %s: %.4s %08X:%s\n", name.c_str(), rec.sig, rec.formID, why.c_str());
					}
				}
			}
			if (bad > 20) std::printf("  merge FO4 real %s: ... %zu records fail in all\n", name.c_str(), bad);
			Check(good, "merge FO4 real: the patch reads back, winner's edits kept, ModA's entries added, counts right");
			fs::remove_all(top);
		}
	}

	// NPC parts that belong together are taken from one plugin (PC level mult + level; template flags + TPLT)
	void TestMergedPatchNpcLinks()
	{
		namespace fs = std::filesystem;
		const auto dir = fs::temp_directory_path() / "rs_merge_test_npclink";
		fs::remove_all(dir);
		fs::create_directories(dir);
		auto save = [&](const char* a_name, const std::vector<std::uint8_t>& a_data) {
			std::ofstream(dir / a_name, std::ios::binary).write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
		};
		// ACBS: flags @0, level @8, min @10, max @12, speed @14, template flags @18
		auto acbs = [](std::uint32_t flags, std::uint16_t level, std::uint16_t mn, std::uint16_t mx, std::uint16_t speed, std::uint16_t tfl) {
			std::vector<std::uint8_t> a(24, 0);
			std::memcpy(a.data(), &flags, 4);
			std::memcpy(a.data() + 8, &level, 2);
			std::memcpy(a.data() + 10, &mn, 2);
			std::memcpy(a.data() + 12, &mx, 2);
			std::memcpy(a.data() + 14, &speed, 2);
			std::memcpy(a.data() + 18, &tfl, 2);
			return a;
		};
		auto npc = [&](std::vector<std::uint8_t> a_acbs, std::optional<std::uint32_t> a_tplt) {
			std::vector<merge::Sub> subs{ MS("EDID", { 'N', 0 }), MS("ACBS", std::move(a_acbs)) };
			if (a_tplt) subs.push_back(MS("TPLT", U32v({ *a_tplt })));
			subs.push_back(MS("RNAM", U32v({ 0x980 })));
			return MR("NPC_", 0x820, std::move(subs));
		};
		// base: level 10, no PC level mult, no template
		save("Skyrim.esm", merge::WritePlugin({}, 1, { npc(acbs(0, 10, 0, 0, 100, 0), std::nullopt) }, "t"));
		// A: makes it PC level mult x1.0 (1000) with min 5 max 50 (level block), and uses template 0x830 for traits (template block)
		save("ModA.esp", merge::WritePlugin({ "Skyrim.esm" }, 0, { npc(acbs(0x80, 1000, 5, 50, 100, 0x0001), 0x830u) }, "t"));
		// B (made against the base only): a plain level 30, and a faster speed
		save("ModB.esp", merge::WritePlugin({ "Skyrim.esm" }, 0, { npc(acbs(0, 30, 0, 0, 120, 0), std::nullopt) }, "t"));
		merge::Options o;
		o.loadOrder = { "Skyrim.esm", "ModA.esp", "ModB.esp" };
		o.findPlugin = [&](const std::string& n) -> std::optional<fs::path> { return dir / n; };
		const auto r = merge::Build(o);
		Check(r.ok && r.merged.size() == 1, "merge npc links: the NPC is merged");
		save("Patch.esp", r.file);
		const auto pf = merge::ReadPlugin(dir / "Patch.esp");
		std::uint32_t flags = 0;
		std::uint16_t level = 0, mn = 0, mx = 0, speed = 0, tfl = 0;
		bool tplt = false;
		for (const auto& rec : pf.records) {
			if (std::memcmp(rec.sig, "NPC_", 4) != 0) continue;
			for (const auto& sb : rec.subs) {
				if (std::memcmp(sb.sig, "ACBS", 4) == 0 && sb.data.size() == 24) {
					std::memcpy(&flags, sb.data.data(), 4);
					std::memcpy(&level, sb.data.data() + 8, 2);
					std::memcpy(&mn, sb.data.data() + 10, 2);
					std::memcpy(&mx, sb.data.data() + 12, 2);
					std::memcpy(&speed, sb.data.data() + 14, 2);
					std::memcpy(&tfl, sb.data.data() + 18, 2);
				}
				if (std::memcmp(sb.sig, "TPLT", 4) == 0) tplt = true;
			}
		}
		// the level block comes whole from B (the last to change it): plain level 30, no PC level mult, no min/max.
		// Merged value by value it was "PC level mult with level 30" = a 0.03x multiplier.
		Check(!(flags & 0x80) && level == 30 && mn == 0 && mx == 0, "merge npc links: PC level mult and level taken from one plugin");
		// B did not touch the template: A's template flags and TPLT stay, together
		Check(tfl == 0x0001 && tplt, "merge npc links: template flags and template kept together");
		// fields that are independent still merge value by value
		Check(speed == 120, "merge npc links: an unrelated value (speed) still merges");
		fs::remove_all(dir);
	}

	void TestMergedPatch()
	{
		namespace fs = std::filesystem;
		const auto dir = fs::temp_directory_path() / "rs_merge_test";
		fs::remove_all(dir);
		fs::create_directories(dir);
		auto save = [&](const char* a_name, const std::vector<std::uint8_t>& a_data) {
			std::ofstream(dir / a_name, std::ios::binary).write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
		};
		// Base.esm: a leveled list with items 0x900 (A) and 0x901 (B); a container with A x1; an armour with keyword 0x950;
		// an NPC in faction 0x960 with spell 0x970
		auto npc = [](std::vector<merge::Sub> a_extra) {
			std::vector<merge::Sub> subs{ MS("EDID", { 'N', 0 }), MS("ACBS", std::vector<std::uint8_t>(24, 0)) };
			subs.insert(subs.end(), a_extra.begin(), a_extra.end());
			subs.push_back(MS("RNAM", U32v({ 0x980 })));
			subs.push_back(MS("AIDT", std::vector<std::uint8_t>(20, 0)));
			subs.push_back(MS("CNAM", U32v({ 0x990 })));
			subs.push_back(MS("DATA", {}));
			return MR("NPC_", 0x800, subs);
		};
		save("Base.esm", merge::WritePlugin({}, 1, {
			Leveled(0x810, { 0x900, 0x901 }),
			MR("CONT", 0x820, { MS("EDID", { 'C', 0 }), MS("COCT", U32v({ 1 })), MS("CNTO", U32v({ 0x900, 1 })), MS("DATA", { 0, 0, 0, 0, 0 }) }),
			MR("ARMO", 0x830, { MS("EDID", { 'A', 0 }), MS("KSIZ", U32v({ 1 })), MS("KWDA", U32v({ 0x950 })), MS("DATA", U32v({ 10, 0 })), MS("DNAM", U32v({ 0 })) }),
			npc({ MS("SNAM", U32v({ 0x960, 0 })), MS("SPCT", U32v({ 1 })), MS("SPLO", U32v({ 0x970 })) }),
		}));
		// ModA.esp (master Base): adds C (0x902) to the list, removes B; container A x5; adds keyword 0x951
		save("ModA.esp", merge::WritePlugin({ "Base.esm" }, 0, {
			Leveled(0x810, { 0x900, 0x902 }),
			MR("CONT", 0x820, { MS("EDID", { 'C', 0 }), MS("COCT", U32v({ 1 })), MS("CNTO", U32v({ 0x900, 5 })), MS("DATA", { 0, 0, 0, 0, 0 }) }),
			MR("ARMO", 0x830, { MS("EDID", { 'A', 0 }), MS("KSIZ", U32v({ 2 })), MS("KWDA", U32v({ 0x950, 0x951 })), MS("DATA", U32v({ 10, 0 })), MS("DNAM", U32v({ 0 })) }),
			npc({ MS("SNAM", U32v({ 0x960, 0 })), MS("SNAM", U32v({ 0x961, 0 })), MS("SPCT", U32v({ 1 })), MS("SPLO", U32v({ 0x970 })) }),
		}));
		// ModB.esp (master Base): adds D (0x903); container adds 0x904 x2; armour value changed (winner's value stays)
		save("ModB.esp", merge::WritePlugin({ "Base.esm" }, 0, {
			Leveled(0x810, { 0x900, 0x901, 0x903 }),
			MR("CONT", 0x820, { MS("EDID", { 'C', 0 }), MS("COCT", U32v({ 2 })), MS("CNTO", U32v({ 0x900, 1 })), MS("CNTO", U32v({ 0x904, 2 })), MS("DATA", { 0, 0, 0, 0, 0 }) }),
			MR("ARMO", 0x830, { MS("EDID", { 'A', 0 }), MS("KSIZ", U32v({ 1 })), MS("KWDA", U32v({ 0x950 })), MS("DATA", U32v({ 99, 0 })), MS("DNAM", U32v({ 0 })) }),
			npc({ MS("SNAM", U32v({ 0x960, 0 })), MS("SPCT", U32v({ 2 })), MS("SPLO", U32v({ 0x970 })), MS("SPLO", U32v({ 0x971 })) }),
		}));
		// ModC.esp (masters Base, ModA): adds its own new item E (0x01000800 = ModC's 0x800) to the list built on ModA's version
		save("ModC.esp", merge::WritePlugin({ "Base.esm", "ModA.esp" }, 0, {
			MR("MISC", 0x02000800, { MS("EDID", { 'E', 0 }) }),
			Leveled(0x810, { 0x900, 0x902, 0x02000800 }),
		}));
		merge::Options o;
		o.loadOrder = { "Base.esm", "ModA.esp", "ModB.esp", "ModC.esp" };
		o.findPlugin = [&](const std::string& n) -> std::optional<fs::path> { return dir / n; };
		const auto r = merge::Build(o);
		Check(r.ok, "merge: patch built");
		Check(r.conflicts == 4 && r.merged.size() == 4, "merge: four conflicting records, all merged");
		Check(r.masters == std::vector<std::string>{ "Base.esm", "ModC.esp" }, "merge: masters are only Base.esm and ModC.esp (the plugin of the new item)");
		save("Patch.esp", r.file);
		const auto pf = merge::ReadPlugin(dir / "Patch.esp");
		Check(pf.flags == 0x200 && pf.masters.size() == 2, "merge: patch is light-flagged with two masters");
		auto find = [&](const char* sig) -> const merge::Record* {
			for (const auto& rec : pf.records) {
				if (std::memcmp(rec.sig, sig, 4) == 0) return &rec;
			}
			return nullptr;
		};
		auto list = [&](const merge::Record* rec, const char* sig, std::size_t off) {
			std::vector<std::uint32_t> v;
			for (const auto& s : rec->subs) {
				if (std::memcmp(s.sig, sig, 4) == 0) {
					std::uint32_t x;
					std::memcpy(&x, s.data.data() + off, 4);
					v.push_back(x);
				}
			}
			return v;
		};
		const auto* lv = find("LVLI");
		Check(lv && list(lv, "LVLO", 4) == std::vector<std::uint32_t>{ 0x900, 0x902, 0x01000800, 0x903 } && lv->subs[3].data[0] == 4,
			"merge: leveled list has A, C, E (as the patch's master 1) and D; B removed; count 4");
		const auto* ct = find("CONT");
		Check(ct && list(ct, "CNTO", 0) == std::vector<std::uint32_t>{ 0x900, 0x904 } && list(ct, "CNTO", 4) == std::vector<std::uint32_t>{ 5, 2 },
			"merge: container keeps ModA's count 5 for A and ModB's new item");
		const auto* ar = find("ARMO");
		Check(ar && list(ar, "KWDA", 0) == std::vector<std::uint32_t>{ 0x950 } && list(ar, "KWDA", 4) == std::vector<std::uint32_t>{ 0x951 } &&
				  list(ar, "KSIZ", 0) == std::vector<std::uint32_t>{ 2 } && list(ar, "DATA", 0) == std::vector<std::uint32_t>{ 99 },
			"merge: armour gets ModA's keyword and keeps the winner's (ModB's) value");
		const auto* np = find("NPC_");
		Check(np && list(np, "SNAM", 0) == std::vector<std::uint32_t>{ 0x960, 0x961 } && list(np, "SPLO", 0) == std::vector<std::uint32_t>{ 0x970, 0x971 } &&
				  list(np, "SPCT", 0) == std::vector<std::uint32_t>{ 2 },
			"merge: NPC gets ModA's faction and ModB's spell");
		// a plugin made for an uncompacted light master: Hair.esp points at Light.esp's head part by its old number 0x1810;
		// Light.esp's own version of the NPC shows the new number 0x810 at the same place
		auto npc2 = [](std::uint32_t a_part) {
			return MR("NPC_", 0x800, { MS("EDID", { 'N', 0 }), MS("ACBS", std::vector<std::uint8_t>(24, 0)), MS("RNAM", U32v({ 0x980 })),
				MS("DATA", {}), MS("PNAM", U32v({ a_part })) });
		};
		save("Light.esp", merge::WritePlugin({ "Base.esm" }, 0x200, { MR("MISC", 0x01000810, { MS("EDID", { 'H', 0 }) }), npc2(0x01000810) }));
		save("Hair.esp", merge::WritePlugin({ "Base.esm", "Light.esp" }, 0x200, { npc2(0x01001810) }));
		merge::Options o2;
		o2.loadOrder = { "Base.esm", "Light.esp", "Hair.esp" };
		o2.findPlugin = o.findPlugin;
		const auto r2 = merge::Build(o2);
		save("Patch2.esp", r2.file);
		const auto pf2 = merge::ReadPlugin(dir / "Patch2.esp");
		const merge::Record* n2 = nullptr;
		for (const auto& rec : pf2.records) {
			if (std::memcmp(rec.sig, "NPC_", 4) == 0) n2 = &rec;
		}
		Check(r2.ok && r2.fixes.size() == 1 && r2.warnings.empty() && n2 && list(n2, "PNAM", 0) == std::vector<std::uint32_t>{ 0x01000810 } &&
				  pf2.masters == std::vector<std::string>{ "Base.esm", "Light.esp" },
			"merge: a reference to a compacted light master's old number is pointed at its new number");
		// stage 2, values: ModA makes the NPC essential (a flag bit) and gives it a new face; ModB (the winner) changes its
		// health offset and aggression. Both change the armour's value: the winner's stays.
		auto npc3 = [](std::uint32_t a_flags, std::uint16_t a_health, std::uint32_t a_part, std::uint8_t a_aggr, std::uint32_t a_value) {
			std::vector<std::uint8_t> acbs(24, 0), aidt(20, 0);
			std::memcpy(acbs.data(), &a_flags, 4);
			std::memcpy(acbs.data() + 20, &a_health, 2);
			aidt[0] = a_aggr;
			return std::vector<merge::Record>{
				MR("NPC_", 0x801, { MS("EDID", { 'M', 0 }), MS("ACBS", acbs), MS("RNAM", U32v({ 0x980 })), MS("AIDT", aidt), MS("DATA", {}),
									  MS("PNAM", U32v({ a_part })), MS("QNAM", std::vector<std::uint8_t>(12, static_cast<std::uint8_t>(a_part))) }),
				MR("ARMO", 0x831, { MS("EDID", { 'B', 0 }), MS("DATA", U32v({ a_value, 0 })), MS("DNAM", U32v({ 0 })) }) };
		};
		save("Base3.esm", merge::WritePlugin({}, 1, npc3(0x10, 0, 0x990, 0, 10)));
		save("A3.esp", merge::WritePlugin({ "Base3.esm" }, 0, npc3(0x12, 0, 0x991, 0, 20)));
		save("B3.esp", merge::WritePlugin({ "Base3.esm" }, 0, npc3(0x10, 50, 0x990, 2, 30)));
		merge::Options o3;
		o3.loadOrder = { "Base3.esm", "A3.esp", "B3.esp" };
		o3.findPlugin = o.findPlugin;
		const auto r3 = merge::Build(o3);
		save("Patch3.esp", r3.file);
		const auto pf3 = merge::ReadPlugin(dir / "Patch3.esp");
		const merge::Record *n3 = nullptr, *a3 = nullptr;
		for (const auto& rec : pf3.records) {
			if (std::memcmp(rec.sig, "NPC_", 4) == 0) n3 = &rec;
			if (std::memcmp(rec.sig, "ARMO", 4) == 0) a3 = &rec;
		}
		auto subOf = [](const merge::Record* r, const char* sig) -> const std::vector<std::uint8_t>* {
			for (const auto& sb : r->subs) {
				if (std::memcmp(sb.sig, sig, 4) == 0) return &sb.data;
			}
			return nullptr;
		};
		bool ok3 = r3.ok && n3 && !a3;  // the armour: only the winner's change counts, so it needs no patch
		if (ok3) {
			std::uint32_t flags;
			std::uint16_t health;
			std::memcpy(&flags, subOf(n3, "ACBS")->data(), 4);
			std::memcpy(&health, subOf(n3, "ACBS")->data() + 20, 2);
			ok3 = flags == 0x12 && health == 50 && (*subOf(n3, "AIDT"))[0] == 2 && list(n3, "PNAM", 0) == std::vector<std::uint32_t>{ 0x991 } &&
				  (*subOf(n3, "QNAM"))[0] == 0x91;
			std::string order;
			for (const auto& sb : n3->subs) order += std::string(sb.sig, 4) + " ";
			ok3 = ok3 && order == "EDID ACBS RNAM AIDT DATA PNAM QNAM ";
			if (!ok3) std::printf("order: %s\n", order.c_str());
		}
		if (!ok3) {
			std::printf("%s", r3.report().c_str());
			if (n3) for (const auto& sb : n3->subs) { std::printf("  %.4s:", sb.sig); for (auto b : sb.data) std::printf(" %02X", b); std::printf("\n"); }
		}
		// stage 3: a potion. ModA (masters Base4, Extra) gives it a conditioned effect pointing at Extra's keyword; ModB, the winner,
		// changes its weight. The patch has both, the condition's keyword pointing at Extra.esp.
		{
			auto ctda = [](std::uint32_t a_keyword) {
				std::vector<std::uint8_t> c(32, 0);
				const std::uint16_t       fn = 560;  // HasKeyword: parameter 1 is a keyword
				std::memcpy(c.data() + 8, &fn, 2);
				std::memcpy(c.data() + 12, &a_keyword, 4);
				return c;
			};
			auto potion = [&](float a_weight, std::uint32_t a_effect, std::uint32_t a_keyword) {
				std::vector<std::uint8_t> w(4), enit(20, 0);
				std::memcpy(w.data(), &a_weight, 4);
				std::vector<merge::Sub> subs{ MS("EDID", { 'P', 0 }), MS("DATA", w), MS("ENIT", enit), MS("EFID", U32v({ a_effect })),
					MS("EFIT", std::vector<std::uint8_t>(12, 0)) };
				if (a_keyword) subs.push_back(MS("CTDA", ctda(a_keyword)));
				return MR("ALCH", 0x840, subs);
			};
			save("Base4.esm", merge::WritePlugin({}, 1, { potion(1.0f, 0x850, 0) }));
			save("Extra.esm", merge::WritePlugin({}, 1, { MR("KYWD", 0x01000900, { MS("EDID", { 'K', 0 }) }) }));
			save("A4.esp", merge::WritePlugin({ "Base4.esm", "Extra.esm" }, 0, { potion(1.0f, 0x851, 0x01000900) }));
			save("B4.esp", merge::WritePlugin({ "Base4.esm" }, 0, { potion(2.0f, 0x850, 0) }));
			merge::Options o4;
			o4.loadOrder = { "Base4.esm", "Extra.esm", "A4.esp", "B4.esp" };
			o4.findPlugin = o.findPlugin;
			const auto r4 = merge::Build(o4);
			save("Patch4.esp", r4.file);
			const auto pf4 = merge::ReadPlugin(dir / "Patch4.esp");
			bool ok4 = r4.ok && pf4.records.size() == 1 && pf4.masters == std::vector<std::string>{ "Base4.esm", "Extra.esm" };
			if (ok4) {
				const auto* p4 = &pf4.records[0];
				float wgt = 0;
				std::memcpy(&wgt, subOf(p4, "DATA")->data(), 4);
				ok4 = wgt == 2.0f && list(p4, "EFID", 0) == std::vector<std::uint32_t>{ 0x851 } && list(p4, "CTDA", 12) == std::vector<std::uint32_t>{ 0x01000900 };
			}
			Check(ok4, "merge: a potion gets ModA's effect with its condition (keyword remapped to Extra.esm) and ModB's weight");
		}
		// cells: an interior cell (ModA's lighting, ModB's location) and an exterior one in a worldspace (ModA's region, ModB's water height)
		{
			auto lighting = [](std::uint8_t a_v) { return std::vector<std::uint8_t>(92, a_v); };
			auto f32 = [](float a_v) { std::vector<std::uint8_t> v(4); std::memcpy(v.data(), &a_v, 4); return v; };
			auto interior = [&](std::uint8_t a_light, std::uint32_t a_loc) {
				std::vector<merge::Sub> subs{ MS("EDID", { 'I', 0 }), MS("DATA", { 1, 0 }), MS("XCLL", lighting(a_light)) };
				if (a_loc) subs.push_back(MS("XLCN", U32v({ a_loc })));
				auto r = MR("CELL", 0x860, subs);
				r.groups = { { 2, 0 }, { 3, 6 } };
				return r;
			};
			auto exterior = [&](std::vector<std::uint32_t> a_regions, float a_water) {
				std::vector<merge::Sub> subs{ MS("DATA", { 2, 0 }), MS("XCLC", std::vector<std::uint8_t>(12, 0)), MS("XCLW", f32(a_water)) };
				if (!a_regions.empty()) {
					std::vector<std::uint8_t> v(a_regions.size() * 4);
					std::memcpy(v.data(), a_regions.data(), v.size());
					subs.push_back(MS("XCLR", v));
				}
				auto r = MR("CELL", 0x871, subs);
				r.groups = { { 1, 0x870 }, { 4, 0 }, { 5, 0 } };
				return r;
			};
			auto world = MR("WRLD", 0x870, { MS("EDID", { 'W', 0 }), MS("CNAM", U32v({ 0x880 })), MS("DATA", { 0 }), MS("OFST", { 1, 2, 3, 4 }) });
			save("Base5.esm", merge::WritePlugin({}, 1, { interior(1, 0), world, exterior({ 0x890 }, 0.0f) }));
			save("A5.esp", merge::WritePlugin({ "Base5.esm" }, 0, { interior(2, 0), world, exterior({ 0x890, 0x891 }, 0.0f) }));
			save("B5.esp", merge::WritePlugin({ "Base5.esm" }, 0, { interior(1, 0x8A0), world, exterior({ 0x890 }, -50.0f) }));
			merge::Options o5;
			o5.loadOrder = { "Base5.esm", "A5.esp", "B5.esp" };
			o5.findPlugin = o.findPlugin;
			const auto r5 = merge::Build(o5);
			save("Patch5.esp", r5.file);
			const auto pf5 = merge::ReadPlugin(dir / "Patch5.esp", { "CELL", "WRLD" });
			const merge::Record *ic = nullptr, *ec = nullptr, *w = nullptr;
			for (const auto& rec : pf5.records) {
				if (rec.formID == 0x860) ic = &rec;
				if (rec.formID == 0x871) ec = &rec;
				if (rec.formID == 0x870) w = &rec;
			}
			bool ok5 = r5.ok && r5.merged.size() == 2 && ic && ec && w;
			if (ok5) {
				float water = 0;
				std::memcpy(&water, subOf(ec, "XCLW")->data(), 4);
				ok5 = (*subOf(ic, "XCLL"))[0] == 2 && list(ic, "XLCN", 0) == std::vector<std::uint32_t>{ 0x8A0 } && ic->groups.size() == 2 &&
					  ic->groups[1].second == 6 && water == -50.0f && subOf(ec, "XCLR")->size() == 8 && ec->groups.size() == 3 &&
					  ec->groups[0].first == 1 && ec->groups[0].second == 0x870 && !subOf(w, "OFST") && subOf(w, "CNAM");
			}
			if (!ok5) {
				std::printf("%s\n", r5.report().c_str());
				for (const auto& rec : pf5.records) {
					std::printf("  %.4s %08X groups:", rec.sig, rec.formID);
					for (auto& g : rec.groups) std::printf(" (%d,%X)", g.first, g.second);
					for (auto& sb : rec.subs) std::printf(" %.4s[%zu]", sb.sig, sb.data.size());
					std::printf("\n");
				}
			}
			Check(ok5, "merge: cells (ModA's lighting and region, ModB's location and water) in their groups, worldspace copied in without OFST");
		}
		Check(ok3, "merge: values merged field by field (ModA's essential flag and face, ModB's health and aggression; winner's armour value)");
		fs::remove_all(dir);
	}
}

// crash log: a leveled actor's rebuilt base NPC (never in the save) leads to the actor, which is the likely cause
void TestCrashLog()
{
	const std::string text =
		"CRASH TIME: 2026-10-28 22:29:47\n"
		"Skyrim SSE v1.6.1170\n"
		"CrashLoggerSSE v1-16-0-0 Oct  7 2025 06:54:42\n"
		"\n"
		"Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF6C100889D SkyrimSE.exe+01D889D\tcmp rdx, [rax]\n"
		"\n"
		"PROBABLE CALL STACK:\n"
		"\t[ 0] 0x7FF6C100889D        SkyrimSE.exe+01D889D -> 14413+0x5D\tcmp rdx, [rax]\n"
		"\t[ 3] 0x7FF6C0FE72F8        SkyrimSE.exe+01B72F8 -> 13723+0x6E8\tmov rcx, rbx\n"
		"\t\tName: \"GuardFaction\"\n"
		"\t\tFormID: 0x00086EEE\n"
		"\t\tFormType: Faction (11)\n"
		"\n"
		"REGISTERS:\n"
		"\tR13 0x20E2A75EED0      (TESNPC*)\n"
		"\t\tFlags: 0x00000008 \n"
		"\t\tName: \"Whiterun Militia Warrior\"\n"
		"\t\tFormID: 0xFF000998\n"
		"\t\tFormType: NPC (43)\n"
		"\tR14 0x20E2AA2ACA0      (Character*)\n"
		"\t\tFlags: 0x00200008 \n"
		"\t\tFormID: 0xFF0040DF\n"
		"\t\tFormType: ActorCharacter (62)\n"
		"\t\tObject Reference: \n"
		"\t\tFormID: 0x00000000\n"
		"\n"
		"STACK:\n"
		"\t[RSP+10  ] 0x20E2AA2ACA0      (Character*)\n"
		"\t\tFormID: 0xFF0040DF\n"
		"\t\tFormType: ActorCharacter (62)\n"
		"\t[RSP+20  ] 0x20E2AA2ACA0      (Character*)\n"
		"\t\tName: \"Spirit\"\n"
		"\t\tFormID: 0xFF0040DE\n"
		"\t\tFormType: ActorCharacter (62)\n";
	const auto log = ess::crashlog::Parse(text);
	Check(log.ok && log.tool == "Crash Logger" && log.time == "2026-10-28 22:29:47", "crash log: tool and time read");
	Check(log.exception.find("EXCEPTION_ACCESS_VIOLATION") != std::string::npos, "crash log: exception line read");
	Check(log.forms.size() == 4, "crash log: four forms (the null form ID is skipped)");
	const ess::crashlog::Form* actor = nullptr;
	const ess::crashlog::Form* base = nullptr;
	const ess::crashlog::Form* spirit = nullptr;
	for (const auto& f : log.forms) {
		actor = f.id == 0xFF0040DF ? &f : actor;
		base = f.id == 0xFF000998 ? &f : base;
		spirit = f.id == 0xFF0040DE ? &f : spirit;
	}
	Check(actor && actor->hits == 2 && actor->atCrash && actor->name.empty() && actor->type == "ActorCharacter (62)",
		"crash log: the actor, at the crash, not given the NPC's name");
	Check(base && base->name == "Whiterun Militia Warrior" && base->type == "NPC (43)", "crash log: the base NPC with its name and type");
	Check(spirit && !spirit->atCrash, "crash log: a form only in the stack memory is not at the crash");
	Check(!log.forms.empty() && log.forms.back().id == 0xFF0040DE, "crash log: forms at the crash come first");

	ess::SaveFile save;
	auto achr = [](std::uint32_t a_created) {
		ess::ChangeForm c;
		c.refID.data = (2u << 22) | (a_created & 0x3FFFFF);
		c.typeField = static_cast<std::uint8_t>(ess::ChangeFormType::ACHR);
		return c;
	};
	save.changeForms.push_back(achr(0x40DF));
	save.changeForms.push_back(achr(0x40DE));
	save.reindexChangeForms();
	const std::vector<ess::crashlog::LeveledBase> leveled = { { 0xFF000998, (2u << 22) | 0x40DF, 0xFF0040DF, 0xEF001E0A } };
	const auto found = ess::crashlog::Match(log, save, leveled);
	std::size_t suspects = 0;
	bool        baseToActor = false, spiritNotSuspect = false, faction = false;
	for (const auto& f : found) {
		suspects += f.suspect ? 1 : 0;
		if (f.form.id == 0xFF000998) {
			baseToActor = f.kind == ess::crashlog::Kind::kRebuiltBase && f.target == 0xFF0040DF && f.refRaw == ((2u << 22) | 0x40DF) && f.suspect;
		}
		if (f.form.id == 0xFF0040DE) {
			spiritNotSuspect = f.kind == ess::crashlog::Kind::kChanged && !f.suspect;
		}
		if (f.form.id == 0x00086EEE) {
			faction = f.kind == ess::crashlog::Kind::kPlugin && !f.suspect;
		}
	}
	Check(baseToActor, "crash log: the rebuilt base NPC FF000998 leads to its actor FF0040DF");
	Check(suspects == 2 && found.size() == 4 && found[0].suspect && found[1].suspect, "crash log: the actor (twice: itself and its base) is the likely cause, listed first");
	Check(spiritNotSuspect, "crash log: an actor only in stack memory is not the likely cause");
	Check(faction, "crash log: an unchanged plugin form is reported as the plugin's");
}

// Fallout 4 crash logs (Buffout 4 and Buffout 4 NG), laid out exactly as Buffout 4's CrashHandler.cpp and
// Introspection.cpp print them: register and stack entries with their object's lines indented two tabs, a
// reference's base form after "Object Reference:", the TESForm lines a second time before GetFullName
void TestCrashLogFO4()
{
	const std::string og =
		"Fallout 4 v1.10.163\n"
		"Buffout 4 v1.28.6 Mar 14 2023 18:26:53\n"
		"\n"
		"Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF6DAECCB9D Fallout4.exe+24FCB9D\tlock inc dword ptr [rax+0x10]\n"
		"Exception Flags: 0x00000000\n"
		"Number of Parameters: 2\n"
		"Access Violation: Tried to read memory at 0xFFFFFFFFFFFFFFFF\n"
		"\n"
		"\t[Compatibility]\n"
		"\t\tF4EE: true\n"
		"\n"
		"SYSTEM SPECS:\n"
		"\tOS: Microsoft Windows 10 Pro v10.0.19045\n"
		"\n"
		"PROBABLE CALL STACK:\n"
		"\t[0] 0x7FF6DAECCB9D Fallout4.exe+24FCB9D\n"
		"\t[1] 0x7FFA10001000  SomeMod.dll+0001000\n"
		"\t[2] 0x7FFA20002000    ntdll.dll+0002000\n"
		"\n"
		"REGISTERS:\n"
		"\tRAX 0xFFFFFFFFFFFFFFFF (size_t) [uint: 18446744073709551615 int: -1]\n"
		"\tRCX 0x1F2C3A4B5C0     (Actor*)\n"
		"\t\tFile: \"Fallout4.esm\"\n"
		"\t\tFlags: 0x00400408\n"
		"\t\tFormID: 0xFF00A1B2\n"
		"\t\tFormType: kACHR (65)\n"
		"\t\tObject Reference: \n"
		"\t\tFile: \"Fallout4.esm\"\n"
		"\t\tModified by: Fallout4.esm -> Unofficial Fallout 4 Patch.esp\n"
		"\t\tFlags: 0x00000408\n"
		"\t\tEditorID: \"CompanionPiper\"\n"
		"\t\tFormID: 0x00002F1F\n"
		"\t\tFormType: kNPC_ (45)\n"
		"\n"
		"STACK:\n"
		"\t[RSP+258 ] 0x1F2D0000000      (NavMesh*)\n"
		"\t\tFile: \"NRO-NewRedRocketOverhaul.esp\"\n"
		"\t\tFlags: 0x00000008\n"
		"\t\tFormID: 0x00019A70\n"
		"\t\tFormType: kNAVM (76)\n"
		"\t[RSP+870 ] 0x1F2E0000000      (TESObjectCELL*)\n"
		"\t\tFile: \"Concord Revised.esp\"\n"
		"\t\tModified by: Fallout4.esm -> Concord Revised.esp\n"
		"\t\tFlags: 0x00040009\n"
		"\t\tEditorID: \"ConcordCivicAccess01\"\n"
		"\t\tFormID: 0x0003776D\n"
		"\t\tFormType: kCELL (63)\n"
		"\t\tFile: \"Concord Revised.esp\"\n"
		"\t\tModified by: Fallout4.esm -> Concord Revised.esp\n"
		"\t\tFlags: 0x00040009\n"
		"\t\tEditorID: \"ConcordCivicAccess01\"\n"
		"\t\tFormID: 0x0003776D\n"
		"\t\tFormType: kCELL (63)\n"
		"\t\tGetFullName: \"Concord Civic Access\"\n"
		"\n"
		"MODULES:\n"
		"\tFallout4.exe 0x7FF6D8A00000\n"
		"\n"
		"F4SE PLUGINS:\n"
		"\tBuffout4.dll v1.28.6\n"
		"\n"
		"PLUGINS:\n"
		"\t[00]     Fallout4.esm\n";
	const auto log = ess::crashlog::Parse(og);
	Check(log.ok && log.fo4 && log.tool == "Buffout 4" && log.game == "Fallout 4 v1.10.163", "fo4 crash log: Buffout 4, Fallout 4 1.10.163");
	Check(log.exception.find("EXCEPTION_ACCESS_VIOLATION") != std::string::npos && log.exception.find("Fallout4.exe+24FCB9D") != std::string::npos,
		"fo4 crash log: the exception line");
	const ess::crashlog::Form *actor = nullptr, *npc = nullptr, *navm = nullptr, *cell = nullptr;
	for (const auto& f : log.forms) {
		actor = f.id == 0xFF00A1B2 ? &f : actor;
		npc = f.id == 0x00002F1F ? &f : npc;
		navm = f.id == 0x00019A70 ? &f : navm;
		cell = f.id == 0x0003776D ? &f : cell;
	}
	Check(log.forms.size() == 4, "fo4 crash log: four forms");
	Check(actor && actor->atCrash && actor->type == "kACHR (65)" && actor->file == "Fallout4.esm" && actor->editorID.empty() && actor->name.empty(),
		"fo4 crash log: the actor in a register, at the crash, not given its base's EditorID");
	Check(npc && npc->atCrash && npc->type == "kNPC_ (45)" && npc->editorID == "CompanionPiper" && npc->file == "Fallout4.esm",
		"fo4 crash log: the base NPC after Object Reference, with its EditorID and plugin");
	Check(navm && !navm->atCrash && navm->file == "NRO-NewRedRocketOverhaul.esp" && navm->type == "kNAVM (76)", "fo4 crash log: a navmesh in the stack");
	Check(cell && cell->hits == 2 && cell->name == "Concord Civic Access" && cell->editorID == "ConcordCivicAccess01" && cell->file == "Concord Revised.esp",
		"fo4 crash log: a cell named twice, with its full name, EditorID and plugin");
	Check(!log.forms.empty() && log.forms[0].atCrash && log.forms[1].atCrash, "fo4 crash log: the register forms come first");
	const auto c = ess::logs::ReadCrash(og);
	Check(c.where == "Fallout4.exe+24FCB9D" && c.modules.size() == 1 && c.modules[0] == "SomeMod.dll",
		"fo4 crash log: crash place and call stack DLLs, without Fallout4.exe and Windows ones");

	// Buffout 4 NG (next-gen and Anniversary games)
	const std::string ng =
		"Fallout 4 v1.10.984\n"
		"Buffout 4 v1.38.5 Oct 28 2026 01:59:55\n"
		"\n"
		"Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF71234ABCD Fallout4.exe+1A2B3C4\tmov rax, [rcx+0x8]\n"
		"Exception Flags: 0x00000000\n"
		"\n"
		"PROBABLE CALL STACK:\n"
		"\t[ 0] 0x7FF71234ABCD Fallout4.exe+1A2B3C4\n"
		"\n"
		"REGISTERS:\n"
		"\tRBX 0x2A0B0C0D0E0     (TESObjectREFR*)\n"
		"\t\tFile: \"MyMod.esp\"\n"
		"\t\tFlags: 0x00000000\n"
		"\t\tFormID: 0x0A001234\n"
		"\t\tFormType: kREFR (64)\n"
		"\t\tObject Reference: None\n";
	const auto log2 = ess::crashlog::Parse(ng);
	Check(log2.ok && log2.fo4 && log2.tool == "Buffout 4" && log2.game == "Fallout 4 v1.10.984" && log2.forms.size() == 1 &&
			  log2.forms[0].id == 0x0A001234 && log2.forms[0].atCrash && log2.forms[0].file == "MyMod.esp" && log2.forms[0].type == "kREFR (64)",
		"fo4 crash log: Buffout 4 NG, a reference with no base");

	// matched against a Fallout 4 save: its change forms only (the leveled-actor list is not used)
	ess::SaveFile save;
	save.header.magic = "FO4_SAVEGAME";
	ess::ChangeForm achr;
	achr.refID.data = (2u << 22) | 0xA1B2;
	achr.typeField = 1;  // ACHR (the same code in Fallout 4)
	save.changeForms.push_back(achr);
	save.reindexChangeForms();
	const std::vector<ess::crashlog::LeveledBase> leveled = { { 0x00002F1F, achr.refID.data, 0xFF00A1B2, 0 } };
	const auto found = ess::crashlog::Match(log, save, leveled);
	bool actorSuspect = false, npcPlugin = false;
	for (const auto& f : found) {
		if (f.form.id == 0xFF00A1B2) {
			actorSuspect = f.kind == ess::crashlog::Kind::kChanged && f.suspect && f.cfType == "ACHR" && f.created;
		}
		if (f.form.id == 0x00002F1F) {
			npcPlugin = f.kind == ess::crashlog::Kind::kPlugin && !f.suspect;
		}
	}
	Check(actorSuspect, "fo4 crash log: the spawned actor at the crash is the likely cause");
	Check(npcPlugin, "fo4 crash log: no leveled-actor lookup on a Fallout 4 save (the base stays a plugin form)");
	Check(ess::crashlog::LeveledBases(save).empty(), "fo4 crash log: no leveled actors are read from a Fallout 4 save");
}

void TestSkyrimLogs()
{
	const std::string skse =
		"SKSE64 runtime: initialize (version = 2.2.6 01061700 01DB12345, os = 6.2 (9200))\n"
		"plugin C:\\Data\\SKSE\\Plugins\\\\po3_Tweaks.dll (00000001 Tweaks 00010500) loaded correctly (handle 2)\n"
		"plugin C:\\Data\\SKSE\\Plugins\\\\Old.dll (00000001 Old 00000001) disabled, address library needs to be updated (handle 0)\n"
		"couldn't load plugin C:\\Data\\SKSE\\Plugins\\\\Broken.dll (Error 126)\n";
	const auto f = ess::logs::ReadSkseLog("skse64.log", skse);
	Check(f.size() == 3 && f[0].title.find("Old.dll") != std::string::npos && f[0].detail.find("Address Library") != std::string::npos &&
			  f[0].title.find("handle") == std::string::npos,
		"logs: a plugin SKSE disabled for Address Library is found, named and explained");
	Check(f.size() == 3 && f[1].title.find("Broken.dll") != std::string::npos && f[1].detail.find("126") != std::string::npos,
		"logs: a plugin that could not load (error 126) is found");
	const std::string crash =
		"Skyrim SSE v1.6.1170\nCrashLoggerSSE v1-15-0-0\n\n"
		"Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF6E3F4C680 SkyrimSE.exe+0cec680\tmov rax, [rcx]\n\n"
		"PROBABLE CALL STACK:\n\t[ 0] 0x7FF6E3F4C680 SkyrimSE.exe+0CEC680\n\t[ 1] 0x7FFA00001000 hdtSMP64.dll+0001000\n"
		"\t[ 2] 0x7FFA00002000 ntdll.dll+0002000\n\t[ 3] 0x7FFA00003000 hdtSMP64.dll+0001100\n\nREGISTERS:\n";
	const auto c = ess::logs::ReadCrash(crash);
	Check(c.where == "SkyrimSE.exe+0CEC680", "logs: the crash place is read from the exception line");
	Check(c.modules.size() == 1 && c.modules[0] == "hdtSMP64.dll", "logs: the call stack's DLLs, without the game and Windows ones, once each");
	Check(ess::json::Quote(std::string("a\"b\n\xE9")) == "\"a\\\"b\\n\xC3\xA9\"", "json: quotes, newlines and a Windows-1252 byte are written as valid UTF-8 JSON");
}

// Fallout 4's logs: a fake Documents\My Games tree (F4SE folder with f4se.log, a Buffout 4 crash log and plugin logs,
// Logs\Script, the INI files, a GOG folder, a Skyrim folder that must not be read), found and analyzed; F4SE's log lines
// as f4se 0.7.x (PluginManager::LogPluginLoadError: "<status> <code> (handle n)") and 0.6.x write them.
void TestFo4Logs()
{
	namespace fs = std::filesystem;
	const auto mg = fs::temp_directory_path() / "rs_unit_fo4logs" / "My Games";
	std::error_code ec;
	fs::remove_all(mg.parent_path(), ec);
	auto write = [](const fs::path& a_p, const std::string& a_text) {
		fs::create_directories(a_p.parent_path());
		std::ofstream(a_p, std::ios::binary) << a_text;
	};
	const auto f4 = mg / "Fallout4";
	write(f4 / "F4SE" / "f4se.log",
		"F4SE runtime: initialize (version = 0.7.2 010A3D80 01DA1234ABCD0000, os = 6.2 (9200))\n"
		"plugin directory = C:\\Games\\Fallout 4\\Data\\F4SE\\Plugins\\\n"
		"checking plugin Broken.dll\n"
		"plugin Broken.dll (00000000  00000000) couldn't load plugin 126 (handle 0)\n"
		"plugin Old.dll (00000001 Old 00000001) disabled, address library needs to be updated 0 (handle 0)\n"
		"plugin Ancient.dll (00000000  00000000) no version data 0 (handle 0)\n"
		"plugin tbb.dll (00000000  00000000) does not appear to be an F4SE plugin 0 (handle 3)\n"
		"plugin Good.dll (00000001 Good 00000001) loaded correctly (handle 2)\n");
	write(f4 / "F4SE" / "f4se_loader.log", "version = 0001000A03D80000\n");
	write(f4 / "F4SE" / "crash-2026-11-05-10-00-00.log",
		"Fallout 4 v1.10.984\nBuffout 4 v1.38.5 Oct 28 2026 01:59:55\n\n"
		"Unhandled exception \"EXCEPTION_ACCESS_VIOLATION\" at 0x7FF71234ABCD Fallout4.exe+1A2B3C4\tmov rax, [rcx+0x8]\n\n"
		"PROBABLE CALL STACK:\n\t[ 0] 0x7FF71234ABCD Fallout4.exe+1A2B3C4\n\t[ 1] 0x7FFA00001000 SomeMod.dll+0001000\n\nREGISTERS:\n");
	write(f4 / "F4SE" / "ReSaverHelper.log", "[info] ReSaverHelper 13 loaded\n[error] could not open the pipe\n");
	write(f4 / "F4SE" / "Plugins" / "Deep.log", "[error] something failed\n");
	write(f4 / "Logs" / "Script" / "Papyrus.0.log", "[11/05/2026 - 10:00:00AM] Papyrus log opened (PC-64)\n");
	write(f4 / "Fallout4.ini", "[General]\n");
	write(f4 / "Fallout4Prefs.ini", "[Display]\n");
	write(f4 / "Fallout4Custom.ini", "[Papyrus]\nbEnableLogging=1\n");
	write(f4 / "Saves" / "Save1.fos", "FO4_SAVEGAME");
	write(mg / "Fallout4 GOG" / "F4SE" / "f4se.log", "F4SE runtime: initialize (version = 0.6.23)\n");
	write(mg / "Skyrim Special Edition" / "SKSE" / "skse64.log", "SKSE64 runtime: initialize\n");

	const auto files = ess::logs::FindGameLogs({ mg }, {}, true);
	auto has = [&](const std::string& a_name, ess::logs::Kind a_kind) {
		return std::any_of(files.begin(), files.end(), [&](const ess::logs::LogFile& f) {
			return f.path.filename().string() == a_name && f.kind == a_kind && f.game.rfind("Fallout4", 0) == 0;
		});
	};
	using K = ess::logs::Kind;
	Check(has("f4se.log", K::kSkse) && has("f4se_loader.log", K::kSkse), "fo4 logs: F4SE's own logs are found as the script extender's");
	Check(has("crash-2026-11-05-10-00-00.log", K::kCrash), "fo4 logs: a Buffout 4 crash log in the F4SE folder");
	Check(has("ReSaverHelper.log", K::kPlugin) && has("Deep.log", K::kPlugin), "fo4 logs: the F4SE plugins' own logs, one folder down too");
	Check(has("Papyrus.0.log", K::kPapyrus), "fo4 logs: the Papyrus log in Logs\\Script");
	Check(has("Fallout4.ini", K::kIni) && has("Fallout4Prefs.ini", K::kIni) && has("Fallout4Custom.ini", K::kIni), "fo4 logs: the game's INI files");
	Check(std::count_if(files.begin(), files.end(), [](const ess::logs::LogFile& f) { return f.game == "Fallout4 GOG"; }) == 1,
		"fo4 logs: the GOG game's My Games folder too");
	Check(std::none_of(files.begin(), files.end(), [](const ess::logs::LogFile& f) {
		return f.path.filename() == "skse64.log" || f.path.extension() == ".fos";
	}), "fo4 logs: no Skyrim logs and no saves");
	Check(files.size() == 10, ("fo4 logs: ten files in all (" + std::to_string(files.size()) + ")").c_str());
	const auto sky = ess::logs::FindGameLogs({ mg }, {}, false);
	Check(sky.size() == 1 && sky[0].path.filename() == "skse64.log", "fo4 logs: the Skyrim search finds only Skyrim's log, no INI files");
	Check(std::string(ess::logs::KindName(K::kSkse, true)) == "F4SE" && std::string(ess::logs::KindName(K::kSkse)) == "SKSE" &&
			  std::string(ess::logs::KindName(K::kIni, true)) == "INI file",
		"fo4 logs: kind names");

	// f4se.log: what did not load and why
	std::string text;
	{
		std::ifstream in(f4 / "F4SE" / "f4se.log", std::ios::binary);
		text.assign(std::istreambuf_iterator<char>(in), {});
	}
	const auto fx = ess::logs::ReadSkseLog(f4 / "F4SE" / "f4se.log", text, true);
	auto find = [&](const std::string& a_part) -> const ess::logs::Finding* {
		for (const auto& x : fx) {
			if (x.title.find(a_part) != std::string::npos) {
				return &x;
			}
		}
		return nullptr;
	};
	const auto* broken = find("Broken.dll");
	Check(broken && broken->severity == ess::logs::Severity::kBad && broken->title.rfind("F4SE could not load", 0) == 0 &&
			  broken->detail.find("126") != std::string::npos,
		"fo4 logs: couldn't load plugin 126 (F4SE 0.7: the code after the status)");
	const auto* old = find("Old.dll");
	Check(old && old->detail.find("Address Library for F4SE Plugins") != std::string::npos && old->title.find("handle") == std::string::npos &&
			  old->title.size() >= 2 && old->title.substr(old->title.size() - 2) != " 0",
		"fo4 logs: disabled for the address library, without the error code and handle");
	const auto* ancient = find("Ancient.dll");
	Check(ancient && ancient->severity == ess::logs::Severity::kBad && ancient->title.find("no version data") != std::string::npos,
		"fo4 logs: a plugin without version data is not loaded");
	const auto* tbb = find("tbb.dll");
	Check(tbb && tbb->severity == ess::logs::Severity::kWarn, "fo4 logs: a DLL that is not an F4SE plugin is worth a look only");
	Check(!find("Good.dll"), "fo4 logs: a plugin that loaded is not a finding");
	Check(find("F4SE: F4SE runtime: initialize") != nullptr, "fo4 logs: F4SE's version line");
	const auto o06 = ess::logs::ReadSkseLog("f4se.log", "couldn't load plugin C:\\Games\\Fallout 4\\Data\\F4SE\\Plugins\\X.dll (Error 126: Y.dll)\n", true);
	Check(o06.size() == 1 && o06[0].title == "F4SE could not load X.dll" && o06[0].detail.find("126") != std::string::npos,
		"fo4 logs: F4SE 0.6's couldn't load line");

	// all of it
	ess::logs::Options opt;
	opt.days = 0;
	opt.fo4 = true;
	const auto a = ess::logs::Analyze(files, opt);
	const auto all = ess::logs::ToText(a);
	Check(all.find("F4SE could not load Broken.dll") != std::string::npos && all.find("Fallout4.exe+1A2B3C4") != std::string::npos &&
			  all.find("SomeMod.dll") != std::string::npos && all.find("ReSaverHelper.log") != std::string::npos,
		"fo4 logs: the analysis has the F4SE load failure, the Buffout 4 crash and a plugin log's error");
	Check(all.find("skse64") == std::string::npos, "fo4 logs: nothing from Skyrim's folder");
	const auto none = ess::logs::Analyze({}, opt);
	Check(!none.findings.empty() && none.findings[0].title == "No Fallout 4 logs found" && none.findings[0].detail.find("F4SE") != std::string::npos,
		"fo4 logs: none found says where F4SE writes them");
	fs::remove_all(mg.parent_path(), ec);
}

// The AI chat's game-dependent parts (core/AiGame): the lesson book, the game line, what the command line and the
// helper refuse for Fallout 4.
void TestAiGame()
{
	namespace ag = ess::aigame;
	Check(ag::FromFlags(true, false, false) == ag::Game::kFallout4 && ag::FromFlags(false, true, false) == ag::Game::kSkyrimLE &&
			  ag::FromFlags(false, false, true) == ag::Game::kSkyrimVR && ag::FromFlags(false, false, false) == ag::Game::kSkyrimSE,
		"ai: the game from the save's flags");
	const auto f4 = ag::Lesson(ag::Game::kFallout4);
	const auto se = ag::Lesson(ag::Game::kSkyrimSE);
	Check(f4.find("FALLOUT 4") != std::string::npos && f4.find(".fos") != std::string::npos && f4.find(".f4se") != std::string::npos &&
			  f4.find("F4SE") != std::string::npos && f4.find("Buffout 4") != std::string::npos,
		"ai: the Fallout 4 lesson names the game, its saves, F4SE and Buffout 4");
	Check(f4.find("fo4_player") != std::string::npos && f4.find("fo4_edit_player") != std::string::npos && f4.find("leftover_items") != std::string::npos &&
			  f4.find("--fo4-transplant") != std::string::npos && f4.find("--quests") != std::string::npos,
		"ai: the Fallout 4 lesson lists the Fallout 4 tools");
	Check(f4.find("NOT AVAILABLE FOR FALLOUT 4 SAVES") != std::string::npos && f4.find("--fix-mcm") != std::string::npos &&
			  f4.find("CONTROLSRESET") != std::string::npos && f4.find("F4NG") != std::string::npos,
		"ai: the Fallout 4 lesson says what does not work and the Fallout 4 helper's commands");
	Check(f4.find("SkyrimSE.exe+0CEC680") == std::string::npos && f4.find("SKSE plugin load errors") == std::string::npos &&
			  f4.find("FOLLOWERS ->") == std::string::npos,
		"ai: no Skyrim crash notes or Skyrim helper commands in the Fallout 4 lesson");
	Check(se.find("SkyrimSE.exe+0CEC680") != std::string::npos && se.find(".ess") != std::string::npos &&
			  se.find("WHAT WORKS ON A FALLOUT 4 SAVE") == std::string::npos && se.find("FOLLOWERS ->") != std::string::npos,
		"ai: the Skyrim lesson keeps Skyrim's notes");
	const auto line = ag::GameLine(ag::Game::kFallout4, "1.10.984.0", true);
	Check(line.rfind("- game: Fallout 4", 0) == 0 && line.find("1.10.984.0") != std::string::npos && line.find(".fos") != std::string::npos &&
			  line.find("F4SE") != std::string::npos && line.find("Buffout 4") != std::string::npos,
		"ai: the Fallout 4 game line (version, saves, F4SE, Buffout 4)");
	Check(ag::GameLine(ag::Game::kSkyrimSE, {}, true).find("Skyrim Special Edition / AE") != std::string::npos &&
			  ag::GameLine(ag::Game::kSkyrimLE, {}, true).find("Legendary") != std::string::npos &&
			  ag::GameLine(ag::Game::kSkyrimVR, {}, false).find("no save open") != std::string::npos,
		"ai: the Skyrim game lines");
	const auto no = ag::CliRefusal(ag::Game::kFallout4, { "C:\\Saves\\Save1.fos", "--player" });
	Check(no.rfind("not available for Fallout 4 saves", 0) == 0 && no.find("fo4_player") != std::string::npos, "ai: --player is refused for Fallout 4");
	Check(ag::CliRefusal(ag::Game::kFallout4, { "x.fos", "--Fix-MCM", "-o", "y.fos" }).find("--Fix-MCM") != std::string::npos,
		"ai: an option is refused in any letter case");
	for (const char* ok : { "--quests", "--journal", "--describe", "--remove-unattached", "--purge-script", "--fo4-set-level", "--fo4-perks",
			 "--fo4-transplant", "--crash-log", "--mods", "--spawned", "--remove-mod", "--compare", "--delete-form", "--reset-havok", "--purify-formlists", "--merged-patch" }) {
		Check(ag::CliRefusal(ag::Game::kFallout4, { "x.fos", ok }).empty(), (std::string("ai: works on Fallout 4: ") + ok).c_str());
	}
	Check(ag::CliRefusal(ag::Game::kSkyrimSE, { "x.ess", "--player", "--reset-havok" }).empty(), "ai: nothing is refused for Skyrim");
	Check(ag::HelperIsFo4("F4") && ag::HelperIsFo4("F4NG") && ag::HelperIsFo4("F4AE") && !ag::HelperIsFo4("SE") && !ag::HelperIsFo4("AE") &&
			  !ag::HelperIsFo4("LE"),
		"ai: the Fallout 4 helper from HELLO's game field");
	Check(!ag::HelperRefusal("F4NG", "QUEST").empty() && !ag::HelperRefusal("F4AE", "followers").empty() && ag::HelperRefusal("F4NG", "GATHER").empty() &&
			  ag::HelperRefusal("F4", "apply").empty() && ag::HelperRefusal("F4AE", "CONTROLSRESET").empty() && ag::HelperRefusal("AE", "QUEST").empty(),
		"ai: the Fallout 4 helper's commands");
	Check(ag::HelperReadOnly("HELLO") && ag::HelperReadOnly("getoption") && !ag::HelperReadOnly("SAVEGAME") && !ag::HelperReadOnly("APPLY"),
		"ai: read-only helper commands need no approval");
	Check(ag::LogsTitle(ag::Game::kFallout4) == "Fallout 4 logs" && ag::LogsTitle(ag::Game::kSkyrimLE) == "Skyrim logs", "ai: the logs window's title");
	Check(ag::ChatIntro(ag::Game::kFallout4).find("Fallout 4") != std::string::npos && ag::ChatExamples(ag::Game::kFallout4).size() == 5 &&
			  ag::ChatExamples(ag::Game::kFallout4)[3].find("F4SE") != std::string::npos,
		"ai: the chat's Fallout 4 intro and examples");
}

// A Fallout 4 BA2 (GNRL, version 8) made in memory: one stored file and one zlib file; listed and unpacked.
void TestBa2()
{
	const std::string           a = "stored text file";
	std::vector<std::uint8_t>   b(5000);
	for (std::size_t i = 0; i < b.size(); ++i) {
		b[i] = static_cast<std::uint8_t>("pex data "[i % 9]);
	}
	const auto                  bz = CompressZlib(b);
	std::vector<std::uint8_t>   f;
	auto put = [&](std::uint64_t v, int n) {
		for (int i = 0; i < n; ++i) f.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
	};
	const std::uint64_t data = 24 + 2 * 36;
	const std::uint64_t names = data + a.size() + bz.size();
	f.insert(f.end(), { 'B', 'T', 'D', 'X' });
	put(8, 4);
	f.insert(f.end(), { 'G', 'N', 'R', 'L' });
	put(2, 4);
	put(names, 8);
	// records: hash, ext, dir hash, flags (0, 1 chunk, 16), offset, packed, size, 0xBAADF00D
	put(1, 4); f.insert(f.end(), { 't', 'x', 't', 0 }); put(2, 4); put(0x00100100, 4); put(data, 8); put(0, 4); put(a.size(), 4); put(0xBAADF00D, 4);
	put(3, 4); f.insert(f.end(), { 'p', 'e', 'x', 0 }); put(4, 4); put(0x00100100, 4); put(data + a.size(), 8); put(bz.size(), 4); put(b.size(), 4); put(0xBAADF00D, 4);
	f.insert(f.end(), a.begin(), a.end());
	f.insert(f.end(), bz.begin(), bz.end());
	for (const std::string n : { "Misc/Read Me.TXT", "Scripts\\Foo.pex" }) {
		put(n.size(), 2);
		f.insert(f.end(), n.begin(), n.end());
	}
	const auto file = std::filesystem::temp_directory_path() / "rs_unit_test.ba2";
	{
		std::ofstream o(file, std::ios::binary | std::ios::trunc);
		o.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
	}
	try {
		BsaArchive ba(file);
		Check(ba.isBa2() && !ba.isTextureArchive() && ba.version() == 8 && ba.kind() == "BA2 GNRL v8", "ba2: a GNRL BA2 is opened");
		Check(ba.entries().size() == 2 && ba.entries()[0].path == "misc\\read me.txt" && ba.entries()[1].path == "scripts\\foo.pex",
			"ba2: the paths come from the name table, lowercase with backslashes");
		const auto* e0 = ba.find("misc\\read me.txt");
		const auto* e1 = ba.find("scripts\\foo.pex");
		Check(e0 && !e0->compressed && e1 && e1->compressed, "ba2: stored and compressed files are told apart");
		const auto d0 = ba.extract(*e0);
		const auto d1 = ba.extract(*e1);
		Check(std::string(d0.begin(), d0.end()) == a, "ba2: a stored file is extracted as it is");
		Check(d1 == b, "ba2: a zlib file is unpacked");
		const auto l = ListBSA(file);
		Check(l.size() == 2 && l[1] == "scripts\\foo.pex", "ba2: ListBSA lists a BA2");
		const auto out = std::filesystem::temp_directory_path() / "rs_unit_ba2_out";
		std::filesystem::remove_all(out);
		const auto r = ExtractBsa(ba, out, "foo", true);
		Check(r.written == 1 && r.failed == 0 && std::filesystem::file_size(out / "scripts" / "foo.pex") == b.size(), "ba2: ExtractBsa unpacks the matching files");
		std::filesystem::remove_all(out);
		BsaArchive::Texture t;
		t.width = 256;
		t.height = 128;
		t.mips = 9;
		t.format = 71;  // BC1
		const auto dh = MakeDdsHeader(t);
		Check(dh.size() == 128 && std::memcmp(dh.data(), "DDS ", 4) == 0 && std::memcmp(dh.data() + 84, "DXT1", 4) == 0, "ba2: a BC1 texture gets a DXT1 DDS header");
		t.format = 98;  // BC7
		const auto d7 = MakeDdsHeader(t);
		Check(d7.size() == 148 && std::memcmp(d7.data() + 84, "DX10", 4) == 0 && d7[128] == 98, "ba2: a BC7 texture gets the DX10 header");
	} catch (const std::exception& e) {
		std::printf("ba2: %s\n", e.what());
		Check(false, "ba2: the test archive opens");
	}
	std::filesystem::remove(file);
}


// Fallout 4 quests: QUST change forms (Skyrim's layout, checked against TESQuest::LoadGame in Fallout4.exe 1.11.240),
// the Pip-Boy's quest log + objectives in the player's record, and the Fallout 4 QUST VMAD
void TestQuestsFO4()
{
	ess::SaveFile save;
	save.header.magic = "FO4_SAVEGAME";
	save.plugins.full = { "Fallout4.esm" };
	auto refOf = [](std::uint32_t a_local) { return (1u << 22) | a_local; };  // a form of the first plugin
	// a real body (MainQuestRailroad.fos, quest 0000089C): flags, 2 stages (0 done, 255 done), already run
	ess::ChangeForm q1;
	q1.refID.data = refOf(0x89C);
	q1.typeField = 8;  // QUST (the same code in Fallout 4)
	q1.changeFlags = 0x84000002;
	q1.data = { 0x00, 0x00, 0x08, 0x00, 0x00, 0x01, 0xFF, 0x00, 0x01, 0x01 };
	q1.length1 = static_cast<std::uint32_t>(q1.data.size());
	// run data whose last item has a type Skyrim does not know (Fallout 4 reads nothing more for it)
	ess::ChangeForm q2;
	q2.refID.data = refOf(0x1234);
	q2.typeField = 8;
	q2.changeFlags = 0x30000003;  // form flags, quest flags, objectives, run data
	q2.data = { 1, 0, 0, 0, 0, 0,  0x01, 0x00,  0x04, 10, 0, 0, 0, 3, 0, 0, 0,  0x00, 0, 0, 0, 0, 0, 0, 0, 0,  0x01, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0 };
	q2.length1 = static_cast<std::uint32_t>(q2.data.size());
	// the player: something, then the quest log (1 entry: quest 1234, stage 10, log 0) and the objectives (1: 1234 #10 shown + completed)
	ess::ChangeForm pl;
	pl.refID.data = refOf(0x14);
	pl.typeField = 1;
	pl.data = { 0xAA, 0xBB, 0xCC,  0x04, 0x40, 0x12, 0x34, 10, 0, 0,  0x04, 0x40, 0x12, 0x34, 10, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0,  0xDD };
	pl.length1 = static_cast<std::uint32_t>(pl.data.size());
	save.changeForms = { q1, q2, pl };
	save.reindexChangeForms();
	const auto d1 = ess::cf::Decode(save, save.changeForms[0]);
	const auto d2 = ess::cf::Decode(save, save.changeForms[1]);
	Check(d1.complete && d2.complete, "fo4 quests: QUST change forms decode completely (incl. a run data type Skyrim has not)");
	const auto st = ess::quests::ReadStates(save);
	const ess::quests::QuestState* a = nullptr;
	const ess::quests::QuestState* b = nullptr;
	for (const auto& q : st) {
		a = q.formID == 0x89C ? &q : a;
		b = q.formID == 0x1234 ? &q : b;
	}
	Check(a && a->stages.size() == 2 && a->stages[0].second && a->stages[1].first == 255 && a->highestDoneStage() == 255, "fo4 quests: stages and done flags");
	Check(b && b->running() && b->objectives.size() == 1 && b->objectives[0].index == 10 && b->objectives[0].state == 3, "fo4 quests: flags and objective states");
	Check(b && b->journalKnown && b->journal.size() == 1 && b->journal[0].state == 3 && b->logShown.size() == 1 && b->logShown[0].first == 10,
		"fo4 quests: the Pip-Boy's objectives and quest log from the player's record");
	bool found = false;
	const auto log = ess::quests::ReadQuestLog(save, &found);
	Check(found && log.size() == 1 && log[0].quest == 0x1234 && log[0].stage == 10, "fo4 quests: ReadQuestLog");

	// VMAD (version 6, object format 2): one script with an int and a struct property, then the fragments: fragment
	// script with its own property, one fragment for stage 20
	std::vector<std::uint8_t> v;
	auto u8 = [&](std::uint8_t x) { v.push_back(x); };
	auto u16 = [&](std::uint16_t x) { u8(x & 0xFF); u8(x >> 8); };
	auto u32 = [&](std::uint32_t x) { u16(x & 0xFFFF); u16(x >> 16); };
	auto str = [&](const std::string& t) { u16(static_cast<std::uint16_t>(t.size())); for (char c : t) u8(static_cast<std::uint8_t>(c)); };
	u16(6); u16(2);
	u16(1); str("MyQuestScript"); u8(0); u16(2);
	str("Count"); u8(3); u8(1); u32(7);
	str("Data"); u8(7); u8(1); u32(1); str("Member"); u8(5); u8(1); u8(1);
	u8(3); u16(1); str("Fragments:Quests:QF_MyQuest"); u8(0); u16(1); str("Alias"); u8(3); u8(1); u32(9);
	u32(20); u32(0); u8(0); str("Fragments:Quests:QF_MyQuest"); str("Fragment_Stage_0020_Item_00");
	ess::quests::QuestDef def;
	const bool ok = ess::quests::ParseQuestVMADFO4(v.data(), v.size(), {}, def);
	Check(ok && def.scripts.size() == 1 && def.fragmentScript == "Fragments:Quests:QF_MyQuest" && def.fragments.size() == 1 &&
			  def.fragments[0].stage == 20 && def.fragments[0].function == "Fragment_Stage_0020_Item_00" && def.properties.size() == 1 &&
			  def.properties[0].name == "Alias",
		"fo4 quests: VMAD with a struct property and the fragment script's own properties");
}

// Fallout 4 REFR / ACHR / NPC_ bodies built by hand in the layout the game reads
void TestFo4ChangeForms()
{
	using B = std::vector<std::uint8_t>;
	auto u32 = [](B& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
	auto u16 = [](B& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); };
	auto f32 = [&](B& b, float f) { std::uint32_t v; std::memcpy(&v, &f, 4); u32(b, v); };
	auto ref = [](B& b, std::uint32_t formID) { const std::uint32_t d = (1u << 22) | formID; b.push_back(d >> 16); b.push_back(d >> 8); b.push_back(d); };
	auto initial = [&](B& b) { ref(b, 0x3C); f32(b, 10.f); f32(b, 20.f); f32(b, 30.f); f32(b, 0.f); f32(b, 0.f); f32(b, 1.5f); };

	ess::SaveFile save;
	save.header.magic = "FO4_SAVEGAME";
	save.formVersion = 68;

	// REFR: moved (1), scale (4), base (7), extra data (6), inventory (5)
	B r;
	initial(r);
	ref(r, 0x1234);                 // base object
	f32(r, 2.f);                    // scale
	r.push_back(3 << 2);            // 3 extras
	r.push_back(0x98); r.push_back(1 << 2); r.insert(r.end(), 3, 0); u32(r, 0);  // 0x98: one entry with a null form, then u16 u16
	r.push_back(0x5f);              // the game's default case: nothing read
	r.push_back(0x2a); r.push_back(1); r.push_back(2); ref(r, 0x99); u32(r, 7);  // lock (version >= 0x36)
	u32(r, 1);                      // one item
	ref(r, 0xF);
	u32(r, 2);                      // two stacks
	r.push_back(1 << 2); r.push_back(0x35); u16(r, 0); u32(r, 1); ref(r, 0x55); r.push_back(1); r.push_back(0); r.push_back(0);  // object mods
	u32(r, 5); u16(r, 0);
	r.push_back(0); u32(r, 7); u16(r, 0);
	ess::ChangeForm refr;
	refr.refID.data = (1u << 22) | 0xABC;
	refr.typeField = 0;
	refr.changeFlags = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);
	refr.data = r;
	refr.length1 = static_cast<std::uint32_t>(r.size());
	const auto d = ess::fo4cf::DecodeRefr(save, refr);
	Check(d.decoded && d.complete, "fo4 REFR: read completely");
	Check(d.hasInitial && save.formIDOf(d.cell) == 0x3C && d.pos[1] == 20.f && d.rot[2] == 1.5f, "fo4 REFR: cell, position and rotation");
	Check(save.formIDOf(d.base) == 0x1234 && d.hasScale && d.scale == 2.f, "fo4 REFR: base object and scale");
	Check(d.extras.size() == 3 && d.extras[0].type == 0x98 && d.extras[1].type == 0x5f && d.extras[1].range.size() == 1 && d.extras[2].type == 0x2a,
		"fo4 REFR: extra data, a null 0x98 entry and a default-case type");
	Check(d.inventory.size() == 1 && d.inventory[0].formID == 0xF && d.inventory[0].count == 12 && d.inventory[0].stacks.size() == 2 &&
			  d.inventory[0].stacks[0].extras.size() == 1 && d.inventory[0].stacks[0].extras[0].type == 0x35,
		"fo4 REFR: inventory item, stacks and their object mods");
	const auto& cr = d.inventory[0].stacks[1].countRange;
	std::int32_t c = 0;
	std::memcpy(&c, r.data() + cr.begin, 4);
	Check(cr.size() == 4 && c == 7, "fo4 REFR: the stack count's byte range");

	// an extra type that is not decoded stops with an error and no "complete"
	auto bad = refr;
	bad.data[27 + 3 + 4 + 1] = 0x16;
	const auto db = ess::fo4cf::DecodeRefr(save, bad);
	Check(!db.decoded && db.error.find("0x16") != std::string::npos, "fo4 REFR: an undecoded extra type is reported");

	// the player: ACHR 0x14 with caps; the actor part is left over
	B a;
	initial(a);
	u32(a, 0); u32(a, 0);           // the actor prefix
	u32(a, 2);
	ref(a, 0xF); u32(a, 1); a.push_back(0); u32(a, 437); u16(a, 0);
	ref(a, 0x1000); u32(a, 1); a.push_back(0); u32(a, 1); u16(a, 0);
	a.push_back(0xAA); a.push_back(0xBB);  // actor part
	ess::ChangeForm achr;
	achr.refID.data = (1u << 22) | 0x14;
	achr.typeField = 1;
	achr.changeFlags = (1u << 1) | (1u << 5);
	achr.data = a;
	achr.length1 = static_cast<std::uint32_t>(a.size());
	save.changeForms.push_back(achr);
	const auto dp = ess::fo4cf::DecodeRefr(save, achr);
	Check(dp.decoded && !dp.complete && dp.parts.back().name == std::string("actor") && dp.parts.back().range.size() == 2 &&
			  dp.refrPart.end == a.size() - 2,
		"fo4 ACHR: the reference part, then the actor part's range");
	Check(ess::fo4cf::FindPlayer(save) && ess::fo4cf::PlayerCaps(save) == 437, "fo4 player: caps from the inventory");
	Check(ess::fo4cf::SetPlayerCaps(save, 1000).empty() && ess::fo4cf::PlayerCaps(save) == 1000, "fo4 player: set caps");
	Check(!ess::fo4cf::SetPlayerCaps(save, -1).empty() && ess::fo4cf::PlayerCaps(save) == 1000, "fo4 player: negative caps refused");

	// an actor part with no AI process: actor fields, then one actor value (Luck 0x2C8 = 3)
	{
		B ab;
		u32(ab, 0xFFFFFFFFu); u32(ab, 0);       // prefix: no process
		ab.push_back(0); for (int i = 0; i < 9; ++i) u32(ab, 0);
		ab.insert(ab.end(), 12, 0); ref(ab, 0);
		u16(ab, 0); ab.insert(ab.end(), 20, 0);  // 0xceb7c0 (form version 68: 20 bytes)
		ab.push_back(0);                          // no magic casters
		for (int i = 0; i < 5; ++i) ref(ab, 0);
		for (int i = 0; i < 4; ++i) u32(ab, 0);
		ab.push_back(1 << 2); ref(ab, 0x2C8); f32(ab, 3.f);
		ab.push_back(0);
		ess::ChangeForm npcA;
		npcA.refID.data = (1u << 22) | 0x1234;
		npcA.typeField = 1;
		npcA.data = ab;
		npcA.length1 = static_cast<std::uint32_t>(ab.size());
		const auto da = ess::fo4cf::DecodeActor(save, npcA);
		Check(da.decoded && da.processLevel == -1 && da.values.size() == 1 && da.values[0].formID == 0x2C8 && da.values[0].base == 3.f &&
				  da.values[0].baseRange.end == ab.size() - 1,
			"fo4 ACHR: actor part read up to the actor values");
		Check(ess::fo4cf::SetActorValueBase(save, npcA, 0x2C8, 7.f).empty() && ess::fo4cf::DecodeActor(save, npcA).values[0].base == 7.f,
			"fo4 ACHR: set an actor value's base");
		Check(!ess::fo4cf::SetActorValueBase(save, npcA, 0x2C2, 7.f).empty(), "fo4 ACHR: a value the actor has no base for is refused");

		// the rest of Actor::LoadGame: a movement controller, the actor's fields and the perk lists
		B t = ab;
		// (ab ends with the empty modifier list)
		u32(t, 0);               // refs
		t.push_back(1);          // has a movement controller
		u32(t, 0); u32(t, 0);    // dcc700, dcc5a0
		u32(t, 1); t.insert(t.end(), 7, 0);
		u32(t, 1); t.push_back(2); t.push_back(1); f32(t, 0.5f);  // one arbiter: Planner (bool, f32)
		u32(t, 2);
		t.push_back(0x0E); u32(t, 0); u32(t, 5); u32(t, 0); u32(t, 0);  // Virtual Path Follower: u32, pointer id 5 (new:
		                                                                // request class 0), pointer id 0
		t.push_back(0x04);                                              // Angle Pass Through: nothing
		t.push_back(0);                                                 // no trailing object
		u32(t, 0); t.push_back(0); u32(t, 0);  // u32, no 0x87c540 block, no 0xcdf2d0 entries
		u32(t, 0); u32(t, 0); t.insert(t.end(), 3, 0); t.push_back(0); u32(t, 0);  // u32, u32, form id, u8, u32
		t.push_back(1);  // perk lists
		t.push_back(2 << 2); ref(t, 0x1A54A4); t.push_back(1); ref(t, 0x84B); t.push_back(2);
		t.push_back(1 << 2); ref(t, 0x1A54A4);
		t.push_back(1 << 2); ref(t, 0x84B); u32(t, 1);
		t.insert(t.end(), 3, 0);  // a null form id
		ess::ChangeForm npcT;
		npcT.refID.data = (1u << 22) | 0x1235;
		npcT.typeField = 1;
		npcT.data = t;
		npcT.length1 = static_cast<std::uint32_t>(t.size());
		const auto dt = ess::fo4cf::DecodeActor(save, npcT);
		Check(dt.tail && dt.hasController && dt.tailEnd == t.size() && dt.perks.size() == 2 && dt.perks[0].formID == 0x1A54A4 &&
				  dt.perks[1].rank == 2 && dt.perkRefs.size() == 1 && dt.perkValues.size() == 1 && dt.perkValues[0].value == 1,
			"fo4 ACHR: movement controller, actor tail and perk lists");
		Check(ess::fo4cf::SetPerkRank(save, npcT, 0x84B, 4).empty() && ess::fo4cf::DecodeActor(save, npcT).perks[1].rank == 4,
			"fo4 ACHR: set a perk rank");
		Check(!ess::fo4cf::SetPerkRank(save, npcT, 0x12345, 4).empty(), "fo4 ACHR: a perk the actor does not have is refused");
		Check(ess::fo4cf::AddPerk(save, npcT, 0xD9784, 1).empty(), "fo4 ACHR: add a perk");
		{
			const auto d2 = ess::fo4cf::DecodeActor(save, npcT);
			Check(d2.tail && d2.perks.size() == 3 && d2.perks[2].formID == 0xD9784 && d2.tailEnd == t.size() + 4, "fo4 ACHR: the added perk reads back");
		}
		Check(ess::fo4cf::RemovePerk(save, npcT, 0x1A54A4).empty(), "fo4 ACHR: remove a perk");
		{
			const auto d3 = ess::fo4cf::DecodeActor(save, npcT);
			Check(d3.tail && d3.perks.size() == 2 && d3.perks[0].formID == 0x84B && d3.perkRefs.size() == 1 && d3.tailEnd == t.size(),
				"fo4 ACHR: the list after a removal");
		}
	}

	// a created reference (a dropped item): its initial data ends with u8 + the base object
	B crb;
	initial(crb);
	crb.push_back(0);
	ref(crb, 0x24F55);
	ess::ChangeForm created;
	created.refID.data = (2u << 22) | 0x1199;
	created.typeField = 0;
	created.changeFlags = 1u << 1;
	created.data = crb;
	created.length1 = static_cast<std::uint32_t>(crb.size());
	const auto dc = ess::fo4cf::DecodeRefr(save, created);
	Check(dc.complete && dc.createdBase && save.formIDOf(dc.base) == 0x24F55, "fo4 REFR: a created reference's base object");

	// NPC_: factions (6) and name (5)
	B n;
	n.push_back(1 << 2); ref(n, 0x777); n.push_back(1);  // one faction: ref + rank
	u16(n, 4); for (char ch : std::string("Nate")) n.push_back(static_cast<std::uint8_t>(ch));
	ess::ChangeForm npc;
	npc.refID.data = (1u << 22) | 7;
	npc.typeField = 9;
	npc.changeFlags = (1u << 5) | (1u << 6);
	npc.data = n;
	npc.length1 = static_cast<std::uint32_t>(n.size());
	const auto dn = ess::fo4cf::DecodeNpc(save, npc);
	Check(dn.complete && dn.name == "Nate" && dn.parts.size() == 2, "fo4 NPC_: factions and name");
	Check(ess::fo4cf::PlayerLevel(save) == -1, "fo4 player level: none without a changed ACBS");

	// the player's NPC_ with a changed ACBS (flag 1): the level is the i16 at offset 6
	{
		B ac;
		u32(ac, 0x24); u16(ac, 50); u16(ac, 12); ac.insert(ac.end(), 12, 0);
		ess::ChangeForm pn;
		pn.refID.data = (1u << 22) | 7;
		pn.typeField = 9;
		pn.changeFlags = 1u << 1;
		pn.data = ac;
		pn.length1 = static_cast<std::uint32_t>(ac.size());
		auto& s2 = save;
		auto  kept = std::move(s2.changeForms);
		const auto keptLevel = s2.header.playerLevel;
		s2.changeForms.clear();
		s2.changeForms.push_back(pn);
		Check(ess::fo4cf::PlayerLevel(s2) == 12, "fo4 player level from the ACBS block");
		Check(ess::fo4cf::SetPlayerLevel(s2, 30).empty() && ess::fo4cf::PlayerLevel(s2) == 30 && s2.header.playerLevel == 30,
			"fo4 player level: set (ACBS and header)");
		Check(!ess::fo4cf::SetPlayerLevel(s2, 0).empty(), "fo4 player level: 0 refused");
		Check(!ess::fo4cf::SetPlayerXP(s2, 10.f).empty(), "fo4 player experience: no player reference, refused");
		s2.changeForms = std::move(kept);
		s2.header.playerLevel = keptLevel;
	}
	Check(std::string(ess::fo4cf::RefrFlagMeaning(10, false)) == "extra data list" && std::string(ess::fo4cf::RefrFlagMeaning(10, true)).empty(),
		"fo4 change flags: extra data masks differ for REFR and ACHR");
}

// Fallout 4 Transplant: form ids, helper records, and real saves read offline and compared. The real saves are
// found in RESAVERPP_FO4_SAVES (folders separated by ';'; every .fos under them); without it those checks are skipped.
void TestTransplantFO4()
{
	namespace tf4 = ess::transplant::fo4;
	namespace fs = std::filesystem;
	PluginInfo lo;
	lo.full = { "Fallout4.esm", "DLCRobot.esm", "MyMod.esp" };
	lo.lite = { "ccBGSFO4001-PipBoy(Black).esl", "Small.esp" };
	lo.hasLite = true;
	Check(tf4::RuntimeID(lo, "fallout4.esm", 0x24F55) == 0x00024F55 && tf4::RuntimeID(lo, "MYMOD.ESP", 0x000801) == 0x02000801,
		"fo4 transplant: form id of a full plugin's form (case does not matter)");
	Check(tf4::RuntimeID(lo, "Small.esp", 0x801) == 0xFE001801 && tf4::RuntimeID(lo, "Gone.esp", 0x801) == 0, "fo4 transplant: light plugin, missing plugin");
	Check(tf4::LocalOf(0xFE001801) == 0x801 && tf4::LocalOf(0x02000801) == 0x000801, "fo4 transplant: local ids");
	auto lo2 = lo;
	Check(tf4::SameLoadOrder(lo, lo2), "fo4 transplant: same load order");
	std::swap(lo2.lite[0], lo2.lite[1]);
	Check(!tf4::SameLoadOrder(lo, lo2), "fo4 transplant: light plugins moved: not the same load order");
	{
		tf4::Change c;
		c.kind = tf4::Kind::kItem;
		c.via = tf4::Via::kGame;
		c.count = 9;
		const auto r = tf4::GameRecords(c, 0x00024F55, true);
		Check(r.size() == 1 && r[0] == ess::transplant::Record{ "additem", "00024F55", "9" }, "fo4 transplant: additem record");
		c.kind = tf4::Kind::kQuest;
		c.value = 100;
		c.now = 20;
		c.complete = true;
		const auto q = tf4::GameRecords(c, 0xFE001801, true);
		Check(q.size() == 2 && q[0] == ess::transplant::Record{ "setstage", "FE001801", "100" } && q[1] == ess::transplant::Record{ "completequest", "FE001801" },
			"fo4 transplant: setstage then completequest");
		c.now = 100;
		Check(tf4::GameRecords(c, 0xFE001801, false).empty(), "fo4 transplant: no setstage for a stage done already, no completequest when not asked");
		c.via = tf4::Via::kFile;
		Check(tf4::GameRecords(c, 0x14, true).empty(), "fo4 transplant: a file change has no helper record");
		tf4::Change o;
		o.kind = tf4::Kind::kObjective;
		o.via = tf4::Via::kGame;
		o.objective = 40;
		o.state = 3;
		const auto orec = tf4::GameRecords(o, 0x0001CB51, true);
		Check(orec.size() == 1 && orec[0] == ess::transplant::Record{ "objective", "0001CB51", "40", "3" }, "fo4 transplant: objective record");
		o.state = 0;
		Check(tf4::GameRecords(o, 0x0001CB51, true).empty(), "fo4 transplant: no objective record for a dormant state");
		o.state = 6;
		Check(tf4::GameRecords(o, 0x0001CB51, true).empty(), "fo4 transplant: no objective record for a state the game does not have");
	}
	{
		// what makes two stacks the same thing: mods, name, condition; never the count
		tf4::StackData a, b;
		a.count = 1;
		b.count = 5;
		Check(a.key() == b.key() && a.plain(), "fo4 transplant: plain stacks are the same thing");
		a.hasMods = b.hasMods = true;
		a.modTemplate = b.modTemplate = 4;
		tf4::ObjectMod m;
		m.mod = { false, "Fallout4.esm", 0x1F2D44 };
		m.rank = 1;
		a.mods.push_back(m);
		b.mods.push_back(m);
		b.mods.back().mod.plugin = "FALLOUT4.ESM";
		Check(a.key() == b.key() && !a.plain(), "fo4 transplant: the same mods (plugin case does not matter)");
		b.mods.back().mod.local = 0x1F2D45;
		Check(a.key() != b.key(), "fo4 transplant: another mod is another thing");
		b.mods = a.mods;
		a.hasText = b.hasText = a.hasName = b.hasName = true;
		a.textValue = b.textValue = -2;
		a.name = "Pipe Pistol";
		b.name = "My Pistol";
		Check(a.key() != b.key(), "fo4 transplant: another name is another thing");
		b.name = a.name;
		a.hasHealth = true;
		a.health = 0.5f;
		Check(a.key() != b.key(), "fo4 transplant: condition counts");
	}

	const char* env = std::getenv("RESAVERPP_FO4_SAVES");
	if (!env || !*env) {
		std::printf("(fo4 transplant: RESAVERPP_FO4_SAVES not set, real saves skipped)\n");
		return;
	}
	std::vector<fs::path> files;
	std::string           all = env;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) {
			e = all.size();
		}
		const fs::path dir = all.substr(p, e - p);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
				if (f.path().extension() == ".fos") {
					files.push_back(f.path());
				}
			}
		}
		p = e + 1;
	}
	std::sort(files.begin(), files.end());
	std::size_t read = 0;
	fs::path    save7, save8;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;  // (some test folders keep cut saves on purpose)
		}
		const auto snap = tf4::FromSave(*s, f.filename().string());
		if (!snap.valid) {
			std::printf("FAIL: fo4 transplant: %s: %s\n", f.filename().string().c_str(), snap.error.c_str());
			++g_fail;
			continue;
		}
		++read;
		const auto self = tf4::Diff(snap, snap, *s);
		if (!self.empty()) {
			std::printf("FAIL: fo4 transplant: %s compared with itself lists %zu change(s)\n", f.filename().string().c_str(), self.size());
			++g_fail;
		}
		const auto name = f.filename().string();
		if (name.rfind("Save7_F110F019", 0) == 0) save7 = f;
		if (name.rfind("Save8_F110F019", 0) == 0) save8 = f;
	}
	Check(read > 0, "fo4 transplant: real saves read offline");
	std::printf("(fo4 transplant: %zu real saves read offline, each compared with itself)\n", read);

	if (!save7.empty() && !save8.empty()) {
		// Tony's Save7 (before) and Save8 (after dropping 9 Pipe Pistols, Fallout4.esm 00024F55)
		auto       s7 = SaveFile::load(save7);
		auto       s8 = SaveFile::load(save8);
		const auto a = tf4::FromSave(*s7, "Save7");
		const auto b = tf4::FromSave(*s8, "Save8");
		Check(a.perksKnown && a.itemsKnown && b.perksKnown && b.itemsKnown && a.xp >= 0 && a.special[0] > 0, "fo4 transplant: Save7 / Save8 read fully");
		Check(a.inventoryWritable && b.inventoryWritable, "fo4 transplant: Save7 / Save8: the player reads completely (items go into the file)");
		const auto d = tf4::Diff(b, a, *s8);  // good Save8, old Save7
		Check(d.size() == 1 && d[0].kind == tf4::Kind::kItem && d[0].via == tf4::Via::kFile && d[0].goodFormID == 0x00024F55 && d[0].count == 9 &&
				  d[0].available && d[0].tick,
			"fo4 transplant: Save7 -> Save8 lists the 9 Pipe Pistols (00024F55), to be written into the save");
		// Save7's four stacks the Save8 player lacks: 1 + 3 + 4 named "Pipe Pistol" with the same 6 mods (one stack), 1 with 13 mods
		bool stacksOk = d.size() == 1 && d[0].stacks.size() == 2;
		if (stacksOk) {
			const auto& x = d[0].stacks[0].count == 8 ? d[0].stacks[0] : d[0].stacks[1];
			const auto& y = d[0].stacks[0].count == 8 ? d[0].stacks[1] : d[0].stacks[0];
			stacksOk = x.count == 8 && x.mods.size() == 6 && x.hasName && x.name == "Pipe Pistol" && x.modTemplate == 4 && y.count == 1 &&
			           y.mods.size() == 13 && y.hasName && y.name == "Pipe Pistol" && y.modTemplate == 0x0F && x.problem.empty() && y.problem.empty();
			for (const auto& m : x.mods) {
				stacksOk = stacksOk && m.mod.plugin == "Fallout4.esm" && m.rank == 1;
			}
		}
		Check(stacksOk, "fo4 transplant: Save7 -> Save8: the pistols' stacks with their mods and names");
		Check(d.empty() || tf4::GameRecords(d[0], d[0].goodFormID, true).empty(), "fo4 transplant: Save7 -> Save8: no helper record (written into the file)");
		if (d.size() == 1) {
			auto       s8w = SaveFile::load(save8);
			const auto why = tf4::ApplyToFile(*s8w, d[0]);
			if (!why.empty()) {
				std::printf("  (%s)\n", why.c_str());
			}
			Check(why.empty(), "fo4 transplant: Save7 -> Save8: the pistols written into the save");
			const auto bytes = s8w->serialize();
			const auto tmp = fs::temp_directory_path() / "rs_unit_tf4_s8.fos";
			{
				std::ofstream out(tmp, std::ios::binary);
				out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}
			auto       back = SaveFile::load(tmp);
			const auto c = tf4::FromSave(*back, "written");
			Check(c.inventoryWritable && tf4::Diff(c, a, *back).empty(), "fo4 transplant: Save8 with the pistols read back: nothing left to bring over");
			const auto it = c.items.find(ess::transplant::FormKey{ "fallout4.esm", 0x24F55 });
			std::size_t named = 0;
			if (it != c.items.end()) {
				for (const auto& st : it->second.stacks) {
					named += st.hasName && st.name == "Pipe Pistol" && st.mods.size() >= 6 ? static_cast<std::size_t>(st.count) : 0;
				}
			}
			Check(it != c.items.end() && it->second.count == 16 && named == 9, "fo4 transplant: Save8 now has 16 Pipe Pistols, 9 of them modded and named as in Save7");
			const auto* p = ess::fo4cf::FindPlayer(*back);
			Check(p && ess::fo4cf::DecodeActor(*back, *p).complete, "fo4 transplant: the written player reads to the end of its body");
			fs::remove(tmp);
		}
		Check(tf4::Diff(a, b, *s7).empty(), "fo4 transplant: Save8 -> Save7: nothing (Save8 has less)");
	} else {
		std::printf("(fo4 transplant: Tony's Save7 / Save8 not found, that check skipped)\n");
	}

	// the file part written into the good save: compared again, only the game part is left
	fs::path good, old;
	for (const auto& f : files) {
		const auto n = f.filename().string();
		if (n.rfind("Save8_F6EE2084", 0) == 0) good = f;
		if (n.rfind("Save14_F6EE2084", 0) == 0) old = f;
	}
	if (!good.empty() && !old.empty()) {
		auto       g = SaveFile::load(good);
		auto       o = SaveFile::load(old);
		const auto bad = tf4::FromSave(*o, "old");
		auto       d = tf4::Diff(tf4::FromSave(*g, "good"), bad, *g);
		const auto ids0 = g->formIDs.size();
		std::size_t file = 0, written = 0, game = 0;
		for (const auto& c : d) {
			if (c.via == tf4::Via::kFile && c.available) {
				++file;
				written += tf4::ApplyToFile(*g, c).empty() ? 1 : 0;
			}
		}
		const auto bytes = g->serialize();
		const auto tmp = fs::temp_directory_path() / "rs_unit_tf4.fos";
		{
			std::ofstream out(tmp, std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		auto       back = SaveFile::load(tmp);
		const auto after = tf4::Diff(tf4::FromSave(*back, "written"), bad, *back);
		std::size_t fileLeft = 0;
		for (const auto& c : after) {
			fileLeft += c.via == tf4::Via::kFile ? 1 : 0;
			game += c.via == tf4::Via::kGame ? 1 : 0;
		}
		Check(file > 0 && written == file, "fo4 transplant: the file part is written into the good save");
		Check(fileLeft == 0 && game > 0, "fo4 transplant: after writing and reading the save back, only the game part is left");
		Check(back->header.playerLevel == static_cast<std::uint32_t>(bad.level), "fo4 transplant: the level in the written save's header");
		std::size_t items = 0;
		for (const auto& c : d) {
			items += c.kind == tf4::Kind::kItem && c.via == tf4::Via::kFile ? 1 : 0;
		}
		const auto* pl = ess::fo4cf::FindPlayer(*back);
		Check(items > 0 && back->formIDs.size() > ids0 && pl && ess::fo4cf::DecodeActor(*back, *pl).complete,
			"fo4 transplant: items with their extra data written (form ids added to the array where needed), the player still reads completely");
		fs::remove(tmp);
	}
}

// Fallout 4 Reset Havok and Purify FormLists: synthetic change forms, then (RESAVERPP_FO4_SAVES) real saves reset, written,
// read back and decoded again; form lists get null entries put in, purified, written, read back: equal to the original.
// Fallout 4 ACHR: the package data (procedure trees) and the movement pieces read through fo4cf::detail, on bytes laid
// out as the game's loaders read them; then, with RESAVERPP_FO4_SAVES, every NPC of real saves and the removal of a
// plugin's items from NPCs (written, read back, the actor part kept byte for byte).
void TestFo4ActorPackages()
{
	namespace fs = std::filesystem;
	using B = std::vector<std::uint8_t>;
	auto u8 = [](B& b, std::uint8_t v) { b.push_back(v); };
	auto u16 = [](B& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); };
	auto u32 = [](B& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
	auto ref = [](B& b, std::uint32_t formID) { const std::uint32_t d = formID ? ((1u << 22) | formID) : 0; b.push_back(d >> 16); b.push_back(d >> 8); b.push_back(d); };
	auto vs = [](B& b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v << 2)); };  // below 64
	auto ws = [&](B& b, const std::string& t) { u16(b, static_cast<std::uint16_t>(t.size())); b.insert(b.end(), t.begin(), t.end()); };
	// reads a_b as package data of a_type: "" when it ends exactly at the end, else what happened
	auto run = [](const B& a_b, std::uint8_t a_type, std::uint32_t a_ver = 68) -> std::string {
		ess::Reader r(a_b.data(), a_b.size());
		ess::fo4cf::detail::In in{ r, a_ver };
		try {
			ess::fo4cf::detail::PackageData(in, a_type);
		} catch (const std::exception& e) {
			return e.what();
		}
		return r.pos() == a_b.size() ? std::string() : "stopped at " + std::to_string(r.pos()) + " of " + std::to_string(a_b.size());
	};
	{  // a procedure tree (type 18): one data object (an ObjectList with one entry), Sequence > SitSleep
		B b;
		u32(b, 3);
		vs(b, 1);
		u8(b, 0x17);
		ws(b, "ObjectList");
		vs(b, 1);
		ref(b, 0x33B2); ref(b, 0x100); u32(b, 1); u32(b, 3); u32(b, 0x438A1856); u32(b, 0x42C80000);
		ws(b, "Sequence");
		ws(b, "SitSleep");
		u8(b, 0); u8(b, 0); u8(b, 0); u8(b, 0);  // 3 bytes (form version 0x29 and up) and no Activate child
		u32(b, 0);                               // Sequence's own u32
		Check(run(b, 18).empty(), "fo4 package: procedure tree with an ObjectList and Sequence > SitSleep reads exactly");
		B old(b);
		old.erase(old.end() - 6);  // the version byte SitSleep reads from form version 0x29 only
		Check(run(old, 18, 0x28).empty() && !run(old, 18).empty(), "fo4 package: SitSleep's third byte depends on the form version");
	}
	{  // Stacked > Travel with a LocRef location; Simultaneous with Wait and an empty name (no object, nothing read)
		B b;
		u32(b, 0); vs(b, 0);
		ws(b, "Stacked");
		ws(b, "Travel");
		u8(b, 0); u8(b, 0); u8(b, 0); u8(b, 1);
		ws(b, "LocRef"); ref(b, 0x1234); u32(b, 0); u32(b, 0); u32(b, 0); u8(b, 0);
		u8(b, 0);
		u32(b, 0);  // pointer id 0 (+0x68): nothing follows
		u8(b, 1);   // form version 0x2d and up
		u32(b, 7);  // Stacked's u32
		Check(run(b, 18).empty(), "fo4 package: Stacked > Travel with a LocRef reads exactly");
		B c;
		u32(c, 0); vs(c, 0);
		ws(c, "Simultaneous");
		vs(c, 2);
		u32(c, 1); ws(c, "Wait"); u32(c, 5);
		u32(c, 2); ws(c, "");
		Check(run(c, 18).empty(), "fo4 package: Simultaneous children, an empty class name reads nothing");
		B d = c;
		d.insert(d.end(), { 9, 9 });
		Check(!run(d, 18).empty(), "fo4 package: bytes left over are not taken as read");
		B e;
		u32(e, 0); vs(e, 0); ws(e, "NoSuchState");
		Check(run(e, 18).find("not decoded") != std::string::npos, "fo4 package: an unknown state class stops the decoder");
	}
	{  // types 2, 13, 14 and the types without data
		B t2; ref(t2, 0x55);
		Check(run(t2, 2).empty(), "fo4 package: type 2 is a form id");
		B t13; u32(t13, 1); u32(t13, 2); u32(t13, 3); u8(t13, 0); u8(t13, 1); u32(t13, 4); vs(t13, 2); ref(t13, 0x10); ref(t13, 0x11);
		Check(run(t13, 13).empty(), "fo4 package: type 13 (u32 x3, u8 x2, u32, form ids)");
		B t14; u32(t14, 0); u32(t14, 0); u32(t14, 0); ref(t14, 0x20);
		u8(t14, 1); ws(t14, "Prim"); ref(t14, 0x21); u8(t14, 0);
		u8(t14, 0); u8(t14, 0); u8(t14, 0);
		u8(t14, 0); u8(t14, 0);
		u8(t14, 1); u8(t14, 2); u32(t14, 0); u32(t14, 9);  // a target of kind 2: a u32
		Check(run(t14, 14).empty(), "fo4 package: type 14 with a Prim location and a target");
		Check(run({}, 16).empty() && run({}, 36).empty(), "fo4 package: types 16 and 36 read nothing");
	}

	{  // extra data 0x5c (each entry has its own list of form id + u32), 0x1b, 0xd3, 0xd5 in a REFR (change flag 10)
		B e;  // Head has two (form id, u32), SPINE1 none
		vs(e, 4);
		u8(e, 0x5c);
		u16(e, 0); u32(e, 0); u32(e, 0); u8(e, 0);
		ref(e, 0);
		vs(e, 2);
		u32(e, 0x40140800); u8(e, 1); u8(e, 0); u8(e, 0); u8(e, 0); u8(e, 0); ws(e, "Head");
		vs(e, 2); ref(e, 0x4ED47); u32(e, 0xFFFFFFFF); ref(e, 0xEEB4); u32(e, 0xFFFFFFFF);
		u32(e, 0x40140800); u8(e, 0); u8(e, 1); u8(e, 1); u8(e, 1); u8(e, 0); ws(e, "SPINE1");
		vs(e, 0);
		u8(e, 0x1b); vs(e, 2); ref(e, 0x10); u8(e, 1); ref(e, 0x11); u8(e, 0);
		u8(e, 0xd3); u32(e, 7);
		u8(e, 0xd5); ref(e, 0x12);
		ess::SaveFile save;
		save.header.magic = "FO4_SAVEGAME";
		save.formVersion = 68;
		ess::ChangeForm c;
		c.refID.data = (1u << 22) | 0xC01;
		c.typeField = 0;
		c.changeFlags = 1u << 10;
		c.data = e;
		c.length1 = static_cast<std::uint32_t>(e.size());
		const auto d = ess::fo4cf::DecodeRefr(save, c);
		Check(d.complete && d.extras.size() == 4 && d.extras[0].type == 0x5c && d.extras[3].type == 0xd5,
			"fo4 extra data: 0x5c entries with their own lists, 0x1b, 0xd3 and 0xd5 read exactly");
	}

	const char* env = std::getenv("RESAVERPP_FO4_SAVES");
	if (!env || !*env) {
		std::printf("(fo4 actors: RESAVERPP_FO4_SAVES not set, real saves skipped)\n");
		return;
	}
	std::vector<fs::path> files;
	std::string           all = env;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) e = all.size();
		const fs::path dir = all.substr(p, e - p);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
				if (f.path().extension() == ".fos") files.push_back(f.path());
			}
		}
		p = e + 1;
	}
	std::sort(files.begin(), files.end());
	const auto tmp = fs::temp_directory_path() / "rs_unit_npc.fos";
	std::size_t tested = 0;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;
		}
		if (!s->isFO4()) continue;
		const auto  name = f.filename().string();
		std::size_t npcs = 0, complete = 0, refrRead = 0;
		std::map<std::uint32_t, std::size_t> perPlugin;  // items held by NPCs read completely, by top byte
		for (const auto& c : s->changeForms) {
			if (c.typeCode() != 1 || s->formIDOf(c.refID) == 0x14) continue;
			++npcs;
			const auto r = ess::fo4cf::DecodeRefr(*s, c);
			if (!r.decoded) continue;
			++refrRead;
			const auto a = ess::fo4cf::DecodeActor(*s, c);
			complete += a.complete;
			if (a.complete) {
				for (const auto& it : r.inventory) {
					if ((it.formID >> 24) != 0) ++perPlugin[it.formID >> 24];
				}
			}
		}
		// every NPC must read to the end of its body (TestFo4Combat checks every ACHR, the player included)
		Check(refrRead > 0 && complete == npcs, ("fo4 actors: every NPC reads completely in " + name + " (" +
		      std::to_string(complete) + " of " + std::to_string(refrRead) + ")").c_str());
		if (perPlugin.empty()) continue;
		const auto plugin = std::max_element(perPlugin.begin(), perPlugin.end(), [](auto& x, auto& y) { return x.second < y.second; })->first;
		std::set<std::uint32_t> items;
		for (const auto& c : s->changeForms) {
			if (c.typeCode() != 1) continue;
			for (const auto& it : ess::fo4cf::DecodeRefr(*s, c).inventory) {
				if ((it.formID >> 24) == plugin) items.insert(it.formID);
			}
		}
		std::map<std::uint32_t, std::vector<std::uint8_t>> actorParts;  // refid -> the actor part before the edit
		std::size_t removed = 0, playerHad = 0, playerRemoved = 0;
		for (auto& c : s->changeForms) {
			if (c.typeCode() != 1) continue;
			const auto r = ess::fo4cf::DecodeRefr(*s, c);
			const auto body = ess::cf::Body(c);
			std::size_t n = 0;
			const auto  e = ess::cf::RemoveInventoryItems(*s, c, items, &n);
			if (s->formIDOf(c.refID) == 0x14) {  // the player reads completely too now: edited like an NPC
				for (const auto& it : r.inventory) playerHad += items.contains(it.formID);
				playerRemoved = e.empty() ? n : 0;
			}
			if (e.empty() && n) {
				removed += n;
				actorParts[c.refID.data] = std::vector<std::uint8_t>(body.begin() + static_cast<std::ptrdiff_t>(r.refrPart.end), body.end());
			}
		}
		{
			const auto bytes = s->serialize();
			std::ofstream out(tmp, std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		auto        again = SaveFile::load(tmp);
		std::size_t ok = 0;
		for (const auto& c : again->changeForms) {
			const auto at = actorParts.find(c.refID.data);
			if (c.typeCode() != 1 || at == actorParts.end()) continue;
			const auto r = ess::fo4cf::DecodeRefr(*again, c);
			const auto a = ess::fo4cf::DecodeActor(*again, c);
			const auto body = ess::cf::Body(c);
			bool clean = r.decoded && a.complete;
			for (const auto& it : r.inventory) clean = clean && !items.contains(it.formID);
			clean = clean && std::equal(at->second.begin(), at->second.end(), body.begin() + static_cast<std::ptrdiff_t>(r.refrPart.end), body.end());
			ok += clean;
		}
		Check(removed > 0 && ok == actorParts.size(), ("fo4 actors: a plugin's items removed from " + std::to_string(actorParts.size()) +
		      " NPCs of " + name + ", written and read back completely, actor parts unchanged").c_str());
		Check(playerRemoved == playerHad, ("fo4 actors: the player's items of that plugin removed too in " + name).c_str());
		++tested;
	}
	std::error_code ec;
	fs::remove(tmp, ec);
	std::printf("(fo4 actors: %zu real saves)\n", tested);
}

// Fallout 4 actors: the combat controller (CombatController::LoadGame 0x87c540) and the combat objects (0xed5cf0), the
// extra data lists 0x32 / 0x34 / 0x44 / 0x87 and 0x9d, a created package's data object (mask bit 4) and the package
// data class names the game registers; on bytes laid out as the game reads them (layouts checked against the game's
// loader in tools/fo4emu), then with RESAVERPP_FO4_SAVES every NPC of every real save read completely.
void TestFo4Combat()
{
	using B = std::vector<std::uint8_t>;
	auto u8 = [](B& b, std::uint8_t v) { b.push_back(v); };
	auto u16 = [](B& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); };
	auto u32 = [](B& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
	auto u64 = [&](B& b, std::uint64_t v) { u32(b, static_cast<std::uint32_t>(v)); u32(b, static_cast<std::uint32_t>(v >> 32)); };
	auto raw = [](B& b, std::size_t n) { b.insert(b.end(), n, 0); };
	auto ref = [](B& b, std::uint32_t formID) { const std::uint32_t d = formID ? ((1u << 22) | formID) : 0; b.push_back(d >> 16); b.push_back(d >> 8); b.push_back(d); };
	auto vs = [](B& b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v << 2)); };  // below 64
	auto ws = [&](B& b, const std::string& t) { u16(b, static_cast<std::uint16_t>(t.size())); b.insert(b.end(), t.begin(), t.end()); };
	// reads a_b with a_fn: "" when it ends exactly at the end, else what happened
	auto run = [](const B& a_b, auto a_fn, std::uint32_t a_ver = 68) -> std::string {
		ess::Reader r(a_b.data(), a_b.size());
		ess::fo4cf::detail::In in{ r, a_ver };
		try {
			a_fn(in);
		} catch (const std::exception& e) {
			return e.what();
		}
		return r.pos() == a_b.size() ? std::string() : "stopped at " + std::to_string(r.pos()) + " of " + std::to_string(a_b.size());
	};
	auto object = [](ess::fo4cf::detail::In& in) { ess::fo4cf::detail::Object(in); };

	{  // CombatAreaHoldPosition: the type registry's values (0x89cb10) take the FIRST entry with the hash (0xE9DCB5DD:
	   // entry 3, 32 bytes, not entry 5's 52), an unknown hash entry 0 (12 bytes), 0xFFFFFFFF nothing
		B b;
		u64(b, 0x1CEAFB23A11ull);
		u32(b, 0x55FAB455);
		ref(b, 0x10); u32(b, 1); u8(b, 0); u8(b, 1); u32(b, 0); u32(b, 0); u32(b, 0xFFFFFFFF); ref(b, 0);  // the area part
		ref(b, 0x11); u32(b, 0); u32(b, 0);
		vs(b, 2);
		u32(b, 0); u32(b, 0xE9DCB5DD); u16(b, 0); u8(b, 0); raw(b, 32); ref(b, 0); ref(b, 0x12);
		u32(b, 1); u32(b, 0x12345678); u16(b, 0); u8(b, 0); raw(b, 12); ref(b, 0); ref(b, 0);
		u32(b, 0); u32(b, 0); u32(b, 0);
		Check(run(b, object).empty(), "fo4 combat: CombatAreaHoldPosition with registry values (first entry with the hash; unknown hash -> entry 0)");
		B even; u64(even, 0x1CEAFB23A10ull);
		Check(run(even, object).empty(), "fo4 combat: an even object id reads nothing more");
		B none; u64(none, 0);
		Check(run(none, object).empty(), "fo4 combat: object id 0 is no object");
	}
	{  // an odd id seen again is loaded by the object already known under it (0xed5b40), whatever class id follows
		B b;
		u64(b, 0x2001ull); u32(b, 0xA0DD2269); ref(b, 0x20);   // CombatPositionTracker: a form id
		u64(b, 0x2001ull); u32(b, 0xC8CC82FC); ref(b, 0x21);   // the same object again: still a form id, not 20 bytes
		Check(run(b, [&](ess::fo4cf::detail::In& in) { object(in); object(in); }).empty(),
			"fo4 combat: an odd object id read again is loaded by its first class");
		B u; u64(u, 0x3001ull); u32(u, 0xDEADBEEF);
		Check(run(u, object).find("0xDEADBEEF") != std::string::npos, "fo4 combat: an unknown object class stops the decoder");
	}
	{  // the combat controller with every part: named values and the tree state are sized blocks
		auto build = [&](bool a_new) {
			B b;
			ref(b, 0x30); ref(b, 0x14); u32(b, 0x72); u32(b, 1); u32(b, 0);
			for (int i = 0; i < 5; ++i) u8(b, 0);
			u32(b, 0); u32(b, 0); u32(b, 0);
			vs(b, 1); u64(b, 0x4001ull); u32(b, 0xC5642853); raw(b, 12); u32(b, 3); u32(b, 2); u32(b, 0); u32(b, 0);
			u32(b, 0xFFFFFFFF); u32(b, 0xFFFFFFFF);
			vs(b, 0);
			u32(b, 0);
			vs(b, 1); u64(b, 0x4003ull); u32(b, 0x5ACB99C8); ref(b, 0x31); u32(b, 0); u32(b, 1); ref(b, 0x32); u16(b, 2); u16(b, 0); u32(b, 0); u32(b, 7);
			u32(b, 0);
			vs(b, 0);
			u64(b, 0x4000ull);  // an object read before
			u64(b, 0);
			vs(b, 0);
			vs(b, 1); ws(b, "Key"); ws(b, "Value");
			// 0xeda730
			for (int i = 0; i < 4; ++i) u8(b, 0);
			if (a_new) u8(b, 0);  // form version 0x14 and up
			for (int i = 0; i < 4; ++i) u8(b, 0);
			u32(b, 0); raw(b, 12); raw(b, 12); u32(b, 0); u32(b, 0); u8(b, 4); u8(b, 4); u32(b, 0); u32(b, 0); u8(b, 0); u32(b, 0);
			for (int i = 0; i < 8; ++i) u32(b, 0);
			vs(b, 1); ref(b, 0x33); raw(b, 12); ref(b, 0); u32(b, 0); u32(b, 0);
			vs(b, 1); raw(b, 12); ref(b, 0x34); u32(b, 0);
			vs(b, 2); ref(b, 0x35); ref(b, 0x36);
			u8(b, 0); u32(b, 0); u32(b, 0); u32(b, 0); u32(b, 0);
			// 0xea7100
			for (int i = 0; i < 13; ++i) u32(b, 0);
			u16(b, 0);
			if (a_new) u16(b, 0);  // form version 0x33 and up
			u32(b, 0); u32(b, 0); u16(b, 2);
			for (int i = 0; i < 7; ++i) vs(b, 0);
			vs(b, 0); u16(b, 3); u32(b, 0); u32(b, 0); u32(b, 0); u32(b, 0);
			vs(b, 0); u16(b, 3); u32(b, 0); u32(b, 0); u32(b, 0); u32(b, 0);
			vs(b, 0);
			vs(b, 1); ref(b, 0x37); u32(b, 0);
			vs(b, 0);
			vs(b, 0);
			vs(b, 0);
			// 0xe75e30: two named values (sized: the reader ends at the saved size)
			vs(b, 2); ws(b, "DialogueAttackTimer"); vs(b, 8); u32(b, 0); u32(b, 0x40A00000);
			ws(b, "NotAKnownName"); vs(b, 3); raw(b, 3);
			u32(b, 0);
			// 0xeff4d0
			u32(b, 0x16); u8(b, 1); vs(b, 16); raw(b, 16);
			vs(b, 1); u32(b, 0x15);
			vs(b, 1); ws(b, "Node"); u32(b, 1); vs(b, 1); u32(b, 2); vs(b, 1); u32(b, 3); u32(b, 4);
			return b;
		};
		auto cc = [](ess::fo4cf::detail::In& in) { ess::fo4cf::detail::CombatController(in); };
		Check(run(build(true), cc).empty(), "fo4 combat: the combat controller (0x87c540) with every part reads exactly");
		Check(run(build(false), cc, 0x13).empty() && !run(build(true), cc, 0x13).empty(),
			"fo4 combat: the combat controller's form version branches (0x14, 0x33)");
	}

	{  // extra data lists: 0x32 (form id + active effects), 0x34, 0x44, 0x87 with entries; 0x9d a form id
		B e;
		vs(e, 7);
		u8(e, 0x79); u32(e, 1); u32(e, 2);
		u8(e, 0xa9); u32(e, 1); ref(e, 0x46); ref(e, 0x47); u8(e, 0);
		u8(e, 0x32); ref(e, 0x40); vs(e, 1); ref(e, 0x41); u8(e, 0); vs(e, 2); vs(e, 4); u32(e, 0);
		u8(e, 0x34); vs(e, 2); u32(e, 1); u32(e, 2); u32(e, 3); u32(e, 4);
		u8(e, 0x44); vs(e, 3); u32(e, 0); u32(e, 0); u32(e, 0);
		u8(e, 0x87); raw(e, 12); ref(e, 0x42); u32(e, 0); vs(e, 1); raw(e, 12); ref(e, 0x43); raw(e, 12); ref(e, 0x44); u8(e, 1);
		u8(e, 0x9d); ref(e, 0x45);
		ess::SaveFile save;
		save.header.magic = "FO4_SAVEGAME";
		save.formVersion = 68;
		ess::ChangeForm c;
		c.refID.data = (1u << 22) | 0xC02;
		c.typeField = 0;
		c.changeFlags = 1u << 10;
		c.data = e;
		c.length1 = static_cast<std::uint32_t>(e.size());
		const auto d = ess::fo4cf::DecodeRefr(save, c);
		Check(d.complete && d.extras.size() == 7 && d.extras[0].range.size() == 9 && d.extras[1].range.size() == 12 &&
		          d.extras[3].type == 0x34 && d.extras[6].type == 0x9d &&
		          d.extras[3].range.size() == 18 && d.extras[5].range.size() == 1 + 12 + 3 + 4 + 1 + 12 + 3 + 12 + 3 + 1,
			"fo4 extra data: 0x32, 0x34, 0x44 and 0x87 are lists, 0x9d a form id, 0x79 two u32, 0xa9 u32 + two form ids + u8");
	}
	{  // a created package's data object (mask bit 4): Follow, UseItem, Patrol, Custom; none for other types
		auto pkg = [&](int a_type, const B& a_data) {
			B b; raw(b, 12); u8(b, 4); b.insert(b.end(), a_data.begin(), a_data.end()); u32(b, 0);
			return run(b, [a_type](ess::fo4cf::detail::In& in) { ess::fo4cf::detail::CreatedPackage(in, static_cast<std::uint8_t>(a_type)); });
		};
		B follow; u32(follow, 5); u8(follow, 1); u8(follow, 0); u32(follow, 0); ref(follow, 0x50);
		B useItem; u8(useItem, 0);
		B patrol; u8(patrol, 1); u8(patrol, 0);
		Check(pkg(1, follow).empty() && pkg(8, useItem).empty() && pkg(13, patrol).empty() && pkg(18, {}).empty() && pkg(19, {}).empty(),
			"fo4 package: a created package's data object (Follow, UseItem, Patrol, Custom)");
		Check(!pkg(2, {}).empty(), "fo4 package: a created package type without a data object stops the decoder");
	}
	{  // package data objects by the names the game registers: SingleRef (a target), Ref (nothing), LocWrapper
		B b;
		u32(b, 0);
		vs(b, 4);
		u8(b, 0); ws(b, "SingleRef"); u8(b, 0); u32(b, 0); ref(b, 0x60);
		u8(b, 1); ws(b, "Ref");
		u8(b, 2); ws(b, "LocWrapper"); u8(b, 1); ws(b, "Prim"); ref(b, 0x61); u8(b, 0);
		u8(b, 3); ws(b, "LocationSelector"); u8(b, 5); u32(b, 0); u32(b, 9);
		ws(b, "");
		auto pd = [](ess::fo4cf::detail::In& in) { ess::fo4cf::detail::PackageData(in, 18); };
		Check(run(b, pd).empty(), "fo4 package: data objects SingleRef, Ref, LocWrapper and LocationSelector read as the game's classes");
		B old;
		u32(old, 0); vs(old, 1); u8(old, 0); ws(old, "LocationWrapper"); u8(old, 0); ws(old, "");
		Check(!run(old, pd).empty(), "fo4 package: a name the game does not register stops the decoder");
	}

	const char* env = std::getenv("RESAVERPP_FO4_SAVES");
	if (!env || !*env) {
		std::printf("(fo4 combat: RESAVERPP_FO4_SAVES not set, real saves skipped)\n");
		return;
	}
	namespace fs = std::filesystem;
	std::vector<fs::path> files;
	std::string           all = env;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) e = all.size();
		const fs::path dir = all.substr(p, e - p);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
				if (f.path().extension() == ".fos") files.push_back(f.path());
			}
		}
		p = e + 1;
	}
	std::sort(files.begin(), files.end());
	std::size_t tested = 0, combat = 0;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;
		}
		if (!s->isFO4()) continue;
		std::size_t achr = 0, complete = 0;
		std::string first;
		for (const auto& c : s->changeForms) {
			if (c.typeCode() != 1) continue;
			++achr;
			const auto a = ess::fo4cf::DecodeActor(*s, c);
			complete += a.complete;
			combat += a.complete && a.hasCombat;
			if (!a.complete && first.empty()) first = a.error;
		}
		Check(achr > 0 && complete == achr, ("fo4 combat: every ACHR of " + f.filename().string() + " read to the end of its body (" +
		      std::to_string(complete) + " of " + std::to_string(achr) + (first.empty() ? "" : "; " + first) + ")").c_str());
		++tested;
	}
	Check(tested == 0 || combat > 0, "fo4 combat: the real saves have actors with a combat controller, read completely");
	std::printf("(fo4 combat: %zu real saves, %zu actors with a combat controller)\n", tested, combat);
}

// Fallout 4 active effect removal: the rules of PlanEffectRemoval (what the game's end of each effect undoes, see
// ChangeFormFO4.h), then with RESAVERPP_FO4_SAVES every actor of every real save: its removable effects removed (with
// a value modifier's strength taken off its modifier, the four vitals included), written, read back exactly.
void TestFo4ActiveEffects()
{
	namespace f4 = ess::fo4cf;
	{
		f4::Actor pc;
		pc.player = true;
		f4::Actor npc;
		f4::ActiveEffect e;
		e.data = { 0, 40 };
		e.archetype = 34;
		e.flags = 0x14200;  // started, magnitude as saved, inactive bit 0x4000 only
		e.magnitude = 2;
		f4::EffectSource ab;
		ab.known = true;
		ab.mgefFlags = 0xa02;  // recover
		ab.actorValue = 0x2C2;
		std::memcpy(ab.itemType, "SPEL", 4);
		ab.spellType = 4;
		ab.castType = 0;
		auto u = f4::PlanEffectRemoval(pc, e, &ab);
		Check(u.ok && u.modifier && u.slot == 0 && u.amount == 2 && u.actorValue == 0x2C2, "fo4 effects: the player's ability -> modifier slot 0");
		u = f4::PlanEffectRemoval(npc, e, &ab);
		Check(u.ok && u.modifier && u.slot == 1, "fo4 effects: an NPC's ability -> slot 1");
		auto en = ab;
		std::memcpy(en.itemType, "ENCH", 4);
		en.spellType = 6;
		Check(f4::PlanEffectRemoval(pc, e, &en).slot == 0, "fo4 effects: the player's constant effect enchantment -> slot 0");
		en.castType = 1;
		Check(f4::PlanEffectRemoval(pc, e, &en).slot == 1, "fo4 effects: the player's fire and forget enchantment -> slot 1");
		auto al = ab;
		std::memcpy(al.itemType, "ALCH", 4);
		al.spellType = 7;
		al.castType = 1;
		Check(f4::PlanEffectRemoval(pc, e, &al).slot == 0, "fo4 effects: the player's potion on S.P.E.C.I.A.L. -> slot 0");
		al.actorValue = 0x2D4;
		Check(f4::PlanEffectRemoval(pc, e, &al).slot == 0, "fo4 effects: the player's potion on Health -> slot 0");
		al.actorValue = 0x2D7;
		Check(f4::PlanEffectRemoval(pc, e, &al).slot == 1, "fo4 effects: the player's potion on another game actor value -> slot 1");
		al.poison = true;
		al.actorValue = 0x2C2;
		Check(f4::PlanEffectRemoval(pc, e, &al).slot == 1, "fo4 effects: a poison -> slot 1");
		al.poison = false;
		al.actorValue = 0x01000800;
		Check(!f4::PlanEffectRemoval(pc, e, &al).ok, "fo4 effects: the player's potion on a plugin's actor value is refused (its type is not known)");
		auto norec = ab;
		norec.mgefFlags = 0x800;
		u = f4::PlanEffectRemoval(pc, e, &norec);
		Check(u.ok && !u.modifier, "fo4 effects: without recover nothing is undone");
		Check(!f4::PlanEffectRemoval(pc, e, nullptr).ok, "fo4 effects: a running value modifier with no plugin data is refused");
		auto off = e;
		off.flags = 0xc280;  // inactive, never started
		u = f4::PlanEffectRemoval(pc, off, nullptr);
		Check(u.ok && !u.modifier, "fo4 effects: an effect not started is removed with nothing to undo");
		off.flags = 0x3c280;  // started and finished
		u = f4::PlanEffectRemoval(pc, off, nullptr);
		Check(u.ok && !u.modifier, "fo4 effects: a finished effect is removed with nothing to undo");
		auto taper = e;
		taper.flags = 0x14000;
		Check(!f4::PlanEffectRemoval(pc, taper, &ab).ok, "fo4 effects: a running value modifier whose strength tapers is refused");
		auto script = e;
		script.archetype = 1;
		Check(f4::PlanEffectRemoval(pc, script, nullptr).ok, "fo4 effects: a running script effect is removed (its Finish changes nothing saved)");
		auto dual = e;
		dual.archetype = 5;
		Check(!f4::PlanEffectRemoval(pc, dual, &ab).ok, "fo4 effects: a running dual value modifier is refused");
		dual.archetype = 36;
		Check(f4::PlanEffectRemoval(pc, dual, nullptr).ok, "fo4 effects: archetype 36 (a plain ActiveEffect) is removed");
	}

	const char* env = std::getenv("RESAVERPP_FO4_SAVES");
	if (!env || !*env) {
		std::printf("(fo4 effects: RESAVERPP_FO4_SAVES not set, real saves skipped)\n");
		return;
	}
	namespace fs = std::filesystem;
	std::vector<fs::path> files;
	std::string           all = env;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) e = all.size();
		const fs::path dir = all.substr(p, e - p);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
				if (f.path().extension() == ".fos") files.push_back(f.path());
			}
		}
		p = e + 1;
	}
	std::sort(files.begin(), files.end());
	const auto  tmp = fs::temp_directory_path() / "rs_unit_fo4fx.fos";
	std::size_t tested = 0, removedAll = 0, statsAll = 0, scriptsAll = 0, threadsAll = 0, keptAll = 0;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;
		}
		if (!s->isFO4()) continue;
		struct Want
		{
			std::size_t                                  effects = 0;
			std::map<std::uint32_t, std::array<float, 3>> mods;
			float                                        vit[4][3]{};
		};
		std::map<std::uint32_t, Want> want;
		std::size_t                   removed = 0, failed = 0, stats = 0, scripts = 0, kept = 0, scriptsWrong = 0;
		std::string                   firstErr;
		const auto                    instancesBefore = s->papyrus() ? s->papyrus()->instances.size() : 0;
		for (auto& c : s->changeForms) {
			if (c.typeCode() != 1) continue;
			const auto a = f4::DecodeActor(*s, c);
			if (!a.complete || a.effects.empty()) continue;
			// every started value modifier gets a made-up source: an ability on an actor value the actor has a modifier for
			// (the player's slot 0, an NPC's slot 1), so the modifier edit is exercised; the others none
			std::vector<f4::EffectSource> src(a.effects.size());
			Want                          w;
			for (const auto& m : a.modifiers) w.mods[m.formID] = { m.mod[0], m.mod[1], m.mod[2] };
			for (int k = 0; k < 4; ++k)
				for (int j = 0; j < 3; ++j) w.vit[k][j] = j < static_cast<int>(std::min<std::uint32_t>(a.vitals[k].count, 3)) ? a.vitals[k].mod[j] : 0.0f;
			std::vector<std::size_t> pick;
			for (std::size_t i = 0; i < a.effects.size(); ++i) {
				const auto& e = a.effects[i];
				if ((e.archetype == 0 || e.archetype == 34) && e.started() && !a.modifiers.empty()) {
					auto& x = src[i];
					x.known = true;
					x.mgefFlags = 0x2;
					x.actorValue = i % 5 == 4 ? f4::kVitalAV[i % 4] : a.modifiers[i % a.modifiers.size()].formID;
					std::memcpy(x.itemType, "SPEL", 4);
					x.spellType = 4;
					x.castType = 0;
				}
				const auto u = f4::PlanEffectRemoval(a, e, &src[i]);
				if (!u.ok) continue;
				pick.push_back(i);
				if (u.modifier && u.amount != 0) {
					float* v = nullptr;
					for (int k = 0; k < 4; ++k)
						if (f4::kVitalAV[k] == u.actorValue) v = &w.vit[k][u.slot];
					if (!v) v = &w.mods[u.actorValue][u.slot];
					*v -= u.amount;
					if (std::fabs(*v) <= 0.0001f) *v = 0;
					++stats;
				}
			}
			if (pick.empty()) continue;
			// the picked effects' ActiveMagicEffect instances (handle type 2, unique id, this actor)
			std::set<std::uint64_t> inst;
			for (const auto i : pick) {
				const auto v = f4::EffectScriptInstances(*s, s->formIDOf(c.refID), a.effects[i]);
				inst.insert(v.begin(), v.end());
			}
			std::vector<std::string> changes;
			const auto               e = f4::RemoveActiveEffects(*s, c, pick, src, &changes);
			if (!e.empty()) {
				++failed;
				if (firstErr.empty()) firstErr = e;
				continue;
			}
			if (!inst.empty()) {
				const bool keptThem = !changes.empty() && changes[0].find("left in the save") != std::string::npos;
				std::size_t still = 0;
				for (const auto& x : s->papyrus()->instances) still += inst.contains(x.id);
				if (keptThem) {
					kept += inst.size();
					scriptsWrong += still != inst.size();
				} else {
					scripts += inst.size();
					scriptsWrong += still != 0 || changes.empty() || changes[0].find("script instances removed") == std::string::npos;
					// the threads removed with them are counted in the line
					if (const auto at = changes[0].find("); "); at != std::string::npos) threadsAll += std::strtoul(changes[0].c_str() + at + 3, nullptr, 10);
				}
			}
			w.effects = a.effects.size() - pick.size();
			removed += pick.size();
			want[c.refID.data] = std::move(w);
		}
		{
			const auto    bytes = s->serialize();
			std::ofstream out(tmp, std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		auto        again = SaveFile::load(tmp);
		Check(scriptsWrong == 0 && again->papyrus() && again->papyrus()->instances.size() == instancesBefore - scripts &&
				  again->papyrus()->check().empty(),
			("fo4 effects: " + f.filename().string() + ": " + std::to_string(scripts) + " ActiveMagicEffect instances removed with their effects, " +
				std::to_string(kept) + " kept (named elsewhere); the Papyrus section reads back with the right instance count").c_str());
		scriptsAll += scripts;
		keptAll += kept;
		std::size_t ok = 0;
		for (const auto& c : again->changeForms) {
			const auto it = want.find(c.refID.data);
			if (c.typeCode() != 1 || it == want.end()) continue;
			const auto a = f4::DecodeActor(*again, c);
			bool       good = a.complete && a.effects.size() == it->second.effects;
			for (const auto& m : a.modifiers) {
				const auto& x = it->second.mods[m.formID];
				good = good && m.mod[0] == x[0] && m.mod[1] == x[1] && m.mod[2] == x[2];
			}
			for (int k = 0; k < 4; ++k)
				for (int j = 0; j < 3; ++j)
					good = good && (j < static_cast<int>(std::min<std::uint32_t>(a.vitals[k].count, 3)) ? a.vitals[k].mod[j] : 0.0f) == it->second.vit[k][j];
			ok += good;
		}
		Check(failed == 0 && ok == want.size(), ("fo4 effects: " + f.filename().string() + ": " + std::to_string(removed) + " effects removed from " +
		      std::to_string(want.size()) + " actors (" + std::to_string(stats) + " modifiers put back), " + std::to_string(ok) +
		      " read back exactly" + (firstErr.empty() ? "" : "; " + firstErr)).c_str());
		removedAll += removed;
		statsAll += stats;
		++tested;
	}
	std::error_code ec;
	fs::remove(tmp, ec);
	Check(tested == 0 || (removedAll > 0 && statsAll > 0), "fo4 effects: the real saves had effects to remove and modifiers to put back");
	Check(tested == 0 || scriptsAll > 0, "fo4 effects: the real saves had effect script instances to remove");
	std::printf("(fo4 effects: %zu real saves, %zu effects removed, %zu modifiers put back, %zu script instances removed with them (%zu threads), %zu kept)\n",
		tested, removedAll, statsAll, scriptsAll, threadsAll, keptAll);
}

// Fallout 4 player: the PlayerCharacter part (PlayerCharacter::LoadGame 0xd65bd0 after Actor::LoadGame) read through
// fo4cf::detail::PlayerPart on bytes laid out as the game reads them, at form versions that take different branches;
// then, with RESAVERPP_FO4_SAVES, every real save's player read completely, its Pip-Boy lists, a plugin's items removed
// from the player (Leftover items) with caps / perk rank / Strength / XP / level edits, written and read back.
void TestFo4Player()
{
	namespace fs = std::filesystem;
	using B = std::vector<std::uint8_t>;
	auto u8 = [](B& b, std::uint8_t v) { b.push_back(v); };
	auto u16 = [](B& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); };
	auto u32 = [](B& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
	auto ref = [](B& b, std::uint32_t formID) { const std::uint32_t d = formID ? ((1u << 22) | formID) : 0; b.push_back(d >> 16); b.push_back(d >> 8); b.push_back(d); };
	auto vs = [](B& b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v << 2)); };  // below 64
	auto raw = [](B& b, std::size_t n) { b.insert(b.end(), n, 0); };
	auto n32 = [&](B& b, int n) { for (int i = 0; i < n; ++i) u32(b, 0); };
	// a PlayerCharacter part as PlayerCharacter::LoadGame reads it at form version a_ver
	auto build = [&](std::uint32_t a_ver) {
		B b;
		u8(b, 1); u32(b, 0); u8(b, 0); u8(b, 0); n32(b, 2); u8(b, 0); u32(b, 0); u8(b, 0);
		n32(b, 1); raw(b, 12); n32(b, 2); u8(b, 0); u8(b, 0); raw(b, 8); u8(b, 0);
		n32(b, 3); u8(b, 0); n32(b, 8); u8(b, 0); u8(b, 0); n32(b, 1); n32(b, 7); raw(b, 24); n32(b, 1);
		vs(b, 1); raw(b, 12); u32(b, 0);                                    // list +0x750
		u8(b, 1); ref(b, 0x10); raw(b, 24); ref(b, 0x11); ref(b, 0x12); n32(b, 1);  // block +0x948 (set)
		for (int i = 0; i < 8; ++i) ref(b, 0);
		vs(b, 2); u32(b, 3); u32(b, 0); ref(b, 0x13); u32(b, 15);           // table +0xa64: index 15 reads nothing more
		vs(b, 1); ref(b, 0x1A); u16(b, 100); u8(b, 2);                      // quest log
		vs(b, 2); ref(b, 0x1A); u32(b, 1); u32(b, 0); u32(b, 2); ref(b, 0x1B); u32(b, 7); u32(b, 1); u32(b, 3);  // objectives
		n32(b, 3);
		// camera
		u8(b, 0); u8(b, 0); n32(b, 2); ref(b, 0); u32(b, 0);
		n32(b, 4); raw(b, 3);
		auto st = [&] { raw(b, 12); u32(b, 0); raw(b, 8); u8(b, 0); u32(b, 0); u16(b, 3); b.insert(b.end(), { 'a', 'b', 'c' }); u8(b, 0); };
		st(); raw(b, 12 + 8 + 8 + 2 + 2); st(); st(); u8(b, 0); ref(b, 0); u32(b, 0); st(); st(); st();
		if (a_ver >= 0x26) u32(b, 0);
		u32(b, 0); u32(b, 2); n32(b, 2); n32(b, 2); u8(b, 0);
		vs(b, 1); ref(b, 0x20); n32(b, 4);                                  // map +0x640
		ref(b, 0); u32(b, 0);
		vs(b, 0);                                                           // map +0x670
		u8(b, 0); u8(b, 3); u16(b, 9);                                      // not 0xFF: a u16 follows
		ref(b, 0); raw(b, 12); if (a_ver >= 0x23) raw(b, 12);
		u32(b, 0); for (int i = 0; i < 6; ++i) ref(b, 0);
		u32(b, 0); u32(b, 2); for (int i = 0; i < 2; ++i) { u16(b, 0x4AB); u32(b, 0xFFFFFFFF); u32(b, 0x3F800000); }
		ref(b, 0); u32(b, 0); ref(b, 0);
		vs(b, 1); ref(b, 0x21); for (int i = 0; i < 4; ++i) u16(b, 0);    // map +0x6d0
		raw(b, 6);
		vs(b, 2); n32(b, 2);                                                // 0xb79ad0
		vs(b, 1); ref(b, 0); u32(b, 0); ref(b, 0); u32(b, 0); u32(b, 0x6241C761u); ref(b, 0); n32(b, 2);  // knowledge
		u8(b, 0);
		vs(b, 1); ref(b, 0x22); n32(b, 2);                                  // map +0x828
		u32(b, 0); raw(b, 6); n32(b, 2); if (a_ver >= 0xa) u32(b, 0);
		u32(b, 1); ref(b, 0x23);                                            // list +0x9d8
		ref(b, 0); u8(b, 0); u32(b, 0); u8(b, 0); ref(b, 0); if (a_ver < 0x39) u8(b, 0);
		u8(b, 0); u32(b, 0); u8(b, 0); for (int i = 0; i < 5; ++i) ref(b, 0);
		if (a_ver < 0x2e || a_ver >= 0x31) { u32(b, 1); ref(b, 0x24); u32(b, 1); vs(b, 0); u32(b, 5); u16(b, 0); }  // one item
		u8(b, 0); u32(b, 0); u8(b, 0);
		if (a_ver < 7) { vs(b, 1); ref(b, 0x25); }
		vs(b, 0);                                                           // map +0x858
		u32(b, 1); ref(b, 0x26); if (a_ver < 7) raw(b, 8); else u32(b, 0);  // list +0xdb0
		ref(b, 0);
		if (a_ver >= 0x1f) { ref(b, 0); u8(b, 0); u8(b, 0); }
		if (a_ver >= 0x21) u8(b, 0);
		if (a_ver >= 0x2f && a_ver < 0x39) u32(b, 0);
		if (a_ver >= 0x32) { u32(b, 0); u8(b, 0); }
		if (a_ver >= 0x43) u8(b, 0);
		return b;
	};
	ess::SaveFile save;
	save.header.magic = "FO4_SAVEGAME";
	for (const std::uint32_t ver : { 69u, 68u, 65u, 0x30u, 0x22u, 6u }) {
		save.formVersion = ver;
		const auto       b = build(ver);
		ess::Reader      r(b.data(), b.size());
		ess::fo4cf::detail::In in{ r, ver };
		ess::fo4cf::Actor a;
		std::string err;
		try {
			ess::fo4cf::detail::PlayerPart(in, save, a);
		} catch (const std::exception& e) {
			err = e.what();
		}
		const bool inv = ver < 0x2e || ver >= 0x31;
		Check(err.empty() && a.playerPart && r.pos() == b.size() && a.questLog.size() == 1 && a.questLog[0].stage == 100 &&
		          a.questLog[0].log == 2 && a.objectives.size() == 2 && a.objectives[1].index == 7 && a.objectives[1].state == 3 &&
		          a.extraInventory.size() == (inv ? 1u : 0u) && a.questLogRange.end == a.objectivesRange.begin,
			("fo4 player: the PlayerCharacter part reads exactly at form version " + std::to_string(ver)).c_str());
		auto shorter = b;
		shorter.pop_back();
		ess::Reader r2(shorter.data(), shorter.size());
		ess::fo4cf::detail::In in2{ r2, ver };
		bool threw = false;
		try {
			ess::fo4cf::detail::PlayerPart(in2, save, a);
		} catch (const std::exception&) {
			threw = true;
		}
		Check(threw, ("fo4 player: a cut PlayerCharacter part does not read at form version " + std::to_string(ver)).c_str());
	}

	const char* env = std::getenv("RESAVERPP_FO4_SAVES");
	if (!env || !*env) {
		std::printf("(fo4 player: RESAVERPP_FO4_SAVES not set, real saves skipped)\n");
		return;
	}
	std::vector<fs::path> files;
	std::string           all = env;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) e = all.size();
		const fs::path dir = all.substr(p, e - p);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
				if (f.path().extension() == ".fos") files.push_back(f.path());
			}
		}
		p = e + 1;
	}
	std::sort(files.begin(), files.end());
	const auto  tmp = fs::temp_directory_path() / "rs_unit_player.fos";
	std::size_t tested = 0;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;
		}
		if (!s->isFO4()) continue;
		const auto name = f.filename().string();
		auto*      p = const_cast<ess::ChangeForm*>(ess::fo4cf::FindPlayer(*s));
		if (!p) continue;
		const auto a = ess::fo4cf::DecodeActor(*s, *p);
		const auto body = ess::cf::Body(*p);
		Check(a.complete && a.player && a.playerRange.begin == a.tailEnd && a.playerRange.end == body.size() && !a.playerParts.empty(),
			("fo4 player: the player's body reads completely in " + name + (a.error.empty() ? "" : ": " + a.error)).c_str());
		if (!a.complete) continue;
		bool found = false;
		const auto log = ess::quests::ReadQuestLog(*s, &found);
		const auto journal = ess::quests::ReadJournal(*s);
		Check(found && log.size() == a.questLog.size() && journal.size() == a.objectives.size(),
			("fo4 player: the Pip-Boy quest log and objectives are the decoded lists in " + name).c_str());
		// Leftover items on the player: the plugin with most items (Fallout4.esm when it has nothing else)
		const auto r = ess::fo4cf::DecodeRefr(*s, *p);
		std::map<std::uint32_t, std::size_t> per;
		for (const auto& it : r.inventory) ++per[it.formID >> 24];
		if (per.empty()) continue;
		std::uint32_t plugin = 0;
		std::size_t   most = 0;
		for (const auto& [k, n] : per) {
			if (k != 0 && n > most) { plugin = k; most = n; }
		}
		std::set<std::uint32_t> items;
		for (const auto& it : r.inventory) {
			if ((it.formID >> 24) == plugin) items.insert(it.formID);
		}
		std::size_t n = 0;
		const auto  e = ess::cf::RemoveInventoryItems(*s, *p, items, &n);
		Check(e.empty() && n == items.size(), ("fo4 player: a plugin's items removed from the player in " + name + (e.empty() ? "" : ": " + e)).c_str());
		// the other edits on the edited player
		const auto caps = ess::fo4cf::PlayerCaps(*s);
		const bool capsSet = caps >= 0 && ess::fo4cf::SetPlayerCaps(*s, static_cast<std::int32_t>(caps + 25)).empty();
		const auto a1 = ess::fo4cf::DecodeActor(*s, *p);
		std::uint32_t perk = 0;
		if (!a1.perks.empty() && ess::fo4cf::SetPerkRank(*s, *p, a1.perks.front().formID, 3).empty()) perk = a1.perks.front().formID;
		const bool strSet = ess::fo4cf::SetPlayerSpecial(*s, 0, 7).empty();
		const bool xpSet = ess::fo4cf::SetPlayerXP(*s, 4321).empty();
		const bool lvlSet = ess::fo4cf::PlayerLevel(*s) >= 0 && ess::fo4cf::SetPlayerLevel(*s, 42).empty();
		{
			const auto bytes = s->serialize();
			std::ofstream out(tmp, std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		auto       again = SaveFile::load(tmp);
		const auto* p2 = ess::fo4cf::FindPlayer(*again);
		const auto a2 = ess::fo4cf::DecodeActor(*again, *p2);
		const auto r2 = ess::fo4cf::DecodeRefr(*again, *p2);
		const auto b2 = ess::cf::Body(*p2);
		bool ok = a2.complete && r2.inventory.size() == r.inventory.size() - n;
		for (const auto& it : r2.inventory) ok = ok && !items.contains(it.formID);
		ok = ok && std::equal(body.begin() + static_cast<std::ptrdiff_t>(a.playerRange.begin), body.end(),
		                      b2.begin() + static_cast<std::ptrdiff_t>(a2.playerRange.begin), b2.end());
		Check(ok, ("fo4 player: written and read back completely without the items, PlayerCharacter part unchanged, in " + name).c_str());
		bool edits = !capsSet || ess::fo4cf::PlayerCaps(*again) == caps + 25;
		if (perk) {
			bool hit = false;
			for (const auto& x : a2.perks) hit = hit || (x.formID == perk && x.rank == 3);
			edits = edits && hit;
		}
		if (strSet) {
			bool hit = false;
			for (const auto& v : a2.values) hit = hit || (v.formID == 0x2C2 && v.base == 7.0f);
			edits = edits && hit;
		}
		edits = edits && (!xpSet || ess::fo4cf::PlayerXP(*again) == 4321.0);
		edits = edits && (!lvlSet || (ess::fo4cf::PlayerLevel(*again) == 42 && again->header.playerLevel == 42));
		Check(edits && strSet && xpSet, ("fo4 player: caps, perk rank, Strength, XP and level edits read back in " + name).c_str());
		++tested;
	}
	std::error_code ec;
	fs::remove(tmp, ec);
	std::printf("(fo4 player: %zu real saves)\n", tested);
}

// Fallout 4 settlements: the workbench's actor values (extra 0xC2), the build meter, the used budget reset
void TestFo4Workshops()
{
	namespace fs = std::filesystem;
	namespace ws = ess::fo4ws;
	using B = std::vector<std::uint8_t>;
	auto u32 = [](B& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
	auto f32 = [&](B& b, float f) { std::uint32_t v; std::memcpy(&v, &f, 4); u32(b, v); };
	auto ref = [](B& b, std::uint32_t formID) { const std::uint32_t d = formID ? ((1u << 22) | formID) : 0; b.push_back(d >> 16); b.push_back(d >> 8); b.push_back(d); };
	{
		// a REFR with extra data (change flag 31): vsval 2 extras: 0xC2 {bases: MaxDraws 1000, CurrentDraws 900, Happiness 50;
		// modifiers: CurrentTriangles (0, 0, 10)}, then 0x1f (one byte)
		B r;
		r.push_back(2 << 2);
		r.push_back(0xC2);
		r.push_back(3 << 2);
		ref(r, ws::kAvMaxDraws); f32(r, 1000.f);
		ref(r, ws::kAvCurrentDraws); f32(r, 900.f);
		ref(r, ws::kAvHappiness); f32(r, 50.f);
		r.push_back(1 << 2);
		ref(r, ws::kAvCurrentTriangles); f32(r, 0.f); f32(r, 0.f); f32(r, 10.f);
		r.push_back(0x1f);
		r.push_back(7);
		ess::SaveFile save;
		save.header.magic = "FO4_SAVEGAME";
		save.formVersion = 68;
		ess::ChangeForm c;
		c.refID.data = (1u << 22) | 0x250FE;
		c.typeField = 0;
		c.changeFlags = 1u << 31;
		c.data = r;
		c.length1 = static_cast<std::uint32_t>(r.size());
		save.changeForms.push_back(c);
		const auto d = ess::fo4cf::DecodeRefr(save, save.changeForms[0]);
		const auto st = ws::ReadStorage(r, d, save);
		Check(d.complete && st && st->present && st->bases.size() == 3 && st->mods.size() == 1 && st->value(ws::kAvCurrentDraws) == 900.f &&
				  st->value(ws::kAvCurrentTriangles) == 10.f && st->value(ws::kAvMaxTriangles) == 0.f,
			"fo4 workshop: extra 0xC2 read (vsval n x {ref, f32}, vsval m x {ref, 3 x f32}); value = base + modifiers");
		bool       changed = false;
		const auto why = ws::SetUsed(save, 0x250FE, 0.f, 0.f, &changed);
		const auto b2 = ess::cf::Body(save.changeForms[0]);
		const auto d2 = ess::fo4cf::DecodeRefr(save, save.changeForms[0]);
		const auto st2 = ws::ReadStorage(b2, d2, save);
		Check(why.empty() && changed && d2.complete && st2 && st2->value(ws::kAvCurrentDraws) == 0.f && st2->value(ws::kAvCurrentTriangles) == 0.f &&
				  st2->base(ws::kAvCurrentTriangles) == -10.f && st2->value(ws::kAvMaxDraws) == 1000.f && st2->value(ws::kAvHappiness) == 50.f &&
				  st2->bases.size() == 4 && b2.size() == r.size() + 7 && b2.back() == 7 && b2[b2.size() - 2] == 0x1f,
			"fo4 workshop: reset sets the current bases so base + modifiers is 0 (a triangles entry added), the rest kept");
		changed = true;
		Check(ws::SetUsed(save, 0x250FE, 0.f, 0.f, &changed).empty() && !changed, "fo4 workshop: resetting again changes nothing");
		Check(!ws::SetUsed(save, 0x250FE, -1.f, std::nullopt).empty() && !ws::SetUsed(save, 0x12345, 0.f, 0.f).empty(),
			"fo4 workshop: negative numbers and unknown workshops refused");

		ws::Settlement s;
		s.currentDraws = 861;
		s.maxDraws = 1000;
		s.currentTriangles = 497844;
		s.maxTriangles = 500000;
		Check(std::fabs(s.meter() - 497844.0 / 500000.0) < 1e-6 && !s.overBudget(), "fo4 workshop: the meter is the larger share");
		s.maxDraws = 0;
		s.maxTriangles = 0;
		s.currentDraws = 10000;
		Check(s.limitDraws() == 10000 && s.limitTriangles() == 10000000 && s.overBudget(), "fo4 workshop: max 0 = the game's default; full at 1.0");
	}

	const char* env = std::getenv("RESAVERPP_FO4_SAVES");
	if (!env || !*env) {
		std::printf("(fo4 workshops: RESAVERPP_FO4_SAVES not set, real saves skipped)\n");
		return;
	}
	std::vector<fs::path> files;
	std::string           all = env;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) e = all.size();
		const fs::path dir = all.substr(p, e - p);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
				if (f.path().extension() == ".fos") files.push_back(f.path());
			}
		}
		p = e + 1;
	}
	std::sort(files.begin(), files.end());
	const auto tmp = fs::temp_directory_path() / "rs_unit_ws.fos";
	auto       CheckS = [](bool a_ok, const std::string& a_what) { Check(a_ok, a_what.c_str()); };
	std::size_t tested = 0;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;
		}
		const auto name = f.filename().string();
		const auto list = ws::List(*s);
		std::size_t sanctuary = 0, edited = 0;
		for (const auto& w : list) {
			sanctuary += w.formID == 0x250FE && w.location == 0x1F228;  // SanctuaryHillsLocation
		}
		CheckS(list.size() >= 30 && sanctuary == 1, "fo4 workshop: " + name + ": " + std::to_string(list.size()) + " workshops, Sanctuary's at its location");
		if (name.rfind("Save14_", 0) == 0) {
			std::size_t over = 0;
			for (const auto& w : ws::Check(*s)) {
				over += w.kind == ws::Finding::Kind::kOverBudget && w.form == 0x66EB6;
			}
			CheckS(over == 1, "fo4 workshop: Save14: The Castle (66EB6) over budget (1005094 / 1000000 triangles)");
		}
		const auto before = s->changeForms;
		std::set<std::uint32_t> touched;
		for (const auto& w : list) {
			if (!w.hasStorage || !w.readable) continue;
			bool changed = false;
			const auto why = ws::SetUsed(*s, w.formID, 0.f, 0.f, &changed);
			CheckS(why.empty(), "fo4 workshop: " + name + ": reset " + why);
			if (changed) {
				++edited;
				touched.insert(w.formID);
			}
		}
		const auto bytes = s->serialize();
		{
			std::ofstream out(tmp, std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		auto back = SaveFile::load(tmp);
		bool same = back->changeForms.size() == before.size() && back->serialize() == bytes;
		std::size_t others = 0;
		for (std::size_t i = 0; same && i < before.size(); ++i) {
			const auto id = back->formIDOf(back->changeForms[i].refID);
			if (!touched.contains(id) || back->changeForms[i].typeCode() != 0) {
				others += ess::cf::Body(back->changeForms[i]) != ess::cf::Body(before[i]);
			}
		}
		bool zero = true;
		const auto list2 = ws::List(*back);
		for (std::size_t i = 0; i < list2.size() && i < list.size(); ++i) {
			const auto& a = list[i];
			const auto& b = list2[i];
			if (a.hasStorage && a.readable) {
				zero = zero && b.usedDraws() == 0 && b.usedTriangles() == 0 && b.maxDraws == a.maxDraws && b.maxTriangles == a.maxTriangles &&
				       b.population == a.population && b.happiness == a.happiness && b.readable;
			}
		}
		CheckS(same && others == 0 && zero && list2.size() == list.size(), "fo4 workshop: " + name + ": " + std::to_string(edited) +
			" workshops reset, written and read back; every other change form unchanged");
		++tested;
	}
	fs::remove(tmp);
	CheckS(tested > 0, "fo4 workshop: real saves tested (" + std::to_string(tested) + ")");
}

void TestFo4HavokFormLists()
{
	namespace fs = std::filesystem;
	using B = std::vector<std::uint8_t>;
	auto u32 = [](B& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
	auto f32 = [&](B& b, float f) { std::uint32_t v; std::memcpy(&v, &f, 4); u32(b, v); };
	auto ref = [](B& b, std::uint32_t formID) { const std::uint32_t d = formID ? ((1u << 22) | formID) : 0; b.push_back(d >> 16); b.push_back(d >> 8); b.push_back(d); };
	auto mk = [](std::uint8_t type, std::uint32_t flags, std::uint32_t id, const B& body) {
		ess::ChangeForm c;
		c.refID.data = (1u << 22) | id;
		c.typeField = type;
		c.changeFlags = flags;
		c.data = body;
		c.length1 = static_cast<std::uint32_t>(body.size());
		return c;
	};
	ess::SaveFile save;
	save.header.magic = "FO4_SAVEGAME";
	save.formVersion = 68;
	{
		B r;  // havok moved (2) + scale (4): initial data, u8 version, vsval 5, 5 bytes, f32 scale
		ref(r, 0x3C);
		for (int i = 0; i < 6; ++i) f32(r, 1.f + i);
		r.push_back(68);
		r.push_back(5 << 2);
		r.insert(r.end(), { 1, 2, 3, 4, 5 });
		f32(r, 2.5f);
		save.changeForms.push_back(mk(0, (1u << 2) | (1u << 4), 0xA01, r));
		B e = r;  // the same with an empty blob (as the game writes it)
		e.erase(e.begin() + 27 + 1, e.begin() + 27 + 2 + 5);
		e.insert(e.begin() + 27 + 1, 0);
		save.changeForms.push_back(mk(0, (1u << 2) | (1u << 4), 0xA02, e));
		B bad = r;  // left over bytes: not read completely, left alone
		bad.push_back(0);
		save.changeForms.push_back(mk(0, (1u << 2) | (1u << 4), 0xA03, bad));
		B l;  // form list: 0, 0x100, 0, 0x200
		u32(l, 4);
		ref(l, 0); ref(l, 0x100); ref(l, 0); ref(l, 0x200);
		save.changeForms.push_back(mk(ess::fo4cf::kFlstType, 1u << 31, 0xB01, l));
		save.changeForms.push_back(mk(43, 1u << 31, 0xB02, l));  // 43 is LVLN in Fallout 4: not a form list
	}
	const auto before3 = save.changeForms[2].data;
	const auto d0 = ess::fo4cf::DecodeRefr(save, save.changeForms[0]);
	Check(d0.complete && d0.hasHavok && d0.havokVersion == 68 && d0.havokSize == 5 && d0.havokRange.size() == 7, "fo4 havok: the blob is read (u8, vsval, bytes)");
	const auto dry = ess::cf::ResetHavok(save, {}, true);
	Check(dry.reset == 1 && dry.failed == 1, "fo4 havok: dry run counts the one blob to clear and the reference not read completely");
	const auto h = ess::cf::ResetHavok(save);
	const auto d1 = ess::fo4cf::DecodeRefr(save, save.changeForms[0]);
	Check(h.reset == 1 && h.failed == 1 && d1.complete && d1.havokSize == 0 && d1.havokVersion == 68 && d1.hasScale && d1.scale == 2.5f && d1.pos[2] == 3.f,
		"fo4 havok: the blob's size becomes 0, the version byte, position and scale stay");
	Check(ess::cf::Body(save.changeForms[0]) == ess::cf::Body(save.changeForms[1]), "fo4 havok: the reset body is what the game writes for no havok data");
	Check(save.changeForms[2].data == before3 && save.changeForms[0].changeFlags == ((1u << 2) | (1u << 4)), "fo4 havok: change flags kept, unread reference untouched");
	Check(ess::cf::ResetHavok(save, {}, true).reset == 0, "fo4 havok: nothing left to reset");
	const auto pd = ess::cf::PurifyFormLists(save, true);
	Check(pd.entries == 2 && pd.forms == 1 && ess::cf::NullEntries(save, save.changeForms[3]) == 2 && ess::cf::NullEntries(save, save.changeForms[4]) == 0,
		"fo4 purify: two null entries in the one form list (type 42; 43 is not a list in Fallout 4)");
	const auto lvln = save.changeForms[4].data;
	const auto pr = ess::cf::PurifyFormLists(save);
	const auto fl = ess::fo4cf::DecodeFormList(save, save.changeForms[3]);
	Check(pr.entries == 2 && fl.complete && fl.raw.size() == 2 && save.formIDOf(RefID{ fl.raw[0] }) == 0x100 && save.formIDOf(RefID{ fl.raw[1] }) == 0x200 &&
			  save.changeForms[4].data == lvln,
		"fo4 purify: nulls removed, order kept, type 43 untouched");

	const char* env = std::getenv("RESAVERPP_FO4_SAVES");
	if (!env || !*env) {
		std::printf("(fo4 havok / purify: RESAVERPP_FO4_SAVES not set, real saves skipped)\n");
		return;
	}
	std::vector<fs::path> files;
	std::string           all = env;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) e = all.size();
		const fs::path dir = all.substr(p, e - p);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
				if (f.path().extension() == ".fos") files.push_back(f.path());
			}
		}
		p = e + 1;
	}
	std::sort(files.begin(), files.end());
	const auto tmp = fs::temp_directory_path() / "rs_unit_hav.fos";
	auto       writeBack = [&](ess::SaveFile& s) {
        const auto bytes = s.serialize();
        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        return SaveFile::load(tmp);
	};
	std::size_t tested = 0;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;
		}
		const auto name = f.filename().string();
		std::size_t blobs = 0, complete = 0;
		for (const auto& c : s->changeForms) {
			if (c.typeCode() != 0 || !((c.changeFlags >> 2) & 1)) continue;
			const auto d = ess::fo4cf::DecodeRefr(*s, c);
			complete += d.complete;
			blobs += d.complete && d.havokSize > 0;
		}
		const auto orig = s->changeForms;
		const auto h2 = ess::cf::ResetHavok(*s);
		auto       back = writeBack(*s);
		std::size_t empty = 0, stillComplete = 0, other = 0;
		for (std::size_t i = 0; i < back->changeForms.size(); ++i) {
			const auto& c = back->changeForms[i];
			if (c.typeCode() == 0 && ((c.changeFlags >> 2) & 1)) {
				const auto d = ess::fo4cf::DecodeRefr(*back, c);
				stillComplete += d.complete;
				empty += d.complete && d.havokSize == 0;
			} else {
				other += ess::cf::Body(c) == ess::cf::Body(orig[i]) ? 0 : 1;
			}
		}
		const bool ok = h2.reset == blobs && stillComplete == complete && empty == complete && other == 0 && ess::cf::ResetHavok(*back, {}, true).reset == 0;
		Check(ok, ("fo4 havok: " + name + ": reset, written, read back, every reference read completely has an empty blob, nothing else changed").c_str());
		std::printf("(fo4 havok: %s: %zu blobs cleared, %zu references with havok read completely)\n", name.c_str(), h2.reset, complete);
		if (!ok) {
			std::printf("  reset %zu of %zu blobs, complete %zu -> %zu, empty %zu, other changed %zu\n", h2.reset, blobs, complete, stillComplete, empty, other);
		}
		// form lists: two nulls put in each, purified, written, read back: as the original
		auto        s2 = SaveFile::load(f);
		std::size_t lists = 0;
		for (auto& c : s2->changeForms) {
			if (c.typeCode() != ess::fo4cf::kFlstType || !((c.changeFlags >> 31) & 1)) continue;
			const auto d = ess::fo4cf::DecodeFormList(*s2, c);
			if (!d.complete) continue;
			auto          body = ess::cf::Body(c);
			std::uint32_t k = static_cast<std::uint32_t>(d.raw.size() + 2);
			std::memcpy(&body[d.list.begin], &k, 4);
			body.insert(body.begin() + static_cast<std::ptrdiff_t>(d.list.end), { 0, 0, 0 });
			body.insert(body.begin() + static_cast<std::ptrdiff_t>(d.list.begin + 4), { 0, 0, 0 });
			ess::cf::SetBody(c, body);
			++lists;
		}
		auto       withNulls = writeBack(*s2);
		const auto p2 = ess::cf::PurifyFormLists(*withNulls);
		auto       pure = writeBack(*withNulls);
		auto       ref0 = SaveFile::load(f);
		bool       same = pure->changeForms.size() == ref0->changeForms.size();
		for (std::size_t i = 0; same && i < pure->changeForms.size(); ++i) {
			same = ess::cf::Body(pure->changeForms[i]) == ess::cf::Body(ref0->changeForms[i]);
		}
		Check(p2.entries == 2 * lists && p2.forms == lists && p2.failed == 0 && same,
			("fo4 purify: " + name + ": nulls put in every form list, purified, written, read back: as the original").c_str());
		++tested;
	}
	fs::remove(tmp);
	std::printf("(fo4 havok / purify: %zu real saves)\n", tested);
}

// Skyrim followers / beast form (core/FollowersSky): the global variables block in memory, then real saves from
// RESAVERPP_SKY_SAVES="dir;dir" (Tony's Save88, SkyrimIstari's Save89: PlayerFollowerCount 1 with an empty Follower alias)
void TestSkyFollowers()
{
	namespace fs = std::filesystem;
	{
		ess::SaveFile s;
		ess::GlobalData g;
		g.type = 3;
		g.raw = { 2 << 2 };  // vsval 2
		auto put = [&](std::uint32_t id, float v) {
			const std::uint32_t r = (1u << 22) | id;  // a Skyrim.esm form id (RefID type 1)
			g.raw.push_back(static_cast<std::uint8_t>(r >> 16));
			g.raw.push_back(static_cast<std::uint8_t>(r >> 8));
			g.raw.push_back(static_cast<std::uint8_t>(r));
			std::uint8_t b[4];
			std::memcpy(b, &v, 4);
			g.raw.insert(g.raw.end(), b, b + 4);
		};
		put(ess::skyf::kPlayerFollowerCount, 1.0f);
		put(0x39, 47.5f);
		s.table1.push_back(std::move(g));
		std::string err;
		auto        all = ess::skyf::ReadGlobals(s, &err);
		Check(err.empty() && all.size() == 2 && all[0].formID == 0x0BCC98 && all[0].value == 1.0f && all[1].formID == 0x39 && all[1].value == 47.5f,
			"skyf: the global variables block (vsval count, RefID + f32)");
		Check(ess::skyf::SetGlobal(s, 0x0BCC98, 0.0f).empty() && ess::skyf::ReadGlobals(s)[0].value == 0.0f && ess::skyf::ReadGlobals(s)[1].value == 47.5f,
			"skyf: a global set in place, the others kept");
		Check(!ess::skyf::SetGlobal(s, 0x123456, 1.0f).empty(), "skyf: a global the save does not have is refused");
		s.table1[0].raw.push_back(0);
		Check(ess::skyf::ReadGlobals(s, &err).empty() && !err.empty(), "skyf: a block with bytes left over is not trusted");
		// a FollowerActor's rank: the reference's change first, then the base NPC's, else the plugin's (-2)
		ess::skyf::FollowerActor a;
		a.refFactions = { { ess::skyf::kCurrentFollowerFaction, -1 } };
		a.baseFactions = { { ess::skyf::kCurrentFollowerFaction, 0 }, { ess::skyf::kDismissedFollowerFaction, 0 } };
		Check(a.rank(ess::skyf::kCurrentFollowerFaction) == -1 && a.inFaction(ess::skyf::kDismissedFollowerFaction) &&
				  a.rank(ess::skyf::kPlayerFollowerFaction) == -2,
			"skyf: faction rank from the reference, then the base NPC");
	}

	// the beast form verdicts (PlayerWerewolfChangeScript / DLC1PlayerVampireChangeScript), one per case
	{
		using B = ess::skyf::BeastState;
		auto make = [](ess::skyf::Beast k) {
			B b;
			b.kind = k;
			b.questEdid = k == ess::skyf::Beast::kWerewolf ? "PlayerWerewolfQuest" : "DLC1PlayerVampireQuest";
			b.questInSave = true;
			b.beastRace = 0x000CDD84;
			b.originalRace = 0x00013746;
			return b;
		};
		auto wolf = make(ess::skyf::Beast::kWerewolf);
		wolf.questRunning = true;
		wolf.stagesDone = { 0, 1, 10 };
		wolf.raceIsBeast = true;
		wolf.hasShiftBackTime = true;
		wolf.shiftBackTime = 10.0f;
		wolf.gameDaysPassed = 10.01f;
		ess::skyf::Judge(wolf);
		Check(wolf.fix == B::Fix::kNone, "skyf beast: a werewolf whose time is not up is fine");
		wolf.gameDaysPassed = 10.5f;
		ess::skyf::Judge(wolf);
		Check(wolf.fix == B::Fix::kSetStage, "skyf beast: a werewolf past its time: stage 100");
		wolf.untimed = true;
		ess::skyf::Judge(wolf);
		Check(wolf.fix == B::Fix::kNone, "skyf beast: an untimed werewolf (C03Rampage) is left alone");
		wolf.untimed = false;
		wolf.stagesDone.push_back(100);
		ess::skyf::Judge(wolf);
		Check(wolf.fix == B::Fix::kGameRevert, "skyf beast: still a werewolf after stage 100: the script's ShiftBack again");
		wolf.questRunning = false;
		ess::skyf::Judge(wolf);
		Check(wolf.fix == B::Fix::kGameRevert && wolf.problem.find("not running") != std::string::npos, "skyf beast: a werewolf with the quest stopped");
		wolf.originalRace = 0;
		ess::skyf::Judge(wolf);
		Check(wolf.fix == B::Fix::kCannot, "skyf beast: no race to go back to: the console");
		wolf.originalRace = 0x00013746;
		wolf.raceIsBeast = false;
		ess::skyf::Judge(wolf);
		Check(wolf.fix == B::Fix::kNone, "skyf beast: human, quest stopped, nothing left: fine");
		wolf.spells = { 0x0010C6E8 };
		ess::skyf::Judge(wolf);
		Check(wolf.fix == B::Fix::kGameRevert, "skyf beast: human but the claws are still there");
		wolf.spells.clear();
		wolf.questRunning = true;
		ess::skyf::Judge(wolf);
		Check(wolf.fix == B::Fix::kGameRevert && wolf.problem.find("Shutdown") != std::string::npos, "skyf beast: human but the quest still runs after stage 100");
		auto lord = make(ess::skyf::Beast::kVampireLord);
		lord.questRunning = true;
		lord.stagesDone = { 0, 1, 10 };
		lord.raceIsBeast = true;
		lord.hasRevertPower = true;
		ess::skyf::Judge(lord);
		Check(lord.fix == B::Fix::kNone, "skyf beast: a Vampire Lord with Revert Form is fine (no time limit)");
		lord.hasRevertPower = false;
		ess::skyf::Judge(lord);
		Check(lord.fix == B::Fix::kSetStage, "skyf beast: a Vampire Lord without Revert Form: stage 100");
	}

	const char* env = std::getenv("RESAVERPP_SKY_SAVES");
	if (!env || !*env) {
		std::printf("(skyf: RESAVERPP_SKY_SAVES not set, real saves skipped)\n");
		return;
	}
	std::vector<fs::path> files;
	std::string           all = env;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) {
			e = all.size();
		}
		const fs::path dir = all.substr(p, e - p);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::directory_iterator(dir, ec)) {
				if (f.path().extension() == ".ess") {
					files.push_back(f.path());
				}
			}
		}
		p = e + 1;
	}
	std::sort(files.begin(), files.end());
	for (const auto& f : files) {
		const auto tag = "skyf " + f.filename().string().substr(0, 12) + ": ";
		auto       save = ess::SaveFile::load(f);
		const auto st = ess::skyf::ReadFollowers(*save);
		Check(st.error.empty() && st.questInSave && st.scriptFound && (st.hasCount || st.followerCount == 0.0f), (tag + "the follower quest and its script are read (a count never changed in game is not saved: 0)").c_str());
		std::size_t teammates = 0;
		for (const auto& a : st.actors) {
			teammates += a.teammate ? 1 : 0;
			Check(a.decoded, (tag + "every follower-sign actor's state is read").c_str());
		}
		const bool stuckCount = std::any_of(st.findings.begin(), st.findings.end(), [](const ess::skyf::Finding& x) { return x.issue == ess::skyf::Issue::kCountWithoutFollower; });
		Check(stuckCount == (st.followerCount >= 1 && !st.inAlias(ess::skyf::kFollowerAlias)), (tag + "\"you already have a follower\" exactly when the count is 1 and the alias empty").c_str());
		for (const auto& b : ess::skyf::ReadBeastForms(*save)) {
			Check(!b.raceIsBeast && b.fix == ess::skyf::BeastState::Fix::kNone, (tag + b.questEdid + ": a human player with nothing left over").c_str());
		}
		// the fixes: the count, then one teammate flag; the save writes and reads back with only those bytes changed
		if (stuckCount) {
			std::string done;
			Check(ess::skyf::FixFollowerCount(*save, false, &done).empty() && done == "PlayerFollowerCount 1 -> 0", (tag + "the count fixed").c_str());
			Check(!ess::skyf::FixFollowerCount(*save, false).empty(), (tag + "a count that already fits is left alone").c_str());
		}
		const ess::skyf::FollowerActor* mate = nullptr;
		for (const auto& a : st.actors) {
			if (a.teammate && !mate) {
				mate = &a;
			}
		}
		if (mate) {
			const auto id = mate->formID;
			Check(ess::skyf::ClearTeammate(*save, id).empty(), (tag + "a teammate flag switched off").c_str());
			Check(!ess::skyf::ClearTeammate(*save, id).empty(), (tag + "an actor that is no teammate is refused").c_str());
			Check(!ess::skyf::ClearTeammate(*save, 0x14).empty(), (tag + "never the player").c_str());
		}
		const auto bytes = save->serialize();
		const auto tmp = fs::temp_directory_path() / "rs_skyf_test.ess";
		{
			std::ofstream o(tmp, std::ios::binary);
			o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		auto       back = ess::SaveFile::load(tmp);
		const auto st2 = ess::skyf::ReadFollowers(*back);
		std::size_t teammates2 = 0;
		for (const auto& a : st2.actors) {
			teammates2 += a.teammate ? 1 : 0;
		}
		Check(teammates2 + (mate ? 1 : 0) == teammates && (!stuckCount || st2.followerCount == 0.0f) &&
				  std::none_of(st2.findings.begin(), st2.findings.end(), [](const ess::skyf::Finding& x) { return x.issue == ess::skyf::Issue::kCountWithoutFollower; }),
			(tag + "written and read back with the fixes").c_str());
		std::error_code ec;
		fs::remove(tmp, ec);
		for (const auto& x : ess::skyf::FindStuckStages(*save)) {
			Check(x.quest != 0 && !x.topFrame.empty(), (tag + "a running stage script names its quest and where it waits").c_str());
		}
		// the shut-down stage check: a running quest (not starting / stopping) with a done stage passed as shut-down-only is found,
		// with the same stage passed as not done nothing is; a quest saved with flag 0x80 is never reported
		const ess::quests::QuestState* pick = nullptr;
		std::int16_t                   doneStage = -1, notDone = -1;
		for (const auto& q : ess::quests::ReadStates(*save)) {
			if (!q.running() || (q.flags & 0x80)) {
				continue;
			}
			std::int16_t d = -1, n = -1;
			for (const auto& [st, done] : q.stages) {
				(done ? d : n) = st;
			}
			if (d >= 0 && n >= 0) {
				static ess::quests::QuestState keep;
				keep = q;
				pick = &keep;
				doneStage = d;
				notDone = n;
				break;
			}
		}
		if (pick) {
			const auto id = pick->formID;
			auto shut = [&](std::int16_t a_stage) {
				return [=](std::uint32_t q) { return q == id ? std::vector<int>{ a_stage } : std::vector<int>{}; };
			};
			const auto found = ess::skyf::FindStuckStages(*save, shut(doneStage));
			Check(std::count_if(found.begin(), found.end(), [&](const ess::skyf::StuckStage& x) { return x.shutdown && x.quest == id && x.stage == doneStage; }) == 1,
				(tag + "a running quest with its shut-down stage done is reported").c_str());
			const auto none = ess::skyf::FindStuckStages(*save, shut(notDone));
			Check(std::none_of(none.begin(), none.end(), [](const ess::skyf::StuckStage& x) { return x.shutdown; }),
				(tag + "a shut-down stage not done is not reported").c_str());
		}
	}
	// only stages flagged shut-down and not start-up (INDX flags 4 without 2)
	const auto only = ess::skyf::ShutdownOnlyStages({ { 0, 6 }, { 10, 4 }, { 20, 2 }, { 30, 0 }, { 40, 0x0C } });
	Check(only == std::vector<int>{ 10, 40 }, "skyf: shut-down stages that only a stop runs (start-up ones left out)");
}

// Fallout 4 dropped / created items in the world (fo4world) and reset an area (fo4cell): synthetic change forms, then with
// RESAVERPP_FO4_SAVES: Tony's Save8 minus its dropped pistols has Save7's change forms; every real save's removable items
// removed, written, read back, nothing else changed; cells reset as ResetInterior does, written, read back.
void TestFo4WorldItems()
{
	namespace fs = std::filesystem;
	using B = std::vector<std::uint8_t>;
	auto u32 = [](B& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
	auto f32 = [&](B& b, float f) { std::uint32_t v; std::memcpy(&v, &f, 4); u32(b, v); };
	auto ref = [](B& b, std::uint32_t d) { b.push_back(static_cast<std::uint8_t>(d >> 16)); b.push_back(static_cast<std::uint8_t>(d >> 8)); b.push_back(static_cast<std::uint8_t>(d)); };
	auto mk = [](std::uint8_t type, std::uint32_t flags, std::uint32_t refid, const B& body) {
		ess::ChangeForm c;
		c.refID.data = refid;
		c.typeField = type;
		c.changeFlags = flags;
		c.data = body;
		c.length1 = static_cast<std::uint32_t>(body.size());
		return c;
	};
	// a created reference as the game writes it with no other change: initial type 5 (worldspace, position, rotation,
	// a byte, base object)
	auto created = [&](std::uint32_t base, float x, float y) {
		B r;
		ref(r, 0x40003C);
		f32(r, x); f32(r, y); f32(r, 100.f);
		f32(r, 0.f); f32(r, 0.f); f32(r, 1.f);
		r.push_back(1);
		ref(r, 0x400000 | base);
		return r;
	};
	ess::SaveFile save;
	save.header.magic = "FO4_SAVEGAME";
	save.formVersion = 68;
	save.visitedWorldspaces = { 0x3C };
	save.changeForms.push_back(mk(0, 0, 0x800101, created(0x24F55, -75436.f, 90779.f)));  // FF000101: nothing refers to it
	save.changeForms.push_back(mk(0, 0, 0x800102, created(0x24F55, 4096.f, 0.f)));       // FF000102: a quest names it
	B q = { 1, 2, 3 };
	ref(q, 0x800102);
	save.changeForms.push_back(mk(8, 0x2, 0x400200, q));  // (QUST bytes holding the RefID)
	const fo4world::Lookup look{ [](std::uint32_t id) { return id == 0x24F55 ? std::string("WEAP") : id == 0x3C ? std::string("WRLD") : std::string(); },
		[](std::uint32_t id) { return id == 0x24F55 ? std::string("Pipe Pistol") : id == 0x3C ? std::string("Commonwealth") : std::string(); } };
	const auto l = fo4world::List(save, look);
	Check(l.items.size() == 2 && l.items[0].removable && l.items[0].kind == "Weapon" && l.items[0].item && l.items[0].exterior &&
			  l.items[0].gridX == -19 && l.items[0].gridY == 22 && l.items[0].base == 0x24F55 && l.items[0].count == 1,
		"fo4 world items: a created reference lying in the world is listed with its kind and cell");
	Check(l.items.size() == 2 && !l.items[1].removable && l.items[1].why.find("00000200") != std::string::npos &&
			  fo4world::PlaceOf(l.items[1], look) == "Commonwealth (1, 0)",
		"fo4 world items: one whose RefID another change form holds is kept, the form named");
	const auto questBody = save.changeForms[2].data;
	const auto rr = fo4world::Remove(save, { 0xFF000101, 0xFF000102, 0xFF000999 });
	Check(rr.removed == 1 && rr.refused.size() == 2 && save.changeForms.size() == 2 && save.changeForms[0].refID.data == 0x800102 &&
			  save.changeForms[1].data == questBody,
		"fo4 world items: only the removable one is removed, the rest refused with a reason, nothing else changed");

	// a CELL change form as the game writes it: interior with a detach time and map seen data
	B cellBody;
	u32(cellBody, 9);
	cellBody.push_back(1 << 2);  // vsval 1
	cellBody.push_back(0x14);
	cellBody.push_back(0xFF);
	cellBody.insert(cellBody.end(), 32, 0x7F);
	const auto cellCf = mk(6, 0xC0000000u, 0x4016D8, cellBody);
	const auto cd = fo4cell::DecodeCell(save, cellCf);
	Check(cd.complete && cd.initialType == 3 && cd.initialSize == 4 && cd.detach == 9 && cd.hasSeen && cd.interior == 1,
		"fo4 cell: interior: detach time, seen data (vsval n x 34 bytes), read to the end");
	B ext = { 1, 0, 0xEF, 0x18 };  // worldspace index 1, x -17, y 24
	u32(ext, 0x22);
	ext.insert(ext.end(), 32, 0x11);
	const auto ed = fo4cell::DecodeCell(save, mk(6, 0xE0000000u, 0x40DCFA, ext));
	Check(ed.complete && ed.initialType == 1 && ed.x == -17 && ed.y == 24 && ed.worldspace == 0x3C && ed.detach == 0x22 && ed.interior == 0,
		"fo4 cell: exterior: worldspace index (1-based), byte coordinates, detach time, 32 bytes of seen data");
	B seenOnly(cellBody.begin() + 4, cellBody.end());
	const auto sd = fo4cell::DecodeCell(save, mk(6, 0x80000000u, 0x4016D8, seenOnly));
	Check(sd.complete && sd.interior == 1 && sd.initialType == 0, "fo4 cell: seen data without a detach time: the layout that reads to the end");

	const char* env = std::getenv("RESAVERPP_FO4_SAVES");
	if (!env || !*env) {
		std::printf("(fo4 world items / cells: RESAVERPP_FO4_SAVES not set, real saves skipped)\n");
		return;
	}
	std::vector<fs::path> files;
	std::string           all = env;
	for (std::size_t p = 0; p <= all.size();) {
		auto e = all.find(';', p);
		if (e == std::string::npos) e = all.size();
		const fs::path dir = all.substr(p, e - p);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
				if (f.path().extension() == ".fos") files.push_back(f.path());
			}
		}
		p = e + 1;
	}
	std::sort(files.begin(), files.end());
	const auto tmp = fs::temp_directory_path() / "rs_unit_world.fos";
	auto       writeBack = [&](ess::SaveFile& s) {
        const auto bytes = s.serialize();
        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        return SaveFile::load(tmp);
	};
	auto bodies = [](const ess::SaveFile& s) {
		std::map<std::uint32_t, B> m;
		for (const auto& c : s.changeForms) m[s.formIDOf(c.refID)] = ess::cf::Body(c);
		return m;
	};
	// Tony's Save7 -> Save8: 9 Pipe Pistols dropped (3 new references; FF000D87 left the inventory, a quest alias)
	const fs::path* s7 = nullptr;
	const fs::path* s8 = nullptr;
	for (const auto& f : files) {
		const auto n = f.filename().string();
		if (n.rfind("Save7_F110F019", 0) == 0) s7 = &f;
		if (n.rfind("Save8_F110F019", 0) == 0) s8 = &f;
	}
	if (s7 && s8) {
		auto a = SaveFile::load(*s7);
		auto b = SaveFile::load(*s8);
		const auto lb = fo4world::List(*b);
		std::map<std::uint32_t, const fo4world::Item*> byId;
		for (const auto& it : lb.items) byId[it.formID] = &it;
		const bool listed = byId.count(0xFF001199) && byId.count(0xFF0011BB) && byId.count(0xFF0013CF) && byId.count(0xFF000D87);
		Check(listed && byId[0xFF001199]->removable && byId[0xFF001199]->count == 3 && byId[0xFF0011BB]->count == 4 &&
				  byId[0xFF0013CF]->count == 1 && byId[0xFF000D87]->count == 1 && byId[0xFF001199]->base == 0x24F55 &&
				  !byId[0xFF000D87]->removable && byId[0xFF000D87]->why.find("quest") != std::string::npos,
			"fo4 world items: Save8 lists the 9 dropped Pipe Pistols (3 + 4 + 1 + 1), FF000D87 kept (quest alias)");
		const auto before = bodies(*b);
		const auto r = fo4world::Remove(*b, { 0xFF001199, 0xFF0011BB, 0xFF0013CF, 0xFF000D87 });
		auto       back = writeBack(*b);
		const auto after = bodies(*back);
		std::set<std::uint32_t> ka, kb;
		for (const auto& c : a->changeForms) ka.insert(a->formIDOf(c.refID));
		for (const auto& c : back->changeForms) kb.insert(back->formIDOf(c.refID));
		bool same = after.size() + 3 == before.size();
		for (const auto& [id, body] : after) same = same && before.count(id) && before.at(id) == body;
		Check(r.removed == 3 && r.refused.size() == 1 && ka == kb && same && back->formIDs == a->formIDs,
			"fo4 world items: Save8 minus the dropped pistols has Save7's change forms and form id array, nothing else changed");
	}
	std::size_t tested = 0, removedAll = 0;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;
		}
		const auto name = f.filename().string();
		const auto l2 = fo4world::List(*s);
		std::set<std::uint32_t> ids;
		for (const auto& it : l2.items) if (it.removable) ids.insert(it.formID);
		const auto before = bodies(*s);
		const auto r = fo4world::Remove(*s, ids);
		auto       back = writeBack(*s);
		const auto after = bodies(*back);
		bool same = after.size() + ids.size() == before.size();
		for (const auto& [id, body] : after) same = same && !ids.count(id) && before.count(id) && before.at(id) == body;
		const auto l3 = fo4world::List(*back);
		std::size_t still = 0;
		for (const auto& it : l3.items) still += it.removable && ids.count(it.formID);
		Check(r.removed == ids.size() && r.refused.empty() && same && still == 0,
			("fo4 world items: " + name + ": every removable item removed, written, read back, nothing else changed").c_str());
		removedAll += r.removed;

		// reset: an interior with a change form, an interior without one, an exterior; the player's cell refused
		auto cs = SaveFile::load(f);
		const auto cells = fo4cell::ListCells(*cs);
		std::vector<std::uint32_t> pick;
		const fo4cell::CellInfo* playerCell = nullptr;
		bool intCf = false, intNew = false, ext2 = false;
		for (const auto& c : cells) {
			if (c.player) playerCell = &c;
			if (!c.why.empty()) continue;
			if (!c.exterior && c.hasChangeForm && !intCf) { intCf = true; pick.push_back(c.formID); }
			else if (!c.exterior && !c.hasChangeForm && !intNew) { intNew = true; pick.push_back(c.formID); }
			else if (c.exterior && !ext2) { ext2 = true; pick.push_back(c.formID); }
		}
		const auto cbefore = bodies(*cs);
		bool       ok = true;
		for (const auto id : pick) ok = ok && fo4cell::ResetCell(*cs, id).empty();
		if (playerCell) ok = ok && !fo4cell::ResetCell(*cs, playerCell->formID).empty();
		auto cback = writeBack(*cs);
		const auto cafter = bodies(*cback);
		for (const auto id : pick) {
			const ess::ChangeForm* cf = nullptr;
			for (const auto& c : cback->changeForms) if (c.typeCode() == 6 && cback->formIDOf(c.refID) == id) cf = &c;
			const auto d = cf ? fo4cell::DecodeCell(*cback, *cf) : fo4cell::CellData{};
			ok = ok && cf && d.complete && d.detach == fo4cell::kDetachReset && (cf->changeFlags & 0x40000000u);
		}
		std::size_t changed = 0;
		for (const auto& [id, body] : cafter) changed += !cbefore.count(id) || cbefore.at(id) != body;
		Check(ok && playerCell && changed == pick.size() && cafter.size() == cbefore.size() + (intNew ? 1 : 0),
			("fo4 cell: " + name + ": cells reset (detach time 0xFFFFFFFF), written, read back, nothing else changed; the player's cell refused").c_str());
		std::printf("(fo4 world: %s: %zu items listed, %zu removed; %zu cells, %zu reset)\n", name.c_str(), l2.items.size(), r.removed, cells.size(), pick.size());
		++tested;
	}
	fs::remove(tmp);
	std::printf("(fo4 world items / cells: %zu real saves, %zu items removed)\n", tested, removedAll);
}

// Fallout 4 reset an area with the plugins' CELL records (PluginDB::CellDef through fo4world::LookupFrom). Needs
// RESAVERPP_FO4_SAVES (Tony's Save8 carries the synthetic cases: the player's place must be readable) and, for the real
// plugins part, RESAVERPP_FO4_DATA (Fallout4.esm and the DLC esm files).
void TestFo4CellReset()
{
	namespace fs = std::filesystem;
	using B = std::vector<std::uint8_t>;
	const char* env = std::getenv("RESAVERPP_FO4_SAVES");
	if (!env || !*env) {
		std::printf("(fo4 cell reset: RESAVERPP_FO4_SAVES not set, skipped)\n");
		return;
	}
	std::vector<fs::path> files;
	for (std::string all = env, dir; !all.empty();) {
		const auto e = all.find(';');
		dir = all.substr(0, e);
		all = e == std::string::npos ? std::string() : all.substr(e + 1);
		std::error_code ec;
		if (!dir.empty() && fs::is_directory(dir, ec)) {
			for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
				if (f.path().extension() == ".fos") files.push_back(f.path());
			}
		}
	}
	std::sort(files.begin(), files.end());
	const auto tmp = fs::temp_directory_path() / "rs_unit_cells.fos";
	auto writeBack = [&](ess::SaveFile& s) {
		const auto bytes = s.serialize();
		{
			std::ofstream out(tmp, std::ios::binary);
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		}
		return SaveFile::load(tmp);
	};
	auto cellCf = [](const ess::SaveFile& s, std::uint32_t id) -> const ess::ChangeForm* {
		for (const auto& c : s.changeForms)
			if (c.typeCode() == 6 && s.formIDOf(c.refID) == id) return &c;
		return nullptr;
	};

	// synthetic cells on Tony's Save8 (made-up plugin records for 0000A001..; the save has no change forms for them)
	const fs::path* s8 = nullptr;
	for (const auto& f : files)
		if (f.filename().string().rfind("Save8_F110F019", 0) == 0) s8 = &f;
	if (s8) {
		auto save = SaveFile::load(*s8);
		auto mk = [&](std::uint32_t ref, std::uint32_t flags, const B& body) {
			ess::ChangeForm c;
			c.refID.data = ref;
			c.typeField = 6;
			c.version = save->changeForms.front().version;
			c.changeFlags = flags;
			c.data = body;
			c.length1 = static_cast<std::uint32_t>(body.size());
			save->changeForms.push_back(c);
		};
		B flagsOnly = { 0x84, 0x00 };  // bit 1 only: an indoor cell whose only change is its flags
		mk(0x40A005, 0x2, flagsOnly);
		B seenFlags = { 0x02, 0x00 };  // bit 1 + bit 31 (exterior seen data, 32 bytes), no detach time
		seenFlags.insert(seenFlags.end(), 32, 0x11);
		mk(0x40A001, 0x80000002u, seenFlags);
		save->reindexChangeForms();
		const auto visited = save->visitedWorldspaces;
		fo4world::Lookup look;
		look.type = [](std::uint32_t id) {
			return id == 0x3C || id == 0x1234 || id == 0xA00A ? std::string("WRLD") : id >= 0xA001 && id <= 0xA009 ? std::string("CELL") : std::string();
		};
		look.cell = [](std::uint32_t id) {
			fo4world::CellRecord r;
			r.known = id >= 0xA001 && id <= 0xA009;
			r.interior = id == 0xA003 || id == 0xA005;
			r.hasGrid = !r.interior;
			r.persistent = id == 0xA006;
			r.worldspace = id == 0xA004 ? 0x1234 : 0x3C;
			r.x = id == 0xA001 ? -5 : id == 0xA002 ? 200 : 3;
			r.y = id == 0xA001 ? 7 : id == 0xA002 ? -300 : 4;
			return r;
		};
		// without the plugins the flags-only cell is refused (the save does not say indoors)
		Check(!fo4cell::ResetCell(*save, 0xA005).empty(), "fo4 cell reset: without plugin data a flags-only cell is refused");
		const auto r = fo4cell::ResetCells(*save, { 0xA001, 0xA002, 0xA003, 0xA004, 0xA005, 0xA006, 0xA00A, 0xFF000BDD }, look);
		Check(r.size() == 8 && r[0].empty() && r[1].empty() && r[2].empty() && r[3].empty() && r[4].empty() &&
				  r[5].find("persistent") != std::string::npos && r[6].find("not a cell") != std::string::npos &&
				  r[7].find("made during play") != std::string::npos,
			"fo4 cell reset: outdoor / indoor cells from the plugins reset; persistent cell, worldspace, FF cell refused");
		auto back = writeBack(*save);
		auto body = [&](std::uint32_t id) { const auto* c = cellCf(*back, id); return c ? ess::cf::Body(*c) : B{}; };
		auto flags = [&](std::uint32_t id) { const auto* c = cellCf(*back, id); return c ? c->changeFlags : 0u; };
		const std::size_t n = back->visitedWorldspaces.size();
		const auto ix = static_cast<std::uint8_t>(std::find(visited.begin(), visited.end(), 0x3Cu) - visited.begin() + 1);
		B e1 = { ix, 0, 0xFB, 0x07, 0xFF, 0xFF, 0xFF, 0xFF };
		e1.insert(e1.end(), seenFlags.begin(), seenFlags.end());
		B e2 = { ix, 0, 200, 0, 0xD4, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF };
		B e4 = { static_cast<std::uint8_t>(n), 0, 3, 4, 0xFF, 0xFF, 0xFF, 0xFF };
		B e5 = { 0xFF, 0xFF, 0xFF, 0xFF, 0x84, 0x00 };
		Check(flags(0xA001) == 0xE0000002u && body(0xA001) == e1, "fo4 cell reset: outdoor change form without a detach time: bits 30+29, {index, i8 x, i8 y, FFFFFFFF} put in front");
		Check(flags(0xA002) == 0x50000000u && body(0xA002) == e2, "fo4 cell reset: outdoor cell beyond a byte's range: bits 30+28, i16 coordinates");
		Check(flags(0xA003) == 0x40000000u && body(0xA003) == B(4, 0xFF), "fo4 cell reset: indoor cell with no change form: bit 30, 4 bytes");
		Check(flags(0xA004) == 0x60000000u && body(0xA004) == e4 && n == visited.size() + 1 && back->visitedWorldspaces.back() == 0x1234,
			"fo4 cell reset: a worldspace the save has not visited is appended to the array and its index written");
		Check(flags(0xA005) == 0x40000002u && body(0xA005) == e5, "fo4 cell reset: indoor flags-only change form: bit 30, detach time in front of the flags");
		Check(!cellCf(*back, 0xA006) && fo4cell::ResetCell(*back, 0xA001, look).find("already") != std::string::npos,
			"fo4 cell reset: refused cells left alone; a second reset is refused (already set)");
	}

	const char* data = std::getenv("RESAVERPP_FO4_DATA");
	if (!data || !*data) {
		std::printf("(fo4 cell reset: RESAVERPP_FO4_DATA not set, real plugins skipped)\n");
		return;
	}
	ScriptSources src;
	src.scan(data);
	std::size_t tested = 0, resetAll = 0;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;
		}
		const auto name = f.filename().string();
		PluginDB::Input in;
		PrepareInput(*s, in);
		in.sources = src;
		in.fo4 = true;
		in.language = "en";
		PluginDB::Progress pr;
		PluginDB           db;
		db.load(in, pr);
		const auto look = fo4world::LookupFrom(&db);
		if (s8 && f == *s8) {
			// Fallout4.esm as an independent scan of the file reads it (CELL DATA bit 0, XCLC, the World Children group label): Diamond City houses indoors, an outdoor cell's place, a persistent cell, the grid
			const auto* house = db.cell(0x1E4A);
			const auto* out = db.cell(0xA800E);
			const auto* pers = db.cell(0x18AA2);
			Check(house && house->interior() && !house->worldspace && out && !out->interior() && out->worldspace == 0xA7FF4 &&
					  out->x == -18 && out->y == 24 && pers && pers->persistent && pers->worldspace == 0x3C &&
					  db.cellAt(0xA7FF4, -18, 24) == 0xA800E && db.cellAt(0x3C, 0, 0) != 0x18AA2,
				"fo4 plugins: CELL DATA / XCLC / worldspace / persistent cell / grid kept");
		}
		const auto cells = fo4cell::ListCells(*s, look);
		std::vector<std::uint32_t> ids;
		std::size_t flagsOnly = 0, noCf = 0, outdoorNew = 0;
		for (const auto& c : cells) {
			if (!c.why.empty()) continue;
			ids.push_back(c.formID);
			flagsOnly += c.hasChangeForm && !c.hasDetach && !c.exterior;
			noCf += !c.hasChangeForm;
			outdoorNew += !c.hasChangeForm && c.exterior;
		}
		std::map<std::uint32_t, B> before;
		for (const auto& c : s->changeForms) before[s->formIDOf(c.refID)] = ess::cf::Body(c);
		const auto formIDs = s->formIDs;
		const auto visited = s->visitedWorldspaces;
		const auto res = fo4cell::ResetCells(*s, ids, look);
		bool allDone = true;
		for (const auto& w : res) allDone = allDone && w.empty();
		auto back = writeBack(*s);
		const std::set<std::uint32_t> idSet(ids.begin(), ids.end());
		bool ok = allDone;
		std::size_t created = 0;
		for (const auto& c : back->changeForms) {
			const auto id = back->formIDOf(c.refID);
			const auto b = ess::cf::Body(c);
			if (!idSet.contains(id)) {
				ok = ok && before.count(id) && before.at(id) == b;
				continue;
			}
			const auto d = fo4cell::DecodeCell(*back, c);
			ok = ok && d.complete && d.detach == fo4cell::kDetachReset && (c.changeFlags & 0x40000000u);
			created += !before.count(id);
		}
		const bool prefixKept = std::equal(formIDs.begin(), formIDs.end(), back->formIDs.begin()) &&
		                        std::equal(visited.begin(), visited.end(), back->visitedWorldspaces.begin());
		Check(ok && prefixKept && back->changeForms.size() == before.size() + created && created == noCf,
			("fo4 cell reset: " + name + ": every resettable cell reset with the plugins, written, read back (detach 0xFFFFFFFF), nothing else changed").c_str());
		std::printf("(fo4 cell reset: %s: %zu cells, %zu reset (%zu indoor flags-only, %zu new change forms, %zu of them outdoors))\n", name.c_str(),
			cells.size(), ids.size(), flagsOnly, noCf, outdoorNew);
		resetAll += ids.size();
		++tested;
	}
	std::error_code ec;
	fs::remove(tmp, ec);
	std::printf("(fo4 cell reset: %zu real saves, %zu cells reset)\n", tested, resetAll);
}


// Character effects (core/CharEffects.h): grouping and stuck detection on a hand-made view; real saves with
// RESAVERPP_SKY_SAVES / RESAVERPP_FO4_SAVES (no stuck effect on Tony's saves), RESAVERPP_TG_SAVE (trashgod69's save: the
// five enchantments whose item is gone), RESAVERPP_FO4_DATA (plugins: an unequipped legendary item's effects are found
// and removed).
void TestCharEffects()
{
	namespace fs = std::filesystem;
	auto CheckS = [](bool a_ok, const std::string& a_what) { Check(a_ok, a_what.c_str()); };
	{
		chfx::View v;
		auto fx = [&](std::uint32_t item, std::uint32_t flags, std::uint32_t caster, std::uint32_t source, float dur = 0) {
			chfx::Effect e;
			e.index = v.effects.size();
			e.item = item;
			e.flags = flags;
			e.hasRefs = true;
			e.caster = caster;
			e.target = 0x14;
			e.source = source;
			e.duration = dur;
			v.effects.push_back(e);
		};
		chfx::Worn ring;
		ring.item = 0x0100A000;  // worn, carries enchantment 0x0100E004 on its stack
		ring.enchantment = 0x0100E004;
		v.worn.push_back(ring);
		v.carried = { 0x0100A000, 0x0100B000 };
		fx(0x0100E001, 0x10300, 0x14, 0x0100A000);            // 0: from the worn ring
		fx(0x0100E002, 0x10300, 0x14, 0x0100B000);            // 1: from a carried, unworn item: stuck
		fx(0x0100E002, 0x18300, 0x14, 0x0100B000);            // 2: same source, switched off: stuck, not running
		fx(0x0100E003, 0x10300, 0x14, 0xFF000C00);            // 3: the item is gone: stuck
		fx(0x0100E004, 0x10300, 0x14, 0x0100D000);            // 4: the worn ring carries this enchantment: worn
		fx(0x0100E005, 0x10300, 0x00001234, 0x0100C000, 20);  // 5: someone else's weapon: others
		fx(0x0100E006, 0x10300, 0x14, 0x0100C000, 30);        // 6: timed: not stuck
		fx(0x0100E007, 0x50300, 0x14, 0x0100C000);            // 7: dispelled: not stuck (the game drops it)
		fx(0x05000001, 0x14200, 0x14, 0);                     // 8: no plugin read, the plugin is gone
		fx(0x0100E008, 0x10300, 0x14, 0x0100A000);            // 9: an enchantment a script put on the worn ring: worn
		v.pluginOf = [](std::uint32_t id) { return std::string(id >> 24 == 5 ? "Gone.esp" : id >> 24 == 0xFF ? "Created" : "Mod.esp"); };
		v.savePlugins = { "Skyrim.esm", "Mod.esp", "Gone.esp" };
		std::set<std::string> lo = { "skyrim.esm", "mod.esp" };
		chfx::Options opt;
		opt.loadOrder = &lo;
		chfx::Classify(v, opt);
		auto cardOf = [&](std::size_t i) -> const chfx::Card* {
			for (const auto& c : v.cards) {
				if (std::find(c.effects.begin(), c.effects.end(), i) != c.effects.end()) {
					return &c;
				}
			}
			return nullptr;
		};
		CheckS(cardOf(0)->group == chfx::Group::kWorn && cardOf(4)->group == chfx::Group::kWorn && cardOf(9)->group == chfx::Group::kWorn,
			"char effects: worn by source, by the stack's enchantment, and a scripted enchantment on a worn item");
		CheckS(cardOf(1) == cardOf(2) && cardOf(1)->group == chfx::Group::kNotWorn && cardOf(1)->running == 1, "char effects: one card per enchantment + item");
		CheckS(cardOf(5)->group == chfx::Group::kOthers && cardOf(8)->group == chfx::Group::kUnknown, "char effects: others' effects, unknown items");
		std::size_t notWorn = 0, gone = 0;
		std::set<std::size_t> stuckFx;
		for (const auto& f : v.stuck) {
			notWorn += f.kind == chfx::Stuck::kNotWorn;
			gone += f.kind == chfx::Stuck::kPluginGone;
			stuckFx.insert(f.effects.begin(), f.effects.end());
		}
		CheckS(notWorn == 2 && gone == 1 && stuckFx == std::set<std::size_t>{ 1, 2, 3, 8 }, "char effects: stuck = unworn item, gone item, gone plugin; not timed / dispelled / others' / worn");
		bool carriedText = false, goneText = false;
		for (const auto& f : v.stuck) {
			carriedText |= f.text.find("you carry") != std::string::npos;
			goneText |= f.text.find("not in your inventory any more") != std::string::npos;
		}
		CheckS(carriedText && goneText && v.worn[0].effects == 3, "char effects: finding texts; the ring counts its 3 worn effects (source or enchantment)");
		std::set<std::string> other = { "skyrim.esm" };
		opt.loadOrder = &other;
		v.pluginOf = [](std::uint32_t id) { return std::string(id >> 24 == 5 ? "Gone.esp" : "Mod.esp"); };
		v.savePlugins = { "Skyrim.esm", "Mod.esp", "Gone.esp" };
		chfx::Classify(v, opt);
		CheckS(v.loadOrderOther && !v.loadOrderChecked, "char effects: a load order without most of the save's plugins is another setup: gone plugins not reported");
	}

	auto list = [](const char* a_env, const char* a_ext) {
		std::vector<fs::path> files;
		const char*           env = std::getenv(a_env);
		std::string           all = env ? env : "";
		for (std::size_t p = 0; p < all.size();) {
			auto e = all.find(';', p);
			if (e == std::string::npos) {
				e = all.size();
			}
			const fs::path dir = all.substr(p, e - p);
			std::error_code ec;
			if (fs::is_directory(dir, ec)) {
				for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
					if (f.path().extension() == a_ext) {
						files.push_back(f.path());
					}
				}
			} else if (fs::is_regular_file(dir, ec)) {
				files.push_back(dir);
			}
			p = e + 1;
		}
		std::sort(files.begin(), files.end());
		return files;
	};
	for (const auto& f : list("RESAVERPP_SKY_SAVES", ".ess")) {
		auto       s = SaveFile::load(f);
		const auto v = chfx::Build(*s, {});
		std::size_t fromItem = 0, wornSrc = 0;
		for (const auto& e : v.effects) {
			fromItem += e.fromItem();
			for (const auto& w : v.worn) {
				wornSrc += e.fromItem() && e.source == w.item;
			}
		}
		CheckS(v.error.empty() && v.stuck.empty() && fromItem == wornSrc && !v.worn.empty(),
			"char effects " + f.filename().string().substr(0, 10) + ": no stuck effect; every item effect's source is worn");
	}
	if (const char* tg = std::getenv("RESAVERPP_TG_SAVE"); tg && *tg) {
		auto       s = SaveFile::load(tg);
		const auto v = chfx::Build(*s, {});
		std::set<std::uint32_t> sources;
		std::size_t             fx = 0;
		for (const auto& f : v.stuck) {
			sources.insert(v.cards[f.card].source);
			fx += f.effects.size();
		}
		CheckS(v.effects.size() == 1927 && v.worn.size() == 7 && v.stuck.size() == 5 && fx == 9 &&
				  sources == std::set<std::uint32_t>{ 0xFF009928, 0xFF007039, 0xFF006F82, 0xFF008650, 0xFF003A1F },
			"char effects trashgod69: 5 enchantments (9 effects) whose item is in no inventory");
		std::vector<std::size_t> rm;
		for (const auto& f : v.stuck) {
			for (const auto i : f.effects) {
				if (v.effects[i].canRemove) {
					rm.push_back(i);
				}
			}
		}
		std::vector<std::string> changes;
		const auto               why = chfx::Remove(*s, v, rm, &changes);
		const auto               tmp = fs::temp_directory_path() / "rs_cfx_tg.ess";
		s->write(tmp, false);
		auto       back = SaveFile::load(tmp);
		const auto v2 = chfx::Build(*back, {});
		CheckS(why.empty() && rm.size() == 4 && v2.effects.size() == 1923 && v2.stuck.size() == 3,
			"char effects trashgod69: without plugins the 4 effects that change no saved stat are removed, written, read back: 3 findings left (" + why + ")");
		fs::remove(tmp);
		fs::path t2 = tmp;
		fs::remove(t2.replace_extension(".skse"));
	}
	std::unique_ptr<PluginDB> f4db;
	const char*               data = std::getenv("RESAVERPP_FO4_DATA");
	ScriptSources             f4src;
	if (data && *data) {
		f4src.scan(data);
	}
	for (const auto& f : list("RESAVERPP_FO4_SAVES", ".fos")) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;  // (fo4full's Quicksave0 is cut short)
		}
		std::unique_ptr<PluginDB> db;
		if (data && *data) {
			PluginDB::Input in;
			PrepareInput(*s, in);
			in.sources = f4src;
			in.fo4 = true;
			in.language = "en";
			PluginDB::Progress pr;
			db = std::make_unique<PluginDB>();
			db->load(in, pr);
		}
		chfx::Options opt;
		opt.db = db.get();
		const auto v = chfx::Build(*s, opt);
		const auto tag = "char effects " + f.filename().string().substr(0, 10) + ": ";
		CheckS(v.error.empty() && v.stuck.empty() && !v.worn.empty(), tag + "no stuck effect, something worn");
		// take an equipped item with effects off (its stack flags to 0): its effects become stuck
		// (one whose enchantments no other worn item gives too: an enchantment a worn item carries is not reported)
		std::uint32_t item = 0;
		for (const auto& w : v.worn) {
			bool shared = false;
			for (const auto& c : v.cards) {
				for (const auto& c2 : v.cards) {
					shared |= c.source == w.item && c2.item == c.item && c2.source != w.item && c2.group == chfx::Group::kWorn;
				}
			}
			if (w.effects && !shared) {
				item = w.item;
				break;
			}
		}
		if (!item) {
			continue;
		}
		auto* pc = s->findChangeForm(fo4cf::FindPlayer(*s)->refID);
		auto  body = cf::Body(*pc);
		const auto refr = fo4cf::DecodeRefr(*s, *pc);
		bool  flagsAt = true;
		for (const auto& it : refr.inventory) {
			if (it.formID != item) {
				continue;
			}
			for (const auto& st : it.stacks) {
				const auto at = st.countRange.end;
				flagsAt = flagsAt && at + 2 <= st.range.end && (body[at] | body[at + 1] << 8) == st.flags;
				body[at] = 0;
				body[at + 1] = 0;
			}
		}
		CheckS(flagsAt, tag + "the stack flags are the u16 after the count");
		cf::SetBody(*pc, body);
		const auto v2 = chfx::Build(*s, opt);
		std::size_t n = 0;
		std::vector<std::size_t> rm;
		for (const auto& fd : v2.stuck) {
			n += fd.effects.size();
			for (const auto i : fd.effects) {
				if (v2.effects[i].canRemove) {
					rm.push_back(i);
				}
			}
		}
		std::size_t expect = 0;
		for (const auto& e : v2.effects) {
			expect += e.fromItem() && e.source == item && e.duration == 0 && !(e.flags & 0x40000u);
		}
		CheckS(n == expect && n > 0, tag + "unequipped an item: its " + std::to_string(expect) + " effect(s) are found stuck");
		if (db) {
			const auto why = chfx::Remove(*s, v2, rm);
			const auto v3 = chfx::Build(*s, opt);
			CheckS(why.empty() && rm.size() == n && v3.effects.size() + n == v2.effects.size() && v3.stuck.empty(),
				tag + "with the plugins they are removed and the player reads back without them (" + why + ")");
		}
	}
}

// Cleanups and ActiveMagicEffect instances (Save140 investigation, see the handoff). RESAVERPP_SKY_SAVES / RESAVERPP_TG_SAVE:
// every AME handle has a unique id; an AME instance whose effect is in its actor's saved list is kept by Remove undefined
// elements (with its script), the others of that script go, the save writes and reads back; an actor the same edit deletes
// takes its instances with it; Compare lists an active effect taken off the player. RESAVERPP_S140="old.ess;new.ess":
// Tony's two saves (the cleanup removes the same 6 elements and keeps none; old + cleanup differs from new only in the player).
void TestLiveEffectInstances()
{
	namespace fs = std::filesystem;
	auto CheckS = [](bool a_ok, const std::string& a_what) { Check(a_ok, a_what.c_str()); };
	auto list = [](const char* a_env) {
		std::vector<fs::path> files;
		const char*           env = std::getenv(a_env);
		if (!env || !*env) {
			return files;
		}
		std::string all = env;
		for (std::size_t p = 0; p <= all.size();) {
			auto e = all.find(';', p);
			if (e == std::string::npos) {
				e = all.size();
			}
			const fs::path dir = all.substr(p, e - p);
			std::error_code ec;
			if (fs::is_directory(dir, ec)) {
				for (const auto& f : fs::directory_iterator(dir, ec)) {
					if (f.path().extension() == ".ess") {
						files.push_back(f.path());
					}
				}
			} else if (fs::is_regular_file(dir, ec)) {
				files.push_back(dir);
			}
			p = e + 1;
		}
		std::sort(files.begin(), files.end());
		return files;
	};
	auto saves = list("RESAVERPP_SKY_SAVES");
	if (const char* tg = std::getenv("RESAVERPP_TG_SAVE"); tg && *tg) {
		saves.emplace_back(tg);
	}
	if (saves.empty()) {
		std::printf("(live effect instances: RESAVERPP_SKY_SAVES not set, real saves skipped)\n");
	}
	for (const auto& f : saves) {
		const auto tag = "live fx " + f.filename().string().substr(0, 10) + ": ";
		auto       s = SaveFile::load(f);
		auto*      p = s->papyrus();
		std::set<EID> ame;
		bool          noZero = true;
		for (const auto& i : p->instances) {
			if (i.unknown2bits == 2) {
				ame.insert(i.id);
				noZero = noZero && i.unknown != 0;
			}
		}
		CheckS(!ame.empty() && noZero, tag + "every ActiveMagicEffect handle has a unique id (" + std::to_string(ame.size()) + ")");
		const auto live = ops::LiveEffectInstances(*s, ame);
		std::size_t read = 0;
		for (const auto& k : live) {
			read += !k.unread;
		}
		CheckS(read > 0, tag + std::to_string(read) + " AME instance(s) whose effect is in the actor's saved list");
		// one script with a running effect's instance: make it undefined as the game writes it (no parent), clean
		const ops::KeptInstance* pick = nullptr;
		for (const auto& k : live) {
			if (!k.unread) {
				pick = &k;
				break;
			}
		}
		if (!pick) {
			continue;
		}
		const auto* inst = p->findInstance(pick->id);
		const TStr  script = inst->script;
		const auto  empty = p->internString("");
		std::set<EID> ofScript, liveOfScript;
		for (const auto& i : p->instances) {
			if (i.script == script) {
				ofScript.insert(i.id);
			}
		}
		for (const auto& k : live) {
			if (!k.unread && ofScript.contains(k.id)) {
				liveOfScript.insert(k.id);
			}
		}
		for (auto& sc : p->scripts) {
			if (sc.name == script) {
				sc.parent = empty;
			}
		}
		const auto r = ops::RemoveUndefinedElements(*s);
		std::set<EID> kept;
		for (const auto& k : r.kept) {
			kept.insert(k.id);
		}
		std::size_t left = 0;
		for (const auto& i : p->instances) {
			left += i.script == script;
		}
		CheckS(kept == liveOfScript && left == liveOfScript.size() && p->findScript(script) != nullptr,
			tag + "Remove undefined elements keeps the " + std::to_string(liveOfScript.size()) + " running one(s) of " + std::to_string(ofScript.size()) +
				" " + p->str(script) + " instance(s) and the script");
		std::printf("(%s%zu AME instances, %zu with their effect in the saved list; %s: kept %zu of %zu, removed %zu)\n", tag.c_str(), ame.size(),
			read, p->str(script).c_str(), kept.size(), ofScript.size(), ofScript.size() - kept.size());
		const auto tmp = fs::temp_directory_path() / "rs_unit_livefx.ess";
		s->save(tmp);
		auto back = SaveFile::load(tmp);
		std::size_t backLeft = 0;
		for (const auto& i : back->papyrus()->instances) {
			backLeft += back->papyrus()->str(i.script) == p->str(script);
		}
		CheckS(back->papyrus()->check().empty() && back->problems.empty() && backLeft == liveOfScript.size(), tag + "written and read back clean");
		fs::remove(tmp);
		fs::remove(fs::path(tmp).replace_extension(".skse"));
		// the same instances when the edit also deletes the actor: they go
		auto s2 = SaveFile::load(f);
		std::set<std::uint32_t> going;
		for (const auto& i : s2->papyrus()->instances) {
			if (liveOfScript.contains(i.id)) {
				going.insert(i.refID.data);
			}
		}
		CheckS(ops::LiveEffectInstances(*s2, liveOfScript, &going).empty(), tag + "an actor deleted by the same edit takes its instances with it");
		// Compare: an effect taken off the player shows as the player's ACHR edited in place
		std::string err;
		const auto  fx = cf::ReadPlayerActiveEffects(*s2, err);
		std::size_t at = fx.size();
		for (std::size_t i = 0; i < fx.size(); ++i) {
			if (cf::ActiveEffectModifier(fx[i]) < 0) {
				at = i;
				break;
			}
		}
		if (at < fx.size()) {
			auto s3 = SaveFile::load(f);
			const auto why = cf::RemovePlayerActiveEffects(*s3, { cf::EffectRemoval{ at, -1 } });
			const auto c = compare::Compare(*s2, *s3);
			char wantBuf[64];
			std::snprintf(wantBuf, sizeof(wantBuf), "active effects %zu -> %zu", fx.size(), fx.size() - 1);
			const std::string want = wantBuf;
			CheckS(why.empty() && c.editedCount == 1 && !c.edited.empty() && c.edited[0].formID == 0x14 && c.edited[0].note == want,
				tag + "Compare lists the player's ACHR with " + want);
		}
	}
	// Tony's Save140
	std::vector<fs::path> s140;  // in the order given: old, new
	if (const char* e = std::getenv("RESAVERPP_S140"); e && *e) {
		const std::string v = e;
		if (const auto semi = v.find(';'); semi != std::string::npos) {
			s140 = { v.substr(0, semi), v.substr(semi + 1) };
		}
	}
	if (s140.size() == 2) {
		auto oldS = SaveFile::load(s140[0]);
		auto newS = SaveFile::load(s140[1]);
		const auto c = compare::Compare(*oldS, *newS);
		CheckS(c.editedCount == 1 && c.edited[0].formID == 0x14 && c.edited[0].note == "active effects 165 -> 164",
			"Save140: new.ess = old.ess with Papyrus cleaned AND one player effect removed (Compare shows it)");
		const auto r = ops::RemoveUndefinedElements(*oldS);
		const auto c2 = compare::Compare(*oldS, *newS);
		CheckS(r.removed == 6 && r.kept.empty() && c2.editedCount == 1 && c2.instancesByScript.empty(),
			"Save140: the cleanup removes the same 6 undefined elements (none is an AME), leaving only the player's effect as the difference");
		std::printf("(Save140: Compare old -> new edited %zu: %s; cleanup removed %zu, kept %zu)\n", c.editedCount, c.edited.empty() ? "" : c.edited[0].note.c_str(),
			r.removed, r.kept.size());
	} else {
		std::printf("(Save140: RESAVERPP_S140=\"old.ess;new.ess\" not set, skipped)\n");
	}
}

// Suspended stacks report (Nexus, 8 Nov 2026): deleting suspended stacks without their threads left threads nothing
// resumes; SkyrimSE.exe 1.6.1170 then never finishes pausing its scripts (0x14517c0) and writes every later save's
// Papyrus block as FF FF FF FF (flagged invalid), which ReSaver Plus could not read ("needed 4 bytes while reading the
// header"). Synthetic: each delete keeps every thread held; the extra slot (unk1/unk2) holds a thread; the invalid
// block reads and writes back. Real saves (RESAVERPP_SKY_SAVES, RESAVERPP_TG_SAVE, RESAVERPP_STACK_SAVES="a.ess;b.ess"):
// no stranded thread in a game-made save, every thread held exactly once; delete suspended stacks, write, read back:
// consistent; the old delete (entries only) leaves exactly that many stranded; a game-style invalid block on a real
// save reads, keeps everything else and writes back byte-identical.
void TestSuspendedStacks()
{
	namespace fs = std::filesystem;
	auto CheckS = [](bool a_ok, const std::string& a_what) { Check(a_ok, a_what.c_str()); };
	auto heldOnce = [](const Papyrus& p) {
		std::map<ThreadID, int> held;
		for (const auto* list : { &p.suspended1, &p.suspended2 }) {
			for (const auto& x : *list) {
				held[x.id]++;
			}
		}
		for (const auto& m : p.functionMessages) {
			if (m.id) {
				held[*m.id]++;
			}
		}
		if (p.unk1 && p.unk2) {
			held[*p.unk2]++;
		}
		std::size_t bad = 0;
		for (const auto& a : p.activeScripts) {
			bad += (held[a.id] + (a.type != 0 ? 1 : 0)) != 1;
		}
		std::size_t dangling = 0;
		for (const auto& [id, n] : held) {
			dangling += p.findActive(id) == nullptr;
		}
		return std::pair{ bad, dangling };
	};
	{
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		for (ThreadID t : { 7u, 8u, 9u, 10u, 11u }) {
			p.activeScripts.push_back(Thread(t, 1, Ref(0)));
			p.activeDataOrder.push_back(t);
		}
		p.activeScripts[2].type = 1;  // 9: latent wait
		SuspendedStack st;
		st.id = 7;
		st.flag = 1;
		st.data = Call(1, Ref(0));
		p.suspended1.push_back(st);
		FunctionMessage m;
		m.flag = 1;
		m.id = 8;
		m.flags = 1;
		m.data = Call(2, Ref(0));
		p.functionMessages.push_back(m);
		m.flag = 3;  // a queued call with no thread
		m.id.reset();
		p.functionMessages.push_back(m);
		st.id = 10;
		p.suspended1.push_back(st);
		p.unk1 = 1;  // 11: the engine's extra slot
		p.unk2 = 11;
		p.reindex();
		Check(ops::StrandedThreads(p).empty(), "stacks: suspended, queued, latent and the extra slot each hold a thread");

		auto old = p;
		old.reindex();
		ops::DeleteSuspendedStacks(old, { 7 });
		const auto st1 = ops::StrandedThreads(old);
		Check(st1.size() == 1 && st1[0] == 7, "stacks: deleting only the suspended entry leaves its thread stranded (the bug)");
		const auto rs = ops::RemoveStrandedThreads(old);
		Check(rs.threads == 1 && !old.findActive(7) && ops::StrandedThreads(old).empty(), "stacks: the stranded thread is removed");

		auto r = ops::RemoveSuspendedStacks(p, { 7 });
		Check(r.stacks == 1 && r.threads == 1 && !p.findActive(7) && ops::StrandedThreads(p).empty(),
			"stacks: deleting a suspended stack takes its thread with it");
		r = ops::RemoveFunctionMessages(p, { 0, 1 });
		Check(r.messages == 2 && r.threads == 1 && !p.findActive(8) && p.functionMessages.empty() && ops::StrandedThreads(p).empty(),
			"stacks: deleting queued calls takes the thread each was made for (none for a call without one)");
		r = ops::RemoveThreads(p, { 10, 11 });
		Check(r.threads == 2 && r.stacks == 1 && p.suspended1.empty() && p.unk1 == 0 && !p.unk2,
			"stacks: deleting threads takes their suspended stacks and empties the extra slot");
		Check(p.activeScripts.size() == 1 && p.activeScripts[0].id == 9 && heldOnce(p) == std::pair<std::size_t, std::size_t>{ 0, 0 },
			"stacks: the latent thread stays, held");
		Writer w;
		p.write(w);
		Papyrus q;
		Reader rd(w.buf.data(), w.buf.size());
		q.read(rd);
		Check(q.activeScripts.size() == 1 && q.unk1 == 0 && !q.unk2 && q.check().empty(), "stacks: the edited section reads back");
	}
	// the game's invalid block: VM u16 0xFFFF, SkyrimVM u16 0xFFFF
	for (int str16 = 0; str16 < 2; ++str16) {
		const std::vector<std::uint8_t> bytes = { 0xFF, 0xFF, 0xFF, 0xFF };
		Papyrus p;
		Reader  r(bytes.data(), bytes.size());
		r.str16 = str16 != 0;
		bool ok = true;
		try {
			p.read(r);
		} catch (const std::exception&) {
			ok = false;
		}
		Check(ok && p.invalid && p.header == 0xFFFF && p.tail.size() == 2 && p.check().empty(), "invalid Papyrus block (FF FF FF FF) reads");
		Writer w;
		w.str16 = r.str16;
		p.write(w);
		Check(w.buf == bytes, "invalid Papyrus block writes back as it was");
		p.instances.push_back(Instance(10, 1, 1));
		bool threw = false;
		try {
			Writer w2;
			p.write(w2);
		} catch (const std::exception&) {
			threw = true;
		}
		Check(threw, "script data put into an invalid Papyrus block is refused, not lost");
	}

	// real saves
	std::vector<fs::path> saves;
	auto add = [&](const char* a_env) {
		const char* env = std::getenv(a_env);
		if (!env || !*env) {
			return;
		}
		std::string all = env;
		for (std::size_t q = 0; q <= all.size();) {
			auto e = all.find(';', q);
			if (e == std::string::npos) {
				e = all.size();
			}
			const fs::path x = all.substr(q, e - q);
			std::error_code ec;
			if (fs::is_directory(x, ec)) {
				for (const auto& f : fs::directory_iterator(x, ec)) {
					if (f.path().extension() == ".ess") {
						saves.push_back(f.path());
					}
				}
			} else if (fs::is_regular_file(x, ec)) {
				saves.push_back(x);
			}
			q = e + 1;
		}
	};
	add("RESAVERPP_SKY_SAVES");
	add("RESAVERPP_TG_SAVE");
	add("RESAVERPP_STACK_SAVES");
	std::sort(saves.begin(), saves.end());
	saves.erase(std::unique(saves.begin(), saves.end()), saves.end());
	if (saves.empty()) {
		std::printf("(suspended stacks: RESAVERPP_SKY_SAVES not set, real saves skipped)\n");
	}
	const auto tmp = fs::temp_directory_path() / "rs_unit_stacks.ess";
	auto       writeRaw = [&](const SaveFile& a_s) {
        const auto    b = a_s.serialize();
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
	};
	for (const auto& f : saves) {
		const auto tag = "stacks " + f.filename().string().substr(0, 12) + ": ";
		auto       s = SaveFile::load(f);
		if (s->isFO4() || s->isLE()) {
			continue;
		}
		auto*      p = s->papyrus();
		CheckS(ops::StrandedThreads(*p).empty() && heldOnce(*p) == std::pair<std::size_t, std::size_t>{ 0, 0 },
			tag + "the game's save holds every thread exactly once, none stranded");
		const auto threads0 = p->activeScripts.size(), stacks0 = p->suspended1.size() + p->suspended2.size(), cf0 = s->changeForms.size();
		if (stacks0 == 0) {
			continue;
		}
		// pick several: the first, the last and every 7th (some with a queued call, some without)
		std::set<ThreadID> pick;
		for (std::size_t i = 0; i < p->suspended1.size(); ++i) {
			if (i == 0 || i + 1 == p->suspended1.size() || i % 7 == 3) {
				pick.insert(p->suspended1[i].id);
			}
		}
		{
			auto old = SaveFile::load(f);
			const auto n = ops::DeleteSuspendedStacks(*old->papyrus(), pick);
			CheckS(n == pick.size() && ops::StrandedThreads(*old->papyrus()).size() == pick.size(),
				tag + "deleting only the entries strands exactly their threads (what the game cannot pause with)");
		}
		const auto r = ops::RemoveSuspendedStacks(*p, pick);
		CheckS(r.stacks == pick.size() && r.threads == pick.size() && r.messages == 0, tag + "each suspended stack goes with its one thread");
		writeRaw(*s);
		auto back = SaveFile::load(tmp);
		auto* q = back->papyrus();
		CheckS(back->problems.empty() && q && q->check().empty(), tag + "the edited save reads back with no problems");
		CheckS(q->activeScripts.size() == threads0 - pick.size() && q->suspended1.size() + q->suspended2.size() == stacks0 - pick.size() &&
				   back->changeForms.size() == cf0,
			tag + "read back: only those stacks and threads are gone");
		CheckS(ops::StrandedThreads(*q).empty() && heldOnce(*q) == std::pair<std::size_t, std::size_t>{ 0, 0 },
			tag + "read back: every thread held exactly once, nothing names a missing thread");
		bool none = true;
		for (const auto id : pick) {
			none = none && !q->findActive(id) && !ops::IsThreadSuspended(*q, id);
		}
		CheckS(none, tag + "read back: the deleted ids are nowhere");

		// the game's invalid block in this save
		auto inv = SaveFile::load(f);
		for (auto& g : inv->table3) {
			if (g.type == 1001) {
				g.papyrus.reset();
				g.raw = { 0xFF, 0xFF, 0xFF, 0xFF };
			}
		}
		writeRaw(*inv);
		std::string err;
		std::unique_ptr<SaveFile> ib;
		try {
			ib = SaveFile::load(tmp);
		} catch (const std::exception& e) {
			err = e.what();
		}
		CheckS(ib && ib->papyrus() && ib->papyrus()->invalid && ib->problems.empty() && ib->changeForms.size() == cf0,
			tag + "a save the game wrote with its Papyrus flagged invalid opens (" + err + ")");
		if (ib) {
			const auto a = ib->serialize();
			std::ifstream in(tmp, std::ios::binary);
			std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), {});
			CheckS(a == file, tag + "and writes back byte-identical");
			const auto rep = ops::Analyze(*ib);
			bool said = false;
			for (const auto& w : rep.warnings) {
				said = said || w.find("invalid") != std::string::npos;
			}
			CheckS(said, tag + "the report says the game flagged it invalid");
		}
	}
	std::error_code ec;
	fs::remove(tmp, ec);
}

// Script-call floods (core/ScriptFlood.h): grouping by script.event, the threshold, the safe clear (only calls whose thread has
// not started, each with its thread; running threads, latent waits, lock holders and threads named elsewhere stay), and real
// saves: RESAVERPP_FLOOD_SAVE = Zordrac's Save1086 (13303 + 1281 + 402 queued event calls), RESAVERPP_SKY_SAVES /
// RESAVERPP_TG_SAVE / RESAVERPP_STACK_SAVES / RESAVERPP_S140 (Skyrim) and RESAVERPP_FO4_SAVES (Fallout 4) for no false finding.
void TestScriptFloods()
{
	namespace fs = std::filesystem;
	auto CheckS = [](bool a_ok, const std::string& a_what) { Check(a_ok, a_what.c_str()); };
	auto heldOnce = [](const Papyrus& p) {
		std::map<ThreadID, int> held;
		for (const auto* list : { &p.suspended1, &p.suspended2 }) {
			for (const auto& x : *list) {
				held[x.id]++;
			}
		}
		for (const auto& m : p.functionMessages) {
			if (m.id) {
				held[*m.id]++;
			}
		}
		if (p.unk1 && p.unk2) {
			held[*p.unk2]++;
		}
		std::size_t bad = 0, dangling = 0;
		for (const auto& a : p.activeScripts) {
			bad += (held[a.id] + (a.type != 0 ? 1 : 0)) != 1;
		}
		for (const auto& [id, n] : held) {
			dangling += p.findActive(id) == nullptr;
		}
		return bad + dangling;
	};
	{
		auto  s = MakeSave();
		auto& p = *s->papyrus();
		p.instances.push_back(Instance(10, 1, 1));  // ScriptA on a ModA form
		p.instances.push_back(Instance(11, 2, 2));
		p.instanceDataOrder = { 10, 11 };
		ThreadID next = 100;
		auto queued = [&](TStr a_script, EID a_obj, bool a_started, std::int8_t a_state = 4) {
			ActiveScript a = Thread(next, a_script, Ref(a_obj));
			if (!a_started) {
				a.data->frames.clear();
			} else {
				a.data->unknown5 = 0;  // the byte after a running thread's frames
			}
			a.data->major = a_state;
			a.data->minor = 1;
			p.activeScripts.push_back(a);
			p.activeDataOrder.push_back(next);
			SuspendedStack st;
			st.id = next;
			st.flag = 1;  // has its call
			st.data = Call(a_script, Ref(a_obj));
			p.suspended1.push_back(st);
			return next++;
		};
		for (int i = 0; i < 140; ++i) {
			queued(1, 10, false);
		}
		const auto running = queued(1, 10, true, 5);  // made by a thread that runs (frames > 0)
		const auto latent = queued(1, 10, false);
		p.activeScripts.back().type = 1;
		const auto byMsg = queued(1, 10, false);  // a queued message names it too
		FunctionMessage m;
		m.flag = 1;
		m.id = byMsg;
		m.flags = 1;
		m.data = Call(1, Ref(10));
		p.functionMessages.push_back(m);
		const auto locker = queued(1, 10, false);  // holds an object's lock
		p.instances[1].data->flag = 4;
		p.instances[1].data->unknown2 = locker;
		const auto running0 = queued(1, 10, false, 0);  // state 0: not a call the resume path makes
		for (int i = 0; i < 50; ++i) {
			queued(2, 11, false);  // under the line
		}
		p.reindex();
		auto f = flood::Find(*s);
		CheckS(f.size() == 1 && f[0].script == "ScriptA" && f[0].event == "OnUpdate" && f[0].suspended == 145 && f[0].messages == 1 &&
				   f[0].notStarted == 140 && f[0].running == 1 && f[0].other == 4 && f[0].objects == 1 && f[0].targets.size() == 1 &&
				   f[0].targets[0].object == 10 && f[0].targets[0].calls == 146 && f[0].targets[0].form == 0x01000800,
			"floods: one script.event over 100 calls, split into not started / running / left for another reason");
		CheckS(flood::Find(*s, 200).empty(), "floods: nothing over a higher line");
		auto none = flood::Clear(*s, { "ScriptB.OnUpdate" });
		CheckS(none.calls == 0 && none.floods.empty() && p.activeScripts.size() == 195, "floods: a script.event under the line is not cleared");
		const auto r = flood::Clear(*s, { "scripta.onupdate" });
		CheckS(r.calls == 140 && r.threads == 140 && r.messages == 0 && r.leftRunning == 1 && r.leftOther == 4 && r.floods.size() == 1,
			"floods: only the 140 calls that never started are cleared, with their threads");
		bool kept = p.findActive(running) && p.findActive(latent) && p.findActive(byMsg) && p.findActive(locker) && p.findActive(running0);
		CheckS(kept && p.activeScripts.size() == 55 && p.suspended1.size() == 55 && p.functionMessages.size() == 1 && p.instances.size() == 2,
			"floods: running, latent, message-held, lock-holding and state-0 threads stay, and the instances");
		CheckS(ops::StrandedThreads(p).empty(), "floods: no stranded thread after the clear");
		const auto back = flood::ReadBack(*s);
		CheckS(back.ok && back.threads == 55 && back.suspended == 55 && back.stranded == 0, "floods: the cleared section reads back (" + back.error + ")");
	}

	// real saves
	auto list = [](const char* a_env, const char* a_ext) {
		std::vector<fs::path> out;
		const char*           env = std::getenv(a_env);
		if (!env || !*env) {
			return out;
		}
		std::string all = env;
		for (std::size_t q = 0; q <= all.size();) {
			auto e = all.find(';', q);
			if (e == std::string::npos) {
				e = all.size();
			}
			const fs::path  x = all.substr(q, e - q);
			std::error_code ec;
			if (fs::is_directory(x, ec)) {
				for (const auto& f : fs::recursive_directory_iterator(x, ec)) {
					if (f.path().extension() == a_ext) {
						out.push_back(f.path());
					}
				}
			} else if (fs::is_regular_file(x, ec)) {
				out.push_back(x);
			}
			q = e + 1;
		}
		return out;
	};
	std::vector<fs::path> normal;
	for (const char* e : { "RESAVERPP_SKY_SAVES", "RESAVERPP_TG_SAVE", "RESAVERPP_STACK_SAVES", "RESAVERPP_S140" }) {
		for (auto& f : list(e, ".ess")) {
			normal.push_back(f);
		}
	}
	for (auto& f : list("RESAVERPP_FO4_SAVES", ".fos")) {
		normal.push_back(f);
	}
	std::sort(normal.begin(), normal.end());
	normal.erase(std::unique(normal.begin(), normal.end()), normal.end());
	std::size_t checked = 0;
	for (const auto& f : normal) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;  // a save that does not open is another test's business
		}
		++checked;
		CheckS(flood::Find(*s).empty(), "floods " + f.filename().string().substr(0, 16) + ": a normal save has no flood");
	}
	if (normal.empty()) {
		std::printf("(floods: RESAVERPP_SKY_SAVES / RESAVERPP_FO4_SAVES not set, normal saves skipped)\n");
	}

	const char* zord = std::getenv("RESAVERPP_FLOOD_SAVE");
	if (!zord || !*zord) {
		std::printf("(floods: RESAVERPP_FLOOD_SAVE not set, Zordrac's save skipped)\n");
		return;
	}
	auto  s = SaveFile::load(zord);
	auto* p = s->papyrus();
	const auto cf0 = s->changeForms.size(), inst0 = p->instances.size();
	CheckS(p->activeScripts.size() == 15162 && p->suspended1.size() == 15151 && p->suspended2.empty() && ops::StrandedThreads(*p).empty() &&
			   heldOnce(*p) == 0,
		"floods zordrac: 15162 threads, 15151 suspended stacks, every thread held once");
	const auto f = flood::Find(*s);
	CheckS(f.size() == 3 && f[0].key() == "AX_ValQuestAliasScript.OnMagicEffectApply" && f[0].total() == 13303 && f[0].notStarted == 13303 &&
			   f[1].key() == "SM_AstridQuestAliasScript.OnMagicEffectApply" && f[1].total() == 1281 && f[1].notStarted == 1281 &&
			   f[2].key() == "AX_ValQuestAliasScript.OnHit" && f[2].total() == 402 && f[2].notStarted == 402,
		"floods zordrac: the three floods, every call not started");
	CheckS(f.size() == 3 && f[0].objects == 1 && f[0].targets[0].alias && f[0].targets[0].aliasID == 0 && f[0].targets[0].form == 0x0D093567 &&
			   f[0].targets[0].plugin == "AX ValSerano.esp" && f[1].targets[0].alias && f[1].targets[0].form == 0x20014C19 &&
			   f[1].targets[0].plugin == "SM_Astrid.esp" && f[1].runningNow == 1,
		"floods zordrac: alias 0 of Val Serano's quest and of Astrid's quest (one copy of Astrid's event is running)");
	{
		auto one = SaveFile::load(zord);
		const auto r = flood::Clear(*one, { "AX_ValQuestAliasScript.OnHit" });
		CheckS(r.calls == 402 && r.threads == 402 && one->papyrus()->activeScripts.size() == 15162 - 402 && flood::Find(*one).size() == 2,
			"floods zordrac: clearing one flood removes only its 402 calls");
	}
	const auto r = flood::Clear(*s);
	CheckS(r.calls == 14986 && r.threads == 14986 && r.messages == 0 && r.leftRunning == 0 && r.leftOther == 0 && r.floods.size() == 3,
		"floods zordrac: all floods: 14986 calls with their threads");
	const auto tmp = fs::temp_directory_path() / "rs_unit_floods.ess";
	{
		const auto    b = s->serialize();
		std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
		o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
	}
	auto  back = SaveFile::load(tmp);
	auto* q = back->papyrus();
	CheckS(back->problems.empty() && q && q->check().empty(), "floods zordrac: the cleared save reads back with no problems");
	CheckS(q->activeScripts.size() == 176 && q->suspended1.size() + q->suspended2.size() == 165 && q->functionMessages.size() == 2 &&
			   q->instances.size() == inst0 && back->changeForms.size() == cf0,
		"floods zordrac: read back: 176 threads, 165 suspended stacks, instances and change forms unchanged");
	CheckS(ops::StrandedThreads(*q).empty() && heldOnce(*q) == 0, "floods zordrac: read back: no stranded thread, every thread held once");
	CheckS(flood::Find(*back).empty(), "floods zordrac: read back: no flood left");
	std::error_code ec;
	fs::remove(tmp, ec);
	std::printf("(floods: %zu normal saves checked)\n", checked);
}

// The freeze fix (SkyrimIstari, v64 under Wine): the shortcuts must give what the slow ways gave.
// - ops::RenameFinder (all missing plugins at once) = ops::RenameCandidates (one at a time, the save read each time)
// - ScriptSources::pluginPath from the names noted while scanning = the file found on disk, any case
// - PluginDB::Input::wantedShared is read like wantedRefs
// With RESAVERPP_SKY_SAVES the rename check also runs on the real saves' plugin lists.
// Spawned actors whose base is another actor's temporary leveled base (core/GhostActors.h). Real saves:
// RESAVERPP_GHOST_SAVES="file=count[:id,id...];..." (Tony's Save88=13, Save132=2:FF003D2A,FF0040DF,
// Save140=4:FF003307,FF0033C7,FF003D2A,FF0040DF); RESAVERPP_TG_SAVE (trashgod69's a.ess: none).
void TestGhostActors()
{
	auto CheckS = [](bool a_ok, const std::string& a_what) { Check(a_ok, a_what.c_str()); };
	auto verify = [&](const std::string& a_file, std::size_t a_want, const std::vector<std::uint32_t>& a_ids) {
		auto       s = SaveFile::load(a_file);
		const auto g = ghost::Find(*s);
		const auto name = std::filesystem::path(a_file).filename().string().substr(0, 10);
		CheckS(g.size() == a_want, "ghost actors " + name + ": " + std::to_string(g.size()) + " found, " + std::to_string(a_want) + " expected");
		std::set<std::uint32_t> npcs, created;
		for (const auto& c : s->changeForms) {
			if (c.refID.type() == RefID::Type::kCreated) {
				created.insert(s->formIDOf(c.refID));
				if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::NPC_) {
					npcs.insert(s->formIDOf(c.refID));
				}
			}
		}
		bool ok = true;
		for (const auto& a : g) {
			ok = ok && (a.formID >> 24) == 0xFF && created.contains(a.formID) && (a.missing() >> 24) == 0xFF && !npcs.contains(a.missing());
			ok = ok && !ghost::Describe(*s, a).empty();
		}
		CheckS(ok, "ghost actors " + name + ": each is a created actor naming a created base with no NPC change form");
		if (!a_ids.empty()) {
			std::set<std::uint32_t> got;
			for (const auto& a : g) {
				got.insert(a.formID);
			}
			CheckS(got == std::set<std::uint32_t>(a_ids.begin(), a_ids.end()), "ghost actors " + name + ": the expected actors");
		}
		// the analysis report carries them (the Save check and the CLI report read it)
		const auto r = ops::Analyze(*s);
		bool warned = false;
		for (const auto& w : r.warnings) {
			warned = warned || w.find("name a base NPC the save does not have") != std::string::npos;
		}
		CheckS(r.ghosts.size() == g.size() && warned == !g.empty(), "ghost actors " + name + ": in ops::Analyze and its warnings");
		// every other created leveled actor names a plugin's original base (what the game saves for them)
		std::size_t leveled = 0, pluginBase = 0;
		for (const auto& c : s->changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::ACHR || c.refID.type() != RefID::Type::kCreated) {
				continue;
			}
			const auto d = cf::Decode(*s, c, {}, true);
			if (d.leveledBase) {
				++leveled;
				pluginBase += (d.leveledBase >> 24) != 0xFF;
			}
		}
		CheckS(leveled > 0 && pluginBase + g.size() >= leveled, "ghost actors " + name + ": the other leveled actors' original base is a plugin's NPC");
	};
	const char* env = std::getenv("RESAVERPP_GHOST_SAVES");
	if (!env || !*env) {
		std::printf("(ghost actors: RESAVERPP_GHOST_SAVES not set, real saves skipped)\n");
	} else {
		std::string all = env;
		std::size_t p = 0;
		while (p <= all.size()) {
			auto e = all.find(';', p);
			if (e == std::string::npos) {
				e = all.size();
			}
			const auto item = all.substr(p, e - p);
			p = e + 1;
			const auto eq = item.rfind('=');
			if (item.empty() || eq == std::string::npos) {
				continue;
			}
			const auto file = item.substr(0, eq);
			auto       rest = item.substr(eq + 1);
			std::vector<std::uint32_t> ids;
			if (const auto colon = rest.find(':'); colon != std::string::npos) {
				std::stringstream ss(rest.substr(colon + 1));
				std::string       id;
				while (std::getline(ss, id, ',')) {
					ids.push_back(static_cast<std::uint32_t>(std::stoul(id, nullptr, 16)));
				}
				rest = rest.substr(0, colon);
			}
			verify(file, static_cast<std::size_t>(std::stoul(rest)), ids);
		}
	}
	if (const char* tg = std::getenv("RESAVERPP_TG_SAVE"); tg && *tg) {
		verify(tg, 0, {});
	}
}

// Ghost actors removed in the running game (helper 15 GHOSTREMOVE): the argument, the helper's parse of it, its reply, the
// console fallback, the AI lesson and refusal.
void TestGhostRemove()
{
	const std::vector<std::uint32_t> ids{ 0xFF003D2A, 0xFF0040DF, 0xFF003307 };
	const auto arg = ghost::RemoveArgument(ids);
	Check(arg == "FF003D2A,FF0040DF,FF003307", "ghost remove: the GHOSTREMOVE argument");
	Check(proto::ParseIdList(arg) == ids, "ghost remove: the helper reads back the ids ReSaver Plus sends");
	Check(proto::ParseIdList("ff003d2a  0xFF0040DF\n[FF003307]; FF003D2A, nothex, 123456789, 0") == ids,
		"ghost remove: pasted ids (spaces, new lines, 0x, brackets) read, duplicates, junk, too long and 0 skipped");
	Check(proto::ParseIdList("").empty() && proto::ParseIdList(" , ;").empty(), "ghost remove: nothing to read");
	Check(ghost::ConsoleText({ 0xFF003D2A, 0xFF0040DF }) == "prid FF003D2A\ndisable\nmarkfordelete\nprid FF0040DF\ndisable\nmarkfordelete\n",
		"ghost remove: the console lines for doing it by hand");
	Check(ghost::ConsoleText({}).empty() && ghost::RemoveArgument({}).empty(), "ghost remove: nothing ticked, nothing to send or copy");
	const auto r = ghost::ParseRemoveReply({ "3", "FF003D2A*done", "FF0040DF*not found", "FF003307*not a created actor" });
	Check(r.size() == 3 && r[0].id == 0xFF003D2A && r[0].done() && r[1].word == "not found" && !r[1].done() &&
			  r[2].id == 0xFF003307 && r[2].word == "not a created actor",
		"ghost remove: the helper's reply, one result per id");
	Check(ghost::ParseRemoveReply({ "1", "FF003D2A*the game did not answer within 30 seconds" }).at(0).word ==
			  "the game did not answer within 30 seconds",
		"ghost remove: a time-out is reported as such");
	Check(ghost::ParseRemoveReply({}).empty() && ghost::ParseRemoveReply({ "2", "FF003D2A*done" }).empty() &&
			  ghost::ParseRemoveReply({ "1", "FF003D2Adone" }).empty() && ghost::ParseRemoveReply({ "1", "XYZ*done" }).empty() &&
			  ghost::ParseRemoveReply({ "x" }).empty(),
		"ghost remove: malformed replies give nothing");
	namespace ag = ess::aigame;
	const auto lesson = ag::Lesson(ag::Game::kSkyrimSE);
	Check(lesson.find("GHOSTREMOVE") != std::string::npos && lesson.find("can crash the game") != std::string::npos &&
			  lesson.find("prid <id>, disable, markfordelete") != std::string::npos,
		"ghost remove: the AI lesson has the command, the risk and the console fallback");
	Check(!ag::HelperRefusal("LE", "ghostremove").empty() && ag::HelperRefusal("AE", "GHOSTREMOVE").empty() &&
			  ag::HelperRefusal("SE", "GHOSTREMOVE").empty() && !ag::HelperReadOnly("GHOSTREMOVE"),
		"ghost remove: refused for the LE helper, asks for approval (not read-only)");
	// the real saves' ghosts make an argument the helper reads back whole
	if (const char* env = std::getenv("RESAVERPP_GHOST_SAVES"); env && *env) {
		std::string all = env;
		for (std::size_t p = 0; p <= all.size();) {
			auto e = all.find(';', p);
			if (e == std::string::npos) {
				e = all.size();
			}
			const auto item = all.substr(p, e - p);
			p = e + 1;
			const auto eq = item.rfind('=');
			if (eq == std::string::npos) {
				continue;
			}
			auto       sv = SaveFile::load(item.substr(0, eq));
			const auto g = ghost::Find(*sv);
			std::vector<std::uint32_t> want;
			for (const auto& a : g) {
				want.push_back(a.formID);
			}
			const auto text = ghost::ConsoleText(want);
			Check(proto::ParseIdList(ghost::RemoveArgument(want)) == want &&
					  static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) == 3 * want.size(),
				("ghost remove: " + std::filesystem::path(item.substr(0, eq)).filename().string().substr(0, 10) + " ids round trip").c_str());
		}
	}
}

void TestFreezeFix()
{
	auto check = [](const SaveFile& a_save, const std::vector<std::string>& a_installed, const std::vector<std::string>& a_missing) {
		const ops::RenameFinder f(a_save, a_installed);
		bool same = true;
		for (const auto& m : a_missing) {
			same = same && f.find(m) == ops::RenameCandidates(a_save, m, a_installed);
		}
		return same;
	};
	{
		auto s = MakeSave();
		s->plugins.lite = { "Light.esl" };
		const std::vector<std::string> installed = { "ModA.esl", "MODC.esl", "ModC.esm", "modc.esp", "Other.esp", "ModB.esp", "Light.esp", "NoExt", "ModC.txt" };
		Check(check(*s, installed, { "ModC.esp", "ModA.esp", "Light.esl", "Gone.esp", "modc.ESP", "ModC" }), "freeze: RenameFinder = RenameCandidates (synthetic)");
		const ops::RenameFinder f(*s, installed);
		const auto              r = f.find("ModC.esp");
		Check(r == std::vector<std::string>({ "MODC.esl", "ModC.esm" }), "freeze: RenameFinder keeps the installed list's order and leaves out the same name");
	}
	if (const char* env = std::getenv("RESAVERPP_SKY_SAVES")) {
		std::size_t n = 0;
		std::stringstream ss(env);
		for (std::string dir; std::getline(ss, dir, ';');) {
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (e.path().extension() != ".ess") {
					continue;
				}
				try {
					const auto s = SaveFile::load(e.path());
					// installed: every other plugin renamed to each extension; missing: every plugin of the save
					std::vector<std::string> installed, missing;
					std::size_t              k = 0;
					for (const auto* list : { &s->plugins.full, &s->plugins.lite }) {
						for (const auto& p : *list) {
							missing.push_back(p);
							if (k++ % 2) {
								const auto stem = p.substr(0, p.find_last_of('.'));
								installed.push_back(stem + ".esl");
								installed.push_back(stem + ".ESM");
								installed.push_back(p);
							}
						}
					}
					Check(check(*s, installed, missing), "freeze: RenameFinder = RenameCandidates (real save)");
					++n;
				} catch (const std::exception&) {
				}
			}
		}
		std::printf("(freeze: rename check on %zu real saves)\n", n);
	}
	{
		// pluginPath: a Data folder with plugins in mixed case
		const auto dir = std::filesystem::temp_directory_path() / "rs_freeze_data";
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
		std::filesystem::create_directories(dir / "Scripts", ec);
		for (const char* n : { "Skyrim.esm", "MyMod.ESP", "lower.esl" }) {
			std::ofstream(dir / n) << "x";
		}
		ScriptSources src;
		src.scan(dir, nullptr, {}, {}, nullptr);
		auto name = [&](const char* a) {
			const auto p = src.pluginPath(a);
			return p ? p->filename().string() : std::string("(none)");
		};
		Check(name("mymod.esp") == "MyMod.ESP" && name("MYMOD.esp") == "MyMod.ESP" && name("Skyrim.esm") == "Skyrim.esm" && name("LOWER.ESL") == "lower.esl",
			"freeze: pluginPath gives the file's own name for any spelling");
		Check(name("gone.esp") == "(none)", "freeze: pluginPath of a plugin that is not there");
		Check(src.pluginPath("mymod.esp")->parent_path() == dir, "freeze: pluginPath's folder");
		std::filesystem::remove_all(dir, ec);
	}
	{
		PluginDB::Input in;
		in.wantedRefs = { 1, 2 };
		Check(in.wanted().contains(2), "freeze: Input::wanted() is wantedRefs");
		in.wantedShared = std::make_shared<const std::unordered_set<std::uint32_t>>(std::unordered_set<std::uint32_t>{ 3 });
		Check(in.wanted().contains(3) && !in.wanted().contains(2), "freeze: Input::wanted() is wantedShared when set");
	}
}

// Journal leftovers (SkyrimIstari, 9 Nov 2026: "quests not closed out properly" pointed at completed and repeating quests).
// The rule follows SkyrimSE.exe 1.6.1170's Journal (quest list 0x989f90, entry 0x98b270): an entry is shown when its state is
// 1/3/5 (Miscellaneous, type 6: 1 only) and is under Completed when the quest is completed or the entry is of an earlier run.
// RESAVERPP_IST2_SAVE = his Save103 (2997 journal entries; before the fix 1528 shown objectives of completed or stopped quests
// were counted).
// A change form whose zlib data does not unpack to its length2 (InfinityA1's Skyrim LE save, v66/v67: "zlib decompression
// failed" every frame from quests::ReadJournal on the player's record). Such a form must be skipped by every reader, reported
// (cf::DamagedForms, the analysis), never written from a body that could not be read, and saved back byte for byte.
// Synthetic Skyrim LE + Fallout 4 saves always; real saves with RESAVERPP_DAMAGED_SAVES="file;dir" (any game: the player's
// record and one other compressed form are damaged in memory, every check runs, the save is written and read back).
namespace
{
	void DamagedBattery(SaveFile& s, const std::string& a_tag)
	{
		auto run = [&](const char* a_what, const std::function<void()>& a_f) {
			try {
				a_f();
				Check(true, a_what);
			} catch (const std::exception& e) {
				std::printf("  %s: %s threw: %s\n", a_tag.c_str(), a_what, e.what());
				Check(false, (std::string("damaged form: ") + a_what + " does not throw").c_str());
			}
		};
		run("ops::Analyze", [&] { ops::Analyze(s); });
		run("quests::ReadStates", [&] { quests::ReadStates(s); });
		run("quests::ReadJournal", [&] { quests::ReadJournal(s); });
		run("quests::ReadQuestLog", [&] { quests::ReadQuestLog(s); });
		run("cf::FindChargedItems", [&] { cf::FindChargedItems(s); });
		run("cf::CountStuckPathIdles", [&] {
			for (const auto& c : s.changeForms) {
				cf::CountStuckPathIdles(s, c);
			}
		});
		run("cf::ListActors", [&] { cf::ListActors(s); });
		run("cf::ListInventories", [&] { cf::ListInventories(s); });
		run("cf::ReadPlayer", [&] { cf::ReadPlayer(s); });
		run("cf::ReadPlayerActiveEffects", [&] {
			std::string err;
			cf::ReadPlayerActiveEffects(s, err);
		});
		run("cf::FindStuckPlayerMovement", [&] { cf::FindStuckPlayerMovement(s); });
		run("bloat::Build", [&] { bloat::Build(s); });
		run("chfx::Build", [&] { chfx::Build(s, {}); });
		run("ghost::Find", [&] { ghost::Find(s); });
		run("ops::LiveEffectInstances", [&] {
			std::set<EID> all;
			if (const auto* p = s.papyrus()) {
				for (const auto& i : p->instances) {
					all.insert(i.id);
				}
			}
			ops::LiveEffectInstances(s, all);
		});
		run("cf::Decode (every form)", [&] {
			for (const auto& c : s.changeForms) {
				cf::Decode(s, c);
			}
		});
		if (s.isFO4()) {
			run("fo4world::List", [&] { fo4world::List(s); });
			run("fo4ws::List / Check", [&] { fo4ws::Check(s); });
			run("fo4comp::Read", [&] { fo4comp::Read(s); });
			run("fo4cf::DecodeRefr / DecodeActor / DecodeNpc", [&] {
				for (const auto& c : s.changeForms) {
					fo4cf::DecodeRefr(s, c);
					if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR) {
						fo4cf::DecodeActor(s, c);
					}
					if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::NPC_) {
						fo4cf::DecodeNpc(s, c);
					}
				}
			});
		} else {
			run("skyf::ReadFollowers", [&] { skyf::ReadFollowers(s); });
			run("skyf::ReadBeastForms", [&] { skyf::ReadBeastForms(s); });
			// fixes over every form: skip the damaged ones, leave them as they are
			run("cf::FixCharges / FixStuckPathIdles", [&] {
				cf::FixCharges(s, 100, {});
				cf::FixStuckPathIdles(s);
			});
		}
	}

	ChangeForm ZForm(std::uint32_t a_refData, ChangeFormType a_type, const std::vector<std::uint8_t>& a_body, int a_damage)
	{
		ChangeForm c;
		c.refID.data = a_refData;
		c.typeField = static_cast<std::uint8_t>(static_cast<std::uint8_t>(a_type) | (1 << 6));  // u16 lengths
		c.data = CompressZlib(a_body);
		c.length1 = static_cast<std::uint32_t>(c.data.size());
		c.length2 = static_cast<std::uint32_t>(a_body.size());
		if (a_damage == 1) {
			++c.length2;  // inflates to fewer bytes than the save says
		} else if (a_damage == 2) {
			c.data[c.data.size() / 2] ^= 0x5A;  // bad deflate data or checksum
		} else if (a_damage == 3) {
			c.data.erase(c.data.end() - 6, c.data.end());  // cut short
			c.length1 = static_cast<std::uint32_t>(c.data.size());
		}
		return c;
	}
}

void TestDamagedChangeForms()
{
	for (const bool fo4 : { false, true }) {
		auto s = MakeSave();
		s->header.magic = fo4 ? "FO4_SAVEGAME" : "TESV_SAVEGAME";
		s->header.version = fo4 ? 15 : 9;
		std::vector<std::uint8_t> body(300);
		for (std::size_t i = 0; i < body.size(); ++i) {
			body[i] = static_cast<std::uint8_t>(i * 7);
		}
		s->changeForms.push_back(ZForm((1u << 22) | 0x14, ChangeFormType::ACHR, body, 1));  // the player
		s->changeForms.push_back(ZForm((1u << 22) | 0x07, ChangeFormType::NPC_, body, 2));  // the player's base
		s->changeForms.push_back(ZForm((1u << 22) | 0x1234, ChangeFormType::REFR, body, 3));
		s->changeForms.push_back(ZForm((1u << 22) | 0x1235, ChangeFormType::REFR, body, 0));  // a good one
		s->reindexChangeForms();
		const std::string tag = fo4 ? "synthetic FO4" : "synthetic LE";
		const auto        dmg = cf::DamagedForms(*s);
		Check(dmg.size() == 3 && dmg[0].formID == 0x14 && dmg[1].formID == 0x07 && dmg[2].formID == 0x1234,
			"damaged forms: the three damaged change forms are listed, the good one is not");
		Check(dmg.size() == 3 && dmg[0].why.find("inflates to 300 bytes, not the 301") != std::string::npos &&
				  dmg[2].why.find("cut short") != std::string::npos,
			"damaged forms: says what is wrong");
		std::vector<std::uint8_t> b;
		Check(!cf::TryBody(s->changeForms[0], b) && b.empty() && cf::TryBody(s->changeForms[3], b) && b == body,
			"damaged forms: TryBody false for a damaged form, the body for a good one");
		bool threw = false;
		try {
			cf::Body(s->changeForms[0]);
		} catch (const cf::BodyError&) {
			threw = true;
		}
		Check(threw, "damaged forms: Body throws BodyError (an edit never writes a body it could not read)");
		if (!fo4) {
			const auto d = cf::Decode(*s, s->changeForms[0]);
			Check(d.damaged && !d.complete && d.error.find("zlib data damaged") != std::string::npos, "damaged forms: Decode says why");
			const auto r = ops::Analyze(*s);
			Check(r.damaged.size() == 3 && !r.warnings.empty() && r.warnings[0].find("3 change form(s) could not be decompressed: 00000014 (ACHR") == 0,
				"damaged forms: the analysis reports them");
			const auto again = ops::Analyze(*s, &r.damaged);  // after an edit: only the known ones are checked again
			Check(again.damaged.size() == 3 && again.damaged[1].formID == 0x07, "damaged forms: the analysis after an edit keeps them");
			const auto before = s->changeForms[0].data;
			Check(cf::MoveActorToPlayer(*s, s->changeForms[3]).find("could not be read") != std::string::npos ||
					  s->changeForms[0].data == before,
				"damaged forms: an edit needing the player's damaged record refuses, the record is untouched");
		} else {
			Check(fo4cf::DecodeRefr(*s, s->changeForms[0]).error.find("zlib data damaged") != std::string::npos,
				"damaged forms: Fallout 4 DecodeRefr says why");
			Check(fo4cf::SetPlayerCaps(*s, 5).find("could not be read") != std::string::npos, "damaged forms: Fallout 4 caps edit refuses");
		}
		DamagedBattery(*s, tag);
		if (!fo4) {
			// the Save check's "Reset damaged forms": every damaged form but the player's (00000014 / 00000007) is deleted
			std::set<std::uint32_t> reset;
			for (const auto& d : cf::DamagedForms(*s)) {
				if (d.formID != 0x14 && d.formID != 0x7 && (d.formID >> 24) != 0xFF) {
					reset.insert(s->changeForms[d.index].refID.data);
				}
			}
			const auto n = ops::DeleteChangeForms(*s, reset);
			const auto left = cf::DamagedForms(*s);
			Check(n == 1 && s->changeForms.size() == 3 && left.size() == 2 && left[0].formID == 0x14 && left[1].formID == 0x07,
				"damaged forms: resetting them deletes the damaged non-player form only; the player's two stay as they were");
		}
	}

	// real saves
	std::vector<std::filesystem::path> files;
	if (const char* env = std::getenv("RESAVERPP_DAMAGED_SAVES")) {
		std::string all = env;
		for (std::size_t p = 0; p <= all.size();) {
			auto e = std::min(all.find(';', p), all.size());
			const std::filesystem::path f = all.substr(p, e - p);
			std::error_code ec;
			if (std::filesystem::is_directory(f, ec)) {
				for (const auto& x : std::filesystem::directory_iterator(f, ec)) {
					if (x.path().extension() == ".ess" || x.path().extension() == ".fos") {
						files.push_back(x.path());
					}
				}
			} else if (!f.empty()) {
				files.push_back(f);
			}
			p = e + 1;
		}
	}
	if (files.empty()) {
		std::printf("(damaged forms: RESAVERPP_DAMAGED_SAVES not set, real saves skipped)\n");
	}
	std::sort(files.begin(), files.end());
	for (const auto& f : files) {
		auto s = SaveFile::load(f);
		const auto already = cf::DamagedForms(*s).size();
		// damage the player's record (length2 one too big) and the middle compressed form (one byte of its stream)
		std::size_t player = SIZE_MAX, other = SIZE_MAX, compressed = 0;
		for (std::size_t i = 0; i < s->changeForms.size(); ++i) {
			const auto& c = s->changeForms[i];
			if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR && s->formIDOf(c.refID) == 0x14) {
				player = i;
			}
			compressed += c.length2 > 0;
		}
		for (std::size_t i = 0, k = 0; i < s->changeForms.size(); ++i) {
			if (s->changeForms[i].length2 > 0 && i != player && k++ == compressed / 2) {
				other = i;
			}
		}
		if (other == SIZE_MAX) {
			std::printf("(damaged forms: %s has no zlib-compressed change form, skipped)\n", f.filename().string().c_str());
			continue;
		}
		// (Skyrim SE / AE compresses the whole body with LZ4 and few forms with zlib: the player's may not be one)
		const bool playerToo = player != SIZE_MAX && s->changeForms[player].length2 > 0;
		if (playerToo) {
			++s->changeForms[player].length2;  // fits the length width: it is only checked against the stream
		}
		auto& oc = s->changeForms[other];
		oc.data[oc.data.size() / 2] ^= 0x5A;
		const auto dmg = cf::DamagedForms(*s);
		const std::size_t made = playerToo ? 2 : 1;
		Check(dmg.size() == already + made, "damaged forms (real save): the damaged forms found");
		DamagedBattery(*s, f.filename().string());
		// written and read back: every change form byte for byte as in memory (the damaged ones too)
		const auto expect = s->changeForms;
		const auto tmp = std::filesystem::temp_directory_path() / ("rs_damaged" + f.extension().string());
		bool       same = false;
		try {
			s->save(tmp);
			auto back = SaveFile::load(tmp);
			same = back->changeForms.size() == expect.size();
			for (std::size_t i = 0; same && i < expect.size(); ++i) {
				const auto& a = expect[i];
				const auto& c = back->changeForms[i];
				same = a.refID.data == c.refID.data && a.changeFlags == c.changeFlags && a.typeField == c.typeField && a.version == c.version &&
				       a.length1 == c.length1 && a.length2 == c.length2 && a.data == c.data;
			}
			same = same && cf::DamagedForms(*back).size() == already + made;
		} catch (const std::exception& e) {
			std::printf("  %s: write / read back: %s\n", f.filename().string().c_str(), e.what());
		}
		std::filesystem::remove(tmp);
		std::printf("damaged forms: %s: %zu compressed, damaged %s%08X, %zu found, written back %s\n", f.filename().string().c_str(), compressed,
			playerToo ? "00000014 (player) and " : "", s->formIDOf(oc.refID), dmg.size(), same ? "byte for byte" : "CHANGED");
		Check(same, "damaged forms (real save): written back byte for byte and found again");
	}
}

#include "sf_test.inc"

void TestJournalLeftovers()
{
	using ess::quests::QuestState;
	using ess::quests::QuestDef;
	auto mk = [](std::uint32_t a_id, std::uint16_t a_flags, std::uint32_t a_cur, std::vector<ess::quests::ObjectiveState> a_journal) {
		QuestState q;
		q.formID = a_id;
		q.hasFlags = true;
		q.flags = a_flags;
		q.currentInstance = a_cur;
		q.journalKnown = true;
		q.journal = std::move(a_journal);
		return q;
	};
	const auto running = mk(1, 1, 0, { { 10, 1, 0 } });
	const auto completed = mk(2, 2, 0, { { 10, 3, 0 }, { 20, 1, 0 } });
	const auto stopped = mk(3, 0, 0, { { 10, 1, 0 }, { 20, 3, 0 }, { 30, 2, 0 } });
	const auto earlier = mk(4, 0, 2, { { 10, 1, 1 }, { 20, 3, 1 } });
	const auto mixed = mk(5, 0, 2, { { 10, 3, 1 }, { 10, 1, 2 } });
	const auto misc = mk(6, 0, 0, { { 10, 3, 0 }, { 20, 1, 0 } });
	Check(running.leftoverObjectives() == 0, "journal leftovers: a running quest has none");
	Check(completed.leftoverObjectives() == 0, "journal leftovers: a completed quest's entries are the Completed list");
	Check(stopped.leftoverObjectives() == 2, "journal leftovers: a stopped quest's shown entries of its current run (state 2 not shown)");
	Check(earlier.leftoverObjectives() == 0 && earlier.earlierRunsOnly(), "journal leftovers: entries of an earlier run are not counted");
	Check(mixed.leftoverObjectives() == 1 && !mixed.earlierRunsOnly(), "journal leftovers: only the current run's entry counts");
	Check(misc.leftoverObjectives(6) == 1 && misc.leftoverObjectives() == 2, "journal leftovers: a Miscellaneous quest shows only state 1");
	QuestDef sm, rep, plain;
	sm.event = 'C' | ('L' << 8) | ('O' << 16) | (static_cast<std::uint32_t>('C') << 24);
	rep.flags = 0x8;
	std::string why;
	Check(sm.eventText() == "CLOC" && ess::quests::QuestRepeats(&stopped, &sm, &why) && why.find("Story Manager (CLOC)") != std::string::npos,
		"journal leftovers: a Story Manager quest repeats");
	Check(ess::quests::QuestRepeats(&stopped, &rep, &why) && why == "allows repeated stages", "journal leftovers: repeated stages");
	Check(!ess::quests::QuestRepeats(&stopped, &plain, &why) && why.empty(), "journal leftovers: a plain stopped quest does not repeat");
	Check(ess::quests::QuestRepeats(&mixed, nullptr, &why), "journal leftovers: an earlier run in the journal means it repeats");
	{
		const std::vector<QuestState> all = { running, completed, stopped, earlier, mixed, misc };
		const auto lo = ess::quests::JournalLeftovers(all, [&](std::uint32_t a_id) -> const QuestDef* { return a_id == 3 ? &sm : nullptr; });
		std::set<std::uint32_t> ids;
		for (const auto& l : lo) {
			ids.insert(l.quest);
		}
		Check(ids == std::set<std::uint32_t>({ 3, 5, 6 }) && lo[0].repeats && lo[1].repeats && !lo[2].repeats,
			"journal leftovers: listed with their repeat reason");
	}
	const char* env = std::getenv("RESAVERPP_IST2_SAVE");
	if (!env || !std::filesystem::exists(env)) {
		std::printf("(journal leftovers: RESAVERPP_IST2_SAVE not set, SkyrimIstari's Save103 skipped)\n");
		return;
	}
	auto       s = SaveFile::load(env);
	const auto st = ess::quests::ReadStates(*s);
	std::size_t entries = 0, oldCount = 0, oldQuests = 0, instances = 0;
	for (const auto& q : st) {
		entries += q.journal.size();
		const auto n = (q.completed() || !q.running()) ? q.shownObjectives() : 0;
		oldCount += n;
		oldQuests += n ? 1 : 0;
		instances += q.currentInstance ? 1 : 0;
	}
	// the instance id read from CHANGE_QUEST_INSTANCES fits the journal: no entry is of a later run than the quest's current one
	// (2823 entries of quests with a saved id on Save103), and quests without a saved id (the game keeps 0) list run 0 only
	std::size_t withId = 0, later = 0, noId = 0, noIdOther = 0;
	for (const auto& q : st) {
		for (const auto& o : q.journal) {
			if (q.currentInstance) {
				++withId;
				later += o.instance > q.currentInstance ? 1 : 0;
			} else {
				++noId;
				noIdOther += o.instance ? 1 : 0;
			}
		}
	}
	std::printf("journal leftovers (Save103): %zu entries of quests with a saved instance id (%zu of a later run), %zu without (%zu not of run 0)\n", withId,
		later, noId, noIdOther);
	Check(withId > 2000 && later == 0 && noIdOther == 0, "journal leftovers (Save103): the saved instance id is each quest's latest run");
	const auto lo = ess::quests::JournalLeftovers(st, nullptr);
	std::size_t loObj = 0, repeating = 0;
	bool date = false, drink = false;
	for (const auto& l : lo) {
		loObj += l.objectives;
		repeating += l.repeats ? 1 : 0;
		date = date || l.quest == 0x2FC73A4A;   // SDA_SeranaRomance11 "A Date at the Bannered Mare" (completed)
		drink = drink || l.quest == 0x2F88E922;  // SDA_DrinkingBuddiesTriggerNew (Story Manager, CLOC)
	}
	std::printf("journal leftovers (Save103): %zu entries; before: %zu objectives in %zu quests; now: %zu objectives in %zu quests (%zu repeat by "
				"an earlier run); %zu quests with a saved instance id\n",
		entries, oldCount, oldQuests, loObj, lo.size(), repeating, instances);
	Check(entries == 2997, "journal leftovers (Save103): 2997 journal entries");
	Check(oldCount == 1528, "journal leftovers (Save103): the old rule counted 1528 objectives");
	Check(loObj == 89 && lo.size() == 48, "journal leftovers (Save103): 89 objectives of 48 stopped quests are shown as in progress");
	Check(!date && !drink, "journal leftovers (Save103): the completed Bannered Mare date and the radiant drinking quest are not counted");
}

int main()
{
	TestFloatLiteral();
	TestCreationClub();
	TestCheck();
	TestPurgeKeepsThreadsConsistent();
	TestRemoveKeepsSharedArrays();
	TestPapyrusRoundTrip();
	TestEditedName();
	TestTransplant();
	TestSaveArchive();
	TestPluginsTxtFormats();
	TestPluginProperty();
	TestMergedPatch();
	TestMergedPatchLE();
	TestMergedPatchNpcLinks();
	TestMergedPatchFO4();
	TestMergedPatchFO4Types();
	TestMergedPatchFO4Real();
	TestCrashLog();
	TestCrashLogFO4();
	TestQuestsFO4();
	TestSkyrimLogs();
	TestFo4Logs();
	TestAiGame();
	TestBa2();
	TestFo4ChangeForms();
	TestTransplantFO4();
	TestFo4HavokFormLists();
	TestFo4Workshops();
	TestFo4ActorPackages();
	TestFo4Player();
	TestFo4Combat();
	TestSkyFollowers();
	TestFo4WorldItems();
	TestFo4CellReset();
	TestFo4ActiveEffects();
	TestCharEffects();
	TestLiveEffectInstances();
	TestSuspendedStacks();
	TestScriptFloods();
	TestFreezeFix();
	TestGhostActors();
	TestGhostRemove();
	TestJournalLeftovers();
	TestDamagedChangeForms();
	TestStarfield();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
