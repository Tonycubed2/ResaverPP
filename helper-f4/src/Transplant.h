#pragma once

// Transplant for Fallout 4 (helper 13; objectives from 14): ReSaver Plus reads the old save and the good one from their files, writes
// what a save can hold into the good save itself, and sends the rest here as APPLY records once the good save is
// loaded in the game. Each record kind maps to ONE fixed console command; the arguments are checked (form ids are
// 8 hex digits, numbers are plain decimal in range), so nothing but these commands can be run through the pipe.
// ReSaver Plus works out the form ids from the game's load order (it reads a save the game has just made).
//
//   additem <formid> <count>     player.additem <formid> <count>     (AddItem, script function 0x1002)
//   addperk <formid> <ranks>     player.addperk <formid>, <ranks> times (AddPerk 0x1176: each call gives the
//                                                                   perk's next rank, up to its number of ranks)
//   setstage <quest> <stage>     setstage <quest> <stage>            (SetStage 0x1039)
//   completequest <quest>        completequest <quest>               (CompleteQuest 0x1071)
//   objective <quest> <index> <state>   (version 14) the objective's state as the old save had it, state 1..5:
//                                setobjectivedisplayed <quest> <index> 1  when the state shows it (1, 3, 5)
//                                (SetObjectiveDisplayed 0x11a2), then
//                                setobjectivecompleted <quest> <index> 1  for 2 and 3 (SetObjectiveCompleted 0x11a1) or
//                                setobjectivefailed <quest> <index> 1     for 4 and 5 (SetObjectiveFailed 0x1184)
//
// Names, opcodes and argument lists checked in the script function tables of Fallout4.exe 1.10.163, 1.10.984,
// 1.11.240 (and 1.1.29): the same in all. The commands' code: 1.10.984 is instruction for instruction 1.11.240's;
// 1.10.163 is compiled differently but does the same (see README.md, "What was checked").
namespace Transplant
{
	// Checks one record (fields after "APPLY") and turns it into its console command(s). False with a_why
	// when the record is not one of the kinds above or an argument is out of range.
	bool Commands(const std::vector<std::string>& a_record, std::vector<std::string>& a_out, std::string& a_why);
}
