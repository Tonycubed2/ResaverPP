#pragma once

#include <string>
#include <vector>

// The local named pipe ReSaver++ connects to. Only the Windows user running the game can open it
// (owner-only access, remote clients rejected), and it only understands the commands in Protocol.h.
namespace Server
{
	void Start();

	// ReSaver Plus is connected right now (version 15)
	bool Connected();

	// runs one request as if it came through the pipe: a_fields = <id> <COMMAND> <args...>; the reply's fields after the id
	// (version 23: the F1 page's "Ask the AI" sends its game commands here). Never on the main thread: commands wait for it.
	std::vector<std::string> Run(const std::vector<std::string>& a_fields);
}
