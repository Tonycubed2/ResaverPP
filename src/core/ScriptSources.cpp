// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "ScriptSources.h"

#include "Bsa.h"
#include "ByteIO.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <fstream>
#include <set>
#include <cstring>

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

		bool IsPlugin(const std::string& lowerExt) { return lowerExt == ".esp" || lowerExt == ".esm" || lowerExt == ".esl"; }
	}

	std::vector<std::string> ListBSA(const std::filesystem::path& a_bsa)
	{
		BsaArchive               bsa(a_bsa);
		std::vector<std::string> out;
		out.reserve(bsa.entries().size());
		for (const auto& e : bsa.entries()) {
			out.push_back(e.path);
		}
		return out;
	}

	void ScriptSources::scanFolder(const std::filesystem::path& a_dir, const std::string& a_modName, bool a_enabled)
	{
		if (a_enabled) {
			enabledFolders.insert(a_modName);
		}
		std::error_code          ec;
		std::vector<std::string> plugins;
		std::vector<std::filesystem::path> bsas;
		const bool vanillaOnly = gameDataVanillaOnly && a_modName == "Game Data";
		auto       vanillaArchive = [](const std::string& n) {
            for (const char* v : { "update.bsa", "dawnguard.bsa", "hearthfires.bsa", "dragonborn.bsa", "_resourcepack.bsa", "marketplacetextures.bsa" }) {
                if (n == v) {
                    return true;
                }
            }
            return n.rfind("skyrim - ", 0) == 0 || n.rfind("cc", 0) == 0;
		};
		for (const auto& e : std::filesystem::directory_iterator(a_dir, ec)) {
			if (!e.is_regular_file(ec)) {
				continue;
			}
			const auto ext = Lower(U8(e.path().extension()));
			if (IsPlugin(ext)) {
				plugins.push_back(Lower(U8(e.path().filename())));
			} else if (ext == ".bsa" && (!vanillaOnly || vanillaArchive(Lower(U8(e.path().filename()))))) {
				bsas.push_back(e.path());
			}
		}
		for (const auto& p : plugins) {
			pluginFolder.emplace(p, a_modName);
		}

		auto add = [&](const std::string& scriptLower, bool bsa, const std::filesystem::path& archive = {}, const std::string& shown = {}) {
			auto& v = _byScript[scriptLower];
			for (const auto& x : v) {
				if (x.mod == a_modName) {
					return;
				}
			}
			scriptsByMod[a_modName].push_back(shown.empty() ? scriptLower : shown);
			v.push_back({ a_modName, plugins, bsa, archive, a_enabled });
			++pexFound;
		};

		// loose scripts: <mod>\Scripts\*.pex (folder name case varies); loose Strings\*.strings
		for (const auto& e : std::filesystem::directory_iterator(a_dir, ec)) {
			if (e.is_directory(ec) && (Lower(U8(e.path().filename())) == "source" || Lower(U8(e.path().filename())) == "scripts")) {
				// Source\Scripts\*.psc (SE layout) or Scripts\Source\*.psc (LE layout)
				const char* sub = Lower(U8(e.path().filename())) == "source" ? "scripts" : "source";
				for (const auto& d : std::filesystem::directory_iterator(e.path(), ec)) {
					if (d.is_directory(ec) && Lower(U8(d.path().filename())) == sub) {
						bool any = false;
						for (const auto& s : std::filesystem::directory_iterator(d.path(), ec)) {
							if (s.is_regular_file(ec) && Lower(U8(s.path().extension())) == ".psc") {
								const auto key = Lower(U8(s.path().stem()));
								if (sourceFiles.emplace(key, s.path()).second) {
									sourceMods.emplace(key, a_modName);
								}
								any = true;
							}
						}
						if (any) {
							sourceDirs.push_back({ d.path(), a_enabled });
						}
					}
				}
			}
			if (e.is_directory(ec) && Lower(U8(e.path().filename())) == "strings") {
				for (const auto& s : std::filesystem::directory_iterator(e.path(), ec)) {
					if (s.is_regular_file(ec) && Lower(U8(s.path().extension())).ends_with("strings")) {
						stringsFiles.emplace(Lower(U8(s.path().filename())), AssetLocation{ s.path(), {} });
					}
				}
			}
			if (!e.is_directory(ec) || Lower(U8(e.path().filename())) != "scripts") {
				continue;
			}
			for (const auto& s : std::filesystem::directory_iterator(e.path(), ec)) {
				if (s.is_regular_file(ec) && Lower(U8(s.path().extension())) == ".pex") {
					add(Lower(U8(s.path().stem())), false, {}, U8(s.path().stem()));
				}
			}
		}
		for (const auto& b : bsas) {
			try {
				for (const auto& f : ListBSA(b)) {
					if (f.rfind("scripts\\", 0) == 0 && f.size() > 12 && f.ends_with(".pex") && f.find('\\', 8) == std::string::npos) {
						add(f.substr(8, f.size() - 12), true, b);
					} else if (f.rfind("strings\\", 0) == 0 && f.ends_with("strings")) {
						stringsFiles.emplace(f.substr(8), AssetLocation{ b, f });
					}
				}
				++bsasRead;
			} catch (const std::exception& e) {
				errors.push_back(U8(b.filename()) + ": " + e.what());
			}
		}
	}

	void ScriptSources::scan(const std::filesystem::path& a_root, const std::vector<std::string>* a_modOrder, const std::filesystem::path& a_overwrite,
		const std::filesystem::path& a_gameData, ScanProgress* a_progress)
	{
		auto step = [&] {
			if (a_progress) {
				++a_progress->done;
			}
		};
		auto cancelled = [&] { return a_progress && a_progress->cancel.load(); };
		_byScript.clear();
		pluginFolder.clear();
		scriptsByMod.clear();
		errors.clear();
		enabledFolders.clear();
		stringsFiles.clear();
		sourceFiles.clear();
		sourceMods.clear();
		sourceDirs.clear();
		modsScanned = disabledScanned = pexFound = bsasRead = 0;
		root = a_root;
		ranked = false;
		gameDataScanned = false;
		overwriteDir = a_overwrite;
		gameData = a_gameData;

		std::error_code ec;
		if (!std::filesystem::is_directory(a_root, ec)) {
			errors.push_back("not a folder: " + U8(a_root));
			return;
		}
		if (a_progress) {
			std::size_t n = 2;  // overwrite + the game's Data folder
			for (const auto& e : std::filesystem::directory_iterator(a_root, ec)) {
				n += e.is_directory(ec) ? 1 : 0;
			}
			a_progress->total = n;
		}
		if (a_modOrder && !a_modOrder->empty()) {
			// MO2 priority order: overwrite first, then modlist.txt from the top. The first
			// provider recorded for a script is therefore the one the game loads.
			ranked = true;
			if (!a_overwrite.empty() && std::filesystem::is_directory(a_overwrite, ec)) {
				scanFolder(a_overwrite, "Overwrite");
				++modsScanned;
				step();
			}
			std::set<std::string> listed;
			for (const auto& name : *a_modOrder) {
				listed.insert(name);
				const auto dir = a_root / std::filesystem::path(std::u8string(name.begin(), name.end()));
				if (std::filesystem::is_directory(dir, ec)) {
					scanFolder(dir, name);
					++modsScanned;
				}
				step();
				if (cancelled()) {
					return;
				}
			}
			// the game's own Data folder (vanilla BSAs) has the lowest priority of the loaded files
			scanGameData();
			step();
			// disabled mods after that: only credited for scripts nothing loaded provides
			std::vector<std::filesystem::path> disabled;
			for (const auto& e : std::filesystem::directory_iterator(a_root, ec)) {
				if (e.is_directory(ec) && !listed.contains(U8(e.path().filename()))) {
					disabled.push_back(e.path());
				}
			}
			std::sort(disabled.begin(), disabled.end());
			for (const auto& d : disabled) {
				scanFolder(d, U8(d.filename()), false);
				++disabledScanned;
				step();
				if (cancelled()) {
					return;
				}
			}
			return;
		}
		// A Data folder has Scripts\ or plugins directly inside; an MO2 mods folder has one
		// sub-folder per mod.
		bool looksLikeData = false;
		for (const auto& e : std::filesystem::directory_iterator(a_root, ec)) {
			const auto n = Lower(U8(e.path().filename()));
			if (n == "skyrim.esm" || (e.is_directory(ec) && n == "scripts")) {
				looksLikeData = true;
				break;
			}
		}
		if (looksLikeData) {
			scanFolder(a_root, "Data");
			modsScanned = 1;
			gameDataScanned = true;
			return;
		}
		for (const auto& e : std::filesystem::directory_iterator(a_root, ec)) {
			if (e.is_directory(ec)) {
				scanFolder(e.path(), U8(e.path().filename()));
				++modsScanned;
				step();
				if (cancelled()) {
					return;
				}
			}
		}
		scanGameData();
	}

	void ScriptSources::scanGameData()
	{
		std::error_code ec;
		if (!gameData.empty() && std::filesystem::is_directory(gameData, ec)) {
			scanFolder(gameData, "Game Data");
			gameDataScanned = true;
		}
	}

	std::filesystem::path ScriptSources::folderPath(const std::string& a_folder) const
	{
		if (a_folder == "Overwrite") {
			return overwriteDir;
		}
		if (a_folder == "Game Data") {
			return gameData;
		}
		if (a_folder == "Data") {
			return root;
		}
		return root / std::filesystem::path(std::u8string(a_folder.begin(), a_folder.end()));
	}

	std::optional<std::filesystem::path> ScriptSources::pluginPath(const std::string& a_plugin) const
	{
		auto it = pluginFolder.find(Lower(a_plugin));
		if (it == pluginFolder.end()) {
			return std::nullopt;
		}
		const auto dir = folderPath(it->second);
		std::error_code ec;
		// exact name first, then a case-insensitive match
		auto p = dir / std::filesystem::path(std::u8string(a_plugin.begin(), a_plugin.end()));
		if (std::filesystem::exists(p, ec)) {
			return p;
		}
		for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
			if (Lower(U8(e.path().filename())) == Lower(a_plugin)) {
				return e.path();
			}
		}
		return std::nullopt;
	}

	namespace
	{
		std::string NormPath(std::string s)
		{
			for (auto& c : s) {
				c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			while (!s.empty() && s.back() == '/') {
				s.pop_back();
			}
			return s;
		}

		std::string IniValue(std::string v)
		{
			// @ByteArray(x) -> x ; "\\" -> "\\" once
			if (v.rfind("@ByteArray(", 0) == 0 && v.back() == ')') {
				v = v.substr(11, v.size() - 12);
			}
			std::string out;
			for (std::size_t i = 0; i < v.size(); ++i) {
				if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') {
					++i;
				}
				out.push_back(v[i]);
			}
			return out;
		}

		std::optional<Mo2Instance> ReadIni(const std::filesystem::path& a_ini)
		{
			std::ifstream f(a_ini);
			if (!f) {
				return std::nullopt;
			}
			Mo2Instance m;
			m.ini = a_ini;
			m.base = a_ini.parent_path();
			std::string mods, profiles, overwrite, game, line;
			bool        general = false, settings = false;
			while (std::getline(f, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (!line.empty() && line.front() == '[') {
					general = line == "[General]";
					settings = line == "[Settings]";
					continue;
				}
				const auto eq = line.find('=');
				if (eq == std::string::npos) {
					continue;
				}
				const auto key = line.substr(0, eq);
				const auto val = IniValue(line.substr(eq + 1));
				if (general && key == "gameName") {
					m.gameName = val;
				} else if (general && key == "selected_profile") {
					m.selectedProfile = val;
				} else if (general && key == "gamePath") {
					game = val;
				} else if (settings && key == "base_directory") {
					m.base = std::filesystem::path(std::u8string(val.begin(), val.end()));
				} else if (settings && key == "mod_directory") {
					mods = val;
				} else if (settings && key == "profiles_directory") {
					profiles = val;
				} else if (settings && key == "overwrite_directory") {
					overwrite = val;
				}
			}
			auto resolve = [&](std::string v, const char* def) {
				if (v.empty()) {
					return m.base / def;
				}
				if (auto p = v.find("%BASE_DIR%"); p != std::string::npos) {
					v.replace(p, 10, U8(m.base));
				}
				return std::filesystem::path(std::u8string(v.begin(), v.end()));
			};
			m.modsDir = resolve(mods, "mods");
			m.profilesDir = resolve(profiles, "profiles");
			m.overwriteDir = resolve(overwrite, "overwrite");
			if (!game.empty()) {
				m.gamePath = std::filesystem::path(std::u8string(game.begin(), game.end()));
			}
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(m.profilesDir, ec)) {
				if (e.is_directory(ec)) {
					m.profiles.push_back(U8(e.path().filename()));
				}
			}
			std::sort(m.profiles.begin(), m.profiles.end());
			return m;
		}
	}

	std::optional<Mo2Instance> ReadMo2Ini(const std::filesystem::path& a_ini) { return ReadIni(a_ini); }

	std::vector<Mo2Instance> FindMo2Instances(const std::vector<std::filesystem::path>& a_extraDirs)
	{
		std::vector<std::filesystem::path> inis;
		std::error_code                    ec;
		if (const char* la = std::getenv("LOCALAPPDATA")) {
			for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(la) / "ModOrganizer", ec)) {
				inis.push_back(e.path() / "ModOrganizer.ini");
			}
		}
		for (auto d : a_extraDirs) {
			for (int up = 0; up < 4 && !d.empty(); ++up) {
				inis.push_back(d / "ModOrganizer.ini");
				if (d.parent_path() == d) {
					break;
				}
				d = d.parent_path();
			}
		}
		std::vector<Mo2Instance> out;
		std::vector<std::string> seen;
		for (const auto& ini : inis) {
			if (!std::filesystem::exists(ini, ec)) {
				continue;
			}
			auto m = ReadIni(ini);
			if (!m) {
				continue;
			}
			const auto key = NormPath(U8(m->modsDir));
			if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
				continue;
			}
			seen.push_back(key);
			out.push_back(std::move(*m));
		}
		return out;
	}

	std::optional<Mo2Instance> FindMo2Instance(const std::filesystem::path& a_modsFolder)
	{
		std::vector<std::filesystem::path> inis;
		inis.push_back(a_modsFolder.parent_path() / "ModOrganizer.ini");
		if (const char* la = std::getenv("LOCALAPPDATA")) {
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(la) / "ModOrganizer", ec)) {
				inis.push_back(e.path() / "ModOrganizer.ini");
			}
		}
		const auto want = NormPath(U8(a_modsFolder));
		for (const auto& ini : inis) {
			std::error_code ec;
			if (!std::filesystem::exists(ini, ec)) {
				continue;
			}
			if (auto m = ReadIni(ini); m && NormPath(U8(m->modsDir)) == want) {
				return m;
			}
		}
		return std::nullopt;
	}

	std::vector<std::string> LoadModList(const std::filesystem::path& a_profileDir)
	{
		std::vector<std::string> out;
		std::ifstream            f(a_profileDir / "modlist.txt");
		for (std::string line; std::getline(f, line);) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.size() > 1 && line[0] == '+') {
				out.push_back(line.substr(1));
			}
		}
		return out;
	}

	const std::vector<ScriptProvider>* ScriptSources::providers(const std::string& a_script) const
	{
		auto it = _byScript.find(Lower(a_script));
		return it == _byScript.end() ? nullptr : &it->second;
	}

	std::vector<std::string> ScriptSources::scriptNames() const
	{
		std::vector<std::string> out;
		out.reserve(_byScript.size() + sourceFiles.size());
		for (const auto& [k, v] : _byScript) {
			out.push_back(k);
		}
		for (const auto& [k, v] : sourceFiles) {
			if (!_byScript.contains(k)) {
				out.push_back(k);
			}
		}
		return out;
	}

	std::optional<ScriptSources::PexFile> ScriptSources::findPex(const std::string& a_script) const
	{
		const auto* list = providers(a_script);
		if (!list || list->empty()) {
			return std::nullopt;
		}
		const auto      want = Lower(a_script) + ".pex";
		std::error_code ec;
		// the copy the game loads first (providers are in priority order), then any other copy
		for (const auto& pr : *list) {
			const auto dir = folderPath(pr.mod);
			if (!pr.fromBSA) {
				for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
					if (!e.is_directory(ec) || Lower(U8(e.path().filename())) != "scripts") {
						continue;
					}
					for (const auto& f : std::filesystem::directory_iterator(e.path(), ec)) {
						if (f.is_regular_file(ec) && Lower(U8(f.path().filename())) == want) {
							return PexFile{ pr.mod, f.path(), {} };
						}
					}
				}
				continue;
			}
			// the archive was recorded during the scan: open only that one (a mod or the game's Data folder
			// can hold many large BSAs)
			if (!pr.bsa.empty()) {
				return PexFile{ pr.mod, pr.bsa, "scripts\\" + want };
			}
		}
		return std::nullopt;
	}

	std::vector<std::uint8_t> ScriptSources::readPex(const PexFile& a_pex)
	{
		if (a_pex.bsaEntry.empty()) {
			std::ifstream in(a_pex.file, std::ios::binary);
			if (!in) {
				throw std::runtime_error("cannot open " + U8(a_pex.file));
			}
			return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		}
		BsaArchive bsa(a_pex.file);
		const auto* e = bsa.find(a_pex.bsaEntry);
		if (!e) {
			throw std::runtime_error(a_pex.bsaEntry + " is not in " + U8(a_pex.file.filename()));
		}
		return bsa.extract(*e);
	}
}
