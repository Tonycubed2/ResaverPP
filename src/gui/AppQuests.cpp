// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Quests page: every quest in the save with its stages, journal entries and objectives (read from the
// plugins and the save), what its completion stage's script does, and - with ReSaverHelper.dll in the
// running game - completing a quest, setting a stage and refreshing the journal in game.
#include "App.h"

#include "Theme.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <functional>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[2048];
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

	void Dim(const std::string& s) { TextColored(theme::Dim(), s); }

	// "needs the running game" hint on a disabled button
	void LiveHint(const LiveLink::Info& a_info)
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::SetTooltip("%s", !a_info.connected ? "Start Skyrim with ReSaverHelper.dll installed to use this (optional download on the ReSaver Plus page)."
														: "Load a save in the game first.");
		}
	}
}

// ---- data ---------------------------------------------------------------------------------------

const std::vector<quests::QuestState>& App::questStates()
{
	if (_questGen != _editGen) {
		_questStates = _save ? quests::ReadStates(*_save) : std::vector<quests::QuestState>{};
		_questGen = _editGen;
		_questPreviews.clear();
		_chains.clear();
	}
	return _questStates;
}

const App::QuestPreview& App::questPreview(std::uint32_t a_formID)
{
	if (auto it = _questPreviews.find(a_formID); it != _questPreviews.end()) {
		return it->second;
	}
	// not ready yet: answer without keeping the answer, so it is read again once the plugins and scripts are in
	if (!_db) {
		_previewPending = true;
		_previewTmp = QuestPreview{};
		_previewTmp.error = namesLoading() ? "Reading your plugins..." : "The plugins are not read yet (Options > Settings: mods folder, then names are read automatically).";
		return _previewTmp;
	}
	if (!ensureSourcesScanned()) {
		_previewPending = true;
		_previewTmp = QuestPreview{};
		_previewTmp.error = "Reading your mods' scripts (in the background)...";
		return _previewTmp;
	}
	auto& pv = _questPreviews[a_formID];
	pv.done = true;
	const auto* def = _db->quest(a_formID);
	if (!def) {
		pv.error = "This quest is not in your plugins.";
		return pv;
	}
	if (def->fragmentScript.empty()) {
		pv.error = "This quest has no stage scripts.";
		return pv;
	}
	try {
		const auto px = _sources.findPex(def->fragmentScript);
		if (!px) {
			pv.error = "Its stage script " + def->fragmentScript + ".pex was not found in your mods.";
			return pv;
		}
		const auto file = pex::Read(ScriptSources::readPex(*px));
		const auto code = pex::Decompile(file);
		for (auto st : def->completionStages()) {
			if (const auto* fr = def->fragmentFor(st)) {
				pv.bodies.push_back({ st, quests::FunctionBody(code, fr->function) });
			} else {
				pv.bodies.push_back({ st, {} });
			}
		}
	} catch (const std::exception& e) {
		pv.error = std::string("Could not read its stage script: ") + e.what();
	}
	return pv;
}

std::string App::liveNotifyMode() const
{
	switch (_liveNotify) {
	case 1: return "box";
	case 2: return "none";
	default: return "hud";
	}
}

void App::liveQuest(std::uint32_t a_formID)
{
	_live.request({ "QUEST", Fmt("%08X", a_formID) }, [this, a_formID](const LiveLink::Reply& r) {
		auto& lq = _liveQuests[a_formID];
		lq = LiveQuest{};
		if (!LiveLink::Ok(r)) {
			lq.error = LiveLink::Error(r);
			return;
		}
		lq.valid = true;
		lq.editorID = LiveLink::Field(r, 0);
		lq.stage = std::atoi(LiveLink::Field(r, 2).c_str());
		lq.running = LiveLink::Field(r, 3) == "1";
		lq.completed = LiveLink::Field(r, 4) == "1";
	});
}

void App::liveSetStage(std::uint32_t a_formID, std::uint16_t a_stage, const std::string& a_what, std::function<void(bool)> a_then)
{
	const auto msg = "ReSaver++: " + a_what;
	_status = "Asking the game: " + a_what + "...";
	_live.request({ "SETSTAGE", Fmt("%08X", a_formID), std::to_string(a_stage), liveNotifyMode(), msg },
		[this, a_formID, a_what, a_then](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				_status = "The game could not do it: " + LiveLink::Error(r);
				if (a_then) {
					a_then(false);
				}
				return;
			}
			const auto f = LiveLink::Field(r, 0);
			std::string why;
			if (f == "0") {
				// helper 3 also tells where the quest stands after the refusal
				const auto running = LiveLink::Field(r, 1), completed = LiveLink::Field(r, 2), stage = LiveLink::Field(r, 3);
				if (completed == "1") {
					why = " The quest is already completed in the game.";
				} else if (running == "0") {
					why = " The quest could not start: the game could not find the people, places or items it needs (they are set up by "
						  "the quest's normal beginning). \"Mark completed\" can still mark it done in the journal.";
				} else if (!stage.empty()) {
					why = " The quest is running at stage " + stage + "; that stage's conditions were not met. \"Mark completed\" can still mark it done.";
				}
			}
			_status = f == "1" ? "Done in game: " + a_what + ". Save in the game to keep it." :
			          f == "?" ? "The game is still running that stage's script: " + a_what + "." :
			                     "The game refused that stage: " + a_what + "." + why;
			liveQuest(a_formID);
			if (a_then) {
				a_then(f != "0");
			}
		});
}

bool App::helperAtLeast(int a_version) const
{
	const auto info = _live.info();
	return info.connected && std::atoi(info.helperVersion.c_str()) >= a_version;
}

void App::liveComplete(std::uint32_t a_formID, const std::string& a_name)
{
	const std::string what = "marked \"" + a_name + "\" completed";
	_status = "Asking the game: " + what + "...";
	_live.request({ "COMPLETE", Fmt("%08X", a_formID), liveNotifyMode(), "ReSaver++: " + what }, [this, a_formID, what](const LiveLink::Reply& r) {
		if (!LiveLink::Ok(r)) {
			_status = "The game could not do it: " + LiveLink::Error(r);
			return;
		}
		const auto f = LiveLink::Field(r, 0);
		_status = f == "1" ? "Done in game: " + what + ". Save in the game to keep it." :
		          f == "?" ? "The game is busy; it does it as soon as it can: " + what + "." :
		                     "The game did not mark it completed: " + what + ".";
		liveQuest(a_formID);
	});
}

std::vector<App::ChainItem>& App::questChain(std::uint32_t a_formID)
{
	if (auto it = _chains.find(a_formID); it != _chains.end()) {
		return it->second;
	}
	if (!_db) {
		static std::vector<ChainItem> none;
		none.clear();
		return none;  // not cached: the plugins are read later
	}
	std::vector<ChainItem> out;
	_previewPending = false;
	const auto& defs = _db->questDefs();
	std::unordered_map<std::uint32_t, const quests::QuestState*> state;
	for (const auto& q : questStates()) {
		state[q.formID] = &q;
	}
	auto nameOf = [&](std::uint32_t id) {
		const auto* d = _db->quest(id);
		return d ? (d->name.empty() ? d->edid : d->name + " (" + d->edid + ")") : Fmt("%08X", id);
	};
	auto predecessors = [&](std::uint32_t a_target) {
		return quests::LeadsInto(a_target, defs, [this](std::uint32_t id) { return questPreview(id).bodies; });
	};
	// walk back, oldest first; stop at quests already completed
	std::set<std::uint32_t> seen{ a_formID };
	std::function<void(std::uint32_t, int)> walk = [&](std::uint32_t a_target, int a_depth) {
		if (a_depth > 12) {
			return;
		}
		std::vector<std::uint32_t> pre;
		for (auto id : predecessors(a_target)) {
			if (!seen.contains(id)) {
				pre.push_back(id);  // quests already on the list (reached another way) do not count as alternatives
			}
		}
		for (auto id : pre) {
			if (!seen.insert(id).second) {
				continue;
			}
			auto       it = state.find(id);
			const bool done = it != state.end() && it->second->completed();
			if (!done) {
				walk(id, a_depth + 1);  // its own earlier quests come first
			}
			ChainItem c;
			c.formID = id;
			c.stage = _db->quest(id)->completionStages().front();
			c.name = nameOf(id);
			c.done = done;
			c.alternative = pre.size() > 1;
			c.tick = !done && !c.alternative;
			out.push_back(std::move(c));
		}
	};
	walk(a_formID, 0);
	if (_previewPending) {
		// some stage scripts could not be read yet: show what is known, look again next frame
		_chainPending = std::move(out);
		return _chainPending;
	}
	return _chains[a_formID] = std::move(out);
}

void App::runChain()
{
	if (_chainPos >= _chainRun.size()) {
		_status = Fmt("Quest line done in game (%zu quest(s)). Save in the game to keep it.", _chainRun.size());
		_chainRun.clear();
		_chainPos = 0;
		return;
	}
	const auto c = _chainRun[_chainPos];
	liveSetStage(c.formID, c.stage, Fmt("completed \"%s\" (stage %u, %zu of %zu)", c.name.c_str(), c.stage, _chainPos + 1, _chainRun.size()),
		[this](bool ok) {
			if (!ok) {
				_status += "  Stopped the quest line here.";
				_chainRun.clear();
				_chainPos = 0;
				return;
			}
			++_chainPos;
			runChain();
		});
}

void App::drawQuestNotifyChoice()
{
	const char* modes[] = { "Top-left notification", "Message box", "No message" };
	ImGui::SetNextItemWidth(200);
	if (ImGui::Combo("##qnotify", &_liveNotify, modes, 3)) {
		saveSettings();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("How the game tells you what ReSaver++ did (also in Options > Settings > Live game).");
	}
}

// ---- live status (status bar) -------------------------------------------------------------------

void App::drawLiveStatus()
{
	_live.poll();
	const auto info = _live.info();
	if (!info.connected) {
		ImGui::TextDisabled("Live game: not connected");
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Start Skyrim with ReSaverHelper.dll installed to complete quests, set stages and refresh the journal from here.");
		}
		return;
	}
	TextColored(theme::Good(), Fmt("Live game: %s %s%s", info.runtime.c_str(), info.gameVersion.c_str(), info.inGame ? "" : " (main menu)"));
}

// ---- journal refresh (live) -----------------------------------------------------------------------

void App::startLiveRefresh()
{
	_live.request({ "REFRESH", liveNotifyMode() }, [this](const LiveLink::Reply& r) {
		if (!LiveLink::Ok(r)) {
			_status = "The game could not refresh the journal: " + LiveLink::Error(r);
			return;
		}
		const auto n = std::strtoull(LiveLink::Field(r, 0).c_str(), nullptr, 10);
		if (n == 0) {
			_status = "Nothing to refresh: no running quest shows an objective.";
			return;
		}
		_liveRefresh = LiveRefresh{ true, 0, n, 0 };
	});
}

void App::drawRefreshJournal(bool a_compact)
{
	const auto info = _live.info();
	const bool can = info.connected && info.inGame;
	// progress of a refresh started from here (or by the helper on load)
	if (_liveRefresh.running && ImGui::GetTime() > _liveRefresh.nextPoll) {
		_liveRefresh.nextPoll = ImGui::GetTime() + 0.7;
		_live.request({ "PROGRESS" }, [this](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				_liveRefresh.running = false;
				return;
			}
			_liveRefresh.done = std::strtoull(LiveLink::Field(r, 0).c_str(), nullptr, 10);
			_liveRefresh.total = std::strtoull(LiveLink::Field(r, 1).c_str(), nullptr, 10);
			if (LiveLink::Field(r, 2) != "1") {
				_liveRefresh.running = false;
				_status = Fmt("Journal refreshed in game: %zu objective(s). Save in the game to keep it.", _liveRefresh.done);
			}
		});
	}
	ImGui::BeginDisabled(!can || _liveRefresh.running);
	if (ImGui::Button("Refresh journal in game")) {
		startLiveRefresh();
	}
	ImGui::EndDisabled();
	if (!can) {
		LiveHint(info);
	} else if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("For every running quest, each objective the journal shows is hidden and shown again (completed / failed\n"
						  "put back), so the game rebuilds its list. Brings back quests and objectives the journal stopped showing\n"
						  "(the same fix as Quest Journal Limit Bug Fixer).\n\n"
						  "Make a save in the game first and keep menus closed until it is done (helper 4 waits for them).");
	}
	if (_liveRefresh.running) {
		ImGui::SameLine();
		const float frac = _liveRefresh.total ? float(_liveRefresh.done) / float(_liveRefresh.total) : 0.0f;
		ImGui::ProgressBar(frac, ImVec2(a_compact ? 160.0f : 240.0f, 0), Fmt("%zu / %zu", _liveRefresh.done, _liveRefresh.total).c_str());
	}
	// the helper's own option: refresh after every save load
	if (info.connected && !_liveOptionAsked) {
		_liveOptionAsked = true;
		_live.request({ "GETOPTION", "bRefreshJournalOnLoad" }, [this](const LiveLink::Reply& r) {
			_liveAutoRefresh = LiveLink::Ok(r) ? (LiveLink::Field(r, 0) == "1" ? 1 : 0) : -1;
		});
	}
	if (!info.connected) {
		_liveOptionAsked = false;
		_liveAutoRefresh = -1;
	}
	if (!a_compact) {
		ImGui::SameLine();
	}
	bool autoOn = _liveAutoRefresh == 1;
	ImGui::BeginDisabled(!info.connected || _liveAutoRefresh < 0);
	if (ImGui::Checkbox("Refresh automatically after every save load", &autoOn)) {
		_live.request({ "SETOPTION", "bRefreshJournalOnLoad", autoOn ? "1" : "0" }, [this, autoOn](const LiveLink::Reply& r) {
			if (LiveLink::Ok(r)) {
				_liveAutoRefresh = autoOn ? 1 : 0;
				_status = autoOn ? "ReSaverHelper will refresh the journal after every save load (saved in ReSaverHelper.ini)."
				                 : "ReSaverHelper will no longer refresh the journal on load.";
			} else {
				_status = "Could not change the helper's setting: " + LiveLink::Error(r);
			}
		});
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("%s", info.connected ? "Saved in ReSaverHelper.ini (bRefreshJournalOnLoad). The helper waits a few seconds after the load,\n"
											   "then refreshes and tells you when it is done."
											   : "Start the game with ReSaverHelper.dll to change this, or set bRefreshJournalOnLoad=1 in ReSaverHelper.ini.");
	}
}

// ---- list -----------------------------------------------------------------------------------------

void App::drawQuestsList()
{
	const auto& states = questStates();
	std::size_t shown = 0, shownDone = 0;
	for (const auto& q : states) {
		const auto n = q.shownObjectives();
		shown += n;
		if (q.completed() || !q.running()) {
			shownDone += n;
		}
	}
	ImGui::TextUnformatted(Fmt("Your journal shows %zu objective(s).", shown).c_str());
	if (shownDone) {
		ImGui::SameLine();
		TextColored(theme::Warn(), Fmt("%zu of them belong to quests that are completed or stopped.", shownDone));
	}
	drawRefreshJournal(true);
	if (!_db) {
		Dim(namesLoading() ? "Reading your plugins for quest names, stages and journal text..."
						   : "Quest names, stages and journal text come from your plugins: set your mods folder in Options > Settings.");
	}
	ImGui::Separator();

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Show");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	const char* shows[] = { "In the journal", "Running", "Completed", "All in this save", "Every quest in the game" };
	ImGui::Combo("##qshow", &_questShow, shows, 5);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Search");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(_questFilter[0] ? -60.0f : -1.0f);
	ImGui::InputTextWithHint("##qfilter", "any word of the quest name, its editor ID or form ID", _questFilter, sizeof(_questFilter));
	if (_questFilter[0]) {
		ImGui::SameLine();
		if (ImGui::Button("Clear", ImVec2(-1, 0))) {
			_questFilter[0] = 0;
		}
	}
	if (_questShow == 4 && !_db) {
		Dim("Every quest in the game comes from your plugins: they are not read yet.");
	}

	struct Row
	{
		std::uint32_t             formID = 0;
		const quests::QuestState* q = nullptr;  // null: never started in this save
		const quests::QuestDef*   def = nullptr;
		std::string               label, status;
		int                       stage = -1;
		std::size_t               shown = 0;
	};
	std::vector<Row> rows;
	const auto       needle = Lower(_questFilter);
	auto             add = [&](std::uint32_t a_formID, const quests::QuestState* q) {
        const auto* def = _db ? _db->quest(a_formID) : nullptr;
        if (q) {
            switch (_questShow) {
            case 0:
                if (q->shownObjectives() == 0) {
                    return;
                }
                break;
            case 1:
                if (!q->running() || q->completed()) {
                    return;
                }
                break;
            case 2:
                if (!q->completed()) {
                    return;
                }
                break;
            default:
                break;
            }
        }
        // name and editor ID from the plugins; until they are read, the plugin the quest comes from
        std::string label = def ? (def->name.empty() ? def->edid : def->name + "  (" + def->edid + ")") : "(" + _save->pluginOf(a_formID) + ")";
        if (!needle.empty()) {
            // every word typed must appear somewhere in the name, editor ID or form ID
            const auto hay = Lower(label) + " " + Lower(Fmt("%08X", a_formID));
            std::size_t pos = 0;
            while (pos < needle.size()) {
                const auto start = needle.find_first_not_of(' ', pos);
                if (start == std::string::npos) {
                    break;
                }
                const auto end = std::min(needle.find(' ', start), needle.size());
                if (hay.find(needle.substr(start, end - start)) == std::string::npos) {
                    return;
                }
                pos = end;
            }
        }
        Row r;
        r.formID = a_formID;
        r.q = q;
        r.def = def;
        r.label = std::move(label);
        r.status = q ? q->statusText() : "not started (not in this save)";
        r.stage = q ? q->highestDoneStage() : -1;
        r.shown = q ? q->shownObjectives() : 0;
        rows.push_back(std::move(r));
	};
	std::unordered_map<std::uint32_t, const quests::QuestState*> byID;
	for (const auto& q : states) {
		byID[q.formID] = &q;
	}
	if (_questShow == 4 && _db) {
		// every QUST record of the load order, started or not
		for (const auto& [id, def] : _db->questDefs()) {
			auto it = byID.find(id);
			add(id, it == byID.end() ? nullptr : it->second);
		}
	} else {
		for (const auto& q : states) {
			add(q.formID, &q);
		}
	}

	if (ImGui::BeginTable("quests", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
										   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Quest", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 4.0f, 0);
		ImGui::TableSetupColumn("Form ID", ImGuiTableColumnFlags_WidthStretch, 1.0f, 1);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.6f, 2);
		ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 0.7f, 3);
		ImGui::TableSetupColumn("Shown", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 0.7f, 4);
		ImGui::TableHeadersRow();
		// click a column header to sort by it (again to reverse); ties keep the name order
		int  col = 0;
		bool asc = true;
		if (const ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
			col = static_cast<int>(specs->Specs[0].ColumnUserID);
			asc = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
		}
		std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return Lower(a.label) < Lower(b.label); });
		std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
			auto cmp = [&]() -> int {
				switch (col) {
				case 1: return a.formID < b.formID ? -1 : a.formID > b.formID ? 1 : 0;
				case 2: return a.status < b.status ? -1 : a.status > b.status ? 1 : 0;
				case 3: return a.stage < b.stage ? -1 : a.stage > b.stage ? 1 : 0;
				case 4: return a.shown < b.shown ? -1 : a.shown > b.shown ? 1 : 0;
				default:
					{
						const auto la = Lower(a.label), lb = Lower(b.label);
						return la < lb ? -1 : la > lb ? 1 : 0;
					}
				}
			}();
			return asc ? cmp < 0 : cmp > 0;
		});
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(rows.size()));
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
				const auto& r = rows[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushID(static_cast<int>(r.formID));
				if (ImGui::Selectable(r.label.c_str(), _questSel == r.formID, ImGuiSelectableFlags_SpanAllColumns)) {
					_questSel = r.formID;
					_setStageIdx = 0;
					if (_live.info().inGame) {
						liveQuest(_questSel);
					}
				}
				ImGui::PopID();
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%08X", r.formID);
				ImGui::TableNextColumn();
				const bool odd = r.q && r.shown && (r.q->completed() || !r.q->running());
				if (odd) {
					TextColored(theme::Warn(), r.status);
				} else if (!r.q) {
					ImGui::TextDisabled("%s", r.status.c_str());
				} else {
					ImGui::TextUnformatted(r.status.c_str());
				}
				ImGui::TableNextColumn();
				if (r.stage >= 0) {
					ImGui::Text("%d", r.stage);
				}
				ImGui::TableNextColumn();
				if (r.shown) {
					ImGui::Text("%zu", r.shown);
				}
			}
		}
		ImGui::EndTable();
	}
}

// ---- detail ---------------------------------------------------------------------------------------

void App::drawQuestDetail()
{
	const auto& states = questStates();
	const quests::QuestState* q = nullptr;
	for (const auto& s : states) {
		if (s.formID == _questSel) {
			q = &s;
		}
	}
	quests::QuestState notStarted;  // a quest of the load order this save never touched
	if (!q && _db && _db->quest(_questSel)) {
		notStarted.formID = _questSel;
		q = &notStarted;
	}
	if (!q) {
		Dim("Click a quest to see its stages, journal entries and objectives, and what completing it gives.");
		return;
	}
	const auto* def = _db ? _db->quest(q->formID) : nullptr;
	const auto  info = _live.info();
	const bool  live = info.connected && info.inGame;

	ImGui::TextUnformatted(def && !def->name.empty() ? def->name.c_str() : (def ? def->edid.c_str() : Fmt("%08X", q->formID).c_str()));
	Dim(Fmt("%s  |  %08X  |  %s", def ? def->edid.c_str() : "?", q->formID, def ? _db->plugins()[def->plugin].c_str() : _save->pluginOf(q->formID).c_str()));
	ImGui::Text("In this save: %s, highest stage done %d, %zu objective(s) shown in the journal", q->statusText(), q->highestDoneStage(), q->shownObjectives());
	if (q->journalKnown) {
		std::string js;
		for (const auto& o : q->journal) {
			js += (js.empty() ? "" : ",  ") + Fmt("objective %d %s", o.index, quests::ObjectiveStateText(o.state));
		}
		Dim(js.empty() ? std::string("The journal does not list it.") : "The journal lists: " + js);
	}
	if (live) {
		auto it = _liveQuests.find(q->formID);
		if (it != _liveQuests.end() && it->second.valid) {
			TextColored(theme::Good(), Fmt("In the running game: stage %d, %s", it->second.stage,
										   it->second.completed ? "completed" : it->second.running ? "running" : "not running"));
		} else if (it != _liveQuests.end()) {
			Dim("In the running game: " + it->second.error);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Check in game")) {
			liveQuest(q->formID);
		}
	}

	// ---- actions
	const auto completion = def ? def->completionStages() : std::vector<std::uint16_t>{};
	const std::string name = def ? (def->name.empty() ? def->edid : def->name) : Fmt("%08X", q->formID);
	ImGui::Spacing();
	ImGui::BeginDisabled(!live || completion.empty());
	if (ImGui::Button("Complete quest in game")) {
		const auto st = completion.front();
		const auto& pv = questPreview(q->formID);
		std::string effects;
		for (const auto& [s, body] : pv.bodies) {
			if (s != st) {
				continue;
			}
			for (const auto& e : quests::ScanEffects(body)) {
				effects += "\n    " + e.line;
			}
		}
		std::string earlier;
		for (const auto& c : questChain(q->formID)) {
			if (!c.done) {
				earlier += "\n    " + c.name;
			}
		}
		if (!earlier.empty()) {
			earlier = "\n\nEarlier quests of this quest line are not completed:" + earlier +
					  "\nThis button completes only this quest. To complete them too, use \"Complete ... earlier quest(s) and this one\" below.";
		}
		confirm("Complete quest",
			Fmt("Set \"%s\" to its completion stage %u in the running game?\n\nThe game runs that stage's own script, so its rewards are given and "
				"follow-up quests start the normal way.%s\n\nSkipping a quest's earlier stages can leave things unfinished (items not "
				"collected, people not met). Make a save in the game first.%s",
				name.c_str(), st, effects.empty() ? "" : ("\n\nWhat the stage script does:" + effects).c_str(),
				q->hasFlags ? "" :
				              "\n\nThis quest has not started in this save: setting the stage starts it and completes it at once. Its people and "
				              "places are not set up yet, so parts of its script may do nothing, and quests that come after it may still start.") + earlier,
			[this, fid = q->formID, st, name] { liveSetStage(fid, st, Fmt("completed \"%s\" (stage %u)", name.c_str(), st)); });
	}
	ImGui::EndDisabled();
	if (!live) {
		LiveHint(info);
	} else if (completion.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("No stage of this quest is marked as completing it.");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!live || !helperAtLeast(3));
	if (ImGui::Button("Mark completed")) {
		confirm("Mark completed",
			Fmt("Mark \"%s\" as completed in the running game, without running its stage script?\n\nIts objectives are completed and it moves to "
				"the completed quests in the journal. Unlike \"Complete quest in game\", no rewards are given and quests that normally "
				"follow it are NOT started. Use it when the game refuses the completion stage. Make a save in the game first.",
				name.c_str()),
			[this, fid = q->formID, name] { liveComplete(fid, name); });
	}
	ImGui::EndDisabled();
	if (!live) {
		LiveHint(info);
	} else if (!helperAtLeast(3) && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("Needs ReSaverHelper version 3 (you have version %s).", info.helperVersion.c_str());
	}
	ImGui::SameLine();
	// set any stage
	std::vector<std::uint16_t> stageList;
	if (def) {
		for (const auto& s : def->stages) {
			stageList.push_back(s.index);
		}
	}
	_setStageIdx = std::clamp(_setStageIdx, 0, std::max(0, static_cast<int>(stageList.size()) - 1));
	ImGui::SetNextItemWidth(110);
	ImGui::BeginDisabled(stageList.empty());
	if (ImGui::BeginCombo("##stage", stageList.empty() ? "stage" : Fmt("stage %u", stageList[_setStageIdx]).c_str())) {
		for (int i = 0; i < static_cast<int>(stageList.size()); ++i) {
			if (ImGui::Selectable(Fmt("stage %u", stageList[i]).c_str(), i == _setStageIdx)) {
				_setStageIdx = i;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!live || stageList.empty());
	if (ImGui::Button("Set stage in game") && !stageList.empty()) {
		const auto st = stageList[_setStageIdx];
		confirm("Set stage", Fmt("Set \"%s\" to stage %u in the running game? The stage's script runs. Make a save in the game first.", name.c_str(), st),
			[this, fid = q->formID, st, name] { liveSetStage(fid, st, Fmt("\"%s\" set to stage %u", name.c_str(), st)); });
	}
	ImGui::EndDisabled();
	if (!live) {
		LiveHint(info);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!def || def->edid.empty() || (completion.empty() && stageList.empty()));
	if (ImGui::Button("Copy console command") && def) {
		const auto st = completion.empty() ? stageList[_setStageIdx] : completion.front();
		ImGui::SetClipboardText(Fmt("setstage %s %u", def->edid.c_str(), st).c_str());
		_status = Fmt("Copied: setstage %s %u  (paste it into the game's console with Ctrl+V)", def->edid.c_str(), st);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("Copies the console command that completes the quest (its completion stage), or sets the stage picked on the left.");
	}
	ImGui::TextDisabled("The game confirms with:");
	ImGui::SameLine();
	drawQuestNotifyChoice();

	if (!def) {
		ImGui::Spacing();
		Dim("Stages, journal entries and objectives are read from your plugins once your mods folder is set.");
		return;
	}

	// ---- earlier quests of the quest line
	if (!completion.empty()) {
		auto&       chain = questChain(q->formID);
		std::size_t open = 0;
		for (const auto& c : chain) {
			open += c.done ? 0 : 1;
		}
		if (open) {
			ImGui::SeparatorText(Fmt("Earlier quests in this quest line (%zu not completed)", open).c_str());
			Dim("These quests lead to this one: their journal names it as the next quest, or their last stage starts it. Ticked ones are "
				"completed first, oldest first, then this quest. Quests that are one of several ways to get here (two factions, two "
				"endings) are not ticked: tick the one you want.");
			for (auto& c : chain) {
				if (c.done) {
					continue;
				}
				ImGui::PushID(static_cast<int>(c.formID));
				ImGui::Checkbox(c.name.c_str(), &c.tick);
				ImGui::SameLine();
				ImGui::TextDisabled("stage %u%s", c.stage, c.alternative ? "   (one of several ways here)" : "");
				ImGui::PopID();
			}
			std::vector<ChainItem> run;
			for (const auto& c : chain) {
				if (c.tick && !c.done) {
					run.push_back(c);
				}
			}
			ChainItem self;
			self.formID = q->formID;
			self.stage = completion.front();
			self.name = name;
			run.push_back(self);
			ImGui::BeginDisabled(!live || run.size() < 2 || !_chainRun.empty());
			if (ImGui::Button(Fmt("Complete %zu earlier quest(s) and this one in game", run.size() - 1).c_str())) {
				std::string list;
				for (const auto& c : run) {
					list += Fmt("\n    %s (stage %u)", c.name.c_str(), c.stage);
				}
				confirm("Complete quest line",
					"Complete these quests in the running game, one after the other, in this order?" + list +
						"\n\nEach stage's own script runs (rewards, follow-up quests). If the game refuses a stage, it stops there. "
						"Skipping quests can leave things unfinished. Make a save in the game first.",
					[this, run] {
						_chainRun = run;
						_chainPos = 0;
						runChain();
					});
			}
			ImGui::EndDisabled();
			if (!live) {
				LiveHint(info);
			}
			if (!_chainRun.empty()) {
				ImGui::SameLine();
				ImGui::TextDisabled("working: %zu of %zu", _chainPos + 1, _chainRun.size());
			}
		} else if (_previewPending) {
			Dim("Looking for earlier quests of this quest line (your mods' scripts are still being read)...");
		}
	}

	// ---- what completing it does
	if (!completion.empty()) {
		ImGui::SeparatorText("What completing it does");
		const auto& pv = questPreview(q->formID);
		if (!pv.error.empty()) {
			Dim(pv.error);
		}
		for (const auto& [st, body] : pv.bodies) {
			ImGui::Text("Stage %u completes the quest.", st);
			const auto effects = quests::ScanEffects(body);
			if (body.empty()) {
				Dim("  Its stage has no script.");
			} else if (effects.empty()) {
				Dim("  Its script gives nothing directly (rewards, if any, come from other scripts or dialogue).");
			}
			for (const auto& e : effects) {
				const auto col = e.kind == quests::Effect::Kind::kReward ? theme::Good() : e.kind == quests::Effect::Kind::kQuest ? theme::Warn() : theme::Dim();
				TextColored(col, (e.kind == quests::Effect::Kind::kReward ? "  gives / changes:  " : e.kind == quests::Effect::Kind::kQuest ? "  quests:  " : "  calls:  ") + e.line);
			}
			if (!body.empty() && ImGui::TreeNode(Fmt("##body%u", st).c_str(), "Its full script (%zu lines)", body.size())) {
				if (monoFont) {
					ImGui::PushFont(monoFont);
				}
				for (const auto& l : body) {
					ImGui::TextUnformatted(l.c_str());
				}
				if (monoFont) {
					ImGui::PopFont();
				}
				ImGui::TreePop();
			}
		}
		// the properties the script uses, so names like JarlRewardLightArmor can be looked up
		if (!def->properties.empty() && ImGui::TreeNode("props", "Properties of its stage script (%zu)", def->properties.size())) {
			for (const auto& p : def->properties) {
				std::string v = p.type == 1 ? formText(p.form) : p.text;
				if (p.type == 1 && p.alias >= 0) {
					v = Fmt("alias %d of this quest", p.alias);
				}
				ImGui::BulletText("%s = %s", p.name.c_str(), v.c_str());
			}
			ImGui::TreePop();
		}
	}

	// ---- stages
	ImGui::SeparatorText(Fmt("Stages (%zu)", def->stages.size()).c_str());
	if (ImGui::BeginTable("stages", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 0.7f);
		ImGui::TableSetupColumn("Done", ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn("Journal entry", ImGuiTableColumnFlags_WidthStretch, 6.0f);
		ImGui::TableHeadersRow();
		for (const auto& s : def->stages) {
			bool done = false;
			for (const auto& [st, d] : q->stages) {
				done |= st == static_cast<std::int16_t>(s.index) && d;
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%u", s.index);
			ImGui::TableNextColumn();
			if (done) {
				TextColored(theme::Good(), "done");
			}
			ImGui::TableNextColumn();
			std::string text;
			for (const auto& l : s.logs) {
				if (!l.text.empty()) {
					text += (text.empty() ? "" : "\n") + l.text;
				}
			}
			if (s.completes()) {
				TextColored(theme::Good(), "[completes the quest]");
			}
			if (s.fails()) {
				TextColored(theme::Bad(), "[fails the quest]");
			}
			if (!text.empty()) {
				ImGui::TextWrapped("%s", text.c_str());
			}
		}
		ImGui::EndTable();
	}

	// ---- objectives
	ImGui::SeparatorText(Fmt("Objectives (%zu)", def->objectives.size()).c_str());
	if (ImGui::BeginTable("objs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthStretch, 0.5f);
		ImGui::TableSetupColumn("In this save", ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn("Objective", ImGuiTableColumnFlags_WidthStretch, 5.0f);
		ImGui::TableHeadersRow();
		for (const auto& o : def->objectives) {
			int state = 0;
			for (const auto& os : q->objectives) {
				if (os.index == o.index) {
					state = os.state;
				}
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%u", o.index);
			ImGui::TableNextColumn();
			const bool shown = state == 1 || state == 3 || state == 5;
			if (shown && (q->completed() || !q->running())) {
				TextColored(theme::Warn(), quests::ObjectiveStateText(state));
			} else {
				ImGui::TextUnformatted(quests::ObjectiveStateText(state));
			}
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", o.text.c_str());
		}
		ImGui::EndTable();
	}
}
