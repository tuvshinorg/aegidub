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

/// @file dialog_voice_cast.cpp
/// @brief Assign a dubbing voice to every character in the script

#include "json_util.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "command/command.h"
#include "compat.h"
#include "dialog_progress.h"
#include "dub_settings.h"
#include "elevenlabs_client.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "openai_client.h"
#include "series_cast.h"
#include "subs_controller.h"
#include "voice_cast.h"


#include <algorithm>
#include <boost/algorithm/string/trim.hpp>
#include <map>
#include <set>
#include <sstream>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/sound.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

namespace {
using json_util::parse_json;
using json_util::to_json;
/// Most lines of a character sent to the model when auto-detecting
const size_t max_sample_lines = 8;
/// Longest line text sent to the model or spoken in a preview
const size_t max_line_length = 220;
/// Characters cast per request; small batches get quick replies, so slow
/// models don't hold an idle connection open for minutes
const size_t casting_batch_size = 12;
/// Dialog return code asking for speaker detection before reopening
const int detect_speakers_code = wxID_HIGHEST + 1;

struct Character {
	std::string name;
	/// Spoken text of each of the character's lines, in file order
	std::vector<std::string> lines;
};

/// A voice the model chose for a character
struct Assignment {
	std::string character;
	std::string voice_id;
	std::string reason;
	size_t line_count = 0;
};

struct Row {
	Character character;
	wxChoice *choice;
	wxButton *play;
	wxStaticText *reason;
};

/// The text of a line as it would be spoken
std::string spoken_text(AssDialogue const& line) {
	return dub::SpokenText(line.Text.get(), max_line_length);
}

/// Characters of the file, the ones with the most lines first
std::vector<Character> collect_characters(AssFile const& file, size_t& unassigned) {
	std::map<std::string, Character> by_name;
	unassigned = 0;
	for (auto const& line : file.Events) {
		if (line.Comment) continue;
		std::string text = spoken_text(line);
		if (text.empty()) continue;
		std::string actor = line.Actor.get();
		boost::trim(actor);
		if (actor.empty()) {
			++unassigned;
			continue;
		}
		auto& character = by_name[actor];
		character.name = actor;
		character.lines.push_back(std::move(text));
	}

	std::vector<Character> characters;
	for (auto& entry : by_name)
		characters.push_back(std::move(entry.second));
	std::stable_sort(characters.begin(), characters.end(), [](Character const& a, Character const& b) {
		return a.lines.size() > b.lines.size();
	});
	return characters;
}

/// A line that gives a good impression of the voice: the first reasonably
/// long one, or else the longest
std::string preview_line(Character const& character) {
	std::string best;
	for (auto const& line : character.lines) {
		if (line.size() >= 40) return line;
		if (line.size() > best.size()) best = line;
	}
	return best;
}

bool valid_voice_id(std::string const& id) {
	return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char ch) {
		return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
	});
}

std::string casting_prompt() {
	return
		"You are the casting director of a Mongolian voice dub of an anime or film.\n"
		"Assign every character a voice from the voice catalogue.\n"
		"Work out each character's gender, approximate age and personality from their name "
		"(many are well-known characters: use what you know about them) and from their lines.\n"
		"Rules:\n"
		"- The voice's gender must match the character. A young boy may get a young, "
		"energetic voice of either gender if no young male voice fits.\n"
		"- Match age and personality: calm elders get mature, calm voices; energetic "
		"children and teens get young, upbeat voices; villains get firm or rough voices.\n"
		"- The characters with the most lines must all get different voices. Reuse a voice "
		"only for minor characters, and never for two characters who speak in the same scenes.\n"
		"- `already_cast` lists characters cast earlier in this or previous episodes, with their "
		"voices. Do not give one of those voices to another character unless no other voice "
		"fits, and then only to a minor character.\n"
		"- Only use voice_id values from the catalogue.\n"
		"Reply with a single JSON object of the form "
		"{\"cast\":[{\"character\":\"<name>\",\"voice_id\":\"<id>\",\"reason\":\"<a few words in English>\"}]} "
		"containing every character exactly once.";
}

std::string casting_request(std::vector<elevenlabs::Voice> const& voices, std::vector<Assignment> const& already_cast,
	std::vector<Character> const& characters, size_t start, size_t end)
{
	json::Array voice_arr;
	for (auto const& voice : voices) {
		json::Object obj;
		obj.emplace("voice_id", json::UnknownElement(voice.id));
		obj.emplace("name", json::UnknownElement(voice.name));
		obj.emplace("gender", json::UnknownElement(voice.gender.empty() ? std::string("unknown") : voice.gender));
		obj.emplace("description", json::UnknownElement(voice.description));
		voice_arr.emplace_back(std::move(obj));
	}

	json::Array cast_arr;
	for (auto const& assignment : already_cast) {
		json::Object obj;
		obj.emplace("character", json::UnknownElement(assignment.character));
		obj.emplace("voice_id", json::UnknownElement(assignment.voice_id));
		obj.emplace("line_count", json::UnknownElement(static_cast<int64_t>(assignment.line_count)));
		cast_arr.emplace_back(std::move(obj));
	}

	json::Array char_arr;
	for (size_t index = start; index < end; ++index) {
		auto const& character = characters[index];
		json::Object obj;
		obj.emplace("character", json::UnknownElement(character.name));
		obj.emplace("line_count", json::UnknownElement(static_cast<int64_t>(character.lines.size())));
		json::Array samples;
		for (size_t i = 0; i < character.lines.size() && i < max_sample_lines; ++i)
			samples.emplace_back(json::UnknownElement(character.lines[i]));
		obj.emplace("sample_lines", json::UnknownElement(std::move(samples)));
		char_arr.emplace_back(std::move(obj));
	}

	json::Object root;
	root.emplace("voice_catalogue", json::UnknownElement(std::move(voice_arr)));
	if (!cast_arr.empty())
		root.emplace("already_cast", json::UnknownElement(std::move(cast_arr)));
	root.emplace("characters", json::UnknownElement(std::move(char_arr)));
	return to_json(json::UnknownElement(std::move(root)));
}

struct DialogVoiceCast {
	wxDialog d;
	agi::Context *c;

	/// Voices offered in every row; the choice index is offset by one for "No voice"
	std::vector<elevenlabs::Voice> voices;
	std::vector<Row> rows;
	bool account_loaded = false;

	/// File holding the voices of every episode of the series, if any
	agi::fs::path series_path;
	series_cast::Cast series;
	wxStaticText *series_label = nullptr;

	/// Generated previews by voice ID and character, so replaying is free
	std::map<std::pair<std::string, std::string>, std::vector<char>> previews;
	wxSound sound;

	DialogVoiceCast(agi::Context *c);

	std::string SelectedVoice(Row const& row) const;
	void SelectVoice(Row& row, std::string const& id);
	void RefreshChoices();
	int VoiceIndex(std::string const& id) const;
	wxString VoiceLabel(elevenlabs::Voice const& voice) const;
	void AddCustomVoice(std::string const& id);

	bool CheckTtsKey(elevenlabs::Config const& config);
	bool LoadAccountVoices(bool quiet);

	bool LoadSeries(agi::fs::path const& path);
	void ApplySeries(bool override_episode);
	void UpdateSeriesLabel();
	void OnChooseSeries(wxCommandEvent&);
	void OnNewSeries(wxCommandEvent&);

	void OnChoice(Row& row);
	void OnPreview(Row& row);
	void OnAutoDetect(wxCommandEvent&);
	void OnLoadAccount(wxCommandEvent&);
	void SaveCast();
	void OnDetectSpeakers(wxCommandEvent&);
	void OnOK(wxCommandEvent&);
};

DialogVoiceCast::DialogVoiceCast(agi::Context *c)
: d(c->parent, -1, _("Voice Cast"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, c(c)
, voices(elevenlabs::LibraryVoices())
{
	size_t unassigned = 0;
	auto characters = collect_characters(*c->ass, unassigned);
	auto cast = voice_cast::Load(c->ass.get());

	// Keep voices assigned earlier selectable even if they aren't in any list
	for (auto const& entry : cast) {
		if (VoiceIndex(entry.second) < 0 && valid_voice_id(entry.second))
			AddCustomVoice(entry.second);
	}

	auto subs_path = c->subsController->Filename();
	auto found = series_cast::Find(c->ass.get(), subs_path);
	if (!found.empty())
		LoadSeries(found);

	wxString intro = characters.empty()
		? _("No line has a character yet. Click Detect speakers to let the AI guess who says each line, or set the Character column by hand.")
		: fmt_tl("%d characters found. Pick a voice for each one, or let Auto-detect cast them from their names and lines. Play speaks one of the character's own lines.", (int)characters.size());
	if (unassigned)
		intro += "\n" + fmt_tl("%d lines have no character and are not listed.", (int)unassigned);
	auto intro_text = new wxStaticText(&d, -1, intro);
	intro_text->Wrap(760);

	auto scroll = new wxScrolledWindow(&d, -1, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_THEME);
	scroll->SetScrollRate(0, 10);
	auto grid = new wxFlexGridSizer(5, 4, 10);
	grid->AddGrowableCol(4, 1);

	for (auto const& header : {_("Character"), _("Lines"), _("Voice"), wxString(), _("Why")}) {
		auto label = new wxStaticText(scroll, -1, header);
		label->SetFont(label->GetFont().Bold());
		grid->Add(label, wxSizerFlags().CenterVertical());
	}

	rows.reserve(characters.size());
	for (auto& character : characters) {
		auto name = new wxStaticText(scroll, -1, to_wx(character.name));
		auto count = new wxStaticText(scroll, -1, fmt_wx("%d", (int)character.lines.size()));
		auto choice = new wxChoice(scroll, -1);
		choice->SetMinSize(wxSize(330, -1));
		auto play = new wxButton(scroll, -1, _("Play"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
		play->SetToolTip(_("Speak one of this character's lines with the selected voice"));
		auto reason = new wxStaticText(scroll, -1, "");
		reason->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));

		grid->Add(name, wxSizerFlags().CenterVertical());
		grid->Add(count, wxSizerFlags().CenterVertical().Right());
		grid->Add(choice, wxSizerFlags().CenterVertical());
		grid->Add(play, wxSizerFlags().CenterVertical());
		grid->Add(reason, wxSizerFlags().CenterVertical());

		rows.push_back(Row{std::move(character), choice, play, reason});
	}

	RefreshChoices();
	for (auto& row : rows) {
		auto it = cast.find(row.character.name);
		SelectVoice(row, it == cast.end() ? std::string() : it->second);
	}
	// Characters this episode hasn't cast yet take their voice from the series
	ApplySeries(false);

	// Bind after the rows vector is complete so the captured pointers stay valid
	for (size_t i = 0; i < rows.size(); ++i) {
		rows[i].choice->Bind(wxEVT_CHOICE, [this, i](wxCommandEvent&) { OnChoice(rows[i]); });
		rows[i].play->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { OnPreview(rows[i]); });
	}

	auto scroll_sizer = new wxBoxSizer(wxVERTICAL);
	scroll_sizer->Add(grid, wxSizerFlags(1).Expand().Border(wxALL, 8));
	scroll->SetSizer(scroll_sizer);
	scroll->FitInside();
	int height = std::min(scroll_sizer->GetMinSize().GetHeight(), 420);
	scroll->SetMinSize(wxSize(860, std::max(height, 80)));

	auto auto_detect = new wxButton(&d, -1, _("&Auto-detect voices"));
	auto_detect->SetToolTip(_("Ask the AI model set in Preferences > AI Translation to pick a voice for every character"));
	auto load_account = new wxButton(&d, -1, _("Load &my ElevenLabs voices"));
	load_account->SetToolTip(_("Add the voices in your ElevenLabs account to the lists"));
	auto_detect->Enable(!rows.empty());
	auto detect_speakers = new wxButton(&d, -1, _("&Detect speakers..."));
	detect_speakers->SetToolTip(_("Ask the AI model who speaks each line that has no character yet"));

	auto tools = new wxBoxSizer(wxHORIZONTAL);
	tools->Add(detect_speakers, wxSizerFlags().Border(wxRIGHT, 5));
	tools->Add(auto_detect, wxSizerFlags().Border(wxRIGHT, 5));
	tools->Add(load_account);

	series_label = new wxStaticText(&d, -1, "");
	auto choose_series = new wxButton(&d, -1, _("&Open..."));
	choose_series->SetToolTip(_("Use the cast file of a series, so its characters keep their voices in every episode"));
	auto new_series = new wxButton(&d, -1, _("&New..."));
	new_series->SetToolTip(_("Start a cast file for a new series"));
	auto series_row = new wxBoxSizer(wxHORIZONTAL);
	auto series_title = new wxStaticText(&d, -1, _("Series cast:"));
	series_title->SetFont(series_title->GetFont().Bold());
	series_row->Add(series_title, wxSizerFlags().CenterVertical().Border(wxRIGHT, 6));
	series_row->Add(series_label, wxSizerFlags(1).CenterVertical().Border(wxRIGHT, 6));
	series_row->Add(choose_series, wxSizerFlags().CenterVertical().Border(wxRIGHT, 5));
	series_row->Add(new_series, wxSizerFlags().CenterVertical());
	UpdateSeriesLabel();

	auto buttons = d.CreateStdDialogButtonSizer(wxOK | wxCANCEL);

	auto bottom = new wxBoxSizer(wxHORIZONTAL);
	bottom->Add(tools, wxSizerFlags().CenterVertical());
	bottom->AddStretchSpacer();
	bottom->Add(buttons, wxSizerFlags().CenterVertical());

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(intro_text, wxSizerFlags().Expand().Border(wxALL, 8));
	sizer->Add(series_row, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 8));
	sizer->Add(scroll, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT, 8));
	sizer->Add(bottom, wxSizerFlags().Expand().Border(wxALL, 8));
	d.SetSizerAndFit(sizer);
	d.CenterOnParent();

	auto_detect->Bind(wxEVT_BUTTON, &DialogVoiceCast::OnAutoDetect, this);
	load_account->Bind(wxEVT_BUTTON, &DialogVoiceCast::OnLoadAccount, this);
	detect_speakers->Bind(wxEVT_BUTTON, &DialogVoiceCast::OnDetectSpeakers, this);
	choose_series->Bind(wxEVT_BUTTON, &DialogVoiceCast::OnChooseSeries, this);
	new_series->Bind(wxEVT_BUTTON, &DialogVoiceCast::OnNewSeries, this);
	d.Bind(wxEVT_BUTTON, &DialogVoiceCast::OnOK, this, wxID_OK);
}

bool DialogVoiceCast::LoadSeries(agi::fs::path const& path) {
	try {
		series = series_cast::Load(path);
	}
	catch (agi::Exception const& e) {
		wxMessageBox(fmt_tl("The series cast file could not be read:\n\n%s", e.GetMessage()), _("Voice Cast"), wxOK | wxICON_ERROR, &d);
		return false;
	}
	series_path = path;

	bool added = false;
	for (auto const& entry : series) {
		if (VoiceIndex(entry.second.voice_id) < 0 && valid_voice_id(entry.second.voice_id)) {
			AddCustomVoice(entry.second.voice_id);
			if (!entry.second.voice_name.empty())
				voices.back().name = entry.second.voice_name;
			added = true;
		}
	}
	if (added && !rows.empty())
		RefreshChoices();
	return true;
}

void DialogVoiceCast::ApplySeries(bool override_episode) {
	for (auto& row : rows) {
		auto it = series.find(row.character.name);
		if (it == series.end()) continue;
		if (!override_episode && !SelectedVoice(row).empty()) continue;
		SelectVoice(row, it->second.voice_id);
		row.reason->SetLabel(_("From the series cast"));
	}
}

void DialogVoiceCast::UpdateSeriesLabel() {
	if (!series_label) return;
	if (series_path.empty())
		series_label->SetLabel(_("none - voices are kept in this episode only"));
	else
		series_label->SetLabel(fmt_tl("%s (%d characters)", series_path.filename().string(), (int)series.size()));
	series_label->SetToolTip(to_wx(series_path.string()));
	d.Layout();
}

void DialogVoiceCast::OnChooseSeries(wxCommandEvent&) {
	auto start = series_path.empty() ? c->subsController->Filename().parent_path() : series_path.parent_path();
	wxFileDialog dialog(&d, _("Open series cast"), to_wx(start.string()), "",
		_("Series cast files (*.cast.json)|*.cast.json|JSON files (*.json)|*.json"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
	if (dialog.ShowModal() != wxID_OK) return;
	if (!LoadSeries(agi::fs::path(from_wx(dialog.GetPath())))) return;
	// Choosing a series on purpose means using its voices
	ApplySeries(true);
	UpdateSeriesLabel();
}

void DialogVoiceCast::OnNewSeries(wxCommandEvent&) {
	auto suggested = series_cast::SuggestPath(c->subsController->Filename());
	wxFileDialog dialog(&d, _("New series cast"), to_wx(suggested.parent_path().string()), to_wx(suggested.filename().string()),
		_("Series cast files (*.cast.json)|*.cast.json"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
	if (dialog.ShowModal() != wxID_OK) return;
	series_path = agi::fs::path(from_wx(dialog.GetPath()));
	series.clear();
	// The file is written with this episode's voices when OK is clicked
	UpdateSeriesLabel();
}

int DialogVoiceCast::VoiceIndex(std::string const& id) const {
	for (size_t i = 0; i < voices.size(); ++i) {
		if (voices[i].id == id) return static_cast<int>(i);
	}
	return -1;
}

wxString DialogVoiceCast::VoiceLabel(elevenlabs::Voice const& voice) const {
	wxString label = to_wx(voice.name);
	if (!voice.gender.empty())
		label += " (" + to_wx(voice.gender) + ")";
	if (voice.account)
		label += voice.mongolian ? _(" - my voice, Mongolian") : _(" - my voice");
	else if (!voice.description.empty())
		label += " - " + to_wx(voice.description);
	return label;
}

void DialogVoiceCast::AddCustomVoice(std::string const& id) {
	elevenlabs::Voice voice;
	voice.id = id;
	voice.name = "Voice " + id;
	voice.account = true;
	voices.push_back(std::move(voice));
}

std::string DialogVoiceCast::SelectedVoice(Row const& row) const {
	int sel = row.choice->GetSelection();
	if (sel <= 0 || sel > static_cast<int>(voices.size())) return {};
	return voices[sel - 1].id;
}

void DialogVoiceCast::SelectVoice(Row& row, std::string const& id) {
	int index = id.empty() ? -1 : VoiceIndex(id);
	row.choice->SetSelection(index + 1);
}

void DialogVoiceCast::RefreshChoices() {
	wxArrayString labels;
	labels.push_back(_("(No voice)"));
	for (auto const& voice : voices)
		labels.push_back(VoiceLabel(voice));
	labels.push_back(_("Other voice ID..."));

	for (auto& row : rows) {
		std::string selected = row.choice->GetCount() ? SelectedVoice(row) : std::string();
		row.choice->Set(labels);
		SelectVoice(row, selected);
	}
}

void DialogVoiceCast::OnChoice(Row& row) {
	// The last entry asks for a voice ID from the ElevenLabs voice library
	if (row.choice->GetSelection() != static_cast<int>(voices.size()) + 1) {
		row.reason->SetLabel("");
		return;
	}

	wxString value = wxGetTextFromUser(
		_("Voice ID from ElevenLabs (shown under the voice's name in the ElevenLabs voice library):"),
		_("Other voice"), "", &d);
	std::string id = from_wx(value.Trim(true).Trim(false));
	if (id.empty()) {
		SelectVoice(row, {});
		return;
	}
	if (!valid_voice_id(id)) {
		wxMessageBox(_("A voice ID contains only letters and digits, like 2cecqSnkajrth9sJSoEH."), _("Voice Cast"), wxOK | wxICON_WARNING, &d);
		SelectVoice(row, {});
		return;
	}
	if (VoiceIndex(id) < 0) {
		AddCustomVoice(id);
		RefreshChoices();
	}
	SelectVoice(row, id);
	row.reason->SetLabel("");
}

bool DialogVoiceCast::CheckTtsKey(elevenlabs::Config const& config) {
	if (!config.api_key.empty() && !config.model.empty()) return true;
	wxMessageBox(
		_("No ElevenLabs API key is set.\n\nEnter it in Preferences > Voice Cast, or set the ELEVENLABS_API_KEY environment variable."),
		_("Voice Cast"), wxOK | wxICON_INFORMATION, &d);
	return false;
}

bool DialogVoiceCast::LoadAccountVoices(bool quiet) {
	if (account_loaded) return true;
	auto config = dub::LoadElevenLabsConfig();
	if (config.api_key.empty()) {
		if (!quiet) CheckTtsKey(config);
		return false;
	}

	std::vector<elevenlabs::Voice> account;
	std::string error;
	DialogProgress progress(&d, _("Voice Cast"), _("Loading your ElevenLabs voices..."));
	progress.Run([&](agi::ProgressSink *ps) {
		try {
			account = elevenlabs::AccountVoices(config, [ps] { return ps->IsCancelled(); });
		}
		catch (elevenlabs::Error const& e) {
			if (!ps->IsCancelled())
				error = e.what();
		}
	});

	if (!error.empty()) {
		if (!quiet)
			wxMessageBox(fmt_tl("Could not load your voices:\n\n%s", error), _("Voice Cast"), wxOK | wxICON_ERROR, &d);
		return false;
	}

	for (auto& voice : account) {
		int index = VoiceIndex(voice.id);
		if (index >= 0) {
			// Replace a placeholder for a voice ID typed in by hand
			if (voices[index].name == "Voice " + voice.id)
				voices[index] = std::move(voice);
		}
		else
			voices.push_back(std::move(voice));
	}
	account_loaded = true;
	RefreshChoices();
	return true;
}

void DialogVoiceCast::OnLoadAccount(wxCommandEvent&) {
	if (account_loaded) {
		wxMessageBox(_("Your voices are already in the lists."), _("Voice Cast"), wxOK | wxICON_INFORMATION, &d);
		return;
	}
	LoadAccountVoices(false);
}

void DialogVoiceCast::OnPreview(Row& row) {
	std::string voice_id = SelectedVoice(row);
	if (voice_id.empty()) {
		wxMessageBox(_("Pick a voice for this character first."), _("Voice Cast"), wxOK | wxICON_INFORMATION, &d);
		return;
	}

	auto key = std::make_pair(voice_id, row.character.name);
	auto it = previews.find(key);
	if (it == previews.end()) {
		auto config = dub::LoadElevenLabsConfig();
		if (!CheckTtsKey(config)) return;

		std::string text = preview_line(row.character);
		std::vector<char> wav;
		std::string error;
		DialogProgress progress(&d, _("Voice Cast"), fmt_tl("Generating %s's voice...", row.character.name));
		progress.Run([&](agi::ProgressSink *ps) {
			try {
				wav = elevenlabs::Speak(config, voice_id, text, [ps] { return ps->IsCancelled(); });
			}
			catch (elevenlabs::Error const& e) {
				if (!ps->IsCancelled())
					error = e.what();
			}
		});

		if (!error.empty()) {
			wxMessageBox(fmt_tl("Could not generate the preview:\n\n%s", error), _("Voice Cast"), wxOK | wxICON_ERROR, &d);
			return;
		}
		if (wav.empty()) return;
		it = previews.emplace(key, std::move(wav)).first;
	}

	wxSound::Stop();
	if (!sound.Create(it->second.size(), it->second.data()) || !sound.Play(wxSOUND_ASYNC))
		wxMessageBox(_("The preview could not be played."), _("Voice Cast"), wxOK | wxICON_ERROR, &d);
}

void DialogVoiceCast::OnAutoDetect(wxCommandEvent&) {
	auto config = dub::LoadOpenAIConfig();
	if (config.api_key.empty() || config.base_url.empty() || config.model.empty()) {
		wxMessageBox(
			_("Auto-detect uses the AI model set up for translation.\n\nEnter an API key, URL and model in Preferences > AI Translation."),
			_("Voice Cast"), wxOK | wxICON_INFORMATION, &d);
		return;
	}

	// Cast only from voices ElevenLabs has verified for Mongolian. The user's
	// own voices may have been designed for another project or language, so
	// they are left for picking by hand.
	std::vector<elevenlabs::Voice> catalogue;
	for (auto const& voice : voices) {
		if (voice.mongolian)
			catalogue.push_back(voice);
	}

	// Characters of the series keep their voices; only new ones are cast,
	// avoiding the voices the series already uses
	std::vector<Character> characters;
	for (auto const& row : rows) {
		if (!series.count(row.character.name))
			characters.push_back(row.character);
	}
	if (characters.empty()) {
		wxMessageBox(_("Every character already has a voice from the series cast."), _("Voice Cast"), wxOK | wxICON_INFORMATION, &d);
		return;
	}

	std::vector<Assignment> cast;
	for (auto const& entry : series) {
		Assignment assignment;
		assignment.character = entry.first;
		assignment.voice_id = entry.second.voice_id;
		auto row = std::find_if(rows.begin(), rows.end(), [&](Row const& r) { return r.character.name == entry.first; });
		assignment.line_count = row == rows.end() ? 0 : row->character.lines.size();
		cast.push_back(std::move(assignment));
	}
	const size_t from_series = cast.size();
	std::string error;
	bool cancelled = false;
	DialogProgress progress(&d, _("Voice Cast"), fmt_tl("Casting %d characters with %s...", (int)characters.size(), config.model));
	progress.Run([&](agi::ProgressSink *ps) {
		ps->SetProgress(0, characters.size());
		for (size_t start = 0; start < characters.size(); start += casting_batch_size) {
			if (ps->IsCancelled()) {
				cancelled = true;
				return;
			}
			size_t end = std::min(characters.size(), start + casting_batch_size);
			ps->SetMessage(from_wx(fmt_tl("Casting characters %d-%d of %d", (int)start + 1, (int)end, (int)characters.size())));

			try {
				auto content = openai::CompleteJson(config, casting_prompt(),
					casting_request(catalogue, cast, characters, start, end),
					[ps] { return ps->IsCancelled(); });
				auto root = parse_json(content);
				json::Object const& obj = root;
				auto list = obj.find("cast");
				if (list == obj.end()) throw openai::Error("The model's reply has no \"cast\" list");

				for (auto const& entry : static_cast<json::Array const&>(list->second)) {
					try {
						json::Object const& item = entry;
						auto name = item.find("character");
						auto voice = item.find("voice_id");
						if (name == item.end() || voice == item.end()) continue;
						Assignment assignment;
						assignment.character = static_cast<json::String const&>(name->second);
						assignment.voice_id = static_cast<json::String const&>(voice->second);
						auto batch_begin = characters.begin() + start;
						auto batch_end = characters.begin() + end;
						auto character = std::find_if(batch_begin, batch_end,
							[&](Character const& ch) { return ch.name == assignment.character; });
						bool in_catalogue = std::any_of(catalogue.begin(), catalogue.end(),
							[&](elevenlabs::Voice const& v) { return v.id == assignment.voice_id; });
						if (character == batch_end || !in_catalogue) continue;
						assignment.line_count = character->lines.size();
						auto why = item.find("reason");
						if (why != item.end()) {
							try { assignment.reason = static_cast<json::String const&>(why->second); }
							catch (json::Exception const&) { }
						}
						cast.push_back(std::move(assignment));
					}
					catch (json::Exception const&) {
						// Skip malformed entries; the summary reports the shortfall
					}
				}
			}
			catch (openai::Error const& e) {
				if (ps->IsCancelled())
					cancelled = true;
				else
					error = e.what();
				return;
			}
			catch (std::exception const& e) {
				error = std::string("Could not read the model's reply: ") + e.what();
				return;
			}

			ps->SetProgress(end, characters.size());
		}
	});

	// Show what was cast, even after an error or cancel: those batches
	// have already been paid for
	size_t assigned = 0;
	for (size_t i = from_series; i < cast.size(); ++i) {
		auto const& assignment = cast[i];
		auto row = std::find_if(rows.begin(), rows.end(), [&](Row const& r) { return r.character.name == assignment.character; });
		if (row == rows.end()) continue;
		SelectVoice(*row, assignment.voice_id);
		row->reason->SetLabel(to_wx(assignment.reason));
		++assigned;
	}

	d.Layout();

	if (!error.empty())
		wxMessageBox(fmt_tl("Auto-detect stopped after %d of %d characters:\n\n%s", (int)assigned, (int)characters.size(), error),
			_("Voice Cast"), wxOK | wxICON_ERROR, &d);
	else if (cancelled)
		return;
	else if (assigned < characters.size())
		wxMessageBox(fmt_tl("%d of %d characters were cast. Pick voices for the rest by hand.", (int)assigned, (int)characters.size()),
			_("Voice Cast"), wxOK | wxICON_WARNING, &d);
}

void DialogVoiceCast::SaveCast() {
	auto cast = voice_cast::Load(c->ass.get());
	auto before = cast;
	for (auto const& row : rows) {
		std::string id = SelectedVoice(row);
		if (id.empty())
			cast.erase(row.character.name);
		else
			cast[row.character.name] = id;
	}

	bool changed = cast != before;
	if (changed)
		voice_cast::Save(c->ass.get(), cast);

	if (!series_path.empty()) {
		// Add this episode's voices to the series. A character left without a
		// voice here keeps its series voice: it may just not matter this time.
		auto updated = series;
		for (auto const& row : rows) {
			std::string id = SelectedVoice(row);
			if (id.empty()) continue;
			int index = VoiceIndex(id);
			updated[row.character.name] = series_cast::Entry{id, index < 0 ? std::string() : voices[index].name};
		}
		if (updated != series || !agi::fs::FileExists(series_path)) {
			try {
				series_cast::Save(series_path, updated);
				series = std::move(updated);
			}
			catch (agi::Exception const& e) {
				wxMessageBox(fmt_tl("The series cast file could not be saved:\n\n%s", e.GetMessage()), _("Voice Cast"), wxOK | wxICON_ERROR, &d);
			}
		}

		auto subs_path = c->subsController->Filename();
		bool was_linked = series_cast::IsLinked(c->ass.get());
		auto previous = series_cast::Find(c->ass.get(), subs_path);
		series_cast::Link(c->ass.get(), subs_path, series_path);
		changed = changed || !was_linked || previous != series_path;
	}

	if (changed)
		c->ass->Commit(_("voice cast"), AssFile::COMMIT_SCRIPTINFO);
}

void DialogVoiceCast::OnDetectSpeakers(wxCommandEvent&) {
	// Keep the voices picked so far; the dialog reopens with the new characters
	wxSound::Stop();
	SaveCast();
	d.EndModal(detect_speakers_code);
}

void DialogVoiceCast::OnOK(wxCommandEvent&) {
	wxSound::Stop();
	SaveCast();
	d.EndModal(wxID_OK);
}
}

void ShowVoiceCastDialog(agi::Context *c) {
	while (true) {
		int result;
		{
			DialogVoiceCast dlg(c);
			result = dlg.d.ShowModal();
		}
		wxSound::Stop();
		if (result != detect_speakers_code) break;
		cmd::call("tool/speakers/detect/all", c);
	}
}
