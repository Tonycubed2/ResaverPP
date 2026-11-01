#include "PCH.h"
#include "Rescue.h"
#include "Game.h"
#include "Settings.h"

#include <ctime>

namespace Rescue
{
	namespace
	{
		constexpr RE::FormID kCurrentFollowerFaction = 0x0005C84E;  // Skyrim.esm
		constexpr RE::FormID kDialogueFollower = 0x000750BA;        // the vanilla follower quest

		// runs a console command (main thread)
		void Console(const std::string& a_command)
		{
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto*      script = factory ? factory->Create() : nullptr;
			if (!script) {
				spdlog::error("rescue: could not create a script for '{}'", a_command);
				return;
			}
			script->SetCommand(a_command);
			script->CompileAndRun(nullptr);
			delete script;
			spdlog::info("rescue: console {}", a_command);
		}

		std::string NameOf(RE::Actor* a)
		{
			const char* n = a ? a->GetDisplayFullName() : nullptr;
			return n && *n ? std::string(n) : std::format("{:08X}", a ? a->GetFormID() : 0);
		}

		bool IsFollower(RE::Actor* a, RE::TESFaction* a_faction)
		{
			if (!a || a == RE::PlayerCharacter::GetSingleton() || a->IsDeleted() || a->IsDisabled() || a->IsDead() || a->IsCommandedActor()) {
				return false;
			}
			if (a->IsPlayerTeammate()) {
				return true;
			}
			// dismissed followers stay in the faction with rank -1
			if (a_faction && a->GetFactionRank(a_faction, false) >= 0) {
				return true;
			}
			// held by the vanilla follower quest (DialogueFollower)
			if (auto* xa = a->extraList.GetByType<RE::ExtraAliasInstanceArray>()) {
				const RE::BSReadLockGuard guard{ xa->lock };
				for (auto* inst : xa->aliases) {
					if (inst && inst->quest && inst->quest->GetFormID() == kDialogueFollower) {
						return true;
					}
				}
			}
			return false;
		}

		std::string JoinNames(const std::vector<std::string>& a_names)
		{
			std::string s;
			for (const auto& n : a_names) {
				s += (s.empty() ? "" : ", ") + n;
			}
			return s;
		}
	}

	std::vector<std::string> GatherFollowers()
	{
		std::vector<std::string> names;
		auto*                    player = RE::PlayerCharacter::GetSingleton();
		if (!player || !player->GetParentCell()) {
			spdlog::warn("rescue: no player in a cell; followers not moved");
			return names;
		}
		auto*                   faction = RE::TESForm::LookupByID<RE::TESFaction>(kCurrentFollowerFaction);
		std::vector<RE::Actor*> found;
		{
			const auto& [map, lock] = RE::TESForm::GetAllForms();
			[[maybe_unused]] const RE::BSReadLockGuard l{ lock };
			if (map) {
				for (const auto& [id, form] : *map) {
					if (!form || form->GetFormType() != RE::FormType::ActorCharacter) {
						continue;
					}
					auto* a = form->As<RE::Actor>();
					if (IsFollower(a, faction)) {
						found.push_back(a);
					}
				}
			}
		}
		// moved outside the forms lock: moving a reference that is not loaded can load it
		for (auto* a : found) {
			a->MoveTo(player);
			a->EvaluatePackage();
			names.push_back(NameOf(a));
			spdlog::info("rescue: follower {:08X} ({}) moved to the player", a->GetFormID(), names.back());
		}
		spdlog::info("rescue: {} follower(s) gathered", names.size());
		return names;
	}

	void RestoreControls()
	{
		Console("EnablePlayerControls");
	}

	std::string SaveGame()
	{
		const auto now = std::time(nullptr);
		std::tm    tm{};
		localtime_s(&tm, &now);
		char name[64];
		std::strftime(name, sizeof(name), "ReSaverPP_Rescue_%Y-%m-%d_%H%M%S", &tm);
		Console(std::string("save ") + name);
		return name;
	}

	std::string RunAll()
	{
		const auto save = SaveGame();
		RestoreControls();
		const auto names = GatherFollowers();
		return std::format("ReSaver rescue: saved as {}, controls back on, {} follower(s) brought to you{}{}", save, names.size(),
			names.empty() ? "" : ": ", JoinNames(names));
	}

	void StartHotkey()
	{
		std::thread([] {
			using clock = std::chrono::steady_clock;
			std::optional<clock::time_point> down;
			bool                             fired = false, warned = false;
			while (true) {
				std::this_thread::sleep_for(100ms);
				const auto& s = Settings::Get();
				bool        held = false;
				if (s.rescueHotkey && Game::InGame()) {
					// only while Skyrim is the window in front, so Ctrl held in another program does nothing
					DWORD pid = 0;
					if (HWND w = GetForegroundWindow()) {
						GetWindowThreadProcessId(w, &pid);
					}
					held = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
				}
				if (!held) {
					down.reset();
					fired = warned = false;
					continue;
				}
				if (!down) {
					down = clock::now();
				}
				if (fired) {
					continue;  // once per hold: let go of Ctrl to do it again
				}
				const double secs = std::chrono::duration<double>(clock::now() - *down).count();
				const int    need = s.rescueHoldSeconds;
				if (secs >= need) {
					fired = true;
					spdlog::info("rescue: Ctrl held {} seconds", need);
					if (auto* task = SKSE::GetTaskInterface()) {
						task->AddTask([] {
							const auto msg = RunAll();
							spdlog::info("{}", msg);
							Game::Notify("hud", msg);
						});
					}
				} else if (!warned && need >= 5 && secs >= need - 3) {
					warned = true;
					Game::Notify("hud", "ReSaver rescue: keep holding Ctrl to save, get your controls back and bring your followers");
				}
			}
		}).detach();
	}
}
