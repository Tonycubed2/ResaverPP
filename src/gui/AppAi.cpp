// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// "Ask the AI": a chat with an AI engine that uses ReSaver Plus through tools (the command line, the
// running game through ReSaverHelper, the Skyrim logs, text files), and "Analyze Skyrim logs".
// The lesson book the AI gets (aiSystemPrompt) is written here plus the command line's own help text
// (core/CliHelp.h), so it always matches the program; "ReSaverPP AI notes.txt" next to the program is added
// to it when it exists (the user's own notes for the AI).
#include "App.h"

#include "Net.h"
#include "Platform.h"
#include "Theme.h"
#include "Tips.h"
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
				"Read Skyrim's logs (SKSE's log, crash logs, the Papyrus log, the SKSE plugins' own logs) and list the problems found.",
				R"J({"type":"object","properties":{"days":{"type":"integer","description":"only logs written in the last days (default 7, 0 = all)"},"plugin_logs":{"type":"boolean","description":"also the plugins' own logs (default true)"}}})J" },
			{ "list_logs", "Every Skyrim log file found (path, kind, size, age), newest first.",
				R"J({"type":"object","properties":{"days":{"type":"integer","description":"only logs written in the last days (0 = all, default 0)"}}})J" },
			{ "read_log",
				"Read a log (or any text file) by lines. Long files: read the end with tail, or a part with from_line and count.",
				R"J({"type":"object","properties":{"path":{"type":"string"},"from_line":{"type":"integer"},"count":{"type":"integer"},"tail":{"type":"integer","description":"the last N lines instead"}},"required":["path"]})J" },
			{ "search_logs", "Find lines containing a text (not case-sensitive) in the Skyrim logs.",
				R"J({"type":"object","properties":{"text":{"type":"string"},"days":{"type":"integer","description":"only logs written in the last days (0 = all, default 0)"}},"required":["text"]})J" },
			{ "list_folder", "The files and folders in a folder (to find a mod's files, an ini, a report).",
				R"J({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})J" },
			{ "write_text_file",
				"Write a text file (an .ini setting, a plugins list, notes). An existing file is backed up first. Not for saves, plugins "
				"or DLLs.",
				R"J({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string","description":"the whole new content"},"why":{"type":"string","description":"one plain sentence for the user: what this does"}},"required":["path","content"]})J" },
		};
	}

	const char* kLesson = R"LESSON(You are the AI assistant built into ReSaver Plus (ReSaver++), a save editor and troubleshooting tool for Skyrim (Special Edition / Anniversary Edition, VR, and Legendary Edition), made by tonycubed2 as a C++ port of ReSaver (FallrimTools) by Mark Fairchild. The person talking to you is a Skyrim player using the program; most are not programmers. They describe problems in plain English; you look into them with your tools and do what they ask.

HOW TO WORK
- Look before you answer: run the tools on the save, the logs or the quest the question is about. Never guess what a tool would show; run it.
- Explain in plain, short language: what is wrong, why, what you suggest. Plain text with short paragraphs or simple "- " lists; no tables, little markdown.
- Before a change, say in one sentence what you are about to do. The user may have to approve each change; if one is denied, do not retry it: ask what they want instead.
- When asked to do something, do it with the tools instead of telling the user to click things. Only point to the app's own windows for what the tools cannot do (Tools > Transplant progress, the Merged patch window, the Save check window).
- If a command fails, read the error, fix the arguments and try again; after two failures explain what happened.
- Big outputs are cut in the middle; ask for less (a filter text, --top <n>, read_log with tail) when you need the part that was cut.

SAVES
- Saves are .ess files with a co-save next to them (.skse). The save open in the app and the saves folder are in CURRENT STATE below; get_app_state gives them again later.
- Never write over a save. Every edit writes a NEW file with -o; take its name from suggest_save_name (it keeps the game's name format, so the game lists it with the character). run_cli refuses to write over an existing file.
- The command line reads the save FILE. If the app has unsaved edits (unsaved_changes in the state), they are not in the file: tell the user to save them first (File > Save) when it matters.
- After writing a new save, offer to open it in the app (open_save), and tell the user its name so they can load it in the game.
- Form IDs are 8 hex digits (0001A66B). FF000000 and up are objects the game made while playing (spawned, leveled, dropped). The save uses load-order form IDs, as --quests, --npcs and --find show them.
- Prefer the targeted fixes and dry runs: --crash-log with --remove-crash-actors, --path-idles / --fix-path-idles, --remove-undefined, --fix-mcm, --remove-mod with --dry-run first. Removing things blindly from a save can break quests.

THE RUNNING GAME (game_command)
- ReSaverHelper.dll (an optional SKSE plugin of ReSaver Plus; Legendary Edition has its own) lets you act in the game while it runs. get_app_state says whether it is connected and whether a save is loaded in the game.
- Commands (one per call; fields as separate items):
  HELLO -> helper version, game version, SE/AE/VR/LE, in game 0/1
  QUEST <formid> -> editor id, flags, current stage, running 0/1, completed 0/1, objectives (index:state,...)
  SETSTAGE <formid> <stage> <notify> <message> -> 1 set / 0 refused / ? still running, then running, completed, current stage. The stage's own script runs, as in the game.
  COMPLETE <formid> <notify> <message> -> completes all objectives and the quest (no stage script runs)
  STOP <formid> <notify> <message> -> Quest.Stop: the game cleans up after the quest
  RESET <formid> <notify> <message> -> Quest.Reset: back to how it starts, as if never done (then SETSTAGE or the game starts it again)
  REFRESH <notify> -> rebuilds the journal's objectives (runs in the background; PROGRESS shows how far)
  PROGRESS -> done, total, running
  GETOPTION <name> / SETOPTION <name> <value> -> the helper's options
  NOTIFY <notify> <message> -> shows a message in the game
  <notify> is hud (top-left notification), box (message box), none, or ini (the helper's setting). <message> may be empty ("") for the default text.
- Form IDs for game_command are the runtime IDs, the same as the save shows. Quests without any script may answer that only the console can do it; then give the user the console command (setstage / stopquest / resetquest <editor id>).
- When the game is connected, quest fixes in the running game are better than save edits: the game's own scripts run. Tell the user to save in the game afterwards.

LOGS
- analyze_logs lists the problems in SKSE's log (plugins that did not load and why), crash logs (Crash Logger, Trainwreck, .NET Script Framework), the Papyrus log and the SKSE plugins' own logs. list_logs, read_log and search_logs go deeper.
- A crash log: the "Unhandled exception" line says where; PROBABLE CALL STACK lists the code running (the first mod DLL there is often involved, not always the cause); REGISTERS and STACK name objects (form IDs, names, files). If the crash happens while a save loads: run_cli {save} --crash-log <crash log> finds the saved objects it names; --remove-crash-actors removes the spawned or leveled actors the game was loading.
- Known crash: SkyrimSE.exe+0CEC680 on 1.6.1170 = a character saved waiting for a path idle the save does not keep: --path-idles lists them, --fix-path-idles fixes them; the mod Path Idle Crash Fix by Yuof (Nexus SE 196861) prevents it.
- Papyrus log: most errors are harmless noise; thousands from one script, "Dumping Stacks" (script lag) and "Cannot open store for class" (a missing script) matter.
- SKSE plugin load errors: 126 = a DLL it needs is missing (a requirement or the VC++ redistributable); 193 = made for the other game (LE vs SE); "address library needs to be updated" = Address Library for SKSE Plugins missing/old; "incompatible with current runtime version" = made for another game version.

RECIPES
- "Check my save": run_cli {save} (no action: the analysis report), then --path-idles, --spawned, --mods as needed. The app's Save check window shows the same kinds of findings.
- Crash on load: analyze_logs; read the newest crash log; run_cli {save} --crash-log <log>; --path-idles.
- Script lag, a save that grows: --mods (scripts per mod with instances and running threads), --spawned, --report; then --purge-script <name> or --remove-mod for the mod responsible.
- A mod was removed: --remove-mod <plugin> --dry-run --sources <mods folder>, then the same with -o.
- Stuck quest: --quests <name or editor id> for its stage and objectives; with the game connected QUEST, then SETSTAGE / COMPLETE / STOP / RESET.
- Lost follower: --npcs <name>, then --move-to-player <formid> -o <new save>.
- Player: --player, --set-gold, --set-skill, --set-level, --set-perks, --set-item, --set-bounty, --clear-crimes.
- Mod menus (MCM) missing: --fix-mcm. Enchanted items with no charge: --charged, --fix-charges.
- --sources <mods folder> and --profile <MO2 profile folder> let the command line read the installed mods (names, which mod a script belongs to); use the values in CURRENT STATE when they are set.

THE COMMAND LINE (run_cli runs resaverpp-cli with the arguments; its help text follows)
)LESSON";
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
	add(platform::DefaultSavesFolder(false).parent_path().parent_path());
#endif
	return v;
}

std::vector<ess::logs::LogFile> App::findSkyrimLogs() const
{
	std::vector<std::filesystem::path> extra;
	if (const auto g = gameFolder(); !g.empty()) {
		extra.push_back(g / "Data" / "NetScriptFramework" / "Crash");
	}
	return ess::logs::Find(ess::logs::Folders(myGamesFolders(), extra));
}

// ---- Analyze Skyrim logs -------------------------------------------------------------------------

struct App::LogsJob
{
	std::thread                        thread;
	std::atomic<bool>                  done{ false };
	ess::logs::Analysis                result;
	std::vector<std::filesystem::path> folders;
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
	if (!_logsJob) {
		startLogAnalysis();
	}
}

void App::startLogAnalysis()
{
	if (_logsJob && !_logsJob->done) {
		return;
	}
	auto job = std::make_shared<LogsJob>();
	std::vector<std::filesystem::path> extra;
	if (const auto g = gameFolder(); !g.empty()) {
		extra.push_back(g / "Data" / "NetScriptFramework" / "Crash");
	}
	job->folders = ess::logs::Folders(myGamesFolders(), extra);
	ess::logs::Options opt;
	opt.days = _logsDays;
	opt.pluginLogs = _logsPlugins;
	auto* j = job.get();
	job->thread = std::thread([j, opt] {
		j->result = ess::logs::Analyze(ess::logs::Find(j->folders), opt);
		j->done = true;
	});
	_logsJob = std::move(job);
}

void App::drawSkyrimLogs()
{
	prepareWindow("Skyrim logs", ImVec2(0.75f, 0.8f));
	if (!ImGui::Begin("Skyrim logs", &_showSkyrimLogs, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Skyrim logs");
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
	tips::Tip("Also read the logs every SKSE plugin writes for itself (in the SKSE folder) for lines that say error or failed.");
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
		aiSend("Analyze my Skyrim logs. ReSaver Plus found this:\n\n" + ess::logs::ToText(_logsJob->result) +
		       "\nLook deeper with the log tools where it helps, then tell me in plain words what is wrong and what to do about "
		       "it, most important first.");
	}
	tips::Tip("Send these findings to the AI chat: it reads the logs further and explains what to do.");
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
			ImGui::TextDisabled("No Skyrim folders found under Documents\\My Games.");
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
					std::string t = "Look at this from my Skyrim logs and tell me what it means and what to do:\n\n" + f.title + "\n" +
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
	std::string s = kLesson;
	s += ess::CliUsage();
	s += "\nCURRENT STATE (when this message was sent)\n";
	if (_save) {
		const auto& h = _save->header;
		s += "- open save: " + platform::ToUtf8(_path) + "\n";
		s += "- character: " + h.playerName + ", level " + std::to_string(h.playerLevel) + ", in " + h.location + "\n";
		s += std::string("- game: ") + (_save->isLE() ? "Skyrim Legendary Edition" : _vr ? "Skyrim VR" : "Skyrim Special Edition / AE") + "\n";
		s += std::string("- unsaved_changes: ") + (_dirty ? "true (the file does not have them yet)" : "false") + "\n";
	} else {
		s += "- no save is open in the app (open_save opens one; list_saves lists them)\n";
	}
	s += "- saves folder: " + platform::ToUtf8(savesFolder()) + "\n";
	s += "- mods folder (--sources): " + (_sourcesPath.empty() ? std::string("not set") : _sourcesPath) + "\n";
	s += "- MO2 profile (--profile): " + (_profile.empty() ? std::string("not set") : _profile) + "\n";
	s += "- game folder (--game): " + (gameFolder().empty() ? std::string("not found") : platform::ToUtf8(gameFolder())) + "\n";
	const auto li = _live.info();
	if (li.connected) {
		s += "- running game: connected (ReSaverHelper " + li.helperVersion + ", game " + li.gameVersion + " " + li.runtime + ", " +
		     (li.inGame ? "a save is loaded" : "at the main menu, no save loaded") + ")\n";
	} else {
		s += "- running game: not connected (the game is not running, or ReSaverHelper.dll is not installed)\n";
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
		bool                               haveSave = false, dirty = false, le = false;
		std::string                        sources, profile, character;
		std::vector<std::filesystem::path> myGames;
		LiveLink::Info                     live;
	} st;
	_ai.runOnUi([this, &st] {
		st.haveSave = _save != nullptr;
		st.save = _path;
		st.dirty = _dirty;
		st.le = _save && _save->isLE();
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
	auto logsFound = [&](long long a_days) {
		std::vector<std::filesystem::path> extra;
		if (!st.gameDir.empty()) {
			extra.push_back(st.gameDir / "Data" / "NetScriptFramework" / "Crash");
		}
		auto files = ess::logs::Find(ess::logs::Folders(st.myGames, extra));
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
		o += std::string("game: ") + (st.haveSave ? (st.le ? "Skyrim Legendary Edition" : "Skyrim SE/AE/VR") : "unknown") + "\n";
		o += std::string("unsaved_changes: ") + (st.dirty ? "true" : "false") + "\n";
		o += "saves folder: " + platform::ToUtf8(st.savesDir) + "\n";
		o += "mods folder (--sources): " + (st.sources.empty() ? std::string("not set") : st.sources) + "\n";
		o += "MO2 profile (--profile): " + (st.profile.empty() ? std::string("not set") : st.profile) + "\n";
		o += "game folder: " + (st.gameDir.empty() ? std::string("not found") : platform::ToUtf8(st.gameDir)) + "\n";
		o += "running game: " +
		     (st.live.connected ? "connected, ReSaverHelper " + st.live.helperVersion + ", game " + st.live.gameVersion + " " + st.live.runtime +
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
		struct S
		{
			std::filesystem::path           p;
			std::filesystem::file_time_type t;
			std::uintmax_t                  size;
		};
		std::vector<S> v;
		std::error_code ec;
		for (std::filesystem::directory_iterator it(st.savesDir, ec), end; !ec && it != end; it.increment(ec)) {
			if (it->is_regular_file(ec) && LowerA(platform::ToUtf8(it->path().extension())) == ".ess") {
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
			return fail("the game is not connected: it is not running, or ReSaverHelper.dll is not installed (it is an optional file "
			            "of ReSaver Plus on Nexus, installed like any SKSE plugin)");
		}
		static const std::set<std::string> reads = { "HELLO", "QUEST", "PROGRESS", "GETOPTION", "RESOLVE", "SNAPSHOT" };
		if (!reads.contains(cmd.front()) &&
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
		a_shown = "analyzed the Skyrim logs";
		ess::logs::Options opt;
		opt.days = static_cast<int>(args.num("days", 7));
		if (const auto* pl = args.v.get("plugin_logs"); pl && pl->type == Value::Type::kBool) {
			opt.pluginLogs = pl->b;
		}
		return ess::logs::ToText(ess::logs::Analyze(logsFound(0), opt));
	}

	if (name == "list_logs") {
		a_shown = "listed the Skyrim logs";
		const auto files = logsFound(args.num("days", 0));
		std::string o;
		for (const auto& f : files) {
			o += platform::ToUtf8(f.path) + "  [" + ess::logs::KindName(f.kind) + "]  " + SizeText(f.size) + "  " + AgeText(f.time) + "\n";
			if (o.size() > 30000) {
				o += "... (more)\n";
				break;
			}
		}
		return o.empty() ? "no logs found under: " + [&] {
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
		a_shown = "searched the logs for \"" + t + "\"";
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
		static const std::set<std::string> binary = { ".ess", ".skse", ".esp", ".esm", ".esl", ".dll", ".exe", ".pex", ".bsa", ".ba2",
			".nif", ".dds" };
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

	return fail("no tool named " + name);
}

namespace
{
	// the AI's text with a little markdown: ``` code blocks, # headings, - bullets, **bold** markers removed
	void DrawAiText(const std::string& a_text)
	{
		std::istringstream in(a_text);
		bool               code = false;
		for (std::string l; std::getline(in, l);) {
			if (!l.empty() && l.back() == '\r') {
				l.pop_back();
			}
			if (l.rfind("```", 0) == 0) {
				code = !code;
				continue;
			}
			if (code) {
				ImGui::PushStyleColor(ImGuiCol_Text, theme::Dim());
				ImGui::PushTextWrapPos(0);
				ImGui::TextUnformatted(("    " + l).c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
				continue;
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
			std::size_t ind = 0;
			while (ind < t.size() && t[ind] == ' ') {
				++ind;
			}
			if (t.empty()) {
				ImGui::Spacing();
			} else if (t[0] == '#') {
				const auto h = t.find_first_not_of("# ");
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
				ImGui::PushTextWrapPos(0);
				ImGui::TextUnformatted(h == std::string::npos ? "" : t.c_str() + h);
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
			} else if (ind + 1 < t.size() && (t[ind] == '-' || t[ind] == '*') && t[ind + 1] == ' ') {
				ImGui::Indent(static_cast<float>(ind) * 6 + 4);
				ImGui::Bullet();
				ImGui::SameLine();
				ImGui::PushTextWrapPos(0);
				ImGui::TextUnformatted(t.c_str() + ind + 2);
				ImGui::PopTextWrapPos();
				ImGui::Unindent(static_cast<float>(ind) * 6 + 4);
			} else {
				ImGui::PushTextWrapPos(0);
				ImGui::TextUnformatted(t.c_str());
				ImGui::PopTextWrapPos();
			}
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
				ImGui::PushTextWrapPos(0);
				ImGui::TextUnformatted(m.text.c_str());
				ImGui::PopTextWrapPos();
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
				DrawAiText(m.text);
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
				ImGui::PushStyleColor(ImGuiCol_Text, m.error ? ImGui::ColorConvertU32ToFloat4(theme::Bad()) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
				ImGui::PushTextWrapPos(0);
				ImGui::TextUnformatted(m.text.c_str());
				ImGui::PopTextWrapPos();
				ImGui::PopStyleColor();
				break;
			}
			ImGui::PopID();
		}
	});
	if (empty) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
		ImGui::PushTextWrapPos(0);
		ImGui::TextUnformatted("Describe the problem or what you want done, in plain English. The AI can check the open save, read your "
		                       "Skyrim logs, look at quests, NPCs and the player, clean scripts, remove mods from the save, and act in "
		                       "the running game through ReSaverHelper. Edits always go to a new save. For example:");
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::Spacing();
		static const char* kExamples[] = { "Check my open save and tell me what is wrong with it.",
			"My game crashed. Look at my logs and tell me why.", "Which of my quests are stuck, and can you fix them?",
			"My follower is lost. Find her and bring her to me.", "Did any of my SKSE plugins fail to load?" };
		for (const char* e : kExamples) {
			if (ImGui::Button(e)) {
				std::snprintf(_aiInput, sizeof(_aiInput), "%s", e);
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
		ImGui::TextDisabled("Enter sends, Ctrl+Enter starts a new line.");
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
