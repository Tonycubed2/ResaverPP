// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Setting a script's property in the save, the way the Creation Kit's property window does it in the plugin:
// click the property, get a list of only the forms of its type (Spell property: every spell) by name, pick one.
// The value is set on the object the script was opened from (or, opened from the script itself, on every
// object that has it).
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
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

// ---- which objects a script window works on ---------------------------------------------------------

std::vector<App::PropTarget> App::scriptTargets(const std::string& a_script, std::size_t a_var, TStr a_scriptName)
{
	std::vector<PropTarget> out;
	auto&                   p = *_save->papyrus();
	if (auto it = _scriptBinding.find(Lower(a_script)); it != _scriptBinding.end()) {
		if (const auto* i = p.findInstance(it->second); i && i->data && a_var < i->data->vars.size()) {
			out.push_back({ i->id, a_var });
			return out;
		}
	}
	for (const auto& i : p.instances) {
		if (i.script == a_scriptName && i.data && a_var < i.data->vars.size()) {
			out.push_back({ i.id, a_var });
		}
	}
	return out;
}

std::string App::scriptTargetLabel(const std::string& a_script)
{
	if (!_save || !_save->papyrus()) {
		return {};
	}
	auto& p = *_save->papyrus();
	if (auto it = _scriptBinding.find(Lower(a_script)); it != _scriptBinding.end()) {
		if (const auto* i = p.findInstance(it->second)) {
			// the object's name when the plugins give one (a placed object: its base object's name)
			const auto fid = _save->formIDOf(i->refID);
			if (_db && fid) {
				if (const auto* f = _db->find(fid)) {
					const PluginDB::Form* named = f;
					if (f->base) {
						if (const auto* b = _db->find(f->base)) {
							named = b;
						}
					}
					auto n = _db->name(*named);
					if (n.empty()) {
						n = _db->edid(*named);
					}
					if (!n.empty()) {
						return "on " + n + "  (" + _save->pluginOf(fid) + ")";
					}
				}
			}
			return "on " + formLabel(i->refID);
		}
	}
	std::size_t n = 0;
	for (const auto& i : p.instances) {
		if (Lower(p.str(i.script)) == Lower(a_script)) {
			++n;
		}
	}
	return n == 1 ? std::string("on the one object that has it") : Fmt("on all %zu objects that have it in this save", n);
}

// ---- the picker ---------------------------------------------------------------------------------------

bool App::formPicker(Variable& a_var)
{
	auto&      p = *_save->papyrus();
	const auto rec = props::RecordTypeFor(_editType);

	// the form's name as the game shows it; the editor ID when it has none
	auto nameOf = [&](std::uint32_t a_fid) -> std::string {
		if (_db) {
			if (const auto* f = _db->find(a_fid)) {
				auto n = _db->name(*f);
				if (n.empty()) {
					n = _db->edid(*f);
				}
				if (!n.empty()) {
					return n;
				}
			}
		}
		return "(name unknown, from " + _save->pluginOf(a_fid) + ")";
	};
	auto edidOf = [&](std::uint32_t a_fid) -> std::string {
		if (_db) {
			if (const auto* f = _db->find(a_fid)) {
				return _db->edid(*f);
			}
		}
		return {};
	};

	ImGui::TextUnformatted(((_propName.empty() ? std::string("Property") : _propName) + "   (" + _editType + ")").c_str());
	if (!_propScript.empty()) {
		ImGui::TextDisabled("Set %s", scriptTargetLabel(_propScript).c_str());
	} else if (!_editAllTargets.empty()) {
		ImGui::TextDisabled("Set on all %zu objects", _editAllTargets.size());
	}
	{
		const Variable* c = &a_var;
		if (c->type == VType::kVariant && c->inner) {
			c = c->inner.get();
		}
		const ScriptInstance* cur = c->type == VType::kRef && c->value ? p.findInstance(c->value) : nullptr;
		const auto            fid = cur ? _save->formIDOf(cur->refID) : 0;
		ImGui::Text("Now:  %s", fid ? nameOf(fid).c_str() : "None");
	}
	ImGui::Spacing();

	// the names come from the plugins: read them now if they are not yet
	if (!_db) {
		if (!_sources.scanned()) {
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
			ImGui::TextWrapped("To list the forms by name, ReSaver Plus reads your plugins. Tell it where your mods are first: Options > "
							   "Settings > Choose mods folder.");
			ImGui::PopTextWrapPos();
		} else {
			startNames(false);
			const auto done = _dbProgress ? _dbProgress->done.load() : 0, total = _dbProgress ? _dbProgress->total.load() : 0;
			ImGui::Text("Reading your plugins for the names... %zu of %zu", done, total);
		}
		return false;
	}

	// the list: every form of the property's type, by name (like the Creation Kit's drop-down)
	const std::string key = rec + "|" + _editType + Fmt("|%p", static_cast<const void*>(_db.get()));
	if (_pickerListType != key) {
		_pickerListType = key;
		_pickerList.clear();
		if (!rec.empty()) {
			_pickerList = _db->ofType(rec);
		} else {
			// a script type (MyQuestScript): the forms that have that script attached in this save
			for (const auto& c : props::Existing(*_save, _editType)) {
				_pickerList.push_back(c.formID);
			}
			std::sort(_pickerList.begin(), _pickerList.end());
			_pickerList.erase(std::unique(_pickerList.begin(), _pickerList.end()), _pickerList.end());
		}
		std::vector<std::pair<std::string, std::uint32_t>> named;
		named.reserve(_pickerList.size());
		for (const auto f : _pickerList) {
			named.emplace_back(Lower(nameOf(f)) + "\x01" + Lower(edidOf(f)), f);
		}
		std::sort(named.begin(), named.end());
		_pickerList.clear();
		for (const auto& [n, f] : named) {
			_pickerList.push_back(f);
		}
	}

	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 40);
	if (ImGui::IsWindowAppearing()) {
		ImGui::SetKeyboardFocusHere();
	}
	ImGui::InputTextWithHint("##find", "Type to filter", _editFind, sizeof(_editFind));
	const auto find = Lower(_editFind);
	bool       changed = false, choose = false;
	if (ImGui::BeginChild("##picks", ImVec2(ImGui::GetFontSize() * 40, ImGui::GetFontSize() * 20), ImGuiChildFlags_Borders)) {
		std::vector<std::uint32_t> shown;
		shown.reserve(_pickerList.size());
		for (const auto f : _pickerList) {
			if (find.empty() || Lower(nameOf(f)).find(find) != std::string::npos || Lower(edidOf(f)).find(find) != std::string::npos) {
				shown.push_back(f);
			}
		}
		if (shown.empty()) {
			ImGui::TextDisabled(_pickerList.empty() ? "Your plugins have no %s." : "Nothing matches.", _editType.c_str());
		}
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(shown.size()));
		while (clip.Step()) {
			for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
				const auto f = shown[k];
				ImGui::PushID(static_cast<int>(f));
				if (ImGui::Selectable(nameOf(f).c_str(), _editPick == f, ImGuiSelectableFlags_AllowDoubleClick)) {
					_editPick = f;
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
						_pickerDoubleClick = true;
					}
				}
				const auto e = edidOf(f);
				if (!e.empty() && e != nameOf(f)) {
					ImGui::SameLine(ImGui::GetFontSize() * 22);
					ImGui::TextDisabled("%s", e.c_str());
				}
				ImGui::PopID();
			}
		}
	}
	ImGui::EndChild();
	if (_pickerDoubleClick && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		_pickerDoubleClick = false;
		choose = _editPick != 0;
	}
	if (!_editMsg.empty()) {
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Bad()), "%s", _editMsg.c_str());
		ImGui::PopTextWrapPos();
	}

	// read before setting: adding an object can move the variable (it lives in the save's instance list)
	const Variable* cur = &a_var;
	if (cur->type == VType::kVariant && cur->inner) {
		cur = cur->inner.get();
	}
	const bool hasValue = cur->type == VType::kRef && cur->value != 0;

	ImGui::BeginDisabled(_editPick == 0);
	if (ImGui::Button("OK", ImVec2(120, 0)) || (choose && _editPick)) {
		auto apply = [&](Variable& v) { return props::PointAt(*_save, v, _editType, _editPick); };
		if (_editAllTargets.empty()) {
			const auto o = apply(a_var);
			if (o.ok) {
				_editDone = (_propName.empty() ? std::string("Property") : _propName) + " set to " + nameOf(_editPick);
				changed = true;
			} else {
				_editMsg = o.message;
			}
		} else {
			std::size_t done = 0;
			std::string why;
			for (const auto& t : _editAllTargets) {
				auto* inst = p.findInstance(t.instance);  // found again each time: adding an object can move them
				if (!inst || !inst->data || t.var >= inst->data->vars.size()) {
					continue;
				}
				const auto o = apply(inst->data->vars[t.var]);
				o.ok ? (void)++done : (void)(why = o.message);
			}
			if (done) {
				_editDone = Fmt("%s set to %s on %zu object(s)", _propName.c_str(), nameOf(_editPick).c_str(), done);
				changed = true;
				_editAllTargets.clear();
			} else {
				_editMsg = why.empty() ? std::string("Nothing was changed.") : why;
			}
		}
	}
	ImGui::EndDisabled();
	tips::Tip("Set the property to the picked form (or double-click it in the list). Nothing is written to disk until you save.");
	if (!changed && hasValue) {
		ImGui::SameLine();
		if (ImGui::Button("Clear (None)", ImVec2(120, 0))) {
			auto clear = [](Variable& v) {
				Variable* x = &v;
				if (x->type == VType::kVariant && x->inner) {
					x = x->inner.get();
				}
				x->value = 0;
			};
			if (_editAllTargets.empty()) {
				clear(a_var);
			} else {
				for (const auto& t : _editAllTargets) {
					if (auto* inst = p.findInstance(t.instance); inst && inst->data && t.var < inst->data->vars.size()) {
						clear(inst->data->vars[t.var]);
					}
				}
				_editAllTargets.clear();
			}
			_editDone = (_propName.empty() ? std::string("Property") : _propName) + " cleared";
			changed = true;
		}
		tips::Tip("Clear the property: the script will see None here.");
	}
	return changed;
}

// ---- from the script window's left side --------------------------------------------------------------

void App::editScriptProperty(const std::string& a_script, const std::string& a_property)
{
	if (!_save || !_save->papyrus()) {
		_error = "Open a save first: a property's value is kept in the save, with the object the script is on.";
		return;
	}
	auto&      p = *_save->papyrus();
	const auto script = Lower(a_script);
	const auto prop = Lower(a_property);
	_editAllTargets.clear();
	TStr scriptName = UINT32_MAX;
	for (const auto& sc : p.scripts) {
		if (Lower(p.str(sc.name)) == script) {
			scriptName = sc.name;
			break;
		}
	}
	if (scriptName == UINT32_MAX) {
		_error = "Nothing in this save has " + a_script + " attached, so it has no " + a_property + " to set.";
		return;
	}
	// an Auto property's value is the hidden variable ::<name>_var; a variable is its own name
	const auto  names = memberNames(scriptName);
	const auto  types = memberTypes(scriptName);
	std::size_t varIdx = SIZE_MAX;
	std::string type;
	for (std::size_t k = 0; k < names.size(); ++k) {
		const auto n = Lower(names[k]);
		if (n == "::" + prop + "_var" || n == prop) {
			varIdx = k;
			type = k < types.size() ? types[k] : std::string();
			break;
		}
	}
	if (varIdx == SIZE_MAX) {
		_error = a_property + " keeps no value in the save (only Auto properties and variables do).";
		return;
	}
	const auto targets = scriptTargets(a_script, varIdx, scriptName);
	if (targets.empty()) {
		_error = "Nothing in this save has " + a_script + " attached, so it has no " + a_property + " to set.";
		return;
	}
	auto* inst = p.findInstance(targets.front().instance);
	beginEdit(&inst->data->vars[varIdx], type);
	_propScript = a_script;
	_propName = a_property;
	if (targets.size() > 1) {
		_editAllTargets = targets;  // opened from the script itself: every object that has it
	}
}

void App::propertyChooserPopup() {}  // replaced by the binding above (kept for the call in drawModals)

// screenshots: "Script@formid:property" selects that object (and with a_open, edits that property as from its script window)
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
	for (auto& i : p.instances) {
		if (Lower(p.str(i.script)) == script && _save->formIDOf(i.refID) == fid && i.data) {
			select(Cat::kInstances, i.id);
			_scriptBinding[script] = i.id;
			if (a_open) {
				editScriptProperty(p.str(i.script), a_spec.substr(colon + 1));
			}
			return;
		}
	}
}
