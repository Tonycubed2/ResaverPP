#include "PCH.h"
#include "MenuGuard.h"

#include "Game.h"
#include "Settings.h"

#include <psapi.h>
#include <intrin.h>

#include <REX/W32/DXGI.h>
#include <REX/W32/DXGI_4.h>

#include <cctype>
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
namespace MenuGuard
{
	namespace
	{
		using ScaleMode = RE::GFxMovieView::ScaleModeType;
		using LoadMovie_t = bool(RE::BSScaleformManager*, RE::IMenu*, RE::GPtr<RE::GFxMovieView>&, const char*, ScaleMode, float);
		using FileExists_t = bool(const char*);
		using CreateMovie_t = RE::GFxMovieDef*(RE::GFxLoader*, const char*, RE::GFxLoader::LoadConstants, RE::UPInt);
		using Create_t = RE::UI::Create_t;

		// thrown by the LoadMovie hook, caught by the creator wrapper (only when the wrapper is on the stack)
		struct MovieLoadAbort
		{
			std::string movie;
			std::string step;
		};

		LoadMovie_t*   g_origLoadMovie = nullptr;
		FileExists_t*  g_origFileExists = nullptr;
		CreateMovie_t* g_origCreateMovie = nullptr;
		bool           g_installed = false;
		bool           g_stepHooks = false;
		std::string    g_status = "not installed";
		std::uintptr_t g_loadMovie = 0, g_fileExists = 0, g_createMovie = 0;
		DWORD          g_mainThread = 0;

		// ------------------------------------------------------------------ what one LoadMovie try did
		struct Steps
		{
			int         fileChecks = 0;
			std::string checkPath[2];
			bool        checkResult[2]{};
			bool        createCalled = false;
			bool        createOk = false;
			std::string createPath;
			DWORD       createError = 0;
			double      createMs = 0;
		};
		thread_local Steps* t_steps = nullptr;

		// the creator wrapper on this thread: how deep, which menu, and the address of a local in its frame
		thread_local int         t_depth = 0;
		thread_local const char* t_menu = nullptr;
		thread_local const void* t_wrapperFrame = nullptr;

		// ------------------------------------------------------------------ ring of the last 50 calls, failure counts
		struct Call
		{
			std::string when, movie, menu, step;
			bool        ok = false, forced = false;
			double      ms = 0;
			DWORD       tid = 0;
			int         attempt = 0;
		};
		std::mutex                         g_lock;
		std::deque<Call>                   g_ring;
		std::map<std::string, Problem>     g_problems;

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

		void Remember(const Call& a_call)
		{
			std::scoped_lock l(g_lock);
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
			}
		}

		// ------------------------------------------------------------------ process state at a failure
		std::string Mb(std::uint64_t a_bytes) { return std::format("{:.0f} MB", static_cast<double>(a_bytes) / (1024.0 * 1024.0)); }

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
			return out;
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

		void LogFailureDetails(const std::string& a_movie)
		{
			const auto data = GameFolder() / "Data";
			spdlog::warn("Menu guard:   {}", LooseFile(data / "Interface" / (a_movie + ".swf")));
			spdlog::warn("Menu guard:   {}", LooseFile(data / "Interface" / "Exported" / (a_movie + ".gfx")));
			spdlog::warn("Menu guard:   {}", GameFile("Interface/" + a_movie + ".swf"));
			spdlog::warn("Menu guard:   {}", GameFile("Interface/Exported/" + a_movie + ".gfx"));
			for (const auto& l : ProcessState()) {
				spdlog::warn("Menu guard:   {}", l);
			}
			std::vector<Call> ring;
			{
				std::scoped_lock l(g_lock);
				ring.assign(g_ring.begin(), g_ring.end());
			}
			spdlog::warn("Menu guard:   the last {} movie loads (oldest first):", ring.size());
			for (const auto& c : ring) {
				spdlog::warn("Menu guard:     {}", CallText(c));
			}
		}

		std::string StepText(const Steps& s, const std::string& a_movie)
		{
			if (!g_stepHooks) {
				return "which step failed is not known (the step hooks are not installed; see the start of this log)";
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

		void Count(const std::string& a_movie, bool a_recovered, bool a_closed, bool a_forced, const std::string& a_step)
		{
			std::scoped_lock l(g_lock);
			auto&            p = g_problems[a_movie];
			p.movie = a_movie;
			++p.failed;
			p.recovered += a_recovered ? 1 : 0;
			p.closed += a_closed ? 1 : 0;
			p.test = a_forced;
			p.lastStep = a_step;
			p.lastTime = Now();
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
			const std::string menu = t_menu ? t_menu : "-";
			const auto&       set = Settings::Get();
			const int         tries = 1 + std::clamp(set.menuLoadRetries, 0, 5);
			auto*             outerSteps = t_steps;
			bool              ok = false;
			std::string       firstStep;
			bool              forcedAny = false;
			int               attempt = 1;
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
					c.step = forced ? "TEST: forced failure (sTestFailMovies)" : StepText(steps, movie) + std::format("; GetLastError {}", err);
				}
				Remember(c);
				forcedAny = forcedAny || forced;
				if (ok) {
					if (set.logAllMovieLoads || attempt > 1) {
						spdlog::info("Menu guard: {}", CallText(c));
					}
					break;
				}
				if (attempt == 1) {
					firstStep = c.step;
					if (forced) {
						spdlog::warn("Menu guard: TEST: forced failure of movie {} (sTestFailMovies, sTestFailMode={}) - not a real problem", movie, set.testFailMode);
					}
					spdlog::warn("Menu guard: movie {} did not load (menu {}): {}", movie, menu, CallText(c));
					LogFailureDetails(movie);
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
					Count(movie, true, false, forcedAny, firstStep);
				}
				return true;
			}
			if (t_depth > 0 && a_menu && &a_view == &a_menu->uiMovie && CanUnwindToWrapper()) {
				spdlog::warn("Menu guard: movie {} failed {} time(s); menu {} is closed instead of crashing the game", movie, tries, menu);
				throw MovieLoadAbort{ movie, firstStep };
			}
			Count(movie, false, false, forcedAny, firstStep);
			spdlog::warn("Menu guard: movie {} failed {} time(s); the failure goes back to the game ({})", movie, tries,
				t_depth == 0 ? "not loaded while a menu is being created" : "not the menu's own movie, or the stack cannot be unwound");
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
				Count(e.movie, false, true, forced, e.step);
				spdlog::warn("Menu guard: {} was not opened (its movie {} did not load); the game's cleanup ran, a stand-in menu closes it", c.name, e.movie);
				Game::Notify("hud", std::format("ReSaverHelper: {} could not load its interface file ({}.swf){}; see ReSaverHelper.log", c.name, e.movie,
										forced ? " - test" : ""));
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
		g_status = std::format("on: LoadMovie hooked{}, {} retr{}", g_stepHooks ? " with step checks" : " (no step checks)", Settings::Get().menuLoadRetries,
			Settings::Get().menuLoadRetries == 1 ? "y" : "ies");
		spdlog::info("Menu guard: {}{}", g_status, Settings::Get().testFailMovies.empty() ? "" :
		                                         std::format("; TEST: sTestFailMovies={} sTestFailMode={}", Settings::Get().testFailMovies, Settings::Get().testFailMode));
	}

	void WrapCreators(const char* a_when)
	{
		if (!g_installed) {
			return;
		}
		if (!g_mainThread) {
			g_mainThread = GetCurrentThreadId();  // kDataLoaded comes on the main thread
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

	std::string StatusLine() { return g_status; }

	std::string ReportText()
	{
		std::string r = "Menu load guard: " + g_status + "\n";
		const auto  probs = Problems();
		if (probs.empty()) {
			r += "Menu load problems: none\n";
			return r;
		}
		r += "Menu load problems:\n";
		for (const auto& p : probs) {
			r += std::format("  {}{}: failed {}, worked on retry {}, menu not opened {}; last at {}: {}\n", p.movie, p.test ? " (TEST)" : "", p.failed,
				p.recovered, p.closed, p.lastTime, p.lastStep);
		}
		return r;
	}
}
