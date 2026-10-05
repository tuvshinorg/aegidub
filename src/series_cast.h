// Copyright (c) 2026, aegidub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file series_cast.h
/// @brief A series' character to voice assignments, shared by all its episodes
///
/// Kept in a JSON file next to the episodes so that every episode of a series
/// uses the same voice for the same character. Each script remembers the file
/// it uses in its [Script Info]; a script without one uses the file used last.

#pragma once

#include <libaegisub/fs.h>

#include <map>
#include <string>

class AssFile;

namespace series_cast {
	struct Entry {
		std::string voice_id;
		/// Name of the voice, so the file is readable by people
		std::string voice_name;

		bool operator==(Entry const&) const = default;
	};

	/// Character name to voice
	using Cast = std::map<std::string, Entry>;

	/// The series file linked to the script, or failing that the series file
	/// used last if it still exists, or an empty path
	agi::fs::path Find(const AssFile *file, agi::fs::path const& subs_path);

	/// Is the series file the script's own link rather than the last used one?
	bool IsLinked(const AssFile *file);

	/// Link the script to a series file and remember it as the last used.
	/// The caller is responsible for committing.
	void Link(AssFile *file, agi::fs::path const& subs_path, agi::fs::path const& series_path);

	/// A sensible file name for a new series file of this script, e.g.
	/// "Show Name.cast.json" for "Show Name S01 E02-en.ass"
	agi::fs::path SuggestPath(agi::fs::path const& subs_path);

	/// Read a series file; a file that doesn't exist yet is an empty cast
	/// @throws agi::Exception if the file can't be read or isn't a series file
	Cast Load(agi::fs::path const& path);

	/// Write a series file
	/// @throws agi::Exception if the file can't be written
	void Save(agi::fs::path const& path, Cast const& cast);
}
