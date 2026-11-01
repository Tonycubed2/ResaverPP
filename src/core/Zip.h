// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// A small .zip writer (deflate with miniz, UTF-8 names, no zip64: under 4 GB in all), for "Collect all logs".
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ess::zip
{
	class Writer
	{
	public:
		explicit Writer(const std::filesystem::path& a_file);  // throws if it cannot be created
		~Writer();
		// a_name: the path inside the zip, '/' separated
		void add(const std::string& a_name, const std::vector<std::uint8_t>& a_data, std::filesystem::file_time_type a_time);
		void add(const std::string& a_name, const std::string& a_text);
		bool addFile(const std::string& a_name, const std::filesystem::path& a_file);  // false if it could not be read
		void close();  // writes the directory; throws on errors
		std::uint64_t bytesWritten() const { return _offset; }
		std::size_t   count() const { return _entries.size(); }

	private:
		struct Entry
		{
			std::string   name;
			std::uint32_t crc = 0, csize = 0, usize = 0, offset = 0;
			std::uint16_t method = 0, time = 0, date = 0;
		};
		std::ofstream      _out;
		std::vector<Entry> _entries;
		std::uint64_t      _offset = 0;
		bool               _closed = false;
	};
}
