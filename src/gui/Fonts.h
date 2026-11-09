// ReSaver++ - the fonts ImGui draws with: the interface font (Segoe UI / DejaVu Sans), the fixed-width font of the
// script viewer (Consolas / DejaVu Sans Mono), the big start page title, and characters other fonts are needed for.
//
// Chinese, Japanese, Korean and Thai characters are loaded only when the window first has to draw one: ImGui tells
// ImFont::FindGlyph's misses (GImFontMissingGlyphHook, a two-line addition in third_party/imgui), and before the next
// frame the font atlas is built again with those characters taken from a system font (Microsoft YaHei, SimSun,
// Microsoft JhengHei, Yu Gothic, Meiryo, Malgun Gothic, Leelawadee UI...; Noto Sans CJK / WenQuanYi on Linux and
// under Wine). Loading every Chinese character up front (ImGui's ChineseFull: 21 000 glyphs) made a 4096 x 8192
// texture (128 MB) at 100 % scaling and 4096 x 16384 at 150 %, more than many graphics drivers take, and half a
// second more at start; now the atlas holds only the characters seen (plus the 2 500 most common ones once Chinese
// text shows up when the interface or game is Chinese). Without such a font nothing changes (no error).
#pragma once

#include <string>

struct ImFont;

namespace fonts
{
	// The first build, before the first frame (a_scale: the window's content scale; a_maxTexture: GL_MAX_TEXTURE_SIZE)
	void Init(float a_scale, int a_maxTexture);

	// Which font comes first for Chinese characters (they look different in Chinese, Japanese and Korean fonts): from
	// the interface language ("zh-Hans", "zh-Hant", "ja", "ko"), the text code page (936, 950, 932, 949) and the game's
	// language ("chinese", "cn", "japanese"...). Cheap when nothing changed: call it every frame.
	void SetPreference(const std::string& a_uiLanguage, unsigned a_codePage, const std::string& a_gameLanguage);

	// Load the characters of this text with the next build (the interface translation, so it is right in the first frame)
	void Want(const std::string& a_utf8);

	// Between frames: builds the atlas again when characters were asked for (at most every 150 ms). True when it did:
	// the caller then makes the font texture again (ImGui_ImplOpenGL3_DestroyFontsTexture) and takes the new font
	// pointers (Mono(), Title(): the old ones are gone).
	bool Rebuild();

	ImFont* Mono();
	ImFont* Title();

	// for the log and Options: "Microsoft YaHei (msyh.ttc): 312 characters; atlas 2048 x 1024"
	std::string Status();
}
