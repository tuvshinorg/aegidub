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

#include "openai_client.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <curl/curl.h>

#include <cstdint>
#include <sstream>

namespace {
size_t write_cb(char *contents, size_t size, size_t nmemb, void *userp) {
	static_cast<std::string *>(userp)->append(contents, size * nmemb);
	return size * nmemb;
}

int progress_cb(void *userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto cancelled = static_cast<const std::function<bool()> *>(userp);
	return (*cancelled && (*cancelled)()) ? 1 : 0;
}

std::string to_json(json::UnknownElement const& value) {
	std::ostringstream ss;
	agi::JsonWriter::Write(value, ss);
	return ss.str();
}

json::UnknownElement parse_json(std::string const& str) {
	std::istringstream ss(str);
	json::UnknownElement root;
	json::Reader::Read(root, ss);
	return root;
}

const json::UnknownElement *find(json::Object const& obj, const char *key) {
	auto it = obj.find(key);
	return it == obj.end() ? nullptr : &it->second;
}

std::string system_prompt(openai::Config const& config) {
	std::string prompt =
		"You are a professional subtitle translator preparing a script for voice dubbing.\n"
		"Translate the `text` of every line into " + config.target_language + ".\n"
		"Rules:\n"
		"- Write natural spoken language that an actor can perform, and keep each line "
		"roughly as long as the source so it fits the same timing.\n"
		"- `actor` is the character speaking. Use it and the surrounding lines to keep "
		"each character's tone, register and names consistent.\n"
		"- Copy ASS override blocks in {curly braces} unchanged and keep them at the "
		"matching position. Keep the line break markers \\N, \\n and \\h exactly as written.\n"
		"- Translate line by line. Never merge, split, drop, add or reorder lines.\n"
		"- Reply with a single JSON object of the form "
		"{\"lines\":[{\"id\":<id>,\"text\":\"<translation>\"}]} containing exactly the ids you were given.";
	if (!config.instructions.empty())
		prompt += "\nAdditional instructions:\n" + config.instructions;
	return prompt;
}

std::string user_prompt(std::vector<openai::Line> const& lines, std::vector<std::pair<std::string, std::string>> const& context) {
	json::Object root;

	if (!context.empty()) {
		json::Array ctx;
		for (auto const& pair : context) {
			json::Object entry;
			entry.emplace("source", json::UnknownElement(pair.first));
			entry.emplace("translation", json::UnknownElement(pair.second));
			ctx.emplace_back(std::move(entry));
		}
		root.emplace("previous_lines_for_context_only", json::UnknownElement(std::move(ctx)));
	}

	json::Array arr;
	for (auto const& line : lines) {
		json::Object entry;
		entry.emplace("id", json::UnknownElement(static_cast<int64_t>(line.id)));
		if (!line.actor.empty())
			entry.emplace("actor", json::UnknownElement(line.actor));
		entry.emplace("text", json::UnknownElement(line.text));
		arr.emplace_back(std::move(entry));
	}
	root.emplace("lines", json::UnknownElement(std::move(arr)));

	return to_json(json::UnknownElement(std::move(root)));
}

std::string request_body(openai::Config const& config, std::vector<openai::Line> const& lines, std::vector<std::pair<std::string, std::string>> const& context) {
	json::Array messages;
	{
		json::Object msg;
		msg.emplace("role", json::UnknownElement("system"));
		msg.emplace("content", json::UnknownElement(system_prompt(config)));
		messages.emplace_back(std::move(msg));
	}
	{
		json::Object msg;
		msg.emplace("role", json::UnknownElement("user"));
		msg.emplace("content", json::UnknownElement(user_prompt(lines, context)));
		messages.emplace_back(std::move(msg));
	}

	json::Object format;
	format.emplace("type", json::UnknownElement("json_object"));

	json::Object root;
	root.emplace("model", json::UnknownElement(config.model));
	root.emplace("messages", json::UnknownElement(std::move(messages)));
	root.emplace("response_format", json::UnknownElement(std::move(format)));
	return to_json(json::UnknownElement(std::move(root)));
}

/// Extract the human-readable message from an API error response, if any
std::string api_error_message(std::string const& response) {
	try {
		auto root = parse_json(response);
		json::Object const& obj = root;
		if (auto err = find(obj, "error")) {
			json::Object const& err_obj = *err;
			if (auto msg = find(err_obj, "message"))
				return static_cast<json::String const&>(*msg);
		}
	}
	catch (...) { }
	return response.substr(0, 300);
}

std::string http_post(openai::Config const& config, std::string const& body, std::function<bool()> const& cancelled) {
	CURL *curl = curl_easy_init();
	if (!curl)
		throw openai::Error("Could not initialize curl");

	std::string url = config.base_url;
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	url += "/chat/completions";

	std::string auth = "Authorization: Bearer " + config.api_key;
	curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, auth.c_str());

	std::string response;
	char error_buffer[CURL_ERROR_SIZE] = {0};

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "aegidub");
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);

	CURLcode res = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res == CURLE_ABORTED_BY_CALLBACK)
		throw openai::Error("Cancelled");
	if (res != CURLE_OK)
		throw openai::Error(std::string("Request failed: ") + (error_buffer[0] ? error_buffer : curl_easy_strerror(res)));
	if (status < 200 || status >= 300)
		throw openai::Error("API error " + std::to_string(status) + ": " + api_error_message(response));

	return response;
}

int element_to_int(json::UnknownElement const& el) {
	try { return static_cast<int>(static_cast<json::Integer const&>(el)); } catch (json::Exception const&) { }
	try { return static_cast<int>(static_cast<json::Double const&>(el)); } catch (json::Exception const&) { }
	return std::stoi(static_cast<json::String const&>(el));
}
}

namespace openai {
std::map<int, std::string> Translate(Config const& config,
	std::vector<Line> const& lines,
	std::vector<std::pair<std::string, std::string>> const& context,
	std::function<bool()> const& cancelled)
{
	std::map<int, std::string> result;
	if (lines.empty()) return result;

	std::string response = http_post(config, request_body(config, lines, context), cancelled);

	try {
		auto root = parse_json(response);
		json::Object const& root_obj = root;

		auto choices = find(root_obj, "choices");
		if (!choices) throw Error("Response has no choices");
		json::Array const& choices_arr = *choices;
		if (choices_arr.empty()) throw Error("Response has no choices");

		json::Object const& choice = choices_arr.front();
		auto message = find(choice, "message");
		if (!message) throw Error("Response has no message");
		json::Object const& message_obj = *message;
		auto content = find(message_obj, "content");
		if (!content) throw Error("Response has no content");

		auto content_root = parse_json(static_cast<json::String const&>(*content));
		json::Object const& content_obj = content_root;
		auto out_lines = find(content_obj, "lines");
		if (!out_lines) throw Error("Model reply has no \"lines\" array");

		for (auto const& entry : static_cast<json::Array const&>(*out_lines)) {
			try {
				json::Object const& entry_obj = entry;
				auto id = find(entry_obj, "id");
				auto text = find(entry_obj, "text");
				if (!id || !text) continue;
				result[element_to_int(*id)] = static_cast<json::String const&>(*text);
			}
			catch (std::exception const&) {
				// Skip malformed entries; the caller reports missing lines
			}
		}
	}
	catch (Error const&) {
		throw;
	}
	catch (std::exception const& e) {
		throw Error(std::string("Could not parse the API response: ") + e.what());
	}

	return result;
}
}
