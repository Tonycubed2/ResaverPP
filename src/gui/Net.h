// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// What the AI chat needs from the system: HTTPS requests (WinHTTP on Windows, the curl program elsewhere),
// running resaverpp-cli with its output captured, and keeping the API key encrypted for the Windows user
// (DPAPI) in resaverpp.ini.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace net
{
	struct Response
	{
		int         status = 0;   // HTTP status; 0 when no reply came
		std::string body;
		std::string error;        // why there was no reply (no connection, cancelled...)
	};
	using Headers = std::vector<std::pair<std::string, std::string>>;
	// a_method "GET" or "POST"; a_cancel set from another thread stops the request
	Response Request(const std::string& a_method, const std::string& a_url, const Headers& a_headers, const std::string& a_body,
		const std::atomic<bool>& a_cancel, int a_timeoutSeconds);

	struct RunResult
	{
		int         exitCode = -1;
		std::string output;    // stdout and stderr together
		bool        timedOut = false, cancelled = false;
		std::string error;     // could not start it
	};
	RunResult Run(const std::filesystem::path& a_exe, const std::vector<std::string>& a_args, const std::filesystem::path& a_workDir,
		int a_timeoutSeconds, const std::atomic<bool>& a_cancel, std::size_t a_maxOutput);

	// "dpapi:<hex>" on Windows (only this Windows user can read it back), "plain:<text>" elsewhere
	std::string ProtectSecret(const std::string& a_secret);
	std::string UnprotectSecret(const std::string& a_stored);
}
