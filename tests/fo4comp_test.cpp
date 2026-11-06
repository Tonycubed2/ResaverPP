// Fallout 4 companions (core/CompanionsFO4) and the stopped-stage quest check (core/QuestChecksFO4).
// Synthetic checks always run. With RESAVERPP_FO4_SAVES="dir;dir" every .fos under those folders is read too (the follower
// quest's slots, the companions, an affinity edit written and read back); with RESAVERPP_FO4_DATA=<Fallout 4 Data folder>
// the quest check runs on those saves with the game's plugins and scripts (it must check lines and find nothing on them).
#include "core/ChangeFormData.h"
#include "core/ChangeFormFO4.h"
#include "core/CompanionsFO4.h"
#include "core/PluginDB.h"
#include "core/QuestChecksFO4.h"
#include "core/Save.h"
#include "core/ScriptSources.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ess;

static int g_pass = 0, g_fail = 0;
static void Check(bool a_ok, const std::string& a_what)
{
	if (a_ok) {
		++g_pass;
	} else {
		++g_fail;
		std::printf("FAIL: %s\n", a_what.c_str());
	}
}

namespace
{
	pexfo4::Instr Call(const char* a_method, std::vector<std::string> a_args)
	{
		pexfo4::Instr i;
		i.op = "callmethod";
		i.args = { a_method, "self", "::temp1" };
		i.extra = std::move(a_args);
		return i;
	}
	pexfo4::Instr Op(const char* a_op, std::vector<std::string> a_args)
	{
		pexfo4::Instr i;
		i.op = a_op;
		i.args = std::move(a_args);
		return i;
	}

	// a minimal Fallout 4 .pex: one object, one state, the given functions (callmethod with int arguments only)
	std::vector<std::uint8_t> MakePex(const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::vector<int>>>>>& a_funcs)
	{
		std::vector<std::string> strings = { "QF_Test_01000800", "Quest", "", "None", "self", "::temp1" };
		auto                     idx = [&](const std::string& s) -> std::uint16_t {
            auto it = std::find(strings.begin(), strings.end(), s);
            if (it != strings.end()) {
                return static_cast<std::uint16_t>(it - strings.begin());
            }
            strings.push_back(s);
            return static_cast<std::uint16_t>(strings.size() - 1);
		};
		for (const auto& [fn, body] : a_funcs) {
			idx(fn);
			for (const auto& [m, a] : body) {
				idx(m);
			}
		}
		std::vector<std::uint8_t> o;
		auto u8 = [&](std::uint8_t v) { o.push_back(v); };
		auto u16 = [&](std::uint16_t v) { o.push_back(v & 0xff); o.push_back(v >> 8); };
		auto u32 = [&](std::uint32_t v) { for (int k = 0; k < 4; ++k) o.push_back((v >> (8 * k)) & 0xff); };
		auto ws = [&](const std::string& s) { u16(static_cast<std::uint16_t>(s.size())); o.insert(o.end(), s.begin(), s.end()); };
		u32(0xFA57C0DE);
		u8(3);
		u8(9);
		u16(2);
		for (int k = 0; k < 8; ++k) u8(0);
		ws("test.psc");
		ws("user");
		ws("pc");
		u16(static_cast<std::uint16_t>(strings.size()));
		for (const auto& s : strings) ws(s);
		u8(0);    // no debug info
		u16(0);   // user flags
		u16(1);   // objects
		u16(idx("QF_Test_01000800"));
		u32(0);   // size
		u16(idx("Quest"));
		u16(idx(""));
		u8(0);
		u32(0);
		u16(idx(""));
		u16(0);  // structs
		u16(0);  // variables
		u16(0);  // properties
		u16(1);  // states
		u16(idx(""));
		u16(static_cast<std::uint16_t>(a_funcs.size()));
		for (const auto& [fn, body] : a_funcs) {
			u16(idx(fn));
			u16(idx("None"));  // return type
			u16(idx(""));      // doc
			u32(0);
			u8(0);
			u16(0);  // params
			u16(0);  // locals
			u16(static_cast<std::uint16_t>(body.size()));
			for (const auto& [m, a] : body) {
				if (m == "jmp") {
					u8(20);
					u8(3);
					u32(1);
					continue;
				}
				u8(23);  // callmethod name, self, result, count, args
				u8(1);
				u16(idx(m));
				u8(1);
				u16(idx("self"));
				u8(1);
				u16(idx("::temp1"));
				u8(3);
				u32(static_cast<std::uint32_t>(a.size()));
				for (int v : a) {
					u8(3);
					u32(static_cast<std::uint32_t>(v));
				}
			}
		}
		return o;
	}

	std::vector<fs::path> SavesFrom(const char* a_env)
	{
		std::vector<fs::path> files;
		const char*           env = std::getenv(a_env);
		if (!env || !*env) {
			return files;
		}
		std::string all = env;
		for (std::size_t p = 0; p <= all.size();) {
			auto e = all.find(';', p);
			if (e == std::string::npos) e = all.size();
			const fs::path  dir = all.substr(p, e - p);
			std::error_code ec;
			if (!dir.empty() && fs::is_directory(dir, ec)) {
				for (const auto& f : fs::recursive_directory_iterator(dir, ec)) {
					if (f.path().extension() == ".fos") files.push_back(f.path());
				}
			}
			p = e + 1;
		}
		std::sort(files.begin(), files.end());
		return files;
	}
}

static void TestQuestCheckSynthetic()
{
	quests::QuestDef d;
	d.edid = "TestQuest";
	d.fragmentScript = "QF_Test_01000800";
	for (std::uint16_t s : { 10, 20, 30, 40 }) {
		quests::Stage st;
		st.index = s;
		d.stages.push_back(st);
	}
	for (std::uint16_t o : { 5, 6, 7 }) {
		quests::Objective ob;
		ob.index = o;
		d.objectives.push_back(ob);
	}
	d.fragments.push_back({ 10, 0, "QF_Test_01000800", "Fragment_Stage_0010_Item_00" });
	d.fragments.push_back({ 30, 0, "QF_Test_01000800", "Fragment_Stage_0030_Item_00" });
	quests::QuestState q;
	q.formID = 0x01000800;
	q.hasFlags = true;
	q.flags = 1;
	q.decoded = true;
	q.stages = { { 10, true }, { 20, false }, { 30, false } };

	pexfo4::Script s;
	s.name = "QF_Test_01000800";
	s.functions["fragment_stage_0010_item_00"] = { Call("SetObjectiveDisplayed", { "5" }), Call("SetStage", { "20" }),
		Op("jmpf", { "::temp1", "2" }), Call("SetStage", { "40" }) };
	s.functions["fragment_stage_0030_item_00"] = { Call("SetObjectiveCompleted", { "6", "True" }), Call("SetObjectiveDisplayed", { "7" }) };
	s.functions["fragment_stage_0040_item_00"] = { Call("SetObjectiveDisplayed", { "7" }) };

	std::size_t checks = 0;
	auto        f = fo4quest::CheckQuest(q, d, s, &checks);
	Check(f.size() == 2, "quest check: stage 10 done, its stage 20 and objective 5 missing -> 2 findings (" + std::to_string(f.size()) + ")");
	Check(std::any_of(f.begin(), f.end(), [](const auto& x) { return x.kind == fo4quest::Finding::Kind::kStageNotSet && x.target == 20 && x.stage == 10; }),
		"quest check: stage 20 not set");
	Check(std::any_of(f.begin(), f.end(), [](const auto& x) { return x.kind == fo4quest::Finding::Kind::kObjectiveNotShown && x.target == 5; }),
		"quest check: objective 5 not shown");
	Check(std::none_of(f.begin(), f.end(), [](const auto& x) { return x.target == 40; }), "quest check: a call after a jump is not used");
	Check(!f.empty() && f[0].console.size() == 1 &&
			  (f[0].console[0] == "setobjectivedisplayed TestQuest 5 1" || f[0].console[0] == "setstage TestQuest 20"),
		"quest check: console command");

	// what the save has done: nothing to report
	q.stages = { { 10, true }, { 20, true }, { 30, false } };
	q.objectives = { { 5, 1 } };
	Check(fo4quest::CheckQuest(q, d, s).empty(), "quest check: stage set and objective shown -> nothing");
	q.objectives = { { 5, 3 } };
	Check(fo4quest::CheckQuest(q, d, s).empty(), "quest check: objective completed + shown counts as shown");

	// stage 30 done: objective 6 must be completed; objective 7 is set by two functions -> not used
	q.stages = { { 10, true }, { 20, true }, { 30, true } };
	q.objectives = { { 5, 1 }, { 6, 1 }, { 7, 0 } };
	f = fo4quest::CheckQuest(q, d, s);
	Check(f.size() == 1 && f[0].kind == fo4quest::Finding::Kind::kObjectiveNotCompleted && f[0].target == 6 && f[0].stage == 30,
		"quest check: objective 6 shown but not completed; objective 7 touched twice is skipped");
	q.objectives = { { 5, 1 }, { 6, 3 } };
	Check(fo4quest::CheckQuest(q, d, s).empty(), "quest check: objective 6 completed -> nothing");

	// SetObjectiveDisplayed(n, false) and stages / objectives the plugin does not define are not checked
	s.functions["fragment_stage_0010_item_00"] = { Call("SetObjectiveDisplayed", { "7", "False" }), Call("SetStage", { "25" }) };
	q.stages = { { 10, true } };
	q.objectives = {};
	Check(fo4quest::CheckQuest(q, d, s).empty(), "quest check: hiding an objective and an undefined stage are not findings");

	// the .pex reader
	const auto bytes = MakePex({ { "Fragment_Stage_0010_Item_00", { { "SetObjectiveDisplayed", { 5 } }, { "SetStage", { 20 } }, { "jmp", {} },
	                                                                  { "SetStage", { 40 } } } },
		{ "Fragment_Stage_0030_Item_00", { { "SetObjectiveCompleted", { 5 } }, { "SetObjectiveCompleted", { 6, 1 } } } } });
	try {
		const auto px = pexfo4::Read(bytes);
		const auto it = px.functions.find("fragment_stage_0010_item_00");
		Check(px.name == "QF_Test_01000800" && px.parent == "Quest" && it != px.functions.end() && it->second.size() == 4 &&
				  it->second[1].op == "callmethod" && it->second[1].args[0] == "SetStage" && it->second[1].extra.size() == 1 &&
				  it->second[1].extra[0] == "20" && it->second[2].op == "jmp",
			"pexfo4::Read: functions, call arguments, jumps");
		q.stages = { { 10, true }, { 20, false } };
		q.objectives = { { 5, 1 } };
		const auto g = fo4quest::CheckQuest(q, d, px);
		Check(g.size() == 1 && g[0].target == 20, "quest check on the .pex read back");
	} catch (const std::exception& e) {
		Check(false, std::string("pexfo4::Read: ") + e.what());
	}
	auto bad = bytes;
	std::reverse(bad.begin(), bad.begin() + 4);  // FA 57 C0 DE: the big-endian magic of a Skyrim .pex
	bool threw = false;
	try {
		pexfo4::Read(bad);
	} catch (const std::exception&) {
		threw = true;
	}
	Check(threw, "pexfo4::Read refuses a Skyrim (big-endian) .pex");
}

static void TestCompanionCommands()
{
	fo4comp::Companion c;
	c.ref = 0x0001CA7D;
	c.script = "CompanionActorScript";
	Check(fo4comp::SetAffinityCommand(c, 750) == "0001CA7D.cf \"CompanionActorScript.SetAffinity\" 750", "companions: affinity command");
	auto m = fo4comp::MakeCompanionCommands(c);
	Check(m.size() == 1 && m[0] == "cqf 000289E4 \"FollowersScript.AllowCompanion\" 0001CA7D 1 1", "companions: make companion command (quest by form id)");
	Check(fo4comp::FollowCommand(c) == "cqf 000289E4 \"FollowersScript.FollowerFollow\" 0001CA7D", "companions: follow command");
	Check(fo4comp::CompanionRecord(c) == std::vector<std::string>{ "companion", "0001CA7D", "0", "CompanionActorScript" } &&
			  fo4comp::FollowRecord(c) == std::vector<std::string>{ "companionfollow", "0001CA7D" } &&
			  fo4comp::AffinityRecord(c, -250) == std::vector<std::string>{ "affinity", "0001CA7D", "-250", "CompanionActorScript" },
		"companions: helper 15 records");
	c.hasLeftPermanently = true;
	Check(fo4comp::CompanionRecord(c)[2] == "1", "companions: record clears left for good");
	m = fo4comp::MakeCompanionCommands(c);
	Check(m.size() == 2 && m[0] == "0001CA7D.cf \"CompanionActorScript.UnsetHasLeftPlayerPermanently\"", "companions: left for good cleared first");
	c.dogmeat = true;
	Check(fo4comp::MakeCompanionCommands(c).size() == 1 && fo4comp::CompanionRecord(c).size() == 3, "companions: Dogmeat has no CompanionActorScript flag to clear");
	// emptying a slot held by an actor without a companion script
	Check(fo4comp::ClearSlotCommands(0x0A001234, false) ==
			  std::vector<std::string>{ "cqf 000289E4 \"FollowersScript.ClearCompanion\"", "0A001234.setplayerteammate 0" },
		"companions: clear the companion slot (ClearCompanion, then the teammate flag off)");
	Check(fo4comp::ClearSlotCommands(0x0A001234, true) == std::vector<std::string>{ "cqf 000289E4 \"FollowersScript.ClearDogmeatCompanion\"" },
		"companions: clear the Dogmeat slot");
	Check(fo4comp::ClearSlotRecord(0x0A001234, false) == std::vector<std::string>{ "clearcompanion", "0A001234", "0" } &&
			  fo4comp::ClearSlotRecord(0x0A001234, true) == std::vector<std::string>{ "clearcompanion", "0A001234", "1" },
		"companions: helper 15 clearcompanion record");
}

static void TestCompanionsReal()
{
	const auto files = SavesFrom("RESAVERPP_FO4_SAVES");
	if (files.empty()) {
		std::printf("(fo4 companions: RESAVERPP_FO4_SAVES not set, real saves skipped)\n");
		return;
	}
	const auto  tmp = fs::temp_directory_path() / "rs_fo4comp.fos";
	std::size_t saves = 0, edited = 0;
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;  // (saves this build cannot read are other tests' business)
		}
		const auto name = f.filename().string();
		const auto r = fo4comp::Read(*s);
		++saves;
		Check(r.system.found && r.system.questDecoded && r.system.quest == 0x000289E4, name + ": the follower quest");
		Check(r.system.aliasCompanion == 0 && r.system.aliasDogmeat == 5 && r.system.aliasActive == 6, name + ": alias ids 0 / 5 / 6 (Fallout4.esm ALST)");
		Check(r.system.factionCurrent == 0x00023C01 && r.system.avAffinity == 0x000A1B80 && r.system.avFollowerState == 0x00000344,
			name + ": CurrentCompanionFaction, CA_Affinity, FollowerState from the scripts' variables");
		Check(r.companions.size() >= 15, name + ": the base game's companions (" + std::to_string(r.companions.size()) + ")");
		Check(r.find(0x0001D162) && r.find(0x0001D162)->dogmeat, name + ": Dogmeat (DogmeatActorScript)");
		// the two halves of the companion link agree in every test save
		for (const auto& c : r.companions) {
			Check(c.inCompanionAlias == c.aliasExtraSaysCompanion, name + ": " + std::to_string(c.ref) + " quest slot and alias list agree");
		}
		for (const auto& x : r.findings) {
			Check(x.level != fo4comp::Finding::Level::kProblem, name + ": no problem finding: " + x.title);
		}
		if (name.rfind("Save7_F110F019", 0) == 0 || name.rfind("Save8_F110F019", 0) == 0) {
			Check(r.system.companionFill == 0x0001CA7D && r.find(0x0001CA7D) && r.find(0x0001CA7D)->current(), name + ": Codsworth is the companion");
		}
		if (name.rfind("Save8_F110F019", 0) == 0) {
			// the actor's own half of the link broken in memory (its ExtraAliasInstanceArray entry for the Companion
			// alias renumbered in place): the check must see the two halves disagree
			auto copy = SaveFile::load(f);
			bool changed = false;
			for (auto& cf : copy->changeForms) {
				if (static_cast<ChangeFormType>(cf.typeCode()) != ChangeFormType::ACHR || copy->formIDOf(cf.refID) != 0x0001CA7D) continue;
				auto       body = cf::Body(cf);
				const auto rr = fo4cf::DecodeRefr(*copy, cf, body);
				for (const auto& e : rr.extras) {
					if (e.type != 0x88) continue;
					std::size_t p = e.range.begin + 1;
					const auto  sz = body[p] & 3;
					std::uint32_t n = 0;
					for (std::size_t k = 0; k < (sz == 0 ? 1u : sz == 1 ? 2u : 4u); ++k) n |= static_cast<std::uint32_t>(body[p + k]) << (8 * k);
					p += sz == 0 ? 1 : sz == 1 ? 2 : 4;
					n >>= 2;
					for (std::uint32_t i = 0; i < n; ++i, p += 7) {
						RefID q;
						q.data = (static_cast<std::uint32_t>(body[p]) << 16) | (body[p + 1] << 8) | body[p + 2];
						std::uint32_t a;
						std::memcpy(&a, &body[p + 3], 4);
						if (copy->formIDOf(q) == 0x000289E4 && a == 0) {
							a = 99;
							std::memcpy(&body[p + 3], &a, 4);
							changed = true;
						}
					}
				}
				if (changed) cf::SetBody(cf, body);
			}
			Check(changed, name + ": Codsworth's alias list names the Companion alias");
			const auto rb = fo4comp::Read(*copy);
			Check(std::any_of(rb.findings.begin(), rb.findings.end(),
					  [](const auto& x) { return x.ref == 0x0001CA7D && x.level == fo4comp::Finding::Level::kProblem && !x.console.empty(); }),
				name + ": slot filled but the actor's own alias list does not say so -> a problem with console commands");
		}
		if (name.rfind("Save12_F6EE2084", 0) == 0) {
			Check(r.system.companionFill == 0x00079305, name + ": Cait is the companion");
		}
		// an affinity edit on the current companion: written, read back
		const auto* cur = r.find(r.system.companionFill);
		if (cur && cur->hasAffinity && cur->hasRange && s->problems.empty()) {
			const float v = cur->affinity > 0 ? cur->affinity - 10 : cur->affinity + 10;
			Check(!fo4comp::SetAffinity(*s, cur->ref, cur->maxAffinity + 1).empty(), name + ": affinity above the script's range refused");
			const auto why = fo4comp::SetAffinity(*s, cur->ref, v);
			Check(why.empty(), name + ": SetAffinity " + why);
			if (why.empty()) {
				const auto bytes = s->serialize();
				{
					std::ofstream o(tmp, std::ios::binary);
					o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				}
				auto       back = SaveFile::load(tmp);
				const auto r2 = fo4comp::Read(*back);
				const auto* c2 = r2.find(cur->ref);
				Check(c2 && c2->hasAffinity && c2->affinity == v && c2->actorRead && c2->current(), name + ": the new affinity reads back");
				++edited;
			}
		}
		for (const auto& c : r.companions) {
			if (!c.dogmeat && !c.hasAffinity && c.hasChangeForm && c.actorRead) {
				Check(!fo4comp::SetAffinity(*s, c.ref, 100).empty(), name + ": no saved affinity -> refused");
				break;
			}
		}
	}
	std::error_code ec;
	fs::remove(tmp, ec);
	std::printf("(fo4 companions: %zu real saves, %zu affinity edits)\n", saves, edited);

	const char* data = std::getenv("RESAVERPP_FO4_DATA");
	if (!data || !*data) {
		std::printf("(fo4 quest check: RESAVERPP_FO4_DATA not set, skipped)\n");
		return;
	}
	ScriptSources src;
	src.scan(data);
	for (const auto& f : files) {
		std::unique_ptr<SaveFile> s;
		try {
			s = SaveFile::load(f);
		} catch (const std::exception&) {
			continue;
		}
		PluginDB::Input in;
		PrepareInput(*s, in);
		in.sources = src;
		in.fo4 = true;
		in.language = "en";
		PluginDB::Progress pr;
		PluginDB           db;
		db.load(in, pr);
		fo4quest::Stats st;
		const auto      found = fo4quest::FindStoppedStages(*s, db, src, &st);
		Check(st.scriptsRead > 0 && st.checks > 0 && found.empty(), f.filename().string() + ": quest check reads scripts, checks lines, finds nothing (" +
																		 std::to_string(st.checks) + " lines, " + std::to_string(found.size()) + " findings)");
	}
}

int main()
{
	TestQuestCheckSynthetic();
	TestCompanionCommands();
	TestCompanionsReal();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
