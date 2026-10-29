// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "PluginEdit.h"

#include "Save.h"  // CompressZlib / DecompressZlib

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>

namespace ess::plugedit
{
	namespace
	{
		namespace fs = std::filesystem;

		std::uint16_t U16(const std::uint8_t* p)
		{
			std::uint16_t v;
			std::memcpy(&v, p, 2);
			return v;
		}
		std::uint32_t U32(const std::uint8_t* p)
		{
			std::uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
		}
		void Put32(std::uint8_t* p, std::uint32_t v) { std::memcpy(p, &v, 4); }
		void Put16(std::uint8_t* p, std::uint16_t v) { std::memcpy(p, &v, 2); }
		std::string Lower(std::string s)
		{
			for (auto& c : s) {
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return s;
		}
		std::vector<std::uint8_t> ReadAll(const fs::path& p)
		{
			std::ifstream f(p, std::ios::binary);
			return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
		}

		// where a record is in the file, and the groups it sits in (their offsets, outermost first)
		struct Found
		{
			std::size_t              at = 0;
			std::vector<std::size_t> groups;
		};
		std::optional<Found> FindRecord(const std::vector<std::uint8_t>& d, std::uint32_t a_id)
		{
			if (d.size() < 24 || std::memcmp(d.data(), "TES4", 4) != 0) {
				return std::nullopt;
			}
			std::optional<Found>     out;
			std::vector<std::size_t> stack;
			std::function<void(std::size_t, std::size_t)> walk = [&](std::size_t p, std::size_t end) {
				while (!out && p + 24 <= end) {
					if (std::memcmp(d.data() + p, "GRUP", 4) == 0) {
						const auto size = U32(d.data() + p + 4);
						if (size < 24 || p + size > end) {
							return;
						}
						stack.push_back(p);
						walk(p + 24, p + size);
						if (out) {
							return;
						}
						stack.pop_back();
						p += size;
					} else {
						const auto size = U32(d.data() + p + 4);
						if (U32(d.data() + p + 12) == a_id) {
							out = Found{ p, stack };
							return;
						}
						p += 24 + size;
					}
				}
			};
			walk(24 + U32(d.data() + 4), d.size());
			return out;
		}

		// a record's fields: decompressed if needed
		std::vector<std::uint8_t> Fields(const std::vector<std::uint8_t>& d, std::size_t at)
		{
			const auto size = U32(d.data() + at + 4);
			const auto flags = U32(d.data() + at + 8);
			const auto* data = d.data() + at + 24;
			if (flags & 0x40000) {
				if (size < 4) {
					throw std::runtime_error("broken compressed record");
				}
				return DecompressZlib(data + 4, size - 4, U32(data));
			}
			return std::vector<std::uint8_t>(data, data + size);
		}

		struct Sub
		{
			char                      sig[4];
			std::vector<std::uint8_t> data;
		};
		std::vector<Sub> Subs(const std::vector<std::uint8_t>& f)
		{
			std::vector<Sub> out;
			std::size_t      p = 0;
			std::uint32_t    bigSize = 0;
			while (p + 6 <= f.size()) {
				Sub s;
				std::memcpy(s.sig, f.data() + p, 4);
				std::size_t len = U16(f.data() + p + 4);
				p += 6;
				if (std::memcmp(s.sig, "XXXX", 4) == 0) {  // the next field is larger than 64 KB
					bigSize = U32(f.data() + p);
					p += len;
					continue;
				}
				if (bigSize) {
					len = bigSize;
					bigSize = 0;
				}
				if (p + len > f.size()) {
					throw std::runtime_error("broken record fields");
				}
				s.data.assign(f.begin() + static_cast<std::ptrdiff_t>(p), f.begin() + static_cast<std::ptrdiff_t>(p + len));
				out.push_back(std::move(s));
				p += len;
			}
			return out;
		}
		std::vector<std::uint8_t> Join(const std::vector<Sub>& a_subs)
		{
			std::vector<std::uint8_t> out;
			for (const auto& s : a_subs) {
				if (s.data.size() > 0xFFFF) {
					const char x[4] = { 'X', 'X', 'X', 'X' };
					out.insert(out.end(), x, x + 4);
					out.push_back(4);
					out.push_back(0);
					std::uint8_t n[4];
					Put32(n, static_cast<std::uint32_t>(s.data.size()));
					out.insert(out.end(), n, n + 4);
					out.insert(out.end(), s.sig, s.sig + 4);
					out.push_back(0);
					out.push_back(0);
				} else {
					out.insert(out.end(), s.sig, s.sig + 4);
					out.push_back(static_cast<std::uint8_t>(s.data.size() & 0xFF));
					out.push_back(static_cast<std::uint8_t>(s.data.size() >> 8));
				}
				out.insert(out.end(), s.data.begin(), s.data.end());
			}
			return out;
		}

		// the script data (VMAD): where each script's property list is and where each property's value is
		struct VmadProp
		{
			std::string name;
			std::uint8_t type = 0;
			std::size_t valueAt = 0;  // offset of the value in the VMAD data
		};
		struct VmadScript
		{
			std::string           name;
			std::size_t           countAt = 0;  // offset of its property count
			std::size_t           endAt = 0;    // offset just after its last property
			std::vector<VmadProp> props;
		};
		struct Vmad
		{
			std::int16_t            version = 0, objFormat = 0;
			std::vector<VmadScript> scripts;
		};
		Vmad ParseVmad(const std::vector<std::uint8_t>& v)
		{
			Vmad        out;
			std::size_t p = 0;
			auto        need = [&](std::size_t n) {
				if (p + n > v.size()) {
					throw std::runtime_error("the record's script data is cut off");
				}
			};
			auto wstr = [&]() {
				need(2);
				const auto n = U16(v.data() + p);
				p += 2;
				need(n);
				std::string s(reinterpret_cast<const char*>(v.data() + p), n);
				p += n;
				return s;
			};
			need(6);
			std::memcpy(&out.version, v.data(), 2);
			std::memcpy(&out.objFormat, v.data() + 2, 2);
			const auto count = U16(v.data() + 4);
			p = 6;
			for (std::uint16_t i = 0; i < count; ++i) {
				VmadScript sc;
				sc.name = wstr();
				if (out.version >= 4) {
					need(1);
					++p;  // status
				}
				need(2);
				sc.countAt = p;
				const auto pc = U16(v.data() + p);
				p += 2;
				for (std::uint16_t k = 0; k < pc; ++k) {
					VmadProp pr;
					pr.name = wstr();
					need(1);
					pr.type = v[p++];
					if (out.version >= 4) {
						need(1);
						++p;  // status
					}
					pr.valueAt = p;
					auto arr = [&](std::size_t each) {
						need(4);
						const auto n = U32(v.data() + p);
						p += 4;
						for (std::uint32_t j = 0; j < n; ++j) {
							if (each) {
								need(each);
								p += each;
							} else {
								wstr();
							}
						}
					};
					switch (pr.type) {
					case 1: need(8); p += 8; break;
					case 2: wstr(); break;
					case 3:
					case 4: need(4); p += 4; break;
					case 5: need(1); p += 1; break;
					case 11: arr(8); break;
					case 12: arr(0); break;
					case 13:
					case 14: arr(4); break;
					case 15: arr(1); break;
					default: throw std::runtime_error("unknown property type " + std::to_string(pr.type) + " in the record's script data");
					}
					sc.props.push_back(std::move(pr));
				}
				sc.endAt = p;
				out.scripts.push_back(std::move(sc));
			}
			return out;
		}
		void WriteObject(std::uint8_t* at, std::int16_t a_objFormat, std::uint32_t a_id)
		{
			if (a_objFormat == 1) {
				Put32(at, a_id);
				Put16(at + 4, 0xFFFF);  // alias: none
				Put16(at + 6, 0);
			} else {
				Put16(at, 0);
				Put16(at + 2, 0xFFFF);
				Put32(at + 4, a_id);
			}
		}
		std::uint32_t ReadObject(const std::uint8_t* at, std::int16_t a_objFormat) { return a_objFormat == 1 ? U32(at) : U32(at + 4); }
	}

	Header ReadHeader(const fs::path& a_plugin)
	{
		Header        h;
		std::ifstream f(a_plugin, std::ios::binary);
		std::uint8_t  head[24];
		if (!f.read(reinterpret_cast<char*>(head), 24) || std::memcmp(head, "TES4", 4) != 0) {
			return h;
		}
		h.flags = U32(head + 8);
		std::vector<std::uint8_t> d(U32(head + 4));
		if (!f.read(reinterpret_cast<char*>(d.data()), static_cast<std::streamsize>(d.size()))) {
			return h;
		}
		for (std::size_t p = 0; p + 6 <= d.size();) {
			const auto len = U16(d.data() + p + 4);
			if (std::memcmp(d.data() + p, "MAST", 4) == 0 && len > 0 && p + 6 + len <= d.size()) {
				h.masters.emplace_back(reinterpret_cast<const char*>(d.data() + p + 6), len - 1);
			}
			p += 6 + len;
		}
		h.ok = true;
		return h;
	}

	std::uint32_t GetObjectProperty(const fs::path& a_plugin, std::uint32_t a_recordID, const std::string& a_script, const std::string& a_property)
	{
		try {
			const auto d = ReadAll(a_plugin);
			const auto found = FindRecord(d, a_recordID);
			if (!found) {
				return 0;
			}
			for (const auto& s : Subs(Fields(d, found->at))) {
				if (std::memcmp(s.sig, "VMAD", 4) != 0) {
					continue;
				}
				const auto vm = ParseVmad(s.data);
				for (const auto& sc : vm.scripts) {
					if (Lower(sc.name) != Lower(a_script)) {
						continue;
					}
					for (const auto& pr : sc.props) {
						if (Lower(pr.name) == Lower(a_property) && pr.type == 1) {
							return ReadObject(s.data.data() + pr.valueAt, vm.objFormat);
						}
					}
				}
			}
		} catch (...) {
		}
		return 0;
	}

	Result SetObjectProperty(const fs::path& a_plugin, std::uint32_t a_recordID, const std::string& a_script, const std::string& a_property,
		std::uint32_t a_valueID)
	{
		Result r;
		try {
			auto       d = ReadAll(a_plugin);
			const auto found = FindRecord(d, a_recordID);
			if (!found) {
				r.message = "The record is not in " + a_plugin.filename().string() + ".";
				return r;
			}
			const auto at = found->at;
			const auto oldSize = U32(d.data() + at + 4);
			const auto flags = U32(d.data() + at + 8);
			auto       subs = Subs(Fields(d, at));
			Sub*       vmad = nullptr;
			for (auto& s : subs) {
				if (std::memcmp(s.sig, "VMAD", 4) == 0) {
					vmad = &s;
				}
			}
			if (!vmad) {
				r.message = "That record in " + a_plugin.filename().string() + " has no scripts attached (the script may be on a quest alias or "
															   "another record): set it in the Creation Kit.";
				return r;
			}
			const auto vm = ParseVmad(vmad->data);
			const VmadScript* sc = nullptr;
			for (const auto& s : vm.scripts) {
				if (Lower(s.name) == Lower(a_script)) {
					sc = &s;
				}
			}
			if (!sc) {
				r.message = a_script + " is not attached to that record in " + a_plugin.filename().string() +
							" (it may be on a quest alias or added another way): set it in the Creation Kit.";
				return r;
			}
			const VmadProp* pr = nullptr;
			for (const auto& p : sc->props) {
				if (Lower(p.name) == Lower(a_property)) {
					pr = &p;
				}
			}
			if (pr && pr->type != 1) {
				r.message = a_property + " is not an object property in the plugin.";
				return r;
			}
			if (pr) {
				WriteObject(vmad->data.data() + pr->valueAt, vm.objFormat, a_valueID);  // same size: changed in place
			} else {
				// not filled in the plugin yet: added at the end of the script's properties
				std::vector<std::uint8_t> add;
				const auto                n = static_cast<std::uint16_t>(a_property.size());
				add.push_back(static_cast<std::uint8_t>(n & 0xFF));
				add.push_back(static_cast<std::uint8_t>(n >> 8));
				add.insert(add.end(), a_property.begin(), a_property.end());
				add.push_back(1);  // object
				if (vm.version >= 4) {
					add.push_back(1);  // status: edited
				}
				const auto valueAt = add.size();
				add.resize(add.size() + 8);
				WriteObject(add.data() + valueAt, vm.objFormat, a_valueID);
				vmad->data.insert(vmad->data.begin() + static_cast<std::ptrdiff_t>(sc->endAt), add.begin(), add.end());
				Put16(vmad->data.data() + sc->countAt, static_cast<std::uint16_t>(sc->props.size() + 1));
			}
			// the record again, compressed if it was
			auto fields = Join(subs);
			std::vector<std::uint8_t> payload;
			if (flags & 0x40000) {
				const auto z = CompressZlib(fields);
				payload.resize(4);
				Put32(payload.data(), static_cast<std::uint32_t>(fields.size()));
				payload.insert(payload.end(), z.begin(), z.end());
			} else {
				payload = std::move(fields);
			}
			const auto delta = static_cast<std::int64_t>(payload.size()) - static_cast<std::int64_t>(oldSize);
			std::vector<std::uint8_t> out;
			out.reserve(d.size() + static_cast<std::size_t>(std::max<std::int64_t>(0, delta)));
			out.insert(out.end(), d.begin(), d.begin() + static_cast<std::ptrdiff_t>(at + 24));
			Put32(out.data() + at + 4, static_cast<std::uint32_t>(payload.size()));
			out.insert(out.end(), payload.begin(), payload.end());
			out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(at + 24 + oldSize), d.end());
			for (const auto g : found->groups) {  // every group around it grows or shrinks by the same amount
				Put32(out.data() + g + 4, static_cast<std::uint32_t>(static_cast<std::int64_t>(U32(out.data() + g + 4)) + delta));
			}

			// the original is kept: <plugin>.<date>.resaverpp.bak, then the new file is put in place
			const auto now = std::time(nullptr);
			char       stamp[32];
			std::tm    tm{};
#ifdef _WIN32
			localtime_s(&tm, &now);
#else
			localtime_r(&now, &tm);
#endif
			std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
			std::error_code ec;
			for (int n = 1;; ++n) {  // a name not taken: a backup is never replaced
				r.backup = a_plugin;
				r.backup += std::string(".") + stamp + (n > 1 ? "-" + std::to_string(n) : std::string()) + ".resaverpp.bak";
				if (!fs::exists(r.backup, ec)) {
					break;
				}
			}
			fs::copy_file(a_plugin, r.backup, fs::copy_options::none, ec);
			if (ec) {
				r.message = "Could not back up " + a_plugin.filename().string() + ": " + ec.message() + ". The plugin was not changed.";
				r.backup.clear();
				return r;
			}
			auto tmp = a_plugin;
			tmp += ".resaverpp.tmp";
			{
				std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
				f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
				if (!f) {
					fs::remove(tmp, ec);
					r.message = "Could not write " + tmp.string() + ". The plugin was not changed.";
					return r;
				}
			}
			fs::rename(tmp, a_plugin, ec);
			if (ec) {
				fs::remove(tmp, ec);
				r.message = "Could not replace " + a_plugin.filename().string() + " (is the game, the Creation Kit or xEdit using it?). The plugin was not changed.";
				return r;
			}
			// read back what was written
			if (GetObjectProperty(a_plugin, a_recordID, a_script, a_property) != a_valueID) {
				fs::copy_file(r.backup, a_plugin, fs::copy_options::overwrite_existing, ec);
				r.message = "The change could not be read back from " + a_plugin.filename().string() + "; the original was put back.";
				return r;
			}
			r.ok = true;
			r.message = std::string(pr ? "Changed" : "Added") + " in " + a_plugin.filename().string() + " (original kept as " +
						r.backup.filename().string() + ").";
		} catch (const std::exception& e) {
			r.message = std::string("Could not change the plugin: ") + e.what();
		}
		return r;
	}
}
