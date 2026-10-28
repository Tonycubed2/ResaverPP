// ReSaver++ - C++ port of ReSaver (FallrimTools) by Mark Fairchild, Apache License 2.0.
// The save archive: before ReSaver Plus writes a save, the save that was opened (with its .skse co-save)
// is copied into an archive folder, so the last saves it edited can always be put back. The newest
// kKeep entries are kept; the oldest is deleted when an 11th is added.
// Each entry is a folder named by the time it was made ("20261028-163105-123"), holding the .ess, the
// .skse (if there was one) and info.txt (where it came from, when, what was written instead).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ess::archive
{
	inline constexpr std::size_t kKeep = 10;

	struct Entry
	{
		std::filesystem::path folder;    // the entry's folder in the archive
		std::filesystem::path ess, skse;  // the archived files (skse empty if the save had none)
		std::filesystem::path source;    // where the save was when it was archived
		std::filesystem::path savedAs;   // the file ReSaver Plus wrote (empty if not known)
		std::string           reason;    // "Saved over it", "Saved as another file", "Replaced by a restore"
		std::int64_t          archived = 0;  // seconds since 1970
		std::int64_t          modified = 0;  // the save file's own time when it was archived
		std::uintmax_t        size = 0, cosaveSize = 0;
		// from the save's header
		std::string   character, location, race;
		std::uint32_t level = 0, saveNumber = 0;
	};

	// Copies a_ess (and its .skse) into the archive. Nothing is copied when the newest entry is already
	// this file, unchanged. With a_prune, entries beyond kKeep are deleted, oldest first.
	// Returns the new entry, or nothing if it was already archived. Throws std::runtime_error on failure.
	std::optional<Entry> Add(const std::filesystem::path& a_archive, const std::filesystem::path& a_ess,
		const std::filesystem::path& a_savedAs, const std::string& a_reason, bool a_prune = true);

	// Deletes entries beyond kKeep, oldest first.
	void Prune(const std::filesystem::path& a_archive);

	// Every entry, newest first.
	std::vector<Entry> List(const std::filesystem::path& a_archive);

	// Puts an entry back where it came from. The save there now (if any) is archived first, so the restore can
	// be undone the same way. Returns the restored .ess.
	std::filesystem::path RestoreInPlace(const std::filesystem::path& a_archive, const Entry& a_entry);

	// Copies an entry next to where it came from under a new name the game reads (…restored…), leaving the save
	// there alone. Returns the new .ess.
	std::filesystem::path RestoreAsCopy(const Entry& a_entry);
}
