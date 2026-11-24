#include "PCH.h"
#include "AiPage.h"

#include "Game.h"
#include "Protocol.h"
#include "Server.h"
#include "Settings.h"

#include "SKSEMenuFramework.h"

// ReSaver Plus's own AI chat and texts (../src), built into the helper
#include "core/AiGame.h"
#include "core/Json.h"
#include "gui/Ai.h"
#include "gui/Net.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <mutex>
#include <sstream>

namespace AiPage
{
	namespace
	{
		namespace im = ImGuiMCP;
		using ess::json::Value;

		const im::ImVec4 kYellow{ 1.0f, 0.85f, 0.35f, 1.0f };
		const im::ImVec4 kGreen{ 0.55f, 0.9f, 0.55f, 1.0f };
		const im::ImVec4 kBlue{ 0.6f, 0.8f, 1.0f, 1.0f };
		const im::ImVec4 kRed{ 1.0f, 0.5f, 0.45f, 1.0f };

		// never deleted: its thread must not be joined while the game shuts down
		ai::Chat& Chat()
		{
			static auto* chat = new ai::Chat();
			return *chat;
		}

		std::mutex  g_cfgLock;
		bool        g_haveConfig = false;
		std::string g_copiedOn;  // when ReSaver Plus last handed its settings over
		std::string g_keyProblem;
		bool        g_pending = false;  // new settings arrived while the chat was busy
		ai::Settings g_pendingSettings;
		char        g_input[4096] = {};
		std::size_t g_seenRevision = 0;

		std::filesystem::path CfgPath() { return Settings::IniPath().parent_path() / "ReSaverHelper_AI.ini"; }

		std::string Now()
		{
			const auto t = std::time(nullptr);
			std::tm    tm{};
			localtime_s(&tm, &t);
			char b[32];
			std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tm);
			return b;
		}

		std::string Upper(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			}
			return s;
		}
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		// applies settings to the chat now, or when it is no longer busy
		void Apply(const ai::Settings& a_s)
		{
			if (Chat().busy()) {
				g_pendingSettings = a_s;
				g_pending = true;
				return;
			}
			Chat().settings() = a_s;
			g_pending = false;
		}

		bool Write(const ai::Settings& a_s, const std::string& a_protectedKey)
		{
			std::ofstream f(CfgPath(), std::ios::binary | std::ios::trunc);
			if (!f) {
				return false;
			}
			f << "; ReSaverHelper's \"Ask the AI\" (F1 page): copied from ReSaver Plus. The key is encrypted for this Windows user.\n";
			f << "copied=" << g_copiedOn << "\n";
			f << "ai_preset=" << a_s.preset << "\n";
			f << "ai_api=" << (a_s.api == ai::Api::kAnthropic ? "anthropic" : "openai") << "\n";
			f << "ai_url=" << a_s.baseUrl << "\n";
			f << "ai_key=" << a_protectedKey << "\n";
			f << "ai_model=" << a_s.model << "\n";
			f << "ai_ask=" << (a_s.askBeforeChanges ? 1 : 0) << "\n";
			f << "ai_max_tokens=" << a_s.maxTokens << "\n";
			f << "ai_timeout=" << a_s.timeoutSeconds << "\n";
			return static_cast<bool>(f);
		}

		// ---------------------------------------------------------------- what the AI is told
		std::string Section(const std::string& a_text, const char* a_from, const char* a_to)
		{
			const auto a = a_text.find(a_from);
			if (a == std::string::npos) {
				return {};
			}
			const auto b = a_text.find(a_to, a + 1);
			return a_text.substr(a, b == std::string::npos ? std::string::npos : b - a);
		}

		std::string SystemPrompt()
		{
			std::string s =
				"You are the AI assistant of ReSaver Plus (ReSaver++), a save editor and troubleshooting tool for Skyrim made by tonycubed2. "
				"Here you run INSIDE the game: the player talks to you on ReSaverHelper's F1 page (\"Ask the AI\") while Skyrim runs, "
				"with ReSaver Plus itself closed. Most players are not programmers: they describe problems in plain English.\n"
				"\nWHAT YOU HAVE HERE\n"
				"- game_command: ReSaverHelper's commands in the running game (THE RUNNING GAME below). You talk to the helper directly; "
				"it is always connected here. HELLO tells whether a save is loaded.\n"
				"- list_logs, read_log, search_logs: the game's logs (SKSE's log folder with the plugins' logs and crash logs, and the "
				"Papyrus log).\n"
				"- NOT here: save files. You cannot open, check, clean or edit a save, run resaverpp-cli, read plugins or write files. "
				"Of the tools the text below names, get_app_state, run_cli, analyze_logs, suggest_save_name, open_save, find_form and the "
				"rest do not exist here. When a question needs them, say so plainly and tell the player to open ReSaver Plus and ask its AI "
				"(the same AI, with the save tools).\n"
				"- The player has the F1 menu open while talking to you, and the game is paused then. Reading (QUEST, FOLLOWERS, HELLO) "
				"works at once. What runs game scripts (SETSTAGE, STOP, RESET, COMPLETE and the like) may answer \"?\" (still running): "
				"it finishes when the player closes the menu. Say so, and check afterwards with QUEST when asked.\n"
				"- Form IDs: the game's own load-order form IDs (as QUEST and FOLLOWERS show them). If the player names a quest by its "
				"title, say you need its form ID or editor ID (the helper's Quests page on F1 lists them with search) - do not guess IDs.\n"
				"\nHOW TO WORK\n"
				"- Look before you answer: run the commands or read the logs the question is about. Never guess what a command would show.\n"
				"- Plain, short language: what is wrong, why, what you suggest. Plain text, short paragraphs or simple \"- \" lists.\n"
				"- Before a change, say in one sentence what you are about to do. The player may have to approve each change; if one is "
				"denied, do not retry it: ask what they want instead.\n"
				"- After a change in the running game, tell the player to save in the game.\n"
				"- If a command fails, read the error, fix the arguments and try again; after two failures explain what happened.\n";
			const auto lesson = ess::aigame::Lesson(ess::aigame::Game::kSkyrimSE);
			s += "\n" + Section(lesson, "THE RUNNING GAME", "\nRECIPES");
			s += "\nCURRENT STATE\n- ReSaverHelper version " + std::to_string(proto::kHelperVersion) + ", game " +
			     REL::Module::get().version().string() + (Game::InGame() ? ", a save is loaded" : ", no save loaded yet (main menu)") + "\n";
			s += "- logs folder: " + SKSE::log::log_directory().value_or(std::filesystem::path()).string() + "\n";
			return s;
		}

		// ---------------------------------------------------------------- tools
		struct Args
		{
			Value v;
			std::string str(const char* k) const { return v.str(k); }
			long long   num(const char* k, long long d) const
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
				const auto*              x = v.get(k);
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

		std::vector<ai::Tool> Tools()
		{
			return {
				{ "game_command", "Send a command to ReSaverHelper in the running game (see THE RUNNING GAME) and get its reply.",
					R"J({"type":"object","properties":{"command":{"type":"array","items":{"type":"string"},"description":"the command and its arguments, e.g. [\"QUEST\", \"0001F7A3\"] or [\"SETSTAGE\", \"0001F7A3\", \"20\", \"hud\", \"\"]"},"why":{"type":"string","description":"one plain sentence for the player: what this does"}},"required":["command"]})J" },
				{ "list_logs", "The game's log files (SKSE's log folder: the plugins' logs and crash logs; the Papyrus log), newest first.",
					R"J({"type":"object","properties":{"days":{"type":"integer","description":"only logs written in the last days (0 = all, default 0)"}}})J" },
				{ "read_log", "Read a log (or any text file) by lines. Long files: read the end with tail, or a part with from_line and count.",
					R"J({"type":"object","properties":{"path":{"type":"string"},"from_line":{"type":"integer"},"count":{"type":"integer"},"tail":{"type":"integer","description":"the last N lines instead"}},"required":["path"]})J" },
				{ "search_logs", "Find lines containing a text (not case-sensitive) in the game's logs.",
					R"J({"type":"object","properties":{"text":{"type":"string"},"days":{"type":"integer","description":"only logs written in the last days (0 = all, default 0)"}},"required":["text"]})J" },
			};
		}

		struct LogFile
		{
			std::filesystem::path           path;
			std::uintmax_t                  size = 0;
			std::filesystem::file_time_type time;
		};

		std::vector<LogFile> Logs(long long a_days)
		{
			std::vector<std::filesystem::path> dirs;
			if (const auto d = SKSE::log::log_directory()) {
				dirs.push_back(*d);                                     // ...\My Games\Skyrim Special Edition\SKSE
				dirs.push_back(d->parent_path() / "Logs" / "Script");  // the Papyrus log
			}
			std::vector<LogFile> out;
			const auto           now = std::filesystem::file_time_type::clock::now();
			for (const auto& dir : dirs) {
				std::error_code ec;
				for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
					 !ec && it != end; it.increment(ec)) {
					if (it.depth() > 2 || !it->is_regular_file(ec)) {
						continue;
					}
					const auto ext = Lower(it->path().extension().string());
					if (ext != ".log" && ext != ".txt") {
						continue;
					}
					LogFile f{ it->path(), it->file_size(ec), it->last_write_time(ec) };
					if (a_days > 0 && now - f.time > std::chrono::hours(24 * a_days)) {
						continue;
					}
					out.push_back(std::move(f));
				}
			}
			std::sort(out.begin(), out.end(), [](const LogFile& a, const LogFile& b) { return a.time > b.time; });
			return out;
		}

		std::string Age(std::filesystem::file_time_type a_t)
		{
			const auto m = std::chrono::duration_cast<std::chrono::minutes>(std::filesystem::file_time_type::clock::now() - a_t).count();
			if (m < 60) {
				return std::to_string(m) + " min ago";
			}
			if (m < 48 * 60) {
				return std::to_string(m / 60) + " h ago";
			}
			return std::to_string(m / (24 * 60)) + " days ago";
		}

		std::string ReadLines(const std::filesystem::path& a_p, std::size_t a_from, std::size_t a_count, std::size_t a_tail, std::size_t& a_total)
		{
			std::ifstream            f(a_p, std::ios::binary);
			std::vector<std::string> lines;
			for (std::string l; std::getline(f, l);) {
				if (!l.empty() && l.back() == '\r') {
					l.pop_back();
				}
				lines.push_back(std::move(l));
			}
			a_total = lines.size();
			std::size_t b = 0, e = lines.size();
			if (a_tail) {
				b = e > a_tail ? e - a_tail : 0;
			} else {
				b = std::min(lines.size(), a_from - 1);
				if (a_count) {
					e = std::min(lines.size(), b + a_count);
				}
			}
			std::string out;
			for (std::size_t i = b; i < e; ++i) {
				out += std::to_string(i + 1) + ": " + lines[i] + "\n";
				if (out.size() > 30000) {
					out += "... (cut: ask for fewer lines)\n";
					break;
				}
			}
			return out;
		}

		std::string Exec(const ai::ToolCall& a_call, std::string& a_shown, bool& a_error)
		{
			auto fail = [&](std::string m) {
				a_error = true;
				return m;
			};
			Args args;
			try {
				args.v = ess::json::Parse(a_call.args.empty() ? std::string("{}") : a_call.args);
			} catch (const std::exception& e) {
				return fail(std::string("the arguments are not valid JSON: ") + e.what());
			}
			const auto& name = a_call.name;

			if (name == "game_command") {
				auto cmd = args.list("command");
				if (cmd.empty()) {
					return fail("no command given");
				}
				cmd.front() = Upper(cmd.front());
				std::string shown = "game:";
				for (const auto& x : cmd) {
					shown += " " + (x.empty() ? std::string("\"\"") : x);
				}
				a_shown = shown;
				if (cmd.front() == "AICONFIG") {
					return fail("not a command for the AI");
				}
				if (!ess::aigame::HelperReadOnly(cmd.front()) &&
					!Chat().approve(args.str("why").empty() ? std::string("Change something in the running game") : args.str("why"), shown)) {
					return fail("the player did not allow this change");
				}
				std::vector<std::string> fields{ "0" };
				fields.insert(fields.end(), cmd.begin(), cmd.end());
				spdlog::info("Ask the AI: {}", shown);
				const auto  r = Server::Run(fields);
				std::string o;
				for (std::size_t k = 0; k < r.size(); ++k) {
					o += (k ? " | " : "") + r[k];
				}
				a_error = r.empty() || r[0] != "OK";
				return o.empty() ? "no reply" : o;
			}

			if (name == "list_logs") {
				a_shown = "listed the game's logs";
				std::string o;
				for (const auto& f : Logs(args.num("days", 0))) {
					o += f.path.string() + "  " + std::to_string(f.size / 1024) + " KB  " + Age(f.time) + "\n";
					if (o.size() > 30000) {
						o += "... (more)\n";
						break;
					}
				}
				return o.empty() ? std::string("no logs found") : o;
			}

			if (name == "read_log") {
				const std::filesystem::path p = args.str("path");
				std::error_code             ec;
				if (!std::filesystem::is_regular_file(p, ec)) {
					return fail("not a file: " + p.string());
				}
				const auto  tail = static_cast<std::size_t>(std::max(0LL, args.num("tail", 0)));
				const auto  from = static_cast<std::size_t>(std::max(1LL, args.num("from_line", 1)));
				const auto  count = static_cast<std::size_t>(std::max(0LL, args.num("count", 0)));
				std::size_t total = 0;
				auto        text = ReadLines(p, from, count, tail, total);
				a_shown = "read " + p.filename().string();
				return p.string() + " (" + std::to_string(total) + " lines)\n" + text;
			}

			if (name == "search_logs") {
				const auto t = Lower(args.str("text"));
				if (t.empty()) {
					return fail("no text to search for");
				}
				a_shown = "searched the logs for \"" + args.str("text") + "\"";
				std::string o;
				int         hits = 0;
				for (const auto& f : Logs(args.num("days", 0))) {
					std::ifstream in(f.path, std::ios::binary);
					int           n = 0;
					for (std::string l; std::getline(in, l);) {
						++n;
						if (Lower(l).find(t) != std::string::npos) {
							if (!l.empty() && l.back() == '\r') {
								l.pop_back();
							}
							o += f.path.filename().string() + ":" + std::to_string(n) + ": " + l.substr(0, 400) + "\n";
							if (++hits >= 200) {
								return o + "... (200 hits shown; search for something narrower)\n";
							}
						}
					}
				}
				return o.empty() ? std::string("not found") : o;
			}

			return fail("no tool " + name + " here");
		}

		void Setup()
		{
			static bool done = false;
			if (done) {
				return;
			}
			done = true;
			if (const auto d = SKSE::log::log_directory()) {
				ai::SetLogFile(*d / "ReSaverHelper AI.log");
			}
			Chat().setTools(Tools());
			Chat().setExecutor(&Exec);
		}

		void Help(const char* a_text)
		{
			if (im::IsItemHovered(im::ImGuiHoveredFlags_AllowWhenDisabled)) {
				im::BeginTooltip();
				im::PushTextWrapPos(im::GetFontSize() * 30.0f);
				im::TextUnformatted(a_text);
				im::PopTextWrapPos();
				im::EndTooltip();
			}
		}
	}

	void Load()
	{
		Setup();
		std::ifstream f(CfgPath(), std::ios::binary);
		if (!f) {
			spdlog::info("Ask the AI: no settings yet (ReSaver Plus hands them over when it connects)");
			return;
		}
		ai::Settings s;
		std::string  storedKey;
		for (std::string l; std::getline(f, l);) {
			if (!l.empty() && l.back() == '\r') {
				l.pop_back();
			}
			const auto eq = l.find('=');
			if (l.empty() || l[0] == ';' || eq == std::string::npos) {
				continue;
			}
			const auto k = l.substr(0, eq), v = l.substr(eq + 1);
			if (k == "copied") {
				g_copiedOn = v;
			} else if (k == "ai_preset") {
				s.preset = std::clamp(std::atoi(v.c_str()), 0, static_cast<int>(ai::Presets().size()) - 1);
			} else if (k == "ai_api") {
				s.api = v == "anthropic" ? ai::Api::kAnthropic : ai::Api::kOpenAI;
			} else if (k == "ai_url") {
				s.baseUrl = v;
			} else if (k == "ai_key") {
				storedKey = v;
			} else if (k == "ai_model") {
				s.model = v;
			} else if (k == "ai_ask") {
				s.askBeforeChanges = v != "0";
			} else if (k == "ai_max_tokens") {
				s.maxTokens = std::clamp(std::atoi(v.c_str()), 256, 128000);
			} else if (k == "ai_timeout") {
				s.timeoutSeconds = std::clamp(std::atoi(v.c_str()), 30, 3600);
			}
		}
		s.apiKey = net::UnprotectSecret(storedKey);
		g_keyProblem = (!storedKey.empty() && s.apiKey.empty()) ? "the saved API key could not be read (made by another Windows user?)" : "";
		ai::FixApi(s);
		std::scoped_lock l(g_cfgLock);
		Apply(s);
		g_haveConfig = s.configured();
		spdlog::info("Ask the AI: settings loaded ({}, model {}, copied {})", ai::Presets()[s.preset].name, s.model, g_copiedOn);
	}

	std::string Configure(const std::vector<std::string>& a)
	{
		Setup();
		if (a.size() < 8) {
			return "AICONFIG needs 8 fields";
		}
		ai::Settings s;
		s.preset = std::clamp(std::atoi(a[0].c_str()), 0, static_cast<int>(ai::Presets().size()) - 1);
		s.api = a[1] == "anthropic" ? ai::Api::kAnthropic : ai::Api::kOpenAI;
		s.baseUrl = a[2];
		s.apiKey = net::UnprotectSecret(a[3]);
		if (!a[3].empty() && s.apiKey.empty()) {
			return "the API key could not be read here (ReSaver Plus and the game run as different Windows users?)";
		}
		s.model = a[4];
		s.askBeforeChanges = a[5] != "0";
		s.maxTokens = std::clamp(std::atoi(a[6].c_str()), 256, 128000);
		s.timeoutSeconds = std::clamp(std::atoi(a[7].c_str()), 30, 3600);
		ai::FixApi(s);
		std::scoped_lock l(g_cfgLock);
		// the player's own "ask before changes" choice on the F1 page stays (ReSaver Plus only sends its default)
		if (g_haveConfig) {
			s.askBeforeChanges = Chat().busy() ? g_pendingSettings.askBeforeChanges : Chat().settings().askBeforeChanges;
		}
		g_copiedOn = Now();
		if (!Write(s, a[3])) {
			return "could not write " + CfgPath().string();
		}
		Apply(s);
		g_haveConfig = s.configured();
		g_keyProblem.clear();
		spdlog::info("Ask the AI: settings copied from ReSaver Plus ({}, model {})", ai::Presets()[s.preset].name, s.model);
		return {};
	}

	void __stdcall Render()
	{
		Setup();
		auto& chat = Chat();
		chat.pollUi();
		{
			std::scoped_lock l(g_cfgLock);
			if (g_pending && !chat.busy()) {
				chat.settings() = g_pendingSettings;
				g_pending = false;
			}
		}

		bool        have;
		std::string copied, keyProblem;
		{
			std::scoped_lock l(g_cfgLock);
			have = g_haveConfig;
			copied = g_copiedOn;
			keyProblem = g_keyProblem;
		}
		if (!have) {
			im::TextWrapped("%s",
				"Ask the AI needs the AI settings from ReSaver Plus, once:\n"
				"1. In ReSaver Plus, set up Ask the AI (Tools > Ask the AI > Settings) if you have not yet.\n"
				"2. Start ReSaver Plus while the game runs. It connects to the game and hands its AI settings over by itself.\n"
				"After that ReSaver Plus can stay closed: the settings are kept here, the API key encrypted for your Windows user.");
			if (!keyProblem.empty()) {
				im::TextColored(kRed, "%s", keyProblem.c_str());
			}
			return;
		}

		const auto& s = chat.settings();
		im::TextDisabled("%s", std::format("{} - {}   (copied from ReSaver Plus {})", ai::Presets()[s.preset].name, s.model, copied).c_str());
		Help("ReSaver Plus hands its AI settings over every time it connects to the game, so a change there arrives here too.");
		im::SameLine();
		bool ask = s.askBeforeChanges;
		if (im::Checkbox("Ask me before every change", &ask) && !chat.busy()) {
			chat.settings().askBeforeChanges = ask;
			std::ifstream            in(CfgPath(), std::ios::binary);
			std::vector<std::string> lines;
			for (std::string l; std::getline(in, l);) {
				if (!l.empty() && l.back() == '\r') {
					l.pop_back();
				}
				lines.push_back(l.rfind("ai_ask=", 0) == 0 ? std::string("ai_ask=") + (ask ? "1" : "0") : l);
			}
			in.close();
			std::ofstream out(CfgPath(), std::ios::binary | std::ios::trunc);
			for (const auto& l : lines) {
				out << l << "\n";
			}
		}
		Help("On: anything that changes the game (setting a quest stage, stopping a quest, moving followers...) waits for your OK "
			 "here. Reading never asks.");
		im::TextWrapped("%s",
			"Ask about your game in plain English. The AI can look at quests and followers, fix them in the running game, and read "
			"the logs. It cannot open save files: for that, ask ReSaver Plus's AI. The game is paused while this menu is open, so "
			"some fixes finish when you close it.");
		im::Separator();

		// the conversation
		const float inputH = im::GetFontSize() * 4.5f;
		const float barH = im::GetFrameHeightWithSpacing() * 2.2f;
		im::BeginChild("##aichat", im::ImVec2(0, -(inputH + barH)), im::ImGuiChildFlags_Border, 0);
		chat.read([](const std::vector<ai::Message>& a_msgs) {
			if (a_msgs.empty()) {
				im::TextDisabled("%s", "For example: \"Is the quest Blood on the Ice stuck?\"  \"Why did my game crash?\"  \"Bring my followers back.\"");
			}
			for (const auto& m : a_msgs) {
				switch (m.role) {
				case ai::Message::Role::kUser:
					im::TextColored(kGreen, "%s", "You:");
					im::TextWrapped("%s", m.text.c_str());
					im::Spacing();
					break;
				case ai::Message::Role::kAssistant:
					if (!m.text.empty()) {
						im::TextColored(kBlue, "%s", "AI:");
						im::TextWrapped("%s", m.text.c_str());
						im::Spacing();
					}
					break;
				case ai::Message::Role::kTool:
					if (m.error) {
						im::TextColored(kRed, "%s", ("  [" + (m.shown.empty() ? m.toolName : m.shown) + "]  failed").c_str());
					} else {
						im::TextDisabled("%s", ("  [" + (m.shown.empty() ? m.toolName : m.shown) + "]").c_str());
					}
					break;
				case ai::Message::Role::kNote:
					im::TextColored(m.error ? kRed : kYellow, "%s", m.text.c_str());
					break;
				}
			}
		});
		if (chat.revision() != g_seenRevision) {
			g_seenRevision = chat.revision();
			im::SetScrollHereY(1.0f);
		}
		im::EndChild();

		// a change waiting for the OK
		const auto ap = chat.approval();
		if (ap.pending) {
			im::TextColored(kYellow, "%s", ("The AI wants to: " + ap.what).c_str());
			Help(ap.command.c_str());
			if (im::Button("Allow")) {
				chat.answer(1);
			}
			im::SameLine();
			if (im::Button("Don't allow")) {
				chat.answer(0);
			}
			im::SameLine();
			if (im::Button("Allow all changes in this chat")) {
				chat.answer(2);
			}
		} else if (chat.busy()) {
			im::TextColored(kYellow, "%s", chat.status().empty() ? "Working..." : chat.status().c_str());
		} else {
			im::TextDisabled("%s", " ");
		}

		// the question
		im::InputTextMultiline("##aiinput", g_input, sizeof(g_input), im::ImVec2(-1, inputH), 0);
		const bool canSend = !chat.busy() && g_input[0] != '\0';
		im::BeginDisabled(!canSend);
		if (im::Button("Send")) {
			std::string text = g_input;
			g_input[0] = '\0';
			chat.send(text, [] { return SystemPrompt(); });
		}
		im::EndDisabled();
		im::SameLine();
		im::BeginDisabled(!chat.busy());
		if (im::Button("Stop")) {
			chat.stop();
		}
		im::EndDisabled();
		im::SameLine();
		im::BeginDisabled(chat.busy());
		if (im::Button("New chat")) {
			chat.clear();
		}
		im::EndDisabled();
		Help("Starts over: the AI forgets this conversation.");
	}
}
