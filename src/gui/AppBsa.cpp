// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// File > Unpack a BSA archive: the files inside a BSA (Skyrim SE/AE and Legendary Edition), filtered by
// name, unpacked into a folder of your choice (all of them or the ones shown); double-click a script to read
// its decompiled code, any other file to open it with its program.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"
#include "core/Bsa.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace ess;

namespace
{
	std::string Fmt(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	void TextColored(std::uint32_t col, const std::string& s)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, col);
		ImGui::TextUnformatted(s.c_str());
		ImGui::PopStyleColor();
	}

	std::string LowerB(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}
}

struct App::BsaView
{
	std::filesystem::path       path;
	std::unique_ptr<BsaArchive> bsa;
	std::string                 error;
	char                        filter[256] = "";
	char                        dest[1024] = "";
	bool                        overwrite = false;
	// unpacking on its own thread
	std::thread              thread;
	std::atomic<std::size_t> done{ 0 }, total{ 0 };
	std::atomic<bool>        running{ false }, cancel{ false };
	BsaExtractResult         result;
	bool                     haveResult = false;
	std::filesystem::path    resultFolder;
	~BsaView()
	{
		cancel = true;
		if (thread.joinable()) {
			thread.join();
		}
	}
};

void App::openBsa(const std::filesystem::path& a_path)
{
	if (_bsaView && _bsaView->running) {
		return;
	}
	_bsaView = std::make_shared<BsaView>();
	_bsaView->path = a_path;
	_showBsa = true;
	try {
		_bsaView->bsa = std::make_unique<BsaArchive>(a_path);
	} catch (const std::exception& e) {
		_bsaView->error = e.what();
	}
	const auto dest = a_path.parent_path() / (a_path.stem().wstring() + L" (unpacked)");
	std::snprintf(_bsaView->dest, sizeof(_bsaView->dest), "%s", platform::ToUtf8(dest).c_str());
}

void App::drawBsa()
{
	prepareWindow("Unpack a BSA archive", ImVec2(0.75f, 0.8f));
	if (!ImGui::Begin("Unpack a BSA archive", &_showBsa, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Unpack a BSA archive");
	const bool running = _bsaView && _bsaView->running;
	ImGui::BeginDisabled(running);
	if (ImGui::Button("Open a BSA or BA2...")) {
		const auto start = _bsaView ? _bsaView->path.parent_path() : (_sourcesPath.empty() ? gameFolder() / "Data" : platform::FromUtf8(_sourcesPath));
		if (auto p = platform::OpenFileDialog(start, "bsa;ba2", "BSA and BA2 archives")) {
			openBsa(*p);
		}
	}
	ImGui::EndDisabled();
	tips::Tip("Pick a .bsa (Skyrim) or .ba2 (Fallout 4) file: a mod's archive, or one of the game's in Data.");
	if (!_bsaView) {
		ImGui::SameLine();
		ImGui::TextDisabled("No archive open.");
		ImGui::End();
		return;
	}
	auto& v = *_bsaView;
	ImGui::SameLine();
	ImGui::TextUnformatted(platform::ToUtf8(v.path.filename()).c_str());
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("%s", platform::ToUtf8(v.path).c_str());
	}
	if (!v.error.empty() || !v.bsa) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
		ImGui::TextWrapped("Could not read it: %s", v.error.c_str());
		ImGui::PopStyleColor();
		ImGui::End();
		return;
	}
	const auto& entries = v.bsa->entries();
	ImGui::SameLine();
	ImGui::TextDisabled("(%zu files, %s)", entries.size(), v.bsa->version() == 105 ? "Special Edition" : "Legendary Edition");

	// which files the filter shows
	const auto                     f = [&] {
        auto s = LowerB(v.filter);
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
	}();
	std::vector<const BsaArchive::Entry*> shown;
	std::uint64_t                         shownBytes = 0;
	for (const auto& e : entries) {
		if (f.empty() || e.path.find(f) != std::string::npos) {
			shown.push_back(&e);
			shownBytes += e.size;
		}
	}

	ImGui::SetNextItemWidth(460);
	ImGui::InputTextWithHint("##bsafilter", "show only paths containing... (e.g. scripts\\ or a name)", v.filter, sizeof(v.filter));
	ImGui::SameLine();
	ImGui::TextDisabled("%zu shown", shown.size());

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Unpack into:");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(std::max(200.0f, ImGui::GetContentRegionAvail().x - 120));
	ImGui::InputText("##bsadest", v.dest, sizeof(v.dest));
	ImGui::SameLine();
	if (ImGui::Button("Choose...")) {
		if (auto p = platform::PickFolderDialog("Folder to unpack the archive into")) {
			std::snprintf(v.dest, sizeof(v.dest), "%s", platform::ToUtf8(*p).c_str());
		}
	}
	ImGui::PushStyleColor(ImGuiCol_Text, theme::Caution());
	ImGui::TextWrapped("Not into the game's Data folder or a mod's folder: loose files there replace the ones in the archive in the "
	                   "game. Unpack somewhere of your own to look at or work on the files.");
	ImGui::PopStyleColor();
	ImGui::Checkbox("Replace files already in that folder", &v.overwrite);

	auto start = [&](bool a_all) {
		if (v.thread.joinable()) {
			v.thread.join();
		}
		v.cancel = false;
		v.running = true;
		v.haveResult = false;
		v.done = 0;
		v.total = a_all ? entries.size() : shown.size();
		v.resultFolder = platform::FromUtf8(v.dest);
		auto* vp = &v;
		const std::string filter = a_all ? std::string() : f;
		v.thread = std::thread([vp, filter] {
			vp->result = ExtractBsa(*vp->bsa, vp->resultFolder, filter, vp->overwrite, [vp](std::size_t d, std::size_t) {
				vp->done = d;
				return !vp->cancel;
			});
			vp->haveResult = true;
			vp->running = false;
		});
	};
	ImGui::BeginDisabled(running || v.dest[0] == 0);
	if (ImGui::Button(Fmt("Unpack all %zu files", entries.size()).c_str())) {
		start(true);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(f.empty() || shown.empty());
	if (ImGui::Button(Fmt("Unpack the %zu shown (%.1f MB)", shown.size(), shownBytes / 1048576.0).c_str())) {
		start(false);
	}
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	if (running) {
		ImGui::SameLine();
		ImGui::ProgressBar(v.total ? static_cast<float>(v.done) / static_cast<float>(v.total) : 0.0f, ImVec2(220, 0),
			Fmt("%zu of %zu", v.done.load(), v.total.load()).c_str());
		ImGui::SameLine();
		if (ImGui::Button("Stop")) {
			v.cancel = true;
		}
	} else if (v.haveResult) {
		if (v.thread.joinable()) {
			v.thread.join();
		}
		ImGui::SameLine();
		TextColored(v.result.failed ? theme::Warn() : theme::Good(),
			Fmt("Unpacked %zu file(s), %.1f MB%s%s", v.result.written, v.result.bytes / 1048576.0,
				v.result.skipped ? Fmt(", %zu already there kept", v.result.skipped).c_str() : "",
				v.result.failed ? Fmt(", %zu failed", v.result.failed).c_str() : ""));
		ImGui::SameLine();
		if (ImGui::SmallButton("Open the folder")) {
			platform::OpenWithDefaultApp(v.resultFolder);
		}
		for (const auto& e : v.result.errors) {
			ImGui::TextDisabled("  %s", e.c_str());
		}
	}
	ImGui::Separator();

	// the files
	if (ImGui::BeginTable("##bsafiles", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("File (double-click a script to read it, any other file to open it)", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90);
		ImGui::TableSetupColumn("Packed", ImGuiTableColumnFlags_WidthFixed, 70);
		ImGui::TableHeadersRow();
		ImGuiListClipper clip;
		clip.Begin(static_cast<int>(shown.size()));
		const BsaArchive::Entry* open = nullptr;
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
				const auto& e = *shown[static_cast<std::size_t>(i)];
				ImGui::PushID(i);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Selectable(e.path.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick);
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
					open = &e;
				}
				ImGui::TableNextColumn();
				ImGui::Text(e.size >= 1048576 ? "%.1f MB" : "%.0f KB", e.size >= 1048576 ? e.size / 1048576.0 : e.size / 1024.0 + 0.5);
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", e.compressed ? "yes" : "no");
				ImGui::PopID();
			}
		}
		ImGui::EndTable();
		if (open) {
			// to a temporary folder, then the script window (a .pex) or the file's own program
			try {
				const auto data = v.bsa->extract(*open);
				auto       name = open->path.substr(open->path.find_last_of('\\') + 1);
				const auto dir = std::filesystem::temp_directory_path() / "ReSaverPP" / "bsa" / v.path.stem();
				std::error_code ec;
				std::filesystem::create_directories(dir, ec);
				const auto file = dir / std::filesystem::u8path(name);
				std::ofstream out(file, std::ios::binary | std::ios::trunc);
				out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
				out.close();
				if (LowerB(name).size() > 4 && LowerB(name).ends_with(".pex")) {
					openScript(file);
				} else {
					platform::OpenWithDefaultApp(file);
				}
			} catch (const std::exception& e) {
				_error = "Could not read " + open->path + " from the archive:\n\n" + e.what();
			}
		}
	}
	ImGui::End();
}
