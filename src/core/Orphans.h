// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Script instances whose spawned object (FF......) no longer exists in the save, grouped by the
// mod that provides their script, so the player can see which come from mods that are gone and
// which from mods still installed (those may keep them on purpose).
#pragma once

#include "Save.h"
#include "ScriptSources.h"

#include <set>
#include <string>
#include <vector>

namespace ess::orphans
{
	enum class Status
	{
		kNotInstalled,  // the script's file is nowhere (or the game could not load the script)
		kDisabled,      // only in mod folders that are disabled in the profile
		kInstalled,     // provided by an enabled mod (or the game)
		kUnknown,       // the mods were not scanned, so it cannot be told
		kEngine         // an engine object type (ActorBase, ObjectReference...): the game's own link, no mod script
	};
	const char* StatusText(Status a_status);

	struct ScriptGroup
	{
		std::string   script;
		std::set<EID> ids;
		std::set<EID> held;  // ids that something else in the save still points to
	};

	struct Group
	{
		std::string              mod;  // mod folder providing the scripts, or a label
		Status                   status = Status::kUnknown;
		std::vector<ScriptGroup> scripts;
		std::size_t              instances = 0, held = 0;
	};

	// a_sources: null or not scanned -> only "not installed" (the game could not load the script) and
	// "unknown" can be told. Sorted: not installed, disabled, engine links, unknown, installed; then by mod name.
	std::vector<Group> Find(SaveFile& a_save, const ScriptSources* a_sources);

	// Deletes the given instances (and nothing else). Returns how many were deleted.
	std::size_t Remove(SaveFile& a_save, const std::set<EID>& a_ids);
}
