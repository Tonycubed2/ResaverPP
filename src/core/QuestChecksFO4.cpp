// ReSaver++ - Fallout 4 save check: quests whose last stage script stopped early (see QuestChecksFO4.h).
#include "QuestChecksFO4.h"

#include "Papyrus.h"
#include "PluginDB.h"
#include "Quests.h"
#include "ScriptSources.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace ess::pexfo4
{
	namespace
	{
		struct R
		{
			const std::vector<std::uint8_t>& b;
			std::size_t                      o = 0;
			void need(std::size_t n) const
			{
				if (o + n > b.size()) {
					throw std::runtime_error("unexpected end of the .pex");
				}
			}
			std::uint8_t  u8() { need(1); return b[o++]; }
			std::uint16_t u16() { need(2); std::uint16_t v; std::memcpy(&v, b.data() + o, 2); o += 2; return v; }
			std::uint32_t u32() { need(4); std::uint32_t v; std::memcpy(&v, b.data() + o, 4); o += 4; return v; }
			void          skip(std::size_t n) { need(n); o += n; }
			std::string   ws()
			{
				const auto n = u16();
				need(n);
				std::string s(reinterpret_cast<const char*>(b.data() + o), n);
				o += n;
				return s;
			}
		};
		// opcodes 0x00..0x2e and their fixed argument counts (Fallout 4)
		struct Op
		{
			const char* name;
			int         args;
		};
		constexpr Op kOps[] = { { "nop", 0 }, { "iadd", 3 }, { "fadd", 3 }, { "isub", 3 }, { "fsub", 3 }, { "imul", 3 }, { "fmul", 3 },
			{ "idiv", 3 }, { "fdiv", 3 }, { "imod", 3 }, { "not", 2 }, { "ineg", 2 }, { "fneg", 2 }, { "assign", 2 }, { "cast", 2 },
			{ "cmp_eq", 3 }, { "cmp_lt", 3 }, { "cmp_le", 3 }, { "cmp_gt", 3 }, { "cmp_ge", 3 }, { "jmp", 1 }, { "jmpt", 2 },
			{ "jmpf", 2 }, { "callmethod", 3 }, { "callparent", 2 }, { "callstatic", 3 }, { "return", 1 }, { "strcat", 3 },
			{ "propget", 3 }, { "propset", 3 }, { "array_create", 2 }, { "array_length", 2 }, { "array_getelement", 3 },
			{ "array_setelement", 3 }, { "array_findelement", 4 }, { "array_rfindelement", 4 }, { "is", 3 }, { "struct_create", 1 },
			{ "struct_get", 3 }, { "struct_set", 3 }, { "array_findstruct", 5 }, { "array_rfindstruct", 5 }, { "array_add", 3 },
			{ "array_insert", 3 }, { "array_removelast", 1 }, { "array_remove", 3 }, { "array_clear", 1 } };

		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}
	}

	Script Read(const std::vector<std::uint8_t>& a_data)
	{
		R r{ a_data };
		if (r.u32() != 0xFA57C0DE) {
			throw std::runtime_error("not a Fallout 4 .pex (little-endian magic missing)");
		}
		r.u8();   // major
		r.u8();   // minor
		r.u16();  // game id
		r.skip(8);
		r.ws();
		r.ws();
		r.ws();
		std::vector<std::string> st(r.u16());
		for (auto& s : st) {
			s = r.ws();
		}
		auto S = [&]() -> const std::string& {
			const auto i = r.u16();
			if (i >= st.size()) {
				throw std::runtime_error("string index out of range");
			}
			return st[i];
		};
		if (r.u8()) {  // debug info
			r.skip(8);
			for (auto n = r.u16(); n; --n) {
				r.skip(7);
				r.skip(2ull * r.u16());
			}
			for (auto n = r.u16(); n; --n) {  // property groups
				r.skip(10);
				r.skip(2ull * r.u16());
			}
			for (auto n = r.u16(); n; --n) {  // struct orders
				r.skip(4);
				r.skip(2ull * r.u16());
			}
		}
		for (auto n = r.u16(); n; --n) {  // user flags
			r.skip(3);
		}
		auto val = [&]() -> std::string {
			switch (r.u8()) {
			case 0: return "None";
			case 1: return S();
			case 2: return "\"" + S() + "\"";
			case 3: return std::to_string(static_cast<std::int32_t>(r.u32()));
			case 4:
				{
					const auto u = r.u32();
					float      f;
					std::memcpy(&f, &u, 4);
					char b[32];
					std::snprintf(b, sizeof(b), "%g", f);
					return b;
				}
			case 5: return r.u8() ? "True" : "False";
			default: throw std::runtime_error("unknown value type");
			}
		};
		Script out;
		auto   func = [&](const std::string& a_name) {
            S();          // return type
            S();          // doc
            r.skip(5);    // user flags, flags
            r.skip(4ull * r.u16());  // parameters (name, type)
            r.skip(4ull * r.u16());  // locals
            std::vector<Instr> body;
            for (auto n = r.u16(); n; --n) {
                const auto op = r.u8();
                if (op >= std::size(kOps)) {
                    throw std::runtime_error("unknown opcode");
                }
                Instr in;
                in.op = kOps[op].name;
                for (int k = 0; k < kOps[op].args; ++k) {
                    in.args.push_back(val());
                }
                if (op == 23 || op == 24 || op == 25) {
                    const auto c = val();
                    const auto m = std::strtol(c.c_str(), nullptr, 10);
                    for (long k = 0; k < m; ++k) {
                        in.extra.push_back(val());
                    }
                }
                body.push_back(std::move(in));
            }
            auto& f = out.functions[Lower(a_name)];
            if (f.empty()) {
                f = std::move(body);
            }
		};
		for (auto objects = r.u16(); objects; --objects) {
			out.name = S();
			r.u32();  // size
			out.parent = S();
			S();       // doc
			r.u8();    // const
			r.u32();   // user flags
			S();       // auto state
			for (auto n = r.u16(); n; --n) {  // structs
				S();
				for (auto m = r.u16(); m; --m) {
					S();
					S();
					r.u32();
					val();
					r.u8();
					S();
				}
			}
			for (auto n = r.u16(); n; --n) {  // variables
				S();
				S();
				r.u32();
				val();
				r.u8();
			}
			for (auto n = r.u16(); n; --n) {  // properties
				const auto name = S();
				S();
				S();
				r.u32();
				const auto fl = r.u8();
				if (fl & 4) {
					S();
				} else {
					if (fl & 1) {
						func("get_" + name);
					}
					if (fl & 2) {
						func("set_" + name);
					}
				}
			}
			for (auto n = r.u16(); n; --n) {  // states
				S();
				for (auto m = r.u16(); m; --m) {
					const auto fn = S();
					func(fn);
				}
			}
		}
		return out;
	}
}

namespace ess::fo4quest
{
	namespace
	{
		std::string Fmt(const char* f, auto... args)
		{
			char buf[2048];
			std::snprintf(buf, sizeof(buf), f, args...);
			return buf;
		}
		bool Is(const std::string& a, const char* b) { return EqualsNoCase(a, b); }
		bool Number(const std::string& s, std::int32_t& a_out)
		{
			if (s.empty()) {
				return false;
			}
			char*      e = nullptr;
			const long v = std::strtol(s.c_str(), &e, 10);
			if (*e) {
				return false;
			}
			a_out = static_cast<std::int32_t>(v);
			return true;
		}
		enum class Call
		{
			kNone,
			kStage,
			kDisplayed,
			kCompleted,
			kFailed
		};
		// a call on the quest itself with a number literal first: SetStage(n), SetObjectiveXxx(n, ...)
		Call QuestCall(const pexfo4::Instr& in, std::int32_t& a_n, bool& a_value)
		{
			if (in.op != "callmethod" || in.args.size() < 2 || !Is(in.args[1], "self") || in.extra.empty() || !Number(in.extra[0], a_n)) {
				return Call::kNone;
			}
			a_value = in.extra.size() < 2 || !(in.extra[1] == "False" || in.extra[1] == "0");
			const auto& m = in.args[0];
			if (Is(m, "SetStage")) {
				return Call::kStage;
			}
			if (Is(m, "SetObjectiveDisplayed")) {
				return Call::kDisplayed;
			}
			if (Is(m, "SetObjectiveCompleted")) {
				return Call::kCompleted;
			}
			if (Is(m, "SetObjectiveFailed")) {
				return Call::kFailed;
			}
			return Call::kNone;
		}
	}

	std::vector<Finding> CheckQuest(const quests::QuestState& q, const quests::QuestDef& a_def, const pexfo4::Script& script, std::size_t* a_checks)
	{
		std::vector<Finding> out;
		const auto*          d = &a_def;
		auto                 lower = [](std::string s) {
            for (auto& c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
		};
		std::size_t checks = 0;
			// how many functions of the script set each stage / objective
			std::map<std::pair<bool, std::int32_t>, std::set<std::string>> touch;
			for (const auto& [fn, body] : script.functions) {
				for (const auto& in : body) {
					std::int32_t n = 0;
					bool         v = true;
					const auto   c = QuestCall(in, n, v);
					if (c != Call::kNone) {
						touch[{ c != Call::kStage, n }].insert(fn);
					}
				}
			}
			std::set<std::int32_t> done;
			for (const auto& [s, isDone] : q.stages) {
				if (isDone) {
					done.insert(s);
				}
			}
			auto objectiveState = [&](std::int32_t a_index) -> std::int32_t {
				for (const auto& o : q.objectives) {
					if (o.index == a_index) {
						return o.state;
					}
				}
				return -1;
			};
			auto defined = [&](bool a_objective, std::int32_t n) {
				if (a_objective) {
					return std::any_of(d->objectives.begin(), d->objectives.end(), [&](const auto& o) { return o.index == n; });
				}
				return std::any_of(d->stages.begin(), d->stages.end(), [&](const auto& s) { return s.index == n; });
			};
			const std::string questName = d->edid.empty() ? Fmt("%08X", q.formID) : d->edid;
			for (const auto& fr : d->fragments) {
				if (!done.contains(fr.stage)) {
					continue;
				}
				const auto f = script.functions.find(lower(fr.function));
				if (f == script.functions.end()) {
					continue;
				}
				for (const auto& in : f->second) {
					if (in.op == "jmp" || in.op == "jmpt" || in.op == "jmpf" || in.op == "return") {
						break;  // only the straight-line start always runs
					}
					std::int32_t n = 0;
					bool         v = true;
					const auto   c = QuestCall(in, n, v);
					if (c == Call::kNone || c == Call::kFailed || !v) {
						continue;
					}
					const bool obj = c != Call::kStage;
					const auto t = touch.find({ obj, n });
					if (t == touch.end() || t->second.size() != 1 || !defined(obj, n)) {
						continue;
					}
					++checks;
					Finding x;
					x.quest = q.formID;
					x.edid = d->edid;
					x.stage = fr.stage;
					x.function = fr.function;
					x.target = n;
					const auto where = Fmt("Its stage %u is done, and that stage's script (%s.%s) always", fr.stage, d->fragmentScript.c_str(), fr.function.c_str());
					if (c == Call::kStage) {
						if (done.contains(n)) {
							continue;
						}
						x.kind = Finding::Kind::kStageNotSet;
						x.title = Fmt("Quest %s is stuck after stage %u: stage %d never ran", questName.c_str(), fr.stage, n);
						x.what = where + Fmt(" sets stage %d (no other part of it does), but stage %d is not done. The stage script stopped "
						                     "before that line (an error: often something it needs is gone, from a removed or changed mod), "
						                     "so the quest cannot go on by itself. Setting the stage in the game runs it as the script would have:",
						                    n, n);
						x.console.push_back(Fmt("setstage %s %d", questName.c_str(), n));
					} else {
						const auto s = objectiveState(n);
						x.state = s;
						if (c == Call::kDisplayed) {
							if (s >= 1) {
								continue;
							}
							x.kind = Finding::Kind::kObjectiveNotShown;
							x.title = Fmt("Quest %s is stuck after stage %u: objective %d was never shown", questName.c_str(), fr.stage, n);
							x.what = where + Fmt(" shows objective %d (no other part of it does), but the save has it %s. The stage script "
							                     "stopped before that line, so whatever came after it in the script did not happen either. "
							                     "In the game:",
							                    n, s < 0 ? "never touched" : "dormant");
							x.console.push_back(Fmt("setobjectivedisplayed %s %d 1", questName.c_str(), n));
						} else {
							if (s == 2 || s == 3) {
								continue;
							}
							x.kind = Finding::Kind::kObjectiveNotCompleted;
							x.title = Fmt("Quest %s is stuck after stage %u: objective %d was never completed", questName.c_str(), fr.stage, n);
							x.what = where + Fmt(" completes objective %d (no other part of it does), but the save has it %s. The stage "
							                     "script stopped before that line. In the game:",
							                    n, quests::ObjectiveStateText(s < 0 ? 0 : s));
							x.console.push_back(Fmt("setobjectivecompleted %s %d 1", questName.c_str(), n));
						}
					}
					out.push_back(std::move(x));
				}
			}
				if (a_checks) {
			*a_checks += checks;
		}
		return out;
	}

	std::vector<Finding> FindStoppedStages(const SaveFile& a_save, const PluginDB& a_db, const ScriptSources& a_sources, Stats* a_stats)
	{
		std::vector<Finding> out;
		Stats                st;
		if (!a_save.isFO4() || !a_save.papyrus()) {
			return out;
		}
		const auto& p = *a_save.papyrus();
		// scripts running or queued in the save (lower case)
		std::set<std::string> busy;
		auto                  lower = [](std::string s) {
            for (auto& c : s) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return s;
		};
		for (const auto& a : p.activeScripts) {
			if (a.data) {
				for (const auto& f : a.data->frames) {
					busy.insert(lower(p.str(f.scriptName)));
				}
			}
		}
		for (const auto& m : p.functionMessages) {
			if (m.data) {
				busy.insert(lower(p.str(m.data->scriptName)));
			}
		}
		for (const auto* list : { &p.suspended1, &p.suspended2 }) {
			for (const auto& s : *list) {
				if (s.data) {
					busy.insert(lower(p.str(s.data->scriptName)));
				}
			}
		}
		std::unordered_map<std::string, std::shared_ptr<pexfo4::Script>> cache;
		for (const auto& q : quests::ReadStates(a_save)) {
			if (!q.decoded || !q.running() || q.completed() || q.failed()) {
				continue;
			}
			const auto* d = a_db.quest(q.formID);
			if (!d || d->fragmentScript.empty() || d->fragments.empty()) {
				continue;
			}
			++st.quests;
			const auto key = lower(d->fragmentScript);
			if (busy.contains(key)) {
				++st.skippedRunning;
				continue;
			}
			auto it = cache.find(key);
			if (it == cache.end()) {
				std::shared_ptr<pexfo4::Script> s;
				if (const auto px = a_sources.findPex(d->fragmentScript)) {
					try {
						s = std::make_shared<pexfo4::Script>(pexfo4::Read(ScriptSources::readPex(*px)));
					} catch (const std::exception&) {
						s = nullptr;
					}
				}
				it = cache.emplace(key, s).first;
				if (s) {
					++st.scriptsRead;
				} else {
					++st.scriptsMissing;
				}
			}
			if (!it->second) {
				continue;
			}
			const auto found = CheckQuest(q, *d, *it->second, &st.checks);
			out.insert(out.end(), found.begin(), found.end());
		}
		if (a_stats) {
			*a_stats = st;
		}
		return out;
	}
}
