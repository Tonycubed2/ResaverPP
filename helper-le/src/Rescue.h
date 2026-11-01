#pragma once

// Rescue tools for a game that is running but stuck (version 12): bring every follower to the player, switch
// the player's controls back on, save the game. ReSaver++ asks for them over the pipe (GATHER, CONTROLS,
// SAVEGAME, RESCUE); holding Ctrl in the game for iRescueHoldSeconds (8) does all three without ReSaver++.
namespace Rescue
{
	// Every follower the running quests hold in an alias (the vanilla follower quest, DLC followers and the
	// follower mods' own systems) who follows the player (a teammate, in the follower faction or held by the
	// vanilla follower quest), alive, not listed in sGatherSkip: enabled if needed and moved (Papyrus MoveTo) to its
	// own spot in a half circle behind the player, loaded or not.
	// Returns their names. Main thread.
	std::vector<std::string> GatherFollowers();

	// Game.EnablePlayerControls: moving, fighting, menus, activating, looking around, sneaking, the journal.
	// Main thread.
	void RestoreControls();

	// Saves the game as ReSaverPP_Rescue_<date>_<time> (a new save, nothing overwritten). Returns its name.
	// Main thread.
	std::string SaveGame();

	// All three: the save and the controls first, then (3 seconds later, once the save is written) the
	// followers. Returns one line saying what was done. NOT on the main thread: it waits for it.
	std::string RunAll();

	// starts the thread that watches for Ctrl held down (at kDataLoaded)
	void StartHotkey();
}
