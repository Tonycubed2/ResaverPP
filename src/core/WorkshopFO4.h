// ReSaver++ - Fallout 4 settlements (workshops): the build budget, population and happiness the save keeps on each
// workshop workbench, a reset of the used budget, and the settlement checks of the Save check.
//
// Where the data lives (Fallout4.exe 1.11.240, RVAs; checked on real saves):
// - A workshop is the workbench reference that carries WorkshopScript (or a script extending it, e.g.
//   DLC06:VaultWorkshopScript, DLC04:DLC04RedRocketWorkshopScript); its Papyrus instance has myLocation (the settlement's
//   location) and WorkshopID (its index in WorkshopParentScript.Workshops). The workbench's base object has the keyword
//   WorkshopKeyword (Fallout4.esm 0x54BA7: WorkshopWorkbench 0xC1AEB and its variants).
// - Its values are actor values of the REFERENCE, saved as extra data 0xC2 (ExtraActorValueStorage, ctor 0x29c750; loader
//   case 0x2980e3 -> 0x2b2d30 -> ActorValueStorage::LoadGame 0xcedc80): vsval n x {ref AVIF, f32 base}, vsval m x {ref AVIF,
//   f32 x 3 modifiers}. A reference's GetActorValue (TESObjectREFR's ActorValueOwner vfunc 1, 0x507ed0) is the base (the
//   stored base, else the AVIF's default) plus the base object's PRPS value for it (0x508010) plus the three modifiers.
// - The budget values are the hard-coded actor values WorkshopCurrentTriangles 0x348, WorkshopMaxTriangles 0x349,
//   WorkshopCurrentDraws 0x34A, WorkshopMaxDraws 0x34B (ActorValue::ActorValue 0x6fc62b..0x6fc6f4: indices 0x8c..0x8f,
//   form id = 0x2BC + index (0x6fd272), default 0 (+0x1c4 = 0, 0x6fd2ac)). No PRPS on any workbench of Fallout4.esm or
//   the DLC plugins names them.
// - Workshop mode (0x390330 / 0x396558) reads them once: max = (int)value, or uWorkshopBudgetDrawCount:Workshop (10000) /
//   uWorkshopBudgetTriangleCount:Workshop (10000000) when it is 0; current = (int)value. Placing an object adds its draws
//   and triangles (0x390426) and writes them back as the base (vfunc 9 -> SetBaseActorValue 0x5080d0, which also sets
//   change flag 31); scrapping or storing subtracts them, never below 0 (0x3905e6). The game never counts the objects
//   again: these totals are all it knows.
// - The build meter (0x37d6c0) is max(current draws / max draws, current triangles / max triangles), at most 1.0; the
//   workshop compares the unclamped value with 1.0 (0x390770, 0xb2e994).
// - Workshop objects and settlers are linked to their workshop by ExtraLinkedRef (extra 0x50, loader 0x2a5910: vsval n x
//   {ref keyword, ref target}) with keyword WorkshopItemKeyword (Fallout4.esm 0x54BA6); WorkshopParentScript sets that link
//   and the settler's WorkshopNPCScript.workshopID together (AddActorToWorkshop) and clears both together (UnassignActor).
#pragma once
#include "ChangeFormFO4.h"
#include "Save.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ess::fo4ws
{
	constexpr std::uint32_t kAvCurrentTriangles = 0x348;
	constexpr std::uint32_t kAvMaxTriangles = 0x349;
	constexpr std::uint32_t kAvCurrentDraws = 0x34A;
	constexpr std::uint32_t kAvMaxDraws = 0x34B;
	constexpr std::uint32_t kAvPopulation = 0x12723E;  // WorkshopRatingPopulation (Fallout4.esm)
	constexpr std::uint32_t kAvHappiness = 0x129157;   // WorkshopRatingHappiness (Fallout4.esm)
	constexpr std::uint32_t kDefaultMaxDraws = 10000;          // uWorkshopBudgetDrawCount:Workshop (0x2edf5c8)
	constexpr std::uint32_t kDefaultMaxTriangles = 10000000;   // uWorkshopBudgetTriangleCount:Workshop (0x2edf5b0)
	constexpr std::uint32_t kWorkshopItemKeyword = 0x54BA6;    // Fallout4.esm
	constexpr std::uint32_t kWorkshopKeyword = 0x54BA7;        // Fallout4.esm
	constexpr std::uint8_t  kExtraActorValues = 0xC2;
	constexpr std::uint8_t  kExtraLinkedRef = 0x50;
	constexpr std::uint8_t  kExtraWorkshop = 0xBF;

	struct AvBase
	{
		RefID         av;
		std::uint32_t formID = 0;
		float         base = 0;
		fo4cf::Range  range;  // ref + f32
	};
	struct AvMod
	{
		RefID         av;
		std::uint32_t formID = 0;
		float         mod[3] = {};
		fo4cf::Range  range;  // ref + 3 x f32
	};
	// Extra 0xC2 of a reference
	struct AvStorage
	{
		bool                present = false;
		fo4cf::Range        range;  // the whole extra entry (type byte included)
		std::vector<AvBase> bases;
		std::vector<AvMod>  mods;
		bool                hasBase(std::uint32_t a_av) const;
		float               base(std::uint32_t a_av) const;      // the stored base, else 0 (the default of these values)
		float               modifiers(std::uint32_t a_av) const;  // the three modifiers added up
		float               value(std::uint32_t a_av) const { return base(a_av) + modifiers(a_av); }
	};
	// Reads extra 0xC2 from a decoded reference; nullopt when the entry cannot be read
	std::optional<AvStorage> ReadStorage(const std::vector<std::uint8_t>& a_body, const fo4cf::Refr& a_refr, const SaveFile& a_save);

	struct Settlement
	{
		std::uint32_t formID = 0;          // the workbench reference
		std::string   script;              // WorkshopScript or the script extending it
		std::uint32_t location = 0;        // WorkshopScript.myLocation (0: none)
		std::int32_t  workshopID = -1;     // WorkshopScript.WorkshopID (index in WorkshopParentScript.Workshops)
		bool          ownedByPlayer = false;
		bool          hasChangeForm = false;
		bool          readable = false;    // the reference reads completely (so it can be edited)
		std::string   error;
		bool          hasStorage = false;  // extra 0xC2 saved (else every value is 0)
		bool          hasWorkshopExtra = false;  // extra 0xBF (ExtraWorkshop)
		float         currentDraws = 0, currentTriangles = 0, maxDraws = 0, maxTriangles = 0, population = 0, happiness = 0;
		std::size_t   linkedObjects = 0, linkedActors = 0;  // references / actors in the save linked to it (WorkshopItemKeyword)

		// as workshop mode takes them (0x390330): an int, a max of 0 means the game's default
		std::int64_t usedDraws() const { return static_cast<std::int64_t>(currentDraws); }
		std::int64_t usedTriangles() const { return static_cast<std::int64_t>(currentTriangles); }
		std::int64_t limitDraws() const { const auto v = static_cast<std::int64_t>(maxDraws); return v ? v : kDefaultMaxDraws; }
		std::int64_t limitTriangles() const { const auto v = static_cast<std::int64_t>(maxTriangles); return v ? v : kDefaultMaxTriangles; }
		// the build meter before clamping (0x390770); 1.0 = full
		double meter() const;
		bool   overBudget() const { return meter() >= 1.0; }
	};
	// Every workshop of the save, in the order of their Papyrus instances.
	std::vector<Settlement> List(const SaveFile& a_save);

	// Sets the used budget (the bases of WorkshopCurrentDraws / WorkshopCurrentTriangles) of one workshop, so that the
	// game's value (base + modifiers) becomes the given number; nullopt leaves that one alone. The reference must read
	// completely; its extra 0xC2 is rebuilt (an entry is added when the value has none) and the body is decoded again
	// before it is kept. A workshop without extra 0xC2 already has 0 used: then only 0 is accepted (nothing to do).
	// Returns "" or why not (then the save is unchanged). a_changed: whether anything was written.
	std::string SetUsed(SaveFile& a_save, std::uint32_t a_workshop, std::optional<float> a_draws, std::optional<float> a_triangles,
		bool* a_changed = nullptr);

	// ---- Save check ----
	struct Finding
	{
		enum class Kind
		{
			kOverBudget,       // the build meter is full
			kMissingWorkshop,  // a WorkshopItemKeyword link names a reference that is not in the game
			kSettlerBadID,     // a settler's workshopID does not name a workshop in WorkshopParentScript.Workshops
			kSettlerMismatch,  // a settler's workshopID and its workshop link name different workshops
		};
		Kind          kind = Kind::kOverBudget;
		std::uint32_t form = 0;    // the workshop (over budget) / the linked reference or settler
		std::uint32_t target = 0;  // the missing / other workshop
		std::int32_t  id = -1;     // the settler's workshopID
		std::string   text;        // one line, English
	};
	std::vector<Finding> Check(const SaveFile& a_save);
}
