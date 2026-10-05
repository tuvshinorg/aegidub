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

/// @file dub_settings.h
/// @brief Helpers shared by the AI translation, casting and dubbing features

#pragma once

#include "elevenlabs_client.h"
#include "openai_client.h"

#include <libaegisub/fs.h>

#include <string>
#include <string_view>

class AssDialogue;

namespace dub {
	/// Chat model settings from Preferences > AI Translation, with the key
	/// falling back to OPENAI_API_KEY
	openai::Config LoadOpenAIConfig();

	/// Text-to-speech settings from Preferences > Voice Cast, with the key
	/// falling back to ELEVENLABS_API_KEY
	elevenlabs::Config LoadElevenLabsConfig();

	/// ASS dialogue text as it would be spoken: no override tags or line
	/// breaks, and at most max_bytes long (0 for no limit)
	std::string SpokenText(std::string_view ass_text, size_t max_bytes = 0);

	/// Folder holding the generated speech of a script, next to the script
	agi::fs::path Folder(agi::fs::path const& subs_path);

	/// The assembled dub track of a script, next to the script
	agi::fs::path TrackPath(agi::fs::path const& subs_path);
}
