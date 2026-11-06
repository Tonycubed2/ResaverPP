// per ACHR: the extra 0x6c entries of the reference part: refid, entry begin, entry end (C++ decoder)
#include "core/ChangeFormFO4.h"
#include <cstdio>
using namespace ess;
int main(int argc, char** argv)
{
	auto save = SaveFile::load(argv[1]);
	for (auto& c : save->changeForms) {
		if (c.typeCode() != 1 && c.typeCode() != 0) continue;
		const auto r = fo4cf::DecodeRefr(*save, c);
		if (!r.decoded) continue;
		for (auto& e : r.extras) if (e.type == 0x6c) std::printf("%x %zu %zu\n", c.refID.data, e.range.begin, e.range.end);
	}
}
