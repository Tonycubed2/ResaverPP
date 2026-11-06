// ReSaver++ - Fallout 4: items dropped or created in the world, and their removal (see WorldItemsFO4.h).
#include "WorldItemsFO4.h"

#include "ChangeFormData.h"
#include "ChangeFormFO4.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace ess::fo4world
{
	namespace
	{
		constexpr std::uint8_t kExtraReferenceHandles = 0x1C;  // the object's holder (ExtraDataList::LoadGame case 0x1C)
		constexpr std::uint8_t kExtraCount = 0x24;
		constexpr std::uint8_t kExtraAliasArray = 0x88;        // ExtraAliasInstanceArray (checked by 0x4feb70 / 0x4fcd60)
		constexpr std::uint8_t kExtraAlias2 = 0x8C;            // seen with 0x88 on quest alias references
		constexpr std::uint8_t kExtraFromAlias = 0x95;

		std::string Hex(std::uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof b, "%08X", v);
			return b;
		}

		// where the id of each candidate appears outside its own change form
		struct RefScan
		{
			std::unordered_map<std::uint32_t, std::string> where;  // low 24 bits of the form id -> first place found

			void scan(const std::vector<std::uint8_t>& a_bytes, const std::unordered_set<std::uint32_t>& a_ids, const std::string& a_place,
				std::uint32_t a_skip)
			{
				const auto n = a_bytes.size();
				for (std::size_t i = 0; i + 3 <= n; ++i) {
					const std::uint8_t b0 = a_bytes[i];
					// a RefID of a created form: type 2 in the top two bits (0x80..0xBF)
					if ((b0 & 0xC0) == 0x80) {
						const std::uint32_t v = (static_cast<std::uint32_t>(b0 & 0x3F) << 16) | (a_bytes[i + 1] << 8) | a_bytes[i + 2];
						if (v != a_skip && a_ids.contains(v)) {
							where.try_emplace(v, a_place);
						}
					}
					// a u32 form id 0xFF......
					if (i + 4 <= n && a_bytes[i + 3] == 0xFF) {
						const std::uint32_t v = a_bytes[i] | (a_bytes[i + 1] << 8) | (static_cast<std::uint32_t>(a_bytes[i + 2]) << 16);
						if (v != a_skip && a_ids.contains(v)) {
							where.try_emplace(v, a_place);
						}
					}
				}
			}
		};

		bool HasExtra(const std::vector<fo4cf::Extra>& a_extras, std::uint8_t a_type)
		{
			return std::any_of(a_extras.begin(), a_extras.end(), [&](const fo4cf::Extra& e) { return e.type == a_type; });
		}
	}

	std::string KindOf(const std::string& a_type, bool* a_item)
	{
		struct K
		{
			const char* type;
			const char* kind;
		};
		static const K kinds[] = { { "WEAP", "Weapon" }, { "ARMO", "Armor / clothing" }, { "AMMO", "Ammo" }, { "MISC", "Junk / misc" },
			{ "ALCH", "Aid (food, chems)" }, { "BOOK", "Book / magazine" }, { "NOTE", "Holotape" }, { "KEYM", "Key" }, { "INGR", "Ingredient" },
			{ "CMPO", "Component" }, { "LVLI", "Leveled item" } };
		for (const auto& k : kinds) {
			if (a_type == k.type) {
				if (a_item) *a_item = true;
				return k.kind;
			}
		}
		if (a_item) *a_item = false;
		return a_type.empty() ? std::string("Unknown") : "Other (" + a_type + ")";
	}

	Lookup LookupFrom(const PluginDB* a_db)
	{
		Lookup look;
		if (!a_db || !a_db->loaded()) {
			return look;
		}
		look.type = [a_db](std::uint32_t id) {
			const auto* f = a_db->find(id);
			return f ? a_db->type(*f) : std::string();
		};
		look.name = [a_db](std::uint32_t id) {
			const auto* f = a_db->find(id);
			if (!f) return std::string();
			auto n = a_db->name(*f);
			return n.empty() ? a_db->edid(*f) : n;
		};
		look.cell = [a_db](std::uint32_t id) {
			CellRecord r;
			if (const auto* c = a_db->cell(id); c && c->hasData) {
				r.known = true;
				r.interior = c->interior();
				r.persistent = c->persistent;
				r.hasGrid = c->hasGrid;
				r.worldspace = c->worldspace;
				r.x = c->x;
				r.y = c->y;
				r.plugin = c->plugin < a_db->plugins().size() ? a_db->plugins()[c->plugin] : std::string();
			}
			return r;
		};
		look.cellAt = [a_db](std::uint32_t ws, int x, int y) { return a_db->cellAt(ws, x, y); };
		return look;
	}

	std::string PlaceOf(const Item& a_item, const Lookup& a_lookup)
	{
		std::string n = a_lookup.name ? a_lookup.name(a_item.space) : std::string();
		if (n.empty()) {
			n = Hex(a_item.space);
		}
		if (a_item.exterior) {
			char b[48];
			std::snprintf(b, sizeof b, " (%d, %d)", a_item.gridX, a_item.gridY);
			n += b;
		}
		return n;
	}

	Listing List(const SaveFile& a_save, const Lookup& a_lookup)
	{
		Listing out;
		if (!a_save.isFO4()) {
			return out;
		}
		const std::unordered_set<std::uint32_t> worldspaces(a_save.visitedWorldspaces.begin(), a_save.visitedWorldspaces.end());
		std::vector<std::size_t> cfIndex;
		for (std::size_t i = 0; i < a_save.changeForms.size(); ++i) {
			const auto& c = a_save.changeForms[i];
			if (c.typeCode() != static_cast<std::uint8_t>(ChangeFormType::REFR) || c.refID.type() != RefID::Type::kCreated) {
				continue;
			}
			const auto body = cf::Body(c);
			const auto d = fo4cf::DecodeRefr(a_save, c, body);
			Item it;
			it.formID = a_save.formIDOf(c.refID);
			it.ref = c.refID;
			it.bodySize = body.size();
			if (!d.decoded || !d.hasInitial) {
				++out.unread;
				it.why = "its saved data could not be read" + (d.error.empty() ? std::string() : ": " + d.error);
				out.items.push_back(std::move(it));
				cfIndex.push_back(i);
				continue;
			}
			if (HasExtra(d.extras, kExtraReferenceHandles)) {
				++out.held;  // in someone's inventory, not lying in the world
				continue;
			}
			it.base = a_save.formIDOf(d.base);
			it.space = a_save.formIDOf(d.cell);
			std::copy(std::begin(d.pos), std::end(d.pos), it.pos);
			const std::string spaceType = a_lookup.type ? a_lookup.type(it.space) : std::string();
			it.exterior = spaceType == "WRLD" || (spaceType.empty() && worldspaces.contains(it.space));
			if (it.exterior) {
				it.gridX = static_cast<int>(std::floor(it.pos[0] / 4096.0f));
				it.gridY = static_cast<int>(std::floor(it.pos[1] / 4096.0f));
			}
			it.baseType = a_lookup.type ? a_lookup.type(it.base) : std::string();
			it.kind = KindOf(it.baseType, &it.item);
			it.name = a_lookup.name ? a_lookup.name(it.base) : std::string();
			it.plugin = a_save.pluginOf(it.base);
			it.persistent = (d.formFlags & 0x400) != 0;
			for (const auto& e : d.extras) {
				if (e.type == kExtraCount && e.range.size() >= 3) {
					it.count = static_cast<std::int16_t>(body[e.range.begin + 1] | (body[e.range.begin + 2] << 8));
				}
			}
			if (!d.complete) {
				it.why = "its saved data could not be read to the end" + (d.error.empty() ? std::string() : ": " + d.error);
			} else if (HasExtra(d.extras, kExtraAliasArray) || HasExtra(d.extras, kExtraAlias2) || HasExtra(d.extras, kExtraFromAlias)) {
				it.why = "a quest holds it (quest alias)";
			} else {
				for (const auto& inv : d.inventory) {
					for (const auto& s : inv.stacks) {
						if (HasExtra(s.extras, kExtraAliasArray) || HasExtra(s.extras, kExtraReferenceHandles)) {
							it.why = "it holds a quest item or another object";
						}
					}
				}
			}
			out.items.push_back(std::move(it));
			cfIndex.push_back(i);
		}
		if (out.items.empty()) {
			return out;
		}

		// what else in the save may refer to them
		std::unordered_set<std::uint32_t> ids;
		for (const auto& it : out.items) {
			ids.insert(it.formID & 0x3FFFFF);
		}
		RefScan scan;
		std::unordered_set<std::size_t> own(cfIndex.begin(), cfIndex.end());
		for (std::size_t i = 0; i < a_save.changeForms.size(); ++i) {
			const auto& c = a_save.changeForms[i];
			// a listed item's own change form is skipped for its own id only (its body may name another listed item)
			const std::uint32_t self = c.refID.type() == RefID::Type::kCreated ? c.refID.value() : 0xFFFFFFFFu;
			const auto fid = a_save.formIDOf(c.refID);
			scan.scan(cf::Body(c), ids, Hex(fid) + " (" + ChangeFormTypeName(c.typeCode(), true) + ")", self);
		}
		for (const auto* t : { &a_save.table1, &a_save.table2, &a_save.table3 }) {
			for (const auto& g : *t) {
				scan.scan(g.raw, ids, "global data " + std::to_string(g.type), 0xFFFFFFFFu);
			}
		}
		std::unordered_set<std::uint32_t> inArray;
		for (const auto id : a_save.formIDs) {
			if ((id >> 24) == 0xFF) {
				inArray.insert(id & 0x3FFFFF);
			}
		}
		std::unordered_map<std::uint32_t, std::size_t> scripts;
		if (const auto* p = a_save.papyrus()) {
			for (const auto& inst : p->instances) {
				if (inst.refID.type() == RefID::Type::kCreated) {
					++scripts[inst.refID.value()];
				}
			}
		}
		for (auto& it : out.items) {
			const auto v = it.formID & 0x3FFFFF;
			if (!it.why.empty()) {
				continue;
			}
			if (auto s = scripts.find(v); s != scripts.end()) {
				it.why = std::to_string(s->second) + " script(s) attached to it";
			} else if (inArray.contains(v)) {
				it.why = "the save's form id table lists it (something refers to it)";
			} else if (auto w = scan.where.find(v); w != scan.where.end()) {
				it.why = "its id appears in " + w->second + " (may refer to it)";
			}
		}
		for (auto& it : out.items) {
			it.removable = it.why.empty();
		}
		return out;
	}

	RemoveResult Remove(SaveFile& a_save, const std::set<std::uint32_t>& a_formIDs)
	{
		RemoveResult r;
		const auto   listing = List(a_save);
		std::set<std::uint32_t> refs;
		std::set<std::uint32_t> listed;
		for (const auto& it : listing.items) {
			if (!a_formIDs.contains(it.formID)) {
				continue;
			}
			listed.insert(it.formID);
			if (it.removable) {
				refs.insert(it.ref.data);
			} else {
				r.refused.push_back(Hex(it.formID) + ": " + it.why);
			}
		}
		for (const auto id : a_formIDs) {
			if (!listed.contains(id)) {
				r.refused.push_back(Hex(id) + ": not a created object lying in the world in this save");
			}
		}
		if (!refs.empty()) {
			const auto before = a_save.changeForms.size();
			a_save.changeForms.erase(std::remove_if(a_save.changeForms.begin(), a_save.changeForms.end(),
										 [&](const ChangeForm& c) { return refs.contains(c.refID.data); }),
				a_save.changeForms.end());
			a_save.reindexChangeForms();
			r.removed = before - a_save.changeForms.size();
		}
		return r;
	}
}
