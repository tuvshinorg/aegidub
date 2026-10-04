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

/// @file original_text.h
/// @brief Per-line storage of the pre-translation ("original") text
///
/// The original text of a line is kept in the file's extradata section, so
/// it survives saving and reloading without polluting the dialogue text.

#pragma once

#include <string>
#include <string_view>

class AssDialogue;
class AssFile;

namespace original_text {
	/// Does this line have a stored original text?
	bool Has(const AssFile *file, const AssDialogue *line);

	/// Get the stored original text of a line, or an empty string if there is none
	std::string Get(const AssFile *file, const AssDialogue *line);

	/// Store (or replace) the original text of a line
	void Set(AssFile *file, AssDialogue *line, std::string_view text);

	/// Remove the stored original text of a line
	void Clear(AssFile *file, AssDialogue *line);
}
