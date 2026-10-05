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

/// @file voice_cast.h
/// @brief The character to dubbing voice assignments stored in a script

#pragma once

#include <map>
#include <string>

class AssFile;

namespace voice_cast {
	/// Character name to ElevenLabs voice ID
	using Cast = std::map<std::string, std::string>;

	/// Read the voice cast stored in the file's [Script Info]
	Cast Load(const AssFile *file);

	/// Store the voice cast in the file's [Script Info], removing the entry
	/// when the cast is empty. The caller is responsible for committing.
	void Save(AssFile *file, Cast const& cast);
}
