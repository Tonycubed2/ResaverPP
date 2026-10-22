// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Report.h"

#include "ChangeFormData.h"
#include "Ops.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <unordered_map>

namespace ess::report
{
	namespace
	{
		template <class... A>
		std::string Fmt(const char* f, A... a)
		{
			char buf[1024];
			std::snprintf(buf, sizeof(buf), f, a...);
			return buf;
		}

		std::string Pad(std::string s, std::size_t w)
		{
			if (s.size() > w) {
				s = s.substr(0, w - 1) + "~";
			}
			s.resize(w, ' ');
			return s;
		}

		const char* KindText(mods::Kind k)
		{
			switch (k) {
			case mods::Kind::kPlugin: return "plugin";
			case mods::Kind::kLight: return "light plugin";
			case mods::Kind::kCreated: return "objects spawned in game";
			case mods::Kind::kUnattached: return "attached to nothing";
			case mods::Kind::kGlobal: return "no object / unknown mod";
			case mods::Kind::kFolder: return "mod folder (no plugin)";
			}
			return "";
		}

		std::string Index(const mods::ModRow& r)
		{
			if (r.kind == mods::Kind::kPlugin) {
				return Fmt("%02X", r.index);
			}
			if (r.kind == mods::Kind::kLight) {
				return Fmt("FE:%03X", r.index);
			}
			return "";
		}

		void Wrap(std::vector<std::string>& out, const std::string& prefix, const std::string& text, std::size_t width = 110)
		{
			std::string line = prefix;
			std::size_t start = 0;
			while (start < text.size()) {
				auto end = text.find(' ', start);
				if (end == std::string::npos) {
					end = text.size();
				}
				const auto word = text.substr(start, end - start);
				if (line.size() + word.size() + 1 > width && line.size() > prefix.size()) {
					out.push_back(line);
					line = std::string(prefix.size(), ' ');
				}
				line += (line.size() > prefix.size() && line.back() != ' ' ? " " : "") + word;
				start = end + 1;
			}
			out.push_back(line);
		}
	}

	std::vector<std::string> Build(SaveFile& a_save, const mods::Table& a_mods, const PluginDB* a_names, const Options& a_opt,
		const std::string& a_saveName)
	{
		std::vector<std::string> L;
		const auto&              h = a_save.header;
		const auto               now = std::time(nullptr);
		char                     when[64];
		std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&now));

		L.push_back("#ReSaver++ save report");
		L.push_back("Save:      " + a_saveName);
		L.push_back(Fmt("Character: %s, level %u, %s", h.playerName.c_str(), h.playerLevel, h.location.c_str()));
		L.push_back("Game date: " + h.gameDate);
		L.push_back(Fmt("Plugins:   %zu full + %zu light", a_save.plugins.full.size(), a_save.plugins.lite.size()));
		L.push_back(std::string("Report:    ") + when + (a_mods.hasSources ? "  (scripts credited using the scanned mods folder)" : ""));
		L.push_back("");

		auto* p = a_save.papyrus();
		auto  formText = [&](std::uint32_t id) {
            std::string s = Fmt("%08X", id);
            if (a_names) {
                const auto d = a_names->describe(id);
                if (!d.empty()) {
                    s += " " + d;
                }
            }
            return s;
		};

		if (a_opt.mods) {
			L.push_back("#Part 1 - Mods and the scripts they run");
			std::vector<const mods::ModRow*> rows;
			std::size_t                      totalInst = 0, totalThreads = 0;
			for (const auto& r : a_mods.rows) {
				if (r.scriptCount || r.threadCount || r.instanceCount) {
					rows.push_back(&r);
					totalInst += r.instanceCount;
					totalThreads += r.threadCount;
				}
			}
			L.push_back(Fmt("%zu mods have scripts in this save: %zu script instances, %zu running threads.", rows.size(), totalInst, totalThreads));
			L.push_back("Instances count the copies of each mod's own scripts; engine bindings (Form, Quest, ReferenceAlias, ...) are listed");
			L.push_back("under each mod but not counted. Running = threads active in the save right now.");
			L.push_back("");
			L.push_back("##Summary");
			L.push_back(Pad("Load", 8) + Pad("Mod", 58) + Pad("Scripts", 9) + Pad("Instances", 11) + "Running");
			for (const auto* r : rows) {
				L.push_back(Pad(Index(*r), 8) + Pad(r->name, 58) + Pad(std::to_string(r->scriptCount), 9) + Pad(std::to_string(r->instanceCount), 11) +
							std::to_string(r->threadCount));
			}
			L.push_back("");
			L.push_back("##Details");
			for (const auto* r : rows) {
				L.push_back("");
				L.push_back("##" + r->name + "   (" + KindText(r->kind) + (Index(*r).empty() ? "" : ", load index " + Index(*r)) + ")");
				L.push_back(Fmt("%zu scripts on its objects | %zu instances | %zu running threads", r->scriptCount, r->instanceCount, r->threadCount));
				std::vector<const mods::ScriptRow*> list;
				for (const auto& [k, s] : r->scripts) {
					list.push_back(&s);
				}
				std::sort(list.begin(), list.end(), [](auto a, auto b) {
					if (a->engineType != b->engineType) {
						return b->engineType;
					}
					if (a->threads.size() != b->threads.size()) {
						return a->threads.size() > b->threads.size();
					}
					return a->instances.size() > b->instances.size();
				});
				L.push_back("    " + Pad("Script", 52) + Pad("Instances", 11) + Pad("Running", 9) + (a_mods.hasSources ? ".pex from" : ""));
				for (const auto* s : list) {
					L.push_back("    " + Pad(s->script + (s->engineType ? " (engine)" : ""), 52) + Pad(std::to_string(s->instances.size()), 11) +
								Pad(std::to_string(s->threads.size()), 9) + (a_mods.hasSources ? (s->provider.empty() ? "?" : s->provider) : ""));
				}
				if (!r->provided.empty()) {
					std::string all;
					for (const auto& s : r->provided) {
						all += (all.empty() ? "" : ", ") + s;
					}
					Wrap(L, "    Provides (.pex): ", all);
				}
			}
			std::string none;
			std::size_t noneCount = 0;
			for (const auto& r : a_mods.rows) {
				if ((r.kind == mods::Kind::kPlugin || r.kind == mods::Kind::kLight) && !r.scriptCount && !r.threadCount && !r.instanceCount) {
					none += (none.empty() ? "" : ", ") + r.name;
					++noneCount;
				}
			}
			L.push_back("");
			L.push_back(Fmt("##Plugins without scripts in this save (%zu)", noneCount));
			Wrap(L, "    ", none);
			L.push_back("");
		}

		if (a_opt.anomalies && p) {
			const auto rep = ops::Analyze(a_save);
			L.push_back("#Part 2 - Anomalies");
			L.push_back("##Findings");
			if (rep.warnings.empty()) {
				L.push_back("    No problems found by the analysis.");
			}
			for (const auto& w : rep.warnings) {
				Wrap(L, "  * ", w);
			}
			L.push_back("");
			L.push_back("##Counts");
			L.push_back(Fmt("    Script instances %zu: unattached %zu, undefined %zu, on missing spawned objects %zu", rep.instances, rep.unattached,
				rep.undefinedInstances, rep.createdWithoutForm));
			L.push_back(Fmt("    Scripts %zu: undefined (.pex missing) %zu", rep.scripts, rep.undefinedScripts));
			L.push_back(Fmt("    Threads %zu: undefined %zu, terminated %zu", rep.threads, rep.undefinedThreads, rep.terminatedThreads));
			L.push_back(Fmt("    Suspended stacks %zu, function messages %zu, arrays %zu, change forms %zu, strings %zu", rep.suspended,
				rep.functionMessages, rep.arrays, rep.changeForms, rep.strings));
			if (const auto mcm = ops::CountMcmJam(*p); rep.mcmJam) {
				L.push_back(Fmt("    SkyUI MCM registration in progress: %zu threads (Clean > Fix SkyUI MCM registration jam)", mcm));
			}

			// undefined scripts, unattached / missing-object instances by script
			std::map<std::string, std::size_t> undefinedInst, unattached, missing;
			for (const auto& i : p->instances) {
				const auto& n = p->str(i.script);
				if (ops::IsInstanceUndefined(*p, i.script)) {
					undefinedInst[n]++;
				}
				if (ops::IsUnattached(i)) {
					unattached[n]++;
				}
				if (ops::IsCreatedWithoutForm(a_save, i)) {
					missing[n]++;
				}
			}
			auto listCounts = [&](const char* title, const std::map<std::string, std::size_t>& m, const char* note) {
				if (m.empty()) {
					return;
				}
				std::vector<std::pair<std::size_t, std::string>> v;
				std::size_t                                      total = 0;
				for (const auto& [k, n] : m) {
					v.push_back({ n, k });
					total += n;
				}
				std::sort(v.rbegin(), v.rend());
				L.push_back("");
				L.push_back(Fmt("##%s (%zu instances, %zu scripts)", title, total, v.size()));
				Wrap(L, "    ", note);
				for (std::size_t i = 0; i < v.size() && i < 200; ++i) {
					L.push_back("    " + Pad(std::to_string(v[i].first), 8) + v[i].second);
				}
				if (v.size() > 200) {
					L.push_back(Fmt("    ... and %zu more", v.size() - 200));
				}
			};
			listCounts("Instances of undefined scripts", undefinedInst,
				"The game could not load these scripts' .pex when the save was made (mod removed?). Clean > Remove undefined elements.");
			listCounts("Unattached instances", unattached, "Attached to nothing. Clean > Remove unattached instances.");
			listCounts("Instances on spawned objects that no longer exist", missing,
				"Clean > Remove instances on non-existent created forms (some mods keep these on purpose).");

			std::vector<std::string> undefThreads, termThreads;
			for (const auto& a : p->activeScripts) {
				if (!a.data || a.data->frames.empty()) {
					continue;
				}
				const auto& f0 = a.data->frames.front();
				const auto  name = Fmt("thread %u: %s.%s", a.id, p->str(f0.scriptName).c_str(), p->str(f0.event).c_str());
				if (ops::IsThreadUndefined(*p, a)) {
					undefThreads.push_back(name);
				}
				if (ops::IsThreadTerminated(*p, a)) {
					termThreads.push_back(name);
				}
			}
			auto listLines = [&](const std::string& title, const std::vector<std::string>& v) {
				if (v.empty()) {
					return;
				}
				L.push_back("");
				L.push_back(Fmt("##%s (%zu)", title.c_str(), v.size()));
				for (std::size_t i = 0; i < v.size() && i < 200; ++i) {
					L.push_back("    " + v[i]);
				}
				if (v.size() > 200) {
					L.push_back(Fmt("    ... and %zu more", v.size() - 200));
				}
			};
			listLines("Threads running undefined scripts", undefThreads);
			listLines("Terminated threads (still in the save)", termThreads);
			listLines("Scripts looping in Utility.Wait (20+ copies)", rep.loopingScripts);

			std::vector<std::string> nullLists;
			for (const auto& cf : a_save.changeForms) {
				if (const auto n = cf::NullEntries(a_save, cf)) {
					nullLists.push_back(Fmt("%zu null entries: ", n) + formText(a_save.formIDOf(cf.refID)));
				}
			}
			listLines("Form lists with null entries (Clean > Purify FormLists)", nullLists);

			auto top = [&](const char* title, const std::vector<ops::Count>& v) {
				if (v.empty()) {
					return;
				}
				L.push_back("");
				L.push_back(std::string("##") + title);
				for (std::size_t i = 0; i < v.size() && i < 20; ++i) {
					L.push_back("    " + Pad(std::to_string(v[i].n), 8) + v[i].name);
				}
			};
			top("Most script instances (top 20)", rep.instancesByScript);
			top("Threads by event script (top 20)", rep.threadsByTopScript);
			top("Threads by what they wait in (top 20)", rep.threadsByCurrentFrame);
			top("Suspended stacks by function (top 20)", rep.suspendedByFunction);
			top("Function messages by function (top 20)", rep.messagesByFunction);
		}
		return L;
	}

	void WriteText(const std::filesystem::path& a_path, const std::vector<std::string>& a_lines)
	{
		std::ofstream f(a_path, std::ios::binary | std::ios::trunc);
		if (!f) {
			throw std::runtime_error("cannot write the report file");
		}
		for (const auto& l : a_lines) {
			if (l.rfind("##", 0) == 0) {
				f << "\r\n" << l.substr(2) << "\r\n" << std::string(std::min<std::size_t>(l.size() - 2, 100), '-') << "\r\n";
			} else if (!l.empty() && l[0] == '#') {
				f << std::string(100, '=') << "\r\n" << l.substr(1) << "\r\n" << std::string(100, '=') << "\r\n";
			} else {
				f << l << "\r\n";
			}
		}
	}

	// ---- minimal PDF writer: US Letter pages, Courier, WinAnsi text ---------------------------
	namespace
	{
		// UTF-8 -> Windows-1252 (unknown characters become '?')
		std::string ToAnsi(const std::string& s)
		{
			std::string out;
			for (std::size_t i = 0; i < s.size();) {
				const auto c = static_cast<unsigned char>(s[i]);
				char32_t   u;
				int        n;
				if (c < 0x80) {
					u = c;
					n = 1;
				} else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
					u = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
					n = 2;
				} else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
					u = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
					n = 3;
				} else {
					u = '?';
					n = (c & 0xF8) == 0xF0 ? 4 : 1;
				}
				i += n;
				if (u < 0x80 || (u >= 0xA0 && u <= 0xFF)) {
					out.push_back(static_cast<char>(u));
				} else {
					static const std::pair<char32_t, unsigned char> map[] = { { 0x2013, 0x96 }, { 0x2014, 0x97 }, { 0x2018, 0x91 }, { 0x2019, 0x92 },
						{ 0x201C, 0x93 }, { 0x201D, 0x94 }, { 0x2022, 0x95 }, { 0x2026, 0x85 }, { 0x20AC, 0x80 } };
					char r = '?';
					for (const auto& [k, v] : map) {
						if (k == u) {
							r = static_cast<char>(v);
						}
					}
					out.push_back(r);
				}
			}
			return out;
		}

		std::string Esc(const std::string& s)
		{
			std::string o;
			for (char c : s) {
				if (c == '(' || c == ')' || c == '\\') {
					o.push_back('\\');
				}
				o.push_back(c);
			}
			return o;
		}
	}

	void WritePdf(const std::filesystem::path& a_path, const std::string& a_title, const std::vector<std::string>& a_lines)
	{
		constexpr double kW = 612, kH = 792, kMargin = 36, kSize = 7.6, kLead = 9.2;
		constexpr std::size_t kCols = 140;  // Courier is 0.6 em wide: 540pt / (0.6 * 7.6pt) ~ 118; headings use the same grid
		const std::size_t     perPage = static_cast<std::size_t>((kH - 2 * kMargin - 14) / kLead);

		// lay out: (font, text) per line, wrapped to the page width
		struct Line
		{
			int         font;  // 1 regular, 2 bold
			std::string text;
		};
		std::vector<Line> lines;
		const std::size_t width = static_cast<std::size_t>((kW - 2 * kMargin) / (0.6 * kSize));
		(void)kCols;
		for (const auto& raw : a_lines) {
			int         font = 1;
			std::string t = raw;
			if (t.rfind("##", 0) == 0) {
				font = 2;
				t = t.substr(2);
				lines.push_back({ 1, "" });
			} else if (!t.empty() && t[0] == '#') {
				font = 2;
				t = t.substr(1);
				lines.push_back({ 1, "" });
				t = "== " + t + " ==";
			}
			t = ToAnsi(t);
			if (t.empty()) {
				lines.push_back({ font, "" });
			}
			while (!t.empty()) {
				lines.push_back({ font, t.substr(0, width) });
				t = t.size() > width ? "      " + t.substr(width) : "";
			}
		}
		const std::size_t pages = std::max<std::size_t>(1, (lines.size() + perPage - 1) / perPage);

		std::string              pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
		std::vector<std::size_t> offsets;
		auto                     obj = [&](const std::string& body) {
            offsets.push_back(pdf.size());
            pdf += std::to_string(offsets.size()) + " 0 obj\n" + body + "\nendobj\n";
		};
		// 1 catalog, 2 pages, 3 font regular, 4 font bold, then page/content pairs
		obj("<< /Type /Catalog /Pages 2 0 R >>");
		std::string kids;
		for (std::size_t i = 0; i < pages; ++i) {
			kids += std::to_string(5 + i * 2) + " 0 R ";
		}
		obj("<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) + " >>");
		obj("<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>");
		obj("<< /Type /Font /Subtype /Type1 /BaseFont /Courier-Bold /Encoding /WinAnsiEncoding >>");
		for (std::size_t pg = 0; pg < pages; ++pg) {
			std::string s = "BT\n";
			char        buf[128];
			// footer
			std::snprintf(buf, sizeof(buf), "/F1 7 Tf 1 0 0 1 %.1f %.1f Tm\n", kMargin, kMargin - 14);
			s += buf;
			s += "(" + Esc(ToAnsi(a_title)) + Fmt("   -   page %zu of %zu", pg + 1, pages) + ") Tj\n";
			int curFont = 0;
			for (std::size_t i = 0; i < perPage; ++i) {
				const auto idx = pg * perPage + i;
				if (idx >= lines.size()) {
					break;
				}
				const auto& l = lines[idx];
				if (l.text.empty()) {
					continue;
				}
				if (l.font != curFont) {
					std::snprintf(buf, sizeof(buf), "/F%d %.1f Tf\n", l.font, kSize);
					s += buf;
					curFont = l.font;
				}
				std::snprintf(buf, sizeof(buf), "1 0 0 1 %.1f %.1f Tm\n", kMargin, kH - kMargin - kLead * double(i + 1));
				s += buf;
				s += "(" + Esc(l.text) + ") Tj\n";
			}
			s += "ET";
			const auto pageNo = 5 + pg * 2;
			obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " +
				std::to_string(pageNo + 1) + " 0 R >>");
			obj("<< /Length " + std::to_string(s.size()) + " >>\nstream\n" + s + "\nendstream");
		}
		// info + xref
		obj("<< /Title (" + Esc(ToAnsi(a_title)) + ") /Producer (ReSaver++) >>");
		const auto xref = pdf.size();
		pdf += "xref\n0 " + std::to_string(offsets.size() + 1) + "\n0000000000 65535 f \n";
		for (auto o : offsets) {
			char b[32];
			std::snprintf(b, sizeof(b), "%010zu 00000 n \n", o);
			pdf += b;
		}
		pdf += "trailer\n<< /Size " + std::to_string(offsets.size() + 1) + " /Root 1 0 R /Info " + std::to_string(offsets.size()) +
			   " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
		std::ofstream f(a_path, std::ios::binary | std::ios::trunc);
		if (!f || !f.write(pdf.data(), static_cast<std::streamsize>(pdf.size()))) {
			throw std::runtime_error("cannot write the PDF file");
		}
	}
}
