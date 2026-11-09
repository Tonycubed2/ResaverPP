#pragma once
// Interface translation. Every text the GUI shows goes through tr("English text"): it returns the text in the
// chosen language, or the English itself when the catalogue has no translation for it (so anything new or
// untranslated simply shows in English).
//
// Catalogues are UTF-8 text files, lang/<code>.txt in the source tree. They are built into the exe
// (LangEmbedded.cpp, made by CMake), and a lang/<code>.txt next to the exe, if there is one, is read on top of
// the built-in one, so translators can test their edits without a rebuild. Format (see TRANSLATING.md):
//
//   language "Русский"            <- the name shown in Options > Language
//   # a comment (file:line and the code line the text comes from)
//   en "Open a save"
//   tr "Открыть сохранение"
//
// Texts are C-style quoted: \n, \t, \" and \\ are the escapes. An empty tr "" means "not translated yet".
// ImGui ids: in "Set##gold" / "Perks (%zu)###perks" only the part before ## is looked up and translated; the id
// part is kept as it is. trc("home", "Mods") looks up "home|Mods" first (a translation for that place only).
// Format strings keep the same printf conversions in the same order (a unit test checks).

#include <string>
#include <string_view>
#include <vector>

namespace i18n
{
	struct Entry
	{
		std::string en;
		std::string tr;
		int         line = 0;
	};

	struct Catalogue
	{
		std::string        code;  // "ru"
		std::string        name;  // "Русский"
		std::vector<Entry> entries;
	};

	// Parses the text of a catalogue file. Returns false (with a message naming the line) on a syntax error;
	// entries read before the error are kept.
	bool Parse(std::string_view a_text, Catalogue& a_out, std::string& a_error);

	// The catalogues built into the exe: code -> file text
	struct Builtin
	{
		const char*          code;
		const unsigned char* data;
		std::size_t          size;
	};
	const std::vector<Builtin>& Builtins();

	struct Language
	{
		std::string code;  // "en", "ru"
		std::string name;  // "English", "Русский"
	};
	// English first, then the built-in languages, then any other lang/<code>.txt found in the language folder
	std::vector<Language> Languages();

	// the folder with lang/<code>.txt files that override the built-in ones (the exe's own "lang" folder)
	void SetLangFolder(const std::string& a_utf8Folder);

	// Switches the language ("en" = no translation). Takes effect at once for everything drawn after it.
	// Returns false if the language is not known (English is used then).
	bool SetLanguage(const std::string& a_code);
	const std::string& Current();
	bool               IsEnglish();
	// what happened when the language was loaded: "1234 texts built in, 12 from lang/ru.txt", or an error (English)
	std::string LoadNote();
	struct LoadInfo
	{
		std::size_t builtin = 0;   // texts from the catalogue built into the exe
		std::size_t fromFile = 0;  // texts from lang/<code>.txt next to the exe
		std::string file;          // "lang/ru.txt" when there was one
		std::string error;         // a syntax error in it (texts before it are used)
	};
	LoadInfo LastLoad();

	// every translated text of the current language, one after the other (to load the font characters they need)
	std::string AllTranslations();

	// the translation (or the English); the returned pointer stays valid for the whole run
	const char* Tr(const char* a_en);
	// like Tr, but for window titles, popups, headers and tabs: the English stays the ImGui id
	// ("Перенос###Transplant"), so window places and open states do not change with the language
	const char* TrId(const char* a_en);

	// a text whose translation depends on where it is ("Save" the verb on a button, "Save" the noun as a header):
	// the catalogue key is "<context>|<English>"; without that key the plain translation of the English is used
	const char* TrCtx(const char* a_context, const char* a_en);

	// the printf conversions of a format string, in order ("%zu", "%s", "%.1f"); "%%" is not one
	std::vector<std::string> FormatSpecs(std::string_view a_text);
}

inline const char* tr(const char* a_en) { return i18n::Tr(a_en); }
inline const char* tr(const std::string& a_en) = delete;  // would be a dangling pointer: use tr(s.c_str()) on a stable string
inline const char* trId(const char* a_en) { return i18n::TrId(a_en); }
inline const char* trc(const char* a_context, const char* a_en) { return i18n::TrCtx(a_context, a_en); }
// marks a text for the catalogue where it cannot be translated yet (static tables): tr() it where it is shown
#define N_(a_text) a_text
