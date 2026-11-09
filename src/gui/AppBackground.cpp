// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Work moved off the window's thread (SkyrimIstari, 8 Nov 2026: the window froze after opening a big save under Wine,
// "will not make it to check all the mods").
//
// - Opening a save: a worker reads the file and works out what the first frames needed (the save check report, the
//   Mods table, the enchanted items, the quests, the followers view, the MO2 profile match, the history numbers, the
//   picture and the log's description). The window then takes the results over (finishOpen).
// - After the mods scan (and after the plugin names), what depends on them (the Mods table, the missing mods, the
//   followers view) is worked out again on a worker from a fresh read of the same, unedited file, so no thread
//   reads the save the window may be editing. Edited saves, or a file changed on disk since, fall back to the window
//   working it out itself (as before).
// - Every background job is listed in the status bar with its progress (runningJobs / drawJobsBar).
#include "App.h"
#include "Watchdog.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "core/FollowersSky.h"
#include "core/Text.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
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

	std::string LowerB(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	// the game setup a save belongs to (App::setGame's number): Fallout 4, Skyrim VR (lists SkyrimVR.esm) or Skyrim
	int GameOfSave(const SaveFile& a_save)
	{
		if (a_save.isFO4()) {
			return 2;
		}
		for (const auto& p : a_save.plugins.full) {
			if (LowerB(p) == "skyrimvr.esm") {
				return 1;
			}
		}
		return 0;
	}
}

// what a worker worked out for one save, taken over by the window
struct App::Prepared
{
	std::unique_ptr<mods::Table>          mods;
	std::shared_ptr<ScriptSources>        modsSources;  // the scan the table was made with
	bool                                  hasCharged = false, hasQuests = false;
	std::vector<cf::ChargedItem>          charged;
	std::vector<quests::QuestState>       quests;
	std::shared_ptr<SkyFollowersView>     skyf;
	std::shared_ptr<PluginDB>             skyfDb;  // the names it was made with
	bool                                  hasMissing = false;
	MissingResult                         missing;
	std::shared_ptr<const ScriptSources>  missingSources;
};

struct App::OpenJob
{
	std::filesystem::path          path;  // as asked
	std::filesystem::path          abs;   // absolute
	// inputs, copied on the window's thread
	std::shared_ptr<ScriptSources> sources;
	std::shared_ptr<PluginDB>      db;
	int                            game = 0;
	std::optional<Mo2Instance>     mo2;
	std::string                    profile;
	std::filesystem::path          gameFolder;
	// results
	std::unique_ptr<SaveFile>      save;
	std::string                    error;
	std::unique_ptr<ops::Report>   report;
	extras::SaveStat               stat;
	std::vector<std::string>       describe;
	std::vector<std::uint8_t>      rgba;
	int                            w = 0, h = 0;
	Prepared                       prepared;
	ProfileMatch                   profileMatch;
	std::shared_ptr<const std::unordered_set<std::uint32_t>> wanted;  // for the plugin names (PrepareInput)
	std::uintmax_t                 size = 0;
	std::filesystem::file_time_type time{};
	double                         ms = 0;
	std::atomic<std::size_t>       step{ 0 };
	static constexpr std::size_t   kSteps = 3;
	std::atomic<bool>              done{ false }, cancel{ false };
	mutable std::mutex             m;
	std::string                    current;  // guarded by m
	void setCurrent(const char* a_what)
	{
		std::lock_guard l(m);
		current = a_what;
		++step;
	}
	std::string currentText() const
	{
		std::lock_guard l(m);
		return current;
	}
};

struct App::AnalysisJob
{
	std::filesystem::path          path;
	std::uintmax_t                 size = 0;
	std::filesystem::file_time_type time{};
	unsigned                       what = 0;
	std::uint64_t                  gen = 0;
	const SaveFile*                save = nullptr;  // only compared, never read
	std::shared_ptr<ScriptSources> sources;
	std::shared_ptr<PluginDB>      db;
	MissingInputs                  missingIn;
	Prepared                       out;
	std::string                    error;
	std::atomic<std::size_t>       step{ 0 }, steps{ 1 };
	std::atomic<bool>              done{ false }, cancel{ false };
	mutable std::mutex             m;
	std::string                    current;
	void setCurrent(const char* a_what)
	{
		std::lock_guard l(m);
		current = a_what;
		++step;
	}
	std::string currentText() const
	{
		std::lock_guard l(m);
		return current;
	}
};


// ---- opening -------------------------------------------------------------------------------------------------

void App::open(const std::filesystem::path& a_path)
{
	if (!_openAsync) {
		openNow(a_path);
		return;
	}
	if (_openJob) {
		_openJob->cancel = true;  // (its thread ends on its own; what it read is dropped)
	}
	auto job = std::make_shared<OpenJob>();
	job->path = a_path;
	job->sources = _sources;
	job->db = _db;
	job->game = _game;
	job->mo2 = _mo2;
	job->profile = _profile;
	job->gameFolder = gameFolder();
	_openJob = job;
	watchdog::g_opening = true;
	_status = tr("Opening ") + platform::ToUtf8(a_path.filename()) + "...";
	std::thread([job] {
		watchdog::Timed t("opening " + platform::ToUtf8(job->path.filename()));
		App::RunOpenJob(*job);
		job->done = true;
	}).detach();
}

void App::openNow(const std::filesystem::path& a_path)
{
	watchdog::Step st("open (now)");
	if (_openJob) {
		_openJob->cancel = true;
		_openJob.reset();
	}
	OpenJob job;
	job.path = a_path;
	job.sources = _sources;
	job.db = _db;
	job.game = _game;
	job.mo2 = _mo2;
	job.profile = _profile;
	job.gameFolder = gameFolder();
	RunOpenJob(job);
	finishOpen(job);
}

void App::RunOpenJob(OpenJob& a_job)
{
	const auto t0 = std::chrono::steady_clock::now();
	try {
		std::error_code aec;
		const auto      abs = std::filesystem::absolute(a_job.path, aec);
		a_job.abs = aec ? a_job.path : abs;  // (a save given on the command line can be a relative path)
		std::error_code ec;
		a_job.size = std::filesystem::file_size(a_job.path, ec);
		a_job.time = std::filesystem::last_write_time(a_job.path, ec);
		a_job.setCurrent(N_("reading the save file"));
		std::optional<watchdog::Timed> tm(std::in_place, "open: reading the file");
		auto s = SaveFile::load(a_job.path);
		tm.reset();
		if (!s->papyrus()) {
			throw std::runtime_error(tr("the save has no Papyrus section"));
		}
		if (a_job.cancel) {
			return;
		}
		a_job.setCurrent(N_("checking the save"));
		tm.emplace("open: Analyze, history numbers, picture, description, forms for the names");
		a_job.report = std::make_unique<ops::Report>(ops::Analyze(*s));
		a_job.stat = extras::ComputeStat(*s, a_job.abs);
		// the picture, as RGBA for the texture
		const auto&       h = s->header;
		const std::size_t bpp = h.isLE() ? 3 : 4;
		if (h.shotWidth && h.shotHeight && h.screenshot.size() >= std::size_t(h.shotWidth) * h.shotHeight * bpp) {
			a_job.rgba.assign(std::size_t(h.shotWidth) * h.shotHeight * 4, 255);
			for (std::size_t i = 0, n = std::size_t(h.shotWidth) * h.shotHeight; i < n; ++i) {
				for (std::size_t c = 0; c < 3; ++c) {
					a_job.rgba[i * 4 + c] = h.screenshot[i * bpp + c];
				}
			}
			a_job.w = static_cast<int>(h.shotWidth);
			a_job.h = static_cast<int>(h.shotHeight);
		}
		a_job.describe = DescribeSave(*s, s->isFO4());
		{
			PluginDB::Input in;
			PrepareInput(*s, in);
			a_job.wanted = std::make_shared<const std::unordered_set<std::uint32_t>>(std::move(in.wantedRefs));
		}
		// Mod Organizer: which profile this save was played with (reads every profile's plugins.txt). Only for the game
		// setup in use now: a save of another game switches the setup first, then the window scores it itself.
		tm.reset();
		const bool sameGame = GameOfSave(*s) == a_job.game;
		if (sameGame && a_job.mo2) {
			a_job.setCurrent(N_("matching the Mod Organizer profile"));
			a_job.profileMatch = ScoreProfiles(*s, *a_job.mo2, a_job.profile, a_job.gameFolder, a_job.game);
		}
		// (the Mods table, enchanted items, quests, followers: worked out by the analysis right after, while the save is
		// already shown)
		a_job.save = std::move(s);
	} catch (const std::exception& e) {
		a_job.error = e.what();
	}
	a_job.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void App::pollOpen()
{
	if (!_openJob || !_openJob->done) {
		return;
	}
	auto job = std::move(_openJob);
	watchdog::g_opening = false;
	if (job->cancel) {
		return;
	}
	if (_dirty && job->save) {
		// edits were made to the save shown while the other one was being read
		confirm(tr("Unsaved changes"),
			tr("The edits to ") + platform::ToUtf8(_path.filename()) + tr(" have not been saved. Open ") + platform::ToUtf8(job->path.filename()) +
				tr(" anyway (the edits are lost)?"),
			[this, job] { finishOpen(*job); });
		return;
	}
	watchdog::Step st("taking over the opened save");
	finishOpen(*job);
}

void App::finishOpen(OpenJob& a_job)
{
	++_editGen;
	const auto& a_path = a_job.path;
	if (!a_job.save) {
		const std::string why = a_job.error.empty() ? std::string(tr("cancelled")) : a_job.error;
		_error = std::string(tr("Could not open ")) + platform::ToUtf8(a_path.filename()) + ":\n\n" + why;
		_status = _error.substr(0, _error.find('\n'));
		if (startupLog) {
			startupLog("could not open " + platform::ToUtf8(a_path) + ": " + why);
			// what can still be told about the file, for the report
			std::error_code ec;
			startupLog("  file size " + std::to_string(std::filesystem::file_size(a_path, ec)) + " bytes");
			Header h;
			if (ReadSaveHeader(a_path, h)) {
				startupLog("  header: " + h.magic + ", save version " + std::to_string(h.version) + ", " + h.playerName + " level " +
						   std::to_string(h.playerLevel) + ", " + h.location + ", screenshot " + std::to_string(h.shotWidth) + "x" +
						   std::to_string(h.shotHeight) + (h.isFO4() ? " (Fallout 4)" : ""));
			} else {
				startupLog("  the header could not be read either: not a Skyrim or Fallout 4 save, or damaged at the start");
			}
		}
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	try {
		_save = std::move(a_job.save);
		_analysisJob.reset();  // (for the save shown before)
		_analysisNext = 0;
		_openSize = a_job.size;
		_openTime = a_job.time;
		const int gameBefore = _game;
		if (_save->isFO4()) {
			// Fallout 4: its own setup (mods folder, game folder, profile, load order); the tools that need the
			// save's change forms decoded (Skyrim only so far) close
			_showPlayer = _showActors = _showTransplant = _showMerge = _showSkyFollowers = false;
		}
		if (!_settingsLoaded) {
			loadSettings();
		}
		// a Skyrim VR save lists SkyrimVR.esm: use the VR setup (mods, load order, saves)
		setGame(GameOfSave(*_save));
		const bool sameSetup = _game == gameBefore && _sources == a_job.sources;
		_cfFacts.clear();
		_missingDismissed = false;
		_path = a_job.abs;
		if (const auto dir = platform::ToUtf8(a_path.parent_path()); !dir.empty() && dir != lastOpenDir()) {
			lastOpenDir() = dir;  // File > Open starts here next time
			saveSettings();
		}
		_dirty = false;
		_report.reset();
		_selected.clear();
		_focus.reset();
		_rowsDirty = true;
		_cat = Cat::kOverview;
		_homePanel = true;
		if (_shotTex && freeTexture) {
			freeTexture(_shotTex);
			_shotTex = 0;
		}
		if (uploadTexture && !a_job.rgba.empty()) {
			_shotTex = uploadTexture(a_job.rgba.data(), a_job.w, a_job.h);
			_shotW = a_job.w;
			_shotH = a_job.h;
		}
		_report = std::move(a_job.report);
		_wanted = std::move(a_job.wanted);
		_wantedSave = _save.get();
		_wantedGen = _editGen;
		_modsDirty = true;
		_leftoversDirty = true;
		// the Mods table, the missing mods, the enchanted items, the quests and the followers view: worked out on a worker
		// from the file (the window shows the save meanwhile, with their progress)
		(void)sameSetup;
		if (startAnalysis(kAnMods | kAnMissing | kAnFirst | (_save->isFO4() ? 0u : kAnSkyf))) {
			setMods(std::make_unique<mods::Table>());  // (empty until then: never the save shown before)
			_modsDirty = true;
		}
		{
			watchdog::Step st("save history");
			recordHistory(&a_job.stat);
		}
		// Mod Organizer: use the profile this save was played with (the one remembered may be another)
		bool profileSwitched = false;
		{
			watchdog::Step st("matchProfileToSave");
			profileSwitched = matchProfileToSave(sameSetup && _mo2 && a_job.mo2 && _profile == a_job.profile ? &a_job.profileMatch : nullptr);
		}
		const bool sameScanRunning = scanning() && !_scanDone && _scanProgress && !_scanProgress->cancel && !profileSwitched &&
									 _scanRoot == (_vortex ? _vortex->stagingPath : platform::FromUtf8(_sourcesPath));
		if (sameScanRunning) {
			// another save opened while the mods are still being read: that scan goes on (it is the same mods folder and
			// profile) and checks this save when it is done; starting it again only made the wait longer
		} else if (!_sourcesPath.empty() && (!_sources->scanned() || profileSwitched)) {
			watchdog::Step st("scanSources");
			scanSources(platform::FromUtf8(_sourcesPath));  // checks again for missing mods when done
		} else if (_autoNames && _sources->scanned()) {
			watchdog::Step st("startNames");
			startNames();
		}
		// the load order is known now: the banner shows at once (which mods are still installed is
		// known when the scan of the mods is done; that can take a while inside Mod Organizer)
		{
			watchdog::Step st("checkMissingPlugins");
			checkMissingPlugins();
		}
		_status = tr("Loaded ") + platform::ToUtf8(a_path.filename()) + (_save->isFO4() ? tr("  (Fallout 4 save)") : "");
		if (profileSwitched) {
			_status += "  " + _detectNote;
		}
		if (startupLog) {
			const double here = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			startupLog("opened " + platform::ToUtf8(a_path) + " in " + std::to_string(static_cast<long long>(a_job.ms + here)) + " ms (" +
					   std::to_string(static_cast<long long>(a_job.ms)) + " ms reading it, " + std::to_string(static_cast<long long>(here)) +
					   " ms in the window); mods folder: " + (_sourcesPath.empty() ? std::string("(not set)") : _sourcesPath) +
					   (scanning() ? " (reading it now)" : ""));
			// what the save is made of: everything for a Fallout 4 save (newer support, more questions), a summary
			// for Skyrim
			for (const auto& line : a_job.describe) {
				startupLog(std::string(_save->isFO4() ? "  fo4 " : "  ") + line);
			}
		}
		// a save whose structure does not add up may not be written back faithfully: look, don't save
		_readOnly = !_save->problems.empty();
		if (_readOnly) {
			_status += tr("  -  READ-ONLY (damaged save: see the Save check)");
		}
		_showCheck = _checkOnOpen || _readOnly;  // everything noticed about the save, in one window
	} catch (const std::exception& e) {
		_error = std::string(tr("Could not open ")) + platform::ToUtf8(a_path.filename()) + ":\n\n" + e.what();
		if (startupLog) {
			startupLog("could not open " + platform::ToUtf8(a_path) + ": " + e.what());
		}
	}
}

void App::setMods(std::unique_ptr<mods::Table> a_table)
{
	_mods = std::move(a_table);
	_scriptTotals.clear();
	for (const auto& r : _mods->rows) {
		for (const auto& [k, s] : r.scripts) {
			auto& t = _scriptTotals[k];
			t.first += s.instances.size();
			t.second += s.threads.size();
		}
	}
	_modsDirty = false;
	_modOrder.clear();
}

void App::installPrepared(Prepared& a_p)
{
	invalidateCheck();
	if (a_p.mods && a_p.modsSources == _sources) {
		setMods(std::move(a_p.mods));
	}
	if (a_p.hasCharged) {
		_charged = std::move(a_p.charged);
		_chargedGen = _editGen;
	}
	if (a_p.hasQuests) {
		_questStates = std::move(a_p.quests);
		_questGen = _editGen;
		_questPreviews.clear();
		_chains.clear();
	}
	if (a_p.skyf && a_p.skyfDb == _db) {
		adoptSkyf(std::move(a_p.skyf));
	}
	if (a_p.hasMissing && a_p.missingSources == _sources) {
		applyMissing(std::move(a_p.missing));
	}
}

// the parts both workers work out on their own copy of the save
void App::PrepareParts(Prepared& a_out, SaveFile& a_save, unsigned a_what, const std::shared_ptr<ScriptSources>& a_sources,
	const std::shared_ptr<PluginDB>& a_db, const std::function<void(const char*)>& a_step, const std::function<bool()>& a_cancelled)
{
	if ((a_what & App::kAnMods) && !a_cancelled()) {
		a_step(N_("matching scripts to your mods"));
		watchdog::Timed t("background: mods::Build");
		a_out.mods = std::make_unique<mods::Table>(mods::Build(a_save, a_sources && !a_sources->empty() ? a_sources.get() : nullptr));
		a_out.modsSources = a_sources;
	}
	if ((a_what & App::kAnFirst) && !a_cancelled()) {
		a_step(N_("enchanted items and quests"));
		watchdog::Timed t("background: FindChargedItems + quests::ReadStates");
		a_out.charged = cf::FindChargedItems(a_save);
		a_out.hasCharged = true;
		a_out.quests = quests::ReadStates(a_save);
		a_out.hasQuests = true;
	}
	if ((a_what & App::kAnSkyf) && !a_save.isFO4() && !a_cancelled()) {
		a_step(N_("followers, beast form and quests"));
		watchdog::Timed t("background: followers view");
		a_out.skyf = App::ComputeSkyf(a_save, a_db.get());
		a_out.skyfDb = a_db;
	}
}

// ---- the analysis after the mods scan / the plugin names ------------------------------------------------------

bool App::analysisWillProvide(unsigned a_what) const
{
	if (!_save) {
		return false;
	}
	if (_analysisJob && _analysisJob->gen == _editGen && _analysisJob->save == _save.get() && ((_analysisJob->what | _analysisNext) & a_what)) {
		return true;
	}
	return false;
}

bool App::startAnalysis(unsigned a_what)
{
	if (!_save || _dirty || _path.empty() || std::getenv("RESAVERPP_NO_BACKGROUND_ANALYSIS")) {
		return false;
	}
	if (_analysisJob && _analysisJob->gen == _editGen && _analysisJob->save == _save.get()) {
		_analysisNext |= a_what;  // after the one running (its file read is not wasted on parts it already has)
		return true;
	}
	auto job = std::make_shared<AnalysisJob>();
	job->path = _path;
	job->size = _openSize;
	job->time = _openTime;
	job->what = a_what;
	job->gen = _editGen;
	job->save = _save.get();
	job->sources = _sources;
	job->db = _db;
	job->missingIn = missingInputs();
	job->steps = 1 + ((a_what & kAnMods) ? 1 : 0) + ((a_what & kAnMissing) ? 1 : 0) + ((a_what & kAnSkyf) ? 1 : 0) + ((a_what & kAnFirst) ? 1 : 0);
	_analysisJob = job;
	std::thread([job] {
		watchdog::Timed t("analysing the save again");
		try {
			std::error_code ec;
			const auto      size = std::filesystem::file_size(job->path, ec);
			const auto      time = std::filesystem::last_write_time(job->path, ec);
			if (ec || size != job->size || time != job->time) {
				throw std::runtime_error("the file changed since it was opened");
			}
			job->setCurrent(N_("reading the save file"));
			std::optional<watchdog::Timed> tm(std::in_place, "analysis: reading the file");
			auto s = SaveFile::load(job->path);
			tm.reset();
			PrepareParts(job->out, *s, job->what & (kAnMods | kAnSkyf | kAnFirst), job->sources, job->db, [&](const char* a_what) { job->setCurrent(a_what); },
				[&] { return job->cancel.load(); });
			if ((job->what & kAnMissing) && !job->cancel) {
				job->setCurrent(N_("mods no longer in your load order"));
				const mods::Table* table = job->out.mods.get();
				std::unique_ptr<mods::Table> own;
				App::ComputeMissing(*s, job->missingIn, [&]() -> const mods::Table* {
					if (!table) {
						own = std::make_unique<mods::Table>(mods::Build(*s, job->sources && !job->sources->empty() ? job->sources.get() : nullptr));
						table = own.get();
					}
					return table;
				}, job->out.missing);
				job->out.hasMissing = true;
				job->out.missingSources = job->sources;
			}
		} catch (const std::exception& e) {
			job->error = e.what();
		}
		job->done = true;
	}).detach();
	return true;
}

void App::pollAnalysis()
{
	if (!_analysisJob || !_analysisJob->done) {
		return;
	}
	auto job = std::move(_analysisJob);
	const unsigned next = _analysisNext;
	_analysisNext = 0;
	const bool same = job->gen == _editGen && job->save == _save.get() && !job->cancel;
	if (same && job->error.empty()) {
		watchdog::Step st("taking over the analysis");
		installPrepared(job->out);
	} else if (startupLog && !job->error.empty()) {
		startupLog("background analysis not used (" + job->error + "): worked out in the window instead");
	}
	// what it could not give (edited meanwhile, another scan or other names since, an error): the window works it out
	if (!_save) {
		return;
	}
	if (next && startAnalysis(next)) {
		return;
	}
	const unsigned left = next | (same && job->error.empty() ? 0u : job->what);
	if ((left & kAnMissing) || (job->out.hasMissing && job->out.missingSources != _sources)) {
		checkMissingPlugins();
	}
}

// ---- the jobs in the status bar -------------------------------------------------------------------------------

std::vector<App::JobRow> App::runningJobs()
{
	std::vector<JobRow> v;
	if (_openJob) {
		JobRow r;
		r.name = tr("Opening ") + platform::ToUtf8(_openJob->path.filename());
		r.done = _openJob->step.load();
		r.total = OpenJob::kSteps;
		r.current = tr(_openJob->currentText().c_str());
		auto job = _openJob;
		r.cancel = [this, job] {
			job->cancel = true;
			if (_openJob == job) {
				_openJob.reset();
				watchdog::g_opening = false;
				_status = tr("Opening cancelled");
			}
		};
		v.push_back(std::move(r));
	}
	if (scanning() && _scanProgress) {
		JobRow r;
		r.name = tr("Scanning your mods");
		r.done = _scanProgress->done.load();
		r.total = _scanProgress->total.load();
		r.current = _scanProgress->current();
		auto* prog = _scanProgress.get();
		r.cancel = [prog] { prog->cancel = true; };
		v.push_back(std::move(r));
	}
	if (_analysisJob) {
		JobRow r;
		r.name = tr("Working out the results for your mods");
		r.done = _analysisJob->step.load();
		r.total = _analysisJob->steps.load();
		r.current = tr(_analysisJob->currentText().c_str());
		v.push_back(std::move(r));
	}
	if (namesLoading() && _dbProgress) {
		JobRow r;
		r.name = tr("Reading plugin names");
		r.done = _dbProgress->done.load();
		r.total = _dbProgress->total.load();
		{
			std::lock_guard l(_dbProgress->m);
			r.current = _dbProgress->current;
		}
		auto* prog = _dbProgress.get();
		r.cancel = [prog] { prog->cancel = true; };
		v.push_back(std::move(r));
	}
	appendOtherJobs(v);
	return v;
}

bool App::drawJobProgress(const JobRow& a_job, float a_width, bool a_withCancel)
{
	const float       f = a_job.total ? std::min(1.0f, static_cast<float>(a_job.done) / static_cast<float>(a_job.total)) : 0.0f;
	const std::string label = a_job.total ? Fmt(tr("%s: %zu of %zu%s%s"), a_job.name.c_str(), a_job.done, a_job.total, a_job.current.empty() ? "" : "  -  ",
												 a_job.current.c_str())
										  : a_job.name + (a_job.current.empty() ? std::string("...") : "  -  " + a_job.current);
	const float cancelW = a_withCancel && a_job.cancel ? ImGui::CalcTextSize(tr("Cancel")).x + ImGui::GetStyle().FramePadding.x * 2 + 6 : 0.0f;
	const float barW = a_width > 0 ? std::max(40.0f, a_width - cancelW) : -std::max(1.0f, cancelW);
	ImGui::ProgressBar(a_job.total ? f : -1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(barW, 0), "");
	// the text inside the bar, from its left edge (cut at the right end when it does not fit)
	const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	auto*        dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(a, b, true);
	const float pad = ImGui::GetStyle().FramePadding.x;
	dl->AddText(ImVec2(a.x + pad, a.y + (b.y - a.y - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
	dl->PopClipRect();
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", label.c_str());
	}
	if (cancelW > 0) {
		ImGui::SameLine(0, 6);
		ImGui::PushID(a_job.name.c_str());
		if (ImGui::SmallButton(tr("Cancel"))) {
			a_job.cancel();
		}
		ImGui::PopID();
		tips::Tip(tr("Stop this job. What it did so far is dropped."));
	}
	return true;
}

void App::drawJobsBar(float a_width)
{
	const auto jobs = runningJobs();
	if (jobs.empty()) {
		return;
	}
	// the first job in the bar; with more, a button lists them all (each with its own bar and Cancel)
	const float moreW = jobs.size() > 1 ? ImGui::CalcTextSize(Fmt(tr("+%zu more"), jobs.size() - 1).c_str()).x + ImGui::GetStyle().FramePadding.x * 2 + 6 : 0.0f;
	drawJobProgress(jobs.front(), a_width - moreW, true);
	if (jobs.size() > 1) {
		ImGui::SameLine(0, 6);
		if (ImGui::SmallButton(Fmt(tr("+%zu more"), jobs.size() - 1).c_str())) {
			_jobsPopup = !_jobsPopup;
		}
		tips::Tip(tr("Show every job running in the background."));
	}
	if (_jobsPopup && jobs.size() > 1) {
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		const float          w = std::min(620.0f, vp->WorkSize.x - 40);
		ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20, vp->WorkPos.y + vp->WorkSize.y - ImGui::GetFrameHeightWithSpacing() * 1.5f),
			ImGuiCond_Always, ImVec2(1, 1));
		ImGui::SetNextWindowSize(ImVec2(w, 0));
		if (ImGui::Begin(trId("Background jobs"), &_jobsPopup, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
			for (const auto& j : jobs) {
				drawJobProgress(j, w - ImGui::GetStyle().WindowPadding.x * 2, true);
			}
		}
		ImGui::End();
	} else if (jobs.size() <= 1) {
		_jobsPopup = false;
	}
}

bool App::drawJobsInline(std::initializer_list<const char*> a_names)
{
	// the jobs a window waits for (names as runningJobs gives them, untranslated), each with its progress
	bool any = false;
	for (const auto& j : runningJobs()) {
		for (const char* n : a_names) {
			if (j.name.rfind(tr(n), 0) == 0) {
				drawJobProgress(j, 0, true);
				any = true;
				break;
			}
		}
	}
	return any;
}

void App::appendOtherJobs(std::vector<JobRow>& v)
{
	appendPathJob(v);
	appendBloatJobs(v);
	if (_mergeThread.joinable() && _mergeProgress) {
		JobRow r;
		r.name = tr("Building the merged patch");
		r.done = _mergeProgress->done.load();
		r.total = _mergeProgress->total.load();
		auto* prog = _mergeProgress.get();
		r.cancel = [prog] { prog->cancel = true; };
		v.push_back(std::move(r));
	}
	if (_assetsThread.joinable() && _assetsProgress) {
		JobRow r;
		r.name = tr("Finding file conflicts");
		r.done = _assetsProgress->done.load();
		r.total = _assetsProgress->total.load();
		auto* prog = _assetsProgress.get();
		r.cancel = [prog] { prog->cancel = true; };
		v.push_back(std::move(r));
	}
	if (_historyBusy) {
		JobRow r;
		r.name = tr("Reading the character's saves (save history)");
		r.done = _historyDone.load();
		r.total = _historyTotal.load();
		r.cancel = [this] { _historyCancel = true; };
		v.push_back(std::move(r));
	}
	appendGalleryJob(v);
	appendCollectJob(v);
}

void App::stopOpenAndAnalysis()
{
	if (_openJob) {
		_openJob->cancel = true;
	}
	if (_analysisJob) {
		_analysisJob->cancel = true;
	}
}

// test hook (RESAVERPP_TEST_DUMP_STATE): what the checks found, written to the log at exit, to compare the background
// results with the window's own (RESAVERPP_SYNC_OPEN=1 RESAVERPP_NO_BACKGROUND_ANALYSIS=1)
std::vector<std::string> App::testDumpState()
{
	std::vector<std::string> out;
	if (!_save) {
		return out;
	}
	ensureMods();
	std::size_t inst = 0, scripts = 0, threads = 0, provided = 0, shipped = 0;
	for (const auto& r : _mods->rows) {
		inst += r.instanceCount;
		scripts += r.scriptCount;
		threads += r.threadCount;
		provided += r.providedInSave;
		shipped += r.shipped.size();
	}
	out.push_back(Fmt("mods: %zu rows, %zu scripts, %zu instances, %zu threads, %zu provided, %zu shipped, sources %d", _mods->rows.size(), scripts, inst,
		threads, provided, shipped, _mods->hasSources ? 1 : 0));
	out.push_back(Fmt("missing: %zu mods, %zu missing scripts, %zu not found, known %d, suspect %d/%d, active %zu/%zu/%zu, remove %d", _missing.size(),
		_missingScripts.size(), _unfoundScripts.size(), _installKnown ? 1 : 0, _orderSuspect ? 1 : 0, _scanSuspect ? 1 : 0, _activeInSave, _activeFound,
		_activeChecked, _removeMissingScripts ? 1 : 0));
	for (const auto& m : _missing) {
		std::string ren;
		for (const auto& r : m.renamedTo) {
			ren += " " + r;
		}
		out.push_back(Fmt("  %s installed %d selected %d cf %zu scripts %zu inst %zu threads %zu folder %s renamed%s", m.plugin.c_str(), m.installed ? 1 : 0,
			m.selected ? 1 : 0, m.changeForms, m.scripts, m.instances, m.threads, m.folder.c_str(), ren.c_str()));
	}
	out.push_back(Fmt("charged %zu, quests %zu", chargedItems().size(), questStates().size()));
	skyfRefresh();
	out.push_back(skyfSummary());
	_checkNext = 0;
	for (const auto& it : checkItems()) {
		out.push_back("check: " + it.title);
	}
	return out;
}
