// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Skyrim Special Edition / Anniversary Edition .ess save file.
#pragma once

#include "Papyrus.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ess
{
	enum class Compression : std::uint16_t
	{
		kNone = 0,
		kZlib = 1,
		kLZ4 = 2
	};

	struct Header
	{
		std::string               magic;  // "TESV_SAVEGAME" (Skyrim), "FO4_SAVEGAME" (Fallout 4) or "SFS_SAVEGAME" (Starfield)
		std::uint32_t             version = 0;
		std::uint32_t             saveNumber = 0;
		std::string               playerName;
		std::uint32_t             playerLevel = 0;
		std::string               location;
		std::string               gameDate;
		std::string               race;
		std::uint16_t             sex = 0;
		float                     currentXP = 0, neededXP = 0;
		std::uint64_t             fileTime = 0;
		std::uint32_t             shotWidth = 0, shotHeight = 0;
		Compression               compression = Compression::kLZ4;
		std::vector<std::uint8_t> screenshot;  // SE and Fallout 4: RGBA (4 bytes a pixel); LE: RGB (3)
		// Starfield (docs/CLAUDE-HANDOFF.md, "Starfield"): version holds the engine version (27), then a u8 save version;
		// no screenshot; the bytes after the file time (u32 0, u32 0, u32 1 in every save seen) are kept as read
		std::uint8_t              sfSaveVersion = 0;
		std::vector<std::uint8_t> sfRest;

		bool          isFO4() const { return magic == "FO4_SAVEGAME"; }      // Fallout 4 (and Fallout 4 VR)
		bool          isSF() const { return magic == "SFS_SAVEGAME"; }       // Starfield
		bool          isLE() const { return !isFO4() && !isSF() && version < 12; }  // Skyrim Legendary Edition (save version 9)
		bool          hasCompressionField() const { return !isFO4() && !isLE() && !isSF(); }  // Skyrim SE/AE/VR
		void          read(Reader& r);
		void          write(Writer& w) const;
		std::uint32_t partialSize() const;  // the value stored in the header-size field
		std::uint32_t totalSize() const;    // magic + size field + header + screenshot
	};

	struct PluginInfo
	{
		std::vector<std::string> full;
		std::vector<std::string> lite;
		bool                     hasLite = false;
		// Starfield: medium plugins (form ids FD xx yyyy) and, per plugin (save version 120+: names only before), the
		// Creations details the game writes for files that are not base game files
		struct SfExtra
		{
			std::uint8_t isCreation = 0;
			std::string  title, id, version;
			std::uint8_t achievementFriendly = 0;
		};
		std::vector<std::string>   medium;
		bool                       hasMedium = false;  // Starfield: the medium count is in the save (save version 108+)
		bool                       sfExtras = false;  // entries carry SfExtra
		// Starfield: how the entries carry SfExtra (the loader's plugin entry reader 0x141812e80, by the header's save
		// version): 0 names only (< 120); 1 no flag byte, the details only for files that are not base game files
		// (120..139; the achievement byte from 121: sfAchievementByte); 2 a flag byte, the details when it is set (140+)
		std::uint8_t               sfExtraMode = 0;
		bool                       sfAchievementByte = true;
		std::vector<SfExtra>       sfFull, sfLite, sfMedium;
		bool                       sfHasTail = false;  // the u16 after the lists (1 in every save seen)
		std::uint16_t              sfTail = 0;
	};

	struct FileLocationTable
	{
		std::uint32_t formIDArrayCountOffset = 0, unknownTable3Offset = 0;
		std::uint32_t table1Offset = 0, table2Offset = 0, changeFormsOffset = 0, table3Offset = 0;
		std::uint32_t table1Count = 0, table2Count = 0, table3Count = 0, changeFormCount = 0;
		std::uint32_t unused[15]{};  // Starfield: unused[1] (field 11) = offset of the change form type list (rewritten)
	};

	struct GlobalData
	{
		std::uint32_t             type = 0;
		std::vector<std::uint8_t> raw;      // every block except Papyrus
		std::unique_ptr<Papyrus>  papyrus;  // type 1001
		// Starfield: the layout depends on the type (FORMAT: global data tables)
		enum class SfKind : std::uint8_t
		{
			kSized,      // u32 type, u32 size, data
			kOffset,     // types 0 and 2: u32 type, u32 offset (string table / created-ID section; rewritten)
			kStrings,    // u32 type, vsval n, u16 string index[n], u32 size, data
			kTypeOnly    // u32 type and nothing else
		};
		SfKind                     sfKind = SfKind::kSized;
		std::uint32_t              sfOffset = 0;
		std::vector<std::uint16_t> sfStrings;
		// Starfield Papyrus kept as raw bytes (in raw) while papyrus is an empty stand-in: nothing in it is shown or edited
		bool                       sfRawPapyrus = false;
	};

	enum class ChangeFormType : std::uint8_t
	{
		REFR = 0, ACHR, PMIS, PGRE, PBEA, PFLA, CELL, INFO, QUST, NPC_, ACTI, TACT, ARMO, BOOK, CONT, DOOR, INGR, LIGH, MISC, APPA, STAT, MSTT, FURN, WEAP, AMMO, KEYM, ALCH, IDLM, NOTE, ECZN, CLAS, FACT, PACK, NAVM, WOOP, MGEF, SMQN, SCEN, LCTN, RELA, PHZD, PBAR, PCON, FLST, LVLN, LVLI, LVSP, PARW, ENCH
	};
	const char* ChangeFormTypeName(std::uint8_t a_code, bool a_fo4 = false);  // Fallout 4 numbers its types differently from 28 on
	const char* ChangeFormTypeNameSF(std::uint8_t a_code);  // Starfield 1.16.244 (its own numbering: no APPA, TERM 47...)
	// Starfield: the game's "base game file" test (exe 1.16.244 0x140425850), which decides at save versions 120..139
	// whether a plugin entry carries the Creations details
	bool IsBaseGameFileSF(std::string_view a_name);


	struct ChangeForm
	{
		RefID                     refID;
		std::uint32_t             changeFlags = 0;
		std::uint8_t              typeField = 0;  // low 6 bits type, high 2 bits length width
		std::uint8_t              version = 0;
		std::uint32_t             length1 = 0;  // stored data length
		std::uint32_t             length2 = 0;  // uncompressed length (0 = not compressed)
		std::vector<std::uint8_t> data;
		// Starfield: header flags (1 version, 2 change flags, 8 refID list, 0x10 string list, 0x20 data); the type
		// (typeField) comes from the save's type list; the data is never compressed (length2 0)
		std::uint8_t               sfFlags = 0;
		std::vector<RefID>         sfRefs;     // form references hoisted out of the data
		std::vector<std::uint16_t> sfStrings;  // indices into SaveFile::sfStrings

		std::uint8_t typeCode() const { return typeField & 0x3F; }
	};

	// Starfield: a placed reference's change form (types REFR, ACHR, PMIS, PGRE, PBEA, PFLA, PHZD, PBAR, PCON, PARW, PPLA,
	// PEMI: the exe's reference test 0x140b36310, engine types 0x4A..0x55). Its "initial data" (writer 0x141832fd0, type
	// chosen by 0x141832f00 from the form ID and the change flags) comes first and is what the game needs to make the
	// reference again: 5 = made in game (FF) without flag 0x8000, 6 = flags 0x2000008, 4 = flags 0x6, 7 = flag 0x8000.
	// Every form ID written goes to the change form's reference list (0x141868de0) in writing order:
	//   type 5: [0] the cell or worldspace, [1] the base object (GetBaseObject 0x140b4cd10); position and angle in the data
	//   type 6: [0] the cell or worldspace, [1] the cell or worldspace it was placed in by its plugin (vfunc +0x320)
	//   types 4 and 7: [0] the cell / location, no base object
	// then TESForm::SaveGame (0x1405df980) adds the form's own ID, and TESObjectREFR::SaveGame (0x140b29700) adds the base
	// object (the reference's +0x98) when change flag 0x80 is set and the form is not an actor (ACHR).
	bool IsReferenceChangeFormSF(std::uint8_t a_code);
	int  InitialDataTypeSF(std::uint32_t a_formID, std::uint8_t a_code, std::uint32_t a_changeFlags);
	// the base objects a reference's change form names, best first (GetBaseObject's, then the +0x98 one); empty when it
	// names none (a reference of a plugin whose base did not change, or initial data without one)
	std::vector<RefID> BaseObjectsSF(std::uint32_t a_formID, const ChangeForm& a_cf);

	// Starfield: the start of a quest's change form (TESQuest::SaveGame 0x140d0f2e0, Starfield.exe 1.16.244), read up to the
	// stages; the rest (objectives 0x20000000, running data 0x10000000, ...) is kept as it is:
	//   change flag 0x1: u32 form flags, u16 (TESForm::SaveGame 0x1405df980)
	//   0x2: u32 quest flags (TESQuest +0x114: 0x1 enabled, 0x2 completed: Quest.IsCompleted 0x1420cdc30 tests bit 1;
	//        Quest.IsRunning 0x1420cdca0 is 0x1 set, 0x80 clear and nothing starting or stopping)
	//   0x4: u32 (+0x110)
	//   0x80000000: vsval count, then per stage u16 index (+0x10) and u8 whose bit 0 is "done" (+0x12; IsStageDone 0x140d0dbd0)
	struct SfQuestStage
	{
		std::uint16_t index = 0;
		bool          done = false;
		std::size_t   offset = 0;  // of its flag byte in the change form's data
	};
	struct SfQuestData
	{
		bool                      ok = false;           // the data is long enough for every field above
		bool                      hasFlags = false;
		std::uint32_t             questFlags = 0;
		std::size_t               flagsOffset = 0;
		std::vector<SfQuestStage> stages;
		bool completed() const { return hasFlags && (questFlags & 2); }
		bool enabled() const { return hasFlags && (questFlags & 1); }
	};
	SfQuestData DecodeQuestSF(const ChangeForm& a_cf);  // a QUST change form (type code 8)

	// Starfield: a reference's change form data (version 153), read by SaveSFRefr.cpp from the start to the end of its
	// reference part: initial data, havok (0x4), the actor's head (ACHR), TESObjectREFR::SaveGame (form, extra data,
	// inventory, 0x10000000 block, component data). For a REFR family record that is the whole data; an actor's AI
	// process and the rest of Actor::SaveGame follow and are kept as they are.
	// The inventory (change flags 0x8000020; 0x1406447c0) is the whole list: vsval items; per item FORMID base, vsval
	// stacks, per stack its extra data and u32 count; then u32 and u8.
	struct SfInventoryStack
	{
		std::uint32_t count = 0;
		std::size_t   countOffset = 0;  // in the change form's data
		std::size_t   extrasBegin = 0;
		std::size_t   extraRefs = 0;  // forms its extra data names
	};
	struct SfInventoryItem
	{
		std::size_t                   refIndex = 0;  // the item's base object: sfRefs[refIndex]
		std::size_t                   begin = 0, end = 0;
		std::vector<SfInventoryStack> stacks;
		std::uint32_t                 value20 = 0;  // (the item's +0x20 and +0x24; not known yet)
		std::uint8_t                  value24 = 0;
		std::uint64_t total() const
		{
			std::uint64_t n = 0;
			for (const auto& s : stacks) {
				n += s.count;
			}
			return n;
		}
	};
	struct SfReferenceData
	{
		bool          ok = false;
		std::string   error;
		std::uint32_t actorFlags = 0;
		bool          hasInventory = false;
		std::size_t   inventoryBegin = 0, inventoryEnd = 0;  // bytes
		std::size_t   inventoryRefBegin = 0, inventoryRefEnd = 0;  // indices into sfRefs
		std::size_t   referenceEnd = 0, referenceRefs = 0;
		std::vector<SfInventoryItem> items;
	};
	SfReferenceData DecodeReferenceSF(std::uint32_t a_formID, const ChangeForm& a_cf);
	// Starfield: an NPC_ change form (TESNPC::SaveGame 0x140cdf140, version 153), read to its end; the same layout is
	// inside an actor's extra 0x2b (a leveled actor's base). Offsets of the parts a later editor needs.
	struct SfNpcData
	{
		bool        ok = false;
		std::string error;
		std::size_t statsOffset = SIZE_MAX;  // change flag 0x2: 16 bytes (TESActorBase +0xe8 +8)
		std::size_t factionsOffset = SIZE_MAX, factionRefs = 0, factions = 0;  // 0x40: n x (FORMID, u8 rank)
		std::size_t faceOffset = SIZE_MAX;  // 0x800: the face data (0x140cdd070)
		bool        hasName = false;  // 0x20: its name is in the string list
	};
	SfNpcData DecodeNpcSF(const ChangeForm& a_cf);
	class SaveFile;
	// renames an NPC_ (the player is NPC_ 0x7; its header name follows); false when the record has no name to change
	bool RenameNpcSF(SaveFile& a_save, ChangeForm& a_cf, const std::string& a_name);
	// the save's reference to a form (Starfield.esm IDs below 0x400000 as they are, others through the form table,
	// added there when missing)
	RefID RefIDForFormSF(SaveFile& a_save, std::uint32_t a_formID);
	// adds an item (a_formID in the save's load order, as formIDOf gives) to a reference's inventory: one stack of a_count
	bool AddInventoryItemSF(SaveFile& a_save, ChangeForm& a_cf, std::uint32_t a_formID, std::uint32_t a_count, std::string* a_why = nullptr);
	// changes one stack's count in place (the data keeps its size and its forms)
	bool SetInventoryCountSF(ChangeForm& a_cf, const SfInventoryStack& a_stack, std::uint32_t a_count);

	// Starfield's "BCPS" container: the whole save in zlib chunks (FORMAT: container)
	struct SfContainer
	{
		bool          present = false;  // false: the file was the bare SFS_SAVEGAME data
		std::uint64_t unk10 = 0, maxSize = 0x40000000, chunkSize = 0x40000, alignment = 0x10;
		std::uint8_t  level = 3;
		std::uint8_t  pad[3]{};  // uninitialised padding in the game's writer: kept as read
		char          codec[4]{ 'Z', 'I', 'P', ' ' };
		std::size_t   chunks = 0, storedChunks = 0;
	};

	class SaveFile
	{
	public:
		static std::unique_ptr<SaveFile> load(const std::filesystem::path& a_path);
		void save(const std::filesystem::path& a_path) const;  // also writes the .skse co-save next to it

		// Writes the save (and its .skse co-save) safely:
		//  1. both files are written to temporary files and the new save is read back (a save that cannot
		//     be read is never put in place);
		//  2. with a_backup, the files already there are copied to a matching backup generation first
		//     (name.ess.bak + name.skse.bak, name.ess.bak2 + name.skse.bak2, ...); a failed copy stops
		//     everything before anything is replaced;
		//  3. the save is put in place, then the co-save; if the co-save cannot be, the save is rolled
		//     back to the previous file (or removed, if there was none).
		// Throws std::runtime_error on any failure; then nothing but the temporary files was changed.
		struct WriteResult
		{
			std::filesystem::path backup, cosaveBackup;  // empty when none was made
		};
		WriteResult write(const std::filesystem::path& a_path, bool a_backup) const;
		std::vector<std::uint8_t> serialize() const;  // the whole .ess file

		std::vector<std::uint8_t> buildBody() const;  // uncompressed body, offsets recomputed

		std::filesystem::path     path;
		Header                    header;
		std::uint8_t              formVersion = 0;
		std::string               gameVersion;  // Fallout 4: the game version the save was made with ("1.5.157.0")
		bool                      isLE() const { return header.isLE(); }
		bool                      isFO4() const { return header.isFO4(); }
		// Papyrus string indices are 16-bit: Skyrim LE, and Fallout 4 saves of form version 61 and older
		bool                      str16() const { return isLE() || (isFO4() && formVersion <= 61); }
		bool                      isSF() const { return header.isSF(); }
		// Starfield: the header's save version, which the loader goes by for the plugin entries, the change form version
		// default and the change form type list (its stream byte +0x110, set from the header: 0x141830755)
		std::uint8_t              sfVersion() const { return header.sfSaveVersion ? header.sfSaveVersion : formVersion; }
		const char*               cosaveExt() const { return isSF() ? ".sfse" : isFO4() ? ".f4se" : ".skse"; }
		const char*               saveExt() const { return isSF() ? ".sfs" : isFO4() ? ".fos" : ".ess"; }
		const char*               gameTitle() const { return isSF() ? "Starfield" : isFO4() ? "Fallout 4" : isLE() ? "Skyrim Legendary Edition" : "Skyrim Special Edition"; }
		const char*               cfTypeName(const ChangeForm& a_cf) const { return isSF() ? ChangeFormTypeNameSF(a_cf.typeCode()) : ChangeFormTypeName(a_cf.typeCode(), isFO4()); }
		// Starfield
		std::string                sfCreatedVersion;  // the game version the playthrough started on
		SfContainer                sfContainer;
		std::vector<std::uint8_t>  sfUnknownTable3;   // its data (u32 count, 0 in every save seen)
		std::vector<std::string>   sfStrings;         // the global string table (change forms and global data index it)
		std::vector<std::uint8_t>  sfCreated;         // the created-ID section at the end, kept as read
		bool                       sfPapyrusRaw() const;  // Papyrus kept as raw bytes (it could not be read: sfPapyrusError)
		std::string                sfPapyrusError;        // why the Papyrus section could not be read (it is kept as it is)
		PluginInfo                plugins;
		FileLocationTable         flt;
		std::vector<GlobalData>   table1, table2, table3;
		std::vector<ChangeForm>   changeForms;
		std::vector<std::uint32_t> formIDs;
		std::vector<std::uint32_t> visitedWorldspaces;
		std::vector<std::uint8_t> unknown3;
		std::vector<std::uint8_t> cosave;  // .skse (.f4se for Fallout 4, .sfse for Starfield: kept as is) file contents (empty if none)
		std::size_t               originalBodySize = 0;
		// Structural problems found while reading (layout that does not add up, bad string indices,
		// duplicate ids...). A save with problems may not be written back faithfully: open it read-only.
		std::vector<std::string>  problems;

		Papyrus*       papyrus();
		const Papyrus* papyrus() const { return const_cast<SaveFile*>(this)->papyrus(); }

		// Resolves a RefID to a full form id (0xFF...... for created forms). 0 if unknown.
		std::uint32_t formIDOf(const RefID& a_id) const;
		// Which plugin a form id comes from ("Created" for FF forms, "?" if out of range).
		std::string   pluginOf(std::uint32_t a_formID) const;
		ChangeForm*   findChangeForm(const RefID& a_id);
		void          reindexChangeForms();

	private:
		static std::unique_ptr<SaveFile> loadInner(const std::filesystem::path& a_path, const char*& a_stage);
		static std::unique_ptr<SaveFile> loadSF(const std::filesystem::path& a_path, const std::vector<std::uint8_t>& a_file, const char*& a_stage);
		std::vector<std::uint8_t>         serializeSF() const;
		std::unordered_map<std::uint32_t, std::size_t> _cfIdx;
	};

	// A name for an edited copy of a save that the game still reads correctly. The game takes the
	// character's ID and other details from the name's fields (Save<n>_<character id>_..._<date>_<level>_<n>);
	// text added at the end ("_edited") breaks that, and the saves made after it lose the character ID
	// (00000000) and drop out of the character's group in the load menu. So the tag goes right after the
	// 14-digit date, as letters and digits only ("..._20261025075454edited_66_1.ess"). Names not in the
	// game's format get "_<tag>" at the end. a_unique: the first name not taken (tag, tag2, tag3...).
	// Starfield (.sfs) is different: its date field must stay exactly 14 characters, so the tag goes after the save
	// number of the first field ("Save1471cleaned_2C1EE8CF_..._73_1_4.sfs"; Autosave/Quicksave/Exitsave alike): see Save.cpp.
	std::filesystem::path EditedSaveName(const std::filesystem::path& a_save, const std::string& a_tag, bool a_unique = true);

	// What the save is made of, one line each, for the log and for support requests: format, versions, the
	// plugins, the file location table, the global data blocks, change forms by type, the Papyrus counts and
	// widths, every thread (a_detail), and the problems found.
	std::vector<std::string> DescribeSave(const SaveFile& a_save, bool a_detail);

	// Only the header and screenshot of a save (the first few hundred KB of the file): for the save gallery.
	// False if the file is not a Skyrim save.
	bool ReadSaveHeader(const std::filesystem::path& a_save, Header& a_header);

	// Starfield's BCPS container (SaveSF.cpp): unpack (throws on anything the game would reject) and pack again
	// with the container's settings (zlib at its level, its chunk size and alignment; raw chunks where zlib does
	// not shrink them)
	std::vector<std::uint8_t> UnpackBCPS(const std::vector<std::uint8_t>& a_file, SfContainer& a_info);
	std::vector<std::uint8_t> PackBCPS(const std::vector<std::uint8_t>& a_inner, const SfContainer& a_info);

	std::vector<std::uint8_t> DecompressLZ4(const std::uint8_t* a_src, std::size_t a_srcLen, std::size_t a_dstLen);
	std::vector<std::uint8_t> CompressLZ4(const std::vector<std::uint8_t>& a_src);
	std::vector<std::uint8_t> DecompressZlib(const std::uint8_t* a_src, std::size_t a_srcLen, std::size_t a_dstLen);
	std::vector<std::uint8_t> CompressZlib(const std::vector<std::uint8_t>& a_src);
}
