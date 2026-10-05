// Copyright (c) 2007, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file dialog_version_check.cpp
/// @brief Offer new releases published on the project's GitHub repository
///
/// Only the GitHub releases API is contacted, and nothing about the user's
/// system is sent. Nothing is downloaded or installed without asking.

#ifdef WITH_UPDATE_CHECKER

#include "json_util.h"
#include "compat.h"
#include "format.h"
#include "http_request.h"
#include "options.h"
#include "version.h"

#include <libaegisub/dispatch.h>

#include <array>
#include <boost/algorithm/string/predicate.hpp>
#include <ctime>
#include <mutex>
#include <regex>
#include <sstream>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace {
using json_util::find;
using json_util::find_string;
std::mutex VersionCheckLock;

struct Release {
	/// Tag without the leading "v", e.g. "1.3.0"
	std::string version;
	std::string name;
	std::string notes;
	/// Release page on GitHub
	std::string page_url;
	/// Installer or archive for this platform, or empty if there is none
	std::string download_url;
};

using SemVer = std::array<int, 3>;

/// Parse "v1.2.3" or "1.2.3-beta"; false if the text isn't a version
bool parse_version(std::string const& text, SemVer& out) {
	static const std::regex re(R"(^v?(\d+)\.(\d+)\.(\d+))");
	std::smatch m;
	if (!std::regex_search(text, m, re)) return false;
	for (int i = 0; i < 3; ++i)
		out[i] = std::stoi(m[i + 1].str());
	return true;
}

class UpdateDialog final : public wxDialog {
	Release release;
	wxCheckBox *automatic_check_checkbox;

	void OnUpdate(wxCommandEvent &);
	void OnSkip(wxCommandEvent &);
	void OnRemindMeLater(wxCommandEvent &);
	void OnClose(wxCloseEvent &);

public:
	/// @param release The newer release, or nullptr to only show a message
	UpdateDialog(wxString const& main_text, const Release *release);

	bool ShouldPreventAppExit() const override { return false; }
};

UpdateDialog::UpdateDialog(wxString const& main_text, const Release *new_release)
: wxDialog(nullptr, -1, _("aegidub Update"))
{
	const int controls_width = 500;
	if (new_release) release = *new_release;

	wxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

	wxStaticText *text = new wxStaticText(this, -1, main_text);
	text->Wrap(controls_width);
	main_sizer->Add(text, 0, wxBOTTOM | wxEXPAND, 6);

	if (new_release) {
		main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxALL, 6);

		text = new wxStaticText(this, -1, to_wx(release.name.empty() ? release.version : release.name));
		text->SetFont(text->GetFont().Bold());
		main_sizer->Add(text, 0, wxEXPAND | wxBOTTOM, 6);

		if (!release.notes.empty()) {
			auto notes = new wxTextCtrl(this, -1, to_wx(release.notes), wxDefaultPosition,
				wxSize(controls_width, 160), wxTE_MULTILINE | wxTE_READONLY);
			main_sizer->Add(notes, 1, wxEXPAND | wxBOTTOM, 6);
		}
		main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxALL, 6);
	}

	automatic_check_checkbox = new wxCheckBox(this, -1, _("&Check for new releases on startup"));
	automatic_check_checkbox->SetValue(OPT_GET("App/Auto/Check For Updates")->GetBool());
	main_sizer->Add(automatic_check_checkbox, 0, wxEXPAND | wxBOTTOM, 6);

	auto buttons = new wxBoxSizer(wxHORIZONTAL);
	if (new_release) {
		auto update = new wxButton(this, wxID_YES, release.download_url.empty() ? _("&Open release page") : _("&Download update"));
		update->SetDefault();
		buttons->Add(update, 0, wxRIGHT, 5);
		buttons->Add(new wxButton(this, wxID_NO, _("Remind me in a &week")), 0, wxRIGHT, 5);
		buttons->Add(new wxButton(this, wxID_IGNORE, _("&Skip this version")), 0, wxRIGHT, 5);
	}
	buttons->AddStretchSpacer();
	buttons->Add(new wxButton(this, wxID_CLOSE, new_release ? _("&Not now") : _("&Close")));
	main_sizer->Add(buttons, 0, wxEXPAND, 0);

	SetEscapeId(wxID_CLOSE);

	wxSizer *outer_sizer = new wxBoxSizer(wxVERTICAL);
	outer_sizer->Add(main_sizer, 1, wxALL | wxEXPAND, 12);
	SetSizerAndFit(outer_sizer);
	Centre();
	Show();

	Bind(wxEVT_BUTTON, &UpdateDialog::OnUpdate, this, wxID_YES);
	Bind(wxEVT_BUTTON, &UpdateDialog::OnRemindMeLater, this, wxID_NO);
	Bind(wxEVT_BUTTON, &UpdateDialog::OnSkip, this, wxID_IGNORE);
	Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { Close(); }, wxID_CLOSE);
	Bind(wxEVT_CLOSE_WINDOW, &UpdateDialog::OnClose, this);
}

void UpdateDialog::OnUpdate(wxCommandEvent &) {
	// The installer is downloaded by the browser and run by the user, so
	// nothing is replaced while aegidub is running
	wxLaunchDefaultBrowser(to_wx(release.download_url.empty() ? release.page_url : release.download_url));
	Close();
}

void UpdateDialog::OnSkip(wxCommandEvent &) {
	OPT_SET("Version/Skipped Release")->SetString(release.version);
	Close();
}

void UpdateDialog::OnRemindMeLater(wxCommandEvent &) {
	OPT_SET("Version/Next Check")->SetInt(time(nullptr) + 7 * 24 * 60 * 60);
	Close();
}

void UpdateDialog::OnClose(wxCloseEvent &) {
	OPT_SET("App/Auto/Check For Updates")->SetBool(automatic_check_checkbox->GetValue());
	Destroy();
}

void ShowMessage(wxString const& text) {
	agi::dispatch::Main().Async([=] { new UpdateDialog(text, nullptr); });
}

/// The release asset meant for this platform, if any
std::string platform_download(json::Object const& release) {
	auto assets = find(release, "assets");
	if (!assets) return {};
#if defined(_WIN32)
	const std::array<const char *, 2> extensions = {".exe", ".zip"};
#elif defined(__APPLE__)
	const std::array<const char *, 2> extensions = {".dmg", ".zip"};
#else
	const std::array<const char *, 2> extensions = {".AppImage", ".tar.gz"};
#endif
	for (auto ext : extensions) {
		for (auto const& asset : static_cast<json::Array const&>(*assets)) {
			json::Object const& obj = asset;
			if (boost::iends_with(find_string(obj, "name"), ext))
				return find_string(obj, "browser_download_url");
		}
	}
	return {};
}

/// @return The latest release, or false if the repository has none
bool FetchLatestRelease(Release& out) {
	auto response = http::Request(
		std::string("https://api.github.com/repos/") + UPDATE_CHECKER_REPO + "/releases/latest",
		{"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"},
		nullptr, 60L, nullptr);

	// GitHub answers 404 while a repository has no published release
	if (response.status == 404) return false;
	if (response.status != 200)
		throw http::Error("GitHub answered with HTTP " + std::to_string(response.status));

	std::istringstream ss(response.body);
	json::UnknownElement root;
	json::Reader::Read(root, ss);
	json::Object const& obj = root;

	std::string tag = find_string(obj, "tag_name");
	out.version = tag.size() > 1 && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
	out.name = find_string(obj, "name");
	out.notes = find_string(obj, "body");
	out.page_url = find_string(obj, "html_url");
	out.download_url = platform_download(obj);
	return true;
}

void DoCheck(bool interactive) {
	Release release;
	if (!FetchLatestRelease(release)) {
		if (interactive)
			ShowMessage(_("No aegidub release has been published yet."));
		return;
	}

	SemVer latest, current;
	if (!parse_version(release.version, latest)) {
		if (interactive)
			ShowMessage(fmt_tl("The latest release, \"%s\", has no version number that can be compared.", release.version));
		return;
	}
	if (!parse_version(GetReleaseVersion(), current))
		current = {0, 0, 0};

	if (latest <= current) {
		if (interactive)
			ShowMessage(fmt_tl("You have the latest version of aegidub (%s).", GetReleaseVersion()));
		return;
	}

	// An automatic check stays quiet about a version the user chose to skip
	if (!interactive && OPT_GET("Version/Skipped Release")->GetString() == release.version)
		return;

	wxString text = fmt_tl("aegidub %s is available. You have %s.\n\nDo you want to update?",
		release.version, GetReleaseVersion());
	agi::dispatch::Main().Async([=] { new UpdateDialog(text, &release); });
}
}

void PerformVersionCheck(bool interactive) {
	agi::dispatch::Background().Async([=]{
		if (!interactive) {
			// Automatic checking enabled?
			if (!OPT_GET("App/Auto/Check For Updates")->GetBool())
				return;

			// Is it actually time for a check?
			time_t next_check = OPT_GET("Version/Next Check")->GetInt();
			if (next_check > time(nullptr))
				return;
		}

		if (!VersionCheckLock.try_lock()) return;

		try {
			DoCheck(interactive);
		}
		catch (std::exception const& e) {
			if (interactive)
				ShowMessage(fmt_tl("Checking for a new aegidub release failed:\n%s", e.what()));
		}
		catch (...) {
			if (interactive)
				ShowMessage(_("An unknown error occurred while checking for a new aegidub release."));
		}

		VersionCheckLock.unlock();

		agi::dispatch::Main().Async([]{
			// Don't override a "remind me in a week" chosen meanwhile
			time_t next = time(nullptr) + 60 * 60;
			if (OPT_GET("Version/Next Check")->GetInt() < next)
				OPT_SET("Version/Next Check")->SetInt(next);
		});
	});
}

#endif
