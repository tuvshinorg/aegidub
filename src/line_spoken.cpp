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

#include "line_spoken.h"

#include "ass_dialogue.h"
#include "dub_settings.h"
#include "line_extradata.h"

#include <boost/algorithm/string/trim.hpp>

namespace {
const std::string_view spoken_key = "aegidub_spoken";
/// The subtitle text the spoken text was made from
const std::string_view source_key = "aegidub_spoken_for";
}

namespace line_spoken {
std::string Get(const AssFile *file, const AssDialogue *line) {
	if (line->ExtradataIds.get().empty()) return {};
	if (line_extradata::Get(file, line, source_key) != line->Text.get()) return {};
	return line_extradata::Get(file, line, spoken_key);
}

void Set(AssFile *file, AssDialogue *line, std::string_view spoken) {
	std::string value(spoken);
	boost::trim(value);
	// Spoken text identical to what would be read anyway adds nothing
	if (value.empty() || value == dub::SpokenText(line->Text.get())) {
		line_extradata::Clear(file, line, spoken_key);
		line_extradata::Clear(file, line, source_key);
		return;
	}
	line_extradata::Set(file, line, spoken_key, value);
	line_extradata::Set(file, line, source_key, line->Text.get());
}

std::string ForSpeech(const AssFile *file, const AssDialogue *line) {
	std::string spoken = Get(file, line);
	return spoken.empty() ? dub::SpokenText(line->Text.get()) : spoken;
}

// Non-ASCII text is written as UTF-8 byte escapes, since the compiler
// doesn't necessarily read source files as UTF-8
const char *PromptRules() {
	return
		"- `spoken` is the line as a speech engine must read it aloud, in the same language and "
		"script as the line. Write every number, year, date, time, percentage, amount of money, "
		"unit and symbol out in words, with the grammatical endings the sentence needs. Mongolian "
		"examples: \"1990 \xd0\xbe\xd0\xbd\xd0\xb4\" -> \"\xd0\xbc\xd1\x8f\xd0\xbd\xd0\xb3\xd0\xb0 \xd0\xb5\xd1\x81\xd3\xa9\xd0\xbd \xd0\xb7\xd1\x83\xd1\x83\xd0\xbd \xd0\xb5\xd1\x80\xd1\x8d\xd0\xbd \xd0\xbe\xd0\xbd\xd0\xb4\", \"10:30\" -> \"\xd0\xb0\xd1\x80\xd0\xb2\xd0\xb0\xd0\xbd \xd1\x86\xd0\xb0\xd0\xb3 \xd0\xb3\xd1\x83\xd1\x87\xd0\xb8\xd0\xbd \xd0\xbc\xd0\xb8\xd0\xbd\xd1\x83\xd1\x82\", "
		"\"25%\" -> \"\xd1\x85\xd0\xbe\xd1\x80\xd0\xb8\xd0\xbd \xd1\x82\xd0\xb0\xd0\xb2\xd0\xb0\xd0\xbd \xd1\x85\xd1\x83\xd0\xb2\xd1\x8c\", \"5000\xe2\x82\xae\" -> \"\xd1\x82\xd0\xb0\xd0\xb2\xd0\xb0\xd0\xbd \xd0\xbc\xd1\x8f\xd0\xbd\xd0\xb3\xd0\xb0\xd0\xbd \xd1\x82\xd3\xa9\xd0\xb3\xd1\x80\xd3\xa9\xd0\xb3\". Write Latin-script names "
		"and words the way they are pronounced, in the line's own script (\"Dispatch\" -> \"\xd0\x94\xd0\xb8\xd1\x81\xd0\xbf\xd0\xb0\xd1\x82\xd1\x87\"). "
		"Expand abbreviations that are said as words. No ASS tags or line breaks.\n"
		"- Include `spoken` only for lines containing such numbers, symbols, Latin words or "
		"abbreviations; leave it out for every other line.\n";
}

bool NeedsSpokenForm(std::string_view text) {
	for (unsigned char ch : text) {
		if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
			return true;
		switch (ch) {
			case '%': case '$': case '&': case '@': case '#': case '+':
			case '=': case '/': case '*': case '<': case '>': case '^': case '~':
				return true;
		}
	}
	// Multi-byte symbols such as the tugrik, euro and numero signs
	for (std::string_view sym : {"\xE2\x82\xAE", "\xE2\x82\xAC", "\xE2\x84\x96", "\xC2\xB0", "\xC2\xA3", "\xC2\xA5"}) {
		if (text.find(sym) != std::string_view::npos)
			return true;
	}
	return false;
}
}
