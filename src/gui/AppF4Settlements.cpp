// Fallout 4 settlements: every workshop with its build budget, population and happiness, a reset of the used budget,
// and the settlement findings of the Save check (see core/WorkshopFO4.h for where the game keeps all this).
// The settlements are read on a thread of their own from a copy of the save (big saves take a second or two).
#include "App.h"
#include "Theme.h"
#include "Tips.h"
#include "imgui.h"
#include "core/WorkshopFO4.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>

using namespace ess;

struct App::WsState
{
	// the background read
	struct Result
	{
		std::vector<fo4ws::Settlement> list;
		std::vector<fo4ws::Finding>    findings;
	};
	struct Job
	{
		std::mutex        m;
		std::atomic<bool> done{ false };
		Result            result;
	};
	std::shared_ptr<Job> job;
	const void*          jobSave = nullptr;
	std::uint64_t        jobGen = UINT64_MAX;
	Result               have;  // the last finished read
	bool                 haveAny = false;

	// the window
	std::set<std::uint32_t> ticked;
	std::uint32_t           editing = 0;  // the workshop whose numbers are typed in
	int                     editDraws = 0, editTriangles = 0;
	std::string             message;
};

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	// a copy of what fo4ws::List / Check read: the change forms, the form id table, the plugins and the Papyrus section
	std::unique_ptr<SaveFile> Snapshot(const SaveFile& a_save)
	{
		auto s = std::make_unique<SaveFile>();
		s->header = a_save.header;
		s->formVersion = a_save.formVersion;
		s->gameVersion = a_save.gameVersion;
		s->plugins = a_save.plugins;
		s->changeForms = a_save.changeForms;
		s->formIDs = a_save.formIDs;
		if (const auto* p = a_save.papyrus()) {
			GlobalData g;
			g.type = 1001;
			g.papyrus = std::make_unique<Papyrus>(*p);
			s->table3.push_back(std::move(g));
		}
		return s;
	}
}

// starts a read when the save or its edits changed; takes a finished one. True while there is nothing to show yet.
bool App::settlementsPoll()
{
	if (!_ws) {
		_ws = std::make_shared<WsState>();
		if (std::getenv("RESAVERPP_TEST_SETTLEMENTS")) {  // test hook: open the window with the save
			_showSettlements = true;
		}
	}
	auto& w = *_ws;
	if (!fo4()) {
		w.job.reset();
		w.haveAny = false;
		w.jobSave = nullptr;
		return true;
	}
	if (w.jobSave != _save.get() || w.jobGen != _editGen) {
		if (w.jobSave != _save.get()) {
			w.haveAny = false;
			w.ticked.clear();
			w.editing = 0;
			w.message.clear();
		}
		w.jobSave = _save.get();
		w.jobGen = _editGen;
		auto job = std::make_shared<WsState::Job>();
		w.job = job;
		std::shared_ptr<SaveFile> snap = Snapshot(*_save);
		std::thread([job, snap] {
			WsState::Result r;
			try {
				r.findings = fo4ws::Check(*snap);
				r.list = fo4ws::List(*snap);
			} catch (const std::exception&) {
			}
			std::lock_guard l(job->m);
			job->result = std::move(r);
			job->done = true;
		}).detach();
	}
	if (w.job && w.job->done) {
		std::lock_guard l(w.job->m);
		w.have = std::move(w.job->result);
		w.haveAny = true;
		w.job.reset();
	}
	return !w.haveAny;
}

std::string App::settlementName(const fo4ws::Settlement& a_s) const
{
	if (_db && a_s.location) {
		if (const auto* f = _db->find(a_s.location)) {
			const auto n = _db->name(*f);
			if (!n.empty()) {
				return n;
			}
			if (const auto e = _db->edid(*f); !e.empty()) {
				return e;
			}
		}
	}
	return a_s.location ? Fmt(tr("location %08X"), a_s.location) : Fmt(tr("workshop %08X"), a_s.formID);
}

void App::settlementCheckItems(std::vector<CheckItem>& a_items)
{
	if (!fo4() || settlementsPoll()) {
		return;
	}
	const auto& r = _ws->have;
	std::vector<const fo4ws::Settlement*> over;
	for (const auto& s : r.list) {
		if (s.hasStorage && s.overBudget()) {
			over.push_back(&s);
		}
	}
	if (!over.empty()) {
		CheckItem it;
		it.level = Risk::kCaution;
		it.title = Fmt(tr("%zu settlement(s) are over their build budget"), over.size());
		std::string list;
		for (const auto* s : over) {
			list += Fmt(tr("\n  - %s: %lld / %lld draws, %lld / %lld triangles"), settlementName(*s).c_str(), static_cast<long long>(s->usedDraws()),
				static_cast<long long>(s->limitDraws()), static_cast<long long>(s->usedTriangles()), static_cast<long long>(s->limitTriangles()));
		}
		it.what = tr("Their build meter is full, so nothing more can be built there.") + list +
		          tr("\nThe game does not count the objects: it keeps a running total that building adds to and scrapping or storing takes "
		             "from (never below 0). Settlements tells you more and can reset that total.");
		CheckOption o;
		o.label = tr("Open Settlements");
		o.risk = Risk::kSafe;
		o.why = tr("Only shows information.");
		o.act = [this] {
			_showSettlements = true;
			_showCheck = false;
			_ws->ticked.clear();
			for (const auto& s : _ws->have.list) {
				if (s.hasStorage && s.overBudget() && s.readable) {
					_ws->ticked.insert(s.formID);
				}
			}
		};
		it.options.push_back(std::move(o));
		a_items.push_back(std::move(it));
	}
	std::size_t missing = 0, badID = 0, mismatch = 0;
	std::string mList, sList;
	for (const auto& f : r.findings) {
		switch (f.kind) {
		case fo4ws::Finding::Kind::kMissingWorkshop:
			if (++missing <= 20) mList += "\n  - " + f.text;
			break;
		case fo4ws::Finding::Kind::kSettlerBadID:
			if (++badID + mismatch <= 20) sList += "\n  - " + f.text;
			break;
		case fo4ws::Finding::Kind::kSettlerMismatch:
			if (badID + ++mismatch <= 20) sList += "\n  - " + f.text;
			break;
		default: break;
		}
	}
	if (missing) {
		CheckItem it;
		it.level = Risk::kCaution;
		it.title = Fmt(tr("%zu workshop object(s) or settler(s) are linked to a workshop that is not in the game"), missing);
		it.what = tr("Each workshop object and settler is linked to its settlement's workbench (the WorkshopItemKeyword link). These name "
		             "a workbench that was a created object no longer in the save, or one from a plugin the save does not have, so the "
		             "workshop scripts get None for them.") + mList;
		a_items.push_back(std::move(it));
	}
	if (badID || mismatch) {
		CheckItem it;
		it.level = Risk::kCaution;
		it.title = Fmt(tr("%zu settler(s) have workshop data that does not add up"), badID + mismatch);
		it.what = tr("A settler's workshopID (WorkshopNPCScript) is its settlement's place in WorkshopParent's list of workshops, and the "
		             "game links the settler to that same workbench. These name no workshop in that list, or a different workbench "
		             "than the one they are linked to, so the settlement scripts may count them wrongly or not find them.") + sList;
		a_items.push_back(std::move(it));
	}
}

void App::drawSettlementsF4()
{
	if (!_ws) {
		settlementsPoll();  // makes the state (and reads the test hook)
	}
	auto& w = *_ws;
	if (!_showSettlements || !_save) {
		return;
	}
	if (!fo4()) {
		_showSettlements = false;
		return;
	}
	prepareWindow("Settlements", ImVec2(0.7f, 0.75f));
	if (!ImGui::Begin(trId("Settlements"), &_showSettlements, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Settlements");
	ImGui::NewLine();
	ImGui::TextWrapped("%s", tr("Every settlement workshop in this save with its build budget, as the game's build meter counts it: "
	                   "the larger of draws used / max and triangles used / max. The game does not count the objects in a settlement. "
	                   "It keeps a running total on the workbench: building adds the object's draws and triangles, scrapping or storing "
	                   "takes them off (never below 0), and the total also starts with what the settlement had built when you got it."));
	ImGui::TextWrapped("%s", tr("Reset used budget sets that total to 0 (or to the numbers you type). The objects already built stay "
	                   "where they are but are no longer counted, so the whole budget is free again; scrapping them later takes their "
	                   "numbers off again, but never below 0. The max values are not changed. Nothing is written until you save "
	                   "(File > Save or Save As)."));
	if (settlementsPoll()) {
		ImGui::Spacing();
		ImGui::TextDisabled("%s", tr("Reading the settlements..."));
		ImGui::End();
		return;
	}
	const auto& list = w.have.list;
	if (!w.message.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Caution());
		ImGui::TextWrapped("%s", w.message.c_str());
		ImGui::PopStyleColor();
	}
	if (list.empty()) {
		ImGui::TextDisabled("%s", tr("This save has no settlement workshops (no WorkshopScript on any reference)."));
		ImGui::End();
		return;
	}
	auto resetTicked = [&] {
		std::size_t done = 0;
		std::string err;
		for (const auto id : w.ticked) {
			bool       changed = false;
			const auto why = fo4ws::SetUsed(*_save, id, 0.f, 0.f, &changed);
			if (!why.empty()) {
				err += (err.empty() ? "" : "\n") + why;
			} else if (changed) {
				++done;
			}
		}
		w.message = err;
		if (done) {
			markChanged(Fmt(tr("Reset the used build budget of %zu settlement(s)"), done));
		}
	};
	ImGui::Separator();
	if (ImGui::Button(tr("Tick the ones over budget"))) {
		w.ticked.clear();
		for (const auto& s : list) {
			if (s.readable && s.hasStorage && s.overBudget()) w.ticked.insert(s.formID);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(tr("Tick all"))) {
		for (const auto& s : list) {
			if (s.readable && s.hasStorage) w.ticked.insert(s.formID);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(tr("Untick all"))) {
		w.ticked.clear();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(w.ticked.empty() || _readOnly);
	if (ImGui::Button(Fmt(tr("Reset used budget of %zu ticked"), w.ticked.size()).c_str())) {
		resetTicked();
	}
	ImGui::EndDisabled();
	tips::Tip(tr("Set the draws and triangles used of the ticked settlements to 0. Their objects stay; the game just stops counting "
	          "them, so the whole budget is free again. Max values are left alone."));

	const float tableH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
	if (ImGui::BeginTable("ws", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
			ImVec2(0, std::max(120.0f, tableH)))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(tr("Settlement"));
		ImGui::TableSetupColumn(tr("Workbench"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(tr("Draws used / max"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(tr("Triangles used / max"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(tr("Build meter"), ImGuiTableColumnFlags_WidthFixed, 140);
		ImGui::TableSetupColumn(tr("Population"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(tr("Happiness"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn(tr("Linked"), ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableHeadersRow();
		for (const auto& s : list) {
			ImGui::PushID(static_cast<int>(s.formID));
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			const bool editable = s.readable && s.hasStorage && !_readOnly;
			bool       on = w.ticked.contains(s.formID);
			ImGui::BeginDisabled(!editable);
			if (ImGui::Checkbox("##t", &on)) {
				if (on) w.ticked.insert(s.formID);
				else w.ticked.erase(s.formID);
			}
			ImGui::EndDisabled();
			if (!s.readable) {
				tips::Tip((tr("This workbench cannot be edited: ") + s.error).c_str());
			} else if (!s.hasStorage) {
				tips::Tip(tr("The save keeps no budget numbers for this workbench yet: nothing is used."));
			}
			ImGui::TableNextColumn();
			if (ImGui::Selectable(settlementName(s).c_str(), w.editing == s.formID, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
				w.editing = s.formID;
				w.editDraws = static_cast<int>(s.usedDraws());
				w.editTriangles = static_cast<int>(s.usedTriangles());
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", Fmt(tr("%s (%s)\nWorkshopID %d, %s\nClick to type other numbers below."), s.script.c_str(),
					s.location ? Fmt("%08X", s.location).c_str() : "-", s.workshopID, s.ownedByPlayer ? tr("owned by the player") : tr("not owned by the player")).c_str());
			}
			ImGui::TableNextColumn();
			ImGui::Text("%08X", s.formID);
			ImGui::TableNextColumn();
			ImGui::Text("%lld / %lld", static_cast<long long>(s.usedDraws()), static_cast<long long>(s.limitDraws()));
			ImGui::TableNextColumn();
			ImGui::Text("%lld / %lld", static_cast<long long>(s.usedTriangles()), static_cast<long long>(s.limitTriangles()));
			ImGui::TableNextColumn();
			const double m = s.meter();
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, m >= 1.0 ? theme::Bad() : m >= 0.9 ? theme::Caution() : theme::Good());
			ImGui::ProgressBar(static_cast<float>(std::min(1.0, m)), ImVec2(-1, 0), Fmt("%.0f%%", std::floor(m * 100.0)).c_str());
			ImGui::PopStyleColor();
			ImGui::TableNextColumn();
			ImGui::Text("%.0f", s.population);
			ImGui::TableNextColumn();
			ImGui::Text("%.0f", s.happiness);
			ImGui::TableNextColumn();
			ImGui::Text("%zu / %zu", s.linkedObjects, s.linkedActors);
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", tr("Objects / actors in the save linked to this workbench (WorkshopItemKeyword). Objects from the "
				                  "plugins that the save never changed are not counted."));
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	const fo4ws::Settlement* cur = nullptr;
	for (const auto& s : list) {
		if (s.formID == w.editing) cur = &s;
	}
	if (cur) {
		ImGui::TextUnformatted(settlementName(*cur).c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		ImGui::InputInt(tr("draws##ed"), &w.editDraws, 10, 100);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(160);
		ImGui::InputInt(tr("triangles##ed"), &w.editTriangles, 1000, 100000);
		w.editDraws = std::max(0, w.editDraws);
		w.editTriangles = std::max(0, w.editTriangles);
		ImGui::SameLine();
		ImGui::BeginDisabled(!cur->readable || _readOnly || (w.editDraws == cur->usedDraws() && w.editTriangles == cur->usedTriangles()));
		if (ImGui::Button(tr("Set used budget"))) {
			bool       changed = false;
			const auto why = fo4ws::SetUsed(*_save, cur->formID, static_cast<float>(w.editDraws), static_cast<float>(w.editTriangles), &changed);
			w.message = why;
			if (why.empty() && changed) {
				markChanged(Fmt(tr("Set the used build budget of %s to %d draws, %d triangles"), settlementName(*cur).c_str(), w.editDraws, w.editTriangles));
			}
		}
		ImGui::EndDisabled();
		tips::Tip(tr("Write these numbers as the settlement's used draws and triangles (the running total the game adds to and takes from)."));
	} else {
		ImGui::TextDisabled("%s", tr("Click a settlement to type its used numbers yourself."));
	}
	ImGui::End();
}
