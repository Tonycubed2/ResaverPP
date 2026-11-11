#pragma once

// The local named pipe ReSaver Plus connects to. Only the Windows user running the game can open it
// (owner-only access, remote clients rejected), and it only understands the commands in Protocol.h.
namespace Server
{
	void Start();
}
