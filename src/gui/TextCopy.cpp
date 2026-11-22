// ReSaver++ - copy any text in the window (see TextCopy.h).
#include "TextCopy.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <string>

extern void (*GImGuiTextHook)(const char* text_begin, const char* text_end, const ImVec2& min, const ImVec2& max);

namespace textcopy
{
	namespace
	{
		std::string  g_line;         // the line under the mouse this frame
		std::string  g_all;          // every line shown in the window under the mouse this frame
		ImGuiWindow* g_allRoot = nullptr;
		float        g_lastY = -1e9f, g_lastX = 0.0f;
		std::string  g_note;  // "Copied: ..."
		double       g_noteUntil = 0.0;

		void Hook(const char* a_begin, const char* a_end, const ImVec2& a_min, const ImVec2& a_max)
		{
			ImGuiContext& g = *GImGui;
			ImGuiWindow*  w = g.CurrentWindow;
			if (!w || !g.HoveredWindow || !a_begin || a_end <= a_begin) {
				return;
			}
			std::string text(a_begin, a_end);
			if (w->RootWindow == g.HoveredWindow->RootWindow) {
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
			if (g.HoveredWindow == w && ImGui::IsMouseHoveringRect(a_min, a_max, false)) {
				g_line = std::move(text);
			}
		}

		void Copy(const std::string& a_text, const char* a_what)
		{
			ImGui::SetClipboardText(a_text.c_str());
			std::string shown = a_text.size() > 60 ? a_text.substr(0, 57) + "..." : a_text;
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
	}

	void EndFrame()
	{
		ImGuiIO& io = ImGui::GetIO();
		const bool typing = io.WantTextInput;
		if (!typing && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false) && !g_all.empty()) {
			Copy(g_all, "Copied the whole window: ");
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
		g_all.clear();
		g_allRoot = nullptr;
	}
}
