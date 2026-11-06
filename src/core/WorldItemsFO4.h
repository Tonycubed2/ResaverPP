// ReSaver++ - Fallout 4: items dropped or created in the world (created references, FF......), and their removal.
//
// What the game keeps of such an object (checked in Fallout4.exe 1.11.240 and on Tony's Save7 -> Save8, which differ by
// 9 dropped Pipe Pistols): its own REFR change form and nothing else. A created reference's change form carries its
// initial data (initial type 5, 31 bytes: cell or worldspace, position, rotation, a byte, base object; 0xbf6c40 gives
// type 5 to every FF form id, whatever its change flags), so the change form loader (0xbee760 -> 0xbefb30) creates the
// object from that alone. The three new pistol references of Save8 (FF001199, FF0011BB, FF0013CF) appear nowhere else in
// the save: not in any global data block, not in any other change form (the player, the worldspace's cells), not in the
// form id array, not in Papyrus. So a created reference is removed by removing its change form, as long as nothing else
// refers to it.
//
// What must not be removed, and is left alone here (reason given):
//  - what the game itself keeps when it resets a cell (TESObjectCELL attach/reset 0x4d2588: a created reference of a cell
//    being reset is deleted through vfunc 0x140 unless 0x4fcc70 / 0x4fcc90 / 0x4feb70 / 0x4fcd60 say otherwise):
//    an object held by someone (extra 0x1C ExtraReferenceHandles: it is in an inventory, not lying in the world), an
//    object in a quest alias (extra 0x88 ExtraAliasInstanceArray; 0x8C and 0x95 ExtraFromAlias are taken as the same),
//    and a container whose inventory holds such things (stack extras 0x1C / 0x88);
//  - an object a script is attached to (a Papyrus script instance on its RefID);
//  - an object whose RefID (3 bytes) or form id (u32) appears in any other change form, in a global data block, or in
//    the form id array: something may refer to it (e.g. FF000D87 of Save8: quest 0006DFAF holds it in alias 17).
//    This is a byte search, so it can refuse an object nothing refers to, never the other way round.
//  - one whose change form could not be read to its end.
#pragma once
#include "PluginDB.h"
#include "Save.h"

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace ess::fo4world
{
	// A CELL record as the plugins have it (PluginDB::CellDef, the last override).
	struct CellRecord
	{
		bool          known = false;      // a loaded plugin defines the cell
		bool          interior = false;   // DATA bit 0
		bool          persistent = false; // a worldspace's persistent cell
		bool          hasGrid = false;    // XCLC
		std::uint32_t worldspace = 0;     // exterior: the worldspace
		int           x = 0, y = 0;       // exterior: grid position
		std::string   plugin;             // the last plugin that overrides it
	};

	// Names from the plugins (PluginDB): record type ("WEAP") and display name of a form id; either may be empty. cell: the
	// CELL record of a form id; cellAt: the exterior cell at (worldspace, x, y), 0 if none. Any of them may be empty.
	struct Lookup
	{
		std::function<std::string(std::uint32_t)> type, name;
		std::function<CellRecord(std::uint32_t)>  cell;
		std::function<std::uint32_t(std::uint32_t, int, int)> cellAt;
	};
	// All of the above from a loaded PluginDB (empty functions when a_db is null or not loaded). a_db must outlive it.
	Lookup LookupFrom(const PluginDB* a_db);

	struct Item
	{
		std::uint32_t formID = 0;  // FF......
		RefID         ref;
		std::uint32_t base = 0;     // the base object (runtime form id)
		std::string   baseType;     // its record type when known ("WEAP")
		std::string   kind;         // "Weapon", "Armor", "Junk", ... ("Other (STAT)", "Unknown" without plugin data)
		bool          item = false; // the base object is an item (something that can be picked up)
		std::string   name;         // the base object's name when known
		std::string   plugin;       // the mod of the base object ("Fallout4.esm", "Created" for a created base)
		std::uint32_t space = 0;    // interior: the cell; exterior: the worldspace (from the initial data)
		bool          exterior = false;
		int           gridX = 0, gridY = 0;  // exterior: the cell coordinates (position / 4096)
		float         pos[3] = {};
		std::int32_t  count = 1;    // ExtraCount (0x24) or 1
		bool          persistent = false;  // form flag 0x400
		bool          removable = false;
		std::string   why;          // why not removable
		std::size_t   bodySize = 0;
	};

	struct Listing
	{
		std::vector<Item> items;     // created references lying in the world (REFR change forms only)
		std::size_t       held = 0;  // created references that are in an inventory (extra 0x1C): not listed
		std::size_t       unread = 0;  // created REFRs whose reference part could not be read (listed, not removable)
	};

	// Every created reference (REFR change form with an FF id) lying in the world. a_lookup fills names and kinds.
	Listing List(const SaveFile& a_save, const Lookup& a_lookup = {});

	// "Weapon", "Armor", ... for a record type; a_item: something the player can carry
	std::string KindOf(const std::string& a_type, bool* a_item = nullptr);

	// Where an item is, for grouping: "<cell name or id>" or "<worldspace> (x, y)".
	std::string PlaceOf(const Item& a_item, const Lookup& a_lookup = {});

	struct RemoveResult
	{
		std::size_t              removed = 0;
		std::vector<std::string> refused;  // "FF001234: why"
	};
	// Removes the change forms of the given created references, each only if List() says it is removable (checked again
	// here on the save as it is now). Nothing else in the save changes.
	RemoveResult Remove(SaveFile& a_save, const std::set<std::uint32_t>& a_formIDs);
}
