// ReSaver++ - Fallout 4 ACHR: the actor's movement controller (MovementControllerNPC::LoadGame 0xdc8e40, RVAs in
// Fallout4.exe 1.11.240). The controller reads through a reader object (vtable 0x2571060: +0x08 f32, +0x20 u32,
// +0x28 i32, +0x30 u16, +0x40 u8, +0x50 bool, +0x58/+0x60 form id, +0x00 string, +0x68/+0x70/+0x78 pointer ids,
// +0x98 the form version). Its arbiters (0xdcb240) and interfaces (0xdcafd0) are saved as a u8 code each; the code
// names a class registered with the controller's factories and that class's LoadGame reads the rest. Pathing
// objects are saved as a u32 class id followed by the object (factories at 0xdcc3d0, 0xdd93b0, 0x1df2ea0,
// 0x1e61d80, 0x1e93110, 0x1e21680). The class tables were read from the game's own registrations (its static
// initializers run in an emulator); each loader below follows the function named next to it, and the whole was
// checked read by read against the game's loader run on real saves.
#include "ChangeFormFO4In.h"

namespace ess::fo4cf::detail
{
	namespace
	{
		std::string Hex(std::uint32_t a_v)
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "0x%08X", a_v);
			return buf;
		}

		void F(In& b, int n) { for (int i = 0; i < n; ++i) b.f32(); }
		void U(In& b, int n) { for (int i = 0; i < n; ++i) b.u32(); }

		void PathRequest(In& b);  // dd93b0
		void Path(In& b);         // 1e00eb0
	}

	// dd85c0: a pointer id; the first time an id is seen the object follows (0xdd93b0)
	void Pid68(In& b)
	{
		const auto id = b.u32();
		if (id && b.noTable) b.fail("a pointer id outside an actor");
		if (id && b.ids68.insert(id).second) {
			PathRequest(b);
		}
	}
	// dd8700: the same for paths (0xdd9580: u32 class id, then the path 0x1e00eb0)
	void Pid70(In& b)
	{
		const auto id = b.u32();
		if (id && b.noTable) b.fail("a pointer id outside an actor");
		if (id && b.ids70.insert(id).second) {
			if (b.u32() != 0) {
				Path(b);
			}
		}
	}

	namespace
	{
		// dd8850: only the id
		void Pid78(In& b)
		{
			const auto id = b.u32();
			if (id && b.noTable) b.fail("a pointer id outside an actor");
			if (id) b.ids78.insert(id);
		}

		// 0x79e430 (class 0xa5e9a03c): a form id, then a u32 when it was set, else a second form id
		void Obj1df2(In& b)  // 1df2ea0
		{
			const auto id = b.u32();
			if (!id) return;
			if (id != 0xA5E9A03Cu) b.fail("path point object class " + Hex(id));
			if (!b.ref().isZero()) b.u32(); else b.ref();
		}

	}

	// 1df23d0: a path point
	void PathPoint(In& b)
	{
		F(b, 3);
		Obj1df2(b);
		b.ref();
		b.u16();
		b.u8();
		b.u8();
	}

	namespace
	{

		void Obj1e61(In& b)  // 1e61d80
		{
			const auto id = b.u32();
			if (!id) return;
			if (id != 0xE48B73F3u) b.fail("object class " + Hex(id) + " (0x1e61d80)");
			b.ref();  // 79f270
		}

		void Obj1e93(In& b)  // 1e93110
		{
			const auto id = b.u32();
			if (!id) return;
			if (id != 0x5826A5DDu && id != 0xD55D0153u) b.fail("object class " + Hex(id) + " (0x1e93110)");
			if (b.bln()) {  // 7a13f0
				b.ref();
				b.u8();
			}
		}

		void Obj1e21(In& b)  // 1e21680
		{
			const auto id = b.u32();
			if (!id) return;
			if (id != 0x0C28D1C5u && id != 0xBCDCF728u) b.fail("object class " + Hex(id) + " (0x1e21680)");
			F(b, 7);  // 22daa0 / 982eb0
		}

		void Tween(In& b)  // 1e81320
		{
			b.u32();
			F(b, 30);
			F(b, 5);
		}

		void Motion(In& b)  // 1e7f0c0
		{
			b.u32();
			F(b, 6);
		}

		// dcc3d0: an object by u32 class id, its LoadGame (vtable +0x20)
		void ClassObj(In& b)
		{
			const auto id = b.u32();
			switch (id) {
			case 0:
			case 0x0000001Du:  // dd4920
			case 0x917E8572u:  // 100b270
			case 0xC8B4153Eu:  // 1e80cd0
				return;
			case 0x5CC2A237u:  // 1e7f840
				if (b.bln()) Obj1e61(b);
				return;
			case 0x67DA9023u:  // 1e802e0
				PathPoint(b);
				F(b, 3);
				return;
			case 0x8BCEF6C4u:  // 1e99600
			case 0xD7578F99u:
				Pid68(b);
				return;
			case 0x3BED430Bu:  // 1e4b430
				b.u32();
				b.f32();
				return;
			case 0x616653D5u:  // 1e82600
				b.u8();
				return;
			case 0x119563E6u:  // 1e822d0
				b.ref();
				Tween(b);
				Pid78(b);
				b.wstr();
				return;
			case 0x00000013u:  // 1e82950
				b.f32();
				if (b.ver >= 0x13) b.u8();
				return;
			case 0xCDED4F63u:  // 1e996c0
				Pid68(b);
				Pid70(b);
				return;
			case 0x3CF364ECu:  // 1e80700: u32 n, n elements of a class not decoded
				if (b.u32() != 0) b.fail("a 0x3CF364EC list");
				return;
			default:
				b.fail("object class " + Hex(id) + " (0xdcc3d0)");
			}
		}

		// 1e00eb0: a path
		void Path(In& b)
		{
			for (auto n = b.u32(); n; --n) {  // 1e4ff00
				PathPoint(b);
				ClassObj(b);
			}
			b.u32();
			b.u32();
			for (auto n = b.u32(); n; --n) {  // 1e28810
				PathPoint(b);
				F(b, 3);
				ClassObj(b);
			}
			b.f32();
			b.u8();
			b.u8();
			for (auto n = b.u32(); n; --n) {
				PathPoint(b);
				F(b, 3);
				ClassObj(b);
			}
		}

		void Request(In& b)  // 1e20970: the pathing request base
		{
			PathPoint(b);  // 1ea4eb0
			b.f32();
			PathPoint(b);  // 1e56860
			F(b, 7);
			{  // 1ea50a0
				for (auto n = b.u32(); n; --n) {  // 1e69bd0
					F(b, 8);
					b.u32();
				}
				F(b, 4);  // 1ead8e0
			}
			F(b, 2);  // 1ea52a0
			b.u16();
			b.u16();
			F(b, 3);  // 1ea4f90
			b.u32();
			b.u8();
			Obj1e93(b);
			b.ref();
			Obj1e21(b);
			for (auto n = b.u32(); n; --n) b.ref();  // 1ea5770
			b.u32();
			for (auto n = b.u32(); n; --n) {  // 1ea4eb0
				PathPoint(b);
				b.f32();
			}
			b.u8();
			b.u8();
			b.u8();
		}

		void RequestActor(In& b)  // 7b30c0
		{
			Request(b);
			if (b.bln()) b.ref();
			if (b.bln()) b.ref();
			b.f32();
			b.u8();
			b.f32();
			b.f32();
		}

		// ed5cf0: an object pointer (ChangeFormFO4Combat.cpp)
		void HkPtr(In& b) { Object(b); }

		// fe7680: the Havok pathing requests' common part (their LoadGame +0x88, reached through 0xfe74a0 when the
		// reader is the save game's): the request (1e20970), a form id, 4 x u32 (884260 and three more), a Havok
		// object, a path (pointer id +0x70), another Havok object
		void HkRequest(In& b)
		{
			Request(b);
			b.ref();
			U(b, 4);
			HkPtr(b);
			Pid70(b);
			HkPtr(b);
		}

		// fe63f0: a pointer id (+0x68) and 5 x u8
		void HkTail(In& b)
		{
			Pid68(b);
			for (int i = 0; i < 5; ++i) b.u8();
		}

		// dd93b0: a pathing request by u32 class id, its LoadGame (vtable +0x20)
		void PathRequest(In& b)
		{
			const auto id = b.u32();
			switch (id) {
			case 0:
				return;
			case 0xC702BB5Bu: case 0x8B2152AFu: case 0x0618E573u: case 0x330EB0E3u:  // 7a1d80
			case 0x0534ED31u:                                                        // 1e20970
			case 0x6AF11190u:                                                        // 3ea360
			case 0x4773B11Du:                                                        // 7b6b80
				Request(b);
				return;
			case 0x3C5FF134u: case 0x98C4C679u:  // 7b30c0, 7b53d0
				RequestActor(b);
				return;
			case 0xCA622528u:  // 7b6540
				Request(b);
				F(b, 4);
				return;
			case 0xA5021751u:  // 7b3850
				RequestActor(b);
				b.u32();
				if (b.bln()) b.ref();
				F(b, 3);
				if (b.bln()) b.ref();
				if (b.bln()) b.ref();
				if (b.bln()) b.ref();
				b.u8();
				b.u32();
				return;
			case 0xF075EEF7u:  // 7b4bf0
				RequestActor(b);
				F(b, 3);
				b.u32();
				b.f32();
				if (b.ver >= 0x38) {
					b.f32();
					b.u16();
				}
				b.u8();
				b.ref();
				return;
			case 0x8353103Bu:  // 7b3ff0
				RequestActor(b);
				F(b, 3);
				for (auto n = b.u32(); n; --n) {  // 1e56860
					PathPoint(b);
					F(b, 7);
				}
				return;
			case 0xCDF9A2FCu:  // 7b50c0
				RequestActor(b);
				b.f32();
				b.u32();
				return;
			case 0xEB5ED874u:  // 7b05f0
				Request(b);
				F(b, 2);
				b.u8();
				b.u8();
				PathPoint(b);
				b.u8();
				b.u8();
				return;
			case 0xFA2763CEu:  // 7b2b30
				Request(b);
				b.f32();
				return;
			case 0xF31543ABu:  // 7b0da0
				Request(b);
				for (auto n = b.u32(); n; --n) PathPoint(b);
				b.u32();
				return;
			case 0x54DACA55u:  // 7b5d80
				Request(b);
				for (auto n = b.u32(); n; --n) F(b, 3);
				F(b, 5);
				return;
			case 0x0528E757u:  // 7b5790
				Request(b);
				PathPoint(b);
				b.f32();
				return;
			// the classes below load through 0xfe74a0, which hands the save buffer to their LoadGame (+0x88)
			case 0xB5AE3E8u:  // fe7680
				HkRequest(b);
				return;
			case 0x3A161216u: case 0xA52656EFu:  // e7af70, edca10
				HkRequest(b);
				HkPtr(b);
				HkTail(b);
				return;
			case 0x6C1CDFDEu: case 0x78B59DB4u:  // f62ba0, f32200 (1009c00: 12 bytes, u32)
				HkRequest(b);
				b.raw(12);
				b.u32();
				HkTail(b);
				return;
			case 0x78B25F95u:  // f527c0
				HkRequest(b);
				b.raw(12);
				b.raw(12);
				b.u32();
				HkTail(b);
				return;
			case 0x2AE22001u:  // 8509b0
				HkRequest(b);
				b.raw(12);
				b.raw(12);
				b.u32();
				HkTail(b);
				b.u32();
				return;
			case 0xC4728FFBu:  // f856f0
				HkRequest(b);
				b.raw(12);
				b.u8();
				HkTail(b);
				return;
			case 0xCCFD0C27u:  // f1bc80
				HkRequest(b);
				U(b, 2);
				HkTail(b);
				return;
			case 0x984100E6u:  // fad960
				HkRequest(b);
				b.ref();  // 22b640
				b.raw(12);
				HkTail(b);
				return;
			case 0xBA27000Fu:  // fe78c0
				HkRequest(b);
				Pid68(b);
				return;
			default:
				// 0x0221C439, 0x0515204A, 0x4D62DD4A, 0x6B11480F, 0x7210E996, 0x83547F1A, 0x95AC34AF, 0x965E9BAE,
				// 0xA4CE4D05, 0xEE6B8D42, 0xF3ED1E11 (0xfe74a0 classes with parts of their own) and unknown ids
				b.fail("pathing request class " + Hex(id));
			}
		}

		// 0xdcb240: the arbiters (codes from 0x1e83d40)
		void Arbiter(In& b, std::uint8_t a_code)
		{
			switch (a_code) {
			case 0:  // Handler Arbiter 1e411a0
				return;
			case 1:  // Path Manager Arbiter 1e53bf0
				b.u32();
				Pid68(b);
				Pid70(b);
				return;
			case 2:  // Planner Arbiter 1e84d80
				b.u8();
				b.f32();
				return;
			case 3:  // Post Update Arbiter 1e83510
				F(b, 14);
				if (b.bln()) F(b, 3);
				return;
			case 4:  // Tweener Arbiter 1e4c080
				Tween(b);
				F(b, 10);
				return;
			default:
				b.fail("movement arbiter code " + std::to_string(a_code));
			}
		}

		// 0xdcafd0: the interfaces (codes from 0x1e40070)
		void Interface(In& b, std::uint8_t a_code)
		{
			switch (a_code) {
			case 0x00: case 0x01: case 0x03: case 0x04: case 0x05: case 0x07: case 0x08:  // 1e40010: nothing
			case 0x84: case 0x85: case 0x87: case 0x88:
			case 0x8D:  // Node Follower dd8130
				return;
			case 0x0A:  // Actor State 1e56080
				Motion(b);
				F(b, 4);
				U(b, 2);
				return;
			case 0x0B:  // Actor Avoider 1e8c840
				Pid68(b);
				for (auto n = b.u32(); n; --n) {  // 1e8dd80
					b.ref();
					F(b, 12);
					U(b, 2);  // 1e83b40
					b.u8();
				}
				U(b, 2);  // 1e83b40
				b.u32();
				return;
			case 0x0C:  // Path Follower Flight 1e88db0
				b.u32();
				F(b, 2);
				Pid68(b);
				Pid70(b);
				b.f32();
				U(b, 2);
				for (auto n = b.u32(); n; --n) F(b, 3);
				b.u32();
				Motion(b);
				return;
			case 0x0D:  // Path Follower 1e5ae10
				b.u32();
				Pid68(b);
				Pid70(b);
				F(b, 7);
				Pid78(b);
				F(b, 3);
				if (b.ver >= 0x1c) b.u8();
				U(b, 2);
				F(b, 6);
				if (b.ver >= 0x16 && b.bln()) {  // 1ea9760
					Tween(b);
					b.wstr();  // 1e49bb0
					b.u32();
					b.f32();
					b.u8();
					b.u8();
				}
				b.f32();  // +0x128 1e56fb0
				b.f32();  // +0x138 1e57230 (+0x148 1ea9620 reads nothing)
				if (b.bln()) b.ref();  // +0x50 1ea9c60
				b.u32();
				b.f32();  // +0x90 1eaa900
				if (b.ver >= 0x1c) b.u8();
				b.f32();
				b.u32();
				b.u8();
				b.u8();
				F(b, 3);
				Motion(b);  // +0x68 1eac360
				F(b, 4);    // +0xd0 1eab2e0
				b.u32();
				U(b, 2);  // +0x100 1eab8b0
				b.u8();
				return;
			case 0x0E:  // Virtual Path Follower 1e8ade0
				b.u32();
				Pid68(b);
				Pid70(b);
				return;
			case 0x0F:  // NavMesh Bounds 1e90370
				b.u32();
				F(b, 2);
				for (auto n = b.u32(); n; --n) {  // 1e91280 / 1ea6f20
					F(b, 6);
					for (auto m = b.u32(); m; --m) F(b, 3);
					b.u32();
				}
				Pid70(b);
				b.f32();
				return;
			case 0x10:  // Avoid Box 1e91f40 (its +0x18 member 1e52a00 reads nothing)
				b.u32();
				b.f32();
				PathPoint(b);  // 1ead830
				F(b, 3);
				b.u8();
				Obj1e61(b);
				Obj1e93(b);
				return;
			case 0x11:  // Loaded Area Monitor 1e876b0
				if (b.ver >= 0x0b) Pid68(b);
				Pid70(b);
				return;
			case 0x12:  // Static Avoider 1e7b880
				b.f32();  // 1eacd10
				for (auto n = b.u32(); n; --n) F(b, 13);  // 1ead5d0
				F(b, 5);
				for (auto n = b.u32(); n; --n) {  // 1e69bd0
					F(b, 8);
					b.u32();
				}
				Pid68(b);
				F(b, 2);
				b.u32();
				return;
			case 0x13:  // Fixed Delta 1e4d560
				F(b, 24);
				return;
			case 0x14:  // Target Pos and Vel 1e95e40
				F(b, 22);
				b.u8();
				F(b, 2);
				return;
			case 0x15:  // Store Planner Output 1e96940
				F(b, 7);
				return;
			case 0x16:  // Standard Handler Agent 1e981e0
				U(b, 4);  // 1e48970
				F(b, 6);
				b.u32();
				F(b, 3);
				if (b.ver >= 9) b.u8();
				return;
			case 0x80:  // 3e7790
				b.u32();
				b.f32();
				if (b.ver < 0x21) b.u8();
				if (b.ver < 2) b.u8();
				return;
			case 0x82:  // 3e5990
				b.u32();
				F(b, 6);
				return;
			case 0x83:  // 3e64a0
				b.u32();
				return;
			case 0x86:  // Sprint Action Trigger dd3f70
				b.u32();
				b.u8();
				if (b.ver < 0x24) b.u8();
				b.u8();
				return;
			case 0x8B:  // FailSafe Warp dd6e10
				PathPoint(b);
				PathPoint(b);
				b.f32();
				b.ref();
				U(b, 2);
				return;
			default:
				b.fail("movement interface code " + std::to_string(a_code));
			}
		}
	}

	void MovementController(In& b)
	{
		for (auto n = b.u32(); n; --n) ClassObj(b);  // dcc700 / dcc3d0
		for (auto n = b.u32(); n; --n) {               // dcc5a0
			PathPoint(b);
			F(b, 3);
			b.u32();
		}
		b.u32();  // +0x198
		for (int i = 0; i < 7; ++i) b.u8();
		for (auto n = b.u32(); n; --n) Arbiter(b, b.u8());    // dcb240
		for (auto n = b.u32(); n; --n) Interface(b, b.u8());  // dcafd0
		if (b.bln() && b.bln()) {  // a 0x119563E6 object (1e820b0, LoadGame 1e822d0)
			b.ref();
			Tween(b);
			Pid78(b);
			b.wstr();
		}
		if (b.ver >= 2 && b.ver < 4) b.u8();
	}
}
