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

/// @file elevenlabs_client.h
/// @brief Minimal ElevenLabs client for listing voices and previewing speech

#pragma once

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace elevenlabs {
	struct Error : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	struct Config {
		std::string api_key;
		/// Text-to-speech model, e.g. eleven_v4
		std::string model;
		/// ISO 639-1 language code passed to text-to-speech, e.g. "mn"
		std::string language_code;
	};

	struct Voice {
		std::string id;
		std::string name;
		/// "female", "male" or empty when unknown
		std::string gender;
		/// Short description of the voice's character, used to pick voices
		std::string description;
		/// ElevenLabs has verified this voice for the Mongolian language
		bool mongolian = false;
		/// The voice is in the user's own account rather than the built-in list
		bool account = false;
	};

	/// The Mongolian voices of the ElevenLabs voice library
	std::vector<Voice> const& LibraryVoices();

	/// Voices in the user's account, other than ElevenLabs' stock English voices
	/// @throws Error on network, HTTP, or response format errors
	std::vector<Voice> AccountVoices(Config const& config, std::function<bool()> const& cancelled);

	struct Usage {
		/// Characters used and allowed in the current billing period
		int64_t used = 0;
		int64_t limit = 0;
	};

	/// The account's character allowance
	/// @return false if it couldn't be read, e.g. the key lacks User: Read
	bool GetUsage(Config const& config, Usage& usage);

	/// Characters ElevenLabs bills for a text
	int64_t BilledCharacters(std::string const& text);

	/// Speak text with a voice
	/// @return A complete 16-bit mono WAV file
	/// @throws Error on network, HTTP, or response format errors
	std::vector<char> Speak(Config const& config, std::string const& voice_id, std::string const& text, std::function<bool()> const& cancelled);
}
