// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "FollowersSky.h"

#include "ByteIO.h"
#include "ChangeFormData.h"
#include "Ops.h"
#include "Papyrus.h"
#include "Quests.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace ess::skyf
{
	namespace
	{
		template <class... A>
		std::string Fmt(const char* f, A... a)
		{
			char b[512];
			std::snprintf(b, sizeof(b), f, a...);
			return b;
		}

		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		bool StartsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

		const cf::Node* Kid(const cf::Node& n, const std::string& a_prefix)
		{
			for (const auto& k : n.kids) {
				if (StartsWith(k.label, a_prefix)) {
					return &k;
				}
			}
			return nullptr;
		}

		// a decoded change form's fields: the root node itself is named after the type ("ACHR", "NPC_")
		const cf::Node& Top(const cf::Node& a_root) { return a_root; }

		// the extra data entries of a decoded reference with this name
		std::vector<const cf::Node*> Extras(const cf::Node& a_root, const std::string& a_name)
		{
			std::vector<const cf::Node*> out;
			for (const auto& k : Top(a_root).kids) {
				if (StartsWith(k.label, "EXTRADATA (")) {
					for (const auto& e : k.kids) {
						if (e.label == a_name) {
							out.push_back(&e);
						}
					}
				}
			}
			return out;
		}

		int ToInt(const std::string& s)
		{
			try {
				return std::stoi(s, nullptr, 0);
			} catch (...) {
				return 0;
			}
		}

		// "[i]" groups of an array node: { FACTION, RANK }
		std::vector<FactionRank> FactionList(const cf::Node& a_array)
		{
			std::vector<FactionRank> out;
			for (const auto& e : a_array.kids) {
				const auto* f = Kid(e, "FACTION");
				const auto* r = Kid(e, "RANK");
				if (f && r) {
					const int rank = ToInt(r->value);
					out.push_back({ f->formID, rank > 127 ? rank - 256 : rank });  // a signed byte (0xFF = -1)
				}
			}
			return out;
		}

		std::uint32_t VsVal(Reader& r)
		{
			const std::uint8_t b0 = r.u8();
			std::uint32_t      v = b0;
			switch (b0 & 3) {
			case 0: break;
			case 1: v |= std::uint32_t(r.u8()) << 8; break;
			case 2:
				v |= std::uint32_t(r.u8()) << 8;
				v |= std::uint32_t(r.u8()) << 16;
				v |= std::uint32_t(r.u8()) << 24;
				break;
			default: throw FormatError("invalid vsval", r.pos());
			}
			return v >> 2;
		}

		GlobalData* GlobalsBlock(SaveFile& a_save)
		{
			for (auto& g : a_save.table1) {
				if (g.type == 3) {
					return &g;
				}
			}
			return nullptr;
		}

		// ---- Papyrus ----
		const ScriptInstance* InstanceOn(const SaveFile& a_save, std::uint32_t a_form, const std::string& a_script)
		{
			const auto* p = a_save.papyrus();
			if (!p) {
				return nullptr;
			}
			const auto want = Lower(a_script);
			for (const auto& i : p->instances) {
				if (!i.refID.isZero() && Lower(p->str(i.script)) == want && a_save.formIDOf(i.refID) == a_form) {
					return &i;
				}
			}
			return nullptr;
		}

		// a script variable by name (a property's own variable is "::<name>_var")
		const Variable* Var(const SaveFile& a_save, const ScriptInstance& a_inst, const std::string& a_name)
		{
			const auto* p = a_save.papyrus();
			if (!p || !a_inst.data) {
				return nullptr;
			}
			const auto names = ops::ExtendedMembers(*p, a_inst.script);
			const auto want = Lower(a_name), wantProp = Lower("::" + a_name + "_var");
			for (std::size_t i = 0; i < names.size() && i < a_inst.data->vars.size(); ++i) {
				const auto n = Lower(names[i]);
				if (n == want || n == wantProp) {
					return &a_inst.data->vars[i];
				}
			}
			return nullptr;
		}

		int BoolVar(const SaveFile& a_save, const ScriptInstance* a_inst, const std::string& a_name)
		{
			const auto* v = a_inst ? Var(a_save, *a_inst, a_name) : nullptr;
			if (!v || (v->type != VType::kBool && v->type != VType::kInt)) {
				return -1;
			}
			return v->value != 0 ? 1 : 0;
		}

		// the form an object variable points at: the script instance with that id, bound to a form
		std::uint32_t FormVar(const SaveFile& a_save, const ScriptInstance* a_inst, const std::string& a_name)
		{
			const auto* v = a_inst ? Var(a_save, *a_inst, a_name) : nullptr;
			const auto* p = a_save.papyrus();
			if (!v || v->type != VType::kRef || !p || v->value == 0) {
				return 0;
			}
			for (const auto& i : p->instances) {
				if (i.id == v->value) {
					return i.refID.isZero() ? 0 : a_save.formIDOf(i.refID);
				}
			}
			return 0;
		}

		// the player's race as saved: the player's base NPC (00000007) change form, NPC_RACE (flag 25): RACE, OLDRACE
		std::uint32_t PlayerRace(const SaveFile& a_save, std::string& a_from)
		{
			for (const auto& c : a_save.changeForms) {
				if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::NPC_ || a_save.formIDOf(c.refID) != 0x7) {
					continue;
				}
				if (!((c.changeFlags >> 25) & 1)) {
					a_from = "the player's NPC_ change form has no race change";
					return 0;
				}
				const auto d = cf::Decode(a_save, c);
				for (const auto& k : Top(d.root).kids) {
					if (k.label == "RACE" && k.isRef) {
						a_from = "NPC_ 00000007 NPC_RACE";
						return k.formID;
					}
				}
				a_from = "the player's NPC_ change form could not be read: " + d.error;
				return 0;
			}
			a_from = "the save has no change form for the player's NPC";
			return 0;
		}

		bool Has(const std::vector<std::uint32_t>& v, std::uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }
	}

	// ---------------------------------------------------------------------------------------------
	std::vector<GlobalValue> ReadGlobals(const SaveFile& a_save, std::string* a_error)
	{
		std::vector<GlobalValue> out;
		const auto*              g = GlobalsBlock(const_cast<SaveFile&>(a_save));
		if (!g) {
			if (a_error) {
				*a_error = "the save has no global variables block";
			}
			return out;
		}
		try {
			Reader     r(g->raw.data(), g->raw.size());
			const auto n = VsVal(r);
			for (std::uint32_t i = 0; i < n; ++i) {
				const auto  id = RefID::read(r);
				GlobalValue v;
				v.formID = id.isZero() ? 0 : a_save.formIDOf(id);
				v.at = r.pos();
				v.value = r.f32();
				out.push_back(v);
			}
			if (r.remaining() != 0) {
				throw FormatError("bytes left after the global variables", r.pos());
			}
		} catch (const std::exception& e) {
			out.clear();
			if (a_error) {
				*a_error = std::string("the global variables block could not be read: ") + e.what();
			}
		}
		return out;
	}

	std::string SetGlobal(SaveFile& a_save, std::uint32_t a_formID, float a_value)
	{
		std::string err;
		const auto  all = ReadGlobals(a_save, &err);
		if (!err.empty()) {
			return err;
		}
		auto* g = GlobalsBlock(a_save);
		for (const auto& v : all) {
			if (v.formID == a_formID) {
				std::memcpy(g->raw.data() + v.at, &a_value, 4);
				return {};
			}
		}
		return Fmt("the save has no value for the global %08X", a_formID);
	}

	// ---------------------------------------------------------------------------------------------
	int FollowerActor::rank(std::uint32_t a_faction) const
	{
		for (const auto& f : refFactions) {
			if (f.faction == a_faction) {
				return f.rank;
			}
		}
		for (const auto& f : baseFactions) {
			if (f.faction == a_faction) {
				return f.rank;
			}
		}
		return -2;
	}

	const FollowerActor* FollowerState::inAlias(std::int32_t a_alias) const
	{
		const FollowerActor* any = nullptr;
		for (const auto& a : actors) {
			if (a_alias == kFollowerAlias ? a.inFollowerAlias : a.inAnimalAlias) {
				if (!a.dead()) {
					return &a;
				}
				any = &a;
			}
		}
		return any;
	}

	FollowerState ReadFollowers(const SaveFile& a_save, const BaseOf& a_baseOf)
	{
		FollowerState st;
		if (a_save.isFO4()) {
			st.error = "Fallout 4 saves are not read for followers";
			return st;
		}
		// the quest and its script
		for (const auto& q : quests::ReadStates(a_save)) {
			if (q.formID == kDialogueFollower) {
				st.questInSave = q.hasFlags;
				st.questRunning = q.running();
			}
		}
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::QUST || a_save.formIDOf(c.refID) != kDialogueFollower) {
				continue;
			}
			const auto d = cf::Decode(a_save, c);
			if (const auto* run = Kid(d.root, "QUEST_RUN_DATA")) {
				st.hasRunData = true;
				if (const auto* items = Kid(*run, "ITEMS1 (")) {
					for (const auto& e : items->kids) {
						const auto* al = Kid(e, "UNK");
						const auto* fl = Kid(e, "FLAG");
						const auto* ref = Kid(e, "REF0");
						if (al && fl && ref && ToInt(fl->value) == 0) {
							st.runAliases.push_back({ ToInt(al->value), ref->formID });
						}
					}
				}
			}
		}
		const auto* script = InstanceOn(a_save, kDialogueFollower, "DialogueFollowerScript");
		st.scriptFound = script != nullptr;
		if (script) {
			if (const auto* v = Var(a_save, *script, "iFollowerDismiss"); v && v->type == VType::kInt) {
				st.iFollowerDismiss = static_cast<int>(static_cast<std::int32_t>(v->value));
			}
		}
		for (const auto& g : ReadGlobals(a_save)) {
			if (g.formID == kPlayerFollowerCount) {
				st.hasCount = true;
				st.followerCount = g.value;
			} else if (g.formID == kPlayerAnimalCount) {
				st.hasAnimalCount = true;
				st.animalCount = g.value;
			}
		}

		// base NPC faction changes (unique NPCs keep theirs there)
		std::unordered_map<std::uint32_t, std::vector<FactionRank>> baseFactions;
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::NPC_ || !((c.changeFlags >> 6) & 1)) {
				continue;
			}
			const auto d = cf::Decode(a_save, c);
			if (const auto* arr = Kid(Top(d.root), "FACTIONRANKS (")) {
				auto list = FactionList(*arr);
				for (const auto& f : list) {
					if (f.faction == kCurrentFollowerFaction || f.faction == kPlayerFollowerFaction || f.faction == kDismissedFollowerFaction ||
						f.faction == kCurrentHireling) {
						baseFactions[a_save.formIDOf(c.refID)] = std::move(list);
						break;
					}
				}
			}
		}

		std::unordered_map<std::uint32_t, cf::ActorPlace> places;
		for (const auto& a : cf::ListActors(a_save)) {
			places[a.refRaw] = a;
			if (a.isPlayer) {
				st.playerCell = a.cell;
			}
		}

		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::ACHR) {
				continue;
			}
			const auto formID = a_save.formIDOf(c.refID);
			if (formID == 0x14) {
				continue;  // the player
			}
			// the reference part (extra data) is read before the inventory: the quick read is enough to know
			const auto    quick = cf::Decode(a_save, c, {}, true);
			FollowerActor fa;
			fa.refRaw = c.refID.data;
			fa.formID = formID;
			fa.base = a_baseOf ? a_baseOf(formID) : 0;
			for (const auto* e : Extras(quick.root, "AliasInstanceArray")) {
				if (const auto* arr = Kid(*e, "ALIASES (")) {
					for (const auto& one : arr->kids) {
						const auto* q = Kid(one, "QUEST");
						const auto* al = Kid(one, "ALIAS");
						if (q && al) {
							fa.aliases.push_back({ q->formID, ToInt(al->value) });
						}
					}
				}
			}
			for (const auto* e : Extras(quick.root, "FactionChanges")) {
				if (const auto* arr = Kid(*e, "FactionChanges (")) {
					fa.refFactions = FactionList(*arr);
				}
			}
			if (fa.base) {
				if (auto it = baseFactions.find(fa.base); it != baseFactions.end()) {
					fa.baseFactions = it->second;
				}
			}
			for (const auto& [q, al] : fa.aliases) {
				if (q == kDialogueFollower) {
					fa.inFollowerAlias |= al == kFollowerAlias;
					fa.inAnimalAlias |= al == kAnimalAlias;
				}
			}
			// the teammate flag: the u32 right after the process level
			const auto body = cf::Body(c);
			if (quick.levelAt && quick.levelAt + 8 <= body.size()) {
				std::uint32_t bits;
				std::memcpy(&bits, body.data() + quick.levelAt + 4, 4);
				fa.teammate = (bits & kTeammateBit) != 0;
				fa.boolBitsAt = quick.levelAt + 4;
			}
			fa.disabled = quick.hasFormFlags && (quick.formFlags & 0x800);
			for (const auto& [al, ref] : st.runAliases) {
				if (ref == formID) {
					fa.inFollowerAlias |= al == kFollowerAlias;
					fa.inAnimalAlias |= al == kAnimalAlias;
				}
			}
			const bool sign = fa.inFollowerAlias || fa.inAnimalAlias || fa.teammate || fa.inFaction(kCurrentFollowerFaction) ||
			                  fa.inFaction(kPlayerFollowerFaction) || fa.inFaction(kDismissedFollowerFaction) || fa.inFaction(kCurrentHireling);
			if (!sign) {
				continue;
			}
			// the actor's own state (alive or dead): the full read
			const auto full = cf::Decode(a_save, c);
			if (const auto* actor = Kid(Top(full.root), "ACTOR")) {
				if (const auto* state = Kid(*actor, "ACTOR STATE")) {
					if (const auto* bits = Kid(*state, "BITS")) {
						unsigned b[5]{};
						if (std::sscanf(bits->value.c_str(), "%x %x %x %x %x", &b[0], &b[1], &b[2], &b[3], &b[4]) == 5) {
							fa.lifeState = static_cast<int>(b[4]);
							fa.decoded = true;
						}
					}
				}
			}
			if (auto it = places.find(c.refID.data); it != places.end() && it->second.hasPosition) {
				fa.hasPosition = true;
				fa.cell = it->second.cell;
				std::memcpy(fa.pos, it->second.pos, sizeof(fa.pos));
			}
			st.actors.push_back(std::move(fa));
		}

		// ---- findings ----
		auto add = [&](Issue i, std::uint32_t a, std::string t) { st.findings.push_back({ i, a, std::move(t) }); };
		const auto* follower = st.inAlias(kFollowerAlias);
		const auto* animal = st.inAlias(kAnimalAlias);
		const bool  followerAlive = follower && !follower->dead();
		if (st.hasCount && st.followerCount >= 1 && !followerAlive) {
			add(Issue::kCountWithoutFollower, 0,
				Fmt("PlayerFollowerCount is %g but nobody alive is in the vanilla follower quest's Follower alias. Followers then say "
					"\"you already have a follower\" and will not join. DismissFollower and FollowerAliasScript.OnDeath set it to 0 when the "
					"alias empties; here that last step did not happen.",
					st.followerCount));
		}
		if (st.hasCount && st.followerCount < 1 && followerAlive) {
			add(Issue::kFollowerWithoutCount, follower->formID,
				"Somebody is in the Follower alias but PlayerFollowerCount is 0, so another follower can be hired on top and the "
				"vanilla dialogue treats you as having none. SetFollower sets it to 1.");
		}
		if (st.hasAnimalCount && st.animalCount >= 1 && !(animal && !animal->dead())) {
			add(Issue::kAnimalCountWithoutAnimal, 0,
				Fmt("PlayerAnimalCount is %g but the Animal alias holds no living animal, so no dog or other animal will follow you. "
					"DismissAnimal sets it to 0 when it clears the alias.",
					st.animalCount));
		}
		for (const auto& [al, ref] : st.runAliases) {
			const bool listed = std::any_of(st.actors.begin(), st.actors.end(), [&](const FollowerActor& a) {
				return a.formID == ref && std::find(a.aliases.begin(), a.aliases.end(), std::pair<std::uint32_t, std::int32_t>{ kDialogueFollower, al }) != a.aliases.end();
			});
			if ((al == kFollowerAlias || al == kAnimalAlias) && !listed) {
				add(Issue::kAliasMismatch, ref,
					Fmt("The vanilla follower quest lists this reference in its %s alias, but the reference's own data does not say it is "
						"in it. The game rebuilds an alias from both; they should agree.",
						al == kFollowerAlias ? "Follower" : "Animal"));
			}
		}
		for (const auto& a : st.actors) {
			for (const auto& [q, al] : a.aliases) {
				if (q == kDialogueFollower && (al == kFollowerAlias || al == kAnimalAlias) && st.hasRunData &&
					std::find(st.runAliases.begin(), st.runAliases.end(), std::pair<std::int32_t, std::uint32_t>{ al, a.formID }) == st.runAliases.end()) {
					add(Issue::kAliasMismatch, a.formID,
						Fmt("This actor's own data says it is in the vanilla follower quest's %s alias, but the quest does not list it there.",
							al == kFollowerAlias ? "Follower" : "Animal"));
				}
			}
			if ((a.inFollowerAlias || a.inAnimalAlias) && a.dead()) {
				add(Issue::kDeadInAlias, a.formID,
					"This follower is dead but still in the vanilla follower quest's alias. FollowerAliasScript.OnDeath clears the alias "
					"and sets the count to 0; it did not run here.");
			}
			if (a.inFollowerAlias && a.disabled) {
				add(Issue::kDisabledInAlias, a.formID, "This follower is in the Follower alias but disabled (taken out of the world), so it cannot follow you.");
			}
			if (a.inFollowerAlias && !a.dead() && a.hasPosition && st.playerCell && a.cell != st.playerCell) {
				add(Issue::kFollowerAway, a.formID,
					"Your follower is not in the cell or worldspace you are in. If it is not waiting for you, it is lost; bring it to you "
					"in the game (Gather).");
			}
			if (a.inFollowerAlias && a.inFaction(kDismissedFollowerFaction)) {
				add(Issue::kDismissedInAlias, a.formID,
					"This follower is in the Follower alias and also in DismissedFollowerFaction: half dismissed. SetFollower takes it out "
					"of that faction and DismissFollower puts it in; both cannot be true.");
			}
			if (a.inFollowerAlias && a.decoded && !a.teammate && !a.dead()) {
				add(Issue::kAliasNotTeammate, a.formID, "This follower is in the Follower alias but is not your teammate: SetFollower makes it one.");
			}
			if (a.teammate && !a.inFollowerAlias && !a.inAnimalAlias && a.inFaction(kDismissedFollowerFaction) && !a.dead()) {
				add(Issue::kDismissedTeammate, a.formID,
					"This follower was dismissed (it is in DismissedFollowerFaction) but is still flagged as your teammate. "
					"DismissFollower puts it in that faction and switches the teammate flag off in the same go; here the flag "
					"stayed on, so it still follows your sneaking, crimes and fights.");
			} else if (a.teammate && a.aliases.empty() && !a.inFaction(kCurrentFollowerFaction) && !a.dead()) {
				add(Issue::kStrayTeammate, a.formID,
					"This actor is flagged as your teammate but no quest holds it as a follower and it is not in "
					"CurrentFollowerFaction. A mod may keep it as a teammate on purpose; if it is a follower you dismissed, "
					"the flag was left on (DismissFollower switches it off).");
			}
			if (a.inFaction(kCurrentFollowerFaction) && a.aliases.empty() && !a.dead()) {
				add(Issue::kFactionWithoutAlias, a.formID,
					"This actor is in CurrentFollowerFaction but in no quest alias, so no quest makes it follow you any more: a lost "
					"follower. The Follower alias gives that faction while it holds the actor.");
			}
		}
		return st;
	}

	std::string FixFollowerCount(SaveFile& a_save, bool a_animal, std::string* a_done)
	{
		const auto st = ReadFollowers(a_save);
		if (!st.error.empty()) {
			return st.error;
		}
		const auto* who = st.inAlias(a_animal ? kAnimalAlias : kFollowerAlias);
		const bool  alive = who && !who->dead();
		const float want = alive ? 1.0f : 0.0f;
		const bool  has = a_animal ? st.hasAnimalCount : st.hasCount;
		const float now = a_animal ? st.animalCount : st.followerCount;
		if (!has) {
			return a_animal ? "the save has no value for PlayerAnimalCount" : "the save has no value for PlayerFollowerCount";
		}
		if ((now >= 1) == alive) {
			return a_animal ? "PlayerAnimalCount already matches the Animal alias" : "PlayerFollowerCount already matches the Follower alias";
		}
		if (auto e = SetGlobal(a_save, a_animal ? kPlayerAnimalCount : kPlayerFollowerCount, want); !e.empty()) {
			return e;
		}
		if (a_done) {
			*a_done = Fmt("%s %g -> %g", a_animal ? "PlayerAnimalCount" : "PlayerFollowerCount", now, want);
		}
		return {};
	}

	std::string ClearTeammate(SaveFile& a_save, std::uint32_t a_actorFormID)
	{
		if (a_actorFormID == 0x14) {
			return "not the player";
		}
		for (auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) != ChangeFormType::ACHR || a_save.formIDOf(c.refID) != a_actorFormID) {
				continue;
			}
			const auto d = cf::Decode(a_save, c);
			if (!d.complete) {
				return "the actor's saved data cannot be read completely (" + d.error + "), so it is not changed";
			}
			auto body = cf::Body(c);
			const std::size_t bitsAt = d.levelAt + 4, flagsAt = d.movement.second;
			if (!d.levelAt || bitsAt + 4 > body.size() || !flagsAt || flagsAt + 4 > body.size()) {
				return "the actor's flags were not found";
			}
			std::uint32_t bits, flags;
			std::memcpy(&bits, body.data() + bitsAt, 4);
			std::memcpy(&flags, body.data() + flagsAt, 4);
			if (!(bits & kTeammateBit)) {
				return "the actor is not a teammate";
			}
			bits &= ~kTeammateBit;
			flags |= kCanDoFavorBit;
			std::memcpy(body.data() + bitsAt, &bits, 4);
			std::memcpy(body.data() + flagsAt, &flags, 4);
			const auto old = c;
			cf::SetBody(c, body);
			const auto back = cf::Decode(a_save, c);
			if (!back.complete) {
				c = old;
				return "the changed actor did not read back; nothing was changed";
			}
			return {};
		}
		return Fmt("the save has no change form for the actor %08X", a_actorFormID);
	}

	// ---------------------------------------------------------------------------------------------
	std::vector<BeastState> ReadBeastForms(const SaveFile& a_save)
	{
		std::vector<BeastState> out;
		if (a_save.isFO4()) {
			return out;
		}
		// Dawnguard's quest: its form id in this save's load order (local id 0071D0)
		std::uint32_t dlc1 = 0;
		for (std::size_t i = 0; i < a_save.plugins.full.size(); ++i) {
			if (Lower(a_save.plugins.full[i]) == "dawnguard.esm") {
				dlc1 = static_cast<std::uint32_t>(i) << 24 | 0x0071D0;
			}
		}
		const auto states = quests::ReadStates(a_save);
		const auto globals = ReadGlobals(a_save);
		auto       global = [&](std::uint32_t id, float& v) {
            for (const auto& g : globals) {
                if (g.formID == id) {
                    v = g.value;
                    return true;
                }
            }
            return false;
		};
		std::string raceFrom;
		const auto  race = PlayerRace(a_save, raceFrom);

		// the player's own data: added spells, factions, inventory
		std::vector<std::uint32_t> spells, items;
		std::vector<FactionRank>   factions;
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR && a_save.formIDOf(c.refID) == 0x14) {
				const auto d = cf::Decode(a_save, c);
				for (const auto& s : d.addedSpells) {
					spells.push_back(s.id);
				}
				for (const auto& i : d.inventory) {
					items.push_back(i.item);
				}
				for (const auto* e : Extras(d.root, "FactionChanges")) {
					if (const auto* arr = Kid(*e, "FactionChanges (")) {
						factions = FactionList(*arr);
					}
				}
			}
		}
		// the player is unique: AddToFaction goes to the base NPC's faction list
		std::vector<FactionRank> baseFactions;
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::NPC_ && a_save.formIDOf(c.refID) == 0x7 && ((c.changeFlags >> 6) & 1)) {
				const auto d = cf::Decode(a_save, c);
				if (const auto* arr = Kid(Top(d.root), "FACTIONRANKS (")) {
					baseFactions = FactionList(*arr);
				}
			}
		}
		auto inFaction = [&](std::uint32_t f) {
			for (const auto& x : factions) {
				if (x.faction == f) {
					return x.rank >= 0;
				}
			}
			for (const auto& x : baseFactions) {
				if (x.faction == f) {
					return x.rank >= 0;
				}
			}
			return false;
		};

		for (int k = 0; k < 2; ++k) {
			BeastState b;
			b.kind = k == 0 ? Beast::kWerewolf : Beast::kVampireLord;
			b.questEdid = k == 0 ? "PlayerWerewolfQuest" : "DLC1PlayerVampireQuest";
			b.quest = k == 0 ? kPlayerWerewolfQuest : dlc1;
			if (!b.quest) {
				continue;  // no Dawnguard
			}
			for (const auto& q : states) {
				if (q.formID == b.quest) {
					b.questInSave = q.hasFlags;
					b.questRunning = q.running();
					for (const auto& [s, done] : q.stages) {
						if (done) {
							b.stagesDone.push_back(s);
						}
					}
				}
			}
			const auto* inst = InstanceOn(a_save, b.quest, k == 0 ? "PlayerWerewolfChangeScript" : "DLC1PlayerVampireChangeScript");
			b.scriptFound = inst != nullptr;
			b.playerRace = race;
			b.playerRaceFrom = raceFrom;
			if (inst) {
				b.trackingStarted = BoolVar(a_save, inst, "__trackingStarted");
				b.tryingToShiftBack = BoolVar(a_save, inst, "__tryingToShiftBack");
				b.shiftingBack = BoolVar(a_save, inst, "__shiftingBack");
				b.shuttingDown = BoolVar(a_save, inst, "__shuttingDown");
				b.beastRace = FormVar(a_save, inst, k == 0 ? "WerewolfBeastRace" : "VampireLordRace");
				if (k == 0) {
					const auto* c00 = InstanceOn(a_save, FormVar(a_save, inst, "CompanionsTrackingQuest"), "CompanionsHousekeepingScript");
					b.originalRace = FormVar(a_save, c00, "PlayerOriginalRace");
				} else {
					const auto* tq = InstanceOn(a_save, FormVar(a_save, inst, "VampireTrackingQuest"), "DLC1VampireTrackingQuest");
					b.originalRace = FormVar(a_save, tq, "PlayerRace");
				}
				// what ShiftBack / ActuallyShiftBackIfNecessary take away again
				static const char* kWolfSpells[] = { "PlayerWerewolfLvl10AndBelowAbility", "PlayerWerewolfLvl15AndBelowAbility",
					"PlayerWerewolfLvl20AndBelowAbility", "PlayerWerewolfLvl25AndBelowAbility", "PlayerWerewolfLvl30AndBelowAbility",
					"PlayerWerewolfLvl35AndBelowAbility", "PlayerWerewolfLvl40AndBelowAbility", "PlayerWerewolfLvl45AndBelowAbility",
					"PlayerWerewolfLvl50AndOverAbility" };
				static const char* kLordSpells[] = { "LeveledDrainSpell", "LeveledAbility", "LeveledRaiseDeadSpell", "DLC1VampiresGrip",
					"DLC1ConjureGargoyleLeftHand", "DLC1CorpseCurse", "DLC1VampireDetectLife", "DLC1VampireMistForm", "DLC1VampireBats",
					"DLC1SupernaturalReflexes", "DLC1NightCloak", "DLC1Revert", "DLC1VampireLordSunDamage", "DLC1AbVampireFloatBodyFX" };
				std::set<std::uint32_t> seen;
				auto                    check = [&](const char* name) {
                    const auto f = FormVar(a_save, inst, name);
                    if (f && Has(spells, f) && seen.insert(f).second) {
                        b.spells.push_back(f);
                    }
				};
				if (k == 0) {
					for (const char* s : kWolfSpells) {
						check(s);
					}
				} else {
					for (const char* s : kLordSpells) {
						check(s);
					}
				}
				for (const char* f : k == 0 ? std::vector<const char*>{ "PlayerWerewolfFaction", "WerewolfFaction" } : std::vector<const char*>{ "PlayerVampireFaction" }) {
					const auto id = FormVar(a_save, inst, f);
					if (id && inFaction(id)) {
						b.factions.push_back(id);
					}
				}
				const auto armor = FormVar(a_save, inst, k == 0 ? "WolfSkinFXArmor" : "DLC1VampireLordArmor");
				if (armor && Has(items, armor)) {
					b.items.push_back(armor);
				}
				const auto shiftGlobal = FormVar(a_save, inst, k == 0 ? "PlayerWerewolfShiftBackTime" : "PlayerVampireShiftBackTime");
				b.hasShiftBackTime = shiftGlobal && global(shiftGlobal, b.shiftBackTime);
				global(0x39, b.gameDaysPassed);  // GameDaysPassed (Skyrim.esm)
				if (k == 1) {
					float lv = 0;
					if (const auto g = FormVar(a_save, inst, "DCL1VampireLevitateStateGlobal"); g && global(g, lv)) {
						b.levitateState = static_cast<int>(lv);
					}
				}
			}
			b.raceIsBeast = race && b.beastRace && race == b.beastRace;
			if (inst) {
				b.untimed = BoolVar(a_save, inst, "Untimed") == 1;
				if (k == 1) {
					const auto revert = FormVar(a_save, inst, "DLC1Revert");
					b.hasRevertPower = revert && Has(spells, revert);
				}
			}
			const bool ran = !b.stagesDone.empty();
			if (!b.questInSave && !b.raceIsBeast) {
				continue;  // never transformed: nothing to show (a lone spell may come from another mod)
			}
			const bool shiftBackDone = std::find(b.stagesDone.begin(), b.stagesDone.end(), kShiftBackStage) != b.stagesDone.end();
			const char* form = k == 0 ? "werewolf" : "Vampire Lord";
			using F = BeastState::Fix;
			if (b.raceIsBeast && !b.questRunning) {
				b.fix = F::kGameRevert;
				b.problem = Fmt("You are still a %s (the player's race is the beast race), but the quest that turns you back (%s) is not "
								"running, so nothing will ever change you back.",
					form, b.questEdid.c_str());
			} else if (b.raceIsBeast && shiftBackDone) {
				b.fix = F::kGameRevert;
				b.problem = Fmt("You are still a %s although the turn-back stage (%d) has already been set: the script's ShiftBack stopped "
								"half way (it waits for a synced animation to end and for the race change) and will not run again by "
								"itself, because its \"already doing it\" guards are set.",
					form, kShiftBackStage);
			} else if (b.raceIsBeast && k == 0 && !b.untimed && b.hasShiftBackTime && b.gameDaysPassed > b.shiftBackTime + 0.05f) {
				b.fix = F::kSetStage;
				b.problem = Fmt("Your werewolf time ran out %.1f game hours ago (PlayerWerewolfShiftBackTime %.3f, GameDaysPassed %.3f), but "
								"you were not turned back. The script's update (every 5 seconds) sets stage %d when the time is up; it "
								"stopped coming.",
					(b.gameDaysPassed - b.shiftBackTime) * 24.0f, b.shiftBackTime, b.gameDaysPassed, kShiftBackStage);
			} else if (b.raceIsBeast && k == 1 && !b.hasRevertPower) {
				b.fix = F::kSetStage;
				b.problem = Fmt("You are a Vampire Lord without the Revert Form power, so you cannot turn back yourself. Revert Form sets "
								"stage %d of %s; that can be done for you.",
					kShiftBackStage, b.questEdid.c_str());
			} else if (!b.raceIsBeast && b.questRunning && shiftBackDone) {
				b.fix = F::kGameRevert;
				b.problem = Fmt("You are back to your own race, but %s is still running after its turn-back stage: its Shutdown never "
								"finished. While it runs, the next transformation cannot start it again, and what Shutdown resets "
								"(beast form menus, fast travel, being a ghost) may still be on.",
					b.questEdid.c_str());
			} else if (!b.raceIsBeast && !b.questRunning && ran && b.leftovers()) {
				b.fix = F::kGameRevert;
				b.problem = Fmt("You are back to your own race, but things the transformation gives and its turn-back takes away are still "
								"on you: %zu spell(s), %zu faction(s), %zu item(s).",
					b.spells.size(), b.factions.size(), b.items.size());
			}
			if (b.fix == F::kGameRevert && b.raceIsBeast && !b.originalRace) {
				b.fix = F::kCannot;
				b.problem += Fmt(" The script has no race to turn you back into (%s is None in the save), so its own turn-back would leave "
								 "you as you are; set your race in the game's console (player.setrace <your race>), then use this again.",
					k == 0 ? "CompanionsHousekeepingScript.PlayerOriginalRace" : "DLC1VampireTrackingQuest.PlayerRace");
			}
			out.push_back(std::move(b));
		}
		return out;
	}

	// ---------------------------------------------------------------------------------------------
	std::vector<StuckStage> FindStuckStages(const SaveFile& a_save, const StagesOf& a_shutdownStages)
	{
		std::vector<StuckStage> out;
		const auto*             p = a_save.papyrus();
		if (!p || a_save.isFO4()) {
			return out;
		}
		std::map<std::uint32_t, const quests::QuestState*> byID;
		const auto                                          states = quests::ReadStates(a_save);
		for (const auto& q : states) {
			byID[q.formID] = &q;
		}
		std::set<std::pair<std::uint32_t, int>> seen;
		for (const auto& t : p->activeScripts) {
			if (!t.data || !t.data->fragment || t.data->fragment->typeCode != "QuestStage" || t.data->fragment->payload.size() < 5) {
				continue;
			}
			const auto& pl = t.data->fragment->payload;  // RefID quest, u16 stage, u8
			RefID       q;
			q.data = (std::uint32_t(pl[0]) << 16) | (std::uint32_t(pl[1]) << 8) | pl[2];
			const auto quest = q.isZero() ? 0 : a_save.formIDOf(q);
			const int  stage = pl[3] | (pl[4] << 8);
			if (!quest || !seen.insert({ quest, stage }).second) {
				continue;
			}
			StuckStage s;
			s.quest = quest;
			s.stage = stage;
			s.kind = "fragment running";
			s.text = Fmt("The save holds a running script for stage %d of this quest: the stage was set but its script had not finished "
						 "when you saved. If the quest does not move on after loading, that script is stuck (waiting on something that "
						 "never happens, or in a loop).",
				stage);
			out.push_back(s);
		}
		if (a_shutdownStages) {
			for (const auto& q : states) {
				if (!q.running()) {
					continue;
				}
				for (const int st : a_shutdownStages(q.formID)) {
					for (const auto& [s, done] : q.stages) {
						if (s == st && done && !seen.contains({ q.formID, st })) {
							StuckStage x;
							x.quest = q.formID;
							x.stage = st;
							x.kind = "shutdown stage done, still running";
							x.text = Fmt("Stage %d is the quest's shut-down stage and is done, yet the quest is still running and no script of "
										 "that stage is running.",
								st);
							out.push_back(x);
						}
					}
				}
			}
		}
		return out;
	}
}
