// the reference part as fo4cf::DecodeRefr reads it: refrpos <save> <refid hex>... -> per refid: the positions of the
// extra data type bytes (top level and inventory stacks, in body order) and the end of the reference part
// (compared with the game's own TESObjectREFR::LoadGame by refrnative.py)
#include "core/ChangeFormFO4.h"
#include "core/ChangeFormData.h"
#include <algorithm>
#include <cstdio>
#include <string>
using namespace ess;
int main(int argc, char** argv)
{
	auto save = SaveFile::load(argv[1]);
	for (int i = 2; i < argc; ++i) {
		const auto want = static_cast<std::uint32_t>(std::stoul(argv[i], nullptr, 16));
		for (auto& c : save->changeForms) {
			if (c.refID.data != want) continue;
			const auto body = cf::Body(c);
			auto r = fo4cf::DecodeRefr(*save, c, body);
			std::vector<std::size_t> pos;
			for (auto& e : r.extras) pos.push_back(e.range.begin);
			for (auto& it : r.inventory)
				for (auto& st : it.stacks)
					for (auto& e : st.extras) pos.push_back(e.range.begin);
			std::sort(pos.begin(), pos.end());
			std::printf("%06X %s end %zu types", want, r.decoded ? "ok" : r.error.c_str(), r.refrPart.end);
			for (auto p : pos) std::printf(" %zu", p);
			std::printf("\n");
		}
	}
}
