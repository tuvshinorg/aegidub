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

/// @file translate.cpp
/// @brief AI translation commands and original-text management

#include "command.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../ai_batch.h"
#include "../compat.h"
#include "../dialog_progress.h"
#include "../dialogs.h"
#include "../dub_settings.h"
#include "../format.h"
#include "../include/aegisub/context.h"
#include "../line_spoken.h"
#include "../libresrc/libresrc.h"
#include "../openai_client.h"
#include "../options.h"
#include "../original_text.h"
#include "../project.h"
#include "../selection_controller.h"
#include "../series_cast.h"
#include "../speaker_diarization.h"
#include "../subs_controller.h"
#include "../video_controller.h"
#include "../voice_separator.h"

#include <libaegisub/address_of_adaptor.h>
#include <libaegisub/background_runner.h>
#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <algorithm>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>
#include <wx/utils.h>

namespace {
	using cmd::Command;

struct validate_sel_nonempty : public Command {
	CMD_TYPE(COMMAND_VALIDATE)
	bool Validate(const agi::Context *c) override {
		return c->selectionController->GetSelectedSet().size() > 0;
	}
};

struct Job {
	AssDialogue *line;
	std::string actor;
	std::string source;
	std::string translated;
	std::string spoken;
	bool done = false;
};

openai::Config load_config() {
	return dub::LoadOpenAIConfig();
}

/// Make model output safe to store as a single-line ASS dialogue text
std::string sanitize(std::string text) {
	boost::replace_all(text, "\r\n", "\\N");
	boost::replace_all(text, "\n", "\\N");
	boost::replace_all(text, "\r", "");
	boost::trim(text);
	return text;
}

void translate_lines(agi::Context *c, std::vector<AssDialogue *> const& lines) {
	c->videoController->Stop();

	auto config = load_config();
	if (config.api_key.empty()) {
		wxMessageBox(
			_("No API key is set.\n\nEnter your OpenAI API key in Preferences > AI Translation, or set the OPENAI_API_KEY environment variable."),
			_("AI Translation"), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}
	if (config.base_url.empty() || config.model.empty() || config.target_language.empty()) {
		wxMessageBox(
			_("The API URL, model and target language must all be set in Preferences > AI Translation."),
			_("AI Translation"), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	// Always translate from the stored original when there is one, so that
	// re-running the translation never translates a translation.
	std::vector<Job> jobs;
	for (auto line : lines) {
		if (line->Comment) continue;
		std::string source = original_text::Get(c->ass.get(), line);
		if (source.empty())
			source = line->Text.get();
		if (boost::trim_copy(source).empty()) continue;
		jobs.push_back(Job{line, line->Actor.get(), std::move(source), {}, {}, false});
	}

	if (jobs.empty()) {
		wxMessageBox(_("There is nothing to translate."), _("AI Translation"), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	int answer = wxMessageBox(
		fmt_tl("Translate %d lines into %s using %s?\n\nThe current text of each line is kept as its original and stays visible in the Original column.",
			(int)jobs.size(), config.target_language, config.model),
		_("AI Translation"), wxYES_NO | wxICON_QUESTION, c->parent);
	if (answer != wxYES) return;

	size_t batch_size = std::max<int64_t>(1, OPT_GET("Tool/AI Translation/Batch Size")->GetInt());
	const size_t context_size = 6;
	std::string error;

	DialogProgress progress(c->parent, _("AI Translation"), _("Translating..."));
	progress.Run([&](agi::ProgressSink *ps) {
		ps->SetProgress(0, jobs.size());
		std::vector<std::pair<std::string, std::string>> context;

		for (size_t start = 0; start < jobs.size(); start += batch_size) {
			if (ps->IsCancelled()) return;
			size_t end = std::min(jobs.size(), start + batch_size);
			ps->SetMessage(from_wx(fmt_tl("Translating lines %d-%d of %d", (int)start + 1, (int)end, (int)jobs.size())));

			std::vector<openai::Line> batch;
			for (size_t i = start; i < end; ++i)
				batch.push_back(openai::Line{(int)i, jobs[i].actor, jobs[i].source});

			std::map<int, openai::Translation> result;
			try {
				result = openai::Translate(config, batch, context, [ps] { return ps->IsCancelled(); });
			}
			catch (openai::Error const& e) {
				if (!ps->IsCancelled())
					error = e.what();
				return;
			}

			context.clear();
			for (size_t i = start; i < end; ++i) {
				auto it = result.find((int)i);
				if (it == result.end()) continue;
				std::string text = sanitize(it->second.text);
				if (text.empty()) continue;
				jobs[i].translated = std::move(text);
				jobs[i].spoken = it->second.spoken;
				jobs[i].done = true;
				if (end - i <= context_size)
					context.emplace_back(jobs[i].source, jobs[i].translated);
			}

			ps->SetProgress(end, jobs.size());
		}
	});

	// Apply whatever was completed, even after an error or cancel: those
	// batches have already been paid for.
	size_t applied = 0;
	for (auto const& job : jobs) {
		if (!job.done) continue;
		if (!original_text::Has(c->ass.get(), job.line))
			original_text::Set(c->ass.get(), job.line, job.line->Text.get());
		job.line->Text = job.translated;
		// Set after the text, which the spoken form is tied to
		line_spoken::Set(c->ass.get(), job.line, job.spoken);
		++applied;
	}
	if (applied)
		c->ass->Commit(_("AI translation"), AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_EXTRADATA);

	if (!error.empty()) {
		wxMessageBox(
			fmt_tl("Translation stopped after %d of %d lines:\n\n%s", (int)applied, (int)jobs.size(), error),
			_("AI Translation"), wxOK | wxICON_ERROR, c->parent);
	}
	else if (applied < jobs.size()) {
		wxMessageBox(
			fmt_tl("%d of %d lines were translated. The remaining lines were left unchanged; select them and run the translation again.", (int)applied, (int)jobs.size()),
			_("AI Translation"), wxOK | wxICON_WARNING, c->parent);
	}
}

struct tool_translate_ai_selected final : public validate_sel_nonempty {
	CMD_NAME("tool/translate/ai/selected")
	STR_MENU("AI Translate &Selected Lines")
	STR_DISP("AI Translate Selected Lines")
	STR_HELP("Translate the selected lines with the configured AI model, keeping the original text")

	void operator()(agi::Context *c) override {
		translate_lines(c, c->selectionController->GetSortedSelection());
	}
};

struct tool_translate_ai_all final : public Command {
	CMD_NAME("tool/translate/ai/all")
	STR_MENU("AI Translate &All Lines")
	STR_DISP("AI Translate All Lines")
	STR_HELP("Translate every dialogue line with the configured AI model, keeping the original text")

	void operator()(agi::Context *c) override {
		std::vector<AssDialogue *> lines;
		for (auto& line : c->ass->Events)
			lines.push_back(&line);
		translate_lines(c, lines);
	}
};

struct tool_translate_original_restore final : public validate_sel_nonempty {
	CMD_NAME("tool/translate/original/restore")
	STR_MENU("&Restore Original Text")
	STR_DISP("Restore Original Text")
	STR_HELP("Replace the text of the selected lines with their stored original text")

	void operator()(agi::Context *c) override {
		size_t restored = 0;
		for (auto line : c->selectionController->GetSortedSelection()) {
			if (!original_text::Has(c->ass.get(), line)) continue;
			line->Text = original_text::Get(c->ass.get(), line);
			original_text::Clear(c->ass.get(), line);
			++restored;
		}
		if (restored)
			c->ass->Commit(_("restore original text"), AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_EXTRADATA);
		else
			wxMessageBox(_("None of the selected lines have a stored original text."), _("Restore Original Text"), wxOK | wxICON_INFORMATION, c->parent);
	}
};

struct tool_translate_original_store final : public validate_sel_nonempty {
	CMD_NAME("tool/translate/original/store")
	STR_MENU("&Keep Current Text as Original")
	STR_DISP("Keep Current Text as Original")
	STR_HELP("Store the current text of the selected lines as their original text, for translating by hand")

	void operator()(agi::Context *c) override {
		size_t stored = 0;
		for (auto line : c->selectionController->GetSortedSelection()) {
			if (original_text::Has(c->ass.get(), line)) continue;
			if (line->Text.get().empty()) continue;
			original_text::Set(c->ass.get(), line, line->Text.get());
			++stored;
		}
		if (stored)
			c->ass->Commit(_("keep original text"), AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_EXTRADATA);
	}
};

/// Lines sent to the model per speaker detection request
const size_t speaker_batch_size = 60;
/// Already labelled lines sent along with each batch so the conversation carries over
const size_t speaker_context_size = 25;

struct SpeakerJob {
	AssDialogue *line;
	std::string text;
	std::string speaker;
	bool done = false;
	/// Which voice the audio says this is, e.g. "V3", if known
	std::string voice;
};

/// Line text as spoken, preferring the stored original since names and
/// forms of address in the source language give the most clues
std::string speaker_source_text(agi::Context *c, AssDialogue *line) {
	std::string text = original_text::Get(c->ass.get(), line);
	return dub::SpokenText(text.empty() ? line->Text.get() : text);
}

/// Hint at what the script is, so the model can use what it knows of the story
std::string script_title(agi::Context *c) {
	std::string title;
	auto subs = c->subsController->Filename();
	if (!subs.empty())
		title = subs.stem().string();
	auto const& video = c->project->VideoName();
	if (!video.empty())
		title += (title.empty() ? "" : " / ") + video.stem().string();
	return title;
}

/// @param voices The lines carry voices found in the audio
std::string speaker_prompt(bool voices) {
	std::string voice_rules = !voices ? "" :
		"- `voice` comes from analysing the audio. A change of voice between neighbouring lines almost always "
		"means a change of speaker, and the main characters each have a voice of their own. But one voice can "
		"also cover several similar-sounding minor characters, children or crowds, so within a voice let names "
		"and the conversation decide. A line without `voice` had no clear speech, so it is often on-screen text.\n"
		"- `characters_of_voices_so_far` gives the character most often chosen for each voice so far; prefer it "
		"when nothing else points elsewhere.\n";
	return
		"You label who speaks each line of a subtitle script so it can be voice dubbed.\n"
		"For every entry in `lines`, give the character who speaks it.\n"
		"Rules:\n"
		"- `title` names the show or film. Use what you know about its characters and plot.\n"
		"- Use the conversation: names used to address someone, who is replying to whom, "
		"who spoke just before, and `start` times in seconds (a long gap often means a new scene).\n"
		"- A line beginning with '-' can hold two speakers; give the first one.\n"
		+ voice_rules +
		"- Use one spelling per character: reuse names from `known_characters` exactly. "
		"Otherwise use the character's romanised name as it would appear in the subtitles.\n"
		"- Use \"Narrator\" for narration. For minor unnamed characters use a short role such as "
		"\"Soldier\" or \"Secretary\".\n"
		"- Use an empty string for on-screen text, signs, song lyrics and credits, or for "
		"lines that cannot be attributed at all.\n"
		"Reply with a single JSON object of the form "
		"{\"lines\":[{\"id\":<id>,\"character\":\"<name>\"}]} containing exactly the ids you were given.";
}

std::string speaker_request(std::string const& title, std::set<std::string> const& known,
	std::vector<SpeakerJob> const& jobs, size_t start, size_t end)
{
	json::Object root;
	if (!title.empty())
		root.emplace("title", json::UnknownElement(title));

	json::Array known_arr;
	for (auto const& name : known)
		known_arr.emplace_back(json::UnknownElement(name));
	root.emplace("known_characters", json::UnknownElement(std::move(known_arr)));

	// The character each voice has been given most often in earlier batches
	std::map<std::string, std::map<std::string, size_t>> voice_names;
	for (size_t i = 0; i < start; ++i) {
		if (jobs[i].done && !jobs[i].voice.empty() && !jobs[i].speaker.empty())
			++voice_names[jobs[i].voice][jobs[i].speaker];
	}
	if (!voice_names.empty()) {
		json::Object voices;
		for (auto const& [voice, names] : voice_names) {
			auto best = std::max_element(names.begin(), names.end(),
				[](auto const& a, auto const& b) { return a.second < b.second; });
			voices.emplace(voice, json::UnknownElement(best->first));
		}
		root.emplace("characters_of_voices_so_far", json::UnknownElement(std::move(voices)));
	}

	json::Array ctx;
	for (size_t i = start > speaker_context_size ? start - speaker_context_size : 0; i < start; ++i) {
		if (!jobs[i].done) continue;
		json::Object entry;
		entry.emplace("character", json::UnknownElement(jobs[i].speaker));
		if (!jobs[i].voice.empty())
			entry.emplace("voice", json::UnknownElement(jobs[i].voice));
		entry.emplace("text", json::UnknownElement(jobs[i].text));
		ctx.emplace_back(std::move(entry));
	}
	if (!ctx.empty())
		root.emplace("previous_lines_for_context_only", json::UnknownElement(std::move(ctx)));

	json::Array arr;
	for (size_t i = start; i < end; ++i) {
		json::Object entry;
		entry.emplace("id", json::UnknownElement(static_cast<int64_t>(i)));
		entry.emplace("start", json::UnknownElement(static_cast<int64_t>(static_cast<int>(jobs[i].line->Start) / 1000)));
		if (!jobs[i].voice.empty())
			entry.emplace("voice", json::UnknownElement(jobs[i].voice));
		entry.emplace("text", json::UnknownElement(jobs[i].text));
		arr.emplace_back(std::move(entry));
	}
	root.emplace("lines", json::UnknownElement(std::move(arr)));

	std::ostringstream ss;
	agi::JsonWriter::Write(json::UnknownElement(std::move(root)), ss);
	return ss.str();
}

/// Make a model-given name safe for the Actor field
std::string clean_speaker(std::string name) {
	// A comma would end the field when the line is written to the file
	boost::replace_all(name, ",", ";");
	boost::replace_all(name, "\n", " ");
	boost::replace_all(name, "\r", "");
	boost::trim(name);
	if (name.size() > 60) {
		size_t cut = 60;
		// Don't cut a UTF-8 sequence in half
		while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80)
			--cut;
		name.resize(cut);
	}
	return name;
}

/// Ask for a Hugging Face token for pyannote and save it in the options
/// @return The token, or empty if cancelled
std::string ask_hf_token(wxWindow *parent, wxString const& reason) {
	wxTextEntryDialog dialog(parent, reason + "\n\n" + fmt_tl(
		"1. Sign in at huggingface.co (a free account is enough).\n"
		"2. Open %s and accept its terms.\n"
		"3. Create a token with Read access at huggingface.co/settings/tokens and paste it here.",
		speaker_diarization::model_page),
		_("Detect Speakers from Audio"), to_wx(OPT_GET("Tool/Speaker Detection/Hugging Face Token")->GetString()));
	if (dialog.ShowModal() != wxID_OK) return "";
	std::string token = boost::trim_copy(from_wx(dialog.GetValue()));
	OPT_SET("Tool/Speaker Detection/Hugging Face Token")->SetString(token);
	return token;
}

/// Group the lines by voice using the video's soundtrack: separate the voices
/// from the music, then tell them apart with pyannote
/// @return false to stop detecting speakers altogether
bool find_voices(agi::Context *c, std::vector<SpeakerJob>& jobs) {
	// Only needed to download the model when it wasn't installed with aegidub
	std::string token = OPT_GET("Tool/Speaker Detection/Hugging Face Token")->GetString();
	wxString env_token;
	if (token.empty() && wxGetEnv("HF_TOKEN", &env_token))
		token = from_wx(env_token);
	if (token.empty() && !speaker_diarization::Bundled()) {
		token = ask_hf_token(c->parent, _("Detecting speakers from the audio uses pyannote, whose models need a Hugging Face access token."));
		if (token.empty()) return false;
	}

	auto const& video = c->project->VideoName();
	auto work_dir = agi::fs::path(dub::Folder(c->subsController->Filename()) / "render");
	const bool gpu = OPT_GET("Tool/Dub Render/Use GPU If Available")->GetBool();

	for (;;) {
		std::vector<speaker_diarization::Turn> turns;
		std::string error;
		bool cancelled = false, needs_access = false;

		DialogProgress progress(c->parent, _("Detect Speakers from Audio"), _("Preparing..."));
		progress.Run([&](agi::ProgressSink *ps) {
			auto is_cancelled = [ps] { return ps->IsCancelled(); };
			auto show = [ps](std::string const& message, double fraction) {
				ps->SetMessage(message);
				if (fraction < 0)
					ps->SetIndeterminate();
				else
					ps->SetProgress(static_cast<int64_t>(fraction * 1000), 1000);
			};

			try {
				// The separated voices are kept for rendering the dub later
				voice_separator::SeparateVideo(video, work_dir,
					[&](int step, std::string const& message, double f) {
						if (step == 0)
							show(from_wx(_("Step 1 of 3: reading the video's sound...")), f);
						else
							show(from_wx(_("Step 2 of 3: ")) + message, f);
					},
					is_cancelled);
				turns = speaker_diarization::Diarize(agi::fs::path(work_dir / "vocals.wav"), token, gpu,
					[&](std::string const& message, double f) { show(from_wx(_("Step 3 of 3: ")) + message, f); },
					is_cancelled);
			}
			catch (voice_separator::Cancelled const&) { cancelled = true; }
			catch (speaker_diarization::Cancelled const&) { cancelled = true; }
			catch (speaker_diarization::NeedsAccess const&) { needs_access = true; }
			catch (agi::Exception const& e) { error = e.GetMessage(); }
			catch (std::exception const& e) { error = e.what(); }
		});

		if (cancelled) return false;
		if (needs_access) {
			token = ask_hf_token(c->parent, _("Hugging Face did not accept the token. Check it, and that its account has accepted the model's terms."));
			if (token.empty()) return false;
			continue;
		}
		if (error.empty() && turns.empty())
			error = from_wx(_("No speech was found in the video's sound."));
		if (!error.empty()) {
			return wxMessageBox(fmt_tl("Could not tell the voices apart:\n\n%s\n\nGuess the speakers from the text only?", error),
				_("Detect Speakers from Audio"), wxYES_NO | wxICON_WARNING, c->parent) == wxYES;
		}

		std::vector<std::pair<int, int>> ranges;
		for (auto const& job : jobs)
			ranges.emplace_back(static_cast<int>(job.line->Start), static_cast<int>(job.line->End));
		auto voices = speaker_diarization::VoiceOfEach(ranges, turns);
		for (size_t i = 0; i < jobs.size(); ++i)
			jobs[i].voice = voices[i];
		return true;
	}
}

/// @param overwrite  Also relabel lines which already have a character
/// @param from_audio Group the lines by voice before naming the characters
void detect_speakers(agi::Context *c, std::vector<AssDialogue *> const& lines, bool overwrite, bool from_audio = false) {
	c->videoController->Stop();
	const wxString caption = from_audio ? _("Detect Speakers from Audio") : _("AI Detect Speakers");

	auto config = load_config();
	if (config.api_key.empty() || config.base_url.empty() || config.model.empty()) {
		wxMessageBox(
			_("Speaker detection uses the AI model set up for translation.\n\nEnter an API key, URL and model in Preferences > AI Translation."),
			caption, wxOK | wxICON_INFORMATION, c->parent);
		return;
	}
	if (from_audio && c->project->VideoName().empty()) {
		wxMessageBox(_("Open the video first (Video > Open Video)."), caption, wxOK | wxICON_INFORMATION, c->parent);
		return;
	}
	if (from_audio && c->subsController->Filename().empty()) {
		wxMessageBox(_("Save the subtitles first. The separated audio is kept next to them."),
			caption, wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	std::vector<SpeakerJob> jobs;
	std::set<std::string> known;
	for (auto& line : c->ass->Events) {
		if (!line.Actor.get().empty())
			known.insert(line.Actor.get());
	}
	// Spell recurring characters exactly as earlier episodes did, so they
	// keep their voices from the series cast
	try {
		for (auto const& entry : series_cast::Load(series_cast::Find(c->ass.get(), c->subsController->Filename())))
			known.insert(entry.first);
	}
	catch (agi::Exception const&) {
		// An unreadable series file only costs the naming hint
	}
	for (auto line : lines) {
		if (line->Comment) continue;
		if (!overwrite && !line->Actor.get().empty()) continue;
		std::string text = speaker_source_text(c, line);
		if (text.empty()) continue;
		jobs.push_back(SpeakerJob{line, std::move(text), {}, false});
	}

	if (jobs.empty()) {
		wxMessageBox(overwrite
			? _("The selected lines have no text to label.")
			: _("Every line already has a character. Select lines and use AI Detect Speakers for Selected Lines to label them again."),
			caption, wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	int answer = wxMessageBox(from_audio
		? fmt_tl("Detect the speakers of %d lines?\n\nFirst the voices are separated from the music and grouped by how they sound; the first time, this installs pyannote, which takes several minutes. Then %s names the character of each voice from the lines.\n\nCheck the Character column afterwards. This can be undone.",
			(int)jobs.size(), config.model)
		: fmt_tl("Guess the speaker of %d lines using %s?\n\nThe model only sees the text, so check the Character column afterwards. This can be undone.",
			(int)jobs.size(), config.model),
		caption, wxYES_NO | wxICON_QUESTION, c->parent);
	if (answer != wxYES) return;

	if (from_audio && !find_voices(c, jobs)) return;
	const bool have_voices = std::any_of(jobs.begin(), jobs.end(), [](SpeakerJob const& job) { return !job.voice.empty(); });

	std::string title = script_title(c);
	std::string error;

	DialogProgress progress(c->parent, caption, _("Detecting speakers..."));
	progress.Run([&](agi::ProgressSink *ps) {
		ai_batch::Job batch;
		batch.count = jobs.size();
		batch.batch_size = speaker_batch_size;
		batch.system = speaker_prompt(have_voices);
		batch.request = [&](size_t start, size_t end) { return speaker_request(title, known, jobs, start, end); };
		batch.message = [&](size_t start, size_t end) {
			return from_wx(fmt_tl("Detecting speakers of lines %d-%d of %d", (int)start + 1, (int)end, (int)jobs.size()));
		};
		batch.on_item = [&](size_t i, json::Object const& item) {
			auto name = item.find("character");
			if (name == item.end()) return;
			jobs[i].speaker = clean_speaker(static_cast<json::String const&>(name->second));
			jobs[i].done = true;
			// Later batches spell this character the same way
			if (!jobs[i].speaker.empty())
				known.insert(jobs[i].speaker);
		};
		error = ai_batch::Run(config, batch, ps);
	});

	// Apply whatever was completed, even after an error or cancel: those
	// batches have already been paid for.
	size_t labelled = 0;
	size_t changed = 0;
	for (auto const& job : jobs) {
		if (!job.done) continue;
		++labelled;
		if (job.line->Actor.get() == job.speaker) continue;
		job.line->Actor = job.speaker;
		++changed;
	}
	if (changed)
		c->ass->Commit(_("detect speakers"), AssFile::COMMIT_DIAG_META);

	if (!error.empty()) {
		wxMessageBox(
			fmt_tl("Speaker detection stopped after %d of %d lines:\n\n%s", (int)labelled, (int)jobs.size(), error),
			caption, wxOK | wxICON_ERROR, c->parent);
	}
	else if (labelled < jobs.size()) {
		wxMessageBox(
			fmt_tl("%d of %d lines were labelled. Run it again to label the rest.", (int)labelled, (int)jobs.size()),
			caption, wxOK | wxICON_WARNING, c->parent);
	}
}

struct tool_speakers_detect_all final : public Command {
	CMD_NAME("tool/speakers/detect/all")
	CMD_ICON(speakers_detect_button)
	STR_MENU("AI &Detect Speakers")
	STR_DISP("AI Detect Speakers")
	STR_HELP("Guess with the configured AI model who speaks each line that has no character yet")

	void operator()(agi::Context *c) override {
		std::vector<AssDialogue *> lines;
		for (auto& line : c->ass->Events)
			lines.push_back(&line);
		detect_speakers(c, lines, false);
	}
};

struct tool_speakers_detect_audio final : public Command {
	CMD_NAME("tool/speakers/detect/audio")
	CMD_ICON(speakers_detect_button)
	STR_MENU("Detect Speakers from &Audio")
	STR_DISP("Detect Speakers from Audio")
	STR_HELP("Group the lines that have no character yet by voice from the video's sound, then name the characters with the configured AI model")

	void operator()(agi::Context *c) override {
		std::vector<AssDialogue *> lines;
		for (auto& line : c->ass->Events)
			lines.push_back(&line);
		detect_speakers(c, lines, false, true);
	}
};

struct tool_speakers_detect_selected final : public validate_sel_nonempty {
	CMD_NAME("tool/speakers/detect/selected")
	CMD_ICON(speakers_detect_button)
	STR_MENU("AI Detect Speakers for Se&lected Lines")
	STR_DISP("AI Detect Speakers for Selected Lines")
	STR_HELP("Guess with the configured AI model who speaks each selected line, replacing its character")

	void operator()(agi::Context *c) override {
		detect_speakers(c, c->selectionController->GetSortedSelection(), true);
	}
};

struct tool_voice_cast final : public Command {
	CMD_NAME("tool/voice/cast")
	CMD_ICON(voice_cast_button)
	STR_MENU("&Voice Cast...")
	STR_DISP("Voice Cast")
	STR_HELP("Assign a dubbing voice to every character, automatically or by hand, and preview them")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ShowVoiceCastDialog(c);
	}
};
}

namespace cmd {
	void init_translate() {
		reg(std::make_unique<tool_translate_ai_selected>());
		reg(std::make_unique<tool_translate_ai_all>());
		reg(std::make_unique<tool_translate_original_restore>());
		reg(std::make_unique<tool_translate_original_store>());
		reg(std::make_unique<tool_speakers_detect_all>());
		reg(std::make_unique<tool_speakers_detect_audio>());
		reg(std::make_unique<tool_speakers_detect_selected>());
		reg(std::make_unique<tool_voice_cast>());
	}
}
