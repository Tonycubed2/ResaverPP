#pragma once

// Follower and beast form fixes in the running game (version 13), each done by the game's own scripts:
// - SetFollower: DialogueFollowerScript.SetFollower(actor) on the vanilla follower quest (DialogueFollower 000750BA):
//   out of DismissedFollowerFaction, relationship rank 3, SetPlayerTeammate, ForceRefTo the Follower alias (which
//   gives CurrentFollowerFaction / PlayerFollowerFaction while it holds the actor), PlayerFollowerCount 1.
//   Refused while a living actor is in the Follower alias (dismiss that one first: SETSTAGE 000750BA 400).
// - Teammate: Actor.SetPlayerTeammate(false) (or true) on one actor.
// - BeastRevert: the transformation script's own way back (PlayerWerewolfChangeScript / DLC1PlayerVampireChangeScript
//   .ShiftBack: the race from before, the claws / Vampire Lord powers, factions, armor, controls, beast form flag,
//   fast travel...) after its "already doing it" guards (__tryingToShiftBack, __shiftingBack, __shuttingDown) are
//   set back to false, so it runs to the end even when an earlier try stopped half way or the quest has stopped.
namespace SkyFix
{
	// all on the main thread; each returns "" when the call was handed to the game, else why not
	std::string SetFollower(RE::FormID a_actor);
	std::string Teammate(RE::FormID a_actor, bool a_on);
	std::string BeastRevert(const std::string& a_which);  // "werewolf" or "vampire"
}
