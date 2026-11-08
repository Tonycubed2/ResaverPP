// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace platform
{
	// Native open/save dialogs on Windows. Elsewhere these return nullopt and the app
	// falls back to typing a path.
	bool                                 HasNativeDialogs();
	std::optional<std::filesystem::path> OpenSaveDialog(const std::filesystem::path& a_startDir);
	std::optional<std::filesystem::path> SaveAsDialog(const std::filesystem::path& a_suggested);
	// a_game: 0 Skyrim SE/AE, 1 Skyrim VR, 2 Fallout 4 (the same numbers everywhere in the app)
	std::filesystem::path                DefaultSavesFolder(int a_game = 0);  // Documents\My Games\<MyGamesName>\Saves
	const char*                          MyGamesName(int a_game);  // "Skyrim Special Edition", "Skyrim VR", "Fallout4"
	std::optional<std::filesystem::path> PickFolderDialog(const std::string& a_title);
	// Save dialog for other files (reports): a_ext without the dot, e.g. "pdf".
	std::optional<std::filesystem::path> SaveFileDialog(const std::filesystem::path& a_suggested, const std::string& a_ext, const std::string& a_title);
	std::optional<std::filesystem::path> OpenFileDialog(const std::filesystem::path& a_startDir, const std::string& a_ext, const std::string& a_title);
	void                                 OpenWithDefaultApp(const std::filesystem::path& a_path);  // e.g. show a PDF
	std::filesystem::path                SettingsFile();  // resaverpp.ini next to the program
	std::string                          ToUtf8(const std::filesystem::path& a_path);
	// Skyrim Special Edition installs found in the registry (Bethesda key, Steam libraries, GOG), existing ones only
	std::vector<std::filesystem::path>   SkyrimInstallFolders(int a_game = 0);  // Skyrim VR's (1), Fallout 4's (2)
	std::filesystem::path                ExeFolder();  // folder of ReSaverPP.exe
	bool                                 RunningUnderMO2();  // started from Mod Organizer (its virtual file system is loaded)
	// a Mod Organizer (ModOrganizer.exe) is running; nullopt where it cannot be told (not Windows)
	std::optional<bool>                  Mo2ProcessRunning();
	std::filesystem::path                FromUtf8(const std::string& a_utf8);
	std::filesystem::path                DesktopFolder();
	std::filesystem::path                LocalAppDataFolder();  // %LOCALAPPDATA% (plugins.txt lives under it)
	void                                 ShowInFolder(const std::filesystem::path& a_file);  // Explorer with the file selected
	// computer, Windows (or Wine), processor, memory, video cards, screen, the game's version (SysInfo.cpp)
	std::string                          SystemReport(const std::filesystem::path& a_gameExe);
	// Moves files to the Recycle Bin (Windows); elsewhere deletes them. True if all of them went.
	bool                                 MoveToRecycleBin(const std::vector<std::filesystem::path>& a_files);
}
