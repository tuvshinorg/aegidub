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

/// @file dialog_mcp_server.cpp
/// @brief Turn the MCP server on and off and show how to connect agents to it

#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "mcp_server.h"
#include "options.h"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/settings.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace {
struct DialogMcpServer {
	wxDialog d;
	agi::Context *c;
	wxCheckBox *enabled;
	wxSpinCtrl *port;
	wxStaticText *status;
	wxTextCtrl *claude;
	wxTextCtrl *codex;

	DialogMcpServer(agi::Context *c);
	void Apply();
	void UpdateText();
	wxTextCtrl *AddCommand(wxSizer *sizer, wxString const& label, int lines);
};

DialogMcpServer::DialogMcpServer(agi::Context *c)
: d(c->parent, -1, _("AI Agents (MCP Server)"))
, c(c)
{
	auto sizer = new wxBoxSizer(wxVERTICAL);
	const int wrap = d.FromDIP(520);

	auto intro = new wxStaticText(&d, -1,
		_("Lets AI agents such as Claude Code and Codex work in aegidub while it is open: read and edit the subtitle lines, look at video frames, extract burned-in subtitles and export videos with soft subtitles. Only programs on this computer can connect."));
	intro->Wrap(wrap);
	sizer->Add(intro, wxSizerFlags().Border(wxALL, 8));

	auto row = new wxBoxSizer(wxHORIZONTAL);
	enabled = new wxCheckBox(&d, -1, _("&Enable the MCP server"));
	enabled->SetValue(OPT_GET("Tool/MCP Server/Enabled")->GetBool());
	port = new wxSpinCtrl(&d, -1, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1024, 65535,
		(int)OPT_GET("Tool/MCP Server/Port")->GetInt());
	row->Add(enabled, wxSizerFlags().CenterVertical().Border(wxRIGHT, 16));
	row->Add(new wxStaticText(&d, -1, _("Port:")), wxSizerFlags().CenterVertical().Border(wxRIGHT, 4));
	row->Add(port, wxSizerFlags().CenterVertical());
	sizer->Add(row, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

	status = new wxStaticText(&d, -1, "");
	sizer->Add(status, wxSizerFlags().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));

	claude = AddCommand(sizer, _("Claude Code: run this in a terminal"), 1);
	codex = AddCommand(sizer, _("Codex: add this to ~/.codex/config.toml"), 2);

	auto buttons = new wxStdDialogButtonSizer;
	buttons->AddButton(new wxButton(&d, wxID_OK, _("Close")));
	buttons->Realize();
	sizer->Add(buttons, wxSizerFlags().Expand().Border(wxALL, 8));

	d.SetSizerAndFit(sizer);
	d.CenterOnParent();
	UpdateText();

	enabled->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { Apply(); });
	port->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { Apply(); });
}

wxTextCtrl *DialogMcpServer::AddCommand(wxSizer *sizer, wxString const& label, int lines) {
	auto box = new wxStaticBoxSizer(wxHORIZONTAL, &d, label);
	auto text = new wxTextCtrl(box->GetStaticBox(), -1, "", wxDefaultPosition,
		wxSize(d.FromDIP(440), -1), wxTE_READONLY | (lines > 1 ? wxTE_MULTILINE | wxTE_DONTWRAP : 0));
	if (lines > 1)
		text->SetMinSize(wxSize(d.FromDIP(440), text->GetCharHeight() * (lines + 1) + d.FromDIP(6)));
	auto copy = new wxButton(box->GetStaticBox(), -1, _("&Copy"));
	box->Add(text, wxSizerFlags(1).CenterVertical().Border(wxALL, 4));
	box->Add(copy, wxSizerFlags().CenterVertical().Border(wxALL, 4));
	sizer->Add(box, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));
	copy->Bind(wxEVT_BUTTON, [text](wxCommandEvent&) {
		if (wxTheClipboard->Open()) {
			wxTheClipboard->SetData(new wxTextDataObject(text->GetValue()));
			wxTheClipboard->Close();
		}
	});
	return text;
}

void DialogMcpServer::Apply() {
	OPT_SET("Tool/MCP Server/Enabled")->SetBool(enabled->GetValue());
	OPT_SET("Tool/MCP Server/Port")->SetInt(port->GetValue());
	mcp_server::Update(c);
	UpdateText();
}

void DialogMcpServer::UpdateText() {
	std::string url = mcp_server::Url();
	claude->SetValue(to_wx("claude mcp add --transport http aegidub " + url));
	codex->SetValue(to_wx("[mcp_servers.aegidub]\nurl = \"" + url + "\""));

	std::string error = mcp_server::LastError();
	if (!error.empty()) {
		status->SetLabel(to_wx(error));
		status->SetForegroundColour(wxColour(200, 0, 0));
	}
	else if (mcp_server::Running()) {
		status->SetLabel(fmt_tl("Running at %s", url));
		status->SetForegroundColour(wxColour(0, 140, 0));
	}
	else {
		status->SetLabel(_("Off. Agents can't connect until it is enabled."));
		status->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
	}
	status->Wrap(d.FromDIP(520));
	d.Layout();
}
}

void ShowMcpServerDialog(agi::Context *c) {
	DialogMcpServer dialog(c);
	dialog.d.ShowModal();
}
