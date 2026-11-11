#pragma once
// The parts of SFSE's plugin API (sfse/PluginAPI.h, SFSE 0.2.21 for Starfield 1.16.244) this helper uses, declared
// here so it builds without the SFSE sources. Layouts copied from github.com/ianpatt/sfse (sfse/PluginAPI.h):
// the structures only ever gain fields at the end.

#include <cstdint>

namespace sfse
{
	using PluginHandle = std::uint32_t;
	inline constexpr PluginHandle kPluginHandle_Invalid = 0xFFFFFFFF;

	enum : std::uint32_t
	{
		kInterface_Invalid = 0,
		kInterface_Messaging,
		kInterface_Trampoline,
		kInterface_Menu,
		kInterface_Task,
	};

	// sfse_version.h: MAKE_EXE_VERSION (major << 24 | minor << 16 | build << 4)
	constexpr std::uint32_t MakeExeVersion(std::uint32_t a_major, std::uint32_t a_minor, std::uint32_t a_build)
	{
		return ((a_major & 0xFF) << 24) | ((a_minor & 0xFF) << 16) | ((a_build & 0xFFF) << 4);
	}
	inline constexpr std::uint32_t kRuntime_1_16_244 = MakeExeVersion(1, 16, 244);

	struct SFSEInterface
	{
		std::uint32_t sfseVersion;
		std::uint32_t runtimeVersion;
		std::uint32_t interfaceVersion;
		void* (*QueryInterface)(std::uint32_t a_id);
		PluginHandle (*GetPluginHandle)();
		const void* (*GetPluginInfo)(const char* a_name);
	};

	struct SFSEMessagingInterface
	{
		struct Message
		{
			const char*   sender;
			std::uint32_t type;
			std::uint32_t dataLen;
			void*         data;
		};
		using EventCallback = void (*)(Message* a_msg);

		// sent by SFSE (sender "SFSE"); the load / save ones carry the save's name
		enum : std::uint32_t
		{
			kMessage_PostLoad,
			kMessage_PostPostLoad,
			kMessage_PostDataLoad,      // all game data loaded (data: TESDataHandler*)
			kMessage_PostPostDataLoad,
			kMessage_PreSaveGame,
			kMessage_PostSaveGame,
			kMessage_PreLoadGame,
			kMessage_PostLoadGame,
		};

		std::uint32_t interfaceVersion;
		bool (*RegisterListener)(PluginHandle a_listener, const char* a_sender, EventCallback a_handler);
		bool (*Dispatch)(PluginHandle a_sender, std::uint32_t a_type, void* a_data, std::uint32_t a_dataLen, const char* a_receiver);
	};

	// SFSE runs AddTask's tasks once on the game's main thread (in its Command_Process hook), then calls Destroy.
	// (Run first, Destroy second, no virtual destructor: the order SFSE calls them through.)
	class ITaskDelegate
	{
	public:
		virtual void Run() = 0;
		virtual void Destroy() = 0;
	};

	struct SFSETaskInterface
	{
		std::uint32_t interfaceVersion;
		void (*AddTask)(ITaskDelegate* a_task);
		void (*AddTaskPermanent)(ITaskDelegate* a_task);
	};

	// SFSE reads this export before loading the plugin
	struct SFSEPluginVersionData
	{
		enum
		{
			kVersion = 1,
		};
		enum
		{
			kAddressIndependence_Signatures = 1 << 0,
			kAddressIndependence_AddressLibrary = 1 << 1,
			kAddressIndependence_AddressLibraryV2 = 1 << 2,  // Address Library for 1.15.216+ (the one SFSE 0.2.17+ accepts)
		};
		enum
		{
			kStructureIndependence_NoStructs = 1 << 0,
			kStructureIndependence_InitialLayout = 1 << 1,
			kStructureIndependence_1_8_86_Layout = 1 << 2,
			kStructureIndependence_1_14_70_Layout = 1 << 3,
		};

		std::uint32_t dataVersion;
		std::uint32_t pluginVersion;
		char          name[256];
		char          author[256];
		std::uint32_t addressIndependence;
		std::uint32_t structureIndependence;
		std::uint32_t compatibleVersions[16];
		std::uint32_t seVersionRequired;
		std::uint32_t reservedNonBreaking;
		std::uint32_t reservedBreaking;
	};
	static_assert(sizeof(SFSEPluginVersionData) == 4 * 2 + 256 * 2 + 4 * 2 + 4 * 16 + 4 * 3);
}
