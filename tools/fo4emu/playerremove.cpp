// round trip on the player (ACHR 0x14): playerremove <save>                      lists the player's items per plugin
//                                         playerremove <save> <plugin> <out.fos> <out.bin> [edits]
// removes every item of the plugin (index hex as the form id's top byte, a light plugin as FExxx) from the player with
// cf::RemoveInventoryItems (Leftover items), optionally also edits caps, a perk's rank, Strength, level and XP (edits = 1),
// writes the save, reads it back, checks that the player reads completely without those items, that the actor and
// PlayerCharacter parts are unchanged (but for the edits), and dumps the edited player (dumpcf format) for emucheck.py.
#include "core/ChangeFormFO4.h"
#include "core/ChangeFormData.h"
#include "core/Save.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
using namespace ess;
static std::uint32_t PluginKey(std::uint32_t a_formID)
{
	return (a_formID >> 24) == 0xFE ? (0xFE000u | ((a_formID >> 12) & 0xFFFu)) : (a_formID >> 24);
}
int main(int argc, char** argv)
{
	if (argc < 2) return 1;
	auto save = SaveFile::load(argv[1]);
	auto* player = const_cast<ChangeForm*>(fo4cf::FindPlayer(*save));
	if (!player) { std::printf("no player\n"); return 1; }
	const auto before = fo4cf::DecodeRefr(*save, *player);
	const auto beforeA = fo4cf::DecodeActor(*save, *player);
	if (argc == 2) {
		std::map<std::uint32_t, std::pair<int, std::int64_t>> per;
		for (auto& it : before.inventory) { auto& x = per[PluginKey(it.formID)]; ++x.first; x.second += it.count; }
		std::printf("player complete %d (%s); %zu items\n", beforeA.complete, beforeA.error.c_str(), before.inventory.size());
		for (auto& [k, v] : per) std::printf("plugin %X: %d items, %lld in all\n", k, v.first, static_cast<long long>(v.second));
		return 0;
	}
	if (!beforeA.complete) { std::printf("the player does not read completely: %s\n", beforeA.error.c_str()); return 2; }
	const std::uint32_t plugin = std::strtoul(argv[2], nullptr, 16);
	const bool edits = argc > 5 && std::atoi(argv[5]);
	std::set<std::uint32_t> items;
	for (auto& it : before.inventory) if (PluginKey(it.formID) == plugin) items.insert(it.formID);
	const auto oldBody = cf::Body(*player);
	std::size_t n = 0;
	const auto e = cf::RemoveInventoryItems(*save, *player, items, &n);
	std::printf("plugin %X: %zu distinct items; removed %zu entries (%s)\n", plugin, items.size(), n, e.empty() ? "ok" : e.c_str());
	if (!e.empty()) return 2;
	// what must come back after the edits
	std::int32_t caps = -1; std::uint32_t perk = 0; std::uint8_t rank = 0; float str = 0, xp = 0; int level = -1;
	if (edits) {
		auto check = [](const char* a_what, const std::string& a_e) { std::printf("  %s: %s\n", a_what, a_e.empty() ? "ok" : a_e.c_str()); return a_e.empty(); };
		const auto c0 = fo4cf::PlayerCaps(*save);
		if (c0 >= 0) { caps = static_cast<std::int32_t>(c0 + 1234); if (!check("caps", fo4cf::SetPlayerCaps(*save, caps))) caps = -1; }
		const auto a = fo4cf::DecodeActor(*save, *player);
		if (!a.perks.empty()) {
			perk = a.perks.front().formID;
			rank = static_cast<std::uint8_t>(a.perks.front().rank == 1 ? 2 : 1);
			if (!check("perk rank", fo4cf::SetPerkRank(*save, *player, perk, rank))) perk = 0;
		}
		str = 9; if (!check("Strength", fo4cf::SetPlayerSpecial(*save, 0, str))) str = 0;
		xp = 123456; if (!check("XP", fo4cf::SetPlayerXP(*save, xp))) xp = 0;
		level = fo4cf::PlayerLevel(*save) >= 0 ? 77 : -1;
		if (level > 0 && !check("level", fo4cf::SetPlayerLevel(*save, level))) level = -1;
	}
	save->save(argv[3]);
	auto again = SaveFile::load(argv[3]);
	const auto* p2 = fo4cf::FindPlayer(*again);
	const auto r2 = fo4cf::DecodeRefr(*again, *p2);
	const auto a2 = fo4cf::DecodeActor(*again, *p2);
	bool ok = a2.complete && r2.inventory.size() == before.inventory.size() - n;
	for (auto& it : r2.inventory) ok = ok && !items.contains(it.formID);
	// the parts after the reference part: unchanged unless edited
	const auto newBody = cf::Body(*p2);
	const bool sameTail = std::equal(oldBody.begin() + static_cast<std::ptrdiff_t>(before.refrPart.end), oldBody.end(),
	                                 newBody.begin() + static_cast<std::ptrdiff_t>(r2.refrPart.end), newBody.end());
	const bool samePc = std::equal(oldBody.begin() + static_cast<std::ptrdiff_t>(beforeA.playerRange.begin), oldBody.end(),
	                               newBody.begin() + static_cast<std::ptrdiff_t>(a2.playerRange.begin), newBody.end());
	std::printf("read back: complete %d, %zu items (was %zu), actor part %s, PlayerCharacter part %s\n", a2.complete,
		r2.inventory.size(), before.inventory.size(), sameTail ? "unchanged" : "changed", samePc ? "unchanged" : "changed");
	if (!edits) ok = ok && sameTail;
	ok = ok && samePc && a2.questLog.size() == beforeA.questLog.size() && a2.objectives.size() == beforeA.objectives.size();
	if (edits) {
		bool e2 = true;
		if (caps >= 0) e2 = e2 && fo4cf::PlayerCaps(*again) == caps;
		if (perk) { bool f = false; for (auto& x : a2.perks) f = f || (x.formID == perk && x.rank == rank); e2 = e2 && f; }
		if (str) { bool f = false; for (auto& v : a2.values) f = f || (v.formID == 0x2C2 && v.base == str); e2 = e2 && f; }
		if (xp) e2 = e2 && fo4cf::PlayerXP(*again) == xp;
		if (level > 0) e2 = e2 && fo4cf::PlayerLevel(*again) == level && static_cast<int>(again->header.playerLevel) == level;
		std::printf("edits read back: %s (caps %d perk %08X rank %u Strength %g XP %g level %d)\n", e2 ? "ok" : "WRONG", caps, perk, rank, str, xp, level);
		ok = ok && e2;
	}
	std::ofstream o(argv[4], std::ios::binary);
	auto w = [&](std::uint32_t v) { o.write(reinterpret_cast<char*>(&v), 4); };
	w(again->formVersion);
	w(p2->refID.data); o.put(static_cast<char>(p2->typeCode())); o.put(static_cast<char>(p2->version));
	w(p2->changeFlags); w(again->formIDOf(p2->refID)); w(static_cast<std::uint32_t>(newBody.size()));
	o.write(reinterpret_cast<const char*>(newBody.data()), newBody.size());
	std::printf("%s\n", ok ? "ROUND TRIP OK" : "ROUND TRIP FAILED");
	return ok ? 0 : 3;
}
