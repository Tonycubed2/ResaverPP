// ReSaver++ - Starfield: the data of a placed reference's change form (REFR family and ACHR), read up to its end.
//
// Every layout below was read from Starfield.exe 1.16.244's writers (addresses are that version's) and checked on 24
// saves from two players (1,922,510 reference change forms of version 153 land on their last byte, with the reference
// list count matching). Only version 153 records are laid out like this: older versions are the saved bytes of a form
// the game has not loaded since, converted on load (FORMAT.md, "Change form data").
//
// Form IDs (0x141868de0) and strings (0x141868f70) are not in the data: they go to the change form's reference list and
// string list, in writing order. The decoder counts the references so that a part of the data (the inventory) can be
// matched to its part of the list.
#include "Save.h"

#include <cstring>
#include <stdexcept>

namespace ess
{
	namespace
	{
		struct Short : std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};

		struct Rd
		{
			const std::vector<std::uint8_t>& d;
			std::size_t                      p = 0;
			std::size_t                      refs = 0;
			std::string*                     why = nullptr;

			void need(std::size_t n)
			{
				if (p + n > d.size()) {
					throw Short("the data ends early");
				}
			}
			std::uint8_t u8()
			{
				need(1);
				return d[p++];
			}
			std::uint16_t u16()
			{
				need(2);
				const auto v = static_cast<std::uint16_t>(d[p] | (d[p + 1] << 8));
				p += 2;
				return v;
			}
			std::uint32_t u32()
			{
				need(4);
				std::uint32_t v;
				std::memcpy(&v, d.data() + p, 4);
				p += 4;
				return v;
			}
			std::uint64_t u64()
			{
				need(8);
				std::uint64_t v;
				std::memcpy(&v, d.data() + p, 8);
				p += 8;
				return v;
			}
			void skip(std::size_t n)
			{
				need(n);
				p += n;
			}
			// 0x141868770: low 2 bits = width 1 / 2 / 4 bytes, value >> 2
			std::uint32_t vsval()
			{
				need(1);
				const unsigned w = d[p] & 3;
				if (w == 3) {
					throw Short("a count has a width the game never writes");
				}
				const std::size_t n = w == 0 ? 1 : w == 1 ? 2 : 4;
				need(n);
				std::uint32_t v = 0;
				for (std::size_t k = 0; k < n; ++k) {
					v |= static_cast<std::uint32_t>(d[p + k]) << (8 * k);
				}
				p += n;
				return v >> 2;
			}
			// 0x1418689e0: as vsval, bit 2 the sign, value >> 3
			void svsval()
			{
				need(1);
				const unsigned w = d[p] & 3;
				if (w == 3) {
					throw Short("a value has a width the game never writes");
				}
				skip(w == 0 ? 1 : w == 1 ? 2 : 4);
			}
			void fid(std::size_t n = 1) { refs += n; }
			// a count read from the data, before a loop over it: no loop may run past the data (a wrong guess elsewhere
			// would otherwise run for billions of items)
			std::uint32_t count(std::uint32_t n, std::size_t minBytes)
			{
				if (minBytes && static_cast<std::uint64_t>(n) * minBytes > d.size() - p) {
					throw Short("a count is larger than the data left");
				}
				return n;
			}
		};

		struct Unknown : std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};

		// u16 references the block added, u16 strings it added, vsval length (patched in by 0x141868890), the bytes
		void Sized(Rd& r)
		{
			const auto nref = r.u16();
			r.u16();
			r.skip(r.vsval());
			r.fid(nref);
		}

		// 0x141867200 / 0x140b75360: u8 version; u16 references, u16 strings when version >= 4; vsval length; the bytes
		void Block(Rd& r)
		{
			const auto v = r.u8();
			std::uint16_t nref = 0;
			if (v >= 4) {
				nref = r.u16();
				r.u16();
			}
			r.skip(r.vsval());
			r.fid(nref);
		}

		void Extras(Rd& r);

		// two lists of (form, value(s)) with a bit per value that differs from the form's default (0x1419aec20)
		void BitLists(Rd& r)
		{
			const auto n1 = r.count(r.vsval(), 0);
			const auto b1 = r.p;
			r.skip((n1 + 7) / 8);
			for (std::uint32_t i = 0; i < n1; ++i) {
				r.fid();
				if ((r.d[b1 + (i >> 3)] >> (i & 7)) & 1) {
					r.skip(4);
				}
			}
			const auto n2 = r.count(r.vsval(), 0);
			const auto b2 = r.p;
			r.skip((3 * static_cast<std::size_t>(n2) + 7) / 8);
			for (std::uint32_t i = 0; i < n2; ++i) {
				r.fid();
				for (std::uint32_t k = 0; k < 3; ++k) {
					const auto j = 3 * i + k;
					if ((r.d[b2 + (j >> 3)] >> (j & 7)) & 1) {
						r.skip(4);
					}
				}
			}
		}

		// TESNPC::SaveGame 0x140cdf140 (an NPC_ change form, or the leveled base inside extra 0x2b), a_flags its change flags
		void Npc(Rd& r, std::uint32_t f, SfNpcData* a_out)
		{
			// TESActorBase::SaveGame 0x140cc5ce0
			r.fid();  // TESForm::SaveGame: the form's own ID
			if (f & 0x1) {
				r.skip(6);
			}
			if (f & 0x2) {  // 0x1405d5a10: 16 bytes (+0xe8 +8)
				if (a_out) {
					a_out->statsOffset = r.p;
				}
				r.skip(16);
			}
			if (f & 0x40) {  // factions: vsval n, n x (FORMID, u8 rank)
				const auto n = r.count(r.vsval(), 1);
				if (a_out) {
					a_out->factionsOffset = r.p;
					a_out->factionRefs = r.refs;
					a_out->factions = n;
				}
				r.fid(n);
				r.skip(n);
			}
			if (f & 0x10) {
				r.fid(r.vsval());
			}
			if (f & 0x8) {
				r.skip(7);
			}
			if (f & 0x20) {  // the name: a string (string list)
				if (a_out) {
					a_out->hasName = true;
				}
			}
			if (f & 0x1000000) {
				r.u8();
			}
			if (f & 0x100) {
				r.u8();
			}
			if (f & 0x200) {
				r.skip(8);
			}
			if (f & 0x400) {
				r.fid();
			}
			if (f & 0x2000000) {
				r.fid(2);
			}
			if (f & 0x800) {  // face: u8, FORMID, vsval n FORMIDs, then 0x140cdd070
				r.u8();
				r.fid();
				r.fid(r.vsval());
				if (a_out) {
					a_out->faceOffset = r.p;
				}
				const auto n1 = r.count(r.vsval(), 5);  // (vsval key, f32)
				for (std::uint32_t i = 0; i < n1; ++i) {
					r.vsval();
					r.skip(4);
				}
				const auto n2 = r.count(r.vsval(), 2);  // 0x140cefdb0: (vsval key, vsval j, j x (string, f32))
				for (std::uint32_t i = 0; i < n2; ++i) {
					r.vsval();
					r.skip(4ull * r.count(r.vsval(), 4));
				}
				r.u8();  // then 6 strings
				r.skip(4ull * r.count(r.vsval(), 4));  // (string, f32)
				r.skip(12ull * r.u8());  // u8 n, n x (u32, 3 strings, u32, u32)
			}
			if (f & 0x80) {
				r.u8();
			}
			if (f & 0x4000) {  // u32 n, n x f32, then 12 bytes
				r.skip(4ull * r.count(r.u32(), 4) + 12);
			}
			if (f & 0x1000) {
				r.fid();
			}
			if (f & 0x2000) {
				r.fid();
			}
		}

		void Extra(Rd& r, std::uint8_t t)
		{
			switch (t) {
			// no data, references only
			case 0x1b: case 0x3a: case 0xfd:
				return;
			case 0x86: case 0xd2: case 0x20: case 0x45: case 0x5f: case 0x6a: case 0x88: case 0x8f: case 0xc1: case 0xd1:
			case 0x42: case 0x62: case 0x7e: case 0x99: case 0xa0: case 0xc0:
			case 0x83:  // (the first item's form; none when the list is empty: no bytes either way)
				r.fid();
				return;
			case 0x1a:
				r.fid(2);
				return;
			// fixed sizes
			case 0x46: case 0x1d: case 0x4a: case 0x50:
				r.skip(1);
				return;
			case 0x15:
				r.skip(2);
				return;
			case 0x21: case 0x23: case 0x25: case 0x26: case 0x2c: case 0x2d: case 0x54: case 0x64: case 0xc5:
			case 0xd9: case 0x9c: case 0x2a: case 0x57: case 0xb4: case 0xcf: case 0xdd: case 0xb9: case 0x1c:
				r.skip(4);
				return;
			case 0x24: case 0xab:
				r.skip(8);
				return;
			case 0xaf:
				r.skip(24);
				return;
			case 0x61:
				r.skip(32);
				return;
			case 0x1f: r.fid(2); r.skip(1); return;
			case 0x9b: r.fid(); r.skip(2); return;
			case 0xa8: r.fid(); r.skip(0x40); return;
			case 0x28: r.fid(); r.skip(6); return;
			case 0xda: r.fid(5); r.skip(5); return;
			case 0x81: r.fid(2); r.skip(20); return;
			case 0x92: case 0x53: r.fid(); r.skip(4); return;
			case 0x29: r.fid(); r.skip(25); return;
			case 0x36: r.fid(); r.skip(1); return;
			case 0x2f: r.fid(); r.skip(2); return;  // (virtual writer at the extra's +0x18; 2 bytes and a form in every save)
			case 0xa5: r.fid(2); r.skip(5); return;  // ExtraInteraction: u32 type (Passenger 1, Mount 0), 2 forms, u8 (their +0x68 save is empty)
			case 0x17: r.fid(2); r.skip(7); return;  // FORMID, FORMID, u32, u8 x3
			case 0x7c: r.fid(2); r.skip(32); return;  // 0x14031ad70: 4 x 8 bytes, FORMID, FORMID
			case 0x16: r.fid(2); r.skip(16); return;  // FORMID, 12 bytes + FORMID (0x140acc660), u32
			case 0xb3: r.fid(r.vsval()); return;  // 0x14053a200: vsval n FORMIDs
			case 0x71: {  // vsval n, n x (FORMID, 8 bytes, u8)
				const auto n = r.count(r.vsval(), 9);
				r.fid(n);
				r.skip(9ull * n);
				return;
			}
			case 0x19: {  // vsval n (patched in), n x (FORMID, u8)
				const auto n = r.count(r.vsval(), 1);
				r.fid(n);
				r.skip(n);
				return;
			}
			case 0x41: {  // vsval n, n x 0x141a154d0 (u8 flags, f32 if bit 0)
				const auto n = r.count(r.vsval(), 1);
				for (std::uint32_t i = 0; i < n; ++i) {
					if (r.u8() & 1) {
						r.skip(4);
					}
				}
				return;
			}
			case 0x2b: {  // a leveled actor: FORMID; vsval 15, bits, FORMID per bit; FORMID base, u32 its change flags, its NPC_ data
				r.fid();
				const auto n = r.count(r.vsval(), 0);
				const auto b = r.p;
				r.skip((n + 7) / 8);
				for (std::uint32_t i = 0; i < n; ++i) {
					if ((r.d[b + (i >> 3)] >> (i & 7)) & 1) {
						r.fid();
					}
				}
				r.fid();
				const auto f = r.u32();
				Npc(r, f, nullptr);
				return;
			}
			case 0x22: r.svsval(); return;
			// lists
			case 0x84: {  // vsval n, n x (FORMID, u8)
				const auto n = r.count(r.vsval(), 1);
				r.fid(n);
				r.skip(n);
				return;
			}
			case 0x80: {  // 0x14054b340: vsval n, n x (FORMID, u32)
				const auto n = r.count(r.vsval(), 4);
				r.fid(n);
				r.skip(4ull * n);
				return;
			}
			case 0x4c: {  // 0x14055d320: vsval n, n x (FORMID, u8, FORMID)
				const auto n = r.count(r.vsval(), 1);
				r.fid(2ull * n);
				r.skip(n);
				return;
			}
			case 0xbf:
				BitLists(r);
				return;
			case 0x33: {  // 0x140404cf0: u8 flags; 1: u16; 2: vsval n, 3 bits per item, items (FORMID, u8 if bit 0, u8 if bit 1)
				const auto fl = r.u8();
				if (fl & 1) {
					r.skip(2);
				}
				if (fl & 2) {
					const auto n = r.count(r.vsval(), 0);
					const auto b = r.p;
					r.skip((3 * static_cast<std::size_t>(n) + 7) / 8);
					for (std::uint32_t i = 0; i < n; ++i) {
						r.fid();
						for (std::uint32_t k = 0; k < 2; ++k) {
							const auto j = 3 * i + k;
							if ((r.d[b + (j >> 3)] >> (j & 7)) & 1) {
								r.skip(1);
							}
						}
					}
				}
				return;
			}
			case 0x96: {  // 0x14054f5a0: u8 flags; 1/2/4 FORMID; 8 value (0x80: vsval, else u32); 0x10 u16; 0x20 vsval n x u32; 0x40 vsval n FORMIDs
				const auto fl = r.u8();
				for (unsigned b : { 1u, 2u, 4u }) {
					if (fl & b) {
						r.fid();
					}
				}
				if (fl & 8) {
					if (fl & 0x80) {
						r.vsval();
					} else {
						r.skip(4);
					}
				}
				if (fl & 0x10) {
					r.skip(2);
				}
				if (fl & 0x20) {
					r.skip(4ull * r.count(r.vsval(), 4));
				}
				if (fl & 0x40) {
					r.fid(r.vsval());
				}
				return;
			}
			case 0x0c: {  // 0x140552b80: vsval n FORMIDs; vsval n bytes
				r.fid(r.vsval());
				r.skip(r.vsval());
				return;
			}
			case 0x30: {  // FORMID + 0x141764a10: vsval n, n x 0x141741450
				r.fid();
				const auto n = r.count(r.vsval(), 6);
				for (std::uint32_t i = 0; i < n; ++i) {
					r.fid();
					// u8 index (the template's +0x34 is 0; 0xff = none) or vsval of +0x34: the same byte when small
					r.need(1);
					const auto b = r.d[r.p];
					if (b != 0xff && (b & 3)) {
						r.vsval();
					} else {
						r.u8();
					}
					r.vsval();
					Sized(r);
				}
				return;
			}
			case 0xbe: case 0xca:  // vsval n, n strings (string list)
				r.vsval();
				return;
			case 0xe7: {  // 0x14055dc10: vsval n, n x (FORMID, FORMID, u32)
				const auto n = r.count(r.vsval(), 4);
				r.fid(2ull * n);
				r.skip(4ull * n);
				return;
			}
			case 0xeb: {  // u8, vsval n, n x (u8, u32)
				r.u8();
				r.skip(5ull * r.count(r.vsval(), 5));
				return;
			}
			case 0xea:  // u8 (1 = none), else a FORMID
				if (!r.u8()) {
					r.fid();
				}
				return;
			case 0xe2:  // vsval n FORMIDs
				r.fid(r.vsval());
				return;
			case 0xe5: {  // 0x140560070: u32 n, n x (FORMID, u32, u16, vsval m, m FORMIDs)
				const auto n = r.count(r.u32(), 7);
				for (std::uint32_t i = 0; i < n; ++i) {
					r.fid();
					r.skip(6);
					r.fid(r.vsval());
				}
				return;
			}
			case 0xbc: {  // 0x140f50660
				const auto n = r.count(r.u32(), 4);
				for (std::uint32_t i = 0; i < n; ++i) {  // 0x140f515a0
					r.fid();
					const auto k = r.count(r.u32(), 17);
					r.fid(2ull * k);
					r.skip(17ull * k);
				}
				r.fid(r.vsval());
				r.u8();
				r.skip(4 + 12 + 8);
				return;
			}
			case 0xee: {  // u32 n, n x (FORMID, u32)
				const auto n = r.count(r.u32(), 4);
				r.fid(n);
				r.skip(4ull * n);
				return;
			}
			// actor extras
			case 0xe6:  // u32 n, n x (u32, u8)
				r.skip(5ull * r.count(r.u32(), 5));
				return;
			case 0x56: {  // vsval n, n x (FORMID, u8); FORMID, u8, FORMID, u8
				const auto n = r.count(r.vsval(), 1);
				r.fid(n);
				r.skip(n);
				r.fid(2);
				r.skip(2);
				return;
			}
			case 0x7f: {  // 12 bytes + FORMID (0x140acc660), FORMID, u32, vsval n, n x (12+FORMID, FORMID, 12+FORMID, FORMID, u8)
				r.skip(12);
				r.fid(2);
				r.skip(4);
				const auto n = r.count(r.vsval(), 25);
				r.fid(4ull * n);
				r.skip(25ull * n);
				return;
			}
			default:
				// 0x66 (a running AI package's own data: CustomActorPackageData 0x141ad3080 / GuardActorPackageData
				// 0x141ad5a50, not read yet) and types no save has shown yet
				throw Unknown("extra data type " + std::to_string(t));
			}
		}

		void Extras(Rd& r)
		{
			const auto n = r.count(r.vsval(), 1);
			for (std::uint32_t i = 0; i < n; ++i) {
				Extra(r, r.u8());
			}
		}

		// 0x1421592f0: the reference's component data (ships, outposts, ...): u8 has; then the components
		std::uint32_t Components(Rd& r)
		{
			if (!r.u8()) {
				return 0;
			}
			const auto n = r.count(r.u32(), 5);
			r.fid(n);
			for (std::uint32_t i = 0; i < n; ++i) {
				Sized(r);  // 0x142159e40
			}
			r.u8();  // 0x0c
			for (int i = 0; i < 3; ++i) {  // 0x14215a010: u8 code (0 = none), then a sized block
				if (r.u8()) {
					Sized(r);
				}
			}
			const auto m = r.count(r.u32(), 1);
			for (std::uint32_t i = 0; i < m; ++i) {  // u8 1 + FORMID + sized block, or u8 0
				if (r.u8()) {
					r.fid();
					Sized(r);
				}
			}
			r.u8();
			r.fid();
			const auto k = r.count(r.vsval(), 28);  // 0x14215e5e0: k x (FORMID, 16 bytes, 12 bytes)
			r.fid(k);
			r.skip(28ull * k);
			r.skip(4);  // f32 +0xc8
			r.fid();
			r.skip(8);  // f32 +0xcc, +0xd0
			r.fid(2);
			r.skip(8 + 4 + 8);  // f32 +0xd8 +0xdc; u8 x4; f32 +0xf8 +0xfc
			const auto c = r.u64();
			if (c > r.d.size()) {
				throw Short("a count is larger than the data left");
			}
			r.fid(static_cast<std::size_t>(c));
			if (r.u8() & 1) {  // 0x141a154d0
				r.skip(4);
			}
			r.u8();
			return n ? n : 0xFFFFFFFFu;
		}

		void Position(Rd& r)
		{
			// 0x141835450: u8 flags, FORMID cell, 3 x f32, a f32 per rotation bit 0..2, then one byte unless bit 3 or 4
			const auto fl = r.u8();
			r.fid();
			r.skip(12);
			for (int k = 0; k < 3; ++k) {
				if (fl & (1 << k)) {
					r.skip(4);
				}
			}
			if (!(fl & 0x18)) {
				r.u8();
			}
		}

		// one inventory item (0x140641180): FORMID base, vsval stacks, per stack its extra data and u32 count; u32, u8
		void Inventory(Rd& r, std::vector<SfInventoryItem>* a_items)
		{
			const auto n = r.count(r.vsval(), 7);
			for (std::uint32_t i = 0; i < n; ++i) {
				SfInventoryItem it;
				it.begin = r.p;
				it.refIndex = r.refs;
				r.fid();
				const auto stacks = r.count(r.vsval(), 5);
				for (std::uint32_t s = 0; s < stacks; ++s) {
					SfInventoryStack st;
					st.extrasBegin = r.p;
					const auto refs0 = r.refs;
					Extras(r);
					st.extraRefs = r.refs - refs0;
					st.countOffset = r.p;
					st.count = r.u32();
					it.stacks.push_back(st);
				}
				it.value20 = r.u32();
				it.value24 = r.u8();
				it.end = r.p;
				if (a_items) {
					a_items->push_back(std::move(it));
				}
			}
		}
	}

	SfReferenceData DecodeReferenceSF(std::uint32_t a_formID, const ChangeForm& a_cf)
	{
		SfReferenceData out;
		const auto code = a_cf.typeCode();
		if (code != 0 && code != 1) {
			// (projectiles, hazards and the other reference types write more of their own: not read yet)
			out.error = IsReferenceChangeFormSF(code) ? "this kind of reference is not read yet" : "not a reference";
			return out;
		}
		if (a_cf.version != 153) {
			out.error = "saved by an older game version (" + std::to_string(a_cf.version) + "); the game converts it on load";
			return out;
		}
		const auto f = a_cf.changeFlags;
		const bool actor = code == 1;
		Rd r{ a_cf.data };
		try {
			// 1. initial data (0x141832fd0)
			switch (InitialDataTypeSF(a_formID, code, f)) {
			case 5:
				Position(r);
				r.fid();
				break;
			case 6:
				Position(r);
				r.fid();
				r.skip(4);
				break;
			case 4:
				Position(r);
				break;
			case 7:
				r.fid();
				break;
			default:
				break;
			}
			// 2. change flag 0x4: havok
			if (f & 0x4) {
				Block(r);
			}
			// 3. Actor::SaveGame 0x141916b30 (the player's PlayerCharacter::SaveGame 0x141a46940 calls it first): its
			// head, before it calls TESObjectREFR::SaveGame
			if (actor) {
				const auto af = r.u32();
				out.actorFlags = af;
				r.skip(4);
				if (af & 1) {
					r.skip(4);
				}
				if (af & 2) {
					r.u8();
				}
				r.skip(5);
			}
			// 4. TESObjectREFR::SaveGame 0x140b29700
			r.fid();  // TESForm::SaveGame 0x1405df980: the form's own ID
			if (f & 0x1) {
				r.skip(6);
			}
			if ((f & 0x80) && !actor) {
				r.fid();
				if (!(f & 0xa6031f40u)) {
					r.fid();
				}
			}
			if (f & 0x10) {
				r.skip(4);
			}
			if (f & (actor ? 0xa6071b40u : 0xa6031f40u)) {
				Extras(r);
			}
			out.hasInventory = (f & 0x8000020u) != 0;
			out.inventoryBegin = r.p;
			out.inventoryRefBegin = r.refs;
			if (out.hasInventory) {
				Inventory(r, &out.items);
			}
			out.inventoryEnd = r.p;
			out.inventoryRefEnd = r.refs;
			if (f & 0x10000000u) {
				Block(r);
			}
			out.components = Components(r);
			out.referenceEnd = r.p;
			out.referenceRefs = r.refs;
			if (actor) {
				// the actor's AI process and the rest of Actor::SaveGame (and the player's own part) follow: not read
				out.ok = true;
			} else if (r.p != a_cf.data.size()) {
				out.error = std::to_string(a_cf.data.size() - r.p) + " bytes after the end";
			} else if (r.refs != a_cf.sfRefs.size()) {
				out.error = "the data names " + std::to_string(r.refs) + " forms, the list holds " + std::to_string(a_cf.sfRefs.size());
			} else {
				out.ok = true;
			}
			if (out.ok && out.inventoryRefEnd > a_cf.sfRefs.size()) {
				out.ok = false;
				out.error = "the inventory names more forms than the list holds";
			}
		} catch (const std::exception& e) {
			out.error = e.what();
			out.items.clear();
		}
		return out;
	}

	SfNpcData DecodeNpcSF(const ChangeForm& a_cf)
	{
		SfNpcData out;
		if (a_cf.typeCode() != 9) {
			out.error = "not an NPC_";
			return out;
		}
		if (a_cf.version != 153) {
			out.error = "saved by an older game version (" + std::to_string(a_cf.version) + "); the game converts it on load";
			return out;
		}
		Rd r{ a_cf.data };
		try {
			Npc(r, a_cf.changeFlags, &out);
			if (r.p != a_cf.data.size()) {
				out.error = std::to_string(a_cf.data.size() - r.p) + " bytes after the end";
			} else if (r.refs != a_cf.sfRefs.size()) {
				out.error = "the data names " + std::to_string(r.refs) + " forms, the list holds " + std::to_string(a_cf.sfRefs.size());
			} else {
				out.ok = true;
			}
		} catch (const std::exception& e) {
			out.error = e.what();
		}
		return out;
	}

	bool RenameNpcSF(SaveFile& a_save, ChangeForm& a_cf, const std::string& a_name)
	{
		// the name (change flag 0x20) is the first string an NPC_ record writes (TESActorBase::SaveGame 0x140cc5ce0, before
		// any face string): the record's string list entry 0. The new name goes to the end of the save's string table
		// (an entry other forms may share stays as it is).
		const auto d = DecodeNpcSF(a_cf);
		if (!d.ok || !d.hasName || a_cf.sfStrings.empty() || a_name.empty() || a_save.sfStrings.size() >= 0xFFFF) {
			return false;
		}
		a_save.sfStrings.push_back(a_name);
		a_cf.sfStrings[0] = static_cast<std::uint16_t>(a_save.sfStrings.size() - 1);
		if (a_save.formIDOf(a_cf.refID) == 0x7) {
			a_save.header.playerName = a_name;  // (the name the game's load menu shows)
		}
		return true;
	}

	RefID RefIDForFormSF(SaveFile& a_save, std::uint32_t a_formID)
	{
		RefID id;
		if ((a_formID >> 24) == 0xFF) {
			id.data = (2u << 22) | (a_formID & 0x3FFFFF);  // made in game
			return id;
		}
		if (a_formID < 0x400000) {
			id.data = (1u << 22) | a_formID;  // Starfield.esm, small enough to be written as it is
			return id;
		}
		for (std::size_t i = 0; i < a_save.formIDs.size(); ++i) {
			if (a_save.formIDs[i] == a_formID) {
				id.data = static_cast<std::uint32_t>(i + 1);
				return id;
			}
		}
		if (a_save.formIDs.size() + 1 >= 0x400000) {
			id.data = 3u << 22;  // (no room: invalid)
			return id;
		}
		a_save.formIDs.push_back(a_formID);
		id.data = static_cast<std::uint32_t>(a_save.formIDs.size());
		return id;
	}

	bool AddInventoryItemSF(SaveFile& a_save, ChangeForm& a_cf, std::uint32_t a_formID, std::uint32_t a_count, std::string* a_why)
	{
		auto why = [&](const char* m) {
			if (a_why) {
				*a_why = m;
			}
			return false;
		};
		const auto fid = a_save.formIDOf(a_cf.refID);
		const auto r = DecodeReferenceSF(fid, a_cf);
		if (!r.ok) {
			return why("its data cannot be read");
		}
		if (!r.hasInventory) {
			return why("the save keeps no inventory for it");
		}
		if (!a_count || !a_formID) {
			return why("nothing to add");
		}
		for (const auto& it : r.items) {
			if (it.refIndex < a_cf.sfRefs.size() && a_save.formIDOf(a_cf.sfRefs[it.refIndex]) == a_formID) {
				return why("it already has that item: change its count instead");
			}
		}
		const auto id = RefIDForFormSF(a_save, a_formID);
		if (id.type() == RefID::Type::kInvalid) {
			return why("the save's form table is full");
		}
		// the item count (vsval) at the start of the inventory, written again one higher (its width can grow)
		Rd cnt{ a_cf.data, r.inventoryBegin };
		const auto n = cnt.vsval();
		const auto oldLen = cnt.p - r.inventoryBegin;
		std::vector<std::uint8_t> nv;
		const std::uint32_t v = (n + 1) << 2;
		if (n + 1 < 0x40) {
			nv = { static_cast<std::uint8_t>(v) };
		} else if (n + 1 < 0x4000) {
			nv = { static_cast<std::uint8_t>(v | 1), static_cast<std::uint8_t>(v >> 8) };
		} else {
			const std::uint32_t w = v | 2;
			nv = { static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8), static_cast<std::uint8_t>(w >> 16), static_cast<std::uint8_t>(w >> 24) };
		}
		// the new item, as the game's own item constructor (0x14063e5c0 / 0x140646290) leaves it: one stack without extra
		// data holding the count, +0x20 = 0, +0x24 = 0xFE (worked out by the game when it needs it, 0x1405280c0)
		std::vector<std::uint8_t> item = { 1 << 2, 0 };
		for (int k = 0; k < 4; ++k) {
			item.push_back(static_cast<std::uint8_t>(a_count >> (8 * k)));
		}
		item.insert(item.end(), { 0, 0, 0, 0, 0xFE });
		a_cf.data.insert(a_cf.data.begin() + static_cast<std::ptrdiff_t>(r.inventoryEnd), item.begin(), item.end());
		a_cf.data.erase(a_cf.data.begin() + static_cast<std::ptrdiff_t>(r.inventoryBegin),
			a_cf.data.begin() + static_cast<std::ptrdiff_t>(r.inventoryBegin + oldLen));
		a_cf.data.insert(a_cf.data.begin() + static_cast<std::ptrdiff_t>(r.inventoryBegin), nv.begin(), nv.end());
		a_cf.sfRefs.insert(a_cf.sfRefs.begin() + static_cast<std::ptrdiff_t>(r.inventoryRefEnd), id);
		return true;
	}

	bool SetInventoryCountSF(ChangeForm& a_cf, const SfInventoryStack& a_stack, std::uint32_t a_count)
	{
		if (a_stack.countOffset + 4 > a_cf.data.size()) {
			return false;
		}
		std::memcpy(a_cf.data.data() + a_stack.countOffset, &a_count, 4);
		return true;
	}
}
