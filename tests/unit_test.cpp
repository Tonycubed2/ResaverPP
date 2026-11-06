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
#include "core/Pex.h"
#include "core/Save.h"
#include "core/SaveArchive.h"
#include "core/LoadOrder.h"
#include "core/PluginEdit.h"
#include "core/PluginDB.h"
#include "core/Quests.h"
#include "core/ChangeFormData.h"
#include "core/Transplant.h"
#include "core/TransplantFO4.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

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
		// an NPC with Fallout 4 parts: read only for COED owners, never merged
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
		Check(!npcMerged && r.merged.size() == 4, "merge FO4: LVLI, CONT, FLST, MISC merged; NPCs are not merged for Fallout 4");
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
			std::size_t lists = 0;
			for (const auto& r : pf.records) {
				if ((r.formID >> 24) != own || (r.flags & 0x20)) continue;  // overrides of a master (not loaded here), deleted ones
				merge::Record ra = r, rb = r;
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
				}
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
						if (sb.data.size() == 4 && (!std::memcmp(sb.sig, "FULL", 4) || (!std::memcmp(rec.sig, "LVLI", 4) && !std::memcmp(sb.sig, "ONAM", 4)))) {
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
			for (const auto& sk : r.skipped) std::printf("  merge FO4 real %s: not merged: %s\n", name.c_str(), sk.c_str());
			std::printf("  merge FO4 real %s: %zu records of the merged types, %zu with a list, %zu conflicts, %zu merged, %zu not merged\n",
				name.c_str(), a2.size(), lists, r.conflicts, r.merged.size(), r.skipped.size());
			Check(r.ok && r.fo4 && r.skipped.empty() && r.conflicts == a2.size() && r.merged.size() == lists,
				"merge FO4 real: every record of a real Fallout 4 plugin read with the FO4 layouts, the list ones merged");
			save("Patch.esp", r.file);
			const auto back = merge::ReadPlugin(dir / "Patch.esp");
			bool good = back.records.size() == lists;
			if (!good) {
				std::printf("  merge FO4 real %s: the patch has %zu records, %zu expected\n", name.c_str(), back.records.size(), lists);
			}
			std::size_t bad = 0;
			for (const auto& rec : back.records) {
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
		const auto d = tf4::Diff(b, a, *s8);  // good Save8, old Save7
		Check(d.size() == 1 && d[0].kind == tf4::Kind::kItem && d[0].via == tf4::Via::kGame && d[0].goodFormID == 0x00024F55 && d[0].count == 9 &&
				  d[0].available && d[0].tick,
			"fo4 transplant: Save7 -> Save8 lists the 9 Pipe Pistols (00024F55), to be added in the game");
		const auto rec = d.empty() ? std::vector<ess::transplant::Record>{} : tf4::GameRecords(d[0], d[0].goodFormID, true);
		Check(rec.size() == 1 && rec[0] == ess::transplant::Record{ "additem", "00024F55", "9" }, "fo4 transplant: Save7 -> Save8 helper record");
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
		// nearly every NPC whose reference part reads must read to the end of its body now (a few actors still stop
		// in Havok objects or the 0x87c540 block)
		Check(refrRead > 0 && complete * 100 >= refrRead * 97, ("fo4 actors: 97% of the NPCs read completely in " + name + " (" +
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
		std::size_t removed = 0, refusedPlayer = 0;
		for (auto& c : s->changeForms) {
			if (c.typeCode() != 1) continue;
			const auto r = ess::fo4cf::DecodeRefr(*s, c);
			const auto body = ess::cf::Body(c);
			std::size_t n = 0;
			const auto  e = ess::cf::RemoveInventoryItems(*s, c, items, &n);
			if (s->formIDOf(c.refID) == 0x14) {
				refusedPlayer += !e.empty() || n == 0;
				continue;
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
		Check(refusedPlayer == 1, ("fo4 actors: the player is left alone (its own part is not decoded) in " + name).c_str());
		++tested;
	}
	std::error_code ec;
	fs::remove(tmp, ec);
	std::printf("(fo4 actors: %zu real saves)\n", tested);
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
	TestFo4ActorPackages();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
