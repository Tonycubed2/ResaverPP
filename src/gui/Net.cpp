// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Net.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

#ifdef _WIN32
#	include <windows.h>
#	include <wincrypt.h>
#	include <winhttp.h>
#	ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#		define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#	endif
#else
#	include <sys/wait.h>
#	include <unistd.h>
#endif

namespace net
{
	namespace
	{
		std::string CutMiddle(std::string a_text, std::size_t a_max)
		{
			if (a_text.size() <= a_max) {
				return a_text;
			}
			const std::size_t half = a_max / 2;
			return a_text.substr(0, half) + "\n\n... (" + std::to_string(a_text.size() - 2 * half) +
			       " characters of output left out here) ...\n\n" + a_text.substr(a_text.size() - half);
		}

		const char kHex[] = "0123456789abcdef";

		std::string ToHex(const unsigned char* a_p, std::size_t a_n)
		{
			std::string s;
			for (std::size_t i = 0; i < a_n; ++i) {
				s += kHex[a_p[i] >> 4];
				s += kHex[a_p[i] & 15];
			}
			return s;
		}

		std::string FromHex(const std::string& a_hex)
		{
			std::string s;
			for (std::size_t i = 0; i + 1 < a_hex.size(); i += 2) {
				s.push_back(static_cast<char>(std::strtoul(a_hex.substr(i, 2).c_str(), nullptr, 16)));
			}
			return s;
		}
	}

#ifdef _WIN32
	namespace
	{
		std::wstring Wide(const std::string& a_s)
		{
			if (a_s.empty()) {
				return {};
			}
			const int n = MultiByteToWideChar(CP_UTF8, 0, a_s.data(), static_cast<int>(a_s.size()), nullptr, 0);
			std::wstring w(static_cast<std::size_t>(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, a_s.data(), static_cast<int>(a_s.size()), w.data(), n);
			return w;
		}

		std::string LastError(const char* a_what)
		{
			const DWORD e = GetLastError();
			if (e == ERROR_WINHTTP_CANNOT_CONNECT) {
				return std::string(a_what) + ": could not connect (is the address right, and for a local engine: is it running?)";
			}
			if (e == ERROR_WINHTTP_TIMEOUT) {
				return std::string(a_what) + ": no answer in time";
			}
			if (e == ERROR_WINHTTP_NAME_NOT_RESOLVED) {
				return std::string(a_what) + ": the server name was not found (no internet, or a typo in the address)";
			}
			if (e == ERROR_WINHTTP_SECURE_FAILURE) {
				return std::string(a_what) + ": the secure connection failed (certificate problem)";
			}
			return std::string(a_what) + " failed (Windows error " + std::to_string(e) + ")";
		}

		// one argument quoted the way programs built with the Microsoft C runtime (and MinGW) read it back
		std::wstring QuoteArg(const std::wstring& a_arg)
		{
			if (!a_arg.empty() && a_arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
				return a_arg;
			}
			std::wstring q = L"\"";
			for (std::size_t i = 0;; ++i) {
				std::size_t slashes = 0;
				while (i < a_arg.size() && a_arg[i] == L'\\') {
					++i;
					++slashes;
				}
				if (i == a_arg.size()) {
					q.append(slashes * 2, L'\\');
					break;
				}
				if (a_arg[i] == L'"') {
					q.append(slashes * 2 + 1, L'\\');
					q.push_back(L'"');
				} else {
					q.append(slashes, L'\\');
					q.push_back(a_arg[i]);
				}
			}
			q.push_back(L'"');
			return q;
		}
	}

	Response Request(const std::string& a_method, const std::string& a_url, const Headers& a_headers, const std::string& a_body,
		const std::atomic<bool>& a_cancel, int a_timeoutSeconds)
	{
		Response r;
		const auto wurl = Wide(a_url);
		URL_COMPONENTS uc{};
		uc.dwStructSize = sizeof(uc);
		uc.dwHostNameLength = static_cast<DWORD>(-1);
		uc.dwUrlPathLength = static_cast<DWORD>(-1);
		uc.dwExtraInfoLength = static_cast<DWORD>(-1);
		uc.dwSchemeLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
			r.error = "not a web address: " + a_url;
			return r;
		}
		const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
		std::wstring       path(uc.lpszUrlPath, uc.dwUrlPathLength);
		if (uc.lpszExtraInfo && uc.dwExtraInfoLength) {
			path += std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength);
		}
		const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

		HINTERNET session = WinHttpOpen(L"ReSaverPP/" RESAVERPP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session) {
			session = WinHttpOpen(L"ReSaverPP", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		}
		if (!session) {
			r.error = LastError("WinHttpOpen");
			return r;
		}
		const int ms = a_timeoutSeconds * 1000;
		WinHttpSetTimeouts(session, 0, 30000, 120000, ms);
		HINTERNET conn = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
		if (!conn) {
			r.error = LastError("connecting");
			WinHttpCloseHandle(session);
			return r;
		}
		std::atomic<HINTERNET> req = WinHttpOpenRequest(conn, Wide(a_method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
		if (!req) {
			r.error = LastError("opening the request");
			WinHttpCloseHandle(conn);
			WinHttpCloseHandle(session);
			return r;
		}
		// Stop: closing the handle from another thread ends the blocked call
		std::atomic<bool> finished{ false }, cancelled{ false };
		std::thread       watcher([&] {
            while (!finished) {
                if (a_cancel) {
                    cancelled = true;
                    if (HINTERNET h = req.exchange(nullptr)) {
                        WinHttpCloseHandle(h);
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
		});
		std::wstring hdr;
		for (const auto& [k, v] : a_headers) {
			hdr += Wide(k) + L": " + Wide(v) + L"\r\n";
		}
		bool ok = WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), static_cast<DWORD>(-1L),
					  a_body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(a_body.data()), static_cast<DWORD>(a_body.size()),
					  static_cast<DWORD>(a_body.size()), 0) &&
		          WinHttpReceiveResponse(req, nullptr);
		if (!ok) {
			r.error = cancelled ? "stopped" : LastError("sending the request");
		} else {
			DWORD status = 0, len = sizeof(status);
			WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
				WINHTTP_NO_HEADER_INDEX);
			r.status = static_cast<int>(status);
			for (;;) {
				DWORD avail = 0;
				if (!WinHttpQueryDataAvailable(req, &avail)) {
					r.error = cancelled ? "stopped" : LastError("reading the answer");
					r.status = cancelled ? 0 : r.status;
					break;
				}
				if (avail == 0) {
					break;
				}
				std::string chunk(avail, '\0');
				DWORD       got = 0;
				if (!WinHttpReadData(req, chunk.data(), avail, &got)) {
					r.error = cancelled ? "stopped" : LastError("reading the answer");
					break;
				}
				r.body.append(chunk.data(), got);
			}
		}
		finished = true;
		watcher.join();
		if (HINTERNET h = req.exchange(nullptr)) {
			WinHttpCloseHandle(h);
		}
		WinHttpCloseHandle(conn);
		WinHttpCloseHandle(session);
		return r;
	}

	RunResult Run(const std::filesystem::path& a_exe, const std::vector<std::string>& a_args, const std::filesystem::path& a_workDir,
		int a_timeoutSeconds, const std::atomic<bool>& a_cancel, std::size_t a_maxOutput)
	{
		RunResult r;
		std::wstring cmd = QuoteArg(a_exe.wstring());
		for (const auto& a : a_args) {
			cmd += L" " + QuoteArg(Wide(a));
		}
		SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
		HANDLE              rd = nullptr, wr = nullptr;
		if (!CreatePipe(&rd, &wr, &sa, 0)) {
			r.error = "could not make a pipe";
			return r;
		}
		SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdOutput = wr;
		si.hStdError = wr;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		PROCESS_INFORMATION pi{};
		std::wstring        dir = a_workDir.wstring();
		if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.empty() ? nullptr : dir.c_str(),
				&si, &pi)) {
			r.error = "could not start " + a_exe.filename().string() + " (Windows error " + std::to_string(GetLastError()) + ")";
			CloseHandle(rd);
			CloseHandle(wr);
			return r;
		}
		CloseHandle(wr);
		const auto  t0 = std::chrono::steady_clock::now();
		std::string out;
		auto        drain = [&] {
            for (;;) {
                DWORD avail = 0;
                if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
                    return;
                }
                std::string chunk(avail, '\0');
                DWORD       got = 0;
                if (!ReadFile(rd, chunk.data(), avail, &got, nullptr) || got == 0) {
                    return;
                }
                if (out.size() < (64u << 20)) {
                    out.append(chunk.data(), got);
                }
            }
		};
		for (;;) {
			drain();
			if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
				drain();
				break;
			}
			if (a_cancel) {
				r.cancelled = true;
			} else if (a_timeoutSeconds > 0 &&
					   std::chrono::steady_clock::now() - t0 > std::chrono::seconds(a_timeoutSeconds)) {
				r.timedOut = true;
			}
			if (r.cancelled || r.timedOut) {
				TerminateProcess(pi.hProcess, 1);
				WaitForSingleObject(pi.hProcess, 5000);
				drain();
				break;
			}
		}
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		r.exitCode = static_cast<int>(code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		CloseHandle(rd);
		r.output = CutMiddle(std::move(out), a_maxOutput);
		return r;
	}

	std::string ProtectSecret(const std::string& a_secret)
	{
		if (a_secret.empty()) {
			return {};
		}
		DATA_BLOB in{ static_cast<DWORD>(a_secret.size()), reinterpret_cast<BYTE*>(const_cast<char*>(a_secret.data())) }, out{};
		if (!CryptProtectData(&in, L"ReSaverPP AI key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
			return "plain:" + a_secret;
		}
		auto s = "dpapi:" + ToHex(out.pbData, out.cbData);
		LocalFree(out.pbData);
		return s;
	}

	std::string UnprotectSecret(const std::string& a_stored)
	{
		if (a_stored.rfind("plain:", 0) == 0) {
			return a_stored.substr(6);
		}
		if (a_stored.rfind("dpapi:", 0) != 0) {
			return a_stored;
		}
		auto      bytes = FromHex(a_stored.substr(6));
		DATA_BLOB in{ static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data()) }, out{};
		if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
			return {};  // another Windows user / another PC: the key has to be typed again
		}
		std::string s(reinterpret_cast<char*>(out.pbData), out.cbData);
		SecureZeroMemory(out.pbData, out.cbData);
		LocalFree(out.pbData);
		return s;
	}
#else
	namespace
	{
		std::string ShellQuote(const std::string& a_s)
		{
			std::string q = "'";
			for (char c : a_s) {
				if (c == '\'') {
					q += "'\\''";
				} else {
					q += c;
				}
			}
			return q + "'";
		}

		std::string ReadPipe(const std::string& a_cmd, int* a_status)
		{
			std::string out;
			FILE*       p = popen(a_cmd.c_str(), "r");
			if (!p) {
				*a_status = -1;
				return out;
			}
			char buf[65536];
			for (std::size_t n; (n = std::fread(buf, 1, sizeof(buf), p)) > 0;) {
				out.append(buf, n);
			}
			const int st = pclose(p);
			*a_status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
			return out;
		}
	}

	// elsewhere (Linux, for testing): the curl program
	Response Request(const std::string& a_method, const std::string& a_url, const Headers& a_headers, const std::string& a_body,
		const std::atomic<bool>&, int a_timeoutSeconds)
	{
		Response   r;
		const auto tmp = std::filesystem::temp_directory_path() / ("resaverpp-ai-" + std::to_string(::getpid()) + ".json");
		{
			std::ofstream f(tmp, std::ios::binary);
			f << a_body;
		}
		std::string cmd = "curl -s -S -X " + a_method + " --max-time " + std::to_string(a_timeoutSeconds) + " -w '\\n%{http_code}'";
		for (const auto& [k, v] : a_headers) {
			cmd += " -H " + ShellQuote(k + ": " + v);
		}
		if (!a_body.empty()) {
			cmd += " --data-binary @" + ShellQuote(tmp.string());
		}
		cmd += " " + ShellQuote(a_url) + " 2>&1";
		int  st = 0;
		auto out = ReadPipe(cmd, &st);
		std::error_code ec;
		std::filesystem::remove(tmp, ec);
		const auto nl = out.rfind('\n');
		if (nl == std::string::npos) {
			r.error = out.empty() ? "curl failed" : out;
			return r;
		}
		r.status = std::atoi(out.c_str() + nl + 1);
		r.body = out.substr(0, nl);
		if (r.status == 0) {
			r.error = r.body.empty() ? "no answer" : r.body;
		}
		return r;
	}

	RunResult Run(const std::filesystem::path& a_exe, const std::vector<std::string>& a_args, const std::filesystem::path& a_workDir,
		int a_timeoutSeconds, const std::atomic<bool>&, std::size_t a_maxOutput)
	{
		RunResult   r;
		std::string cmd;
		if (!a_workDir.empty()) {
			cmd = "cd " + ShellQuote(a_workDir.string()) + " && ";
		}
		cmd += "timeout " + std::to_string(a_timeoutSeconds) + " " + ShellQuote(a_exe.string());
		for (const auto& a : a_args) {
			cmd += " " + ShellQuote(a);
		}
		cmd += " 2>&1";
		int st = 0;
		r.output = CutMiddle(ReadPipe(cmd, &st), a_maxOutput);
		r.exitCode = st;
		r.timedOut = st == 124;
		return r;
	}

	std::string ProtectSecret(const std::string& a_secret) { return a_secret.empty() ? std::string() : "plain:" + a_secret; }
	std::string UnprotectSecret(const std::string& a_stored) { return a_stored.rfind("plain:", 0) == 0 ? a_stored.substr(6) : a_stored; }
#endif
}
