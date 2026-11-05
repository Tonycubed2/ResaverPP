#pragma once
// The parts of F4SE's plugin API (f4se/PluginAPI.h, f4se/GameThreads.h) this helper uses, declared here so it
// builds without the F4SE sources. These are the same in F4SE 0.6.23 (Fallout 4 1.10.163) and 0.7.x (1.10.984,
// 1.11.x): the structures only ever gained fields at the end, and the helper reads only the old ones.

#include <cstdint>

namespace f4se
{
	using PluginHandle = std::uint32_t;
	inline constexpr PluginHandle kPluginHandle_Invalid = 0xFFFFFFFF;

	enum : std::uint32_t
	{
		kInterface_Invalid = 0,
		kInterface_Messaging,
		kInterface_Scaleform,
		kInterface_Papyrus,
		kInterface_Serialization,
		kInterface_Task,
	};

	// f4se_version.h: MAKE_EXE_VERSION
	constexpr std::uint32_t MakeExeVersion(std::uint32_t a_major, std::uint32_t a_minor, std::uint32_t a_build)
	{
		return ((a_major & 0xFF) << 24) | ((a_minor & 0xFF) << 16) | ((a_build & 0xFFF) << 4);
	}
	inline constexpr std::uint32_t kRuntime_1_10_163 = MakeExeVersion(1, 10, 163);
	inline constexpr std::uint32_t kRuntime_1_10_980 = MakeExeVersion(1, 10, 980);
	inline constexpr std::uint32_t kRuntime_1_10_984 = MakeExeVersion(1, 10, 984);
	inline constexpr std::uint32_t kRuntime_1_11_137 = MakeExeVersion(1, 11, 137);

	struct F4SEInterface
	{
		std::uint32_t f4seVersion;
		std::uint32_t runtimeVersion;
		std::uint32_t editorVersion;
		std::uint32_t isEditor;
		void* (*QueryInterface)(std::uint32_t a_id);
		PluginHandle (*GetPluginHandle)();
		std::uint32_t (*GetReleaseIndex)();
		// (0.6.22+: GetPluginInfo; 0.7.1+: GetSaveFolderName - not used)
	};

	struct PluginInfo
	{
		enum
		{
			kInfoVersion = 1
		};
		std::uint32_t infoVersion;
		const char*   name;
		std::uint32_t version;
	};

	struct F4SEMessagingInterface
	{
		struct Message
		{
			const char*   sender;
			std::uint32_t type;
			std::uint32_t dataLen;
			void*         data;
		};
		using EventCallback = void (*)(Message* a_msg);

		enum : std::uint32_t
		{
			kMessage_PostLoad,
			kMessage_PostPostLoad,
			kMessage_PreLoadGame,
			kMessage_PostLoadGame,  // data: bool, true when the load succeeded
			kMessage_PreSaveGame,
			kMessage_PostSaveGame,
			kMessage_DeleteGame,
			kMessage_InputLoaded,
			kMessage_NewGame,
			kMessage_GameLoaded,     // the game has finished loading (once, at the main menu)
			kMessage_GameDataReady,  // data: bool, true when the data handler finished loading
		};

		std::uint32_t interfaceVersion;
		bool (*RegisterListener)(PluginHandle a_listener, const char* a_sender, EventCallback a_handler);
		bool (*Dispatch)(PluginHandle a_sender, std::uint32_t a_type, void* a_data, std::uint32_t a_dataLen, const char* a_receiver);
	};

	// F4SE runs these on the game's main thread, then deletes them (delete through the virtual destructor)
	class ITaskDelegate
	{
	public:
		virtual ~ITaskDelegate() {}
		virtual void Run() = 0;
	};

	struct F4SETaskInterface
	{
		std::uint32_t interfaceVersion;
		void (*AddTask)(ITaskDelegate* a_task);
		// (0.6.23: AddUITask; 0.7.x: AddUITask, AddTaskPermanent - not used)
	};

	// F4SE 0.7.x: plugins declare what they are compatible with instead of answering a query
	struct F4SEPluginVersionData
	{
		enum
		{
			kVersion = 1,
		};
		enum
		{
			kAddressIndependence_Signatures = 1 << 0,
			kAddressIndependence_AddressLibrary_1_10_980 = 1 << 1,
			kAddressIndependence_AddressLibrary_1_11_137 = 1 << 2,
		};
		enum
		{
			kStructureIndependence_NoStructs = 1 << 0,
			kStructureIndependence_1_10_980Layout = 1 << 1,
			kStructureIndependence_1_11_137Layout = 1 << 2,
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
		std::uint8_t  reserved[512];
	};
	static_assert(sizeof(F4SEPluginVersionData) == 4 * 2 + 256 * 2 + 4 * 2 + 4 * 16 + 4 * 3 + 512);
}
