// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Mods that are still in a save but no longer in the player's load order: listed when the save is
// opened, and their leftovers removed after the player confirms (each through the same safety checks
// as "Remove mod from save"), into a new save file.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/LoadOrder.h"
#include "core/ModRemove.h"
#include "core/Ops.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace ess;

namespace
{

	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

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
}

// Mod Organizer: the profile ReSaver Plus remembers is not always the one the player uses now (another profile was
// picked in Mod Organizer since, or another Mod Organizer setup for the same game). Read with the wrong profile,
// half of the mods a save uses look "not enabled" and every check floods with false findings. So when a save opens,
// every profile of every Mod Organizer setup for this game is compared with the plugins the save was made with, and
// when another one clearly matches it better, it is used instead (and the user is told).
bool App::matchProfileToSave()
{
	if (!_save || !_mo2) {
		return false;
	}
	std::vector<std::string> savePlugins;
	for (const auto& p : _save->plugins.full) {
		savePlugins.push_back(Lower(p));
	}
	for (const auto& p : _save->plugins.lite) {
		savePlugins.push_back(Lower(p));
	}
	if (savePlugins.size() < 10) {
		return false;  // too few to tell profiles apart
	}
	auto score = [&](const Mo2Instance& m, const std::string& profile) {
		const auto active = loadorder::Read(m.profilesDir / platform::FromUtf8(profile) / "plugins.txt", !m.gamePath.empty() ? m.gamePath : gameFolder(), _game);
		if (!active.known) {
			return -1.0;
		}
		std::size_t in = 0;
		for (const auto& p : savePlugins) {
			in += active.plugins.contains(p);
		}
		return static_cast<double>(in) / static_cast<double>(savePlugins.size());
	};
	// this setup first, then the other Mod Organizer setups for the same game
	std::vector<Mo2Instance> setups{ *_mo2 };
	std::error_code          ec;
	for (auto& m : FindMo2Instances({ platform::ExeFolder() })) {
		if (Lower(m.gameName) == Lower(_mo2->gameName) && m.ini != _mo2->ini && std::filesystem::is_directory(m.modsDir, ec)) {
			setups.push_back(std::move(m));
		}
	}
	const double current = score(*_mo2, _profile);
	double       best = current;
	std::size_t  bestSetup = 0;
	std::string  bestProfile = _profile;
	std::string  table;
	for (std::size_t i = 0; i < setups.size(); ++i) {
		for (const auto& pr : setups[i].profiles) {
			const double sc = score(setups[i], pr);
			table += Fmt("\n    %s%s: %.0f%%", i ? (platform::ToUtf8(setups[i].ini.parent_path()) + " / ").c_str() : "", pr.c_str(), sc * 100.0);
			if (sc > best + 1e-9) {
				best = sc;
				bestSetup = i;
				bestProfile = pr;
			}
		}
	}
	if (startupLog) {
		startupLog(Fmt("MO2 profiles against this save's %zu plugins (share of them enabled in each):", savePlugins.size()) + table);
	}
	// only a clear winner: the current one misses more than a tenth of the save and the other has at least nine tenths
	if ((bestSetup == 0 && bestProfile == _profile) || best < 0.9 || best - current < 0.1) {
		return false;
	}
	const std::string was = _profile;
	if (bestSetup != 0) {
		_mo2 = setups[bestSetup];
		_sourcesPath = platform::ToUtf8(_mo2->modsDir);
		if (!_mo2->gamePath.empty()) {
			_gamePath = platform::ToUtf8(_mo2->gamePath);
		}
	}
	_profile = bestProfile;
	_detectNote = Fmt(tr("Switched to the Mod Organizer profile \"%s\"%s: %.0f%% of this save's plugins are enabled in it, only %.0f%% in \"%s\". "
	                     "(Options > Settings to pick another.)"),
		_profile.c_str(), bestSetup ? (" (" + platform::ToUtf8(_mo2->ini.parent_path()) + ")").c_str() : "", best * 100.0, std::max(0.0, current) * 100.0,
		was.c_str());
	if (startupLog) {
		startupLog("switched to MO2 profile " + _profile + (bestSetup ? " of " + platform::ToUtf8(_mo2->ini) : std::string()));
	}
	saveSettings();
	return true;
}

void App::checkMissingPlugins()
{
	_missing.clear();
	_missingSource.clear();
	_missingScripts.clear();
	_unfoundScripts.clear();
	_removeUnfoundScripts = false;
	_removeMissingScripts = true;
	_orderSuspect = _scanSuspect = false;
	_activeInSave = _activeFound = _activeChecked = 0;
	_missingFile.clear();
	if (!_save) {
		return;
	}
	_installKnown = _sources.scanned() && !scanning();
	// the load order the game uses next: the Mod Organizer profile's, or the game's own plugins.txt
	std::filesystem::path txt;
	if (_mo2 && !_profile.empty()) {
		txt = _mo2->profilesDir / platform::FromUtf8(_profile) / "plugins.txt";
		_missingSource = tr("Mod Organizer profile \"") + _profile + "\"";
	} else {
		txt = loadorder::GamePluginsTxt(_game, _save && _save->isLE());
		_missingSource = _vortex ? tr("your load order (Vortex)") : tr("your load order");
	}
	const auto active = loadorder::Read(txt, gameFolder(), _game);
	// scripts whose compiled file is nowhere: not in any mod (enabled or not) and not in the game's own
	// archives. Only when the game's Data folder was scanned too, or every vanilla script would count.
	//  - "missing": the save itself says the game could not load it either (it writes an empty parent
	//    type for those). Certain: removed by default.
	//  - "not found": the game loaded it when it saved, but the scan did not find its file. Either the
	//    mod was uninstalled since, or the scan looked in the wrong place (or missed an archive): not
	//    removed unless the player asks. Not listed at all when it runs on objects of a plugin that is
	//    in the load order (that mod is installed, so its scripts are too).
	if (_installKnown && _sources.gameDataScanned) {
		const auto& p = *_save->papyrus();
		std::unordered_map<std::string, std::size_t> uses;
		std::unordered_set<std::string>              onLoaded;  // scripts with an instance on an object of a loaded plugin
		for (const auto& i : p.instances) {
			const auto key = Lower(p.str(i.script));
			++uses[key];
			if (active.known && !i.refID.isZero() && i.refID.type() != RefID::Type::kCreated) {
				const auto plugin = Lower(_save->pluginOf(_save->formIDOf(i.refID)));
				if (!plugin.empty() && active.plugins.contains(plugin)) {
					onLoaded.insert(key);
				}
			}
		}
		for (const auto& a : p.activeScripts) {
			if (a.data && !a.data->frames.empty()) {
				++uses[Lower(p.str(a.data->frames.front().scriptName))];
			}
		}
		// a script some installed script extends exists somewhere (the game could not load the child
		// without it), even if the scan did not find its file: not listed
		std::unordered_set<std::string> parentsOfInstalled;
		for (const auto& sc : p.scripts) {
			if (!p.str(sc.parent).empty() && _sources.providers(p.str(sc.name))) {
				parentsOfInstalled.insert(Lower(p.str(sc.parent)));
			}
		}
		for (const auto& sc : p.scripts) {
			const auto name = p.str(sc.name);
			const auto key = Lower(name);
			if (name.empty() || ops::IsEngineType(name) || _sources.providers(name) || parentsOfInstalled.contains(key)) {
				continue;
			}
			if (ops::IsScriptUndefined(p, sc)) {
				_missingScripts.emplace_back(name, uses[key]);
			} else if (!onLoaded.contains(key)) {
				_unfoundScripts.emplace_back(name, uses[key]);
			}
		}
		std::sort(_missingScripts.begin(), _missingScripts.end());
		std::sort(_unfoundScripts.begin(), _unfoundScripts.end());
	}
	// archives or folders the scan could not read may hold scripts: "not found" is not proof then
	if (_installKnown && !_sources.errors.empty()) {
		_removeMissingScripts = false;
	}

	if (!active.known) {
		_missingSource = tr("your load order");
		return;  // no load order to compare plugins with (missing scripts can still be listed)
	}
	_missingFile = active.file;
	std::vector<std::string> all = _save->plugins.full;
	all.insert(all.end(), _save->plugins.lite.begin(), _save->plugins.lite.end());
	// Sanity: a save made with this load order has nearly all of its plugins in it, and a scan of the right
	// mods folder finds nearly every plugin the load order enables. Far off means the wrong profile, a stale
	// plugins.txt or the wrong mods folder: then nothing is ticked.
	for (const auto& p : all) {
		const auto low = Lower(p);
		if (!active.plugins.contains(low)) {
			continue;
		}
		++_activeInSave;
		const bool gameFile = (low.rfind("cc", 0) == 0 && low.find("sse") != std::string::npos) || low == "skyrim.esm" || low == "update.esm" || low == "dawnguard.esm" ||
							  low == "hearthfires.esm" || low == "dragonborn.esm" || low == "_resourcepack.esl" || low == "skyrimvr.esm" ||
							  modremove::IsCreationClubPlugin(low) ||
							  std::find(loadorder::Fallout4Masters().begin(), loadorder::Fallout4Masters().end(), low) != loadorder::Fallout4Masters().end();
		if (_installKnown && !gameFile) {
			++_activeChecked;
			_activeFound += _sources.pluginPath(p).has_value() ? 1 : 0;
		}
	}
	_orderSuspect = all.size() >= 10 && _activeInSave * 2 < all.size();
	_scanSuspect = _activeChecked >= 10 && _activeFound * 2 < _activeChecked;
	if (_orderSuspect || _scanSuspect) {
		_removeMissingScripts = false;
	}
	std::unordered_map<std::string, std::size_t> index;
	for (const auto& p : all) {
		if (active.plugins.contains(Lower(p))) {
			continue;
		}
		MissingMod m;
		m.plugin = p;
		m.installed = !_installKnown || _sources.pluginPath(p).has_value();  // unknown: treat as installed (not ticked)
		if (auto it = _sources.pluginFolder.find(Lower(p)); it != _sources.pluginFolder.end()) {
			m.folder = it->second;
		}
		m.selected = !m.installed;  // a disabled mod may come back: not ticked by default
		index[Lower(p)] = _missing.size();
		_missing.push_back(std::move(m));
	}
	if (_missing.empty()) {
		return;
	}
	// what is left of each in the save
	for (const auto& c : _save->changeForms) {
		const auto plugin = Lower(_save->pluginOf(_save->formIDOf(c.refID)));
		if (auto it = index.find(plugin); it != index.end()) {
			++_missing[it->second].changeForms;
		}
	}
	ensureMods();
	if (_mods) {
		for (const auto& r : _mods->rows) {
			if (auto it = index.find(Lower(r.name)); it != index.end()) {
				auto& m = _missing[it->second];
				m.scripts = r.scriptCount;
				m.instances = r.instanceCount;
				m.threads = r.threadCount;
			}
		}
	}
	// renamed rather than removed (Mod.esp -> Mod.esl, ...): offer to point the save at the new name instead
	{
		std::vector<std::string> enabled;
		for (const auto& low : active.plugins) {
			std::string name = low;
			if (const auto path = _sources.pluginPath(low)) {
				name = platform::ToUtf8(path->filename());
			}
			enabled.push_back(name);
		}
		for (auto& m : _missing) {
			m.renamedTo = ops::RenameCandidates(*_save, m.plugin, enabled);
		}
	}
	// nothing left of it: nothing to clean, the game already dropped it
	for (auto& m : _missing) {
		if (!m.renamedTo.empty()) {
			m.selected = false;  // probably renamed: never cleaned by default
		}
		if (m.changeForms == 0 && m.scripts == 0 && m.threads == 0) {
			m.selected = false;
		}
		if (_orderSuspect || _scanSuspect) {
			m.selected = false;
		}
	}
	if (_orderSuspect || _scanSuspect) {
		_removeMissingScripts = false;  // nothing destructive is ticked when the setup does not match the save
	}
}

void App::drawMissingBanner()
{
	if ((_missing.empty() && _missingScripts.empty() && _unfoundScripts.empty()) || _missingDismissed) {
		return;
	}
	std::size_t gone = 0, disabled = 0;
	for (const auto& m : _missing) {
		(m.installed ? disabled : gone) += 1;
	}
	ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::BannerBg());
	ImGui::BeginChild("##missing", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * (scanning() ? 4.6f : 3.2f)), ImGuiChildFlags_Borders);
	std::string msg;
	if (!_missing.empty()) {
		msg = _installKnown ? Fmt(tr("%zu mod(s) in this save are not in %s: %zu no longer installed, %zu installed but not enabled."), _missing.size(),
								  _missingSource.c_str(), gone, disabled)
							: Fmt(tr("%zu mod(s) in this save are not in %s."), _missing.size(), _missingSource.c_str());
	}
	if (!_missingScripts.empty()) {
		msg += (msg.empty() ? "" : " ") + Fmt(tr("%zu script(s) in it are no longer installed anywhere."), _missingScripts.size());
	}
	if (!_unfoundScripts.empty()) {
		msg += (msg.empty() ? "" : " ") + Fmt(tr("%zu script(s) were not found in your mods (the game still had them when it saved)."), _unfoundScripts.size());
	}
	if (!_installKnown && scanning()) {
		msg += tr("  Checking which are still installed (scanning your mods)...");
	}
	if (_orderSuspect || _scanSuspect) {
		msg += tr("  This looks wrong (see the review): nothing is ticked.");
	}
	TextColored(theme::Warn(), msg);
	if (scanning()) {
		drawBackgroundProgress(0);
	}
	if (ImGui::Button(tr("Review and remove their leftovers..."))) {
		for (auto& m : _missing) {
			m.planned = false;
		}
		_missingOpen = true;
	}
	tips::Tip(tr("List the mods this save used that are no longer installed, with what each left behind, and remove their leftovers into a new save file. Nothing changes until you confirm."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Hide"))) {
		_missingDismissed = true;
	}
	tips::Tip(tr("Hides this until the next save is opened. Clean > Mods and scripts no longer installed... brings it back."));
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

void App::drawMissingModal()
{
	if (_missingOpenWhenReady && !scanning() && (!_missing.empty() || !_missingScripts.empty() || !_unfoundScripts.empty())) {
		_missingOpenWhenReady = false;
		_missingOpen = true;  // (test hook)
	}
	if (_missingOpen) {
		ImGui::OpenPopup(trId("Mods no longer in your load order"));
		_missingOpen = false;
	}
	ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_Appearing);
	if (!ImGui::BeginPopupModal(trId("Mods no longer in your load order"))) {
		return;
	}
	ImGui::TextWrapped(tr("These plugins are listed in the save but are not enabled in %s. The game does not load them, so their scripts "
					   "and saved data are leftovers. Removing them cleans the save the way \"Remove mod from save\" does, with the "
					   "same safety checks, and writes a NEW save file: the original is not changed."),
		_missingSource.c_str());
	TextColored(theme::Dim(), tr("A mod that is only disabled for a test, or enabled in another profile, will need its data again: leave it unticked."));
	if (!_installKnown && scanning()) {
		TextColored(theme::Warn(), tr("Still scanning your mods: until it is done ReSaver++ cannot tell uninstalled mods from disabled ones, so nothing "
								   "is ticked. The list updates by itself when the scan is done."));
	} else if (!_installKnown) {
		TextColored(theme::Warn(), tr("Your mods folder is not set (Options > Settings), so ReSaver++ cannot tell uninstalled mods from disabled ones: "
						   "nothing is ticked."));
	}
	TextColored(theme::Dim(), tr("Load order read from: ") + platform::ToUtf8(_missingFile) +
								  (_installKnown ? tr("    Mods folder: ") + _sourcesPath : std::string()));
	if (_orderSuspect) {
		const auto inSave = _save ? _save->plugins.full.size() + _save->plugins.lite.size() : 0;
		if (_mo2) {
			TextColored(theme::Bad(), Fmt(tr("This load order does not match the save: only %zu of the save's %zu plugins are in it. It is probably "
										  "the wrong profile. Nothing is ticked. Pick the profile you play with in Options > Settings (MO2 "
										  "profile, under the mods folder) or in the Mods view."),
										  _activeInSave, inSave));
		} else {
			TextColored(theme::Bad(), Fmt(tr("This load order does not match the save: only %zu of the save's %zu plugins are in it. Nothing is "
										  "ticked. No Mod Organizer was found for your mods folder, so the game's own plugins.txt was read, "
										  "which Mod Organizer does not use. In Options > Settings press \"Use a ModOrganizer.ini...\" and "
										  "pick the ModOrganizer.ini of the Mod Organizer you play with (for a portable one such as Nolvus, "
										  "the one next to its ModOrganizer.exe), then pick your profile in the MO2 profile list there."),
										  _activeInSave, inSave));
		}
	}
	if (_installKnown && !_sources.errors.empty()) {
		std::string list;
		for (std::size_t i = 0; i < _sources.errors.size() && i < 6; ++i) {
			list += "\n    " + _sources.errors[i];
		}
		if (_sources.errors.size() > 6) {
			list += Fmt(tr("\n    ... and %zu more"), _sources.errors.size() - 6);
		}
		TextColored(theme::Warn(), Fmt(tr("%zu archive(s) or folder(s) could not be read, so a script \"not found\" may just be in one of them. "
									   "Missing scripts are not ticked:"),
									   _sources.errors.size()) +
									   list);
	}
	if (_scanSuspect) {
		TextColored(theme::Bad(), Fmt(tr("The mods scan found only %zu of the %zu plugins your load order enables, so it probably looked in the wrong "
									  "folder. Nothing is ticked: check the mods folder in Options > Settings."),
									  _activeFound, _activeChecked));
	}
	ImGui::Separator();

	ImGui::BeginChild("rows", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.6f));
	if (ImGui::BeginTable("missing", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn(tr("Plugin"), 0, 3);
		ImGui::TableSetupColumn(tr("Status"), 0, 3);
		ImGui::TableSetupColumn(tr("Left in the save"), 0, 3);
		ImGui::TableSetupColumn(tr("Safety check"), 0, 4);
		ImGui::TableHeadersRow();
		for (std::size_t i = 0; i < _missing.size(); ++i) {
			auto& m = _missing[i];
			// the safety check of each ticked mod, on the unchanged save
			if (m.selected && !m.planned) {
				const auto plan = modremove::MakePlan(*_save, m.plugin, _sources.scanned() ? &_sources : nullptr, _removeOpt);
				m.planned = true;
				m.blocked = !plan.canApply();
				m.planText = plan.summary();
			}
			ImGui::TableNextRow();
			ImGui::PushID(static_cast<int>(i));
			ImGui::TableSetColumnIndex(0);
			const bool nothing = m.changeForms == 0 && m.scripts == 0 && m.threads == 0;
			ImGui::BeginDisabled(nothing);
			ImGui::Checkbox(m.plugin.c_str(), &m.selected);
			ImGui::EndDisabled();
			tips::Tip(nothing ? tr("This mod left nothing in the save: there is nothing to remove.") : tr("Ticked: remove this mod's leftovers (its changed forms, scripts, instances and threads). A safety check runs first and refuses removals that would break the save."));
			ImGui::TableSetColumnIndex(1);
			if (!m.renamedTo.empty()) {
				TextColored(theme::Warn(), tr("renamed? Now enabled as:"));
				for (const auto& to : m.renamedTo) {
					ImGui::PushID(to.c_str());
					ImGui::BulletText("%s", to.c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("Point the save at it"))) {
						const auto from = m.plugin;
						confirm(tr("Point the save at a renamed plugin"),
							tr("Change the save's entry ") + from + tr(" to ") + to + tr("?\n\nUse this when you renamed the plugin (for example .esp to .esl). "
							"The game then finds it under its new name and keeps everything the save holds from it: placed objects, NPC "
							"changes, quest progress and its scripts.\n\nDo NOT use it if the plugin's records were renumbered (\"compacted\", "
							"e.g. by ESLifier or xEdit's Compact FormIDs): then the save's ids no longer match, and only a new game or "
							"undoing the conversion helps.\n\nKeep a copy of the original save."),
							[this, from, to] {
								if (auto why = ops::RenamePlugin(*_save, from, to); !why.empty()) {
									_error = tr("Could not point the save at ") + to + ": " + why;
									return;
								}
								markChanged(tr("The save now loads ") + to + tr(" where it had ") + from);
								checkMissingPlugins();
							});
					}
					tips::Tip(tr("Rename this plugin's entry in the save to the enabled plugin, so the game keeps the mod's data in this save. Only "
							  "for a renamed plugin, not a compacted one."));
					ImGui::PopID();
				}
			} else if (m.installed) {
				TextColored(theme::Warn(), tr("installed but not enabled") + (m.folder.empty() ? std::string() : " (" + m.folder + ")"));
			} else {
				ImGui::TextUnformatted(tr("not installed"));
			}
			ImGui::TableSetColumnIndex(2);
			if (nothing) {
				ImGui::TextDisabled("%s", tr("nothing (already clean)"));
			} else {
				ImGui::Text(tr("%zu forms, %zu scripts, %zu instances, %zu threads"), m.changeForms, m.scripts, m.instances, m.threads);
			}
			ImGui::TableSetColumnIndex(3);
			if (!m.selected) {
				ImGui::TextDisabled("-");
			} else if (m.blocked) {
				TextColored(theme::Bad(), tr("refused (see details)"));
			} else {
				ImGui::TextUnformatted(tr("OK"));
			}
			if (m.planned && m.selected && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", m.planText.c_str());
			}
			if (m.selected && m.planned) {
				ImGui::SameLine();
				if (ImGui::SmallButton(tr("details"))) {
					ImGui::OpenPopup("plan");
				}
				tips::Tip(tr("Show exactly what would be removed for this mod, or why the safety check refused it."));
				if (ImGui::BeginPopup("plan")) {
					ImGui::PushTextWrapPos(ImGui::GetFontSize() * 45);
					ImGui::TextUnformatted(m.planText.c_str());
					ImGui::PopTextWrapPos();
					ImGui::EndPopup();
				}
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	// scripts whose files are gone (a script-only mod, or a mod's scripts on other mods' objects)
	if (!_missingScripts.empty()) {
		ImGui::Checkbox(Fmt(tr("Also remove %zu script(s) that are no longer installed anywhere (their threads, queued calls and instances)"),
							_missingScripts.size())
							.c_str(),
			&_removeMissingScripts);
		tips::Tip(tr("Also delete the threads, queued calls and instances of scripts whose files exist nowhere any more. The game cannot run them, so they only cost load time and log errors."));
		if (ImGui::TreeNode("missing scripts", "%s", tr("Show them"))) {
			for (const auto& [name, n] : _missingScripts) {
				ImGui::BulletText(tr("%s  (%zu in use)"), name.c_str(), n);
			}
			ImGui::TreePop();
		}
		TextColored(theme::Dim(), tr("Not found in any mod folder (enabled or not) nor in the game's own archives, and the save itself shows the game "
								  "could not load them either. The game cannot run them any more."));
	}
	// the game had these when it saved: only the scan says they are gone, so they are not ticked
	if (!_unfoundScripts.empty()) {
		ImGui::Spacing();
		TextColored(theme::Warn(), Fmt(tr("%zu script(s) were not found in your mods, but the game still loaded them when it made this save."),
									   _unfoundScripts.size()));
		TextColored(theme::Dim(), Fmt(tr("Either their mod was uninstalled after this save, or ReSaver++ looked in the wrong place. Mods folder scanned: "
									  "%s (%zu mod folders, %zu scripts). If these mods are still installed, do NOT remove them: check the "
									  "mods folder in Options > Settings first. Removing a script a mod still uses restarts its quests and its MCM."),
							  _sourcesPath.c_str(), _sources.modsScanned, _sources.pexFound));
		ImGui::Checkbox(Fmt(tr("Also remove these %zu script(s) (only if you uninstalled their mods)"), _unfoundScripts.size()).c_str(), &_removeUnfoundScripts);
		tips::Tip(tr("Only tick this if you really uninstalled these mods. If they are still installed and ReSaver++ just looked in the wrong folder, removing them restarts their quests and MCM."));
		if (ImGui::TreeNode("unfound scripts", "%s", tr("Show them"))) {
			for (const auto& [name, n] : _unfoundScripts) {
				ImGui::BulletText(tr("%s  (%zu in use)"), name.c_str(), n);
			}
			ImGui::TreePop();
		}
	}
	ImGui::EndChild();

	std::size_t ticked = 0, refused = 0;
	for (const auto& m : _missing) {
		ticked += m.selected ? 1 : 0;
		refused += m.selected && m.blocked ? 1 : 0;
	}
	auto target = _path;
	{
		target = EditedSaveName(_path, "cleaned");
	}
	ImGui::TextDisabled(tr("New file: %s%s"), platform::ToUtf8(target.filename()).c_str(),
		refused ? Fmt(tr("   (%zu refused by the safety check will be skipped)"), refused).c_str() : "");
	std::vector<std::pair<std::string, std::size_t>> scriptList;
	if (_removeMissingScripts) {
		scriptList = _missingScripts;
	}
	if (_removeUnfoundScripts) {
		scriptList.insert(scriptList.end(), _unfoundScripts.begin(), _unfoundScripts.end());
	}
	const bool scriptsToo = !scriptList.empty();
	ImGui::BeginDisabled(ticked == refused && !scriptsToo);
	if (ImGui::Button(tr("Remove and write new save"), ImVec2(280, 0))) {
		// one after the other: each plan is made on the save as the previous removal left it
		std::size_t                    done = 0;
		std::string                    skipped;
		std::vector<ops::KeptInstance> kept;  // ActiveMagicEffect instances left: their effect is running
		for (const auto& m : _missing) {
			if (!m.selected) {
				continue;
			}
			const auto plan = modremove::MakePlan(*_save, m.plugin, _sources.scanned() ? &_sources : nullptr, _removeOpt);
			if (!plan.canApply()) {
				skipped += (skipped.empty() ? "" : ", ") + m.plugin;
				continue;
			}
			const auto ar = modremove::Apply(*_save, plan);
			kept.insert(kept.end(), ar.kept.begin(), ar.kept.end());
			++done;
		}
		std::size_t scriptsDone = 0;
		if (scriptsToo) {
			auto& pap = *_save->papyrus();
			std::set<TStr> defs;
			for (const auto& [name, n] : scriptList) {
				// spawned objects it sat on stay: another (installed) script may be on the same object
				const auto pr = ops::PurgeScript(*_save, name, false);
				kept.insert(kept.end(), pr.kept.begin(), pr.kept.end());
				++scriptsDone;
				if (const auto t = pap.findString(name); t != UINT32_MAX) {
					defs.insert(t);
				}
			}
			// the script definitions too (PurgeScript leaves them), or the save keeps listing the scripts
			// and they show up again every time. One another remaining script extends stays.
			for (const auto& sc : pap.scripts) {
				if (!defs.contains(sc.name)) {
					defs.erase(sc.parent);
				}
			}
			// a script whose instance stayed (its effect is running) keeps its definition
			const auto dr = ops::DeleteScripts(*_save, defs);
			kept.insert(kept.end(), dr.kept.begin(), dr.kept.end());
		}
		ImGui::CloseCurrentPopup();
		if (done || scriptsDone) {
			markEdited(Fmt(tr("Removed the leftovers of %zu mod(s) and %zu missing script(s)"), done, scriptsDone) + keptNote(kept));
			_modsDirty = true;
			doSave(target);
			_report = std::make_unique<ops::Report>(ops::Analyze(*_save));
			if (_error.empty()) {
				_status = Fmt(tr("Removed the leftovers of %zu mod(s) and %zu missing script(s), and wrote "), done, scriptsDone) + platform::ToUtf8(target.filename()) +
						  (skipped.empty() ? "" : tr("  (skipped: ") + skipped + ")");
			}
			checkMissingPlugins();
		}
	}
	ImGui::EndDisabled();
	tips::Tip(tr("Remove the ticked leftovers and write the result as a NEW save file next to the original (the original is kept). Load the new save in the game and check it before playing on."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Cancel"), ImVec2(120, 0))) {
		ImGui::CloseCurrentPopup();
	}
	tips::Tip(tr("Close without changing anything."));
	ImGui::EndPopup();
}
