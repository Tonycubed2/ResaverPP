// Active effect removal round trip (fo4cf::RemoveActiveEffects) on every ACHR (NPCs and the player) whose body reads
// completely: fxremove <save> <sources.txt> <mode> <out.fos> <out.bin>
//   sources.txt: one line per magic item effect: "item hex, effect index, mgef flags hex, archetype, actor value hex,
//                item type, spell type, cast type, poison" (fxsources.py writes it from the plugins), as the GUI's
//                EffectSources would give them
//   mode: all     removes every effect each actor has that may be removed (the refused ones stay)
//         first   removes the first removable effect of each actor
//         source  removes every effect from the item of each actor's first removable effect (remove by source)
// Writes the save, reads it back, checks that each edited actor reads completely with exactly the effects that stayed
// (byte for byte) and the modifiers the plan said, and dumps the edited ACHRs (dumpcf format) for emucheck.py.
#include "core/ChangeFormFO4.h"
#include "core/ChangeFormData.h"
#include "core/Save.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
using namespace ess;
int main(int argc, char** argv)
{
	if (argc < 6) return 1;
	auto save = SaveFile::load(argv[1]);
	std::map<std::pair<std::uint32_t, int>, fo4cf::EffectSource> src;
	{
		std::ifstream in(argv[2]);
		std::string   l;
		while (std::getline(in, l)) {
			std::istringstream is(l);
			std::string item, flags, av, type;
			int idx, arch, st, ct, po;
			if (!(is >> item >> idx >> flags >> arch >> av >> type >> st >> ct >> po)) continue;
			fo4cf::EffectSource s;
			s.known = true;
			s.mgefFlags = std::stoul(flags, nullptr, 16);
			s.archetype = static_cast<std::uint32_t>(arch);
			s.actorValue = std::stoul(av, nullptr, 16);
			std::strncpy(s.itemType, type.c_str(), 4);
			s.spellType = static_cast<std::uint8_t>(st);
			s.castType = static_cast<std::uint8_t>(ct);
			s.poison = po != 0;
			src[{ static_cast<std::uint32_t>(std::stoul(item, nullptr, 16)), idx }] = s;
		}
	}
	const std::string mode = argv[3];
	struct Want { std::vector<fo4cf::ActiveEffect> stay; std::size_t removed = 0; std::vector<std::pair<std::uint32_t, std::array<float, 3>>> mods; float vit[4][3]; };
	std::map<std::uint32_t, Want> edited;
	std::size_t actors = 0, complete = 0, withFx = 0, removed = 0, refusedEffects = 0, statEdits = 0;
	std::map<std::string, int> why;
	for (auto& c : save->changeForms) {
		if (c.typeCode() != 1) continue;
		++actors;
		const auto d = fo4cf::DecodeActor(*save, c);
		if (!d.complete) continue;
		++complete;
		if (d.effects.empty()) continue;
		++withFx;
		std::vector<fo4cf::EffectSource> s(d.effects.size());
		for (std::size_t i = 0; i < s.size(); ++i) {
			if (auto it = src.find({ save->formIDOf(d.effects[i].item), d.effects[i].effectIndex }); it != src.end()) s[i] = it->second;
		}
		std::vector<std::size_t> ok;
		for (std::size_t i = 0; i < d.effects.size(); ++i) {
			const auto u = fo4cf::PlanEffectRemoval(d, d.effects[i], &s[i]);
			if (u.ok) ok.push_back(i); else { ++refusedEffects; why[u.why]++; }
		}
		if (ok.empty()) continue;
		std::vector<std::size_t> pick;
		if (mode == "all") pick = ok;
		else if (mode == "first") pick = { ok.front() };
		else {
			for (auto i : fo4cf::EffectsFrom(*save, d, save->formIDOf(d.effects[ok.front()].item))) {
				if (std::find(ok.begin(), ok.end(), i) != ok.end()) pick.push_back(i);
			}
		}
		// what must come back
		Want w;
		for (std::size_t i = 0; i < d.effects.size(); ++i) if (std::find(pick.begin(), pick.end(), i) == pick.end()) w.stay.push_back(d.effects[i]);
		w.removed = pick.size();
		std::map<std::uint32_t, std::array<float, 3>> mods;
		for (auto& m : d.modifiers) mods[m.formID] = { m.mod[0], m.mod[1], m.mod[2] };
		for (int k = 0; k < 4; ++k) for (int j = 0; j < 3; ++j) w.vit[k][j] = j < (int)std::min<std::uint32_t>(d.vitals[k].count, 3) ? d.vitals[k].mod[j] : 0.0f;
		std::map<std::pair<std::uint32_t, int>, float> take;
		for (auto i : pick) {
			const auto u = fo4cf::PlanEffectRemoval(d, d.effects[i], &s[i]);
			if (u.modifier && u.amount != 0) take[{ u.actorValue, u.slot }] += u.amount;
		}
		for (auto& [k, a] : take) {
			bool vital = false;
			for (int v = 0; v < 4; ++v) if (fo4cf::kVitalAV[v] == k.first) { vital = true; float x = w.vit[v][k.second] - a; w.vit[v][k.second] = std::fabs(x) <= 0.0001f ? 0.0f : x; }
			if (!vital && mods.contains(k.first)) { float x = mods[k.first][k.second] - a; mods[k.first][k.second] = std::fabs(x) <= 0.0001f ? 0.0f : x; }
			++statEdits;
		}
		for (auto& m : d.modifiers) w.mods.push_back({ m.formID, mods[m.formID] });
		std::vector<std::string> changes;
		const auto e = fo4cf::RemoveActiveEffects(*save, c, pick, s, &changes);
		if (!e.empty()) { why["RemoveActiveEffects: " + e]++; continue; }
		removed += pick.size();
		edited[c.refID.data] = std::move(w);
		if (save->formIDOf(c.refID) == 0x14) for (auto& ch : changes) std::printf("  player: %s\n", ch.c_str());
	}
	std::printf("%zu ACHRs, %zu read completely, %zu with effects; removed %zu effects from %zu actors (%zu modifier edits); %zu effects refused\n",
		actors, complete, withFx, removed, edited.size(), statEdits, refusedEffects);
	for (auto& [w, k] : why) std::printf("  %d x %s\n", k, w.c_str());
	save->save(argv[4]);
	auto again = SaveFile::load(argv[4]);
	auto old = SaveFile::load(argv[1]);
	std::map<std::uint32_t, const ChangeForm*> oldCf;
	for (auto& c : old->changeForms) oldCf[c.refID.data] = &c;
	int good = 0, bad = 0;
	std::ofstream o(argv[5], std::ios::binary);
	auto wr = [&](std::uint32_t v) { o.write(reinterpret_cast<char*>(&v), 4); };
	wr(again->formVersion);
	std::size_t unchangedOthers = 0, changedOthers = 0;
	for (auto& c : again->changeForms) {
		auto it = edited.find(c.refID.data);
		if (it == edited.end()) {
			const auto* oc = oldCf[c.refID.data];
			if (oc && cf::Body(*oc) == cf::Body(c)) ++unchangedOthers; else ++changedOthers;
			continue;
		}
		const auto& w = it->second;
		const auto a = fo4cf::DecodeActor(*again, c);
		const auto body = cf::Body(c);
		const auto ob = cf::Body(*oldCf[c.refID.data]);
		bool ok = a.complete && a.effects.size() == w.stay.size();
		for (std::size_t i = 0; ok && i < w.stay.size(); ++i) {
			ok = a.effects[i].range.size() == w.stay[i].range.size() &&
			     std::equal(ob.begin() + w.stay[i].range.begin, ob.begin() + w.stay[i].range.end, body.begin() + a.effects[i].range.begin);
		}
		ok = ok && a.modifiers.size() == w.mods.size();
		for (std::size_t i = 0; ok && i < w.mods.size(); ++i) {
			for (int j = 0; j < 3; ++j) ok = ok && a.modifiers[i].formID == w.mods[i].first && a.modifiers[i].mod[j] == w.mods[i].second[j];
		}
		for (int k = 0; ok && k < 4; ++k) for (int j = 0; j < 3; ++j) {
			const float v = j < (int)std::min<std::uint32_t>(a.vitals[k].count, 3) ? a.vitals[k].mod[j] : 0.0f;
			ok = ok && v == w.vit[k][j];
		}
		// the parts after the actor values are byte for byte the old ones
		const auto od = fo4cf::DecodeActor(*old, *oldCf[c.refID.data]);
		ok = ok && ob.size() - od.valuesRange.end == body.size() - a.valuesRange.end &&
		     std::equal(ob.begin() + od.valuesRange.end, ob.end(), body.begin() + a.valuesRange.end);
		// and the bytes before the effect list
		ok = ok && od.effectList.begin == a.effectList.begin && std::equal(ob.begin(), ob.begin() + od.effectList.begin, body.begin());
		if (ok) ++good; else { ++bad; std::printf("  BAD %08X\n", again->formIDOf(c.refID)); }
		wr(c.refID.data); o.put(static_cast<char>(c.typeCode())); o.put(static_cast<char>(c.version));
		wr(c.changeFlags); wr(again->formIDOf(c.refID)); wr(static_cast<std::uint32_t>(body.size()));
		o.write(reinterpret_cast<const char*>(body.data()), body.size());
	}
	std::printf("read back: %d edited actors exact, %d not; other change forms: %zu unchanged, %zu changed\n", good, bad, unchangedOthers, changedOthers);
	return bad || changedOthers ? 2 : 0;
}
