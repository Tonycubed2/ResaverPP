#pragma once

// Follower and beast form fixes in the running game (version 13), the LE copy of helper\src\SkyFix.h: each is
// done by the game's own scripts, the same way as on SE.
// - SetFollower: DialogueFollowerScript.SetFollower(actor) on the vanilla follower quest (DialogueFollower 000750BA):
//   out of DismissedFollowerFaction, relationship rank 3, SetPlayerTeammate, ForceRefTo the Follower alias,
//   PlayerFollowerCount 1. Refused while a living actor is in the Follower alias (alias id 0; dismiss that one
//   first: SETSTAGE 000750BA 400).
// - Teammate: Actor.SetPlayerTeammate(on, true) on one actor.
// - BeastRevert: PlayerWerewolfChangeScript / DLC1PlayerVampireChangeScript .ShiftBack() after its "already doing
//   it" guards (__tryingToShiftBack, __shiftingBack, __shuttingDown: Bool script variables) are set back to false.
// Checked for LE: Skyrim.esm DialogueFollower 000750BA (alias 0 "Follower"), PlayerWerewolfQuest 0002BA16,
// Dawnguard.esm DLC1PlayerVampireQuest 0071D0; the scripts in Skyrim - Misc.bsa / Dawnguard.bsa (Dawnguard's
// PlayerWerewolfChangeScript replaces Skyrim's) have SetFollower(ObjectReference), SetPlayerTeammate(bool, bool),
// ShiftBack() and the three Bool variables. The script variable access is documented in Papyrus.h.
namespace SkyFix
{
	// all on the main thread; each returns "" when the call was handed to the game, else why not
	std::string SetFollower(std::uint32_t a_actor);
	std::string Teammate(std::uint32_t a_actor, bool a_on);
	std::string BeastRevert(const std::string& a_which);  // "werewolf" or "vampire"
}
