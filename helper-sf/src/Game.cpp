#include "PCH.h"
#include "Game.h"
#include "Settings.h"

namespace Game
{
	namespace
	{
		std::uint32_t             g_version = 0;
		sfse::SFSETaskInterface*  g_tasks = nullptr;
		std::atomic<bool>         g_inGame = false;
		std::uintptr_t            g_base = 0;
		std::uintptr_t            g_executeCommand = 0;  // the console's "run this command" (id 113576)
		std::uintptr_t            g_consolePtr = 0;      // where the console object pointer is kept (id 938528)
		std::uintptr_t            g_lookupByID = 0;      // TESForm::LookupByID (id 47401)

		constexpr std::uint64_t kId_ExecuteCommand = 113576;
		constexpr std::uint64_t kId_ConsoleObject = 938528;
		constexpr std::uint64_t kId_LookupByID = 47401;


		std::filesystem::path PluginDir()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&PluginDir), &self);
			wchar_t buf[MAX_PATH * 2]{};
			GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
			return std::filesystem::path(buf).parent_path();
		}

		// The Address Library for SFSE Plugins, format 5 (the one for 1.15.216 and later; versionlib-1-16-244-0.bin is
		// format 5): i32 format (5), u32 game version[4], char name[64], i32 pointer size, i32 data format, i32 count,
		// then count u32 offsets: the offset of id N is entry N (0 = no such id). (Read the same way by CommonLibSF's
		// REL::IDDB::load_v5, github.com/libxse/commonlib-shared.)
		bool LoadOffsets()
		{
			const auto name = std::format("versionlib-{}-{}-{}-0.bin", (g_version >> 24) & 0xFF, (g_version >> 16) & 0xFF, (g_version >> 4) & 0xFFF);
			const auto path = PluginDir() / name;
			std::ifstream f(path, std::ios::binary);
			if (!f) {
				spdlog::error("Address Library file {} not found: install Address Library for SFSE Plugins for this game version", path.string());
				return false;
			}
			std::int32_t  format = 0;
			std::uint32_t game[4]{};
			char          libName[64]{};
			std::int32_t  pointerSize = 0, dataFormat = 0, count = 0;
			f.read(reinterpret_cast<char*>(&format), 4);
			f.read(reinterpret_cast<char*>(game), 16);
			f.read(libName, 64);
			f.read(reinterpret_cast<char*>(&pointerSize), 4);
			f.read(reinterpret_cast<char*>(&dataFormat), 4);
			f.read(reinterpret_cast<char*>(&count), 4);
			if (!f || format != 5 || count <= 0 || count > 50'000'000) {
				spdlog::error("{} is not an Address Library file this helper can read (format {}; the helper reads format 5)", path.string(), format);
				return false;
			}
			const auto expect = std::format("{}.{}.{}", (g_version >> 24) & 0xFF, (g_version >> 16) & 0xFF, (g_version >> 4) & 0xFFF);
			if (std::format("{}.{}.{}", game[0], game[1], game[2]) != expect) {
				spdlog::error("{} is for Starfield {}.{}.{}, the game is {}", path.string(), game[0], game[1], game[2], expect);
				return false;
			}
			std::vector<std::uint32_t> offsets(static_cast<std::size_t>(count));
			f.read(reinterpret_cast<char*>(offsets.data()), static_cast<std::streamsize>(offsets.size() * 4));
			if (!f) {
				spdlog::error("{} is cut short", path.string());
				return false;
			}
			auto find = [&](std::uint64_t a_id) -> std::uint64_t { return a_id < offsets.size() ? offsets[static_cast<std::size_t>(a_id)] : 0; };
			const auto exe = find(kId_ExecuteCommand);
			const auto con = find(kId_ConsoleObject);
			const auto look = find(kId_LookupByID);
			spdlog::info("Address Library {}: {} ids; console command id {} -> +{:X}, console id {} -> +{:X}, LookupByID id {} -> +{:X}", name, count,
				kId_ExecuteCommand, exe, kId_ConsoleObject, con, kId_LookupByID, look);
			if (!exe || !con || !look) {
				spdlog::error("an address the helper needs is not in {}", name);
				return false;
			}
			g_executeCommand = g_base + exe;
			g_consolePtr = g_base + con;
			g_lookupByID = g_base + look;
			return true;
		}

		class Task final : public sfse::ITaskDelegate
		{
		public:
			explicit Task(std::function<void()> a_fn) :
				_fn(std::move(a_fn)) {}
			void Run() override
			{
				try {
					_fn();
				} catch (...) {
					spdlog::error("a main thread task failed");
				}
			}
			void Destroy() override { delete this; }

		private:
			std::function<void()> _fn;
		};

		std::string Clean(std::string a_text)
		{
			// a console string cannot hold quotes or new lines
			for (auto& c : a_text) {
				if (c == '"') {
					c = '\'';
				} else if (c == '\n' || c == '\r' || c == '\t') {
					c = ' ';
				}
			}
			return a_text;
		}

		template <class T>
		T Read(const void* a_form, std::size_t a_offset)
		{
			T v{};
			std::memcpy(&v, static_cast<const std::uint8_t*>(a_form) + a_offset, sizeof(T));
			return v;
		}
	}

	bool Init(std::uint32_t a_runtimeVersion, sfse::SFSETaskInterface* a_tasks)
	{
		g_version = a_runtimeVersion;
		g_tasks = a_tasks;
		g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		if (!g_tasks || !g_tasks->AddTask) {
			spdlog::error("SFSE's task interface is not available");
			return false;
		}
		return LoadOffsets();
	}

	std::string GameVersion()
	{
		return std::format("{}.{}.{}.{}", (g_version >> 24) & 0xFF, (g_version >> 16) & 0xFF, (g_version >> 4) & 0xFFF, g_version & 0xF);
	}

	void SetInGame(bool a_inGame) { g_inGame = a_inGame; }
	bool InGame() { return g_inGame; }

	bool AddTask(std::function<void()> a_fn)
	{
		if (!g_tasks || !g_tasks->AddTask) {
			return false;
		}
		g_tasks->AddTask(new Task(std::move(a_fn)));
		return true;
	}

	bool RunOnMain(std::function<void()> a_fn, std::chrono::milliseconds a_timeout)
	{
		auto done = std::make_shared<std::promise<void>>();
		auto fut = done->get_future();
		if (!AddTask([fn = std::move(a_fn), done]() {
				try {
					fn();
				} catch (...) {
					spdlog::error("main thread task failed");
				}
				try {
					done->set_value();
				} catch (...) {
				}
			})) {
			return false;
		}
		return fut.wait_for(a_timeout) == std::future_status::ready;
	}

	bool Console(const std::string& a_command)
	{
		if (!g_executeCommand || !g_consolePtr) {
			spdlog::error("console: the helper did not start (see above); not run: {}", a_command);
			return false;
		}
		try {
			// (the game calls it as (console, format, ...) with the pointer kept at id 938528; the function itself never
			// reads that first argument (0x141ea1e10 overwrites rcx before any use), so it is passed as the game passes it,
			// null or not. "%s" so a '%' in the command is not read as a format.)
			void* console = *reinterpret_cast<void**>(g_consolePtr);
			using ExecuteCommand_t = void (*)(void*, const char*, ...);
			reinterpret_cast<ExecuteCommand_t>(g_executeCommand)(console, "%s", a_command.c_str());
			spdlog::info("console: {}", a_command);
			return true;
		} catch (...) {
			spdlog::error("console: '{}' failed", a_command);
			return false;
		}
	}

	void Notify(const std::string& a_mode, const std::string& a_text)
	{
		const auto mode = (a_mode.empty() || a_mode == "ini") ? Settings::Get().notify : a_mode;
		if (mode == "none") {
			return;
		}
		const auto cmd = std::format("cgf \"Debug.{}\" \"{}\"", mode == "box" ? "MessageBox" : "Notification", Clean(a_text));
		AddTask([cmd] { Console(cmd); });
	}

	int FormType(std::uint32_t a_formID)
	{
		if (!g_lookupByID || !a_formID) {
			return -1;
		}
		try {
			using LookupByID_t = void* (*)(std::uint32_t);
			const void* form = reinterpret_cast<LookupByID_t>(g_lookupByID)(a_formID);
			return form ? Read<std::uint8_t>(form, 0x2E) : -1;
		} catch (...) {
			spdlog::error("form {:08X}: reading it failed", a_formID);
			return -1;
		}
	}

	QuestState Quest(std::uint32_t a_formID)
	{
		QuestState q;
		if (!g_lookupByID) {
			return q;
		}
		try {
			using LookupByID_t = void* (*)(std::uint32_t);
			const void* form = reinterpret_cast<LookupByID_t>(g_lookupByID)(a_formID);
			if (!form || Read<std::uint8_t>(form, 0x2E) != kFormType_QUST) {
				return q;
			}
			q.found = true;
			q.flags = Read<std::uint32_t>(form, 0x114);
			q.currentStage = Read<std::uint16_t>(form, 0x328);
			q.completed = (q.flags & 0x2) != 0;
			q.running = (q.flags & 0x1) && !(q.flags & 0x80) && Read<std::uint64_t>(form, 0x338) == 0 && Read<std::uint8_t>(form, 0x32C) == 0;
			const auto count = Read<std::uint32_t>(form, 0x138);
			const auto list = Read<const std::uint8_t* const*>(form, 0x140);
			for (std::uint32_t i = 0; list && i < count && i < 4096; ++i) {
				if (const auto* st = list[i]) {
					q.stages.emplace_back(Read<std::uint16_t>(st, 0x10), (Read<std::uint8_t>(st, 0x12) & 1) != 0);
				}
			}
		} catch (...) {
			spdlog::error("quest {:08X}: reading it failed", a_formID);
			q = QuestState{};
		}
		return q;
	}
}
