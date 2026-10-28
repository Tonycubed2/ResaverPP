// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "SaveArchive.h"

#include "Extras.h"
#include "Save.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>

namespace ess::archive
{
	namespace
	{
		namespace fs = std::filesystem;

		std::string ToU8(const fs::path& a_p)
		{
			const auto s = a_p.u8string();
			return std::string(reinterpret_cast<const char*>(s.data()), s.size());
		}
		fs::path FromU8(const std::string& a_s)
		{
			return fs::path(std::u8string(reinterpret_cast<const char8_t*>(a_s.data()), a_s.size()));
		}
		std::int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
		std::int64_t FileTime(const fs::path& a_p)
		{
			std::error_code ec;
			const auto      t = fs::last_write_time(a_p, ec);
			if (ec) {
				return 0;
			}
			return std::chrono::duration_cast<std::chrono::seconds>(
				(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now()).time_since_epoch())
			    .count();
		}
		fs::path Cosave(const fs::path& a_ess)
		{
			auto c = a_ess;
			c.replace_extension(".skse");
			return c;
		}
		// a new, unused folder named by the current time
		fs::path NewFolder(const fs::path& a_archive)
		{
			const auto now = std::chrono::system_clock::now();
			const auto t = std::chrono::system_clock::to_time_t(now);
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
			std::tm    tm{};
#ifdef _WIN32
			localtime_s(&tm, &t);
#else
			localtime_r(&t, &tm);
#endif
			char name[64];
			std::snprintf(name, sizeof(name), "%04d%02d%02d-%02d%02d%02d-%03d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
				tm.tm_sec, static_cast<int>(ms));
			for (int i = 0;; ++i) {
				const auto dir = a_archive / (i == 0 ? std::string(name) : std::string(name) + "-" + std::to_string(i));
				std::error_code ec;
				if (!fs::exists(dir, ec)) {
					return dir;
				}
			}
		}
		void Copy(const fs::path& a_from, const fs::path& a_to)
		{
			std::error_code ec;
			fs::copy_file(a_from, a_to, fs::copy_options::overwrite_existing, ec);
			if (ec) {
				throw std::runtime_error("could not copy " + ToU8(a_from) + " to " + ToU8(a_to) + ": " + ec.message());
			}
		}
		bool IsEntry(const fs::path& a_dir) { return a_dir.filename().string().size() >= 19 && fs::exists(a_dir / "info.txt"); }

		Entry Read(const fs::path& a_dir)
		{
			Entry e;
			e.folder = a_dir;
			std::ifstream                      f(a_dir / "info.txt", std::ios::binary);
			std::map<std::string, std::string> kv;
			for (std::string line; std::getline(f, line);) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				if (const auto eq = line.find('='); eq != std::string::npos) {
					kv[line.substr(0, eq)] = line.substr(eq + 1);
				}
			}
			auto num = [&](const char* k) -> std::int64_t {
				try {
					return kv.count(k) ? std::stoll(kv[k]) : 0;
				} catch (...) {
					return 0;
				}
			};
			e.source = FromU8(kv["source"]);
			e.savedAs = FromU8(kv["savedAs"]);
			e.reason = kv["reason"];
			e.archived = num("archived");
			e.modified = num("modified");
			e.ess = a_dir / FromU8(kv["ess"]);
			if (!kv["skse"].empty()) {
				e.skse = a_dir / FromU8(kv["skse"]);
			}
			std::error_code ec;
			e.size = fs::file_size(e.ess, ec);
			if (!e.skse.empty()) {
				e.cosaveSize = fs::file_size(e.skse, ec);
			}
			const auto info = extras::ReadSaveInfo(e.ess);
			e.character = info.playerName;
			e.location = info.location;
			e.race = info.race;
			e.level = info.playerLevel;
			e.saveNumber = info.saveNumber;
			return e;
		}
	}

	std::vector<Entry> List(const fs::path& a_archive)
	{
		std::vector<Entry> out;
		std::error_code    ec;
		for (fs::directory_iterator it(a_archive, ec), end; !ec && it != end; it.increment(ec)) {
			if (it->is_directory(ec) && IsEntry(it->path())) {
				out.push_back(Read(it->path()));
			}
		}
		// folder names sort by time
		std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.folder.filename() > b.folder.filename(); });
		return out;
	}

	void Prune(const fs::path& a_archive)
	{
		const auto all = List(a_archive);
		for (std::size_t i = kKeep; i < all.size(); ++i) {
			std::error_code ec;
			fs::remove_all(all[i].folder, ec);
		}
	}

	std::optional<Entry> Add(const fs::path& a_archive, const fs::path& a_ess, const fs::path& a_savedAs, const std::string& a_reason, bool a_prune)
	{
		std::error_code ec;
		if (!fs::is_regular_file(a_ess, ec)) {
			return std::nullopt;  // nothing on disk to keep (a new file)
		}
		const auto size = fs::file_size(a_ess, ec);
		const auto modified = FileTime(a_ess);
		const auto existing = List(a_archive);
		if (!existing.empty()) {
			const auto& n = existing.front();
			if (fs::equivalent(n.source, a_ess, ec) && n.size == size && n.modified == modified) {
				return std::nullopt;  // already the newest entry, unchanged
			}
		}
		fs::create_directories(a_archive, ec);
		const auto dir = NewFolder(a_archive);
		if (!fs::create_directories(dir, ec) && ec) {
			throw std::runtime_error("could not make the archive folder " + ToU8(dir) + ": " + ec.message());
		}
		try {
			Copy(a_ess, dir / a_ess.filename());
			const auto co = Cosave(a_ess);
			const bool hasCo = fs::is_regular_file(co, ec);
			if (hasCo) {
				Copy(co, dir / co.filename());
			}
			std::ofstream f(dir / "info.txt", std::ios::binary);
			f << "source=" << ToU8(a_ess) << "\n"
			  << "savedAs=" << ToU8(a_savedAs) << "\n"
			  << "reason=" << a_reason << "\n"
			  << "archived=" << Now() << "\n"
			  << "modified=" << modified << "\n"
			  << "ess=" << ToU8(a_ess.filename()) << "\n"
			  << "skse=" << (hasCo ? ToU8(co.filename()) : std::string()) << "\n";
			f.close();
			if (!f) {
				throw std::runtime_error("could not write " + ToU8(dir / "info.txt"));
			}
		} catch (...) {
			fs::remove_all(dir, ec);
			throw;
		}
		if (a_prune) {
			Prune(a_archive);
		}
		return Read(dir);
	}

	fs::path RestoreInPlace(const fs::path& a_archive, const Entry& a_entry)
	{
		std::error_code ec;
		if (!fs::is_regular_file(a_entry.ess, ec)) {
			throw std::runtime_error("the archived save is gone: " + ToU8(a_entry.ess));
		}
		const auto target = a_entry.source;
		const auto targetCo = Cosave(target);
		fs::create_directories(target.parent_path(), ec);
		// keep what is there now, so the restore can be undone; pruned only after the copy, so restoring the
		// oldest entry cannot delete it first
		Add(a_archive, target, a_entry.ess, "Replaced by a restore", false);
		Copy(a_entry.ess, target);
		if (!a_entry.skse.empty() && fs::is_regular_file(a_entry.skse, ec)) {
			Copy(a_entry.skse, targetCo);
		} else if (fs::exists(targetCo, ec)) {
			fs::remove(targetCo, ec);  // the archived save had no co-save: the one there belongs to the replaced save (archived above)
		}
		Prune(a_archive);
		return target;
	}

	fs::path RestoreAsCopy(const Entry& a_entry)
	{
		std::error_code ec;
		if (!fs::is_regular_file(a_entry.ess, ec)) {
			throw std::runtime_error("the archived save is gone: " + ToU8(a_entry.ess));
		}
		fs::create_directories(a_entry.source.parent_path(), ec);
		const auto target = EditedSaveName(a_entry.source, "restored", true);
		Copy(a_entry.ess, target);
		if (!a_entry.skse.empty() && fs::is_regular_file(a_entry.skse, ec)) {
			Copy(a_entry.skse, Cosave(target));
		}
		return target;
	}
}
