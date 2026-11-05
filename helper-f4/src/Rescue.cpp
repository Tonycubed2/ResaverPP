#include "PCH.h"
#include "Rescue.h"
#include "Game.h"
#include "Settings.h"

#include <ctime>

namespace Rescue
{
	std::string SaveGame()
	{
		const auto now = std::time(nullptr);
		std::tm    tm{};
		localtime_s(&tm, &now);
		char name[64];
		std::strftime(name, sizeof(name), "ReSaverPP_Rescue_%Y-%m-%d_%H%M%S", &tm);
		Game::Console(std::string("save ") + name);
		return name;
	}

	void RestoreControls() { Game::Console("ForceEnablePlayerControls"); }

	void ResetControls() { Game::Console("ResetForceEnabledPlayerControls"); }

	void Gather()
	{
		// (Followers: the vanilla follower quest; its script's own teleport, with the teleport effects)
		Game::Console("cqf Followers \"FollowersScript.TryToTeleportCompanion\" player 1 1");
		Game::Console("cqf Followers \"FollowersScript.TryToTeleportDogmeat\" player 1 1");
	}

	std::string RunAll()
	{
		auto save = std::make_shared<std::string>();
		if (!Game::RunOnMain(
				[save] {
					*save = SaveGame();
					RestoreControls();
				},
				60s)) {
			return "ReSaver rescue: the game is busy";
		}
		std::this_thread::sleep_for(3s);
		Game::RunOnMain([] { Gather(); }, 60s);
		return std::format("ReSaver rescue: saved as {}, controls forced on, your companion and Dogmeat called to you", *save);
	}

	void StartHotkey()
	{
		std::thread([] {
			using clock = std::chrono::steady_clock;
			std::optional<clock::time_point> down;
			bool                             fired = false, warned = false;
			while (true) {
				std::this_thread::sleep_for(100ms);
				const auto& s = Settings::Get();
				bool        held = false;
				if (s.rescueHotkey && Game::InGame()) {
					// only while Fallout 4 is the window in front, so Ctrl held in another program does nothing
					DWORD pid = 0;
					if (HWND w = GetForegroundWindow()) {
						GetWindowThreadProcessId(w, &pid);
					}
					held = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
				}
				if (!held) {
					down.reset();
					fired = warned = false;
					continue;
				}
				if (!down) {
					down = clock::now();
				}
				if (fired) {
					continue;  // once per hold: let go of Ctrl to do it again
				}
				const double secs = std::chrono::duration<double>(clock::now() - *down).count();
				const int    need = s.rescueHoldSeconds;
				if (secs >= need) {
					fired = true;
					spdlog::info("rescue: Ctrl held {} seconds", need);
					std::thread([] {
						const auto msg = RunAll();
						spdlog::info("{}", msg);
						Game::Notify("hud", msg);
					}).detach();
				} else if (!warned && need >= 5 && secs >= need - 3) {
					warned = true;
					Game::Notify("hud", "ReSaver rescue: keep holding Ctrl to save, get your controls back and call your companion");
				}
			}
		}).detach();
	}
}
