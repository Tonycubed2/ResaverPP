// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "TransplantSF.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace ess::transplant::sf
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

		constexpr std::uint32_t kCredits = 0x0000000F;
		constexpr std::uint32_t kPlayerRef = 0x14;

		// a form a snapshot can hold: from a plugin of the save (not made in the game)
		bool Identified(const SaveFile& a_save, std::uint32_t a_id, std::string& a_plugin)
		{
			if (a_id == 0 || (a_id >> 24) == 0xFF) {
				return false;
			}
			a_plugin = a_save.pluginOf(a_id);
			return !a_plugin.empty() && a_plugin != "?" && a_plugin != "Created";
		}

		const ChangeForm* Player(const SaveFile& a_save)
		{
			for (const auto& c : a_save.changeForms) {
				if (c.typeCode() == 1 && a_save.formIDOf(c.refID) == kPlayerRef) {
					return &c;
				}
			}
			return nullptr;
		}
	}

	std::uint32_t LocalOf(std::uint32_t a_formID)
	{
		const auto top = a_formID >> 24;
		return top == 0xFE ? (a_formID & 0xFFF) : top == 0xFD ? (a_formID & 0xFFFF) : (a_formID & 0xFFFFFF);
	}

	std::uint32_t RuntimeID(const PluginInfo& a_plugins, const std::string& a_plugin, std::uint32_t a_local)
	{
		const auto want = Lower(a_plugin);
		for (std::size_t i = 0; i < a_plugins.full.size(); ++i) {
			if (Lower(a_plugins.full[i]) == want) {
				return static_cast<std::uint32_t>(i) << 24 | (a_local & 0xFFFFFF);
			}
		}
		for (std::size_t i = 0; i < a_plugins.medium.size(); ++i) {
			if (Lower(a_plugins.medium[i]) == want) {
				return 0xFD000000u | static_cast<std::uint32_t>(i) << 16 | (a_local & 0xFFFF);
			}
		}
		for (std::size_t i = 0; i < a_plugins.lite.size(); ++i) {
			if (Lower(a_plugins.lite[i]) == want) {
				return 0xFE000000u | static_cast<std::uint32_t>(i) << 12 | (a_local & 0xFFF);
			}
		}
		return 0;
	}

	Snapshot FromSave(const SaveFile& a_save, const std::string& a_name, const SaveFile& a_good, const Lookup& a_lookup)
	{
		Snapshot s;
		s.source = a_name;
		s.player = a_save.header.playerName;
		if (!a_save.isSF()) {
			s.error = a_name + " is not a Starfield save";
			return s;
		}
		s.level = static_cast<int>(a_save.header.playerLevel);
		// this save's form id -> the good save's (the lookups' load order)
		auto toGood = [&](std::uint32_t a_id) -> std::uint32_t {
			if (&a_save == &a_good || (a_id >> 24) == 0xFF) {
				return a_id;
			}
			std::string plugin;
			if (!Identified(a_save, a_id, plugin)) {
				return 0;
			}
			return RuntimeID(a_good.plugins, plugin, LocalOf(a_id));
		};
		const auto* player = Player(a_save);
		if (!player) {
			s.error = a_name + ": no player reference (00000014) in the save";
			return s;
		}

		// the experience (the actor values: needs the names)
		if (a_lookup.isActorValue) {
			const auto xp = FindPlayerXPSF(a_save, *player, [&](std::uint32_t id) {
				const auto g = toGood(id);
				return g ? a_lookup.isActorValue(g) : false;
			});
			if (xp.ok) {
				s.xp = xp.experience;
				s.xpEditable = true;
			}
		}
		if (s.xp < 0) {
			// the load menu's: the level and the XP into it (the game writes them from the same actor value: 0x14182fd80)
			s.xp = XPForLevelSF(static_cast<unsigned>(std::max(1, s.level))) + a_save.header.currentXP;
		}

		// the inventory and the credits
		const auto ref = DecodeReferenceSF(kPlayerRef, *player);
		if (ref.ok && ref.hasInventory) {
			s.itemsKnown = true;
			s.credits = 0;
			for (const auto& it : ref.items) {
				if (it.refIndex >= player->sfRefs.size()) {
					continue;
				}
				const auto id = a_save.formIDOf(player->sfRefs[it.refIndex]);
				const auto n = static_cast<std::int64_t>(it.total());
				if (id == kCredits) {
					s.credits += n;
					continue;
				}
				std::string plugin;
				if (!Identified(a_save, id, plugin) || n <= 0) {
					s.skippedItems += n > 0 ? 1 : 0;
					continue;
				}
				auto& x = s.items[FormKey{ Lower(plugin), LocalOf(id) }];
				x.plugin = plugin;
				x.local = LocalOf(id);
				x.formID = id;
				x.count += n;
				for (const auto& st : it.stacks) {
					x.extras = x.extras || st.extraRefs > 0;
				}
			}
		} else {
			s.missingParts.push_back("items and credits (" + (ref.ok ? std::string("no saved inventory") : ref.error) + ")");
		}

		// the perks (the actor part: needs the names)
		if (a_lookup.perkKind) {
			const auto pk = FindPerksSF(a_save, *player, [&](std::uint32_t id) {
				const auto g = toGood(id);
				return g ? a_lookup.perkKind(g) : -1;
			});
			if (pk.ok) {
				s.perksKnown = true;
				for (const auto& p : pk.perks) {
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
				s.missingParts.push_back("perks (" + pk.error + ")");
			}
		} else {
			s.missingParts.push_back("perks (the plugin names are not read)");
		}

		// the quests: stages done, running, completed
		std::size_t order = 0;
		for (const auto& c : a_save.changeForms) {
			if (c.typeCode() != 8) {
				continue;
			}
			const auto id = a_save.formIDOf(c.refID);
			std::string plugin;
			if (!Identified(a_save, id, plugin)) {
				continue;
			}
			const auto q = DecodeQuestSF(c);
			if (!q.ok || !q.hasFlags) {
				continue;
			}
			auto& x = s.quests[FormKey{ Lower(plugin), LocalOf(id) }];
			x.plugin = plugin;
			x.local = LocalOf(id);
			x.formID = id;
			x.running = q.enabled();
			x.completed = q.completed();
			for (const auto& st : q.stages) {
				if (st.done) {
					x.stage = std::max(x.stage, static_cast<int>(st.index));
				}
			}
			x.order = order++;
		}
		s.valid = true;
		return s;
	}

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

		// the level and the experience: into the file
		if (a_bad.level > a_good.level || (a_bad.level == a_good.level && a_bad.xp > a_good.xp + 0.5)) {
			auto& c = add(Kind::kLevel, Via::kFile,
				a_bad.xp >= 0 ? Fmt("Level %d, %.0f XP (now level %d)", a_bad.level, a_bad.xp, a_good.level) : Fmt("Level %d (now %d)", a_bad.level, a_good.level), "", 0,
				"Sets the level and the experience in the save (and in the load menu). Skill points are not given: the perks and "
				"skills come over on their own.");
			c.value = a_bad.level;
			c.now = a_good.level;
			c.xp = a_bad.xp;
			if (!a_good.xpEditable) {
				c.available = c.tick = false;
				c.risk = 2;
				c.why = "The good save's experience was not found among its actor values (are the plugin names read?), so its level "
						"cannot be changed here.";
			}
		}

		// credits: into the file
		if (a_bad.credits > a_good.credits && a_good.credits >= 0) {
			auto& c = add(Kind::kCredits, Via::kFile, Fmt("%lld credits (now %lld)", static_cast<long long>(a_bad.credits), static_cast<long long>(a_good.credits)), "", 0,
				"Sets the credits you carry in the save.");
			c.value = static_cast<double>(a_bad.credits);
			c.now = static_cast<double>(a_good.credits);
			c.count = a_bad.credits - a_good.credits;
			c.plugin = "Starfield.esm";
			c.local = kCredits;
			c.goodFormID = kCredits;
		}

		// items the old save has more of: into the file, one plain stack
		if (a_good.itemsKnown) {
			for (const auto& [k, it] : a_bad.items) {
				const auto         g = a_good.items.find(k);
				const std::int64_t more = it.count - (g == a_good.items.end() ? 0 : g->second.count);
				if (more <= 0) {
					continue;
				}
				auto& c = add(Kind::kItem, Via::kFile, Fmt("%08X x%lld", it.formID, static_cast<long long>(more)), it.plugin, it.extras ? 1 : 0,
					it.extras ? "Added to your inventory in the save as a plain stack: what the old stack carried on top (weapon or "
								"armor mods, a custom name, who owned it) is not copied."
							  : "Added to your inventory in the save.");
				form(c, it.plugin, it.local);
				c.count = more;
			}
		}

		// perks the old save has more ranks of: through the game (AddPerk gives the next rank each time)
		for (const auto& [k, p] : a_bad.perks) {
			const auto g = a_good.perks.find(k);
			const int  now = g == a_good.perks.end() ? 0 : g->second.rank;
			if (p.rank <= now) {
				continue;
			}
			auto& c = add(Kind::kPerk, Via::kGame, now ? Fmt("%08X rank %d (now %d)", p.formID, p.rank, now) : Fmt("%08X rank %d", p.formID, p.rank), p.plugin,
				a_good.perksKnown ? 0 : 1,
				a_good.perksKnown ? "The game adds the perk (the console's AddPerk, once per rank)."
								  : "The game adds the perk (AddPerk, once per rank). The good save's perks were not read, so it may "
									"already have some of these ranks.");
			form(c, p.plugin, p.local);
			c.value = p.rank;
			c.now = now;
			c.count = p.rank - now;
		}

		// quests: completed in the old save, or further along there
		for (const auto& [k, q] : a_bad.quests) {
			const auto g = a_good.quests.find(k);
			const bool goodDone = g != a_good.quests.end() && g->second.completed;
			const int  goodStage = g != a_good.quests.end() ? g->second.stage : -1;
			if (goodDone || q.stage < 0) {
				continue;
			}
			if (!q.completed && !(q.running && q.stage > goodStage)) {
				continue;
			}
			auto& c = add(Kind::kQuest, Via::kGame,
				q.completed     ? Fmt("%08X - complete it (stage %d)", q.formID, q.stage)
				: goodStage < 0 ? Fmt("%08X - stage %d (not started here)", q.formID, q.stage)
								: Fmt("%08X - stage %d (now %d)", q.formID, q.stage, goodStage),
				q.plugin, 1,
				"The game sets the quest to the stage it had (its stage script runs: rewards and follow-up quests happen normally).");
			form(c, q.plugin, q.local);
			c.value = q.stage;
			c.now = goodStage;
			c.complete = q.completed;
			c.order = q.order;
		}
		return v;
	}

	std::string ApplyToFile(SaveFile& a_save, const Change& a_change, const std::function<bool(std::uint32_t)>& a_isActorValue)
	{
		if (!a_save.isSF()) {
			return "not a Starfield save";
		}
		ChangeForm* player = nullptr;
		for (auto& c : a_save.changeForms) {
			if (c.typeCode() == 1 && a_save.formIDOf(c.refID) == kPlayerRef) {
				player = &c;
			}
		}
		if (!player) {
			return "the save has no player reference";
		}
		std::string why;
		switch (a_change.kind) {
		case Kind::kLevel:
			{
				const auto level = static_cast<unsigned>(a_change.value);
				const float base = XPForLevelSF(level);
				const float need = XPForLevelSF(level + 1) - base;
				float       into = a_change.xp >= 0 ? static_cast<float>(a_change.xp) - base : 0.0f;
				into = std::clamp(into, 0.0f, std::max(0.0f, need - 1.0f));
				if (!SetPlayerLevelSF(a_save, static_cast<std::uint16_t>(level), into, a_isActorValue, &why)) {
					return why.empty() ? "the level could not be set" : why;
				}
				return {};
			}
		case Kind::kCredits:
			{
				const auto ref = DecodeReferenceSF(kPlayerRef, *player);
				if (!ref.ok) {
					return "the player's inventory could not be read: " + ref.error;
				}
				for (const auto& it : ref.items) {
					if (it.refIndex < player->sfRefs.size() && a_save.formIDOf(player->sfRefs[it.refIndex]) == kCredits && !it.stacks.empty()) {
						const auto first = it.stacks.front();
						const auto want = static_cast<std::int64_t>(first.count) + a_change.count;
						if (want > 0xFFFFFFFFll) {
							return "too many credits";
						}
						return SetInventoryCountSF(*player, first, static_cast<std::uint32_t>(want)) ? std::string() : "the credits could not be changed";
					}
				}
				if (a_change.count > 0xFFFFFFFFll) {
					return "too many credits";
				}
				return AddInventoryItemSF(a_save, *player, kCredits, static_cast<std::uint32_t>(a_change.count), &why) ? std::string()
																														: (why.empty() ? "the credits could not be added" : why);
			}
		case Kind::kItem:
			{
				if (!a_change.goodFormID) {
					return a_change.plugin + " is not in this save's load order";
				}
				if (a_change.count <= 0 || a_change.count > 0xFFFFFFFFll) {
					return "a count out of range";
				}
				// an item the player has already: its first stack gets the more
				const auto ref = DecodeReferenceSF(kPlayerRef, *player);
				if (!ref.ok) {
					return "the player's inventory could not be read: " + ref.error;
				}
				for (const auto& it : ref.items) {
					if (it.refIndex < player->sfRefs.size() && a_save.formIDOf(player->sfRefs[it.refIndex]) == a_change.goodFormID && !it.stacks.empty()) {
						const auto want = static_cast<std::int64_t>(it.stacks.front().count) + a_change.count;
						if (want > 0xFFFFFFFFll) {
							return "a count out of range";
						}
						return SetInventoryCountSF(*player, it.stacks.front(), static_cast<std::uint32_t>(want)) ? std::string() : "the count could not be changed";
					}
				}
				return AddInventoryItemSF(a_save, *player, a_change.goodFormID, static_cast<std::uint32_t>(a_change.count), &why) ? std::string()
																																	: (why.empty() ? "the item could not be added" : why);
			}
		default: return "this change goes through the game";
		}
	}

	std::vector<Record> GameRecords(const Change& a_change, std::uint32_t a_formID)
	{
		std::vector<Record> out;
		const auto          id = Fmt("%08X", a_formID);
		switch (a_change.kind) {
		case Kind::kPerk:
			for (std::int64_t i = 0; i < std::min<std::int64_t>(a_change.count, 20); ++i) {
				out.push_back({ "addperk", id });
			}
			break;
		case Kind::kQuest:
			if (a_change.value >= 0) {
				out.push_back({ "setstage", id, std::to_string(static_cast<int>(a_change.value)) });
			}
			if (a_change.complete) {
				out.push_back({ "completequest", id });
			}
			break;
		default: break;
		}
		return out;
	}
}
