// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "CrashLog.h"

#include "ChangeFormData.h"
#include "Papyrus.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

namespace ess::crashlog
{
	namespace
	{
		std::string Upper(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
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

		// a section header: no indentation, ends with ':' (Crash Logger, Trainwreck: "REGISTERS:"; .NET Script
		// Framework: "Registers:", "Possible relevant objects (3)")
		bool IsHeader(const std::string& a_line)
		{
			if (a_line.empty() || std::isspace(static_cast<unsigned char>(a_line[0]))) {
				return false;
			}
			const auto t = Trim(a_line);
			if (t.empty()) {
				return false;
			}
			const auto u = Upper(t);
			if (u.rfind("POSSIBLE RELEVANT OBJECTS", 0) == 0) {
				return true;
			}
			return t.back() == ':' && t.find(": ") == std::string::npos;
		}

		// sections that show what the game was working on when it crashed
		bool AtCrashSection(const std::string& a_header)
		{
			const auto u = Upper(a_header);
			return u.find("CALL STACK") != std::string::npos || u.find("CALLSTACK") != std::string::npos ||
			       u.find("REGISTERS") != std::string::npos || u.find("RELEVANT OBJECTS") != std::string::npos;
		}
	}

	namespace
	{
		// the call stack, the object types, the save names and the script extender plugins (see Log::sf)
		void ReadLayout(const std::vector<std::string>& a_lines, Log& a_log)
		{
			static const std::regex kFrame(R"(^\s*\[\s*(\d+)\s*\]\s+0x[0-9A-Fa-f]+\s+(\S+)\+([0-9A-Fa-f]+)(?:\s+(\S.*?))?\s*$)");
			static const std::regex kTypeRe(R"(\(([A-Za-z_][A-Za-z0-9_:<>, ]*)\*\))");
			static const std::regex kString(R"(\(char\*\)\s*\"([^\"]*)\")");
			static const std::regex kSave(R"(^((?:Save|Autosave|Quicksave|Exitsave)\d*_[0-9A-Fa-f]{8}[^\\/:*?\"<>|]*?)(?:\.sfs|\.ess|\.fos)?$)",
				std::regex::icase);
			static const std::regex kPlugin(R"(^\s+(\S+\.dll)\s*(v\S+)?\s*$)", std::regex::icase);
			enum class Sec { kOther, kCallStack, kRegisters, kStack, kSePlugins };
			Sec                                 sec = Sec::kOther;
			std::map<std::string, Log::TypeSeen> types;
			std::vector<std::string>            order;
			for (const auto& l : a_lines) {
				if (IsHeader(l)) {
					const auto u = Upper(Trim(l));
					sec = u.find("CALL STACK") != std::string::npos   ? Sec::kCallStack
					    : u.rfind("REGISTERS", 0) == 0                 ? Sec::kRegisters
					    : u == "STACK:"                                ? Sec::kStack
					    : u.find("EXTENDER PLUGINS") != std::string::npos ? Sec::kSePlugins
					                                                   : Sec::kOther;
					continue;
				}
				std::smatch m;
				if (sec == Sec::kCallStack && std::regex_match(l, m, kFrame)) {
					a_log.frames.push_back({ static_cast<std::size_t>(std::stoul(m[1].str())), m[2].str(), m[3].str(), m[4].str() });
					continue;
				}
				if (sec == Sec::kSePlugins && std::regex_match(l, m, kPlugin)) {
					a_log.sePlugins.emplace_back(m[1].str(), m[2].str());
					continue;
				}
				if (sec != Sec::kRegisters && sec != Sec::kStack) {
					continue;
				}
				if (std::regex_search(l, m, kTypeRe)) {
					const auto name = Trim(m[1].str());
					static const std::set<std::string> kPlain = { "void", "char", "wchar_t", "const char", "std::byte" };
					if (!kPlain.contains(name)) {
						auto& t = types[name];
						if (t.name.empty()) {
							t.name = name;
							order.push_back(name);
						}
						++(sec == Sec::kRegisters ? t.inRegisters : t.inStack);
					}
				}
				if (std::regex_search(l, m, kString)) {
					std::smatch s;
					const auto  text = m[1].str();
					if (std::regex_match(text, s, kSave)) {
						const auto name = s[1].str();
						if (std::find(a_log.saveNames.begin(), a_log.saveNames.end(), name) == a_log.saveNames.end()) {
							a_log.saveNames.push_back(name);
						}
					}
				}
			}
			for (const auto& n : order) {
				a_log.types.push_back(types[n]);
			}
			std::stable_sort(a_log.types.begin(), a_log.types.end(), [](const Log::TypeSeen& a, const Log::TypeSeen& b) {
				if ((a.inRegisters > 0) != (b.inRegisters > 0)) {
					return a.inRegisters > 0;
				}
				return a.inRegisters + a.inStack > b.inRegisters + b.inStack;
			});
			// the parts of the game the registers' objects belong to (Starfield's type names: Movement*, PathingCell, NavMesh*,
			// Spaceship*, BSScript / GameScript, bhk* / hknp* (Havok), *Animation*, Actor / TESNPC, TESObjectCELL...)
			{
				struct Area
				{
					const char* name;
					std::vector<const char*> keys;
				};
				static const std::vector<Area> kAreas = {
					{ "NPC movement and pathfinding (navmesh)", { "Movement", "Pathing", "NavMesh", "PathFollower" } },
					{ "a ship", { "Spaceship", "ShipCell" } },
					{ "Papyrus scripts", { "BSScript", "GameScript", "GameVM" } },
					{ "physics (Havok)", { "bhk", "hknp", "hkp" } },
					{ "animation", { "Animation" } },
					{ "an NPC or creature", { "Actor", "TESNPC", "Character" } },
					{ "a cell (an area of the world)", { "TESObjectCELL", "TESWorldSpace" } },
					{ "a mesh or 3D model", { "BSFadeNode", "NiNode", "BSGeometry", "NiAVObject" } },
					{ "a menu or the interface", { "Scaleform", "Menu" } },
					{ "an item or an inventory", { "BGSInventoryList", "TESObjectWEAP", "TESObjectARMO", "BGSOutfit" } },
					{ "a save being read or written", { "SaveFile", "LoadGameBuffer", "SaveGameBuffer", "PageFileWriter", "SaveLoadManager" } },
				};
				for (const auto& a : kAreas) {
					std::string why;
					for (const auto& t : a_log.types) {
						if (!t.inRegisters) {
							continue;
						}
						for (const char* k : a.keys) {
							if (t.name.rfind(k, 0) == 0 || t.name.find(std::string("::") + k) != std::string::npos ||
								(std::string(k) == "Animation" && t.name.find(k) != std::string::npos) ||
								(std::string(k) == "Menu" && t.name.size() > 4 && t.name.ends_with("Menu"))) {
								why += (why.empty() ? "" : ", ") + t.name;
								break;
							}
						}
					}
					if (!why.empty()) {
						a_log.areas.emplace_back(a.name, why);
					}
				}
			}
			// what the game was doing, from the types that only take part in reading or in writing a save
			static const char* kLoad[] = { "BGSLoadGameBuffer", "BGSLoadFormBuffer", "LoadGameBuffer" };
			static const char* kSaveW[] = { "PageFileWriter", "BGSSaveFormBuffer", "BGSSaveGameBuffer" };
			for (const auto& t : a_log.types) {
				for (const char* k : kLoad) {
					if (t.name.find(k) != std::string::npos) {
						a_log.activity = "load";
						a_log.activityTypes.push_back(t.name);
					}
				}
			}
			if (a_log.activity.empty()) {
				for (const auto& t : a_log.types) {
					for (const char* k : kSaveW) {
						if (t.name.find(k) != std::string::npos) {
							a_log.activity = "save";
							a_log.activityTypes.push_back(t.name);
						}
					}
				}
			}
		}
	}

	bool Log::isSePlugin(const std::string& a_module) const
	{
		const auto u = Upper(a_module);
		if (u.rfind("SFSE_", 0) == 0 || u.rfind("SKSE64_", 0) == 0 || u.rfind("F4SE_", 0) == 0) {
			return true;
		}
		for (const auto& [dll, ver] : sePlugins) {
			if (Upper(dll) == u) {
				return true;
			}
		}
		return false;
	}

	Log Parse(const std::string& a_text)
	{
		Log log;
		std::vector<std::string> lines;
		{
			std::istringstream in(a_text);
			for (std::string l; std::getline(in, l);) {
				if (!l.empty() && l.back() == '\r') {
					l.pop_back();
				}
				lines.push_back(std::move(l));
			}
		}
		const std::size_t head = std::min<std::size_t>(lines.size(), 12);
		for (std::size_t i = 0; i < head; ++i) {
			const auto& l = lines[i];
			const auto  u = Upper(l);
			if (u.find("CRASHLOGGER") != std::string::npos) {
				log.tool = "Crash Logger";
			} else if (u.find("TRAINWRECK") != std::string::npos) {
				log.tool = "Trainwreck";
			} else if (u.find("NETSCRIPTFRAMEWORK") != std::string::npos || u.find("UNHANDLED NATIVE EXCEPTION") != std::string::npos) {
				log.tool = ".NET Script Framework";
			} else if (u.rfind("BUFFOUT 4", 0) == 0) {
				log.tool = "Buffout 4";  // ("Buffout 4 v1.37.0 Mar 12 2025 22:11:48": Buffout 4 and Buffout 4 NG)
				log.fo4 = true;
			}
			if (u.rfind("CRASH TIME", 0) == 0) {
				log.time = Trim(l.substr(l.find(':') + 1));
			}
			if (log.game.empty() && (u.rfind("SKYRIM ", 0) == 0 || u.rfind("SKYRIMSE ", 0) == 0 || u.rfind("SKYRIMVR ", 0) == 0)) {
				log.game = Trim(l);
			}
			if (log.game.empty() && u.rfind("STARFIELD V", 0) == 0) {
				log.game = Trim(l);  // Trainwreck's first line: "Starfield v1.14.74"
				log.sf = true;
			}
			if (log.game.empty() && u.rfind("FALLOUT 4 V", 0) == 0) {
				log.game = Trim(l);  // Buffout 4's first line: "Fallout 4 v1.10.163"
				log.fo4 = true;
			}
			if (log.exception.empty() && u.find("EXCEPTION") != std::string::npos && u.find("UNHANDLED") != std::string::npos) {
				log.exception = Trim(l);
			}
		}

		static const std::regex kId(R"(form\s*id\s*[:=]\s*(?:0x)?([0-9a-f]{8})\b)", std::regex::icase);
		static const std::regex kName(R"((?:GetFullName|Name)\s*:\s*\"([^\"]*)\")", std::regex::icase);
		static const std::regex kType(R"(form\s*type\s*:\s*([^,}\r\n]+))", std::regex::icase);
		static const std::regex kEditorID(R"(editor\s*id\s*:\s*\"([^\"]*)\")", std::regex::icase);
		static const std::regex kFile(R"(^\s*file\s*:\s*\"([^\"]*)\")", std::regex::icase);
		// a line that starts another object: a register ("R14 0x... (Character*)"), a stack slot ("[RSP+10 ]"), a call
		// stack frame ("[ 3] 0x...") or any typed pointer ("(TESNPC*)")
		static const std::regex kObject(R"(^\s*(R[A-Z0-9]{1,3}\s+0x|\[RSP\+|\[\s*\d+\])|\*\))");
		auto find = [](const std::string& a_line, const std::regex& a_re) -> std::string {
			std::smatch m;
			return std::regex_search(a_line, m, a_re) ? m[1].str() : std::string();
		};

		std::map<std::uint32_t, Form> forms;
		bool                          atCrash = false;
		for (std::size_t i = 0; i < lines.size(); ++i) {
			const auto& l = lines[i];
			if (IsHeader(l)) {
				atCrash = AtCrashSection(l);
				continue;
			}
			std::smatch m;
			if (!std::regex_search(l, m, kId)) {
				continue;
			}
			const auto id = static_cast<std::uint32_t>(std::stoul(m[1].str(), nullptr, 16));
			if (id == 0) {
				continue;
			}
			auto& f = forms[id];
			if (!f.hits) {
				f.id = id;
				f.firstLine = i + 1;
			}
			++f.hits;
			f.atCrash |= atCrash;
			// the name sits on the same line or just before the form ID, the type on the same line or just after;
			// another form's ID ends the search
			if (f.name.empty()) {
				f.name = find(l, kName);
				for (std::size_t k = 1; f.name.empty() && k <= 3 && k <= i; ++k) {
					if (std::regex_search(lines[i - k], kId)) {
						break;
					}
					f.name = find(lines[i - k], kName);
					if (std::regex_search(lines[i - k], kObject)) {
						break;  // the object this form ID belongs to starts here
					}
				}
				for (std::size_t k = 1; f.name.empty() && k <= 3 && i + k < lines.size(); ++k) {
					if (std::regex_search(lines[i + k], kId) || std::regex_search(lines[i + k], kObject)) {
						break;
					}
					f.name = find(lines[i + k], kName);
				}
			}
			// EditorID and File come before the form ID in the same object (Buffout 4: File, Modified by, Flags, Name,
			// EditorID, FormID, FormType; Crash Logger: File, Flags, Name, EditorID, FormID, FormType), up to the line
			// that starts the object or the end of the form before it
			auto before = [&](const std::regex& a_re) -> std::string {
				for (std::size_t k = 1; k <= 8 && k <= i; ++k) {
					const auto& b = lines[i - k];
					if (std::regex_search(b, kId) || std::regex_search(b, kType)) {
						break;
					}
					if (auto v = find(b, a_re); !v.empty()) {
						return v;
					}
					if (std::regex_search(b, kObject)) {
						break;
					}
				}
				return {};
			};
			if (f.editorID.empty()) {
				f.editorID = find(l, kEditorID);
				if (f.editorID.empty()) {
					f.editorID = before(kEditorID);
				}
			}
			if (f.file.empty()) {
				f.file = before(kFile);
			}
			if (f.type.empty()) {
				f.type = Trim(find(l, kType));
				for (std::size_t k = 1; f.type.empty() && k <= 3 && i + k < lines.size(); ++k) {
					if (std::regex_search(lines[i + k], kId) || std::regex_search(lines[i + k], kObject)) {
						break;
					}
					f.type = Trim(find(lines[i + k], kType));
				}
			}
		}
		for (auto& [id, f] : forms) {
			log.forms.push_back(std::move(f));
		}
		ReadLayout(lines, log);
		std::stable_sort(log.forms.begin(), log.forms.end(), [](const Form& a, const Form& b) {
			if (a.atCrash != b.atCrash) {
				return a.atCrash;
			}
			if (a.hits != b.hits) {
				return a.hits > b.hits;
			}
			return a.firstLine < b.firstLine;
		});
		log.ok = true;
		if (log.forms.empty() && !log.sf) {
			log.error = "the log names no forms";
		}
		return log;
	}

	Log Read(const std::filesystem::path& a_file)
	{
		std::ifstream f(a_file, std::ios::binary);
		if (!f) {
			Log log;
			log.file = a_file;
			log.error = "could not open the file";
			return log;
		}
		std::ostringstream ss;
		ss << f.rdbuf();
		auto log = Parse(ss.str());
		log.file = a_file;
		return log;
	}

	std::vector<LeveledBase> LeveledBases(const SaveFile& a_save)
	{
		std::vector<LeveledBase> out;
		if (a_save.isFO4()) {
			return out;  // (Fallout 4 change forms are not decoded: no leveled actors are known)
		}
		for (const auto& a : cf::ListLeveledActors(a_save)) {
			out.push_back({ a.base, a.refRaw, a.formID, a.templ });
		}
		return out;
	}

	std::vector<Finding> Match(const Log& a_log, const SaveFile& a_save, const std::vector<LeveledBase>& a_leveled)
	{
		std::unordered_map<std::uint32_t, const ChangeForm*> byForm;
		for (const auto& c : a_save.changeForms) {
			byForm.emplace(a_save.formIDOf(c.refID), &c);
		}
		std::unordered_map<std::uint32_t, std::size_t> scripts;
		if (const auto* p = a_save.papyrus()) {
			for (const auto& i : p->instances) {
				if (!i.refID.isZero()) {
					++scripts[a_save.formIDOf(i.refID)];
				}
			}
		}
		std::unordered_map<std::uint32_t, const LeveledBase*> byBase;
		if (!a_save.isFO4()) {  // (Skyrim's leveled actors; none are known for a Fallout 4 save)
			for (const auto& l : a_leveled) {
				byBase.emplace(l.base, &l);
			}
		}

		std::vector<Finding> out;
		for (const auto& f : a_log.forms) {
			Finding r;
			r.form = f;
			const ChangeForm* cf = nullptr;
			if (auto it = byForm.find(f.id); it != byForm.end()) {
				r.kind = Kind::kChanged;
				cf = it->second;
			} else if (auto lb = byBase.find(f.id); lb != byBase.end()) {
				r.kind = Kind::kRebuiltBase;
				if (auto a = byForm.find(lb->second->actor); a != byForm.end()) {
					cf = a->second;
				}
			} else {
				r.kind = (f.id >> 24) == 0xFF ? Kind::kTemporary : Kind::kPlugin;
			}
			if (cf) {
				r.refRaw = cf->refID.data;
				r.target = a_save.formIDOf(cf->refID);
				r.cfType = ChangeFormTypeName(cf->typeCode(), a_save.isFO4());
				r.created = cf->refID.type() == RefID::Type::kCreated;
				if (auto s = scripts.find(r.target); s != scripts.end()) {
					r.scripts = s->second;
				}
				// a spawned or leveled actor the game was loading when it crashed
				r.suspect = r.created && f.atCrash && static_cast<ChangeFormType>(cf->typeCode()) == ChangeFormType::ACHR;
			}
			out.push_back(std::move(r));
		}
		// the likely causes first, keeping the log's order otherwise
		std::stable_sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) { return a.suspect > b.suspect; });
		return out;
	}
}
