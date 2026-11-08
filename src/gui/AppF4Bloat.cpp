// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Save bloat by mod (what each plugin accounts for in the save, the biggest causes with links to the tools that clean
// them, growth since an older save of the same character) and the load order check (full and light plugins against
// the game's limits, light plugins numbering forms out of range, missing or late masters, header versions the game
// does not load). Fallout 4 first; Skyrim works the same way.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/Extras.h"
#include "core/LoadOrder.h"
#include "core/LoadOrderCheck.h"
#include "core/LoadOrderFix.h"
#include "core/MergedPatch.h"
#include "core/SaveBloat.h"
#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <optional>
#include <thread>
#include <utility>

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

	void Colored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextWrapped("%s", s.c_str());
		ImGui::PopStyleColor();
	}

	std::filesystem::path HistoryFile() { return platform::SettingsFile().parent_path() / "resaverpp-history.txt"; }

	// a save read again from its file on a worker thread, and its report (the open save is never touched there)
	struct OldJob
	{
		std::atomic<bool>              done{ false };
		std::filesystem::path          path;
		std::string                    error, label;
		std::unique_ptr<bloat::Report> report;
		double                         seconds = 0;
	};

	struct LoJob
	{
		std::atomic<bool>     done{ false };
		std::filesystem::path pluginsTxt;
		locheck::Result       result;
		std::size_t           plugins = 0;
	};
}

namespace
{
	std::shared_ptr<OldJob> StartOld(const std::filesystem::path& a_file, std::shared_ptr<ScriptSources> a_sources);
}

struct App::BloatState
{
	// the open save's report (read again after an edit): from its file on a worker thread while it is unedited, else here
	std::shared_ptr<OldJob>        self;
	const SaveFile*                save = nullptr;
	std::uint64_t                  gen = UINT64_MAX;
	std::unique_ptr<bloat::Report> report;
	int                            waitFrames = 0;  // one frame with "reading..." on screen first
	double                         seconds = 0;
	// the older save it is compared with
	std::shared_ptr<OldJob>        old;
	const SaveFile*                oldFor = nullptr;  // the save the automatic choice was made for
	std::vector<bloat::Growth>     growth;
	bool                           growthReady = false;
	// load order check
	std::shared_ptr<LoJob>         lo;
	std::filesystem::path          loFile;   // the plugins.txt the last check read
	std::filesystem::file_time_type loTime{};
	bool                           loAuto = false;  // started by the Save check
	char                           filter[128] = "";
	bool                           loOnlyProblems = false;
	// Fix the order: the plan shown for confirmation, then what was written
	bool                           loFixAsk = false;  // open the confirmation (the button, or the Save check)
	bool                           loFixTestWrite = false;  // test hook: press "Fix the order" in the confirmation
	std::optional<locheck::FixPlan> loPlan;
	lofix::Targets                 loTargets;
	std::vector<std::string>       loOldOrder;
	bool                           loFixDone = false, loFixOk = false;
	std::string                    loFixMessage;
	std::vector<std::filesystem::path> loFixBackups;
};

App::BloatState& App::bloatState()
{
	if (!_bloatState) {
		_bloatState = std::make_shared<BloatState>();
	}
	return *_bloatState;
}

std::shared_ptr<ScriptSources> App::bloatSources()
{
	// a copy for the worker threads (the scan may be replaced while they run)
	return _sources.scanned() && !scanning() ? std::make_shared<ScriptSources>(_sources) : nullptr;
}

void App::bloatReport(bool a_allowHere)
{
	auto& st = bloatState();
	if (st.save != _save.get() || st.gen != _editGen) {
		if (st.save != _save.get()) {
			st.old.reset();
			st.oldFor = nullptr;
			st.growth.clear();
		}
		st.save = _save.get();
		st.gen = _editGen;
		st.report.reset();
		st.self.reset();
		st.growthReady = false;
		st.waitFrames = 1;
		if (_save && !_dirty && !_path.empty()) {
			st.self = StartOld(_path, bloatSources());  // the file is what is open: read it on a thread
		}
	}
	if (st.report || !_save) {
		return;
	}
	if (st.self) {
		if (st.self->done) {
			if (st.self->report) {
				st.report = std::move(st.self->report);
				st.seconds = st.self->seconds;
			} else {
				st.self.reset();  // could not be read again from the file: read the open save here
			}
		}
		if (st.self) {
			return;
		}
	}
	if (!a_allowHere || st.waitFrames-- > 0) {
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	bloat::Options o;
	o.sources = _sources.scanned() ? &_sources : nullptr;
	st.report = std::make_unique<bloat::Report>(bloat::Build(*_save, o));
	st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

namespace
{
	// the older save to compare with: the newest save of the same character, older than this one, that the save history
	// notes and that is still in this save's folder
	std::optional<std::filesystem::path> OlderFromHistory(const std::vector<extras::SaveStat>& a_history, const SaveFile& a_save,
		const std::filesystem::path& a_path)
	{
		std::error_code ec;
		const auto      nowTime = std::filesystem::last_write_time(a_path, ec);
		std::optional<std::filesystem::path> best;
		std::int64_t                         bestTime = 0;
		for (const auto& s : a_history) {
			if (s.character != a_save.header.playerName) {
				continue;
			}
			const auto f = a_path.parent_path() / platform::FromUtf8(s.file);
			if (f == a_path || Lower(platform::ToUtf8(f.filename())) == Lower(platform::ToUtf8(a_path.filename()))) {
				continue;
			}
			std::error_code e2;
			if (!std::filesystem::exists(f, e2)) {
				continue;
			}
			const auto t = std::filesystem::last_write_time(f, e2);
			if (e2 || (!ec && t >= nowTime)) {
				continue;
			}
			if (!best || s.modified > bestTime) {
				best = f;
				bestTime = s.modified;
			}
		}
		return best;
	}

	std::shared_ptr<OldJob> StartOld(const std::filesystem::path& a_file, std::shared_ptr<ScriptSources> a_sources)
	{
		auto job = std::make_shared<OldJob>();
		job->path = a_file;
		std::thread([job, a_sources] {
			try {
				const auto t0 = std::chrono::steady_clock::now();
				const auto s = SaveFile::load(job->path);
				job->label = Fmt("%s (save %u, level %u)", platform::ToUtf8(job->path.filename()).c_str(), s->header.saveNumber, s->header.playerLevel);
				bloat::Options o;
				o.sources = a_sources.get();
				job->report = std::make_unique<bloat::Report>(bloat::Build(*s, o));
				job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			} catch (const std::exception& e) {
				job->error = e.what();
			}
			job->done = true;
		}).detach();
		return job;
	}
}

void App::drawBloat()
{
	prepareWindow("Save bloat by mod", ImVec2(0.85f, 0.85f));
	if (!ImGui::Begin(trId("Save bloat by mod"), &_showBloat, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Save bloat by mod");
	if (!_save) {
		ImGui::TextDisabled("%s", tr("Open a save first."));
		ImGui::End();
		return;
	}
	auto& st = bloatState();
	bloatReport(true);
	if (!st.report) {
		ImGui::TextDisabled("%s", tr("Reading what each mod has in this save..."));
		ImGui::End();
		return;
	}
	const auto& rep = *st.report;

	// the older save: chosen from the save history once per save
	if (st.oldFor != _save.get()) {
		st.oldFor = _save.get();
		if (!_historyLoaded) {
			_history = extras::LoadHistory(HistoryFile());
			_historyLoaded = true;
		}
		if (const char* t = std::getenv("RESAVERPP_TEST_BLOAT_OLD")) {
			st.old = StartOld(t, bloatSources());
		} else if (auto f = OlderFromHistory(_history, *_save, _path)) {
			st.old = StartOld(*f, bloatSources());
		}
	}
	if (st.old && st.old->done && st.old->report && !st.growthReady) {
		st.growth = bloat::Compare(*st.old->report, rep);
		st.growthReady = true;
	}

	ImGui::TextWrapped("%s", tr("What each mod accounts for in this save: its scripts (instances, running threads, arrays), the changed forms the "
	                   "game remembers for it, the objects created while playing from its items or NPCs, and its items in inventories. "
	                   "Sizes are the bytes the game reads (uncompressed)."));
	ImGui::TextDisabled(tr("Save body %s: Papyrus %s, change forms %s. Shared by all mods (strings, script definitions, queued calls): %s. Read in %.1f s."),
		bloat::Bytes(rep.bodyBytes).c_str(), bloat::Bytes(rep.papyrusBytes).c_str(), bloat::Bytes(rep.changeFormBytes).c_str(),
		bloat::Bytes(rep.papyrusOther).c_str(), st.seconds);
	if (rep.decodeFailures) {
		ImGui::TextDisabled(tr("%zu object(s) could not be read; their inventories are counted with the object itself."), rep.decodeFailures);
	}

	const auto lo = currentLoadOrder();
	auto inLoadOrder = [&](const std::string& a_plugin) { return !lo.known || lo.plugins.contains(Lower(a_plugin)); };

	auto toolButton = [&](const bloat::Cause& c, int a_id) {
		struct Link
		{
			const char* label;
			Sc          sc;
		};
		std::vector<Link> links;
		switch (c.tool) {
		case bloat::Tool::kModsView: links.push_back({ tr("Mods view"), Sc::kMods }); break;
		case bloat::Tool::kPurgeScript: links.push_back({ tr("Purge this script..."), Sc::kPurge }); break;
		case bloat::Tool::kOrphans: links.push_back({ tr("Missing spawned objects"), Sc::kOrphans }); break;
		case bloat::Tool::kUnattached: links.push_back({ tr("Remove unattached instances..."), Sc::kRemoveUnattached }); break;
		case bloat::Tool::kArrays: links.push_back({ tr("Analyze arrays"), Sc::kArrays }); break;
		case bloat::Tool::kMissingMods: links.push_back({ tr("Mods no longer installed"), Sc::kMissingMods }); break;
		case bloat::Tool::kLeftovers: links.push_back({ tr("Leftover items"), Sc::kLeftovers }); break;
		case bloat::Tool::kFindNpcs:
			if (!fo4()) {
				links.push_back({ tr("Find NPCs"), Sc::kActors });
			}
			break;
		case bloat::Tool::kHavok: links.push_back({ tr("Reset Havok..."), Sc::kHavok }); break;
		case bloat::Tool::kCompare: links.push_back({ tr("Compare to another save..."), Sc::kCompare }); break;
		default: break;
		}
		if (c.what == "instances" && c.tool == bloat::Tool::kModsView && !c.game && !c.engine) {
			links.push_back({ tr("Purge this script..."), Sc::kPurge });
		}
		const bool realPlugin = !c.mod.empty() && c.mod[0] != '(' && c.mod[0] != '[';
		if (realPlugin && !inLoadOrder(c.mod) && (c.what == "changeforms" || c.what == "instances" || c.what == "created")) {
			links.push_back({ tr("Mods no longer installed"), Sc::kMissingMods });
		}
		for (std::size_t i = 0; i < links.size(); ++i) {
			ImGui::PushID(a_id * 8 + static_cast<int>(i));
			if (i) {
				ImGui::SameLine();
			}
			if (ImGui::SmallButton(links[i].label)) {
				shortcut(links[i].sc);
				if (links[i].sc == Sc::kPurge) {
					std::snprintf(_purgeName, sizeof(_purgeName), "%s", c.detail.c_str());
				}
			}
			ImGui::PopID();
		}
	};

	// ---- the biggest causes: the mods' first, then the game's own (normal in a long game)
	ImGui::Spacing();
	auto causes = [&](bool a_game, int a_base) {
		int id = a_base, shown = 0;
		for (const auto& c : rep.causes) {
			if (c.game != a_game) {
				continue;
			}
			++shown;
			ImGui::PushID(id);
			const bool gone = !inLoadOrder(c.mod) && !c.mod.empty() && c.mod[0] != '(' && c.mod[0] != '[';
			ImGui::Bullet();
			ImGui::SameLine();
			Colored(gone ? theme::Warn() : ImGui::GetColorU32(ImGuiCol_Text), c.text + (gone ? tr("  (not in your load order)") : ""));
			ImGui::Indent();
			ImGui::PushStyleColor(ImGuiCol_Text, theme::Dim());
			ImGui::TextWrapped("%s", c.advice.c_str());
			ImGui::PopStyleColor();
			toolButton(c, id);
			ImGui::Unindent();
			ImGui::PopID();
			++id;
		}
		return shown;
	};
	if (ImGui::CollapsingHeader(trId("Biggest causes from mods"), ImGuiTreeNodeFlags_DefaultOpen)) {
		if (!causes(false, 0)) {
			ImGui::TextDisabled("%s", tr("None: the biggest parts of this save are the game's own (below)."));
		}
	}
	if (ImGui::CollapsingHeader(trId("The game's own (base game, DLC, Creation Club)"))) {
		ImGui::TextDisabled("%s", tr("These grow with every place visited and every quest done; they are normal in a long game."));
		causes(true, 1000);
	}

	// ---- growth since an older save
	if (ImGui::CollapsingHeader(trId("Growth since an older save"), ImGuiTreeNodeFlags_DefaultOpen)) {
		if (!st.old) {
			ImGui::TextWrapped("%s", tr("The save history has no older save of this character in this folder. Choose one to see which mods grew."));
		} else if (!st.old->done) {
			ImGui::TextDisabled(tr("Reading %s..."), platform::ToUtf8(st.old->path.filename()).c_str());
		} else if (!st.old->error.empty()) {
			Colored(theme::Bad(), Fmt(tr("Could not read %s: %s"), platform::ToUtf8(st.old->path.filename()).c_str(), st.old->error.c_str()));
		} else {
			ImGui::TextWrapped(tr("Compared with %s: what each mod gained or lost since then."), st.old->label.c_str());
			if (st.growth.empty()) {
				ImGui::TextDisabled("%s", tr("No mod changed size."));
			} else if (ImGui::BeginTable("growth", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
						   ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 10))) {
				ImGui::TableSetupScrollFreeze(0, 1);
				for (const char* h : { tr("Mod"), tr("Change"), tr("Size then -> now"), tr("Script instances"), tr("Created objects"), tr("Threads") }) {
					ImGui::TableSetupColumn(h);
				}
				ImGui::TableHeadersRow();
				for (const auto& g : st.growth) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(text::ToUtf8(g.mod).c_str());
					ImGui::TableNextColumn();
					const auto d = g.delta();
					ImGui::PushStyleColor(ImGuiCol_Text, d > 0 ? theme::Warn() : theme::Good());
					ImGui::Text("%s%s", d >= 0 ? "+" : "-", bloat::Bytes(static_cast<std::uint64_t>(d >= 0 ? d : -d)).c_str());
					ImGui::PopStyleColor();
					ImGui::TableNextColumn();
					ImGui::Text("%s -> %s", bloat::Bytes(static_cast<std::uint64_t>(g.oldBytes)).c_str(), bloat::Bytes(static_cast<std::uint64_t>(g.newBytes)).c_str());
					ImGui::TableNextColumn();
					ImGui::Text("%lld -> %lld", static_cast<long long>(g.oldInstances), static_cast<long long>(g.newInstances));
					ImGui::TableNextColumn();
					ImGui::Text("%lld -> %lld", static_cast<long long>(g.oldCreated), static_cast<long long>(g.newCreated));
					ImGui::TableNextColumn();
					ImGui::Text("%lld -> %lld", static_cast<long long>(g.oldThreads), static_cast<long long>(g.newThreads));
				}
				ImGui::EndTable();
			}
		}
		if (ImGui::SmallButton(tr("Compare with another save..."))) {
			if (platform::HasNativeDialogs()) {
				if (auto f = platform::OpenSaveDialog(_path.parent_path())) {
					st.old = StartOld(*f, bloatSources());
					st.growthReady = false;
				}
			}
		}
	}

	// ---- every mod
	if (ImGui::CollapsingHeader(trId("Every mod"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
		ImGui::InputTextWithHint("##bloatfilter", tr("filter by mod"), st.filter, sizeof(st.filter));
		const auto filter = Lower(st.filter);
		if (ImGui::BeginTable("bloat", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
			ImGui::TableSetupScrollFreeze(1, 1);
			ImGui::TableSetupColumn(tr("Mod"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
			for (const char* h : { tr("Total"), tr("Script instances"), tr("Threads"), tr("Arrays / structs"), tr("Changed forms"), tr("Created objects"),
					 tr("Inventory entries") }) {
				ImGui::TableSetupColumn(h, ImGuiTableColumnFlags_WidthStretch, 1.4f);
			}
			ImGui::TableHeadersRow();
			const auto total = std::max<std::uint64_t>(1, rep.attributed + rep.papyrusOther);
			int        id = 0;
			for (const auto& m : rep.mods) {
				if (!filter.empty() && Lower(m.name).find(filter) == std::string::npos) {
					continue;
				}
				ImGui::PushID(id++);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				const bool gone = (m.kind == bloat::RowKind::kPlugin || m.kind == bloat::RowKind::kLight) && !inLoadOrder(m.name);
				if (gone) {
					ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
				}
				const bool open = ImGui::TreeNodeEx(text::ToUtf8(m.name).c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
				if (gone) {
					ImGui::PopStyleColor();
					tips::Tip(tr("Not in your current load order: what it left in the save stays until it is cleaned (Mods no longer installed)."));
				}
				ImGui::TableNextColumn();
				ImGui::Text("%s (%.1f%%)", bloat::Bytes(m.total()).c_str(), 100.0 * static_cast<double>(m.total()) / static_cast<double>(total));
				auto cell = [](std::size_t n, std::uint64_t b) {
					ImGui::TableNextColumn();
					if (n) {
						ImGui::Text("%s  %s", bloat::Num(n).c_str(), bloat::Bytes(b).c_str());
					} else {
						ImGui::TextDisabled("-");
					}
				};
				cell(m.instances.count, m.instances.bytes);
				cell(m.threads.count, m.threads.bytes);
				cell(m.arrays.count + m.structs.count, m.arrays.bytes + m.structs.bytes);
				cell(m.changeFormCount(), m.changeFormBytes());
				cell(m.createdCount(), m.createdBytes());
				cell(m.inventory.count, m.inventory.bytes);
				if (open) {
					auto list = [&](const char* a_title, const std::map<std::string, bloat::Count>& a_map) {
						if (a_map.empty()) {
							return;
						}
						std::vector<std::pair<std::string, bloat::Count>> v(a_map.begin(), a_map.end());
						std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TextDisabled("%s", a_title);
						ImGui::TableNextColumn();
						std::string line;
						for (std::size_t i = 0; i < v.size() && i < 12; ++i) {
							line += (i ? ",  " : "") + v[i].first + " " + bloat::Num(v[i].second.count) + " (" + bloat::Bytes(v[i].second.bytes) + ")";
						}
						if (v.size() > 12) {
							line += Fmt(tr(",  and %zu more"), v.size() - 12);
						}
						ImGui::TextWrapped("%s", text::ToUtf8(line).c_str());
					};
					list(tr("scripts"), m.scripts);
					list(tr("running"), m.threadScripts);
					list(tr("changed forms"), m.changeForms);
					list(tr("created"), m.created);
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
	}
	ImGui::End();
}

// ---- load order check ----------------------------------------------------------------------------------------------

namespace
{
	locheck::Game GameOf(int a_game, bool a_le) { return a_game == 2 ? locheck::Game::kFallout4 : a_le ? locheck::Game::kSkyrimLE : locheck::Game::kSkyrimSE; }

	bool HasLateMasters(const locheck::Result& a_r)
	{
		return std::any_of(a_r.findings.begin(), a_r.findings.end(), [](const locheck::Finding& f) { return !f.lateMaster.empty(); });
	}

	// what happens after the files are written, for the mod manager in use (MO2 / Vortex sources: LoadOrderFix.h)
	std::string AfterWriteNote(lofix::Manager a_manager)
	{
		switch (a_manager) {
		case lofix::Manager::kMo2:
			if (platform::RunningUnderMO2()) {
				return tr("ReSaver Plus was started from Mod Organizer: Mod Organizer reads the new order when ReSaver Plus closes (it refreshes after "
				          "a program it started ends). Close ReSaver Plus before you change anything in Mod Organizer or start the game. If you "
				          "pressed Unlock in Mod Organizer while ReSaver Plus was running, press Refresh (F5) in Mod Organizer after closing "
				          "ReSaver Plus.");
			}
			if (platform::Mo2ProcessRunning().value_or(false)) {
				return tr("Mod Organizer is open: press Refresh (F5) in it right after this, before you change anything there or close it. "
				          "Otherwise it writes its old order back (it saves its own list when something changes and when it closes).");
			}
			return tr("Mod Organizer reads the new order when it starts. (If it is open, press Refresh (F5) in it right after this, before "
			          "you change anything there or close it, or it writes its old order back.)");
		case lofix::Manager::kVortex:
			return tr("Vortex keeps its load order in this plugins.txt and reads it again when another program changes it (if Vortex is open, "
			          "it shows the new order within a second). When Vortex sorts the plugins with LOOT (automatic sorting), a later sort can "
			          "move plugins again; LOOT also keeps every master before the plugins that need it.");
		default:
			return tr("The game reads plugins.txt when it starts.");
		}
	}
}

void App::testFixLoadOrder(bool a_write)
{
	_showLoCheck = true;
	bloatState().loFixAsk = true;
	bloatState().loFixTestWrite = a_write;
}

void App::drawLoadOrderFix()
{
	auto& st = bloatState();
	const char* id = trId("Fix the load order");
	if (st.loFixAsk && st.lo && st.lo->done) {
		st.loFixAsk = false;
		const auto& r = st.lo->result;
		st.loPlan = locheck::PlanFix(r, lofix::EnabledLines(st.lo->pluginsTxt));
		const auto manager = _mo2 && !_profile.empty() ? lofix::Manager::kMo2 : _vortex ? lofix::Manager::kVortex : lofix::Manager::kNone;
		st.loTargets = lofix::TargetsFor(st.lo->pluginsTxt, manager);
		st.loOldOrder.clear();
		for (const auto& p : r.plugins) {
			st.loOldOrder.push_back(p.name);
		}
		st.loFixDone = false;
		ImGui::OpenPopup(id);
	}
	ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 30, 0), ImVec2(ImGui::GetFontSize() * 60, ImGui::GetIO().DisplaySize.y * 0.9f));
	if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		return;
	}
	const float wrap = ImGui::GetFontSize() * 45;
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
	if (!st.loPlan) {
		ImGui::CloseCurrentPopup();
	} else if (!st.loPlan->ok) {
		Colored(theme::Bad(), text::ToUtf8(st.loPlan->error));
		if (ImGui::Button(tr("Close"))) {
			ImGui::CloseCurrentPopup();
		}
	} else if (st.loPlan->moves.empty()) {
		Colored(theme::Good(), tr("No master loads after a plugin that needs it: nothing to fix."));
		if (ImGui::Button(tr("Close"))) {
			ImGui::CloseCurrentPopup();
		}
	} else if (!st.loFixDone) {
		const auto& plan = *st.loPlan;
		ImGui::TextWrapped(tr("These masters move up, each to just before the first plugin that needs it (and a master's own masters before "
		                      "it). Every other plugin keeps its order:"));
		std::string list;
		for (const auto& m : plan.moves) {
			list += text::ToUtf8(locheck::MoveText(m)) + "\n";
		}
		ImGui::BeginChild("moves", ImVec2(wrap, ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min<std::size_t>(plan.moves.size(), 12) + 1)),
			ImGuiChildFlags_Borders);
		for (const auto& m : plan.moves) {
			ImGui::BulletText("%s", text::ToUtf8(locheck::MoveText(m)).c_str());
		}
		ImGui::EndChild();
		ImGui::TextDisabled("%s", tr("#n: the plugin's position among the enabled plugins in plugins.txt, counting from 0."));
		ImGui::Spacing();
		ImGui::TextUnformatted(tr("Files that change:"));
		for (const auto& f : st.loTargets.files) {
			ImGui::BulletText("%s", platform::ToUtf8(f).c_str());
		}
		ImGui::TextDisabled("%s", tr("A copy of each is kept next to it first, as <name>.<date>-<time>.resaverpp.bak."));
		if (!st.loTargets.lockedOrder.empty()) {
			ImGui::TextDisabled(tr("Mod Organizer's locked positions (%s) are checked first."), platform::ToUtf8(st.loTargets.lockedOrder.filename()).c_str());
		}
		ImGui::Spacing();
		Colored(theme::Caution(), AfterWriteNote(st.loTargets.manager));
		ImGui::Spacing();
		if (ImGui::Button(tr("Fix the order")) || std::exchange(st.loFixTestWrite, false)) {
			const auto w = lofix::Write(st.loTargets, plan, st.loOldOrder, false);
			st.loFixDone = true;
			st.loFixOk = w.ok;
			st.loFixMessage = w.message;
			st.loFixBackups.clear();
			for (const auto& f : w.files) {
				if (!f.backup.empty()) {
					st.loFixBackups.push_back(f.backup);
				}
			}
			if (w.ok) {
				st.lo.reset();  // check again from the written files (the window starts the check when there is none)
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(tr("Copy the list"))) {
			ImGui::SetClipboardText(list.c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button(tr("Cancel"))) {
			ImGui::CloseCurrentPopup();
		}
	} else {
		if (st.loFixOk) {
			Colored(theme::Good(), tr("The load order was fixed."));
			Colored(theme::Caution(), AfterWriteNote(st.loTargets.manager));
		} else {
			Colored(theme::Bad(), tr("Nothing was changed."));
		}
		ImGui::TextWrapped("%s", text::ToUtf8(st.loFixMessage).c_str());
		if (!st.loFixBackups.empty()) {
			ImGui::TextUnformatted(tr("Backups:"));
			for (const auto& b : st.loFixBackups) {
				ImGui::BulletText("%s", platform::ToUtf8(b).c_str());
			}
			if (ImGui::Button(tr("Show the backups in their folder"))) {
				platform::ShowInFolder(st.loFixBackups.front());
			}
			ImGui::SameLine();
		}
		if (ImGui::Button(tr("Close"))) {
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::PopTextWrapPos();
	ImGui::EndPopup();
}

void App::drawLoadOrderCheck()
{
	prepareWindow("Load order check", ImVec2(0.8f, 0.8f));
	if (!ImGui::Begin(trId("Load order check"), &_showLoCheck, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Load order check");
	auto&      st = bloatState();
	const bool le = _save && _save->isLE();
	const std::filesystem::path pluginsTxt =
		_mo2 && !_profile.empty() ? _mo2->profilesDir / platform::FromUtf8(_profile) / "plugins.txt" : loadorder::GamePluginsTxt(_game, le);
	const auto game = GameOf(_game, le);
	const auto lim = locheck::LimitsFor(game);

	if (game == locheck::Game::kFallout4) {
		ImGui::TextWrapped(tr("Fallout 4 loads at most %zu full plugins (.esm and .esp without the light flag; load order slots 00 to FD) and %zu light "
		                   "plugins (.esl, or any plugin with the light flag: they share slot FE). A light plugin may only number its own forms "
		                   "up to FFF. A plugin made with a newer header version than %.2f is not loaded at all."),
			lim.maxFull, lim.maxLight, static_cast<double>(lim.maxHedr));
	} else if (game == locheck::Game::kSkyrimLE) {
		ImGui::TextWrapped(tr("Skyrim Legendary Edition loads at most %zu plugins and has no light plugins."), lim.maxFull);
	} else {
		ImGui::TextWrapped(tr("Skyrim Special Edition loads at most %zu full plugins and %zu light plugins (.esl or the light flag). A light plugin may "
		                   "only number its own forms up to FFF."),
			lim.maxFull, lim.maxLight);
	}
	ImGui::TextDisabled(tr("Load order: %s"), platform::ToUtf8(pluginsTxt).c_str());

	const bool noMods = _sourcesPath.empty() && !_sources.scanned() && gameFolder().empty();
	if (noMods) {
		Colored(theme::Warn(), tr("ReSaver Plus does not know where your mods are yet: set your mods folder (Mod Organizer) or game folder in "
		                       "Options > Settings, then come back here."));
		ImGui::End();
		return;
	}
	const bool busy = st.lo && !st.lo->done;
	ImGui::BeginDisabled(busy || scanning());
	if (ImGui::Button(st.lo ? tr("Check again") : tr("Check the load order")) || (!st.lo && !scanning())) {
		auto job = std::make_shared<LoJob>();
		job->pluginsTxt = pluginsTxt;
		auto src = std::make_shared<ScriptSources>(_sources);
		const auto gf = gameFolder();
		locheck::Options o;
		o.game = game;
		o.loadOrder = merge::ReadLoadOrder(pluginsTxt, gf, _game == 2 ? 2 : -1);
		job->plugins = o.loadOrder.size();
		std::thread([job, src, gf, o]() mutable {
			o.findPlugin = [src, gf](const std::string& a_name) -> std::optional<std::filesystem::path> {
				if (auto p = src->pluginPath(a_name)) {
					return p;
				}
				if (!gf.empty()) {
					std::error_code ec;
					if (const auto f = gf / "Data" / a_name; std::filesystem::exists(f, ec)) {
						return f;
					}
				}
				return std::nullopt;
			};
			job->result = locheck::Check(o);
			job->done = true;
		}).detach();
		st.lo = job;
		std::error_code ec;
		st.loFile = pluginsTxt;
		st.loTime = std::filesystem::last_write_time(pluginsTxt, ec);
	}
	ImGui::EndDisabled();
	if (scanning()) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", tr("waiting for the scan of your mods..."));
	}
	if (busy) {
		ImGui::SameLine();
		ImGui::TextDisabled(tr("reading %zu plugins..."), st.lo->plugins);
		drawLoadOrderFix();  // the result of Fix the order stays on screen while the check runs again
		ImGui::End();
		return;
	}
	if (!st.lo) {
		drawLoadOrderFix();
		ImGui::End();
		return;
	}
	const auto& r = st.lo->result;
	ImGui::Spacing();
	if (lim.light) {
		ImGui::Text(tr("Full plugins: %zu of %zu"), r.full, lim.maxFull);
		ImGui::SameLine(0, ImGui::GetFontSize() * 3);
		ImGui::Text(tr("Light plugins: %zu of %zu"), r.light, lim.maxLight);
	} else {
		ImGui::Text(tr("Plugins: %zu of %zu"), r.full + r.light, lim.maxFull);
	}
	ImGui::ProgressBar(static_cast<float>(r.full) / static_cast<float>(lim.maxFull), ImVec2(ImGui::GetFontSize() * 20, 0),
		Fmt("%zu / %zu", r.full, lim.maxFull).c_str());
	if (!r.hasErrors() && r.count(locheck::Severity::kWarning) == 0) {
		Colored(theme::Good(), tr("No problems found."));
	}
	for (const auto& f : r.findings) {
		if (f.severity == locheck::Severity::kInfo && f.plugin.empty()) {
			continue;  // the counts above
		}
		const auto col = f.severity == locheck::Severity::kError ? theme::Bad() : f.severity == locheck::Severity::kWarning ? theme::Caution() : theme::Dim();
		ImGui::Bullet();
		ImGui::SameLine();
		Colored(col, text::ToUtf8(f.text));
	}
	if (HasLateMasters(r)) {
		ImGui::Spacing();
		if (ImGui::Button(tr("Fix the order"))) {
			st.loFixAsk = true;
		}
		ImGui::SameLine();
		ImGui::TextDisabled("%s", tr("moves each late master to just before the first plugin that needs it; shows the moves first"));
	}
	drawLoadOrderFix();
	ImGui::Spacing();
	ImGui::Checkbox(tr("Only plugins with a problem"), &st.loOnlyProblems);
	if (ImGui::BeginTable("lo", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(tr("Slot"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.5f);
		ImGui::TableSetupColumn(tr("Plugin"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn(tr("Kind"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn(tr("Header"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4);
		ImGui::TableSetupColumn(tr("New forms"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn(tr("Masters"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableHeadersRow();
		for (const auto& p : r.plugins) {
			bool problem = false;
			for (const auto& f : r.findings) {
				problem = problem || (f.plugin == p.name && f.severity != locheck::Severity::kInfo);
			}
			if (st.loOnlyProblems && !problem) {
				continue;
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			if (p.slot < 0) {
				ImGui::TextDisabled("--");
			} else if (p.light) {
				ImGui::Text("FE %03X", p.slot);
			} else {
				ImGui::Text("%02X", p.slot);
			}
			ImGui::TableNextColumn();
			if (problem) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
			}
			ImGui::TextUnformatted(text::ToUtf8(p.name).c_str());
			if (problem) {
				ImGui::PopStyleColor();
			}
			ImGui::TableNextColumn();
			std::string kind = p.path.empty() ? tr("not found") : p.light ? (p.esm ? tr("light master") : tr("light")) : p.esm ? tr("master") : tr("plugin");
			if (p.eslFlag && !p.eslExt) {
				kind += tr(" (flag)");
			}
			ImGui::TextUnformatted(kind.c_str());
			ImGui::TableNextColumn();
			if (!p.path.empty()) {
				ImGui::Text("%.2f", static_cast<double>(p.hedr));
			}
			ImGui::TableNextColumn();
			if (p.light && p.newRecords) {
				ImGui::Text(tr("%zu (up to %03X)"), p.newRecords, p.maxObjectID);
			}
			ImGui::TableNextColumn();
			std::string ms;
			for (const auto& m : p.masters) {
				ms += (ms.empty() ? "" : ", ") + m;
			}
			ImGui::TextWrapped("%s", text::ToUtf8(ms).c_str());
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

// ---- Save check ----------------------------------------------------------------------------------------------------

void App::bloatCheckItems(std::vector<CheckItem>& a_items)
{
	if (!_save) {
		return;
	}
	auto& st = bloatState();

	// the load order: checked once in the background when the mods are known (again when plugins.txt changes)
	{
		const bool le = _save->isLE();
		const std::filesystem::path pluginsTxt =
			_mo2 && !_profile.empty() ? _mo2->profilesDir / platform::FromUtf8(_profile) / "plugins.txt" : loadorder::GamePluginsTxt(_game, le);
		std::error_code ec;
		const auto      t = std::filesystem::last_write_time(pluginsTxt, ec);
		const bool      known = !ec && (_sources.scanned() || !gameFolder().empty()) && !scanning();
		if (known && (!st.lo || (st.lo->done && (st.loFile != pluginsTxt || st.loTime != t)))) {
			auto job = std::make_shared<LoJob>();
			job->pluginsTxt = pluginsTxt;
			auto       src = std::make_shared<ScriptSources>(_sources);
			const auto gf = gameFolder();
			locheck::Options o;
			o.game = GameOf(_game, le);
			o.loadOrder = merge::ReadLoadOrder(pluginsTxt, gf, _game == 2 ? 2 : -1);
			job->plugins = o.loadOrder.size();
			std::thread([job, src, gf, o]() mutable {
				o.findPlugin = [src, gf](const std::string& a_name) -> std::optional<std::filesystem::path> {
					if (auto p = src->pluginPath(a_name)) {
						return p;
					}
					if (!gf.empty()) {
						std::error_code e2;
						if (const auto f = gf / "Data" / a_name; std::filesystem::exists(f, e2)) {
							return f;
						}
					}
					return std::nullopt;
				};
				job->result = locheck::Check(o);
				job->done = true;
			}).detach();
			st.lo = job;
			st.loFile = pluginsTxt;
			st.loTime = t;
			st.loAuto = true;
		}
		if (st.lo && st.lo->done) {
			const auto& r = st.lo->result;
			const auto  errors = r.count(locheck::Severity::kError), warnings = r.count(locheck::Severity::kWarning);
			if (errors || warnings) {
				CheckItem it;
				it.level = errors ? Risk::kRisky : Risk::kCaution;
				it.title = errors ? Fmt(tr("Your load order has %zu problem(s) that break plugins"), errors)
				                  : Fmt(tr("Your load order has %zu thing(s) to look at"), warnings);
				std::string list;
				std::size_t n = 0;
				for (const auto& f : r.findings) {
					if (f.severity == locheck::Severity::kInfo) {
						continue;
					}
					if (n++ == 5) {
						list += "\n  ...";
						break;
					}
					list += "\n  - " + text::ToUtf8(f.text);
				}
				it.what = Fmt(tr("Full plugins %zu of %zu, light plugins %zu of %zu."), r.full, r.limits.maxFull, r.light, r.limits.maxLight) + list;
				CheckOption o;
				o.label = tr("Show the load order check");
				o.risk = Risk::kSafe;
				o.why = tr("Only shows information.");
				o.act = [this] {
					_showLoCheck = true;
					_showCheck = false;
				};
				it.options.push_back(std::move(o));
				if (HasLateMasters(r)) {
					CheckOption fx;
					fx.label = tr("Fix the order");
					fx.risk = Risk::kCaution;
					fx.why = tr("Moves each master that loads after a plugin needing it to just before that plugin, in your mod manager's "
					            "plugins.txt / loadorder.txt. Shows the moves first and asks; the files are backed up next to themselves.");
					fx.act = [this] {
						_showLoCheck = true;
						_showCheck = false;
						bloatState().loFixAsk = true;
					};
					it.options.push_back(std::move(fx));
				}
				a_items.push_back(std::move(it));
			}
		}
	}

	// save bloat (read on a worker thread while the save is unedited): a mod that is gone from the load order but still
	// takes a big part of the save is worth a line
	bloatReport(false);
	if (st.report && st.save == _save.get() && st.gen == _editGen) {
		const auto& rep = *st.report;
		const auto  lo = currentLoadOrder();
		for (const auto& m : rep.mods) {
			if ((m.kind != bloat::RowKind::kPlugin && m.kind != bloat::RowKind::kLight) || !lo.known || lo.plugins.contains(Lower(m.name))) {
				continue;
			}
			if (m.total() < 256 * 1024) {
				continue;
			}
			CheckItem it;
			it.level = Risk::kCaution;
			it.title = Fmt(tr("%s is no longer in your load order but still takes %s of this save"), text::ToUtf8(m.name).c_str(), bloat::Bytes(m.total()).c_str());
			it.what = Fmt(tr("%s script instances, %s changed forms, %s created objects, %s inventory entries."), bloat::Num(m.instances.count).c_str(),
				bloat::Num(m.changeFormCount()).c_str(), bloat::Num(m.createdCount()).c_str(), bloat::Num(m.inventory.count).c_str());
			CheckOption o;
			o.label = tr("Mods no longer installed");
			o.risk = Risk::kCaution;
			o.why = tr("Shows what the mod left and cleans it after you confirm.");
			o.act = [this] {
				shortcut(Sc::kMissingMods);
				_showCheck = false;
			};
			it.options.push_back(std::move(o));
			a_items.push_back(std::move(it));
		}
	}
}
