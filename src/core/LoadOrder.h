// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The player's current load order: which plugins the game will load next time. Used to find the
// mods a save still contains although they are no longer installed or enabled.
#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
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

	// Starfield: what the game loads next time and where the plugin files are.
	//  - its own files (StarfieldBaseFiles: the ones the save does not mark as Creations) are always loaded: Tony's install
	//    has no plugins.txt and its saves list exactly those 14;
	//  - %LOCALAPPDATA%\Starfield\plugins.txt, when there is one: lines with '*' are enabled (Starfield.exe's reader
	//    0x140425310; whether unmarked lines load is [U]); plugins Data\Starfield.ccc names (format [U]);
	//  - a plugin whose file is in none of the Data folders (the game's Data, then Documents\My Games\Starfield\Data) is
	//    not loaded.
	struct StarfieldOrder
	{
		bool                                                   pluginsTxtFound = false;
		std::filesystem::path                                  pluginsTxt;
		std::set<std::string>                                  active;  // lowercase
		std::unordered_map<std::string, std::filesystem::path> files;   // lowercase plugin name -> its file
		bool installed(const std::string& a_lower) const { return files.contains(a_lower); }
	};
	// Starfield: which list of the save a plugin file goes in, from its TES4 header (Starfield.exe 0x1405b70ac): 0 full, 1
	// light (flag 0x100, or a .esl file), 2 medium (0x400); -1 when it cannot be told (unreadable, or an overlay file: 0x200
	// without 0x100 / 0x400, which takes no slot of its own when it has masters)
	int StarfieldPluginKind(const std::filesystem::path& a_file);
	StarfieldOrder ReadStarfield(const std::filesystem::path& a_pluginsTxt, const std::vector<std::filesystem::path>& a_dataDirs);
	// Starfield: plugins whose kind (full / light / medium) is not the one the save has them as. When a save loads, each
	// saved form id is turned into today's (TESFile::AdjustFormID 0x1405b78c0, LOADER.md 2.5, PROVEN): a light plugin keeps
	// the low 12 bits of the id, a medium one the low 16, a full one 24. So a plugin made light (or medium) after the save
	// loses every saved id above 0xFFF (0xFFFF): those records land on other records of it, or on none.
	struct SfKindChange
	{
		std::string plugin;
		int         was = 0, now = 0;   // 0 full, 1 light, 2 medium (StarfieldPluginKind)
		std::size_t ids = 0;            // form ids of it in the save
		std::size_t outOfRange = 0;     // ... whose number does not fit the kind it has now
		std::uint32_t example = 0;      // one of those (as saved)
	};
	std::vector<SfKindChange> StarfieldKindChanges(const std::vector<std::string>& a_full,
		const std::vector<std::string>& a_lite, const std::vector<std::string>& a_medium, const std::vector<std::uint32_t>& a_formIDs,
		const StarfieldOrder& a_order);
	// the Data folders of a Starfield install: <game>\Data, then <My Games\Starfield>\Data when it exists (a_myGames empty:
	// %USERPROFILE%\Documents\My Games\Starfield)
	std::vector<std::filesystem::path> StarfieldDataDirs(const std::filesystem::path& a_gameFolder, const std::filesystem::path& a_myGames = {});

	// The game's own plugins.txt (%LOCALAPPDATA%\Skyrim Special Edition\plugins.txt, or Skyrim VR\plugins.txt).
	// a_le: the save is Legendary Edition, so its plugins.txt (%LOCALAPPDATA%\\Skyrim) comes first
	// a_game 2: Fallout 4's (%LOCALAPPDATA%\Fallout4\plugins.txt)
	std::filesystem::path GamePluginsTxt(int a_game = 0, bool a_le = false);
}
