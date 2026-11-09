#pragma once
// ReSaverHelper <-> ReSaver++ protocol, over the local named pipe \\.\pipe\ReSaverHelper (owner-only access).
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
//   FOLLOWERSET <formid hex> <notify>     -> OK  1 | OK 0 <why not>   (DialogueFollowerScript.SetFollower on the vanilla follower
//                                         quest; refused while somebody living is in its Follower alias; version 13)
//   TEAMMATE <formid hex> <0|1> <notify>  -> OK  1 | OK 0 <why not>   (Actor.SetPlayerTeammate; version 13)
//   BEASTREVERT <werewolf|vampire> <notify> -> OK 1 | OK 0 <why not>  (the change script's guards set back to false, then its
//                                         own ShiftBack(): PlayerWerewolfChangeScript / DLC1PlayerVampireChangeScript; version 13)
//   GHOSTREMOVE <refid hex>[,<refid hex>...] [<notify>]
//                                         -> OK  <count>  <one field per id: refid*result>   (version 15; result: done, not found,
//                                         not a created actor, not marked. Ghost actors (GhostRemove.h): for each created (FF)
//                                         actor, the console's "disable" then "markfordelete" on it, on the main thread, one id
//                                         at a time, each step logged first)
// <notify>: hud (top-left notification), box (message box), none, or ini (the helper's own setting).
#include <algorithm>
#include <cstdint>
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

	// "FF003D2A,ff0040df FF003307" -> the ids (hex, separated by commas, spaces, tabs or new lines; "0x" allowed; an
	// optional "[...]" around each kept from a crash log; anything that is not a hex id of up to 8 digits is skipped)
	inline std::vector<std::uint32_t> ParseIdList(const std::string& a_text)
	{
		std::vector<std::uint32_t> out;
		std::string                tok;
		auto                       flush = [&] {
			std::string t = tok;
			tok.clear();
			if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
				t = t.substr(2);
			}
			if (t.empty() || t.size() > 8) {
				return;
			}
			std::uint32_t v = 0;
			for (char c : t) {
				const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
				if (d < 0) {
					return;
				}
				v = v * 16 + static_cast<std::uint32_t>(d);
			}
			if (v && std::find(out.begin(), out.end(), v) == out.end()) {
				out.push_back(v);
			}
		};
		for (char c : a_text) {
			if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ';' || c == '[' || c == ']' || c == '(' || c == ')') {
				flush();
			} else {
				tok += c;
			}
		}
		flush();
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
