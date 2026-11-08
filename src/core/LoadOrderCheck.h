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
//
// The order the game really loads (GameOrder below), checked in the three exes:
//  - SkyrimSE.exe 1.6.1170: the file list is rebuilt in 0x1ba4d0: the base game masters, the Creation Club files of
//    Skyrim.ccc, then every MASTER (TESFile flags +0x438 & 1, IsMaster 0x1c8d10; .esm/.esl set it, see above) in
//    plugins.txt order (0x1ba8fc), then masters not listed, then every other plugin in plugins.txt order (0x1baa7a),
//    then the rest. Then 0x1b8f50 walks the list: for each master, a master of it found further down is taken out and
//    put just before it (repeated for the moved file when it is a master too). plugins.txt itself is read by
//    0x1bb410 (lines starting '#' skipped, '*' = active) and written by 0x1bafa0 ("%s%s\n", '*' for active files).
//  - Fallout4.exe 1.11.240: the same in 0x2e9020 (masters from plugins.txt 0x2e9418, other masters 0x2e94dd, the rest
//    0x2e9598 / 0x2e965d; IsMaster 0x2f9160 = flags +0x334 & 1) and the same hoisting in 0x2e78a0; reader 0x24d760.
//  - TESV.exe 1.9.32.0 (Legendary Edition): the Data folder is listed (0x43e330: *.esm then *.esp) with masters before
//    plugins, each group by file time (CompareFileTime at 0x43e549); then 0x6971a0 reads plugins.txt (every line is an
//    active plugin, no '*') and rebuilds the list: masters in plugins.txt order, other masters (file time order),
//    plugins in plugins.txt order, the rest; then the same hoisting (0x43c1d0, called at 0x697576). IsMaster 0x4446d0
//    is the header flag alone (flags +0x3e8 & 1): no code there tests a .esm extension. So file times only order the
//    plugins plugins.txt does not list (inactive ones).
// So a master (ESM flag, or .esm/.esl in SE/FO4) listed after a plugin that needs it is not late: the game loads every
// master before every plain plugin, and a master's own masters are moved up before it. Only a plain plugin needing a
// plain plugin listed after it really loads too early.
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
		int                      slot = -1;        // load index (full) or light index, in the order the game loads (GameOrder)
		int                      loadPos = -1;     // position in the order the game loads (GameOrder), -1 not found
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
		std::string lateMaster;  // set when the finding is "plugin needs lateMaster, which loads after it" (Fix the order)
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

	// ---- the order the game loads, and fixing late masters ------------------------------------------------------------

	struct OrderEntry
	{
		std::string              name;
		bool                     master = false;  // the game's master flag (PluginInfo::esm)
		std::vector<std::string> masters;
	};
	// The positions of a_listed (the enabled plugins in plugins.txt order) in the order the game loads them: masters
	// before plain plugins (each group keeps the listed order), then each master's masters that come after it moved just
	// before it (SkyrimSE.exe 0x1ba4d0 + 0x1b8f50, Fallout4.exe 0x2e9020 + 0x2e78a0, TESV.exe 0x6971a0 + 0x43c1d0).
	std::vector<std::size_t> GameOrder(const std::vector<OrderEntry>& a_listed);

	// Plugins that need a master which the game loads after them (in GameOrder): {plugin index, master index} into a_listed.
	std::vector<std::pair<std::size_t, std::size_t>> LateMasters(const std::vector<OrderEntry>& a_listed);

	struct Move
	{
		std::string plugin;      // the master that is moved
		std::size_t from = 0;    // its position in the listed order before this move
		std::string before;      // it is put just before this plugin (the first one that needs it)
		std::size_t beforeAt = 0;  // that plugin's position before this move
		std::string fromLabel, beforeLabel;  // "#99": set by PlanFix(Result)
	};
	struct FixPlan
	{
		bool                     ok = false;
		std::string              error;   // why nothing can be planned (a cycle, a master the game would still load late)
		std::vector<std::string> order;   // the new listed order
		std::vector<Move>        moves;   // in the order they are applied
	};
	// Moves each late master to just before the first plugin (in the listed order) that needs it, then the masters that
	// master needs which are now late, until nothing is late; every other plugin keeps its order. A cycle of masters
	// (A needs B, B needs A) is refused. Pure: no files are read.
	FixPlan PlanFix(const std::vector<OrderEntry>& a_listed);
	// The same for a load order check's result (plugins whose file was not found or could not be read count as plain
	// plugins without masters). The moves are labelled "#n" with n = the plugin's position among a_enabledLines (the
	// enabled plugins as plugins.txt lists them, counting from 0: lofix::EnabledLines), else with the check's slot.
	FixPlan PlanFix(const Result& a_result, const std::vector<std::string>& a_enabledLines = {});
	std::string MoveText(const Move& a_move);  // "MysticismMagic.esp: #99 -> before 360 Ward.esp #87"
}
