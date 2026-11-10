#pragma once

// ReSaverHelper.ini, next to the DLL. ReSaver++ can change these through the pipe (SETOPTION); the
// helper writes the file back keeping its comments.
namespace Settings
{
	struct Values
	{
		std::string notify = "hud";           // sNotify: hud | box | none - how the helper confirms what it did
		bool        refreshOnLoad = false;    // bRefreshJournalOnLoad: refresh the quest journal after every save load
		int         refreshDelay = 5;         // iRefreshDelaySeconds: wait after the load before refreshing
		bool        listen = true;            // bListen: accept commands from ReSaver++ at all
		bool        rescueHotkey = true;      // bRescueHotkey: holding Ctrl saves, restores controls, gathers followers
		int         rescueHoldSeconds = 8;    // iRescueHoldSeconds: how long Ctrl must be held
		std::string gatherSkip;               // sGatherSkip: form ids (hex, comma separated) Gather followers leaves alone
		// version 16, the menu load guard (MenuGuard.h); read when the game starts
		bool        menuLoadGuard = true;     // bMenuLoadGuard: hook LoadMovie, retry failed loads, close a menu without its movie
		int         menuLoadRetries = 2;      // iMenuLoadRetries: tries after a failed load (0 to 5; the 2nd and later wait 50 ms)
		bool        logAllMovieLoads = false; // bLogAllMovieLoads: one log line per movie load (else only failures and the last 50)
		std::string testFailMovies;           // sTestFailMovies: TEST ONLY - movie names whose load is made to fail on purpose
		std::string testFailMode = "first";   // sTestFailMode: first = only the first try fails, all = every try fails
	};

	Values&               Get();
	void                  Load();
	bool                  Save();  // writes the current values into the INI (creates it if missing)
	std::filesystem::path IniPath();

	// option by its INI key; false if the name or value is not valid
	bool        SetOption(const std::string& a_name, const std::string& a_value);
	std::string GetOption(const std::string& a_name);

	// iDiagnosticStage (version 14): the stage this game session started with (main.cpp reads it before anything
	// else runs; -1 = normal), the value written in ReSaverHelper.ini now (-1 = no line, i.e. normal), and writing it
	// (version 15, the F1 page): 0-4 sets the line, -1 removes it. It takes effect the next time the game starts.
	int  StartupDiagnosticStage();
	void SetStartupDiagnosticStage(int a_stage);
	int  DiagnosticStageInIni();
	bool WriteDiagnosticStage(int a_stage);
}
