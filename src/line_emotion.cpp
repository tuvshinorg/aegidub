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

#include "line_emotion.h"

#include "line_extradata.h"

#include <boost/algorithm/string/trim.hpp>

namespace {
const std::string_view extradata_key = "aegidub_emotion";
/// Longest single tag kept; anything longer is not a direction but prose
const size_t max_tag_length = 40;
}

namespace line_emotion {
std::string Get(const AssFile *file, const AssDialogue *line) {
	return line_extradata::Get(file, line, extradata_key);
}

void Set(AssFile *file, AssDialogue *line, std::string_view tags) {
	std::string value = Normalize(tags);
	if (value.empty())
		line_extradata::Clear(file, line, extradata_key);
	else if (value != Get(file, line))
		line_extradata::Set(file, line, extradata_key, value);
}

std::string Normalize(std::string_view input) {
	std::string result;
	std::string tag;
	auto flush = [&] {
		boost::trim(tag);
		if (!tag.empty() && tag.size() <= max_tag_length) {
			if (!result.empty()) result += ' ';
			result += '[' + tag + ']';
		}
		tag.clear();
	};

	for (char ch : input) {
		if (ch == '[' || ch == ']' || ch == ',' || ch == ';' || ch == '\n' || ch == '\r')
			flush();
		else
			tag += ch;
	}
	flush();
	return result;
}

std::vector<std::string> const& Presets() {
	static const std::vector<std::string> presets = {
		"happy", "excited", "laughs", "playful", "sarcastic",
		"calm", "tired", "sighs", "sad", "crying",
		"angry", "shouting", "scared", "nervous", "surprised",
		"curious", "whispers", "serious",
	};
	return presets;
}
}
