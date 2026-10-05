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

/// @file project_store.h
/// @brief Dubbing projects: one folder per video, named by date and hash
///
/// A project folder such as "20261005-a1b2c3d4e5f6" holds project.json, the
/// subtitles (<id>.ass), the generated speech (<id>_dub) and a thumbnail.
/// The video itself stays where it is and is only referred to.

#pragma once

#include <libaegisub/fs.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace project_store {
	struct Project {
		/// e.g. "20261005-a1b2c3d4e5f6"
		std::string id;
		/// The video's file name, shown to people
		std::string name;
		agi::fs::path folder;
		agi::fs::path video;
		/// When the project was last worked on, as a Unix time
		int64_t modified = 0;

		agi::fs::path Subtitles() const;
		agi::fs::path Thumbnail() const;
	};

	/// The ID of a project for a video started on the given day: the date and
	/// the first 12 hex digits of the SHA-256 of the video's file name
	std::string MakeId(agi::fs::path const& video, std::time_t day);

	/// Projects in a folder, the most recently worked on first
	std::vector<Project> List(agi::fs::path const& root);

	/// The project a subtitle file belongs to, if it is a project's file
	bool FindForSubtitles(agi::fs::path const& subtitles, Project& project);

	/// Write project.json for a new project
	/// @throws agi::Exception on failure
	void Write(Project const& project);

	/// Save a picture from about a fifth of the way into the video, scaled to
	/// the given width, as a PNG
	/// @return false if no picture could be made
	bool MakeThumbnail(agi::fs::path const& video, agi::fs::path const& png, int width);
}
