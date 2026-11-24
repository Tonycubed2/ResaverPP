#pragma once
// "Ask the AI" on the F1 page (version 23): ReSaver Plus's AI chat, running inside the game. ReSaver Plus hands its AI
// settings over once (AICONFIG, sent when it connects); the helper keeps them in ReSaverHelper_AI.ini, the API key
// encrypted for this Windows user (DPAPI, as ReSaver Plus keeps it), so ReSaver Plus may stay closed afterwards.
// The AI can use the helper's own commands (the same ones ReSaver Plus's AI sends through the pipe) and read the
// game's logs; it cannot open or edit save files (that needs ReSaver Plus).

#include <string>
#include <vector>

namespace AiPage
{
	void Load();  // at kDataLoaded: the saved settings
	// AICONFIG from ReSaver Plus: <preset> <api> <url> <protected key> <model> <ask 0|1> <max tokens> <timeout>; "" or why not
	std::string Configure(const std::vector<std::string>& a_fields);
	void __stdcall Render();
}
