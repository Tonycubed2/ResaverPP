// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Symbols.h"

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#	include <dbghelp.h>
#	include <mutex>
#endif

namespace symbols
{
#ifdef _WIN32
	namespace
	{
		std::string Utf8(const wchar_t* a_s)
		{
			if (!a_s || !*a_s) {
				return {};
			}
			const int n = WideCharToMultiByte(CP_UTF8, 0, a_s, -1, nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
			if (n > 1) {
				WideCharToMultiByte(CP_UTF8, 0, a_s, -1, out.data(), n, nullptr, nullptr);
			}
			return out;
		}
	}

	Result At(const std::filesystem::path& a_dll, std::uint64_t a_offset)
	{
		static std::mutex lock;  // DbgHelp is not thread safe
		std::lock_guard   guard(lock);
		Result            r;
		std::error_code   ec;
		if (!std::filesystem::is_regular_file(a_dll, ec)) {
			r.error = "the DLL was not found";
			return r;
		}
		// a made-up process handle: nothing is attached, the DLL is only read
		const HANDLE proc = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x52535059));
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_EXACT_SYMBOLS);
		// look for the .pdb only next to the DLL (the mod's own folder): no symbol server, nothing downloaded
		const std::wstring dir = a_dll.parent_path().wstring();
		if (!SymInitializeW(proc, dir.c_str(), FALSE)) {
			r.error = "DbgHelp could not start";
			return r;
		}
		constexpr DWORD64 kBase = 0x10000000;
		const DWORD64     base = SymLoadModuleExW(proc, nullptr, a_dll.wstring().c_str(), nullptr, kBase, 0, nullptr, 0);
		if (!base) {
			r.error = "the DLL could not be read";
			SymCleanup(proc);
			return r;
		}
		IMAGEHLP_MODULEW64 mi{};
		mi.SizeOfStruct = sizeof(mi);
		if (!SymGetModuleInfoW64(proc, base, &mi) || mi.SymType != SymPdb) {
			// (SymExport: only the DLL's exported names, which would name the wrong function)
			r.error = "no matching .pdb next to the DLL (the mod's author has to ship the .pdb of this exact build)";
			SymUnloadModule64(proc, base);
			SymCleanup(proc);
			return r;
		}
		alignas(SYMBOL_INFOW) unsigned char buf[sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t)]{};
		auto*                               sym = reinterpret_cast<SYMBOL_INFOW*>(buf);
		sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
		sym->MaxNameLen = 511;
		DWORD64 disp = 0;
		if (SymFromAddrW(proc, base + a_offset, &disp, sym)) {
			r.ok = true;
			r.function = Utf8(sym->Name);
			if (disp) {
				char hex[32];
				std::snprintf(hex, sizeof(hex), " + 0x%llX", static_cast<unsigned long long>(disp));
				r.function += hex;
			}
			IMAGEHLP_LINEW64 line{};
			line.SizeOfStruct = sizeof(line);
			DWORD lineDisp = 0;
			if (SymGetLineFromAddrW64(proc, base + a_offset, &lineDisp, &line) && line.FileName) {
				r.line = Utf8(line.FileName) + ":" + std::to_string(line.LineNumber);
			}
		} else {
			r.error = "the .pdb has no function at this offset";
		}
		SymUnloadModule64(proc, base);
		SymCleanup(proc);
		return r;
	}
#else
	Result At(const std::filesystem::path&, std::uint64_t)
	{
		Result r;
		r.error = "function names from a .pdb are read on Windows only";
		return r;
	}
#endif
}
