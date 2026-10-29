// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Crash log reader: the forms a crash log names (Crash Logger SSE/AE/VR, Trainwreck, .NET Script Framework),
// matched against the open save, so a crash on loading a save can be traced to the saved object behind it.
//
// The usual case: a spawned or leveled actor whose saved data no longer fits the load order crashes the game
// while the save loads. The log names the actor (an ACHR, e.g. FF0040DF) and often, first, its base NPC
// (e.g. FF000998). That base is rebuilt from its template every time the save loads and is never stored in
// the save, so searching the save for it finds nothing; the actor that uses it is what can be removed.
#pragma once

#include "Save.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ess::crashlog
{
	// one form the log names
	struct Form
	{
		std::uint32_t id = 0;
		std::string   type;   // as the log has it: "NPC (43)", "ActorCharacter (62)", "TESNPC*"...
		std::string   name;   // the log's Name / GetFullName, when given
		std::size_t   hits = 0;       // how many times the log names it
		bool          atCrash = false;  // in the call stack or the registers (what the game was working on), not just the stack memory
		std::size_t   firstLine = 0;    // 1-based line of the first mention
	};

	struct Log
	{
		bool                  ok = false;
		std::string           error;     // why it could not be read
		std::filesystem::path file;
		std::string           tool;      // "Crash Logger", "Trainwreck", ".NET Script Framework" or "" (unknown)
		std::string           time;      // CRASH TIME line, when given
		std::string           game;      // the game version line, when given
		std::string           exception; // the "Unhandled exception ..." line
		std::vector<Form>     forms;     // most relevant first: at the crash, then by hits
	};
	Log Read(const std::filesystem::path& a_file);
	Log Parse(const std::string& a_text);  // the same from text (tests)

	enum class Kind
	{
		kChanged,      // the save has a change form for it
		kRebuiltBase,  // a leveled actor's base NPC (not in the save): the actor is shown instead
		kTemporary,    // a created FF form the save does not have (made by the game after loading)
		kPlugin        // a plugin's form the save did not change
	};
	struct Finding
	{
		Form          form;
		Kind          kind = Kind::kPlugin;
		std::uint32_t refRaw = 0;   // the change form to look at / remove (the actor's, for kRebuiltBase); 0 if none
		std::uint32_t target = 0;   // that change form's form ID
		std::string   cfType;       // its change form type ("ACHR", "NPC_"...)
		bool          created = false;  // the change form is of a created (FF) form: a spawned or leveled object
		bool          suspect = false;  // the likely cause: a created actor the game was loading when it crashed
		std::size_t   scripts = 0;  // script instances attached to that change form's form
	};
	// a_leveled: the save's leveled actors (cf::ListLeveledActors), for the rebuilt bases; passed in because
	// reading them takes a moment on a big save
	struct LeveledBase
	{
		std::uint32_t base = 0, actorRefRaw = 0, actor = 0, templ = 0;
	};
	std::vector<Finding> Match(const Log& a_log, const SaveFile& a_save, const std::vector<LeveledBase>& a_leveled);
	std::vector<LeveledBase> LeveledBases(const SaveFile& a_save);  // every leveled actor's rebuilt base, from cf::ListLeveledActors
}
