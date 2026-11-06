// Fallout 4: items dropped or created in the world (core/WorldItemsFO4) and reset an area (core/CellResetFO4).
// Tools > World items and areas. Two tabs: the created objects lying in the world, by place or by mod, with removal of the
// selected ones; the cells the save knows, each with a Reset button that does what the console's ResetInterior does.
#include "App.h"
#include "Theme.h"
#include "Tips.h"
#include "imgui.h"
#include "core/CellResetFO4.h"
#include "core/WorldItemsFO4.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

using namespace ess;

struct F4WorldState
{
	const void*                     save = nullptr;
	std::uint64_t                   gen = UINT64_MAX;
	const void*                     db = nullptr;
	fo4world::Listing               items;
	std::vector<fo4cell::CellInfo>  cells;
	bool                            cellsRead = false;
	std::set<std::uint32_t>         selected;
	bool                            byMod = false, otherObjects = false, onlyRemovable = false, cellsChangedOnly = true;
	char                            filter[128] = "";
	char                            cellFilter[128] = "";
};

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	bool Contains(const std::string& a_hay, const char* a_needle)
	{
		if (!*a_needle) return true;
		std::string h = a_hay, n = a_needle;
		for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		for (auto& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return h.find(n) != std::string::npos;
	}

	fo4world::Lookup MakeLookup(const PluginDB* a_db)
	{
		fo4world::Lookup look;
		if (a_db && a_db->loaded()) {
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
		}
		return look;
	}
}

// the listing for the open save as it is now (read again after every edit and when the plugin names arrive)
static F4WorldState& WorldState(std::shared_ptr<F4WorldState>& a_state, const SaveFile& a_save, std::uint64_t a_gen, const PluginDB* a_db,
	bool a_cells)
{
	if (!a_state) a_state = std::make_shared<F4WorldState>();
	auto& s = *a_state;
	const void* db = a_db && a_db->loaded() ? a_db : nullptr;
	if (s.save != &a_save || s.gen != a_gen || s.db != db) {
		if (s.save != &a_save) s.selected.clear();
		s.save = &a_save;
		s.gen = a_gen;
		s.db = db;
		s.items = fo4world::List(a_save, MakeLookup(a_db));
		s.cells.clear();
		s.cellsRead = false;
		std::set<std::uint32_t> still;
		for (const auto& it : s.items.items) {
			if (it.removable && s.selected.contains(it.formID)) still.insert(it.formID);
		}
		s.selected = std::move(still);
	}
	if (a_cells && !s.cellsRead) {
		s.cells = fo4cell::ListCells(a_save, MakeLookup(a_db));
		s.cellsRead = true;
	}
	return s;
}

std::size_t App::f4WorldRemovable()
{
	if (!fo4()) return 0;
	auto&       s = WorldState(_f4World, *_save, _editGen, _db.get(), false);
	std::size_t n = 0;
	for (const auto& it : s.items.items) {
		n += it.removable && (it.item || !s.db) ? 1 : 0;
	}
	return n;
}

void App::drawF4World()
{
	if (!fo4()) {
		_showF4World = false;
		return;
	}
	prepareWindow("World items and areas", ImVec2(0.7f, 0.8f));
	if (!ImGui::Begin(trId("World items and areas"), &_showF4World, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("World items and areas");
	const bool wantCells = _f4WorldTab == 1;
	auto&      s = WorldState(_f4World, *_save, _editGen, _db.get(), wantCells);
	const auto look = MakeLookup(_db.get());
	if (!s.db) {
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Warn()), "%s",
			tr("Your mods are not scanned yet (or are still being scanned): names, kinds and places show as form IDs until then."));
	}
	if (ImGui::BeginTabBar("##f4world")) {
		// _f4WorldTab picks the tab when set from outside (test hook); otherwise it follows the user's clicks
		ImGuiTabItemFlags f0 = 0, f1 = 0;
		static int applied = -1;
		bool       justForced = false;
		if (applied != _f4WorldTab) {
			(_f4WorldTab == 1 ? f1 : f0) = ImGuiTabItemFlags_SetSelected;
			applied = _f4WorldTab;
			justForced = true;
		}
		if (ImGui::BeginTabItem(trId("Dropped and created items"), nullptr, f0)) {
			if (!justForced) {
				_f4WorldTab = applied = 0;
			}
			ImGui::TextWrapped("%s", tr("Things lying in the world that the game made while you played: items you dropped, loot spilled from "
										"containers and bodies, objects spawned by scripts. They are kept in the save one by one, so thousands of "
										"them make the save bigger and areas slower to load. Removing one takes it out of the save: it is gone "
										"from the world when you load. Items in inventories are not listed."));
			ImGui::PushStyleColor(ImGuiCol_Text, theme::Dim());
			ImGui::TextWrapped("%s", tr("Kept (cannot be removed here): quest items (quest aliases), objects with scripts on them, objects held "
										"by someone, and objects whose ID appears anywhere else in the save, so nothing is left pointing at a "
										"missing object."));
			ImGui::PopStyleColor();
			ImGui::Checkbox(tr("Group by mod"), &s.byMod);
			ImGui::SameLine();
			ImGui::Checkbox(tr("Also other spawned objects"), &s.otherObjects);
			tips::Tip(tr("Furniture, workshop builds, markers, activators and the like: not items. Most of them belong to settlements or quests; "
						 "remove only what you know."));
			ImGui::SameLine();
			ImGui::Checkbox(tr("Only removable"), &s.onlyRemovable);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(220);
			ImGui::InputTextWithHint("##wfilter", tr("filter: name, place, mod, ID"), s.filter, sizeof(s.filter));

			// groups
			struct Group
			{
				std::vector<const fo4world::Item*> items;
				long long                          total = 0;
				std::size_t                        removable = 0;
			};
			std::map<std::string, Group> groups;
			std::size_t shown = 0, removable = 0;
			long long   shownTotal = 0;
			for (const auto& it : s.items.items) {
				if (s.db && !it.item && !s.otherObjects) continue;
				if (s.onlyRemovable && !it.removable) continue;
				const auto place = fo4world::PlaceOf(it, look);
				const auto key = s.byMod ? it.plugin : place;
				if (*s.filter && !Contains(it.name + " " + place + " " + it.plugin + " " + it.kind + " " + Fmt("%08X %08X", it.formID, it.base), s.filter)) {
					continue;
				}
				auto& g = groups[key];
				g.items.push_back(&it);
				g.total += it.count;
				g.removable += it.removable ? 1 : 0;
				++shown;
				shownTotal += it.count;
				removable += it.removable ? 1 : 0;
			}
			ImGui::Text("%s", Fmt(tr("%zu object(s) (%lld counting stacks) in %zu group(s); %zu removable; %zu selected. %zu more are in inventories."),
								  shown, shownTotal, groups.size(), removable, s.selected.size(), s.items.held)
								  .c_str());
			if (ImGui::Button(tr("Select all removable shown"))) {
				for (const auto& [k, g] : groups)
					for (const auto* it : g.items)
						if (it->removable) s.selected.insert(it->formID);
			}
			ImGui::SameLine();
			if (ImGui::Button(tr("Clear selection"))) s.selected.clear();
			ImGui::SameLine();
			ImGui::BeginDisabled(s.selected.empty() || _readOnly);
			if (ImGui::Button(Fmt(tr("Remove selected (%zu)"), s.selected.size()).c_str())) {
				const auto ids = s.selected;
				confirm(tr("Remove world items"),
					Fmt(tr("Remove %zu object(s) from the world?\n\nTheir saved data is taken out of the save; when you load it they are "
						   "gone. Nothing else in the save changes. Keep a copy of the save (File > Save As writes a new file)."),
						ids.size()),
					[this, ids] {
						const auto r = fo4world::Remove(*_save, ids);
						if (_f4World) _f4World->selected.clear();
						markEdited(Fmt(tr("Removed %zu object(s) from the world (%zu kept)"), r.removed, r.refused.size()));
					});
			}
			ImGui::EndDisabled();
			ImGui::Separator();
			const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
			                           ImGuiTableFlags_SizingStretchProp;
			if (ImGui::BeginTable("##witems", 7, tf)) {
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 26);
				ImGui::TableSetupColumn(tr("Object"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
				ImGui::TableSetupColumn(tr("Kind"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
				ImGui::TableSetupColumn(tr("Count"), ImGuiTableColumnFlags_WidthFixed, 60);
				ImGui::TableSetupColumn(s.byMod ? tr("Place") : tr("Mod"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
				ImGui::TableSetupColumn(tr("ID"), ImGuiTableColumnFlags_WidthFixed, 80);
				ImGui::TableSetupColumn(tr("Can remove"), ImGuiTableColumnFlags_WidthStretch, 2.6f);
				ImGui::TableHeadersRow();
				int gi = 0;
				for (const auto& [key, g] : groups) {
					ImGui::PushID(gi++);
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					bool all = g.removable > 0;
					for (const auto* it : g.items)
						if (it->removable && !s.selected.contains(it->formID)) all = false;
					ImGui::BeginDisabled(g.removable == 0);
					if (ImGui::Checkbox("##g", &all)) {
						for (const auto* it : g.items) {
							if (!it->removable) continue;
							if (all) s.selected.insert(it->formID);
							else s.selected.erase(it->formID);
						}
					}
					ImGui::EndDisabled();
					ImGui::TableSetColumnIndex(1);
					const bool open = ImGui::TreeNodeEx("##node", ImGuiTreeNodeFlags_SpanAllColumns, "%s", key.c_str());
					ImGui::TableSetColumnIndex(2);
					ImGui::TextDisabled("%s", Fmt(tr("%zu object(s)"), g.items.size()).c_str());
					ImGui::TableSetColumnIndex(3);
					ImGui::TextDisabled("%lld", g.total);
					ImGui::TableSetColumnIndex(6);
					ImGui::TextDisabled("%s", Fmt(tr("%zu removable"), g.removable).c_str());
					if (open) {
						for (const auto* it : g.items) {
							ImGui::PushID(static_cast<int>(it->formID));
							ImGui::TableNextRow();
							ImGui::TableSetColumnIndex(0);
							bool sel = s.selected.contains(it->formID);
							ImGui::BeginDisabled(!it->removable);
							if (ImGui::Checkbox("##s", &sel)) {
								if (sel) s.selected.insert(it->formID);
								else s.selected.erase(it->formID);
							}
							ImGui::EndDisabled();
							ImGui::TableSetColumnIndex(1);
							ImGui::TextUnformatted((it->name.empty() ? Fmt("%08X", it->base) : it->name).c_str());
							ImGui::TableSetColumnIndex(2);
							ImGui::TextUnformatted(it->kind.c_str());
							ImGui::TableSetColumnIndex(3);
							ImGui::Text("%d", it->count);
							ImGui::TableSetColumnIndex(4);
							ImGui::TextUnformatted((s.byMod ? fo4world::PlaceOf(*it, look) : it->plugin).c_str());
							ImGui::TableSetColumnIndex(5);
							ImGui::Text("%08X", it->formID);
							ImGui::TableSetColumnIndex(6);
							if (it->removable) {
								ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Good()), "%s", tr("yes"));
							} else {
								ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Dim()), "%s", (tr("kept: ") + it->why).c_str());
							}
							ImGui::PopID();
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(trId("Reset an area"), nullptr, f1)) {
			if (!justForced) {
				_f4WorldTab = applied = 1;
			}
			if (!s.cellsRead) {
				s.cells = fo4cell::ListCells(*_save, look);
				s.cellsRead = true;
			}
			ImGui::TextWrapped("%s", tr("Reset an area does what the game's console command ResetInterior does: the cell is marked to reset, and "
										"the game resets it the next time it loads it, by its own rules. Dropped and spawned items in it are "
										"deleted (not quest items, not things someone holds), and the objects placed in it by the game and mods "
										"go through the game's own respawn code, as when a cell respawns after a while."));
			ImGui::PushStyleColor(ImGuiCol_Text, theme::Dim());
			ImGui::TextWrapped("%s", tr("Not done: the player's cell and the cells loaded around the player (the game does not reset a cell "
										 "while a save loads), an outdoor cell the save has no coordinates for, and a cell the save does not say "
										 "is indoors. Only what is in the cell when the game resets it is touched: an actor that walked elsewhere "
										 "is not. Settlements: their workshop objects belong to the settlement; resetting a settlement cell is not "
										 "recommended."));
			ImGui::PopStyleColor();
			ImGui::Checkbox(tr("Only cells with saved changes in them"), &s.cellsChangedOnly);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(220);
			ImGui::InputTextWithHint("##cfilter", tr("filter: name or ID"), s.cellFilter, sizeof(s.cellFilter));
			const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
			                           ImGuiTableFlags_SizingStretchProp;
			if (ImGui::BeginTable("##cells", 6, tf)) {
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn(tr("Cell"), ImGuiTableColumnFlags_WidthStretch, 2.6f);
				ImGui::TableSetupColumn(tr("Where"), ImGuiTableColumnFlags_WidthStretch, 1.8f);
				ImGui::TableSetupColumn(tr("Dropped items"), ImGuiTableColumnFlags_WidthFixed, 110);
				ImGui::TableSetupColumn(tr("Changed objects"), ImGuiTableColumnFlags_WidthFixed, 120);
				ImGui::TableSetupColumn(tr("ID"), ImGuiTableColumnFlags_WidthFixed, 80);
				ImGui::TableSetupColumn(tr("Reset"), ImGuiTableColumnFlags_WidthStretch, 2.4f);
				ImGui::TableHeadersRow();
				for (const auto& c : s.cells) {
					if (s.cellsChangedOnly && !c.createdItems && !c.placed) continue;
					const auto where = c.exterior ? Fmt("%s (%d, %d)", (look.name ? look.name(c.worldspace) : std::string()).c_str(), c.x, c.y)
					                              : std::string(tr("indoors"));
					if (*s.cellFilter && !Contains(c.name + " " + where + Fmt(" %08X", c.formID), s.cellFilter)) continue;
					ImGui::PushID(static_cast<int>(c.formID));
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted((c.name.empty() ? Fmt("%08X", c.formID) : c.name).c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(where.c_str());
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%zu", c.createdItems);
					if (c.createdItems) tips::Tip(Fmt(tr("%zu of them would be deleted by the reset."), c.createdRemovable).c_str());
					ImGui::TableSetColumnIndex(3);
					ImGui::Text("%zu", c.placed);
					ImGui::TableSetColumnIndex(4);
					ImGui::Text("%08X", c.formID);
					ImGui::TableSetColumnIndex(5);
					if (!c.why.empty()) {
						ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Dim()), "%s", c.why.c_str());
					} else {
						ImGui::BeginDisabled(_readOnly);
						if (ImGui::SmallButton(tr("Reset"))) {
							const auto id = c.formID;
							const auto label = c.name.empty() ? Fmt("%08X", c.formID) : c.name;
							confirm(tr("Reset an area"),
								Fmt(tr("Reset %s?\n\nThe game resets this cell the next time you go there: its dropped and spawned items are "
									   "deleted (not quest items) and what is placed in it goes through the game's own respawn code. This is "
									   "what the console command ResetInterior does. Keep a copy of the save."),
									label.c_str()),
								[this, id, label] {
									const auto why = fo4cell::ResetCell(*_save, id, MakeLookup(_db.get()));
									if (why.empty()) {
										markEdited(Fmt(tr("Reset set for %s: the game resets it the next time it loads it"), label.c_str()));
									} else {
										_status = tr("Not reset: ") + why;
									}
								});
						}
						ImGui::EndDisabled();
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::End();
}
