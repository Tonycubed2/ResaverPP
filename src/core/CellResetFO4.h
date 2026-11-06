// ReSaver++ - Fallout 4: reset an area (a cell), the way the game's own console command ResetInterior (and Papyrus
// Cell.Reset()) does it.
//
// How the game resets a cell (Fallout4.exe 1.11.240, RVAs):
//  - ResetInterior (script command 0x1127, 0x5d11d0) calls 0x4c9330(cell, 0xFFFFFFFF, false): the cell's detach time
//    (ExtraDetachTime, set by 0x285110) becomes 0xFFFFFFFF and the cell is marked changed with flag 0x40000000 (bit 30;
//    an exterior cell also 0x20000000 (bit 29) when its coordinates fit in a signed byte, else 0x10000000 (bit 28)).
//  - The CELL change form keeps it as its initial data (0xbf6c40 -> initial type, 0xbf7090 reads it; table 0x2f31860):
//    bit 30 + bit 29: type 1, 8 bytes {u16 worldspace index, i8 x, i8 y, u32 detach time}; bit 30 + bit 28: type 2,
//    10 bytes {u16 worldspace index, i16 x, i16 y, u32 detach time}; bit 30 alone: type 3, 4 bytes {u32 detach time}.
//    The worldspace index is 1-based into the save's visited worldspace array (the second u32 array read by 0xbeadb0,
//    mapped by 0xbeac20). On load 0xbf77b0 hands the detach time to 0x4c9330(cell, time, true).
//  - TESObjectCELL::LoadGame (0x4d7b30, vtable 0x24a34d8 slot 0x12) then reads: TESForm::LoadGame (bit 0: u32 + u16),
//    bit 1: u16 cell flags, bit 31: the map's seen data (interior: vsval n x {u8, u8, 32 bytes}, 0xc44ad0; exterior:
//    32 bytes, 0xc44940), bit 2: the cell's name (u16 length + text), bit 3: the owner (RefID). It holds no list of the
//    cell's references.
//  - When the cell attaches (0x4d2588) with a detach time of 0xFFFFFFFF, the game resets it: every created (FF)
//    reference in it is deleted (vfunc 0x140) unless it is held by someone (0x4fcc70: extra 0x1C), holds such things
//    (0x4fcc90), is in a quest alias (0x4feb70: extra 0x88) or holds quest items (0x4fcd60); the other references go
//    through the game's own respawn code (the same path as a cell whose respawn time ran out). It does not do this while
//    a save is loading (0x4d2601: the save manager's flag 2 at +0x360, set by the loader 0xbee99b and cleared at its
//    end 0xbef454), so a cell attached during the load (the player's cell and the exterior cells around the player)
//    only resets the next time it attaches. After the reset the detach time becomes the current time (or 0).
//  - Indoors and outdoors alike: the attach code (0x4d45d0 -> 0x4d2570) has no interior test, and the exterior grid code
//    calls it (0x2d06f0: grid place = position >> 12, then 0x4d45d0); 0x4c9330 writes bits 29 / 28 only for an exterior.
//    The Papyrus native Cell.Reset() (0x114b5f0, registered as "Reset" on "Cell" at 0x114bca1) and the encounter zone reset (0x49d830) call
//    the same 0x4c9330(cell, 0xFFFFFFFF, false) for any cell, outdoor ones included (the console needs an editor id, which
//    most outdoor cells lack).
//  - A cell with no detach time (no ExtraDetachTime: the save has no CELL change form for it, or one without bit 30, as a
//    cell whose only change is its flags): 0x4c9330 returns at once (0x4c934d: time -1, not loading, 0x2850e0 -> 0), so the
//    console command does nothing to it, and the attach code neither resets nor respawns it (0x4d2601: detach time 0).
//    The game gives a cell its detach time when the player leaves it (0x4d6e30 -> 0x4d7605: current time, bit 30, and
//    29 / 28 outdoors; skipped only for FF cells, 0x3129c0). So for such a cell the reset writes what the game writes
//    after the player has left it once and ResetInterior / Cell.Reset() ran: bit 30 (+ 29 / 28) and 0xFFFFFFFF.
//  - Same code in 1.10.163 (ResetInterior 0x502b80 -> 0x3b32d0, attach check 0x3bbbae) and 1.10.984 (0x57cf20 -> 0x475190,
//    0x47e458).
//
// So "reset an area" writes exactly what ResetInterior / Cell.Reset() followed by a save would write: the CELL change form
// with bit 30 and a detach time of 0xFFFFFFFF, the initial data put in front of the cell's other saved data when it had
// none (0xbf72f0 writes it: {u32} indoors; {u16 worldspace index, i8 x, i8 y, u32} or {u16, i16, i16, u32} outdoors),
// the form created when the save has none. Whether the cell is indoors comes from the save (initial data) or the plugins
// (CELL DATA bit 0, PluginDB::CellDef: the save's own bit 1 flags only carry bits 0x2264 back, 0x4d7a73, so they do not
// say); an outdoor cell's worldspace and grid place from its saved initial data or the plugins (XCLC, the World Children
// group). A worldspace missing from the save's visited worldspace array is appended (the game's writer 0xbeab40 numbers
// a new one the same way; the loader 0xbeadb0 numbers the entries 1, 2, ...). Everything else in the save stays as it is;
// the game itself decides on the next visit what goes back to the plugins' state.
#pragma once
#include "Save.h"
#include "WorldItemsFO4.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ess::fo4cell
{
	constexpr std::uint32_t kDetachReset = 0xFFFFFFFFu;  // the detach time ResetInterior writes

	struct CellData  // a CELL change form, read as the game reads it
	{
		bool          complete = false;  // read to the exact end of the body
		std::string   error;
		int           initialType = 0;   // 0 none, 1 / 2 exterior, 3 interior (see above)
		std::size_t   initialSize = 0;   // 0, 8, 10 or 4 bytes at the start of the body
		std::uint16_t worldspaceIndex = 0;  // 1-based into visitedWorldspaces
		std::uint32_t worldspace = 0;    // its form id (0 if the index is out of range)
		int           x = 0, y = 0;      // exterior cell coordinates
		std::uint32_t detach = 0;        // detach time (game hours), 0xFFFFFFFF = reset on the next visit
		bool          hasFlags = false;
		std::uint16_t flags = 0;         // bit 1: the cell's flags
		bool          hasSeen = false;   // bit 31: map seen data
		int           interior = -1;     // the seen data was read as an interior's (1) or an exterior's (0)
		std::string   name;              // bit 2
		RefID         owner;             // bit 3
	};
	// a_interior: -1 unknown (taken from the initial data), 0 exterior, 1 interior (only matters for bit 31 without bit 30)
	CellData DecodeCell(const SaveFile& a_save, const ChangeForm& a_cf, int a_interior = -1);

	struct CellInfo
	{
		std::uint32_t formID = 0;
		std::string   name;            // from the plugins when known
		bool          hasChangeForm = false;
		std::uint32_t changeFlags = 0;
		bool          exterior = false;
		bool          interiorKnown = false;  // the save (initial data) or the plugins (DATA) say indoors or outdoors
		bool          inPlugins = false;      // a loaded plugin defines it (PluginDB through fo4world::Lookup::cell)
		std::string   plugin;                 // ... the last one that overrides it
		bool          persistent = false;     // a worldspace's persistent cell (from the plugins)
		bool          hasGrid = false;        // exterior: worldspace and coordinates known (save, plugins, or saved positions)
		std::uint32_t worldspace = 0;  // exterior: the worldspace
		int           x = 0, y = 0;    // exterior: coordinates
		bool          hasDetach = false;
		std::uint32_t detach = 0;
		bool          pending = false;      // a reset is already set (detach time 0xFFFFFFFF)
		std::size_t   createdItems = 0;     // created objects lying in it (fo4world::List)
		std::size_t   createdRemovable = 0; // ... of them, ones the game deletes when it resets the cell
		std::size_t   placed = 0;           // other REFR / ACHR change forms whose saved position is in it
		bool          player = false;       // the player is in it
		bool          nearPlayer = false;   // exterior: in the cells loaded around the player (5 x 5)
		std::string   why;                  // why it cannot be reset ("" = it can)
	};
	// The cells this save knows: every CELL change form, the cells saved positions lie in (outdoor ones through the plugins'
	// grid when no change form names them), and with the plugins the player's cell and the 5 x 5 cells around it.
	// a_lookup.cell (the plugins' CELL records) tells indoors from outdoors and gives an outdoor cell's place when the save
	// does not.
	std::vector<CellInfo> ListCells(const SaveFile& a_save, const fo4world::Lookup& a_lookup = {});

	// Marks the cell for a reset as ResetInterior / Cell.Reset() do (see above). Any cell the save or a loaded plugin knows.
	// Refused (with the reason) for the player's cell, for an exterior cell around the player, for a worldspace's
	// persistent cell, for a form that is not a cell, for a cell not known to be indoors or outdoors, an outdoor one with no
	// known place, and when the cell's change form cannot be read. Returns "" when done; the change form is decoded again
	// before it is kept (nothing in the save changes when it does not read back).
	std::string ResetCell(SaveFile& a_save, std::uint32_t a_cellFormID, const fo4world::Lookup& a_lookup = {});
	// Several cells at once (the save's cells are read once): one result per id, "" for each one done.
	std::vector<std::string> ResetCells(SaveFile& a_save, const std::vector<std::uint32_t>& a_cellFormIDs, const fo4world::Lookup& a_lookup = {});
}
