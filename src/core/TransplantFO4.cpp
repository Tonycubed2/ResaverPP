// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "TransplantFO4.h"

#include "ChangeFormFO4.h"
#include "Quests.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace ess::transplant::fo4
{
	namespace
	{
		std::string Fmt(const char* f, auto... args)
		{
			char buf[1024];
			std::snprintf(buf, sizeof(buf), f, args...);
			return buf;
		}

		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		constexpr std::uint32_t kSpecialAV = 0x2C2;  // Strength .. Luck: 0x2C2 .. 0x2C8
		constexpr std::uint32_t kCaps = 0x0000000F;
		constexpr std::uint32_t kPlayerRef = 0x14;

		const char* kSpecialNames[7] = { "Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck" };

		// a form a snapshot can hold: from a plugin of the save (not made in the game, not out of range)
		bool Identified(const SaveFile& a_save, std::uint32_t a_id, std::string& a_plugin)
		{
			if (a_id == 0 || (a_id >> 24) == 0xFF) {
				return false;
			}
			a_plugin = a_save.pluginOf(a_id);
			return !a_plugin.empty() && a_plugin != "?" && a_plugin != "Created";
		}

		// the save has a reference for this form (the perk list can then point at it; see fo4cf::AddPerk)
		bool Referenced(const SaveFile& a_save, std::uint32_t a_formID)
		{
			if ((a_formID >> 24) == 0 && a_formID < 0x400000u) {
				return true;
			}
			return std::find(a_save.formIDs.begin(), a_save.formIDs.end(), a_formID) != a_save.formIDs.end();
		}

		ChangeForm* PlayerRef(SaveFile& a_save) { return const_cast<ChangeForm*>(fo4cf::FindPlayer(a_save)); }
	}

	const char* SpecialName(int a_index) { return a_index >= 0 && a_index < 7 ? kSpecialNames[a_index] : "?"; }

	std::uint32_t LocalOf(std::uint32_t a_formID) { return (a_formID >> 24) == 0xFE ? (a_formID & 0xFFF) : (a_formID & 0xFFFFFF); }

	std::uint32_t RuntimeID(const PluginInfo& a_plugins, const std::string& a_plugin, std::uint32_t a_local)
	{
		const auto want = Lower(a_plugin);
		for (std::size_t i = 0; i < a_plugins.full.size() && i < 0xFE; ++i) {
			if (Lower(a_plugins.full[i]) == want) {
				return static_cast<std::uint32_t>(i << 24) | (a_local & 0xFFFFFF);
			}
		}
		for (std::size_t i = 0; i < a_plugins.lite.size() && i < 0x1000; ++i) {
			if (Lower(a_plugins.lite[i]) == want) {
				return 0xFE000000u | static_cast<std::uint32_t>(i << 12) | (a_local & 0xFFF);
			}
		}
		return 0;
	}

	bool SameLoadOrder(const PluginInfo& a_a, const PluginInfo& a_b)
	{
		auto same = [](const std::vector<std::string>& x, const std::vector<std::string>& y) {
			if (x.size() != y.size()) {
				return false;
			}
			for (std::size_t i = 0; i < x.size(); ++i) {
				if (Lower(x[i]) != Lower(y[i])) {
					return false;
				}
			}
			return true;
		};
		return same(a_a.full, a_b.full) && same(a_a.lite, a_b.lite);
	}

	// ---- reading a save -------------------------------------------------------------------------------

	Snapshot FromSave(const SaveFile& a_save, const std::string& a_name)
	{
		Snapshot s;
		s.source = a_name;
		s.player = a_save.header.playerName;
		if (!a_save.isFO4()) {
			s.error = a_name + " is not a Fallout 4 save";
			return s;
		}
		s.level = fo4cf::PlayerLevel(a_save);
		if (s.level < 0) {
			s.level = static_cast<int>(a_save.header.playerLevel);
		}
		s.xp = fo4cf::PlayerXP(a_save);
		if (s.xp < 0) {
			s.missingParts.push_back("experience");
		}

		const auto* player = fo4cf::FindPlayer(a_save);
		if (!player) {
			s.error = a_name + ": no player reference (ACHR 00000014) in the save";
			return s;
		}
		// the inventory (the reference part) and the caps
		try {
			const auto r = fo4cf::DecodeRefr(a_save, *player);
			if (r.decoded) {
				s.itemsKnown = true;
				s.caps = 0;
				for (const auto& it : r.inventory) {
					if (it.formID == kCaps) {
						s.caps += it.count;
						continue;
					}
					std::string plugin;
					if (!Identified(a_save, it.formID, plugin) || it.count <= 0) {
						s.skippedItems += it.count > 0 ? 1 : 0;
						continue;
					}
					auto& x = s.items[FormKey{ Lower(plugin), LocalOf(it.formID) }];
					x.plugin = plugin;
					x.local = LocalOf(it.formID);
					x.formID = it.formID;
					x.count += it.count;
					for (const auto& st : it.stacks) {
						x.modded = x.modded || !st.extras.empty();
					}
				}
			} else {
				s.missingParts.push_back("items and caps (" + r.error + ")");
			}
		} catch (const std::exception& e) {
			s.missingParts.push_back(std::string("items and caps (") + e.what() + ")");
		}
		// SPECIAL and perks (the actor part)
		try {
			const auto a = fo4cf::DecodeActor(a_save, *player);
			if (a.decoded) {
				for (const auto& v : a.values) {
					if (v.formID >= kSpecialAV && v.formID < kSpecialAV + 7) {
						s.special[v.formID - kSpecialAV] = v.base;
					}
				}
			} else {
				s.missingParts.push_back("S.P.E.C.I.A.L. (" + a.error + ")");
			}
			if (a.tail && a.hasPerks) {
				s.perksKnown = true;
				for (const auto& p : a.perks) {
					std::string plugin;
					if (!Identified(a_save, p.formID, plugin)) {
						continue;
					}
					auto& x = s.perks[FormKey{ Lower(plugin), LocalOf(p.formID) }];
					x.plugin = plugin;
					x.local = LocalOf(p.formID);
					x.formID = p.formID;
					x.rank = std::max(x.rank, static_cast<int>(p.rank));
				}
			} else {
				s.missingParts.push_back(a.decoded ? "perks (" + (a.error.empty() ? std::string("no saved perk list") : a.error) + ")" : "perks");
			}
		} catch (const std::exception& e) {
			s.missingParts.push_back(std::string("S.P.E.C.I.A.L. and perks (") + e.what() + ")");
		}
		// quests
		std::size_t order = 0;
		for (const auto& q : quests::ReadStates(a_save)) {
			std::string plugin;
			if (!q.hasFlags || !Identified(a_save, q.formID, plugin)) {
				continue;
			}
			auto& x = s.quests[FormKey{ Lower(plugin), LocalOf(q.formID) }];
			x.plugin = plugin;
			x.local = LocalOf(q.formID);
			x.formID = q.formID;
			x.running = q.running();
			x.completed = q.completed();
			x.stage = q.highestDoneStage();
			x.journal = !q.logShown.empty() || q.inJournal();
			x.order = order++;
		}
		s.valid = true;
		return s;
	}

	// ---- comparing ------------------------------------------------------------------------------------

	std::vector<Change> Diff(const Snapshot& a_good, const Snapshot& a_bad, const SaveFile& a_goodSave)
	{
		std::vector<Change> v;
		auto add = [&](Kind k, Via via, std::string label, std::string detail, int risk, std::string why) -> Change& {
			Change c;
			c.kind = k;
			c.via = via;
			c.label = std::move(label);
			c.detail = std::move(detail);
			c.risk = risk;
			c.why = std::move(why);
			v.push_back(std::move(c));
			return v.back();
		};
		auto form = [&](Change& c, const std::string& a_plugin, std::uint32_t a_local) {
			c.plugin = a_plugin;
			c.local = a_local;
			c.goodFormID = RuntimeID(a_goodSave.plugins, a_plugin, a_local);
			if (!c.goodFormID) {
				c.available = false;
				c.tick = false;
				c.risk = 2;
				c.why = a_plugin + " is not in the good save's load order, so this cannot be brought over.";
			}
		};

		// the character: written into the file
		if (a_bad.level > a_good.level && a_good.level > 0) {
			auto& c = add(Kind::kLevel, Via::kFile, Fmt("Level %d (now %d)", a_bad.level, a_good.level), "", 0,
				"Sets the level in the save (and in the load menu). Perk points are not given: the perks come over on their own.");
			c.value = a_bad.level;
			c.now = a_good.level;
			if (fo4cf::PlayerLevel(a_goodSave) < 0) {
				c.available = c.tick = false;
				c.risk = 2;
				c.why = "The good save still has the game's starting level (no saved level to change): level up once in the game first.";
			}
		}
		if (a_bad.xp > a_good.xp + 0.5 && a_bad.xp >= 0) {
			auto& c = add(Kind::kXP, Via::kFile, Fmt("Experience %.0f (now %.0f)", a_bad.xp, std::max(0.0, a_good.xp)), "", 0, "Sets the experience points in the save.");
			c.value = a_bad.xp;
			c.now = a_good.xp;
			if (a_good.xp < 0) {
				c.available = c.tick = false;
				c.risk = 2;
				c.why = "The good save has no saved experience to change.";
			}
		}
		for (int i = 0; i < 7; ++i) {
			if (a_bad.special[i] >= 0 && a_bad.special[i] > a_good.special[i] + 0.01f) {
				auto& c = add(Kind::kSpecial, Via::kFile, Fmt("%s %.0f (now %.0f)", kSpecialNames[i], a_bad.special[i], std::max(0.0f, a_good.special[i])), "", 0,
					"Sets the base value in the save (bobbleheads and perks the old save had are counted in it).");
				c.index = i;
				c.value = a_bad.special[i];
				c.now = a_good.special[i];
				if (a_good.special[i] < 0) {
					c.available = c.tick = false;
					c.risk = 2;
					c.why = "The good save has no saved value for it to change.";
				}
			}
		}
		if (a_bad.caps > a_good.caps && a_good.caps >= 0) {
			const bool file = a_good.caps > 0;  // SetPlayerCaps rewrites a caps stack the good save has
			auto&      c = add(Kind::kCaps, file ? Via::kFile : Via::kGame, Fmt("%lld caps (now %lld)", static_cast<long long>(a_bad.caps), static_cast<long long>(a_good.caps)), "",
				0, file ? "Sets the caps you carry in the save." : "The good save has no caps to change: the game gives you the difference.");
			c.value = static_cast<double>(a_bad.caps);
			c.now = static_cast<double>(a_good.caps);
			c.count = a_bad.caps - a_good.caps;
			c.plugin = "Fallout4.esm";
			c.local = kCaps;
			c.goodFormID = kCaps;
		}

		// perks: ranks the good save has are raised in the file; new perks in the file when the save has a reference
		// for them, else through the game (AddPerk gives the next rank each time)
		for (const auto& [k, p] : a_bad.perks) {
			const auto g = a_good.perks.find(k);
			const int  now = g == a_good.perks.end() ? 0 : g->second.rank;
			if (p.rank <= now) {
				continue;
			}
			const auto id = RuntimeID(a_goodSave.plugins, p.plugin, p.local);
			const bool file = a_good.perksKnown && id && (now > 0 || Referenced(a_goodSave, id));
			auto&      c = add(Kind::kPerk, file ? Via::kFile : Via::kGame, now ? Fmt("%08X rank %d (now %d)", p.formID, p.rank, now) : Fmt("%08X rank %d", p.formID, p.rank),
				p.plugin, 0,
				file ? (now ? "Raises the perk's rank in the save." : "Adds the perk with its rank to the save.")
					 : "The game adds the perk (the console's AddPerk, once per rank).");
			form(c, p.plugin, p.local);
			c.value = p.rank;
			c.now = now;
			c.count = p.rank - now;
		}

		// items: the game adds what the old save had more of
		for (const auto& [k, it] : a_bad.items) {
			const auto         g = a_good.items.find(k);
			const std::int64_t more = it.count - (g == a_good.items.end() ? 0 : g->second.count);
			if (more <= 0) {
				continue;
			}
			auto& c = add(Kind::kItem, Via::kGame, Fmt("%08X x%lld", it.formID, static_cast<long long>(more)), it.plugin, 0,
				it.modded ? "The game adds the item(s). Weapon and armor mods, names and condition are not copied: you get the item as the game makes it."
						  : "The game adds the item(s) to your inventory.");
			form(c, it.plugin, it.local);
			c.count = more;
		}

		// quests: completed in the old save, or further along there
		for (const auto& [k, q] : a_bad.quests) {
			const auto g = a_good.quests.find(k);
			const bool goodDone = g != a_good.quests.end() && g->second.completed;
			const int  goodStage = g != a_good.quests.end() ? g->second.stage : -1;
			if (goodDone || q.stage < 0) {
				continue;
			}
			const bool complete = q.completed;
			if (!complete && !(q.running && q.stage > goodStage)) {
				continue;
			}
			auto& c = add(Kind::kQuest, Via::kGame,
				complete ? Fmt("%08X - complete it (stage %d)", q.formID, q.stage) : goodStage < 0 ? Fmt("%08X - stage %d (not started here)", q.formID, q.stage) : Fmt("%08X - stage %d (now %d)", q.formID, q.stage, goodStage), q.plugin,
				q.journal ? 1 : 2,
				q.journal ? "The game sets the quest to the stage it had (its stage script runs: rewards and follow-up quests happen normally)."
						  : "A background quest (radiant job, scene, mod setup): the Pip-Boy never listed it. Setting its stage can start or end things you "
							"never saw. Leave it unless you know it.");
			form(c, q.plugin, q.local);
			if (c.available && !q.journal) {
				c.risk = 2;
				c.tick = false;
			}
			c.value = q.stage;
			c.now = goodStage;
			c.complete = complete;
			c.journal = q.journal;
			c.order = q.order;
		}
		return v;
	}

	// ---- applying -------------------------------------------------------------------------------------

	std::string ApplyToFile(SaveFile& a_save, const Change& a_change)
	{
		if (a_change.via != Via::kFile) {
			return "this one goes into the running game";
		}
		switch (a_change.kind) {
		case Kind::kLevel:
			return fo4cf::SetPlayerLevel(a_save, static_cast<int>(a_change.value));
		case Kind::kXP:
			return fo4cf::SetPlayerXP(a_save, static_cast<float>(a_change.value));
		case Kind::kSpecial:
			return fo4cf::SetPlayerSpecial(a_save, a_change.index, static_cast<float>(a_change.value));
		case Kind::kCaps:
			return fo4cf::SetPlayerCaps(a_save, static_cast<std::int32_t>(std::min<double>(a_change.value, 2147483647.0)));
		case Kind::kPerk:
			{
				auto* p = PlayerRef(a_save);
				if (!p) {
					return "no player reference in the save";
				}
				const auto id = RuntimeID(a_save.plugins, a_change.plugin, a_change.local);
				if (!id) {
					return a_change.plugin + " is not in this save's load order";
				}
				const auto rank = static_cast<std::uint8_t>(std::clamp(a_change.value, 0.0, 255.0));
				return a_change.now > 0 ? fo4cf::SetPerkRank(a_save, *p, id, rank) : fo4cf::AddPerk(a_save, *p, id, rank);
			}
		default:
			return "this one goes into the running game";
		}
	}

	std::vector<Record> GameRecords(const Change& a_change, std::uint32_t a_formID, bool a_markCompleted)
	{
		std::vector<Record> out;
		if (a_change.via != Via::kGame || !a_formID) {
			return out;
		}
		const auto id = Fmt("%08X", a_formID);
		switch (a_change.kind) {
		case Kind::kCaps:
		case Kind::kItem:
			if (a_change.count > 0) {
				out.push_back({ "additem", id, std::to_string(std::min<std::int64_t>(a_change.count, 1000000)) });
			}
			break;
		case Kind::kPerk:
			if (a_change.count > 0) {
				out.push_back({ "addperk", id, std::to_string(std::min<std::int64_t>(a_change.count, 100)) });
			}
			break;
		case Kind::kQuest:
			if (a_change.value > a_change.now) {  // (a stage the good save has done already does not run again)
				out.push_back({ "setstage", id, std::to_string(static_cast<int>(a_change.value)) });
			}
			if (a_change.complete && a_markCompleted) {
				out.push_back({ "completequest", id });
			}
			break;
		default:
			break;
		}
		return out;
	}
}
