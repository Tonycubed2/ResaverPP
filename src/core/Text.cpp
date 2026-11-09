// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Text.h"

#include <cctype>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <iconv.h>
#endif

namespace ess::text
{
	namespace
	{
		std::atomic<unsigned> g_codePage{ 0 };
		std::mutex            g_langMutex;
		std::string           g_gameLanguage;
		std::atomic<unsigned> g_languageCodePage{ 0 };

		void PutUtf8(std::string& out, char32_t u)
		{
			if (u < 0x80) {
				out.push_back(static_cast<char>(u));
			} else if (u < 0x800) {
				out.push_back(static_cast<char>(0xC0 | (u >> 6)));
				out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
			} else if (u < 0x10000) {
				out.push_back(static_cast<char>(0xE0 | (u >> 12)));
				out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
			} else {
				out.push_back(static_cast<char>(0xF0 | (u >> 18)));
				out.push_back(static_cast<char>(0x80 | ((u >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
			}
		}

		// Windows-1252 0x80..0x9F (0x81, 0x8D, 0x8F, 0x90, 0x9D are unassigned: kept as the C1 control of that number)
		const char16_t k1252[32] = { 0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,
			0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178 };

		std::string From1252(const std::string& s)
		{
			std::string out;
			out.reserve(s.size() + s.size() / 2);
			for (unsigned char c : s) {
				PutUtf8(out, c < 0x80 ? c : (c < 0xA0 ? k1252[c - 0x80] : c));
			}
			return out;
		}

		// the next code point of valid UTF-8 at i (i moves past it); -1 on an invalid sequence (i moves one byte)
		long NextCodePoint(const std::string& s, std::size_t& i)
		{
			const auto c = static_cast<unsigned char>(s[i]);
			if (c < 0x80) {
				++i;
				return c;
			}
			int      n;
			char32_t u, min;
			if (c >= 0xC2 && c <= 0xDF) {
				n = 1, u = c & 0x1F, min = 0x80;
			} else if ((c & 0xF0) == 0xE0) {
				n = 2, u = c & 0x0F, min = 0x800;
			} else if (c >= 0xF0 && c <= 0xF4) {
				n = 3, u = c & 0x07, min = 0x10000;
			} else {
				++i;
				return -1;
			}
			if (i + static_cast<std::size_t>(n) >= s.size()) {
				++i;
				return -1;
			}
			for (int k = 1; k <= n; ++k) {
				const auto cc = static_cast<unsigned char>(s[i + k]);
				if ((cc & 0xC0) != 0x80) {
					++i;
					return -1;
				}
				u = (u << 6) | (cc & 0x3F);
			}
			if (u < min || u > 0x10FFFF || (u >= 0xD800 && u <= 0xDFFF)) {  // overlong, beyond Unicode, surrogate
				++i;
				return -1;
			}
			i += static_cast<std::size_t>(n) + 1;
			return static_cast<long>(u);
		}

		// valid UTF-8 cut off in the middle of its last character (a fixed-size field): the length without that piece,
		// or npos when the text is not like that
		std::size_t CutUtf8Length(const std::string& s)
		{
			std::size_t k = s.size();
			int         cont = 0;
			while (k > 0 && cont < 3 && (static_cast<unsigned char>(s[k - 1]) & 0xC0) == 0x80) {
				--k;
				++cont;
			}
			if (k == 0) {
				return std::string::npos;
			}
			const auto lead = static_cast<unsigned char>(s[k - 1]);
			const int  need = lead >= 0xC2 && lead <= 0xDF ? 1 : (lead & 0xF0) == 0xE0 ? 2 : lead >= 0xF0 && lead <= 0xF4 ? 3 : -1;
			if (need < 0 || cont >= need) {
				return std::string::npos;
			}
			return k - 1;
		}

#ifndef _WIN32
		bool Iconv(const char* a_to, const char* a_from, const std::string& a_in, std::string& a_out)
		{
			iconv_t cd = iconv_open(a_to, a_from);
			if (cd == reinterpret_cast<iconv_t>(-1)) {
				return false;
			}
			std::string in = a_in, out(a_in.size() * 4 + 8, '\0');
			char*       ip = in.data();
			char*       op = out.data();
			std::size_t il = in.size(), ol = out.size();
			const auto  rc = iconv(cd, &ip, &il, &op, &ol);
			iconv_close(cd);
			if (rc == static_cast<std::size_t>(-1)) {
				return false;
			}
			out.resize(out.size() - ol);
			a_out = std::move(out);
			return true;
		}

		// glibc's names for the Windows code pages
		std::string IconvName(unsigned cp)
		{
			switch (cp) {
			case 936: return "CP936";
			case 950: return "CP950";
			case 932: return "CP932";
			case 949: return "CP949";
			default: return "CP" + std::to_string(cp);
			}
		}
#endif
	}

	void     SetCodePage(unsigned a_codePage) { g_codePage = a_codePage; }
	unsigned CodePage() { return g_codePage; }

	unsigned SystemCodePage()
	{
#ifdef _WIN32
		return GetACP();
#else
		return 0;
#endif
	}

	void SetGameLanguage(const std::string& a_sLanguage)
	{
		std::lock_guard l(g_langMutex);
		g_gameLanguage = a_sLanguage;
		g_languageCodePage = LanguageCodePage(a_sLanguage);
	}

	std::string GameLanguage()
	{
		std::lock_guard l(g_langMutex);
		return g_gameLanguage;
	}

	unsigned LanguageCodePage(const std::string& a_lang)
	{
		std::string l;
		for (char c : a_lang) {
			l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		// Skyrim's language names, Fallout 4's codes (Fallout4.exe's own list: en fr it de es esmx pl ja nl pt ptbr ru ko
		// zhhant zhhans fi sv da no, and cn for the Chinese strings files). The code pages are xEdit's
		// AddDefaultLEncodingsIfMissing / xTranslator's Data\Skyrim\codepage.txt for the 8-bit languages. For Chinese,
		// Japanese and Korean those tables only say UTF-8 (the strings files are UTF-8): the code page here is for older
		// mods that are not, made on a Windows of that language (936 GBK on a Simplified Chinese Windows, 950 Big5 on a
		// Traditional one, 932 Shift-JIS, 949).
		static const std::pair<const char*, unsigned> map[] = { { "english", 1252 }, { "en", 1252 }, { "french", 1252 }, { "fr", 1252 },
			{ "german", 1252 }, { "de", 1252 }, { "italian", 1252 }, { "it", 1252 }, { "spanish", 1252 }, { "es", 1252 }, { "esmx", 1252 },
			{ "portuguese", 1252 }, { "pt", 1252 }, { "ptbr", 1252 }, { "danish", 1252 }, { "da", 1252 }, { "finnish", 1252 }, { "fi", 1252 },
			{ "norwegian", 1252 }, { "no", 1252 }, { "swedish", 1252 }, { "sv", 1252 }, { "dutch", 1252 }, { "nl", 1252 }, { "polish", 1250 },
			{ "pl", 1250 }, { "czech", 1250 }, { "cs", 1250 }, { "hungarian", 1250 }, { "hu", 1250 }, { "russian", 1251 }, { "ru", 1251 },
			{ "greek", 1253 }, { "turkish", 1254 }, { "tr", 1254 }, { "arabic", 1256 }, { "ar", 1256 }, { "thai", 874 }, { "th", 874 },
			{ "japanese", 932 }, { "ja", 932 }, { "korean", 949 }, { "ko", 949 }, { "chinese", 936 }, { "cn", 936 }, { "zhhans", 936 },
			{ "zhhant", 950 } };
		for (const auto& [name, cp] : map) {
			if (l == name) {
				return cp;
			}
		}
		return 0;
	}

	std::string CodePageName(unsigned cp)
	{
		switch (cp) {
		case 0: return "automatic";
		case 65001: return "UTF-8";
		case 936: return "GBK (936)";
		case 950: return "Big5 (950)";
		case 932: return "Shift-JIS (932)";
		case 949: return "Korean (949)";
		case 874: return "Thai (874)";
		default: return "Windows-" + std::to_string(cp);
		}
	}

	unsigned EffectiveCodePage()
	{
		if (const unsigned cp = g_codePage) {
			return cp;
		}
		// Windows' own code page, unless it is UTF-8 ("Beta: Use Unicode UTF-8 for worldwide language support"), which
		// says nothing about how older text was written
		if (const unsigned sys = SystemCodePage(); sys && sys != 65001 && sys != 1200 && sys != 1201) {
			return sys;
		}
		if (const unsigned lang = g_languageCodePage) {
			return lang;
		}
		return 1252;
	}

	bool IsAscii(const std::string& a)
	{
		return std::all_of(a.begin(), a.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
	}

	bool IsUtf8(const std::string& a)
	{
		for (std::size_t i = 0; i < a.size();) {
			if (static_cast<unsigned char>(a[i]) < 0x80) {
				++i;
			} else if (NextCodePoint(a, i) < 0) {
				return false;
			}
		}
		return true;
	}

	bool DecodeCodePage(const std::string& a, unsigned cp, std::string& a_out)
	{
		a_out.clear();
		if (a.empty()) {
			return true;
		}
		if (cp == 65001) {
			if (IsUtf8(a)) {
				a_out = a;
				return true;
			}
			for (std::size_t i = 0; i < a.size();) {
				const long u = NextCodePoint(a, i);
				PutUtf8(a_out, u < 0 ? 0xFFFD : static_cast<char32_t>(u));
			}
			return false;
		}
		if (cp == 1252) {
			a_out = From1252(a);
			return true;
		}
		// the whole text at once; on bad bytes, character by character (one byte, or a lead + trail byte), U+FFFD for
		// each byte that is no character (the system's own replacement would be '?' or a dot)
		auto strict = [cp](const std::string& in, std::string& out) {
#ifdef _WIN32
			const int n = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, in.data(), static_cast<int>(in.size()), nullptr, 0);
			if (n <= 0) {
				return false;
			}
			std::wstring w(static_cast<std::size_t>(n), L'\0');
			MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, in.data(), static_cast<int>(in.size()), w.data(), n);
			const int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), n, nullptr, 0, nullptr, nullptr);
			out.assign(static_cast<std::size_t>(m), '\0');
			WideCharToMultiByte(CP_UTF8, 0, w.data(), n, out.data(), m, nullptr, nullptr);
			return true;
#else
			return Iconv("UTF-8", IconvName(cp).c_str(), in, out);
#endif
		};
		if (strict(a, a_out)) {
			return true;
		}
		// a code page this system does not have at all: Windows-1252
		if (std::string probe; !strict("A", probe)) {
			a_out = From1252(a);
			return false;
		}
		a_out.clear();
		for (std::size_t i = 0; i < a.size();) {
			std::string piece;
			if (strict(a.substr(i, 1), piece)) {
				a_out += piece;
				i += 1;
			} else if (i + 1 < a.size() && strict(a.substr(i, 2), piece)) {
				a_out += piece;
				i += 2;
			} else {
				PutUtf8(a_out, 0xFFFD);
				i += 1;
			}
		}
		return false;
	}

	bool EncodeCodePage(const std::string& u, unsigned cp, std::string& a_out)
	{
		a_out.clear();
		if (cp == 65001 || IsAscii(u)) {
			a_out = u;
			return true;
		}
		if (cp == 1252) {
			bool ok = true;
			for (std::size_t i = 0; i < u.size();) {
				const long c = NextCodePoint(u, i);
				char       b = '?';
				if (c >= 0 && (c < 0x80 || (c >= 0xA0 && c <= 0xFF))) {
					b = static_cast<char>(c);
				} else if (c >= 0) {
					for (int k = 0; k < 32; ++k) {
						if (k1252[k] == c) {
							b = static_cast<char>(0x80 + k);
						}
					}
				}
				if (b == '?') {
					ok = false;
				}
				a_out.push_back(b);
			}
			return ok;
		}
#ifdef _WIN32
		const int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), static_cast<int>(u.size()), nullptr, 0);
		std::wstring w(static_cast<std::size_t>(std::max(n, 0)), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, u.data(), static_cast<int>(u.size()), w.data(), n);
		BOOL      usedDefault = FALSE;
		const int m = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, w.data(), n, nullptr, 0, nullptr, &usedDefault);
		if (m <= 0) {
			a_out = u;
			return false;
		}
		a_out.assign(static_cast<std::size_t>(m), '\0');
		WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, w.data(), n, a_out.data(), m, nullptr, &usedDefault);
		return !usedDefault;
#else
		if (Iconv(IconvName(cp).c_str(), "UTF-8", u, a_out)) {
			return true;
		}
		a_out.clear();
		for (std::size_t i = 0; i < u.size();) {
			const std::size_t s = i;
			NextCodePoint(u, i);
			std::string piece;
			a_out += Iconv(IconvName(cp).c_str(), "UTF-8", u.substr(s, i - s), piece) ? piece : std::string("?");
		}
		return false;
#endif
	}

	std::string ToUtf8(const std::string& a, unsigned a_codePage)
	{
		if (IsUtf8(a)) {
			return a;
		}
		// UTF-8 cut off in its last character (with real UTF-8 before it: a few bytes alone could be a code page)
		if (const auto cut = CutUtf8Length(a); cut != std::string::npos) {
			const std::string head = a.substr(0, cut);
			if (!IsAscii(head) && IsUtf8(head)) {
				return head;
			}
		}
		unsigned cp = a_codePage ? a_codePage : EffectiveCodePage();
		if (cp == 65001) {
			cp = 1252;  // "UTF-8 only": what is not UTF-8 is shown as Windows-1252, never dropped
		}
		std::string out;
		DecodeCodePage(a, cp, out);
		return out;
	}

	std::string FromUtf8(const std::string& u, const std::string& a_original, bool a_preferUtf8)
	{
		if (IsAscii(u)) {
			return u;
		}
		const bool originalUtf8 = !IsAscii(a_original) && IsUtf8(a_original);
		if (originalUtf8 || (IsAscii(a_original) && a_preferUtf8)) {
			return u;
		}
		unsigned cp = EffectiveCodePage();
		if (cp == 65001) {
			if (IsAscii(a_original)) {
				return u;
			}
			cp = 1252;  // the original was read as Windows-1252 (see ToUtf8)
		}
		std::string out;
		if (EncodeCodePage(u, cp, out)) {
			return out;
		}
		return u;  // a character the code page does not have: UTF-8 rather than '?'
	}

	std::filesystem::path EnvPath(const char* a_name)
	{
#ifdef _WIN32
		std::wstring w;
		for (const char* c = a_name; *c; ++c) {
			w += static_cast<wchar_t>(*c);
		}
		const wchar_t* v = _wgetenv(w.c_str());
		return v ? std::filesystem::path(std::wstring(v)) : std::filesystem::path();
#else
		const char* v = std::getenv(a_name);
		return v ? std::filesystem::path(v) : std::filesystem::path();
#endif
	}

	std::filesystem::path PathFrom(const std::string& a_text)
	{
		const auto utf8 = IsUtf8(a_text) ? a_text : ToUtf8(a_text);
		try {
			return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
		} catch (...) {
			std::string ascii;
			for (char c : utf8) {
				ascii += static_cast<unsigned char>(c) < 0x80 ? c : '_';
			}
			return std::filesystem::path(ascii);
		}
	}
}
