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

/// @file dub_clip.h
/// @brief The generated speech of each line, and how well it fits its time
///
/// A clip is named after everything that went into it (voice, model,
/// emotion, text), so a clip found on disk is always the speech of the line
/// as it is now, and a changed line simply has no clip yet.

#pragma once

#include "elevenlabs_client.h"
#include "voice_cast.h"

#include <libaegisub/fs.h>

#include <string>

class AssDialogue;
class AssFile;

namespace dub_clip {
	/// What to ask the speech engine for one line
	struct Request {
		std::string character;
		std::string voice_id;
		/// Emotion tags followed by the text to speak
		std::string text;
	};

	/// Work out the request for a line
	/// @param why_not If not null, set to the reason a line can't be spoken
	/// @return false if the line can't be spoken
	bool ForLine(const AssFile *file, const AssDialogue *line, voice_cast::Cast const& cast,
		Request& request, std::string *why_not);

	/// Where the clip for a request is kept
	agi::fs::path Path(agi::fs::path const& subs_path, elevenlabs::Config const& config, Request const& request);

	/// Length of a generated clip in milliseconds, or -1 if it doesn't exist
	int DurationMs(agi::fs::path const& clip);

	/// Time a line's speech may take: until the next line starts, or the
	/// line's own duration when it is the last
	int RoomMs(const AssFile *file, const AssDialogue *line);

	/// The most a line's speech may be sped up to fit, e.g. 1.1 for 10%
	double MaxTempo();

	/// How much faster to play speech so it fits its room: 1 when it already
	/// fits, at most MaxTempo() when it doesn't
	double Tempo(int speech_ms, int room_ms);
}
