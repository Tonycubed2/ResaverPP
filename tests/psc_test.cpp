// ReSaver++ - checks the Papyrus source analysis (blocks, outline, colours) over a folder of .psc
// files, and optionally runs the Papyrus compiler on one through ess::papc.
#include "core/PapyrusCompiler.h"
#include "core/PapyrusSyntax.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace ess;

int main(int argc, char** argv)
{
	if (argc >= 2 && std::string(argv[1]) == "compile" && argc >= 6) {
		// compile <compiler> <file.psc> <outdir> <import;import...>
		papc::Request r;
		r.compiler = argv[2];
		r.source = argv[3];
		r.output = argv[4];
		std::string imp = argv[5];
		std::stringstream ss(imp);
		for (std::string part; std::getline(ss, part, ';');) {
			r.imports.push_back(part);
		}
		const auto res = papc::Compile(r);
		std::printf("ran %d ok %d exit %d messages %zu\ncmd: %s\n", res.ran, res.ok, res.exitCode, res.messages.size(), res.commandLine.c_str());
		for (const auto& m : res.messages) {
			std::printf("  [%s] %d,%d: %s\n", m.file.c_str(), m.line, m.column, m.text.c_str());
		}
		for (const auto& l : res.output) {
			std::printf("  | %s\n", l.c_str());
		}
		if (!res.error.empty()) {
			std::printf("error: %s\n", res.error.c_str());
		}
		return 0;
	}
	if (argc < 2) {
		std::puts("usage: rs_psc_test <folder of .psc> | compile <compiler> <file.psc> <outdir> <imports>");
		return 1;
	}
	std::size_t files = 0, withProblems = 0, blocks = 0, outline = 0, lines = 0;
	psc::Context ctx;
	for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
		if (e.path().extension() != ".psc") {
			continue;
		}
		std::ifstream in(e.path(), std::ios::binary);
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const auto        L = psc::SplitLines(text);
		const auto        a = psc::Analyze(L);
		++files;
		blocks += a.blocks.size();
		outline += a.outline.size();
		lines += L.size();
		for (const auto& l : L) {
			std::vector<psc::Kind> k(l.size());
			psc::ColorLine(l.data(), l.size(), k.data(), ctx);
		}
		if (!a.problems.empty()) {
			++withProblems;
			if (withProblems <= 15) {
				std::printf("%s: %s\n", e.path().filename().string().c_str(), a.problems.front().c_str());
			}
		}
	}
	std::printf("files %zu, lines %zu, blocks %zu, outline items %zu, files with unbalanced blocks %zu\n", files, lines, blocks, outline, withProblems);
	return 0;
}
