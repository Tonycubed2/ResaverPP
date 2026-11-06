// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// "Ask the AI": a chat with an AI engine that uses ReSaver Plus through tools (the command line, the
// running game through ReSaverHelper, the game's logs, text files, the Fallout 4 player, leftover items), and
// "Analyze Skyrim logs" / "Analyze Fallout 4 logs".
// The lesson book the AI gets (aiSystemPrompt) depends on the open save's game (core/AiGame: Skyrim SE/AE, VR,
// LE or Fallout 4) plus the command line's own help text (core/CliHelp.h), so it always matches the program;
// "ReSaverPP AI notes.txt" next to the program is added to it when it exists (the user's own notes for the AI).
#include "App.h"

#include "Net.h"
#include "Platform.h"
#include "Theme.h"
#include "Tips.h"
#include "core/AiGame.h"
#include "core/CliHelp.h"
#include "core/Json.h"
#include "core/Save.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <ostream>
#include <sstream>
#include <functional>
#include <map>

namespace
{
	using ess::json::Value;

	std::string LowerA(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	std::string AgeText(std::filesystem::file_time_type a_time)
	{
		const auto m = std::chrono::duration_cast<std::chrono::minutes>(std::filesystem::file_time_type::clock::now() - a_time).count();
		if (m < 2) {
			return "just now";
		}
		if (m < 120) {
			return std::to_string(m) + " minutes ago";
		}
		if (m < 48 * 60) {
			return std::to_string(m / 60) + " hours ago";
		}
		return std::to_string(m / (60 * 24)) + " days ago";
	}

	std::string SizeText(std::uintmax_t a_bytes)
	{
		char buf[32];
		if (a_bytes >= (1u << 20)) {
			std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(a_bytes) / (1 << 20));
		} else {
			std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(a_bytes) / 1024 + 0.5);
		}
		return buf;
	}

	// tool arguments
	struct Args
	{
		Value v;
		explicit Args(const std::string& a_json)
		{
			try {
				v = ess::json::Parse(a_json.empty() ? "{}" : a_json);
			} catch (...) {
			}
		}
		std::string str(const char* k) const
		{
			const auto* x = v.get(k);
			if (!x) {
				return {};
			}
			if (x->type == Value::Type::kString) {
				return x->s;
			}
			if (x->type == Value::Type::kNumber) {
				char b[32];
				std::snprintf(b, sizeof(b), "%g", x->n);
				return b;
			}
			return {};
		}
		long long num(const char* k, long long d) const
		{
			const auto* x = v.get(k);
			if (x && x->type == Value::Type::kNumber) {
				return static_cast<long long>(x->n);
			}
			if (x && x->type == Value::Type::kString && !x->s.empty()) {
				return std::atoll(x->s.c_str());
			}
			return d;
		}
		std::vector<std::string> list(const char* k) const
		{
			std::vector<std::string> out;
			const auto* x = v.get(k);
			if (x && x->type == Value::Type::kArray) {
				for (const auto& e : x->a) {
					if (e.type == Value::Type::kString) {
						out.push_back(e.s);
					} else if (e.type == Value::Type::kNumber) {
						char b[32];
						std::snprintf(b, sizeof(b), "%g", e.n);
						out.push_back(b);
					}
				}
			} else if (x && x->type == Value::Type::kString) {
				// a whole command line in one string: split at spaces, "quoted parts" kept together
				std::string cur;
				bool        q = false, any = false;
				for (char c : x->s) {
					if (c == '"') {
						q = !q;
						any = true;
					} else if (c == ' ' && !q) {
						if (any || !cur.empty()) {
							out.push_back(cur);
						}
						cur.clear();
						any = false;
					} else {
						cur += c;
					}
				}
				if (any || !cur.empty()) {
					out.push_back(cur);
				}
			}
			return out;
		}
	};

	std::vector<ai::Tool> ToolList()
	{
		return {
			{ "get_app_state",
				"What ReSaver Plus has open right now: the save (path, character, game), unsaved edits, the saves folder, the mods "
				"folder / MO2 profile / game folder settings, and whether the running game is connected through ReSaverHelper.",
				R"J({"type":"object","properties":{}})J" },
			{ "run_cli",
				"Run resaverpp-cli (ReSaver Plus's command line, see its help in the instructions) with these arguments and get its "
				"output. Anything the program can do to a save is here: reports, quests, the player, NPCs, cleaning, removing mods, "
				"crash logs. The first argument is usually the save's full path ({save} stands for the save open in the app). Edits "
				"need -o <new save path>; it never writes over an existing file.",
				R"J({"type":"object","properties":{"args":{"type":"array","items":{"type":"string"},"description":"the arguments, one per item, e.g. [\"{save}\", \"--quests\", \"Blood on the Ice\"]"},"why":{"type":"string","description":"one plain sentence for the user: what this does"}},"required":["args"]})J" },
			{ "suggest_save_name",
				"A name for a new, edited copy of a save that the game still lists with the character (the tag goes after the date "
				"in the file name). Use it for -o.",
				R"J({"type":"object","properties":{"save":{"type":"string","description":"the save it is made from (default: the open save)"},"tag":{"type":"string","description":"a short word for what was done, letters only, e.g. fixed, cleaned, noquest"}},"required":["tag"]})J" },
			{ "list_saves", "The newest saves in the saves folder (name, size, age).",
				R"J({"type":"object","properties":{"limit":{"type":"integer","description":"how many (default 15)"}}})J" },
			{ "open_save", "Open a save in ReSaver Plus so the user sees it (e.g. the new save just written).",
				R"J({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})J" },
			{ "game_command",
				"Send a command to ReSaverHelper in the running game (see THE RUNNING GAME in the instructions) and get its reply.",
				R"J({"type":"object","properties":{"command":{"type":"array","items":{"type":"string"},"description":"the command and its arguments, e.g. [\"QUEST\", \"0001F7A3\"] or [\"SETSTAGE\", \"0001F7A3\", \"20\", \"hud\", \"\"]"},"why":{"type":"string","description":"one plain sentence for the user: what this does"}},"required":["command"]})J" },
			{ "analyze_logs",
				"Read the game's logs and list the problems found. Skyrim: SKSE's log, crash logs, the Papyrus log, the SKSE plugins' own "
				"logs. Fallout 4: F4SE's logs, Buffout 4 crash logs, the Papyrus log, the F4SE plugins' own logs. The game is the open "
				"save's (or the game set up in the app).",
				R"J({"type":"object","properties":{"days":{"type":"integer","description":"only logs written in the last days (default 7, 0 = all)"},"plugin_logs":{"type":"boolean","description":"also the plugins' own logs (default true)"}}})J" },
			{ "list_logs", "Every log file of the game found (path, kind, size, age), newest first; for Fallout 4 also its INI files.",
				R"J({"type":"object","properties":{"days":{"type":"integer","description":"only logs written in the last days (0 = all, default 0)"}}})J" },
			{ "read_log",
				"Read a log (or any text file) by lines. Long files: read the end with tail, or a part with from_line and count.",
				R"J({"type":"object","properties":{"path":{"type":"string"},"from_line":{"type":"integer"},"count":{"type":"integer"},"tail":{"type":"integer","description":"the last N lines instead"}},"required":["path"]})J" },
			{ "search_logs", "Find lines containing a text (not case-sensitive) in the game's logs (Fallout 4: and its INI files).",
				R"J({"type":"object","properties":{"text":{"type":"string"},"days":{"type":"integer","description":"only logs written in the last days (0 = all, default 0)"}},"required":["text"]})J" },
			{ "list_folder", "The files and folders in a folder (to find a mod's files, an ini, a report).",
				R"J({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})J" },
			{ "find_form",
				"Find forms (items, NPCs, quests, perks...) by name, editor ID or hex form ID in the plugin names ReSaver Plus has read for "
				"the open save (Skyrim and Fallout 4).",
				R"J({"type":"object","properties":{"text":{"type":"string"},"limit":{"type":"integer","description":"how many (default 30)"}},"required":["text"]})J" },
			{ "leftover_items",
				"Items in the save's inventories whose mod is gone (its plugin is not in the save's plugin list or not in the load order, "
				"or the item is no longer in its plugin), grouped by plugin. Skyrim: the player, NPCs and containers; Fallout 4: containers "
				"and the actors whose saved data is read fully.",
				R"J({"type":"object","properties":{"save":{"type":"string","description":"the save (default: the open save, {save})"}}})J" },
			{ "remove_leftover_items",
				"Take the leftover items of the given plugins (as leftover_items names them) out of every inventory and write the result "
				"as a NEW save.",
				R"J({"type":"object","properties":{"save":{"type":"string","description":"the save (default: the open save)"},"plugins":{"type":"array","items":{"type":"string"},"description":"the plugins (groups) from leftover_items"},"out":{"type":"string","description":"the new save's path (from suggest_save_name)"},"why":{"type":"string","description":"one plain sentence for the user: what this does"}},"required":["plugins","out"]})J" },
			{ "fo4_player",
				"Fallout 4 saves only: the player's level, experience, caps, S.P.E.C.I.A.L., perks with ranks and active effects, read "
				"from the save file.",
				R"J({"type":"object","properties":{"save":{"type":"string","description":"the .fos save (default: the open save)"}}})J" },
			{ "fo4_edit_player",
				"Fallout 4 saves only: change the player's level, experience, caps, S.P.E.C.I.A.L. (1-100), perk ranks (1-255; a perk the "
				"player does not have is added, if the save already refers to it) or remove perks, and write a NEW save. Give only what "
				"changes.",
				R"J({"type":"object","properties":{"save":{"type":"string","description":"the .fos save (default: the open save)"},"out":{"type":"string","description":"the new save's path (from suggest_save_name)"},"level":{"type":"integer"},"experience":{"type":"number"},"caps":{"type":"integer"},"special":{"type":"object","description":"e.g. {\"Strength\": 6, \"Luck\": 3}","properties":{"Strength":{"type":"integer"},"Perception":{"type":"integer"},"Endurance":{"type":"integer"},"Charisma":{"type":"integer"},"Intelligence":{"type":"integer"},"Agility":{"type":"integer"},"Luck":{"type":"integer"}}},"perks":{"type":"array","items":{"type":"string"},"description":"perk form id and rank, e.g. [\"0004D88B:2\"]"},"remove_perks":{"type":"array","items":{"type":"string"},"description":"perk form ids"},"why":{"type":"string","description":"one plain sentence for the user: what this does"}},"required":["out"]})J" },
			{ "write_text_file",
				"Write a text file (an .ini setting, a plugins list, notes). An existing file is backed up first. Not for saves, plugins, "
				"archives or DLLs.",
				R"J({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string","description":"the whole new content"},"why":{"type":"string","description":"one plain sentence for the user: what this does"}},"required":["path","content"]})J" },
		};
	}

	const char* const kSpecialNames[7] = { "Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck" };

	std::string Hex8(std::uint32_t a_id)
	{
		char b[16];
		std::snprintf(b, sizeof(b), "%08X", a_id);
		return b;
	}

	// a hex form id ("0004D88B", "0x4D88B"); false when it is not one
	bool ParseHexId(std::string a_text, std::uint32_t& a_id)
	{
		if (a_text.rfind("0x", 0) == 0 || a_text.rfind("0X", 0) == 0) {
			a_text = a_text.substr(2);
		}
		if (a_text.empty() || a_text.size() > 8 ||
			!std::all_of(a_text.begin(), a_text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
			return false;
		}
		a_id = static_cast<std::uint32_t>(std::strtoul(a_text.c_str(), nullptr, 16));
		return true;
	}
}

// ---- settings ---------------------------------------------------------------------------------

void App::loadAiSetting(const std::string& a_line)
{
	const auto eq = a_line.find('=');
	if (eq == std::string::npos) {
		return;
	}
	const auto key = a_line.substr(0, eq);
	const auto val = a_line.substr(eq + 1);
	auto&      s = _ai.settings();
	if (key == "ai_preset") {
		s.preset = std::clamp(std::atoi(val.c_str()), 0, static_cast<int>(ai::Presets().size()) - 1);
	} else if (key == "ai_api") {
		s.api = val == "anthropic" ? ai::Api::kAnthropic : ai::Api::kOpenAI;
	} else if (key == "ai_url") {
		s.baseUrl = val;
	} else if (key == "ai_key") {
		s.apiKey = net::UnprotectSecret(val);
	} else if (key == "ai_model") {
		s.model = val;
	} else if (key == "ai_ask") {
		s.askBeforeChanges = val != "0";
	} else if (key == "ai_max_tokens") {
		s.maxTokens = std::clamp(std::atoi(val.c_str()), 256, 128000);
	} else if (key == "ai_timeout") {
		s.timeoutSeconds = std::clamp(std::atoi(val.c_str()), 30, 3600);
	} else if (key == "ai_logs_days") {
		_logsDays = std::clamp(std::atoi(val.c_str()), 0, 3650);
	} else if (key == "ai_logs_plugins") {
		_logsPlugins = val != "0";
	}
}

void App::saveAiSettings(std::ostream& a_out)
{
	const auto& s = _ai.settings();
	a_out << "ai_preset=" << s.preset << "\n";
	a_out << "ai_api=" << (s.api == ai::Api::kAnthropic ? "anthropic" : "openai") << "\n";
	a_out << "ai_url=" << s.baseUrl << "\n";
	a_out << "ai_key=" << net::ProtectSecret(s.apiKey) << "\n";
	a_out << "ai_model=" << s.model << "\n";
	a_out << "ai_ask=" << (s.askBeforeChanges ? 1 : 0) << "\n";
	a_out << "ai_max_tokens=" << s.maxTokens << "\n";
	a_out << "ai_timeout=" << s.timeoutSeconds << "\n";
	a_out << "ai_logs_days=" << _logsDays << "\n";
	a_out << "ai_logs_plugins=" << (_logsPlugins ? 1 : 0) << "\n";
}

// ---- where the logs are ------------------------------------------------------------------------

std::vector<std::filesystem::path> App::myGamesFolders() const
{
	std::vector<std::filesystem::path> v;
	auto add = [&](const std::filesystem::path& p) {
		std::error_code ec;
		if (!p.empty() && std::filesystem::is_directory(p, ec) && std::find(v.begin(), v.end(), p) == v.end()) {
			v.push_back(p);
		}
	};
	if (const char* t = std::getenv("RESAVERPP_TEST_MYGAMES")) {
		add(t);
	}
	if (!_path.empty()) {
		add(_path.parent_path().parent_path().parent_path());  // ...\My Games\<game>\Saves\x.ess
	}
	add(savesFolder().parent_path().parent_path());
#ifdef _WIN32
	add(platform::DefaultSavesFolder(_game).parent_path().parent_path());
	add(platform::DefaultSavesFolder(0).parent_path().parent_path());
#endif
	return v;
}

namespace
{
	// the game whose logs are read: the open save's, else the game set up in the app
	bool LogsFo4(bool a_fo4Save, bool a_fo4Game) { return a_fo4Save || a_fo4Game; }

	// the folders besides My Games: Skyrim's .NET Script Framework crash logs in the game's Data folder
	std::vector<std::filesystem::path> ExtraLogFolders(const std::filesystem::path& a_gameDir, bool a_fo4)
	{
		std::vector<std::filesystem::path> extra;
		if (!a_fo4 && !a_gameDir.empty()) {
			extra.push_back(a_gameDir / "Data" / "NetScriptFramework" / "Crash");
		}
		return extra;
	}
}

std::vector<ess::logs::LogFile> App::findSkyrimLogs() const
{
	const bool f4 = LogsFo4(fo4(), fo4Game());
	return ess::logs::FindGameLogs(myGamesFolders(), ExtraLogFolders(gameFolder(), f4), f4);
}

// ---- Analyze Skyrim logs / Analyze Fallout 4 logs ----------------------------------------------------

struct App::LogsJob
{
	std::thread                        thread;
	std::atomic<bool>                  done{ false };
	ess::logs::Analysis                result;
	std::vector<std::filesystem::path> folders;
	std::vector<std::filesystem::path> myGames;
	bool                               fo4 = false;
	~LogsJob()
	{
		if (thread.joinable()) {
			thread.join();
		}
	}
};

void App::openSkyrimLogs()
{
	_showSkyrimLogs = true;
	if (!_logsJob || _logsJob->fo4 != LogsFo4(fo4(), fo4Game())) {  // (the other game's logs were read last)
		startLogAnalysis();
	}
}

void App::startLogAnalysis()
{
	if (_logsJob && !_logsJob->done) {
		return;
	}
	auto job = std::make_shared<LogsJob>();
	job->fo4 = LogsFo4(fo4(), fo4Game());
	job->myGames = myGamesFolders();
	const auto extra = ExtraLogFolders(gameFolder(), job->fo4);
	job->folders = ess::logs::Folders(job->myGames, extra, job->fo4);
	ess::logs::Options opt;
	opt.days = _logsDays;
	opt.pluginLogs = _logsPlugins;
	opt.fo4 = job->fo4;
	auto* j = job.get();
	job->thread = std::thread([j, opt, extra] {
		j->result = ess::logs::Analyze(ess::logs::FindGameLogs(j->myGames, extra, j->fo4), opt);
		j->done = true;
	});
	_logsJob = std::move(job);
}

void App::drawSkyrimLogs()
{
	const bool        f4 = LogsFo4(fo4(), fo4Game());
	const std::string title = ess::aigame::LogsTitle(f4 ? ess::aigame::Game::kFallout4 : ess::aigame::Game::kSkyrimSE);
	const std::string gameName = f4 ? "Fallout 4" : "Skyrim";
	const char*       se = f4 ? "F4SE" : "SKSE";
	prepareWindow("Skyrim logs", ImVec2(0.75f, 0.8f));
	if (!ImGui::Begin((title + "###Skyrim logs").c_str(), &_showSkyrimLogs, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Skyrim logs");
	if (_logsJob && _logsJob->done && _logsJob->fo4 != f4) {
		startLogAnalysis();  // the game changed (a save of the other game was opened)
	}
	const bool running = _logsJob && !_logsJob->done;
	static const int   kDays[] = { 1, 3, 7, 30, 0 };
	static const char* kDayNames[] = { "the last day", "the last 3 days", "the last 7 days", "the last 30 days", "any time" };
	int sel = 2;
	for (int i = 0; i < 5; ++i) {
		sel = kDays[i] == _logsDays ? i : sel;
	}
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Logs written in");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(160);
	if (ImGui::Combo("##days", &sel, kDayNames, 5)) {
		_logsDays = kDays[sel];
		saveSettings();
		startLogAnalysis();
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("The mods' own logs", &_logsPlugins)) {
		saveSettings();
		startLogAnalysis();
	}
	tips::Tip((std::string("Also read the logs every ") + se + " plugin writes for itself (in the " + se +
	           " folder) for lines that say error or failed.").c_str());
	ImGui::SameLine();
	ImGui::BeginDisabled(running);
	if (ImGui::Button("Analyze again")) {
		startLogAnalysis();
	}
	ImGui::EndDisabled();
	const bool haveResult = _logsJob && _logsJob->done;
	ImGui::SameLine();
	ImGui::BeginDisabled(!haveResult);
	if (ImGui::Button("Ask the AI about these")) {
		_showAi = true;
		aiSend("Analyze my " + gameName + " logs. ReSaver Plus found this:\n\n" + ess::logs::ToText(_logsJob->result) +
		       "\nLook deeper with the log tools where it helps, then tell me in plain words what is wrong and what to do about "
		       "it, most important first.");
	}
	tips::Tip("Send these findings to the AI chat: it reads the logs further and explains what to do.");
	ImGui::SameLine();
	if (ImGui::Button("Collect all logs (zip)")) {
		startCollectLogs();
	}
	tips::Tip("Every log, your load order, INI files and PC details in one dated zip on your desktop, to send with a report.");
	ImGui::SameLine();
	if (ImGui::Button("Copy as text")) {
		ImGui::SetClipboardText(ess::logs::ToText(_logsJob->result).c_str());
	}
	ImGui::EndDisabled();
	ImGui::Separator();
	if (!_logsJob || running) {
		ImGui::TextDisabled("Reading the logs...");
		ImGui::End();
		return;
	}
	const auto& a = _logsJob->result;
	std::size_t bad = 0, warn = 0;
	for (const auto& f : a.findings) {
		bad += f.severity == ess::logs::Severity::kBad;
		warn += f.severity == ess::logs::Severity::kWarn;
	}
	ImGui::Text("%zu log(s) read%s. ", a.files.size(),
		a.skipped ? (", " + std::to_string(a.skipped) + " older one(s) left out").c_str() : "");
	ImGui::SameLine(0, 0);
	if (bad) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
		ImGui::Text("%zu problem(s)", bad);
		ImGui::PopStyleColor();
	} else {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Good());
		ImGui::TextUnformatted("No problems");
		ImGui::PopStyleColor();
	}
	ImGui::SameLine(0, 0);
	ImGui::Text(", %zu to look at.", warn);
	if (ImGui::TreeNode("Where it looked")) {
		for (const auto& d : _logsJob->folders) {
			ImGui::PushID(platform::ToUtf8(d).c_str());
			if (ImGui::SmallButton("Open")) {
				platform::OpenWithDefaultApp(d);
			}
			ImGui::SameLine();
			ImGui::TextUnformatted(platform::ToUtf8(d).c_str());
			ImGui::PopID();
		}
		if (_logsJob->folders.empty()) {
			ImGui::TextDisabled("No %s folders found under Documents\\My Games.", gameName.c_str());
		}
		ImGui::TreePop();
	}
	ImGui::Spacing();
	ImGui::BeginChild("##findings", ImVec2(0, 0), ImGuiChildFlags_Borders);
	for (std::size_t i = 0; i < a.findings.size(); ++i) {
		const auto& f = a.findings[i];
		ImGui::PushID(static_cast<int>(i));
		const ImU32 col = f.severity == ess::logs::Severity::kBad    ? theme::Bad()
		                  : f.severity == ess::logs::Severity::kWarn ? theme::Caution()
		                                                             : theme::Dim();
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float  h = ImGui::GetFrameHeight();
		ImGui::GetWindowDrawList()->AddRectFilled(at, ImVec2(at.x + 5, at.y + h), col, 2.0f);
		ImGui::SetCursorScreenPos(ImVec2(at.x + 10, at.y));
		const bool open = ImGui::TreeNodeEx("##f", ImGuiTreeNodeFlags_SpanAvailWidth |
		                                               (f.severity == ess::logs::Severity::kBad ? ImGuiTreeNodeFlags_DefaultOpen : 0),
			"%s", f.title.c_str());
		if (open) {
			if (!f.detail.empty()) {
				ImGui::PushTextWrapPos(0);
				ImGui::TextUnformatted(f.detail.c_str());
				ImGui::PopTextWrapPos();
			}
			for (const auto& e : f.examples) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Dim());
				ImGui::PushTextWrapPos(0);
				ImGui::TextUnformatted(("> " + e).c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			}
			if (!f.file.empty()) {
				if (ImGui::SmallButton("Open the log")) {
					platform::OpenWithDefaultApp(f.file);
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("Ask the AI about this one")) {
					_showAi = true;
					std::string t = "Look at this from my " + gameName + " logs and tell me what it means and what to do:\n\n" + f.title + "\n" +
					                f.detail + "\n";
					for (const auto& e : f.examples) {
						t += "> " + e + "\n";
					}
					t += "Log file: " + platform::ToUtf8(f.file) + (f.line ? ":" + std::to_string(f.line) : std::string());
					aiSend(t);
				}
				ImGui::SameLine();
				ImGui::TextDisabled("%s%s", platform::ToUtf8(f.file).c_str(), f.line ? (":" + std::to_string(f.line)).c_str() : "");
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::End();
}

// ---- the chat --------------------------------------------------------------------------------------

struct App::AiModels
{
	std::thread              thread;
	std::atomic<bool>        done{ false };
	std::vector<std::string> list;
	std::string              error;
	~AiModels()
	{
		if (thread.joinable()) {
			thread.join();
		}
	}
};

void App::aiSetup()
{
	_aiReady = true;
	ai::SetLogFile(platform::ExeFolder() / "ReSaverPP AI.log");
	ai::FixApi(_ai.settings());
	_ai.setTools(ToolList());
	_ai.setExecutor([this](const ai::ToolCall& c, std::string& shown, bool& err) { return aiTool(c, shown, err); });
}

void App::pollAi()
{
	if (!_aiReady) {
		aiSetup();
	}
	_ai.pollUi();
}

void App::aiSend(const std::string& a_text)
{
	if (!_settingsLoaded) {
		loadSettings();
	}
	if (!_aiReady) {
		aiSetup();
	}
	if (!_ai.settings().configured()) {
		_aiShowSettings = true;
		std::snprintf(_aiInput, sizeof(_aiInput), "%s", a_text.c_str());  // kept for when the engine is set up
		return;
	}
	if (_ai.busy()) {
		return;
	}
	ai::FixApi(_ai.settings());
	_ai.send(a_text, [this] { return aiSystemPrompt(); });
}

std::string App::aiSystemPrompt()
{
	namespace ag = ess::aigame;
	const auto  game = _save ? ag::FromFlags(_save->isFO4(), _save->isLE(), _vr) : ag::FromFlags(fo4Game(), false, _vr);
	std::string s = ag::Lesson(game);
	s += ess::CliUsage();
	s += "\nCURRENT STATE (when this message was sent)\n";
	if (_save) {
		const auto& h = _save->header;
		s += "- open save: " + platform::ToUtf8(_path) + "\n";
		s += "- character: " + h.playerName + ", level " + std::to_string(h.playerLevel) + ", in " + h.location + "\n";
		s += ag::GameLine(game, _save->isFO4() ? _save->gameVersion : std::string(), true);
		s += std::string("- unsaved_changes: ") + (_dirty ? "true (the file does not have them yet)" : "false") + "\n";
	} else {
		s += "- no save is open in the app (open_save opens one; list_saves lists them)\n";
		s += ag::GameLine(game, {}, false);
	}
	s += "- saves folder: " + platform::ToUtf8(savesFolder()) + "\n";
	s += "- mods folder (--sources): " + (_sourcesPath.empty() ? std::string("not set") : _sourcesPath) + "\n";
	s += "- MO2 profile (--profile): " + (_profile.empty() ? std::string("not set") : _profile) + "\n";
	s += "- game folder (--game): " + (gameFolder().empty() ? std::string("not found") : platform::ToUtf8(gameFolder())) + "\n";
	const auto li = _live.info();
	if (li.connected) {
		s += "- running game: connected (ReSaverHelper " + li.helperVersion + ", " + (ag::HelperIsFo4(li.runtime) ? "Fallout 4 " : "") + "game " +
		     li.gameVersion + " " + li.runtime + ", " + (li.inGame ? "a save is loaded" : "at the main menu, no save loaded") + ")\n";
		if (ag::HelperIsFo4(li.runtime) != ag::IsFo4(game)) {
			s += "  (the running game is not the game of the open save)\n";
		}
	} else {
		s += std::string("- running game: not connected (the game is not running, or ReSaverHelper.dll is not installed") +
		     (ag::IsFo4(game) ? " in Data\\F4SE\\Plugins" : "") + ")\n";
	}
	// the user's own notes for the AI
	std::ifstream notes(platform::ExeFolder() / "ReSaverPP AI notes.txt");
	if (notes) {
		std::stringstream ss;
		ss << notes.rdbuf();
		s += "\nNOTES FROM THE USER (ReSaverPP AI notes.txt)\n" + ss.str() + "\n";
	}
	return s;
}

std::string App::aiTool(const ai::ToolCall& a_call, std::string& a_shown, bool& a_error)
{
	const Args args(a_call.args);
	const auto& name = a_call.name;
	auto fail = [&](const std::string& t) {
		a_error = true;
		return t;
	};

	// the app's state, copied on the UI thread
	struct State
	{
		std::filesystem::path              save, savesDir, gameDir;
		bool                               haveSave = false, dirty = false, le = false, fo4 = false;
		ess::aigame::Game                  game = ess::aigame::Game::kSkyrimSE;
		std::string                        sources, profile, character, gameVersion;
		std::vector<std::filesystem::path> myGames;
		LiveLink::Info                     live;
	} st;
	_ai.runOnUi([this, &st] {
		st.haveSave = _save != nullptr;
		st.save = _path;
		st.dirty = _dirty;
		st.le = _save && _save->isLE();
		st.fo4 = _save && _save->isFO4();
		st.game = _save ? ess::aigame::FromFlags(st.fo4, st.le, _vr) : ess::aigame::FromFlags(fo4Game(), false, _vr);
		st.gameVersion = st.fo4 ? _save->gameVersion : std::string();
		st.savesDir = savesFolder();
		st.gameDir = gameFolder();
		st.sources = _sourcesPath;
		st.profile = _profile;
		st.myGames = myGamesFolders();
		st.live = _live.info();
		if (_save) {
			st.character = _save->header.playerName + ", level " + std::to_string(_save->header.playerLevel);
		}
		return std::string();
	});
	if (_ai.stopping()) {
		return fail("stopped by the user");
	}
	const bool logsFo4 = ess::aigame::IsFo4(st.game);
	const std::string logsName = logsFo4 ? "Fallout 4" : "Skyrim";
	auto logsFound = [&](long long a_days) {
		auto files = ess::logs::FindGameLogs(st.myGames, ExtraLogFolders(st.gameDir, logsFo4), logsFo4);
		if (a_days > 0) {
			std::erase_if(files, [&](const ess::logs::LogFile& f) { return f.ageMinutes > a_days * 24 * 60; });
		}
		return files;
	};
	auto pathArg = [&](const std::string& a) { return platform::FromUtf8(a == "{save}" ? platform::ToUtf8(st.save) : a); };

	if (name == "get_app_state") {
		a_shown = "looked at what is open";
		std::string o;
		o += "open save: " + (st.haveSave ? platform::ToUtf8(st.save) + " (" + st.character + ")" : std::string("none")) + "\n";
		o += std::string("game: ") + ess::aigame::Name(st.game) + (st.haveSave ? "" : " (the game set up in the app; no save open)") +
		     (st.gameVersion.empty() ? std::string() : ", save made with game version " + st.gameVersion) +
		     (st.fo4 ? "; the Fallout 4 tools are listed in WHAT WORKS ON A FALLOUT 4 SAVE" : "") + "\n";
		o += std::string("unsaved_changes: ") + (st.dirty ? "true" : "false") + "\n";
		o += "saves folder: " + platform::ToUtf8(st.savesDir) + "\n";
		o += "mods folder (--sources): " + (st.sources.empty() ? std::string("not set") : st.sources) + "\n";
		o += "MO2 profile (--profile): " + (st.profile.empty() ? std::string("not set") : st.profile) + "\n";
		o += "game folder: " + (st.gameDir.empty() ? std::string("not found") : platform::ToUtf8(st.gameDir)) + "\n";
		o += "running game: " +
		     (st.live.connected ? "connected, ReSaverHelper " + st.live.helperVersion + (ess::aigame::HelperIsFo4(st.live.runtime) ? " for Fallout 4" : "") +
		                              ", game " + st.live.gameVersion + " " + st.live.runtime +
		                              (st.live.inGame ? ", a save is loaded" : ", no save loaded")
		                        : std::string("not connected")) +
		     "\n";
		for (const auto& m : st.myGames) {
			o += "My Games folder: " + platform::ToUtf8(m) + "\n";
		}
		return o;
	}

	if (name == "run_cli") {
		auto a = args.list("args");
		if (!a.empty() && (LowerA(a.front()) == "resaverpp-cli" || LowerA(a.front()) == "resaverpp-cli.exe")) {
			a.erase(a.begin());
		}
		if (a.empty()) {
			return fail("no arguments given");
		}
		for (auto& x : a) {
			if (x == "{save}") {
				if (!st.haveSave) {
					return fail("no save is open in the app: give the save's full path instead of {save}");
				}
				x = platform::ToUtf8(st.save);
			}
		}
		{
			// the game the command is for: the save it reads, else the open save's / the app's game
			auto       g = st.game;
			const auto ext = LowerA(platform::ToUtf8(platform::FromUtf8(a.front()).extension()));
			if (ext == ".fos") {
				g = ess::aigame::Game::kFallout4;
			} else if (ext == ".ess") {
				g = ess::aigame::Game::kSkyrimSE;
			}
			if (const auto why = ess::aigame::CliRefusal(g, a); !why.empty()) {
				a_shown = "resaverpp-cli (not run: not for Fallout 4)";
				return fail(why);
			}
		}
		const auto workDir = st.haveSave ? st.save.parent_path() : st.savesDir;
		std::string shown = "resaverpp-cli";
		for (const auto& x : a) {
			// a path in the save's folder is shown by its name only
			auto y = x;
			if (const auto folder = platform::ToUtf8(workDir); !folder.empty() && y.rfind(folder, 0) == 0 && y.size() > folder.size() + 1) {
				y = y.substr(folder.size() + 1);
			}
			shown += " " + (y.find(' ') != std::string::npos ? "\"" + y + "\"" : y);
		}
		a_shown = shown;
		// what it writes
		bool change = false;
		std::filesystem::path written;
		for (std::size_t i = 0; i < a.size(); ++i) {
			if (a[i] == "-o" && i + 1 < a.size()) {
				change = true;
				written = platform::FromUtf8(a[i + 1]);
				if (written.is_relative()) {
					written = workDir / written;
				}
				std::error_code ec;
				const auto in = platform::FromUtf8(a.front());
				if (std::filesystem::exists(written, ec)) {
					if (std::filesystem::equivalent(written, in, ec)) {
						return fail("refused: -o is the save itself. Write a new file (suggest_save_name gives a name).");
					}
					return fail("refused: " + platform::ToUtf8(written) + " already exists. Pick a new name (suggest_save_name).");
				}
			}
		}
		if (a.front() == "--merged-patch" || (a.front() == "--plugin-property" && a.size() >= 6)) {
			change = true;
		}
		if (change && !_ai.approve(args.str("why").empty() ? "Run a command that writes a file" : args.str("why"), shown)) {
			return fail("the user did not allow this change");
		}
		const auto exe = platform::ExeFolder() /
#ifdef _WIN32
		                 "resaverpp-cli.exe";
#else
		                 "resaverpp-cli";
#endif
		std::error_code ec;
		if (!std::filesystem::exists(exe, ec)) {
			return fail("resaverpp-cli is not next to ReSaverPP (" + platform::ToUtf8(exe) + "): it comes in the same download; put it back there.");
		}
		const auto r = net::Run(exe, a, workDir, 1800, _ai.stopFlag(), 40000);
		if (!r.error.empty()) {
			return fail(r.error);
		}
		std::string o = r.cancelled ? "stopped by the user\n" : r.timedOut ? "stopped: it ran longer than 30 minutes\n" : "";
		o += "exit code " + std::to_string(r.exitCode) + "\n" + r.output;
		if (r.exitCode == 0 && !written.empty() && std::filesystem::exists(written, ec)) {
			o += "\n(written: " + platform::ToUtf8(written) + ")";
		}
		a_error = r.exitCode != 0;
		return o;
	}

	if (name == "suggest_save_name") {
		const auto src = args.str("save").empty() ? st.save : pathArg(args.str("save"));
		if (src.empty()) {
			return fail("no save is open: give the save it is made from");
		}
		auto tag = args.str("tag");
		if (tag.empty()) {
			tag = "edited";
		}
		const auto n = ess::EditedSaveName(src, tag, true);
		a_shown = "picked a name for the new save";
		return platform::ToUtf8(n);
	}

	if (name == "list_saves") {
		a_shown = "listed the saves";
		const std::string want = st.fo4 || (!st.haveSave && ess::aigame::IsFo4(st.game)) ? ".fos" : ".ess";
		struct S
		{
			std::filesystem::path           p;
			std::filesystem::file_time_type t;
			std::uintmax_t                  size;
		};
		std::vector<S> v;
		std::error_code ec;
		for (std::filesystem::directory_iterator it(st.savesDir, ec), end; !ec && it != end; it.increment(ec)) {
			if (it->is_regular_file(ec) && LowerA(platform::ToUtf8(it->path().extension())) == want) {
				v.push_back({ it->path(), it->last_write_time(ec), it->file_size(ec) });
			}
		}
		std::sort(v.begin(), v.end(), [](const S& x, const S& y) { return x.t > y.t; });
		const auto limit = static_cast<std::size_t>(std::clamp(args.num("limit", 15), 1LL, 200LL));
		std::string o = "saves folder: " + platform::ToUtf8(st.savesDir) + " (" + std::to_string(v.size()) + " saves)\n";
		for (std::size_t i = 0; i < v.size() && i < limit; ++i) {
			o += platform::ToUtf8(v[i].p.filename()) + "  " + SizeText(v[i].size) + "  " + AgeText(v[i].t) + "\n";
		}
		return o;
	}

	if (name == "open_save") {
		const auto p = pathArg(args.str("path"));
		a_shown = "opened " + platform::ToUtf8(p.filename()) + " in ReSaver Plus";
		return _ai.runOnUi([this, p]() -> std::string {
			if (_dirty) {
				return "not opened: the app has unsaved changes in " + platform::ToUtf8(_path.filename()) +
				       ". Ask the user to save or undo them first (File menu).";
			}
			std::error_code ec;
			if (!std::filesystem::exists(p, ec)) {
				return "not opened: " + platform::ToUtf8(p) + " does not exist";
			}
			open(p);
			return _save && _path == p ? "opened " + platform::ToUtf8(p) : "could not open it: " + _status;
		});
	}

	if (name == "game_command") {
		auto cmd = args.list("command");
		if (cmd.empty()) {
			return fail("no command given");
		}
		for (auto& c : cmd.front()) {
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		std::string shown = "game:";
		for (const auto& x : cmd) {
			shown += " " + (x.empty() ? std::string("\"\"") : x);
		}
		a_shown = shown;
		if (!st.live.connected) {
			return fail(ess::aigame::IsFo4(st.game)
			                ? "the game is not connected: Fallout 4 is not running through F4SE, or ReSaverHelper.dll is not installed (it is "
			                  "an optional file on ReSaver Plus's Fallout 4 page on Nexus, installed like any F4SE plugin)"
			                : "the game is not connected: it is not running, or ReSaverHelper.dll is not installed (it is an optional file "
			                  "of ReSaver Plus on Nexus, installed like any SKSE plugin)");
		}
		if (const auto why = ess::aigame::HelperRefusal(st.live.runtime, cmd.front()); !why.empty()) {
			return fail(why);
		}
		if (!ess::aigame::HelperReadOnly(cmd.front()) &&
			!_ai.approve(args.str("why").empty() ? "Change something in the running game" : args.str("why"), shown)) {
			return fail("the user did not allow this change");
		}
		auto p = std::make_shared<std::promise<LiveLink::Reply>>();
		auto f = p->get_future();
		_ai.runOnUi([this, cmd, p] {
			_live.request(cmd, [p](const LiveLink::Reply& r) {
				try {
					p->set_value(r);
				} catch (...) {
				}
			});
			return std::string();
		});
		for (int i = 0; i < 1200; ++i) {
			if (f.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready) {
				const auto r = f.get();
				std::string o;
				for (std::size_t k = 0; k < r.size(); ++k) {
					o += (k ? " | " : "") + r[k];
				}
				a_error = !LiveLink::Ok(r);
				return o.empty() ? "no reply" : o;
			}
			if (_ai.stopping()) {
				return fail("stopped by the user");
			}
		}
		return fail("no reply from the game in 2 minutes (a menu open? the game paused?)");
	}

	if (name == "analyze_logs") {
		a_shown = "analyzed the " + logsName + " logs";
		ess::logs::Options opt;
		opt.fo4 = logsFo4;
		opt.days = static_cast<int>(args.num("days", 7));
		if (const auto* pl = args.v.get("plugin_logs"); pl && pl->type == Value::Type::kBool) {
			opt.pluginLogs = pl->b;
		}
		return ess::logs::ToText(ess::logs::Analyze(logsFound(0), opt));
	}

	if (name == "list_logs") {
		a_shown = "listed the " + logsName + " logs";
		const auto files = logsFound(args.num("days", 0));
		std::string o;
		for (const auto& f : files) {
			o += platform::ToUtf8(f.path) + "  [" + ess::logs::KindName(f.kind, logsFo4) + "]  " + SizeText(f.size) + "  " + AgeText(f.time) + "\n";
			if (o.size() > 30000) {
				o += "... (more)\n";
				break;
			}
		}
		return o.empty() ? "no " + logsName + " logs found under: " + [&] {
			std::string d;
			for (const auto& m : st.myGames) {
				d += platform::ToUtf8(m) + "; ";
			}
			return d;
		}()
		                 : o;
	}

	if (name == "read_log") {
		const auto p = pathArg(args.str("path"));
		std::error_code ec;
		if (!std::filesystem::is_regular_file(p, ec)) {
			return fail("not a file: " + platform::ToUtf8(p));
		}
		const auto  tail = static_cast<std::size_t>(std::max(0LL, args.num("tail", 0)));
		const auto  from = static_cast<std::size_t>(std::max(1LL, args.num("from_line", 1)));
		const auto  count = static_cast<std::size_t>(std::max(0LL, args.num("count", 0)));
		std::size_t total = 0;
		auto        text = ess::logs::ReadLines(p, from, count, tail, 30000, &total);
		a_shown = "read " + platform::ToUtf8(p.filename());
		return platform::ToUtf8(p) + " (" + std::to_string(total) + " lines)\n" + text;
	}

	if (name == "search_logs") {
		const auto t = args.str("text");
		if (t.empty()) {
			return fail("no text to search for");
		}
		a_shown = "searched the " + logsName + " logs for \"" + t + "\"";
		return ess::logs::Search(logsFound(args.num("days", 0)), t, 200);
	}

	if (name == "list_folder") {
		const auto p = pathArg(args.str("path"));
		a_shown = "looked in " + platform::ToUtf8(p);
		std::error_code ec;
		if (!std::filesystem::is_directory(p, ec)) {
			return fail("not a folder: " + platform::ToUtf8(p));
		}
		std::vector<std::string> lines;
		for (std::filesystem::directory_iterator it(p, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end;
			 it.increment(ec)) {
			const bool dir = it->is_directory(ec);
			lines.push_back(platform::ToUtf8(it->path().filename()) + (dir ? "\\" : "  " + SizeText(it->file_size(ec)) + "  " + AgeText(it->last_write_time(ec))));
		}
		std::sort(lines.begin(), lines.end());
		std::string o;
		for (std::size_t i = 0; i < lines.size(); ++i) {
			if (i >= 400) {
				o += "... and " + std::to_string(lines.size() - i) + " more\n";
				break;
			}
			o += lines[i] + "\n";
		}
		return o.empty() ? "(empty)" : o;
	}

	if (name == "write_text_file") {
		const auto p = pathArg(args.str("path"));
		const auto ext = LowerA(platform::ToUtf8(p.extension()));
		static const std::set<std::string> binary = { ".ess", ".skse", ".fos", ".f4se", ".esp", ".esm", ".esl", ".dll", ".exe", ".pex", ".bsa",
			".ba2", ".nif", ".dds" };
		if (binary.contains(ext)) {
			return fail("refused: " + ext + " files are not text; use the command line's own options for saves and plugins");
		}
		a_shown = "wrote " + platform::ToUtf8(p);
		if (!_ai.approve(args.str("why").empty() ? "Write a text file" : args.str("why"), "write " + platform::ToUtf8(p))) {
			return fail("the user did not allow this change");
		}
		std::error_code ec;
		std::string     backup;
		if (std::filesystem::exists(p, ec)) {
			auto b = p;
			b += ".resaverpp.bak";
			for (int n = 2; std::filesystem::exists(b, ec); ++n) {
				b = p;
				b += ".resaverpp" + std::to_string(n) + ".bak";
			}
			std::filesystem::copy_file(p, b, ec);
			if (ec) {
				return fail("could not back up the file first: " + ec.message());
			}
			backup = platform::ToUtf8(b);
		}
		std::ofstream out(p, std::ios::binary | std::ios::trunc);
		const auto content = args.str("content");
		out << content;
		out.close();
		if (!out) {
			return fail("could not write " + platform::ToUtf8(p));
		}
		return "written (" + std::to_string(content.size()) + " bytes)" + (backup.empty() ? std::string() : "; the old file was kept as " + backup);
	}

	// ---- tools that read the save file themselves (the same core code as the app's windows) ----
	// the save a tool works on: "save" (default: the open save) and, for writes, "out" (a new file)
	auto saveArg = [&]() -> std::filesystem::path {
		const auto v = args.str("save");
		return v.empty() || v == "{save}" ? st.save : pathArg(v);
	};
	auto outArg = [&](const std::filesystem::path& a_in, std::string& a_why) -> std::filesystem::path {
		auto out = pathArg(args.str("out"));
		if (out.empty()) {
			a_why = "no out given: the new save's path (suggest_save_name gives one)";
			return {};
		}
		if (out.is_relative()) {
			out = a_in.parent_path() / out;
		}
		std::error_code ec;
		if (std::filesystem::exists(out, ec)) {
			a_why = std::filesystem::equivalent(out, a_in, ec) ? "refused: out is the save itself. Write a new file (suggest_save_name gives a name)."
			                                                    : "refused: " + platform::ToUtf8(out) + " already exists. Pick a new name (suggest_save_name).";
			return {};
		}
		if (LowerA(platform::ToUtf8(out.extension())) != LowerA(platform::ToUtf8(a_in.extension()))) {
			a_why = "refused: out must be a " + platform::ToUtf8(a_in.extension()) + " save like the one it is made from";
			return {};
		}
		return out;
	};
	auto loadSave = [&](const std::filesystem::path& a_path, std::string& a_why) -> std::unique_ptr<ess::SaveFile> {
		if (a_path.empty()) {
			a_why = "no save is open in the app: give the save's full path";
			return {};
		}
		try {
			auto sv = ess::SaveFile::load(a_path);
			if (!sv->problems.empty()) {
				a_why = "this save has structural problems; it is not edited here (the command line's --describe shows them)";
			}
			return sv;
		} catch (const std::exception& e) {
			a_why = std::string("could not read the save: ") + e.what();
			return {};
		}
	};
	auto unsavedNote = [&](const std::filesystem::path& a_path) {
		std::error_code ec;
		return st.dirty && std::filesystem::equivalent(a_path, st.save, ec)
		           ? std::string("\n(note: the app has unsaved edits in this save; they are not in the file read here)")
		           : std::string();
	};
	auto writeSave = [&](ess::SaveFile& a_sv, const std::filesystem::path& a_out) -> std::string {
		try {
			a_sv.save(a_out);  // staged, read back, then put in place (and its co-save next to it)
		} catch (const std::exception& e) {
			return e.what();
		}
		return {};
	};

	if (name == "find_form") {
		const auto t = args.str("text");
		if (t.empty()) {
			return fail("no text to search for");
		}
		a_shown = "looked up \"" + t + "\" in the plugins";
		const auto limit = static_cast<std::size_t>(std::clamp(args.num("limit", 30), 1LL, 200LL));
		return _ai.runOnUi([this, t, limit]() -> std::string {
			if (!_db) {
				return "the plugin names are not read yet: the app reads them from the mods folder / game folder (Options > Settings) "
				       "after a save is opened. Try again in a moment, or set the folders.";
			}
			std::string o;
			std::size_t n = 0;
			std::uint32_t id = 0;
			std::vector<std::uint32_t> hits = _db->search(t, limit);
			if (ParseHexId(t, id) && _db->find(id) && std::find(hits.begin(), hits.end(), id) == hits.end()) {
				hits.insert(hits.begin(), id);
			}
			for (const auto h : hits) {
				const auto* f = _db->find(h);
				o += Hex8(h) + "  " + _db->describe(h) + (f ? "  (" + _db->pluginName(*f) + ")" : std::string()) + "\n";
				++n;
			}
			return n ? o : "nothing found for \"" + t + "\" in " + std::to_string(_db->plugins().size()) + " plugins";
		});
	}

	if (name == "leftover_items" || name == "remove_leftover_items") {
		const bool remove = name == "remove_leftover_items";
		const auto in = saveArg();
		std::string why;
		std::filesystem::path out;
		if (remove) {
			out = outArg(in, why);
			if (out.empty()) {
				return fail(why);
			}
		}
		a_shown = remove ? "removed leftover items into " + platform::ToUtf8(out.filename()) : "looked for leftover items in " + platform::ToUtf8(in.filename());
		auto sv = loadSave(in, why);
		if (!sv || (remove && !why.empty())) {
			return fail(why);
		}
		// grouped by plugin and reason, as Tools > Leftover items does
		struct Group
		{
			std::string plugin, reason;
			std::map<std::uint32_t, std::vector<std::uint32_t>> items;  // item -> holders (raw ref ids)
			std::size_t entries = 0;
		};
		std::vector<Group> groups;
		std::string        note, names;
		const auto         entries = ess::cf::ListInventories(*sv);
		std::error_code    ec;
		const bool         openSave = !st.save.empty() && std::filesystem::equivalent(in, st.save, ec);
		_ai.runOnUi([&]() -> std::string {
			const auto active = currentLoadOrder();
			const auto* db = openSave ? _db.get() : nullptr;  // (the plugin names are read for the open save's plugins)
			std::set<std::string> dbRead;
			if (db) {
				for (const auto& p : db->plugins()) {
					dbRead.insert(LowerA(p));
				}
			}
			if (!active.known && !db) {
				note = "The load order is not known (set the game folder and mod manager in Options > Settings) and plugin names are not "
				       "read, so only items whose plugin is not in the save's own plugin list are found.\n";
			}
			std::map<std::string, std::size_t> index;
			for (const auto& e : entries) {
				std::string plugin, reason;
				if (e.item == 0) {
					plugin = "(none)";
					reason = "an entry with no item";
				} else {
					plugin = sv->pluginOf(e.item);
					const auto low = LowerA(plugin);
					if (plugin == "Created") {
						continue;  // made in game: kept with the save
					}
					if (plugin == "?") {
						reason = "its plugin is not in the save's plugin list";
					} else if (active.known && !active.plugins.contains(low)) {
						reason = "its plugin is not in your load order";
					} else if (db && dbRead.contains(low) && !db->find(e.item)) {
						reason = "the item is no longer in its plugin (the mod was updated or changed)";
					} else {
						continue;
					}
				}
				const auto key = plugin + "\x1f" + reason;
				auto       it = index.find(key);
				if (it == index.end()) {
					it = index.emplace(key, groups.size()).first;
					groups.push_back({ plugin, reason, {}, 0 });
				}
				auto& g = groups[it->second];
				g.items[e.item].push_back(e.holderRefRaw);
				++g.entries;
			}
			if (!remove) {
				for (const auto& g : groups) {
					names += "\n" + g.plugin + ": " + g.reason + "; " + std::to_string(g.items.size()) + " item(s), " + std::to_string(g.entries) + " entries\n";
					std::size_t shown = 0;
					for (const auto& [item, holders] : g.items) {
						if (++shown > 15) {
							names += "  ... " + std::to_string(g.items.size() - 15) + " more\n";
							break;
						}
						ess::RefID r;
						r.data = holders.front();
						const auto h = sv->formIDOf(r);
						names += "  " + (db ? formText(item) : Hex8(item)) + "  held by " + (h == 0x14 ? std::string("the player") : Hex8(h)) +
						         (holders.size() > 1 ? " and " + std::to_string(holders.size() - 1) + " more" : std::string()) + "\n";
					}
				}
			}
			return std::string();
		});
		if (!remove) {
			std::size_t total = 0;
			for (const auto& g : groups) {
				total += g.entries;
			}
			const std::string scope = sv->isFO4() ? " (Fallout 4: containers and the actors whose saved data is read fully)" : "";
			if (groups.empty()) {
				return note + "No leftover items: every item in " + std::to_string(entries.size()) + " inventory entries" + scope +
				       " belongs to a mod that is there." + unsavedNote(in);
			}
			return note + std::to_string(total) + " leftover inventory entries in " + std::to_string(groups.size()) + " group(s)" + scope + ":\n" + names +
			       "\nremove_leftover_items with the plugins takes them out into a new save." + unsavedNote(in);
		}
		// remove the groups asked for
		std::set<std::string> want;
		for (const auto& p : args.list("plugins")) {
			want.insert(LowerA(p));
		}
		std::map<std::uint32_t, std::set<std::uint32_t>> perHolder;
		std::size_t                                       asked = 0;
		for (const auto& g : groups) {
			if (!want.contains(LowerA(g.plugin))) {
				continue;
			}
			asked += g.entries;
			for (const auto& [item, holders] : g.items) {
				for (auto h : holders) {
					perHolder[h].insert(item);
				}
			}
		}
		if (perHolder.empty()) {
			return fail("none of those plugins has leftover items in this save (leftover_items lists the groups)");
		}
		if (!_ai.approve(args.str("why").empty() ? "Remove leftover items into a new save" : args.str("why"),
				"remove " + std::to_string(asked) + " leftover inventory entries -> " + platform::ToUtf8(out.filename()))) {
			return fail("the user did not allow this change");
		}
		std::size_t removed = 0, failed = 0;
		for (const auto& [h, items] : perHolder) {
			ess::RefID id;
			id.data = h;
			auto*       cf = sv->findChangeForm(id);
			std::size_t n = 0;
			if (!cf || !ess::cf::RemoveInventoryItems(*sv, *cf, items, &n).empty()) {
				++failed;
				continue;
			}
			removed += n;
		}
		if (!removed) {
			return fail("no inventory could be changed; nothing was written");
		}
		if (const auto e = writeSave(*sv, out); !e.empty()) {
			return fail(e);
		}
		return "removed " + std::to_string(removed) + " leftover inventory entries" +
		       (failed ? " (" + std::to_string(failed) + " inventories could not be changed and were left alone)" : std::string()) +
		       "\n(written: " + platform::ToUtf8(out) + ")" + unsavedNote(in);
	}

	if (name == "fo4_player" || name == "fo4_edit_player") {
		const bool edit = name == "fo4_edit_player";
		const auto in = saveArg();
		std::string why;
		std::filesystem::path out;
		if (edit) {
			out = outArg(in, why);
			if (out.empty()) {
				return fail(why);
			}
		}
		a_shown = edit ? "changed the player into " + platform::ToUtf8(out.filename()) : "read the player in " + platform::ToUtf8(in.filename());
		auto sv = loadSave(in, why);
		if (!sv || (edit && !why.empty())) {
			return fail(why);
		}
		if (!sv->isFO4()) {
			return fail("only for Fallout 4 saves; for a Skyrim save use run_cli {save} --player (and --set-level, --set-gold, ...)");
		}
		namespace f4 = ess::fo4cf;
		const auto* pc = f4::FindPlayer(*sv);
		if (!pc) {
			return fail("the player's saved data (ACHR 00000014) is not in this save");
		}
		const auto actor = f4::DecodeActor(*sv, *pc);
		if (!actor.decoded) {
			return fail("the player's saved data could not be read: " + actor.error);
		}
		std::error_code ec;
		const bool openSave = !st.save.empty() && std::filesystem::equivalent(in, st.save, ec);
		auto names = [&](const std::vector<std::uint32_t>& a_ids) {
			std::map<std::uint32_t, std::string> m;
			if (openSave) {
				_ai.runOnUi([&]() -> std::string {
					if (_db) {
						for (const auto id : a_ids) {
							m[id] = _db->describe(id);
						}
					}
					return std::string();
				});
			}
			return m;
		};
		if (!edit) {
			std::string o;
			const int   level = f4::PlayerLevel(*sv);
			o += "level: " + (level >= 0 ? std::to_string(level) : std::to_string(sv->header.playerLevel) + " (the save keeps the game's starting level; it can be set once the character has levelled up)") + "\n";
			const double xp = f4::PlayerXP(*sv);
			char b[64];
			std::snprintf(b, sizeof(b), "%.0f", xp);
			o += std::string("experience: ") + (xp >= 0 ? b : "not saved for this character yet") + "\n";
			const auto caps = f4::PlayerCaps(*sv);
			o += "caps: " + (caps >= 0 ? std::to_string(caps) : std::string("none carried (no caps entry to change)")) + "\n";
			o += "S.P.E.C.I.A.L. (saved base values):";
			for (int i = 0; i < 7; ++i) {
				float v = -1;
				for (const auto& x : actor.values) {
					if (x.formID == 0x2C2u + static_cast<std::uint32_t>(i)) {
						v = x.base;
					}
				}
				std::snprintf(b, sizeof(b), "%g", v);
				o += std::string(" ") + kSpecialNames[i] + " " + (v < 0 ? std::string("not saved") : std::string(b)) + (i < 6 ? "," : "\n");
			}
			std::vector<std::uint32_t> ids;
			for (const auto& k : actor.perks) {
				ids.push_back(k.formID);
			}
			for (const auto& e : actor.effects) {
				ids.push_back(sv->formIDOf(e.item));
			}
			const auto nm = names(ids);
			auto label = [&](std::uint32_t a_id) {
				const auto it = nm.find(a_id);
				return Hex8(a_id) + (it != nm.end() && !it->second.empty() ? "  " + it->second : std::string()) + "  (" + sv->pluginOf(a_id) + ")";
			};
			if (actor.hasPerks) {
				o += "perks: " + std::to_string(actor.perks.size()) + "\n";
				for (const auto& k : actor.perks) {
					o += "  " + label(k.formID) + "  rank " + std::to_string(k.rank) + "\n";
				}
			} else {
				o += "perks: could not be read in this save" + (actor.error.empty() ? std::string() : " (" + actor.error + ")") + "\n";
			}
			o += "active effects: " + std::to_string(actor.effects.size()) + " (shown only; they are not removed for Fallout 4)\n";
			for (std::size_t i = 0; i < actor.effects.size() && i < 60; ++i) {
				o += "  " + label(sv->formIDOf(actor.effects[i].item)) + "  effect " + std::to_string(actor.effects[i].effectIndex) + "\n";
			}
			if (!openSave) {
				o += "(names are given for the save open in the app only)\n";
			}
			return o + unsavedNote(in);
		}
		// the changes, checked as the Edit player window checks them
		std::vector<std::string> plan;
		const auto* lv = args.v.get("level");
		const auto* xp = args.v.get("experience");
		const auto* cp = args.v.get("caps");
		if (lv) {
			const auto n = args.num("level", 0);
			if (n < 1 || n > 65535) {
				return fail("level must be 1 to 65535");
			}
			plan.push_back("level " + std::to_string(n));
		}
		if (xp) {
			if (args.num("experience", -1) < 0) {
				return fail("experience must be 0 or more");
			}
			plan.push_back("experience " + args.str("experience"));
		}
		if (cp) {
			const auto n = args.num("caps", -1);
			if (n < 0 || n > 0x7FFFFFFF) {
				return fail("caps must be 0 or more");
			}
			plan.push_back("caps " + std::to_string(n));
		}
		std::vector<std::pair<int, int>> special;
		if (const auto* sp = args.v.get("special"); sp && sp->type == Value::Type::kObject) {
			for (int i = 0; i < 7; ++i) {
				for (const auto& [k, v] : sp->o) {
					if (LowerA(k) != LowerA(kSpecialNames[i])) {
						continue;
					}
					const int n = v.type == Value::Type::kNumber ? static_cast<int>(v.n) : v.type == Value::Type::kString ? std::atoi(v.s.c_str()) : 0;
					if (n < 1 || n > 100) {
						return fail(std::string(kSpecialNames[i]) + " must be 1 to 100");
					}
					special.push_back({ i, n });
					plan.push_back(std::string(kSpecialNames[i]) + " " + std::to_string(n));
				}
			}
		}
		std::vector<std::pair<std::uint32_t, int>> perks;
		for (const auto& p : args.list("perks")) {
			const auto c = p.find(':');
			std::uint32_t id = 0;
			const int rank = c == std::string::npos ? 1 : std::atoi(p.substr(c + 1).c_str());
			if (!ParseHexId(p.substr(0, c), id) || rank < 1 || rank > 255) {
				return fail("perks: \"" + p + "\" is not <perk form id hex>:<rank 1-255>");
			}
			perks.push_back({ id, rank });
			plan.push_back("perk " + Hex8(id) + " rank " + std::to_string(rank));
		}
		std::vector<std::uint32_t> removePerks;
		for (const auto& p : args.list("remove_perks")) {
			std::uint32_t id = 0;
			if (!ParseHexId(p, id)) {
				return fail("remove_perks: \"" + p + "\" is not a perk form id (hex)");
			}
			removePerks.push_back(id);
			plan.push_back("remove perk " + Hex8(id));
		}
		if (plan.empty()) {
			return fail("nothing to change: give level, experience, caps, special, perks or remove_perks");
		}
		std::string planText;
		for (const auto& p : plan) {
			planText += (planText.empty() ? "" : ", ") + p;
		}
		if (!_ai.approve(args.str("why").empty() ? "Change the Fallout 4 player into a new save" : args.str("why"),
				planText + " -> " + platform::ToUtf8(out.filename()))) {
			return fail("the user did not allow this change");
		}
		// the same setters as Edit player (gui/AppPlayerFO4.cpp); each checks its result reads back
		auto step = [&](const std::string& a_what, const std::string& a_err) {
			if (!a_err.empty()) {
				why = a_what + ": " + a_err;
			}
			return a_err.empty();
		};
		bool ok = true;
		if (ok && lv) {
			ok = step("level", f4::SetPlayerLevel(*sv, static_cast<int>(args.num("level", 1))));
		}
		if (ok && xp) {
			ok = step("experience", f4::SetPlayerXP(*sv, static_cast<float>(xp->type == Value::Type::kNumber ? xp->n : std::atof(xp->s.c_str()))));
		}
		if (ok && cp) {
			ok = step("caps", f4::SetPlayerCaps(*sv, static_cast<std::int32_t>(args.num("caps", 0))));
		}
		for (const auto& [i, n] : special) {
			if (ok) {
				ok = step(kSpecialNames[i], f4::SetPlayerSpecial(*sv, i, static_cast<float>(n)));
			}
		}
		for (const auto& [id, rank] : perks) {
			if (!ok) {
				break;
			}
			const auto* p = f4::FindPlayer(*sv);
			auto*       pcf = p ? sv->findChangeForm(p->refID) : nullptr;
			if (!pcf) {
				ok = step("perk " + Hex8(id), "the player's saved data is not in this save");
				break;
			}
			const auto now = f4::DecodeActor(*sv, *pcf);
			const bool has = std::any_of(now.perks.begin(), now.perks.end(), [&](const f4::Perk& k) { return k.formID == id; });
			ok = step("perk " + Hex8(id), has ? f4::SetPerkRank(*sv, *pcf, id, static_cast<std::uint8_t>(rank))
			                                  : f4::AddPerk(*sv, *pcf, id, static_cast<std::uint8_t>(rank)));
		}
		for (const auto id : removePerks) {
			if (!ok) {
				break;
			}
			const auto* p = f4::FindPlayer(*sv);
			auto*       pcf = p ? sv->findChangeForm(p->refID) : nullptr;
			ok = step("remove perk " + Hex8(id), pcf ? f4::RemovePerk(*sv, *pcf, id) : std::string("the player's saved data is not in this save"));
		}
		if (!ok) {
			return fail("not changed (nothing was written): " + why);
		}
		if (const auto e = writeSave(*sv, out); !e.empty()) {
			return fail(e);
		}
		return "changed: " + planText + "\n(written: " + platform::ToUtf8(out) + ")" + unsavedNote(in);
	}

	return fail("no tool named " + name);
}

namespace
{

	// ---- chat text that can be selected with the mouse and copied ----------------------------------------
	// A read-only text box per message. Dear ImGui's text boxes do not wrap, so the text is wrapped here first:
	// a line break replaces a space, or is put in a word too long for the line; each character remembers which,
	// so a copy gives back the text as written (spaces back, added breaks out).
	struct Wrapped
	{
		float             width = 0;
		std::size_t       srcHash = 0;
		std::string       text;   // wrapped
		std::vector<char> kind;   // per character of text: 0 as written, 1 break that was a space, 2 added break
		int               selA = 0, selB = 0;
	};
	std::map<std::string, Wrapped> g_wrapped;

	std::string CleanAiText(const std::string& a_text)  // markdown markers out (**bold**, `code`, # headings, ``` fences)
	{
		std::string        out;
		std::istringstream in(a_text);
		bool               first = true;
		for (std::string l; std::getline(in, l);) {
			if (!l.empty() && l.back() == '\r') {
				l.pop_back();
			}
			if (l.rfind("```", 0) == 0) {
				continue;
			}
			std::size_t h = 0;
			while (h < l.size() && l[h] == '#') {
				++h;
			}
			if (h && h < l.size() && l[h] == ' ') {
				l = l.substr(h + 1);
			}
			std::string t;
			for (std::size_t i = 0; i < l.size(); ++i) {
				if (l.compare(i, 2, "**") == 0 || l.compare(i, 2, "__") == 0) {
					++i;
					continue;
				}
				if (l[i] == '`') {
					continue;
				}
				t += l[i];
			}
			out += (first ? "" : "\n") + t;
			first = false;
		}
		return out;
	}

	Wrapped& WrapFor(const std::string& a_id, const std::string& a_text, float a_width)
	{
		auto&      w = g_wrapped[a_id];
		const auto hash = std::hash<std::string>{}(a_text);
		if (w.width == a_width && w.srcHash == hash) {
			return w;
		}
		w.width = a_width;
		w.srcHash = hash;
		w.text.clear();
		w.kind.clear();
		ImFont*     font = ImGui::GetFont();
		const float scale = ImGui::GetFontSize() / font->FontSize;
		const char* s = a_text.c_str();
		const char* end = s + a_text.size();
		while (s < end) {
			const char* nl = static_cast<const char*>(std::memchr(s, '\n', static_cast<std::size_t>(end - s)));
			const char* lineEnd = nl ? nl : end;
			const char* p = s;
			while (p < lineEnd) {
				const char* brk = font->CalcWordWrapPositionA(scale, p, lineEnd, a_width);
				if (brk >= lineEnd) {
					w.text.append(p, lineEnd);
					w.kind.insert(w.kind.end(), static_cast<std::size_t>(lineEnd - p), 0);
					p = lineEnd;
					break;
				}
				if (brk <= p) {
					brk = p + 1;  // always move on
				}
				w.text.append(p, brk);
				w.kind.insert(w.kind.end(), static_cast<std::size_t>(brk - p), 0);
				if (*brk == ' ') {
					w.text += '\n';
					w.kind.push_back(1);
					p = brk + 1;
				} else {
					w.text += '\n';
					w.kind.push_back(2);
					p = brk;
				}
			}
			if (nl) {
				w.text += '\n';
				w.kind.push_back(0);
				s = nl + 1;
			} else {
				s = end;
			}
		}
		return w;
	}

	int OnSelection(ImGuiInputTextCallbackData* a_data)
	{
		auto* w = static_cast<Wrapped*>(a_data->UserData);
		w->selA = std::min(a_data->SelectionStart, a_data->SelectionEnd);
		w->selB = std::max(a_data->SelectionStart, a_data->SelectionEnd);
		return 0;
	}

	std::string Unwrap(const Wrapped& a_w, int a_from, int a_to)
	{
		std::string out;
		for (int i = std::max(0, a_from); i < a_to && i < static_cast<int>(a_w.text.size()); ++i) {
			const char k = a_w.kind[static_cast<std::size_t>(i)];
			if (k == 2) {
				continue;
			}
			out += k == 1 ? ' ' : a_w.text[static_cast<std::size_t>(i)];
		}
		return out;
	}

	// the text as a read-only box the mouse can select in; Ctrl+C (or right-click > Copy) copies the selection
	void SelectableText(const std::string& a_id, const std::string& a_text, ImU32 a_color = 0)
	{
		const float width = std::max(50.0f, ImGui::GetContentRegionAvail().x - 8);
		auto&       w = WrapFor(a_id, a_text, width - 4);
		const int   lines = 1 + static_cast<int>(std::count(w.text.begin(), w.text.end(), '\n'));
		ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
		if (a_color) {
			ImGui::PushStyleColor(ImGuiCol_Text, a_color);
		}
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 1));
		ImGui::InputTextMultiline(("##" + a_id).c_str(), w.text.data(), w.text.size() + 1,
			ImVec2(width, lines * ImGui::GetTextLineHeight() + 4),
			ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll | ImGuiInputTextFlags_CallbackAlways, OnSelection, &w);
		ImGui::PopStyleVar();
		if (a_color) {
			ImGui::PopStyleColor();
		}
		ImGui::PopStyleColor();
		const auto& io = ImGui::GetIO();
		if (ImGui::IsItemActive() && io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_C, false) || ImGui::IsKeyPressed(ImGuiKey_Insert, false)) &&
			w.selB > w.selA) {
			ImGui::SetClipboardText(Unwrap(w, w.selA, w.selB).c_str());  // after the box's own copy: as written
		}
		if (ImGui::BeginPopupContextItem(("##ctx" + a_id).c_str())) {
			if (ImGui::MenuItem("Copy", "Ctrl+C", false, w.selB > w.selA)) {
				ImGui::SetClipboardText(Unwrap(w, w.selA, w.selB).c_str());
			}
			if (ImGui::MenuItem("Copy all of it")) {
				ImGui::SetClipboardText(Unwrap(w, 0, static_cast<int>(w.text.size())).c_str());
			}
			ImGui::EndPopup();
		}
	}

	std::string Transcript(const std::vector<ai::Message>& a_msgs)
	{
		std::string o;
		for (const auto& m : a_msgs) {
			switch (m.role) {
			case ai::Message::Role::kUser: o += "You:\n" + m.text + "\n\n"; break;
			case ai::Message::Role::kAssistant:
				if (!m.text.empty()) {
					o += "AI:\n" + m.text + "\n\n";
				}
				break;
			case ai::Message::Role::kTool: o += "[" + (m.shown.empty() ? m.toolName : m.shown) + "]\n"; break;
			case ai::Message::Role::kNote: o += "(" + m.text + ")\n\n"; break;
			}
		}
		return o;
	}
}

void App::drawAiSettings()
{
	auto& s = _ai.settings();
	bool  changed = false;
	const auto& presets = ai::Presets();
	ImGui::SetNextItemWidth(260);
	if (ImGui::BeginCombo("Engine", presets[static_cast<std::size_t>(s.preset)].name)) {
		for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
			if (ImGui::Selectable(presets[static_cast<std::size_t>(i)].name, i == s.preset) && i != s.preset) {
				const auto& p = presets[static_cast<std::size_t>(i)];
				s.preset = i;
				s.api = p.api;
				s.baseUrl = p.baseUrl;
				s.model = p.model;
				changed = true;
			}
		}
		ImGui::EndCombo();
	}
	ai::FixApi(s);
	const auto& preset = presets[static_cast<std::size_t>(s.preset)];
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::PushTextWrapPos(0);
	ImGui::TextUnformatted(preset.note);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	if (std::string(preset.name).rfind("Other", 0) == 0) {
		int api = s.api == ai::Api::kAnthropic ? 1 : 0;
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Kind:");
		ImGui::SameLine();
		changed |= ImGui::RadioButton("OpenAI-compatible", &api, 0);
		ImGui::SameLine();
		changed |= ImGui::RadioButton("Anthropic", &api, 1);
		s.api = api ? ai::Api::kAnthropic : ai::Api::kOpenAI;
	}
	char buf[1024];
	std::snprintf(buf, sizeof(buf), "%s", s.baseUrl.c_str());
	ImGui::SetNextItemWidth(420);
	if (ImGui::InputText("Address", buf, sizeof(buf))) {
		s.baseUrl = buf;
	}
	changed |= ImGui::IsItemDeactivatedAfterEdit();
	static bool showKey = false;
	std::snprintf(buf, sizeof(buf), "%s", s.apiKey.c_str());
	ImGui::SetNextItemWidth(420);
	if (ImGui::InputText("API key", buf, sizeof(buf), showKey ? 0 : ImGuiInputTextFlags_Password)) {
		s.apiKey = buf;
	}
	changed |= ImGui::IsItemDeactivatedAfterEdit();
	ImGui::SameLine();
	ImGui::Checkbox("Show", &showKey);
	tips::Tip("Kept in resaverpp.ini, encrypted for your Windows user account (only you on this PC can read it back).");
	if (!preset.needsKey && s.apiKey.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("(not needed for an engine on this PC)");
	}
	std::snprintf(buf, sizeof(buf), "%s", s.model.c_str());
	ImGui::SetNextItemWidth(420);
	if (ImGui::InputText("Model", buf, sizeof(buf))) {
		s.model = buf;
	}
	changed |= ImGui::IsItemDeactivatedAfterEdit();
	ImGui::SameLine();
	const bool listing = _aiModels && !_aiModels->done;
	ImGui::BeginDisabled(listing || s.baseUrl.empty());
	if (ImGui::Button(listing ? "Listing..." : "List models")) {
		auto job = std::make_shared<AiModels>();
		auto* j = job.get();
		job->thread = std::thread([j, settings = s] {
			j->list = ai::Chat::ListModels(settings, j->error);
			j->done = true;
		});
		_aiModels = std::move(job);
		ImGui::OpenPopup("##models");
	}
	ImGui::EndDisabled();
	tips::Tip("Ask the engine which models it has (also a quick test of the address and key).");
	if (_aiModels && _aiModels->done) {
		if (!_aiModels->error.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
			ImGui::PushTextWrapPos(0);
			ImGui::TextUnformatted(_aiModels->error.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		} else if (ImGui::BeginPopup("##models")) {
			for (const auto& m : _aiModels->list) {
				if (ImGui::Selectable(m.c_str(), m == s.model)) {
					s.model = m;
					changed = true;
				}
			}
			ImGui::EndPopup();
		} else {
			ImGui::TextDisabled("%zu models found: click List models again to pick one.", _aiModels->list.size());
		}
	} else if (listing) {
		ImGui::OpenPopup("##models");
		if (ImGui::BeginPopup("##models")) {
			ImGui::TextDisabled("Asking the engine...");
			ImGui::EndPopup();
		}
	}
	if (s.api == ai::Api::kAnthropic) {
		ImGui::SetNextItemWidth(120);
		changed |= ImGui::InputInt("Longest answer (tokens)", &s.maxTokens, 1024);
		s.maxTokens = std::clamp(s.maxTokens, 256, 128000);
	}
	ImGui::SetNextItemWidth(120);
	changed |= ImGui::InputInt("Wait for an answer (seconds)", &s.timeoutSeconds, 30);
	s.timeoutSeconds = std::clamp(s.timeoutSeconds, 30, 3600);
	tips::Tip("Engines on your own PC can be slow: give them more time.");
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::PushTextWrapPos(0);
	ImGui::TextUnformatted("What you type, and what the AI reads with its tools (parts of your save, your logs, file and mod names), is "
	                       "sent to the engine you pick. An engine on this PC (Ollama, LM Studio) keeps everything here. Paid engines "
	                       "charge your account for each question; a question usually costs a few cents.");
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
	if (changed) {
		saveSettings();
	}
}

void App::drawAiChat()
{
	prepareWindow("Ask the AI", ImVec2(0.6f, 0.85f));
	if (!ImGui::Begin("Ask the AI", &_showAi, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Ask the AI");
	auto&      s = _ai.settings();
	const bool busy = _ai.busy();

	// top row
	if (s.configured()) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s, %s", ai::Presets()[static_cast<std::size_t>(s.preset)].name, s.model.c_str());
		ImGui::SameLine();
	}
	if (ImGui::Button(_aiShowSettings ? "Hide engine settings" : "Engine settings")) {
		_aiShowSettings = !_aiShowSettings;
	}
	ImGui::SameLine();
	if (ImGui::Checkbox("Ask me before changes", &s.askBeforeChanges)) {
		saveSettings();
	}
	tips::Tip("On: the AI asks for your OK before it writes a new save, changes the running game or writes a file. It can look "
	          "at anything without asking. Off: it does what it decides.");
	ImGui::SameLine();
	if (ImGui::Button("New chat")) {
		_ai.clear();
	}
	ImGui::SameLine();
	if (ImGui::Button("Copy chat")) {
		std::string t;
		_ai.read([&](const std::vector<ai::Message>& m) { t = Transcript(m); });
		ImGui::SetClipboardText(t.c_str());
	}
	if (_aiShowSettings || !s.configured()) {
		ImGui::Separator();
		if (!s.configured()) {
			ImGui::PushStyleColor(ImGuiCol_Text, theme::Warn());
			ImGui::TextUnformatted("Pick an AI engine first: its address, API key and model.");
			ImGui::PopStyleColor();
		}
		drawAiSettings();
	}
	ImGui::Separator();

	// the conversation
	const float inputH = ImGui::GetTextLineHeight() * 4 + ImGui::GetStyle().FramePadding.y * 2;
	const auto  appr = _ai.approval();
	const float apprH = appr.pending ? ImGui::GetFrameHeightWithSpacing() * 3 + ImGui::GetTextLineHeightWithSpacing() * 2 : 0;
	ImGui::BeginChild("##chat", ImVec2(0, -(inputH + apprH + ImGui::GetFrameHeightWithSpacing() * 2)), ImGuiChildFlags_Borders);
	const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 30;
	bool       empty = true;
	_ai.read([&](const std::vector<ai::Message>& msgs) {
		empty = msgs.empty();
		for (std::size_t i = 0; i < msgs.size(); ++i) {
			const auto& m = msgs[i];
			ImGui::PushID(static_cast<int>(i));
			switch (m.role) {
			case ai::Message::Role::kUser:
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Good());
				ImGui::TextUnformatted("You");
				ImGui::PopStyleColor();
				SelectableText(std::to_string(i), m.text);
				break;
			case ai::Message::Role::kAssistant:
				if (m.text.empty()) {
					break;
				}
				if (m.calls.empty()) {  // an answer; text before tool calls is the AI saying what it does next
					ImGui::Spacing();
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
					ImGui::TextUnformatted("AI");
					ImGui::PopStyleColor();
					ImGui::SameLine();
					if (ImGui::SmallButton("Copy")) {
						ImGui::SetClipboardText(m.text.c_str());
					}
				}
				SelectableText(std::to_string(i), CleanAiText(m.text));
				break;
			case ai::Message::Role::kTool:
				{
					ImGui::PushStyleColor(ImGuiCol_Text, m.error ? ImGui::ColorConvertU32ToFloat4(theme::Bad()) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
					const bool open = ImGui::TreeNodeEx("##tool", ImGuiTreeNodeFlags_SpanAvailWidth, "%s%s",
						m.shown.empty() ? m.toolName.c_str() : m.shown.c_str(), m.error ? "  (failed)" : "");
					ImGui::PopStyleColor();
					if (open) {
						const float lines = static_cast<float>(std::count(m.text.begin(), m.text.end(), '\n') + 1);
						const float h = std::min(lines, 18.0f) * ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2 + 4;
						ImGui::InputTextMultiline("##out", const_cast<char*>(m.text.c_str()), m.text.size() + 1, ImVec2(-FLT_MIN, h),
							ImGuiInputTextFlags_ReadOnly);
						ImGui::TreePop();
					}
					break;
				}
			case ai::Message::Role::kNote:
				SelectableText(std::to_string(i), m.text, m.error ? theme::Bad() : ImGui::GetColorU32(ImGuiCol_TextDisabled));
				break;
			}
			ImGui::PopID();
		}
	});
	if (empty) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::PushTextWrapPos(0);
		const auto game = _save ? ess::aigame::FromFlags(_save->isFO4(), _save->isLE(), _vr) : ess::aigame::FromFlags(fo4Game(), false, _vr);
		ImGui::TextUnformatted(ess::aigame::ChatIntro(game).c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::Spacing();
		for (const auto& e : ess::aigame::ChatExamples(game)) {
			if (ImGui::Button(e.c_str())) {
				std::snprintf(_aiInput, sizeof(_aiInput), "%s", e.c_str());
				_aiFocusInput = true;
			}
		}
	}
	const auto rev = _ai.revision();
	if (rev != _aiSeenRevision) {
		_aiSeenRevision = rev;
		if (atBottom || busy) {
			ImGui::SetScrollHereY(1.0f);
		}
	}
	ImGui::EndChild();

	// a change waiting for the user's OK
	if (appr.pending) {
		ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::BannerBg());
		ImGui::BeginChild("##approve", ImVec2(0, apprH), ImGuiChildFlags_Borders);
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Caution());
		ImGui::TextUnformatted("The AI wants to make a change:");
		ImGui::PopStyleColor();
		ImGui::PushTextWrapPos(0);
		ImGui::TextUnformatted(appr.what.c_str());
		ImGui::PopTextWrapPos();
		ImGui::TextDisabled("%s", appr.command.c_str());
		if (ImGui::Button("Allow")) {
			_ai.answer(1);
		}
		ImGui::SameLine();
		if (ImGui::Button("Allow every change in this chat")) {
			_ai.answer(2);
		}
		ImGui::SameLine();
		if (ImGui::Button("Don't")) {
			_ai.answer(0);
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
	}

	// status
	if (busy) {
		static const char* kSpin = "|/-\\";
		ImGui::AlignTextToFramePadding();
		ImGui::Text("%c %s", kSpin[static_cast<int>(ImGui::GetTime() * 8) & 3], _ai.status().c_str());
		ImGui::SameLine();
		if (ImGui::Button("Stop")) {
			_ai.stop();
		}
	} else {
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Enter sends, Ctrl+Enter starts a new line. Select any text in the chat with the mouse; Ctrl+C copies it.");
	}

	// input
	if (_aiFocusInput) {
		ImGui::SetKeyboardFocusHere();
		_aiFocusInput = false;
	}
	const float sendW = 90;
	bool send = ImGui::InputTextMultiline("##input", _aiInput, sizeof(_aiInput),
		ImVec2(ImGui::GetContentRegionAvail().x - sendW - ImGui::GetStyle().ItemSpacing.x, inputH),
		ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
	ImGui::SameLine();
	ImGui::BeginDisabled(busy || !s.configured());
	send |= ImGui::Button("Send", ImVec2(sendW, inputH));
	ImGui::EndDisabled();
	if (send && !busy && s.configured()) {
		std::string t = _aiInput;
		while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) {
			t.pop_back();
		}
		if (!t.empty()) {
			ImGui::ClearActiveID();  // else the box keeps its own copy of the text
			_aiInput[0] = '\0';
			aiSend(t);
		}
		_aiFocusInput = true;
	}
	ImGui::End();
}
