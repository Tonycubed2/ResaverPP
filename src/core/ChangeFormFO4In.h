// ReSaver++ - internal to the Fallout 4 actor decoders (ChangeFormFO4Actor.cpp, ChangeFormFO4Move.cpp): the read
// cursor with an optional trace of every read, in the sizes and order the game's loaders use.
#pragma once
#include "ChangeFormData.h"

#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ess::fo4cf::detail
{
	struct Stop : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

	// the buffer with an optional trace of every read: (position, kind) - R raw, F refid, V vsval, S string, B block
	struct In
	{
		Reader&                                      r;
		std::uint32_t                                ver;
		std::vector<std::pair<std::uint32_t, char>>* trace = nullptr;
		// the movement controller's pointer ids already read in this change form (the game keeps one table per
		// Actor::LoadGame; an id seen for the first time is followed by the object it stands for)
		std::set<std::uint32_t> ids68, ids70, ids78;
		bool                    noTable = false;  // no pointer id table (a REFR's own extra data): an id stops reading

		void          mark(char k) { if (trace) trace->push_back({ static_cast<std::uint32_t>(r.pos()), k }); }
		void          raw(std::size_t n) { mark('R'); r.need(n); r.seek(r.pos() + n); }
		std::uint8_t  u8() { mark('R'); return r.u8(); }
		std::uint16_t u16() { mark('R'); return r.u16(); }
		std::uint32_t u32() { mark('R'); return r.u32(); }
		float         f32() { mark('R'); return r.f32(); }
		bool          bln() { return u8() != 0; }
		RefID         ref() { mark('F'); return RefID::read(r); }
		std::uint32_t vs()
		{
			mark('V');
			r.need(1);
			const std::uint8_t a = *r.here();
			switch (a & 3) {
			case 0: return r.u8() >> 2;
			case 1: return r.u16() >> 2;
			default:
				{
					r.need(3);
					const std::uint32_t b0 = r.u8(), b1 = r.u8(), b2 = r.u8();
					return (b0 | (b1 << 8) | (b2 << 16)) >> 2;
				}
			}
		}
		void wstr() { mark('S'); const auto n = r.u16(); r.need(n); r.seek(r.pos() + n); }
		std::string wstrS()  // the same read, keeping the text (class names)
		{
			mark('S');
			const auto n = r.u16();
			r.need(n);
			std::string s(reinterpret_cast<const char*>(r.here()), n);
			r.seek(r.pos() + n);
			return s;
		}
		void blob(std::size_t n) { mark('B'); r.need(n); r.seek(r.pos() + n); }  // copied as one block
		void skip(std::size_t n) { r.need(n); r.seek(r.pos() + n); }             // not read by the game here
		[[noreturn]] void fail(const std::string& a_what) { throw Stop("actor part: " + a_what + " not decoded"); }
	};

	// The movement controller (0xdc8e40), read when the actor's "has a controller" byte is set.
	void MovementController(In& b);
	// Pathing pieces shared with the package data (ChangeFormFO4Move.cpp): a path point (0x1df23d0) and the
	// reader's pointer ids +0x68 (0xdd85c0) and +0x70 (0xdd8700).
	void PathPoint(In& b);
	void Pid68(In& b);
	void Pid70(In& b);
	// A package's saved data (the AI process's package instances, 0xcea430): a_type is the package type byte.
	void PackageData(In& b, std::uint8_t a_type);
	// A created package's own form (a_type: its package type), read before the type byte by 0xcea430.
	void CreatedPackage(In& b, std::uint8_t a_type);
}
