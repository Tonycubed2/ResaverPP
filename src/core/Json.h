// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// A small JSON reader (enough for Vortex's deployment manifest).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ess::json
{
	struct Value
	{
		enum class Type
		{
			kNull,
			kBool,
			kNumber,
			kString,
			kArray,
			kObject
		} type = Type::kNull;
		bool                         b = false;
		double                       n = 0;
		std::string                  s;
		std::vector<Value>           a;
		std::map<std::string, Value> o;

		const Value* get(const std::string& a_key) const;  // object member or nullptr
		std::string  str(const std::string& a_key) const;  // string member or ""
	};

	// Throws std::runtime_error on malformed input.
	Value Parse(const std::string& a_text);

	// Writing (the AI chat's requests). Text that is not valid UTF-8 (a log written in Windows-1252) is
	// read as Windows-1252 / Latin-1 bytes, so the result is always valid UTF-8 JSON.
	std::string Quote(const std::string& a_text);  // "a JSON string, with quotes"
	std::string Write(const Value& a_value);       // compact; object keys in sorted order
	std::string ValidUtf8(const std::string& a_text);
	Value       Str(std::string a_s);
	Value       Num(double a_n);
	Value       Bool(bool a_b);
	Value       Obj();
	Value       Arr();
}
