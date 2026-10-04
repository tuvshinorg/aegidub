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
#include "../compat.h"
#include "../dialog_progress.h"
#include "../format.h"
#include "../include/aegisub/context.h"
#include "../openai_client.h"
#include "../options.h"
#include "../original_text.h"
#include "../selection_controller.h"
#include "../video_controller.h"

#include <libaegisub/address_of_adaptor.h>
#include <libaegisub/background_runner.h>

#include <algorithm>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <cstdlib>
#include <wx/msgdlg.h>

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
	bool done = false;
};

openai::Config load_config() {
	openai::Config config;
	config.api_key = OPT_GET("Tool/AI Translation/API Key")->GetString();
	if (config.api_key.empty()) {
		if (const char *env = std::getenv("OPENAI_API_KEY"))
			config.api_key = env;
	}
	boost::trim(config.api_key);
	config.base_url = OPT_GET("Tool/AI Translation/Base URL")->GetString();
	config.model = OPT_GET("Tool/AI Translation/Model")->GetString();
	config.target_language = OPT_GET("Tool/AI Translation/Target Language")->GetString();
	config.instructions = OPT_GET("Tool/AI Translation/Instructions")->GetString();
	boost::trim(config.base_url);
	boost::trim(config.model);
	boost::trim(config.target_language);
	return config;
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
		jobs.push_back(Job{line, line->Actor.get(), std::move(source), {}, false});
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

			std::map<int, std::string> result;
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
				std::string text = sanitize(it->second);
				if (text.empty()) continue;
				jobs[i].translated = std::move(text);
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
}

namespace cmd {
	void init_translate() {
		reg(std::make_unique<tool_translate_ai_selected>());
		reg(std::make_unique<tool_translate_ai_all>());
		reg(std::make_unique<tool_translate_original_restore>());
		reg(std::make_unique<tool_translate_original_store>());
	}
}
