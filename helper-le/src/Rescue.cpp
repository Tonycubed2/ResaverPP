#include "PCH.h"
#include "Rescue.h"
#include "Game.h"
#include "LE.h"
#include "Papyrus.h"
#include "Settings.h"

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
				found.push_back(actor);
			}
		}
		for (auto* a : found) {
			const auto id = a->formID;
			if ((a->flags & 0x800) != 0) {  // disabled
				papyrus::CallMethod(a, "ObjectReference", "Enable", { Arg::Bool(false) });
			}
			// MoveTo, then EvaluatePackage once it is there (by form id: the reference may be unloaded meanwhile)
			const bool queued = papyrus::CallMethod(a, "ObjectReference", "MoveTo",
				{ Arg::Form(player), Arg::Float(0.0f), Arg::Float(0.0f), Arg::Float(0.0f), Arg::Bool(true) },
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
			spdlog::info("rescue: follower {:08X} ({}) moved to the player", id, names.back());
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
					Game::RunOnMain(
						[] {
							const auto msg = RunAll();
							spdlog::info("{}", msg);
							Game::Notify("hud", msg);
						},
						60s);
				} else if (!warned && need >= 5 && secs >= need - 3) {
					warned = true;
					Game::Notify("hud", "ReSaver rescue: keep holding Ctrl to save, get your controls back and bring your followers");
				}
			}
		}).detach();
	}
}
