// ReSaver++ - Fallout 4: reset an area (a cell) as ResetInterior does (see CellResetFO4.h).
#include "CellResetFO4.h"

#include "ChangeFormData.h"
#include "ChangeFormFO4.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
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
		std::vector<std::uint8_t> body;
		if (std::string why; !cf::TryBody(a_cf, body, &why)) {
			d.error = "could not be read: " + why;
			return d;
		}
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

	namespace
	{
		fo4world::CellRecord RecordOf(const fo4world::Lookup& a_lookup, std::uint32_t a_id)
		{
			return a_lookup.cell ? a_lookup.cell(a_id) : fo4world::CellRecord{};
		}

		bool FitsByte(int v) { return v >= -128 && v <= 127; }

		// The cells of the save (every CELL change form, the cells saved positions lie in) and a_extra when a plugin
		// defines it, with the reason each one cannot be reset.
		std::vector<CellInfo> Collect(const SaveFile& a_save, const fo4world::Lookup& a_lookup, const std::vector<std::uint32_t>& a_extra)
		{
			std::vector<CellInfo> out;
			if (!a_save.isFO4()) {
				return out;
			}
			const std::unordered_set<std::uint32_t> visited(a_save.visitedWorldspaces.begin(), a_save.visitedWorldspaces.end());
			auto typeOf = [&](std::uint32_t id) { return a_lookup.type ? a_lookup.type(id) : std::string(); };
			auto isWorldspace = [&](std::uint32_t id) {
				const auto t = typeOf(id);
				return t == "WRLD" || (t.empty() && visited.contains(id));
			};
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
			auto exteriorCell = [&](std::uint32_t ws, int x, int y) -> CellInfo* {
				if (auto e = exteriorAt.find({ ws, x, y }); e != exteriorAt.end()) {
					return &out[byId[e->second]];
				}
				const auto id = a_lookup.cellAt ? a_lookup.cellAt(ws, x, y) : 0u;
				return id ? &add(id) : nullptr;
			};
			for (const auto& c : a_save.changeForms) {
				if (c.typeCode() != kCellType) {
					continue;
				}
				const auto id = a_save.formIDOf(c.refID);
				auto&      ci = add(id);
				ci.hasChangeForm = true;
				ci.changeFlags = c.changeFlags;
				const auto rec = RecordOf(a_lookup, id);
				const auto d = DecodeCell(a_save, c, rec.known ? (rec.interior ? 1 : 0) : -1);
				if (d.initialType == 1 || d.initialType == 2) {
					ci.exterior = true;
					ci.interiorKnown = true;
					ci.worldspace = d.worldspace;
					ci.x = d.x;
					ci.y = d.y;
					ci.hasGrid = true;
				} else if (d.initialType == 3) {
					ci.interiorKnown = true;
				}
				if (d.initialType) {
					ci.hasDetach = true;
					ci.detach = d.detach;
					ci.pending = d.detach == kDetachReset;
				}
				if (rec.known && ci.interiorKnown && ci.exterior == rec.interior) {
					ci.why = std::string("the save keeps it as an ") + (ci.exterior ? "outdoor" : "indoor") + " cell, the plugins as an " +
					         (rec.interior ? "indoor" : "outdoor") + " one";
				}
				if (!d.complete) {
					ci.why = "its saved data could not be read: " + d.error;
				}
				if (ci.exterior) {
					exteriorAt[{ ci.worldspace, ci.x, ci.y }] = id;
				}
			}
			// what the plugins say of every cell found so far (and of the cells added below, at the end)
			auto fromPlugins = [&](CellInfo& ci) {
				const auto rec = RecordOf(a_lookup, ci.formID);
				if (!rec.known) {
					return;
				}
				ci.inPlugins = true;
				ci.plugin = rec.plugin;
				ci.persistent = rec.persistent;
				if (!ci.interiorKnown) {
					ci.interiorKnown = true;
					ci.exterior = !rec.interior;
				}
				if (ci.exterior && !ci.hasGrid && rec.hasGrid && rec.worldspace && !rec.persistent) {
					ci.hasGrid = true;
					ci.worldspace = rec.worldspace;
					ci.x = rec.x;
					ci.y = rec.y;
				}
			};
			for (auto& ci : out) {
				fromPlugins(ci);
				if (ci.exterior && ci.hasGrid && !exteriorAt.contains({ ci.worldspace, ci.x, ci.y })) {
					exteriorAt[{ ci.worldspace, ci.x, ci.y }] = ci.formID;
				}
			}
			// saved positions: the cells references lie in, and how many changed references lie in each cell
			const auto items = fo4world::List(a_save, a_lookup);
			std::unordered_set<std::uint32_t> created;
			for (const auto& it : items.items) {
				created.insert(it.formID);
				if (it.why.find("could not be read") != std::string::npos) {
					continue;
				}
				CellInfo* ci = nullptr;
				if (it.exterior) {
					ci = exteriorCell(it.space, it.gridX, it.gridY);
				} else if (it.space && !isWorldspace(it.space)) {
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
				if (isWorldspace(space)) {
					const int x = static_cast<int>(std::floor(d.pos[0] / 4096.0f)), y = static_cast<int>(std::floor(d.pos[1] / 4096.0f));
					if (auto* ci = exteriorCell(space, x, y)) {
						++ci->placed;
					}
				} else if (space) {
					++add(space).placed;
				}
			}
			const auto pl = WherePlayer(a_save, visited);
			if (pl.known && pl.exterior && a_lookup.cellAt) {
				// the player's cell and the cells loaded around it, so they show why they are not reset
				for (int dx = -2; dx <= 2; ++dx) {
					for (int dy = -2; dy <= 2; ++dy) {
						exteriorCell(pl.space, pl.x + dx, pl.y + dy);
					}
				}
			}
			for (const auto extra : a_extra) {
				if (extra && !byId.contains(extra) && RecordOf(a_lookup, extra).known) {
					add(extra);
				}
			}
			for (auto& ci : out) {
				fromPlugins(ci);
				if (ci.exterior && !ci.hasGrid) {
					// an outdoor cell the save names only by positions in it: its grid place is where they lie
					for (const auto& [key, cell] : exteriorAt) {
						if (cell == ci.formID) {
							std::tie(ci.worldspace, ci.x, ci.y) = key;
							ci.hasGrid = true;
						}
					}
				}
				if (a_lookup.name) {
					ci.name = a_lookup.name(ci.formID);
				}
				if (pl.known) {
					if (!pl.exterior && ci.formID == pl.space) {
						ci.player = true;
					} else if (pl.exterior && ci.exterior && ci.hasGrid && ci.worldspace == pl.space) {
						ci.player = ci.x == pl.x && ci.y == pl.y;
						ci.nearPlayer = std::abs(ci.x - pl.x) <= 2 && std::abs(ci.y - pl.y) <= 2;
					}
				}
				if (!ci.why.empty()) {
					continue;
				}
				const auto type = typeOf(ci.formID);
				if (!pl.known) {
					ci.why = "where the player is could not be read";
				} else if ((ci.formID >> 24) == 0xFF) {
					// (the game's detach 0x4d6e30 skips forms made during play: 0x3129c0 -> 0x2ecb40, so it never resets them)
					ci.why = "it was made during play (an FF form id), not a cell from a plugin: the game never resets such a cell";
				} else if (!type.empty() && type != "CELL") {
					ci.why = "it is not a cell (" + type + ")";
				} else if (!ci.exterior && visited.contains(ci.formID)) {
					ci.why = "it is a worldspace";
				} else if (ci.player) {
					ci.why = "the player is in it";
				} else if (ci.nearPlayer) {
					ci.why = "it is next to the player's cell (loaded with the game, so the game would not reset it now)";
				} else if (ci.pending) {
					ci.why = "a reset is already set for it";
				} else if (ci.persistent) {
					ci.why = "it is a worldspace's persistent cell (it holds the worldspace's always-loaded objects; it is not an area of the map)";
				} else if (!ci.interiorKnown) {
					ci.why = "neither the save nor the loaded plugins say whether it is indoors (its plugin is not loaded)";
				} else if (ci.exterior && !ci.hasGrid) {
					ci.why = "neither the save nor the loaded plugins give this outdoor cell's place on the map";
				} else if (ci.exterior && !ci.worldspace) {
					ci.why = "its worldspace is not known";
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

		// the 1-based index of a worldspace in the save's visited worldspace array, appended when missing: the game's writer
		// (0xbeab40) gives a worldspace not yet in the array the next index the same way, and the loader (0xbeadb0) numbers
		// the array's entries 1, 2, ... in order
		std::size_t WorldspaceIndex(SaveFile& a_save, std::uint32_t a_ws)
		{
			for (std::size_t i = 0; i < a_save.visitedWorldspaces.size(); ++i) {
				if (a_save.visitedWorldspaces[i] == a_ws) {
					return i + 1;
				}
			}
			a_save.visitedWorldspaces.push_back(a_ws);
			return a_save.visitedWorldspaces.size();
		}
	}

	std::vector<CellInfo> ListCells(const SaveFile& a_save, const fo4world::Lookup& a_lookup) { return Collect(a_save, a_lookup, {}); }

	namespace
	{
	// one cell of a_cells (the save's cells before any of this batch's edits; a cell's own data does not depend on the
	// other cells' change forms)
	std::string ResetOne(SaveFile& a_save, std::uint32_t a_cellFormID, const std::vector<CellInfo>& a_cells, const fo4world::Lookup& a_lookup)
	{
		const auto& cells = a_cells;
		const auto it = std::find_if(cells.begin(), cells.end(), [&](const CellInfo& c) { return c.formID == a_cellFormID; });
		if (it == cells.end()) {
			const auto type = a_lookup.type ? a_lookup.type(a_cellFormID) : std::string();
			if (!type.empty() && type != "CELL") {
				return Hex(a_cellFormID) + ": it is not a cell (" + type + ")";
			}
			return Hex(a_cellFormID) + ": neither the save nor the loaded plugins know this cell";
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
		const int  interior = it->exterior ? 0 : 1;
		const auto visitedBefore = a_save.visitedWorldspaces;
		const auto formIDsBefore = a_save.formIDs;
		ChangeForm copy;
		std::vector<std::uint8_t> body;
		if (cfp) {
			const auto d = DecodeCell(a_save, *cfp, interior);
			if (!d.complete) {
				return Hex(a_cellFormID) + ": its saved data could not be read: " + d.error;
			}
			copy = *cfp;
			body = cf::Body(copy);
			if (d.initialType) {
				// the detach time is the last u32 of the initial data
				for (int k = 0; k < 4; ++k) {
					body[d.initialSize - 4 + k] = 0xFF;
				}
				cf::SetBody(copy, body);
			}
		} else {
			// a new CELL change form with no other change
			copy.refID = RefFor(a_save, a_cellFormID);
			copy.changeFlags = 0;
			copy.typeField = kCellType;
			copy.version = a_save.formVersion;
			for (const auto& o : a_save.changeForms) {
				if (o.typeCode() == kCellType) {
					copy.version = o.version;
					break;
				}
			}
		}
		if (!(copy.changeFlags & kBitDetach)) {
			// as 0x4c9330 marks the cell: bit 30, and an exterior also bit 29 when both coordinates fit in a signed byte,
			// else bit 28; the initial data (0xbf72f0) is then {u32 detach} (interior), {u16 worldspace index, i8 x, i8 y,
			// u32 detach} (bit 29) or {u16 index, i16 x, i16 y, u32 detach} (bit 28)
			std::vector<std::uint8_t> init;
			copy.changeFlags |= kBitDetach;
			if (it->exterior) {
				copy.changeFlags |= FitsByte(it->x) && FitsByte(it->y) ? kBitChar : kBitShort;
				const bool small = (copy.changeFlags & kBitChar) != 0;  // (bit 29 wins over 28: 0xbf6c40)
				if (small ? !(FitsByte(it->x) && FitsByte(it->y)) : (it->x < -32768 || it->x > 32767 || it->y < -32768 || it->y > 32767)) {
					a_save.formIDs = formIDsBefore;
					return Hex(a_cellFormID) + ": its place on the map does not fit the cell data its saved flags call for";
				}
				const auto idx = WorldspaceIndex(a_save, it->worldspace);
				if (idx > 0xFFFF) {
					a_save.visitedWorldspaces = visitedBefore;
					a_save.formIDs = formIDsBefore;
					return Hex(a_cellFormID) + ": too many worldspaces in the save";
				}
				init.push_back(static_cast<std::uint8_t>(idx));
				init.push_back(static_cast<std::uint8_t>(idx >> 8));
				for (const int v : { it->x, it->y }) {
					init.push_back(static_cast<std::uint8_t>(v));
					if (!small) {
						init.push_back(static_cast<std::uint8_t>(v >> 8));
					}
				}
			}
			init.insert(init.end(), 4, 0xFF);
			body.insert(body.begin(), init.begin(), init.end());
			cf::SetBody(copy, body);
		}
		const auto check = DecodeCell(a_save, copy, interior);
		const bool where = !it->exterior || (check.worldspace == it->worldspace && check.x == it->x && check.y == it->y);
		if (!check.complete || check.detach != kDetachReset || !where || (check.initialType == 3) == it->exterior) {
			a_save.visitedWorldspaces = visitedBefore;
			a_save.formIDs = formIDsBefore;
			return Hex(a_cellFormID) + ": the edited cell does not read back (" + check.error + ")";
		}
		if (cfp) {
			*cfp = std::move(copy);
			return "";
		}
		std::size_t at = a_save.changeForms.size();
		for (std::size_t i = 0; i < a_save.changeForms.size(); ++i) {
			if (a_save.changeForms[i].typeCode() == kCellType) {
				at = i + 1;
			}
		}
		a_save.changeForms.insert(a_save.changeForms.begin() + static_cast<std::ptrdiff_t>(at), std::move(copy));
		a_save.reindexChangeForms();
		return "";
	}
	}

	std::string ResetCell(SaveFile& a_save, std::uint32_t a_cellFormID, const fo4world::Lookup& a_lookup)
	{
		return ResetCells(a_save, { a_cellFormID }, a_lookup).front();
	}

	std::vector<std::string> ResetCells(SaveFile& a_save, const std::vector<std::uint32_t>& a_cellFormIDs, const fo4world::Lookup& a_lookup)
	{
		std::vector<std::string> out;
		if (!a_save.isFO4()) {
			out.assign(std::max<std::size_t>(a_cellFormIDs.size(), 1), "not a Fallout 4 save");
			return out;
		}
		const auto cells = Collect(a_save, a_lookup, a_cellFormIDs);
		std::set<std::uint32_t> done;
		for (const auto id : a_cellFormIDs) {
			if (done.contains(id)) {
				out.push_back(Hex(id) + ": a reset is already set for it");
				continue;
			}
			out.push_back(ResetOne(a_save, id, cells, a_lookup));
			if (out.back().empty()) {
				done.insert(id);
			}
		}
		if (out.empty()) {
			out.push_back("no cell given");
		}
		return out;
	}
}
