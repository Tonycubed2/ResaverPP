// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Clean > Remove instances on missing spawned objects: the instances grouped by the mod that provides
// their script (not installed / disabled / installed), with how many something else still points to,
// so the player removes the leftovers of mods that are gone and keeps what an installed mod may use.
#include "App.h"

#include "Theme.h"

#include "core/Orphans.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	void TextColored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	std::uint32_t StatusColor(orphans::Status s)
	{
		switch (s) {
		case orphans::Status::kNotInstalled: return theme::Bad();
		case orphans::Status::kDisabled: return theme::Warn();
		case orphans::Status::kInstalled: return theme::Good();
		case orphans::Status::kEngine: return theme::Dim();
		default: return theme::Dim();
		}
	}

	// ticked by default: what no installed mod can use any more
	bool DefaultTick(orphans::Status s)
	{
		return s == orphans::Status::kNotInstalled || s == orphans::Status::kDisabled || s == orphans::Status::kEngine;
	}
}

void App::buildOrphans()
{
	_orphans.clear();
	_orphanSel.clear();
	if (!_save) {
		return;
	}
	_orphansBuiltScanning = scanning();
	_orphans = orphans::Find(*_save, _orphansBuiltScanning ? nullptr : &_sources);
	for (const auto& g : _orphans) {
		_orphanSel.push_back(DefaultTick(g.status) ? 1 : 0);
	}
}

void App::openOrphans()
{
	buildOrphans();
	_orphansOpen = true;
}

void App::drawOrphansModal()
{
	if (_orphansOpenWhenReady && _save && !scanning()) {
		_orphansOpenWhenReady = false;  // (test hook)
		openOrphans();
	}
	if (_orphansOpen) {
		ImGui::OpenPopup("Instances on missing spawned objects");
		_orphansOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(1000, 660), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("Instances on missing spawned objects")) {
		return;
	}
	// the mods scan finished while this was open: now "installed" can be told
	if (_orphansBuiltScanning && !scanning()) {
		buildOrphans();
	}

	ImGui::TextWrapped("When a mod spawns something during play (a summon, a dropped item, a marker), its script gets saved with it. "
					   "These script instances belong to spawned objects that no longer exist in the save, so nothing in the world "
					   "uses them. They are grouped below by the mod that provides their script.");
	TextColored(theme::Dim(), "Not installed / disabled: leftovers of mods that are gone; safe to remove (ticked). Game object types (ActorBase, "
							  "ObjectReference...) are only the game's link to the deleted object; also ticked. Installed: a mod you still use; "
							  "it may keep a reference to them on purpose, so they are not ticked. \"Held\" counts the ones another script, "
							  "array or call still points to: removing those turns that reference into None.");
	if (scanning()) {
		TextColored(theme::Warn(), "Your mods are still being scanned; the Status column fills in when the scan finishes.");
		drawBackgroundProgress(0);
	}

	std::size_t total = 0, totalHeld = 0;
	for (const auto& g : _orphans) {
		total += g.instances;
		totalHeld += g.held;
	}
	if (_orphans.empty()) {
		ImGui::Spacing();
		ImGui::TextUnformatted("This save has no script instances on missing spawned objects.");
	} else {
		ImGui::Text("%zu instance(s) from %zu mod group(s); %zu held by something else.", total, _orphans.size(), totalHeld);
	}

	const float footer = ImGui::GetFrameHeightWithSpacing() * 3.2f;
	if (ImGui::BeginTable("orphans", 5,
			ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
				ImGuiTableFlags_Resizable,
			ImVec2(0, -footer))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupColumn("Mod / script", ImGuiTableColumnFlags_WidthStretch, 4.0f);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.8f);
		ImGui::TableSetupColumn("Instances", ImGuiTableColumnFlags_WidthStretch, 0.9f);
		ImGui::TableSetupColumn("Held", ImGuiTableColumnFlags_WidthStretch, 0.7f);
		ImGui::TableHeadersRow();
		for (std::size_t gi = 0; gi < _orphans.size(); ++gi) {
			const auto& g = _orphans[gi];
			ImGui::PushID(static_cast<int>(gi));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			bool tick = _orphanSel[gi] != 0;
			if (ImGui::Checkbox("##sel", &tick)) {
				_orphanSel[gi] = tick ? 1 : 0;
			}
			ImGui::TableNextColumn();
			const bool open = ImGui::TreeNodeEx("##mod", ImGuiTreeNodeFlags_SpanAvailWidth, "%s  (%zu script%s)", g.mod.c_str(),
				g.scripts.size(), g.scripts.size() == 1 ? "" : "s");
			ImGui::TableNextColumn();
			ImGui::PushStyleColor(ImGuiCol_Text, StatusColor(g.status));
			ImGui::TextUnformatted(orphans::StatusText(g.status));
			ImGui::PopStyleColor();
			ImGui::TableNextColumn();
			ImGui::Text("%zu", g.instances);
			ImGui::TableNextColumn();
			if (g.held) {
				ImGui::Text("%zu", g.held);
			} else {
				ImGui::TextDisabled("0");
			}
			if (open) {
				for (const auto& s : g.scripts) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					ImGui::Indent();
					ImGui::Selectable(s.script.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						viewScript(s.script);
					}
					if (ImGui::IsItemHovered()) {
						ImGui::SetTooltip("Double-click to view the script");
					}
					ImGui::Unindent();
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					ImGui::Text("%zu", s.ids.size());
					ImGui::TableNextColumn();
					if (!s.held.empty()) {
						ImGui::Text("%zu", s.held.size());
					} else {
						ImGui::TextDisabled("0");
					}
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}

	if (ImGui::Button("Tick not installed")) {
		for (std::size_t i = 0; i < _orphans.size(); ++i) {
			_orphanSel[i] = DefaultTick(_orphans[i].status) ? 1 : 0;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Tick all")) {
		std::fill(_orphanSel.begin(), _orphanSel.end(), 1);
	}
	ImGui::SameLine();
	if (ImGui::Button("Tick none")) {
		std::fill(_orphanSel.begin(), _orphanSel.end(), 0);
	}
	ImGui::SameLine();
	ImGui::Checkbox("Keep the held ones (safer)", &_orphansKeepHeld);

	std::set<EID> ids;
	for (std::size_t i = 0; i < _orphans.size(); ++i) {
		if (!_orphanSel[i]) {
			continue;
		}
		for (const auto& s : _orphans[i].scripts) {
			for (EID id : s.ids) {
				if (!_orphansKeepHeld || !s.held.contains(id)) {
					ids.insert(id);
				}
			}
		}
	}
	ImGui::BeginDisabled(ids.empty());
	if (ImGui::Button(Fmt("Remove %zu instance(s)", ids.size()).c_str(), ImVec2(220, 0))) {
		const auto n = orphans::Remove(*_save, ids);
		ImGui::CloseCurrentPopup();
		_orphans.clear();
		_orphanSel.clear();
		markEdited(Fmt("Removed %zu instance(s) on missing spawned objects", n));
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0))) {
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	TextColored(theme::Dim(), "Only the edited save in memory changes; write it with File > Save As.");
	ImGui::EndPopup();
}
