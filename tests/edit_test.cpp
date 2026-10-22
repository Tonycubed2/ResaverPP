// Edits an int, a float, a bool and a string variable, saves, reloads and checks the values.
#include "core/Save.h"

#include <cstdio>
#include <cstring>

using namespace ess;

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::puts("usage: rs_edit_test <in.ess> <out.ess>");
		return 1;
	}
	auto  s = SaveFile::load(argv[1]);
	auto& p = *s->papyrus();
	EID   iid = 0, fid = 0, bid = 0, sid = 0;
	std::size_t iix = 0, fix = 0, bix = 0, six = 0;
	for (auto& in : p.instances) {
		if (!in.data) continue;
		for (std::size_t k = 0; k < in.data->vars.size(); ++k) {
			auto& v = in.data->vars[k];
			if (!iid && v.type == VType::kInt) { v.value = static_cast<std::uint32_t>(-12345); iid = in.id; iix = k; }
			if (!fid && v.type == VType::kFloat) { float f = 3.25f; std::uint32_t b; std::memcpy(&b, &f, 4); v.value = b; fid = in.id; fix = k; }
			if (!bid && v.type == VType::kBool) { v.value = v.value ? 0 : 1; bid = in.id; bix = k; }
			if (!sid && v.type == VType::kString) { v.value = p.internString("ReSaver++ edit test string"); sid = in.id; six = k; }
		}
		if (iid && fid && bid && sid) break;
	}
	const bool oldBool = !p.findInstance(bid)->data->vars[bix].value;
	s->save(argv[2]);
	auto  t = SaveFile::load(argv[2]);
	auto& q = *t->papyrus();
	bool ok = true;
	ok &= static_cast<std::int32_t>(q.findInstance(iid)->data->vars[iix].value) == -12345;
	ok &= q.findInstance(fid)->data->vars[fix].asFloat() == 3.25f;
	ok &= (q.findInstance(bid)->data->vars[bix].value != 0) == !oldBool;
	ok &= q.str(static_cast<TStr>(q.findInstance(sid)->data->vars[six].value)) == "ReSaver++ edit test string";
	ok &= q.strings.size() == p.strings.size();
	std::printf("edit test: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
