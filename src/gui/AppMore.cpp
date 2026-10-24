// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The rest of ReSaver's features: filters, plugin actions, array editing, arrays analysis,
// plugin list export, log, settings, support links, script (.pex) viewer.
#include "App.h"

#include "Platform.h"
#include "Theme.h"

#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
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
	if (ImGui::Button(active ? "Filters *" : "Filters")) {
		ImGui::OpenPopup("filters");
	}
	if (ImGui::BeginPopup("filters")) {
		bool ch = false;
		ImGui::TextDisabled("Checked filters in a group are alternatives (show rows matching any).");
		ImGui::SeparatorText("Scripts");
		ch |= ImGui::Checkbox("Scripts with no parent", &F.noParent);
		ch |= ImGui::Checkbox("Scripts with missing parent", &F.missingParent);
		ImGui::SeparatorText("Script instances");
		ch |= ImGui::Checkbox("Memberless instances (script has variables, instance has none)", &F.memberless);
		ch |= ImGui::Checkbox("Zeroed canaries (::iPapyrusDataVerification_var = 0)", &F.zeroedCanary);
		ch |= ImGui::Checkbox("Forms with scripts (instances on changed forms / changed forms with scripts)", &F.withScripts);
		ImGui::SeparatorText("Strings");
		ch |= ImGui::Checkbox("Long strings (512 characters or more)", &F.longStrings);
		ImGui::SeparatorText("Change forms");
		ch |= ImGui::Checkbox("Form lists containing null entries", &F.nullLists);
		ch |= ImGui::Checkbox("Cell(-1) change forms (moved to the deleted cell)", &F.cellMinus1);
		ch |= ImGui::Checkbox("Empty REFR (no changes but empty initial data)", &F.emptyRef);
		const char* levels[] = { "Ignore", "Fully decoded", "Partly decoded", "Not decoded" };
		ImGui::SetNextItemWidth(160);
		ch |= ImGui::Combo("Decoding", &F.parseLevel, levels, 4);
		ImGui::TextUnformatted("Change flags: mask");
		ImGui::SameLine();
		ch |= HexInput("##fm", F.flagMask);
		ImGui::SameLine();
		ImGui::TextUnformatted("value");
		ImGui::SameLine();
		ch |= HexInput("##fv", F.flagValue);
		ImGui::TextUnformatted("Form flags:   mask");
		ImGui::SameLine();
		ch |= HexInput("##ffm", F.formFlagMask);
		ImGui::SameLine();
		ImGui::TextUnformatted("value");
		ImGui::SameLine();
		ch |= HexInput("##ffv", F.formFlagValue);
		ImGui::TextDisabled("Rows pass when (flags & mask) == (value & mask). Mask 0 = off.");
		ImGui::SetNextItemWidth(420);
		ch |= ImGui::InputTextWithHint("##content", "content: e.g. EXTRADATA/Ownership; INITIAL/CELL/0000003C", F.content, sizeof(F.content));
		ImGui::TextDisabled("Content: \";\" = and, \",\" = or, \"!\" = not; path of fields ending in a value or a field name.");
		ImGui::Separator();
		if (ImGui::Button("Clear all filters")) {
			F = Filters{};
			ch = true;
		}
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
	const auto label = a_plugins.size() == 1 ? *a_plugins.begin() : Fmt("%zu plugins", a_plugins.size());
	const auto both = ops::SelectPluginElements(*_save, a_plugins, true, true);
	if (ImGui::Button(Fmt("Delete its change forms (%zu)", both.changeForms.size()).c_str())) {
		confirm("Delete change forms of " + label,
			Fmt("Delete the saved state of %zu form(s) defined by %s?\n\nThe forms go back to how the plugin defines them on the next load.",
				both.changeForms.size(), label.c_str()),
			[this, a_plugins] {
				const auto s = ops::SelectPluginElements(*_save, a_plugins, true, false);
				ops::ApplyPluginPurge(*_save, s);
				markEdited(Fmt("Deleted %zu change forms", s.changeForms.size()));
			});
	}
	ImGui::SameLine();
	if (ImGui::Button(Fmt("Delete its script instances (%zu)", both.instances.size()).c_str())) {
		confirm("Delete script instances of " + label,
			Fmt("Delete %zu script instance(s) on forms of %s, and %zu thread(s) running on them?", both.instances.size(), label.c_str(),
				both.threads.size()),
			[this, a_plugins] {
				const auto s = ops::SelectPluginElements(*_save, a_plugins, false, true);
				ops::ApplyPluginPurge(*_save, s);
				markEdited(Fmt("Deleted %zu instances and %zu threads", s.instances.size(), s.threads.size()));
			});
	}
	if (ImGui::Button(Fmt("Purge %s (both)", label.c_str()).c_str())) {
		confirm("Purge " + label,
			Fmt("Delete %zu change form(s), %zu script instance(s) and %zu thread(s) of %s?\n\nFor a safety-checked removal of one mod use "
				"Mods > Remove mod from save.",
				both.changeForms.size(), both.instances.size(), both.threads.size(), label.c_str()),
			[this, a_plugins] {
				const auto s = ops::SelectPluginElements(*_save, a_plugins, true, true);
				ops::ApplyPluginPurge(*_save, s);
				markEdited(Fmt("Purged %zu change forms, %zu instances, %zu threads", s.changeForms.size(), s.instances.size(), s.threads.size()));
			});
	}
	if (a_plugins.size() == 1) {
		ImGui::SameLine();
		if (ImGui::Button("Set filter for this plugin")) {
			_pluginFilter = *a_plugins.begin();
			_cat = Cat::kChangeForms;
			_filter[0] = 0;
			_selected.clear();
			_focus.reset();
			_rowsDirty = true;
		}
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Change Forms, Script Instances and threads then show only this plugin's elements.");
		}
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
		platform::HasNativeDialogs() ? platform::SaveFileDialog(suggested, "txt", "Export plugin list") : std::optional{ suggested };
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
	_status = f ? "Plugin list exported to " + platform::ToUtf8(target->filename()) : "Could not write the plugin list";
}

// ==========================================================================================
// array editing (ReSaver: Add Entry / Remove Entry / Shift Up / Shift Down)
// ==========================================================================================

void App::arrayEditor(ArrayInfo& a)
{
	ImGui::SeparatorText("Edit elements");
	const int n = static_cast<int>(a.vars.size());
	if (_arrayIndex >= n) {
		_arrayIndex = n ? n - 1 : 0;
	}
	ImGui::SetNextItemWidth(110);
	ImGui::InputInt("element #", &_arrayIndex);
	_arrayIndex = std::clamp(_arrayIndex, 0, std::max(0, n - 1));
	ImGui::SameLine();
	ImGui::BeginDisabled(n == 0);
	if (ImGui::Button("Remove")) {
		a.vars.erase(a.vars.begin() + _arrayIndex);
		markChanged(Fmt("Removed element %d of an array", _arrayIndex));
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(_arrayIndex == 0);
	if (ImGui::Button("Shift up")) {
		std::swap(a.vars[_arrayIndex], a.vars[_arrayIndex - 1]);
		--_arrayIndex;
		markChanged("Moved an array element up");
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(_arrayIndex + 1 >= n);
	if (ImGui::Button("Shift down")) {
		std::swap(a.vars[_arrayIndex], a.vars[_arrayIndex + 1]);
		++_arrayIndex;
		markChanged("Moved an array element down");
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(a.type == VType::kStructArray || a.type == VType::kStruct || a.type == VType::kNull);
	if (ImGui::Button("Add entry")) {
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
		markChanged("Added an array element (edit its value above)");
	}
	ImGui::EndDisabled();
}

// ==========================================================================================
// arrays analysis (ReSaver: Analyze Arrays Block)
// ==========================================================================================

void App::drawArraysAnalysis()
{
	ImGui::SetNextWindowSize(ImVec2(820, 600), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Arrays block", &_showArrays)) {
		ImGui::End();
		return;
	}
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
	ImGui::Text("%zu arrays, %zu elements in all, %zu not held by anything", p.arrays.size(), total, _arrOrphans.size());
	if (ImGui::Button("Refresh")) {
		_arraysDirty = true;
	}
	if (!_arrOrphans.empty()) {
		ImGui::SameLine();
		if (ImGui::Button(Fmt("Delete the %zu arrays nothing holds", _arrOrphans.size()).c_str())) {
			const std::set<EID> ids(_arrOrphans.begin(), _arrOrphans.end());
			confirm("Delete unreferenced arrays",
				Fmt("Delete %zu arrays that no instance, reference, array, thread or queued call refers to?", ids.size()), [this, ids] {
					markEdited(Fmt("Deleted %zu unreferenced arrays", ops::DeleteArrays(*_save->papyrus(), ids)));
					_arraysDirty = true;
				});
		}
	}
	if (ImGui::BeginTabBar("arr")) {
		if (ImGui::BeginTabItem("By element type")) {
			if (ImGui::BeginTable("as", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
				ImGui::TableSetupColumn("Type");
				ImGui::TableSetupColumn("Arrays", ImGuiTableColumnFlags_WidthFixed, 90);
				ImGui::TableSetupColumn("Elements", ImGuiTableColumnFlags_WidthFixed, 90);
				ImGui::TableHeadersRow();
				for (const auto& [t, n, e] : _arrStats) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(t.c_str());
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%zu", n);
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%zu", e);
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
					if (ImGui::SmallButton("go")) {
						select(Cat::kArrays, id);
					}
					ImGui::SameLine();
					const auto t = IsRefType(a->type) ? p.str(a->refType) + "[]" : std::string(VTypeName(a->type));
					ImGui::Text("%016" PRIX64 "  %s%s", id, t.c_str(), showSize ? Fmt("  %zu elements", a->vars.size()).c_str() : "");
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
		idList("Largest arrays", big, true);
		idList("Held by nothing", _arrOrphans, true);
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
	ImGui::SetNextWindowSize(ImVec2(820, 420), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Log", &_showLog)) {
		ImGui::End();
		return;
	}
	if (ImGui::Button("Copy")) {
		std::string all;
		for (const auto& l : _log) {
			all += l + "\n";
		}
		ImGui::SetClipboardText(all.c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button("Clear")) {
		_log.clear();
	}
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

void App::drawSettings()
{
	ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Appearing);
	if (!ImGui::Begin("Settings", &_showSettings, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return;
	}
	bool ch = false;
	ImGui::SeparatorText("Game");
	{
		int g = _vr ? 1 : 0;
		const char* games[] = { "Skyrim Special Edition / Anniversary Edition", "Skyrim VR" };
		ImGui::SetNextItemWidth(380);
		if (ImGui::Combo("##game", &g, games, 2)) {
			setGame(g == 1);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(switches by itself to the game of the save you open)");
	}
	ImGui::SeparatorText("Script sources and plugins");
	ImGui::TextWrapped("Mods folder: %s", _sourcesPath.empty() ? "(not set)" : _sourcesPath.c_str());
	ImGui::TextDisabled("Your Mod Organizer \"mods\" folder, Vortex's staging folder (or the game folder: Vortex is found from it),\n"
						"or the game's Data folder. Used for script sources, the script viewer, plugin names and Remove mod.");
	if (ImGui::Button("Choose mods folder...")) {
		if (platform::HasNativeDialogs()) {
			if (auto p = platform::PickFolderDialog("Pick your Mod Organizer \"mods\" folder (or the game's Data folder)")) {
				scanSources(*p);
			}
		} else {
			std::snprintf(_scanPathBuf, sizeof(_scanPathBuf), "%s", _sourcesPath.c_str());
			_scanPathPopup = true;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Use a ModOrganizer.ini...") && platform::HasNativeDialogs()) {
		if (auto p = platform::OpenFileDialog({}, "ini", "Pick ModOrganizer.ini")) {
			if (auto m = ReadMo2Ini(*p)) {
				scanSources(m->modsDir);
			} else {
				_error = "Could not read " + platform::ToUtf8(*p);
			}
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Detect automatically")) {
		autoDetectPaths(true);
	}
	if (!_detectNote.empty()) {
		ImGui::TextDisabled("%s", _detectNote.c_str());
	}
	ImGui::TextWrapped("Game folder: %s", gameFolder().empty() ? "(not found)" : platform::ToUtf8(gameFolder()).c_str());
	ImGui::SameLine();
	if (ImGui::SmallButton("Change...##game") && platform::HasNativeDialogs()) {
		if (auto p = platform::PickFolderDialog(_vr ? "Pick the Skyrim VR folder (the one with SkyrimVR.exe)" : "Pick the Skyrim Special Edition folder (the one with SkyrimSE.exe)")) {
			_gamePath = platform::ToUtf8(*p);
			ch = true;
		}
	}
	{
		const auto comp = compilerPath();
		std::error_code ec;
		ImGui::TextDisabled("Papyrus compiler: %s", !comp.empty() && std::filesystem::exists(comp, ec) ? platform::ToUtf8(comp).c_str()
																										: "not found (it comes with the Creation Kit)");
	}
	{
		auto& custom = savesDirSetting();
		ImGui::TextWrapped("Saves folder: %s", platform::ToUtf8(savesFolder()).c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Change...##saves") && platform::HasNativeDialogs()) {
			if (auto p = platform::PickFolderDialog("Pick the folder with your saves (File > Open starts there)")) {
				custom = platform::ToUtf8(*p);
				ch = true;
				if (_watch) {
					setWatching(true);
				}
			}
		}
		if (!custom.empty()) {
			ImGui::SameLine();
			if (ImGui::SmallButton("Find it by itself##saves")) {
				custom.clear();
				ch = true;
				if (_watch) {
					setWatching(true);
				}
			}
		}
		ImGui::TextDisabled(custom.empty() ? "Found by itself: the folder you last opened a save from, else the one with your newest save\n"
											 "(Documents\\My Games, sLocalSavePath in Skyrim.ini, Mod Organizer profile saves)."
										   : "Chosen by you: File > Open starts here and \"Watch saves folder\" watches it.");
	}
	// Mod Organizer instances installed for this user (%LOCALAPPDATA%\ModOrganizer\<instance>)
	if (const char* la = std::getenv("LOCALAPPDATA")) {
		std::error_code ec;
		const auto      root = platform::FromUtf8(la) / "ModOrganizer";
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
				if (ImGui::SmallButton("Use")) {
					scanSources(m->modsDir);
				}
				ImGui::SameLine();
				ImGui::TextDisabled("Mod Organizer \"%s\": %s", platform::ToUtf8(e.path().filename()).c_str(), mods.c_str());
				ImGui::PopID();
			}
		}
	}
	if (!_vortex && !_mo2 && !gameFolder().empty()) {
		std::error_code ec;
		if (std::filesystem::exists(gameFolder() / "Data" / "vortex.deployment.json", ec)) {
			TextColored(theme::Warn(), "The game's Data folder has Vortex's vortex.deployment.json, but it could not be read: mods are credited to the Data folder.");
		}
	}
	if (_vortex && !_mo2) {
		ImGui::TextWrapped("Vortex: staging folder %s, %zu mods deployed to %s (%s)", platform::ToUtf8(_vortex->stagingPath).c_str(), _vortex->mods.size(),
			platform::ToUtf8(_vortex->dataPath).c_str(), _vortex->method.empty() ? "?" : _vortex->method.c_str());
	}
	if (_mo2) {
		ImGui::TextWrapped("Mod Organizer: %s", platform::ToUtf8(_mo2->ini).c_str());
		if (ImGui::BeginCombo("MO2 profile", _profile.c_str())) {
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
	if (!_sourcesPath.empty() && ImGui::Button("Rescan mods folder")) {
		scanSources(platform::FromUtf8(_sourcesPath));
	}
	ch |= ImGui::Checkbox("Read plugin names automatically", &_autoNames);
	ImGui::SeparatorText("Language of game text");
	{
		struct Cp
		{
			unsigned    cp;
			const char* name;
		};
		static const Cp cps[] = { { 0, "Automatic" }, { 65001, "UTF-8" }, { 1250, "Central European (Polish, Czech, Hungarian...) 1250" },
			{ 1251, "Cyrillic (Russian, Ukrainian...) 1251" }, { 1252, "Western European 1252" }, { 1253, "Greek 1253" }, { 1254, "Turkish 1254" },
			{ 1257, "Baltic 1257" }, { 1258, "Vietnamese 1258" }, { 874, "Thai 874" }, { 932, "Japanese 932" }, { 936, "Chinese Simplified 936" },
			{ 949, "Korean 949" }, { 950, "Chinese Traditional 950" } };
		std::string current = "Automatic";
		for (const auto& c : cps) {
			if (c.cp == text::CodePage()) {
				current = c.name;
			}
		}
		if (text::CodePage() == 0) {
			current = Fmt("Automatic (this system: %u)", text::EffectiveCodePage());
		}
		ImGui::SetNextItemWidth(420);
		if (ImGui::BeginCombo("Text encoding", current.c_str())) {
			for (const auto& c : cps) {
				if (ImGui::Selectable(c.name, c.cp == text::CodePage())) {
					text::SetCodePage(c.cp);
					_rowsDirty = true;
					ch = true;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("Names and places in saves are shown in this encoding when they are not UTF-8.\n"
							"Japanese, Chinese, Korean and Thai characters need a restart after changing it (fonts).");
	}
	ImGui::SeparatorText("Saving");
	const char* comp[] = { "Keep the save's compression", "LZ4 (game default)", "zlib", "Uncompressed" };
	int         c = _saveCompression + 1;
	if (ImGui::Combo("Compression when writing", &c, comp, 4)) {
		_saveCompression = c - 1;
		ch = true;
	}
	ImGui::SeparatorText("Live game (ReSaverHelper.dll)");
	ImGui::TextDisabled("With the optional ReSaverHelper.dll in the game, the Quests page can complete quests, set stages and\n"
						"refresh the journal in the running game.");
	if (ImGui::Checkbox("Connect to the running game", &_liveEnabled)) {
		ch = true;
	}
	{
		const char* modes[] = { "Top-left notification", "Message box", "No message" };
		ImGui::SetNextItemWidth(260);
		if (ImGui::Combo("How the game confirms what it did", &_liveNotify, modes, 3)) {
			ch = true;
		}
	}
	drawRefreshJournal(true);
	drawLiveStatus();
	ImGui::SeparatorText("Other");
	bool watch = _watch;
	{
		int k = static_cast<int>(theme::Current());
		const char* names[] = { "Light", "Grey", "Dark" };
		ImGui::SetNextItemWidth(160);
		if (ImGui::Combo("Theme", &k, names, 3)) {
			setTheme(k);
		}
	}
	if (ImGui::Checkbox("Watch saves folder (open new saves automatically)", &watch)) {
		setWatching(watch);
	}
	const char* sorts[] = { "Save order", "Alphabetical", "Size", "Mass" };
	if (ImGui::Combo("List sorting", &_sortMode, sorts, 4)) {
		_rowsDirty = true;
		ch = true;
	}
	ImGui::TextDisabled("Settings are kept in %s", platform::ToUtf8(platform::SettingsFile()).c_str());
	if (ch) {
		saveSettings();
	}
	ImGui::End();
}

void App::drawSupport()
{
	ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
	if (!ImGui::Begin("Support", &_showSupport, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return;
	}
	ImGui::TextWrapped("ReSaver++ is a C++ port of ReSaver from FallrimTools by Mark Fairchild. These are his support links, "
					   "as listed in ReSaver's own Support dialog:");
	ImGui::Spacing();
	const char* links[][2] = { { "Patreon", "https://www.patreon.com/user?u=18285974" }, { "PayPal", "https://www.paypal.me/MarkDFSoftware" },
		{ "Nexus Mods", "https://www.nexusmods.com/users/723902" } };
	for (const auto& l : links) {
		if (ImGui::Button(l[0], ImVec2(120, 0))) {
			platform::OpenWithDefaultApp(platform::FromUtf8(l[1]));
		}
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
	} else if (Lower(platform::ToUtf8(a_path.filename())) == "modorganizer.ini") {
		// ReSaver: dropping ModOrganizer.ini selects that MO2 instance
		if (auto m = ReadMo2Ini(a_path)) {
			scanSources(m->modsDir);
		} else {
			_error = "Could not read " + platform::ToUtf8(a_path);
		}
	} else {
		open(a_path);
	}
}
