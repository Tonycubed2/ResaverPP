// Text encodings of game text (core/Text.*): UTF-8 detection, code page reading and writing back, the PDF report's
// Chinese text. Samples: Chinese in UTF-8 and GBK (936), Big5 (950), Shift-JIS (932), Korean (949), Russian in 1251 and
// UTF-8, Western European 1252.
#include "core/Report.h"
#include "core/Text.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
	int g_failed = 0;

	void Check(bool a_ok, const char* a_what)
	{
		if (!a_ok) {
			std::printf("FAIL: %s\n", a_what);
			++g_failed;
		}
	}

	std::string Hex(const std::string& s)
	{
		std::string o;
		char        b[4];
		for (unsigned char c : s) {
			std::snprintf(b, sizeof(b), "%02X ", c);
			o += b;
		}
		return o;
	}

	void CheckEq(const std::string& a_got, const std::string& a_want, const char* a_what)
	{
		if (a_got != a_want) {
			std::printf("FAIL: %s\n   got  %s\n   want %s\n", a_what, Hex(a_got).c_str(), Hex(a_want).c_str());
			++g_failed;
		}
	}

	// samples (the bytes as the encodings have them)
	const std::string kZhUtf8 = "\xE9\x93\x81\xE5\x8C\xA0";          // 铁匠 (blacksmith), UTF-8
	const std::string kZhGbk = "\xCC\xFA\xBD\xB3";                    // 铁匠, GBK / 936
	const std::string kZhTUtf8 = "\xE9\x90\xB5\xE5\x8C\xA0";         // 鐵匠, UTF-8
	const std::string kZhBig5 = "\xC5\x4B\xA6\x4B";                   // 鐵匠, Big5 / 950
	const std::string kJaUtf8 = "\xE3\x83\x9B\xE3\x83\xAF\xE3\x82\xA4\xE3\x83\x88\xE3\x83\xA9\xE3\x83\xB3";  // ホワイトラン
	const std::string kJaSjis = "\x83\x7A\x83\x8F\x83\x43\x83\x67\x83\x89\x83\x93";                          // ホワイトラン, 932
	const std::string kKoUtf8 = "\xED\x99\x94\xEC\x9D\xB4\xED\x8A\xB8\xEB\x9F\xB0";                          // 화이트런
	const std::string kKoUhc = "\xC8\xAD\xC0\xCC\xC6\xAE\xB7\xB1";                                            // 화이트런, 949
	const std::string kRuUtf8 = "\xD0\x92\xD0\xB0\xD0\xB9\xD1\x82\xD1\x80\xD0\xB0\xD0\xBD";                  // Вайтран
	const std::string kRu1251 = "\xC2\xE0\xE9\xF2\xF0\xE0\xED";                                               // Вайтран, 1251
	const std::string kFrUtf8 = "Touch\xC3\xA9 \xE2\x80\x94 \xE2\x82\xAC";                                     // Touché — €
	const std::string kFr1252 = "Touch\xE9 \x97 \x80";

	void TestDetection()
	{
		using namespace ess::text;
		Check(IsUtf8(""), "empty is UTF-8");
		Check(IsUtf8("Whiterun"), "ASCII is UTF-8");
		Check(IsUtf8(kZhUtf8) && IsUtf8(kJaUtf8) && IsUtf8(kKoUtf8) && IsUtf8(kRuUtf8) && IsUtf8(kFrUtf8), "UTF-8 samples are UTF-8");
		Check(!IsUtf8(kZhGbk), "GBK is not UTF-8");
		Check(!IsUtf8(kZhBig5), "Big5 is not UTF-8");
		Check(!IsUtf8(kJaSjis), "Shift-JIS is not UTF-8");
		Check(!IsUtf8(kKoUhc), "949 is not UTF-8");
		Check(!IsUtf8(kRu1251), "1251 is not UTF-8");
		Check(!IsUtf8(kFr1252), "1252 is not UTF-8");
		Check(!IsUtf8("\xC0\xAF"), "overlong '/' is not UTF-8");
		Check(!IsUtf8("\xE0\x80\xAF"), "overlong 3-byte is not UTF-8");
		Check(!IsUtf8("\xED\xA0\x80"), "a surrogate is not UTF-8");
		Check(!IsUtf8("\xF5\x80\x80\x80"), "beyond U+10FFFF is not UTF-8");
		Check(IsUtf8("\xF0\x9F\x98\x80"), "an emoji is UTF-8");
		Check(IsAscii("abc") && !IsAscii(kZhUtf8), "IsAscii");
	}

	void TestReading()
	{
		using namespace ess::text;
		SetCodePage(0);
		// valid UTF-8 is kept whatever the code page
		for (unsigned cp : { 0u, 936u, 950u, 932u, 949u, 1251u, 1252u, 65001u }) {
			SetCodePage(cp);
			CheckEq(ToUtf8(kZhUtf8), kZhUtf8, "UTF-8 Chinese kept");
			CheckEq(ToUtf8(kRuUtf8), kRuUtf8, "UTF-8 Russian kept");
		}
		SetCodePage(0);
		// code pages
		CheckEq(ToUtf8(kZhGbk, 936), kZhUtf8, "GBK -> UTF-8");
		CheckEq(ToUtf8(kZhBig5, 950), kZhTUtf8, "Big5 -> UTF-8");
		CheckEq(ToUtf8(kJaSjis, 932), kJaUtf8, "Shift-JIS -> UTF-8");
		CheckEq(ToUtf8(kKoUhc, 949), kKoUtf8, "949 -> UTF-8");
		CheckEq(ToUtf8(kRu1251, 1251), kRuUtf8, "1251 -> UTF-8");
		CheckEq(ToUtf8(kFr1252, 1252), kFrUtf8, "1252 -> UTF-8");
		// the setting is the code page used when none is given
		SetCodePage(936);
		CheckEq(ToUtf8(kZhGbk), kZhUtf8, "setting 936: GBK read");
		SetCodePage(1251);
		CheckEq(ToUtf8(kRu1251), kRuUtf8, "setting 1251: 1251 read");
		// "UTF-8 only": anything else as Windows-1252 (never dropped)
		SetCodePage(65001);
		CheckEq(ToUtf8(kFr1252), kFrUtf8, "setting UTF-8: other text read as 1252");
		SetCodePage(0);
		// UTF-8 cut off inside its last character (a fixed-size field): the piece is dropped, the rest stays UTF-8
		CheckEq(ToUtf8(kZhUtf8 + "\xE5\x8C"), kZhUtf8, "cut-off UTF-8 tail dropped");
		// ... but a few bytes that only look like a cut-off are read in the code page
		CheckEq(ToUtf8("\xE4\xB8", 936), "\xE6\xB6\x93", "GBK E4B8 is not a cut-off UTF-8 character");  // 涓
		// bad bytes in a double-byte code page: U+FFFD, not '?', and the rest is kept
		std::string out;
		Check(!DecodeCodePage("A\x81", 936, out), "lone GBK lead byte reported");
		Check(out.rfind("A", 0) == 0 && out.find("\xEF\xBF\xBD") != std::string::npos, "lone lead byte -> U+FFFD");
	}

	void TestAutomatic()
	{
		using namespace ess::text;
		SetCodePage(0);
		// automatic: Windows' code page when it is a legacy one, else the game language's, else 1252
		SetGameLanguage("chinese");
		const unsigned sys = SystemCodePage();
		if (sys == 0 || sys == 65001) {
			Check(EffectiveCodePage() == 936, "automatic + sLanguage=chinese -> 936");
			CheckEq(ToUtf8(kZhGbk), kZhUtf8, "automatic + chinese: GBK read");
			SetGameLanguage("ru");
			Check(EffectiveCodePage() == 1251, "automatic + ru -> 1251");
			SetGameLanguage("zhhant");
			Check(EffectiveCodePage() == 950, "automatic + zhhant -> 950");
			SetGameLanguage("english");
			Check(EffectiveCodePage() == 1252, "automatic + english -> 1252");
			SetGameLanguage("");
			Check(EffectiveCodePage() == 1252, "automatic, nothing known -> 1252");
		} else {
			Check(EffectiveCodePage() == sys, "automatic -> the Windows code page");
		}
		SetGameLanguage("");
		Check(LanguageCodePage("Japanese") == 932 && LanguageCodePage("ko") == 949 && LanguageCodePage("polish") == 1250 &&
				  LanguageCodePage("klingon") == 0,
			"LanguageCodePage table");
	}

	void TestWriting()
	{
		using namespace ess::text;
		SetCodePage(0);
		// ASCII stays ASCII
		CheckEq(FromUtf8("Lydia", kZhGbk), "Lydia", "ASCII edit");
		// an edit of UTF-8 text stays UTF-8
		SetCodePage(936);
		CheckEq(FromUtf8(kZhUtf8, kRuUtf8), kZhUtf8, "edit of UTF-8 text stays UTF-8");
		// an edit of GBK text goes back to GBK
		CheckEq(FromUtf8(kZhUtf8, kZhGbk), kZhGbk, "edit of GBK text written in GBK");
		// round trip: read with the code page, written back the same bytes
		for (const auto& [bytes, cp] : { std::pair{ kZhGbk, 936u }, std::pair{ kZhBig5, 950u }, std::pair{ kJaSjis, 932u },
				 std::pair{ kKoUhc, 949u }, std::pair{ kRu1251, 1251u }, std::pair{ kFr1252, 1252u } }) {
			SetCodePage(cp);
			CheckEq(FromUtf8(ToUtf8(bytes), bytes), bytes, "code page round trip");
		}
		// a typed character the original's code page does not have: UTF-8, never '?'
		SetCodePage(1251);
		CheckEq(FromUtf8(kRuUtf8 + kZhUtf8, kRu1251), kRuUtf8 + kZhUtf8, "unrepresentable -> UTF-8");
		// an ASCII / empty original says nothing: UTF-8 when the file uses UTF-8 (the games' own encoding) ...
		SetCodePage(0);
		CheckEq(FromUtf8(kZhUtf8, "old name"), kZhUtf8, "new text, UTF-8 file -> UTF-8");
		CheckEq(FromUtf8(kZhUtf8, ""), kZhUtf8, "new text, empty original -> UTF-8");
		// ... and the code page when the file uses a code page
		SetCodePage(936);
		CheckEq(FromUtf8(kZhUtf8, "old name", false), kZhGbk, "new text, GBK file -> GBK");
		SetCodePage(1252);
		CheckEq(FromUtf8(kZhUtf8, "old name", false), kZhUtf8, "new text not in 1252 -> UTF-8");
		std::string out;
		Check(!EncodeCodePage(kZhUtf8, 1252, out), "Chinese has no place in 1252");
		Check(EncodeCodePage(kFrUtf8, 1252, out) && out == kFr1252, "1252 encode with the 0x80..0x9F characters");
		SetCodePage(0);
	}

	void TestPdf()
	{
		// the PDF report: Chinese text goes into a CJK font, not '?'
		const auto p = std::filesystem::temp_directory_path() / "rs_text_test.pdf";
		ess::report::WritePdf(p, "Report " + kZhUtf8, { "#Character", "Name: " + kZhUtf8 + " (level 3)", "Place: " + kJaUtf8, "plain ASCII" });
		std::ifstream f(p, std::ios::binary);
		const std::string pdf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		Check(pdf.find("/UniGB-UCS2-H") != std::string::npos, "PDF has a CJK font");
		Check(pdf.find("<94C15320>") != std::string::npos, "PDF has 铁匠 as UCS-2 (94C1 5320)");
		Check(pdf.find("(Name: ) Tj") != std::string::npos, "PDF keeps the Latin part in Courier");
		Check(pdf.find("Name: ??") == std::string::npos, "PDF has no '?' for the Chinese");
		std::error_code ec;
		std::filesystem::remove(p, ec);
	}
}

int main()
{
	TestDetection();
	TestReading();
	TestAutomatic();
	TestWriting();
	TestPdf();
	if (g_failed) {
		std::printf("%d failed\n", g_failed);
		return 1;
	}
	std::puts("text: all passed");
	return 0;
}
