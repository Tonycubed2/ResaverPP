#include "PCH.h"
#include "SkyFix.h"
#include "Game.h"

namespace SkyFix
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;

		constexpr RE::FormID kDialogueFollower = 0x000750BA;     // Skyrim.esm
		constexpr RE::FormID kPlayerWerewolfQuest = 0x0002BA16;  // Skyrim.esm
		constexpr RE::FormID kDLC1PlayerVampireQuest = 0x0071D0;  // Dawnguard.esm local id

		RE::VMHandle HandleOf(RE::TESForm* a_form, RE::VMTypeID a_type)
		{
			auto* vm = VM::GetSingleton();
			auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
			return policy && a_form ? policy->GetHandleForObject(a_type, a_form) : 0;
		}

		std::string NameOf(RE::Actor* a)
		{
			const char* n = a ? a->GetDisplayFullName() : nullptr;
			return n && *n ? std::string(n) : std::format("{:08X}", a ? a->GetFormID() : 0);
		}
	}

	std::string SetFollower(RE::FormID a_actor)
	{
		auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(kDialogueFollower);
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
		if (!quest) {
			return "the vanilla follower quest is not in this game";
		}
		if (!actor || actor == RE::PlayerCharacter::GetSingleton()) {
			return std::format("{:08X} is not a character in this game", a_actor);
		}
		if (actor->IsDead()) {
			return NameOf(actor) + " is dead";
		}
		if (!Game::Running(quest)) {
			return "the vanilla follower quest is not running";
		}
		// SetFollower forces the actor into the alias on top of whoever is there: only when nobody living holds it
		if (auto held = quest->GetAliasedRef(0).get()) {
			auto* other = held->As<RE::Actor>();
			if (other && other != actor && !other->IsDead()) {
				return NameOf(other) + " is your follower now: dismiss them first";
			}
		}
		auto*      vm = VM::GetSingleton();
		const auto h = HandleOf(quest, static_cast<RE::VMTypeID>(RE::TESQuest::FORMTYPE));
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
		if (!vm || !h ||
			!vm->DispatchMethodCall(h, "DialogueFollowerScript", "SetFollower", RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(actor)), none)) {
			return "the vanilla follower quest has no DialogueFollowerScript";
		}
		spdlog::info("follower: DialogueFollowerScript.SetFollower({:08X} {}) dispatched", a_actor, NameOf(actor));
		return {};
	}

	std::string Teammate(RE::FormID a_actor, bool a_on)
	{
		auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
		if (!actor || actor == RE::PlayerCharacter::GetSingleton()) {
			return std::format("{:08X} is not a character in this game", a_actor);
		}
		auto*      vm = VM::GetSingleton();
		const auto h = HandleOf(actor, static_cast<RE::VMTypeID>(RE::Character::FORMTYPE));
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
		if (!vm || !h || !vm->DispatchMethodCall(h, "Actor", "SetPlayerTeammate", RE::MakeFunctionArguments(a_on, true), none)) {
			return "no script object for this character";
		}
		spdlog::info("follower: {:08X} ({}) SetPlayerTeammate({}) dispatched", a_actor, NameOf(actor), a_on);
		return {};
	}

	std::string BeastRevert(const std::string& a_which)
	{
		const bool    wolf = a_which == "werewolf";
		RE::TESQuest* quest = nullptr;
		if (wolf) {
			quest = RE::TESForm::LookupByID<RE::TESQuest>(kPlayerWerewolfQuest);
		} else if (auto* dh = RE::TESDataHandler::GetSingleton()) {
			quest = dh->LookupForm<RE::TESQuest>(kDLC1PlayerVampireQuest, "Dawnguard.esm");
		}
		if (!quest) {
			return wolf ? "PlayerWerewolfQuest is not in this game" : "DLC1PlayerVampireQuest is not in this game (Dawnguard)";
		}
		const char* script = wolf ? "PlayerWerewolfChangeScript" : "DLC1PlayerVampireChangeScript";
		auto*       vm = VM::GetSingleton();
		const auto  h = HandleOf(quest, static_cast<RE::VMTypeID>(RE::TESQuest::FORMTYPE));
		RE::BSTSmartPointer<RE::BSScript::Object> obj;
		if (!vm || !h || !vm->FindBoundObject(h, script, obj) || !obj) {
			return std::string("the quest has no ") + script;
		}
		// ShiftBack / ActuallyShiftBackIfNecessary / Shutdown each return at once when their guard is already set
		for (const char* guard : { "__tryingToShiftBack", "__shiftingBack", "__shuttingDown" }) {
			if (auto* v = obj->GetVariable(guard); v && v->IsBool()) {
				spdlog::info("beast: {}.{} was {}, set to false", script, guard, v->GetBool());
				v->SetBool(false);
			}
		}
		RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
		if (!vm->DispatchMethodCall(h, script, "ShiftBack", RE::MakeFunctionArguments(), none)) {
			return std::string("the game did not take the call to ") + script + ".ShiftBack";
		}
		spdlog::info("beast: {}.ShiftBack() dispatched ({} running {})", script, quest->GetFormEditorID(), Game::Running(quest));
		return {};
	}
}
