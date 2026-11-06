// fo4cf::DecodeActor's reads for the emulator comparison (trcmp.py): cpptrace <save> <out> [all|combat]
// out: per complete non-player ACHR: u32 refid, u32 end of the reference part, u32 n, n x (u32 pos, u8 kind), u32 m, m x (u32 begin, u32 end) ranges
// the decoder skips without reading (active effect data). 'combat' keeps only actors with a combat controller or a
// combat object (an odd object id).
#include "core/ChangeFormFO4.h"
#include "core/ChangeFormData.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace ess;
int main(int argc, char** argv)
{
	auto save = SaveFile::load(argv[1]);
	FILE* f = std::fopen(argv[2], "wb");
	const bool combatOnly = argc > 3 && std::string(argv[3]) == "combat";
	auto w = [&](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
	int n = 0;
	for (auto& c : save->changeForms) {
		if (c.typeCode() != 1) continue;
		std::vector<std::pair<std::uint32_t, char>> t;
		auto a = fo4cf::DecodeActor(*save, c, &t);
		if (!a.complete || a.player) continue;
		bool odd = false;
		const auto body = cf::Body(c);
		if (combatOnly && !a.hasCombat) {
			// an odd object id read in the movement controller
			for (const auto& [p, k] : t) {
				if (k == 'R' && p + 8 <= body.size() && p >= a.controllerRange.begin && p < a.controllerRange.end) {
					std::uint64_t id; std::memcpy(&id, body.data() + p, 8);
					(void)id;
				}
			}
			odd = a.hasController;  // checked by the comparison itself: all controller actors when asked
		}
		if (combatOnly && !a.hasCombat && !odd) continue;
		const auto refr = fo4cf::DecodeRefr(*save, c, body);
		w(c.refID.data); w(static_cast<std::uint32_t>(refr.refrPart.end)); w(static_cast<std::uint32_t>(t.size()));
		for (auto& [p, k] : t) { w(p); std::fputc(k, f); }
		w(static_cast<std::uint32_t>(a.effects.size()));
		for (auto& e : a.effects) { w(static_cast<std::uint32_t>(e.data.begin)); w(static_cast<std::uint32_t>(e.data.end)); }
		++n;
	}
	std::fclose(f);
	std::printf("%d actors\n", n);
}
