// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Runs Bethesda's Papyrus compiler (PapyrusCompiler.exe from the Creation Kit, installed in the game
// folder under "Papyrus Compiler") on a .psc and reads its messages, like the npp-papyrus Notepad++
// plugin does. The compiler itself is not part of ReSaver++.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ess::papc
{
	struct Message
	{
		std::string file;  // as printed by the compiler ("<unknown>" for general errors)
		int         line = 0, column = 0;
		std::string text;
	};

	struct Request
	{
		std::filesystem::path              compiler;  // PapyrusCompiler.exe
		std::filesystem::path              source;    // the .psc to compile
		std::vector<std::filesystem::path> imports;   // import folders, highest priority first
		std::filesystem::path              output;    // folder for the .pex
		std::string                        flags = "TESV_Papyrus_Flags.flg";
		bool                               optimize = false;
		bool                               anonymize = false;  // blank the source path, user and machine names stored in the .pex
	};

	struct Result
	{
		bool                     ran = false;  // the compiler could be started
		bool                     ok = false;   // a new .pex was written
		int                      exitCode = -1;
		std::vector<Message>     messages;
		std::vector<std::string> output;       // everything the compiler printed
		std::string              commandLine;
		std::filesystem::path    pex;          // the written file
		std::string              error;        // why it could not run, or a note (anonymize failure, ...)
		std::size_t              importsDropped = 0;  // folders left out because the command line was too long
	};

	// Runs the compiler and waits for it (call it from a worker thread).
	Result Compile(const Request& a_request);

	// "file.psc(12,5): message" lines of the compiler's output.
	std::vector<Message> ParseOutput(const std::vector<std::string>& a_lines);

	// Replaces the source path, user name and machine name stored in a .pex with dashes (same length).
	bool Anonymize(const std::filesystem::path& a_pex, std::string& a_error);

	// PapyrusCompiler.exe under a game folder ("<game>\Papyrus Compiler\PapyrusCompiler.exe").
	std::optional<std::filesystem::path> FindCompiler(const std::filesystem::path& a_gameFolder);
}
