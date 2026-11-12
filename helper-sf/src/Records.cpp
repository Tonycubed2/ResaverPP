#include "PCH.h"
#include "Records.h"

namespace Records
{
	namespace
	{
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

	bool Commands(const std::vector<std::string>& a_record, std::vector<std::string>& a_out, std::string& a_why)
	{
		a_out.clear();
		const auto& kind = a_record.empty() ? std::string() : a_record[0];
		auto        at = [&](std::size_t i) { return i < a_record.size() ? a_record[i] : std::string(); };
		std::string id;
		long long   n = 0;
		static const std::vector<std::string> kinds = { "additem", "removeitem", "addperk", "setstage", "completequest", "objective", "moveto" };
		if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) {
			a_why = "unknown record: " + kind;
			return false;
		}
		if (!FormID(at(1), id)) {
			a_why = "not a form id: " + at(1);
			return false;
		}
		if (kind == "additem" || kind == "removeitem") {
			if (!Number(at(2), 1, 1000000, n)) {
				a_why = "not a count (1 to 1000000): " + at(2);
				return false;
			}
			a_out.push_back(std::format("player.{} {} {}", kind, id, n));
		} else if (kind == "addperk") {
			a_out.push_back(std::format("player.addperk {}", id));
		} else if (kind == "setstage") {
			if (!Number(at(2), 0, 65535, n)) {
				a_why = "not a stage (0 to 65535): " + at(2);
				return false;
			}
			a_out.push_back(std::format("setstage {} {}", id, n));
		} else if (kind == "completequest") {
			a_out.push_back(std::format("completequest {}", id));
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
			if (state & 1) {
				a_out.push_back(std::format("setobjectivedisplayed {} {} 1", id, n));
			}
			if (state == 2 || state == 3) {
				a_out.push_back(std::format("setobjectivecompleted {} {} 1", id, n));
			} else if (state == 4 || state == 5) {
				a_out.push_back(std::format("setobjectivefailed {} {} 1", id, n));
			}
		} else if (kind == "moveto") {
			a_out.push_back(std::format("{}.moveto player", id));
			a_out.push_back(std::format("{}.evp", id));
		}
		return true;
	}
}
