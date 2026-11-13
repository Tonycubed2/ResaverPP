#include "PCH.h"
#include "ProtonDrive.h"

#include <cwctype>

#include "Game.h"
#include "Settings.h"

namespace ProtonDrive
{
	namespace
	{
		std::mutex  g_lock;
		std::string g_status;
		std::atomic<int> g_fixes{ 0 };

		std::string Narrow(const std::wstring& a_w)
		{
			if (a_w.empty()) {
				return {};
			}
			const int n = WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), static_cast<int>(a_w.size()), nullptr, 0, nullptr, nullptr);
			std::string s(static_cast<std::size_t>(n), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), static_cast<int>(a_w.size()), s.data(), n, nullptr, nullptr);
			return s;
		}

		std::wstring Widen(const std::string& a_s)
		{
			if (a_s.empty()) {
				return {};
			}
			const int n = MultiByteToWideChar(CP_UTF8, 0, a_s.c_str(), static_cast<int>(a_s.size()), nullptr, 0);
			std::wstring w(static_cast<std::size_t>(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, a_s.c_str(), static_cast<int>(a_s.size()), w.data(), n);
			return w;
		}

		void SetStatus(std::string a_s)
		{
			std::lock_guard l(g_lock);
			g_status = std::move(a_s);
		}

		// single quotes for /bin/sh
		std::string Quote(const std::string& a_s)
		{
			std::string q = "'";
			for (const char c : a_s) {
				q += c == '\'' ? std::string("'\\''") : std::string(1, c);
			}
			return q + "'";
		}

		std::string g_prefix;  // $WINEPREFIX (Linux path), set by Start

		// runs a_script with /bin/sh (a Linux program; Wine starts it). The script goes into a file in the prefix's C: first:
		// Wine's command line to a Linux program does not keep \" escapes, a file needs none. Wine gives no handle to wait
		// on, so the caller looks for the script's result itself.
		bool RunShell(const std::string& a_script)
		{
			{
				std::ofstream f{ std::filesystem::path(L"C:\\ReSaverHelper-drive.sh"), std::ios::binary | std::ios::trunc };
				f << a_script << "\n";
				if (!f) {
					spdlog::warn("Proton drive: could not write C:\\ReSaverHelper-drive.sh");
					return false;
				}
			}
			std::wstring        cmd = L"/bin/sh \"" + Widen(g_prefix + "/drive_c/ReSaverHelper-drive.sh") + L"\"";
			STARTUPINFOW        si{ sizeof(si) };
			PROCESS_INFORMATION pi{};
			if (!CreateProcessW(L"Z:\\bin\\sh", cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
				spdlog::warn("Proton drive: could not start /bin/sh (error {})", GetLastError());
				return false;
			}
			if (pi.hThread) {
				CloseHandle(pi.hThread);
			}
			if (pi.hProcess) {
				CloseHandle(pi.hProcess);
			}
			return true;
		}

		// waits up to 3 s for a file the shell writes, returns its first line ("" when none)
		std::string ReadResult(const std::wstring& a_file)
		{
			for (int i = 0; i < 30; ++i) {
				std::ifstream f{ std::filesystem::path(a_file) };
				std::string   line;
				if (f && std::getline(f, line) && !line.empty()) {
					return line;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			return {};
		}

		void Watch(wchar_t a_letter, std::string a_link, std::string a_target, std::wstring a_exe)
		{
			const std::string letter(1, static_cast<char>(std::tolower(a_letter)));
			bool              told = false;
			for (;;) {
				std::this_thread::sleep_for(std::chrono::seconds(2));
				if (GetFileAttributesW(a_exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
					continue;
				}
				// the drive is gone (or points somewhere else): put it back where it was when the game started
				spdlog::warn("Proton drive: {}: no longer reaches the game ({}): another program was started into this Proton prefix. "
							 "Putting {} -> {} back.", static_cast<char>(std::toupper(a_letter)), Narrow(a_exe), a_link, a_target);
				RunShell("ln -sfn " + Quote(a_target) + " " + Quote(a_link));
				bool back = false;
				for (int i = 0; i < 30 && !back; ++i) {
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
					back = GetFileAttributesW(a_exe.c_str()) != INVALID_FILE_ATTRIBUTES;
				}
				const int n = ++g_fixes;
				if (back) {
					spdlog::info("Proton drive: {}: is back; the game can open its files again ({} time(s) this session)", letter, n);
					SetStatus(std::format("Proton drive {}: watched ({} -> {}); put back {} time(s) after other programs removed it", letter, a_link, a_target, n));
					if (!told) {
						told = true;
						Game::Notify("ini", std::format("ReSaverHelper: Proton removed the game's {}: drive when another program started; it is put back.",
												static_cast<char>(std::toupper(a_letter))));
					}
				} else {
					spdlog::error("Proton drive: could not put {} back; menus not opened yet may crash. Run: ln -sfn {} {}", letter, Quote(a_target), Quote(a_link));
					SetStatus(std::format("Proton drive {}: GONE and could not be put back (ln -sfn {} {})", letter, a_target, a_link));
					if (!told) {
						told = true;
						Game::Notify("ini", std::format("ReSaverHelper: the game's {}: drive was removed by Proton and could not be put back. Save and restart the game.",
												static_cast<char>(std::toupper(a_letter))));
					}
					std::this_thread::sleep_for(std::chrono::seconds(30));  // (do not try every 2 s)
				}
			}
		}
	}

	std::string Status()
	{
		std::lock_guard l(g_lock);
		return g_status;
	}

	void Start()
	{
		const auto ntdll = GetModuleHandleW(L"ntdll.dll");
		const auto wineVersion = ntdll ? reinterpret_cast<const char* (*)()>(GetProcAddress(ntdll, "wine_get_version")) : nullptr;
		if (!wineVersion) {
			return;  // Windows: nothing to do
		}
		if (!Settings::Get().fixProtonGameDrive) {
			spdlog::info("Proton drive: off (bFixProtonGameDrive=0)");
			return;
		}
		wchar_t      buf[MAX_PATH * 2] = {};
		const DWORD  len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
		std::wstring exe(buf, len);
		if (exe.size() < 3 || exe[1] != L':') {
			spdlog::info("Proton drive: Wine {}, game at {}: not on a drive letter, nothing to watch", wineVersion(), Narrow(exe));
			return;
		}
		const wchar_t letter = static_cast<wchar_t>(std::towlower(exe[0]));
		if (letter == L'c' || letter == L'z') {
			spdlog::info("Proton drive: Wine {}, game on {}: (not Proton's game drive), nothing to watch", wineVersion(), static_cast<char>(letter));
			return;
		}
		std::string pfx;
		if (wchar_t env[2048] = {}; GetEnvironmentVariableW(L"WINEPREFIX", env, static_cast<DWORD>(std::size(env))) > 0) {
			pfx = Narrow(env);  // (Proton sets it to the prefix's Linux path)
		}
		if (pfx.empty()) {
			spdlog::info("Proton drive: Wine {}, game on {}:, but WINEPREFIX is not set: nothing to watch", wineVersion(), static_cast<char>(letter));
			return;
		}
		while (pfx.size() > 1 && pfx.back() == '/') {
			pfx.pop_back();
		}
		g_prefix = pfx;
		const std::string link = pfx + "/dosdevices/" + std::string(1, static_cast<char>(letter)) + ":";
		const std::string out = pfx + "/drive_c/ReSaverHelper-drive.txt";
		const std::wstring outDos = L"C:\\ReSaverHelper-drive.txt";  // (= out)
		DeleteFileW(outDos.c_str());
		std::thread([letter, link, out, outDos, exe, ver = std::string(wineVersion())]() {
			if (!RunShell("readlink " + Quote(link) + " > " + Quote(out))) {
				return;
			}
			const auto target = ReadResult(outDos);
			DeleteFileW(outDos.c_str());
			if (target.empty() || target[0] != '/') {
				spdlog::warn("Proton drive: Wine {}, game on {}:, but {} could not be read: nothing to watch", ver, static_cast<char>(letter), link);
				return;
			}
			spdlog::info("Proton drive: Wine {}: the game runs from {}: = {} -> {}; it is watched and put back if another program's "
						 "Proton launch removes it", ver, static_cast<char>(letter), link, target);
			SetStatus(std::format("Proton drive {}: watched ({} -> {})", static_cast<char>(letter), link, target));
			Watch(letter, link, target, exe);
		}).detach();
	}
}
