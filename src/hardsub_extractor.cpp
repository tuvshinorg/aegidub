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

#include "hardsub_extractor.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "external_process.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "subtitle_format.h"

#include <libaegisub/exception.h>
#include <libaegisub/path.h>

#include <algorithm>
#include <boost/algorithm/string/trim.hpp>
#include <charconv>
#include <fstream>
#include <iterator>
#include <locale>
#include <memory>
#include <sstream>
#include <vector>

#include <wx/intl.h>

namespace {
/// The GitHub repository and the commit of it that is downloaded; pinned so
/// that an update upstream can't break extraction without a new aegidub
const char *repo = "YaoFANGUK/video-subtitle-extractor";
const char *commit = "85746f7df5bf85978fd05f3ca6ce66e321a87a72";

/// Downloads the parts of the repository that are needed and installs their
/// requirements into a Python environment of their own
const char *setup_script = R"PY(
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request

root, repo, commit, gpu = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == "1"
src = os.path.join(root, "src")
env = os.path.join(root, "env")
tools = os.path.join(root, "tools")
bin_dir = "Scripts" if os.name == "nt" else "bin"
exe = ".exe" if os.name == "nt" else ""
env_python = os.path.join(env, bin_dir, "python" + exe)
last_report = 0.0


def report(message, fraction=-1.0, force=True):
    global last_report
    now = time.monotonic()
    if force or now - last_report > 0.25:
        last_report = now
        print(f"PROGRESS {fraction:.4f} {message}", flush=True)


def read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def write(path, text):
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def fetch(url, timeout):
    request = urllib.request.Request(url, headers={"User-Agent": "aegidub"})
    return urllib.request.urlopen(request, timeout=timeout)


def wanted(path):
    if path in ("LICENSE", "requirements.txt", "requirements_directml.txt"):
        return True
    if not path.startswith("backend/"):
        return False
    other = ("macos", "linux") if os.name == "nt" else ("windows",)
    return not any(path.startswith(f"backend/subfinder/{name}/") for name in other)


def download_source():
    marker = os.path.join(src, ".commit")
    if read(marker) == commit:
        return
    report("Downloading video-subtitle-extractor from GitHub...")
    with fetch(f"https://api.github.com/repos/{repo}/git/trees/{commit}?recursive=1", 60) as r:
        tree = json.load(r)
    files = [e for e in tree["tree"] if e["type"] == "blob" and wanted(e["path"])]
    total = sum(e["size"] for e in files)
    done = 0
    for e in files:
        dest = os.path.join(src, *e["path"].split("/"))
        if os.path.exists(dest) and os.path.getsize(dest) == e["size"]:
            done += e["size"]
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        part = dest + ".part"
        url = f"https://raw.githubusercontent.com/{repo}/{commit}/{urllib.parse.quote(e['path'])}"
        with fetch(url, 300) as r, open(part, "wb") as out:
            while chunk := r.read(1 << 20):
                out.write(chunk)
                done += len(chunk)
                report(f"Downloading video-subtitle-extractor from GitHub ({done >> 20} of {total >> 20} MB)...",
                       done / total, False)
        if os.path.getsize(part) != e["size"]:
            raise SystemExit(f"Incomplete download of {e['path']}; try again.")
        os.replace(part, dest)
    write(marker, commit)


def run(args, environ=None):
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=environ,
                               text=True, encoding="utf-8", errors="replace")
    for line in process.stdout:
        line = line.rstrip()
        if line:
            print(line, flush=True)
    if process.wait() != 0:
        raise SystemExit(f"Failed: {' '.join(args)}")


def setup_env():
    marker = os.path.join(env, ".ready")
    ready = f"{commit} gpu={int(gpu)}"
    if read(marker) == ready:
        return
    report("Installing the OCR engine (several minutes, once only)...")

    # uv installs quickly and fetches Python 3.12 when the installed Python is older
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
    run([uv, "pip", "install", "--python", env_python, "-r", os.path.join(src, "requirements.txt")], environ)
    if gpu and os.name == "nt":
        run([uv, "pip", "install", "--python", env_python, "-r", os.path.join(src, "requirements_directml.txt")], environ)
    shutil.rmtree(environ["UV_CACHE_DIR"], ignore_errors=True)
    write(marker, ready)


download_source()
setup_env()
print("PYTHON " + env_python, flush=True)
print("DONE", flush=True)
)PY";

/// Runs video-subtitle-extractor without its GUI
const char *run_script = R"PY(
import json
import multiprocessing
import os
import shutil
import sys
import time

# Worker processes import this file too, so this runs in each of them
for stream in (sys.stdout, sys.stderr):
    stream.reconfigure(encoding="utf-8", errors="replace")
# The models are downloaded with the source, so don't look for them online
os.environ["PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK"] = "True"


def patch_ocr():
    """Paddle 3.3's oneDNN CPU backend fails on these models with
    "ConvertPirAttribute2RuntimeAttribute not support", so turn it off"""
    import backend.tools.ocr as ocr
    paddle_ocr = ocr.PaddleOCR
    ocr.PaddleOCR = lambda **kwargs: paddle_ocr(enable_mkldnn=False, **kwargs)


# The OCR worker processes run this file as __mp_main__, from the VSE folder
if __name__ == "__mp_main__":
    patch_ocr()


def main():
    src, video, output, work_dir, language, mode, gpu = sys.argv[1:8]
    top, bottom, left, right = map(float, sys.argv[8:12])

    # VSE reads config/config.json relative to the working directory
    os.chdir(src)
    sys.path.insert(0, src)
    os.makedirs("config", exist_ok=True)
    try:
        with open("config/config.json", encoding="utf-8") as f:
            settings = json.load(f)
    except (OSError, ValueError):
        settings = {}
    settings.setdefault("Window", {})["Interface"] = "en"
    main_settings = settings.setdefault("Main", {})
    main_settings.update({
        "Language": language,
        "Mode": mode,
        "HardwareAcceleration": gpu == "1",
        "GenerateTxt": False,
        "WordSegmentation": True,
        "DebugNoDeleteCache": False,
        "CheckUpdateOnStartup": False,
    })
    with open("config/config.json", "w", encoding="utf-8") as f:
        json.dump(settings, f, indent=2)

    # OpenCV can't open paths outside the ANSI code page on Windows
    if not video.isascii():
        os.makedirs(work_dir, exist_ok=True)
        link = os.path.join(work_dir, "video" + os.path.splitext(video)[1])
        if os.path.exists(link):
            os.remove(link)
        try:
            os.link(video, link)
        except OSError:
            shutil.copyfile(video, link)
        video = link

    patch_ocr()
    from backend.main import SubtitleExtractor
    from backend.bean.subtitle_area import SubtitleArea

    # Upstream's GUI replaces this, but the method left behind lacks self
    SubtitleExtractor.manage_process = lambda self, pid: None

    extractor = SubtitleExtractor(video)
    if not extractor.frame_count or not extractor.frame_width:
        raise SystemExit("OpenCV could not read the video.")
    h, w = extractor.frame_height, extractor.frame_width
    extractor.sub_area = SubtitleArea(int(top * h), int(bottom * h), int(left * w), int(right * w))
    extractor.subtitle_output_path = output

    last = [0.0]

    def on_progress(ocr, frame_extract, total, finished, post=0):
        now = time.monotonic()
        if now - last[0] > 0.5 or finished:
            last[0] = now
            print(f"PROGRESS {(ocr + frame_extract + post) / total:.4f}", flush=True)

    extractor.add_progress_listener(on_progress)
    extractor.run()
    if not os.path.exists(output):
        raise SystemExit("No subtitles were found in the selected area.")
    print("DONE", flush=True)


if __name__ == "__main__":
    multiprocessing.set_start_method("spawn")
    try:
        main()
    except BaseException:
        import traceback
        traceback.print_exc()
        sys.stdout.flush()
        sys.stderr.flush()
        # Exiting normally would wait for the OCR worker, which never finishes
        os._exit(1)
    sys.stdout.flush()
    os._exit(0)
)PY";

agi::fs::path root() {
	return config::path->Decode("?local/vse");
}

agi::fs::path in_root(const char *name) {
	return agi::fs::path(root() / name);
}

std::string read_file(agi::fs::path const& path) {
	std::ifstream in(path, std::ios::binary);
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	boost::trim(text);
	return text;
}

agi::fs::path write_script(const char *name, const char *script) {
	auto path = in_root(name);
	std::ofstream(path, std::ios::binary | std::ios::trunc) << script;
	return path;
}

/// Run a script, passing "PROGRESS <fraction> [message]" lines to progress
/// and everything else to log
/// @return Every line it printed that wasn't a progress report
std::string run(std::vector<std::string> const& args, std::string const& default_message,
	hardsub_extractor::ProgressFn const& progress, std::function<void(std::string const&)> const& log,
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
				progress(space == std::string::npos ? default_message : line.substr(space + 1), fraction);
				return;
			}
			output += line + "\n";
			if (log) log(line);
		}, cancelled);
	}
	catch (external_process::Error const& e) {
		throw hardsub_extractor::Error(std::string(e.what()) +
			"\n\nPython 3 is needed to extract burned-in subtitles. Install it from python.org, then set its path in Preferences > Dub Render if it isn't on the PATH.");
	}

	if (result.cancelled) throw hardsub_extractor::Cancelled();
	if (result.exit_code != 0 || output.find("DONE") == std::string::npos)
		throw hardsub_extractor::Error(result.tail);
	return output;
}
}

namespace hardsub_extractor {
bool Installed() {
	return read_file(in_root("src/.commit")) == commit && agi::fs::FileExists(in_root("env/.ready"));
}

void Extract(Settings const& s, ProgressFn const& progress, std::function<void(std::string const&)> const& log,
	std::function<bool()> const& cancelled)
{
	agi::fs::CreateDirectory(root());
	std::string python = OPT_GET("Tool/Dub Render/Python")->GetString();
	if (python.empty()) python = "python";

	progress("Getting video-subtitle-extractor ready...", -1);
	auto setup = write_script("setup.py", setup_script);
	auto output = run({python, "-I", setup.string(), root().string(), repo, commit, s.gpu ? "1" : "0"},
		"Getting video-subtitle-extractor ready...", progress, log, cancelled);

	auto marker = output.find("PYTHON ");
	if (marker == std::string::npos)
		throw Error("The video-subtitle-extractor setup did not say where it was installed.");
	std::string env_python = output.substr(marker + 7, output.find('\n', marker) - marker - 7);

	auto work_dir = in_root("work");
	agi::fs::CreateDirectory(work_dir);
	agi::fs::Remove(s.output);
	auto script = write_script("run.py", run_script);
	auto num = [](double v) {
		std::ostringstream ss;
		ss.imbue(std::locale::classic());
		ss << v;
		return ss.str();
	};
	progress("Reading the subtitles in the video...", 0);
	run({env_python, "-I", script.string(), in_root("src").string(), s.video.string(), s.output.string(),
		work_dir.string(), s.language, s.mode, s.gpu ? "1" : "0",
		num(s.top), num(s.bottom), num(s.left), num(s.right)},
		"Reading the subtitles in the video...", progress, log, cancelled);

	if (!agi::fs::FileExists(s.output))
		throw Error("video-subtitle-extractor finished without writing any subtitles.");
}

void ReadLines(agi::Context *c, agi::fs::path const& srt, AssFile& lines) {
	try {
		SubtitleFormat::GetReader(srt, "UTF-8")->ReadFile(&lines, srt, c->project->Timecodes(), "UTF-8");
	}
	catch (agi::Exception const& e) {
		throw Error("Could not read the extracted subtitles: " + e.GetMessage());
	}
}

bool HasLines(agi::Context *c) {
	return std::any_of(c->ass->Events.begin(), c->ass->Events.end(),
		[](AssDialogue const& line) { return !line.Text.get().empty(); });
}

size_t ImportLines(agi::Context *c, AssFile& lines, bool replace) {
	if (lines.Events.empty()) return 0;
	// An empty file still has its one blank starting line. The old lines are
	// only deleted once the new ones are selected, as the selection points at them.
	std::vector<std::unique_ptr<AssDialogue>> old_lines;
	if (replace || !HasLines(c))
		c->ass->Events.clear_and_dispose([&](AssDialogue *e) { old_lines.emplace_back(e); });

	AssDialogue *first = nullptr;
	size_t count = 0;
	for (auto const& line : lines.Events) {
		auto copy = new AssDialogue(line);
		c->ass->Events.push_back(*copy);
		if (!first) first = copy;
		++count;
	}
	c->ass->Commit(_("extract burned-in subtitles"), AssFile::COMMIT_DIAG_ADDREM);
	c->selectionController->SetSelectionAndActive({first}, first);
	return count;
}
}
