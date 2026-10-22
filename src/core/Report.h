// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Printable reports: every mod with its scripts and instances, and the anomalies found in the
// save. Written as plain text or as a PDF (a small built-in writer, no external libraries).
#pragma once

#include "Mods.h"
#include "PluginDB.h"
#include "Save.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ess::report
{
	struct Options
	{
		bool mods = true;       // all mods/plugins with their scripts, instances and running threads
		bool anomalies = true;  // everything the analysis flags
		bool pdf = false;       // write a PDF instead of a .txt
	};

	// One line per entry. A line starting with '#' is a heading (bold in the PDF), "##" a
	// sub-heading; the marker is not printed.
	std::vector<std::string> Build(SaveFile& a_save, const mods::Table& a_mods, const PluginDB* a_names, const Options& a_opt,
		const std::string& a_saveName);

	void WriteText(const std::filesystem::path& a_path, const std::vector<std::string>& a_lines);
	void WritePdf(const std::filesystem::path& a_path, const std::string& a_title, const std::vector<std::string>& a_lines);
}
