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

/// @file soft_subs.h
/// @brief Write a copy of a video with the subtitles as a track that can be
/// switched on and off, without re-encoding the picture or sound

#pragma once

#include <libaegisub/fs.h>

#include <functional>
#include <stdexcept>
#include <string>

class AssFile;
namespace agi { struct Context; }

namespace soft_subs {
	struct Error : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	struct Cancelled : Error {
		Cancelled() : Error("Cancelled") { }
	};

	struct Settings {
		agi::fs::path video;
		/// An ASS file holding the subtitles
		agi::fs::path subtitles;
		/// .mkv keeps the ASS styling; .mp4 gets plain mov_text
		agi::fs::path output;
		/// ISO 639-2 code for the track, e.g. "mon" or "eng"
		std::string language = "und";
		/// Keep the video's existing subtitle tracks too (MKV only)
		bool keep_existing = true;
	};

	/// Write the open subtitles to an ASS file for Export, leaving out
	/// comments and aegidub's own data. Call on the GUI thread.
	void WriteSubtitles(agi::Context *c, agi::fs::path const& ass);

	/// @param progress Called with the fraction done, 0 to 1
	/// @throws Cancelled if cancelled, Error on failure
	void Export(Settings const& settings, std::function<void(double)> const& progress,
		std::function<bool()> const& cancelled);
}
