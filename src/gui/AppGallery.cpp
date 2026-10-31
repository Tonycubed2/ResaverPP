// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// File > Open: the saves folder as a gallery (each save's picture, character, level, place and date, newest
// first) or as a plain list that sorts by any column. Asked for by Richard (SkyrimIstari) so the last save
// made is easy to find. Headers and pictures are read on a background thread, newest first; "Browse..."
// still opens the Windows file dialog for a save anywhere else.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"
#include "core/Save.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>

using namespace ess;

namespace
{
	std::string Fmtg(const char* f, auto... args)
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), f, args...);
		return buf;
	}

	std::string LowerG(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}

	std::int64_t UnixTime(std::filesystem::file_time_type a_t)
	{
		const auto now = std::filesystem::file_time_type::clock::now();
		return static_cast<std::int64_t>(std::time(nullptr)) +
		       std::chrono::duration_cast<std::chrono::seconds>(a_t - now).count();
	}

	std::string WhenG(std::int64_t a_unix)
	{
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

	std::string AgoG(std::int64_t a_unix)
	{
		const auto d = static_cast<std::int64_t>(std::time(nullptr)) - a_unix;
		if (d < 90) {
			return "just now";
		}
		if (d < 5400) {
			return Fmtg("%lld minutes ago", static_cast<long long>(d / 60));
		}
		if (d < 129600) {
			return Fmtg("%lld hours ago", static_cast<long long>(d / 3600));
		}
		return Fmtg("%lld days ago", static_cast<long long>(d / 86400));
	}
}

struct App::Gallery
{
	struct Entry
	{
		std::filesystem::path     path;
		std::int64_t              time = 0;  // last written (unix)
		std::uintmax_t            size = 0;
		bool                      read = false, ok = false;  // header read (on the thread) / it is a save
		std::string               name, location, gameDate, race;
		std::uint32_t             level = 0, number = 0;
		bool                      le = false;
		std::vector<std::uint8_t> rgba;  // the picture until it is uploaded
		int                       w = 0, h = 0;
		std::uint64_t             tex = 0;
	};
	std::filesystem::path folder;
	std::vector<Entry>    entries;  // newest first; the thread fills them in that order
	std::mutex            m;
	std::atomic<bool>     cancel{ false }, done{ false };
	std::atomic<std::size_t> readCount{ 0 };
	std::thread           thread;
	~Gallery()
	{
		cancel = true;
		if (thread.joinable()) {
			thread.join();
		}
	}
};

void App::freeGallery()
{
	if (!_gallery) {
		return;
	}
	_gallery->cancel = true;
	if (_gallery->thread.joinable()) {
		_gallery->thread.join();
	}
	for (auto& e : _gallery->entries) {
		if (e.tex && freeTexture) {
			freeTexture(e.tex);
		}
	}
	_gallery.reset();
}

void App::loadGallery(const std::filesystem::path& a_folder)
{
	freeGallery();
	auto g = std::make_unique<Gallery>();
	g->folder = a_folder;
	std::error_code ec;
	for (std::filesystem::directory_iterator it(a_folder, ec), end; !ec && it != end; it.increment(ec)) {
		if (!it->is_regular_file(ec) || LowerG(platform::ToUtf8(it->path().extension())) != ".ess") {
			continue;
		}
		Gallery::Entry e;
		e.path = it->path();
		e.time = UnixTime(it->last_write_time(ec));
		e.size = it->file_size(ec);
		g->entries.push_back(std::move(e));
	}
	std::sort(g->entries.begin(), g->entries.end(), [](const Gallery::Entry& a, const Gallery::Entry& b) { return a.time > b.time; });
	auto* gp = g.get();
	g->thread = std::thread([gp] {
		for (std::size_t i = 0; i < gp->entries.size() && !gp->cancel; ++i) {
			std::filesystem::path p;
			{
				std::lock_guard l(gp->m);
				p = gp->entries[i].path;
			}
			Header h;
			const bool       ok = ReadSaveHeader(p, h);
			std::vector<std::uint8_t> rgba;
			if (ok && h.shotWidth && h.shotHeight) {
				const std::size_t n = std::size_t(h.shotWidth) * h.shotHeight, bpp = h.isLE() ? 3 : 4;
				if (h.screenshot.size() >= n * bpp) {
					rgba.assign(n * 4, 255);
					for (std::size_t k = 0; k < n; ++k) {
						for (std::size_t c = 0; c < 3; ++c) {
							rgba[k * 4 + c] = h.screenshot[k * bpp + c];
						}
					}
				}
			}
			std::lock_guard l(gp->m);
			auto&           e = gp->entries[i];
			e.ok = ok;
			if (ok) {
				e.name = h.playerName;
				e.level = h.playerLevel;
				e.location = h.location;
				e.gameDate = h.gameDate;
				e.race = h.race;
				e.number = h.saveNumber;
				e.le = h.isLE();
				e.rgba = std::move(rgba);
				e.w = static_cast<int>(h.shotWidth);
				e.h = static_cast<int>(h.shotHeight);
			}
			e.read = true;
			++gp->readCount;
		}
		gp->done = true;
	});
	_gallery = std::move(g);
	_galleryCharacter.clear();
}

void App::startOpenDialog()
{
	if (!_settingsLoaded) {
		const bool list = _galleryList;
		loadSettings();
		_galleryList = _galleryList || list;  // (test hook asked for the list)
	}
	_showGallery = true;
	const auto folder = _path.empty() ? savesFolder() : _path.parent_path();
	if (!_gallery || _gallery->folder != folder) {
		loadGallery(folder);
	}
}

void App::openWithFileDialog()
{
	auto start = _path.empty() ? savesFolder() : _path.parent_path();
	if (platform::HasNativeDialogs()) {
		if (auto p = platform::OpenSaveDialog(start)) {
			openFromGallery(*p);
		}
	} else {
		std::snprintf(_pathBuf, sizeof(_pathBuf), "%s", platform::ToUtf8(start).c_str());
		_openPathPopup = true;
		_showGallery = false;
	}
}

void App::openFromGallery(const std::filesystem::path& a_path)
{
	auto go = [this, a_path] {
		_showGallery = false;
		open(a_path);
	};
	if (_dirty) {
		confirm("Unsaved changes", "The edits to " + platform::ToUtf8(_path.filename()) + " have not been saved. Open " +
		                               platform::ToUtf8(a_path.filename()) + " anyway (the edits are lost)?",
			go);
	} else {
		go();
	}
}

void App::drawGallery()
{
	if (!_gallery) {
		startOpenDialog();
	}
	prepareWindow("Open a save", ImVec2(0.85f, 0.85f));
	if (!ImGui::Begin("Open a save", &_showGallery, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	maximizeControl("Open a save");
	auto& g = *_gallery;

	// the pictures that have arrived become textures (the UI thread owns OpenGL)
	{
		std::lock_guard l(g.m);
		for (auto& e : g.entries) {
			if (!e.tex && !e.rgba.empty() && uploadTexture) {
				e.tex = uploadTexture(e.rgba.data(), e.w, e.h);
				e.rgba.clear();
				e.rgba.shrink_to_fit();
			}
		}
	}

	// top: folder, view, character filter, search
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("Folder:");
	ImGui::SameLine();
	ImGui::TextDisabled("%s", platform::ToUtf8(g.folder).c_str());
	ImGui::SameLine();
	if (ImGui::Button("Other folder...")) {
		if (auto f = platform::PickFolderDialog("Folder with Skyrim saves")) {
			loadGallery(*f);
			ImGui::End();
			return;
		}
	}
	tips::Tip("Show the saves of another folder (e.g. a Mod Organizer profile's own saves).");
	ImGui::SameLine();
	if (ImGui::Button("Browse for a file...")) {
		openWithFileDialog();
	}
	tips::Tip("Pick a save anywhere with the Windows file dialog.");
	ImGui::SameLine();
	if (ImGui::Button("Refresh")) {
		loadGallery(g.folder);
		ImGui::End();
		return;
	}

	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted("View:");
	ImGui::SameLine();
	if (ImGui::RadioButton("Pictures", !_galleryList)) {
		_galleryList = false;
		saveSettings();
	}
	ImGui::SameLine();
	if (ImGui::RadioButton("List", _galleryList)) {
		_galleryList = true;
		saveSettings();
	}
	tips::Tip("Pictures: each save's screenshot, newest first. List: one line per save, sorted by any column (click its header).");
	// characters seen so far
	std::vector<std::string> chars;
	std::size_t              total = 0, read = 0;
	{
		std::lock_guard l(g.m);
		total = g.entries.size();
		for (const auto& e : g.entries) {
			read += e.read ? 1 : 0;
			if (e.ok && std::find(chars.begin(), chars.end(), e.name) == chars.end()) {
				chars.push_back(e.name);
			}
		}
	}
	std::sort(chars.begin(), chars.end());
	ImGui::SameLine(0, 24);
	ImGui::SetNextItemWidth(220);
	if (ImGui::BeginCombo("Character", _galleryCharacter.empty() ? "All characters" : _galleryCharacter.c_str())) {
		if (ImGui::Selectable("All characters", _galleryCharacter.empty())) {
			_galleryCharacter.clear();
		}
		for (const auto& c : chars) {
			if (ImGui::Selectable(c.c_str(), c == _galleryCharacter)) {
				_galleryCharacter = c;
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine(0, 24);
	ImGui::SetNextItemWidth(220);
	ImGui::InputTextWithHint("##gfind", "find (name, place, file)", _galleryFind, sizeof(_galleryFind));
	if (read < total) {
		ImGui::SameLine(0, 24);
		ImGui::TextDisabled("reading %zu of %zu saves...", read, total);
	} else {
		ImGui::SameLine(0, 24);
		ImGui::TextDisabled("%zu saves", total);
	}
	ImGui::Separator();

	const auto find = LowerG(_galleryFind);
	auto       shown = [&](const Gallery::Entry& e) {
        if (!_galleryCharacter.empty() && (!e.read || e.name != _galleryCharacter)) {
            return false;
        }
        if (!find.empty()) {
            const auto hay = LowerG(e.name + " " + e.location + " " + platform::ToUtf8(e.path.filename()));
            if (hay.find(find) == std::string::npos) {
                return false;
            }
        }
        return true;
	};
	std::filesystem::path clicked;

	if (total == 0) {
		ImGui::TextDisabled("No saves (.ess) in this folder. Use Other folder... or Browse for a file...");
	} else if (!_galleryList) {
		// pictures, newest first
		ImGui::BeginChild("##gallery", ImVec2(0, 0));
		const float cardW = 250, picH = cardW * 0.6f, spacing = ImGui::GetStyle().ItemSpacing.x;
		const int   perRow = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (cardW + spacing)));
		int         col = 0;
		std::lock_guard l(g.m);
		for (std::size_t i = 0; i < g.entries.size(); ++i) {
			const auto& e = g.entries[i];
			if (!shown(e)) {
				continue;
			}
			if (col > 0) {
				ImGui::SameLine();
			}
			ImGui::PushID(static_cast<int>(i));
			ImGui::BeginGroup();
			const bool current = _save && e.path == _path;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			bool         click = false;
			if (e.tex) {
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
				click = ImGui::ImageButton("##pic", static_cast<ImTextureID>(e.tex), ImVec2(cardW, picH));
				ImGui::PopStyleVar();
			} else {
				click = ImGui::Button(e.read ? (e.ok ? "(no picture)" : "(not a save)") : "...", ImVec2(cardW, picH));
			}
			if (current) {
				ImGui::GetWindowDrawList()->AddRect(at, ImVec2(at.x + cardW, at.y + picH), theme::Good(), 0, 0, 3.0f);
			}
			if (ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s\n%s, %s", platform::ToUtf8(e.path.filename()).c_str(), WhenG(e.time).c_str(),
					Fmtg("%.1f MB", e.size / 1048576.0).c_str());
			}
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardW);
			if (e.ok) {
				ImGui::Text("%s, level %u", e.name.c_str(), e.level);
				ImGui::TextDisabled("%s", e.location.c_str());
			} else {
				ImGui::TextUnformatted(platform::ToUtf8(e.path.filename()).c_str());
				ImGui::TextDisabled(" ");
			}
			ImGui::TextDisabled("%s  (%s)", WhenG(e.time).c_str(), AgoG(e.time).c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndGroup();
			ImGui::PopID();
			if (click && e.read && e.ok) {
				clicked = e.path;
			}
			if (++col >= perRow) {
				col = 0;
				ImGui::Spacing();
			}
		}
		ImGui::EndChild();
	} else {
		// plain list, sortable by any column
		const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
		                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
		if (ImGui::BeginTable("##glist", 7, flags)) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 1.3f);
			ImGui::TableSetupColumn("Character", 0, 1.2f);
			ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_PreferSortDescending, 0.5f);
			ImGui::TableSetupColumn("Place", 0, 1.6f);
			ImGui::TableSetupColumn("Save #", ImGuiTableColumnFlags_PreferSortDescending, 0.5f);
			ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_PreferSortDescending, 0.6f);
			ImGui::TableSetupColumn("File", 0, 3.0f);
			ImGui::TableHeadersRow();
			std::lock_guard l(g.m);
			std::vector<std::size_t> rows;
			for (std::size_t i = 0; i < g.entries.size(); ++i) {
				if (shown(g.entries[i])) {
					rows.push_back(i);
				}
			}
			if (const ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs(); ss && ss->SpecsCount > 0) {
				const auto& sp = ss->Specs[0];
				const bool  desc = sp.SortDirection == ImGuiSortDirection_Descending;
				std::stable_sort(rows.begin(), rows.end(), [&](std::size_t a, std::size_t b) {
					const auto& x = g.entries[a];
					const auto& y = g.entries[b];
					int         c = 0;
					switch (sp.ColumnIndex) {
					case 0: c = x.time < y.time ? -1 : x.time > y.time; break;
					case 1: c = LowerG(x.name).compare(LowerG(y.name)); break;
					case 2: c = x.level < y.level ? -1 : x.level > y.level; break;
					case 3: c = LowerG(x.location).compare(LowerG(y.location)); break;
					case 4: c = x.number < y.number ? -1 : x.number > y.number; break;
					case 5: c = x.size < y.size ? -1 : x.size > y.size; break;
					default: c = LowerG(platform::ToUtf8(x.path.filename())).compare(LowerG(platform::ToUtf8(y.path.filename())));
					}
					return desc ? c > 0 : c < 0;
				});
			}
			ImGuiListClipper clip;
			clip.Begin(static_cast<int>(rows.size()));
			while (clip.Step()) {
				for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
					const auto& e = g.entries[rows[static_cast<std::size_t>(r)]];
					ImGui::PushID(r);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					const bool current = _save && e.path == _path;
					if (ImGui::Selectable(WhenG(e.time).c_str(), current, ImGuiSelectableFlags_SpanAllColumns) && e.read && e.ok) {
						clicked = e.path;
					}
					if (ImGui::IsItemHovered() && e.tex) {
						ImGui::BeginTooltip();
						ImGui::Image(static_cast<ImTextureID>(e.tex), ImVec2(320, 192));
						ImGui::TextDisabled("%s", AgoG(e.time).c_str());
						ImGui::EndTooltip();
					}
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(e.ok ? e.name.c_str() : (e.read ? "(not a save)" : "..."));
					ImGui::TableNextColumn();
					if (e.ok) {
						ImGui::Text("%u", e.level);
					}
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(e.location.c_str());
					ImGui::TableNextColumn();
					if (e.ok) {
						ImGui::Text("%u", e.number);
					}
					ImGui::TableNextColumn();
					ImGui::Text("%.1f MB", e.size / 1048576.0);
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", platform::ToUtf8(e.path.filename()).c_str());
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
	}
	ImGui::End();
	if (!clicked.empty()) {
		openFromGallery(clicked);
	}
}
