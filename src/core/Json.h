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
}
