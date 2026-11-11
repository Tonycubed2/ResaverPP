// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Skyrim BSA archives (version 104 / 105) and Fallout 4 / Starfield BA2 archives ("BTDX" version 1, 7, 8; Starfield 2, 3; GNRL general files
// and DX10 textures): file listing and extraction.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
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
			std::uint32_t size = 0;    // BSA: the stored size; BA2: the unpacked size (a texture: of all its chunks, no DDS header)
			std::uint64_t offset = 0;
			bool          compressed = false;
			std::uint32_t packedSize = 0;  // BA2 general file: the zlib size (0 = stored)
			std::int32_t  texture = -1;    // BA2 texture: index into the archive's texture table
		};

		// Reads the header and directory (not the file data). Throws on errors.
		explicit BsaArchive(const std::filesystem::path& a_path);

		const std::vector<Entry>& entries() const { return _entries; }
		const Entry*              find(const std::string& a_lowerPath) const;
		std::vector<std::uint8_t> extract(const Entry& a_entry) const;  // throws on errors

		std::uint32_t version() const { return _version; }
		bool          isBa2() const { return _ba2; }
		bool          isTextureArchive() const { return _ba2 && _textureArchive; }  // a BA2 of type DX10
		std::string   kind() const;  // "BSA 105", "BA2 GNRL v8", "BA2 DX10 v7"

		// A BA2 texture: the DDS header is made from this when it is extracted.
		struct Chunk
		{
			std::uint64_t offset = 0;
			std::uint32_t packedSize = 0, size = 0;  // packedSize 0: stored
		};
		struct Texture
		{
			std::uint16_t      height = 0, width = 0;
			std::uint8_t       mips = 0, format = 0, flags = 0;  // format: DXGI_FORMAT; flags & 1: a cube map
			std::vector<Chunk> chunks;
		};

	private:
		void openBa2(std::ifstream& a_file);

		std::filesystem::path _path;
		std::uint32_t         _version = 0, _flags = 0;
		std::uint32_t         _compression = 0;  // BA2 version 3 (Starfield): 0 zlib, 3 LZ4 blocks
		bool                  _ba2 = false, _textureArchive = false;
		std::vector<Entry>    _entries;
		std::vector<Texture>  _textures;
	};

	// The DDS header (with "DDS " and, for formats without a legacy code, the DX10 header) of a BA2 texture.
	// Throws for a DXGI format it does not know.
	std::vector<std::uint8_t> MakeDdsHeader(const BsaArchive::Texture& a_tex);

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
