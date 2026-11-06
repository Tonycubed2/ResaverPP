// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Transplant for Fallout 4: bring a character's progress from an old (often broken) save into a good one.
// Both saves are read OFFLINE from their .fos files (a broken save often cannot even be loaded): level,
// experience, S.P.E.C.I.A.L., perks and their ranks, caps, the player's inventory and the quests. Forms are
// kept as plugin name + local id, so the two saves' load orders do not have to match.
// Diff() lists what the old save has on top of the good one. Each change goes one of two ways:
//  - into the good save's file (kFile): level, experience, SPECIAL, caps and perk ranks, written with the
//    fo4cf setters (and perks the good save already has a reference for); items with their weapon / armor mods,
//    custom name and condition (added as inventory stacks to the player's ACHR, form ids inside them moved from
//    the old save's load order to the good save's; see AddItemStacks);
//  - into the running game (kGame), through the Fallout 4 ReSaverHelper (version 14): items only when the good
//    save's player cannot be edited (as the game makes them), perks the save has no reference for, quest stages
//    and quest objectives. The helper turns each record into one of a fixed set of console commands; the form ids
//    are worked out here from the game's load order (a save the game just made).
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

	// A form named inside an item's extra data: plugin + local id (null: no form)
	struct FormRef
	{
		bool          null = true;
		std::string   plugin;
		std::uint32_t local = 0;
		bool          operator==(const FormRef& o) const = default;
	};

	// One attached object mod (BGSObjectInstanceExtra, extra 0x35; its LoadGame 0x24a240 reads per entry a form id,
	// u8 attach index, u8 rank and, from form version 0x15, u8 flags)
	struct ObjectMod
	{
		FormRef       mod;
		std::uint8_t  attach = 0, rank = 0, flags = 0;
		bool          operator==(const ObjectMod& o) const = default;
	};

	// One inventory stack with what of its extra data is copied: object mods (0x35), the display name (0x99,
	// ExtraTextDisplayData, 0x2acdf0) and condition (0x25, ExtraHealth: f32). Other extra data of the stack (unique id,
	// aliases, leveled item, ownership, reference handles...) belongs to the old game's state and is left behind.
	struct StackData
	{
		std::int32_t  count = 0;
		bool          hasMods = false;
		std::uint16_t modTemplate = 0xFFFF;  // the u16 before the list (the object template index; 0xFFFF none)
		std::vector<ObjectMod> mods;
		bool          hasText = false;
		FormRef       textMessage, textQuest;
		std::int32_t  textValue = 0;
		std::uint16_t textFlags = 0;
		bool          hasName = false;       // message and quest null and value -2: a name of its own
		std::string   name;
		std::vector<std::pair<std::string, FormRef>> textEntries;
		bool          hasHealth = false;
		float         health = 0;
		std::vector<std::uint8_t> leftBehind;  // extra types of the old stack that are not copied
		std::string   problem;   // why this stack cannot be written (a form made in the game, an unreadable extra)

		// what makes two stacks the same thing (the count left out)
		std::string key() const;
		bool        plain() const { return !hasMods && !hasText && !hasHealth; }
	};

	struct Item
	{
		std::string   plugin;
		std::uint32_t local = 0, formID = 0;
		std::int64_t  count = 0;  // every stack added up
		bool          modded = false;  // a stack carries its own extra data (weapon / armor mods, condition...)
		std::vector<StackData> stacks;
	};

	struct ObjectiveState
	{
		std::uint32_t index = 0;
		std::uint32_t state = 0;  // 0 dormant, 1 shown, 2 completed, 3 completed + shown, 4 failed, 5 failed + shown
	};

	struct Quest
	{
		std::string   plugin;
		std::uint32_t local = 0, formID = 0;
		bool          running = false, completed = false;
		int           stage = -1;   // the highest stage done (-1 none)
		bool          journal = false;  // the Pip-Boy lists it (a log entry or an objective shown)
		std::size_t   order = 0;    // position in the save (change form order)
		std::vector<ObjectiveState> objectives;  // from the QUST change form (TESQuest::LoadGame 0x66ca40, flag 29)
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
		bool        inventoryWritable = false;  // the player's body reads completely: items can be added to the file
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
		kObjective,  // a quest objective's state (in the game: SetObjectiveDisplayed / Completed / Failed)
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
		std::vector<StackData> stacks;    // items (file): the stacks to add, each with its count
		std::uint32_t objective = 0;      // objectives: the index
		std::uint32_t state = 0;          // objectives: the old save's state (1..5)
	};

	// What a_bad (the old save) has that a_good (the save open now) does not. a_goodSave decides which
	// changes can be written into the file (a perk the save has no reference for goes to the game).
	std::vector<Change> Diff(const Snapshot& a_good, const Snapshot& a_bad, const SaveFile& a_goodSave);

	// Writes one kFile change into the save. "" when done, else why not (then the save is unchanged).
	std::string ApplyToFile(SaveFile& a_save, const Change& a_change);

	// Adds inventory stacks of plugin + local id to the player's ACHR in a_save: appended to the item's entry when
	// the player has one, else a new entry. Form ids (the item and those in the stacks' extra data) are written in
	// a_save's load order; one the save's form id array does not have is appended to it (the game's loader 0xbeadb0
	// reads the array as plain index -> form id pairs). Needs the player's body to read completely; the actor part
	// is kept byte for byte, the result is decoded again and checked before it is kept. "" or why not (then the
	// save is unchanged).
	std::string AddItemStacks(SaveFile& a_save, const std::string& a_plugin, std::uint32_t a_local, const std::vector<StackData>& a_stacks);

	// The ReSaverHelper APPLY records of one kGame change (a_formID: its form id in the game's load order).
	// additem <id> <count> | addperk <id> <ranks> | setstage <quest> <stage> | completequest <quest> |
	// objective <quest> <index> <state 1..5> (helper 14)
	// a_markCompleted: a quest completed in the old save is also marked completed after its stage.
	std::vector<Record> GameRecords(const Change& a_change, std::uint32_t a_formID, bool a_markCompleted);
}
