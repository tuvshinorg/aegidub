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

/// @file dialog_soft_subs.cpp
/// @brief Save a copy of the video with the subtitles as a switchable track

#include "compat.h"
#include "dialog_progress.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "soft_subs.h"

#include <libaegisub/exception.h>
#include <libaegisub/path.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace {
const char *title = "Export Video with Soft Subtitles";

/// @return false if cancelled
bool ask_settings(agi::Context *c, soft_subs::Settings& s) {
	wxDialog d(c->parent, -1, _(title));
	auto sizer = new wxBoxSizer(wxVERTICAL);

	auto intro = new wxStaticText(&d, -1,
		_("Writes a copy of the video with the subtitles as a track viewers can switch on and off. The picture and sound are copied as they are, so this is quick. MKV keeps the styling; MP4 gets plain text."));
	intro->Wrap(d.FromDIP(480));
	sizer->Add(intro, wxSizerFlags().Border(wxALL, 8));

	auto video = s.video;
	auto default_output = agi::fs::path(video.parent_path() / (video.stem().string() + ".softsub.mkv"));
	auto out_row = new wxBoxSizer(wxHORIZONTAL);
	auto output = new wxTextCtrl(&d, -1, to_wx(default_output.string()));
	output->SetMinSize(wxSize(d.FromDIP(380), -1));
	auto browse = new wxButton(&d, -1, _("&Browse..."));
	out_row->Add(output, wxSizerFlags(1).CenterVertical().Border(wxRIGHT, 4));
	out_row->Add(browse, wxSizerFlags().CenterVertical());
	sizer->Add(out_row, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

	auto lang_row = new wxBoxSizer(wxHORIZONTAL);
	auto language = new wxTextCtrl(&d, -1, to_wx(OPT_GET("Tool/Soft Subtitles/Language")->GetString()));
	language->SetMaxLength(3);
	lang_row->Add(new wxStaticText(&d, -1, _("Track language (3-letter code, e.g. mon, eng):")), wxSizerFlags().CenterVertical().Border(wxRIGHT, 4));
	lang_row->Add(language, wxSizerFlags().CenterVertical());
	sizer->Add(lang_row, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

	auto keep = new wxCheckBox(&d, -1, _("Keep the video's existing subtitle tracks (MKV only)"));
	keep->SetValue(OPT_GET("Tool/Soft Subtitles/Keep Existing")->GetBool());
	sizer->Add(keep, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

	auto buttons = new wxStdDialogButtonSizer;
	auto ok = new wxButton(&d, wxID_OK, _("&Export"));
	ok->SetDefault();
	buttons->AddButton(ok);
	buttons->AddButton(new wxButton(&d, wxID_CANCEL));
	buttons->Realize();
	sizer->Add(buttons, wxSizerFlags().Expand().Border(wxALL, 8));
	d.SetSizerAndFit(sizer);
	d.CenterOnParent();

	browse->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
		wxFileDialog dialog(&d, _("Save video with soft subtitles"), "", "",
			_("Matroska video (*.mkv)|*.mkv|MP4 video (*.mp4)|*.mp4"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
		dialog.SetPath(output->GetValue());
		if (dialog.ShowModal() == wxID_OK)
			output->SetValue(dialog.GetPath());
	});
	ok->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
		auto path = agi::fs::path(from_wx(output->GetValue()));
		if (!agi::fs::HasExtension(path, "mkv") && !agi::fs::HasExtension(path, "mp4")) {
			wxMessageBox(_("Choose an output file ending in .mkv or .mp4."), _(title), wxOK | wxICON_INFORMATION, &d);
			return;
		}
		if (path == video) {
			wxMessageBox(_("Choose a different file from the video itself."), _(title), wxOK | wxICON_INFORMATION, &d);
			return;
		}
		if (agi::fs::FileExists(path) &&
			wxMessageBox(fmt_tl("%s already exists. Replace it?", path.filename().string()), _(title),
				wxYES_NO | wxICON_QUESTION, &d) != wxYES)
			return;
		d.EndModal(wxID_OK);
	});

	if (d.ShowModal() != wxID_OK) return false;

	s.output = from_wx(output->GetValue());
	s.language = from_wx(language->GetValue().Lower());
	if (s.language.empty()) s.language = "und";
	s.keep_existing = keep->GetValue();
	OPT_SET("Tool/Soft Subtitles/Language")->SetString(s.language);
	OPT_SET("Tool/Soft Subtitles/Keep Existing")->SetBool(s.keep_existing);
	return true;
}
}

void ShowSoftSubsExportDialog(agi::Context *c) {
	if (c->project->VideoName().empty()) {
		wxMessageBox(_("Open the video first (Video > Open Video)."), _(title), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	soft_subs::Settings settings;
	settings.video = c->project->VideoName();
	if (!ask_settings(c, settings)) return;

	settings.subtitles = config::path->Decode("?temp/aegidub-softsub.ass");
	std::string error;
	bool cancelled = false;
	try {
		soft_subs::WriteSubtitles(c, settings.subtitles);
	}
	catch (agi::Exception const& e) {
		error = e.GetMessage();
	}

	if (error.empty()) {
		DialogProgress progress(c->parent, _(title), _("Writing the video..."));
		progress.Run([&](agi::ProgressSink *ps) {
			try {
				soft_subs::Export(settings,
					[ps](double f) { ps->SetProgress(static_cast<int64_t>(f * 1000), 1000); },
					[ps] { return ps->IsCancelled(); });
			}
			catch (soft_subs::Cancelled const&) { cancelled = true; }
			catch (agi::Exception const& e) { error = e.GetMessage(); }
			catch (std::exception const& e) { error = e.what(); }
		});
	}
	agi::fs::Remove(settings.subtitles);

	if (cancelled) return;
	if (!error.empty()) {
		wxMessageBox(fmt_tl("Exporting failed:\n\n%s", error), _(title), wxOK | wxICON_ERROR, c->parent);
		return;
	}
	if (wxMessageBox(fmt_tl("The video was written to:\n%s\n\nOpen the folder?", settings.output.string()),
			_(title), wxYES_NO | wxICON_INFORMATION, c->parent) == wxYES)
		wxLaunchDefaultApplication(to_wx(settings.output.parent_path().string()));
}
