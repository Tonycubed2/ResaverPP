// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Starfield plugins for names (PluginDB::load with Input::sf): editor ID, display name, record type, and the base object of
// the placed references the save mentions.
//
// What was checked on Starfield 1.16.244's own files (Tony's install, docs/CLAUDE-HANDOFF.md "Starfield names"), and
// confirmed by the game's own plugin loader (scratchpad sf2/LOADER.md section 5: Starfield.exe 0x1405b8653, 0x1405b62a0):
//  - records, groups and subrecords are Fallout 4's (24-byte headers, flag 0x40000 = u32 size + zlib, XXXX for long
//    subrecords); TES4 flags 0x80 localized, 0x100 light, 0x400 medium (SFBGS003 / SFBGS006, the save's 2 medium plugins);
//  - FORM IDS INSIDE A PLUGIN: the top byte is not a plain master index. 00..FC = index among the plugin's FULL masters
//    (the plugin's own records when it is a full plugin: index = the number of full masters); FD xx yyyy = medium plugin
//    xx among its MEDIUM masters (own records: xx = number of medium masters); FE xxx yyy = light plugin xxx among its
//    LIGHT masters (own: xxx = number of light masters). Seen: SFBGS003 (medium, master Starfield) FD00xxxx, SFBGS004
//    (light) FE000xxx, SFBGS047 (light; masters starfield, sfbgs004 (light), sfbgs00d) its own records FE001xxx and
//    overrides of sfbgs00d with top byte 01, BlueprintShips-SFBGS050 (full; 3 full masters) own records 03xxxxxx. The
//    reader counts the ids this rule cannot place (sfUnresolved) and the overrides of forms no earlier plugin has
//    (sfOrphanOverrides): both 0 on every plugin of Tony's install;
//  - FULL of a localized plugin is a u32 id into <plugin>_<language>.strings, found loose in Data\Strings or in a BA2 (v2):
//    "Starfield - Localization.ba2", but the official Creations keep theirs in "<plugin> - Main.ba2", ShatteredSpace in
//    "ShatteredSpace - Main02.ba2" and BlueprintShips-SFBGS050 inside "SFBGS050 - Main.ba2": so every general archive of
//    the Data folders is looked in (big sound / animation archives last), not only ones named after the plugin.
// Starfield.esm is 1.46 GB: plugins are read as a stream (8 MB at a time), placed references the save does not mention
// are skipped without reading their data, and the strings files are read only for the plugins being read.
#include "PluginDB.h"

#include "Bsa.h"
#include "Save.h"
#include "Text.h"

#include "miniz.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace ess
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		std::string U8(const std::filesystem::path& p)
		{
			const auto s = p.u8string();
			return std::string(s.begin(), s.end());
		}

		std::uint32_t U32(const std::uint8_t* p)
		{
			std::uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
		}

		bool Is(const std::uint8_t* t, const char* s) { return std::memcmp(t, s, 4) == 0; }

		std::string Fmt8(std::uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof(b), "%08X", v);
			return b;
		}

		bool IsPlacedRef(const std::uint8_t* t)
		{
			static const char* refs[] = { "REFR", "ACHR", "PGRE", "PMIS", "PHZD", "PARW", "PBAR", "PBEA", "PCON", "PFLA" };
			for (const char* r : refs) {
				if (Is(t, r)) {
					return true;
				}
			}
			return false;
		}

		// no names in them, and many / large
		bool Skip(const std::uint8_t* t) { return Is(t, "LAND") || Is(t, "NAVM") || Is(t, "NAVI"); }

		// a file read front to back in big pieces; take() returns the next bytes (valid until the next call)
		class Stream
		{
		public:
			explicit Stream(const std::filesystem::path& a_path) :
				_f(a_path, std::ios::binary)
			{
				if (_f) {
					_f.seekg(0, std::ios::end);
					_size = static_cast<std::uint64_t>(_f.tellg());
					_f.seekg(0);
				}
				_buf.resize(8u << 20);
			}
			bool          ok() const { return static_cast<bool>(_f); }
			std::uint64_t size() const { return _size; }
			std::uint64_t pos() const { return _base + _at; }
			const std::uint8_t* take(std::size_t n)
			{
				if (_len - _at < n && !refill(n)) {
					return nullptr;
				}
				const auto* p = _buf.data() + _at;
				_at += n;
				return p;
			}
			void skip(std::uint64_t n)
			{
				if (_len - _at >= n) {
					_at += static_cast<std::size_t>(n);
					return;
				}
				const auto target = pos() + n;
				_base = target;
				_at = _len = 0;
				_f.clear();
				_f.seekg(static_cast<std::streamoff>(target));
			}

		private:
			bool refill(std::size_t n)
			{
				const auto rem = _len - _at;
				std::memmove(_buf.data(), _buf.data() + _at, rem);
				_base += _at;
				_at = 0;
				_len = rem;
				if (_buf.size() < n) {
					_buf.resize(n);
				}
				_f.clear();
				_f.read(reinterpret_cast<char*>(_buf.data() + _len), static_cast<std::streamsize>(_buf.size() - _len));
				_len += static_cast<std::size_t>(_f.gcount());
				return _len >= n;
			}
			std::ifstream             _f;
			std::vector<std::uint8_t> _buf;
			std::size_t               _at = 0, _len = 0;
			std::uint64_t             _base = 0, _size = 0;
		};

		struct PluginHeader
		{
			bool                     ok = false;
			std::uint32_t            flags = 0;
			std::vector<std::string> masters;  // as written
		};

		PluginHeader ReadHeader(Stream& a_s)
		{
			PluginHeader h;
			const auto* t = a_s.take(24);
			if (!t || !Is(t, "TES4")) {
				return h;
			}
			h.flags = U32(t + 8);
			const auto size = U32(t + 4);
			const auto* d = a_s.take(size);
			if (!d) {
				return h;
			}
			std::uint32_t big = 0;
			for (std::size_t p = 0; p + 6 <= size;) {
				const auto*   st = d + p;
				std::uint16_t l16;
				std::memcpy(&l16, st + 4, 2);
				const std::uint32_t len = big ? big : l16;
				big = 0;
				p += 6;
				if (p + len > size) {
					break;
				}
				if (Is(st, "XXXX") && len == 4) {
					big = U32(d + p);
				} else if (Is(st, "MAST")) {
					h.masters.emplace_back(reinterpret_cast<const char*>(d + p), strnlen(reinterpret_cast<const char*>(d + p), len));
				}
				p += len;
			}
			h.ok = true;
			return h;
		}

		// the first a_want bytes of a zlib stream (the whole of it when it is shorter): the names are at the start of a record,
		// and Starfield.esm's biggest compressed records (SFTR, PNDT, some CELL: 2 GB unpacked) need not be unpacked whole
		bool InflatePrefix(const std::uint8_t* a_src, std::size_t a_len, std::size_t a_want, std::vector<std::uint8_t>& a_out)
		{
			a_out.resize(a_want);
			mz_stream z{};
			if (mz_inflateInit(&z) != MZ_OK) {
				return false;
			}
			z.next_in = a_src;
			z.avail_in = static_cast<unsigned int>(a_len);
			z.next_out = a_out.data();
			z.avail_out = static_cast<unsigned int>(a_want);
			int r = MZ_OK;
			while (z.avail_out > 0 && (r = mz_inflate(&z, MZ_NO_FLUSH)) == MZ_OK) {
			}
			const auto got = a_want - z.avail_out;
			mz_inflateEnd(&z);
			if (r != MZ_OK && r != MZ_STREAM_END && r != MZ_BUF_ERROR) {
				return false;
			}
			a_out.resize(got);
			return got > 0;
		}

		// archives with nothing but sounds, voices, animations, meshes or textures: looked in last
		bool BulkArchive(const std::string& a_lower)
		{
			for (const char* k : { " - voices", " - wwisesounds", " - faceanimation", " - animations", " - meshes", " - terrain", " - lod",
					 " - planetdata", " - densitymaps", " - materials", " - particles", " - shaders", " - generatedtextures" }) {
				if (a_lower.find(k) != std::string::npos) {
					return true;
				}
			}
			return false;
		}
	}

	const std::vector<std::string>& StarfieldBaseFiles()
	{
		static const std::vector<std::string> v = { "starfield.esm", "shatteredspace.esm", "sfbgs00d.esm", "sfbgs050.esm", "blueprintships-starfield.esm",
			"blueprintships-sfbgs050.esm", "constellation.esm", "oldmars.esm", "sfbgs004.esm", "sfbgs007.esm", "sfbgs008.esm", "sfbgs047.esm", "sfbgs003.esm",
			"sfbgs006.esm" };
		return v;
	}

	bool IsStarfieldBaseFile(const std::string& a_lower)
	{
		const auto& v = StarfieldBaseFiles();
		return std::find(v.begin(), v.end(), a_lower) != v.end();
	}

	void PluginDB::loadStarfield(const Input& a_in, Progress& a_progress)
	{
		using Clock = std::chrono::steady_clock;
		_forms.clear();
		_cells.clear();
		_cellAt.clear();
		_quests.clear();
		_mgef.clear();
		_effects.clear();
		_pool.clear();
		_plugins.clear();
		_readOk.clear();
		missing.clear();
		errors.clear();
		pluginsRead = recordsRead = 0;
		sfUnresolved = sfOrphanOverrides = sfStringsFiles = sfArchivesRead = 0;
		sfSamples.clear();
		sfBytes = 0;
		sfMsStrings = sfMsPlugins = sfMsInflate = 0;
		sfInflated = 0;
		_sf = true;

		// the runtime form id of every plugin: full idx << 24, light FE + idx << 12, medium FD + idx << 16
		enum class Kind : std::uint8_t
		{
			kFull,
			kLight,
			kMedium
		};
		struct Slot
		{
			Kind          kind;
			std::uint32_t index;
		};
		auto runtime = [](const Slot& s, std::uint32_t low) -> std::uint32_t {
			switch (s.kind) {
			case Kind::kLight: return 0xFE000000u | (s.index << 12) | (low & 0xFFF);
			case Kind::kMedium: return 0xFD000000u | (s.index << 16) | (low & 0xFFFF);
			default: return (s.index << 24) | (low & 0xFFFFFF);
			}
		};
		std::vector<std::pair<std::string, Slot>> order;
		for (std::size_t i = 0; i < a_in.full.size(); ++i) {
			order.push_back({ a_in.full[i], { Kind::kFull, static_cast<std::uint32_t>(i) } });
		}
		for (std::size_t i = 0; i < a_in.lite.size(); ++i) {
			order.push_back({ a_in.lite[i], { Kind::kLight, static_cast<std::uint32_t>(i) } });
		}
		for (std::size_t i = 0; i < a_in.medium.size(); ++i) {
			order.push_back({ a_in.medium[i], { Kind::kMedium, static_cast<std::uint32_t>(i) } });
		}
		std::unordered_map<std::string, Slot> slots;
		for (const auto& [n, s] : order) {
			slots.emplace(Lower(n), s);
		}

		// ---- the files: every Data folder listed once (the first folder that has a file wins)
		std::unordered_map<std::string, std::filesystem::path> files;   // lowercase name -> plugin or archive
		std::vector<std::filesystem::path>                     archives;  // .ba2, in folder order
		std::unordered_map<std::string, std::filesystem::path> looseStrings;
		for (const auto& dir : a_in.sfDataDirs) {
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
				const auto name = Lower(U8(e.path().filename()));
				if (e.is_directory(ec)) {
					if (name == "strings") {
						for (const auto& s : std::filesystem::directory_iterator(e.path(), ec)) {
							looseStrings.emplace(Lower(U8(s.path().filename())), s.path());
						}
					}
					continue;
				}
				if (files.emplace(name, e.path()).second && name.ends_with(".ba2")) {
					archives.push_back(e.path());
				}
			}
		}

		// ---- strings files: the language asked for, then English, then any
		const auto t0 = Clock::now();
		std::vector<std::string> langs{ Lower(a_in.language.empty() ? std::string("en") : a_in.language) };
		if (langs.front() != "en") {
			langs.push_back("en");
		}
		struct Loc
		{
			std::filesystem::path file;
			std::string           entry;  // empty: loose
		};
		std::unordered_map<std::string, Loc> found;  // "<stem>_<lang>.strings" -> where
		std::unordered_set<std::string>      stemsNeeded;
		for (const auto& [n, s] : order) {
			stemsNeeded.insert(Lower(U8(text::PathFrom(n).stem())));
		}
		auto wanted = [&](const std::string& a_name) {  // "strings\x_en.strings" or "x_en.strings"
			auto n = a_name;
			if (const auto sl = n.find_last_of('\\'); sl != std::string::npos) {
				n = n.substr(sl + 1);
			}
			if (!n.ends_with(".strings")) {
				return std::string();
			}
			const auto us = n.rfind('_');
			if (us == std::string::npos || !stemsNeeded.contains(n.substr(0, us))) {
				return std::string();
			}
			return n;
		};
		for (const auto& [n, p] : looseStrings) {
			if (const auto k = wanted(n); !k.empty()) {
				found.emplace(k, Loc{ p, {} });
			}
		}
		auto haveAll = [&] {
			for (const auto& st : stemsNeeded) {
				bool any = false;
				for (const auto& l : langs) {
					any = any || found.contains(st + "_" + l + ".strings");
				}
				if (!any) {
					return false;
				}
			}
			return true;
		};
		if (!haveAll()) {
			// archives named after a plugin of the load order and localization archives first, then the other general ones,
			// the bulk ones last; stop as soon as every plugin has its strings
			std::vector<std::pair<int, std::filesystem::path>> ranked;
			for (const auto& a : archives) {
				const auto low = Lower(U8(a.filename()));
				const auto dash = low.find(" - ");
				const bool named = dash != std::string::npos && stemsNeeded.contains(low.substr(0, dash));
				const bool bulk = BulkArchive(low);
				const int  rank = low.find("localization") != std::string::npos ? 0 : named && !bulk ? 1 : !bulk ? 2 : 3;
				ranked.push_back({ rank, a });
			}
			std::stable_sort(ranked.begin(), ranked.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
			for (const auto& [rank, a] : ranked) {
				if (a_progress.cancel || haveAll()) {
					break;
				}
				// texture archives cannot hold strings: not opened
				{
					std::ifstream f(a, std::ios::binary);
					char          h[12]{};
					if (!f.read(h, 12) || std::memcmp(h, "BTDX", 4) != 0 || std::memcmp(h + 8, "GNRL", 4) != 0) {
						continue;
					}
				}
				try {
					BsaArchive bsa(a);
					++sfArchivesRead;
					for (const auto& e : bsa.entries()) {
						if (e.path.rfind("strings\\", 0) != 0) {
							continue;
						}
						if (const auto k = wanted(e.path); !k.empty()) {
							found.emplace(k, Loc{ a, e.path });
						}
					}
				} catch (const std::exception& e) {
					errors.push_back(U8(a.filename()) + ": " + e.what());
				}
			}
		}
		sfMsStrings = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		auto loadStrings = [&](const std::string& a_stem, const std::string& a_plugin) {
			std::unordered_map<std::uint32_t, std::string> table;
			std::vector<std::string>                       tryLangs = langs;
			for (const auto& [k, loc] : found) {
				if (k.rfind(a_stem + "_", 0) == 0) {
					auto l = k.substr(a_stem.size() + 1, k.size() - a_stem.size() - 1 - 8);
					if (std::find(tryLangs.begin(), tryLangs.end(), l) == tryLangs.end()) {
						tryLangs.push_back(l);
					}
				}
			}
			std::sort(tryLangs.begin() + static_cast<std::ptrdiff_t>(langs.size()), tryLangs.end());
			for (const auto& l : tryLangs) {
				const auto it = found.find(a_stem + "_" + l + ".strings");
				if (it == found.end()) {
					continue;
				}
				try {
					std::vector<std::uint8_t> data;
					if (it->second.entry.empty()) {
						std::ifstream f(it->second.file, std::ios::binary | std::ios::ate);
						data.resize(static_cast<std::size_t>(f.tellg()));
						f.seekg(0);
						f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
					} else {
						BsaArchive bsa(it->second.file);
						if (const auto* e = bsa.find(it->second.entry)) {
							data = bsa.extract(*e);
						}
					}
					table = ReadStringsTable(data);
				} catch (const std::exception& e) {
					errors.push_back(a_plugin + " strings: " + e.what());
				}
				if (!table.empty()) {
					++sfStringsFiles;
					const unsigned cp = text::LanguageCodePage(l);
					for (auto& [id, t] : table) {
						t = text::ToUtf8(t, cp);
					}
					break;
				}
			}
			return table;
		};

		// ---- progress in MB of the plugin files
		// ---- the order the plugins are read in (the last one read wins an override). The save lists full, light and medium
		// plugins apart, not how they interleave in the game's load order; a plugin loads after its masters, so each one is
		// read after its masters (otherwise in the save's order: full, light, medium). [U: how the game interleaves light /
		// medium plugins that do not depend on each other; it only matters when two of them override the same form]
		{
			std::vector<std::vector<std::string>> mastersOf(order.size());
			for (std::size_t i = 0; i < order.size(); ++i) {
				if (auto it = files.find(Lower(order[i].first)); it != files.end()) {
					Stream s(it->second);
					for (const auto& m : ReadHeader(s).masters) {
						mastersOf[i].push_back(Lower(m));
					}
				}
			}
			std::vector<bool>                         placed(order.size(), false);
			std::unordered_set<std::string>           done;
			std::vector<std::pair<std::string, Slot>> sorted;
			sorted.reserve(order.size());
			std::size_t from = 0;  // every plugin before this one is placed
			while (sorted.size() < order.size()) {
				std::size_t pick = order.size();
				for (std::size_t i = from; i < order.size(); ++i) {
					if (placed[i]) {
						continue;
					}
					bool ready = true;
					for (const auto& m : mastersOf[i]) {
						ready = ready && (done.contains(m) || !slots.contains(m));
					}
					if (ready) {
						pick = i;
						break;
					}
				}
				if (pick == order.size()) {  // (masters in a loop: in the save's order)
					pick = from;
					while (placed[pick]) {
						++pick;
					}
				}
				placed[pick] = true;
				done.insert(Lower(order[pick].first));
				sorted.push_back(order[pick]);
				while (from < order.size() && placed[from]) {
					++from;
				}
			}
			order = std::move(sorted);
		}

		std::vector<std::filesystem::path> paths(order.size());
		std::uint64_t                      totalBytes = 0;
		for (std::size_t i = 0; i < order.size(); ++i) {
			if (auto it = files.find(Lower(order[i].first)); it != files.end()) {
				paths[i] = it->second;
				std::error_code ec;
				const auto      sz = std::filesystem::file_size(it->second, ec);
				totalBytes += ec ? 0 : sz;
			}
		}
		a_progress.total = static_cast<std::size_t>(totalBytes >> 20) + 1;
		a_progress.done = 0;
		std::uint64_t doneBytes = 0;

		// a master's kind: from the save's lists; one the save does not have (cannot load, but read the file then)
		// (Starfield.exe 0x1405b70ac: ".esl" forces light unless 0x200; 0x200 is cleared with 0x100 / 0x400 or no masters)
		auto masterSlot = [&](const std::string& a_master, Kind& a_kind, bool& a_overlay) -> const Slot* {
			a_overlay = false;
			if (auto it = slots.find(Lower(a_master)); it != slots.end()) {
				a_kind = it->second.kind;
				return &it->second;
			}
			const auto low = Lower(a_master);
			a_kind = low.ends_with(".esl") ? Kind::kLight : Kind::kFull;
			if (auto it = files.find(low); it != files.end()) {
				Stream s(it->second);
				const auto h = ReadHeader(s);
				a_kind = (h.flags & 0x100) ? Kind::kLight : (h.flags & 0x400) ? Kind::kMedium : Kind::kFull;
				a_overlay = (h.flags & 0x200) && !(h.flags & 0x500) && !h.masters.empty();
				if (low.ends_with(".esl") && !a_overlay) {
					a_kind = Kind::kLight;
				}
			}
			return nullptr;
		};

		const auto tp = Clock::now();
		_plugins.reserve(order.size());
		std::vector<std::uint8_t> inflated;
		for (std::size_t pi = 0; pi < order.size(); ++pi) {
			if (a_progress.cancel) {
				break;
			}
			const auto& [pluginName, self] = order[pi];
			_plugins.push_back(pluginName);
			const auto plugIdx = static_cast<std::uint16_t>(_plugins.size() - 1);
			{
				std::lock_guard l(a_progress.m);
				a_progress.current = pluginName;
			}
			if (paths[pi].empty()) {
				missing.push_back(pluginName);
				continue;
			}
			Stream st(paths[pi]);
			const auto hdr = st.ok() ? ReadHeader(st) : PluginHeader{};
			if (!hdr.ok) {
				missing.push_back(pluginName);
				doneBytes += st.size();
				continue;
			}
			try {
				const bool localized = hdr.flags & 0x80;
				std::vector<const Slot*> fullM, lightM, mediumM;
				for (const auto& m : hdr.masters) {
					Kind k;
					bool overlay = false;
					const auto* s = masterSlot(m, k, overlay);
					if (overlay) {
						continue;  // (0x200: not counted among the full masters; the exe's wording is [U] about where they go)
					}
					(k == Kind::kLight ? lightM : k == Kind::kMedium ? mediumM : fullM).push_back(s);
				}
				// (null in a list: a master the save does not load; ids of it resolve to nothing)
				auto toRuntime = [&](std::uint32_t raw, std::uint32_t& out) {
					if (raw >= 1 && raw <= 0x7FF) {  // (the exe: ids 1..0x7FF are absolute, the engine's own forms)
						out = raw;
						return true;
					}
					const auto top = raw >> 24;
					const Slot* s = nullptr;
					if (top == 0xFE) {
						const auto i = (raw >> 12) & 0xFFF;
						s = i < lightM.size() ? lightM[i] : (i == lightM.size() && self.kind == Kind::kLight) ? &self : nullptr;
					} else if (top == 0xFD) {
						const auto i = (raw >> 16) & 0xFF;
						s = i < mediumM.size() ? mediumM[i] : (i == mediumM.size() && self.kind == Kind::kMedium) ? &self : nullptr;
					} else {
						s = top < fullM.size() ? fullM[top] : (top == fullM.size() && self.kind == Kind::kFull) ? &self : nullptr;
					}
					if (!s) {
						return false;
					}
					out = runtime(*s, raw);
					return true;
				};
				std::unordered_map<std::uint32_t, std::string> strings;
				if (localized) {
					strings = loadStrings(Lower(U8(text::PathFrom(pluginName).stem())), pluginName);
				}
				const std::uint32_t selfTop = runtime(self, 0) & (self.kind == Kind::kLight ? 0xFFFFF000u : self.kind == Kind::kMedium ? 0xFFFF0000u : 0xFF000000u);
				const std::uint32_t selfMask = self.kind == Kind::kLight ? 0xFFFFF000u : self.kind == Kind::kMedium ? 0xFFFF0000u : 0xFF000000u;
				std::uint64_t       lastReport = 0;
				const std::uint64_t base = doneBytes;
				while (const auto* h = st.take(24)) {
					if (st.pos() - lastReport > (16u << 20)) {
						lastReport = st.pos();
						a_progress.done = static_cast<std::size_t>((base + lastReport) >> 20);
						if (a_progress.cancel) {
							break;
						}
					}
					if (Is(h, "GRUP")) {
						continue;  // descend into the group
					}
					const auto dataSize = U32(h + 4), flags = U32(h + 8), rawID = U32(h + 12);
					std::uint8_t type[4];
					std::memcpy(type, h, 4);
					if (Skip(type) || (flags & 0x20)) {
						st.skip(dataSize);
						continue;
					}
					std::uint32_t id;
					if (!toRuntime(rawID, id)) {
						if (++sfUnresolved <= 8) {
							sfSamples.push_back("raw " + Fmt8(rawID) + " " + std::string(reinterpret_cast<const char*>(type), 4) + " in " + pluginName + " (not placed)");
						}
						st.skip(dataSize);
						continue;
					}
					const bool placed = IsPlacedRef(type);
					if (placed && !a_in.wanted().contains(id)) {
						st.skip(dataSize);
						continue;
					}
					const std::uint8_t* d = st.take(dataSize);
					if (!d) {
						break;
					}
					std::size_t n = dataSize;
					if (flags & 0x40000) {  // compressed
						if (n < 4) {
							continue;
						}
						const auto ti = Clock::now();
						if (!InflatePrefix(d + 4, n - 4, std::min<std::size_t>(U32(d), a_in.sfPrefix), inflated)) {
							continue;
						}
						sfMsInflate += std::chrono::duration<double, std::milli>(Clock::now() - ti).count();
						++sfInflated;
						d = inflated.data();
						n = inflated.size();
					}
					// an override (not this plugin's own id) of a form no earlier plugin defines: the id rules would be off
					if ((id & selfMask) != selfTop && !placed && !_forms.contains(id)) {
						if (++sfOrphanOverrides <= 8) {
							sfSamples.push_back(Fmt8(id) + " " + std::string(reinterpret_cast<const char*>(type), 4) + " in " + pluginName);
						}
					}
					std::string   edid, full;
					std::uint32_t baseObj = 0, big = 0;
					for (std::size_t p = 0; p + 6 <= n;) {
						const auto*   sr = d + p;
						std::uint16_t l16;
						std::memcpy(&l16, sr + 4, 2);
						const std::uint32_t len = big ? big : l16;
						big = 0;
						p += 6;
						if (p + len > n) {
							break;
						}
						if (Is(sr, "XXXX") && len == 4) {
							big = U32(d + p);
						} else if (Is(sr, "EDID")) {
							edid.assign(reinterpret_cast<const char*>(d + p), strnlen(reinterpret_cast<const char*>(d + p), len));
						} else if (Is(sr, "FULL") && full.empty()) {
							if (localized && len == 4) {
								if (auto it = strings.find(U32(d + p)); it != strings.end()) {
									full = it->second;
								}
							} else {
								full.assign(reinterpret_cast<const char*>(d + p), strnlen(reinterpret_cast<const char*>(d + p), len));
							}
						} else if (placed && Is(sr, "NAME") && len == 4) {
							std::uint32_t b;
							if (toRuntime(U32(d + p), b)) {
								baseObj = b;
							}
						}
						p += len;
					}
					auto& f = _forms[id];
					std::memcpy(f.type, type, 4);
					f.plugin = plugIdx;
					if (!edid.empty()) {
						f.edid = addString(GameTextToUtf8(edid));
					}
					if (!full.empty()) {
						f.name = addString(GameTextToUtf8(full));
					}
					if (baseObj) {
						f.base = baseObj;
					}
					++recordsRead;
				}
				if (!a_progress.cancel) {
					++pluginsRead;
					_readOk.insert(Lower(pluginName));
				}
			} catch (const std::exception& e) {
				errors.push_back(pluginName + ": " + e.what());
			}
			doneBytes += st.size();
			sfBytes += st.size();
			a_progress.done = static_cast<std::size_t>(doneBytes >> 20);
		}
		sfMsPlugins = std::chrono::duration<double, std::milli>(Clock::now() - tp).count();
	}
}
