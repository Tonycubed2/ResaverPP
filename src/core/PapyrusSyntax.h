// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Papyrus source (.psc) analysis for the script viewer: syntax colouring, matching block keywords,
// foldable blocks and an outline. The colour classes and keyword lists follow the npp-papyrus
// Notepad++ plugin (blu3mania/npp-papyrus) so scripts look the same as there.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace ess::psc
{
	enum class Kind : std::uint8_t
	{
		kDefault,
		kOperator,
		kFlow,      // if else elseif endif while endwhile
		kType,      // bool float int string var
		kKeyword,   // scriptname extends function property auto ...
		kKeyword2,  // none parent self true false
		kComment,   // ; and ;/ /;
		kDoc,       // { documentation }
		kNumber,
		kString,
		kProperty,
		kClass,     // a script name
		kFunction,
		kCount
	};

	// What the colouriser knows about the script being shown and the installed scripts.
	struct Context
	{
		std::unordered_set<std::string> scripts;     // lowercase script names (classes)
		std::unordered_set<std::string> properties;  // lowercase property names of this script
	};

	// Colours one line (a_line[0..a_len)) into a_out (one Kind per byte). Multi-line ;/ /; comments are
	// handled by the editor; a { doc comment } is coloured to the end of the line when not closed.
	void ColorLine(const char* a_line, std::size_t a_len, Kind* a_out, const Context& a_ctx);

	// A token of a line, without comments and strings: used for blocks and the outline.
	struct Word
	{
		std::string text, lower;
		int         line = 0;
		int         begin = 0, end = 0;  // byte offsets in the line
	};

	struct Block
	{
		enum class Type
		{
			kIf,
			kWhile,
			kFunction,
			kEvent,
			kState,
			kProperty,
			kStruct,
			kGroup
		} type = Type::kIf;
		std::vector<Word> keywords;  // opening keyword, Else/ElseIf, closing keyword (when found)
		int               startLine = 0, endLine = -1;  // endLine -1: not closed
	};

	struct OutlineItem
	{
		enum class Type
		{
			kProperty,
			kFunction,
			kEvent,
			kState,
			kVariable
		} type = Type::kFunction;
		std::string name, detail, state;  // state: the State the function is in ("" = empty state)
		int         line = 0;
		bool        native = false;
	};

	struct Analysis
	{
		std::vector<Block>        blocks;
		std::vector<OutlineItem>  outline;
		std::vector<std::string>  problems;  // unbalanced blocks (shown, not fatal)
		std::string               scriptName, parent;
		std::unordered_set<std::string> properties;  // lowercase
	};

	Analysis Analyze(const std::vector<std::string>& a_lines);

	// The block whose keyword is at (line, byte index), if any (for highlighting its other keywords).
	const Block* BlockAt(const Analysis& a_analysis, int a_line, int a_index);

	// Line of a function/event (case-insensitive) in a state ("" = the empty state, falling back to any
	// state). -1 if not found.
	int FindFunction(const Analysis& a_analysis, const std::string& a_function, const std::string& a_state = {});

	// Splits text into lines (handles \r\n).
	std::vector<std::string> SplitLines(const std::string& a_text);
}
