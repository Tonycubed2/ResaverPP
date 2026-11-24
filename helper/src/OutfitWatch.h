#pragma once
// OutfitWatch (version 23): who dresses a follower in their sleep outfit. Every time a piece of a follower's sleep
// outfit (TESNPC sleepOutfit) or default outfit is put on or taken off, ReSaverHelper.log gets: the follower, the
// piece, which outfit it belongs to, the follower's current AI package (and whether it has "Wear Sleep Outfit"),
// whether they are lying in a bed, the cell, and the call stack as module+offset (TESEquipEvent is sent from inside
// the equip call, so the stack names the game function or the mod's DLL that equipped it).

namespace OutfitWatch
{
	void Install();  // at kDataLoaded
}
