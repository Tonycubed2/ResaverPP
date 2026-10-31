// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
#include "Json.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace ess::json
{
	const Value* Value::get(const std::string& a_key) const
	{
		if (type != Type::kObject) {
			return nullptr;
		}
		auto it = o.find(a_key);
		return it == o.end() ? nullptr : &it->second;
	}

	std::string Value::str(const std::string& a_key) const
	{
		const auto* v = get(a_key);
		return v && v->type == Type::kString ? v->s : std::string();
	}

	namespace
	{
		class Parser
		{
		public:
			explicit Parser(const std::string& t) :
				_t(t)
			{
				// a UTF-8 byte order mark
				if (_t.size() >= 3 && static_cast<unsigned char>(_t[0]) == 0xEF && static_cast<unsigned char>(_t[1]) == 0xBB &&
					static_cast<unsigned char>(_t[2]) == 0xBF) {
					_i = 3;
				}
			}

			Value parse()
			{
				Value v = value(0);
				ws();
				if (_i != _t.size()) {
					fail("text after the end");
				}
				return v;
			}

		private:
			const std::string& _t;
			std::size_t        _i = 0;

			[[noreturn]] void fail(const char* what) const { throw std::runtime_error(std::string("JSON: ") + what + " at offset " + std::to_string(_i)); }

			void ws()
			{
				while (_i < _t.size() && (_t[_i] == ' ' || _t[_i] == '\t' || _t[_i] == '\r' || _t[_i] == '\n')) {
					++_i;
				}
			}

			bool lit(const char* w)
			{
				std::size_t k = 0;
				while (w[k]) {
					if (_i + k >= _t.size() || _t[_i + k] != w[k]) {
						return false;
					}
					++k;
				}
				_i += k;
				return true;
			}

			static void put(std::string& out, unsigned cp)
			{
				if (cp < 0x80) {
					out.push_back(static_cast<char>(cp));
				} else if (cp < 0x800) {
					out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				} else if (cp < 0x10000) {
					out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				} else {
					out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				}
			}

			unsigned hex4()
			{
				if (_i + 4 > _t.size()) {
					fail("short \\u escape");
				}
				const unsigned v = static_cast<unsigned>(std::strtoul(_t.substr(_i, 4).c_str(), nullptr, 16));
				_i += 4;
				return v;
			}

			std::string string()
			{
				++_i;  // opening quote
				std::string out;
				while (true) {
					if (_i >= _t.size()) {
						fail("unterminated string");
					}
					const char c = _t[_i++];
					if (c == '"') {
						return out;
					}
					if (c != '\\') {
						out.push_back(c);
						continue;
					}
					if (_i >= _t.size()) {
						fail("bad escape");
					}
					const char e = _t[_i++];
					switch (e) {
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					case '/': out.push_back('/'); break;
					case 'b': out.push_back('\b'); break;
					case 'f': out.push_back('\f'); break;
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case 'u':
						{
							unsigned cp = hex4();
							if (cp >= 0xD800 && cp < 0xDC00 && _i + 6 <= _t.size() && _t[_i] == '\\' && _t[_i + 1] == 'u') {
								_i += 2;
								const unsigned lo = hex4();
								cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
							}
							put(out, cp);
							break;
						}
					default: fail("bad escape");
					}
				}
			}

			Value value(int depth)
			{
				if (depth > 200) {
					fail("nested too deeply");
				}
				ws();
				if (_i >= _t.size()) {
					fail("unexpected end");
				}
				Value      v;
				const char c = _t[_i];
				if (c == '{') {
					v.type = Value::Type::kObject;
					++_i;
					ws();
					if (_i < _t.size() && _t[_i] == '}') {
						++_i;
						return v;
					}
					while (true) {
						ws();
						if (_i >= _t.size() || _t[_i] != '"') {
							fail("expected a key");
						}
						std::string key = string();
						ws();
						if (_i >= _t.size() || _t[_i] != ':') {
							fail("expected ':'");
						}
						++_i;
						v.o[std::move(key)] = value(depth + 1);
						ws();
						if (_i < _t.size() && _t[_i] == ',') {
							++_i;
							continue;
						}
						if (_i < _t.size() && _t[_i] == '}') {
							++_i;
							return v;
						}
						fail("expected ',' or '}'");
					}
				}
				if (c == '[') {
					v.type = Value::Type::kArray;
					++_i;
					ws();
					if (_i < _t.size() && _t[_i] == ']') {
						++_i;
						return v;
					}
					while (true) {
						v.a.push_back(value(depth + 1));
						ws();
						if (_i < _t.size() && _t[_i] == ',') {
							++_i;
							continue;
						}
						if (_i < _t.size() && _t[_i] == ']') {
							++_i;
							return v;
						}
						fail("expected ',' or ']'");
					}
				}
				if (c == '"') {
					v.type = Value::Type::kString;
					v.s = string();
					return v;
				}
				if (lit("true")) {
					v.type = Value::Type::kBool;
					v.b = true;
					return v;
				}
				if (lit("false")) {
					v.type = Value::Type::kBool;
					return v;
				}
				if (lit("null")) {
					return v;
				}
				char*       end = nullptr;
				const char* start = _t.c_str() + _i;
				v.n = std::strtod(start, &end);
				if (end == start) {
					fail("unexpected character");
				}
				v.type = Value::Type::kNumber;
				_i += static_cast<std::size_t>(end - start);
				return v;
			}
		};
	}

	Value Parse(const std::string& a_text) { return Parser(a_text).parse(); }

	std::string ValidUtf8(const std::string& a_text)
	{
		std::string out;
		out.reserve(a_text.size());
		const auto* p = reinterpret_cast<const unsigned char*>(a_text.data());
		const std::size_t n = a_text.size();
		for (std::size_t i = 0; i < n;) {
			const unsigned char c = p[i];
			std::size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
			bool ok = len > 0 && i + len <= n && !(len == 2 && c < 0xC2);
			for (std::size_t k = 1; ok && k < len; ++k) {
				ok = (p[i + k] & 0xC0) == 0x80;
			}
			if (ok) {
				out.append(a_text, i, len);
				i += len;
				continue;
			}
			// a lone byte: Latin-1 / Windows-1252 as its code point (U+0080..U+00FF)
			out.push_back(static_cast<char>(0xC0 | (c >> 6)));
			out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
			++i;
		}
		return out;
	}

	std::string Quote(const std::string& a_text)
	{
		const auto  t = ValidUtf8(a_text);
		std::string o = "\"";
		o.reserve(t.size() + 2);
		for (char ch : t) {
			const auto c = static_cast<unsigned char>(ch);
			switch (ch) {
			case '"': o += "\\\""; break;
			case '\\': o += "\\\\"; break;
			case '\n': o += "\\n"; break;
			case '\r': o += "\\r"; break;
			case '\t': o += "\\t"; break;
			default:
				if (c < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					o += buf;
				} else {
					o.push_back(ch);
				}
			}
		}
		return o + "\"";
	}

	std::string Write(const Value& a_value)
	{
		switch (a_value.type) {
		case Value::Type::kNull: return "null";
		case Value::Type::kBool: return a_value.b ? "true" : "false";
		case Value::Type::kNumber:
			{
				char buf[64];
				if (a_value.n == static_cast<double>(static_cast<long long>(a_value.n)) && a_value.n < 1e15 && a_value.n > -1e15) {
					std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(a_value.n));
				} else {
					std::snprintf(buf, sizeof(buf), "%.17g", a_value.n);
				}
				return buf;
			}
		case Value::Type::kString: return Quote(a_value.s);
		case Value::Type::kArray:
			{
				std::string o = "[";
				for (std::size_t i = 0; i < a_value.a.size(); ++i) {
					o += (i ? "," : "") + Write(a_value.a[i]);
				}
				return o + "]";
			}
		case Value::Type::kObject:
			{
				std::string o = "{";
				bool        first = true;
				for (const auto& [k, v] : a_value.o) {
					o += (first ? "" : ",") + Quote(k) + ":" + Write(v);
					first = false;
				}
				return o + "}";
			}
		}
		return "null";
	}

	Value Str(std::string a_s)
	{
		Value v;
		v.type = Value::Type::kString;
		v.s = std::move(a_s);
		return v;
	}
	Value Num(double a_n)
	{
		Value v;
		v.type = Value::Type::kNumber;
		v.n = a_n;
		return v;
	}
	Value Bool(bool a_b)
	{
		Value v;
		v.type = Value::Type::kBool;
		v.b = a_b;
		return v;
	}
	Value Obj()
	{
		Value v;
		v.type = Value::Type::kObject;
		return v;
	}
	Value Arr()
	{
		Value v;
		v.type = Value::Type::kArray;
		return v;
	}
}
