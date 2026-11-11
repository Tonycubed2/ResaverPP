// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Starfield .sfs saves: the "BCPS" container and the SFS_SAVEGAME data inside it.
//
// The layout was read from Starfield.exe 1.16.244.0 (the writers) and checked on four saves (form versions 110 and 153,
// every count/offset/size landing exactly); see docs/CLAUDE-HANDOFF.md, "Starfield", for the layout and what is verified.
// Unknown parts (global data payloads, change form data, the created-ID section) are kept as raw bytes.
#include "Save.h"

#include <fstream>
#include <iterator>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "miniz.h"

namespace ess
{
	bool IsReferenceChangeFormSF(std::uint8_t a_code)
	{
		switch (a_code) {
		case 0: case 1: case 2: case 3: case 4: case 5:  // REFR ACHR PMIS PGRE PBEA PFLA
		case 38: case 39: case 40: case 45: case 53: case 54:  // PHZD PBAR PCON PARW PPLA PEMI
			return true;
		default:
			return false;
		}
	}

	int InitialDataTypeSF(std::uint32_t a_formID, std::uint8_t a_code, std::uint32_t a_changeFlags)
	{
		// 0x141832f00 (references only; the QUST / other-form cases of that function do not concern these)
		if (!IsReferenceChangeFormSF(a_code)) {
			return 0;
		}
		const bool f8000 = (a_changeFlags >> 15) & 1;
		if (static_cast<std::uint32_t>(a_formID + 0x1000000u) <= 0xFFFFFEu && !f8000) {
			return 5;
		}
		if (a_changeFlags & 0x2000008u) {
			return 6;
		}
		if (a_changeFlags & 6u) {
			return 4;
		}
		return f8000 ? 7 : 0;
	}

	std::vector<RefID> BaseObjectsSF(std::uint32_t a_formID, const ChangeForm& a_cf)
	{
		std::vector<RefID> out;
		const auto code = a_cf.typeCode();
		const int  it = InitialDataTypeSF(a_formID, code, a_cf.changeFlags);
		if (!IsReferenceChangeFormSF(code)) {
			return out;
		}
		const std::size_t initRefs = (it == 5 || it == 6) ? 2 : (it == 4 || it == 7) ? 1 : 0;
		if (it == 5 && a_cf.sfRefs.size() > 1) {
			out.push_back(a_cf.sfRefs[1]);
		}
		if ((a_cf.changeFlags & 0x80) && code != 1 && a_cf.sfRefs.size() > initRefs + 1) {
			const auto& b = a_cf.sfRefs[initRefs + 1];
			if (out.empty() || !(out.front() == b)) {
				out.push_back(b);
			}
		}
		return out;
	}

	SfQuestData DecodeQuestSF(const ChangeForm& a_cf)
	{
		SfQuestData q;
		const auto& d = a_cf.data;
		const auto  f = a_cf.changeFlags;
		std::size_t p = 0;
		auto        u32 = [&](std::size_t o) {
			return static_cast<std::uint32_t>(d[o]) | (static_cast<std::uint32_t>(d[o + 1]) << 8) | (static_cast<std::uint32_t>(d[o + 2]) << 16) |
				   (static_cast<std::uint32_t>(d[o + 3]) << 24);
		};
		if (f & 0x1) {
			p += 6;
		}
		if (f & 0x2) {
			if (p + 4 > d.size()) {
				return q;
			}
			q.hasFlags = true;
			q.flagsOffset = p;
			q.questFlags = u32(p);
			p += 4;
		}
		if (f & 0x4) {
			p += 4;
		}
		if (f & 0x80000000u) {
			// vsval (0x141868770): low 2 bits = width 1 / 2 / 4 bytes, value >> 2
			if (p >= d.size()) {
				return q;
			}
			const unsigned w = d[p] & 3;
			const std::size_t len = w == 0 ? 1 : w == 1 ? 2 : 4;
			if (w == 3 || p + len > d.size()) {
				return q;
			}
			std::uint32_t v = 0;
			for (std::size_t k = 0; k < len; ++k) {
				v |= static_cast<std::uint32_t>(d[p + k]) << (8 * k);
			}
			p += len;
			const std::size_t n = v >> 2;
			if (p + 3 * n > d.size()) {
				return q;
			}
			for (std::size_t k = 0; k < n; ++k, p += 3) {
				SfQuestStage st;
				st.index = static_cast<std::uint16_t>(d[p] | (d[p + 1] << 8));
				st.done = d[p + 2] & 1;
				st.offset = p + 2;
				q.stages.push_back(st);
			}
		}
		q.ok = p <= d.size();
		return q;
	}

	const char* ChangeFormTypeNameSF(std::uint8_t a_code)
	{
		// exe 1.16.244: change form type table 0x144a53290 mapped through the form type table 0x1458ceb00
		static const char* names[] = { "REFR", "ACHR", "PMIS", "PGRE", "PBEA", "PFLA", "CELL", "INFO", "QUST", "NPC_", "ACTI", "TACT", "ARMO",
			"BOOK", "CONT", "DOOR", "INGR", "LIGH", "MISC", "STAT", "MSTT", "FURN", "WEAP", "AMMO", "KEYM", "ALCH", "IDLM", "NOTE", "CLAS", "FACT",
			"PACK", "NAVM", "WOOP", "MGEF", "SMQN", "SCEN", "LCTN", "RELA", "PHZD", "PBAR", "PCON", "FLST", "LVLN", "LVLI", "LVSP", "PARW", "ENCH",
			"TERM", "INNR", "AFFE", "LVLP", "LVSC", "LVLB", "PPLA", "PEMI", "PCMT", "PCCN", "PCBN", "GPOF" };
		return a_code < std::size(names) ? names[a_code] : "????";
	}

	bool SaveFile::sfPapyrusRaw() const
	{
		for (const auto& g : table3) {
			if (g.sfRawPapyrus) {
				return true;
			}
		}
		return false;
	}

	namespace
	{
		std::uint64_t Get64(const std::uint8_t* p)
		{
			std::uint64_t v;
			std::memcpy(&v, p, 8);
			return v;
		}
		std::uint32_t Get32(const std::uint8_t* p)
		{
			std::uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
		}
		void Put64(std::vector<std::uint8_t>& a_v, std::size_t a_at, std::uint64_t a_x) { std::memcpy(a_v.data() + a_at, &a_x, 8); }
		void Put32(std::vector<std::uint8_t>& a_v, std::size_t a_at, std::uint32_t a_x) { std::memcpy(a_v.data() + a_at, &a_x, 4); }

		bool PowerOfTwo(std::uint64_t v) { return v && !(v & (v - 1)); }

		// the game's variable-size count (exe 0x14140c7a0): low 2 bits the width (0 u8, 1 u16, 2 u32), value = raw >> 2
		std::uint32_t ReadVsval(Reader& r)
		{
			r.need(1);
			switch (*r.here() & 3) {
			case 0:
				return r.u8() >> 2;
			case 1:
				return r.u16() >> 2;
			case 2:
				return r.u32() >> 2;
			default:
				throw FormatError("invalid count (width 3)", r.pos());
			}
		}
		void WriteVsval(Writer& w, std::uint32_t v)
		{
			if (v < 0x40) {
				w.u8(static_cast<std::uint8_t>(v << 2));
			} else if (v < 0x4000) {
				w.u16(static_cast<std::uint16_t>((v << 2) | 1));
			} else if (v < 0x40000000) {
				w.u32((v << 2) | 2);
			} else {
				throw std::runtime_error("count too large for the save format");
			}
		}

		// global data layouts by type (exe 0x1418236c0 masks, 0x141824a60 dispatcher, 0x141825820 old-style writer)
		GlobalData::SfKind KindOf(std::uint32_t t)
		{
			using K = GlobalData::SfKind;
			if (t == 0 || t == 2) {
				return K::kOffset;
			}
			if (t == 1 || t == 3 || t == 101 || t == 102 || t == 1000) {
				return K::kStrings;
			}
			if (t == 4 || t == 6 || t == 15 || t == 1001) {
				return K::kSized;
			}
			if ((t >= 5 && t <= 16) || t == 100 || t == 103 || t == 105 || t == 106 || (t >= 108 && t <= 111) || (t >= 113 && t <= 124) ||
				(t >= 1002 && t <= 1007)) {
				return K::kSized;
			}
			return K::kTypeOnly;  // (104, 107, 112, 1009, ...: the writer puts the type only)
		}

		GlobalData ReadGlobalSF(Reader& r, std::string* a_papyrusError = nullptr)
		{
			GlobalData g;
			g.type = r.u32();
			g.sfKind = KindOf(g.type);
			switch (g.sfKind) {
			case GlobalData::SfKind::kOffset:
				g.sfOffset = r.u32();
				break;
			case GlobalData::SfKind::kStrings:
				{
					const auto n = ReadVsval(r);
					r.need(std::size_t(n) * 2);
					g.sfStrings.reserve(n);
					for (std::uint32_t i = 0; i < n; ++i) {
						g.sfStrings.push_back(r.u16());
					}
					g.raw = r.bytes(r.u32());
					break;
				}
			case GlobalData::SfKind::kSized:
				g.raw = r.bytes(r.u32());
				break;
			case GlobalData::SfKind::kTypeOnly:
				break;
			}
			if (g.type == 1001) {
				// Papyrus (PapyrusSF.cpp). If it cannot be read, it is kept as raw bytes and written back as it was; an empty
				// stand-in keeps the views that list Papyrus data empty
				g.papyrus = std::make_unique<Papyrus>();
				try {
					Reader pr(g.raw.data(), g.raw.size());
					g.papyrus->readStarfield(pr);
					if (pr.remaining()) {
						throw FormatError(std::to_string(pr.remaining()) + " byte(s) after the script data", pr.pos());
					}
					g.raw.clear();
					g.raw.shrink_to_fit();
				} catch (const std::exception& e) {
					if (a_papyrusError) {
						*a_papyrusError = e.what();
					}
					g.papyrus = std::make_unique<Papyrus>();
					g.sfRawPapyrus = true;
				}
			}
			return g;
		}

		void WriteGlobalSF(Writer& w, const GlobalData& g, std::uint32_t a_offset)
		{
			w.u32(g.type);
			switch (g.sfKind) {
			case GlobalData::SfKind::kOffset:
				w.u32(a_offset);
				break;
			case GlobalData::SfKind::kStrings:
				WriteVsval(w, static_cast<std::uint32_t>(g.sfStrings.size()));
				for (auto s : g.sfStrings) {
					w.u16(s);
				}
				w.u32(static_cast<std::uint32_t>(g.raw.size()));
				w.bytes(g.raw);
				break;
			case GlobalData::SfKind::kSized:
				if (g.papyrus && !g.sfRawPapyrus) {
					Writer pw;
					g.papyrus->write(pw);  // (the Starfield layout: Papyrus::starfield)
					w.u32(static_cast<std::uint32_t>(pw.size()));
					w.bytes(pw.buf);
					break;
				}
				w.u32(static_cast<std::uint32_t>(g.raw.size()));
				w.bytes(g.raw);
				break;
			case GlobalData::SfKind::kTypeOnly:
				break;
			}
		}

		constexpr std::uint32_t kDefaultChangeFlags = 0x80008000u;  // a change form without the flags field (exe 0x14185dff0)

		ChangeForm ReadChangeFormSF(Reader& r, std::uint8_t a_formVersion)
		{
			ChangeForm c;
			const auto at = r.pos();
			c.sfFlags = r.u8();
			if (c.sfFlags & ~0x3B) {
				throw FormatError("change form with unknown header flags " + std::to_string(c.sfFlags), at);
			}
			c.version = (c.sfFlags & 1) ? r.u8() : a_formVersion;
			c.refID = RefID::read(r);
			c.changeFlags = (c.sfFlags & 2) ? r.u32() : kDefaultChangeFlags;
			const auto nRefs = (c.sfFlags & 8) ? ReadVsval(r) : 0;
			const auto nStrings = (c.sfFlags & 0x10) ? ReadVsval(r) : 0;
			const auto nData = (c.sfFlags & 0x20) ? ReadVsval(r) : 0;
			r.need(std::size_t(nRefs) * 3);
			c.sfRefs.reserve(nRefs);
			for (std::uint32_t i = 0; i < nRefs; ++i) {
				c.sfRefs.push_back(RefID::read(r));
			}
			r.need(std::size_t(nStrings) * 2);
			c.sfStrings.reserve(nStrings);
			for (std::uint32_t i = 0; i < nStrings; ++i) {
				c.sfStrings.push_back(r.u16());
			}
			c.data = r.bytes(nData);
			c.length1 = nData;
			return c;
		}

		void WriteChangeFormSF(Writer& w, const ChangeForm& c, std::uint8_t a_formVersion)
		{
			std::uint8_t fl = c.sfFlags;
			if (c.version != a_formVersion) {
				fl |= 1;
			}
			if (c.changeFlags != kDefaultChangeFlags) {
				fl |= 2;
			}
			fl |= c.sfRefs.empty() ? 0 : 8;
			fl |= c.sfStrings.empty() ? 0 : 0x10;
			fl |= c.data.empty() ? 0 : 0x20;
			w.u8(fl);
			if (fl & 1) {
				w.u8(c.version);
			}
			c.refID.write(w);
			if (fl & 2) {
				w.u32(c.changeFlags);
			}
			if (fl & 8) {
				WriteVsval(w, static_cast<std::uint32_t>(c.sfRefs.size()));
			}
			if (fl & 0x10) {
				WriteVsval(w, static_cast<std::uint32_t>(c.sfStrings.size()));
			}
			if (fl & 0x20) {
				WriteVsval(w, static_cast<std::uint32_t>(c.data.size()));
			}
			for (const auto& id : c.sfRefs) {
				id.write(w);
			}
			for (auto s : c.sfStrings) {
				w.u16(s);
			}
			w.bytes(c.data);
		}

		// one plugin entry as the loader reads it (0x141812e80, a_mode as PluginInfo::sfExtraMode)
		PluginInfo::SfExtra ReadPluginSF(Reader& r, std::string& a_name, std::uint8_t a_mode, bool a_achievement)
		{
			PluginInfo::SfExtra e;
			a_name = r.wstring();
			if (a_mode == 2) {
				e.isCreation = r.u8();
			} else if (a_mode == 1) {
				e.isCreation = IsBaseGameFileSF(a_name) ? 0 : 1;
			}
			if (a_mode != 0 && e.isCreation) {
				e.title = r.wstring();
				e.id = r.wstring();
				e.version = r.wstring();
				if (a_mode == 2 || a_achievement) {
					e.achievementFriendly = r.u8();
				}
			}
			return e;
		}

		void WritePluginSF(Writer& w, const std::string& a_name, const PluginInfo::SfExtra& e, std::uint8_t a_mode, bool a_achievement)
		{
			w.wstring(a_name);
			if (a_mode == 2) {
				w.u8(e.isCreation);
			} else if (a_mode == 1 && (e.isCreation != 0) == IsBaseGameFileSF(a_name)) {
				// at these save versions the game decides by the name alone: an entry that does not follow the rule
				// would be misread
				throw std::runtime_error("plugin '" + a_name + "': its Creations details do not follow the game's base file rule");
			}
			if (a_mode != 0 && e.isCreation) {
				w.wstring(e.title);
				w.wstring(e.id);
				w.wstring(e.version);
				if (a_mode == 2 || a_achievement) {
					w.u8(e.achievementFriendly);
				}
			}
		}

		// the plugin info block (exe 0x141812990 writes it, 0x141813760 reads it): u8 n full, u16 n light, [save version
		// 108+: u32 n medium], u16 (the loader skips it)
		bool ReadPluginsSF(const std::uint8_t* a_p, std::size_t a_n, std::uint8_t a_mode, bool a_achievement, bool a_medium, PluginInfo& a_out)
		{
			PluginInfo pi;
			try {
				Reader r(a_p, a_n);
				pi.sfExtraMode = a_mode;
				pi.sfExtras = a_mode != 0;
				pi.sfAchievementByte = a_achievement;
				pi.hasLite = true;
				pi.hasMedium = a_medium;
				const auto nFull = r.u8();
				for (unsigned i = 0; i < nFull; ++i) {
					pi.full.emplace_back();
					pi.sfFull.push_back(ReadPluginSF(r, pi.full.back(), a_mode, a_achievement));
				}
				const auto nLite = r.u16();
				for (unsigned i = 0; i < nLite; ++i) {
					pi.lite.emplace_back();
					pi.sfLite.push_back(ReadPluginSF(r, pi.lite.back(), a_mode, a_achievement));
				}
				if (a_medium) {
					const auto nMedium = r.u32();
					if (nMedium > a_n) {
						return false;
					}
					for (std::uint32_t i = 0; i < nMedium; ++i) {
						pi.medium.emplace_back();
						pi.sfMedium.push_back(ReadPluginSF(r, pi.medium.back(), a_mode, a_achievement));
					}
				}
				if (r.remaining() == 2) {
					pi.sfHasTail = true;
					pi.sfTail = r.u16();
				}
				if (r.remaining() != 0) {
					return false;
				}
			} catch (const FormatError&) {
				return false;
			}
			a_out = std::move(pi);
			return true;
		}
	}

	bool IsBaseGameFileSF(std::string_view a_name)
	{
		// exe 1.16.244: 0x140425850 with its lists 0x142cc7ec0 (0x145973580) and 0x142cc7f20 (0x145931158) and the
		// "prefix-<listed name>" rule 0x142cc7fc0; names compared with _stricmp
		static constexpr std::string_view kListed[] = { "Constellation.esm", "OldMars.esm", "ShatteredSpace.esm", "SFBGS050.esm", "SFBGS003.esm",
			"SFBGS004.esm", "SFBGS006.esm", "SFBGS007.esm", "SFBGS008.esm", "SFBGS00D.esm", "SFBGS047.esm" };
		static constexpr std::string_view kOthers[] = { "Starfield.esm", "TestData.esm", "BlueprintShips", "BlueprintShips-Starfield.esm",
			"BlueprintShips-TestData.esm" };
		auto same = [](std::string_view a, std::string_view b) {
			if (a.size() != b.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a.size(); ++i) {
				const auto x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
				if ((x >= 'A' && x <= 'Z' ? x + 32 : x) != (y >= 'A' && y <= 'Z' ? y + 32 : y)) {
					return false;
				}
			}
			return true;
		};
		auto listed = [&](std::string_view n) {
			return std::any_of(std::begin(kListed), std::end(kListed), [&](std::string_view k) { return same(n, k); });
		};
		if (a_name.empty()) {
			return false;
		}
		if (listed(a_name) || std::any_of(std::begin(kOthers), std::end(kOthers), [&](std::string_view k) { return same(a_name, k); })) {
			return true;
		}
		const auto dash = a_name.find('-');
		return dash != std::string_view::npos && listed(a_name.substr(dash + 1));
	}

	// ---- container --------------------------------------------------------------------------------------------

	std::vector<std::uint8_t> UnpackBCPS(const std::vector<std::uint8_t>& a_file, SfContainer& a_info)
	{
		// exe 0x1428ef9c0 (reader checks), 0x141859ba0 (a chunk as long as its block is stored raw)
		if (a_file.size() < 0x48 || std::memcmp(a_file.data(), "BCPS", 4) != 0) {
			throw FormatError("not a Starfield save container (no BCPS)", 0);
		}
		const auto* d = a_file.data();
		if (Get32(d + 4) != 1) {
			throw FormatError("unsupported BCPS container version " + std::to_string(Get32(d + 4)), 4);
		}
		const auto table = Get64(d + 0x08);
		a_info.unk10 = Get64(d + 0x10);
		const auto dataOffset = Get64(d + 0x18);
		const auto total = Get64(d + 0x20);
		a_info.maxSize = Get64(d + 0x28);
		a_info.chunkSize = Get64(d + 0x30);
		a_info.alignment = Get64(d + 0x38);
		a_info.level = d[0x40];
		std::memcpy(a_info.pad, d + 0x41, 3);
		std::memcpy(a_info.codec, d + 0x44, 4);
		a_info.present = true;
		const auto a = a_info.alignment;
		if (!PowerOfTwo(a) || a_info.unk10 % a || dataOffset % a || !PowerOfTwo(a_info.chunkSize) || (a_info.chunkSize & 0xFFFF) ||
			a_info.maxSize < a_info.chunkSize) {
			throw FormatError("BCPS container header is not valid", 0x10);
		}
		if (std::memcmp(a_info.codec, "ZIP ", 4) != 0) {
			throw FormatError("BCPS container codec '" + std::string(a_info.codec, 4) + "' is not supported (only ZIP)", 0x44);
		}
		if (total > (std::uint64_t(1) << 32)) {
			throw FormatError("BCPS container too large", 0x20);
		}
		const auto n = static_cast<std::size_t>((total + a_info.chunkSize - 1) / a_info.chunkSize);
		if (table + 4 * n > a_file.size()) {
			throw FormatError("BCPS chunk table past the end of the file", table);
		}
		a_info.chunks = n;
		a_info.storedChunks = 0;
		std::vector<std::uint8_t> out(static_cast<std::size_t>(total));
		std::uint64_t             p = dataOffset;
		for (std::size_t i = 0; i < n; ++i) {
			const std::uint32_t s = Get32(d + table + 4 * i);
			const auto          ulen = static_cast<std::size_t>(std::min<std::uint64_t>(a_info.chunkSize, total - i * a_info.chunkSize));
			if (s == 0 || s > ulen || p + s > a_file.size()) {
				throw FormatError("BCPS chunk " + std::to_string(i) + " has an invalid size", table + 4 * i);
			}
			auto* dst = out.data() + i * a_info.chunkSize;
			if (s == ulen) {
				std::memcpy(dst, d + p, s);
				++a_info.storedChunks;
			} else {
				mz_ulong len = static_cast<mz_ulong>(ulen);
				if (mz_uncompress(dst, &len, d + p, s) != MZ_OK || len != ulen) {
					throw FormatError("BCPS chunk " + std::to_string(i) + " does not unpack (zlib)", p);
				}
			}
			p += (s + a - 1) / a * a;
		}
		return out;
	}

	std::vector<std::uint8_t> PackBCPS(const std::vector<std::uint8_t>& a_inner, const SfContainer& a_info)
	{
		// the game's writer (0x1428eef80 / 0x1428f0650): header, chunk size table, then each chunk at the next multiple
		// of the alignment, the gaps filled with "padding\0"; no checksum anywhere
		const std::size_t cs = static_cast<std::size_t>(a_info.chunkSize);
		const std::size_t a = static_cast<std::size_t>(a_info.alignment);
		const std::size_t n = (a_inner.size() + cs - 1) / cs;
		if (0x48 + 4 * n > 0x10000) {
			// the reader takes the chunk table from the first 64 KB it reads (0x1428ef9c0)
			throw std::runtime_error("the save is too large for the Starfield container");
		}
		std::vector<std::vector<std::uint8_t>> packed(n);
		{
			const unsigned          workers = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
			std::vector<std::thread> pool;
			std::exception_ptr       failed;
			std::mutex               m;
			for (unsigned t = 0; t < workers; ++t) {
				pool.emplace_back([&, t] {
					for (std::size_t i = t; i < n; i += workers) {
						const auto* src = a_inner.data() + i * cs;
						const auto  ulen = std::min(cs, a_inner.size() - i * cs);
						mz_ulong    len = mz_compressBound(static_cast<mz_ulong>(ulen));
						std::vector<std::uint8_t> c(len);
						if (mz_compress2(c.data(), &len, src, static_cast<mz_ulong>(ulen), a_info.level) != MZ_OK) {
							std::lock_guard l(m);
							failed = std::make_exception_ptr(std::runtime_error("zlib compression failed"));
							return;
						}
						if (len >= ulen) {
							c.assign(src, src + ulen);  // stored raw: its size equals the block's
						} else {
							c.resize(len);
						}
						packed[i] = std::move(c);
					}
				});
			}
			for (auto& th : pool) {
				th.join();
			}
			if (failed) {
				std::rethrow_exception(failed);
			}
		}
		auto alignUp = [&](std::size_t v) { return (v + a - 1) / a * a; };
		static const char kPad[8] = { 'p', 'a', 'd', 'd', 'i', 'n', 'g', 0 };
		auto pad = [&](std::vector<std::uint8_t>& v, std::size_t to) {
			for (std::size_t k = 0; v.size() < to; ++k) {
				v.push_back(static_cast<std::uint8_t>(kPad[k % 8]));
			}
		};
		std::vector<std::uint8_t> out(0x48, 0);
		std::memcpy(out.data(), "BCPS", 4);
		Put32(out, 4, 1);
		Put64(out, 0x08, 0x48);
		Put64(out, 0x10, a_info.unk10);
		const std::size_t dataOffset = alignUp(0x48 + 4 * n);
		Put64(out, 0x18, dataOffset);
		Put64(out, 0x20, a_inner.size());
		Put64(out, 0x28, a_info.maxSize);
		Put64(out, 0x30, a_info.chunkSize);
		Put64(out, 0x38, a_info.alignment);
		out[0x40] = a_info.level;
		std::memcpy(out.data() + 0x41, a_info.pad, 3);
		std::memcpy(out.data() + 0x44, a_info.codec, 4);
		for (const auto& c : packed) {
			const auto s = static_cast<std::uint32_t>(c.size());
			out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&s), reinterpret_cast<const std::uint8_t*>(&s) + 4);
		}
		pad(out, dataOffset);
		for (const auto& c : packed) {
			out.insert(out.end(), c.begin(), c.end());
			pad(out, alignUp(out.size()));
		}
		return out;
	}

	// ---- reading ----------------------------------------------------------------------------------------------

	std::unique_ptr<SaveFile> SaveFile::loadSF(const std::filesystem::path& a_path, const std::vector<std::uint8_t>& a_file, const char*& a_stage)
	{
		auto save = std::make_unique<SaveFile>();
		save->path = a_path;
		std::vector<std::uint8_t> unpacked;
		const std::vector<std::uint8_t>* innerP = &a_file;
		if (a_file.size() >= 4 && std::memcmp(a_file.data(), "BCPS", 4) == 0) {
			a_stage = "the Starfield container (unpacking it)";
			unpacked = UnpackBCPS(a_file, save->sfContainer);
			innerP = &unpacked;
		}
		const auto& in = *innerP;
		save->originalBodySize = in.size();
		Reader r(in.data(), in.size());
		a_stage = "the header";
		save->header.read(r);
		if (!save->isSF()) {
			throw FormatError("the container does not hold a Starfield save", 0);
		}

		a_stage = "the plugin list";
		save->formVersion = r.u8();
		save->gameVersion = r.wstring();
		save->sfCreatedVersion = r.wstring();
		{
			const auto infoSize = r.u32();
			r.need(infoSize);
			// the layout follows the header's save version (the loader keeps it at +0x110 of its stream: 0x141830755),
			// not the form version (equal in every save seen). 0x141812e80: names only before 120; 120..139 no flag
			// byte, the details for files that are not base game files (the achievement byte from 121); 140+ a flag
			// byte. 0x141813760: the medium count from 108. If that does not fill the block, the other layouts are tried
			const unsigned v = save->sfVersion();
			const std::uint8_t mode = v >= 140 ? 2 : v >= 120 ? 1 : 0;
			const bool         ach = v >= 121 || v < 120;
			const bool         med = v >= 108;
			struct Try
			{
				std::uint8_t mode;
				bool         ach, med;
			};
			const Try tries[] = { { mode, ach, med }, { 2, true, true }, { 1, true, true }, { 1, false, true }, { 0, true, true }, { 0, true, false } };
			bool      ok = false;
			for (const auto& t : tries) {
				if (ReadPluginsSF(r.here(), infoSize, t.mode, t.ach, t.med, save->plugins)) {
					ok = true;
					if (&t != &tries[0]) {
						save->problems.push_back("the plugin list is not laid out as save version " + std::to_string(v) + " says (read another way)");
					}
					break;
				}
			}
			if (!ok) {
				throw FormatError("the plugin list does not match its size", r.pos());
			}
			r.seek(r.pos() + infoSize);
		}

		a_stage = "the file location table";
		auto& t = save->flt;
		t.formIDArrayCountOffset = r.u32();
		t.unknownTable3Offset = r.u32();
		t.table1Offset = r.u32();
		t.table2Offset = r.u32();
		t.changeFormsOffset = r.u32();
		t.table3Offset = r.u32();
		t.table1Count = r.u32();
		t.table2Count = r.u32();
		t.table3Count = r.u32();  // (true counts, as Fallout 4)
		t.changeFormCount = r.u32();
		for (auto& u : t.unused) {
			u = r.u32();
		}
		const std::uint32_t runsOffset = t.unused[1];

		// each section must start where the table says, right after the one before
		auto expect = [&](std::uint32_t a_offset, const char* a_what) {
			if (r.pos() != a_offset) {
				save->problems.push_back(std::string(a_what) + " does not start where the previous section ends (" +
										 std::to_string(static_cast<long long>(a_offset) - static_cast<long long>(r.pos())) + " byte(s) unaccounted for)");
				r.seek(a_offset);
			}
		};
		expect(t.table1Offset, "global data table 1");
		a_stage = "global data table 1";
		for (std::uint32_t i = 0; i < t.table1Count; ++i) {
			save->table1.push_back(ReadGlobalSF(r));
		}
		expect(t.table2Offset, "global data table 2");
		a_stage = "global data table 2";
		for (std::uint32_t i = 0; i < t.table2Count; ++i) {
			save->table2.push_back(ReadGlobalSF(r));
		}
		expect(t.changeFormsOffset, "the change forms");
		a_stage = "the change forms";
		save->changeForms.reserve(t.changeFormCount);
		for (std::uint32_t i = 0; i < t.changeFormCount; ++i) {
			save->changeForms.push_back(ReadChangeFormSF(r, save->sfVersion()));
		}
		expect(t.table3Offset, "global data table 3");
		a_stage = "global data table 3 (Papyrus)";
		for (std::uint32_t i = 0; i < t.table3Count; ++i) {
			save->table3.push_back(ReadGlobalSF(r, &save->sfPapyrusError));
		}
		expect(t.formIDArrayCountOffset, "the form ID array");
		a_stage = "the form ID array";
		{
			const auto n = r.u32();
			r.need(std::size_t(n) * 4);
			save->formIDs.reserve(n);
			for (std::uint32_t i = 0; i < n; ++i) {
				save->formIDs.push_back(r.u32());
			}
		}
		expect(t.unknownTable3Offset, "unknown table 3");
		a_stage = "unknown table 3";
		save->sfUnknownTable3 = r.bytes(r.u32());
		expect(runsOffset, "the change form type list");
		a_stage = "the change form type list";
		{
			const auto runs = ReadVsval(r);
			std::size_t i = 0;
			for (std::uint32_t k = 0; k < runs; ++k) {
				const auto type = r.u8();
				const auto count = ReadVsval(r);
				if (type >= 59) {  // the loader maps codes below 0x3b through 0x144a53290, others to no type (0x141816231)
					save->problems.push_back("change form type " + std::to_string(type) + " is out of range");
				}
				if (count > save->changeForms.size() - i) {
					throw FormatError("the change form type list covers more forms than the save has", r.pos());
				}
				for (std::uint32_t j = 0; j < count; ++j) {
					save->changeForms[i++].typeField = type;
				}
			}
			if (i != save->changeForms.size()) {
				throw FormatError("the change form type list covers " + std::to_string(i) + " of " + std::to_string(save->changeForms.size()) + " forms",
					r.pos());
			}
		}
		const GlobalData* g0 = nullptr;
		const GlobalData* g2 = nullptr;
		for (const auto& g : save->table1) {
			if (g.sfKind == GlobalData::SfKind::kOffset) {
				(g.type == 0 ? g0 : g2) = &g;
			}
		}
		if (!g0 || !g2) {
			throw FormatError("global data types 0 and 2 (string table and created-ID section) are missing", r.pos());
		}
		expect(g0->sfOffset, "the string table");
		a_stage = "the string table";
		{
			const auto n = r.u16();
			save->sfStrings.reserve(n);
			for (unsigned i = 0; i < n; ++i) {
				save->sfStrings.push_back(r.wstring());
			}
		}
		expect(g2->sfOffset, "the created-ID section");
		a_stage = "the created-ID section";
		save->sfCreated = r.bytes(r.remaining());

		a_stage = "the checks after reading";
		const auto nStrings = save->sfStrings.size();
		std::size_t badStrings = 0;
		for (const auto& c : save->changeForms) {
			for (auto s : c.sfStrings) {
				badStrings += s >= nStrings;
			}
		}
		for (const auto* tb : { &save->table1, &save->table2, &save->table3 }) {
			for (const auto& g : *tb) {
				for (auto s : g.sfStrings) {
					badStrings += s >= nStrings;
				}
			}
		}
		if (badStrings) {
			save->problems.push_back(std::to_string(badStrings) + " string index(es) past the string table");
		}
		// a .sfse next to it (SFSE writes none up to 0.2.21; kept as it is if one is there: Cosave.h)
		{
			std::error_code ec;
			auto            co = a_path;
			co.replace_extension(".sfse");
			if (std::filesystem::is_regular_file(co, ec)) {
				std::ifstream f(co, std::ios::binary);
				save->cosave.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
			}
		}
		save->reindexChangeForms();
		if (auto* p = save->papyrus(); p && !save->sfPapyrusRaw()) {
			p->reindex();
			for (auto& msg : p->check()) {
				save->problems.push_back("Papyrus: " + msg);
			}
		}
		return save;
	}

	// ---- writing ----------------------------------------------------------------------------------------------

	std::vector<std::uint8_t> SaveFile::serializeSF() const
	{
		auto inner = buildBody();
		return sfContainer.present ? PackBCPS(inner, sfContainer) : inner;
	}

	namespace
	{
		std::vector<std::uint8_t> BuildSF(const SaveFile& s)
		{
			Writer w;
			s.header.write(w);
			w.u8(s.formVersion);
			w.wstring(s.gameVersion);
			w.wstring(s.sfCreatedVersion);
			{
				const auto& pl = s.plugins;
				auto        extra = [&](const std::vector<PluginInfo::SfExtra>& a_v, std::size_t i) {
                    static const PluginInfo::SfExtra none;
                    return i < a_v.size() ? a_v[i] : none;
				};
				if (pl.full.size() > 0xFF || pl.lite.size() > 0xFFFF) {
					throw std::runtime_error("too many plugins for the save format");
				}
				Writer pw;
				pw.u8(static_cast<std::uint8_t>(pl.full.size()));
				for (std::size_t i = 0; i < pl.full.size(); ++i) {
					WritePluginSF(pw, pl.full[i], extra(pl.sfFull, i), pl.sfExtraMode, pl.sfAchievementByte);
				}
				pw.u16(static_cast<std::uint16_t>(pl.lite.size()));
				for (std::size_t i = 0; i < pl.lite.size(); ++i) {
					WritePluginSF(pw, pl.lite[i], extra(pl.sfLite, i), pl.sfExtraMode, pl.sfAchievementByte);
				}
				if (pl.hasMedium) {
					pw.u32(static_cast<std::uint32_t>(pl.medium.size()));
					for (std::size_t i = 0; i < pl.medium.size(); ++i) {
						WritePluginSF(pw, pl.medium[i], extra(pl.sfMedium, i), pl.sfExtraMode, pl.sfAchievementByte);
					}
				} else if (!pl.medium.empty()) {
					throw std::runtime_error("medium plugins cannot be written to a save of this version");
				}
				if (pl.sfHasTail) {
					pw.u16(pl.sfTail);
				}
				w.u32(static_cast<std::uint32_t>(pw.size()));
				w.bytes(pw.buf);
			}
			const std::size_t fltAt = w.size();
			for (int i = 0; i < 25; ++i) {
				w.u32(0);  // patched below
			}
			// types 0 and 2 hold the offsets of the string table and the created-ID section: patched at the end
			std::vector<std::pair<std::size_t, std::uint32_t>> offsetSlots;  // (position of the u32, type)
			auto table = [&](const std::vector<GlobalData>& a_t) {
				for (const auto& g : a_t) {
					if (g.sfKind == GlobalData::SfKind::kOffset) {
						offsetSlots.emplace_back(w.size() + 4, g.type);
					}
					WriteGlobalSF(w, g, 0);
				}
			};
			const auto t1 = static_cast<std::uint32_t>(w.size());
			table(s.table1);
			const auto t2 = static_cast<std::uint32_t>(w.size());
			table(s.table2);
			const auto cf = static_cast<std::uint32_t>(w.size());
			for (const auto& c : s.changeForms) {
				WriteChangeFormSF(w, c, s.sfVersion());
			}
			const auto t3 = static_cast<std::uint32_t>(w.size());
			table(s.table3);
			const auto fid = static_cast<std::uint32_t>(w.size());
			w.u32(static_cast<std::uint32_t>(s.formIDs.size()));
			for (auto id : s.formIDs) {
				w.u32(id);
			}
			const auto u3 = static_cast<std::uint32_t>(w.size());
			w.u32(static_cast<std::uint32_t>(s.sfUnknownTable3.size()));
			w.bytes(s.sfUnknownTable3);
			// the type list: runs of forms of one type, in file order
			const auto runsAt = static_cast<std::uint32_t>(w.size());
			{
				std::vector<std::pair<std::uint8_t, std::uint32_t>> runs;
				for (const auto& c : s.changeForms) {
					if (runs.empty() || runs.back().first != c.typeField) {
						runs.emplace_back(c.typeField, 0);
					}
					++runs.back().second;
				}
				WriteVsval(w, static_cast<std::uint32_t>(runs.size()));
				for (const auto& [type, count] : runs) {
					w.u8(type);
					WriteVsval(w, count);
				}
			}
			const auto strAt = static_cast<std::uint32_t>(w.size());
			if (s.sfStrings.size() > 0xFFFF) {
				throw std::runtime_error("too many strings for the save format");
			}
			w.u16(static_cast<std::uint16_t>(s.sfStrings.size()));
			for (const auto& str : s.sfStrings) {
				w.wstring(str);
			}
			const auto createdAt = static_cast<std::uint32_t>(w.size());
			w.bytes(s.sfCreated);

			for (const auto& [at, type] : offsetSlots) {
				w.patch32(at, type == 0 ? strAt : createdAt);
			}
			const std::uint32_t vals[10] = { fid, u3, t1, t2, cf, t3, static_cast<std::uint32_t>(s.table1.size()),
				static_cast<std::uint32_t>(s.table2.size()), static_cast<std::uint32_t>(s.table3.size()),
				static_cast<std::uint32_t>(s.changeForms.size()) };
			for (int i = 0; i < 10; ++i) {
				w.patch32(fltAt + 4 * i, vals[i]);
			}
			// FLT[10]: the loader skips that many bytes (SEEK_CUR) after the change forms before table 3 (0x141817607); table 3
			// is written right after them, so 0 (the game's writer never sets it). FLT[11]: the type list
			for (int i = 0; i < 15; ++i) {
				w.patch32(fltAt + 40 + 4 * i, i == 0 ? 0 : i == 1 ? runsAt : s.flt.unused[i]);
			}
			return std::move(w.buf);
		}
	}

	std::vector<std::uint8_t> BuildBodySF(const SaveFile& a_save) { return BuildSF(a_save); }
}
