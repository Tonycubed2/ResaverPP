// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Leftover items from removed mods, save history (bloat over time), the Papyrus log reader and the
// saves folder manager.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/LoadOrder.h"
#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <set>

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

	std::string Date(std::int64_t a_unix)
	{
		if (a_unix <= 0) {
			return "?";
		}
		const std::time_t t = static_cast<std::time_t>(a_unix);
		std::tm           tm{};
#ifdef _WIN32
		localtime_s(&tm, &t);
#else
		localtime_r(&t, &tm);
#endif
		char buf[32];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
		return buf;
	}

	std::filesystem::path HistoryFile() { return platform::SettingsFile().parent_path() / "resaverpp-history.txt"; }
}

loadorder::Active App::currentLoadOrder() const
{
	std::filesystem::path txt;
	if (_mo2 && !_profile.empty()) {
		txt = _mo2->profilesDir / platform::FromUtf8(_profile) / "plugins.txt";
	} else {
		txt = loadorder::GamePluginsTxt(_game, _save && _save->isLE());
	}
	return loadorder::Read(txt, gameFolder(), _game);
}

// ==========================================================================================
// leftover items
// ==========================================================================================

void App::drawLeftovers()
{
	prepareWindow("Leftover items", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin(trId("Leftover items"), &_showLeftovers, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Leftover items");
	if (_leftoversDirty) {
		_leftoversDirty = false;
		_leftovers.clear();
		_leftoversNote.clear();
		const auto active = currentLoadOrder();
		if (!active.known && !_db) {
			_leftoversNote = tr("Your load order is not known yet (set your game folder and mod manager in Options > Settings) and plugin names "
							 "are not read, so ReSaver Plus cannot tell which items no longer exist.");
		}
		if (_leftoversNote.empty() && _db && (!_db->missing.empty() || !_db->errors.empty())) {
			_leftoversNote = Fmt(tr("%zu of your plugins were not found or could not be read: items from them are not checked here (an "
									"item counts as gone only when its plugin was read and no longer has it)."),
				_db->missing.size() + _db->errors.size());
		}
		std::map<std::string, std::size_t> index;
		for (const auto& e : cf::ListInventories(*_save)) {
			std::string plugin, reason;
			if (e.item == 0) {
				plugin = tr("(none)");
				reason = tr("an entry with no item");
			} else {
				plugin = _save->pluginOf(e.item);
				const auto low = Lower(plugin);
				if (plugin == "Created") {
					continue;  // made in game (enchanted, crafted): kept with the save
				}
				if (plugin == "?") {
					reason = tr("its plugin is not in the save's plugin list");
				} else if (active.known && !active.plugins.contains(low)) {
					reason = tr("its plugin is not in your load order");
				} else if (_db && _db->wasRead(plugin) && !_db->find(e.item)) {
					reason = tr("the item is no longer in its plugin (the mod was updated or changed)");
				} else {
					continue;
				}
			}
			const auto key = plugin + "\x1f" + reason;
			auto       it = index.find(key);
			if (it == index.end()) {
				it = index.emplace(key, _leftovers.size()).first;
				LeftoverGroup g;
				g.plugin = plugin;
				g.reason = reason;
				_leftovers.push_back(std::move(g));
			}
			auto& g = _leftovers[it->second];
			g.items[e.item].push_back(e.holderRefRaw);
			++g.entries;
		}
	}

	ImGui::NewLine();
	ImGui::TextWrapped("%s", tr("Items in the inventories of the player, NPCs and containers whose mod is gone. The game cannot show them, and they "
					   "can break inventory and container menus or cause crashes. Removing them takes them out of every inventory in the save. "
					   "Changes are made in the loaded save; nothing is written until you use File > Save or Save As."));
	if (!_leftoversNote.empty()) {
		TextColored(theme::Warn(), _leftoversNote);
	}
	if (ImGui::Button(tr("Look again"))) {
		_leftoversDirty = true;
	}
	tips::Tip(tr("Read every inventory again (after you changed your load order or read plugin names)."));
	ImGui::Spacing();
	if (_leftovers.empty()) {
		TextColored(theme::Good(), _leftoversNote.empty() ? tr("No leftover items: every item in every inventory belongs to a mod you have.") : tr("Nothing found."));
		ImGui::End();
		return;
	}
	std::size_t ticked = 0;
	for (const auto& g : _leftovers) {
		ticked += g.selected ? g.entries : 0;
	}
	if (ImGui::BeginTable("leftovers", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
			ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Mod (plugin)"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn(tr("Why"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
		ImGui::TableSetupColumn(tr("Items"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn(tr("Entries"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableHeadersRow();
		for (std::size_t gi = 0; gi < _leftovers.size(); ++gi) {
			auto& g = _leftovers[gi];
			ImGui::PushID(static_cast<int>(gi));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Checkbox("##sel", &g.selected);
			tips::Tip(tr("Ticked: remove these items from every inventory."));
			ImGui::SameLine();
			const bool open = ImGui::TreeNodeEx(g.plugin.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
			tips::Tip(tr("Show the items and who holds them."));
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", g.reason.c_str());
			ImGui::TableNextColumn();
			ImGui::Text(tr("%zu"), g.items.size());
			ImGui::TableNextColumn();
			ImGui::Text(tr("%zu"), g.entries);
			if (open) {
				for (const auto& [item, holders] : g.items) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextDisabled("   %s", formText(item).c_str());
					ImGui::TableNextColumn();
					std::string who;
					std::set<std::uint32_t> seen;
					for (auto h : holders) {
						if (seen.insert(h).second && seen.size() <= 4) {
							RefID id;
							id.data = h;
							const auto fid = _save->formIDOf(id);
							who += (who.empty() ? "" : ", ") + (fid == 0x14 ? std::string(tr("you")) : formText(fid));
						}
					}
					if (seen.size() > 4) {
						who += Fmt(tr(" and %zu more"), seen.size() - 4);
					}
					ImGui::TextDisabled(tr("held by %s"), who.c_str());
					ImGui::TableNextColumn();
					ImGui::TableNextColumn();
					ImGui::TextDisabled(tr("%zu"), holders.size());
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::BeginDisabled(ticked == 0);
	if (ImGui::Button(Fmt(tr("Remove the ticked items (%zu entries)"), ticked).c_str())) {
		confirm(tr("Remove leftover items"),
			Fmt(tr("Remove %zu inventory entries of items whose mod is gone, from every inventory in the save?\n\nOnly do this for mods you "
				"removed for good: if you put the mod back, its items will not come back.\n\nKeep a copy of the original save."),
				ticked),
			[this] {
				// per holder, the items to take out
				std::map<std::uint32_t, std::set<std::uint32_t>> perHolder;
				for (const auto& g : _leftovers) {
					if (!g.selected) {
						continue;
					}
					for (const auto& [item, holders] : g.items) {
						for (auto h : holders) {
							perHolder[h].insert(item);
						}
					}
				}
				std::size_t removed = 0, failed = 0;
				for (const auto& [h, items] : perHolder) {
					RefID id;
					id.data = h;
					auto* cf = _save->findChangeForm(id);
					std::size_t n = 0;
					if (!cf || !cf::RemoveInventoryItems(*_save, *cf, items, &n).empty()) {
						++failed;
						continue;
					}
					removed += n;
				}
				markChanged(Fmt(tr("Removed %zu leftover inventory entries%s"), removed,
					failed ? Fmt(tr(" (%zu inventories could not be changed and were left alone)"), failed).c_str() : ""));
				_leftoversDirty = true;
			});
	}
	ImGui::EndDisabled();
	tips::Tip(tr("Take the ticked mods' items out of every inventory in the loaded save."));
	ImGui::End();
}

// ==========================================================================================
// save history
// ==========================================================================================

void App::recordHistory()
{
	if (!_save) {
		return;
	}
	if (!_historyLoaded) {
		_history = extras::LoadHistory(HistoryFile());
		_historyLoaded = true;
	}
	extras::AddToHistory(_history, extras::ComputeStat(*_save, _path));
	extras::SaveHistory(HistoryFile(), _history);
	_historyChar = _save->header.playerName;
}

void App::drawHistory()
{
	prepareWindow("Save history", ImVec2(0.8f, 0.8f));
	if (!ImGui::Begin(trId("Save history"), &_showHistory, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Save history");
	if (!_historyLoaded) {
		_history = extras::LoadHistory(HistoryFile());
		_historyLoaded = true;
	}
	// results of a folder read
	if (!_historyBusy && _historyThread.joinable()) {
		_historyThread.join();
		std::lock_guard lk(_historyMutex);
		for (const auto& s : _historyIncoming) {
			extras::AddToHistory(_history, s);
		}
		_historyIncoming.clear();
		extras::SaveHistory(HistoryFile(), _history);
	}

	std::set<std::string> chars;
	for (const auto& s : _history) {
		chars.insert(s.character);
	}
	if (_historyChar.empty() && !chars.empty()) {
		_historyChar = *chars.begin();
	}
	ImGui::NewLine();
	ImGui::TextWrapped("%s", tr("Every save ReSaver Plus opens is noted here (only numbers, in resaverpp-history.txt next to the program). Over "
					   "several saves it shows what keeps growing: a mod whose script instances climb save after save usually has a "
					   "runaway script that will slow the game down and bloat the save."));
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
	if (ImGui::BeginCombo(tr("Character"), text::ToUtf8(_historyChar).c_str())) {
		for (const auto& c : chars) {
			if (ImGui::Selectable(text::ToUtf8(c).c_str(), c == _historyChar)) {
				_historyChar = c;
			}
		}
		ImGui::EndCombo();
	}
	tips::Tip(tr("Whose saves to show."));
	ImGui::SameLine();
	ImGui::BeginDisabled(_historyBusy || _historyChar.empty());
	if (ImGui::Button(tr("Read all of this character's saves"))) {
		const auto folder = !_path.empty() ? _path.parent_path() : savesFolder();
		const auto who = _historyChar;
		_historyBusy = true;
		_historyCancel = false;
		_historyDone = 0;
		_historyTotal = 0;
		if (_historyThread.joinable()) {
			_historyThread.join();
		}
		_historyThread = std::thread([this, folder, who] {
			std::vector<std::filesystem::path> files;
			for (const auto& i : extras::ListSaves(folder)) {
				if (i.ok && i.playerName == who) {
					files.push_back(i.path);
				}
			}
			_historyTotal = files.size();
			for (const auto& f : files) {
				if (_historyCancel) {
					break;
				}
				try {
					const auto save = SaveFile::load(f);
					auto       st = extras::ComputeStat(*save, f);
					std::lock_guard lk(_historyMutex);
					_historyIncoming.push_back(std::move(st));
				} catch (const std::exception&) {
				}
				++_historyDone;
			}
			_historyBusy = false;
		});
	}
	ImGui::EndDisabled();
	tips::Tip(tr("Open every save of this character in the saves folder (one by one, in the background) and note their numbers, so the "
			  "history starts complete instead of filling up as you play."));
	if (_historyBusy) {
		ImGui::SameLine();
		ImGui::Text(tr("reading %zu of %zu saves..."), _historyDone.load(), _historyTotal.load());
	}

	std::vector<const extras::SaveStat*> rows;
	for (const auto& s : _history) {
		if (s.character == _historyChar) {
			rows.push_back(&s);
		}
	}
	std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) {
		return a->saveNumber != b->saveNumber ? a->saveNumber < b->saveNumber : a->modified < b->modified;
	});
	if (rows.empty()) {
		ImGui::TextDisabled("%s", tr("No saves noted for this character yet."));
		ImGui::End();
		return;
	}

	// what keeps growing
	std::vector<extras::SaveStat> ordered;
	for (const auto* r : rows) {
		ordered.push_back(*r);
	}
	const auto growth = extras::FindGrowth(ordered);
	ImGui::Spacing();
	if (rows.size() < 4) {
		ImGui::TextDisabled(tr("Growth is judged over 4 or more saves; %zu noted so far."), rows.size());
	} else if (growth.empty()) {
		TextColored(theme::Good(), tr("Nothing keeps climbing: no mod's script instances grew steadily over the last saves."));
	} else {
		TextColored(theme::Warn(), tr("Keeps growing over the last saves (script instances on that mod's objects):"));
		for (const auto& g : growth) {
			ImGui::BulletText(tr("%s: %zu -> %zu over %zu saves"), g.plugin == "Created" ? tr("Spawned objects (any mod)") : g.plugin.c_str(), g.first, g.last,
				g.saves);
		}
		ImGui::TextDisabled("%s", tr("Look at that mod in the Mods view: a script that keeps creating instances or threads is the usual cause."));
	}

	// small charts
	auto plot = [&](const char* a_label, auto a_value) {
		std::vector<float> v;
		for (const auto* r : rows) {
			v.push_back(static_cast<float>(a_value(*r)));
		}
		const float hi = *std::max_element(v.begin(), v.end());
		ImGui::PlotLines(a_label, v.data(), static_cast<int>(v.size()), 0, Fmt(tr("max %.0f"), hi).c_str(), 0.0f, hi * 1.1f + 1,
			ImVec2(ImGui::GetContentRegionAvail().x * 0.3f, ImGui::GetFontSize() * 4));
	};
	ImGui::Spacing();
	plot("##inst", [](const extras::SaveStat& s) { return s.instances; });
	ImGui::SameLine();
	plot("##created", [](const extras::SaveStat& s) { return s.created; });
	ImGui::SameLine();
	plot("##size", [](const extras::SaveStat& s) { return s.size / 1048576.0; });
	ImGui::TextDisabled("%s", tr("Script instances (left), spawned objects (middle), save size in MB (right), oldest save first."));

	if (ImGui::BeginTable("hist", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		for (const char* h : { trc("column", "Save"), tr("Date"), tr("Size"), tr("Script instances"), tr("Threads"), tr("Spawned objects"), tr("Change forms"), tr("Arrays") }) {
			ImGui::TableSetupColumn(h);
		}
		ImGui::TableHeadersRow();
		const extras::SaveStat* prev = nullptr;
		for (const auto* r : rows) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("#%u", r->saveNumber);
			tips::Tipf("%s", text::ToUtf8(r->file).c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(Date(r->modified).c_str());
			auto cell = [&](double a_now, double a_before, const char* a_fmt) {
				ImGui::TableNextColumn();
				const bool up = prev && a_now > a_before * 1.1 && a_now - a_before > 100;
				if (up) {
					ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
				}
				ImGui::Text(a_fmt, a_now);
				if (up) {
					ImGui::PopStyleColor();
				}
			};
			cell(r->size / 1048576.0, prev ? prev->size / 1048576.0 : 0, tr("%.1f MB"));
			cell(static_cast<double>(r->instances), prev ? static_cast<double>(prev->instances) : 0, "%.0f");
			cell(static_cast<double>(r->threads), prev ? static_cast<double>(prev->threads) : 0, "%.0f");
			cell(static_cast<double>(r->created), prev ? static_cast<double>(prev->created) : 0, "%.0f");
			cell(static_cast<double>(r->changeForms), prev ? static_cast<double>(prev->changeForms) : 0, "%.0f");
			cell(static_cast<double>(r->arrays), prev ? static_cast<double>(prev->arrays) : 0, "%.0f");
			prev = r;
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

// ==========================================================================================
// Papyrus log
// ==========================================================================================

void App::drawPapyrusLog()
{
	prepareWindow("Papyrus log", ImVec2(0.8f, 0.8f));
	if (!ImGui::Begin(trId("Papyrus log"), &_showPapyrusLog, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Papyrus log");
	auto candidates = [&] {
		std::vector<std::filesystem::path> v;
		if (!_path.empty()) {
			v.push_back(_path.parent_path().parent_path() / "Logs" / "Script" / "Papyrus.0.log");
		}
		v.push_back(savesFolder().parent_path() / "Logs" / "Script" / "Papyrus.0.log");
		v.push_back(platform::DefaultSavesFolder(_game).parent_path() / "Logs" / "Script" / "Papyrus.0.log");
		if (_save && _save->isLE()) {
			v.push_back(platform::DefaultSavesFolder(false).parent_path().parent_path() / "Skyrim" / "Logs" / "Script" / "Papyrus.0.log");
		}
		return v;
	};
	if (!_plogLoaded) {
		_plogLoaded = true;
		_plogOpen = -1;
		_plog = {};
		if (const char* t = std::getenv("RESAVERPP_TEST_PLOG")) {
			_plog = extras::ReadPapyrusLog(t);
		} else {
			for (const auto& c : candidates()) {
				std::error_code ec;
				if (std::filesystem::exists(c, ec)) {
					_plog = extras::ReadPapyrusLog(c);
					break;
				}
			}
		}
	}
	ImGui::NewLine();
	if (ImGui::Button(tr("Open another log..."))) {
		if (platform::HasNativeDialogs()) {
			if (auto f = platform::OpenFileDialog(candidates().front().parent_path(), "log", tr("Papyrus log"))) {
				_plog = extras::ReadPapyrusLog(*f);
				_plogOpen = -1;
			}
		}
	}
	tips::Tip(tr("Pick a Papyrus log yourself (Papyrus.1.log is the session before, Papyrus.2.log the one before that)."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Read again"))) {
		_plogLoaded = false;
	}
	tips::Tip(tr("Read the log again, e.g. after playing some more."));
	if (!_plog.ok) {
		if (_game == 2) {
			TextColored(theme::Warn(), tr("No Papyrus log found. Fallout 4 only writes one when logging is on: in Fallout4Custom.ini "
									   "(Documents\\My Games\\Fallout4, or your Mod Organizer profile's) under [Papyrus] set bEnableLogging=1, "
									   "bEnableTrace=1 and bLoadDebugInformation=1, play a while, then look again. The log is in "
									   "Documents\\My Games\\Fallout4\\Logs\\Script\\Papyrus.0.log."));
		} else {
			TextColored(theme::Warn(), tr("No Papyrus log found. The game only writes one when logging is on: in Skyrim.ini (or SkyrimCustom.ini) "
									   "under [Papyrus] set bEnableLogging=1, bEnableTrace=1 and bLoadDebugInformation=1, play a while, then "
									   "look again. The log is in Documents\\My Games\\Skyrim Special Edition\\Logs\\Script\\Papyrus.0.log."));
		}
		ImGui::End();
		return;
	}
	ImGui::TextWrapped("%s", platform::ToUtf8(_plog.file).c_str());
	ImGui::TextDisabled("%s", _plog.started.c_str());
	ImGui::Text(tr("%zu errors and %zu warnings in %zu lines, in %zu groups."), _plog.errors, _plog.warnings, _plog.lines, _plog.groups.size());
	ImGui::TextWrapped("%s", tr("The same error repeated many times by one script usually means a mod that is broken, missing a requirement, or "
					   "running on an object that is gone. Click a row for the full error and stack."));
	ImGui::Spacing();
	const auto* p = _save ? _save->papyrus() : nullptr;
	auto inSave = [&](const std::string& a_script) {
		std::size_t inst = 0, thr = 0;
		if (!p || a_script.empty()) {
			return std::pair<std::size_t, std::size_t>{ 0, 0 };
		}
		const auto low = Lower(a_script);
		for (const auto& i : p->instances) {
			inst += Lower(p->str(i.script)) == low ? 1 : 0;
		}
		for (const auto& a : p->activeScripts) {
			if (a.data) {
				for (const auto& f : a.data->frames) {
					if (Lower(p->str(f.scriptName)) == low) {
						++thr;
						break;
					}
				}
			}
		}
		return std::pair<std::size_t, std::size_t>{ inst, thr };
	};
	if (ImGui::BeginTable("plog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Times"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4);
		ImGui::TableSetupColumn(tr("Script"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableSetupColumn(tr("Mod"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
		ImGui::TableSetupColumn(tr("In this save"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn(tr("Error"), ImGuiTableColumnFlags_WidthStretch, 4.0f);
		ImGui::TableHeadersRow();
		const std::size_t shown = std::min<std::size_t>(_plog.groups.size(), 500);
		for (std::size_t i = 0; i < shown; ++i) {
			const auto& g = _plog.groups[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text(tr("%zu"), g.count);
			ImGui::TableNextColumn();
			if (ImGui::Selectable(g.script.empty() ? tr("(not named)") : g.script.c_str(), _plogOpen == static_cast<int>(i),
					ImGuiSelectableFlags_SpanAllColumns)) {
				_plogOpen = _plogOpen == static_cast<int>(i) ? -1 : static_cast<int>(i);
			}
			tips::Tip(tr("Click to show the full error and its stack."));
			ImGui::TableNextColumn();
			std::string mod;
			if (!g.script.empty()) {
				if (const auto* prov = _sources.providers(g.script); prov && !prov->empty()) {
					mod = prov->front().mod;
				}
			}
			ImGui::TextUnformatted(mod.empty() ? "-" : mod.c_str());
			ImGui::TableNextColumn();
			if (!g.script.empty() && p) {
				const auto [inst, thr] = inSave(g.script);
				if (inst || thr) {
					const bool go = ImGui::SmallButton(tr("show"));
					tips::Tip(tr("Show this script's instances (or threads) in the main window."));
					ImGui::SameLine();
					ImGui::Text(tr("%zu instances, %zu threads"), inst, thr);
					if (go) {
						_cat = thr ? Cat::kThreads : Cat::kInstances;
						std::snprintf(_filter, sizeof(_filter), "%s", g.script.c_str());
						_rowsDirty = true;
						_focusFirstRow = true;
					}
				} else {
					ImGui::TextDisabled("%s", tr("not in this save"));
				}
			}
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", g.message.c_str());
			if (_plogOpen == static_cast<int>(i)) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TableNextColumn();
				ImGui::TableSetColumnIndex(4);
				ImGui::PushTextWrapPos(0);
				ImGui::TextDisabled("%s", g.example.c_str());
				ImGui::PopTextWrapPos();
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

// ==========================================================================================
// crash log: the objects a crash log names, found in the open save
// ==========================================================================================

const std::vector<crashlog::LeveledBase>& App::leveledBases()
{
	if (_leveledSave != _save.get() || _leveledGen != _editGen) {
		_leveledSave = _save.get();
		_leveledGen = _editGen;
		// (Fallout 4: its change forms are not decoded, so its leveled actors are not known)
		_leveledBases = _save && !_save->isFO4() ? crashlog::LeveledBases(*_save) : std::vector<crashlog::LeveledBase>{};
	}
	return _leveledBases;
}

std::pair<std::string, std::uint32_t> App::missingFormHint(std::uint32_t a_formID)
{
	if (!_save || (a_formID >> 24) != 0xFF) {
		return {};
	}
	for (const auto& c : _save->changeForms) {
		if (_save->formIDOf(c.refID) == a_formID) {
			return {};  // it is in the save
		}
	}
	for (const auto& l : leveledBases()) {
		if (l.base == a_formID) {
			return { Fmt(tr("%08X is not stored in this save: it is the base NPC of leveled actor %08X. The game builds it again from its "
						 "template (%s) every time the save loads, so it has no change form. The actor %08X is what the save has."),
						 a_formID, l.actor, formText(l.templ).c_str(), l.actor),
				l.actor };
		}
	}
	return { Fmt(tr("%08X is not in this save. A spawned (FF) form that a crash log names but the save does not have was made by the "
				 "game after the save loaded, usually for an actor or object the save does have. Tools > Crash log reads the whole log "
				 "and finds those."),
				 a_formID),
		0 };
}

// where the crash loggers write: Crash Logger (the SKSE folder), Trainwreck (SKSE\Crashlogs), .NET Script Framework
// (Data\NetScriptFramework\Crash, mostly Legendary Edition); Fallout 4's Buffout 4: Documents\My Games\Fallout4\F4SE
std::vector<std::filesystem::path> App::crashLogFolders()
{
	std::vector<std::filesystem::path> v;
	const char* se = _game == 2 ? "F4SE" : "SKSE";
	auto docs = [&](const std::filesystem::path& a_saves) {
		for (auto d : { a_saves.parent_path() / se, a_saves.parent_path() / se / "Crashlogs" }) {
			if (std::find(v.begin(), v.end(), d) == v.end()) {
				v.push_back(d);
			}
		}
	};
	if (!_path.empty()) {
		docs(_path.parent_path());
	}
	docs(savesFolder());
	docs(platform::DefaultSavesFolder(_game));
	if (_save && _save->isLE()) {
		docs(platform::DefaultSavesFolder(false).parent_path().parent_path() / "Skyrim" / "Saves");
	}
	if (const auto g = gameFolder(); !g.empty()) {
		v.push_back(g / "Data" / "NetScriptFramework" / "Crash");
	}
	return v;
}

std::filesystem::path App::newestCrashLog(std::filesystem::file_time_type* a_time)
{
	if (const char* t = std::getenv("RESAVERPP_TEST_CRASHLOG")) {
		std::error_code ec;
		if (a_time) {
			*a_time = std::filesystem::last_write_time(t, ec);
		}
		return t;
	}
	std::filesystem::path           newest;
	std::filesystem::file_time_type newestTime{};
	for (const auto& d : crashLogFolders()) {
		std::error_code ec;
		for (std::filesystem::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
			const auto name = Lower(platform::ToUtf8(it->path().filename()));
			const auto ext = Lower(platform::ToUtf8(it->path().extension()));
			if (!it->is_regular_file(ec) || name.rfind("crash", 0) != 0 || (ext != ".log" && ext != ".txt")) {
				continue;
			}
			const auto t = it->last_write_time(ec);
			if (newest.empty() || t > newestTime) {
				newest = it->path();
				newestTime = t;
			}
		}
	}
	if (a_time) {
		*a_time = newestTime;
	}
	return newest;
}

const App::CrashCheck& App::crashCheck()
{
	if (_crashCheckSave == _save.get() && _crashCheckGen == _editGen) {
		return _crashCheck;
	}
	_crashCheckSave = _save.get();
	_crashCheckGen = _editGen;
	_crashCheck = {};
	if (!_save || _path.empty() || _save->isFO4()) {
		return _crashCheck;
	}
	// only a crash after this save was made (loading it, or playing on from it)
	std::filesystem::file_time_type logTime{};
	const auto                      file = newestCrashLog(&logTime);
	std::error_code                 ec;
	const auto                      saveTime = std::filesystem::last_write_time(_path, ec);
	if (file.empty() || ec || (logTime < saveTime && !std::getenv("RESAVERPP_TEST_CRASHLOG"))) {
		return _crashCheck;
	}
	const auto log = crashlog::Read(file);
	// the leveled actors take a moment to list on a big save: only when the log names a spawned form at the crash
	bool spawnedAtCrash = false;
	for (const auto& f : log.forms) {
		spawnedAtCrash |= f.atCrash && (f.id >> 24) == 0xFF;
	}
	if (!log.ok || !spawnedAtCrash) {
		return _crashCheck;
	}
	std::set<std::uint32_t> seen;
	for (auto& f : crashlog::Match(log, *_save, leveledBases())) {
		if (f.suspect && seen.insert(f.refRaw).second) {
			_crashCheck.suspects.push_back(std::move(f));
		}
	}
	_crashCheck.file = file;
	_crashCheck.time = log.time;
	return _crashCheck;
}

void App::drawCrashLog()
{
	prepareWindow("Crash log", ImVec2(0.85f, 0.8f));
	if (!ImGui::Begin(trId("Crash log"), &_showCrashLog, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Crash log");
	if (!_clogLoaded) {
		_clogLoaded = true;
		_clog = {};
		_clogSave = nullptr;
		if (const auto newest = newestCrashLog(); !newest.empty()) {
			_clog = crashlog::Read(newest);
		}
	}
	if (_clog.ok && (_clogSave != _save.get() || _clogGen != _editGen)) {
		_clogSave = _save.get();
		_clogGen = _editGen;
		_clogFindings = crashlog::Match(_clog, *_save, leveledBases());
	}

	ImGui::NewLine();
	if (ImGui::Button(tr("Open another log..."))) {
		if (platform::HasNativeDialogs()) {
			if (auto f = platform::OpenFileDialog(crashLogFolders().front(), "log", tr("Crash log"))) {
				_clog = crashlog::Read(*f);
				_clogSave = nullptr;
			}
		}
	}
	tips::Tip(tr("Pick a crash log yourself, e.g. an older one or one from Trainwreck or .NET Script Framework."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Read again"))) {
		_clogLoaded = false;
	}
	tips::Tip(tr("Look for the newest crash log again, e.g. after the game crashed once more."));
	if (!_clog.ok) {
		TextColored(theme::Warn(), !_clog.file.empty() ? tr("Could not read ") + platform::ToUtf8(_clog.file) + ": " + _clog.error
								   : _game == 2
									   ? tr("No crash log found. Buffout 4 (Buffout 4 NG for next-gen and Anniversary) writes them to "
										 "Documents\\My Games\\Fallout4\\F4SE (crash-<date>.log) when the game crashes. Install it, let the game "
										 "crash once, then look again, or pick a log with \"Open another log...\".")
									   : tr("No crash log found. Crash Logger SSE/AE writes them to Documents\\My Games\\Skyrim Special Edition\\SKSE "
										 "(crash-<date>.log) when the game crashes. Install it, let the game crash once, then look again, or "
										 "pick a log with \"Open another log...\"."));
		ImGui::End();
		return;
	}
	ImGui::TextWrapped("%s", platform::ToUtf8(_clog.file).c_str());
	if (_save && _clog.fo4 != _save->isFO4()) {
		TextColored(theme::Warn(), _clog.fo4 ? tr("This is a Fallout 4 crash log, and the save open is a Skyrim save: open the Fallout 4 save it is about.")
											 : tr("This is a Skyrim crash log, and the save open is a Fallout 4 save: open the Skyrim save it is about."));
	}
	ImGui::TextDisabled("%s%s%s", _clog.tool.empty() ? tr("crash log") : _clog.tool.c_str(), _clog.time.empty() ? "" : tr(", crashed "),
		_clog.time.c_str());
	if (!_clog.exception.empty()) {
		ImGui::TextDisabled("%s", _clog.exception.c_str());
	}
	ImGui::Spacing();
	ImGui::TextWrapped("%s", tr("The forms the log names, the ones the game was working on when it crashed first, and what this save has of each. "
					   "A crash while a save loads is often one spawned or leveled NPC whose saved data no longer fits your mods. The log "
					   "may name its base NPC first (an NPC FF......): the game builds that again every time the save loads, so it is not "
					   "in the save, and the actor that uses it is shown instead."));
	ImGui::Spacing();

	std::vector<const crashlog::Finding*> suspects;
	std::set<std::uint32_t>               seen;
	bool                                  anyInSave = false;
	for (const auto& f : _clogFindings) {
		anyInSave |= f.refRaw != 0;
		if (f.suspect && seen.insert(f.refRaw).second) {
			suspects.push_back(&f);
		}
	}
	auto removeOne = [&](const crashlog::Finding& a_f) {
		const auto     raw = a_f.refRaw;
		const auto     id = a_f.target;
		const auto     scripts = a_f.scripts;
		confirm(tr("Remove the actor from the save"),
			Fmt(tr("Delete the saved data of %s actor %08X?\n\nThe game drops it when the save loads; if a mod placed or spawns it, the mod "
				"can make a new one. Nothing else in the save changes.%s\n\nWrite the save with File > Save As (under a new name) and "
				"load that. If the game crashes again, open the new crash log here."),
				a_f.created ? tr("spawned/leveled") : "", id,
				scripts ? Fmt(tr(" Its %zu script instance(s) stay; Clean > Missing spawned objects can remove them later."), scripts).c_str() : ""),
			[this, raw, id] {
				ops::DeleteChangeForms(*_save, { raw });
				markEdited(Fmt(tr("Removed actor %08X (named in the crash log)"), id));
			});
	};

	if (!suspects.empty()) {
		ImGui::SeparatorText(tr("Likely cause"));
		for (const auto* f : suspects) {
			ImGui::PushID(static_cast<int>(f->refRaw));
			std::string name;
			for (const auto& g : _clogFindings) {
				if (g.refRaw == f->refRaw && !g.form.name.empty()) {
					name = g.form.name;
					break;
				}
			}
			TextColored(theme::Caution(), Fmt(tr("Spawned or leveled actor %08X%s: the game was loading it when it crashed."), f->target,
											  name.empty() ? "" : (" (\"" + name + "\")").c_str()));
			if (ImGui::Button(Fmt(tr("Remove %08X from the save"), f->target).c_str())) {
				removeOne(*f);
			}
			tips::Tip(tr("Delete this actor's change form, the same as finding it in Change Forms and deleting it. Back up your save first; "
					  "File > Save As keeps the original."));
			ImGui::SameLine();
			if (ImGui::Button(tr("Show it"))) {
				std::snprintf(_filter, sizeof(_filter), "%08X", f->target);
				select(Cat::kChangeForms, f->refRaw);
				_rowsDirty = true;
			}
			tips::Tip(tr("Open its change form in the main window: everything the save has about it."));
			ImGui::PopID();
		}
		ImGui::Spacing();
	} else if (anyInSave) {
		TextColored(theme::Dim(), tr("No spawned or leveled actor of this save was being loaded when the game crashed. The table shows what "
								  "the log names; a crash in the middle of play is more often a mod's DLL or a mesh than the save."));
	} else {
		TextColored(theme::Warn(), tr("None of the forms this log names are in this save. The log may be from another save or another game "
								   "session: check its date."));
	}

	if (ImGui::BeginTable("clog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Form ID"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 6);
		ImGui::TableSetupColumn(tr("In the log"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
		ImGui::TableSetupColumn(tr("Times"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3.5f);
		ImGui::TableSetupColumn(tr("In this save"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
		ImGui::TableHeadersRow();
		const std::size_t shown = std::min<std::size_t>(_clogFindings.size(), 300);
		for (std::size_t i = 0; i < shown; ++i) {
			const auto& f = _clogFindings[i];
			ImGui::PushID(static_cast<int>(i));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (f.suspect) {
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, (theme::Caution() & 0x00FFFFFF) | 0x30000000);
			}
			ImGui::Text("%08X", f.form.id);
			ImGui::TableNextColumn();
			std::string what = f.form.type;
			if (!f.form.name.empty()) {
				what += (what.empty() ? "\"" : "  \"") + f.form.name + "\"";
			} else if (_db && (f.form.id >> 24) != 0xFF) {
				what += "  " + _db->describe(f.form.id);
			}
			ImGui::TextWrapped("%s", what.c_str());
			if (f.form.atCrash) {
				ImGui::TextDisabled("%s", tr("at the crash"));
			}
			ImGui::TableNextColumn();
			ImGui::Text(tr("%zu"), f.form.hits);
			ImGui::TableNextColumn();
			switch (f.kind) {
			case crashlog::Kind::kChanged:
				ImGui::TextWrapped("%s", Fmt(tr("%s change form%s%s"), f.cfType.c_str(), f.created ? tr(", spawned or leveled") : "",
											 f.scripts ? Fmt(tr(", %zu scripts"), f.scripts).c_str() : "")
											 .c_str());
				break;
			case crashlog::Kind::kRebuiltBase:
				ImGui::TextWrapped("%s", f.refRaw ? Fmt(tr("Not stored: the base NPC of leveled actor %08X, rebuilt at every load. The actor "
														"is in the save (%s)."),
														f.target, f.cfType.c_str())
														.c_str()
												  : tr("Not stored: a leveled actor's base NPC, rebuilt at every load."));
				break;
			case crashlog::Kind::kTemporary:
				ImGui::TextDisabled("%s", tr("Not in the save: made by the game after the save loaded."));
				break;
			case crashlog::Kind::kPlugin:
				ImGui::TextDisabled("%s", tr("Not changed by the save (the plugin's own form)."));
				break;
			}
			ImGui::TableNextColumn();
			if (f.refRaw) {
				if (ImGui::SmallButton(tr("show"))) {
					std::snprintf(_filter, sizeof(_filter), "%08X", f.target);
					select(Cat::kChangeForms, f.refRaw);
					_rowsDirty = true;
				}
				tips::Tip(tr("Open the change form in the main window."));
				if (f.created && f.cfType == "ACHR") {
					ImGui::SameLine();
					if (ImGui::SmallButton(tr("remove"))) {
						removeOne(f);
					}
					tips::Tip(tr("Delete this spawned or leveled actor's change form from the save."));
				}
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

// ==========================================================================================
// saves folder manager
// ==========================================================================================

void App::drawSavesManager()
{
	prepareWindow("Manage saves folder", ImVec2(0.75f, 0.8f));
	if (!ImGui::Begin(trId("Manage saves folder"), &_showSavesMgr, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Manage saves folder");
	const auto folder = !_path.empty() ? _path.parent_path() : savesFolder();
	if (!_savesLoaded) {
		_savesLoaded = true;
		_saves = extras::ListSaves(folder);
		_saveSel.assign(_saves.size(), 0);
	}
	ImGui::NewLine();
	ImGui::TextWrapped(tr("Saves folder: %s"), platform::ToUtf8(folder).c_str());
	ImGui::TextWrapped("%s", tr("Pick old saves to delete. Each save's script extender file (.skse, or .f4se for Fallout 4) goes with it. Deleted saves go "
					   "to the Recycle Bin, so you can still get them back. The save that is open here is never deleted."));
	std::uintmax_t total = 0;
	std::map<std::string, std::vector<std::size_t>> byChar;  // newest first
	for (std::size_t i = 0; i < _saves.size(); ++i) {
		total += _saves[i].size + _saves[i].cosaveSize;
		byChar[_saves[i].ok ? _saves[i].playerName : std::string(tr("(unreadable)"))].push_back(i);
	}
	ImGui::Text(total >= 1073741824 ? tr("%zu saves, %.1f GB") : tr("%zu saves, %.0f MB"), _saves.size(), total >= 1073741824 ? total / 1073741824.0 : total / 1048576.0);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
	ImGui::InputInt("##keep", &_keepNewest);
	_keepNewest = std::clamp(_keepNewest, 0, 9999);
	tips::Tip(tr("How many of each character's newest saves to keep."));
	ImGui::SameLine();
	if (ImGui::Button(Fmt(tr("Tick all but the newest %d of each character"), _keepNewest).c_str())) {
		for (const auto& [c, idx] : byChar) {
			for (std::size_t k = 0; k < idx.size(); ++k) {
				_saveSel[idx[k]] = k >= static_cast<std::size_t>(_keepNewest) ? 1 : 0;
			}
		}
	}
	tips::Tip(tr("Tick every save except each character's newest ones. Nothing is deleted until you press Delete."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Untick all"))) {
		std::fill(_saveSel.begin(), _saveSel.end(), 0);
	}
	tips::Tip(tr("Clear every tick."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Read the folder again"))) {
		_savesLoaded = false;
	}
	tips::Tip(tr("List the saves again (after the game wrote new ones)."));

	std::size_t    ticked = 0;
	std::uintmax_t tickedBytes = 0;
	for (std::size_t i = 0; i < _saves.size(); ++i) {
		if (_saveSel[i]) {
			++ticked;
			tickedBytes += _saves[i].size + _saves[i].cosaveSize;
		}
	}
	if (ImGui::BeginChild("savelist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f), ImGuiChildFlags_Borders)) {
		for (auto& [c, idx] : byChar) {
			std::size_t sel = 0;
			for (auto i : idx) {
				sel += _saveSel[i] ? 1 : 0;
			}
			const bool open = ImGui::CollapsingHeader(Fmt(tr("%s  (%zu saves, %zu ticked)###%s"), text::ToUtf8(c).c_str(), idx.size(), sel, c.c_str()).c_str(),
				byChar.size() == 1 ? ImGuiTreeNodeFlags_DefaultOpen : 0);
			tips::Tip(tr("This character's saves, newest first."));
			if (!open) {
				continue;
			}
			if (ImGui::BeginTable(c.c_str(), 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
				for (auto i : idx) {
					const auto& s = _saves[i];
					const bool  isOpen = !_path.empty() && std::filesystem::equivalent(s.path, _path);
					ImGui::PushID(static_cast<int>(i));
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					bool b = _saveSel[i] != 0;
					ImGui::BeginDisabled(isOpen);
					if (ImGui::Checkbox("##s", &b)) {
						_saveSel[i] = b ? 1 : 0;
					}
					ImGui::EndDisabled();
					if (isOpen) {
						_saveSel[i] = 0;
					}
					ImGui::SameLine();
					ImGui::TextUnformatted(text::ToUtf8(s.path.filename().string()).c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(Date(s.modified).c_str());
					ImGui::TableNextColumn();
					ImGui::Text(tr("level %u"), s.playerLevel);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(text::ToUtf8(s.location).c_str());
					ImGui::TableNextColumn();
					ImGui::Text(tr("%.1f MB%s"), (s.size + s.cosaveSize) / 1048576.0, isOpen ? tr("  (open)") : "");
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}
	}
	ImGui::EndChild();
	ImGui::BeginDisabled(ticked == 0);
	if (ImGui::Button(Fmt(tr("Delete %zu ticked saves (%.1f MB) to the Recycle Bin"), ticked, tickedBytes / 1048576.0).c_str())) {
		confirm(tr("Delete saves"), Fmt(tr("Move %zu saves and their .skse / .f4se files to the Recycle Bin?"), ticked), [this] {
			std::vector<std::filesystem::path> files;
			for (std::size_t i = 0; i < _saves.size(); ++i) {
				if (!_saveSel[i]) {
					continue;
				}
				files.push_back(_saves[i].path);
				if (!_saves[i].cosave.empty()) {
					files.push_back(_saves[i].cosave);  // .skse / .f4se
				}
			}
			if (platform::MoveToRecycleBin(files)) {
				_status = Fmt(tr("Moved %zu files to the Recycle Bin"), files.size());
			} else {
				_error = tr("Some saves could not be deleted (in use, or no permission). The list shows what is left.");
			}
			_savesLoaded = false;
		});
	}
	ImGui::EndDisabled();
	tips::Tip(tr("Move the ticked saves and their .skse / .f4se files to the Recycle Bin."));
	ImGui::End();
}
