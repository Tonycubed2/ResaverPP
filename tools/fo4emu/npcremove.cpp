// round trip: remove every inventory item of plugin index a_plugin (top byte of the form id; light plugins 0xFE + 12
// bits when given as FExxx) from every NPC (ACHR, not the player) whose body reads completely; write the save; read it
// back and check; dump the edited ACHRs (dumpcf format) for the emulator.
#include "core/ChangeFormFO4.h"
#include "core/ChangeFormData.h"
#include "core/Save.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
using namespace ess;
int main(int argc, char** argv)
{
	if (argc < 2) return 1;
	auto save = SaveFile::load(argv[1]);
	if (argc == 2) {  // list: items held by NPCs per plugin index, and how many holders read completely
		std::map<std::uint32_t, std::pair<int, int>> per;
		for (auto& c : save->changeForms) {
			if (c.typeCode() != 1) continue;
			const auto r = fo4cf::DecodeRefr(*save, c);
			if (!r.decoded || r.inventory.empty()) continue;
			const bool full = fo4cf::DecodeActor(*save, c).complete;
			for (auto& it : r.inventory) { auto& x = per[it.formID >> 24]; ++x.first; x.second += full; }
		}
		for (auto& [k, v] : per) std::printf("plugin %02X: %d items held by actors, %d in actors read completely\n", k, v.first, v.second);
		return 0;
	}
	const std::uint32_t plugin = std::strtoul(argv[2], nullptr, 16);
	std::set<std::uint32_t> items;
	for (auto& c : save->changeForms) {
		if (c.typeCode() != 1) continue;
		const auto r = fo4cf::DecodeRefr(*save, c);
		for (auto& it : r.inventory) if ((it.formID >> 24) == plugin) items.insert(it.formID);
	}
	std::printf("%zu distinct items of plugin %02X\n", items.size(), plugin);
	std::set<std::uint32_t> edited;
	std::size_t total = 0, refused = 0;
	std::map<std::string, int> why;
	for (auto& c : save->changeForms) {
		if (c.typeCode() != 1) continue;
		std::size_t n = 0;
		const auto e = cf::RemoveInventoryItems(*save, c, items, &n);
		if (!e.empty()) {
			const auto r = fo4cf::DecodeRefr(*save, c);
			bool has = false;
			for (auto& it : r.inventory) has = has || items.contains(it.formID);
			if (has) { ++refused; why[e]++; }
			continue;
		}
		if (n) { edited.insert(c.refID.data); total += n; }
	}
	std::printf("removed %zu entries from %zu actors; refused %zu actors\n", total, edited.size(), refused);
	for (auto& [w, k] : why) std::printf("  %d %s\n", k, w.c_str());
	save->save(argv[3]);
	auto again = SaveFile::load(argv[3]);
	int ok = 0, bad = 0, combat = 0;
	std::ofstream o(argv[4], std::ios::binary);
	auto w = [&](std::uint32_t v) { o.write(reinterpret_cast<char*>(&v), 4); };
	w(again->formVersion);
	for (auto& c : again->changeForms) {
		if (c.typeCode() != 1 || !edited.contains(c.refID.data)) continue;
		const auto a = fo4cf::DecodeActor(*again, c);
		const auto r = fo4cf::DecodeRefr(*again, c);
		bool has = false;
		for (auto& it : r.inventory) has = has || items.contains(it.formID);
		if (a.complete && !has) ++ok; else ++bad;
		if (a.hasCombat) ++combat;
		auto body = cf::Body(c);
		w(c.refID.data); o.put(static_cast<char>(c.typeCode())); o.put(static_cast<char>(c.version));
		w(c.changeFlags); w(again->formIDOf(c.refID)); w(static_cast<std::uint32_t>(body.size()));
		o.write(reinterpret_cast<const char*>(body.data()), body.size());
	}
	std::printf("read back: %d edited actors read completely without the items, %d not (%d with a combat controller)\n", ok, bad, combat);
	return bad ? 2 : 0;
}
