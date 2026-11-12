// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// File conflicts between mods: loose files and the files inside the archives (BSAs, Fallout 4's BA2s) the game loads.
// The game's order: archives first (the base game's, then each plugin's "<plugin>.bsa" and
// "<plugin> - Textures.bsa" in load order), then every loose file above every archive; among loose
// files Mod Organizer's mod priority decides, Overwrite last. For each file the last one wins.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ess::assets
{
	struct Input
	{
		std::filesystem::path    modsDir;       // Mod Organizer's mods folder (empty: no mod manager)
		std::vector<std::string> modsLowFirst;  // enabled mods, lowest priority first
		std::filesystem::path    overwrite;     // Mod Organizer's Overwrite folder
		std::filesystem::path    gameData;      // the game's Data folder
		std::vector<std::string> loadOrder;     // enabled plugins in load order (which archives load, and in what order)
		// Fallout 4: its archives are .ba2; the base game's "Fallout4 - *.ba2" first, then each plugin's
		// "<plugin> - Main.ba2", "<plugin> - Textures.ba2" and "<plugin> - Voices_<language>.ba2" in load order
		bool                     fo4 = false;
		// Starfield: .ba2 too; its own archives as Starfield.ini's [Archive] lists name them (then any other "Starfield - *.ba2"),
		// then per plugin in load order "<plugin> - Main.ba2" and "<plugin> - Textures.ba2", each, when missing, as
		// "<plugin> - Main01.ba2", "...02" and on until one is missing (Starfield.exe 0x1405a8fe0: "%s%s.ba2", "%s%s%02u.ba2")
		bool                     sf = false;
	};

	struct Progress
	{
		std::atomic<std::size_t> done{ 0 }, total{ 0 };
		std::atomic<bool>        cancel{ false };
		void set(const std::string& a)
		{
			std::lock_guard l(_m);
			_cur = a;
		}
		std::string current() const
		{
			std::lock_guard l(_m);
			return _cur;
		}

	private:
		mutable std::mutex _m;
		std::string        _cur;
	};

	struct Source
	{
		std::string mod;      // mod folder ("Game Data", "Overwrite")
		std::string archive;  // the BSA's file name, empty for loose files
		std::string label() const { return archive.empty() ? mod + " (loose)" : mod + " (" + archive + ")"; }
	};

	struct Conflict
	{
		std::string           path;     // lowercase, backslashes
		std::vector<uint32_t> sources;  // indexes into Result::sources, the winner last
	};

	// one mod overriding another's files
	struct Pair
	{
		std::string              winner, loser;  // mod names
		std::size_t              files = 0, scripts = 0;
		std::vector<std::size_t> conflicts;  // indexes into Result::conflicts
	};

	struct Result
	{
		std::vector<Source>      sources;
		std::vector<Conflict>    conflicts;
		std::vector<Pair>        pairs;  // most files first
		std::size_t              files = 0, archives = 0, modsRead = 0, scriptConflicts = 0;
		std::vector<std::string> errors;
		std::string              report(std::size_t a_maxFiles = 30) const;
	};

	Result Find(const Input& a_in, Progress* a_progress = nullptr);
}
