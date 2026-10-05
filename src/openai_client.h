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

/// @file openai_client.h
/// @brief Minimal OpenAI-compatible chat completions client for subtitle translation

#pragma once

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace openai {
	struct Error : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	struct Config {
		std::string api_key;
		/// Base URL of the API, e.g. https://api.openai.com/v1
		std::string base_url;
		std::string model;
		/// Language to translate into, e.g. "Mongolian"
		std::string target_language;
		/// Optional additional instructions appended to the system prompt
		std::string instructions;
	};

	struct Line {
		int id;
		std::string actor;
		std::string text;
	};

	/// Send one system + user message pair in JSON mode
	/// @return The model's reply, which should be a JSON object
	/// @throws Error on network, HTTP, or response format errors
	std::string CompleteJson(Config const& config,
		std::string const& system,
		std::string const& user,
		std::function<bool()> const& cancelled);

	struct Translation {
		/// Subtitle text, as shown on screen
		std::string text;
		/// The same line written out for a speech engine, or empty when the
		/// text can be read as it is
		std::string spoken;
	};

	/// Translate a batch of subtitle lines
	/// @param config    API settings
	/// @param lines     Lines to translate
	/// @param context   Preceding (source, translation) pairs given as context only
	/// @param cancelled Polled during the request; return true to abort
	/// @return Map of line id to translation. Lines the model failed to
	///         return are simply absent.
	/// @throws Error on network, HTTP, or response format errors
	std::map<int, Translation> Translate(Config const& config,
		std::vector<Line> const& lines,
		std::vector<std::pair<std::string, std::string>> const& context,
		std::function<bool()> const& cancelled);
}
