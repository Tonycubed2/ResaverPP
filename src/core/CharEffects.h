// ReSaver++ - "Character effects": the player's active magic effects grouped by where they come from, what the
// character wears (a paper doll of equipment slots) and the effects that are stuck (an item's enchantment still
// running while no worn item carries it, or an effect whose mod is gone from the load order). Skyrim SE/AE/VR/LE and
// Fallout 4. Removal goes through the existing verified code (cf::RemovePlayerActiveEffects, fo4cf::RemoveActiveEffects).
//
// What is read and where it was checked:
//  - Each active effect's data starts (Skyrim ActiveEffect::SaveGame, SkyrimSE.exe 1.6.1170 vfunc 8 0x5acea0; LE TESV.exe
//    1.9.32.0 0x6568d0, the same order; Fallout4.exe 1.11.240 ActiveEffect::LoadGame 0xb74e90, see ChangeFormFO4.h) with
//    f32 elapsed (+0x70), f32 magnitude (+0x78), f32 duration (+0x74), u32 flags (+0x7c), then four 3-byte RefIDs: the
//    caster (from its handle +0x34), the target, ActiveEffect +0x68 and ActiveEffect +0x58 (Skyrim; LE +0x44 / +0x3c), the
//    last one being the item the effect came from ("source": the armor / weapon whose enchantment it is). Checked on saves:
//    every effect with flag 0x100 (and only those) has a source, and on Tony's saves every such source is a worn item
//    carrying that enchantment (trashgod69: 30 of 39; Fallout 4: all 12 on 12 saves).
//  - Worn: Skyrim inventory stacks with extra data 0x16 ExtraWorn / 0x17 ExtraWornLeft (GetType of their vtables in
//    SkyrimSE.exe 1.6.1170: 0x17bfb0 returns 0x16, 0x17c010 0x17). Fallout 4: the stack's u16 flags & 7 (Actor.IsEquipped,
//    native 0x10f9b30 -> 0x502380: an item is equipped when a stack's flags at +0x24 have one of the bits 0..2).
//  - Which slot: ARMO BOD2 / BODT first u32 (biped slots, bit 0 = slot 30), weapons in the hands (PluginDB).
//  - Timed effects past their duration are not reported: Skyrim's ActiveEffect::Update (0x5ae701..0x5ae7fd) counts an
//    effect's time only when flags & 0x4100 is 0, and ends such an effect itself (flag 0x40000) once elapsed >= duration;
//    effects with 0x4000 / 0x100 never run out by time (Tony's ordinary saves have such effects "past" their duration).
//    So a saved effect past its duration is not stuck by that alone.
#pragma once
#include "Save.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ess
{
	class PluginDB;
}

namespace ess::chfx
{
	enum class Group : std::uint8_t
	{
		kWorn,         // an enchantment of something the character wears
		kNotWorn,      // an item's enchantment, the item not worn (see stuck)
		kSpells,       // spells, powers, shouts, scrolls
		kConsumables,  // potions, food, chems, ingredients, poisons
		kAbilities,    // abilities (race, standing stone, perk abilities, mods' constant effects)
		kDiseases,     // diseases and addictions
		kOthers,       // put on the character by someone else (a hit, a spell cast at you)
		kUnknown,      // the magic item is not in the plugins read (plugin missing, or names not loaded)
		kCount
	};
	const char* GroupName(Group a_group, bool a_fo4 = false);  // English (the GUI translates)

	struct Effect
	{
		std::size_t   index = 0;       // in the game's list (ReadPlayerActiveEffects / DecodeActor().effects)
		std::uint32_t item = 0;        // magic item (spell, enchantment, potion...), runtime form id
		std::uint8_t  effectIndex = 0;
		std::uint32_t archetype = 0;
		float         elapsed = 0, magnitude = 0, duration = 0;
		std::uint32_t flags = 0;
		bool          hasRefs = false;  // the four refs were read (data of 28 bytes or more)
		std::uint32_t caster = 0, target = 0, source = 0;
		std::uint32_t mgef = 0;         // from the plugins (0: not known)
		std::int64_t  actorValue = -1;  // Skyrim: actor value index; Fallout 4: AVIF form id; -1 not known / none
		bool          stat = false;     // removing it changes a saved stat
		bool          canRemove = true;
		std::string   why;              // why not, or what removing it changes (English)
		// the same as parts, for the GUI's own (translated) text
		enum class Why : std::uint8_t
		{
			kNone,
			kStat,              // removing it changes: whyText (actor value name or AVIF id), statAmount, statSlot
			kPluginNotRead,     // a stat effect whose item's plugin (whyText) was not read
			kNamesNotLoaded,    // ... plugin names not loaded at all (whyText: the plugin)
			kMadeInGameUnknown, // ... an item made in game whose effects the save does not list
			kMgefNotRead,       // ... its magic effect (whyText "%08X") is in a plugin not read (whyText2)
			kRefused,           // Fallout 4: the removal plan refused it (why, English, from the core)
			kIncomplete,        // the player's data does not read completely (whyText: the error)
		};
		Why           whyKind = Why::kNone;
		std::string   whyText, whyText2;
		float         statAmount = 0;
		int           statSlot = -1;
		bool          off() const { return (flags & 0x8000u) != 0 || (flags & 0x40000u) != 0; }  // switched off / dispelled
		bool          fromItem() const { return (flags & 0x100u) != 0; }
	};

	struct Card  // one source: a magic item, and for enchantments the item carrying it
	{
		std::uint32_t item = 0, source = 0;
		Group         group = Group::kUnknown;
		std::vector<std::size_t> effects;  // View::effects indexes
		std::string   type;                // record type from the plugins ("ENCH", "SPEL"...), empty if not known
		std::uint8_t  spellType = 0xFF;
		std::string   plugin;              // SaveFile::pluginOf(item)
		bool          known = false;       // the item is in the plugins read
		bool          sourceWorn = false;
		std::size_t   running = 0;         // effects not switched off
		bool          removable = true;    // every effect of it can be removed
	};

	enum class Slot : std::uint8_t  // the doll's places (both games; each game uses its own)
	{
		kHead, kBody, kHands, kFeet, kAmulet, kRing, kShield, kRightHand, kLeftHand, kAmmo,  // Skyrim
		kChest, kLeftArm, kRightArm, kLeftLeg, kRightLeg, kNeck, kPipBoy, kWeapons,          // Fallout 4 (+ head, body, hands, ring)
		kOther,    // a slot the doll has no place for (tail, mod slots)
		kUnknown,  // worn, but its slots are not known (plugin not read)
		kCount
	};
	const char* SlotName(Slot a_slot);  // English
	const std::vector<Slot>& DollSlots(bool a_fo4);  // the places the doll draws, for that game

	struct Worn
	{
		std::uint32_t item = 0;
		std::int32_t  count = 0;
		bool          left = false;         // Skyrim ExtraWornLeft
		std::uint32_t enchantment = 0;      // the stack's own (made in game / by a script), else the base item's EITM
		std::uint32_t baseEnchantment = 0;  // EITM
		std::string   type;                 // ARMO, WEAP, AMMO, LIGH... (empty: not known)
		std::uint32_t biped = 0;
		Slot          slot = Slot::kUnknown;
		std::size_t   effects = 0;          // effects of the cards whose source this is
	};

	enum class Stuck : std::uint8_t
	{
		kNotWorn,     // an item's constant enchantment on the player, cast by the player, while no worn item is its source
		              // or carries that enchantment
		kPluginGone,  // the magic item's (or source item's) plugin is not in the current load order / not in the save's list
	};
	struct Finding
	{
		Stuck         kind = Stuck::kNotWorn;
		std::size_t   card = 0;            // View::cards index
		std::vector<std::size_t> effects;  // View::effects indexes
		std::size_t   running = 0;
		std::string   plugin;              // the plugin that is gone (kPluginGone)
		std::string   text;                // English, for the CLI, the Save check and the AI
	};

	// Skyrim's made-in-game enchantments, potions and poisons (FF forms): global data type 4 "Created Objects", four tables
	// (weapon enchantments, armor enchantments, potions, poisons), each vsval n x {RefID, u32 times used, vsval k x {RefID
	// MGEF, f32 magnitude, u32 duration, u32 area, f32 price}} (UESP's layout; read to the exact end of the block on
	// trashgod69's save (14843 bytes: 17 + 31 + 211 + 97 items) and Tony's Save88/89/132; FF005BE4's 18 effects match
	// the 18 active effects it gives, effect index 0..17). Empty when the block does not read exactly.
	struct CreatedItem
	{
		int                        table = 0;  // 0 weapon enchantment, 1 armor enchantment, 2 potion, 3 poison
		std::vector<std::uint32_t> effects;    // MGEF runtime form ids, by effect index
		std::vector<float>         magnitudes;
	};
	std::map<std::uint32_t, CreatedItem> CreatedObjects(const SaveFile& a_save);

	struct Options
	{
		const PluginDB*              db = nullptr;
		const std::set<std::string>* loadOrder = nullptr;  // lowercase active plugins (null: not known)
	};

	struct View
	{
		bool                 fo4 = false, le = false;
		std::string          error;      // empty when read
		bool                 removable = true;  // the player's record reads completely (removal possible)
		std::vector<Effect>  effects;
		std::vector<Card>    cards;      // grouped; sorted by group, then by effect count
		std::vector<Worn>    worn;
		std::vector<Finding> stuck;
		bool                 loadOrderChecked = false;
		bool                 loadOrderOther = false;  // not checked: the load order lacks most of the save's plugins
		std::string          loadOrderNote;  // why the load order part was not checked
		std::string          name(std::uint32_t a_id) const;  // in-game name, editor id, or the form id (+ "made in game")
		// the words name() uses for FF forms: "%08X (%s made in game)", "%08X (made in game)", then the four created kinds
		// (weapon enchantment, armor enchantment, potion, poison); the GUI puts translations here
		const char*          labels[6] = { "%08X (%s made in game)", "%08X (made in game)", "weapon enchantment", "armor enchantment", "potion", "poison" };
		const PluginDB*      db = nullptr;
		const SaveFile*      save = nullptr;
		std::map<std::uint32_t, std::string> created;  // FF forms the save has a change form for: their type ("ARMO")
		std::set<std::uint32_t>              carried;  // every item in the player's inventory
		std::map<std::uint32_t, CreatedItem> createdItems;  // Skyrim: CreatedObjects
		std::vector<std::string>             savePlugins;  // the save's plugin lists (full, then light)
		std::function<std::string(std::uint32_t)> pluginOf;  // set while classifying (SaveFile::pluginOf)
	};

	// Reads the player's effects and worn items from the save, then Classify.
	View Build(const SaveFile& a_save, const Options& a_options);
	// Groups the effects into cards, finds the worn items' slots and enchantments and the stuck effects, from
	// View::effects, worn, carried, savePlugins and pluginOf (the unit test fills these by hand).
	void Classify(View& a_view, const Options& a_options);

	// Removes these effects (View::effects indexes) through the game's verified removal; the player's record is read
	// again afterwards and must hold exactly that many fewer effects, else the change form is put back. a_changes: what
	// was changed. Returns "" or why not (then nothing changed).
	std::string Remove(SaveFile& a_save, const View& a_view, const std::vector<std::size_t>& a_effects, std::vector<std::string>* a_changes = nullptr);
}
