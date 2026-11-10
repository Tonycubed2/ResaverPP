#pragma once

// Menu load guard (version 16, Skyrim SE / AE; not on VR): a menu whose interface movie (Interface/<name>.swf) fails to
// load no longer crashes the game, and ReSaverHelper.log says why the load failed.
//
// Why: every menu's constructor calls BSScaleformManager::LoadMovie (RELOCATION_ID(80302, 82325)) for its movie. When
// that returns false the menu keeps no movie (uiMovie = null) and the game uses it anyway: MapMenu's constructor
// crashes at once (+0x200 on 1.6.640 / 1.6.1170), the Journal crashes on opening (GFxMovieView::InvokeNoReturn with a
// null movie). Seen by a tester on 1.6.640 under Proton; the reason for the failed load is not known.
//
// What it does (all verified on the 1.6.1170 executable, see docs/CLAUDE-HANDOFF.md "Menu load guard"):
// - LoadMovie is hooked at its entry. A failed load is logged with the step that failed (the game's file check,
//   GFxLoader::CreateMovie, or the movie instance) and the process state, then tried again (iMenuLoadRetries).
// - Every menu creator in UI's menu map is wrapped. When all tries fail while a menu is being created, the hook throws
//   a C++ exception that the wrapper catches: the game's own unwind code destroys the half-built menu and frees its
//   memory (the same cleanup it does when a constructor throws). The wrapper then gives the game a small stand-in menu
//   that answers kIgnore to the "show" message, so UI::ProcessMessages drops it from the menu map: the menu simply
//   does not open, and the next try creates it anew. A notification says so.
// - The F1 page (Troubleshooting tests > Helper and connection) and the report list the failures per movie.
namespace MenuGuard
{
	// at plugin load (normal stage only, bMenuLoadGuard=1, SE/AE): reserves the hook trampoline
	void Prepare();
	// kPostLoad (every SKSE plugin is loaded, so their own hooks are in place): logs who else hooks LoadMovie, then hooks it
	void Install();
	// kDataLoaded and after every load / new game: wraps the menu creators not wrapped yet, logs the hook chain again
	void WrapCreators(const char* a_when);

	struct Problem
	{
		std::string movie;          // the name given to LoadMovie, e.g. "Map", "Quest_Journal"
		int         failed = 0;     // loads that failed at the first try
		int         recovered = 0;  // ... and worked on a retry
		int         closed = 0;     // menus that could not open (all tries failed): shown the notice instead of crashing
		bool        test = false;   // the last failure was a forced one (sTestFailMovies)
		std::string lastStep;       // which step failed the last time
		std::string lastTime;       // when (local time)
	};
	std::vector<Problem> Problems();
	std::string          StatusLine();  // installed or not, and why
	std::string          ReportText();  // the "Menu load problems" lines for the F1 report
}
