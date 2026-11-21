#pragma once
// BodyWatch (version 22): characters that are there but cannot be seen. A follower (or a dead body) whose 3D - the body
// the game draws - was taken away while the character itself stayed in the world: the save still has them next to the
// player, the game no longer draws them. Seen in a load order where followers turned invisible and bodies vanished
// 1-3 s after fights.
//  - Hooks Character::Set3D (vfunc 0x6C): when a follower's or a dead body's 3D is removed near the player, the log
//    gets who removed it: the call stack as module+offset (a mod's DLL, or only SkyrimSE.exe: the game itself or a
//    Papyrus script), and again when the 3D comes back.
//  - Every second: a follower near the player that has no 3D, is hidden (culled), faded out or switched off is logged
//    once per change.
//  - The F1 Followers page shows each follower's body state, with "Rebuild body" (switch off and on again, as the
//    console's disable / enable: the game loads the 3D again).

namespace BodyWatch
{
	void Install();  // at kDataLoaded
	// "shown", "NO BODY (3D not loaded)", "hidden (culled)", "faded out (alpha 0.00)", "switched off (disabled)", ...
	std::string Describe(RE::Actor* a_actor, bool* a_problem = nullptr);
	// disable now, enable 0.5 s later (main thread); false when it is not a character
	bool Rebuild(RE::FormID a_id);
}
