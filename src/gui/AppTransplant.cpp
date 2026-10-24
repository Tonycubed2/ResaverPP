// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Transplant page: bring a character's progress from a bad save into a good one. The bad save is
// read through the game (ReSaverHelper's snapshot, complete) or from its file (partial); the good one
// is the save loaded in the game now. What the bad save has and the good one does not is listed,
// coloured by risk; the ticked things are put into the running game one by one by ReSaverHelper, and
// the player saves in the game.
#include "App.h"

#include "Platform.h"
#include "Theme.h"

#include "imgui.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <cctype>
#include <cstdio>

using namespace ess;
namespace tp = ess::transplant;

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
		ImGui::TextUnformatted(s.c_str());
		ImGui::PopStyleColor();
	}

	void Wrapped(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	std::uint32_t RiskCol(int r) { return r == 0 ? theme::Good() : r == 1 ? theme::Caution() : theme::Bad(); }

	void Dot(std::uint32_t col)
	{
		const float sz = ImGui::GetTextLineHeight();
		const auto  p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + sz * 0.5f, p.y + sz * 0.55f), sz * 0.33f, col);
		ImGui::Dummy(ImVec2(sz, sz));
		ImGui::SameLine();
	}

	std::string Summary(const tp::Snapshot& s)
	{
		if (!s.valid) {
			return s.error.empty() ? "not chosen yet" : s.error;
		}
		std::size_t items = 0, done = 0;
		for (const auto& i : s.items) {
			items += static_cast<std::size_t>(std::max(0, i.count));
		}
		for (const auto& [k, q] : s.quests) {
			done += q.completed ? 1 : 0;
		}
		return Fmt("%s%s%s: %zu quests completed, %zu items%s", s.player.empty() ? "" : (s.player + " ").c_str(), s.level > 0 ? Fmt("level %d", s.level).c_str() : "",
			s.fromFile ? "  (read from the save file)" : "  (from the game)", done, items,
			s.fromFile ? "" : Fmt(", %zu perks, %zu spells", s.perks.size(), s.spells.size()).c_str());
	}
}

// ---- getting the two snapshots --------------------------------------------------------------------

void App::transplantSnapshot(bool a_bad)
{
	_status = "Asking the game for a snapshot of the loaded save...";
	_live.request({ "SNAPSHOT" }, [this, a_bad](const LiveLink::Reply& r) {
		if (!LiveLink::Ok(r)) {
			_status = "The game could not take a snapshot: " + LiveLink::Error(r);
			return;
		}
		auto s = tp::ReadSnapshot(platform::FromUtf8(LiveLink::Field(r, 0)));
		if (!s.valid) {
			_status = s.error;
			return;
		}
		_status = Fmt("Snapshot of %s taken (%s records).", s.player.c_str(), LiveLink::Field(r, 1).c_str());
		(a_bad ? _tpBad : _tpGood) = std::move(s);
		_tpDirty = true;
	});
}

void App::transplantFromFile(const std::filesystem::path& a_path)
{
	try {
		if (Lower(platform::ToUtf8(a_path.extension())) == ".txt") {
			_tpBad = tp::ReadSnapshot(a_path);
		} else {
			auto save = SaveFile::load(a_path);
			_tpBad = tp::FromSave(*save, platform::ToUtf8(a_path.filename()));
			transplantNames(_tpBad, *save);
		}
		_tpDirty = true;
		_status = _tpBad.valid ? "Read " + platform::ToUtf8(a_path.filename()) : _tpBad.error;
	} catch (const std::exception& e) {
		_error = "Could not read " + platform::ToUtf8(a_path.filename()) + ":\n\n" + e.what();
	}
}

// names for a snapshot read from a save file (the file has form IDs only)
void App::transplantNames(tp::Snapshot& a_snap, const SaveFile& a_save)
{
	if (!_db || !_save || _save->plugins.full != a_save.plugins.full) {
		return;  // the plugin names read belong to another load order
	}
	auto nameOf = [&](const tp::Record& r) {
		const auto id = static_cast<std::uint32_t>(std::strtoul((r.size() > 3 ? r[3] : std::string()).c_str(), nullptr, 16));
		const auto* f = _db->find(id);
		if (!f) {
			return std::string();
		}
		auto n = _db->name(*f);
		return n.empty() ? _db->edid(*f) : n;
	};
	for (auto* m : { &a_snap.perks, &a_snap.spells, &a_snap.shouts, &a_snap.words, &a_snap.factions, &a_snap.markers }) {
		for (auto& [k, e] : *m) {
			if (e.name.empty()) {
				e.name = nameOf(e.record);
				if (e.record.size() > 4) {
					e.record[4] = e.name;
				}
			}
		}
	}
	for (auto& it : a_snap.items) {
		if (it.name.empty()) {
			it.name = nameOf(it.record);
			if (it.record.size() > 4) {
				it.record[4] = it.name;
			}
		}
	}
	for (auto& [k, q] : a_snap.quests) {
		if (const auto* d = _db->quest(q.runtime)) {
			q.edid = d->edid;
			q.name = d->name;
		}
	}
}

// ---- applying -------------------------------------------------------------------------------------

std::int32_t App::completionStageOf(const tp::Quest& a_q) const
{
	// the quest's completion stage, from the plugins (the open save's load order must hold the quest)
	if (!_db || !_save) {
		return -1;
	}
	for (const auto& [id, def] : _db->questDefs()) {
		const auto local = (id >> 24) == 0xFE ? (id & 0xFFF) : (id & 0xFFFFFF);
		if (local == a_q.local && Lower(_save->pluginOf(id)) == Lower(a_q.plugin)) {
			const auto st = def.completionStages();
			return st.empty() ? -1 : st.front();
		}
	}
	return -1;
}

void App::transplantStep()
{
	if (_tpPos >= _tpRun.size()) {
		std::size_t failed = 0;
		for (const auto& r : _tpResults) {
			failed += r.ok ? 0 : 1;
		}
		_status = Fmt("Transplant done: %zu of %zu brought over%s. Save in the game to keep it.", _tpRun.size() - failed, _tpRun.size(),
			failed ? Fmt(", %zu could not be (see the Transplant page)", failed).c_str() : "");
		_live.request({ "NOTIFY", liveNotifyMode(), Fmt("ReSaver++: transplant done (%zu of %zu). Save your game now.", _tpRun.size() - failed, _tpRun.size()) },
			[](const LiveLink::Reply&) {});
		_tpRunning = false;
		return;
	}
	const auto idx = _tpRun[_tpPos];
	if (idx >= _tpChanges.size()) {
		++_tpPos;
		transplantStep();
		return;
	}
	const auto& ch = _tpChanges[idx];
	_status = Fmt("Transplanting %zu of %zu: %s", _tpPos + 1, _tpRun.size(), ch.label.c_str());
	auto done = [this, idx](bool a_ok, const std::string& a_why) {
		_tpResults.push_back({ idx, a_ok, a_why });
		++_tpPos;
		transplantStep();
	};

	if (!ch.quest) {
		std::vector<std::string> cmd{ "APPLY" };
		cmd.insert(cmd.end(), ch.record.begin(), ch.record.end());
		_live.request(cmd, [done](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				done(false, LiveLink::Error(r));
			} else if (LiveLink::Field(r, 0) == "0") {
				done(false, LiveLink::Field(r, 1));
			} else {
				done(true, LiveLink::Field(r, 0) == "?" ? "the game is still doing it" : "");
			}
		});
		return;
	}

	// a quest: find it in the game, then its completion stage (or the stage it had), else mark it completed
	const auto* q = ch.quest;
	const bool  complete = ch.complete;
	auto        run = [this, q, complete, done](std::uint32_t a_id) {
        const int stage = complete ? completionStageOf(*q) : q->stage;
        auto      mark = [this, a_id, done](const std::string& a_before) {
            _live.request({ "COMPLETE", Fmt("%08X", a_id), "none", "" }, [done, a_before](const LiveLink::Reply& r) {
                const bool ok = LiveLink::Ok(r) && LiveLink::Field(r, 0) != "0";
                done(ok, ok ? a_before + "marked completed (no rewards, follow-up quests not started)"
                            : a_before + "could not be marked completed either");
            });
        };
        if (stage < 0) {
            if (complete) {
                mark("its completion stage is not known (plugins not read): ");
            } else {
                done(false, "no stage to set");
            }
            return;
        }
        _live.request({ "SETSTAGE", Fmt("%08X", a_id), std::to_string(stage), "none", "" }, [complete, mark, done, stage](const LiveLink::Reply& r) {
            if (!LiveLink::Ok(r)) {
                done(false, LiveLink::Error(r));
                return;
            }
            const auto f = LiveLink::Field(r, 0);
            if (f == "0") {
                if (complete) {
                    mark(Fmt("the game refused stage %d, so it was ", stage));
                } else {
                    done(false, Fmt("the game refused stage %d", stage));
                }
                return;
            }
            done(true, f == "?" ? "the game is still running the stage's script" : "");
        });
	};
	if (ch.goodRuntime) {
		run(ch.goodRuntime);
	} else {
		_live.request({ "RESOLVE", q->plugin, Fmt("%06X", q->local) }, [run, done](const LiveLink::Reply& r) {
			if (!LiveLink::Ok(r)) {
				done(false, LiveLink::Error(r));
				return;
			}
			run(static_cast<std::uint32_t>(std::strtoul(LiveLink::Field(r, 0).c_str(), nullptr, 16)));
		});
	}
}

// quests in quest-line order: a quest that leads into another one (its journal names the next quest, or
// its last stage starts it) is done first. Needs the plugins read for the open save's load order.
void App::transplantOrderQuests(std::vector<std::size_t>& a_run)
{
	if (!_db || !_save) {
		return;
	}
	// the open save's form ID of each quest in the run
	std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> idOf;
	for (const auto& [id, def] : _db->questDefs()) {
		const auto local = (id >> 24) == 0xFE ? (id & 0xFFF) : (id & 0xFFFFFF);
		idOf[{ Lower(_save->pluginOf(id)), local }] = id;
	}
	std::vector<std::size_t>                  quests;
	std::map<std::uint32_t, std::size_t>      byId;
	std::vector<std::size_t>                  rest;
	for (auto i : a_run) {
		const auto* q = _tpChanges[i].quest;
		if (!q) {
			rest.push_back(i);
			continue;
		}
		quests.push_back(i);
		if (auto it = idOf.find({ Lower(q->plugin), q->local }); it != idOf.end()) {
			byId[it->second] = i;
		}
	}
	const auto&                                     defs = _db->questDefs();
	std::map<std::size_t, std::vector<std::size_t>> before;  // change -> changes to run before it
	for (const auto& [id, i] : byId) {
		for (auto p : quests::LeadsInto(id, defs, [this](std::uint32_t q) { return questPreview(q).bodies; })) {
			if (auto it = byId.find(p); it != byId.end() && it->second != i) {
				before[i].push_back(it->second);
			}
		}
	}
	std::vector<std::size_t> ordered;
	std::set<std::size_t>    placed, visiting;
	std::function<void(std::size_t)> place = [&](std::size_t i) {
		if (placed.contains(i) || visiting.contains(i)) {
			return;  // (a loop in the quest data: keep the order found)
		}
		visiting.insert(i);
		for (auto p : before[i]) {
			place(p);
		}
		visiting.erase(i);
		placed.insert(i);
		ordered.push_back(i);
	};
	for (auto i : quests) {
		place(i);
	}
	a_run = rest;
	a_run.insert(a_run.end(), ordered.begin(), ordered.end());
}

// ---- the window -----------------------------------------------------------------------------------

void App::drawTransplant()
{
	ImGui::SetNextWindowSize(ImVec2(1000, 720), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Transplant", &_showTransplant)) {
		ImGui::End();
		return;
	}
	const auto info = _live.info();
	const bool live = info.connected && info.inGame;
	const bool helperOk = helperAtLeast(5);

	Wrapped(ImGui::GetColorU32(ImGuiCol_Text),
		"Bring your character's progress from a bad save (unstable, too many mods added and removed) into a good one from before. "
		"ReSaver++ lists what the bad save has and the good one does not; you tick what to bring; the game itself puts it into the "
		"good save, the normal way (scripts run, rewards are given), so the result is a clean save written by the game.");
	ImGui::TextDisabled("Needs Skyrim running with ReSaverHelper.dll version 5 or newer.");
	if (!helperOk) {
		Wrapped(theme::Caution(), info.connected ? Fmt("ReSaverHelper %s is running: version 5 or newer is needed (Optional files on the mod page).", info.helperVersion.c_str())
												 : "ReSaverHelper is not connected: start Skyrim with ReSaverHelper.dll (version 5 or newer) installed.");
	}

	// step 1: the bad save
	ImGui::SeparatorText("1. The bad save: take its progress");
	ImGui::TextWrapped("%s", Summary(_tpBad).c_str());
	ImGui::BeginDisabled(!live || !helperOk || _tpRunning);
	if (ImGui::Button("Snapshot the game (bad save loaded)")) {
		confirm("Snapshot the bad save", "Is the BAD save loaded in the game right now?\n\nReSaverHelper writes down everything about your character "
										 "in the loaded game: level, skills, perks, spells, shouts, items, quests, map locations.",
			[this] { transplantSnapshot(true); });
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(_tpRunning);
	if (ImGui::Button("Read it from a save file...") && platform::HasNativeDialogs()) {
		if (auto p = platform::OpenSaveDialog(savesFolder())) {
			transplantFromFile(*p);
		}
	}
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("For a bad save the game cannot load at all. Only part of it can be read without the game:\n"
						  "quests, items, spells and shouts, level. Not skills, perks, words of power, map locations or things you made.");
	}
	if (_save) {
		ImGui::SameLine();
		if (ImGui::Button("Use the save open here")) {
			_tpBad = tp::FromSave(*_save, platform::ToUtf8(_path.filename()));
			transplantNames(_tpBad, *_save);
			_tpDirty = true;
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Open a snapshot file...") && platform::HasNativeDialogs()) {
		if (auto p = platform::OpenFileDialog({}, "txt", "ReSaverHelper snapshot (Documents\\My Games\\...\\SKSE\\ReSaverHelper)")) {
			transplantFromFile(*p);
		}
	}
	ImGui::EndDisabled();
	if (_tpBad.valid && _tpBad.fromFile) {
		std::string parts;
		for (const auto& m : _tpBad.missingParts) {
			parts += (parts.empty() ? "" : ", ") + m;
		}
		Wrapped(theme::Caution(), "Read from the file without the game: " + parts + " cannot be read this way. If the game can load this save, a snapshot "
								  "from the game is complete.");
	}

	// step 2: the good save
	ImGui::SeparatorText("2. The good save: load it in the game, then compare");
	ImGui::TextWrapped("%s", Summary(_tpGood).c_str());
	ImGui::BeginDisabled(!live || !helperOk || !_tpBad.valid || _tpRunning);
	if (ImGui::Button(_tpGood.valid ? "Snapshot the game again (good save loaded)" : "Snapshot the game (good save loaded)")) {
		confirm("Snapshot the good save", "Is the GOOD save loaded in the game right now? (Load it from the game's menu first.)\n\n"
										  "ReSaverHelper writes down what the character has there, and ReSaver++ lists what the bad save has on top.",
			[this] { transplantSnapshot(false); });
	}
	ImGui::EndDisabled();
	if (_tpBad.valid && _tpGood.valid && !_tpBad.player.empty() && !_tpGood.player.empty() && _tpBad.player != _tpGood.player) {
		ImGui::SameLine();
		TextColored(theme::Bad(), "These are two different characters (" + _tpBad.player + " / " + _tpGood.player + ").");
	}

	if (_tpDirty && _tpBad.valid && _tpGood.valid) {
		_tpChanges = tp::Diff(_tpGood, _tpBad);
		_tpResults.clear();
		_tpDirty = false;
	}
	if (!_tpBad.valid || !_tpGood.valid) {
		ImGui::End();
		return;
	}

	// step 3: what to bring
	ImGui::SeparatorText(Fmt("3. What to bring over (%zu)", _tpChanges.size()).c_str());
	Dot(theme::Good());
	ImGui::TextUnformatted("Safe");
	ImGui::SameLine(0, 20);
	Dot(theme::Caution());
	ImGui::TextUnformatted("Caution: hover to read why");
	ImGui::SameLine(0, 20);
	Dot(theme::Bad());
	ImGui::TextUnformatted("Cannot: its mod is not in the game now");
	ImGui::SameLine(0, 30);
	ImGui::SetNextItemWidth(240);
	ImGui::InputTextWithHint("##tpfilter", "search", _tpFilter, sizeof(_tpFilter));
	const auto needle = Lower(_tpFilter);

	std::map<std::size_t, const TpResult*> results;
	for (const auto& r : _tpResults) {
		results[r.index] = &r;
	}
	ImGui::BeginChild("##tplist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.6f), ImGuiChildFlags_Borders);
	for (int c = 0; c < static_cast<int>(tp::Category::kCount); ++c) {
		std::vector<std::size_t> in;
		for (std::size_t i = 0; i < _tpChanges.size(); ++i) {
			const auto& ch = _tpChanges[i];
			if (static_cast<int>(ch.cat) == c && (needle.empty() || Lower(ch.label + " " + ch.detail).find(needle) != std::string::npos)) {
				in.push_back(i);
			}
		}
		if (in.empty()) {
			continue;
		}
		std::size_t ticked = 0;
		for (auto i : in) {
			ticked += _tpChanges[i].tick && _tpChanges[i].available ? 1 : 0;
		}
		ImGui::PushID(c);
		const bool open = ImGui::CollapsingHeader(Fmt("%s  (%zu of %zu ticked)###cat", tp::CategoryName(static_cast<tp::Category>(c)), ticked, in.size()).c_str(),
			c == static_cast<int>(tp::Category::kQuests) ? 0 : ImGuiTreeNodeFlags_DefaultOpen);
		if (open) {
			if (ImGui::SmallButton("Tick all")) {
				for (auto i : in) {
					_tpChanges[i].tick = _tpChanges[i].available;
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Untick all")) {
				for (auto i : in) {
					_tpChanges[i].tick = false;
				}
			}
			if (c == static_cast<int>(tp::Category::kQuests)) {
				ImGui::SameLine();
				ImGui::TextDisabled("Each quest's stage runs in the game. Quests the game refuses to complete are marked completed instead.");
			}
			for (auto i : in) {
				auto& ch = _tpChanges[i];
				ImGui::PushID(static_cast<int>(i));
				ImGui::BeginDisabled(!ch.available || _tpRunning);
				ImGui::Checkbox("##t", &ch.tick);
				ImGui::EndDisabled();
				ImGui::SameLine();
				Dot(RiskCol(ch.risk));
				ImGui::TextUnformatted(ch.label.c_str());
				if (ImGui::IsItemHovered() && !ch.why.empty()) {
					ImGui::SetTooltip("%s", ch.why.c_str());
				}
				if (!ch.detail.empty()) {
					ImGui::SameLine();
					ImGui::TextDisabled("%s", ch.detail.c_str());
				}
				if (auto it = results.find(i); it != results.end()) {
					ImGui::SameLine();
					if (it->second->ok) {
						TextColored(theme::Good(), it->second->why.empty() ? "done" : "done: " + it->second->why);
					} else {
						TextColored(theme::Bad(), "not done: " + it->second->why);
					}
				}
				ImGui::PopID();
			}
		}
		ImGui::PopID();
	}
	if (_tpChanges.empty()) {
		Wrapped(theme::Good(), "The good save already has everything the bad one has (of what could be read).");
	}
	ImGui::EndChild();

	std::vector<std::size_t> run;
	for (int c = 0; c < static_cast<int>(tp::Category::kCount); ++c) {  // character first, quests last
		for (std::size_t i = 0; i < _tpChanges.size(); ++i) {
			if (static_cast<int>(_tpChanges[i].cat) == c && _tpChanges[i].tick && _tpChanges[i].available) {
				run.push_back(i);
			}
		}
	}
	ImGui::BeginDisabled(!live || !helperOk || run.empty() || _tpRunning);
	if (ImGui::Button(Fmt("Transplant %zu ticked thing(s) into the game", run.size()).c_str())) {
		confirm("Transplant",
			Fmt("Put %zu ticked thing(s) into the game now?\n\n"
				"- The GOOD save must be the one loaded in the game (the one snapshotted in step 2).\n"
				"- Make a save in the game first, so you can go back.\n"
				"- Keep the game unpaused and menus closed until it is done.\n"
				"- Then save in the game: that new save is the result.",
				run.size()),
			[this, run] {
				_tpRun = run;
				transplantOrderQuests(_tpRun);
				_tpPos = 0;
				_tpResults.clear();
				_tpRunning = true;
				transplantStep();
			});
	}
	ImGui::EndDisabled();
	if (_tpRunning) {
		ImGui::SameLine();
		ImGui::ProgressBar(_tpRun.empty() ? 0.0f : static_cast<float>(_tpPos) / static_cast<float>(_tpRun.size()), ImVec2(260, 0),
			Fmt("%zu of %zu", _tpPos, _tpRun.size()).c_str());
	} else if (!_tpResults.empty()) {
		std::size_t failed = 0;
		for (const auto& r : _tpResults) {
			failed += r.ok ? 0 : 1;
		}
		ImGui::SameLine();
		TextColored(failed ? theme::Caution() : theme::Good(),
			Fmt("Done: %zu brought over, %zu not (see the list). Save in the game to keep it.", _tpResults.size() - failed, failed));
	}
	ImGui::TextDisabled("Nothing is taken away from the good save: things are only added, and levels and skills only raised.");
	ImGui::End();
}
