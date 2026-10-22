// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Per-mod view of the scripts in a save.
#pragma once

#include "Save.h"
#include "ScriptSources.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ess::mods
{
	struct ScriptRow
	{
		std::string        script;
		std::set<EID>      instances;  // instances of this script on this mod's objects
		std::set<ThreadID> threads;    // threads of this script attributed to this mod
		std::string        provider;   // mod folder providing the .pex (when scanned)
		bool               engineType = false;  // Form/Actor/Quest... binding, not a mod script
	};

	enum class Kind
	{
		kPlugin,
		kLight,
		kCreated,     // objects spawned during play (FF......)
		kUnattached,  // instances attached to nothing
		kGlobal,      // threads with no owning object (quests, global functions) and no known provider
		kFolder       // MO2 mod folder without a plugin (script-only mods)
	};

	struct ModRow
	{
		std::string                      name;
		Kind                             kind = Kind::kPlugin;
		int                              index = -1;  // load index (full) or FE index (light)
		std::map<std::string, ScriptRow> scripts;     // keyed by lowercase script name
		std::size_t                      scriptCount = 0, instanceCount = 0, threadCount = 0;  // engine-type bindings excluded
		std::size_t                      providedInSave = 0;  // distinct scripts in this save whose .pex this mod provides
		std::set<std::string>            provided;            // their names
	};

	struct Table
	{
		std::vector<ModRow> rows;
		bool                hasSources = false;
	};

	// Attribution:
	//  - a script instance belongs to the plugin that defines the object it is attached to;
	//    for objects spawned during play, to the mod providing the script (if scanned);
	//  - a thread belongs to the plugin of the object running it (frame 0's "self"); a thread
	//    with no object goes to the mod that provides its script's .pex (if scanned), else Global.
	Table Build(SaveFile& a_save, const ScriptSources* a_sources);
}
