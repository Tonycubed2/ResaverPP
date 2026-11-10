#include "PCH.h"
#include "MenuGuard.h"

#include "Game.h"
#include "Settings.h"

#include <psapi.h>
#include <intrin.h>

#include <REX/W32/DXGI.h>
#include <REX/W32/DXGI_4.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <deque>
#include <map>

// Addresses (Address Library ids; SE id from CommonLibSSE-NG 6.7.0, AE id checked in the 1.6.1170 and 1.6.640 libraries):
//   BSScaleformManager::LoadMovie       RELOCATION_ID(80302, 82325)  1.6.1170 0x140fb0110 (CommonLib BSScaleformManager.cpp)
//   BSScaleformManager::FileExists      RELOCATION_ID(80087, 82411)  1.6.1170 0x140fb49a0, called at LoadMovie+0xaf / +0xdc
//   GFxLoader::CreateMovie              RELOCATION_ID(80620, 84640)  1.6.1170 0x141034e30, called at LoadMovie+0x124
//   MapMenu constructor (AE only)       REL::ID(53093)               1.6.1170 0x1409836c0, LoadMovie call at +0x1d1
//   JournalMenu creator (AE only)       REL::ID(53342)               1.6.1170 0x140995cd0 (constructor inlined), call at +0x127
// LoadMovie on 1.6.1170 fails in exactly three places: both FileExists calls false ("Interface/%s.swf",
// "Interface/Exported/%s.gfx"), CreateMovie null, or GFxMovieDef vtable +0xC0 (CreateInstance) null; each ends by
// writing null to the movie pointer it was given and returning false.
//
// Version 17 (verified on the 1.6.1170 executable; 1.6.640 only through the ids and the crash offsets, see the handoff):
//   StatsMenu (the Skills menu) does not load its movie in its constructor. Its constructor (52508) calls 82326
//   (0x140fb0590), which queues a BSScaleformMovieLoadTask (vtable RELOCATION_ID(292005, 242930), 1.6.1170
//   0x141a0a3b8, 0x38 bytes: +0x18 IMenu*, +0x20 GPtr<GFxMovieView>* (the menu's uiMovie), +0x28 movie name,
//   +0x30 scale mode, +0x34 alpha). The task's slot 1 (84665) calls LoadMovie on a loader thread; its slot 2 (84666:
//   mov rcx,[rcx+0x18]; mov rax,[rcx]; jmp [rax+0x18]) then calls the menu's IMenu slot 3, which for StatsMenu
//   (52529) only sets +0x321 = 1, whether the load worked or not. StatsMenu::ProcessMessage (52510) on kUpdate, once
//   its 3D is ready (+0x323) and +0x321 is set, calls uiMovie's vtable +0x88 WITHOUT a null check (+0x939, the
//   crash). Every other use of uiMovie in StatsMenu checks for null, because the menu is open while its movie loads.
//   The guard therefore skips the slot-3 call when the load failed (the menu stays in its "still loading" state) and
//   closes the menu with kHide, as when the player closes it before the movie has loaded.
namespace MenuGuard
{
	namespace
	{
		using ScaleMode = RE::GFxMovieView::ScaleModeType;
		using LoadMovie_t = bool(RE::BSScaleformManager*, RE::IMenu*, RE::GPtr<RE::GFxMovieView>&, const char*, ScaleMode, float);
		using FileExists_t = bool(const char*);
		using CreateMovie_t = RE::GFxMovieDef*(RE::GFxLoader*, const char*, RE::GFxLoader::LoadConstants, RE::UPInt);
		using Create_t = RE::UI::Create_t;
		using TaskFn_t = std::uintptr_t(void*);  // BSScaleformMovieLoadTask slots 1 and 2 (rax passed through)
		using ProcessMessage_t = RE::UI_MESSAGE_RESULTS(RE::IMenu*, RE::UIMessage&);

		// thrown by the LoadMovie hook, caught by the creator wrapper (only when the wrapper is on the stack)
		struct MovieLoadAbort
		{
			std::string movie;
			std::string step;
		};

		LoadMovie_t*   g_origLoadMovie = nullptr;
		FileExists_t*  g_origFileExists = nullptr;
		CreateMovie_t* g_origCreateMovie = nullptr;
		TaskFn_t*      g_origTaskRun = nullptr;
		TaskFn_t*      g_origTaskFinish = nullptr;
		bool           g_installed = false;
		bool           g_stepHooks = false;
		bool           g_taskHooks = false;
		std::string    g_status = "not installed";
		std::uintptr_t g_loadMovie = 0, g_fileExists = 0, g_createMovie = 0;
		DWORD          g_mainThread = 0;

		// ------------------------------------------------------------------ what one LoadMovie try did
		struct Steps
		{
			int                      fileChecks = 0;
			std::string              checkPath[2];
			bool                     checkResult[2]{};
			bool                     createCalled = false;
			bool                     createOk = false;
			std::string              createPath;
			DWORD                    createError = 0;
			double                   createMs = 0;
			std::vector<std::string> gfx;    // what Scaleform logged on this thread during the try
			std::vector<std::string> opens;  // the file opener calls on this thread during the try
		};
		thread_local Steps* t_steps = nullptr;

		// the creator wrapper on this thread: how deep, which menu, and the address of a local in its frame
		thread_local int         t_depth = 0;
		thread_local const char* t_menu = nullptr;
		thread_local const void* t_wrapperFrame = nullptr;
		// the BSScaleformMovieLoadTask whose Run is on this thread's stack
		thread_local void* t_task = nullptr;

		// ------------------------------------------------------------------ ring of the last 50 calls, failure counts
		struct Call
		{
			std::string   when, movie, menu, step;
			bool          ok = false, forced = false;
			double        ms = 0;
			DWORD         tid = 0;
			int           attempt = 0;
			std::uint64_t seq = 0;
		};
		std::mutex                     g_lock;
		std::deque<Call>               g_ring;
		std::uint64_t                  g_seq = 0;         // calls remembered so far
		std::uint64_t                  g_listedSeq = 0;   // the last call already printed in a "last loads" list
		std::string                    g_listedWhen;      // when that list was printed
		std::map<std::string, Problem> g_problems;

		std::string Now()
		{
			SYSTEMTIME t{};
			GetLocalTime(&t);
			return std::format("{:02}:{:02}:{:02}.{:03}", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		}

		std::string Lower(std::string s)
		{
			std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		std::string ThreadText(DWORD a_tid) { return std::format("{}{}", a_tid, a_tid == g_mainThread ? " (main)" : ""); }

		std::string CallText(const Call& c)
		{
			return std::format("{} {} {}{} in {:.1f} ms, try {}, thread {}, menu {}{}", c.when, c.movie, c.ok ? "ok" : "FAILED",
				c.forced ? " (TEST: forced failure)" : "", c.ms, c.attempt, ThreadText(c.tid), c.menu, c.step.empty() ? "" : " - " + c.step);
		}

		void Remember(Call& a_call)
		{
			std::scoped_lock l(g_lock);
			a_call.seq = ++g_seq;
			g_ring.push_back(a_call);
			while (g_ring.size() > 50) {
				g_ring.pop_front();
			}
		}

		// ------------------------------------------------------------------ memory reads that never fault
		bool Readable(std::uintptr_t a_addr, std::size_t a_len)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!a_addr || !VirtualQuery(reinterpret_cast<LPCVOID>(a_addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
				return false;
			}
			const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			if (!(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD)) {
				return false;
			}
			return a_addr + a_len <= reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		}

		std::string Bytes(std::uintptr_t a_addr, std::size_t a_len)
		{
			if (!Readable(a_addr, a_len)) {
				return "(not readable)";
			}
			std::string s;
			for (std::size_t i = 0; i < a_len; ++i) {
				s += std::format("{}{:02X}", i ? " " : "", reinterpret_cast<const std::uint8_t*>(a_addr)[i]);
			}
			return s;
		}

		HMODULE ModuleOf(std::uintptr_t a_addr)
		{
			HMODULE m = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_addr), &m);
			return m;
		}

		std::string ModuleName(HMODULE a_module)
		{
			wchar_t buf[MAX_PATH]{};
			if (!a_module || !GetModuleFileNameW(a_module, buf, MAX_PATH)) {
				return {};
			}
			return std::filesystem::path(buf).filename().string();
		}

		std::string ModuleText(std::uintptr_t a_addr)
		{
			const auto m = ModuleName(ModuleOf(a_addr));
			return m.empty() ? "hook stub memory" : m;
		}

		// slot a_index of the vtable of the object at a_obj (the object's first 8 bytes point to its vtable)
		std::uintptr_t VtableSlot(const void* a_obj, std::size_t a_index)
		{
			const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(a_obj);
			return *reinterpret_cast<const std::uintptr_t*>(vtbl + a_index * sizeof(void*));
		}

		// where code at a_addr ends up: follows jmp rel32, jmp [rip+x] and mov rax,imm64 / jmp rax (the hook stubs)
		std::uintptr_t FollowJumps(std::uintptr_t a_addr, std::string* a_path)
		{
			for (int hop = 0; hop < 8 && Readable(a_addr, 14); ++hop) {
				const auto* p = reinterpret_cast<const std::uint8_t*>(a_addr);
				std::uintptr_t next = 0;
				if (p[0] == 0xE9) {
					next = a_addr + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
				} else if (p[0] == 0xFF && p[1] == 0x25) {
					const auto slot = a_addr + 6 + *reinterpret_cast<const std::int32_t*>(p + 2);
					if (!Readable(slot, 8)) {
						break;
					}
					next = *reinterpret_cast<const std::uintptr_t*>(slot);
				} else if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) {
					next = *reinterpret_cast<const std::uintptr_t*>(p + 2);
				} else {
					break;
				}
				if (a_path) {
					const auto mod = ModuleName(ModuleOf(next));
					*a_path += std::format(" -> {:X}{}", next, mod.empty() ? " (hook stub memory)" : " (" + mod + ")");
				}
				a_addr = next;
			}
			return a_addr;
		}

		// "who else is in the chain" for code at a_addr: its first bytes and where its jumps go
		void LogChain(const char* a_what, std::uintptr_t a_addr, std::size_t a_len)
		{
			std::string path;
			FollowJumps(a_addr, &path);
			spdlog::info("Menu guard: {} at {:X}: {}{}", a_what, a_addr, Bytes(a_addr, a_len), path.empty() ? " (no jump: the game's own code)" : path);
		}

		// a call at a_site: E8 rel32; logs where it goes
		void LogCallSite(const char* a_what, std::uintptr_t a_site)
		{
			if (!Readable(a_site, 5)) {
				spdlog::info("Menu guard: {} at {:X}: not readable", a_what, a_site);
				return;
			}
			const auto* p = reinterpret_cast<const std::uint8_t*>(a_site);
			if (p[0] != 0xE8) {
				spdlog::info("Menu guard: {} at {:X}: {} (no call here in this game version)", a_what, a_site, Bytes(a_site, 8));
				return;
			}
			const auto  target = a_site + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
			std::string path;
			const auto  end = FollowJumps(target, &path);
			const auto  mod = ModuleName(ModuleOf(target));
			spdlog::info("Menu guard: {} at {:X}: call {:X}{}{}{}", a_what, a_site, target, mod.empty() ? " (hook stub memory)" : " (" + mod + ")", path,
				end == g_loadMovie || target == g_loadMovie ? " = LoadMovie" : "");
		}

		void LogHookChain(const char* a_when)
		{
			spdlog::info("Menu guard: hook check ({})", a_when);
			LogChain("LoadMovie entry", g_loadMovie, 16);
			if (REL::Module::IsAE()) {
				// verified on 1.6.1170 (the 1.6.640 crash offset +0x200 in MapMenu's constructor matches the same layout)
				LogCallSite("MapMenu constructor + 0x1D1 (LoadMovie \"Map\")", REL::ID(53093).address() + 0x1D1);
				LogCallSite("JournalMenu creator + 0x127 (LoadMovie \"Quest_Journal\")", REL::ID(53342).address() + 0x127);
				// version 17: StatsMenu's constructor queues its movie load (1.6.1170: call 82326 = 0x140fb0590)
				LogCallSite("StatsMenu constructor + 0x46B (background load of \"StatsMenu\")", REL::ID(52508).address() + 0x46B);
				spdlog::info("Menu guard: (82326, the background load function, is at {:X})", REL::ID(82326).address());
			}
		}

		// ------------------------------------------------------------------ the notice (version 17)
		// Shown on the main thread, through the game's own functions, not through Papyrus (a Papyrus Debug.Notification
		// waits until the game runs again, so it was not seen while a menu had the game paused):
		//   sNotify=box  -> a message box (MessageBoxMenu::Create, RELOCATION_ID(51420, 52269), the function Papyrus'
		//                   Debug.MessageBox ends in; 1.6.1170 0x14094b280 reads the buttons as a null-ended list)
		//   sNotify=hud  -> the HUD notification (SendHUDMessage::ShowHUDMessage, RELOCATION_ID(52050, 52933)); while a
		//                   menu has the game paused the HUD is not shown, so a message box is used instead
		//   sNotify=none -> nothing (the log and the F1 page still have it)
		void Notice(const std::string& a_text)
		{
			const auto mode = Settings::Get().notify;
			if (mode == "none") {
				return;
			}
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}
			task->AddTask([mode, a_text]() {
				auto*      ui = RE::UI::GetSingleton();
				const bool paused = ui && ui->GameIsPaused();
				if (mode == "box" || paused) {
					const char* ok = "OK";
					if (auto* gs = RE::GameSettingCollection::GetSingleton()) {
						if (auto* s = gs->GetSetting("sOk"); s && s->GetString() && *s->GetString()) {
							ok = s->GetString();
						}
					}
					const bool shown = RE::MessageBoxMenu::Create(a_text.c_str(), nullptr, 0, 4, 10, ok);
					spdlog::info("Menu guard: notice as a message box{}: {}", shown ? "" : " (the game refused it)", a_text);
				} else {
					RE::SendHUDMessage::ShowHUDMessage(a_text.c_str(), nullptr, false);
					spdlog::info("Menu guard: notice on the HUD: {}", a_text);
				}
			});
		}

		std::string NoticeText(const std::string& a_menu, const std::string& a_movie, bool a_forced)
		{
			return std::format("ReSaverHelper: {} could not load its interface file ({}.swf) and did not open{}. See ReSaverHelper.log.", a_menu, a_movie,
				a_forced ? " - test" : "");
		}

		// ------------------------------------------------------------------ process state at a failure
		std::string Mb(std::uint64_t a_bytes) { return std::format("{:.0f} MB", static_cast<double>(a_bytes) / (1024.0 * 1024.0)); }
		std::string Kb(std::uint64_t a_bytes) { return std::format("{:.0f} KB", static_cast<double>(a_bytes) / 1024.0); }

		std::string VideoMemory()
		{
			auto* device = RE::BSGraphics::Renderer::GetDevice();
			if (!device) {
				return "video memory: no renderer device yet";
			}
			REX::W32::IDXGIDevice* dxgi = nullptr;
			if (device->QueryInterface(REX::W32::IID_IDXGIDevice, reinterpret_cast<void**>(&dxgi)) < 0 || !dxgi) {
				return "video memory: the device has no IDXGIDevice";
			}
			REX::W32::IDXGIAdapter* adapter = nullptr;
			const auto              hr = dxgi->GetAdapter(&adapter);
			dxgi->Release();
			if (hr < 0 || !adapter) {
				return "video memory: no adapter";
			}
			REX::W32::IDXGIAdapter3* adapter3 = nullptr;
			const auto               hr3 = adapter->QueryInterface(REX::W32::IID_IDXGIAdapter3, reinterpret_cast<void**>(&adapter3));
			adapter->Release();
			if (hr3 < 0 || !adapter3) {
				return "video memory: IDXGIAdapter3 not available (needs DXGI 1.4)";
			}
			REX::W32::DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
			const auto h1 = adapter3->QueryVideoMemoryInfo(0, REX::W32::DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
			const auto h2 = adapter3->QueryVideoMemoryInfo(0, REX::W32::DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
			adapter3->Release();
			if (h1 < 0) {
				return "video memory: QueryVideoMemoryInfo failed";
			}
			return std::format("video memory: {} used of a {} budget{}", Mb(local.currentUsage), Mb(local.budget),
				h2 >= 0 ? std::format(" (shared system memory {} of {})", Mb(shared.currentUsage), Mb(shared.budget)) : std::string());
		}

		// Scaleform's global memory heap (RE::GMemory::GetGlobalHeap, RELOCATION_ID(525584, 412058); 1.6.1170 0x143292490,
		// the pointer the BSScaleformManager constructor allocates from). Checked on 1.6.1170: the heap's vtable
		// (0x141a09af8) slots 0x14-0x17 lock the heap (+0x98) and ask its engine (+0xC8) for footprint / used space, slot 6
		// (SetLimit) stores the limit at +0x60 = CommonLib's GetLimit(); slots 0x1C/0x1E/0x20 are the empty ones CommonLib
		// marks. The getters take the heap's lock, so they are safe from any thread.
		std::string ScaleformHeap()
		{
			auto* heap = RE::GMemory::GetGlobalHeap();
			if (!heap) {
				return "Scaleform memory: the global heap does not exist yet";
			}
			const auto limit = heap->GetLimit();
			return std::format("Scaleform memory (global heap): footprint {}, used {}; with its child heaps: footprint {}, used {}; limit {}",
				Mb(heap->GetFootprint()), Mb(heap->GetUsedSpace()), Mb(heap->GetTotalFootprint()), Mb(heap->GetTotalUsedSpace()),
				limit ? Mb(limit) : std::string("none"));
		}

		std::vector<std::string> ProcessState()
		{
			std::vector<std::string> out;
			MEMORYSTATUSEX ms{};
			ms.dwLength = sizeof(ms);
			if (GlobalMemoryStatusEx(&ms)) {
				out.push_back(std::format("system memory: {}% in use, {} free of {}; page file {} free of {}; address space {} free", ms.dwMemoryLoad,
					Mb(ms.ullAvailPhys), Mb(ms.ullTotalPhys), Mb(ms.ullAvailPageFile), Mb(ms.ullTotalPageFile), Mb(ms.ullAvailVirtual)));
			}
			PROCESS_MEMORY_COUNTERS_EX pmc{};
			pmc.cb = sizeof(pmc);
			if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
				out.push_back(std::format("game process: commit {}, private {}, working set {} (peak {})", Mb(pmc.PagefileUsage), Mb(pmc.PrivateUsage),
					Mb(pmc.WorkingSetSize), Mb(pmc.PeakWorkingSetSize)));
			}
			DWORD handles = 0;
			if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
				out.push_back(std::format("open handles: {}", handles));
			}
			out.push_back(VideoMemory());
			out.push_back(ScaleformHeap());
			return out;
		}

		// ------------------------------------------------------------------ Scaleform's log and file opener (version 17)
		// The game gives its GFxLoader (BSScaleformManager+0x08) a log state, DejaGFxLogger (vtable 0x1419ffbe0, set with
		// SetState(4 = kLog) in the BSScaleformManager setup 82323, 0x140faf9e0), whose LogMessageVarg (82391, 0x140fb2dc0)
		// is a bare `ret`: every Scaleform message ("GFxLoader failed to open", "Error loading SWF file", "file does not
		// start with a SWF header"... all strings in the exe) is thrown away. The guard puts its own log in front of it
		// (the old one keeps getting every message) and writes what Scaleform says during a load to ReSaverHelper.log.
		// The file opener (BSScaleformFileOpener, vtable 0x141a00478, state type 10 = kFileOpener, given to the loader
		// in its LoaderConfig): slot 1 OpenFile(url, flags, mode) calls slot 3 OpenFileEx(url, log, flags, mode); slot 2
		// GetFileModifyTime returns 0. The guard's opener calls the old one and notes, during a load, which file GFx asked
		// for and what came back. The file object is a Scaleform GFile (ScaleformFile::MemoryFile, vtable 0x141a004d8):
		// slot 2 IsValid, slot 6 GetLength, slot 8 GetErrorCode (the GFile order; checked on that class).
		std::atomic<std::uint64_t> g_gfxMessages = 0, g_gfxScriptMessages = 0, g_gfxWritten = 0, g_opens = 0;
		bool                       g_gfxInstalled = false;
		std::string                g_gfxStatus = "not installed";

		class GuardLog final : public RE::GFxLog
		{
		public:
			explicit GuardLog(RE::GFxState* a_previous) :
				previous(a_previous) {}

			void LogMessageVarg(LogMessageType a_type, const char* a_fmt, std::va_list a_args) override
			{
				++g_gfxMessages;
				const auto t = static_cast<std::uint32_t>(a_type);
				const auto channel = t & 0xF0;
				auto*      steps = t_steps;
				if (a_fmt && (steps || channel == 0x10 || channel == 0x30)) {  // during a load, or general / parse messages
					char         buf[1024];
					std::va_list copy;
					va_copy(copy, a_args);
					std::vsnprintf(buf, sizeof(buf), a_fmt, copy);
					va_end(copy);
					std::string text(buf);
					while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
						text.pop_back();
					}
					const char* kind = (t & 0x0F) == 0 ? "error" : (t & 0x0F) == 1 ? "warning" : "message";
					if (steps) {
						if (steps->gfx.size() < 30) {
							steps->gfx.push_back(std::format("{} {}: {}", Now(), kind, text));
						}
					} else if (++g_gfxWritten <= 200) {
						spdlog::info("Menu guard: Scaleform {} (thread {}): {}", kind, ThreadText(GetCurrentThreadId()), text);
					}
				} else {
					++g_gfxScriptMessages;  // ActionScript messages outside a load: counted, not written
				}
				if (previous) {
					// the previous log's LogMessageVarg, GFxLog vtable slot 1 (checked on the game's DejaGFxLogger)
					using fn_t = void(RE::GFxState*, LogMessageType, const char*, std::va_list);
					reinterpret_cast<fn_t*>(VtableSlot(previous, 1))(previous, a_type, a_fmt, a_args);
				}
			}

			RE::GFxState* previous;  // its reference is kept for the rest of the game
		};

		std::string FileText(const char* a_call, const char* a_url, void* a_file)
		{
			if (!a_file) {
				return std::format("{}(\"{}\") gave no file", a_call, a_url ? a_url : "");
			}
			using bool_t = bool(void*);
			using int_t = std::int32_t(void*);
			const bool valid = reinterpret_cast<bool_t*>(VtableSlot(a_file, 2))(a_file);
			const auto length = reinterpret_cast<int_t*>(VtableSlot(a_file, 6))(a_file);
			const auto error = reinterpret_cast<int_t*>(VtableSlot(a_file, 8))(a_file);
			return std::format("{}(\"{}\") gave a file: {}, {} bytes, error code {} (file class in {})", a_call, a_url ? a_url : "",
				valid ? "valid" : "NOT valid", length, error, ModuleText(*reinterpret_cast<const std::uintptr_t*>(a_file)));
		}

		class GuardFileOpener final : public RE::GFxState
		{
		public:
			explicit GuardFileOpener(RE::GFxState* a_previous) :
				RE::GFxState(StateType::kFileOpener),
				previous(a_previous) {}

			// the GFxFileOpenerBase slots 1-3 (see above)
			virtual void* OpenFile(const char* a_url, std::int32_t a_flags, std::int32_t a_mode)
			{
				using fn_t = void*(RE::GFxState*, const char*, std::int32_t, std::int32_t);
				auto* f = reinterpret_cast<fn_t*>(VtableSlot(previous, 1))(previous, a_url, a_flags, a_mode);
				Note("OpenFile", a_url, f);
				return f;
			}
			virtual std::int64_t GetFileModifyTime(const char* a_url)
			{
				using fn_t = std::int64_t(RE::GFxState*, const char*);
				return reinterpret_cast<fn_t*>(VtableSlot(previous, 2))(previous, a_url);
			}
			virtual void* OpenFileEx(const char* a_url, RE::GFxLog* a_log, std::int32_t a_flags, std::int32_t a_mode)
			{
				using fn_t = void*(RE::GFxState*, const char*, RE::GFxLog*, std::int32_t, std::int32_t);
				auto* f = reinterpret_cast<fn_t*>(VtableSlot(previous, 3))(previous, a_url, a_log, a_flags, a_mode);
				Note("OpenFileEx", a_url, f);
				return f;
			}

			RE::GFxState* previous;  // its reference is kept for the rest of the game

		private:
			static void Note(const char* a_call, const char* a_url, void* a_file)
			{
				++g_opens;
				if (auto* s = t_steps; s && s->opens.size() < 10) {
					s->opens.push_back(Now() + " " + FileText(a_call, a_url, a_file));
				}
			}
		};

		GuardLog*        g_guardLog = nullptr;
		GuardFileOpener* g_guardOpener = nullptr;

		// which object is the loader's state of this type now, and whose code it is
		std::string StateOwner(RE::GFxLoader* a_loader, RE::GFxState::StateType a_type, const RE::GFxState* a_ours)
		{
			auto* st = a_loader->GetStateAddRef(a_type);
			if (!st) {
				return "none";
			}
			const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(st);
			const auto text = st == a_ours ? std::string("the guard's") : std::format("{:X} (vtable in {})", reinterpret_cast<std::uintptr_t>(st), ModuleText(vtbl));
			st->Release();
			return text;
		}

		void InstallGfxDiagnostics(const char* a_when)
		{
			if (g_gfxInstalled || !Settings::Get().menuLoadDiagnostics) {
				if (!Settings::Get().menuLoadDiagnostics) {
					g_gfxStatus = "off (bMenuLoadDiagnostics=0)";
				}
				return;
			}
			auto* mgr = RE::BSScaleformManager::GetSingleton();
			if (!mgr || !mgr->loader || !RE::GMemory::GetGlobalHeap()) {
				spdlog::info("Menu guard: Scaleform is not set up yet ({}); its log and file opener are watched later", a_when);
				return;
			}
			using ST = RE::GFxState::StateType;
			auto* loader = mgr->loader;
			auto* oldLog = loader->GetStateAddRef(ST::kLog);  // a reference, kept
			auto* oldOpener = loader->GetStateAddRef(ST::kFileOpener);
			spdlog::info("Menu guard: Scaleform loader {:X}: log state {}, file opener {}", reinterpret_cast<std::uintptr_t>(loader),
				oldLog ? std::format("{:X} (vtable in {})", reinterpret_cast<std::uintptr_t>(oldLog), ModuleText(*reinterpret_cast<const std::uintptr_t*>(oldLog))) : "none",
				oldOpener ? std::format("{:X} (vtable in {})", reinterpret_cast<std::uintptr_t>(oldOpener), ModuleText(*reinterpret_cast<const std::uintptr_t*>(oldOpener))) :
							"none");
			g_guardLog = new GuardLog(oldLog);
			loader->SetState(ST::kLog, g_guardLog);
			if (oldOpener) {
				g_guardOpener = new GuardFileOpener(oldOpener);
				loader->SetState(ST::kFileOpener, g_guardOpener);
			}
			g_gfxInstalled = true;
			g_gfxStatus = std::format("Scaleform log watched{}", g_guardOpener ? ", file opener watched" : " (the loader has no file opener to watch)");
			spdlog::info("Menu guard: {} ({})", g_gfxStatus, a_when);
			spdlog::info("Menu guard: {}", ScaleformHeap());
		}

		// ------------------------------------------------------------------ the movie's files at a failure
		std::filesystem::path GameFolder()
		{
			wchar_t buf[MAX_PATH * 2]{};
			GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
			return std::filesystem::path(buf).parent_path();
		}

		std::string LooseFile(const std::filesystem::path& a_file)
		{
			WIN32_FILE_ATTRIBUTE_DATA fad{};
			if (!GetFileAttributesExW(a_file.c_str(), GetFileExInfoStandard, &fad)) {
				return std::format("{}: no loose file (error {})", a_file.string(), GetLastError());
			}
			const auto size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
			HANDLE     h = CreateFileW(a_file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
					FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) {
				return std::format("{}: loose file, {} bytes, CANNOT be opened for reading (error {})", a_file.string(), size, GetLastError());
			}
			CloseHandle(h);
			return std::format("{}: loose file, {} bytes, opens for reading", a_file.string(), size);
		}

		const char* ErrorName(RE::BSResource::ErrorCode a_e)
		{
			using E = RE::BSResource::ErrorCode;
			switch (a_e) {
			case E::kNone: return "none";
			case E::kNotExist: return "does not exist";
			case E::kInvalidPath: return "invalid path";
			case E::kFileError: return "file error";
			case E::kInvalidType: return "invalid type";
			case E::kMemoryError: return "memory error";
			case E::kBusy: return "busy";
			case E::kInvalidParam: return "invalid parameter";
			case E::kUnsupported: return "unsupported";
			default: return "?";
			}
		}

		// through the game: BSScaleformManager::FileExists (the check LoadMovie itself makes, loose files and archives) and
		// BSResourceNiBinaryStream (opens it as the game's resource system does)
		std::string GameFile(const std::string& a_rel)
		{
			const bool exists = RE::BSScaleformManager::FileExists(a_rel.c_str());
			RE::BSResourceNiBinaryStream stream(a_rel);
			const bool opens = stream.good();
			return std::format("{}: game file check {}, resource stream {}{}", a_rel, exists ? "finds it" : "does NOT find it",
				opens ? "opens" : "does NOT open", opens ? std::string() : std::format(" ({})", ErrorName(stream.lastError)));
		}

		// once per failed open (after all tries): what Scaleform said, the files, the process, the loads since the last list
		void LogFailureDetails(const std::string& a_movie, const std::vector<Steps>& a_tries)
		{
			if (a_tries.empty()) {
				spdlog::warn("Menu guard:   (TEST: every try was a forced failure; the game's loader was not called)");
			}
			for (std::size_t i = 0; i < a_tries.size(); ++i) {
				const auto& s = a_tries[i];
				if (s.gfx.empty() && s.opens.empty()) {
					continue;
				}
				for (const auto& o : s.opens) {
					spdlog::warn("Menu guard:   try {}: file opener: {}", i + 1, o);
				}
				for (const auto& g : s.gfx) {
					spdlog::warn("Menu guard:   try {}: Scaleform {}", i + 1, g);
				}
			}
			if (g_gfxInstalled && !a_tries.empty()) {
				bool anyOpen = false, anyGfx = false;
				for (const auto& s : a_tries) {
					anyOpen = anyOpen || !s.opens.empty();
					anyGfx = anyGfx || !s.gfx.empty();
				}
				if (!anyOpen && g_guardOpener) {
					spdlog::warn("Menu guard:   GFx did not ask the file opener for any file during these tries");
				}
				if (!anyGfx) {
					spdlog::warn("Menu guard:   Scaleform logged nothing during these tries");
				}
				if (auto* mgr = RE::BSScaleformManager::GetSingleton(); mgr && mgr->loader) {
					spdlog::warn("Menu guard:   the loader's log now: {}; its file opener now: {}",
						StateOwner(mgr->loader, RE::GFxState::StateType::kLog, g_guardLog),
						StateOwner(mgr->loader, RE::GFxState::StateType::kFileOpener, g_guardOpener));
				}
				spdlog::warn("Menu guard:   Scaleform messages since the game started: {} ({} ActionScript ones not written); file opens: {}", g_gfxMessages.load(),
					g_gfxScriptMessages.load(), g_opens.load());
			} else if (!g_gfxInstalled) {
				spdlog::warn("Menu guard:   Scaleform's log and file opener are not watched ({})", g_gfxStatus);
			}
			const auto data = GameFolder() / "Data";
			spdlog::warn("Menu guard:   {}", LooseFile(data / "Interface" / (a_movie + ".swf")));
			spdlog::warn("Menu guard:   {}", LooseFile(data / "Interface" / "Exported" / (a_movie + ".gfx")));
			spdlog::warn("Menu guard:   {}", GameFile("Interface/" + a_movie + ".swf"));
			spdlog::warn("Menu guard:   {}", GameFile("Interface/Exported/" + a_movie + ".gfx"));
			for (const auto& l : ProcessState()) {
				spdlog::warn("Menu guard:   {}", l);
			}
			std::vector<Call> ring;
			std::uint64_t     listed = 0;
			std::string       listedWhen;
			{
				std::scoped_lock l(g_lock);
				listed = g_listedSeq;
				listedWhen = g_listedWhen;
				for (const auto& c : g_ring) {
					if (c.seq > listed) {
						ring.push_back(c);
					}
				}
				g_listedSeq = g_seq;
				g_listedWhen = Now();
			}
			spdlog::warn("Menu guard:   the movie loads {} (oldest first, at most 50):",
				listed ? std::format("since the list written at {}", listedWhen) : std::string("so far"));
			for (const auto& c : ring) {
				spdlog::warn("Menu guard:     {}", CallText(c));
			}
		}

		std::string StepText(const Steps& s, const std::string& a_movie, DWORD a_error)
		{
			if (!g_stepHooks) {
				return std::format("which step failed is not known (the step hooks are not installed; see the start of this log); GetLastError {}", a_error);
			}
			if (s.fileChecks == 0 && !s.createCalled) {
				return "LoadMovie returned before its file check (a hook in front of the game's code returned at once)";
			}
			const bool found = (s.fileChecks > 0 && s.checkResult[0]) || (s.fileChecks > 1 && s.checkResult[1]);
			if (!found && !s.createCalled) {
				return std::format("the game's file check found neither Interface/{0}.swf nor Interface/Exported/{0}.gfx", a_movie);
			}
			if (!s.createCalled) {
				return "the file was found but GFxLoader::CreateMovie was not called";
			}
			if (!s.createOk) {
				return std::format("GFx could not read the movie: GFxLoader::CreateMovie(\"{}\") returned nothing after {:.1f} ms (GetLastError {})",
					s.createPath, s.createMs, s.createError);
			}
			return "the movie was read (CreateMovie worked) but GFxMovieDef::CreateInstance gave no movie";
		}

		std::vector<std::string> TestList()
		{
			std::vector<std::string> out;
			const auto&              v = Settings::Get().testFailMovies;
			std::size_t              a = 0;
			while (a <= v.size()) {
				auto b = v.find(',', a);
				if (b == std::string::npos) {
					b = v.size();
				}
				auto item = v.substr(a, b - a);
				std::erase_if(item, [](unsigned char c) { return std::isspace(c) != 0; });
				if (!item.empty()) {
					out.push_back(Lower(item));
				}
				a = b + 1;
			}
			return out;
		}

		bool ForcedFailure(const std::string& a_movie, int a_attempt)
		{
			const auto list = TestList();
			if (std::ranges::find(list, Lower(a_movie)) == list.end()) {
				return false;
			}
			return a_attempt == 1 || Settings::Get().testFailMode == "all";
		}

		// every frame between here and the creator wrapper can be unwound (has unwind data): only then is throwing safe
		__declspec(noinline) bool CanUnwindToWrapper()
		{
			if (!t_wrapperFrame) {
				return false;
			}
			CONTEXT ctx{};
			RtlCaptureContext(&ctx);
			for (int i = 0; i < 128; ++i) {
				if (ctx.Rsp > reinterpret_cast<DWORD64>(t_wrapperFrame)) {
					return true;  // past the wrapper's frame
				}
				DWORD64 imageBase = 0;
				auto*   fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
				if (!fe) {
					spdlog::warn("Menu guard: code at {:X} ({}) has no unwind data; the menu cannot be closed safely", ctx.Rip,
						ModuleName(ModuleOf(ctx.Rip)));
					return false;
				}
				PVOID   handlerData = nullptr;
				DWORD64 establisher = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
				if (!ctx.Rip) {
					return false;
				}
			}
			return false;
		}

		// a movie's load failed (every try) or worked only on a retry
		void CountFailure(const std::string& a_movie, bool a_recovered, bool a_forced, const std::string& a_step, const std::string& a_how)
		{
			std::scoped_lock l(g_lock);
			auto&            p = g_problems[a_movie];
			p.movie = a_movie;
			++p.failed;
			p.recovered += a_recovered ? 1 : 0;
			p.test = a_forced;
			p.lastStep = a_step;
			p.lastHow = a_how;
			p.lastTime = Now();
		}

		// the menu of a failed movie did not open (or was closed) instead of crashing
		void CountClosed(const std::string& a_movie, const std::string& a_how)
		{
			std::scoped_lock l(g_lock);
			auto&            p = g_problems[a_movie];
			p.movie = a_movie;
			++p.closed;
			p.lastHow = a_how;
		}

		// ------------------------------------------------------------------ background loads (StatsMenu), version 17
		// Checked at run time before hooking: slot 1 starts with these bytes (they hold the task offsets used here) and
		// calls LoadMovie at +0x29; slot 2 is exactly kTaskFinish. Both from 1.6.1170 (84665 0x141036640, 84666 0x141036680).
		constexpr std::uint8_t kTaskRun[] = { 0x48, 0x83, 0xEC, 0x38, 0xF3, 0x0F, 0x10, 0x41, 0x34, 0x8B, 0x41, 0x30, 0x4C, 0x8B, 0x49, 0x28,
			0x4C, 0x8B, 0x41, 0x20, 0x48, 0x8B, 0x51, 0x18, 0x48, 0x8B, 0x0D };
		constexpr std::uint8_t kTaskFinish[] = { 0x48, 0x8B, 0x49, 0x18, 0x48, 0x8B, 0x01, 0x48, 0xFF, 0x60, 0x18 };

		RE::IMenu*                  TaskMenu(void* a_task) { return *reinterpret_cast<RE::IMenu**>(static_cast<std::uint8_t*>(a_task) + 0x18); }
		RE::GPtr<RE::GFxMovieView>* TaskView(void* a_task)
		{
			return *reinterpret_cast<RE::GPtr<RE::GFxMovieView>**>(static_cast<std::uint8_t*>(a_task) + 0x20);
		}

		struct FailedTask
		{
			std::string movie, step;
			bool        forced = false;
		};
		std::map<void*, FailedTask> g_failedTasks;

		std::uintptr_t TaskRunHook(void* a_task)
		{
			const auto prev = t_task;
			t_task = a_task;
			const auto r = g_origTaskRun(a_task);
			t_task = prev;
			return r;
		}

		// on the main thread: send kHide to the menu if it is still open, then the notice
		void CloseAfterBackgroundLoad(RE::GPtr<RE::IMenu> a_menu, FailedTask a_info)
		{
			auto* task = SKSE::GetTaskInterface();
			if (!task) {
				return;
			}
			task->AddTask([menu = std::move(a_menu), info = std::move(a_info)]() {
				std::string name;
				if (auto* ui = RE::UI::GetSingleton()) {
					for (auto& [key, entry] : ui->menuMap) {
						if (entry.menu.get() == menu.get()) {
							name = key.c_str() ? key.c_str() : "";
							break;
						}
					}
				}
				if (menu->uiMovie) {
					spdlog::warn("Menu guard: the menu of {} has a movie after all; it is left open", info.movie);
					return;
				}
				const std::string how = "closed after its background load failed";
				if (name.empty()) {
					spdlog::warn("Menu guard: the menu of {} is already closed", info.movie);
				} else if (auto* q = RE::UIMessageQueue::GetSingleton()) {
					q->AddMessage(RE::BSFixedString(name), RE::UI_MESSAGE_TYPE::kHide, nullptr);
					spdlog::warn("Menu guard: {} is closed (kHide, as when it is closed before its movie has loaded)", name);
				}
				CountClosed(info.movie, how);
				Notice(NoticeText(name.empty() ? info.movie : name, info.movie, info.forced));
			});
		}

		std::uintptr_t TaskFinishHook(void* a_task)
		{
			auto* menu = TaskMenu(a_task);
			auto* view = TaskView(a_task);
			std::optional<FailedTask> failed;
			{
				std::scoped_lock l(g_lock);
				if (auto it = g_failedTasks.find(a_task); it != g_failedTasks.end()) {
					failed = it->second;
					g_failedTasks.erase(it);
				}
			}
			const bool noMovie = view && !view->get();
			if (!failed && !noMovie) {
				return g_origTaskFinish(a_task);
			}
			if (!failed) {
				// not seen failing (e.g. the load never ran): the "movie loaded" call would still use the missing movie
				spdlog::warn("Menu guard: a background movie load finished without a movie (thread {}); the menu is not told it loaded",
					ThreadText(GetCurrentThreadId()));
				return 0;
			}
			spdlog::warn("Menu guard: the background load of {} failed: the menu is not told the movie loaded (StatsMenu would use the missing movie) and is closed",
				failed->movie);
			if (menu) {
				CloseAfterBackgroundLoad(RE::GPtr<RE::IMenu>(menu), *failed);
			}
			return 0;
		}

		void InstallTaskHooks()
		{
			REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_BSScaleformMovieLoadTask[0] };
			const auto                      run = *reinterpret_cast<const std::uintptr_t*>(vtbl.address() + 1 * sizeof(void*));
			const auto                      finish = *reinterpret_cast<const std::uintptr_t*>(vtbl.address() + 2 * sizeof(void*));
			const bool runOk = Readable(run, 0x2E) && std::memcmp(reinterpret_cast<const void*>(run), kTaskRun, sizeof(kTaskRun)) == 0 &&
			                   reinterpret_cast<const std::uint8_t*>(run)[0x29] == 0xE8;
			const bool finishOk = Readable(finish, sizeof(kTaskFinish)) && std::memcmp(reinterpret_cast<const void*>(finish), kTaskFinish, sizeof(kTaskFinish)) == 0;
			if (!runOk || !finishOk) {
				spdlog::warn("Menu guard: BSScaleformMovieLoadTask (vtable {:X}) is not as expected (Run {:X} {}: {}; Finish {:X} {}: {}): background loads "
							 "(StatsMenu) are not guarded",
					vtbl.address(), run, ModuleText(run), Bytes(run, 16), finish, ModuleText(finish), Bytes(finish, 11));
				return;
			}
			const auto call = run + 0x29;
			const auto target = call + 5 + *reinterpret_cast<const std::int32_t*>(call + 1);
			spdlog::info("Menu guard: BSScaleformMovieLoadTask Run {:X} calls {:X}{}", run, target, target == g_loadMovie ? " = LoadMovie" : " (" + ModuleText(target) + ")");
			g_origTaskRun = reinterpret_cast<TaskFn_t*>(vtbl.write_vfunc(1, &TaskRunHook));
			g_origTaskFinish = reinterpret_cast<TaskFn_t*>(vtbl.write_vfunc(2, &TaskFinishHook));
			g_taskHooks = true;
			spdlog::info("Menu guard: background movie loads (StatsMenu) guarded: BSScaleformMovieLoadTask slots 1 and 2 hooked");
		}

		// ------------------------------------------------------------------ the first-message net, version 17
		// For a menu whose own movie failed while no creator wrapper could close it (its creator was not wrapped, or the
		// stack could not be unwound): its class's ProcessMessage (IMenu vtable slot 4) is wrapped. While that menu still
		// has no movie, kShow is answered kIgnore: UI::ProcessMessages (1.6.1170 0x140fa465a, result 1 -> 0x140fa4951)
		// then does not put it on the menu stack and, unless it is kAlwaysOpen, drops it from the menu map (0x140fa5c40),
		// the same path the stand-in menu takes. Other messages go to the menu as before (what they do without a movie is
		// up to the menu). PostCreate (slot 2), which the game calls before kShow, is not covered.
		struct NetMenu
		{
			std::string movie, step;
			bool        forced = false;
		};
		std::map<const RE::IMenu*, NetMenu>         g_netMenus;
		std::map<std::uintptr_t, ProcessMessage_t*> g_netOriginal;  // vtable -> its ProcessMessage before ours

		RE::UI_MESSAGE_RESULTS NetProcessMessage(RE::IMenu* a_menu, RE::UIMessage& a_message)
		{
			ProcessMessage_t* original = nullptr;
			std::optional<NetMenu> net;
			{
				std::scoped_lock l(g_lock);
				if (auto it = g_netOriginal.find(*reinterpret_cast<const std::uintptr_t*>(a_menu)); it != g_netOriginal.end()) {
					original = it->second;
				}
				if (a_message.type.get() == RE::UI_MESSAGE_TYPE::kShow && !a_menu->uiMovie) {
					if (auto it = g_netMenus.find(a_menu); it != g_netMenus.end()) {
						net = it->second;
						if (!a_menu->AlwaysOpen()) {
							g_netMenus.erase(it);
						}
					}
				}
			}
			if (net) {
				const std::string name = a_message.menu.c_str() ? a_message.menu.c_str() : net->movie;
				spdlog::warn("Menu guard: {} has no movie ({} did not load); its kShow is refused, so it does not open", name, net->movie);
				CountClosed(net->movie, "show refused (no creator wrapper on the stack)");
				Notice(NoticeText(name, net->movie, net->forced));
				return RE::UI_MESSAGE_RESULTS::kIgnore;
			}
			if (!original) {
				spdlog::error("Menu guard: no original ProcessMessage for vtable {:X}", *reinterpret_cast<const std::uintptr_t*>(a_menu));
				return RE::UI_MESSAGE_RESULTS::kIgnore;
			}
			return original(a_menu, a_message);
		}

		void AddToNet(RE::IMenu* a_menu, const std::string& a_movie, const std::string& a_step, bool a_forced)
		{
			const auto vtbl = *reinterpret_cast<const std::uintptr_t*>(a_menu);
			const auto slot = vtbl + 4 * sizeof(void*);
			std::scoped_lock l(g_lock);
			g_netMenus[a_menu] = { a_movie, a_step, a_forced };
			if (!g_netOriginal.contains(vtbl)) {
				const auto current = *reinterpret_cast<const std::uintptr_t*>(slot);
				if (current != reinterpret_cast<std::uintptr_t>(&NetProcessMessage)) {
					g_netOriginal[vtbl] = reinterpret_cast<ProcessMessage_t*>(current);
					const auto ours = reinterpret_cast<std::uintptr_t>(&NetProcessMessage);
					REL::safe_write(slot, &ours, sizeof(ours));
					spdlog::warn("Menu guard: ProcessMessage of vtable {:X} ({}) wrapped (was {:X} in {})", vtbl, ModuleText(vtbl), current, ModuleText(current));
				}
			}
		}

		// ------------------------------------------------------------------ the hooks
		bool FileExistsHook(const char* a_path)
		{
			const bool r = g_origFileExists(a_path);
			if (auto* s = t_steps; s && s->fileChecks < 2) {
				s->checkPath[s->fileChecks] = a_path ? a_path : "";
				s->checkResult[s->fileChecks] = r;
				++s->fileChecks;
			}
			return r;
		}

		RE::GFxMovieDef* CreateMovieHook(RE::GFxLoader* a_loader, const char* a_file, RE::GFxLoader::LoadConstants a_flags, RE::UPInt a_arena)
		{
			const auto t0 = std::chrono::steady_clock::now();
			SetLastError(0);
			auto*       def = g_origCreateMovie(a_loader, a_file, a_flags, a_arena);
			const DWORD err = GetLastError();
			if (auto* s = t_steps) {
				s->createCalled = true;
				s->createOk = def != nullptr;
				s->createPath = a_file ? a_file : "";
				s->createError = err;
				s->createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			}
			return def;
		}

		bool LoadMovieHook(RE::BSScaleformManager* a_mgr, RE::IMenu* a_menu, RE::GPtr<RE::GFxMovieView>& a_view, const char* a_name, ScaleMode a_mode,
			float a_alpha)
		{
			const std::string movie = a_name ? a_name : "(no name)";
			const bool        background = t_task && TaskMenu(t_task) == a_menu && TaskView(t_task) == &a_view;
			const std::string menu = t_menu ? t_menu : background ? "- (background load task)" : "-";
			const auto&       set = Settings::Get();
			const int         tries = 1 + std::clamp(set.menuLoadRetries, 0, 5);
			auto*             outerSteps = t_steps;
			bool              ok = false;
			std::string       firstStep;
			bool              forcedAny = false;
			int               attempt = 1;
			std::vector<Steps> done;
			for (; attempt <= tries; ++attempt) {
				if (attempt >= 3) {
					Sleep(50);
				}
				Steps      steps;
				const bool forced = ForcedFailure(movie, attempt);
				const auto t0 = std::chrono::steady_clock::now();
				DWORD      err = 0;
				if (forced) {
					// what a real failure leaves: the movie pointer emptied, false returned (the game's code is not run)
					a_view.reset();
					ok = false;
				} else {
					t_steps = &steps;
					SetLastError(0);
					ok = g_origLoadMovie(a_mgr, a_menu, a_view, a_name, a_mode, a_alpha);
					err = GetLastError();
					t_steps = outerSteps;
				}
				Call c;
				c.when = Now();
				c.movie = movie;
				c.menu = menu;
				c.ok = ok;
				c.forced = forced;
				c.tid = GetCurrentThreadId();
				c.attempt = attempt;
				c.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				if (!ok) {
					c.step = forced ? "TEST: forced failure (sTestFailMovies)" : StepText(steps, movie, err);
				}
				Remember(c);
				forcedAny = forcedAny || forced;
				if (ok) {
					if (set.logAllMovieLoads || attempt > 1) {
						spdlog::info("Menu guard: {}", CallText(c));
					}
					break;
				}
				if (!forced) {
					done.push_back(std::move(steps));
				}
				if (attempt == 1) {
					firstStep = c.step;
					if (forced) {
						spdlog::warn("Menu guard: TEST: forced failure of movie {} (sTestFailMovies, sTestFailMode={}) - not a real problem", movie, set.testFailMode);
					}
					spdlog::warn("Menu guard: movie {} did not load (menu {}): {}", movie, menu, CallText(c));
				} else {
					spdlog::warn("Menu guard: retry {}: {}", attempt - 1, CallText(c));
				}
				if (attempt < tries) {
					spdlog::info("Menu guard: trying {} again{}", movie, attempt + 1 >= 3 ? " after 50 ms" : "");
				}
			}
			if (ok) {
				if (attempt > 1) {
					spdlog::info("Menu guard: movie {} loaded on retry {}", movie, attempt - 1);
					CountFailure(movie, true, forcedAny, firstStep, "worked on a retry");
				}
				return true;
			}
			LogFailureDetails(movie, done);
			const bool ownMovie = a_menu && &a_view == &a_menu->uiMovie;
			if (t_depth > 0 && ownMovie && CanUnwindToWrapper()) {
				spdlog::warn("Menu guard: movie {} failed {} time(s); menu {} is closed instead of crashing the game", movie, tries, menu);
				CountFailure(movie, false, forcedAny, firstStep, "closed in its creator");
				throw MovieLoadAbort{ movie, firstStep };
			}
			if (background && g_taskHooks) {
				spdlog::warn("Menu guard: movie {} failed {} time(s) in the game's background load task (thread {}); the menu is closed when the task finishes",
					movie, tries, ThreadText(GetCurrentThreadId()));
				CountFailure(movie, false, forcedAny, firstStep, "background load failed");
				std::scoped_lock l(g_lock);
				g_failedTasks[t_task] = { movie, firstStep, forcedAny };
				return false;
			}
			if (ownMovie) {
				spdlog::warn("Menu guard: movie {} failed {} time(s) with no creator wrapper able to close the menu ({}); the menu will not be shown without its movie",
					movie, tries, t_depth == 0 ? "not loaded while a menu is being created" : "the stack cannot be unwound");
				CountFailure(movie, false, forcedAny, firstStep, "went back to the game; show refused");
				AddToNet(a_menu, movie, firstStep, forcedAny);
				return false;
			}
			CountFailure(movie, false, forcedAny, firstStep, "went back to the game (not the menu's own movie)");
			spdlog::warn("Menu guard: movie {} failed {} time(s); the failure goes back to the game (it is not the menu's own movie: the menu handles it)", movie, tries);
			return false;
		}

		// ------------------------------------------------------------------ the stand-in menu and the creator wrappers
		// Answers kIgnore to everything. On "show", UI::ProcessMessages (1.6.1170 0x140fa4660) then removes it from the menu map
		// (0x140fa5c40: it has no fxDelegate and is not kAlwaysOpen) and releases it; the menu never goes on the menu stack.
		class LoadFailedMenu final : public RE::IMenu
		{
		public:
			LoadFailedMenu() = default;
			~LoadFailedMenu() override = default;

			RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage&) override { return RE::UI_MESSAGE_RESULTS::kIgnore; }
			void                   AdvanceMovie(float, std::uint32_t) override {}
			void                   PostDisplay() override {}
			void                   RefreshPlatform() override {}
		};

		struct Creator
		{
			std::string    name;
			Create_t*      original = nullptr;
			std::uintptr_t thunk = 0;
		};
		constexpr std::size_t kMaxCreators = 1024;
		constexpr std::size_t kThunkSize = 24;
		std::vector<Creator>  g_creators;  // reserved once, so entries never move
		std::uint8_t*         g_thunks = nullptr;

		RE::IMenu* CreatorWrapper(std::size_t a_index)
		{
			const Creator& c = g_creators[a_index];
			int            frameMark = 0;  // its address marks this frame for CanUnwindToWrapper
			const auto     prevMenu = t_menu;
			const auto     prevFrame = t_wrapperFrame;
			++t_depth;
			t_menu = c.name.c_str();
			t_wrapperFrame = &frameMark;
			try {
				RE::IMenu* m = c.original();
				--t_depth;
				t_menu = prevMenu;
				t_wrapperFrame = prevFrame;
				return m;
			} catch (const MovieLoadAbort& e) {
				--t_depth;
				t_menu = prevMenu;
				t_wrapperFrame = prevFrame;
				const bool forced = e.step.starts_with("TEST:");
				CountClosed(e.movie, "closed in its creator");
				spdlog::warn("Menu guard: {} was not opened (its movie {} did not load); the game's cleanup ran, a stand-in menu closes it", c.name, e.movie);
				Notice(NoticeText(c.name, e.movie, forced));
				return new LoadFailedMenu();
			}
		}

		bool IsOurThunk(const void* a_fn)
		{
			const auto p = reinterpret_cast<std::uintptr_t>(a_fn);
			const auto b = reinterpret_cast<std::uintptr_t>(g_thunks);
			return g_thunks && p >= b && p < b + kMaxCreators * kThunkSize;
		}

		// mov rcx, index ; mov rax, CreatorWrapper ; jmp rax  (no stack frame of its own)
		std::uintptr_t MakeThunk(std::size_t a_index)
		{
			std::uint8_t* t = g_thunks + a_index * kThunkSize;
			const auto    fn = reinterpret_cast<std::uint64_t>(&CreatorWrapper);
			const auto    idx = static_cast<std::uint64_t>(a_index);
			t[0] = 0x48;
			t[1] = 0xB9;
			std::memcpy(t + 2, &idx, 8);
			t[10] = 0x48;
			t[11] = 0xB8;
			std::memcpy(t + 12, &fn, 8);
			t[20] = 0xFF;
			t[21] = 0xE0;
			t[22] = 0xCC;
			t[23] = 0xCC;
			FlushInstructionCache(GetCurrentProcess(), t, kThunkSize);
			return reinterpret_cast<std::uintptr_t>(t);
		}

		// ------------------------------------------------------------------ installing
		// the first 7 bytes of LoadMovie on 1.6.1170: mov rax,rsp ; mov [rax+0x10],rdx (position independent)
		constexpr std::uint8_t kPrologue[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x50, 0x10 };

		bool HookEntry()
		{
			auto&      tramp = SKSE::GetTrampoline();
			const auto p = reinterpret_cast<const std::uint8_t*>(g_loadMovie);
			if (!Readable(g_loadMovie, 16)) {
				g_status = "LoadMovie is not readable";
				return false;
			}
			std::uintptr_t orig = 0;
			if (std::memcmp(p, kPrologue, sizeof(kPrologue)) == 0) {
				// gateway: the 7 bytes, then jmp [rip+0] to LoadMovie+7
				auto* gate = static_cast<std::uint8_t*>(tramp.allocate(sizeof(kPrologue) + 14));
				std::uint8_t code[sizeof(kPrologue) + 14]{};
				std::memcpy(code, kPrologue, sizeof(kPrologue));
				code[7] = 0xFF;
				code[8] = 0x25;
				const std::uint64_t back = g_loadMovie + sizeof(kPrologue);
				std::memcpy(code + 13, &back, 8);
				REL::safe_write(reinterpret_cast<std::uintptr_t>(gate), code, sizeof(code));
				FlushInstructionCache(GetCurrentProcess(), gate, sizeof(code));
				orig = reinterpret_cast<std::uintptr_t>(gate);
			} else if (p[0] == 0xE9 || (p[0] == 0xFF && p[1] == 0x25) || (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0)) {
				// another mod's jump is already there: we go in front of it and call it as "the original"
				const auto* q = p;
				if (q[0] == 0xE9) {
					orig = g_loadMovie + 5 + *reinterpret_cast<const std::int32_t*>(q + 1);
				} else if (q[0] == 0xFF) {
					const auto slot = g_loadMovie + 6 + *reinterpret_cast<const std::int32_t*>(q + 2);
					orig = Readable(slot, 8) ? *reinterpret_cast<const std::uintptr_t*>(slot) : 0;
				} else {
					orig = *reinterpret_cast<const std::uintptr_t*>(q + 2);
				}
				spdlog::info("Menu guard: LoadMovie already starts with another mod's jump to {:X} ({}); the guard calls it as the original", orig,
					ModuleName(ModuleOf(orig)));
			} else {
				g_status = std::format("LoadMovie starts with unexpected bytes {}: not hooked", Bytes(g_loadMovie, 12));
				return false;
			}
			if (!orig) {
				g_status = "could not read the jump already at LoadMovie";
				return false;
			}
			g_origLoadMovie = reinterpret_cast<LoadMovie_t*>(orig);
			tramp.write_branch<5>(g_loadMovie, &LoadMovieHook);
			return true;
		}

		// the E8 calls inside LoadMovie whose target is exactly FileExists / CreateMovie (body bounds from the unwind table)
		void HookSteps()
		{
			DWORD64 imageBase = 0;
			auto*   fe = RtlLookupFunctionEntry(g_loadMovie, &imageBase, nullptr);
			if (!fe) {
				spdlog::warn("Menu guard: no unwind entry for LoadMovie; step hooks not installed");
				return;
			}
			const auto begin = static_cast<std::uintptr_t>(imageBase + fe->BeginAddress);
			const auto end = static_cast<std::uintptr_t>(imageBase + fe->EndAddress);
			std::vector<std::uintptr_t> exists, create;
			for (auto a = begin + 7; a + 5 <= end; ++a) {
				const auto* q = reinterpret_cast<const std::uint8_t*>(a);
				if (q[0] != 0xE8) {
					continue;
				}
				const auto target = a + 5 + *reinterpret_cast<const std::int32_t*>(q + 1);
				if (target == g_fileExists) {
					exists.push_back(a);
				} else if (target == g_createMovie) {
					create.push_back(a);
				} else if (!ModuleName(ModuleOf(target)).empty() && ModuleOf(target) != ModuleOf(g_loadMovie)) {
					spdlog::info("Menu guard: LoadMovie+{:X} calls into {} ({:X})", a - begin, ModuleName(ModuleOf(target)), target);
				} else if (ModuleOf(target) == nullptr) {
					std::string path;
					const auto  last = FollowJumps(target, &path);
					if (ModuleOf(last) && ModuleOf(last) != ModuleOf(g_loadMovie)) {
						spdlog::info("Menu guard: LoadMovie+{:X} calls a hook: {:X}{}", a - begin, target, path);
					}
				}
			}
			spdlog::info("Menu guard: LoadMovie {:X}-{:X}: {} FileExists call(s), {} CreateMovie call(s) (1.6.1170: 2 and 1)", begin, end, exists.size(),
				create.size());
			if (exists.size() != 2 || create.size() != 1) {
				spdlog::warn("Menu guard: not the expected calls (another mod may have hooked them); step hooks not installed");
				return;
			}
			auto& tramp = SKSE::GetTrampoline();
			for (auto a : exists) {
				g_origFileExists = reinterpret_cast<FileExists_t*>(tramp.write_call<5>(a, &FileExistsHook));
			}
			g_origCreateMovie = reinterpret_cast<CreateMovie_t*>(tramp.write_call<5>(create[0], &CreateMovieHook));
			g_stepHooks = true;
			spdlog::info("Menu guard: step hooks installed at LoadMovie+{:X}, +{:X} (FileExists) and +{:X} (CreateMovie)", exists[0] - begin,
				exists[1] - begin, create[0] - begin);
		}
	}

	void Prepare()
	{
		if (!Settings::Get().menuLoadGuard) {
			spdlog::info("Menu guard: off (bMenuLoadGuard=0): no hooks");
			return;
		}
		if (REL::Module::IsVR()) {
			spdlog::info("Menu guard: not on Skyrim VR (its addresses are not checked)");
			return;
		}
		SKSE::AllocTrampoline(256);
	}

	void Install()
	{
		if (!Settings::Get().menuLoadGuard) {
			g_status = "off (bMenuLoadGuard=0)";
			return;
		}
		if (REL::Module::IsVR()) {
			g_status = "not on Skyrim VR";
			return;
		}
		g_loadMovie = RELOCATION_ID(80302, 82325).address();
		g_fileExists = RELOCATION_ID(80087, 82411).address();
		g_createMovie = RELOCATION_ID(80620, 84640).address();
		LogHookChain("at start, before the guard hooks LoadMovie");
		HookSteps();
		if (!HookEntry()) {
			spdlog::warn("Menu guard: {}", g_status);
			return;
		}
		g_installed = true;
		InstallTaskHooks();
		g_status = std::format("on: LoadMovie hooked{}, {} retr{}{}", g_stepHooks ? " with step checks" : " (no step checks)", Settings::Get().menuLoadRetries,
			Settings::Get().menuLoadRetries == 1 ? "y" : "ies", g_taskHooks ? ", background loads guarded" : ", background loads NOT guarded");
		spdlog::info("Menu guard: {}{}", g_status, Settings::Get().testFailMovies.empty() ? "" :
		                                         std::format("; TEST: sTestFailMovies={} sTestFailMode={}", Settings::Get().testFailMovies, Settings::Get().testFailMode));
		InstallGfxDiagnostics("at start");
	}

	void WrapCreators(const char* a_when)
	{
		if (!g_installed) {
			return;
		}
		if (!g_mainThread) {
			g_mainThread = GetCurrentThreadId();  // kDataLoaded comes on the main thread
		}
		InstallGfxDiagnostics(a_when);
		if (g_gfxInstalled) {
			spdlog::info("Menu guard: {} ({})", ScaleformHeap(), a_when);
		}
		if (!g_thunks) {
			g_thunks = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kMaxCreators * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
			if (!g_thunks) {
				spdlog::warn("Menu guard: no memory for the menu creator wrappers (error {})", GetLastError());
				return;
			}
			g_creators.reserve(kMaxCreators);
		}
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			return;
		}
		int added = 0;
		for (auto& kv : ui->menuMap) {
			auto& entry = kv.second;
			if (!entry.create || IsOurThunk(reinterpret_cast<const void*>(entry.create))) {
				continue;
			}
			if (g_creators.size() >= kMaxCreators) {
				spdlog::warn("Menu guard: more than {} menus; {} is not wrapped", kMaxCreators, kv.first.c_str());
				continue;
			}
			const auto index = g_creators.size();
			g_creators.push_back({ kv.first.c_str() ? kv.first.c_str() : "?", entry.create, 0 });
			g_creators.back().thunk = MakeThunk(index);
			entry.create = reinterpret_cast<Create_t*>(g_creators.back().thunk);
			spdlog::info("Menu guard: wrapped the creator of {} ({:X} in {})", g_creators.back().name, reinterpret_cast<std::uintptr_t>(g_creators.back().original),
				ModuleText(reinterpret_cast<std::uintptr_t>(g_creators.back().original)));
			++added;
		}
		spdlog::info("Menu guard: {} menu creator(s) wrapped ({}; {} in all)", added, a_when, g_creators.size());
		LogHookChain(a_when);
	}

	std::vector<Problem> Problems()
	{
		std::scoped_lock     l(g_lock);
		std::vector<Problem> out;
		for (const auto& [k, v] : g_problems) {
			out.push_back(v);
		}
		return out;
	}

	std::string StatusLine() { return g_status + (g_installed ? "; " + g_gfxStatus : std::string()); }

	std::string ReportText()
	{
		std::string r = "Menu load guard: " + StatusLine() + "\n";
		const auto  probs = Problems();
		if (probs.empty()) {
			r += "Menu load problems: none\n";
			return r;
		}
		r += "Menu load problems:\n";
		for (const auto& p : probs) {
			r += std::format("  {}{}: failed {}, worked on retry {}, menu not opened {}; last at {} ({}): {}\n", p.movie, p.test ? " (TEST)" : "", p.failed,
				p.recovered, p.closed, p.lastTime, p.lastHow, p.lastStep);
		}
		return r;
	}
}
