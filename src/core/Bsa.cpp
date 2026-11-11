// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Bsa.h"

#include "ByteIO.h"
#include "Save.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <lz4.h>

namespace ess
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}
	}

	BsaArchive::BsaArchive(const std::filesystem::path& a_path) :
		_path(a_path)
	{
		std::ifstream f(a_path, std::ios::binary);
		if (!f) {
			throw std::runtime_error("cannot open");
		}
		std::uint8_t hdr[36];
		if (f.read(reinterpret_cast<char*>(hdr), 4) && std::memcmp(hdr, "BTDX", 4) == 0) {
			openBa2(f);
			return;
		}
		f.clear();
		f.seekg(0);
		if (!f.read(reinterpret_cast<char*>(hdr), 36) || std::memcmp(hdr, "BSA\0", 4) != 0) {
			throw std::runtime_error("not a BSA or BA2 archive");
		}
		Reader h(hdr, 36);
		h.seek(4);
		_version = h.u32();
		const auto offset = h.u32();
		_flags = h.u32();
		const auto folderCount = h.u32();
		const auto fileCount = h.u32();
		const auto folderNamesLen = h.u32();
		const auto fileNamesLen = h.u32();
		if (_version != 104 && _version != 105) {
			throw std::runtime_error("unsupported BSA version " + std::to_string(_version));
		}
		const bool        dirNames = _flags & 1, fileNames = _flags & 2, compressedByDefault = _flags & 4;
		const std::size_t folderRec = _version == 105 ? 24 : 16;
		const std::size_t need = offset + folderCount * folderRec + (dirNames ? folderNamesLen + folderCount : 0) + std::size_t(fileCount) * 16 + fileNamesLen;
		std::vector<std::uint8_t> buf(need);
		f.seekg(0);
		f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(need));
		buf.resize(static_cast<std::size_t>(f.gcount()));

		Reader r(buf.data(), buf.size());
		r.seek(offset);
		std::vector<std::uint32_t> counts(folderCount);
		for (auto& c : counts) {
			r.u64();  // hash
			c = r.u32();
			if (_version == 105) {
				r.u32();
				r.u64();
			} else {
				r.u32();
			}
		}
		std::vector<std::string> folders(folderCount);
		_entries.reserve(fileCount);
		for (std::uint32_t i = 0; i < folderCount; ++i) {
			if (dirNames) {
				const auto len = r.u8();
				auto       b = r.bytes(len);
				std::string s(b.begin(), b.end());
				while (!s.empty() && s.back() == '\0') {
					s.pop_back();
				}
				folders[i] = Lower(s);
			}
			for (std::uint32_t k = 0; k < counts[i]; ++k) {
				r.u64();
				std::uint32_t size = r.u32();
				const auto    off = r.u32();
				const bool    toggle = (size & (1u << 30)) != 0;
				size &= ~(1u << 30);
				Entry e;
				e.path = folders[i];
				e.size = size;
				e.offset = off;
				e.compressed = compressedByDefault != toggle;
				_entries.push_back(std::move(e));
			}
		}
		if (fileNames) {
			for (auto& e : _entries) {
				std::string name;
				for (char c; r.remaining() && (c = static_cast<char>(r.u8())) != '\0';) {
					name.push_back(c);
				}
				e.path += "\\" + Lower(name);
			}
		}
	}

	// Fallout 4 BA2 ("BTDX"): a 24-byte header (version 1, and 7 / 8 since the 2024 update, have the same layout),
	// then one record per file, then the data, then the name table (u16 length + characters per file, in record
	// order). Checked against Tony's Fallout 4 1.11 archives (v1 DLCCoast, v7 textures, v8 general).
	//   header:  "BTDX", u32 version, char[4] type "GNRL" / "DX10", u32 file count, u64 name table offset
	//   GNRL:    u32 name hash, char[4] ext, u32 dir hash, u32 flags, u64 offset, u32 packed size (0 = stored),
	//            u32 size, u32 0xBAADF00D                                                       (36 bytes)
	//   DX10:    u32 name hash, char[4] ext, u32 dir hash, u8 unknown, u8 chunk count, u16 chunk header size (24),
	//            u16 height, u16 width, u8 mips, u8 DXGI format, u8 flags (1: cube map), u8 tile mode   (24 bytes)
	//            then per chunk: u64 offset, u32 packed size (0 = stored), u32 size, u16 first mip, u16 last mip,
	//            u32 0xBAADF00D                                                                  (24 bytes)
	// Compressed data is zlib.
	// Starfield (checked on its 1.16.244 archives and Starfield.exe's reader 0x1427de4a0: version <= 3, 8 more bytes from
	// version 2, a u32 compression from version 3): version 2 (GNRL and DX10) has 8 more header bytes (u32 1, u32 0), version
	// 3 (its DX10 texture archives) 12 (u32 1, u32 0, u32 compression: 3 = LZ4 blocks); the records are Fallout 4's (GNRL 36
	// bytes, DX10 24 + 24 per chunk, tile mode 8 in the last byte); names use '/' ("strings/starfield_en.strings").
	void BsaArchive::openBa2(std::ifstream& a_f)
	{
		_ba2 = true;
		std::uint8_t hdr[24];
		a_f.seekg(0);
		if (!a_f.read(reinterpret_cast<char*>(hdr), 24)) {
			throw std::runtime_error("BA2 header cut short");
		}
		Reader h(hdr, 24);
		h.seek(4);
		_version = h.u32();
		char type[4];
		std::memcpy(type, hdr + 8, 4);
		h.seek(12);
		const auto count = h.u32();
		const auto names = h.u64();
		if (_version != 1 && _version != 7 && _version != 8 && _version != 2 && _version != 3) {
			throw std::runtime_error("unsupported BA2 version " + std::to_string(_version) + " (Fallout 4 uses 1, 7 and 8, Starfield 2 and 3)");
		}
		// Starfield: version 2 adds two u32 (1, 0 in every archive of Starfield 1.16.244) after the 24-byte header, version 3
		// a third, the compression of the data: 0 zlib, 3 LZ4 blocks (3 in all of the game's v3 archives, its texture archives)
		std::uint64_t recStart = 24;
		if (_version == 2 || _version == 3) {
			std::uint8_t ext[12]{};
			if (!a_f.read(reinterpret_cast<char*>(ext), _version == 3 ? 12 : 8)) {
				throw std::runtime_error("BA2 header cut short");
			}
			recStart = _version == 3 ? 36 : 32;
			if (_version == 3) {
				std::memcpy(&_compression, ext + 8, 4);
				if (_compression != 0 && _compression != 3) {
					throw std::runtime_error("unsupported BA2 compression " + std::to_string(_compression));
				}
			}
		}
		if (std::memcmp(type, "GNRL", 4) == 0) {
			_textureArchive = false;
		} else if (std::memcmp(type, "DX10", 4) == 0) {
			_textureArchive = true;
		} else {
			throw std::runtime_error("unsupported BA2 type " + std::string(type, 4));
		}
		a_f.seekg(0, std::ios::end);
		const auto fileSize = static_cast<std::uint64_t>(a_f.tellg());
		if (names > fileSize || count > 10'000'000) {
			throw std::runtime_error("BA2 header is damaged");
		}
		// the records: read in one go (GNRL: 36 bytes each; DX10: 24 + 24 per chunk, read as they come)
		a_f.seekg(static_cast<std::streamoff>(recStart));
		const std::uint64_t recEnd = names ? names : fileSize;
		_entries.reserve(count);
		if (!_textureArchive) {
			std::vector<std::uint8_t> buf(std::size_t(count) * 36);
			if (!a_f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))) {
				throw std::runtime_error("BA2 file records cut short");
			}
			Reader r(buf.data(), buf.size());
			for (std::uint32_t i = 0; i < count; ++i) {
				r.seek(std::size_t(i) * 36 + 16);
				Entry e;
				e.offset = r.u64();
				e.packedSize = r.u32();
				e.size = r.u32();
				if (r.u32() != 0xBAADF00Du) {
					throw std::runtime_error("BA2 file record " + std::to_string(i) + " is damaged");
				}
				e.compressed = e.packedSize != 0;
				_entries.push_back(std::move(e));
			}
		} else {
			_textures.reserve(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				std::uint8_t rec[24];
				if (!a_f.read(reinterpret_cast<char*>(rec), 24)) {
					throw std::runtime_error("BA2 texture records cut short");
				}
				Reader r(rec, 24);
				r.seek(13);
				Texture t;
				const auto chunks = r.u8();
				const auto chunkSize = r.u16();
				t.height = r.u16();
				t.width = r.u16();
				t.mips = r.u8();
				t.format = r.u8();
				t.flags = r.u8();
				if (chunkSize != 24) {
					throw std::runtime_error("BA2 texture record " + std::to_string(i) + " is damaged");
				}
				Entry e;
				e.texture = static_cast<std::int32_t>(_textures.size());
				for (std::uint8_t c = 0; c < chunks; ++c) {
					std::uint8_t cr[24];
					if (!a_f.read(reinterpret_cast<char*>(cr), 24)) {
						throw std::runtime_error("BA2 texture chunks cut short");
					}
					Reader q(cr, 24);
					Chunk k;
					k.offset = q.u64();
					k.packedSize = q.u32();
					k.size = q.u32();
					q.u16();
					q.u16();
					if (q.u32() != 0xBAADF00Du) {
						throw std::runtime_error("BA2 texture chunk of record " + std::to_string(i) + " is damaged");
					}
					e.compressed = e.compressed || k.packedSize != 0;
					e.size += k.size;
					t.chunks.push_back(k);
				}
				e.offset = t.chunks.empty() ? 0 : t.chunks.front().offset;
				_textures.push_back(std::move(t));
				_entries.push_back(std::move(e));
			}
		}
		if (static_cast<std::uint64_t>(a_f.tellg()) > recEnd) {
			throw std::runtime_error("BA2 records run past the name table");
		}
		if (names) {
			const auto                len = fileSize - names;
			std::vector<std::uint8_t> buf(static_cast<std::size_t>(len));
			a_f.seekg(static_cast<std::streamoff>(names));
			if (!a_f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(len))) {
				throw std::runtime_error("BA2 name table cut short");
			}
			Reader r(buf.data(), buf.size());
			for (auto& e : _entries) {
				if (r.remaining() < 2) {
					throw std::runtime_error("BA2 name table has fewer names than files");
				}
				const auto n = r.u16();
				auto       b = r.bytes(n);
				std::string name(b.begin(), b.end());
				std::replace(name.begin(), name.end(), '/', '\\');
				e.path = Lower(name);
			}
		}
	}

	std::string BsaArchive::kind() const
	{
		if (_ba2) {
			return std::string("BA2 ") + (_textureArchive ? "DX10" : "GNRL") + " v" + std::to_string(_version);
		}
		return "BSA " + std::to_string(_version);
	}

	std::vector<std::uint8_t> MakeDdsHeader(const BsaArchive::Texture& a_t)
	{
		const std::uint32_t w = a_t.width, h = a_t.height, fmt = a_t.format;
		// bytes per 4x4 block (block compressed) or per pixel (others); fourCC 0: written with the DX10 header
		std::uint32_t block = 0, pixel = 0;
		char          fourCC[5] = "";
		switch (fmt) {
		case 70: case 71: case 72: block = 8; if (fmt == 71) std::memcpy(fourCC, "DXT1", 4); break;          // BC1
		case 73: case 74: case 75: block = 16; if (fmt == 74) std::memcpy(fourCC, "DXT3", 4); break;         // BC2
		case 76: case 77: case 78: block = 16; if (fmt == 77) std::memcpy(fourCC, "DXT5", 4); break;         // BC3
		case 79: case 80: case 81: block = 8; if (fmt == 80) std::memcpy(fourCC, "ATI1", 4); break;          // BC4
		case 82: case 83: case 84: block = 16; if (fmt == 83) std::memcpy(fourCC, "ATI2", 4); break;         // BC5
		case 94: case 95: case 96: case 97: case 98: case 99: block = 16; break;                           // BC6H, BC7
		case 27: case 28: case 29: case 30: case 31: case 32: case 87: case 88: case 90: case 91: case 24: case 10: pixel = fmt == 10 ? 8 : 4; break;
		case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 56: case 57: case 85: case 86: case 115: pixel = 2; break;
		case 60: case 61: case 62: case 63: case 64: case 65: pixel = 1; break;
		default: throw std::runtime_error("texture format " + std::to_string(fmt) + " (DXGI) is not known to this extractor");
		}
		const bool    cube = (a_t.flags & 1) != 0;
		const bool    legacyRgb = fmt == 87;  // B8G8R8A8: plain 32-bit RGBA, readable by every tool
		const bool    dx10 = !fourCC[0] && !legacyRgb;
		std::vector<std::uint8_t> o;
		auto put = [&](std::uint32_t v) {
			for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
		};
		o.insert(o.end(), { 'D', 'D', 'S', ' ' });
		const std::uint32_t pitch = block ? std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * block : w * pixel;
		put(124);                                                                  // dwSize
		put(0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (block ? 0x80000u : 0x8u));        // CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT, LINEARSIZE / PITCH
		put(h);
		put(w);
		put(pitch);
		put(0);                                                                    // depth
		put(std::max<std::uint32_t>(1, a_t.mips));
		for (int i = 0; i < 11; ++i) put(0);
		// pixel format
		put(32);
		if (legacyRgb) {
			put(0x41);  // DDPF_RGB | DDPF_ALPHAPIXELS
			put(0);
			put(32);
			put(0x00FF0000);
			put(0x0000FF00);
			put(0x000000FF);
			put(0xFF000000);
		} else {
			put(0x4);  // DDPF_FOURCC
			o.insert(o.end(), dx10 ? std::initializer_list<std::uint8_t>{ 'D', 'X', '1', '0' }
								   : std::initializer_list<std::uint8_t>{ std::uint8_t(fourCC[0]), std::uint8_t(fourCC[1]), std::uint8_t(fourCC[2]), std::uint8_t(fourCC[3]) });
			for (int i = 0; i < 5; ++i) put(0);
		}
		put(0x1000 | 0x8 | 0x400000);                // DDSCAPS_TEXTURE | COMPLEX | MIPMAP
		put(cube ? 0xFE00u : 0u);                   // DDSCAPS2_CUBEMAP + all six faces
		put(0);
		put(0);
		put(0);
		if (dx10) {
			put(fmt);            // DXGI_FORMAT
			put(3);              // D3D10_RESOURCE_DIMENSION_TEXTURE2D
			put(cube ? 4u : 0u); // D3D10_RESOURCE_MISC_TEXTURECUBE
			put(1);              // array size
			put(0);
		}
		return o;
	}

	const BsaArchive::Entry* BsaArchive::find(const std::string& a_lowerPath) const
	{
		for (const auto& e : _entries) {
			if (e.path == a_lowerPath) {
				return &e;
			}
		}
		return nullptr;
	}

	std::vector<std::uint8_t> BsaArchive::extract(const Entry& a_entry) const
	{
		std::ifstream f(_path, std::ios::binary);
		if (!f) {
			throw std::runtime_error("cannot open archive");
		}
		if (_ba2) {
			auto read = [&](std::uint64_t a_off, std::uint32_t a_packed, std::uint32_t a_size) {
				std::vector<std::uint8_t> raw(a_packed ? a_packed : a_size);
				f.seekg(static_cast<std::streamoff>(a_off));
				if (!f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
					throw std::runtime_error("short read in archive");
				}
				if (!a_packed) {
					return raw;
				}
				// Starfield v3: compression 3 = LZ4 blocks for TEXTURES only; general files are zlib in every version
				// (Starfield.exe 1.16.244: GNRL read 0x1427e29b0 always uses "ZIP ", DX10 0x14248ebf0 tests the field)
				if (_compression == 3 && _textureArchive) {
					std::vector<std::uint8_t> out(a_size);
					const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(raw.data()), reinterpret_cast<char*>(out.data()),
						static_cast<int>(raw.size()), static_cast<int>(out.size()));
					if (n < 0 || static_cast<std::uint32_t>(n) != a_size) {
						throw std::runtime_error("damaged LZ4 data in archive");
					}
					return out;
				}
				return DecompressZlib(raw.data(), raw.size(), a_size);
			};
			if (a_entry.texture < 0) {
				return read(a_entry.offset, a_entry.packedSize, a_entry.size);
			}
			const auto& t = _textures.at(static_cast<std::size_t>(a_entry.texture));
			auto        out = MakeDdsHeader(t);
			for (const auto& c : t.chunks) {
				const auto d = read(c.offset, c.packedSize, c.size);
				out.insert(out.end(), d.begin(), d.end());
			}
			return out;
		}
		std::vector<std::uint8_t> raw(a_entry.size);
		f.seekg(static_cast<std::streamoff>(a_entry.offset));
		if (!f.read(reinterpret_cast<char*>(raw.data()), a_entry.size)) {
			throw std::runtime_error("short read in archive");
		}
		std::size_t pos = 0;
		if (_flags & 0x100) {  // embedded file name: bstring
			pos = 1 + std::size_t(raw.empty() ? 0 : raw[0]);
		}
		if (pos > raw.size()) {
			throw std::runtime_error("bad embedded name");
		}
		if (!a_entry.compressed) {
			return { raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.end() };
		}
		if (raw.size() < pos + 4) {
			throw std::runtime_error("bad compressed entry");
		}
		std::uint32_t original;
		std::memcpy(&original, raw.data() + pos, 4);
		pos += 4;
		if (_version == 104) {
			return DecompressZlib(raw.data() + pos, raw.size() - pos, original);
		}
		return DecompressLZ4Frame(raw.data() + pos, raw.size() - pos, original);
	}

	BsaExtractResult ExtractBsa(const BsaArchive& a_bsa, const std::filesystem::path& a_out, const std::string& a_filter, bool a_overwrite,
		const std::function<bool(std::size_t, std::size_t)>& a_progress)
	{
		BsaExtractResult r;
		std::string      filter = a_filter;
		for (auto& c : filter) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		std::replace(filter.begin(), filter.end(), '/', '\\');
		std::vector<const BsaArchive::Entry*> todo;
		for (const auto& e : a_bsa.entries()) {
			if (filter.empty() || e.path.find(filter) != std::string::npos) {
				todo.push_back(&e);
			}
		}
		for (std::size_t i = 0; i < todo.size(); ++i) {
			const auto& e = *todo[i];
			// a path that would leave the folder is not written
			std::filesystem::path rel;
			bool                  bad = e.path.empty() || e.path.find(':') != std::string::npos;
			std::size_t           start = 0;
			while (!bad && start <= e.path.size()) {
				const auto end = e.path.find('\\', start);
				const auto part = e.path.substr(start, end == std::string::npos ? std::string::npos : end - start);
				if (part == ".." || part == "." || (part.empty() && end != std::string::npos)) {
					bad = true;
					break;
				}
				if (!part.empty()) {
					rel /= std::filesystem::u8path(part);
				}
				if (end == std::string::npos) {
					break;
				}
				start = end + 1;
			}
			if (bad) {
				++r.failed;
				if (r.errors.size() < 10) {
					r.errors.push_back(e.path + ": not a safe path, skipped");
				}
			} else {
				const auto      dst = a_out / rel;
				std::error_code ec;
				if (!a_overwrite && std::filesystem::exists(dst, ec)) {
					++r.skipped;
				} else {
					try {
						const auto data = a_bsa.extract(e);
						std::filesystem::create_directories(dst.parent_path(), ec);
						std::ofstream f(dst, std::ios::binary | std::ios::trunc);
						f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
						if (!f) {
							throw std::runtime_error("could not write " + dst.string());
						}
						++r.written;
						r.bytes += data.size();
					} catch (const std::exception& ex) {
						++r.failed;
						if (r.errors.size() < 10) {
							r.errors.push_back(e.path + ": " + ex.what());
						}
					}
				}
			}
			if (a_progress && !a_progress(i + 1, todo.size())) {
				break;
			}
		}
		return r;
	}

	std::vector<std::uint8_t> DecompressLZ4Frame(const std::uint8_t* a_src, std::size_t a_srcLen, std::size_t a_dstLen)
	{
		Reader r(a_src, a_srcLen);
		if (r.u32() != 0x184D2204) {
			throw std::runtime_error("not an LZ4 frame");
		}
		const auto flg = r.u8();
		r.u8();  // BD
		const bool blockChecksum = flg & 0x10, contentSize = flg & 0x08, dictID = flg & 0x01;
		if (contentSize) {
			r.u64();
		}
		if (dictID) {
			r.u32();
		}
		r.u8();  // header checksum
		std::vector<std::uint8_t> out(a_dstLen);
		std::size_t               pos = 0;
		for (;;) {
			const auto bsize = r.u32();
			if (bsize == 0) {
				break;
			}
			const bool          stored = bsize & 0x80000000u;
			const std::uint32_t len = bsize & 0x7FFFFFFFu;
			r.need(len);
			if (stored) {
				if (pos + len > out.size()) {
					throw std::runtime_error("LZ4 frame larger than expected");
				}
				std::memcpy(out.data() + pos, r.here(), len);
				pos += len;
			} else {
				// linked blocks: the previous output is the dictionary (it sits right before dst)
				const int n = LZ4_decompress_safe_usingDict(reinterpret_cast<const char*>(r.here()), reinterpret_cast<char*>(out.data() + pos),
					static_cast<int>(len), static_cast<int>(out.size() - pos), reinterpret_cast<const char*>(out.data()), static_cast<int>(pos));
				if (n < 0) {
					throw std::runtime_error("LZ4 frame block is corrupt");
				}
				pos += static_cast<std::size_t>(n);
			}
			r.seek(r.pos() + len);
			if (blockChecksum) {
				r.u32();
			}
		}
		if (pos != a_dstLen) {
			throw std::runtime_error("LZ4 frame size mismatch");
		}
		return out;
	}
}
