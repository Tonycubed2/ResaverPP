// compare fo4cf::DecodeActor's reads with the emulator trace (trace_<save>.bin)
#include "core/ChangeFormFO4.h"
#include <cstdio>
#include <fstream>
#include <map>
using namespace ess;
int main(int argc, char** argv)
{
	auto save = SaveFile::load(argv[1]);
	std::ifstream f(argv[2], std::ios::binary);
	std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
	size_t p = 0;
	auto u32 = [&] { std::uint32_t v; std::memcpy(&v, d.data() + p, 4); p += 4; return v; };
	std::map<std::uint32_t, const ChangeForm*> byRef;
	for (auto& c : save->changeForms) if (c.typeCode() == 1) byRef[c.refID.data] = &c;
	const int verbose = argc > 3 ? std::atoi(argv[3]) : 0;
	std::uint32_t n = u32(); int ok = 0, bad = 0, stop = 0, skipped = 0; std::map<std::string,int> why;
	for (std::uint32_t i = 0; i < n; ++i) {
		std::uint32_t ref = u32(); bool exact = d[p++]; u32(); u32(); std::uint32_t k = u32();
		struct T { std::uint32_t pos; char kind; std::uint32_t site; };
		std::vector<T> t(k);
		for (auto& x : t) { x.pos = u32(); x.kind = d[p++]; x.site = u32(); }
		if (!exact || !byRef.count(ref)) { ++skipped; continue; }
		std::vector<std::pair<std::uint32_t, char>> mine;
		auto a = fo4cf::DecodeActor(*save, *byRef[ref], &mine);
		// emulator reads up to the actor value storage end: first read after the modifiers list
		size_t e = 0; while (e < t.size() && t[e].site != 0xcedcf1) ++e;   // VSVAL of base values
		// my trace starts at the prefix; emulator's too (prefix, then process after the REFR part)
		// drop emulator reads inside the REFR part (between the prefix and process): compare from index 2
		std::vector<T> em; for (size_t j = 0; j < t.size(); ++j) { if (j >= 2 && t[j].pos < (mine.size() > 2 ? mine[2].first : 0)) continue; em.push_back(t[j]); }
		size_t lim = mine.size();
		size_t j = 0; bool mism = false; size_t ei = 0;
		for (; j < lim; ++j) {
			while (ei < em.size() && a.effects.size()) { bool in=false; for (auto& x : a.effects) if (em[ei].pos > x.data.begin - 1 && em[ei].pos < x.data.end && em[ei].pos >= x.data.begin) in = true; if (!in) break; ++ei; }
			if (ei >= em.size() || em[ei].pos != mine[j].first) { mism = true; break; }
			++ei;
		}
		if (!a.decoded) {
			++stop; why[a.error]++;
			if (mism && verbose) std::printf("%06X mismatch at my read %zu pos %u (emu %u site %x) before stop\n", ref, j, j < mine.size() ? mine[j].first : 0, ei < em.size() ? em[ei].pos : 0, ei < em.size() ? em[ei].site : 0);
			continue;
		}
		if (mism) { ++bad; if (verbose) std::printf("%06X mismatch at my read %zu pos %u (emu %u site %x, prev site %x)\n", ref, j, mine[j].first, ei < em.size() ? em[ei].pos : 0, ei < em.size() ? em[ei].site : 0, ei ? em[ei-1].site : 0); }
		else ++ok;
	}
	std::printf("exact %d mismatch %d stopped %d (not exact in emulator %d)\n", ok, bad, stop, skipped);
	for (auto& [w, c] : why) std::printf("  %5d %s\n", c, w.c_str());
}
