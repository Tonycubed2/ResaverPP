// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// resaverpp-cli --cosave: show and clean the SKSE / F4SE co-save (core/Cosave.h).
#include "core/ByteIO.h"
#include "core/Cosave.h"
#include "core/Save.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace ess;

namespace
{
	std::string Lower(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	std::vector<std::string> Split(const std::string& a_s)
	{
		std::vector<std::string> out;
		std::string              cur;
		for (char c : a_s) {
			if (c == ',' || c == ';') {
				if (!cur.empty()) {
					out.push_back(cur);
				}
				cur.clear();
			} else {
				cur += c;
			}
		}
		if (!cur.empty()) {
			out.push_back(cur);
		}
		return out;
	}

	// "SKEE", "'SKEE'", "534B4545", "0x534B4545"
	bool ParseId(std::string a_s, std::uint32_t& a_out)
	{
		if (a_s.size() >= 2 && a_s.front() == '\'' && a_s.back() == '\'') {
			a_s = a_s.substr(1, a_s.size() - 2);
		}
		std::string hex = a_s;
		if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) {
			hex = hex.substr(2);
		}
		if (hex.size() == 8 && std::all_of(hex.begin(), hex.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
			a_out = static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
			return true;
		}
		if (a_s.size() >= 1 && a_s.size() <= 4) {
			std::uint32_t v = 0;
			for (char c : a_s) {
				v = (v << 8) | static_cast<std::uint8_t>(c);
			}
			a_out = v;
			return true;
		}
		return false;
	}

	std::vector<std::uint8_t> ReadAll(const std::filesystem::path& a_p)
	{
		std::ifstream f(a_p, std::ios::binary);
		if (!f) {
			throw std::runtime_error("cannot open " + a_p.string());
		}
		return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	}

	// MO2 profile: modlist.txt, "+Name" enabled, "-Name" disabled
	std::set<std::string> DisabledMods(const std::filesystem::path& a_profile)
	{
		std::set<std::string> off;
		std::ifstream         f(a_profile / "modlist.txt");
		std::string           line;
		while (std::getline(f, line)) {
			while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
				line.pop_back();
			}
			if (line.size() > 1 && line[0] == '-') {
				off.insert(line.substr(1));
			}
		}
		return off;
	}
}

int CosaveMain(int argc, char** argv)
{
	if (argc < 3) {
		std::fputs("usage: resaverpp-cli --cosave <save.ess|.fos|.sfs|.skse|.f4se|.sfse> [--sources <folder>] [--profile <folder>] [--chunks] [--top n]\n"
				   "       [--cosave-remove <ids>|not-installed] [--cosave-gone <plugin,plugin>] [-o <new save or co-save>]\n",
			stderr);
		return 1;
	}
	const std::filesystem::path in = argv[2];
	std::filesystem::path       sources, profile, out;
	std::string                 removeArg, goneArg;
	std::size_t                 top = 20;
	bool                        chunks = false;
	for (int i = 3; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "--sources") {
			sources = next();
		} else if (a == "--profile") {
			profile = next();
		} else if (a == "--chunks") {
			chunks = true;
		} else if (a == "--top") {
			top = static_cast<std::size_t>(std::atoi(next().c_str()));
		} else if (a == "--cosave-remove") {
			removeArg = next();
		} else if (a == "--cosave-gone") {
			goneArg = next();
		} else if (a == "-o") {
			out = next();
		} else {
			std::fprintf(stderr, "unknown option %s\n", a.c_str());
			return 1;
		}
	}
	try {
		const auto ext = Lower(in.extension().string());
		if (ext == ".sfs" || ext == ".sfse") {
			// Starfield: SFSE writes no co-save (Cosave.h kSfseNote); an .sfse is only shown and kept
			auto co = in;
			co.replace_extension(".sfse");
			std::error_code ec;
			if (!std::filesystem::is_regular_file(co, ec)) {
				std::printf("%s has no .sfse next to it\n", in.filename().string().c_str());
			} else {
				std::printf("%s: %s, not read; it is kept as it is and written next to every save ReSaver Plus writes\n",
					co.filename().string().c_str(), cosave::SizeText(std::filesystem::file_size(co, ec)).c_str());
			}
			std::printf("%s\n", cosave::kSfseNote);
			if (!sources.empty()) {
				const auto dlls = cosave::SfsePluginDlls(sources);
				std::printf("SFSE plugins in %s: %zu\n", (sources / "SFSE" / "Plugins").string().c_str(), dlls.size());
				for (const auto& d : dlls) {
					std::printf("  %s\n", d.c_str());
				}
			}
			if (!removeArg.empty()) {
				std::fprintf(stderr, "--cosave-remove: an .sfse has no known format, nothing can be removed from it\n");
				return 1;
			}
			return 0;
		}
		const bool isCosave = ext == ".skse" || ext == ".f4se";
		std::unique_ptr<SaveFile>  save;
		std::vector<std::uint8_t>  bytes;
		bool                       x86 = false;
		if (isCosave) {
			bytes = ReadAll(in);
		} else {
			save = SaveFile::load(in);
			bytes = save->cosave;
			x86 = save->isLE();
			if (bytes.empty()) {
				std::printf("%s has no %s co-save next to it\n", in.filename().string().c_str(), save->cosaveExt());
				return 0;
			}
		}
		auto       file = cosave::Parse(bytes);
		const auto again = cosave::Write(file);
		std::vector<cosave::Dll> dlls;
		if (!sources.empty()) {
			dlls = cosave::ListPluginDlls(nullptr, sources, file.fo4);
			if (!profile.empty()) {
				const auto off = DisabledMods(profile);
				for (auto& d : dlls) {
					d.enabled = !off.contains(d.mod);
				}
			}
		}
		const auto view = cosave::MakeView(file, dlls, x86);
		std::fputs(cosave::Report(file, view, top, chunks).c_str(), stdout);
		std::printf("read back: %s\n", again == bytes ? "byte-identical" : "DIFFERENT (please report)");

		if (removeArg.empty() && goneArg.empty()) {
			return 0;
		}
		cosave::CleanOptions opt;
		if (Lower(removeArg) == "not-installed") {
			if (!view.searched) {
				std::fputs("--cosave-remove not-installed needs --sources (a mods folder or the Data folder) to tell what is installed\n", stderr);
				return 1;
			}
			if (!view.plausible()) {
				std::fputs("none of the co-save's plugins was found in --sources: that is probably not the folder the game loads its DLLs from; nothing removed\n", stderr);
				return 1;
			}
			for (const auto& r : view.rows) {
				if (r.status == cosave::Status::kNotInstalled) {
					opt.removeUids.insert(r.uid);
				}
			}
		} else {
			for (const auto& t : Split(removeArg)) {
				std::uint32_t u;
				if (!ParseId(t, u)) {
					std::fprintf(stderr, "not a plugin id: %s (give 'SKEE', 534B4545 or 0x534B4545)\n", t.c_str());
					return 1;
				}
				if (u == 0) {
					std::fputs("id 0 is the script extender's own data and is never removed\n", stderr);
					return 1;
				}
				if (!file.find(u)) {
					std::fprintf(stderr, "the co-save has no plugin %s\n", cosave::IdText(u).c_str());
					return 1;
				}
				opt.removeUids.insert(u);
			}
		}
		for (const auto& g : Split(goneArg)) {
			opt.gonePlugins.insert(Lower(g));
		}
		if (out.empty()) {
			std::fputs("cleaning needs -o (a new save next to which the cleaned co-save is written, or a new co-save file)\n", stderr);
			return 1;
		}
		std::error_code ec;
		if (std::filesystem::equivalent(out, in, ec)) {
			std::fputs("-o must be a new file: the original is never changed\n", stderr);
			return 1;
		}
		std::printf("\n%s\n\n", cosave::kRemoveExplanation);
		cosave::CleanResult res;
		const auto          cleaned = cosave::CleanBytes(bytes, opt, &res);
		for (const auto& l : res.lines) {
			std::printf("  %s\n", l.c_str());
		}
		std::printf("co-save %s -> %s (%zu plugin block(s) removed, %zu PapyrusUtil object(s) removed)\n", cosave::SizeText(res.before).c_str(),
			cosave::SizeText(res.after).c_str(), res.plugins, res.papuObjects);
		const auto outExt = Lower(out.extension().string());
		if (isCosave || outExt == ".skse" || outExt == ".f4se") {
			if (!isCosave) {
				std::fputs("-o: give a save file (.ess / .fos); the co-save is written next to it with the same name\n", stderr);
				return 1;
			}
			std::ofstream f(out, std::ios::binary);
			f.write(reinterpret_cast<const char*>(cleaned.data()), static_cast<std::streamsize>(cleaned.size()));
			if (!f) {
				throw std::runtime_error("could not write " + out.string());
			}
			f.close();
			const auto back = cosave::Parse(ReadAll(out));
			std::printf("written %s and verified (%zu plugins)\n", out.string().c_str(), back.plugins.size());
		} else {
			save->cosave = cleaned;
			const auto wr = save->write(out, std::filesystem::exists(out, ec));
			if (!wr.backup.empty()) {
				std::printf("previous file kept as %s%s\n", wr.backup.filename().string().c_str(),
					wr.cosaveBackup.empty() ? "" : (" + " + wr.cosaveBackup.filename().string()).c_str());
			}
			auto check = SaveFile::load(out);
			const auto back = cosave::Parse(check->cosave);
			std::printf("written %s + %s and verified (save re-read OK, co-save %zu plugins)\n", out.string().c_str(),
				out.filename().replace_extension(check->cosaveExt()).string().c_str(), back.plugins.size());
		}
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
	return 0;
}
