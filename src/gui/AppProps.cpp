// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Filling an object property of a script in the save (a Spell, a Weapon, a Quest...), like the Creation
// Kit's property window does for the plugin: pick the form by name, from the objects the save already has
// of that type, or by form id.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
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
	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}
}

bool App::formPicker(Variable& a_var)
{
	auto& p = *_save->papyrus();
	const auto rec = props::RecordTypeFor(_editType);
	ImGui::TextWrapped("Property type: %s%s", _editType.c_str(), rec.empty() ? "  (any kind of form)" : ("  (" + rec + " records)").c_str());
	ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42);
	ImGui::TextDisabled("Pick the form this property should point at, like filling a property in the Creation Kit. The script reads it "
						"the next time the save loads.");
	ImGui::PopTextWrapPos();
	ImGui::Spacing();

	auto label = [&](std::uint32_t a_fid) {
		std::string s = Fmt("%08X (%s)", a_fid, _save->pluginOf(a_fid).c_str());
		if (_db) {
			const auto d = _db->describe(a_fid);
			if (!d.empty()) {
				s += "  " + d;
			}
		}
		return s;
	};
	const auto existing = props::Existing(*_save, _editType);
	const std::string find = Lower(_editFind);

	// search
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 42);
	ImGui::InputTextWithHint("##find", "Search by name or editor ID (or type a form id)", _editFind, sizeof(_editFind));
	tips::Tip("Type part of the form's name or editor ID. The forms of the right kind from your plugins are listed, and the objects the "
			  "save already has of this type.");
	if (ImGui::BeginChild("##picks", ImVec2(ImGui::GetFontSize() * 42, ImGui::GetFontSize() * 17), ImGuiChildFlags_Borders)) {
		int shown = 0;
		if (_db && find.size() >= 2) {
			ImGui::SeparatorText("From your plugins");
			for (const auto fid : _db->search(_editFind, 400)) {
				const auto* f = _db->find(fid);
				if (!f || (!rec.empty() && _db->type(*f) != rec && !(rec == "ACHR" && _db->type(*f) == "REFR"))) {
					continue;
				}
				ImGui::PushID(static_cast<int>(fid));
				if (ImGui::Selectable(label(fid).c_str(), _editPick == fid)) {
					_editPick = fid;
					std::snprintf(_editFormID, sizeof(_editFormID), "%08X", fid);
				}
				ImGui::PopID();
				if (++shown >= 150) {
					ImGui::TextDisabled("... more: type more of the name");
					break;
				}
			}
			if (shown == 0) {
				ImGui::TextDisabled("No %s in your plugins matches.", rec.empty() ? "form" : rec.c_str());
			}
		} else if (!_db) {
			ImGui::TextWrapped("Names of your plugins' forms are not read yet.");
			if (ImGui::SmallButton("Read the plugins for names")) {
				startNames(true);
			}
			tips::Tip("Read the plugins of this save (needs your mods folder in Options), so forms can be searched by name. You can also type "
					  "the form id below.");
		}
		ImGui::SeparatorText(Fmt("Already in this save as %s (%zu)", _editType.c_str(), existing.size()).c_str());
		int n = 0;
		for (const auto& c : existing) {
			const auto l = label(c.formID);
			if (!find.empty() && Lower(l).find(find) == std::string::npos) {
				continue;
			}
			ImGui::PushID(static_cast<int>(c.id ^ (c.id >> 32)));
			if (ImGui::Selectable(l.c_str(), _editPick == c.formID)) {
				_editPick = c.formID;
				std::snprintf(_editFormID, sizeof(_editFormID), "%08X", c.formID);
			}
			ImGui::PopID();
			if (++n >= 300) {
				ImGui::TextDisabled("... more: type part of the name above");
				break;
			}
		}
		if (existing.empty()) {
			ImGui::TextDisabled("(none)");
		}
	}
	ImGui::EndChild();

	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
	if (ImGui::InputTextWithHint("##fid", "Form id: 0A012345 or Plugin.esp:012345", _editFormID, sizeof(_editFormID))) {
		_editPick = props::ParseFormID(*_save, _editFormID);
	}
	tips::Tip("The form id as the game's console shows it (load order index first, FE... for light plugins), or the plugin's name with the "
			  "form id as xEdit and the Creation Kit show it inside the plugin (Plugin.esp:012345).");
	ImGui::SameLine();
	if (_editPick) {
		ImGui::TextWrapped("%s", label(_editPick).c_str());
	} else if (_editFormID[0]) {
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Bad()), "not a form id of a plugin in this save");
	}
	if (_editPick && _db) {
		if (const auto* f = _db->find(_editPick); f && !rec.empty() && _db->type(*f) != rec && !(rec == "ACHR" && _db->type(*f) == "REFR")) {
			ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Warn()), "%s",
				Fmt("That form is a %s, but the property wants a %s: the script will see None, or errors.", _db->type(*f).c_str(), rec.c_str()).c_str());
		}
	}
	if (!_editMsg.empty()) {
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Bad()), "%s", _editMsg.c_str());
	}
	ImGui::Spacing();
	bool changed = false;
	// read before "Use this form": adding an object can move the variable (it lives in the save's instance list)
	const Variable* cur = &a_var;
	if (cur->type == VType::kVariant && cur->inner) {
		cur = cur->inner.get();
	}
	const bool hasValue = cur->type == VType::kRef && cur->value != 0;
	ImGui::BeginDisabled(_editPick == 0);
	if (ImGui::Button("Use this form", ImVec2(160, 0))) {
		const auto o = props::PointAt(*_save, a_var, _editType, _editPick);
		if (o.ok) {
			_status = o.message;
			changed = true;
		} else {
			_editMsg = o.message;
		}
	}
	ImGui::EndDisabled();
	tips::Tip("Point the property at this form. The save's own object for the form is used; for the game's own types (Spell, Weapon, Form...) "
			  "one is added if the save has none yet. Nothing is written to disk until you save.");
	if (!changed && hasValue) {
		ImGui::SameLine();
		if (ImGui::Button("Set to None", ImVec2(120, 0))) {
			Variable* v = &a_var;
			if (v->type == VType::kVariant && v->inner) {
				v = v->inner.get();
			}
			v->value = 0;
			changed = true;
		}
		tips::Tip("Clear the property: the script will see None here. Scripts not expecting None may log errors.");
	}
	(void)p;
	return changed;
}

void App::testEditProperty(const std::string& a_spec, bool a_open)
{
	if (!_save || !_save->papyrus()) {
		return;
	}
	auto&      p = *_save->papyrus();
	const auto at = a_spec.find('@'), colon = a_spec.rfind(':');
	if (at == std::string::npos || colon == std::string::npos || colon < at) {
		return;
	}
	const auto script = Lower(a_spec.substr(0, at));
	const auto fid = props::ParseFormID(*_save, a_spec.substr(at + 1, colon - at - 1));
	const auto prop = Lower(a_spec.substr(colon + 1));
	for (auto& i : p.instances) {
		if (Lower(p.str(i.script)) == script && _save->formIDOf(i.refID) == fid && i.data) {
			select(Cat::kInstances, i.id);
			if (!a_open) {
				return;
			}
			const auto names = memberNames(i.script);
			const auto types = memberTypes(i.script);
			for (std::size_t k = 0; k < names.size() && k < i.data->vars.size(); ++k) {
				if (Lower(names[k]).find(prop) != std::string::npos) {
					beginEdit(&i.data->vars[k], k < types.size() ? types[k] : std::string());
					std::snprintf(_editFind, sizeof(_editFind), "%s", "fire");
					return;
				}
			}
		}
	}
}
