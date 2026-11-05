// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The player's current load order: which plugins the game will load next time. Used to find the
// mods a save still contains although they are no longer installed or enabled.
#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace ess::loadorder
{
	struct Active
	{
		bool                  known = false;  // a plugins.txt was found and read
		std::filesystem::path file;           // the plugins.txt that was read
		std::set<std::string> plugins;        // lowercase: enabled plugins plus the ones the game always loads
	};

	// a_pluginsTxt: Mod Organizer's profiles\<profile>\plugins.txt, or the game's own
	// %LOCALAPPDATA%\Skyrim Special Edition\plugins.txt. Lines starting with '*' are enabled.
	// a_gameFolder adds the plugins the game loads without listing them (the base game masters and
	// the Creation Club content named in Skyrim.ccc).
	// a_game: 0 Skyrim (SE/AE/LE), 1 Skyrim VR, 2 Fallout 4 (its masters and Fallout4.ccc instead)
	Active Read(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_gameFolder, int a_game = 0);

	// the plugins Fallout 4 loads without listing them in plugins.txt, in the game's order (lowercase)
	const std::vector<std::string>& Fallout4Masters();

	// The game's own plugins.txt (%LOCALAPPDATA%\Skyrim Special Edition\plugins.txt, or Skyrim VR\plugins.txt).
	// a_le: the save is Legendary Edition, so its plugins.txt (%LOCALAPPDATA%\\Skyrim) comes first
	// a_game 2: Fallout 4's (%LOCALAPPDATA%\Fallout4\plugins.txt)
	std::filesystem::path GamePluginsTxt(int a_game = 0, bool a_le = false);
}
