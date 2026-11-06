// ReSaver++ - Fallout 4 ACHR: the actor's combat controller (CombatController::LoadGame 0x87c540, read by
// Actor::LoadGame when the byte at 0xc8b31c is set) and the combat objects saved through the object pointer
// reader 0xed5cf0 (also 0xed5bd0 and 0xed5e10, the same format). RVAs in Fallout4.exe 1.11.240.
//
// Every function below follows the game function named next to it, read for read; the class ids are the ones the
// game's factory (0xed5950) creates, the type registry (0x3271390) is the one the game fills in 0x89ce90. Checked
// against the game's own loader run in an emulator on the test saves (tools/fo4emu: achr.py, stack1.py, combat.py).
#include "ChangeFormFO4In.h"

#include <cstdio>

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

		void U(In& b, int n) { for (int i = 0; i < n; ++i) b.u32(); }
		void Pair4(In& b) { b.u32(); b.u32(); }  // 0xd4c740
		void One4(In& b) { b.u32(); }            // 0xd4c830
		void RefU32(In& b) { b.ref(); b.u32(); } // 0x437100 -> 0x436f00

		// 0x89cb10 (and the same code inline in 0x89cd40): a value of a registered type. u32 type hash; 0xFFFFFFFF
		// none; else u16, u8 and the type's own data, read by the LoadGame (+0x30) of the FIRST registry entry with
		// that hash (entry 0 when none has it). The registry is filled by 0x89ce90 (21 entries, in this order); each
		// entry's loader reads a fixed number of bytes (0x959xxx: c0fd90 with that size).
		void Tagged(In& b)
		{
			static constexpr struct
			{
				std::uint32_t hash;
				std::uint32_t size;
			} kTypes[] = {
				{ 0xB0462B3Du, 12 }, { 0xE397C212u, 16 }, { 0x90B94761u, 32 }, { 0xE9DCB5DDu, 32 }, { 0x0C8CA830u, 36 },
				{ 0xE9DCB5DDu, 52 }, { 0x086DD594u, 24 }, { 0xCFA5D7C8u, 92 }, { 0xC521D2CFu, 28 }, { 0xE9DCB5DDu, 48 },
				{ 0xE9DCB5DDu, 124 }, { 0xE9DCB5DDu, 28 }, { 0xE9DCB5DDu, 40 }, { 0xE9DCB5DDu, 44 }, { 0x4CB7B1A5u, 156 },
				{ 0x4CB7B1A5u, 44 }, { 0x4CB7B1A5u, 48 }, { 0x4CB7B1A5u, 64 }, { 0x4CB7B1A5u, 68 }, { 0x4CB7B1A5u, 56 },
				{ 0x4CB7B1A5u, 60 },
			};
			const auto hash = b.u32();
			if (hash == 0xFFFFFFFFu) {
				return;
			}
			std::size_t idx = 0;
			for (std::size_t i = 0; i < std::size(kTypes); ++i) {
				if (kTypes[i].hash == hash) {
					idx = i;
					break;
				}
			}
			b.u16();
			b.u8();  // 0x884220
			b.raw(kTypes[idx].size);
		}

		// 0xe712e0: CombatArea's common part
		void Area(In& b)
		{
			b.ref(); b.u32(); b.u8(); b.u8();
			Pair4(b);
			Tagged(b);
			b.ref();
		}

		// 0xeb0eb0: CombatInventoryItem's common part
		void Item(In& b)
		{
			RefU32(b);
			b.u32(); b.ref(); b.u16(); b.u16(); b.u32();
		}

		// 0xe67c10: CombatAimController
		void Aim(In& b)
		{
			b.raw(12);
			b.ref();  // 0x21e390
			U(b, 4);
			Pair4(b);
		}

		// 0xe7e380: CombatCoverSearch's search data
		void CoverSearchData(In& b)
		{
			b.ref();
			U(b, 5);
			for (int i = 0; i < 8; ++i) b.u8();
			U(b, 2);  // 0x883e90
			U(b, 2);  // 0x883e90
			b.raw(12); b.raw(12); b.raw(12);
			for (auto n = b.vs(); n; --n) b.raw(12);
			Object(b);
		}
	}

	// 0xed5cf0 (0xed5bd0, 0xed5e10): u64 id; 0 none; an even id refers to an object read before (no reads); an odd id
	// is the object itself: u32 class id, then the LoadGame (vtable +0x20) of the object the factory makes for that
	// class (0xed5950), or of the object already known under that id (0xed5b40 / 0xed5a80).
	void Object(In& b)
	{
		b.mark('R');
		b.r.need(8);
		const std::uint64_t lo = b.r.u32();
		const std::uint64_t id = lo | (static_cast<std::uint64_t>(b.r.u32()) << 32);
		if (!(id & 1)) {
			return;
		}
		auto cls = b.u32();
		const auto [it, fresh] = b.objects.emplace(id & ~std::uint64_t{ 1 }, cls);
		if (!fresh) {
			cls = it->second;
		}
		switch (cls) {
		case 0xC8CC82FCu:  // CombatViewControllerStandard 0x97b830
			b.raw(12); b.u32(); b.u32();
			return;
		case 0xC5642853u:  // CombatViewControllerPath 0x97c520
			b.raw(12); b.u32(); b.u32(); Pair4(b);
			return;
		case 0xA0DD2269u:  // CombatPositionTracker 0x8968a0
			b.ref();
			return;
		case 0xC3719B85u:  // CombatTargetSelectorStandard 0xee8c30
			b.ref(); b.u32(); b.u32(); Pair4(b);
			return;
		case 0xA9BDAAFEu:  // CombatAreaStandard 0xe72060
			Area(b);
			b.u32();
			return;
		case 0x55FAB455u:  // CombatAreaHoldPosition 0xe72fd0
			Area(b);
			b.ref();  // 0x21e390
			Pair4(b);
			for (auto n = b.vs(); n; --n) {
				b.u32();
				Tagged(b);
				b.ref();
				b.ref();  // 0x21e390
			}
			Pair4(b);
			b.u32();
			return;
		case 0x5ACB99C8u:  // CombatInventoryItemMelee 0xeb17c0
		case 0x8121581Du:  // CombatInventoryItemOneHandedBlock 0xeb2e70
			Item(b);
			b.u32();
			return;
		case 0x71B72E31u:  // CombatInventoryItemRanged 0xeb2170
		case 0x26038F0Fu:  // CombatInventoryItemThrown
		case 0xD10237D8u:  // CombatInventoryItemGrenade
			Item(b);
			b.ref();
			U(b, 2);  // 0x883e90
			b.u32();
			return;
		case 0x1B0C8C0Du:  // CombatInventoryItemMagicT<Magic, MagicCasterReanimate> 0xeb4240
		case 0x70CBCB0Au:  // CombatInventoryItemMagicT<Potion, MagicCasterRestore>
			Item(b);
			U(b, 2);  // 0x883e90
			U(b, 4);
			return;
		case 0x86D330A0u:  // CombatAimController 0xe67c10
		case 0xBE236AC9u:  // CombatDisableAimController
			Aim(b);
			return;
		case 0xF06E7A91u:  // CombatProjectileAimController 0xe6ae60
			Aim(b);
			b.ref();
			U(b, 3);
			b.raw(12); b.raw(12); b.raw(8);
			b.raw(12); b.raw(12); b.raw(12); b.raw(12);
			U(b, 3);
			U(b, 2);  // 0x883e90
			One4(b);
			b.wstr();  // 0xc0fb90
			return;
		case 0x4F117E2Bu:  // CombatCoverLocation 0xe801a0 (the first three through PathingStreamLoadGame 0xdd8270)
			b.ref(); b.u16(); b.u32();
			b.u32(); b.raw(24); b.raw(12); b.raw(12); b.u32(); b.raw(4);
			b.u32(); b.u8(); b.u8(); b.u16(); b.raw(8); b.raw(8);
			b.ref();
			b.raw(12); b.raw(12); b.raw(12);
			U(b, 4);
			b.u8(); b.u8();
			return;
		case 0x93409B55u:  // CombatCoverSearch 0xe7f280
			CoverSearchData(b);
			Object(b);
			Object(b);
			b.u32();
			b.raw(12);
			Object(b);
			return;
		case 0x9C2C29FAu:  // CombatTargetLocationSearch 0xede770
			Object(b);
			for (auto n = b.vs(); n; --n) b.u32();
			for (auto n = b.vs(); n; --n) {
				U(b, 5);
				b.raw(12);
			}
			b.ref(); b.ref(); b.ref();
			b.raw(12);
			U(b, 4);
			return;
		case 0x30290669u:  // CombatPathingSearchArea 0xfe6570 -> 0x89cd40
			for (auto n = b.vs(); n; --n) Tagged(b);
			return;
		default:
			b.fail("combat object class " + Hex(cls) + " (0xed5cf0)");
		}
	}

	namespace
	{
		// 0xeda730 (the combat controller's +0x48)
		void State(In& b)
		{
			for (int i = 0; i < 4; ++i) b.u8();
			if (b.ver >= 0x14) b.u8();
			for (int i = 0; i < 4; ++i) b.u8();
			b.u32();
			b.raw(12); b.raw(12);
			b.u32(); b.u32();
			b.u8(); b.u8();
			b.u32(); b.u32();
			b.u8();
			b.u32();
			for (int i = 0; i < 8; ++i) One4(b);
			for (auto n = b.vs(); n; --n) {
				b.ref();
				if (b.ver >= 8) { b.raw(12); b.ref(); }  // 0x4a9380
				b.u32();
				if (b.ver >= 8) One4(b);
			}
			for (auto n = b.vs(); n; --n) {
				b.raw(12); b.ref();  // 0x4a9380
				One4(b);
			}
			for (auto n = b.vs(); n; --n) b.ref();
			b.u8();
			Pair4(b);
			Pair4(b);
		}

		// 0xea4830: a list of objects, u16, 4 x u32
		void ObjectList(In& b)
		{
			for (auto n = b.vs(); n; --n) Object(b);
			b.u16();
			U(b, 4);
		}

		// 0xea7100 (the combat controller's +0x50)
		void Targets(In& b)
		{
			U(b, 13);
			b.u16();
			if (b.ver >= 0x33) b.u16();
			Pair4(b);
			b.u16();  // 0x884240
			for (int i = 0; i < 7; ++i) {
				for (auto n = b.vs(); n; --n) Object(b);
			}
			ObjectList(b);
			ObjectList(b);
			for (auto n = b.vs(); n; --n) {
				Object(b);
				One4(b);
			}
			for (auto n = b.vs(); n; --n) RefU32(b);
			for (auto n = b.vs(); n; --n) b.ref();
			for (auto n = b.vs(); n; --n) {
				RefU32(b);
				b.u32();
			}
			for (auto n = b.vs(); n; --n) RefU32(b);
		}

		// a block of the size before it: the writer (0xc11dd0 / 0xc11f40) stores the number of bytes the value's own
		// SaveGame wrote and the reader ends at that size (the position is max(its own end, start + size))
		void Sized(In& b)
		{
			const auto n = b.vs();
			if (n) b.blob(n);
		}

		// 0xe75e30 (the combat controller's +0x58): named values; a name the game knows is loaded by its type
		// (0xe75eed map), an unknown one is skipped; either way the reader ends at the saved size (writer 0xe76300)
		void Values(In& b)
		{
			for (auto n = b.vs(); n; --n) {
				b.wstr();
				Sized(b);
			}
			b.u32();
		}

		// 0xeff4d0 (the combat controller's +0x60)
		void Tree(In& b)
		{
			b.u32();
			if (b.u8()) {
				Sized(b);  // 0xf04bf0 -> 0xf04f50 (writer 0xf04b90)
			}
			for (auto n = b.vs(); n; --n) b.u32();
			for (auto n = b.vs(); n; --n) {
				b.wstr();
				b.u32();  // 0xf02fd0
				for (auto m = b.vs(); m; --m) b.u32();
				for (auto m = b.vs(); m; --m) { b.u32(); b.u32(); }
			}
		}
	}

	// 0x87c540
	void CombatController(In& b)
	{
		b.ref();  // 0x22b640
		b.ref();
		b.u32();
		b.u32(); b.u32();
		for (int i = 0; i < 5; ++i) b.u8();
		b.u32();
		Pair4(b);
		for (auto n = b.vs(); n; --n) Object(b);  // +0x98
		b.u32(); b.u32();
		for (auto n = b.vs(); n; --n) Object(b);  // +0xc0
		b.u32();
		for (auto n = b.vs(); n; --n) Object(b);  // +0xe0
		b.u32();
		for (auto n = b.vs(); n; --n) Object(b);  // +0x100
		Object(b);                                // +0x118
		Object(b);                                // +0x120
		for (auto n = b.vs(); n; --n) Object(b);  // +0x128
		for (auto n = b.vs(); n; --n) {
			b.wstr();
			b.wstr();
		}
		State(b);
		Targets(b);
		Values(b);
		Tree(b);
	}
}
