// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The SKSE / F4SE co-save (.skse / .f4se next to the save): what each script extender plugin keeps there, which of
// them are no longer installed (their DLL searched for the plugin's unique id), PapyrusUtil's StorageUtil data by
// plugin, key and object, and removing the data of plugins that are gone. The cleaned co-save is written next to the
// new save with File > Save As (core/Cosave.h).
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/Cosave.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <thread>

using namespace ess;

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

	// the DLL search, on its own thread; it owns everything it reads
	struct DllJob
	{
		std::vector<cosave::Dll>                                    dlls;
		std::map<std::uint32_t, std::vector<std::size_t>>           found;
		std::size_t                                                 searched = 0;
		std::atomic<bool>                                           done{ false };
	};

	const char* StatusWord(cosave::Status a_s)
	{
		switch (a_s) {
		case cosave::Status::kExtender: return tr("script extender");
		case cosave::Status::kInstalled: return tr("installed");
		case cosave::Status::kDisabled: return tr("disabled");
		case cosave::Status::kNotInstalled: return tr("not installed");
		default: return tr("not checked");
		}
	}
}

struct App::CosaveView
{
	const void*                      save = nullptr;
	std::uint64_t                    gen = UINT64_MAX;
	bool                             ok = false;
	std::string                      error;
	cosave::File                     file;
	cosave::View                     view;
	cosave::papu::Breakdown          papu;
	std::string                      jcHeader;
	cosave::jc::Breakdown            jc;  // JContainers' object tree
	// the DLL search: keyed by the folders searched
	std::string                      dllKey;
	std::shared_ptr<DllJob>          job;
	bool                             jobApplied = false;
	// the window
	std::set<std::uint32_t>          ticked;
	bool                             tickedInit = false;
	bool                             papuGone = true;
	std::uint32_t                    selected = 0;
	bool                             haveSelected = false;
	// PapyrusUtil objects on forms of plugins no longer in the load order
	std::set<std::string>            gonePlugins;  // lowercase
	std::size_t                      goneObjects = 0, goneBytes = 0;
};

App::CosaveView& App::cosaveView()
{
	if (!_cosaveView) {
		_cosaveView = std::make_shared<CosaveView>();
	}
	auto& c = *_cosaveView;
	const void* key = _save.get();
	if (c.save != key || c.gen != _editGen) {
		const bool sameSave = c.save == key;
		auto       job = sameSave ? c.job : nullptr;
		auto       dllKey = sameSave ? c.dllKey : std::string();
		auto       ticked = sameSave ? c.ticked : std::set<std::uint32_t>{};
		const bool tickedInit = sameSave && c.tickedInit;
		c = CosaveView{};
		c.save = key;
		c.gen = _editGen;
		c.job = job;
		c.dllKey = dllKey;
		c.tickedInit = tickedInit;
		if (_save && !_save->cosave.empty()) {
			try {
				c.file = cosave::Parse(_save->cosave);
				c.ok = true;
				c.papu = cosave::papu::Analyze(c.file);
				c.jcHeader = cosave::jc::Header(c.file);
				c.jc = cosave::jc::Analyze(c.file);
			} catch (const std::exception& e) {
				c.error = e.what();
			}
		}
		for (auto u : ticked) {
			if (c.file.find(u)) {
				c.ticked.insert(u);
			}
		}
	}
	if (!c.ok) {
		return c;
	}
	// start (or restart) the DLL search once the mods have been read
	if (!scanning()) {
		const auto gameData = gameFolder().empty() ? std::filesystem::path() : gameFolder() / "Data";
		auto       roots = cosave::RootsOf(*_sources);
		const auto k = platform::ToUtf8(roots.root) + "|" + std::to_string(_sources->modsScanned) + "|" + platform::ToUtf8(roots.gameData) + "|" +
					   platform::ToUtf8(gameData) + "|" + std::to_string(roots.enabled.size());
		if (k != c.dllKey || !c.job) {
			c.dllKey = k;
			c.jobApplied = false;
			auto job = std::make_shared<DllJob>();
			c.job = job;
			std::set<std::uint32_t> uids;
			for (const auto& p : c.file.plugins) {
				uids.insert(p.uid);
			}
			const bool fo4Save = c.file.fo4, x86 = _save->isLE();
			// the game's Data folder: the script extender's own DLLs and anything installed there by hand
			const auto data = roots.scanned && roots.gameData == gameData ? std::filesystem::path() : gameData;
			std::thread([job, roots, data, uids, fo4Save, x86] {
				try {
					job->dlls = cosave::ListPluginDlls(roots, data, fo4Save);
					job->found = cosave::FindIds(job->dlls, uids, x86, &job->searched);
				} catch (const std::exception&) {
				}
				job->done = true;
			}).detach();
		}
	}
	if (c.job && c.job->done && !c.jobApplied) {
		c.jobApplied = true;
		c.view = cosave::MakeView(c.file, c.job->dlls, c.job->found, c.job->searched);
		if (!c.tickedInit && c.view.plausible()) {
			c.tickedInit = true;
			for (const auto& r : c.view.rows) {
				if (r.status == cosave::Status::kNotInstalled) {
					c.ticked.insert(r.uid);
				}
			}
		}
	} else if (!c.jobApplied && c.view.rows.empty()) {
		c.view = cosave::MakeView(c.file, {}, {}, 0);  // names and sizes until the search is done
	}
	// PapyrusUtil objects on forms of the plugins the load order no longer has (the "missing mods" list)
	if (c.papu.present && c.gonePlugins.empty() && !_missing.empty()) {
		for (const auto& m : _missing) {
			c.gonePlugins.insert(Lower(m.plugin));
		}
		for (const auto& m : c.papu.mods) {
			if (c.gonePlugins.contains(Lower(m.plugin))) {
				c.goneObjects += m.objects;
				c.goneBytes += m.bytes;
			}
		}
	}
	return c;
}

void App::cosaveClean(const std::set<std::uint32_t>& a_uids, bool a_papu)
{
	auto& c = cosaveView();
	if (!c.ok || !_save) {
		return;
	}
	cosave::CleanOptions opt;
	opt.removeUids = a_uids;
	if (a_papu) {
		opt.gonePlugins = c.gonePlugins;
	}
	try {
		cosave::CleanResult r;
		auto                out = cosave::CleanBytes(_save->cosave, opt, &r);
		if (out == _save->cosave) {
			_status = tr("Nothing to remove from the co-save.");
			return;
		}
		_save->cosave = std::move(out);
		if (startupLog) {
			for (const auto& l : r.lines) {
				startupLog("co-save: " + l);
			}
		}
		markEdited(Fmt(tr("Co-save: removed the data of %zu plugin(s) and %zu PapyrusUtil object(s), %s -> %s"), r.plugins, r.papuObjects,
			cosave::SizeText(r.before).c_str(), cosave::SizeText(r.after).c_str()));
	} catch (const std::exception& e) {
		_error = std::string(tr("The co-save could not be cleaned (nothing was changed):\n\n")) + e.what();
	}
}

void App::cosaveCheck(std::vector<CheckItem>& a_items)
{
	if (!_save || _save->cosave.empty()) {
		return;
	}
	auto& c = cosaveView();
	if (!c.ok) {
		CheckItem it;
		it.level = Risk::kCaution;
		it.title = Fmt(tr("The %s co-save could not be read"), _save->cosaveExt());
		it.what = c.error;
		a_items.push_back(std::move(it));
		return;
	}
	if (!c.view.plausible() || !c.view.notInstalledCount) {
		return;
	}
	std::set<std::uint32_t> gone;
	std::string             names;
	for (const auto& r : c.view.rows) {
		if (r.status == cosave::Status::kNotInstalled) {
			gone.insert(r.uid);
			names += (names.empty() ? "" : ", ") + r.name;
		}
	}
	CheckItem it;
	it.level = Risk::kInfo;
	it.title = Fmt(tr("The co-save holds %s from %zu plugin(s) no longer installed"), cosave::SizeText(c.view.notInstalledBytes).c_str(),
		c.view.notInstalledCount);
	it.what = Fmt(tr("%s keeps data for script extender plugins whose DLL is no longer in your mods (or is in a disabled mod): %s. "
					 "The script extender skips it when the save loads, so it only takes room."),
		c.file.xseName().c_str(), names.c_str());
	if (!gone.empty()) {
		CheckOption o;
		o.label = tr("Remove their data from the co-save");
		o.risk = Risk::kCaution;
		o.why = tr("Only the data of plugins that are not installed. If you install one of them again, it starts fresh on this save. "
				   "File > Save As writes the new save with the cleaned co-save next to it.");
		o.act = [this, gone] { cosaveClean(gone, false); };
		it.options.push_back(std::move(o));
	}
	CheckOption v;
	v.label = tr("Show the co-save");
	v.risk = Risk::kSafe;
	v.why = tr("Only shows information.");
	v.act = [this] {
		_showCosave = true;
		_showCheck = false;
	};
	it.options.push_back(std::move(v));
	a_items.push_back(std::move(it));
}

void App::drawCosave()
{
	if (!_save) {
		_showCosave = false;
		return;
	}
	prepareWindow("Co-save", ImVec2(0.75f, 0.8f));
	if (!ImGui::Begin(trId("Co-save"), &_showCosave, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Co-save");
	auto& c = cosaveView();
	const auto coName = platform::ToUtf8(std::filesystem::path(_path).replace_extension(_save->cosaveExt()).filename());
	if (_save->cosave.empty()) {
		ImGui::TextWrapped(tr("This save has no %s file next to it (%s): nothing to show."), _save->cosaveExt(), coName.c_str());
		ImGui::End();
		return;
	}
	if (!c.ok) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
		ImGui::TextWrapped(tr("%s could not be read: %s"), coName.c_str(), c.error.c_str());
		ImGui::PopStyleColor();
		ImGui::End();
		return;
	}
	const auto& f = c.file;
	ImGui::Text(tr("%s co-save: %s, %zu plugin(s)"), f.xseName().c_str(), cosave::SizeText(_save->cosave.size()).c_str(), f.plugins.size());
	ImGui::TextWrapped(tr("%s, written by %s %s for the game %s"), coName.c_str(), f.xseName().c_str(), cosave::PackedVersion(f.xseVersion).c_str(),
		cosave::PackedVersion(f.runtimeVersion).c_str());
	for (const auto& n : f.notes) {
		ImGui::TextDisabled("%s", n.c_str());
	}
	if (!c.jobApplied) {
		ImGui::TextDisabled("%s", scanning() ? tr("Reading your mods first; then their DLLs are searched to tell which plugins are installed.")
		                                     : tr("Searching your mods' DLLs for the plugins' ids..."));
	} else if (!c.view.searched) {
		ImGui::TextDisabled("%s", tr("No DLLs found to search (set your mods folder or game folder in Options): installed or not cannot be told."));
	} else if (!c.view.plausible()) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Caution());
		ImGui::TextWrapped(tr("%zu DLL(s) searched and none of these plugins found: that is probably not the folder your game loads from (check the "
							  "mods folder in Options). Nothing is marked as not installed."),
			c.view.dllsSearched);
		ImGui::PopStyleColor();
	} else {
		ImGui::TextDisabled(tr("%zu DLL(s) of your mods and the game's Data folder searched for each plugin's id."), c.view.dllsSearched);
	}
	ImGui::TextWrapped("%s", tr("Each SKSE / F4SE plugin that keeps data in the save has its own block here, under a unique id it chose. A block "
							"whose plugin is not installed is skipped when the save loads. Removing a block deletes what that plugin saved "
							"for this character; if you install the plugin again later, it starts fresh on this save, as on a new game. The "
							"script extender's own block (id 0) is never removed."));
	ImGui::Separator();

	// the plugins, biggest first
	std::vector<const cosave::PluginRow*> rows;
	for (const auto& r : c.view.rows) {
		rows.push_back(&r);
	}
	std::stable_sort(rows.begin(), rows.end(), [](auto* a, auto* b) { return a->bytes > b->bytes; });
	const float tableH = ImGui::GetContentRegionAvail().y * 0.5f;
	if (ImGui::BeginTable("cosave", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
			ImVec2(0, tableH))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Remove"), ImGuiTableColumnFlags_WidthFixed, 60);
		ImGui::TableSetupColumn(tr("Plugin"), ImGuiTableColumnFlags_WidthStretch, 3);
		ImGui::TableSetupColumn(tr("Id"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
		ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 90);
		ImGui::TableSetupColumn(tr("Chunks"), ImGuiTableColumnFlags_WidthFixed, 60);
		ImGui::TableSetupColumn(tr("Installed"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
		ImGui::TableHeadersRow();
		int id = 0;
		for (const auto* r : rows) {
			ImGui::PushID(id++);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			bool tick = c.ticked.contains(r->uid);
			ImGui::BeginDisabled(r->uid == 0);
			if (ImGui::Checkbox("##t", &tick)) {
				tick ? (void)c.ticked.insert(r->uid) : (void)c.ticked.erase(r->uid);
			}
			ImGui::EndDisabled();
			ImGui::TableNextColumn();
			if (ImGui::Selectable(r->name.c_str(), c.haveSelected && c.selected == r->uid, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
				c.selected = r->uid;
				c.haveSelected = true;
			}
			if (!r->source.empty()) {
				tips::Tip((tr("Named from: ") + r->source).c_str());
			}
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(cosave::IdText(r->uid).c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(cosave::SizeText(r->bytes).c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%zu", r->chunks);
			ImGui::TableNextColumn();
			const auto col = r->status == cosave::Status::kNotInstalled ? theme::Caution()
			               : r->status == cosave::Status::kDisabled     ? theme::Caution()
			               : r->status == cosave::Status::kInstalled    ? theme::Good()
			                                                            : theme::Dim();
			ImGui::PushStyleColor(ImGuiCol_Text, col);
			std::string where = StatusWord(r->status);
			if (!r->dlls.empty()) {
				where += ": " + r->dlls.front() + (r->dlls.size() > 1 ? Fmt(" (+%zu)", r->dlls.size() - 1) : "");
			}
			ImGui::TextUnformatted(where.c_str());
			ImGui::PopStyleColor();
			if (r->dlls.size() > 1) {
				std::string all;
				for (const auto& d : r->dlls) {
					all += d + "\n";
				}
				tips::Tip(all.c_str());
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	if (c.view.searched && c.view.notInstalledCount) {
		ImGui::TextWrapped(tr("%s from %zu plugin(s) no longer installed or disabled."), cosave::SizeText(c.view.notInstalledBytes).c_str(), c.view.notInstalledCount);
	}

	// the remove button
	std::size_t tickedBytes = 0;
	for (auto u : c.ticked) {
		if (const auto* p = f.find(u)) {
			tickedBytes += p->fileSize();
		}
	}
	if (c.papu.present && c.goneObjects) {
		ImGui::Checkbox(Fmt(tr("Also remove PapyrusUtil data stored on forms of the %zu plugin(s) no longer in your load order (%zu object(s), %s)"),
							c.gonePlugins.size(), c.goneObjects, cosave::SizeText(c.goneBytes).c_str())
							.c_str(),
			&c.papuGone);
		tips::Tip(tr("PapyrusUtil itself drops these when the save loads (the forms no longer exist), so this only makes the file smaller."));
	}
	const bool papuToo = c.papu.present && c.goneObjects && c.papuGone;
	ImGui::BeginDisabled(c.ticked.empty() && !papuToo);
	if (ImGui::Button(Fmt(tr("Remove the ticked data (%zu plugin(s), %s)"), c.ticked.size(), cosave::SizeText(tickedBytes).c_str()).c_str())) {
		const auto uids = c.ticked;
		confirm(tr("Remove co-save data"),
			Fmt(tr("Remove the data of %zu plugin(s) from the co-save%s? If you install one of them again, it starts fresh on this save. "
				   "Nothing is written until File > Save As, which puts the new co-save next to the new save; the original files stay as they are."),
				uids.size(), papuToo ? tr(" and the PapyrusUtil data on removed plugins' forms") : ""),
			[this, uids, papuToo] { cosaveClean(uids, papuToo); });
	}
	ImGui::EndDisabled();
	tips::Tip(tr("Takes the ticked plugins' blocks out of the co-save in memory. File > Save As writes the new save with its co-save."));
	ImGui::SameLine();
	if (ImGui::Button(tr("Save As..."))) {
		startSaveAsDialog();
	}
	tips::Tip(tr("Write the save and its co-save to a new file (the save you opened is not changed)."));
	ImGui::Separator();

	ImGui::BeginChild("##cosavedetail");
	// the selected plugin's chunks
	if (c.haveSelected) {
		if (const auto* p = f.find(c.selected)) {
			ImGui::TextUnformatted(Fmt(tr("Chunks of %s"), cosave::IdText(p->uid).c_str()).c_str());
			if (!p->regular) {
				ImGui::TextDisabled("%s", tr("Its chunks do not fill its block exactly; it is kept as it is."));
			} else {
				std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> byType;
				for (const auto& ch : p->chunks) {
					auto& t = byType[ch.type];
					++t.first;
					t.second += 12 + ch.data.size();
				}
				for (const auto& [t, n] : byType) {
					const auto label = cosave::ChunkLabel(p->uid, t, f.fo4);
					ImGui::BulletText("%s  x%zu  %s%s%s", cosave::FourCC(t).c_str(), n.first, cosave::SizeText(n.second).c_str(), label.empty() ? "" : "  ",
						label.c_str());
				}
			}
			ImGui::Separator();
		}
	}
	// PapyrusUtil
	if (c.papu.present && ImGui::CollapsingHeader(tr("PapyrusUtil (StorageUtil) data"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("%s", tr("Values mods store with StorageUtil on forms (an NPC, an item) or globally. A mod that keeps adding keys is "
								"the usual reason this grows. By the plugin of the form the data is on:"));
		if (ImGui::BeginTable("papumods", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
			ImGui::TableSetupColumn(tr("Plugin"));
			ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 90);
			ImGui::TableSetupColumn(tr("Objects"), ImGuiTableColumnFlags_WidthFixed, 70);
			ImGui::TableSetupColumn(tr("Keys"), ImGuiTableColumnFlags_WidthFixed, 70);
			ImGui::TableHeadersRow();
			for (std::size_t i = 0; i < c.papu.mods.size() && i < 40; ++i) {
				const auto& m = c.papu.mods[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				const bool gone = c.gonePlugins.contains(Lower(m.plugin));
				if (gone) {
					ImGui::PushStyleColor(ImGuiCol_Text, theme::Caution());
				}
				ImGui::TextUnformatted((m.plugin + (gone ? tr("  (not in the load order)") : "")).c_str());
				if (gone) {
					ImGui::PopStyleColor();
				}
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(cosave::SizeText(m.bytes).c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%zu", m.objects);
				ImGui::TableNextColumn();
				ImGui::Text("%zu", m.keys);
			}
			ImGui::EndTable();
		}
		ImGui::TextUnformatted(tr("Biggest keys (the key names usually tell which mod):"));
		if (ImGui::BeginTable("papukeys", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
			ImGui::TableSetupColumn(tr("Key"));
			ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 90);
			ImGui::TableSetupColumn(tr("Objects"), ImGuiTableColumnFlags_WidthFixed, 70);
			ImGui::TableSetupColumn(tr("Values"), ImGuiTableColumnFlags_WidthFixed, 70);
			ImGui::TableHeadersRow();
			for (std::size_t i = 0; i < c.papu.keys.size() && i < 30; ++i) {
				const auto& k = c.papu.keys[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(k.name.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(cosave::SizeText(k.bytes).c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%zu", k.objects);
				ImGui::TableNextColumn();
				ImGui::Text("%zu", k.values);
			}
			ImGui::EndTable();
		}
		ImGui::TextUnformatted(tr("Biggest objects:"));
		if (ImGui::BeginTable("papuobj", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
			ImGui::TableSetupColumn(tr("Form"));
			ImGui::TableSetupColumn(tr("Plugin"));
			ImGui::TableSetupColumn(tr("Kind"));
			ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 90);
			ImGui::TableSetupColumn(tr("Keys"), ImGuiTableColumnFlags_WidthFixed, 70);
			ImGui::TableHeadersRow();
			for (std::size_t i = 0; i < c.papu.objects.size() && i < 30; ++i) {
				const auto& o = c.papu.objects[i];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(o.key == 0 ? tr("(global)") : Fmt("%08X", static_cast<std::uint32_t>(o.key)).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(o.plugin.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(cosave::ChunkLabel(cosave::papu::kUid, o.record, false).c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(cosave::SizeText(o.bytes).c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%zu", o.keys);
			}
			ImGui::EndTable();
		}
	}
	if (const auto* j = f.fo4 ? nullptr : f.find(cosave::jc::kUid)) {
		const auto& b = c.jc;
		if (!b.ok) {
			ImGui::TextWrapped(tr("JContainers: %s%s%s. Its object tree could not be broken down: %s"), cosave::SizeText(j->fileSize()).c_str(),
				c.jcHeader.empty() ? "" : ", ", c.jcHeader.c_str(), b.error.empty() ? "?" : b.error.c_str());
		} else if (ImGui::CollapsingHeader(Fmt(tr("JContainers data: %s, %zu object(s)###jcdata"), cosave::SizeText(j->fileSize()).c_str(), b.objects.size()).c_str(),
					   ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::TextWrapped(tr("Arrays and maps that mods keep with JContainers (JArray, JMap, JFormMap, JIntMap), as this co-save holds them: "
								  "%zu map(s), %zu array(s), %zu form map(s), %zu integer map(s); %zu value(s): %zu text, %zu number(s), %zu form(s), "
								  "%zu link(s) to other objects, %zu empty. %zu form(s) watched, %zu object(s) waiting to be freed%s."),
				b.maps, b.arrays, b.formMaps, b.intMaps, b.values, b.strings, b.numbers, b.forms, b.objectRefs, b.empty, b.watchedForms, b.queue,
				b.domains.empty() ? "" : Fmt(tr(", %zu separate domain(s)"), b.domains.size()).c_str());
			if (!b.tags.empty()) {
				std::string t;
				for (std::size_t i = 0; i < b.tags.size() && i < 12; ++i) {
					t += (t.empty() ? "" : ", ") + b.tags[i].first + Fmt(" (%zu)", b.tags[i].second);
				}
				ImGui::TextWrapped(tr("Tags (mods usually tag their objects with their name): %s"), t.c_str());
			}
			ImGui::TextUnformatted(tr("Biggest objects (the keys usually tell which mod):"));
			if (ImGui::BeginTable("jcobj", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
				ImGui::TableSetupColumn(tr("Kind"), ImGuiTableColumnFlags_WidthFixed, 100);
				ImGui::TableSetupColumn(tr("Id / tag"), ImGuiTableColumnFlags_WidthFixed, 140);
				ImGui::TableSetupColumn(tr("Entries"), ImGuiTableColumnFlags_WidthFixed, 70);
				ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 90);
				ImGui::TableSetupColumn(tr("Keys"));
				ImGui::TableHeadersRow();
				for (std::size_t i = 0; i < b.objects.size() && i < 30; ++i) {
					const auto& o = b.objects[i];
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(o.type.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted((std::to_string(o.id) + (o.tag.empty() ? "" : " " + o.tag)).c_str());
					ImGui::TableNextColumn();
					ImGui::Text("%zu", o.entries);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(cosave::SizeText(o.bytes).c_str());
					ImGui::TableNextColumn();
					std::string keys;
					for (const auto& k : o.keys) {
						keys += (keys.empty() ? "" : ", ") + k;
					}
					ImGui::TextUnformatted(keys.c_str());
				}
				ImGui::EndTable();
			}
			ImGui::TextWrapped("%s", tr("Read only: JContainers' data is not edited here (its objects point at each other; a mod's own "
										"menu or JContainers itself clears them)."));
		}
	}
	ImGui::EndChild();
	ImGui::End();
}
