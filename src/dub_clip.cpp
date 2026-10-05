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

#include "dub_clip.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "dub_settings.h"
#include "format.h"
#include "line_emotion.h"
#include "line_spoken.h"
#include "options.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

namespace {
uint64_t fnv1a(std::string const& data) {
	uint64_t hash = 0xcbf29ce484222325ULL;
	for (unsigned char ch : data) {
		hash ^= ch;
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

/// Read the length from a WAV header without loading the audio
int read_duration_ms(agi::fs::path const& path) {
	std::ifstream in(path, std::ios::binary);
	char header[12];
	if (!in.read(header, 12) || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4))
		return -1;
	uint32_t byte_rate = 0;
	char chunk[8];
	while (in.read(chunk, 8)) {
		uint32_t size;
		memcpy(&size, chunk + 4, 4);
		if (!memcmp(chunk, "fmt ", 4) && size >= 16) {
			char fmt[16];
			in.read(fmt, 16);
			memcpy(&byte_rate, fmt + 8, 4);
			in.seekg(size - 16 + (size & 1), std::ios::cur);
		}
		else if (!memcmp(chunk, "data", 4))
			return byte_rate ? static_cast<int>(static_cast<uint64_t>(size) * 1000 / byte_rate) : -1;
		else
			in.seekg(size + (size & 1), std::ios::cur);
	}
	return -1;
}
}

namespace dub_clip {
bool ForLine(const AssFile *file, const AssDialogue *line, voice_cast::Cast const& cast,
	Request& request, std::string *why_not)
{
	std::string text = line_spoken::ForSpeech(file, line);
	if (line->Comment || text.empty()) {
		if (why_not) *why_not = from_wx(_("The line has no text to speak."));
		return false;
	}
	std::string character = line->Actor.get();
	if (character.empty()) {
		if (why_not) *why_not = from_wx(_("The line has no character. Set its Character cell, or run AI Detect Speakers."));
		return false;
	}
	auto voice = cast.find(character);
	if (voice == cast.end()) {
		if (why_not) *why_not = from_wx(fmt_tl("%s has no voice yet. Pick one in Subtitle > Voice Cast.", character));
		return false;
	}

	std::string tags = line_emotion::Get(file, line);
	request.character = character;
	request.voice_id = voice->second;
	request.text = tags.empty() ? text : tags + " " + text;
	return true;
}

agi::fs::path Path(agi::fs::path const& subs_path, elevenlabs::Config const& config, Request const& request) {
	// Identical requests give the same clip, so name it after what was asked
	char name[32];
	snprintf(name, sizeof(name), "%016llx.wav", static_cast<unsigned long long>(
		fnv1a(request.voice_id + '\n' + config.model + '\n' + config.language_code + '\n' + request.text)));
	return agi::fs::path(dub::Folder(subs_path) / "clips" / name);
}

int DurationMs(agi::fs::path const& clip) {
	// A clip's contents never change once written, so its length can be
	// remembered; only whether it still exists needs checking
	static std::mutex mutex;
	static std::map<std::string, int> known;

	if (!agi::fs::FileExists(clip)) return -1;
	std::lock_guard<std::mutex> lock(mutex);
	auto it = known.find(clip.string());
	if (it != known.end()) return it->second;
	int ms = read_duration_ms(clip);
	if (ms >= 0) known[clip.string()] = ms;
	return ms;
}

double MaxTempo() {
	return 1.0 + std::clamp<int64_t>(OPT_GET("Tool/Voice Cast/Max Speed Up")->GetInt(), 0, 50) / 100.0;
}

double Tempo(int speech_ms, int room_ms) {
	if (room_ms <= 0 || speech_ms <= room_ms) return 1.0;
	return std::min(MaxTempo(), static_cast<double>(speech_ms) / room_ms);
}

int RoomMs(const AssFile *file, const AssDialogue *line) {
	const int start = static_cast<int>(line->Start);
	auto it = file->Events.iterator_to(*line);
	for (++it; it != file->Events.end(); ++it) {
		if (it->Comment || static_cast<int>(it->Start) <= start) continue;
		if (dub::SpokenText(it->Text.get()).empty()) continue;
		return static_cast<int>(it->Start) - start;
	}
	return static_cast<int>(line->End) - start;
}
}
