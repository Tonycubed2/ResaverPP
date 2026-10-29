// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// App features: plugin names, form lookup, decoded change forms, Reset Havok / Purify
// FormLists, batch clean, compare, reports and the save-folder watcher.
#include "App.h"

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
	tips::Tip("Type part of an editor ID (e.g. WhiterunGuard), a name shown in game (e.g. Nazeem) or a hex form ID, then press Enter. Searches every plugin of this save's load order.");
	ImGui::SameLine();
	go |= ImGui::Button("Search", ImVec2(110, 0));
	tips::Tip("Search the plugins for forms matching the text. Each result shows whether the save changed it and how many scripts sit on it.");
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
					if (ImGui::SmallButton(Fmt("Show change form %08X", shown).c_str())) {
						std::snprintf(_filter, sizeof(_filter), "%08X", shown);
						select(Cat::kChangeForms, c.refID.data);
						_rowsDirty = true;
					}
					tips::Tip("Spawned forms are not in your plugins, only in the save: open its change form in the main window.");
					break;
				}
			}
		}
	}
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
				if (li.hasCf) {
					tips::Tip("Open the change form: everything the game saved about this form (position, inventory, AI, flags...).");
				}
				if (li.scripts) {
					ImGui::SameLine();
					if (ImGui::SmallButton("scripts")) {
						select(Cat::kInstances, li.firstInstance);
						std::snprintf(_filter, sizeof(_filter), "%08X", id);
						_rowsDirty = true;
					}
					tips::Tip("Show the script instances attached to this form (the Script Instances list, filtered to it).");
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
				tips::Tip(tips::Explain(n.label));
				if (target) {
					ImGui::SameLine();
					if (ImGui::SmallButton("go")) {
						select(Cat::kChangeForms, n.refRaw);
					}
					tips::Tip("Go to the change form of what this points at.");
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
		tips::Tip(tips::Explain(n));
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
			if (const char* what = tips::Explain(n)) {
				tips::Tipf("%s\n\nRemove clears this flag and cuts out the data it holds, so the game uses what the plugin says instead. "
						   "Asks first. Edit only while the game is not using this save; save as a new file and keep the original.", what);
			} else {
				tips::Tip("Remove clears this flag and cuts out the data it holds, so the game uses what the plugin says instead. Asks "
						  "first. Save as a new file and keep the original.");
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
		if (d.complete) {
			tips::Tip("Zero this object's saved physics state; it falls back to its resting position when the cell loads. Fixes objects "
					  "that fly off, shake or sink through the floor.");
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
		ImGui::Text("Form list: %zu entries, %zu null", d.listRaw.size(), nulls);
		if (nulls) {
			TextColored(theme::Warn(), "This list has null entries.");
			if (ImGui::SmallButton("Cleanse this form list")) {
				const auto r = cf::PurifyFormLists(*_save, false, c.refID.data);
				markChanged(Fmt("Removed %zu null entries from the form list", r.entries));
			}
			tips::Tip("Remove the empty (null) entries from this list. They are left behind when a form it held no longer exists, e.g. after "
					  "uninstalling a mod; scripts walking the list can trip on them.");
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
			if (cf::CanRemoveExtraFrom(c, d.extras[i].name)) {
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
			tips::Tip(tips::Explain(name));
			if (i < _actorExtraOk.size() && _actorExtraOk[i]) {
				ImGui::SameLine();
				if (ImGui::SmallButton("Remove")) {
					const auto effect = cf::RemoveExtraEffect(name);
					const bool player = refRaw == 0x400014;
					confirm("Remove extra data",
						Fmt("Remove \"%s\" from %s?\n\n%s\n\nEverything else in this change form stays.%s Save as a new file "
							"afterwards and keep the original.",
							name.c_str(), player ? "YOUR CHARACTER" : "this actor", effect.c_str(),
							player ? "\n\nThis is your own character: test the new save before playing on with it." : ""),
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
				{
					const char* what = tips::Explain(name);
					const auto  effect = cf::RemoveExtraEffect(name);
					tips::Tipf("%s%s%s\n\nRemove cuts only this entry out of the actor's saved data (tested on a copy first). Asks first; "
							   "save as a new file and keep the original.",
						what ? what : "", what ? "\n\n" : "", effect.c_str());
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
		tips::Tip("Drop this actor's saved AI (package, procedure, targets, movement and combat state) so the game builds it fresh when "
				  "the actor loads. Fixes NPCs stuck in combat, in a package or frozen in place. Stats, inventory and position stay.");
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
		tips::Tip("One script name per line. Name* matches every script starting with Name; \"Name @@ reason\" asks before purging; lines starting with ; or # are comments.");
		if (ImGui::Button("Find these scripts", ImVec2(180, 0))) {
			_batchMatches = batch::Resolve(*_save, _batchBuf);
			_batchResolved = true;
		}
		tips::Tip("Look up which of the listed scripts are in this save. Nothing is changed yet: you get a list to review first.");
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			ImGui::CloseCurrentPopup();
		}
		tips::Tip("Close without changing anything.");
	} else {
		ImGui::Text("%zu script(s) in this save match. Untick any you want to keep:", _batchMatches.size());
		ImGui::BeginChild("bm", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.5f), ImGuiChildFlags_Borders);
		for (auto& m : _batchMatches) {
			ImGui::PushID(&m);
			ImGui::Checkbox(Fmt("%s   (%zu instances, %zu threads)", m.script.c_str(), m.instances, m.threads).c_str(), &m.selected);
			tips::Tip("Ticked scripts are purged: their running threads, queued calls and instances are deleted. Untick one to keep it.");
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
		tips::Tip("Delete everything the ticked scripts left in the save: threads, suspended stacks, queued calls and instances. Only for "
				  "mods you uninstalled; save as a new file and keep the original.");
		ImGui::SameLine();
		if (ImGui::Button("Back", ImVec2(120, 0))) {
			_batchResolved = false;
		}
		tips::Tip("Go back and edit the list of scripts.");
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
	tips::Tip("Copy the whole comparison as text, e.g. to paste into a bug report or forum post.");
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
	tips::Tip("Include every mod with its scripts, instances and running threads.");
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("Every mod, every script on its objects, its instances and running threads (like the Mods view).");
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Checkbox("Anomalies found", &_reportOpt.anomalies);
	tips::Tip("Include problems ReSaver++ found: undefined or unattached scripts, stuck threads, MCM jams, null form list entries and more.");
	ImGui::Indent(32);
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("Analysis findings, undefined / unattached / looping scripts, terminated threads, MCM jam, form lists with null "
						"entries, the busiest scripts.");
	ImGui::PopTextWrapPos();
	ImGui::Unindent(32);
	ImGui::Separator();
	ImGui::Checkbox("Export the report as a PDF (otherwise a .txt file)", &_reportOpt.pdf);
	tips::Tip("Ticked: a PDF you can open in any viewer. Unticked: a plain text file, easy to paste into a forum post.");
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
	tips::Tip("Choose where to save the report and write it. The save itself is not changed.");
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

// ==========================================================================================
// Player editor: gold, skills, bounties
// ==========================================================================================

void App::drawPlayer()
{
	prepareWindow("Edit player", ImVec2(0.7f, 0.85f));
	if (!ImGui::Begin("Edit player", &_showPlayer, ImGuiWindowFlags_NoCollapse)) {
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
	}
	if (!_pv.error.empty()) {
		TextColored(theme::Warn(), "Cannot edit the player: " + _pv.error);
		ImGui::End();
		return;
	}
	ImGui::NewLine();
	ImGui::TextWrapped("Changes are made in the loaded save; nothing is written until you use File > Save or Save As. Keep the original "
					   "save and test the new one before playing on with it.");
	auto done = [this](const std::string& a_why, const std::string& a_what) {
		if (a_why.empty()) {
			markChanged(a_what);
		} else {
			_error = "Could not change the player: " + a_why;
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
	auto setButton = [](bool a_changed, const char* a_label = "Set") {
		ImGui::BeginDisabled(!a_changed);
		const bool b = ImGui::SmallButton(a_label);
		ImGui::EndDisabled();
		return b;
	};

	// ---- character ----
	const bool hChar = ImGui::CollapsingHeader("Character", ImGuiTreeNodeFlags_DefaultOpen);
	tips::Tip("Gold, character level, level-up progress, unspent perk points and dragon souls.");
	if (hChar) {
		if (ImGui::BeginTable("char", 3, ImGuiTableFlags_SizingFixedFit)) {
			auto row = [&](const char* a_name) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(a_name);
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(150);
			};
			row("Gold");
			ImGui::InputInt("##gold", &_pvGold, 100, 1000);
			tips::Tip("The gold your character carries. Type a new amount, then press Set.");
			_pvGold = std::max(_pvGold, 0);
			ImGui::TableNextColumn();
			if (setButton(_pvGold != _pv.gold, "Set##gold")) {
				done(cf::SetPlayerGold(*_save, _pvGold), Fmt("Set the player's gold to %d", _pvGold));
			}
			tips::Tip("Write the new gold amount into the loaded save (nothing is written to disk until File > Save).");
			if (_pv.hasLevel) {
				row("Level");
				ImGui::InputInt("##level", &_pvLevel, 1, 5);
				tips::Tip("Your character level. Changing it does not grant the perk points, health, magicka or stamina of the levels skipped.");
				_pvLevel = std::clamp(_pvLevel, 1, 65535);
				ImGui::TableNextColumn();
				if (setButton(_pvLevel != _pv.level, "Set##level")) {
					done(cf::SetPlayerLevel(*_save, _pvLevel), Fmt("Set the player's level to %d", _pvLevel));
				}
				tips::Tip("Write the new character level into the loaded save. Enemies that scale with you follow the new level.");
				ImGui::SameLine();
				ImGui::TextDisabled("(perk points and stats are not added for the levels skipped)");
			}
			row("Level-up XP");
			ImGui::InputFloat("##xp", &_pvXP, 10.0f, 100.0f, "%.0f");
			tips::Tip("Experience toward the next character level (the bar in the skills menu). Reaching the amount on the right lets you level up.");
			_pvXP = std::max(_pvXP, 0.0f);
			ImGui::TableNextColumn();
			if (setButton(_pvXP != _pv.xp, "Set##xp")) {
				done(cf::SetPlayerXP(*_save, _pvXP), Fmt("Set the player's level-up XP to %.0f", _pvXP));
			}
			tips::Tip("Write the new level-up progress into the loaded save.");
			ImGui::SameLine();
			ImGui::TextDisabled("of %.0f for the next level", _pv.xpNext);
			row("Perk points");
			ImGui::InputInt("##perks", &_pvPerks, 1, 5);
			tips::Tip("Perk points you have not spent yet (0 to 255).");
			_pvPerks = std::clamp(_pvPerks, 0, 255);
			ImGui::TableNextColumn();
			if (setButton(_pvPerks != _pv.perkPoints, "Set##perks")) {
				done(cf::SetPlayerPerkPoints(*_save, _pvPerks), Fmt("Set the player's perk points to %d", _pvPerks));
			}
			tips::Tip("Write the new number of unspent perk points into the loaded save.");
			const bool hasSouls = std::any_of(_pv.avs.begin(), _pv.avs.end(), [](const auto& a) { return a.id == 133 && a.hasBase; });
			if (hasSouls) {
				int current = 0;
				for (const auto& a : _pv.avs) {
					if (a.id == 133) {
						current = static_cast<int>(a.current());
					}
				}
				row("Dragon souls");
				ImGui::InputInt("##souls", &_pvSouls, 1, 5);
				tips::Tip("Dragon souls you have not spent yet; each unlocks a shout word.");
				_pvSouls = std::max(_pvSouls, 0);
				ImGui::TableNextColumn();
				if (setButton(_pvSouls != current, "Set##souls")) {
					done(cf::SetPlayerDragonSouls(*_save, _pvSouls), Fmt("Set the player's dragon souls to %d", _pvSouls));
				}
				tips::Tip("Write the new number of dragon souls into the loaded save.");
			}
			ImGui::EndTable();
		}
	}

	// ---- skills ----
	const bool hSkills = ImGui::CollapsingHeader("Skills");
	tips::Tip("Your 18 skills: their levels and how often each was made legendary.");
	if (hSkills) {
		ImGui::TextDisabled("Setting a skill starts its progress toward the next level over. Your character level is not changed.");
		if (ImGui::BeginTable("skills", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
			ImGui::TableSetupColumn("Skill");
			ImGui::TableSetupColumn("Level");
			ImGui::TableSetupColumn("");
			ImGui::TableSetupColumn(_pv.hasLegendary ? "Legendary" : "");
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
				tips::Tipf("Your %s skill level. Raising skills normally also counts toward your character level; setting it here does not.", cf::kSkillNames[i]);
				_pvSkills[i] = std::clamp(_pvSkills[i], 0.0f, 1000.0f);
				ImGui::TableNextColumn();
				if (setButton(_pvSkills[i] != _pv.skills[i])) {
					done(cf::SetPlayerSkill(*_save, i, _pvSkills[i]), Fmt("Set the player's %s to %.0f", cf::kSkillNames[i], _pvSkills[i]));
				}
				tips::Tip("Write the new skill level into the loaded save. Its progress toward the next skill level starts over.");
				ImGui::EndDisabled();
				if (_pv.hasLegendary) {
					ImGui::TableNextColumn();
					ImGui::SetNextItemWidth(100);
					ImGui::InputInt("##leg", &_pvLegend[i], 1, 1);
					tips::Tip("How many times this skill was made legendary (reset to 15 for its perk points back).");
					_pvLegend[i] = std::max(_pvLegend[i], 0);
					ImGui::TableNextColumn();
					if (setButton(_pvLegend[i] != _pv.legendary[i], "Set##leg")) {
						done(cf::SetPlayerLegendary(*_save, i, _pvLegend[i]),
							Fmt("Set the player's %s legendary count to %d", cf::kSkillNames[i], _pvLegend[i]));
					}
					tips::Tip("Write the new legendary count into the loaded save. It only changes the counter (and the stat the game shows), not your perks.");
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// ---- stats and other actor values ----
	const bool hAV = ImGui::CollapsingHeader("Stats and actor values");
	tips::Tip("Every actor value the save stores for you (carry weight, regen rates, resistances...), with the changes active effects and items make to them. Use Reset for odd values a mod left behind.");
	if (hAV) {
		ImGui::TextWrapped("Base is the saved value; permanent, temporary and damage are the changes on top of it. Reset drops the saved "
						   "value and its permanent and damage changes, so your race and plugins give it again (for odd values left by "
						   "mods). Health, Magicka, Stamina, skills and dragon souls hold your progress, so they can be set but not reset. "
						   "Temporary changes usually come from active effects: clear them only when nothing should be affecting you.");
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##avf", "filter (e.g. rate, resist, carry)", _pvAVFilter, sizeof(_pvAVFilter));
		tips::Tip("Show only actor values whose name contains this text.");
		if (ImGui::BeginTable("avs", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 380))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			for (const char* h : { "Actor value", "Base", "", "Permanent", "Temporary", "Damage", "Now" }) {
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
					tips::Tip(tips::Explain(name) ? tips::Explain(name) : "The saved base value of this actor value. Type a new value, then press Set.");
					ImGui::TableNextColumn();
					if (setButton(e != a.base)) {
						done(cf::SetPlayerAV(*_save, a.id, e), Fmt("Set the player's %s to %g", name.c_str(), e));
					}
					tips::Tip("Write the new base value into the loaded save.");
				} else {
					ImGui::TextDisabled("(plugin)");
					ImGui::TableNextColumn();
				}
				if (cf::CanResetPlayerAV(a.id) && (a.hasBase || a.permanent != 0 || a.damage != 0)) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Reset")) {
						done(cf::ResetPlayerAV(*_save, a.id), "Reset the player's " + name);
					}
					tips::Tip("Drop the saved value and its permanent and damage changes, so your race, perks and plugins decide it again. For odd values a mod left behind.");
				}
				ImGui::TableNextColumn();
				ImGui::Text("%g", a.permanent);
				if (a.permanent != 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Clear##p")) {
						done(cf::ClearPlayerAVModifiers(*_save, a.id, 1), "Cleared the permanent change to the player's " + name);
					}
					tips::Tip("Zero the permanent change (from perks, quest rewards or mods). Effects still active may add theirs back.");
				}
				ImGui::TableNextColumn();
				ImGui::Text("%g", a.temporary);
				if (a.temporary != 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Clear##t")) {
						done(cf::ClearPlayerAVModifiers(*_save, a.id, 2), "Cleared the temporary change to the player's " + name);
					}
					tips::Tip("Zero the temporary change (from active effects: worn enchantments, potions, spells). Only when nothing should be affecting you: worn items do not re-add theirs until re-equipped.");
				}
				ImGui::TableNextColumn();
				ImGui::Text("%g", a.damage);
				if (a.damage != 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Clear##d")) {
						done(cf::ClearPlayerAVModifiers(*_save, a.id, 4), "Cleared the damage to the player's " + name);
					}
					tips::Tip("Zero the damage (e.g. lost health or magicka): the value is back to full.");
				}
				ImGui::TableNextColumn();
				if (a.hasBase) {
					ImGui::Text("%g", a.current());
				} else {
					ImGui::TextDisabled("plugin %+g", a.permanent + a.temporary + a.damage);
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}

	// ---- inventory ----
	const bool hInv = ImGui::CollapsingHeader("Inventory");
	tips::Tip("Items your character carries beyond the starting inventory: change counts or remove items.");
	if (hInv) {
		ImGui::TextWrapped("Counts are what the save adds to the character's starting inventory. Set changes the plain stack (enchanted, "
						   "tempered or worn stacks keep their own); Remove takes every stack of the item. Removing a quest item can "
						   "break its quest.");
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##itf", "filter by name or form ID", _pvItemFilter, sizeof(_pvItemFilter));
		tips::Tip("Show only items whose name or form ID contains this text.");
		if (ImGui::BeginTable("items", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 380))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			for (const char* h : { "Item", "Count", "", "" }) {
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
					ImGui::TextDisabled("(special)");
				}
				ImGui::TableNextColumn();
				auto& e = _pvItemEdit[it.item];
				ImGui::SetNextItemWidth(110);
				ImGui::InputInt("##c", &e, 1, 10);
				tips::Tip("How many the save adds to your starting inventory. Type a new count, then press Set.");
				e = std::max(e, 0);
				ImGui::TableNextColumn();
				if (setButton(e != it.count)) {
					done(cf::SetPlayerItemCount(*_save, it.item, e), Fmt("Set the player's %s to %d", name.c_str(), e));
				}
				tips::Tip("Write the new count of the plain stack into the loaded save. Enchanted, tempered or worn stacks keep their own counts.");
				ImGui::TableNextColumn();
				if (ImGui::SmallButton("Remove")) {
					const auto item = it.item;
					confirm("Remove an item", "Remove every " + name + " the player carries?",
						[this, item, name, done] { done(cf::RemovePlayerItem(*_save, item), "Removed " + name + " from the player"); });
				}
				tips::Tip("Remove every stack of this item from your character (asks first). Removing a quest item can break its quest.");
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
			{ "Spells added in game", "Spells in your magic menu that you learned or a mod gave you.", {} },
			{ "Powers added in game", "Greater and lesser powers in your magic menu.", {} },
			{ "Abilities added in game",
				"Constant effects (shown under Active Effects, not as spells). Mods often leave these behind; removing one stops its effect.", {} },
			{ "Diseases, addictions and other effects", "Diseases, addictions, poisons and other spell types.", {} },
			{ "Not found in your plugins",
				"No plugin of this save defines these (or plugin names are not loaded yet): usually leftovers from a removed mod.", {} },
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
				"Plugin names are not loaded, so spells cannot be told apart from powers or shown by name. Load them (Tools > Read plugin "
				"names) to split the list.");
		}
		ImGui::SetNextItemWidth(250);
		ImGui::InputTextWithHint("##spf", "filter spells, powers and abilities", _pvSpellFilter, sizeof(_pvSpellFilter));
		tips::Tip("Show only spells, powers and abilities whose name, editor ID or form ID contains this text.");
		int gi = 0;
		for (auto& g : groups) {
			++gi;
			if (g.rows.empty()) {
				continue;
			}
			ImGui::PushID(gi + 70000);
			const bool hSp = ImGui::CollapsingHeader(Fmt("%s (%zu)", g.title, g.rows.size()).c_str());
			tips::Tip(g.note);
			if (hSp) {
				ImGui::TextDisabled("%s", g.note);
				if (ImGui::BeginTable("sp", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
						ImVec2(0, std::min(360.0f, 30.0f + 26.0f * static_cast<float>(g.rows.size()))))) {
					ImGui::TableSetupScrollFreeze(0, 1);
					for (const char* h : { "", "Name in game", "Editor ID", "Form" }) {
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
						if (ImGui::SmallButton("Remove")) {
							const auto label = inGame.empty() ? (edid.empty() ? form : edid) : inGame;
							confirm("Remove from the player", "Remove " + label + " from the player?",
								[this, sp, label, done] { done(cf::RemovePlayerSpell(*_save, sp), "Removed " + label + " from the player"); });
						}
						tips::Tip("Take this from your character (asks first). For an ability, its constant effect stops. Spells your race or plugins give you are not listed here and stay.");
						ImGui::TableNextColumn();
						if (inGame.empty()) {
							ImGui::TextDisabled("(no name)");
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
			ImGui::TextDisabled("No spells, powers or abilities were added in game.");
		}
	}

	// ---- bounty ----
	const bool hCrime = ImGui::CollapsingHeader("Bounty and crime");
	tips::Tip("Your bounty in each hold and the crimes on record with each faction. Clear a bounty or wipe the record.");
	if (hCrime) {
		ImGui::Checkbox("Show every faction (not only those with a bounty or crimes)", &_pvAllFactions);
		tips::Tip("Also list factions you have no bounty or crimes with, e.g. to give yourself a bounty.");
		if (ImGui::BeginTable("bounty", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
			for (const char* h : { "Faction", "Violent", "Non-violent", "", "Crime totals" }) {
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
				tips::Tip("Bounty for violent crimes (assault, murder). Guards attack on sight above 1000.");
				ImGui::TableNextColumn();
				ImGui::SetNextItemWidth(110);
				ImGui::InputInt("##n", &_pvBounty[i][1], 100, 1000);
				tips::Tip("Bounty for non-violent crimes (theft, trespassing, pickpocketing).");
				_pvBounty[i][0] = std::max(_pvBounty[i][0], 0);
				_pvBounty[i][1] = std::max(_pvBounty[i][1], 0);
				ImGui::TableNextColumn();
				if (setButton(_pvBounty[i][0] != b.violent || _pvBounty[i][1] != b.nonViolent)) {
					done(cf::SetPlayerBounty(*_save, b.faction, _pvBounty[i][0], _pvBounty[i][1]),
						Fmt("Set the player's bounty with %s to %d", formText(b.faction).c_str(), _pvBounty[i][0] + _pvBounty[i][1]));
				}
				tips::Tip("Write the new bounty with this faction into the loaded save.");
				ImGui::SameLine();
				ImGui::BeginDisabled(b.violent == 0 && b.nonViolent == 0);
				if (ImGui::SmallButton("Clear")) {
					done(cf::SetPlayerBounty(*_save, b.faction, 0, 0), "Cleared the player's bounty with " + formText(b.faction));
				}
				ImGui::EndDisabled();
				tips::Tip("Set both bounties with this faction to 0. Your crime record (the totals on the right) stays.");
				ImGui::TableNextColumn();
				ImGui::Text("%d violent, %d non-violent", b.violentTotal, b.nonViolentTotal);
				if (b.violentTotal || b.nonViolentTotal) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Reset all")) {
						done(cf::ClearPlayerCrimes(*_save, b.faction), "Cleared the player's bounty and crime record with " + formText(b.faction));
					}
					tips::Tip("Clear the bounty and the crime totals with this faction: a clean record, as if no crime was ever reported.");
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
			if (!shown) {
				ImGui::TextDisabled("No bounties or crimes recorded.");
			}
		}
	}
	ImGui::End();
}
