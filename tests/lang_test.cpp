// Interface translation checks (rs_lang_test):
//  - every built-in catalogue (lang/<code>.txt) parses, has no text twice, and every translation keeps exactly the
//    printf conversions of its English text, in the same order (a "%s" lost or a "%zu" turned into "%d" would
//    print garbage or crash); no translation carries an ImGui "##" id
//  - tr()/trId() behave: translation, English fallback, "##" ids kept, English as the id of window titles
// That every text the code uses is in the catalogue is checked by tools/i18n/tr_keys.py --check (CTest i18n_keys).
#include "core/Lang.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace
{
	int g_fail = 0, g_pass = 0;

	void Check(bool a_ok, const std::string& a_what)
	{
		if (a_ok) {
			++g_pass;
		} else {
			++g_fail;
			std::printf("FAIL: %s\n", a_what.c_str());
		}
	}

	std::string Join(const std::vector<std::string>& a_v)
	{
		std::string s;
		for (const auto& x : a_v) {
			s += (s.empty() ? "" : " ") + x;
		}
		return s.empty() ? "(none)" : s;
	}

	void TestBuiltins()
	{
		Check(!i18n::Builtins().empty(), "at least one built-in catalogue (lang/ru.txt)");
		for (const auto& b : i18n::Builtins()) {
			i18n::Catalogue c;
			std::string     err;
			const bool      ok = i18n::Parse(std::string_view(reinterpret_cast<const char*>(b.data), b.size), c, err);
			Check(ok, std::string("lang/") + b.code + ".txt parses: " + err);
			Check(!c.name.empty(), std::string("lang/") + b.code + ".txt has a language \"name\" line");
			std::set<std::string> seen;
			std::size_t           translated = 0, specsOk = 0;
			for (const auto& e : c.entries) {
				const auto where = std::string("lang/") + b.code + ".txt line " + std::to_string(e.line) + " ";
				if (!seen.insert(e.en).second) {
					Check(false, where + "has the same English text twice: " + e.en.substr(0, 80));
				}
				if (e.tr.empty()) {
					continue;
				}
				++translated;
				const auto en = i18n::FormatSpecs(e.en), tr = i18n::FormatSpecs(e.tr);
				if (en != tr) {
					Check(false, where + "conversions differ: English " + Join(en) + ", translation " + Join(tr) + "  (" + e.en.substr(0, 60) + ")");
				} else {
					++specsOk;
				}
				if (e.tr.find("##") != std::string::npos) {
					Check(false, where + "the translation contains ## (ids are not part of the catalogue)");
				}
			}
			Check(true, std::string("lang/") + b.code + ".txt: " + std::to_string(c.entries.size()) + " texts, " + std::to_string(translated) +
			                " translated, " + std::to_string(specsOk) + " with matching conversions");
			std::printf("lang/%s.txt (%s): %zu texts, %zu translated\n", b.code, c.name.c_str(), c.entries.size(), translated);
		}
	}

	void TestFormatSpecs()
	{
		using V = std::vector<std::string>;
		Check(i18n::FormatSpecs("%zu of %zu (%d%%) %.1f MB %-28s %08X %lld") == V{ "%zu", "%zu", "%d", "%.1f", "%-28s", "%08X", "%lld" },
			"FormatSpecs reads the usual conversions and skips %%");
		Check(i18n::FormatSpecs("no conversions").empty(), "FormatSpecs: none");
	}

	void TestTr()
	{
		const auto dir = std::filesystem::temp_directory_path() / "rs_lang_test";
		std::filesystem::create_directories(dir);
		{
			std::ofstream f(dir / "xx.txt", std::ios::binary);
			f << "\xEF\xBB\xBF"  // a BOM, as Notepad writes it
			  << "language \"Test\"\n# comment\nen \"Open\"\ntr \"Otkryt\"\n\nen \"Set\"\ntr \"Zadat\"\nen \"Transplant\"\ntr \"Perenos\"\n"
			  << "en \"Two\\nlines \\\"q\\\"\"\ntr \"Dve\\nstroki \\\"q\\\"\"\nen \"Untranslated\"\ntr \"\"\n"
			  << "en \"Save\"\ntr \"Sokhranit\"\nen \"sidebar|Save\"\ntr \"Sokhranenie\"\n";
		}
		i18n::SetLangFolder(dir.string());
		bool found = false;
		for (const auto& l : i18n::Languages()) {
			found |= l.code == "xx" && l.name == "Test";
		}
		Check(found, "Languages() lists lang/xx.txt with its name");
		Check(i18n::SetLanguage("xx"), "SetLanguage(xx)");
		Check(i18n::Current() == "xx" && !i18n::IsEnglish(), "current language xx");
		Check(std::strcmp(tr("Open"), "Otkryt") == 0, "tr translates");
		Check(std::strcmp(tr("Set##gold"), "Zadat##gold") == 0, "tr keeps a ## id");
		Check(std::strcmp(tr("Set###g2"), "Zadat###g2") == 0, "tr keeps a ### id");
		Check(std::strcmp(trId("Transplant"), "Perenos###Transplant") == 0, "trId keeps the English as the id");
		Check(std::strcmp(tr("Two\nlines \"q\""), "Dve\nstroki \"q\"") == 0, "escapes");
		Check(std::strcmp(trc("sidebar", "Save"), "Sokhranenie") == 0, "trc uses the context's own translation");
		Check(std::strcmp(trc("menu", "Save"), "Sokhranit") == 0, "trc without a context entry uses the plain translation");
		const char* u = "Untranslated";
		Check(tr(u) == u, "an empty translation falls back to the English");
		const char* n = "Not in the catalogue##x";
		Check(tr(n) == n && trId("Nope") == std::string("Nope"), "unknown texts stay English");
		Check(tr("##only") == std::string("##only"), "a bare id is left alone");
		Check(tr("Set##gold") == tr("Set##gold"), "the same pointer each time");
		Check(!i18n::SetLanguage("zz") && i18n::IsEnglish() && std::strcmp(tr("Open"), "Open") == 0, "an unknown language falls back to English");
		Check(i18n::SetLanguage("en") && std::strcmp(trId("Transplant"), "Transplant") == 0, "English: trId gives the text itself");
		i18n::Catalogue c;
		std::string     err;
		Check(!i18n::Parse("en \"a\"\ntr \"b\" x\n", c, err) && err.find("line 2") != std::string::npos, "a syntax error names its line: " + err);
		std::filesystem::remove_all(dir);
		i18n::SetLangFolder("");
	}
}

int main()
{
	TestFormatSpecs();
	TestTr();
	TestBuiltins();
	std::printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
