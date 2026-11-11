// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Save bloat by mod: how many bytes and things of the save each plugin accounts for.
//  - Papyrus: script instances (by the plugin of the object they sit on; on a created object by the plugin of
//    its base object; on nothing or an unknown created object by the mod shipping the script, when scanned),
//    threads (by their first frame's object), arrays and struct instances (by the instance, thread or struct
//    that holds them);
//  - change forms of the plugin's own forms, by type;
//  - created forms (FF......), by the plugin of their base object (references; Skyrim's leveled actors by their
//    template) when it can be read;
//  - inventory entries, by the plugin of the item (their bytes are taken out of the holder's change form, so
//    nothing is counted twice).
// Sizes are the uncompressed bytes the game reads (change forms as decompressed, headers included).
// Fallout 4, Skyrim (SE/AE/VR/LE) and Starfield (its change form data is not decoded: created forms are not traced to
// the plugin of their base and inventories are not split out; Papyrus sizes are those of its own layout).
#pragma once

#include "Save.h"
#include "ScriptSources.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ess::bloat
{
	struct Count
	{
		std::size_t   count = 0;
		std::uint64_t bytes = 0;
		void          add(std::uint64_t a_bytes, std::size_t a_n = 1)
		{
			count += a_n;
			bytes += a_bytes;
		}
	};

	enum class RowKind
	{
		kPlugin,     // a full plugin of the save
		kLight,      // a light plugin of the save
		kFolder,     // a mod folder without a plugin of the save ("[name]"), from the scanned script sources
		kCreated,    // created objects whose base could not be told
		kNothing,    // instances attached to nothing, threads without an object
		kOther,      // a plugin the save does not list (should not happen)
		kMedium      // a medium plugin of the save (Starfield)
	};

	struct ModBloat
	{
		std::string name;
		RowKind     kind = RowKind::kPlugin;
		int         index = -1;  // load index (full) or light index
		Count       instances, threads, arrays, structs;
		std::map<std::string, Count> scripts;      // script instances per script (name as in the save)
		std::map<std::string, Count> threadScripts; // threads per script
		std::map<std::string, Count> changeForms;  // the plugin's own forms, per change form type
		std::map<std::string, Count> created;      // created forms whose base comes from this plugin, per type
		Count       inventory;                     // inventory entries of this plugin's items (count = entries)
		std::int64_t itemCount = 0;                // items in those entries

		std::uint64_t papyrusBytes() const { return instances.bytes + threads.bytes + arrays.bytes + structs.bytes; }
		std::uint64_t changeFormBytes() const;
		std::uint64_t createdBytes() const;
		std::uint64_t total() const { return papyrusBytes() + changeFormBytes() + createdBytes() + inventory.bytes; }
		std::size_t   changeFormCount() const;
		std::size_t   createdCount() const;
	};

	// what to do about a cause, as a link to an existing tool
	enum class Tool
	{
		kNone,
		kModsView,      // the Mods view (scripts, instances, threads of each mod)
		kPurgeScript,   // Purge a script (detail = the script)
		kOrphans,       // Missing spawned objects (instances on created objects that are gone)
		kUnattached,    // Remove unattached instances
		kArrays,        // Analyze arrays block
		kMissingMods,   // Mods no longer installed (the plugin is not in the load order)
		kLeftovers,     // Leftover items
		kFindNpcs,      // Find NPCs (created actors)
		kHavok,         // Reset Havok (references with saved physics)
		kCompare        // Compare to another save
	};

	struct Cause
	{
		std::string   mod;     // the row's name
		std::string   what;    // "instances", "threads", "arrays", "structs", "changeforms", "created", "inventory"
		std::string   detail;  // script name, change form type...
		std::size_t   count = 0;
		std::uint64_t bytes = 0;
		std::string   text;    // plain words: "12,000 script instances of Foo (3.1 MB)"
		std::string   advice;  // what can be done
		Tool          tool = Tool::kNone;
		bool          game = false;   // the game's own plugin (base game, DLC, Creation Club): big numbers there are normal
		bool          engine = false; // a script instance of an engine type (ObjectReference, Actor...): the game's own link
	};

	struct Report
	{
		bool                  fo4 = false;
		std::vector<ModBloat> mods;  // biggest first
		std::uint64_t         bodyBytes = 0;      // the save's uncompressed body as written
		std::uint64_t         papyrusBytes = 0;   // the whole Papyrus block
		std::uint64_t         changeFormBytes = 0; // every change form (headers + decompressed data)
		std::uint64_t         attributed = 0;     // what the rows add up to
		std::uint64_t         papyrusOther = 0;   // strings, script definitions, queued calls, suspended stacks...
		std::size_t           decodeFailures = 0; // references whose body could not be read (their inventory and base are not split out)
		std::size_t           createdUnknown = 0; // created forms whose base could not be told
		std::vector<Cause>    causes;             // the biggest causes, biggest first

		const ModBloat* find(const std::string& a_name) const;
	};

	struct Options
	{
		const ScriptSources* sources = nullptr;  // scanned mods: attributes scripts on nothing / unknown objects to their mod
		std::size_t          maxCauses = 25;
		std::atomic<bool>*   cancel = nullptr;
		std::atomic<std::size_t>* done = nullptr;   // progress: change forms read
		std::atomic<std::size_t>* total = nullptr;
	};

	Report Build(const SaveFile& a_save, const Options& a_opt = {});

	// Growth between an older and a newer save of the same character, per row (matched by name, any case).
	struct Growth
	{
		std::string   mod;
		std::int64_t  oldBytes = 0, newBytes = 0;
		std::int64_t  oldInstances = 0, newInstances = 0;
		std::int64_t  oldCreated = 0, newCreated = 0;
		std::int64_t  oldChangeForms = 0, newChangeForms = 0;
		std::int64_t  oldThreads = 0, newThreads = 0;
		std::int64_t  delta() const { return newBytes - oldBytes; }
	};
	// Every row whose size changed, the biggest growth first.
	std::vector<Growth> Compare(const Report& a_older, const Report& a_newer);

	// "12,345" / "3.1 MB"
	std::string Num(std::uint64_t a_n);
	std::string Bytes(std::uint64_t a_n);

	// A plain text report (CLI, log, AI): totals, the top a_rows rows, the causes.
	std::vector<std::string> Describe(const Report& a_report, std::size_t a_rows = 30);
	std::vector<std::string> DescribeGrowth(const std::vector<Growth>& a_growth, std::size_t a_rows = 20);
}
