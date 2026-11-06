// ReSaver++ - Fallout 4 companions: who they are, where the save has them and what the companion scripts know
// about them (affinity, romance, whether they left for good), and what does not add up between the parts.
//
// Everything is read from the save itself; nothing about a companion is hard coded:
//  - the companions are the actors with a CompanionActorScript instance (or a script extending it; mod companions
//    use the same system) and Dogmeat-type companions (DogmeatActorScript);
//  - the follower quest is the quest carrying the FollowersScript instance (vanilla: Followers 000289E4); the forms
//    the scripts use (CurrentCompanionFaction, DisallowedCompanionFaction, the CA_Affinity actor value, ...) are read
//    from that instance's variables, and the actor value behind "following" (FollowerState) from the
//    CommonPropertiesScript instance;
//  - the alias the quest's "Companion" / "DogmeatCompanion" / "ActiveCompanions" properties point at is the
//    ReferenceAlias / RefCollectionAlias object those variables hold: its instance carries the alias id in the field
//    the save stores after the form (checked: Companion 0, DogmeatCompanion 5, ActiveCompanions 6, CommandActor 10,
//    AvailableMessageActor 15, LastPlayerVertibird 16 = the ALST/ALCS ids of Followers in Fallout4.esm);
//  - who fills an alias is the quest's own data (QUST change flag 28, TESQuest::LoadGame, Fallout4.exe 1.11.240 RVA
//    0x66cd80..0x66d1c5: per entry u32 alias id (looked up with 0x6785f0), u8 created flag, then for flag 0 one form
//    id: the reference whose handle is stored as that alias's fill);
//  - which aliases an actor is in is the actor's own extra data 0x88 ExtraAliasInstanceArray (loader 0x2ab7c0: vsval
//    count, per entry a form id cast to TESQuest and a u32 alias id looked up with 0x66e040);
//  - faction changes made in game are extra data 0x5b ExtraFactionChanges (0x295ce9: vsval count, per entry a
//    TESFaction form id and an i8 rank; then a TESFaction form id and a u8). A rank below 0 is a removal.
//    CurrentCompanionFaction itself comes from the Companion / DogmeatCompanion alias (ALFC 00023C01 on both in
//    Fallout4.esm), so the game gives it to whoever fills those aliases; it is not saved for them.
#pragma once
#include "Save.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ess::fo4comp
{
	// FollowerState values: the constant globals iFollower_Com_Follow / _Wait / _GoHome (Fallout4.esm 0002A106,
	// 0002A107, 0002A108, record flag 0x40 "constant", FLTV 1, 2, 4) that FollowersScript.FollowerFollow / FollowerWait
	// / DismissCompanion put into the actor value
	constexpr float kFollow = 1.0f, kWait = 2.0f, kGoHome = 4.0f;

	struct FollowerSystem
	{
		bool          found = false;     // a FollowersScript instance on a quest was found
		std::uint32_t quest = 0;         // Followers
		std::string   questScript;       // the script's name as the save has it
		bool          questDecoded = false;  // the quest's change form was read completely
		bool          questRunning = false;
		std::int32_t  aliasCompanion = -1, aliasDogmeat = -1, aliasActive = -1;  // alias ids (-1 unknown)
		std::uint32_t companionFill = 0, dogmeatFill = 0;  // the references in those aliases (0: empty)
		std::vector<std::uint32_t> active;  // the ActiveCompanions collection
		// forms the scripts use (0 when the variable is empty)
		std::uint32_t factionCurrent = 0, factionHasBeen = 0, factionDisallowed = 0;
		std::uint32_t avAffinity = 0, avThreshold = 0, avHighest = 0, avLowest = 0, avIsRomantic = 0, avRomanceableNow = 0;
		std::uint32_t avFollowerState = 0;  // CommonPropertiesScript.FollowerState
	};

	struct FactionChange
	{
		std::uint32_t faction = 0;
		std::int8_t   rank = 0;  // below 0: removed from it
	};

	struct Companion
	{
		std::uint32_t ref = 0;           // the actor's reference
		std::string   script;            // CompanionActorScript (or the script extending it), DogmeatActorScript
		bool          dogmeat = false;   // a DogmeatActorScript actor
		std::uint64_t instance = 0;      // its script instance

		// the reference (ACHR change form)
		bool          hasChangeForm = false;
		bool          refRead = false;   // its reference part was read
		bool          actorRead = false; // its actor values were read
		std::uint32_t cell = 0;          // where the save has it (a cell or a worldspace); 0 unknown
		float         pos[3] = {};
		std::vector<std::pair<std::uint32_t, std::uint32_t>> aliases;  // ExtraAliasInstanceArray: (quest, alias id)
		std::vector<FactionChange> factions;                           // ExtraFactionChanges

		// actor values the scripts use (has* false: no saved base value, the plugin's value applies)
		bool  hasAffinity = false, hasThreshold = false, hasFollowerState = false, hasRomantic = false, hasRomanceable = false;
		float affinity = 0, threshold = 0, followerState = 0, romantic = 0, romanceable = 0;

		// CompanionActorScript variables (has* false: not found / not this script)
		bool  scriptRead = false;
		bool  hasLeftPermanently = false;  // HasLeftPlayerPermanently
		bool  perkUnlocked = false;        // InfatuationPerkUnlocked
		bool  infatuated = false;          // LastThresholdCrossed is the InfatuationThreshold global (IsInfatuated())
		bool  hasRange = false;
		float minAffinity = 0, maxAffinity = 0;  // MinAffinity / MaxAffinity

		// where it stands
		bool inCompanionAlias = false;  // the quest's Companion (or DogmeatCompanion for Dogmeat) alias holds it
		bool inActive = false;          // in the ActiveCompanions collection
		bool aliasExtraSaysCompanion = false;  // its own alias list names the Companion / DogmeatCompanion alias
		bool disallowed = false;        // DisallowedCompanionFaction (rank 0 or more) in its faction changes
		bool hasBeen = false;           // HasBeenCompanionFaction (rank 0 or more) in its faction changes

		bool current() const { return inCompanionAlias; }
		bool following() const { return hasFollowerState && followerState == kFollow; }
	};

	struct Finding
	{
		enum class Level
		{
			kInfo,
			kCaution,
			kProblem
		} level = Level::kInfo;
		std::uint32_t ref = 0;  // the companion (0: the follower system as a whole)
		std::string   title, what;
		std::vector<std::string> console;  // console commands for the fix in the running game (one per line), empty: none
		std::string   consoleWhy;          // what the commands do
	};

	struct Result
	{
		FollowerSystem         system;
		std::vector<Companion> companions;  // in script instance order
		std::vector<Finding>   findings;
		std::vector<std::string> notes;     // what could not be read
		const Companion*       find(std::uint32_t a_ref) const;
	};

	// Reads everything above. Fallout 4 saves only (empty for others).
	Result Read(const SaveFile& a_save);

	// Console text for the running game (Fallout4.exe's own console commands, checked in the command tables of
	// 1.10.163, 1.10.984 and 1.11.240: CallFunction "cf" 0x028b with a reference, CallQuestFunction "cqf" 0x028c).
	// The function arguments are converted by the game (0x602590 / 0x60aa80): a form is a hex form id, "player" or
	// "none"; a bool is false for "0", "false" or "", else true.
	//  - affinity through the companion's own script (CompanionActorScript.SetAffinity -> ModAffinity: thresholds,
	//    scenes and perk as in normal play): <ref>.cf "<script>.SetAffinity" <value>
	std::string SetAffinityCommand(const Companion& a_c, float a_value);
	//  - make it a companion again the way the game does (FollowersScript.AllowCompanion with ForceCompanion: leaves
	//    DisallowedCompanionFaction, then SetCompanion / SetDogmeatCompanion by race), first clearing
	//    HasLeftPlayerPermanently when it is set (CompanionActorScript.UnsetHasLeftPlayerPermanently).
	//    a_quest: how the console names the follower quest ("Followers", or its form id).
	std::vector<std::string> MakeCompanionCommands(const Companion& a_c, const std::string& a_quest);

	// Sets the companion's affinity in the save: the saved base of the CA_Affinity actor value, and like
	// CompanionActorScript.ModAffinity also CA_HighestReached / CA_LowestReached when the new value passes them (when
	// those are saved). In place; the actor is decoded again before the change is kept. Refused when the companion
	// has no saved affinity (the plugin's value applies until the scripts first set it) or the value is outside the
	// script's MinAffinity..MaxAffinity. Thresholds, threshold scenes, the companion perk and quest stages are not run:
	// the scripts do that the next time the affinity changes in game. Returns "" or why not (then nothing changed).
	std::string SetAffinity(SaveFile& a_save, std::uint32_t a_ref, float a_value);
}
