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

/// @file json_util.h
/// @brief Small helpers for reading and writing JSON with cajun

#pragma once

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <sstream>
#include <string>

namespace json_util {
	/// Parse a JSON document
	/// @throws json::Exception on malformed input
	inline json::UnknownElement parse_json(std::string const& str) {
		std::istringstream ss(str);
		json::UnknownElement root;
		json::Reader::Read(root, ss);
		return root;
	}

	/// Serialise a JSON value
	inline std::string to_json(json::UnknownElement const& value) {
		std::ostringstream ss;
		agi::JsonWriter::Write(value, ss);
		return ss.str();
	}

	/// A member of an object, or nullptr if there is none
	inline const json::UnknownElement *find(json::Object const& obj, const char *key) {
		auto it = obj.find(key);
		return it == obj.end() ? nullptr : &it->second;
	}

	/// A string member of an object, or empty if missing or not a string
	inline std::string find_string(json::Object const& obj, const char *key) {
		auto el = find(obj, key);
		if (!el) return {};
		try { return static_cast<json::String const&>(*el); }
		catch (json::Exception const&) { return {}; }
	}
}
