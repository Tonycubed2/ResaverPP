// ReSaver++ - copy any text in the window (Steamvikings asked to copy form IDs and names instead of typing them into
// xEdit). Every text ImGui draws passes through a hook (third_party/imgui TextEx and Selectable) before it is drawn:
//  - press the mouse on text and drag: the text passed over is highlighted, letter-exact, like in a document, and
//    copied when the mouse is let go (table cells on one row joined with tabs, rows with new lines); the window
//    scrolls when the mouse goes past its top or bottom; Ctrl+C copies the selection again; a click elsewhere clears it;
//  - right-click a line of text: that line is copied (unless the right-click opened a menu of its own);
//  - Ctrl+C with the mouse over a line (and nothing selected): that line;
//  - Ctrl+Shift+C: all the text of the window under the mouse (lines scrolled out of view too, except in long lists
//    that only draw what is on screen).
// A small "Copied" note shows next to the mouse. Windows move by their title bar only, so dragging over text selects
// it instead of moving the window.
#pragma once

namespace textcopy
{
	void Install();   // once, after ImGui::CreateContext
	void EndFrame();  // every frame, after the app has drawn, before ImGui::Render
}
