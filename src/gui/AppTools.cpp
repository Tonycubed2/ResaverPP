// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// App features: plugin names, form lookup, decoded change forms, Reset Havok / Purify
// FormLists, batch clean, compare, reports and the save-folder watcher.
#include "App.h"
#include "Watchdog.h"
#include "core/Text.h"
#include <mutex>
#include <set>

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <cstdlib>

#include <thread>

#include <chrono>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <unordered_map>

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
}

App::~App()
{
	_ai.clear();  // stops the chat's thread before the rest of the app goes
	stopPathJob();
	stopBackgroundJobs();  // (the watch, open and analysis workers own their data and end on their own)
	if (_dbProgress) {
		_dbProgress->cancel = true;
	}
	if (_scanProgress) {
		_scanProgress->cancel = true;
	}
	_historyCancel = true;
	if (_mergeProgress) {
		_mergeProgress->cancel = true;
	}
	if (_assetsProgress) {
		_assetsProgress->cancel = true;
	}
	// Background work only reads files. Give it a moment to stop, then leave without it: inside Mod
	// Organizer reading one large archive can take long, and MO2 stays locked while this process exists.
	auto busy = [&] {
		return (_dbThread.joinable() && !_dbDone) || (_scanThread.joinable() && !_scanDone) || scriptJobsBusy() || _historyBusy || _mergeBusy || _assetsBusy;
	};
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (busy() && std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	if (busy()) {
		std::fflush(nullptr);
		std::_Exit(0);
	}
	if (_dbThread.joinable()) {
		_dbThread.join();
	}
	stopScriptJobs();
	if (_scanThread.joinable()) {
		_scanThread.join();
	}
	if (_historyThread.joinable()) {
		_historyThread.join();
	}
	if (_mergeThread.joinable()) {
		_mergeThread.join();
	}
	if (_assetsThread.joinable()) {
		_assetsThread.join();
	}
}

// ==========================================================================================
// plugin names
// ==========================================================================================

std::string App::formText(std::uint32_t a_formID) const
{
	std::string s = Fmt("%08X", a_formID);
	if (_db) {
		const auto d = _db->describe(a_formID);
		if (!d.empty()) {
			s += "  " + d;
		}
	}
	return s;
}

std::string App::gameLanguage() const
{
	if (sf() || _game == 3) {
		// Starfield: sLanguage in StarfieldCustom.ini / StarfieldPrefs.ini (Documents\My Games\Starfield), then the game's own
		// Starfield.ini; its strings files are <plugin>_en.strings, _de, _fr, ... (Starfield - Localization.ba2)
		std::vector<std::filesystem::path> inis;
		if (const auto saves = platform::DefaultSavesFolder(3); !saves.empty()) {
			inis = { saves.parent_path() / "StarfieldCustom.ini", saves.parent_path() / "StarfieldPrefs.ini" };
		}
		inis.push_back(gameFolder() / "Starfield.ini");
		for (const auto& ini : inis) {
			std::ifstream f(ini);
			for (std::string line; std::getline(f, line);) {
				const auto low = Lower(line);
				if (low.rfind("slanguage=", 0) == 0) {
					auto v = Lower(line.substr(10));
					while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) {
						v.pop_back();
					}
					if (!v.empty()) {
						return v;
					}
				}
			}
		}
		return "en";
	}
	// sLanguage from the MO2 profile's Skyrim.ini (Fallout4.ini / Fallout4Custom.ini, then Documents\My Games\Fallout4's),
	// else English ("en" for Fallout 4: its strings files are Fallout4_en.STRINGS)
	std::vector<std::filesystem::path> inis;
	if (_mo2 && !_profile.empty()) {
		const auto prof = _mo2->profilesDir / platform::FromUtf8(_profile);
		if (_game == 2) {
			inis = { prof / "Fallout4Custom.ini", prof / "Fallout4.ini" };
		} else {
			inis = { prof / "skyrim.ini", prof / "Skyrim.ini" };
		}
	}
	if (_game == 2) {
		if (const auto my = platform::DefaultSavesFolder(2).parent_path(); !my.empty()) {
			inis.push_back(my / "Fallout4Custom.ini");
			inis.push_back(my / "Fallout4.ini");
		}
	}
	{
		for (const auto& ini : inis) {
			std::ifstream f(ini);
			for (std::string line; std::getline(f, line);) {
				const auto low = Lower(line);
				if (low.rfind("slanguage=", 0) == 0) {
					auto v = Lower(line.substr(10));
					while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) {
						v.pop_back();
					}
					if (!v.empty()) {
						return v;
					}
				}
			}
		}
	}
	return _game == 2 ? "en" : "english";
}

void App::startNames(bool a_force)
{
	if (_save && sf()) {
		// Starfield: the plugins are read from the game's Data folders and, with Mod Organizer, the profile's mods (no scan)
		const auto dirs = sfDataDirs();
		if (dirs.empty()) {
			return;
		}
		std::string key = platform::ToUtf8(_path) + "|sf";  // (every folder: a Mod Organizer profile switch changes them)
		for (const auto& d : dirs) {
			key += "|" + platform::ToUtf8(d);
		}
		if (!a_force && key == _dbKey) {
			return;
		}
		if (_dbProgress) {
			_dbProgress->cancel = true;
		}
		if (_dbThread.joinable()) {
			watchdog::Step st("waiting for the previous plugin names reading to stop");
			_dbThread.join();
		}
		_dbKey = key;
		_dbDone = false;
		_dbProgress = std::make_unique<PluginDB::Progress>();
		_dbBuilding = std::make_unique<PluginDB>();
		auto in = std::make_shared<PluginDB::Input>();
		if (_wanted && _wantedSave == _save.get() && _wantedGen == _editGen) {
			in->full = _save->plugins.full;  // (the forms the save refers to were worked out when it was opened)
			in->lite = _save->plugins.lite;
			in->wantedShared = _wanted;
		} else {
			watchdog::Step st("PrepareInput (the forms the save refers to)");
			PrepareInput(*_save, *in);
		}
		in->sf = true;
		in->medium = _save->plugins.medium;
		in->sfDataDirs = dirs;
		in->language = gameLanguage();
		_dbThread = std::thread([this, in] {
			_dbBuilding->load(*in, *_dbProgress);
			_dbDone = true;
		});
		return;
	}
	if (!_save || !_sources->scanned()) {
		return;
	}
	const auto key = platform::ToUtf8(_path) + "|" + _sourcesPath + "|" + _profile;
	if (!a_force && key == _dbKey) {
		return;  // already loaded / loading for this save
	}
	if (_dbProgress) {
		_dbProgress->cancel = true;
	}
	if (_dbThread.joinable()) {
		watchdog::Step st("waiting for the previous plugin names reading to stop");
		_dbThread.join();
	}
	_dbKey = key;
	_dbDone = false;
	_dbProgress = std::make_unique<PluginDB::Progress>();
	_dbBuilding = std::make_unique<PluginDB>();
	auto in = std::make_shared<PluginDB::Input>();
	{
		in->shared = _sources;
		if (_wanted && _wantedSave == _save.get() && _wantedGen == _editGen) {
			in->full = _save->plugins.full;  // (the forms the save refers to were worked out when it was opened)
			in->lite = _save->plugins.lite;
			in->wantedShared = _wanted;
		} else {
			watchdog::Step st("PrepareInput (the forms the save refers to)");
			PrepareInput(*_save, *in);
		}
	}
	in->language = gameLanguage();
	in->fo4 = _save->isFO4();
	// the game's language is also the hint for text that is not UTF-8 when Windows' code page says nothing (UTF-8 / Linux)
	if (text::GameLanguage() != in->language) {
		text::SetGameLanguage(in->language);
		_rowsDirty = true;
	}
	_dbThread = std::thread([this, in] {
		_dbBuilding->load(*in, *_dbProgress);
		_dbDone = true;
	});
}

void App::pollNames()
{
	if (_dbThread.joinable() && _dbDone) {
		_dbThread.join();
		if (!_dbProgress->cancel) {
			_db = std::move(_dbBuilding);
			_rowsDirty = true;
			invalidateCheck();
			if (_save && !_save->isSF()) {
				startAnalysis(kAnFx | (_save->isFO4() ? 0u : kAnSkyf));  // these use the names: worked out again on a worker
			}
			_status = Fmt(tr("Read %zu of %zu plugins: %zu named forms%s"), _db->pluginsRead, _db->plugins().size(), _db->size(),
				_db->missing.empty() ? "" : Fmt(tr(" (%zu plugins not found)"), _db->missing.size()).c_str());
			if (_db->starfield()) {
				_status += Fmt(tr("  (%.1f s)"), (_db->sfMsStrings + _db->sfMsPlugins) / 1000.0);
				if (startupLog) {
					startupLog(Fmt("Starfield names: %zu of %zu plugins, %zu forms, %.0f MB, strings %.0f ms (%zu files, %zu archives), plugins %.0f ms, "
								   "ids not placed %zu, overrides of unknown forms %zu",
						_db->pluginsRead, _db->plugins().size(), _db->size(), _db->sfBytes / 1048576.0, _db->sfMsStrings, _db->sfStringsFiles,
						_db->sfArchivesRead, _db->sfMsPlugins, _db->sfUnresolved, _db->sfOrphanOverrides));
				}
			}
		}
		_dbBuilding.reset();
	}
}

bool App::namesLoading() const { return _dbThread.joinable() && !_dbDone; }

// ==========================================================================================
// form lookup (ReSaver: Lookup ID by name / Lookup base object)
// ==========================================================================================

void App::drawLookup()
{
	prepareWindow("Find form", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin(trId("Find form"), &_showLookup, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Find form");
	if (!_db) {
		if (namesLoading()) {
			ImGui::TextUnformatted(tr("Reading your plugins..."));
		} else {
			ImGui::TextWrapped("%s", tr("Plugin names are not loaded. Use \"Find script sources (.pex)...\" in the Mods view once (pick your MO2 mods "
							   "folder); the plugins are then read automatically."));
		}
		ImGui::End();
		return;
	}
	ImGui::TextDisabled("%s", tr("Editor ID, name, or hex form ID. Placed references show their base object after \"->\"."));
	ImGui::SetNextItemWidth(-120);
	bool go = ImGui::InputTextWithHint("##look", fo4() ? tr("e.g. Piper, CompanionPiper, 0001D162") : tr("e.g. Nazeem, WhiterunGuard, 00013BBF"), _lookupBuf,
		sizeof(_lookupBuf), ImGuiInputTextFlags_EnterReturnsTrue);
	tips::Tip(tr("Type part of an editor ID (e.g. WhiterunGuard), a name shown in game (e.g. Nazeem) or a hex form ID, then press Enter. Searches every plugin of this save's load order."));
	ImGui::SameLine();
	go |= ImGui::Button(tr("Search"), ImVec2(110, 0));
	tips::Tip(tr("Search the plugins for forms matching the text. Each result shows whether the save changed it and how many scripts sit on it."));
	if (_lookupAuto) {
		go = true;
		_lookupAuto = false;
	}
	if (go) {
		_lookupResults = _db->search(_lookupBuf, 2000);
		// where each result appears in the save (one pass over the save)
		_lookupInfo.clear();
		std::unordered_map<std::uint32_t, std::size_t> idx;
		for (std::size_t i = 0; i < _lookupResults.size(); ++i) {
			idx.emplace(_lookupResults[i], i);
		}
		_lookupInfo.resize(_lookupResults.size());
		for (const auto& c : _save->changeForms) {
			if (auto it = idx.find(_save->formIDOf(c.refID)); it != idx.end()) {
				_lookupInfo[it->second].cfRaw = c.refID.data;
				_lookupInfo[it->second].hasCf = true;
			}
		}
		for (const auto& i : _save->papyrus()->instances) {
			if (auto it = idx.find(_save->formIDOf(i.refID)); it != idx.end()) {
				auto& li = _lookupInfo[it->second];
				if (!li.scripts++) {
					li.firstInstance = i.id;
				}
			}
		}
	}
	ImGui::Text(tr("%zu result(s)%s"), _lookupResults.size(), _lookupResults.size() >= 2000 ? tr(" (first 2000)") : "");
	{
		// spawned (FF) forms are not in any plugin: say what the save has instead (a leveled actor's rebuilt base NPC
		// points at the actor)
		std::string t = _lookupBuf;
		t.erase(0, t.find_first_not_of(" \t"));
		t.erase(t.find_last_not_of(" \t") + 1);
		if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
			t = t.substr(2);
		}
		if (t.size() == 8 && t.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos && (t[0] == 'f' || t[0] == 'F') &&
			(t[1] == 'f' || t[1] == 'F')) {
			const auto id = static_cast<std::uint32_t>(std::stoul(t, nullptr, 16));
			const auto [hint, actor] = missingFormHint(id);
			if (!hint.empty()) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
				ImGui::TextWrapped("%s", hint.c_str());
				ImGui::PopStyleColor();
			}
			const auto shown = actor ? actor : id;
			for (const auto& c : _save->changeForms) {
				if (_save->formIDOf(c.refID) == shown) {
					if (ImGui::SmallButton(Fmt(tr("Show change form %08X"), shown).c_str())) {
						std::snprintf(_filter, sizeof(_filter), "%08X", shown);
						select(Cat::kChangeForms, c.refID.data);
						_rowsDirty = true;
					}
					tips::Tip(tr("Spawned forms are not in your plugins, only in the save: open its change form in the main window."));
					break;
				}
			}
		}
	}
	if (ImGui::BeginTable("look", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Form ID"), ImGuiTableColumnFlags_WidthFixed, 80);
		ImGui::TableSetupColumn(tr("Type"), ImGuiTableColumnFlags_WidthFixed, 45);
		ImGui::TableSetupColumn(tr("Editor ID / name"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(tr("Defined last by"), ImGuiTableColumnFlags_WidthFixed, 200);
		ImGui::TableSetupColumn(tr("In save"), ImGuiTableColumnFlags_WidthFixed, 140);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120);
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(_lookupResults.size()));
		while (clip.Step()) {
			for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
				const auto  id = _lookupResults[row];
				const auto* f = _db->find(id);
				if (!f) {
					continue;
				}
				ImGui::TableNextRow();
				ImGui::PushID(row);
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%08X", id);
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(_db->type(*f).c_str());
				ImGui::TableSetColumnIndex(2);
				ImGui::TextUnformatted(_db->describe(id).c_str());
				ImGui::TableSetColumnIndex(3);
				ImGui::TextUnformatted(_db->pluginName(*f).c_str());
				const auto& li = _lookupInfo[row];
				ImGui::TableSetColumnIndex(4);
				ImGui::TextUnformatted(((li.hasCf ? std::string(tr("changed")) : std::string("-")) + (li.scripts ? Fmt(tr(", %zu scripts"), li.scripts) : std::string())).c_str());
				ImGui::TableSetColumnIndex(5);
				if (li.hasCf && ImGui::SmallButton(tr("form"))) {
					select(Cat::kChangeForms, li.cfRaw);
				}
				if (li.hasCf) {
					tips::Tip(tr("Open the change form: everything the game saved about this form (position, inventory, AI, flags...)."));
				}
				if (li.scripts) {
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("scripts"))) {
						select(Cat::kInstances, li.firstInstance);
						std::snprintf(_filter, sizeof(_filter), "%08X", id);
						_rowsDirty = true;
					}
					tips::Tip(tr("Show the script instances attached to this form (the Script Instances list, filtered to it)."));
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

// ==========================================================================================
// decoded change forms
// ==========================================================================================

void App::drawCfNode(const cf::Node& n, int a_depth, int a_index)
{
	// the decoded tree is rebuilt every frame: IDs must not depend on where it sits in memory, or an
	// opened section would close again on the next frame (it flickered)
	ImGui::PushID(a_index);
	if (n.kids.empty()) {
		if (n.isRef) {
			if (n.refRaw == 0) {
				ImGui::BulletText(tr("%s: None"), n.label.c_str());
			} else {
				RefID id;
				id.data = n.refRaw;
				const auto* target = _save->findChangeForm(id);
				ImGui::BulletText("%s: %s", n.label.c_str(), n.formID ? formText(n.formID).c_str() : Fmt(tr("RefID %06X"), n.refRaw).c_str());
				tips::Tip(tips::Explain(n.label));
				if (target) {
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("go"))) {
						select(Cat::kChangeForms, n.refRaw);
					}
					tips::Tip(tr("Go to the change form of what this points at."));
				}
			}
		} else {
			ImGui::BulletText("%s: %s", n.label.c_str(), n.value.c_str());
			tips::Tip(tips::Explain(n.label));
		}
	} else if (const bool open = ImGui::TreeNodeEx(n.label.c_str(), a_depth < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
			   tips::Tip(tips::Explain(n.label)), open) {
		for (std::size_t i = 0; i < n.kids.size(); ++i) {
			drawCfNode(n.kids[i], a_depth + 1, static_cast<int>(i));
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

// Fallout 4: Reset Havok for one reference and Purify for one form list (the Skyrim decoder does not read these)
void App::detailChangeFormFO4(const ChangeForm& c)
{
	if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::REFR && (c.changeFlags >> 2) & 1) {
		const auto r = fo4cf::DecodeRefr(*_save, c);
		if (r.hasHavok && r.havokSize) {
			TextColored(theme::Warn(), Fmt(tr("Has saved physics (Havok) state: %u bytes."), r.havokSize));
			if (r.complete && ImGui::SmallButton(tr("Reset this object's Havok"))) {
				const auto refRaw = c.refID.data;
				confirm(tr("Reset Havok"), tr("Clear the saved physics state of this object? It is placed at its saved position when the cell loads."),
					[this, refRaw] {
						RefID id;
						id.data = refRaw;
						if (auto* cfp = _save->findChangeForm(id)) {
							bool       changed = false;
							const auto why = fo4cf::ClearHavok(*_save, *cfp, &changed);
							if (!why.empty()) {
								_error = tr("Could not reset Havok: ") + why;
							} else if (changed) {
								markChanged(tr("Reset Havok of one object"));
							}
						}
					});
			}
			if (r.complete) {
				tips::Tip(tr("Clear this object's saved physics state (the game then keeps no Havok data for it, as for an object that was "
						  "never pushed around); it is placed at its saved position when the cell loads. Fixes objects that fly off, "
						  "shake or sink through the floor."));
			}
		} else if (r.hasHavok) {
			ImGui::TextDisabled("%s", tr("Saved physics (Havok) state: none (0 bytes)."));
		}
	}
	if (c.typeCode() == fo4cf::kFlstType) {
		const auto l = fo4cf::DecodeFormList(*_save, c);
		if (l.complete) {
			const auto nulls = static_cast<std::size_t>(std::count(l.raw.begin(), l.raw.end(), 0u));
			ImGui::Text(tr("Form list: %zu added entries, %zu null"), l.raw.size(), nulls);
			if (nulls) {
				TextColored(theme::Warn(), tr("This list has null entries."));
				if (ImGui::SmallButton(tr("Cleanse this form list"))) {
					const auto rr = cf::PurifyFormLists(*_save, false, c.refID.data);
					markChanged(Fmt(tr("Removed %zu null entries from the form list"), rr.entries));
				}
				tips::Tip(tr("Remove the empty (null) entries from this list. They are left behind when a form it held no longer exists, e.g. "
						  "after uninstalling a mod; scripts walking the list can trip on them."));
			}
		} else {
			ImGui::TextDisabled(tr("Form list: %s"), l.error.c_str());
		}
	}
}

void App::detailChangeFormDecoded(const ChangeForm& c)
{
	const auto fid = _save->formIDOf(c.refID);
	if (_db) {
		const auto d = _db->describe(fid);
		if (!d.empty()) {
			ImGui::TextWrapped("%s", d.c_str());
		}
	}
	cf::TypeLookup types;
	if (_db) {
		types = [this](std::uint32_t id) {
			const auto* f = _db->find(id);
			return f ? _db->type(*f) : std::string();
		};
	}
	const auto d = cf::Decode(*_save, c, types);
	if (d.damaged || (_save->isFO4() && c.length2 && !cf::BodyProblem(c).empty())) {
		// its zlib data does not unpack (cf::TryBody): nothing below can be read
		TextColored(theme::Warn(), tr("Could not be read: zlib data damaged"));
		ImGui::TextWrapped("%s", d.damaged ? d.error.c_str() : cf::BodyProblem(c).c_str());
		ImGui::TextWrapped("%s", tr("Its compressed data does not unpack to the size the save gives. Checks and tools skip it; saving writes it "
		                            "back exactly as it is."));
	}
	if (_save->isFO4()) {
		detailChangeFormFO4(c);
	}

	ImGui::SeparatorText(tr("Change flags"));
	const auto names = cf::FlagNames(c.typeCode(), c.changeFlags);
	if (names.empty()) {
		ImGui::TextDisabled("%s", tr("(none)"));
	}
	for (const auto& n : names) {
		ImGui::BulletText("%s", n.c_str());
		tips::Tip(tips::Explain(n));
		const int bit = std::atoi(n.c_str());
		if (d.complete && cf::CanRemoveFlag(c, bit) && fid != 0x7) {  // not the player: that would reset your own character
			ImGui::SameLine();
			ImGui::PushID(bit);
			if (ImGui::SmallButton(tr("Remove"))) {
				const auto refRaw = c.refID.data;
				const auto effect = cf::RemoveFlagEffect(c.typeCode(), bit);
				confirm(tr("Remove a change flag"),
					Fmt(tr("Remove \"%s\" from this change form?\n\nThe flag is cleared and the data it holds is cut out; everything else in "
						"this change form stays. %s\n\nSave as a new file afterwards and keep the original."),
						n.c_str(), effect.c_str()),
					[this, refRaw, bit, n] {
						RefID id;
						id.data = refRaw;
						if (auto* cfp = _save->findChangeForm(id)) {
							const auto why = cf::RemoveFlag(*_save, *cfp, bit);
							if (why.empty()) {
								markEdited(tr("Removed ") + n + tr(" from ") + formText(_save->formIDOf(id)));
							} else {
								_error = tr("Could not remove ") + n + ": " + why;
							}
						}
					});
			}
			if (const char* what = tips::Explain(n)) {
				tips::Tipf(tr("%s\n\nRemove clears this flag and cuts out the data it holds, so the game uses what the plugin says instead. "
						   "Asks first. Edit only while the game is not using this save; save as a new file and keep the original."), what);
			} else {
				tips::Tip(tr("Remove clears this flag and cuts out the data it holds, so the game uses what the plugin says instead. Asks "
						  "first. Save as a new file and keep the original."));
			}
			ImGui::PopID();
		}
	}
	if (d.baseObject) {
		ImGui::Text(tr("Base object: %s"), formText(d.baseObject).c_str());
	}
	if (d.hasHavok) {
		TextColored(theme::Warn(), Fmt(tr("Has saved physics (Havok) state: %zu bytes."), d.havokSize));
		if (d.complete && ImGui::SmallButton(tr("Reset this object's Havok"))) {
			const auto refRaw = c.refID.data;
			confirm(tr("Reset Havok"), tr("Zero the saved physics state of this object? It falls back to its resting position when loaded."),
				[this, refRaw] {
					RefID id;
					id.data = refRaw;
					if (auto* cf = _save->findChangeForm(id)) {
						const auto dd = cf::Decode(*_save, *cf);
						if (dd.complete && dd.hasHavok) {
							auto body = cf::Body(*cf);
							std::fill(body.begin() + static_cast<std::ptrdiff_t>(dd.havokOffset),
								body.begin() + static_cast<std::ptrdiff_t>(dd.havokOffset + dd.havokSize), std::uint8_t(0));
							cf::SetBody(*cf, body);
							markChanged(tr("Reset Havok of one object"));
						}
					}
				});
		}
		if (d.complete) {
			tips::Tip(tr("Zero this object's saved physics state; it falls back to its resting position when the cell loads. Fixes objects "
					  "that fly off, shake or sink through the floor."));
		}
	}
	if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR && d.complete) {
		actorFixes(c, d);
	}
	if (d.isFormList) {
		std::size_t nulls = 0;
		for (auto v : d.listRaw) {
			nulls += v == 0;
		}
		ImGui::Text(tr("Form list: %zu entries, %zu null"), d.listRaw.size(), nulls);
		if (nulls) {
			TextColored(theme::Warn(), tr("This list has null entries."));
			if (ImGui::SmallButton(tr("Cleanse this form list"))) {
				const auto r = cf::PurifyFormLists(*_save, false, c.refID.data);
				markChanged(Fmt(tr("Removed %zu null entries from the form list"), r.entries));
			}
			tips::Tip(tr("Remove the empty (null) entries from this list. They are left behind when a form it held no longer exists, e.g. after "
					  "uninstalling a mod; scripts walking the list can trip on them."));
		}
	}
	ImGui::SeparatorText(tr("Decoded data"));
	if (d.damaged) {
		TextColored(theme::Warn(), tr("Could not be read: zlib data damaged"));
	} else if (!d.supported) {
		ImGui::TextDisabled("%s", tr("This change form type is not decoded (ReSaver does not decode it either); the raw bytes are below."));
	} else if (d.complete) {
		TextColored(theme::Good(), Fmt(tr("Fully decoded (%zu bytes)."), d.bodySize));
	} else {
		TextColored(theme::Warn(), Fmt(tr("Partly decoded (%zu of %zu bytes): %s"), d.consumed, d.bodySize, d.error.c_str()));
	}
	for (std::size_t i = 0; i < d.root.kids.size(); ++i) {
		drawCfNode(d.root.kids[i], 0, static_cast<int>(i));
	}
}

// Actor (ACHR) fixes: remove single extra data entries, reset the saved AI
void App::actorFixes(const ChangeForm& c, const cf::Decoded& d)
{
	const auto key = Fmt("%06X/%08X/%u/%u/%zu", c.refID.data, c.changeFlags, c.length1, c.length2, d.extras.size());
	if (key != _actorFixKey) {  // try each removable entry on a copy, once per version of this change form
		_actorFixKey = key;
		_actorExtraOk.assign(d.extras.size(), 0);
		for (std::size_t i = 0; i < d.extras.size(); ++i) {
			if (cf::CanRemoveExtraFrom(c, d.extras[i].name)) {
				ChangeForm copy = c;
				_actorExtraOk[i] = cf::RemoveExtra(*_save, copy, i).empty() ? 1 : 0;
			}
		}
	}
	const auto refRaw = c.refID.data;
	const auto who = formText(_save->formIDOf(c.refID));
	if (!d.extras.empty()) {
		ImGui::SeparatorText(tr("Extra data"));
		for (std::size_t i = 0; i < d.extras.size(); ++i) {
			const auto& name = d.extras[i].name;
			ImGui::PushID(static_cast<int>(i) + 1000);
			ImGui::BulletText("%s", name.c_str());
			tips::Tip(tips::Explain(name));
			if (i < _actorExtraOk.size() && _actorExtraOk[i]) {
				ImGui::SameLine();
				if (ImGui::SmallButton(tr("Remove"))) {
					const auto effect = cf::RemoveExtraEffect(name);
					const bool player = refRaw == 0x400014;
					confirm(tr("Remove extra data"),
						Fmt(tr("Remove \"%s\" from %s?\n\n%s\n\nEverything else in this change form stays.%s Save as a new file "
							"afterwards and keep the original."),
							name.c_str(), player ? tr("YOUR CHARACTER") : tr("this actor"), effect.c_str(),
							player ? tr("\n\nThis is your own character: test the new save before playing on with it.") : ""),
						[this, refRaw, i, name, who] {
							RefID id;
							id.data = refRaw;
							if (auto* cfp = _save->findChangeForm(id)) {
								const auto why = cf::RemoveExtra(*_save, *cfp, i);
								if (why.empty()) {
									markEdited(tr("Removed ") + name + tr(" from ") + who);
								} else {
									_error = tr("Could not remove ") + name + ": " + why;
								}
							}
						});
				}
				{
					const char* what = tips::Explain(name);
					const auto  effect = cf::RemoveExtraEffect(name);
					tips::Tipf(tr("%s%s%s\n\nRemove cuts only this entry out of the actor's saved data (tested on a copy first). Asks first; "
							   "save as a new file and keep the original."),
						what ? what : "", what ? "\n\n" : "", effect.c_str());
				}
			}
			ImGui::PopID();
		}
	}
	if (cf::CanResetAI(c, d)) {
		ImGui::SeparatorText(tr("AI"));
		ImGui::TextWrapped(tr("Saved AI: process level %d%s%s."), d.processLevel,
			d.movement.second > d.movement.first + 1 ? tr(", movement controller") : "",
			d.combat.second > d.combat.first + 1 ? tr(", combat controller") : "");
		if (ImGui::SmallButton(tr("Reset this actor's AI"))) {
			confirm(tr("Reset AI"),
				tr("Reset this actor's saved AI?\n\nIts AI process (current package, procedure, targets, equipped state, dialogue and "
				"knowledge), movement and combat state are dropped and the process level set to -1, the state the game saves for "
				"actors it has no AI loaded for. The game builds fresh AI when the actor loads. Its stats, spells, inventory, "
				"factions and position stay.\n\nUseful for an NPC stuck in combat, stuck in a package or frozen in place.\n\nSave as "
				"a new file afterwards and keep the original."),
				[this, refRaw, who] {
					RefID id;
					id.data = refRaw;
					if (auto* cfp = _save->findChangeForm(id)) {
						const auto why = cf::ResetAI(*_save, *cfp);
						if (why.empty()) {
							markEdited(tr("Reset the AI of ") + who);
						} else {
							_error = tr("Could not reset the AI: ") + why;
						}
					}
				});
		}
		tips::Tip(tr("Drop this actor's saved AI (package, procedure, targets, movement and combat state) so the game builds it fresh when "
				  "the actor loads. Fixes NPCs stuck in combat, in a package or frozen in place. Stats, inventory and position stay."));
	}
}

// ==========================================================================================
// Clean: Reset Havok, Purify FormLists, Batch Clean
// ==========================================================================================

void App::resetHavok()
{
	const auto dry = cf::ResetHavok(*_save, {}, true);
	if (!dry.reset) {
		_status = tr("No object reference with saved Havok data could be decoded; nothing to reset.");
		return;
	}
	confirm(tr("Reset Havok"),
		_save->isFO4() || _save->isSF()
			? Fmt(tr("Clear the saved physics (Havok) state of %zu object references?\n\nThe game then keeps no Havok data for them, as for "
				  "objects that were never pushed around; they are placed at their saved positions when their cell loads. %zu reference(s) "
				  "whose data could not be fully read are left alone. This helps with physics explosions and flying objects on load."),
				  dry.reset, dry.failed)
			: Fmt(tr("Zero the saved physics (Havok) state of %zu object references?\n\n%zu reference(s) whose data could not be fully decoded "
				  "are left alone (same rule as ReSaver). Objects fall back to their resting positions when the save loads; this helps with "
				  "physics explosions and flying objects on load."),
				  dry.reset, dry.failed),
		[this] {
			cf::TypeLookup types;
			if (_db) {
				types = [this](std::uint32_t id) {
					const auto* f = _db->find(id);
					return f ? _db->type(*f) : std::string();
				};
			}
			const auto r = cf::ResetHavok(*_save, types);
			markEdited(Fmt(tr("Reset Havok of %zu objects (%zu left alone)"), r.reset, r.failed));
		});
}

void App::purifyFormLists()
{
	const auto dry = cf::PurifyFormLists(*_save, true);
	if (!dry.entries) {
		_status = Fmt(tr("No form list has null entries%s."), dry.failed ? Fmt(tr(" (%zu could not be decoded)"), dry.failed).c_str() : "");
		return;
	}
	confirm(tr("Purify FormLists"), Fmt(tr("Remove %zu null entries from %zu form list(s)?"), dry.entries, dry.forms), [this] {
		const auto r = cf::PurifyFormLists(*_save);
		markEdited(Fmt(tr("Removed %zu null entries from %zu form lists"), r.entries, r.forms));
	});
}

void App::drawBatchModal()
{
	if (_batchOpen) {
		ImGui::OpenPopup(trId("Batch Clean"));
		_batchOpen = false;
		_batchResolved = false;
	}
	ImGui::SetNextWindowSize(ImVec2(760, 600), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal(trId("Batch Clean"))) {
		return;
	}
	if (!_batchResolved) {
		ImGui::TextWrapped("%s", tr("One script per line (ReSaver's batch list format). \"Name*\" matches every script starting with Name; "
						   "\"Name @@ reason\" asks first; lines starting with ; or # are ignored."));
		ImGui::InputTextMultiline("##batch", _batchBuf, sizeof(_batchBuf), ImVec2(-1, -ImGui::GetFrameHeightWithSpacing() * 1.5f));
		tips::Tip(tr("One script name per line. Name* matches every script starting with Name; \"Name @@ reason\" asks before purging; lines starting with ; or # are comments."));
		if (ImGui::Button(tr("Find these scripts"), ImVec2(180, 0))) {
			_batchMatches = batch::Resolve(*_save, _batchBuf);
			_batchResolved = true;
		}
		tips::Tip(tr("Look up which of the listed scripts are in this save. Nothing is changed yet: you get a list to review first."));
		ImGui::SameLine();
		if (ImGui::Button(tr("Cancel"), ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		tips::Tip(tr("Close without changing anything."));
	} else {
		ImGui::Text(tr("%zu script(s) in this save match. Untick any you want to keep:"), _batchMatches.size());
		ImGui::BeginChild("bm", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), ImGuiChildFlags_Borders);
		for (auto& m : _batchMatches) {
			ImGui::PushID(&m);
			ImGui::Checkbox(Fmt(tr("%s   (%zu instances, %zu threads)"), m.script.c_str(), m.instances, m.threads).c_str(), &m.selected);
			tips::Tip(tr("Ticked scripts are purged: their running threads, queued calls and instances are deleted. Untick one to keep it."));
			if (!m.prompt.empty()) {
				ImGui::SameLine();
				TextColored(theme::Warn(), "@@ " + m.prompt);
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
		const auto n = std::count_if(_batchMatches.begin(), _batchMatches.end(), [](const auto& m) { return m.selected; });
		ImGui::BeginDisabled(n == 0);
		if (ImGui::Button(Fmt(tr("Purge %zu script(s)"), static_cast<std::size_t>(n)).c_str(), ImVec2(180, 0))) {
			const auto r = batch::Apply(*_save, _batchMatches);
			ImGui::CloseCurrentPopup();
			markEdited(Fmt(tr("Batch clean: %zu scripts purged (%zu threads, %zu stacks, %zu queued calls, %zu instances)"), r.scripts, r.threads,
						   r.stacks, r.messages, r.instances) +
					   keptNote(r.kept));
		}
		ImGui::EndDisabled();
		tips::Tip(tr("Delete everything the ticked scripts left in the save: threads, suspended stacks, queued calls and instances. Only for "
				  "mods you uninstalled; save as a new file and keep the original."));
		ImGui::SameLine();
		if (ImGui::Button(tr("Back"), ImVec2(120, 0))) {
			_batchResolved = false;
		}
		tips::Tip(tr("Go back and edit the list of scripts."));
		ImGui::SameLine();
		if (ImGui::Button(tr("Cancel"), ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndPopup();
}

// ==========================================================================================
// Compare To
// ==========================================================================================

void App::compareWith(const std::filesystem::path& a_other)
{
	try {
		auto other = SaveFile::load(a_other);
		_compareLines = compare::Compare(*_save, *other).text();
		_compareTitle = platform::ToUtf8(_path.filename()) + "  ->  " + platform::ToUtf8(a_other.filename());
		_showCompare = true;
	} catch (const std::exception& e) {
		_error = tr("Could not read ") + platform::ToUtf8(a_other.filename()) + ":\n\n" + e.what();
	}
}

void App::drawCompare()
{
	prepareWindow("Compare saves", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin(trId("Compare saves"), &_showCompare, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Compare saves");
	ImGui::TextUnformatted(("A = " + _compareTitle).c_str());
	if (ImGui::Button(tr("Copy to clipboard"))) {
		std::string all;
		for (const auto& l : _compareLines) {
			all += l + "\n";
		}
		ImGui::SetClipboardText(all.c_str());
	}
	tips::Tip(tr("Copy the whole comparison as text, e.g. to paste into a bug report or forum post."));
	ImGui::Separator();
	ImGui::BeginChild("cmp");
	for (const auto& l : _compareLines) {
		if (!l.empty() && l.back() == ':') {
			ImGui::Spacing();
			TextColored(theme::Warn(), l);
		} else {
			ImGui::TextUnformatted(l.c_str());
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

// ==========================================================================================
// reports
// ==========================================================================================

void App::drawReportModal()
{
	if (_reportOpen) {
		ImGui::OpenPopup(trId("Report"));
		_reportOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(640, 0));
	if (!ImGui::BeginPopupModal(trId("Report"), nullptr, ImGuiWindowFlags_NoResize)) {
		return;
	}
	ImGui::TextWrapped("%s", tr("Write a report of this save. Choose what goes in it:"));
	ImGui::Spacing();
	ImGui::Checkbox(tr("Mods and plugins with their scripts and instances"), &_reportOpt.mods);
	tips::Tip(tr("Include every mod with its scripts, instances and running threads."));
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("%s", tr("Every mod, every script on its objects, its instances and running threads (like the Mods view)."));
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Checkbox(tr("Anomalies found"), &_reportOpt.anomalies);
	tips::Tip(tr("Include problems ReSaver++ found: undefined or unattached scripts, stuck threads, MCM jams, null form list entries and more."));
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("%s", tr("Analysis findings, undefined / unattached / looping scripts, terminated threads, MCM jam, form lists with null "
						"entries, the busiest scripts."));
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Separator();
	ImGui::Checkbox(tr("Export the report as a PDF (otherwise a .txt file)"), &_reportOpt.pdf);
	tips::Tip(tr("Ticked: a PDF you can open in any viewer. Unticked: a plain text file, easy to paste into a forum post."));
	if (!_db && _sources->scanned()) {
		ImGui::TextDisabled("%s", namesLoading() ? tr("Plugin names are still being read; the report will show form IDs only.") : "");
	}
	ImGui::Spacing();
	ImGui::BeginDisabled(!_reportOpt.mods && !_reportOpt.anomalies);
	if (ImGui::Button(tr("Write report..."), ImVec2(160, 0))) {
		const char* ext = _reportOpt.pdf ? "pdf" : "txt";
		auto        suggested = _path;
		suggested.replace_filename(_path.stem().u8string() + u8"_report." + std::u8string(reinterpret_cast<const char8_t*>(ext)));
		std::optional<std::filesystem::path> target;
		if (platform::HasNativeDialogs()) {
			target = platform::SaveFileDialog(suggested, ext, tr("Save report as"));
		} else {
			target = suggested;
		}
		if (target) {
			writeReport(*target);
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndDisabled();
	tips::Tip(tr("Choose where to save the report and write it. The save itself is not changed."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Cancel"), ImVec2(120, 0))) {
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void App::writeReport(const std::filesystem::path& a_path)
{
	try {
		ensureMods();
		const auto lines = report::Build(*_save, *_mods, _db.get(), _reportOpt, platform::ToUtf8(_path.filename()));
		auto       ext = Lower(platform::ToUtf8(a_path.extension()));
		if (ext == ".pdf") {
			report::WritePdf(a_path, "ReSaver++ report - " + platform::ToUtf8(_path.filename()), lines);
		} else {
			report::WriteText(a_path, lines);
		}
		_status = Fmt(tr("Report written: %s (%zu lines)"), platform::ToUtf8(a_path.filename()).c_str(), lines.size());
		platform::OpenWithDefaultApp(a_path);
	} catch (const std::exception& e) {
		_error = std::string(tr("Could not write the report:\n\n")) + e.what();
	}
}

// ==========================================================================================
// watch the saves folder (ReSaver: Watch Savefile Directory)
// ==========================================================================================

std::vector<std::filesystem::path> App::watchFolders() const
{
	std::vector<std::filesystem::path> v;
	if (auto d = platform::DefaultSavesFolder(_game); !d.empty()) {
		v.push_back(d);
		// Vortex's profile-specific saves (and similar tools) use sub-folders of Saves
		std::error_code ec;
		for (const auto& e : std::filesystem::directory_iterator(d, ec)) {
			if (e.is_directory(ec)) {
				v.push_back(e.path());
			}
		}
	}
	if (_mo2 && !_profile.empty()) {
		v.push_back(_mo2->profilesDir / platform::FromUtf8(_profile) / "saves");  // MO2 profile-local saves
	}
	for (auto& d : iniSaveFolders()) {
		v.push_back(std::move(d));
	}
	if (const auto& s = savesDirSetting(); !s.empty()) {
		v.insert(v.begin(), platform::FromUtf8(s));  // chosen in Settings
	}
	// each folder once
	std::vector<std::filesystem::path> out;
	for (auto& p : v) {
		const auto key = Lower(platform::ToUtf8(p.lexically_normal()));
		if (std::none_of(out.begin(), out.end(), [&](const std::filesystem::path& o) { return Lower(platform::ToUtf8(o.lexically_normal())) == key; })) {
			out.push_back(std::move(p));
		}
	}
	return out;
}

std::vector<std::filesystem::path> App::iniSaveFolders() const
{
	// sLocalSavePath in Skyrim.ini / SkyrimCustom.ini moves the saves; it is relative to Documents\My Games\<game>
	std::vector<std::filesystem::path> v;
	const auto saves = platform::DefaultSavesFolder(_game);
	if (saves.empty()) {
		return v;
	}
	const auto              myGames = saves.parent_path();
	std::vector<std::filesystem::path> inis = _game == 3   ? std::vector<std::filesystem::path>{ myGames / "StarfieldPrefs.ini", myGames / "StarfieldCustom.ini" }
											  : _game == 2 ? std::vector<std::filesystem::path>{ myGames / "Fallout4.ini", myGames / "Fallout4Custom.ini" }
														 : std::vector<std::filesystem::path>{ myGames / "Skyrim.ini", myGames / "SkyrimCustom.ini", myGames / "SkyrimVR.ini" };
	if (_mo2 && !_profile.empty()) {  // Mod Organizer profile-specific game ini files
		const auto prof = _mo2->profilesDir / platform::FromUtf8(_profile);
		inis.push_back(prof / (_game == 2 ? "Fallout4.ini" : "Skyrim.ini"));
		inis.push_back(prof / (_game == 2 ? "Fallout4Custom.ini" : "SkyrimCustom.ini"));
	}
	std::error_code ec;
	for (const auto& ini : inis) {
		std::ifstream f(ini);
		for (std::string line; std::getline(f, line);) {
			const auto low = Lower(line);
			const auto eq = low.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			auto key = low.substr(0, eq);
			key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c); }), key.end());
			if (key != "slocalsavepath") {
				continue;
			}
			auto val = line.substr(eq + 1);
			while (!val.empty() && (std::isspace(static_cast<unsigned char>(val.back())) || val.back() == '\\' || val.back() == '/')) {
				val.pop_back();
			}
			while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) {
				val.erase(val.begin());
			}
			if (val.empty() || Lower(val).rfind("__mo_", 0) == 0) {  // Mod Organizer's own redirect: its profile saves are added already
				continue;
			}
			auto p = platform::FromUtf8(val);
			if (p.is_relative()) {
				p = myGames / p;
			}
			p = p.lexically_normal();
			if (std::filesystem::is_directory(p, ec)) {
				v.push_back(p);
			}
		}
	}
	return v;
}

// The saves folders are looked at every 1.5 s by a worker thread (WatchJob): listing a folder of hundreds of saves and
// reading each one's time took the window ~10 ms a time under Wine, and much longer while the mods scan used the disk.
// The window only opens what the worker found (pollWatch).
struct App::WatchJob
{
	std::mutex                         m;
	std::vector<std::filesystem::path> folders;  // guarded by m (the window refreshes it)
	std::vector<std::filesystem::path> ready;    // new saves whose size stopped changing, guarded by m
	std::atomic<bool>                  stop{ false };
};

void App::setWatching(bool a_on)
{
	_watch = a_on;
	if (_watchJob) {
		_watchJob->stop = true;
		_watchJob.reset();
	}
	if (a_on) {
		// what is there now is remembered by the worker on its first look: only saves written from now on are opened
		auto job = std::make_shared<WatchJob>();
		job->folders = watchFolders();
		_watchJob = job;
		_watchFoldersNext = ImGui::GetTime() + 15.0;
		std::thread([job] {
			std::map<std::filesystem::path, std::filesystem::file_time_type>     seen;
			std::map<std::filesystem::path, std::pair<std::uintmax_t, double>>  pending;
			std::set<std::filesystem::path>                                     known;  // folders looked at once
			const auto t0 = std::chrono::steady_clock::now();
			auto       secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
			while (!job->stop) {
				std::vector<std::filesystem::path> folders;
				{
					std::lock_guard l(job->m);
					folders = job->folders;
				}
				const double    now = secs();
				std::error_code ec;
				for (const auto& dir : folders) {
					const bool first = known.insert(dir).second;  // a folder seen for the first time: what is in it is old
					for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
						if (!e.is_regular_file(ec) || !IsSaveFile(e.path())) {
							continue;
						}
						const auto t = e.last_write_time(ec);
						auto       it = seen.find(e.path());
						if (it != seen.end() && it->second == t) {
							continue;
						}
						seen[e.path()] = t;
						if (!first) {
							pending[e.path()] = { e.file_size(ec), now };
						}
					}
				}
				// a new save is ready once its size has stopped changing (the game writes .ess then .skse)
				for (auto it = pending.begin(); it != pending.end();) {
					const auto size = std::filesystem::file_size(it->first, ec);
					if (ec) {
						it = pending.erase(it);
						continue;
					}
					if (size != it->second.first) {
						it->second = { size, now };
						++it;
						continue;
					}
					if (now - it->second.second < 2.0) {
						++it;
						continue;
					}
					{
						std::lock_guard l(job->m);
						job->ready.push_back(it->first);
					}
					it = pending.erase(it);
				}
				for (int i = 0; i < 15 && !job->stop; ++i) {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
			}
		}).detach();
		_status = tr("Watching the saves folder: new saves open automatically.");
	}
	saveSettings();
}

void App::stopBackgroundJobs()
{
	if (_watchJob) {
		_watchJob->stop = true;
	}
	stopOpenAndAnalysis();
}

void App::pollWatch()
{
	if (!_watch || !_watchJob) {
		return;
	}
	const double now = ImGui::GetTime();
	if (now >= _watchFoldersNext) {
		// the folders themselves (the saves folder's sub-folders, sLocalSavePath in the game's ini files) change rarely
		watchdog::Step st("the saves folders to watch");
		_watchFoldersNext = now + 15.0;
		auto f = watchFolders();
		std::lock_guard l(_watchJob->m);
		_watchJob->folders = std::move(f);
	}
	std::vector<std::filesystem::path> ready;
	{
		std::lock_guard l(_watchJob->m);
		ready.swap(_watchJob->ready);
	}
	for (const auto& path : ready) {
		if (_dirty) {
			_status = tr("New save ") + platform::ToUtf8(path.filename()) + tr(" appeared; not opened because this one has unsaved edits.");
		} else {
			open(path);
		}
	}
}

// ==========================================================================================
// Player editor: gold, skills, bounties
// ==========================================================================================

void App::drawPlayer()
{
	prepareWindow("Edit player", ImVec2(0.7f, 0.85f));
	if (!ImGui::Begin(trId("Edit player"), &_showPlayer, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Edit player");
	if (!_pvLoaded) {
		_pv = cf::ReadPlayer(*_save);
		_pvLoaded = true;
		_pvGold = _pv.gold;
		for (int i = 0; i < 18; ++i) {
			_pvSkills[i] = _pv.skills[i];
			_pvLegend[i] = _pv.legendary[i];
		}
		_pvBounty.clear();
		for (const auto& b : _pv.bounties) {
			_pvBounty.push_back({ b.violent, b.nonViolent });
		}
		_pvLevel = _pv.level;
		_pvXP = _pv.xp;
		_pvPerks = _pv.perkPoints;
		_pvSouls = 0;
		_pvAVEdit.clear();
		for (const auto& a : _pv.avs) {
			_pvAVEdit[a.id] = a.base;
			if (a.id == 133) {
				_pvSouls = static_cast<int>(a.current());
			}
		}
		_pvItemEdit.clear();
		for (const auto& it : _pv.items) {
			_pvItemEdit[it.item] = it.count;
		}
		_pvFxError.clear();
		_pvFx = cf::ReadPlayerActiveEffects(*_save, _pvFxError);
		_pvFxPick.clear();
		_pvStuck = cf::FindStuckPlayerMovement(*_save);
	}
	if (!_pv.error.empty()) {
		TextColored(theme::Warn(), tr("Cannot edit the player: ") + _pv.error);
		ImGui::End();
		return;
	}
	ImGui::NewLine();
	ImGui::TextWrapped("%s", tr("Changes are made in the loaded save; nothing is written until you use File > Save or Save As. Keep the original "
					   "save and test the new one before playing on with it."));
	auto done = [this](const std::string& a_why, const std::string& a_what) {
		if (a_why.empty()) {
			markChanged(a_what);
		} else {
			_error = tr("Could not change the player: ") + a_why;
		}
		_pvLoaded = false;  // read the values again
	};
	auto matches = [](const std::string& a_text, const char* a_filter) {
		if (!a_filter[0]) {
			return true;
		}
		std::string t = a_text, f = a_filter;
		std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		return t.find(f) != std::string::npos;
	};
	auto setButton = [](bool a_changed, const char* a_label = tr("Set")) {
		ImGui::BeginDisabled(!a_changed);
		const bool b = ImGui::SmallButton(a_label);
		ImGui::EndDisabled();
		return b;
	};

	// ---- repairs: only shown when something is wrong ----
	if (_pvStuck.found) {
		const bool hRep = ImGui::CollapsingHeader(trId("Repairs"), ImGuiTreeNodeFlags_DefaultOpen);
		tips::Tip(tr("Problems found in the player's record, with a button that fixes each."));
		if (hRep) {
			TextColored(theme::Warn(), tr("The player cannot use furniture: chairs, beds and crafting stations do nothing."));
			ImGui::TextWrapped("%s", cf::StuckMovementText(_pvStuck).c_str());
			if (ImGui::Button(tr("Fix: player can use furniture again"))) {
				done(cf::ClearStuckPlayerMovement(*_save), tr("The player can use furniture again"));
			}
			tips::Tip(tr("Switches the player's AI back on and sets the \"walk to\" part of the player's movement back to idle, as a new "
						 "character has them. Nothing else in the save changes. Save the result under a new name and load it."));
			ImGui::Separator();
		}
	}

	// ---- character ----
	const bool hChar = ImGui::CollapsingHeader(trId("Character"), ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip(tr("Gold, character level, level-up progress, unspent perk points and dragon souls."));
	if (hChar) {
		if (ImGui::BeginTable("char", 3, ImGuiTableFlags_SizingFixedFit)) {
			auto row = [&](const char* a_name) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(a_name);
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(150);
			};
			row(tr("Gold"));
			ImGui::InputInt("##gold", &_pvGold, 100, 1000);
			tips::Tip(tr("The gold your character carries. Type a new amount, then press Set."));
			_pvGold = std::max(_pvGold, 0);
			ImGui::TableNextColumn();
			if (setButton(_pvGold != _pv.gold, tr("Set##gold"))) {
				done(cf::SetPlayerGold(*_save, _pvGold), Fmt(tr("Set the player's gold to %d"), _pvGold));
			}
			tips::Tip(tr("Write the new gold amount into the loaded save (nothing is written to disk until File > Save)."));
			if (_pv.hasLevel) {
				row(tr("Level"));
				ImGui::InputInt("##level", &_pvLevel, 1, 5);
				tips::Tip(tr("Your character level. Changing it does not grant the perk points, health, magicka or stamina of the levels skipped."));
				_pvLevel = std::clamp(_pvLevel, 1, 65535);
				ImGui::TableNextColumn();
				if (setButton(_pvLevel != _pv.level, tr("Set##level"))) {
					done(cf::SetPlayerLevel(*_save, _pvLevel), Fmt(tr("Set the player's level to %d"), _pvLevel));
				}
				tips::Tip(tr("Write the new character level into the loaded save. Enemies that scale with you follow the new level."));
				ImGui::SameLine();
				ImGui::TextDisabled("%s", tr("(perk points and stats are not added for the levels skipped)"));
			}
			row(tr("Level-up XP"));
			ImGui::InputFloat("##xp", &_pvXP, 10.0f, 100.0f, "%.0f");
			tips::Tip(tr("Experience toward the next character level (the bar in the skills menu). Reaching the amount on the right lets you level up."));
			_pvXP = std::max(_pvXP, 0.0f);
			ImGui::TableNextColumn();
			if (setButton(_pvXP != _pv.xp, tr("Set##xp"))) {
				done(cf::SetPlayerXP(*_save, _pvXP), Fmt(tr("Set the player's level-up XP to %.0f"), _pvXP));
			}
			tips::Tip(tr("Write the new level-up progress into the loaded save."));
			ImGui::SameLine();
			ImGui::TextDisabled(tr("of %.0f for the next level"), _pv.xpNext);
			row(tr("Perk points"));
			ImGui::InputInt("##perks", &_pvPerks, 1, 5);
			tips::Tip(tr("Perk points you have not spent yet (0 to 255)."));
			_pvPerks = std::clamp(_pvPerks, 0, 255);
			ImGui::TableNextColumn();
			if (setButton(_pvPerks != _pv.perkPoints, tr("Set##perks"))) {
				done(cf::SetPlayerPerkPoints(*_save, _pvPerks), Fmt(tr("Set the player's perk points to %d"), _pvPerks));
			}
			tips::Tip(tr("Write the new number of unspent perk points into the loaded save."));
			const bool hasSouls = std::any_of(_pv.avs.begin(), _pv.avs.end(), [](const auto& a) { return a.id == 133 && a.hasBase; });
			if (hasSouls) {
				int current = 0;
				for (const auto& a : _pv.avs) {
					if (a.id == 133) {
						current = static_cast<int>(a.current());
					}
				}
				row(tr("Dragon souls"));
				ImGui::InputInt("##souls", &_pvSouls, 1, 5);
				tips::Tip(tr("Dragon souls you have not spent yet; each unlocks a shout word."));
				_pvSouls = std::max(_pvSouls, 0);
				ImGui::TableNextColumn();
				if (setButton(_pvSouls != current, tr("Set##souls"))) {
					done(cf::SetPlayerDragonSouls(*_save, _pvSouls), Fmt(tr("Set the player's dragon souls to %d"), _pvSouls));
				}
				tips::Tip(tr("Write the new number of dragon souls into the loaded save."));
			}
			ImGui::EndTable();
		}
	}

	// ---- skills ----
	const bool hSkills = ImGui::CollapsingHeader(trId("Skills"));
	tips::Tip(tr("Your 18 skills: their levels and how often each was made legendary."));
	if (hSkills) {
		ImGui::TextDisabled("%s", tr("Setting a skill starts its progress toward the next level over. Your character level is not changed."));
		if (ImGui::BeginTable("skills", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn(tr("Skill"));
			ImGui::TableSetupColumn(tr("Level"));
			ImGui::TableSetupColumn("");
			ImGui::TableSetupColumn(_pv.hasLegendary ? tr("Legendary") : "");
			ImGui::TableSetupColumn("");
			ImGui::TableHeadersRow();
			for (int i = 0; i < 18; ++i) {
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(cf::kSkillNames[i]);
				ImGui::TableNextColumn();
				ImGui::BeginDisabled(!_pv.hasSkill[i]);
				ImGui::SetNextItemWidth(120);
				ImGui::InputFloat("##lvl", &_pvSkills[i], 1.0f, 10.0f, "%.0f");
				tips::Tipf(tr("Your %s skill level. Raising skills normally also counts toward your character level; setting it here does not."), cf::kSkillNames[i]);
				_pvSkills[i] = std::clamp(_pvSkills[i], 0.0f, 1000.0f);
				ImGui::TableNextColumn();
				if (setButton(_pvSkills[i] != _pv.skills[i])) {
					done(cf::SetPlayerSkill(*_save, i, _pvSkills[i]), Fmt(tr("Set the player's %s to %.0f"), cf::kSkillNames[i], _pvSkills[i]));
				}
				tips::Tip(tr("Write the new skill level into the loaded save. Its progress toward the next skill level starts over."));
				ImGui::EndDisabled();
				if (_pv.hasLegendary) {
					ImGui::TableNextColumn();
					ImGui::SetNextItemWidth(100);
					ImGui::InputInt("##leg", &_pvLegend[i], 1, 1);
					tips::Tip(tr("How many times this skill was made legendary (reset to 15 for its perk points back)."));
					_pvLegend[i] = std::max(_pvLegend[i], 0);
					ImGui::TableNextColumn();
					if (setButton(_pvLegend[i] != _pv.legendary[i], tr("Set##leg"))) {
						done(cf::SetPlayerLegendary(*_save, i, _pvLegend[i]),
							Fmt(tr("Set the player's %s legendary count to %d"), cf::kSkillNames[i], _pvLegend[i]));
					}
					tips::Tip(tr("Write the new legendary count into the loaded save. It only changes the counter (and the stat the game shows), not your perks."));
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// ---- stats and other actor values ----
	const bool hAV = ImGui::CollapsingHeader(trId("Stats and actor values"));
	tips::Tip(tr("Every actor value the save stores for you (carry weight, regen rates, resistances...), with the changes active effects and items make to them. Use Reset for odd values a mod left behind."));
	if (hAV) {
		ImGui::TextWrapped("%s", tr("Base is the saved value; permanent, temporary and damage are the changes on top of it. Reset drops the saved "
						   "value and its permanent and damage changes, so your race and plugins give it again (for odd values left by "
						   "mods). Health, Magicka, Stamina, skills and dragon souls hold your progress, so they can be set but not reset. "
						   "Temporary changes usually come from active effects: clear them only when nothing should be affecting you."));
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##avf", tr("filter (e.g. rate, resist, carry)"), _pvAVFilter, sizeof(_pvAVFilter));
		tips::Tip(tr("Show only actor values whose name contains this text."));
		if (ImGui::BeginTable("avs", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 380))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			for (const char* h : { tr("Actor value"), tr("Base"), "", tr("Permanent"), tr("Temporary"), tr("Damage"), tr("Now") }) {
				ImGui::TableSetupColumn(h);
			}
			ImGui::TableHeadersRow();
			for (const auto& a : _pv.avs) {
				if (a.id >= 6 && a.id <= 23) {
					continue;  // the skills have their own section
				}
				const std::string name = cf::ActorValueName(a.id);
				if (!matches(name, _pvAVFilter)) {
					continue;
				}
				ImGui::PushID(static_cast<int>(a.id) + 5000);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(name.c_str());
				ImGui::TableNextColumn();
				if (a.hasBase) {
					auto& e = _pvAVEdit[a.id];
					ImGui::SetNextItemWidth(110);
					ImGui::InputFloat("##b", &e, 0, 0, "%.2f");
					tips::Tip(tips::Explain(name) ? tips::Explain(name) : tr("The saved base value of this actor value. Type a new value, then press Set."));
					ImGui::TableNextColumn();
					if (setButton(e != a.base)) {
						done(cf::SetPlayerAV(*_save, a.id, e), Fmt(tr("Set the player's %s to %g"), name.c_str(), e));
					}
					tips::Tip(tr("Write the new base value into the loaded save."));
				} else {
					ImGui::TextDisabled("%s", tr("(plugin)"));
					ImGui::TableNextColumn();
				}
				if (cf::CanResetPlayerAV(a.id) && (a.hasBase || a.permanent != 0 || a.damage != 0)) {
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("Reset"))) {
						done(cf::ResetPlayerAV(*_save, a.id), tr("Reset the player's ") + name);
					}
					tips::Tip(tr("Drop the saved value and its permanent and damage changes, so your race, perks and plugins decide it again. For odd values a mod left behind."));
				}
				ImGui::TableNextColumn();
				ImGui::Text("%g", a.permanent);
				if (a.permanent != 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("Clear##p"))) {
						done(cf::ClearPlayerAVModifiers(*_save, a.id, 1), tr("Cleared the permanent change to the player's ") + name);
					}
					tips::Tip(tr("Zero the permanent change (from perks, quest rewards or mods). Effects still active may add theirs back."));
				}
				ImGui::TableNextColumn();
				ImGui::Text("%g", a.temporary);
				if (a.temporary != 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("Clear##t"))) {
						done(cf::ClearPlayerAVModifiers(*_save, a.id, 2), tr("Cleared the temporary change to the player's ") + name);
					}
					tips::Tip(tr("Zero the temporary change (from active effects: worn enchantments, potions, spells). Only when nothing should be affecting you: worn items do not re-add theirs until re-equipped."));
				}
				ImGui::TableNextColumn();
				ImGui::Text("%g", a.damage);
				if (a.damage != 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("Clear##d"))) {
						done(cf::ClearPlayerAVModifiers(*_save, a.id, 4), tr("Cleared the damage to the player's ") + name);
					}
					tips::Tip(tr("Zero the damage (e.g. lost health or magicka): the value is back to full."));
				}
				ImGui::TableNextColumn();
				if (a.hasBase) {
					ImGui::Text("%g", a.current());
				} else {
					ImGui::TextDisabled(tr("plugin %+g"), a.permanent + a.temporary + a.damage);
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// ---- inventory ----
	const bool hInv = ImGui::CollapsingHeader(trId("Inventory"));
	tips::Tip(tr("Items your character carries beyond the starting inventory: change counts or remove items."));
	if (hInv) {
		ImGui::TextWrapped("%s", tr("Counts are what the save adds to the character's starting inventory. Set changes the plain stack (enchanted, "
						   "tempered or worn stacks keep their own); Remove takes every stack of the item. Removing a quest item can "
						   "break its quest."));
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##itf", tr("filter by name or form ID"), _pvItemFilter, sizeof(_pvItemFilter));
		tips::Tip(tr("Show only items whose name or form ID contains this text."));
		if (ImGui::BeginTable("items", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 380))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			for (const char* h : { tr("Item"), tr("Count"), "", "" }) {
				ImGui::TableSetupColumn(h);
			}
			ImGui::TableHeadersRow();
			for (const auto& it : _pv.items) {
				const auto name = formText(it.item);
				if (!matches(name, _pvItemFilter)) {
					continue;
				}
				ImGui::PushID(static_cast<int>(it.item));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(name.c_str());
				if (it.hasExtra) {
					ImGui::SameLine();
					ImGui::TextDisabled("%s", tr("(special)"));
				}
				ImGui::TableNextColumn();
				auto& e = _pvItemEdit[it.item];
				ImGui::SetNextItemWidth(110);
				ImGui::InputInt("##c", &e, 1, 10);
				tips::Tip(tr("How many the save adds to your starting inventory. Type a new count, then press Set."));
				e = std::max(e, 0);
				ImGui::TableNextColumn();
				if (setButton(e != it.count)) {
					done(cf::SetPlayerItemCount(*_save, it.item, e), Fmt(tr("Set the player's %s to %d"), name.c_str(), e));
				}
				tips::Tip(tr("Write the new count of the plain stack into the loaded save. Enchanted, tempered or worn stacks keep their own counts."));
				ImGui::TableNextColumn();
				if (ImGui::SmallButton(tr("Remove"))) {
					const auto item = it.item;
					confirm(tr("Remove an item"), tr("Remove every ") + name + tr(" the player carries?"),
						[this, item, name, done] { done(cf::RemovePlayerItem(*_save, item), tr("Removed ") + name + tr(" from the player")); });
				}
				tips::Tip(tr("Remove every stack of this item from your character (asks first). Removing a quest item can break its quest."));
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// ---- spells, powers, abilities: the ones added in game, by their type in the plugins ----
	{
		struct Group
		{
			const char* title;
			const char* note;
			std::vector<std::size_t> rows;
		};
		Group groups[] = {
			{ tr("Spells added in game"), tr("Spells in your magic menu that you learned or a mod gave you."), {} },
			{ tr("Powers added in game"), tr("Greater and lesser powers in your magic menu."), {} },
			{ tr("Abilities added in game"),
				tr("Constant effects (shown under Active Effects, not as spells). Mods often leave these behind; removing one stops its effect."), {} },
			{ tr("Diseases, addictions and other effects"), tr("Diseases, addictions, poisons and other spell types."), {} },
			{ tr("Not found in your plugins"),
				tr("No plugin of this save defines these (or plugin names are not loaded yet): usually leftovers from a removed mod."), {} },
		};
		auto groupOf = [this](std::uint32_t a_id) -> int {
			const auto* f = _db ? _db->find(a_id) : nullptr;
			if (!f) {
				return 4;
			}
			if (_db->type(*f) != "SPEL") {
				return 3;
			}
			switch (f->spellType) {
			case 0:
				return 0;
			case 2:
			case 3:
				return 1;
			case 4:
				return 2;
			default:
				return 3;
			}
		};
		for (std::size_t i = 0; i < _pv.spells.size(); ++i) {
			groups[groupOf(_pv.spells[i])].rows.push_back(i);
		}
		if (!_db) {
			TextColored(theme::Warn(),
				tr("Plugin names are not loaded, so spells cannot be told apart from powers or shown by name. Load them (Tools > Read plugin "
				"names) to split the list."));
		}
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##spf", tr("filter spells, powers and abilities"), _pvSpellFilter, sizeof(_pvSpellFilter));
		tips::Tip(tr("Show only spells, powers and abilities whose name, editor ID or form ID contains this text."));
		int gi = 0;
		for (auto& g : groups) {
			++gi;
			if (g.rows.empty()) {
				continue;
			}
			ImGui::PushID(gi + 70000);
			const bool hSp = ImGui::CollapsingHeader(Fmt(tr("%s (%zu)"), g.title, g.rows.size()).c_str());
			tips::Tip(g.note);
			if (hSp) {
				ImGui::TextDisabled("%s", g.note);
				if (ImGui::BeginTable("sp", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
						ImVec2(0, std::min(360.0f, 30.0f + 26.0f * static_cast<float>(g.rows.size()))))) {
					ImGui::TableSetupScrollFreeze(0, 1);
					for (const char* h : { "", tr("Name in game"), tr("Editor ID"), tr("Form") }) {
						ImGui::TableSetupColumn(h);
					}
					ImGui::TableHeadersRow();
					for (const auto i : g.rows) {
						const auto  sp = _pv.spells[i];
						const auto* f = _db ? _db->find(sp) : nullptr;
						const auto  inGame = f ? _db->name(*f) : std::string();
						const auto  edid = f ? _db->edid(*f) : std::string();
						const auto  form = Fmt("%08X", sp) + (f ? " (" + _db->pluginName(*f) + ")" : std::string());
						if (!matches(inGame + " " + edid + " " + form, _pvSpellFilter)) {
							continue;
						}
						ImGui::PushID(static_cast<int>(i) + 90000);
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						if (ImGui::SmallButton(tr("Remove"))) {
							const auto label = inGame.empty() ? (edid.empty() ? form : edid) : inGame;
							confirm(tr("Remove from the player"), tr("Remove ") + label + tr(" from the player?"),
								[this, sp, label, done] { done(cf::RemovePlayerSpell(*_save, sp), tr("Removed ") + label + tr(" from the player")); });
						}
						tips::Tip(tr("Take this from your character (asks first). For an ability, its constant effect stops. Spells your race or plugins give you are not listed here and stay."));
						ImGui::TableNextColumn();
						if (inGame.empty()) {
							ImGui::TextDisabled("%s", tr("(no name)"));
						} else {
							ImGui::TextUnformatted(inGame.c_str());
						}
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(edid.c_str());
						ImGui::TableNextColumn();
						ImGui::TextDisabled("%s", form.c_str());
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
			}
			ImGui::PopID();
		}
		if (_pv.spells.empty()) {
			ImGui::TextDisabled("%s", tr("No spells, powers or abilities were added in game."));
		}
	}

	// ---- active magic effects ----
	if (std::getenv("RESAVERPP_TEST_FX")) {  // test: open this section
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	}
	const bool hFx = ImGui::CollapsingHeader(Fmt(tr("Active magic effects (%zu)"), _pvFx.size()).c_str());
	tips::Tip(tr("The magic effects on your character right now (enchantments, abilities, potions, diseases, mod effects). Remove ones stuck on you, e.g. from a ring you took off; the stat they changed is put back."));
	if (hFx) {
		if (ImGui::Button(tr("Open Character effects (worn items, by source, stuck ones)"))) {
			shortcut(Sc::kCharEffects);
		}
		tips::Tip(tr("The same effects grouped by where they come from, with a paper doll of what you wear and the stuck ones at the top."));
		ImGui::TextWrapped("%s", tr("What the game shows under Active Effects, and more (mods' hidden effects). Tick the ones to remove, then press "
						   "Remove: each is taken off your character and the stat it changes is put back as if it had ended (Stats shows "
						   "the result). For an effect that is stuck, e.g. from a ring or armor you no longer wear. An effect from "
						   "something you still wear, an ability your race or a mod gives you, comes back when you re-equip it or a "
						   "mod's script gives it again."));
		if (!_pvFxError.empty()) {
			TextColored(theme::Warn(), tr("Cannot read the active effects: ") + _pvFxError);
		}
		if (!_db) {
			TextColored(theme::Warn(),
				tr("Plugin names are not loaded, so the effects cannot be shown by name and the ones that change a stat cannot be "
				"removed (which stat is in the plugins). Load them (Tools > Read plugin names)."));
		}
		// the effect's MGEF and actor value (-1: not known)
		auto effectOf = [this](const cf::ActiveEffectView& e, std::uint32_t& a_mgef) -> std::int32_t {
			a_mgef = 0;
			if (!_db) {
				return -1;
			}
			if (const auto* fx = _db->effects(e.magicItem); fx && e.effectIndex < fx->size()) {
				a_mgef = (*fx)[e.effectIndex].effect;
				if (const auto* m = _db->magicEffect(a_mgef)) {
					return static_cast<std::int32_t>(m->actorValue);
				}
			}
			return -1;
		};
		auto nameOf = [this](std::uint32_t a_id) {
			const auto* f = _db ? _db->find(a_id) : nullptr;
			if (!f) {
				return Fmt("%08X", a_id);
			}
			const auto n = _db->name(*f);
			return n.empty() ? _db->edid(*f) : n;
		};
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##fxf", tr("filter (e.g. ring, fortify, carry)"), _pvFxFilter, sizeof(_pvFxFilter));
		tips::Tip(tr("Show only effects whose source, effect, editor ID, plugin or stat contains this text."));
		ImGui::SameLine();
		ImGui::Checkbox(tr("Also show switched off ones and those that change no stat"), &_pvFxAll);
		tips::Tip(tr("Effects switched off (their conditions are not met) and effects that change no saved stat (scripts, cloaks, lights...) are hidden unless this is ticked. They can be removed too."));
		ImGui::BeginDisabled(_pvFxPick.empty());
		if (ImGui::Button(Fmt(tr("Remove %zu ticked##fx"), _pvFxPick.size()).c_str())) {
			std::vector<cf::EffectRemoval> rm;
			std::string                    names;
			for (const auto i : _pvFxPick) {
				if (i >= _pvFx.size()) {
					continue;
				}
				cf::EffectRemoval r;
				r.index = i;
				std::uint32_t mgef = 0;
				r.actorValue = effectOf(_pvFx[i], mgef);
				rm.push_back(r);
				if (rm.size() <= 8) {
					names += "\n  " + nameOf(_pvFx[i].magicItem) + (mgef ? " (" + nameOf(mgef) + ")" : std::string());
				}
			}
			if (rm.size() > 8) {
				names += Fmt(tr("\n  ... and %zu more"), rm.size() - 8);
			}
			confirm(tr("Remove active effects"), Fmt(tr("Remove %zu active effect(s) from the player, putting back the stats they change?"), rm.size()) + names,
				[this, rm, done] {
					std::vector<std::string> changes;
					const auto               why = cf::RemovePlayerActiveEffects(*_save, rm, &changes);
					std::string              what = Fmt(tr("Removed %zu active effect(s) from the player"), rm.size());
					for (std::size_t k = 0; k < changes.size() && k < 6; ++k) {
						what += (k ? "; " : ": ") + changes[k];
					}
					done(why, what);
				});
		}
		ImGui::EndDisabled();
		tips::Tip(tr("Take the ticked effects off your character (asks first) and put back the stats they change."));
		ImGui::SameLine();
		if (ImGui::SmallButton(tr("Untick all##fx"))) {
			_pvFxPick.clear();
		}
		if (ImGui::BeginTable("fx", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
				ImVec2(0, 380))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			for (const char* h : { "", tr("From"), tr("Effect"), tr("Strength"), tr("Changes"), tr("Time") }) {
				ImGui::TableSetupColumn(h);
			}
			ImGui::TableHeadersRow();
			int shown = 0;
			for (const auto& e : _pvFx) {
				std::uint32_t mgef = 0;
				const auto    av = effectOf(e, mgef);
				const int     slot = cf::ActiveEffectModifier(e);
				const bool    stat = slot >= 0 && e.magnitude != 0;
				if (!_pvFxAll && !stat && !_pvFxPick.count(e.index)) {
					continue;
				}
				const auto* fi = _db ? _db->find(e.magicItem) : nullptr;
				const auto  from = nameOf(e.magicItem);
				const auto  fromFull = from + (fi ? " " + _db->edid(*fi) + " " + _db->type(*fi) + " " + _db->pluginName(*fi) : std::string());
				const auto  effect = mgef ? nameOf(mgef) : Fmt(tr("effect %u"), e.effectIndex);
				const auto  changes = stat ? Fmt("%s %s", av >= 0 ? cf::ActorValueName(static_cast<std::uint32_t>(av)) : tr("(stat not known)"),
												slot ? tr("(temporary)") : tr("(permanent)"))
										   : std::string(e.inactive() ? tr("switched off") : tr("no saved stat"));
				if (!matches(fromFull + " " + effect + " " + changes, _pvFxFilter)) {
					continue;
				}
				++shown;
				ImGui::PushID(static_cast<int>(e.index) + 120000);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				bool picked = _pvFxPick.count(e.index) != 0;
				const bool blocked = stat && av < 0;
				ImGui::BeginDisabled(blocked);
				if (ImGui::Checkbox("##p", &picked)) {
					if (picked) {
						_pvFxPick.insert(e.index);
					} else {
						_pvFxPick.erase(e.index);
					}
				}
				ImGui::EndDisabled();
				tips::Tip(blocked ? tr("Which stat this effect changes is in the plugins: load the plugin names (Tools > Read plugin names) to remove it.")
								  : tr("Tick to remove this effect with the Remove button above."));
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(from.c_str());
				if (fi) {
					tips::Tip((_db->edid(*fi) + " [" + _db->type(*fi) + "] " + Fmt("%08X", e.magicItem) + tr(" from ") + _db->pluginName(*fi)).c_str());
				}
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(effect.c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%g", e.magnitude);
				ImGui::TableNextColumn();
				if (stat) {
					ImGui::Text("%+g %s", e.magnitude, changes.c_str());
				} else {
					ImGui::TextDisabled("%s", changes.c_str());
				}
				ImGui::TableNextColumn();
				if (e.duration > 0) {
					ImGui::Text(tr("%.0f of %.0f s"), e.elapsed, e.duration);
				} else {
					ImGui::TextDisabled("%s", tr("constant"));
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
			if (!shown) {
				ImGui::TextDisabled("%s", _pvFx.empty() ? tr("No active effects.") : tr("No effects to show (tick the box above to see all)."));
			}
		}
	}

	// ---- bounty ----
	const bool hCrime = ImGui::CollapsingHeader(trId("Bounty and crime"));
	tips::Tip(tr("Your bounty in each hold and the crimes on record with each faction. Clear a bounty or wipe the record."));
	if (hCrime) {
		ImGui::Checkbox(tr("Show every faction (not only those with a bounty or crimes)"), &_pvAllFactions);
		tips::Tip(tr("Also list factions you have no bounty or crimes with, e.g. to give yourself a bounty."));
		if (ImGui::BeginTable("bounty", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
			for (const char* h : { tr("Faction"), tr("Violent"), tr("Non-violent"), "", tr("Crime totals") }) {
				ImGui::TableSetupColumn(h);
			}
			ImGui::TableHeadersRow();
			int shown = 0;
			for (std::size_t i = 0; i < _pv.bounties.size() && i < _pvBounty.size(); ++i) {
				const auto& b = _pv.bounties[i];
				const bool  any = b.violent || b.nonViolent || b.violentTotal || b.nonViolentTotal;
				if (!_pvAllFactions && !any) {
					continue;
				}
				++shown;
				ImGui::PushID(static_cast<int>(i));
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(formText(b.faction).c_str());
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(110);
				ImGui::InputInt("##v", &_pvBounty[i][0], 100, 1000);
				tips::Tip(tr("Bounty for violent crimes (assault, murder). Guards attack on sight above 1000."));
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(110);
				ImGui::InputInt("##n", &_pvBounty[i][1], 100, 1000);
				tips::Tip(tr("Bounty for non-violent crimes (theft, trespassing, pickpocketing)."));
				_pvBounty[i][0] = std::max(_pvBounty[i][0], 0);
				_pvBounty[i][1] = std::max(_pvBounty[i][1], 0);
				ImGui::TableNextColumn();
				if (setButton(_pvBounty[i][0] != b.violent || _pvBounty[i][1] != b.nonViolent)) {
					done(cf::SetPlayerBounty(*_save, b.faction, _pvBounty[i][0], _pvBounty[i][1]),
						Fmt(tr("Set the player's bounty with %s to %d"), formText(b.faction).c_str(), _pvBounty[i][0] + _pvBounty[i][1]));
				}
				tips::Tip(tr("Write the new bounty with this faction into the loaded save."));
				ImGui::SameLine();
				ImGui::BeginDisabled(b.violent == 0 && b.nonViolent == 0);
				if (ImGui::SmallButton(tr("Clear"))) {
					done(cf::SetPlayerBounty(*_save, b.faction, 0, 0), tr("Cleared the player's bounty with ") + formText(b.faction));
				}
				ImGui::EndDisabled();
				tips::Tip(tr("Set both bounties with this faction to 0. Your crime record (the totals on the right) stays."));
				ImGui::TableNextColumn();
				ImGui::Text(tr("%d violent, %d non-violent"), b.violentTotal, b.nonViolentTotal);
				if (b.violentTotal || b.nonViolentTotal) {
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("Reset all"))) {
						done(cf::ClearPlayerCrimes(*_save, b.faction), tr("Cleared the player's bounty and crime record with ") + formText(b.faction));
					}
					tips::Tip(tr("Clear the bounty and the crime totals with this faction: a clean record, as if no crime was ever reported."));
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
			if (!shown) {
				ImGui::TextDisabled("%s", tr("No bounties or crimes recorded."));
			}
		}
	}
	ImGui::End();
}
