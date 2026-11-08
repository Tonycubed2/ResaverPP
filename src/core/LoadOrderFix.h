// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Writing a fixed load order (locheck::PlanFix) to the files the mod manager and the game read, with backups.
//
// Where the order lives (checked in the sources / exes, see docs/CLAUDE-HANDOFF.md "Fix the order"):
//  - Mod Organizer 2 (modorganizer 2.5.2 + modorganizer-game_bethesda): the profile's loadorder.txt (every plugin,
//    enabled or not) and plugins.txt (Skyrim SE / Fallout 4: every plugin but the base ones, '*' = enabled; Skyrim LE:
//    the enabled ones). On a refresh MO2 reads the order from loadorder.txt when it is newer than its last read
//    (GamebryoGamePlugins::readPluginLists), else from plugins.txt; it reads them again after a program it started
//    ends (OrganizerCore::afterRun -> refreshESPList) and on Refresh (F5), and writes both from memory on every change
//    and when it closes (OrganizerCore::~OrganizerCore -> saveCurrentProfile -> savePluginList). lockedorder.txt
//    ("name|load index") pins plugins to a load index (PluginList::refreshLoadOrder).
//  - Vortex (extension-plugin-management): its load order IS %LOCALAPPDATA%\<game>\plugins.txt + loadorder.txt
//    (PluginPersistor is the 'loadOrder' state's persistor); it watches that folder and reads them again when another
//    program changes them (startWatch -> deserialize). Skyrim LE ('original' format) takes the order from loadorder.txt.
//  - No mod manager: the game reads %LOCALAPPDATA%\<game>\plugins.txt only (SkyrimSE.exe 0x1bb410, Fallout4.exe
//    0x24d760, TESV.exe 0x6971a0); a loadorder.txt there is LOOT's / Wrye Bash's and is kept in step when present.
#pragma once

#include "LoadOrderCheck.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ess::lofix
{
	// Applies the moves to one plugins.txt / loadorder.txt: each moved plugin's line is taken out and put just before the
	// line of the plugin it goes before. Every other line (comments, disabled plugins, '*' marks, spacing) stays as it was,
	// and so do the line endings (CRLF or LF), a byte order mark and the last line's newline (or its absence). A move
	// whose plugin or target is not in the file is skipped (listed in a_skipped). Names compare without case.
	std::string ApplyMoves(const std::string& a_text, const std::vector<locheck::Move>& a_moves, std::vector<std::string>* a_skipped = nullptr);

	// MO2's lockedorder.txt ("Plugin.esp|index", '#' comments): the plugins whose load index (position among the
	// enabled plugins) the new order changes. MO2 would put them back where they are locked, so the fix is refused.
	struct Locked
	{
		std::string plugin;
		int         index = -1;     // where it is locked
		int         newIndex = -1;  // where the new order puts it
	};
	std::vector<Locked> LockedMoved(const std::string& a_lockedOrderText, const std::vector<std::string>& a_oldOrder,
		const std::vector<std::string>& a_newOrder);

	enum class Manager
	{
		kNone,    // the game's own plugins.txt
		kMo2,     // a Mod Organizer profile
		kVortex,  // Vortex (the game's plugins.txt, which is Vortex's load order)
	};

	struct Targets
	{
		Manager                            manager = Manager::kNone;
		std::vector<std::filesystem::path> files;        // written in this order (plugins.txt first, loadorder.txt last)
		std::filesystem::path              lockedOrder;  // MO2 lockedorder.txt (empty if none)
	};
	// a_pluginsTxt: the plugins.txt the check read (the MO2 profile's, or the game's own). loadorder.txt next to it is
	// changed too when it exists; for MO2 lockedorder.txt is read.
	Targets TargetsFor(const std::filesystem::path& a_pluginsTxt, Manager a_manager);

	struct FileResult
	{
		std::filesystem::path file, backup;
		bool                  changed = false;
		std::string           before, after;  // the file's text before and after
		std::vector<std::string> skipped;     // moves not in this file
	};
	struct WriteResult
	{
		bool                    ok = false;
		std::string             message;
		std::vector<FileResult> files;
	};
	// Checks lockedorder.txt, then for each file: reads it, applies the moves, and (unless a_dryRun) backs it up next to
	// itself as <file>.<YYYYmmdd-HHMMSS>[-n].resaverpp.bak, writes a temp file, renames it over the original and reads
	// it back. A file that fails is put back from its backup and the files written before it too.
	WriteResult Write(const Targets& a_targets, const locheck::FixPlan& a_plan, const std::vector<std::string>& a_oldOrder, bool a_dryRun);

	// A short unified-style diff of two texts (only the lines that differ, with a line of context): for the CLI and logs.
	std::string Diff(const std::string& a_before, const std::string& a_after, const std::string& a_name);

	// What the user has to know (or do) for the change to stick with this mod manager. a_underMo2: ReSaver Plus was
	// started from Mod Organizer.
	std::string AfterWriteNote(Manager a_manager, bool a_underMo2, bool a_mo2Running);
}
