#pragma once

// The in-game "ReSaver Plus" page (version 15) in SKSE Menu Framework's Mod Control Panel (F1 by default; Nexus 120352).
// Optional: when SKSEMenuFramework.dll is not loaded, Register() only writes one log line and the helper works exactly
// as before. The framework draws the page (the helper adds no hook of its own); the page reads copies of game data that
// SKSE tasks make on the main thread (LiveInfo.h), and every button queues its game action as an SKSE task.
namespace MenuPage
{
	// at kPostLoad (every SKSE plugin is loaded by then), only at the normal diagnostic stage
	void Register();
}
