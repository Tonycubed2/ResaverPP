// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// "Batch Clean": purge a list of scripts in one go. Same list format as ReSaver, one script per
// line:
//     ScriptName            purge it
//     ScriptName.pex        same
//     Prefix*               every script whose name starts with Prefix
//     ScriptName @@ text    ask before purging (text is shown as the reason)
//     ; comment / # comment ignored
#pragma once

#include "Save.h"

#include <string>
#include <vector>

namespace ess::batch
{
	struct Match
	{
		std::string script;      // script name as it appears in the save
		std::string fromLine;    // the list line that matched it
		std::string prompt;      // "@@" text (empty: no confirmation asked)
		std::size_t instances = 0, threads = 0;
		bool        selected = true;  // prompted entries start unselected
	};

	std::vector<Match> Resolve(const SaveFile& a_save, const std::string& a_list);

	struct Result
	{
		std::size_t scripts = 0, threads = 0, stacks = 0, messages = 0, instances = 0;
	};
	// Purges every selected match (threads, suspended stacks, queued calls, instances).
	Result Apply(SaveFile& a_save, const std::vector<Match>& a_matches);
}
