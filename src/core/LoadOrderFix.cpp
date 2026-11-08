// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "LoadOrderFix.h"
#include "Text.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace ess::lofix
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}

		// one line of the file: its text without the line end, and the plugin it names ("" for comments / blank lines)
		struct Line
		{
			std::string text, eol, key;
		};

		std::string KeyOf(const std::string& a_line)
		{
			std::string s = a_line;
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
				s.pop_back();
			}
			std::size_t a = 0;
			while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) {
				++a;
			}
			s = s.substr(a);
			if (s.empty() || s[0] == '#') {
				return {};
			}
			if (s[0] == '*') {
				s = s.substr(1);
			}
			return Lower(s);
		}

		bool ReadFile(const fs::path& a_file, std::string& a_out)
		{
			std::ifstream f(a_file, std::ios::binary);
			if (!f) {
				return false;
			}
			std::ostringstream ss;
			ss << f.rdbuf();
			a_out = ss.str();
			return true;
		}

		std::string Stamp()
		{
			const auto now = std::time(nullptr);
			char       stamp[32];
			std::tm    tm{};
#ifdef _WIN32
			localtime_s(&tm, &now);
#else
			localtime_r(&now, &tm);
#endif
			std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
			return stamp;
		}

		std::string Name(const fs::path& a_p) { return text::ToUtf8(a_p.filename().string()); }
	}

	std::string ApplyMoves(const std::string& a_text, const std::vector<locheck::Move>& a_moves, std::vector<std::string>* a_skipped)
	{
		std::string bom, body = a_text;
		if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF && static_cast<unsigned char>(body[1]) == 0xBB &&
			static_cast<unsigned char>(body[2]) == 0xBF) {
			bom = body.substr(0, 3);
			body = body.substr(3);
		}
		std::vector<Line> lines;
		for (std::size_t at = 0; at < body.size();) {
			const auto nl = body.find('\n', at);
			Line       l;
			if (nl == std::string::npos) {
				l.text = body.substr(at);
				at = body.size();
			} else {
				l.text = body.substr(at, nl - at);
				l.eol = "\n";
				at = nl + 1;
			}
			if (!l.text.empty() && l.text.back() == '\r') {
				l.text.pop_back();
				l.eol = "\r" + l.eol;
			}
			l.key = KeyOf(l.text);
			lines.push_back(std::move(l));
		}
		// the file's own line end, for a line that moves away from the end of the file (which may have none)
		std::string eol = "\r\n";
		for (const auto& l : lines) {
			if (!l.eol.empty()) {
				eol = l.eol;
				break;
			}
		}
		auto find = [&](const std::string& a_name) -> std::ptrdiff_t {
			const auto k = Lower(a_name);
			const auto k8 = Lower(text::ToUtf8(a_name));
			for (std::size_t i = 0; i < lines.size(); ++i) {
				if (!lines[i].key.empty() && (lines[i].key == k || Lower(text::ToUtf8(lines[i].key)) == k8)) {
					return static_cast<std::ptrdiff_t>(i);
				}
			}
			return -1;
		};
		for (const auto& mv : a_moves) {
			const auto from = find(mv.plugin), to = find(mv.before);
			if (from < 0 || to < 0) {
				if (a_skipped) {
					a_skipped->push_back(mv.plugin + (from < 0 ? " (not in the file)" : " (" + mv.before + " is not in the file)"));
				}
				continue;
			}
			if (from < to) {
				continue;  // already before it in this file
			}
			const bool lastLine = static_cast<std::size_t>(from) + 1 == lines.size();
			Line       l = lines[static_cast<std::size_t>(from)];
			lines.erase(lines.begin() + from);
			if (lastLine) {
				// the line before it becomes the last one: it takes over the old last line's ending (maybe none)
				std::swap(lines.back().eol, l.eol);
				if (l.eol.empty()) {
					l.eol = eol;
				}
			}
			lines.insert(lines.begin() + to, std::move(l));
		}
		std::string out = bom;
		for (const auto& l : lines) {
			out += l.text + l.eol;
		}
		return out;
	}

	std::vector<Locked> LockedMoved(const std::string& a_lockedOrderText, const std::vector<std::string>& a_oldOrder,
		const std::vector<std::string>& a_newOrder)
	{
		auto indexIn = [](const std::vector<std::string>& v, const std::string& n) {
			const auto k = Lower(n);
			for (std::size_t i = 0; i < v.size(); ++i) {
				if (Lower(v[i]) == k) {
					return static_cast<int>(i);
				}
			}
			return -1;
		};
		std::vector<Locked> out;
		std::istringstream  in(a_lockedOrderText);
		for (std::string line; std::getline(in, line);) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line.empty() || line[0] == '#') {
				continue;
			}
			// MO2's PluginList::readLockedOrderFrom: "name|priority", exactly two fields
			const auto bar = line.find('|');
			if (bar == std::string::npos || line.find('|', bar + 1) != std::string::npos) {
				continue;
			}
			const auto name = line.substr(0, bar);
			const int  was = indexIn(a_oldOrder, name), now = indexIn(a_newOrder, name);
			if (was >= 0 && was != now) {
				Locked l;
				l.plugin = name;
				try {
					l.index = std::stoi(line.substr(bar + 1));
				} catch (...) {
					l.index = -1;
				}
				l.newIndex = now;
				out.push_back(l);
			}
		}
		return out;
	}

	Targets TargetsFor(const fs::path& a_pluginsTxt, Manager a_manager)
	{
		Targets t;
		t.manager = a_manager;
		std::error_code ec;
		t.files.push_back(a_pluginsTxt);
		if (const auto lo = a_pluginsTxt.parent_path() / "loadorder.txt"; fs::exists(lo, ec)) {
			t.files.push_back(lo);
		}
		if (a_manager == Manager::kMo2) {
			if (const auto lk = a_pluginsTxt.parent_path() / "lockedorder.txt"; fs::exists(lk, ec)) {
				t.lockedOrder = lk;
			}
		}
		return t;
	}

	WriteResult Write(const Targets& a_targets, const locheck::FixPlan& a_plan, const std::vector<std::string>& a_oldOrder, bool a_dryRun)
	{
		WriteResult r;
		if (!a_plan.ok) {
			r.message = a_plan.error.empty() ? "There is no plan to write." : a_plan.error;
			return r;
		}
		if (a_plan.moves.empty()) {
			r.ok = true;
			r.message = "No master loads after a plugin that needs it: nothing to change.";
			return r;
		}
		if (!a_targets.lockedOrder.empty()) {
			std::string text;
			if (ReadFile(a_targets.lockedOrder, text)) {
				const auto moved = LockedMoved(text, a_oldOrder, a_plan.order);
				if (!moved.empty()) {
					std::string list;
					for (const auto& l : moved) {
						list += (list.empty() ? "" : ", ") + l.plugin + " (locked at " + std::to_string(l.index) + ", would be " + std::to_string(l.newIndex) + ")";
					}
					r.message = "Mod Organizer has locked the load order position of " + list + " (" + text::ToUtf8(a_targets.lockedOrder.string()) +
					            "). It would put them back where they are locked, so nothing was changed. Unlock them in Mod Organizer's Plugins "
					            "tab (the lock icon), then try again.";
					return r;
				}
			}
		}
		// read and change every file first: nothing is written when one of them cannot be read
		for (const auto& file : a_targets.files) {
			FileResult fr;
			fr.file = file;
			if (!ReadFile(file, fr.before)) {
				r.message = "Could not read " + text::ToUtf8(file.string()) + ". Nothing was changed.";
				return r;
			}
			fr.after = ApplyMoves(fr.before, a_plan.moves, &fr.skipped);
			fr.changed = fr.after != fr.before;
			r.files.push_back(std::move(fr));
		}
		if (a_dryRun) {
			r.ok = true;
			r.message = "Dry run: nothing was written.";
			return r;
		}
		const auto stamp = Stamp();
		std::error_code ec;
		auto undo = [&](std::size_t a_upTo) {
			for (std::size_t k = 0; k < a_upTo; ++k) {
				auto& fr = r.files[k];
				if (fr.changed && !fr.backup.empty()) {
					std::error_code e2;
					fs::copy_file(fr.backup, fr.file, fs::copy_options::overwrite_existing, e2);
				}
			}
		};
		for (std::size_t k = 0; k < r.files.size(); ++k) {
			auto& fr = r.files[k];
			if (!fr.changed) {
				continue;
			}
			for (int n = 1;; ++n) {  // a name not taken: a backup is never replaced
				fr.backup = fr.file;
				fr.backup += "." + stamp + (n > 1 ? "-" + std::to_string(n) : std::string()) + ".resaverpp.bak";
				if (!fs::exists(fr.backup, ec)) {
					break;
				}
			}
			fs::copy_file(fr.file, fr.backup, fs::copy_options::none, ec);
			if (ec) {
				r.message = "Could not back up " + text::ToUtf8(fr.file.string()) + ": " + ec.message() + ". Nothing was changed.";
				fr.backup.clear();
				undo(k);
				return r;
			}
			auto tmp = fr.file;
			tmp += ".resaverpp.tmp";
			{
				std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
				f.write(fr.after.data(), static_cast<std::streamsize>(fr.after.size()));
				f.close();
				if (!f) {
					fs::remove(tmp, ec);
					r.message = "Could not write " + text::ToUtf8(tmp.string()) + ". Nothing was changed.";
					undo(k);
					return r;
				}
			}
			fs::rename(tmp, fr.file, ec);
			if (ec) {
				fs::remove(tmp, ec);
				r.message = "Could not replace " + text::ToUtf8(fr.file.string()) + " (is a program using it?). Nothing was changed.";
				undo(k);
				return r;
			}
			std::string back;
			if (!ReadFile(fr.file, back) || back != fr.after) {
				r.message = "The new " + Name(fr.file) + " could not be read back; the originals were put back.";
				undo(k + 1);
				return r;
			}
		}
		r.ok = true;
		std::string written, backups;
		for (const auto& fr : r.files) {
			if (fr.changed) {
				written += (written.empty() ? "" : ", ") + text::ToUtf8(fr.file.string());
				backups += (backups.empty() ? "" : ", ") + Name(fr.backup);
			}
		}
		r.message = written.empty() ? "The files already have this order: nothing was written."
		                            : "Written: " + written + ". The originals are kept next to them as " + backups + ".";
		return r;
	}

	std::string Diff(const std::string& a_before, const std::string& a_after, const std::string& a_name)
	{
		auto split = [](const std::string& s) {
			std::vector<std::string> v;
			std::istringstream       in(s);
			for (std::string l; std::getline(in, l);) {
				if (!l.empty() && l.back() == '\r') {
					l.pop_back();
				}
				v.push_back(l);
			}
			return v;
		};
		const auto a = split(a_before), b = split(a_after);
		// longest common subsequence (the files are a few thousand lines at most)
		const auto n = a.size(), m = b.size();
		std::vector<std::vector<std::uint32_t>> L(n + 1, std::vector<std::uint32_t>(m + 1, 0));
		for (std::size_t i = n; i-- > 0;) {
			for (std::size_t j = m; j-- > 0;) {
				L[i][j] = a[i] == b[j] ? L[i + 1][j + 1] + 1 : std::max(L[i + 1][j], L[i][j + 1]);
			}
		}
		struct Op
		{
			char        c;
			std::string t;
			std::size_t ai, bi;
		};
		std::vector<Op> ops;
		std::size_t     i = 0, j = 0;
		while (i < n || j < m) {
			if (i < n && j < m && a[i] == b[j]) {
				ops.push_back({ ' ', a[i], i, j });
				++i;
				++j;
			} else if (j < m && (i == n || L[i][j + 1] >= L[i + 1][j])) {
				ops.push_back({ '+', b[j], i, j });
				++j;
			} else {
				ops.push_back({ '-', a[i], i, j });
				++i;
			}
		}
		std::string out = "--- " + a_name + " (before)\n+++ " + a_name + " (after)\n";
		std::vector<bool> show(ops.size(), false);
		for (std::size_t k = 0; k < ops.size(); ++k) {
			if (ops[k].c != ' ') {
				for (std::size_t q = k > 0 ? k - 1 : 0; q <= std::min(ops.size() - 1, k + 1); ++q) {
					show[q] = true;
				}
			}
		}
		bool gap = true;
		for (std::size_t k = 0; k < ops.size(); ++k) {
			if (!show[k]) {
				gap = true;
				continue;
			}
			if (gap) {
				out += "@@ line " + std::to_string(ops[k].ai + 1) + " @@\n";
				gap = false;
			}
			out += std::string(1, ops[k].c) + ops[k].t + "\n";
		}
		return out;
	}

	std::string AfterWriteNote(Manager a_manager, bool a_underMo2, bool a_mo2Running)
	{
		switch (a_manager) {
		case Manager::kMo2:
			if (a_underMo2) {
				return "ReSaver Plus was started from Mod Organizer: Mod Organizer reads the new order when ReSaver Plus closes (it refreshes "
				       "after a program it started ends). Close ReSaver Plus before you change anything in Mod Organizer or start the game. "
				       "If you pressed Unlock in Mod Organizer while ReSaver Plus was running, press Refresh (F5) in Mod Organizer after "
				       "closing ReSaver Plus.";
			}
			if (a_mo2Running) {
				return "Mod Organizer is open: press Refresh (F5) in it now, before you change anything there or close it. Otherwise it "
				       "writes its old order back (it saves its own list when something changes and when it closes).";
			}
			return "Mod Organizer reads the new order when it starts (if it is open, press Refresh (F5) in it now, before you change "
			       "anything there or close it, or it writes its old order back).";
		case Manager::kVortex:
			return "Vortex keeps its load order in this plugins.txt and reads it again when another program changes it (if Vortex is open, "
			       "it shows the new order within a second). When Vortex sorts the plugins with LOOT (automatic sorting), a later sort can "
			       "move plugins again; LOOT also keeps every master before the plugins that need it.";
		default:
			return "The game reads plugins.txt when it starts.";
		}
	}
}
