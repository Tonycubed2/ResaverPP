// ReSaver++ - Fallout 4 ACHR actor part (after the reference part): the AI process with the active effects, the
// actor's own fields and its actor values, following Actor::LoadGame 0xc8abd0 and AIProcess::LoadGame 0xd058d0
// (RVAs in Fallout4.exe 1.11.240). Every read below is a read the game makes, in its order and with its size;
// each was checked against the game's own loader run on real saves (an emulator trace of every read).
#include "ChangeFormData.h"
#include "ChangeFormFO4.h"

#include <cstring>
#include <stdexcept>

namespace ess::fo4cf
{
	namespace
	{
		struct Stop : std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};

		// the buffer with an optional trace of every read: (position, kind) - R raw, F refid, V vsval, S string
		struct In
		{
			Reader&                                      r;
			std::uint32_t                                ver;
			std::vector<std::pair<std::uint32_t, char>>* trace = nullptr;
			void          mark(char k) { if (trace) trace->push_back({ static_cast<std::uint32_t>(r.pos()), k }); }
			void          raw(std::size_t n) { mark('R'); r.need(n); r.seek(r.pos() + n); }
			std::uint8_t  u8() { mark('R'); return r.u8(); }
			std::uint16_t u16() { mark('R'); return r.u16(); }
			std::uint32_t u32() { mark('R'); return r.u32(); }
			float         f32() { mark('R'); return r.f32(); }
			RefID         ref() { mark('F'); return RefID::read(r); }
			std::uint32_t vs()
			{
				mark('V');
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
			void wstr() { mark('S'); const auto n = r.u16(); r.need(n); r.seek(r.pos() + n); }
			void blob(std::size_t n) { mark('B'); r.need(n); r.seek(r.pos() + n); }  // copied as one block
			void skip(std::size_t n) { r.need(n); r.seek(r.pos() + n); }             // not read by the game here
			[[noreturn]] void fail(const char* a_what) { throw Stop(std::string("actor part: ") + a_what + " not decoded"); }
		};

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
				if (b.u8() != 0xFF) {
					b.fail("a created package's form");
				}
			}
			const auto type = b.u8();
			if (type != 0xFF) {
				if (type != 16) {  // 16: no data (0x7634d0)
					b.fail("package data (procedure trees)");
				}
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
}
