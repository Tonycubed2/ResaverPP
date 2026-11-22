// ReSaver++ - copy any text in the window, drag-select (see TextCopy.h).
#include "TextCopy.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

extern void (*GImGuiTextHook)(const char* text_begin, const char* text_end, const ImVec2& min, const ImVec2& max, float wrap_width,
	bool selectable, bool visible);

namespace textcopy
{
	namespace
	{
		// ---------------------------------------------------------------- one line / whole window
		std::string  g_line;  // the line under the mouse this frame
		bool         g_lineIsText = false;  // ... plain text (not a list row's label): a drag can start on it
		ImGuiID      g_lineWindow = 0;
		std::string  g_all;  // every line shown in the window under the mouse this frame
		ImGuiWindow* g_allRoot = nullptr;
		float        g_lastY = -1e9f, g_lastX = 0.0f;
		std::string  g_note;  // "Copied: ..."
		double       g_noteUntil = 0.0;

		// ---------------------------------------------------------------- drag selection
		// points are in the window's content coordinates (screen - window pos + scroll): they stay put when it scrolls
		struct Selection
		{
			ImGuiID window = 0;
			ImVec2  a{}, b{};          // where the drag started, where the mouse is (or was let go)
			bool    pending = false;   // mouse pressed on text, not moved far enough yet
			bool    dragging = false;
			bool    shown = false;     // a finished selection still highlighted
		} g_sel;
		std::string g_selText;  // the selected text, rebuilt every frame while drawing
		float       g_selLastY = -1e9f, g_selLastX = 0.0f;

		ImVec2 ToContent(ImGuiWindow* w, const ImVec2& p) { return ImVec2(p.x - w->Pos.x + w->Scroll.x, p.y - w->Pos.y + w->Scroll.y); }
		ImVec2 ToScreen(ImGuiWindow* w, const ImVec2& p) { return ImVec2(p.x + w->Pos.x - w->Scroll.x, p.y + w->Pos.y - w->Scroll.y); }

		// a before b in reading order (rows first, then left to right)
		bool Before(const ImVec2& a, const ImVec2& b, float a_lineH)
		{
			if (std::fabs(a.y - b.y) < a_lineH * 0.5f) {
				return a.x <= b.x;
			}
			return a.y < b.y;
		}

		// the byte index in [a_s, a_e) whose glyph a_x falls in (nearest letter boundary)
		std::size_t IndexAt(const char* a_s, const char* a_e, float a_x)
		{
			if (a_x <= 0.0f) {
				return 0;
			}
			const char* p = a_s;
			float       x = 0.0f;
			while (p < a_e) {
				unsigned int c = 0;
				const int    n = ImTextCharFromUtf8(&c, p, a_e);
				const float  w = ImGui::CalcTextSize(p, p + (n > 0 ? n : 1)).x;
				if (a_x < x + w * 0.5f) {
					break;
				}
				x += w;
				p += n > 0 ? n : 1;
			}
			return static_cast<std::size_t>(p - a_s);
		}

		void AddSelected(const std::string& a_piece, const ImVec2& a_contentMin)
		{
			if (!g_selText.empty()) {
				g_selText += std::fabs(a_contentMin.y - g_selLastY) < 2.0f && a_contentMin.x > g_selLastX ? "\t" : "\n";
			}
			g_selText += a_piece;
			g_selLastY = a_contentMin.y;
			g_selLastX = a_contentMin.x;
		}

		// the selected part of one text item: highlighted, and added to g_selText
		void SelectIn(ImGuiWindow* w, const char* a_begin, const char* a_end, const ImVec2& a_min, float a_wrap, bool a_visible)
		{
			ImGuiContext& g = *GImGui;
			const float   lineH = g.FontSize;
			ImVec2        s = g_sel.a, e = g_sel.b;
			if (!Before(s, e, lineH)) {
				std::swap(s, e);
			}
			const ImVec2 origin = ToContent(w, a_min);
			ImFont*     font = g.Font;
			const float scale = g.FontSize / font->FontSize;
			float       y = origin.y;
			const char* p = a_begin;
			while (p <= a_end) {
				// one source line (up to '\n'), wrapped as ImFont::RenderText does
				const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(a_end - p)));
				const char* srcEnd = nl ? nl : a_end;
				const char* q = p;
				do {
					const char* eol = a_wrap > 0.0f ? font->CalcWordWrapPositionA(scale, q, srcEnd, a_wrap) : srcEnd;
					if (eol <= q && q < srcEnd) {
						eol = q + 1;  // never stuck
					}
					// this visual line: [q, eol) at (origin.x, y)
					if (!(e.y < y) && !(s.y >= y + lineH)) {
						const bool  startRow = s.y >= y && s.y < y + lineH;
						const bool  endRow = e.y >= y && e.y < y + lineH;
						std::size_t from = startRow ? IndexAt(q, eol, s.x - origin.x) : 0;
						std::size_t to = endRow ? IndexAt(q, eol, e.x - origin.x) : static_cast<std::size_t>(eol - q);
						if (to > from) {
							const float x0 = ImGui::CalcTextSize(q, q + from).x;
							const float x1 = ImGui::CalcTextSize(q, q + to).x;
							const ImVec2 sMin = ToScreen(w, ImVec2(origin.x + x0, y));
							const ImVec2 sMax = ToScreen(w, ImVec2(origin.x + x1, y + lineH));
							ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_TextSelectedBg);
							if (a_visible) {
								w->DrawList->AddRectFilled(sMin, sMax, ImGui::GetColorU32(col));
							}
							AddSelected(std::string(q + from, q + to), ImVec2(origin.x + x0, y));
						} else if (!startRow && !endRow && eol == q) {
							AddSelected(std::string(), ImVec2(origin.x, y));  // an empty line inside the selection
						}
					}
					y += lineH;
					q = eol;
					while (q < srcEnd && (*q == ' ' || *q == '\t')) {
						++q;  // wrapping skips the blanks at the start of the next line
					}
				} while (q < srcEnd);
				if (!nl) {
					break;
				}
				p = nl + 1;
				if (p == a_end) {
					break;
				}
			}
		}

		void Hook(const char* a_begin, const char* a_end, const ImVec2& a_min, const ImVec2& a_max, float a_wrap, bool a_selectable,
			bool a_visible)
		{
			ImGuiContext& g = *GImGui;
			ImGuiWindow*  w = g.CurrentWindow;
			if (!w || !a_begin || a_end < a_begin) {
				return;
			}
			if (!a_selectable && (g_sel.dragging || g_sel.shown) && w->ID == g_sel.window && a_end > a_begin) {
				SelectIn(w, a_begin, a_end, a_min, a_wrap, a_visible);
			}
			if (!g.HoveredWindow || a_end == a_begin) {
				return;
			}
			std::string text(a_begin, a_end);
			const ImGuiIO& io = ImGui::GetIO();
			if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyDown(ImGuiKey_C) && w->RootWindow == g.HoveredWindow->RootWindow) {  // only when asked
				if (g_allRoot != w->RootWindow) {
					g_allRoot = w->RootWindow;
					g_all.clear();
					g_lastY = -1e9f;
				}
				if (!g_all.empty()) {
					g_all += std::fabs(a_min.y - g_lastY) < 2.0f && a_min.x > g_lastX ? "\t" : "\n";
				}
				g_all += text;
				g_lastY = a_min.y;
				g_lastX = a_min.x;
			}
			if (a_visible && g.HoveredWindow == w && ImGui::IsMouseHoveringRect(a_min, a_max, false)) {
				g_line = std::move(text);
				g_lineIsText = !a_selectable;
				g_lineWindow = w->ID;
			}
		}

		void Copy(const std::string& a_text, const char* a_what)
		{
			ImGui::SetClipboardText(a_text.c_str());
			std::string shown = a_text.substr(a_text.find_first_not_of(" \t\n") == std::string::npos ? 0 : a_text.find_first_not_of(" \t\n"));
			shown = shown.size() > 60 ? shown.substr(0, 57) + "..." : shown;
			for (auto& c : shown) {
				if (c == '\n' || c == '\t') {
					c = ' ';
				}
			}
			g_note = std::string(a_what) + shown;
			g_noteUntil = ImGui::GetTime() + 1.5;
		}
	}

	void Install()
	{
		GImGuiTextHook = &Hook;
		ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;  // a drag over text selects it instead of moving the window
	}

	void EndFrame()
	{
		ImGuiContext& g = *GImGui;
		ImGuiIO&      io = ImGui::GetIO();
		const bool    typing = io.WantTextInput;
		ImGuiWindow*  selWin = g_sel.window ? ImGui::FindWindowByID(g_sel.window) : nullptr;

		// ---- drag selection
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			g_sel = {};
			if (g_lineIsText && !g_line.empty() && g.ActiveId == 0) {
				if (auto* w = ImGui::FindWindowByID(g_lineWindow)) {
					g_sel.window = g_lineWindow;
					g_sel.a = g_sel.b = ToContent(w, io.MousePos);
					g_sel.pending = true;
				}
			}
		} else if (g_sel.pending || g_sel.dragging) {
			if (!selWin) {
				g_sel = {};
			} else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
				// ImGui makes a click on a window's empty space or on text "move the window" (ActiveId = its MoveId) even when only
				// the title bar moves it: that is not a widget taking the click
				const bool moveId = g.ActiveId == selWin->MoveId || g.ActiveId == selWin->RootWindow->MoveId ||
				                    (g.ActiveIdWindow && g.ActiveId == g.ActiveIdWindow->MoveId);
				if (g_sel.pending && g.ActiveId != 0 && !moveId) {
					g_sel = {};  // a widget took the click (a list row, a button): not a text drag
				} else {
					g_sel.b = ToContent(selWin, io.MousePos);
					if (g_sel.pending && std::fabs(g_sel.b.x - g_sel.a.x) + std::fabs(g_sel.b.y - g_sel.a.y) > 4.0f) {
						g_sel.pending = false;
						g_sel.dragging = true;
					}
					if (g_sel.dragging) {
						// past the top or bottom: scroll
						const float top = selWin->InnerRect.Min.y, bottom = selWin->InnerRect.Max.y;
						if (io.MousePos.y < top) {
							ImGui::SetScrollY(selWin, std::max(0.0f, selWin->Scroll.y - (top - io.MousePos.y) * 0.5f));
						} else if (io.MousePos.y > bottom) {
							ImGui::SetScrollY(selWin, std::min(selWin->ScrollMax.y, selWin->Scroll.y + (io.MousePos.y - bottom) * 0.5f));
						}
					}
				}
			} else {  // let go
				if (g_sel.dragging) {
					g_sel.dragging = false;
					g_sel.shown = true;
					if (!g_selText.empty()) {
						Copy(g_selText, "Copied: ");
					}
				} else {
					g_sel = {};
				}
			}
		}
		const std::string selected = g_selText;
		g_selText.clear();
		g_selLastY = -1e9f;

		// ---- keys and right-click
		if (!typing && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false) && !g_all.empty()) {
			Copy(g_all, "Copied the whole window: ");
		} else if (!typing && io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false) && g_sel.shown && !selected.empty()) {
			Copy(selected, "Copied: ");
		} else if (!typing && io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false) && !g_line.empty()) {
			Copy(g_line, "Copied: ");
		} else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !g_line.empty() &&
				   !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
			Copy(g_line, "Copied: ");
		}
		if (!g_note.empty() && ImGui::GetTime() < g_noteUntil) {
			ImGui::SetTooltip("%s", g_note.c_str());
		}
		g_line.clear();
		g_lineIsText = false;
		g_all.clear();
		g_allRoot = nullptr;
	}
}
