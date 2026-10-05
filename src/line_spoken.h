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

/// @file line_spoken.h
/// @brief Per-line text for speech synthesis, when it differs from the subtitle
///
/// Subtitles show "10:30" or "25%", but a speech engine reads those badly.
/// The spoken text spells them out ("арван цаг гучин минут"). It is stored
/// with the subtitle text it was made from, and is ignored once the subtitle
/// text changes, so an edited line is never dubbed with stale words.

#pragma once

#include <string>
#include <string_view>

class AssDialogue;
class AssFile;

namespace line_spoken {
	/// The line's spoken text, or empty if it has none or the line's text has
	/// changed since it was made
	std::string Get(const AssFile *file, const AssDialogue *line);

	/// Store spoken text for the line's current text; an empty value clears it
	void Set(AssFile *file, AssDialogue *line, std::string_view spoken);

	/// Text to send to the speech engine: the spoken text if there is a
	/// current one, otherwise the subtitle text without tags or line breaks
	std::string ForSpeech(const AssFile *file, const AssDialogue *line);

	/// Does the text contain digits, symbols or Latin letters that a speech
	/// engine might read wrongly in a Cyrillic-script language?
	bool NeedsSpokenForm(std::string_view text);

	/// Prompt instructions telling a model how to write the `spoken` field
	const char *PromptRules();
}
