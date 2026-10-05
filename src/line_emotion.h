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

/// @file line_emotion.h
/// @brief Per-line delivery directions for dubbing, as ElevenLabs audio tags
///
/// An emotion is stored as a string of audio tags such as "[sad] [whispers]"
/// in the file's extradata, so the dialogue text itself stays clean.

#pragma once

#include <string>
#include <string_view>
#include <vector>

class AssDialogue;
class AssFile;

namespace line_emotion {
	/// The line's emotion tags, or an empty string for a neutral delivery
	std::string Get(const AssFile *file, const AssDialogue *line);

	/// Set the line's emotion; an empty value clears it
	void Set(AssFile *file, AssDialogue *line, std::string_view tags);

	/// Turn user or model input such as "sad, whispers" or "[sad][whispers]"
	/// into the canonical "[sad] [whispers]" form
	std::string Normalize(std::string_view input);

	/// Common tags offered in the emotion picker, without brackets
	std::vector<std::string> const& Presets();
}
