#include "PCH.h"
#include "Server.h"
#include "Game.h"
#include "LE.h"
#include "Protocol.h"
#include "Settings.h"
#include "Transplant.h"

// SKSE's IPrefix.h targets Windows XP (_WIN32_WINNT 0x0501), which can hide this Vista flag
#ifndef PIPE_REJECT_REMOTE_CLIENTS
#	define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

// The same commands and replies as the SE helper (helper\src\Server.cpp); see Protocol.h.
namespace Server
{
	namespace
	{
		std::string g_gameVersion = "1.9.32.0";

		// how long a request waits for the game's main thread (a heavily modded game can stall for many seconds)
		constexpr auto kWait = 30s;

		std::uint32_t ParseHex(const std::string& s)
		{
			try {
				return static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
			} catch (...) {
				return 0;
			}
		}

		std::vector<std::string> Handle(const std::vector<std::string>& f)
		{
			const auto& cmd = f.size() > 1 ? f[1] : std::string();
			auto        arg = [&](std::size_t i) { return f.size() > i + 2 ? f[i + 2] : std::string(); };
			auto        err = [](std::string m) { return std::vector<std::string>{ "ERR", std::move(m) }; };

			if (cmd == "HELLO") {
				return { "OK", "ReSaverHelper", std::to_string(proto::kHelperVersion), g_gameVersion, "LE", Game::InGame() ? "1" : "0" };
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
				spdlog::info("Option {} set to {} by ReSaver++", arg(0), arg(1));
				return { "OK" };
			}
			if (cmd == "NOTIFY") {
				Game::Notify(arg(0), arg(1));
				return { "OK" };
			}
			if (cmd == "PROGRESS") {
				const auto p = Game::RefreshProgress();
				return { "OK", std::to_string(p.done), std::to_string(p.total), p.running ? "1" : "0" };
			}
			if (!Game::InGame()) {
				return err("no game loaded yet (load a save first)");
			}
			// Everything a main-thread task touches is owned by the task (values and shared_ptr copies),
			// never by this function's stack: when the game is busy the task can run long after this
			// request has given up waiting.
			if (cmd == "QUEST") {
				auto       st = std::make_shared<Game::QuestInfo>();
				const auto formID = ParseHex(arg(0));
				if (!Game::RunOnMain([st, formID] { *st = Game::GetQuest(formID); }, kWait)) {
					return err("the game is busy (it did not answer within 30 seconds)");
				}
				const auto& q = *st;
				if (!q.found) {
					return err("no quest " + arg(0) + " in this game");
				}
				std::string objs;
				for (const auto& [i, o] : q.objectives) {
					objs += (objs.empty() ? "" : ",") + std::to_string(i) + ":" + std::to_string(o);
				}
				return { "OK", q.editorID, std::format("{:04X}", q.flags), std::to_string(q.currentStage), q.running ? "1" : "0",
					q.completed ? "1" : "0", objs };
			}
			if (cmd == "SETSTAGE" || cmd == "STOP") {
				const auto formID = ParseHex(arg(0));
				const bool stop = cmd == "STOP";
				int        stage = -1;
				if (!stop) {
					try {
						stage = std::stoi(arg(1));
					} catch (...) {
					}
				}
				if (!formID || (!stop && (stage < 0 || stage > 0xFFFF))) {
					return err(stop ? "bad quest" : "bad quest or stage");
				}
				struct Outcome
				{
					bool             ok = false;
					Game::QuestState state;
				};
				auto       result = std::make_shared<std::promise<Outcome>>();
				auto       fut = result->get_future();
				const auto notify = stop ? arg(1) : arg(2), message = stop ? arg(2) : arg(3);
				const auto st16 = static_cast<std::uint16_t>(stage < 0 ? 0 : stage);
				auto       reply = [formID, result, notify, message, stop](bool ok) {
                    Game::Notify(notify, ok ? (message.empty() ? (stop ? "ReSaverHelper: quest stopped" : "ReSaverHelper: quest stage set") : message)
										  : (stop ? "ReSaverHelper: the quest could not be stopped" : "ReSaverHelper: the game refused that stage"));
                    Outcome o;
                    o.ok = ok;
                    o.state = Game::StateOf(formID);
                    try {
                        result->set_value(o);
                    } catch (...) {
                    }
				};
				const bool ran = Game::RunOnMain(
					[formID, st16, stop, reply] {
						if (stop) {
							Game::Stop(formID, reply);
						} else {
							Game::SetStage(formID, st16, reply);
						}
					},
					kWait);
				if (!ran || fut.wait_for(15s) != std::future_status::ready) {
					return { "OK", "?" };  // queued or still running: it continues in the game
				}
				const auto o = fut.get();
				return { "OK", o.ok ? "1" : "0", o.state.running ? "1" : "0", o.state.completed ? "1" : "0", std::to_string(o.state.stage) };
			}
			if (cmd == "COMPLETE") {
				const auto formID = ParseHex(arg(0));
				if (!formID) {
					return err("bad quest");
				}
				auto       result = std::make_shared<std::promise<bool>>();
				auto       fut = result->get_future();
				const auto notify = arg(1), message = arg(2);
				const bool ran = Game::RunOnMain(
					[formID, result, notify, message] {
						Game::Complete(formID, [result, notify, message](bool ok) {
							Game::Notify(notify, ok ? (message.empty() ? "ReSaverHelper: quest marked completed" : message) : "ReSaverHelper: the quest could not be marked completed");
							try {
								result->set_value(ok);
							} catch (...) {
							}
						});
					},
					kWait);
				if (!ran || fut.wait_for(15s) != std::future_status::ready) {
					return { "OK", "?" };
				}
				return { "OK", fut.get() ? "1" : "0" };
			}
			if (cmd == "SNAPSHOT") {
				struct State
				{
					std::filesystem::path path;
					std::string           error;
					std::size_t           lines = 0;
				};
				auto st = std::make_shared<State>();
				if (!Game::RunOnMain([st] { st->path = Transplant::Snapshot(st->error, st->lines); }, 60s)) {
					return err("the game is busy (it did not answer within 60 seconds)");
				}
				if (st->path.empty()) {
					return err(st->error);
				}
				return { "OK", st->path.string(), std::to_string(st->lines) };
			}
			if (cmd == "APPLY") {
				auto       why = std::make_shared<std::string>("the game did not run it");
				const auto record = std::vector<std::string>(f.begin() + 2, f.end());
				if (!Game::RunOnMain([why, record] {
						*why = Transplant::Apply(record);
						std::string what;
						for (std::size_t i = 0; i < record.size() && i < 6; ++i) {
							what += (i ? " " : "") + record[i];
						}
						spdlog::info("Apply {}: {}", what, why->empty() ? "done" : *why);
					}, kWait)) {
					return { "OK", "?" };
				}
				return why->empty() ? std::vector<std::string>{ "OK", "1" } : std::vector<std::string>{ "OK", "0", *why };
			}
			if (cmd == "RESOLVE") {
				auto       id = std::make_shared<std::uint32_t>(0);
				const auto plugin = arg(0), local = arg(1);
				if (!Game::RunOnMain([id, plugin, local] {
						auto* form = le::Lookup(plugin, local);
						*id = form ? form->formID : 0;
					}, kWait)) {
					return err("the game is busy");
				}
				if (!*id) {
					return err(local + " from " + plugin + " is not in this game");
				}
				return { "OK", std::format("{:08X}", *id) };
			}
			if (cmd == "REFRESH") {
				auto       n = std::make_shared<std::atomic<std::size_t>>(0);
				const auto notify = arg(0);
				auto       blocked = std::make_shared<std::atomic<bool>>(false);
				if (!Game::RunOnMain([blocked] { blocked->store(Game::MenuBlocksRefresh()); }, kWait)) {
					return err("the game is busy (it did not answer within 30 seconds)");
				}
				if (blocked->load()) {
					return err("close the game's menus first (the journal is refreshed only while you are playing)");
				}
				if (!Game::RunOnMain([n, notify] { n->store(Game::StartRefresh(notify)); }, kWait)) {
					return err("the game is busy (it did not answer within 30 seconds); the refresh starts when it can");
				}
				return { "OK", std::to_string(n->load()) };
			}
			return err("unknown command " + cmd);
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
			const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")(A;;GA;;;SY)";
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
					spdlog::info("Server: ReSaver++ connected");
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
							const bool quiet = cmd == "HELLO" || cmd == "APPLY" || cmd == "PROGRESS";  // APPLY logs itself
							if (!quiet) {
								std::string what;
								for (std::size_t i = 1; i < fields.size() && i < 5; ++i) {
									what += (i > 1 ? " " : "") + fields[i];
								}
								spdlog::info("Request: {}", what);
							}
							std::vector<std::string> reply;
							try {
								reply = Handle(fields);
							} catch (const std::exception& e) {
								reply = { "ERR", std::string("the helper failed: ") + e.what() };
							} catch (...) {
								reply = { "ERR", "the helper failed (see ReSaverHelper.log)" };
							}
							if (!quiet || (!reply.empty() && reply[0] != "OK")) {
								spdlog::info("Reply to {}: {}{}", cmd, reply.empty() ? "?" : reply[0], reply.size() > 1 ? " " + reply[1] : "");
							}
							reply.insert(reply.begin(), fields.empty() ? std::string("0") : fields[0]);
							const auto out = proto::Join(reply) + "\n";
							DWORD      wrote = 0;
							WriteFile(pipe, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr);
						}
						if (pending.size() > 1024 * 1024) {
							break;  // not a ReSaver++ client
						}
					}
					spdlog::info("Server: ReSaver++ disconnected");
				}
				DisconnectNamedPipe(pipe);
				CloseHandle(pipe);
			}
		}
	}

	void SetGameVersion(std::string a_version) { g_gameVersion = std::move(a_version); }

	void Start()
	{
		std::thread(Serve).detach();
	}
}
