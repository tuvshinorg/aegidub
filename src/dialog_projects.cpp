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

/// @file dialog_projects.cpp
/// @brief The project home screen: every dubbing project as a card

#include "ass_file.h"
#include "command/command.h"
#include "compat.h"
#include "dub_settings.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "project.h"
#include "project_store.h"
#include "series_cast.h"
#include "subs_controller.h"

#include <libaegisub/fs.h>

#include <ctime>

#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <wx/wrapsizer.h>

namespace {
const int thumb_width = 256;
const int thumb_height = 144;

agi::fs::path documents_projects() {
	return agi::fs::path(from_wx(wxStandardPaths::Get().GetDocumentsDir())) / "aegidub Projects";
}

agi::fs::path projects_root() {
	std::string folder = OPT_GET("Tool/Projects/Folder")->GetString();
	if (!folder.empty()) return agi::fs::path(folder);
	return documents_projects();
}

/// On the first run, offer to keep projects on the desktop. Installs that
/// already have projects in Documents keep them there without asking.
void ask_projects_folder(wxWindow *parent) {
	if (!OPT_GET("Tool/Projects/Folder")->GetString().empty()) return;
	if (agi::fs::DirectoryExists(documents_projects())) return;

	auto desktop = agi::fs::path(from_wx(wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Desktop))) / "aegidub Projects";
	wxMessageDialog ask(parent,
		fmt_tl("Keep your projects on the desktop?\n\nThey will be saved in:\n%s\n\nYou can change this later with Projects folder.", desktop.string()),
		_("Projects folder"), wxOK | wxCANCEL | wxICON_QUESTION);
	ask.SetOKCancelLabels(_("Use the desktop"), _("Use Documents"));
	auto chosen = ask.ShowModal() == wxID_OK ? desktop : documents_projects();
	OPT_SET("Tool/Projects/Folder")->SetString(chosen.string());
}

std::string format_time(int64_t t) {
	std::time_t tt = static_cast<std::time_t>(t);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &tt);
#else
	localtime_r(&tt, &tm);
#endif
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
	return buf;
}

/// A thumbnail letterboxed into the card's picture area
wxBitmap card_picture(project_store::Project const& project) {
	wxImage canvas(thumb_width, thumb_height);
	canvas.SetRGB(wxRect(0, 0, thumb_width, thumb_height), 24, 24, 28);

	wxImage thumb;
	auto path = project.Thumbnail();
	if (agi::fs::FileExists(path) && thumb.LoadFile(to_wx(path.string()), wxBITMAP_TYPE_PNG)) {
		double scale = std::min(double(thumb_width) / thumb.GetWidth(), double(thumb_height) / thumb.GetHeight());
		int w = std::max(1, int(thumb.GetWidth() * scale)), h = std::max(1, int(thumb.GetHeight() * scale));
		thumb.Rescale(w, h, wxIMAGE_QUALITY_HIGH);
		canvas.Paste(thumb, (thumb_width - w) / 2, (thumb_height - h) / 2);
	}
	return wxBitmap(canvas);
}

class DialogProjects {
	wxDialog d;
	agi::Context *c;
	wxScrolledWindow *scroll;
	wxWrapSizer *cards;
	std::vector<project_store::Project> projects;

	void Reload();
	wxWindow *MakeCard(size_t index);
	bool CloseCurrent();
	void Open(project_store::Project const& project);
	void OnNew(wxCommandEvent&);
	void OnOpenFile(wxCommandEvent&);
	void OnChooseFolder(wxCommandEvent&);
	void OnCardMenu(size_t index);

public:
	DialogProjects(agi::Context *c);
	void Show() { d.ShowModal(); }
};

DialogProjects::DialogProjects(agi::Context *c)
: d(c->parent, -1, _("aegidub Projects"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, c(c)
{

	auto header = new wxBoxSizer(wxHORIZONTAL);
	auto logo = new wxStaticBitmap(&d, -1, GETIMAGE(wxicon));
	auto title = new wxStaticText(&d, -1, _("Projects"));
	title->SetFont(title->GetFont().Scaled(1.6f).Bold());
	auto new_project = new wxButton(&d, -1, _("&New project..."));
	new_project->SetToolTip(_("Start a project from a video, optionally importing subtitles you already have"));
	auto open_file = new wxButton(&d, -1, _("&Open file..."));
	open_file->SetToolTip(_("Open subtitles outside of any project"));
	auto folder = new wxButton(&d, -1, _("Projects &folder..."));
	folder->SetToolTip(_("Choose where projects are kept"));
	header->Add(logo, wxSizerFlags().CenterVertical().Border(wxRIGHT, 8));
	header->Add(title, wxSizerFlags().CenterVertical());
	header->AddStretchSpacer();
	header->Add(new_project, wxSizerFlags().CenterVertical().Border(wxRIGHT, 5));
	header->Add(open_file, wxSizerFlags().CenterVertical().Border(wxRIGHT, 5));
	header->Add(folder, wxSizerFlags().CenterVertical());

	scroll = new wxScrolledWindow(&d, -1, wxDefaultPosition, wxSize(900, 560), wxVSCROLL);
	scroll->SetScrollRate(0, 20);
	scroll->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
	cards = new wxWrapSizer(wxHORIZONTAL);
	scroll->SetSizer(cards);

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(header, wxSizerFlags().Expand().Border(wxALL, 10));
	sizer->Add(scroll, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 10));
	d.SetSizerAndFit(sizer);
	d.CenterOnParent();

	new_project->Bind(wxEVT_BUTTON, &DialogProjects::OnNew, this);
	open_file->Bind(wxEVT_BUTTON, &DialogProjects::OnOpenFile, this);
	folder->Bind(wxEVT_BUTTON, &DialogProjects::OnChooseFolder, this);

	ask_projects_folder(c->parent);
	Reload();
}

void DialogProjects::Reload() {
	scroll->Freeze();
	cards->Clear(true);
	projects = project_store::List(projects_root());

	if (projects.empty()) {
		auto empty = new wxStaticText(scroll, -1,
			fmt_tl("No projects yet.\n\nClick New project and choose a video to start. Projects are kept in:\n%s",
				projects_root().string()));
		empty->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
		cards->Add(empty, wxSizerFlags().Border(wxALL, 20));
	}
	for (size_t i = 0; i < projects.size(); ++i)
		cards->Add(MakeCard(i), wxSizerFlags().Border(wxALL, 8));

	scroll->FitInside();
	scroll->Layout();
	scroll->Thaw();
}

wxWindow *DialogProjects::MakeCard(size_t index) {
	auto const& project = projects[index];
	auto card = new wxPanel(scroll, -1, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
	card->SetCursor(wxCursor(wxCURSOR_HAND));

	auto picture = new wxStaticBitmap(card, -1, card_picture(project));
	auto id = new wxStaticText(card, -1, to_wx(project.id));
	id->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE).Bold()));
	auto name = new wxStaticText(card, -1, to_wx(project.name), wxDefaultPosition, wxSize(thumb_width, -1),
		wxST_ELLIPSIZE_MIDDLE | wxST_NO_AUTORESIZE);
	name->SetToolTip(to_wx(project.video.string()));
	auto edited = new wxStaticText(card, -1, fmt_tl("Edited %s", format_time(project.modified)));
	edited->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(picture);
	sizer->Add(id, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxTOP, 6));
	sizer->Add(name, wxSizerFlags().Border(wxLEFT | wxRIGHT, 6));
	sizer->Add(edited, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));
	if (!agi::fs::FileExists(project.video)) {
		auto missing = new wxStaticText(card, -1, _("Video not found"));
		missing->SetForegroundColour(wxColour(200, 60, 40));
		sizer->Add(missing, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM, 6));
	}
	card->SetSizerAndFit(sizer);

	// Clicking anywhere on the card opens it
	for (wxWindow *w : std::initializer_list<wxWindow *>{card, picture, id, name, edited}) {
		w->Bind(wxEVT_LEFT_UP, [this, index](wxMouseEvent&) { Open(projects[index]); });
		w->Bind(wxEVT_RIGHT_UP, [this, index](wxMouseEvent&) { OnCardMenu(index); });
	}
	return card;
}

bool DialogProjects::CloseCurrent() {
	return c->subsController->TryToClose() != wxCANCEL;
}

void DialogProjects::Open(project_store::Project const& project) {
	if (!agi::fs::FileExists(project.Subtitles())) {
		wxMessageBox(fmt_tl("The project's subtitles are missing:\n%s", project.Subtitles().string()), _("aegidub Projects"), wxOK | wxICON_ERROR, &d);
		return;
	}
	if (!CloseCurrent()) return;

	d.EndModal(wxID_OK);
	c->project->LoadSubtitles(project.Subtitles(), "", false);
	if (agi::fs::FileExists(project.video)) {
		c->project->LoadVideo(project.video);
		c->project->LoadAudio(project.video);
	}
	else
		wxMessageBox(fmt_tl("The video of this project was moved or deleted:\n%s\n\nOpen it again with Video > Open Video.", project.video.string()),
			_("aegidub Projects"), wxOK | wxICON_WARNING, c->parent);
}

void DialogProjects::OnNew(wxCommandEvent&) {
	wxFileDialog video_dialog(&d, _("Choose the video to dub"), "", "",
		_("Video files|*.mkv;*.mp4;*.avi;*.mov;*.webm;*.m4v;*.ts;*.wmv|All files|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (video_dialog.ShowModal() != wxID_OK) return;
	agi::fs::path video(from_wx(video_dialog.GetPath()));

	project_store::Project project;
	project.id = project_store::MakeId(video, std::time(nullptr));
	project.name = video.filename().string();
	project.video = video;
	project.folder = agi::fs::path(projects_root() / project.id);

	// The same video started on the same day is the same project
	for (auto const& existing : projects) {
		if (existing.id == project.id) {
			Open(existing);
			return;
		}
	}

	agi::fs::path import;
	int answer = wxMessageBox(_("Import subtitles you already have for this video?\n\nChoose No to start with empty subtitles."),
		_("New project"), wxYES_NO | wxCANCEL | wxICON_QUESTION, &d);
	if (answer == wxCANCEL) return;
	if (answer == wxYES) {
		wxFileDialog subs_dialog(&d, _("Choose the subtitles to import"), to_wx(video.parent_path().string()), "",
			_("Subtitles|*.ass;*.ssa;*.srt;*.sub;*.ttxt;*.txt;*.vtt|All files|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (subs_dialog.ShowModal() != wxID_OK) return;
		import = agi::fs::path(from_wx(subs_dialog.GetPath()));
	}

	if (!CloseCurrent()) return;

	try {
		agi::fs::CreateDirectory(project.folder);
		project_store::Write(project);

		if (!import.empty()) {
			c->project->LoadSubtitles(import, "", false);
			// Speech already generated for these subtitles comes along, so
			// none of it has to be paid for again
			auto old_dub = dub::Folder(import);
			if (agi::fs::DirectoryExists(old_dub)) {
				std::error_code ec;
				std::filesystem::copy(old_dub, dub::Folder(project.Subtitles()),
					std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_existing, ec);
			}
			// Keep the series cast link working from the new location
			if (series_cast::IsLinked(c->ass.get()))
				series_cast::Link(c->ass.get(), project.Subtitles(), series_cast::Find(c->ass.get(), import));
		}
		else
			c->project->CloseSubtitles();

		c->subsController->Save(project.Subtitles());
	}
	catch (agi::Exception const& e) {
		wxMessageBox(fmt_tl("The project could not be created:\n\n%s", e.GetMessage()), _("New project"), wxOK | wxICON_ERROR, &d);
		return;
	}

	{
		wxBusyCursor wait;
		project_store::MakeThumbnail(video, project.Thumbnail(), thumb_width * 2);
	}

	d.EndModal(wxID_OK);
	c->project->LoadVideo(video);
	c->project->LoadAudio(video);
	c->subsController->Save(project.Subtitles());
}

void DialogProjects::OnOpenFile(wxCommandEvent&) {
	d.EndModal(wxID_OK);
	cmd::call("subtitle/open", c);
}

void DialogProjects::OnChooseFolder(wxCommandEvent&) {
	wxDirDialog dialog(&d, _("Where to keep projects"), to_wx(projects_root().string()));
	if (dialog.ShowModal() != wxID_OK) return;
	OPT_SET("Tool/Projects/Folder")->SetString(from_wx(dialog.GetPath()));
	Reload();
}

void DialogProjects::OnCardMenu(size_t index) {
	enum { ID_OPEN = 1, ID_SHOW, ID_DELETE };
	wxMenu menu;
	menu.Append(ID_OPEN, _("&Open"));
	menu.Append(ID_SHOW, _("&Show in folder"));
	menu.AppendSeparator();
	menu.Append(ID_DELETE, _("&Delete project..."));

	int choice = d.GetPopupMenuSelectionFromUser(menu);
	auto const project = projects[index];
	if (choice == ID_OPEN)
		Open(project);
	else if (choice == ID_SHOW)
		wxLaunchDefaultApplication(to_wx(project.folder.string()));
	else if (choice == ID_DELETE) {
		if (c->subsController->Filename() == project.Subtitles()) {
			wxMessageBox(_("This project is open. Open another file first."), _("Delete project"), wxOK | wxICON_INFORMATION, &d);
			return;
		}
		int answer = wxMessageBox(
			fmt_tl("Delete the project %s?\n\nIts subtitles, generated speech and separated audio are deleted. The video itself is not touched:\n%s",
				project.id, project.video.string()),
			_("Delete project"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, &d);
		if (answer != wxYES) return;
		std::error_code ec;
		std::filesystem::remove_all(project.folder, ec);
		if (ec)
			wxMessageBox(fmt_tl("Some files could not be deleted:\n%s", ec.message()), _("Delete project"), wxOK | wxICON_ERROR, &d);
		Reload();
	}
}
}

void ShowProjectsDialog(agi::Context *c) {
	DialogProjects(c).Show();
}
