#pragma once
// ReSaverHelper <-> ReSaver++ protocol, over the local named pipe \\.\pipe\ReSaverHelper (owner-only access).
// Fallout 4 helper (helper-f4): the same pipe and the same line format; HELLO's game field is F4 (1.10.163),
// F4NG (1.10.984) or F4AE (1.11.x), and it answers HELLO, GETOPTION, SETOPTION, NOTIFY, SAVEGAME, CONTROLS,
// GATHER (the companion and Dogmeat: the game's own FollowersScript.TryToTeleportCompanion / ...Dogmeat; no
// ids), RESCUE and CONTROLSRESET (ResetForceEnabledPlayerControls), and from version 13 APPLY with the Fallout 4
// Transplant records (Transplant.h: additem, addperk, setstage, completequest, from 14 objective; nothing else). Everything else
// answers ERR.
// One request per line, one reply line per request. Fields are separated by tabs; tab, newline and
// backslash inside a field are written as \t, \n and \\.
//
//   request: <id>\t<COMMAND>\t<arg>...          reply: <id>\tOK\t<field>...   or   <id>\tERR\t<message>
//
//   HELLO                                 -> OK  ReSaverHelper  <helper version>  <game version>  <SE|AE|VR|LE>  <in game 0|1>
//                                            (LE: the Legendary Edition helper, helper-le, from version 11)
//   QUEST <formid hex>                    -> OK  <editor id>  <flags hex>  <current stage>  <running 0|1>  <completed 0|1>  <index:state,...>
//   SETSTAGE <formid hex> <stage> <notify> <message>   -> OK <1 set, 0 refused, ? still running>  <running 0|1>  <completed 0|1>  <current stage>
//   COMPLETE <formid hex> <notify> <message>           -> OK <1 if the quest is now completed, 0 if not, ? still running>
//                                         (CompleteAllObjectives + CompleteQuest: no stage script runs; version 3)
//   STOP <formid hex> <notify> <message>  -> OK <1 stopped, 0 not, ? still stopping>  <running 0|1>  <completed 0|1>  <current stage>
//                                         (Quest.Stop: the game cleans up after the quest; version 11)
//   RESET <formid hex> <notify> <message> -> OK <1 reset, 0 not, ? still busy>  <running 0|1>  <completed 0|1>  <current stage>
//                                         (Quest.Reset: back to how it starts, as if never done; version 11)
//   REFRESH <notify>                      -> OK  <objectives to refresh>        (runs in the background, one quest at a time;
//                                            ERR while a menu is open; pauses while one is opened - version 4)
//   PROGRESS                              -> OK  <done>  <total>  <running 0|1>
//   GETOPTION <name>                      -> OK  <value>
//   SETOPTION <name> <value>              -> OK                                 (saved to ReSaverHelper.ini)
//   NOTIFY <notify> <message>             -> OK
//   SNAPSHOT                              -> OK  <file written>  <records>      (version 5: see Transplant.h)
//   APPLY <record fields...>              -> OK  1 | OK 0 <why not> | OK ?     (one snapshot record, applied to the loaded game)
//                                            Fallout 4 (version 13): OK 1 <the console command(s) run>; 1 means the game ran
//                                            the command, not that it did something (the console tells nobody). The records:
//                                              additem <formid 8 hex> <count 1..1000000>   -> player.additem <formid> <count>
//                                              addperk <formid> <ranks 1..100>             -> player.addperk <formid>, <ranks> times
//                                              setstage <quest formid> <stage 0..65535>    -> setstage <quest> <stage>
//                                              completequest <quest formid>                -> completequest <quest>
//                                            (version 14) objective <quest formid> <index 0..65535> <state 1..5>
//                                                -> setobjectivedisplayed <quest> <index> 1 (states 1, 3, 5), then
//                                                   setobjectivecompleted <quest> <index> 1 (2, 3) or setobjectivefailed (4, 5)
//                                            (version 15) the companion fixes (Transplant.h): companion <ref> <0|1> [script],
//                                                companionfollow <ref>, affinity <ref> <n> [script] -> the follower scripts'
//                                                AllowCompanion / FollowerFollow / SetAffinity through cqf and cf
//                                            The form ids are the game's (ReSaver Plus works them out from the load order).
//   RESOLVE <plugin> <local id hex>       -> OK  <runtime form id hex>
//   FOLLOWERS                             -> OK  <count>  <one field per follower: runtime id*Plugin.esp:localid*name*why*where*skip 0|1>
//                                         (version 12.2; skip: listed in sGatherSkip, so the hold-Ctrl rescue leaves it alone)
//   GATHER <notify> [<ids, comma separated>] -> OK  <followers moved>  <their names, comma separated>    (version 12)
//                                         (every follower not in sGatherSkip - or only the ids given (12.2) - moved to its own
//                                          spot in a half circle behind the player, loaded or not)
//   CONTROLS <notify>                     -> OK                          (EnablePlayerControls; version 12)
//   SAVEGAME <notify>                     -> OK  <save name>             (a new save ReSaverPP_Rescue_<date>; version 12)
//   RESCUE <notify>                       -> OK  <one line: what was done>   (SAVEGAME, CONTROLS, GATHER; version 12;
//                                            the same as holding Ctrl in the game for iRescueHoldSeconds)
// <notify>: hud (top-left notification), box (message box), none, or ini (the helper's own setting).
#include <string>
#include <vector>

namespace proto
{
	inline constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\ReSaverHelper";
	inline constexpr int            kHelperVersion = 15;

	inline std::string Escape(const std::string& a_in)
	{
		std::string o;
		for (char c : a_in) {
			switch (c) {
			case '\\': o += "\\\\"; break;
			case '\t': o += "\\t"; break;
			case '\n': o += "\\n"; break;
			case '\r': break;
			default: o += c;
			}
		}
		return o;
	}

	inline std::vector<std::string> Split(const std::string& a_line)
	{
		std::vector<std::string> out(1);
		for (std::size_t i = 0; i < a_line.size(); ++i) {
			const char c = a_line[i];
			if (c == '\t') {
				out.emplace_back();
			} else if (c == '\\' && i + 1 < a_line.size()) {
				const char n = a_line[++i];
				out.back() += n == 't' ? '\t' : n == 'n' ? '\n' : n;
			} else if (c != '\r') {
				out.back() += c;
			}
		}
		return out;
	}

	inline std::string Join(const std::vector<std::string>& a_fields)
	{
		std::string o;
		for (std::size_t i = 0; i < a_fields.size(); ++i) {
			o += (i ? "\t" : "") + Escape(a_fields[i]);
		}
		return o;
	}
}
