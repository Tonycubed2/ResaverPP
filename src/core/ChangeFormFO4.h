// ReSaver++ - Fallout 4 change form decoding: REFR, the reference part of ACHR (the player too) and NPC_.
// The layouts follow the game's own loaders in Fallout4.exe 1.11.240 (TESObjectREFR::LoadGame 0x4f5c50,
// Actor::LoadGame 0xc8abd0, ExtraDataList::LoadGame 0x294760, the inventory loader 0x34ee00,
// TESNPC::LoadGame 0x65bae0 / TESActorBase 0x644dd0; RVAs) and were checked byte for byte on real saves.
// Every part keeps the byte range it was read from, so an edit can replace exactly that range.
#pragma once
#include "Save.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ess::fo4cf
{
	struct Range
	{
		std::size_t begin = 0, end = 0;  // [begin, end) in the decompressed body
		std::size_t size() const { return end - begin; }
	};

	struct Extra
	{
		std::uint8_t type = 0;
		Range        range;  // the type byte and its data
	};

	struct Stack  // one stack of an inventory item
	{
		std::vector<Extra> extras;  // the stack's own extra data (object mods, health, ...)
		std::int32_t       count = 0;
		std::uint16_t      flags = 0;
		Range              range;  // the extra list, count and flags
		Range              countRange;  // the i32 count alone
	};

	struct Item
	{
		RefID              item;
		std::uint32_t      formID = 0;
		std::int32_t       count = 0;  // sum over the stacks
		std::vector<Stack> stacks;
		Range              range;
	};

	struct Part
	{
		const char* name = "";  // initial, havok, actor prefix, form flags, base object, scale, extra data,
		                        // inventory, animation, actor
		Range       range;
	};

	struct Refr
	{
		bool        decoded = false;  // the reference part was read without an error
		bool        complete = false; // ... and, for a REFR, the whole body was read (an ACHR has its actor part left)
		std::string error;
		bool        actor = false;
		std::vector<Part> parts;

		// initial data (bits 1/2/3/25, or a created reference): cell or worldspace, position, rotation.
		// The 27 bytes are read as one block by the game; that they hold cell + 3 + 3 floats is the Skyrim
		// layout and matches the values seen (not traced further in the exe).
		bool          hasInitial = false;
		RefID         cell;
		float         pos[3] = {}, rot[3] = {};
		RefID         startCell;  // bit 3 / 25: the cell it started in (initial type 6)
		RefID         base;       // a created reference's base object (from its initial data), or bit 7
		bool          createdBase = false;  // base came from the created reference's initial data
		bool          hasScale = false;
		float         scale = 1.0f;
		std::uint32_t formFlags = 0;  // bit 0

		std::vector<Extra> extras;
		bool               hasInventory = false;
		std::vector<Item>  inventory;
		Range              refrPart;  // the reference part as a whole (an ACHR's actor part follows it)
	};

	struct Npc
	{
		bool        decoded = false;
		bool        complete = false;
		std::string error;
		std::vector<Part> parts;  // flags, ACBS, factions, spells, AI data, name, gender, skills, class, race,
		                          // face, body morphs, outfit, sleep outfit
		std::string name;         // bit 5
	};

	// ---- the actor part of an ACHR (after its reference part) ----
	struct ActiveEffect
	{
		RefID         item;            // the magic item (spell, potion, enchantment, ...)
		std::uint8_t  effectIndex = 0; // which of the item's effects
		std::uint32_t archetype = 0;   // the effect archetype the data was saved for
		Range         range;           // the whole entry
		Range         data;            // the effect's own data (ActiveEffect::LoadGame), skipped by size
	};
	struct ActorValue
	{
		RefID         av;        // the actor value (AVIF); SPECIAL is 0x2C2 (Strength) .. 0x2C8 (Luck)
		std::uint32_t formID = 0;
		float         base = 0;
		Range         range;     // ref + f32
		Range         baseRange; // the f32 alone
	};
	struct ActorValueMod
	{
		RefID         av;
		std::uint32_t formID = 0;
		float         mod[3] = {};  // the three modifier slots (temporary, permanent, damage)
		Range         range;
	};
	struct Actor
	{
		bool          decoded = false;  // read up to and including the actor values
		std::string   error;
		std::size_t   stoppedAt = 0;    // where reading stopped (end of the actor values when decoded)
		std::uint32_t formID = 0;
		std::int32_t  processLevel = -1;  // 0 high .. 3 low, -1 no AI process
		Range         processRange;
		Range         effectsRange;     // end of the active effect list (begin 0 when there is none)
		std::vector<ActiveEffect>  effects;
		Range                      valuesRange;  // the actor value storage (0xcedc80)
		std::vector<ActorValue>    values;       // base values
		std::vector<ActorValueMod> modifiers;
	};
	// Reads the actor part up to and including the actor values (the rest of the body is not decoded yet).
	// a_trace (tests): receives (position, kind) of every read.
	Actor DecodeActor(const SaveFile& a_save, const ChangeForm& a_cf, std::vector<std::pair<std::uint32_t, char>>* a_trace = nullptr);
	// Rewrites an actor value's saved base in place; the body is decoded again before the change is kept.
	std::string SetActorValueBase(SaveFile& a_save, ChangeForm& a_cf, std::uint32_t a_avif, float a_value);
	// The player's SPECIAL: 0 Strength .. 6 Luck.
	std::string SetPlayerSpecial(SaveFile& a_save, int a_index, float a_value);

	// The body is decompressed here; a_cf must be a REFR / ACHR (Refr) or NPC_ (Npc) of a Fallout 4 save.
	Refr DecodeRefr(const SaveFile& a_save, const ChangeForm& a_cf);
	Refr DecodeRefr(const SaveFile& a_save, const ChangeForm& a_cf, const std::vector<std::uint8_t>& a_body);
	Npc  DecodeNpc(const SaveFile& a_save, const ChangeForm& a_cf);

	// The player's reference (ACHR 0x14).
	const ChangeForm* FindPlayer(const SaveFile& a_save);
	// Caps the player carries (item 0x0000000F in the player's inventory); -1 when the inventory cannot be read.
	std::int64_t PlayerCaps(const SaveFile& a_save);

	// Sets the caps the player carries: the first stack of item 0x0000000F gets a_count, the other caps stacks 0
	// (counts are rewritten in place, so nothing else moves). The body is decoded again afterwards and must give
	// a_count. Returns "" or what went wrong (then the save is unchanged).
	std::string SetPlayerCaps(SaveFile& a_save, std::int32_t a_count);

	// What a change flag of a REFR / ACHR makes the game read (short text), or "" when it reads nothing itself.
	const char* RefrFlagMeaning(int a_bit, bool a_actor);

	struct Coverage
	{
		std::size_t refr = 0, refrComplete = 0, achr = 0, achrRefrPart = 0, npc = 0, npcComplete = 0;
		std::vector<std::pair<std::string, std::size_t>> errors;  // most frequent first
	};
	Coverage Stats(const SaveFile& a_save);
}
