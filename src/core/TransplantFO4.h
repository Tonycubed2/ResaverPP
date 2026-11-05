// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Transplant for Fallout 4: bring a character's progress from an old (often broken) save into a good one.
// Both saves are read OFFLINE from their .fos files (a broken save often cannot even be loaded): level,
// experience, S.P.E.C.I.A.L., perks and their ranks, caps, the player's inventory and the quests. Forms are
// kept as plugin name + local id, so the two saves' load orders do not have to match.
// Diff() lists what the old save has on top of the good one. Each change goes one of two ways:
//  - into the good save's file (kFile): level, experience, SPECIAL, caps and perk ranks, written with the
//    fo4cf setters (and perks the good save already has a reference for);
//  - into the running game (kGame), through the Fallout 4 ReSaverHelper (version 13): items, perks the save
//    has no reference for, quest stages. The helper turns each record into one of a fixed set of console
//    commands; the form ids are worked out here from the game's load order (a save the game just made).
#pragma once

#include "Save.h"
#include "Transplant.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ess::transplant::fo4
{
	struct Perk
	{
		std::string   plugin;  // as the save spells it
		std::uint32_t local = 0, formID = 0;
		int           rank = 0;
	};

	struct Item
	{
		std::string   plugin;
		std::uint32_t local = 0, formID = 0;
		std::int64_t  count = 0;  // every stack added up
		bool          modded = false;  // a stack carries its own extra data (weapon / armor mods, condition...)
	};

	struct Quest
	{
		std::string   plugin;
		std::uint32_t local = 0, formID = 0;
		bool          running = false, completed = false;
		int           stage = -1;   // the highest stage done (-1 none)
		bool          journal = false;  // the Pip-Boy lists it (a log entry or an objective shown)
		std::size_t   order = 0;    // position in the save (change form order)
	};

	struct Snapshot
	{
		bool        valid = false;
		std::string source, player, error;
		int         level = -1;       // the ACBS level, else the header's
		double      xp = -1;          // -1: not saved
		std::array<float, 7> special{ -1, -1, -1, -1, -1, -1, -1 };  // -1: no saved base
		std::int64_t caps = -1;       // -1: the inventory cannot be read
		bool        perksKnown = false, itemsKnown = false;
		std::map<FormKey, Perk>  perks;
		std::map<FormKey, Item>  items;   // caps (0x0000000F) not included
		std::map<FormKey, Quest> quests;
		std::size_t skippedItems = 0;     // made in the game (FF ids) or from no plugin of the save
		std::vector<std::string> missingParts;  // what could not be read, in plain words
	};

	Snapshot    FromSave(const SaveFile& a_save, const std::string& a_name);
	const char* SpecialName(int a_index);  // 0 Strength .. 6 Luck

	// The local id of a form id: 0xFE (light plugin) ids keep 12 bits, the others 24.
	std::uint32_t LocalOf(std::uint32_t a_formID);
	// The form id of plugin + local id in a load order (full and light plugins, as a save lists them); 0 when the
	// plugin is not in it. Light plugins: 0xFE000000 | index << 12 | local.
	std::uint32_t RuntimeID(const PluginInfo& a_plugins, const std::string& a_plugin, std::uint32_t a_local);
	// Same plugins in the same order (case does not matter).
	bool          SameLoadOrder(const PluginInfo& a_a, const PluginInfo& a_b);

	enum class Kind
	{
		kLevel,
		kXP,
		kSpecial,
		kCaps,
		kPerk,
		kItem,
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
		int           risk = 0;           // 0 safe, 1 caution, 2 cannot
		bool          available = true;   // false: its plugin is not in the good save's load order
		bool          tick = true;
		// what to do
		int           index = 0;          // SPECIAL: 0..6
		double        value = 0;          // level / xp / SPECIAL / caps / perk rank / quest stage: the old save's value
		double        now = 0;            // the good save's value (perk rank 0: it does not have it)
		std::int64_t  count = 0;          // items: how many to add; perks (game): how many ranks to add
		std::string   plugin;             // form: plugin + local id
		std::uint32_t local = 0;
		std::uint32_t goodFormID = 0;     // the form id in the good save's load order (0: its plugin is not there)
		bool          complete = false;   // quests: the old save had it completed
		bool          journal = false;    // quests: the Pip-Boy listed it in the old save
		std::size_t   order = 0;          // quests: their order in the old save
	};

	// What a_bad (the old save) has that a_good (the save open now) does not. a_goodSave decides which
	// changes can be written into the file (a perk the save has no reference for goes to the game).
	std::vector<Change> Diff(const Snapshot& a_good, const Snapshot& a_bad, const SaveFile& a_goodSave);

	// Writes one kFile change into the save. "" when done, else why not (then the save is unchanged).
	std::string ApplyToFile(SaveFile& a_save, const Change& a_change);

	// The ReSaverHelper APPLY records of one kGame change (a_formID: its form id in the game's load order).
	// additem <id> <count> | addperk <id> <ranks> | setstage <quest> <stage> | completequest <quest>
	// a_markCompleted: a quest completed in the old save is also marked completed after its stage.
	std::vector<Record> GameRecords(const Change& a_change, std::uint32_t a_formID, bool a_markCompleted);
}
