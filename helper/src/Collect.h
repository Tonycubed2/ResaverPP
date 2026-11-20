#pragma once
// Collect (version 21): the F1 page's "Save all game data" button. Puts in one zip on the player's Desktop, named
// SkyrimGameData_<date>_<time>.zip (no name to pick, no Save As dialog while the game runs), what ReSaver Plus's
// "Collect all logs" puts in its zip for Skyrim: the game state report (Report.h) and the helper report (LiveInfo), the
// SKSE folder with its logs and crash logs, Logs\Script (Papyrus), the INI files in My Games, the load order the game
// reads, the .NET Script Framework crash logs, Mod Organizer's logs and the profile's mod list / plugins / load order,
// the last 3 saves with their .skse files, the list of SKSE plugin DLLs, and a README.txt with the versions and what
// was left out. The zip is written on its own thread; the page shows how far it is.

namespace Collect
{
	struct Progress
	{
		bool                  busy = false;
		std::size_t           done = 0, total = 0;
		std::filesystem::path zip;      // the finished zip ("" while busy or after a failure)
		std::string           message;  // the last result, for the page
	};
	Progress Get();
	bool     Busy();
	// a_gameReport: Report::Write's file ("" when it could not be written, a_gameReportNote says why);
	// a_helperReport: the text of LiveInfo::ReportText. Returns false if a collection is already running.
	bool Start(const std::filesystem::path& a_gameReport, const std::string& a_gameReportNote, std::string a_helperReport);
	// opens Explorer with the zip selected
	void ShowInFolder(const std::filesystem::path& a_zip);
}
