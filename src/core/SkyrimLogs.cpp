// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "SkyrimLogs.h"

#include "CrashLog.h"
#include "Extras.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace ess::logs
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

		std::string Trim(const std::string& s)
		{
			std::size_t a = 0, b = s.size();
			while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) {
				++a;
			}
			while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) {
				--b;
			}
			return s.substr(a, b - a);
		}

		std::string U8(const std::filesystem::path& p)
		{
			const auto s = p.u8string();
			return std::string(s.begin(), s.end());
		}

		std::string Name(const std::filesystem::path& p) { return U8(p.filename()); }

		// reads at most a_max bytes: the end of a bigger file (logs grow at the end)
		std::string ReadText(const std::filesystem::path& a_file, std::uintmax_t a_max = 8u << 20)
		{
			std::ifstream f(a_file, std::ios::binary);
			if (!f) {
				return {};
			}
			f.seekg(0, std::ios::end);
			const auto size = static_cast<std::uintmax_t>(f.tellg());
			const auto from = size > a_max ? size - a_max : 0;
			f.seekg(static_cast<std::streamoff>(from));
			std::string s(static_cast<std::size_t>(size - from), '\0');
			f.read(s.data(), static_cast<std::streamsize>(s.size()));
			s.resize(static_cast<std::size_t>(f.gcount()));
			if (from > 0) {
				const auto nl = s.find('\n');
				s.erase(0, nl == std::string::npos ? 0 : nl + 1);  // a cut line
			}
			return s;
		}

		std::vector<std::string> Lines(const std::string& a_text)
		{
			std::vector<std::string> v;
			std::istringstream       in(a_text);
			for (std::string l; std::getline(in, l);) {
				if (!l.empty() && l.back() == '\r') {
					l.pop_back();
				}
				v.push_back(std::move(l));
			}
			return v;
		}

		std::string Age(std::int64_t a_minutes)
		{
			char buf[64];
			if (a_minutes < 2) {
				return "just now";
			}
			if (a_minutes < 120) {
				std::snprintf(buf, sizeof(buf), "%lld minutes ago", static_cast<long long>(a_minutes));
			} else if (a_minutes < 48 * 60) {
				std::snprintf(buf, sizeof(buf), "%lld hours ago", static_cast<long long>(a_minutes / 60));
			} else {
				std::snprintf(buf, sizeof(buf), "%lld days ago", static_cast<long long>(a_minutes / (60 * 24)));
			}
			return buf;
		}

		// the DLL / exe file name in a path ("C:\...\Data\SKSE\Plugins\\po3_Tweaks.dll" -> po3_Tweaks.dll)
		std::string DllIn(const std::string& a_line)
		{
			static const std::regex re(R"(([^\\/:*?"<>|\s()]+\.dll))", std::regex::icase);
			std::smatch             m;
			return std::regex_search(a_line, m, re) ? m[1].str() : std::string();
		}

		// numbers and form ids out, so repeats of one message group together
		std::string Normalize(const std::string& a_line)
		{
			static const std::regex stamp(R"(^\s*\[[^\]]*\d{1,2}:\d{2}[^\]]*\]\s*)");  // a leading time stamp
			static const std::regex numbers(R"(0x[0-9A-Fa-f]+|\b[0-9A-Fa-f]{8}\b|\b\d+(\.\d+)?\b)");
			auto s = std::regex_replace(a_line, stamp, "");
			s = std::regex_replace(s, numbers, "#");
			return Trim(s);
		}

		const std::set<std::string>& SystemModules()
		{
			static const std::set<std::string> s = { "skyrimse.exe", "skyrimvr.exe", "tesv.exe", "ntdll.dll", "kernelbase.dll",
				"kernel32.dll", "ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "user32.dll", "win32u.dll",
				"combase.dll", "rpcrt4.dll", "msvcrt.dll" };
			return s;
		}

		struct KnownCrash
		{
			const char* where;    // lower case "skyrimse.exe+0cec680"
			const char* game;     // part of the game version line ("1.6.1170"), or "" for any
			const char* title;
			const char* detail;
		};
		const KnownCrash kKnown[] = {
			{ "skyrimse.exe+0cec680", "1.6.1170", "Known crash: a character saved waiting for a path idle",
				"The game crashed reading a character's path (MovementAgentPathFollowerStandard) that was saved while it waited for a "
				"start/stop/turn idle; the save keeps the waiting state but not the idle. If it happens while a save loads, open that "
				"save in ReSaver Plus: the Save check finds these characters and fixes them (or resaverpp-cli --fix-path-idles). Path "
				"Idle Crash Fix by Yuof (Nexus SE 196861) prevents it in the game." },
		};
	}

	const char* KindName(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::kSkse: return "SKSE";
		case Kind::kCrash: return "crash log";
		case Kind::kPapyrus: return "Papyrus log";
		default: return "plugin log";
		}
	}

	Kind KindOf(const std::filesystem::path& a_file)
	{
		const auto n = Lower(Name(a_file));
		static const std::set<std::string> skse = { "skse64.log", "skse.log", "sksevr.log", "skse64_loader.log", "skse_loader.log",
			"sksevr_loader.log", "skse64_steam_loader.log", "skse_steam_loader.log" };
		if (skse.contains(n)) {
			return Kind::kSkse;
		}
		if (n.rfind("crash", 0) == 0) {
			return Kind::kCrash;
		}
		if (n.rfind("papyrus.", 0) == 0) {
			return Kind::kPapyrus;
		}
		return Kind::kPlugin;
	}

	std::vector<std::filesystem::path> Folders(const std::vector<std::filesystem::path>& a_myGames,
		const std::vector<std::filesystem::path>& a_extra)
	{
		std::vector<std::filesystem::path> v;
		auto add = [&](const std::filesystem::path& p) {
			std::error_code ec;
			if (std::filesystem::is_directory(p, ec) && std::find(v.begin(), v.end(), p) == v.end()) {
				v.push_back(p);
			}
		};
		for (const auto& mg : a_myGames) {
			for (const char* g : { "Skyrim Special Edition", "Skyrim Special Edition GOG", "Skyrim Special Edition EPIC", "Skyrim VR",
					 "Skyrim" }) {
				add(mg / g / "SKSE");
				add(mg / g / "Logs" / "Script");
			}
		}
		for (const auto& e : a_extra) {
			add(e);
		}
		return v;
	}

	std::vector<LogFile> Find(const std::vector<std::filesystem::path>& a_folders)
	{
		std::vector<LogFile> out;
		std::set<std::filesystem::path> seen;
		const auto now = std::filesystem::file_time_type::clock::now();
		for (const auto& folder : a_folders) {
			// the game: the folder under My Games ("...\My Games\Skyrim Special Edition\SKSE")
			std::string game;
			for (auto p = folder; p.has_parent_path() && p.parent_path() != p; p = p.parent_path()) {
				if (Lower(Name(p.parent_path())) == "my games") {
					game = Name(p);
					break;
				}
			}
			std::error_code ec;
			std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end;
			for (; !ec && it != end; it.increment(ec)) {
				if (it.depth() >= 2) {
					it.disable_recursion_pending();
				}
				if (!it->is_regular_file(ec)) {
					continue;
				}
				const auto ext = Lower(U8(it->path().extension()));
				const auto kind = KindOf(it->path());
				if (ext != ".log" && !(ext == ".txt" && kind == Kind::kCrash)) {
					continue;
				}
				if (!seen.insert(it->path()).second) {
					continue;
				}
				LogFile f;
				f.path = it->path();
				f.game = game;
				f.kind = kind;
				f.size = it->file_size(ec);
				f.time = it->last_write_time(ec);
				f.ageMinutes = std::chrono::duration_cast<std::chrono::minutes>(now - f.time).count();
				out.push_back(std::move(f));
			}
		}
		std::sort(out.begin(), out.end(), [](const LogFile& a, const LogFile& b) { return a.time > b.time; });
		return out;
	}

	// ---- SKSE's own log ----------------------------------------------------------------------

	std::vector<Finding> ReadSkseLog(const std::filesystem::path& a_file, const std::string& a_text)
	{
		std::vector<Finding> out;
		const auto lines = Lines(a_text);
		std::string version;
		for (std::size_t i = 0; i < lines.size(); ++i) {
			const auto& l = lines[i];
			const auto  low = Lower(l);
			if (version.empty() && low.find("runtime: initialize") != std::string::npos) {
				version = Trim(l);
			}
			Finding f;
			f.file = a_file;
			f.line = i + 1;
			f.examples = { Trim(l) };
			const auto dll = DllIn(l);
			if (low.find("couldn't load plugin") != std::string::npos || low.find("could not load plugin") != std::string::npos ||
				low.find("couldn't load") != std::string::npos) {
				static const std::regex code(R"((?:error|last error)\s*=?\s*(\d+))", std::regex::icase);
				std::smatch m;
				const int   err = std::regex_search(l, m, code) ? std::atoi(m[1].str().c_str()) : 0;
				f.severity = Severity::kBad;
				f.title = "SKSE could not load " + (dll.empty() ? std::string("a plugin") : dll);
				switch (err) {
				case 126:
					f.detail = "Windows error 126: a DLL this plugin needs is missing. Usually another mod it depends on, or the "
					           "Visual C++ redistributable (2015-2022, x64). Check the plugin's requirements on its mod page.";
					break;
				case 193:
					f.detail = "Windows error 193: not a DLL for this game. A Legendary Edition (32-bit) plugin in a Special Edition "
					           "game or the other way round, or a damaged download. Install the version made for your game.";
					break;
				case 127:
					f.detail = "Windows error 127: the plugin needs a function another DLL does not have: a different version of a "
					           "DLL it depends on is installed.";
					break;
				default:
					f.detail = "The plugin's DLL could not be loaded, so the mod does not work. Reinstall it or check its requirements.";
				}
				out.push_back(std::move(f));
			} else if (low.find("disabled, ") != std::string::npos) {
				static const std::regex handle(R"(\s*\(handle\s+\d+\)\s*$)", std::regex::icase);
				const auto why = std::regex_replace(Trim(l.substr(low.find("disabled, ") + 10)), handle, "");
				f.severity = Severity::kBad;
				f.title = (dll.empty() ? std::string("A plugin") : dll) + " was not loaded: " + why;
				const auto w = Lower(why);
				if (w.find("address library") != std::string::npos) {
					f.detail = "Address Library for SKSE Plugins is missing or too old for this game version. Install the Address "
					           "Library version that matches your game (the All in one package covers every version).";
				} else if (w.find("incompatible") != std::string::npos || w.find("runtime") != std::string::npos) {
					f.detail = "The plugin was made for another game version. Install the version of the mod for your game version, "
					           "or downgrade/upgrade the game to what the mod supports.";
				} else if (w.find("no version data") != std::string::npos) {
					f.detail = "The DLL does not say which game versions it supports (an old or not an SKSE plugin). It may be for an "
					           "older game version.";
				} else {
					f.detail = "SKSE refused to load this plugin, so the mod does not work.";
				}
				out.push_back(std::move(f));
			} else if (low.find("reported as incompatible") != std::string::npos) {
				f.severity = Severity::kBad;
				f.title = (dll.empty() ? std::string("A plugin") : dll) + " said it cannot run on this game";
				f.detail = "The plugin itself refused to load (wrong game version, or something it needs is missing). Its own log "
				           "(in the same SKSE folder) usually says why.";
				out.push_back(std::move(f));
			}
		}
		if (!version.empty()) {
			Finding f;
			f.severity = Severity::kInfo;
			f.title = "SKSE: " + version;
			f.file = a_file;
			out.push_back(std::move(f));
		}
		return out;
	}

	// ---- crash logs ------------------------------------------------------------------------

	CrashInfo ReadCrash(const std::string& a_text)
	{
		CrashInfo  c;
		const auto log = crashlog::Parse(a_text);
		c.exception = log.exception;
		c.tool = log.tool;
		c.game = log.game;
		static const std::regex mod(R"(([A-Za-z0-9_.\-]+\.(?:dll|exe))\s*\+\s*(?:0x)?([0-9A-Fa-f]+))", std::regex::icase);
		std::smatch m;
		if (std::regex_search(c.exception, m, mod)) {
			std::string off = m[2].str();
			for (auto& ch : off) {
				ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
			}
			c.where = m[1].str() + "+" + off;
		}
		// the call stack section ("PROBABLE CALL STACK:", "CALL STACK:", .NET "Probable callstack:")
		const auto lines = Lines(a_text);
		bool       inStack = false;
		std::set<std::string> seen;
		for (const auto& l : lines) {
			const auto low = Lower(Trim(l));
			if (!l.empty() && !std::isspace(static_cast<unsigned char>(l[0])) && !low.empty() && low.back() == ':') {
				inStack = low.find("call stack") != std::string::npos || low.find("callstack") != std::string::npos;
				continue;
			}
			if (!inStack) {
				continue;
			}
			if (std::regex_search(l, m, mod)) {
				const auto name = m[1].str();
				if (!SystemModules().contains(Lower(name)) && seen.insert(Lower(name)).second) {
					c.modules.push_back(name);
				}
			}
		}
		// the module the exception is in counts first, unless it is the game itself
		if (!c.where.empty()) {
			const auto inMod = c.where.substr(0, c.where.find('+'));
			if (!SystemModules().contains(Lower(inMod))) {
				std::erase_if(c.modules, [&](const std::string& s) { return Lower(s) == Lower(inMod); });
				c.modules.insert(c.modules.begin(), inMod);
			}
		}
		for (const auto& f : log.forms) {
			if (c.forms.size() >= 6) {
				break;
			}
			if (!f.atCrash) {
				continue;
			}
			char id[16];
			std::snprintf(id, sizeof(id), "%08X", f.id);
			std::string s = id;
			if (!f.name.empty()) {
				s = "\"" + f.name + "\" " + s;
			}
			if (!f.type.empty()) {
				s += " (" + Trim(f.type) + ")";
			}
			c.forms.push_back(s);
		}
		return c;
	}

	namespace
	{
		std::string ModuleNote(const std::string& a_module)
		{
			const auto m = Lower(a_module);
			if (m.find("hdtsmp") != std::string::npos) {
				return "HDT-SMP / FSMP physics (hair, clothing, body physics).";
			}
			if (m.rfind("nvwgf2um", 0) == 0 || m.rfind("nvd3dum", 0) == 0 || m.rfind("atidxx", 0) == 0 || m.rfind("amdxx", 0) == 0 ||
				m.rfind("igd", 0) == 0 || m == "d3d11.dll" || m == "dxgi.dll") {
				return "the graphics driver / Direct3D: often ENB, ReShade, textures or meshes, or the driver itself.";
			}
			if (m == "xaudio2_7.dll") {
				return "game audio: a sound file a mod added, or too many sounds at once.";
			}
			if (m == "skee64.dll" || m == "skee.dll") {
				return "RaceMenu / skee (body morphs, overlays).";
			}
			if (m.rfind("skse", 0) == 0) {
				return "SKSE itself: usually a plugin calling into it.";
			}
			if (m == "tbbmalloc.dll") {
				return "the memory allocator (Engine Fixes / SSE Fixes use it): usually not the cause, the code that called it is.";
			}
			return "";
		}
	}

	// ---- everything --------------------------------------------------------------------------

	Analysis Analyze(const std::vector<LogFile>& a_logs, const Options& a_options)
	{
		Analysis a;
		const std::int64_t maxAge = a_options.days > 0 ? static_cast<std::int64_t>(a_options.days) * 24 * 60 : -1;
		for (const auto& f : a_logs) {
			if (maxAge >= 0 && f.ageMinutes > maxAge) {
				++a.skipped;
			} else {
				a.files.push_back(f);
			}
		}

		// SKSE logs: the newest of each name per game
		std::set<std::string> sksesDone;
		for (const auto& f : a.files) {
			if (f.kind != Kind::kSkse || !sksesDone.insert(f.game + "|" + Lower(Name(f.path))).second) {
				continue;
			}
			auto found = ReadSkseLog(f.path, ReadText(f.path));
			for (auto& x : found) {
				if (x.severity == Severity::kInfo) {
					x.title += " (" + (f.game.empty() ? std::string() : f.game + ", ") + Age(f.ageMinutes) + ")";
				}
				a.findings.push_back(std::move(x));
			}
		}

		// crash logs: the newest few; the same crash place more than once is said once with the count
		{
			struct Seen
			{
				std::size_t finding = 0, count = 0;
			};
			std::map<std::string, Seen> byWhere;
			int  read = 0;
			bool newest = true;
			for (const auto& f : a.files) {
				if (f.kind != Kind::kCrash || read >= a_options.crashLogs) {
					continue;
				}
				++read;
				const auto c = ReadCrash(ReadText(f.path, 4u << 20));
				if (c.exception.empty() && c.modules.empty()) {
					continue;
				}
				const auto key = Lower(c.where.empty() ? c.exception : c.where);
				if (auto it = byWhere.find(key); it != byWhere.end()) {
					++it->second.count;
					auto& fx = a.findings[it->second.finding];
					fx.examples.push_back("also: " + Name(f.path) + " (" + Age(f.ageMinutes) + ")");
					continue;
				}
				Finding fx;
				fx.file = f.path;
				fx.severity = newest ? Severity::kBad : Severity::kWarn;
				fx.title = std::string(newest ? "Newest crash" : "Crash") + " (" + Age(f.ageMinutes) + "): " +
				           (c.where.empty() ? c.exception : "in " + c.where);
				std::string d;
				if (!c.exception.empty()) {
					d += c.exception + "\n";
				}
				const auto lowWhere = Lower(c.where);
				for (const auto& k : kKnown) {
					if (lowWhere == k.where && (std::string(k.game).empty() || c.game.empty() || c.game.find(k.game) != std::string::npos)) {
						fx.title += " - " + std::string(k.title);
						d += std::string(k.detail) + "\n";
					}
				}
				if (!c.modules.empty()) {
					d += "DLLs in the call stack (most relevant first; the first one is often the mod involved, not always the cause): ";
					for (std::size_t i = 0; i < c.modules.size() && i < 5; ++i) {
						d += (i ? ", " : "") + c.modules[i];
					}
					d += ".\n";
					for (std::size_t i = 0; i < c.modules.size() && i < 3; ++i) {
						if (const auto n = ModuleNote(c.modules[i]); !n.empty()) {
							d += c.modules[i] + ": " + n + "\n";
						}
					}
				} else if (!c.where.empty()) {
					d += "Only the game's own code is in the call stack: look at the objects the log names (below) or at what was "
					     "happening (loading a save, a cell, a menu).\n";
				}
				if (Lower(c.where).rfind("kernelbase.dll", 0) == 0) {
					d += "KERNELBASE.dll: a program error thrown on purpose (a C++ exception) that nothing caught; the first DLL in the "
					     "call stack is usually the one that threw it.\n";
				} else if (c.where.empty() && !c.exception.empty()) {
					d += "The crash address is in no DLL: the code jumped to a bad address (a broken function pointer or an object "
					     "already freed). The DLLs in the call stack, if any, are the best lead.\n";
				}
				if (!c.forms.empty()) {
					d += "Objects the log links to the crash: ";
					for (std::size_t i = 0; i < c.forms.size(); ++i) {
						d += (i ? ", " : "") + c.forms[i];
					}
					d += ". If the crash happens while a save loads, open that save in ReSaver Plus and use Crash log to find them.\n";
				}
				if (!c.tool.empty()) {
					d += "Written by " + c.tool + (c.game.empty() ? std::string() : ", " + c.game) + ".";
				}
				fx.detail = Trim(d);
				byWhere[key] = { a.findings.size(), 1 };
				a.findings.push_back(std::move(fx));
				newest = false;
			}
			for (const auto& [k, s] : byWhere) {
				if (s.count > 1) {
					auto& fx = a.findings[s.finding];
					fx.severity = Severity::kBad;
					fx.title += " - happened " + std::to_string(s.count) + " times";
				}
			}
		}

		// Papyrus log: the newest one
		for (const auto& f : a.files) {
			if (f.kind != Kind::kPapyrus) {
				continue;
			}
			const auto s = extras::ReadPapyrusLog(f.path);
			if (!s.ok) {
				break;
			}
			const auto text = ReadText(f.path, 16u << 20);
			std::size_t dumps = 0;
			std::map<std::string, std::size_t> missing;
			static const std::regex store(R"(Cannot open store for class \"([^\"]+)\")", std::regex::icase);
			for (const auto& l : Lines(text)) {
				const auto low = Lower(l);
				if (low.find("dumping stack") != std::string::npos) {
					++dumps;
				}
				std::smatch m;
				if (std::regex_search(l, m, store)) {
					++missing[m[1].str()];
				}
			}
			if (dumps > 0) {
				Finding fx;
				fx.severity = Severity::kBad;
				fx.file = f.path;
				fx.title = "Papyrus log: the script engine dumped its stacks " + std::to_string(dumps) + " time(s)";
				fx.detail = "The game wrote out every running script because there were too many at once (script lag). Saves made "
				            "around then can carry a pile of stuck scripts. Open the save in ReSaver Plus and look at the threads and "
				            "the Mods view for the mod that runs the most.";
				a.findings.push_back(std::move(fx));
			}
			if (!missing.empty()) {
				Finding fx;
				fx.severity = Severity::kWarn;
				fx.file = f.path;
				fx.title = "Papyrus log: " + std::to_string(missing.size()) + " script(s) the game could not find";
				fx.detail = "\"Cannot open store for class\": a plugin or save uses a script whose .pex file is not installed (a mod "
				            "removed, or installed without its scripts). Harmless if the mod was removed on purpose; otherwise "
				            "reinstall it. Leftovers can be cleaned from the save with Remove missing scripts.";
				std::vector<std::pair<std::string, std::size_t>> v(missing.begin(), missing.end());
				std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
				for (std::size_t i = 0; i < v.size() && i < 8; ++i) {
					fx.examples.push_back(v[i].first + " (" + std::to_string(v[i].second) + "x)");
				}
				a.findings.push_back(std::move(fx));
			}
			if (s.errors + s.warnings > 0) {
				Finding fx;
				fx.severity = s.errors >= 500 ? Severity::kWarn : Severity::kInfo;
				fx.file = f.path;
				fx.title = "Papyrus log (" + Age(f.ageMinutes) + "): " + std::to_string(s.errors) + " errors, " +
				           std::to_string(s.warnings) + " warnings";
				fx.detail = "Most Papyrus errors are harmless noise; thousands from one script point at a broken or outdated mod. The "
				            "most frequent ones are below; the Papyrus log window groups them by mod.";
				for (std::size_t i = 0; i < s.groups.size() && i < 5; ++i) {
					const auto& g = s.groups[i];
					fx.examples.push_back(std::to_string(g.count) + "x " + (g.script.empty() ? std::string() : g.script + ": ") + g.message);
				}
				a.findings.push_back(std::move(fx));
			}
			break;
		}

		// the other plugins' own logs: lines that say error / critical / exception
		if (a_options.pluginLogs) {
			// an error level marker ("[error]", "[ error ]", "[E]", "<error>"), an exception, or ERROR / FATAL in capitals
			static const std::regex hard(R"(\[\s*(error|critical|crit|fatal|err)\s*\]|<error>|\bexception\b)", std::regex::icase);
			static const std::regex hardCase(R"(\[E\]|\bERROR\b|\bFATAL\b|\bCRITICAL\b)");
			static const std::regex soft(R"(\bfailed to\b|\bunable to\b|\bcould not\b|\bcouldn't\b|\bnot found\b)", std::regex::icase);
			// lines marked info / debug / trace are not errors, whatever they say
			static const std::regex quiet(R"(\[\s*(info|debug|trace|I|D|T)\s*\]|\]\[I\]|\]\[D\])", std::regex::icase);
			// another mod that is not installed (an optional link between mods): not a problem
			static const std::regex optional(R"(dispatch message to|messaging listener for|SmoothCamAPI|not installed|not found|skipp|optional|is not loaded)",
				std::regex::icase);
			for (const auto& f : a.files) {
				if (f.kind != Kind::kPlugin) {
					continue;
				}
				const auto lines = Lines(ReadText(f.path, 4u << 20));
				struct Group
				{
					std::string example;
					std::size_t count = 0, line = 0;
					bool        hard = false;
				};
				std::map<std::string, Group> groups;
				std::size_t hardCount = 0, softCount = 0;
				for (std::size_t i = 0; i < lines.size(); ++i) {
					const auto& l = lines[i];
					if (l.size() > 2000) {
						continue;
					}
					const bool q = std::regex_search(l, quiet);
					bool       h = !q && (std::regex_search(l, hard) || std::regex_search(l, hardCase));
					bool       s = !h && std::regex_search(l, soft);
					if (!h && !s) {
						continue;
					}
					if (h && std::regex_search(l, optional)) {
						h = false;
						s = true;
					}
					(h ? hardCount : softCount)++;
					auto& g = groups[Normalize(l)];
					if (!g.count) {
						g.example = Trim(l);
						g.line = i + 1;
						g.hard = h;
					}
					++g.count;
				}
				if (groups.empty()) {
					continue;
				}
				std::vector<Group> v;
				for (auto& [k, g] : groups) {
					v.push_back(g);
				}
				std::sort(v.begin(), v.end(), [](const Group& x, const Group& y) {
					return x.hard != y.hard ? x.hard : x.count > y.count;
				});
				Finding fx;
				fx.file = f.path;
				fx.line = v.front().line;
				fx.severity = hardCount ? Severity::kWarn : Severity::kInfo;
				fx.title = Name(f.path) + " (" + (f.game.empty() ? std::string() : f.game + ", ") + Age(f.ageMinutes) + "): " +
				           (hardCount ? std::to_string(hardCount) + " error line(s)" : std::to_string(softCount) + " line(s) about something that failed");
				fx.detail = "The mod's own log. Errors here are about that mod: what it could not do. Many are harmless (an optional "
				            "file not there); repeated ones or ones at the end of the log, just before a crash, matter more.";
				for (std::size_t i = 0; i < v.size() && i < 4; ++i) {
					std::string e = v[i].example;
					if (e.size() > 300) {
						e = e.substr(0, 300) + "...";
					}
					fx.examples.push_back((v[i].count > 1 ? std::to_string(v[i].count) + "x " : std::string()) + e);
				}
				a.findings.push_back(std::move(fx));
			}
		}

		if (a.files.empty()) {
			Finding fx;
			fx.severity = Severity::kInfo;
			fx.title = a_logs.empty() ? "No Skyrim logs found" : "No logs written in the last " + std::to_string(a_options.days) + " days";
			fx.detail = a_logs.empty() ? "SKSE writes its logs to Documents\\My Games\\<game>\\SKSE. Start the game through SKSE once."
			                           : "Include older logs to look at them too.";
			a.findings.push_back(std::move(fx));
		}
		std::stable_sort(a.findings.begin(), a.findings.end(),
			[](const Finding& x, const Finding& y) { return static_cast<int>(x.severity) < static_cast<int>(y.severity); });
		return a;
	}

	std::string ToText(const Analysis& a_analysis)
	{
		std::string o;
		o += std::to_string(a_analysis.files.size()) + " log(s) looked at";
		if (a_analysis.skipped) {
			o += ", " + std::to_string(a_analysis.skipped) + " older one(s) left out";
		}
		o += ".\n";
		std::size_t bad = 0, warn = 0;
		for (const auto& f : a_analysis.findings) {
			bad += f.severity == Severity::kBad;
			warn += f.severity == Severity::kWarn;
		}
		o += std::to_string(bad) + " problem(s), " + std::to_string(warn) + " to look at.\n";
		for (const auto& f : a_analysis.findings) {
			o += "\n[";
			o += f.severity == Severity::kBad ? "PROBLEM" : f.severity == Severity::kWarn ? "LOOK" : "INFO";
			o += "] " + f.title + "\n";
			if (!f.file.empty()) {
				o += "  file: " + U8(f.file) + (f.line ? ":" + std::to_string(f.line) : std::string()) + "\n";
			}
			if (!f.detail.empty()) {
				std::istringstream in(f.detail);
				for (std::string l; std::getline(in, l);) {
					o += "  " + l + "\n";
				}
			}
			for (const auto& e : f.examples) {
				o += "    > " + e + "\n";
			}
		}
		return o;
	}

	std::string ReadLines(const std::filesystem::path& a_file, std::size_t a_from, std::size_t a_count, std::size_t a_tail,
		std::size_t a_maxChars, std::size_t* a_total)
	{
		const auto lines = Lines(ReadText(a_file, 32u << 20));
		if (a_total) {
			*a_total = lines.size();
		}
		std::size_t first = a_from > 0 ? a_from - 1 : 0;
		std::size_t last = lines.size();
		if (a_tail > 0) {
			first = lines.size() > a_tail ? lines.size() - a_tail : 0;
		} else if (a_count > 0) {
			last = std::min(lines.size(), first + a_count);
		}
		std::string o;
		for (std::size_t i = first; i < last; ++i) {
			std::string l = std::to_string(i + 1) + ": " + lines[i] + "\n";
			if (o.size() + l.size() > a_maxChars) {
				o += "... (cut at line " + std::to_string(i + 1) + " of " + std::to_string(lines.size()) + "; read on from there)\n";
				break;
			}
			o += l;
		}
		return o;
	}

	std::string Search(const std::vector<LogFile>& a_logs, const std::string& a_text, std::size_t a_maxHits)
	{
		const auto  needle = Lower(a_text);
		std::string o;
		std::size_t hits = 0;
		for (const auto& f : a_logs) {
			const auto lines = Lines(ReadText(f.path, 16u << 20));
			for (std::size_t i = 0; i < lines.size(); ++i) {
				if (Lower(lines[i]).find(needle) == std::string::npos) {
					continue;
				}
				if (++hits > a_maxHits) {
					o += "... more than " + std::to_string(a_maxHits) + " matches; search for something narrower\n";
					return o;
				}
				std::string l = lines[i];
				if (l.size() > 400) {
					l = l.substr(0, 400) + "...";
				}
				o += U8(f.path) + ":" + std::to_string(i + 1) + ": " + l + "\n";
			}
		}
		return hits ? o : "no matches\n";
	}
}
