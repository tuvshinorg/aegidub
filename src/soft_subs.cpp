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

#include "soft_subs.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "include/aegisub/context.h"
#include "project.h"
#include "subtitle_format.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using soft_subs::Error;

std::string av_error(int err) {
	char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

void check(int err, const char *what) {
	if (err < 0)
		throw Error(std::string(what) + ": " + av_error(err));
}

struct FormatInput {
	AVFormatContext *ctx = nullptr;
	~FormatInput() { avformat_close_input(&ctx); }
};
struct CodecContext {
	AVCodecContext *ctx = nullptr;
	~CodecContext() { avcodec_free_context(&ctx); }
};
struct FormatOutput {
	AVFormatContext *ctx = nullptr;
	bool opened = false;
	~FormatOutput() {
		if (ctx) {
			if (opened) avio_closep(&ctx->pb);
			avformat_free_context(ctx);
		}
	}
};
struct PacketDeleter {
	void operator()(AVPacket *p) const { av_packet_free(&p); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

PacketPtr new_packet() {
	return PacketPtr(av_packet_alloc());
}

/// Read every subtitle packet, converting them to mov_text if needed
/// @return The packets in the subtitle input's time base, in time order
std::vector<PacketPtr> read_subtitles(AVFormatContext *subs, AVCodecContext *dec, AVCodecContext *enc) {
	std::vector<PacketPtr> packets;
	std::vector<uint8_t> buffer(1 << 16);
	auto pkt = new_packet();
	while (av_read_frame(subs, pkt.get()) >= 0) {
		if (pkt->stream_index != 0) {
			av_packet_unref(pkt.get());
			continue;
		}
		if (!enc) {
			packets.push_back(std::move(pkt));
			pkt = new_packet();
			continue;
		}

		AVSubtitle sub{};
		int got = 0;
		int err = avcodec_decode_subtitle2(dec, &sub, &got, pkt.get());
		if (err >= 0 && got) {
			int size = avcodec_encode_subtitle(enc, buffer.data(), (int)buffer.size(), &sub);
			if (size > 0) {
				auto out = new_packet();
				check(av_new_packet(out.get(), size), "Out of memory");
				memcpy(out->data, buffer.data(), size);
				out->pts = out->dts = pkt->pts;
				out->duration = pkt->duration;
				packets.push_back(std::move(out));
			}
		}
		avsubtitle_free(&sub);
		av_packet_unref(pkt.get());
	}

	std::stable_sort(packets.begin(), packets.end(), [](PacketPtr const& a, PacketPtr const& b) { return a->pts < b->pts; });
	return packets;
}
}

namespace soft_subs {
void WriteSubtitles(agi::Context *c, agi::fs::path const& ass) {
	AssFile copy(*c->ass);
	copy.Properties = ProjectProperties{};
	copy.Extradata.clear();
	for (auto it = copy.Events.begin(); it != copy.Events.end(); ) {
		if (it->Comment) {
			it = copy.Events.erase_and_dispose(it, [](AssDialogue *e) { delete e; });
			continue;
		}
		it->ExtradataIds = std::vector<uint32_t>();
		++it;
	}
	SubtitleFormat::GetWriter(ass)->WriteFile(&copy, ass, c->project->Timecodes(), "UTF-8");
}

void Export(Settings const& s, std::function<void(double)> const& progress, std::function<bool()> const& cancelled) {
	const bool mp4 = agi::fs::HasExtension(s.output, "mp4") || agi::fs::HasExtension(s.output, "m4v")
		|| agi::fs::HasExtension(s.output, "mov");

	FormatInput in;
	check(avformat_open_input(&in.ctx, s.video.string().c_str(), nullptr, nullptr), "Could not open the video");
	check(avformat_find_stream_info(in.ctx, nullptr), "Could not read the video");

	FormatInput subs;
	check(avformat_open_input(&subs.ctx, s.subtitles.string().c_str(), av_find_input_format("ass"), nullptr),
		"Could not read the subtitles");
	check(avformat_find_stream_info(subs.ctx, nullptr), "Could not read the subtitles");
	if (subs.ctx->nb_streams < 1)
		throw Error("There are no subtitles to add");
	AVStream *sub_in = subs.ctx->streams[0];

	FormatOutput out;
	auto tmp = agi::fs::path(s.output.string() + ".part" + s.output.extension().string());
	check(avformat_alloc_output_context2(&out.ctx, nullptr, nullptr, tmp.string().c_str()), "Unsupported output file type");

	// Copy the picture and sound, and for MKV the other subtitles and fonts
	std::vector<AVStream *> mapping(in.ctx->nb_streams, nullptr);
	for (unsigned i = 0; i < in.ctx->nb_streams; ++i) {
		AVStream *stream = in.ctx->streams[i];
		auto type = stream->codecpar->codec_type;
		bool wanted = (type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
			|| type == AVMEDIA_TYPE_AUDIO
			|| (!mp4 && s.keep_existing && (type == AVMEDIA_TYPE_SUBTITLE || type == AVMEDIA_TYPE_ATTACHMENT));
		if (!wanted) continue;

		AVStream *copy = avformat_new_stream(out.ctx, nullptr);
		check(avcodec_parameters_copy(copy->codecpar, stream->codecpar), "Could not copy a track");
		copy->codecpar->codec_tag = 0;
		copy->time_base = stream->time_base;
		copy->disposition = stream->disposition;
		if (type == AVMEDIA_TYPE_SUBTITLE)
			copy->disposition &= ~AV_DISPOSITION_DEFAULT;
		av_dict_copy(&copy->metadata, stream->metadata, 0);
		mapping[i] = copy;
	}

	// The new subtitle track: copied as ASS, or converted for MP4
	CodecContext dec, enc;
	AVStream *sub_out = avformat_new_stream(out.ctx, nullptr);
	if (mp4) {
		const AVCodec *decoder = avcodec_find_decoder(sub_in->codecpar->codec_id);
		const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_MOV_TEXT);
		if (!decoder || !encoder) throw Error("No MP4 subtitle encoder available");
		dec.ctx = avcodec_alloc_context3(decoder);
		check(avcodec_parameters_to_context(dec.ctx, sub_in->codecpar), "Could not read the subtitles");
		dec.ctx->pkt_timebase = sub_in->time_base;
		check(avcodec_open2(dec.ctx, decoder, nullptr), "Could not read the subtitles");

		enc.ctx = avcodec_alloc_context3(encoder);
		enc.ctx->time_base = sub_in->time_base;
		if (dec.ctx->subtitle_header) {
			enc.ctx->subtitle_header = static_cast<uint8_t *>(av_mallocz(dec.ctx->subtitle_header_size + 1));
			memcpy(enc.ctx->subtitle_header, dec.ctx->subtitle_header, dec.ctx->subtitle_header_size);
			enc.ctx->subtitle_header_size = dec.ctx->subtitle_header_size;
		}
		check(avcodec_open2(enc.ctx, encoder, nullptr), "Could not convert the subtitles for MP4");
		check(avcodec_parameters_from_context(sub_out->codecpar, enc.ctx), "Could not convert the subtitles for MP4");
	}
	else {
		check(avcodec_parameters_copy(sub_out->codecpar, sub_in->codecpar), "Could not add the subtitles");
		sub_out->codecpar->codec_tag = 0;
	}
	sub_out->time_base = sub_in->time_base;
	sub_out->disposition = AV_DISPOSITION_DEFAULT;
	av_dict_set(&sub_out->metadata, "language", s.language.empty() ? "und" : s.language.c_str(), 0);
	av_dict_set(&sub_out->metadata, "title", "aegidub", 0);

	auto packets = read_subtitles(subs.ctx, dec.ctx, enc.ctx);
	if (packets.empty())
		throw Error("There are no subtitles to add");

	check(avio_open(&out.ctx->pb, tmp.string().c_str(), AVIO_FLAG_WRITE), "Could not create the output file");
	out.opened = true;

	try {
		check(avformat_write_header(out.ctx, nullptr), "Could not write the output file");

		const int64_t file_start = in.ctx->start_time == AV_NOPTS_VALUE ? 0 : in.ctx->start_time;
		const double total = in.ctx->duration > 0 ? in.ctx->duration / static_cast<double>(AV_TIME_BASE) : 0;
		size_t next_sub = 0;

		// Subtitles go in alongside the copied packets, in time order
		auto write_subs_until = [&](double seconds) {
			while (next_sub < packets.size() && packets[next_sub]->pts * av_q2d(sub_in->time_base) <= seconds) {
				AVPacket *p = packets[next_sub++].get();
				av_packet_rescale_ts(p, sub_in->time_base, sub_out->time_base);
				p->stream_index = sub_out->index;
				check(av_interleaved_write_frame(out.ctx, p), "Could not write the output file");
			}
		};

		auto pkt = new_packet();
		while (av_read_frame(in.ctx, pkt.get()) >= 0) {
			if (cancelled && cancelled()) throw soft_subs::Cancelled();

			AVStream *in_stream = in.ctx->streams[pkt->stream_index];
			AVStream *target = mapping[pkt->stream_index];
			if (!target) {
				av_packet_unref(pkt.get());
				continue;
			}

			// Start the copied tracks at zero, where the subtitle times count from
			int64_t offset = av_rescale_q(file_start, AVRational{1, AV_TIME_BASE}, in_stream->time_base);
			if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= offset;
			if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= offset;
			int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
			if (ts != AV_NOPTS_VALUE) {
				double seconds = ts * av_q2d(in_stream->time_base);
				write_subs_until(seconds);
				if (total > 0 && progress) progress(std::clamp(seconds / total, 0.0, 1.0));
			}

			av_packet_rescale_ts(pkt.get(), in_stream->time_base, target->time_base);
			pkt->stream_index = target->index;
			pkt->pos = -1;
			check(av_interleaved_write_frame(out.ctx, pkt.get()), "Could not write the output file");
		}
		write_subs_until(1e300);

		check(av_write_trailer(out.ctx), "Could not finish the output file");
	}
	catch (...) {
		avio_closep(&out.ctx->pb);
		out.opened = false;
		agi::fs::Remove(tmp);
		throw;
	}

	avio_closep(&out.ctx->pb);
	out.opened = false;
	if (agi::fs::FileExists(s.output))
		agi::fs::Remove(s.output);
	agi::fs::Rename(tmp, s.output);
	if (progress) progress(1);
}
}
