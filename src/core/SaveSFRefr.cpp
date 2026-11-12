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

#include <cmath>
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
			std::size_t                      strs = 0;  // strings the data writes (the string list), counted like refs
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
			r.strs += r.u16();
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
				r.strs += r.u16();
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
			std::size_t strings = 0;
			if (f & 0x20) {  // the name: a string (string list)
				++strings;
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
				const auto add = [&](int a_group, std::uint32_t a_key, std::size_t a_name) {
					if (a_out) {
						a_out->face.push_back({ a_group, a_key, a_name, r.p });
					}
					r.skip(4);
				};
				const auto n1 = r.count(r.vsval(), 5);  // (vsval key, f32)
				for (std::uint32_t i = 0; i < n1; ++i) {
					const auto key = r.vsval();
					add(1, key, SIZE_MAX);
				}
				const auto n2 = r.count(r.vsval(), 2);  // 0x140cefdb0: (vsval key, vsval j, j x (string, f32))
				for (std::uint32_t i = 0; i < n2; ++i) {
					const auto key = r.vsval();
					const auto j = r.count(r.vsval(), 4);
					for (std::uint32_t k = 0; k < j; ++k) {
						add(2, key, strings++);
					}
				}
				r.u8();  // then 6 strings
				strings += 6;
				const auto n5 = r.count(r.vsval(), 4);  // (string, f32)
				for (std::uint32_t k = 0; k < n5; ++k) {
					add(3, 0, strings++);
				}
				const auto n6 = r.u8();  // u8 n, n x (u32, 3 strings, u32, u32)
				r.skip(12ull * n6);
				strings += 3ull * n6;
			}
			r.strs += strings;
			if (a_out) {
				a_out->strings = strings;
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
			case 0x96: {  // 0x14054f5a0: u8 flags; 1/2/4 FORMID; 8 value (0x80: vsval, else u32); 0x10 u16; no 2/4/8: a string; 0x20 vsval n x (string, u32); 0x40 vsval n x (string, FORMID)
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
				if (!(fl & (2 | 4 | 8))) {
					++r.strs;  // a string (0x14054f725) when it has neither form 2, form 4 nor a value
				}
				if (fl & 0x20) {
					const auto n = r.count(r.vsval(), 4);  // n x (string, u32)
					r.strs += n;
					r.skip(4ull * n);
				}
				if (fl & 0x40) {
					const auto n = r.vsval();  // n x (string (inlined writer 0x14054f8ff), FORMID)
					r.strs += n;
					r.fid(n);
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
				r.strs += r.vsval();
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
				++r.strs;  // a string (+0x70)
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

		void Position(Rd& r, SfReferenceData* a_out = nullptr)
		{
			// 0x141835450: u8 flags, FORMID cell, 3 x f32, a f32 per rotation bit 0..2, then one byte unless bit 3 or 4
			const auto fl = r.u8();
			if (a_out) {
				a_out->hasPosition = true;
				a_out->cellRef = r.refs;
			}
			r.fid();
			r.need(12);
			if (a_out) {
				std::memcpy(a_out->pos, r.d.data() + r.p, 12);
			}
			r.skip(12);
			for (int k = 0; k < 3; ++k) {
				if (fl & (1 << k)) {
					r.need(4);
					if (a_out) {
						std::memcpy(&a_out->rot[k], r.d.data() + r.p, 4);
					}
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
				it.stringBegin = r.strs;
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
				it.refEnd = r.refs;
				it.stringEnd = r.strs;
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
				Position(r, &out);
				out.baseRef = r.refs;
				r.fid();
				break;
			case 6:
				Position(r, &out);
				out.startCellRef = r.refs;
				r.fid();
				r.skip(4);
				break;
			case 4:
				Position(r, &out);
				break;
			case 7:
				r.fid();
				break;
			default:
				break;
			}
			// 2. change flag 0x4: havok
			if (f & 0x4) {
				out.havokOffset = r.p;
				Block(r);
				out.havokEnd = r.p;
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
			out.referenceStrings = r.strs;
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
			} else if (out.strings != a_cf.sfStrings.size()) {
				out.error = "the data writes " + std::to_string(out.strings) + " strings, the list holds " + std::to_string(a_cf.sfStrings.size());
			} else {
				out.ok = true;
			}
		} catch (const std::exception& e) {
			out.error = e.what();
		}
		return out;
	}

	bool SetFaceValueSF(ChangeForm& a_cf, std::size_t a_offset, float a_value)
	{
		if (!std::isfinite(a_value) || a_offset + 4 > a_cf.data.size()) {
			return false;
		}
		std::memcpy(a_cf.data.data() + a_offset, &a_value, 4);
		return true;
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

	float XPForLevelSF(unsigned a_level)
	{
		// 0x140d83030 (the save header's "XP needed" and the XP shown in the load menu use it): fXPStart for level 2, then
		// each level adds a step that grows by powf(fXPBase, (level - 1) * fXPExpMult + 1), rounded to a multiple of 5
		// (up from 2.5). fXPBase 75 and fXPStart 200 are the exe's defaults (registered at 0x14011fa40 / 0x14011faa0;
		// Starfield.esm does not set them); fXPExpMult is 0.0015 in Starfield.esm (exe default 0.01). Checked against 21
		// saves: levels 17 to 73 give the game's own numbers.
		if (a_level <= 1) {
			return 0.0f;
		}
		const float base = 75.0f, mult = 0.0015f, start = 200.0f;
		float total = start, step = start;
		for (unsigned l = 3; l <= a_level; ++l) {
			step = std::pow(base, static_cast<float>(l - 1) * mult + 1.0f) + step;
			const float rem = std::fmod(step, 5.0f);
			step = std::floor(step - rem + (rem >= 2.5f ? 5.0f : 0.0f));
			total += step;
		}
		return total;
	}

	SfActorValues FindActorValuesSF(const SaveFile& a_save, const ChangeForm& a_actor, const std::function<bool(std::uint32_t)>& a_isActorValue)
	{
		// The actor values (Actor +0x260, 0x1419aec20) come after the AI process in Actor::SaveGame, which is not read; the
		// lists are found instead by what they must look like: four 0x1419ae290 blocks (u8 0x0c, u8 bits 0..2, a f32 per
		// bit), then two lists (vsval n, n bits, n x (FORMID, f32 if its bit: the base value, written when it differs from
		// the actor value's default, AVIF +0x178); vsval m, 3m bits, m x (FORMID, f32 per bit: three modifiers, written when
		// not 0)) whose forms, taken in a row from the record's reference list, are every one an actor value
		// (a_isActorValue). Exactly one place may fit. (In 21 saves of two players exactly one fit for the player, and
		// Experience minus the header's XP gave the game's XP for the level each time.)
		SfActorValues out;
		const auto& d = a_actor.data;
		const auto& refs = a_actor.sfRefs;
		std::vector<std::uint32_t> pre(refs.size() + 1, 0);
		for (std::size_t i = 0; i < refs.size(); ++i) {
			pre[i + 1] = pre[i] + (a_isActorValue(a_save.formIDOf(refs[i])) ? 1u : 0u);
		}
		std::vector<SfActorValues> fits;
		for (std::size_t p = 0; p + 8 < d.size(); ++p) {
			if (d[p] != 0x0c) {
				continue;
			}
			Rd r{ d, p };
			try {
				bool shape = true;
				for (int k = 0; k < 4 && shape; ++k) {
					const auto v = r.u8();
					const auto b = r.u8();
					shape = v == 0x0c && !(b & ~7u);
					if (shape) {
						r.skip(4u * ((b & 1) + ((b >> 1) & 1) + ((b >> 2) & 1)));
					}
				}
				if (!shape) {
					continue;
				}
				SfActorValues f;
				f.listOffset = r.p;
				const auto n1 = r.vsval();
				if (!n1 || n1 > 4000) {
					continue;
				}
				const auto b1 = r.p;
				r.skip((n1 + 7) / 8);
				for (std::uint32_t i = 0; i < n1; ++i) {
					SfActorValue e;
					if ((d[b1 + (i >> 3)] >> (i & 7)) & 1) {
						e.value[0] = r.p;
						r.skip(4);
					}
					f.base.push_back(e);
				}
				const auto n2 = r.vsval();
				if (n2 > 4000) {
					continue;
				}
				const auto b2 = r.p;
				r.skip((3 * static_cast<std::size_t>(n2) + 7) / 8);
				for (std::uint32_t i = 0; i < n2; ++i) {
					SfActorValue e;
					for (std::uint32_t k = 0; k < 3; ++k) {
						const auto j = 3 * i + k;
						if ((d[b2 + (j >> 3)] >> (j & 7)) & 1) {
							e.value[k] = r.p;
							r.skip(4);
						}
					}
					f.modifiers.push_back(e);
				}
				const std::size_t n = n1 + n2;
				for (std::size_t base = 0; base + n <= refs.size(); ++base) {
					if (pre[base + n] - pre[base] == n) {
						auto g = f;
						for (std::size_t k = 0; k < g.base.size(); ++k) {
							g.base[k].refIndex = base + k;
						}
						for (std::size_t k = 0; k < g.modifiers.size(); ++k) {
							g.modifiers[k].refIndex = base + g.base.size() + k;
						}
						fits.push_back(std::move(g));
					}
				}
			} catch (const std::exception&) {
			}
		}
		if (fits.size() != 1) {
			out.fits = fits.size();
			out.error = fits.empty() ? "the actor values were not found" : "the actor values fit in more than one place";
			return out;
		}
		out = std::move(fits.front());
		out.fits = 1;
		out.ok = true;
		return out;
	}

	SfPerks FindPerksSF(const SaveFile& a_save, const ChangeForm& a_actor, const std::function<int(std::uint32_t)>& a_kind)
	{
		SfPerks     out;
		const auto& d = a_actor.data;
		const auto& refs = a_actor.sfRefs;
		// per reference: could be a perk (a perk, or a form of a plugin not read), is a perk; prefix sums for windows
		std::vector<std::size_t> maybe(refs.size() + 1), perk(refs.size() + 1);
		for (std::size_t i = 0; i < refs.size(); ++i) {
			const auto id = a_save.formIDOf(refs[i]);
			const int  k = id == 0 || (id >> 24) == 0xFF ? 0 : a_kind(id);
			maybe[i + 1] = maybe[i] + (k != 0);
			perk[i + 1] = perk[i] + (k == 1);
		}
		struct Fit
		{
			std::size_t p = 0, ranks = 0, base = 0, n = 0, total = 0;
		};
		std::vector<Fit> fits;
		for (std::size_t p = 0; p + 3 < d.size(); ++p) {
			if ((d[p] & 3) == 3) {
				continue;
			}
			Rd r{ d, p };
			try {
				const auto n = r.vsval();
				if (n < 1 || n > 4000) {
					continue;
				}
				r.need(n);
				const auto ranks = r.p;
				bool       ok = true;
				for (std::uint32_t i = 0; i < n && ok; ++i) {
					ok = d[ranks + i] >= 1 && d[ranks + i] <= 20;
				}
				if (!ok) {
					continue;
				}
				r.p += n;
				const auto m = r.vsval();
				const auto k = r.vsval();
				// (the two lists after it are the perks' entry points: never longer than the perk list itself in a save seen)
				if (m > n || k > n) {
					continue;
				}
				r.need(4u * static_cast<std::size_t>(k));
				const std::size_t total = static_cast<std::size_t>(n) + m + k;
				for (std::size_t base = 0; base + total <= refs.size(); ++base) {
					if (maybe[base + total] - maybe[base] == total && 2 * (perk[base + total] - perk[base]) >= total && perk[base + n] - perk[base] >= 1) {
						fits.push_back({ p, ranks, base, n, total });
					}
				}
			} catch (const std::exception&) {
			}
		}
		std::size_t best = 0, bestCount = 0;
		for (const auto& f : fits) {
			if (f.total > best) {
				best = f.total;
				bestCount = 1;
			} else if (f.total == best) {
				++bestCount;
			}
		}
		out.fits = bestCount;
		if (fits.empty() || best < 3) {
			out.error = "the perk list was not found";
			return out;
		}
		if (bestCount != 1) {
			out.error = "the perk list fits in more than one place";
			return out;
		}
		for (const auto& f : fits) {
			if (f.total != best) {
				continue;
			}
			out.listOffset = f.p;
			for (std::size_t i = 0; i < f.n; ++i) {
				SfPerk e;
				e.refIndex = f.base + i;
				e.formID = a_save.formIDOf(refs[e.refIndex]);
				e.rankOffset = f.ranks + i;
				e.rank = d[e.rankOffset];
				out.perks.push_back(e);
			}
		}
		out.ok = true;
		return out;
	}

	SfActiveEffects FindActiveEffectsSF(const SaveFile& a_save, const ChangeForm& a_actor, const std::function<int(std::uint32_t)>& a_kind)
	{
		SfActiveEffects out;
		const auto& d = a_actor.data;
		const auto& refs = a_actor.sfRefs;
		std::vector<int> kind(refs.size());
		for (std::size_t i = 0; i < refs.size(); ++i) {
			kind[i] = a_kind(a_save.formIDOf(refs[i]));
		}
		struct Fit
		{
			std::size_t                 p = 0, end = 0, base = 0;
			std::vector<SfActiveEffect> effects;
		};
		std::vector<Fit> fits;
		for (std::size_t p = 0; p + 8 < d.size(); ++p) {
			if ((d[p] & 3) == 3) {
				continue;
			}
			Rd r{ d, p };
			try {
				const auto n = r.vsval();
				if (n < 1 || n > 4000) {
					continue;
				}
				std::vector<SfActiveEffect> list;
				std::size_t refsUsed = 0;
				bool        shape = true;
				for (std::uint32_t i = 0; i < n && shape; ++i) {
					SfActiveEffect e;
					e.begin = r.p;
					r.need(1);
					// u8 index (0xff: none) or a vsval id; a byte with width bits 3 can only be the u8
					if (d[r.p] == 0xFF || (d[r.p] & 3) == 3) {
						e.effect = r.u8();
					} else {
						e.effect = r.vsval();
					}
					e.archetype = r.vsval();
					const auto rc = r.u16();
					e.strings = r.u16();
					const auto len = r.vsval();
					if (e.archetype > 200 || rc < 4 || rc > 40 || e.strings > 16 || len < 16 || len > 4096) {
						shape = false;
						break;
					}
					const auto at = r.p;
					r.skip(len);
					std::memcpy(&e.elapsed, d.data() + at, 4);
					std::memcpy(&e.magnitude, d.data() + at + 4, 4);
					std::memcpy(&e.duration, d.data() + at + 8, 4);
					std::memcpy(&e.flags, d.data() + at + 12, 4);
					if (!std::isfinite(e.elapsed) || !std::isfinite(e.magnitude) || !std::isfinite(e.duration) ||
						std::fabs(e.elapsed) > 1e9f || std::fabs(e.duration) > 1e9f) {
						shape = false;
						break;
					}
					e.end = r.p;
					e.refIndex = refsUsed;  // relative for now
					e.refCount = 1u + rc;
					refsUsed += e.refCount;
					list.push_back(e);
				}
				if (!shape || list.size() < 1) {
					continue;
				}
				// the magic items, taken in a row from the reference list
				for (std::size_t base = 0; base + refsUsed <= refs.size(); ++base) {
					std::size_t known = 0;
					bool        all = true;
					for (const auto& e : list) {
						const int k = kind[base + e.refIndex];
						if (k == 0) {
							all = false;
							break;
						}
						known += k == 1;
					}
					if (all && known * 2 >= list.size() && known >= 1) {
						Fit f{ p, r.p, base, list };
						fits.push_back(std::move(f));
					}
				}
			} catch (const std::exception&) {
			}
		}
		// a list of one or two effects can fit by chance anywhere: the one with the most effects, when it is the only one
		// of its size (the player's list held 104 to 293 effects in every save seen)
		std::size_t best = 0, bestCount = 0;
		for (const auto& f : fits) {
			if (f.effects.size() > best) {
				best = f.effects.size();
				bestCount = 1;
			} else if (f.effects.size() == best) {
				++bestCount;
			}
		}
		out.fits = bestCount;
		if (fits.empty() || bestCount != 1 || best < 3) {
			out.error = fits.empty() || best < 3 ? "the active effects were not found" : "the active effects fit in more than one place";
			return out;
		}
		for (auto& f : fits) {
			if (f.effects.size() != best) {
				continue;
			}
			out.countOffset = f.p;
			out.listEnd = f.end;
			for (auto& e : f.effects) {
				e.refIndex += f.base;
				const auto id = [&](std::size_t k) { return k < e.refCount ? a_save.formIDOf(refs[e.refIndex + k]) : 0u; };
				e.item = id(0);
				e.caster = id(1);
				e.target = id(2);
				e.source = id(4);
				out.effects.push_back(e);
			}
			break;
		}
		out.ok = true;
		return out;
	}

	std::string RemoveActiveEffectsSF(const SaveFile& a_save, ChangeForm& a_actor, const std::set<std::size_t>& a_indices,
		const std::function<int(std::uint32_t)>& a_kind, std::size_t* a_removed)
	{
		const auto fx = FindActiveEffectsSF(a_save, a_actor, a_kind);
		if (!fx.ok) {
			return fx.error;
		}
		std::vector<std::size_t> gone(a_indices.begin(), a_indices.end());
		for (auto i : gone) {
			if (i >= fx.effects.size()) {
				return "no such effect";
			}
			if (fx.effects[i].strings) {
				return "an effect that writes text cannot be taken out";
			}
		}
		if (gone.empty()) {
			return {};
		}
		const auto backup = a_actor;
		std::sort(gone.rbegin(), gone.rend());
		for (auto i : gone) {
			const auto& e = fx.effects[i];
			a_actor.data.erase(a_actor.data.begin() + static_cast<std::ptrdiff_t>(e.begin), a_actor.data.begin() + static_cast<std::ptrdiff_t>(e.end));
			a_actor.sfRefs.erase(a_actor.sfRefs.begin() + static_cast<std::ptrdiff_t>(e.refIndex),
				a_actor.sfRefs.begin() + static_cast<std::ptrdiff_t>(e.refIndex + e.refCount));
		}
		Rd cnt{ a_actor.data, fx.countOffset };
		const auto n = cnt.vsval();
		const auto oldLen = cnt.p - fx.countOffset;
		const auto left = n - static_cast<std::uint32_t>(gone.size());
		const std::uint32_t v = left << 2;
		std::vector<std::uint8_t> nv;
		if (left < 0x40) {
			nv = { static_cast<std::uint8_t>(v) };
		} else if (left < 0x4000) {
			nv = { static_cast<std::uint8_t>(v | 1), static_cast<std::uint8_t>(v >> 8) };
		} else {
			const std::uint32_t w = v | 2;
			nv = { static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8), static_cast<std::uint8_t>(w >> 16), static_cast<std::uint8_t>(w >> 24) };
		}
		a_actor.data.erase(a_actor.data.begin() + static_cast<std::ptrdiff_t>(fx.countOffset),
			a_actor.data.begin() + static_cast<std::ptrdiff_t>(fx.countOffset + oldLen));
		a_actor.data.insert(a_actor.data.begin() + static_cast<std::ptrdiff_t>(fx.countOffset), nv.begin(), nv.end());
		// read back: the list is found again with that many fewer effects, and the rest of the record still reads
		const auto back = FindActiveEffectsSF(a_save, a_actor, a_kind);
		const auto fid = a_save.formIDOf(a_actor.refID);
		const auto ref = DecodeReferenceSF(fid, a_actor);
		if (!back.ok || back.effects.size() != fx.effects.size() - gone.size() || !ref.ok) {
			a_actor = backup;
			return "the record did not read back after the change: nothing changed";
		}
		if (a_removed) {
			*a_removed = gone.size();
		}
		return {};
	}

	SfPlayerXP FindPlayerXPSF(const SaveFile& a_save, const ChangeForm& a_player, const std::function<bool(std::uint32_t)>& a_isActorValue)
	{
		SfPlayerXP out;
		const auto av = FindActorValuesSF(a_save, a_player, a_isActorValue);
		out.fits = av.fits;
		if (!av.ok) {
			out.error = av.error;
			return out;
		}
		for (const auto& e : av.base) {
			if (a_save.formIDOf(a_player.sfRefs[e.refIndex]) == 0x2C9) {
				if (e.value[0] == SIZE_MAX) {
					out.error = "Experience has no value saved";
					return out;
				}
				out.xpOffset = e.value[0];
				std::memcpy(&out.experience, a_player.data.data() + out.xpOffset, 4);
				out.ok = true;
				return out;
			}
		}
		out.error = "Experience is not among the actor values";
		return out;
	}

	bool SetPlayerLevelSF(SaveFile& a_save, std::uint16_t a_level, float a_xpIntoLevel, const std::function<bool(std::uint32_t)>& a_isActorValue,
		std::string* a_why)
	{
		auto why = [&](const char* m) {
			if (a_why) {
				*a_why = m;
			}
			return false;
		};
		ChangeForm* npc = nullptr;
		ChangeForm* achr = nullptr;
		for (auto& c : a_save.changeForms) {
			const auto fid = a_save.formIDOf(c.refID);
			if (c.typeCode() == 9 && fid == 0x7) {
				npc = &c;
			} else if (c.typeCode() == 1 && fid == 0x14) {
				achr = &c;
			}
		}
		if (!npc || !achr) {
			return why("the save has no record of the player");
		}
		const auto n = DecodeNpcSF(*npc);
		if (!n.ok || n.statsOffset == SIZE_MAX) {
			return why("the player's level is not in the save");
		}
		const auto xp = FindPlayerXPSF(a_save, *achr, a_isActorValue);
		if (!xp.ok) {
			if (a_why) {
				*a_why = xp.error;
			}
			return false;
		}
		std::uint16_t cur = 0;
		std::memcpy(&cur, npc->data.data() + n.statsOffset + 6, 2);
		// the XP curve of this game: the save's own numbers must agree with it (a mod changing the curve is refused)
		if (std::fabs((xp.experience - XPForLevelSF(cur)) - a_save.header.currentXP) > 1.0f) {
			return why("the save's XP does not follow the game's XP curve (a mod may change it)");
		}
		if (a_level < 1 || a_level > 1000) {
			return why("level out of range");
		}
		const float need = XPForLevelSF(a_level + 1u) - XPForLevelSF(a_level);
		if (a_xpIntoLevel < 0 || a_xpIntoLevel >= need) {
			return why("the XP is more than the level holds");
		}
		const float total = XPForLevelSF(a_level) + a_xpIntoLevel;
		std::memcpy(npc->data.data() + n.statsOffset + 6, &a_level, 2);
		std::memcpy(achr->data.data() + xp.xpOffset, &total, 4);
		a_save.header.playerLevel = a_level;
		a_save.header.currentXP = a_xpIntoLevel;
		a_save.header.neededXP = need;
		return true;
	}

	bool SetActorValueSF(ChangeForm& a_actor, std::size_t a_offset, float a_value)
	{
		if (a_offset == SIZE_MAX || a_offset + 4 > a_actor.data.size() || !std::isfinite(a_value)) {
			return false;
		}
		std::memcpy(a_actor.data.data() + a_offset, &a_value, 4);
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

	std::map<std::uint32_t, std::string> ShipNamesSF(const SaveFile& a_save)
	{
		std::map<std::uint32_t, std::string> out;
		const GlobalData* g = nullptr;
		for (const auto& t : a_save.table1) {
			if (t.type == 4) {
				g = &t;
				break;
			}
		}
		if (!g) {
			return out;
		}
		const auto& d = g->raw;
		const auto  u32 = [&](std::size_t a_p) {
			std::uint32_t v;
			std::memcpy(&v, d.data() + a_p, 4);
			return v;
		};
		const auto refOf = [](std::uint32_t a_v) {
			RefID r;
			r.data = a_v & 0xFFFFFF;
			return r;
		};
		for (std::size_t p = 0; p + 4 <= d.size(); ++p) {
			const auto n = u32(p);
			if (n == 0 || n > 127 || p + 4 + n + 18 > d.size()) {
				continue;
			}
			bool text = true;
			for (std::size_t i = 0; i < n && text; ++i) {
				text = d[p + 4 + i] >= 0x20 && d[p + 4 + i] < 0x7F;
			}
			if (!text) {
				continue;
			}
			const auto q = p + 4 + n;
			const auto ship = u32(q + 4), a = u32(q + 10), b = u32(q + 14);
			if ((ship | a | b) >> 24) {
				continue;
			}
			const auto* cf = const_cast<SaveFile&>(a_save).findChangeForm(refOf(ship));
			if (!cf || !IsReferenceChangeFormSF(cf->typeCode())) {
				continue;
			}
			const auto fid = a_save.formIDOf(refOf(ship));
			const auto fa = a_save.formIDOf(refOf(a)), fb = a_save.formIDOf(refOf(b));
			bool baseMatches = false;
			for (const auto& base : BaseObjectsSF(fid, *cf)) {
				const auto bf = a_save.formIDOf(base);
				baseMatches = baseMatches || (bf && (bf == fa || bf == fb));
			}
			if (!baseMatches) {
				continue;
			}
			out[fid] = std::string(reinterpret_cast<const char*>(d.data() + p + 4), n);
			p = q + 17;
		}
		return out;
	}

	std::vector<SfHeldItem> ListInventoriesSF(const SaveFile& a_save)
	{
		std::vector<SfHeldItem> out;
		for (const auto& c : a_save.changeForms) {
			if (c.typeCode() > 1 || c.version != 153 || !(c.changeFlags & 0x8000020)) {
				continue;
			}
			const auto fid = a_save.formIDOf(c.refID);
			const auto r = DecodeReferenceSF(fid, c);
			if (!r.ok) {
				continue;
			}
			for (const auto& it : r.items) {
				SfHeldItem h;
				h.holderRefRaw = c.refID.data;
				h.holder = fid;
				h.holderType = c.typeCode();
				h.item = it.refIndex < c.sfRefs.size() ? a_save.formIDOf(c.sfRefs[it.refIndex]) : 0;
				h.count = static_cast<std::int64_t>(it.total());
				out.push_back(h);
			}
		}
		return out;
	}

	std::string RemoveInventoryItemsSF(const SaveFile& a_save, ChangeForm& a_cf, const std::set<std::uint32_t>& a_items, std::size_t* a_removed)
	{
		const auto fid = a_save.formIDOf(a_cf.refID);
		const auto r = DecodeReferenceSF(fid, a_cf);
		if (!r.ok) {
			return "its data cannot be read (" + r.error + "): nothing changed";
		}
		const bool actor = a_cf.typeCode() == 1;
		// a REFR's data is its reference part: its strings must add up before any is taken out
		if (!actor && r.referenceStrings != a_cf.sfStrings.size()) {
			return "its strings do not add up: nothing changed";
		}
		std::vector<const SfInventoryItem*> gone;
		for (const auto& it : r.items) {
			const auto f = it.refIndex < a_cf.sfRefs.size() ? a_save.formIDOf(a_cf.sfRefs[it.refIndex]) : 0;
			if (a_items.contains(f)) {
				if (actor && it.stringEnd != it.stringBegin) {
					return "an item to remove carries text (a name) on an actor, whose strings are not all counted yet: nothing changed";
				}
				gone.push_back(&it);
			}
		}
		if (gone.empty()) {
			if (a_removed) {
				*a_removed = 0;
			}
			return {};
		}
		const auto backup = a_cf;
		// from the last item back, so the earlier offsets stay right
		for (auto i = gone.rbegin(); i != gone.rend(); ++i) {
			const auto& it = **i;
			a_cf.data.erase(a_cf.data.begin() + static_cast<std::ptrdiff_t>(it.begin), a_cf.data.begin() + static_cast<std::ptrdiff_t>(it.end));
			a_cf.sfRefs.erase(a_cf.sfRefs.begin() + static_cast<std::ptrdiff_t>(it.refIndex), a_cf.sfRefs.begin() + static_cast<std::ptrdiff_t>(it.refEnd));
			a_cf.sfStrings.erase(a_cf.sfStrings.begin() + static_cast<std::ptrdiff_t>(it.stringBegin),
				a_cf.sfStrings.begin() + static_cast<std::ptrdiff_t>(it.stringEnd));
		}
		// the item count (vsval) at the start of the inventory, written again (its width can shrink)
		Rd cnt{ a_cf.data, r.inventoryBegin };
		const auto n = cnt.vsval();
		const auto oldLen = cnt.p - r.inventoryBegin;
		const auto left = n - static_cast<std::uint32_t>(gone.size());
		const std::uint32_t v = left << 2;
		std::vector<std::uint8_t> nv;
		if (left < 0x40) {
			nv = { static_cast<std::uint8_t>(v) };
		} else if (left < 0x4000) {
			nv = { static_cast<std::uint8_t>(v | 1), static_cast<std::uint8_t>(v >> 8) };
		} else {
			const std::uint32_t w = v | 2;
			nv = { static_cast<std::uint8_t>(w), static_cast<std::uint8_t>(w >> 8), static_cast<std::uint8_t>(w >> 16), static_cast<std::uint8_t>(w >> 24) };
		}
		a_cf.data.erase(a_cf.data.begin() + static_cast<std::ptrdiff_t>(r.inventoryBegin),
			a_cf.data.begin() + static_cast<std::ptrdiff_t>(r.inventoryBegin + oldLen));
		a_cf.data.insert(a_cf.data.begin() + static_cast<std::ptrdiff_t>(r.inventoryBegin), nv.begin(), nv.end());
		// read back: the record still decodes, with the items gone
		const auto back = DecodeReferenceSF(fid, a_cf);
		if (!back.ok || back.items.size() != r.items.size() - gone.size() || (!actor && back.referenceStrings != a_cf.sfStrings.size())) {
			a_cf = backup;
			return "the record did not read back after the change: nothing changed";
		}
		if (a_removed) {
			*a_removed = gone.size();
		}
		return {};
	}

	int ResetHavokSF(ChangeForm& a_cf, std::uint32_t a_formID, bool a_dryRun)
	{
		if (a_cf.typeCode() != 0 || a_cf.version != 153 || !(a_cf.changeFlags & 0x4)) {
			return 0;
		}
		const auto r = DecodeReferenceSF(a_formID, a_cf);
		if (!r.ok || r.havokOffset == SIZE_MAX) {
			return -1;
		}
		// the block: u8 version; u16 references, u16 strings (version >= 4); vsval length; the bytes
		Rd b{ a_cf.data, r.havokOffset };
		const auto v = b.u8();
		std::uint16_t nref = 0, nstr = 0;
		if (v >= 4) {
			nref = b.u16();
			nstr = b.u16();
		}
		const auto lenAt = b.p;
		const auto n = b.vsval();
		if (n == 0) {
			return 0;  // nothing saved
		}
		if (nref || nstr) {
			return -1;  // (never seen: the block names forms or strings)
		}
		if (a_dryRun) {
			return 1;
		}
		const auto backup = a_cf.data;
		// the game's own way to save a reference with no physics state: the same block with length 0 (Save1471: 88 of the
		// 2,641 REFR havok blocks and 59 of the 237 actors' are saved so)
		a_cf.data.erase(a_cf.data.begin() + static_cast<std::ptrdiff_t>(lenAt), a_cf.data.begin() + static_cast<std::ptrdiff_t>(r.havokEnd));
		a_cf.data.insert(a_cf.data.begin() + static_cast<std::ptrdiff_t>(lenAt), std::uint8_t(0));
		const auto back = DecodeReferenceSF(a_formID, a_cf);
		if (!back.ok || back.items.size() != r.items.size()) {
			a_cf.data = backup;
			return -1;
		}
		return 1;
	}
}

