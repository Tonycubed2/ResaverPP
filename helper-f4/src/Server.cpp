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

		std::vector<std::string> Handle(const std::vector<std::string>& f)
		{
			const auto& cmd = f.size() > 1 ? f[1] : std::string();
			auto        arg = [&](std::size_t i) { return f.size() > i + 2 ? f[i + 2] : std::string(); };
			auto        err = [](std::string m) { return std::vector<std::string>{ "ERR", std::move(m) }; };

			if (cmd == "HELLO") {
				return { "OK", "ReSaverHelper", std::to_string(proto::kHelperVersion), Game::GameVersion(), Game::RuntimeName(),
					Game::InGame() ? "1" : "0" };
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
			if (!Game::InGame()) {
				return err("no game loaded yet (load a save first)");
			}
			if (cmd == "RESCUE") {
				const auto line = Rescue::RunAll();
				Game::Notify(arg(0), line);
				return { "OK", line };
			}
			if (cmd == "GATHER" || cmd == "CONTROLS" || cmd == "CONTROLSRESET" || cmd == "SAVEGAME") {
				// (Everything a main-thread task touches is owned by the task: it can run after this request gave up.)
				auto save = std::make_shared<std::string>();
				if (!Game::RunOnMain(
						[save, cmd] {
							if (cmd == "GATHER") {
								Rescue::Gather();
							} else if (cmd == "CONTROLS") {
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
				if (cmd == "GATHER") {
					// the game's teleport tells nobody whether anyone came: the count is unknown
					Game::Notify(arg(0), "ReSaver++: your companion and Dogmeat were called to you");
					return { "OK", "?", "your companion and Dogmeat (if they are with you)" };
				}
				if (cmd == "CONTROLS") {
					Game::Notify(arg(0), "ReSaver++: your controls are forced back on");
					return { "OK" };
				}
				if (cmd == "CONTROLSRESET") {
					Game::Notify(arg(0), "ReSaver++: your controls are back to the game's own settings");
					return { "OK" };
				}
				Game::Notify(arg(0), "ReSaver++: game saved as " + *save);
				return { "OK", *save };
			}
			return err("not available in the Fallout 4 helper: " + cmd);
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

	void Start()
	{
		std::thread(Serve).detach();
	}
}
