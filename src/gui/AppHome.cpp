// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The main screen's shortcut buttons: the Clean and Tools menu entries (and Open script) as buttons under
// the overview, so players do not have to dig through the menus. Choosing one (or anything from the top
// menu) hides them and the tool opens as usual; "Main menu" on the menu bar brings them back.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/Ops.h"

#include "imgui.h"

#include <cmath>
#include <set>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace ess;

namespace
{
	// a still border around the big features, in a shade of the button colour
	void DrawMarquee(ImVec2 a_min, ImVec2 a_max)
	{
		auto*        dl = ImGui::GetWindowDrawList();
		const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_Button);
		const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
		float        hh, ss, vv;
		ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, hh, ss, vv);
		// dark themes: a lighter shade of the button colour. Light themes, where the buttons are already pale and
		// a lighter shade would vanish into the white page: the same hue, stronger, so the border shows.
		if (0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z < 0.5f) {
			ss = ss * 0.75f;
			vv = vv + (1.0f - vv) * 0.55f;
		} else {
			ss = std::max(ss, 0.55f);
			vv = std::min(vv, 0.80f);
		}
		float r, g, b;
		ImGui::ColorConvertHSVtoRGB(hh, ss, vv, r, g, b);
		const float pad = 2.0f;
		dl->AddRect(ImVec2(a_min.x - pad, a_min.y - pad), ImVec2(a_max.x + pad, a_max.y + pad),
			ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f)), ImGui::GetStyle().FrameRounding + pad, 0, 2.0f);
	}
}

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[512];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	enum class HomeRisk
	{
		kNone,     // looks, does not change the save
		kSafe,     // green
		kCaution,  // yellow: read what it does first
		kRisky     // red
	};
}

bool App::anyToolOpen() const
{
	return _showTransplant || _missingOpen || _orphansOpen || _showAnalysis || _purgeOpen || _showLookup || _showActors || _showPlayer ||
	       _showCompare || _batchOpen || _reportOpen || _showArrays || _showViewer || _removeOpen || _showLeftovers || _showHistory ||
	       _showPapyrusLog || _showSavesMgr || _showMerge || _showAssets || _showArchive || _showCrashLog;
}

void App::goHome()
{
	_showTransplant = _missingOpen = _orphansOpen = _showAnalysis = _purgeOpen = _showLookup = _showActors = _showPlayer = false;
	_showCompare = _batchOpen = _reportOpen = _showArrays = _removeOpen = false;
	_showViewer = false;
	_showCheck = false;
	_showLeftovers = _showHistory = _showPapyrusLog = _showSavesMgr = _showMerge = _showAssets = _showArchive = false;
	_showCrashLog = false;
	if (_cat != Cat::kOverview) {
		select(Cat::kOverview, 0);
	}
	_homePanel = true;
}

void App::shortcut(Sc a_which)
{
	if (!_save && a_which != Sc::kOpenPex) {
		return;
	}
	_homePanel = false;  // the chosen tool gets the screen
	auto* p = _save ? _save->papyrus() : nullptr;
	switch (a_which) {
	case Sc::kRemoveUnattached:
		confirm("Remove unattached instances", Fmt("Delete %zu script instances that are attached to nothing?", _report ? _report->unattached : 0),
			[this, p] { markEdited(Fmt("Removed %zu unattached instances", ops::RemoveUnattachedInstances(*p))); });
		break;
	case Sc::kRemoveUndefined:
		confirm("Remove undefined elements",
			"Delete scripts whose .pex is no longer installed, their instances and references, and terminate threads running them?",
			[this, p] { markEdited(Fmt("Removed/terminated %zu undefined elements", ops::RemoveUndefinedElements(*p))); });
		break;
	case Sc::kTerminateUndefined:
		confirm("Terminate undefined threads", "Terminate every thread that runs a script that is no longer installed?",
			[this, p] { markEdited(Fmt("Terminated %zu undefined threads", ops::TerminateUndefinedThreads(*p))); });
		break;
	case Sc::kMissingMods:
		_missingDismissed = false;  // the banner comes back too
		for (auto& m : _missing) {
			m.planned = false;
		}
		_missingOpen = true;
		break;
	case Sc::kOrphans: openOrphans(); break;
	case Sc::kMcm: fixMcmJam(); break;
	case Sc::kHavok: resetHavok(); break;
	case Sc::kPurify: purifyFormLists(); break;
	case Sc::kBatch: _batchOpen = true; break;
	case Sc::kPurge:
		_purgeName[0] = 0;
		_purgeOpen = true;
		break;
	case Sc::kPlayer:
		_showPlayer = true;
		_pvLoaded = false;
		break;
	case Sc::kActors: _showActors = true; break;
	case Sc::kTransplant: _showTransplant = true; break;
	case Sc::kLookup: _showLookup = true; break;
	case Sc::kMods: select(Cat::kMods, 0); break;
	case Sc::kQuests: select(Cat::kQuests, 0); break;
	case Sc::kCompare:
		if (platform::HasNativeDialogs()) {
			if (auto f = platform::OpenSaveDialog(_path.parent_path())) {
				compareWith(*f);
			}
		} else if (const char* other = std::getenv("RESAVERPP_TEST_COMPARE")) {
			compareWith(other);
		}
		break;
	case Sc::kReport: _reportOpen = true; break;
	case Sc::kAnalysis: _showAnalysis = true; break;
	case Sc::kArrays:
		_showArrays = true;
		_arraysDirty = true;
		break;
	case Sc::kLeftovers:
		_showLeftovers = true;
		_leftoversDirty = true;
		break;
	case Sc::kHistory: _showHistory = true; break;
	case Sc::kPapyrusLog:
		_showPapyrusLog = true;
		_plogLoaded = false;
		break;
	case Sc::kCrashLog:
		_showCrashLog = true;
		_clogLoaded = false;
		break;
	case Sc::kSavesMgr:
		_showSavesMgr = true;
		_savesLoaded = false;
		break;
	case Sc::kMerge: _showMerge = true; break;
	case Sc::kFileConflicts: _showAssets = true; break;
	case Sc::kOpenPex:
		if (platform::HasNativeDialogs()) {
			if (auto f = platform::OpenFileDialog(_sourcesPath.empty() ? std::filesystem::path() : platform::FromUtf8(_sourcesPath), "pex",
					"Open compiled Papyrus script")) {
				openScript(*f);
			}
		} else if (const char* s = std::getenv("RESAVERPP_TEST_PEX")) {
			openScript(s);
		}
		break;
	}
}

void App::drawHome()
{
	struct Button
	{
		Sc          which;
		std::string label;
		const char* line;  // one short line under the button
		const char* tip;
		HomeRisk    risk;
		long long   count = -1;  // -1: no count; 0: nothing to do (greyed out)
	};
	const auto* r = _report.get();
	const auto  n = [&](std::size_t v) { return r ? static_cast<long long>(v) : -1LL; };
	const bool  anyMissing = !_missing.empty() || !_missingScripts.empty() || !_unfoundScripts.empty();

	const std::vector<std::pair<const char*, std::vector<Button>>> saveGroups = {
		{ "Clean",
			{
				{ Sc::kRemoveUndefined, "Remove missing scripts", "scripts of mods you removed",
					"Delete scripts whose .pex file is no longer installed (a removed mod), with their instances and references, and end threads "
					"running them. Back up your save first.",
					HomeRisk::kCaution, r ? n(r->undefinedScripts + r->undefinedInstances) : -1 },
				{ Sc::kRemoveUnattached, "Remove unattached instances", "scripts attached to nothing",
					"Delete script instances that are attached to no object (their object is gone). They do nothing but take space. Usually safe.",
					HomeRisk::kSafe, r ? n(r->unattached) : -1 },
				{ Sc::kTerminateUndefined, "Stop threads of missing scripts", "running scripts that can never finish",
					"End running scripts whose .pex is no longer installed. They can never finish and keep the script engine busy.", HomeRisk::kSafe,
					r ? n(r->undefinedThreads) : -1 },
				{ Sc::kOrphans, "Missing spawned objects", "scripts on objects that are gone",
					"Find script instances sitting on spawned (FF......) objects that no longer exist, grouped by mod, and remove them.",
					HomeRisk::kCaution, r ? n(r->createdWithoutForm) : -1 },
				{ Sc::kLeftovers, "Leftover items", "items whose mod is gone",
					"Items in the player's, NPCs' and containers' inventories whose mod is no longer installed: find them and remove them. "
					"They can break inventory menus and cause crashes.",
					HomeRisk::kCaution, -1 },
				{ Sc::kMissingMods, "Mods no longer installed", "what removed mods left behind",
					"Mods that are in this save but not in your load order: see what they left and clean it after you confirm. A renamed plugin "
					"can be pointed at its new name instead.",
					HomeRisk::kCaution, anyMissing ? static_cast<long long>(_missing.size() + _missingScripts.size() + _unfoundScripts.size()) : 0 },
			} },
		{ "Fix",
			{
				{ Sc::kMcm, r && r->mcmJam ? "SkyUI MCM fix (stuck)" : "SkyUI MCM fix", "mod menus that never appear",
					"Clear a stuck SkyUI mod-menu (MCM) registration, the usual cause of mod menus that never appear.", HomeRisk::kCaution, -1 },
				{ Sc::kHavok, "Reset Havok", "objects that fly around or jitter",
					"Clear the saved physics state of objects, so they settle again when loaded.", HomeRisk::kSafe, -1 },
				{ Sc::kPurify, "Purify FormLists", "empty entries left by removed mods",
					"Remove entries in form lists that point at things that no longer exist (left by removed mods).", HomeRisk::kSafe, -1 },
				{ Sc::kPurge, "Purge a script", "remove one script completely",
					"Remove everything of one script by name: its running threads, queued calls and instances. Use for a script that floods the "
					"save. Back up your save first.",
					HomeRisk::kRisky, -1 },
				{ Sc::kBatch, "Batch Clean", "several cleanups in one go",
					"Run several cleanup steps in one go, with a preview of what each will remove.", HomeRisk::kCaution, -1 },
				{ Sc::kCrashLog, "Crash log", "find the object a crash names",
					"Read your newest crash log (Crash Logger, Trainwreck, .NET Script Framework) and find the objects it names in this save. "
					"A spawned or leveled NPC that crashes the game while the save loads can be removed from the save here.",
					HomeRisk::kCaution, -1 },
			} },
		{ "Your game",
			{
				{ Sc::kPlayer, "Edit player", "gold, level, skills, stats, items, bounty",
					"Edit your character: gold, level, skills, perk points, stats, inventory, spells and bounties.", HomeRisk::kCaution, -1 },
				{ Sc::kActors, "Find NPCs", "where they are; bring a lost follower to you",
					"Every NPC and creature with where the save has them. Find a lost follower and bring them next to you.", HomeRisk::kCaution, -1 },
				{ Sc::kTransplant, "Transplant progress", "from a broken save into a healthy one",
					"Carry your progress (quests, stats, inventory...) from a broken save into a healthy older one.", HomeRisk::kCaution, -1 },
				{ Sc::kSavesMgr, "Manage saves folder", "delete old saves in bulk",
					"All saves by character with size and date; delete old ones (with their .skse files) in bulk, keeping the newest.",
					HomeRisk::kCaution, -1 },
				{ Sc::kHistory, "Save history", "which mods keep growing",
					"Script instances, spawned objects and size of each of this character's saves, and the mods whose numbers keep climbing.",
					HomeRisk::kNone, -1 },
			} },
		{ "Look into it",
			{
				{ Sc::kMods, "Mods view", "every mod with its scripts and threads",
					"Every mod in the save with its scripts, instances and running threads, and what can be done about each.", HomeRisk::kNone, -1 },
				{ Sc::kQuests, "Quests", "running, completed, the journal",
					"Every quest in the save with its stages, journal entries and objectives.", HomeRisk::kNone, -1 },
				{ Sc::kCompare, "Compare to another save", "what changed between two saves",
					"Compare this save with another one (e.g. an older, healthy save) to see which scripts, threads and change forms grew.",
					HomeRisk::kNone, -1 },
				{ Sc::kReport, "Report", "text or PDF, to share when asking for help",
					"Write a report of the save (mods, scripts, anomalies) as text or PDF.", HomeRisk::kNone, -1 },
				{ Sc::kAnalysis, "Analysis", "what takes the most space and runs the most",
					"A text report of the save: which mods and scripts take the most space and run the most.", HomeRisk::kNone, -1 },
			} },
	};
	// the second tab: tools for the mods and the load order rather than the save
	const std::vector<std::pair<const char*, std::vector<Button>>> modGroups = {
		{ "Patch your load order",
			{
				{ Sc::kMerge, "Merged patch", "one plugin with every mod's changes",
					"Read every plugin of your load order and build one light plugin that keeps the changes of every mod that edits the "
					"same leveled lists, containers, NPCs, keywords, items, spells and cells. You choose what is merged.",
					HomeRisk::kCaution, -1 },
			} },
		{ "Check your mods",
			{
				{ Sc::kFileConflicts, "File conflicts", "which mod overrides which mod's files",
					"Which mods override which mods' files, loose and inside archives (BSAs), in the order the game uses them. Scripts "
					"overridden by another mod's copy are listed first.",
					HomeRisk::kNone, -1 },
				{ Sc::kPapyrusLog, "Papyrus log", "which mod is spamming errors",
					"Read Papyrus.0.log: errors grouped by script and mod, with how often each happens.", HomeRisk::kNone, -1 },
			} },
		{ "Look up",
			{
				{ Sc::kLookup, "Find a form", "by name or editor ID",
					"Search the installed plugins for a form by its in-game name or editor ID, and see whether this save changed it.", HomeRisk::kNone, -1 },
				{ Sc::kOpenPex, "Open script (.pex)", "read a compiled script's code",
					"Open a compiled Papyrus script (.pex) and read its decompiled code.", HomeRisk::kNone, -1 },
			} },
	};
	// the big features get a border in a shade of the button colour
	const std::set<Sc> major = { Sc::kTransplant, Sc::kPlayer, Sc::kActors, Sc::kMods, Sc::kLeftovers, Sc::kBatch, Sc::kMerge,
		Sc::kFileConflicts, Sc::kCrashLog };

	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("Shortcuts: the same as the Clean and Tools menus. Green is safe, yellow: read what it does first, red: risky. "
	                   "Every change is made in the loaded save and only written with File > Save or Save As.");
	ImGui::PopStyleColor();
	ImGui::Spacing();
	auto drawGroups = [&](const char* a_id, const std::vector<std::pair<const char*, std::vector<Button>>>& groups) {
	if (!ImGui::BeginTable(a_id, 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
		return;
	}
	for (const auto& [title, buttons] : groups) {
		ImGui::TableNextColumn();
		ImGui::SeparatorText(title);
		for (const auto& b : buttons) {
			ImGui::PushID(static_cast<int>(b.which));
			const bool nothing = b.count == 0;
			std::string label = b.label;
			if (b.count > 0) {
				label += Fmt("  (%lld)", b.count);
			}
			ImU32 col = 0;
			switch (b.risk) {
			case HomeRisk::kSafe: col = theme::Good(); break;
			case HomeRisk::kCaution: col = theme::Caution(); break;
			case HomeRisk::kRisky: col = theme::Bad(); break;
			default: break;
			}
			ImGui::BeginDisabled(nothing);
			// a coloured edge on the left says how risky it is
			const ImVec2 at = ImGui::GetCursorScreenPos();
			const float  h = ImGui::GetFrameHeight();
			if (col) {
				ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + 5, at.y + h), col, 2.0f);
			}
			ImGui::SetCursorScreenPos(ImVec2(at.x + 8, at.y));
			const bool click = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0));
			if (major.contains(b.which) && !nothing) {
				DrawMarquee(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
			}
			ImGui::EndDisabled();
			if (nothing) {
				tips::Tip("Nothing to do: this save has none.");
			} else {
				tips::Tip(b.tip);
			}
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
			ImGui::TextDisabled("%s", b.line);
			ImGui::Spacing();
			if (click) {
				shortcut(b.which);
			}
			ImGui::PopID();
		}
	}
	ImGui::EndTable();
	};
	if (ImGui::BeginTabBar("hometabs")) {
		if (ImGui::BeginTabItem("Your save")) {
			drawGroups("homesave", saveGroups);
			ImGui::EndTabItem();
		}
		static bool modsFirst = std::getenv("RESAVERPP_TEST_HOMETAB") != nullptr;  // for screenshots
		if (ImGui::BeginTabItem("Your mods: Merged patch, File conflicts", nullptr, modsFirst ? ImGuiTabItemFlags_SetSelected : 0)) {
			modsFirst = false;
			drawGroups("homemods", modGroups);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}
