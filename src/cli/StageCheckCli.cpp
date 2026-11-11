// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// resaverpp-cli --stage-check <Data or MO2 mods folder> <plugin,plugin,...> <quest EDID[:stage] | --all>... [--starts]
// (--all: every quest with a completion stage, one line each)
// What Transplant's stage check finds for quests (core/StageSafety.h): runs the same check on the plugins and scripts of
// a folder, without a save or the game. The stage defaults to the quest's completion stage; --starts: the quest is not
// running (its start-up stages run first).
#include "core/PluginDB.h"
#include "core/ScriptSources.h"
#include "core/StageSafety.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace ess;

namespace
{
	std::string LowerS(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}
}

int StageCheckMain(int argc, char** argv)
{
	if (argc < 5) {
		std::fputs("usage: resaverpp-cli --stage-check <Data or mods folder> <plugin,plugin,...> <quest EDID[:stage]>... [--starts]\n", stderr);
		return 2;
	}
	const std::filesystem::path folder = argv[2];
	PluginDB::Input             in;
	for (std::string list = argv[3]; !list.empty();) {
		const auto comma = list.find(',');
		in.full.push_back(list.substr(0, comma));
		list = comma == std::string::npos ? std::string() : list.substr(comma + 1);
	}
	bool                     starts = false, all = false;
	std::vector<std::string> wanted;
	for (int i = 4; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--starts") {
			starts = true;
		} else if (a == "--all") {
			all = true;
		} else {
			wanted.push_back(a);
		}
	}
	in.sources.scan(folder);
	PluginDB::Progress prog;
	PluginDB           db;
	db.load(in, prog);
	for (const auto& m : db.missing) {
		std::fprintf(stderr, "plugin not found: %s\n", m.c_str());
	}
	std::map<std::string, std::unique_ptr<pex::File>> cache;
	stagesafety::Env                                  env;
	env.quest = [&](std::uint32_t id) { return db.quest(id); };
	env.script = [&](const std::string& a_name) -> const pex::File* {
		const auto key = LowerS(a_name);
		if (auto it = cache.find(key); it != cache.end()) {
			return it->second.get();
		}
		auto& slot = cache[key];
		if (const auto px = in.sources.findPex(a_name)) {
			slot = std::make_unique<pex::File>(pex::Read(ScriptSources::readPex(*px)));
		}
		return slot.get();
	};
	int rc = 0;
	if (all) {
		std::map<std::string, std::uint32_t> byEdid;  // sorted output
		for (const auto& [qid, d] : db.questDefs()) {
			if (!d.completionStages().empty()) {
				byEdid[d.edid] = qid;
			}
		}
		std::size_t counts[3]{};
		for (const auto& [edid, qid] : byEdid) {
			const auto* d = db.quest(qid);
			const int   stage = d->completionStages().front();
			const auto  r = stagesafety::Check(qid, stage, starts, env);
			const int   k = r.state == stagesafety::Report::State::kUnsafe ? 0 : r.state == stagesafety::Report::State::kSafe ? 1 : 2;
			++counts[k];
			std::printf("%-8s %s '%s' stage %d%s%s\n", k == 0 ? "UNSAFE" : k == 1 ? "safe" : "NOTCHK", edid.c_str(), d->name.c_str(), stage,
				k == 0 ? ": " : k == 2 ? ": " : "", k == 0 ? r.what(2).c_str() : k == 2 ? r.notChecked.c_str() : "");
		}
		std::printf("%zu unsafe, %zu safe, %zu not checked\n", counts[0], counts[1], counts[2]);
		return 0;
	}
	for (const auto& w : wanted) {
		const auto colon = w.find(':');
		const auto edid = LowerS(w.substr(0, colon));
		std::uint32_t                 id = 0;
		const quests::QuestDef*       def = nullptr;
		for (const auto& [qid, d] : db.questDefs()) {
			if (LowerS(d.edid) == edid) {
				id = qid;
				def = &d;
			}
		}
		if (!def) {
			std::printf("%s: no such quest\n", w.c_str());
			rc = 1;
			continue;
		}
		int stage = -1;
		if (colon != std::string::npos) {
			stage = std::atoi(w.c_str() + colon + 1);
		} else if (const auto st = def->completionStages(); !st.empty()) {
			stage = st.front();
		}
		const auto r = stagesafety::Check(id, stage, starts, env);
		std::printf("%08X %s '%s' stage %d%s: %s  (%zu functions read)\n", id, def->edid.c_str(), def->name.c_str(), stage, starts ? " (+ start-up stages)" : "",
			r.state == stagesafety::Report::State::kUnsafe ? "UNSAFE" : r.state == stagesafety::Report::State::kSafe ? "safe" : "NOT CHECKED", r.functions);
		for (const auto& f : r.findings) {
			std::printf("    %s%s: %s\n        in %s\n", f.followUp ? "(a quest it starts, not counted) " : "", stagesafety::KindText(f.kind), f.call.c_str(),
				f.where.c_str());
		}
		for (const auto& x : r.rewards) {
			std::printf("    gives: %s\n", x.c_str());
		}
		for (const auto& x : r.followUps) {
			std::printf("    starts / moves on: %s\n", x.c_str());
		}
		for (const auto& x : r.notRead) {
			std::printf("    script not found (not followed): %s\n", x.c_str());
		}
		if (!r.notChecked.empty()) {
			std::printf("    not checked: %s\n", r.notChecked.c_str());
		}
	}
	return rc;
}
