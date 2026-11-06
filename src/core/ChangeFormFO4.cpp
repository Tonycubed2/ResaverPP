// ReSaver++ - Fallout 4 change form decoding (see ChangeFormFO4.h). Addresses are RVAs in Fallout4.exe 1.11.240.
#include "ChangeFormFO4.h"

#include "ChangeFormData.h"
#include "ChangeFormFO4In.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace ess::fo4cf
{
	namespace
	{
		struct Unsupported : std::runtime_error
		{
			using std::runtime_error::runtime_error;
		};

		bool Bit(std::uint32_t a_flags, int a_n) { return (a_flags >> a_n) & 1; }

		// BGSLoadFormBuffer: vsval (0xc0fd30): low 2 bits give the width (1, 2 or 3 bytes), value >> 2
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
		void Skip(Reader& r, std::size_t n)
		{
			r.need(n);
			r.seek(r.pos() + n);
		}
		RefID Ref(Reader& r) { return RefID::read(r); }
		void  WStr(Reader& r) { Skip(r, r.u16()); }  // 0xc0fa90: u16 length + characters

		// Extra data types the game's loop (0x294760, jump table at 0x298868 / 0x2986e0) handles with a
		// fixed number of bytes.
		int FixedSize(std::uint8_t t)
		{
			switch (t) {
			case 0x17: case 0x24: case 0x4f: return 2;
			case 0x18: return 19;
			case 0x19: return 13;
			case 0x1c: return 6;
			case 0x1d: return 0;
			case 0x1e: case 0x25: case 0x2e: case 0x55: case 0x5d: case 0xa0: case 0xbb: case 0xc9:
			case 0x2f: case 0x23: case 0x27: case 0x28: case 0x53: case 0x59: case 0x6a: return 4;
			case 0x1f: case 0x2c: case 0x49: case 0x54: case 0x4d: case 0x9c: return 1;
			case 0x21: case 0x58: case 0x95: case 0x3e: return 7;
			case 0x31: return 28;
			case 0x38: case 0x48: case 0x65: case 0x70: case 0x85: case 0x89: case 0x8b: case 0x92: case 0xc3:
			case 0xc4: case 0x8e: case 0x68: case 0x22: return 3;
			case 0x9f: case 0x9b: return 5;
			case 0xa1: return 88;
			case 0xb0: case 0x26: return 8;
			case 0xc0: case 0x82: return 12;
			case 0x67: return 32;
			default: return -1;
			}
		}
		// Types the loop sends to its default case: it logs them and reads nothing.
		bool DefaultCase(std::uint8_t t)
		{
			static const std::uint8_t kList[] = { 0xd, 0xe, 0xf, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x20, 0x29, 0x30, 0x33, 0x36,
				0x37, 0x39, 0x3a, 0x3b, 0x3c, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x46, 0x47, 0x4a, 0x4b, 0x4c, 0x4e, 0x51, 0x52, 0x56,
				0x57, 0x5a, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x66, 0x69, 0x6b, 0x6d, 0x6e, 0x72, 0x73, 0x74, 0x75, 0x76,
				0x77, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x83, 0x84, 0x86, 0x8a, 0x8d, 0x8f, 0x90, 0x91, 0x93, 0x94,
				0x96, 0x97, 0x9a, 0x9e, 0xa2, 0xa3, 0xa5, 0xa6, 0xa7, 0xa8, 0xaa, 0xab, 0xad, 0xb1, 0xb2, 0xb3, 0xb5, 0xb6, 0xb7,
				0xb8, 0xb9, 0xba, 0xbc, 0xbe, 0xc5, 0xc7, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xd1, 0xd2, 0xd4 };
			if (t < 0x0c || t > 0xd5) {
				return true;
			}
			return std::find(std::begin(kList), std::end(kList), t) != std::end(kList);
		}

		struct Ctx
		{
			std::uint32_t ver = 0;  // form version (65 = 1.10.163, 68 = next-gen)
			Refr*         refr = nullptr;  // where the pointer ids read in package data go (an ACHR's reference part)
		};

		void NpcBody(Reader& r, std::uint32_t flags, std::uint32_t ver, std::vector<Part>* parts, std::string* name);

		void ExtraEntry(Reader& r, std::uint8_t t, const Ctx& c, bool top)
		{
			if (const int n = FixedSize(t); n >= 0) {
				Skip(r, static_cast<std::size_t>(n));
				return;
			}
			const auto ver = c.ver;
			switch (t) {
			case 0xac:  // read only when the buffer has the reference being loaded: not for inventory stacks
				if (top) {
					Ref(r);
				}
				return;
			case 0x2a:  // lock
				Skip(r, 2);
				Ref(r);
				Skip(r, ver < 0x36 ? 8 : 4);
				return;
			case 0x0c:
				{
					const auto n = Vs(r);
					Skip(r, 3ull * n);
					Skip(r, Vs(r));
					return;
				}
			case 0xbf:  // workshop (0x388bc0)
				{
					const auto n = r.u32();
					for (std::uint32_t i = 0; i < n; ++i) {
						const auto m = r.u32();
						Skip(r, 9ull * m + 8);
						if (ver >= 0x37) {
							Skip(r, 3ull * Vs(r));
						}
					}
					const auto p = r.u32();
					for (std::uint32_t i = 0; i < p; ++i) {
						Skip(r, ver >= 0x42 ? 3 : 4);
						Skip(r, 4);
					}
					return;
				}
			case 0xb4:
				if (ver < 0x19) {
					throw Unsupported("extra 0xb4 (old version)");
				}
				Skip(r, 24);
				return;
			case 0xd0:
				if (ver < 0x3f) {
					throw Unsupported("extra 0xd0 (old version)");
				}
				Skip(r, 7ull * Vs(r));
				return;
			case 0x8c: Skip(r, 3ull * Vs(r)); return;
			case 0x88: Skip(r, 7ull * Vs(r)); return;
			case 0x50: Skip(r, 6ull * Vs(r)); return;
			case 0x5b: Skip(r, 4ull * Vs(r) + 4); return;
			case 0xc6: Skip(r, 4ull * Vs(r)); return;
			case 0x78: Skip(r, 12ull * Vs(r)); return;
			case 0xc1:
				for (auto n = Vs(r); n; --n) {
					WStr(r);
				}
				return;
			case 0xcf:
				for (auto n = Vs(r); n; --n) {
					WStr(r);
					WStr(r);
					Skip(r, 4);
				}
				return;
			case 0xc2:
				Skip(r, 7ull * Vs(r));
				Skip(r, 15ull * Vs(r));
				return;
			case 0x32:  // 0x29661c: a form id (0x21e330), then an active effect list (0xb896c0, as the actor's)
				Ref(r);
				for (auto n = Vs(r); n; --n) {
					Ref(r);
					Skip(r, 1);
					Vs(r);
					Skip(r, Vs(r));
				}
				return;
			case 0x9d:  // 0x2977ee: a form id (0x21e390)
				Ref(r);
				return;
			case 0x34:  // 0x295a4b: vsval n x (u32, u32)
				Skip(r, 8ull * Vs(r));
				return;
			case 0x44:  // 0x295bdb: vsval n x u32 (0xd4c830)
				Skip(r, 4ull * Vs(r));
				return;
			case 0x87:  // 0x29757e: 12 bytes, a form id, u32, vsval n x (12 bytes, form id, 12 bytes, form id, u8)
				Skip(r, 12);
				Ref(r);
				Skip(r, 4);
				for (auto n = Vs(r); n; --n) {
					Skip(r, 12);
					Ref(r);
					Skip(r, 12);
					Ref(r);
					Skip(r, 1);
				}
				return;
			case 0x35:  // object mods
				{
					Skip(r, 2);
					const auto n = r.u32();
					Skip(r, static_cast<std::size_t>(n) * (ver >= 0x15 ? 6 : 5));
					return;
				}
			case 0x99:
				{
					const auto msg = Ref(r), quest = Ref(r);
					const auto val = r.i32();
					Skip(r, 2);
					if (msg.isZero() && quest.isZero() && val == -2) {
						WStr(r);
					}
					for (auto n = Vs(r); n; --n) {
						WStr(r);
						Ref(r);
					}
					return;
				}
			case 0x5c:
				{
					Skip(r, 11);
					Ref(r);
					for (auto n = Vs(r); n; --n) {  // the entries (0x296aab)
						Skip(r, (ver >= 0x17 ? 4 : 0) + 4 + (ver >= 0x1e ? 1 : 0));
						WStr(r);
						for (auto m = Vs(r); m; --m) {  // 0x437100 -> 0x436f00: a form id and a u32
							Ref(r);
							Skip(r, 4);
						}
					}
					return;
				}
			case 0x98:  // 0x2ada30 / 0x2ae0b0
				for (auto n = Vs(r); n; --n) {
					if (Ref(r).isZero()) {
						continue;  // the rest is read only when the form resolves
					}
					if (r.u16() < 0xFFFF) {
						Skip(r, 8 + 12 + 16 + 4);  // 0xc0fdd0 reads a 48-byte transform packed in 16 bytes
					}
				}
				Skip(r, 4);
				return;
			case 0x1b:  // 0x29556f: vsval n x (form id 0x29bfa0, u8)
				for (auto n = Vs(r); n; --n) {
					Ref(r);
					Skip(r, 1);
				}
				return;
			case 0xd3:  // 0x298341 -> 0x2b5550: a u32
				Skip(r, 4);
				return;
			case 0xd5:  // 0x2983a8 -> 0x2b5860: a form id
				Ref(r);
				return;
			case 0x6c:  // package data (0x295f2b): the type byte, then that type's object (0xceaed0) and its LoadGame
				{
					const auto type = r.u8();
					if (type == 0xFF) {
						return;
					}
					// Actor::LoadGame sets up the pointer id tables before the reference part, so an ACHR's ids are
					// shared with its actor part; a plain REFR has no table of its own (a pointer id stops it)
					detail::In b{ r, ver };
					b.noTable = c.refr == nullptr;
					if (c.refr) {
						b.ids68 = c.refr->ids68;
						b.ids70 = c.refr->ids70;
						b.ids78 = c.refr->ids78;
					}
					try {
						detail::PackageData(b, type);
					} catch (const detail::Stop& e) {
						throw Unsupported(std::string("extra 0x6c: ") + e.what());
					}
					if (c.refr) {
						c.refr->ids68 = b.ids68;
						c.refr->ids70 = b.ids70;
						c.refr->ids78 = b.ids78;
					}
					return;
				}
			case 0x2d:  // leveled creature: base, 13 refs, template, flags, then the NPC data those flags name
				{
					Skip(r, 3 + 13 * 3 + 3);
					const auto fl = r.u32();
					if (fl) {
						NpcBody(r, fl, ver, nullptr, nullptr);
					}
					return;
				}
			default:
				if (DefaultCase(t)) {
					return;
				}
				char b[48];
				std::snprintf(b, sizeof(b), "extra 0x%02x not decoded", t);
				throw Unsupported(b);
			}
		}

		std::vector<Extra> ExtraList(Reader& r, const Ctx& c, bool top)
		{
			std::vector<Extra> out;
			for (auto n = Vs(r); n; --n) {
				Extra e;
				e.range.begin = r.pos();
				e.type = r.u8();
				ExtraEntry(r, e.type, c, top);
				e.range.end = r.pos();
				out.push_back(e);
			}
			return out;
		}

		std::vector<Item> Inventory(Reader& r, const Ctx& c, const SaveFile& s)
		{
			std::vector<Item> items;
			const auto        n = r.u32();
			for (std::uint32_t i = 0; i < n; ++i) {
				Item it;
				it.range.begin = r.pos();
				it.item = Ref(r);
				it.formID = s.formIDOf(it.item);
				const auto stacks = r.u32();
				for (std::uint32_t k = 0; k < stacks; ++k) {
					Stack st;
					st.range.begin = r.pos();
					st.extras = ExtraList(r, c, false);  // loaded with no form: 0xac reads nothing here
					st.countRange.begin = r.pos();
					st.count = r.i32();
					st.countRange.end = r.pos();
					st.flags = r.u16();
					st.range.end = r.pos();
					it.count += st.count;
					it.stacks.push_back(std::move(st));
				}
				it.range.end = r.pos();
				items.push_back(std::move(it));
			}
			return items;
		}

	}

	std::vector<Item> detail::Inventory(In& b, const SaveFile& a_save)
	{
		b.mark('R');  // the u32 count (the items' own reads are not traced)
		Refr ids;     // the actor's pointer id tables, for package data in a stack's extra data
		ids.ids68 = b.ids68;
		ids.ids70 = b.ids70;
		ids.ids78 = b.ids78;
		Ctx c{ b.ver, &ids };
		try {
			auto items = fo4cf::Inventory(b.r, c, a_save);
			b.ids68 = ids.ids68;
			b.ids70 = ids.ids70;
			b.ids78 = ids.ids78;
			return items;
		} catch (const Unsupported& e) {
			throw detail::Stop(std::string("inventory: ") + e.what());
		}
	}

	namespace
	{
		void NpcBody(Reader& r, std::uint32_t f, std::uint32_t ver, std::vector<Part>* parts, std::string* name)
		{
			auto part = [&](const char* a_name, auto&& a_read) {
				const auto b = r.pos();
				a_read();
				if (parts) {
					parts->push_back({ a_name, { b, r.pos() } });
				}
			};
			if (Bit(f, 0)) part("form flags", [&] { Skip(r, 6); });
			if (Bit(f, 1)) part("ACBS", [&] { Skip(r, 20); });
			if (Bit(f, 6)) part("factions", [&] { Skip(r, 4ull * Vs(r)); });
			if (Bit(f, 4)) part("spells", [&] { for (int k = 0; k < 3; ++k) Skip(r, 3ull * Vs(r)); });
			if (Bit(f, 3)) part("AI data", [&] { Skip(r, 24); });
			if (Bit(f, 5)) part("name", [&] {
				const auto n = r.u16();
				r.need(n);
				if (name) {
					name->assign(reinterpret_cast<const char*>(r.here()), n);
				}
				Skip(r, n);
			});
			if (ver >= 0x20 && Bit(f, 24)) part("gender", [&] { Skip(r, 1); });
			if (Bit(f, 9)) part("skills", [&] { Skip(r, 8); });
			if (Bit(f, 10)) part("class", [&] { Ref(r); });
			if (Bit(f, 25)) part("race", [&] { Ref(r); Ref(r); });
			if (Bit(f, 11)) part("face", [&] {
				if (r.u8()) {
					Ref(r);
					if (ver >= 0x40) Ref(r);
					Skip(r, 4);
					Ref(r);
				}
				Skip(r, 3ull * Vs(r));  // head parts
				if (r.u8()) Skip(r, 8ull * r.u32());
				Skip(r, 40ull * r.u32());  // morphs
				Skip(r, 10ull * r.u32());
			});
			if (Bit(f, 14)) part("body morphs", [&] { Skip(r, 4ull * r.u32() + 12); });
			if (ver < 0x20 && Bit(f, 24)) part("gender", [&] { Skip(r, 1); });
			if (Bit(f, 12)) part("outfit", [&] { Ref(r); });
			if (Bit(f, 13)) part("sleep outfit", [&] { Ref(r); });
		}

		std::string Msg(const std::exception& e) { return e.what(); }
	}

	Refr DecodeRefr(const SaveFile& a_save, const ChangeForm& a_cf) { return DecodeRefr(a_save, a_cf, cf::Body(a_cf)); }

	Refr DecodeRefr(const SaveFile& a_save, const ChangeForm& a_cf, const std::vector<std::uint8_t>& a_body)
	{
		Refr out;
		const auto t = static_cast<ChangeFormType>(a_cf.typeCode());
		out.actor = t == ChangeFormType::ACHR;
		if (!a_save.isFO4() || (t != ChangeFormType::REFR && !out.actor)) {
			out.error = "not a Fallout 4 REFR / ACHR";
			return out;
		}
		const std::uint32_t f = a_cf.changeFlags;
		Ctx                 c{ a_save.formVersion, out.actor ? &out : nullptr };
		Reader              r(a_body.data(), a_body.size());
		auto                part = [&](const char* a_name, auto&& a_read) {
            const auto b = r.pos();
            a_read();
            out.parts.push_back({ a_name, { b, r.pos() } });
		};
		try {
			// initial data (TESObjectREFR::LoadGame 0x4f5c50; the same 4 kinds as Skyrim)
			const bool created = a_cf.refID.type() == RefID::Type::kCreated;
			const int  initial = created ? 5 : (Bit(f, 25) || Bit(f, 3)) ? 6 : (Bit(f, 2) || Bit(f, 1)) ? 4 : 0;
			if (initial) {
				part("initial", [&] {
					out.hasInitial = true;
					out.cell = Ref(r);
					for (auto& v : out.pos) v = r.f32();
					for (auto& v : out.rot) v = r.f32();
					if (initial == 5) {  // a created reference: u8, then its base object
						Skip(r, 1);
						out.base = Ref(r);
						out.createdBase = true;
					} else if (initial == 6) {
						out.startCell = Ref(r);
						Skip(r, 4);
					}
				});
			}
			if (Bit(f, 2)) {
				part("havok", [&] {
					out.hasHavok = true;
					out.havokVersion = r.u8();
					out.havokSize = Vs(r);
					Skip(r, out.havokSize);
				});
				out.havokRange = out.parts.back().range;
			}
			if (out.actor) part("actor prefix", [&] { Skip(r, 8); });  // Actor::LoadGame (0xc8abd0) reads u32 u32 first
			const auto refrBegin = r.pos();
			if (Bit(f, 0)) part("form flags", [&] { out.formFlags = r.u32(); Skip(r, 2); });
			if (Bit(f, 7)) part("base object", [&] { out.base = Ref(r); });
			if (Bit(f, 4)) part("scale", [&] { out.hasScale = true; out.scale = r.f32(); });
			if (f & (out.actor ? 0xa6061b40u : 0xa6021f40u)) part("extra data", [&] { out.extras = ExtraList(r, c, true); });
			if (f & 0x08000020u) part("inventory", [&] { out.hasInventory = true; out.inventory = Inventory(r, c, a_save); });
			if (Bit(f, 28)) part("animation", [&] { Skip(r, 1); Skip(r, Vs(r)); });
			out.refrPart = { refrBegin, r.pos() };
			out.decoded = true;
			if (out.actor) {
				if (r.pos() < a_body.size()) {
					out.parts.push_back({ "actor", { r.pos(), a_body.size() } });  // not decoded
				}
			} else {
				out.complete = r.pos() == a_body.size();
				if (!out.complete) {
					out.error = "data left over (" + std::to_string(a_body.size() - r.pos()) + " bytes)";
				}
			}
		} catch (const Unsupported& e) {
			out.error = Msg(e);
		} catch (const std::exception& e) {
			out.error = Msg(e);
		}
		return out;
	}

	Npc DecodeNpc(const SaveFile& a_save, const ChangeForm& a_cf)
	{
		Npc out;
		if (!a_save.isFO4() || static_cast<ChangeFormType>(a_cf.typeCode()) != ChangeFormType::NPC_) {
			out.error = "not a Fallout 4 NPC_";
			return out;
		}
		const auto body = cf::Body(a_cf);
		Reader     r(body.data(), body.size());
		try {
			NpcBody(r, a_cf.changeFlags, a_save.formVersion, &out.parts, &out.name);
			out.decoded = true;
			out.complete = r.pos() == body.size();
			if (!out.complete) {
				out.error = "data left over (" + std::to_string(body.size() - r.pos()) + " bytes)";
			}
		} catch (const std::exception& e) {
			out.error = Msg(e);
		}
		return out;
	}

	std::string ClearHavok(const SaveFile& a_save, ChangeForm& a_cf, bool* a_changed)
	{
		if (a_changed) {
			*a_changed = false;
		}
		if (static_cast<ChangeFormType>(a_cf.typeCode()) != ChangeFormType::REFR) {
			return "not an object reference (REFR)";
		}
		auto       body = cf::Body(a_cf);
		const auto d = DecodeRefr(a_save, a_cf, body);
		if (!d.complete) {
			return "this reference could not be read completely (" + d.error + "): nothing changed";
		}
		if (!d.hasHavok) {
			return "this reference has no saved havok data";
		}
		if (d.havokSize == 0) {
			return {};
		}
		// keep the version byte, write vsval 0 (one byte), drop the blob
		const auto                begin = d.havokRange.begin, end = d.havokRange.end;
		std::vector<std::uint8_t> nb(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(begin + 1));
		nb.push_back(0);
		nb.insert(nb.end(), body.begin() + static_cast<std::ptrdiff_t>(end), body.end());
		ChangeForm copy = a_cf;
		cf::SetBody(copy, nb);
		const auto again = DecodeRefr(a_save, copy);
		const bool same = again.complete && again.hasHavok && again.havokSize == 0 && again.havokRange.end == begin + 2 &&
		                  std::equal(nb.begin() + static_cast<std::ptrdiff_t>(begin + 2), nb.end(), body.begin() + static_cast<std::ptrdiff_t>(end));
		if (!same) {
			return "the reset reference did not read back (" + again.error + "): nothing changed";
		}
		a_cf = std::move(copy);
		if (a_changed) {
			*a_changed = true;
		}
		return {};
	}

	FormList DecodeFormList(const SaveFile& a_save, const ChangeForm& a_cf)
	{
		FormList out;
		if (!a_save.isFO4() || a_cf.typeCode() != kFlstType) {
			out.error = "not a Fallout 4 form list";
			return out;
		}
		try {
			const auto body = cf::Body(a_cf);
			Reader     r(body.data(), body.size());
			if (Bit(a_cf.changeFlags, 0)) {
				Skip(r, 6);
			}
			if (Bit(a_cf.changeFlags, 31)) {
				out.list.begin = r.pos();
				const auto n = r.u32();
				r.need(3ull * n);
				for (std::uint32_t i = 0; i < n; ++i) {
					out.raw.push_back(Ref(r).data);
				}
				out.list.end = r.pos();
			}
			out.complete = r.pos() == body.size();
			if (!out.complete) {
				out.error = "data left over (" + std::to_string(body.size() - r.pos()) + " bytes)";
			}
		} catch (const std::exception& e) {
			out.error = Msg(e);
		}
		return out;
	}

	std::string PurifyFormList(const SaveFile& a_save, ChangeForm& a_cf, std::size_t* a_removed)
	{
		if (a_removed) {
			*a_removed = 0;
		}
		const auto d = DecodeFormList(a_save, a_cf);
		if (!d.complete) {
			return "this form list could not be read completely (" + d.error + "): nothing changed";
		}
		const auto nulls = static_cast<std::size_t>(std::count(d.raw.begin(), d.raw.end(), 0u));
		if (!nulls) {
			return {};
		}
		const auto                body = cf::Body(a_cf);
		std::vector<std::uint8_t> nb(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(d.list.begin));
		const auto                kept = static_cast<std::uint32_t>(d.raw.size() - nulls);
		for (int i = 0; i < 4; ++i) {
			nb.push_back(static_cast<std::uint8_t>(kept >> (8 * i)));
		}
		std::vector<std::uint32_t> want;
		for (auto v : d.raw) {
			if (v) {
				nb.push_back(static_cast<std::uint8_t>(v >> 16));
				nb.push_back(static_cast<std::uint8_t>(v >> 8));
				nb.push_back(static_cast<std::uint8_t>(v));
				want.push_back(v);
			}
		}
		nb.insert(nb.end(), body.begin() + static_cast<std::ptrdiff_t>(d.list.end), body.end());
		ChangeForm copy = a_cf;
		cf::SetBody(copy, nb);
		const auto again = DecodeFormList(a_save, copy);
		if (!again.complete || again.raw != want) {
			return "the form list did not read back (" + again.error + "): nothing changed";
		}
		a_cf = std::move(copy);
		if (a_removed) {
			*a_removed = nulls;
		}
		return {};
	}

	const ChangeForm* FindPlayer(const SaveFile& a_save)
	{
		for (const auto& c : a_save.changeForms) {
			if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR && a_save.formIDOf(c.refID) == 0x14) {
				return &c;
			}
		}
		return nullptr;
	}

	std::int64_t PlayerCaps(const SaveFile& a_save)
	{
		const auto* p = FindPlayer(a_save);
		if (!p) {
			return -1;
		}
		const auto d = DecodeRefr(a_save, *p);
		if (!d.decoded) {
			return -1;
		}
		std::int64_t caps = 0;
		for (const auto& it : d.inventory) {
			if (it.formID == 0x0000000F) {
				caps += it.count;
			}
		}
		return caps;
	}

	std::string SetPlayerCaps(SaveFile& a_save, std::int32_t a_count)
	{
		if (a_count < 0) {
			return "caps cannot be negative";
		}
		auto* p = const_cast<ChangeForm*>(FindPlayer(a_save));
		if (!p) {
			return "no player reference (ACHR 0x14) in this save";
		}
		auto       body = cf::Body(*p);
		const auto d = DecodeRefr(a_save, *p, body);
		if (!d.decoded) {
			return "the player's inventory cannot be read: " + d.error;
		}
		bool first = true;
		for (const auto& it : d.inventory) {
			if (it.formID != 0x0000000F) {
				continue;
			}
			for (const auto& st : it.stacks) {
				const std::int32_t v = first ? a_count : 0;
				std::memcpy(body.data() + st.countRange.begin, &v, 4);
				first = false;
			}
		}
		if (first) {
			return "the player carries no caps (adding the item is not supported)";
		}
		ChangeForm copy = *p;
		cf::SetBody(copy, body);
		const auto again = DecodeRefr(a_save, copy);
		std::int64_t caps = 0;
		for (const auto& it : again.inventory) {
			if (it.formID == 0x0000000F) {
				caps += it.count;
			}
		}
		if (!again.decoded || caps != a_count) {
			return "the re-encoded player reference does not read back";
		}
		*p = std::move(copy);
		return {};
	}

	const char* RefrFlagMeaning(int a_bit, bool a_actor)
	{
		const std::uint32_t m = 1u << a_bit;
		switch (a_bit) {
		case 0: return "form flags (u32 + u16)";
		case 1: return "moved: initial data (cell, position, rotation)";
		case 2: return "havok moved: initial data + havok blob (u8, vsval size, bytes)";
		case 3: return "cell changed: initial data + start cell + u32";
		case 4: return "scale (f32)";
		case 5: return "inventory";
		case 7: return "base object (ref)";
		case 25: return "promoted: initial data + start cell + u32; extra data";
		case 27: return "leveled inventory: inventory";
		case 28: return "animation (u8, vsval size, bytes)";
		default:
			if (m & (a_actor ? 0xa6061b40u : 0xa6021f40u)) {
				return "extra data list";
			}
			return "";
		}
	}

	Coverage Stats(const SaveFile& a_save)
	{
		Coverage                           c;
		std::map<std::string, std::size_t> err;
		auto                               trim = [](std::string e) {
            if (auto p = e.find(" (at byte"); p != std::string::npos) {
                e.resize(p);
            }
            return e;
		};
		for (const auto& cf : a_save.changeForms) {
			const auto t = static_cast<ChangeFormType>(cf.typeCode());
			if (t == ChangeFormType::REFR || t == ChangeFormType::ACHR) {
				const auto d = DecodeRefr(a_save, cf);
				if (t == ChangeFormType::REFR) {
					++c.refr;
					c.refrComplete += d.complete;
				} else {
					++c.achr;
					c.achrRefrPart += d.decoded;
					if (d.decoded) {
						const auto a = DecodeActor(a_save, cf);
						c.achrValues += a.decoded;
						c.achrTail += a.tail;
						c.achrComplete += a.complete;
						if (!a.tail) {
							++err["ACHR actor part: " + trim(a.error)];
						}
					}
				}
				if (!d.error.empty()) {
					++err[std::string(t == ChangeFormType::REFR ? "REFR: " : "ACHR: ") + trim(d.error)];
				}
			} else if (t == ChangeFormType::NPC_) {
				const auto d = DecodeNpc(a_save, cf);
				++c.npc;
				c.npcComplete += d.complete;
				if (!d.error.empty()) {
					++err["NPC_: " + trim(d.error)];
				}
			}
		}
		for (const auto& [e, n] : err) {
			c.errors.push_back({ e, n });
		}
		std::stable_sort(c.errors.begin(), c.errors.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		return c;
	}
}
