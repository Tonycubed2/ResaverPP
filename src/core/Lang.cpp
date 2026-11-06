#include "Lang.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace i18n
{
	namespace
	{
		// one loaded language; never freed, so the pointers Tr hands out stay valid
		struct Table
		{
			std::string                                           code;
			std::list<std::string>                                store;  // the texts the views below point into
			std::unordered_map<std::string_view, const char*>     map;    // English -> translation
			std::mutex                                            m;
			std::unordered_map<std::string, std::string>          composed;  // "Set##gold" -> "Задать##gold"
			std::unordered_map<std::string, std::string>          ids;       // "Transplant" -> "Перенос###Transplant"
			std::string                                           note;
			LoadInfo                                              info;
		};

		std::atomic<Table*> g_table{ nullptr };
		std::string         g_current = "en";
		std::string         g_folder;
		std::mutex          g_loadMutex;

		const std::string& English()
		{
			static const std::string s = "en";
			return s;
		}

		bool ReadQuoted(std::string_view a_s, std::size_t& a_pos, std::string& a_out, std::string& a_err)
		{
			while (a_pos < a_s.size() && (a_s[a_pos] == ' ' || a_s[a_pos] == '\t')) {
				++a_pos;
			}
			if (a_pos >= a_s.size() || a_s[a_pos] != '"') {
				a_err = "a text in \"quotes\" was expected";
				return false;
			}
			++a_pos;
			a_out.clear();
			while (a_pos < a_s.size()) {
				const char c = a_s[a_pos++];
				if (c == '"') {
					// only spaces (or a comment) may follow
					while (a_pos < a_s.size() && (a_s[a_pos] == ' ' || a_s[a_pos] == '\t' || a_s[a_pos] == '\r')) {
						++a_pos;
					}
					if (a_pos < a_s.size() && a_s[a_pos] != '#') {
						a_err = "something follows the closing quote (write \\\" for a quote inside the text)";
						return false;
					}
					return true;
				}
				if (c == '\\' && a_pos < a_s.size()) {
					const char n = a_s[a_pos++];
					switch (n) {
					case 'n': a_out += '\n'; break;
					case 't': a_out += '\t'; break;
					case 'r': a_out += '\r'; break;
					case '"': a_out += '"'; break;
					case '\\': a_out += '\\'; break;
					case '0': a_out += '\0'; break;
					default:
						a_err = std::string("unknown escape \\") + n + " (use \\n, \\t, \\\" or \\\\)";
						return false;
					}
					continue;
				}
				a_out += c;
			}
			a_err = "the closing quote is missing";
			return false;
		}

		std::string ReadFile(const std::filesystem::path& a_p)
		{
			std::ifstream f(a_p, std::ios::binary);
			if (!f) {
				return {};
			}
			std::ostringstream ss;
			ss << f.rdbuf();
			return ss.str();
		}

		std::filesystem::path FolderPath()
		{
			return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(g_folder.data()), g_folder.size()));
		}

		bool ValidCode(const std::string& a_code)
		{
			return !a_code.empty() && a_code.size() <= 16 &&
			       std::all_of(a_code.begin(), a_code.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; });
		}

		const char* Lookup(Table* a_t, std::string_view a_en)
		{
			const auto it = a_t->map.find(a_en);
			return it == a_t->map.end() ? nullptr : it->second;
		}
	}

	bool Parse(std::string_view a_text, Catalogue& a_out, std::string& a_error)
	{
		if (a_text.size() >= 3 && static_cast<unsigned char>(a_text[0]) == 0xEF && static_cast<unsigned char>(a_text[1]) == 0xBB &&
			static_cast<unsigned char>(a_text[2]) == 0xBF) {
			a_text.remove_prefix(3);  // UTF-8 BOM (Notepad)
		}
		std::size_t pos = 0;
		int         lineNo = 0;
		bool        haveEn = false;
		Entry       cur;
		while (pos < a_text.size()) {
			auto end = a_text.find('\n', pos);
			if (end == std::string_view::npos) {
				end = a_text.size();
			}
			std::string_view line = a_text.substr(pos, end - pos);
			pos = end + 1;
			++lineNo;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
				line.remove_suffix(1);
			}
			std::size_t i = 0;
			while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
				++i;
			}
			if (i >= line.size() || line[i] == '#') {
				continue;
			}
			std::size_t kw = i;
			while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '"') {
				++i;
			}
			const std::string key(line.substr(kw, i - kw));
			std::string       value, err;
			if (!ReadQuoted(line, i, value, err)) {
				a_error = "line " + std::to_string(lineNo) + ": " + err;
				return false;
			}
			if (key == "language") {
				a_out.name = value;
			} else if (key == "code") {
				a_out.code = value;
			} else if (key == "en") {
				if (haveEn) {
					a_out.entries.push_back(cur);  // an English text without a translation line: not translated
				}
				cur = Entry{ value, {}, lineNo };
				haveEn = true;
			} else if (key == "tr") {
				if (!haveEn) {
					a_error = "line " + std::to_string(lineNo) + ": a tr line without the en line before it";
					return false;
				}
				cur.tr = value;
				a_out.entries.push_back(cur);
				haveEn = false;
			} else {
				a_error = "line " + std::to_string(lineNo) + ": unknown word \"" + key + "\" (lines start with en, tr, language or #)";
				return false;
			}
		}
		if (haveEn) {
			a_out.entries.push_back(cur);
		}
		return true;
	}

	std::vector<Language> Languages()
	{
		std::vector<Language> v{ { "en", "English" } };
		auto add = [&](const std::string& a_code, const std::string& a_name) {
			if (std::none_of(v.begin(), v.end(), [&](const Language& l) { return l.code == a_code; })) {
				v.push_back({ a_code, a_name.empty() ? a_code : a_name });
			}
		};
		for (const auto& b : Builtins()) {
			Catalogue   c;
			std::string err;
			Parse(std::string_view(reinterpret_cast<const char*>(b.data), b.size), c, err);
			add(b.code, c.name);
		}
		if (!g_folder.empty()) {
			std::error_code ec;
			for (const auto& e : std::filesystem::directory_iterator(FolderPath(), ec)) {
				if (!e.is_regular_file(ec) || e.path().extension() != ".txt") {
					continue;
				}
				const auto code = e.path().stem().string();
				if (!ValidCode(code) || code == "en") {
					continue;
				}
				Catalogue   c;
				std::string err;
				Parse(ReadFile(e.path()), c, err);
				add(code, c.name);
			}
		}
		return v;
	}

	void SetLangFolder(const std::string& a_utf8Folder) { g_folder = a_utf8Folder; }

	bool SetLanguage(const std::string& a_code)
	{
		std::lock_guard l(g_loadMutex);
		if (a_code.empty() || a_code == "en" || !ValidCode(a_code)) {
			g_table.store(nullptr);
			g_current = "en";
			return a_code.empty() || a_code == "en";
		}
		auto* t = new Table;  // (never freed: Tr's pointers into it must stay valid)
		t->code = a_code;
		std::size_t builtin = 0, fromFile = 0;
		bool        known = false;
		auto        load = [&](std::string_view a_text, std::size_t& a_count, std::string& a_err) {
            Catalogue c;
            const bool ok = Parse(a_text, c, a_err);
            for (auto& e : c.entries) {
                if (e.tr.empty() || e.en.empty()) {
                    continue;
                }
                auto& en = t->store.emplace_back(std::move(e.en));
                auto& tx = t->store.emplace_back(std::move(e.tr));
                t->map[en] = tx.c_str();
                ++a_count;
            }
            return ok;
		};
		std::string note;
		for (const auto& b : Builtins()) {
			if (a_code == b.code) {
				std::string err;
				load(std::string_view(reinterpret_cast<const char*>(b.data), b.size), builtin, err);
				known = true;
				note = std::to_string(builtin) + " texts built in";
				if (!err.empty()) {
					note += " (built-in catalogue: " + err + ")";
				}
			}
		}
		if (!g_folder.empty()) {
			const auto p = FolderPath() / (a_code + ".txt");
			std::error_code ec;
			if (std::filesystem::is_regular_file(p, ec)) {
				std::string err;
				const bool  ok = load(ReadFile(p), fromFile, err);
				known = true;
				t->info.file = "lang/" + a_code + ".txt";
				if (!ok) {
					t->info.error = err;
				}
				note += std::string(note.empty() ? "" : ", ") + std::to_string(fromFile) + " from lang/" + a_code + ".txt";
				if (!ok) {
					note += " (stopped at an error, " + err + ")";
				}
			}
		}
		if (!known) {
			delete t;
			g_table.store(nullptr);
			g_current = "en";
			return false;
		}
		t->note = note;
		t->info.builtin = builtin;
		t->info.fromFile = fromFile;
		g_table.store(t);
		g_current = a_code;
		return true;
	}

	const std::string& Current() { return g_table.load() ? g_current : English(); }
	bool               IsEnglish() { return g_table.load() == nullptr; }

	std::string LoadNote()
	{
		auto* t = g_table.load();
		return t ? t->note : std::string();
	}

	LoadInfo LastLoad()
	{
		auto* t = g_table.load();
		return t ? t->info : LoadInfo{};
	}

	const char* Tr(const char* a_en)
	{
		Table* t = g_table.load();
		if (!t || !a_en || !a_en[0]) {
			return a_en;
		}
		const char* hash = std::strstr(a_en, "##");
		if (!hash) {
			const char* s = Lookup(t, a_en);
			return s ? s : a_en;
		}
		if (hash == a_en) {
			return a_en;  // only an id
		}
		std::lock_guard l(t->m);
		if (auto it = t->composed.find(a_en); it != t->composed.end()) {
			return it->second.c_str();
		}
		const char* s = Lookup(t, std::string_view(a_en, static_cast<std::size_t>(hash - a_en)));
		if (!s) {
			return a_en;
		}
		auto& c = t->composed[a_en];
		c = std::string(s) + hash;
		return c.c_str();
	}

	const char* TrId(const char* a_en)
	{
		Table* t = g_table.load();
		if (!t || !a_en || !a_en[0] || std::strstr(a_en, "##")) {
			return Tr(a_en);
		}
		const char* s = Lookup(t, a_en);
		if (!s) {
			return a_en;
		}
		std::lock_guard l(t->m);
		auto [it, added] = t->ids.try_emplace(a_en);
		if (added) {
			it->second = std::string(s) + "###" + a_en;
		}
		return it->second.c_str();
	}

	const char* TrCtx(const char* a_context, const char* a_en)
	{
		Table* t = g_table.load();
		if (!t || !a_en || !a_en[0] || !a_context) {
			return a_en;
		}
		std::string key = std::string(a_context) + "|" + a_en;
		{
			std::lock_guard l(t->m);
			if (auto it = t->composed.find(key); it != t->composed.end()) {
				return it->second.c_str();
			}
		}
		const char* s = Lookup(t, key);
		if (!s) {
			return Tr(a_en);
		}
		std::lock_guard l(t->m);
		auto& c = t->composed[key];
		c = s;
		return c.c_str();
	}

	std::vector<std::string> FormatSpecs(std::string_view a_text)
	{
		std::vector<std::string> v;
		for (std::size_t i = 0; i < a_text.size(); ++i) {
			if (a_text[i] != '%') {
				continue;
			}
			if (i + 1 < a_text.size() && a_text[i + 1] == '%') {
				++i;
				continue;
			}
			std::size_t j = i + 1;
			while (j < a_text.size() && std::strchr("-+ #0", a_text[j])) {
				++j;
			}
			while (j < a_text.size() && ((a_text[j] >= '0' && a_text[j] <= '9') || a_text[j] == '*' || a_text[j] == '.')) {
				++j;
			}
			while (j < a_text.size() && std::strchr("hljztL", a_text[j])) {
				++j;
			}
			if (j < a_text.size() && std::strchr("diouxXeEfFgGaAcspn", a_text[j])) {
				v.emplace_back(a_text.substr(i, j - i + 1));
				i = j;
			} else {
				v.emplace_back(a_text.substr(i, (j < a_text.size() ? j + 1 : j) - i));  // a stray %: kept so it is compared too
				i = j;
			}
		}
		return v;
	}
}
