// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Ops.h"
#include "ChangeFormData.h"
#include "ChangeFormFO4.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace ess::ops
{
	// ---- classification -------------------------------------------------------------------

	bool IsNativeScriptName(const std::string& a_name)
	{
		static const char* natives[] = { "ActiveMagicEffect", "Alias", "Debug", "Form", "Game", "Input", "Math", "ModEvent", "SKSE",
			"StringUtil", "UI", "Utility", "CommonArrayFunctions", "ScriptObject", "InputEnableLayer" };
		for (const char* n : natives) {
			if (EqualsNoCase(a_name, n)) {
				return true;
			}
		}
		return false;
	}

	bool IsEngineType(const std::string& a_name)
	{
		static const char* types[] = { "Action", "Activator", "ActiveMagicEffect", "Actor", "ActorBase", "ActorValueInfo", "Alias", "Ammo",
			"Apparatus", "Armor", "ArmorAddon", "Art", "AssociationType", "Book", "Cell", "Class", "ColorForm", "CombatStyle",
			"ConstructibleObject", "Container", "DefaultObjectManager", "Door", "EffectShader", "Enchantment", "EncounterZone", "EquipSlot",
			"Explosion", "Faction", "Flora", "Form", "FormList", "Furniture", "GlobalVariable", "Hazard", "HeadPart", "Idle", "ImageSpaceModifier",
			"ImpactDataSet", "Ingredient", "Key", "Keyword", "LeveledActor", "LeveledItem", "LeveledSpell", "Light", "Location", "LocationAlias",
			"LocationRefType", "MagicEffect", "Message", "MiscObject", "MusicType", "NetImmerse", "ObjectReference", "Outfit", "Package", "Perk",
			"Potion", "Projectile", "Quest", "Race", "ReferenceAlias", "Scene", "Scroll", "ShaderParticleGeometry", "Shout", "SoulGem", "Sound",
			"SoundCategory", "SoundDescriptor", "Spell", "Static", "TalkingActivator", "TextureSet", "Topic", "TopicInfo", "TreeObject",
			"VisualEffect", "VoiceType", "Weapon", "Weather", "WordOfPower", "WorldSpace", "TESModPlatform", "FootstepSet", "Lightning" };
		for (const char* t : types) {
			if (EqualsNoCase(a_name, t)) {
				return true;
			}
		}
		return false;
	}

	bool IsScriptUndefined(const Papyrus& p, const Script& s)
	{
		// The game writes an empty parent type for scripts whose .pex it could not load.
		if (!p.str(s.parent).empty()) {
			return false;
		}
		return !IsNativeScriptName(p.str(s.name));
	}

	bool IsInstanceUndefined(const Papyrus& p, TStr a_script)
	{
		if (const auto* s = p.findScript(a_script)) {
			return IsScriptUndefined(p, *s);
		}
		return !IsNativeScriptName(p.str(a_script));
	}

	bool IsUnattached(const ScriptInstance& s) { return s.refID.isZero(); }

	bool IsFrameNative(const StackFrame& f) { return (f.fnFlags & 0x02) != 0; }

	bool IsFrameZeroed(const StackFrame& f)
	{
		if (IsFrameNative(f) || f.code.empty()) {
			return false;
		}
		return std::all_of(f.code.begin(), f.code.end(), [](const Opcode& o) { return o.op == 0 && o.params.empty(); });
	}

	bool IsThreadSuspended(const Papyrus& p, ThreadID id)
	{
		for (const auto& s : p.suspended1) {
			if (s.id == id) {
				return true;
			}
		}
		for (const auto& s : p.suspended2) {
			if (s.id == id) {
				return true;
			}
		}
		return false;
	}

	static bool ThreadHasMessage(const Papyrus& p, ThreadID id)
	{
		for (const auto& m : p.functionMessages) {
			if (m.id && *m.id == id) {
				return true;
			}
		}
		return false;
	}

	bool IsThreadTerminated(const Papyrus& p, const ActiveScript& a)
	{
		if (!a.data || a.data->frames.empty() || IsThreadSuspended(p, a.id) || ThreadHasMessage(p, a.id)) {
			return false;
		}
		const auto& first = a.data->frames.front();
		if (IsFrameNative(first) || !IsFrameZeroed(first)) {
			return false;
		}
		return std::all_of(a.data->frames.begin(), a.data->frames.end(), [](const StackFrame& f) { return IsFrameZeroed(f) || IsFrameNative(f); });
	}

	bool IsThreadUndefined(const Papyrus& p, const ActiveScript& a)
	{
		if (!a.data) {
			return false;
		}
		for (const auto& f : a.data->frames) {
			if (IsFrameNative(f)) {
				continue;
			}
			if (const auto* s = p.findScript(f.scriptName); s && IsScriptUndefined(p, *s)) {
				return true;
			}
		}
		return false;
	}

	bool IsCreatedWithoutForm(SaveFile& s, const ScriptInstance& i)
	{
		return i.refID.type() == RefID::Type::kCreated && s.findChangeForm(i.refID) == nullptr;
	}

	// ---- edits -----------------------------------------------------------------------------

	namespace
	{
		template <class Vec, class Pred>
		std::size_t EraseIf(Vec& v, Pred pred)
		{
			const auto before = v.size();
			v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());
			return before - v.size();
		}

		template <class ID>
		void PruneOrder(std::vector<ID>& order, const auto& items)
		{
			std::unordered_set<ID> live;
			live.reserve(items.size());
			for (const auto& it : items) {
				live.insert(it.id);
			}
			EraseIf(order, [&](ID id) { return !live.contains(id); });
		}
	}

	std::size_t DeleteInstances(Papyrus& p, const std::set<EID>& a_ids)
	{
		const auto n = EraseIf(p.instances, [&](const ScriptInstance& s) { return a_ids.contains(s.id); });
		PruneOrder(p.instanceDataOrder, p.instances);
		p.reindex();
		return n;
	}

	std::size_t DeleteReferences(Papyrus& p, const std::set<EID>& a_ids)
	{
		const auto n = EraseIf(p.references, [&](const Reference& s) { return a_ids.contains(s.id); });
		PruneOrder(p.referenceDataOrder, p.references);
		p.reindex();
		return n;
	}

	std::size_t DeleteArrays(Papyrus& p, const std::set<EID>& a_ids)
	{
		const auto n = EraseIf(p.arrays, [&](const ArrayInfo& s) { return a_ids.contains(s.id); });
		PruneOrder(p.arrayDataOrder, p.arrays);
		p.reindex();
		return n;
	}

	std::size_t DeleteThreads(Papyrus& p, const std::set<ThreadID>& a_ids)
	{
		const auto n = EraseIf(p.activeScripts, [&](const ActiveScript& s) { return a_ids.contains(s.id); });
		PruneOrder(p.activeDataOrder, p.activeScripts);
		p.reindex();
		return n;
	}

	std::size_t TerminateThreads(Papyrus& p, const std::set<ThreadID>& a_ids)
	{
		// ReSaver's "terminate": every opcode of every frame becomes NOP, so the thread
		// finishes the moment the game resumes it.
		std::size_t n = 0;
		for (auto& a : p.activeScripts) {
			if (!a_ids.contains(a.id) || !a.data) {
				continue;
			}
			for (auto& f : a.data->frames) {
				for (auto& o : f.code) {
					o.op = 0;
					o.params.clear();
				}
			}
			++n;
		}
		return n;
	}

	std::size_t DeleteThreadsFully(Papyrus& p, const std::set<ThreadID>& a_ids)
	{
		std::set<std::size_t> msgs;
		for (std::size_t i = 0; i < p.functionMessages.size(); ++i) {
			if (p.functionMessages[i].id && a_ids.contains(*p.functionMessages[i].id)) {
				msgs.insert(i);
			}
		}
		DeleteFunctionMessages(p, msgs);
		DeleteSuspendedStacks(p, a_ids);
		return DeleteThreads(p, a_ids);
	}

	std::size_t DeleteSuspendedStacks(Papyrus& p, const std::set<ThreadID>& a_ids)
	{
		return EraseIf(p.suspended1, [&](const SuspendedStack& s) { return a_ids.contains(s.id); }) +
		       EraseIf(p.suspended2, [&](const SuspendedStack& s) { return a_ids.contains(s.id); });
	}

	std::vector<ThreadID> StrandedThreads(const Papyrus& p)
	{
		std::unordered_set<ThreadID> held;
		for (const auto* list : { &p.suspended1, &p.suspended2 }) {
			for (const auto& s : *list) {
				held.insert(s.id);
			}
		}
		for (const auto& m : p.functionMessages) {
			if (m.id) {
				held.insert(*m.id);
			}
		}
		if (p.unk1 != 0 && p.unk2) {
			held.insert(*p.unk2);
		}
		std::vector<ThreadID> out;
		for (const auto& a : p.activeScripts) {
			if (a.type == 0 && !held.contains(a.id)) {
				out.push_back(a.id);
			}
		}
		return out;
	}

	namespace
	{
		ThreadDelete RemoveWhole(Papyrus& p, std::set<ThreadID> a_threads, std::set<std::size_t> a_msgs)
		{
			ThreadDelete r;
			for (std::size_t i = 0; i < p.functionMessages.size(); ++i) {
				if (const auto& m = p.functionMessages[i]; m.id && a_threads.contains(*m.id)) {
					a_msgs.insert(i);
				}
			}
			r.messages = DeleteFunctionMessages(p, a_msgs);
			r.stacks = DeleteSuspendedStacks(p, a_threads);
			r.threads = DeleteThreads(p, a_threads);
			// the engine's extra slot: a count (0 or 1) and, when 1, the id of the thread in it (SkyrimSE.exe 1.6.1170: VM
			// save 0x144a840 writes VM+0x9398 then, if not 0, the id of the stack at VM+0x9390; load 0x144b150 reads them
			// back). A thread that goes empties the slot.
			if (p.unk1 != 0 && p.unk2 && a_threads.contains(*p.unk2)) {
				p.unk1 = 0;
				p.unk2.reset();
			}
			return r;
		}
	}

	ThreadDelete RemoveSuspendedStacks(Papyrus& p, const std::set<ThreadID>& a_ids) { return RemoveWhole(p, a_ids, {}); }

	ThreadDelete RemoveFunctionMessages(Papyrus& p, const std::set<std::size_t>& a_indices)
	{
		std::set<ThreadID>    threads;
		std::set<std::size_t> msgs;
		for (const auto i : a_indices) {
			if (i < p.functionMessages.size()) {
				msgs.insert(i);
				if (const auto& m = p.functionMessages[i]; m.id) {
					threads.insert(*m.id);
				}
			}
		}
		return RemoveWhole(p, std::move(threads), std::move(msgs));
	}

	ThreadDelete RemoveThreads(Papyrus& p, const std::set<ThreadID>& a_ids) { return RemoveWhole(p, a_ids, {}); }

	ThreadDelete RemoveStrandedThreads(Papyrus& p)
	{
		const auto v = StrandedThreads(p);
		return RemoveWhole(p, std::set<ThreadID>(v.begin(), v.end()), {});
	}

	std::size_t DeleteFunctionMessages(Papyrus& p, const std::set<std::size_t>& a_indices)
	{
		std::size_t                  n = 0;
		std::vector<FunctionMessage> keep;
		keep.reserve(p.functionMessages.size());
		for (std::size_t i = 0; i < p.functionMessages.size(); ++i) {
			if (a_indices.contains(i)) {
				++n;
			} else {
				keep.push_back(std::move(p.functionMessages[i]));
			}
		}
		p.functionMessages = std::move(keep);
		return n;
	}

	std::size_t DeleteUnbinds(Papyrus& p, const std::set<EID>& a_ids)
	{
		return EraseIf(p.unbinds, [&](const QueuedUnbind& u) { return a_ids.contains(u.id); });
	}

	std::size_t DeleteScripts(Papyrus& p, const std::set<TStr>& a_names)
	{
		std::size_t n = EraseIf(p.scripts, [&](const Script& s) { return a_names.contains(s.name); });
		n += EraseIf(p.instances, [&](const ScriptInstance& s) { return a_names.contains(s.script); });
		PruneOrder(p.instanceDataOrder, p.instances);
		p.reindex();
		return n;
	}

	std::size_t DeleteChangeForms(SaveFile& s, const std::set<std::uint32_t>& a_refIDs)
	{
		const auto n = EraseIf(s.changeForms, [&](const ChangeForm& c) { return a_refIDs.contains(c.refID.data); });
		s.reindexChangeForms();
		return n;
	}

	std::size_t RemoveUnattachedInstances(Papyrus& p)
	{
		std::set<EID> ids;
		for (const auto& i : p.instances) {
			if (IsUnattached(i)) {
				ids.insert(i.id);
			}
		}
		return DeleteInstances(p, ids);
	}

	std::size_t TerminateUndefinedThreads(Papyrus& p)
	{
		std::set<ThreadID> ids;
		for (const auto& a : p.activeScripts) {
			if (IsThreadUndefined(p, a) && !IsThreadTerminated(p, a)) {
				ids.insert(a.id);
			}
		}
		return TerminateThreads(p, ids);
	}

	std::size_t RemoveUndefinedElements(Papyrus& p)
	{
		// Threads first: their "undefined" test needs the script definitions still present.
		std::size_t n = TerminateUndefinedThreads(p);

		std::set<TStr> scripts;
		for (const auto& s : p.scripts) {
			if (IsScriptUndefined(p, s)) {
				scripts.insert(s.name);
			}
		}
		std::set<EID> insts, refs;
		for (const auto& i : p.instances) {
			if (IsInstanceUndefined(p, i.script)) {
				insts.insert(i.id);
			}
		}
		for (const auto& r : p.references) {
			if (IsInstanceUndefined(p, r.script)) {
				refs.insert(r.id);
			}
		}
		n += DeleteInstances(p, insts);
		n += DeleteReferences(p, refs);
		n += DeleteScripts(p, scripts);
		return n;
	}

	// ---- ActiveMagicEffect instances whose effect is still running -------------------------------

	namespace
	{
		// the unique ids of the effects in an actor's saved active effect list; false when the list could not be read
		// (an effect whose unique id cannot be read is reported as 0xFFFFFFFF in a_unread)
		bool LiveUniqueIDs(const SaveFile& s, const ChangeForm& a_cf, std::set<std::uint16_t>& a_ids, bool& a_someUnread)
		{
			a_someUnread = false;
			if (s.isFO4()) {
				const auto a = fo4cf::DecodeActor(s, a_cf);
				if (a.effectsRange.begin == 0 && !a.complete) {
					return false;  // stopped before its effect list (or it has none: cannot tell)
				}
				for (const auto& e : a.effects) {
					if (!e.uniqueKnown) {
						a_someUnread = true;
					} else {
						a_ids.insert(e.uniqueID);
					}
				}
				return true;
			}
			const auto d = cf::Decode(s, a_cf);
			if (!d.complete && d.activeEffectsCountAt == 0) {
				return false;
			}
			const auto body = cf::Body(a_cf);
			for (const auto& e : d.activeEffects) {
				// ActiveEffect::LoadGame (0x5ad0d0): f32 x3 + u32 flags (16), 4 refs (12), the hit list's vsval count (one
				// 0 byte when empty), u32 (+0x80), u16 unique id (+0x84)
				const auto at = e.dataAt;
				if (e.dataSize < 35 || at + 35 > body.size() || body[at + 28] != 0) {
					a_someUnread = true;
					continue;
				}
				a_ids.insert(static_cast<std::uint16_t>(body[at + 33] | (body[at + 34] << 8)));
			}
			return true;
		}
	}

	std::vector<KeptInstance> LiveEffectInstances(const SaveFile& s, const std::set<EID>& a_candidates, const std::set<std::uint32_t>* a_changeFormsGoing)
	{
		std::vector<KeptInstance> out;
		const auto*               p = s.papyrus();
		if (!p || a_candidates.empty()) {
			return out;
		}
		// the candidates that are ActiveMagicEffect handles, by actor
		std::map<std::uint32_t, std::vector<const ScriptInstance*>> byActor;  // refID.data -> instances
		for (const auto& i : p->instances) {
			if (i.unknown2bits == 2 && i.unknown != 0 && !i.refID.isZero() && a_candidates.contains(i.id)) {
				byActor[i.refID.data].push_back(&i);
			}
		}
		for (const auto& [ref, insts] : byActor) {
			if (a_changeFormsGoing && a_changeFormsGoing->contains(ref)) {
				continue;  // the actor goes too, with its effects
			}
			RefID r;
			r.data = ref;
			const ChangeForm* cf = nullptr;
			for (const auto& c : s.changeForms) {  // findChangeForm is not const; actors are few here
				if (c.refID == r && c.typeCode() == static_cast<std::uint8_t>(ChangeFormType::ACHR)) {
					cf = &c;
					break;
				}
			}
			if (!cf) {
				continue;  // no saved actor data: no saved effects
			}
			std::set<std::uint16_t> live;
			bool                    someUnread = false;
			const bool              read = LiveUniqueIDs(s, *cf, live, someUnread);
			for (const auto* i : insts) {
				const bool hit = live.contains(i->unknown);
				if (hit || !read || someUnread) {
					KeptInstance k;
					k.id = i->id;
					k.script = p->str(i->script);
					k.actor = s.formIDOf(i->refID);
					k.uniqueID = i->unknown;
					k.unread = !hit;
					out.push_back(std::move(k));
				}
			}
		}
		return out;
	}

	std::vector<KeptInstance> KeepLiveEffectInstances(const SaveFile& s, std::set<EID>& a_ids, const std::set<std::uint32_t>* a_changeFormsGoing)
	{
		auto kept = LiveEffectInstances(s, a_ids, a_changeFormsGoing);
		for (const auto& k : kept) {
			a_ids.erase(k.id);
		}
		return kept;
	}

	std::string KeptText(const std::vector<KeptInstance>& a_kept)
	{
		if (a_kept.empty()) {
			return {};
		}
		std::string s = "kept " + std::to_string(a_kept.size()) +
		                " ActiveMagicEffect script instance(s) whose effect is still running on an actor (removing only the script "
		                "would leave the effect without it; the game wrote both):";
		char b[160];
		for (std::size_t i = 0; i < a_kept.size() && i < 8; ++i) {
			const auto& k = a_kept[i];
			std::snprintf(b, sizeof(b), " %s on %08X (effect %u%s)%s", k.script.c_str(), k.actor, k.uniqueID,
				k.unread ? ", its effects could not be read" : "", i + 1 < a_kept.size() ? ";" : "");
			s += b;
		}
		if (a_kept.size() > 8) {
			s += " ...";
		}
		return s;
	}

	CleanResult RemoveUnattachedInstances(SaveFile& s)
	{
		CleanResult r;
		auto*       p = s.papyrus();
		if (!p) {
			return r;
		}
		std::set<EID> ids;
		for (const auto& i : p->instances) {
			if (IsUnattached(i)) {
				ids.insert(i.id);
			}
		}
		r.kept = KeepLiveEffectInstances(s, ids);  // an unattached instance names no actor: none is kept in practice
		r.removed = DeleteInstances(*p, ids);
		return r;
	}

	CleanResult DeleteScripts(SaveFile& s, const std::set<TStr>& a_names)
	{
		CleanResult r;
		auto*       p = s.papyrus();
		if (!p) {
			return r;
		}
		std::set<EID> insts;
		for (const auto& i : p->instances) {
			if (a_names.contains(i.script)) {
				insts.insert(i.id);
			}
		}
		r.kept = KeepLiveEffectInstances(s, insts);
		std::set<TStr> names = a_names;
		for (const auto& k : r.kept) {
			if (const auto* i = p->findInstance(k.id)) {
				names.erase(i->script);  // its definition stays with it
			}
		}
		r.removed = DeleteInstances(*p, insts);  // the kept ones are not in insts
		r.removed += DeleteScripts(*p, names);   // (their own instances are gone already)
		return r;
	}

	CleanResult RemoveUndefinedElements(SaveFile& s)
	{
		CleanResult r;
		auto*       p = s.papyrus();
		if (!p) {
			return r;
		}
		// as RemoveUndefinedElements(Papyrus&), with the kept instances and their scripts left out
		std::size_t    n = TerminateUndefinedThreads(*p);
		std::set<TStr> scripts;
		for (const auto& sc : p->scripts) {
			if (IsScriptUndefined(*p, sc)) {
				scripts.insert(sc.name);
			}
		}
		std::set<EID> insts, refs;
		for (const auto& i : p->instances) {
			if (IsInstanceUndefined(*p, i.script)) {
				insts.insert(i.id);
			}
		}
		for (const auto& rf : p->references) {
			if (IsInstanceUndefined(*p, rf.script)) {
				refs.insert(rf.id);
			}
		}
		r.kept = KeepLiveEffectInstances(s, insts);
		for (const auto& k : r.kept) {
			if (const auto* i = p->findInstance(k.id)) {
				scripts.erase(i->script);
			}
		}
		n += DeleteInstances(*p, insts);
		n += DeleteReferences(*p, refs);
		n += DeleteScripts(*p, scripts);
		r.removed = n;
		return r;
	}

	std::size_t RemoveNonexistentCreated(SaveFile& s)
	{
		auto* p = s.papyrus();
		if (!p) {
			return 0;
		}
		std::set<EID> ids;
		for (const auto& i : p->instances) {
			if (IsCreatedWithoutForm(s, i)) {
				ids.insert(i.id);
			}
		}
		return DeleteInstances(*p, ids);
	}

	namespace
	{
		bool VarHitsAny(const Variable& v, const std::set<EID>& ids)
		{
			const Variable* x = &v;
			if (x->type == VType::kVariant && x->inner) {
				x = x->inner.get();
			}
			return x->type == VType::kRef && ids.contains(x->value);
		}
		bool VarsHitAny(const std::vector<Variable>& vars, const std::set<EID>& ids)
		{
			for (const auto& v : vars) {
				if (VarHitsAny(v, ids)) {
					return true;
				}
			}
			return false;
		}
	}

	static bool VarRefersTo(const Variable& v, EID target);

	namespace
	{
		// PurgeScript's work for a set of instances (and, with a_matches, every frame / call of a script): their threads
		// (any frame running the script or running on one of the instances), those threads' suspended stacks and queued
		// calls, calls made on one of the instances or (when the instances go) passing one of them, then the instances
		struct PurgeWork
		{
			std::set<ThreadID>    threads;
			std::set<std::size_t> msgs;
		};
		template <class Matches>
		PurgeWork PurgeSelect(const Papyrus& p_, const std::set<EID>& insts, Matches matches, bool a_keepInstances)
		{
			const auto* p = &p_;
			PurgeWork   w;
			auto&       threads = w.threads;
			auto&       msgs = w.msgs;
			// threads running the script, or running any function on one of its instances
			for (const auto& a : p->activeScripts) {
				if (!a.data) {
					continue;
				}
				for (const auto& f : a.data->frames) {
					if (matches(f.scriptName) || VarHitsAny(f.owner, insts)) {
						threads.insert(a.id);
						break;
					}
				}
			}
			// suspended calls: belonging to those threads, of the script itself, called on one of its
			// instances, or (when the instances go) passing one of them as an argument. Queued calls the
			// same way. A thread whose pending or queued call goes is removed with it (it would wait
			// forever), and its own pending and queued calls with it: repeated until nothing changes.
			for (bool changed = true; changed;) {
				changed = false;
				for (const auto* list : { &p->suspended1, &p->suspended2 }) {
					for (const auto& st : *list) {
						if (threads.contains(st.id)) {
							continue;
						}
						if (st.data && (matches(st.data->scriptName) || VarHitsAny(st.data->unknownVar, insts) ||
										   (!a_keepInstances && VarsHitAny(st.data->vars, insts)))) {
							threads.insert(st.id);
							changed = true;
						}
					}
				}
				for (std::size_t i = 0; i < p->functionMessages.size(); ++i) {
					if (msgs.contains(i)) {
						continue;
					}
					const auto& m = p->functionMessages[i];
					if ((m.id && threads.contains(*m.id)) ||
						(m.data && (matches(m.data->scriptName) || VarHitsAny(m.data->unknownVar, insts) ||
									   (!a_keepInstances && VarsHitAny(m.data->vars, insts))))) {
						msgs.insert(i);
						if (m.id && !threads.contains(*m.id)) {
							threads.insert(*m.id);
							changed = true;
						}
					}
				}
			}
			return w;
		}
	}

	PurgeResult PurgeInstances(SaveFile& s, const std::set<EID>& a_ids)
	{
		PurgeResult r;
		auto*       p = s.papyrus();
		if (!p || a_ids.empty()) {
			return r;
		}
		std::set<EID> ids = a_ids;
		r.kept = KeepLiveEffectInstances(s, ids);
		if (ids.empty()) {
			return r;
		}
		const auto w = PurgeSelect(*p, ids, [](TStr) { return false; }, false);
		r.messages = DeleteFunctionMessages(*p, w.msgs);
		r.stacks = DeleteSuspendedStacks(*p, w.threads);
		r.threads = DeleteThreads(*p, w.threads);
		r.instances = DeleteInstances(*p, ids);
		DeleteUnbinds(*p, ids);
		return r;
	}

	std::vector<std::string> ReferrersOutside(const Papyrus& p, const std::set<EID>& a_ids)
	{
		std::vector<std::string> out;
		if (a_ids.empty()) {
			return out;
		}
		const auto w = PurgeSelect(p, a_ids, [](TStr) { return false; }, false);
		auto       scan = [&](const std::vector<Variable>& vars, const std::string& what) {
            for (std::size_t i = 0; i < vars.size(); ++i) {
                for (const auto id : a_ids) {
                    if (VarRefersTo(vars[i], id)) {
                        char b[40];
                        std::snprintf(b, sizeof(b), " variable %zu", i);
                        out.push_back(what + b);
                    }
                }
            }
		};
		char b[64];
		for (const auto& i : p.instances) {
			if (!a_ids.contains(i.id) && i.data) {
				std::snprintf(b, sizeof(b), " (instance %016llX)", static_cast<unsigned long long>(i.id));
				scan(i.data->vars, p.str(i.script) + b);
			}
		}
		for (const auto& r : p.references) {
			if (r.data) {
				std::snprintf(b, sizeof(b), " (reference %016llX)", static_cast<unsigned long long>(r.id));
				scan(r.data->vars, p.str(r.script) + b);
			}
		}
		for (const auto& a : p.arrays) {
			std::snprintf(b, sizeof(b), "array %016llX", static_cast<unsigned long long>(a.id));
			scan(a.vars, b);
		}
		for (const auto& st : p.structInstances) {
			std::snprintf(b, sizeof(b), " (struct %016llX)", static_cast<unsigned long long>(st.id));
			scan(st.vars, p.str(st.def) + b);
		}
		for (const auto& t : p.activeScripts) {
			if (!t.data || w.threads.contains(t.id)) {
				continue;
			}
			for (const auto& f : t.data->frames) {
				std::snprintf(b, sizeof(b), " (thread %u)", static_cast<unsigned>(t.id));
				scan(f.vars, p.str(f.baseName) + "." + p.str(f.event) + b);
			}
		}
		return out;
	}

	PurgeResult PurgeScript(SaveFile& s, const std::string& a_scriptName, bool a_deleteCreatedForms, bool a_keepInstances)
	{
		PurgeResult r;
		auto*       p = s.papyrus();
		if (!p) {
			return r;
		}
		auto matches = [&](TStr t) { return EqualsNoCase(p->str(t), a_scriptName); };

		std::set<EID>           insts;
		std::set<std::uint32_t> forms;
		for (const auto& i : p->instances) {
			if (matches(i.script)) {
				insts.insert(i.id);
				if (a_deleteCreatedForms && i.refID.type() == RefID::Type::kCreated) {
					forms.insert(i.refID.data);
				}
			}
		}
		if (!a_keepInstances) {
			r.kept = KeepLiveEffectInstances(s, insts);  // they stay, so calls passing them stay valid too
			for (const auto& k : r.kept) {
				if (const auto* i = p->findInstance(k.id); i && i->refID.type() == RefID::Type::kCreated) {
					forms.erase(i->refID.data);  // the actor the effect runs on stays
				}
			}
		}
		const auto w = PurgeSelect(*p, insts, matches, a_keepInstances);
		const auto& threads = w.threads;
		const auto& msgs = w.msgs;

		r.messages = DeleteFunctionMessages(*p, msgs);
		r.stacks = DeleteSuspendedStacks(*p, threads);
		r.threads = DeleteThreads(*p, threads);
		if (!a_keepInstances) {
			r.instances = DeleteInstances(*p, insts);
			if (a_deleteCreatedForms) {
				r.forms = DeleteChangeForms(s, forms);
			}
		}
		return r;
	}

	// ---- ReSaver filters ---------------------------------------------------------------------

	std::vector<std::string> ExtendedMembers(const Papyrus& p, TStr a_script)
	{
		std::vector<const Script*> chain;
		for (const Script* s = p.findScript(a_script); s && chain.size() < 32; s = p.findScript(s->parent)) {
			chain.push_back(s);
			if (p.str(s->parent).empty()) {
				break;
			}
		}
		std::vector<std::string> names;
		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			for (const auto& m : (*it)->members) {
				names.push_back(p.str(m.name));
			}
		}
		return names;
	}

	bool IsMemberless(const Papyrus& p, const ScriptInstance& i)
	{
		const bool noVars = !i.data || i.data->vars.empty();
		return noVars && !ExtendedMembers(p, i.script).empty();
	}

	bool HasCanary(const Papyrus& p, const ScriptInstance& i)
	{
		for (const auto& n : ExtendedMembers(p, i.script)) {
			if (n == "::iPapyrusDataVerification_var") {
				return true;
			}
		}
		return false;
	}

	int CanaryValue(const Papyrus& p, const ScriptInstance& i)
	{
		const auto names = ExtendedMembers(p, i.script);
		for (std::size_t k = 0; k < names.size(); ++k) {
			if (names[k] == "::iPapyrusDataVerification_var") {
				if (i.data && k < i.data->vars.size() && i.data->vars[k].type == VType::kInt) {
					return static_cast<std::int32_t>(i.data->vars[k].value);
				}
				return 0;
			}
		}
		return 0;
	}

	bool IsNoParent(const Papyrus& p, const Script& s) { return p.str(s.parent).empty() && !IsNativeScriptName(p.str(s.name)); }

	bool IsMissingParent(const Papyrus& p, const Script& s)
	{
		if (IsNativeScriptName(p.str(s.name))) {
			return false;
		}
		return p.str(s.parent).empty() || !p.findScript(s.parent);
	}

	// ---- plugin actions ----------------------------------------------------------------------

	PluginPurge SelectPluginElements(const SaveFile& s, const std::set<std::string>& a_plugins, bool a_changeForms, bool a_instances)
	{
		PluginPurge sel;
		std::set<std::string> lower;
		for (const auto& n : a_plugins) {
			std::string l = n;
			for (auto& c : l) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			lower.insert(l);
		}
		auto mine = [&](const RefID& id) {
			if (id.isZero() || id.type() == RefID::Type::kCreated) {
				return false;
			}
			auto n = s.pluginOf(s.formIDOf(id));
			for (auto& c : n) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return lower.contains(n);
		};
		if (a_changeForms) {
			for (const auto& c : s.changeForms) {
				if (mine(c.refID)) {
					sel.changeForms.insert(c.refID.data);
				}
			}
		}
		if (const auto* p = s.papyrus(); p && a_instances) {
			for (const auto& i : p->instances) {
				if (mine(i.refID)) {
					sel.instances.insert(i.id);
				}
			}
			for (const auto& a : p->activeScripts) {
				if (!a.data) {
					continue;
				}
				for (const auto& f : a.data->frames) {
					if ((f.owner.type == VType::kRef) && sel.instances.contains(f.owner.value)) {
						sel.threads.insert(a.id);
						break;
					}
				}
			}
		}
		return sel;
	}

	std::vector<KeptInstance> ApplyPluginPurge(SaveFile& s, const PluginPurge& a_sel)
	{
		std::vector<KeptInstance> kept;
		if (auto* p = s.papyrus()) {
			std::set<EID> insts = a_sel.instances;
			kept = KeepLiveEffectInstances(s, insts, &a_sel.changeForms);
			DeleteThreadsFully(*p, a_sel.threads);
			DeleteInstances(*p, insts);
		}
		DeleteChangeForms(s, a_sel.changeForms);
		return kept;
	}

	// ---- SkyUI MCM registration jam ------------------------------------------------------------

	namespace
	{
		bool IsMcmEvent(const std::string& e) { return EqualsNoCase(e, "OnConfigManagerReady") || EqualsNoCase(e, "OnConfigManagerReset"); }
		bool IsManager(const std::string& s) { return EqualsNoCase(s, "SKI_ConfigManager"); }

		struct McmSel
		{
			std::set<ThreadID>    threads;
			std::set<ThreadID>    stacks;
			std::set<std::size_t> messages;
		};

		McmSel SelectMcm(const Papyrus& p)
		{
			McmSel s;
			for (const auto& a : p.activeScripts) {
				if (!a.data) {
					continue;
				}
				for (const auto& f : a.data->frames) {
					if (IsMcmEvent(p.str(f.event)) || IsManager(p.str(f.scriptName)) || IsManager(p.str(f.baseName))) {
						s.threads.insert(a.id);
						break;
					}
				}
			}
			auto dataHit = [&](const FunctionMessageData& d) { return IsMcmEvent(p.str(d.event)) || IsManager(p.str(d.scriptName)); };
			// a thread whose pending call goes is removed too, or it would wait forever
			for (bool changed = true; changed;) {
				changed = false;
				for (const auto* list : { &p.suspended1, &p.suspended2 }) {
					for (const auto& st : *list) {
						if (!s.stacks.contains(st.id) && (s.threads.contains(st.id) || (st.data && dataHit(*st.data)))) {
							s.stacks.insert(st.id);
							if (p.findActive(st.id) && s.threads.insert(st.id).second) {
								changed = true;
							}
						}
					}
				}
				for (std::size_t i = 0; i < p.functionMessages.size(); ++i) {
					const auto& m = p.functionMessages[i];
					if (s.messages.contains(i)) {
						continue;
					}
					if ((m.id && (s.threads.contains(*m.id) || s.stacks.contains(*m.id))) || (m.data && dataHit(*m.data))) {
						s.messages.insert(i);
						if (m.id && p.findActive(*m.id) && s.threads.insert(*m.id).second) {
							changed = true;
						}
					}
				}
			}
			return s;
		}
	}

	std::size_t CountMcmJam(const Papyrus& p) { return SelectMcm(p).threads.size(); }

	McmFixResult FixMcmJam(Papyrus& p, bool a_dryRun)
	{
		McmFixResult r;
		const auto   sel = SelectMcm(p);
		for (auto& i : p.instances) {
			if (!IsManager(p.str(i.script)) || !i.data) {
				continue;
			}
			r.managerFound = true;
			// _modConfigs is the only Object[] array member; count its filled entries
			for (const auto& v : i.data->vars) {
				if (v.type == VType::kRefArray) {
					if (const auto* a = p.findArray(v.value)) {
						for (const auto& e : a->vars) {
							r.registered += e.value != 0;
						}
					}
				}
			}
			if (EqualsNoCase(p.str(i.data->state), "BUSY")) {
				r.stateReset = true;
				if (!a_dryRun) {
					i.data->state = p.internString("");
				}
			}
		}
		if (a_dryRun) {
			r.threads = sel.threads.size();
			r.stacks = sel.stacks.size();
			r.messages = sel.messages.size();
			return r;
		}
		r.messages = DeleteFunctionMessages(p, sel.messages);
		std::set<ThreadID> st = sel.stacks;
		st.insert(sel.threads.begin(), sel.threads.end());
		r.stacks = DeleteSuspendedStacks(p, st);
		r.threads = DeleteThreads(p, sel.threads);
		return r;
	}

	std::string McmFixResult::text() const
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf), "%zu registration threads, %zu suspended stacks, %zu queued calls%s%s", threads, stacks, messages,
			stateReset ? "; SKI_ConfigManager taken out of its stuck BUSY state" : "",
			managerFound ? "" : " (SKI_ConfigManager not found in this save)");
		return buf;
	}

	// ---- queries -----------------------------------------------------------------------------

	static bool VarRefersTo(const Variable& v, EID target)
	{
		if (v.type == VType::kVariant) {
			return v.inner && VarRefersTo(*v.inner, target);
		}
		return (v.type == VType::kRef || v.type == VType::kStruct || IsArrayType(v.type)) && v.value == target;
	}

	std::vector<Referrer> FindReferrers(const Papyrus& p, EID a_target)
	{
		std::vector<Referrer> out;
		auto                  scanVars = [&](const std::vector<Variable>& vars, Referrer::Kind k, std::uint64_t id, const std::string& owner) {
            for (std::size_t i = 0; i < vars.size(); ++i) {
                if (VarRefersTo(vars[i], a_target)) {
                    out.push_back({ k, id, owner + " var[" + std::to_string(i) + "]" });
                }
            }
		};
		for (const auto& i : p.instances) {
			if (i.id != a_target && i.data) {
				scanVars(i.data->vars, Referrer::Kind::kInstance, i.id, p.str(i.script));
			}
		}
		for (const auto& r : p.references) {
			if (r.id != a_target && r.data) {
				scanVars(r.data->vars, Referrer::Kind::kReference, r.id, p.str(r.script));
			}
		}
		for (const auto& a : p.arrays) {
			if (a.id != a_target) {
				scanVars(a.vars, Referrer::Kind::kArray, a.id, std::string("array ") + VTypeName(a.type));
			}
		}
		for (const auto& t : p.activeScripts) {
			if (!t.data) {
				continue;
			}
			for (const auto& f : t.data->frames) {
				const std::string where = p.str(f.baseName) + "." + p.str(f.event);
				if (VarRefersTo(f.owner, a_target)) {
					out.push_back({ Referrer::Kind::kThread, t.id, where + " (owner)" });
				}
				scanVars(f.vars, Referrer::Kind::kThread, t.id, where);
			}
		}
		return out;
	}

	// ---- analysis ---------------------------------------------------------------------------

	namespace
	{
		std::vector<Count> Sorted(const std::unordered_map<std::string, std::size_t>& m)
		{
			std::vector<Count> v;
			v.reserve(m.size());
			for (const auto& [k, n] : m) {
				v.push_back({ k, n });
			}
			std::sort(v.begin(), v.end(), [](const Count& a, const Count& b) { return a.n != b.n ? a.n > b.n : a.name < b.name; });
			return v;
		}
		std::string FrameName(const Papyrus& p, const StackFrame& f) { return p.str(f.baseName) + "." + p.str(f.event); }
		std::string MsgName(const Papyrus& p, const FunctionMessageData& d) { return p.str(d.scriptName) + "." + p.str(d.event); }
	}

	std::string DamagedFormsText(const SaveFile& s, const std::vector<cf::DamagedForm>& a_damaged, std::size_t a_max)
	{
		std::string list;
		for (std::size_t i = 0; i < a_damaged.size() && i < a_max; ++i) {
			const auto& d = a_damaged[i];
			char        id[16];
			std::snprintf(id, sizeof(id), "%08X", d.formID);
			list += std::string(i ? "; " : "") + id + " (" + ChangeFormTypeName(s.changeForms[d.index].typeCode(), s.isFO4()) + ": " + d.why + ")";
		}
		if (a_damaged.size() > a_max) {
			list += "; and " + std::to_string(a_damaged.size() - a_max) + " more";
		}
		return std::to_string(a_damaged.size()) + " change form(s) could not be decompressed: " + list +
		       ". Their data is shown as unreadable and every check skips them; saving keeps their bytes exactly as they are.";
	}

	Report Analyze(SaveFile& s, const std::vector<cf::DamagedForm>* a_knownDamaged)
	{
		Report r;
		r.changeForms = s.changeForms.size();
		std::unordered_map<std::string, std::size_t> cfTypes;
		for (const auto& c : s.changeForms) {
			cfTypes[ChangeFormTypeName(c.typeCode(), s.isFO4())]++;
		}
		r.changeFormsByType = Sorted(cfTypes);

		// change forms whose compressed data cannot be read: shown as unreadable, skipped by every check, written back as
		// they are (Skyrim LE 1.9.32 itself loads them without a word: it never checks the result of its inflate)
		r.damaged = a_knownDamaged ? cf::DamagedForms(s, a_knownDamaged) : cf::DamagedForms(s);
		if (!r.damaged.empty()) {
			r.warnings.push_back(DamagedFormsText(s, r.damaged));
		}

		// spawned actors whose created base is not in the save (change forms only: checked even without script data)
		r.ghosts = ghost::Find(s);
		if (!r.ghosts.empty()) {
			std::string list;
			for (std::size_t i = 0; i < r.ghosts.size() && i < 8; ++i) {
				list += (i ? "; " : "") + ghost::Describe(s, r.ghosts[i]);
			}
			if (r.ghosts.size() > 8) {
				list += "; and " + std::to_string(r.ghosts.size() - 8) + " more";
			}
			r.warnings.push_back(std::to_string(r.ghosts.size()) +
				" spawned actor(s) name a base NPC the save does not have: " + list +
				". Each was placed from another actor's temporary leveled base (a script's PlaceAtMe of "
				"Actor.GetLeveledActorBase()), which the game never saves and deletes with its owner. On load that id is free "
				"(the actor has no base) or another created NPC's (it loads as that NPC and can delete that NPC's base): crashes "
				"in Actor::LoadGame, SPID, or a script error naming the actor, on some loads and not others. Fix the script that "
				"spawns them (GetActorBase instead of GetLeveledActorBase); ReSaver Plus does not remove them (the player's record "
				"and scripts still name them).");
		}

		const auto* pp = s.papyrus();
		if (!pp) {
			r.warnings.push_back("This save has no Papyrus section.");
			return r;
		}
		const auto& p = *pp;
		if (p.invalid) {
			r.warnings.push_back("The game flagged this save's Papyrus data invalid and wrote none of it: its script engine could not pause when "
								 "it saved (it gives up after 30 seconds). Loading this save, the game ignores all script data. The usual "
								 "cause is a thread nothing resumes (see the stranded threads check) in the save loaded before it.");
			return r;
		}
		r.scripts = p.scripts.size();
		r.instances = p.instances.size();
		r.references = p.references.size();
		r.arrays = p.arrays.size();
		r.threads = p.activeScripts.size();
		r.functionMessages = p.functionMessages.size();
		r.suspended = p.suspended1.size() + p.suspended2.size();
		r.strings = p.strings.size();

		for (const auto& sc : p.scripts) {
			if (IsScriptUndefined(p, sc)) {
				++r.undefinedScripts;
			}
		}

		std::unordered_map<std::string, std::size_t> inst, top, cur, susp, msg;
		for (const auto& i : p.instances) {
			inst[p.str(i.script)]++;
			if (IsUnattached(i)) {
				++r.unattached;
			}
			if (IsInstanceUndefined(p, i.script)) {
				++r.undefinedInstances;
			}
			if (IsCreatedWithoutForm(s, i)) {
				++r.createdWithoutForm;
			}
		}

		std::size_t waitLoops = 0;
		std::unordered_map<std::string, std::size_t> waitByScript;
		for (const auto& a : p.activeScripts) {
			if (!a.data || a.data->frames.empty()) {
				top["<no frames>"]++;
				continue;
			}
			top[p.str(a.data->frames.front().scriptName)]++;
			const auto& inner = a.data->frames.back();
			cur[FrameName(p, inner)]++;
			if (IsThreadUndefined(p, a)) {
				++r.undefinedThreads;
			}
			if (IsThreadTerminated(p, a)) {
				++r.terminatedThreads;
			}
			const auto innerName = FrameName(p, inner);
			if (EqualsNoCase(innerName, "utility.Wait") || EqualsNoCase(innerName, "utility.WaitGameTime") || EqualsNoCase(innerName, "utility.WaitMenuMode")) {
				++waitLoops;
				waitByScript[p.str(a.data->frames.front().scriptName)]++;
			}
		}
		for (const auto* list : { &p.suspended1, &p.suspended2 }) {
			for (const auto& st : *list) {
				susp[st.data ? MsgName(p, *st.data) : "<no data>"]++;
			}
		}
		for (const auto& m : p.functionMessages) {
			msg[m.data ? MsgName(p, *m.data) : "<no data>"]++;
		}

		r.instancesByScript = Sorted(inst);
		r.threadsByTopScript = Sorted(top);
		r.threadsByCurrentFrame = Sorted(cur);
		r.suspendedByFunction = Sorted(susp);
		r.messagesByFunction = Sorted(msg);

		// ---- plain-language findings ----
		char buf[512];
		for (const auto& c : Sorted(waitByScript)) {
			if (c.n >= 20) {
				r.loopingScripts.push_back(c.name);
				std::snprintf(buf, sizeof(buf),
					"%zu threads of '%s' are sleeping in Utility.Wait loops. That many copies of one looping script usually means "
					"a script that never exits (each wakes several times a second). Consider purging '%s'.",
					c.n, c.name.c_str(), c.name.c_str());
				r.warnings.push_back(buf);
			}
		}
		std::size_t mcmThreads = 0;
		for (const auto& a : p.activeScripts) {
			if (a.data && !a.data->frames.empty() && EqualsNoCase(p.str(a.data->frames.front().event), "OnConfigManagerReady")) {
				++mcmThreads;
			}
		}
		std::size_t registerMod = 0;
		for (const auto& c : r.suspendedByFunction) {
			if (EqualsNoCase(c.name, "SKI_ConfigManager.RegisterMod")) {
				registerMod = c.n;
			}
		}
		bool busy = false;
		for (const auto& i : p.instances) {
			if (i.data && EqualsNoCase(p.str(i.script), "SKI_ConfigManager") && EqualsNoCase(p.str(i.data->state), "BUSY")) {
				busy = true;
			}
		}
		r.mcmJam = mcmThreads > 0 && (mcmThreads >= 10 || registerMod >= 10 || busy);
		if (mcmThreads >= 10 || registerMod >= 10) {
			std::snprintf(buf, sizeof(buf),
				"SkyUI MCM registration was still running when this save was made: %zu menus are inside OnConfigManagerReady and "
				"%zu RegisterMod calls are queued%s. Loading replays all of it. Fix: Clean > Fix SkyUI MCM registration jam "
				"(removes the stuck registration work; SkyUI registers the remaining menus again on the next load).",
				mcmThreads, registerMod, busy ? ", and SKI_ConfigManager was saved in its BUSY state" : "");
			r.warnings.push_back(buf);
		}
		if (!s.isFO4() && !s.isLE()) {
			r.stranded = StrandedThreads(p).size();
			if (r.stranded) {
				std::snprintf(buf, sizeof(buf),
					"%zu thread(s) are waiting for nothing: not suspended, no queued call, no latent wait. The game never runs or ends them, "
					"and while one is there it cannot pause its scripts to save: every save of that session is written without script "
					"data. A save the game made has none; they come from deleting a suspended stack or queued call without its "
					"thread. Fix: --remove-stranded-threads (GUI: the Save check).",
					r.stranded);
				r.warnings.push_back(buf);
			}
		}
		if (!s.isFO4()) {
			if (const auto stuck = cf::FindStuckPlayerMovement(s); stuck.found) {
				r.warnings.push_back(cf::StuckMovementText(stuck));
			}
		}
		r.floods = flood::Find(s);
		for (const auto& f : r.floods) {
			char fb[1024];
			std::snprintf(fb, sizeof(fb),
				"Script-call flood: %zu calls of %s are queued (%zu not started, %zu already running, %zu left for another reason, "
				"%zu queued messages), on %zu object(s)%s%s. Events are queued faster than the game can run them: script lag. "
				"Fix: --clear-floods %s (drops only the calls that never started; it comes back unless the cause is fixed).",
				f.total(), f.key().c_str(), f.notStarted, f.running, f.other, f.messages, f.objects, f.targets.empty() ? "" : ", most on ",
				f.targets.empty() ? "" : flood::TargetText(f.targets.front()).c_str(), f.key().c_str());
			r.warnings.push_back(fb);
		}
		if (r.suspended >= 50) {
			std::snprintf(buf, sizeof(buf), "%zu suspended stacks are queued (normal is a handful).", r.suspended);
			r.warnings.push_back(buf);
		}
		if (r.threads >= 300) {
			std::snprintf(buf, sizeof(buf), "%zu active script threads were saved (a healthy save usually has under ~150).", r.threads);
			r.warnings.push_back(buf);
		}
		if (r.undefinedInstances || r.undefinedThreads) {
			std::snprintf(buf, sizeof(buf),
				"%zu script instances and %zu threads belong to scripts that are no longer installed. Clean > Remove Undefined fixes this.",
				r.undefinedInstances, r.undefinedThreads);
			r.warnings.push_back(buf);
		}
		if (r.unattached >= 100) {
			std::snprintf(buf, sizeof(buf), "%zu script instances are attached to nothing (Clean > Remove Unattached).", r.unattached);
			r.warnings.push_back(buf);
		}
		if (r.instances >= 150000) {
			std::snprintf(buf, sizeof(buf),
				"%zu script instances is very high; every load has to rebuild them, which makes the script engine slow to settle after loading.",
				r.instances);
			r.warnings.push_back(buf);
		}
		return r;
	}

	std::string FormatReport(const Report& r, std::size_t a_top)
	{
		std::string out;
		char        buf[512];
		std::snprintf(buf, sizeof(buf),
			"Scripts %zu | instances %zu | references %zu | arrays %zu | threads %zu | function messages %zu | suspended stacks %zu | change forms %zu | strings %zu\n"
			"Unattached instances %zu | undefined scripts %zu | undefined instances %zu | undefined threads %zu | terminated threads %zu | instances on missing created forms %zu\n",
			r.scripts, r.instances, r.references, r.arrays, r.threads, r.functionMessages, r.suspended, r.changeForms, r.strings, r.unattached,
			r.undefinedScripts, r.undefinedInstances, r.undefinedThreads, r.terminatedThreads, r.createdWithoutForm);
		out += buf;
		if (!r.warnings.empty()) {
			out += "\nFINDINGS\n";
			for (const auto& w : r.warnings) {
				out += "  * " + w + "\n";
			}
		}
		auto section = [&](const char* title, const std::vector<Count>& v) {
			if (v.empty()) {
				return;
			}
			out += std::string("\n") + title + "\n";
			for (std::size_t i = 0; i < v.size() && i < a_top; ++i) {
				std::snprintf(buf, sizeof(buf), "  %7zu  %s\n", v[i].n, v[i].name.c_str());
				out += buf;
			}
		};
		section("Threads by event script (frame 0)", r.threadsByTopScript);
		section("Threads by current frame (what they are waiting in)", r.threadsByCurrentFrame);
		section("Suspended stacks", r.suspendedByFunction);
		section("Function messages", r.messagesByFunction);
		section("Script instances by script", r.instancesByScript);
		section("Change forms by type", r.changeFormsByType);
		return out;
	}

	namespace
	{
		std::string LowerName(std::string a)
		{
			for (auto& c : a) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return a;
		}
		std::string Stem(const std::string& a)
		{
			const auto dot = a.find_last_of('.');
			return LowerName(dot == std::string::npos ? a : a.substr(0, dot));
		}
		bool PluginExt(const std::string& a)
		{
			const auto l = LowerName(a);
			return l.size() > 4 && (l.ends_with(".esp") || l.ends_with(".esm") || l.ends_with(".esl"));
		}
	}

	std::string RenamePlugin(SaveFile& s, const std::string& a_from, const std::string& a_to)
	{
		if (!PluginExt(a_to) || a_to.size() > 255 || a_to.find_first_of("\\/:*?\"<>|") != std::string::npos) {
			return "\"" + a_to + "\" is not a plugin file name (.esp, .esm or .esl)";
		}
		std::string* entry = nullptr;
		for (auto* list : { &s.plugins.full, &s.plugins.lite }) {
			for (auto& p : *list) {
				if (LowerName(p) == LowerName(a_to) && LowerName(p) != LowerName(a_from)) {
					return "the save already has a plugin named " + a_to + ": nothing changed";
				}
				if (LowerName(p) == LowerName(a_from)) {
					entry = &p;
				}
			}
		}
		if (!entry) {
			return "the save has no plugin named " + a_from;
		}
		*entry = a_to;
		return {};
	}

	RenameFinder::RenameFinder(const SaveFile& s, const std::vector<std::string>& a_installed)
	{
		std::set<std::string> inSave;
		for (const auto* list : { &s.plugins.full, &s.plugins.lite }) {
			for (const auto& p : *list) {
				inSave.insert(LowerName(p));
			}
		}
		// the installed plugins the save does not list, by name without extension (in a_installed's order)
		for (const auto& p : a_installed) {
			if (!inSave.contains(LowerName(p)) && PluginExt(p)) {
				_byStem[Stem(p)].push_back(p);
			}
		}
	}

	std::vector<std::string> RenameFinder::find(const std::string& a_missing) const
	{
		// = RenameCandidates(s, a_missing, a_installed), without reading the save and the list again for each plugin
		std::vector<std::string> out;
		const auto it = _byStem.find(Stem(a_missing));
		if (it == _byStem.end()) {
			return out;
		}
		const auto low = LowerName(a_missing);
		for (const auto& p : it->second) {
			if (LowerName(p) != low) {
				out.push_back(p);
			}
		}
		return out;
	}

	std::vector<std::string> RenameCandidates(const SaveFile& s, const std::string& a_missing, const std::vector<std::string>& a_installed)
	{
		std::set<std::string> inSave;
		for (const auto* list : { &s.plugins.full, &s.plugins.lite }) {
			for (const auto& p : *list) {
				inSave.insert(LowerName(p));
			}
		}
		std::vector<std::string> out;
		const auto stem = Stem(a_missing);
		for (const auto& p : a_installed) {
			if (!inSave.contains(LowerName(p)) && LowerName(p) != LowerName(a_missing) && Stem(p) == stem && PluginExt(p)) {
				out.push_back(p);
			}
		}
		return out;
	}
}
