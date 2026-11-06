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
		if (kind != "additem" && kind != "addperk" && kind != "setstage" && kind != "completequest" && kind != "objective") {
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
		} else {
			a_out.push_back(std::format("completequest {}", id));
		}
		return true;
	}
}
