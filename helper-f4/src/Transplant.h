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
// Version 15, the companion fixes of ReSaver Plus's Companions window (the follower system's own Papyrus functions,
// called with the console's CallFunction "cf" 0x028b / CallQuestFunction "cqf" 0x028c; [script] is the companion's
// script, CompanionActorScript unless a mod companion uses one extending it):
//   companion <ref> <leftForGood 0|1> [script]
//                                <ref>.cf "<script>.UnsetHasLeftPlayerPermanently"        (only for 1), then
//                                cqf 000289E4 "FollowersScript.AllowCompanion" <ref> 1 1  (out of DisallowedCompanionFaction,
//                                                                   then SetCompanion / SetDogmeatCompanion by race)
//   companionfollow <ref>        cqf 000289E4 "FollowersScript.FollowerFollow" <ref>      (FollowerState = follow)
//   affinity <ref> <n> [script]  <ref>.cf "<script>.SetAffinity" <n>  (n -100000..100000; the script's own ModAffinity)
// 000289E4 is the Followers quest (Fallout4.esm, always load index 00). How the console reads these (1.10.163 0x4e7620,
// 1.10.984 0x561ae0, 1.11.240 0x5b5d90, the same code in 984 and 1.11.240): a form argument is "player", else a form
// whose editor id the game kept, else _strtoui64(text, 16) -> a form id; so a hex form id always works, an editor id
// only for forms the game keeps editor ids of. cf / cqf hand the strings to the Papyrus call (1.11.240 0x60b2c0 ->
// 0x60c5c0: the function name is split at '.' into script and function; 0x602b70 converts each argument to the
// function's parameter type: a form by "none" / "player" / "selected" / hex form id (0x60aa80), a float by atof, an int
// by atoi, a bool false for "0", "false" or ""). The same functions in 1.10.984 (0x5b8360, 0x5b6820: instruction for
// instruction) and 1.10.163 (0x533770 splits at '.', 0x533420 the same argument order).
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
