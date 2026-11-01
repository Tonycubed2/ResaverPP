// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Skyrim SE BSA archives (version 104 / 105): file listing and extraction.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ess
{
	class BsaArchive
	{
	public:
		struct Entry
		{
			std::string   path;  // lowercase, backslashes: "scripts\\foo.pex"
			std::uint32_t size = 0;
			std::uint32_t offset = 0;
			bool          compressed = false;
		};

		// Reads the header and directory (not the file data). Throws on errors.
		explicit BsaArchive(const std::filesystem::path& a_path);

		const std::vector<Entry>& entries() const { return _entries; }
		const Entry*              find(const std::string& a_lowerPath) const;
		std::vector<std::uint8_t> extract(const Entry& a_entry) const;  // throws on errors

		std::uint32_t version() const { return _version; }

	private:
		std::filesystem::path _path;
		std::uint32_t         _version = 0, _flags = 0;
		std::vector<Entry>    _entries;
	};

	// Unpacks the files of a BSA into a folder, keeping their folders ("scripts\\foo.pex" -> <out>\\scripts\\foo.pex).
	// a_filter: only paths containing it (lowercase match, "" = all). Existing files are kept unless a_overwrite.
	// a_progress(done, total) is called after each file; returning false stops.
	struct BsaExtractResult
	{
		std::size_t              written = 0, skipped = 0, failed = 0;
		std::uint64_t            bytes = 0;
		std::vector<std::string> errors;  // the first few
	};
	BsaExtractResult ExtractBsa(const BsaArchive& a_bsa, const std::filesystem::path& a_out, const std::string& a_filter, bool a_overwrite,
		const std::function<bool(std::size_t, std::size_t)>& a_progress = {});

	// Decodes an LZ4 frame (the format SE BSAs use for compressed files).
	std::vector<std::uint8_t> DecompressLZ4Frame(const std::uint8_t* a_src, std::size_t a_srcLen, std::size_t a_dstLen);
}
