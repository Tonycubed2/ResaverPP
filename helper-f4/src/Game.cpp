#include "PCH.h"
#include "Game.h"
#include "Settings.h"

namespace Game
{
	namespace
	{
		Runtime                   g_runtime = Runtime::kUnknown;
		std::uint32_t             g_version = 0;
		f4se::F4SETaskInterface*  g_tasks = nullptr;
		std::atomic<bool>         g_inGame = false;
		std::uintptr_t            g_base = 0;
		std::uintptr_t            g_compileAndRun = 0;  // Script::CompileAndRun
		std::uintptr_t            g_factories = 0;      // IFormFactory* [ENUM_FORM_ID::kTotal]

		// Address Library ids (CommonLibF4: alandtse's fork for 1.10.163 / 1.10.984, libxse's for 1.11.x)
		struct Ids
		{
			std::uint64_t compileAndRun, factories;
		};
		constexpr Ids kOG{ 526625, 228366 };
		constexpr Ids kNG{ 2204287, 2689177 };
		constexpr Ids kAE{ 2204287, 4796464 };

		constexpr std::size_t kFormType_Script = 0x16;  // ENUM_FORM_ID::kSCPT (the same in every version)
		constexpr std::size_t kScriptText = 0x38;       // Script::text (Script: TESForm 0x20 + SCRIPT_HEADER 0x18)
		constexpr int         kCompiler_SystemWindow = 1;  // COMPILER_NAME::kSystemWindow: as the console does

		std::filesystem::path PluginDir()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&PluginDir), &self);
			wchar_t buf[MAX_PATH * 2]{};
			GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
			return std::filesystem::path(buf).parent_path();
		}

		// The Address Library for Fallout 4 (every version: "version-<a>-<b>-<c>-<d>.bin" next to the plugins):
		// a u64 count, then that many { u64 id, u64 offset } pairs sorted by id.
		bool LoadOffsets(const Ids& a_ids)
		{
			const auto name = std::format("version-{}-{}-{}-0.bin", (g_version >> 24) & 0xFF, (g_version >> 16) & 0xFF, (g_version >> 4) & 0xFFF);
			const auto path = PluginDir() / name;
			std::ifstream f(path, std::ios::binary);
			if (!f) {
				spdlog::error("Address Library file {} not found: install Address Library for F4SE Plugins for this game version", path.string());
				return false;
			}
			std::uint64_t count = 0;
			f.read(reinterpret_cast<char*>(&count), 8);
			if (!f || count == 0 || count > 10'000'000) {
				spdlog::error("{} is not an Address Library file this helper can read", path.string());
				return false;
			}
			std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs(static_cast<std::size_t>(count));
			f.read(reinterpret_cast<char*>(pairs.data()), static_cast<std::streamsize>(count * 16));
			if (!f) {
				spdlog::error("{} is cut short", path.string());
				return false;
			}
			auto find = [&](std::uint64_t a_id) -> std::uint64_t {
				const auto it = std::lower_bound(pairs.begin(), pairs.end(), a_id, [](const auto& p, std::uint64_t id) { return p.first < id; });
				return it != pairs.end() && it->first == a_id ? it->second : 0;
			};
			const auto car = find(a_ids.compileAndRun);
			const auto fac = find(a_ids.factories);
			spdlog::info("Address Library {}: {} entries; CompileAndRun id {} -> +{:X}, form factories id {} -> +{:X}", name, count,
				a_ids.compileAndRun, car, a_ids.factories, fac);
			if (!car || !fac) {
				spdlog::error("an address the helper needs is not in {}", name);
				return false;
			}
			g_compileAndRun = g_base + car;
			g_factories = g_base + fac;
			return true;
		}

		class Task : public f4se::ITaskDelegate
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

		private:
			std::function<void()> _fn;
		};

		// one script form, kept for the whole session (as the console keeps its own)
		void* g_script = nullptr;
		std::string g_text;  // the script's text (the form points at it)

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
	}

	bool Init(std::uint32_t a_runtimeVersion, f4se::F4SETaskInterface* a_tasks)
	{
		g_version = a_runtimeVersion;
		g_tasks = a_tasks;
		g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		const Ids* ids = nullptr;
		if (a_runtimeVersion == f4se::kRuntime_1_10_163) {
			g_runtime = Runtime::kOG;
			ids = &kOG;
		} else if (a_runtimeVersion >= f4se::kRuntime_1_10_980 && a_runtimeVersion < f4se::kRuntime_1_11_137) {
			g_runtime = Runtime::kNG;
			ids = &kNG;
		} else if (a_runtimeVersion >= f4se::kRuntime_1_11_137) {
			g_runtime = Runtime::kAE;
			ids = &kAE;
		} else {
			spdlog::error("Fallout 4 {} is not supported: the helper works with 1.10.163, 1.10.984 and 1.11.x", GameVersion());
			return false;
		}
		if (!g_tasks || !g_tasks->AddTask) {
			spdlog::error("F4SE's task interface is not available");
			return false;
		}
		return LoadOffsets(*ids);
	}

	Runtime GetRuntime() { return g_runtime; }

	std::string RuntimeName()
	{
		switch (g_runtime) {
		case Runtime::kOG: return "F4";
		case Runtime::kNG: return "F4NG";
		case Runtime::kAE: return "F4AE";
		default: return "F4?";
		}
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
		if (!g_compileAndRun || !g_factories) {
			spdlog::error("console: the helper did not start (see above); not run: {}", a_command);
			return false;
		}
		try {
			if (!g_script) {
				// IFormFactory for Script forms: its DoCreate (virtual function 1) makes a new form
				auto* factory = reinterpret_cast<void**>(g_factories)[kFormType_Script];
				if (!factory) {
					spdlog::error("console: no script form factory");
					return false;
				}
				using DoCreate_t = void* (*)(void*);
				const auto doCreate = reinterpret_cast<DoCreate_t*>(*reinterpret_cast<void***>(factory))[1];
				g_script = doCreate(factory);
				if (!g_script) {
					spdlog::error("console: the script form could not be made");
					return false;
				}
			}
			g_text = a_command;
			*reinterpret_cast<char**>(reinterpret_cast<std::uint8_t*>(g_script) + kScriptText) = g_text.data();
			using CompileAndRun_t = void (*)(void* a_script, void* a_compiler, int a_compilerName, void* a_owner);
			std::uint8_t compiler = 0;  // ScriptCompiler: an empty object
			reinterpret_cast<CompileAndRun_t>(g_compileAndRun)(g_script, &compiler, kCompiler_SystemWindow, nullptr);
			*reinterpret_cast<char**>(reinterpret_cast<std::uint8_t*>(g_script) + kScriptText) = nullptr;
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
}
