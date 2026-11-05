#include "PCH.h"
#include "F4SE.h"
#include "Game.h"
#include "Protocol.h"
#include "Rescue.h"
#include "Server.h"
#include "Settings.h"

// ReSaverHelper for Fallout 4: one DLL for 1.10.163 (F4SE 0.6.23, which asks F4SEPlugin_Query) and 1.10.984 /
// 1.11.x (F4SE 0.7.x, which reads F4SEPlugin_Version). Same pipe and command format as the Skyrim helpers.

namespace
{
	f4se::PluginHandle            g_handle = f4se::kPluginHandle_Invalid;
	f4se::F4SEMessagingInterface* g_messaging = nullptr;
	bool                          g_started = false;

	void OnMessage(f4se::F4SEMessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case f4se::F4SEMessagingInterface::kMessage_GameDataReady:
			if (!g_started && a_msg->data) {  // (true once the data has finished loading)
				g_started = true;
				Server::Start();
				Rescue::StartHotkey();
			}
			break;
		case f4se::F4SEMessagingInterface::kMessage_PreLoadGame:
			Game::SetInGame(false);
			break;
		case f4se::F4SEMessagingInterface::kMessage_PostLoadGame:
			Game::SetInGame(a_msg->data != nullptr);  // true when the load succeeded
			break;
		case f4se::F4SEMessagingInterface::kMessage_NewGame:
			Game::SetInGame(true);
			break;
		default:
			break;
		}
	}

	bool Load(const f4se::F4SEInterface* a_f4se)
	{
		rshlog::Open();
		spdlog::info("ReSaverHelper for Fallout 4, protocol {} - F4SE {:08X}, game {:08X}", proto::kHelperVersion, a_f4se->f4seVersion,
			a_f4se->runtimeVersion);
		if (a_f4se->isEditor) {
			return false;
		}
		g_handle = a_f4se->GetPluginHandle();
		Settings::Load();
		if (!Game::Init(a_f4se->runtimeVersion, static_cast<f4se::F4SETaskInterface*>(a_f4se->QueryInterface(f4se::kInterface_Task)))) {
			spdlog::error("the helper does not start (see above)");
			return false;
		}
		spdlog::info("Fallout 4 {} ({})", Game::GameVersion(), Game::RuntimeName());
		g_messaging = static_cast<f4se::F4SEMessagingInterface*>(a_f4se->QueryInterface(f4se::kInterface_Messaging));
		if (!g_messaging || !g_messaging->RegisterListener(g_handle, "F4SE", OnMessage)) {
			spdlog::error("F4SE messaging unavailable; the helper cannot start");
			return false;
		}
		return true;
	}
}

extern "C"
{
	// F4SE 0.7.x (1.10.984, 1.11.x): what the plugin works with, read before anything is called
	__declspec(dllexport) f4se::F4SEPluginVersionData F4SEPlugin_Version = {
		f4se::F4SEPluginVersionData::kVersion,
		proto::kHelperVersion,
		"ReSaverHelper",
		"tonycubed2",
		f4se::F4SEPluginVersionData::kAddressIndependence_AddressLibrary_1_10_980 | f4se::F4SEPluginVersionData::kAddressIndependence_AddressLibrary_1_11_137,
		f4se::F4SEPluginVersionData::kStructureIndependence_1_10_980Layout | f4se::F4SEPluginVersionData::kStructureIndependence_1_11_137Layout,
		{ 0 },
		0,
		0,
		0,
		{ 0 },
	};

	// F4SE 0.6.23 (1.10.163) asks first
	__declspec(dllexport) bool F4SEPlugin_Query(const f4se::F4SEInterface* a_f4se, f4se::PluginInfo* a_info)
	{
		a_info->infoVersion = f4se::PluginInfo::kInfoVersion;
		a_info->name = "ReSaverHelper";
		a_info->version = proto::kHelperVersion;
		return !a_f4se->isEditor && a_f4se->runtimeVersion == f4se::kRuntime_1_10_163;
	}

	__declspec(dllexport) bool F4SEPlugin_Load(const f4se::F4SEInterface* a_f4se) { return Load(a_f4se); }
}
