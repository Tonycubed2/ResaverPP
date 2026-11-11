#pragma once

// ReSaverHelper.ini, next to the DLL (Data\SFSE\Plugins). ReSaver Plus can change these through the pipe
// (SETOPTION); the helper writes the file back keeping its comments.
namespace Settings
{
	struct Values
	{
		std::string notify = "hud";         // sNotify: hud | box | none - how the helper confirms what it did
		bool        listen = true;          // bListen: accept commands from ReSaver Plus at all
		bool        rescueHotkey = true;    // bRescueHotkey: holding Ctrl saves and forces your controls on
		int         rescueHoldSeconds = 8;  // iRescueHoldSeconds: how long Ctrl must be held
	};

	Values&               Get();
	void                  Load();
	bool                  Save();  // writes the current values into the INI (creates it if missing)
	std::filesystem::path IniPath();

	// option by its INI key; false if the name or value is not valid
	bool        SetOption(const std::string& a_name, const std::string& a_value);
	std::string GetOption(const std::string& a_name);
}
