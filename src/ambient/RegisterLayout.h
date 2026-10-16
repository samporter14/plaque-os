#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RegisterModel.h"

// Where the Register screen's text goes on the 480 x 800 panel. A pure function from a Model and a way to measure
// text to a list of text lines, rules and one frame, each with its place: no renderer, no fonts, so the host test can
// prove with the real font tables that every line fits, nothing overlaps, and a title or a long list is cut with an
// ellipsis rather than run off the edge (test/register_layout). The firmware draws the items it is given
// (RegisterProvider.cpp); the preview tool draws the same items to a PNG.
//
// The screen, top to bottom:
//
//   Centrifuge 5424                 the instrument's name, serif, up to two lines
//   ───────────────────────
//   ┌────────────────────┐          the state: a word and who/until when. In use is framed, Free is not
//   │ In use             │
//   │ Ana · until 3:00 PM│
//   └────────────────────┘
//   NEXT  4:00–6:00 PM  Alex        one line (the label over the booking when the time is long)
//   TODAY
//   1:00–3:00 PM   Ana              the booking in progress in bold
//   4:00–6:00 PM   Alex
//   TOMORROW
//   9:00–11:00 AM  Lee              "+ N more" when a day has more bookings than there is room for
//   (a note about what could not be read: up to two lines)
//   ───────────────────────
//   As of 2:05 PM                   or "Calendar unavailable since 1:40 PM"
namespace reg {

constexpr int PANEL_W = 480;
constexpr int PANEL_H = 800;
constexpr int MARGIN_X = 32;
constexpr int CONTENT_W = PANEL_W - 2 * MARGIN_X;
constexpr int MARGIN_BOTTOM = 28;

// The faces the screen uses. The firmware maps each to a built-in font (NotoSerif 18 bold, NotoSans 14, ...); the
// largest serif the reader has is the 18-point one, so that is the instrument's name.
enum class FontRole : uint8_t {
  Name,      // the instrument: serif, bold, the largest
  State,     // "In use", "Free": serif, bold
  Detail,    // who and until when: sans
  Row,       // a booking in a list: sans
  RowBold,   // the booking in progress, and the next one's title: sans, bold
  Note,      // the footer, a note, an empty list's sentence: small sans
  NoteBold,  // a footer that warns: small sans, bold
  Overline,  // NEXT, TODAY, TOMORROW: smaller, upper case, letter-spaced
};

class TextMeter {
 public:
  virtual ~TextMeter() = default;
  virtual int width(FontRole role, const std::string& text) const = 0;  // the advance width in pixels, as drawn
  virtual int lineHeight(FontRole role) const = 0;
};

struct Item {
  enum class Kind : uint8_t { Text, Rule, Frame };
  Kind kind = Kind::Text;
  FontRole role = FontRole::Row;  // Text only
  int x = 0;
  int y = 0;
  int w = 0;  // a text line's measured width
  int h = 0;  // a text line's height; a rule's or frame's thickness is in `thickness`
  int thickness = 0;
  std::string text;  // Text only, already fitted: ends in an ellipsis when it was cut
};

struct Layout {
  std::vector<Item> items;
  int bottom = 0;           // y below the last item
  int todayShown = 0;       // bookings listed under Today, and how many a "+ N more" row stands for
  int todayHidden = 0;
  int tomorrowShown = 0;
  int tomorrowHidden = 0;
  int page = 0;             // the full-day view: this page (0-based) of `pages`
  int pages = 1;
};

// The ellipsis, UTF-8.
constexpr char ELLIPSIS[] = "\xe2\x80\xa6";

// The text cut with an ellipsis to fit `maxWidth`, or whole when it already fits; empty when not even the ellipsis does.
std::string fitLine(const TextMeter& meter, FontRole role, const std::string& text, int maxWidth);
// The text broken into at most `maxLines` lines at spaces (a word wider than a line is broken inside); the last line
// carries an ellipsis when text is left over.
std::vector<std::string> wrapLines(const TextMeter& meter, FontRole role, const std::string& text, int maxWidth,
                                   int maxLines);

Layout layoutScreen(const Model& model, const Words& words, const TextMeter& meter);

// The full-day view, `page` counted from 0 (clamped), with a footer saying which page and how to go back.
Layout layoutFullDay(const FullDay& day, const Words& words, const TextMeter& meter, int page);

}  // namespace reg
