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

/// @file dub.cpp
/// @brief Emotion detection and dub track generation commands

#include "../json_util.h"
#include "command.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../ai_batch.h"
#include "../compat.h"
#include "../dialog_progress.h"
#include "../base_grid.h"
#include "../dialogs.h"
#include "../dub_clip.h"
#include "../dub_render.h"
#include "../dub_settings.h"
#include "../elevenlabs_client.h"
#include "../format.h"
#include "../include/aegisub/context.h"
#include "../libresrc/libresrc.h"
#include "../line_emotion.h"
#include "../line_spoken.h"
#include "../openai_client.h"
#include "../options.h"
#include "../original_text.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../subs_controller.h"
#include "../video_controller.h"
#include "../voice_cast.h"
#include "../wav_util.h"

#include <libaegisub/character_count.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <atomic>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <wx/msgdlg.h>
#include <wx/utils.h>
#include <wx/sound.h>

namespace {
using json_util::parse_json;
using json_util::to_json;
	using cmd::Command;

/// Sample rate of the clips ElevenLabs returns and of the assembled track
const int sample_rate = 24000;
/// Lines sent to the model per emotion detection request
const size_t emotion_batch_size = 60;
/// Already directed lines sent along with each batch so the scene carries over
const size_t emotion_context_size = 20;
/// How far a clip may run into the next line before it is flagged
const int overlap_tolerance_ms = 100;

struct validate_sel_nonempty : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return c->selectionController->GetSelectedSet().size() > 0;
	}
};

struct validate_active_line : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return c->selectionController->GetActiveLine() != nullptr;
	}
};

bool check_ai_config(agi::Context *c, openai::Config const& config, wxString const& title) {
	if (!config.api_key.empty() && !config.base_url.empty() && !config.model.empty()) return true;
	wxMessageBox(
		_("This uses the AI model set up for translation.\n\nEnter an API key, URL and model in Preferences > AI Translation."),
		title, wxOK | wxICON_INFORMATION, c->parent);
	return false;
}

bool check_tts_config(agi::Context *c, elevenlabs::Config const& config, wxString const& title) {
	if (!config.api_key.empty() && !config.model.empty()) return true;
	wxMessageBox(
		_("No ElevenLabs API key is set.\n\nEnter it in Preferences > Voice Cast, or set the ELEVENLABS_API_KEY environment variable."),
		title, wxOK | wxICON_INFORMATION, c->parent);
	return false;
}

// ---------------------------------------------------------------------------
// Emotion detection

struct EmotionJob {
	AssDialogue *line;
	std::string character;
	std::string text;
	std::string original;
	std::string tags;
	std::string spoken;
	/// The line already had an emotion and is only here for its spoken form
	bool keep_emotion = false;
	/// The line contains numbers, symbols or Latin words
	bool needs_spoken = false;
	bool done = false;
};

std::string emotion_prompt() {
	std::string tags;
	for (auto const& tag : line_emotion::Presets())
		tags += (tags.empty() ? "" : ", ") + tag;
	return
		"You are the voice director of a dub. For every entry in `lines`, choose how the "
		"line should be performed, as ElevenLabs audio tags.\n"
		"Rules:\n"
		"- Read the scene: who is speaking (`character`), what was said before, and what the "
		"line means. `original` is the untranslated line when there is one.\n"
		"- Give 0, 1 or 2 short lowercase tags per line. Prefer these: " + tags + ". "
		"Other short tags such as \"laughs softly\" or \"clears throat\" are allowed when they clearly fit.\n"
		"- Most everyday lines need no tag: use an empty string for a neutral delivery. "
		"Keep tags for lines where the feeling is clear, so the performance does not sound overacted.\n"
		"- Keep a character's mood consistent across consecutive lines of the same scene.\n"
		+ std::string(line_spoken::PromptRules()) +
		"- Lines marked `needs_spoken` contain such things; check them carefully.\n"
		"Reply with a single JSON object of the form "
		"{\"lines\":[{\"id\":<id>,\"tags\":\"<tags separated by commas, or empty>\",\"spoken\":\"<only when needed>\"}]} "
		"containing exactly the ids you were given.";
}

std::string emotion_request(std::vector<EmotionJob> const& jobs, size_t start, size_t end) {
	json::Object root;

	json::Array ctx;
	for (size_t i = start > emotion_context_size ? start - emotion_context_size : 0; i < start; ++i) {
		if (!jobs[i].done) continue;
		json::Object entry;
		entry.emplace("character", json::UnknownElement(jobs[i].character));
		entry.emplace("text", json::UnknownElement(jobs[i].text));
		entry.emplace("tags", json::UnknownElement(jobs[i].tags));
		ctx.emplace_back(std::move(entry));
	}
	if (!ctx.empty())
		root.emplace("previous_lines_for_context_only", json::UnknownElement(std::move(ctx)));

	json::Array arr;
	for (size_t i = start; i < end; ++i) {
		json::Object entry;
		entry.emplace("id", json::UnknownElement(static_cast<int64_t>(i)));
		if (!jobs[i].character.empty())
			entry.emplace("character", json::UnknownElement(jobs[i].character));
		entry.emplace("text", json::UnknownElement(jobs[i].text));
		if (!jobs[i].original.empty() && jobs[i].original != jobs[i].text)
			entry.emplace("original", json::UnknownElement(jobs[i].original));
		if (jobs[i].needs_spoken)
			entry.emplace("needs_spoken", json::UnknownElement(true));
		arr.emplace_back(std::move(entry));
	}
	root.emplace("lines", json::UnknownElement(std::move(arr)));
	return to_json(json::UnknownElement(std::move(root)));
}

/// @param overwrite Also redo lines which already have an emotion
void detect_emotions(agi::Context *c, std::vector<AssDialogue *> const& lines, bool overwrite) {
	c->videoController->Stop();
	const wxString title = _("AI Detect Emotions");

	auto config = dub::LoadOpenAIConfig();
	if (!check_ai_config(c, config, title)) return;

	std::vector<EmotionJob> jobs;
	for (auto line : lines) {
		if (line->Comment) continue;
		std::string text = dub::SpokenText(line->Text.get());
		if (text.empty()) continue;

		// Lines that already have an emotion are still sent when they need a
		// spoken form they don't have yet, but keep their emotion
		bool has_emotion = !line_emotion::Get(c->ass.get(), line).empty();
		bool needs_spoken = line_spoken::NeedsSpokenForm(text);
		bool missing_spoken = needs_spoken && line_spoken::Get(c->ass.get(), line).empty();
		if (!overwrite && has_emotion && !missing_spoken) continue;

		EmotionJob job{line, line->Actor.get(), std::move(text), dub::SpokenText(original_text::Get(c->ass.get(), line))};
		job.keep_emotion = !overwrite && has_emotion;
		job.needs_spoken = needs_spoken;
		jobs.push_back(std::move(job));
	}

	if (jobs.empty()) {
		wxMessageBox(overwrite
			? _("The selected lines have no text to direct.")
			: _("Every line already has an emotion. Select lines and use AI Detect Emotions for Selected Lines to redo them."),
			title, wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	int answer = wxMessageBox(
		fmt_tl("Choose emotions for %d lines using %s?\n\nCheck the Emotion column afterwards; click a cell to change it. This can be undone.",
			(int)jobs.size(), config.model),
		title, wxYES_NO | wxICON_QUESTION, c->parent);
	if (answer != wxYES) return;

	std::string error;
	DialogProgress progress(c->parent, title, _("Detecting emotions..."));
	progress.Run([&](agi::ProgressSink *ps) {
		ai_batch::Job batch;
		batch.count = jobs.size();
		batch.batch_size = emotion_batch_size;
		batch.system = emotion_prompt();
		batch.request = [&](size_t start, size_t end) { return emotion_request(jobs, start, end); };
		batch.message = [&](size_t start, size_t end) {
			return from_wx(fmt_tl("Directing lines %d-%d of %d", (int)start + 1, (int)end, (int)jobs.size()));
		};
		batch.on_item = [&](size_t i, json::Object const& item) {
			auto tags = item.find("tags");
			if (tags == item.end()) return;
			jobs[i].tags = line_emotion::Normalize(static_cast<json::String const&>(tags->second));
			jobs[i].spoken = json_util::find_string(item, "spoken");
			jobs[i].done = true;
		};
		error = ai_batch::Run(config, batch, ps);
	});

	// Apply whatever was completed, even after an error or cancel: those
	// batches have already been paid for.
	size_t directed = 0;
	for (auto const& job : jobs) {
		if (!job.done) continue;
		if (!job.keep_emotion)
			line_emotion::Set(c->ass.get(), job.line, job.tags);
		if (job.needs_spoken || !job.spoken.empty())
			line_spoken::Set(c->ass.get(), job.line, job.spoken);
		++directed;
	}
	if (directed)
		c->ass->Commit(_("detect emotions"), AssFile::COMMIT_EXTRADATA);

	if (!error.empty()) {
		wxMessageBox(fmt_tl("Emotion detection stopped after %d of %d lines:\n\n%s", (int)directed, (int)jobs.size(), error),
			title, wxOK | wxICON_ERROR, c->parent);
	}
	else if (directed < jobs.size()) {
		wxMessageBox(fmt_tl("%d of %d lines were directed. Run it again to do the rest.", (int)directed, (int)jobs.size()),
			title, wxOK | wxICON_WARNING, c->parent);
	}
}

struct tool_emotion_detect_all final : public Command {
	CMD_NAME("tool/emotion/detect/all")
	CMD_ICON(emotion_detect_button)
	STR_MENU("AI Detect &Emotions")
	STR_DISP("AI Detect Emotions")
	STR_HELP("Choose with the configured AI model how each line without an emotion should be performed")

	void operator()(agi::Context *c) override {
		std::vector<AssDialogue *> lines;
		for (auto& line : c->ass->Events)
			lines.push_back(&line);
		detect_emotions(c, lines, false);
	}
};

struct tool_emotion_detect_selected final : public validate_sel_nonempty {
	CMD_NAME("tool/emotion/detect/selected")
	CMD_ICON(emotion_detect_button)
	STR_MENU("AI Detect Emotions for Selected Li&nes")
	STR_DISP("AI Detect Emotions for Selected Lines")
	STR_HELP("Choose with the configured AI model how each selected line should be performed, replacing its emotion")

	void operator()(agi::Context *c) override {
		detect_emotions(c, c->selectionController->GetSortedSelection(), true);
	}
};

// ---------------------------------------------------------------------------
// Speech generation

/// One line to be spoken
struct Clip {
	AssDialogue *line;
	std::string character;
	std::string voice_id;
	/// Emotion tags followed by the spoken text
	std::string text;
	agi::fs::path path;
	int start_ms = 0;
	/// Length of the audio as it goes into the track, once known
	size_t samples = 0;
	/// How much faster it is played so it fits; 1 for as generated
	double tempo = 1.0;
};

/// A clip's audio as it goes into the track
std::vector<int16_t> clip_audio(std::vector<int16_t> samples, double tempo);

/// What generating speech for these texts costs, and what the plan has left
wxString cost_note(elevenlabs::Config const& config, std::vector<Clip *> const& todo) {
	int64_t characters = 0;
	for (auto clip : todo)
		characters += elevenlabs::BilledCharacters(clip->text);

	wxString note = fmt_tl("About %d characters will be billed by ElevenLabs.", (int)characters);
	elevenlabs::Usage usage;
	wxBusyCursor wait;
	if (elevenlabs::GetUsage(config, usage)) {
		int64_t left = std::max<int64_t>(0, usage.limit - usage.used);
		note += " " + fmt_tl("Your plan has %d of %d characters left this period.", (int)left, (int)usage.limit);
		if (characters > left)
			note += "\n\n" + _("That is more than is left: generating will stop part way when the allowance runs out.");
	}
	return note;
}

/// Folder that holds the generated audio of a script
agi::fs::path dub_folder(agi::Context *c) {
	return dub::Folder(c->subsController->Filename());
}

/// Build the clip for a line, or explain why it can't be spoken
bool make_clip(agi::Context *c, voice_cast::Cast const& cast, elevenlabs::Config const& config,
	AssDialogue *line, Clip& clip, std::string *why_not)
{
	dub_clip::Request request;
	if (!dub_clip::ForLine(c->ass.get(), line, cast, request, why_not))
		return false;
	clip.line = line;
	clip.character = request.character;
	clip.voice_id = request.voice_id;
	clip.text = request.text;
	clip.start_ms = static_cast<int>(line->Start);
	clip.path = dub_clip::Path(c->subsController->Filename(), config, request);
	return true;
}

/// Show newly generated speech in the grid's Fit column
void refresh_grid(agi::Context *c) {
	if (c->subsGrid) c->subsGrid->Refresh(false);
}

void write_file(agi::fs::path const& path, std::vector<char> const& data) {
	// Write to a temporary name first so a cancelled run never leaves a
	// truncated clip that would later be mistaken for a finished one
	agi::fs::path tmp(path.string() + ".part");
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
		if (!out) throw elevenlabs::Error("Could not write " + tmp.string());
	}
	agi::fs::Rename(tmp, path);
}

/// Sample data of a 16-bit mono WAV file
std::vector<int16_t> read_wav(agi::fs::path const& path) {
	std::ifstream in(path, std::ios::binary);
	std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (data.size() < 12 || std::string(data.data(), 4) != "RIFF")
		return {};

	size_t pos = 12;
	while (pos + 8 <= data.size()) {
		uint32_t size = static_cast<uint8_t>(data[pos + 4]) | static_cast<uint8_t>(data[pos + 5]) << 8
			| static_cast<uint8_t>(data[pos + 6]) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(data[pos + 7])) << 24;
		if (std::string(data.data() + pos, 4) == "data") {
			size_t bytes = std::min<size_t>(size, data.size() - pos - 8) & ~size_t(1);
			std::vector<int16_t> samples(bytes / 2);
			memcpy(samples.data(), data.data() + pos + 8, bytes);
			return samples;
		}
		pos += 8 + size + (size & 1);
	}
	return {};
}

std::vector<int16_t> clip_audio(std::vector<int16_t> samples, double tempo) {
	if (tempo <= 1.0) return samples;
	return dub_render::ChangeTempo(samples, sample_rate, tempo);
}

size_t wav_samples(agi::fs::path const& path) {
	return read_wav(path).size();
}

/// Generate the clips that don't exist yet, several at a time
/// @return An error message, or empty on success or cancel
std::string generate_clips(elevenlabs::Config const& config, std::vector<Clip *> const& todo, agi::ProgressSink *ps) {
	std::atomic<size_t> next{0};
	std::atomic<bool> failed{false};
	std::mutex mutex;
	size_t finished = 0;
	std::string error;

	auto worker = [&] {
		while (!failed && !ps->IsCancelled()) {
			size_t i = next++;
			if (i >= todo.size()) return;
			Clip& clip = *todo[i];
			try {
				auto wav = elevenlabs::Speak(config, clip.voice_id, clip.text, [&] { return ps->IsCancelled() || failed; });
				write_file(clip.path, wav);
			}
			catch (std::exception const& e) {
				std::lock_guard<std::mutex> lock(mutex);
				if (!failed && !ps->IsCancelled())
					error = from_wx(fmt_tl("Line %d (%s): %s", clip.line->Row + 1, clip.character, e.what()));
				failed = true;
				return;
			}

			std::lock_guard<std::mutex> lock(mutex);
			++finished;
			ps->SetProgress(finished, todo.size());
			ps->SetMessage(from_wx(fmt_tl("Generated %d of %d lines", (int)finished, (int)todo.size())));
		}
	};

	size_t thread_count = std::clamp<int64_t>(OPT_GET("Tool/Voice Cast/Concurrent Requests")->GetInt(), 1, 10);
	thread_count = std::min(thread_count, todo.size());
	std::vector<std::thread> threads;
	for (size_t i = 0; i < thread_count; ++i)
		threads.emplace_back(worker);
	for (auto& thread : threads)
		thread.join();
	return error;
}

/// Mix the clips into one track, each at its line's start time
void write_track(agi::fs::path const& path, std::vector<Clip>& clips) {
	size_t total = 0;
	for (auto const& clip : clips)
		total = std::max(total, static_cast<size_t>(clip.start_ms) * sample_rate / 1000 + clip.samples);

	agi::fs::path tmp(path.string() + ".part");
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		wav_util::WriteHeader(out, 1, sample_rate, static_cast<uint32_t>(total * 2));

		// Mix a block at a time so an hour-long episode doesn't need all its
		// audio in memory at once
		const size_t block = static_cast<size_t>(sample_rate) * 30;
		std::map<size_t, std::vector<int16_t>> loaded;
		std::vector<int32_t> mix;
		std::vector<int16_t> pcm;
		for (size_t begin = 0; begin < total; begin += block) {
			size_t end = std::min(total, begin + block);
			mix.assign(end - begin, 0);

			for (size_t i = 0; i < clips.size(); ++i) {
				size_t clip_begin = static_cast<size_t>(clips[i].start_ms) * sample_rate / 1000;
				size_t clip_end = clip_begin + clips[i].samples;
				if (clip_end <= begin || clip_begin >= end) continue;

				auto it = loaded.find(i);
				if (it == loaded.end())
					it = loaded.emplace(i, clip_audio(read_wav(clips[i].path), clips[i].tempo)).first;
				auto const& samples = it->second;

				for (size_t s = std::max(begin, clip_begin); s < std::min(end, clip_end); ++s) {
					size_t offset = s - clip_begin;
					if (offset < samples.size())
						mix[s - begin] += samples[offset];
				}
			}

			pcm.resize(mix.size());
			for (size_t s = 0; s < mix.size(); ++s)
				pcm[s] = static_cast<int16_t>(std::clamp<int32_t>(mix[s], INT16_MIN, INT16_MAX));
			out.write(reinterpret_cast<const char *>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));

			// Drop clips that have finished playing
			for (auto it = loaded.begin(); it != loaded.end();) {
				size_t clip_end = static_cast<size_t>(clips[it->first].start_ms) * sample_rate / 1000 + clips[it->first].samples;
				it = clip_end <= end ? loaded.erase(it) : std::next(it);
			}
		}

		if (!out) throw elevenlabs::Error("Could not write " + tmp.string());
	}
	agi::fs::Rename(tmp, path);
}

std::string format_seconds(int ms) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%d:%02d.%d", ms / 60000, ms / 1000 % 60, ms / 100 % 10);
	return buf;
}

void generate_dub_track(agi::Context *c) {
	c->videoController->Stop();
	const wxString title = _("Generate Dub Track");

	if (c->subsController->Filename().empty() || !c->subsController->CanSave()) {
		wxMessageBox(_("Save the subtitles as an .ass file first. The audio is written next to it."),
			title, wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	auto config = dub::LoadElevenLabsConfig();
	if (!check_tts_config(c, config, title)) return;

	auto cast = voice_cast::Load(c->ass.get());
	std::vector<Clip> clips;
	size_t no_character = 0;
	std::map<std::string, size_t> no_voice;
	for (auto& line : c->ass->Events) {
		if (line.Comment || dub::SpokenText(line.Text.get()).empty()) continue;
		Clip clip;
		if (make_clip(c, cast, config, &line, clip, nullptr))
			clips.push_back(std::move(clip));
		else if (line.Actor.get().empty())
			++no_character;
		else
			++no_voice[line.Actor.get()];
	}

	if (clips.empty()) {
		wxMessageBox(_("No line can be spoken yet. Every line needs a character (AI Detect Speakers) and every character a voice (Voice Cast)."),
			title, wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	std::vector<Clip *> todo;
	for (auto& clip : clips) {
		if (!agi::fs::FileExists(clip.path))
			todo.push_back(&clip);
	}
	// Two lines with identical text and voice share a clip; generate it once
	std::sort(todo.begin(), todo.end(), [](Clip *a, Clip *b) { return a->path < b->path; });
	todo.erase(std::unique(todo.begin(), todo.end(), [](Clip *a, Clip *b) { return a->path == b->path; }), todo.end());

	wxString message = fmt_tl("%d lines will be in the dub track: %d need generating and %d are already done.",
		(int)clips.size(), (int)todo.size(), (int)(clips.size() - todo.size()));
	if (!todo.empty())
		message += "\n\n" + cost_note(config, todo);
	if (no_character)
		message += "\n\n" + fmt_tl("%d lines have no character and will be silent.", (int)no_character);
	if (!no_voice.empty()) {
		wxString names;
		for (auto const& entry : no_voice)
			names += fmt_wx("\n    %s (%d lines)", entry.first, (int)entry.second);
		message += "\n\n" + _("These characters have no voice and will be silent:") + names;
	}
	message += "\n\n" + _("Continue?");
	if (wxMessageBox(message, title, wxYES_NO | wxICON_QUESTION, c->parent) != wxYES) return;

	auto folder = dub_folder(c);
	auto track_path = dub::TrackPath(c->subsController->Filename());
	auto report_path = agi::fs::path(folder / "report.txt");
	try {
		agi::fs::CreateDirectory(agi::fs::path(folder / "clips"));
	}
	catch (agi::Exception const& e) {
		wxMessageBox(fmt_tl("Could not create %s:\n\n%s", folder.string(), e.GetMessage()), title, wxOK | wxICON_ERROR, c->parent);
		return;
	}

	std::string error;
	bool cancelled = false;
	DialogProgress progress(c->parent, title, _("Generating speech..."));
	progress.Run([&](agi::ProgressSink *ps) {
		if (!todo.empty()) {
			ps->SetProgress(0, todo.size());
			error = generate_clips(config, todo, ps);
			if (!error.empty()) return;
			if (ps->IsCancelled()) {
				cancelled = true;
				return;
			}
		}

		ps->SetMessage(from_wx(_("Building the dub track...")));
		try {
			for (auto& clip : clips) {
				size_t generated = wav_samples(clip.path);
				// Lines that run a little long are sped up rather than overlap
				int length_ms = static_cast<int>(generated * 1000 / sample_rate);
				clip.tempo = dub_clip::Tempo(length_ms, dub_clip::RoomMs(c->ass.get(), clip.line));
				clip.samples = static_cast<size_t>(generated / clip.tempo);
			}
			write_track(track_path, clips);
		}
		catch (std::exception const& e) {
			error = e.what();
		}
	});

	if (!error.empty()) {
		wxMessageBox(fmt_tl("Generating the dub stopped:\n\n%s\n\nThe lines generated so far are kept and will not be paid for again.", error),
			title, wxOK | wxICON_ERROR, c->parent);
		return;
	}
	if (cancelled) return;

	// A clip overlaps when it is still playing as the next line starts
	std::sort(clips.begin(), clips.end(), [](Clip const& a, Clip const& b) { return a.start_ms < b.start_ms; });
	Selection flagged;
	AssDialogue *first_flagged = nullptr;
	std::ostringstream report;
	size_t sped_up = std::count_if(clips.begin(), clips.end(), [](Clip const& clip) { return clip.tempo > 1.0; });
	report << "Lines that run into the next line, even sped up by "
	       << static_cast<int>(std::lround((dub_clip::MaxTempo() - 1) * 100)) << "%\n"
	       << "Shorten their text (AI Shorten Long Lines), or move the next line, then generate again.\n\n";
	for (size_t i = 0; i < clips.size(); ++i) {
		// The same measure as the grid's Fit column
		int length_ms = static_cast<int>(clips[i].samples * 1000 / sample_rate);
		int slot_ms = dub_clip::RoomMs(c->ass.get(), clips[i].line);
		if (length_ms <= slot_ms + overlap_tolerance_ms) continue;

		flagged.insert(clips[i].line);
		if (!first_flagged) first_flagged = clips[i].line;
		report << "Line " << clips[i].line->Row + 1 << " at " << format_seconds(clips[i].start_ms)
		       << "  " << clips[i].character << ": speech " << format_seconds(length_ms)
		       << ", room " << format_seconds(slot_ms) << "\n    " << clips[i].text << "\n";
	}
	if (flagged.empty())
		report << "None.\n";
	{
		std::ofstream out(report_path, std::ios::binary | std::ios::trunc);
		out << report.str();
	}

	if (first_flagged)
		c->selectionController->SetSelectionAndActive(std::move(flagged), first_flagged);
	refresh_grid(c);

	wxString done = fmt_tl("The dub track was written to:\n%s", track_path.string());
	if (sped_up)
		done += "\n\n" + fmt_tl("%d lines that ran a little long were sped up to fit.", (int)sped_up);
	if (first_flagged)
		done += "\n\n" + fmt_tl("%d lines run into the next line. They are now selected, and listed in %s.",
			(int)c->selectionController->GetSelectedSet().size(), report_path.string());
	done += "\n\n" + _("Open the dub track as the audio, to review it alongside the subtitles?");
	if (wxMessageBox(done, title, wxYES_NO | wxICON_INFORMATION, c->parent) == wxYES)
		c->project->LoadAudio(track_path);
}

struct tool_dub_generate final : public Command {
	CMD_NAME("tool/dub/generate")
	CMD_ICON(dub_generate_button)
	STR_MENU("&Generate Dub Track...")
	STR_DISP("Generate Dub Track")
	STR_HELP("Speak every line with its character's voice and emotion, and build one audio track timed to the subtitles")

	void operator()(agi::Context *c) override {
		generate_dub_track(c);
	}
};

struct tool_dub_preview final : public validate_active_line {
	CMD_NAME("tool/dub/preview")
	CMD_ICON(dub_preview_button)
	STR_MENU("&Play Dub of Line")
	STR_DISP("Play Dub of Line")
	STR_HELP("Speak the active line with its character's voice and emotion")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		const wxString title = _("Play Dub of Line");
		auto line = c->selectionController->GetActiveLine();
		if (!line) return;

		auto config = dub::LoadElevenLabsConfig();
		if (!check_tts_config(c, config, title)) return;

		Clip clip;
		std::string why_not;
		if (!make_clip(c, voice_cast::Load(c->ass.get()), config, line, clip, &why_not)) {
			wxMessageBox(to_wx(why_not), title, wxOK | wxICON_INFORMATION, c->parent);
			return;
		}

		if (!agi::fs::FileExists(clip.path)) {
			if (c->subsController->Filename().empty()) {
				wxMessageBox(_("Save the subtitles first. Generated audio is kept next to the file."), title, wxOK | wxICON_INFORMATION, c->parent);
				return;
			}

			std::string error;
			DialogProgress progress(c->parent, title, fmt_tl("Generating line %d...", line->Row + 1));
			progress.Run([&](agi::ProgressSink *ps) {
				try {
					agi::fs::CreateDirectory(clip.path.parent_path());
					auto wav = elevenlabs::Speak(config, clip.voice_id, clip.text, [ps] { return ps->IsCancelled(); });
					write_file(clip.path, wav);
				}
				catch (agi::Exception const& e) {
					error = e.GetMessage();
				}
				catch (std::exception const& e) {
					if (!ps->IsCancelled())
						error = e.what();
				}
			});
			if (!error.empty()) {
				wxMessageBox(fmt_tl("Could not generate the line:\n\n%s", error), title, wxOK | wxICON_ERROR, c->parent);
				return;
			}
			if (!agi::fs::FileExists(clip.path)) return; // cancelled
			refresh_grid(c);
		}

		// Kept alive while it plays
		static wxSound preview_sound;
		static std::vector<char> preview_data;

		// Play it as it will sound in the track, sped up if it runs long
		std::vector<char> data;
		auto samples = read_wav(clip.path);
		int length_ms = static_cast<int>(samples.size() * 1000 / sample_rate);
		auto audio = clip_audio(std::move(samples), dub_clip::Tempo(length_ms, dub_clip::RoomMs(c->ass.get(), line)));
		{
			std::ostringstream wav;
			wav_util::WriteHeader(wav, 1, sample_rate, static_cast<uint32_t>(audio.size() * 2));
			wav.write(reinterpret_cast<const char *>(audio.data()), static_cast<std::streamsize>(audio.size() * 2));
			auto bytes = wav.str();
			data.assign(bytes.begin(), bytes.end());
		}
		wxSound::Stop();
		preview_data = std::move(data);
		if (!preview_sound.Create(preview_data.size(), preview_data.data()) || !preview_sound.Play(wxSOUND_ASYNC))
			wxMessageBox(_("The line could not be played."), title, wxOK | wxICON_ERROR, c->parent);
	}
};

// ---------------------------------------------------------------------------
// Shortening lines whose speech doesn't fit

/// Aim this far under the room available, so a regenerated line isn't
/// borderline again
const double shorten_margin = 0.9;
/// Lines sent to the model per shortening request
const size_t shorten_batch_size = 20;

struct ShortenJob {
	AssDialogue *line;
	std::string character;
	std::string text;
	std::string original;
	std::string before;
	std::string after;
	int speech_ms;
	int room_ms;
	std::string shortened;
	std::string spoken;
	bool done = false;
};

std::string shorten_prompt() {
	return
		"You adapt a dub script so every line fits its time. Each line in `lines` was spoken by a "
		"speech engine and the audio runs longer than the time available before the next line.\n"
		"Rewrite each `text` shorter, in the same language, so it can be spoken within `room_seconds` "
		"(it takes `speech_seconds` now). Aim for about `target_characters` characters.\n"
		"Rules:\n"
		"- Keep the meaning: what the speaker wants to say, every key fact, name and number, the "
		"tone and the character's way of speaking. Only the wording gets shorter.\n"
		"- Use shorter words and phrasing, drop filler, repetition and words the scene makes "
		"obvious. Never drop information the story needs.\n"
		"- `original` is the untranslated line and `before`/`after` are the neighbouring lines, "
		"for context only.\n"
		"- Keep ASS override blocks in {curly braces} unchanged, and \\N line breaks only where "
		"they still make sense.\n"
		+ std::string(line_spoken::PromptRules()) +
		"Reply with a single JSON object of the form "
		"{\"lines\":[{\"id\":<id>,\"text\":\"<shorter line>\",\"spoken\":\"<only when needed>\"}]} "
		"containing exactly the ids you were given.";
}

std::string shorten_request(std::vector<ShortenJob> const& jobs, size_t start, size_t end) {
	json::Array arr;
	for (size_t i = start; i < end; ++i) {
		auto const& job = jobs[i];
		const double ratio = shorten_margin * job.room_ms / std::max(job.speech_ms, 1);
		const auto target = static_cast<int64_t>(std::max(1.0, agi::CharacterCount(job.text, agi::IGNORE_BLOCKS) * ratio));
		json::Object entry;
		entry.emplace("id", json::UnknownElement(static_cast<int64_t>(i)));
		if (!job.character.empty())
			entry.emplace("character", json::UnknownElement(job.character));
		entry.emplace("text", json::UnknownElement(job.text));
		if (!job.original.empty())
			entry.emplace("original", json::UnknownElement(job.original));
		if (!job.before.empty())
			entry.emplace("before", json::UnknownElement(job.before));
		if (!job.after.empty())
			entry.emplace("after", json::UnknownElement(job.after));
		entry.emplace("speech_seconds", json::UnknownElement(std::round(job.speech_ms / 100.0) / 10.0));
		entry.emplace("room_seconds", json::UnknownElement(std::round(job.room_ms / 100.0) / 10.0));
		entry.emplace("target_characters", json::UnknownElement(target));
		arr.emplace_back(std::move(entry));
	}
	json::Object root;
	root.emplace("lines", json::UnknownElement(std::move(arr)));
	return to_json(json::UnknownElement(std::move(root)));
}

/// Spoken text of the neighbouring dialogue line, for context
std::string neighbour_text(agi::Context *c, AssDialogue *line, bool forward) {
	auto it = c->ass->Events.iterator_to(*line);
	for (;;) {
		if (forward) {
			if (++it == c->ass->Events.end()) return {};
		}
		else {
			if (it == c->ass->Events.begin()) return {};
			--it;
		}
		if (it->Comment) continue;
		auto text = dub::SpokenText(it->Text.get());
		if (!text.empty()) return text;
	}
}

/// Make model output safe to store as a single-line ASS dialogue text
std::string single_line(std::string text) {
	boost::replace_all(text, "\r\n", "\\N");
	boost::replace_all(text, "\n", "\\N");
	boost::replace_all(text, "\r", "");
	boost::trim(text);
	return text;
}

void shorten_lines(agi::Context *c) {
	c->videoController->Stop();
	const wxString title = _("AI Shorten Long Lines");

	auto ai = dub::LoadOpenAIConfig();
	if (!check_ai_config(c, ai, title)) return;
	auto tts = dub::LoadElevenLabsConfig();
	auto subs = c->subsController->Filename();
	if (subs.empty()) {
		wxMessageBox(_("Save the subtitles and generate the dub first: lines are measured by their generated speech."),
			title, wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	// With several lines selected only those are looked at, otherwise all
	auto const& selection = c->selectionController->GetSelectedSet();
	std::vector<AssDialogue *> candidates;
	for (auto& line : c->ass->Events) {
		if (selection.size() > 1 && !selection.count(&line)) continue;
		candidates.push_back(&line);
	}

	auto cast = voice_cast::Load(c->ass.get());
	std::vector<ShortenJob> jobs;
	size_t not_generated = 0;
	for (auto line : candidates) {
		dub_clip::Request request;
		if (!dub_clip::ForLine(c->ass.get(), line, cast, request, nullptr)) continue;
		int speech = dub_clip::DurationMs(dub_clip::Path(subs, tts, request));
		if (speech < 0) {
			++not_generated;
			continue;
		}
		int room = dub_clip::RoomMs(c->ass.get(), line);
		if (speech <= room * dub_clip::MaxTempo()) continue;

		ShortenJob job{line, line->Actor.get(), line->Text.get(),
			dub::SpokenText(original_text::Get(c->ass.get(), line)),
			neighbour_text(c, line, false), neighbour_text(c, line, true), speech, std::max(room, 1)};
		jobs.push_back(std::move(job));
	}

	if (jobs.empty()) {
		wxString message = _("Every line with generated speech fits its time.");
		if (not_generated)
			message += "\n\n" + fmt_tl("%d lines have no generated speech yet, so they could not be measured. Generate the dub first.", (int)not_generated);
		wxMessageBox(message, title, wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	int answer = wxMessageBox(
		fmt_tl("%d lines run past their time. Shorten them with %s, keeping their meaning?\n\nThe Fit column shows how long each one is now. This can be undone.",
			(int)jobs.size(), ai.model),
		title, wxYES_NO | wxICON_QUESTION, c->parent);
	if (answer != wxYES) return;

	std::string error;
	DialogProgress progress(c->parent, title, _("Shortening lines..."));
	progress.Run([&](agi::ProgressSink *ps) {
		ai_batch::Job batch;
		batch.count = jobs.size();
		batch.batch_size = shorten_batch_size;
		batch.system = shorten_prompt();
		batch.request = [&](size_t start, size_t end) { return shorten_request(jobs, start, end); };
		batch.message = [&](size_t start, size_t end) {
			return from_wx(fmt_tl("Shortening lines %d-%d of %d", (int)start + 1, (int)end, (int)jobs.size()));
		};
		batch.on_item = [&](size_t i, json::Object const& item) {
			jobs[i].shortened = single_line(json_util::find_string(item, "text"));
			jobs[i].spoken = json_util::find_string(item, "spoken");
			jobs[i].done = !jobs[i].shortened.empty();
		};
		error = ai_batch::Run(ai, batch, ps);
	});

	// Apply whatever was completed, even after an error or cancel
	std::vector<AssDialogue *> changed;
	for (auto const& job : jobs) {
		if (!job.done || job.shortened == job.line->Text.get()) continue;
		job.line->Text = job.shortened;
		// After the text, which the spoken form is tied to
		line_spoken::Set(c->ass.get(), job.line, job.spoken);
		changed.push_back(job.line);
	}
	if (!changed.empty())
		c->ass->Commit(_("shorten lines"), AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_EXTRADATA);

	if (!error.empty()) {
		wxMessageBox(fmt_tl("Shortening stopped after %d of %d lines:\n\n%s", (int)changed.size(), (int)jobs.size(), error),
			title, wxOK | wxICON_ERROR, c->parent);
		if (changed.empty()) return;
	}
	if (changed.empty()) return;

	// Measure the new wording straight away if the user agrees to the cost
	if (!check_tts_config(c, tts, title)) return;
	std::vector<Clip> clips;
	for (auto line : changed) {
		Clip clip;
		if (make_clip(c, cast, tts, line, clip, nullptr) && !agi::fs::FileExists(clip.path))
			clips.push_back(std::move(clip));
	}
	std::vector<Clip *> todo;
	for (auto& clip : clips) todo.push_back(&clip);

	answer = wxMessageBox(
		fmt_tl("%d lines were shortened. Generate their speech now to check that they fit?", (int)changed.size()) +
			"\n\n" + cost_note(tts, todo),
		title, wxYES_NO | wxICON_QUESTION, c->parent);
	if (answer != wxYES) {
		refresh_grid(c);
		return;
	}

	error.clear();
	if (!todo.empty()) {
		try {
			agi::fs::CreateDirectory(todo.front()->path.parent_path());
		}
		catch (agi::Exception const& e) {
			wxMessageBox(to_wx(e.GetMessage()), title, wxOK | wxICON_ERROR, c->parent);
			return;
		}
		DialogProgress gen(c->parent, title, _("Generating speech..."));
		gen.Run([&](agi::ProgressSink *ps) {
			ps->SetProgress(0, todo.size());
			error = generate_clips(tts, todo, ps);
		});
	}
	refresh_grid(c);

	size_t fit = 0;
	for (auto line : changed) {
		dub_clip::Request request;
		if (!dub_clip::ForLine(c->ass.get(), line, cast, request, nullptr)) continue;
		int speech = dub_clip::DurationMs(dub_clip::Path(subs, tts, request));
		if (speech >= 0 && speech <= dub_clip::RoomMs(c->ass.get(), line)) ++fit;
	}

	wxString summary = fmt_tl("%d of %d shortened lines now fit.", (int)fit, (int)changed.size());
	if (!error.empty())
		summary += "\n\n" + fmt_tl("Generating stopped:\n%s", error);
	else if (fit < changed.size())
		summary += "\n\n" + _("Run AI Shorten again for the rest, or edit them by hand. The Fit column shows where they stand.");
	summary += "\n\n" + _("Generate the dub track again to hear them in place.");
	wxMessageBox(summary, title, wxOK | wxICON_INFORMATION, c->parent);
}

struct tool_dub_shorten final : public Command {
	CMD_NAME("tool/dub/shorten")
	CMD_ICON(dub_shorten_button)
	STR_MENU("AI &Shorten Long Lines...")
	STR_DISP("AI Shorten Long Lines")
	STR_HELP("Rewrite lines whose generated speech runs past their time, shorter but with the same meaning")

	void operator()(agi::Context *c) override {
		shorten_lines(c);
	}
};

struct tool_dub_render final : public Command {
	CMD_NAME("tool/dub/render")
	CMD_ICON(dub_render_button)
	STR_MENU("&Render Dubbed Video...")
	STR_DISP("Render Dubbed Video")
	STR_HELP("Separate the video's voices from its background, mix in the dub and write a dubbed copy of the video")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ShowDubRenderDialog(c);
	}
};
}

namespace cmd {
	void init_dub() {
		reg(std::make_unique<tool_emotion_detect_all>());
		reg(std::make_unique<tool_emotion_detect_selected>());
		reg(std::make_unique<tool_dub_generate>());
		reg(std::make_unique<tool_dub_preview>());
		reg(std::make_unique<tool_dub_render>());
		reg(std::make_unique<tool_dub_shorten>());
	}
}
