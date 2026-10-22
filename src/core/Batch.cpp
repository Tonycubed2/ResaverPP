// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Batch.h"

#include "Ops.h"

#include <cctype>
#include <set>
#include <sstream>
#include <unordered_map>

namespace ess::batch
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}
		std::string Trim(const std::string& s)
		{
			const auto a = s.find_first_not_of(" \t\r\n");
			if (a == std::string::npos) {
				return {};
			}
			return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
		}
	}

	std::vector<Match> Resolve(const SaveFile& a_save, const std::string& a_list)
	{
		std::vector<Match> out;
		const auto*        p = a_save.papyrus();
		if (!p) {
			return out;
		}
		std::unordered_map<std::string, std::size_t> inst, thr;
		for (const auto& i : p->instances) {
			inst[Lower(p->str(i.script))]++;
		}
		for (const auto& a : p->activeScripts) {
			if (!a.data) {
				continue;
			}
			std::set<std::string> seen;
			for (const auto& f : a.data->frames) {
				if (seen.insert(Lower(p->str(f.scriptName))).second) {
					thr[Lower(p->str(f.scriptName))]++;
				}
			}
		}
		std::set<std::string> taken;
		std::istringstream    in(a_list);
		for (std::string raw; std::getline(in, raw);) {
			auto line = Trim(raw);
			if (line.empty() || line[0] == ';' || line[0] == '#') {
				continue;
			}
			std::string prompt;
			if (auto at = line.find("@@"); at != std::string::npos) {
				prompt = Trim(line.substr(at + 2));
				line = Trim(line.substr(0, at));
			}
			if (auto sp = line.find_first_of(" \t"); sp != std::string::npos) {
				line = line.substr(0, sp);
			}
			if (Lower(line).ends_with(".pex")) {
				line = line.substr(0, line.size() - 4);
			}
			if (line.empty()) {
				continue;
			}
			const bool prefix = line.back() == '*';
			const auto key = Lower(prefix ? line.substr(0, line.size() - 1) : line);
			for (const auto& s : p->scripts) {
				const auto& name = p->str(s.name);
				const auto  low = Lower(name);
				if (prefix ? low.rfind(key, 0) == 0 : low == key) {
					if (!taken.insert(low).second) {
						continue;
					}
					Match m;
					m.script = name;
					m.fromLine = Trim(raw);
					m.prompt = prompt;
					m.instances = inst[low];
					m.threads = thr[low];
					m.selected = prompt.empty();
					out.push_back(std::move(m));
				}
			}
		}
		return out;
	}

	Result Apply(SaveFile& a_save, const std::vector<Match>& a_matches)
	{
		Result r;
		for (const auto& m : a_matches) {
			if (!m.selected) {
				continue;
			}
			const auto x = ops::PurgeScript(a_save, m.script, false);
			++r.scripts;
			r.threads += x.threads;
			r.stacks += x.stacks;
			r.messages += x.messages;
			r.instances += x.instances;
		}
		return r;
	}
}
