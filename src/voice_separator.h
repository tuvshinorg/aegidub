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

/// @file voice_separator.h
/// @brief Split a soundtrack into the voices and everything else with htdemucs
///
/// The built-in separator is a C++ port of htdemucs running on the CPU. When
/// Python with demucs and a CUDA GPU is available it is used instead, being
/// much faster; the result is the same model either way.

#pragma once

#include <libaegisub/fs.h>

#include <functional>
#include <stdexcept>
#include <string>

namespace voice_separator {
	struct Error : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	struct Cancelled : Error {
		Cancelled() : Error("Cancelled") { }
	};

	/// @param message  What is happening now
	/// @param fraction Progress of the whole job, 0 to 1, or negative if unknown
	using ProgressFn = std::function<void(std::string const& message, double fraction)>;

	/// Split a 44.1 kHz 16-bit stereo WAV into a voices WAV and a background
	/// WAV of the same format. Downloads the model on first use.
	/// @return A short description of how it was done, e.g. "GPU (CUDA)"
	/// @throws Cancelled if cancelled, Error on failure
	std::string Separate(agi::fs::path const& input,
		agi::fs::path const& vocals,
		agi::fs::path const& background,
		agi::fs::path const& work_dir,
		ProgressFn const& progress,
		std::function<bool()> const& cancelled);
}
