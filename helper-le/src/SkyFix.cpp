#include "PCH.h"
#include "SkyFix.h"
#include "LE.h"
#include "Papyrus.h"

namespace SkyFix
{
	namespace
	{
		using papyrus::Arg;

		constexpr std::uint32_t kDialogueFollower = 0x000750BA;     // Skyrim.esm
		constexpr std::uint32_t kPlayerWerewolfQuest = 0x0002BA16;  // Skyrim.esm
		constexpr std::uint32_t kDLC1PlayerVampireQuest = 0x0071D0;  // Dawnguard.esm local id

		Actor* ActorOf(std::uint32_t a_id)
		{
			return le::Cast<Actor>(le::Lookup(a_id), RTTI_TESForm, RTTI_Actor);
		}

		std::string NameOf(Actor* a)
		{
			auto name = le::NameOf(le::BaseNPC(a));
			if (name.empty()) {
				name = le::NameOf(a);
			}
			return name.empty() ? std::format("{:08X}", a ? a->formID : 0) : name;
		}

		// the actor in the quest's reference alias with this alias id (GetAliasedRef on SE). The aliases are TESQuest
		// +0x30 (count +0x38) and the id is BGSBaseAlias +0x0C: the game's own lookup by id (0x56B6F0, behind the
		// Papyrus native Quest.GetAlias 0x9152C0) reads them so.
		Actor* AliasActor(TESQuest* a_quest, UInt32 a_aliasID)
		{
			for (UInt32 i = 0; i < a_quest->aliases.count; ++i) {
				auto* alias = a_quest->aliases.arr.entries[i];
				if (alias && alias->aliasId == a_aliasID) {
					auto* ref = le::Cast<BGSRefAlias>(alias, RTTI_BGSBaseAlias, RTTI_BGSRefAlias);
					return ref ? ref->GetActorReference() : nullptr;
				}
			}
			return nullptr;
		}
	}

	std::string SetFollower(std::uint32_t a_actor)
	{
		auto* quest = le::LookupAs<TESQuest>(kDialogueFollower);
		auto* actor = ActorOf(a_actor);
		if (!quest) {
			return "the vanilla follower quest is not in this game";
		}
		if (!actor || actor == le::Player()) {
			return std::format("{:08X} is not a character in this game", a_actor);
		}
		if (actor->IsDead(1)) {
			return NameOf(actor) + " is dead";
		}
		if (!le::Running(quest)) {
			return "the vanilla follower quest is not running";
		}
		// SetFollower forces the actor into the alias on top of whoever is there: only when nobody living holds it
		if (auto* other = AliasActor(quest, 0); other && other != actor && !other->IsDead(1)) {
			return NameOf(other) + " is your follower now: dismiss them first";
		}
		if (!papyrus::CallMethod(quest, "DialogueFollowerScript", "SetFollower", { Arg::Form(actor) })) {
			return "the vanilla follower quest has no DialogueFollowerScript";
		}
		spdlog::info("follower: DialogueFollowerScript.SetFollower({:08X} {}) dispatched", a_actor, NameOf(actor));
		return {};
	}

	std::string Teammate(std::uint32_t a_actor, bool a_on)
	{
		auto* actor = ActorOf(a_actor);
		if (!actor || actor == le::Player()) {
			return std::format("{:08X} is not a character in this game", a_actor);
		}
		if (!papyrus::CallMethod(actor, "Actor", "SetPlayerTeammate", { Arg::Bool(a_on), Arg::Bool(true) })) {
			return "no script object for this character";
		}
		spdlog::info("follower: {:08X} ({}) SetPlayerTeammate({}) dispatched", a_actor, NameOf(actor), a_on);
		return {};
	}

	std::string BeastRevert(const std::string& a_which)
	{
		const bool wolf = a_which == "werewolf";
		if (!wolf && a_which != "vampire") {
			return "werewolf or vampire";
		}
		auto* quest = wolf ? le::LookupAs<TESQuest>(kPlayerWerewolfQuest) :
		                     static_cast<TESQuest*>(le::Lookup("Dawnguard.esm", kDLC1PlayerVampireQuest));
		if (quest && quest->formType != kFormType_Quest) {
			quest = nullptr;
		}
		if (!quest) {
			return wolf ? "PlayerWerewolfQuest is not in this game" : "DLC1PlayerVampireQuest is not in this game (Dawnguard)";
		}
		const char* script = wolf ? "PlayerWerewolfChangeScript" : "DLC1PlayerVampireChangeScript";
		// ShiftBack / ActuallyShiftBackIfNecessary / Shutdown each return at once when their guard is already set
		for (const char* guard : { "__tryingToShiftBack", "__shiftingBack", "__shuttingDown" }) {
			bool      was = false;
			const int r = papyrus::SetBoolVariable(quest, script, guard, false, &was);
			if (r < 0) {
				return std::string("the quest has no ") + script;
			}
			if (r > 0) {
				spdlog::info("beast: {}.{} was {}, set to false", script, guard, was);
			} else {
				spdlog::warn("beast: {} has no Bool variable {}", script, guard);
			}
		}
		if (!papyrus::CallMethod(quest, script, "ShiftBack", {})) {
			return std::string("the game did not take the call to ") + script + ".ShiftBack";
		}
		spdlog::info("beast: {}.ShiftBack() dispatched ({} running {})", script, le::EditorID(quest), le::Running(quest));
		return {};
	}
}
