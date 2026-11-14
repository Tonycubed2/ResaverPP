// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Text in saves and plugins is stored as the game or the mod's tools wrote it, byte for byte:
//  - UTF-8: Skyrim SE / AE / VR and Fallout 4 themselves (the Scaleform interface works in UTF-8; every
//    WideCharToMultiByte call in Fallout4.exe 1.11.240 is to CP_UTF8, and SkyrimSE.exe 1.6.1170 does not import
//    MultiByteToWideChar at all, so neither turns text into the Windows code page), their non-English strings files
//    (Chinese, Japanese, Korean, Russian... - xEdit and xTranslator read them as UTF-8 first), and most translated mods;
//  - an 8-bit / double-byte Windows code page: English strings files (Windows-1252), Skyrim LE's strings files of
//    their language (1251 Russian, 1250 Polish...), and older mods made on a Chinese (936 GBK), Japanese (932),
//    Korean (949) or Traditional Chinese (950 Big5) Windows.
// These turn it into UTF-8 for display (valid UTF-8 is kept, anything else is read in a code page), and back for
// edits in the encoding the text had.
#pragma once

#include <filesystem>
#include <string>

namespace ess::text
{
	// The setting "Text encoding of plugins and saves". 0 = automatic: the Windows code page when it is a legacy one
	// (not 65001 "Beta: UTF-8"), else the one of the game's language (sLanguage), else Windows-1252. 65001 = UTF-8
	// only (text that is not UTF-8 is then read as Windows-1252).
	void     SetCodePage(unsigned a_codePage);
	unsigned CodePage();           // the setting (0 = automatic)
	unsigned EffectiveCodePage();  // the code page actually used for text that is not UTF-8
	unsigned SystemCodePage();     // GetACP() on Windows; 0 elsewhere

	// The game's language (sLanguage: "english", "chinese", "en", "cn", "zhhans"...), a hint for Automatic.
	void        SetGameLanguage(const std::string& a_sLanguage);
	std::string GameLanguage();
	// The code page text in that language uses when it is not UTF-8 (xEdit's / xTranslator's tables: chinese 936,
	// japanese 932, russian 1251, polish 1250...). 0 when the language is not known.
	unsigned LanguageCodePage(const std::string& a_sLanguage);
	// A name for a code page: "UTF-8", "GBK (936)", "Windows-1252"...
	std::string CodePageName(unsigned a_codePage);

	bool IsUtf8(const std::string& a_text);  // ASCII counts as UTF-8; overlong forms and surrogates do not
	bool IsAscii(const std::string& a_text);
	// To UTF-8: valid UTF-8 is kept (a sequence cut off at the very end, from a fixed-size field, is dropped);
	// anything else is read in a_codePage (0: EffectiveCodePage()).
	std::string ToUtf8(const std::string& a_gameText, unsigned a_codePage = 0);
	// Text in one code page to UTF-8, without the UTF-8 check (for tests and for text known to be in that code page).
	// false when the bytes are not valid in it (a_out then has U+FFFD for them).
	bool DecodeCodePage(const std::string& a_bytes, unsigned a_codePage, std::string& a_out);
	// UTF-8 to a code page. false when a character has no place in it (a_out then has '?' there).
	bool EncodeCodePage(const std::string& a_utf8, unsigned a_codePage, std::string& a_out);

	// For an edited value: back to the encoding a_original had. UTF-8 stays UTF-8; text from a code page goes back to
	// that code page (UTF-8 when a typed character has no place in it: never '?'). An ASCII or empty original says
	// nothing: a_preferUtf8 decides (what the rest of the file uses; the games write UTF-8).
	std::string FromUtf8(const std::string& a_utf8, const std::string& a_original = {}, bool a_preferUtf8 = true);

	// An environment variable holding a folder (LOCALAPPDATA, APPDATA...), read as Unicode. getenv() gives
	// it in the Windows code page, and a path made from that breaks on a user name with letters outside
	// ASCII ("Cannot convert character sequence"). Empty when the variable is not set.
	std::filesystem::path EnvPath(const char* a_name);
	// ReSaver++ run under Wine / Proton in a prefix of its own (a non-Steam game): the Windows user folder of the
	// game's own Proton prefix (...\steamapps\compatdata\<app id>\pfx\drive_c\users\steamuser), where the game keeps
	// My Games and AppData\Local\<game>\plugins.txt. Empty everywhere else (Windows, or run inside the game's prefix).
	// a_game: 0 Skyrim SE/AE, 1 Skyrim VR, 2 Fallout 4, 3 Starfield. The finder is set by the GUI (Platform.cpp).
	using GameUserFolderFn = std::filesystem::path (*)(int a_game);
	void                  SetGameUserFolderFinder(GameUserFolderFn a_fn);
	std::filesystem::path GameUserFolder(int a_game);

	// A path from text that may be UTF-8 or the Windows code page (names read from saves, archives, INIs):
	// never throws, unlike std::filesystem::path(std::string) with text that is not UTF-8.
	std::filesystem::path PathFrom(const std::string& a_text);
}
