#include "PCH.h"
#include "Rescue.h"
#include "Game.h"
#include "Settings.h"

#include <cmath>
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

		// Characters that are not really with the player: a mod's script holder parked in a dummy or holding cell (Vilja's
		// "Outfit configuration" sits in AAEMDummy3 as a teammate). Moving one of those next to the player is wrong.
		bool InHoldingCell(RE::Actor* a)
		{
			auto* cell = a->GetParentCell();
			if (!cell || !cell->IsInteriorCell()) {
				return false;
			}
			std::string id = cell->GetFormEditorID() ? cell->GetFormEditorID() : "";
			std::ranges::transform(id, id.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			for (const char* w : { "dummy", "holding", "storage", "stash", "debug", "test", "marker" }) {
				if (id.find(w) != std::string::npos) {
					return true;
				}
			}
			return false;
		}

		// bSkip: listed in sGatherSkip (ReSaverHelper.ini, form ids separated by commas)
		bool Skipped(RE::Actor* a)
		{
			const auto& list = Settings::Get().gatherSkip;
			std::size_t start = 0;
			for (std::size_t i = 0; i <= list.size(); ++i) {
				if (i == list.size() || list[i] == ',' || list[i] == ' ' || list[i] == ';') {
					if (i > start) {
						try {
							if (static_cast<RE::FormID>(std::stoul(list.substr(start, i - start), nullptr, 16)) == a->GetFormID()) {
								return true;
							}
						} catch (...) {
						}
					}
					start = i + 1;
				}
			}
			return false;
		}

		// why a_actor is a follower, or "" when it is not one
		std::string FollowerReason(RE::Actor* a, RE::TESFaction* a_faction)
		{
			if (!a || a == RE::PlayerCharacter::GetSingleton() || a->IsDeleted() || a->IsDisabled() || a->IsDead() || a->IsCommandedActor()) {
				return {};
			}
			// held by the vanilla follower quest (DialogueFollower)
			if (auto* xa = a->extraList.GetByType<RE::ExtraAliasInstanceArray>()) {
				const RE::BSReadLockGuard guard{ xa->lock };
				for (auto* inst : xa->aliases) {
					if (inst && inst->quest && inst->quest->GetFormID() == kDialogueFollower) {
						return "vanilla follower quest";
					}
				}
			}
			// dismissed followers stay in the faction with rank -1
			if (a_faction && a->GetFactionRank(a_faction, false) >= 0) {
				return "follower faction";
			}
			if (a->IsPlayerTeammate()) {
				return "teammate";
			}
			return {};
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
		auto*                    vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
		auto*                    policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
		if (!player || !player->GetParentCell() || !policy) {
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
					auto*      a = form->As<RE::Actor>();
					const auto why = FollowerReason(a, faction);
					if (why.empty()) {
						continue;
					}
					if (Skipped(a)) {
						spdlog::info("rescue: {:08X} ({}) left alone: listed in sGatherSkip", a->GetFormID(), NameOf(a));
						continue;
					}
					if (InHoldingCell(a)) {
						spdlog::info("rescue: {:08X} ({}) left alone: a {} parked in the holding cell {} (a mod's helper, not a follower)", a->GetFormID(),
							NameOf(a), why, a->GetParentCell()->GetFormEditorID());
						continue;
					}
					found.push_back(a);
					spdlog::info("rescue: {:08X} ({}) is a follower ({})", a->GetFormID(), NameOf(a), why);
				}
			}
		}
		// Papyrus MoveTo (as the console's moveto), each one at its own spot in a half circle behind the player:
		// never on top of the player or of each other (several characters dropped on the player's own spot push
		// the player out of the world).
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
		const float heading = player->GetAngleZ();
		const float n = static_cast<float>(found.size());
		for (std::size_t i = 0; i < found.size(); ++i) {
			auto*       a = found[i];
			const auto  h = policy->GetHandleForObject(static_cast<RE::VMTypeID>(RE::Character::FORMTYPE), a);
			if (!h) {
				spdlog::warn("rescue: {:08X} ({}) has no script handle; not moved", a->GetFormID(), NameOf(a));
				continue;
			}
			const float spread = n > 1 ? (static_cast<float>(i) - (n - 1) / 2.0f) * (2.4f / std::max(n - 1.0f, 1.0f)) : 0.0f;  // up to +-69 degrees
			const float ang = heading + 3.14159265f + spread;
			const float dist = 160.0f + 20.0f * static_cast<float>(i % 2);
			const float x = std::sin(ang) * dist, y = std::cos(ang) * dist;
			const auto  from = a->GetParentCell();
			const auto  pos = a->GetPosition();
			vm->DispatchMethodCall(h, "ObjectReference", "MoveTo",
				RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(player), x, y, 0.0f, false), none);
			vm->DispatchMethodCall(h, "Actor", "EvaluatePackage", RE::MakeFunctionArguments(), none);
			names.push_back(NameOf(a));
			spdlog::info("rescue: follower {:08X} ({}) moved to the player (was in {} {:08X} at {:.0f}, {:.0f}, {:.0f})", a->GetFormID(), names.back(),
				from && from->GetFormEditorID() ? from->GetFormEditorID() : "", from ? from->GetFormID() : 0, pos.x, pos.y, pos.z);
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

	std::string SaveAndControls()
	{
		const auto save = SaveGame();
		RestoreControls();
		return save;
	}

	std::string Describe(const std::string& a_save, const std::vector<std::string>& a_names)
	{
		return std::format("ReSaver rescue: saved as {}, controls back on, {} follower(s) brought to you{}{}", a_save, a_names.size(),
			a_names.empty() ? "" : ": ", JoinNames(a_names));
	}

	std::string RunAll()
	{
		// the save first, on its own: it is written at the end of the frame, so the followers are moved a few
		// seconds later and the rescue save is the game as it was (a way back if anything goes wrong)
		struct State
		{
			std::string              save;
			std::vector<std::string> names;
		};
		auto st = std::make_shared<State>();
		if (!Game::RunOnMain([st] { st->save = SaveAndControls(); }, 60s)) {
			return "ReSaver rescue: the game is busy";
		}
		std::this_thread::sleep_for(3s);
		Game::RunOnMain([st] { st->names = GatherFollowers(); }, 60s);
		return Describe(st->save, st->names);
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
					std::thread([] {
						const auto msg = RunAll();
						spdlog::info("{}", msg);
						Game::Notify("hud", msg);
					}).detach();
				} else if (!warned && need >= 5 && secs >= need - 3) {
					warned = true;
					Game::Notify("hud", "ReSaver rescue: keep holding Ctrl to save, get your controls back and bring your followers");
				}
			}
		}).detach();
	}
}
