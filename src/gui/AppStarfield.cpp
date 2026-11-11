// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Starfield saves (.sfs) in the window: the categories, the overview, the main screen, the menus, the lists and details
// of plugins, change forms and global data, and the Save check. Everything built on Skyrim's or Fallout 4's data (their
// change form contents, plugin records, mods, logs, the running game) is left out for these saves.
#include "App.h"

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <functional>
#include <utility>

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/LoadOrder.h"
#include "core/Ops.h"
#include "core/PluginDB.h"
#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <map>
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

	void TextColoredSF(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
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
	// the script data (Papyrus): the same code as Skyrim's
	case Sc::kRemoveUnattached:
	case Sc::kRemoveUndefined:
	case Sc::kTerminateUndefined:
	case Sc::kPurge:
	case Sc::kBatch:
	case Sc::kAnalysis:
	case Sc::kArrays:
	case Sc::kMods:
		return a_papyrus;
	// the same tools as Skyrim's, reading Starfield's files
	case Sc::kMissingMods:  // (openMissingSF)
	case Sc::kReport:
	case Sc::kBsa:
	case Sc::kCosave:
	case Sc::kLiveGame:
	case Sc::kAskAi:
	case Sc::kLookup:
	case Sc::kHistory:
	case Sc::kPapyrusLog:
	case Sc::kSavesMgr:
	case Sc::kLeftovers:  // (cf::ListInventories / RemoveInventoryItems read Starfield's inventories)
	// Starfield's own versions
	case Sc::kPlayer:  // the character (NPC_ 7): name, level and XP, face
	case Sc::kQuests:  // the quests window
	case Sc::kInventorySF:
	case Sc::kShipsSF:
		return true;
	default:
		return false;
	}
}

const char* App::SfNotYet(Sc a_which)
{
	switch (a_which) {
	case Sc::kOrphans:
		return N_("Not for Starfield saves yet: Starfield keeps many objects made in play (ships, outposts) in another part of the save, so a "
				  "script on one of them can look orphaned when it is not.");
	case Sc::kActors:
		return N_("Not for Starfield saves yet: it reads Skyrim's actor data.");
	case Sc::kHavok:
		return N_("Not for Starfield saves yet: Starfield's physics data is laid out differently.");
	case Sc::kPurify:
		return N_("Not for Starfield saves yet: Starfield's form lists are laid out differently.");
	case Sc::kLoadOrderCheck:
		return N_("Not for Starfield yet: it checks Skyrim's and Fallout 4's plugin limits.");
	case Sc::kBloat:
	case Sc::kCompare:
		return N_("Not for Starfield saves yet: it reads Skyrim's and Fallout 4's change forms.");
	case Sc::kCrashLog:
		return N_("Not for Starfield yet: it reads Skyrim's and Fallout 4's crash logs.");
	case Sc::kSkyrimLogs:
	case Sc::kCollectLogs:
		return N_("Not for Starfield yet: it reads Skyrim's and Fallout 4's logs. The Papyrus log tool reads Starfield's.");
	case Sc::kMerge:
	case Sc::kFileConflicts:
		return N_("Not for Starfield yet: it reads Skyrim's and Fallout 4's plugins and archives.");
	default:
		return N_("Not available for Starfield saves: it is for Skyrim (or Fallout 4) only.");
	}
}

std::string App::sfBaseName(std::uint32_t a_formID, const ChangeForm& a_cf) const
{
	// a reference made in game: "base <its base object's name>" when the save records the base (BaseObjectsSF)
	if (!_db || !IsReferenceChangeFormSF(a_cf.typeCode())) {
		return {};
	}
	for (const auto& b : BaseObjectsSF(a_formID, a_cf)) {
		const auto bf = _save->formIDOf(b);
		if ((bf >> 24) == 0xFF) {
			continue;
		}
		if (auto d = _db->describe(bf); !d.empty()) {
			return std::string(tr("base")) + " " + d;
		}
	}
	return {};
}

void App::liveQuestSF(const std::string& a_command, std::uint32_t a_formID, int a_stage)
{
	std::vector<std::string> cmd{ a_command, Fmt("%08X", a_formID) };
	if (a_command == "SETSTAGE") {
		cmd.push_back(std::to_string(a_stage));
	}
	if (a_command != "QUEST") {
		cmd.push_back(liveNotifyMode());
		cmd.push_back("");
	}
	_liveQuestSFResult = tr("Asking the game...");
	_live.request(cmd, [this, a_command](const LiveLink::Reply& r) {
		if (!LiveLink::Ok(r)) {
			_liveQuestSFResult = tr("The game said: ") + LiveLink::Error(r);
			return;
		}
		if (a_command == "QUEST") {
			// OK <editor id> <flags> <current stage> <running> <completed> <index:done,...>
			std::string done;
			const auto  list = LiveLink::Field(r, 5);
			for (std::size_t a = 0; a < list.size();) {
				auto b = list.find(',', a);
				if (b == std::string::npos) {
					b = list.size();
				}
				const auto one = list.substr(a, b - a);
				if (one.size() > 2 && one.substr(one.size() - 2) == ":1") {
					done += (done.empty() ? "" : ", ") + one.substr(0, one.size() - 2);
				}
				a = b + 1;
			}
			_liveQuestSFResult = Fmt(tr("In the game: %s, current stage %s, stages done: %s"),
				LiveLink::Field(r, 4) == "1" ? tr("completed") : LiveLink::Field(r, 3) == "1" ? tr("running") : tr("not running"),
				LiveLink::Field(r, 2).c_str(), done.empty() ? tr("none") : done.c_str());
			return;
		}
		// OK <1 done | ? not seen yet> <running> <completed> <current stage>
		_liveQuestSFResult = Fmt(tr("%s - now %s, current stage %s"),
			LiveLink::Field(r, 0) == "1" ? tr("Done in the game") : tr("Sent to the game (not seen done yet: check again in a moment)"),
			LiveLink::Field(r, 2) == "1" ? tr("completed") : LiveLink::Field(r, 1) == "1" ? tr("running") : tr("not running"), LiveLink::Field(r, 3).c_str());
	});
}

bool App::sfPapyrus() const { return sf() && !_save->sfPapyrusRaw() && _save->papyrus(); }

std::string App::namesStatusSF() const
{
	if (namesLoading()) {
		const auto done = _dbProgress ? _dbProgress->done.load() : 0, total = _dbProgress ? _dbProgress->total.load() : 0;
		return Fmt(tr("reading the plugins (%zu of %zu MB)..."), done, total);
	}
	if (_db && _db->starfield()) {
		return Fmt(tr("%zu forms named from %zu of %zu plugins (%.1f s)"), _db->size(), _db->pluginsRead, _db->plugins().size(),
				   (_db->sfMsStrings + _db->sfMsPlugins) / 1000.0) +
			   (_db->missing.empty() ? std::string() : Fmt(tr(", %zu not found"), _db->missing.size()));
	}
	if (gameFolder().empty()) {
		return tr("not read: the Starfield folder is not known (Options > Settings)");
	}
	return tr("not read yet (Tools > Read plugin names)");
}

void App::openMissingSF()
{
	_missingDismissed = false;
	for (auto& m : _missing) {
		m.planned = false;
	}
	_missingOpen = true;
}

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
	if (sfPapyrus()) {
		item(Cat::kMods, pl.full.size() + pl.lite.size() + pl.medium.size(), N_("Mods"), !_missing.empty());
	}
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
		KV(tr("Names"), namesStatusSF());
		if (!_missing.empty()) {
			std::size_t gone = 0;
			for (const auto& m : _missing) {
				gone += !m.installed;
			}
			KV(tr("Not loaded"), Fmt(tr("%zu plugin(s) of this save are not in %s (%zu not installed)"), _missing.size(), _missingSource.c_str(), gone));
		}
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
			const std::string name = _db ? (c.refID.type() != RefID::Type::kCreated ? _db->describe(fid) : sfBaseName(fid, c)) : std::string();
			// (the name first: the list is narrow, and the name is what a row is looked for by)
			add(c.refID.data,
				Fmt("%s  %08X  %-22s  ", s.cfTypeName(c), fid, plugin.c_str()) + (name.empty() ? "" : name + "  ") +
					Fmt("(%zu bytes, flags %08X)", c.data.size(), c.changeFlags),
				0, true, c.data.size(), c.data.size() + 3 * c.sfRefs.size() + 2 * c.sfStrings.size());
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
	for (const auto& m : _missing) {
		if (m.plugin == list[i]) {
			TextColoredSF(theme::Warn(), m.installed ? tr("Installed but not enabled in plugins.txt: the game does not load it.")
													 : tr("Not installed: the game does not load it. What it left is listed in Mods no longer installed."));
		}
	}
	if (_db) {
		std::size_t named = 0;
		for (const auto& c : _save->changeForms) {
			const auto fid = _save->formIDOf(c.refID);
			const auto top = fid >> 24;
			if ((medium ? (top == 0xFD && ((fid >> 16) & 0xFF) == i) : lite ? (top == 0xFE && ((fid >> 12) & 0xFFF) == i) : top == i) && _db->find(fid)) {
				++named;
			}
		}
		ImGui::TextDisabled(tr("%zu of them found in the plugin file (names)."), named);
	}
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
		if (c->refID.type() == RefID::Type::kCreated && IsReferenceChangeFormSF(c->typeCode())) {
			// made in game: the save names its base object only for some (BaseObjectsSF)
			const auto bases = BaseObjectsSF(fid, *c);
			if (bases.empty()) {
				KV(tr("Base object"), tr("not in the save (the game makes this object again from its cell, e.g. a generated place)"));
			}
			for (const auto& b : bases) {
				const auto bf = s.formIDOf(b);
				const auto d = _db && (bf >> 24) != 0xFF ? _db->describe(bf) : std::string();
				KV(tr("Base object"), Fmt("%08X  ", bf) + (!d.empty() ? d : s.pluginOf(bf)));
			}
		}
		if (c->refID.type() != RefID::Type::kCreated) {
			const auto d = _db ? _db->describe(fid) : std::string();
			KV(tr("Name"), !d.empty() ? d : _db ? tr("(not in the plugin files)") : namesStatusSF());
			if (const auto* f = _db ? _db->find(fid) : nullptr; f && f->base) {
				KV(tr("Base object"), Fmt("%08X  ", f->base) + _db->describe(f->base));
			}
		}
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
	// the player's actor values (FindActorValuesSF)
	if (c->typeCode() == 1 && fid == 0x14) {
		drawActorValuesSF(c->refID);
	}
	// a reference with an inventory (the player, followers, NPCs, containers, ships): its items (DecodeReferenceSF, read
	// in Starfield.exe 1.16.244); a stack's count can be changed in place
	if (c->typeCode() <= 1 && (c->changeFlags & 0x8000020u)) {
		const auto r = DecodeReferenceSF(fid, *c);
		if (ImGui::CollapsingHeader(Fmt(tr("Inventory (%zu items)###inv"), r.items.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (!r.ok) {
				ImGui::PushTextWrapPos(0);
				ImGui::TextDisabled("%s", (tr("Not read: ") + r.error).c_str());
				ImGui::PopTextWrapPos();
			} else if (r.items.empty()) {
				ImGui::TextDisabled("%s", tr("Empty."));
				drawAddItemSF(c->refID);
			} else {
				ImGui::PushTextWrapPos(0);
				ImGui::TextDisabled("%s", tr("Every item the save keeps for it, in stacks (a stack is a group with the same mods, damage, "
											 "name...). Change a count and save: the game loads that many."));
				ImGui::PopTextWrapPos();
				drawAddItemSF(c->refID);
				ImGui::BeginDisabled(_readOnly);
				if (ImGui::BeginTable("##inv", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
					ImGui::TableSetupColumn(tr("Item"), ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn(tr("Form ID"), ImGuiTableColumnFlags_WidthFixed, 80);
					ImGui::TableSetupColumn(tr("Stack"), ImGuiTableColumnFlags_WidthFixed, 50);
					ImGui::TableSetupColumn(tr("Count"), ImGuiTableColumnFlags_WidthFixed, 130);
					ImGui::TableHeadersRow();
					for (std::size_t i = 0; i < r.items.size(); ++i) {
						const auto& it = r.items[i];
						const auto  bf = it.refIndex < c->sfRefs.size() ? s.formIDOf(c->sfRefs[it.refIndex]) : 0;
						std::string name = _db && (bf >> 24) != 0xFF ? _db->describe(bf) : std::string();
						if (name.empty()) {
							name = bf == 0xF ? std::string(tr("Credits")) : s.pluginOf(bf);
						}
						for (std::size_t k = 0; k < it.stacks.size(); ++k) {
							const auto& st = it.stacks[k];
							const bool  own = st.countOffset > st.extrasBegin + 1;  // the stack has extra data
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							if (k == 0) {
								ImGui::TextUnformatted(name.c_str());
							}
							ImGui::TableNextColumn();
							if (k == 0) {
								ImGui::Text("%08X", bf);
							}
							ImGui::TableNextColumn();
							ImGui::Text("%zu%s", k + 1, own ? "*" : "");
							if (own) {
								tips::Tip(tr("This stack carries data of its own (mods, condition, a name, equipped...)."));
							}
							ImGui::TableNextColumn();
							ImGui::PushID(static_cast<int>(i * 64 + k));
							ImGui::SetNextItemWidth(-1);
							std::uint32_t v = st.count;
							if (ImGui::InputScalar("##n", ImGuiDataType_U32, &v, nullptr, nullptr, "%u", ImGuiInputTextFlags_EnterReturnsTrue) &&
								v != st.count) {
								v = std::clamp<std::uint32_t>(v, 1, 1000000000u);
								if (auto* cf = _save->findChangeForm(c->refID); cf && SetInventoryCountSF(*cf, st, v)) {
									markChanged(Fmt(tr("%s: count %u -> %u"), name.c_str(), st.count, v));
								}
							}
							ImGui::PopID();
						}
					}
					ImGui::EndTable();
				}
				ImGui::EndDisabled();
				ImGui::TextDisabled("%s", tr("Type a number and press Enter (1 to 1,000,000,000)."));
			}
		}
	}
	// a character (NPC_; the player is 0x7): its name, changeable (RenameNpcSF)
	if (c->typeCode() == 9) {
		const auto n = DecodeNpcSF(*c);
		if (ImGui::CollapsingHeader(trId("Character"), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (!n.ok) {
				ImGui::PushTextWrapPos(0);
				ImGui::TextDisabled("%s", (tr("Not read: ") + n.error).c_str());
				ImGui::PopTextWrapPos();
			} else if (!n.hasName || c->sfStrings.empty() || c->sfStrings[0] >= s.sfStrings.size()) {
				ImGui::TextDisabled("%s", tr("The save keeps no name for this character (its plugin's name applies)."));
			} else {
				const auto cur = text::ToUtf8(s.sfStrings[c->sfStrings[0]]);
				ImGui::Text(tr("Name: %s"), cur.c_str());
				static char buf[256];
				static RefID bufFor;
				if (!(bufFor == c->refID)) {
					bufFor = c->refID;
					std::snprintf(buf, sizeof(buf), "%s", cur.c_str());
				}
				ImGui::BeginDisabled(_readOnly);
				ImGui::SetNextItemWidth(260);
				ImGui::InputText("##name", buf, sizeof(buf));
				ImGui::SameLine();
				if (ImGui::Button(tr("Rename")) && buf[0] && cur != buf) {
					const auto raw = c->refID;
					const std::string want = buf;
					if (auto* cf = _save->findChangeForm(raw); cf && RenameNpcSF(*_save, *cf, text::FromUtf8(want, s.sfStrings[c->sfStrings[0]]))) {
						markChanged(Fmt(tr("Renamed to %s"), want.c_str()));
					}
				}
				ImGui::EndDisabled();
				tips::Tip(tr("The name the game shows for this character. For your own character, the name in the load menu changes too."));
			}
			if (n.ok && s.formIDOf(c->refID) == 0x7) {
				drawPlayerLevelSF();
			}
			if (n.ok && !n.face.empty() && ImGui::TreeNodeEx(Fmt(tr("Face and body (%zu numbers)###facesf"), n.face.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::PushTextWrapPos(0);
				ImGui::TextDisabled("%s", tr("The face the save keeps for this character, as the character creator wrote it: numbered sliders, "
											 "blends of face shapes by region, and named shapes (body and face). Type a number and press Enter; "
											 "the game reads them at the next load. Keep the original save."));
				ImGui::PopTextWrapPos();
				ImGui::BeginDisabled(_readOnly);
				if (ImGui::BeginTable("##face", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
						ImVec2(0, 320))) {
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableSetupColumn(tr("Kind"), ImGuiTableColumnFlags_WidthFixed, 110);
					ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn(tr("Value"), ImGuiTableColumnFlags_WidthFixed, 130);
					ImGui::TableHeadersRow();
					for (std::size_t i = 0; i < n.face.size(); ++i) {
						const auto& v = n.face[i];
						const std::string nm = v.name < c->sfStrings.size() && c->sfStrings[v.name] < s.sfStrings.size()
						                           ? text::ToUtf8(s.sfStrings[c->sfStrings[v.name]])
						                           : std::string();
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextUnformatted(v.group == 1 ? tr("Slider") : v.group == 2 ? tr("Blend") : tr("Shape"));
						ImGui::TableNextColumn();
						if (v.group == 1) {
							ImGui::Text(tr("slider %u"), v.key);
						} else if (v.group == 2) {
							ImGui::Text(tr("%s (blend %u)"), nm.c_str(), v.key);
						} else {
							ImGui::TextUnformatted(nm.c_str());
						}
						ImGui::TableNextColumn();
						float x = 0;
						std::memcpy(&x, c->data.data() + v.offset, 4);
						ImGui::PushID(static_cast<int>(i));
						ImGui::SetNextItemWidth(-1);
						float want = x;
						if (ImGui::InputFloat("##v", &want, 0, 0, "%.4g", ImGuiInputTextFlags_EnterReturnsTrue) && want != x) {
							if (auto* cf = _save->findChangeForm(c->refID); cf && SetFaceValueSF(*cf, v.offset, want)) {
								markChanged(Fmt(tr("Face: %s %g -> %g"), v.group == 1 ? Fmt(tr("slider %u"), v.key).c_str() : nm.c_str(), x, want));
							}
						}
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
				ImGui::EndDisabled();
				ImGui::TreePop();
			}
		}
	}
	// a quest: its state and stages (DecodeQuestSF, read in Starfield.exe 1.16.244), editable in place
	if (c->typeCode() == 8) {
		const auto q = DecodeQuestSF(*c);
		if (q.ok && ImGui::CollapsingHeader(trId("Quest"), ImGuiTreeNodeFlags_DefaultOpen)) {
			if (q.hasFlags) {
				ImGui::Text(tr("State: %s  (quest flags %08X)"), q.completed() ? tr("completed") : q.enabled() ? tr("running") : tr("not running"), q.questFlags);
				ImGui::BeginDisabled(_readOnly);
				bool done = q.completed();
				if (ImGui::Checkbox(tr("Completed"), &done)) {
					const auto raw = c->refID;
					const auto off = q.flagsOffset;
					const bool want = done;
					confirm(tr("Change the quest's completed flag"),
						tr("Set or clear the flag Quest.IsCompleted reads (bit 0x2 of the quest's saved flags).\n\nOnly the flag changes: no "
						   "stage runs, no script is told, rewards are not given or taken, and the quest's objectives and aliases stay as "
						   "they are. Use it to make a quest count as done (or not) for other quests and dialogue that check it. "
						   "Keep the original save."),
						[this, raw, off, want] {
							auto* cf = _save->findChangeForm(raw);
							if (!cf || off + 4 > cf->data.size()) {
								return;
							}
							if (want) {
								cf->data[off] |= 0x02;
							} else {
								cf->data[off] &= static_cast<std::uint8_t>(~0x02);
							}
							markChanged(want ? tr("Quest marked completed") : tr("Quest no longer marked completed"));
						});
				}
				ImGui::EndDisabled();
				tips::Tip(tr("The completed flag the game and Quest.IsCompleted() read. Only this flag changes."));
			} else {
				ImGui::TextDisabled("%s", tr("The save keeps no state for this quest (its plugin's start state applies)."));
			}
			if (q.stages.empty()) {
				ImGui::TextDisabled("%s", tr("No stage is saved for this quest."));
			} else {
				ImGui::TextUnformatted(Fmt(tr("Stages saved: %zu (ticked = done)"), q.stages.size()).c_str());
				ImGui::BeginDisabled(_readOnly);
				for (const auto& st : q.stages) {
					bool d = st.done;
					ImGui::PushID(st.index);
					if (ImGui::Checkbox(Fmt("%u", st.index).c_str(), &d)) {
						auto* cf = _save->findChangeForm(c->refID);
						if (cf && st.offset < cf->data.size()) {
							if (d) {
								cf->data[st.offset] |= 1;
							} else {
								cf->data[st.offset] &= static_cast<std::uint8_t>(~1);
							}
							markChanged(Fmt(d ? tr("Stage %u marked done") : tr("Stage %u marked not done"), st.index));
						}
					}
					ImGui::PopID();
					ImGui::SameLine();
				}
				ImGui::NewLine();
				ImGui::EndDisabled();
				ImGui::TextDisabled("%s", tr("Ticking a stage only marks it done (Quest.IsStageDone): its script fragment does not run."));
			}
		}
		// in the running game (the Starfield helper, an SFSE plugin): the game's own console commands for this quest
		if (c->refID.type() != RefID::Type::kCreated && ImGui::CollapsingHeader(trId("In the running game"), ImGuiTreeNodeFlags_DefaultOpen)) {
			const auto info = _live.info();
			const bool ready = info.connected && info.inGame && liveSF() && helperAtLeast(11);
			if (!ready) {
				ImGui::PushTextWrapPos(0);
				ImGui::TextDisabled("%s", !info.connected ? tr("Not connected: start Starfield with SFSE and the Starfield ReSaverHelper.dll installed.")
											  : !liveSF()   ? tr("The game connected is not Starfield.")
															: tr("Load a save in the game first."));
				ImGui::PopTextWrapPos();
			}
			ImGui::BeginDisabled(!ready || _live.busy());
			if (ImGui::SmallButton(tr("Read it from the game"))) {
				liveQuestSF("QUEST", fid, 0);
			}
			ImGui::SetNextItemWidth(90);
			ImGui::InputInt(tr("stage"), &_liveQuestSFStage, 0, 0);
			_liveQuestSFStage = std::clamp(_liveQuestSFStage, 0, 65535);
			ImGui::SameLine();
			if (ImGui::SmallButton(tr("Set stage (runs its script)"))) {
				liveQuestSF("SETSTAGE", fid, _liveQuestSFStage);
			}
			tips::Tip(tr("The game's own setstage command: the quest goes to that stage and the stage's script runs (rewards, follow-up quests, scenes)."));
			if (ImGui::SmallButton(tr("Complete"))) {
				liveQuestSF("COMPLETE", fid, 0);
			}
			tips::Tip(tr("The game's own completequest command for this quest."));
			ImGui::SameLine();
			if (ImGui::SmallButton(tr("Stop"))) {
				liveQuestSF("STOP", fid, 0);
			}
			tips::Tip(tr("The game's own stopquest command for this quest."));
			ImGui::SameLine();
			if (ImGui::SmallButton(tr("Reset"))) {
				liveQuestSF("RESET", fid, 0);
			}
			tips::Tip(tr("The game's own resetquest command for this quest."));
			ImGui::EndDisabled();
			if (!_liveQuestSFResult.empty()) {
				ImGui::PushTextWrapPos(0);
				ImGui::TextUnformatted(_liveQuestSFResult.c_str());
				ImGui::PopTextWrapPos();
			}
		}
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
			t += Fmt("%08X  %s", f, s.pluginOf(f).c_str());
			if (_db && (f >> 24) != 0xFF) {
				if (const auto d = _db->describe(f); !d.empty()) {
					t += "  " + d;
				}
			}
			t += "\n";
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
		checkItemsSF(v);
		cosaveCheck(v);
		return v;
	}
	return v;  // (with the script data read: App::computeCheckItems, its Papyrus part)
}

const std::vector<ops::SfTypeMismatch>& App::sfTypeMismatches()
{
	const PluginDB* db = _db && _db->starfield() && !namesLoading() ? _db.get() : nullptr;
	if (_sfMismatchSave != _save.get() || _sfMismatchDb != db || _sfMismatchGen != _editGen) {
		_sfMismatchSave = _save.get();
		_sfMismatchDb = db;
		_sfMismatchGen = _editGen;
		_sfMismatch.clear();
		if (_save && db) {
			_sfMismatch = ops::SfTypeMismatches(*_save, [db](std::uint32_t a_id) -> std::string {
				const auto* f = db->find(a_id);
				return f ? db->type(*f) : std::string();
			});
		}
	}
	return _sfMismatch;
}

void App::checkItemsSF(std::vector<CheckItem>& a_items)
{
	if (!_save || !sf()) {
		return;
	}
	// plugins that are light or medium now but were not when the save was made (or the other way): ids that no longer fit
	if (_sfOrder) {
		const auto kinds = loadorder::StarfieldKindChanges(_save->plugins.full, _save->plugins.lite, _save->plugins.medium, _save->formIDs, *_sfOrder);
		auto kindName = [](int k) { return k == 1 ? tr("light") : k == 2 ? tr("medium") : tr("full"); };
		std::string list;
		std::size_t broken = 0, shown = 0;
		for (const auto& k : kinds) {
			if (k.outOfRange == 0) {
				continue;
			}
			++broken;
			if (++shown <= 10) {
				list += Fmt(tr("\n  - %s: %s in the save, %s now: %zu of its %zu ids do not fit (e.g. %08X)"), k.plugin.c_str(), kindName(k.was), kindName(k.now),
					k.outOfRange, k.ids, k.example);
			}
		}
		if (broken > 0) {
			CheckItem it;
			it.level = Risk::kRisky;
			it.title = Fmt(tr("%zu plugin(s) changed kind since this save: their saved records land on other records"), broken);
			it.what = tr("A plugin was flagged light or medium (or the flag was removed) after this save was made. When a save loads, "
						 "Starfield rebuilds every saved form id for today's plugins: a light plugin keeps only the last 3 hex digits of "
						 "an id, a medium one the last 4 (Starfield.exe 1.16.244, TESFile::AdjustFormID). The save's ids of these plugins "
						 "that are larger than that now point at other records of the plugin, or at none: placed objects, quests and NPC "
						 "changes of the mod go to the wrong things.") +
					  list +
					  tr("\nThe fix is outside the save: give the plugin back the kind it had when you saved (undo the light / medium flag, "
						 "or reinstall the mod's earlier version), or load a save made after the change. Renumbering (\"compacting\") a "
						 "plugin's ids for the new kind does not help either: the save still holds the old numbers.");
			a_items.push_back(std::move(it));
		}
	}
	// change forms whose type is not the one the plugins give the form now: the game skips their saved data
	const auto& mm = sfTypeMismatches();
	if (!mm.empty()) {
		CheckItem it;
		it.level = Risk::kCaution;
		it.title = Fmt(tr("%zu change form(s) have another type in your plugins now: the game skips them"), mm.size());
		std::string list;
		for (std::size_t i = 0; i < mm.size() && i < 10; ++i) {
			list += Fmt("\n  - %08X: %s -> %s", mm[i].formID, mm[i].saved.c_str(), mm[i].now.c_str());
			if (_db) {
				if (const auto n = _db->describe(mm[i].formID); !n.empty()) {
					list += "  " + n;
				}
			}
		}
		if (mm.size() > 10) {
			list += Fmt(tr("\n  - and %zu more"), mm.size() - 10);
		}
		it.what = tr("The save stored these forms as one type, but a plugin you load now defines them as another (a mod changed or "
					 "replaced them). Starfield's loader logs \"Form ... was saved with form type ..., but currently has form type ...\" "
					 "and skips their saved changes (Starfield.exe 1.16.244), so they load as the plugins define them:") + list;
		CheckOption o;
		o.label = tr("Show it in Change forms");
		o.risk = Risk::kSafe;
		o.why = tr("Only shows information.");
		const auto first = _save->changeForms[mm.front().index].refID.data;
		o.act = [this, first] {
			select(Cat::kChangeForms, first);
			_showCheck = false;
		};
		it.options.push_back(std::move(o));
		std::set<std::uint32_t> raws;
		for (const auto& m : mm) {
			raws.insert(_save->changeForms[m.index].refID.data);
		}
		CheckOption r;
		r.label = Fmt(tr("Remove their saved changes (%zu)"), raws.size());
		r.risk = Risk::kSafe;
		r.why = tr("The game already skips them when the save loads, so the result in the game is the same; the save just no longer "
				   "carries data nothing uses.");
		r.act = [this, raws] {
			confirm(tr("Remove skipped change forms"),
				Fmt(tr("Delete %zu change form(s) the game skips because their type changed?\n\nOnly the save in memory changes: write it "
					   "with File > Save As under a new name."),
					raws.size()),
				[this, raws] { markEdited(Fmt(tr("Removed %zu change form(s) whose type changed"), ops::DeleteChangeForms(*_save, raws))); });
		};
		it.options.push_back(std::move(r));
		a_items.push_back(std::move(it));
	}
}

// "Add an item" under an inventory: search the plugins' names, pick a form of a type the save already carries as an
// item somewhere (so only real items are offered), give a count, add (AddInventoryItemSF)
void App::drawAddItemSF(const RefID& a_ref)
{
	static const ess::SaveFile*  typesFor = nullptr;
	static std::set<std::string> itemTypes;
	static char                  text[128] = "";
	static std::vector<std::uint32_t> found;
	static std::uint32_t         pick = 0;
	static std::uint32_t         count = 1;
	static std::string           result;
	if (!ImGui::TreeNode(tr("Add an item"))) {
		return;
	}
	if (!_db || !_db->loaded()) {
		ImGui::PushTextWrapPos(0);
		ImGui::TextDisabled("%s", tr("Needs the item names from your plugins: Tools > Read plugin names (the Starfield folder must be set in Options > Settings)."));
		ImGui::PopTextWrapPos();
		ImGui::TreePop();
		return;
	}
	const auto& s = *_save;
	if (typesFor != _save.get()) {
		// the record types of every item any inventory of this save holds
		typesFor = _save.get();
		itemTypes.clear();
		for (const auto& c : s.changeForms) {
			if (c.typeCode() > 1 || c.version != 153 || !(c.changeFlags & 0x8000020u)) {
				continue;
			}
			const auto d = DecodeReferenceSF(s.formIDOf(c.refID), c);
			for (const auto& it : d.items) {
				if (it.refIndex < c.sfRefs.size()) {
					if (const auto* f = _db->find(s.formIDOf(c.sfRefs[it.refIndex]))) {
						itemTypes.insert(_db->type(*f));
					}
				}
			}
		}
		found.clear();
		pick = 0;
		result.clear();
	}
	ImGui::BeginDisabled(_readOnly);
	ImGui::SetNextItemWidth(300);
	if (ImGui::InputTextWithHint("##find", tr("name, editor ID or form ID"), text, sizeof(text))) {
		found.clear();
		pick = 0;
		if (std::strlen(text) >= 2) {
			for (const auto id : _db->search(text, 2000)) {
				const auto* f = _db->find(id);
				if (f && itemTypes.count(_db->type(*f))) {
					found.push_back(id);
					if (found.size() >= 200) {
						break;
					}
				}
			}
		}
	}
	if (!found.empty() && ImGui::BeginListBox("##found", ImVec2(-1, 160))) {
		for (const auto id : found) {
			const auto label = Fmt("%08X  %s", id, _db->describe(id).c_str());
			if (ImGui::Selectable(label.c_str(), pick == id)) {
				pick = id;
			}
		}
		ImGui::EndListBox();
	} else if (std::strlen(text) >= 2 && found.empty()) {
		ImGui::TextDisabled("%s", tr("No item found with that text."));
	}
	ImGui::SetNextItemWidth(130);
	ImGui::InputScalar(tr("count"), ImGuiDataType_U32, &count);
	count = std::clamp<std::uint32_t>(count, 1, 1000000000u);
	ImGui::SameLine();
	ImGui::BeginDisabled(!pick);
	if (ImGui::Button(tr("Add"))) {
		std::string why;
		if (auto* cf = _save->findChangeForm(a_ref); cf && AddInventoryItemSF(*_save, *cf, pick, count, &why)) {
			result = Fmt(tr("Added %u x %s."), count, _db->describe(pick).c_str());
			markChanged(result);
		} else {
			result = tr("Not added: ") + why;
		}
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	if (!result.empty()) {
		ImGui::TextWrapped("%s", result.c_str());
	}
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("%s", tr("The item is added as one plain stack (no mods, full condition), as the game makes a new item. Keep the original save."));
	ImGui::PopTextWrapPos();
	ImGui::TreePop();
}

// the ships (and stations) the save keeps: references whose base object is a GBFM (ships, starstations: read from the
// plugins), and references with component data whose base was made in play (ships built or changed in the ship
// builder; their base is not in any plugin). Their inventory is their cargo.
// Starfield: every quest the save keeps (DecodeQuestSF), with its name; Open shows it (state and stages editable, and the
// running game's commands through the helper)
void App::drawQuestsSF()
{
	struct Row
	{
		RefID         ref;
		std::uint32_t fid = 0;
		std::string   name;
		int           state = 0;  // 0 not running, 1 running, 2 completed, -1 not saved
		std::size_t   stagesDone = 0, stages = 0;
	};
	static const ess::SaveFile* rowsFor = nullptr;
	static std::uint64_t        rowsGen = UINT64_MAX;
	static const void*          rowsDb = nullptr;
	static std::vector<Row>     rows;
	static char                 filter[128] = "";
	static int                  show = 1;  // 0 all, 1 running, 2 completed, 3 not running
	ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(trId("Quests"), &_showQuestsSF, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	const auto& s = *_save;
	const void* dbNow = _db && _db->loaded() ? static_cast<const void*>(_db.get()) : nullptr;
	if (rowsFor != _save.get() || rowsGen != static_cast<std::uint64_t>(_editGen) || rowsDb != dbNow) {
		rowsFor = _save.get();
		rowsGen = static_cast<std::uint64_t>(_editGen);
		rowsDb = dbNow;
		rows.clear();
		for (const auto& c : s.changeForms) {
			if (c.typeCode() != 8) {
				continue;
			}
			Row r;
			r.ref = c.refID;
			r.fid = s.formIDOf(c.refID);
			const auto q = DecodeQuestSF(c);
			r.state = !q.hasFlags ? -1 : q.completed() ? 2 : q.enabled() ? 1 : 0;
			r.stages = q.stages.size();
			for (const auto& st : q.stages) {
				r.stagesDone += st.done;
			}
			r.name = dbNow ? _db->describe(r.fid) : std::string();
			if (r.name.empty()) {
				r.name = s.pluginOf(r.fid);
			}
			rows.push_back(std::move(r));
		}
		std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.name < b.name; });
	}
	if (!dbNow) {
		ImGui::PushTextWrapPos(0);
		ImGui::TextDisabled("%s", tr("Quest names come from your plugins: Tools > Read plugin names (the Starfield folder must be set in Options > Settings). "
									 "Until then they are listed by form ID."));
		ImGui::PopTextWrapPos();
	}
	{
		const auto info = _live.info();
		ImGui::TextDisabled("%s", !info.connected ? tr("Running game: not connected (start Starfield with sfse_loader.exe to use the helper).")
								  : !info.inGame  ? tr("Running game: connected, at the main menu. Load a save in the game to use its quest commands.")
												  : tr("Running game: connected, a save is loaded. Open a quest for its commands."));
	}
	ImGui::SetNextItemWidth(260);
	ImGui::InputTextWithHint("##qf", tr("filter (name or form ID)"), filter, sizeof(filter));
	ImGui::SameLine();
	ImGui::RadioButton(tr("Running"), &show, 1);
	ImGui::SameLine();
	ImGui::RadioButton(tr("Completed"), &show, 2);
	ImGui::SameLine();
	ImGui::RadioButton(tr("Not running"), &show, 3);
	ImGui::SameLine();
	ImGui::RadioButton(tr("All"), &show, 0);
	std::string f = filter;
	for (auto& ch : f) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	std::vector<std::size_t> shown;
	for (std::size_t i = 0; i < rows.size(); ++i) {
		const auto& r = rows[i];
		if (show == 1 && r.state != 1) continue;
		if (show == 2 && r.state != 2) continue;
		if (show == 3 && r.state != 0 && r.state != -1) continue;
		if (!f.empty()) {
			std::string n = r.name + " " + Fmt("%08X", r.fid);
			for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
			if (n.find(f) == std::string::npos) continue;
		}
		shown.push_back(i);
	}
	ImGui::Text(tr("%zu of %zu quests"), shown.size(), rows.size());
	if (ImGui::BeginTable("##quests", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
			ImVec2(0, -1))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Quest"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(tr("Form ID"), ImGuiTableColumnFlags_WidthFixed, 80);
		ImGui::TableSetupColumn(tr("State"), ImGuiTableColumnFlags_WidthFixed, 100);
		ImGui::TableSetupColumn(tr("Stages done"), ImGuiTableColumnFlags_WidthFixed, 90);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(shown.size()));
		while (clip.Step()) {
			for (int k = clip.DisplayStart; k < clip.DisplayEnd; ++k) {
				const auto& r = rows[shown[static_cast<std::size_t>(k)]];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(r.name.c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%08X", r.fid);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(r.state == 2 ? tr("completed") : r.state == 1 ? tr("running") : r.state == 0 ? tr("not running") : tr("not saved"));
				ImGui::TableNextColumn();
				ImGui::Text("%zu / %zu", r.stagesDone, r.stages);
				ImGui::TableNextColumn();
				ImGui::PushID(k);
				if (ImGui::SmallButton(tr("Open"))) {
					select(Cat::kChangeForms, r.ref.data);
				}
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

void App::drawShipsSF()
{
	struct Row
	{
		RefID         ref;
		std::uint32_t fid = 0, base = 0;
		std::string   name;
		std::size_t   items = 0;
		std::uint64_t total = 0;
		bool          built = false, readable = false;
	};
	static const ess::SaveFile* rowsFor = nullptr;
	static std::uint64_t        rowsGen = UINT64_MAX;
	static std::vector<Row>     rows;
	ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(trId("Ships"), &_showShipsSF, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	const auto& s = *_save;
	if (!_db || !_db->loaded()) {
		ImGui::PushTextWrapPos(0);
		ImGui::TextDisabled("%s", tr("Needs the names from your plugins to tell ships from other objects: Tools > Read plugin names (the Starfield folder "
									 "must be set in Options > Settings)."));
		ImGui::PopTextWrapPos();
		ImGui::End();
		return;
	}
	if (rowsFor != _save.get() || rowsGen != static_cast<std::uint64_t>(_editGen)) {
		rowsFor = _save.get();
		rowsGen = static_cast<std::uint64_t>(_editGen);
		rows.clear();
		const auto shipNames = ShipNamesSF(s);
		for (const auto& c : s.changeForms) {
			if (c.typeCode() != 0 || c.version != 153) {
				continue;
			}
			const auto fid = s.formIDOf(c.refID);
			std::uint32_t base = 0;
			if (const auto* f = _db->find(fid); f && f->base) {
				base = f->base;
			} else {
				for (const auto& b : BaseObjectsSF(fid, c)) {
					base = s.formIDOf(b);
					break;
				}
			}
			const auto* bf = base ? _db->find(base) : nullptr;
			const bool  gbfm = bf && _db->type(*bf) == "GBFM";
			const bool  maybeBuilt = !bf && (base >> 24) == 0xFF;
			if (!gbfm && !maybeBuilt) {
				continue;
			}
			const auto d = DecodeReferenceSF(fid, c);
			if (!gbfm && !d.components) {
				continue;  // (a made-in-play base without component data: not a ship)
			}
			Row r;
			r.ref = c.refID;
			r.fid = fid;
			r.base = base;
			r.built = !gbfm;
			r.readable = d.ok;
			r.items = d.items.size();
			for (const auto& it : d.items) {
				r.total += it.total();
			}
			if (const auto it = shipNames.find(fid); it != shipNames.end()) {
				r.name = it->second;  // the name the player gave it
			} else if (gbfm) {
				r.name = _db->name(*bf);
			}
			if (r.name.empty()) {
				r.name = gbfm ? _db->describe(base) : tr("(built or changed in the ship builder)");
			}
			rows.push_back(std::move(r));
		}
		std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.items > b.items; });
	}
	ImGui::TextWrapped(tr("%zu ships and stations. Open one to see and change its cargo (its inventory), or add to it."), rows.size());
	if (ImGui::BeginTable("##ships", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
			ImVec2(0, -1))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Ship"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(tr("Reference"), ImGuiTableColumnFlags_WidthFixed, 80);
		ImGui::TableSetupColumn(tr("Cargo items"), ImGuiTableColumnFlags_WidthFixed, 90);
		ImGui::TableSetupColumn(tr("Total"), ImGuiTableColumnFlags_WidthFixed, 90);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
		ImGui::TableHeadersRow();
		for (std::size_t i = 0; i < rows.size(); ++i) {
			const auto& r = rows[i];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(r.name.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%08X", r.fid);
			ImGui::TableNextColumn();
			if (r.readable) {
				ImGui::Text("%zu", r.items);
			} else {
				ImGui::TextDisabled("%s", tr("not read"));
			}
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(r.total));
			ImGui::TableNextColumn();
			ImGui::PushID(static_cast<int>(i));
			if (ImGui::SmallButton(tr("Open"))) {
				select(Cat::kChangeForms, r.ref.data);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

// the player's level and XP (SetPlayerLevelSF): found among the actor values by their shape, which needs the plugins'
// names to tell actor values (AVIF) from other forms
void App::drawPlayerLevelSF()
{
	static const ess::SaveFile* cachedFor = nullptr;
	static std::uint64_t        cachedGen = UINT64_MAX;
	static SfPlayerXP           xp;
	static int                  wantLevel = 0;
	static float                wantXP = 0;
	static std::string          result;
	ImGui::SeparatorText(tr("Level and XP"));
	const auto& h = _save->header;
	ImGui::Text(tr("Level %u, XP %.0f of %.0f to the next level"), h.playerLevel, h.currentXP, h.neededXP);
	if (!_db || !_db->loaded()) {
		ImGui::PushTextWrapPos(0);
		ImGui::TextDisabled("%s", tr("To change them, ReSaver Plus needs the names from your plugins: Tools > Read plugin names."));
		ImGui::PopTextWrapPos();
		return;
	}
	auto isAV = [this](std::uint32_t f) {
		if (const auto* d = _db->find(f)) {
			return _db->type(*d) == "AVIF";
		}
		return f > 0 && f < 0x800;  // (the engine's own actor values: in no plugin)
	};
	if (cachedFor != _save.get() || cachedGen != static_cast<std::uint64_t>(_editGen)) {
		cachedFor = _save.get();
		cachedGen = static_cast<std::uint64_t>(_editGen);
		xp = SfPlayerXP{};
		for (const auto& c : _save->changeForms) {
			if (c.typeCode() == 1 && _save->formIDOf(c.refID) == 0x14) {
				xp = FindPlayerXPSF(*_save, c, isAV);
			}
		}
		wantLevel = static_cast<int>(h.playerLevel);
		wantXP = h.currentXP;
	}
	if (!xp.ok) {
		ImGui::TextDisabled("%s", (tr("Cannot be changed here: ") + xp.error).c_str());
		return;
	}
	ImGui::TextDisabled(tr("Total experience saved: %.0f"), xp.experience);
	ImGui::BeginDisabled(_readOnly);
	ImGui::SetNextItemWidth(120);
	ImGui::InputInt(tr("level"), &wantLevel);
	wantLevel = std::clamp(wantLevel, 1, 1000);
	const float need = XPForLevelSF(static_cast<unsigned>(wantLevel) + 1) - XPForLevelSF(static_cast<unsigned>(wantLevel));
	ImGui::SameLine();
	ImGui::SetNextItemWidth(140);
	ImGui::InputFloat(tr("XP into it"), &wantXP, 0, 0, "%.0f");
	wantXP = std::clamp(wantXP, 0.0f, std::max(0.0f, need - 1.0f));
	ImGui::SameLine();
	ImGui::TextDisabled(tr("(of %.0f)"), need);
	if (ImGui::Button(tr("Set level and XP"))) {
		std::string why;
		if (SetPlayerLevelSF(*_save, static_cast<std::uint16_t>(wantLevel), wantXP, isAV, &why)) {
			result = Fmt(tr("Level %d, XP %.0f."), wantLevel, wantXP);
			markChanged(result);
		} else {
			result = tr("Not changed: ") + why;
		}
	}
	ImGui::EndDisabled();
	if (!result.empty()) {
		ImGui::TextWrapped("%s", result.c_str());
	}
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("%s", tr("Sets the level and the experience the game keeps for you (and the load menu's). Only these change: nothing else "
								 "the game does at a level-up happens (no skill point is added). Keep the original save."));
	ImGui::PopTextWrapPos();
}

// the player's saved actor values: base values (editable where saved) and the three modifiers (shown)
void App::drawActorValuesSF(const RefID& a_ref)
{
	static const ess::SaveFile* cachedFor = nullptr;
	static std::uint64_t        cachedGen = UINT64_MAX;
	static SfActorValues        av;
	static char                 filter[64] = "";
	if (!ImGui::CollapsingHeader(trId("Actor values"))) {
		return;
	}
	if (!_db || !_db->loaded()) {
		ImGui::PushTextWrapPos(0);
		ImGui::TextDisabled("%s", tr("Needs the names from your plugins: Tools > Read plugin names."));
		ImGui::PopTextWrapPos();
		return;
	}
	const auto* c = _save->findChangeForm(a_ref);
	if (!c) {
		return;
	}
	if (cachedFor != _save.get() || cachedGen != static_cast<std::uint64_t>(_editGen)) {
		cachedFor = _save.get();
		cachedGen = static_cast<std::uint64_t>(_editGen);
		av = FindActorValuesSF(*_save, *c, [this](std::uint32_t f) {
			if (const auto* d = _db->find(f)) {
				return _db->type(*d) == "AVIF";
			}
			return f > 0 && f < 0x800;
		});
	}
	if (!av.ok) {
		ImGui::TextDisabled("%s", (tr("Not found: ") + av.error).c_str());
		return;
	}
	ImGui::PushTextWrapPos(0);
	ImGui::TextDisabled("%s", tr("The values the save keeps for you. A base value is saved only when it is not the game's default; the modifiers "
								 "only when not 0: Permanent (scripts and perks, ModValue), Temporary (equipment and active effects) and Damage "
								 "(DamageValue; 0 = fully restored). Saved numbers can be changed (press Enter). Use with care: keep the original save."));
	ImGui::PopTextWrapPos();
	ImGui::SetNextItemWidth(240);
	ImGui::InputTextWithHint("##avf", tr("filter"), filter, sizeof(filter));
	auto name = [&](std::size_t refIndex) {
		const auto f = _save->formIDOf(c->sfRefs[refIndex]);
		auto n = _db->describe(f);
		if (n.empty()) {
			n = Fmt(tr("(engine value %X)"), f);
		}
		return std::make_pair(f, n);
	};
	auto match = [&](const std::string& n) {
		if (!filter[0]) {
			return true;
		}
		std::string a = n, b = filter;
		for (auto& ch : a) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		for (auto& ch : b) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return a.find(b) != std::string::npos;
	};
	auto valueAt = [&](std::size_t off) {
		float v = 0;
		std::memcpy(&v, c->data.data() + off, 4);
		return v;
	};
	if (ImGui::BeginTable("##av", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY, ImVec2(0, 360))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Actor value"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(tr("Base"), ImGuiTableColumnFlags_WidthFixed, 110);
		ImGui::TableSetupColumn(tr("Permanent"), ImGuiTableColumnFlags_WidthFixed, 90);
		ImGui::TableSetupColumn(tr("Temporary"), ImGuiTableColumnFlags_WidthFixed, 90);
		ImGui::TableSetupColumn(tr("Damage"), ImGuiTableColumnFlags_WidthFixed, 90);
		ImGui::TableHeadersRow();
		// one row per actor value: its base (list 1) and its modifiers (list 2), matched by form
		std::map<std::uint32_t, std::pair<const SfActorValue*, const SfActorValue*>> rows;
		std::map<std::uint32_t, std::string> names;
		for (const auto& e : av.base) {
			const auto [f, n] = name(e.refIndex);
			rows[f].first = &e;
			names[f] = n;
		}
		for (const auto& e : av.modifiers) {
			const auto [f, n] = name(e.refIndex);
			rows[f].second = &e;
			names[f] = n;
		}
		std::vector<std::pair<std::string, std::uint32_t>> order;
		for (const auto& [f, n] : names) {
			if (match(n)) {
				order.emplace_back(n, f);
			}
		}
		std::sort(order.begin(), order.end());
		ImGui::BeginDisabled(_readOnly);
		for (const auto& [n, f] : order) {
			const auto& [b, m] = rows[f];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(n.c_str());
			ImGui::TableNextColumn();
			if (b && b->value[0] != SIZE_MAX) {
				float v = valueAt(b->value[0]);
				ImGui::PushID(static_cast<int>(f));
				ImGui::SetNextItemWidth(-1);
				if (ImGui::InputFloat("##b", &v, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
					if (auto* cf = _save->findChangeForm(a_ref); cf && SetActorValueSF(*cf, b->value[0], v)) {
						markChanged(Fmt(tr("%s set to %.2f"), n.c_str(), v));
					}
				}
				ImGui::PopID();
			} else {
				ImGui::TextDisabled("%s", tr("default"));
			}
			static const char* kModNames[3] = { "Permanent", "Temporary", "Damage" };
			for (int k = 0; k < 3; ++k) {
				ImGui::TableNextColumn();
				if (m && m->value[k] != SIZE_MAX) {
					float v = valueAt(m->value[k]);
					ImGui::PushID(static_cast<int>(f) * 4 + k + 1);
					ImGui::SetNextItemWidth(-1);
					if (ImGui::InputFloat("##m", &v, 0, 0, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
						if (auto* cf = _save->findChangeForm(a_ref); cf && SetActorValueSF(*cf, m->value[k], v)) {
							markChanged(Fmt(tr("%s: %s modifier set to %.2f"), n.c_str(), tr(kModNames[k]), v));
						}
					}
					ImGui::PopID();
				}
			}
		}
		ImGui::EndDisabled();
		ImGui::EndTable();
	}
}
