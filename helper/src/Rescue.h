#pragma once

// Rescue tools for a game that is running but stuck (version 12): bring every follower to the player, switch
// the player's controls back on, save the game. ReSaver++ asks for them over the pipe (GATHER, CONTROLS,
// SAVEGAME, RESCUE); holding Ctrl in the game for iRescueHoldSeconds (8) does all three without ReSaver++.
namespace Rescue
{
	// Every current follower (CurrentFollowerFaction rank 0 or more, or a player teammate: the follower
	// frameworks and DLC followers use that), alive, enabled, not a summon, moved next to the player,
	// loaded or not. Returns their names. Main thread.
	std::vector<std::string> GatherFollowers();

	// Game.EnablePlayerControls (the console's EnablePlayerControls): moving, fighting, menus, activating,
	// looking around, sneaking, the journal. Main thread.
	void RestoreControls();

	// Saves the game as ReSaverPP_Rescue_<date>_<time> (a new save, nothing overwritten). Returns its name.
	// Main thread.
	std::string SaveGame();

	// All three: save, controls, followers. Returns one line saying what was done. Main thread.
	std::string RunAll();

	// starts the thread that watches for Ctrl held down (at kDataLoaded)
	void StartHotkey();
}
