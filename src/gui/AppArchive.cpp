// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// File > Restore an archived save: the last 10 saves ReSaver Plus wrote over or edited, with their
// screenshots, and putting one back.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}
	std::string When(std::int64_t a_unix)
	{
		if (a_unix <= 0) {
			return "?";
		}
		const std::time_t t = static_cast<std::time_t>(a_unix);
		std::tm           tm{};
#ifdef _WIN32
		localtime_s(&tm, &t);
#else
		localtime_r(&t, &tm);
#endif
		char b[64];
		std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tm);
		return b;
	}
	std::string Ago(std::int64_t a_unix)
	{
		const auto d = static_cast<std::int64_t>(std::time(nullptr)) - a_unix;
		if (d < 90) {
			return "just now";
		}
		if (d < 5400) {
			return Fmt("%lld minutes ago", static_cast<long long>(d / 60));
		}
		if (d < 129600) {
			return Fmt("%lld hours ago", static_cast<long long>(d / 3600));
		}
		return Fmt("%lld days ago", static_cast<long long>(d / 86400));
	}
	std::string MB(std::uintmax_t a_bytes) { return Fmt("%.1f MB", a_bytes / 1048576.0); }

	// the save's screenshot, read from the start of the file only: RGBA pixels, width, height
	bool ReadShot(const std::filesystem::path& a_ess, std::vector<std::uint8_t>& a_rgba, int& a_w, int& a_h)
	{
		std::ifstream f(a_ess, std::ios::binary);
		unsigned char head[17];
		if (!f.read(reinterpret_cast<char*>(head), 17) || std::memcmp(head, "TESV_SAVEGAME", 13) != 0) {
			return false;
		}
		std::uint32_t hs = 0;
		std::memcpy(&hs, head + 13, 4);
		if (hs < 30 || hs > 65536) {
			return false;
		}
		std::vector<std::uint8_t> h(hs);
		if (!f.read(reinterpret_cast<char*>(h.data()), hs)) {
			return false;
		}
		std::uint32_t version = 0;
		std::memcpy(&version, h.data(), 4);
		const bool    le = version < 12;
		const std::size_t end = le ? hs : hs - 2;  // SE ends with the compression field
		std::uint32_t w = 0, ht = 0;
		std::memcpy(&w, h.data() + end - 8, 4);
		std::memcpy(&ht, h.data() + end - 4, 4);
		if (w == 0 || ht == 0 || w > 4096 || ht > 4096) {
			return false;
		}
		const std::size_t bpp = le ? 3 : 4, n = std::size_t(w) * ht;
		std::vector<std::uint8_t> px(n * bpp);
		if (!f.read(reinterpret_cast<char*>(px.data()), static_cast<std::streamsize>(px.size()))) {
			return false;
		}
		a_rgba.assign(n * 4, 255);
		for (std::size_t i = 0; i < n; ++i) {
			for (std::size_t c = 0; c < 3; ++c) {
				a_rgba[i * 4 + c] = px[i * bpp + c];
			}
		}
		a_w = static_cast<int>(w);
		a_h = static_cast<int>(ht);
		return true;
	}
}

std::filesystem::path App::archiveFolder() const { return platform::SettingsFile().parent_path() / "ReSaverPP Archive"; }

void App::freeArchiveShots()
{
	for (auto& s : _archiveShots) {
		if (s.tex && freeTexture) {
			freeTexture(s.tex);
		}
	}
	_archiveShots.clear();
}

void App::loadArchive()
{
	freeArchiveShots();
	_archive = archive::List(archiveFolder());
	_archiveShots.resize(_archive.size());
	for (std::size_t i = 0; i < _archive.size(); ++i) {
		std::vector<std::uint8_t> rgba;
		int                       w = 0, h = 0;
		if (uploadTexture && ReadShot(_archive[i].ess, rgba, w, h)) {
			_archiveShots[i] = { uploadTexture(rgba.data(), w, h), w, h };
		}
	}
	_archiveSel = std::clamp(_archiveSel, 0, std::max(0, static_cast<int>(_archive.size()) - 1));
	_archiveLoaded = true;
}

// Before a save is written: keep a copy of the save that was opened. Never stops the save.
void App::archiveBeforeWrite(const std::filesystem::path& a_target)
{
	if (_path.empty()) {
		return;
	}
	try {
		std::error_code ec;
		const bool      over = std::filesystem::exists(a_target, ec) && std::filesystem::equivalent(_path, a_target, ec);
		if (archive::Add(archiveFolder(), _path, a_target, over ? "Saved over it" : "Saved as another file")) {
			_archiveNote = "  (the original is in File > Restore an archived save)";
		} else {
			_archiveNote.clear();
		}
		_archiveLoaded = false;
	} catch (const std::exception& e) {
		_archiveNote = std::string("  (could not archive the original: ") + e.what() + ")";
	}
}

void App::drawArchive()
{
	static const bool testSel = [this] {  // screenshots: RESAVERPP_TEST_ARCHIVE=<n> selects entry n
		if (const char* t = std::getenv("RESAVERPP_TEST_ARCHIVE")) {
			_archiveSel = std::atoi(t);
		}
		return true;
	}();
	(void)testSel;
	prepareWindow("Restore an archived save", ImVec2(0.8f, 0.85f));
	if (!ImGui::Begin("Restore an archived save", &_showArchive, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Restore an archived save");
	ImGui::NewLine();
	if (!_archiveLoaded) {
		loadArchive();
	}
	ImGui::TextWrapped("Every time ReSaver Plus writes a save, it first copies the save you opened (with its .skse file) here. The last %zu "
					   "are kept; when another is added, the oldest is deleted. Pick one to see its screenshot and details, then put it back.",
		archive::kKeep);
	ImGui::TextDisabled("Archive folder: %s", platform::ToUtf8(archiveFolder()).c_str());
	if (ImGui::SmallButton("Open the archive folder")) {
		std::error_code ec;
		std::filesystem::create_directories(archiveFolder(), ec);
		platform::OpenWithDefaultApp(archiveFolder());
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Read it again")) {
		_archiveLoaded = false;
	}
	if (!_archiveMsg.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::Good()));
		ImGui::TextWrapped("%s", _archiveMsg.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::Separator();
	if (_archive.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("Nothing archived yet. The first time you save from ReSaver Plus (Save, Save As or a cleanup), the save you opened is "
						   "copied here first.");
		ImGui::End();
		return;
	}

	// keyboard: up/down or left/right go through the list
	if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
		if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
			_archiveSel = std::min(_archiveSel + 1, static_cast<int>(_archive.size()) - 1);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
			_archiveSel = std::max(_archiveSel - 1, 0);
		}
	}

	const float listW = std::max(ImGui::GetContentRegionAvail().x * 0.38f, 320.0f);
	ImGui::BeginChild("##archlist", ImVec2(listW, 0), ImGuiChildFlags_Borders);
	const float thumbW = ImGui::GetFontSize() * 7.5f;
	for (int i = 0; i < static_cast<int>(_archive.size()); ++i) {
		const auto& e = _archive[i];
		const auto& s = _archiveShots[i];
		ImGui::PushID(i);
		const float thumbH = s.tex ? thumbW * s.h / std::max(1, s.w) : thumbW * 0.56f;
		const float rowH = std::max(thumbH, ImGui::GetTextLineHeightWithSpacing() * 4) + 6;
		const ImVec2 at = ImGui::GetCursorPos();
		if (ImGui::Selectable("##row", _archiveSel == i, ImGuiSelectableFlags_None, ImVec2(0, rowH))) {
			_archiveSel = i;
		}
		if (_archiveSel == i && ImGui::IsWindowAppearing()) {
			ImGui::SetScrollHereY();
		}
		ImGui::SetCursorPos(ImVec2(at.x + 3, at.y + 3));
		if (s.tex) {
			ImGui::Image(static_cast<ImTextureID>(s.tex), ImVec2(thumbW, thumbH));
		} else {
			ImGui::Dummy(ImVec2(thumbW, thumbH));
		}
		ImGui::SetCursorPos(ImVec2(at.x + thumbW + 12, at.y + 3));
		ImGui::BeginGroup();
		ImGui::Text("%d.  %s, level %u", i + 1, e.character.empty() ? "(unreadable)" : e.character.c_str(), e.level);
		ImGui::TextDisabled("%s", e.location.c_str());
		ImGui::TextDisabled("archived %s (%s)", When(e.archived).c_str(), Ago(e.archived).c_str());
		ImGui::TextDisabled("%s", e.reason.c_str());
		ImGui::EndGroup();
		ImGui::SetCursorPos(ImVec2(at.x, at.y + rowH + ImGui::GetStyle().ItemSpacing.y));
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	ImGui::BeginChild("##archdetail", ImVec2(0, 0), ImGuiChildFlags_Borders);
	const auto& e = _archive[_archiveSel];
	const auto& s = _archiveShots[_archiveSel];
	if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) {
		_archiveSel = std::max(_archiveSel - 1, 0);
	}
	tips::Tip("The newer archived save (or press the up / left arrow key).");
	ImGui::SameLine();
	ImGui::Text("%d of %zu", _archiveSel + 1, _archive.size());
	ImGui::SameLine();
	if (ImGui::ArrowButton("##next", ImGuiDir_Right)) {
		_archiveSel = std::min(_archiveSel + 1, static_cast<int>(_archive.size()) - 1);
	}
	tips::Tip("The older archived save (or press the down / right arrow key).");
	ImGui::SameLine();
	ImGui::TextDisabled("  newest first");
	if (s.tex) {
		// as wide as there is room, but no taller than half the window, so the buttons and details stay in view
		float w = std::min(ImGui::GetContentRegionAvail().x, static_cast<float>(s.w) * 2.0f);
		const float maxH = ImGui::GetWindowHeight() * 0.5f;
		if (w * s.h / std::max(1, s.w) > maxH) {
			w = maxH * s.w / std::max(1, s.h);
		}
		ImGui::Image(static_cast<ImTextureID>(s.tex), ImVec2(w, w * s.h / std::max(1, s.w)));
	} else {
		ImGui::TextDisabled("(no screenshot)");
	}
	ImGui::Spacing();
	drawArchiveButtons(e);
	ImGui::Spacing();
	if (ImGui::BeginTable("##archinfo", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
		ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("File's own date").x + 12);
		ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
		auto row = [](const char* a_k, const std::string& a_v) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(a_k);
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", a_v.c_str());
		};
		row("Character", e.character + ", level " + std::to_string(e.level) + (e.race.empty() ? "" : " (" + e.race + ")"));
		row("Location", e.location);
		row("Save number", std::to_string(e.saveNumber));
		row("Archived", When(e.archived) + " (" + Ago(e.archived) + ")");
		row("Why", e.reason);
		row("Save file", platform::ToUtf8(e.source.filename()) + "\n" + platform::ToUtf8(e.source.parent_path()));
		row("Saved from it", e.savedAs.empty() ? std::string("-") : platform::ToUtf8(e.savedAs.filename()));
		row("File's own date", When(e.modified));
		row("Size", MB(e.size) + (e.skse.empty() ? std::string("  (no .skse co-save)") : "  + .skse " + MB(e.cosaveSize)));
		ImGui::EndTable();
	}
	ImGui::EndChild();
	ImGui::End();
}

void App::drawArchiveButtons(const ess::archive::Entry& e)
{
	std::error_code ec;
	const bool      there = std::filesystem::exists(e.source, ec);
	if (ImGui::Button("Put it back in its place")) {
		confirm("Put the archived save back",
			"Copy this save back to\n" + platform::ToUtf8(e.source) +
				(there ? "\n\nThe save there now is archived first, so you can undo this from this same window." : ""),
			[this, entry = e] {
				try {
					const auto p = archive::RestoreInPlace(archiveFolder(), entry);
					_archiveMsg = "Put back: " + platform::ToUtf8(p.filename()) + ". Load it in the game, or open it here.";
					_archiveSel = 0;
				} catch (const std::exception& ex) {
					_error = std::string("Could not put the save back:\n\n") + ex.what();
				}
				_archiveLoaded = false;
			});
	}
	tips::Tip("Copies the archived save (and its .skse) back to the file it came from. If that file is there, it is archived first, so nothing is lost.");
	ImGui::SameLine();
	if (ImGui::Button("Restore it as a new save")) {
		try {
			const auto p = archive::RestoreAsCopy(e);
			_archiveMsg = "Restored as " + platform::ToUtf8(p.filename()) + ", next to the original. It shows in the game's load menu.";
		} catch (const std::exception& ex) {
			_error = std::string("Could not restore the save:\n\n") + ex.what();
		}
	}
	tips::Tip("Copies the archived save next to the original under a new name (with \"restored\" in it), leaving every other save alone.");
	ImGui::SameLine();
	if (ImGui::Button("Restore as a new save and open it")) {
		try {
			const auto p = archive::RestoreAsCopy(e);
			_archiveMsg = "Restored as " + platform::ToUtf8(p.filename()) + " and opened.";
			if (!_dirty) {
				open(p);
			} else {
				confirm("Unsaved changes", "The open save has edits that are not written. Open the restored save anyway?", [this, p] { open(p); });
			}
		} catch (const std::exception& ex) {
			_error = std::string("Could not restore the save:\n\n") + ex.what();
		}
	}
	tips::Tip("Restores it as a new save (as the button before) and opens it here to look at or clean.");
}
