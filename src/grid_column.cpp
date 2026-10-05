// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

#include "grid_column.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "line_emotion.h"
#include "line_spoken.h"
#include "dub_clip.h"
#include "dub_settings.h"
#include "subs_controller.h"
#include "voice_cast.h"
#include "options.h"
#include "original_text.h"
#include "video_controller.h"

#include <libaegisub/character_count.h>

#include <algorithm>
#include <cmath>
#include <wx/control.h>
#include <wx/dc.h>
#include <wx/settings.h>

void WidthHelper::Age() {
	for (auto it = begin(widths), e = end(widths); it != e; ) {
		if (it->second.age == age)
			++it;
		else
			it = widths.erase(it);
	}
	++age;
}

int WidthHelper::operator()(boost::flyweight<std::string> const& str) {
	if (str.get().empty()) return 0;
	auto it = widths.find(str);
	if (it != end(widths)) {
		it->second.age = age;
		return it->second.width;
	}

#ifdef _WIN32
	wxMBConvUTF8 conv;
	size_t len = conv.ToWChar(nullptr, 0, str.get().c_str(), str.get().size());
	scratch.resize(len);
	conv.ToWChar(const_cast<wchar_t *>(scratch.wx_str()), len, str.get().c_str(), str.get().size());
	int width = dc->GetTextExtent(scratch).GetWidth();
#else
	int width = dc->GetTextExtent(to_wx(str)).GetWidth();
#endif

	widths[str] = {width, age};
	return width;
}

int WidthHelper::operator()(std::string const& str) {
	return dc->GetTextExtent(to_wx(str)).GetWidth();
}

int WidthHelper::operator()(wxString const& str) {
	return dc->GetTextExtent(str).GetWidth();
}

int WidthHelper::operator()(const char *str) {
	return dc->GetTextExtent(wxString::FromUTF8(str)).GetWidth();
}

int WidthHelper::operator()(const wchar_t *str) {
	return dc->GetTextExtent(str).GetWidth();
}

void GridColumn::UpdateWidth(const agi::Context *c, WidthHelper &helper) {
	if (!visible) {
		width = 0;
		return;
	}

	width = Width(c, helper);
	if (width) // 10 is an arbitrary amount of padding
		width = 10 + std::max(width, helper(Header()));
}

void GridColumn::Paint(wxDC &dc, int x, int y, const AssDialogue *d, const agi::Context *c) const {
	wxString str = Value(d, c);
	if (Centered())
		x += (width - 6 - dc.GetTextExtent(str).GetWidth()) / 2;
	dc.DrawText(str, x + 4, y + 2);
}

namespace {
#define COLUMN_HEADER(value) \
	private: const wxString header = value; \
	public: wxString const& Header() const override { return header; }
#define COLUMN_DESCRIPTION(value) \
	private: const wxString description = value; \
	public: wxString const& Description() const override { return description; }

struct GridColumnLineNumber final : GridColumn {
	COLUMN_HEADER(_("#"))
	COLUMN_DESCRIPTION(_("Line Number"))
	bool Centered() const override { return true; }

	wxString Value(const AssDialogue *d, const agi::Context * = nullptr) const override {
		return std::to_wstring(d->Row + 1);
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		if (c->ass->Events.empty())
			return helper("1");
		return helper(Value(&c->ass->Events.back()));
	}
};

template<typename T>
T max_value(T AssDialogueBase::*field, EntryList<AssDialogue> const& lines) {
	T value = 0;
	for (AssDialogue const& line : lines) {
		if (line.*field > value)
			value = line.*field;
	}
	return value;
}

struct GridColumnLayer final : GridColumn {
	COLUMN_HEADER(_("L"))
	COLUMN_DESCRIPTION(_("Layer"))
	bool Centered() const override { return true; }

	wxString Value(const AssDialogue *d, const agi::Context *) const override {
		return d->Layer ? wxString(std::to_wstring(d->Layer)) : wxString();
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		int max_layer = max_value(&AssDialogue::Layer, c->ass->Events);
		return max_layer == 0 ? 0 : helper(std::to_wstring(max_layer));
	}
};

struct GridColumnTime : GridColumn {
	bool by_frame = false;

	bool Centered() const override { return true; }
	void SetByFrame(bool by_frame) override { this->by_frame = by_frame; }
};

struct GridColumnStartTime final : GridColumnTime {
	COLUMN_HEADER(_("Start"))
	COLUMN_DESCRIPTION(_("Start Time"))

	wxString Value(const AssDialogue *d, const agi::Context *c) const override {
		if (by_frame)
			return std::to_wstring(c->videoController->FrameAtTime(d->Start, agi::vfr::START));
		return to_wx(d->Start.GetAssFormatted());
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		if (!by_frame)
			return helper(wxS("0:00:00.00"));
		int frame = c->videoController->FrameAtTime(max_value(&AssDialogue::Start, c->ass->Events), agi::vfr::START);
		return helper(std::to_wstring(frame));
	}
};

struct GridColumnEndTime final : GridColumnTime {
	COLUMN_HEADER(_("End"))
	COLUMN_DESCRIPTION(_("End Time"))

	wxString Value(const AssDialogue *d, const agi::Context *c) const override {
		if (by_frame)
			return std::to_wstring(c->videoController->FrameAtTime(d->End, agi::vfr::END));
		return to_wx(d->End.GetAssFormatted());
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		if (!by_frame)
			return helper(wxS("0:00:00.00"));
		int frame = c->videoController->FrameAtTime(max_value(&AssDialogue::End, c->ass->Events), agi::vfr::END);
		return helper(std::to_wstring(frame));
	}
};

template<typename T>
int max_width(T AssDialogueBase::*field, EntryList<AssDialogue> const& lines, WidthHelper &helper) {
	int w = 0;
	for (AssDialogue const& line : lines) {
		auto const& v = line.*field;
		if (v.get().empty()) continue;
		int width = helper(v);
		if (width > w)
			w = width;
	}
	return w;
}

struct GridColumnStyle final : GridColumn {
	COLUMN_HEADER(_("Style"))
	COLUMN_DESCRIPTION(_("Style"))
	bool Centered() const override { return false; }

	wxString Value(const AssDialogue *d, const agi::Context *) const override {
		return to_wx(d->Style);
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		return max_width(&AssDialogue::Style, c->ass->Events, helper);
	}
};

struct GridColumnEffect final : GridColumn {
	COLUMN_HEADER(_("Effect"))
	COLUMN_DESCRIPTION(_("Effect"))
	bool Centered() const override { return false; }

	wxString Value(const AssDialogue *d, const agi::Context *) const override {
		return to_wx(d->Effect);
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		return max_width(&AssDialogue::Effect, c->ass->Events, helper);
	}
};

struct GridColumnActor final : GridColumn {
	COLUMN_HEADER(_("Character"))
	COLUMN_DESCRIPTION(_("Character (Actor)"))
	bool Centered() const override { return false; }
	bool IsActorColumn() const override { return true; }

	wxString Value(const AssDialogue *d, const agi::Context *) const override {
		return to_wx(d->Actor);
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		// Never collapse to nothing: the cell is what the user clicks on to
		// assign a character to a line
		return std::max(max_width(&AssDialogue::Actor, c->ass->Events, helper), helper(L"XXXXXXXX"));
	}
};

/// Delivery direction of the line for dubbing, as ElevenLabs audio tags
struct GridColumnEmotion final : GridColumn {
	COLUMN_HEADER(_("Emotion"))
	COLUMN_DESCRIPTION(_("Emotion (Dubbing)"))
	bool Centered() const override { return false; }
	bool IsEmotionColumn() const override { return true; }

	wxString Value(const AssDialogue *d, const agi::Context *c) const override {
		if (d->ExtradataIds.get().empty()) return wxString();
		return to_wx(line_emotion::Get(c->ass.get(), d));
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		// Never collapse to nothing: the cell is what the user clicks on to
		// set a line's emotion
		int w = helper(L"[surprised]");
		const int cap = helper(L"x") * 30;
		for (AssDialogue const& line : c->ass->Events) {
			if (line.ExtradataIds.get().empty()) continue;
			w = std::max(w, std::min(cap, helper(line_emotion::Get(c->ass.get(), &line))));
			if (w >= cap) break;
		}
		return w;
	}

	void Paint(wxDC &dc, int x, int y, const AssDialogue *d, const agi::Context *c) const override {
		wxString str = Value(d, c);
		if (str.empty()) return;
		dc.DrawText(wxControl::Ellipsize(str, dc, wxELLIPSIZE_END, std::max(0, width - 8), wxELLIPSIZE_FLAGS_NONE), x + 4, y + 2);
	}
};

/// The line written out for the speech engine, when it differs from the text
struct GridColumnSpoken final : GridColumn {
	COLUMN_HEADER(_("Spoken"))
	COLUMN_DESCRIPTION(_("Spoken Text (Dubbing)"))
	bool Centered() const override { return false; }
	bool RefreshOnTextChange() const override { return true; }
	bool IsSpokenColumn() const override { return true; }

	wxString Value(const AssDialogue *d, const agi::Context *c) const override {
		return to_wx(line_spoken::Get(c->ass.get(), d));
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		// Hidden entirely until at least one line has a spoken text
		int w = 0;
		const int cap = helper(L"x") * 40;
		for (AssDialogue const& line : c->ass->Events) {
			if (line.ExtradataIds.get().empty()) continue;
			auto text = line_spoken::Get(c->ass.get(), &line);
			if (text.empty()) continue;
			w = std::max(w, std::min(cap, helper(text)));
			if (w >= cap) break;
		}
		return w;
	}

	void Paint(wxDC &dc, int x, int y, const AssDialogue *d, const agi::Context *c) const override {
		wxString str = Value(d, c);
		if (str.empty()) return;
		dc.DrawText(wxControl::Ellipsize(str, dc, wxELLIPSIZE_END, std::max(0, width - 8), wxELLIPSIZE_FLAGS_NONE), x + 4, y + 2);
	}
};

/// Pre-translation text of the line, as stored by the translation commands
struct GridColumnOriginal final : GridColumn {
	COLUMN_HEADER(_("Original"))
	COLUMN_DESCRIPTION(_("Original Text"))
	bool Centered() const override { return false; }
	bool RefreshOnTextChange() const override { return true; }

	wxString Value(const AssDialogue *d, const agi::Context *c) const override {
		if (d->ExtradataIds.get().empty()) return wxString();
		wxString str = to_wx(original_text::Get(c->ass.get(), d));
		if (str.size() > 512)
			str = str.Left(512) + "...";
		return str;
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		// Hidden entirely until at least one line has an original text
		int w = 0;
		const int cap = helper(L"x") * 45;
		for (AssDialogue const& line : c->ass->Events) {
			if (line.ExtradataIds.get().empty()) continue;
			auto text = original_text::Get(c->ass.get(), &line);
			if (text.empty()) continue;
			w = std::max(w, std::min(cap, helper(text)));
			if (w >= cap) break;
		}
		return w;
	}

	void Paint(wxDC &dc, int x, int y, const AssDialogue *d, const agi::Context *c) const override {
		wxString str = Value(d, c);
		if (str.empty()) return;
		// The grid does not clip cells, so shorten the text to fit instead
		dc.DrawText(wxControl::Ellipsize(str, dc, wxELLIPSIZE_END, std::max(0, width - 8), wxELLIPSIZE_FLAGS_NONE), x + 4, y + 2);
	}
};

struct GridColumnMargin : GridColumn {
	int index;
	GridColumnMargin(int index) : index(index) { }

	bool Centered() const override { return true; }

	wxString Value(const AssDialogue *d, const agi::Context *) const override {
		return d->Margin[index] ? wxString(std::to_wstring(d->Margin[index])) : wxString();
	}

	int Width(const agi::Context *c, WidthHelper &helper) const override {
		int max = 0;
		for (AssDialogue const& line : c->ass->Events) {
			if (line.Margin[index] > max)
				max = line.Margin[index];
		}
		return max == 0 ? 0 : helper(std::to_wstring(max));
	}
};

struct GridColumnMarginLeft final : GridColumnMargin {
	GridColumnMarginLeft() : GridColumnMargin(0) { }
	COLUMN_HEADER(_("Left"))
	COLUMN_DESCRIPTION(_("Left Margin"))
};

struct GridColumnMarginRight final : GridColumnMargin {
	GridColumnMarginRight() : GridColumnMargin(1) { }
	COLUMN_HEADER(_("Right"))
	COLUMN_DESCRIPTION(_("Right Margin"))
};

struct GridColumnMarginVert final : GridColumnMargin {
	GridColumnMarginVert() : GridColumnMargin(2) { }
	COLUMN_HEADER(_("Vert"))
	COLUMN_DESCRIPTION(_("Vertical Margin"))
};

wxColor blend(wxColor fg, wxColor bg, double alpha) {
	return wxColor(
		wxColor::AlphaBlend(fg.Red(), bg.Red(), alpha),
		wxColor::AlphaBlend(fg.Green(), bg.Green(), alpha),
		wxColor::AlphaBlend(fg.Blue(), bg.Blue(), alpha));
}

class GridColumnCPS final : public GridColumn {
	const agi::OptionValue *ignore_whitespace = OPT_GET("Subtitle/Character Counter/Ignore Whitespace");
	const agi::OptionValue *ignore_punctuation = OPT_GET("Subtitle/Character Counter/Ignore Punctuation");
	const agi::OptionValue *cps_warn = OPT_GET("Subtitle/Character Counter/CPS Warning Threshold");
	const agi::OptionValue *cps_error = OPT_GET("Subtitle/Character Counter/CPS Error Threshold");
	const agi::OptionValue *bg_color = OPT_GET("Colour/Subtitle Grid/CPS Error");

public:
	COLUMN_HEADER(_("CPS"))
	COLUMN_DESCRIPTION(_("Characters Per Second"))
	bool Centered() const override { return true; }
	bool RefreshOnTextChange() const override { return true; }

	wxString Value(const AssDialogue *, const agi::Context *) const override {
		return wxS("");
	}

	int CPS(const AssDialogue *d) const {
		int duration = d->End - d->Start;
		auto const& text = d->Text.get();

		if (duration <= 100 || text.size() > static_cast<size_t>(duration))
			return -1;

		int ignore = agi::IGNORE_BLOCKS;
		if (ignore_whitespace->GetBool())
			ignore |= agi::IGNORE_WHITESPACE;
		if (ignore_punctuation->GetBool())
			ignore |= agi::IGNORE_PUNCTUATION;

		return agi::CharacterCount(text, ignore) * 1000 / duration;
	}

	int Width(const agi::Context *, WidthHelper &helper) const override {
		return helper(wxS("999"));
	}

	void Paint(wxDC &dc, int x, int y, const AssDialogue *d, const agi::Context *) const override {
		int cps = CPS(d);
		if (cps < 0 || cps > 100) return;

		wxString str = std::to_wstring(cps);
		wxSize ext = dc.GetTextExtent(str);
		auto tc = dc.GetTextForeground();

		int cps_min = cps_warn->GetInt();
		int cps_max = std::max<int>(cps_min, cps_error->GetInt());
		if (cps > cps_min) {
			double alpha = std::min((double)(cps - cps_min + 1) / (cps_max - cps_min + 1), 1.0);
			dc.SetBrush(wxBrush(blend(to_wx(bg_color->GetColor()), dc.GetBrush().GetColour(), alpha)));
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.DrawRectangle(x, y + 1, width, ext.GetHeight() + 3);
			dc.SetTextForeground(blend(*wxBLACK, tc, alpha));
		}

		x += (width + 2 - ext.GetWidth()) / 2;
		dc.DrawText(str, x, y + 2);
		dc.SetTextForeground(tc);
	}
};

/// How much of the time before the next line the line's generated speech
/// takes, like CPS but measured on the actual dub
class GridColumnFit final : public GridColumn {
	const agi::OptionValue *bg_color = OPT_GET("Colour/Subtitle Grid/CPS Error");

	// Parsing the cast for every row would be slow; reuse it while the
	// script's cast entry is unchanged
	mutable std::string cast_source;
	mutable voice_cast::Cast cast;

	voice_cast::Cast const& Cast(const agi::Context *c) const {
		std::string current(c->ass->GetScriptInfo("Aegidub Voice Cast"));
		if (current != cast_source) {
			cast = voice_cast::Load(c->ass.get());
			cast_source = std::move(current);
		}
		return cast;
	}

public:
	COLUMN_HEADER(_("Fit"))
	COLUMN_DESCRIPTION(_("Dub Fit (speech length / time before the next line; amber is sped up to fit, red needs shortening)"))
	bool Centered() const override { return true; }
	bool RefreshOnTextChange() const override { return true; }

	/// Percentage, or -1 when the line has no generated speech
	int Percent(const AssDialogue *d, const agi::Context *c) const {
		auto subs = c->subsController->Filename();
		if (subs.empty() || d->Comment) return -1;
		dub_clip::Request request;
		if (!dub_clip::ForLine(c->ass.get(), d, Cast(c), request, nullptr)) return -1;
		int length = dub_clip::DurationMs(dub_clip::Path(subs, dub::LoadElevenLabsConfig(), request));
		if (length < 0) return -1;
		int room = dub_clip::RoomMs(c->ass.get(), d);
		if (room <= 0) return 999;
		return std::min(999, length * 100 / room);
	}

	wxString Value(const AssDialogue *d, const agi::Context *c) const override {
		int percent = Percent(d, c);
		return percent < 0 ? wxString() : wxString(std::to_wstring(percent) + L"%");
	}

	int Width(const agi::Context *, WidthHelper &helper) const override {
		return helper(wxS("999%"));
	}

	void Paint(wxDC &dc, int x, int y, const AssDialogue *d, const agi::Context *c) const override {
		int percent = Percent(d, c);
		// A dash marks speech that still has to be generated
		wxString str = percent < 0 ? wxString(d->Comment || d->Actor.get().empty() ? L"" : L"\u2013")
		                           : wxString(std::to_wstring(percent) + L"%");
		if (str.empty()) return;
		wxSize ext = dc.GetTextExtent(str);
		auto tc = dc.GetTextForeground();

		// Amber while speeding it up makes it fit, red once even that won't
		const int max_percent = static_cast<int>(std::lround(dub_clip::MaxTempo() * 100));
		if (percent > 100 && percent <= max_percent) {
			dc.SetBrush(wxBrush(blend(wxColour(255, 190, 60), dc.GetBrush().GetColour(), 0.7)));
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.DrawRectangle(x, y + 1, width, ext.GetHeight() + 3);
		}
		else if (percent > 100) {
			double alpha = std::min((percent - max_percent) / 10.0 + 0.5, 1.0);
			dc.SetBrush(wxBrush(blend(to_wx(bg_color->GetColor()), dc.GetBrush().GetColour(), alpha)));
			dc.SetPen(*wxTRANSPARENT_PEN);
			dc.DrawRectangle(x, y + 1, width, ext.GetHeight() + 3);
			dc.SetTextForeground(blend(*wxBLACK, tc, alpha));
		}
		else if (percent < 0)
			dc.SetTextForeground(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));

		x += (width + 2 - ext.GetWidth()) / 2;
		dc.DrawText(str, x, y + 2);
		dc.SetTextForeground(tc);
	}
};

class GridColumnText final : public GridColumn {
	const agi::OptionValue *override_mode;
	wxString replace_char;

	agi::signal::Connection replace_char_connection;

public:
	GridColumnText()
	: override_mode(OPT_GET("Subtitle/Grid/Hide Overrides"))
	, replace_char(to_wx(OPT_GET("Subtitle/Grid/Hide Overrides Char")->GetString()))
	, replace_char_connection(OPT_SUB("Subtitle/Grid/Hide Overrides Char",
		[&](agi::OptionValue const& v) { replace_char = to_wx(v.GetString()); }))
	{
	}

	COLUMN_HEADER(_("Text"))
	COLUMN_DESCRIPTION(_("Text"))
	bool Centered() const override { return false; }
	bool CanHide() const override { return false; }
	bool RefreshOnTextChange() const override { return true; }

	wxString Value(const AssDialogue *d, const agi::Context *) const override {
		wxString str;
		int mode = override_mode->GetInt();

		// Show overrides
		if (mode == 0)
			str = to_wx(d->Text);
		// Hidden overrides
		else {
			auto const& text = d->Text.get();
			str.reserve(text.size());
			size_t start = 0, pos;
			while ((pos = text.find('{', start)) != std::string::npos) {
				str += to_wx(text.substr(start, pos - start));
				if (mode == 1)
					str += replace_char;
				start = text.find('}', pos);
				if (start != std::string::npos) ++start;
			}
			if (start != std::string::npos)
				str += to_wx(text.substr(start));
		}

		// Cap length and set text
		if (str.size() > 512)
			str = str.Left(512) + "...";
		return str;
	}

	int Width(const agi::Context *, WidthHelper &) const override {
		return 5000;
	}
};

template<typename T>
std::unique_ptr<GridColumn> make() {
	return std::unique_ptr<GridColumn>(new T);
}

}

std::vector<std::unique_ptr<GridColumn>> GetGridColumns() {
	std::vector<std::unique_ptr<GridColumn>> ret;
	ret.push_back(make<GridColumnLineNumber>());
	ret.push_back(make<GridColumnActor>());
	ret.push_back(make<GridColumnEmotion>());
	ret.push_back(make<GridColumnLayer>());
	ret.push_back(make<GridColumnStartTime>());
	ret.push_back(make<GridColumnEndTime>());
	ret.push_back(make<GridColumnCPS>());
	ret.push_back(make<GridColumnFit>());
	ret.push_back(make<GridColumnStyle>());
	ret.push_back(make<GridColumnEffect>());
	ret.push_back(make<GridColumnMarginLeft>());
	ret.push_back(make<GridColumnMarginRight>());
	ret.push_back(make<GridColumnMarginVert>());
	ret.push_back(make<GridColumnOriginal>());
	ret.push_back(make<GridColumnSpoken>());
	ret.push_back(make<GridColumnText>());
	return ret;
}
