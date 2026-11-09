#pragma once

// Ghost actors (version 15): created actors (FF) whose base NPC is another actor's temporary leveled base, which the save
// does not contain (ReSaver Plus finds them in the save: core/GhostActors.h). On load such an actor gets no base or another
// actor's base, which can crash the game. The live game cannot tell them apart reliably, so the ids come from ReSaver Plus
// (GHOSTREMOVE) or are typed / pasted on the F1 page.
//
// Remove = exactly the console's "prid <id>", "disable", "markfordelete": each command runs through the game's own
// console route (Script::CompileAndRun with the actor as the target reference, as the console does with its selected
// reference). Verified in SkyrimSE.exe 1.6.1170:
// - The console window (0x8f4270, id 442673) runs a line with CompileAndRun_Impl 0x33d6a0 (CommonLib Script::
//   CompileAndRun_Impl, id 441582), compiler name 1 (kSystemWindowCompiler) and the console's selected ref
//   (0x31acd74 = CommonLib Console selectedRef, id 504099) as the target. CommonLib's Script::CompileAndRun(ref) calls the
//   same function with kSystemWindowCompiler. The compile step (0x3412d0) never reads the target; the run step (0x33d300)
//   reads only its form type (+0x1A, 0x3E = an actor) before the script's command runs on it.
// - Disable: script command table entry 0x1fdc4b0 (opcode 0x1022), execute 0x34cb00 (id 22096): with no enable parent
//   (ExtraEnableStateParent 0x36, via 0x2fc8c0 -> 0x15ffd0), not already disabled (form flag 0x800) and not temporary
//   (0x4000), it calls 0x33f1d0 (id 21913, fade = the optional argument), which queues the reference on the game's
//   disable list; the game disables it on a later frame. (Not CommonLib's TESObjectREFR::Disable, vfunc 0x8A.)
// - MarkForDelete: entry 0x1fe4430 (opcode 0x11ba), execute 0x35c350 (id 22510): refuses no reference, an already deleted
//   one (form flag 0x20) and the player (0x31874f8, PlayerCharacter singleton id 403521); calls SetDelete(true) (TESForm
//   vfunc 0x23; Character's 0x67e390); then, for a reference from no plugin file (TESForm::GetFile(0), 0x1e0420) that is
//   not persistent (flag 0x400, 0x2de550), sets in-game flag kWantsDelete (0x1e14a0 -> AddChange vfunc 0x0A) and puts it
//   on the game's delete list (0x3137f48).
// Touches as little of the actor as possible: LookupByID (the form map), its form id, form type and form flags. Never its
// base, its name or its extra data (GetDisplayFullName crashed before through ExtraLeveledCreature).
namespace GhostRemove
{
	enum class Result
	{
		kDone,        // disable and markfordelete ran; the delete flag is set
		kNotFound,    // no form with that id in the game now
		kNotCreated,  // not a created (FF) actor reference
		kNotMarked,   // the commands ran but the game did not set the delete flag
	};
	// the reply word: "done", "not found", "not a created actor", "not marked"
	const char* Word(Result a_result);

	// main thread. Logs every step to ReSaverHelper.log before doing it.
	Result Remove(RE::FormID a_id);
}
