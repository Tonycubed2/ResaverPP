// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Transplant page: bring a character's progress from a bad save into a good one. The bad save is
// read through the game (ReSaverHelper's snapshot, complete) or from its file (partial); the good one
// is the save loaded in the game now. What the bad save has and the good one does not is listed,
// coloured by risk; the ticked things are put into the running game one by one by ReSaverHelper, and
// the player saves in the game.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
			return s.error.empty() ? tr("not chosen yet") : s.error;
		}
		std::size_t items = 0, done = 0;
		for (const auto& i : s.items) {
			items += static_cast<std::size_t>(std::max(0, i.count));
		}
		for (const auto& [k, q] : s.quests) {
			done += q.completed ? 1 : 0;
		}
		return Fmt(tr("%s%s%s: %zu quests completed, %zu items%s"), s.player.empty() ? "" : (s.player + " ").c_str(), s.level > 0 ? Fmt(tr("level %d"), s.level).c_str() : "",
			s.fromFile ? tr("  (read from the save file)") : tr("  (from the game)"), done, items,
			s.fromFile ? "" : Fmt(tr(", %zu perks, %zu spells"), s.perks.size(), s.spells.size()).c_str());
	}

	// a moving dashed frame around the last item: "do this next"
	void Marquee()
	{
		const float pad = 4.0f;
		const auto  mn = ImVec2(ImGui::GetItemRectMin().x - pad, ImGui::GetItemRectMin().y - pad);
		const auto  mx = ImVec2(ImGui::GetItemRectMax().x + pad, ImGui::GetItemRectMax().y + pad);
		const float w = mx.x - mn.x, h = mx.y - mn.y, per = 2 * (w + h);
		auto        at = [&](float d) {
            d = std::fmod(d + per, per);
            if (d < w) {
                return ImVec2(mn.x + d, mn.y);
            }
            d -= w;
            if (d < h) {
                return ImVec2(mx.x, mn.y + d);
            }
            d -= h;
            if (d < w) {
                return ImVec2(mx.x - d, mx.y);
            }
            return ImVec2(mn.x, mx.y - (d - w));
		};
		const float t = static_cast<float>(ImGui::GetTime());
		const auto  alpha = static_cast<int>(170 + 85 * std::sin(t * 5.0f));
		const auto  col = IM_COL32(255, 196, 40, alpha);
		auto*       dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(mn, mx, IM_COL32(255, 196, 40, 22 + static_cast<int>(18 * std::sin(t * 5.0f))), 4.0f);
		const float dash = 12.0f, gap = 7.0f, offset = std::fmod(t * 60.0f, dash + gap);
		for (float d = offset - (dash + gap); d < per; d += dash + gap) {
			ImVec2 pts[7];
			for (int k = 0; k < 7; ++k) {
				pts[k] = at(d + dash * static_cast<float>(k) / 6.0f);
			}
			dl->AddPolyline(pts, 7, col, 0, 2.5f);
		}
	}

	// what a snapshot holds, group by group (shown under step 1 while there is no good save yet)
	void SnapshotContents(const tp::Snapshot& s)
	{
		auto list = [](const char* a_title, const std::vector<std::string>& a_names) {
			if (a_names.empty()) {
				return;
			}
			if (ImGui::TreeNodeEx(a_title, ImGuiTreeNodeFlags_SpanAvailWidth, tr("%s (%zu)"), a_title, a_names.size())) {
				std::string line;
				for (const auto& n : a_names) {
					line += (line.empty() ? "" : ",   ") + n;
				}
				ImGui::TextWrapped("%s", line.c_str());
				ImGui::TreePop();
			}
		};
		auto names = [](const std::map<tp::FormKey, tp::Entry>& a_m) {
			std::vector<std::string> v;
			for (const auto& [k, e] : a_m) {
				v.push_back(e.name.empty() ? Fmt("%06X", k.local) : e.name);
			}
			std::sort(v.begin(), v.end());
			return v;
		};
		if (s.level > 0) {
			ImGui::Text(tr("Level %d%s%s"), s.level, s.perkPoints > 0 ? Fmt(tr(",  %d perk point(s) to spend"), s.perkPoints).c_str() : "",
				s.look.empty() ? "" : tr(",  look saved (RaceMenu)"));
		}
		std::vector<std::string> skills;
		for (int i = 0; i < 18; ++i) {
			if (s.skills[i].level >= 0) {
				skills.push_back(Fmt("%s %.0f", tp::SkillName(i), s.skills[i].level));
			}
		}
		list(tr("Skills"), skills);
		list(tr("Perks"), names(s.perks));
		list(tr("Spells and powers"), names(s.spells));
		list(tr("Shouts"), names(s.shouts));
		list(tr("Words of power"), names(s.words));
		list(tr("Map locations"), names(s.markers));
		list(tr("Factions"), names(s.factions));
		std::vector<std::string> active, completed;
		for (const auto& [k, q] : s.quests) {
			const bool journal = s.journalKnown ? q.inJournal : q.known ? q.shown() : q.completed;
			if (!journal) {
				continue;
			}
			const auto n = !q.name.empty() ? q.name : !q.edid.empty() ? q.edid : Fmt(tr("%06X"), q.local);
			(q.completed ? completed : active).push_back(n);
		}
		std::sort(active.begin(), active.end());
		std::sort(completed.begin(), completed.end());
		list(tr("Quests in progress (journal)"), active);
		list(tr("Quests completed (journal)"), completed);
		std::map<std::string, std::vector<std::string>> kinds;
		for (const auto& it : s.items) {
			const auto kind = it.record.size() > 12 && !it.record[12].empty() ? it.record[12] : std::string(tr("Items"));
			kinds[kind].push_back(it.count > 1 ? Fmt(tr("%s x%d"), it.name.c_str(), it.count) : it.name);
		}
		for (auto& [k, v] : kinds) {
			std::sort(v.begin(), v.end());
			list((tr("Inventory: ") + k).c_str(), v);
		}
		std::vector<std::string> made, worn;
		for (const auto& p : s.potions) {
			made.push_back(Fmt(tr("%s x%d"), p.name.c_str(), p.count));
		}
		for (const auto& e : s.equips) {
			worn.push_back(e.name);
		}
		list(tr("Potions and poisons you made"), made);
		list(tr("Worn and held"), worn);
		std::vector<std::string> followers;
		for (const auto& f : s.followers) {
			followers.push_back(f.name);
		}
		list(tr("Followers"), followers);
	}
}

// ---- getting the two snapshots --------------------------------------------------------------------

void App::transplantSnapshot(bool a_bad)
{
	_status = tr("Asking the game for a snapshot of the loaded save: switch to Skyrim for a moment (it pauses in the background)...");
	_tpWaiting = true;
	_tpWaitStart = ImGui::GetTime();
	_live.request({ "SNAPSHOT" }, [this, a_bad](const LiveLink::Reply& r) {
		_tpWaiting = false;
		if (!LiveLink::Ok(r)) {
			_status = tr("The game could not take a snapshot: ") + LiveLink::Error(r);
			return;
		}
		auto s = tp::ReadSnapshot(platform::FromUtf8(LiveLink::Field(r, 0)));
		if (!s.valid) {
			_status = s.error;
			return;
		}
		_status = Fmt(tr("Snapshot of %s taken (%s records)."), s.player.c_str(), LiveLink::Field(r, 1).c_str());
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
		_status = _tpBad.valid ? tr("Read ") + platform::ToUtf8(a_path.filename()) : _tpBad.error;
	} catch (const std::exception& e) {
		_error = tr("Could not read ") + platform::ToUtf8(a_path.filename()) + ":\n\n" + e.what();
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
		_status = Fmt(tr("Transplant done: %zu of %zu brought over%s. Quests may keep loading for a couple of minutes: save in the game when "
					  "their messages stop."),
			_tpRun.size() - failed, _tpRun.size(), failed ? Fmt(tr(", %zu could not be (see the Transplant page)"), failed).c_str() : "");
		bool quests = false;
		for (auto i : _tpRun) {
			quests = quests || (i < _tpChanges.size() && _tpChanges[i].quest);
		}
		if (quests && helperAtLeast(9)) {
			// a quest's stage script can leave the controls off (the player cannot move until a reload)
			_live.request({ "APPLY", "controls" }, [](const LiveLink::Reply&) {});
		}
		_live.request({ "NOTIFY", liveNotifyMode(),
						  Fmt("ReSaver++: transplant done (%zu of %zu). Quests may still be loading: wait until their messages stop, then save.",
							  _tpRun.size() - failed, _tpRun.size()) },
			[](const LiveLink::Reply&) {});
		_tpRunning = false;
		if (quests && helperAtLeast(9)) {
			transplantCheckJournal();
		}
		return;
	}
	const auto idx = _tpRun[_tpPos];
	if (idx >= _tpChanges.size()) {
		++_tpPos;
		transplantStep();
		return;
	}
	const auto& ch = _tpChanges[idx];
	_status = Fmt(tr("Transplanting %zu of %zu: %s"), _tpPos + 1, _tpRun.size(), ch.label.c_str());
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
				done(true, LiveLink::Field(r, 0) == "?" ? tr("the game is still doing it") : "");
			}
		});
		return;
	}

	// a quest: find it in the game, then its completion stage (or the stage it had), else mark it completed.
	// Helper 9: the objectives are shown first (the quest started if needed), so a quest that ends at its
	// stage is in the journal before it stops; then again afterwards, for the states its script changed.
	const auto* q = ch.quest;
	const bool  complete = ch.complete;
	const bool  journalOnly = ch.journalOnly;
	const bool  markOnly = ch.markOnly;  // the Helgen intro: never started or staged, only marked completed
	const bool  safeMark = ch.safeMark();  // its last stage would move the player / play a scene: only marked completed
	const bool  markRefused = _tpMarkRefused && complete;  // only quests completed in the bad save
	const bool  before = !q->objectives.empty() && helperAtLeast(9);
	// after the stage / mark: show the quest's objectives as the bad save had them, so the journal lists it
	auto        shown = [this, q, done](bool a_ok, const std::string& a_why) {
        if (q->objectives.empty() || !helperAtLeast(8)) {
            done(a_ok, a_why);
            return;
        }
        _live.request({ "APPLY", "questobj", q->plugin, Fmt("%06X", q->local), "", q->objectives }, [done, a_ok, a_why](const LiveLink::Reply& r) {
            const bool ok = LiveLink::Ok(r) && LiveLink::Field(r, 0) != "0";
            done(a_ok || ok, a_why + (a_why.empty() ? "" : ", ") + (ok ? tr("objectives shown in the journal") : tr("objectives not shown: ") + LiveLink::Field(r, 1)));
        });
	};
	auto        run = [this, q, complete, markRefused, journalOnly, markOnly, safeMark, before, shown, done](std::uint32_t a_id) {
        const int stage = complete ? completionStageOf(*q) : q->stage;
        // marks the quest completed: no stage script, no rewards, follow-up quests not started
        auto      mark = [this, a_id, shown](const std::string& a_before) {
            _live.request({ "COMPLETE", Fmt("%08X", a_id), "none", "" }, [shown, a_before](const LiveLink::Reply& r) {
                const bool ok = LiveLink::Ok(r) && LiveLink::Field(r, 0) != "0";
                shown(ok, ok ? a_before + tr(", marked as done") : a_before + tr(", and it could not be marked as done either"));
            });
        };
        auto      stageStep = [this, a_id, stage, complete, markRefused, journalOnly, mark, shown, done]() {
            if (journalOnly) {
                shown(true, "");
                return;
            }
            if (stage < 0) {
                if (complete || markRefused) {
                    mark(tr("completion stage not known (plugins not read)"));
                } else {
                    shown(false, tr("no stage to set"));
                }
                return;
            }
            _live.request({ "SETSTAGE", Fmt("%08X", a_id), std::to_string(stage), "none", "" }, [markRefused, mark, shown, done, stage](const LiveLink::Reply& r) {
                if (!LiveLink::Ok(r)) {
                    done(false, LiveLink::Error(r));
                    return;
                }
                const auto f = LiveLink::Field(r, 0);
                if (f == "0") {
                    if (markRefused) {
                        mark(Fmt(tr("refused stage %d"), stage));
                    } else {
                        shown(false, Fmt(tr("refused stage %d"), stage));
                    }
                    return;
                }
                shown(true, f == "?" ? Fmt(tr("stage %d set; the game is still running its script"), stage) : Fmt(tr("stage %d set"), stage));
            });
        };
        if (markOnly) {
            // only CompleteQuest: no start, no stage, no objectives (any of those could play the intro)
            _live.request({ "COMPLETE", Fmt("%08X", a_id), "none", "" }, [done](const LiveLink::Reply& r) {
                const bool ok = LiveLink::Ok(r) && LiveLink::Field(r, 0) != "0";
                done(ok, ok ? tr("marked as done, its scenes not played") : tr("could not be marked as done (nothing else was changed)"));
            });
            return;
        }
        if (safeMark) {
            // CompleteQuest only (no start, no stage: SkyrimSE.exe's CompleteQuest runs no stage), then the objectives are written
            // into the journal list directly (questobj without "start": no Papyrus runs)
            _live.request({ "COMPLETE", Fmt("%08X", a_id), "none", "" }, [shown](const LiveLink::Reply& r) {
                const bool ok = LiveLink::Ok(r) && LiveLink::Field(r, 0) != "0";
                shown(ok, ok ? tr("marked as done; its last stage was not run (it would move you or play a scene)")
                             : tr("could not be marked as done (nothing else was changed)"));
            });
            return;
        }
        if (!before) {
            stageStep();
            return;
        }
        _live.request({ "APPLY", "questobj", q->plugin, Fmt("%06X", q->local), "start", q->objectives }, [stageStep](const LiveLink::Reply&) { stageStep(); });
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

// after a run: a new snapshot of the game, and the quests the bad save's journal has that the game's
// journal does not, each with why (from the run's results)
void App::transplantCheckJournal()
{
	_tpChecking = true;
	_live.request({ "SNAPSHOT" }, [this](const LiveLink::Reply& r) {
		_tpChecking = false;
		if (!LiveLink::Ok(r)) {
			_status = tr("Could not check the journal: ") + LiveLink::Error(r);
			return;
		}
		transplantMissing(tp::ReadSnapshot(platform::FromUtf8(LiveLink::Field(r, 0))));
	});
}

void App::transplantMissing(const ess::transplant::Snapshot& now)
{
		if (!now.valid || !now.journalKnown) {
			_status = tr("Could not check the journal (the snapshot has no journal list: ReSaverHelper 9 is needed).");
			return;
		}
		// what the run did with each quest
		std::map<const tp::Quest*, std::string> why;
		for (std::size_t i = 0; i < _tpChanges.size(); ++i) {
			if (const auto* q = _tpChanges[i].quest) {
				why[q] = !_tpChanges[i].available ? tr("its mod is not in this game") : !_tpChanges[i].tick ? tr("it was not ticked") : "";
			}
		}
		for (const auto& res : _tpResults) {
			if (res.index < _tpChanges.size() && _tpChanges[res.index].quest && !res.ok) {
				why[_tpChanges[res.index].quest] = res.why;
			}
		}
		_tpMissing.clear();
		for (const auto& [k, q] : _tpBad.quests) {
			const bool inBad = _tpBad.journalKnown ? q.inJournal : q.known ? q.shown() : q.completed;
			if (!inBad) {
				continue;
			}
			const auto n = now.quests.find(k);
			if (n != now.quests.end() && n->second.inJournal) {
				continue;
			}
			TpMissing m;
			m.name = !q.name.empty() ? q.name : !q.edid.empty() ? q.edid : Fmt("%06X", q.local);
			m.edid = q.edid;
			m.quest = &q;
			m.completed = q.completed;
			auto w = why.find(&q);
			m.why = w == why.end() ? tr("it was not in the list (nothing to bring: the good save had it as far)")
					: !w->second.empty() ? w->second
					: q.completed ? tr("the game would not complete it here")
								  : tr("the game would not start it here: it starts from something in the game (a note, a person, a place)");
			_tpMissing.push_back(std::move(m));
		}
		std::sort(_tpMissing.begin(), _tpMissing.end(), [](const TpMissing& a, const TpMissing& b) { return a.name < b.name; });
		_tpChecked = true;
		_status = _tpMissing.empty() ? tr("Every quest of the bad save's journal is in the game's journal now.")
									 : Fmt(tr("%zu quest(s) of the bad save's journal are not in the game's journal: see the list."), _tpMissing.size());
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

// the group a change is listed under inside its category: items by kind, the rest by mod
std::string App::transplantGroup(const tp::Change& a_ch) const
{
	auto at = [&](std::size_t i) { return i < a_ch.record.size() ? a_ch.record[i] : std::string(); };
	if (a_ch.cat == tp::Category::kItems) {
		if (!at(12).empty()) {
			return at(12);  // written by ReSaverHelper 6
		}
		if (Lower(at(1)) == "skyrim.esm" && std::strtoul(at(2).c_str(), nullptr, 16) == 0xF) {
			return tr("Gold");
		}
		if (_db) {
			if (const auto* f = _db->find(static_cast<std::uint32_t>(std::strtoul(at(3).c_str(), nullptr, 16)))) {
				const auto t = _db->type(*f);
				return t == "WEAP" ? tr("Weapons") : t == "ARMO" ? tr("Armor and clothing") : t == "AMMO" ? tr("Arrows and bolts") : t == "BOOK" ? tr("Books and spell tomes")
					   : t == "ALCH" ? tr("Potions and food") : t == "INGR" ? tr("Ingredients") : t == "SLGM" ? tr("Soul gems") : t == "KEYM" ? tr("Keys")
					   : t == "SCRL" ? tr("Scrolls") : t == "MISC" ? tr("Miscellaneous") : t == "LIGH" ? tr("Torches and lights") : tr("Other");
			}
		}
		return tr("Other");
	}
	if (a_ch.cat == tp::Category::kMade) {
		return at(0) == "potion" ? (at(3) == "1" ? tr("Poisons") : tr("Potions")) : tr("Enchanted gear");
	}
	if (a_ch.cat == tp::Category::kCharacter) {
		return {};
	}
	if (a_ch.cat == tp::Category::kLook || a_ch.cat == tp::Category::kAppearance) {
		return {};
	}
	if (a_ch.quest && a_ch.stageCheck == tp::Change::StageCheck::kUnsafe) {
		return tr("! Quests whose stage would move you or play a scene (not run)");
	}
	if (a_ch.quest) {
		return a_ch.background ? tr("~ Background quests (not in the journal): normally leave these unticked") : a_ch.quest->plugin;
	}
	return at(1);
}

void App::maximizeControl(const char* a_id)
{
	// a Maximize / Restore button at the top right, and a double-click on the title bar does the same
	const bool  maxed = _maximized.contains(a_id);
	const auto  wp = ImGui::GetWindowPos();
	const auto  ws = ImGui::GetWindowSize();
	const float title = ImGui::GetFrameHeight();
	bool        toggle = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(wp, ImVec2(wp.x + ws.x, wp.y + title), false) &&
				  ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
	const char* label = maxed ? tr("Restore size") : tr("Maximize");
	const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
	const auto  back = ImGui::GetCursorPos();
	ImGui::SetCursorPos(ImVec2(ImGui::GetWindowContentRegionMax().x - w, back.y));
	toggle |= ImGui::SmallButton(label);
	tips::Tipf(tr("%s (or double-click the title bar)"), maxed ? tr("Back to the size it had") : tr("Fill the whole ReSaver++ window"));
	ImGui::SetCursorPos(back);
	if (toggle) {
		if (maxed) {
			_restoreRects[a_id] = _maximized[a_id];
			_maximized.erase(a_id);
		} else {
			_maximized[a_id] = { wp, ws };
		}
	}
}

void App::prepareWindow(const char* a_id, ImVec2 a_defaultFraction)
{
	const auto* vp = ImGui::GetMainViewport();
	if (_maximized.contains(a_id)) {
		ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
		ImGui::SetNextWindowSize(vp->WorkSize, ImGuiCond_Always);
	} else if (auto it = _restoreRects.find(a_id); it != _restoreRects.end()) {
		ImGui::SetNextWindowPos(it->second.first, ImGuiCond_Always);
		ImGui::SetNextWindowSize(it->second.second, ImGuiCond_Always);
		_restoreRects.erase(it);
	} else {
		ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * a_defaultFraction.x, vp->WorkSize.y * a_defaultFraction.y), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
	}
}

void App::drawTransplant()
{
	if (fo4()) {
		drawTransplantF4();  // (AppTransplantF4.cpp: both saves read from their files)
		return;
	}
	if (sf()) {
		drawTransplantSF();  // (AppTransplantSF.cpp: both saves read from their files)
		return;
	}
	prepareWindow("Transplant", ImVec2(0.9f, 0.9f));
	if (!ImGui::Begin(trId("Transplant"), &_showTransplant, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Transplant");
	const auto info = _live.info();
	const bool live = info.connected && info.inGame;
	const bool helperOk = helperAtLeast(5);
	const bool busy = _tpRunning || _tpWaiting;

	ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 140);
	ImGui::TextWrapped("%s", tr("Bring your character's progress from a bad save into a good one from before. ReSaver++ lists what the bad save has and "
					   "the good one does not; you tick what to bring; the game itself puts it into the good save the normal way, so the result "
					   "is a clean save written by the game. Needs Skyrim running with ReSaverHelper.dll 5 or newer."));
	ImGui::PopTextWrapPos();
	if (!helperOk) {
		Wrapped(theme::Caution(), info.connected ? Fmt(tr("ReSaverHelper %s is running: version 5 or newer is needed (Optional files on the mod page)."), info.helperVersion.c_str())
												 : tr("ReSaverHelper is not connected: start Skyrim with ReSaverHelper.dll (version 5 or newer) installed."));
	}
	if (helperOk && helperAtLeast(9) && !helperAtLeast(10)) {
		Wrapped(theme::Caution(), Fmt(tr("ReSaverHelper %s is running: followers need version 10. Install the newest ReSaverHelper and take new snapshots."),
									  info.helperVersion.c_str()));
	}
	if (helperOk && !helperAtLeast(9)) {
		Wrapped(theme::Caution(), Fmt(tr("ReSaverHelper %s is running: your look and worn gear need version 7, and quests brought into the journal need "
									  "version 9. Install the newest ReSaverHelper and take new snapshots."),
									  info.helperVersion.c_str()));
	}
	if (_tpWaiting && ImGui::GetTime() - _tpWaitStart > 75.0) {
		_tpWaiting = false;
		_status = tr("No answer from the game for the snapshot. Was Skyrim the active window for a few seconds? Try again; if it still does not "
				  "answer, restart the game and send ReSaverHelper.log (Documents\\My Games\\Skyrim Special Edition\\SKSE).");
	}
	if (_tpWaiting) {
		Wrapped(theme::Caution(), Fmt(tr("Waiting for the game (%.0f s): switch to Skyrim for a few seconds (it pauses while it is not the active window), "
									  "then come back."), ImGui::GetTime() - _tpWaitStart));
	}

	// steps 1 and 2 side by side
	if (ImGui::BeginTable("steps", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableNextColumn();
		ImGui::SeparatorText(tr("1. The bad save: take its progress"));
		ImGui::TextWrapped("%s", Summary(_tpBad).c_str());
		ImGui::BeginDisabled(!live || !helperOk || busy);
		const bool badBtn = ImGui::Button(tr("Snapshot the game (bad save loaded in the game)"));
		tips::Tip(tr("Load the BAD save in the game first. ReSaverHelper then writes down your character as the game sees it: level, skills, perks, spells, shouts, items, quests, map locations. Nothing is changed."));
		if (!_tpBad.valid && !busy) {
			Marquee();
			ImGui::SameLine();
			TextColored(IM_COL32(255, 196, 40, 255), tr("<- step 1: start here"));
		}
		if (badBtn) {
			confirm(tr("Snapshot the bad save"), tr("Is the BAD save loaded in the game right now?\n\nReSaverHelper writes down everything about your character "
											 "in the loaded game: level, skills, perks, spells, shouts, items, quests, map locations.\n\n"
											 "Skyrim pauses while it is not the active window: after OK, switch to the game for a moment."),
				[this] { transplantSnapshot(true); });
		}
		ImGui::EndDisabled();
		ImGui::BeginDisabled(busy);
		if (ImGui::Button(tr("Bad save won't load? Read it from the save file...")) && platform::HasNativeDialogs()) {
			if (auto p = platform::OpenSaveDialog(savesFolder())) {
				transplantFromFile(*p);
			}
		}
		tips::Tip(tr("For a save the game cannot load at all. Without the game only part of it can be read:\n"
							  "quests, items, spells and shouts, level. Not skills, perks, words of power, map locations or things you made."));
		if (_save) {
			if (ImGui::SmallButton(tr("Use the save open in ReSaver++"))) {
				_tpBad = tp::FromSave(*_save, platform::ToUtf8(_path.filename()));
				transplantNames(_tpBad, *_save);
				_tpDirty = true;
			}
			tips::Tip(tr("Use the save opened in ReSaver++ as the bad save, without the game. Only part of it can be read this way (no skills, perks or map locations)."));
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(tr("Open a snapshot file...")) && platform::HasNativeDialogs()) {
			if (auto p = platform::OpenFileDialog({}, "txt", tr("ReSaverHelper snapshot (Documents\\My Games\\...\\SKSE\\ReSaverHelper)"))) {
				transplantFromFile(*p);
			}
		}
		tips::Tip(tr("Open a snapshot ReSaverHelper wrote earlier (a .txt file in Documents\\My Games\\...\\SKSE\\ReSaverHelper)."));
		ImGui::EndDisabled();
		if (_tpBad.valid && _tpBad.fromFile) {
			std::string parts;
			for (const auto& m : _tpBad.missingParts) {
				parts += (parts.empty() ? "" : ", ") + m;
			}
			Wrapped(theme::Caution(), tr("Read from the file without the game: ") + parts + tr(" are not included."));
		}

		ImGui::TableNextColumn();
		ImGui::SeparatorText(tr("2. The good save: load it in the game, then snapshot it"));
		ImGui::TextWrapped("%s", Summary(_tpGood).c_str());
		ImGui::BeginDisabled(!live || !helperOk || !_tpBad.valid || busy);
		const bool goodBtn = ImGui::Button(_tpGood.valid ? tr("Snapshot the game again (good save loaded in the game)") : tr("Snapshot the game (good save loaded in the game)"));
		tips::Tip(tr("Load the GOOD save (an older save that works) in the game first, and make a backup save. ReSaverHelper writes down what it has, so ReSaver++ can list what the bad save has on top."));
		if (_tpBad.valid && !_tpGood.valid && !busy) {
			Marquee();
			ImGui::SameLine();
			TextColored(IM_COL32(255, 196, 40, 255), tr("<- step 2: now this"));
		}
		if (goodBtn) {
			confirm(tr("Snapshot the good save"), tr("Is the GOOD save loaded in the game right now? (Load it from the game's menu first, and make a backup save.)\n\n"
											  "After OK, switch to the game for a moment: it pauses while it is not the active window."),
				[this] { transplantSnapshot(false); });
		}
		ImGui::EndDisabled();
		if (_tpBad.valid && _tpGood.valid && !_tpBad.player.empty() && !_tpGood.player.empty() && _tpBad.player != _tpGood.player) {
			Wrapped(theme::Bad(), tr("These are two different characters (") + _tpBad.player + " / " + _tpGood.player + ").");
		}
		ImGui::EndTable();
	}

	if (_tpDirty && _tpBad.valid && _tpGood.valid) {
		_tpChanges = tp::Diff(_tpGood, _tpBad);
		_tpResults.clear();
		_tpDirty = false;
	}
	if (const char* f = std::getenv("RESAVERPP_TEST_TPFILTER"); f && !_tpFilter[0]) {
		std::snprintf(_tpFilter, sizeof(_tpFilter), "%s", f);  // (test) filter the list, which opens the groups
	}
	// what each quest's stage would do in the game: checked once the plugins and scripts are read (StageSafety.h)
	bool stagesPending = false;
	if (!_tpRunning && _tpBad.valid && _tpGood.valid) {
		const std::string noPlugins = "the plugins are not read (open a save of this character in ReSaver Plus first)";
		for (const auto& c : _tpChanges) {
			stagesPending |= c.quest && !c.markOnly &&
			                 (c.stageCheck == tp::Change::StageCheck::kNotAsked || (_db && _save && c.stageCheck == tp::Change::StageCheck::kNotChecked && c.stageWhy == noPlugins));
		}
		const bool dbReady = _db && _save;
		if (stagesPending && (dbReady ? ensureSourcesScanned() : !namesLoading())) {
			std::map<std::pair<std::string, std::uint32_t>, std::uint32_t> idOf;  // (plugin, local) -> form ID in the open save's load order
			if (dbReady) {
				for (const auto& [id, def] : _db->questDefs()) {
					const auto local = (id >> 24) == 0xFE ? (id & 0xFFF) : (id & 0xFFFFFF);
					idOf[{ Lower(_save->pluginOf(id)), local }] = id;
				}
			}
			std::map<std::string, std::unique_ptr<pex::File>> scripts;  // read once for all the quests
			stagesafety::Env                                  env;
			if (dbReady) {
				env.quest = [this](std::uint32_t id) { return _db->quest(id); };
				env.script = [this, &scripts](const std::string& a_name) -> const pex::File* {
					const auto key = Lower(a_name);
					if (auto it = scripts.find(key); it != scripts.end()) {
						return it->second.get();
					}
					auto& slot = scripts[key];
					if (const auto px = _sources->findPex(a_name)) {
						slot = std::make_unique<pex::File>(pex::Read(ScriptSources::readPex(*px)));
					}
					return slot.get();
				};
			}
			// a few at a time (each reads its scripts from the mods' archives): the window keeps drawing meanwhile
			const auto   start = std::chrono::steady_clock::now();  // (ImGui::GetTime() stays the same for a whole frame)
			bool         more = false;
			for (auto& c : _tpChanges) {
				const bool todo = c.quest && !c.markOnly &&
				                  (c.stageCheck == tp::Change::StageCheck::kNotAsked || (dbReady && c.stageCheck == tp::Change::StageCheck::kNotChecked && c.stageWhy == noPlugins));
				if (!todo) {
					continue;
				}
				if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(50)) {
					more = true;
					break;
				}
				stagesafety::Report r;
				if (!dbReady) {
					r.notChecked = noPlugins;
				} else if (auto it = idOf.find({ Lower(c.quest->plugin), c.quest->local }); it == idOf.end()) {
					r.notChecked = "the quest is not in the plugins read for the open save";
				} else {
					r = stagesafety::Check(it->second, tp::StageToRun(c, completionStageOf(*c.quest)), tp::StartsQuest(c, _tpGood), env);
				}
				tp::ApplyStageCheck(c, r);
			}
			stagesPending = more;
		}
	}
	if (!_tpBad.valid || !_tpGood.valid) {
		if (_tpBad.valid) {
			ImGui::SeparatorText(tr("What the bad save has (this is what can be brought over)"));
			ImGui::BeginChild("##tpbad", ImVec2(0, 0), ImGuiChildFlags_Borders);
			SnapshotContents(_tpBad);
			ImGui::EndChild();
		}
		ImGui::End();
		return;
	}

	// step 3: what to bring, in folded groups
	ImGui::SeparatorText(Fmt(tr("3. What to bring over (%zu)"), _tpChanges.size()).c_str());
	Dot(theme::Good());
	ImGui::TextUnformatted(tr("Safe"));
	ImGui::SameLine(0, 18);
	Dot(theme::Caution());
	ImGui::TextUnformatted(tr("Caution (hover to read why)"));
	ImGui::SameLine(0, 18);
	Dot(theme::Bad());
	ImGui::TextUnformatted(tr("Cannot: its mod is not in the game"));
	ImGui::SameLine(0, 30);
	int openAll = -1;
	if (ImGui::SmallButton(tr("Expand all"))) {
		openAll = 1;
	}
	tips::Tip(tr("Open every group below."));
	ImGui::SameLine();
	if (ImGui::SmallButton(tr("Collapse all"))) {
		openAll = 0;
	}
	tips::Tip(tr("Fold every group below."));
	ImGui::SameLine();
	ImGui::BeginDisabled(_tpRunning);
	if (ImGui::SmallButton(tr("Tick everything"))) {
		for (auto& c : _tpChanges) {
			c.tick = c.available && !(c.stageCheck == tp::Change::StageCheck::kUnsafe && !c.complete);
		}
	}
	tips::Tip(tr("Tick everything that can be brought over (things whose mod is not in the game stay unticked, and so do quest stages that "
				 "would move you or play a scene)."));
	ImGui::SameLine();
	if (ImGui::SmallButton(tr("Untick everything"))) {
		for (auto& c : _tpChanges) {
			c.tick = false;
		}
	}
	tips::Tip(tr("Untick everything, e.g. to then tick only the few things you want."));
	ImGui::EndDisabled();
	ImGui::SameLine(0, 20);
	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##tpfilter", tr("search (opens the groups that match)"), _tpFilter, sizeof(_tpFilter));
	tips::Tip(tr("Show only the things whose name contains this text; the groups that match open."));
	const auto needle = Lower(_tpFilter);
	if (!needle.empty()) {
		openAll = 1;
	}

	std::map<std::size_t, const TpResult*> results;
	for (const auto& r : _tpResults) {
		results[r.index] = &r;
	}
	auto row = [&](std::size_t i) {
		auto& ch = _tpChanges[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::BeginDisabled(!ch.available || _tpRunning);
		ImGui::Checkbox("##t", &ch.tick);
		ImGui::EndDisabled();
		tips::Tip(ch.available ? tr("Ticked things are put into the game when you press Transplant.") : tr("Cannot be brought over: the mod it comes from is not in the game."));
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
		if (ch.stageCheck == tp::Change::StageCheck::kUnsafe && ch.complete) {
			ImGui::SameLine(0, 16);
			ImGui::BeginDisabled(!ch.available || _tpRunning);
			if (ImGui::Checkbox(tr("run its last stage anyway (risky)"), &ch.runStage)) {
				tp::RelabelStageCheck(ch);
			}
			ImGui::EndDisabled();
			tips::Tip(tr("Run the quest's last stage in the game after all, for its rewards and follow-up quests. It does what the line below "
						 "says (moves you, plays a scene...), which a transplant should not repeat. Unticked: the quest is only marked as done."));
		}
		if (auto it = results.find(i); it != results.end()) {
			ImGui::SameLine();
			TextColored(it->second->ok ? theme::Good() : theme::Bad(),
				it->second->ok ? (it->second->why.empty() ? tr("done") : tr("done: ") + it->second->why) : tr("not done: ") + it->second->why);
		}
		if (ch.stageCheck == tp::Change::StageCheck::kUnsafe || ch.stageCheck == tp::Change::StageCheck::kNotChecked) {
			// why, on its own line: what the stage would do (or why it could not be checked), and what marking it leaves out
			ImGui::Indent(ImGui::GetTextLineHeight() * 2.6f);
			std::string why = ch.stageCheck == tp::Change::StageCheck::kUnsafe ? std::string(tr("Its stage would: ")) + ch.stageWhy
																				: std::string(tr("Stage not checked: ")) + ch.stageWhy;
			if (ch.stageCheck == tp::Change::StageCheck::kUnsafe && ch.complete && !ch.stageLoses.empty() && !ch.runStage) {
				why += std::string("\n") + tr("Not given / not started when only marked as done: ") + ch.stageLoses;
			}
			Wrapped(ch.stageCheck == tp::Change::StageCheck::kUnsafe ? theme::Bad() : theme::Caution(), why);
			ImGui::Unindent(ImGui::GetTextLineHeight() * 2.6f);
		}
		ImGui::PopID();
	};
	auto tickButtons = [&](const std::vector<std::size_t>& a_in) {
		ImGui::BeginDisabled(_tpRunning);
		if (ImGui::SmallButton(tr("Tick all"))) {
			for (auto i : a_in) {
				const auto& c = _tpChanges[i];
				_tpChanges[i].tick = c.available && !(c.stageCheck == tp::Change::StageCheck::kUnsafe && !c.complete);
			}
		}
		tips::Tip(tr("Tick everything in this group (not the quest stages that would move you or play a scene)."));
		ImGui::SameLine();
		if (ImGui::SmallButton(tr("Untick all"))) {
			for (auto i : a_in) {
				_tpChanges[i].tick = false;
			}
		}
		tips::Tip(tr("Untick everything in this group."));
		ImGui::EndDisabled();
	};
	auto counts = [&](const std::vector<std::size_t>& a_in) {
		std::size_t t = 0;
		for (auto i : a_in) {
			t += _tpChanges[i].tick && _tpChanges[i].available ? 1 : 0;
		}
		return Fmt(tr("%zu of %zu ticked"), t, a_in.size());
	};

	ImGui::BeginChild("##tplist", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.4f), ImGuiChildFlags_Borders);
	if (stagesPending) {
		Wrapped(theme::Caution(), tr("Checking what each quest's stage does (reading your plugins and scripts)..."));
	}
	if (_tpChecking) {
		Wrapped(theme::Caution(), tr("Checking the game's journal (switch to Skyrim for a moment if it takes long)..."));
	} else if (_tpChecked) {
		if (_tpMissing.empty()) {
			Wrapped(theme::Good(), tr("Every quest of the bad save's journal is in the game's journal now."));
		} else {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::Caution()));
			ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
			const bool open = ImGui::CollapsingHeader(Fmt(tr("Quests that could not be brought over (%zu)###tpmissing"), _tpMissing.size()).c_str());
			tips::Tip(tr("Quests from the bad save's journal that are not in the game's journal after the transplant, and why."));
			ImGui::PopStyleColor();
			if (open) {
				ImGui::Indent();
				ImGui::TextWrapped("%s", tr("These were in the bad save's journal but are not in the game's journal now. Unfinished ones come back when "
								   "you do what starts them in the game (read the note, talk to the person, go to the place). Completed ones "
								   "can be marked as done here (like \"Mark refused quests as done\": no rewards, follow-up quests not started)."));
				std::string all;
				for (std::size_t mi = 0; mi < _tpMissing.size(); ++mi) {
					auto& m = _tpMissing[mi];
					ImGui::PushID(static_cast<int>(mi));
					if (m.completed && m.quest) {
						ImGui::BeginDisabled(!_live.info().connected || _tpRunning || !m.done.empty());
						if (ImGui::SmallButton(tr("Mark as done"))) {
							m.done = tr("marking...");
							const auto* q = m.quest;
							_live.request({ "RESOLVE", q->plugin, Fmt("%06X", q->local) }, [this, mi, q](const LiveLink::Reply& r) {
								auto set = [this, mi](const std::string& a_done) {
									if (mi < _tpMissing.size()) {
										_tpMissing[mi].done = a_done;
									}
								};
								if (!LiveLink::Ok(r)) {
									set(tr("not done: ") + LiveLink::Error(r));
									return;
								}
								_live.request({ "COMPLETE", LiveLink::Field(r, 0), "none", "" }, [this, q, set](const LiveLink::Reply& c) {
									const bool ok = LiveLink::Ok(c) && LiveLink::Field(c, 0) != "0";
									if (!ok) {
										set(tr("the game would not mark it as done"));
										return;
									}
									set(tr("marked as done"));
									if (!q->objectives.empty() && helperAtLeast(9)) {  // and listed in the journal as it was
										_live.request({ "APPLY", "questobj", q->plugin, Fmt("%06X", q->local), "", q->objectives }, [](const LiveLink::Reply&) {});
									}
								});
							});
						}
						ImGui::EndDisabled();
						tips::Tip(tr("Mark this quest completed in the game, as it was in the bad save. No rewards are given and follow-up quests do not start."));
						ImGui::SameLine();
					}
					Dot(m.completed ? theme::Caution() : theme::Bad());
					ImGui::TextUnformatted(m.name.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("[%s]  %s  -  %s", m.edid.c_str(), m.completed ? tr("completed in the bad save") : tr("in progress in the bad save"), m.why.c_str());
					all += m.name + "  [" + m.edid + "]  (" + (m.completed ? tr("completed") : tr("in progress")) + "): " + m.why + "\n";
					if (!m.done.empty()) {
						ImGui::SameLine();
						TextColored(m.done == tr("marked as done") ? theme::Good() : theme::Caution(), m.done);
					}
					ImGui::PopID();
				}
				if (ImGui::SmallButton(tr("Copy the list"))) {
					ImGui::SetClipboardText(all.c_str());
				}
				tips::Tip(tr("Copy these quests and why they are missing as text."));
				ImGui::SameLine();
				ImGui::BeginDisabled(!_live.info().connected || _tpRunning || _tpChecking);
				if (ImGui::SmallButton(tr("Check the journal again"))) {
					transplantCheckJournal();
				}
				ImGui::EndDisabled();
				tips::Tip(tr("Quests may keep loading for a minute or two after the transplant: check again after that."));
				ImGui::Unindent();
			}
		}
		ImGui::Spacing();
	}
	// shown in this order: your character's look, then your followers (both stand out), then the rest.
	// (They are still put into the game in category order: followers last, after the quests.)
	std::vector<int> shown{ static_cast<int>(tp::Category::kAppearance), static_cast<int>(tp::Category::kFollowers) };
	for (int c = 0; c < static_cast<int>(tp::Category::kCount); ++c) {
		if (c != shown[0] && c != shown[1]) {
			shown.push_back(c);
		}
	}
	for (const int c : shown) {
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
		ImGui::PushID(c);
		if (openAll >= 0) {
			ImGui::SetNextItemOpen(openAll == 1);
		}
		// the character's look stands out: it is the one thing that changes how your character looks
		const bool appearance = c == static_cast<int>(tp::Category::kAppearance) || c == static_cast<int>(tp::Category::kFollowers);
		if (appearance) {
			const bool light = theme::IsLight();
			ImGui::PushStyleColor(ImGuiCol_Header, light ? ImVec4(0.12f, 0.12f, 0.14f, 1) : ImVec4(0.02f, 0.02f, 0.03f, 1));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, light ? ImVec4(0.2f, 0.2f, 0.24f, 1) : ImVec4(0.10f, 0.08f, 0.02f, 1));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.18f, 0.14f, 0.04f, 1));
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.30f, 1));
			if (openAll < 0) {
				ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
			}
		}
		const bool open = ImGui::CollapsingHeader(Fmt("%s   (%s)###cat", tp::CategoryName(static_cast<tp::Category>(c)), counts(in).c_str()).c_str());
		tips::Tip(tr("One kind of progress the bad save has and the good one does not. Open it to tick what to bring over."));
		if (appearance) {
			// a glow around the header
			const auto mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
			auto*      dl = ImGui::GetWindowDrawList();
			for (int g = 1; g <= 3; ++g) {
				dl->AddRect(ImVec2(mn.x - g, mn.y - g), ImVec2(mx.x + g, mx.y + g), IM_COL32(255, 200, 70, 140 / g), 4.0f, 0, 1.5f);
			}
			ImGui::PopStyleColor(4);
		}
		if (open) {
			ImGui::Indent();
			tickButtons(in);
			if (c == static_cast<int>(tp::Category::kQuests)) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", tr("Quests run in quest-line order, after everything else."));
			}
			// groups: items by kind, the rest by mod (when there are many)
			std::map<std::string, std::vector<std::size_t>> groups;
			for (auto i : in) {
				groups[transplantGroup(_tpChanges[i])].push_back(i);
			}
			const bool flat = groups.size() <= 1 || in.size() <= 12;
			if (flat) {
				for (auto i : in) {
					row(i);
				}
			} else {
				for (const auto& [name, gi] : groups) {
					ImGui::PushID(name.c_str());
					if (openAll >= 0) {
						ImGui::SetNextItemOpen(openAll == 1);
					}
					if (ImGui::TreeNodeEx("##grp", ImGuiTreeNodeFlags_SpanAvailWidth, "%s   (%s)", name.empty() ? tr("Other") : name.c_str(), counts(gi).c_str())) {
						tickButtons(gi);
						for (auto i : gi) {
							row(i);
						}
						ImGui::TreePop();
					}
					ImGui::PopID();
				}
			}
			ImGui::Unindent();
		}
		ImGui::PopID();
	}
	if (_tpChanges.empty()) {
		Wrapped(theme::Good(), tr("The good save already has everything the bad one has (of what could be read)."));
	}
	ImGui::EndChild();

	// apply
	std::vector<std::size_t> run;
	for (int c = 0; c < static_cast<int>(tp::Category::kCount); ++c) {  // character first, quests last
		for (std::size_t i = 0; i < _tpChanges.size(); ++i) {
			if (static_cast<int>(_tpChanges[i].cat) == c && _tpChanges[i].tick && _tpChanges[i].available) {
				run.push_back(i);
			}
		}
	}
	ImGui::BeginDisabled(_tpRunning);
	ImGui::Checkbox(tr("Mark refused quests as done"), &_tpMarkRefused);
	ImGui::EndDisabled();
	tips::Tip(tr("When the game refuses a quest's stage (its people or places are not set up in this save), the quest is marked\n"
						  "completed instead, and the list says so (\"refused stage 10, marked as done\"). Marked quests give no rewards\n"
						  "and do not start the quests that normally follow them. This applies to quests that were only further along in\n"
						  "the bad save too. Untick to leave refused quests as they are."));
	ImGui::SameLine(0, 30);
	ImGui::BeginDisabled(!live || !helperOk || run.empty() || busy);
	const bool goBtn = ImGui::Button(Fmt(tr("Transplant %zu ticked thing(s) into the game"), run.size()).c_str());
	tips::Tip(tr("Put the ticked things into the game the normal way (the good save must be loaded). Make a save in the game first; then save in the game when it is done for a clean save."));
	if (!busy && _tpResults.empty() && !run.empty() && live && helperOk) {
		Marquee();
		ImGui::SameLine();
		TextColored(IM_COL32(255, 196, 40, 255), tr("<- step 3: go"));
	}
	if (goBtn) {
		confirm(tr("Transplant"),
			Fmt(tr("Put %zu ticked thing(s) into the game now?\n\n"
				"- The GOOD save must be the one loaded in the game (the one snapshotted in step 2).\n"
				"- Make a save in the game first, so you can go back.\n"
				"- After OK, switch to Skyrim and stay there, unpaused with menus closed, until it is done.\n"
				"- Quests may keep loading for a couple of minutes after that: save in the game when their messages stop."),
				run.size()),
			[this, run] {
				_tpRun = run;
				transplantOrderQuests(_tpRun);
				_tpPos = 0;
				_tpResults.clear();
				_tpMissing.clear();
				_tpChecked = false;
				_tpRunning = true;
				transplantStep();
			});
	}
	ImGui::EndDisabled();
	if (_tpRunning) {
		ImGui::SameLine();
		ImGui::ProgressBar(_tpRun.empty() ? 0.0f : static_cast<float>(_tpPos) / static_cast<float>(_tpRun.size()), ImVec2(260, 0),
			Fmt(tr("%zu of %zu"), _tpPos, _tpRun.size()).c_str());
	} else if (!_tpResults.empty()) {
		std::size_t failed = 0;
		for (const auto& r : _tpResults) {
			failed += r.ok ? 0 : 1;
		}
		ImGui::SameLine();
		TextColored(failed ? theme::Caution() : theme::Good(),
			Fmt(tr("Done: %zu brought over, %zu not (see the list). Quests may keep loading for a couple of minutes: save in the game when their "
				"messages stop."),
				_tpResults.size() - failed, failed));
	}
	ImGui::TextDisabled("%s", tr("Nothing is taken away from the good save: things are only added, and levels and skills only raised."));
	ImGui::End();
}
