// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// Decoding of change form bodies (REFR, ACHR, NPC_, QUST, FLST, LVLN/LVLI, RELA), following
// ReSaver's ChangeForm* classes, plus the edits built on it: Reset Havok and Purify FormLists.
#pragma once

#include "Save.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ess::cf
{
	struct Node
	{
		std::string       label;
		std::string       value;      // text for plain values
		bool              isRef = false;
		std::uint32_t     refRaw = 0;  // the 3-byte RefID as stored
		std::uint32_t     formID = 0;  // resolved runtime form ID (0 if none / unknown)
		std::vector<Node> kids;
	};

	struct Decoded
	{
		bool        supported = false;  // the type has a decoder
		bool        complete = false;   // the whole body was decoded with nothing left over
		std::string error;              // why decoding stopped (if it did)
		std::size_t bodySize = 0, consumed = 0;
		Node        root;

		bool          hasHavok = false;
		std::size_t   havokOffset = 0, havokSize = 0;  // in the uncompressed body
		std::uint32_t baseObject = 0;                  // runtime form ID, when the body names one

		// ReSaver filter facts
		bool          hasFormFlags = false;
		std::uint32_t formFlags = 0;      // CHANGE_FORM_FLAGS block, when present
		bool          isEmptyRef = false;  // "empty REFR": no change flags 0-7 and nothing but empty initial data
		bool          inCellMinus1 = false;  // moved to the "-1" cell ("cell(-1) change forms")

		// form lists
		bool                       isFormList = false;
		std::vector<std::uint32_t> listRaw;  // the entries' RefIDs (raw 3-byte values)
	};

	// Optional: record type of a form ("EXPL", ...), from the plugin database. Needed only to
	// decode the explosion data of spawned explosion references.
	using TypeLookup = std::function<std::string(std::uint32_t)>;

	std::vector<std::uint8_t> Body(const ChangeForm& a_cf);                        // decompressed
	void                      SetBody(ChangeForm& a_cf, const std::vector<std::uint8_t>& a_body);  // recompresses if it was compressed
	Decoded                   Decode(const SaveFile& a_save, const ChangeForm& a_cf, const TypeLookup& a_types = {});
	std::vector<std::string>  FlagNames(std::uint8_t a_type, std::uint32_t a_flags);

	// ReSaver "Reset Havok": zeroes the saved physics state of every object reference whose body
	// decodes completely. Returns {reset, could not be decoded}.
	struct HavokResult
	{
		std::size_t reset = 0, failed = 0;
	};
	HavokResult ResetHavok(SaveFile& a_save, const TypeLookup& a_types = {}, bool a_dryRun = false);

	// ReSaver "Purify FormLists": removes null entries from form lists.
	struct PurifyResult
	{
		std::size_t entries = 0, forms = 0, failed = 0;
	};
	PurifyResult PurifyFormLists(SaveFile& a_save, bool a_dryRun = false, std::uint32_t a_onlyRefID = 0);  // 0: every list

	// ReSaver's change form content filter: terms separated by ";" must all match, alternatives
	// separated by "," need one match, "!" negates. A term is a path of field names separated by
	// "/" ending in a value, e.g. "EXTRADATA/Ownership" or "INITIAL/CELL/0000003C" or
	// "INVENTORY/ITEM/0001F4" (hex form IDs match as prefixes). Field names ignore case and counts.
	bool ContentMatches(const Node& a_root, const std::string& a_expression);
	std::size_t  NullEntries(const SaveFile& a_save, const ChangeForm& a_cf);  // null entries in a form list (0 if none / not a list)
}
