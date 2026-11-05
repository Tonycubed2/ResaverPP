#pragma once
#include "F4SE.h"

// The running game, for every Fallout 4 version the helper supports (1.10.163, 1.10.984, 1.11.x).
// Everything the helper does in the game goes through the game's own console commands, run the way the
// console runs them (a script form compiled and run on the main thread). That needs only two addresses from
// the Address Library and one structure field, the same in every version, so one DLL works on all of them.
namespace Game
{
	enum class Runtime
	{
		kUnknown,
		kOG,  // 1.10.163, F4SE 0.6.23
		kNG,  // 1.10.980 / 1.10.984, F4SE 0.7.2
		kAE,  // 1.11.137 and later, F4SE 0.7.4+
	};

	// a_runtimeVersion: F4SEInterface::runtimeVersion. False (logged) if the version is not supported or its
	// Address Library file is missing.
	bool        Init(std::uint32_t a_runtimeVersion, f4se::F4SETaskInterface* a_tasks);
	Runtime     GetRuntime();
	std::string RuntimeName();  // "F4", "F4NG", "F4AE"
	std::string GameVersion();  // "1.10.984.0"

	void SetInGame(bool a_inGame);
	bool InGame();

	// a_fn on the game's main thread; false if it did not run within a_timeout (it still runs later)
	bool AddTask(std::function<void()> a_fn);
	bool RunOnMain(std::function<void()> a_fn, std::chrono::milliseconds a_timeout = 30s);

	// Runs one console command, as typed in the console. Main thread only. False (logged) if it could not
	// be run at all; what the command itself prints goes to the game's console.
	bool Console(const std::string& a_command);

	// "hud", "box", "none" or "ini" (= the INI's sNotify). Safe from any thread.
	void Notify(const std::string& a_mode, const std::string& a_text);
}
