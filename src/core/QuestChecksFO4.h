// ReSaver++ - Fallout 4 save check: quests whose last stage script stopped before it did its work.
//
// For each running, not completed quest of the save: every stage the save has done whose stage script (the quest's
// fragment script, from the plugin's VMAD) is installed is read (Fallout 4 .pex, little-endian, the format of
// Caprica / Champollion). Only the fragment's straight-line start is used (the instructions before its first jump or
// return: they always run), and in it only calls on the quest itself with a number literal:
//   SetStage(n)                      -> stage n must be done,
//   SetObjectiveDisplayed(n[, true]) -> objective n must be shown, completed or failed (state 1..5),
//   SetObjectiveCompleted(n[, true]) -> objective n must be completed (state 2 or 3),
// and only when no other function of that script sets the same stage / objective (then nothing in the quest's own
// script can have undone it) and the plugin defines that stage / objective. A quest whose script is running or queued
// in the save (a thread or a queued call on that script) is skipped: its fragment may simply not be finished yet.
// What is left is a stage whose script started (the stage is done) and stopped before a line that always runs: the
// quest cannot go on by itself (usually a form the script needs is gone, or a mod's script error).
//
// The scripts come from every archive and loose Scripts folder ScriptSources scanned: for the game's Data folder that is
// "Fallout4 - Misc.ba2" (the base game) and each DLC's "<DLC> - Main.ba2" (DLCRobot, DLCworkshop01..03, DLCCoast,
// DLCNukaWorld: Scripts\DLC01..DLC06\...; a namespaced script "DLC03:Fragments:Quests:QF_..." is that path).
// Checked on Tony's real Data folder (F:\SteamLibrary\steamapps\common\Fallout 4\Data): every stage script of the running
// quests of the base game and all six DLCs was found in the test saves but one the game itself does not ship
// (Fragments:Quests:QF_DiamondCitySuperMutantInt_001A8A0E, named by Fallout4.esm's VMAD, not in Fallout4 - Misc.ba2).
// Tried on 10 saves (about 2,400 running quests; with the base game's scripts only, 8 to 38 lines checked per save): no
// finding. The same rules in a Python prototype over the same saves gave the same counts.
#pragma once
#include "Quests.h"
#include "Save.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ess
{
	class PluginDB;
	class ScriptSources;
}

namespace ess::pexfo4
{
	// one instruction: the opcode name and its arguments as text (identifiers as they are, strings quoted, numbers)
	struct Instr
	{
		std::string              op;
		std::vector<std::string> args;   // fixed arguments
		std::vector<std::string> extra;  // call arguments (callmethod / callparent / callstatic)
	};
	struct Script
	{
		std::string                                   name, parent;
		std::map<std::string, std::vector<Instr>>     functions;  // lower-case name -> body (every state)
	};
	// Throws std::runtime_error when the data is not a Fallout 4 .pex
	Script Read(const std::vector<std::uint8_t>& a_data);
}

namespace ess::fo4quest
{
	struct Finding
	{
		enum class Kind
		{
			kStageNotSet,
			kObjectiveNotShown,
			kObjectiveNotCompleted
		} kind = Kind::kStageNotSet;
		std::uint32_t quest = 0;
		std::string   edid;
		std::uint16_t stage = 0;       // the done stage whose script stopped
		std::string   function;        // its fragment
		std::int32_t  target = 0;      // the stage / objective it sets
		std::int32_t  state = -1;      // the objective's state in the save (-1: not saved)
		std::string   title, what;
		std::vector<std::string> console;  // console commands for the running game
	};
	struct Stats
	{
		std::size_t quests = 0, scriptsRead = 0, scriptsMissing = 0, skippedRunning = 0, checks = 0;
		// per plugin of the quest (Fallout4.esm, DLCRobot.esm, DLCCoast.esm, ...): stage scripts read / not found, lines checked
		std::map<std::string, std::size_t> readBy, missingBy, checksBy;
		std::vector<std::string>           missing;  // the stage scripts not found (none of the scanned folders or archives has them)
	};
	// one quest against its fragment script (a_checks: += the stage / objective lines checked)
	std::vector<Finding> CheckQuest(const quests::QuestState& a_state, const quests::QuestDef& a_def, const pexfo4::Script& a_script,
		std::size_t* a_checks = nullptr);
	std::vector<Finding> FindStoppedStages(const SaveFile& a_save, const PluginDB& a_db, const ScriptSources& a_sources, Stats* a_stats = nullptr);
}
