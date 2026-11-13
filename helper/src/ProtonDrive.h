#pragma once

// Proton game drive guard (version 18). Under Steam's Proton the game is started from the drive letter Proton makes for
// the Steam library (S:, the "gamedrive" option: pfx/dosdevices/s: -> the library folder). Proton's launch script
// (proton_9.0 / proton_10.0, the "run" verb: setup_game_dir_drive -> setup_dir_drive) sets that symlink again on EVERY
// launch into the same prefix, and REMOVES it when the program launched has no "gamedrive" option (a non-Steam shortcut,
// a script running "proton run": ReSaver++, BodySlide, xEdit...). From then on the running game cannot open any file it
// has not opened yet through S:\ paths (menus' interface files: the Journal, Map and Skills menus crash on a null movie).
// Found with SkyrimIstari (Linux Mint, 1.6.640), Nov 2026.
//
// Only under Wine (ntdll exports wine_get_version) and only when the game runs from a drive other than C: or Z:: at start
// the helper reads where that drive points (readlink of $WINEPREFIX/dosdevices/<letter>:, run through /bin/sh and written
// to a file, as a Wine program cannot read a Linux program's output through a pipe), then checks every 2 s that the game's
// own SkyrimSE.exe can still be found there. When it cannot, it puts the link back (ln -sfn <the same folder>), logs it and
// tells the player once. Checked in Wine 9.0: CreateProcess of Z:\bin\sh runs it, the new link is seen at once.
// bFixProtonGameDrive=0 turns it off.
namespace ProtonDrive
{
	void Start();  // at plugin load (before other programs can be started into the prefix)

	// for the F1 page / log: "" when not under Proton, else one line on what the guard watches and has done
	std::string Status();
}
