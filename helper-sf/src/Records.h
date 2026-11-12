#pragma once
#include <string>
#include <vector>

// ReSaverHelper for Starfield, version 2: what ReSaver Plus can ask the running game to do beyond the quest commands.
// Each record kind maps to fixed console commands; the arguments are checked (form ids are 8 hex digits, numbers are
// plain decimal in range), so nothing but these commands can be run through the pipe. The commands and their opcodes
// were read in Starfield.exe 1.16.244 (the script function table the game builds at start, e.g. 0x1400c60cb MoveTo):
//
//   additem <formid> <count>          player.additem <formid> <count>       (AddItem 0x1002)
//   removeitem <formid> <count>       player.removeitem <formid> <count>    (RemoveItem 0x1052)
//   addperk <formid>                  player.addperk <formid>               (AddPerk 0x1176)
//   setstage <quest> <stage>          setstage <quest> <stage>              (SetStage 0x1039)
//   completequest <quest>             completequest <quest>                 (CompleteQuest 0x1071)
//   objective <quest> <index> <state> state 1..5: setobjectivedisplayed <quest> <index> 1 (1, 3, 5) (0x11a2), then
//                                     setobjectivecompleted <quest> <index> 1 (2, 3) (0x11a1) or
//                                     setobjectivefailed <quest> <index> 1 (4, 5) (0x1184)
//   moveto <ref>                      <ref>.moveto player (MoveTo 0x109e), then <ref>.evp (EvaluatePackage 0x105e):
//                                     the reference is put next to the player and an actor picks its AI package again
//
// The server also checks a moveto reference in the game first (Game::RefKind): it must be a placed object or an actor.
namespace Records
{
	// a_record: the kind and its fields. False with a_why when the record is not one of the above or a field is wrong.
	bool Commands(const std::vector<std::string>& a_record, std::vector<std::string>& a_out, std::string& a_why);

	// exactly 8 hex digits, not 00000000
	bool FormID(const std::string& a_text, std::string& a_out);
}
