// ReSaver++ - Fallout 4 player (ACHR 0x14): the PlayerCharacter part after Actor::LoadGame, following
// PlayerCharacter::LoadGame 0xd65bd0 (vtable slot 0x12) from its call of Actor::LoadGame (0xd65c12) to its end
// (0xd68175), RVAs in Fallout4.exe 1.11.240. Every read below is a read the game makes, in its order and with its size;
// "ver" is the save's form version (the load buffer's vtable +8). The only branches are on the form version, on
// bytes and counts read here, and the knowledge entries' tag (0xd2faa0); where the game looks a form up (0x311b80) it
// reads the same bytes whether the form is found or not (checked in the code at each lookup: 0xd6647b, 0xd668af,
// 0x61e1da, 0x3ff18c). Checked read for read against the game's loader run in the emulator (tools/fo4emu).
#include "ChangeFormData.h"
#include "ChangeFormFO4.h"
#include "ChangeFormFO4In.h"

namespace ess::fo4cf
{
	namespace
	{
		using detail::In;

		void One4(In& b) { b.u32(); }  // 0xd4c830

		// PlayerCamera::LoadGame (0x102aac0) with its camera states' LoadGame (vtable +0x78). The states are the ones
		// PlayerCamera's constructor (0x1024f80) makes, loaded in this order: +0xe0 0x10218c0, +0x120 0x102e5e0,
		// +0xe8 / +0xf0 nothing (0x3738b0), +0xf8 0x1022970, +0x100 nothing, +0x128 0x1023a70 (-> 0x102e5e0),
		// +0x110 nothing, +0x130 0x1024430, +0x108 nothing, +0x138 / +0x118 0x102e5e0, +0x140 0x830a70.
		void CameraState102e5e0(In& b)
		{
			b.raw(12); b.u32(); b.raw(8); b.u8(); b.u32();
			b.wstr();  // 0xc0fb90
			b.u8();
		}
		void Camera(In& b)
		{
			b.u8(); b.u8(); b.u32(); b.u32(); b.ref(); b.u32();
			// +0xe0: 0x10218c0
			for (int i = 0; i < 4; ++i) b.u32();
			b.u8(); b.u8(); b.u8();
			CameraState102e5e0(b);  // +0x120
			// +0xf8: 0x1022970 (both of its branches, on 0x102b5c0, read these sizes)
			b.raw(12); b.raw(8); b.raw(8); b.u16(); b.u8(); b.u8();
			CameraState102e5e0(b);  // +0x128 (0x1023a70)
			// +0x130: 0x1024430
			CameraState102e5e0(b);
			b.u8(); b.ref(); b.u32();
			CameraState102e5e0(b);  // +0x138
			CameraState102e5e0(b);  // +0x118
			// +0x140: 0x830a70
			CameraState102e5e0(b);
			if (b.ver >= 0x26) b.u32();
			b.u32();  // the current state
			for (auto n = b.u32(); n; --n) b.u32();
			b.u32(); b.u32(); b.u8();
		}
	}

	void detail::PlayerPart(In& b, const SaveFile& a_save, Actor& a)
	{
		auto&      r = b.r;
		const auto ver = b.ver;
		a.playerRange.begin = r.pos();
		auto part = [&](const char* a_name, auto&& a_read) {
			const auto begin = r.pos();
			a_read();
			a.playerParts.push_back({ a_name, { begin, r.pos() } });
		};

		part("fields", [&] {  // 0xd65c31 .. 0xd66127
			b.u8(); b.u32(); b.u8(); b.u8(); b.u32(); b.u32(); b.u8(); b.u32(); b.u8();
			if (ver < 1) b.u8();
			b.u32(); b.raw(12); b.u32(); b.u32(); b.u8(); b.u8(); b.raw(8); b.u8();
			b.u32(); b.u32(); b.u32(); b.u8();
			for (int i = 0; i < 7; ++i) b.u32();  // +0xc18, +0xcb4, +0xcb8, +0xa08 .. +0xa14
			One4(b);                              // +0xa18
			b.u8(); b.u8(); b.u32();
			if (ver < 1) b.u32();
			for (int i = 0; i < 7; ++i) b.u32();  // +0xbe0
			b.raw(12);                            // 0xc0ffb0, one 12-byte element (+0x710)
			b.raw(12);                            // +0x72c
			One4(b);                              // +0xcd0
		});
		part("list +0x750", [&] {  // 0xd6612a: vsval n x (12 bytes, 0xd4c830)
			for (auto n = b.vs(); n; --n) { b.raw(12); One4(b); }
		});
		part("block +0x948", [&] {  // 0xd66237: u8; set: ref, 12, 12, ref, ref, 4 x u8
			if (b.u8()) {
				b.ref(); b.raw(12); b.raw(12); b.ref(); b.ref();
				b.u8(); b.u8(); b.u8(); b.u8();
			}
		});
		part("refs", [&] {
			b.ref();  // 0xd6643a (+0xbd8)
			b.ref();  // 0xd66476: a form found or not, no reads either way
			b.ref(); b.ref();  // 0xda6cf0
			b.ref(); b.ref();  // +0x9f8, +0xa00
			b.ref(); b.ref();  // +0xb5c, 0xd66687
		});
		part("table +0xa64", [&] {  // 0xd66738: vsval n x (u32 index; index <= 14: u32, ref)
			for (auto n = b.vs(); n; --n) {
				if (b.u32() <= 0xe) {
					b.u32();
					b.ref();
				}
			}
		});
		a.questLogRange.begin = r.pos();
		part("quest log", [&] {  // 0xd6682a: vsval n x (quest, u16 stage, u8 log entry)
			for (auto n = b.vs(); n; --n) {
				QuestLogEntry e;
				e.range.begin = r.pos();
				e.quest = b.ref();
				e.formID = a_save.formIDOf(e.quest);
				e.stage = b.u16();
				e.log = b.u8();
				e.range.end = r.pos();
				a.questLog.push_back(e);
			}
		});
		a.questLogRange.end = a.objectivesRange.begin = r.pos();
		part("objectives", [&] {  // 0xd66961: vsval n x 0x61e170 (quest, u32 objective, u32, u32)
			for (auto n = b.vs(); n; --n) {
				ObjectiveEntry e;
				e.range.begin = r.pos();
				e.quest = b.ref();
				e.formID = a_save.formIDOf(e.quest);
				e.index = b.u32();
				e.instance = b.u32();
				e.state = b.u32();
				e.range.end = r.pos();
				a.objectives.push_back(e);
			}
		});
		a.objectivesRange.end = r.pos();
		part("fields 2", [&] { b.u32(); b.u32(); b.u32(); });  // 0xd66b0e, 0xd66b4b, 0xd66b60
		a.cameraRange.begin = r.pos();
		part("camera", [&] { Camera(b); });  // 0x102aac0
		a.cameraRange.end = r.pos();
		part("map +0x640", [&] {  // 0xd66b86: vsval n x (ref, 4 x u32)
			for (auto n = b.vs(); n; --n) { b.ref(); b.u32(); b.u32(); b.u32(); b.u32(); }
		});
		part("fields 3", [&] { b.ref(); b.u32(); });  // 0xd66c71, +0xcd4
		part("map +0x670", [&] {  // 0xd66ce1: vsval n x (ref, u32, u32)
			for (auto n = b.vs(); n; --n) { b.ref(); b.u32(); b.u32(); }
		});
		part("fields 4", [&] {
			b.u8();
			if (b.u8() != 0xFF) b.u16();  // 0xd66dd8
			b.ref(); b.raw(12);           // +0x8b0, +0x8b8
			if (ver >= 0x23) b.raw(12);   // 0xc0ffb0 (+0x9b0)
			b.u32();                      // +0xcd8
			for (int i = 0; i < 6; ++i) b.ref();  // 0xd66ed4 .. 0xd67070
		});
		part("list 0x3ff0f0", [&] {  // u32, u32 n, n x (u16, u32, u32): both of 0x3ff0f0's branches read these
			b.u32();
			for (auto n = b.u32(); n; --n) { b.u16(); b.u32(); b.u32(); }
		});
		part("fields 5", [&] { b.ref(); b.u32(); b.ref(); });  // +0xd08, +0xa30, +0xa28
		part("map +0x6d0", [&] {  // 0xd671a9: vsval n x (ref, 4 x u16)
			for (auto n = b.vs(); n; --n) { b.ref(); for (int i = 0; i < 4; ++i) b.u16(); }
		});
		part("flags", [&] { for (int i = 0; i < 6; ++i) b.u8(); });  // 0xd67280 .. 0xd67394
		part("list 0xb79ad0", [&] {  // vsval n x u32
			for (auto n = b.vs(); n; --n) b.u32();
		});
		part("knowledge", [&] {  // 0xd673bf: vsval n x 0xd2faa0 (+0xc98)
			for (auto n = b.vs(); n; --n) detail::Knowledge(b);
		});
		part("fields 6", [&] { b.u8(); });  // +0xcf1
		part("map +0x828", [&] {  // 0xd674e3: vsval n x (ref, u32, u32)
			for (auto n = b.vs(); n; --n) { b.ref(); b.u32(); b.u32(); }
		});
		part("fields 7", [&] {
			b.u32();                                // +0xb9c
			for (int i = 0; i < 6; ++i) b.u8();     // 0xd6757b .. 0xd67681
			b.u32(); b.u32();                       // +0xd48, +0xd4c
			if (ver >= 0xa) b.u32();                // +0xd50
		});
		part("list +0x9d8", [&] {  // 0xd67708: u32 n x ref
			for (auto n = b.u32(); n; --n) b.ref();
		});
		part("fields 8", [&] {
			b.ref();                 // +0x9f0
			b.u8(); b.u32(); b.u8(); // 0xd677e5, +0xd54, 0xd67835
			b.ref();                 // +0xd28
			if (ver < 0x39) b.u8();
			b.u8(); b.u32(); b.u8(); // 0xd678d8, 0xd678f9, 0xd67920
			for (int i = 0; i < 5; ++i) b.ref();  // +0xd70, +0xd6c, +0xd68, +0xa5c, +0xd78
		});
		a.extraInventoryRange.begin = r.pos();
		if (ver < 0x2e || ver >= 0x31) {  // 0xd67b30: 0x4f6bb0 on the reference at +0xd80 -> 0x34ee00
			part("inventory +0xd80", [&] { a.extraInventory = detail::Inventory(b, a_save); });
		}
		a.extraInventoryRange.end = r.pos();
		part("fields 9", [&] { b.u8(); b.u32(); b.u8(); });  // 0xd67b4b, +0xce0, 0xd67b9b
		if (ver < 7) {
			part("old list", [&] { for (auto n = b.vs(); n; --n) b.ref(); });  // 0xd67be7
		}
		part("map +0x858", [&] {  // 0xd67c98: vsval n x (ref, u32, u32)
			for (auto n = b.vs(); n; --n) { b.ref(); b.u32(); b.u32(); }
		});
		part("list +0xdb0", [&] {  // 0xd67d23: u32 n x (ref, 8 bytes below form version 7, else u32)
			for (auto n = b.u32(); n; --n) {
				b.ref();
				if (ver < 7) b.raw(8);
				else b.u32();
			}
		});
		part("fields 10", [&] {
			b.ref();  // 0xd67e4c (+0xd90)
			if (ver >= 0x1f) { b.ref(); b.u8(); b.u8(); }
			if (ver >= 0x21) b.u8();                  // +0xdf4
			if (ver >= 0x2f && ver < 0x39) b.u32();
			if (ver >= 0x32) { b.u32(); b.u8(); }     // +0xdf8
			if (ver >= 0x43) b.u8();
		});
		a.playerRange.end = r.pos();
		a.playerPart = true;
	}
}
