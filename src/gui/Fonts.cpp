// ReSaver++ - fonts, with Chinese / Japanese / Korean / Thai characters loaded when first needed (see Fonts.h)
#include "Fonts.h"

#include "core/Text.h"

#include "imgui.h"

// (a private copy of stb_truetype, only to read font metrics)
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>

namespace fonts
{
	namespace
	{
		namespace fs = std::filesystem;

		enum Group
		{
			kNone = -1,
			kHan,     // Chinese characters, kana, bopomofo, CJK punctuation, full-width forms (and other symbols)
			kHangul,  // Korean
			kThai,
			kGroups
		};

		// a character the interface font does not have that a fallback font may have
		Group GroupOf(unsigned c)
		{
			if (c >= 0x0E00 && c <= 0x0E7F) {
				return kThai;
			}
			if ((c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) || (c >= 0xA960 && c <= 0xA97F) || (c >= 0xAC00 && c <= 0xD7FF)) {
				return kHangul;
			}
			if (c < 0x2000 || (c >= 0xD800 && c <= 0xF8FF) || c >= 0xFFF0) {
				return kNone;  // Latin, Greek, Cyrillic... (the interface font's own), surrogates, private use, specials
			}
			return kHan;  // CJK and the symbol blocks (arrows, box drawing, circled numbers...: CJK fonts have most)
		}

		struct Candidate
		{
			fs::path    file;
			int         fontNo = 0;  // in a .ttc collection
			std::string name;
		};

		// the preference: 'S' Simplified Chinese (also the default), 'T' Traditional Chinese, 'J' Japanese, 'K' Korean
		char        g_pref = 'S';
		bool        g_cjk = false;       // the interface, the text code page or the game is Chinese / Japanese / Korean
		std::string g_prefKey = "\x01";  // what SetPreference was last given

		std::vector<fs::path> WindowsFontDirs()
		{
			std::vector<fs::path> v;
#ifdef _WIN32
			if (const auto w = ess::text::EnvPath("WINDIR"); !w.empty()) {
				v.push_back(w / "Fonts");
			}
			v.emplace_back("C:\\Windows\\Fonts");
#endif
			return v;
		}

		// a Linux font file: as it is, and on Windows (Wine / Proton) on Wine's Z: drive
		std::vector<fs::path> UnixPaths(const char* a_path)
		{
#ifdef _WIN32
			std::string w = std::string("Z:") + a_path;
			std::replace(w.begin(), w.end(), '/', '\\');
			return { fs::path(w) };
#else
			return { fs::path(a_path) };
#endif
		}

		std::vector<Candidate> Chain(Group g)
		{
			struct W
			{
				const char* file;
				const char* name;
			};
			std::vector<W> win;
			int            noto = 2;  // NotoSansCJK-Regular.ttc: 0 JP, 1 KR, 2 SC, 3 TC, 4 HK
			switch (g) {
			case kHan:
				switch (g_pref) {
				case 'T':
					win = { { "msjh.ttc", "Microsoft JhengHei" }, { "mingliu.ttc", "MingLiU" }, { "msyh.ttc", "Microsoft YaHei" }, { "simsun.ttc", "SimSun" },
						{ "YuGothM.ttc", "Yu Gothic" }, { "meiryo.ttc", "Meiryo" }, { "msgothic.ttc", "MS Gothic" } };
					noto = 3;
					break;
				case 'J':
					win = { { "YuGothM.ttc", "Yu Gothic" }, { "meiryo.ttc", "Meiryo" }, { "msgothic.ttc", "MS Gothic" }, { "msyh.ttc", "Microsoft YaHei" },
						{ "msjh.ttc", "Microsoft JhengHei" }, { "simsun.ttc", "SimSun" } };
					noto = 0;
					break;
				case 'K':
					win = { { "malgun.ttf", "Malgun Gothic" }, { "gulim.ttc", "Gulim" }, { "msyh.ttc", "Microsoft YaHei" }, { "simsun.ttc", "SimSun" },
						{ "YuGothM.ttc", "Yu Gothic" } };
					noto = 1;
					break;
				default:
					win = { { "msyh.ttc", "Microsoft YaHei" }, { "simsun.ttc", "SimSun" }, { "msjh.ttc", "Microsoft JhengHei" }, { "YuGothM.ttc", "Yu Gothic" },
						{ "meiryo.ttc", "Meiryo" }, { "msgothic.ttc", "MS Gothic" } };
					noto = 2;
				}
				break;
			case kHangul:
				win = { { "malgun.ttf", "Malgun Gothic" }, { "gulim.ttc", "Gulim" } };
				noto = 1;
				break;
			case kThai:
				win = { { "LeelawUI.ttf", "Leelawadee UI" }, { "leelawad.ttf", "Leelawadee" }, { "tahoma.ttf", "Tahoma" } };
				break;
			default: break;
			}
			std::vector<Candidate> v;
			for (const auto& dir : WindowsFontDirs()) {
				for (const auto& w : win) {
					v.push_back({ dir / w.file, 0, w.name });
				}
			}
			auto addUnix = [&](const char* a_path, int a_no, const char* a_name) {
				for (auto& p : UnixPaths(a_path)) {
					v.push_back({ p, a_no, a_name });
				}
			};
			if (g == kHan || g == kHangul) {
				for (const char* p : { "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
						 "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/OTF/NotoSansCJK-Regular.ttc" }) {
					addUnix(p, noto, "Noto Sans CJK");
				}
				for (const char* p : { "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
						 "/usr/share/fonts/TTF/wqy-microhei.ttc" }) {
					addUnix(p, 0, "WenQuanYi Micro Hei");
				}
				if (g == kHangul) {
					addUnix("/usr/share/fonts/truetype/nanum/NanumGothic.ttf", 0, "NanumGothic");
				}
				addUnix("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", 0, "WenQuanYi Zen Hei");
				addUnix("/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", 0, "Droid Sans Fallback");
			} else if (g == kThai) {
				addUnix("/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf", 0, "Noto Sans Thai");
				addUnix("/usr/share/fonts/opentype/tlwg/Loma.otf", 0, "Loma");
				addUnix("/usr/share/fonts/truetype/tlwg/Loma.ttf", 0, "Loma");
			}
			// only the ones that exist, each file once
			std::vector<Candidate> out;
			for (auto& c : v) {
				std::error_code ec;
				if (fs::is_regular_file(c.file, ec) &&
					std::none_of(out.begin(), out.end(), [&](const Candidate& o) { return fs::equivalent(o.file, c.file, ec); })) {
					out.push_back(std::move(c));
				}
			}
			return out;
		}

		// font files read once, kept for the whole run (the atlas uses them from memory at every build)
		std::map<std::string, std::vector<char>> g_files;

		const std::vector<char>* FileData(const fs::path& a_file)
		{
			const auto key = a_file.generic_u8string();
			auto       it = g_files.find(std::string(key.begin(), key.end()));
			if (it != g_files.end()) {
				return &it->second;
			}
			std::ifstream     f(a_file, std::ios::binary);
			std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (data.size() < 1024) {
				return nullptr;
			}
			return &g_files.emplace(std::string(key.begin(), key.end()), std::move(data)).first->second;
		}

		// the em size (in pixels) of a font file at an ImGui size: ImGui sizes a font by ascent - descent, and CJK fonts
		// have a much taller ascent + descent than Segoe UI / DejaVu, so the same size drew their characters ~20 % smaller
		float EmPixels(const std::vector<char>& a_data, int a_fontNo, float a_size)
		{
			const auto* d = reinterpret_cast<const unsigned char*>(a_data.data());
			const int   off = stbtt_GetFontOffsetForIndex(d, a_fontNo);
			stbtt_fontinfo info;
			if (off < 0 || !stbtt_InitFont(&info, d, off)) {
				return 0;
			}
			return stbtt_ScaleForPixelHeight(&info, a_size) / stbtt_ScaleForMappingEmToPixels(&info, 1.0f);
		}

		float g_scale = 1;
		int   g_maxTexture = 16384;

		ImFont* g_main = nullptr;
		ImFont* g_mono = nullptr;
		ImFont* g_title = nullptr;

		std::set<unsigned>     g_wanted[kGroups];   // characters asked for, per group
		std::vector<Candidate> g_loaded[kGroups];   // fonts merged for that group (the chain's first, more when it lacks some)
		std::set<std::string>  g_tried[kGroups];    // chain files already merged for that group (also the ones dropped)
		bool                   g_pending = false;
		bool                   g_commonAdded = false;
		bool                   g_monoExtras = true;  // false when the atlas got too big with them in the fixed-width font too
		std::vector<ImWchar>   g_ranges[kGroups];   // the ImGui glyph ranges of g_wanted (must live until the atlas is built)
		std::chrono::steady_clock::time_point g_lastBuild{};
		std::string g_status;

		void Ask(unsigned c)
		{
			const Group g = GroupOf(c);
			if (g == kNone) {
				return;
			}
			if (g_wanted[g].insert(c).second) {
				g_pending = true;
			}
			// the first Chinese character when the interface or the game is Chinese / Japanese: the common ones at once,
			// so reading a list does not build the atlas again for every new character
			if (g == kHan && !g_commonAdded && g_cjk) {
				g_commonAdded = true;
				const ImWchar* r = g_pref == 'J' ? ImGui::GetIO().Fonts->GetGlyphRangesJapanese() : g_pref == 'K' ? nullptr
																											   : ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon();
				for (; r && r[0]; r += 2) {
					for (unsigned k = r[0]; k <= r[1]; ++k) {
						if (GroupOf(k) == kHan) {
							g_wanted[kHan].insert(k);
						}
					}
				}
			}
		}

		void OnMissingGlyph(ImFont*, ImWchar c) { Ask(c); }

		// Latin, Greek, Cyrillic, Vietnamese... (names and places in saves from other languages): Segoe UI / Consolas have them
		const ImWchar* WideRanges()
		{
			static const ImWchar r[] = { 0x0020, 0x024F, 0x0250, 0x02FF, 0x0300, 0x036F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x1E00, 0x1EFF, 0x2000, 0x206F,
				0x20A0, 0x20CF, 0x2100, 0x218F, 0x2190, 0x21FF, 0x2200, 0x22FF, 0x2500, 0x25FF, 0xFFFD, 0xFFFD, 0 };
			return r;
		}

		// the first of these font files that exists (a_data: its file data, for its em size)
		ImFont* AddFirst(const std::vector<const char*>& a_files, float a_size, const ImWchar* a_ranges, bool a_fallbackDefault,
			const std::vector<char>** a_data = nullptr)
		{
			ImGuiIO& io = ImGui::GetIO();
			for (const char* f : a_files) {
				std::error_code ec;
				if (!fs::exists(f, ec)) {
					continue;
				}
				if (const auto* data = FileData(f)) {
					ImFontConfig cfg;
					cfg.FontDataOwnedByAtlas = false;
					if (ImFont* font = io.Fonts->AddFontFromMemoryTTF(const_cast<char*>(data->data()), static_cast<int>(data->size()), a_size, &cfg, a_ranges)) {
						if (a_data) {
							*a_data = data;
						}
						return font;
					}
				}
			}
			if (a_data) {
				*a_data = nullptr;
			}
			if (!a_fallbackDefault) {
				return nullptr;
			}
			ImFontConfig cfg;
			cfg.SizePixels = a_size * 13.0f / 17.0f;
			return io.Fonts->AddFontDefault(&cfg);
		}

		// the wanted characters merged into the font just added
		void MergeExtras(float a_size, const std::vector<char>* a_mainData)
		{
			static const std::vector<char> none;
			if (!a_mainData) {
				a_mainData = &none;
			}
			for (int g = 0; g < kGroups; ++g) {
				if (g_ranges[g].empty()) {
					continue;
				}
				for (const auto& c : g_loaded[g]) {
					const auto* data = FileData(c.file);
					if (!data) {
						continue;
					}
					ImFontConfig cfg;
					cfg.MergeMode = true;
					cfg.FontDataOwnedByAtlas = false;
					cfg.FontNo = c.fontNo;
					// the same em size as the interface font (see EmPixels)
					float size = a_size;
					if (const float mainEm = EmPixels(*a_mainData, 0, a_size), em1 = EmPixels(*data, c.fontNo, 1.0f); mainEm > 0 && em1 > 0) {
						size = mainEm / em1;
					}
					cfg.OversampleH = 1;  // (ImGui's advice for CJK: half the texture, no visible difference)
					cfg.PixelSnapH = true;
					ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<char*>(data->data()), static_cast<int>(data->size()), size, &cfg,
						g_ranges[g].data());
				}
			}
		}

		void BuildOnce()
		{
			for (int g = 0; g < kGroups; ++g) {
				g_ranges[g].clear();
				for (auto it = g_wanted[g].begin(); it != g_wanted[g].end();) {
					const unsigned first = *it;
					unsigned       last = first;
					for (++it; it != g_wanted[g].end() && *it == last + 1; ++it) {
						last = *it;
					}
					g_ranges[g].push_back(static_cast<ImWchar>(first));
					g_ranges[g].push_back(static_cast<ImWchar>(last));
				}
				if (!g_ranges[g].empty()) {
					g_ranges[g].push_back(0);
				}
			}
			ImGuiIO& io = ImGui::GetIO();
			io.Fonts->Clear();
			const std::vector<char>* mainData = nullptr;
			g_main = AddFirst({ "C:\\Windows\\Fonts\\segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf" },
				17.0f * g_scale, WideRanges(), true, &mainData);
			MergeExtras(17.0f * g_scale, mainData);
			// fixed-width font for the script viewer
			g_mono = AddFirst({ "C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\cour.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
								  "/usr/share/fonts/TTF/DejaVuSansMono.ttf" },
				16.0f * g_scale, WideRanges(), true, &mainData);
			if (g_monoExtras) {
				MergeExtras(16.0f * g_scale, mainData);
			}
			// the start page logo: a big bold font with only the characters it needs (a big font with every character
			// would take a lot of texture memory): space to Z, capitals, digits, +
			static const ImWchar titleRanges[] = { 0x0020, 0x005A, 0 };
			g_title = AddFirst({ "C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
								   "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf" },
				110.0f * g_scale, titleRanges, false);
			io.Fonts->Build();
		}

		void Build()
		{
			// the first font of each chain for the groups with characters; more of the chain while characters are missing
			for (int round = 0; round < 4; ++round) {
				bool more = false;
				for (int g = 0; g < kGroups; ++g) {
					if (!g_wanted[g].empty() && g_loaded[g].empty()) {
						for (auto& c : Chain(static_cast<Group>(g))) {
							if (g_tried[g].insert(c.file.generic_string()).second) {
								g_loaded[g].push_back(std::move(c));
								break;
							}
						}
					}
				}
				BuildOnce();
				for (int g = 0; g < kGroups && g_main; ++g) {
					std::size_t missing = 0;
					for (unsigned c : g_wanted[g]) {
						missing += g_main->FindGlyphNoFallback(static_cast<ImWchar>(c)) == nullptr;
					}
					// a second font of the chain for what the first lacks (at most 2 per group: each costs memory)
					if (missing && g_loaded[g].size() < 2) {
						for (auto& c : Chain(static_cast<Group>(g))) {
							if (g_tried[g].insert(c.file.generic_string()).second) {
								g_loaded[g].push_back(std::move(c));
								more = true;
								break;
							}
						}
					}
				}
				// too big a texture for the graphics driver: the extra characters only in the interface font
				auto* atlas = ImGui::GetIO().Fonts;
				if ((atlas->TexHeight > g_maxTexture || atlas->TexWidth > g_maxTexture) && g_monoExtras) {
					g_monoExtras = false;
					more = true;
				}
				if (!more) {
					break;
				}
			}
			g_pending = false;
			g_lastBuild = std::chrono::steady_clock::now();
			// status
			auto*       atlas = ImGui::GetIO().Fonts;
			std::string s;
			const char* names[kGroups] = { "Chinese/Japanese", "Korean", "Thai" };
			for (int g = 0; g < kGroups; ++g) {
				if (g_wanted[g].empty()) {
					continue;
				}
				std::size_t found = 0;
				for (unsigned c : g_wanted[g]) {
					found += g_main && g_main->FindGlyphNoFallback(static_cast<ImWchar>(c)) != nullptr;
				}
				std::string fontsUsed;
				for (const auto& c : g_loaded[g]) {
					const auto f = c.file.filename().u8string();
					fontsUsed += (fontsUsed.empty() ? "" : ", ") + c.name + " (" + std::string(f.begin(), f.end()) + ")";
				}
				char buf[160];
				std::snprintf(buf, sizeof(buf), "%s: %zu of %zu characters from ", names[g], found, g_wanted[g].size());
				s += (s.empty() ? "" : "; ") + std::string(buf) + (fontsUsed.empty() ? "no font found" : fontsUsed);
			}
			char buf[96];
			std::snprintf(buf, sizeof(buf), "%sfont atlas %d x %d%s", s.empty() ? "" : "; ", atlas->TexWidth, atlas->TexHeight,
				g_monoExtras ? "" : " (fixed-width font without them: too big)");
			g_status = (s.empty() ? std::string("no extra characters") + "; " : s) + buf;
		}
	}

	void Init(float a_scale, int a_maxTexture)
	{
		g_scale = a_scale;
		g_maxTexture = a_maxTexture > 0 ? a_maxTexture : 16384;
		GImFontMissingGlyphHook = OnMissingGlyph;
		Build();
	}

	void SetPreference(const std::string& a_ui, unsigned a_cp, const std::string& a_game)
	{
		std::string key = a_ui + "|" + std::to_string(a_cp) + "|" + a_game;
		if (key == g_prefKey) {
			return;
		}
		g_prefKey = key;
		char        pref = 'S';
		bool        cjk = false;
		std::string game;
		for (char c : a_game) {
			game.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
		if (a_ui.rfind("zh", 0) == 0) {
			pref = a_ui.find("Hant") != std::string::npos || a_ui.find("TW") != std::string::npos || a_ui.find("HK") != std::string::npos ? 'T' : 'S';
			cjk = true;
		} else if (a_ui == "ja") {
			pref = 'J', cjk = true;
		} else if (a_ui == "ko") {
			pref = 'K', cjk = true;
		} else if (a_cp == 936 || a_cp == 950 || a_cp == 932 || a_cp == 949) {
			pref = a_cp == 950 ? 'T' : a_cp == 932 ? 'J' : a_cp == 949 ? 'K' : 'S';
			cjk = true;
		} else if (game == "chinese" || game == "cn" || game == "zhhans" || game == "zhhant" || game == "japanese" || game == "ja" ||
				   game == "korean" || game == "ko") {
			pref = game == "zhhant" ? 'T' : (game == "japanese" || game == "ja") ? 'J' : (game == "korean" || game == "ko") ? 'K' : 'S';
			cjk = true;
		}
		g_cjk = cjk;
		if (pref != g_pref) {
			g_pref = pref;
			// another font first for Chinese characters: choose again at the next build
			g_loaded[kHan].clear();
			g_tried[kHan].clear();
			if (!g_wanted[kHan].empty()) {
				g_pending = true;
			}
		}
	}

	void Want(const std::string& a_utf8)
	{
		for (std::size_t i = 0; i < a_utf8.size();) {
			const auto c = static_cast<unsigned char>(a_utf8[i]);
			const int  n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
			unsigned   u = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
			for (int k = 1; k < n && i + k < a_utf8.size(); ++k) {
				u = (u << 6) | (static_cast<unsigned char>(a_utf8[i + k]) & 0x3F);
			}
			i += static_cast<std::size_t>(n);
			if (u >= 0x0E00 && u <= 0xFFFF) {
				Ask(u);
			}
		}
	}

	bool Rebuild()
	{
		if (!g_pending) {
			return false;
		}
		if (std::chrono::steady_clock::now() - g_lastBuild < std::chrono::milliseconds(150)) {
			return false;
		}
		Build();
		return true;
	}

	ImFont*     Mono() { return g_mono; }
	ImFont*     Title() { return g_title; }
	std::string Status() { return g_status; }
}
