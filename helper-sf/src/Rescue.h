#pragma once

// Rescue tools for a running game that is stuck, done with the game's own console commands (all of them are in
// Starfield.exe 1.16.244's console command list):
//   SaveGame:        save ReSaverPP_Rescue_<date>_<time>   (a new save, nothing overwritten)
//   RestoreControls: ForceEnablePlayerControls (fepc). Undone with ResetForceEnabledPlayerControls (ResetControls).
// Holding Ctrl in the game for iRescueHoldSeconds (8) does the save and the controls.
namespace Rescue
{
	std::string SaveGame();         // main thread; returns the save's name
	void        RestoreControls();  // main thread
	void        ResetControls();    // main thread

	// both: NOT on the main thread (it waits for it). Returns one line saying what was done.
	std::string RunAll();

	// starts the thread that watches for Ctrl held down
	void StartHotkey();
}
