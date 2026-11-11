// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Starfield saves (.sfs) in the window: the categories, the overview, the main screen, the menus, the lists and details
// of plugins, change forms and global data, and the Save check. Everything built on Skyrim's or Fallout 4's data (their
// change form contents, plugin records, mods, logs, the running game) is left out for these saves.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/Ops.h"
#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <map>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	void KV(const char* k, const std::string& v)
	{
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(k);
		ImGui::TableSetColumnIndex(1);
		ImGui::TextWrapped("%s", v.c_str());
	}

	bool BeginKV(const char* id)
	{
		if (ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 170.0f);
			ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
			return true;
		}
		return false;
	}

	// list keys of the plugins: full 0.., light 0x10000.., medium 0x20000..
	constexpr std::uint64_t kLiteKey = 0x10000, kMediumKey = 0x20000;

	const char* GlobalKindName(GlobalData::SfKind k)
	{
		switch (k) {
		case GlobalData::SfKind::kOffset: return "offset";
		case GlobalData::SfKind::kStrings: return "strings + data";
		case GlobalData::SfKind::kTypeOnly: return "type only";
		default: return "data";
		}
	}

	std::string HexDump(const std::vector<std::uint8_t>& a_v, std::size_t a_max)
	{
		std::string hex;
		char        b[8];
		for (std::size_t i = 0; i < a_v.size() && i < a_max; ++i) {
			std::snprintf(b, sizeof(b), "%02X%s", a_v[i], (i % 16 == 15) ? "\n" : " ");
			hex += b;
		}
		return hex;
	}
}

bool App::SfAllowed(Sc a_which, bool a_papyrus)
{
	switch (a_which) {
	case Sc::kRemoveUnattached:
	case Sc::kRemoveUndefined:
	case Sc::kTerminateUndefined:
	case Sc::kPurge:
	case Sc::kAnalysis:
		return a_papyrus;  // (they work on the script data)
	case Sc::kReport:
	case Sc::kBsa:
		return true;
	default:
		return false;
	}
}

bool App::sfPapyrus() const { return sf() && !_save->sfPapyrusRaw() && _save->papyrus(); }

void App::drawCategoriesSF()
{
	ImGui::BeginChild("##cats", ImVec2(250, 0), ImGuiChildFlags_Borders);
	auto item = [&](Cat c, std::size_t n, const char* a_name, bool warn = false) {
		std::string label = tr(a_name);
		if (c != Cat::kOverview) {
			label += Fmt(tr("  (%zu)"), n);
		}
		if (warn) {
			ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
		}
		if (ImGui::Selectable(label.c_str(), _cat == c) && _cat != c) {
			_cat = c;
			_filter[0] = 0;
			_onlyFlagged = false;
			_selected.clear();
			_focus.reset();
			_rowsDirty = true;
		}
		if (warn) {
			ImGui::PopStyleColor();
		}
	};
	item(Cat::kOverview, 0, N_("Overview"), _report && !_report->warnings.empty());
	ImGui::SeparatorText(trc("sidebar", "Save"));
	const auto& pl = _save->plugins;
	item(Cat::kPlugins, pl.full.size() + pl.lite.size() + pl.medium.size(), N_("Plugins"));
	item(Cat::kChangeForms, _save->changeForms.size(), N_("Change Forms"));
	item(Cat::kGlobals, _save->table1.size() + _save->table2.size() + _save->table3.size(), N_("Global Data"));
	ImGui::SeparatorText(tr("Papyrus"));
	if (!sfPapyrus()) {
		ImGui::PushTextWrapPos(0);
		ImGui::TextDisabled("%s", tr("Not read yet for Starfield: the script data is kept exactly as it is and written back unchanged."));
		ImGui::PopTextWrapPos();
	} else {
		auto& p = *_save->papyrus();
		item(Cat::kStrings, p.strings.size(), N_("Strings"));
		item(Cat::kScripts, p.scripts.size(), N_("Scripts"), _report && _report->undefinedScripts > 0);
		item(Cat::kInstances, p.instances.size(), N_("Script Instances"), _report && (_report->unattached > 0 || _report->undefinedInstances > 0));
		item(Cat::kReferences, p.references.size(), N_("References"));
		item(Cat::kArrays, p.arrays.size(), N_("Arrays"));
		item(Cat::kThreads, p.activeScripts.size(), N_("Active Scripts (threads)"));
		item(Cat::kMessages, p.functionMessages.size(), N_("Function Messages"));
		item(Cat::kSuspended, p.suspended1.size() + p.suspended2.size(), N_("Suspended Stacks"));
		item(Cat::kUnbinds, p.unbinds.size(), N_("Queued Unbinds"));
	}
	ImGui::EndChild();
}

void App::drawOverviewSF()
{
	const auto& h = _save->header;
	const auto& s = *_save;
	drawCheckSummary();
	ImGui::Spacing();
	ImGui::Text(tr("%s, level %u"), text::ToUtf8(h.playerName).c_str(), h.playerLevel);
	ImGui::TextUnformatted(text::ToUtf8(h.location).c_str());
	ImGui::TextDisabled("%s  |  %s", text::ToUtf8(h.race).c_str(), text::ToUtf8(h.gameDate).c_str());
	ImGui::Spacing();
	if (BeginKV("hdr")) {
		KV(tr("File"), platform::ToUtf8(_path.filename()));
		KV(tr("Save number"), std::to_string(h.saveNumber));
		KV(tr("Game"), "Starfield " + s.gameVersion + Fmt(tr("  (playthrough started on %s)"), s.sfCreatedVersion.c_str()));
		KV(tr("Format"), Fmt(tr("engine version %u, save version %u, form version %u, %zu zlib chunks"), h.version, static_cast<unsigned>(h.sfSaveVersion),
							 static_cast<unsigned>(s.formVersion), s.sfContainer.chunks));
		KV(tr("Experience"), Fmt("%.0f / %.0f", h.currentXP, h.neededXP));
		KV(tr("Plugins"), Fmt(tr("%zu full + %zu light + %zu medium"), s.plugins.full.size(), s.plugins.lite.size(), s.plugins.medium.size()));
		KV(tr("Body size"), Fmt(tr("%.1f MB"), s.originalBodySize / 1048576.0));
		KV(tr("Change forms"), std::to_string(s.changeForms.size()));
		KV(tr("Form IDs"), std::to_string(s.formIDs.size()));
		ImGui::EndTable();
	}
	ImGui::Spacing();
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("%s", sfPapyrus() ? tr("Starfield save. Shown: the header, the plugins, the change forms (type, plugin, sizes), the global data and the "
											"script data. The contents of change forms and global data are not decoded.")
										  : tr("Starfield save. Shown: the header, the plugins, the change forms (type, plugin, sizes) and the global data. "
											"The script data (Papyrus) is not read yet: it is written back exactly as it was."));
	ImGui::PopTextWrapPos();
	if (_report && sfPapyrus()) {
		const auto& r = *_report;
		ImGui::SeparatorText(tr("Counts"));
		if (BeginKV("counts")) {
			KV(tr("Script instances"), Fmt(tr("%zu  (%zu unattached, %zu undefined)"), r.instances, r.unattached, r.undefinedInstances));
			KV(tr("Scripts"), Fmt(tr("%zu  (%zu undefined)"), r.scripts, r.undefinedScripts));
			KV(tr("Threads"), Fmt(tr("%zu  (%zu undefined, %zu terminated)"), r.threads, r.undefinedThreads, r.terminatedThreads));
			KV(tr("Suspended stacks"), std::to_string(r.suspended));
			KV(tr("Function messages"), std::to_string(r.functionMessages));
			KV(tr("Arrays"), std::to_string(r.arrays));
			KV(tr("Strings"), std::to_string(r.strings));
			ImGui::EndTable();
		}
	}
}

void App::drawHomeSF()
{
	ImGui::SeparatorText(tr("Starfield save"));
	const float w = 260;
	if (ImGui::Button(tr("Save check..."), ImVec2(w, 0))) {
		_showCheck = true;
	}
	tips::Tip(tr("Everything ReSaver++ noticed about this save, and what can safely be done about it."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Save As..."), ImVec2(w, 0))) {
		startSaveAsDialog();
	}
	tips::Tip(tr("Write your edits to a new save file and leave the original untouched. The safest way to try a cleanup."));
	if (sfPapyrus()) {
		if (ImGui::Button(trc("home", "Remove unattached instances"), ImVec2(w, 0))) {
			shortcut(Sc::kRemoveUnattached);
		}
		tips::Tip(tr("Delete script instances that are attached to no object (their object is gone). They do nothing but take space. Usually safe."));
		ImGui::SameLine();
		if (ImGui::Button(trc("home", "Stop threads of missing scripts"), ImVec2(w, 0))) {
			shortcut(Sc::kTerminateUndefined);
		}
		tips::Tip(tr("End running scripts whose .pex is no longer installed. They can never finish and keep the script engine busy."));
	}
	ImGui::Spacing();
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("%s", tr("The Skyrim and Fallout 4 tools (mods, quests, players, followers, logs, the running game) do not work on Starfield saves "
								 "and are not shown."));
	ImGui::PopTextWrapPos();
}

void App::drawMenuSF()
{
	const bool pap = sfPapyrus();
	if (ImGui::BeginMenu(trId("Clean"), pap)) {
		if (ImGui::MenuItem(tr("Remove unattached instances"))) {
			shortcut(Sc::kRemoveUnattached);
		}
		if (ImGui::MenuItem(tr("Remove undefined elements"))) {
			shortcut(Sc::kRemoveUndefined);
		}
		if (ImGui::MenuItem(tr("Terminate undefined threads"))) {
			shortcut(Sc::kTerminateUndefined);
		}
		ImGui::Separator();
		if (ImGui::MenuItem(tr("Purge a script..."))) {
			shortcut(Sc::kPurge);
		}
		ImGui::EndMenu();
	}
	if (!pap && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
		ImGui::SetTooltip("%s", tr("Starfield: the script data is not read yet, so there is nothing to clean."));
	}
	if (ImGui::BeginMenu(trId("Tools"))) {
		if (ImGui::MenuItem(tr("Save check..."), "F2")) {
			_showCheck = true;
		}
		ImGui::MenuItem(tr("Analysis"), nullptr, &_showAnalysis, pap);
		if (ImGui::MenuItem(tr("Report..."))) {
			shortcut(Sc::kReport);
		}
		ImGui::EndMenu();
	}
}

void App::rebuildRowsSF(const std::function<void(std::uint64_t, std::string, std::uint32_t, bool, std::uint64_t, std::uint64_t)>& add)
{
	const auto& s = *_save;
	switch (_cat) {
	case Cat::kPlugins:
		for (std::size_t i = 0; i < s.plugins.full.size(); ++i) {
			add(i, Fmt("%02zX       %s", i, s.plugins.full[i].c_str()), 0, true, i, i);
		}
		for (std::size_t i = 0; i < s.plugins.medium.size(); ++i) {
			add(kMediumKey + i, Fmt("FD:%02zX    %s", i, s.plugins.medium[i].c_str()), theme::Dim(), true, kMediumKey + i, kMediumKey + i);
		}
		for (std::size_t i = 0; i < s.plugins.lite.size(); ++i) {
			add(kLiteKey + i, Fmt("FE:%03zX   %s", i, s.plugins.lite[i].c_str()), theme::Dim(), true, kLiteKey + i, kLiteKey + i);
		}
		break;
	case Cat::kChangeForms:
		for (const auto& c : s.changeForms) {
			const auto fid = s.formIDOf(c.refID);
			const auto plugin = s.pluginOf(fid);
			if (!_pluginFilter.empty() && (c.refID.type() == RefID::Type::kCreated || plugin != _pluginFilter)) {
				continue;
			}
			add(c.refID.data,
				Fmt("%s  %08X  %-28s  flags %08X  %zu bytes", s.cfTypeName(c), fid, plugin.c_str(), c.changeFlags, c.data.size()), 0, true, c.data.size(),
				c.data.size() + 3 * c.sfRefs.size() + 2 * c.sfStrings.size());
		}
		break;
	case Cat::kGlobals:
		{
			int t = 0;
			for (const auto* tb : { &s.table1, &s.table2, &s.table3 }) {
				++t;
				for (std::size_t i = 0; i < tb->size(); ++i) {
					const auto& g = (*tb)[i];
					std::string what = g.type == 1001 ? std::string(tr("Papyrus")) : g.type == 0 ? std::string(tr("string table offset"))
																		 : g.type == 2   ? std::string(tr("created-ID section offset"))
																						 : std::string();
					const std::size_t size = g.raw.size() + 2 * g.sfStrings.size();
					add((std::uint64_t(t) << 32) | i,
						Fmt(tr("table %d  type %4u  %-14s  %zu bytes"), t, g.type, GlobalKindName(g.sfKind), size) + (what.empty() ? "" : "  " + what), 0, true,
						size, size);
				}
			}
			break;
		}
	default:
		break;
	}
}

void App::detailPluginSF(std::uint64_t k)
{
	const auto& pl = _save->plugins;
	const bool  medium = k >= kMediumKey, lite = !medium && k >= kLiteKey;
	const auto  i = static_cast<std::size_t>(medium ? k - kMediumKey : lite ? k - kLiteKey : k);
	const auto& list = medium ? pl.medium : lite ? pl.lite : pl.full;
	const auto& extras = medium ? pl.sfMedium : lite ? pl.sfLite : pl.sfFull;
	if (i >= list.size()) {
		return;
	}
	ImGui::TextUnformatted(list[i].c_str());
	ImGui::TextDisabled("%s", (medium ? Fmt(tr("Medium plugin, index FD:%02X"), static_cast<unsigned>(i))
							   : lite ? Fmt(tr("Light plugin (ESL), index FE:%03X"), static_cast<unsigned>(i))
									  : Fmt(tr("Full plugin, load index %02X"), static_cast<unsigned>(i)))
								  .c_str());
	if (i < extras.size() && extras[i].isCreation) {
		const auto& e = extras[i];
		if (BeginKV("creation")) {
			KV(tr("Creation"), e.title);
			KV(tr("Creation ID"), e.id);
			KV(tr("Version"), e.version);
			KV(tr("Achievement friendly"), e.achievementFriendly ? tr("yes") : tr("no"));
			ImGui::EndTable();
		}
	}
	std::size_t forms = 0;
	for (const auto& c : _save->changeForms) {
		const auto fid = _save->formIDOf(c.refID);
		const auto top = fid >> 24;
		if (medium ? (top == 0xFD && ((fid >> 16) & 0xFF) == i) : lite ? (top == 0xFE && ((fid >> 12) & 0xFFF) == i) : top == i) {
			++forms;
		}
	}
	ImGui::Text(tr("%zu change forms belong to this plugin."), forms);
	if (ImGui::SmallButton(tr("Show its change forms"))) {
		_pluginFilter = list[i];
		select(Cat::kChangeForms, 0);
	}
}

void App::detailChangeFormSF(std::uint64_t k)
{
	RefID id;
	id.data = static_cast<std::uint32_t>(k);
	const auto* c = _save->findChangeForm(id);
	if (!c) {
		return;
	}
	const auto& s = *_save;
	const auto  fid = s.formIDOf(c->refID);
	ImGui::Text("%s  %08X", s.cfTypeName(*c), fid);
	if (BeginKV("cf")) {
		KV(tr("Plugin"), s.pluginOf(fid));
		KV(tr("RefID"), Fmt("%06X (%s)", c->refID.data,
							c->refID.type() == RefID::Type::kCreated   ? tr("created in game")
							: c->refID.type() == RefID::Type::kDefault ? "Starfield.esm"
																	   : tr("form index")));
		KV(tr("Change flags"), Fmt("%08X", c->changeFlags));
		KV(tr("Version"), std::to_string(c->version));
		KV(tr("Data"), Fmt(tr("%zu bytes"), c->data.size()));
		KV(tr("Form references"), std::to_string(c->sfRefs.size()));
		KV(tr("Strings"), std::to_string(c->sfStrings.size()));
		ImGui::EndTable();
	}
	if (sfPapyrus()) {
		std::size_t attached = 0;
		for (const auto& i : _save->papyrus()->instances) {
			attached += i.refID == c->refID;
		}
		ImGui::Text(tr("%zu script instance(s) attached."), attached);
	}
	if (!c->sfRefs.empty() && ImGui::CollapsingHeader(trId("Form references"))) {
		std::string t;
		for (std::size_t i = 0; i < c->sfRefs.size() && i < 400; ++i) {
			const auto f = s.formIDOf(c->sfRefs[i]);
			t += Fmt("%08X  %s\n", f, s.pluginOf(f).c_str());
		}
		ImGui::TextUnformatted(t.c_str());
	}
	if (!c->sfStrings.empty() && ImGui::CollapsingHeader(trId("Strings"))) {
		std::string t;
		for (auto i : c->sfStrings) {
			t += Fmt("%5u  ", i) + (i < s.sfStrings.size() ? text::ToUtf8(s.sfStrings[i]) : std::string("?")) + "\n";
		}
		ImGui::TextUnformatted(t.c_str());
	}
	const bool rawOpen = ImGui::CollapsingHeader(trId("Raw data (first 512 bytes, as stored)"));
	tips::Tip(tr("The record's bytes exactly as the save stores them (after decompression), in hex. For checking by hand."));
	if (rawOpen) {
		ImGui::TextUnformatted(HexDump(c->data, 512).c_str());
	}
}

void App::detailGlobalSF(std::uint64_t k)
{
	const int   t = static_cast<int>(k >> 32);
	const auto  i = static_cast<std::size_t>(k & 0xFFFFFFFFu);
	const auto& s = *_save;
	const auto* tb = t == 1 ? &s.table1 : t == 2 ? &s.table2 : &s.table3;
	if (i >= tb->size()) {
		return;
	}
	const auto& g = (*tb)[i];
	ImGui::Text(tr("Global data type %u (table %d)"), g.type, t);
	if (BeginKV("glob")) {
		KV(tr("Layout"), GlobalKindName(g.sfKind));
		if (g.sfKind == GlobalData::SfKind::kOffset) {
			KV(tr("Points at"), g.type == 0 ? tr("the string table (written again on save)") : tr("the created-ID section (written again on save)"));
		}
		KV(tr("Data"), Fmt(tr("%zu bytes"), g.raw.size()));
		if (g.sfKind == GlobalData::SfKind::kStrings) {
			KV(tr("Strings"), std::to_string(g.sfStrings.size()));
		}
		ImGui::EndTable();
	}
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("%s", g.type == 1001 ? (sfPapyrus() ? tr("The script data: see the Papyrus lists.")
															: tr("The script data (Papyrus): not read yet, written back exactly as it is."))
											 : tr("Not decoded: kept and written back exactly as it is."));
	ImGui::PopTextWrapPos();
	if (!g.sfStrings.empty() && ImGui::CollapsingHeader(trId("Strings"))) {
		std::string txt;
		for (auto x : g.sfStrings) {
			txt += Fmt("%5u  ", x) + (x < s.sfStrings.size() ? text::ToUtf8(s.sfStrings[x]) : std::string("?")) + "\n";
		}
		ImGui::TextUnformatted(txt.c_str());
	}
	if (!g.raw.empty() && ImGui::CollapsingHeader(trId("Raw data (first 512 bytes, as stored)"))) {
		ImGui::TextUnformatted(HexDump(g.raw, 512).c_str());
	}
}

std::vector<App::CheckItem> App::computeCheckItemsSF()
{
	std::vector<CheckItem> v;
	if (_readOnly) {
		CheckItem it;
		it.level = Risk::kRisky;
		it.title = tr("This save is damaged: opened read-only");
		std::string list;
		for (const auto& pr : _save->problems) {
			list += "\n  - " + pr;
		}
		it.what = tr("Its structure does not add up, or it uses a layout ReSaver++ does not fully understand:") + list;
		v.push_back(std::move(it));
	}
	if (!sfPapyrus()) {
		CheckItem it;
		it.level = Risk::kInfo;
		it.title = tr("Starfield: script data not read yet");
		it.what = tr("ReSaver Plus reads this save's header, plugins, change forms and global data, and writes it back. Its script data "
					 "(Papyrus) is kept exactly as it is, so there is nothing to check or clean in it yet.");
		v.push_back(std::move(it));
		return v;
	}
	return v;  // (with the script data read: App::computeCheckItems, its Papyrus part)
}
