// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// AI and logs > Collect all logs: every Skyrim and Fallout 4 log and what is needed to read them, in one dated zip
// on the desktop, ready to send with a report.
//   - Documents\My Games\<game>\SKSE (Fallout 4: F4SE) with all its folders (the script extender, every plugin's
//     log, crash logs: Crash Logger, Buffout 4), Logs (Papyrus), and the game's INI files (Skyrim.ini,
//     SkyrimPrefs.ini, SkyrimCustom.ini; Fallout4.ini, Fallout4Prefs.ini, Fallout4Custom.ini) - not the saves
//   - the load order the game reads: %LOCALAPPDATA%\<game>\plugins.txt and loadorder.txt
//   - .NET Script Framework crash logs (<game>\Data\NetScriptFramework\Crash)
//   - Mod Organizer: its own logs and the profile's mod list, plugins and load order
//   - ReSaver Plus's own logs (not resaverpp.ini: it holds the AI key)
//   - system-info.txt: computer, Windows / Wine, processor, memory, video cards, screen, game version
// A file over 200 MB is left out (and listed as left out); the zip is written by a background thread.
#include "App.h"

#include "Platform.h"
#include "Theme.h"
#include "Tips.h"
#include "core/Zip.h"

#include "imgui.h"

#include <atomic>
#include <cctype>
#include <ctime>
#include <thread>

namespace
{
	std::string LowerC(std::string s)
	{
		for (auto& c : s) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return s;
	}
}

struct App::CollectJob
{
	struct Item
	{
		std::filesystem::path file;
		std::string           name;  // in the zip
	};
	std::vector<Item>        items;
	std::vector<std::string> notes;  // what was left out, for the readme
	std::string              sysInfo, header;
	std::filesystem::path    zip;
	std::thread              thread;
	std::atomic<std::size_t> done{ 0 };
	std::atomic<bool>        finished{ false };
	std::string              error;
	std::uint64_t            bytes = 0;
	std::size_t              files = 0;
	~CollectJob()
	{
		if (thread.joinable()) {
			thread.join();
		}
	}
};

void App::startCollectLogs()
{
	_showCollect = true;
	if (_collectJob && !_collectJob->finished) {
		return;
	}
	auto job = std::make_shared<CollectJob>();
	std::error_code ec;
	auto            add = [&](const std::filesystem::path& a_file, const std::string& a_name) {
        const auto size = std::filesystem::file_size(a_file, ec);
        if (ec) {
            return;
        }
        if (size > 200ull * 1024 * 1024) {
            job->notes.push_back("left out (over 200 MB): " + platform::ToUtf8(a_file));
            return;
        }
        for (const auto& it : job->items) {
            if (it.file == a_file) {
                return;
            }
        }
        job->items.push_back({ a_file, a_name });
	};
	auto addTree = [&](const std::filesystem::path& a_dir, const std::string& a_prefix) {
		if (!std::filesystem::is_directory(a_dir, ec)) {
			return;
		}
		for (std::filesystem::recursive_directory_iterator it(a_dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
			 !ec && it != end; it.increment(ec)) {
			if (it->is_regular_file(ec)) {
				auto rel = platform::ToUtf8(std::filesystem::relative(it->path(), a_dir, ec));
				std::replace(rel.begin(), rel.end(), '\\', '/');
				add(it->path(), a_prefix + "/" + rel);
			}
		}
	};

	// Documents\My Games\<game>
	const char* games[] = { "Skyrim Special Edition", "Skyrim Special Edition GOG", "Skyrim Special Edition EPIC", "Skyrim VR", "Skyrim", "Fallout4" };
	for (const auto& mg : myGamesFolders()) {
		for (const char* g : games) {
			const auto dir = mg / g;
			if (!std::filesystem::is_directory(dir, ec)) {
				continue;
			}
			const char* se = std::string(g) == "Fallout4" ? "F4SE" : "SKSE";
			addTree(dir / se, std::string(g) + "/" + se);
			addTree(dir / "Logs", std::string(g) + "/Logs");
			for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
				if (it->is_regular_file(ec) && LowerC(platform::ToUtf8(it->path().extension())) == ".ini") {
					add(it->path(), std::string(g) + "/" + platform::ToUtf8(it->path().filename()));
				}
			}
		}
	}
	// the load order the game reads
	if (const auto la = platform::LocalAppDataFolder(); !la.empty()) {
		for (const char* g : games) {
			for (const char* f : { "plugins.txt", "loadorder.txt", "DLCList.txt" }) {
				const auto p = la / g / f;
				if (std::filesystem::is_regular_file(p, ec)) {
					add(p, std::string("Load order (AppData)/") + g + "/" + f);
				}
			}
		}
	}
	// .NET Script Framework crash logs
	if (const auto gf = gameFolder(); !gf.empty()) {
		addTree(gf / "Data" / "NetScriptFramework" / "Crash", "NetScriptFramework crash logs");
	}
	// Mod Organizer
	if (_mo2) {
		addTree(_mo2->base / "logs", "Mod Organizer/logs");
		if (!_profile.empty()) {
			const auto prof = _mo2->profilesDir / platform::FromUtf8(_profile);
			for (const char* f : { "modlist.txt", "plugins.txt", "loadorder.txt", "settings.ini" }) {
				if (std::filesystem::is_regular_file(prof / f, ec)) {
					add(prof / f, "Mod Organizer/profile " + _profile + "/" + f);
				}
			}
		}
	}
	// ReSaver Plus's own logs (not resaverpp.ini: it holds the AI key)
	for (const char* f : { "ReSaverPP-startup.log", "ReSaverPP-startup.previous.log", "ReSaverPP AI.log", "PapyrusCompileLog.txt" }) {
		const auto p = platform::ExeFolder() / f;
		if (std::filesystem::is_regular_file(p, ec)) {
			add(p, std::string("ReSaver Plus/") + f);
		}
	}

	// system-info.txt
	const auto gf = gameFolder();
	auto       exe = gf / "SkyrimSE.exe";
	if (!std::filesystem::exists(exe, ec)) {
		for (const char* e : { "SkyrimVR.exe", "TESV.exe", "Fallout4.exe" }) {
			if (std::filesystem::exists(gf / e, ec)) {
				exe = gf / e;
			}
		}
	}
	job->sysInfo = platform::SystemReport(gf.empty() ? std::filesystem::path() : exe);
	job->sysInfo += std::string("ReSaver Plus: version ") + RESAVERPP_VERSION + "\r\n";
	if (_mo2) {
		job->sysInfo += "Mod manager: Mod Organizer 2 (" + platform::ToUtf8(_mo2->base) + "), profile " + _profile + "\r\n";
	} else if (_vortex) {
		job->sysInfo += "Mod manager: Vortex\r\n";
	}
	if (_save) {
		job->sysInfo += "Save open in ReSaver Plus: " + platform::ToUtf8(_path.filename()) + "\r\n";
	}

	const auto  now = std::time(nullptr);
	std::tm     tm{};
#ifdef _WIN32
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif
	char stamp[64], when[64];
	std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M", &tm);
	std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
	auto desk = platform::DesktopFolder();
	if (desk.empty() || !std::filesystem::is_directory(desk, ec)) {
		desk = platform::ExeFolder();
	}
	const std::string what = _game == 2 ? "Fallout 4 logs" : "Skyrim logs";
	job->zip = desk / (what + " " + stamp + ".zip");
	job->header = what + " collected by ReSaver Plus on " + when + "\r\n\r\n";
	auto* jp = job.get();
	job->thread = std::thread([jp] {
		try {
			ess::zip::Writer z(jp->zip);
			std::string      readme = jp->header + jp->sysInfo + "\r\nFiles:\r\n";
			for (const auto& it : jp->items) {
				if (z.addFile(it.name, it.file)) {
					readme += "  " + it.name + "\r\n";
				} else {
					readme += "  could not be read: " + platform::ToUtf8(it.file) + "\r\n";
				}
				++jp->done;
			}
			for (const auto& n : jp->notes) {
				readme += n + "\r\n";
			}
			z.add("system-info.txt", jp->header + jp->sysInfo);
			z.add("README.txt", readme);
			z.close();
			jp->files = z.count();
			jp->bytes = z.bytesWritten();
		} catch (const std::exception& e) {
			jp->error = e.what();
		}
		jp->finished = true;
	});
	_collectJob = std::move(job);
}

void App::drawCollectLogs()
{
	if (!_collectJob) {
		_showCollect = false;
		return;
	}
	ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
	const auto* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.4f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if (!ImGui::Begin("Collect all logs", &_showCollect, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::End();
		return;
	}
	auto& j = *_collectJob;
	if (!j.finished) {
		ImGui::TextUnformatted("Putting your game logs, load order, INI files and PC details into one zip...");
		ImGui::ProgressBar(j.items.empty() ? 0.0f : static_cast<float>(j.done) / static_cast<float>(j.items.size()), ImVec2(580, 0),
			(std::to_string(j.done.load()) + " of " + std::to_string(j.items.size()) + " files").c_str());
	} else if (!j.error.empty()) {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Bad());
		ImGui::PushTextWrapPos(580);
		ImGui::TextUnformatted(("The zip could not be made: " + j.error).c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	} else {
		ImGui::PushStyleColor(ImGuiCol_Text, theme::Good());
		ImGui::TextUnformatted("Done: your logs are in a zip on your desktop.");
		ImGui::PopStyleColor();
		ImGui::TextUnformatted(platform::ToUtf8(j.zip.filename()).c_str());
		char b[128];
		std::snprintf(b, sizeof(b), "%zu files, %.1f MB", j.files, j.bytes / 1048576.0);
		ImGui::TextDisabled("%s", b);
		ImGui::PushTextWrapPos(580);
		ImGui::TextDisabled("Inside: the SKSE / F4SE folder with all its logs and crash logs, the Papyrus logs, your INI files and load order, "
		                    "Mod Organizer's logs and profile lists, ReSaver Plus's logs, and system-info.txt (Windows, memory, "
		                    "processor, video card). No saves, and not your AI key.");
		ImGui::PopTextWrapPos();
		if (!j.notes.empty()) {
			for (const auto& n : j.notes) {
				ImGui::TextDisabled("%s", n.c_str());
			}
		}
		if (ImGui::Button("Show it in its folder")) {
			platform::ShowInFolder(j.zip);
		}
		ImGui::SameLine();
		if (ImGui::Button("Copy its path")) {
			ImGui::SetClipboardText(platform::ToUtf8(j.zip).c_str());
		}
		ImGui::SameLine();
		if (ImGui::Button("Close")) {
			_showCollect = false;
		}
	}
	ImGui::End();
}
