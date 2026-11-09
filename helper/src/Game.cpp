#include "PCH.h"
#include "Game.h"
#include "Settings.h"

namespace Game
{
	namespace
	{
		std::atomic<bool> g_inGame{ false };

		using VM = RE::BSScript::Internal::VirtualMachine;

		// calls a_fn with the Papyrus function's return value
		class Callback : public RE::BSScript::IStackCallbackFunctor
		{
		public:
			explicit Callback(std::function<void(const RE::BSScript::Variable&)> a_fn) :
				_fn(std::move(a_fn))
			{}
			void operator()(RE::BSScript::Variable a_result) override
			{
				if (_fn) {
					_fn(a_result);
				}
			}
			void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

		private:
			std::function<void(const RE::BSScript::Variable&)> _fn;
		};

		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> MakeCallback(std::function<void(const RE::BSScript::Variable&)> a_fn)
		{
			return RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>{ new Callback(std::move(a_fn)) };
		}

		RE::VMHandle HandleOf(RE::TESQuest* a_quest)
		{
			auto* vm = VM::GetSingleton();
			if (!vm || !a_quest) {
				return 0;
			}
			auto* policy = vm->GetObjectHandlePolicy();
			return policy ? policy->GetHandleForObject(RE::TESQuest::FORMTYPE, a_quest) : 0;
		}

		// runs a console command (main thread)
		void Console(const std::string& a_command)
		{
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto*      script = factory ? factory->Create() : nullptr;
			if (!script) {
				spdlog::error("console: could not create a script for '{}'", a_command);
				return;
			}
			script->SetCommand(a_command);
			script->CompileAndRun(nullptr);
			delete script;
			spdlog::info("console: {}", a_command);
		}

		// ---- journal refresh ----------------------------------------------------------------------
		// Only objectives the journal lost are put back: an objective the quest shows (state 1, 3, 5) that the
		// player's journal list (PlayerCharacter objectives, the list the Journal menu reads) does not hold for the
		// quest's current run with that state. Objectives already listed are left alone: every
		// SetObjectiveDisplayed(true) queues an objective notification on the HUD (SkyrimSE.exe 1.6.1170: native
		// 0xa46d70 -> 23933 0x394440 -> 51646 0x9201c0 HUD message 0x11), so re-showing all of them (286 for
		// SkyrimIstari, helper 4-14) kept the HUD busy for many minutes.
		// One quest at a time: its lost objectives are hidden, then shown again (the game's own path adds them to
		// the list again: 40510 0x737cd0), then their completed / failed state is put back, before the next quest is
		// touched; nothing happens while a menu is open.
		struct Obj
		{
			std::uint16_t index = 0;
			std::uint8_t  state = 0;  // 1 shown, 3 completed + shown, 5 failed + shown
		};
		struct QuestItem
		{
			RE::FormID       quest = 0;
			std::vector<Obj> objs;
		};
		std::vector<QuestItem>    g_quests;
		std::size_t               g_total = 0;
		std::atomic<std::size_t>  g_done{ 0 }, g_failedCalls{ 0 };
		std::atomic<bool>         g_refreshing{ false };
		std::string               g_refreshNotify;

		std::size_t g_listed = 0;  // shown objectives the journal list already held (left alone)

		// the player's journal list holds this objective for the quest's current run, with this state
		bool Listed(const RE::BSTArray<RE::BGSInstancedQuestObjective>& a_list, const RE::TESQuest* a_quest, const RE::BGSQuestObjective* a_obj)
		{
			for (const auto& io : a_list) {
				if (io.Objective == a_obj && io.instanceID == a_quest->currentInstanceID && io.InstanceState == a_obj->state) {
					return true;
				}
			}
			return false;
		}

		void Finish()
		{
			g_refreshing = false;
			const auto msg = std::format("ReSaverHelper: quest journal refreshed ({} objective(s) put back in {} quest(s), {} already listed)", g_done.load(),
				g_quests.size(), g_listed);
			spdlog::info("{} ({} call(s) failed)", msg, g_failedCalls.load());
			Notify(g_refreshNotify, msg);
		}

		// a menu is open (Journal, pause menu, inventory...): wait
		bool MenuOpen()
		{
			auto* ui = RE::UI::GetSingleton();
			return ui && (ui->GameIsPaused() || ui->IsMenuOpen(RE::JournalMenu::MENU_NAME));
		}

		void Step(std::size_t a_quest, int a_phase, std::size_t a_obj);

		// runs Step later on the main thread (while a menu is open)
		void StepLater(std::size_t a_quest, int a_phase, std::size_t a_obj)
		{
			std::thread([a_quest, a_phase, a_obj] {
				std::this_thread::sleep_for(400ms);
				if (auto* task = SKSE::GetTaskInterface()) {
					task->AddTask([a_quest, a_phase, a_obj] { Step(a_quest, a_phase, a_obj); });
				}
			}).detach();
		}

		template <class... Args>
		void CallThen(std::size_t a_quest, int a_phase, std::size_t a_nextObj, const char* a_fn, Args... a_args)
		{
			auto*      quest = RE::TESForm::LookupByID<RE::TESQuest>(g_quests[a_quest].quest);
			auto*      vm = VM::GetSingleton();
			const auto handle = HandleOf(quest);
			auto       cb = MakeCallback([a_quest, a_phase, a_nextObj](const RE::BSScript::Variable&) { Step(a_quest, a_phase, a_nextObj); });
			if (!vm || !handle || !vm->DispatchMethodCall(handle, "Quest", a_fn, RE::MakeFunctionArguments(std::move(a_args)...), cb)) {
				++g_failedCalls;
				// on through the task queue (next frame), not by recursion: many failed calls in a row would
				// otherwise pile up on the main thread's stack
				if (auto* task = SKSE::GetTaskInterface()) {
					task->AddTask([a_quest, a_phase, a_nextObj] { Step(a_quest, a_phase, a_nextObj); });
				} else {
					Step(a_quest, a_phase, a_nextObj);
				}
			}
		}

		// phase 0: hide each shown objective; 1: show each again; 2: put completed / failed back; then the next quest
		void Step(std::size_t a_quest, int a_phase, std::size_t a_obj)
		{
			if (a_quest >= g_quests.size()) {
				Finish();
				return;
			}
			if (a_phase == 0 && a_obj == 0 && MenuOpen()) {
				StepLater(a_quest, a_phase, a_obj);  // a quest is only started with every menu closed
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
			const auto idx = static_cast<std::int32_t>(objs[a_obj].index);
			switch (a_phase) {
			case 0:
				CallThen(a_quest, 0, a_obj + 1, "SetObjectiveDisplayed", idx, false, false);
				return;
			case 1:
				CallThen(a_quest, 1, a_obj + 1, "SetObjectiveDisplayed", idx, true, false);
				return;
			default:
				if (objs[a_obj].state == 3) {
					CallThen(a_quest, 2, a_obj + 1, "SetObjectiveCompleted", idx, true);
				} else if (objs[a_obj].state == 5) {
					CallThen(a_quest, 2, a_obj + 1, "SetObjectiveFailed", idx, true);
				} else {
					Step(a_quest, 2, a_obj + 1);
				}
				return;
			}
		}
	}

	void SetInGame(bool a_inGame) { g_inGame = a_inGame; }
	bool InGame() { return g_inGame; }

	bool RunOnMain(std::function<void()> a_fn, std::chrono::milliseconds a_timeout)
	{
		auto task = SKSE::GetTaskInterface();
		if (!task) {
			return false;
		}
		auto done = std::make_shared<std::promise<void>>();
		auto fut = done->get_future();
		task->AddTask([fn = std::move(a_fn), done]() {
			try {
				fn();
			} catch (const std::exception& e) {
				spdlog::error("main thread task failed: {}", e.what());
			} catch (...) {
				spdlog::error("main thread task failed (unknown error)");
			}
			try {
				done->set_value();
			} catch (...) {
			}
		});
		return fut.wait_for(a_timeout) == std::future_status::ready;
	}

	void Notify(const std::string& a_mode, const std::string& a_text)
	{
		const auto mode = (a_mode.empty() || a_mode == "ini") ? Settings::Get().notify : a_mode;
		if (mode == "none") {
			return;
		}
		auto task = SKSE::GetTaskInterface();
		if (!task) {
			return;
		}
		task->AddTask([mode, a_text]() {
			auto* vm = VM::GetSingleton();
			if (!vm) {
				return;
			}
			RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
			vm->DispatchStaticCall("Debug", mode == "box" ? "MessageBox" : "Notification", RE::MakeFunctionArguments(RE::BSFixedString(a_text)), none);
		});
	}

	QuestInfo GetQuest(std::uint32_t a_formID)
	{
		QuestInfo info;
		auto*     quest = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
		if (!quest) {
			return info;
		}
		info.found = true;
		info.editorID = quest->GetFormEditorID() ? quest->GetFormEditorID() : "";
		info.flags = static_cast<std::uint16_t>(quest->data.flags.underlying());
		info.currentStage = quest->GetCurrentStageID();
		info.running = Running(quest);
		info.completed = quest->IsCompleted();
		for (auto* o : quest->objectives) {
			if (o) {
				info.objectives.push_back({ o->index, static_cast<std::uint8_t>(o->state.underlying()) });
			}
		}
		return info;
	}

	bool Running(const RE::TESQuest* a_quest)
	{
		return a_quest && a_quest->IsEnabled() && !a_quest->IsStopping();
	}

	void SetStage(std::uint32_t a_formID, std::uint16_t a_stage, std::function<void(bool)> a_done)
	{
		auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
		if (!quest) {
			a_done(false);
			return;
		}
		auto*      vm = VM::GetSingleton();
		const auto handle = HandleOf(quest);
		auto       cb = MakeCallback([a_done, a_formID, a_stage](const RE::BSScript::Variable& a_result) {
            const bool ok = a_result.IsBool() ? a_result.GetBool() : true;
            auto*      q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
            spdlog::info("SetStage {:08X} stage {}: {} (running {}, completed {}, current stage {})", a_formID, a_stage, ok ? "accepted" : "refused",
                q && Running(q), q && q->IsCompleted(), q ? q->GetCurrentStageID() : 0);
            a_done(ok);
        });
		if (vm && handle && vm->DispatchMethodCall(handle, "Quest", "SetStage", RE::MakeFunctionArguments(static_cast<std::int32_t>(a_stage)), cb)) {
			spdlog::info("SetStage {:08X} ({}) stage {} dispatched", a_formID, quest->GetFormEditorID(), a_stage);
			return;
		}
		// no script object for this quest: the console command does the same
		const std::string edid = quest->GetFormEditorID() ? quest->GetFormEditorID() : "";
		if (edid.empty()) {
			a_done(false);
			return;
		}
		Console(std::format("setstage {} {}", edid, a_stage));
		a_done(quest->GetCurrentStageID() == a_stage);
	}

	void Complete(std::uint32_t a_formID, std::function<void(bool)> a_done)
	{
		auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
		if (!quest) {
			a_done(false);
			return;
		}
		spdlog::info("Complete {:08X} ({}): running {}, completed {}, current stage {}", a_formID, quest->GetFormEditorID(), Running(quest), quest->IsCompleted(),
			quest->GetCurrentStageID());
		// last resort, on the main thread: the console's CompleteQuest
		auto viaConsole = [a_formID, a_done] {
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				a_done(false);
				return;
			}
			task->AddTask([a_formID, a_done] {
				auto*      q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
				const auto edid = q && q->GetFormEditorID() ? std::string(q->GetFormEditorID()) : std::string();
				if (!edid.empty()) {
					Console("completequest " + edid);
				}
				const bool done = q && q->IsCompleted();
				spdlog::info("Complete {:08X}: {} after the console command", a_formID, done ? "completed" : "still not completed");
				a_done(done);
			});
		};
		auto finish = [a_formID, a_done, viaConsole](const RE::BSScript::Variable&) {
			auto* q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
			if (q && q->IsCompleted()) {
				spdlog::info("Complete {:08X}: completed", a_formID);
				a_done(true);
				return;
			}
			viaConsole();
		};
		auto second = [a_formID, finish](const RE::BSScript::Variable&) {
			auto*      q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
			auto*      vm2 = VM::GetSingleton();
			const auto h = HandleOf(q);
			auto       cb = MakeCallback(finish);
			if (!q || !vm2 || !h || !vm2->DispatchMethodCall(h, "Quest", "CompleteQuest", RE::MakeFunctionArguments(), cb)) {
				finish(RE::BSScript::Variable{});
			}
		};
		auto*      vm = VM::GetSingleton();
		const auto handle = HandleOf(quest);
		auto       cb = MakeCallback(second);
		if (!vm || !handle || !vm->DispatchMethodCall(handle, "Quest", "CompleteAllObjectives", RE::MakeFunctionArguments(), cb)) {
			second(RE::BSScript::Variable{});
		}
	}

	void Stop(std::uint32_t a_formID, std::function<void(bool)> a_done)
	{
		auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
		if (!quest) {
			a_done(false);
			return;
		}
		spdlog::info("Stop {:08X} ({}): running {}, completed {}, current stage {}", a_formID, quest->GetFormEditorID(), Running(quest), quest->IsCompleted(),
			quest->GetCurrentStageID());
		auto*      vm = VM::GetSingleton();
		const auto handle = HandleOf(quest);
		auto       cb = MakeCallback([a_formID, a_done](const RE::BSScript::Variable&) {
            auto*      q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
            const bool stopped = q && !Running(q);
            spdlog::info("Stop {:08X}: {}", a_formID, stopped ? "stopped" : "still running");
            a_done(stopped);
        });
		if (vm && handle && vm->DispatchMethodCall(handle, "Quest", "Stop", RE::MakeFunctionArguments(), cb)) {
			return;
		}
		// no script object for this quest: the console command does the same
		const std::string edid = quest->GetFormEditorID() ? quest->GetFormEditorID() : "";
		if (edid.empty()) {
			a_done(false);
			return;
		}
		Console("stopquest " + edid);
		a_done(!Running(quest));
	}

	void Reset(std::uint32_t a_formID, std::function<void(bool)> a_done)
	{
		auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
		if (!quest) {
			a_done(false);
			return;
		}
		spdlog::info("Reset {:08X} ({}): running {}, completed {}, current stage {}", a_formID, quest->GetFormEditorID(), Running(quest), quest->IsCompleted(),
			quest->GetCurrentStageID());
		auto*      vm = VM::GetSingleton();
		const auto handle = HandleOf(quest);
		auto       cb = MakeCallback([a_formID, a_done](const RE::BSScript::Variable&) {
            auto*      q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
            const bool reset = q && !q->IsCompleted();
            spdlog::info("Reset {:08X}: {} (running {}, current stage {})", a_formID, reset ? "reset" : "still completed", q && Running(q), q ? q->GetCurrentStageID() : 0);
            a_done(reset);
        });
		if (vm && handle && vm->DispatchMethodCall(handle, "Quest", "Reset", RE::MakeFunctionArguments(), cb)) {
			return;
		}
		const std::string edid = quest->GetFormEditorID() ? quest->GetFormEditorID() : "";
		if (edid.empty()) {
			a_done(false);
			return;
		}
		Console("resetquest " + edid);
		a_done(!quest->IsCompleted());
	}

	void CompleteObjective(std::uint32_t a_formID, std::uint16_t a_index, std::function<void(bool)> a_done)
	{
		auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
		auto* vm = VM::GetSingleton();
		const auto handle = HandleOf(quest);
		auto       cb = MakeCallback([a_formID, a_index, a_done](const RE::BSScript::Variable&) {
            bool  done = false;
            if (auto* q = RE::TESForm::LookupByID<RE::TESQuest>(a_formID)) {
                for (auto* o : q->objectives) {
                    if (o && o->index == a_index) {
                        const auto st = static_cast<std::uint8_t>(o->state.underlying());
                        done = st == 2 || st == 3;  // completed (hidden or shown)
                    }
                }
            }
            spdlog::info("CompleteObjective {:08X} objective {}: {}", a_formID, a_index, done ? "completed" : "not completed");
            a_done(done);
        });
		if (!quest || !vm || !handle ||
			!vm->DispatchMethodCall(handle, "Quest", "SetObjectiveCompleted", RE::MakeFunctionArguments(static_cast<std::int32_t>(a_index), true), cb)) {
			spdlog::info("CompleteObjective {:08X} objective {}: the quest has no script object", a_formID, a_index);
			a_done(false);
			return;
		}
		spdlog::info("CompleteObjective {:08X} ({}) objective {} dispatched", a_formID, quest->GetFormEditorID(), a_index);
	}

	std::size_t StartRefresh(const std::string& a_notify)
	{
		if (g_refreshing) {
			return 0;
		}
		auto* dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return 0;
		}
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return 0;
		}
		const auto& list = player->GetPlayerRuntimeData().objectives;
		g_quests.clear();
		g_total = 0;
		g_listed = 0;
		for (auto* quest : dh->GetFormArray<RE::TESQuest>()) {
			if (!quest || !Running(quest)) {
				continue;
			}
			QuestItem qi;
			qi.quest = quest->GetFormID();
			for (auto* o : quest->objectives) {
				if (!o) {
					continue;
				}
				const auto st = static_cast<std::uint8_t>(o->state.underlying());
				if (st == 1 || st == 3 || st == 5) {
					if (Listed(list, quest, o)) {
						++g_listed;  // the journal has it: no call, no HUD notification
					} else {
						qi.objs.push_back({ o->index, st });
					}
				}
			}
			if (!qi.objs.empty()) {
				g_total += qi.objs.size();
				g_quests.push_back(std::move(qi));
			}
		}
		spdlog::info("Journal refresh: the journal list holds {} entries; {} shown objective(s) already listed, {} missing in {} quest(s)", list.size(),
			g_listed, g_total, g_quests.size());
		if (g_quests.empty()) {
			Notify(a_notify, std::format("ReSaverHelper: the journal already lists all {} shown objective(s), nothing to put back", g_listed));
			return 0;
		}
		g_done = 0;
		g_failedCalls = 0;
		g_refreshNotify = a_notify;
		g_refreshing = true;
		spdlog::info("Journal refresh: putting back {} objective(s) in {} quest(s), one quest at a time", g_total, g_quests.size());
		Notify(a_notify, std::format("ReSaverHelper: putting {} lost objective(s) back in the journal - keep menus closed until it is done", g_total));
		Step(0, 0, 0);
		return g_total;
	}

	bool MenuBlocksRefresh() { return MenuOpen(); }

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
			if (auto task = SKSE::GetTaskInterface()) {
				task->AddTask([]() { StartRefresh("ini"); });
			}
		}).detach();
	}
}
