// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Script-call floods: one script's event queued thousands of times, faster than the script engine runs them
// (classic script lag), and the safe way to drop the calls that never started.
//
// What the save holds (Papyrus section): each queued call is a suspended stack table entry (thread id + the call:
// script, event, the object it is called on, the arguments) or a queued function message, and each names a thread.
// A thread with 0 frames has not started: no line of the event has run.
//
// Verified in SkyrimSE.exe 1.6.1170 (scratchpad skyf/exe, tools skyf/xdis.py, fn.py, calls.py, xref.py):
// - Stack::Load 0x14337b0 reads the thread's first two bytes into Stack+0x68 (state, must be < 9) and Stack+0x6c
//   (freeze state, < 3), then the return value (+0x70), a flag byte (bit 0: a u32 "next stack" id looked up into
//   +0x98) and the stack type (+0x84, < 3). So ActiveScriptData::major is the stack's state and minor its freeze state.
// - The VM's update 0x144f8a0 swaps the two suspend queues (VM+0x92f8 / 0x9308 and their overflow arrays 0x9300 /
//   0x9310), pops one entry at a time (0x14509c0: {stack, call}, FIFO when the VM is overstressed), finds the stack by
//   its id (Stack+0x80) and, for state 2, 4 or 8 (waiting on memory, waiting to make its call, retry call: jump table
//   at 0x144fec0), makes the call in 0x144e370: IFuncCallQuery::GetFunctionCallInfo (vfunc 1: object, function,
//   arguments), the object's checks, then pushes the function's frame and runs it. When the object is not ready (not
//   constructed/initialized, Object+0 bits) or its lock is held by another stack, the entry goes back into the
//   suspend queue (0x14507b0) with state 4. A saved 0-frame thread with state 4 is therefore an event call that has
//   not run one line yet; dropping it (entry + thread) is the same as that event never having fired.
// - Object::Load 0x1422890: the instance's saved flag byte is constructed (1), valid (2), lock holder saved (4: a u32
//   stack id into Object+0x18), initialized (8), lock bit (0x10); unknown1 is the number of properties still to be
//   initialized (<< 3 into Object+0). Nothing in an object counts or lists the stacks waiting to call it, so the
//   script instance and its object stay exactly as they are.
// - The game's own warning: the SkyrimVM StatsEvent sink 0x9c9070 (SkyrimVM+0x198) compares suspendedStacksCount
//   (StatsEvent+4) with 100; over it, it sets the VM overstressed (VM vfunc 6, VM+0x827C) and, at most once a minute,
//   logs "Suspended stack count is over our warning threshold, dumping stacks:" and dumps them.
// - Loading: the stack table 0x1454140 registers every thread; each suspended entry is read by 0x1455fd0 (u32 id,
//   lookup, u8 has-call, the call 0x1471ba0) and pushed into its queue; an entry whose thread is missing is dropped
//   ("Failed to read suspended stack from save game") and its stack destroyed. The VM pauses (needed to save) when
//   suspend queues + queued messages + latent waits + the extra slot >= running stacks (0x14517c0, see Ops.h
//   StrandedThreads): removing a 0-frame thread together with its one suspended entry lowers both sides by one.
//
// Fallout 4 (re/Fallout4.exe): only the load side was checked: the suspended stack loader 0x21302b0 reads the same
// thing (u32 stack id, lookup, u8 has-call, the call 0x215ac80, queue push; "Failed to read suspended stack from save
// game" otherwise). Its resume path and stack state values were NOT checked, and no Fallout 4 save we have has a
// suspended stack. Skyrim LE (32-bit ids, same layout) was not checked in its exe either.
#pragma once

#include "Save.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace ess::flood
{
	// One script.event with more queued calls than this is a flood. It is the game's own warning line for ALL
	// suspended stacks together (SkyrimVM 0x9c9070: count > 100), here for one script.event alone. On our normal saves
	// (Tony's, trashgod's, Save140, the Fallout 4 saves) the most a single script.event has is 50 (50 different
	// objects' OnCellAttach after a cell load); Zordrac's has 13303, 1281 and 402.
	inline constexpr std::size_t kOver = 100;

	struct Target  // the object the calls are made on (the receiver of the event)
	{
		EID           object = 0;
		std::size_t   calls = 0;
		bool          found = false;    // the object's instance is in the save
		std::string   script;           // the instance's script
		std::uint32_t form = 0;         // the form it is on (quest for an alias, actor for an active effect)
		std::string   plugin;           // the plugin of that form ("Created" for FF forms)
		bool          alias = false;    // an alias's script (the script extends an Alias type): aliasID is the alias
		std::uint16_t aliasID = 0;      // the alias id (checked against Skyrim.esm VMAD alias ids)
		bool          effect = false;   // an ActiveMagicEffect's script: form is the actor
	};

	struct Flood
	{
		std::string script, event;  // as the queued calls name them
		std::size_t suspended = 0;   // suspended stack entries
		std::size_t messages = 0;    // queued function messages (counted, never cleared: not checked in the exe)
		std::size_t notStarted = 0;  // suspended entries whose thread has 0 frames: what Clear removes
		std::size_t running = 0;     // entries whose thread already runs (frames > 0): left alone
		std::size_t other = 0;       // 0-frame entries left for another reason (see Clearable): left alone
		std::size_t runningNow = 0;  // threads (not queued calls) already inside this event: frame 0 is Script.Event
		std::size_t objects = 0;     // how many different objects the calls are made on
		std::vector<Target> targets; // the busiest objects first (up to 5)
		std::size_t total() const { return suspended + messages; }
		std::string key() const { return script + "." + event; }
	};

	// Every script.event with more than a_over queued calls, the biggest first.
	std::vector<Flood> Find(const SaveFile& a_save, std::size_t a_over = kOver);

	// A suspended entry counts as not started (notStarted, cleared) when its call never started and nothing but this
	// one entry holds its thread: the thread exists with no frames, is not a latent wait (type 0), carries no
	// quest/scene/terminal fragment callback, names no next stack, is in a state the resume path turns into the call
	// (2, 4, 8), and is not named by a queued function message, another suspended entry, the extra slot, another
	// thread's next-stack id or an object's lock holder. Anything else with 0 frames is counted in other.
	struct ClearResult
	{
		std::size_t calls = 0, threads = 0, messages = 0;  // what was removed
		std::size_t leftRunning = 0, leftOther = 0;         // entries of those floods left alone
		std::vector<std::string> floods;                    // the script.event keys cleared
	};
	// Removes the not-started calls of the floods (a_keys: "Script.Event", case-insensitive; empty = every flood)
	// together with their threads and everything that names those threads (ops::RemoveSuspendedStacks).
	ClearResult Clear(SaveFile& a_save, const std::set<std::string>& a_keys = {}, std::size_t a_over = kOver);

	// Writes the save's Papyrus section as File > Save would and reads it back (in memory): after a clear, the GUI
	// checks with this that it reads and that no thread is left stranded (ops::StrandedThreads).
	struct Reread
	{
		bool        ok = false;
		std::string error;
		std::size_t threads = 0, suspended = 0, messages = 0, stranded = 0;
	};
	Reread ReadBack(const SaveFile& a_save);

	// One plain line about a target ("alias 0 of quest 0D093567 (AX ValSerano.esp)").
	std::string TargetText(const Target& a_t);
}
