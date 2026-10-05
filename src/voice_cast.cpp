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

#include "voice_cast.h"

#include "ass_file.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <sstream>

namespace {
/// Stored as a single-line JSON object so that any character name survives
const std::string_view info_key = "Aegidub Voice Cast";
}

namespace voice_cast {
Cast Load(const AssFile *file) {
	Cast cast;
	std::string value(file->GetScriptInfo(info_key));
	if (value.empty()) return cast;

	try {
		std::istringstream ss(value);
		json::UnknownElement root;
		json::Reader::Read(root, ss);
		json::Object const& obj = root;
		for (auto const& entry : obj) {
			try {
				std::string id = static_cast<json::String const&>(entry.second);
				if (!entry.first.empty() && !id.empty())
					cast[entry.first] = std::move(id);
			}
			catch (json::Exception const&) { }
		}
	}
	catch (...) {
		// A hand-edited or damaged entry is treated as no cast
	}
	return cast;
}

void Save(AssFile *file, Cast const& cast) {
	if (cast.empty()) {
		file->SetScriptInfo(info_key, "");
		return;
	}

	json::Object obj;
	for (auto const& entry : cast)
		obj.emplace(entry.first, json::UnknownElement(entry.second));

	std::ostringstream ss;
	agi::JsonWriter::Write(json::UnknownElement(std::move(obj)), ss);

	// The writer pretty-prints; [Script Info] values must be a single line
	std::string value;
	for (char ch : ss.str()) {
		if (ch != '\n' && ch != '\r' && ch != '\t')
			value += ch;
	}
	file->SetScriptInfo(info_key, value);
}
}
