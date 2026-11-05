#pragma once

// Rescue tools for a running game that is stuck, done with the game's own console commands:
//   SaveGame:        save ReSaverPP_Rescue_<date>_<time>   (a new save, nothing overwritten)
//   RestoreControls: ForceEnablePlayerControls (fepc)       - the game: "Force-enables the player's controls,
//                    regardless of layers". Undone with ResetForceEnabledPlayerControls (ResetControls).
//   Gather:          cqf Followers "FollowersScript.TryToTeleportCompanion" player 1 1, and the same for
//                    Dogmeat: the vanilla follower system's own teleport (the one the game uses), so it brings
//                    the current companion and Dogmeat if they are with you, and does nothing otherwise.
// Holding Ctrl in the game for iRescueHoldSeconds (8) does the save, the controls and the companions.
namespace Rescue
{
	std::string SaveGame();        // main thread; returns the save's name
	void        RestoreControls();  // main thread
	void        ResetControls();    // main thread
	void        Gather();           // main thread

	// all three: the save and the controls first, the companions 3 seconds later (once the save is
	// written). NOT on the main thread: it waits for it. Returns one line saying what was done.
	std::string RunAll();

	// starts the thread that watches for Ctrl held down
	void StartHotkey();
}
