// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Freeze detector. The window marks which part of a frame it is in (Section); a watchdog thread (main.cpp)
// writes to ReSaverPP-startup.log when no frame has finished for 5 seconds, naming that part and how far the
// background work is, and any part of a frame that took longer than a second. So a report of "it stops
// responding" comes with where.
#pragma once

#include <atomic>
#include <cstddef>

namespace watchdog
{
	void Section(const char* a_name);  // the part of the frame starting now (a string literal)
	void FrameDone();
	// background work, for the report
	inline std::atomic<std::size_t> g_scanDone{ 0 }, g_scanTotal{ 0 }, g_pathDone{ 0 }, g_pathTotal{ 0 };
	inline std::atomic<bool>        g_namesLoading{ false };
}
