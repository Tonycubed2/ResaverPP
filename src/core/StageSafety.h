// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Stage safety (Skyrim): before Transplant runs a quest stage in the game, what would that stage do? The stage's fragment
// (QUST VMAD: script QF_<edid>_<formid>, one function per stage / log entry) is read from its compiled script (.pex) and
// walked instruction by instruction, following what it sets off: SetStage / Stop of its own quest (the other stage, the
// shut-down stages), Start / SetStage of other quests (their start-up stages and that stage, up to a few quests deep) and
// calls into the quest's own scripts. Calls that act on the player or the world in a way a transplant must not repeat are
// reported: moving the player (MoveTo, SetPosition...), playing a scene (Scene.Start / ForceStart), switching the controls
// off, fading the screen out, starting a fight, placing new actors, switching fast travel or saving off, opening the race
// menu or playing a video. What a stage would give the player (AddItem, AddPerk, AddSpell...) and the quests it would start
// are listed too, so the user knows what marking the quest as done instead leaves out.
//
// What the game runs (SkyrimSE.exe 1.6.1170): Quest.SetStage (Papyrus SetCurrentStageID, native 0xa46fd0) starts the quest
// first when it is not running (0x3c9bf0: start-up stages, INDX flag 0x02, run by 0x3c9a60) and then sets the stage
// (0x3c9d00); stopping a quest runs its shut-down stages (INDX flag 0x04, 0x3c9af0). Quest.CompleteQuest (native 0xa46900 ->
// 0x3c9570) only sets the completed flag (0x2), clears the active flag and sends the completed event: it runs no stage.
#pragma once

#include "Pex.h"
#include "Quests.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ess::stagesafety
{
	struct Env
	{
		// a quest's record by runtime form ID (nullptr: not a quest / not in the plugins)
		std::function<const quests::QuestDef*(std::uint32_t)> quest;
		// a compiled script by name (nullptr: not found); throws std::exception when it cannot be read
		std::function<const pex::File*(const std::string&)> script;
	};

	enum class Kind
	{
		kMovesPlayer,
		kScene,
		kControls,
		kFade,
		kCombat,
		kSpawn,
		kFastTravel,
		kSaving,
		kMenu,
		kVideo,
		kHoldsPlayer,
	};
	const char* KindText(Kind a_kind);  // "moves you", "plays a scene", ...

	struct Finding
	{
		Kind        kind = Kind::kMovesPlayer;
		std::string call;   // "Game.GetPlayer().MoveTo(Alias_DA15BluePalaceReturnMarker.GetRef())"
		std::string where;  // "DA15 stage 200 > DA15Return.SetStage(0) > DA15Return stage 10"
		// a scene, fight or new actors of a quest it starts (not of the quest itself): that quest starting the way it normally
		// does, listed but not counted as unsafe. What acts on the player (moves you, controls, fade, saving...) always counts.
		bool        followUp = false;
		bool        blocking() const { return !followUp; }
	};

	struct Report
	{
		enum class State
		{
			kSafe,        // read: nothing that moves the player, plays a scene...
			kUnsafe,      // read: it would (findings)
			kNotChecked,  // a script could not be found or read (notChecked says which)
		} state = State::kNotChecked;
		std::vector<Finding>     findings;
		std::vector<std::string> rewards;    // "AddItem(Razor)": what running it would give the player
		std::vector<std::string> followUps;  // "DA15Return.SetStage(0)": quests it would start or move on
		std::vector<std::string> notRead;    // scripts it calls that could not be read (not followed)
		std::string              notChecked; // why it could not be checked
		std::size_t              functions = 0;  // functions read

		bool        unsafe() const { return state == State::kUnsafe; }
		// "moves you: Game.GetPlayer().MoveTo(...) (DA15Return stage 10); plays a scene: ..." (at most a_max findings that count)
		std::string what(std::size_t a_max = 3) const;
		std::string notes(std::size_t a_max = 3) const;  // the same for the follow-up findings that do not count
		std::string firstKind() const;  // "moves you" (the first counting finding's kind); empty when none
	};

	// What setting a_stage of quest a_quest (runtime form ID) would do. a_stage < 0: none (only the start). a_starts: the quest
	// is not running in the game, so the stage (or Start()) starts it first and its start-up stages run too.
	Report Check(std::uint32_t a_quest, int a_stage, bool a_starts, const Env& a_env);
}
