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

#include "json_util.h"
#include "elevenlabs_client.h"

#include "http_request.h"
#include "wav_util.h"


#include <cstdint>
#include <sstream>

namespace {
using json_util::parse_json;
using json_util::find;
using json_util::find_string;
const char *api_base = "https://api.elevenlabs.io";
const int sample_rate = 24000;

/// Extract the human-readable message from an API error response, if any
std::string api_error_message(std::string const& response) {
	try {
		auto root = parse_json(response);
		json::Object const& obj = root;
		if (auto detail = find(obj, "detail")) {
			try {
				json::Object const& detail_obj = *detail;
				auto msg = find_string(detail_obj, "message");
				if (!msg.empty()) return msg;
			}
			catch (json::Exception const&) {
				return static_cast<json::String const&>(*detail);
			}
		}
	}
	catch (...) { }
	return response.substr(0, 300);
}

/// Perform a request; a non-empty body makes it a POST
std::string http_request(elevenlabs::Config const& config, std::string const& path, std::string const& body, std::function<bool()> const& cancelled) {
	http::Response response;
	try {
		response = http::Request(api_base + path,
			{"Content-Type: application/json", "xi-api-key: " + config.api_key},
			body.empty() ? nullptr : &body, 180L, cancelled);
	}
	catch (http::Error const& e) {
		throw elevenlabs::Error(std::string("Request failed: ") + e.what());
	}

	if (response.status < 200 || response.status >= 300)
		throw elevenlabs::Error("ElevenLabs error " + std::to_string(response.status) + ": " + api_error_message(response.body));
	return response.body;
}

/// Wrap raw 16-bit little-endian mono PCM in a WAV header
std::vector<char> make_wav(std::string const& pcm) {
	std::ostringstream header;
	wav_util::WriteHeader(header, 1, sample_rate, static_cast<uint32_t>(pcm.size()));
	std::string head = header.str();
	std::vector<char> out;
	out.reserve(head.size() + pcm.size());
	out.insert(out.end(), head.begin(), head.end());
	out.insert(out.end(), pcm.begin(), pcm.end());
	return out;
}
}

namespace elevenlabs {
std::vector<Voice> const& LibraryVoices() {
	static const std::vector<Voice> voices = {
		{"WgH4JH8sD6a2SIrujiKn", "Sarnai", "female", "proud, expressive narrator; storytelling", true, false},
		{"6OjaeAQxnuXC0oUZXZR2", "Bolor", "female", "young, playful and energetic; Ulaanbaatar accent", true, false},
		{"2cecqSnkajrth9sJSoEH", "Uyanga", "female", "kind, friendly and natural conversational voice", true, false},
		{"49bcW9p7CyYxa3c0X0im", "Enkhtuya", "female", "proud, strong and confident; Ovorkhangai accent", true, false},
		{"4pSHaU93d1XS027ZFhHB", "Oyuna", "female", "warm, gentle narrator; mature", true, false},
		{"sjPAZPn7M1KgdmdYfsuu", "Bataar", "male", "calm, steady narrator; mature and wise", true, false},
		{"DLfKtGm2VGo06slN2VJE", "Munkhbat", "male", "warm and friendly; Khentii accent", true, false},
		{"RbMF2tQ1nCK38TfvNGLk", "Ganbold", "male", "confident, bold and commanding", true, false},
		{"Die79un8ishA33PLnH1j", "Batbayar", "male", "firm, serious and stern", true, false},
		{"ztVKSTjXnQBPBYroYARn", "Temuulen", "male", "young, upbeat and energetic", true, false},
		{"D9okmaITNQEQZq1w4Z1C", "Erdene", "male", "blunt, casual, rough conversational voice", true, false},
		{"sQRZO8j8yYwJ3eCSUFy3", "Naran", "male", "patient, gentle and kind conversational voice", true, false},
	};
	return voices;
}

std::vector<Voice> AccountVoices(Config const& config, std::function<bool()> const& cancelled) {
	std::string response = http_request(config, "/v2/voices?page_size=100", {}, cancelled);

	std::vector<Voice> voices;
	try {
		auto root = parse_json(response);
		json::Object const& root_obj = root;
		auto list = find(root_obj, "voices");
		if (!list) throw Error("Response has no voices");

		for (auto const& entry : static_cast<json::Array const&>(*list)) {
			json::Object const& obj = entry;
			Voice voice;
			voice.id = find_string(obj, "voice_id");
			voice.name = find_string(obj, "name");
			voice.account = true;
			if (voice.id.empty()) continue;

			if (auto verified = find(obj, "verified_languages")) {
				for (auto const& lang : static_cast<json::Array const&>(*verified)) {
					if (find_string(lang, "language") == "mn")
						voice.mongolian = true;
				}
			}

			// The stock voices are English-only and would only clutter the list
			if (find_string(obj, "category") == "premade" && !voice.mongolian)
				continue;

			if (auto labels = find(obj, "labels")) {
				json::Object const& labels_obj = *labels;
				voice.gender = find_string(labels_obj, "gender");
				voice.description = find_string(labels_obj, "descriptive");
			}
			if (voice.description.empty())
				voice.description = find_string(obj, "description");

			voices.push_back(std::move(voice));
		}
	}
	catch (Error const&) {
		throw;
	}
	catch (std::exception const& e) {
		throw Error(std::string("Could not parse the voice list: ") + e.what());
	}
	return voices;
}

bool GetUsage(Config const& config, Usage& usage) {
	try {
		auto response = http::Request(std::string(api_base) + "/v1/user/subscription",
			{"xi-api-key: " + config.api_key}, nullptr, 20L, nullptr);
		if (response.status != 200) return false;
		auto root = parse_json(response.body);
		json::Object const& obj = root;
		auto used = find(obj, "character_count");
		auto limit = find(obj, "character_limit");
		if (!used || !limit) return false;
		usage.used = static_cast<json::Integer const&>(*used);
		usage.limit = static_cast<json::Integer const&>(*limit);
		return true;
	}
	catch (...) {
		return false;
	}
}

int64_t BilledCharacters(std::string const& text) {
	// Characters, not bytes: count everything but UTF-8 continuation bytes
	int64_t count = 0;
	for (unsigned char ch : text)
		count += (ch & 0xC0) != 0x80;
	return count;
}

std::vector<char> Speak(Config const& config, std::string const& voice_id, std::string const& text, std::function<bool()> const& cancelled) {
	json::Object root;
	root.emplace("text", json::UnknownElement(text));
	root.emplace("model_id", json::UnknownElement(config.model));
	if (!config.language_code.empty())
		root.emplace("language_code", json::UnknownElement(config.language_code));

	std::ostringstream body;
	agi::JsonWriter::Write(json::UnknownElement(std::move(root)), body);

	std::string pcm = http_request(config,
		"/v1/text-to-speech/" + voice_id + "?output_format=pcm_" + std::to_string(sample_rate),
		body.str(), cancelled);
	if (pcm.size() < 2)
		throw Error("ElevenLabs returned no audio");
	if (pcm.size() % 2)
		pcm.pop_back();
	return make_wav(pcm);
}
}
