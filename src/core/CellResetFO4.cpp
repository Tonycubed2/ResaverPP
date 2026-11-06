// ReSaver++ - Fallout 4: reset an area (a cell) as ResetInterior does (see CellResetFO4.h).
#include "CellResetFO4.h"

#include "ChangeFormData.h"
#include "ChangeFormFO4.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace ess::fo4cell
{
	namespace
	{
		constexpr std::uint8_t  kCellType = static_cast<std::uint8_t>(ChangeFormType::CELL);
		constexpr std::uint32_t kBitDetach = 1u << 30, kBitChar = 1u << 29, kBitShort = 1u << 28, kBitSeen = 1u << 31;

		std::string Hex(std::uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof b, "%08X", v);
			return b;
		}

		// the save's RefID for a form id, as the game writes it: a Fallout4.esm form below 0x400000 directly (type 1),
		// any other through the form id array (type 0; appended when the array lacks it)
		RefID RefFor(SaveFile& a_save, std::uint32_t a_formID)
		{
			RefID r;
			if ((a_formID >> 24) == 0 && a_formID < 0x400000u) {
				r.data = 0x400000u | a_formID;
				return r;
			}
			for (std::size_t i = 0; i < a_save.formIDs.size(); ++i) {
				if (a_save.formIDs[i] == a_formID) {
					r.data = static_cast<std::uint32_t>(i + 1);
					return r;
				}
			}
			a_save.formIDs.push_back(a_formID);
			r.data = static_cast<std::uint32_t>(a_save.formIDs.size());
			return r;
		}

		// vsval as 0xc0fd30 reads it: the low 2 bits of the first byte give the width (1, 2 or 4 bytes), the value is >> 2
		std::uint32_t Vsval(Reader& r)
		{
			const std::uint32_t b0 = r.u8();
			switch (b0 & 3) {
			case 0: return b0 >> 2;
			case 1: return (b0 | (static_cast<std::uint32_t>(r.u8()) << 8)) >> 2;
			case 2:
				{
					std::uint32_t v = b0;
					for (int k = 1; k < 4; ++k) v |= static_cast<std::uint32_t>(r.u8()) << (8 * k);
					return v >> 2;
				}
			default: throw std::runtime_error("bad vsval");
			}
		}

		struct Player
		{
			bool          known = false, exterior = false;
			std::uint32_t space = 0;
			int           x = 0, y = 0;
		};
		Player WherePlayer(const SaveFile& a_save, const std::unordered_set<std::uint32_t>& a_worldspaces)
		{
			Player p;
			const auto* cf = fo4cf::FindPlayer(a_save);
			if (!cf) {
				return p;
			}
			const auto d = fo4cf::DecodeRefr(a_save, *cf);
			if (!d.decoded || !d.hasInitial) {
				return p;
			}
			p.known = true;
			p.space = a_save.formIDOf(d.cell);
			p.exterior = a_worldspaces.contains(p.space);
			p.x = static_cast<int>(std::floor(d.pos[0] / 4096.0f));
			p.y = static_cast<int>(std::floor(d.pos[1] / 4096.0f));
			return p;
		}
	}

	namespace
	{
	CellData DecodeAs(const SaveFile& a_save, const ChangeForm& a_cf, int a_interior)
	{
		CellData d;
		const auto body = cf::Body(a_cf);
		const auto fl = a_cf.changeFlags;
		Reader     r(body.data(), body.size());
		try {
			if (fl & kBitDetach) {
				d.initialType = (fl & kBitChar) ? 1 : (fl & kBitShort) ? 2 : 3;
				if (d.initialType == 3) {
					d.detach = r.u32();
					d.initialSize = 4;
				} else {
					d.worldspaceIndex = r.u16();
					if (d.initialType == 1) {
						d.x = static_cast<std::int8_t>(r.u8());
						d.y = static_cast<std::int8_t>(r.u8());
					} else {
						d.x = static_cast<std::int16_t>(r.u16());
						d.y = static_cast<std::int16_t>(r.u16());
					}
					d.detach = r.u32();
					d.initialSize = d.initialType == 1 ? 8 : 10;
					if (d.worldspaceIndex >= 1 && d.worldspaceIndex <= a_save.visitedWorldspaces.size()) {
						d.worldspace = a_save.visitedWorldspaces[d.worldspaceIndex - 1];
					}
				}
			}
			if (fl & 1) {  // TESForm::LoadGame
				r.u32();
				r.u16();
			}
			if (fl & 2) {
				d.hasFlags = true;
				d.flags = r.u16();
			}
			if (fl & kBitSeen) {
				d.hasSeen = true;
				const int interior = a_interior >= 0 ? a_interior : d.initialType == 3 ? 1 : d.initialType ? 0 : -1;
				if (interior < 0) {
					throw std::runtime_error("map seen data of a cell not known to be interior or exterior");
				}
				d.interior = interior;
				if (interior) {
					const auto n = Vsval(r);
					for (std::uint32_t i = 0; i < n; ++i) {
						r.u8();
						r.u8();
						r.bytes(32);
					}
				} else {
					r.bytes(32);
				}
			}
			if (fl & 4) {
				const auto n = r.u16();
				const auto b = r.bytes(n);
				d.name.assign(b.begin(), b.end());
			}
			if (fl & 8) {
				d.owner = RefID::read(r);
			}
			if (r.remaining()) {
				d.error = std::to_string(r.remaining()) + " bytes left after the cell's data";
			} else {
				d.complete = true;
			}
		} catch (const std::exception& e) {
			d.error = e.what();
		}
		return d;
	}
	}

	CellData DecodeCell(const SaveFile& a_save, const ChangeForm& a_cf, int a_interior)
	{
		auto d = DecodeAs(a_save, a_cf, a_interior);
		if (!d.complete && a_interior < 0 && d.hasSeen && !d.initialType) {
			// map seen data without bit 30: the layout tells interior (vsval n x 34 bytes) from exterior (32 bytes) when
			// exactly one of them reads to the end
			auto in = DecodeAs(a_save, a_cf, 1), ex = DecodeAs(a_save, a_cf, 0);
			if (in.complete != ex.complete) {
				return in.complete ? in : ex;
			}
		}
		return d;
	}

	std::vector<CellInfo> ListCells(const SaveFile& a_save, const fo4world::Lookup& a_lookup)
	{
		std::vector<CellInfo> out;
		if (!a_save.isFO4()) {
			return out;
		}
		const std::unordered_set<std::uint32_t> worldspaces(a_save.visitedWorldspaces.begin(), a_save.visitedWorldspaces.end());
		std::map<std::uint32_t, std::size_t> byId;
		auto add = [&](std::uint32_t id) -> CellInfo& {
			auto [it, fresh] = byId.try_emplace(id, out.size());
			if (fresh) {
				out.push_back({});
				out.back().formID = id;
			}
			return out[it->second];
		};
		std::map<std::tuple<std::uint32_t, int, int>, std::uint32_t> exteriorAt;  // (worldspace, x, y) -> cell
		for (const auto& c : a_save.changeForms) {
			if (c.typeCode() != kCellType) {
				continue;
			}
			const auto id = a_save.formIDOf(c.refID);
			auto&      ci = add(id);
			ci.hasChangeForm = true;
			ci.changeFlags = c.changeFlags;
			const auto d = DecodeCell(a_save, c);
			if (d.initialType == 1 || d.initialType == 2) {
				ci.exterior = true;
				ci.worldspace = d.worldspace;
				ci.x = d.x;
				ci.y = d.y;
				exteriorAt[{ d.worldspace, d.x, d.y }] = id;
			}
			if (d.initialType) {
				ci.hasDetach = true;
				ci.detach = d.detach;
				ci.pending = d.detach == kDetachReset;
			}
			if (!d.complete) {
				ci.why = "its saved data could not be read: " + d.error;
			}
		}
		// saved positions: interior cells named by references, and how many changed references lie in each cell
		const auto items = fo4world::List(a_save, a_lookup);
		std::unordered_set<std::uint32_t> created;
		for (const auto& it : items.items) {
			created.insert(it.formID);
			if (it.why.find("could not be read") != std::string::npos) {
				continue;
			}
			CellInfo* ci = nullptr;
			if (it.exterior) {
				if (auto e = exteriorAt.find({ it.space, it.gridX, it.gridY }); e != exteriorAt.end()) {
					ci = &out[byId[e->second]];
				}
			} else if (!worldspaces.contains(it.space) && it.space) {
				ci = &add(it.space);
			}
			if (ci) {
				++ci->createdItems;
				ci->createdRemovable += it.removable ? 1 : 0;
			}
		}
		for (const auto& c : a_save.changeForms) {
			const auto t = c.typeCode();
			if ((t != static_cast<std::uint8_t>(ChangeFormType::REFR) && t != static_cast<std::uint8_t>(ChangeFormType::ACHR)) ||
				created.contains(a_save.formIDOf(c.refID))) {
				continue;
			}
			if (!(c.changeFlags & 0x200000E) && c.refID.type() != RefID::Type::kCreated) {
				continue;  // no saved position (bits 1/2/3/25)
			}
			const auto d = fo4cf::DecodeRefr(a_save, c);
			if (!d.decoded || !d.hasInitial) {
				continue;
			}
			const auto space = a_save.formIDOf(d.cell);
			if (worldspaces.contains(space)) {
				const int x = static_cast<int>(std::floor(d.pos[0] / 4096.0f)), y = static_cast<int>(std::floor(d.pos[1] / 4096.0f));
				if (auto e = exteriorAt.find({ space, x, y }); e != exteriorAt.end()) {
					++out[byId[e->second]].placed;
				}
			} else if (space) {
				++add(space).placed;
			}
		}
		const auto pl = WherePlayer(a_save, worldspaces);
		for (auto& ci : out) {
			if (a_lookup.name) {
				ci.name = a_lookup.name(ci.formID);
			}
			if (pl.known) {
				if (!pl.exterior && ci.formID == pl.space) {
					ci.player = true;
				} else if (pl.exterior && ci.exterior && ci.worldspace == pl.space) {
					ci.player = ci.x == pl.x && ci.y == pl.y;
					ci.nearPlayer = std::abs(ci.x - pl.x) <= 2 && std::abs(ci.y - pl.y) <= 2;
				}
			}
			if (!ci.why.empty()) {
				continue;
			}
			if (!pl.known) {
				ci.why = "where the player is could not be read";
			} else if (ci.player) {
				ci.why = "the player is in it";
			} else if (ci.nearPlayer) {
				ci.why = "it is next to the player's cell (loaded with the game, so the game would not reset it now)";
			} else if (ci.pending) {
				ci.why = "a reset is already set for it";
			} else if (!ci.exterior && worldspaces.contains(ci.formID)) {
				ci.why = "it is a worldspace";
			} else if (!ci.exterior && ci.hasChangeForm && !ci.hasDetach && !ci.placed && !ci.createdItems) {
				ci.why = "the save does not say whether it is an interior";
			}
		}
		std::sort(out.begin(), out.end(), [](const CellInfo& a, const CellInfo& b) {
			if (a.exterior != b.exterior) return !a.exterior;
			if (a.exterior && a.worldspace != b.worldspace) return a.worldspace < b.worldspace;
			if (a.exterior && a.x != b.x) return a.x < b.x;
			if (a.exterior && a.y != b.y) return a.y < b.y;
			return a.formID < b.formID;
		});
		return out;
	}

	std::string ResetCell(SaveFile& a_save, std::uint32_t a_cellFormID, const fo4world::Lookup& a_lookup)
	{
		if (!a_save.isFO4()) {
			return "not a Fallout 4 save";
		}
		const auto cells = ListCells(a_save, a_lookup);
		const auto it = std::find_if(cells.begin(), cells.end(), [&](const CellInfo& c) { return c.formID == a_cellFormID; });
		if (it == cells.end()) {
			return Hex(a_cellFormID) + ": the save has no change form for this cell and no saved position in it (an exterior cell, or one "
										"it never changed)";
		}
		if (!it->why.empty()) {
			return Hex(a_cellFormID) + ": " + it->why;
		}
		ChangeForm* cfp = nullptr;
		for (auto& c : a_save.changeForms) {
			if (c.typeCode() == kCellType && a_save.formIDOf(c.refID) == a_cellFormID) {
				cfp = &c;
				break;
			}
		}
		if (cfp) {
			const auto d = DecodeCell(a_save, *cfp, it->exterior ? 0 : 1);
			if (!d.complete) {
				return Hex(a_cellFormID) + ": its saved data could not be read: " + d.error;
			}
			ChangeForm copy = *cfp;
			auto       body = cf::Body(copy);
			if (d.initialType) {
				// the detach time is the last u32 of the initial data
				for (int k = 0; k < 4; ++k) {
					body[d.initialSize - 4 + k] = 0xFF;
				}
			} else if (!it->exterior) {
				body.insert(body.begin(), 4, 0xFF);
				copy.changeFlags |= kBitDetach;
			} else {
				return Hex(a_cellFormID) + ": the save has no coordinates for this exterior cell";
			}
			cf::SetBody(copy, body);
			const auto check = DecodeCell(a_save, copy, it->exterior ? 0 : 1);
			if (!check.complete || check.detach != kDetachReset) {
				return Hex(a_cellFormID) + ": the edited cell does not read back (" + check.error + ")";
			}
			*cfp = std::move(copy);
			return "";
		}
		if (it->exterior) {
			return Hex(a_cellFormID) + ": the save has no coordinates for this exterior cell";
		}
		// a new CELL change form: flag 0x40000000 and the 4-byte detach time, as the game writes it after ResetInterior
		ChangeForm c;
		c.refID = RefFor(a_save, a_cellFormID);
		c.changeFlags = kBitDetach;
		c.typeField = kCellType;
		c.version = a_save.formVersion;
		for (const auto& o : a_save.changeForms) {
			if (o.typeCode() == kCellType) {
				c.version = o.version;
				break;
			}
		}
		c.data = { 0xFF, 0xFF, 0xFF, 0xFF };
		c.length1 = 4;
		c.length2 = 0;
		const auto check = DecodeCell(a_save, c, 1);
		if (!check.complete || check.detach != kDetachReset) {
			return Hex(a_cellFormID) + ": the new cell change form does not read back (" + check.error + ")";
		}
		std::size_t at = a_save.changeForms.size();
		for (std::size_t i = 0; i < a_save.changeForms.size(); ++i) {
			if (a_save.changeForms[i].typeCode() == kCellType) {
				at = i + 1;
			}
		}
		a_save.changeForms.insert(a_save.changeForms.begin() + static_cast<std::ptrdiff_t>(at), std::move(c));
		a_save.reindexChangeForms();
		return "";
	}
}
