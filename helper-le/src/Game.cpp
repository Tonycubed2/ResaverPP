#include "PCH.h"
#include "Game.h"
#include "LE.h"
#include "Papyrus.h"
#include "Settings.h"

namespace Game
{
	namespace
	{
		SKSETaskInterface* g_task = nullptr;
		std::atomic<bool>  g_inGame{ false };

		class Task : public TaskDelegate
		{
		public:
			explicit Task(std::function<void()> a_fn) :
				_fn(std::move(a_fn))
			{}
			virtual void Run() override
			{
				try {
					_fn();
				} catch (const std::exception& e) {
					spdlog::error("main thread task failed: {}", e.what());
				} catch (...) {
					spdlog::error("main thread task failed (unknown error)");
				}
			}
			virtual void Dispose() override { delete this; }

		private:
			std::function<void()> _fn;
		};

		TESQuest* Quest(std::uint32_t a_formID) { return le::LookupAs<TESQuest>(a_formID); }

		// ---- journal refresh (as on SE: one quest at a time, never while a menu is open) ------------
		struct Obj
		{
			std::uint16_t index = 0;
			std::uint8_t  state = 0;  // 1 shown, 3 completed + shown, 5 failed + shown
		};
		struct QuestItem
		{
			std::uint32_t    quest = 0;
			std::vector<Obj> objs;
		};
		std::vector<QuestItem>   g_quests;
		std::size_t              g_total = 0;
		std::atomic<std::size_t> g_done{ 0 }, g_failedCalls{ 0 };
		std::atomic<bool>        g_refreshing{ false };
		std::string              g_refreshNotify;

		void Finish()
		{
			g_refreshing = false;
			const auto msg = std::format("ReSaverHelper: quest journal refreshed ({} objective(s) in {} quest(s))", g_done.load(), g_quests.size());
			spdlog::info("{} ({} call(s) failed)", msg, g_failedCalls.load());
			Notify(g_refreshNotify, msg);
		}

		void Step(std::size_t a_quest, int a_phase, std::size_t a_obj);

		void StepLater(std::size_t a_quest, int a_phase, std::size_t a_obj)
		{
			std::thread([a_quest, a_phase, a_obj] {
				std::this_thread::sleep_for(400ms);
				AddTask([a_quest, a_phase, a_obj] { Step(a_quest, a_phase, a_obj); });
			}).detach();
		}

		void CallThen(std::size_t a_quest, int a_phase, std::size_t a_nextObj, const char* a_fn, papyrus::Args a_args)
		{
			auto* quest = Quest(g_quests[a_quest].quest);
			if (!papyrus::CallMethod(quest, "Quest", a_fn, std::move(a_args),
					[a_quest, a_phase, a_nextObj](const papyrus::Result&) { Step(a_quest, a_phase, a_nextObj); })) {
				++g_failedCalls;
				// on through the task queue (next frame), not by recursion: many failed calls in a row would
				// otherwise pile up on the main thread's stack
				AddTask([a_quest, a_phase, a_nextObj] { Step(a_quest, a_phase, a_nextObj); });
			}
		}

		// phase 0: hide each shown objective; 1: show each again; 2: put completed / failed back; then the next quest
		void Step(std::size_t a_quest, int a_phase, std::size_t a_obj)
		{
			if (a_quest >= g_quests.size()) {
				Finish();
				return;
			}
			if (a_phase == 0 && a_obj == 0 && le::MenuOpen()) {
				StepLater(a_quest, a_phase, a_obj);
				return;
			}
			const auto& objs = g_quests[a_quest].objs;
			if (a_obj >= objs.size()) {
				if (a_phase < 2) {
					Step(a_quest, a_phase + 1, 0);
				} else {
					g_done += objs.size();
					Step(a_quest + 1, 0, 0);
				}
				return;
			}
			using papyrus::Arg;
			const auto idx = static_cast<SInt32>(objs[a_obj].index);
			switch (a_phase) {
			case 0:
				CallThen(a_quest, 0, a_obj + 1, "SetObjectiveDisplayed", { Arg::Int(idx), Arg::Bool(false), Arg::Bool(false) });
				return;
			case 1:
				CallThen(a_quest, 1, a_obj + 1, "SetObjectiveDisplayed", { Arg::Int(idx), Arg::Bool(true), Arg::Bool(false) });
				return;
			default:
				if (objs[a_obj].state == 3) {
					CallThen(a_quest, 2, a_obj + 1, "SetObjectiveCompleted", { Arg::Int(idx), Arg::Bool(true) });
				} else if (objs[a_obj].state == 5) {
					CallThen(a_quest, 2, a_obj + 1, "SetObjectiveFailed", { Arg::Int(idx), Arg::Bool(true) });
				} else {
					Step(a_quest, 2, a_obj + 1);
				}
				return;
			}
		}
	}

	void SetTaskInterface(SKSETaskInterface* a_task) { g_task = a_task; }

	void SetInGame(bool a_inGame) { g_inGame = a_inGame; }
	bool InGame() { return g_inGame; }

	bool AddTask(std::function<void()> a_fn)
	{
		if (!g_task) {
			return false;
		}
		g_task->AddTask(new Task(std::move(a_fn)));
		return true;
	}

	bool RunOnMain(std::function<void()> a_fn, std::chrono::milliseconds a_timeout)
	{
		auto done = std::make_shared<std::promise<void>>();
		auto fut = done->get_future();
		if (!AddTask([fn = std::move(a_fn), done]() {
				try {
					fn();
				} catch (...) {
					spdlog::error("main thread task failed");
				}
				try {
					done->set_value();
				} catch (...) {
				}
			})) {
			return false;
		}
		return fut.wait_for(a_timeout) == std::future_status::ready;
	}

	void Notify(const std::string& a_mode, const std::string& a_text)
	{
		const auto mode = (a_mode.empty() || a_mode == "ini") ? Settings::Get().notify : a_mode;
		if (mode == "none") {
			return;
		}
		AddTask([mode, a_text]() {
			papyrus::CallStatic("Debug", mode == "box" ? "MessageBox" : "Notification", { papyrus::Arg::String(a_text) });
		});
	}

	QuestInfo GetQuest(std::uint32_t a_formID)
	{
		QuestInfo info;
		auto*     quest = Quest(a_formID);
		if (!quest) {
			return info;
		}
		info.found = true;
		info.editorID = le::EditorID(quest);
		info.flags = quest->unk07C.flags;
		info.currentStage = quest->GetCurrentStageID();
		info.running = le::Running(quest);
		info.completed = le::Completed(quest);
		info.objectives = le::Objectives(quest);
		return info;
	}

	QuestState StateOf(std::uint32_t a_formID)
	{
		QuestState s;
		if (auto* q = Quest(a_formID)) {
			s.running = le::Running(q);
			s.completed = le::Completed(q);
			s.stage = q->GetCurrentStageID();
		}
		return s;
	}

	void SetStage(std::uint32_t a_formID, std::uint16_t a_stage, std::function<void(bool)> a_done)
	{
		auto* quest = Quest(a_formID);
		if (!quest) {
			a_done(false);
			return;
		}
		const auto edid = le::EditorID(quest);
		const bool ok = papyrus::CallMethod(quest, "Quest", "SetStage", { papyrus::Arg::Int(a_stage) },
			[a_done, a_formID, a_stage](const papyrus::Result& a_r) {
				const bool accepted = a_r.IsBool() ? a_r.GetBool() : true;
				const auto s = StateOf(a_formID);
				spdlog::info("SetStage {:08X} stage {}: {} (running {}, completed {}, current stage {})", a_formID, a_stage, accepted ? "accepted" : "refused",
					s.running, s.completed, s.stage);
				a_done(accepted);
			});
		if (ok) {
			spdlog::info("SetStage {:08X} ({}) stage {} dispatched", a_formID, edid, a_stage);
			return;
		}
		// the quest has no script object, so Papyrus cannot reach it (the SE helper uses the console here)
		spdlog::warn("SetStage {:08X} ({}): the quest has no script object; type  setstage {} {}  in the game's console", a_formID, edid, edid, a_stage);
		a_done(false);
	}

	void Complete(std::uint32_t a_formID, std::function<void(bool)> a_done)
	{
		auto* quest = Quest(a_formID);
		if (!quest) {
			a_done(false);
			return;
		}
		spdlog::info("Complete {:08X} ({}): running {}, completed {}, current stage {}", a_formID, le::EditorID(quest), le::Running(quest), le::Completed(quest),
			quest->GetCurrentStageID());
		auto finish = [a_formID, a_done](const papyrus::Result&) {
			const bool done = StateOf(a_formID).completed;
			spdlog::info("Complete {:08X}: {}", a_formID, done ? "completed" : "still not completed");
			a_done(done);
		};
		auto second = [a_formID, finish](const papyrus::Result&) {
			if (!papyrus::CallMethod(Quest(a_formID), "Quest", "CompleteQuest", {}, finish)) {
				finish({});
			}
		};
		if (!papyrus::CallMethod(quest, "Quest", "CompleteAllObjectives", {}, second)) {
			second({});
		}
	}

	void Stop(std::uint32_t a_formID, std::function<void(bool)> a_done)
	{
		auto* quest = Quest(a_formID);
		if (!quest) {
			a_done(false);
			return;
		}
		spdlog::info("Stop {:08X} ({}): running {}, completed {}, current stage {}", a_formID, le::EditorID(quest), le::Running(quest), le::Completed(quest),
			quest->GetCurrentStageID());
		const bool ok = papyrus::CallMethod(quest, "Quest", "Stop", {}, [a_formID, a_done](const papyrus::Result&) {
			const auto s = StateOf(a_formID);
			spdlog::info("Stop {:08X}: {}", a_formID, s.running ? "still running" : "stopped");
			a_done(!s.running);
		});
		if (!ok) {
			spdlog::warn("Stop {:08X}: the quest has no script object; type  stopquest {}  in the game's console", a_formID, le::EditorID(quest));
			a_done(false);
		}
	}

	std::size_t StartRefresh(const std::string& a_notify)
	{
		if (g_refreshing) {
			return 0;
		}
		auto* dh = le::Data();
		if (!dh) {
			return 0;
		}
		g_quests.clear();
		g_total = 0;
		for (UInt32 i = 0; i < dh->quests.count; ++i) {
			auto* quest = dh->quests.arr.entries[i];
			if (!quest || !le::Running(quest)) {
				continue;
			}
			QuestItem qi;
			qi.quest = quest->formID;
			for (const auto& [index, st] : le::Objectives(quest)) {
				if (st == 1 || st == 3 || st == 5) {
					qi.objs.push_back({ index, st });
				}
			}
			if (!qi.objs.empty()) {
				g_total += qi.objs.size();
				g_quests.push_back(std::move(qi));
			}
		}
		if (g_quests.empty()) {
			Notify(a_notify, "ReSaverHelper: no journal objectives to refresh");
			return 0;
		}
		g_done = 0;
		g_failedCalls = 0;
		g_refreshNotify = a_notify;
		g_refreshing = true;
		spdlog::info("Journal refresh: {} objective(s) in {} quest(s), one quest at a time", g_total, g_quests.size());
		Notify(a_notify, std::format("ReSaverHelper: refreshing the quest journal ({} objectives) - keep menus closed until it is done", g_total));
		Step(0, 0, 0);
		return g_total;
	}

	bool MenuBlocksRefresh() { return le::MenuOpen(); }

	Progress RefreshProgress() { return { g_done.load(), g_total, g_refreshing.load() }; }

	void OnGameLoaded()
	{
		SetInGame(true);
		const auto& s = Settings::Get();
		if (!s.refreshOnLoad) {
			return;
		}
		const int delay = s.refreshDelay;
		std::thread([delay]() {
			std::this_thread::sleep_for(std::chrono::seconds(delay));
			AddTask([]() { StartRefresh("ini"); });
		}).detach();
	}
}
