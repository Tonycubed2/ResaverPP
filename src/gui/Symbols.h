// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Function names for a crash log's frames in a mod's DLL, from the .pdb its author ships next to it (Windows DbgHelp,
// read offline: the DLL is loaded as data at a made-up address, never run). Crash Logger SSE does this inside the game for
// Skyrim; for Starfield's Trainwreck and Starfield Engine Fixes logs, which give only "Mod.dll + 0x1A2B", ReSaver Plus does
// it afterwards.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace symbols
{
	struct Result
	{
		bool        ok = false;
		std::string function;  // "Hooks::OnLoad(RE::TESForm*) + 0x2A"
		std::string line;      // "src\\Hooks.cpp:118", when the .pdb has line numbers
		std::string error;     // why not: no .pdb next to the DLL, a .pdb of another build...
	};
	// a_dll: the DLL file; a_offset: the offset in it the log gives (from the DLL's start). Elsewhere than Windows: not ok.
	Result At(const std::filesystem::path& a_dll, std::uint64_t a_offset);
}
