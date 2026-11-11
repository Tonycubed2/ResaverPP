// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Skyrim Special Edition / Anniversary Edition .ess save file.
#pragma once

#include "Papyrus.h"

#include <filesystem>
#include <memory>
#include <string>
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
		// Starfield: medium plugins (form ids FD xx yyyy) and, per plugin (form version 140+: names only before), the
		// Creations details the game writes for files that are not base game files
		struct SfExtra
		{
			std::uint8_t isCreation = 0;
			std::string  title, id, version;
			std::uint8_t achievementFriendly = 0;
		};
		std::vector<std::string>   medium;
		bool                       hasMedium = false;
		bool                       sfExtras = false;  // entries carry SfExtra
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
		std::vector<std::uint8_t> cosave;  // .skse (.f4se for Fallout 4) file contents (empty if none)
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
