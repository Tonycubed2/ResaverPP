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
	Active Read(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_gameFolder, bool a_vr = false);

	// The game's own plugins.txt (%LOCALAPPDATA%\Skyrim Special Edition\plugins.txt, or Skyrim VR\plugins.txt).
	std::filesystem::path GamePluginsTxt(bool a_vr = false);
}
