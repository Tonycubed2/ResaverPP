#pragma once

// Everything that touches the game, for Skyrim LE (the same functions as the SE helper's Game.h).
// Game state is only read or changed on the game's main thread (through SKSE's task interface);
// Papyrus functions are called through the script engine, the same way a script would call them, so
// the game's own rules apply (stage scripts run, rewards are given). LE has no console call here: what
// the SE helper does with a console command, this one does through Papyrus.
namespace Game
{
	void SetTaskInterface(SKSETaskInterface* a_task);

	// a save is loaded (or a new game started): quests can be looked at and changed
	void SetInGame(bool a_inGame);
	bool InGame();

	// queues a_fn for the main thread (no waiting)
	bool AddTask(std::function<void()> a_fn);
	// Runs a_fn on the main thread and waits for it, up to a_timeout. False on timeout.
	bool RunOnMain(std::function<void()> a_fn, std::chrono::milliseconds a_timeout = 30s);

	// "hud", "box", "none" or "ini" (= the INI's sNotify). Safe from any thread.
	void Notify(const std::string& a_mode, const std::string& a_text);

	struct QuestInfo
	{
		bool          found = false;
		std::string   editorID;
		std::uint16_t flags = 0;
		std::uint16_t currentStage = 0;
		bool          running = false, completed = false;
		std::vector<std::pair<std::uint16_t, std::uint8_t>> objectives;  // index, state
	};
	QuestInfo GetQuest(std::uint32_t a_formID);  // main thread

	struct QuestState
	{
		bool          running = false, completed = false;
		std::uint16_t stage = 0;
	};
	QuestState StateOf(std::uint32_t a_formID);  // main thread

	// Quest.SetStage (main thread). a_done gets true if the game accepted the stage.
	void SetStage(std::uint32_t a_formID, std::uint16_t a_stage, std::function<void(bool)> a_done);

	// Marks the quest completed without running a stage script: Quest.CompleteAllObjectives, then
	// Quest.CompleteQuest. a_done gets whether the quest is completed afterwards.
	void Complete(std::uint32_t a_formID, std::function<void(bool)> a_done);

	// Quest.Stop (main thread): the game stops the quest and cleans up after it (its aliases are
	// emptied, its scripts get OnStoryScriptStop/stop events, its objectives leave the journal).
	// a_done gets whether the quest is stopped afterwards (or ? while it is still stopping).
	void Stop(std::uint32_t a_formID, std::function<void(bool)> a_done);

	// Journal refresh: for every running quest, every objective the journal shows is hidden and shown
	// again (its completed / failed state put back), so the game rebuilds its list of shown objectives.
	// Returns how many objectives will be refreshed; 0 if nothing to do or one is already running.
	std::size_t StartRefresh(const std::string& a_notify);  // main thread
	bool        MenuBlocksRefresh();                         // main thread
	struct Progress
	{
		std::size_t done = 0, total = 0;
		bool        running = false;
	};
	Progress RefreshProgress();

	// after a save load: refresh automatically if bRefreshJournalOnLoad=1 (after iRefreshDelaySeconds)
	void OnGameLoaded();
}
