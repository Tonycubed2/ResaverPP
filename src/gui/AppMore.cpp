// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The rest of ReSaver's features: filters, plugin actions, array editing, arrays analysis,
// plugin list export, log, settings, support links, script (.pex) viewer.
#include "App.h"
#include <algorithm>

#include "Fonts.h"
#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/Text.h"

#include "imgui.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <unordered_set>

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

	void TextColored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	bool HexInput(const char* label, std::uint32_t& v)
	{
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%08X", v);
		ImGui::SetNextItemWidth(90);
		if (ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase)) {
			v = static_cast<std::uint32_t>(std::strtoul(buf, nullptr, 16));
			return true;
		}
		return false;
	}
}

// ==========================================================================================
// filters (ReSaver's Filter menu)
// ==========================================================================================

bool App::drawFiltersButton()
{
	auto&      F = _filters;
	const bool active = F.noParent || F.missingParent || F.memberless || F.zeroedCanary || F.withScripts || F.longStrings || F.nullLists ||
						F.cellMinus1 || F.emptyRef || F.parseLevel || F.flagMask || F.formFlagMask || F.content[0];
	if (ImGui::Button(active ? tr("Filters *") : tr("Filters"))) {
		ImGui::OpenPopup("filters");
	}
	tips::Tip(tr("Narrow the list to rows with a particular problem or content (ReSaver's Filter menu). A * means a filter is on."));
	if (ImGui::BeginPopup("filters")) {
		bool ch = false;
		ImGui::TextDisabled("%s", tr("Checked filters in a group are alternatives (show rows matching any)."));
		ImGui::SeparatorText(tr("Scripts"));
		ch |= ImGui::Checkbox(tr("Scripts with no parent"), &F.noParent);
		tips::Tip(tr("Scripts whose parent (the script they extend) is not in the save. Usually harmless."));
		ch |= ImGui::Checkbox(tr("Scripts with missing parent"), &F.missingParent);
		tips::Tip(tr("Scripts that extend a script that is no longer in the save: often left behind by an uninstalled mod."));
		ImGui::SeparatorText(tr("Script instances"));
		if (!sf()) {  // (Starfield: an instance without data is normal)
			ch |= ImGui::Checkbox(tr("Memberless instances (script has variables, instance has none)"), &F.memberless);
			tips::Tip(tr("Script instances with no variables although their script has some: their data was lost. Often from a mod update or removal."));
		}
		ch |= ImGui::Checkbox(tr("Zeroed canaries (::iPapyrusDataVerification_var = 0)"), &F.zeroedCanary);
		tips::Tip(tr("Instances whose ::iPapyrusDataVerification_var check value is 0, a sign the game discarded their data on load (mod changed or broken)."));
		ch |= ImGui::Checkbox(tr("Forms with scripts (instances on changed forms / changed forms with scripts)"), &F.withScripts);
		tips::Tip(tr("Change forms that have script instances on them, and script instances on changed forms."));
		ImGui::SeparatorText(tr("Strings"));
		ch |= ImGui::Checkbox(tr("Long strings (512 characters or more)"), &F.longStrings);
		tips::Tip(tr("Very long script strings. Huge strings can come from mods that build text in loops and bloat the save."));
		if (!sf()) {  // (Starfield: its change forms are not decoded)
		ImGui::SeparatorText(tr("Change forms"));
		ch |= ImGui::Checkbox(tr("Form lists containing null entries"), &F.nullLists);
		tips::Tip(tr("Form lists (FLST) with empty entries: forms from removed mods. Clean > Purify FormLists removes them."));
		ch |= ImGui::Checkbox(tr("Cell(-1) change forms (moved to the deleted cell)"), &F.cellMinus1);
		tips::Tip(tr("Things moved to a deleted (invalid) cell. They are lost and can usually be deleted."));
		ch |= ImGui::Checkbox(tr("Empty REFR (no changes but empty initial data)"), &F.emptyRef);
		tips::Tip(tr("Object references with no changes saved, just empty initial data: harmless leftovers."));
		const char* levels[] = { tr("Ignore"), tr("Fully decoded"), tr("Partly decoded"), tr("Not decoded") };
		ImGui::SetNextItemWidth(160);
		ch |= ImGui::Combo(tr("Decoding"), &F.parseLevel, levels, 4);
		tips::Tip(tr("Show only change forms ReSaver++ could fully, partly or not decode. Partly or not decoded ones are left alone by edits."));
		ImGui::TextUnformatted(tr("Change flags: mask"));
		ImGui::SameLine();
		ch |= HexInput("##fm", F.flagMask);
		tips::Tip(tr("Change flags mask: which flag bits to test (hex). Rows pass when (flags & mask) == (value & mask). 0 = off."));
		ImGui::SameLine();
		ImGui::TextUnformatted(tr("value"));
		ImGui::SameLine();
		ch |= HexInput("##fv", F.flagValue);
		tips::Tip(tr("Change flags value: the bits the masked flags must equal (hex)."));
		ImGui::TextUnformatted(tr("Form flags:   mask"));
		ImGui::SameLine();
		ch |= HexInput("##ffm", F.formFlagMask);
		tips::Tip(tr("Form flags mask: which record flag bits to test (hex). 0 = off."));
		ImGui::SameLine();
		ImGui::TextUnformatted(tr("value"));
		ImGui::SameLine();
		ch |= HexInput("##ffv", F.formFlagValue);
		tips::Tip(tr("Form flags value: the bits the masked form flags must equal (hex)."));
		ImGui::TextDisabled("%s", tr("Rows pass when (flags & mask) == (value & mask). Mask 0 = off."));
		ImGui::SetNextItemWidth(420);
		ch |= ImGui::InputTextWithHint("##content", tr("content: e.g. EXTRADATA/Ownership; INITIAL/CELL/0000003C"), F.content, sizeof(F.content));
		tips::Tip(tr("Search inside decoded change forms by a path of fields, e.g. EXTRADATA/Ownership. \";\" = and, \",\" = or, \"!\" = not."));
		ImGui::TextDisabled("%s", tr("Content: \";\" = and, \",\" = or, \"!\" = not; path of fields ending in a value or a field name."));
		}
		ImGui::Separator();
		if (ImGui::Button(tr("Clear all filters"))) {
			F = Filters{};
			ch = true;
		}
		tips::Tip(tr("Turn off every filter and show all rows again."));
		if (ch) {
			_rowsDirty = true;
		}
		ImGui::EndPopup();
	}
	return active;
}

// ==========================================================================================
// plugin actions (ReSaver's plugin context menu)
// ==========================================================================================

void App::pluginActions(const std::set<std::string>& a_plugins)
{
	if (a_plugins.empty()) {
		return;
	}
	const auto label = a_plugins.size() == 1 ? *a_plugins.begin() : Fmt(tr("%zu plugins"), a_plugins.size());
	const auto both = ops::SelectPluginElements(*_save, a_plugins, true, true);
	if (ImGui::Button(Fmt(tr("Delete its change forms (%zu)"), both.changeForms.size()).c_str())) {
		confirm(tr("Delete change forms of ") + label,
			Fmt(tr("Delete the saved state of %zu form(s) defined by %s?\n\nThe forms go back to how the plugin defines them on the next load."),
				both.changeForms.size(), label.c_str()),
			[this, a_plugins] {
				const auto s = ops::SelectPluginElements(*_save, a_plugins, true, false);
				const auto kept = ops::ApplyPluginPurge(*_save, s);
				markEdited(Fmt(tr("Deleted %zu change forms"), s.changeForms.size()) + keptNote(kept));
			});
	}
	tips::Tip(tr("Delete the saved state of every form this plugin defines. They go back to how the plugin defines them on the next load."));
	ImGui::SameLine();
	if (ImGui::Button(Fmt(tr("Delete its script instances (%zu)"), both.instances.size()).c_str())) {
		confirm(tr("Delete script instances of ") + label,
			Fmt(tr("Delete %zu script instance(s) on forms of %s, and %zu thread(s) running on them?"), both.instances.size(), label.c_str(),
				both.threads.size()),
			[this, a_plugins] {
				const auto s = ops::SelectPluginElements(*_save, a_plugins, false, true);
				const auto kept = ops::ApplyPluginPurge(*_save, s);
				markEdited(Fmt(tr("Deleted %zu instances and %zu threads"), s.instances.size() - kept.size(), s.threads.size()) + keptNote(kept));
			});
	}
	tips::Tip(tr("Delete the script instances on this plugin's forms and the threads running on them. Its scripts restart fresh if the mod is still installed."));
	if (ImGui::Button(Fmt(tr("Purge %s (both)"), label.c_str()).c_str())) {
		confirm(tr("Purge ") + label,
			Fmt(tr("Delete %zu change form(s), %zu script instance(s) and %zu thread(s) of %s?\n\nFor a safety-checked removal of one mod use "
				"Mods > Remove mod from save."),
				both.changeForms.size(), both.instances.size(), both.threads.size(), label.c_str()),
			[this, a_plugins] {
				const auto s = ops::SelectPluginElements(*_save, a_plugins, true, true);
				const auto kept = ops::ApplyPluginPurge(*_save, s);
				markEdited(Fmt(tr("Purged %zu change forms, %zu instances, %zu threads"), s.changeForms.size(), s.instances.size() - kept.size(),
							   s.threads.size()) +
						   keptNote(kept));
			});
	}
	tips::Tip(tr("Delete both its change forms and its script instances. For a checked removal of a whole mod use Mods > Remove mod from save."));
	if (a_plugins.size() == 1) {
		ImGui::SameLine();
		if (ImGui::Button(tr("Set filter for this plugin"))) {
			_pluginFilter = *a_plugins.begin();
			_cat = Cat::kChangeForms;
			_filter[0] = 0;
			_selected.clear();
			_focus.reset();
			_rowsDirty = true;
		}
		tips::Tip(tr("Change Forms, Script Instances and threads then show only this plugin's elements."));
	}
}

// The change forms, script instances and threads a plugin's buttons above would delete, so they can be looked
// through first: in collapsible groups (change forms by type, instances by script), each in form ID order;
// clicking one opens it in its own list.
void App::pluginContents(const std::set<std::string>& a_plugins)
{
	auto& pc = _plContents;
	if (pc.plugins != a_plugins || pc.gen != _editGen || pc.save != _save.get()) {
		pc = PluginContents{};
		pc.plugins = a_plugins;
		pc.gen = _editGen;
		pc.save = _save.get();
		const auto sel = ops::SelectPluginElements(*_save, a_plugins, true, true);

		// change forms, by type
		std::map<std::string, std::vector<std::pair<std::uint32_t, PluginContents::Row>>> forms;
		for (const auto& c : _save->changeForms) {
			if (!sel.changeForms.count(c.refID.data)) {
				continue;
			}
			const auto  fid = _save->formIDOf(c.refID);
			std::string name = _db ? _db->describe(fid) : std::string();
			forms[ChangeFormTypeName(c.typeCode(), _save->isFO4())].push_back({ fid, { c.refID.data, Fmt("%08X", fid), Fmt(tr("%u bytes"), static_cast<unsigned>(c.data.size())), std::move(name) } });
		}
		for (auto& [type, rows] : forms) {
			std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			auto& g = pc.forms.emplace_back(type, std::vector<PluginContents::Row>{});
			for (auto& r : rows) {
				g.second.push_back(std::move(r.second));
			}
			pc.nForms += g.second.size();
		}

		// script instances, by script
		if (const auto* p = _save->papyrus()) {
			std::map<std::string, std::vector<std::pair<std::uint32_t, PluginContents::Row>>> inst;
			for (const auto& i : p->instances) {
				if (!sel.instances.count(i.id)) {
					continue;
				}
				std::string text = "on  " + formLabel(i.refID);
				if (ops::IsUnattached(i)) {
					text += "  [UNATTACHED]";
				}
				inst[p->str(i.script)].push_back(
					{ _save->formIDOf(i.refID), { static_cast<std::uint64_t>(i.id), std::move(text), {}, {} } });
			}
			for (auto& [script, rows] : inst) {
				std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				auto& g = pc.instances.emplace_back(script, std::vector<PluginContents::Row>{});
				for (auto& r : rows) {
					g.second.push_back(std::move(r.second));
				}
				pc.nInstances += g.second.size();
			}
			for (const auto& a : p->activeScripts) {
				if (sel.threads.count(a.id)) {
					pc.threads.push_back({ static_cast<std::uint64_t>(a.id), threadLabel(a), {}, {} });
				}
			}
			std::sort(pc.threads.begin(), pc.threads.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
		}
	}

	ImGui::Separator();
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", tr("What the buttons above would delete. Open a group to look through it; click a line to open it."));
	ImGui::PopStyleColor();
	if (!ImGui::BeginChild("##plcontents", ImVec2(0, 0), ImGuiChildFlags_None)) {
		ImGui::EndChild();
		return;
	}
	Cat target = Cat::kChangeForms;
	std::optional<std::uint64_t> go;
	// screenshots: RESAVERPP_TEST_PLOPEN=1 opens everything, =2 everything but the change forms
	static const char* testOpen = std::getenv("RESAVERPP_TEST_PLOPEN");
	bool               skipForms = testOpen && testOpen[0] == '2';
	auto open = [&] {
		if (testOpen && !skipForms) {
			ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		}
	};
	auto rows = [&](const std::vector<PluginContents::Row>& a_rows, Cat a_cat) {
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(a_rows.size()));
		while (clip.Step()) {
			for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
				const auto& row = a_rows[r];
				ImGui::PushID(r);
				const float x = ImGui::GetCursorPosX();
				if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap)) {
					go = row.key;
					target = a_cat;
				}
				tips::Tip(tr("Click to open it in its list"));
				ImGui::SameLine(x);
				ImGui::TextUnformatted(row.text.c_str());
				float next = x + ImGui::CalcTextSize(row.text.c_str()).x + ImGui::GetFontSize();
				if (!row.right.empty()) {
					const float col = x + ImGui::CalcTextSize("00000000").x + ImGui::CalcTextSize(tr("0000000 bytes")).x + ImGui::GetFontSize();
					ImGui::SameLine(col - ImGui::CalcTextSize(row.right.c_str()).x);
					ImGui::TextUnformatted(row.right.c_str());
					next = col + ImGui::GetFontSize();
				}
				if (!row.rest.empty()) {
					ImGui::SameLine(next);
					ImGui::TextUnformatted(row.rest.c_str());
				}
				ImGui::PopID();
			}
		}
	};
	auto groups = [&](const PluginContents::Groups& a_groups, Cat a_cat) {
		for (const auto& [name, list] : a_groups) {
			open();
			if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s  (%zu)", name.c_str(), list.size())) {
				rows(list, a_cat);
				ImGui::TreePop();
			}
		}
	};
	if (pc.nForms == 0 && pc.nInstances == 0 && pc.threads.empty()) {
		ImGui::TextDisabled("%s", tr("Nothing of this plugin is in the save."));
	}
	open();
	const bool tipOpen305 = pc.nForms && ImGui::CollapsingHeader(Fmt(tr("Change forms (%zu)###plcf"), pc.nForms).c_str());
	if (pc.nForms) {
		tips::Tip(tr("Forms this plugin defines that changed in this save."));
	}
	if (tipOpen305) {
		ImGui::PushID("cf");
		groups(pc.forms, Cat::kChangeForms);
		ImGui::PopID();
	}
	skipForms = false;
	open();
	const bool tipOpen312 = pc.nInstances && ImGui::CollapsingHeader(Fmt(tr("Script instances (%zu), by script###plsi"), pc.nInstances).c_str());
	if (pc.nInstances) {
		tips::Tip(tr("Scripts attached to this plugin's forms, grouped by script."));
	}
	if (tipOpen312) {
		ImGui::PushID("si");
		groups(pc.instances, Cat::kInstances);
		ImGui::PopID();
	}
	open();
	const bool tipOpen318 = !pc.threads.empty() && ImGui::CollapsingHeader(Fmt(tr("Threads running on them (%zu)###plth"), pc.threads.size()).c_str());
	if (!pc.threads.empty()) {
		tips::Tip(tr("Script threads that were running on this plugin's forms when you saved."));
	}
	if (tipOpen318) {
		ImGui::PushID("th");
		rows(pc.threads, Cat::kThreads);
		ImGui::PopID();
	}
	ImGui::EndChild();
	if (go) {
		if (!_pluginFilter.empty()) {
			_pluginFilter.clear();
			_rowsDirty = true;
		}
		select(target, *go);
	}
}

std::set<std::string> App::selectedPlugins() const
{
	std::set<std::string> out;
	for (auto k : _selected) {
		const bool lite = k >= 0x10000;
		const auto i = lite ? k - 0x10000 : k;
		const auto& list = lite ? _save->plugins.lite : _save->plugins.full;
		if (i < list.size()) {
			out.insert(list[i]);
		}
	}
	return out;
}

void App::exportPluginList()
{
	auto suggested = _path;
	suggested.replace_filename(_path.stem().u8string() + u8"_plugins.txt");
	std::optional<std::filesystem::path> target =
		platform::HasNativeDialogs() ? platform::SaveFileDialog(suggested, "txt", tr("Export plugin list")) : std::optional{ suggested };
	if (!target) {
		return;
	}
	std::ofstream f(*target, std::ios::binary | std::ios::trunc);
	f << "# Plugins of " << platform::ToUtf8(_path.filename()) << "\r\n";
	for (std::size_t i = 0; i < _save->plugins.full.size(); ++i) {
		f << Fmt("%02zX      ", i) << _save->plugins.full[i] << "\r\n";
	}
	for (std::size_t i = 0; i < _save->plugins.lite.size(); ++i) {
		f << Fmt("FE:%03zX  ", i) << _save->plugins.lite[i] << "\r\n";
	}
	_status = f ? tr("Plugin list exported to ") + platform::ToUtf8(target->filename()) : tr("Could not write the plugin list");
}

// ==========================================================================================
// array editing (ReSaver: Add Entry / Remove Entry / Shift Up / Shift Down)
// ==========================================================================================

void App::arrayEditor(ArrayInfo& a)
{
	ImGui::SeparatorText(tr("Edit elements"));
	const int n = static_cast<int>(a.vars.size());
	if (_arrayIndex >= n) {
		_arrayIndex = n ? n - 1 : 0;
	}
	ImGui::SetNextItemWidth(110);
	ImGui::InputInt(tr("element #"), &_arrayIndex);
	tips::Tip(tr("Which element of the array the buttons act on (counting from 0)."));
	_arrayIndex = std::clamp(_arrayIndex, 0, std::max(0, n - 1));
	ImGui::SameLine();
	ImGui::BeginDisabled(n == 0);
	if (ImGui::Button(tr("Remove"))) {
		a.vars.erase(a.vars.begin() + _arrayIndex);
		markChanged(Fmt(tr("Removed element %d of an array"), _arrayIndex));
	}
	tips::Tip(tr("Delete this element from the array. Scripts reading the array by position will see later elements shift down."));
	ImGui::SameLine();
	ImGui::BeginDisabled(_arrayIndex == 0);
	if (ImGui::Button(tr("Shift up"))) {
		std::swap(a.vars[_arrayIndex], a.vars[_arrayIndex - 1]);
		--_arrayIndex;
		markChanged(tr("Moved an array element up"));
	}
	tips::Tip(tr("Swap this element with the one before it."));
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(_arrayIndex + 1 >= n);
	if (ImGui::Button(tr("Shift down"))) {
		std::swap(a.vars[_arrayIndex], a.vars[_arrayIndex + 1]);
		++_arrayIndex;
		markChanged(tr("Moved an array element down"));
	}
	tips::Tip(tr("Swap this element with the one after it."));
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(a.type == VType::kStructArray || a.type == VType::kStruct || a.type == VType::kNull);
	if (ImGui::Button(tr("Add entry"))) {
		// ArrayInfo::type is the element type in SE saves (Ref, String, Int, ...); array codes are mapped too
		auto t = a.type;
		if (IsArrayType(t)) {
			t = static_cast<VType>(static_cast<std::uint8_t>(t) - 10);
		}
		Variable v;
		v.type = t;
		switch (t) {
		case VType::kRef:
			v.refType = a.refType;
			break;
		case VType::kString:
			v.value = _save->papyrus()->internString("");
			break;
		case VType::kVariant:
			v.inner = std::make_unique<Variable>();
			break;
		default:
			break;
		}
		a.vars.push_back(std::move(v));
		_arrayIndex = static_cast<int>(a.vars.size()) - 1;
		markChanged(tr("Added an array element (edit its value above)"));
	}
	tips::Tip(tr("Add an empty element of the array type at the end."));
	ImGui::EndDisabled();
}

// ==========================================================================================
// arrays analysis (ReSaver: Analyze Arrays Block)
// ==========================================================================================

void App::drawArraysAnalysis()
{
	prepareWindow("Arrays block", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin(trId("Arrays block"), &_showArrays, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Arrays block");
	auto& p = *_save->papyrus();
	if (_arraysDirty) {
		_arraysDirty = false;
		_arrStats.clear();
		_arrBiggest.clear();
		_arrOrphans.clear();
		std::unordered_set<EID> held;
		auto                    hold = [&](const std::vector<Variable>& vars) {
            for (const auto& v : vars) {
                const Variable* x = v.type == VType::kVariant && v.inner ? v.inner.get() : &v;
                if (IsArrayType(x->type)) {
                    held.insert(x->value);
                }
            }
		};
		for (const auto& i : p.instances) {
			if (i.data) {
				hold(i.data->vars);
			}
		}
		for (const auto& r : p.references) {
			if (r.data) {
				hold(r.data->vars);
			}
		}
		for (const auto& a : p.arrays) {
			hold(a.vars);
		}
		for (const auto& t : p.activeScripts) {
			if (t.data) {
				for (const auto& f : t.data->frames) {
					hold(f.vars);
				}
			}
		}
		for (const auto* list : { &p.suspended1, &p.suspended2 }) {
			for (const auto& s : *list) {
				if (s.data) {
					hold(s.data->vars);
				}
			}
		}
		for (const auto& m : p.functionMessages) {
			if (m.data) {
				hold(m.data->vars);
			}
		}
		std::map<std::string, std::pair<std::size_t, std::size_t>> stats;
		for (const auto& a : p.arrays) {
			const auto t = IsRefType(a.type) ? p.str(a.refType) + "[]" : std::string(VTypeName(a.type));
			auto&      s = stats[t];
			++s.first;
			s.second += a.vars.size();
			_arrBiggest.push_back({ a.vars.size(), a.id });
			if (!held.contains(a.id)) {
				_arrOrphans.push_back(a.id);
			}
		}
		for (const auto& [t, s] : stats) {
			_arrStats.push_back({ t, s.first, s.second });
		}
		std::sort(_arrStats.begin(), _arrStats.end(), [](const auto& a, const auto& b) { return std::get<2>(a) > std::get<2>(b); });
		std::sort(_arrBiggest.rbegin(), _arrBiggest.rend());
		if (_arrBiggest.size() > 50) {
			_arrBiggest.resize(50);
		}
	}
	std::size_t total = 0;
	for (const auto& a : p.arrays) {
		total += a.vars.size();
	}
	ImGui::Text(tr("%zu arrays, %zu elements in all, %zu not held by anything"), p.arrays.size(), total, _arrOrphans.size());
	if (ImGui::Button(tr("Refresh"))) {
		_arraysDirty = true;
	}
	tips::Tip(tr("Count the arrays and find which ones nothing refers to again."));
	if (!_arrOrphans.empty()) {
		ImGui::SameLine();
		if (ImGui::Button(Fmt(tr("Delete the %zu arrays nothing holds"), _arrOrphans.size()).c_str())) {
			const std::set<EID> ids(_arrOrphans.begin(), _arrOrphans.end());
			confirm(tr("Delete unreferenced arrays"),
				Fmt(tr("Delete %zu arrays that no instance, reference, array, thread or queued call refers to?"), ids.size()), [this, ids] {
					markEdited(Fmt(tr("Deleted %zu unreferenced arrays"), ops::DeleteArrays(*_save->papyrus(), ids)));
					_arraysDirty = true;
				});
		}
		tips::Tip(tr("Delete arrays that no script instance, reference, array, thread or queued call points to. They are dead weight in the save."));
	}
	if (ImGui::BeginTabBar("arr")) {
		if (ImGui::BeginTabItem(trId("By element type"))) {
			if (ImGui::BeginTable("as", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
				ImGui::TableSetupColumn(tr("Type"));
				ImGui::TableSetupColumn(tr("Arrays"), ImGuiTableColumnFlags_WidthFixed, 90);
				ImGui::TableSetupColumn(tr("Elements"), ImGuiTableColumnFlags_WidthFixed, 90);
				ImGui::TableHeadersRow();
				for (const auto& [t, n, e] : _arrStats) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(t.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::Text(tr("%zu"), n);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text(tr("%zu"), e);
				}
				ImGui::EndTable();
			}
			ImGui::EndTabItem();
		}
		auto idList = [&](const char* tab, const std::vector<EID>& ids, bool showSize) {
			if (ImGui::BeginTabItem(tab)) {
				ImGui::BeginChild("ids");
				for (const auto id : ids) {
					const auto* a = p.findArray(id);
					if (!a) {
						continue;
					}
					ImGui::PushID(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(id)));
					if (ImGui::SmallButton(tr("go"))) {
						select(Cat::kArrays, id);
					}
					tips::Tip(tr("Open this array in the Arrays list."));
					ImGui::SameLine();
					const auto t = IsRefType(a->type) ? p.str(a->refType) + "[]" : std::string(VTypeName(a->type));
					ImGui::Text("%016" PRIX64 "  %s%s", id, t.c_str(), showSize ? Fmt(tr("  %zu elements"), a->vars.size()).c_str() : "");
					ImGui::PopID();
				}
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
		};
		std::vector<EID> big;
		for (const auto& [n, id] : _arrBiggest) {
			big.push_back(id);
		}
		idList(tr("Largest arrays"), big, true);
		idList(tr("Held by nothing"), _arrOrphans, true);
		ImGui::EndTabBar();
	}
	ImGui::End();
}

// ==========================================================================================
// log, settings, support
// ==========================================================================================

void App::logStatus()
{
	auto add = [&](const std::string& s) {
		const auto now = std::time(nullptr);
		char       t[16];
		std::strftime(t, sizeof(t), "%H:%M:%S", std::localtime(&now));
		_log.push_back(std::string(t) + "  " + s);
		if (_log.size() > 5000) {
			_log.erase(_log.begin(), _log.begin() + 1000);
		}
	};
	if (_status != _lastLogged) {
		_lastLogged = _status;
		add(_status);
	}
	if (!_error.empty() && _error != _lastLoggedError) {
		_lastLoggedError = _error;
		add("ERROR: " + _error);
	}
}

void App::drawLog()
{
	prepareWindow("Log", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin(trId("Log"), &_showLog, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Log");
	if (ImGui::Button(tr("Copy"))) {
		std::string all;
		for (const auto& l : _log) {
			all += l + "\n";
		}
		ImGui::SetClipboardText(all.c_str());
	}
	tips::Tip(tr("Copy the whole log to the clipboard (e.g. to paste in a bug report)."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Clear"))) {
		_log.clear();
	}
	tips::Tip(tr("Empty the log."));
	ImGui::Separator();
	ImGui::BeginChild("log");
	for (const auto& l : _log) {
		ImGui::TextUnformatted(l.c_str());
	}
	if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
	ImGui::End();
}

const std::vector<i18n::Language>& App::languages(bool a_rescan)
{
	if (_languages.empty() || a_rescan) {
		_languages = i18n::Languages();
	}
	return _languages;
}

void App::setLanguage(const std::string& a_code)
{
	if (!i18n::SetLanguage(a_code)) {
		_status = "Language \"" + a_code + "\" was not found: English is used.";
	}
	_rowsDirty = true;  // (row labels are made once)
	saveSettings();
}

void App::drawLanguageMenu()
{
	// "Language" in both words, so someone who does not read the current language finds it
	const std::string label = i18n::IsEnglish() ? std::string("Language") : std::string(tr("Language")) + " (Language)";
	if (ImGui::BeginMenu((label + "###Language").c_str())) {
		for (const auto& l : languages()) {
			if (ImGui::MenuItem(l.name.c_str(), nullptr, l.code == i18n::Current())) {
				setLanguage(l.code);
			}
		}
		ImGui::EndMenu();
	}
	tips::Tip(tr("The language of ReSaver Plus. It changes at once and is kept in resaverpp.ini (language=)."));
}

void App::drawSettings()
{
	ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
	if (!ImGui::Begin(trId("Settings"), &_showSettings, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return;
	}
	bool ch = false;
	ImGui::SeparatorText(tr("Language"));
	{
		const auto& langs = languages();
		std::string current = "English";
		for (const auto& l : langs) {
			if (l.code == i18n::Current()) {
				current = l.name;
			}
		}
		ImGui::SetNextItemWidth(220);
		const bool open = ImGui::BeginCombo("##language", current.c_str());
		tips::Tip(tr("The language of ReSaver Plus. It changes at once and is kept in resaverpp.ini (language=)."));
		if (open) {
			for (const auto& l : langs) {
				if (ImGui::Selectable(l.name.c_str(), l.code == i18n::Current())) {
					setLanguage(l.code);
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Button(tr("Reload language files"))) {
			languages(true);
			setLanguage(i18n::Current());
		}
		tips::Tip(tr("For translators: reads lang/<code>.txt next to ReSaverPP.exe again, so your edits show without restarting. See TRANSLATING.md."));
		if (!i18n::IsEnglish()) {
			ImGui::SameLine();
			const auto info = i18n::LastLoad();
			std::string note = Fmt(tr("%zu texts built in"), info.builtin);
			if (!info.file.empty()) {
				note += Fmt(tr(", %zu from %s"), info.fromFile, info.file.c_str());
			}
			if (!info.error.empty()) {
				note += Fmt(tr(" (stopped at an error: %s)"), info.error.c_str());
			}
			ImGui::TextDisabled("%s", note.c_str());
			if (info.rejected) {
				std::string list;
				for (const auto& r : info.rejectedTexts) {
					list += "\n" + r;
				}
				TextColored(theme::Warn(), Fmt(tr("%zu translated text(s) not used: their codes (%%s, %%d, %%zu...) are not the same, in the same "
												  "order, as in the English. They show in English until fixed."),
											   info.rejected));
				tips::Tip(tr("The program fills the codes in the order they appear, so a code moved or changed would show the wrong value or "
							 "crash. Reword the translation so its codes come in the English order.") +
						  list);
			}
		}
	}
	ImGui::SeparatorText(tr("Game"));
	{
		int g = _game;
		const char* games[] = { "Skyrim Special Edition / Anniversary Edition", "Skyrim VR", "Fallout 4", "Starfield" };
		ImGui::SetNextItemWidth(380);
		if (ImGui::Combo("##game", &g, games, 4)) {
			setGame(g);
		}
		tips::Tip(tr("Which game the folders below are for. Switches by itself to the game of the save you open."));
		ImGui::SameLine();
		ImGui::TextDisabled("%s", tr("(switches by itself to the game of the save you open)"));
	}
	ImGui::SeparatorText(tr("Script sources and plugins"));
	ImGui::TextWrapped(tr("Mods folder: %s"), _sourcesPath.empty() ? tr("(not set)") : _sourcesPath.c_str());
	ImGui::TextDisabled("%s", tr("Your Mod Organizer \"mods\" folder, Vortex's staging folder (or the game folder: Vortex is found from it),\n"
						"or the game's Data folder. Used for script sources, the script viewer, plugin names and Remove mod."));
	if (ImGui::Button(tr("Choose mods folder..."))) {
		if (platform::HasNativeDialogs()) {
			if (auto p = platform::PickFolderDialog(tr("Pick your Mod Organizer \"mods\" folder (or the game's Data folder)"))) {
				scanSources(*p);
			}
		} else {
			std::snprintf(_scanPathBuf, sizeof(_scanPathBuf), "%s", _sourcesPath.c_str());
			_scanPathPopup = true;
		}
	}
	tips::Tip(tr("Pick your Mod Organizer \"mods\" folder, Vortex's staging folder or the game's Data folder. ReSaver++ reads scripts and plugin names from it."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Use a ModOrganizer.ini...")) && platform::HasNativeDialogs()) {
		if (auto p = platform::OpenFileDialog({}, "ini", tr("Pick ModOrganizer.ini"))) {
			if (auto m = ReadMo2Ini(*p)) {
				useMo2Ini(*p, *m);
			} else {
				_error = tr("Could not read ") + platform::ToUtf8(*p);
			}
		}
	}
	tips::Tip(tr("Pick a ModOrganizer.ini: its mods folder and profiles are used (handy for portable MO2 instances)."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Detect automatically"))) {
		autoDetectPaths(true);
	}
	tips::Tip(tr("Look for Mod Organizer, Vortex and the game in the usual places and use what is found."));
	if (!_detectNote.empty()) {
		ImGui::TextDisabled("%s", _detectNote.c_str());
	}
	ImGui::TextWrapped(tr("Game folder: %s"), gameFolder().empty() ? tr("(not found)") : platform::ToUtf8(gameFolder()).c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton(tr("Change...##game")) && platform::HasNativeDialogs()) {
		if (auto p = platform::PickFolderDialog(_game == 3   ? tr("Pick the Starfield folder (the one with Starfield.exe)")
												: _game == 2 ? tr("Pick the Fallout 4 folder (the one with Fallout4.exe)")
												: _vr      ? tr("Pick the Skyrim VR folder (the one with SkyrimVR.exe)")
														   : tr("Pick the Skyrim folder (the one with SkyrimSE.exe, or TESV.exe for Legendary Edition)"))) {
			_gamePath = platform::ToUtf8(*p);
			ch = true;
		}
	}
	tips::Tip(tr("Pick the game folder (the one with SkyrimSE.exe, SkyrimVR.exe, Fallout4.exe or, for Legendary Edition, TESV.exe). Needed for the Papyrus compiler and the Data folder."));
	{
		const auto comp = compilerPath();
		std::error_code ec;
		ImGui::TextDisabled(tr("Papyrus compiler: %s"), !comp.empty() && std::filesystem::exists(comp, ec) ? platform::ToUtf8(comp).c_str()
																										: tr("not found (it comes with the Creation Kit)"));
	}
	{
		auto& custom = savesDirSetting();
		ImGui::TextWrapped(tr("Saves folder: %s"), platform::ToUtf8(savesFolder()).c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton(tr("Change...##saves")) && platform::HasNativeDialogs()) {
			if (auto p = platform::PickFolderDialog(tr("Pick the folder with your saves (File > Open starts there)"))) {
				custom = platform::ToUtf8(*p);
				ch = true;
				if (_watch) {
					setWatching(true);
				}
			}
		}
		tips::Tip(tr("Pick the folder File > Open starts in and \"Watch saves folder\" watches."));
		if (!custom.empty()) {
			ImGui::SameLine();
			if (ImGui::SmallButton(tr("Find it by itself##saves"))) {
				custom.clear();
				ch = true;
				if (_watch) {
					setWatching(true);
				}
			}
			tips::Tip(tr("Forget the folder you chose and find the saves folder by itself again."));
		}
		ImGui::TextDisabled("%s", custom.empty() ? tr("Found by itself: the folder you last opened a save from, else the one with your newest save\n"
											 "(Documents\\My Games, sLocalSavePath in Skyrim.ini / Fallout4.ini, Mod Organizer profile saves).")
										   : tr("Chosen by you: File > Open starts here and \"Watch saves folder\" watches it."));
	}
	// Mod Organizer instances installed for this user (%LOCALAPPDATA%\ModOrganizer\<instance>)
	if (const auto la = text::EnvPath("LOCALAPPDATA"); !la.empty()) {
		std::error_code ec;
		const auto      root = la / "ModOrganizer";
		for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
			const auto ini = e.path() / "ModOrganizer.ini";
			if (!e.is_directory(ec) || !std::filesystem::exists(ini, ec)) {
				continue;
			}
			if (auto m = ReadMo2Ini(ini); m && !m->modsDir.empty()) {
				const auto mods = platform::ToUtf8(m->modsDir);
				if (mods == _sourcesPath) {
					continue;
				}
				ImGui::PushID(mods.c_str());
				if (ImGui::SmallButton(tr("Use"))) {
					scanSources(m->modsDir);
				}
				tips::Tip(tr("Use this Mod Organizer instance's mods folder."));
				ImGui::SameLine();
				ImGui::TextDisabled(tr("Mod Organizer \"%s\": %s"), platform::ToUtf8(e.path().filename()).c_str(), mods.c_str());
				ImGui::PopID();
			}
		}
	}
	if (!_vortex && !_mo2 && !gameFolder().empty()) {
		std::error_code ec;
		if (std::filesystem::exists(gameFolder() / "Data" / "vortex.deployment.json", ec)) {
			TextColored(theme::Warn(), tr("The game's Data folder has Vortex's vortex.deployment.json, but it could not be read: mods are credited to the Data folder."));
		}
	}
	if (_vortex && !_mo2) {
		ImGui::TextWrapped(tr("Vortex: staging folder %s, %zu mods deployed to %s (%s)"), platform::ToUtf8(_vortex->stagingPath).c_str(), _vortex->mods.size(),
			platform::ToUtf8(_vortex->dataPath).c_str(), _vortex->method.empty() ? "?" : _vortex->method.c_str());
	}
	if (_mo2) {
		ImGui::TextWrapped(tr("Mod Organizer: %s"), platform::ToUtf8(_mo2->ini).c_str());
		const bool tipOpen767 = ImGui::BeginCombo(tr("MO2 profile"), _profile.c_str());
		tips::Tip(tr("The Mod Organizer profile whose load order and enabled mods are used."));
		if (tipOpen767) {
			for (const auto& pr : _mo2->profiles) {
				if (ImGui::Selectable(pr.c_str(), pr == _profile)) {
					_profile = pr;
					ch = true;
					if (!_sourcesPath.empty()) {
						scanSources(platform::FromUtf8(_sourcesPath));
					}
				}
			}
			ImGui::EndCombo();
		}
	}
	if (!_sourcesPath.empty() && ImGui::Button(tr("Rescan mods folder"))) {
		scanSources(platform::FromUtf8(_sourcesPath));
	}
	if (!_sourcesPath.empty()) {
		tips::Tip(tr("Read the mods folder again (after installing or removing mods)."));
	}
	ch |= ImGui::Checkbox(tr("Read plugin names automatically"), &_autoNames);
	tips::Tip(tr("Read your plugins after a scan so forms show their names and editor IDs. Takes a few seconds with big load orders."));
	ImGui::SeparatorText(tr("Language of game text"));
	{
		struct Cp
		{
			unsigned    cp;
			const char* name;
		};
		static const Cp cps[] = { { 0, N_("Automatic") }, { 65001, "UTF-8" }, { 1252, N_("Western European 1252") },
			{ 1250, N_("Central European (Polish, Czech, Hungarian...) 1250") }, { 1251, N_("Cyrillic (Russian, Ukrainian...) 1251") },
			{ 1253, N_("Greek 1253") }, { 1254, N_("Turkish 1254") }, { 1257, N_("Baltic 1257") }, { 1258, N_("Vietnamese 1258") },
			{ 936, N_("Chinese Simplified GBK 936") }, { 950, N_("Chinese Traditional Big5 950") }, { 932, N_("Japanese Shift-JIS 932") },
			{ 949, N_("Korean 949") }, { 874, N_("Thai 874") } };
		std::string current;
		for (const auto& c : cps) {
			if (c.cp == text::CodePage()) {
				current = tr(c.name);
			}
		}
		if (text::CodePage() == 0) {
			current = Fmt(tr("Automatic (now: %s)"), text::CodePageName(text::EffectiveCodePage()).c_str());
		} else if (current.empty()) {
			current = text::CodePageName(text::CodePage());
		}
		ImGui::SetNextItemWidth(420);
		const bool tipOpen805 = ImGui::BeginCombo(tr("Text encoding of plugins and saves"), current.c_str());
		tips::Tip(tr("How names, places and texts that are not UTF-8 are read: older mods made on a Chinese, Japanese, Russian... Windows. "
					 "Automatic uses your Windows code page, or the game's language when Windows uses UTF-8."));
		if (tipOpen805) {
			for (const auto& c : cps) {
				if (ImGui::Selectable(tr(c.name), c.cp == text::CodePage())) {
					text::SetCodePage(c.cp);
					_rowsDirty = true;
					ch = true;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("%s", tr("Text that is valid UTF-8 is always shown as UTF-8 (the games and most translations use it); this is for the rest.\n"
							"Edited texts are written back in the encoding they had. Chinese, Japanese, Korean and Thai characters\n"
							"come from a font of your system when they are first shown."));
		ImGui::TextDisabled("%s", fonts::Status().c_str());
	}
	ImGui::SeparatorText(tr("Saving"));
	const char* comp[] = { tr("Keep the save's compression"), tr("LZ4 (game default)"), "zlib", tr("Uncompressed") };
	int         c = _saveCompression + 1;
	if (ImGui::Combo(tr("Compression when writing"), &c, comp, 4)) {
		_saveCompression = c - 1;
		ch = true;
	}
	tips::Tip(tr("How saves are compressed when ReSaver++ writes them. LZ4 is what the game uses; uncompressed saves are bigger but load the same."));
	ImGui::SeparatorText(tr("Live game (ReSaverHelper.dll)"));
	ImGui::TextDisabled("%s", tr("With the optional ReSaverHelper.dll in the game, the Quests page can complete quests, set stages and\n"
						"refresh the journal in the running game."));
	if (ImGui::Checkbox(tr("Connect to the running game"), &_liveEnabled)) {
		ch = true;
	}
	tips::Tip(tr("Talk to ReSaverHelper.dll in the running game to complete quests, set stages and refresh the journal from here."));
	{
		const char* modes[] = { tr("Top-left notification"), tr("Message box"), tr("No message") };
		ImGui::SetNextItemWidth(260);
		if (ImGui::Combo(tr("How the game confirms what it did"), &_liveNotify, modes, 3)) {
			ch = true;
		}
		tips::Tip(tr("How the game tells you what ReSaver++ did: a notification, a message box or nothing."));
	}
	drawRefreshJournal(true);
	drawLiveStatus();
	ImGui::SeparatorText(tr("Other"));
	bool watch = _watch;
	{
		int k = static_cast<int>(theme::Current());
		const char* names[] = { tr("Light"), tr("Grey"), tr("Dark") };
		ImGui::SetNextItemWidth(160);
		if (ImGui::Combo(tr("Theme"), &k, names, 3)) {
			setTheme(k);
		}
		tips::Tip(tr("The colors of ReSaver++."));
	}
	if (ImGui::Checkbox(tr("Watch saves folder (open new saves automatically)"), &watch)) {
		setWatching(watch);
	}
	tips::Tip(tr("Open each new save as soon as the game writes it."));
	ch |= ImGui::Checkbox(tr("Open the newest save when ReSaver Plus starts"), &_openNewest);
	tips::Tip(tr("Open your most recent save automatically when ReSaver++ starts."));
	const char* sorts[] = { tr("Save order"), tr("Alphabetical"), tr("Size"), tr("Mass") };
	if (ImGui::Combo(tr("List sorting"), &_sortMode, sorts, 4)) {
		_rowsDirty = true;
		ch = true;
	}
	tips::Tip(tr("How lists are ordered: as stored in the save, by name, by own size, or by mass (size including what it holds)."));
	ImGui::TextDisabled(tr("Settings are kept in %s"), platform::ToUtf8(platform::SettingsFile()).c_str());
	if (ch) {
		saveSettings();
	}
	ImGui::End();
}

void App::drawSupport()
{
	ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
	if (!ImGui::Begin(trId("Support"), &_showSupport, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return;
	}
	ImGui::TextWrapped("%s", tr("ReSaver++ is a C++ port of ReSaver from FallrimTools by Mark Fairchild. These are his support links, "
					   "as listed in ReSaver's own Support dialog:"));
	ImGui::Spacing();
	const char* links[][2] = { { "Patreon", "https://www.patreon.com/user?u=18285974" }, { "PayPal", "https://www.paypal.me/MarkDFSoftware" },
		{ "Nexus Mods", "https://www.nexusmods.com/users/723902" } };
	for (const auto& l : links) {
		if (ImGui::Button(l[0], ImVec2(120, 0))) {
			platform::OpenWithDefaultApp(platform::FromUtf8(l[1]));
		}
		tips::Tip(tr("Open this support link in your browser."));
		ImGui::SameLine();
		ImGui::TextDisabled("%s", l[1]);
	}
	ImGui::End();
}

// ==========================================================================================
// scripts (.pex): ReSaver opens a dropped .pex and shows its disassembly
// ==========================================================================================

void App::openScript(const std::filesystem::path& a_path)
{
	viewScriptFile(a_path);  // the script viewer decompiles it (AppScripts.cpp)
}

void App::openAny(const std::filesystem::path& a_path)
{
	const auto ext = Lower(platform::ToUtf8(a_path.extension()));
	if (ext == ".pex" || ext == ".psc") {
		viewScriptFile(a_path);
	} else if (ext == ".bsa" || ext == ".ba2") {
		openBsa(a_path);  // dropping a BSA shows what is in it
	} else if (Lower(platform::ToUtf8(a_path.filename())) == "modorganizer.ini") {
		// ReSaver: dropping ModOrganizer.ini selects that MO2 instance
		if (auto m = ReadMo2Ini(a_path)) {
			useMo2Ini(a_path, *m);
		} else {
			_error = tr("Could not read ") + platform::ToUtf8(a_path);
		}
	} else {
		open(a_path);
	}
}
