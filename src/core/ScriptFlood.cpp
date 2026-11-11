// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Script-call floods (see ScriptFlood.h for what was verified in the game).
#include "ScriptFlood.h"

#include "Ops.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace ess::flood
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

		// the thread ids something other than suspended entries holds or names
		struct Named
		{
			std::unordered_map<ThreadID, std::size_t> suspendedEntries;  // entries per thread
			std::unordered_set<ThreadID>               byMessage, byNextStack, byLock;
			std::optional<ThreadID>                    extraSlot;
			bool named(ThreadID t) const
			{
				const auto it = suspendedEntries.find(t);
				return (it != suspendedEntries.end() && it->second > 1) || byMessage.contains(t) || byNextStack.contains(t) ||
				       byLock.contains(t) || (extraSlot && *extraSlot == t);
			}
		};

		Named Collect(const Papyrus& p)
		{
			Named n;
			for (const auto* list : { &p.suspended1, &p.suspended2 }) {
				for (const auto& s : *list) {
					n.suspendedEntries[s.id]++;
				}
			}
			for (const auto& m : p.functionMessages) {
				if (m.id) {
					n.byMessage.insert(*m.id);
				}
			}
			for (const auto& a : p.activeScripts) {
				if (a.data && (a.data->flag & 1)) {  // Stack::Load 0x14337b0: flag bit 0 = a next stack id follows
					n.byNextStack.insert(a.data->unknown2);
				}
			}
			auto lock = [&](const std::optional<InstanceData>& d) {
				if (d && (d->flag & 4)) {  // Object::Load 0x1422890: flag 4 = the lock holder's stack id follows
					n.byLock.insert(d->unknown2);
				}
			};
			for (const auto& i : p.instances) {
				lock(i.data);
			}
			for (const auto& r : p.references) {
				lock(r.data);
			}
			if (p.unk1 != 0 && p.unk2) {
				n.extraSlot = *p.unk2;
			}
			return n;
		}

		enum class Kind
		{
			kNotStarted,
			kRunning,
			kOther
		};

		Kind Classify(const Papyrus& p, const Named& a_named, ThreadID a_id)
		{
			const auto* a = p.findActive(a_id);
			if (!a || !a->data) {
				return Kind::kOther;
			}
			const auto& d = *a->data;
			if (!d.frames.empty()) {
				return Kind::kRunning;
			}
			// the resume path makes the call for states 2, 4 and 8 (0x144f8a0, jump table 0x144fec0)
			const bool callState = d.major == 2 || d.major == 4 || d.major == 8;
			if (a->type != 0 || d.fragment || (d.flag & 1) || !callState || a_named.named(a_id)) {
				return Kind::kOther;
			}
			return Kind::kNotStarted;
		}

		bool IsAliasScript(const Papyrus& p, TStr a_script)
		{
			static const char* const kAlias[] = { "Alias", "ReferenceAlias", "LocationAlias", "RefCollectionAlias" };
			TStr s = a_script;
			for (int depth = 0; depth < 64; ++depth) {
				const auto& name = p.str(s);
				for (const char* k : kAlias) {
					if (EqualsNoCase(name, k)) {
						return true;
					}
				}
				const auto* sc = p.findScript(s);
				if (!sc || sc->parent == s || p.str(sc->parent).empty()) {
					return false;
				}
				s = sc->parent;
			}
			return false;
		}

		struct Group
		{
			Flood                                 f;
			std::unordered_map<EID, std::size_t>  byObject;
			std::vector<ThreadID>                 clearable;
		};

		// every queued call grouped by script.event (lower case key)
		std::unordered_map<std::string, Group> Groups(const Papyrus& p, const Named& a_named)
		{
			std::unordered_map<std::string, Group> g;
			auto group = [&](const FunctionMessageData& d) -> Group& {
				auto& e = g[Lower(p.str(d.scriptName)) + "." + Lower(p.str(d.event))];
				if (e.f.script.empty()) {
					e.f.script = p.str(d.scriptName);
					e.f.event = p.str(d.event);
				}
				if (d.unknownVar.type == VType::kRef && d.unknownVar.value != 0) {
					e.byObject[d.unknownVar.value]++;
				}
				return e;
			};
			for (const auto* list : { &p.suspended1, &p.suspended2 }) {
				for (const auto& s : *list) {
					if (!s.data) {
						continue;  // no call of its own: a running thread's own wait (it has frames)
					}
					auto& e = group(*s.data);
					++e.f.suspended;
					switch (Classify(p, a_named, s.id)) {
					case Kind::kNotStarted:
						++e.f.notStarted;
						e.clearable.push_back(s.id);
						break;
					case Kind::kRunning: ++e.f.running; break;
					default: ++e.f.other; break;
					}
				}
			}
			for (const auto& m : p.functionMessages) {
				if (m.data) {
					++group(*m.data).f.messages;
				}
			}
			for (const auto& a : p.activeScripts) {
				if (a.data && !a.data->frames.empty()) {
					const auto& f0 = a.data->frames.front();
					if (const auto it = g.find(Lower(p.str(f0.scriptName)) + "." + Lower(p.str(f0.event))); it != g.end()) {
						++it->second.f.runningNow;
					}
				}
			}
			return g;
		}

		Target MakeTarget(const SaveFile& a_save, const Papyrus& p, EID a_obj, std::size_t a_calls)
		{
			Target t;
			t.object = a_obj;
			t.calls = a_calls;
			const auto* in = p.findInstance(a_obj);
			if (!in) {
				return t;
			}
			t.found = true;
			t.script = p.str(in->script);
			t.form = a_save.formIDOf(in->refID);
			t.plugin = t.form ? a_save.pluginOf(t.form) : std::string();
			// handle parts as saved: unknown2bits 2 = an active effect (unknown = its unique id, form = the actor);
			// 0 with unknown != 0xFFFF on an Alias script = an alias (unknown = its alias id, form = the quest)
			if (in->unknown2bits == 2) {
				t.effect = true;
			} else if (in->unknown2bits == 0 && in->unknown != 0xFFFF && IsAliasScript(p, in->script)) {
				t.alias = true;
				t.aliasID = in->unknown;
			}
			return t;
		}
	}

	std::vector<Flood> Find(const SaveFile& a_save, std::size_t a_over)
	{
		std::vector<Flood> out;
		const auto*        p = a_save.papyrus();
		if (!p || p->invalid) {
			return out;
		}
		const auto named = Collect(*p);
		for (auto& [k, e] : Groups(*p, named)) {
			if (e.f.total() <= a_over) {
				continue;
			}
			std::vector<std::pair<std::size_t, EID>> objs;
			for (const auto& [o, n] : e.byObject) {
				objs.push_back({ n, o });
			}
			std::sort(objs.begin(), objs.end(), [](const auto& x, const auto& y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
			e.f.objects = objs.size();
			for (std::size_t i = 0; i < objs.size() && i < 5; ++i) {
				e.f.targets.push_back(MakeTarget(a_save, *p, objs[i].second, objs[i].first));
			}
			out.push_back(std::move(e.f));
		}
		std::sort(out.begin(), out.end(), [](const Flood& a, const Flood& b) { return a.total() != b.total() ? a.total() > b.total() : a.key() < b.key(); });
		return out;
	}

	ClearResult Clear(SaveFile& a_save, const std::set<std::string>& a_keys, std::size_t a_over)
	{
		ClearResult r;
		auto*       p = a_save.papyrus();
		if (!p || p->invalid) {
			return r;
		}
		std::set<std::string> want;
		for (const auto& k : a_keys) {
			want.insert(Lower(k));
		}
		const auto         named = Collect(*p);
		std::set<ThreadID> ids;
		for (auto& [k, e] : Groups(*p, named)) {
			if (e.f.total() <= a_over || (!want.empty() && !want.contains(k))) {
				continue;
			}
			r.floods.push_back(e.f.key());
			r.leftRunning += e.f.running;
			r.leftOther += e.f.other;
			ids.insert(e.clearable.begin(), e.clearable.end());
		}
		std::sort(r.floods.begin(), r.floods.end());
		if (ids.empty()) {
			return r;
		}
		// the v64 delete: each thread goes with its suspended entry and everything else naming it
		const auto d = ops::RemoveSuspendedStacks(*p, ids);
		r.calls = d.stacks;
		r.threads = d.threads;
		r.messages = d.messages;
		return r;
	}

	Reread ReadBack(const SaveFile& a_save)
	{
		Reread      out;
		const auto* p = a_save.papyrus();
		if (!p) {
			out.error = "no Papyrus section";
			return out;
		}
		try {
			Writer w;
			w.le = w.id32 = a_save.isLE();
			w.str16 = a_save.str16();
			w.fo4 = a_save.isFO4();
			p->write(w);  // (Starfield: writeStarfield, which sets its own widths)
			Reader r(w.buf.data(), w.buf.size());
			r.le = r.id32 = a_save.isLE();
			r.str16 = a_save.str16();
			r.fo4 = a_save.isFO4();
			Papyrus back;
			if (p->starfield) {
				back.readStarfield(r);
			} else {
				back.read(r);
			}
			out.ok = true;
			out.threads = back.activeScripts.size();
			out.suspended = back.suspended1.size() + back.suspended2.size();
			out.messages = back.functionMessages.size();
			out.stranded = back.invalid ? 0 : ops::StrandedThreads(back).size();
		} catch (const std::exception& e) {
			out.error = e.what();
		}
		return out;
	}

	std::string TargetText(const Target& a_t)
	{
		char buf[256];
		if (!a_t.found) {
			std::snprintf(buf, sizeof(buf), "object %llX (not in the save)", static_cast<unsigned long long>(a_t.object));
		} else if (a_t.alias) {
			std::snprintf(buf, sizeof(buf), "%s on alias %u of quest %08X (%s)", a_t.script.c_str(), a_t.aliasID, a_t.form, a_t.plugin.c_str());
		} else if (a_t.effect) {
			std::snprintf(buf, sizeof(buf), "%s on a magic effect on actor %08X (%s)", a_t.script.c_str(), a_t.form, a_t.plugin.c_str());
		} else {
			std::snprintf(buf, sizeof(buf), "%s on form %08X (%s)", a_t.script.c_str(), a_t.form, a_t.plugin.c_str());
		}
		return buf;
	}
}
