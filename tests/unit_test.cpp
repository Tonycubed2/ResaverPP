// ReSaver++ unit and regression tests that need no save files: each builds the data it needs in memory.
// Run by CTest (rs_unit). Exit code 0 = all passed.
#include "core/ModRemove.h"
#include "core/Ops.h"
#include "core/Pex.h"
#include "core/Save.h"
#include "core/Transplant.h"

#include <filesystem>
#include <fstream>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
		std::filesystem::remove(good);
		std::filesystem::remove(bad);
	}

}

int main()
{
	TestFloatLiteral();
	TestCreationClub();
	TestCheck();
	TestPurgeKeepsThreadsConsistent();
	TestRemoveKeepsSharedArrays();
	TestPapyrusRoundTrip();
	TestTransplant();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
