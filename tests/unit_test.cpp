// ReSaver++ unit and regression tests that need no save files: each builds the data it needs in memory.
// Run by CTest (rs_unit). Exit code 0 = all passed.
#include "core/CrashLog.h"
#include "core/MergedPatch.h"
#include "core/ModRemove.h"
#include "core/Ops.h"
#include "core/Pex.h"
#include "core/Save.h"
#include "core/SaveArchive.h"
#include "core/LoadOrder.h"
#include "core/PluginEdit.h"
#include "core/Transplant.h"

#include <filesystem>
#include <fstream>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
	TestCrashLog();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
