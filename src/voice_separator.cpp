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

#include "voice_separator.h"

#include "dub_render.h"
#include "external_process.h"
#include "http_request.h"
#include "options.h"
#include "wav_util.h"

#include <libaegisub/path.h>

#include <demucs_bridge.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace {
const int sample_rate = 44100;
const char *model_url = "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/ggml-model-htdemucs-4s-f16.bin";
const char *model_file = "ggml-model-htdemucs-4s-f16.bin";
const uint64_t model_size = 83994361;

/// Length of the pieces the built-in separator works on, and how far
/// neighbouring pieces overlap on each side for crossfading
const size_t segment_frames = sample_rate * 30;
const size_t overlap_frames = sample_rate * 1;

// ---------------------------------------------------------------------------
// WAV files

struct WavReader {
	std::ifstream in;
	std::streamoff data_offset = 0;
	size_t frames = 0;

	explicit WavReader(agi::fs::path const& path) : in(path, std::ios::binary) {
		char header[12];
		if (!in.read(header, 12) || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4))
			throw voice_separator::Error("Not a WAV file: " + path.string());

		bool format_ok = false;
		char chunk[8];
		while (in.read(chunk, 8)) {
			uint32_t size;
			memcpy(&size, chunk + 4, 4);
			if (!memcmp(chunk, "fmt ", 4)) {
				std::vector<char> fmt(size);
				in.read(fmt.data(), size);
				uint16_t format, channels, bits;
				uint32_t rate;
				memcpy(&format, &fmt[0], 2);
				memcpy(&channels, &fmt[2], 2);
				memcpy(&rate, &fmt[4], 4);
				memcpy(&bits, &fmt[14], 2);
				format_ok = format == 1 && channels == 2 && rate == sample_rate && bits == 16;
			}
			else if (!memcmp(chunk, "data", 4)) {
				data_offset = in.tellg();
				frames = size / 4;
				break;
			}
			else
				in.seekg(size + (size & 1), std::ios::cur);
		}
		if (!format_ok || !data_offset)
			throw voice_separator::Error("Expected 44.1 kHz 16-bit stereo audio in " + path.string());
	}

	/// Read frames [start, start + count) as interleaved stereo floats
	std::vector<float> Read(size_t start, size_t count) {
		std::vector<int16_t> pcm(count * 2);
		in.clear();
		in.seekg(data_offset + static_cast<std::streamoff>(start * 4));
		in.read(reinterpret_cast<char *>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
		std::vector<float> out(pcm.size());
		for (size_t i = 0; i < pcm.size(); ++i)
			out[i] = pcm[i] / 32768.f;
		return out;
	}
};

class WavWriter {
	std::ofstream out;

public:
	WavWriter(agi::fs::path const& path, size_t frames) : out(path, std::ios::binary | std::ios::trunc) {
		wav_util::WriteHeader(out, 2, sample_rate, static_cast<uint32_t>(frames * 4));
		if (!out) throw voice_separator::Error("Could not write " + path.string());
	}

	/// Append interleaved stereo float samples
	void Write(std::vector<float> const& samples) {
		std::vector<int16_t> pcm(samples.size());
		for (size_t i = 0; i < samples.size(); ++i)
			pcm[i] = static_cast<int16_t>(std::clamp(samples[i], -1.f, 1.f) * 32767.f);
		out.write(reinterpret_cast<const char *>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
		if (!out) throw voice_separator::Error("Could not write the separated audio (is the disk full?)");
	}
};

// ---------------------------------------------------------------------------
// Built-in separator

bool cpu_has_avx2() {
#ifdef _MSC_VER
	int info[4];
	__cpuid(info, 0);
	if (info[0] < 7) return false;
	__cpuidex(info, 7, 0);
	bool avx2 = (info[1] & (1 << 5)) != 0;
	__cpuid(info, 1);
	bool fma = (info[2] & (1 << 12)) != 0;
	bool osxsave = (info[2] & (1 << 27)) != 0;
	return avx2 && fma && osxsave && (_xgetbv(0) & 6) == 6;
#elif defined(__x86_64__) || defined(__i386__)
	return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
	return false;
#endif
}

agi::fs::path ensure_model(voice_separator::ProgressFn const& progress, std::function<bool()> const& cancelled) {
	auto dir = config::path->Decode("?user/models");
	auto path = agi::fs::path(dir / model_file);
	std::error_code ec;
	if (agi::fs::FileExists(path) && std::filesystem::file_size(path, ec) == model_size)
		return path;

	agi::fs::CreateDirectory(dir);
	auto part = agi::fs::path(path.string() + ".part");
	progress("Downloading the voice separation model (84 MB, once only)...", 0);
	try {
		http::Download(model_url, part.string(),
			[&](double fraction) { progress("Downloading the voice separation model (84 MB, once only)...", fraction); },
			cancelled);
	}
	catch (http::Cancelled const&) {
		throw voice_separator::Cancelled();
	}
	catch (http::Error const& e) {
		throw voice_separator::Error(std::string("Could not download the voice separation model: ") + e.what());
	}

	if (std::filesystem::file_size(part, ec) != model_size) {
		agi::fs::Remove(part);
		throw voice_separator::Error("The downloaded voice separation model is incomplete; try again.");
	}
	agi::fs::Rename(part, path);
	return path;
}

/// The separated output of one piece of the input
struct Piece {
	size_t start; ///< First frame, including the overlap
	std::vector<float> vocals; ///< Interleaved stereo
	std::vector<float> background;
};

void separate_builtin(agi::fs::path const& input, agi::fs::path const& vocals_path, agi::fs::path const& background_path,
	voice_separator::ProgressFn const& progress, std::function<bool()> const& cancelled)
{
	if (!cpu_has_avx2())
		throw voice_separator::Error("The built-in voice separator needs a processor with AVX2 (most made since 2013).");

	auto model_path = ensure_model(progress, cancelled);

	progress("Loading the voice separation model...", -1);
	std::unique_ptr<demucs_bridge::Model, void (*)(demucs_bridge::Model *)> model(
		demucs_bridge::Load(model_path.string()), demucs_bridge::Free);
	if (!model)
		throw voice_separator::Error("Could not load the voice separation model " + model_path.string());

	WavReader reader(input);
	const size_t total = reader.frames;
	if (!total) throw voice_separator::Error("The audio is empty");
	const size_t count = (total + segment_frames - 1) / segment_frames;

	auto piece_range = [&](size_t i) {
		size_t start = i * segment_frames;
		size_t padded_start = start > overlap_frames ? start - overlap_frames : 0;
		size_t padded_end = std::min(total, (i + 1) * segment_frames + overlap_frames);
		return std::make_pair(padded_start, padded_end);
	};

	WavWriter vocals_out(vocals_path, total);
	WavWriter background_out(background_path, total);

	std::mutex mutex;
	std::condition_variable changed;
	std::map<size_t, Piece> done;
	std::mutex reader_mutex;
	std::atomic<size_t> next{0};
	std::atomic<bool> stop{false};
	size_t flushed = 0; ///< Pieces written out
	std::string error;

	const unsigned threads = std::max(1u, std::min(std::thread::hardware_concurrency(), 16u));
	// Don't run further ahead of the writer than this, to bound memory use
	const size_t max_ahead = threads * 2;

	auto worker = [&] {
		for (;;) {
			size_t i;
			{
				std::unique_lock<std::mutex> lock(mutex);
				changed.wait(lock, [&] { return stop || next >= count || next < flushed + max_ahead; });
				if (stop || next >= count) return;
				i = next++;
			}

			auto [start, end] = piece_range(i);
			std::vector<float> audio;
			{
				std::lock_guard<std::mutex> lock(reader_mutex);
				audio = reader.Read(start, end - start);
			}

			const size_t frames = end - start;
			Piece piece{start, std::vector<float>(frames * 2), std::vector<float>(frames * 2)};
			std::string failure;
			bool finished = demucs_bridge::Separate(model.get(), audio.data(), frames,
				piece.vocals.data(), piece.background.data(),
				[&] { return !stop && !(cancelled && cancelled()); }, failure);
			if (!finished) {
				std::lock_guard<std::mutex> lock(mutex);
				if (!failure.empty() && error.empty()) error = failure;
				stop = true;
				changed.notify_all();
				return;
			}

			std::lock_guard<std::mutex> lock(mutex);
			done.emplace(i, std::move(piece));
			changed.notify_all();
		}
	};

	std::vector<std::thread> pool;
	for (unsigned t = 0; t < threads; ++t)
		pool.emplace_back(worker);

	// Write each piece's own range once its neighbours are known, crossfading
	// linearly across the overlaps
	try {
		for (size_t i = 0; i < count; ++i) {
			{
				std::unique_lock<std::mutex> lock(mutex);
				changed.wait_for(lock, std::chrono::milliseconds(200), [&] {
					return stop || (done.count(i) && (i + 1 >= count || done.count(i + 1)));
				});
				while (!stop && !(done.count(i) && (i + 1 >= count || done.count(i + 1)))) {
					if (cancelled && cancelled()) stop = true;
					changed.wait_for(lock, std::chrono::milliseconds(200));
				}
				if (stop) break;
			}

			size_t core_start = i * segment_frames;
			size_t core_end = std::min(total, core_start + segment_frames);
			std::vector<float> vocals((core_end - core_start) * 2), background((core_end - core_start) * 2);

			std::unique_lock<std::mutex> lock(mutex);
			Piece const& cur = done.at(i);
			Piece const *prev = i > 0 ? &done.at(i - 1) : nullptr;
			Piece const *nxt = i + 1 < count ? &done.at(i + 1) : nullptr;
			lock.unlock();

			auto sample = [](Piece const& p, std::vector<float> const Piece::*which, size_t t, int ch) {
				return (p.*which)[(t - p.start) * 2 + ch];
			};

			for (size_t t = core_start; t < core_end; ++t) {
				for (int ch = 0; ch < 2; ++ch) {
					for (auto which : {&Piece::vocals, &Piece::background}) {
						float value = sample(cur, which, t, ch);
						if (prev && t < core_start + overlap_frames) {
							float a = static_cast<float>(t - (core_start - overlap_frames)) / (2 * overlap_frames);
							value = (1 - a) * sample(*prev, which, t, ch) + a * value;
						}
						else if (nxt && t + overlap_frames >= core_start + segment_frames) {
							float a = static_cast<float>(t + overlap_frames - (core_start + segment_frames)) / (2 * overlap_frames);
							value = (1 - a) * value + a * sample(*nxt, which, t, ch);
						}
						(which == &Piece::vocals ? vocals : background)[(t - core_start) * 2 + ch] = value;
					}
				}
			}

			vocals_out.Write(vocals);
			background_out.Write(background);

			lock.lock();
			if (i > 0) done.erase(i - 1);
			flushed = i + 1;
			changed.notify_all();
			lock.unlock();

			progress("Separating voices on the CPU (this takes a while)...", static_cast<double>(i + 1) / count);
		}
	}
	catch (...) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stop = true;
			changed.notify_all();
		}
		for (auto& t : pool) t.join();
		throw;
	}

	{
		std::lock_guard<std::mutex> lock(mutex);
		if (flushed < count) stop = true;
		changed.notify_all();
	}
	for (auto& t : pool) t.join();

	if (!error.empty())
		throw voice_separator::Error("Voice separation failed: " + error);
	if (flushed < count)
		throw voice_separator::Cancelled();
}

// ---------------------------------------------------------------------------
// Python demucs on a GPU

const char *python_script = R"PY(
import sys
import numpy as np
import soundfile as sf
import torch
from demucs.apply import apply_model
from demucs.pretrained import get_model

src, vocals_path, background_path = sys.argv[1:4]
model = get_model("htdemucs")
model.eval()
audio, rate = sf.read(src, dtype="float32", always_2d=True)
wav = torch.from_numpy(audio[:, :2].T.copy())
ref = wav.mean(0)
mean, std = ref.mean(), ref.std() + 1e-8
wav = (wav - mean) / std
with torch.no_grad():
    sources = apply_model(model, wav[None], device="cuda", split=True, overlap=0.25, progress=True)[0]
del wav
# In place: an episode's four sources already take several GB
sources.mul_(std).add_(mean)
vocals = sources[model.sources.index("vocals")].clone()
background = sources.sum(0)
del sources
background.sub_(vocals)
sf.write(vocals_path, vocals.T.cpu().numpy(), rate, subtype="PCM_16")
sf.write(background_path, background.T.cpu().numpy(), rate, subtype="PCM_16")
print("DONE", flush=True)
)PY";

/// Is Python with demucs, soundfile and a CUDA GPU available?
bool python_gpu_available(std::string const& python) {
	try {
		auto result = external_process::Run({python, "-c",
			"import sys, demucs, soundfile, torch; sys.exit(0 if torch.cuda.is_available() else 3)"},
			nullptr, nullptr);
		return result.exit_code == 0;
	}
	catch (external_process::Error const&) {
		return false;
	}
}

/// @return false if it failed and the built-in separator should be used
bool separate_python(std::string const& python, agi::fs::path const& input, agi::fs::path const& vocals,
	agi::fs::path const& background, agi::fs::path const& work_dir,
	voice_separator::ProgressFn const& progress, std::function<bool()> const& cancelled)
{
	auto script = agi::fs::path(work_dir / "separate.py");
	{
		std::ofstream out(script, std::ios::binary | std::ios::trunc);
		out << python_script;
	}

	progress("Separating voices on the GPU...", 0);
	static const std::regex percent(R"((\d+)%\|)");
	auto result = external_process::Run({python, script.string(), input.string(), vocals.string(), background.string()},
		[&](std::string const& line) {
			std::smatch m;
			if (std::regex_search(line, m, percent))
				progress("Separating voices on the GPU...", std::stoi(m[1].str()) / 100.0);
		},
		cancelled);

	if (result.cancelled) throw voice_separator::Cancelled();
	return result.exit_code == 0 && result.tail.find("DONE") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Separations kept for reuse

/// Identifies a video file well enough to know when cached audio is stale
std::string video_fingerprint(agi::fs::path const& video) {
	std::ostringstream ss;
	ss << video.string() << '\n' << agi::fs::Size(video) << '\n'
	   << agi::fs::ModifiedTime(video).time_since_epoch().count();
	return ss.str();
}

std::string read_file(agi::fs::path const& path) {
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}

namespace voice_separator {
std::string Separate(agi::fs::path const& input, agi::fs::path const& vocals, agi::fs::path const& background,
	agi::fs::path const& work_dir, ProgressFn const& progress, std::function<bool()> const& cancelled)
{
	agi::fs::CreateDirectory(work_dir);

	if (OPT_GET("Tool/Dub Render/Use GPU If Available")->GetBool()) {
		std::string python = OPT_GET("Tool/Dub Render/Python")->GetString();
		progress("Looking for a GPU...", -1);
		if (!python.empty() && python_gpu_available(python)) {
			if (separate_python(python, input, vocals, background, work_dir, progress, cancelled))
				return "GPU (Python demucs)";
			// Fall through to the built-in separator, which always works
		}
	}

	separate_builtin(input, vocals, background, progress, cancelled);
	return "CPU (built-in)";
}

bool VideoSeparated(agi::fs::path const& video, agi::fs::path const& work_dir) {
	auto stamp = agi::fs::path(work_dir / "source.txt");
	return agi::fs::FileExists(agi::fs::path(work_dir / "vocals.wav"))
		&& agi::fs::FileExists(agi::fs::path(work_dir / "background.wav"))
		&& agi::fs::FileExists(stamp)
		&& read_file(stamp) == video_fingerprint(video);
}

std::string SeparateVideo(agi::fs::path const& video, agi::fs::path const& work_dir,
	std::function<void(int step, std::string const& message, double fraction)> const& progress,
	std::function<bool()> const& cancelled)
{
	if (VideoSeparated(video, work_dir)) return "";

	agi::fs::CreateDirectory(work_dir);
	auto original = agi::fs::path(work_dir / "original.wav");
	progress(0, "", 0);
	try {
		dub_render::ExtractAudio(video, original, [&](double f) { progress(0, "", f); }, cancelled);
	}
	catch (dub_render::Cancelled const&) {
		throw Cancelled();
	}

	auto engine = Separate(original, agi::fs::path(work_dir / "vocals.wav"), agi::fs::path(work_dir / "background.wav"),
		work_dir, [&](std::string const& message, double f) { progress(1, message, f); }, cancelled);

	// Remember which video this separation belongs to
	std::ofstream(agi::fs::path(work_dir / "source.txt"), std::ios::binary | std::ios::trunc) << video_fingerprint(video);
	agi::fs::Remove(original);
	return engine;
}
}
