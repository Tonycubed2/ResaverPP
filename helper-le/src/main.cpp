#include "PCH.h"
#include "Game.h"
#include "Protocol.h"
#include "Server.h"
#include "Settings.h"

// ReSaverHelper for Skyrim Legendary Edition: an SKSE 1.7.3 plugin (TESV.exe 1.9.32.0).
// Same pipe, same commands as the SE/AE/VR helper, so ReSaver++ talks to either one the same way.

namespace
{
	PluginHandle            g_handle = kPluginHandle_Invalid;
	SKSEMessagingInterface* g_messaging = nullptr;

	void OnMessage(SKSEMessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSEMessagingInterface::kMessage_DataLoaded:
			Server::Start();
			break;
		case SKSEMessagingInterface::kMessage_PreLoadGame:
			Game::SetInGame(false);
			break;
		case SKSEMessagingInterface::kMessage_PostLoadGame:
			if (a_msg->data) {  // true when the load succeeded
				Game::OnGameLoaded();
			}
			break;
		case SKSEMessagingInterface::kMessage_NewGame:
			Game::SetInGame(true);
			break;
		default:
			break;
		}
	}

	std::string Version(UInt32 a_runtime)
	{
		return std::format("1.{}.{}.{}", (a_runtime >> 24) & 0xFF, (a_runtime >> 16) & 0xFF, (a_runtime >> 4) & 0xFFF);
	}
}

extern "C"
{
	__declspec(dllexport) bool SKSEPlugin_Query(const SKSEInterface* a_skse, PluginInfo* a_info)
	{
		rshlog::Open();
		a_info->infoVersion = PluginInfo::kInfoVersion;
		a_info->name = "ReSaverHelper";
		a_info->version = proto::kHelperVersion;
		spdlog::info("ReSaverHelper (Legendary Edition) version {} - game {}", proto::kHelperVersion, Version(a_skse->runtimeVersion));
		if (a_skse->isEditor) {
			return false;
		}
		if (a_skse->runtimeVersion != RUNTIME_VERSION_1_9_32_0) {
			spdlog::error("This helper is for Skyrim 1.9.32.0 (the last Legendary Edition patch); not loaded");
			return false;
		}
		g_handle = a_skse->GetPluginHandle();
		return true;
	}

	__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* a_skse)
	{
		Server::SetGameVersion(Version(a_skse->runtimeVersion));
		Settings::Load();
		Game::SetTaskInterface(static_cast<SKSETaskInterface*>(a_skse->QueryInterface(kInterface_Task)));
		g_messaging = static_cast<SKSEMessagingInterface*>(a_skse->QueryInterface(kInterface_Messaging));
		if (!g_messaging || !g_messaging->RegisterListener(g_handle, "SKSE", OnMessage)) {
			spdlog::error("SKSE messaging unavailable; the helper cannot start");
			return false;
		}
		return true;
	}
}
