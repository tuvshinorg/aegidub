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
#include "project_store.h"

#include "dub_settings.h"
#include "sha256.h"

#include <libaegisub/exception.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <wx/image.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace {
using json_util::find_string;
const char *file_name = "project.json";
const char *format_name = "aegidub-project";

DEFINE_EXCEPTION(ProjectError, agi::Exception);

int64_t unix_time(std::filesystem::file_time_type t) {
	auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
	return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

bool read(agi::fs::path const& folder, project_store::Project& project) {
	auto path = agi::fs::path(folder / file_name);
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	try {
		json::UnknownElement root;
		json::Reader::Read(root, in);
		json::Object const& obj = root;
		if (find_string(obj, "format") != format_name) return false;
		project.id = find_string(obj, "id");
		project.name = find_string(obj, "name");
		project.video = agi::fs::path(find_string(obj, "video"));
	}
	catch (...) {
		return false;
	}
	if (project.id.empty()) return false;
	project.folder = folder;

	// Last worked on: whichever was saved last
	std::error_code ec;
	project.modified = unix_time(std::filesystem::last_write_time(path, ec));
	auto subs = project.Subtitles();
	auto t = std::filesystem::last_write_time(subs, ec);
	if (!ec) project.modified = std::max(project.modified, unix_time(t));
	return true;
}
}

namespace project_store {
agi::fs::path Project::Subtitles() const {
	return agi::fs::path(folder / (id + ".ass"));
}

agi::fs::path Project::Thumbnail() const {
	return agi::fs::path(folder / "thumbnail.png");
}

std::string MakeId(agi::fs::path const& video, std::time_t day) {
	char date[16];
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &day);
#else
	localtime_r(&day, &tm);
#endif
	std::strftime(date, sizeof(date), "%Y%m%d", &tm);
	return std::string(date) + "-" + sha256::Hex(video.filename().string()).substr(0, 12);
}

std::vector<Project> List(agi::fs::path const& root) {
	std::vector<Project> projects;
	std::error_code ec;
	for (auto const& entry : std::filesystem::directory_iterator(root, ec)) {
		if (!entry.is_directory(ec)) continue;
		Project project;
		if (read(agi::fs::path(entry.path()), project))
			projects.push_back(std::move(project));
	}
	std::sort(projects.begin(), projects.end(), [](Project const& a, Project const& b) {
		return a.modified > b.modified;
	});
	return projects;
}

bool FindForSubtitles(agi::fs::path const& subtitles, Project& project) {
	if (subtitles.empty()) return false;
	return read(agi::fs::path(subtitles.parent_path()), project) && project.Subtitles() == subtitles;
}

void Write(Project const& project) {
	json::Object root;
	root.emplace("format", json::UnknownElement(format_name));
	root.emplace("version", json::UnknownElement(1));
	root.emplace("id", json::UnknownElement(project.id));
	root.emplace("name", json::UnknownElement(project.name));
	root.emplace("video", json::UnknownElement(project.video.string()));

	std::ostringstream ss;
	agi::JsonWriter::Write(json::UnknownElement(std::move(root)), ss);
	std::ofstream out(agi::fs::path(project.folder / file_name), std::ios::binary | std::ios::trunc);
	out << ss.str() << '\n';
	if (!out)
		throw ProjectError("Could not write the project file in " + project.folder.string());
}

bool MakeThumbnail(agi::fs::path const& video, agi::fs::path const& png, int width) {
	AVFormatContext *fmt = nullptr;
	if (avformat_open_input(&fmt, video.string().c_str(), nullptr, nullptr) < 0) return false;
	std::unique_ptr<AVFormatContext, void (*)(AVFormatContext *)> fmt_guard(fmt, [](AVFormatContext *f) { avformat_close_input(&f); });
	if (avformat_find_stream_info(fmt, nullptr) < 0) return false;

	const AVCodec *decoder = nullptr;
	int index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (index < 0 || !decoder) return false;
	AVStream *stream = fmt->streams[index];

	AVCodecContext *dec = avcodec_alloc_context3(decoder);
	std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)> dec_guard(dec, [](AVCodecContext *d) { avcodec_free_context(&d); });
	if (avcodec_parameters_to_context(dec, stream->codecpar) < 0 || avcodec_open2(dec, decoder, nullptr) < 0) return false;

	// A fifth of the way in skips logos and black openings
	if (fmt->duration > 0)
		av_seek_frame(fmt, -1, fmt->duration / 5, AVSEEK_FLAG_BACKWARD);

	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	bool got = false;
	for (int packets = 0; !got && packets < 2000 && av_read_frame(fmt, pkt) >= 0; ++packets) {
		if (pkt->stream_index == index && avcodec_send_packet(dec, pkt) >= 0)
			got = avcodec_receive_frame(dec, frame) >= 0;
		av_packet_unref(pkt);
	}
	av_packet_free(&pkt);

	bool saved = false;
	if (got && frame->width > 0 && frame->height > 0) {
		int height = std::max(1, frame->height * width / frame->width);
		SwsContext *sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
			width, height, AV_PIX_FMT_RGB24, SWS_BICUBIC, nullptr, nullptr, nullptr);
		if (sws) {
			wxImage image(width, height, false);
			uint8_t *dst[] = {image.GetData()};
			int stride[] = {width * 3};
			sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, stride);
			sws_freeContext(sws);
			saved = image.SaveFile(wxString::FromUTF8(png.string()), wxBITMAP_TYPE_PNG);
		}
	}
	av_frame_free(&frame);
	return saved;
}
}
