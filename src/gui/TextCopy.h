// ReSaver++ - copy any text in the window (version: Steamvikings asked to copy form IDs and names instead of typing
// them into xEdit). Every line of text ImGui draws passes through a hook (third_party/imgui TextEx and Selectable):
//  - right-click a line of text: that line is copied (unless the right-click opened a menu of its own);
//  - Ctrl+C with the mouse over a line: the same;
//  - Ctrl+Shift+C: every line shown in the window under the mouse (table rows joined with tabs).
// A small "Copied" note shows next to the mouse.
#pragma once

namespace textcopy
{
	void Install();   // once, after ImGui::CreateContext
	void EndFrame();  // every frame, after the app has drawn, before ImGui::Render
}
