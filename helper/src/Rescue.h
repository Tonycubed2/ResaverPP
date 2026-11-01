#pragma once

// Rescue tools for a game that is running but stuck (version 12): bring every follower to the player, switch
// the player's controls back on, save the game. ReSaver++ asks for them over the pipe (GATHER, CONTROLS,
// SAVEGAME, RESCUE); holding Ctrl in the game for iRescueHoldSeconds (8) does all three without ReSaver++.
namespace Rescue
{
	// Every current follower (held by the vanilla follower quest, CurrentFollowerFaction rank 0 or more, or a
	// player teammate: the follower frameworks and DLC followers use those), alive, enabled, not a summon, not
	// a mod's helper parked in a dummy / holding cell, not listed in sGatherSkip: moved (Papyrus MoveTo) to its
	// own spot in a half circle behind the player, loaded or not. Returns their names. Main thread.
	// a_only: just these characters (picked in ReSaver Plus); nullptr: every follower not in sGatherSkip.
	std::vector<std::string> GatherFollowers(const std::vector<RE::FormID>* a_only);

	// every follower found, one record each: runtime id * Plugin.esp:localid * name * why * where * in sGatherSkip (0/1).
	// Main thread.
	std::vector<std::string> ListFollowers();

	// "Plugin.esp:0012C4" for a form (sGatherSkip's format)
	std::string KeyOf(const RE::TESForm* a_form);

	// Game.EnablePlayerControls (the console's EnablePlayerControls): moving, fighting, menus, activating,
	// looking around, sneaking, the journal. Main thread.
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
