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

/// @file dub_render.h
/// @brief Mix the dub over a video's own background sound and write the result
///
/// Uses the FFmpeg libraries built into the program: nothing external is run.

#pragma once

#include <libaegisub/fs.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dub_render {
	struct Error : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	struct Cancelled : Error {
		Cancelled() : Error("Cancelled") { }
	};

	/// Called with the fraction done, 0 to 1
	using ProgressFn = std::function<void(double)>;

	/// Decode the video's main audio track to a 44.1 kHz 16-bit stereo WAV
	void ExtractAudio(agi::fs::path const& video, agi::fs::path const& wav,
		ProgressFn const& progress, std::function<bool()> const& cancelled);

	struct Settings {
		agi::fs::path video;
		/// Everything but the voices, 44.1 kHz 16-bit stereo
		agi::fs::path background;
		/// The dub track, any 16-bit PCM WAV
		agi::fs::path dub;
		/// The original voices, 44.1 kHz 16-bit stereo; used when original_volume > 0
		agi::fs::path original_voices;
		/// Level of the original voices under the dub, 0 to leave them out
		double original_volume = 0;
		/// Even out the dub's level, duck the background under it and
		/// normalise the loudness of the result
		bool remaster = true;
		/// Also keep the video's own audio as a second, non-default track
		bool keep_original_track = true;
		/// .mkv or .mp4
		agi::fs::path output;
	};

	/// Write the video with the dubbed sound. The video is copied, not re-encoded.
	void Render(Settings const& settings, ProgressFn const& progress, std::function<bool()> const& cancelled);

	/// Speed mono 16-bit speech up (factor > 1) or down without changing its pitch
	std::vector<int16_t> ChangeTempo(std::vector<int16_t> const& samples, int rate, double factor);
}
