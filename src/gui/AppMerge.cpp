// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Tools > Merged patch: one plugin that combines the changes of mods that edit the same records.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/LoadOrder.h"
#include "core/ScriptSources.h"

#include "imgui.h"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <fstream>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	void Colored(std::uint32_t a_col, const std::string& a_text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, a_col);
		ImGui::TextWrapped("%s", a_text.c_str());
		ImGui::PopStyleColor();
	}

	const char* const kModFolder = "ReSaver Plus Merged Patch";
	const char* const kPatchName = "ReSaverPP Merged Patch.esp";
}

void App::drawMergedPatch()
{
	prepareWindow("Merged patch", ImVec2(0.8f, 0.85f));
	if (!ImGui::Begin("Merged patch", &_showMerge, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Merged patch");

	// a finished build
	if (!_mergeBusy && _mergeThread.joinable()) {
		_mergeThread.join();
		_mergeResult = std::move(_mergeIncoming);
		_mergeSaved.clear();
	}

	const std::filesystem::path pluginsTxt =
		_mo2 && !_profile.empty() ? _mo2->profilesDir / platform::FromUtf8(_profile) / "plugins.txt" : loadorder::GamePluginsTxt(_vr);
	const std::filesystem::path outFile =
		_mo2 ? _mo2->modsDir / kModFolder / kPatchName : (gameFolder().empty() ? std::filesystem::path() : gameFolder() / "Data" / kPatchName);

	ImGui::NewLine();
	ImGui::TextWrapped("When two mods change the same leveled list, container, NPC, keyword list, form list or outfit, the game only "
					   "uses the one loaded last, and the other mod's additions are lost. The merged patch reads every plugin in your "
					   "load order and writes one plugin that keeps both: what each mod added or removed is combined. Every other value "
					   "stays as the winning mod has it, as the game does without a patch.");
	ImGui::TextWrapped("The patch is a light (ESL-flagged) plugin, so it takes no load order slot. It only needs a mod as a master when a "
					   "patched record comes from that mod or points at one of its forms. Rebuild it after you add, remove or reorder mods.");
	ImGui::Spacing();
	ImGui::TextDisabled("Load order: %s", platform::ToUtf8(pluginsTxt).c_str());
	if (_mo2 && !_profile.empty()) {
		ImGui::TextDisabled("Mod Organizer profile: %s", _profile.c_str());
	}

	const bool noMods = _sourcesPath.empty() && !_sources.scanned();
	if (noMods) {
		Colored(theme::Warn(), "ReSaver Plus does not know where your mods are yet: set your mods folder (Mod Organizer) or game folder in "
							   "Options > Settings, then come back here.");
		ImGui::End();
		return;
	}

	// waiting for the scan of the mods, then the build starts by itself
	auto start = [this, pluginsTxt] {
		auto opt = std::make_shared<merge::Options>();
		for (int i = 0; i < static_cast<int>(merge::Category::kCount); ++i) {
			opt->enabled[i] = !_mergeOff[i];
		}
		_mergeOptionsChanged = false;
		opt->loadOrder = merge::ReadLoadOrder(pluginsTxt, gameFolder());
		opt->patchName = kPatchName;
		auto src = std::make_shared<ScriptSources>(_sources);
		merge::UseSources(*opt, *src);
		_mergeLoadOrder = opt->loadOrder.size();
		_mergeProgress = std::make_unique<merge::Progress>();
		_mergeBusy = true;
		auto* prog = _mergeProgress.get();
		_mergeThread = std::thread([this, opt, src, prog] {
			auto r = std::make_unique<merge::Result>(merge::Build(*opt, prog));
			_mergeIncoming = std::move(r);
			_mergeBusy = false;
		});
	};
	if (_mergeWaitScan && !scanning() && _sources.scanned()) {
		_mergeWaitScan = false;
		start();
	}

	// ---- what to merge -----------------------------------------------------------------------------
	ImGui::Spacing();
	if (ImGui::CollapsingHeader("What to merge", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("Untick a kind of change to leave it to the winning mod, as the game does without a patch. Under each kind is "
						   "everything it covers. SAFE: nothing to check. CAUTION: hover the row for what can go wrong. RISKY: check the "
						   "records it changed after building (they are listed in red below and in the report).");
		// how many changes of each kind the last build made
		std::size_t counts[static_cast<int>(merge::Category::kCount)] = {};
		if (_mergeResult) {
			for (const auto& m : _mergeResult->merged) {
				for (const auto c : m.categories) ++counts[static_cast<int>(c)];
			}
		}
		if (ImGui::SmallButton("Tick all")) {
			for (auto& off : _mergeOff) off = false;
			_mergeOptionsChanged = true;
			saveSettings();
		}
		tips::Tip("Merge every kind of change.");
		ImGui::SameLine();
		if (ImGui::SmallButton("Only the green ones")) {
			for (int i = 0; i < static_cast<int>(merge::Category::kCount); ++i) {
				_mergeOff[i] = merge::CategoryRisk(static_cast<merge::Category>(i)) > 0;
			}
			_mergeOptionsChanged = true;
			saveSettings();
		}
		tips::Tip("Merge only the safe kinds: leveled lists, containers, outfits, keywords, item values, names and models.");
		ImGui::SameLine();
		if (ImGui::SmallButton("Green and yellow")) {
			for (int i = 0; i < static_cast<int>(merge::Category::kCount); ++i) {
				_mergeOff[i] = merge::CategoryRisk(static_cast<merge::Category>(i)) > 1;
			}
			_mergeOptionsChanged = true;
			saveSettings();
		}
		tips::Tip("Leave out the risky kinds (scripts on records; a cell's location, owner, music and other settings).");
		if (ImGui::BeginTable("mergekinds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 1.6f);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("CAUTION").x + ImGui::GetFontSize());
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 4.0f);
			ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.5f);
			for (int i = 0; i < static_cast<int>(merge::Category::kCount); ++i) {
				const auto cat = static_cast<merge::Category>(i);
				const int  risk = merge::CategoryRisk(cat);
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				bool on = !_mergeOff[i];
				if (ImGui::Checkbox("##on", &on)) {
					_mergeOff[i] = !on;
					_mergeOptionsChanged = true;
					saveSettings();
				}
				ImGui::TableNextColumn();
				ImGui::PushStyleColor(ImGuiCol_Text, risk >= 2 ? theme::Bad() : risk == 1 ? theme::Caution() : theme::Good());
				ImGui::TextUnformatted(risk >= 2 ? "RISKY" : risk == 1 ? "CAUTION" : "SAFE");
				ImGui::PopStyleColor();
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", merge::CategoryName(cat));
				tips::Tip(merge::CategoryWhy(cat));
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				ImGui::TextWrapped("%s", merge::CategoryDetail(cat));
				ImGui::PopStyleColor();
				tips::Tip(merge::CategoryWhy(cat));
				ImGui::TableNextColumn();
				if (_mergeResult) {
					ImGui::TextDisabled("%zu change(s) last time", counts[i]);
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		if (_mergeOptionsChanged && _mergeResult) {
			Colored(theme::Warn(), "Changed since the last build: build again to apply.");
		}
	}

	ImGui::Spacing();
	ImGui::BeginDisabled(_mergeBusy || _mergeWaitScan);
	if (ImGui::Button(_mergeResult ? "Build the merged patch again" : "Build the merged patch")) {
		if (ensureSourcesScanned()) {
			start();
		} else {
			_mergeWaitScan = true;
		}
	}
	ImGui::EndDisabled();
	tips::Tip("Read every plugin of your load order and work out the merged patch. Nothing is written until you save it.");
	if (_mergeWaitScan) {
		ImGui::SameLine();
		ImGui::TextDisabled("finding your mods first...");
		drawBackgroundProgress(0);
	}
	if (_mergeBusy && _mergeProgress) {
		ImGui::SameLine();
		const auto total = _mergeProgress->total.load(), done = _mergeProgress->done.load();
		ImGui::ProgressBar(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f, ImVec2(-1, 0),
			Fmt("%s  %zu / %zu", _mergeProgress->current().c_str(), done, total).c_str());
		if (ImGui::Button("Cancel")) {
			_mergeProgress->cancel = true;
		}
	}
	if (!_mergeResult || _mergeBusy) {
		ImGui::End();
		return;
	}

	const auto& r = *_mergeResult;
	ImGui::Separator();
	if (!r.ok) {
		Colored(theme::Bad(), "The patch could not be built: " + r.error);
		ImGui::End();
		return;
	}
	ImGui::Text("%zu of %zu plugins read. %zu records are changed differently by two or more plugins.", r.pluginsRead, _mergeLoadOrder, r.conflicts);
	Colored(theme::Good(), Fmt("%zu records merged into the patch, with %zu masters.", r.merged.size(), r.masters.size()));
	if (!r.skipped.empty()) {
		ImGui::TextDisabled("%zu could not be merged; for those the winning mod's version stays, as without a patch.", r.skipped.size());
	}
	if (!r.missing.empty()) {
		Colored(theme::Warn(), Fmt("%zu plugins of the load order were not found or could not be read (see the list below); their "
								   "changes are not in the patch.",
								   r.missing.size()));
	}

	// save
	ImGui::Spacing();
	ImGui::BeginDisabled(outFile.empty() || r.merged.empty());
	auto write = [this](const std::filesystem::path& a_file) {
		std::error_code ec;
		std::filesystem::create_directories(a_file.parent_path(), ec);
		std::ofstream f(a_file, std::ios::binary);
		f.write(reinterpret_cast<const char*>(_mergeResult->file.data()), static_cast<std::streamsize>(_mergeResult->file.size()));
		if (!f) {
			_error = "Could not write " + platform::ToUtf8(a_file);
			return;
		}
		f.close();
		auto rep = a_file;
		rep.replace_extension(".txt");
		std::ofstream(rep) << _mergeResult->report();
		_mergeSaved = platform::ToUtf8(a_file);
		_status = "Wrote the merged patch: " + _mergeSaved;
	};
	if (ImGui::Button(_mo2 ? "Save it as a new mod in Mod Organizer" : "Save it in the game's Data folder")) {
		std::error_code ec;
		if (std::filesystem::exists(outFile, ec)) {
			confirm("Replace the merged patch", "Replace the merged patch made before?\n\n" + platform::ToUtf8(outFile),
				[write, outFile] { write(outFile); });
		} else {
			write(outFile);
		}
	}
	ImGui::EndDisabled();
	tips::Tipf("Write %s and a report of what was merged (.txt) to %s", kPatchName, platform::ToUtf8(outFile.parent_path()).c_str());
	ImGui::SameLine();
	ImGui::BeginDisabled(r.merged.empty());
	if (ImGui::Button("Save as...")) {
		if (auto f = platform::SaveFileDialog(outFile.empty() ? std::filesystem::path(kPatchName) : outFile, "esp", "Merged patch")) {
			write(*f);
		}
	}
	ImGui::EndDisabled();
	tips::Tip("Choose where to write the patch yourself.");
	if (!_mergeSaved.empty()) {
		Colored(theme::Good(), "Saved: " + _mergeSaved);
		if (_mo2) {
			ImGui::TextWrapped("In Mod Organizer: press F5 (refresh), tick the mod \"%s\" on the left, and make sure %s is ticked at the "
							   "bottom of the plugin list on the right. Leave it last.",
				kModFolder, kPatchName);
		} else {
			ImGui::TextWrapped("Enable %s in your mod manager or the game's launcher and keep it last in the load order.", kPatchName);
		}
		ImGui::TextWrapped("Before playing, you can open it in SSEEdit (Check for Errors) to see what it changed.");
	}

	// details
	ImGui::Spacing();
	if (ImGui::BeginChild("mergedetails", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
		if (ImGui::CollapsingHeader(Fmt("Merged records (%zu)###merged", r.merged.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGuiListClipper clip;
			clip.Begin(static_cast<int>(r.merged.size()));
			while (clip.Step()) {
				for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
					const auto& m = r.merged[static_cast<std::size_t>(i)];
					std::string changes;
					for (const auto& c : m.changes) {
						changes += (changes.empty() ? "" : ";  ") + c;
					}
					// green: only safe kinds of changes; yellow: read first; red: risky (see the report for each change)
					const std::uint32_t riskCol = m.risk >= 2 ? theme::Bad() : m.risk == 1 ? theme::Caution() : theme::Good();
					ImGui::PushStyleColor(ImGuiCol_Text, riskCol);
					ImGui::Text("%s %s [%s]", m.type.c_str(), m.edid.c_str(), m.id.c_str());
					ImGui::PopStyleColor();
					tips::Tipf("Starts from %s (the winning mod).%s%s", m.winner.c_str(), changes.empty() ? "" : "\nAdded from the others: ",
						changes.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("winner %s%s%s", m.winner.c_str(), changes.empty() ? "" : "  |  ", changes.c_str());
				}
			}
		}
		if (ImGui::CollapsingHeader(Fmt("Masters of the patch (%zu)###masters", r.masters.size()).c_str())) {
			for (const auto& m : r.masters) {
				auto it = r.masterWhy.find(m);
				ImGui::BulletText("%s", m.c_str());
				if (it != r.masterWhy.end()) {
					ImGui::SameLine();
					ImGui::TextDisabled("needed by %s", it->second.c_str());
				}
			}
		}
		if (!r.skipped.empty() && ImGui::CollapsingHeader(Fmt("Not merged (%zu)###skipped", r.skipped.size()).c_str())) {
			for (const auto& s : r.skipped) {
				ImGui::BulletText("%s", s.c_str());
			}
		}
		if (!r.fixes.empty() && ImGui::CollapsingHeader(Fmt("Broken references the patch fixes (%zu)###fixes", r.fixes.size()).c_str(),
									  ImGuiTreeNodeFlags_DefaultOpen)) {
			for (const auto& s : r.fixes) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Good());
				ImGui::BulletText("%s", s.c_str());
				ImGui::PopStyleColor();
			}
		}
		if (!r.warnings.empty() && ImGui::CollapsingHeader(Fmt("Problems in your load order (%zu)###warnings", r.warnings.size()).c_str(),
										 ImGuiTreeNodeFlags_DefaultOpen)) {
			for (const auto& s : r.warnings) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
				ImGui::BulletText("%s", s.c_str());
				ImGui::PopStyleColor();
			}
		}
		if (!r.missing.empty() && ImGui::CollapsingHeader(Fmt("Plugins not found (%zu)###missing", r.missing.size()).c_str())) {
			for (const auto& s : r.missing) {
				ImGui::BulletText("%s", s.c_str());
			}
		}
	}
	ImGui::EndChild();
	ImGui::End();
}

// ==========================================================================================
// file conflicts: loose files and archives
// ==========================================================================================

void App::drawFileConflicts()
{
	prepareWindow("File conflicts", ImVec2(0.8f, 0.85f));
	if (!ImGui::Begin("File conflicts", &_showAssets, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("File conflicts");
	if (!_assetsBusy && _assetsThread.joinable()) {
		_assetsThread.join();
		_assetsResult = std::move(_assetsIncoming);
	}
	ImGui::NewLine();
	ImGui::TextWrapped("When two mods have the same file (a mesh, texture, script...), the game uses one of them. Loose files beat every "
					   "archive (BSA); among loose files your mod order decides; among archives the plugin load order. This lists which "
					   "mod overrides which, loose and inside archives. A script overridden by another mod's copy is worth a look: one "
					   "of the two mods may not work as its author meant.");
	if (!_mo2) {
		Colored(theme::Warn(), "Without Mod Organizer only the game's Data folder can be read: files a mod manager already copied over each "
							   "other there cannot be told apart any more. Archives against loose files still show.");
	}
	ImGui::BeginDisabled(_assetsBusy);
	if (ImGui::Button(_assetsResult ? "Look again" : "Find file conflicts")) {
		assets::Input in;
		if (_mo2) {
			in.modsDir = _mo2->modsDir;
			in.overwrite = _mo2->overwriteDir;
			if (!_profile.empty()) {
				in.modsLowFirst = LoadModList(_mo2->profilesDir / platform::FromUtf8(_profile));
				std::reverse(in.modsLowFirst.begin(), in.modsLowFirst.end());
			}
		}
		if (!gameFolder().empty()) {
			in.gameData = gameFolder() / "Data";
		}
		const auto pluginsTxt =
			_mo2 && !_profile.empty() ? _mo2->profilesDir / platform::FromUtf8(_profile) / "plugins.txt" : loadorder::GamePluginsTxt(_vr);
		in.loadOrder = merge::ReadLoadOrder(pluginsTxt, gameFolder());
		_assetsProgress = std::make_unique<assets::Progress>();
		_assetsBusy = true;
		auto* prog = _assetsProgress.get();
		_assetsThread = std::thread([this, in, prog] {
			auto r = std::make_unique<assets::Result>(assets::Find(in, prog));
			_assetsIncoming = std::move(r);
			_assetsBusy = false;
		});
	}
	ImGui::EndDisabled();
	tips::Tip("Read the file list of every enabled mod and every archive the game loads (the files themselves are not opened).");
	if (_assetsBusy && _assetsProgress) {
		ImGui::SameLine();
		const auto total = _assetsProgress->total.load(), done = _assetsProgress->done.load();
		ImGui::ProgressBar(total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f, ImVec2(-1, 0),
			Fmt("%s  %zu / %zu", _assetsProgress->current().c_str(), done, total).c_str());
	}
	if (!_assetsResult || _assetsBusy) {
		ImGui::End();
		return;
	}
	const auto& r = *_assetsResult;
	ImGui::Text("%zu files in %zu folders and %zu archives; %zu paths come from more than one place (%zu scripts).", r.files, r.modsRead,
		r.archives, r.conflicts.size(), r.scriptConflicts);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
	ImGui::InputTextWithHint("##assetfilter", "Filter: mod name or file", _assetsFilter, sizeof(_assetsFilter));
	tips::Tip("Show only pairs where a mod's name or one of the files contains this text.");
	ImGui::SameLine();
	ImGui::Checkbox("Only scripts", &_assetsScriptsOnly);
	tips::Tip("Only pairs where one mod overrides another's scripts (.pex).");
	ImGui::SameLine();
	ImGui::Checkbox("Hide the base game", &_assetsHideGame);
	tips::Tip("Hide mods overriding the base game's own files: replacing them is what most mods are for.");
	if (ImGui::Button("Save the full list...")) {
		if (auto f = platform::SaveFileDialog("ReSaverPP File Conflicts.txt", "txt", "File conflicts")) {
			std::ofstream(*f) << r.report(100000);
			_status = "Wrote " + platform::ToUtf8(*f);
		}
	}
	tips::Tip("Write every conflict to a text file.");
	auto lower = [](std::string x) {
		for (auto& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return x;
	};
	const auto flt = lower(_assetsFilter);
	if (ImGui::BeginChild("assetlist", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
		std::size_t shown = 0;
		for (std::size_t pi = 0; pi < r.pairs.size(); ++pi) {
			const auto& p = r.pairs[pi];
			if ((_assetsScriptsOnly && !p.scripts) || (_assetsHideGame && p.loser == "Game Data")) {
				continue;
			}
			if (!flt.empty() && lower(p.winner).find(flt) == std::string::npos && lower(p.loser).find(flt) == std::string::npos) {
				bool any = false;
				for (const auto ci : p.conflicts) {
					if (r.conflicts[ci].path.find(flt) != std::string::npos) {
						any = true;
						break;
					}
				}
				if (!any) continue;
			}
			if (++shown > 2000) {
				ImGui::TextDisabled("... more: narrow the filter");
				break;
			}
			ImGui::PushID(static_cast<int>(pi));
			const auto head = Fmt("%s  overrides  %s:  %zu file(s)%s", p.winner.c_str(), p.loser.c_str(), p.files,
				p.scripts ? Fmt(", %zu script(s)", p.scripts).c_str() : "");
			if (p.scripts) ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
			const bool open = ImGui::TreeNode(head.c_str());
			if (p.scripts) ImGui::PopStyleColor();
			if (open) {
				std::size_t n = 0;
				for (const auto ci : p.conflicts) {
					const auto& c = r.conflicts[ci];
					if (_assetsScriptsOnly && !c.path.ends_with(".pex")) continue;
					if (++n > 500) {
						ImGui::TextDisabled("... %zu more (Save the full list)", p.conflicts.size() - 500);
						break;
					}
					std::string from;
					for (std::size_t i = 0; i < c.sources.size(); ++i) {
						from += (i ? "  <  " : "") + r.sources[c.sources[i]].label();
					}
					ImGui::TextUnformatted(c.path.c_str());
					tips::Tipf("Lowest to highest (the last one is used):\n%s", from.c_str());
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if (!shown) ImGui::TextDisabled("Nothing to show with these filters.");
	}
	ImGui::EndChild();
	ImGui::End();
}
