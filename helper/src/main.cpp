#include "PCH.h"

#include <cctype>
#include "Game.h"
#include "MenuGuard.h"
#include "MenuPage.h"
#include "Protocol.h"
#include "Rescue.h"
#include "Server.h"
#include "Settings.h"

namespace
{
	// Diagnostic stages (iDiagnosticStage in ReSaverHelper.ini, normally absent = -1 = run normally). Each stage stops
	// the helper after one more step of what it does at load, so a user can find which step (if any) changes their game:
	//   0: nothing at all (not even SKSE's own setup): only the DLL being loaded
	//   1: + SKSE setup, log file, settings
	//   2: + the SKSE message listener (it ignores every message)
	//   3: + the listener tracks loads / new games (no threads)
	//   4: + the ReSaver Plus connection thread (no Ctrl hotkey thread)
	//   5 or -1: everything (normal), including the menu load guard (version 16, its hooks)
	// Read straight from the file before anything else runs, so stage 0 really does nothing.
	int g_diag = -1;

	int ReadDiagnosticStage()
	{
		std::ifstream f(Settings::IniPath());  // next to the DLL (plain Windows calls, nothing from SKSE)
		std::string   line;
		while (std::getline(f, line)) {
			auto eq = line.find('=');
			if (eq == std::string::npos) {
				continue;
			}
			std::string key = line.substr(0, eq);
			std::erase_if(key, [](unsigned char c) { return std::isspace(c); });
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (key == "idiagnosticstage") {
				try {
					return std::stoi(line.substr(eq + 1));
				} catch (...) {
					return -1;
				}
			}
		}
		return -1;
	}

	bool Diag(int a_stage) { return g_diag >= 0 && g_diag < 5 && g_diag < a_stage; }  // true: stop before a_stage

	void SetupLog()
	{
		auto dir = SKSE::log::log_directory();
		if (!dir) {
			return;
		}
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "ReSaverHelper.log").string(), true);
		auto log = std::make_shared<spdlog::logger>("ReSaverHelper", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		log->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
		spdlog::set_default_logger(std::move(log));
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg || Diag(3)) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			// version 15: the in-game page in SKSE Menu Framework (F1), only at the normal stage. Every SKSE plugin is loaded by
			// now, so SKSEMenuFramework.dll is found if it is installed; without it nothing more happens.
			if (!Diag(5)) {
				MenuPage::Register();
				// version 16: hooked after every plugin has put its own hooks in, so the log shows who else hooks LoadMovie
				MenuGuard::Install();
			}
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			if (!Diag(4)) {
				Server::Start();
			}
			if (!Diag(5)) {
				Rescue::StartHotkey();
				MenuGuard::WrapCreators("after the game data loaded");
			}
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			Game::SetInGame(false);
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			if (a_msg->data) {  // true when the load succeeded
				Game::OnGameLoaded();
			}
			if (!Diag(5)) {
				MenuGuard::WrapCreators("after a save load");  // menus other mods registered later
			}
			break;
		case SKSE::MessagingInterface::kNewGame:
			Game::SetInGame(true);
			if (!Diag(5)) {
				MenuGuard::WrapCreators("new game");
			}
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	g_diag = ReadDiagnosticStage();
	if (Diag(1)) {
		return true;  // stage 0: loaded, nothing else
	}
	SKSE::Init(a_skse);
	SetupLog();
	spdlog::info("ReSaverHelper version {} - game {}", proto::kHelperVersion, REL::Module::get().version().string());
	if (g_diag >= 0 && g_diag < 5) {
		spdlog::info("Diagnostic stage {}: the helper stops after step {} (see the README)", g_diag, g_diag);
	}
	Settings::Load();
	Settings::SetStartupDiagnosticStage(g_diag >= 0 && g_diag < 5 ? g_diag : -1);
	if (Diag(2)) {
		return true;
	}
	if (auto messaging = SKSE::GetMessagingInterface(); !messaging || !messaging->RegisterListener(OnMessage)) {
		spdlog::error("SKSE messaging unavailable; the helper cannot start");
		return false;
	}
	if (!Diag(5)) {
		MenuGuard::Prepare();  // version 16: the hook trampoline is reserved while SKSE loads plugins
	}
	return true;
}
