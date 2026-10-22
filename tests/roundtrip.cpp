// Round-trip test: load a save, rebuild the body, compare with the original body byte-for-byte.
#include "core/Save.h"

#include <chrono>
#include <cstdio>
#include <fstream>

using namespace ess;

static std::vector<std::uint8_t> OriginalBody(const std::filesystem::path& p, std::size_t headerSize, Compression c)
{
	std::ifstream f(p, std::ios::binary);
	std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(f)), {});
	if (c == Compression::kNone) {
		return { file.begin() + headerSize, file.end() };
	}
	std::uint32_t u, n;
	std::memcpy(&u, file.data() + headerSize, 4);
	std::memcpy(&n, file.data() + headerSize + 4, 4);
	return c == Compression::kLZ4 ? DecompressLZ4(file.data() + headerSize + 8, n, u) : DecompressZlib(file.data() + headerSize + 8, n, u);
}

int main(int argc, char** argv)
{
	int fails = 0;
	for (int i = 1; i < argc; ++i) {
		try {
			auto t0 = std::chrono::steady_clock::now();
			auto save = SaveFile::load(argv[i]);
			auto t1 = std::chrono::steady_clock::now();
			auto body = save->buildBody();
			auto orig = OriginalBody(argv[i], save->header.totalSize(), save->header.compression);
			const Papyrus* p = save->papyrus();
			std::printf("%s\n  load %.2fs | plugins %zu+%zu | changeforms %zu | strings %zu scripts %zu instances %zu refs %zu arrays %zu threads %zu fm %zu susp %zu/%zu\n",
				argv[i], std::chrono::duration<double>(t1 - t0).count(), save->plugins.full.size(), save->plugins.lite.size(), save->changeForms.size(),
				p->strings.size(), p->scripts.size(), p->instances.size(), p->references.size(), p->arrays.size(), p->activeScripts.size(),
				p->functionMessages.size(), p->suspended1.size(), p->suspended2.size());
			if (body == orig) {
				std::printf("  ROUND TRIP: identical (%zu bytes)\n", body.size());
			} else {
				std::size_t k = 0;
				while (k < body.size() && k < orig.size() && body[k] == orig[k]) ++k;
				std::printf("  ROUND TRIP: MISMATCH sizes %zu vs %zu, first difference at body offset %zu\n", body.size(), orig.size(), k);
				++fails;
			}
		} catch (const std::exception& e) {
			std::printf("%s\n  ERROR: %s\n", argv[i], e.what());
			++fails;
		}
	}
	return fails;
}
