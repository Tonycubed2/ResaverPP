#pragma once
#include "SFSE.h"

// The running game (Starfield, with SFSE). Every address comes from the Address Library for SFSE Plugins
// (versionlib-<game version>.bin in Data\SFSE\Plugins), by id, so a game update needs a new Address Library file, not
// a new helper (unless Bethesda changes the functions themselves). The ids and what they are were read in
// Starfield.exe 1.16.244 (addresses below are that version's):
//   113576  0x141ea1e10  the console's "run this command" (printf-style: (console, format, ...)); the game itself runs
//                        console commands through it (e.g. "ttl"); it puts the line in the console's queue (0x14591dc10),
//                        which the console works through on its next update (0x141ea3130)
//   938528  0x1461e26b0  the console object the game passes to it (a pointer)
//   47401   0x1405de3b0  TESForm* LookupByID(u32)  (CommonLibSF: TESForm::LookupByID)
// Quests are read straight from the form (offsets read in Starfield.exe 1.16.244: the save writer
// TESQuest::SaveGame 0x140d0f2e0 and the Papyrus natives Quest.IsCompleted 0x1420cdc30, IsRunning 0x1420cdca0,
// GetCurrentStageID 0x1420cdc00, IsStageDone 0x140d0dbd0): form type +0x2E (QUST = 0x5B), flags +0x114 (0x1 enabled,
// 0x2 completed; IsRunning also wants 0x80 clear, +0x338 null and +0x32C zero), current stage +0x328 (u16), stages
// +0x138 (u32 count) / +0x140 (stage pointers: +0x10 u16 index, +0x12 bit 0 done).
namespace Game
{
	// a_runtimeVersion: SFSEInterface::runtimeVersion. False (logged) when the Address Library file is missing or lacks
	// an id the helper needs.
	bool        Init(std::uint32_t a_runtimeVersion, sfse::SFSETaskInterface* a_tasks);
	std::string GameVersion();  // "1.16.244.0"

	void SetInGame(bool a_inGame);
	bool InGame();

	// a_fn on the game's main thread; false if it did not run within a_timeout (it still runs later)
	bool AddTask(std::function<void()> a_fn);
	bool RunOnMain(std::function<void()> a_fn, std::chrono::milliseconds a_timeout = 30s);

	// Runs one console command, as typed in the console (queued: the console runs it this frame). Main thread only.
	bool Console(const std::string& a_command);

	// "hud", "box", "none" or "ini" (= the INI's sNotify). Safe from any thread.
	void Notify(const std::string& a_mode, const std::string& a_text);

	struct QuestState
	{
		bool          found = false;  // a quest with that form id exists
		std::uint32_t flags = 0;
		std::uint16_t currentStage = 0;
		bool          running = false, completed = false;
		std::vector<std::pair<std::uint16_t, bool>> stages;  // index, done
	};
	QuestState Quest(std::uint32_t a_formID);  // main thread

	// the form's type (TESForm +0x2E, the game's form type numbers: REFR 0x4A, ACHR 0x4B, QUST 0x5B, PERK 0x6A), or -1 when
	// the game has no form with that id. Main thread.
	int FormType(std::uint32_t a_formID);
	inline constexpr int kFormType_REFR = 0x4A, kFormType_ACHR = 0x4B, kFormType_QUST = 0x5B, kFormType_PERK = 0x6A;
}
