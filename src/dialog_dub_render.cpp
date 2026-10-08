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

/// @file dialog_dub_render.cpp
/// @brief Render the finished dub into a copy of the video

#include "command/command.h"
#include "compat.h"
#include "dialog_progress.h"
#include "dub_render.h"
#include "dub_settings.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "project_store.h"
#include "subs_controller.h"
#include "voice_separator.h"

#include <libaegisub/fs.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace {
/// Dialog return code asking to generate the dub track and reopen
const int generate_code = wxID_HIGHEST + 1;

struct DialogDubRender {
	wxDialog d;
	agi::Context *c;
	agi::fs::path video;
	agi::fs::path track;
	agi::fs::path work_dir;

	wxCheckBox *keep_voices;
	wxSlider *voice_volume;
	wxStaticText *voice_volume_label;
	wxCheckBox *remaster;
	wxCheckBox *keep_track;
	wxTextCtrl *output;

	DialogDubRender(agi::Context *c);

	bool SeparationCached() const;
	void UpdateVolumeLabel();
	void OnBrowse(wxCommandEvent&);
	void OnRender(wxCommandEvent&);
};

DialogDubRender::DialogDubRender(agi::Context *c)
: d(c->parent, -1, _("Render Dubbed Video"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
, c(c)
, video(c->project->VideoName())
, track(dub::TrackPath(c->subsController->Filename()))
, work_dir(agi::fs::path(dub::Folder(c->subsController->Filename()) / "render"))
{
	auto sizer = new wxBoxSizer(wxVERTICAL);
	const int wrap = 520;

	// What goes in
	auto sources = new wxStaticBoxSizer(wxVERTICAL, &d, _("Sources"));
	auto add_line = [&](wxString const& text) {
		auto label = new wxStaticText(sources->GetStaticBox(), -1, text);
		label->Wrap(wrap);
		sources->Add(label, wxSizerFlags().Border(wxALL, 4));
		return label;
	};
	add_line(fmt_tl("Video: %s", video.filename().string()));

	bool have_track = agi::fs::FileExists(track);
	if (!have_track)
		add_line(_("Dub track: not generated yet. Click Generate dub track first."));
	else {
		add_line(fmt_tl("Dub track: %s", track.filename().string()));
		auto subs = c->subsController->Filename();
		if (agi::fs::FileExists(subs) && agi::fs::ModifiedTime(subs) > agi::fs::ModifiedTime(track))
			add_line(_("The subtitles were saved after the dub track was made. Generate it again if you changed any lines."))
				->SetForegroundColour(wxColour(200, 100, 0));
	}
	add_line(SeparationCached()
		? _("Voices: already separated for this video, so rendering is quick.")
		: _("Voices: will be separated from the background first. With an NVIDIA GPU and Python demucs this takes minutes; on the CPU about 1.5 times the video's length."));
	sizer->Add(sources, wxSizerFlags().Expand().Border(wxALL, 8));

	// How it is mixed
	auto mix = new wxStaticBoxSizer(wxVERTICAL, &d, _("Mix"));
	auto box = mix->GetStaticBox();
	keep_voices = new wxCheckBox(box, -1, _("Keep the original voices quietly under the dub"));
	keep_voices->SetValue(OPT_GET("Tool/Dub Render/Keep Original Voices")->GetBool());
	voice_volume = new wxSlider(box, -1, (int)OPT_GET("Tool/Dub Render/Original Voice Volume")->GetInt(), 1, 50);
	voice_volume_label = new wxStaticText(box, -1, "");
	auto volume_row = new wxBoxSizer(wxHORIZONTAL);
	volume_row->AddSpacer(20);
	volume_row->Add(voice_volume, wxSizerFlags(1).CenterVertical());
	volume_row->Add(voice_volume_label, wxSizerFlags().CenterVertical().Border(wxLEFT, 6));

	remaster = new wxCheckBox(box, -1, _("Auto remaster: even out the dub, lower the background while it speaks, and normalise the loudness"));
	remaster->SetValue(OPT_GET("Tool/Dub Render/Remaster")->GetBool());
	keep_track = new wxCheckBox(box, -1, _("Keep the original audio as a second track"));
	keep_track->SetValue(OPT_GET("Tool/Dub Render/Keep Original Track")->GetBool());

	mix->Add(keep_voices, wxSizerFlags().Border(wxALL, 4));
	mix->Add(volume_row, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 4));
	mix->Add(remaster, wxSizerFlags().Border(wxALL, 4));
	mix->Add(keep_track, wxSizerFlags().Border(wxALL, 4));
	sizer->Add(mix, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

	// Where it goes
	auto ext = video.extension().string() == ".mp4" ? ".mp4" : ".mkv";
	auto default_output = agi::fs::path(video.parent_path() / (video.stem().string() + ".mn-dub" + ext));
	project_store::Project project;
	if (project_store::FindForSubtitles(c->subsController->Filename(), project))
		default_output = agi::fs::path(project.folder / (project.id + ".mn-dub" + ext));
	auto out_box = new wxStaticBoxSizer(wxHORIZONTAL, &d, _("Output"));
	output = new wxTextCtrl(out_box->GetStaticBox(), -1, to_wx(default_output.string()));
	output->SetMinSize(wxSize(420, -1));
	auto browse = new wxButton(out_box->GetStaticBox(), -1, _("&Browse..."));
	out_box->Add(output, wxSizerFlags(1).CenterVertical().Border(wxALL, 4));
	out_box->Add(browse, wxSizerFlags().CenterVertical().Border(wxALL, 4));
	sizer->Add(out_box, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

	auto buttons = new wxBoxSizer(wxHORIZONTAL);
	auto generate = new wxButton(&d, -1, _("&Generate dub track..."));
	auto render = new wxButton(&d, wxID_OK, _("&Render"));
	render->Enable(have_track);
	render->SetDefault();
	buttons->Add(generate);
	buttons->AddStretchSpacer();
	buttons->Add(render, wxSizerFlags().Border(wxRIGHT, 5));
	buttons->Add(new wxButton(&d, wxID_CANCEL, _("Close")));
	sizer->Add(buttons, wxSizerFlags().Expand().Border(wxALL, 8));

	d.SetSizerAndFit(sizer);
	d.CenterOnParent();
	UpdateVolumeLabel();

	keep_voices->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { UpdateVolumeLabel(); });
	voice_volume->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { UpdateVolumeLabel(); });
	browse->Bind(wxEVT_BUTTON, &DialogDubRender::OnBrowse, this);
	generate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { d.EndModal(generate_code); });
	render->Bind(wxEVT_BUTTON, &DialogDubRender::OnRender, this);
}

bool DialogDubRender::SeparationCached() const {
	return voice_separator::VideoSeparated(video, work_dir);
}

void DialogDubRender::UpdateVolumeLabel() {
	voice_volume->Enable(keep_voices->GetValue());
	voice_volume_label->SetLabel(fmt_wx("%d%%", voice_volume->GetValue()));
	voice_volume_label->Enable(keep_voices->GetValue());
}

void DialogDubRender::OnBrowse(wxCommandEvent&) {
	wxFileDialog dialog(&d, _("Save dubbed video"), "", output->GetValue(),
		_("Matroska video (*.mkv)|*.mkv|MP4 video (*.mp4)|*.mp4"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	dialog.SetPath(output->GetValue());
	if (dialog.ShowModal() == wxID_OK)
		output->SetValue(dialog.GetPath());
}

void DialogDubRender::OnRender(wxCommandEvent&) {
	auto out_path = agi::fs::path(from_wx(output->GetValue()));
	if (out_path.empty() || (!agi::fs::HasExtension(out_path, "mkv") && !agi::fs::HasExtension(out_path, "mp4"))) {
		wxMessageBox(_("Choose an output file ending in .mkv or .mp4."), _("Render Dubbed Video"), wxOK | wxICON_INFORMATION, &d);
		return;
	}
	if (out_path == video) {
		wxMessageBox(_("Choose a different file from the video itself."), _("Render Dubbed Video"), wxOK | wxICON_INFORMATION, &d);
		return;
	}
	if (agi::fs::FileExists(out_path) &&
		wxMessageBox(fmt_tl("%s already exists. Replace it?", out_path.filename().string()), _("Render Dubbed Video"),
			wxYES_NO | wxICON_QUESTION, &d) != wxYES)
		return;

	OPT_SET("Tool/Dub Render/Keep Original Voices")->SetBool(keep_voices->GetValue());
	OPT_SET("Tool/Dub Render/Original Voice Volume")->SetInt(voice_volume->GetValue());
	OPT_SET("Tool/Dub Render/Remaster")->SetBool(remaster->GetValue());
	OPT_SET("Tool/Dub Render/Keep Original Track")->SetBool(keep_track->GetValue());

	dub_render::Settings settings;
	settings.video = video;
	settings.dub = track;
	settings.background = agi::fs::path(work_dir / "background.wav");
	settings.original_voices = agi::fs::path(work_dir / "vocals.wav");
	settings.original_volume = keep_voices->GetValue() ? voice_volume->GetValue() / 100.0 : 0;
	settings.remaster = remaster->GetValue();
	settings.keep_original_track = keep_track->GetValue();
	settings.output = out_path;

	std::string error, engine;
	bool cancelled = false;

	DialogProgress progress(&d, _("Render Dubbed Video"), _("Preparing..."));
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
			engine = voice_separator::SeparateVideo(video, work_dir,
				[&](int step, std::string const& message, double f) {
					if (step == 0)
						show(from_wx(_("Step 1 of 3: reading the video's sound...")), f);
					else
						show(from_wx(_("Step 2 of 3: ")) + message, f);
				},
				is_cancelled);

			show(from_wx(_("Step 3 of 3: mixing and writing the video...")), 0);
			dub_render::Render(settings,
				[&](double f) { show(from_wx(_("Step 3 of 3: mixing and writing the video...")), f); }, is_cancelled);
		}
		catch (voice_separator::Cancelled const&) { cancelled = true; }
		catch (dub_render::Cancelled const&) { cancelled = true; }
		catch (agi::Exception const& e) { error = e.GetMessage(); }
		catch (std::exception const& e) { error = e.what(); }
	});

	if (cancelled) return;
	if (!error.empty()) {
		wxMessageBox(fmt_tl("Rendering failed:\n\n%s", error), _("Render Dubbed Video"), wxOK | wxICON_ERROR, &d);
		return;
	}

	wxString done = fmt_tl("The dubbed video was written to:\n%s", out_path.string());
	if (!engine.empty())
		done += "\n\n" + fmt_tl("Voices were separated on the %s. Later renders of this video reuse them.", engine);
	done += "\n\n" + _("Open the folder?");
	if (wxMessageBox(done, _("Render Dubbed Video"), wxYES_NO | wxICON_INFORMATION, &d) == wxYES)
		wxLaunchDefaultApplication(to_wx(out_path.parent_path().string()));
	d.EndModal(wxID_OK);
}
}

void ShowDubRenderDialog(agi::Context *c) {
	if (c->project->VideoName().empty()) {
		wxMessageBox(_("Open the video first (Video > Open Video)."), _("Render Dubbed Video"), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}
	if (c->subsController->Filename().empty()) {
		wxMessageBox(_("Save the subtitles first. The dub and the separated audio are kept next to them."),
			_("Render Dubbed Video"), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	for (;;) {
		int result;
		{
			DialogDubRender dlg(c);
			result = dlg.d.ShowModal();
		}
		if (result != generate_code) break;
		cmd::call("tool/dub/generate", c);
	}
}
