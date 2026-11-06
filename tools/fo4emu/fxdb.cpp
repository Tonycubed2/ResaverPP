// fxdb <save> <game Data folder> <sources.txt> [out.fos]: fo4cf::EffectSources from the real plugins (PluginDB, Fallout 4)
// compared with fxsources.py's list for every effect of every actor; with out.fos also removes every removable effect
// of every complete actor with those sources (the GUI's path), writes, reads back and prints the player's changes.
#include "core/ChangeFormFO4.h"
#include "core/ChangeFormData.h"
#include "core/PluginDB.h"
#include "core/Save.h"
#include "core/ScriptSources.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
using namespace ess;
int main(int argc, char** argv)
{
	if (argc < 4) return 1;
	auto           save = SaveFile::load(argv[1]);
	PluginDB::Input in;
	PrepareInput(*save, in);
	in.fo4 = true;
	in.language = "en";
	std::error_code ec;
	const auto empty = std::filesystem::temp_directory_path() / "fxdb-empty";
	std::filesystem::create_directories(empty, ec);
	in.sources.scan(empty, nullptr, {}, argv[2]);
	PluginDB          db;
	PluginDB::Progress prog;
	db.load(in, prog);
	std::printf("plugins read %zu, forms %zu, missing %zu\n", db.pluginsRead, db.size(), db.missing.size());
	std::map<std::pair<std::uint32_t, int>, std::string> ref;
	{
		std::ifstream f(argv[3]);
		std::string   l;
		while (std::getline(f, l)) {
			std::istringstream is(l);
			std::string        item;
			int                idx;
			is >> item >> idx;
			std::string rest;
			std::getline(is, rest);
			ref[{ static_cast<std::uint32_t>(std::stoul(item, nullptr, 16)), idx }] = rest;
		}
	}
	std::size_t same = 0, differ = 0, unknownBoth = 0, onlyRef = 0, onlyDb = 0;
	for (auto& c : save->changeForms) {
		if (c.typeCode() != 1) continue;
		const auto a = fo4cf::DecodeActor(*save, c);
		if (!a.decoded) continue;
		const auto s = fo4cf::EffectSources(*save, a, &db);
		for (std::size_t i = 0; i < a.effects.size(); ++i) {
			const auto key = std::make_pair(save->formIDOf(a.effects[i].item), static_cast<int>(a.effects[i].effectIndex));
			const auto it = ref.find(key);
			if (!s[i].known) { (it == ref.end() ? unknownBoth : onlyRef)++; continue; }
			if (it == ref.end()) { ++onlyDb; continue; }
			char buf[128];
			std::snprintf(buf, sizeof(buf), " %X %d %X %s %d %d %d", s[i].mgefFlags, s[i].archetype, s[i].actorValue, s[i].itemType, s[i].spellType, s[i].castType, int(s[i].poison));
			if (it->second == buf) ++same; else { if (differ++ < 10) std::printf("differ %08X/%d: db%s ref%s\n", key.first, key.second, buf, it->second.c_str()); }
		}
	}
	std::printf("sources: %zu same, %zu differ, %zu only in the list, %zu only from the plugins, %zu unknown to both\n", same, differ, onlyRef, onlyDb, unknownBoth);
	if (argc < 5) return differ ? 2 : 0;
	std::size_t removed = 0, actors = 0, refused = 0;
	for (auto& c : save->changeForms) {
		if (c.typeCode() != 1) continue;
		const auto a = fo4cf::DecodeActor(*save, c);
		if (!a.complete || a.effects.empty()) continue;
		const auto s = fo4cf::EffectSources(*save, a, &db);
		std::vector<std::size_t> pick;
		for (std::size_t i = 0; i < a.effects.size(); ++i) {
			if (fo4cf::PlanEffectRemoval(a, a.effects[i], &s[i]).ok) pick.push_back(i); else ++refused;
		}
		if (pick.empty()) continue;
		std::vector<std::string> ch;
		const auto e = fo4cf::RemoveActiveEffects(*save, c, pick, s, &ch);
		if (!e.empty()) { std::printf("refused %08X: %s\n", save->formIDOf(c.refID), e.c_str()); continue; }
		removed += pick.size(); ++actors;
		if (save->formIDOf(c.refID) == 0x14) for (auto& x : ch) std::printf("  player: %s\n", x.c_str());
	}
	save->save(argv[4]);
	auto again = SaveFile::load(argv[4]);
	std::size_t complete = 0, all = 0;
	for (auto& c : again->changeForms) {
		if (c.typeCode() != 1) continue;
		++all;
		complete += fo4cf::DecodeActor(*again, c).complete;
	}
	std::printf("removed %zu effects from %zu actors (%zu refused); read back: %zu of %zu ACHRs complete\n", removed, actors, refused, complete, all);
	return differ ? 2 : 0;
}
