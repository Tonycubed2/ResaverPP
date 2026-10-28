// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Merged patch. The approach follows Mator Smash (copy the winning override, then apply each
// override's own changes measured against the version its plugin was built on) and xEdit's
// required-masters rule; record layouts follow xEdit's Skyrim definitions (wbDefinitionsTES5).
#include "MergedPatch.h"

#include "Bsa.h"
#include "ConditionFunctions.h"
#include "Save.h"  // DecompressZlib
#include "ScriptSources.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
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
		int GroupRank(const char* sig)
		{
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
		const std::vector<Record>& a_records, const std::string& a_author, const std::string& a_desc, float a_hedrVersion)
	{
		// group the records by type, groups in the game's order
		std::map<int, std::pair<std::string, std::vector<const Record*>>> groups;
		for (const auto& r : a_records) {
			// exterior cells live under their worldspace
			const char* top = Is(r.sig, "CELL") && !r.groups.empty() && r.groups[0].first == 1 ? "WRLD" : r.sig;
			if (top != r.sig) {
				auto& g = groups[GroupRank(top) * 10000 + 0];
				g.first = "WRLD";
				g.second.push_back(&r);
				continue;
			}
			auto& g = groups[GroupRank(r.sig) * 10000 + 0];
			if (g.first.empty() || g.first == Sig(r.sig)) {
				g.first = Sig(r.sig);
				g.second.push_back(&r);
			} else {
				// several unknown types share the rank: keep them apart
				auto& g2 = groups[GroupRank(r.sig) * 10000 + static_cast<int>(std::hash<std::string>{}(Sig(r.sig)) % 9999) + 1];
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
			Put32(hedr.data, 0x800);
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
				Sub data;
				std::memcpy(data.sig, "DATA", 4);
				data.data.assign(8, 0);
				hs.push_back(std::move(data));
			}
			Sub intv;
			std::memcpy(intv.sig, "INTV", 4);
			Put32(intv.data, 1);
			hs.push_back(std::move(intv));
		}
		std::vector<std::uint8_t> hd;
		WriteSubs(hd, hs);
		std::vector<std::uint8_t> out;
		PutSig(out, "TES4");
		Put32(out, static_cast<std::uint32_t>(hd.size()));
		Put32(out, a_headerFlags);
		Put32(out, 0);
		Put32(out, 0);
		Put16(out, 44);
		Put16(out, 0);
		out.insert(out.end(), hd.begin(), hd.end());
		out.insert(out.end(), body.begin(), body.end());
		return out;
	}

	std::vector<std::string> ReadLoadOrder(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_gameFolder)
	{
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
			std::ifstream f(a_pluginsTxt);
			for (std::string line; std::getline(f, line);) {
				line = trim(line);
				if (!line.empty() && line[0] == '*') {
					enabled.push_back(trim(line.substr(1)));
					enabledLow.insert(Lower(enabled.back()));
				}
			}
		}
		for (const char* p : { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" }) {
			add(p);
		}
		// the game loads the Creation Club files listed in Skyrim.ccc (and _ResourcePack.esl) by itself: Mod
		// Organizer does not mark them enabled in plugins.txt
		std::set<std::string> always = { "_resourcepack.esl" };
		std::error_code       ec;
		{
			std::ifstream ccc(a_gameFolder / "Skyrim.ccc");
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
			std::ifstream ccc(a_gameFolder / "Skyrim.ccc");
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
		};

		Kind Layout(const char* rec, const char* sub)
		{
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
		bool VmadForms(const std::vector<std::uint8_t>& d, std::vector<std::uint32_t>& offs, std::string& err)
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
						switch (type) {
						case 1: object(); break;
						case 2: wstr(); break;
						case 3:
						case 4: u32(); break;
						case 5: u8(); break;
						case 11: { const auto n = u32(); for (std::uint32_t j = 0; j < n; ++j) object(); break; }
						case 12: { const auto n = u32(); for (std::uint32_t j = 0; j < n; ++j) wstr(); break; }
						case 13:
						case 14: { const auto n = u32(); need(std::size_t(n) * 4); p += std::size_t(n) * 4; break; }
						case 15: { const auto n = u32(); need(n); p += n; break; }
						default: throw std::runtime_error("unknown script property type " + std::to_string(type));
						}
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
			const char* follower;  // a subrecord belonging to the entry before it (COED), or nullptr
			const char* count;     // the count subrecord before the list, or nullptr
			int         countSize; // 1 (LLCT) or 4
			bool        byForm;    // entries are identified by their first form ID; the rest is a value
			bool        array;     // one subrecord holding 4-byte form IDs (KWDA, OTFT INAM)
			std::size_t max;       // most entries the game takes (0: no limit)
			const char* what;      // for the report
		};

		std::vector<ListSpec> Lists(const char* rec)
		{
			const std::string r(rec, 4);
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
		std::vector<std::string> Order(const char* rec)
		{
			const std::string r(rec, 4);
			std::string s;
			if (r == "LVLI") s = "EDID OBND LVLD LVLF LVLG LLCT LVLO COED";
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

		std::vector<std::vector<std::string>> Groups(const char* rec)
		{
			const std::string r(rec, 4);
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

		std::vector<Field> Fields(const char* rec, const char* sub, std::size_t a_size)
		{
			const std::string k = std::string(rec, 4) + "." + std::string(sub, 4);
			std::vector<Field> f;
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
			if (r == "LVLI" || r == "LVLN" || r == "LVSP") return Category::kLeveledLists;
			if (r == "CONT") return Category::kContainers;
			if (r == "OTFT") return Category::kOutfits;
			if (r == "FLST") return Category::kFormLists;
			if (r == "NPC_") return Category::kNpcLists;
			if (r == "CELL") return Category::kCellRegions;
			return Category::kOther;
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
			if (has("EDID") || has("FULL") || has("DESC") || has("SHRT") || has("OBND") || (r == "BOOK" && has("CNAM"))) return Category::kNames;
			if (has("MODL") || has("MOD2") || has("MOD3") || has("MOD4") || has("ICON") || has("ICO2") || has("DEST") || has("MDOB") ||
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
				_opt(a_opt), _progress(a_progress), _res(a_res) {}

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
						pf = ReadPlugin(*path, kTypes);
					} catch (const std::exception& e) {
						_res.missing.push_back(p.name + " (" + e.what() + ")");
						if (_progress) ++_progress->done;
						continue;
					}
					p.found = true;
					p.localized = pf.flags & 0x80;
					p.light = pf.flags & 0x200;
					for (const auto& m : pf.masters) {
						auto it = index.find(Lower(m));
						const int lo = it == index.end() || it->second >= static_cast<int>(i) ? -1 : it->second;
						p.masterLo.push_back(lo);
						if (lo >= 0) {
							p.masterSet.insert(lo);
						}
					}
					++_res.pluginsRead;
					if (pf.flags & 0x200) {
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
						for (const auto& lang : { Lower(_opt.language), std::string("english") }) {
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
				for (const auto& s : v.rec.subs) {
					CSub cs;
					std::memcpy(cs.sig, s.sig, 4);
					cs.data = s.data;
					std::vector<std::uint32_t> offs;
					switch (Layout(v.rec.sig, s.sig)) {
					case Kind::kUnknown:
						err = "has a part (" + Sig(s.sig) + ") ReSaver Plus does not know yet";
						return std::nullopt;
					case Kind::kNone: break;
					case Kind::kDrop: continue;
					case Kind::kLString:
						if (_plugins[v.plugin].localized) {
							if (s.data.size() != 4) {
								err = "a localized text (" + Sig(s.sig) + ") is not a string id";
								return std::nullopt;
							}
							const auto id = U32(s.data.data());
							std::string text;
							if (id) {
								const auto& t = strings(v.plugin);
								auto        it = t.find(id);
								if (it == t.end()) {
									err = "the text of " + Sig(s.sig) + " is not in " + _plugins[v.plugin].name + "'s strings files";
									return std::nullopt;
								}
								text = it->second;
							}
							cs.data.assign(text.begin(), text.end());
							cs.data.push_back(0);
						}
						break;
					case Kind::kForm0:
						if (s.data.size() < 4) {
							err = Sig(s.sig) + " is too short";
							return std::nullopt;
						}
						offs.push_back(0);
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
						if (!VmadForms(s.data, offs, err)) {
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
								auto it = _remap.find({ target, obj });
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
					while (sp.follower && i + 1 < c.subs.size() && Is(c.subs[i + 1].sig, sp.follower)) {
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
					const bool isFollower = sp.follower && afterEntry && Is(s.sig, sp.follower);
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
					const auto order = Order(c.sig);
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
					for (const auto& e : es) {
						ins.insert(ins.end(), e.subs.begin(), e.subs.end());
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
			static void SetBlock(CRec& c, const std::set<std::string>& sigs, const std::vector<CSub>& block)
			{
				const auto order = Order(c.sig);
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
				for (const auto& sp : Lists(a_patch.sig)) {
					listSigs.insert(sp.entry);
					if (sp.follower) listSigs.insert(sp.follower);
					if (sp.count) listSigs.insert(sp.count);
				}
				// the units: the groups, then every other part on its own
				std::vector<std::set<std::string>> units;
				std::set<std::string>              grouped;
				for (const auto& g : Groups(a_patch.sig)) {
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
						const auto fields = Fields(a_patch.sig, o[0].sig, o[0].data.size());
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
						note(mr, "fixed " + std::to_string(n) + " reference(s) to renumbered forms of a compacted plugin", Category::kOther);
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
						note(mr, "fixed " + std::to_string(winFixes) + " reference(s) to renumbered forms of a compacted plugin", Category::kOther);
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
					bool failed = false;
					for (const auto& sp : Lists(win.rec.sig)) {
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
					if ((k & 0xFFFFFF) < 0x800 && Lower(_plugins[p].name) != "skyrim.esm") {
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
						r.subs.push_back(std::move(o));
					}
					recs.push_back(std::move(r));
				}
				// only overrides, no new forms: flagged light (ESL), so it takes no load order slot
				_res.file = WritePlugin(_res.masters, 0x200, recs, "ReSaver Plus",
					"Merged patch made by ReSaver Plus. Rebuild it after changing your mods.", low ? 1.71f : 1.7f);
				_res.ok = true;
			}

			const Options&                                   _opt;
			Progress*                                        _progress;
			Result&                                          _res;
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
			Builder b(a_opt, a_progress, r);
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

	const char* CategoryName(Category a_cat)
	{
		switch (a_cat) {
		case Category::kLeveledLists: return "Leveled lists (items, NPCs, spells)";
		case Category::kContainers: return "Container items";
		case Category::kOutfits: return "Outfit items";
		case Category::kFormLists: return "Form lists";
		case Category::kKeywords: return "Keywords";
		case Category::kNpcLists: return "NPC factions, spells, perks and items";
		case Category::kNpcFace: return "NPC faces and races";
		case Category::kNpcValues: return "NPC stats, AI, skills and other values";
		case Category::kItemValues: return "Item values (value, weight, damage, armour, costs)";
		case Category::kEffects: return "Magic effects and their conditions";
		case Category::kNames: return "Names, descriptions and editor IDs";
		case Category::kModels: return "Models, icons and destruction";
		case Category::kScripts: return "Scripts on records";
		case Category::kCellLighting: return "Cell lighting";
		case Category::kCellWater: return "Cell water";
		case Category::kCellRegions: return "Cell regions";
		case Category::kCellOther: return "Cell location, owner, music and other settings";
		default: return "Other";
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
		case Category::kLeveledLists: return "What each mod added to or removed from a leveled list is kept. Safe.";
		case Category::kContainers: return "What each mod added to or removed from a container is kept. Safe.";
		case Category::kOutfits: return "Items each mod added to or removed from an outfit are kept. Safe.";
		case Category::kFormLists:
			return "Form lists are combined. Some scripts read a form list by position: an entry added at the end is fine, but a mod "
				   "that relies on the exact order may see a different one.";
		case Category::kKeywords: return "Keywords each mod added or removed are kept. Safe.";
		case Category::kNpcLists:
			return "Factions, spells, perks and items of NPCs are combined. An NPC can end up with abilities from two overhauls at once "
				   "(e.g. two perk sets).";
		case Category::kNpcFace:
			return "An NPC's face and race are taken from the last mod that changed them. That mod's face files (FaceGen) must win in your "
				   "mod order too, or the face turns dark or wrong.";
		case Category::kNpcValues:
			return "NPC stats, AI, skills, outfits and the like are taken value by value from the last mod that changed each. Mods meant "
				   "to work as a set (e.g. a combat overhaul's stats) can end up mixed.";
		case Category::kItemValues: return "Value, weight, damage, armour rating and costs, value by value from the last mod that changed each.";
		case Category::kEffects:
			return "A potion's, ingredient's, scroll's or spell's effects are taken as one list from the last mod that changed them. Safe "
				   "unless two mods each changed different effects.";
		case Category::kNames: return "Names, descriptions and editor IDs from the last mod that changed them (e.g. typo fixes). Safe.";
		case Category::kModels: return "Models and icons from the last mod that changed them. Safe.";
		case Category::kScripts:
			return "Scripts attached to a record, with their properties, are taken from the last mod that changed them. If two mods each "
				   "add their own script to the same record, only one keeps it: check those records.";
		case Category::kCellLighting:
			return "A cell's lighting (and lighting template) from the last mod that changed it, as one set. Fine when that is your lighting "
				   "mod; if a mod that only meant to change something else carries old lighting and loads later, it wins.";
		case Category::kCellWater:
			return "A cell's water height, type and currents from the last mod that changed them, as one set. A wrong water height floods "
				   "or empties an area: check cells two water mods touch.";
		case Category::kCellRegions: return "The regions each mod added to or removed from a cell are kept (weather, sounds, map names).";
		case Category::kCellOther:
			return "A cell's location, owner, encounter zone, music, acoustics, image space and flags, each from the last mod that changed "
				   "it. Location and encounter zone drive quests, crime and respawns: merging two mods' settings can be what neither "
				   "meant. Check these cells.";
		default: return "Anything else, from the last mod that changed it.";
		}
	}

	std::string Result::report() const
	{
		std::ostringstream o;
		o << "ReSaver Plus merged patch\n";
		o << "=========================\n\n";
		o << pluginsRead << " plugins read, " << recordsRead << " records of the merged types.\n";
		o << conflicts << " records are changed differently by two or more plugins.\n";
		o << merged.size() << " of them were merged into the patch; " << skipped.size() << " could not be merged (the winning plugin's version stays).\n\n";
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
