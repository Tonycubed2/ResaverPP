// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Transplant for Starfield: bring a character's progress from an old (often broken) save into a good one.
// Both saves are read OFFLINE from their .sfs files: the level and experience, the credits and items the player
// carries, the perks with their ranks (skills included) and the quests (stages done, completed). Forms are kept as
// plugin name + local id, so the two saves' load orders do not have to match.
// Diff() lists what the old save has on top of the good one. Each change goes one of two ways:
//  - into the good save's file (kFile): the level and experience (SetPlayerLevelSF), the credits and the items
//    (SetInventoryCountSF / AddInventoryItemSF: one plain stack; weapon and armor mods are not copied);
//  - into the running game (kGame), through ReSaverHelper for Starfield (version 2, APPLY): perks (AddPerk once per
//    rank), quest stages (SetStage) and completed quests (CompleteQuest). The form ids come from the game's load
//    order (the good save's, as the game has it loaded).
// Reading the perks and the experience needs the plugins' names (which forms are perks and actor values): the
// caller gives them through Lookup, in the good save's load order.
#pragma once

#include "Save.h"
#include "Transplant.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ess::transplant::sf
{
	struct Perk
	{
		std::string   plugin;
		std::uint32_t local = 0, formID = 0;  // formID: in its own save's load order
		int           rank = 0;
	};

	struct Item
	{
		std::string   plugin;
		std::uint32_t local = 0, formID = 0;
		std::int64_t  count = 0;
		bool          extras = false;  // a stack carries extra data that names forms (weapon / armor mods...)
	};

	struct Quest
	{
		std::string   plugin;
		std::uint32_t local = 0, formID = 0;
		bool          running = false, completed = false;
		int           stage = -1;  // the highest stage done (-1 none)
		std::size_t   order = 0;   // position in the save
	};

	struct Snapshot
	{
		bool         valid = false;
		std::string  source, player, error;
		int          level = -1;
		double       xp = -1;        // the total experience (the actor value, else the header's level and XP into it); -1: not known
		bool         xpEditable = false;  // the experience actor value was found: the level can be set in this save
		std::int64_t credits = -1;   // -1: the inventory cannot be read
		bool         perksKnown = false, itemsKnown = false;
		std::map<FormKey, Perk>  perks;
		std::map<FormKey, Item>  items;   // credits (0000000F) not included
		std::map<FormKey, Quest> quests;
		std::size_t  skippedItems = 0;    // made in the game (FF ids) or from no plugin of the save
		std::vector<std::string> missingParts;
	};

	// The names the snapshot needs, for forms in the good save's load order
	struct Lookup
	{
		std::function<int(std::uint32_t)>  perkKind;  // 1 a perk, 0 not, -1 not known (its plugin was not read)
		std::function<bool(std::uint32_t)> isActorValue;
	};

	// a_save: the save to read; a_good: the save whose load order the lookups use (a_save itself for the good save)
	Snapshot FromSave(const SaveFile& a_save, const std::string& a_name, const SaveFile& a_good, const Lookup& a_lookup);

	// The local id of a form id: 0xFE (light) keeps 12 bits, 0xFD (medium) 16, the others 24.
	std::uint32_t LocalOf(std::uint32_t a_formID);
	// The form id of plugin + local id in a save's load order (full, medium and light plugins); 0 when the plugin is not in it.
	std::uint32_t RuntimeID(const PluginInfo& a_plugins, const std::string& a_plugin, std::uint32_t a_local);

	enum class Kind
	{
		kLevel,    // the level and the experience together
		kCredits,
		kItem,
		kPerk,
		kQuest,
	};
	enum class Via
	{
		kFile,  // written into the good save's file now
		kGame,  // put into the running game by ReSaverHelper
	};

	struct Change
	{
		Kind          kind = Kind::kLevel;
		Via           via = Via::kFile;
		std::string   label, detail, why;
		int           risk = 0;           // 0 safe, 1 caution, 2 cannot / leave
		bool          available = true;   // false: its plugin is not in the good save's load order
		bool          tick = true;
		double        value = 0, now = 0; // level: the old / good level; credits; perk rank; quest stage
		double        xp = 0;             // level: the old save's total experience
		std::int64_t  count = 0;          // items / credits: how many to add; perks: ranks to add
		std::string   plugin;
		std::uint32_t local = 0;
		std::uint32_t goodFormID = 0;     // in the good save's load order
		bool          complete = false;   // quests: completed in the old save
		std::size_t   order = 0;
	};

	// What a_bad (the old save) has that a_good (the save open now) does not
	std::vector<Change> Diff(const Snapshot& a_good, const Snapshot& a_bad, const SaveFile& a_goodSave);

	// Writes one kFile change into the save. "" when done, else why not (then the save is unchanged).
	std::string ApplyToFile(SaveFile& a_save, const Change& a_change, const std::function<bool(std::uint32_t)>& a_isActorValue);

	// The ReSaverHelper (Starfield, version 2) APPLY records of one kGame change (a_formID: in the game's load order):
	// addperk <id> (once per rank to add) | setstage <quest> <stage> | completequest <quest>
	std::vector<Record> GameRecords(const Change& a_change, std::uint32_t a_formID);
}
