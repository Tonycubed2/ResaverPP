// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// What PC this is, for "Collect all logs": computer, Windows (or Wine), processor, memory, video cards, screen,
// the game's version. Read from the registry, DXGI and the game exe's version data; nothing is installed or sent.
#include "Platform.h"

#ifdef _WIN32
#	include <windows.h>
#	include <dxgi.h>
#	include <shlobj.h>
#endif

#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

namespace platform
{
#ifdef _WIN32
	namespace
	{
		std::string Narrow(const std::wstring& w)
		{
			if (w.empty()) {
				return {};
			}
			const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
			std::string s(static_cast<std::size_t>(n), '\0');
			WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
			while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) {
				s.pop_back();
			}
			return s;
		}

		std::string Reg(const wchar_t* a_key, const wchar_t* a_value)
		{
			wchar_t buf[512] = L"";
			DWORD   size = sizeof(buf);
			if (RegGetValueW(HKEY_LOCAL_MACHINE, a_key, a_value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
				return {};
			}
			return Narrow(buf);
		}

		DWORD RegDword(const wchar_t* a_key, const wchar_t* a_value)
		{
			DWORD v = 0, size = sizeof(v);
			RegGetValueW(HKEY_LOCAL_MACHINE, a_key, a_value, RRF_RT_REG_DWORD, nullptr, &v, &size);
			return v;
		}

		std::string Gb(unsigned long long a_bytes)
		{
			char b[32];
			std::snprintf(b, sizeof(b), "%.1f GB", a_bytes / 1073741824.0);
			return b;
		}

		std::string FileVersion(const std::filesystem::path& a_file)
		{
			using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
			using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
			using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
			HMODULE v = LoadLibraryW(L"version.dll");
			if (!v) {
				return {};
			}
			auto size = reinterpret_cast<SizeFn>(GetProcAddress(v, "GetFileVersionInfoSizeW"));
			auto info = reinterpret_cast<InfoFn>(GetProcAddress(v, "GetFileVersionInfoW"));
			auto query = reinterpret_cast<QueryFn>(GetProcAddress(v, "VerQueryValueW"));
			std::string out;
			if (size && info && query) {
				DWORD      h = 0;
				const auto n = size(a_file.c_str(), &h);
				std::vector<std::uint8_t> data(n);
				VS_FIXEDFILEINFO*         ffi = nullptr;
				UINT                      len = 0;
				if (n && info(a_file.c_str(), 0, n, data.data()) && query(data.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) && ffi) {
					char b[64];
					std::snprintf(b, sizeof(b), "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
						HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
					out = b;
				}
			}
			FreeLibrary(v);
			return out;
		}
	}

	std::filesystem::path DesktopFolder()
	{
		PWSTR                 p = nullptr;
		std::filesystem::path out;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) {
			out = p;
			CoTaskMemFree(p);
		}
		return out;
	}

	std::filesystem::path LocalAppDataFolder()
	{
		PWSTR                 p = nullptr;
		std::filesystem::path out;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) {
			out = p;
			CoTaskMemFree(p);
		}
		return out;
	}

	void ShowInFolder(const std::filesystem::path& a_file)
	{
		const std::wstring args = L"/select,\"" + a_file.wstring() + L"\"";
		ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
	}

	std::string SystemReport(const std::filesystem::path& a_gameExe)
	{
		std::ostringstream o;
		const wchar_t* cv = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
		const wchar_t* bios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
		const auto maker = Reg(bios, L"SystemManufacturer"), model = Reg(bios, L"SystemProductName");
		if (!maker.empty() || !model.empty()) {
			o << "Computer: " << maker << " " << model << "\r\n";
		}
		const auto board = Reg(bios, L"BaseBoardManufacturer") + " " + Reg(bios, L"BaseBoardProduct");
		if (board != " ") {
			o << "Motherboard: " << board << " (BIOS " << Reg(bios, L"BIOSVersion") << ")\r\n";
		}

		// Windows: the build number tells 10 from 11 (ProductName says "Windows 10" on both)
		OSVERSIONINFOW vi{};
		vi.dwOSVersionInfoSize = sizeof(vi);
		using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
			fn(&vi);
		}
		std::string product = Reg(cv, L"ProductName");
		if (vi.dwBuildNumber >= 22000 && product.find("Windows 10") != std::string::npos) {
			product.replace(product.find("Windows 10"), 10, "Windows 11");
		}
		o << "Windows: " << product << " " << Reg(cv, L"DisplayVersion") << " (build " << vi.dwBuildNumber << "." << RegDword(cv, L"UBR")
		  << ")\r\n";
		using WineVersionFn = const char*(__cdecl*)();
		using WineHostFn = void(__cdecl*)(const char**, const char**);
		if (auto wv = reinterpret_cast<WineVersionFn>(GetProcAddress(ntdll, "wine_get_version"))) {
			o << "Runs under Wine " << wv();
			if (auto wh = reinterpret_cast<WineHostFn>(GetProcAddress(ntdll, "wine_get_host_version"))) {
				const char *sys = nullptr, *rel = nullptr;
				wh(&sys, &rel);
				o << " on " << (sys ? sys : "?") << " " << (rel ? rel : "");
			}
			o << " (Linux / Steam Proton / Steam Deck)\r\n";
		}

		o << "Processor: " << Reg(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString") << " ("
		  << std::thread::hardware_concurrency() << " threads)\r\n";
		MEMORYSTATUSEX ms{};
		ms.dwLength = sizeof(ms);
		if (GlobalMemoryStatusEx(&ms)) {
			o << "Memory: " << Gb(ms.ullTotalPhys) << " (" << Gb(ms.ullAvailPhys) << " free now); page file " << Gb(ms.ullTotalPageFile)
			  << "\r\n";
		}

		// video cards with their own memory (DXGI, loaded only here)
		using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
		if (HMODULE dx = LoadLibraryW(L"dxgi.dll")) {
			if (auto create = reinterpret_cast<CreateFn>(GetProcAddress(dx, "CreateDXGIFactory1"))) {
				IDXGIFactory1* f = nullptr;
				if (SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) && f) {
					IDXGIAdapter1* a = nullptr;
					for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
						DXGI_ADAPTER_DESC1 d{};
						if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
							char b[64];
							std::snprintf(b, sizeof(b), " (vendor %04X, device %04X)", d.VendorId, d.DeviceId);
							o << "Video card: " << Narrow(d.Description) << ", " << Gb(d.DedicatedVideoMemory) << " video memory" << b << "\r\n";
						}
						a->Release();
					}
					f->Release();
				}
			}
			FreeLibrary(dx);
		}
		o << "Screen: " << GetSystemMetrics(SM_CXSCREEN) << " x " << GetSystemMetrics(SM_CYSCREEN) << " (" << GetSystemMetrics(SM_CMONITORS)
		  << " monitor(s))\r\n";
		if (!a_gameExe.empty()) {
			std::error_code ec;
			if (std::filesystem::exists(a_gameExe, ec)) {
				o << "Game: " << ToUtf8(a_gameExe) << " version " << FileVersion(a_gameExe) << "\r\n";
			} else {
				o << "Game: not found (" << ToUtf8(a_gameExe) << ")\r\n";
			}
		}
		return o.str();
	}
#else
	std::filesystem::path DesktopFolder()
	{
		const char* home = std::getenv("HOME");
		if (!home) {
			return {};
		}
		std::error_code ec;
		const auto d = std::filesystem::path(home) / "Desktop";
		return std::filesystem::is_directory(d, ec) ? d : std::filesystem::path(home);
	}
	std::filesystem::path LocalAppDataFolder() { return {}; }
	void                  ShowInFolder(const std::filesystem::path&) {}
	std::string           SystemReport(const std::filesystem::path&)
	{
		std::ostringstream o;
		auto               first = [](const char* a_file, const char* a_key) {
            std::ifstream f(a_file);
            for (std::string l; std::getline(f, l);) {
                if (l.rfind(a_key, 0) == 0) {
                    return l;
                }
            }
            return std::string();
		};
		o << "System: " << first("/etc/os-release", "PRETTY_NAME") << "\n";
		o << "Processor: " << first("/proc/cpuinfo", "model name") << " (" << std::thread::hardware_concurrency() << " threads)\n";
		o << "Memory: " << first("/proc/meminfo", "MemTotal") << "\n";
		return o.str();
	}
#endif
}
