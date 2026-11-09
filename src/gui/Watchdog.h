// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Freeze detector. The window marks which part of a frame it is in (Section); a watchdog thread (main.cpp)
// writes to ReSaverPP-startup.log when no frame has finished for 5 seconds, naming that part and how far the
// background work is, and any part of a frame that took longer than a second. So a report of "it stops
// responding" comes with where.
//
// Inside a part, Step marks a named piece of work (nested: "pollScan > checkMissingPlugins > plugin files").
// The freeze report names the innermost step and the steps around it, and a step that takes longer than
// half a second is written to the log as "slow: ..." when it ends. RESAVERPP_TIMING=1 writes every step of
// 1 ms or more (for measuring).
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>

namespace watchdog
{
	void Section(const char* a_name);  // the part of the frame starting now (a string literal)
	void FrameDone();
	void Alive(const char* a_name);  // the loop runs but draws nothing (minimized): not a freeze
	void Log(const std::string& a_line);  // a line in ReSaverPP-startup.log (any thread)

	// a named step of the UI thread's work (a string literal), for the freeze report and the slow-step log
	class Step
	{
	public:
		explicit Step(const char* a_name);
		~Step();
		Step(const Step&) = delete;
		Step& operator=(const Step&) = delete;

	private:
		std::chrono::steady_clock::time_point _t0;
		int                                   _depth;
	};

	// time a piece of work on another thread: written as "slow (background): ..." when over a second, or with
	// RESAVERPP_TIMING=1 from 1 ms
	class Timed
	{
	public:
		explicit Timed(std::string a_name);
		~Timed();
		Timed(const Timed&) = delete;
		Timed& operator=(const Timed&) = delete;

	private:
		std::string                           _name;
		std::chrono::steady_clock::time_point _t0;
	};

	// background work, for the report
	inline std::atomic<std::size_t> g_scanDone{ 0 }, g_scanTotal{ 0 }, g_pathDone{ 0 }, g_pathTotal{ 0 };
	inline std::atomic<bool>        g_namesLoading{ false }, g_opening{ false };
}
