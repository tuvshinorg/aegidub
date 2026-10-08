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

/// @file dialog_hardsub_extract.cpp
/// @brief Turn subtitles burned into the video into editable lines

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "compat.h"
#include "dialog_progress.h"
#include "format.h"
#include "hardsub_extractor.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "subtitle_format.h"
#include "video_controller.h"
#include "video_frame.h"

#include <libaegisub/exception.h>
#include <libaegisub/path.h>

#include <algorithm>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace {
const char *title = "Extract Burned-in Subtitles";

struct Language {
	const char *code; ///< video-subtitle-extractor's name for it
	const char *name;
};

const Language languages[] = {
	{"en", "English"},
	{"ch", "Chinese (Simplified)"},
	{"chinese_cht", "Chinese (Traditional)"},
	{"japan", "Japanese"},
	{"korean", "Korean"},
	{"mn", "Mongolian"},
	{"ru", "Russian"},
	{"uk", "Ukrainian"},
	{"ar", "Arabic"},
	{"fa", "Persian"},
	{"hi", "Hindi"},
	{"ta", "Tamil"},
	{"te", "Telugu"},
	{"kn", "Kannada"},
	{"de", "German"},
	{"fr", "French"},
	{"es", "Spanish"},
	{"pt", "Portuguese"},
	{"it", "Italian"},
	{"nl", "Dutch"},
	{"pl", "Polish"},
	{"tr", "Turkish"},
	{"vi", "Vietnamese"},
	{"id", "Indonesian"},
	{"ms", "Malay"},
};

/// The current video frame, with a box the user drags around the subtitles
class AreaPicker final : public wxWindow {
	wxBitmap frame;
	/// The box, as fractions of the frame size
	double top, bottom, left, right;
	wxPoint anchor;

	void OnPaint(wxPaintEvent&) {
		wxAutoBufferedPaintDC dc(this);
		dc.DrawBitmap(frame, 0, 0);
		auto size = GetClientSize();
		wxRect box(wxPoint(left * size.x, top * size.y), wxPoint(right * size.x, bottom * size.y));
		dc.SetBrush(*wxTRANSPARENT_BRUSH);
		dc.SetPen(wxPen(wxColour(0, 255, 0), FromDIP(2)));
		dc.DrawRectangle(box);
	}

	double clamp(double v) { return std::clamp(v, 0.0, 1.0); }

	void SetBox(wxPoint a, wxPoint b) {
		auto size = GetClientSize();
		left = clamp((double)std::min(a.x, b.x) / size.x);
		right = clamp((double)std::max(a.x, b.x) / size.x);
		top = clamp((double)std::min(a.y, b.y) / size.y);
		bottom = clamp((double)std::max(a.y, b.y) / size.y);
		Refresh(false);
	}

	void OnMouse(wxMouseEvent& evt) {
		if (evt.LeftDown()) {
			anchor = evt.GetPosition();
			CaptureMouse();
		}
		else if (evt.Dragging() && HasCapture())
			SetBox(anchor, evt.GetPosition());
		else if (evt.LeftUp() && HasCapture()) {
			ReleaseMouse();
			// A click without a drag would leave a box too small to hold anything
			if ((right - left) * GetClientSize().x < 8 || (bottom - top) * GetClientSize().y < 8)
				SetBox(wxPoint(0, anchor.y - GetClientSize().y / 10), wxPoint(GetClientSize().x, anchor.y + GetClientSize().y / 10));
		}
	}

public:
	AreaPicker(wxWindow *parent, wxImage const& image, double top, double bottom, double left, double right)
	: wxWindow(parent, -1)
	, top(top), bottom(bottom), left(left), right(right)
	{
		SetBackgroundStyle(wxBG_STYLE_PAINT);
		int width = std::min(image.GetWidth(), FromDIP(720));
		int height = std::max(1, image.GetHeight() * width / std::max(1, image.GetWidth()));
		frame = wxBitmap(image.Scale(width, height, wxIMAGE_QUALITY_HIGH));
		SetInitialSize(wxSize(width, height));
		SetCursor(wxCursor(wxCURSOR_CROSS));
		Bind(wxEVT_PAINT, &AreaPicker::OnPaint, this);
		Bind(wxEVT_LEFT_DOWN, &AreaPicker::OnMouse, this);
		Bind(wxEVT_LEFT_UP, &AreaPicker::OnMouse, this);
		Bind(wxEVT_MOTION, &AreaPicker::OnMouse, this);
		Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) { });
	}

	double Top() const { return top; }
	double Bottom() const { return bottom; }
	double Left() const { return left; }
	double Right() const { return right; }
};

/// Ask for the area and language
/// @return false if cancelled
bool ask_settings(agi::Context *c, hardsub_extractor::Settings& s) {
	auto provider = c->project->VideoProvider();
	int frame_n = c->videoController->GetFrameN();
	wxImage image = GetImage(*provider->GetFrame(frame_n, c->project->Timecodes().TimeAtFrame(frame_n), true));

	wxDialog d(c->parent, -1, _(title));
	auto sizer = new wxBoxSizer(wxVERTICAL);
	auto intro = new wxStaticText(&d, -1,
		_("Drag a box around where the subtitles appear. Move the video to a frame that shows one first, so you can see where they are. A box that only just fits them gives the best results."));
	intro->Wrap(d.FromDIP(720));
	sizer->Add(intro, wxSizerFlags().Border(wxALL, 8));

	auto picker = new AreaPicker(&d, image,
		OPT_GET("Tool/Hardsub Extract/Area Top")->GetDouble(),
		OPT_GET("Tool/Hardsub Extract/Area Bottom")->GetDouble(),
		OPT_GET("Tool/Hardsub Extract/Area Left")->GetDouble(),
		OPT_GET("Tool/Hardsub Extract/Area Right")->GetDouble());
	sizer->Add(picker, wxSizerFlags().Center().Border(wxLEFT | wxRIGHT, 8));

	auto row = new wxBoxSizer(wxHORIZONTAL);
	auto language = new wxChoice(&d, -1);
	std::string saved_language = OPT_GET("Tool/Hardsub Extract/Language")->GetString();
	for (auto const& lang : languages) {
		language->Append(_(lang.name));
		if (saved_language == lang.code)
			language->SetSelection(language->GetCount() - 1);
	}
	if (language->GetSelection() == wxNOT_FOUND)
		language->SetSelection(0);
	auto mode = new wxChoice(&d, -1);
	mode->Append(_("Fast"));
	mode->Append(_("Accurate (slower)"));
	mode->SetSelection(OPT_GET("Tool/Hardsub Extract/Mode")->GetString() == "accurate" ? 1 : 0);
	row->Add(new wxStaticText(&d, -1, _("Language:")), wxSizerFlags().CenterVertical().Border(wxRIGHT, 4));
	row->Add(language, wxSizerFlags().CenterVertical().Border(wxRIGHT, 16));
	row->Add(new wxStaticText(&d, -1, _("Mode:")), wxSizerFlags().CenterVertical().Border(wxRIGHT, 4));
	row->Add(mode, wxSizerFlags().CenterVertical());
	sizer->Add(row, wxSizerFlags().Border(wxALL, 8));

	auto gpu = new wxCheckBox(&d, -1, _("Use the GPU when there is one (DirectML)"));
	gpu->SetValue(OPT_GET("Tool/Hardsub Extract/Use GPU If Available")->GetBool());
	sizer->Add(gpu, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

	if (!hardsub_extractor::Installed()) {
		auto note = new wxStaticText(&d, -1,
			_("The first time, video-subtitle-extractor is downloaded from GitHub (about 450 MB) and installed with its OCR engine (about 2 GB more). This takes a while, but only once."));
		note->Wrap(d.FromDIP(720));
		note->SetForegroundColour(wxColour(200, 100, 0));
		sizer->Add(note, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));
	}

	auto buttons = new wxStdDialogButtonSizer;
	auto ok = new wxButton(&d, wxID_OK, _("&Extract"));
	ok->SetDefault();
	buttons->AddButton(ok);
	buttons->AddButton(new wxButton(&d, wxID_CANCEL));
	buttons->Realize();
	sizer->Add(buttons, wxSizerFlags().Expand().Border(wxALL, 8));

	d.SetSizerAndFit(sizer);
	d.CenterOnParent();
	if (d.ShowModal() != wxID_OK) return false;

	s.top = picker->Top();
	s.bottom = picker->Bottom();
	s.left = picker->Left();
	s.right = picker->Right();
	s.language = languages[language->GetSelection()].code;
	s.mode = mode->GetSelection() == 1 ? "accurate" : "fast";
	s.gpu = gpu->GetValue();

	OPT_SET("Tool/Hardsub Extract/Area Top")->SetDouble(s.top);
	OPT_SET("Tool/Hardsub Extract/Area Bottom")->SetDouble(s.bottom);
	OPT_SET("Tool/Hardsub Extract/Area Left")->SetDouble(s.left);
	OPT_SET("Tool/Hardsub Extract/Area Right")->SetDouble(s.right);
	OPT_SET("Tool/Hardsub Extract/Language")->SetString(s.language);
	OPT_SET("Tool/Hardsub Extract/Mode")->SetString(s.mode);
	OPT_SET("Tool/Hardsub Extract/Use GPU If Available")->SetBool(s.gpu);
	return true;
}

/// Put the extracted lines into the open subtitles
void import_lines(agi::Context *c, agi::fs::path const& srt) {
	AssFile extracted;
	try {
		hardsub_extractor::ReadLines(c, srt, extracted);
	}
	catch (hardsub_extractor::Error const& e) {
		wxMessageBox(to_wx(e.what()), _(title), wxOK | wxICON_ERROR, c->parent);
		return;
	}
	if (extracted.Events.empty()) {
		wxMessageBox(_("No subtitles were found in the box. Check that it covers where they appear and that the language is right."),
			_(title), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	bool replace = false;
	if (hardsub_extractor::HasLines(c)) {
		wxMessageDialog ask(c->parent,
			fmt_tl("%d lines were extracted. Replace the current subtitles with them, or add them after the current lines?", (int)extracted.Events.size()),
			_(title), wxYES_NO | wxCANCEL | wxICON_QUESTION);
		ask.SetYesNoCancelLabels(_("&Replace"), _("&Add"), _("Cancel"));
		int answer = ask.ShowModal();
		if (answer == wxID_CANCEL) return;
		replace = answer == wxID_YES;
	}

	size_t count = hardsub_extractor::ImportLines(c, extracted, replace);
	wxMessageBox(fmt_tl("%d lines were extracted. OCR makes mistakes, so read them through against the video.", (int)count),
		_(title), wxOK | wxICON_INFORMATION, c->parent);
}
}

void ShowHardsubExtractDialog(agi::Context *c) {
	if (c->project->VideoName().empty() || !c->project->VideoProvider()) {
		wxMessageBox(_("Open the video first (Video > Open Video)."), _(title), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	hardsub_extractor::Settings settings;
	if (!ask_settings(c, settings)) return;
	settings.video = c->project->VideoName();
	settings.output = config::path->Decode("?local/vse/work/extracted.srt");

	std::string error;
	bool cancelled = false;
	DialogProgress progress(c->parent, _(title), _("Preparing..."));
	progress.Run([&](agi::ProgressSink *ps) {
		auto show = [ps](std::string const& message, double fraction) {
			ps->SetMessage(message);
			if (fraction < 0)
				ps->SetIndeterminate();
			else
				ps->SetProgress(static_cast<int64_t>(fraction * 1000), 1000);
		};
		try {
			hardsub_extractor::Extract(settings, show, [ps](std::string const& line) { ps->Log(line + "\n"); },
				[ps] { return ps->IsCancelled(); });
		}
		catch (hardsub_extractor::Cancelled const&) { cancelled = true; }
		catch (agi::Exception const& e) { error = e.GetMessage(); }
		catch (std::exception const& e) { error = e.what(); }
	});

	if (cancelled) return;
	if (!error.empty()) {
		wxMessageBox(fmt_tl("Extracting the subtitles failed:\n\n%s", error), _(title), wxOK | wxICON_ERROR, c->parent);
		return;
	}
	import_lines(c, settings.output);
}
