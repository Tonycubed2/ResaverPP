// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Zip.h"

#include "miniz.h"

#include <chrono>
#include <ctime>
#include <cstdlib>
#include <stdexcept>

namespace ess::zip
{
	namespace
	{
		void Put16(std::string& o, std::uint16_t v)
		{
			o.push_back(static_cast<char>(v & 0xFF));
			o.push_back(static_cast<char>(v >> 8));
		}
		void Put32(std::string& o, std::uint32_t v)
		{
			for (int i = 0; i < 4; ++i) {
				o.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
			}
		}
		void DosTime(std::filesystem::file_time_type a_t, std::uint16_t& a_time, std::uint16_t& a_date)
		{
			const auto now = std::filesystem::file_time_type::clock::now();
			const auto secs = static_cast<std::time_t>(std::time(nullptr) + std::chrono::duration_cast<std::chrono::seconds>(a_t - now).count());
			std::tm    tm{};
#ifdef _WIN32
			localtime_s(&tm, &secs);
#else
			localtime_r(&secs, &tm);
#endif
			const int year = tm.tm_year + 1900 < 1980 ? 1980 : tm.tm_year + 1900;
			a_time = static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
			a_date = static_cast<std::uint16_t>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
		}
	}

	Writer::Writer(const std::filesystem::path& a_file) :
		_out(a_file, std::ios::binary | std::ios::trunc)
	{
		if (!_out) {
			throw std::runtime_error("cannot create " + a_file.string());
		}
	}

	Writer::~Writer()
	{
		if (!_closed) {
			try {
				close();
			} catch (...) {
			}
		}
	}

	void Writer::add(const std::string& a_name, const std::vector<std::uint8_t>& a_data, std::filesystem::file_time_type a_time)
	{
		if (_offset + a_data.size() > 0xF0000000ull || a_data.size() > 0xF0000000ull) {
			throw std::runtime_error("the zip would be over 4 GB");
		}
		Entry e;
		e.name = a_name;
		e.usize = static_cast<std::uint32_t>(a_data.size());
		e.crc = static_cast<std::uint32_t>(mz_crc32(MZ_CRC32_INIT, a_data.data(), a_data.size()));
		e.offset = static_cast<std::uint32_t>(_offset);
		DosTime(a_time, e.time, e.date);
		// raw deflate (no zlib header); stored when that is not smaller
		std::size_t clen = 0;
		void*       c = a_data.empty() ? nullptr : tdefl_compress_mem_to_heap(a_data.data(), a_data.size(), &clen, TDEFL_DEFAULT_MAX_PROBES);
		const bool  deflated = c && clen < a_data.size();
		e.method = deflated ? 8 : 0;
		e.csize = static_cast<std::uint32_t>(deflated ? clen : a_data.size());
		std::string h;
		Put32(h, 0x04034b50);
		Put16(h, 20);
		Put16(h, 0x0800);  // names are UTF-8
		Put16(h, e.method);
		Put16(h, e.time);
		Put16(h, e.date);
		Put32(h, e.crc);
		Put32(h, e.csize);
		Put32(h, e.usize);
		Put16(h, static_cast<std::uint16_t>(e.name.size()));
		Put16(h, 0);
		h += e.name;
		_out.write(h.data(), static_cast<std::streamsize>(h.size()));
		if (deflated) {
			_out.write(static_cast<const char*>(c), static_cast<std::streamsize>(clen));
		} else if (!a_data.empty()) {
			_out.write(reinterpret_cast<const char*>(a_data.data()), static_cast<std::streamsize>(a_data.size()));
		}
		if (c) {
			mz_free(c);
		}
		if (!_out) {
			throw std::runtime_error("could not write the zip (disk full?)");
		}
		_offset += h.size() + e.csize;
		_entries.push_back(std::move(e));
	}

	void Writer::add(const std::string& a_name, const std::string& a_text)
	{
		add(a_name, std::vector<std::uint8_t>(a_text.begin(), a_text.end()), std::filesystem::file_time_type::clock::now());
	}

	bool Writer::addFile(const std::string& a_name, const std::filesystem::path& a_file)
	{
		std::ifstream f(a_file, std::ios::binary);
		if (!f) {
			return false;
		}
		std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		std::error_code ec;
		const auto      t = std::filesystem::last_write_time(a_file, ec);
		add(a_name, data, ec ? std::filesystem::file_time_type::clock::now() : t);
		return true;
	}

	void Writer::close()
	{
		if (_closed) {
			return;
		}
		_closed = true;
		std::string cd;
		for (const auto& e : _entries) {
			Put32(cd, 0x02014b50);
			Put16(cd, 20);
			Put16(cd, 20);
			Put16(cd, 0x0800);
			Put16(cd, e.method);
			Put16(cd, e.time);
			Put16(cd, e.date);
			Put32(cd, e.crc);
			Put32(cd, e.csize);
			Put32(cd, e.usize);
			Put16(cd, static_cast<std::uint16_t>(e.name.size()));
			Put16(cd, 0);
			Put16(cd, 0);
			Put16(cd, 0);
			Put16(cd, 0);
			Put32(cd, 0);
			Put32(cd, e.offset);
			cd += e.name;
		}
		std::string end;
		Put32(end, 0x06054b50);
		Put16(end, 0);
		Put16(end, 0);
		Put16(end, static_cast<std::uint16_t>(_entries.size()));
		Put16(end, static_cast<std::uint16_t>(_entries.size()));
		Put32(end, static_cast<std::uint32_t>(cd.size()));
		Put32(end, static_cast<std::uint32_t>(_offset));
		Put16(end, 0);
		_out.write(cd.data(), static_cast<std::streamsize>(cd.size()));
		_out.write(end.data(), static_cast<std::streamsize>(end.size()));
		_out.close();
		if (!_out) {
			throw std::runtime_error("could not finish the zip");
		}
	}
}
