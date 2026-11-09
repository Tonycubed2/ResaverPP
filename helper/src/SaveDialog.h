#pragma once

// The Windows "Save As" dialog for the in-game page (F1, MenuPage.cpp) (version 15): lets the player choose where a text
// file goes. The dialog runs on its own thread (COM initialised there, single-threaded apartment), never on the game's
// main thread, so the game keeps drawing while it is open. It has no owner window on purpose: a window owned by a window
// of another thread attaches the two threads' input (keyboard focus, the cursor's show / hide count), and the game hides
// the cursor. Instead it is put on top and brought to the front when it opens, and the game window is brought back to
// the front when it closes.
//
// When the dialog cannot be shown (the game runs in exclusive fullscreen: Windows would have to switch the game out of
// its display mode; or COM / the dialog fails), the file is written to the default folder and name instead.
namespace SaveDialog
{
	struct Result
	{
		enum class Kind
		{
			kSaved,      // the player chose a place; written there
			kDefault,    // no dialog (exclusive fullscreen or a dialog error): written to the default place
			kCancelled,  // the player closed the dialog: nothing written
			kFailed,     // the file could not be written (message says why)
		};
		Kind                  kind = Kind::kFailed;
		std::filesystem::path path;     // the file written (kSaved, kDefault)
		std::string           message;  // one line for the page's status
	};

	// true while a dialog is open (one at a time)
	bool Busy();

	// Starts the dialog thread: asks where to save a_text (UTF-8), proposing a_folder\a_name, writes the file, then calls
	// a_done on that thread. a_what names the file in messages ("the report"). Returns false (and calls nothing) when a
	// dialog is already open.
	bool SaveText(std::filesystem::path a_folder, std::filesystem::path a_name, std::string a_text, std::string a_what,
		std::function<void(const Result&)> a_done);

	// the player's Desktop (FOLDERID_Desktop), or "" when Windows does not say
	std::filesystem::path Desktop();

	// Windows reports a Direct3D program in exclusive fullscreen (SHQueryUserNotificationState: QUNS_RUNNING_D3D_FULL_SCREEN)
	bool ExclusiveFullscreen();

	// writes a_text to a_path (binary, replaces the file); false and a_error when it cannot
	bool WriteText(const std::filesystem::path& a_path, const std::string& a_text, std::string& a_error);

	// a path as UTF-8 text, for the page (ImGui draws UTF-8; path::string() would use the ANSI code page)
	std::string PathText(const std::filesystem::path& a_path);
}
