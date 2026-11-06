// ReSaver++ - Fallout 4 settlements (see WorkshopFO4.h for where the data lives and the addresses).
#include "WorkshopFO4.h"

#include "ChangeFormData.h"
#include "Ops.h"
#include "Papyrus.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace ess::fo4ws
{
	namespace
	{
		// BGSLoadFormBuffer vsval (0xc0fd30)
		std::uint32_t Vs(Reader& r)
		{
			const std::uint8_t a = *r.here();
			switch (a & 3) {
			case 0:
				return r.u8() >> 2;
			case 1:
				return r.u16() >> 2;
			default:
				{
					const std::uint32_t b0 = r.u8(), b1 = r.u8(), b2 = r.u8();
					return (b0 | (b1 << 8) | (b2 << 16)) >> 2;
				}
			}
		}
		void PutVs(std::vector<std::uint8_t>& a_out, std::uint32_t a_v)
		{
			if (a_v < 0x40) {
				a_out.push_back(static_cast<std::uint8_t>(a_v << 2));
			} else if (a_v < 0x4000) {
				const std::uint32_t x = (a_v << 2) | 1;
				a_out.push_back(static_cast<std::uint8_t>(x));
				a_out.push_back(static_cast<std::uint8_t>(x >> 8));
			} else {
				const std::uint32_t x = (a_v << 2) | 2;
				a_out.push_back(static_cast<std::uint8_t>(x));
				a_out.push_back(static_cast<std::uint8_t>(x >> 8));
				a_out.push_back(static_cast<std::uint8_t>(x >> 16));
			}
		}
		void PutRef(std::vector<std::uint8_t>& a_out, RefID a_r)
		{
			a_out.push_back(static_cast<std::uint8_t>(a_r.data >> 16));
			a_out.push_back(static_cast<std::uint8_t>(a_r.data >> 8));
			a_out.push_back(static_cast<std::uint8_t>(a_r.data));
		}
		void PutF32(std::vector<std::uint8_t>& a_out, float a_v)
		{
			std::uint8_t b[4];
			std::memcpy(b, &a_v, 4);
			a_out.insert(a_out.end(), b, b + 4);
		}

		std::string Lower(std::string a)
		{
			for (auto& c : a) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a;
		}
		// the script or one of its parents has this name (the string table keeps the game's own spelling, any case)
		bool Extends(const Papyrus& p, TStr a_script, const char* a_lower)
		{
			int depth = 0;
			for (const Script* s = p.findScript(a_script); s && depth < 32; s = p.findScript(s->parent), ++depth) {
				if (Lower(p.str(s->name)) == a_lower) {
					return true;
				}
				if (p.str(s->parent).empty()) {
					break;
				}
			}
			return false;
		}
		const Variable* Var(const Papyrus& p, const ScriptInstance& a_i, const char* a_lower)
		{
			if (!a_i.data) {
				return nullptr;
			}
			const auto names = ops::ExtendedMembers(p, a_i.script);
			for (std::size_t k = 0; k < names.size() && k < a_i.data->vars.size(); ++k) {
				if (Lower(names[k]) == a_lower) {
					return &a_i.data->vars[k];
				}
			}
			return nullptr;
		}
		// the form an object variable points at (the reference id of the instance it names), 0 for none
		std::uint32_t ObjectForm(const SaveFile& a_save, const Papyrus& p, const Variable* v)
		{
			if (!v || v->type != VType::kRef || !v->value) {
				return 0;
			}
			const auto* i = p.findInstance(v->value);
			return i ? a_save.formIDOf(i->refID) : 0;
		}

		// the WorkshopItemKeyword link of a decoded reference: (has one, target form id, raw target)
		struct Link
		{
			bool          has = false;
			std::uint32_t target = 0;
			RefID         raw;
		};
		Link WorkshopLink(const SaveFile& a_save, const std::vector<std::uint8_t>& a_body, const fo4cf::Refr& a_d)
		{
			Link out;
			for (const auto& e : a_d.extras) {
				if (e.type != kExtraLinkedRef) {
					continue;
				}
				Reader r(a_body.data() + e.range.begin + 1, e.range.size() - 1);
				for (auto n = Vs(r); n; --n) {
					const auto kw = RefID::read(r), target = RefID::read(r);
					if (a_save.formIDOf(kw) == kWorkshopItemKeyword) {
						out.has = true;
						out.raw = target;
						out.target = a_save.formIDOf(target);
					}
				}
			}
			return out;
		}

		// the save's reference for a Fallout4.esm form below 0x400000 (as ChangeFormFO4Actor's AddPerk writes them),
		// or its index in the form id array; 0 when it has none
		RefID RefFor(const SaveFile& a_save, std::uint32_t a_formID)
		{
			RefID r;
			if ((a_formID >> 24) == 0 && a_formID < 0x400000u) {
				r.data = 0x400000u | a_formID;
				return r;
			}
			for (std::size_t i = 0; i < a_save.formIDs.size() && i + 1 < 0x400000u; ++i) {
				if (a_save.formIDs[i] == a_formID) {
					r.data = static_cast<std::uint32_t>(i + 1);
					return r;
				}
			}
			return r;
		}

		std::string Hex(std::uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "%08X", v);
			return b;
		}
	}

	bool AvStorage::hasBase(std::uint32_t a_av) const
	{
		return std::any_of(bases.begin(), bases.end(), [&](const AvBase& b) { return b.formID == a_av; });
	}
	float AvStorage::base(std::uint32_t a_av) const
	{
		// ActorValueStorage::LoadGame keeps one entry per actor value (0x1659940 finds the slot): the last one read wins
		float v = 0;
		for (const auto& b : bases) {
			if (b.formID == a_av) {
				v = b.base;
			}
		}
		return v;
	}
	float AvStorage::modifiers(std::uint32_t a_av) const
	{
		float v = 0;
		for (const auto& m : mods) {
			if (m.formID == a_av) {
				v = m.mod[0] + m.mod[1] + m.mod[2];
			}
		}
		return v;
	}

	std::optional<AvStorage> ReadStorage(const std::vector<std::uint8_t>& a_body, const fo4cf::Refr& a_refr, const SaveFile& a_save)
	{
		AvStorage s;
		for (const auto& e : a_refr.extras) {
			if (e.type != kExtraActorValues) {
				continue;
			}
			s = AvStorage{};
			s.present = true;
			s.range = e.range;
			try {
				Reader r(a_body.data(), e.range.end);
				r.seek(e.range.begin + 1);
				for (auto n = Vs(r); n; --n) {
					AvBase b;
					b.range.begin = r.pos();
					b.av = RefID::read(r);
					b.formID = a_save.formIDOf(b.av);
					b.base = r.f32();
					b.range.end = r.pos();
					s.bases.push_back(b);
				}
				for (auto n = Vs(r); n; --n) {
					AvMod m;
					m.range.begin = r.pos();
					m.av = RefID::read(r);
					m.formID = a_save.formIDOf(m.av);
					for (auto& x : m.mod) {
						x = r.f32();
					}
					m.range.end = r.pos();
					s.mods.push_back(m);
				}
				if (r.pos() != e.range.end) {
					return std::nullopt;
				}
			} catch (const std::exception&) {
				return std::nullopt;
			}
		}
		return s;
	}

	double Settlement::meter() const
	{
		const double d = static_cast<double>(static_cast<float>(static_cast<std::uint32_t>(usedDraws()))) / static_cast<double>(limitDraws());
		const double t = static_cast<double>(static_cast<float>(static_cast<std::uint32_t>(usedTriangles()))) / static_cast<double>(limitTriangles());
		return std::max(d, t);
	}

	namespace
	{
		// The workshops, and every reference / actor with a WorkshopItemKeyword link handed to a_sink (its change form
		// type, its form, the link): one pass over the change forms.
		using LinkSink = std::function<void(ChangeFormType, std::uint32_t, const Link&)>;
		std::vector<Settlement> Scan(const SaveFile& a_save, const LinkSink& a_sink)
		{
			std::vector<Settlement> out;
			const auto*             p = a_save.papyrus();
			if (!a_save.isFO4() || !p) {
				return out;
			}
			std::unordered_map<std::uint32_t, std::size_t> byForm;
			std::set<TStr>                                   workshopScripts, other;
			for (const auto& i : p->instances) {
				if (!workshopScripts.contains(i.script)) {
					if (other.contains(i.script)) {
						continue;
					}
					(Extends(*p, i.script, "workshopscript") ? workshopScripts : other).insert(i.script);
					if (!workshopScripts.contains(i.script)) {
						continue;
					}
				}
				const auto form = a_save.formIDOf(i.refID);
				if (!form || byForm.contains(form)) {
					continue;
				}
				Settlement s;
				s.formID = form;
				s.script = p->str(i.script);
				s.location = ObjectForm(a_save, *p, Var(*p, i, "::mylocation_var"));
				if (const auto* v = Var(*p, i, "workshopid"); v && v->type == VType::kInt) {
					s.workshopID = static_cast<std::int32_t>(static_cast<std::uint32_t>(v->value));
				}
				if (const auto* v = Var(*p, i, "::ownedbyplayer_var"); v && v->type == VType::kBool) {
					s.ownedByPlayer = v->value != 0;
				}
				byForm[form] = out.size();
				out.push_back(std::move(s));
			}
			if (out.empty() && !a_sink) {
				return out;
			}
			for (const auto& c : a_save.changeForms) {
				const auto t = static_cast<ChangeFormType>(c.typeCode());
				if (t != ChangeFormType::REFR && t != ChangeFormType::ACHR) {
					continue;
				}
				const auto form = a_save.formIDOf(c.refID);
				const auto it = byForm.find(form);
				const bool workshop = it != byForm.end();
				// links are counted on every reference and actor (their reference part)
				const auto body = cf::Body(c);
				const auto d = fo4cf::DecodeRefr(a_save, c, body);
				if (workshop) {
					auto& s = out[it->second];
					s.hasChangeForm = true;
					s.readable = t == ChangeFormType::REFR && d.complete;
					if (!d.decoded || (t == ChangeFormType::REFR && !d.complete)) {
						s.error = d.error.empty() ? "cannot be read" : d.error;
					}
					if (d.decoded) {
						for (const auto& e : d.extras) {
							s.hasWorkshopExtra |= e.type == kExtraWorkshop;
						}
						if (const auto st = ReadStorage(body, d, a_save)) {
							s.hasStorage = st->present;
							s.currentDraws = st->value(kAvCurrentDraws);
							s.currentTriangles = st->value(kAvCurrentTriangles);
							s.maxDraws = st->value(kAvMaxDraws);
							s.maxTriangles = st->value(kAvMaxTriangles);
							s.population = st->value(kAvPopulation);
							s.happiness = st->value(kAvHappiness);
						} else {
							s.readable = false;
							s.error = "its actor values (extra 0xC2) cannot be read";
						}
					}
				}
				if (d.decoded) {
					const auto l = WorkshopLink(a_save, body, d);
					if (l.has && l.target) {
						if (const auto w = byForm.find(l.target); w != byForm.end()) {
							(t == ChangeFormType::ACHR ? out[w->second].linkedActors : out[w->second].linkedObjects) += 1;
						}
					}
					if (l.has && a_sink) {
						a_sink(t, form, l);
					}
				}
			}
			return out;
		}
	}

	std::vector<Settlement> List(const SaveFile& a_save) { return Scan(a_save, nullptr); }

	std::string SetUsed(SaveFile& a_save, std::uint32_t a_workshop, std::optional<float> a_draws, std::optional<float> a_triangles, bool* a_changed)
	{
		if (a_changed) {
			*a_changed = false;
		}
		if (!a_save.isFO4()) {
			return "not a Fallout 4 save";
		}
		for (auto v : { a_draws, a_triangles }) {
			if (v && (!std::isfinite(*v) || *v < 0 || *v > 2147483520.0f)) {
				return "the used budget must be a number from 0 up";
			}
		}
		ChangeForm* cf = nullptr;
		for (auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::REFR && a_save.formIDOf(c.refID) == a_workshop) {
				cf = &c;
				break;
			}
		}
		if (!cf) {
			return "the save has no changed reference " + Hex(a_workshop) + " (its budget is the plugin's: nothing used)";
		}
		const auto body = cf::Body(*cf);
		const auto d = fo4cf::DecodeRefr(a_save, *cf, body);
		if (!d.complete) {
			return "reference " + Hex(a_workshop) + " cannot be read completely (" + d.error + "): nothing changed";
		}
		const auto st = ReadStorage(body, d, a_save);
		if (!st) {
			return "the actor values of " + Hex(a_workshop) + " cannot be read: nothing changed";
		}
		if (!st->present) {
			// no saved actor values: every value is its default, 0
			if ((a_draws && *a_draws != 0) || (a_triangles && *a_triangles != 0)) {
				return "reference " + Hex(a_workshop) + " has no saved actor values (its used budget is 0); setting a number is not supported";
			}
			return {};
		}
		struct Want
		{
			std::uint32_t av;
			float         base;
		};
		std::vector<Want> want;
		if (a_draws) {
			want.push_back({ kAvCurrentDraws, *a_draws - st->modifiers(kAvCurrentDraws) });
		}
		if (a_triangles) {
			want.push_back({ kAvCurrentTriangles, *a_triangles - st->modifiers(kAvCurrentTriangles) });
		}
		// the new entry: bases in their order with the wanted ones replaced (every entry of that value), missing ones
		// appended; the modifiers copied byte for byte
		std::vector<std::uint8_t> ex;
		ex.push_back(kExtraActorValues);
		std::vector<std::pair<RefID, float>> bases;
		bool                                 same = true;
		for (const auto& b : st->bases) {
			float v = b.base;
			for (const auto& w : want) {
				if (b.formID == w.av) {
					v = w.base;
				}
			}
			same &= std::memcmp(&v, &b.base, 4) == 0;
			bases.push_back({ b.av, v });
		}
		for (const auto& w : want) {
			if (!st->hasBase(w.av)) {
				const auto ref = RefFor(a_save, w.av);
				if (ref.isZero()) {
					return "the save has no reference for actor value " + Hex(w.av);
				}
				bases.push_back({ ref, w.base });
				same = false;
			}
		}
		if (same) {
			return {};
		}
		PutVs(ex, static_cast<std::uint32_t>(bases.size()));
		for (const auto& [r, v] : bases) {
			PutRef(ex, r);
			PutF32(ex, v);
		}
		PutVs(ex, static_cast<std::uint32_t>(st->mods.size()));
		for (const auto& m : st->mods) {
			ex.insert(ex.end(), body.begin() + static_cast<std::ptrdiff_t>(m.range.begin), body.begin() + static_cast<std::ptrdiff_t>(m.range.end));
		}
		std::vector<std::uint8_t> nb(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(st->range.begin));
		nb.insert(nb.end(), ex.begin(), ex.end());
		nb.insert(nb.end(), body.begin() + static_cast<std::ptrdiff_t>(st->range.end), body.end());

		// read it back: complete, the same extras in the same order, everything outside the entry unchanged, and the
		// values as wanted (all others as they were)
		ChangeForm copy = *cf;
		cf::SetBody(copy, nb);
		const auto again = fo4cf::DecodeRefr(a_save, copy, nb);
		bool       ok = again.complete && again.extras.size() == d.extras.size();
		for (std::size_t k = 0; ok && k < d.extras.size(); ++k) {
			ok = again.extras[k].type == d.extras[k].type;
		}
		const auto delta = static_cast<std::ptrdiff_t>(nb.size()) - static_cast<std::ptrdiff_t>(body.size());
		ok = ok && std::equal(body.begin() + static_cast<std::ptrdiff_t>(st->range.end), body.end(),
		                      nb.begin() + static_cast<std::ptrdiff_t>(st->range.end) + delta);
		std::optional<AvStorage> st2;
		if (ok) {
			st2 = ReadStorage(nb, again, a_save);
			ok = st2 && st2->present && st2->mods.size() == st->mods.size() && st2->bases.size() == bases.size();
		}
		for (const auto& w : want) {
			ok = ok && std::fabs(st2->value(w.av) - (w.av == kAvCurrentDraws ? *a_draws : *a_triangles)) < 0.5f;
		}
		if (ok) {
			for (const auto& b : st->bases) {
				const bool edited = std::any_of(want.begin(), want.end(), [&](const Want& w) { return w.av == b.formID; });
				ok = ok && (edited || st2->base(b.formID) == st->base(b.formID));
			}
		}
		if (!ok) {
			return "the edited reference " + Hex(a_workshop) + " did not read back: nothing changed";
		}
		*cf = std::move(copy);
		if (a_changed) {
			*a_changed = true;
		}
		return {};
	}

	std::vector<Finding> Check(const SaveFile& a_save)
	{
		std::vector<Finding> out;
		const auto*          p = a_save.papyrus();
		if (!a_save.isFO4() || !p) {
			return out;
		}
		// WorkshopParentScript.Workshops: index -> workshop reference (0: None)
		std::vector<std::uint32_t> workshops;
		bool                       haveArray = false;
		for (const auto& i : p->instances) {
			if (!Extends(*p, i.script, "workshopparentscript")) {
				continue;
			}
			const auto* v = Var(*p, i, "::workshops_var");
			if (!v || v->type != VType::kRefArray) {
				continue;
			}
			const auto* a = p->findArray(v->value);
			if (!a || !a->hasData) {
				continue;
			}
			haveArray = true;
			workshops.clear();
			for (const auto& e : a->vars) {
				workshops.push_back(ObjectForm(a_save, *p, &e));
			}
			break;
		}
		// settlers: WorkshopNPCScript.workshopID
		std::unordered_map<std::uint32_t, std::int32_t> settlerID;
		for (const auto& i : p->instances) {
			if (!Extends(*p, i.script, "workshopnpcscript")) {
				continue;
			}
			const auto* v = Var(*p, i, "workshopid");
			if (!v || v->type != VType::kInt) {
				continue;
			}
			settlerID[a_save.formIDOf(i.refID)] = static_cast<std::int32_t>(static_cast<std::uint32_t>(v->value));
		}
		std::unordered_set<std::uint32_t> created;
		for (const auto& c : a_save.changeForms) {
			if (c.refID.type() == RefID::Type::kCreated) {
				created.insert(a_save.formIDOf(c.refID));
			}
		}

		std::unordered_map<std::uint32_t, std::uint32_t> linkOf;  // settlers' links (0 = cleared)
		std::vector<Finding>                             missing;
		const auto list = Scan(a_save, [&](ChangeFormType t, std::uint32_t form, const Link& l) {
			if (settlerID.contains(form)) {
				linkOf[form] = l.target;
			}
			if (l.raw.isZero() || !l.target) {
				return;  // a cleared link (UnassignActor sets None) or a reference the save cannot name
			}
			const bool createdGone = (l.target >> 24) == 0xFF && !created.contains(l.target);
			if (createdGone || a_save.pluginOf(l.target) == "?") {
				Finding f;
				f.kind = Finding::Kind::kMissingWorkshop;
				f.form = form;
				f.target = l.target;
				f.text = std::string(t == ChangeFormType::ACHR ? "Settler " : "Workshop object ") + Hex(form) + " is linked to workshop " +
				         Hex(l.target) + (createdGone ? ", a created reference that is not in the save" : ", from a plugin the save does not have");
				missing.push_back(std::move(f));
			}
		});
		for (const auto& s : list) {
			if (s.hasStorage && s.overBudget()) {
				Finding f;
				f.kind = Finding::Kind::kOverBudget;
				f.form = s.formID;
				char b[256];
				std::snprintf(b, sizeof(b), "Settlement workshop %08X is over its build budget: draws %lld / %lld, triangles %lld / %lld (meter %.0f%%)",
					s.formID, static_cast<long long>(s.usedDraws()), static_cast<long long>(s.limitDraws()),
					static_cast<long long>(s.usedTriangles()), static_cast<long long>(s.limitTriangles()), s.meter() * 100.0);
				f.text = b;
				out.push_back(std::move(f));
			}
		}
		for (auto& f : missing) {
			out.push_back(std::move(f));
		}
		if (haveArray) {
			std::vector<std::pair<std::uint32_t, std::int32_t>> sorted(settlerID.begin(), settlerID.end());
			std::sort(sorted.begin(), sorted.end());
			for (const auto& [form, id] : sorted) {
				if (id < 0) {
					continue;  // not assigned to a workshop
				}
				Finding f;
				f.form = form;
				f.id = id;
				if (static_cast<std::size_t>(id) >= workshops.size() || !workshops[static_cast<std::size_t>(id)]) {
					f.kind = Finding::Kind::kSettlerBadID;
					f.text = "Settler " + Hex(form) + " has workshopID " + std::to_string(id) + ", which names no workshop in WorkshopParentScript.Workshops (" +
					         std::to_string(workshops.size()) + " entries): the workshop scripts get None for it";
					out.push_back(std::move(f));
					continue;
				}
				const auto ws = workshops[static_cast<std::size_t>(id)];
				if (const auto it = linkOf.find(form); it != linkOf.end() && it->second && it->second != ws) {
					f.kind = Finding::Kind::kSettlerMismatch;
					f.target = it->second;
					f.text = "Settler " + Hex(form) + " has workshopID " + std::to_string(id) + " (workshop " + Hex(ws) + ") but is linked to workshop " + Hex(it->second);
					out.push_back(std::move(f));
				}
			}
		}
		return out;
	}
}
