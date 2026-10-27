// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// App features: plugin names, form lookup, decoded change forms, Reset Havok / Purify
// FormLists, batch clean, compare, reports and the save-folder watcher.
#include "App.h"

#include "Platform.h"
#include "Theme.h"

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
	if (_dbProgress) {
		_dbProgress->cancel = true;
	}
	if (_scanProgress) {
		_scanProgress->cancel = true;
	}
	// Background work only reads files. Give it a moment to stop, then leave without it: inside Mod
	// Organizer reading one large archive can take long, and MO2 stays locked while this process exists.
	auto busy = [&] {
		return (_dbThread.joinable() && !_dbDone) || (_scanThread.joinable() && !_scanDone) || scriptJobsBusy();
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
	// sLanguage from the MO2 profile's Skyrim.ini, else English
	if (_mo2 && !_profile.empty()) {
		for (const char* ini : { "skyrim.ini", "Skyrim.ini" }) {
			std::ifstream f(_mo2->profilesDir / platform::FromUtf8(_profile) / ini);
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
	return "english";
}

void App::startNames(bool a_force)
{
	if (!_save || !_sources.scanned()) {
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
		_dbThread.join();
	}
	_dbKey = key;
	_dbDone = false;
	_dbProgress = std::make_unique<PluginDB::Progress>();
	_dbBuilding = std::make_unique<PluginDB>();
	auto in = std::make_shared<PluginDB::Input>();
	PrepareInput(*_save, *in);
	in->sources = _sources;
	in->language = gameLanguage();
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
			_status = Fmt("Read %zu of %zu plugins: %zu named forms%s", _db->pluginsRead, _db->plugins().size(), _db->size(),
				_db->missing.empty() ? "" : Fmt(" (%zu plugins not found)", _db->missing.size()).c_str());
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
	if (!ImGui::Begin("Find form", &_showLookup, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Find form");
	if (!_db) {
		if (namesLoading()) {
			ImGui::TextUnformatted("Reading your plugins...");
		} else {
			ImGui::TextWrapped("Plugin names are not loaded. Use \"Find script sources (.pex)...\" in the Mods view once (pick your MO2 mods "
							   "folder); the plugins are then read automatically.");
		}
		ImGui::End();
		return;
	}
	ImGui::TextDisabled("Editor ID, name, or hex form ID. Placed references show their base object after \"->\".");
	ImGui::SetNextItemWidth(-120);
	bool go = ImGui::InputTextWithHint("##look", "e.g. Nazeem, WhiterunGuard, 00013BBF", _lookupBuf, sizeof(_lookupBuf), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine();
	go |= ImGui::Button("Search", ImVec2(110, 0));
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
	ImGui::Text("%zu result(s)%s", _lookupResults.size(), _lookupResults.size() >= 2000 ? " (first 2000)" : "");
	if (ImGui::BeginTable("look", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Form ID", ImGuiTableColumnFlags_WidthFixed, 80);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 45);
		ImGui::TableSetupColumn("Editor ID / name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Defined last by", ImGuiTableColumnFlags_WidthFixed, 200);
		ImGui::TableSetupColumn("In save", ImGuiTableColumnFlags_WidthFixed, 140);
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
				ImGui::TextUnformatted(((li.hasCf ? std::string("changed") : std::string("-")) + (li.scripts ? Fmt(", %zu scripts", li.scripts) : std::string())).c_str());
				ImGui::TableSetColumnIndex(5);
				if (li.hasCf && ImGui::SmallButton("form")) {
					select(Cat::kChangeForms, li.cfRaw);
				}
				if (li.scripts) {
					ImGui::SameLine();
					if (ImGui::SmallButton("scripts")) {
						select(Cat::kInstances, li.firstInstance);
						std::snprintf(_filter, sizeof(_filter), "%08X", id);
						_rowsDirty = true;
					}
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
				ImGui::BulletText("%s: None", n.label.c_str());
			} else {
				RefID id;
				id.data = n.refRaw;
				const auto* target = _save->findChangeForm(id);
				ImGui::BulletText("%s: %s", n.label.c_str(), n.formID ? formText(n.formID).c_str() : Fmt("RefID %06X", n.refRaw).c_str());
				if (target) {
					ImGui::SameLine();
					if (ImGui::SmallButton("go")) {
						select(Cat::kChangeForms, n.refRaw);
					}
				}
			}
		} else {
			ImGui::BulletText("%s: %s", n.label.c_str(), n.value.c_str());
		}
	} else if (ImGui::TreeNodeEx(n.label.c_str(), a_depth < 2 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
		for (std::size_t i = 0; i < n.kids.size(); ++i) {
			drawCfNode(n.kids[i], a_depth + 1, static_cast<int>(i));
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
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

	ImGui::SeparatorText("Change flags");
	const auto names = cf::FlagNames(c.typeCode(), c.changeFlags);
	if (names.empty()) {
		ImGui::TextDisabled("(none)");
	}
	for (const auto& n : names) {
		ImGui::BulletText("%s", n.c_str());
		const int bit = std::atoi(n.c_str());
		if (d.complete && cf::CanRemoveFlag(c, bit) && fid != 0x7) {  // not the player: that would reset your own character
			ImGui::SameLine();
			ImGui::PushID(bit);
			if (ImGui::SmallButton("Remove")) {
				const auto refRaw = c.refID.data;
				const auto effect = cf::RemoveFlagEffect(c.typeCode(), bit);
				confirm("Remove a change flag",
					Fmt("Remove \"%s\" from this change form?\n\nThe flag is cleared and the data it holds is cut out; everything else in "
						"this change form stays. %s\n\nSave as a new file afterwards and keep the original.",
						n.c_str(), effect.c_str()),
					[this, refRaw, bit, n] {
						RefID id;
						id.data = refRaw;
						if (auto* cfp = _save->findChangeForm(id)) {
							const auto why = cf::RemoveFlag(*_save, *cfp, bit);
							if (why.empty()) {
								markEdited("Removed " + n + " from " + formText(_save->formIDOf(id)));
							} else {
								_error = "Could not remove " + n + ": " + why;
							}
						}
					});
			}
			ImGui::PopID();
		}
	}
	if (d.baseObject) {
		ImGui::Text("Base object: %s", formText(d.baseObject).c_str());
	}
	if (d.hasHavok) {
		TextColored(theme::Warn(), Fmt("Has saved physics (Havok) state: %zu bytes.", d.havokSize));
		if (d.complete && ImGui::SmallButton("Reset this object's Havok")) {
			const auto refRaw = c.refID.data;
			confirm("Reset Havok", "Zero the saved physics state of this object? It falls back to its resting position when loaded.",
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
							markChanged("Reset Havok of one object");
						}
					}
				});
		}
	}
	if (static_cast<ChangeFormType>(c.typeCode()) == ChangeFormType::ACHR && d.complete && c.refID.data != 0x400014) {
		actorFixes(c, d);
	}
	if (d.isFormList) {
		std::size_t nulls = 0;
		for (auto v : d.listRaw) {
			nulls += v == 0;
		}
		ImGui::Text("Form list: %zu entries, %zu null", d.listRaw.size(), nulls);
		if (nulls) {
			TextColored(theme::Warn(), "This list has null entries.");
			if (ImGui::SmallButton("Cleanse this form list")) {
				const auto r = cf::PurifyFormLists(*_save, false, c.refID.data);
				markChanged(Fmt("Removed %zu null entries from the form list", r.entries));
			}
		}
	}
	ImGui::SeparatorText("Decoded data");
	if (!d.supported) {
		ImGui::TextDisabled("This change form type is not decoded (ReSaver does not decode it either); the raw bytes are below.");
	} else if (d.complete) {
		TextColored(theme::Good(), Fmt("Fully decoded (%zu bytes).", d.bodySize));
	} else {
		TextColored(theme::Warn(), Fmt("Partly decoded (%zu of %zu bytes): %s", d.consumed, d.bodySize, d.error.c_str()));
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
			if (cf::CanRemoveExtra(d.extras[i].name)) {
				ChangeForm copy = c;
				_actorExtraOk[i] = cf::RemoveExtra(*_save, copy, i).empty() ? 1 : 0;
			}
		}
	}
	const auto refRaw = c.refID.data;
	const auto who = formText(_save->formIDOf(c.refID));
	if (!d.extras.empty()) {
		ImGui::SeparatorText("Extra data");
		for (std::size_t i = 0; i < d.extras.size(); ++i) {
			const auto& name = d.extras[i].name;
			ImGui::PushID(static_cast<int>(i) + 1000);
			ImGui::BulletText("%s", name.c_str());
			if (i < _actorExtraOk.size() && _actorExtraOk[i]) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove")) {
					const auto effect = cf::RemoveExtraEffect(name);
					confirm("Remove extra data",
						Fmt("Remove \"%s\" from this actor?\n\n%s\n\nEverything else in this change form stays. Save as a new file "
							"afterwards and keep the original.",
							name.c_str(), effect.c_str()),
						[this, refRaw, i, name, who] {
							RefID id;
							id.data = refRaw;
							if (auto* cfp = _save->findChangeForm(id)) {
								const auto why = cf::RemoveExtra(*_save, *cfp, i);
								if (why.empty()) {
									markEdited("Removed " + name + " from " + who);
								} else {
									_error = "Could not remove " + name + ": " + why;
								}
							}
						});
				}
			}
			ImGui::PopID();
		}
	}
	if (cf::CanResetAI(c, d)) {
		ImGui::SeparatorText("AI");
		ImGui::TextWrapped("Saved AI: process level %d%s%s.", d.processLevel,
			d.movement.second > d.movement.first + 1 ? ", movement controller" : "",
			d.combat.second > d.combat.first + 1 ? ", combat controller" : "");
		if (ImGui::SmallButton("Reset this actor's AI")) {
			confirm("Reset AI",
				"Reset this actor's saved AI?\n\nIts AI process (current package, procedure, targets, equipped state, dialogue and "
				"knowledge), movement and combat state are dropped and the process level set to -1, the state the game saves for "
				"actors it has no AI loaded for. The game builds fresh AI when the actor loads. Its stats, spells, inventory, "
				"factions and position stay.\n\nUseful for an NPC stuck in combat, stuck in a package or frozen in place.\n\nSave as "
				"a new file afterwards and keep the original.",
				[this, refRaw, who] {
					RefID id;
					id.data = refRaw;
					if (auto* cfp = _save->findChangeForm(id)) {
						const auto why = cf::ResetAI(*_save, *cfp);
						if (why.empty()) {
							markEdited("Reset the AI of " + who);
						} else {
							_error = "Could not reset the AI: " + why;
						}
					}
				});
		}
	}
}

// ==========================================================================================
// Clean: Reset Havok, Purify FormLists, Batch Clean
// ==========================================================================================

void App::resetHavok()
{
	const auto dry = cf::ResetHavok(*_save, {}, true);
	if (!dry.reset) {
		_status = "No object reference with saved Havok data could be decoded; nothing to reset.";
		return;
	}
	confirm("Reset Havok",
		Fmt("Zero the saved physics (Havok) state of %zu object references?\n\n%zu reference(s) whose data could not be fully decoded "
			"are left alone (same rule as ReSaver). Objects fall back to their resting positions when the save loads; this helps with "
			"physics explosions and flying objects on load.",
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
			markEdited(Fmt("Reset Havok of %zu objects (%zu left alone)", r.reset, r.failed));
		});
}

void App::purifyFormLists()
{
	const auto dry = cf::PurifyFormLists(*_save, true);
	if (!dry.entries) {
		_status = Fmt("No form list has null entries%s.", dry.failed ? Fmt(" (%zu could not be decoded)", dry.failed).c_str() : "");
		return;
	}
	confirm("Purify FormLists", Fmt("Remove %zu null entries from %zu form list(s)?", dry.entries, dry.forms), [this] {
		const auto r = cf::PurifyFormLists(*_save);
		markEdited(Fmt("Removed %zu null entries from %zu form lists", r.entries, r.forms));
	});
}

void App::drawBatchModal()
{
	if (_batchOpen) {
		ImGui::OpenPopup("Batch Clean");
		_batchOpen = false;
		_batchResolved = false;
	}
	ImGui::SetNextWindowSize(ImVec2(760, 600), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal("Batch Clean")) {
		return;
	}
	if (!_batchResolved) {
		ImGui::TextWrapped("One script per line (ReSaver's batch list format). \"Name*\" matches every script starting with Name; "
						   "\"Name @@ reason\" asks first; lines starting with ; or # are ignored.");
		ImGui::InputTextMultiline("##batch", _batchBuf, sizeof(_batchBuf), ImVec2(-1, -ImGui::GetFrameHeightWithSpacing() * 1.5f));
		if (ImGui::Button("Find these scripts", ImVec2(180, 0))) {
			_batchMatches = batch::Resolve(*_save, _batchBuf);
			_batchResolved = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
	} else {
		ImGui::Text("%zu script(s) in this save match. Untick any you want to keep:", _batchMatches.size());
		ImGui::BeginChild("bm", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), ImGuiChildFlags_Borders);
		for (auto& m : _batchMatches) {
			ImGui::PushID(&m);
			ImGui::Checkbox(Fmt("%s   (%zu instances, %zu threads)", m.script.c_str(), m.instances, m.threads).c_str(), &m.selected);
			if (!m.prompt.empty()) {
				ImGui::SameLine();
				TextColored(theme::Warn(), "@@ " + m.prompt);
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
		const auto n = std::count_if(_batchMatches.begin(), _batchMatches.end(), [](const auto& m) { return m.selected; });
		ImGui::BeginDisabled(n == 0);
		if (ImGui::Button(Fmt("Purge %zu script(s)", static_cast<std::size_t>(n)).c_str(), ImVec2(180, 0))) {
			const auto r = batch::Apply(*_save, _batchMatches);
			ImGui::CloseCurrentPopup();
			markEdited(Fmt("Batch clean: %zu scripts purged (%zu threads, %zu stacks, %zu queued calls, %zu instances)", r.scripts, r.threads,
				r.stacks, r.messages, r.instances));
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Back", ImVec2(120, 0))) {
			_batchResolved = false;
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
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
		_error = "Could not read " + platform::ToUtf8(a_other.filename()) + ":\n\n" + e.what();
	}
}

void App::drawCompare()
{
	prepareWindow("Compare saves", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin("Compare saves", &_showCompare, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Compare saves");
	ImGui::TextUnformatted(("A = " + _compareTitle).c_str());
	if (ImGui::Button("Copy to clipboard")) {
		std::string all;
		for (const auto& l : _compareLines) {
			all += l + "\n";
		}
		ImGui::SetClipboardText(all.c_str());
	}
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
		ImGui::OpenPopup("Report");
		_reportOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(640, 0));
	if (!ImGui::BeginPopupModal("Report", nullptr, ImGuiWindowFlags_NoResize)) {
		return;
	}
	ImGui::TextWrapped("Write a report of this save. Choose what goes in it:");
	ImGui::Spacing();
	ImGui::Checkbox("Mods and plugins with their scripts and instances", &_reportOpt.mods);
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("Every mod, every script on its objects, its instances and running threads (like the Mods view).");
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Checkbox("Anomalies found", &_reportOpt.anomalies);
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("Analysis findings, undefined / unattached / looping scripts, terminated threads, MCM jam, form lists with null "
						"entries, the busiest scripts.");
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Separator();
	ImGui::Checkbox("Export the report as a PDF (otherwise a .txt file)", &_reportOpt.pdf);
	if (!_db && _sources.scanned()) {
		ImGui::TextDisabled(namesLoading() ? "Plugin names are still being read; the report will show form IDs only." : "");
	}
	ImGui::Spacing();
	ImGui::BeginDisabled(!_reportOpt.mods && !_reportOpt.anomalies);
	if (ImGui::Button("Write report...", ImVec2(160, 0))) {
		const char* ext = _reportOpt.pdf ? "pdf" : "txt";
		auto        suggested = _path;
		suggested.replace_filename(_path.stem().u8string() + u8"_report." + std::u8string(reinterpret_cast<const char8_t*>(ext)));
		std::optional<std::filesystem::path> target;
		if (platform::HasNativeDialogs()) {
			target = platform::SaveFileDialog(suggested, ext, "Save report as");
		} else {
			target = suggested;
		}
		if (target) {
			writeReport(*target);
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Cancel", ImVec2(120, 0))) {
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
		_status = Fmt("Report written: %s (%zu lines)", platform::ToUtf8(a_path.filename()).c_str(), lines.size());
		platform::OpenWithDefaultApp(a_path);
	} catch (const std::exception& e) {
		_error = std::string("Could not write the report:\n\n") + e.what();
	}
}

// ==========================================================================================
// watch the saves folder (ReSaver: Watch Savefile Directory)
// ==========================================================================================

std::vector<std::filesystem::path> App::watchFolders() const
{
	std::vector<std::filesystem::path> v;
	if (auto d = platform::DefaultSavesFolder(_vr); !d.empty()) {
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
	const auto saves = platform::DefaultSavesFolder(_vr);
	if (saves.empty()) {
		return v;
	}
	const auto              myGames = saves.parent_path();
	std::vector<std::filesystem::path> inis = { myGames / "Skyrim.ini", myGames / "SkyrimCustom.ini", myGames / "SkyrimVR.ini" };
	if (_mo2 && !_profile.empty()) {  // Mod Organizer profile-specific game ini files
		const auto prof = _mo2->profilesDir / platform::FromUtf8(_profile);
		inis.push_back(prof / "Skyrim.ini");
		inis.push_back(prof / "SkyrimCustom.ini");
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

void App::setWatching(bool a_on)
{
	_watch = a_on;
	_watchPending.clear();
	_watchSeen.clear();
	if (a_on) {
		// remember what is there now; only saves written from now on are opened
		std::error_code ec;
		for (const auto& dir : watchFolders()) {
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				if (e.is_regular_file(ec) && Lower(platform::ToUtf8(e.path().extension())) == ".ess") {
					_watchSeen[e.path()] = e.last_write_time(ec);
				}
			}
		}
		_status = "Watching the saves folder: new saves open automatically.";
	}
	saveSettings();
}

void App::pollWatch()
{
	if (!_watch) {
		return;
	}
	const double now = ImGui::GetTime();
	if (now < _watchNext) {
		return;
	}
	_watchNext = now + 1.5;
	std::error_code ec;
	for (const auto& dir : watchFolders()) {
		for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
			if (!e.is_regular_file(ec) || Lower(platform::ToUtf8(e.path().extension())) != ".ess") {
				continue;
			}
			const auto t = e.last_write_time(ec);
			auto       it = _watchSeen.find(e.path());
			if (it != _watchSeen.end() && it->second == t) {
				continue;
			}
			_watchSeen[e.path()] = t;
			_watchPending[e.path()] = { e.file_size(ec), now };
		}
	}
	// open a new save once its size has stopped changing (the game writes .ess then .skse)
	for (auto it = _watchPending.begin(); it != _watchPending.end();) {
		const auto size = std::filesystem::file_size(it->first, ec);
		if (ec) {
			it = _watchPending.erase(it);
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
		const auto path = it->first;
		it = _watchPending.erase(it);
		if (_dirty) {
			_status = "New save " + platform::ToUtf8(path.filename()) + " appeared; not opened because this one has unsaved edits.";
		} else {
			open(path);
		}
	}
}
