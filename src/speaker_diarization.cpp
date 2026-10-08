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

#include "speaker_diarization.h"

#include "external_process.h"
#include "options.h"

#include <libaegisub/path.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>

#include <wx/utils.h>

namespace {
const char *model = "pyannote/speaker-diarization-community-1";
/// Pinned so that an update upstream can't break detection without a new aegidub
const char *package = "pyannote.audio==4.0.7";

/// Installs pyannote into a Python environment of its own
const char *setup_script = R"PY(
import os
import shutil
import subprocess
import sys

root, package, gpu = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
env = os.path.join(root, "env")
tools = os.path.join(root, "tools")
bin_dir = "Scripts" if os.name == "nt" else "bin"
exe = ".exe" if os.name == "nt" else ""
env_python = os.path.join(env, bin_dir, "python" + exe)


def read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def run(args, environ=None):
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environ,
                               text=True, encoding="utf-8", errors="replace")
    for line in process.stdout:
        line = line.rstrip()
        if line:
            print(line, flush=True)
    if process.wait() != 0:
        raise SystemExit(f"Failed: {' '.join(args)}")


marker = os.path.join(env, ".ready")
ready = f"{package} gpu={int(gpu)}"
if read(marker) != ready:
    print("PROGRESS -1 Installing the speaker detection engine (several minutes, once only)...", flush=True)

    # uv installs quickly, fetches Python 3.12 when the installed Python is
    # older, and picks the PyTorch build that suits the GPU
    uv = os.path.join(tools, bin_dir, "uv" + exe)
    if not os.path.exists(uv):
        run([sys.executable, "-m", "venv", tools])
        run([os.path.join(tools, bin_dir, "python" + exe), "-m", "pip", "install",
             "--disable-pip-version-check", "--quiet", "uv"])

    environ = dict(os.environ)
    environ["UV_CACHE_DIR"] = os.path.join(root, "cache")
    environ["UV_PYTHON_INSTALL_DIR"] = os.path.join(root, "python")
    environ["UV_NO_PROGRESS"] = "1"
    if not os.path.exists(env_python):
        run([uv, "venv", "--python", "3.12", env], environ)
    run([uv, "pip", "install", "--python", env_python, "--torch-backend", "auto" if gpu else "cpu",
         package, "soundfile"], environ)
    shutil.rmtree(environ["UV_CACHE_DIR"], ignore_errors=True)
    with open(marker, "w", encoding="utf-8") as f:
        f.write(ready)

print("PYTHON " + env_python, flush=True)
print("DONE", flush=True)
)PY";

/// Writes "<start ms> <end ms> <voice>" lines for each turn
const char *run_script = R"PY(
import os
import sys
import warnings

for stream in (sys.stdout, sys.stderr):
    stream.reconfigure(encoding="utf-8", errors="replace")
os.environ["PYANNOTE_METRICS_ENABLED"] = "0"
os.environ["HF_HUB_DISABLE_SYMLINKS_WARNING"] = "1"
# torchcodec can't find FFmpeg on most Windows installs, but the audio is
# passed in memory so it isn't needed
warnings.filterwarnings("ignore")

wav, output, model, gpu = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == "1"


def progress(fraction, message):
    print(f"PROGRESS {fraction:.4f} {message}", flush=True)


def needs_access(detail=""):
    print("NEEDS_ACCESS " + detail.replace("\n", " "), flush=True)
    sys.exit(2)


progress(-1, "Loading the speaker model...")
import soundfile as sf
import torch
from pyannote.audio import Pipeline

try:
    pipeline = Pipeline.from_pretrained(model, token=os.environ.get("HF_TOKEN") or None)
except Exception as e:
    text = f"{type(e).__name__}: {e}"
    if any(s in text.lower() for s in ("401", "403", "gated", "unauthorized", "token")):
        needs_access(text)
    raise
if pipeline is None:
    needs_access()

device = "CPU"
if gpu and torch.cuda.is_available():
    pipeline.to(torch.device("cuda"))
    device = "GPU"

progress(-1, "Reading the voices...")
audio, rate = sf.read(wav, dtype="float32", always_2d=True)
waveform = torch.from_numpy(audio.mean(axis=1))[None]
del audio

steps = {
    "segmentation": (0.0, 0.4, "Finding speech"),
    "embeddings": (0.4, 1.0, "Telling the voices apart"),
}


def hook(name, artifact, file=None, total=None, completed=None):
    if name in steps and total:
        start, end, message = steps[name]
        progress(start + (end - start) * completed / total, f"{message} on the {device}...")


result = pipeline({"waveform": waveform, "sample_rate": rate}, hook=hook)
# The exclusive version has no overlapping turns, which suits subtitle lines
if hasattr(result, "exclusive_speaker_diarization"):
    annotation = result.exclusive_speaker_diarization
elif hasattr(result, "speaker_diarization"):
    annotation = result.speaker_diarization
else:
    annotation = result

names = {}
with open(output + ".part", "w", encoding="utf-8") as f:
    for turn, _, speaker in annotation.itertracks(yield_label=True):
        voice = names.setdefault(speaker, f"V{len(names) + 1}")
        f.write(f"{int(turn.start * 1000)} {int(turn.end * 1000)} {voice}\n")
os.replace(output + ".part", output)
print("DONE", flush=True)
)PY";

agi::fs::path root() {
	return config::path->Decode("?local/diarize");
}

/// The copy of the model the installer puts next to aegidub.exe
agi::fs::path bundled_model() {
	return config::path->Decode("?data/models/speaker-diarization-community-1");
}

/// The bundled model's folder, or the model's name on Hugging Face
std::string model_source() {
	auto bundled = bundled_model();
	return speaker_diarization::Bundled() ? bundled.string() : model;
}

agi::fs::path write_script(const char *name, const char *script) {
	auto path = agi::fs::path(root() / name);
	std::ofstream(path, std::ios::binary | std::ios::trunc) << script;
	return path;
}

/// Run a script, passing "PROGRESS <fraction> <message>" lines to progress
/// @return Every line it printed that wasn't a progress report
std::string run(std::vector<std::string> const& args, speaker_diarization::ProgressFn const& progress,
	std::function<bool()> const& cancelled)
{
	std::string output;
	external_process::Result result;
	try {
		result = external_process::Run(args, [&](std::string const& line) {
			if (line.compare(0, 9, "PROGRESS ") == 0) {
				auto space = line.find(' ', 9);
				double fraction = -1;
				auto end = space == std::string::npos ? line.size() : space;
				std::from_chars(line.data() + 9, line.data() + end, fraction);
				progress(space == std::string::npos ? "" : line.substr(space + 1), fraction);
				return;
			}
			output += line + "\n";
		}, cancelled);
	}
	catch (external_process::Error const& e) {
		throw speaker_diarization::Error(std::string(e.what()) +
			"\n\nPython 3 is needed to detect speakers from the audio. Install it from python.org, then set its path in Preferences > Dub Render if it isn't on the PATH.");
	}

	if (result.cancelled) throw speaker_diarization::Cancelled();
	auto access = output.find("NEEDS_ACCESS");
	if (access != std::string::npos)
		throw speaker_diarization::NeedsAccess(output.substr(access + 12, output.find('\n', access) - access - 12));
	if (result.exit_code != 0 || output.find("DONE") == std::string::npos)
		throw speaker_diarization::Error(result.tail);
	return output;
}

/// Identifies the audio and model a saved result came from
std::string stamp(agi::fs::path const& wav) {
	std::ostringstream ss;
	ss << model_source() << ' ' << agi::fs::Size(wav) << ' ' << agi::fs::ModifiedTime(wav).time_since_epoch().count();
	return ss.str();
}

std::vector<speaker_diarization::Turn> read_turns(std::istream& in) {
	std::vector<speaker_diarization::Turn> turns;
	speaker_diarization::Turn turn;
	while (in >> turn.start_ms >> turn.end_ms >> turn.voice)
		turns.push_back(turn);
	return turns;
}
}

namespace speaker_diarization {
const char *model_page = "https://huggingface.co/pyannote/speaker-diarization-community-1";

bool Bundled() {
	return agi::fs::FileExists(agi::fs::path(bundled_model() / "config.yaml"));
}

std::vector<Turn> Diarize(agi::fs::path const& wav, std::string const& token, bool gpu,
	ProgressFn const& progress, std::function<bool()> const& cancelled)
{
	auto saved = agi::fs::path(wav.parent_path() / "speakers.txt");
	const std::string current = stamp(wav);
	{
		std::ifstream in(saved, std::ios::binary);
		std::string first;
		if (std::getline(in, first) && first == current)
			return read_turns(in);
	}

	agi::fs::CreateDirectory(root());
	std::string python = OPT_GET("Tool/Dub Render/Python")->GetString();
	if (python.empty()) python = "python";

	progress("Getting the speaker detection engine ready...", -1);
	auto setup = write_script("setup.py", setup_script);
	auto output = run({python, "-I", setup.string(), root().string(), package, gpu ? "1" : "0"}, progress, cancelled);

	auto marker = output.find("PYTHON ");
	if (marker == std::string::npos)
		throw Error("The speaker detection setup did not say where it was installed.");
	std::string env_python = output.substr(marker + 7, output.find('\n', marker) - marker - 7);

	// Passed in the environment rather than on the command line, where other
	// programs could read it
	wxSetEnv("HF_TOKEN", wxString::FromUTF8(token));
	auto turns_path = agi::fs::path(root() / "turns.txt");
	auto script = write_script("run.py", run_script);
	try {
		run({env_python, "-I", script.string(), wav.string(), turns_path.string(), model_source(), gpu ? "1" : "0"},
			progress, cancelled);
	}
	catch (...) {
		wxUnsetEnv("HF_TOKEN");
		throw;
	}
	wxUnsetEnv("HF_TOKEN");

	std::vector<Turn> turns;
	{
		std::ifstream in(turns_path, std::ios::binary);
		turns = read_turns(in);
	}
	agi::fs::Remove(turns_path);

	std::ofstream out(saved, std::ios::binary | std::ios::trunc);
	out << current << '\n';
	for (auto const& turn : turns)
		out << turn.start_ms << ' ' << turn.end_ms << ' ' << turn.voice << '\n';
	return turns;
}

std::vector<std::string> VoiceOfEach(std::vector<std::pair<int, int>> const& ranges, std::vector<Turn> const& turns) {
	std::vector<std::string> voices;
	voices.reserve(ranges.size());
	for (auto [start, end] : ranges) {
		std::map<std::string, int> heard;
		for (auto const& turn : turns) {
			int overlap = std::min(end, turn.end_ms) - std::max(start, turn.start_ms);
			if (overlap > 0) heard[turn.voice] += overlap;
		}
		auto best = std::max_element(heard.begin(), heard.end(),
			[](auto const& a, auto const& b) { return a.second < b.second; });
		// Lines are usually timed a little longer than the speech in them
		const int needed = std::max(200, (end - start) / 4);
		voices.push_back(best != heard.end() && best->second >= needed ? best->first : "");
	}
	return voices;
}
}
