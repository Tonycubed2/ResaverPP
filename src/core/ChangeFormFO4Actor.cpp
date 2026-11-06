// ReSaver++ - Fallout 4 ACHR actor part (after the reference part): the AI process with the active effects, the
// actor's own fields and its actor values, following Actor::LoadGame 0xc8abd0 and AIProcess::LoadGame 0xd058d0
// (RVAs in Fallout4.exe 1.11.240). Every read below is a read the game makes, in its order and with its size;
// each was checked against the game's own loader run on real saves (an emulator trace of every read).
#include "ChangeFormData.h"
#include "ChangeFormFO4.h"
#include "ChangeFormFO4In.h"

#include <cstring>
#include <stdexcept>

namespace ess::fo4cf
{
	namespace
	{
		using detail::In;

		void R4n(In& b, int n) { for (int i = 0; i < n; ++i) b.u32(); }
		void Pair4(In& b) { b.u32(); b.u32(); }       // 0xd4c740
		void One4(In& b) { b.u32(); }                  // 0xd4c830

		// 0xd4ebb0: u32 n x (AVIF ref, f32, u8)
		void AvList(In& b)
		{
			for (auto n = b.u32(); n; --n) { b.ref(); b.u32(); b.u8(); }
		}

		// 0xcea430: a package instance
		void Package(In& b)
		{
			const auto at = b.r.pos();
			const auto pkg = b.ref();
			(void)at;
			if (pkg.isZero()) {
				return;
			}
			if (pkg.type() == RefID::Type::kCreated) {  // 0x2ecb40: a created package saves its own form
				const auto t = b.u8();
				if (t != 0xFF) {
					detail::CreatedPackage(b, t);  // ChangeFormFO4Pkg.cpp
				}
			}
			const auto type = b.u8();
			if (type != 0xFF) {
				detail::PackageData(b, type);  // ChangeFormFO4Pkg.cpp
			}
			b.u32(); b.u32(); b.u32(); b.u16(); b.u8(); b.u8();
			b.ref();
		}

		// 0xb896c0 / 0xb753d0: the active effects
		void Effects(In& b, std::vector<ActiveEffect>* a_out)
		{
			for (auto n = b.vs(); n; --n) {
				ActiveEffect e;
				e.range.begin = b.r.pos();
				e.item = b.ref();
				e.effectIndex = b.u8();
				e.archetype = b.vs();
				const auto size = b.vs();
				e.data.begin = b.r.pos();
				b.skip(size);  // ActiveEffect::LoadGame (0xb74e90) or a skip of the same size
				e.data.end = e.range.end = b.r.pos();
				if (a_out) {
					a_out->push_back(e);
				}
			}
		}

		// 0xd48350: n x (ref, u32, u8, 0xce5c70: u32, ref, ref, 5 x u8)
		void Combat(In& b, std::uint32_t n)
		{
			for (; n; --n) {
				b.ref(); b.u32(); b.u8();
				b.u32(); b.ref(); b.ref();
				for (int i = 0; i < 5; ++i) b.u8();
			}
		}

		// 0xd2faa0
		void Knowledge(In& b)
		{
			b.ref(); b.u32();  // 0x436f00
			b.ref(); b.u32();
			const auto tag = b.u32();
			if (tag == 0x6241C761u) {  // 0xd30c20
				b.ref(); b.u32(); b.u32();
			} else if (tag != 0) {
				b.fail("a knowledge entry");
			}
		}

		// 0xbae9f0
		void Dialogue(In& b)
		{
			b.wstr(); b.wstr(); b.u8(); b.ref(); b.ref(); b.ref(); b.u8(); b.u16(); b.ref(); b.u8();
		}

		// 0xdf04a0
		void Path(In& b)
		{
			b.ref(); b.ref(); b.u32(); One4(b);
			if (b.u8()) {  // 0xdf3560
				b.u16(); b.u16();  // 0xdf1380
				for (int i = 0; i < 4; ++i) b.u8();
				for (int i = 0; i < 4; ++i) b.raw(12);
				One4(b); One4(b); One4(b);
			}
		}

		// AIProcess high data (+0x10)
		void High(In& b)
		{
			const auto ver = b.ver;
			b.u8(); b.u8(); b.u8();
			b.u16();
			R4n(b, 9);
			b.u8();
			b.raw(12); b.raw(12);
			R4n(b, 5);
			b.u8(); b.u32(); b.u8(); b.u32(); b.u8(); b.u32(); b.u32(); b.u8();
			R4n(b, 4); b.u8(); b.u8(); b.u32(); b.u32(); b.u8(); b.u8(); b.u32(); b.u32(); b.u8();
			R4n(b, 4); b.u8(); b.u8(); b.u8(); b.u32(); b.u8(); b.u8(); b.u32(); b.u8(); b.u8();
			for (int i = 0; i < 7; ++i) b.ref();
			for (int i = 0; i < 6; ++i) { b.ref(); b.u8(); }
			if (b.u8()) {  // 0xbae160
				for (auto n = b.vs(); n; --n) Dialogue(b);
			}
			for (auto n = b.vs(); n; --n) { b.u32(); b.ref(); }
			if (b.u8()) { b.u32(); b.raw(12); One4(b); b.ref(); }
			{  // 0xbedaf0 -> 0xc10b00: u8, vsval size, a block of that size
				b.u8();
				const auto n = b.vs();
				if (n) b.blob(n);
			}
			b.u8();
			for (auto n = b.vs(); n; --n) { b.u32(); b.u32(); b.ref(); b.ref(); b.ref(); }
			for (auto n = b.vs(); n; --n) { b.ref(); Path(b); }
			b.u32(); b.ref();
			R4n(b, 8);
			b.u8(); b.u32(); b.u32(); One4(b); One4(b);
			b.ref(); b.ref();
			b.wstr();
			if (b.u8()) {
				b.u8();
				b.wstr();  // 0xe11e20
				for (auto n = b.u32(); n; --n) b.wstr();
				for (auto n = b.u32(); n; --n) {  // 0xe125a0
					b.wstr();
					for (auto m = b.u32(); m; --m) b.wstr();
					b.u8();
				}
				if (ver >= 0x27) {
					for (auto n = b.u32(); n; --n) {
						b.u32(); b.wstr();
						for (auto m = b.u32(); m; --m) b.wstr();
						b.u8();
					}
				}
			}
			for (int i = 0; i < 6; ++i) b.u8();
			b.u32(); b.u8(); b.u8(); b.u32();
			b.ref(); b.ref(); Pair4(b);
			b.u8(); b.u8(); b.ref(); b.u8(); b.u8(); b.u32(); One4(b);
			b.u8(); b.u8(); Pair4(b); One4(b);
			b.u8(); b.u32();
			b.raw(12); b.raw(12); b.raw(12); b.raw(12);
			b.ref(); b.ref();
			b.u8(); b.ref(); b.u32(); b.u8(); b.u32(); b.u8(); b.u32(); b.u16(); b.u8(); b.u8();
			b.ref();
			b.u8(); b.u8();
			if (ver >= 3) b.u8();
			if (ver >= 0x30) b.u8();
		}

		void Process(In& b, std::uint32_t a_level, Actor& a)
		{
			const auto ver = b.ver;
			b.u32(); b.u32();
			if (b.u8()) {  // 0xd4e770
				R4n(b, 12);
				AvList(b);
				AvList(b);
			}
			Package(b);  // 0xcea430 (+0x18)
			b.u8(); b.u32();
			b.ref();
			b.u32(); b.u32(); b.u32(); b.u8(); b.u16();
			Pair4(b);
			b.ref();
			for (auto n = b.vs(); n; --n) b.ref();
			b.u8(); b.ref(); b.u8();
			if (a_level > 2) {
				return;  // no middle-low process
			}
			b.u32();
			if (a_level > 1) {
				return;  // no middle-high process
			}
			// middle-high (+0x08)
			b.u32(); b.u32(); b.u32(); b.u8();
			b.raw(12); b.u32(); b.u32();
			b.u8(); b.u8(); b.u8();
			b.u32(); b.u8(); b.u16(); b.u32(); b.u8();
			b.u32(); b.u32(); b.u8(); b.u8(); b.u8();
			b.ref(); b.ref(); b.ref(); b.ref(); b.ref();
			b.raw(12); b.raw(12); b.u32(); b.u32();
			b.u8(); b.u8(); b.u8(); b.u32();
			for (auto n = b.vs(); n; --n) b.ref();
			Package(b);  // +0x60
			Effects(b, &a.effects);
			a.effectsRange.end = b.r.pos();
			b.ref();
			Combat(b, b.vs());
			b.u32(); b.u32(); b.u32();
			b.ref();
			b.u32(); b.u32(); b.u8(); b.u32();
			for (int i = 0; i < 7; ++i) b.u8();
			b.u32();
			b.ref();
			b.u8(); b.u8();
			if (ver >= 0x21) b.u8();
			b.u8();
			for (auto n = b.vs(); n; --n) Knowledge(b);
			b.raw(12); b.raw(12); b.u32();
			if (ver >= 0x11) b.u32();
			b.u8();
			if (ver >= 0x0e) One4(b);
			if (a_level == 0) {
				High(b);
			}
			b.u8(); b.u8(); b.raw(12);
		}

		// 0xb85b10 + 0xb79840: an actor magic caster
		void Caster(In& b)
		{
			b.u32(); b.ref(); b.u32(); b.u32(); b.ref(); b.u32();
			b.u32(); b.ref(); b.u32();
			b.u8();  // 0xc10700
			const auto n = b.vs();
			if (n) b.blob(n);
		}

		// 0xced2e0: u32 n, then min(n, 3) u32
		void Three(In& b)
		{
			const auto n = b.u32();
			for (std::uint32_t i = 0; i < n && i < 3; ++i) b.u32();
		}
	}

	Actor DecodeActor(const SaveFile& a_save, const ChangeForm& a_cf, std::vector<std::pair<std::uint32_t, char>>* a_trace)
	{
		Actor out;
		const auto body = cf::Body(a_cf);
		const auto refr = DecodeRefr(a_save, a_cf, body);
		if (!refr.actor || !refr.decoded) {
			out.error = refr.actor ? "the reference part: " + refr.error : "not an ACHR";
			return out;
		}
		out.formID = a_save.formIDOf(a_cf.refID);
		Reader r(body.data(), body.size());
		In     b{ r, a_save.formVersion, a_trace };
		try {
			// the 8-byte prefix read by Actor::LoadGame before TESObjectREFR::LoadGame
			std::size_t prefix = 0;
			for (const auto& p : refr.parts) {
				if (std::string(p.name) == "actor prefix") prefix = p.range.begin;
			}
			r.seek(prefix);
			out.processLevel = static_cast<std::int32_t>(b.u32());
			b.u32();
			r.seek(refr.refrPart.end);
			out.processRange.begin = r.pos();
			out.effectsRange.begin = 0;
			if (out.processLevel != -1) {
				Process(b, static_cast<std::uint32_t>(out.processLevel), out);
			}
			out.processRange.end = r.pos();
			// Actor::LoadGame after the process
			b.u8();
			for (int i = 0; i < 9; ++i) b.u32();
			b.raw(12);
			b.ref();
			b.u16();  // 0xceb7c0
			for (int i = 0; i < 19; ++i) b.u8();
			if (b.ver >= 0x3c) b.u8();
			const auto casters = b.u8();
			for (int i = 0; i < 4; ++i) {
				if (casters & (1u << i)) Caster(b);
			}
			for (int i = 0; i < 5; ++i) b.ref();
			for (int i = 0; i < 4; ++i) Three(b);
			// 0xcedc80: the actor values
			out.valuesRange.begin = r.pos();
			for (auto n = b.vs(); n; --n) {
				ActorValue v;
				v.range.begin = r.pos();
				v.av = b.ref();
				v.formID = a_save.formIDOf(v.av);
				v.baseRange.begin = r.pos();
				v.base = b.f32();
				v.baseRange.end = v.range.end = r.pos();
				out.values.push_back(v);
			}
			for (auto n = b.vs(); n; --n) {
				ActorValueMod m;
				m.range.begin = r.pos();
				m.av = b.ref();
				m.formID = a_save.formIDOf(m.av);
				for (auto& x : m.mod) x = b.f32();
				m.range.end = r.pos();
				out.modifiers.push_back(m);
			}
			out.valuesRange.end = r.pos();
			out.decoded = true;
			out.stoppedAt = r.pos();
			// Actor::LoadGame from 0xc8b0d2
			for (auto n = b.u32(); n; --n) b.ref();
			out.hasController = b.u8() != 0;  // 0xc8b22c
			out.controllerRange.begin = r.pos();
			if (out.hasController) {
				detail::MovementController(b);
			}
			out.controllerRange.end = r.pos();
			b.u32();
			if (b.u8()) {  // 0xc8b31c: 0x87c540, set in none of the saves seen
				b.fail("the 0x87c540 block");
			}
			for (auto n = b.u32(); n; --n) {  // 0xcdf2d0
				b.ref();
				b.f32();
			}
			b.u32();
			b.u32();
			b.ref();
			b.u8();
			b.u32();
			out.hasPerks = b.u8() != 0;  // 0xc8b484
			out.perksRange.begin = r.pos();
			if (out.hasPerks) {  // 0x986b40
				for (auto n = b.vs(); n; --n) {
					Perk p;
					p.range.begin = r.pos();
					p.perk = b.ref();
					p.formID = a_save.formIDOf(p.perk);
					p.rankRange.begin = r.pos();
					p.rank = b.u8();
					p.rankRange.end = p.range.end = r.pos();
					out.perks.push_back(p);
				}
				out.perkList = { out.perksRange.begin, r.pos() };
				for (auto n = b.vs(); n; --n) {
					PerkRef p;
					p.range.begin = r.pos();
					p.perk = b.ref();
					p.formID = a_save.formIDOf(p.perk);
					p.range.end = r.pos();
					out.perkRefs.push_back(p);
				}
				for (auto n = b.vs(); n; --n) {
					PerkValue p;
					p.range.begin = r.pos();
					p.perk = b.ref();
					p.formID = a_save.formIDOf(p.perk);
					p.value = b.u32();
					p.range.end = r.pos();
					out.perkValues.push_back(p);
				}
			}
			out.perksRange.end = r.pos();
			b.ref();  // 0xc8b4fb
			out.tailEnd = r.pos();
			out.tail = true;
			out.stoppedAt = r.pos();
		} catch (const std::exception& e) {
			out.error = e.what();
			out.stoppedAt = r.pos();
		}
		return out;
	}

	std::string SetActorValueBase(SaveFile& a_save, ChangeForm& a_cf, std::uint32_t a_avif, float a_value)
	{
		const auto d = DecodeActor(a_save, a_cf);
		if (!d.decoded) {
			return "the actor's values cannot be read: " + d.error;
		}
		const ActorValue* v = nullptr;
		for (const auto& x : d.values) {
			if (x.formID == a_avif) v = &x;
		}
		if (!v) {
			return "the actor has no saved base value for this actor value (adding one is not supported)";
		}
		auto body = cf::Body(a_cf);
		std::memcpy(body.data() + v->baseRange.begin, &a_value, 4);
		ChangeForm copy = a_cf;
		cf::SetBody(copy, body);
		const auto again = DecodeActor(a_save, copy);
		bool ok = again.decoded && again.values.size() == d.values.size();
		if (ok) {
			ok = false;
			for (const auto& x : again.values) {
				if (x.formID == a_avif && x.base == a_value) ok = true;
			}
		}
		if (!ok) {
			return "the re-encoded actor does not read back";
		}
		a_cf = std::move(copy);
		return {};
	}

	std::string SetPlayerSpecial(SaveFile& a_save, int a_index, float a_value)
	{
		if (a_index < 0 || a_index > 6) {
			return "SPECIAL index must be 0 (Strength) to 6 (Luck)";
		}
		auto* p = const_cast<ChangeForm*>(FindPlayer(a_save));
		if (!p) {
			return "no player reference (ACHR 0x14) in this save";
		}
		return SetActorValueBase(a_save, *p, 0x2C2u + static_cast<std::uint32_t>(a_index), a_value);
	}

	namespace
	{
		void PutVsval(std::vector<std::uint8_t>& a_out, std::uint32_t a_v)
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

		// the save's reference for a form id: a Fallout4.esm form below 0x400000 directly, else its index in the
		// form id array (0 when the save has no reference for it)
		std::uint32_t RefFor(const SaveFile& a_save, std::uint32_t a_formID)
		{
			if ((a_formID >> 24) == 0 && a_formID < 0x400000u) {
				return 0x400000u | a_formID;
			}
			for (std::size_t i = 0; i < a_save.formIDs.size() && i + 1 < 0x400000u; ++i) {
				if (a_save.formIDs[i] == a_formID) return static_cast<std::uint32_t>(i + 1);
			}
			return 0;
		}

		std::string Perks(const SaveFile& a_save, const ChangeForm& a_cf, Actor& a_out)
		{
			a_out = DecodeActor(a_save, a_cf);
			if (!a_out.tail) {
				return "the actor's perks cannot be read: " + a_out.error;
			}
			if (!a_out.hasPerks) {
				return "the actor has no saved perk lists";
			}
			return {};
		}

		// rebuilds the first perk list from a_perks and checks that the body reads back with exactly that list
		std::string WritePerks(const SaveFile& a_save, ChangeForm& a_cf, const Actor& a_d, const std::vector<Perk>& a_perks)
		{
			const auto body = cf::Body(a_cf);
			std::vector<std::uint8_t> out(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(a_d.perkList.begin));
			PutVsval(out, static_cast<std::uint32_t>(a_perks.size()));
			for (const auto& p : a_perks) {
				out.push_back(static_cast<std::uint8_t>(p.perk.data >> 16));
				out.push_back(static_cast<std::uint8_t>(p.perk.data >> 8));
				out.push_back(static_cast<std::uint8_t>(p.perk.data));
				out.push_back(p.rank);
			}
			out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(a_d.perkList.end), body.end());
			ChangeForm copy = a_cf;
			cf::SetBody(copy, out);
			const auto again = DecodeActor(a_save, copy);
			bool ok = again.tail && again.perks.size() == a_perks.size() && again.perkRefs.size() == a_d.perkRefs.size() &&
			          again.perkValues.size() == a_d.perkValues.size() &&
			          static_cast<std::ptrdiff_t>(again.tailEnd) - static_cast<std::ptrdiff_t>(a_d.tailEnd) ==
			              static_cast<std::ptrdiff_t>(out.size()) - static_cast<std::ptrdiff_t>(body.size());
			for (std::size_t i = 0; ok && i < a_perks.size(); ++i) {
				ok = again.perks[i].perk == a_perks[i].perk && again.perks[i].rank == a_perks[i].rank;
			}
			if (!ok) {
				return "the re-encoded actor does not read back";
			}
			a_cf = std::move(copy);
			return {};
		}
	}

	std::string SetPerkRank(SaveFile& a_save, ChangeForm& a_cf, std::uint32_t a_perkFormID, std::uint8_t a_rank)
	{
		Actor d;
		if (auto e = Perks(a_save, a_cf, d); !e.empty()) return e;
		const Perk* p = nullptr;
		for (const auto& x : d.perks) {
			if (x.formID == a_perkFormID) p = &x;
		}
		if (!p) {
			return "the actor does not have this perk";
		}
		auto body = cf::Body(a_cf);
		body[p->rankRange.begin] = a_rank;
		ChangeForm copy = a_cf;
		cf::SetBody(copy, body);
		const auto again = DecodeActor(a_save, copy);
		bool ok = again.tail && again.perks.size() == d.perks.size() && again.tailEnd == d.tailEnd;
		if (ok) {
			ok = false;
			for (const auto& x : again.perks) {
				if (x.formID == a_perkFormID && x.rank == a_rank) ok = true;
			}
		}
		if (!ok) {
			return "the re-encoded actor does not read back";
		}
		a_cf = std::move(copy);
		return {};
	}

	std::string AddPerk(SaveFile& a_save, ChangeForm& a_cf, std::uint32_t a_perkFormID, std::uint8_t a_rank)
	{
		Actor d;
		if (auto e = Perks(a_save, a_cf, d); !e.empty()) return e;
		for (const auto& x : d.perks) {
			if (x.formID == a_perkFormID) return SetPerkRank(a_save, a_cf, a_perkFormID, a_rank);
		}
		Perk n;
		n.perk.data = RefFor(a_save, a_perkFormID);
		if (!n.perk.data || a_save.formIDOf(n.perk) != a_perkFormID) {
			return "the save has no reference for this form id (adding one to the form id array is not supported)";
		}
		n.formID = a_perkFormID;
		n.rank = a_rank;
		auto perks = d.perks;
		perks.push_back(n);
		return WritePerks(a_save, a_cf, d, perks);
	}

	std::string RemovePerk(SaveFile& a_save, ChangeForm& a_cf, std::uint32_t a_perkFormID)
	{
		Actor d;
		if (auto e = Perks(a_save, a_cf, d); !e.empty()) return e;
		std::vector<Perk> perks;
		for (const auto& x : d.perks) {
			if (x.formID != a_perkFormID) perks.push_back(x);
		}
		if (perks.size() == d.perks.size()) {
			return "the actor does not have this perk";
		}
		return WritePerks(a_save, a_cf, d, perks);
	}

	namespace
	{
		ChangeForm* PlayerNpc(SaveFile& a_save, Range* a_acbs)
		{
			for (auto& c : a_save.changeForms) {
				if (c.typeCode() != 9 || a_save.formIDOf(c.refID) != 7) continue;
				const auto n = DecodeNpc(a_save, c);
				for (const auto& p : n.parts) {
					if (std::string(p.name) == "ACBS" && p.range.size() == 20) {
						*a_acbs = p.range;
						return &c;
					}
				}
				return nullptr;
			}
			return nullptr;
		}
	}

	int PlayerLevel(const SaveFile& a_save)
	{
		Range r;
		auto* c = PlayerNpc(const_cast<SaveFile&>(a_save), &r);
		if (!c) return -1;
		const auto body = cf::Body(*c);
		std::int16_t v;
		std::memcpy(&v, body.data() + r.begin + 6, 2);
		return v;
	}

	std::string SetPlayerLevel(SaveFile& a_save, int a_level)
	{
		if (a_level < 1 || a_level > 32767) {
			return "the level must be 1 to 32767";
		}
		Range r;
		auto* c = PlayerNpc(a_save, &r);
		if (!c) {
			return "the save has no changed ACBS block for the player (form 0x7, change flag 1): the level is the plugin's";
		}
		auto body = cf::Body(*c);
		const auto v = static_cast<std::int16_t>(a_level);
		std::memcpy(body.data() + r.begin + 6, &v, 2);
		ChangeForm copy = *c;
		cf::SetBody(copy, body);
		const auto n = DecodeNpc(a_save, copy);
		const bool wasComplete = DecodeNpc(a_save, *c).complete;
		if (n.complete != wasComplete) {
			return "the re-encoded NPC_ does not read back";
		}
		*c = std::move(copy);
		if (PlayerLevel(a_save) != a_level) {
			return "the re-encoded NPC_ does not read back";
		}
		a_save.header.playerLevel = static_cast<std::uint32_t>(a_level);
		return {};
	}

	double PlayerXP(const SaveFile& a_save)
	{
		const auto* p = FindPlayer(a_save);
		if (!p) return -1;
		const auto a = DecodeActor(a_save, *p);
		if (!a.decoded) return -1;
		for (const auto& v : a.values) {
			if (v.formID == 0x2C9) return v.base;
		}
		return -1;  // no saved base
	}

	std::string SetPlayerXP(SaveFile& a_save, float a_xp)
	{
		if (!(a_xp >= 0)) {
			return "experience must be 0 or more";
		}
		auto* p = const_cast<ChangeForm*>(FindPlayer(a_save));
		if (!p) {
			return "no player reference (ACHR 0x14) in this save";
		}
		return SetActorValueBase(a_save, *p, 0x2C9, a_xp);
	}
}
