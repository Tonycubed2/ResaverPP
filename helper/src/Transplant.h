#pragma once

// Transplant: moving a character's progress from one save to another through the running game.
//
// Snapshot() writes what the loaded game knows about the player to a text file: level, skills, perks,
// spells, shouts and words, factions, map markers, quests, and every inventory item (with its custom
// name, temper, charge, poison and enchantment; enchantments and potions the player made are written
// out effect by effect so they can be made again). Every form is written as its plugin name and local
// form ID, so the file can be applied to another save, even with a different load order.
//
// Apply() takes one line of such a file and makes it so in the loaded game (adds the perk, the item,
// the map marker...). ReSaver++ compares two snapshots and sends the lines the user ticked.
//
// File format: one record per line, fields separated by tabs (escaped as in Protocol.h):
//   RSH-SNAPSHOT  1  <game version>  <player name>
//   plugin   <file name>                                every plugin the game loaded
//   player   <level>  <xp>  <xp needed>  <perk points>
//   skill    <0-17>  <level>  <xp>  <xp needed>  <legendary count>
//   av       <name>  <base value>                      Health Magicka Stamina CarryWeight DragonSouls
//   perk     <plugin>  <local id>  <runtime id>  <name>
//   spell    <plugin>  <local id>  <runtime id>  <name>
//   shout    <plugin>  <local id>  <runtime id>  <name>
//   word     <plugin>  <local id>  <runtime id>  <name>  <known 0|1>  <unlocked 0|1>
//   faction  <plugin>  <local id>  <runtime id>  <name>  <rank>
//   marker   <plugin>  <local id>  <runtime id>  <name>  <flags: 1 visible, 2 can travel to>
//   quest    <plugin>  <local id>  <runtime id>  <editor id|name>  <running>  <completed>  <stage>  <done stages, comma separated>
//   item     <plugin>  <local id>  <runtime id>  <name>  <count>  <custom name>  <temper>  <charge>  <enchantment>  <poison>  <quest item 0|1>
//            enchantment / poison: "" | "F:<plugin>:<local id>:<amount>" (from a plugin) | "C:<kind>:<amount>:<effect>,<effect>..." (made in game;
//            kind w/a = weapon/armour enchantment, amount = max charge; p = poison, amount = count) with effect = "<plugin>:<local id>:<magnitude>:<area>:<duration>"
//   potion   <name>  <count>  <poison 0|1>  <effect>,<effect>...           (a potion or poison the player made)
namespace Transplant
{
	// main thread. Returns the file written, or "" (a_error says why).
	std::filesystem::path Snapshot(std::string& a_error, std::size_t& a_lines);

	// main thread. Applies one record. Returns "" when done, or why not.
	std::string Apply(const std::vector<std::string>& a_record);
}
