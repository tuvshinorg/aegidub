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

#include "dub_render.h"

#include "wav_util.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

namespace {
const int mix_rate = 44100;

std::string av_error(int err) {
	char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

void check(int err, const char *what) {
	if (err < 0)
		throw dub_render::Error(std::string(what) + ": " + av_error(err));
}

// RAII holders for the FFmpeg types used here
struct FormatInput {
	AVFormatContext *ctx = nullptr;
	~FormatInput() { avformat_close_input(&ctx); }
};
struct CodecContext {
	AVCodecContext *ctx = nullptr;
	~CodecContext() { avcodec_free_context(&ctx); }
};
struct Frame {
	AVFrame *frame = av_frame_alloc();
	~Frame() { av_frame_free(&frame); }
};
struct Packet {
	AVPacket *pkt = av_packet_alloc();
	~Packet() { av_packet_free(&pkt); }
};
struct Resampler {
	SwrContext *ctx = nullptr;
	~Resampler() { swr_free(&ctx); }
};
struct FilterGraph {
	AVFilterGraph *graph = avfilter_graph_alloc();
	~FilterGraph() { avfilter_graph_free(&graph); }
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

void open_input(FormatInput& in, agi::fs::path const& path) {
	check(avformat_open_input(&in.ctx, path.string().c_str(), nullptr, nullptr), "Could not open the video");
	check(avformat_find_stream_info(in.ctx, nullptr), "Could not read the video");
}

double duration_seconds(AVFormatContext *ctx) {
	return ctx->duration > 0 ? ctx->duration / static_cast<double>(AV_TIME_BASE) : 0;
}

/// Seconds from the start of the file
double seconds(int64_t ts, AVRational time_base, int64_t start) {
	if (ts == AV_NOPTS_VALUE) return 0;
	return (ts - start) * av_q2d(time_base);
}

/// Sequential reader of a 16-bit PCM WAV
struct WavInput {
	std::ifstream in;
	int channels = 0;
	int rate = 0;
	uint64_t frames_left = 0;
	int64_t next_pts = 0;
	bool eof_sent = false;

	explicit WavInput(agi::fs::path const& path) : in(path, std::ios::binary) {
		char header[12];
		if (!in.read(header, 12) || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4))
			throw dub_render::Error("Not a WAV file: " + path.string());
		char chunk[8];
		int bits = 0;
		while (in.read(chunk, 8)) {
			uint32_t size;
			memcpy(&size, chunk + 4, 4);
			if (!memcmp(chunk, "fmt ", 4)) {
				std::vector<char> fmt(size);
				in.read(fmt.data(), size);
				uint16_t ch, b;
				uint32_t r;
				memcpy(&ch, &fmt[2], 2);
				memcpy(&r, &fmt[4], 4);
				memcpy(&b, &fmt[14], 2);
				channels = ch;
				rate = static_cast<int>(r);
				bits = b;
			}
			else if (!memcmp(chunk, "data", 4)) {
				if (bits != 16 || channels < 1 || channels > 2 || rate <= 0)
					throw dub_render::Error("Unsupported WAV format: " + path.string());
				frames_left = size / (2u * channels);
				return;
			}
			else
				in.seekg(size + (size & 1), std::ios::cur);
		}
		throw dub_render::Error("No audio in " + path.string());
	}

	/// Push up to the given number of seconds into a buffer source
	void Feed(AVFilterContext *src, double secs) {
		if (eof_sent) return;
		uint64_t want = std::min<uint64_t>(frames_left, static_cast<uint64_t>(secs * rate));
		if (!want) {
			check(av_buffersrc_add_frame_flags(src, nullptr, 0), "Could not end an audio input");
			eof_sent = true;
			return;
		}

		Frame f;
		f.frame->format = AV_SAMPLE_FMT_S16;
		f.frame->sample_rate = rate;
		f.frame->nb_samples = static_cast<int>(want);
		av_channel_layout_default(&f.frame->ch_layout, channels);
		check(av_frame_get_buffer(f.frame, 0), "Out of memory");
		in.read(reinterpret_cast<char *>(f.frame->data[0]), static_cast<std::streamsize>(want * 2 * channels));
		size_t got = static_cast<size_t>(in.gcount()) / (2u * channels);
		if (got < want) {
			// Truncated file: end here
			f.frame->nb_samples = static_cast<int>(got);
			frames_left = 0;
		}
		else
			frames_left -= want;
		f.frame->pts = next_pts;
		next_pts += f.frame->nb_samples;
		if (f.frame->nb_samples)
			check(av_buffersrc_add_frame_flags(src, f.frame, 0), "Could not mix the audio");
	}
};

std::string abuffer_args(WavInput const& in) {
	return "sample_rate=" + std::to_string(in.rate) + ":sample_fmt=s16:channel_layout=" +
		(in.channels == 1 ? "mono" : "stereo") + ":time_base=1/" + std::to_string(in.rate);
}

/// Loudness of a remastered dub, as for online video
const double target_lufs = -16;
/// Highest sample level after remastering, -1.5 dBFS
const double true_peak_limit = 0.841;

/// The dub mixed over the background (and the original voices, if kept),
/// fed from the WAV files a second at a time
class Mixer {
	WavInput background;
	WavInput dub;
	std::unique_ptr<WavInput> original;
	FilterGraph graph;
	AVFilterContext *src_bg = nullptr;
	AVFilterContext *src_dub = nullptr;
	AVFilterContext *src_orig = nullptr;

public:
	AVFilterContext *sink = nullptr;

	/// @param tail Filters applied to the finished mix
	Mixer(dub_render::Settings const& s, std::string const& tail)
	: background(s.background)
	, dub(s.dub)
	{
		if (s.original_volume > 0)
			original = std::make_unique<WavInput>(s.original_voices);

		// Remastering evens out the dub and ducks the background while it speaks
		const std::string stereo = "aformat=sample_fmts=fltp:sample_rates=44100:channel_layouts=stereo";
		std::string desc =
			"abuffer@bg=" + abuffer_args(background) + "," + stereo + "[bg];" +
			"abuffer@dub=" + abuffer_args(dub) + "," + stereo;
		if (s.remaster)
			desc += ",highpass=f=70,acompressor=threshold=0.1:ratio=3:attack=5:release=120:makeup=2,asplit=2[dub][key];"
				"[bg][key]sidechaincompress=threshold=0.05:ratio=4:attack=30:release=400[bgd];";
		else
			desc += "[dub];[bg]anull[bgd];";
		int inputs = 2;
		if (original) {
			desc += "abuffer@orig=" + abuffer_args(*original) + "," + stereo +
				",volume=" + std::to_string(s.original_volume) + "[orig];";
			++inputs;
		}
		desc += "[bgd][dub]";
		if (original) desc += "[orig]";
		desc += "amix=inputs=" + std::to_string(inputs) + ":duration=first:normalize=0," + tail + ",";
		desc += "aresample=44100," + stereo + ",abuffersink@out";

		AVFilterInOut *open_in = nullptr, *open_out = nullptr;
		int err = avfilter_graph_parse2(graph.graph, desc.c_str(), &open_in, &open_out);
		avfilter_inout_free(&open_in);
		avfilter_inout_free(&open_out);
		check(err, "Could not set up the audio mix");
		check(avfilter_graph_config(graph.graph, nullptr), "Could not set up the audio mix");
		src_bg = avfilter_graph_get_filter(graph.graph, "abuffer@bg");
		src_dub = avfilter_graph_get_filter(graph.graph, "abuffer@dub");
		src_orig = original ? avfilter_graph_get_filter(graph.graph, "abuffer@orig") : nullptr;
		sink = avfilter_graph_get_filter(graph.graph, "abuffersink@out");
		if (!src_bg || !src_dub || !sink || (original && !src_orig))
			throw dub_render::Error("Could not set up the audio mix");
	}

	/// Feed a second of every input, then pass each finished frame to
	/// on_frame, using frame as scratch space
	/// @return false once the mix has ended
	bool Pump(AVFrame *frame, std::function<void(AVFrame *)> const& on_frame) {
		background.Feed(src_bg, 1.0);
		dub.Feed(src_dub, 1.0);
		if (original) original->Feed(src_orig, 1.0);

		for (;;) {
			int e = av_buffersink_get_frame(sink, frame);
			if (e == AVERROR(EAGAIN)) return true;
			if (e == AVERROR_EOF) return false;
			check(e, "Could not mix the audio");
			on_frame(frame);
			av_frame_unref(frame);
		}
	}

	/// How far into the background the mix has got
	double Seconds() const {
		return static_cast<double>(background.next_pts) / background.rate;
	}
};
}

namespace dub_render {
void ExtractAudio(agi::fs::path const& video, agi::fs::path const& wav,
	ProgressFn const& progress, std::function<bool()> const& cancelled)
{
	FormatInput in;
	open_input(in, video);

	const AVCodec *decoder = nullptr;
	int stream_index = av_find_best_stream(in.ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
	check(stream_index, "The video has no audio track");
	AVStream *stream = in.ctx->streams[stream_index];

	CodecContext dec;
	dec.ctx = avcodec_alloc_context3(decoder);
	check(avcodec_parameters_to_context(dec.ctx, stream->codecpar), "Could not set up the audio decoder");
	check(avcodec_open2(dec.ctx, decoder, nullptr), "Could not open the audio decoder");

	Resampler swr;
	AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	check(swr_alloc_set_opts2(&swr.ctx, &stereo, AV_SAMPLE_FMT_S16, mix_rate,
		&dec.ctx->ch_layout, dec.ctx->sample_fmt, dec.ctx->sample_rate, 0, nullptr), "Could not set up resampling");
	check(swr_init(swr.ctx), "Could not set up resampling");

	auto tmp = agi::fs::path(wav.string() + ".part");
	std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
	wav_util::WriteHeader(out, 2, mix_rate, 0);
	uint64_t bytes = 0;

	const double total = duration_seconds(in.ctx);
	const int64_t start = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
	Packet pkt;
	Frame frame;
	std::vector<int16_t> buffer;

	auto convert = [&](AVFrame *f) {
		int max_out = swr_get_out_samples(swr.ctx, f ? f->nb_samples : 0);
		if (max_out <= 0) return;
		buffer.resize(static_cast<size_t>(max_out) * 2);
		uint8_t *out_planes[] = {reinterpret_cast<uint8_t *>(buffer.data())};
		int got = swr_convert(swr.ctx, out_planes, max_out,
			f ? const_cast<const uint8_t **>(f->extended_data) : nullptr, f ? f->nb_samples : 0);
		check(got, "Could not resample the audio");
		out.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(got) * 4);
		bytes += static_cast<uint64_t>(got) * 4;
	};

	auto drain = [&] {
		for (;;) {
			int err = avcodec_receive_frame(dec.ctx, frame.frame);
			if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) return;
			check(err, "Could not decode the audio");
			if (total > 0 && progress)
				progress(std::min(1.0, seconds(frame.frame->pts, stream->time_base, start) / total));
			convert(frame.frame);
			av_frame_unref(frame.frame);
		}
	};

	while (av_read_frame(in.ctx, pkt.pkt) >= 0) {
		if (cancelled && cancelled()) {
			out.close();
			agi::fs::Remove(tmp);
			throw Cancelled();
		}
		if (pkt.pkt->stream_index == stream_index) {
			// A damaged packet is skipped rather than failing a long render
			if (avcodec_send_packet(dec.ctx, pkt.pkt) >= 0)
				drain();
		}
		av_packet_unref(pkt.pkt);
	}
	avcodec_send_packet(dec.ctx, nullptr);
	drain();
	convert(nullptr);

	if (bytes > 0xFFFFFFF0ull)
		throw Error("The audio is too long for a WAV file");
	out.seekp(0);
	wav_util::WriteHeader(out, 2, mix_rate, static_cast<uint32_t>(bytes));
	out.close();
	if (!out) throw Error("Could not write " + tmp.string() + " (is the disk full?)");
	agi::fs::Rename(tmp, wav);
}

void Render(Settings const& s, ProgressFn const& progress, std::function<bool()> const& cancelled) {
	FormatInput in;
	open_input(in, s.video);
	int video_index = av_find_best_stream(in.ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	check(video_index, "The file has no video track");
	int audio_index = av_find_best_stream(in.ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	const double total = duration_seconds(in.ctx);
	const int64_t file_start = in.ctx->start_time == AV_NOPTS_VALUE ? 0 : in.ctx->start_time;

	// The renders below get the first part of the progress bar, if any
	double progress_start = 0;
	auto report = [&](double seconds_done, double weight) {
		if (progress && total > 0)
			progress(progress_start + weight * std::min(1.0, seconds_done / total));
	};

	// Remastering brings the result to -16 LUFS. Measuring the whole mix first
	// and then applying one gain hits the target exactly and keeps the
	// speech's natural dynamics, which a single-pass loudnorm doesn't.
	std::string tail = "alimiter=limit=0.95:level=0";
	if (s.remaster) {
		const double measure_weight = 0.3;
		Mixer measure(s, "ebur128=metadata=1");
		double integrated = target_lufs;
		Frame frame;
		while (measure.Pump(frame.frame, [&](AVFrame *f) {
			if (auto e = av_dict_get(f->metadata, "lavfi.r128.I", nullptr, 0))
				integrated = std::atof(e->value);
		})) {
			if (cancelled && cancelled()) throw Cancelled();
			report(measure.Seconds(), measure_weight);
		}
		progress_start = measure_weight;

		// Nothing to measure in near silence; don't boost noise
		double gain = integrated > -70 ? std::clamp(target_lufs - integrated, -20.0, 20.0) : 0.0;
		tail = "volume=" + std::to_string(gain) + "dB,alimiter=limit=" + std::to_string(true_peak_limit) + ":level=0";
	}

	Mixer mixer(s, tail);
	AVFilterContext *sink = mixer.sink;

	// Output
	FormatOutput out;
	auto tmp = agi::fs::path(s.output.string() + ".part" + s.output.extension().string());
	check(avformat_alloc_output_context2(&out.ctx, nullptr, nullptr, tmp.string().c_str()), "Unsupported output file type");

	AVStream *out_video = avformat_new_stream(out.ctx, nullptr);
	check(avcodec_parameters_copy(out_video->codecpar, in.ctx->streams[video_index]->codecpar), "Could not copy the video");
	out_video->codecpar->codec_tag = 0;
	out_video->time_base = in.ctx->streams[video_index]->time_base;
	out_video->disposition = in.ctx->streams[video_index]->disposition | AV_DISPOSITION_DEFAULT;
	av_dict_copy(&out_video->metadata, in.ctx->streams[video_index]->metadata, 0);

	const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!aac) throw Error("No AAC encoder available");
	CodecContext enc;
	enc.ctx = avcodec_alloc_context3(aac);
	enc.ctx->sample_rate = mix_rate;
	enc.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	enc.ctx->bit_rate = 192000;
	av_channel_layout_default(&enc.ctx->ch_layout, 2);
	enc.ctx->time_base = AVRational{1, mix_rate};
	if (out.ctx->oformat->flags & AVFMT_GLOBALHEADER)
		enc.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	check(avcodec_open2(enc.ctx, aac, nullptr), "Could not open the AAC encoder");
	av_buffersink_set_frame_size(sink, enc.ctx->frame_size);

	AVStream *out_dub = avformat_new_stream(out.ctx, nullptr);
	check(avcodec_parameters_from_context(out_dub->codecpar, enc.ctx), "Could not set up the dub track");
	out_dub->time_base = enc.ctx->time_base;
	out_dub->disposition = AV_DISPOSITION_DEFAULT;
	av_dict_set(&out_dub->metadata, "language", "mon", 0);
	av_dict_set(&out_dub->metadata, "title", "Mongolian dub", 0);

	AVStream *out_orig = nullptr;
	if (s.keep_original_track && audio_index >= 0) {
		out_orig = avformat_new_stream(out.ctx, nullptr);
		check(avcodec_parameters_copy(out_orig->codecpar, in.ctx->streams[audio_index]->codecpar), "Could not copy the original audio");
		out_orig->codecpar->codec_tag = 0;
		out_orig->time_base = in.ctx->streams[audio_index]->time_base;
		out_orig->disposition = 0;
		av_dict_copy(&out_orig->metadata, in.ctx->streams[audio_index]->metadata, 0);
		av_dict_set(&out_orig->metadata, "title", "Original", 0);
	}

	check(avio_open(&out.ctx->pb, tmp.string().c_str(), AVIO_FLAG_WRITE), "Could not create the output file");
	out.opened = true;
	check(avformat_write_header(out.ctx, nullptr), "Could not write the output file");

	Packet pkt;
	Frame mixed;
	double video_time = 0, audio_time = 0;
	bool input_done = false, audio_done = false;

	auto write_encoded = [&] {
		for (;;) {
			int e = avcodec_receive_packet(enc.ctx, pkt.pkt);
			if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) return;
			check(e, "Could not encode the audio");
			av_packet_rescale_ts(pkt.pkt, enc.ctx->time_base, out_dub->time_base);
			pkt.pkt->stream_index = out_dub->index;
			audio_time = pkt.pkt->pts * av_q2d(out_dub->time_base);
			check(av_interleaved_write_frame(out.ctx, pkt.pkt), "Could not write the output file");
		}
	};

	auto pump_audio = [&] {
		bool more = mixer.Pump(mixed.frame, [&](AVFrame *f) {
			check(avcodec_send_frame(enc.ctx, f), "Could not encode the audio");
			write_encoded();
		});
		if (!more) {
			check(avcodec_send_frame(enc.ctx, nullptr), "Could not encode the audio");
			write_encoded();
			audio_done = true;
		}
	};

	try {
		// Keep the video and the new audio roughly in step so the muxer
		// doesn't have to buffer much of either
		while (!input_done || !audio_done) {
			if (cancelled && cancelled()) throw Cancelled();

			if (!input_done && (audio_done || video_time <= audio_time)) {
				if (av_read_frame(in.ctx, pkt.pkt) < 0) {
					input_done = true;
					continue;
				}
				AVStream *in_stream = in.ctx->streams[pkt.pkt->stream_index];
				AVStream *target = pkt.pkt->stream_index == video_index ? out_video
					: (out_orig && pkt.pkt->stream_index == audio_index) ? out_orig : nullptr;
				if (!target) {
					av_packet_unref(pkt.pkt);
					continue;
				}
				// Start the copied tracks at zero, like the mixed audio
				int64_t offset = av_rescale_q(file_start, AVRational{1, AV_TIME_BASE}, in_stream->time_base);
				if (pkt.pkt->pts != AV_NOPTS_VALUE) pkt.pkt->pts -= offset;
				if (pkt.pkt->dts != AV_NOPTS_VALUE) pkt.pkt->dts -= offset;
				if (pkt.pkt->stream_index == video_index)
					video_time = seconds(pkt.pkt->dts != AV_NOPTS_VALUE ? pkt.pkt->dts : pkt.pkt->pts, in_stream->time_base, 0);
				av_packet_rescale_ts(pkt.pkt, in_stream->time_base, target->time_base);
				pkt.pkt->stream_index = target->index;
				pkt.pkt->pos = -1;
				check(av_interleaved_write_frame(out.ctx, pkt.pkt), "Could not write the output file");
			}
			else if (!audio_done)
				pump_audio();
			else
				input_done = true;

			report(std::min(audio_done ? total : audio_time, input_done ? total : video_time), 1 - progress_start);
		}

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
	agi::fs::Rename(tmp, s.output);
}
std::vector<int16_t> ChangeTempo(std::vector<int16_t> const& samples, int rate, double factor) {
	if (samples.empty() || factor == 1.0) return samples;

	FilterGraph graph;
	const std::string r = std::to_string(rate);
	// atempo keeps the pitch, so the voice still sounds like itself
	std::string desc = "abuffer@in=sample_rate=" + r + ":sample_fmt=s16:channel_layout=mono:time_base=1/" + r +
		",atempo=" + std::to_string(factor) +
		",aformat=sample_fmts=s16:sample_rates=" + r + ":channel_layouts=mono,abuffersink@out";
	AVFilterInOut *open_in = nullptr, *open_out = nullptr;
	int err = avfilter_graph_parse2(graph.graph, desc.c_str(), &open_in, &open_out);
	avfilter_inout_free(&open_in);
	avfilter_inout_free(&open_out);
	check(err, "Could not set up the speed change");
	check(avfilter_graph_config(graph.graph, nullptr), "Could not set up the speed change");
	AVFilterContext *src = avfilter_graph_get_filter(graph.graph, "abuffer@in");
	AVFilterContext *sink = avfilter_graph_get_filter(graph.graph, "abuffersink@out");
	if (!src || !sink) throw Error("Could not set up the speed change");

	Frame in;
	in.frame->format = AV_SAMPLE_FMT_S16;
	in.frame->sample_rate = rate;
	in.frame->nb_samples = static_cast<int>(samples.size());
	av_channel_layout_default(&in.frame->ch_layout, 1);
	check(av_frame_get_buffer(in.frame, 0), "Out of memory");
	memcpy(in.frame->data[0], samples.data(), samples.size() * 2);
	in.frame->pts = 0;
	check(av_buffersrc_add_frame_flags(src, in.frame, 0), "Could not change the speed");
	check(av_buffersrc_add_frame_flags(src, nullptr, 0), "Could not change the speed");

	std::vector<int16_t> out;
	out.reserve(static_cast<size_t>(samples.size() / factor) + 1024);
	Frame got;
	for (;;) {
		int e = av_buffersink_get_frame(sink, got.frame);
		if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) break;
		check(e, "Could not change the speed");
		auto data = reinterpret_cast<const int16_t *>(got.frame->data[0]);
		out.insert(out.end(), data, data + got.frame->nb_samples);
		av_frame_unref(got.frame);
	}
	return out;
}
}
