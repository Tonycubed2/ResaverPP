// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Merged patch. The approach follows Mator Smash (copy the winning override, then apply each
// override's own changes measured against the version its plugin was built on) and xEdit's
// required-masters rule; record layouts follow xEdit's Skyrim definitions (wbDefinitionsTES5).
#include "MergedPatch.h"

#include "Bsa.h"
#include "ConditionFunctions.h"
#include "ConditionFunctionsFO4.h"
#include "ConditionFunctionsSF.h"
#include "Save.h"  // DecompressZlib
#include "ScriptSources.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace ess::merge
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}
		std::uint16_t U16(const std::uint8_t* p)
		{
			std::uint16_t v;
			std::memcpy(&v, p, 2);
			return v;
		}
		std::uint32_t U32(const std::uint8_t* p)
		{
			std::uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
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
		void PutSig(std::vector<std::uint8_t>& o, const char* s) { o.insert(o.end(), s, s + 4); }
		bool Is(const char* a, const char* b) { return std::memcmp(a, b, 4) == 0; }
		std::string Sig(const char* s) { return std::string(s, 4); }
		std::string Hex(std::uint32_t v, int w = 6)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "%0*X", w, v);
			return b;
		}

		std::vector<Sub> ParseSubs(const std::uint8_t* d, std::size_t n)
		{
			std::vector<Sub> out;
			std::uint32_t    big = 0;
			std::size_t      p = 0;
			while (p + 6 <= n) {
				Sub s;
				std::memcpy(s.sig, d + p, 4);
				std::uint32_t len = U16(d + p + 4);
				if (big) {
					len = big;
					big = 0;
				}
				p += 6;
				if (p + len > n) {
					throw std::runtime_error("subrecord " + Sig(s.sig) + " runs past the end of its record");
				}
				if (Is(s.sig, "XXXX") && len == 4) {
					big = U32(d + p);
				} else {
					s.data.assign(d + p, d + p + len);
					out.push_back(std::move(s));
				}
				p += len;
			}
			return out;
		}

		void WriteSubs(std::vector<std::uint8_t>& o, const std::vector<Sub>& subs)
		{
			for (const auto& s : subs) {
				if (s.data.size() > 0xFFFF) {
					PutSig(o, "XXXX");
					Put16(o, 4);
					Put32(o, static_cast<std::uint32_t>(s.data.size()));
					PutSig(o, s.sig);
					Put16(o, 0);
				} else {
					PutSig(o, s.sig);
					Put16(o, static_cast<std::uint16_t>(s.data.size()));
				}
				o.insert(o.end(), s.data.begin(), s.data.end());
			}
		}

		// Skyrim's top group order (as the Creation Kit writes it)
		const char* const kGroupOrder[] = { "GMST", "KYWD", "LCRT", "AACT", "TXST", "GLOB", "CLAS", "FACT", "HDPT", "HAIR", "EYES", "RACE", "SOUN",
			"ASPC", "MGEF", "SCPT", "LTEX", "ENCH", "SPEL", "SCRL", "ACTI", "TACT", "ARMO", "BOOK", "CONT", "DOOR", "INGR", "LIGH", "MISC", "APPA",
			"STAT", "SCOL", "MSTT", "PWAT", "GRAS", "TREE", "CLDC", "FLOR", "FURN", "WEAP", "AMMO", "NPC_", "LVLN", "KEYM", "ALCH", "IDLM", "COBJ",
			"PROJ", "HAZD", "SLGM", "LVLI", "WTHR", "CLMT", "SPGD", "RFCT", "REGN", "NAVI", "CELL", "WRLD", "DIAL", "QUST", "IDLE", "PACK", "CSTY",
			"LSCR", "LVSP", "ANIO", "WATR", "EFSH", "EXPL", "DEBR", "IMGS", "IMAD", "FLST", "PERK", "BPTD", "ADDN", "AVIF", "CAMS", "CPTH", "VTYP",
			"MATT", "IPCT", "IPDS", "ARMA", "ECZN", "LCTN", "MESG", "RGDL", "DOBJ", "LGTM", "MUSC", "FSTP", "FSTS", "SMBN", "SMQN", "SMEN", "DLBR",
			"MUST", "DLVW", "WOOP", "SHOU", "EQUP", "RELA", "SCEN", "ASTP", "OTFT", "ARTO", "MATO", "MOVT", "SNDR", "DUAL", "SNCT", "SOPM", "COLL",
			"CLFM", "REVB" };
		// Fallout 4's top group order (xEdit wbDefinitionsFO4, AddGroupOrder)
		const char* const kGroupOrderFO4[] = { "GMST", "KYWD", "LCRT", "AACT", "TRNS", "CMPO", "TXST", "MICN", "GLOB", "DMGT", "CLAS", "FACT", "HDPT",
			"EYES", "RACE", "SOUN", "ASPC", "SKIL", "MGEF", "SCPT", "LTEX", "ENCH", "SPEL", "SCRL", "ACTI", "TACT", "ARMO", "BOOK", "CONT", "DOOR",
			"INGR", "LIGH", "MISC", "STAT", "SCOL", "MSTT", "GRAS", "TREE", "FLOR", "FURN", "WEAP", "AMMO", "NPC_", "PLYR", "LVLN", "KEYM", "ALCH",
			"IDLM", "NOTE", "PROJ", "HAZD", "BNDS", "SLGM", "TERM", "LVLI", "WTHR", "CLMT", "SPGD", "RFCT", "REGN", "NAVI", "CELL", "REFR", "ACHR",
			"PMIS", "PARW", "PGRE", "PBEA", "PFLA", "PCON", "PBAR", "PHZD", "WRLD", "LAND", "NAVM", "TLOD", "DIAL", "INFO", "QUST", "IDLE", "PACK",
			"CSTY", "LSCR", "LVSP", "ANIO", "WATR", "EFSH", "TOFT", "EXPL", "DEBR", "IMGS", "IMAD", "FLST", "PERK", "BPTD", "ADDN", "AVIF", "CAMS",
			"CPTH", "VTYP", "MATT", "IPCT", "IPDS", "ARMA", "ECZN", "LCTN", "MESG", "RGDL", "DOBJ", "DFOB", "LGTM", "MUSC", "FSTP", "FSTS", "SMBN",
			"SMQN", "SMEN", "DLBR", "MUST", "DLVW", "WOOP", "SHOU", "EQUP", "RELA", "SCEN", "ASTP", "OTFT", "ARTO", "MATO", "MOVT", "SNDR", "DUAL",
			"SNCT", "SOPM", "COLL", "CLFM", "REVB", "PKIN", "RFGP", "AMDL", "LAYR", "COBJ", "OMOD", "MSWP", "ZOOM", "INNR", "KSSM", "AECH", "SCCO",
			"AORU", "SCSN", "STAG", "NOCM", "LENS", "LSPR", "GDRY", "OVIS" };
		// Starfield: set by Build for the build's thread. The functions that take a_fo4 (layouts, lists, order, groups, the
		// writer) use Starfield's own definitions when it is set (xEdit wbDefinitionsSF1, dev-4.1.6).
		thread_local bool t_sf = false;
		// Starfield's top group order (xEdit wbDefinitionsSF1, AddGroupOrder)
		const char* const kGroupOrderSF[] = { "GMST", "KYWD", "FFKW", "LCRT", "AACT", "TRNS", "TXST", "GLOB", "DMGT", "CLAS", "FACT", "AFFE", "HDPT",
			"EYES", "RACE", "SOUN", "SECH", "ASPC", "AOPF", "SKIL", "MGEF", "SCPT", "LTEX", "PDCL", "ENCH", "SPEL", "SCRL", "ACTI", "TACT", "CURV",
			"CUR3", "ARMO", "BOOK", "CONT", "DOOR", "INGR", "LIGH", "MISC", "STAT", "SCOL", "PKIN", "MSTT", "GRAS", "FLOR", "FURN", "WEAP", "AMMO",
			"NPC_", "PLYR", "LVLN", "LVLP", "KEYM", "ALCH", "IDLM", "BMMO", "NOTE", "PROJ", "HAZD", "BNDS", "SLGM", "TERM", "LVLI", "GBFT", "GBFM",
			"LVLB", "WTHR", "WTHS", "CLMT", "SPGD", "REGN", "NAVI", "CELL", "REFR", "ACHR", "PMIS", "PARW", "PGRE", "PBEA", "PFLA", "PCON", "PPLA",
			"PBAR", "PEMI", "PHZD", "WRLD", "NAVM", "TLOD", "DIAL", "INFO", "QUST", "IDLE", "PACK", "CSTY", "LSCR", "LVSP", "ANIO", "WATR", "EFSH",
			"TOFT", "EXPL", "DEBR", "IMGS", "IMAD", "FLST", "PERK", "BPTD", "ADDN", "AVIF", "CAMS", "CPTH", "VTYP", "MATT", "IPCT", "IPDS", "ARMA",
			"LCTN", "MESG", "RGDL", "DOBJ", "DFOB", "LGTM", "MUSC", "FSTP", "FSTS", "SMBN", "SMQN", "SMEN", "DLBR", "MUST", "DLVW", "WOOP", "SHOU",
			"EQUP", "RELA", "SCEN", "ASTP", "OTFT", "ARTO", "MOVT", "DUAL", "COLL", "CLFM", "REVB", "RFGP", "AMDL", "AAMD", "MAAM", "LAYR", "COBJ",
			"OMOD", "ZOOM", "INNR", "KSSM", "SCCO", "AORU", "STAG", "IRES", "BIOM", "NOCM", "LENS", "LSPR", "OVIS", "DLYR", "STND", "STMP", "GCVR",
			"MRPH", "TRAV", "RSGD", "OSWP", "ATMO", "LVSC", "SPCH", "AAPD", "VOLI", "SFBK", "SFPC", "SFPT" };
		int GroupRank(const char* sig, bool a_fo4 = false)
		{
			if (t_sf) {
				for (int i = 0; i < static_cast<int>(std::size(kGroupOrderSF)); ++i) {
					if (Is(sig, kGroupOrderSF[i])) {
						return i;
					}
				}
				return 1000;
			}
			if (a_fo4) {
				for (int i = 0; i < static_cast<int>(std::size(kGroupOrderFO4)); ++i) {
					if (Is(sig, kGroupOrderFO4[i])) {
						return i;
					}
				}
				return 1000;
			}
			for (int i = 0; i < static_cast<int>(std::size(kGroupOrder)); ++i) {
				if (Is(sig, kGroupOrder[i])) {
					return i;
				}
			}
			return 1000;
		}
	}

	// ======================================================================================
	// plugin files
	// ======================================================================================

	PluginFile ReadPlugin(const std::filesystem::path& a_file, const std::vector<std::string>& a_types)
	{
		PluginFile    pf;
		std::ifstream f(a_file, std::ios::binary);
		if (!f) {
			throw std::runtime_error("cannot open the file");
		}
		std::uint8_t h[24];
		if (!f.read(reinterpret_cast<char*>(h), 24) || std::memcmp(h, "TES4", 4) != 0) {
			throw std::runtime_error("not a Skyrim plugin");
		}
		pf.flags = U32(h + 8);
		{
			std::vector<std::uint8_t> d(U32(h + 4));
			if (!f.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()))) {
				throw std::runtime_error("the plugin header is cut off");
			}
			for (const auto& s : ParseSubs(d.data(), d.size())) {
				if (Is(s.sig, "MAST")) {
					std::string m(s.data.begin(), s.data.end());
					while (!m.empty() && m.back() == '\0') {
						m.pop_back();
					}
					pf.masters.push_back(m);
				}
			}
		}
		std::vector<std::uint8_t> grp, inflated;
		while (f.read(reinterpret_cast<char*>(h), 24)) {
			if (std::memcmp(h, "GRUP", 4) != 0) {
				throw std::runtime_error("a record outside of any group");
			}
			const auto size = U32(h + 4);
			if (size < 24) {
				throw std::runtime_error("a group with a bad size");
			}
			bool want = a_types.empty();
			for (const auto& t : a_types) {
				want = want || std::memcmp(h + 8, t.data(), 4) == 0;
			}
			if (!want) {
				f.seekg(size - 24, std::ios::cur);
				continue;
			}
			if (std::memcmp(h + 8, "CELL", 4) == 0 || std::memcmp(h + 8, "WRLD", 4) == 0) {
				// cells and worldspaces: walk the nested groups, keep the CELL and WRLD records with the groups they sit
				// in, and skip everything inside a cell (placed objects, landscape, navmeshes: types 6 to 10)
				struct Open
				{
					std::int32_t         type;
					std::uint32_t        label;
					std::streamoff       end;
				};
				std::vector<Open> stack;
				const auto        topEnd = static_cast<std::streamoff>(f.tellg()) + static_cast<std::streamoff>(size - 24);
				std::vector<std::uint8_t> data;
				while (static_cast<std::streamoff>(f.tellg()) < topEnd) {
					while (!stack.empty() && static_cast<std::streamoff>(f.tellg()) >= stack.back().end) {
						stack.pop_back();
					}
					std::uint8_t r[24];
					if (!f.read(reinterpret_cast<char*>(r), 24)) {
						throw std::runtime_error("a cell group is cut off");
					}
					if (std::memcmp(r, "GRUP", 4) == 0) {
						const auto gs = U32(r + 4);
						const auto type = static_cast<std::int32_t>(U32(r + 12));
						if (gs < 24) {
							throw std::runtime_error("a group with a bad size");
						}
						if (type >= 6) {
							f.seekg(gs - 24, std::ios::cur);
							continue;
						}
						stack.push_back({ type, U32(r + 8), static_cast<std::streamoff>(f.tellg()) + static_cast<std::streamoff>(gs - 24) });
						continue;
					}
					const auto dataSize = U32(r + 4);
					if (std::memcmp(r, "CELL", 4) != 0 && std::memcmp(r, "WRLD", 4) != 0) {
						f.seekg(dataSize, std::ios::cur);
						continue;
					}
					Record rec;
					std::memcpy(rec.sig, r, 4);
					rec.flags = U32(r + 8);
					rec.formID = U32(r + 12);
					rec.version = U16(r + 20);
					data.resize(dataSize);
					if (!f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(dataSize))) {
						throw std::runtime_error("a cell record is cut off");
					}
					const std::uint8_t* d = data.data();
					std::size_t         n = dataSize;
					if (rec.flags & 0x40000) {
						if (n < 4) {
							throw std::runtime_error("a compressed record is too short");
						}
						inflated = DecompressZlib(d + 4, n - 4, U32(d));
						d = inflated.data();
						n = inflated.size();
						rec.flags &= ~0x40000u;
					}
					rec.subs = ParseSubs(d, n);
					for (const auto& o : stack) {
						rec.groups.push_back({ o.type, o.label });
					}
					pf.records.push_back(std::move(rec));
				}
				f.seekg(topEnd);
				continue;
			}
			grp.resize(size - 24);
			if (!f.read(reinterpret_cast<char*>(grp.data()), static_cast<std::streamsize>(grp.size()))) {
				throw std::runtime_error("a group is cut off");
			}
			std::size_t p = 0;
			while (p + 24 <= grp.size()) {
				const auto* r = grp.data() + p;
				if (std::memcmp(r, "GRUP", 4) == 0) {  // a nested group (cells, topics...): not read here
					const auto gs = U32(r + 4);
					p += gs < 24 ? 24 : gs;
					continue;
				}
				const auto dataSize = U32(r + 4);
				if (p + 24 + dataSize > grp.size()) {
					throw std::runtime_error("a record runs past the end of its group");
				}
				Record rec;
				std::memcpy(rec.sig, r, 4);
				rec.flags = U32(r + 8);
				rec.formID = U32(r + 12);
				rec.version = U16(r + 20);
				const std::uint8_t* d = r + 24;
				std::size_t         n = dataSize;
				if (rec.flags & 0x40000) {
					if (n < 4) {
						throw std::runtime_error("a compressed record is too short");
					}
					inflated = DecompressZlib(d + 4, n - 4, U32(d));
					d = inflated.data();
					n = inflated.size();
					rec.flags &= ~0x40000u;
				}
				rec.subs = ParseSubs(d, n);
				pf.records.push_back(std::move(rec));
				p += 24 + dataSize;
			}
		}
		return pf;
	}

	std::vector<std::uint8_t> WritePlugin(const std::vector<std::string>& a_masters, std::uint32_t a_headerFlags,
		const std::vector<Record>& a_records, const std::string& a_author, const std::string& a_desc, float a_hedrVersion,
		std::uint16_t a_headerRecordVersion, bool a_fo4)
	{
		// group the records by type, groups in the game's order
		std::map<int, std::pair<std::string, std::vector<const Record*>>> groups;
		for (const auto& r : a_records) {
			// exterior cells live under their worldspace
			const char* top = Is(r.sig, "CELL") && !r.groups.empty() && r.groups[0].first == 1 ? "WRLD" : r.sig;
			if (top != r.sig) {
				auto& g = groups[GroupRank(top, a_fo4) * 10000 + 0];
				g.first = "WRLD";
				g.second.push_back(&r);
				continue;
			}
			auto& g = groups[GroupRank(r.sig, a_fo4) * 10000 + 0];
			if (g.first.empty() || g.first == Sig(r.sig)) {
				g.first = Sig(r.sig);
				g.second.push_back(&r);
			} else {
				// several unknown types share the rank: keep them apart
				auto& g2 = groups[GroupRank(r.sig, a_fo4) * 10000 + static_cast<int>(std::hash<std::string>{}(Sig(r.sig)) % 9999) + 1];
				g2.first = Sig(r.sig);
				g2.second.push_back(&r);
			}
		}
		std::vector<std::uint8_t> body;
		std::uint32_t             count = 0;
		auto writeRecord = [&](std::vector<std::uint8_t>& o, const Record* r) {
			std::vector<std::uint8_t> d;
			WriteSubs(d, r->subs);
			PutSig(o, r->sig);
			Put32(o, static_cast<std::uint32_t>(d.size()));
			Put32(o, r->flags & ~0x40000u);
			Put32(o, r->formID);
			Put32(o, 0);  // version control
			Put16(o, r->version);
			Put16(o, 0);
			o.insert(o.end(), d.begin(), d.end());
			++count;
		};
		auto writeGroup = [&](std::vector<std::uint8_t>& o, std::uint32_t a_label, std::int32_t a_type, const std::vector<std::uint8_t>& a_content) {
			PutSig(o, "GRUP");
			Put32(o, static_cast<std::uint32_t>(a_content.size() + 24));
			Put32(o, a_label);
			Put32(o, static_cast<std::uint32_t>(a_type));
			Put32(o, 0);
			Put32(o, 0);
			o.insert(o.end(), a_content.begin(), a_content.end());
			++count;
		};
		std::uint32_t sigLabel;
		for (auto& [rank, g] : groups) {
			std::sort(g.second.begin(), g.second.end(), [](const Record* a, const Record* b) { return a->formID < b->formID; });
			std::memcpy(&sigLabel, g.first.data(), 4);
			std::vector<std::uint8_t> gb;
			if (g.first == "CELL") {
				// interior cells: block (type 2) > sub-block (type 3) > cell
				std::map<std::uint32_t, std::map<std::uint32_t, std::vector<const Record*>>> blocks;
				for (const auto* r : g.second) {
					const auto b = r->groups.size() > 0 ? r->groups[0].second : 0, sb = r->groups.size() > 1 ? r->groups[1].second : 0;
					blocks[b][sb].push_back(r);
				}
				for (const auto& [b, subs] : blocks) {
					std::vector<std::uint8_t> bb;
					for (const auto& [sb, recs] : subs) {
						std::vector<std::uint8_t> sbb;
						for (const auto* r : recs) writeRecord(sbb, r);
						writeGroup(bb, sb, 3, sbb);
					}
					writeGroup(gb, b, 2, bb);
				}
			} else if (g.first == "WRLD") {
				// each worldspace, then its children group (type 1): its persistent cell, then block (4) > sub-block (5) > cell
				for (const auto* w : g.second) {
					if (std::memcmp(w->sig, "WRLD", 4) != 0) continue;
					writeRecord(gb, w);
					std::vector<const Record*> persistent;
					std::map<std::uint32_t, std::map<std::uint32_t, std::vector<const Record*>>> blocks;
					for (const auto* r : g.second) {
						if (std::memcmp(r->sig, "CELL", 4) != 0 || r->groups.empty() || r->groups[0].second != w->formID) continue;
						if (r->groups.size() < 3) {
							persistent.push_back(r);
						} else {
							blocks[r->groups[1].second][r->groups[2].second].push_back(r);
						}
					}
					if (persistent.empty() && blocks.empty()) continue;
					std::vector<std::uint8_t> wb;
					for (const auto* r : persistent) writeRecord(wb, r);
					for (const auto& [b, subs] : blocks) {
						std::vector<std::uint8_t> bb;
						for (const auto& [sb, recs] : subs) {
							std::vector<std::uint8_t> sbb;
							for (const auto* r : recs) writeRecord(sbb, r);
							writeGroup(bb, sb, 5, sbb);
						}
						writeGroup(wb, b, 4, bb);
					}
					writeGroup(gb, w->formID, 1, wb);
				}
			} else {
				for (const auto* r : g.second) writeRecord(gb, r);
			}
			writeGroup(body, sigLabel, 0, gb);
		}
		std::vector<Sub> hs;
		{
			Sub hedr;
			std::memcpy(hedr.sig, "HEDR", 4);
			const float ver = a_hedrVersion;
			std::uint32_t vb;
			std::memcpy(&vb, &ver, 4);
			Put32(hedr.data, vb);
			Put32(hedr.data, count);
			Put32(hedr.data, t_sf ? 0xFF001000u : 0x800u);  // (Starfield: next object ID with the 0xFF top byte its plugins have)
			hs.push_back(std::move(hedr));
			Sub cnam;
			std::memcpy(cnam.sig, "CNAM", 4);
			cnam.data.assign(a_author.begin(), a_author.end());
			cnam.data.push_back(0);
			hs.push_back(std::move(cnam));
			if (!a_desc.empty()) {
				Sub snam;
				std::memcpy(snam.sig, "SNAM", 4);
				snam.data.assign(a_desc.begin(), a_desc.end());
				snam.data.push_back(0);
				hs.push_back(std::move(snam));
			}
			for (const auto& m : a_masters) {
				Sub mast;
				std::memcpy(mast.sig, "MAST", 4);
				mast.data.assign(m.begin(), m.end());
				mast.data.push_back(0);
				hs.push_back(std::move(mast));
				if (t_sf) {
					continue;  // Starfield: MAST alone (wbDefinitionsSF1 TES4; Constellation.esm, OldMars.esm have no DATA)
				}
				Sub data;
				std::memcpy(data.sig, "DATA", 4);
				data.data.assign(8, 0);
				hs.push_back(std::move(data));
			}
			if (!t_sf) {  // (Starfield's own plugins have no INTV: HEDR, CNAM, SNAM, MAST, INCC)
				Sub intv;
				std::memcpy(intv.sig, "INTV", 4);
				Put32(intv.data, 1);
				hs.push_back(std::move(intv));
			}
			if (a_fo4 || t_sf) {
				// Fallout 4: the interior cell count (required in its header)
				std::uint32_t interiors = 0;
				for (const auto& r : a_records) {
					interiors += Is(r.sig, "CELL") && (r.groups.empty() || r.groups[0].first != 1) ? 1 : 0;
				}
				Sub incc;
				std::memcpy(incc.sig, "INCC", 4);
				Put32(incc.data, interiors);
				hs.push_back(std::move(incc));
			}
		}
		std::vector<std::uint8_t> hd;
		WriteSubs(hd, hs);
		std::vector<std::uint8_t> out;
		PutSig(out, "TES4");
		Put32(out, static_cast<std::uint32_t>(hd.size()));
		Put32(out, a_headerFlags);
		Put32(out, 0);
		Put32(out, 0);
		Put16(out, a_headerRecordVersion);
		Put16(out, 0);
		out.insert(out.end(), hd.begin(), hd.end());
		out.insert(out.end(), body.begin(), body.end());
		return out;
	}

	std::vector<std::string> ReadLoadOrder(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_gameFolder, int a_game)
	{
		std::error_code fec;
		const bool      fo4 = a_game == 2 || (a_game < 0 && ((!a_gameFolder.empty() && std::filesystem::exists(a_gameFolder / "Fallout4.exe", fec)) ||
																Lower(a_pluginsTxt.parent_path().filename().string()) == "fallout4"));
		auto trim = [](std::string s) {
			while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
				s.pop_back();
			}
			std::size_t a = 0;
			if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
				static_cast<unsigned char>(s[2]) == 0xBF) {
				a = 3;
			}
			while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) {
				++a;
			}
			return s.substr(a);
		};
		std::vector<std::string> out;
		std::set<std::string>    seen;
		auto add = [&](const std::string& p) {
			if (!p.empty() && seen.insert(Lower(p)).second) {
				out.push_back(p);
			}
		};
		// enabled plugins (MO2 writes them in load order; the game's own file too)
		std::vector<std::string> enabled;
		std::set<std::string>    enabledLow;
		{
			// SE/AE: enabled lines start with '*'. Legendary Edition has no marks: every plugin listed is enabled.
			// Only read that way when the game is LE (TESV.exe, or the game's own %LOCALAPPDATA%\Skyrim\plugins.txt):
			// an SE file with every plugin disabled has no marks either.
			std::vector<std::string> marked, listed;
			std::ifstream            f(a_pluginsTxt);
			for (std::string line; std::getline(f, line);) {
				line = trim(line);
				if (line.empty() || line[0] == '#') {
					continue;
				}
				if (line[0] == '*') {
					marked.push_back(trim(line.substr(1)));
				} else {
					listed.push_back(line);
				}
			}
			std::error_code lec;
			const bool      le = !fo4 && ((!a_gameFolder.empty() && std::filesystem::exists(a_gameFolder / "TESV.exe", lec)) ||
			                Lower(a_pluginsTxt.parent_path().filename().string()) == "skyrim");
			enabled = marked.empty() && le ? listed : marked;
			for (const auto& e : enabled) {
				enabledLow.insert(Lower(e));
			}
		}
		if (fo4) {
			// Fallout 4's masters (named as the files are: Fallout4.esm, DLCRobot.esm, ...)
			for (const char* p : { "Fallout4.esm", "DLCRobot.esm", "DLCworkshop01.esm", "DLCCoast.esm", "DLCworkshop02.esm", "DLCworkshop03.esm",
					 "DLCNukaWorld.esm", "DLCUltraHighResolution.esm" }) {
				if (a_gameFolder.empty() || std::filesystem::exists(a_gameFolder / "Data" / p, fec) || std::string(p) == "Fallout4.esm") {
					add(p);
				}
			}
		} else {
			for (const char* p : { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" }) {
				add(p);
			}
		}
		// the game loads the Creation Club files listed in Skyrim.ccc (and _ResourcePack.esl) by itself: Mod
		// Organizer does not mark them enabled in plugins.txt
		std::set<std::string> always = { "_resourcepack.esl" };
		std::error_code       ec;
		const char*           cccName = fo4 ? "Fallout4.ccc" : "Skyrim.ccc";
		{
			std::ifstream ccc(a_gameFolder / cccName);
			for (std::string line; std::getline(ccc, line);) {
				line = trim(line);
				if (!line.empty()) always.insert(Lower(line));
			}
		}
		// Mod Organizer keeps the full order (Creation Club files included) in loadorder.txt
		const auto loadorderTxt = a_pluginsTxt.parent_path() / "loadorder.txt";
		if (std::filesystem::exists(loadorderTxt, ec)) {
			std::ifstream f(loadorderTxt);
			for (std::string line; std::getline(f, line);) {
				line = trim(line);
				if (line.empty() || line[0] == '#') {
					continue;
				}
				const auto low = Lower(line);
				const bool cc = always.contains(low) || (a_gameFolder.empty() && low.rfind("cc", 0) == 0 && enabledLow.empty() == false &&
															 (low.ends_with(".esl") || low.ends_with(".esm")));
				if (enabledLow.contains(low) || seen.contains(low) || cc) {
					add(line);
				}
			}
		} else {
			std::ifstream ccc(a_gameFolder / cccName);
			for (std::string line; std::getline(ccc, line);) {
				line = trim(line);
				if (!line.empty() && std::filesystem::exists(a_gameFolder / "Data" / line, ec)) {
					add(line);
				}
			}
		}
		for (const auto& e : enabled) {
			add(e);
		}
		return out;
	}

	namespace
	{
		// records of the plugin itself (not overrides) numbered above 0xFFF, every group, headers only
		std::size_t CountLightOverflow(const std::filesystem::path& a_file, std::uint32_t a_masters)
		{
			std::ifstream f(a_file, std::ios::binary);
			std::uint8_t  h[24];
			if (!f.read(reinterpret_cast<char*>(h), 24)) {
				return 0;
			}
			f.seekg(U32(h + 4), std::ios::cur);
			std::size_t over = 0;
			while (f.read(reinterpret_cast<char*>(h), 24)) {
				if (std::memcmp(h, "GRUP", 4) == 0) {
					continue;  // step into the group
				}
				const auto id = U32(h + 12);
				over += (id >> 24) >= a_masters && (id & 0xFFFFFF) > 0xFFF ? 1 : 0;
				f.seekg(U32(h + 4), std::ios::cur);
			}
			return over;
		}
	}

	// ======================================================================================
	// record layouts: where the form IDs are
	// ======================================================================================

	namespace
	{
		// a subrecord in comparable form: form ID fields are zeroed in data and listed in ids as
		// load-order-wide keys ((plugin index + 1) << 32 | object id; 0 = none)
		struct CSub
		{
			char                                            sig[4];
			std::vector<std::uint8_t>                       data;
			std::vector<std::pair<std::uint32_t, std::uint64_t>> ids;  // offset, key
		};
		bool operator==(const CSub& a, const CSub& b) { return Is(a.sig, b.sig) && a.data == b.data && a.ids == b.ids; }

		struct CRec
		{
			char              sig[4];
			std::uint32_t     flags = 0;
			std::uint16_t     version = 44;
			std::vector<CSub> subs;
		};
		bool SameSubs(const CRec& a, const CRec& b) { return a.subs == b.subs; }

		enum class Kind
		{
			kUnknown,
			kNone,     // no form IDs
			kLString,  // FULL/DESC/SHRT: a strings-file id in localized plugins
			kForm0,    // one form ID at offset 0
			kFormArr,  // form IDs, 4 bytes each
			kVmad,
			kAltTex,   // MODS / MO2S...: alternate textures
			kCoed,
			kDstd,
			kAtkd,
			kLvlo,
			kCrdt,
			kBookData,  // BOOK DATA: a spell's form ID when the book teaches one
			kAlchEnit,  // ALCH ENIT: addiction and consume sound
			kSpit,      // SPIT: half-cost perk
			kCtda,      // a condition: form IDs depend on the function
			kDrop,      // left out of the patch (WRLD's large-reference and offset tables: xEdit never copies them either)
			kFormPairs, // an array of 8-byte {form ID, u32 or float} entries (Fallout 4: LLKC, CVPA, DAMC, PRPS, DAMA)
			kFixed,     // Fallout 4: a struct whose form IDs are found from its size or flags (FixedForms)
			kForm12,    // Starfield: an array of 12-byte entries, each starting with a form ID (LLKC)
		};

		// Fallout 4, from xEdit's wbDefinitionsFO4 (dev-4.1.6). Only the types listed here are merged for Fallout 4; any part
		// not named here leaves the record to the winning plugin (kUnknown), as for Skyrim.
		//  - LVLO: level u16, unused 2, form @4, count u16, chance none u8, unused 1 (12 bytes)
		//  - COED: owner @0, global (owner is an NPC) / rank (faction) / unused @4, item condition float @8
		//  - CNTO: item @0, count s32; COCT u32; LLCT u8 (at most 255 entries)
		//  - MODS / DMDS: a material swap form ID (in Skyrim alternate textures); MODT/DMDT texture hashes, MODC/DMDC float,
		//    MODF flags u8
		//  - DEST header (no forms), DAMC {damage type, value}, DSTD (explosion @8, debris @12), DSTA name, DSTF end marker
		//  - PRPS {actor value form, float}, LLKC {keyword, chance}, CVPA {component, count}: kFormPairs
		Kind LayoutFO4(const char* rec, const char* sub)
		{
			const std::string r(rec, 4), s(sub, 4);
			auto in = [&](std::initializer_list<const char*> a_list) {
				for (const char* x : a_list) {
					if (s == x) return true;
				}
				return false;
			};
			const bool model = in({ "MODL", "MODT", "MODC", "MODF" });
			const bool destNone = in({ "DEST", "DSTA", "DMDL", "DMDT", "DMDC", "DSTF" });
			if (r == "LVLI" || r == "LVLN" || r == "LVSP") {
				if (in({ "EDID", "OBND", "LVLD", "LVLM", "LVLF", "LLCT" })) return Kind::kNone;
				if (s == "LVLO") return Kind::kLvlo;
				if (r == "LVSP") return Kind::kUnknown;
				if (s == "COED") return Kind::kCoed;
				if (s == "LVLG") return Kind::kForm0;
				if (s == "LLKC") return Kind::kFormPairs;
				if (r == "LVLI") {
					if (s == "LVSG") return Kind::kForm0;
					if (s == "ONAM") return Kind::kLString;
				} else {
					if (model) return Kind::kNone;
					if (s == "MODS") return Kind::kForm0;
				}
				return Kind::kUnknown;
			}
			if (r == "CONT") {
				if (in({ "EDID", "OBND", "COCT", "DATA", "KSIZ" }) || model || destNone) return Kind::kNone;
				if (in({ "PTRN", "MODS", "CNTO", "DMDS", "FTYP", "NTRM", "SNAM", "QNAM", "TNAM", "ONAM" })) return Kind::kForm0;
				if (s == "VMAD") return Kind::kVmad;
				if (s == "FULL") return Kind::kLString;
				if (s == "COED") return Kind::kCoed;
				if (s == "DSTD") return Kind::kDstd;
				if (s == "DAMC" || s == "PRPS") return Kind::kFormPairs;
				if (s == "KWDA") return Kind::kFormArr;
				return Kind::kUnknown;
			}
			if (r == "FLST") {
				if (s == "EDID") return Kind::kNone;
				if (s == "FULL") return Kind::kLString;
				if (s == "LNAM") return Kind::kForm0;
				return Kind::kUnknown;
			}
			if (r == "OTFT") {
				if (s == "EDID") return Kind::kNone;
				if (s == "INAM") return Kind::kFormArr;
				return Kind::kUnknown;
			}
			if (r == "MISC" || r == "KEYM") {
				if (in({ "EDID", "OBND", "ICON", "MICO", "KSIZ", "DATA" }) || model || destNone) return Kind::kNone;
				if (in({ "PTRN", "MODS", "DMDS", "YNAM", "ZNAM" })) return Kind::kForm0;
				if (s == "VMAD") return Kind::kVmad;
				if (s == "FULL") return Kind::kLString;
				if (s == "DSTD") return Kind::kDstd;
				if (s == "DAMC") return Kind::kFormPairs;
				if (s == "KWDA") return Kind::kFormArr;
				if (r == "MISC") {
					if (s == "FIMD") return Kind::kForm0;
					if (s == "CVPA") return Kind::kFormPairs;
					if (s == "CDIX") return Kind::kNone;
				}
				return Kind::kUnknown;
			}
			// parts most items share: editor ID, script, bounds, preview transform, name, keywords, model, destruction, sounds
			const bool common = in({ "EDID", "OBND", "KSIZ" }) || model || destNone;
			auto shared = [&](Kind& k) {
				if (common) { k = Kind::kNone; return true; }
				if (s == "VMAD") { k = Kind::kVmad; return true; }
				if (s == "FULL" || s == "DESC") { k = Kind::kLString; return true; }
				if (in({ "PTRN", "MODS", "DMDS", "YNAM", "ZNAM", "ETYP" })) { k = Kind::kForm0; return true; }
				if (s == "DSTD") { k = Kind::kDstd; return true; }
				if (s == "DAMC") { k = Kind::kFormPairs; return true; }
				if (s == "KWDA") { k = Kind::kFormArr; return true; }
				return false;
			};
			// the effects of potions, ingredients and spells: EFID, EFIT (magnitude, area, duration), conditions
			auto effects = [&](Kind& k) {
				if (s == "EFID") { k = Kind::kForm0; return true; }
				if (in({ "EFIT", "CIS1", "CIS2" })) { k = Kind::kNone; return true; }
				if (s == "CTDA") { k = Kind::kCtda; return true; }
				return false;
			};
			Kind k = Kind::kUnknown;
			if (r == "NPC_") {
				// wbDefinitionsFO4 NPC_. ACBS 20 bytes, SNAM {faction, rank s8} 5, PRKR {perk, rank u8} 5, TPTA 13 template actors,
				// ATKD 44 (attack spell @8), AIDT 24, DNAM 8, MWGT 12, CS2H..CS2F actor sounds, TETI/TEND tints, FMRI/FMRS face
				// morphs, MSDK/MSDV morph keys and values, MRSV body morph regions; the object template (OBTE..STOP) is read as one
				// part (see canon)
				if (in({ "EDID", "OBND", "ACBS", "SPCT", "PRKZ", "COCT", "AIDT", "KSIZ", "DATA", "DNAM", "NAM5", "NAM6", "NAM7", "NAM4", "MWGT",
						"NAM8", "CS2H", "CS2E", "CS2F", "QNAM", "MSDK", "MSDV", "TETI", "TEND", "MRSV", "FMRI", "FMRS", "FMIN", "ATKE", "ATKT" }) ||
					destNone)
					return Kind::kNone;
				if (in({ "PTRN", "STCP", "SNAM", "INAM", "VTCK", "TPLT", "LTPT", "LTPC", "RNAM", "SPLO", "WNAM", "ANAM", "ATKR", "ATKW", "ATKS",
						"SPOR", "OCOR", "GWOR", "ECOR", "FCPL", "RCLR", "PRKR", "FTYP", "NTRM", "CNTO", "PKID", "CNAM", "PNAM", "HCLF", "BCLF",
						"ZNAM", "GNAM", "CS2K", "CS2D", "CSCR", "PFRN", "DOFT", "SOFT", "DPLT", "CRIF", "FTST", "DMDS" }))
					return Kind::kForm0;
				if (in({ "TPTA", "KWDA", "APPR" })) return Kind::kFormArr;
				if (in({ "PRPS", "DAMC" })) return Kind::kFormPairs;
				if (in({ "FULL", "SHRT", "ATTX" })) return Kind::kLString;
				if (s == "VMAD") return Kind::kVmad;
				if (s == "COED") return Kind::kCoed;
				if (s == "DSTD") return Kind::kDstd;
				if (s == "ATKD") return Kind::kAtkd;
				return Kind::kUnknown;
			}
			if (r == "ARMO") {
				// male and female world models (MOD2/MOD4 with MO2T/MO4T, MODC, MO2S/MO4S), icons, BOD2, armatures (INDX + MODL =
				// an ARMA form), DATA {value, weight, health}, FNAM {armour rating, base addon index, stagger, unused 3}, DAMA
				if (in({ "MOD2", "MO2T", "MOD4", "MO4T", "ICON", "MICO", "ICO2", "MIC2", "BOD2", "INDX", "DATA", "FNAM" })) return Kind::kNone;
				if (in({ "EITM", "MO2S", "MO4S", "BIDS", "BAMT", "RNAM", "INRD", "TNAM" }) || s == "MODL") return Kind::kForm0;
				if (s == "APPR") return Kind::kFormArr;
				if (s == "DAMA") return Kind::kFormPairs;
				if (in({ "MODT", "MODF", "MODS" })) return Kind::kUnknown;  // not in ARMO's definition (its models are MOD2 / MOD4)
				return shared(k) ? k : Kind::kUnknown;
			}
			if (r == "WEAP") {
				// DNAM 132 bytes, FNAM 41, CRDT 12 (forms found by size: kFixed), the 1st person model (MOD4, MO4T, MO4S, MO4C, MO4F)
				if (in({ "ICON", "MICO", "EAMT", "MOD4", "MO4T", "MO4C", "MO4F", "FLTR", "MASE" })) return Kind::kNone;
				if (in({ "STCP", "EITM", "BIDS", "BAMT", "INRD", "NNAM", "MO4S", "INAM", "LNAM", "WAMD", "WZMD", "CNAM" })) return Kind::kForm0;
				if (in({ "DNAM", "FNAM", "CRDT" })) return Kind::kFixed;
				if (s == "APPR") return Kind::kFormArr;
				if (s == "DAMA") return Kind::kFormPairs;
				return shared(k) ? k : Kind::kUnknown;
			}
			if (r == "AMMO") {
				// DATA {value, weight (optional)}, DNAM {projectile, flags u8, unused 3, damage, health}, ONAM short name, NAM1
				// casing model with NAM2 texture data
				if (in({ "DATA", "NAM1", "NAM2" })) return Kind::kNone;
				if (s == "DNAM") return Kind::kFixed;
				if (s == "ONAM") return Kind::kLString;
				if (s == "ETYP") return Kind::kUnknown;
				return shared(k) ? k : Kind::kUnknown;
			}
			if (r == "BOOK") {
				// DATA {value, weight}, DNAM {flags u8, teaches (actor value / spell / perk by the flags), text offset x, y}
				if (in({ "ICON", "MICO", "DATA" })) return Kind::kNone;
				if (in({ "FIMD", "INAM" })) return Kind::kForm0;
				if (s == "DNAM") return Kind::kFixed;
				if (s == "CNAM") return Kind::kLString;
				if (s == "ETYP") return Kind::kUnknown;
				return shared(k) ? k : Kind::kUnknown;
			}
			if (r == "ALCH") {
				// DATA weight, ENIT {value, flags, addiction, addiction chance, consume sound}, DNAM addiction name, effects
				if (in({ "ICON", "MICO", "DATA" })) return Kind::kNone;
				if (s == "CUSD") return Kind::kForm0;
				if (s == "ENIT") return Kind::kAlchEnit;
				if (s == "DNAM") return Kind::kLString;
				if (effects(k)) return k;
				return shared(k) ? k : Kind::kUnknown;
			}
			if (r == "INGR") {
				// DATA {value, weight}, ENIT {ingredient value, flags}, effects
				if (in({ "ICON", "MICO", "DATA", "ENIT" })) return Kind::kNone;
				if (effects(k)) return k;
				if (s == "DESC") return Kind::kUnknown;
				return shared(k) ? k : Kind::kUnknown;
			}
			if (r == "SPEL") {
				// EDID OBND FULL KSIZ KWDA ETYP DESC SPIT (36 bytes, casting perk @32) and the effects; nothing else
				if (in({ "EDID", "OBND", "KSIZ" })) return Kind::kNone;
				if (s == "FULL" || s == "DESC") return Kind::kLString;
				if (s == "KWDA") return Kind::kFormArr;
				if (s == "ETYP") return Kind::kForm0;
				if (s == "SPIT") return Kind::kSpit;
				if (effects(k)) return k;
				return Kind::kUnknown;
			}
			if (r == "CELL") {
				// DATA flags u16, VISI/PCMB timestamps, RVIS previs cell, XCLC grid, XCLL lighting, CNAM/ZNAM precombined levels u8,
				// TVDT, MHDT, XCLW water height, XCLR regions, XWCN/XWCU currents, XOWN {owner, unused 4, no crime u8, unused 3},
				// XRNK, XILW {worldspace, offsets}, XWEM, XPRI previs references, XCRI combined references (kFixed)
				if (in({ "EDID", "DATA", "VISI", "PCMB", "XCLC", "XCLL", "CNAM", "ZNAM", "TVDT", "MHDT", "XCLW", "XWCN", "XWCU", "XRNK", "XWEM" }))
					return Kind::kNone;
				if (s == "FULL") return Kind::kLString;
				if (in({ "RVIS", "LTMP", "XLCN", "XCWT", "XOWN", "XILL", "XILW", "XCCM", "XCAS", "XEZN", "XCMO", "XCIM", "XGDR" })) return Kind::kForm0;
				if (s == "XCLR" || s == "XPRI") return Kind::kFormArr;
				if (s == "XCRI") return Kind::kFixed;
				return Kind::kUnknown;
			}
			if (r == "WRLD") {
				// only copied as the parent of an exterior cell: the large-reference (RNAM) and offset (OFST) tables are left out,
				// as for Skyrim
				if (s == "RNAM" || s == "OFST") return Kind::kDrop;
				if (in({ "EDID", "MHDT", "WCTR", "PNAM", "NAM4", "DNAM", "ICON", "MNAM", "ONAM", "NAMA", "DATA", "NAM0", "NAM9", "NNAM", "XWEM",
						"TNAM", "UNAM", "WLEV", "CLSZ" }) ||
					model)
					return Kind::kNone;
				if (s == "FULL") return Kind::kLString;
				if (in({ "LTMP", "XEZN", "XLCN", "WNAM", "CNAM", "NAM2", "NAM3", "ZNAM", "MODS" })) return Kind::kForm0;
				return Kind::kUnknown;
			}
			return Kind::kUnknown;
		}

		// Starfield, from xEdit's wbDefinitionsSF1 (dev-4.1.6), subrecords checked against Starfield.esm 1.16.244. Only the list
		// records are merged: LVLI, LVLN, LVLP, LVLB, LVSC, FLST, OTFT. Anything not named here (scripts VMAD, base form
		// components BFCB..BFCE, material swaps MOLM, entry COED, a form list's conditional entries INAM / CITC) leaves the
		// record to the winning plugin.
		//  - LVLO as Fallout 4's (form @4); an entry's conditions (CTDA, CIS1, CIS2) follow it; LLKC {keyword, chance u32,
		//    unused 4}: 12 bytes; PTT2: 8 transform form IDs; FTYP: form IDs; OBND, ODTY, OPDS, XALG, LVLD/M/F, LLCT, LVLL,
		//    LIMC, FLLD, MODL, MODC, XFLG: no form IDs; SNTP, SNBH, DEFL, LVLG, LLRB, LLRV, LVSG: one form ID
		//  - CTDA (32 bytes): comparison global @4 (flag 0x04), parameters @12 / @16 by Starfield's function table, the
		//    reference @24 when run on a reference (2), the linked keyword @28 when run on a linked reference (4)
		Kind LayoutSF(const char* rec, const char* sub)
		{
			const std::string r(rec, 4), s(sub, 4);
			auto in = [&](std::initializer_list<const char*> a_list) {
				for (const char* x : a_list) {
					if (s == x) return true;
				}
				return false;
			};
			if (r == "LVLI" || r == "LVLN" || r == "LVLP" || r == "LVLB" || r == "LVSC") {
				if (in({ "EDID", "OBND", "ODTY", "OPDS", "XALG", "LVLD", "LVLM", "LVLF", "LLCT", "LVLL", "LIMC", "MODL", "FLLD", "MODC", "XFLG", "CIS1",
						"CIS2" }))
					return Kind::kNone;
				if (s == "LVLO") return Kind::kLvlo;
				if (s == "CTDA") return Kind::kCtda;
				if (in({ "LVLG", "LLRB", "LLRV", "LVSG", "SNTP", "SNBH", "DEFL" })) return Kind::kForm0;
				if (s == "LLKC") return Kind::kForm12;
				if (s == "PTT2" || s == "FTYP") return Kind::kFormArr;
				if (s == "ONAM" && (r == "LVLI" || r == "LVLN")) return Kind::kLString;
				return Kind::kUnknown;
			}
			if (r == "FLST") {
				if (s == "EDID") return Kind::kNone;
				if (s == "FULL") return Kind::kLString;
				if (s == "LNAM" || s == "ANAM") return Kind::kForm0;
				return Kind::kUnknown;  // (INAM / CITC / CTDA: conditional entries point at LNAM by index)
			}
			if (r == "OTFT") {
				if (s == "EDID") return Kind::kNone;
				if (s == "INAM") return Kind::kFormArr;
				return Kind::kUnknown;
			}
			return Kind::kUnknown;
		}
		Kind Layout(const char* rec, const char* sub, bool a_fo4)
		{
			if (t_sf) {
				return LayoutSF(rec, sub);
			}
			if (a_fo4) {
				return LayoutFO4(rec, sub);
			}
			const std::string r(rec, 4), s(sub, 4);
			// type-specific first
			if (r == "LVLI" || r == "LVLN" || r == "LVSP") {
				if (s == "LVLD" || s == "LVLF" || s == "LLCT") return Kind::kNone;
				if (s == "LVLG") return Kind::kForm0;
				if (s == "LVLO") return Kind::kLvlo;
			} else if (r == "CONT") {
				if (s == "DATA") return Kind::kNone;
				if (s == "SNAM" || s == "QNAM") return Kind::kForm0;
			} else if (r == "FLST") {
				if (s == "LNAM") return Kind::kForm0;
			} else if (r == "OTFT") {
				if (s == "INAM") return Kind::kFormArr;
			} else if (r == "ARMO") {
				if (s == "RNAM" || s == "MODL" || s == "TNAM") return Kind::kForm0;
				if (s == "DATA" || s == "DNAM") return Kind::kNone;
			} else if (r == "WEAP") {
				if (s == "EFSD" || s == "INAM" || s == "WNAM" || s == "SNAM" || s == "XNAM" || s == "NAM7" || s == "TNAM" || s == "UNAM" ||
					s == "NAM9" || s == "NAM8" || s == "CNAM")
					return Kind::kForm0;
				if (s == "NNAM" || s == "DATA" || s == "DNAM" || s == "VNAM") return Kind::kNone;
				if (s == "CRDT") return Kind::kCrdt;
			} else if (r == "AMMO") {
				if (s == "DATA") return Kind::kForm0;
				if (s == "ONAM") return Kind::kNone;
			} else if (r == "CELL") {
				if (s == "DATA" || s == "XCLC" || s == "XCLL" || s == "TVDT" || s == "MHDT" || s == "LNAM" || s == "XCLW" || s == "XNAM" ||
					s == "XWCN" || s == "XWCS" || s == "XWCU" || s == "XRNK" || s == "XWEM")
					return Kind::kNone;
				if (s == "LTMP" || s == "XLCN" || s == "XCWT" || s == "XOWN" || s == "XILL" || s == "XCCM" || s == "XCAS" || s == "XEZN" ||
					s == "XCMO" || s == "XCIM")
					return Kind::kForm0;
				if (s == "XCLR") return Kind::kFormArr;
			} else if (r == "WRLD") {
				if (s == "RNAM" || s == "OFST") return Kind::kDrop;
				if (s == "MHDT" || s == "WCTR" || s == "PNAM" || s == "NAM4" || s == "DNAM" || s == "MNAM" || s == "ONAM" || s == "NAMA" ||
					s == "DATA" || s == "NAM0" || s == "NAM9" || s == "NNAM" || s == "XNAM" || s == "XWEM" || s == "TNAM" || s == "UNAM" || s == "CLSZ")
					return Kind::kNone;
				if (s == "LTMP" || s == "XEZN" || s == "XLCN" || s == "WNAM" || s == "CNAM" || s == "NAM2" || s == "NAM3" || s == "ZNAM")
					return Kind::kForm0;
			} else if (r == "MISC" || r == "KEYM") {
				if (s == "DATA") return Kind::kNone;
			} else if (r == "SLGM") {
				if (s == "DATA" || s == "SOUL" || s == "SLCP") return Kind::kNone;
				if (s == "NAM0") return Kind::kForm0;
			} else if (r == "BOOK") {
				if (s == "DATA") return Kind::kBookData;
				if (s == "INAM") return Kind::kForm0;
				if (s == "CNAM") return Kind::kLString;
			} else if (r == "ALCH") {
				if (s == "DATA") return Kind::kNone;
				if (s == "ENIT") return Kind::kAlchEnit;
			} else if (r == "INGR") {
				if (s == "DATA" || s == "ENIT") return Kind::kNone;
			} else if (r == "SCRL" || r == "SPEL") {
				if (s == "DATA") return Kind::kNone;
				if (s == "SPIT") return Kind::kSpit;
			} else if (r == "NPC_") {
				static const char* const forms[] = { "SNAM", "INAM", "VTCK", "TPLT", "RNAM", "SPLO", "WNAM", "ANAM", "ATKR", "SPOR", "OCOR", "GWOR",
					"ECOR", "PRKR", "PKID", "CNAM", "PNAM", "HCLF", "ZNAM", "GNAM", "CSDI", "CSCR", "DOFT", "SOFT", "DPLT", "CRIF", "FTST" };
				for (const char* f : forms) {
					if (s == f) return Kind::kForm0;
				}
				static const char* const none[] = { "ACBS", "SPCT", "ATKE", "PRKZ", "AIDT", "DATA", "DNAM", "NAM5", "NAM6", "NAM7", "NAM8", "CSDT",
					"CSDC", "QNAM", "NAM9", "NAMA", "TINI", "TINC", "TINV", "TIAS" };
				for (const char* f : none) {
					if (s == f) return Kind::kNone;
				}
				if (s == "ATKD") return Kind::kAtkd;
			}
			// shared parts
			if (s == "EDID" || s == "OBND" || s == "MODT" || s == "MOD2" || s == "MO2T" || s == "MOD3" || s == "MO3T" || s == "MOD4" ||
				s == "MO4T" || s == "ICON" || s == "MICO" || s == "ICO2" || s == "MIC2" || s == "DEST" || s == "DMDL" || s == "DMDT" ||
				s == "DSTF" || s == "KSIZ" || s == "COCT" || s == "EAMT" || s == "BODT" || s == "BOD2" || s == "BMCT")
				return Kind::kNone;
			if (s == "MODL") return Kind::kNone;  // a model path (ARMO's MODL is handled above)
			if (s == "FULL" || s == "DESC" || s == "SHRT") return Kind::kLString;
			if (s == "VMAD") return Kind::kVmad;
			if (s == "MODS" || s == "MO2S" || s == "MO3S" || s == "MO4S" || s == "DMDS") return Kind::kAltTex;
			if (s == "DSTD") return Kind::kDstd;
			if (s == "KWDA") return Kind::kFormArr;
			if (s == "CNTO" || s == "EITM" || s == "ETYP" || s == "BIDS" || s == "BAMT" || s == "YNAM" || s == "ZNAM" || s == "MDOB" || s == "EFID")
				return Kind::kForm0;
			if (s == "EFIT" || s == "CIS1" || s == "CIS2") return Kind::kNone;
			if (s == "CTDA") return Kind::kCtda;
			if (s == "COED") return Kind::kCoed;
			return Kind::kUnknown;
		}

		// form ID offsets inside a script attachment (VMAD) of a record without fragments
		bool VmadForms(const std::vector<std::uint8_t>& d, std::vector<std::uint32_t>& offs, std::string& err, bool a_fo4)
		{
			std::size_t p = 0;
			auto need = [&](std::size_t n) {
				if (p + n > d.size()) {
					throw std::runtime_error("script data cut off");
				}
			};
			auto u8 = [&] { need(1); return d[p++]; };
			auto u16 = [&] { need(2); auto v = U16(d.data() + p); p += 2; return v; };
			auto u32 = [&] { need(4); auto v = U32(d.data() + p); p += 4; return v; };
			auto wstr = [&] { const auto n = u16(); need(n); p += n; };
			try {
				const auto version = static_cast<std::int16_t>(u16());
				const auto objFormat = static_cast<std::int16_t>(u16());
				auto object = [&] {
					need(8);
					offs.push_back(static_cast<std::uint32_t>(objFormat == 1 ? p : p + 4));
					p += 8;
				};
				// Fallout 4 (VMAD version 6, xEdit wbDefinitionsFO4) adds structs: 7 a struct (u32 member count, each member
				// {name, type u8, flags u8, value}), 17 an array of structs (u32 count of structs). Types 6 and 16 (variables)
				// are not read: xEdit gives them no data, which is not confirmed.
				std::function<void(std::uint8_t, int)> value;
				auto structValue = [&](int a_depth) {
					if (a_depth > 8) {
						throw std::runtime_error("script structs nested too deep");
					}
					const auto members = u32();
					for (std::uint32_t m = 0; m < members; ++m) {
						wstr();
						const auto type = u8();
						u8();  // flags
						value(type, a_depth + 1);
					}
				};
				value = [&](std::uint8_t type, int a_depth) {
					switch (type) {
					case 1: object(); break;
					case 2: wstr(); break;
					case 3:
					case 4: u32(); break;
					case 5: u8(); break;
					case 7:
						if (!a_fo4) throw std::runtime_error("unknown script property type 7");
						structValue(a_depth);
						break;
					case 11: { const auto n = u32(); for (std::uint32_t j = 0; j < n; ++j) object(); break; }
					case 12: { const auto n = u32(); for (std::uint32_t j = 0; j < n; ++j) wstr(); break; }
					case 13:
					case 14: { const auto n = u32(); need(std::size_t(n) * 4); p += std::size_t(n) * 4; break; }
					case 15: { const auto n = u32(); need(n); p += n; break; }
					case 17: {
						if (!a_fo4) throw std::runtime_error("unknown script property type 17");
						const auto n = u32();
						for (std::uint32_t j = 0; j < n; ++j) structValue(a_depth);
						break;
					}
					default: throw std::runtime_error("unknown script property type " + std::to_string(type));
					}
				};
				const auto scripts = u16();
				for (std::uint16_t i = 0; i < scripts; ++i) {
					wstr();
					if (version >= 4) {
						u8();
					}
					const auto props = u16();
					for (std::uint16_t k = 0; k < props; ++k) {
						wstr();
						const auto type = u8();
						if (version >= 4) {
							u8();
						}
						value(type, 0);
					}
				}
				if (p != d.size()) {
					throw std::runtime_error("script data has extra parts (fragments) not expected here");
				}
				return true;
			} catch (const std::exception& e) {
				err = std::string("scripts (VMAD): ") + e.what();
				return false;
			}
		}

		bool AltTexForms(const std::vector<std::uint8_t>& d, std::vector<std::uint32_t>& offs)
		{
			std::size_t p = 0;
			if (d.size() < 4) {
				return d.empty();
			}
			const auto n = U32(d.data());
			p = 4;
			for (std::uint32_t i = 0; i < n; ++i) {
				if (p + 4 > d.size()) return false;
				const auto len = U32(d.data() + p);
				p += 4 + len;
				if (p + 8 > d.size()) return false;
				offs.push_back(static_cast<std::uint32_t>(p));
				p += 8;
			}
			return p == d.size();
		}

		// Fallout 4 structs whose form IDs depend on their size or flags (wbDefinitionsFO4); false: not this layout
		bool FixedForms(const char* rec, const char* sub, const std::vector<std::uint8_t>& d, std::vector<std::uint32_t>& offs)
		{
			const std::string k = std::string(rec, 4) + "." + std::string(sub, 4);
			if (k == "WEAP.DNAM") {
				// ammo @0, skill @40, resist @44, eight sounds @73..@101 (attack, attack 2D, attack loop, attack fail, idle, equip,
				// unequip, fast equip)
				if (d.size() != 132) return false;
				offs = { 0, 40, 44 };
				for (std::uint32_t o = 73; o <= 101; o += 4) offs.push_back(o);
				return true;
			}
			if (k == "WEAP.FNAM") {  // override projectile @29
				if (d.size() != 41) return false;
				offs = { 29 };
				return true;
			}
			if (k == "WEAP.CRDT") {  // crit damage mult, crit charge bonus, crit effect @8
				if (d.size() != 12) return false;
				offs = { 8 };
				return true;
			}
			if (k == "AMMO.DNAM") {  // projectile @0, flags u8, unused 3, damage, health
				if (d.size() != 16) return false;
				offs = { 0 };
				return true;
			}
			if (k == "BOOK.DNAM") {
				// flags u8; teaches @1: an actor value (0x01), else a spell (0x04), else a perk (0x10), else unused; text offset x, y
				if (d.size() != 13) return false;
				if (d[0] & 0x15) offs = { 1 };
				return true;
			}
			if (k == "CELL.XCRI") {
				// meshes count u32, references count u32 (twice the entries), meshes (u32 each), entries {reference, mesh u32}
				if (d.size() < 8) return false;
				const auto meshes = U32(d.data()), refs = U32(d.data() + 4);
				if (refs % 2) return false;
				const std::uint64_t start = 8 + std::uint64_t(meshes) * 4, end = start + std::uint64_t(refs / 2) * 8;
				if (end != d.size()) return false;
				for (std::uint64_t o = start; o < end; o += 8) offs.push_back(static_cast<std::uint32_t>(o));
				return true;
			}
			return false;
		}

		// Fallout 4 OBTS (an object template combination): include count u32, property count u32, level min u8, unused, level
		// max u8, unused, parent combination index s16, default u8, keywords (u8 count, then form IDs), min level for ranks u8,
		// alt levels per tier u8, includes (mod form ID, attach point index u8, optional u8, don't use all u8: 7 bytes each),
		// properties (24 bytes each: value type u8, unused 3, function type u8, unused 3, property u16, unused 2, value 1, value
		// 2, step; value 1 is a form ID for value types 4 and 6)
		bool ObtsForms(const std::vector<std::uint8_t>& d, std::vector<std::uint32_t>& offs)
		{
			if (d.size() < 18) return false;
			const std::uint64_t includes = U32(d.data()), props = U32(d.data() + 4), kw = d[15];
			if (18 + kw * 4 + includes * 7 + props * 24 != d.size()) return false;
			std::uint32_t p = 16;
			for (std::uint64_t i = 0; i < kw; ++i, p += 4) offs.push_back(p);
			p += 2;
			for (std::uint64_t i = 0; i < includes; ++i, p += 7) offs.push_back(p);
			for (std::uint64_t i = 0; i < props; ++i, p += 24) {
				if (d[p] == 4 || d[p] == 6) offs.push_back(p + 12);
			}
			return true;
		}
	}

	// ======================================================================================
	// building the patch
	// ======================================================================================

	namespace
	{
		struct PInfo
		{
			std::string           name;
			bool                  found = false, localized = false, light = false;
			std::vector<int>      masterLo;  // load order index of each master (-1: not loaded)
			std::set<int>         masterSet;
			bool                  stringsLoaded = false;
			std::unordered_map<std::uint32_t, std::string> strings;
		};

		struct Ver
		{
			int    plugin;
			Record rec;
		};

		// a list inside a record that is merged entry by entry
		struct ListSpec
		{
			const char* entry;     // the subrecord starting an entry (or holding an array)
			const char* follower;  // the subrecords belonging to the entry before it ("COED", Starfield "COED CTDA CIS1 CIS2"), or nullptr
			const char* count;     // the count subrecord before the list, or nullptr
			int         countSize; // 1 (LLCT) or 4
			bool        byForm;    // entries are identified by their first form ID; the rest is a value
			bool        array;     // one subrecord holding 4-byte form IDs (KWDA, OTFT INAM)
			std::size_t max;       // most entries the game takes (0: no limit)
			const char* what;      // for the report
		};

		// a_sig is one of the space-separated signatures of a_list
		bool Follows(const char* a_sig, const char* a_list)
		{
			for (const char* p = a_list; *p;) {
				while (*p == ' ') ++p;
				if (!*p) break;
				if (std::strncmp(p, a_sig, 4) == 0 && (p[4] == ' ' || p[4] == 0)) return true;
				while (*p && *p != ' ') ++p;
			}
			return false;
		}

		std::vector<ListSpec> Lists(const char* rec, bool a_fo4)
		{
			const std::string r(rec, 4);
			if (t_sf) {
				// Starfield (wbDefinitionsSF1): LLCT u8, at most 255 entries; an entry's conditions follow its LVLO (COED too, which
				// leaves the record to the winner: see LayoutSF)
				if (r == "LVLI" || r == "LVLN" || r == "LVLB") return { { "LVLO", "COED CTDA CIS1 CIS2", "LLCT", 1, false, false, 255, "entries" } };
				if (r == "LVLP" || r == "LVSC") return { { "LVLO", "CTDA CIS1 CIS2", "LLCT", 1, false, false, 255, "entries" } };
				if (r == "FLST") return { { "LNAM", nullptr, nullptr, 0, false, false, 0, "forms" } };
				if (r == "OTFT") return { { "INAM", nullptr, nullptr, 0, false, true, 0, "items" } };
				return {};
			}
			if (a_fo4) {
				// Fallout 4 (wbDefinitionsFO4): LLCT u8 with at most 255 entries; LVSP entries have no COED
				if (r == "LVLI" || r == "LVLN") return { { "LVLO", "COED", "LLCT", 1, false, false, 255, "entries" } };
				if (r == "LVSP") return { { "LVLO", nullptr, "LLCT", 1, false, false, 255, "entries" } };
				if (r == "CONT") {
					return { { "CNTO", "COED", "COCT", 4, true, false, 0, "items" }, { "KWDA", nullptr, "KSIZ", 4, false, true, 0, "keywords" } };
				}
				if (r == "FLST") return { { "LNAM", nullptr, nullptr, 0, false, false, 0, "forms" } };
				if (r == "OTFT") return { { "INAM", nullptr, nullptr, 0, false, true, 0, "items" } };
				if (r == "NPC_") {
					// SNAM {faction, rank}, SPLO with SPCT, PRKR {perk, rank} with PRKZ, CNTO (+ COED) with COCT, keywords
					return { { "SNAM", nullptr, nullptr, 0, true, false, 0, "factions" }, { "SPLO", nullptr, "SPCT", 4, false, false, 0, "spells" },
						{ "PRKR", nullptr, "PRKZ", 4, true, false, 0, "perks" }, { "CNTO", "COED", "COCT", 4, true, false, 0, "items" },
						{ "KWDA", nullptr, "KSIZ", 4, false, true, 0, "keywords" } };
				}
				if (r == "CELL") return { { "XCLR", nullptr, nullptr, 0, false, true, 0, "regions" } };
				// armatures (ARMO INDX + MODL) are not merged as a list, as for Skyrim
				if (r == "MISC" || r == "KEYM" || r == "ARMO" || r == "WEAP" || r == "AMMO" || r == "BOOK" || r == "ALCH" || r == "INGR" || r == "SPEL")
					return { { "KWDA", nullptr, "KSIZ", 4, false, true, 0, "keywords" } };
				return {};
			}
			if (r == "LVLI" || r == "LVLN" || r == "LVSP") {
				return { { "LVLO", "COED", "LLCT", 1, false, false, 255, "entries" } };
			}
			if (r == "CONT") {
				return { { "CNTO", "COED", "COCT", 4, true, false, 0, "items" } };
			}
			if (r == "CELL") {
				return { { "XCLR", nullptr, nullptr, 0, false, true, 0, "regions" } };
			}
			if (r == "FLST") {
				return { { "LNAM", nullptr, nullptr, 0, false, false, 0, "forms" } };
			}
			if (r == "OTFT") {
				return { { "INAM", nullptr, nullptr, 0, false, true, 0, "items" } };
			}
			if (r == "NPC_") {
				return { { "SNAM", nullptr, nullptr, 0, true, false, 0, "factions" }, { "SPLO", nullptr, "SPCT", 4, false, false, 0, "spells" },
					{ "PRKR", nullptr, "PRKZ", 4, true, false, 0, "perks" }, { "CNTO", "COED", "COCT", 4, true, false, 0, "items" },
					{ "KWDA", nullptr, "KSIZ", 4, false, true, 0, "keywords" } };
			}
			if (r == "ARMO") {
				// armatures (MODL) are not merged: a mod that replaces an armour's look swaps them on purpose, and
				// adding back the old ones would draw both meshes
				return { { "KWDA", nullptr, "KSIZ", 4, false, true, 0, "keywords" } };
			}
			if (r == "WEAP" || r == "AMMO" || r == "MISC" || r == "KEYM" || r == "SLGM" || r == "BOOK" || r == "ALCH" || r == "INGR" || r == "SCRL" ||
				r == "SPEL") {
				return { { "KWDA", nullptr, "KSIZ", 4, false, true, 0, "keywords" } };
			}
			return {};
		}

		// subrecord order of each type, to place a list the winning override does not have
		std::vector<std::string> Order(const char* rec, bool a_fo4)
		{
			const std::string r(rec, 4);
			std::string s;
			if (t_sf) {
				// Starfield: the order of xEdit's definitions (record conditions after LVLF; an entry's after its LVLO)
				if (r == "LVLI")
					s = "EDID VMAD OBND ODTY OPDS PTT2 SNBH DEFL XALG BFCB BFCE LVLD LVLM LVLF CTDA CIS1 CIS2 LVLG LLRB LLRV LLCT LVLO COED LLKC LVSG "
						"LIMC ONAM LVLL MODL MOLM FLLD MODC XFLG FTYP";
				else if (r == "LVLN") s = "EDID VMAD OBND ODTY OPDS XALG BFCB BFCE LVLD LVLM LVLF CTDA CIS1 CIS2 LVLG LLCT LVLO COED LLKC ONAM MODL MOLM FLLD MODC XFLG";
				else if (r == "LVLP") s = "EDID VMAD OBND ODTY OPDS PTT2 XALG BFCB BFCE LVLD LVLM LVLF CTDA CIS1 CIS2 LVLG LLCT LVLO MODL MOLM FLLD MODC XFLG FTYP";
				else if (r == "LVLB")
					s = "EDID VMAD OBND ODTY OPDS PTT2 SNTP SNBH DEFL XALG BFCB BFCE LVLD LVLM LVLF CTDA CIS1 CIS2 LVLG LLCT LVLO COED LLKC MODL MOLM FLLD MODC XFLG";
				else if (r == "LVSC") s = "EDID VMAD BFCB BFCE LVLD LVLM LVLF CTDA CIS1 CIS2 LVLG LLCT LVLO";
				else if (r == "FLST") s = "EDID BFCB BFCE FULL LNAM INAM CITC CTDA ANAM";
				else if (r == "OTFT") s = "EDID VMAD BFCB BFCE INAM";
				std::vector<std::string> out;
				std::istringstream       is(s);
				for (std::string w; is >> w;) out.push_back(w);
				return out;
			}
			if (a_fo4) {
				// Fallout 4: the order of xEdit's definitions
				if (r == "LVLI") s = "EDID OBND LVLD LVLM LVLF LVLG LLCT LVLO COED LLKC LVSG ONAM";
				else if (r == "LVLN") s = "EDID OBND LVLD LVLM LVLF LVLG LLCT LVLO COED LLKC MODL MODT MODC MODS MODF";
				else if (r == "LVSP") s = "EDID OBND LVLD LVLM LVLF LLCT LVLO";
				else if (r == "CONT")
					s = "EDID VMAD OBND PTRN FULL MODL MODT MODC MODS MODF COCT CNTO COED DEST DAMC DSTD DSTA DMDL DMDT DMDC DMDS DSTF DATA KSIZ KWDA "
						"FTYP NTRM PRPS SNAM QNAM TNAM ONAM";
				else if (r == "FLST") s = "EDID FULL LNAM";
				else if (r == "OTFT") s = "EDID INAM";
				else if (r == "MISC")
					s = "EDID VMAD OBND PTRN FULL MODL MODT MODC MODS MODF ICON MICO DEST DAMC DSTD DSTA DMDL DMDT DMDC DMDS DSTF YNAM ZNAM KSIZ KWDA "
						"FIMD DATA CVPA CDIX";
				else if (r == "KEYM")
					s = "EDID VMAD OBND PTRN FULL MODL MODT MODC MODS MODF ICON MICO DEST DAMC DSTD DSTA DMDL DMDT DMDC DMDS DSTF YNAM ZNAM KSIZ KWDA "
						"DATA";
				else if (r == "NPC_")
					s = "EDID VMAD OBND PTRN STCP ACBS SNAM INAM VTCK TPLT LTPT LTPC TPTA RNAM SPCT SPLO DEST DAMC DSTD DSTA DMDL DMDT DMDC DMDS DSTF "
						"WNAM ANAM ATKR ATKD ATKE ATKW ATKS ATKT SPOR OCOR GWOR ECOR FCPL RCLR PRKZ PRKR PRPS FTYP NTRM COCT CNTO COED AIDT PKID KSIZ "
						"KWDA APPR OBTE CNAM FULL SHRT DATA DNAM PNAM HCLF BCLF ZNAM GNAM NAM5 NAM6 NAM7 NAM4 MWGT NAM8 CS2H CS2K CS2D CS2E CS2F CSCR "
						"PFRN DOFT SOFT DPLT CRIF FTST QNAM MSDK MSDV TETI TEND MRSV FMRI FMRS FMIN ATTX";
				else if (r == "ARMO")
					s = "EDID VMAD OBND PTRN FULL EITM MOD2 MO2T MODC MO2S ICON MICO MOD4 MO4T MO4S ICO2 MIC2 BOD2 DEST DAMC DSTD DSTA DMDL DMDT DMDC "
						"DMDS DSTF YNAM ZNAM ETYP BIDS BAMT RNAM KSIZ KWDA DESC INRD INDX MODL DATA FNAM DAMA TNAM APPR OBTE";
				else if (r == "WEAP")
					s = "EDID VMAD OBND PTRN STCP FULL MODL MODT MODC MODS MODF ICON MICO EITM EAMT DEST DAMC DSTD DSTA DMDL DMDT DMDC DMDS DSTF ETYP "
						"BIDS BAMT YNAM ZNAM KSIZ KWDA DESC INRD APPR OBTE NNAM MOD4 MO4T MO4S MO4C MO4F DNAM FNAM CRDT INAM LNAM WAMD WZMD CNAM DAMA "
						"FLTR MASE";
				else if (r == "AMMO")
					s = "EDID VMAD OBND PTRN FULL MODL MODT MODC MODS MODF DEST DAMC DSTD DSTA DMDL DMDT DMDC DMDS DSTF YNAM ZNAM DESC KSIZ KWDA DATA "
						"DNAM ONAM NAM1 NAM2";
				else if (r == "BOOK")
					s = "EDID VMAD OBND PTRN FULL MODL MODT MODC MODS MODF ICON MICO DESC DEST DAMC DSTD DSTA DMDL DMDT DMDC DMDS DSTF YNAM ZNAM KSIZ "
						"KWDA FIMD DATA DNAM CNAM INAM";
				else if (r == "ALCH")
					s = "EDID VMAD OBND PTRN FULL KSIZ KWDA MODL MODT MODC MODS MODF ICON MICO DEST DAMC DSTD DSTA DMDL DMDT DMDC DMDS DSTF YNAM ZNAM "
						"ETYP CUSD DESC DATA ENIT DNAM EFID EFIT CTDA CIS1 CIS2";
				else if (r == "INGR")
					s = "EDID VMAD OBND PTRN FULL KSIZ KWDA MODL MODT MODC MODS MODF ICON MICO DEST DAMC DSTD DSTA DMDL DMDT DMDC DMDS DSTF ETYP YNAM "
						"ZNAM DATA ENIT EFID EFIT CTDA CIS1 CIS2";
				else if (r == "SPEL") s = "EDID OBND FULL KSIZ KWDA ETYP DESC SPIT EFID EFIT CTDA CIS1 CIS2";
				else if (r == "CELL")
					s = "EDID FULL DATA VISI RVIS PCMB XCLC XCLL CNAM ZNAM TVDT MHDT LTMP XCLW XCLR XLCN XWCN XWCU XCWT XOWN XRNK XILL XILW XWEM XCCM "
						"XCAS XEZN XCMO XCIM XGDR XPRI XCRI";
				else if (r == "WRLD")
					s = "EDID RNAM MHDT FULL WCTR LTMP XEZN XLCN WNAM PNAM CNAM NAM2 NAM3 NAM4 DNAM ICON MODL MODT MODC MODS MODF MNAM ONAM NAMA DATA "
						"NAM0 NAM9 ZNAM NNAM XWEM TNAM UNAM WLEV OFST CLSZ";
			} else if (r == "LVLI") s = "EDID OBND LVLD LVLF LVLG LLCT LVLO COED";
			else if (r == "LVLN") s = "EDID OBND LVLD LVLF LVLG LLCT LVLO COED MODL MODT MODS";
			else if (r == "LVSP") s = "EDID OBND LVLD LVLF LLCT LVLO";
			else if (r == "CONT") s = "EDID VMAD OBND FULL MODL MODT MODS COCT CNTO COED DEST DSTD DMDL DMDT DMDS DSTF DATA SNAM QNAM";
			else if (r == "FLST") s = "EDID LNAM";
			else if (r == "OTFT") s = "EDID INAM";
			else if (r == "NPC_")
				s = "EDID VMAD OBND ACBS SNAM INAM VTCK TPLT RNAM SPCT SPLO DEST DSTD DMDL DMDT DMDS DSTF WNAM ANAM ATKR ATKD ATKE SPOR OCOR GWOR "
					"ECOR PRKZ PRKR COCT CNTO COED AIDT PKID KSIZ KWDA CNAM FULL SHRT DATA DNAM PNAM HCLF ZNAM GNAM NAM5 NAM6 NAM7 NAM8 CSDT CSDI "
					"CSDC CSCR DOFT SOFT DPLT CRIF FTST QNAM NAM9 NAMA TINI TINC TINV TIAS";
			else if (r == "ARMO")
				s = "EDID VMAD OBND FULL EITM EAMT MOD2 MO2T MO2S ICON MICO MOD4 MO4T MO4S ICO2 MIC2 BODT BOD2 DEST DSTD DMDL DMDT DMDS DSTF YNAM "
					"ZNAM BMCT ETYP BIDS BAMT RNAM KSIZ KWDA DESC MODL DATA DNAM TNAM";
			else if (r == "WEAP")
				s = "EDID VMAD OBND FULL MODL MODT MODS ICON MICO EITM EAMT DEST DSTD DMDL DMDT DMDS DSTF ETYP BIDS BAMT YNAM ZNAM KSIZ KWDA DESC "
					"MOD3 MO3T MO3S EFSD NNAM INAM WNAM SNAM XNAM NAM7 TNAM UNAM NAM9 NAM8 DATA DNAM CRDT VNAM CNAM";
			else if (r == "AMMO") s = "EDID VMAD OBND FULL MODL MODT MODS ICON MICO DEST DSTD DMDL DMDT DMDS DSTF YNAM ZNAM DESC KSIZ KWDA DATA ONAM";
			else if (r == "MISC" || r == "KEYM") s = "EDID VMAD OBND FULL MODL MODT MODS ICON MICO DEST DSTD DMDL DMDT DMDS DSTF YNAM ZNAM KSIZ KWDA DATA";
			else if (r == "SLGM")
				s = "EDID OBND FULL MODL MODT MODS ICON MICO DEST DSTD DMDL DMDT DMDS DSTF YNAM ZNAM KSIZ KWDA DATA SOUL SLCP NAM0";
			else if (r == "BOOK")
				s = "EDID VMAD OBND FULL MODL MODT MODS ICON MICO DESC DEST DSTD DMDL DMDT DMDS DSTF YNAM ZNAM KSIZ KWDA DATA INAM CNAM";
			else if (r == "ALCH")
				s = "EDID VMAD OBND FULL KSIZ KWDA DESC MODL MODT MODS DEST DSTD DMDL DMDT DMDS DSTF ICON MICO YNAM ZNAM ETYP DATA ENIT EFID EFIT "
					"CTDA CIS1 CIS2";
			else if (r == "INGR")
				s = "EDID VMAD OBND FULL KSIZ KWDA MODL MODT MODS ICON MICO DEST DSTD DMDL DMDT DMDS DSTF ETYP YNAM ZNAM DATA ENIT EFID EFIT "
					"CTDA CIS1 CIS2";
			else if (r == "SCRL")
				s = "EDID VMAD OBND FULL KSIZ KWDA MDOB ETYP DESC MODL MODT MODS DEST DSTD DMDL DMDT DMDS DSTF YNAM ZNAM DATA SPIT EFID EFIT "
					"CTDA CIS1 CIS2";
			else if (r == "SPEL") s = "EDID OBND FULL KSIZ KWDA MDOB ETYP DESC SPIT EFID EFIT CTDA CIS1 CIS2";
			else if (r == "CELL")
				s = "EDID FULL DATA XCLC XCLL TVDT MHDT LTMP LNAM XCLW XNAM XCLR XLCN XWCN XWCS XWCU XCWT XOWN XRNK XILL XWEM XCCM XCAS XEZN XCMO XCIM";
			else if (r == "WRLD")
				s = "EDID RNAM MHDT FULL WCTR LTMP XEZN XLCN WNAM PNAM CNAM NAM2 NAM3 NAM4 DNAM ICON MNAM ONAM NAMA DATA NAM0 NAM9 ZNAM NNAM XNAM XWEM "
					"TNAM UNAM OFST CLSZ";
			std::vector<std::string> out;
			std::istringstream       in(s);
			for (std::string w; in >> w;) {
				out.push_back(w);
			}
			return out;
		}

		// Fallout 4: the merged types (see LayoutFO4); WRLD only as the parent of an exterior cell, as for Skyrim
		const std::vector<std::string> kTypesFO4 = { "LVLI", "LVLN", "LVSP", "CONT", "FLST", "OTFT", "MISC", "KEYM", "NPC_", "ARMO", "WEAP",
			"AMMO", "BOOK", "ALCH", "INGR", "SPEL", "CELL", "WRLD" };
		const std::vector<std::string>& kReadFO4 = kTypesFO4;
		// Starfield: the list records (see LayoutSF)
		const std::vector<std::string> kTypesSF = { "LVLI", "LVLN", "LVLP", "LVLB", "LVSC", "FLST", "OTFT" };
		const std::vector<std::string> kTypes = { "LVLI", "LVLN", "LVSP", "CONT", "FLST", "OTFT", "NPC_", "ARMO", "WEAP", "AMMO", "MISC", "KEYM",
			"SLGM", "BOOK", "ALCH", "INGR", "SCRL", "SPEL", "CELL", "WRLD" };

		std::string Serialize(const std::vector<CSub>& subs)
		{
			std::string s;
			for (const auto& c : subs) {
				s.append(c.sig, 4);
				const auto n = static_cast<std::uint32_t>(c.data.size());
				s.append(reinterpret_cast<const char*>(&n), 4);
				s.append(reinterpret_cast<const char*>(c.data.data()), c.data.size());
				for (const auto& [o, k] : c.ids) {
					s.append(reinterpret_cast<const char*>(&o), 4);
					s.append(reinterpret_cast<const char*>(&k), 8);
				}
				s.push_back('|');
			}
			return s;
		}

		// ---- stage 2: the parts of a record that are not lists ----------------------------------------
		// A unit is merged as one block: subrecords that only make sense together (a model and its texture
		// data, an NPC's face, destruction stages) are taken from one plugin. Structs with a known layout are
		// merged field by field, flags bit by bit.
		struct Field
		{
			std::uint32_t off, size;
			bool          flags = false;
			bool          unused = false;  // padding: plugins differ in it without meaning anything, never merged
		};

		// Fallout 4 cells: the previs and precombined data (timestamps, previs cell, precombined levels, previs references, combined
		// references) belong to the previs and precombined mesh files the winning mod's cell was made with: never taken from another
		// plugin, the patch keeps the winner's
		std::vector<std::string> WinnerOnlyFO4CELL() { return { "VISI", "RVIS", "PCMB", "CNAM", "ZNAM", "XPRI", "XCRI" }; }

		std::vector<std::vector<std::string>> Groups(const char* rec, bool a_fo4)
		{
			const std::string r(rec, 4);
			if (t_sf) {
				return { { "MODL", "MOLM", "FLLD", "MODC", "XFLG" } };  // a model with its material swaps, light layer, colour and flags
			}
			if (a_fo4) {
				// a model with its texture hashes, colour remap, material swap and flags; destruction with its stages; the
				// components of a misc item with their display indices (xEdit: as many as CVPA has)
				std::vector<std::vector<std::string>> g = { { "DEST", "DAMC", "DSTD", "DSTA", "DMDL", "DMDT", "DMDC", "DMDS", "DSTF" } };
				if (r != "ARMO") {
					// (ARMO: MODL are armatures, MODC belongs to its world models, ICON to the male model: see below)
					g.push_back({ "MODL", "MODT", "MODC", "MODS", "MODF" });
					g.push_back({ "ICON", "MICO" });
				}
				if (r == "MISC") g.push_back({ "CVPA", "CDIX" });
				if (r == "NPC_") {
					// the face (head parts, colours, morphs, tints, body weight: the FaceGen files are made for one set of them) and
					// the race; an attack; the actor sounds with the NPC they are inherited from
					g.push_back({ "RNAM", "PNAM", "HCLF", "BCLF", "FTST", "QNAM", "MSDK", "MSDV", "TETI", "TEND", "MRSV", "FMRI", "FMRS", "FMIN", "MWGT" });
					g.push_back({ "ATKD", "ATKE", "ATKW", "ATKS", "ATKT" });
					g.push_back({ "CS2H", "CS2K", "CS2D", "CS2E", "CS2F", "CSCR" });
				}
				if (r == "ARMO") {
					// male and female world models with their icons (MODC appears in both); the armatures (addon index + ARMA)
					g.push_back({ "MOD2", "MO2T", "MODC", "MO2S", "ICON", "MICO", "MOD4", "MO4T", "MO4S", "ICO2", "MIC2" });
					g.push_back({ "INDX", "MODL" });
				}
				if (r == "WEAP") {
					g.push_back({ "EITM", "EAMT" });
					g.push_back({ "MOD4", "MO4T", "MO4S", "MO4C", "MO4F" });  // 1st person model
				}
				if (r == "AMMO") g.push_back({ "NAM1", "NAM2" });  // casing model
				if (r == "ALCH" || r == "INGR" || r == "SPEL") g.push_back({ "EFID", "EFIT", "CTDA", "CIS1", "CIS2" });
				if (r == "CELL") {
					g.push_back({ "XCLL", "LTMP" });                           // lighting
					g.push_back({ "XCLW", "XCWT", "XWEM", "XWCN", "XWCU" });  // water
					g.push_back({ "XOWN", "XRNK" });                           // owner
					g.push_back(WinnerOnlyFO4CELL());                          // previs and precombined data: the winner's
				}
				return g;
			}
			std::vector<std::vector<std::string>> g = { { "MOD2", "MO2T", "MO2S" }, { "MOD3", "MO3T", "MO3S" }, { "MOD4", "MO4T", "MO4S" },
				{ "ICON", "MICO" }, { "ICO2", "MIC2" }, { "DEST", "DSTD", "DMDL", "DMDT", "DMDS", "DSTF" }, { "EITM", "EAMT" }, { "BODT", "BOD2" } };
			if (r != "ARMO") {
				g.push_back({ "MODL", "MODT", "MODS" });  // a model (ARMO's MODL are armatures, a unit of their own)
			}
			if (r == "NPC_") {
				// the face: head parts, colours, morphs and tints must match the FaceGen files of one mod
				// (and the race: tint layers and head parts are the race's)
				g.push_back({ "RNAM", "PNAM", "HCLF", "FTST", "QNAM", "NAM9", "NAMA", "TINI", "TINC", "TINV", "TIAS" });
				g.push_back({ "ATKD", "ATKE" });
				g.push_back({ "CSDT", "CSDI", "CSDC", "CSCR" });
			}
			if (r == "CELL") {
				g.push_back({ "XCLL", "LTMP", "LNAM" });                                   // lighting
				g.push_back({ "XCLW", "XCWT", "XNAM", "XWEM", "XWCN", "XWCS", "XWCU" });  // water
				g.push_back({ "XOWN", "XRNK" });                                           // owner
			}
			if (r == "ALCH" || r == "INGR" || r == "SCRL" || r == "SPEL") {
				g.push_back({ "EFID", "EFIT", "CTDA", "CIS1", "CIS2" });  // the magic effects with their conditions, as one list
			}
			return g;
		}

		std::vector<Field> Fields(const char* rec, const char* sub, std::size_t a_size, bool a_fo4)
		{
			const std::string k = std::string(rec, 4) + "." + std::string(sub, 4);
			std::vector<Field> f;
			if (t_sf) {
				return f;  // Starfield: every part merged as a whole
			}
			if (a_fo4) {
				// Fallout 4: only the layouts confirmed in wbDefinitionsFO4; everything else is merged as a whole
				if (k == "LVLI.LVLF" || k == "LVLN.LVLF" || k == "LVSP.LVLF") {
					if (a_size == 1) f.push_back({ 0, 1, true });
				} else if (k == "CONT.DATA") {
					if (a_size == 5) f = { { 0, 1, true }, { 1, 4 } };
				} else if (k == "MISC.DATA" || k == "KEYM.DATA") {
					if (a_size == 8) f = { { 0, 4 }, { 4, 4 } };
				}
				auto seq4 = [&](std::initializer_list<int> sizes, std::initializer_list<int> flagIdx = {}, std::initializer_list<int> unusedIdx = {}) {
					std::uint32_t o = 0;
					int           i = 0;
					for (int sz : sizes) {
						bool fl = false, un = false;
						for (int x : flagIdx) fl = fl || x == i;
						for (int x : unusedIdx) un = un || x == i;
						f.push_back({ o, static_cast<std::uint32_t>(sz), fl, un });
						o += static_cast<std::uint32_t>(sz);
						++i;
					}
					if (o != a_size) f.clear();  // another size than the layout: merged as a whole
				};
				// flags u32, XP value offset, level (or level mult), calc min, calc max, disposition, template flags, bleedout, unused
				if (k == "NPC_.ACBS") seq4({ 4, 2, 2, 2, 2, 2, 2, 2, 2 }, { 0, 6 }, { 8 });
				// aggression, confidence, energy, morality, mood, assistance, aggro radius behaviour, unknown u8, warn, warn/attack,
				// attack (u32 each), no slow approach u8, unknown 3 (from form version 29)
				else if (k == "NPC_.AIDT") {
					if (a_size == 24) seq4({ 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 1, 3 });
					else seq4({ 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4 });
				}
				else if (k == "NPC_.DNAM") seq4({ 2, 2, 2, 1, 1 }, {}, { 4 });  // calculated health, AP, far-away distance, geared-up weapons
				// ammo, speed, reload, reach, min/max range, attack delay, unused float, out-of-range mult, on hit, skill, resist,
				// flags u32, capacity u16, animation type u8, secondary damage, weight, value, base damage u16, sound level, 8 sounds,
				// accuracy bonus u8, attack seconds, unknown 2, AP cost, full power seconds, min power per shot, stagger, unknown 4
				else if (k == "WEAP.DNAM")
					seq4({ 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 1, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4, 4, 1, 4, 2, 4, 4, 4, 4, 4 }, { 12 });
				else if (k == "WEAP.FNAM") seq4({ 4, 4, 4, 4, 4, 4, 4, 1, 4, 4, 4 });
				else if (k == "WEAP.CRDT") seq4({ 4, 4, 4 });
				else if (k == "ARMO.DATA") seq4({ 4, 4, 4 });                // value, weight, health
				else if (k == "ARMO.FNAM") seq4({ 2, 2, 1, 3 }, {}, { 3 });  // armour rating, base addon index, stagger, unused
				else if (k == "AMMO.DATA") {
					if (a_size == 8) seq4({ 4, 4 });
					else seq4({ 4 });
				}
				else if (k == "AMMO.DNAM") seq4({ 4, 1, 3, 4, 4 }, { 1 }, { 2 });  // projectile, flags, unused, damage, health
				else if (k == "BOOK.DATA" || k == "INGR.DATA") seq4({ 4, 4 });
				// the flags with what the book teaches as one value (which form the bytes are depends on the flags), text offset x, y
				else if (k == "BOOK.DNAM") seq4({ 5, 4, 4 });
				else if (k == "ALCH.ENIT") seq4({ 4, 4, 4, 4, 4 }, { 1 });  // value, flags, addiction, addiction chance, sound
				else if (k == "INGR.ENIT") seq4({ 4, 4 }, { 1 });
				else if (k == "SPEL.SPIT") seq4({ 4, 4, 4, 4, 4, 4, 4, 4, 4 }, { 1 });
				else if (k == "CELL.DATA") seq4({ 2 }, { 0 });
				else if (k == "CELL.XCLC") seq4({ 4, 4, 1, 3 }, { 2 }, { 3 });
				return f;
			}
			auto seq = [&](std::initializer_list<int> sizes, std::initializer_list<int> flagIdx = {}, std::initializer_list<int> unusedIdx = {}) {
				std::uint32_t o = 0;
				int           i = 0;
				for (int sz : sizes) {
					bool fl = false, un = false;
					for (int x : flagIdx) fl = fl || x == i;
					for (int x : unusedIdx) un = un || x == i;
					f.push_back({ o, static_cast<std::uint32_t>(sz), fl, un });
					o += static_cast<std::uint32_t>(sz);
					++i;
				}
			};
			if (k == "NPC_.ACBS") seq({ 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 }, { 0, 8 });
			else if (k == "NPC_.AIDT") seq({ 1, 1, 1, 1, 1, 1, 1, 1, 4, 4, 4 }, {}, { 7 });
			else if (k == "NPC_.DNAM") {
				for (std::uint32_t i = 0; i < 36; ++i) f.push_back({ i, 1, false });
				f.push_back({ 36, 2 }); f.push_back({ 38, 2 }); f.push_back({ 40, 2 }); f.push_back({ 42, 2, false, true });
				f.push_back({ 44, 4 }); f.push_back({ 48, 1 }); f.push_back({ 49, 3, false, true });
			}
			else if (k == "WEAP.DATA") seq({ 4, 4, 2 });
			else if (k == "WEAP.DNAM")
				seq({ 1, 3, 4, 4, 2, 2, 4, 4, 1, 1, 1, 1, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 4, 4, 4 }, { 4, 15 }, { 1, 5 });
			else if (k == "WEAP.CRDT") { if (a_size == 24) seq({ 2, 2, 4, 1, 7, 4, 4 }, {}, { 1, 4, 6 }); else seq({ 2, 2, 4, 1, 3, 4 }, {}, { 1, 4 }); }
			else if (k == "ARMO.DATA") seq({ 4, 4 });
			else if (k == "AMMO.DATA") { if (a_size == 20) seq({ 4, 4, 4, 4, 4 }, { 1 }); else seq({ 4, 4, 4, 4 }, { 1 }); }
			else if (k == "CONT.DATA") seq({ 1, 4 }, { 0 });
			else if (k == "LVLI.LVLF" || k == "LVLN.LVLF" || k == "LVSP.LVLF") seq({ 1 }, { 0 });
			else if (k == "CELL.DATA") { if (a_size == 2) seq({ 2 }, { 0 }); else seq({ 1 }, { 0 }); }
			else if (k == "CELL.XCLC") seq({ 4, 4, 1, 3 }, { 2 }, { 3 });
			else if (k == "MISC.DATA" || k == "KEYM.DATA" || k == "SLGM.DATA" || k == "INGR.DATA" || k == "SCRL.DATA") seq({ 4, 4 });
			else if (k == "BOOK.DATA") seq({ 1, 1, 2, 4, 4, 4 }, { 0 }, { 2 });
			else if (k == "ALCH.ENIT") seq({ 4, 4, 4, 4, 4 }, { 1 });
			else if (k == "INGR.ENIT") seq({ 4, 4 }, { 1 });
			else if (k == "SCRL.SPIT" || k == "SPEL.SPIT") seq({ 4, 4, 4, 4, 4, 4, 4, 4, 4 }, { 1 });
			std::uint32_t total = 0;
			for (const auto& x : f) total += x.size;
			if (total != a_size) {
				f.clear();  // another size than the layout: merged as a whole
			}
			return f;
		}

		Category ListCategory(const char* rec, const ListSpec& sp)
		{
			const std::string r(rec, 4), e(sp.entry, 4);
			if (e == "KWDA") return Category::kKeywords;
			if (r == "LVLI" || r == "LVLN" || r == "LVSP" || r == "LVLP" || r == "LVLB" || r == "LVSC") return Category::kLeveledLists;
			if (r == "CONT") return Category::kContainers;
			if (r == "OTFT") return Category::kOutfits;
			if (r == "FLST") return Category::kFormLists;
			if (r == "NPC_") return Category::kNpcLists;
			if (r == "CELL") return Category::kCellRegions;
			return Category::kItemValues;
		}

		Category UnitCategory(const char* rec, const std::set<std::string>& u)
		{
			const std::string r(rec, 4);
			auto has = [&](const char* s) { return u.contains(s); };
			if (has("VMAD")) return Category::kScripts;
			if (r == "CELL") {
				if (has("XCLL")) return Category::kCellLighting;
				if (has("XCLW")) return Category::kCellWater;
				if (has("EDID") || has("FULL")) return Category::kNames;
				return Category::kCellOther;
			}
			if (r == "NPC_" && has("PNAM")) return Category::kNpcFace;
			if (has("EFID")) return Category::kEffects;
			if (has("EDID") || has("FULL") || has("DESC") || has("SHRT") || (r == "BOOK" && has("CNAM")) || ((r == "LVLI" || r == "LVLN") && has("ONAM")) ||
				(r == "AMMO" && has("ONAM")) || has("ATTX"))
				return Category::kNames;  // (Fallout 4: LVLI ONAM override name, AMMO ONAM short name, NPC_ ATTX activate text)
			if ((r == "AMMO" && has("NAM1")) || has("BOD2")) return Category::kModels;  // (Fallout 4: the casing model, body slots)
			if (has("OBND") || has("PTRN") || has("MODL") || has("MOD2") || has("MOD3") || has("MOD4") || has("ICON") || has("ICO2") || has("DEST") || has("MDOB") ||
				has("BODT"))
				return Category::kModels;
			if (r == "NPC_") return Category::kNpcValues;
			return Category::kItemValues;
		}

		struct Entry
		{
			std::string       key;    // identity (with an occurrence number for repeats)
			std::string       value;  // everything, to see a changed value
			std::vector<CSub> subs;
		};

		class Builder
		{
		public:
			Builder(const Options& a_opt, Progress* a_progress, Result& a_res) :
				_opt(a_opt), _progress(a_progress), _res(a_res), _fo4(a_opt.game == 2), _sf(a_opt.game == 3) {}

			void run()
			{
				load();
				if (cancelled()) {
					_res.error = "cancelled";
					return;
				}
				learnRenumbering();
				merge();
				if (cancelled()) {
					_res.error = "cancelled";
					return;
				}
				write();
			}

		private:
			bool cancelled() const { return _progress && _progress->cancel; }

			std::uint64_t key(int a_plugin, std::uint32_t a_raw, bool& a_ok) const
			{
				a_ok = true;
				if (a_raw == 0) {
					return 0;
				}
				const auto& p = _plugins[a_plugin];
				const auto  m = a_raw >> 24;
				int         lo = a_plugin;
				if (m < p.masterLo.size()) {
					lo = p.masterLo[m];
					if (lo < 0) {
						a_ok = false;
						return 0;
					}
				}
				return (static_cast<std::uint64_t>(lo + 1) << 32) | (a_raw & 0xFFFFFF);
			}

			std::string idText(std::uint64_t k) const
			{
				if (!k) {
					return "NULL";
				}
				const auto lo = static_cast<int>(k >> 32) - 1;
				return _plugins[lo].name + ":0x" + Hex(static_cast<std::uint32_t>(k & 0xFFFFFF));
			}

			void load()
			{
				const auto n = _opt.loadOrder.size();
				_plugins.resize(n);
				std::unordered_map<std::string, int> index;
				for (std::size_t i = 0; i < n; ++i) {
					_plugins[i].name = _opt.loadOrder[i];
					index.emplace(Lower(_opt.loadOrder[i]), static_cast<int>(i));
				}
				if (_progress) {
					_progress->total = n;
					_progress->done = 0;
				}
				for (std::size_t i = 0; i < n && !cancelled(); ++i) {
					auto& p = _plugins[i];
					if (_progress) {
						_progress->set(p.name);
					}
					if (Lower(p.name) == Lower(_opt.patchName)) {
						if (_progress) ++_progress->done;
						continue;  // an older merged patch is not an input
					}
					const auto path = _opt.findPlugin ? _opt.findPlugin(p.name) : std::nullopt;
					if (!path) {
						_res.missing.push_back(p.name);
						if (_progress) ++_progress->done;
						continue;
					}
					PluginFile pf;
					try {
						pf = ReadPlugin(*path, _sf ? kTypesSF : _fo4 ? kReadFO4 : kTypes);
					} catch (const std::exception& e) {
						_res.missing.push_back(p.name + " (" + e.what() + ")");
						if (_progress) ++_progress->done;
						continue;
					}
					p.found = true;
					p.localized = pf.flags & 0x80;
					// Legendary Edition has no light plugins; Starfield's light (0x100) and medium (0x400) plugins are left out of the
					// light-plugin checks (compacted numbers), which are Skyrim's and Fallout 4's
					p.light = !_opt.legendary && !_sf && (pf.flags & 0x200);
					for (const auto& m : pf.masters) {
						auto it = index.find(Lower(m));
						const int lo = it == index.end() || it->second >= static_cast<int>(i) ? -1 : it->second;
						p.masterLo.push_back(lo);
						if (lo >= 0) {
							p.masterSet.insert(lo);
						}
					}
					++_res.pluginsRead;
					if (p.light) {
						// a light plugin can only number its own forms 0x000-0xFFF; the game cuts higher ones short
						const auto over = CountLightOverflow(*path, static_cast<std::uint32_t>(pf.masters.size()));
						if (over) {
							_res.warnings.push_back(p.name + " is flagged light (ESL) but numbers " + std::to_string(over) +
													" of its own forms above 0xFFF. The game cuts those IDs short, so they point at "
													"other forms or at nothing; the mod needs compacting in SSEEdit, or the ESL flag removed.");
						}
					}
					for (auto& r : pf.records) {
						bool ok;
						const auto k = key(static_cast<int>(i), r.formID, ok);
						if (!ok || !k) {
							continue;
						}
						if (Is(r.sig, "NPC_")) {
							_npcs.insert(k);
						}
						auto& vs = _versions[k];
						if (vs.empty()) {
							_order.push_back(k);
						}
						vs.push_back({ static_cast<int>(i), std::move(r) });
						++_res.recordsRead;
					}
					if (_progress) ++_progress->done;
				}
			}

			const std::unordered_map<std::uint32_t, std::string>& strings(int a_plugin)
			{
				auto& p = _plugins[a_plugin];
				if (!p.stringsLoaded) {
					p.stringsLoaded = true;
					auto stem = Lower(std::filesystem::path(p.name).stem().string());
					for (const char* ext : { ".strings", ".dlstrings", ".ilstrings" }) {
						for (const auto& lang : { Lower(_opt.language), std::string(_fo4 || _sf ? "en" : "english") }) {
							const auto data = _opt.readStrings ? _opt.readStrings(stem + "_" + lang + ext) : std::vector<std::uint8_t>{};
							if (data.size() < 8) {
								continue;
							}
							const bool lenPrefixed = std::strcmp(ext, ".strings") != 0;
							const auto count = U32(data.data()), size = U32(data.data() + 4);
							const std::size_t start = 8 + std::size_t(count) * 8;
							if (start > data.size()) {
								continue;
							}
							for (std::uint32_t e = 0; e < count; ++e) {
								const auto id = U32(data.data() + 8 + e * 8), off = U32(data.data() + 12 + e * 8);
								std::size_t q = start + off;
								if (off >= size || q >= data.size()) {
									continue;
								}
								std::size_t len = data.size() - q;
								if (lenPrefixed) {
									if (q + 4 > data.size()) continue;
									len = std::min<std::size_t>(U32(data.data() + q), data.size() - q - 4);
									q += 4;
								}
								const auto* s = reinterpret_cast<const char*>(data.data() + q);
								p.strings.emplace(id, std::string(s, strnlen(s, len)));
							}
							break;
						}
					}
				}
				return p.strings;
			}

			// the comparable form of one version of a record; nullopt (with a reason) if it cannot be read safely
			std::optional<CRec> canon(const Ver& v, std::string& err, std::size_t* a_remapped = nullptr)
			{
				CRec c;
				std::memcpy(c.sig, v.rec.sig, 4);
				c.flags = v.rec.flags;
				c.version = v.rec.version;
				// a localized text: the text itself from the plugin's strings files, so versions compare by what they say
				auto lstring = [&](const Sub& s, std::vector<std::uint8_t>& out) {
					if (!_plugins[v.plugin].localized) {
						out = s.data;
						return true;
					}
					if (s.data.size() != 4) {
						err = "a localized text (" + Sig(s.sig) + ") is not a string id";
						return false;
					}
					const auto  id = U32(s.data.data());
					std::string text;
					if (id) {
						const auto& t = strings(v.plugin);
						auto        it = t.find(id);
						if (it == t.end()) {
							err = "the text of " + Sig(s.sig) + " is not in " + _plugins[v.plugin].name + "'s strings files";
							return false;
						}
						text = it->second;
					}
					out.assign(text.begin(), text.end());
					out.push_back(0);
					return true;
				};
				for (std::size_t si = 0; si < v.rec.subs.size(); ++si) {
					const auto& s = v.rec.subs[si];
					CSub cs;
					std::memcpy(cs.sig, s.sig, 4);
					cs.data = s.data;
					std::vector<std::uint32_t> offs;
					if (_fo4 && Is(s.sig, "OBTE")) {
						// Fallout 4's object template (wbObjectTemplate): OBTE count, then each combination (OBTF editor-only marker,
						// FULL name, OBTS), then STOP. Its FULL would mix with the record's own name, so the whole template is kept as
						// one part here: the subrecords one after the other (signature, u16 size, data), unpacked again in write()
						cs.data.clear();
						bool done = false;
						for (; si < v.rec.subs.size() && !done; ++si) {
							const auto& in = v.rec.subs[si];
							std::vector<std::uint8_t> d;
							std::vector<std::uint32_t> inner;
							if (Is(in.sig, "OBTE") || Is(in.sig, "OBTF") || Is(in.sig, "STOP")) {
								d = in.data;
								done = Is(in.sig, "STOP");
							} else if (Is(in.sig, "FULL")) {
								if (!lstring(in, d)) return std::nullopt;
							} else if (Is(in.sig, "OBTS")) {
								d = in.data;
								if (!ObtsForms(d, inner)) {
									err = "an object template entry (OBTS) could not be read";
									return std::nullopt;
								}
							} else {
								err = "has a part (" + Sig(in.sig) + ") in its object template that ReSaver Plus does not know yet";
								return std::nullopt;
							}
							if (d.size() > 0xFFFF) {
								err = "an object template part is too large";
								return std::nullopt;
							}
							const auto base = static_cast<std::uint32_t>(cs.data.size() + 6);
							PutSig(cs.data, in.sig);
							Put16(cs.data, static_cast<std::uint16_t>(d.size()));
							cs.data.insert(cs.data.end(), d.begin(), d.end());
							for (const auto o : inner) offs.push_back(base + o);
						}
						--si;
						if (!done) {
							err = "its object template (OBTE) has no end marker (STOP)";
							return std::nullopt;
						}
					} else switch (Layout(v.rec.sig, s.sig, _fo4)) {
					case Kind::kUnknown:
						err = "has a part (" + Sig(s.sig) + ") ReSaver Plus does not know yet";
						return std::nullopt;
					case Kind::kNone: break;
					case Kind::kDrop: continue;
					case Kind::kLString:
						if (!lstring(s, cs.data)) return std::nullopt;
						break;
					case Kind::kFixed:
						if (!FixedForms(v.rec.sig, s.sig, s.data, offs)) {
							err = Sig(s.sig) + " has a size (" + std::to_string(s.data.size()) + " bytes) its layout does not have";
							return std::nullopt;
						}
						break;
					case Kind::kForm0:
						if (s.data.size() < 4) {
							err = Sig(s.sig) + " is too short";
							return std::nullopt;
						}
						offs.push_back(0);
						break;
					case Kind::kFormPairs:
						if (s.data.size() % 8) {
							err = Sig(s.sig) + " has a bad size";
							return std::nullopt;
						}
						if (Is(s.sig, "DAMA") && v.rec.version >= 152) {
							// from record version 152 (Fallout 76's) each entry has a curve table too: 12 bytes
							err = "DAMA of record version " + std::to_string(v.rec.version) + " is not Fallout 4's layout";
							return std::nullopt;
						}
						for (std::uint32_t o = 0; o < s.data.size(); o += 8) {
							offs.push_back(o);
						}
						break;
					case Kind::kForm12:
						if (s.data.size() % 12) {
							err = Sig(s.sig) + " has a bad size";
							return std::nullopt;
						}
						for (std::uint32_t o = 0; o < s.data.size(); o += 12) {
							offs.push_back(o);
						}
						break;
					case Kind::kFormArr:
						if (s.data.size() % 4) {
							err = Sig(s.sig) + " has a bad size";
							return std::nullopt;
						}
						for (std::uint32_t o = 0; o < s.data.size(); o += 4) {
							offs.push_back(o);
						}
						break;
					case Kind::kVmad:
						if (!VmadForms(s.data, offs, err, _fo4)) {
							return std::nullopt;
						}
						break;
					case Kind::kAltTex:
						if (!AltTexForms(s.data, offs)) {
							err = "alternate textures (" + Sig(s.sig) + ") could not be read";
							return std::nullopt;
						}
						break;
					case Kind::kCoed:
						{
							if (s.data.size() < 12) {
								err = "COED is too short";
								return std::nullopt;
							}
							offs.push_back(0);
							bool ok;
							const auto owner = key(v.plugin, U32(s.data.data()), ok);
							if (ok && owner && _npcs.contains(owner)) {
								offs.push_back(4);  // the owner is an NPC: a global variable follows
							}
							break;
						}
					case Kind::kDstd:
						if (s.data.size() < 20) {
							err = "DSTD is too short";
							return std::nullopt;
						}
						offs = { 8, 12 };
						break;
					case Kind::kAtkd:
						if (s.data.size() < 12) {
							err = "ATKD is too short";
							return std::nullopt;
						}
						offs = { 8 };
						break;
					case Kind::kLvlo:
						if (s.data.size() < 8) {
							err = "LVLO is too short";
							return std::nullopt;
						}
						offs = { 4 };
						break;
					case Kind::kBookData:
						if (s.data.size() < 8) {
							err = "book DATA is too short";
							return std::nullopt;
						}
						if ((s.data[0] & 0x04) && !(s.data[0] & 0x01)) {
							offs = { 4 };  // teaches a spell
						}
						break;
					case Kind::kAlchEnit:
						if (s.data.size() != 20) {
							err = "ENIT has an unexpected size";
							return std::nullopt;
						}
						offs = { 8, 16 };
						break;
					case Kind::kSpit:
						if (s.data.size() != 36) {
							err = "SPIT has an unexpected size";
							return std::nullopt;
						}
						offs = { 32 };
						break;
					case Kind::kCtda:
						{
							if (s.data.size() != 32) {
								err = "a condition has an unexpected size";
								return std::nullopt;
							}
							const auto fn = U16(s.data.data() + 8);
							const ConditionFunction* cf = nullptr;
							if (_sf) {
								// Starfield's own function table (wbDefinitionsSF1), the same alias / package data rule as Fallout 4's;
								// the reference @24 (run on 2) and the linked keyword @28 (run on 4: wbConditionParam3Decider)
								for (const auto& f : kConditionFunctionsSF) {
									if (f.index == fn) {
										cf = &f;
										break;
									}
								}
								if (!cf) {
									err = "a condition uses function " + std::to_string(fn) + ", which is not in Starfield's function table";
									return std::nullopt;
								}
								const auto type = s.data[0];
								const auto runOn = U32(s.data.data() + 20);
								auto isForm = [&](std::uint8_t a_p, bool a_first) {
									if (a_p != 2) return a_p == 1;
									if (type & 0x02) return a_first && fn == 161 && runOn == 5;  // GetIsCurrentPackage on a quest alias
									return !(type & 0x08);
								};
								if (type & 0x04) offs.push_back(4);
								if (isForm(cf->p1, true)) offs.push_back(12);
								if (isForm(cf->p2, false)) offs.push_back(16);
								if (runOn == 2) offs.push_back(24);
								if (runOn == 4) offs.push_back(28);
								break;
							}
							if (_fo4) {
								// Fallout 4's own function table (wbDefinitionsFO4). Parameters that are a reference, an actor or a
								// package (2) are a number when the condition's flags say alias (0x02) or package data (0x08)
								for (const auto& f : kConditionFunctionsFO4) {
									if (f.index == fn) {
										cf = &f;
										break;
									}
								}
								if (!cf) {
									err = "a condition uses function " + std::to_string(fn) + ", which is not in Fallout 4's function table";
									return std::nullopt;
								}
								const auto type = s.data[0];
								const auto runOn = U32(s.data.data() + 20);
								auto isForm = [&](std::uint8_t a_p, bool a_first) {
									if (a_p != 2) return a_p == 1;
									if (type & 0x02) return a_first && fn == 161 && runOn == 5;  // GetIsCurrentPackage on a quest alias
									return !(type & 0x08);
								};
								if (type & 0x04) offs.push_back(4);  // compared with a global variable
								if (isForm(cf->p1, true)) offs.push_back(12);
								if (isForm(cf->p2, false)) offs.push_back(16);
								if (runOn == 2) offs.push_back(24);  // run on a reference
								break;
							}
							for (const auto& f : kConditionFunctions) {
								if (f.index == fn) {
									cf = &f;
									break;
								}
							}
							if (!cf || cf->p1 == 2 || cf->p2 == 2) {
								err = "a condition uses function " + std::to_string(fn) + ", whose parameters ReSaver Plus cannot read yet";
								return std::nullopt;
							}
							if (s.data[0] & 0x04) offs.push_back(4);  // compared with a global variable
							if (cf->p1 == 1) offs.push_back(12);
							if (cf->p2 == 1) offs.push_back(16);
							if (U32(s.data.data() + 20) == 2) offs.push_back(24);  // run on a reference
							break;
						}
					case Kind::kCrdt:
						if (s.data.size() == 24) {
							offs = { 16 };
						} else if (s.data.size() == 16) {
							offs = { 12 };
						} else {
							err = "CRDT has an unexpected size";
							return std::nullopt;
						}
						break;
					}
					for (const auto o : offs) {
						if (o + 4 > cs.data.size()) {
							err = Sig(s.sig) + " is too short";
							return std::nullopt;
						}
						bool ok;
						const auto k = key(v.plugin, U32(cs.data.data() + o), ok);
						if (!ok) {
							err = "points to a form of a plugin that is not loaded (" + Sig(s.sig) + " in " + _plugins[v.plugin].name + ": " +
								  Hex(U32(cs.data.data() + o), 8) + ")";
							return std::nullopt;
						}
						std::memset(cs.data.data() + o, 0, 4);
						auto fixedKey = k;
						if (k && (k & 0xFFFFFF) > 0xFFF && !_learning) {
							const int target = static_cast<int>(k >> 32) - 1;
							if (target != v.plugin && _plugins[target].light) {
								// a light plugin has no forms above 0xFFF: an old number from before it was compacted
								const auto obj = static_cast<std::uint32_t>(k & 0xFFFFFF);
								auto it = _opt.enabled[static_cast<int>(Category::kRenumberFix)] ? _remap.find({ target, obj }) : _remap.end();
								if (it != _remap.end()) {
									fixedKey = (k & ~std::uint64_t(0xFFFFFF)) | it->second;
									_fixed[{ v.plugin, target }].insert(obj);
									if (a_remapped) {
										++*a_remapped;
									}
								} else {
									_badLight[{ v.plugin, target }].insert(obj);
								}
							}
						}
						cs.ids.push_back({ o, fixedKey });
					}
					c.subs.push_back(std::move(cs));
				}
				return c;
			}

			std::vector<Entry> entries(const CRec& c, const ListSpec& sp) const
			{
				std::vector<Entry> out;
				for (std::size_t i = 0; i < c.subs.size(); ++i) {
					const auto& s = c.subs[i];
					if (!Is(s.sig, sp.entry)) {
						continue;
					}
					if (sp.array) {
						for (const auto& [o, k] : s.ids) {
							Entry e;
							CSub  one;
							std::memcpy(one.sig, sp.entry, 4);
							one.data.assign(4, 0);
							one.ids.push_back({ 0, k });
							e.subs.push_back(std::move(one));
							out.push_back(std::move(e));
						}
						continue;
					}
					Entry e;
					e.subs.push_back(s);
					while (sp.follower && i + 1 < c.subs.size() && Follows(c.subs[i + 1].sig, sp.follower)) {
						e.subs.push_back(c.subs[++i]);
					}
					out.push_back(std::move(e));
				}
				std::map<std::string, int> seen;
				for (auto& e : out) {
					e.value = Serialize(e.subs);
					std::string id;
					if (sp.byForm && !e.subs[0].ids.empty()) {
						const auto k = e.subs[0].ids[0].second;
						id.assign(reinterpret_cast<const char*>(&k), 8);
					} else {
						id = e.value;
					}
					e.key = id + "#" + std::to_string(seen[id]++);
				}
				return out;
			}

			// a_patch gets what a_ovr changed compared with a_base; returns a short description of it
			std::string apply(std::vector<Entry>& a_patch, const std::vector<Entry>& a_base, const std::vector<Entry>& a_ovr, const ListSpec& sp)
			{
				std::unordered_map<std::string, const Entry*> base, ovr;
				for (const auto& e : a_base) base.emplace(e.key, &e);
				for (const auto& e : a_ovr) ovr.emplace(e.key, &e);
				std::size_t added = 0, removed = 0, changed = 0;
				// removed by the override
				for (const auto& e : a_base) {
					if (ovr.contains(e.key)) {
						continue;
					}
					auto it = std::find_if(a_patch.begin(), a_patch.end(), [&](const Entry& p) { return p.key == e.key; });
					if (it != a_patch.end()) {
						a_patch.erase(it);
						++removed;
					}
				}
				// added or changed by the override
				for (const auto& e : a_ovr) {
					auto b = base.find(e.key);
					auto it = std::find_if(a_patch.begin(), a_patch.end(), [&](const Entry& p) { return p.key == e.key; });
					if (b == base.end()) {
						if (it == a_patch.end()) {
							a_patch.push_back(e);
							++added;
						}
					} else if (sp.byForm && b->second->value != e.value && it != a_patch.end() && it->value != e.value) {
						*it = e;
						++changed;
					}
				}
				std::string out;
				auto part = [&](std::size_t n, const char* sign) {
					if (n) {
						out += (out.empty() ? "" : ", ") + std::string(sign) + std::to_string(n);
					}
				};
				part(added, "+");
				part(removed, "-");
				part(changed, "~");
				return out.empty() ? out : out + " " + sp.what;
			}

			// puts a list back into the record: the count, then the entries, where the list was (or belongs)
			bool place(CRec& c, const ListSpec& sp, const std::vector<Entry>& es, std::string& err) const
			{
				if (sp.max && es.size() > sp.max) {
					err = "the merged list would have " + std::to_string(es.size()) + " " + sp.what + " (the game takes at most " +
						  std::to_string(sp.max) + ")";
					return false;
				}
				// where: the first count or entry subrecord, else by the type's order
				std::size_t pos = SIZE_MAX;
				std::vector<CSub> kept;
				bool afterEntry = false;
				for (const auto& s : c.subs) {
					const bool isEntry = Is(s.sig, sp.entry);
					const bool isCount = sp.count && Is(s.sig, sp.count);
					const bool isFollower = sp.follower && afterEntry && Follows(s.sig, sp.follower);
					if (isEntry || isCount || isFollower) {
						if (pos == SIZE_MAX) {
							pos = kept.size();
						}
						afterEntry = isEntry || isFollower;
						continue;
					}
					afterEntry = false;
					kept.push_back(s);
				}
				if (pos == SIZE_MAX) {
					const auto order = Order(c.sig, _fo4);
					auto rank = [&](const char* sig) {
						for (std::size_t i = 0; i < order.size(); ++i) {
							if (Is(sig, order[i].c_str())) return static_cast<int>(i);
						}
						return -1;
					};
					const int mine = rank(sp.count ? sp.count : sp.entry);
					pos = kept.size();
					for (std::size_t i = 0; i < kept.size(); ++i) {
						if (rank(kept[i].sig) > mine) {
							pos = i;
							break;
						}
					}
				}
				std::vector<CSub> ins;
				const bool lvl = sp.count && Is(sp.count, "LLCT");
				std::vector<const Entry*> order;
				for (const auto& e : es) order.push_back(&e);
				if ((_fo4 || _sf) && Is(sp.entry, "LVLO")) {
					// Fallout 4: entries by level, as the Creation Kit and xEdit (a sorted array there) write them
					std::stable_sort(order.begin(), order.end(), [](const Entry* a, const Entry* b) {
						auto level = [](const Entry* e) { return e->subs[0].data.size() >= 2 ? U16(e->subs[0].data.data()) : 0; };
						return level(a) < level(b);
					});
				}
				if (sp.count && (!es.empty() || lvl)) {
					CSub cnt;
					std::memcpy(cnt.sig, sp.count, 4);
					const auto n = static_cast<std::uint32_t>(es.size());
					for (int i = 0; i < sp.countSize; ++i) {
						cnt.data.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
					}
					ins.push_back(std::move(cnt));
				}
				if (sp.array) {
					if (!es.empty()) {
						CSub arr;
						std::memcpy(arr.sig, sp.entry, 4);
						for (const auto& e : es) {
							arr.ids.push_back({ static_cast<std::uint32_t>(arr.data.size()), e.subs[0].ids[0].second });
							arr.data.insert(arr.data.end(), 4, 0);
						}
						ins.push_back(std::move(arr));
					}
				} else {
					for (const auto* e : order) {
						ins.insert(ins.end(), e->subs.begin(), e->subs.end());
					}
				}
				kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(pos), ins.begin(), ins.end());
				c.subs = std::move(kept);
				return true;
			}

			std::string edidOf(const Record& r) const
			{
				for (const auto& s : r.subs) {
					if (Is(s.sig, "EDID")) {
						return std::string(reinterpret_cast<const char*>(s.data.data()), strnlen(reinterpret_cast<const char*>(s.data.data()), s.data.size()));
					}
				}
				return {};
			}

			// A plugin made for an older version of a light master points at its forms by their old numbers. Where
			// it overrides a record the master also has, the same place in both versions shows the old number and
			// the new one (Bijin AIO - Hair Physics.esp and Bijin AIO.esp: each NPC's head parts). Only numbers
			// that always pair with the same new number are used.
			void learnRenumbering()
			{
				std::map<std::pair<int, std::uint32_t>, std::set<std::uint32_t>> votes;
				_learning = true;
				for (const auto k : _order) {
					if (cancelled()) {
						break;
					}
					const auto& vs = _versions[k];
					for (std::size_t i = 1; i < vs.size(); ++i) {
						const auto& P = _plugins[vs[i].plugin];
						for (std::size_t j = 0; j < i; ++j) {
							const int m = vs[j].plugin;
							if (!_plugins[m].light || !P.masterSet.contains(m)) {
								continue;
							}
							std::string err;
							auto a = canon(vs[i], err), b = canon(vs[j], err);
							if (!a || !b) {
								continue;
							}
							std::map<std::string, std::vector<const CSub*>> bs;
							for (const auto& sb : b->subs) bs[Sig(sb.sig)].push_back(&sb);
							std::map<std::string, std::size_t> seen;
							for (const auto& sa : a->subs) {
								const auto  sig = Sig(sa.sig);
								const auto  n = seen[sig]++;
								const auto& list = bs[sig];
								if (n >= list.size() || list[n]->ids.size() != sa.ids.size()) {
									continue;
								}
								for (std::size_t t = 0; t < sa.ids.size(); ++t) {
									const auto ka = sa.ids[t].second, kb = list[n]->ids[t].second;
									if (ka && kb && (ka >> 32) == static_cast<std::uint64_t>(m + 1) && (kb >> 32) == (ka >> 32) &&
										(ka & 0xFFFFFF) > 0xFFF && (kb & 0xFFFFFF) <= 0xFFF) {
										votes[{ m, static_cast<std::uint32_t>(ka & 0xFFFFFF) }].insert(static_cast<std::uint32_t>(kb & 0xFFFFFF));
									}
								}
							}
						}
					}
				}
				_learning = false;
				for (const auto& [old, news] : votes) {
					if (news.size() == 1) {
						_remap[old] = *news.begin();
					}
				}
			}

			static std::vector<CSub> Block(const CRec& c, const std::set<std::string>& sigs)
			{
				std::vector<CSub> out;
				for (const auto& s : c.subs) {
					if (sigs.contains(Sig(s.sig))) out.push_back(s);
				}
				return out;
			}

			// puts a block into the record: each subrecord where its type belongs (a unit can be spread over the record,
			// like an NPC's face: head parts after the skills, tints at the end), keeping the block's own order
			void SetBlock(CRec& c, const std::set<std::string>& sigs, const std::vector<CSub>& block) const
			{
				const auto order = Order(c.sig, _fo4);
				auto rank = [&](const char* sig) {
					for (std::size_t i = 0; i < order.size(); ++i) {
						if (Is(sig, order[i].c_str())) return static_cast<int>(i);
					}
					return -1;
				};
				std::vector<CSub> kept;
				std::size_t       firstRemoved = SIZE_MAX;
				for (const auto& s : c.subs) {
					if (sigs.contains(Sig(s.sig))) {
						if (firstRemoved == SIZE_MAX) firstRemoved = kept.size();
						continue;
					}
					kept.push_back(s);
				}
				std::size_t last = 0;  // just after the block's previous subrecord
				bool        any = false;
				for (const auto& b : block) {
					const int   mine = rank(b.sig);
					std::size_t pos;
					if (mine < 0) {
						pos = any ? last : (firstRemoved == SIZE_MAX ? kept.size() : firstRemoved);
					} else {
						pos = kept.size();
						for (std::size_t i = any ? last : 0; i < kept.size(); ++i) {
							const int r = rank(kept[i].sig);
							if (r > mine) {
								pos = i;
								break;
							}
						}
					}
					kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(pos), b);
					last = pos + 1;
					any = true;
					if (firstRemoved != SIZE_MAX && pos <= firstRemoved) ++firstRemoved;
				}
				c.subs = std::move(kept);
			}

			static bool SameField(const CSub& a, const CSub& b, const Field& f)
			{
				if (std::memcmp(a.data.data() + f.off, b.data.data() + f.off, f.size) != 0) return false;
				std::vector<std::uint64_t> ia, ib;
				for (const auto& [o, k] : a.ids) if (o >= f.off && o < f.off + f.size) ia.push_back(k);
				for (const auto& [o, k] : b.ids) if (o >= f.off && o < f.off + f.size) ib.push_back(k);
				return ia == ib;
			}

			// a_patch gets the parts of a_ovr that differ from a_base; returns what was taken
			// a_patch gets the parts of a_ovr that differ from a_base; returns what was taken, by kind
			std::map<Category, std::string> applyUnits(CRec& a_patch, const CRec& a_base, const CRec& a_ovr)
			{
				// the list parts are merged entry by entry, not here
				std::set<std::string> listSigs;
				for (const auto& sp : Lists(a_patch.sig, _fo4)) {
					listSigs.insert(sp.entry);
					if (sp.follower) {
						std::istringstream fs(sp.follower);
						for (std::string f; fs >> f;) listSigs.insert(f);
					}
					if (sp.count) listSigs.insert(sp.count);
				}
				// the units: the groups, then every other part on its own
				std::vector<std::set<std::string>> units;
				std::set<std::string>              grouped;
				for (const auto& g : Groups(a_patch.sig, _fo4)) {
					units.emplace_back(g.begin(), g.end());
					grouped.insert(g.begin(), g.end());
				}
				for (const CRec* c : std::initializer_list<const CRec*>{ &a_base, &a_ovr, &a_patch }) {
					for (const auto& s : c->subs) {
						const auto sig = Sig(s.sig);
						if (!grouped.contains(sig) && !listSigs.contains(sig)) {
							grouped.insert(sig);
							units.push_back({ sig });
						}
					}
				}
				std::map<Category, std::string> taken;
				auto took = [&](Category c, const std::string& what) { taken[c] += (taken[c].empty() ? "" : ", ") + what; };
				for (const auto& u : units) {
					if (_fo4 && Is(a_patch.sig, "CELL") && u.contains("XCRI")) {
						continue;  // previs and precombined data: always the winner's (WinnerOnlyFO4CELL)
					}
					const auto cat = UnitCategory(a_patch.sig, u);
					if (!_opt.enabled[static_cast<int>(cat)]) {
						continue;
					}
					const auto b = Block(a_base, u), o = Block(a_ovr, u);
					if (Serialize(b) == Serialize(o)) {
						continue;  // this plugin did not change it
					}
					auto       p = Block(a_patch, u);
					const auto name = u.size() > 1 && u.contains("PNAM") ? std::string("face") : Sig(o.empty() ? b.front().sig : o.front().sig);
					// one struct in all three: field by field
					if (u.size() == 1 && b.size() == 1 && o.size() == 1 && p.size() == 1 && b[0].data.size() == o[0].data.size() &&
						o[0].data.size() == p[0].data.size()) {
						const auto fields = Fields(a_patch.sig, o[0].sig, o[0].data.size(), _fo4);
						if (!fields.empty()) {
							std::size_t n = 0;
							for (const auto& f : fields) {
								if (f.unused || SameField(b[0], o[0], f) || SameField(p[0], o[0], f)) {
									continue;
								}
								if (f.flags) {
									for (std::uint32_t i = 0; i < f.size; ++i) {
										const auto mask = static_cast<std::uint8_t>(b[0].data[f.off + i] ^ o[0].data[f.off + i]);
										p[0].data[f.off + i] = static_cast<std::uint8_t>((p[0].data[f.off + i] & ~mask) | (o[0].data[f.off + i] & mask));
									}
								} else {
									std::memcpy(p[0].data.data() + f.off, o[0].data.data() + f.off, f.size);
									std::erase_if(p[0].ids, [&](const auto& x) { return x.first >= f.off && x.first < f.off + f.size; });
									for (const auto& x : o[0].ids) {
										if (x.first >= f.off && x.first < f.off + f.size) p[0].ids.push_back(x);
									}
									std::sort(p[0].ids.begin(), p[0].ids.end());
								}
								++n;
							}
							if (n) {
								SetBlock(a_patch, u, p);
								took(cat, name + " (" + std::to_string(n) + (n == 1 ? " value)" : " values)"));
							}
							continue;
						}
					}
					if (Serialize(p) != Serialize(o)) {
						SetBlock(a_patch, u, o);
						took(cat, name);
					}
				}
				return taken;
			}

			// ---- NPC parts that belong together ------------------------------------------------------------
			// ACBS layout: flags u32 @0, magicka @4, stamina @6, level (or level multiplier x1000 when flag 0x80
			// "PC level mult" is set) @8, calc min level @10, calc max level @12, speed @14, disposition @16,
			// template flags @18, health @20, bleedout @22.
			//  - level: flag 0x80 and the level / min / max fields. With the flag the level field is a multiplier;
			//    taken from two plugins it becomes a nonsense level (or multiplier) that breaks leveled actors.
			//  - template: the template flags (which parts come from the template) and TPLT (the template NPC).
			// For each, the last plugin (in load order) that changed it from the version it was made against wins,
			// and the patch gets that plugin's values as one piece.
			// Fallout 4 (wbDefinitionsFO4, ACBS 20 bytes): flags @0 (0x80 PC level mult), XP value offset @4, level (or level
			// multiplier) @6, calc min level @8, calc max level @10, disposition @12, template flags @14, bleedout @16; the
			// template is TPLT (default template) with TPTA (a template actor for each kind of data).
			struct NpcAcbs
			{
				std::size_t size, level, tflags;
			};
			NpcAcbs Acbs() const { return _fo4 ? NpcAcbs{ 20, 6, 14 } : NpcAcbs{ 24, 8, 18 }; }
			std::set<std::string> TemplateSigs() const { return _fo4 ? std::set<std::string>{ "TPLT", "TPTA" } : std::set<std::string>{ "TPLT" }; }
			struct NpcLink
			{
				bool                      hasAcbs = false;
				std::uint8_t              pcLevelMult = 0;
				std::vector<std::uint8_t> level;     // level, min and max (6 bytes)
				std::vector<std::uint8_t> tflags;    // template flags (2 bytes)
				std::vector<CSub>         tplt;      // the template subrecords
			};
			NpcLink ReadNpcLink(const CRec& c) const
			{
				NpcLink    l;
				const auto a = Acbs();
				const auto t = TemplateSigs();
				for (const auto& s : c.subs) {
					if (Is(s.sig, "ACBS") && s.data.size() >= a.size && !l.hasAcbs) {
						l.hasAcbs = true;
						l.pcLevelMult = s.data[0] & 0x80;
						l.level.assign(s.data.begin() + static_cast<std::ptrdiff_t>(a.level), s.data.begin() + static_cast<std::ptrdiff_t>(a.level + 6));
						l.tflags.assign(s.data.begin() + static_cast<std::ptrdiff_t>(a.tflags), s.data.begin() + static_cast<std::ptrdiff_t>(a.tflags + 2));
					} else if (t.contains(Sig(s.sig))) {
						l.tplt.push_back(s);
					}
				}
				return l;
			}
			static bool SameLevel(const NpcLink& a, const NpcLink& b) { return a.pcLevelMult == b.pcLevelMult && a.level == b.level; }
			static bool SameTemplate(const NpcLink& a, const NpcLink& b) { return a.tflags == b.tflags && a.tplt == b.tplt; }

			// returns (what, plugin) for each part it had to put back together
			std::vector<std::pair<std::string, std::string>> linkNpcParts(CRec& a_patch, const std::vector<CRec>& a_cv,
				const std::vector<Ver>& a_vs)
			{
				std::vector<std::pair<std::string, std::string>> out;
				std::vector<NpcLink> links;
				for (const auto& c : a_cv) links.push_back(ReadNpcLink(c));
				// the last override that changed a part from its own base version
				int lastLevel = -1, lastTemplate = -1;
				for (std::size_t i = 1; i < a_cv.size(); ++i) {
					std::size_t b = 0;
					for (std::size_t j = i; j-- > 1;) {
						if (_plugins[a_vs[i].plugin].masterSet.contains(a_vs[j].plugin)) {
							b = j;
							break;
						}
					}
					if (!links[i].hasAcbs || !links[b].hasAcbs) continue;
					if (!SameLevel(links[i], links[b])) lastLevel = static_cast<int>(i);
					if (!SameTemplate(links[i], links[b])) lastTemplate = static_cast<int>(i);
				}
				const auto now = ReadNpcLink(a_patch);
				if (!now.hasAcbs) return out;
				const auto ac = Acbs();
				CSub* acbs = nullptr;
				for (auto& s : a_patch.subs) {
					if (Is(s.sig, "ACBS") && s.data.size() >= ac.size) {
						acbs = &s;
						break;
					}
				}
				if (!acbs) return out;
				if (lastLevel >= 0 && !SameLevel(now, links[lastLevel])) {
					const auto& src = links[lastLevel];
					acbs->data[0] = static_cast<std::uint8_t>((acbs->data[0] & ~0x80) | src.pcLevelMult);
					std::copy(src.level.begin(), src.level.end(), acbs->data.begin() + static_cast<std::ptrdiff_t>(ac.level));
					out.push_back({ "PC level multiplier with level and min/max level", _plugins[a_vs[lastLevel].plugin].name });
				}
				if (lastTemplate >= 0 && !SameTemplate(ReadNpcLink(a_patch), links[lastTemplate])) {
					const auto& src = links[lastTemplate];
					for (auto& s : a_patch.subs) {
						if (Is(s.sig, "ACBS") && s.data.size() >= ac.size) {
							std::copy(src.tflags.begin(), src.tflags.end(), s.data.begin() + static_cast<std::ptrdiff_t>(ac.tflags));
							break;
						}
					}
					SetBlock(a_patch, TemplateSigs(), src.tplt);
					out.push_back({ "template with template flags", _plugins[a_vs[lastTemplate].plugin].name });
				}
				return out;
			}

			static void note(MergedRecord& a_mr, const std::string& a_text, Category a_cat)
			{
				a_mr.changes.push_back(a_text);
				a_mr.categories.push_back(a_cat);
				a_mr.risk = std::max(a_mr.risk, CategoryRisk(a_cat));
			}

			void merge()
			{
				if (_progress) {
					_progress->set("merging records");
					_progress->total = _order.size();
					_progress->done = 0;
				}
				for (const auto k : _order) {
					if (cancelled()) {
						return;
					}
					if (_progress) ++_progress->done;
					auto& vs = _versions[k];
					const int definer = static_cast<int>(k >> 32) - 1;
					if (vs.front().plugin != definer) {
						continue;  // its own plugin is not loaded
					}
					if (Is(vs.back().rec.sig, "WRLD")) {
						continue;  // worldspaces are not merged: one is copied in only as the parent of an exterior cell
					}
					const auto& win = vs.back();
					const std::string label = Sig(win.rec.sig) + " " + edidOf(win.rec) + " [" + idText(k) + "]";
					// only one version that counts: nothing to merge, but its references to renumbered forms can be fixed
					auto fixOnly = [&] {
						if (_remap.empty() || (win.rec.flags & 0x20)) {
							return;
						}
						std::string err;
						std::size_t n = 0;
						auto        c = canon(win, err, &n);
						if (!c || !n) {
							return;
						}
						MergedRecord mr;
						mr.type = Sig(win.rec.sig);
						mr.edid = edidOf(win.rec);
						mr.id = idText(k);
						mr.winner = _plugins[win.plugin].name;
						note(mr, "fixed " + std::to_string(n) + " reference(s) to renumbered forms of a compacted plugin", Category::kRenumberFix);
						_patched.push_back({ k, std::move(*c) });
						_res.merged.push_back(std::move(mr));
					};
					if (vs.size() < 3) {
						fixOnly();
						continue;
					}
					// all overrides the same? then there is no conflict
					bool differ = false;
					for (std::size_t i = 2; i < vs.size() && !differ; ++i) {
						differ = !(vs[i].rec.subs.size() == vs[1].rec.subs.size() &&
								   std::equal(vs[i].rec.subs.begin(), vs[i].rec.subs.end(), vs[1].rec.subs.begin(), [](const Sub& a, const Sub& b) {
									   return Is(a.sig, b.sig) && a.data == b.data;
								   }));
					}
					if (!differ) {
						fixOnly();
						continue;
					}
					++_res.conflicts;
					bool deleted = false;
					for (const auto& v : vs) {
						deleted = deleted || (v.rec.flags & 0x20);
					}
					if (deleted) {
						_res.skipped.push_back(label + ": a plugin deletes it");
						continue;
					}
					// a partial form (record flag 0x4000, cells and some other types) holds only some of the record: the game
					// fills in the rest from the version before it. Merged as a whole record, what it leaves out would look
					// removed, and a patch built on it may not hold more than it does.
					bool partial = false;
					for (const auto& v : vs) {
						partial = partial || (v.rec.flags & 0x4000);
					}
					if (partial && (Is(win.rec.sig, "CELL") || Is(win.rec.sig, "WRLD"))) {
						_res.skipped.push_back(label + ": a plugin's version is a partial form (the game combines those itself)");
						continue;
					}
					std::vector<CRec> cv;
					std::string       err;
					std::size_t       winFixes = 0;
					for (std::size_t vi = 0; vi < vs.size(); ++vi) {
						const auto& v = vs[vi];
						auto c = canon(v, err, vi + 1 == vs.size() ? &winFixes : nullptr);
						if (!c) {
							break;
						}
						cv.push_back(std::move(*c));
					}
					if (cv.size() != vs.size()) {
						_res.skipped.push_back(label + ": " + err);
						continue;
					}
					CRec patch = cv.back();
					MergedRecord mr;
					mr.type = Sig(win.rec.sig);
					mr.edid = edidOf(win.rec);
					mr.id = idText(k);
					mr.winner = _plugins[win.plugin].name;
					if (winFixes) {
						note(mr, "fixed " + std::to_string(winFixes) + " reference(s) to renumbered forms of a compacted plugin", Category::kRenumberFix);
					}
					// the values first: each plugin's changes in load order, so the last one to change a part wins
					for (std::size_t i = 1; i < vs.size(); ++i) {
						std::size_t b = 0;
						for (std::size_t j = i; j-- > 1;) {
							if (_plugins[vs[i].plugin].masterSet.contains(vs[j].plugin)) {
								b = j;
								break;
							}
						}
						for (const auto& [cat, what] : applyUnits(patch, cv[b], cv[i])) {
							if (i + 1 != vs.size()) {
								note(mr, _plugins[vs[i].plugin].name + ": " + what, cat);
							}
						}
					}
					// NPC parts that only mean something together must come from ONE plugin, the way a manual xEdit
					// patch takes them (merged value by value they can mix into an NPC no plugin made):
					if (Is(win.rec.sig, "NPC_") && _opt.enabled[static_cast<int>(Category::kNpcValues)]) {
						for (const auto& [what, n] : linkNpcParts(patch, cv, vs)) {
							note(mr, n + ": " + what + " taken together", Category::kNpcValues);
						}
					}
					bool failed = false;
					for (const auto& sp : Lists(win.rec.sig, _fo4)) {
						const auto cat = ListCategory(win.rec.sig, sp);
						if (!_opt.enabled[static_cast<int>(cat)]) {
							continue;
						}
						auto list = entries(patch, sp);
						const auto before = Serialize([&] { std::vector<CSub> a; for (auto& e : list) a.insert(a.end(), e.subs.begin(), e.subs.end()); return a; }());
						for (std::size_t i = 1; i < vs.size(); ++i) {
							// the version this override was made against: the last one among its plugin's masters
							std::size_t b = 0;
							for (std::size_t j = i; j-- > 1;) {
								if (_plugins[vs[i].plugin].masterSet.contains(vs[j].plugin)) {
									b = j;
									break;
								}
							}
							const auto what = apply(list, entries(cv[b], sp), entries(cv[i], sp), sp);
							if (!what.empty() && i + 1 != vs.size()) {
								note(mr, _plugins[vs[i].plugin].name + ": " + what, cat);
							}
						}
						const auto after = Serialize([&] { std::vector<CSub> a; for (auto& e : list) a.insert(a.end(), e.subs.begin(), e.subs.end()); return a; }());
						if (after == before) {
							continue;
						}
						if (!place(patch, sp, list, err)) {
							failed = true;
							break;
						}
					}
					if (failed) {
						_res.skipped.push_back(label + ": " + err);
						continue;
					}
					if (SameSubs(patch, cv.back()) && !winFixes) {
						continue;  // the winning override already has everything
					}
					_patched.push_back({ k, std::move(patch) });
					_res.merged.push_back(std::move(mr));
				}
			}

			void write()
			{
				// masters: the plugin of every patched record and of every form a patched record points to
				// an exterior cell needs its worldspace in the patch (the winning version, as xEdit copies it)
				std::unordered_map<std::uint64_t, std::uint64_t> cellWorld;  // cell key -> worldspace key
				{
					std::set<std::uint64_t>            have;
					std::vector<std::pair<std::uint64_t, CRec>> parents;
					for (const auto& [k, c] : _patched) have.insert(k);
					std::vector<std::uint64_t> drop;
					for (const auto& [k, c] : _patched) {
						if (!Is(c.sig, "CELL")) continue;
						const auto& win = _versions[k].back();
						if (win.rec.groups.empty() || win.rec.groups[0].first != 1) continue;
						bool       ok;
						const auto wk = key(win.plugin, win.rec.groups[0].second, ok);
						auto       wv = _versions.find(wk);
						std::string err = "its worldspace was not found";
						std::optional<CRec> wc;
						if (ok && wv != _versions.end() && !wv->second.empty()) {
							wc = canon(wv->second.back(), err);
						}
						if (!wc) {
							drop.push_back(k);
							_res.skipped.push_back("CELL " + idText(k) + ": its worldspace could not be copied (" + err + ")");
							continue;
						}
						cellWorld[k] = wk;
						if (have.insert(wk).second) parents.push_back({ wk, std::move(*wc) });
					}
					if (!drop.empty()) {
						std::erase_if(_patched, [&](const auto& x) { return std::find(drop.begin(), drop.end(), x.first) != drop.end(); });
						std::erase_if(_res.merged, [&](const MergedRecord& m) {
							for (const auto d : drop) if (m.id == idText(d) && m.type == "CELL") return true;
							return false;
						});
					}
					for (auto& p : parents) _patched.push_back(std::move(p));
				}
				std::set<int> lo;
				auto need = [&](std::uint64_t k, const std::string& why) {
					if (!k) {
						return;
					}
					const int p = static_cast<int>(k >> 32) - 1;
					if (lo.insert(p).second) {
						_res.masterWhy[_plugins[p].name] = why;
					}
				};
				for (const auto& [k, c] : _patched) {
					const auto why = Sig(c.sig) + " " + idText(k);
					need(k, why);
					for (const auto& s : c.subs) {
						for (const auto& [o, id] : s.ids) {
							need(id, why);
						}
					}
				}
				for (const auto& [pt, olds] : _fixed) {
					_res.fixes.push_back(_plugins[pt.first].name + " was made for an older version of " + _plugins[pt.second].name +
										 ", before it was compacted: " + std::to_string(olds.size()) +
										 " of its references to it used old form numbers. The patch points them at the same forms' new numbers.");
				}
				for (const auto& [pt, olds] : _badLight) {
					_res.warnings.push_back(_plugins[pt.first].name + " points at " + std::to_string(olds.size()) + " form(s) of " + _plugins[pt.second].name +
											" numbered above 0xFFF, but " + _plugins[pt.second].name + " is light (ESL) and has no such forms: " +
											_plugins[pt.first].name + " was made for an older, uncompacted version of it, and the new numbers "
											"could not be worked out for these. They stay broken, with or without the patch.");
				}
				if (lo.size() > 254) {
					_res.error = "the patch would need " + std::to_string(lo.size()) + " masters; a plugin can have at most 254";
					return;
				}
				std::unordered_map<int, std::uint32_t> idx;
				for (const int p : lo) {
					idx[p] = static_cast<std::uint32_t>(_res.masters.size());
					_res.masters.push_back(_plugins[p].name);
				}
				// form IDs below 0x800 of other plugins than Skyrim.esm: a header version 1.70 plugin reads those as
				// Skyrim.esm's hardcoded forms, so the patch needs version 1.71 then (as plugins with such forms have)
				bool low = false;
				auto raw = [&](std::uint64_t k) -> std::uint32_t {
					if (!k) {
						return 0;
					}
					const int p = static_cast<int>(k >> 32) - 1;
					if (!_fo4 && !_sf && (k & 0xFFFFFF) < 0x800 && Lower(_plugins[p].name) != "skyrim.esm") {
						low = true;
					}
					return (idx.at(static_cast<int>(k >> 32) - 1) << 24) | static_cast<std::uint32_t>(k & 0xFFFFFF);
				};
				std::vector<Record> recs;
				for (const auto& [k, c] : _patched) {
					Record r;
					std::memcpy(r.sig, c.sig, 4);
					r.flags = c.flags & ~0x40000u;
					r.version = c.version;
					r.formID = raw(k);
					if (Is(c.sig, "CELL")) {
						r.groups = _versions[k].back().rec.groups;  // the winner's block and sub-block
						if (auto it = cellWorld.find(k); it != cellWorld.end() && !r.groups.empty()) {
							r.groups[0].second = raw(it->second);
						}
					}
					for (const auto& s : c.subs) {
						Sub o;
						std::memcpy(o.sig, s.sig, 4);
						o.data = s.data;
						for (const auto& [off, id] : s.ids) {
							const auto v = raw(id);
							std::memcpy(o.data.data() + off, &v, 4);
						}
						if (_fo4 && Is(s.sig, "OBTE")) {
							// an object template kept as one part (see canon): its subrecords again
							for (auto& in : ParseSubs(o.data.data(), o.data.size())) r.subs.push_back(std::move(in));
							continue;
						}
						r.subs.push_back(std::move(o));
					}
					recs.push_back(std::move(r));
				}
				if (_opt.legendary) {
					_res.light = false;
					// Legendary Edition: a normal plugin in its own format (header 0.94, record version 43). It has no
					// light plugins, so the patch takes a load order slot: the game loads at most 255 plugins.
					std::size_t used = 0;
					for (const auto& p : _plugins) {
						if (Lower(p.name) != Lower(_opt.patchName)) {
							++used;
						}
					}
					if (used >= 255) {
						_res.warnings.push_back("Legendary Edition loads at most 255 plugins and your load order already has " +
												std::to_string(used) + ". The patch needs a slot of its own: disable or merge a plugin, or the "
												"game will not load the patch (or the plugins after it).");
					}
					_res.file = WritePlugin(_res.masters, 0, recs, "ReSaver Plus",
						"Merged patch made by ReSaver Plus. Rebuild it after changing your mods.", 0.94f, 43);
				} else if (_sf) {
					// Starfield: its header (HEDR and record version as Starfield.esm has them, MAST without DATA, INCC), its group
					// order. Only overrides, no new forms: a light master (0x1 | 0x100), as nearly all Starfield mods are masters
					// (masters load before every non-master plugin, so a normal .esp patch would load after them all)
					_res.file = WritePlugin(_res.masters, _opt.light ? 0x101u : 0x1u, recs, "ReSaver Plus",
						"Merged patch made by ReSaver Plus. Rebuild it after changing your mods.", _opt.hedrVersion > 0 ? _opt.hedrVersion : 0.96f,
						_opt.headerRecordVersion ? _opt.headerRecordVersion : 581, false);
					_res.light = _opt.light;
				} else if (_fo4) {
					// Fallout 4: its header (HEDR and record version as Fallout4.esm has them, INTV, INCC), its group order.
					// Only overrides, no new forms: flagged light (ESL, 0x200) unless asked not to (Fallout 4 before 1.10).
					_res.file = WritePlugin(_res.masters, _opt.light ? 0x200u : 0u, recs, "ReSaver Plus",
						"Merged patch made by ReSaver Plus. Rebuild it after changing your mods.", _opt.hedrVersion > 0 ? _opt.hedrVersion : 1.0f,
						_opt.headerRecordVersion ? _opt.headerRecordVersion : 131, true);
					_res.light = _opt.light;
				} else {
					// only overrides, no new forms: flagged light (ESL), so it takes no load order slot
					_res.file = WritePlugin(_res.masters, _opt.light ? 0x200u : 0u, recs, "ReSaver Plus",
						"Merged patch made by ReSaver Plus. Rebuild it after changing your mods.", low ? 1.71f : 1.7f);
					_res.light = _opt.light;
				}
				_res.fo4 = _fo4;
				_res.sf = _sf;
				_res.legendary = _opt.legendary;
				_res.ok = true;
			}

			const Options&                                   _opt;
			Progress*                                        _progress;
			Result&                                          _res;
			const bool                                       _fo4;  // a Fallout 4 load order
			const bool                                       _sf;   // a Starfield load order
			std::vector<PInfo>                               _plugins;
			std::unordered_map<std::uint64_t, std::vector<Ver>> _versions;
			std::vector<std::uint64_t>                       _order;  // record keys in the order first seen
			std::unordered_set<std::uint64_t>                _npcs;
			std::vector<std::pair<std::uint64_t, CRec>>      _patched;
			// references to light (ESL) masters numbered above 0xFFF: old numbers from before the master was compacted.
			// (plugin, light master) -> the old numbers, fixed or not
			std::map<std::pair<int, int>, std::set<std::uint32_t>> _badLight, _fixed;
			std::map<std::pair<int, std::uint32_t>, std::uint32_t> _remap;  // (light master, old number) -> its number now
			bool                                             _learning = false;
		};
	}

	Result Build(const Options& a_opt, Progress* a_progress)
	{
		Result r;
		try {
			// Legendary Edition: Skyrim.esm's header has record version 43 (SE/AE 44). The patch is then written in
			// LE's format, as a normal plugin (LE has no light plugins).
			Options opt = a_opt;
			if (opt.game < 0) {
				opt.game = 0;
				for (const auto& p : opt.loadOrder) {
					if (Lower(p) == "fallout4.esm") {
						opt.game = 2;
						break;
					}
				}
			}
			struct SfFlag
			{
				explicit SfFlag(bool a_on) { t_sf = a_on; }
				~SfFlag() { t_sf = false; }
			} sfFlag(opt.game == 3);
			if (opt.game == 3) {
				// Starfield: the patch's header takes Starfield.esm's HEDR version and record version (0.96 and 581 on 1.16.244)
				if (opt.findPlugin) {
					if (const auto esm = opt.findPlugin("Starfield.esm")) {
						std::ifstream f(*esm, std::ios::binary);
						unsigned char h[34] = {};
						if (f.read(reinterpret_cast<char*>(h), sizeof(h)) && std::memcmp(h, "TES4", 4) == 0 && std::memcmp(h + 24, "HEDR", 4) == 0) {
							std::memcpy(&opt.hedrVersion, h + 30, 4);
							opt.headerRecordVersion = static_cast<std::uint16_t>(h[20] | (h[21] << 8));
							if (!(opt.hedrVersion > 0.5f && opt.hedrVersion < 2.0f)) opt.hedrVersion = 0.0f;
						}
					}
				}
				if (Lower(opt.language) == "english") opt.language = "en";  // Starfield_en.strings
			} else if (opt.game == 2) {
				// Fallout 4: the patch's header takes Fallout4.esm's HEDR version and record version (1.0 and 131 on the
				// current game)
				if (opt.findPlugin) {
					if (const auto esm = opt.findPlugin("Fallout4.esm")) {
						std::ifstream f(*esm, std::ios::binary);
						unsigned char h[34] = {};
						if (f.read(reinterpret_cast<char*>(h), sizeof(h)) && std::memcmp(h, "TES4", 4) == 0 && std::memcmp(h + 24, "HEDR", 4) == 0) {
							std::memcpy(&opt.hedrVersion, h + 30, 4);
							opt.headerRecordVersion = static_cast<std::uint16_t>(h[20] | (h[21] << 8));
							if (!(opt.hedrVersion > 0.5f && opt.hedrVersion < 2.0f)) opt.hedrVersion = 0.0f;
						}
					}
				}
				if (Lower(opt.language) == "english") opt.language = "en";  // Fallout4_en.strings
			} else if (opt.findPlugin) {
				if (const auto esm = opt.findPlugin("Skyrim.esm")) {
					std::ifstream f(*esm, std::ios::binary);
					unsigned char h[24] = {};
					if (f.read(reinterpret_cast<char*>(h), sizeof(h)) && std::memcmp(h, "TES4", 4) == 0 && (h[20] | (h[21] << 8)) < 44) {
						opt.legendary = true;
					}
				}
			}
			Builder b(opt, a_progress, r);
			b.run();
		} catch (const std::exception& e) {
			r.ok = false;
			r.error = e.what();
		}
		return r;
	}

	void UseSources(Options& a_opt, const ScriptSources& a_sources)
	{
		const auto* src = &a_sources;
		a_opt.findPlugin = [src](const std::string& a_name) { return src->pluginPath(a_name); };
		a_opt.readStrings = [src](const std::string& a_file) -> std::vector<std::uint8_t> {
			auto it = src->stringsFiles.find(a_file);
			if (it == src->stringsFiles.end()) {
				return {};
			}
			try {
				if (it->second.bsaEntry.empty()) {
					std::ifstream f(it->second.file, std::ios::binary);
					return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
				}
				BsaArchive bsa(it->second.file);
				if (const auto* e = bsa.find(it->second.bsaEntry)) {
					return bsa.extract(*e);
				}
			} catch (const std::exception&) {
			}
			return {};
		};
	}

	const std::vector<std::string>& MergedTypes(bool a_fo4) { return a_fo4 ? kTypesFO4 : kTypes; }

	const char* Fo4NotMerged()
	{
		return "Fallout 4: worldspaces are not merged (one is copied in only as the parent of a merged exterior cell, as for Skyrim), "
			   "and a cell's previs and precombined data always stays as the winning mod has it: it belongs to that mod's previs and "
			   "precombined files. Any record with a part ReSaver Plus does not know is left to the winning mod, and the report says "
			   "which part.";
	}

	const char* SfMerged()
	{
		return "Starfield: the patch merges leveled lists (items, NPCs, packins, base forms and space cells: their entries, entry "
			   "conditions, keyword chances, and the list settings), form lists and outfits. Every other record type is not merged. "
			   "A list that has a part ReSaver Plus does not read yet (scripts, form components, material swaps, an entry's owner, "
			   "a form list's conditional entries) is left to the winning mod, and the report says which part.";
	}

	const char* CategoryDetailSF(Category a_cat)
	{
		switch (a_cat) {
		case Category::kLeveledLists:
			return "Leveled items, leveled NPCs, leveled packins, leveled base forms and leveled space cells: each entry (level, what it "
				   "gives, count, chance none) with its conditions, the entry count, and the keyword chances (LLKC). What every mod "
				   "added or removed is combined, entries sorted by level. A list that would grow past 255 entries is left to the "
				   "winning mod.";
		case Category::kOutfits: return "Outfits: the items in each outfit. What every mod added or removed is combined.";
		case Category::kFormLists:
			return "Form lists: their entries. What every mod added or removed is combined; added entries go at the end. A form list "
				   "with conditional entries is left to the winning mod.";
		case Category::kItemValues:
			return "A leveled list's settings: chance none and its global, max count, flags, use-all, the list's own conditions, epic "
				   "loot chance, transforms, snap and default forms, filter keywords.";
		case Category::kNames: return "Editor IDs, form list names and the override names of leveled items and NPCs.";
		case Category::kModels: return "A leveled list's bounds, preview model with its material swaps, colour and flags.";
		default: return "";
		}
	}

	bool CategoryAppliesSF(Category a_cat)
	{
		switch (a_cat) {
		case Category::kLeveledLists:
		case Category::kOutfits:
		case Category::kFormLists:
		case Category::kItemValues:
		case Category::kNames:
		case Category::kModels:
			return true;
		default:
			return false;
		}
	}

	bool CategoryApplies(Category, bool)
	{
		return true;  // every kind of change can happen for Fallout 4 too since its NPCs, items, spells and cells are merged
	}

	const char* CategoryName(Category a_cat)
	{
		switch (a_cat) {
		case Category::kLeveledLists: return "Leveled lists: entries of leveled items, leveled NPCs and leveled spells";
		case Category::kContainers: return "Container items";
		case Category::kOutfits: return "Outfit items";
		case Category::kFormLists: return "Form list entries";
		case Category::kKeywords: return "Keywords";
		case Category::kNpcLists: return "NPC factions, spells, perks and inventory";
		case Category::kNpcFace: return "NPC faces and races";
		case Category::kNpcValues: return "NPC stats, AI, skills, class, voice, outfits, packages and other NPC settings";
		case Category::kItemValues: return "Values and settings of weapons, armour, ammo, items, books, potions, ingredients, scrolls, spells, containers and leveled lists";
		case Category::kEffects: return "Magic effects (with their conditions) of potions, ingredients, scrolls and spells";
		case Category::kNames: return "Names, descriptions, book text and editor IDs";
		case Category::kModels: return "Models, textures, icons, armatures, body slots, bounds and destruction";
		case Category::kScripts: return "Scripts attached to records, with their properties";
		case Category::kCellLighting: return "Cell lighting";
		case Category::kCellWater: return "Cell water";
		case Category::kCellRegions: return "Cell regions";
		case Category::kCellOther: return "Cell location, owner, encounter zone, music, sound, image space, flags and grid";
		case Category::kRenumberFix: return "Fix references to compacted light (ESL) plugins";
		default: return "?";
		}
	}

	const char* CategoryDetail(Category a_cat, bool a_fo4)
	{
		if (!a_fo4) {
			return CategoryDetail(a_cat);
		}
		switch (a_cat) {
		case Category::kLeveledLists:
			return "Leveled item, leveled NPC and leveled spell lists: each entry (level, the item, NPC, spell or list it gives, count, "
				   "chance none, and the entry's owner and item condition) and the entry count. What every mod added or removed is "
				   "combined, entries sorted by level as the Creation Kit writes them. A list that would grow past 255 entries is left "
				   "to the winning mod. The list's own settings are under Values and settings.";
		case Category::kContainers:
			return "Containers: each item with its count, owner and item condition, and the item count. What every mod added or "
				   "removed, and count changes, are combined.";
		case Category::kOutfits:
			return "Outfits: the armour pieces and leveled items in each outfit. What every mod added or removed is combined.";
		case Category::kFormLists:
			return "Form lists (lists of forms that scripts, conditions, workshop menus and crafting read): their entries. What every mod "
				   "added or removed is combined; added entries go at the end.";
		case Category::kKeywords:
			return "Keywords on NPCs, containers, armour, weapons, ammunition, misc items, keys, books, potions, ingredients and spells, "
				   "with the keyword count. What every mod added or removed is combined.";
		case Category::kNpcLists:
			return "NPCs: factions (with rank), spells and leveled spells (with their count), perks (with rank and the perk count), and "
				   "inventory items (with count, owner and item condition, and the item count). What every mod added or removed, and "
				   "rank or count changes, are combined.";
		case Category::kNpcFace:
			return "NPCs, taken as one set from the last mod that changed any part of it: race, head parts, hair and facial hair colour, "
				   "head texture, texture lighting, morph keys and values, face tint layers (with their value and colour), body morph "
				   "regions, face morphs, facial morph intensity and body weight (thin, muscular, fat).";
		case Category::kNpcValues:
			return "NPCs, each value from the last mod that changed it. Configuration: flags (female, essential, respawn, unique, "
				   "protected, PC level mult, simple actor, invulnerable...), XP value offset, level or level multiplier with the "
				   "minimum and maximum calculated level (taken together from one mod), disposition, template flags with the default "
				   "template and the template actors (taken together from one mod), bleedout override. AI data: aggression, "
				   "confidence, energy, morality, mood, assistance, aggro radius behaviour and distances, no slow approach. Also: death "
				   "item, voice, legendary template and chance, preview transform, animation sound, skin, far-away model, attack race, "
				   "attacks (damage, chance, spell, flags, angles, stagger, event, weapon and required slot), the spectator, "
				   "observe-dead-body, guard-warn, combat, follower command and follower elevator package lists, properties, forced "
				   "location ref type, native terminal, AI packages, attach parent slots, object template (default mods), class, "
				   "calculated health and action points, far-away model distance, geared-up weapons, height, sound level, actor sounds "
				   "or the NPC they are inherited from, power armor stand, default and sleeping outfit, default package list, crime "
				   "faction, combat style and gift filter.";
		case Category::kItemValues:
			return "Each value from the last mod that changed it. Weapons: ammo, speed, reload speed, reach, minimum and maximum range, "
				   "attack delay, out-of-range damage, on hit, skill, resist, flags (automatic, bolt action, has scope...), capacity, "
				   "animation type, base and secondary damage, weight, value, sound level, attack, attack 2D, attack loop, attack fail, "
				   "idle, equip, unequip and fast-equip sounds, accuracy bonus, attack seconds, action point cost, full power seconds, "
				   "minimum power per shot, stagger, firing (fire and reload seconds, rumble, sighted transition, number of projectiles, "
				   "override projectile), critical damage, critical charge bonus and critical effect, enchantment and its amount, "
				   "impact data set, NPC ammo list, aim model, zoom, template, damage types, embedded weapon mod, melee speed. Armour: "
				   "value, weight, health, armour rating, base addon index, stagger rating, resistances, race, template armour, "
				   "enchantment. Weapons and armour: equip type, block bash impact data set and material, instance naming, attach "
				   "parent slots, object template (default mods). Ammunition: value, weight, projectile, flags, damage, health. Books: "
				   "value, weight, flags together with what the book teaches (actor value, spell or perk), text offset, featured item "
				   "message, inventory art. Potions: weight, value, flags (food, medicine, poison...), addiction, addiction chance, "
				   "consume and crafting sounds, equip type. Ingredients: value, weight, flags. Spells: base cost, flags, type, charge "
				   "time, cast type, target type, cast duration, range, casting perk, equip type. Leveled lists: chance none, max count, "
				   "flags (calculate from all levels, for each item in count, use all), chance-none global, keyword chances, epic loot "
				   "chance. Containers: flags (respawns, show owner...), weight, properties, forced location ref type, native terminal, "
				   "open, close and take-all sounds, filter list. Misc items and keys: value, weight, featured item message, components "
				   "(with their display indices). Items: pick-up and put-down sounds.";
		case Category::kEffects:
			return "Potions, ingredients and spells: the whole list of magic effects, each with its magnitude, area and duration and its "
				   "conditions (read with Fallout 4's own condition functions), taken as one list from the last mod that changed it.";
		case Category::kNames:
			return "Editor IDs, names, NPC short names and activate text, descriptions, book text, a leveled item's override name, "
				   "ammunition short names and a potion's addiction name, from the last mod that changed each, on every merged record "
				   "type, cells included.";
		case Category::kModels:
			return "From the last mod that changed each: models with their texture data, colour remapping, material swap and flags "
				   "(world model, male and female world models, 1st person model, ammunition casing model), inventory icons, armour "
				   "armatures (ARMA, with their addon index), body slots, object bounds, preview transform and destruction data "
				   "(health, resistances, stages, explosion, debris, damaged models).";
		case Category::kScripts:
			return "The scripts attached to NPCs, containers, armour, weapons, ammunition, misc items, keys, books, potions and "
				   "ingredients, with every property value (structs included), taken as one set from the last mod that changed it.";
		case Category::kCellLighting:
			return "Cells, taken as one set from the last mod that changed any part of it: the lighting (ambient, directional and fog "
				   "colours, fog distances, power and clip distance, light fade, near and far height fog, which values come from the "
				   "lighting template) and the lighting template.";
		case Category::kCellWater:
			return "Cells, taken as one set from the last mod that changed any part of it: water height, water type, water environment "
				   "map and water currents.";
		case Category::kCellOther:
			return "Cells, each from the last mod that changed it: location, owner (an NPC or a faction, with its no-crime flag) and "
				   "required faction rank, lock list, exterior LOD, sky and weather from a region, acoustic space, encounter zone, music "
				   "type, image space, god rays, cell flags, exterior grid position and land flags, terrain visibility and maximum "
				   "height data. A cell's previs and precombined data (timestamps, previs cell, precombined object levels, previs and "
				   "combined references) always stays as the winning mod has it: it belongs to that mod's previs and precombined files.";
		default:
			return CategoryDetail(a_cat);
		}
	}

	const char* CategoryDetail(Category a_cat)
	{
		switch (a_cat) {
		case Category::kLeveledLists:
			return "Leveled item, leveled NPC and leveled spell lists: each entry (level, the item, NPC, spell or list it gives, count, and "
				   "the entry's owner and condition data) and the entry count. What every mod added or removed is combined. A list that "
				   "would grow past 255 entries is left to the winning mod. The list's own settings are under Values and settings.";
		case Category::kContainers:
			return "Containers (chests, barrels, sacks, merchant chests...): each item with its count, owner and condition data, and the "
				   "item count. What every mod added or removed, and count changes, are combined.";
		case Category::kOutfits:
			return "Outfits (what NPCs wear by default and to sleep): the armour pieces and leveled items in each outfit. What every mod "
				   "added or removed is combined.";
		case Category::kFormLists:
			return "Form lists (lists of forms that scripts, conditions, crafting and quests read): their entries. What every mod added "
				   "or removed is combined; added entries go at the end.";
		case Category::kKeywords:
			return "Keywords on NPCs, armour, weapons, ammunition, misc items, keys, soul gems, books, potions, ingredients, scrolls and "
				   "spells, with the keyword count. What every mod added or removed is combined.";
		case Category::kNpcLists:
			return "NPCs: factions (with rank), spells, shouts and leveled spells (with their count), perks (with rank and the perk "
				   "count), and inventory items (with count, owner and condition data, and the item count). What every mod added or "
				   "removed, and rank or count changes, are combined.";
		case Category::kNpcFace:
			return "NPCs, taken as one set from the last mod that changed any part of it: race, head parts (head, eyes, hair, brows, "
				   "beard, scars), hair colour, face texture set, texture lighting colour, face morphs (nose, jaw, cheeks, eyes, brows, "
				   "lips, chin), face parts (nose, eyes, mouth), and every tint layer (skin tone, war paint, dirt...) with its colour, "
				   "strength and preset.";
		case Category::kNpcValues:
			return "NPCs, each value from the last mod that changed it. Configuration: flags (female, essential, protected, unique, "
				   "respawn, auto-calc stats, PC level mult, summonable, doesn't bleed, simple actor, ghost, invulnerable...), level or "
				   "level multiplier, minimum and maximum calculated level, health, magicka and stamina offsets, speed multiplier, "
				   "template flags (which parts come from the template) and bleedout override. AI data: aggression, confidence, energy, "
				   "morality, mood, assistance, aggro radius (warn, warn/attack, attack). Skills: value and offset of all 18 skills, base "
				   "health, magicka and stamina, far-away model distance, geared-up weapons. Also: template, class, voice type, combat "
				   "style, gift filter, crime faction, death item, skin, far-away model, attack race, attacks (damage, chance, spell, "
				   "flags), height, weight, sound level, sound types or the NPC they are inherited from, default outfit, sleeping outfit, "
				   "default package list, AI packages, and the spectator, observe-dead-body, guard-warn and combat override package lists.";
		case Category::kItemValues:
			return "Each value from the last mod that changed it. Weapons: value, weight, damage, animation type, speed, reach, flags, "
				   "sight FOV, VATS to-hit chance, attack animation, number of projectiles, embedded weapon actor value, minimum and "
				   "maximum range, on-hit behaviour, flags 2, attack animation multiplier, fire rate, rumble, damage-to-weapon multiplier, "
				   "shots per second, skill, resist, stagger, critical damage, critical % multiplier, critical on death, critical spell "
				   "effect, detection sound level, template, scope effect, embedded weapon node, impact data set, 1st person model "
				   "object, attack, attack 2D, attack loop, attack fail, idle, equip and unequip sounds. Armour: value, weight, armour "
				   "rating, race, template, ragdoll constraint template. Weapons and armour: enchantment and enchantment amount, equip "
				   "type, block bash impact data and material. Ammunition: projectile, flags, damage, value, weight, short name. Misc "
				   "items and keys: value, weight. Soul gems: value, weight, contained soul, capacity, linked soul gem. Books: flags "
				   "(teaches a skill or a spell, can't be taken), type, the skill or spell taught, value, weight, inventory art. Potions: "
				   "weight, value, flags (food, poison, medicine...), addiction and addiction chance, consume sound, equip type. "
				   "Ingredients: value, weight, flags. Scrolls and spells: base cost, flags, type, charge time, cast type, delivery, cast "
				   "duration, range, half-cost perk, equip type, and the scroll's value and weight. Items: pick-up and put-down sounds. "
				   "Containers: flags (respawns, show owner...), weight, open and close sounds. Leveled lists: chance none, flags "
				   "(calculate from all levels, calculate for each item in count, use all), chance-none global.";
		case Category::kEffects:
			return "Potions, ingredients, scrolls and spells: the whole list of magic effects, each with its magnitude, area and duration "
				   "and its conditions, taken as one list from the last mod that changed it.";
		case Category::kNames:
			return "Editor IDs, names, NPC short names, descriptions, book text and book descriptions, from the last mod that changed "
				   "each (e.g. typo fixes), on every merged record type, cells included.";
		case Category::kModels:
			return "From the last mod that changed each: models with their texture data and alternate textures (world model, male and "
				   "female world models, 1st person model, scope model), inventory icons (male and female), armour armatures (ARMA), "
				   "biped body slots and armour type (light, heavy, clothing), object bounds, menu display object (spells, scrolls), and "
				   "destruction data (health, stages, explosion, debris, damaged models).";
		case Category::kScripts:
			return "The scripts attached to NPCs, containers, armour, weapons, ammunition, misc items, keys, books, potions and "
				   "ingredients, with every property value, taken as one set from the last mod that changed it.";
		case Category::kCellLighting:
			return "Cells, taken as one set from the last mod that changed any part of it: ambient colour, directional colour, "
				   "rotation and fade, fog colours (near, far), fog near, far, power, maximum and clip distance, light fade begin and "
				   "end, which of these come from the lighting template, and the lighting template.";
		case Category::kCellWater:
			return "Cells, taken as one set from the last mod that changed any part of it: water height, water type, water noise "
				   "texture, water environment map, and water currents (velocities).";
		case Category::kCellRegions:
			return "Cells: the regions they belong to (they bring weather, ambient sounds, map names and region objects). What every "
				   "mod added or removed is combined.";
		case Category::kCellOther:
			return "Cells, each from the last mod that changed it: location, owner (an NPC or a faction) and required faction rank, "
				   "encounter zone, music type, acoustic space, image space, sky and weather from a region, lock list, cell flags "
				   "(interior, has water, can't travel from here, no LOD water, public area, hand changed, show sky, use sky lighting), "
				   "exterior grid position and land flags, terrain visibility (occlusion) data and maximum height data.";
		case Category::kRenumberFix:
			return "A plugin made for an older version of a light (ESL) plugin, before it was compacted, points at its forms by their "
				   "old numbers (e.g. a hair or body patch for an NPC overhaul): those references are pointed at the same forms' new "
				   "numbers. The pairs are worked out from records both plugins change; only numbers that pair up the same way every "
				   "time are fixed. Unticked, those references stay broken, as without the patch.";
		default: return "";
		}
	}

	int CategoryRisk(Category a_cat)
	{
		switch (a_cat) {
		case Category::kLeveledLists:
		case Category::kContainers:
		case Category::kOutfits:
		case Category::kKeywords:
		case Category::kNames:
		case Category::kItemValues:
		case Category::kModels:
			return 0;
		case Category::kScripts:
		case Category::kCellOther:
			return 2;
		default:
			return 1;
		}
	}

	const char* CategoryWhy(Category a_cat)
	{
		switch (a_cat) {
		case Category::kLeveledLists: return "Safe: entries every mod added stay, entries a mod removed stay removed.";
		case Category::kContainers: return "Safe: items every mod added stay, items a mod removed stay removed.";
		case Category::kOutfits:
			return "Safe as a rule. Two mods that each give an NPC a different outfit piece for the same body slot can leave both in the "
				   "outfit; the game equips one of them.";
		case Category::kFormLists:
			return "Some scripts read a form list by position. An entry added at the end is fine, but a script that relies on the exact "
				   "order of a list another mod changed may see a different entry.";
		case Category::kKeywords: return "Safe: keywords every mod added stay, keywords a mod removed stay removed.";
		case Category::kNpcLists:
			return "An NPC can end up with the factions, spells and perks of two overhauls at once (e.g. two perk sets or two "
				   "combat styles' spells), which makes it stronger or behave in ways neither mod meant.";
		case Category::kNpcFace:
			return "The face must match the face files (FaceGen) the game loads for that NPC, which come from your mod order, not the "
				   "plugin order. If the mod whose face is taken does not also win the FaceGen files, the face turns dark or wrong.";
		case Category::kNpcValues:
			return "Values are taken one by one from different mods. Mods meant to work as a set (e.g. a combat overhaul's levels, "
				   "stats and AI) can end up mixed with another mod's values.";
		case Category::kItemValues:
			return "Safe as a rule: each value is the last change made to it. Balance mods meant as a set can be mixed with another "
				   "mod's values, as with the NPC values.";
		case Category::kEffects:
			return "The effects are one list from one mod: if two mods each changed different effects of the same potion or spell, only "
				   "the later mod's list is kept.";
		case Category::kNames: return "Safe: names, descriptions and editor IDs do not change how anything works.";
		case Category::kModels:
			return "Safe as a rule. A model from one mod with textures from another can look wrong if the other mod's meshes are not "
				   "installed; body slots decide what can be worn together.";
		case Category::kScripts:
			return "Two mods that each add their own script to the same record cannot both keep it: only the later mod's scripts stay, "
				   "and the other mod's feature on that record stops working. Check these records.";
		case Category::kCellLighting:
			return "The lighting of the last mod that changed it wins. Fine when that is your lighting mod; if a mod that only meant to "
				   "change something else carries old lighting and loads later, its lighting wins.";
		case Category::kCellWater:
			return "A wrong water height floods an area or leaves it dry. Check cells that two water or city mods change.";
		case Category::kCellRegions: return "Regions every mod added stay. A cell in two regions gets weather and sounds from both.";
		case Category::kCellOther:
			return "Location and encounter zone drive quests, crime, respawning and leveled enemies; owner decides what counts as "
				   "stealing. Mixing two mods' settings can be what neither meant. Check these cells.";
		case Category::kRenumberFix:
			return "Points broken references at the forms they were meant for. A pairing is only used when it is the same everywhere.";
		default: return "";
		}
	}

	std::string Result::report() const
	{
		std::ostringstream o;
		o << "ReSaver Plus merged patch\n";
		o << "=========================\n\n";
		o << pluginsRead << " plugins read, " << recordsRead << " records of the merged types.\n";
		o << conflicts << " records are changed differently by two or more plugins.\n";
		o << merged.size() << " of them were merged into the patch; " << skipped.size() << " could not be merged (the winning plugin's version stays).\n";
		if (fo4) {
			o << "Fallout 4 load order. Merged record types: leveled items, leveled NPCs, leveled spells, containers, form lists, outfits, "
				 "misc items, keys, NPCs, armour, weapons, ammunition, books, potions, ingredients, spells and cells.\n"
			  << Fo4NotMerged() << "\n";
		}
		o << (legendary ? "Skyrim Legendary Edition load order: the patch is a normal plugin in LE's format and takes a load order slot.\n\n"
			  : light   ? "The patch is a light (ESL-flagged) plugin: it takes no load order slot.\n\n"
						: "The patch is a normal plugin (not light): it takes a load order slot.\n\n");
		if (!fixes.empty()) {
			o << "Broken references fixed by the patch:\n";
			for (const auto& w : fixes) o << "  " << w << "\n";
			o << "\n";
		}
		if (!warnings.empty()) {
			o << "Problems in the load order itself (not caused by the patch):\n";
			for (const auto& w : warnings) o << "  " << w << "\n";
			o << "\n";
		}
		if (!missing.empty()) {
			o << "Plugins of the load order that were not found or not read:\n";
			for (const auto& m : missing) o << "  " << m << "\n";
			o << "\n";
		}
		o << "Masters of the patch (" << masters.size() << "), each with a record that needs it:\n";
		for (const auto& m : masters) {
			auto it = masterWhy.find(m);
			o << "  " << m << (it != masterWhy.end() ? "   (" + it->second + ")" : "") << "\n";
		}
		o << "\nMerged records (the winning plugin first, then what each other plugin's changes added (+), removed (-) or changed (~)):\n";
		for (const auto& r : merged) {
			o << "  " << r.type << " " << (r.edid.empty() ? "" : r.edid + " ") << "[" << r.id << "]  winner " << r.winner << "\n";
			for (std::size_t i = 0; i < r.changes.size(); ++i) {
				static const char* const tag[] = { "[safe]   ", "[caution]", "[risky]  " };
				o << "      " << (i < r.categories.size() ? tag[CategoryRisk(r.categories[i])] : "") << " " << r.changes[i] << "\n";
			}
		}
		if (!skipped.empty()) {
			o << "\nNot merged:\n";
			for (const auto& s : skipped) o << "  " << s << "\n";
		}
		return o.str();
	}
}
