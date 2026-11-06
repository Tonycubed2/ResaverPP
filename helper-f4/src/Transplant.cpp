#include "PCH.h"
#include "Transplant.h"

namespace Transplant
{
	namespace
	{
		// exactly 8 hex digits (a form id as ReSaver Plus writes it)
		bool FormID(const std::string& a_text, std::string& a_out)
		{
			if (a_text.size() != 8) {
				return false;
			}
			for (const char c : a_text) {
				if (!std::isxdigit(static_cast<unsigned char>(c))) {
					return false;
				}
			}
			if (a_text == "00000000") {
				return false;
			}
			a_out = a_text;
			return true;
		}

		bool Number(const std::string& a_text, long long a_min, long long a_max, long long& a_out);

		// a Papyrus script name: letters, digits, '_' and ':' (namespaces), 1 to 80 characters; nothing that could end the
		// quoted "Script.Function" argument or start another console command
		bool ScriptName(const std::string& a_text)
		{
			if (a_text.empty() || a_text.size() > 80) {
				return false;
			}
			for (const char c : a_text) {
				if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != ':') {
					return false;
				}
			}
			return true;
		}

		// a decimal number with an optional leading '-' in [a_min, a_max]
		bool Signed(const std::string& a_text, long long a_min, long long a_max, long long& a_out)
		{
			const bool neg = !a_text.empty() && a_text[0] == '-';
			long long  v = 0;
			if (!Number(neg ? a_text.substr(1) : a_text, 0, 9999999999LL, v)) {
				return false;
			}
			v = neg ? -v : v;
			if (v < a_min || v > a_max) {
				return false;
			}
			a_out = v;
			return true;
		}

		// a plain decimal number in [a_min, a_max]
		bool Number(const std::string& a_text, long long a_min, long long a_max, long long& a_out)
		{
			if (a_text.empty() || a_text.size() > 10) {
				return false;
			}
			long long v = 0;
			for (const char c : a_text) {
				if (c < '0' || c > '9') {
					return false;
				}
				v = v * 10 + (c - '0');
			}
			if (v < a_min || v > a_max) {
				return false;
			}
			a_out = v;
			return true;
		}
	}

	bool Commands(const std::vector<std::string>& a_record, std::vector<std::string>& a_out, std::string& a_why)
	{
		a_out.clear();
		const auto& kind = a_record.empty() ? std::string() : a_record[0];
		auto        at = [&](std::size_t i) { return i < a_record.size() ? a_record[i] : std::string(); };
		std::string id;
		long long   n = 0;
		if (kind != "additem" && kind != "addperk" && kind != "setstage" && kind != "completequest" && kind != "objective" &&
			kind != "companion" && kind != "companionfollow" && kind != "affinity" && kind != "clearcompanion") {
			a_why = "unknown record: " + kind;
			return false;
		}
		if (!FormID(at(1), id)) {
			a_why = "not a form id: " + at(1);
			return false;
		}
		if (kind == "additem") {
			if (!Number(at(2), 1, 1000000, n)) {
				a_why = "not a count (1 to 1000000): " + at(2);
				return false;
			}
			a_out.push_back(std::format("player.additem {} {}", id, n));
		} else if (kind == "addperk") {
			if (!Number(at(2), 1, 100, n)) {
				a_why = "not a number of ranks (1 to 100): " + at(2);
				return false;
			}
			for (long long i = 0; i < n; ++i) {
				a_out.push_back(std::format("player.addperk {}", id));
			}
		} else if (kind == "setstage") {
			if (!Number(at(2), 0, 65535, n)) {
				a_why = "not a stage (0 to 65535): " + at(2);
				return false;
			}
			a_out.push_back(std::format("setstage {} {}", id, n));
		} else if (kind == "objective") {
			long long state = 0;
			if (!Number(at(2), 0, 65535, n)) {
				a_why = "not an objective index (0 to 65535): " + at(2);
				return false;
			}
			if (!Number(at(3), 1, 5, state)) {
				a_why = "not an objective state (1 to 5): " + at(3);
				return false;
			}
			if (a_record.size() > 4) {
				a_why = "too many fields for objective";
				return false;
			}
			// the game's own state changes (0x629bc0 in 1.11.240): shown first, then completed or failed, so a dormant
			// objective ends as 3 / 5 (shown) or 2 / 4 (not shown) like in the old save
			if (state & 1) {
				a_out.push_back(std::format("setobjectivedisplayed {} {} 1", id, n));
			}
			if (state == 2 || state == 3) {
				a_out.push_back(std::format("setobjectivecompleted {} {} 1", id, n));
			} else if (state == 4 || state == 5) {
				a_out.push_back(std::format("setobjectivefailed {} {} 1", id, n));
			}
		} else if (kind == "clearcompanion") {
			// (version 15) empty the follower quest's Companion slot (0) or Dogmeat slot (1) the game's way. Both FollowersScript
			// functions take no arguments. ClearCompanion's first step works on the holder as a CompanionActorScript; for a
			// holder without that script it fails (Papyrus error, the function goes on), so the teammate flag it would turn off
			// is turned off with the console's SetPlayerTeammate (0x11c4; actors only, the favor / XP arguments only matter
			// when turning it on). ClearDogmeatCompanion turns it off itself.
			if (!Number(at(2), 0, 1, n)) {
				a_why = "not 0 (companion slot) or 1 (Dogmeat slot): " + at(2);
				return false;
			}
			if (a_record.size() > 3) {
				a_why = "too many fields for clearcompanion";
				return false;
			}
			if (n == 1) {
				a_out.push_back("cqf 000289E4 \"FollowersScript.ClearDogmeatCompanion\"");
			} else {
				a_out.push_back("cqf 000289E4 \"FollowersScript.ClearCompanion\"");
				a_out.push_back(std::format("{}.setplayerteammate 0", id));
			}
		} else if (kind == "companion" || kind == "companionfollow" || kind == "affinity") {
			// (version 15) the companion fixes, through the follower system's own scripts. The follower quest is given by
			// its form id: Followers is 000289E4 in Fallout4.esm, which is always load index 00 (the console's form
			// argument reader takes a hex form id; an editor id only when the game kept that form's editor id)
			const std::size_t scriptAt = kind == "companionfollow" ? 2 : 3;
			std::string       script = "CompanionActorScript";
			if (a_record.size() > scriptAt + 1) {
				a_why = "too many fields for " + kind;
				return false;
			}
			if (kind != "companionfollow" && a_record.size() > scriptAt) {
				if (!ScriptName(at(scriptAt))) {
					a_why = "not a script name: " + at(scriptAt);
					return false;
				}
				script = at(scriptAt);
			} else if (kind == "companionfollow" && a_record.size() > 2) {
				a_why = "too many fields for companionfollow";
				return false;
			}
			if (kind == "companion") {
				if (!Number(at(2), 0, 1, n)) {
					a_why = "not 0 or 1 (left for good): " + at(2);
					return false;
				}
				if (n == 1) {
					a_out.push_back(std::format("{}.cf \"{}.UnsetHasLeftPlayerPermanently\"", id, script));
				}
				a_out.push_back(std::format("cqf 000289E4 \"FollowersScript.AllowCompanion\" {} 1 1", id));
			} else if (kind == "companionfollow") {
				a_out.push_back(std::format("cqf 000289E4 \"FollowersScript.FollowerFollow\" {}", id));
			} else {
				if (!Signed(at(2), -100000, 100000, n)) {
					a_why = "not an affinity (-100000 to 100000): " + at(2);
					return false;
				}
				a_out.push_back(std::format("{}.cf \"{}.SetAffinity\" {}", id, script, n));
			}
		} else {
			a_out.push_back(std::format("completequest {}", id));
		}
		return true;
	}
}
