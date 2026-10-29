// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Setting a script property in a plugin file, like the Creation Kit's property window: the script data
// (VMAD) of one record is changed in place and the file written back, with a backup of the original.
// Only the record changes; the rest of the file is copied byte for byte.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ess::plugedit
{
	struct Header
	{
		bool                     ok = false;
		std::uint32_t            flags = 0;  // 0x1 master, 0x80 localized, 0x200 light
		std::vector<std::string> masters;
	};
	Header ReadHeader(const std::filesystem::path& a_plugin);

	struct Result
	{
		bool                  ok = false;
		std::string           message;  // what was done, or why not
		std::filesystem::path backup;   // the original file, kept next to it
	};

	// In the plugin a_plugin, the record a_recordID (a form id as the plugin itself writes it: master index in
	// the top byte) has a_script attached; its object property a_property is set to a_valueID (also as the
	// plugin writes form ids). The property is added if the script does not have it filled yet.
	Result SetObjectProperty(const std::filesystem::path& a_plugin, std::uint32_t a_recordID, const std::string& a_script,
		const std::string& a_property, std::uint32_t a_valueID);

	// Reads back an object property (for checks and tests): the form id stored, 0 if none.
	std::uint32_t GetObjectProperty(const std::filesystem::path& a_plugin, std::uint32_t a_recordID, const std::string& a_script,
		const std::string& a_property);
}
