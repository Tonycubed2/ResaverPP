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
	       _showPapyrusLog || _showSavesMgr || _showMerge || _showAssets || _showArchive || _showCrashLog || _showBloat || _showLoCheck || _showCosave || _showCharFx || _showShipsSF || _showQuestsSF;
}

void App::goHome()
{
	_showTransplant = _missingOpen = _orphansOpen = _showAnalysis = _purgeOpen = _showLookup = _showActors = _showPlayer = false;
	_showCompare = _batchOpen = _reportOpen = _showArrays = _removeOpen = false;
	_showViewer = false;
	_showCheck = false;
	_showLeftovers = _showHistory = _showPapyrusLog = _showSavesMgr = _showMerge = _showAssets = _showArchive = false;
	_showCrashLog = false;
	_showBloat = _showLoCheck = false;
	_showCosave = false;
	_showCharFx = false;
	_showShipsSF = false;
	_showQuestsSF = false;
	if (_cat != Cat::kOverview) {
		select(Cat::kOverview, 0);
	}
	_homePanel = true;
}

bool App::SkyrimOnly(Sc a_which)
{
	switch (a_which) {
	// (these read or change the save's change forms, decoded for Skyrim only so far, or Skyrim's plugin records)
	case Sc::kMcm:
	case Sc::kActors:
	case Sc::kOpenPex:
		return true;
	default:
		return false;
	}
}

void App::shortcut(Sc a_which)
{
	if (sf() && !SfAllowed(a_which, sfPapyrus())) {
		_status = tr(SfNotYet(a_which));
		return;
	}
	if (sf()) {
		// Starfield's own versions of the same entries
		auto selectForm = [this](std::uint32_t a_fid, std::uint8_t a_code) {
			for (const auto& c : _save->changeForms) {
				if (c.typeCode() == a_code && _save->formIDOf(c.refID) == a_fid) {
					select(Cat::kChangeForms, c.refID.data);
					return;
				}
			}
		};
		switch (a_which) {
		case Sc::kPlayer: selectForm(0x7, 9); return;        // the character: name, level and XP, face and body
		case Sc::kInventorySF: selectForm(0x14, 1); return;  // the player's reference: inventory, actor values
		case Sc::kQuests: _showQuestsSF = true; return;
		case Sc::kShipsSF: _showShipsSF = true; return;
		case Sc::kMissingMods: openMissingSF(); return;
		default: break;
		}
	}
	if (fo4() && SkyrimOnly(a_which)) {
		_status = std::string(tr("Not for Fallout 4 saves yet: ")) + tr(kSkyrimOnlyTip);
		return;
	}
	if (a_which == Sc::kSkyrimLogs) {
		openSkyrimLogs();
		return;
	}
	if (a_which == Sc::kAskAi) {
		_showAi = true;
		return;
	}
	if (a_which == Sc::kBsa) {
		_showBsa = true;
		if (!_bsaView) {
			const auto start = _sourcesPath.empty() ? gameFolder() / "Data" : platform::FromUtf8(_sourcesPath);
			if (auto p = platform::OpenFileDialog(start, "bsa;ba2", tr("BSA and BA2 archives"))) {
				openBsa(*p);
			}
		}
		return;
	}
	if (a_which == Sc::kCollectLogs) {
		startCollectLogs();
		return;
	}
	if (a_which == Sc::kLiveGame) {
		_showLiveGame = true;
		return;
	}
	if (a_which == Sc::kLoadOrderCheck) {
		_showLoCheck = true;  // needs no save
		_homePanel = false;
		return;
	}
	if (a_which == Sc::kGather) {
		// in the running game; the window shows the result (or why it cannot be done)
		_showLiveGame = true;
		if (_live.info().connected && _live.info().inGame && helperAtLeast(12)) {
			liveListFollowers();
		}
		return;
	}
	if (!_save && a_which != Sc::kOpenPex) {
		return;
	}
	_homePanel = false;  // the chosen tool gets the screen
	auto* p = _save ? _save->papyrus() : nullptr;
	switch (a_which) {
	case Sc::kRemoveUnattached:
		confirm(tr("Remove unattached instances"), Fmt(tr("Delete %zu script instances that are attached to nothing?"), _report ? _report->unattached : 0),
			[this] {
				const auto r = ops::RemoveUnattachedInstances(*_save);
				markEdited(Fmt(tr("Removed %zu unattached instances"), r.removed) + keptNote(r.kept) +
						   (r.keptNamed ? Fmt(tr(" (%zu kept: a script variable still names them)"), r.keptNamed) : std::string()));
			});
		break;
	case Sc::kRemoveUndefined:
		confirm(tr("Remove undefined elements"),
			tr("Delete scripts whose .pex is no longer installed, their instances and references, and terminate threads running them?"),
			[this] {
				const auto r = ops::RemoveUndefinedElements(*_save);
				markEdited(Fmt(tr("Removed/terminated %zu undefined elements"), r.removed) + keptNote(r.kept));
			});
		break;
	case Sc::kTerminateUndefined:
		confirm(tr("Terminate undefined threads"), tr("Terminate every thread that runs a script that is no longer installed?"),
			[this, p] { markEdited(Fmt(tr("Terminated %zu undefined threads"), ops::TerminateUndefinedThreads(*p))); });
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
	case Sc::kBloat: _showBloat = true; break;
	case Sc::kLoadOrderCheck: _showLoCheck = true; break;
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
	case Sc::kCosave: _showCosave = true; break;
	case Sc::kCharEffects: _showCharFx = true; break;
	case Sc::kMerge: _showMerge = true; break;
	case Sc::kFileConflicts: _showAssets = true; break;
	case Sc::kOpenPex:
		if (platform::HasNativeDialogs()) {
			if (auto f = platform::OpenFileDialog(_sourcesPath.empty() ? std::filesystem::path() : platform::FromUtf8(_sourcesPath), "pex",
					tr("Open compiled Papyrus script"))) {
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

	std::vector<std::pair<const char*, std::vector<Button>>> saveGroups = {
		{ tr("Clean"),
			{
				{ Sc::kRemoveUndefined, tr("Remove missing scripts"), tr("scripts of mods you removed"),
					tr("Delete scripts whose .pex file is no longer installed (a removed mod), with their instances and references, and end threads "
					"running them. Back up your save first."),
					HomeRisk::kCaution, r ? n(r->undefinedScripts + r->undefinedInstances) : -1 },
				{ Sc::kRemoveUnattached, trc("home", "Remove unattached instances"), tr("scripts attached to nothing"),
					tr("Delete script instances that are attached to no object (their object is gone). They do nothing but take space. Usually safe."),
					HomeRisk::kSafe, r ? n(r->unattached) : -1 },
				{ Sc::kTerminateUndefined, trc("home", "Stop threads of missing scripts"), tr("running scripts that can never finish"),
					tr("End running scripts whose .pex is no longer installed. They can never finish and keep the script engine busy."), HomeRisk::kSafe,
					r ? n(r->undefinedThreads) : -1 },
				{ Sc::kOrphans, tr("Missing spawned objects"), tr("scripts on objects that are gone"),
					tr("Find script instances sitting on spawned (FF......) objects that no longer exist, grouped by mod, and remove them."),
					HomeRisk::kCaution, r ? n(r->createdWithoutForm) : -1 },
				{ Sc::kLeftovers, tr("Leftover items"), tr("items whose mod is gone"),
					tr("Items in the player's, NPCs' and containers' inventories whose mod is no longer installed: find them and remove them. "
					"They can break inventory menus and cause crashes."),
					HomeRisk::kCaution, -1 },
				{ Sc::kMissingMods, tr("Mods no longer installed"), tr("what removed mods left behind"),
					tr("Mods that are in this save but not in your load order: see what they left and clean it after you confirm. A renamed plugin "
					"can be pointed at its new name instead."),
					HomeRisk::kCaution, anyMissing ? static_cast<long long>(_missing.size() + _missingScripts.size() + _unfoundScripts.size()) : 0 },
			} },
		{ tr("Fix"),
			{
				{ Sc::kMcm, r && r->mcmJam ? tr("SkyUI MCM fix (stuck)") : tr("SkyUI MCM fix"), tr("mod menus that never appear"),
					tr("Clear a stuck SkyUI mod-menu (MCM) registration, the usual cause of mod menus that never appear."), HomeRisk::kCaution, -1 },
				{ Sc::kHavok, tr("Reset Havok"), tr("objects that fly around or jitter"),
					tr("Clear the saved physics state of objects, so they settle again when loaded."), HomeRisk::kSafe, -1 },
				{ Sc::kPurify, tr("Purify FormLists"), tr("empty entries left by removed mods"),
					tr("Remove entries in form lists that point at things that no longer exist (left by removed mods)."), HomeRisk::kSafe, -1 },
				{ Sc::kPurge, tr("Purge a script"), tr("remove one script completely"),
					tr("Remove everything of one script by name: its running threads, queued calls and instances. Use for a script that floods the "
					"save. Back up your save first."),
					HomeRisk::kRisky, -1 },
				{ Sc::kBatch, tr("Batch Clean"), tr("several cleanups in one go"),
					tr("Run several cleanup steps in one go, with a preview of what each will remove."), HomeRisk::kCaution, -1 },
				{ Sc::kCrashLog, tr("Crash log"), tr("find the object a crash names"),
					tr("Read your newest crash log (Crash Logger, Trainwreck, .NET Script Framework) and find the objects it names in this save. "
					"A spawned or leveled NPC that crashes the game while the save loads can be removed from the save here."),
					HomeRisk::kCaution, -1 },
			} },
		{ tr("Your game"),
			{
				{ Sc::kPlayer, tr("Edit player"),
					sf() ? tr("name, level and XP, face and body") : fo4() ? tr("caps, level, S.P.E.C.I.A.L., perks") : tr("gold, level, skills, stats, items, bounty"),
					sf() ? tr("Your character's saved record: the name, level and XP, face and body, all editable.")
						 : tr("Edit your character: gold, level, skills, perk points, stats, inventory, spells and bounties."),
					HomeRisk::kCaution, -1 },
				{ Sc::kCharEffects, tr("Character effects"), tr("what you wear; effects stuck on you"),
					tr("A paper doll of what your character wears with the effects each item gives, every effect grouped by where it comes "
					"from, and the effects stuck on you (an enchantment of an item you no longer wear), with a button that removes them."),
					HomeRisk::kCaution, -1 },
				{ Sc::kActors, tr("Find NPCs"), tr("where they are; bring a lost follower to you"),
					tr("Every NPC and creature with where the save has them. Find a lost follower and bring them next to you."), HomeRisk::kCaution, -1 },
				{ Sc::kGather, tr("Gather followers (running game)"), tr("all followers next to you, now"),
					tr("In the running game (ReSaverHelper): every follower is brought next to you, wherever they are. No save editing."),
					HomeRisk::kSafe, -1 },
				{ Sc::kLiveGame, tr("Work on the running game"), tr("followers, controls, save: no save needed"),
					tr("Act in the game while it runs: gather your followers, get your controls back, save the game, and set up the "
					"hold-Ctrl rescue."),
					HomeRisk::kNone, -1 },
				{ Sc::kTransplant, tr("Transplant progress"), tr("from a broken save into a healthy one"),
					tr("Carry your progress (quests, stats, inventory...) from a broken save into a healthy older one."), HomeRisk::kCaution, -1 },
				{ Sc::kSavesMgr, tr("Manage saves folder"), tr("delete old saves in bulk"),
					tr("All saves by character with size and date; delete old ones (with their .skse files) in bulk, keeping the newest."),
					HomeRisk::kCaution, -1 },
				{ Sc::kCosave, sf() ? tr("SFSE co-save") : fo4() ? tr("F4SE co-save") : tr("SKSE co-save"), tr("plugins' data; clean what is no longer installed"),
					tr("What each script extender plugin keeps in the save's co-save (.skse / .f4se), which of them are no longer "
					"installed, PapyrusUtil's data by mod and key, and removing the data of plugins that are gone."),
					HomeRisk::kCaution, -1 },
				{ Sc::kHistory, tr("Save history"), tr("which mods keep growing"),
					tr("Script instances, spawned objects and size of each of this character's saves, and the mods whose numbers keep climbing."),
					HomeRisk::kNone, -1 },
			} },
		{ tr("Look into it"),
			{
				{ Sc::kMods, tr("Mods view"), tr("every mod with its scripts and threads"),
					tr("Every mod in the save with its scripts, instances and running threads, and what can be done about each."), HomeRisk::kNone, -1 },
				{ Sc::kQuests, tr("Quests"), tr("running, completed, the journal"),
					tr("Every quest in the save with its stages, journal entries and objectives."), HomeRisk::kNone, -1 },
				{ Sc::kCompare, tr("Compare to another save"), tr("what changed between two saves"),
					tr("Compare this save with another one (e.g. an older, healthy save) to see which scripts, threads and change forms grew."),
					HomeRisk::kNone, -1 },
				{ Sc::kReport, tr("Report"), tr("text or PDF, to share when asking for help"),
					tr("Write a report of the save (mods, scripts, anomalies) as text or PDF."), HomeRisk::kNone, -1 },
				{ Sc::kBloat, tr("Save bloat by mod"), tr("what each mod takes in this save"),
					tr("Bytes and counts each mod accounts for in this save: script instances, threads, arrays, changed forms, objects created "
					"from its items or NPCs, inventory entries. The biggest causes in plain words, with the tool that cleans each, and the "
					"growth since an older save."),
					HomeRisk::kNone, -1 },
				{ Sc::kAnalysis, tr("Analysis"), tr("what takes the most space and runs the most"),
					tr("A text report of the save: which mods and scripts take the most space and run the most."), HomeRisk::kNone, -1 },
			} },
	};
	if (sf()) {
		// Starfield: its inventory and ships next to Edit player
		for (auto& [title, buttons] : saveGroups) {
			for (std::size_t i = 0; i < buttons.size(); ++i) {
				if (buttons[i].which == Sc::kPlayer) {
					buttons.insert(buttons.begin() + static_cast<std::ptrdiff_t>(i) + 1,
						{ { Sc::kInventorySF, tr("Your inventory"), tr("items and credits, actor values"),
							  tr("Your character's items and credits as the save keeps them, with their counts (editable), and your saved actor values."),
							  HomeRisk::kCaution, -1 },
							{ Sc::kShipsSF, tr("Ships"), tr("your ships and their cargo"),
								tr("Every ship and station the save keeps, with its cargo: open one to change or add cargo."), HomeRisk::kCaution, -1 } });
					break;
				}
			}
		}
	}
	// the second tab: tools for the mods and the load order rather than the save
	const std::vector<std::pair<const char*, std::vector<Button>>> modGroups = {
		{ tr("Patch your load order"),
			{
				{ Sc::kMerge, tr("Merged patch"), tr("one plugin with every mod's changes"),
					tr("Read every plugin of your load order and build one light plugin that keeps the changes of every mod that edits the "
					"same leveled lists, containers, NPCs, keywords, items, spells and cells. You choose what is merged."),
					HomeRisk::kCaution, -1 },
			} },
		{ tr("Check your mods"),
			{
				{ Sc::kFileConflicts, tr("File conflicts"), tr("which mod overrides which mod's files"),
					tr("Which mods override which mods' files, loose and inside archives (BSAs), in the order the game uses them. Scripts "
					"overridden by another mod's copy are listed first."),
					HomeRisk::kNone, -1 },
				{ Sc::kLoadOrderCheck, tr("Load order check"), tr("plugin limits, light plugins, masters"),
					tr("Count your full and light plugins against the game's limits, find light plugins whose forms are numbered out of range, "
					"masters that are missing or load too late, and plugins made for a newer game version."),
					HomeRisk::kNone, -1 },
				{ Sc::kPapyrusLog, tr("Papyrus log"), tr("which mod is spamming errors"),
					tr("Read Papyrus.0.log: errors grouped by script and mod, with how often each happens."), HomeRisk::kNone, -1 },
				{ Sc::kBsa, tr("Unpack a BSA"), tr("see and unpack the files in an archive"),
					tr("See the files inside a mod's or the game's BSA archive, unpack all or some of them into a folder, and read a "
					"script packed inside without unpacking it."),
					HomeRisk::kNone, -1 },
			} },
		{ tr("Logs and AI"),
			{
				{ Sc::kSkyrimLogs, fo4Game() || fo4() ? tr("Analyze Fallout 4 logs") : tr("Analyze Skyrim logs"),
					fo4Game() || fo4() ? tr("F4SE, Buffout 4, Papyrus and mod logs") : tr("SKSE, crash, Papyrus and mod logs"),
					fo4Game() || fo4() ? tr("Read your F4SE logs, Buffout 4 crash logs, the Papyrus log and the mods' own logs, and list the problems "
					                     "found: plugins that did not load, what crashed, scripts that are missing.")
					                   : tr("Read your SKSE logs, crash logs, the Papyrus log and the mods' own logs, and list the problems found: plugins that "
					                     "did not load, what crashed, scripts that are missing."),
					HomeRisk::kNone, -1 },
				{ Sc::kCollectLogs, tr("Collect all logs"), tr("one zip on the desktop, to share"),
					tr("Put every Skyrim log (SKSE and all its folders, crash logs, Papyrus), your load order and INI files, and what PC "
					"it is (Windows, memory, processor, video card) into one zip file on your desktop, ready to send with a report."),
					HomeRisk::kNone, -1 },
				{ Sc::kAskAi, tr("Ask the AI"), tr("plain English: it uses ReSaver Plus for you"),
					tr("Chat with an AI engine (Claude, ChatGPT, Gemini, or a free one on your PC) that can use everything ReSaver Plus "
					"does: check the save, read the logs, fix quests, clean scripts, act in the running game."),
					HomeRisk::kCaution, -1 },
			} },
		{ tr("Look up"),
			{
				{ Sc::kLookup, tr("Find a form"), tr("by name or editor ID"),
					tr("Search the installed plugins for a form by its in-game name or editor ID, and see whether this save changed it."), HomeRisk::kNone, -1 },
				{ Sc::kOpenPex, tr("Open script (.pex)"), tr("read a compiled script's code"),
					tr("Open a compiled Papyrus script (.pex) and read its decompiled code."), HomeRisk::kNone, -1 },
			} },
	};
	// the big features get a border in a shade of the button colour
	const std::set<Sc> major = { Sc::kTransplant, Sc::kPlayer, Sc::kActors, Sc::kMods, Sc::kLeftovers, Sc::kBatch, Sc::kMerge,
		Sc::kFileConflicts, Sc::kCrashLog, Sc::kSkyrimLogs, Sc::kAskAi, Sc::kLiveGame };

	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", tr("Shortcuts: the same as the Clean and Tools menus. Green is safe, yellow: read what it does first, red: risky. "
	                   "Every change is made in the loaded save and only written with File > Save or Save As."));
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
			const bool notHere = (fo4() && SkyrimOnly(b.which)) || !sfOk(b.which);  // a Fallout 4 / Starfield save: tools not for it are off
			const bool nothing = b.count == 0 || notHere;
			std::string label = b.label;
			if (b.count > 0) {
				label += Fmt(tr("  (%lld)"), b.count);
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
			if (notHere) {
				tips::Tip(sf() ? tr(SfNotYet(b.which)) : tr(kSkyrimOnlyTip));
			} else if (nothing) {
				tips::Tip(tr("Nothing to do: this save has none."));
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
		if (ImGui::BeginTabItem(trId("Your save"))) {
			drawGroups("homesave", saveGroups);
			ImGui::EndTabItem();
		}
		static bool modsFirst = std::getenv("RESAVERPP_TEST_HOMETAB") != nullptr;  // for screenshots
		if (ImGui::BeginTabItem(trId("Your mods: Merged patch, File conflicts"), nullptr, modsFirst ? ImGuiTabItemFlags_SetSelected : 0)) {
			modsFirst = false;
			drawGroups("homemods", modGroups);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	if (sf()) {
		ImGui::TextDisabled("%s", (tr("Names: ") + namesStatusSF()).c_str());
	}
}
