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

/// @file hardsub_extractor.h
/// @brief Read subtitles burned into a video's picture with OCR
///
/// Uses video-subtitle-extractor (https://github.com/YaoFANGUK/video-subtitle-extractor,
/// Apache License 2.0), which is downloaded from GitHub into its own Python
/// environment on first use. Nothing of it is built into aegidub.

#pragma once

#include <libaegisub/fs.h>

#include <functional>
#include <stdexcept>
#include <string>

class AssFile;
namespace agi { struct Context; }

namespace hardsub_extractor {
	struct Error : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	struct Cancelled : Error {
		Cancelled() : Error("Cancelled") { }
	};

	/// @param message  What is happening now
	/// @param fraction Progress of the current step, 0 to 1, or negative if unknown
	using ProgressFn = std::function<void(std::string const& message, double fraction)>;

	struct Settings {
		agi::fs::path video;
		/// Where to write the SRT
		agi::fs::path output;
		/// A video-subtitle-extractor language code, e.g. "en" or "ch"
		std::string language;
		bool gpu = true;
		/// The area holding the subtitles, as fractions of the frame size
		double top = 0, bottom = 1, left = 0, right = 1;
	};

	/// Is video-subtitle-extractor downloaded and installed already?
	bool Installed();

	/// Extract the subtitles in an area of a video to an SRT file, first
	/// downloading and installing video-subtitle-extractor if needed
	/// @throws Cancelled if cancelled, Error on failure
	void Extract(Settings const& settings, ProgressFn const& progress, std::function<void(std::string const&)> const& log,
		std::function<bool()> const& cancelled);

	/// Read an SRT written by Extract
	/// @throws Error if it can't be read
	void ReadLines(agi::Context *c, agi::fs::path const& srt, AssFile& lines);

	/// Put extracted lines into the open subtitles, replacing what is there
	/// or adding after it, and select the first. Call on the GUI thread.
	/// @return How many lines were added
	size_t ImportLines(agi::Context *c, AssFile& lines, bool replace);

	/// Are there any non-empty lines in the open subtitles?
	bool HasLines(agi::Context *c);
}
