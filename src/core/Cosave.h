// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Script extender co-saves: the .skse file SKSE (Skyrim LE 1.9.32, SE/AE) writes next to each save and the .f4se
// file F4SE writes for Fallout 4. Read, shown per plugin, and cleaned of the data of plugins no longer installed.
//
// Format (identical in skse/Serialization.cpp (LE, SKSE 1.7.x), skse64/Serialization.cpp and
// f4se/Serialization.cpp; all little-endian u32):
//   Header        { signature 'SKSE' / 'F4SE' (file bytes in that order), formatVersion (1), xseVersion,
//                   runtimeVersion, numPlugins }
//   PluginHeader  { signature = the plugin's unique id (SetUniqueID), numChunks, length (bytes that follow,
//                   chunk headers included) } x numPlugins
//   ChunkHeader   { type, version, length } + length bytes, x numChunks
// The loader (HandleLoadGlobalData) reads plugin headers while 12 bytes remain (numPlugins is not used), hands
// each block to the plugin with that unique id ("plugin with signature %08X not loaded" when none has it) and
// always jumps to block start + length afterwards. The script extender's own data has unique id 0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ess
{
	class ScriptSources;
}

namespace ess::cosave
{
	struct Chunk
	{
		std::uint32_t             type = 0, version = 0;
		std::vector<std::uint8_t> data;
	};

	struct Plugin
	{
		std::uint32_t             uid = 0;
		std::uint32_t             declaredChunks = 0, declaredLength = 0;  // as stored
		std::vector<Chunk>        chunks;
		bool                      regular = true;  // the chunks fill the block exactly and their count matches
		std::vector<std::uint8_t> raw;             // the block as stored (written back as is when !regular)

		std::size_t fileSize() const { return 12 + raw.size(); }  // bytes in the file, header included
		std::size_t chunkCount() const { return regular ? chunks.size() : declaredChunks; }
	};

	struct File
	{
		bool                      fo4 = false;
		std::uint32_t             signature = 0, formatVersion = 0, xseVersion = 0, runtimeVersion = 0, numPlugins = 0;
		std::vector<Plugin>       plugins;
		std::vector<std::uint8_t> trailing;  // fewer than 12 bytes left at the end (the loader ignores them)
		std::vector<std::string>  notes;     // irregularities found while reading
		std::size_t               size = 0;  // bytes read

		const Plugin* find(std::uint32_t a_uid) const;
		std::string   xseName() const { return fo4 ? "F4SE" : "SKSE"; }
	};

	// Throws FormatError when the bytes are not a co-save.
	File                      Parse(const std::vector<std::uint8_t>& a_bytes);
	std::vector<std::uint8_t> Write(const File& a_file);
	bool                      LooksLikeCosave(const std::vector<std::uint8_t>& a_bytes);

	// "SKEE" for a printable four-character id, else hex with the printable part: "424510A2 (BE..)".
	std::string IdText(std::uint32_t a_uid);
	std::string Hex(std::uint32_t a_uid);    // "534B4545"
	std::string FourCC(std::uint32_t a_v);   // the multi-character literal the source wrote: 'SKEE', dots for others
	std::string PackedVersion(std::uint32_t a_v);  // MAKE_EXE_VERSION: "1.6.1170" / "2.2.6"

	// Plugins whose unique id is known from their source (SetUniqueID calls) or from their DLL.
	struct KnownPlugin
	{
		std::uint32_t uid;
		const char*   name;
		const char*   dll;     // the DLL that registers it (file name, may be empty)
		const char*   source;  // where the id comes from
		int           game;    // 0 Skyrim, 1 Fallout 4, 2 both
	};
	const KnownPlugin*              Known(std::uint32_t a_uid, bool a_fo4);
	const std::vector<KnownPlugin>& KnownTable();
	std::string                     ChunkLabel(std::uint32_t a_uid, std::uint32_t a_type, bool a_fo4);  // "" when not known

	// ---- which installed DLL registers which id --------------------------------------------------------------
	// A plugin's DLL passes its id to SetUniqueID as an immediate: x64 "mov edx, imm32" (CommonLib's
	// SetUniqueID(uid), F4SE / old SKSE64 SetUniqueID(handle, uid) "mov r8d, imm32"), x86 (LE) "push imm32".
	// The DLLs are searched for those instructions with the id.
	struct Dll
	{
		std::filesystem::path path;
		std::string           mod;  // mod folder ("" for the game's Data folder)
		bool                  enabled = true;
	};
	// The DLLs in <folder>\SKSE\Plugins (F4SE\Plugins) of every mod folder of the scan, its overwrite folder and
	// the game's Data folder; with no scan, only a_data (a Data folder, a mods folder or a single mod).
	std::vector<Dll> ListPluginDlls(const ScriptSources* a_sources, const std::filesystem::path& a_data, bool a_fo4);
	// uid -> the DLLs that set it. a_x86: Skyrim LE.
	std::map<std::uint32_t, std::vector<std::size_t>> FindIds(const std::vector<Dll>& a_dlls, const std::set<std::uint32_t>& a_uids, bool a_x86,
		std::size_t* a_read = nullptr);

	enum class Status
	{
		kExtender,      // the script extender's own data (unique id 0)
		kInstalled,     // a DLL in an enabled mod (or the Data folder) sets this id
		kDisabled,      // only a DLL in a disabled mod sets it
		kNotInstalled,  // the DLLs were searched and none sets it
		kUnknown        // the DLLs were not searched
	};
	const char* StatusText(Status a_s);

	struct PluginRow
	{
		std::uint32_t            uid = 0;
		std::string              name;    // known name, or the DLL that sets it, or the id
		std::string              source;  // where the name comes from
		Status                   status = Status::kUnknown;
		std::vector<std::string> dlls;    // "Mod folder: X.dll"
		std::size_t              bytes = 0, chunks = 0;
	};
	struct View
	{
		std::vector<PluginRow> rows;  // in file order
		std::size_t            dllsSearched = 0;
		bool                   searched = false;
		std::size_t            notInstalledBytes = 0, notInstalledCount = 0;
	};
	// a_dlls empty: nothing searched, every plugin kUnknown (except id 0).
	View MakeView(const File& a_file, const std::vector<Dll>& a_dlls, bool a_x86);

	// ---- the plugin list SKSE / F4SE / PapyrusUtil keep in a 'PLGN' chunk -------------------------------------
	// u16 count, then per plugin: u8 index, u16 light index when index == 0xFE, u16 name length, name.
	struct PlgnEntry
	{
		std::uint8_t  index = 0;
		std::uint16_t light = 0;
		std::string   name;
	};
	bool ReadPlgn(const Chunk& a_chunk, std::vector<PlgnEntry>& a_out);

	// ---- PapyrusUtil (StorageUtil) -----------------------------------------------------------------------------
	// eeveelo/PapyrusUtil Serialize.cpp / Data.cpp: unique id 884715692 + 227106806 (0x424510A2). Records
	// INTV FLOV STRV FORV (values: obj -> key -> value) and INTL FLOL STRL FORL (lists: obj -> key -> values),
	// version 1, each a text stream: "<count>" then per object " <objKey> <keys>" and per key " <key> <value>"
	// (lists: " <key> <n> <v1> ... <vn>"); objKey = form type << 32 | form id (0 = global), form ids in the
	// load order of the save (its own PLGN record); strings: ' ' -> 0x07, "" -> 0x1B. <count> counts objects with
	// no keys too, which are not written. On load, objects whose form no longer exists are skipped.
	namespace papu
	{
		constexpr std::uint32_t kUid = 884715692u + 227106806u;
		struct Object
		{
			std::uint64_t key = 0;
			std::size_t   keys = 0, values = 0;
			std::size_t   begin = 0, end = 0;  // byte range in the chunk (the space before the key included)
			std::size_t   bytes() const { return end - begin; }
		};
		struct KeyStat
		{
			std::string name;
			std::size_t objects = 0, values = 0, bytes = 0;
		};
		struct Record
		{
			std::uint32_t        type = 0;
			bool                 list = false;
			bool                 ok = false;  // parsed to the end
			std::string          error;
			long long            declared = 0;  // <count>
			std::size_t          countEnd = 0;  // end of the <count> text
			std::vector<Object>  objects;
			std::vector<KeyStat> keys;  // by key name
		};
		bool   IsStorageRecord(std::uint32_t a_type);
		Record ParseRecord(const Chunk& a_chunk);
		// The plugin (from PapyrusUtil's own PLGN) a saved object belongs to; "" for global data and created forms.
		std::string Owner(std::uint64_t a_key, const std::vector<PlgnEntry>& a_plgn);

		struct ModStat
		{
			std::string plugin;  // "(global)" for key 0, "(made in game)" for FF forms
			std::size_t objects = 0, keys = 0, bytes = 0;
		};
		struct Breakdown
		{
			bool                   present = false;
			std::vector<PlgnEntry> plgn;
			struct Rec
			{
				std::uint32_t type = 0;
				std::size_t   bytes = 0, objects = 0, keys = 0, values = 0;
				bool          ok = true;
				std::string   error;
			};
			std::vector<Rec>     records;
			std::vector<ModStat> mods;     // biggest first
			std::vector<KeyStat> keys;     // biggest first (summed over the records)
			struct Big
			{
				std::uint32_t record = 0;
				std::uint64_t key = 0;
				std::string   plugin;
				std::size_t   keys = 0, values = 0, bytes = 0;
			};
			std::vector<Big> objects;  // biggest first
		};
		Breakdown Analyze(const File& a_file);
	}

	// ---- JContainers ('JSTR', SilverIce/JContainers src/jcontainers_constants.h) ------------------------------
	// One 'JSTR' record: a JSON header line ({"commonVersion": N}) then a boost binary archive of the object
	// tree. Only the size and the header are shown.
	namespace jc
	{
		constexpr std::uint32_t kUid = 0x4A535452;  // 'JSTR'
		std::string Header(const File& a_file);    // the JSON header, "" when none
	}

	// ---- cleaning ---------------------------------------------------------------------------------------------
	struct CleanOptions
	{
		std::set<std::uint32_t> removeUids;     // plugin blocks to drop (never id 0)
		std::set<std::string>   gonePlugins;    // lowercase plugin file names: PapyrusUtil objects on their forms are dropped
	};
	struct CleanResult
	{
		std::size_t plugins = 0, pluginBytes = 0;
		std::size_t papuObjects = 0, papuBytes = 0;
		std::size_t before = 0, after = 0;
		std::vector<std::string> lines;
	};
	// Edits a_file. Nothing in it is changed when nothing matches.
	CleanResult Clean(File& a_file, const CleanOptions& a_opt);
	// Parse, Clean, Write and parse the result again (throws if it does not read back the same).
	std::vector<std::uint8_t> CleanBytes(const std::vector<std::uint8_t>& a_bytes, const CleanOptions& a_opt, CleanResult* a_result = nullptr);

	// What removing a plugin's co-save data does (shown in the GUI and the CLI).
	extern const char* kRemoveExplanation;

	// Plain-text report (CLI, AI): header, plugins with size / chunks / status, PapyrusUtil and JContainers parts.
	std::string Report(const File& a_file, const View& a_view, std::size_t a_top, bool a_chunks);
	std::string SizeText(std::size_t a_bytes);  // "1.2 MB", "340 KB", "12 bytes"
}
