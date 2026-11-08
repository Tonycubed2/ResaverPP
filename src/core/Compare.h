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
		// change forms both saves have (same RefID and type) whose saved data differs: an edit that keeps the count the
		// same (e.g. an active effect taken off the player) shows only here
		struct Edited
		{
			std::uint32_t formID = 0;
			std::string   type;
			std::uint32_t flagsA = 0, flagsB = 0;
			std::size_t   sizeA = 0, sizeB = 0;  // data as stored
			std::string   note;                   // what changed inside, when known (e.g. "active effects 165 -> 164")
		};
		std::size_t              editedCount = 0;  // all of them; `edited` holds the first a_top by size change
		std::vector<Edited>      edited;
		std::vector<std::string> text() const;    // plain-language listing
	};

	Result Compare(const SaveFile& a_a, const SaveFile& a_b, std::size_t a_top = 40);
}
