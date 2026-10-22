// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// "Compare To": what changed between two saves (ReSaver only says "match" or the first
// mismatch; this lists the differences that matter when hunting a problem).
#pragma once

#include "Save.h"

#include <string>
#include <vector>

namespace ess::compare
{
	struct Delta
	{
		std::string name;
		long long   a = 0, b = 0;  // value in save A and save B
		long long   diff() const { return b - a; }
	};

	struct Result
	{
		bool                     identical = false;  // same body bytes
		std::vector<std::string> header;             // character / location / date lines
		std::vector<std::string> pluginsAdded, pluginsRemoved;
		std::vector<Delta>       totals;          // instances, threads, ...
		std::vector<Delta>       instancesByScript;  // biggest changes first
		std::vector<Delta>       threadsByScript;
		std::vector<Delta>       changeFormsByType;
		std::vector<std::string> text() const;    // plain-language listing
	};

	Result Compare(const SaveFile& a_a, const SaveFile& a_b, std::size_t a_top = 40);
}
