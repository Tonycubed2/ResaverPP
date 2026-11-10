// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "TransplantFO4.h"

#include "ByteIO.h"
#include "ChangeFormData.h"
#include "ChangeFormFO4.h"
#include "Quests.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace ess::transplant::fo4
{
	namespace
	{
		std::string Fmt(const char* f, auto... args)
		{
			char buf[1024];
			std::snprintf(buf, sizeof(buf), f, args...);
			return buf;
		}

		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		constexpr std::uint32_t kSpecialAV = 0x2C2;  // Strength .. Luck: 0x2C2 .. 0x2C8
		constexpr std::uint32_t kCaps = 0x0000000F;
		constexpr std::uint32_t kPlayerRef = 0x14;

		const char* kSpecialNames[7] = { "Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck" };

		// a form a snapshot can hold: from a plugin of the save (not made in the game, not out of range)
		bool Identified(const SaveFile& a_save, std::uint32_t a_id, std::string& a_plugin)
		{
			if (a_id == 0 || (a_id >> 24) == 0xFF) {
				return false;
			}
			a_plugin = a_save.pluginOf(a_id);
			return !a_plugin.empty() && a_plugin != "?" && a_plugin != "Created";
		}

		// the save has a reference for this form (the perk list can then point at it; see fo4cf::AddPerk)
		bool Referenced(const SaveFile& a_save, std::uint32_t a_formID)
		{
			if ((a_formID >> 24) == 0 && a_formID < 0x400000u) {
				return true;
			}
			return std::find(a_save.formIDs.begin(), a_save.formIDs.end(), a_formID) != a_save.formIDs.end();
		}

		ChangeForm* PlayerRef(SaveFile& a_save) { return const_cast<ChangeForm*>(fo4cf::FindPlayer(a_save)); }

		// ---- item stacks: the extra data that is copied ----------------------------------------------------

		constexpr std::uint8_t kExtraHealth = 0x25;   // ExtraHealth: f32
		constexpr std::uint8_t kExtraMods = 0x35;     // BGSObjectInstanceExtra (0x24a240)
		constexpr std::uint8_t kExtraText = 0x99;     // ExtraTextDisplayData (0x2acdf0)

		std::uint32_t Vsval(Reader& r)
		{
			r.need(1);
			const std::uint8_t a = *r.here();
			switch (a & 3) {
			case 0: return r.u8() >> 2;
			case 1: return r.u16() >> 2;
			default:
				{
					const std::uint32_t b0 = r.u8(), b1 = r.u8(), b2 = r.u8();
					return (b0 | (b1 << 8) | (b2 << 16)) >> 2;
				}
			}
		}
		std::string WStr(Reader& r)
		{
			const auto n = r.u16();
			r.need(n);
			std::string t(reinterpret_cast<const char*>(r.here()), n);
			r.seek(r.pos() + n);
			return t;
		}

		// a form id as the old save stores it -> plugin + local id ("" when it cannot be named)
		std::string Name(const SaveFile& a_save, RefID a_ref, FormRef& a_out)
		{
			a_out = {};
			if (a_ref.isZero()) {
				return {};
			}
			const auto id = a_save.formIDOf(a_ref);
			if (!id) {
				return "a form id the save's form id array does not have";
			}
			std::string plugin;
			if (!Identified(a_save, id, plugin)) {
				return (id >> 24) == 0xFF ? Fmt("a form made in the game (%08X)", id) : Fmt("a form of no plugin of the save (%08X)", id);
			}
			a_out.null = false;
			a_out.plugin = plugin;
			a_out.local = LocalOf(id);
			return {};
		}

		// what is copied of one stack (body: the old save's player body; a_stack: as fo4cf decoded it)
		StackData ReadStack(const SaveFile& a_save, const std::vector<std::uint8_t>& a_body, const fo4cf::Stack& a_stack)
		{
			StackData d;
			d.count = a_stack.count;
			auto note = [&](const std::string& a_why) {
				if (!a_why.empty() && d.problem.empty()) {
					d.problem = a_why;
				}
			};
			for (const auto& e : a_stack.extras) {
				if (e.type != kExtraHealth && e.type != kExtraMods && e.type != kExtraText) {
					d.leftBehind.push_back(e.type);
					continue;
				}
				try {
					Reader r(a_body.data() + e.range.begin + 1, e.range.size() - 1);
					if (e.type == kExtraHealth) {
						d.hasHealth = true;
						d.health = r.f32();
					} else if (e.type == kExtraMods) {
						d.hasMods = true;
						d.modTemplate = r.u16();
						const auto n = r.u32();
						for (std::uint32_t i = 0; i < n; ++i) {
							ObjectMod m;
							note(Name(a_save, RefID::read(r), m.mod));
							m.attach = r.u8();
							m.rank = r.u8();
							m.flags = a_save.formVersion >= 0x15 ? r.u8() : 0;
							if (m.mod.null) {
								note("an object mod with no form");
							}
							d.mods.push_back(std::move(m));
						}
					} else {
						d.hasText = true;
						note(Name(a_save, RefID::read(r), d.textMessage));
						note(Name(a_save, RefID::read(r), d.textQuest));
						d.textValue = r.i32();
						d.textFlags = r.u16();
						if (d.textMessage.null && d.textQuest.null && d.textValue == -2) {
							d.hasName = true;
							d.name = WStr(r);
						}
						for (auto n = Vsval(r); n; --n) {
							std::pair<std::string, FormRef> x;
							x.first = WStr(r);
							note(Name(a_save, RefID::read(r), x.second));
							d.textEntries.push_back(std::move(x));
						}
					}
					if (r.pos() != e.range.size() - 1) {
						note(Fmt("extra 0x%02X did not read to its end", e.type));
					}
				} catch (const std::exception& ex) {
					note(Fmt("extra 0x%02X: ", e.type) + ex.what());
				}
			}
			return d;
		}

		std::string KeyOf(const FormRef& f) { return f.null ? std::string("-") : Lower(f.plugin) + Fmt(":%X", f.local); }

		// ---- writing ----------------------------------------------------------------------------------------

		void PutVs(std::vector<std::uint8_t>& o, std::uint32_t v)
		{
			if (v < 0x40) {
				o.push_back(static_cast<std::uint8_t>(v << 2));
			} else if (v < 0x4000) {
				const auto x = static_cast<std::uint16_t>((v << 2) | 1);
				o.push_back(static_cast<std::uint8_t>(x));
				o.push_back(static_cast<std::uint8_t>(x >> 8));
			} else {
				const auto x = (v << 2) | 2;
				o.push_back(static_cast<std::uint8_t>(x));
				o.push_back(static_cast<std::uint8_t>(x >> 8));
				o.push_back(static_cast<std::uint8_t>(x >> 16));
			}
		}
		void Put16(std::vector<std::uint8_t>& o, std::uint16_t v)
		{
			o.push_back(static_cast<std::uint8_t>(v));
			o.push_back(static_cast<std::uint8_t>(v >> 8));
		}
		void Put32(std::vector<std::uint8_t>& o, std::uint32_t v)
		{
			for (int i = 0; i < 4; ++i) {
				o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
			}
		}
		void PutRef(std::vector<std::uint8_t>& o, std::uint32_t a_ref)
		{
			o.push_back(static_cast<std::uint8_t>(a_ref >> 16));
			o.push_back(static_cast<std::uint8_t>(a_ref >> 8));
			o.push_back(static_cast<std::uint8_t>(a_ref));
		}
		void PutWStr(std::vector<std::uint8_t>& o, const std::string& t)
		{
			Put16(o, static_cast<std::uint16_t>(std::min<std::size_t>(t.size(), 0xFFFF)));
			o.insert(o.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(t.size(), 0xFFFF)));
		}

		// form ids written into the good save: plugin + local id -> the save's 3-byte reference. A Fallout4.esm
		// form below 0x400000 is written directly (type bits 01, as the reader 0xc0af70 takes it); any other form
		// takes its index in the form id array, appended when the save has none for it.
		struct Refs
		{
			SaveFile&   save;
			std::string error;
			std::uint32_t operator()(const FormRef& f)
			{
				if (f.null) {
					return 0;
				}
				const auto id = RuntimeID(save.plugins, f.plugin, f.local);
				if (!id) {
					if (error.empty()) {
						error = f.plugin + " is not in this save's load order";
					}
					return 0;
				}
				if ((id >> 24) == 0 && id < 0x400000u) {
					return 0x400000u | id;
				}
				for (std::size_t i = 0; i < save.formIDs.size(); ++i) {
					if (save.formIDs[i] == id) {
						return static_cast<std::uint32_t>(i + 1);
					}
				}
				if (save.formIDs.size() + 1 >= 0x3FFFFF) {
					if (error.empty()) {
						error = "the save's form id array is full";
					}
					return 0;
				}
				save.formIDs.push_back(id);
				return static_cast<std::uint32_t>(save.formIDs.size());
			}
		};

		// one stack as the inventory loader (0x34a310) reads it: extra data list, i32 count, u16 flags (0: not equipped)
		void PutStack(std::vector<std::uint8_t>& o, const StackData& d, Refs& a_refs, std::uint32_t a_ver)
		{
			PutVs(o, static_cast<std::uint32_t>((d.hasHealth ? 1 : 0) + (d.hasMods ? 1 : 0) + (d.hasText ? 1 : 0)));
			if (d.hasHealth) {
				o.push_back(kExtraHealth);
				std::uint32_t v = 0;
				std::memcpy(&v, &d.health, 4);
				Put32(o, v);
			}
			if (d.hasMods) {
				o.push_back(kExtraMods);
				Put16(o, d.modTemplate);
				Put32(o, static_cast<std::uint32_t>(d.mods.size()));
				for (const auto& m : d.mods) {
					PutRef(o, a_refs(m.mod));
					o.push_back(m.attach);
					o.push_back(m.rank);
					if (a_ver >= 0x15) {
						o.push_back(m.flags);
					}
				}
			}
			if (d.hasText) {
				o.push_back(kExtraText);
				PutRef(o, a_refs(d.textMessage));
				PutRef(o, a_refs(d.textQuest));
				Put32(o, static_cast<std::uint32_t>(d.textValue));
				Put16(o, d.textFlags);
				if (d.hasName) {
					PutWStr(o, d.name);
				}
				PutVs(o, static_cast<std::uint32_t>(d.textEntries.size()));
				for (const auto& [t, f] : d.textEntries) {
					PutWStr(o, t);
					PutRef(o, a_refs(f));
				}
			}
			Put32(o, static_cast<std::uint32_t>(d.count));
			Put16(o, 0);
		}

		std::string Describe(const StackData& d)
		{
			std::string s;
			if (d.hasName) {
				s += "\"" + d.name + "\"";
			}
			if (d.hasMods) {
				s += (s.empty() ? "" : ", ") + Fmt("%zu mod(s)", d.mods.size());
			}
			if (d.hasHealth) {
				s += (s.empty() ? "" : ", ") + Fmt("condition %.0f%%", d.health * 100.0f);
			}
			return s;
		}
	}

	std::string StackData::key() const
	{
		std::string k;
		if (hasHealth) {
			std::uint32_t v = 0;
			std::memcpy(&v, &health, 4);
			k += Fmt("H%08X;", v);
		}
		if (hasMods) {
			k += Fmt("M%04X", modTemplate);
			for (const auto& m : mods) {
				k += "," + KeyOf(m.mod) + Fmt("/%u/%u/%u", m.attach, m.rank, m.flags);
			}
			k += ";";
		}
		if (hasText) {
			k += "T" + KeyOf(textMessage) + "|" + KeyOf(textQuest) + Fmt("|%d|%u|", textValue, textFlags) + (hasName ? "N" + name : std::string("-"));
			for (const auto& [t, f] : textEntries) {
				k += "," + t + "=" + KeyOf(f);
			}
			k += ";";
		}
		return k;
	}

	const char* SpecialName(int a_index) { return a_index >= 0 && a_index < 7 ? kSpecialNames[a_index] : "?"; }

	std::uint32_t LocalOf(std::uint32_t a_formID) { return (a_formID >> 24) == 0xFE ? (a_formID & 0xFFF) : (a_formID & 0xFFFFFF); }

	std::uint32_t RuntimeID(const PluginInfo& a_plugins, const std::string& a_plugin, std::uint32_t a_local)
	{
		const auto want = Lower(a_plugin);
		for (std::size_t i = 0; i < a_plugins.full.size() && i < 0xFE; ++i) {
			if (Lower(a_plugins.full[i]) == want) {
				return static_cast<std::uint32_t>(i << 24) | (a_local & 0xFFFFFF);
			}
		}
		for (std::size_t i = 0; i < a_plugins.lite.size() && i < 0x1000; ++i) {
			if (Lower(a_plugins.lite[i]) == want) {
				return 0xFE000000u | static_cast<std::uint32_t>(i << 12) | (a_local & 0xFFF);
			}
		}
		return 0;
	}

	bool SameLoadOrder(const PluginInfo& a_a, const PluginInfo& a_b)
	{
		auto same = [](const std::vector<std::string>& x, const std::vector<std::string>& y) {
			if (x.size() != y.size()) {
				return false;
			}
			for (std::size_t i = 0; i < x.size(); ++i) {
				if (Lower(x[i]) != Lower(y[i])) {
					return false;
				}
			}
			return true;
		};
		return same(a_a.full, a_b.full) && same(a_a.lite, a_b.lite);
	}

	// ---- reading a save -------------------------------------------------------------------------------

	Snapshot FromSave(const SaveFile& a_save, const std::string& a_name)
	{
		Snapshot s;
		s.source = a_name;
		s.player = a_save.header.playerName;
		if (!a_save.isFO4()) {
			s.error = a_name + " is not a Fallout 4 save";
			return s;
		}
		s.level = fo4cf::PlayerLevel(a_save);
		if (s.level < 0) {
			s.level = static_cast<int>(a_save.header.playerLevel);
		}
		s.xp = fo4cf::PlayerXP(a_save);
		if (s.xp < 0) {
			s.missingParts.push_back("experience");
		}

		const auto* player = fo4cf::FindPlayer(a_save);
		if (!player) {
			s.error = a_name + ": no player reference (ACHR 00000014) in the save";
			return s;
		}
		// the inventory (the reference part) and the caps
		try {
			const auto body = cf::Body(*player);
			const auto r = fo4cf::DecodeRefr(a_save, *player, body);
			if (r.decoded) {
				s.itemsKnown = true;
				s.caps = 0;
				for (const auto& it : r.inventory) {
					if (it.formID == kCaps) {
						s.caps += it.count;
						continue;
					}
					std::string plugin;
					if (!Identified(a_save, it.formID, plugin) || it.count <= 0) {
						s.skippedItems += it.count > 0 ? 1 : 0;
						continue;
					}
					auto& x = s.items[FormKey{ Lower(plugin), LocalOf(it.formID) }];
					x.plugin = plugin;
					x.local = LocalOf(it.formID);
					x.formID = it.formID;
					x.count += it.count;
					for (const auto& st : it.stacks) {
						x.modded = x.modded || !st.extras.empty();
						if (st.count > 0) {
							x.stacks.push_back(ReadStack(a_save, body, st));
						}
					}
				}
			} else {
				s.missingParts.push_back("items and caps (" + r.error + ")");
			}
		} catch (const std::exception& e) {
			s.missingParts.push_back(std::string("items and caps (") + e.what() + ")");
		}
		// SPECIAL and perks (the actor part)
		try {
			const auto a = fo4cf::DecodeActor(a_save, *player);
			s.inventoryWritable = s.itemsKnown && a.complete;
			if (a.decoded) {
				for (const auto& v : a.values) {
					if (v.formID >= kSpecialAV && v.formID < kSpecialAV + 7) {
						s.special[v.formID - kSpecialAV] = v.base;
					}
				}
			} else {
				s.missingParts.push_back("S.P.E.C.I.A.L. (" + a.error + ")");
			}
			if (a.tail && a.hasPerks) {
				s.perksKnown = true;
				for (const auto& p : a.perks) {
					std::string plugin;
					if (!Identified(a_save, p.formID, plugin)) {
						continue;
					}
					auto& x = s.perks[FormKey{ Lower(plugin), LocalOf(p.formID) }];
					x.plugin = plugin;
					x.local = LocalOf(p.formID);
					x.formID = p.formID;
					x.rank = std::max(x.rank, static_cast<int>(p.rank));
				}
			} else {
				s.missingParts.push_back(a.decoded ? "perks (" + (a.error.empty() ? std::string("no saved perk list") : a.error) + ")" : "perks");
			}
		} catch (const std::exception& e) {
			s.missingParts.push_back(std::string("S.P.E.C.I.A.L. and perks (") + e.what() + ")");
		}
		// quests
		std::size_t order = 0;
		for (const auto& q : quests::ReadStates(a_save)) {
			std::string plugin;
			if (!q.hasFlags || !Identified(a_save, q.formID, plugin)) {
				continue;
			}
			auto& x = s.quests[FormKey{ Lower(plugin), LocalOf(q.formID) }];
			x.plugin = plugin;
			x.local = LocalOf(q.formID);
			x.formID = q.formID;
			x.running = q.running();
			x.completed = q.completed();
			x.stage = q.highestDoneStage();
			x.journal = !q.logShown.empty() || q.inJournal();
			x.order = order++;
			for (const auto& o : q.objectives) {
				x.objectives.push_back({ static_cast<std::uint32_t>(o.index), static_cast<std::uint32_t>(o.state) });
			}
		}
		s.valid = true;
		return s;
	}

	// ---- comparing ------------------------------------------------------------------------------------

	std::vector<Change> Diff(const Snapshot& a_good, const Snapshot& a_bad, const SaveFile& a_goodSave)
	{
		std::vector<Change> v;
		auto add_ = [](std::vector<Change>& a_v, Kind k, Via via, std::string label, std::string detail, int risk, std::string why) -> Change& {
			Change c;
			c.kind = k;
			c.via = via;
			c.label = std::move(label);
			c.detail = std::move(detail);
			c.risk = risk;
			c.why = std::move(why);
			a_v.push_back(std::move(c));
			return a_v.back();
		};
		auto add = [&](Kind k, Via via, std::string label, std::string detail, int risk, std::string why) -> Change& {
			Change c;
			c.kind = k;
			c.via = via;
			c.label = std::move(label);
			c.detail = std::move(detail);
			c.risk = risk;
			c.why = std::move(why);
			v.push_back(std::move(c));
			return v.back();
		};
		auto form = [&](Change& c, const std::string& a_plugin, std::uint32_t a_local) {
			c.plugin = a_plugin;
			c.local = a_local;
			c.goodFormID = RuntimeID(a_goodSave.plugins, a_plugin, a_local);
			if (!c.goodFormID) {
				c.available = false;
				c.tick = false;
				c.risk = 2;
				c.why = a_plugin + " is not in the good save's load order, so this cannot be brought over.";
			}
		};

		// the character: written into the file
		if (a_bad.level > a_good.level && a_good.level > 0) {
			auto& c = add(Kind::kLevel, Via::kFile, Fmt("Level %d (now %d)", a_bad.level, a_good.level), "", 0,
				"Sets the level in the save (and in the load menu). Perk points are not given: the perks come over on their own.");
			c.value = a_bad.level;
			c.now = a_good.level;
			if (fo4cf::PlayerLevel(a_goodSave) < 0) {
				c.available = c.tick = false;
				c.risk = 2;
				c.why = "The good save still has the game's starting level (no saved level to change): level up once in the game first.";
			}
		}
		if (a_bad.xp > a_good.xp + 0.5 && a_bad.xp >= 0) {
			auto& c = add(Kind::kXP, Via::kFile, Fmt("Experience %.0f (now %.0f)", a_bad.xp, std::max(0.0, a_good.xp)), "", 0, "Sets the experience points in the save.");
			c.value = a_bad.xp;
			c.now = a_good.xp;
			if (a_good.xp < 0) {
				c.available = c.tick = false;
				c.risk = 2;
				c.why = "The good save has no saved experience to change.";
			}
		}
		for (int i = 0; i < 7; ++i) {
			if (a_bad.special[i] >= 0 && a_bad.special[i] > a_good.special[i] + 0.01f) {
				auto& c = add(Kind::kSpecial, Via::kFile, Fmt("%s %.0f (now %.0f)", kSpecialNames[i], a_bad.special[i], std::max(0.0f, a_good.special[i])), "", 0,
					"Sets the base value in the save (bobbleheads and perks the old save had are counted in it).");
				c.index = i;
				c.value = a_bad.special[i];
				c.now = a_good.special[i];
				if (a_good.special[i] < 0) {
					c.available = c.tick = false;
					c.risk = 2;
					c.why = "The good save has no saved value for it to change.";
				}
			}
		}
		if (a_bad.caps > a_good.caps && a_good.caps >= 0) {
			const bool file = a_good.caps > 0;  // SetPlayerCaps rewrites a caps stack the good save has
			auto&      c = add(Kind::kCaps, file ? Via::kFile : Via::kGame, Fmt("%lld caps (now %lld)", static_cast<long long>(a_bad.caps), static_cast<long long>(a_good.caps)), "",
				0, file ? "Sets the caps you carry in the save." : "The good save has no caps to change: the game gives you the difference.");
			c.value = static_cast<double>(a_bad.caps);
			c.now = static_cast<double>(a_good.caps);
			c.count = a_bad.caps - a_good.caps;
			c.plugin = "Fallout4.esm";
			c.local = kCaps;
			c.goodFormID = kCaps;
		}

		// perks: ranks the good save has are raised in the file; new perks in the file when the save has a reference
		// for them, else through the game (AddPerk gives the next rank each time)
		for (const auto& [k, p] : a_bad.perks) {
			const auto g = a_good.perks.find(k);
			const int  now = g == a_good.perks.end() ? 0 : g->second.rank;
			if (p.rank <= now) {
				continue;
			}
			const auto id = RuntimeID(a_goodSave.plugins, p.plugin, p.local);
			const bool file = a_good.perksKnown && id && (now > 0 || Referenced(a_goodSave, id));
			auto&      c = add(Kind::kPerk, file ? Via::kFile : Via::kGame, now ? Fmt("%08X rank %d (now %d)", p.formID, p.rank, now) : Fmt("%08X rank %d", p.formID, p.rank),
				p.plugin, 0,
				file ? (now ? "Raises the perk's rank in the save." : "Adds the perk with its rank to the save.")
					 : "The game adds the perk (the console's AddPerk, once per rank).");
			form(c, p.plugin, p.local);
			c.value = p.rank;
			c.now = now;
			c.count = p.rank - now;
		}

		// items: the stacks the old save has more of (same mods, name and condition: the same thing), into the file when the
		// good save's player can be edited, else (or for stacks that cannot be written) the game adds them plain
		for (const auto& [k, it] : a_bad.items) {
			const auto         g = a_good.items.find(k);
			const std::int64_t more = it.count - (g == a_good.items.end() ? 0 : g->second.count);
			if (more <= 0) {
				continue;  // (not more of it: a stack that only differs, e.g. by condition, is not a second one)
			}
			std::int64_t budget = more;  // never more than the old save has on top
			std::map<std::string, std::int64_t> have;
			if (g != a_good.items.end()) {
				for (const auto& st : g->second.stacks) {
					have[st.key()] += st.count;
				}
			}
			std::vector<StackData> add;       // into the file
			std::int64_t           plain = 0; // through the game
			std::string            problem;
			std::set<std::uint8_t> left;
			for (const auto& st : it.stacks) {
				auto&              h = have[st.key()];
				const std::int64_t extra = std::min<std::int64_t>(budget, std::max<std::int64_t>(0, st.count - h));
				h = std::max<std::int64_t>(0, h - st.count);
				if (extra <= 0) {
					continue;
				}
				budget -= extra;
				if (!a_good.inventoryWritable || !st.problem.empty()) {
					plain += extra;
					if (!st.problem.empty() && problem.empty()) {
						problem = st.problem;
					}
					continue;
				}
				// the same thing twice in the old save (stacks kept apart by what is left behind): one stack
				auto same = std::find_if(add.begin(), add.end(), [&](const StackData& x) { return x.key() == st.key(); });
				if (same != add.end()) {
					same->count += static_cast<std::int32_t>(extra);
				} else {
					add.push_back(st);
					add.back().count = static_cast<std::int32_t>(extra);
				}
				left.insert(st.leftBehind.begin(), st.leftBehind.end());
			}
			if (!add.empty()) {
				std::int64_t n = 0;
				std::string  what;
				for (const auto& st : add) {
					n += st.count;
					if (const auto d = Describe(st); !d.empty()) {
						what += (what.empty() ? "" : "; ") + Fmt("%d x ", st.count) + d;
					}
				}
				std::string why = what.empty() ? "Added to your inventory in the save." : "Added to your inventory in the save as it was: " + what + ".";
				if (left.contains(0x95) || left.contains(0x88)) {
					why += " It belonged to a quest (alias) in the old save: it comes over as a normal item.";
				}
				auto label = Fmt("%08X x%lld", it.formID, static_cast<long long>(n));
				if (!what.empty()) {
					label += "  (" + (what.size() > 160 ? what.substr(0, 157) + "..." : what) + ")";
				}
				auto& c = add_(v, Kind::kItem, Via::kFile, label, it.plugin, 0, why);
				form(c, it.plugin, it.local);
				c.count = n;
				c.stacks = std::move(add);
			}
			if (plain > 0) {
				std::string why = !a_good.inventoryWritable
				                      ? (it.modded ? "The game adds the item(s) (the good save's player cannot be edited here). Weapon and armor mods, names and condition are not copied: you get the item as the game makes it."
				                                   : "The game adds the item(s) to your inventory (the good save's player cannot be edited here).")
				                      : "This stack cannot be written into the save (" + problem + "): the game adds the item plain (no mods, name or condition).";
				auto& c = add_(v, Kind::kItem, Via::kGame, Fmt("%08X x%lld", it.formID, static_cast<long long>(plain)), it.plugin, a_good.inventoryWritable ? 1 : 0, why);
				form(c, it.plugin, it.local);
				c.count = plain;
			}
		}

		// quests: completed in the old save, or further along there
		for (const auto& [k, q] : a_bad.quests) {
			const auto g = a_good.quests.find(k);
			const bool goodDone = g != a_good.quests.end() && g->second.completed;
			const int  goodStage = g != a_good.quests.end() ? g->second.stage : -1;
			if (goodDone || q.stage < 0) {
				continue;
			}
			const bool complete = q.completed;
			if (!complete && !(q.running && q.stage > goodStage)) {
				continue;
			}
			auto& c = add(Kind::kQuest, Via::kGame,
				complete ? Fmt("%08X - complete it (stage %d)", q.formID, q.stage) : goodStage < 0 ? Fmt("%08X - stage %d (not started here)", q.formID, q.stage) : Fmt("%08X - stage %d (now %d)", q.formID, q.stage, goodStage), q.plugin,
				q.journal ? 1 : 2,
				q.journal ? "The game sets the quest to the stage it had (its stage script runs: rewards and follow-up quests happen normally)."
						  : "A background quest (radiant job, scene, mod setup): the Pip-Boy never listed it. Setting its stage can start or end things you "
							"never saw. Leave it unless you know it.");
			form(c, q.plugin, q.local);
			if (c.available && !q.journal) {
				c.risk = 2;
				c.tick = false;
			}
			c.value = q.stage;
			c.now = goodStage;
			c.complete = complete;
			c.journal = q.journal;
			c.order = q.order;
		}

		// quest objectives: shown / completed / failed in the old save, not yet in the good one. Only for quests the
		// Pip-Boy lists that are still running in the old save (a completed quest is handled by its stage and
		// CompleteQuest). Only objectives the good save has dormant or shown are listed: the game's commands never
		// take an objective back from completed or failed (Fallout4.exe 0x5d4bc0 / 0x5d4cb0 / 0x5d4d80).
		std::set<FormKey> staged;
		for (const auto& c : v) {
			if (c.kind == Kind::kQuest && c.available) {
				staged.insert(FormKey{ Lower(c.plugin), c.local });
			}
		}
		for (const auto& [k, q] : a_bad.quests) {
			if (!q.journal || !q.running || q.completed || q.objectives.empty()) {
				continue;
			}
			const auto g = a_good.quests.find(k);
			if (g != a_good.quests.end() && g->second.completed) {
				continue;
			}
			const bool goodRuns = (g != a_good.quests.end() && g->second.running) || staged.contains(k);
			for (const auto& o : q.objectives) {
				if (o.state < 1 || o.state > 5) {
					continue;
				}
				std::uint32_t now = 0;
				if (g != a_good.quests.end()) {
					for (const auto& x : g->second.objectives) {
						if (x.index == o.index) {
							now = x.state;
						}
					}
				}
				if (now > 1 || now == o.state) {
					continue;
				}
				static const char* kState[6] = { "dormant", "shown", "completed (not shown)", "completed", "failed (not shown)", "failed" };
				auto& c = add(Kind::kObjective, Via::kGame, Fmt("%08X - objective %u: %s (now %s)", q.formID, o.index, kState[o.state], kState[now]), q.plugin,
					goodRuns ? 0 : 1,
					goodRuns ? "The game sets the objective as it was in the old save (SetObjectiveDisplayed, then SetObjectiveCompleted or "
					           "SetObjectiveFailed), after the quest's stage: the Pip-Boy and the quest markers follow."
					         : "The quest is not running in the good save and no stage is set for it here: the game only changes "
					           "objectives of running quests, so this does nothing unless the quest runs by then.");
				form(c, q.plugin, q.local);
				c.objective = o.index;
				c.state = o.state;
				c.value = o.state;
				c.now = now;
				c.journal = true;
				c.order = q.order;
			}
		}
		return v;
	}

	// ---- applying -------------------------------------------------------------------------------------

	std::string AddItemStacks(SaveFile& a_save, const std::string& a_plugin, std::uint32_t a_local, const std::vector<StackData>& a_stacks)
	{
		if (!a_save.isFO4()) {
			return "not a Fallout 4 save";
		}
		if (a_stacks.empty()) {
			return {};
		}
		std::int64_t want = 0;
		for (const auto& st : a_stacks) {
			if (!st.problem.empty()) {
				return st.problem;
			}
			if (st.count <= 0) {
				return "a stack with no items";
			}
			want += st.count;
		}
		auto* p = PlayerRef(a_save);
		if (!p) {
			return "no player reference (ACHR 00000014) in the save";
		}
		std::vector<std::uint8_t> body;
		const auto d = cf::TryBody(*p, body) ? fo4cf::DecodeRefr(a_save, *p, body) : fo4cf::DecodeRefr(a_save, *p);
		if (!d.decoded) {
			return "the player's inventory cannot be read: " + d.error;
		}
		const auto act = fo4cf::DecodeActor(a_save, *p);
		if (!act.complete) {
			return "the player's saved data is not fully read (" + act.error + "): nothing changed";
		}
		const fo4cf::Part* inv = nullptr;
		for (const auto& part : d.parts) {
			if (std::string(part.name) == "inventory") {
				inv = &part;
			}
		}
		if (!inv || inv->range.size() < 4) {
			return "the player has no saved inventory: nothing changed";
		}
		const auto itemID = RuntimeID(a_save.plugins, a_plugin, a_local);
		if (!itemID) {
			return a_plugin + " is not in this save's load order";
		}
		const auto savedIDs = a_save.formIDs;
		Refs       refs{ a_save, {} };
		const auto itemRef = refs(FormRef{ false, a_plugin, a_local });
		std::vector<std::uint8_t> stacks;
		for (const auto& st : a_stacks) {
			PutStack(stacks, st, refs, a_save.formVersion);
		}
		if (!refs.error.empty()) {
			a_save.formIDs = savedIDs;
			return refs.error;
		}
		auto at = [&](std::size_t a_pos) {
			std::uint32_t v = 0;
			std::memcpy(&v, &body[a_pos], 4);
			return v;
		};
		auto nb = body;
		const fo4cf::Item* had = nullptr;
		std::int64_t       before = 0;
		for (const auto& it : d.inventory) {
			if (it.formID == itemID) {
				had = &it;  // the last entry of the item: the stacks go after its last stack
				before += it.count;
			}
		}
		if (had) {
			// more stacks for the entry the player has: after its last stack, its u32 stack count raised
			nb.insert(nb.begin() + static_cast<std::ptrdiff_t>(had->range.end), stacks.begin(), stacks.end());
			const std::uint32_t n = at(had->range.begin + 3) + static_cast<std::uint32_t>(a_stacks.size());
			std::memcpy(&nb[had->range.begin + 3], &n, 4);
		} else {
			// a new entry after the last one: form id, u32 stack count, the stacks; the u32 item count raised
			std::vector<std::uint8_t> entry;
			PutRef(entry, itemRef);
			Put32(entry, static_cast<std::uint32_t>(a_stacks.size()));
			entry.insert(entry.end(), stacks.begin(), stacks.end());
			nb.insert(nb.begin() + static_cast<std::ptrdiff_t>(inv->range.end), entry.begin(), entry.end());
			const std::uint32_t n = at(inv->range.begin) + 1;
			std::memcpy(&nb[inv->range.begin], &n, 4);
		}
		// read back: the reference part with the stacks, the actor part unchanged and complete
		ChangeForm copy = *p;
		cf::SetBody(copy, nb);
		const auto again = fo4cf::DecodeRefr(a_save, copy);
		const auto act2 = fo4cf::DecodeActor(a_save, copy);
		bool       ok = again.decoded && act2.complete && again.inventory.size() == d.inventory.size() + (had ? 0 : 1) &&
		          nb.size() - again.refrPart.end == body.size() - d.refrPart.end &&
		          std::equal(body.begin() + static_cast<std::ptrdiff_t>(d.refrPart.end), body.end(), nb.begin() + static_cast<std::ptrdiff_t>(again.refrPart.end));
		std::int64_t after = 0;
		std::vector<StackData> found;
		for (const auto& it : again.inventory) {
			if (it.formID == itemID) {
				after += it.count;
				for (const auto& st : it.stacks) {
					found.push_back(ReadStack(a_save, nb, st));
				}
			}
		}
		ok = ok && after == before + want && found.size() >= a_stacks.size();
		// each stack written reads back as the same thing (its key), with its count
		for (std::size_t i = 0; ok && i < a_stacks.size(); ++i) {
			const auto& w = a_stacks[i];
			const auto& f = found[found.size() - a_stacks.size() + i];
			ok = f.key() == w.key() && f.count == w.count && f.problem.empty();
		}
		if (!ok) {
			a_save.formIDs = savedIDs;
			return "the player's inventory did not read back as written (" + (again.error.empty() ? act2.error : again.error) + "): nothing changed";
		}
		*p = std::move(copy);
		return {};
	}

	std::string ApplyToFile(SaveFile& a_save, const Change& a_change)
	{
		if (a_change.via != Via::kFile) {
			return "this one goes into the running game";
		}
		switch (a_change.kind) {
		case Kind::kLevel:
			return fo4cf::SetPlayerLevel(a_save, static_cast<int>(a_change.value));
		case Kind::kXP:
			return fo4cf::SetPlayerXP(a_save, static_cast<float>(a_change.value));
		case Kind::kSpecial:
			return fo4cf::SetPlayerSpecial(a_save, a_change.index, static_cast<float>(a_change.value));
		case Kind::kCaps:
			return fo4cf::SetPlayerCaps(a_save, static_cast<std::int32_t>(std::min<double>(a_change.value, 2147483647.0)));
		case Kind::kItem:
			return AddItemStacks(a_save, a_change.plugin, a_change.local, a_change.stacks);
		case Kind::kPerk:
			{
				auto* p = PlayerRef(a_save);
				if (!p) {
					return "no player reference in the save";
				}
				const auto id = RuntimeID(a_save.plugins, a_change.plugin, a_change.local);
				if (!id) {
					return a_change.plugin + " is not in this save's load order";
				}
				const auto rank = static_cast<std::uint8_t>(std::clamp(a_change.value, 0.0, 255.0));
				return a_change.now > 0 ? fo4cf::SetPerkRank(a_save, *p, id, rank) : fo4cf::AddPerk(a_save, *p, id, rank);
			}
		default:
			return "this one goes into the running game";
		}
	}

	std::vector<Record> GameRecords(const Change& a_change, std::uint32_t a_formID, bool a_markCompleted)
	{
		std::vector<Record> out;
		if (a_change.via != Via::kGame || !a_formID) {
			return out;
		}
		const auto id = Fmt("%08X", a_formID);
		switch (a_change.kind) {
		case Kind::kCaps:
		case Kind::kItem:
			if (a_change.count > 0) {
				out.push_back({ "additem", id, std::to_string(std::min<std::int64_t>(a_change.count, 1000000)) });
			}
			break;
		case Kind::kPerk:
			if (a_change.count > 0) {
				out.push_back({ "addperk", id, std::to_string(std::min<std::int64_t>(a_change.count, 100)) });
			}
			break;
		case Kind::kQuest:
			if (a_change.value > a_change.now) {  // (a stage the good save has done already does not run again)
				out.push_back({ "setstage", id, std::to_string(static_cast<int>(a_change.value)) });
			}
			if (a_change.complete && a_markCompleted) {
				out.push_back({ "completequest", id });
			}
			break;
		case Kind::kObjective:
			if (a_change.state >= 1 && a_change.state <= 5 && a_change.objective <= 0xFFFF) {
				out.push_back({ "objective", id, std::to_string(a_change.objective), std::to_string(a_change.state) });
			}
			break;
		default:
			break;
		}
		return out;
	}
}
