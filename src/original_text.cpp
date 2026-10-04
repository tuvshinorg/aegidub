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

#include "ass_dialogue.h"
#include "ass_file.h"

#include <algorithm>
#include <vector>

namespace {
const std::string_view extradata_key = "aegidub_original";

/// IDs of the line's extradata entries which are not original-text entries
std::vector<uint32_t> other_ids(const AssFile *file, const AssDialogue *line) {
	std::vector<uint32_t> ids = line->ExtradataIds.get();
	if (ids.empty()) return ids;
	std::sort(ids.begin(), ids.end());
	for (auto const& entry : file->GetExtradata(ids)) {
		if (entry.key == extradata_key)
			ids.erase(std::remove(ids.begin(), ids.end(), entry.id), ids.end());
	}
	return ids;
}
}

namespace original_text {
bool Has(const AssFile *file, const AssDialogue *line) {
	auto const& ids = line->ExtradataIds.get();
	if (ids.empty()) return false;
	std::vector<uint32_t> sorted = ids;
	std::sort(sorted.begin(), sorted.end());
	for (auto const& entry : file->GetExtradata(sorted)) {
		if (entry.key == extradata_key)
			return true;
	}
	return false;
}

std::string Get(const AssFile *file, const AssDialogue *line) {
	auto const& ids = line->ExtradataIds.get();
	if (ids.empty()) return {};
	std::vector<uint32_t> sorted = ids;
	std::sort(sorted.begin(), sorted.end());
	for (auto const& entry : file->GetExtradata(sorted)) {
		if (entry.key == extradata_key)
			return entry.value;
	}
	return {};
}

void Set(AssFile *file, AssDialogue *line, std::string_view text) {
	auto ids = other_ids(file, line);
	ids.push_back(file->AddExtradata(extradata_key, text));
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	line->ExtradataIds = std::move(ids);
}

void Clear(AssFile *file, AssDialogue *line) {
	if (line->ExtradataIds.get().empty()) return;
	line->ExtradataIds = other_ids(file, line);
}
}
