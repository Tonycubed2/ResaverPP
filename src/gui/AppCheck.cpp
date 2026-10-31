// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Save check: everything ReSaver++ noticed about the open save, in one window that opens with the
// save. Each finding says what it means, and each thing that can be done about it says how likely
// it is to break the game, and why:
//   green  = safe: removes only what the game can no longer use, or changes nothing in the save
//   yellow = caution: safe in the normal case, read why before doing it
//   red    = risky: likely to break a mod or quest, or ReSaver++ cannot be sure; better not
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	void Wrapped(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	std::uint32_t RiskColor(App::Risk r)
	{
		switch (r) {
		case App::Risk::kSafe: return theme::Good();
		case App::Risk::kCaution: return theme::Caution();
		case App::Risk::kRisky: return theme::Bad();
		default: return theme::Dim();
		}
	}

	const char* RiskWord(App::Risk r)
	{
		switch (r) {
		case App::Risk::kSafe: return "SAFE";
		case App::Risk::kCaution: return "CAUTION";
		case App::Risk::kRisky: return "RISKY";
		default: return "INFO";
		}
	}

	// a filled dot in the risk colour, on the current line
	void Dot(App::Risk r)
	{
		const float sz = ImGui::GetTextLineHeight();
		const auto  p = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + sz * 0.5f, p.y + sz * 0.55f), sz * 0.36f, RiskColor(r));
		ImGui::Dummy(ImVec2(sz, sz));
		ImGui::SameLine();
	}

	// a button tinted with the risk colour
	bool RiskButton(const std::string& a_label, App::Risk a_risk, bool a_enabled)
	{
		ImVec4 c = ImGui::ColorConvertU32ToFloat4(RiskColor(a_risk));
		const bool light = theme::IsLight();
		auto       mix = [&](float k, float a) { return ImVec4(c.x * k, c.y * k, c.z * k, a); };
		ImGui::PushStyleColor(ImGuiCol_Button, mix(light ? 1.0f : 0.55f, light ? 0.22f : 0.55f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(light ? 1.0f : 0.7f, light ? 0.38f : 0.75f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, mix(light ? 1.0f : 0.85f, light ? 0.55f : 0.9f));
		ImGui::BeginDisabled(!a_enabled);
		const bool pressed = ImGui::Button(a_label.c_str(), ImVec2(330, 0));
		ImGui::EndDisabled();
		ImGui::PopStyleColor(3);
		return pressed && a_enabled;
	}
}

// ---- what is in the save -------------------------------------------------------------------------

std::vector<App::CheckItem> App::checkItems()
{
	std::vector<CheckItem> v;
	if (!_save) {
		return v;
	}
	auto* p = _save->papyrus();
	const auto info = _live.info();

	// damaged save: nothing can be written
	if (_readOnly) {
		CheckItem it;
		it.level = Risk::kRisky;
		it.title = "This save is damaged: opened read-only";
		std::string list;
		for (const auto& pr : _save->problems) {
			list += "\n  - " + pr;
		}
		it.what = "Its structure does not add up, or it uses a layout ReSaver++ does not fully understand:" + list +
				  "\nYou can look at everything, but ReSaver++ will not write it back (that could lose or invent data). "
				  "Load an earlier save in the game, or report this with the file.";
		v.push_back(std::move(it));
	}

	// the newest crash log came after this save and names one of its spawned or leveled actors
	if (const auto& cc = crashCheck(); !cc.suspects.empty()) {
		CheckItem it;
		it.level = Risk::kRisky;
		std::string ids;
		for (const auto& f : cc.suspects) {
			ids += (ids.empty() ? "" : ", ") + Fmt("%08X", f.target);
		}
		it.title = Fmt("Your newest crash log names %s from this save", cc.suspects.size() == 1 ? ("spawned NPC " + ids).c_str()
		                                                                                        : Fmt("%zu spawned NPCs", cc.suspects.size()).c_str());
		it.what = Fmt("The game crashed%s%s while working on %s, a spawned or leveled NPC this save has (%s). A crash while loading a "
					  "save is often one such NPC whose saved data no longer fits your mods. The log may name its base NPC first (an NPC "
					  "FF......); the game rebuilds that at every load, so it is not in the save: the NPC itself is.",
			cc.time.empty() ? "" : " on ", cc.time.c_str(), ids.c_str(), platform::ToUtf8(cc.file.filename()).c_str());
		for (const auto& f : cc.suspects) {
			CheckOption o;
			o.label = Fmt("Remove NPC %08X from the save", f.target);
			o.risk = Risk::kCaution;
			o.why = "Deletes only this NPC's saved data; the game drops it when the save loads, and a mod that spawns it can make a new one. "
					"Write the save with File > Save As and load that.";
			const auto raw = f.refRaw;
			const auto id = f.target;
			o.act = [this, raw, id] {
				confirm(Fmt("Remove NPC %08X", id),
					Fmt("Delete the saved data of spawned/leveled NPC %08X, which your newest crash log names?\n\nOnly the save in memory "
						"changes: write it with File > Save As (under a new name) and load that.",
						id),
					[this, raw, id] {
						ops::DeleteChangeForms(*_save, { raw });
						markEdited(Fmt("Removed NPC %08X (named in the crash log)", id));
					});
			};
			it.options.push_back(std::move(o));
		}
		CheckOption o;
		o.label = "Open the crash log...";
		o.risk = Risk::kSafe;
		o.why = "Only shows information: every form the log names and what this save has of each.";
		o.act = [this] {
			_showCrashLog = true;
			_clogLoaded = false;
			_showCheck = false;
		};
		it.options.push_back(std::move(o));
		v.push_back(std::move(it));
	}

	// mods no longer in the load order, scripts no longer installed
	if (!_missing.empty() || !_missingScripts.empty() || !_unfoundScripts.empty()) {
		std::size_t gone = 0, disabled = 0;
		for (const auto& m : _missing) {
			(m.installed ? disabled : gone) += 1;
		}
		CheckItem it;
		const bool suspect = _orderSuspect || _scanSuspect;
		it.level = suspect ? Risk::kRisky : Risk::kCaution;
		it.title = !_missing.empty() ? Fmt("%zu mod(s) in this save are not in your load order", _missing.size())
									 : Fmt("%zu script(s) in this save are no longer installed", _missingScripts.size() + _unfoundScripts.size());
		std::string w;
		if (!_missing.empty()) {
			w = _installKnown ? Fmt("Not in %s: %zu no longer installed, %zu installed but not enabled. ", _missingSource.c_str(), gone, disabled)
							  : Fmt("Not in %s. ", _missingSource.c_str());
		}
		if (!_missingScripts.empty()) {
			w += Fmt("%zu script(s) the game could not load because their files are gone. ", _missingScripts.size());
		}
		if (!_unfoundScripts.empty()) {
			w += Fmt("%zu script(s) were not found in your mods, but the game still had them when it saved (probably fine). ", _unfoundScripts.size());
		}
		w += "Their scripts and saved data are leftovers the game no longer uses.";
		if (!_installKnown && scanning()) {
			w += "\nStill scanning your mods: until it is done, uninstalled mods cannot be told from disabled ones.";
		}
		if (suspect) {
			w += "\nBUT the load order or the mods folder ReSaver++ read does not match this save (wrong MO2 profile, old plugins.txt, "
				 "or archives that could not be read). Cleaning now could remove mods you still use. Check Options > Settings first.";
		}
		it.what = w;
		CheckOption o;
		o.label = "Review and remove their leftovers...";
		o.risk = suspect ? Risk::kRisky : Risk::kCaution;
		o.why = suspect ? "The mod list looks wrong, so nothing is ticked. Removing a mod you still use deletes its progress and settings."
						: "Writes a NEW save without them; the original is not touched. Safe for mods you removed for good. A mod you only "
						  "disabled (for a test, or in another profile) would lose its progress and settings: those are not ticked.";
		o.act = [this] {
			_missingDismissed = false;
			for (auto& m : _missing) {
				m.planned = false;
			}
			_missingOpen = true;
		};
		it.options.push_back(std::move(o));
		v.push_back(std::move(it));
	}

	if (_report && p) {
		const auto& r = *_report;

		// scripts stuck in wait loops
		for (const auto& s : r.loopingScripts) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = "Script '" + s + "' is running in many copies at once";
			it.what = "Dozens of its threads sit in wait loops. A script that never stops (like the Firefly bug) slows the whole script "
					  "engine, and it gets worse with every save.";
			CheckOption o;
			o.label = "Purge " + s;
			o.risk = Risk::kCaution;
			o.why = "Removes all of its threads and its data, plus the spawned objects they sit on. Right for a runaway script. If it belongs "
					"to a mod you use, that mod's feature restarts or stops until the mod starts it again.";
			const std::string name = s;
			o.act = [this, name] {
				confirm("Purge " + name,
					"Remove every thread, suspended stack, function message and instance of '" + name +
						"', plus the spawned objects they sit on?\n\nOnly the save in memory changes: write it with File > Save As.",
					[this, name] {
						auto x = ops::PurgeScript(*_save, name, true);
						markEdited(Fmt("Purged %s: %zu threads, %zu instances, %zu spawned objects", name.c_str(), x.threads, x.instances, x.forms));
					});
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// SkyUI MCM registration jam
		if (r.mcmJam) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = "SkyUI was still registering MCM menus when this save was made";
			it.what = "Loading replays all of that work, which can make menus missing or the game sluggish after loading.";
			CheckOption o;
			o.label = "Fix SkyUI MCM registration jam...";
			o.risk = Risk::kSafe;
			o.why = "Removes only the stuck registration work. Menus already registered keep their settings; SkyUI registers the rest "
					"again on the next load.";
			o.act = [this] { fixMcmJam(); };
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// scripts that are no longer installed
		if (r.undefinedInstances || r.undefinedThreads) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = Fmt("%zu script instance(s) and %zu thread(s) belong to scripts the game could not load", r.undefinedInstances, r.undefinedThreads);
			it.what = "Their script files were not found when this save was made, so the game cannot run them: dead data.";
			CheckOption o;
			o.label = "Remove undefined elements";
			o.risk = Risk::kCaution;
			o.why = "The game cannot use these scripts any more, but stopping their threads also ends the scripts that called them, which "
					"can leave a quest or mod waiting forever. If that mod is installed again, or a script it extends is only missing for "
					"now, its data still matters. Back up the save and check it in game.";
			o.act = [this, p] {
				confirm("Remove undefined elements",
					"Delete scripts whose .pex is no longer installed, their instances and references, and terminate threads running them?"
					"\n\nOnly the save in memory changes: write it with File > Save As.",
					[this, p] { markEdited(Fmt("Removed/terminated %zu undefined elements", ops::RemoveUndefinedElements(*p))); });
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// instances on spawned objects that are gone
		if (r.createdWithoutForm) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = Fmt("%zu script instance(s) sit on spawned objects that no longer exist", r.createdWithoutForm);
			it.what = "Objects a mod created during play were deleted, but their scripts stayed in the save.";
			CheckOption o;
			o.label = "Review missing spawned objects...";
			o.risk = Risk::kCaution;
			o.why = "Shows them grouped by mod first. Ticked are those of mods that are not installed; the ones another script still points "
					"to are kept unless you choose otherwise.";
			o.act = [this] { openOrphans(); };
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// instances attached to nothing
		if (r.unattached >= 100) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = Fmt("%zu script instance(s) are attached to nothing", r.unattached);
			it.what = "Script data whose object is gone. A few are normal; many slow down loading.";
			CheckOption o;
			o.label = "Remove unattached instances";
			o.risk = Risk::kSafe;
			o.why = "Nothing in the game can reach these any more.";
			o.act = [this, p] {
				confirm("Remove unattached instances",
					Fmt("Delete %zu script instances that are attached to nothing?\n\nOnly the save in memory changes: write it with File > Save As.",
						_report ? _report->unattached : 0),
					[this, p] { markEdited(Fmt("Removed %zu unattached instances", ops::RemoveUnattachedInstances(*p))); });
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}

		// a busy script engine with no single cause
		std::vector<std::string> heavy;
		if (r.threads >= 300) {
			heavy.push_back(Fmt("%zu running script threads (a healthy save usually has under ~150)", r.threads));
		}
		if (r.suspended >= 50) {
			heavy.push_back(Fmt("%zu suspended stacks (normal is a handful)", r.suspended));
		}
		if (r.instances >= 150000) {
			heavy.push_back(Fmt("%zu script instances (very high: every load rebuilds them)", r.instances));
		}
		if (!heavy.empty()) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = "The script engine is very busy in this save";
			for (const auto& h : heavy) {
				it.what += "- " + h + "\n";
			}
			it.what += "No single cause was found. The Mods page shows which mod runs the most scripts.";
			CheckOption o;
			o.label = "Show the Mods page";
			o.risk = Risk::kSafe;
			o.why = "Only shows information.";
			o.act = [this] {
				_cat = Cat::kMods;
				_showCheck = false;
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}
	}

	// actors saved while waiting to play a start / stop / turn idle the save does not keep: the load crashes
	{
		const auto& stuck = stuckPathIdles();
		if (!stuck.empty()) {
			CheckItem it;
			it.level = Risk::kRisky;
			it.title = Fmt("%zu character(s) will crash the game when this save loads", stuck.size());
			it.what = "They were saved just as they were about to play a short start, stop or turn animation while walking or "
					  "fighting. The game keeps that waiting state in the save but not the animation, so right after the load it "
					  "tries to play the missing animation and crashes (Crash Logger: SkyrimSE.exe+0CEC680 on 1.6.1170, "
					  "MovementMessagePlayIdle, PathFollowerStatePlayIdle).\nCharacters:";
			for (std::size_t i = 0; i < stuck.size() && i < 8; ++i) {
				it.what += Fmt(" %08X", stuck[i].formID);
				if (_db) {
					if (const auto n = _db->describe(stuck[i].formID); !n.empty()) {
						it.what += " (" + n + ")";
					}
				}
			}
			if (stuck.size() > 8) {
				it.what += Fmt(" and %zu more", stuck.size() - 8);
			}
			CheckOption o;
			o.label = "Let them follow their path again";
			o.risk = Risk::kSafe;
			o.why = "Sets their path back to \"follow the path\", the way the game itself does when a character gets a new path "
					"(the PathIdleCrashFix plugin does the same in game). They skip that one short animation; nothing else changes.";
			o.act = [this] {
				std::vector<std::uint32_t> only;
				for (const auto& s : stuckPathIdles()) {
					only.push_back(s.refID);
				}
				const auto n = cf::FixStuckPathIdles(*_save, only);
				markEdited(Fmt("Fixed the path of %zu character(s) that would crash the load", n));
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}
	}

	// a save whose characters take long to read: tell the user instead of only being slow
	if (_stuckPathDone && _stuckPathMs > (std::getenv("RESAVERPP_TEST_HEAVY") ? 0.0 : 10000.0)) {  // test hook: any time counts
		CheckItem it;
		it.level = Risk::kCaution;
		it.title = Fmt("This save is very heavy: reading its %zu characters took %.0f seconds", _stuckPathActors, _stuckPathMs / 1000);
		it.what = Fmt("%zu of them were made while playing (spawned or leveled actors, form IDs starting with FF). ", _stuckPathSpawned);
		it.what += "A save this heavy loads slowly in the game too, and can crash or stutter. The usual causes are a mod that keeps "
		           "spawning NPCs or creatures that are never cleaned up, or a very long playthrough in many places.\n"
		           "Find NPCs lists every character with where it is: look for many copies of the same one. Save history shows "
		           "whether the count keeps growing from save to save. Ask the AI can look into it for you.";
		CheckOption o;
		o.label = "Find NPCs";
		o.risk = Risk::kSafe;
		o.why = "Only shows information.";
		o.act = [this] {
			_showActors = true;
			_showCheck = false;
		};
		it.options.push_back(std::move(o));
		CheckOption h;
		h.label = "Save history";
		h.risk = Risk::kSafe;
		h.why = "Only shows information.";
		h.act = [this] {
			shortcut(Sc::kHistory);
			_showCheck = false;
		};
		it.options.push_back(std::move(h));
		v.push_back(std::move(it));
	}

	// enchanted items that can never be recharged
	{
		std::size_t broken = 0;
		for (const auto& c : chargedItems()) {
			broken += cf::NeedsChargeFix(c.inv) ? 1 : 0;
		}
		if (broken) {
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = Fmt("%zu enchanted item(s) cannot be recharged", broken);
			it.what = "They have an enchantment but a max charge of 0 (or more charge than their max), so soul gems do nothing.";
			CheckOption o;
			o.label = "Give them a max charge of 3000";
			o.risk = Risk::kSafe;
			o.why = "Only adds the missing max charge to those items; nothing else changes. (Per mod, with your own value: Mods page.)";
			o.act = [this] {
				std::vector<std::pair<std::uint32_t, std::uint32_t>> only;
				for (const auto& c : chargedItems()) {
					if (cf::NeedsChargeFix(c.inv)) {
						only.push_back({ c.holderRefID, c.inv.item });
					}
				}
				const auto n = cf::FixCharges(*_save, 3000, only);
				markEdited(Fmt("Gave %zu item(s) a max charge of 3000", n));
			};
			it.options.push_back(std::move(o));
			v.push_back(std::move(it));
		}
	}

	// the journal shows objectives of quests that are over
	{
		std::size_t shownDone = 0;
		for (const auto& q : questStates()) {
			if (q.completed() || !q.running()) {
				shownDone += q.shownObjectives();
			}
		}
		if (shownDone) {
			CheckItem it;
			it.level = Risk::kInfo;
			it.title = Fmt("Your journal shows %zu objective(s) of quests that are completed or stopped", shownDone);
			it.what = "Leftovers of quests that ended. Harmless, but they fill the journal, and a very full journal stops showing new quests.";
			CheckOption o;
			o.label = "Refresh journal in game";
			o.risk = Risk::kCaution;
			o.why = std::string(info.connected && info.inGame ? "Done in the running game; this file is not changed. "
															  : "Needs the game running with ReSaverHelper.dll and a save loaded. ") +
					"Make a save in the game first and keep menus closed while it runs. With journal UI mods (such as QUI) a crash when "
					"opening the menu afterwards was reported with helper 3; helper 4 works more carefully. If the game crashes, load "
					"the save from before.";
			o.enabled = info.connected && info.inGame && !_liveRefresh.running;
			o.act = [this] { startLiveRefresh(); };
			it.options.push_back(std::move(o));
			CheckOption q;
			q.label = "Show the Quests page";
			q.risk = Risk::kSafe;
			q.why = "Only shows information.";
			q.act = [this] {
				_cat = Cat::kQuests;
				_showCheck = false;
			};
			it.options.push_back(std::move(q));
			v.push_back(std::move(it));
		}
	}

	std::stable_sort(v.begin(), v.end(), [](const CheckItem& a, const CheckItem& b) { return static_cast<int>(a.level) > static_cast<int>(b.level); });
	return v;
}

App::Risk App::checkWorst()
{
	Risk worst = Risk::kSafe;
	for (const auto& it : checkItems()) {
		if (static_cast<int>(it.level) > static_cast<int>(worst)) {
			worst = it.level;
		}
	}
	return worst;
}

// ---- the window ----------------------------------------------------------------------------------

void App::drawSaveCheck()
{
	if (!_save) {
		_showCheck = false;
		return;
	}
	prepareWindow("Save check", ImVec2(0.62f, 0.72f));
	if (!ImGui::Begin("Save check", &_showCheck, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Save check");
	ImGui::TextUnformatted(("Save check:  " + platform::ToUtf8(_path.filename())).c_str());
	// legend
	Dot(Risk::kSafe);
	ImGui::TextUnformatted("Safe");
	ImGui::SameLine(0, 24);
	Dot(Risk::kCaution);
	ImGui::TextUnformatted("Caution: read why first");
	ImGui::SameLine(0, 24);
	Dot(Risk::kRisky);
	ImGui::TextUnformatted("Risky: can break a mod or quest");
	ImGui::TextDisabled("Nothing here changes your save file until you use File > Save or Save As (the previous file is kept as .bak).\n"
						"Removing mods writes a NEW save next to the old one. Make a backup of saves you care about anyway.");
	if (scanning()) {
		ImGui::TextDisabled("Reading your mods in the background: this list updates when it is done.");
		drawBackgroundProgress(0);
	}
	ImGui::Separator();
	if (_save && !_stuckPathDone && !_save->changeForms.empty()) {
		ImGui::TextDisabled("Still checking the characters for the crash-on-load problem: %zu%%",
			_stuckPathNext * 100 / _save->changeForms.size());
	}

	const auto items = checkItems();
	ImGui::BeginChild("##items", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f));
	if (items.empty()) {
		ImGui::Spacing();
		Dot(Risk::kSafe);
		Wrapped(theme::Good(), "No problems found in this save.");
		ImGui::TextDisabled("Everything else (scripts, threads, quests, plugins) is on the pages on the left.");
	}
	int id = 0;
	for (const auto& it : items) {
		ImGui::PushID(id++);
		ImGui::Spacing();
		Dot(it.level);
		ImGui::PushStyleColor(ImGuiCol_Text, it.level == Risk::kInfo ? ImGui::GetColorU32(ImGuiCol_Text) : RiskColor(it.level));
		ImGui::TextWrapped("%s", it.title.c_str());
		ImGui::PopStyleColor();
		ImGui::Indent(ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.x);
		ImGui::TextWrapped("%s", it.what.c_str());
		for (const auto& o : it.options) {
			if (RiskButton(o.label, o.risk, o.enabled)) {
				o.act();
			}
			tips::Tip(o.risk == Risk::kRisky ? "Risky: this can break a mod or a quest. Read the note beside it, and keep a backup of the save." : o.risk == Risk::kCaution ? "Look first: usually fine, but read the note beside it before using it." : "Safe: does not harm the game. Nothing is written until you save (File > Save or Save As).");
			ImGui::SameLine();
			const float x = ImGui::GetCursorPosX();
			TextWrappedAt(RiskColor(o.risk), std::string(RiskWord(o.risk)) + ": " + o.why, x);
		}
		ImGui::Unindent(ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.x);
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::PopID();
	}
	ImGui::EndChild();
	if (ImGui::Checkbox("Show this when a save opens", &_checkOnOpen)) {
		saveSettings();
	}
	tips::Tip("Open this save check window by itself every time you open a save.");
	ImGui::SameLine();
	ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 140);
	if (ImGui::Button("Close", ImVec2(120, 0))) {
		_showCheck = false;
	}
	tips::Tip("Close the save check. Open it again with Tools > Save check (F2) or the button on the Overview page.");
	ImGui::End();
}

void App::TextWrappedAt(std::uint32_t a_col, const std::string& a_text, float a_x)
{
	// wrapped text that keeps its left edge at a_x on every line (beside a button)
	ImGui::PushStyleColor(ImGuiCol_Text, a_col);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::BeginGroup();
	ImGui::SetCursorPosX(a_x);
	ImGui::TextWrapped("%s", a_text.c_str());
	ImGui::EndGroup();
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

// one line for the Overview page and the menu bar: how the save looks, and the way back to the window
void App::drawCheckSummary()
{
	const auto  items = checkItems();
	std::size_t red = 0, yellow = 0;
	for (const auto& it : items) {
		red += it.level == Risk::kRisky ? 1 : 0;
		yellow += it.level == Risk::kCaution ? 1 : 0;
	}
	const Risk worst = red ? Risk::kRisky : yellow ? Risk::kCaution : Risk::kSafe;
	Dot(worst);
	if (items.empty()) {
		Wrapped(theme::Good(), "Save check: no problems found.");
	} else {
		ImGui::PushStyleColor(ImGuiCol_Text, RiskColor(worst));
		ImGui::TextUnformatted(Fmt("Save check: %zu thing(s) to look at%s", items.size(),
								   red ? Fmt(" (%zu risky)", red).c_str() : yellow ? Fmt(" (%zu to read first)", yellow).c_str() : "")
								   .c_str());
		ImGui::PopStyleColor();
	}
	ImGui::SameLine();
	if (ImGui::Button("Open save check")) {
		_showCheck = true;
		ImGui::SetWindowFocus("Save check");
	}
	tips::Tip("Open the list of everything ReSaver++ noticed about this save, with what can safely be done about each (F2).");
}
