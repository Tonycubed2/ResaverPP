// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Spawned actors whose base is another actor's temporary leveled base ("ghost bases"): the save names a base NPC
// it does not have, so on load the actor gets no base or the wrong one.
//
// How a leveled actor is built (verified in SkyrimSE.exe 1.6.1170, scratchpad skyf/exe, tools skyf/xdis.py,
// calls.py, xref.py, fn.py, vt.py):
// - Placing an NPC whose template is a leveled list (0x1d74e0): the game picks the template from the list, makes
//   a NEW form of the base's type (form factory 0x1bcb40; the TESForm constructor 0x1dd960 gives it the next
//   created id, TESDataHandler 0x1b8480: FF000800..FF3FFFFF, skipping only ids a live form has), copies the
//   placed base into it (vfunc 0x178), applies the template (TESActorBaseData vfunc 0x20), stores ExtraLeveledCreature
//   {+0x10 original base = the base that was placed, +0x18 template} (0x2fb0e0 -> 0x164140) and makes the copy the
//   actor's base (vfunc 0x420). The copy is made with the thread's "no change tracking" flag set (TLS +0x670 bit 0;
//   BGSSaveLoadManager::AddChange 0x604320 records nothing while it is set), so the copy never gets a change form of
//   its own. What changes on it is saved inside the actor's ExtraLeveledCreature (ReSaver's NPCChangeFlags / NPC).
// - Saving: the actor's initial base and its ExtraLeveledCreature original base are the ORIGINAL base: on Tony's
//   Save140, 197 of 458 created actors have ExtraLeveledCreature, every one with a plugin NPC there (BASE_OBJECT ==
//   LEVELED_BASE, e.g. Skyrim.esm LvlBanditMeleeCommonerM with template EncBandit01Melee1HImperialM). No save we have
//   holds a created NPC_ change form (Tony's Save88, Save132, Save140, SkyrimIstari's Save89, trashgod69's a.ess).
// - Loading ExtraLeveledCreature (ExtraDataList load 0x170380, case 0x2D at 0x1728ae..0x172a6a): both form ids are
//   looked up as they are at that moment (0x175000); with an original and a template the game builds a new copy
//   (0x1d7880 = the same copy + template steps), DELETES the actor's current base if its id is a created one
//   (0x1be0e0: id >= 0xFF000000; vfunc 0 with 1 at 0x172a1e) and sets the copy as the base. With no original the
//   extra data entry is removed (0x172a51) and the actor keeps whatever its initial base resolved to.
// - TESObjectREFR::ClearData (vtable index 5, 0x2d77b0) deletes the reference's own base when that base is a
//   created form and the reference has ExtraLeveledCreature (0x2d7825..0x2d784b): the copy dies with its owner.
// - Papyrus: Actor.GetLeveledActorBase (native 0x9e8980) returns the actor's base pointer (+0x40) when it is an NPC:
//   for a leveled actor that is the temporary copy. ObjectReference.GetBaseObject (native 0xa2d060) returns the
//   ExtraLeveledCreature original base when there is one, else +0x40, and Actor.GetActorBase is
//   "GetBaseObject() as ActorBase" (vanilla Actor.psc): the plugin's base.
//
// So PlaceAtMe(someActor.GetLeveledActorBase()) makes an actor whose original base is someone else's temporary copy
// (a created FF form). That copy is never saved and dies with its owner: in the session the new actor's
// ExtraLeveledCreature+0x10 then points at freed memory (its display name reads it: 0x2ea0e0), and in every save
// the actor names a created base id the save does not have. On load that id is free (no base: Actor::LoadGame and
// mods like SPID read garbage) or held by some other actor's new copy (the actor loads as that NPC, and the load
// above deletes that other actor's base, since it is "the actor's current base and a created form").
// Seen on Tony's saves: all of them made by Ultimate Deadly Encounters' clone spawns (waiting12, WalkingDangers49,
// SOT_Multiply: PlaceAtMe of Clone.GetLeveledActorBase()); Save88 13, Save132 2, Save140 4; none in other saves.
//
// Not removed from the save file: the player's record and the scripts that name such an actor would also need handling,
// and that was not proven safe. Removal is done in the running game instead (ReSaverHelper 15 GHOSTREMOVE: the
// console's disable + markfordelete on each, helper/src/GhostRemove.h), or by hand with the console (ConsoleText).
#pragma once

#include "Save.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ess::ghost
{
	struct Actor
	{
		std::uint32_t            refRaw = 0;           // the actor's change form RefID (raw)
		std::uint32_t            formID = 0;           // the actor (a created FF reference)
		std::uint32_t            base = 0;             // the base its initial data names (0 if none is saved)
		std::uint32_t            leveledBase = 0;      // ExtraLeveledCreature original base (0 if none)
		std::uint32_t            leveledTemplate = 0;  // ExtraLeveledCreature template (a plugin NPC)
		std::uint32_t            cell = 0;             // the cell its initial data names
		std::string              heldBy;               // the save has a change form of another type with that id ("" none)
		std::vector<std::string> plugins;              // plugins of the factions it was added to (who made it), base game left out
		std::size_t              scripts = 0;          // script instances on it
		// the created base id it names (the initial base, else the leveled one)
		std::uint32_t missing() const { return (base >> 24) == 0xFF ? base : leveledBase; }
	};

	// Created actors (FF) whose initial base or ExtraLeveledCreature original base is a created form the save has no
	// NPC change form for. Skyrim only (an empty list for Fallout 4).
	std::vector<Actor> Find(const SaveFile& a_save);
	// "FF003307 (base FF003327 not in the save; built from E50362F9, The Restless Dead.esp) in 0001527F, made by SOTFull.esp"
	std::string Describe(const SaveFile& a_save, const Actor& a_actor);

	// ---- removal in the running game ------------------------------------------------------------------------------
	// The console commands for doing it by hand, for each id: "prid FF003D2A", "disable", "markfordelete", one per line.
	std::string ConsoleText(const std::vector<std::uint32_t>& a_ids);
	// The helper's GHOSTREMOVE argument: "FF003D2A,FF0040DF".
	std::string RemoveArgument(const std::vector<std::uint32_t>& a_ids);
	struct RemoveResult
	{
		std::uint32_t id = 0;
		std::string   word;  // done, not found, not a created actor, not marked, or why the game did not answer
		bool          done() const { return word == "done"; }
	};
	// The reply fields after "OK": <count> then one "FF003D2A*done" per id. Empty when malformed.
	std::vector<RemoveResult> ParseRemoveReply(const std::vector<std::string>& a_fields);
}
