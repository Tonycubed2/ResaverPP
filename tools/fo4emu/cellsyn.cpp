// Synthetic CELL cases reset with fo4cell::ResetCells and dumped (dumpcf format) for cellemu.py:
//  0000A001 outdoor, change form with map seen data + flags and no detach time (bits 31 | 1), at (-5, 7)  -> bits 30|29
//  0000A002 outdoor, no change form, at (200, -300)                                                      -> bits 30|28
//  0000A003 indoor, change form with seen data only (bit 31)                                              -> bit 30
//  0000A004 outdoor, no change form, at (3, 4) in worldspace 0x1234 the save has not visited               -> index appended
// usage: cellsyn <out.bin> <save.fos>
#include "core/CellResetFO4.h"
#include "core/ChangeFormData.h"
#include <cstdio>
#include <fstream>
using namespace ess;
int main(int, char** argv)
{
	// on Tony's real Save8 (argv[2]) so the player's place is known; ids 0000A001.. are given made-up plugin records
	auto loaded = SaveFile::load(argv[2]);
	SaveFile& save = *loaded;
	// the player (so the player's place is known): an interior elsewhere is enough for ListCells; take an ACHR-less save:
	auto mk = [&](std::uint32_t ref, std::uint32_t flags, std::vector<std::uint8_t> body) {
		ChangeForm c;
		c.refID.data = ref;
		c.typeField = 6;
		c.version = save.changeForms.front().version;
		c.changeFlags = flags;
		c.data = body;
		c.length1 = static_cast<std::uint32_t>(body.size());
		save.changeForms.push_back(c);
	};
	std::vector<std::uint8_t> ext(32, 0x11);  // exterior seen data, then u16 flags? (order: bit 1 flags before bit 31)
	std::vector<std::uint8_t> a = { 0x02, 0x00 };
	a.insert(a.end(), ext.begin(), ext.end());
	mk(0x40A001, 0x80000002u, a);
	std::vector<std::uint8_t> in = { 1 << 2, 0x14, 0xFF };
	in.insert(in.end(), 32, 0x7F);
	mk(0x40A003, 0x80000000u, in);
	fo4world::Lookup look;
	look.type = [](std::uint32_t id) { return id == 0x3C || id == 0x1234 ? std::string("WRLD") : std::string("CELL"); };
	look.cell = [](std::uint32_t id) {
		fo4world::CellRecord r;
		r.known = id >= 0xA001 && id <= 0xA004;
		r.interior = id == 0xA003;
		r.hasGrid = !r.interior;
		r.worldspace = id == 0xA004 ? 0x1234 : 0x3C;
		r.x = id == 0xA001 ? -5 : id == 0xA002 ? 200 : 3;
		r.y = id == 0xA001 ? 7 : id == 0xA002 ? -300 : 4;
		return r;
	};
	const auto res = fo4cell::ResetCells(save, { 0xA001, 0xA002, 0xA003, 0xA004 }, look);
	for (const auto& r : res) std::printf("result: '%s'\n", r.c_str());
	std::printf("visited worldspaces:");
	for (auto w : save.visitedWorldspaces) std::printf(" %08X", w);
	std::printf("\n");
	std::ofstream o(argv[1], std::ios::binary);
	auto w = [&](std::uint32_t v) { o.write(reinterpret_cast<char*>(&v), 4); };
	w(save.formVersion);
	for (const auto& c : save.changeForms) {
		if (c.typeCode() != 6 || save.formIDOf(c.refID) < 0xA001 || save.formIDOf(c.refID) > 0xA004) continue;
		auto body = cf::Body(c);
		const auto d = fo4cell::DecodeCell(save, c);
		std::printf("%08X flags %08X body %zu: initial type %d ws %08X (%d, %d) detach %08X complete %d\n", save.formIDOf(c.refID), c.changeFlags,
			body.size(), d.initialType, d.worldspace, d.x, d.y, d.detach, d.complete);
		w(c.refID.data);
		o.put(static_cast<char>(c.typeCode()));
		o.put(static_cast<char>(c.version));
		w(c.changeFlags);
		w(save.formIDOf(c.refID));
		w(static_cast<std::uint32_t>(body.size()));
		o.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
	}
}
