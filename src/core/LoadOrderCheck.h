// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Load order limits: the plugins of the load order (plugins.txt / Mod Organizer profile, in order) and their headers,
// checked against what the game can load.
//
// Fallout 4 (checked in Fallout4.exe 1.11.240; the same code is in 1.10.163 and 1.10.984):
//  - a plugin is light when its header has flag 0x200 or its file name ends in .esl: the file opener (RVA 0x2f6217)
//    keeps the header's flags & 0x391, then sets 1 (master) for .esm/.esl and 0x200 (light) for .esl (0x2f62ba);
//    IsLight (0x2f91a0) is flag 0x200 alone, so an ESL-flagged .esp or .esm is light too;
//  - the data handler (0x2e6dc0) puts a light file in the light list with index = its position there and load index
//    0xFE, every other file in the full list with load index = its position; a form id is 0xFE000000 | light index << 12
//    | (object id & 0xFFF) for a light file, load index << 24 | object id otherwise (0x2f91c0). So: full plugins take
//    0x00..0xFD (254; 0xFE is the light block, 0xFF the forms made in game) and light plugins 0x000..0xFFF (4096);
//  - a light plugin's object ids keep only their low 12 bits, so new records numbered above 0xFFF collide with others;
//    below 0x800 is allowed for a plugin with header version 1.0 and at least one master (xEdit dev-4.1.6,
//    wbDefinitionsFO4: HardcodedRangeAdmitted, HardcodedRangeMinVersion 1.0; the game only masks the id);
//  - header version (HEDR) above 1.0: the plugin and its dependants are not loaded (0x2f67e0 / 0x2f7360 compare
//    with 1.0 and clear the active flags 0x4 and 0x8).
// Skyrim (from xEdit dev-4.1.6 wbDefinitionsTES5, not checked in SkyrimSE.exe): SE/AE 254 full + 4096 light, HEDR 1.71
// (1.6.1130+) at most, object ids below 0x800 in light plugins from HEDR 1.71 with a master; Legendary Edition: no
// light plugins, 255 plugins (0xFF is for forms made in game), plugins in Special Edition's format (record version 44)
// are a mistake.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ess::locheck
{
	enum class Game
	{
		kSkyrimSE,  // SE/AE (and VR with light support)
		kSkyrimLE,
		kFallout4
	};

	struct Limits
	{
		std::size_t   maxFull = 254;
		std::size_t   maxLight = 4096;
		bool          light = true;          // the game has light plugins
		float         maxHedr = 1.0f;        // newest header version the game loads
		float         extendedRangeHedr = 1.0f;  // header version from which light plugins may number from 0x001
	};
	Limits LimitsFor(Game a_game);

	struct PluginInfo
	{
		std::string              name;
		std::filesystem::path    path;  // empty: not found
		std::string              error;  // could not be read
		bool                     esm = false, eslFlag = false, eslExt = false, light = false;
		float                    hedr = 0;
		std::uint16_t            formVersion = 0;  // the header record's version
		std::vector<std::string> masters;
		int                      slot = -1;        // load index (full) or light index
		std::size_t              newRecords = 0;   // records the plugin adds (counted for light plugins only)
		std::size_t              overRange = 0;    // ... numbered above 0xFFF
		std::size_t              underFloor = 0;   // ... numbered below 0x800 where that is not allowed
		std::uint32_t            maxObjectID = 0;
	};

	enum class Severity
	{
		kInfo,
		kWarning,
		kError
	};
	struct Finding
	{
		Severity    severity = Severity::kInfo;
		std::string plugin;  // empty: the whole load order
		std::string text;
	};

	struct Result
	{
		Game                    game = Game::kFallout4;
		Limits                  limits;
		std::vector<PluginInfo> plugins;  // in load order
		std::size_t             full = 0, light = 0, missing = 0;
		std::vector<Finding>    findings;  // errors first
		bool                    hasErrors() const;
		std::size_t             count(Severity a_s) const;
	};

	struct Options
	{
		Game                     game = Game::kFallout4;
		std::vector<std::string> loadOrder;  // enabled plugins in load order (MergedPatch::ReadLoadOrder)
		std::function<std::optional<std::filesystem::path>(const std::string&)> findPlugin;
	};

	Result Check(const Options& a_opt);

	// one plugin's header (and, with a_scanRecords, its records' form ids). Throws on an unreadable file.
	PluginInfo ReadHeader(const std::filesystem::path& a_file, bool a_scanRecords, Game a_game);

	const char*              SeverityWord(Severity a_s);
	std::vector<std::string> Describe(const Result& a_result);
}
