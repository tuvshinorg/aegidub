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

#include "original_text.h"

#include "line_extradata.h"

namespace {
const std::string_view extradata_key = "aegidub_original";
}

namespace original_text {
bool Has(const AssFile *file, const AssDialogue *line) {
	return line_extradata::Has(file, line, extradata_key);
}

std::string Get(const AssFile *file, const AssDialogue *line) {
	return line_extradata::Get(file, line, extradata_key);
}

void Set(AssFile *file, AssDialogue *line, std::string_view text) {
	line_extradata::Set(file, line, extradata_key, text);
}

void Clear(AssFile *file, AssDialogue *line) {
	line_extradata::Clear(file, line, extradata_key);
}
}
