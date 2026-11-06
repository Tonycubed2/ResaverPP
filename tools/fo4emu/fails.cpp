// list ACHRs whose actor part does not read to its end: refid formid error stoppedAt bodysize
#include "core/ChangeFormFO4.h"
#include "core/ChangeFormData.h"
#include <cstdio>
#include <map>
using namespace ess;
int main(int argc, char** argv)
{
	auto save = SaveFile::load(argv[1]);
	std::map<std::string, int> why;
	int n = 0, ok = 0, full = 0;
	for (auto& c : save->changeForms) {
		if (c.typeCode() != 1) continue;
		++n;
		auto a = fo4cf::DecodeActor(*save, c);
		const auto len = cf::Body(c).size();
		if (a.tail) { ++ok; if (a.tailEnd == len) ++full; else if (argc > 2) std::printf("%06X %08X TAILEND %zu of %zu\n", c.refID.data, save->formIDOf(c.refID), a.tailEnd, len); continue; }
		why[a.error]++;
		if (argc > 2) std::printf("%06X %08X %s at %zu of %zu\n", c.refID.data, save->formIDOf(c.refID), a.error.c_str(), a.stoppedAt, len);
	}
	std::printf("ACHR %d, Actor::LoadGame read %d, body read to its end %d\n", n, ok, full);
	for (auto& [w, k] : why) std::printf("  %5d %s\n", k, w.c_str());
}
