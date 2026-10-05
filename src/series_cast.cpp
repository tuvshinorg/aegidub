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
#include "series_cast.h"

#include "ass_file.h"
#include "options.h"

#include <libaegisub/exception.h>

#include <boost/algorithm/string/trim.hpp>
#include <fstream>
#include <regex>
#include <sstream>

namespace {
using json_util::find_string;
const std::string_view info_key = "Aegidub Series Cast";
const char *format_name = "aegidub-series-cast";

DEFINE_EXCEPTION(SeriesCastError, agi::Exception);

}

namespace series_cast {
agi::fs::path Find(const AssFile *file, agi::fs::path const& subs_path) {
	std::string linked(file->GetScriptInfo(info_key));
	if (!linked.empty()) {
		agi::fs::path path(linked);
		// Stored relative to the script so a moved project folder still works
		if (path.is_relative() && !subs_path.empty())
			path = agi::fs::path(subs_path.parent_path() / path);
		return path;
	}

	std::string last = OPT_GET("Tool/Voice Cast/Series File")->GetString();
	if (!last.empty() && agi::fs::FileExists(agi::fs::path(last)))
		return agi::fs::path(last);
	return {};
}

bool IsLinked(const AssFile *file) {
	return !file->GetScriptInfo(info_key).empty();
}

void Link(AssFile *file, agi::fs::path const& subs_path, agi::fs::path const& series_path) {
	std::string stored = series_path.string();
	if (!subs_path.empty()) {
		std::error_code ec;
		auto relative = std::filesystem::relative(series_path, subs_path.parent_path(), ec);
		if (!ec && !relative.empty())
			stored = agi::fs::path(relative).generic_string();
	}
	file->SetScriptInfo(info_key, stored);
	OPT_SET("Tool/Voice Cast/Series File")->SetString(series_path.string());
}

agi::fs::path SuggestPath(agi::fs::path const& subs_path) {
	std::string name = subs_path.stem().string();
	// Drop the episode part: "Show S01 E02-en", "Show.S01E02", "Show - 02"
	static const std::regex episode(R"([ ._-]*(S\d+[ ._-]*E\d+|E\d+|EP?\s*\d+|- *\d+)\b.*$)", std::regex::icase);
	std::string series = std::regex_replace(name, episode, "");
	boost::trim(series);
	if (series.empty()) series = name.empty() ? "series" : name;
	return agi::fs::path(subs_path.parent_path() / (series + ".cast.json"));
}

Cast Load(agi::fs::path const& path) {
	Cast cast;
	if (path.empty() || !agi::fs::FileExists(path)) return cast;

	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw SeriesCastError("Could not open " + path.string());

	try {
		json::UnknownElement root;
		json::Reader::Read(root, in);
		json::Object const& obj = root;
		if (find_string(obj, "format") != format_name)
			throw SeriesCastError(path.string() + " is not an aegidub series cast file");

		auto characters = obj.find("characters");
		if (characters == obj.end()) return cast;
		for (auto const& entry : static_cast<json::Object const&>(characters->second)) {
			try {
				json::Object const& voice = entry.second;
				Entry e{find_string(voice, "voice_id"), find_string(voice, "voice_name")};
				if (!entry.first.empty() && !e.voice_id.empty())
					cast[entry.first] = std::move(e);
			}
			catch (json::Exception const&) { }
		}
	}
	catch (json::Exception const& e) {
		throw SeriesCastError("Could not read " + path.string() + ": " + e.what());
	}
	return cast;
}

void Save(agi::fs::path const& path, Cast const& cast) {
	json::Object characters;
	for (auto const& entry : cast) {
		json::Object voice;
		voice.emplace("voice_id", json::UnknownElement(entry.second.voice_id));
		if (!entry.second.voice_name.empty())
			voice.emplace("voice_name", json::UnknownElement(entry.second.voice_name));
		characters.emplace(entry.first, json::UnknownElement(std::move(voice)));
	}

	json::Object root;
	root.emplace("format", json::UnknownElement(format_name));
	root.emplace("version", json::UnknownElement(1));
	root.emplace("characters", json::UnknownElement(std::move(characters)));

	std::ostringstream ss;
	agi::JsonWriter::Write(json::UnknownElement(std::move(root)), ss);

	// Write to a temporary name first so a failed write never destroys the
	// cast of a whole series
	agi::fs::path tmp(path.string() + ".part");
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out << ss.str() << '\n';
		if (!out)
			throw SeriesCastError("Could not write " + tmp.string());
	}
	agi::fs::Rename(tmp, path);
}
}
