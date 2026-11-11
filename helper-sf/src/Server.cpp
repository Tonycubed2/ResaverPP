#include "PCH.h"
#include "Server.h"
#include "Game.h"
#include "Protocol.h"
#include "Rescue.h"
#include "Settings.h"

namespace Server
{
	namespace
	{
		// how long a request waits for the game's main thread (a heavily modded game can stall for many seconds)
		constexpr auto kWait = 30s;

		std::uint32_t ParseHex(const std::string& a_text)
		{
			try {
				std::size_t used = 0;
				const auto  v = std::stoul(a_text, &used, 16);
				return used == a_text.size() ? static_cast<std::uint32_t>(v) : 0;
			} catch (...) {
				return 0;
			}
		}

		// the quest after a console command: read on the main thread a moment later (the console runs a queued
		// command on its next update, so the state is read on a later frame)
		Game::QuestState QuestAfter(std::uint32_t a_formID)
		{
			std::this_thread::sleep_for(500ms);
			auto st = std::make_shared<Game::QuestState>();
			Game::RunOnMain([st, a_formID] { *st = Game::Quest(a_formID); }, kWait);
			return *st;
		}

		std::vector<std::string> QuestReply(const Game::QuestState& q, const char* a_result)
		{
			return { "OK", a_result, q.running ? "1" : "0", q.completed ? "1" : "0", std::to_string(q.currentStage) };
		}

		std::vector<std::string> Handle(const std::vector<std::string>& f)
		{
			const auto& cmd = f.size() > 1 ? f[1] : std::string();
			auto        arg = [&](std::size_t i) { return f.size() > i + 2 ? f[i + 2] : std::string(); };
			auto        err = [](std::string m) { return std::vector<std::string>{ "ERR", std::move(m) }; };

			if (cmd == "HELLO") {
				return { "OK", "ReSaverHelper", std::to_string(proto::kHelperVersion), Game::GameVersion(), "SF", Game::InGame() ? "1" : "0" };
			}
			if (cmd == "GETOPTION") {
				const auto v = Settings::GetOption(arg(0));
				return v.empty() ? err("unknown option " + arg(0)) : std::vector<std::string>{ "OK", v };
			}
			if (cmd == "SETOPTION") {
				if (!Settings::SetOption(arg(0), arg(1))) {
					return err("invalid option or value: " + arg(0) + "=" + arg(1));
				}
				if (!Settings::Save()) {
					return err("could not write " + Settings::IniPath().string());
				}
				spdlog::info("Option {} set to {} by ReSaver Plus", arg(0), arg(1));
				return { "OK" };
			}
			if (cmd == "NOTIFY") {
				Game::Notify(arg(0), arg(1));
				return { "OK" };
			}
			if (!Game::InGame()) {
				return err("no game loaded yet (load a save first)");
			}
			if (cmd == "QUEST") {
				const auto formID = ParseHex(arg(0));
				auto       st = std::make_shared<Game::QuestState>();
				if (!Game::RunOnMain([st, formID] { *st = Game::Quest(formID); }, kWait)) {
					return err("the game is busy (it did not answer within 30 seconds)");
				}
				const auto& q = *st;
				if (!q.found) {
					return err("no quest " + arg(0) + " in this game");
				}
				// (Starfield: the last field is the quest's stages, index:1 done / index:0 not done; the editor id is not
				// kept by the game, so it is empty)
				std::string stages;
				for (const auto& [i, done] : q.stages) {
					stages += (stages.empty() ? "" : ",") + std::to_string(i) + ":" + (done ? "1" : "0");
				}
				return { "OK", "", std::format("{:08X}", q.flags), std::to_string(q.currentStage), q.running ? "1" : "0", q.completed ? "1" : "0", stages };
			}
			if (cmd == "SETSTAGE" || cmd == "COMPLETE" || cmd == "STOP" || cmd == "RESET") {
				const auto formID = ParseHex(arg(0));
				int        stage = -1;
				if (cmd == "SETSTAGE") {
					try {
						stage = std::stoi(arg(1));
					} catch (...) {
					}
					if (stage < 0 || stage > 0xFFFF) {
						return err("bad stage " + arg(1));
					}
				}
				if (!formID) {
					return err("bad quest " + arg(0));
				}
				auto before = std::make_shared<Game::QuestState>();
				if (!Game::RunOnMain([before, formID] { *before = Game::Quest(formID); }, kWait)) {
					return err("the game is busy (it did not answer within 30 seconds)");
				}
				if (!before->found) {
					return err("no quest " + arg(0) + " in this game");
				}
				const auto line = cmd == "SETSTAGE" ? std::format("setstage {:08X} {}", formID, stage)
				                  : cmd == "COMPLETE" ? std::format("completequest {:08X}", formID)
				                  : cmd == "STOP"     ? std::format("stopquest {:08X}", formID)
				                                      : std::format("resetquest {:08X}", formID);
				auto ok = std::make_shared<std::atomic<bool>>(false);
				if (!Game::RunOnMain([ok, line] { *ok = Game::Console(line); }, kWait)) {
					return { "OK", "?", "", "", "" };  // queued: the game is busy and runs it when it can
				}
				if (!*ok) {
					return err("the console could not run: " + line);
				}
				const auto q = QuestAfter(formID);
				const std::size_t notifyArg = cmd == "SETSTAGE" ? 2 : 1;
				bool              done = false;
				if (cmd == "SETSTAGE") {
					for (const auto& [i, d] : q.stages) {
						done = done || (i == stage && d);
					}
				} else if (cmd == "COMPLETE") {
					done = q.completed;
				} else if (cmd == "STOP") {
					done = !q.running;
				} else {
					done = true;  // (reset: nothing simple tells; the quest is as its plugin starts it)
				}
				const auto message = arg(notifyArg + 1);
				Game::Notify(arg(notifyArg), message.empty() ? "ReSaver Plus: " + line : message);
				return QuestReply(q, done ? "1" : "?");
			}
			if (cmd == "RESCUE") {
				const auto line = Rescue::RunAll();
				Game::Notify(arg(0), line);
				return { "OK", line };
			}
			if (cmd == "CONTROLS" || cmd == "CONTROLSRESET" || cmd == "SAVEGAME") {
				// (Everything a main-thread task touches is owned by the task: it can run after this request gave up.)
				auto save = std::make_shared<std::string>();
				if (!Game::RunOnMain(
						[save, cmd] {
							if (cmd == "CONTROLS") {
								Rescue::RestoreControls();
							} else if (cmd == "CONTROLSRESET") {
								Rescue::ResetControls();
							} else {
								*save = Rescue::SaveGame();
							}
						},
						kWait)) {
					return err("the game is busy (it did not answer within 30 seconds)");
				}
				if (cmd == "CONTROLS") {
					Game::Notify(arg(0), "ReSaver Plus: your controls are forced back on");
					return { "OK" };
				}
				if (cmd == "CONTROLSRESET") {
					Game::Notify(arg(0), "ReSaver Plus: your controls are back to the game's own settings");
					return { "OK" };
				}
				Game::Notify(arg(0), "ReSaver Plus: game saved as " + *save);
				return { "OK", *save };
			}
			return err("not available in the Starfield helper: " + cmd);
		}

		// security attributes: only the current user (and SYSTEM) may open the pipe
		bool OwnerOnly(SECURITY_ATTRIBUTES& a_sa)
		{
			HANDLE token = nullptr;
			if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
				return false;
			}
			std::vector<std::uint8_t> buf(512);
			DWORD                     len = 0;
			const bool                ok = GetTokenInformation(token, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &len);
			CloseHandle(token);
			if (!ok) {
				return false;
			}
			LPWSTR sid = nullptr;
			if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
				return false;
			}
			const std::wstring   sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")(A;;GA;;;SY)";
			LocalFree(sid);
			PSECURITY_DESCRIPTOR sd = nullptr;
			if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
				return false;
			}
			a_sa.nLength = sizeof(a_sa);
			a_sa.lpSecurityDescriptor = sd;  // kept for the life of the process
			a_sa.bInheritHandle = FALSE;
			return true;
		}

		void Serve()
		{
			SECURITY_ATTRIBUTES sa{};
			if (!OwnerOnly(sa)) {
				spdlog::error("Server: could not build owner-only pipe security; not listening");
				return;
			}
			spdlog::info("Server: listening on \\\\.\\pipe\\ReSaverHelper");
			while (true) {
				if (!Settings::Get().listen) {
					std::this_thread::sleep_for(2s);
					continue;
				}
				HANDLE pipe = CreateNamedPipeW(proto::kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
					64 * 1024, 64 * 1024, 0, &sa);
				if (pipe == INVALID_HANDLE_VALUE) {
					spdlog::error("Server: CreateNamedPipe failed ({})", GetLastError());
					std::this_thread::sleep_for(5s);
					continue;
				}
				const bool connected = ConnectNamedPipe(pipe, nullptr) ? true : (GetLastError() == ERROR_PIPE_CONNECTED);
				if (connected) {
					spdlog::info("Server: ReSaver Plus connected");
					std::string pending;
					char        buf[4096];
					DWORD       got = 0;
					while (ReadFile(pipe, buf, sizeof(buf), &got, nullptr) && got) {
						pending.append(buf, got);
						for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos;) {
							const auto line = pending.substr(0, nl);
							pending.erase(0, nl + 1);
							const auto fields = proto::Split(line);
							const auto cmd = fields.size() > 1 ? fields[1] : std::string();
							const bool quiet = cmd == "HELLO";
							if (!quiet) {
								std::string what;
								for (std::size_t i = 1; i < fields.size() && i < 5; ++i) {
									what += (i > 1 ? " " : "") + fields[i];
								}
								spdlog::info("Request: {}", what);
							}
							auto reply = Handle(fields);
							if (!quiet || (!reply.empty() && reply[0] != "OK")) {
								spdlog::info("Reply to {}: {}{}", cmd, reply.empty() ? "?" : reply[0], reply.size() > 1 ? " " + reply[1] : "");
							}
							reply.insert(reply.begin(), fields.empty() ? std::string("0") : fields[0]);
							const auto out = proto::Join(reply) + "\n";
							DWORD      wrote = 0;
							WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr);
						}
						if (pending.size() > 1024 * 1024) {
							break;  // not a ReSaver Plus client
						}
					}
					spdlog::info("Server: ReSaver Plus disconnected");
				}
				DisconnectNamedPipe(pipe);
				CloseHandle(pipe);
			}
		}
	}

	void Start()
	{
		std::thread(Serve).detach();
	}
}
