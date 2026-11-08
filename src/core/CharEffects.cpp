// ReSaver++ - Character effects: see CharEffects.h for what is read and where it was checked.
#include "CharEffects.h"

#include "ChangeFormData.h"
#include "ChangeFormFO4.h"
#include "PluginDB.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

namespace ess::chfx
{
	namespace
	{
		std::string Fmt(const char* a_fmt, ...)
		{
			char    buf[1024];
			va_list ap;
			va_start(ap, a_fmt);
			std::vsnprintf(buf, sizeof(buf), a_fmt, ap);
			va_end(ap);
			return buf;
		}
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}
		constexpr std::uint32_t kPlayer = 0x14;

		// a 3-byte RefID in an effect's data, as a runtime form id (0: none)
		std::uint32_t Ref3(const SaveFile& a_save, const std::uint8_t* a_p)
		{
			RefID r;
			r.data = (std::uint32_t(a_p[0]) << 16) | (std::uint32_t(a_p[1]) << 8) | a_p[2];
			return r.data ? a_save.formIDOf(r) : 0;
		}
		void ReadRefs(const SaveFile& a_save, const std::uint8_t* a_data, std::size_t a_size, Effect& a_e)
		{
			if (a_size < 28) {
				return;
			}
			a_e.hasRefs = true;
			a_e.caster = Ref3(a_save, a_data + 16);
			a_e.target = Ref3(a_save, a_data + 19);
			a_e.source = Ref3(a_save, a_data + 25);
		}

		// the doll place of a worn item, by its record type and biped slots
		Slot SlotOf(bool a_fo4, const Worn& a_w, bool a_hasBiped)
		{
			if (a_w.type.empty()) {
				return Slot::kUnknown;
			}
			if (a_w.type == "WEAP") {
				return a_w.left ? Slot::kLeftHand : Slot::kRightHand;
			}
			if (!a_fo4 && a_w.type == "LIGH") {
				return Slot::kLeftHand;  // a torch
			}
			if (!a_fo4 && a_w.type == "AMMO") {
				return Slot::kAmmo;
			}
			if (a_w.type != "ARMO") {
				return Slot::kOther;
			}
			if (!a_hasBiped) {
				return Slot::kUnknown;
			}
			const auto b = a_w.biped;
			auto       any = [b](std::initializer_list<int> a_bits) {
                for (const int i : a_bits) {
                    if (b & (1u << i)) {
                        return true;
                    }
                }
                return false;
			};
			if (!a_fo4) {  // xEdit tes5: 30 Head 31 Hair 32 Body 33 Hands 34 Forearms 35 Amulet 36 Ring 37 Feet 38 Calves 39 Shield 41 LongHair 42 Circlet 43 Ears
				if (any({ 2 })) return Slot::kBody;
				if (any({ 9 })) return Slot::kShield;
				if (any({ 0, 1, 11, 12, 13 })) return Slot::kHead;
				if (any({ 3, 4 })) return Slot::kHands;
				if (any({ 7, 8 })) return Slot::kFeet;
				if (any({ 5 })) return Slot::kAmulet;
				if (any({ 6 })) return Slot::kRing;
				return Slot::kOther;
			}
			// xEdit wbDefinitionsFO4: 30 Hair Top 31 Hair Long 32 FaceGen Head 33 BODY 34 L Hand 35 R Hand 36-40 [U] Torso / L Arm /
			// R Arm / L Leg / R Leg 41-45 [A] the same 46 Headband 47 Eyes 48 Beard 49 Mouth 50 Neck 51 Ring 52 Scalp 60 Pipboy
			if (any({ 3 })) return Slot::kBody;
			if (any({ 11 })) return Slot::kChest;
			if (any({ 0, 1, 2, 16, 17, 18, 19, 22 })) return Slot::kHead;
			if (any({ 12, 7 })) return Slot::kLeftArm;
			if (any({ 13, 8 })) return Slot::kRightArm;
			if (any({ 14, 9 })) return Slot::kLeftLeg;
			if (any({ 15, 10 })) return Slot::kRightLeg;
			if (any({ 6 })) return Slot::kBody;
			if (any({ 4, 5 })) return Slot::kHands;
			if (any({ 20 })) return Slot::kNeck;
			if (any({ 21 })) return Slot::kRing;
			if (any({ 30 })) return Slot::kPipBoy;
			return Slot::kOther;
		}

		const ChangeForm* SkyrimPlayer(const SaveFile& a_save)
		{
			for (const auto& c : a_save.changeForms) {
				if (c.refID.data == 0x400014 && static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR) {
					return &c;
				}
			}
			return nullptr;
		}
	}

	const char* GroupName(Group a_group)
	{
		switch (a_group) {
		case Group::kWorn: return "Worn items";
		case Group::kNotWorn: return "Items not worn";
		case Group::kSpells: return "Spells, powers and shouts";
		case Group::kConsumables: return "Potions, food and ingredients";
		case Group::kAbilities: return "Abilities (race, standing stone, perks, mods)";
		case Group::kDiseases: return "Diseases and addictions";
		case Group::kOthers: return "Put on you by others";
		case Group::kUnknown: return "Unknown (plugin missing or not read)";
		default: return "";
		}
	}

	const char* SlotName(Slot a_slot)
	{
		switch (a_slot) {
		case Slot::kHead: return "Head";
		case Slot::kBody: return "Body";
		case Slot::kHands: return "Hands";
		case Slot::kFeet: return "Feet";
		case Slot::kAmulet: return "Amulet";
		case Slot::kRing: return "Ring";
		case Slot::kShield: return "Shield";
		case Slot::kRightHand: return "Right hand";
		case Slot::kLeftHand: return "Left hand";
		case Slot::kAmmo: return "Ammo";
		case Slot::kChest: return "Chest armor";
		case Slot::kLeftArm: return "Left arm";
		case Slot::kRightArm: return "Right arm";
		case Slot::kLeftLeg: return "Left leg";
		case Slot::kRightLeg: return "Right leg";
		case Slot::kNeck: return "Neck";
		case Slot::kPipBoy: return "Pip-Boy";
		case Slot::kOther: return "Other slots";
		case Slot::kUnknown: return "Slot not known";
		default: return "";
		}
	}

	const std::vector<Slot>& DollSlots(bool a_fo4)
	{
		static const std::vector<Slot> sky = { Slot::kHead, Slot::kAmulet, Slot::kBody, Slot::kHands, Slot::kRing, Slot::kFeet, Slot::kRightHand,
			Slot::kLeftHand, Slot::kShield, Slot::kAmmo };
		static const std::vector<Slot> f4 = { Slot::kHead, Slot::kNeck, Slot::kBody, Slot::kChest, Slot::kLeftArm, Slot::kRightArm, Slot::kHands,
			Slot::kRing, Slot::kLeftLeg, Slot::kRightLeg, Slot::kRightHand, Slot::kPipBoy };
		return a_fo4 ? f4 : sky;
	}

	std::string View::name(std::uint32_t a_id) const
	{
		if (const auto* f = db ? db->find(a_id) : nullptr) {
			auto n = db->name(*f);
			if (n.empty()) {
				n = db->edid(*f);
			}
			if (!n.empty()) {
				return n;
			}
		}
		if ((a_id >> 24) == 0xFF) {
			if (const auto it = created.find(a_id); it != created.end()) {
				return Fmt("%08X (%s made in game)", a_id, it->second.c_str());
			}
			return Fmt("%08X (made in game)", a_id);
		}
		return Fmt("%08X", a_id);
	}

	View Build(const SaveFile& a_save, const Options& a_options)
	{
		View v;
		v.fo4 = a_save.isFO4();
		v.le = a_save.isLE();
		v.db = a_options.db;
		v.save = &a_save;
		const auto* db = a_options.db;
		std::set<std::uint32_t> inventoryItems;  // everything the player carries
		for (const auto& c : a_save.changeForms) {
			if (c.refID.type() == RefID::Type::kCreated) {
				v.created[a_save.formIDOf(c.refID)] = ChangeFormTypeName(c.typeCode(), v.fo4);
			}
		}

		// ---- the effects and the worn stacks ----
		if (!v.fo4) {
			const auto* pc = SkyrimPlayer(a_save);
			if (!pc) {
				v.error = "this save has no player change form";
				return v;
			}
			std::string err;
			const auto  list = cf::ReadPlayerActiveEffects(a_save, err);
			if (!err.empty()) {
				v.error = err;
				v.removable = false;
				return v;
			}
			const auto d = cf::Decode(a_save, *pc);
			std::map<std::pair<std::uint32_t, bool>, std::size_t> wornAt;
			for (const auto& st : d.stacks) {
				inventoryItems.insert(st.item);
				if (!st.worn && !st.wornLeft) {
					continue;
				}
				const auto key = std::make_pair(st.item, st.wornLeft && !st.worn);
				auto       it = wornAt.find(key);
				if (it == wornAt.end()) {
					Worn w;
					w.item = st.item;
					w.left = key.second;
					it = wornAt.emplace(key, v.worn.size()).first;
					v.worn.push_back(w);
				}
				auto& w = v.worn[it->second];
				w.count += st.count;
				if (st.ench) {
					w.enchantment = st.ench;
				}
			}
			for (const auto& e : list) {
				Effect x;
				x.index = e.index;
				x.item = e.magicItem;
				x.effectIndex = e.effectIndex;
				x.archetype = e.type;
				x.elapsed = e.elapsed;
				x.magnitude = e.magnitude;
				x.duration = e.duration;
				x.flags = e.flags;
				ReadRefs(a_save, e.data.data(), e.data.size(), x);
				if (db) {
					if (const auto* fx = db->effects(e.magicItem); fx && e.effectIndex < fx->size()) {
						x.mgef = (*fx)[e.effectIndex].effect;
						if (const auto* m = db->magicEffect(x.mgef)) {
							x.actorValue = m->actorValue;
						}
					}
				}
				const int slot = cf::ActiveEffectModifier(e);
				x.stat = slot >= 0 && e.magnitude != 0;
				if (x.stat && x.actorValue < 0) {
					x.canRemove = false;
					x.why = "it changes a stat, and which one is in its plugin (" + a_save.pluginOf(e.magicItem) +
					        (db ? "), which was not read" : "): load the plugin names first");
				} else if (x.stat) {
					x.why = Fmt("%s %+g (%s)", cf::ActorValueName(static_cast<std::uint32_t>(x.actorValue)), -e.magnitude, slot ? "temporary" : "permanent");
				}
				v.effects.push_back(std::move(x));
			}
		} else {
			const auto* pc = fo4cf::FindPlayer(a_save);
			if (!pc) {
				v.error = "the player's saved data (ACHR 00000014) is not in this save";
				return v;
			}
			const auto act = fo4cf::DecodeActor(a_save, *pc);
			if (!act.decoded) {
				v.error = "the player's saved data could not be read: " + act.error;
				return v;
			}
			v.removable = act.complete;
			const auto refr = fo4cf::DecodeRefr(a_save, *pc);
			for (const auto& it : refr.inventory) {
				inventoryItems.insert(it.formID);
				bool        worn = false;
				std::int32_t n = 0;
				for (const auto& st : it.stacks) {
					if (st.flags & 7) {  // equipped (Actor.IsEquipped 0x502380: stack flags & 7)
						worn = true;
						n += st.count;
					}
				}
				if (worn) {
					Worn w;
					w.item = it.formID;
					w.count = n;
					v.worn.push_back(w);
				}
			}
			const auto body = cf::Body(*pc);
			const auto sources = fo4cf::EffectSources(a_save, act, db);
			for (std::size_t i = 0; i < act.effects.size(); ++i) {
				const auto& e = act.effects[i];
				Effect      x;
				x.index = i;
				x.item = a_save.formIDOf(e.item);
				x.effectIndex = e.effectIndex;
				x.archetype = e.archetype;
				x.elapsed = e.elapsed;
				x.magnitude = e.magnitude;
				x.duration = e.duration;
				x.flags = e.flags;
				if (e.data.end <= body.size() && e.data.size() >= 28) {
					ReadRefs(a_save, body.data() + e.data.begin, e.data.size(), x);
				}
				const auto& src = sources[i];
				x.mgef = src.mgef;
				const auto plan = fo4cf::PlanEffectRemoval(act, e, &src);
				x.stat = plan.modifier && plan.amount != 0;
				if (x.stat) {
					x.actorValue = plan.actorValue;
				}
				if (!plan.ok) {
					x.canRemove = false;
					x.why = plan.why;
				} else if (!act.complete) {
					x.canRemove = false;
					x.why = "the player's saved data does not read completely: " + act.error;
				} else if (x.stat) {
					x.why = Fmt("%08X %+g (modifier %d)", plan.actorValue, -plan.amount, plan.slot);
				}
				v.effects.push_back(std::move(x));
			}
		}

		// ---- worn items: type, slots, enchantment from the plugins ----
		std::set<std::uint32_t> wornItems, wornEnch;
		for (auto& w : v.worn) {
			bool hasBiped = false;
			if (const auto* f = db ? db->find(w.item) : nullptr) {
				w.type = db->type(*f);
				w.biped = f->bipedSlots;
				hasBiped = f->hasBiped;
				w.baseEnchantment = f->enchantment;
				if (!w.enchantment) {
					w.enchantment = f->enchantment;
				}
			}
			w.slot = SlotOf(v.fo4, w, hasBiped);
			wornItems.insert(w.item);
			if (w.enchantment) {
				wornEnch.insert(w.enchantment);
			}
			if (w.baseEnchantment) {
				wornEnch.insert(w.baseEnchantment);
			}
		}

		// ---- cards ----
		std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> cardAt;
		for (std::size_t i = 0; i < v.effects.size(); ++i) {
			const auto& e = v.effects[i];
			const auto* f = db ? db->find(e.item) : nullptr;
			const auto  type = f ? db->type(*f) : std::string();
			const bool  other = e.caster && e.caster != kPlayer;
			const bool  ench = e.fromItem() || type == "ENCH";
			const auto  key = std::make_pair(e.item, ench || other ? e.source : 0u);
			auto        it = cardAt.find(key);
			if (it == cardAt.end()) {
				Card c;
				c.item = e.item;
				c.source = key.second;
				c.type = type;
				c.known = f != nullptr;
				c.spellType = f ? f->spellType : 0xFF;
				c.plugin = a_save.pluginOf(e.item);
				c.sourceWorn = c.source && wornItems.contains(c.source);
				if (other) {
					c.group = Group::kOthers;
				} else if (ench) {
					c.group = c.sourceWorn || wornEnch.contains(e.item) ? Group::kWorn : Group::kNotWorn;
				} else if (!f) {
					c.group = Group::kUnknown;
				} else if (type == "SPEL") {
					switch (f->spellType) {
					case 4: c.group = Group::kAbilities; break;
					case 1:
					case 10: c.group = Group::kDiseases; break;
					case 5: c.group = Group::kConsumables; break;
					default: c.group = Group::kSpells; break;
					}
				} else if (type == "ALCH" || type == "INGR") {
					c.group = Group::kConsumables;
				} else if (type == "SCRL") {
					c.group = Group::kSpells;
				} else {
					c.group = Group::kUnknown;
				}
				it = cardAt.emplace(key, v.cards.size()).first;
				v.cards.push_back(std::move(c));
			}
			auto& c = v.cards[it->second];
			c.effects.push_back(i);
			c.running += !e.off();
			c.removable = c.removable && e.canRemove && v.removable;
		}
		std::stable_sort(v.cards.begin(), v.cards.end(), [](const Card& a, const Card& b) {
			if (a.group != b.group) {
				return a.group < b.group;
			}
			return a.effects.size() > b.effects.size();
		});
		for (auto& w : v.worn) {
			for (const auto& c : v.cards) {
				if (c.group == Group::kWorn && (c.source == w.item || (!c.sourceWorn && (c.item == w.enchantment || c.item == w.baseEnchantment)))) {
					w.effects += c.effects.size();
				}
			}
		}

		// ---- stuck: an item's constant enchantment with no worn item behind it ----
		for (std::size_t ci = 0; ci < v.cards.size(); ++ci) {
			const auto& c = v.cards[ci];
			if (c.group != Group::kNotWorn) {
				continue;
			}
			const auto* ef = db ? db->find(c.item) : nullptr;
			if (ef && db->type(*ef) == "ENCH" && ef->castType != 0xFF && ef->castType != 0) {
				continue;  // not a constant effect (a weapon's enchantment works on hits)
			}
			Finding fd;
			fd.kind = Stuck::kNotWorn;
			fd.card = ci;
			for (const auto i : c.effects) {
				const auto& e = v.effects[i];
				// put on by the player itself, never running out (duration 0), not dispelled (the game drops those itself)
				if (e.fromItem() && e.hasRefs && e.caster == kPlayer && e.duration == 0 && !(e.flags & 0x40000u)) {
					fd.effects.push_back(i);
					fd.running += !e.off();
				}
			}
			if (fd.effects.empty()) {
				continue;
			}
			std::string where;
			if (!c.source) {
				where = "no item is named as its source and no worn item carries this enchantment";
			} else if (inventoryItems.contains(c.source)) {
				where = "you carry " + v.name(c.source) + " but do not wear it, and no worn item carries this enchantment";
			} else {
				where = v.name(c.source) + " is not in your inventory any more, and no worn item carries this enchantment";
			}
			fd.text = Fmt("%s (%s, %08X): %zu effect(s) still on you (%zu running): ", v.name(c.item).c_str(), c.plugin.c_str(), c.item,
						  fd.effects.size(), fd.running) +
			          where;
			v.stuck.push_back(std::move(fd));
		}

		// ---- stuck: the source's plugin is gone from the load order ----
		if (!a_options.loadOrder) {
			v.loadOrderNote = "the current load order is not known";
		} else {
			std::size_t in = 0, all = 0;
			for (const auto* list : { &a_save.plugins.full, &a_save.plugins.lite }) {
				for (const auto& p : *list) {
					++all;
					in += a_options.loadOrder->contains(Lower(p));
				}
			}
			if (all && in * 2 < all) {
				v.loadOrderNote = Fmt("the current load order has only %zu of this save's %zu plugins (the save comes from another setup), so gone "
									  "plugins are not reported",
					in, all);
			} else {
				v.loadOrderChecked = true;
				for (std::size_t ci = 0; ci < v.cards.size(); ++ci) {
					const auto& c = v.cards[ci];
					std::string gone;
					for (const auto id : { c.item, c.source }) {
						if (!id) {
							continue;
						}
						const auto p = a_save.pluginOf(id);
						if (p == "Created") {
							continue;
						}
						if (p == "?" || !a_options.loadOrder->contains(Lower(p))) {
							gone = p;
							break;
						}
					}
					if (gone.empty()) {
						continue;
					}
					Finding fd;
					fd.kind = Stuck::kPluginGone;
					fd.card = ci;
					fd.plugin = gone;
					fd.effects = c.effects;
					fd.running = c.running;
					fd.text = Fmt("%s (%08X): %zu effect(s) from %s, which is no longer in your load order", v.name(c.item).c_str(), c.item,
						c.effects.size(), gone == "?" ? "a plugin not in the save's own list" : gone.c_str());
					v.stuck.push_back(std::move(fd));
				}
			}
		}
		return v;
	}

	std::string Remove(SaveFile& a_save, const View& a_view, const std::vector<std::size_t>& a_effects, std::vector<std::string>* a_changes)
	{
		if (!a_view.error.empty()) {
			return a_view.error + ": nothing changed";
		}
		if (a_effects.empty()) {
			return "no effect picked";
		}
		std::set<std::size_t> pick;
		for (const auto i : a_effects) {
			if (i >= a_view.effects.size()) {
				return "no such effect: nothing changed";
			}
			const auto& e = a_view.effects[i];
			if (!e.canRemove) {
				return Fmt("%s (effect %u) cannot be removed: %s. Nothing changed", a_view.name(e.item).c_str(), e.effectIndex, e.why.c_str());
			}
			pick.insert(i);
		}
		if (!a_view.fo4) {
			const auto* pc = SkyrimPlayer(a_save);
			if (!pc) {
				return "this save has no player change form";
			}
			std::string err;
			const auto  before = cf::ReadPlayerActiveEffects(a_save, err).size();
			if (before != a_view.effects.size()) {
				return "the save changed since the list was read: read it again. Nothing changed";
			}
			std::vector<cf::EffectRemoval> rm;
			for (const auto i : pick) {
				cf::EffectRemoval r;
				r.index = a_view.effects[i].index;
				r.actorValue = static_cast<std::int32_t>(a_view.effects[i].actorValue);
				rm.push_back(r);
			}
			const auto backup = *pc;
			if (auto why = cf::RemovePlayerActiveEffects(a_save, rm, a_changes); !why.empty()) {
				return why;
			}
			// read back: the player's record decodes and holds exactly that many fewer effects
			const auto after = cf::ReadPlayerActiveEffects(a_save, err);
			if (!err.empty() || after.size() + rm.size() != before) {
				if (auto* c = a_save.findChangeForm(backup.refID)) {
					*c = backup;
				}
				return "the edited save did not read back as expected: nothing changed";
			}
			return {};
		}
		const auto* pcc = fo4cf::FindPlayer(a_save);
		auto*       pc = pcc ? a_save.findChangeForm(pcc->refID) : nullptr;
		if (!pc) {
			return "the player's saved data is not in this save";
		}
		const auto act = fo4cf::DecodeActor(a_save, *pc);
		if (act.effects.size() != a_view.effects.size()) {
			return "the save changed since the list was read: read it again. Nothing changed";
		}
		std::vector<std::size_t> idx;
		for (const auto i : pick) {
			idx.push_back(a_view.effects[i].index);
		}
		const auto sources = fo4cf::EffectSources(a_save, act, a_view.db);
		if (auto why = fo4cf::RemoveActiveEffects(a_save, *pc, idx, sources, a_changes); !why.empty()) {
			return why;
		}
		const auto after = fo4cf::DecodeActor(a_save, *pc);
		if (!after.complete || after.effects.size() + idx.size() != act.effects.size()) {
			return "the edited save did not read back with the expected effects: reload the save before writing it";
		}
		return {};
	}
}
