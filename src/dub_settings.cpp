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

#include "dub_settings.h"

#include "ass_dialogue.h"
#include "options.h"

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cstdlib>

namespace {
std::string option_or_env(const char *option, const char *env) {
	std::string value = OPT_GET(option)->GetString();
	if (value.empty()) {
		if (const char *env_value = std::getenv(env))
			value = env_value;
	}
	boost::trim(value);
	return value;
}

std::string trimmed_option(const char *option) {
	std::string value = OPT_GET(option)->GetString();
	boost::trim(value);
	return value;
}
}

namespace dub {
openai::Config LoadOpenAIConfig() {
	openai::Config config;
	config.api_key = option_or_env("Tool/AI Translation/API Key", "OPENAI_API_KEY");
	config.base_url = trimmed_option("Tool/AI Translation/Base URL");
	config.model = trimmed_option("Tool/AI Translation/Model");
	config.target_language = trimmed_option("Tool/AI Translation/Target Language");
	config.instructions = OPT_GET("Tool/AI Translation/Instructions")->GetString();
	return config;
}

elevenlabs::Config LoadElevenLabsConfig() {
	elevenlabs::Config config;
	config.api_key = option_or_env("Tool/Voice Cast/API Key", "ELEVENLABS_API_KEY");
	config.model = trimmed_option("Tool/Voice Cast/Model");
	config.language_code = trimmed_option("Tool/Voice Cast/Language Code");
	return config;
}

agi::fs::path Folder(agi::fs::path const& subs_path) {
	return agi::fs::path(subs_path.parent_path() / (subs_path.stem().string() + "_dub"));
}

agi::fs::path TrackPath(agi::fs::path const& subs_path) {
	return agi::fs::path(subs_path.parent_path() / (subs_path.stem().string() + "_dub.wav"));
}

std::string SpokenText(std::string_view ass_text, size_t max_bytes) {
	AssDialogue line;
	line.Text = std::string(ass_text);
	std::string text = line.GetStrippedText();
	boost::replace_all(text, "\\N", " ");
	boost::replace_all(text, "\\n", " ");
	boost::replace_all(text, "\\h", " ");
	boost::trim(text);
	if (max_bytes && text.size() > max_bytes) {
		size_t cut = max_bytes;
		// Don't cut a UTF-8 sequence in half
		while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
			--cut;
		text.resize(cut);
	}
	return text;
}
}
