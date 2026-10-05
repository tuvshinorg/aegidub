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

/// @file line_extradata.h
/// @brief Per-line values kept in the file's extradata section, one per key

#pragma once

#include <string>
#include <string_view>

class AssDialogue;
class AssFile;

namespace line_extradata {
	/// Does this line have a value for the key?
	bool Has(const AssFile *file, const AssDialogue *line, std::string_view key);

	/// Get the line's value for the key, or an empty string if there is none
	std::string Get(const AssFile *file, const AssDialogue *line, std::string_view key);

	/// Store (or replace) the line's value for the key
	void Set(AssFile *file, AssDialogue *line, std::string_view key, std::string_view value);

	/// Remove the line's value for the key
	void Clear(AssFile *file, AssDialogue *line, std::string_view key);
}
