#include "PCH.h"
#include "Rescue.h"
#include "Game.h"
#include "LE.h"
#include "Papyrus.h"
#include "Settings.h"

#include <cmath>
#include <ctime>

namespace Rescue
{
	namespace
	{
		constexpr std::uint32_t kCurrentFollowerFaction = 0x0005C84E;  // Skyrim.esm
		constexpr std::uint32_t kDialogueFollower = 0x000750BA;        // the vanilla follower quest

		using papyrus::Arg;

		std::string ActorName(Actor* a)
		{
			auto name = le::NameOf(le::BaseNPC(a));
			if (name.empty()) {
				name = le::NameOf(a);
			}
			return name.empty() ? std::format("{:08X}", a ? a->formID : 0) : name;
		}

		bool HeldByVanillaQuest(Actor* a)
		{
			if (auto* xa = static_cast<ExtraAliasInstanceArray*>(a->extraData.GetByType(kExtraData_AliasInstanceArray))) {
				for (UInt32 k = 0; k < xa->aliases.count; ++k) {
					auto* inst = xa->aliases.arr.entries[k];
					if (inst && inst->quest && inst->quest->formID == kDialogueFollower) {
						return true;
					}
				}
			}
			return false;
		}

		// listed in sGatherSkip (ReSaverHelper.ini, form ids separated by commas)
		bool Skipped(Actor* a)
		{
			const auto& list = Settings::Get().gatherSkip;
			std::size_t start = 0;
			for (std::size_t i = 0; i <= list.size(); ++i) {
				if (i == list.size() || list[i] == ',' || list[i] == ' ' || list[i] == ';') {
					if (i > start) {
						try {
							if (static_cast<std::uint32_t>(std::stoul(list.substr(start, i - start), nullptr, 16)) == a->formID) {
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
		auto*                    player = le::Player();
		auto*                    dh = le::Data();
		if (!player || !dh || !player->parentCell) {
			spdlog::warn("rescue: no player in a cell; followers not moved");
			return names;
		}
		auto* faction = le::LookupAs<TESFaction>(kCurrentFollowerFaction);
		// LE has no list of every form to walk: the followers are found through the quest aliases that hold
		// them (the vanilla follower quest and every follower mod's own system keep their followers in aliases)
		std::set<std::uint32_t> seen;
		std::vector<Actor*>     found;
		for (UInt32 qi = 0; qi < dh->quests.count; ++qi) {
			auto* quest = dh->quests.arr.entries[qi];
			if (!quest || !le::Running(quest)) {
				continue;
			}
			for (UInt32 ai = 0; ai < quest->aliases.count; ++ai) {
				auto* alias = quest->aliases.arr.entries[ai];
				auto* refAlias = alias ? le::Cast<BGSRefAlias>(alias, RTTI_BGSBaseAlias, RTTI_BGSRefAlias) : nullptr;
				auto* actor = refAlias ? refAlias->GetActorReference() : nullptr;
				if (!actor || actor == player || !seen.insert(actor->formID).second || actor->IsDead(1)) {
					continue;
				}
				const int rank = faction ? le::FactionRank(actor, faction) : -1;
				if (!le::IsTeammate(actor) && rank < 0 && !HeldByVanillaQuest(actor)) {
					continue;
				}
				if (Skipped(actor)) {
					spdlog::info("rescue: {:08X} ({}) left alone: listed in sGatherSkip", actor->formID, ActorName(actor));
					continue;
				}
				found.push_back(actor);
			}
		}
		// each one at its own spot in a half circle behind the player: never on top of the player or of each other
		// (several characters dropped on the player's own spot push the player out of the world)
		const float heading = player->rot.z;
		const float n = static_cast<float>(found.size());
		for (std::size_t i = 0; i < found.size(); ++i) {
			auto*       a = found[i];
			const auto  id = a->formID;
			if ((a->flags & 0x800) != 0) {  // disabled
				papyrus::CallMethod(a, "ObjectReference", "Enable", { Arg::Bool(false) });
			}
			const float spread = n > 1 ? (static_cast<float>(i) - (n - 1) / 2.0f) * (2.4f / (std::max)(n - 1.0f, 1.0f)) : 0.0f;
			const float ang = heading + 3.14159265f + spread;
			const float dist = 160.0f + 20.0f * static_cast<float>(i % 2);
			// MoveTo, then EvaluatePackage once it is there (by form id: the reference may be unloaded meanwhile)
			const bool queued = papyrus::CallMethod(a, "ObjectReference", "MoveTo",
				{ Arg::Form(player), Arg::Float(std::sin(ang) * dist), Arg::Float(std::cos(ang) * dist), Arg::Float(0.0f), Arg::Bool(false) },
				[id](const papyrus::Result&) {
					if (auto* f = le::Lookup(id)) {
						papyrus::CallMethod(f, "Actor", "EvaluatePackage", {});
					}
				});
			if (!queued) {
				spdlog::warn("rescue: could not move {:08X} ({})", id, ActorName(a));
				continue;
			}
			names.push_back(ActorName(a));
			spdlog::info("rescue: follower {:08X} ({}) moved to the player (was at {:.0f}, {:.0f}, {:.0f})", id, names.back(), a->pos.x, a->pos.y, a->pos.z);
		}
		spdlog::info("rescue: {} follower(s) gathered", names.size());
		return names;
	}

	void RestoreControls()
	{
		// EnablePlayerControls(movement, fighting, camSwitch, looking, sneaking, menu, activate, journalTabs, disablePOVType)
		papyrus::Args on;
		for (int i = 0; i < 8; ++i) {
			on.push_back(Arg::Bool(true));
		}
		on.push_back(Arg::Int(0));
		papyrus::CallStatic("Game", "EnablePlayerControls", on);
		spdlog::info("rescue: EnablePlayerControls");
	}

	std::string SaveGame()
	{
		const auto now = std::time(nullptr);
		std::tm    tm{};
		localtime_s(&tm, &now);
		char name[64];
		std::strftime(name, sizeof(name), "ReSaverPP_Rescue_%Y-%m-%d_%H%M%S", &tm);
		if (auto* slm = BGSSaveLoadManager::GetSingleton()) {
			slm->Save(name);
			spdlog::info("rescue: saved as {}", name);
		} else {
			spdlog::error("rescue: no save manager; not saved");
		}
		return name;
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
		if (!Game::RunOnMain(
				[st] {
					st->save = SaveGame();
					RestoreControls();
				},
				60s)) {
			return "ReSaver rescue: the game is busy";
		}
		std::this_thread::sleep_for(3s);
		Game::RunOnMain([st] { st->names = GatherFollowers(); }, 60s);
		return std::format("ReSaver rescue: saved as {}, controls back on, {} follower(s) brought to you{}{}", st->save, st->names.size(),
			st->names.empty() ? "" : ": ", JoinNames(st->names));
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
