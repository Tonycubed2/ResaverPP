// ReSaver++ - Fallout 4 ACHR: the saved data of a running package (the AI process's package instances, read by
// 0xcea430 after the package's type byte; RVAs in Fallout4.exe 1.11.240).
//
// The type byte names the package data object the game makes for it (0xceac70 / 0xceaed0) and that object's
// LoadGame (vtable +0x30) reads the rest:
//   2  (0xde76a0, LoadGame 0xde7980): a form id
//   13 (0xdea8b0, 0xdeaf50): u32 x3, u8 x2, u32, vsval n x form id
//   14 (0xdea140, 0xdea3f0): u32 x3, a form id, 4 optional locations (0x231930), u8 x3, a target (0x7683e0)
//   16 (0x7634d0, 0x763530): nothing
//   18 (0xde6800, 0xde69f0): a procedure tree - u32, the package's data objects (0x701db0), the root state's class
//      name and that state; every state saves its children by class name
// Any other type makes no object and nothing is read.
//
// Class names are looked up in the game's procedure factory (0x7106f0; the state LoadGame is vtable +0x38, a
// package data object's +0x40) or the location factory (0x21c2b0, LoadGame +0xc8). A name that is not found makes
// no object, so nothing is read for it (the empty name). Each loader below follows the function named next to it;
// the whole was checked read by read against the game's own loaders run on real saves (tools/fo4emu).
#include "ChangeFormFO4In.h"

#include <cstdio>

namespace ess::fo4cf::detail
{
	namespace
	{
		void U(In& b, int n) { for (int i = 0; i < n; ++i) b.u32(); }

		// a location from the location factory (LoadGame +0xc8)
		void Loc(In& b, const std::string& a_name)
		{
			if (a_name == "LocRef") {  // 21f570
				b.ref();
				U(b, 3);
				b.u8();
			} else if (a_name == "LocPt") {  // 21ce90
				b.raw(12);
				b.ref();
				b.ref();
				U(b, 2);
				b.u8();
				b.u8();
			} else if (a_name == "Prim") {  // 21dc20
				b.ref();
				b.u8();
			} else if (a_name == "LocCell") {  // 21c570
				b.ref();
				b.raw(12);
			} else {
				b.fail("package location class '" + a_name + "'");
			}
		}

		// 0x231930 (and the same code inlined in many states): a flag, then a location by class name
		void LocIf(In& b)
		{
			if (b.bln()) {
				Loc(b, b.wstrS());
			}
		}

		// 0x643af0: u32 kind, then a form id (kind 0) or a u32
		void Topic(In& b)
		{
			if (b.u32() == 0) b.ref(); else b.u32();
		}

		// a package data object from the procedure factory (LoadGame +0x40), 0x701db0's list
		void DataObject(In& b, const std::string& a_name)
		{
			if (a_name == "Bool") {  // 7034c0
				b.u8();
			} else if (a_name == "Float" || a_name == "Int") {  // 7035c0, 703660
				b.u32();
			} else if (a_name == "RefOLD" || a_name == "TargetSelector") {  // 703a20, 704260: nothing
			} else {
				b.fail("package data class '" + a_name + "'");
			}
		}

		void State(In& b, const std::string& a_name);

		void Child(In& b) { State(b, b.wstrS()); }  // 0x74aa20 (TreeOneChild): a child by class name

		// type 13 (0xdeaf50), also Patrol's own
		void Type13(In& b)
		{
			U(b, 3);
			b.u8();
			b.u8();
			b.u32();
			for (auto n = b.vs(); n; --n) b.ref();
		}

		// the procedure states (BGSProcedure<name>ExecState, LoadGame = vtable +0x38)
		void State(In& b, const std::string& a_name)
		{
			const auto& s = a_name;
			if (s.empty() || s == "Done") {  // no object / 221060
				return;
			}
			if (s == "OneChild") {  // 74aa20
				Child(b);
			} else if (s == "Sequence" || s == "Stacked") {  // 74ae30, 74b170
				Child(b);
				b.u32();
			} else if (s == "Random") {  // 74c3b0
				Child(b);
				b.u32();
				for (auto n = b.vs(); n; --n) b.u32();
			} else if (s == "Simultaneous") {  // 74bca0
				for (auto n = b.vs(); n; --n) {
					b.u32();
					Child(b);
				}
			} else if (s == "Headtrack") {  // 220740
				b.u32();
				b.u8();
			} else if (s == "PlayIdle") {  // 221e60
				b.ref();
				b.ref();
				b.ref();
			} else if (s == "Range") {  // 2273d0
				PathPoint(b);
				Pid68(b);
				Pid68(b);
				U(b, 4);
				b.raw(12);
				b.u32();
				b.u8();
				if (b.ver < 0x0e) b.u32();
				b.ref();  // 22b640
				b.u8();
				b.u8();
			} else if (s == "StayAway") {  // 22fab0
				U(b, 3);
				for (auto n = b.vs(); n; --n) b.ref();
				b.u8();
				b.u8();
				Pid68(b);
			} else if (s == "Acquire") {  // 713580
				b.u32();
				b.ref();
				b.u32();
				b.ref();
				if (b.bln()) State(b, "Activate");
			} else if (s == "Activate") {  // 714670
				b.u8();
				b.ref();
				LocIf(b);
				U(b, 2);
				for (int i = 0; i < 4; ++i) b.u8();
				Pid68(b);
				if (b.bln()) {
					b.ref();  // 21e390
					b.ref();
					b.u32();
				}
			} else if (s == "Dialogue") {  // 718350
				b.u32();
				b.ref();  // 22b640
				b.ref();
				Topic(b);
				b.u8();
			} else if (s == "DialogueActivate") {  // 719a20
				LocIf(b);
				LocIf(b);
				b.ref();
				Topic(b);
				if (b.bln()) State(b, "Dialogue");
				b.u8();
				Pid68(b);
			} else if (s == "Eat") {  // 71b160
				b.ref();
				b.u32();
				b.u8();
				b.u8();
			} else if (s == "Escort") {  // 71edb0
				LocIf(b);
				for (auto n = b.vs(); n; --n) b.ref();
				b.u8();
				b.u8();
				Pid68(b);
				U(b, 2);
				b.u8();
				if (b.ver >= 0x25) b.u8();
				b.u8();
				U(b, 2);
			} else if (s == "Find") {  // 720520
				LocIf(b);
				b.u32();
			} else if (s == "Flee") {  // 722500
				LocIf(b);
				if (b.bln()) State(b, "Travel");
				b.raw(12);
				U(b, 5);
				for (auto n = b.vs(); n; --n) b.ref();  // 21e390
				b.ref();
				b.ref();
				b.u8();
				b.u8();
				Pid68(b);
			} else if (s == "FlightGrab") {  // 7260c0
				b.ref();
				Pid68(b);
				b.u8();
				b.u8();
			} else if (s == "Follow") {  // 727e80
				b.ref();
				LocIf(b);
				LocIf(b);
				LocIf(b);
				U(b, 3);
				Pid68(b);
				for (int i = 0; i < 5; ++i) b.u8();
				Pid68(b);
				Pid70(b);
				b.u32();
				b.u8();
			} else if (s == "Guard") {  // 72dde0: two flags first, then their locations
				const bool a = b.bln();
				const bool c = b.bln();
				if (a) Loc(b, b.wstrS());
				if (c) Loc(b, b.wstrS());
				LocIf(b);
				LocIf(b);
			} else if (s == "HoldPosition") {  // 72ee70
				LocIf(b);
				LocIf(b);
				LocIf(b);
			} else if (s == "Hover") {  // 730580
				LocIf(b);
				b.u8();
			} else if (s == "KeepAnEyeOn") {  // 7312d0
				LocIf(b);
				LocIf(b);
			} else if (s == "LockUnlock") {  // 7322e0
				LocIf(b);
				b.u8();
				b.u8();
			} else if (s == "Orbit") {  // 733d30
				b.u8();
				LocIf(b);
				b.u8();
			} else if (s == "Patrol") {  // 739220
				b.u32();
				Type13(b);  // its own 0xdea8b0 object, LoadGame 0xdeaf50
				if (b.bln()) State(b, "UseIdleMarker");
				if (b.bln()) State(b, "SitSleep");
				Pid68(b);
				b.u8();
				b.u8();
				b.u8();
				Pid68(b);
				Pid70(b);
				Pid68(b);
				Pid68(b);
			} else if (s == "Sandbox") {  // 740a20
				U(b, 4);
				b.u8();
				U(b, 3);  // d4c830
				for (int i = 0; i < 4; ++i) b.u8();
				b.u32();
				b.u8();
				U(b, 5);
				b.raw(7);
				b.ref();  // 21e390
				b.ref();
				for (auto n = b.vs(); n; --n) {
					U(b, 4);
					b.ref();
				}
				LocIf(b);
				Child(b);
				b.u32();
				Pid68(b);
				b.u32();
				b.u8();
				b.u8();
				b.u8();
				U(b, 2);
				for (auto n = b.vs(); n; --n) b.u32();
			} else if (s == "Say") {  // 743f00
				b.u8();
			} else if (s == "SitSleep") {  // 745860
				b.u8();
				b.u8();
				if (b.ver >= 0x29) b.u8();
				if (b.bln()) State(b, "Activate");
			} else if (s == "Travel") {  // 747650
				b.u8();
				b.u8();
				b.u8();
				if (b.bln()) Loc(b, b.wstrS());
				b.u8();
				Pid68(b);
				if (b.ver >= 0x2d) b.u8();
			} else if (s == "UseIdleMarker") {  // 74d740
				b.u8();
				b.ref();
				b.ref();
				U(b, 2);
				Pid68(b);
				b.u8();
			} else if (s == "UseMagic") {  // 750890
				LocIf(b);
				LocIf(b);
				if (b.bln()) State(b, "Travel");
				b.ref();
				U(b, 2);
				b.ref();  // 21e390
				b.ref();
				b.u16();
				U(b, 4);
			} else if (s == "UseWeapon") {  // 754b10
				LocIf(b);
				LocIf(b);
				if (b.bln()) State(b, "Travel");
				b.ref();
				b.u32();
				b.ref();  // 21e390
				b.ref();
				b.u16();
				b.u16();
				b.u16();
				b.u32();
				b.u8();
				b.u8();
			} else if (s == "Wait") {  // 755d40
				b.u32();
			} else if (s == "Wander") {  // 7567f0
				LocIf(b);
				b.u32();
				b.u8();
				b.u32();
				Pid68(b);
			} else {
				b.fail("procedure state '" + s + "'");
			}
		}

		// 0x7683e0: a target - u8 kind, u32, then by kind a form id (0, 1, 3, 7), a u32 (2, 4, 5, 8) or nothing
		void Target(In& b)
		{
			const auto kind = b.u8();
			b.u32();
			switch (kind) {
			case 0: case 1: case 3: case 7: b.ref(); break;
			case 2: case 4: case 5: case 8: b.u32(); break;
			default: break;
			}
		}
	}

	void PackageData(In& b, std::uint8_t a_type)
	{
		switch (a_type) {
		case 2:  // de7980
			b.ref();
			return;
		case 13:
			Type13(b);
			return;
		case 14:  // dea3f0
			U(b, 3);
			b.ref();
			for (int i = 0; i < 4; ++i) LocIf(b);
			b.u8();
			b.u8();
			if (b.bln()) Target(b);
			return;
		case 18:  // de69f0: the procedure tree
			b.u32();
			for (auto n = b.vs(); n; --n) {  // 701db0
				b.u8();
				DataObject(b, b.wstrS());
			}
			State(b, b.wstrS());
			return;
		default:  // 16 (763530) and the types without an object
			return;
		}
	}
}
