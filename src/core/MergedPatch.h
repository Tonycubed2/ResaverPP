// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Merged patch: reads every plugin of the load order, finds records that two or more plugins
// override, and writes one plugin that combines their changes, the way Mator Smash and a manual
// xEdit patch do:
//  - the winning override is copied into the patch;
//  - every other override is compared with the version its own plugin was built on (the last
//    version among that plugin's masters), and what it added to or removed from a list is applied;
//  - a mod becomes a master of the patch only when a patched record comes from it, or a value in a
//    patched record points at one of its forms (like xEdit's "copy as override").
// Lists (leveled lists, inventories, factions, spells, perks, keywords, form lists, outfits) are merged
// entry by entry; every other part of a record value by value (stage 2), parts that belong together as
// one block. Types: leveled lists, containers, form lists, outfits, NPCs, armour, weapons, ammo, misc
// items, keys, soul gems, books, potions, ingredients, scrolls, spells, and cells (interior and exterior,
// with their worldspace copied in as the parent).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ess
{
	class ScriptSources;
}

namespace ess::merge
{
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

	// What kinds of changes are merged. Each has a risk, for the page where players choose what to merge:
	// 0 safe (green), 1 read first (yellow), 2 risky (red).
	enum class Category : std::uint8_t
	{
		kLeveledLists,   // entries of leveled items, NPCs and spells
		kContainers,     // container items
		kOutfits,        // outfit items
		kFormLists,      // form list entries
		kKeywords,       // keywords on items and NPCs
		kNpcLists,       // NPC factions, spells, perks and items
		kNpcFace,        // an NPC's face and race
		kNpcValues,      // NPC stats, AI, skills, outfits, voice, class...
		kItemValues,     // value, weight, damage, armour rating, effect costs...
		kEffects,        // magic effects of potions, ingredients, scrolls and spells (with their conditions)
		kNames,          // names, descriptions, editor IDs, bounds
		kModels,         // models, icons, destruction data
		kScripts,        // scripts attached to records (VMAD) with their properties
		kCellLighting,   // a cell's lighting and lighting template
		kCellWater,      // a cell's water (height, type, textures, currents)
		kCellRegions,    // the regions a cell belongs to
		kCellOther,      // a cell's location, owner, encounter zone, music, acoustics, image space, flags...
		kRenumberFix,    // references to a compacted light plugin's old form numbers, pointed at its new ones
		kCount
	};
	const char*  CategoryName(Category a_cat);
	int          CategoryRisk(Category a_cat);
	const char*  CategoryWhy(Category a_cat);     // what can go wrong, for the menu's hover text
	const char*  CategoryDetail(Category a_cat);  // everything it covers, record by record

	struct Options
	{
		bool enabled[static_cast<int>(Category::kCount)] = { true, true, true, true, true, true, true, true, true, true, true, true, true,
			true, true, true, true, true };  // which kinds of changes are merged
		std::vector<std::string> loadOrder;  // enabled plugins, in load order
		// where a plugin file is (empty: not found, the plugin is left out and reported)
		std::function<std::optional<std::filesystem::path>(const std::string&)> findPlugin;
		// the bytes of a strings file by lowercase name ("skyrim_english.strings"); empty if none
		std::function<std::vector<std::uint8_t>(const std::string&)> readStrings;
		std::string language = "english";
		std::string patchName = "ReSaverPP Merged Patch.esp";
	};

	struct MergedRecord
	{
		std::string              type, edid;
		std::string              id;       // "Skyrim.esm:0x01A2B3"
		std::string              winner;   // the plugin whose version the patch starts from
		std::vector<std::string> changes;  // "ModA.esp: +2 entries", ...
		std::vector<Category>    categories;  // the kind of each change
		int                      risk = 0;    // the highest risk of its changes
	};

	struct Result
	{
		bool                      ok = false;
		std::string               error;
		std::vector<std::uint8_t> file;  // the patch plugin
		std::vector<std::string>  masters;
		std::map<std::string, std::string> masterWhy;  // master -> a record that needs it
		std::vector<MergedRecord> merged;
		std::vector<std::string>  skipped;  // records with conflicts that could not be merged, and why
		std::vector<std::string>  missing;  // plugins of the load order whose file was not found
		std::vector<std::string>  warnings;  // problems of the load order itself, found while reading it
		std::vector<std::string>  fixes;     // problems of the load order the patch fixes
		std::size_t               pluginsRead = 0, recordsRead = 0, conflicts = 0;

		std::string report() const;
	};

	Result Build(const Options& a_opt, Progress* a_progress = nullptr);

	// Finds plugins and strings files (loose or in BSAs) through a scan of the mods (MO2 or Vortex) or
	// the game's Data folder. a_sources must outlive the build.
	void UseSources(Options& a_opt, const ScriptSources& a_sources);

	// The load order from a plugins.txt (Mod Organizer profile or the game's own): the plugins the
	// game always loads first (base game masters, then the Creation Club files in Skyrim.ccc), then
	// the enabled lines in file order.
	std::vector<std::string> ReadLoadOrder(const std::filesystem::path& a_pluginsTxt, const std::filesystem::path& a_gameFolder);

	// Low level, for tests: one record of a plugin and a minimal plugin writer.
	struct Sub
	{
		char                      sig[4];
		std::vector<std::uint8_t> data;
	};
	struct Record
	{
		char             sig[4];
		std::uint32_t    flags = 0, formID = 0;
		std::uint16_t    version = 44;
		std::vector<Sub> subs;
		// cells: the groups the record sits in below its top group, outermost first, as (group type, label):
		// interior cells (2 block, 3 sub-block); exterior cells (1 worldspace = its form ID, 4 block, 5 sub-block)
		std::vector<std::pair<std::int32_t, std::uint32_t>> groups;
	};
	std::vector<std::uint8_t> WritePlugin(const std::vector<std::string>& a_masters, std::uint32_t a_headerFlags,
		const std::vector<Record>& a_records, const std::string& a_author = "ReSaver Plus", const std::string& a_desc = {},
		float a_hedrVersion = 1.7f);
	struct PluginFile
	{
		std::vector<std::string> masters;
		std::uint32_t            flags = 0;
		std::vector<Record>      records;  // top-level records of the wanted types (all types if a_types is empty)
	};
	PluginFile ReadPlugin(const std::filesystem::path& a_file, const std::vector<std::string>& a_types = {});
}
