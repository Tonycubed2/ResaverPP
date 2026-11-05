#pragma once

// Transplant for Fallout 4 (helper 13): ReSaver Plus reads the old save and the good one from their files, writes
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
//
// The names and argument lists are the ones in Fallout4.exe 1.11.240's script function table.
namespace Transplant
{
	// Checks one record (fields after "APPLY") and turns it into its console command(s). False with a_why
	// when the record is not one of the kinds above or an argument is out of range.
	bool Commands(const std::vector<std::string>& a_record, std::vector<std::string>& a_out, std::string& a_why);
}
