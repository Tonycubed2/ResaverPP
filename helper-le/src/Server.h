#pragma once

// The named pipe server ReSaver++ talks to (see Protocol.h). Started once the game's data is loaded.
namespace Server
{
	void SetGameVersion(std::string a_version);  // as HELLO reports it, e.g. 1.9.32.0
	void Start();
}
