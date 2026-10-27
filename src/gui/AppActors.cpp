// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Find NPCs: every actor (ACHR) with where the save has it, and "Move to me" for a lost follower.
#include "App.h"

#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	std::string Upper(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		return s;
	}
}

void App::drawActors()
{
	prepareWindow("Find NPCs", ImVec2(0.75f, 0.8f));
	if (!ImGui::Begin("Find NPCs", &_showActors, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Find NPCs");

	// (re)build the rows when the save, its edits or the plugin names change
	if (_actorRowsSave != _save.get() || _actorRowsGen != _editGen || _actorRowsDb != _db.get()) {
		_actorRowsSave = _save.get();
		_actorRowsGen = _editGen;
		_actorRowsDb = _db.get();
		_actorRows.clear();
		const auto places = cf::ListActors(*_save);
		const cf::ActorPlace* player = nullptr;
		for (const auto& p : places) {
			if (p.isPlayer) {
				player = &p;
			}
		}
		for (const auto& p : places) {
			ActorRow r;
			r.place = p;
			r.name = p.isPlayer ? "You (the player)" : formText(p.formID);
			if (p.baseObject) {
				r.name += "  (made in game: " + formText(p.baseObject) + ")";
			}
			if (p.hasPosition) {
				r.where = formText(p.cell) + Fmt("  at %.0f, %.0f, %.0f", p.pos[0], p.pos[1], p.pos[2]);
				if (player && player->hasPosition && player->cell == p.cell && !p.isPlayer) {
					const float dx = p.pos[0] - player->pos[0], dy = p.pos[1] - player->pos[1], dz = p.pos[2] - player->pos[2];
					r.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
				}
			} else {
				r.where = "where its plugin places it (the game never moved it)";
			}
			r.filterKey = Upper(r.name + " " + Fmt("%08X", p.formID) + " " + r.where);
			_actorRows.push_back(std::move(r));
		}
	}

	ImGui::NewLine();
	ImGui::TextWrapped("Every NPC and creature the save has a record for, with the cell (inside) or worldspace (outside) the save has them "
					   "in. \"Move to me\" puts one in front of you and resets its AI: use it for a lost or stuck follower. Changes are made "
					   "in the loaded save; nothing is written until you use File > Save or Save As.");
	ImGui::Spacing();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
	ImGui::InputTextWithHint("##actfilter", "Filter: name, form id or place", _actorFilter, sizeof(_actorFilter));
	tips::Tip("Show only actors whose name, form id or place contains this text, e.g. Lydia, Breezehome or 000A2C94.");
	ImGui::SameLine();
	ImGui::Checkbox("Only near you", &_actorNearOnly);
	tips::Tip("Only actors in the same cell (inside) or worldspace (outside) as you.");
	ImGui::SameLine();
	ImGui::Checkbox("Only actors the game moved", &_actorMovedOnly);
	tips::Tip("Hide actors the game never moved: they stand where their plugin places them, so the save has no position for them "
			  "(and they cannot be moved from here).");

	const auto filter = Upper(_actorFilter);
	std::vector<const ActorRow*> rows;
	rows.reserve(_actorRows.size());
	for (const auto& r : _actorRows) {
		if (_actorMovedOnly && !r.place.hasPosition && !r.place.isPlayer) {
			continue;
		}
		if (_actorNearOnly && r.distance < 0 && !r.place.isPlayer) {
			continue;
		}
		if (!filter.empty() && r.filterKey.find(filter) == std::string::npos) {
			continue;
		}
		rows.push_back(&r);
	}
	ImGui::SameLine();
	ImGui::TextDisabled("%zu of %zu actors", rows.size(), _actorRows.size());

	constexpr ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
	                               ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
	if (ImGui::BeginTable("actors", 4, tf)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Actor", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Where the save has it", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, ImGui::GetFontSize() * 6);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, ImGui::GetFontSize() * 8);
		ImGui::TableHeadersRow();
		if (auto* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
			_actorSortCol = specs->Specs[0].ColumnIndex;
			_actorSortAsc = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
		}
		std::stable_sort(rows.begin(), rows.end(), [&](const ActorRow* a, const ActorRow* b) {
			if (a->place.isPlayer != b->place.isPlayer) {
				return a->place.isPlayer;  // you first
			}
			bool less = false;
			switch (_actorSortCol) {
			case 0: less = a->name < b->name; break;
			case 1: less = a->where < b->where; break;
			default:
				if ((a->distance < 0) != (b->distance < 0)) {
					return a->distance >= 0;  // another place: last either way
				}
				if (a->distance == b->distance) {
					return a->name < b->name;
				}
				less = a->distance < b->distance;
			}
			return _actorSortAsc ? less : !less;
		});

		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(rows.size()));
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
				const auto& r = *rows[static_cast<std::size_t>(i)];
				ImGui::PushID(static_cast<int>(r.place.refRaw));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(r.name.c_str());
				ImGui::TableNextColumn();
				if (r.place.hasPosition) {
					ImGui::TextUnformatted(r.where.c_str());
					if (r.place.startCell && r.place.startCell != r.place.cell) {
						tips::Tipf("Its plugin placed it in %s; it has moved since.", formText(r.place.startCell).c_str());
					}
				} else {
					ImGui::TextDisabled("%s", r.where.c_str());
				}
				ImGui::TableNextColumn();
				if (r.place.isPlayer) {
					ImGui::TextDisabled("-");
				} else if (r.distance >= 0) {
					ImGui::Text("%.0f", r.distance);
					tips::Tip("Distance from you in game units (about 70 units to a metre).");
				} else {
					ImGui::TextDisabled("elsewhere");
				}
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("go")) {
					select(Cat::kChangeForms, r.place.refRaw);
				}
				tips::Tip("Open this actor's change form: its saved data, extra data and the NPC fixes.");
				if (!r.place.isPlayer) {
					ImGui::SameLine();
					ImGui::BeginDisabled(!r.place.hasPosition);
					if (ImGui::SmallButton("Move to me")) {
						const auto raw = r.place.refRaw;
						const auto name = r.name;
						confirm("Move to me",
							"Move " + name + " next to you?\n\nThe save puts it a few steps in front of you, in the cell or worldspace you are in, "
							"and resets its AI so it does not keep walking an old path. A follower then follows you again. Its own AI "
							"package can still send it home later; that is the game, not the save.\n\nKeep a copy of the original save.",
							[this, raw, name] {
								RefID id;
								id.data = raw;
								auto* cf = _save->findChangeForm(id);
								if (!cf) {
									_error = "Could not move " + name + ": its change form is gone";
									return;
								}
								if (auto why = cf::MoveActorToPlayer(*_save, *cf); !why.empty()) {
									_error = "Could not move " + name + ": " + why;
									return;
								}
								markChanged("Moved " + name + " next to you");
							});
					}
					ImGui::EndDisabled();
					if (r.place.hasPosition) {
						tips::Tip("Put this actor in front of you and reset its AI. For a lost or stuck follower.");
					} else {
						tips::Tip("The game never moved this actor, so the save has no position to change. In game: open the console, "
								  "type prid followed by its form id, then moveto player.");
					}
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	ImGui::End();
}
