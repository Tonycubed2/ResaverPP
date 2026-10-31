// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Skyrim's logs: finding them (Documents\My Games\<game>\SKSE and its subfolders, Logs\Script, the
// .NET Script Framework crash folder) and reading them for problems: SKSE plugins that did not load,
// crash logs (what crashed, which DLLs are in the call stack, known crashes), the Papyrus log, and the
// errors other SKSE plugins wrote into their own logs. Used by "Analyze Skyrim logs", the AI chat's
// log tools and resaverpp-cli --analyze-logs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ess::logs
{
	enum class Kind
	{
		kSkse,     // skse64.log / skse.log / sksevr.log / skse64_loader.log
		kCrash,    // a crash log (Crash Logger, Trainwreck, .NET Script Framework)
		kPapyrus,  // Papyrus.N.log
		kPlugin    // another SKSE plugin's own log
	};
	const char* KindName(Kind a_kind);

	struct LogFile
	{
		std::filesystem::path           path;
		std::string                     game;   // the My Games folder it is under ("Skyrim Special Edition", "Skyrim"...), or ""
		Kind                            kind = Kind::kPlugin;
		std::uintmax_t                  size = 0;
		std::filesystem::file_time_type time{};
		std::int64_t                    ageMinutes = 0;  // how long ago it was last written
	};

	// The folders searched: for each a_myGames folder (Documents\My Games), every Skyrim game in it
	// (Skyrim Special Edition, Skyrim Special Edition GOG, Skyrim VR, Skyrim) with its SKSE folder (and the
	// folders in it) and Logs\Script; plus a_extra folders as given (e.g. <game>\Data\NetScriptFramework\Crash).
	std::vector<std::filesystem::path> Folders(const std::vector<std::filesystem::path>& a_myGames,
		const std::vector<std::filesystem::path>& a_extra);
	// every .log / .txt file in those folders (the SKSE folder two levels deep), newest first
	std::vector<LogFile> Find(const std::vector<std::filesystem::path>& a_folders);
	Kind                 KindOf(const std::filesystem::path& a_file);

	enum class Severity
	{
		kBad,   // most likely a cause of problems
		kWarn,  // worth a look
		kInfo   // for context
	};
	struct Finding
	{
		Severity                 severity = Severity::kInfo;
		std::string              title;    // one line
		std::string              detail;   // what it means and what to do
		std::filesystem::path    file;     // the log it came from
		std::size_t              line = 0; // 1-based, 0 when not one line
		std::vector<std::string> examples; // a few lines from the log
	};
	struct Options
	{
		int  days = 7;           // only logs written in the last days (0: all of them)
		int  crashLogs = 10;     // the newest crash logs read
		bool pluginLogs = true;  // look into the other plugins' own logs
	};
	struct Analysis
	{
		std::vector<LogFile> files;     // the logs looked at
		std::size_t          skipped = 0;  // older logs left out (Options::days)
		std::vector<Finding> findings;  // worst first
	};
	Analysis Analyze(const std::vector<LogFile>& a_logs, const Options& a_options = {});
	std::string ToText(const Analysis& a_analysis);  // the findings as plain text (CLI, the AI)

	// reading for the AI chat: a_from 1-based; a_count lines (0: to the end); a_tail > 0: the last lines instead
	std::string ReadLines(const std::filesystem::path& a_file, std::size_t a_from, std::size_t a_count, std::size_t a_tail,
		std::size_t a_maxChars, std::size_t* a_total = nullptr);
	// lines containing a_text (case-insensitive) in the given logs, "file:line: text"
	std::string Search(const std::vector<LogFile>& a_logs, const std::string& a_text, std::size_t a_maxHits);

	// parts used by Analyze (tests)
	std::vector<Finding> ReadSkseLog(const std::filesystem::path& a_file, const std::string& a_text);
	struct CrashInfo
	{
		std::string              exception;  // "Unhandled exception "EXCEPTION_ACCESS_VIOLATION" at 0x7FF6... SkyrimSE.exe+0CEC680"
		std::string              where;      // "SkyrimSE.exe+0CEC680"
		std::string              tool;
		std::string              game;       // game version line, when given
		std::vector<std::string> modules;    // DLLs in the call stack, most relevant first (game and Windows DLLs left out)
		std::vector<std::string> forms;      // names / form ids the log links to the crash
	};
	CrashInfo ReadCrash(const std::string& a_text);
}
