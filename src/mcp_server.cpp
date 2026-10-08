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

// Asio must come before anything that includes windows.h
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
// Boost.JSON is used header-only; this compiles it into this file
#include <boost/json/src.hpp>

#include "mcp_server.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "hardsub_extractor.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "soft_subs.h"
#include "subs_controller.h"
#include "version.h"
#include "video_controller.h"
#include "video_frame.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/path.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include <wx/base64.h>
#include <wx/image.h>
#include <wx/intl.h>
#include <wx/mstream.h>

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace js = boost::json;
using tcp = asio::ip::tcp;

/// A tool failed in a way the agent should be told about
struct ToolError : std::runtime_error {
	using std::runtime_error::runtime_error;
};

/// A JSON-RPC level error
struct RpcError : std::runtime_error {
	int code;
	RpcError(int code, std::string const& message) : std::runtime_error(message), code(code) { }
};

agi::Context *context = nullptr;
std::atomic<bool> stopping{false};
std::string last_error;

// ---------------------------------------------------------------------------
// Running things on the GUI thread

/// Run fn on the GUI thread and wait for its result
js::value on_main(std::function<js::value()> fn) {
	auto promise = std::make_shared<std::promise<js::value>>();
	auto future = promise->get_future();
	agi::dispatch::Main().Async([promise, fn = std::move(fn)] {
		if (stopping) {
			promise->set_exception(std::make_exception_ptr(ToolError("aegidub is closing")));
			return;
		}
		try {
			promise->set_value(fn());
		}
		catch (agi::Exception const& e) {
			promise->set_exception(std::make_exception_ptr(ToolError(e.GetMessage())));
		}
		catch (...) {
			promise->set_exception(std::current_exception());
		}
	});
	// Don't wait on a GUI thread that is busy shutting this server down
	while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
		if (stopping) throw ToolError("aegidub is closing");
	}
	return future.get();
}

// ---------------------------------------------------------------------------
// Arguments

js::value const *arg(js::object const& args, const char *name) {
	auto v = args.if_contains(name);
	return v && !v->is_null() ? v : nullptr;
}

std::string string_arg(js::object const& args, const char *name, std::string const& def = "") {
	auto v = arg(args, name);
	if (!v) return def;
	if (!v->is_string()) throw ToolError(std::string(name) + " must be a string");
	return std::string(v->as_string());
}

double number_arg(js::object const& args, const char *name, double def) {
	auto v = arg(args, name);
	if (!v) return def;
	if (v->is_int64()) return (double)v->as_int64();
	if (v->is_uint64()) return (double)v->as_uint64();
	if (v->is_double()) return v->as_double();
	throw ToolError(std::string(name) + " must be a number");
}

int64_t int_arg(js::object const& args, const char *name, int64_t def) {
	return (int64_t)std::llround(number_arg(args, name, (double)def));
}

bool bool_arg(js::object const& args, const char *name, bool def) {
	auto v = arg(args, name);
	if (!v) return def;
	if (!v->is_bool()) throw ToolError(std::string(name) + " must be true or false");
	return v->as_bool();
}

js::array const& array_arg(js::object const& args, const char *name) {
	auto v = arg(args, name);
	if (!v || !v->is_array()) throw ToolError(std::string(name) + " must be an array");
	return v->as_array();
}

// ---------------------------------------------------------------------------
// Subtitle lines

std::vector<AssDialogue *> all_lines() {
	std::vector<AssDialogue *> lines;
	for (auto& line : context->ass->Events)
		lines.push_back(&line);
	return lines;
}

js::object line_json(AssDialogue const& line, size_t index) {
	return {
		{"index", index},
		{"start_ms", (int)line.Start},
		{"end_ms", (int)line.End},
		{"style", line.Style.get()},
		{"actor", line.Actor.get()},
		{"text", line.Text.get()},
		{"comment", line.Comment},
	};
}

AssDialogue *line_at(std::vector<AssDialogue *> const& lines, js::value const& index) {
	if (!index.is_int64() && !index.is_uint64())
		throw ToolError("index must be a whole number");
	int64_t i = index.to_number<int64_t>();
	if (i < 0 || i >= (int64_t)lines.size())
		throw ToolError("There is no line " + std::to_string(i) + "; there are " + std::to_string(lines.size()));
	return lines[i];
}

/// Set the fields an edit or insert gives
void apply_fields(AssDialogue *line, js::object const& fields) {
	if (fields.if_contains("start_ms")) line->Start = (int)int_arg(fields, "start_ms", 0);
	if (fields.if_contains("end_ms")) line->End = (int)int_arg(fields, "end_ms", 0);
	if (fields.if_contains("text")) line->Text = string_arg(fields, "text");
	if (fields.if_contains("actor")) line->Actor = string_arg(fields, "actor");
	if (fields.if_contains("style")) line->Style = string_arg(fields, "style", "Default");
	if (fields.if_contains("comment")) line->Comment = bool_arg(fields, "comment", false);
	if (line->End < line->Start)
		throw ToolError("end_ms is before start_ms");
}

// ---------------------------------------------------------------------------
// Long-running jobs, which agents poll with get_job

struct Job {
	std::string id;
	std::string kind;
	std::atomic<bool> cancel{false};
	std::thread thread;

	std::mutex mutex;
	std::string status = "running";
	std::string message;
	double progress = -1;
	std::deque<std::string> log;
	js::value result;
	std::string error;

	js::object Json() {
		std::lock_guard<std::mutex> lock(mutex);
		js::object o{{"job_id", id}, {"kind", kind}, {"status", status}, {"message", message}};
		o["progress"] = progress < 0 ? js::value(nullptr) : js::value(progress);
		if (!log.empty()) {
			js::array lines;
			for (auto const& line : log) lines.push_back(js::string(line));
			o["log_tail"] = std::move(lines);
		}
		if (!result.is_null()) o["result"] = result;
		if (!error.empty()) o["error"] = error;
		return o;
	}

	void Progress(std::string const& msg, double fraction) {
		std::lock_guard<std::mutex> lock(mutex);
		message = msg;
		progress = fraction;
	}

	void Log(std::string const& line) {
		std::lock_guard<std::mutex> lock(mutex);
		log.push_back(line);
		while (log.size() > 15) log.pop_front();
	}

	void Finish(std::string const& new_status, js::value res, std::string const& err) {
		std::lock_guard<std::mutex> lock(mutex);
		status = new_status;
		result = std::move(res);
		error = err;
		if (new_status == "done") progress = 1;
	}

	bool Running() {
		std::lock_guard<std::mutex> lock(mutex);
		return status == "running";
	}
};

std::mutex jobs_mutex;
std::map<std::string, std::shared_ptr<Job>> jobs;
int next_job = 1;

std::shared_ptr<Job> start_job(std::string const& kind, std::function<js::value(Job&)> work) {
	std::lock_guard<std::mutex> lock(jobs_mutex);
	for (auto const& [id, job] : jobs) {
		if (job->kind == kind && job->Running())
			throw ToolError("A " + kind + " job is already running: " + id);
	}
	auto job = std::make_shared<Job>();
	job->id = kind + "-" + std::to_string(next_job++);
	job->kind = kind;
	jobs[job->id] = job;
	job->thread = std::thread([job, work = std::move(work)] {
		try {
			js::value result = work(*job);
			job->Finish("done", std::move(result), "");
		}
		catch (hardsub_extractor::Cancelled const&) { job->Finish("cancelled", nullptr, ""); }
		catch (soft_subs::Cancelled const&) { job->Finish("cancelled", nullptr, ""); }
		catch (agi::Exception const& e) { job->Finish("failed", nullptr, e.GetMessage()); }
		catch (std::exception const& e) { job->Finish("failed", nullptr, e.what()); }
	});
	return job;
}

std::shared_ptr<Job> find_job(std::string const& id) {
	std::lock_guard<std::mutex> lock(jobs_mutex);
	auto it = jobs.find(id);
	if (it == jobs.end()) throw ToolError("No job " + id);
	return it->second;
}

void stop_jobs() {
	std::map<std::string, std::shared_ptr<Job>> old;
	{
		std::lock_guard<std::mutex> lock(jobs_mutex);
		old.swap(jobs);
	}
	for (auto& [id, job] : old) job->cancel = true;
	for (auto& [id, job] : old) {
		if (job->thread.joinable()) job->thread.join();
	}
}

// ---------------------------------------------------------------------------
// Tools

struct ToolResult {
	js::value data;
	/// A PNG to return as an image, base64 encoded
	std::string png_base64;
};

struct Tool {
	const char *name;
	const char *description;
	const char *schema;
	std::function<ToolResult(js::object const&)> run;
};

js::value video_info() {
	return on_main([]() -> js::value {
		js::object o;
		o["subtitles_file"] = context->subsController->Filename().string();
		o["subtitles_modified"] = context->subsController->IsModified();
		o["line_count"] = context->ass->Events.size();
		auto provider = context->project->VideoProvider();
		if (provider) {
			auto const& fps = context->project->Timecodes();
			int frames = provider->GetFrameCount();
			o["video_file"] = context->project->VideoName().string();
			o["video_width"] = provider->GetWidth();
			o["video_height"] = provider->GetHeight();
			o["video_duration_ms"] = fps.TimeAtFrame(frames);
			o["video_position_ms"] = fps.TimeAtFrame(context->videoController->GetFrameN());
			o["fps"] = fps.FPS();
		}
		else
			o["video_file"] = nullptr;
		return o;
	});
}

std::vector<Tool> make_tools() {
	std::vector<Tool> tools;

	tools.push_back({"get_status",
		"What is open in aegidub: the subtitle file, whether it has unsaved changes, how many lines it has, and the video's file, size, length, frame rate and current position. Also lists recent jobs.",
		R"JSON({"type":"object","properties":{}})JSON",
		[](js::object const&) {
			js::object o = video_info().as_object();
			js::array list;
			std::lock_guard<std::mutex> lock(jobs_mutex);
			for (auto const& [id, job] : jobs) list.push_back(job->Json());
			o["jobs"] = std::move(list);
			return ToolResult{o, ""};
		}});

	tools.push_back({"list_lines",
		"List subtitle lines with their index, times in milliseconds, style, actor, text and whether they are comments. Text uses ASS override tags such as {\\i1} and \\N for line breaks. Use offset and limit to page through long files.",
		R"JSON({"type":"object","properties":{"offset":{"type":"integer","minimum":0,"default":0},"limit":{"type":"integer","minimum":1,"maximum":2000,"default":300}}})JSON",
		[](js::object const& args) {
			int64_t offset = std::max<int64_t>(0, int_arg(args, "offset", 0));
			int64_t limit = std::clamp<int64_t>(int_arg(args, "limit", 300), 1, 2000);
			return ToolResult{on_main([=]() -> js::value {
				auto lines = all_lines();
				js::array out;
				for (int64_t i = offset; i < (int64_t)lines.size() && i < offset + limit; ++i)
					out.push_back(line_json(*lines[i], i));
				return js::object{{"total", lines.size()}, {"offset", offset}, {"lines", std::move(out)}};
			}), ""};
		}});

	tools.push_back({"edit_lines",
		"Change existing lines, given by index. Only the fields given are changed. All edits are one undo step in aegidub.",
		R"JSON({"type":"object","required":["edits"],"properties":{"edits":{"type":"array","items":{"type":"object","required":["index"],"properties":{"index":{"type":"integer"},"text":{"type":"string"},"start_ms":{"type":"integer"},"end_ms":{"type":"integer"},"actor":{"type":"string"},"style":{"type":"string"},"comment":{"type":"boolean"}}}}}})JSON",
		[](js::object const& args) {
			js::array edits = array_arg(args, "edits");
			return ToolResult{on_main([edits]() -> js::value {
				auto lines = all_lines();
				// Check everything before changing anything
				std::vector<std::pair<AssDialogue *, js::object const *>> todo;
				for (auto const& edit : edits) {
					if (!edit.is_object()) throw ToolError("Each edit must be an object");
					auto index = edit.as_object().if_contains("index");
					if (!index) throw ToolError("Each edit needs an index");
					todo.emplace_back(line_at(lines, *index), &edit.as_object());
				}
				for (auto& [line, fields] : todo) {
					AssDialogue check(*line);
					apply_fields(&check, *fields);
				}
				for (auto& [line, fields] : todo)
					apply_fields(line, *fields);
				if (!todo.empty())
					context->ass->Commit(_("MCP: edit lines"), AssFile::COMMIT_DIAG_FULL);
				return js::object{{"edited", todo.size()}};
			}), ""};
		}});

	tools.push_back({"insert_lines",
		"Add new lines before the line at before_index, or at the end when before_index is left out. Returns the index of the first new line.",
		R"JSON({"type":"object","required":["lines"],"properties":{"before_index":{"type":"integer"},"lines":{"type":"array","items":{"type":"object","required":["start_ms","end_ms","text"],"properties":{"start_ms":{"type":"integer"},"end_ms":{"type":"integer"},"text":{"type":"string"},"actor":{"type":"string"},"style":{"type":"string"},"comment":{"type":"boolean"}}}}}})JSON",
		[](js::object const& args) {
			js::array new_lines = array_arg(args, "lines");
			auto before = arg(args, "before_index");
			std::optional<js::value> before_index;
			if (before) before_index = *before;
			return ToolResult{on_main([new_lines, before_index]() -> js::value {
				auto lines = all_lines();
				AssDialogue *before_line = before_index ? line_at(lines, *before_index) : nullptr;
				std::vector<std::unique_ptr<AssDialogue>> made;
				for (auto const& fields : new_lines) {
					if (!fields.is_object()) throw ToolError("Each line must be an object");
					auto line = std::make_unique<AssDialogue>();
					apply_fields(line.get(), fields.as_object());
					made.push_back(std::move(line));
				}
				auto pos = before_line ? context->ass->iterator_to(*before_line) : context->ass->Events.end();
				AssDialogue *first = made.empty() ? nullptr : made.front().get();
				for (auto& line : made)
					context->ass->Events.insert(pos, *line.release());
				if (first) {
					context->ass->Commit(_("MCP: insert lines"), AssFile::COMMIT_DIAG_ADDREM);
					context->selectionController->SetSelectionAndActive({first}, first);
				}
				int64_t first_index = before_index ? before_index->to_number<int64_t>() : (int64_t)lines.size();
				return js::object{{"inserted", new_lines.size()}, {"first_index", first_index}};
			}), ""};
		}});

	tools.push_back({"delete_lines",
		"Delete lines by index. Indexes of the lines after them shift down.",
		R"JSON({"type":"object","required":["indices"],"properties":{"indices":{"type":"array","items":{"type":"integer"}}}})JSON",
		[](js::object const& args) {
			js::array indices = array_arg(args, "indices");
			return ToolResult{on_main([indices]() -> js::value {
				auto lines = all_lines();
				std::set<AssDialogue *> doomed;
				for (auto const& index : indices)
					doomed.insert(line_at(lines, index));
				if (doomed.empty()) return js::object{{"deleted", 0}};

				// As in aegidub's own delete: select a surviving line before
				// freeing the deleted ones, which the selection may point at
				AssDialogue *keep = nullptr;
				for (auto line : lines) {
					if (!doomed.count(line)) { keep = line; break; }
				}
				std::vector<std::unique_ptr<AssDialogue>> removed;
				context->ass->Events.remove_and_dispose_if(
					[&](AssDialogue const& e) { return doomed.count(const_cast<AssDialogue *>(&e)) > 0; },
					[&](AssDialogue *e) { removed.emplace_back(e); });
				if (!keep) {
					keep = new AssDialogue;
					context->ass->Events.push_back(*keep);
				}
				context->ass->Commit(_("MCP: delete lines"), AssFile::COMMIT_DIAG_ADDREM);
				context->selectionController->SetSelectionAndActive({keep}, keep);
				return js::object{{"deleted", removed.size()}};
			}), ""};
		}});

	tools.push_back({"save_subtitles",
		"Save the subtitles, to their current file or to path. The format follows the extension (.ass, .srt and others).",
		R"JSON({"type":"object","properties":{"path":{"type":"string","description":"Absolute path; leave out to save to the current file"}}})JSON",
		[](js::object const& args) {
			std::string path = string_arg(args, "path");
			return ToolResult{on_main([path]() -> js::value {
				agi::fs::path target = path.empty() ? context->subsController->Filename() : agi::fs::path(path);
				if (target.empty())
					throw ToolError("The subtitles have never been saved; give a path");
				context->subsController->Save(target);
				return js::object{{"saved", target.string()}};
			}), ""};
		}});

	tools.push_back({"open_video",
		"Open a video file in aegidub.",
		R"JSON({"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Absolute path"}}})JSON",
		[](js::object const& args) {
			std::string path = string_arg(args, "path");
			if (!agi::fs::FileExists(path)) throw ToolError("No such file: " + path);
			on_main([path]() -> js::value { context->project->LoadVideo(path); return nullptr; });
			return ToolResult{video_info(), ""};
		}});

	tools.push_back({"open_subtitles",
		"Open a subtitle file in aegidub. If the open subtitles have unsaved changes, aegidub asks the user first.",
		R"JSON({"type":"object","required":["path"],"properties":{"path":{"type":"string","description":"Absolute path"}}})JSON",
		[](js::object const& args) {
			std::string path = string_arg(args, "path");
			if (!agi::fs::FileExists(path)) throw ToolError("No such file: " + path);
			on_main([path]() -> js::value {
				if (context->subsController->TryToClose() == wxCANCEL)
					throw ToolError("The user chose to keep the current subtitles open");
				context->project->LoadSubtitles(path);
				return nullptr;
			});
			return ToolResult{video_info(), ""};
		}});

	tools.push_back({"seek_video",
		"Move the video to a time, in milliseconds.",
		R"JSON({"type":"object","required":["time_ms"],"properties":{"time_ms":{"type":"integer","minimum":0}}})JSON",
		[](js::object const& args) {
			int ms = (int)int_arg(args, "time_ms", 0);
			on_main([ms]() -> js::value {
				if (!context->project->VideoProvider()) throw ToolError("No video is open");
				context->videoController->JumpToTime(ms);
				return nullptr;
			});
			return ToolResult{video_info(), ""};
		}});

	tools.push_back({"get_video_frame",
		"Get a picture of a video frame without subtitles drawn on it, for example to see where burned-in subtitles are before extracting them.",
		R"JSON({"type":"object","properties":{"time_ms":{"type":"integer","minimum":0,"description":"Leave out for the current frame"},"max_width":{"type":"integer","minimum":64,"maximum":1920,"default":960}}})JSON",
		[](js::object const& args) {
			auto time = arg(args, "time_ms");
			int ms = time ? (int)int_arg(args, "time_ms", 0) : -1;
			int max_width = (int)std::clamp<int64_t>(int_arg(args, "max_width", 960), 64, 1920);
			js::value info = on_main([ms, max_width]() -> js::value {
				auto provider = context->project->VideoProvider();
				if (!provider) throw ToolError("No video is open");
				auto const& fps = context->project->Timecodes();
				int frame = ms < 0 ? context->videoController->GetFrameN() : fps.FrameAtTime(ms);
				frame = std::clamp(frame, 0, provider->GetFrameCount() - 1);
				wxImage image = GetImage(*provider->GetFrame(frame, fps.TimeAtFrame(frame), true));
				if (image.GetWidth() > max_width)
					image = image.Scale(max_width, image.GetHeight() * max_width / image.GetWidth(), wxIMAGE_QUALITY_HIGH);
				wxMemoryOutputStream out;
				image.SaveFile(out, wxBITMAP_TYPE_PNG);
				std::vector<char> png(out.GetSize());
				out.CopyTo(png.data(), png.size());
				return js::object{
					{"time_ms", fps.TimeAtFrame(frame)},
					{"frame", frame},
					{"width", image.GetWidth()},
					{"height", image.GetHeight()},
					{"png", std::string(wxBase64Encode(png.data(), png.size()).utf8_str())},
				};
			});
			auto& o = info.as_object();
			std::string png(o["png"].as_string());
			o.erase("png");
			return ToolResult{info, png};
		}});

	tools.push_back({"extract_burned_in_subtitles",
		"Read subtitles burned into the open video's picture with OCR (video-subtitle-extractor) and put them in the subtitle file. Starts a job and returns its job_id; poll get_job until it finishes. The first run downloads and installs the OCR engine, which takes about 10 minutes. The area is where the subtitles appear, as fractions of the frame from the top-left corner; use get_video_frame to see it. OCR makes mistakes, so read the result through.",
		R"JSON({"type":"object","properties":{"language":{"type":"string","description":"video-subtitle-extractor language code: en, ch, chinese_cht, japan, korean, mn, ru, uk, ar, fa, hi, ta, te, kn, de, fr, es, pt, it, nl, pl, tr, vi, id, ms, ..."},"area":{"type":"object","properties":{"top":{"type":"number"},"bottom":{"type":"number"},"left":{"type":"number"},"right":{"type":"number"}}},"use_gpu":{"type":"boolean"},"replace":{"type":"boolean","default":true,"description":"Replace the current lines; false adds after them"}}})JSON",
		[](js::object const& args) {
			hardsub_extractor::Settings s;
			s.language = string_arg(args, "language", OPT_GET("Tool/Hardsub Extract/Language")->GetString());
			s.gpu = bool_arg(args, "use_gpu", OPT_GET("Tool/Hardsub Extract/Use GPU If Available")->GetBool());
			s.top = OPT_GET("Tool/Hardsub Extract/Area Top")->GetDouble();
			s.bottom = OPT_GET("Tool/Hardsub Extract/Area Bottom")->GetDouble();
			s.left = OPT_GET("Tool/Hardsub Extract/Area Left")->GetDouble();
			s.right = OPT_GET("Tool/Hardsub Extract/Area Right")->GetDouble();
			if (auto area = arg(args, "area")) {
				if (!area->is_object()) throw ToolError("area must be an object");
				auto const& a = area->as_object();
				s.top = number_arg(a, "top", s.top);
				s.bottom = number_arg(a, "bottom", s.bottom);
				s.left = number_arg(a, "left", s.left);
				s.right = number_arg(a, "right", s.right);
			}
			if (!(0 <= s.top && s.top < s.bottom && s.bottom <= 1 && 0 <= s.left && s.left < s.right && s.right <= 1))
				throw ToolError("area must have 0 <= top < bottom <= 1 and 0 <= left < right <= 1");
			bool replace = bool_arg(args, "replace", true);

			s.video = std::string(on_main([]() -> js::value {
				if (!context->project->VideoProvider()) throw ToolError("No video is open");
				return js::string(context->project->VideoName().string());
			}).as_string());
			s.output = config::path->Decode("?local/vse/work/extracted-mcp.srt");

			auto job = start_job("extract", [s, replace](Job& job) -> js::value {
				hardsub_extractor::Extract(s,
					[&](std::string const& message, double fraction) { job.Progress(message, fraction); },
					[&](std::string const& line) { job.Log(line); },
					[&] { return job.cancel.load(); });
				job.Progress("Adding the lines to the subtitles...", -1);
				auto path = s.output;
				return on_main([path, replace]() -> js::value {
					AssFile lines;
					hardsub_extractor::ReadLines(context, path, lines);
					size_t added = hardsub_extractor::ImportLines(context, lines, replace);
					agi::fs::Remove(path);
					return js::object{{"lines_added", added}, {"replaced", replace}};
				});
			});
			return ToolResult{job->Json(), ""};
		}});

	tools.push_back({"export_soft_subtitles",
		"Write a copy of the open video with the current subtitles as a track that can be switched on and off. The picture and sound are copied without re-encoding. .mkv keeps ASS styling; .mp4 gets plain mov_text. Starts a job and returns its job_id; poll get_job until it finishes.",
		R"JSON({"type":"object","properties":{"output_path":{"type":"string","description":"Absolute path ending in .mkv or .mp4; defaults to the video's name with .softsub.mkv"},"language":{"type":"string","description":"ISO 639-2 code for the track, e.g. mon or eng"},"keep_existing":{"type":"boolean","description":"Keep the video's existing subtitle tracks (MKV only)"}}})JSON",
		[](js::object const& args) {
			soft_subs::Settings s;
			s.language = string_arg(args, "language", OPT_GET("Tool/Soft Subtitles/Language")->GetString());
			s.keep_existing = bool_arg(args, "keep_existing", OPT_GET("Tool/Soft Subtitles/Keep Existing")->GetBool());
			std::string output = string_arg(args, "output_path");
			int id = 0;
			{
				std::lock_guard<std::mutex> lock(jobs_mutex);
				id = next_job;
			}
			s.subtitles = config::path->Decode("?temp/aegidub-mcp-softsub-" + std::to_string(id) + ".ass");

			s.video = std::string(on_main([subtitles = s.subtitles]() -> js::value {
				if (context->project->VideoName().empty()) throw ToolError("No video is open");
				soft_subs::WriteSubtitles(context, subtitles);
				return js::string(context->project->VideoName().string());
			}).as_string());
			s.output = output.empty()
				? agi::fs::path(s.video.parent_path() / (s.video.stem().string() + ".softsub.mkv"))
				: agi::fs::path(output);
			if (!agi::fs::HasExtension(s.output, "mkv") && !agi::fs::HasExtension(s.output, "mp4")) {
				agi::fs::Remove(s.subtitles);
				throw ToolError("output_path must end in .mkv or .mp4");
			}
			if (s.output == s.video) {
				agi::fs::Remove(s.subtitles);
				throw ToolError("output_path must not be the video itself");
			}

			auto job = start_job("export", [s](Job& job) -> js::value {
				struct Cleanup {
					agi::fs::path path;
					~Cleanup() { agi::fs::Remove(path); }
				} cleanup{s.subtitles};
				job.Progress("Writing the video...", 0);
				soft_subs::Export(s, [&](double f) { job.Progress("Writing the video...", f); },
					[&] { return job.cancel.load(); });
				return js::object{{"output", s.output.string()}};
			});
			return ToolResult{job->Json(), ""};
		}});

	tools.push_back({"get_job",
		"Check on a job started by extract_burned_in_subtitles or export_soft_subtitles: status is running, done, failed or cancelled. Waits up to wait_seconds for it to finish first.",
		R"JSON({"type":"object","required":["job_id"],"properties":{"job_id":{"type":"string"},"wait_seconds":{"type":"integer","minimum":0,"maximum":30,"default":10}}})JSON",
		[](js::object const& args) {
			auto job = find_job(string_arg(args, "job_id"));
			auto wait = std::chrono::seconds(std::clamp<int64_t>(int_arg(args, "wait_seconds", 10), 0, 30));
			auto until = std::chrono::steady_clock::now() + wait;
			while (job->Running() && std::chrono::steady_clock::now() < until && !stopping)
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			return ToolResult{job->Json(), ""};
		}});

	tools.push_back({"cancel_job",
		"Stop a running job.",
		R"JSON({"type":"object","required":["job_id"],"properties":{"job_id":{"type":"string"}}})JSON",
		[](js::object const& args) {
			auto job = find_job(string_arg(args, "job_id"));
			job->cancel = true;
			return ToolResult{job->Json(), ""};
		}});

	return tools;
}

std::vector<Tool> const& tools() {
	static const std::vector<Tool> list = make_tools();
	return list;
}

// ---------------------------------------------------------------------------
// MCP over JSON-RPC

const char *protocol_versions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

js::value call_tool(js::object const& params) {
	auto name = params.if_contains("name");
	if (!name || !name->is_string()) throw RpcError(-32602, "Missing tool name");
	auto it = std::find_if(tools().begin(), tools().end(), [&](Tool const& t) { return name->as_string() == t.name; });
	if (it == tools().end()) throw RpcError(-32602, "Unknown tool: " + std::string(name->as_string()));

	js::object args;
	if (auto a = params.if_contains("arguments"); a && a->is_object())
		args = a->as_object();

	js::array content;
	bool is_error = false;
	try {
		ToolResult result = it->run(args);
		content.push_back(js::object{{"type", "text"}, {"text", js::serialize(result.data)}});
		if (!result.png_base64.empty())
			content.push_back(js::object{{"type", "image"}, {"data", result.png_base64}, {"mimeType", "image/png"}});
	}
	catch (agi::Exception const& e) {
		content.push_back(js::object{{"type", "text"}, {"text", e.GetMessage()}});
		is_error = true;
	}
	catch (std::exception const& e) {
		content.push_back(js::object{{"type", "text"}, {"text", e.what()}});
		is_error = true;
	}
	return js::object{{"content", std::move(content)}, {"isError", is_error}};
}

js::value call_method(std::string const& method, js::object const& params) {
	if (method == "initialize") {
		std::string version = protocol_versions[0];
		if (auto v = params.if_contains("protocolVersion"); v && v->is_string()) {
			for (auto supported : protocol_versions) {
				if (v->as_string() == supported) version = supported;
			}
		}
		return js::object{
			{"protocolVersion", version},
			{"capabilities", {{"tools", {{"listChanged", false}}}}},
			{"serverInfo", {{"name", "aegidub"}, {"version", GetAegisubShortVersionString()}}},
			{"instructions", "aegidub is a subtitle editor for dubbing. These tools act on the file and video open in the running aegidub window, which the user may be editing too. Line indexes come from list_lines and change when lines are inserted or deleted, so list again after doing either. Times are in milliseconds."},
		};
	}
	if (method == "ping")
		return js::object{};
	if (method == "tools/list") {
		js::array list;
		for (auto const& tool : tools()) {
			list.push_back(js::object{
				{"name", tool.name},
				{"description", tool.description},
				{"inputSchema", js::parse(tool.schema)},
			});
		}
		return js::object{{"tools", std::move(list)}};
	}
	if (method == "tools/call")
		return call_tool(params);
	throw RpcError(-32601, "Unknown method: " + method);
}

/// @return The response, or null for a notification
js::value handle_message(js::value const& message) {
	if (!message.is_object())
		return js::object{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32600}, {"message", "Invalid request"}}}};
	auto const& msg = message.as_object();
	auto id = msg.if_contains("id");
	auto method = msg.if_contains("method");
	// Notifications and the client's replies need no answer
	if (!id || !method || !method->is_string())
		return nullptr;

	js::object response{{"jsonrpc", "2.0"}, {"id", *id}};
	try {
		js::object params;
		if (auto p = msg.if_contains("params"); p && p->is_object())
			params = p->as_object();
		response["result"] = call_method(std::string(method->as_string()), params);
	}
	catch (RpcError const& e) {
		response["error"] = {{"code", e.code}, {"message", e.what()}};
	}
	catch (std::exception const& e) {
		response["error"] = {{"code", -32603}, {"message", e.what()}};
	}
	return response;
}

// ---------------------------------------------------------------------------
// HTTP

/// Is this host name, with or without a port, this computer?
bool is_local_host(std::string_view host) {
	if (host.substr(0, 5) == "[::1]") return true;
	host = host.substr(0, host.find(':'));
	return host == "127.0.0.1" || host == "localhost";
}

http::response<http::string_body> handle_request(http::request<http::string_body> const& req) {
	http::response<http::string_body> res{http::status::ok, req.version()};
	res.set(http::field::server, "aegidub");
	res.keep_alive(req.keep_alive());
	auto reply = [&](http::status status, std::string body, const char *type = "application/json") {
		res.result(status);
		if (!body.empty()) res.set(http::field::content_type, type);
		res.body() = std::move(body);
		res.prepare_payload();
		return res;
	};

	auto target = req.target();
	target = target.substr(0, target.find('?'));
	if (target != "/mcp" && target != "/mcp/")
		return reply(http::status::not_found, "Not found", "text/plain");

	// Web pages must not be able to reach the server through the browser,
	// whether directly or by pointing their own host name at this computer
	auto origin = req[http::field::origin];
	if (!is_local_host(req[http::field::host]) ||
		(!origin.empty() && !(origin.starts_with("http://") && is_local_host(origin.substr(7)))))
		return reply(http::status::forbidden, "Forbidden", "text/plain");

	if (req.method() != http::verb::post) {
		res.set(http::field::allow, "POST");
		return reply(http::status::method_not_allowed, "");
	}

	boost::system::error_code ec;
	js::value body = js::parse(req.body(), ec);
	if (ec) {
		return reply(http::status::bad_request, js::serialize(js::object{
			{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "Parse error"}}}}));
	}

	js::value response;
	if (body.is_array()) {
		js::array responses;
		for (auto const& message : body.as_array()) {
			auto r = handle_message(message);
			if (!r.is_null()) responses.push_back(std::move(r));
		}
		if (!responses.empty()) response = std::move(responses);
	}
	else
		response = handle_message(body);

	if (response.is_null())
		return reply(http::status::accepted, "");
	return reply(http::status::ok, js::serialize(response));
}

class Session : public std::enable_shared_from_this<Session> {
	beast::tcp_stream stream;
	beast::flat_buffer buffer;
	std::optional<http::request_parser<http::string_body>> parser;
	http::response<http::string_body> response;

public:
	explicit Session(tcp::socket&& socket) : stream(std::move(socket)) { }

	void Read() {
		parser.emplace();
		parser->body_limit(16 * 1024 * 1024);
		stream.expires_after(std::chrono::minutes(30));
		http::async_read(stream, buffer, *parser, [self = shared_from_this()](beast::error_code ec, size_t) {
			self->OnRead(ec);
		});
	}

	void OnRead(beast::error_code ec) {
		if (ec) {
			stream.socket().shutdown(tcp::socket::shutdown_send, ec);
			return;
		}
		response = handle_request(parser->get());
		stream.expires_after(std::chrono::minutes(1));
		http::async_write(stream, response, [self = shared_from_this()](beast::error_code ec, size_t) {
			if (ec) return;
			if (!self->response.keep_alive()) {
				self->stream.socket().shutdown(tcp::socket::shutdown_send, ec);
				return;
			}
			self->Read();
		});
	}
};

struct Server {
	asio::io_context io;
	tcp::acceptor acceptor{io};
	std::vector<std::thread> threads;
	int port = 0;

	void Accept() {
		acceptor.async_accept([this](beast::error_code ec, tcp::socket socket) {
			if (ec) return;
			std::make_shared<Session>(std::move(socket))->Read();
			Accept();
		});
	}
};

std::unique_ptr<Server> server;

void start(int port) {
	auto s = std::make_unique<Server>();
	s->port = port;
	boost::system::error_code ec;
	tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), (unsigned short)port);
	s->acceptor.open(endpoint.protocol(), ec);
	if (!ec) s->acceptor.bind(endpoint, ec);
	if (!ec) s->acceptor.listen(asio::socket_base::max_listen_connections, ec);
	if (ec) {
		last_error = "Could not listen on port " + std::to_string(port) + ": " + ec.message();
		return;
	}

	last_error.clear();
	stopping = false;
	s->Accept();
	// Tool calls wait on the GUI thread or on jobs, so serve a few at once
	for (int i = 0; i < 4; ++i)
		s->threads.emplace_back([io = &s->io] { io->run(); });
	server = std::move(s);
}
}

namespace mcp_server {
void Update(agi::Context *c) {
	context = c;
	bool enabled = OPT_GET("Tool/MCP Server/Enabled")->GetBool();
	int port = (int)OPT_GET("Tool/MCP Server/Port")->GetInt();
	if (server && (!enabled || server->port != port))
		Stop();
	if (enabled && !server)
		start(port);
	if (!enabled)
		last_error.clear();
}

void Stop() {
	if (!server) return;
	stopping = true;
	server->io.stop();
	for (auto& t : server->threads) t.join();
	server.reset();
	stop_jobs();
}

bool Running() {
	return !!server;
}

std::string Url() {
	return "http://127.0.0.1:" + std::to_string(OPT_GET("Tool/MCP Server/Port")->GetInt()) + "/mcp";
}

std::string LastError() {
	return last_error;
}
}
