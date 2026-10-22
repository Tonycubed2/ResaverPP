// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Finds which installed mod provides each compiled script (.pex), by scanning a Mod Organizer
// mods folder (or a plain Data folder): loose Scripts\*.pex files and the file lists of BSAs.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ess
{
	struct ScriptProvider
	{
		std::string              mod;      // mod folder name ("Data" when scanning a Data folder)
		std::vector<std::string> plugins;  // .esp/.esm/.esl files in that folder (lowercase)
		bool                     fromBSA = false;
		std::filesystem::path    bsa;  // the archive holding it (fromBSA)
		bool                     enabled = true;  // false: mod folder exists but is disabled in the MO2 profile
	};

	// A Mod Organizer 2 instance that uses a given mods folder.
	struct Mo2Instance
	{
		std::filesystem::path    ini, base, modsDir, profilesDir, overwriteDir;
		std::filesystem::path    gamePath;  // game install folder (its Data folder holds the vanilla scripts)
		std::string              selectedProfile;
		std::string              gameName;  // "Skyrim Special Edition", "Fallout 4", ...
		std::vector<std::string> profiles;
	};
	// Looks for ModOrganizer.ini (global instances under %LOCALAPPDATA%\ModOrganizer, and a
	// portable instance next to the mods folder) whose mod_directory is a_modsFolder.
	std::optional<Mo2Instance> FindMo2Instance(const std::filesystem::path& a_modsFolder);
	std::optional<Mo2Instance> ReadMo2Ini(const std::filesystem::path& a_ini);  // one ModOrganizer.ini
	// Every Mod Organizer instance found: global ones (%LOCALAPPDATA%\ModOrganizer\<name>) and portable
	// ones (ModOrganizer.ini in a_extraDirs or their parent folders).
	std::vector<Mo2Instance> FindMo2Instances(const std::vector<std::filesystem::path>& a_extraDirs = {});
	// Enabled mods of a profile, highest priority first (MO2 writes modlist.txt that way).
	std::vector<std::string> LoadModList(const std::filesystem::path& a_profileDir);

	// Vortex: mods are kept in a staging folder (one sub-folder per mod) and deployed into the game's
	// Data folder; vortex.deployment.json lists every deployed file and the mod it came from.
	struct VortexDeployment
	{
		std::filesystem::path                        manifest, dataPath, stagingPath;
		std::string                                  method, gameId;
		std::vector<std::string>                     mods;    // deployed mods (staging folder names)
		std::unordered_map<std::string, std::string> winner;  // lowercase relative path (backslashes) -> mod whose file is deployed
		std::size_t                                  files = 0;
	};
	std::optional<VortexDeployment> ReadVortexDeployment(const std::filesystem::path& a_manifest);
	// a_folder: the game folder, its Data folder, or Vortex's staging folder for the game.
	std::optional<VortexDeployment> FindVortexDeployment(const std::filesystem::path& a_folder);

	// Progress of a scan running on another thread.
	struct ScanProgress
	{
		std::atomic<std::size_t> done{ 0 }, total{ 0 };
		std::atomic<bool>        cancel{ false };
	};

	class ScriptSources
	{
	public:
		// a_root: MO2 "mods" folder (one sub-folder per mod) or a game Data folder.
		// With a mod order, only enabled mods are scanned and providers are ranked by priority,
		// so providers(x).front() is the copy the game actually loads.
		// Disabled mods are scanned too (after the enabled ones, marked enabled=false), and the game's
		// own Data folder last as "Game Data", so vanilla scripts are known.
		void scan(const std::filesystem::path& a_root, const std::vector<std::string>* a_modOrder = nullptr,
			const std::filesystem::path& a_overwrite = {}, const std::filesystem::path& a_gameData = {}, ScanProgress* a_progress = nullptr);
		// Running inside Mod Organizer (its virtual file system): the game's Data folder shows every mod's
		// files too, so only the game's own archives are read there (the mods are scanned in their folders).
		bool                                 gameDataVanillaOnly = false;
		// Vortex: scan its staging folder, credit each deployed file to the mod Vortex deployed it from
		void scanVortex(const VortexDeployment& a_vortex, ScanProgress* a_progress = nullptr);
		bool                                 vortex = false;
		bool                                 ranked = false;  // providers are in MO2 priority order
		bool                                 gameDataScanned = false;  // vanilla scripts/BSAs were included
		std::set<std::string>                enabledFolders;  // mod folders enabled in the profile
		std::filesystem::path                overwriteDir, gameData;

		// Folder on disk for a provider/mod name ("Overwrite", "Game Data", "Data" or a mod folder).
		std::filesystem::path                folderPath(const std::string& a_folder) const;
		// Plugin file on disk, if it was seen during the scan.
		std::optional<std::filesystem::path> pluginPath(const std::string& a_plugin) const;

		bool                                 empty() const { return _byScript.empty(); }  // no scripts found
		bool                                 scanned() const { return modsScanned > 0; }
		const std::vector<ScriptProvider>*   providers(const std::string& a_script) const;  // case-insensitive
		std::filesystem::path                root;
		std::size_t                          modsScanned = 0, disabledScanned = 0, pexFound = 0, bsasRead = 0;
		std::vector<std::string>             errors;

		// localized string tables (lowercase file name, e.g. "skyrim_english.strings"); the highest
		// priority copy wins. bsaEntry is empty for loose files.
		struct AssetLocation
		{
			std::filesystem::path file;
			std::string           bsaEntry;
		};
		std::unordered_map<std::string, AssetLocation> stringsFiles;
		// Papyrus sources (.psc) by lowercase script name: Source\Scripts or Scripts\Source in each folder
		std::unordered_map<std::string, std::filesystem::path> sourceFiles;

		std::unordered_map<std::string, std::string> sourceMods;  // lowercase script -> mod folder of its .psc
		// folders holding .psc files, in priority order (enabled = the mod is active in the profile)
		std::vector<std::pair<std::filesystem::path, bool>> sourceDirs;

		// every script name seen (compiled or source), lowercase
		std::vector<std::string> scriptNames() const;

		// where the compiled script the game loads is: a loose file, or an entry of a BSA
		struct PexFile
		{
			std::string           mod;
			std::filesystem::path file;      // the .pex, or the .bsa
			std::string           bsaEntry;  // "scripts\\x.pex" when inside a BSA
		};
		std::optional<PexFile>           findPex(const std::string& a_script) const;
		static std::vector<std::uint8_t> readPex(const PexFile& a_pex);  // throws on errors

		// mod folder -> the compiled scripts it ships (loose or in its BSAs)
		std::unordered_map<std::string, std::vector<std::string>> scriptsByMod;

		// plugin (lowercase) -> mod folder that contains it
		std::unordered_map<std::string, std::string> pluginFolder;

	private:
		void scanFolder(const std::filesystem::path& a_dir, const std::string& a_modName, bool a_enabled = true);
		void scanGameData();
		void reset();
		std::unordered_set<std::string> _gameDataSkip;  // archive names (lowercase) not to read in the game's Data folder
		std::unordered_map<std::string, std::vector<ScriptProvider>> _byScript;  // lowercase script name
	};

	// Lists the file paths stored in a Skyrim SE BSA (version 104/105), lowercase with backslashes.
	std::vector<std::string> ListBSA(const std::filesystem::path& a_bsa);
}
