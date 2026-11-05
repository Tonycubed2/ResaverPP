// dump every change form: u32 refid(raw 3 bytes), u8 type, u8 version, u32 flags, u32 formid, u32 len, body
#include "core/Save.h"
#include "core/ChangeFormData.h"
#include <cstdio>
#include <fstream>
using namespace ess;
int main(int argc, char** argv)
{
	auto save = SaveFile::load(argv[1]);
	std::ofstream o(argv[2], std::ios::binary);
	auto w = [&](std::uint32_t v) { o.write(reinterpret_cast<char*>(&v), 4); };
	std::printf("form version %u, %zu change forms\n", save->formVersion, save->changeForms.size());
	w(save->formVersion);
	for (const auto& c : save->changeForms) {
		auto body = cf::Body(c);
		w(c.refID.data);
		o.put(static_cast<char>(c.typeCode()));
		o.put(static_cast<char>(c.version));
		w(c.changeFlags);
		w(save->formIDOf(c.refID));
		w(static_cast<std::uint32_t>(body.size()));
		o.write(reinterpret_cast<const char*>(body.data()), body.size());
	}
}
