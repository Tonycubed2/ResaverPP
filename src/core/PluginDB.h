// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Reads the plugins (.esm/.esp/.esl) a save uses, so forms can be shown by editor ID and name
// (ReSaver's "Parse ESP/ESMs"). Only what is needed for naming is kept: editor ID, display
// name, record type, and the base object of placed references the save mentions.
#pragma once
#include <cctype>
#include <set>

#include "Quests.h"
#include "ScriptSources.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ess
{
	class PluginDB
	{
	public:
		struct Input
		{
			std::vector<std::string>          full, lite;  // the save's plugin lists (load order)
			std::unordered_set<std::uint32_t> wantedRefs;  // runtime form IDs of placed references to keep
			ScriptSources                     sources;     // where the plugin and strings files are
			std::string                       language = "english";  // Fallout 4: "en" (Fallout4_en.strings)
			bool                              fo4 = false;  // Fallout 4 plugins: names, editor IDs, quests (QUST in
			                                                // Fallout 4's layout) and the magic effect / spell data
			                                                // (MGEF, SPEL, ENCH, ALCH, INGR effects)
		};

		struct Progress
		{
			std::atomic<std::size_t> done{ 0 }, total{ 0 };
			std::atomic<bool>        cancel{ false };
			std::mutex               m;
			std::string              current;  // guarded by m
		};

		struct Form
		{
			std::uint32_t edid = UINT32_MAX;  // offsets into the string pool
			std::uint32_t name = UINT32_MAX;
			std::uint32_t base = 0;           // NAME of a placed reference (runtime form ID)
			std::uint16_t plugin = 0;         // index into plugins(): last plugin that defines it
			char          type[4]{};
			std::uint8_t  spellType = 0xFF;   // SPEL: 0 spell, 1 disease, 2 power, 3 lesser power, 4 ability, 5 poison,
			                                  // 10 addiction, 11 voice... (0xFF: not read). Fallout 4 also: ENCH its
			                                  // enchant type (ENIT +0x14: 6 enchantment), ALCH 7
			std::uint8_t  castType = 0xFF;    // Fallout 4 SPEL (SPIT +0x10) / ENCH (ENIT +8): 0 constant effect, 1 fire
			                                  // and forget, ...; ALCH 1
			bool          poison = false;     // Fallout 4 ALCH: ENIT flags (+4) bit 0x20000
			// ARMO: the biped slots it takes, the first u32 of BOD2 (Skyrim SE and Fallout 4) or BODT (Skyrim LE-format
			// records; xEdit tes5 / wbDefinitionsFO4 "First Person Flags": bit 0 = slot 30 ...). 0 when not read.
			std::uint32_t bipedSlots = 0;
			bool          hasBiped = false;
			// ARMO / WEAP: its enchantment (EITM, runtime form id; 0 none)
			std::uint32_t enchantment = 0;
		};

		// Reads every plugin of the save that can be found. Safe to call on a worker thread.
		void load(const Input& a_in, Progress& a_progress);

		bool                     loaded() const { return !_forms.empty(); }
		std::size_t              size() const { return _forms.size(); }
		const Form*              find(std::uint32_t a_formID) const;
		std::string              edid(const Form& f) const { return str(f.edid); }
		std::string              name(const Form& f) const { return str(f.name); }
		std::string              type(const Form& f) const { return std::string(f.type, 4); }
		const std::string&       pluginName(const Form& f) const { return _plugins[f.plugin]; }
		const std::vector<std::string>& plugins() const { return _plugins; }

		// "EditorID 'Name' [TYPE]" (whatever is known), empty if the form is unknown.
		std::string describe(std::uint32_t a_formID) const;
		// Forms whose editor ID or name contains the text (case-insensitive). Exact hex form IDs
		// match too.
		std::vector<std::uint32_t> search(const std::string& a_text, std::size_t a_max) const;
		// Every form of one record type ("SPEL"), as the last plugin that defines it has it.
		std::vector<std::uint32_t> ofType(const std::string& a_type) const;

		// QUST records (the last override of each), with stages, journal entries, objectives and
		// stage scripts: what the Quests view shows
		const quests::QuestDef* quest(std::uint32_t a_formID) const
		{
			auto it = _quests.find(a_formID);
			return it == _quests.end() ? nullptr : &it->second;
		}
		const std::unordered_map<std::uint32_t, quests::QuestDef>& questDefs() const { return _quests; }

		// Magic effects (MGEF DATA) and the effects of spells, enchantments, potions, ingredients and scrolls
		// (EFID + EFIT), as the last plugin that defines them has them: what an active effect does.
		struct MagicEffectDef
		{
			std::uint32_t flags = 0;
			std::uint32_t archetype = 0xFFFFFFFF;  // 0 value modifier, 34 peak value modifier, ...
			std::int32_t  actorValue = -1;         // primary actor value
		};
		struct EffectItem
		{
			std::uint32_t effect = 0;  // MGEF runtime form ID
			float         magnitude = 0.0f;
			std::uint32_t area = 0, duration = 0;
		};
		const MagicEffectDef* magicEffect(std::uint32_t a_formID) const
		{
			auto it = _mgef.find(a_formID);
			return it == _mgef.end() ? nullptr : &it->second;
		}
		const std::vector<EffectItem>* effects(std::uint32_t a_formID) const
		{
			auto it = _effects.find(a_formID);
			return it == _effects.end() ? nullptr : &it->second;
		}

		// CELL records, as the last plugin in load order that overrides each has them (a deleted override removes the cell).
		// The game's TESObjectCELL keeps DATA in +0x40 (bit 0: interior, Fallout4.exe 1.11.240 0x4c28d1; ResetInterior's
		// 0x4c9330 tests it) and XCLC's x / y in its exterior data (0x4c2ac9); the worldspace is the one whose World Children
		// group (group type 1) holds the record.
		struct CellDef
		{
			std::uint16_t flags = 0;          // DATA (bit 0: interior)
			bool          hasData = false;
			bool          hasGrid = false;    // XCLC read
			std::int32_t  x = 0, y = 0;       // XCLC
			std::uint32_t worldspace = 0;     // runtime form id of the WRLD holding it (0: not in a worldspace's group)
			bool          persistent = false; // record flag 0x400 in a worldspace: the worldspace's persistent cell
			std::uint16_t plugin = 0;         // index into plugins(): the last plugin that overrides it
			bool          interior() const { return hasData && (flags & 1); }
		};
		const CellDef* cell(std::uint32_t a_formID) const
		{
			auto it = _cells.find(a_formID);
			return it == _cells.end() ? nullptr : &it->second;
		}
		// the exterior cell at a grid position of a worldspace (not its persistent cell); 0 when no plugin has one
		std::uint32_t cellAt(std::uint32_t a_worldspace, std::int32_t a_x, std::int32_t a_y) const;
		std::size_t   cellCount() const { return _cells.size(); }

		std::vector<std::string> missing;  // plugins of the save whose file was not found
		// True only for a plugin that was found and read to the end (not missing, no read error): only then does
		// "the form is not in it" mean the plugin really no longer has the form. a_plugin: any letter case.
		bool wasRead(const std::string& a_plugin) const
		{
			std::string l = a_plugin;
			for (auto& c : l) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return _readOk.contains(l);
		}
		std::vector<std::string> errors;
		std::size_t              pluginsRead = 0, recordsRead = 0;

	private:
		std::set<std::string> _readOk;  // lower-case names of the plugins read completely
		std::string str(std::uint32_t off) const { return off == UINT32_MAX ? std::string() : std::string(_pool.data() + off); }
		std::uint32_t addString(const std::string& s);

		std::unordered_map<std::uint32_t, Form> _forms;
		std::unordered_map<std::uint32_t, CellDef> _cells;
		std::unordered_map<std::uint64_t, std::uint32_t> _cellAt;  // (worldspace, x, y) key -> cell
		std::unordered_map<std::uint32_t, quests::QuestDef> _quests;
		std::unordered_map<std::uint32_t, MagicEffectDef>   _mgef;
		std::unordered_map<std::uint32_t, std::vector<EffectItem>> _effects;
		std::vector<char>                       _pool;
		std::vector<std::string>                _plugins;
	};

	class SaveFile;
	// Fills Input::full/lite/wantedRefs from a save (every form the save refers to).
	void PrepareInput(const SaveFile& a_save, PluginDB::Input& a_in);

	// Text from a plugin or strings file: kept if valid UTF-8, otherwise read as Windows-1252.
	std::string GameTextToUtf8(const std::string& a_text);
}
