#include "PCH.h"
#include "SFSE.h"
#include "Game.h"
#include "Protocol.h"
#include "Rescue.h"
#include "Server.h"
#include "Settings.h"

// ReSaverHelper for Starfield: an SFSE plugin (SFSE 0.2.21 for 1.16.244, and later SFSE versions as long as the
// Address Library has the ids Game.h names). Same pipe and command format as the Skyrim and Fallout 4 helpers.

namespace
{
	sfse::PluginHandle              g_handle = sfse::kPluginHandle_Invalid;
	sfse::SFSEMessagingInterface*   g_messaging = nullptr;
	bool                            g_started = false;

	void OnMessage(sfse::SFSEMessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case sfse::SFSEMessagingInterface::kMessage_PostDataLoad:
			if (!g_started) {
				g_started = true;
				Server::Start();
				Rescue::StartHotkey();
			}
			break;
		case sfse::SFSEMessagingInterface::kMessage_PreLoadGame:
			Game::SetInGame(false);
			break;
		case sfse::SFSEMessagingInterface::kMessage_PostLoadGame:
			// (SFSE sends it after the game's load function returns, whatever it returned; the save's name is the data)
			Game::SetInGame(true);
			spdlog::info("game loaded: {}", a_msg->data ? std::string(static_cast<const char*>(a_msg->data), a_msg->dataLen) : std::string());
			break;
		case sfse::SFSEMessagingInterface::kMessage_PostSaveGame:
			Game::SetInGame(true);  // (a new game is never "loaded": its first save tells the helper a game runs)
			break;
		default:
			break;
		}
	}
}

extern "C"
{
	// what SFSE reads before loading the plugin: Address Library ids (version 2 = 1.15.216+) and the 1.14.70+ layout of
	// the game's forms (the quest fields in Game.h)
	__declspec(dllexport) sfse::SFSEPluginVersionData SFSEPlugin_Version = {
		sfse::SFSEPluginVersionData::kVersion,
		proto::kHelperVersion,
		"ReSaverHelper",
		"tonycubed2",
		sfse::SFSEPluginVersionData::kAddressIndependence_AddressLibraryV2,
		sfse::SFSEPluginVersionData::kStructureIndependence_1_14_70_Layout,
		{ sfse::kRuntime_1_16_244, 0 },
		0,
		0,
		0,
	};

	__declspec(dllexport) bool SFSEPlugin_Load(const sfse::SFSEInterface* a_sfse)
	{
		rshlog::Open();
		spdlog::info("ReSaverHelper for Starfield, version {} - SFSE {:08X}, game {:08X}", proto::kHelperVersion, a_sfse->sfseVersion, a_sfse->runtimeVersion);
		g_handle = a_sfse->GetPluginHandle();
		Settings::Load();
		if (!Game::Init(a_sfse->runtimeVersion, static_cast<sfse::SFSETaskInterface*>(a_sfse->QueryInterface(sfse::kInterface_Task)))) {
			spdlog::error("the helper does not start (see above)");
			return false;
		}
		spdlog::info("Starfield {}", Game::GameVersion());
		g_messaging = static_cast<sfse::SFSEMessagingInterface*>(a_sfse->QueryInterface(sfse::kInterface_Messaging));
		if (!g_messaging || !g_messaging->RegisterListener(g_handle, "SFSE", OnMessage)) {
			spdlog::error("SFSE messaging unavailable; the helper cannot start");
			return false;
		}
		return true;
	}
}
