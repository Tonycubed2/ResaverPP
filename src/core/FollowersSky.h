// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Skyrim followers and beast forms as a save has them: who follows the player (the vanilla follower quest's
// alias, the follower factions, the teammate flag), followers left half dismissed or lost somewhere, the follower
// count the vanilla script keeps, and a player left as a werewolf or a Vampire Lord (or with what the
// transformation adds still on). Plus the few fixes that are plain, verified edits of the save; everything else
// is done in the running game through ReSaverHelper with the game's own scripts.
//
// Verified against (2026-11-06):
// - DialogueFollowerScript.psc / FollowerAliasScript.psc (Skyrim SE Data\Source\Scripts) and the DialogueFollower
//   quest (Skyrim.esm / Update.esm 000750BA): alias 0 "Follower" (alias factions CurrentFollowerFaction 05C84E,
//   PlayerFollowerFaction 084D1B, WIFollowerCommentFaction 0750B8), alias 1 "Animal"; script properties
//   pPlayerFollowerCount = PlayerFollowerCount 0BCC98, pPlayerAnimalCount = PlayerAnimalCount 05C84A,
//   pDismissedFollower = DismissedFollowerFaction 05C84C, pCurrentHireling = CurrentHireling 0BD738; stage 400 runs
//   Fragment_5 = DismissFollower() (QF_DialogueFollower_000750BA).
// - SkyrimSE.exe 1.6.1170: Actor.IsPlayerTeammate (native 0x9F5500) = bit 26 of Actor+0xE8, the u32 the save writes
//   right after the process level (Actor::SaveGame 0x6867FE; the ACHR "BOOL BITS"). Actor::SetPlayerTeammate 0x68D690
//   flips that bit, sets/clears bit 7 (can do favor) of Actor+0x204 (the "BOOL FLAGS" written after the movement
//   controller, 0x686CAA) and counts PlayerCharacter+0xA10 up/down; that count is NOT saved (PlayerCharacter::SaveGame
//   0x73A2F0 writes +0xA14 on) and Actor::FinishLoadGame 0x687720 counts it again from each actor's bit (0x687881).
// - Actor::GetFactionRank 0x68A8A0: the reference's ExtraFactionChanges first, then the base NPC's factions;
//   Actor::RemoveFromFaction 0x68B470: a unique NPC's change goes to its base NPC (rank -1, NPC_ change form
//   ACTOR_BASE_FACTIONS), any other's to the reference's ExtraFactionChanges.
// - ActorState save 0x6CEA60: u16, u8 sit/sleep, u8 fly, u8 life state (0 alive, 1 dying, 2 dead, ...), ...
// - PlayerWerewolfChangeScript.psc + PlayerWerewolfQuest 0002BA16 (stage 0 PrepShift, 1 InitialShift, 10/11/20,
//   100 ShiftBack), DLC1PlayerVampireChangeScript.psc + DLC1PlayerVampireQuest (Dawnguard 0071D0: the same stages,
//   100 ShiftBack), CompanionsHousekeepingScript.PlayerOriginalRace (C00 04B2D9), DLC1VampireTrackingQuest.PlayerRace.
#pragma once

#include "Save.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ess::skyf
{
	// Skyrim.esm forms (Skyrim.esm is load order index 00 in every Skyrim save, so these are runtime ids too)
	inline constexpr std::uint32_t kDialogueFollower = 0x000750BA;
	inline constexpr std::uint32_t kCurrentFollowerFaction = 0x0005C84E;
	inline constexpr std::uint32_t kPlayerFollowerFaction = 0x00084D1B;
	inline constexpr std::uint32_t kPotentialFollowerFaction = 0x0005C84D;
	inline constexpr std::uint32_t kDismissedFollowerFaction = 0x0005C84C;
	inline constexpr std::uint32_t kCurrentHireling = 0x000BD738;
	inline constexpr std::uint32_t kPlayerFollowerCount = 0x000BCC98;
	inline constexpr std::uint32_t kPlayerAnimalCount = 0x0005C84A;
	inline constexpr std::uint32_t kPlayerWerewolfQuest = 0x0002BA16;
	inline constexpr std::uint32_t kCompanionsC00 = 0x0004B2D9;
	inline constexpr std::int32_t  kFollowerAlias = 0, kAnimalAlias = 1;
	inline constexpr int           kDismissStage = 400;    // DialogueFollower: DismissFollower()
	inline constexpr int           kShiftBackStage = 100;  // PlayerWerewolfQuest / DLC1PlayerVampireQuest: ShiftBack()
	inline constexpr std::uint32_t kTeammateBit = 1u << 26;  // ACHR "BOOL BITS"
	inline constexpr std::uint32_t kCanDoFavorBit = 1u << 7;  // ACHR "BOOL FLAGS"

	// ---- global variables (save table 1, type 3: vsval count, then { RefID, f32 }) ----
	struct GlobalValue
	{
		std::uint32_t formID = 0;
		float         value = 0;
		std::size_t   at = 0;  // offset of the f32 in the block
	};
	std::vector<GlobalValue> ReadGlobals(const SaveFile& a_save, std::string* a_error = nullptr);
	// sets a global the save already has (in place, 4 bytes); "" when done, else why not
	std::string SetGlobal(SaveFile& a_save, std::uint32_t a_formID, float a_value);

	// ---- followers ----
	struct FactionRank
	{
		std::uint32_t faction = 0;
		int           rank = 0;  // -1: taken out of it
	};
	struct FollowerActor
	{
		std::uint32_t refRaw = 0, formID = 0, base = 0;  // base: from a_baseOf (0 if unknown)
		bool          decoded = false;                   // the actor's own data was read (state, teammate flag)
		bool          teammate = false;
		std::size_t   boolBitsAt = 0;
		std::vector<std::pair<std::uint32_t, std::int32_t>> aliases;  // quest form id, alias id
		std::vector<FactionRank> refFactions, baseFactions;            // the reference's / its base NPC's saved changes
		bool          inFollowerAlias = false, inAnimalAlias = false;
		bool          disabled = false;
		int           lifeState = -1;  // 0 alive, 1 dying, 2 dead, 3 unconscious, ... (-1 not read)
		bool          dead() const { return lifeState == 1 || lifeState == 2; }
		bool          hasPosition = false;
		std::uint32_t cell = 0;  // the cell (interior) or worldspace (exterior)
		float         pos[3]{};
		// the rank the game would give: the reference's change, else its base NPC's change; -2 when the save has
		// neither (the plugin's own rank stands)
		int           rank(std::uint32_t a_faction) const;
		bool          inFaction(std::uint32_t a_faction) const { return rank(a_faction) >= 0; }
	};

	enum class Issue
	{
		kCountWithoutFollower,    // PlayerFollowerCount 1, nobody alive in the Follower alias ("you already have a follower")
		kFollowerWithoutCount,    // somebody in the Follower alias, PlayerFollowerCount 0
		kAnimalCountWithoutAnimal,
		kDeadInAlias,             // the alias still holds a dead follower (OnDeath did not clear it)
		kDisabledInAlias,         // the follower in the alias is disabled
		kFollowerAway,            // the follower in the alias is not where the player is
		kDismissedInAlias,        // in the Follower alias and in DismissedFollowerFaction
		kDismissedTeammate,       // dismissed (DismissedFollowerFaction) but still a teammate, in no follower alias: half dismissed
		kStrayTeammate,           // a teammate no quest alias holds, not in CurrentFollowerFaction: maybe a mod's, maybe left over
		kFactionWithoutAlias,     // CurrentFollowerFaction rank >= 0 but in no quest alias (a lost follower)
		kAliasNotTeammate,        // in the Follower alias but not a teammate
		kAliasMismatch,           // the quest's alias list and the actor's own alias list disagree
	};
	struct Finding
	{
		Issue         issue{};
		std::uint32_t actor = 0;  // form id (0: about the quest itself)
		std::string   text;       // plain explanation
	};

	struct FollowerState
	{
		std::string                error;
		bool                       questInSave = false, questRunning = false;  // DialogueFollower's change form
		bool                       hasCount = false, hasAnimalCount = false;
		float                      followerCount = 0, animalCount = 0;
		bool                       scriptFound = false;  // DialogueFollowerScript's instance
		int                        iFollowerDismiss = -1;
		std::uint32_t              playerCell = 0;
		// the quest's own record of its filled aliases (QUST change form, QUEST_RUN_DATA: { alias id, flag 0, reference }), which
		// the save keeps next to each reference's ExtraAliasInstanceArray (both agree in every save checked)
		bool                       hasRunData = false;
		std::vector<std::pair<std::int32_t, std::uint32_t>> runAliases;
		std::vector<FollowerActor> actors;  // every actor with a follower sign (alias, follower factions, teammate)
		std::vector<Finding>       findings;
		const FollowerActor*       inAlias(std::int32_t a_alias) const;  // the living one, else any
	};
	using BaseOf = std::function<std::uint32_t(std::uint32_t a_ref)>;
	FollowerState ReadFollowers(const SaveFile& a_save, const BaseOf& a_baseOf = {});

	// What can be fixed in the file (each a verified edit), and what only the running game can do.
	// PlayerFollowerCount (or PlayerAnimalCount) to the value the script would leave: 0 once the alias is empty
	// (DismissFollower's and FollowerAliasScript.OnDeath's last step), 1 with somebody alive in it (SetFollower).
	std::string FixFollowerCount(SaveFile& a_save, bool a_animal = false, std::string* a_done = nullptr);
	// Clears an actor's teammate flag as SetPlayerTeammate(false) does (bit 26 off, can-do-favor bit 7 on). The
	// player's teammate count is not saved, the game counts it again on load. The actor is read back completely
	// before and after, else nothing is changed.
	std::string ClearTeammate(SaveFile& a_save, std::uint32_t a_actorFormID);

	// ---- beast forms ----
	enum class Beast
	{
		kWerewolf,
		kVampireLord
	};
	struct BeastState
	{
		Beast         kind = Beast::kWerewolf;
		std::string   questEdid;          // PlayerWerewolfQuest / DLC1PlayerVampireQuest
		std::uint32_t quest = 0;          // its form id (0: the quest is not in this game, e.g. no Dawnguard)
		bool          questInSave = false, questRunning = false;
		std::vector<std::int16_t> stagesDone;
		bool          scriptFound = false;  // the change script's instance
		// the script's own guards (-1 unknown)
		int           trackingStarted = -1, tryingToShiftBack = -1, shiftingBack = -1, shuttingDown = -1;
		std::uint32_t beastRace = 0;        // WerewolfBeastRace / VampireLordRace (script property)
		std::uint32_t originalRace = 0;     // what ShiftBack sets again (C00 PlayerOriginalRace / tracking quest PlayerRace)
		std::uint32_t playerRace = 0;       // the player's race in the save (0: not read)
		std::string   playerRaceFrom;       // where it was read
		bool          raceIsBeast = false;
		std::vector<std::uint32_t> spells;    // abilities the transformation adds that the player still has
		std::vector<std::uint32_t> factions;  // PlayerWerewolfFaction / WerewolfFaction / PlayerVampireFaction still joined
		std::vector<std::uint32_t> items;     // the transition skin / Vampire Lord armor still carried
		bool          hasShiftBackTime = false;
		float         shiftBackTime = 0, gameDaysPassed = 0;
		int           levitateState = -1;  // DLC1: DCL1VampireLevitateStateGlobal (0 not a Vampire Lord, 1 walking, 2 flying)
		bool          hasRevertPower = false;  // DLC1: DLC1Revert ("Revert Form") among the player's added spells
		bool          untimed = false;         // the script's Untimed property
		// what is wrong (empty: nothing) and what fixes it
		enum class Fix
		{
			kNone,
			kSetStage,     // the quest runs and only needs stage 100 (what the script's own OnUpdate / Revert() does)
			kGameRevert,   // ReSaverHelper 13 BEASTREVERT: the script's ShiftBack() after its guards are reset
			kCannot        // the script has no race to go back to (PlayerOriginalRace / PlayerRace is None)
		};
		Fix           fix = Fix::kNone;
		std::string   problem;  // plain explanation
		bool          stuck() const { return fix != Fix::kNone; }
		bool          leftovers() const { return !spells.empty() || !factions.empty() || !items.empty(); }
	};
	std::vector<BeastState> ReadBeastForms(const SaveFile& a_save);

	// ---- quests stuck between stages (only what the save proves) ----
	struct StuckStage
	{
		std::uint32_t quest = 0;
		int           stage = -1;     // the stage whose fragment is still running (-1 not known)
		std::string   kind;           // "fragment running" / "shutdown stage done, still running"
		std::string   topFrame;       // fragment running: the function the thread was in ("utility.Wait")
		std::uint32_t thread = 0;
		std::string   text;
	};
	// a_shutdownStages(quest) -> its stages flagged shut-down in the plugin (empty: not known)
	using StagesOf = std::function<std::vector<int>(std::uint32_t a_quest)>;
	std::vector<StuckStage> FindStuckStages(const SaveFile& a_save, const StagesOf& a_shutdownStages = {});
}
