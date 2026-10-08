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

/// @file speaker_diarization.h
/// @brief Find who speaks when in a soundtrack with pyannote
///
/// pyannote runs in a Python environment of its own, which is set up on
/// first use. The installer includes the model (CC-BY-4.0, from pyannote);
/// without it, the model is downloaded from Hugging Face, where it is gated,
/// so that needs a token from an account that has accepted its terms.

#pragma once

#include <libaegisub/fs.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace speaker_diarization {
	struct Error : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	struct Cancelled : Error {
		Cancelled() : Error("Cancelled") { }
	};

	/// The token is missing or wrong, or its account hasn't accepted the
	/// model's terms
	struct NeedsAccess : Error {
		using Error::Error;
	};

	/// The Hugging Face page whose terms must be accepted
	extern const char *model_page;

	/// Is the model installed with aegidub, so that no token is needed?
	bool Bundled();

	/// One stretch of speech by one voice
	struct Turn {
		int start_ms;
		int end_ms;
		/// "V1", "V2"... in order of first appearance
		std::string voice;
	};

	/// @param message  What is happening now
	/// @param fraction Progress of this step, 0 to 1, or negative if unknown
	using ProgressFn = std::function<void(std::string const& message, double fraction)>;

	/// Find the voices in a WAV of speech, ideally with the music and effects
	/// already taken out. The result is kept next to the WAV and reused while
	/// the WAV is unchanged.
	/// @param token Hugging Face access token; unused when Bundled()
	/// @throws NeedsAccess, Cancelled or Error
	std::vector<Turn> Diarize(agi::fs::path const& wav, std::string const& token, bool gpu,
		ProgressFn const& progress, std::function<bool()> const& cancelled);

	/// The voice heard most in each [start, end) range in milliseconds, or
	/// empty where no voice covers enough of it to tell
	std::vector<std::string> VoiceOfEach(std::vector<std::pair<int, int>> const& ranges,
		std::vector<Turn> const& turns);
}
