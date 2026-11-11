// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// resaverpp-cli for Starfield saves (.sfs): what the save is made of, its change forms, deleting change forms, and
// writing it back. The Skyrim / Fallout 4 options do not apply to these saves and are refused.
#include "core/Ops.h"
#include "core/Save.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>

using namespace ess;

namespace
{
	bool IsSfs(const char* a_path)
	{
		std::string low = a_path;
		for (auto& c : low) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		if (low.size() > 4 && low.ends_with(".sfs")) {
			return true;
		}
		std::FILE* f = std::fopen(a_path, "rb");
		if (!f) {
			return false;
		}
		char       m[12]{};
		const auto n = std::fread(m, 1, sizeof(m), f);
		std::fclose(f);
		return (n >= 4 && std::memcmp(m, "BCPS", 4) == 0) || (n == 12 && std::memcmp(m, "SFS_SAVEGAME", 12) == 0);
	}
}

// -1: not a Starfield save (the caller goes on); else the exit code
int StarfieldMain(int argc, char** argv)
{
	if (argc < 2 || argv[1][0] == '-' || !IsSfs(argv[1])) {
		return -1;
	}
	const std::string in = argv[1];
	std::string       out, formsType;
	bool              describe = false, forms = false, report = false, unattached = false, undefined = false, termUndef = false;
	std::set<std::uint32_t> deleteForms;
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		auto              next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %s\n", a.c_str());
                std::exit(1);
            }
            return argv[++i];
		};
		if (a == "-o") {
			out = next();
		} else if (a == "--describe") {
			describe = true;
		} else if (a == "--forms") {
			forms = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				formsType = argv[++i];
			}
		} else if (a == "--report") {
			report = true;
		} else if (a == "--remove-unattached") {
			unattached = true;
		} else if (a == "--remove-undefined") {
			undefined = true;
		} else if (a == "--terminate-undefined") {
			termUndef = true;
		} else if (a == "--delete-form") {
			deleteForms.insert(static_cast<std::uint32_t>(std::stoul(next(), nullptr, 16)));
		} else {
			std::fprintf(stderr,
				"%s: not available for Starfield saves (they support: -o <out.sfs>, --describe, --report, --forms [TYPE], --delete-form <form id>, "
				"--remove-unattached, --remove-undefined, --terminate-undefined)\n",
				a.c_str());
			return 1;
		}
	}
	try {
		auto save = SaveFile::load(in);
		if (!save->problems.empty()) {
			std::fprintf(stderr, "warning: this save has structural problems; it may not be written back faithfully:\n");
			for (const auto& pr : save->problems) {
				std::fprintf(stderr, "  - %s\n", pr.c_str());
			}
		}
		for (const auto& l : DescribeSave(*save, describe)) {
			std::printf("%s\n", l.c_str());
		}
		if (forms) {
			for (const auto& c : save->changeForms) {
				const char* t = save->cfTypeName(c);
				if (!formsType.empty() && formsType != t) {
					continue;
				}
				const auto fid = save->formIDOf(c.refID);
				std::printf("%06X  form %08X  %-4s  %-28s flags %08X  version %u  refs %zu  strings %zu  data %zu\n", c.refID.data, fid, t,
					save->pluginOf(fid).c_str(), c.changeFlags, c.version, c.sfRefs.size(), c.sfStrings.size(), c.data.size());
			}
		}
		if (report) {
			auto r = ops::Analyze(*save);
			std::printf("%s", ops::FormatReport(r).c_str());
			if (const auto* p = save->papyrus(); p && !save->sfPapyrusRaw()) {
				for (const auto& sc : p->scripts) {
					if (ops::IsScriptUndefined(*p, sc)) {
						std::printf("undefined script: %s\n", p->str(sc.name).c_str());
					}
				}
				// references to the unattached instances from any variable (what removing them would leave dangling)
				std::set<std::uint64_t> un;
				for (const auto& i : p->instances) {
					if (ops::IsUnattached(i)) {
						un.insert(i.id);
					}
				}
				std::size_t refsTo = 0;
				for (const auto id : ops::NamedObjectIDs(*p)) {
					refsTo += un.contains(id);
				}
				std::printf("unattached instances a variable names: %zu\n", refsTo);
				for (const auto& i : p->instances) {
					if (ops::IsUnattached(i)) {
						std::printf("unattached: %016llX %s (handle kind %u, %u, data %s)\n", static_cast<unsigned long long>(i.id), p->str(i.script).c_str(),
							static_cast<unsigned>(i.unknown2bits), static_cast<unsigned>(i.unknown), i.data ? "yes" : "no");
					}
				}
			}
		}
		const bool papyrusEdits = unattached || undefined || termUndef;
		if (papyrusEdits && (!save->papyrus() || save->sfPapyrusRaw())) {
			std::fprintf(stderr, "the script data of this save could not be read: %s\n", save->sfPapyrusError.c_str());
			return 2;
		}
		bool edited = false;
		if (termUndef) {
			std::printf("terminated %zu undefined thread(s)\n", ops::TerminateUndefinedThreads(*save->papyrus()));
			edited = true;
		}
		if (unattached) {
			const auto r = ops::RemoveUnattachedInstances(*save);
			std::printf("removed %zu unattached instance(s)%s; %zu kept because a variable names them\n", r.removed,
				r.kept.empty() ? "" : (", kept " + std::to_string(r.kept.size())).c_str(), r.keptNamed);
			edited = true;
		}
		if (undefined) {
			const auto r = ops::RemoveUndefinedElements(*save);
			std::printf("removed/terminated %zu undefined element(s)%s\n", r.removed, r.kept.empty() ? "" : (", kept " + std::to_string(r.kept.size())).c_str());
			edited = true;
		}
		std::size_t removed = 0;
		if (!deleteForms.empty()) {
			std::vector<ChangeForm> keep;
			keep.reserve(save->changeForms.size());
			for (auto& c : save->changeForms) {
				if (deleteForms.contains(save->formIDOf(c.refID))) {
					std::printf("deleted change form %08X (%s)\n", save->formIDOf(c.refID), save->cfTypeName(c));
					++removed;
					continue;
				}
				keep.push_back(std::move(c));
			}
			save->changeForms = std::move(keep);
			save->reindexChangeForms();
			if (removed != deleteForms.size()) {
				std::fprintf(stderr, "warning: %zu of the form ids have no change form in this save\n", deleteForms.size() - removed);
			}
		}
		if (!out.empty()) {
			if (!save->problems.empty()) {
				std::fprintf(stderr, "not written: the save has structural problems\n");
				return 2;
			}
			std::error_code ec;
			const auto      wr = save->write(out, std::filesystem::exists(out, ec));
			std::printf("written %s%s%s\n", out.c_str(), wr.backup.empty() ? "" : " (backup ", wr.backup.empty() ? "" : (wr.backup.string() + ")").c_str());
		} else if (removed || edited) {
			std::fprintf(stderr, "nothing written: add -o <out.sfs>\n");
		}
	} catch (const std::exception& e) {
		std::fprintf(stderr, "error: %s\n", e.what());
		return 2;
	}
	return 0;
}
