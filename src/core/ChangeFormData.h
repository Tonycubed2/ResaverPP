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
		std::size_t offset = 0, size = 0;  // byte range in the decompressed body
	};
	struct ExtraTrace
	{
		std::uint8_t type = 0;
		std::size_t offset = 0;
		int predecessor = -1;  // previous type in read order, including nested elements; -1 for the first
	};

	// An inventory entry that carries an enchantment and/or a charge in its extra data (weapons and
	// staves enchanted at a table or by a script, or partly used). Offsets are in the uncompressed body.
	struct InvCharge
	{
		std::uint32_t item = 0;     // runtime form ID of the base item
		std::int32_t  count = 0;
		bool          hasEnchant = false;
		std::uint32_t enchRaw = 0;  // ENCH RefID as stored (0: none)
		std::uint32_t ench = 0;     // runtime form ID (0 if it does not resolve)
		std::uint16_t maxCharge = 0;
		std::size_t   maxChargeOffset = 0;
		bool          hasCharge = false;
		float         charge = 0;
		std::size_t   chargeOffset = 0;
	};

	struct Decoded
	{
		bool        supported = false;  // the type has a decoder
		bool        complete = false;   // the whole body was decoded with nothing left over
		std::string error;              // why decoding stopped (if it did)
		std::size_t bodySize = 0, consumed = 0;
		Node        root;
		// Best-effort classification of the existing error and deepest failed group path.
		// A partial or unsupported decode is not evidence of save corruption.
		std::string failureCategory, failurePath;
		std::vector<ExtraTrace> extraTrace;  // stops at the first failed element

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

		// inventory entries with an enchantment or charge (REFR containers, ACHR actors incl. the player)
		std::vector<InvCharge> charged;
	};

	// Every inventory entry with an enchantment or charge in the save, with the change form holding it.
	struct ChargedItem
	{
		std::uint32_t holderRefID = 0;  // the change form's RefID (raw)
		std::uint32_t holder = 0;       // its runtime form ID
		InvCharge     inv;
	};
	std::vector<ChargedItem> FindChargedItems(const SaveFile& a_save);

	// Fixes enchanted inventory items that cannot be recharged: max charge 0 (no "EAMT") is set to
	// a_maxCharge, and a current charge above the max is lowered to it. Only fixed-size values are
	// rewritten in place, nothing is added or removed. a_only: change form RefIDs (raw) to touch (empty:
	// all) and item form IDs (empty: all). Returns how many items were fixed.
	std::size_t FixCharges(SaveFile& a_save, std::uint16_t a_maxCharge, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& a_only);
	bool        NeedsChargeFix(const InvCharge& a_item);  // has an enchantment with max charge 0, or charge > max

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
