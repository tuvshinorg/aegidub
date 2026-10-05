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

#include "line_extradata.h"

#include "ass_dialogue.h"
#include "ass_file.h"

#include <algorithm>
#include <vector>

namespace {
std::vector<ExtradataEntry> entries(const AssFile *file, const AssDialogue *line) {
	auto const& ids = line->ExtradataIds.get();
	if (ids.empty()) return {};
	std::vector<uint32_t> sorted = ids;
	std::sort(sorted.begin(), sorted.end());
	return file->GetExtradata(sorted);
}

/// IDs of the line's extradata entries which are not for the key
std::vector<uint32_t> other_ids(const AssFile *file, const AssDialogue *line, std::string_view key) {
	std::vector<uint32_t> ids = line->ExtradataIds.get();
	if (ids.empty()) return ids;
	std::sort(ids.begin(), ids.end());
	for (auto const& entry : file->GetExtradata(ids)) {
		if (entry.key == key)
			ids.erase(std::remove(ids.begin(), ids.end(), entry.id), ids.end());
	}
	return ids;
}
}

namespace line_extradata {
bool Has(const AssFile *file, const AssDialogue *line, std::string_view key) {
	for (auto const& entry : entries(file, line)) {
		if (entry.key == key)
			return true;
	}
	return false;
}

std::string Get(const AssFile *file, const AssDialogue *line, std::string_view key) {
	for (auto const& entry : entries(file, line)) {
		if (entry.key == key)
			return entry.value;
	}
	return {};
}

void Set(AssFile *file, AssDialogue *line, std::string_view key, std::string_view value) {
	auto ids = other_ids(file, line, key);
	ids.push_back(file->AddExtradata(key, value));
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	line->ExtradataIds = std::move(ids);
}

void Clear(AssFile *file, AssDialogue *line, std::string_view key) {
	if (line->ExtradataIds.get().empty()) return;
	line->ExtradataIds = other_ids(file, line, key);
}
}
