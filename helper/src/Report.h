#pragma once
// Report::Write (version 19, REPORT): the game-state troubleshooting report of the Snapshot mod, for ReSaver Plus's
// Collect all logs: the game time, weather and open menus; the player (position, place, state, every actor value,
// perks, bounties, inventory, spells, active effects, factions); every follower; every NPC near the player (same
// interior, or within 10000 units outdoors) with its current AI package, packages, factions and stats; every running
// quest with its stage, objectives, playing scenes and filled aliases; the completed quests; every actor in memory with
// its place and factions; the load order and the SKSE plugin DLLs. Main thread only (it reads live game data).
// Written to Documents\My Games\<game>\SKSE\ReSaverHelper\report_<date>.txt.

namespace Report
{
	std::filesystem::path Write(std::string& a_error, std::size_t& a_lines);
}
