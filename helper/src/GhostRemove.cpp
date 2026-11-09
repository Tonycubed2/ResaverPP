#include "PCH.h"
#include "GhostRemove.h"

namespace GhostRemove
{
	namespace
	{
		// one console line on a_ref, the way the console runs it on its selected reference (see GhostRemove.h)
		bool ConsoleOn(RE::TESObjectREFR* a_ref, const char* a_command)
		{
			const auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
			auto*      script = factory ? factory->Create() : nullptr;
			if (!script) {
				spdlog::error("ghost: could not create a script for '{}'", a_command);
				return false;
			}
			script->SetCommand(a_command);
			script->CompileAndRun(a_ref);
			delete script;
			return true;
		}
	}

	const char* Word(Result a_result)
	{
		switch (a_result) {
		case Result::kDone: return "done";
		case Result::kNotFound: return "not found";
		case Result::kNotCreated: return "not a created actor";
		default: return "not marked";
		}
	}

	Result Remove(RE::FormID a_id)
	{
		spdlog::info("ghost {:08X}: looking it up", a_id);
		if ((a_id >> 24) != 0xFF) {
			spdlog::info("ghost {:08X}: not a created id (FF), left alone", a_id);
			return Result::kNotCreated;
		}
		// the untyped lookup: no RTTI cast, only the form map
		auto* form = RE::TESForm::LookupByID(a_id);
		if (!form) {
			spdlog::info("ghost {:08X}: not found in the game", a_id);
			return Result::kNotFound;
		}
		const auto type = form->GetFormType();
		if (type != RE::FormType::ActorCharacter) {
			spdlog::info("ghost {:08X}: form type {} is not an actor reference (ACHR), left alone", a_id, static_cast<int>(type));
			return Result::kNotCreated;
		}
		// the game's own accessor, never a cast (TESForm vfunc 0x2B; Character's, 0x2f3d60 on 1.6.1170, returns this)
		auto* ref = form->AsReference();
		if (!ref) {
			spdlog::info("ghost {:08X}: not a reference, left alone", a_id);
			return Result::kNotCreated;
		}
		spdlog::info("ghost {:08X}: form flags {:08X}; console 'disable' on it next", a_id, form->GetFormFlags());
		ConsoleOn(ref, "Disable");
		spdlog::info("ghost {:08X}: 'disable' done (the game disables it on a coming frame); console 'markfordelete' on it next", a_id);
		ConsoleOn(ref, "MarkForDelete");
		const bool deleted = form->IsDeleted();
		spdlog::info("ghost {:08X}: 'markfordelete' done; delete flag {}", a_id, deleted ? "set" : "NOT set");
		return deleted ? Result::kDone : Result::kNotMarked;
	}
}
