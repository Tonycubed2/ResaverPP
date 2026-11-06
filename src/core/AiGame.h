// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The game-dependent parts of "Ask the AI" (gui/AppAi.cpp): the lesson book for the open save's game
// (Skyrim SE/AE, VR, LE or Fallout 4), the game line of the current state, which command line options and
// which ReSaverHelper commands do not work for that game (the AI gets a clear "not available" instead of wrong
// data), and the chat window's texts. Here, in the core, so the unit tests can check them.
#pragma once

#include <string>
#include <vector>

namespace ess::aigame
{
	enum class Game
	{
		kSkyrimSE,  // Skyrim Special Edition / Anniversary Edition
		kSkyrimVR,
		kSkyrimLE,  // Legendary Edition
		kFallout4
	};
	// the open save's game (a_fo4 / a_le from the save, a_vr from the app's game setup)
	Game        FromFlags(bool a_fo4, bool a_le, bool a_vr);
	const char* Name(Game a_game);  // "Skyrim Special Edition / AE", "Skyrim VR", "Skyrim Legendary Edition", "Fallout 4"
	inline bool IsFo4(Game a_game) { return a_game == Game::kFallout4; }

	// The lesson book: who the AI is, how to work, the saves, the running game, the logs and recipes, for this game;
	// ends with the heading the command line's help text follows.
	std::string Lesson(Game a_game);

	// "- game: ..." of CURRENT STATE: the game, the version the save was made with when the save says it (Fallout 4
	// saves do: "1.10.984.0"), the save files and the script extender. a_haveSave false: the game set up in the app.
	std::string GameLine(Game a_game, const std::string& a_gameVersion, bool a_haveSave);

	// run_cli: "" when the arguments may run for a save of this game, else the answer for the AI (what is not
	// available for Fallout 4 saves and what to use instead)
	std::string CliRefusal(Game a_game, const std::vector<std::string>& a_args);

	// The connected ReSaverHelper's game, from HELLO's game field: F4 / F4NG / F4AE are the Fallout 4 helper
	bool        HelperIsFo4(const std::string& a_runtime);
	// game_command: "" when the connected helper has a_command, else the answer for the AI
	std::string HelperRefusal(const std::string& a_runtime, const std::string& a_command);
	// the commands that only read (no approval needed)
	bool        HelperReadOnly(const std::string& a_command);

	// the logs window and the AI chat's texts
	std::string              LogsTitle(Game a_game);  // "Skyrim logs" / "Fallout 4 logs"
	std::string              ChatIntro(Game a_game);
	std::vector<std::string> ChatExamples(Game a_game);
}
