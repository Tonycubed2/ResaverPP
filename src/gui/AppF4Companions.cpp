// Fallout 4 companions window and the Fallout 4 findings of the Save check (companions, quests whose stage script
// stopped early). The data comes from core/CompanionsFO4.h and core/QuestChecksFO4.h; this file only shows it.
#include "App.h"
#include "Theme.h"
#include "Tips.h"
#include "imgui.h"
#include "core/CompanionsFO4.h"
#include "core/QuestChecksFO4.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace ess;

struct App::F4CompanionsState
{
	const void*   save = nullptr;
	std::uint64_t gen = UINT64_MAX;
	fo4comp::Result r;
	// the quest check needs the plugins (stage scripts, stages, objectives) and the scripts
	const void*   questDb = nullptr;
	std::uint64_t questGen = UINT64_MAX;
	const void*   questSave = nullptr;
	std::vector<fo4quest::Finding> quests;
	fo4quest::Stats                questStats;
	// the window
	std::uint32_t selected = 0;
	std::map<std::uint32_t, float> affinityEdit;
	std::string   status;
	std::string   gameNote;  // the answer of the last record sent to the game
};

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[2048];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}
	void Wrapped(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}
	std::string Lines(const std::vector<std::string>& a_lines)
	{
		std::string s;
		for (const auto& l : a_lines) {
			s += l + "\n";
		}
		return s;
	}
}

// sends one ReSaverHelper (Fallout 4, version 15) APPLY record; the answer is shown in the Companions window
void App::f4compSend(std::vector<std::string> a_record, const std::string& a_what)
{
	auto& st = f4companions();
	std::vector<std::string> cmd{ "APPLY" };
	cmd.insert(cmd.end(), a_record.begin(), a_record.end());
	st.gameNote = a_what + tr(": sending to the game...");
	_status = st.gameNote;
	auto state = _f4comp;
	_live.request(std::move(cmd), [this, state, a_what](const LiveLink::Reply& r) {
		std::string note;
		if (!LiveLink::Ok(r)) {
			note = a_what + tr(": the game could not do it: ") + LiveLink::Error(r);
		} else if (LiveLink::Field(r, 0) == "0") {
			note = a_what + tr(": the game refused: ") + LiveLink::Field(r, 1);
		} else if (LiveLink::Field(r, 0) == "?") {
			note = a_what + tr(": the game is busy; the commands run when it gets to them. Look in the game.");
		} else {
			note = a_what + tr(": the game ran ") + LiveLink::Field(r, 1) +
			       tr(". The console does not say whether the scripts did it: look in the game, then save there and open that save here.");
		}
		state->gameNote = note;
		_status = note;
	});
}

// why the running game cannot take a record now ("" when it can)
std::string App::f4compCannot() const
{
	const auto info = _live.info();
	if (!info.connected) {
		return tr("Needs the game running with ReSaverHelper for Fallout 4 (version 15 or later).");
	}
	if (info.runtime.rfind("F4", 0) != 0) {
		return tr("The game connected is not Fallout 4.");
	}
	if (!helperAtLeast(fo4comp::kCompanionHelper)) {
		return Fmt(tr("Needs ReSaverHelper for Fallout 4 version 15 or later (the game has version %s)."), info.helperVersion.c_str());
	}
	if (!info.inGame) {
		return tr("Load a save in the game first.");
	}
	return {};
}

App::F4CompanionsState& App::f4companions()
{
	if (!_f4comp) {
		_f4comp = std::make_shared<F4CompanionsState>();
	}
	auto& st = *_f4comp;
	if (st.save != _save.get() || st.gen != _editGen) {
		st.save = _save.get();
		st.gen = _editGen;
		st.r = _save && _save->isFO4() ? fo4comp::Read(*_save) : fo4comp::Result{};
		st.affinityEdit.clear();
	}
	// the quest check, once the plugins are read (and again after an edit)
	if (_save && _save->isFO4() && _db && (st.questDb != _db.get() || st.questGen != _editGen || st.questSave != _save.get())) {
		st.questDb = _db.get();
		st.questGen = _editGen;
		st.questSave = _save.get();
		st.questStats = {};
		st.quests = fo4quest::FindStoppedStages(*_save, *_db, _sources, &st.questStats);
	}
	return st;
}

void App::appendCheckItemsF4(std::vector<CheckItem>& a_items)
{
	if (!fo4()) {
		return;
	}
	auto& st = f4companions();
	// test hook: RESAVERPP_TEST_COMPANIONS=<ref hex> opens the Companions window on that companion (the Save check opens with the save)
	if (static bool once = false; !once) {
		once = true;
		if (const char* t = std::getenv("RESAVERPP_TEST_COMPANIONS")) {
			_showCompanionsF4 = true;
			_showCheck = false;
			st.selected = static_cast<std::uint32_t>(std::strtoul(t, nullptr, 16));
		}
	}
	for (const auto& f : st.r.findings) {
		CheckItem it;
		it.level = f.level == fo4comp::Finding::Level::kInfo ? Risk::kInfo : Risk::kCaution;
		it.title = f.title;
		it.what = f.what;
		if (const auto* c = f.ref ? st.r.find(f.ref) : nullptr; c && _db) {
			if (const auto* form = _db->find(c->ref)) {
				auto n = _db->name(*form);
				if (n.empty() && form->base) {
					if (const auto* b = _db->find(form->base)) {
						n = _db->name(*b);
					}
				}
				if (!n.empty()) {
					it.title += " (" + n + ")";
				}
			}
		}
		if (!f.console.empty()) {
			it.what += "\n    " + f.console.front();
			for (std::size_t i = 1; i < f.console.size(); ++i) {
				it.what += "\n    " + f.console[i];
			}
			CheckOption o;
			o.label = tr("Copy the console commands");
			o.risk = Risk::kCaution;
			o.why = f.consoleWhy.empty() ? std::string(tr("Runs the game's own scripts in the running game; this file is not changed.")) : f.consoleWhy;
			const auto text = Lines(f.console);
			o.act = [text] { ImGui::SetClipboardText(text.c_str()); };
			if (!f.record.empty()) {
				// the same through ReSaverHelper (version 15) when the game runs; copying stays for when it does not
				const auto cannot = f4compCannot();
				CheckOption g;
				g.label = tr("Run it in the game");
				g.risk = Risk::kCaution;
				g.why = (cannot.empty() ? std::string() : cannot + " ") + o.why + tr(" ReSaverHelper runs these commands in the loaded game.");
				g.enabled = cannot.empty();
				const auto rec = f.record;
				const auto title = f.title;
				g.act = [this, rec, title] {
					f4compSend(rec, title);
					_showCompanionsF4 = true;
				};
				it.options.push_back(std::move(g));
			}
			it.options.push_back(std::move(o));
		}
		// a value outside the script's range can be put back in the save
		if (const auto* c = f.ref ? st.r.find(f.ref) : nullptr; c && c->hasAffinity && c->hasRange &&
																   (c->affinity > c->maxAffinity || c->affinity < c->minAffinity)) {
			const float v = std::clamp(c->affinity, c->minAffinity, c->maxAffinity);
			CheckOption o;
			o.label = Fmt(tr("Set its affinity to %g in the save"), v);
			o.risk = Risk::kCaution;
			o.why = tr("Changes only the saved CA_Affinity value (and the highest/lowest reached, as the scripts do). Threshold scenes "
			           "and the companion perk follow the next time affinity changes in the game.");
			const auto ref = c->ref;
			o.act = [this, ref, v] {
				const auto why = fo4comp::SetAffinity(*_save, ref, v);
				if (why.empty()) {
					markEdited(Fmt(tr("Set the affinity of %08X to %g"), ref, v));
				} else {
					_f4comp->status = why;
				}
			};
			it.options.push_back(std::move(o));
		}
		CheckOption w;
		w.label = tr("Open Companions");
		w.risk = Risk::kSafe;
		w.why = tr("Only shows information.");
		const auto ref = f.ref;
		w.act = [this, ref] {
			_showCompanionsF4 = true;
			_f4comp->selected = ref;
			_showCheck = false;
		};
		it.options.push_back(std::move(w));
		a_items.push_back(std::move(it));
	}
	for (const auto& q : st.quests) {
		CheckItem it;
		it.level = Risk::kCaution;
		it.title = q.title;
		it.what = q.what;
		for (const auto& c : q.console) {
			it.what += "\n    " + c;
		}
		CheckOption o;
		o.label = tr("Copy the console command");
		o.risk = Risk::kCaution;
		o.why = tr("For the running game (open the console with ~): it does what the stage script would have done at that line. The "
		           "lines after it in that script did not run either; if the quest still does not go on, look at that script. Save "
		           "in the game first.");
		const auto text = Lines(q.console);
		o.act = [text] { ImGui::SetClipboardText(text.c_str()); };
		it.options.push_back(std::move(o));
		CheckOption s;
		s.label = tr("Show the Quests page");
		s.risk = Risk::kSafe;
		s.why = tr("Only shows information.");
		s.act = [this] {
			_cat = Cat::kQuests;
			_showCheck = false;
		};
		it.options.push_back(std::move(s));
		a_items.push_back(std::move(it));
	}
}

void App::drawCompanionsF4()
{
	if (!fo4()) {
		_showCompanionsF4 = false;
		return;
	}
	prepareWindow("Companions", ImVec2(0.7f, 0.8f));
	if (!ImGui::Begin(trId("Companions"), &_showCompanionsF4, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Companions");
	auto&       st = f4companions();
	const auto& r = st.r;
	const auto& sys = r.system;
	const auto  quest = sys.quest ? Fmt("%08X", sys.quest) : std::string(fo4comp::kFollowersQuest);
	auto        nameOf = [&](std::uint32_t a_id) -> std::string {
        if (!a_id || !_db) {
            return {};
        }
        const auto* f = _db->find(a_id);
        if (!f) {
            return {};
        }
        auto n = _db->name(*f);
        if (n.empty() && f->base) {
            if (const auto* b = _db->find(f->base)) {
                n = _db->name(*b);
            }
        }
        return n.empty() ? _db->edid(*f) : n;
	};
	auto label = [&](std::uint32_t a_id) {
		const auto n = nameOf(a_id);
		return n.empty() ? Fmt("%08X", a_id) : Fmt("%s (%08X)", n.c_str(), a_id);
	};

	ImGui::TextWrapped("%s", tr("Every companion of this save (actors with CompanionActorScript or DogmeatActorScript, mod companions "
	                            "too), read from the save: affinity, romance, whether it left for good, where it is, and the follower "
	                            "quest's companion slots. Fixes the save cannot make consistently are given as console commands for the "
	                            "running game."));
	if (!sys.found) {
		Wrapped(theme::Caution(), tr("The follower quest's script (FollowersScript) is not in this save."));
	} else {
		ImGui::Text("%s %s", tr("Companion:"), sys.companionFill ? label(sys.companionFill).c_str() : tr("nobody"));
		ImGui::SameLine(0, 30);
		ImGui::Text("%s %s", tr("Dogmeat slot:"), sys.dogmeatFill ? label(sys.dogmeatFill).c_str() : tr("nobody"));
		ImGui::SameLine(0, 30);
		ImGui::Text(tr("Have been companions: %zu"), sys.active.size());
	}
	for (const auto& n : r.notes) {
		Wrapped(theme::Dim(), n);
	}
	if (!st.status.empty()) {
		Wrapped(theme::Caution(), st.status);
	}
	if (!st.gameNote.empty()) {
		Wrapped(theme::Good(), st.gameNote);
	}
	if (ImGui::Button(tr("Gather companions in the running game"))) {
		shortcut(Sc::kGather);
	}
	tips::Tip(tr("ReSaverHelper brings your followers next to you in the running game (the game's own teleport)."));
	// findings about the follower quest's slots themselves (a slot held by an actor that is not a companion, two claimants):
	// they belong to no row of the table below
	{
		const auto cannot = f4compCannot();
		int        k = 0;
		for (const auto& f : r.findings) {
			if (f.ref && r.find(f.ref)) {
				continue;
			}
			ImGui::PushID(k++);
			Wrapped(f.level == fo4comp::Finding::Level::kInfo ? theme::Dim() : theme::Caution(), f.title);
			ImGui::TextWrapped("%s", f.what.c_str());
			for (const auto& cmd : f.console) {
				ImGui::BulletText("%s", cmd.c_str());
			}
			if (!f.record.empty()) {
				ImGui::BeginDisabled(!cannot.empty());
				if (ImGui::Button(tr("Run in the game: empty the slot"))) {
					f4compSend(f.record, f.title);
				}
				ImGui::EndDisabled();
				tips::Tip(cannot.empty() ? (tr("ReSaverHelper runs these commands in the loaded game. ") + f.consoleWhy).c_str() : cannot.c_str());
				ImGui::SameLine();
			}
			if (!f.console.empty()) {
				if (ImGui::Button(tr("Copy the console commands"))) {
					ImGui::SetClipboardText(Lines(f.console).c_str());
				}
				if (!f.consoleWhy.empty()) {
					tips::Tip(f.consoleWhy.c_str());
				}
			}
			ImGui::PopID();
		}
	}
	ImGui::Separator();

	const float detailH = ImGui::GetContentRegionAvail().y * 0.45f;
	if (ImGui::BeginTable("##f4comp", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
			ImVec2(0, ImGui::GetContentRegionAvail().y - detailH))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Companion"));
		ImGui::TableSetupColumn(tr("Reference"));
		ImGui::TableSetupColumn(tr("State"));
		ImGui::TableSetupColumn(tr("Affinity"));
		ImGui::TableSetupColumn(tr("Romance"));
		ImGui::TableSetupColumn(tr("Where"));
		ImGui::TableHeadersRow();
		for (const auto& c : r.companions) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			const auto n = nameOf(c.ref);
			if (ImGui::Selectable(Fmt("%s##%08X", n.empty() ? (c.dogmeat ? "(Dogmeat)" : "-") : n.c_str(), c.ref).c_str(), st.selected == c.ref, ImGuiSelectableFlags_SpanAllColumns)) {
				st.selected = c.ref;
			}
			ImGui::TableNextColumn();
			ImGui::Text("%08X", c.ref);
			ImGui::TableNextColumn();
			std::string state;
			if (c.current()) {
				state = c.hasFollowerState && c.followerState == fo4comp::kWait   ? tr("companion, waiting")
				        : c.hasFollowerState && c.followerState == fo4comp::kGoHome ? tr("companion, sent home")
				                                                                    : tr("your companion");
			} else if (c.hasLeftPermanently) {
				state = tr("left for good");
			} else if (c.disallowed) {
				state = tr("not allowed now");
			} else if (c.inActive || c.hasBeen) {
				state = tr("has been your companion");
			} else {
				state = tr("not recruited");
			}
			ImGui::TextUnformatted(state.c_str());
			ImGui::TableNextColumn();
			if (c.dogmeat) {
				ImGui::TextDisabled("-");
			} else if (c.hasAffinity) {
				ImGui::Text("%.0f", c.affinity);
			} else {
				ImGui::TextDisabled("%s", tr("not saved"));
			}
			ImGui::TableNextColumn();
			std::string rom;
			if (c.hasRomantic && c.romantic == 1.0f) {
				rom = tr("romanced");
			} else if (c.infatuated) {
				rom = tr("infatuated");
			}
			ImGui::TextUnformatted(rom.c_str());
			ImGui::TableNextColumn();
			if (c.refRead && c.cell) {
				ImGui::TextUnformatted(label(c.cell).c_str());
			} else {
				ImGui::TextDisabled("%s", c.hasChangeForm ? tr("not read") : tr("where the plugin puts it"));
			}
		}
		ImGui::EndTable();
	}

	ImGui::BeginChild("##f4compdetail", ImVec2(0, 0), true);
	const auto* c = r.find(st.selected);
	if (!c) {
		ImGui::TextDisabled("%s", tr("Pick a companion above."));
	} else {
		ImGui::Text("%s  -  %s", label(c->ref).c_str(), c->script.c_str());
		// the record carries the save's form id: the game's own for Fallout4.esm (always index 00) and for objects of this
		// save's playthrough; for a DLC or mod companion only when the game runs the save's load order
		const auto  top = c->ref >> 24;
		std::string idNote = top == 0 || top == 0xFF ? std::string()
		                                             : std::string(tr("Its form id comes from this save's load order: the game must run the same "
		                                                              "plugins in the same order (load this save, or a later one with the same mods)."));
		const auto cannot = f4compCannot();
		if (!c->dogmeat) {
			if (c->hasAffinity) {
				ImGui::Text(tr("Affinity %.1f"), c->affinity);
			} else {
				ImGui::TextDisabled("%s", tr("No affinity saved yet: the plugin's value applies until the companion's script first changes it."));
			}
			if (c->hasThreshold) {
				ImGui::SameLine(0, 20);
				ImGui::Text(tr("threshold reached %.0f"), c->threshold);
			}
			if (c->hasRange) {
				ImGui::SameLine(0, 20);
				ImGui::TextDisabled(tr("(script range %.0f to %.0f)"), c->minAffinity, c->maxAffinity);
			}
			std::string flags;
			auto        add = [&](const char* s) { flags += (flags.empty() ? "" : ", ") + std::string(s); };
			if (c->hasRomantic && c->romantic == 1.0f) add(tr("romanced (CA_IsRomantic 1)"));
			if (c->infatuated) add(tr("infatuated (its last threshold is the infatuation one)"));
			if (c->perkUnlocked) add(tr("companion perk unlocked"));
			if (c->hasLeftPermanently) add(tr("left for good (HasLeftPlayerPermanently)"));
			if (c->disallowed) add(tr("in DisallowedCompanionFaction"));
			if (!flags.empty()) {
				ImGui::TextWrapped("%s", flags.c_str());
			}
		}
		if (c->hasFollowerState) {
			ImGui::Text(tr("FollowerState %g (1 follow, 2 wait, 4 go home)"), c->followerState);
		}
		if (c->refRead) {
			ImGui::Text(tr("Saved at %s, position %.0f, %.0f, %.0f"), c->cell ? label(c->cell).c_str() : "?", c->pos[0], c->pos[1], c->pos[2]);
		}
		ImGui::Text(tr("Follower quest companion slot: %s; its own alias list names the slot: %s"), c->inCompanionAlias ? tr("yes") : tr("no"),
			c->aliasExtraSaysCompanion ? tr("yes") : tr("no"));
		ImGui::Separator();
		if (!c->dogmeat) {
			auto& v = st.affinityEdit.try_emplace(c->ref, c->hasAffinity ? c->affinity : 0.0f).first->second;
			ImGui::SetNextItemWidth(160);
			ImGui::InputFloat(tr("New affinity"), &v, 50, 250, "%.0f");
			ImGui::SameLine();
			ImGui::BeginDisabled(!c->hasAffinity || !c->actorRead || _readOnly);
			if (ImGui::Button(tr("Set in the save"))) {
				const auto ref = c->ref;
				const float val = v;
				confirm(tr("Set affinity"),
					Fmt(tr("Set the affinity of %s to %g in the save?\n\nOnly the saved value changes (and the highest/lowest reached, as "
					       "the scripts do). Threshold scenes, the companion perk and quest stages follow the next time its affinity "
					       "changes in the game. Write the save with File > Save As and load that."),
						label(ref).c_str(), val),
					[this, ref, val] {
						const auto why = fo4comp::SetAffinity(*_save, ref, val);
						if (why.empty()) {
							markEdited(Fmt(tr("Set the affinity of %08X to %g"), ref, val));
							_f4comp->status.clear();
						} else {
							_f4comp->status = why;
						}
					});
			}
			ImGui::EndDisabled();
			tips::Tip(c->hasAffinity ? tr("Writes the new value into the save (CA_Affinity).")
			                         : tr("The save has no affinity value for this companion yet: use the console command instead."));
			ImGui::SameLine();
			ImGui::BeginDisabled(!cannot.empty());
			if (ImGui::Button(tr("Run in the game##aff"))) {
				const int n = static_cast<int>(v < 0 ? v - 0.5f : v + 0.5f);
				f4compSend(fo4comp::AffinityRecord(*c, n), Fmt(tr("Affinity of %s to %d"), label(c->ref).c_str(), n));
			}
			ImGui::EndDisabled();
			tips::Tip(cannot.empty() ? (tr("ReSaverHelper runs it in the game: the companion's own script sets the affinity "
			                               "(CompanionActorScript.SetAffinity), with its threshold scenes and perk as in normal play. ") + idNote).c_str()
			                         : cannot.c_str());
			ImGui::SameLine();
			if (ImGui::Button(tr("Copy console command##aff"))) {
				ImGui::SetClipboardText(fo4comp::SetAffinityCommand(*c, v).c_str());
			}
			tips::Tip(tr("For the running game: the companion's own script sets the affinity (CompanionActorScript.SetAffinity), with "
			             "its threshold scenes and perk as in normal play. The scripts hold it below a threshold whose scene has not "
			             "played yet."));
		}
		ImGui::BeginDisabled(!cannot.empty());
		if (ImGui::Button(tr("Run in the game: make it my companion again"))) {
			f4compSend(fo4comp::CompanionRecord(*c), Fmt(tr("Make %s your companion"), label(c->ref).c_str()));
		}
		ImGui::EndDisabled();
		tips::Tip(cannot.empty() ? (tr("ReSaverHelper runs the game's own FollowersScript.AllowCompanion (and clears 'left for good' "
		                               "first when it is set). Your current companion is dismissed. ") + idNote).c_str()
		                         : cannot.c_str());
		ImGui::SameLine();
		ImGui::BeginDisabled(!cannot.empty());
		if (ImGui::Button(tr("Run in the game: follow me"))) {
			f4compSend(fo4comp::FollowRecord(*c), Fmt(tr("%s: follow"), label(c->ref).c_str()));
		}
		ImGui::EndDisabled();
		tips::Tip(cannot.empty() ? (tr("ReSaverHelper runs FollowersScript.FollowerFollow: a companion told to wait or sent home follows "
		                               "you again. ") + idNote).c_str()
		                         : cannot.c_str());
		if (ImGui::Button(tr("Copy commands: make it my companion again"))) {
			ImGui::SetClipboardText(Lines(fo4comp::MakeCompanionCommands(*c, quest)).c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button(tr("Copy command: follow me"))) {
			ImGui::SetClipboardText(fo4comp::FollowCommand(*c, quest).c_str());
		}
		tips::Tip(tr("For the running game (console ~): FollowersScript.AllowCompanion with ForceCompanion, the game's own way to make an "
		             "actor your companion (out of DisallowedCompanionFaction, the companion slot, follow, teammate); clears 'left for "
		             "good' first when it is set. Your current companion is dismissed. The save file cannot do this consistently: "
		             "it is the scripts' work (teammate flag, keywords, event registrations), not stored values."));
		for (const auto& f : r.findings) {
			if (f.ref != c->ref) {
				continue;
			}
			ImGui::Separator();
			Wrapped(f.level == fo4comp::Finding::Level::kInfo ? theme::Dim() : theme::Caution(), f.title);
			ImGui::TextWrapped("%s", f.what.c_str());
			for (const auto& cmd : f.console) {
				ImGui::BulletText("%s", cmd.c_str());
			}
		}
	}
	ImGui::EndChild();
	ImGui::End();
}
