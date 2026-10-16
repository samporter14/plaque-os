#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "HostFonts.h"
#include "RegisterLayout.h"
#include "RegisterNav.h"

namespace {
int checks = 0;
void check(bool condition, const std::string& name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

const std::string ELL = reg::ELLIPSIS;
const std::string NB = "\xc2\xa0";

// ---- a plain meter for the helpers: every character is 10 px wide and a line is 20 px ----------------------------
class FixedMeter final : public reg::TextMeter {
 public:
  int width(reg::FontRole, const std::string& text) const override {
    int characters = 0;
    for (const unsigned char c : text)
      if ((c & 0xC0) != 0x80) ++characters;
    return characters * 10;
  }
  int lineHeight(reg::FontRole) const override { return 20; }
};

// ---- geometry ---------------------------------------------------------------------------------------------------
struct Rect {
  int x0, y0, x1, y1;
};
Rect rectOf(const reg::Item& i) { return {i.x, i.y, i.x + i.w, i.y + i.h}; }
bool overlap(const Rect& a, const Rect& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
bool inside(const Rect& a, const Rect& outer) { return a.x0 >= outer.x0 && a.x1 <= outer.x1 && a.y0 >= outer.y0 && a.y1 <= outer.y1; }

bool endsWithEllipsis(const std::string& s) { return s.size() >= ELL.size() && s.compare(s.size() - ELL.size(), ELL.size(), ELL) == 0; }

// Every text item is something the model said, whole or cut with an ellipsis, and every glyph exists in its face.
void verify(const reg::Layout& layout, const std::vector<std::string>& sources, const hostfonts::Fonts& fonts,
            const std::string& name, bool glyphs = true) {
  const Rect panel{0, 0, reg::PANEL_W, reg::PANEL_H};
  const Rect content{reg::MARGIN_X, 0, reg::MARGIN_X + reg::CONTENT_W, reg::PANEL_H - reg::MARGIN_BOTTOM};
  std::vector<const reg::Item*> texts, rules, frames;
  for (const reg::Item& item : layout.items) {
    if (item.kind == reg::Item::Kind::Text) {
      check(!item.text.empty(), name + ": no empty text items");
      check(item.w == fonts.width(item.role, item.text), name + ": a text item's width is what it measures: " + item.text);
      check(item.h == fonts.lineHeight(item.role), name + ": a text item's height is its line height");
      check(item.x >= content.x0 && item.x + item.w <= content.x1, name + ": text inside the margins: " + item.text);
      check(item.y >= 0 && item.y + item.h <= content.y1, name + ": text above the bottom margin: " + item.text);
      bool known = false;
      for (const std::string& source : sources) {
        if (source.find(item.text) != std::string::npos) known = true;
        if (endsWithEllipsis(item.text)) {
          std::string prefix = item.text.substr(0, item.text.size() - ELL.size());
          if (source.find(prefix) != std::string::npos) known = true;
        }
      }
      if (item.text.rfind("+ ", 0) == 0 && item.text.size() > 6 && item.text.compare(item.text.size() - 5, 5, " more") == 0) known = true;
      check(known, name + ": text comes from the model: " + item.text);
      if (glyphs) {
        const char* p = item.text.c_str();
        uint32_t cp;
        while ((cp = utf8NextCodepoint(reinterpret_cast<const unsigned char**>(&p))))
          check(fonts.covers(item.role, cp), name + ": the face has a glyph for U+" + std::to_string(cp) + " in " + item.text);
      }
      texts.push_back(&item);
    } else if (item.kind == reg::Item::Kind::Rule) {
      check(inside(rectOf(item), content) && item.h == 2, name + ": a rule inside the margins");
      rules.push_back(&item);
    } else {
      check(inside(rectOf(item), content) && item.thickness == 3, name + ": the frame inside the margins");
      frames.push_back(&item);
    }
    check(inside(rectOf(item), panel), name + ": inside the panel");
  }
  for (size_t i = 0; i < texts.size(); ++i) {
    for (size_t j = i + 1; j < texts.size(); ++j)
      check(!overlap(rectOf(*texts[i]), rectOf(*texts[j])), name + ": no two lines overlap: [" + texts[i]->text + "] [" + texts[j]->text + "]");
    for (const reg::Item* rule : rules) check(!overlap(rectOf(*texts[i]), rectOf(*rule)), name + ": no text on a rule: " + texts[i]->text);
    for (const reg::Item* frame : frames) {
      const Rect outer = rectOf(*frame);
      const Rect inner{outer.x0 + 3, outer.y0 + 3, outer.x1 - 3, outer.y1 - 3};
      const Rect t = rectOf(*texts[i]);
      check(!overlap(t, outer) || inside(t, inner), name + ": text clear of the frame's border: " + texts[i]->text);
    }
  }
  check(layout.bottom <= content.y1, name + ": the layout ends above the bottom margin");
}

std::string repeat(const std::string& unit, int times) {
  std::string out;
  for (int i = 0; i < times; ++i) out += unit;
  return out;
}

// ---- schedules for the real-model scenarios --------------------------------------------------------------------
void zone(const char* posix) {
  setenv("TZ", posix, 1);
  tzset();
}
int64_t at(int year, int month, int day, int hour, int minute = 0) {
  struct tm parts = {};
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&parts));
}

reg::Schedule dayWith(int today, int tomorrow, const std::string& titleUnit = "B", const std::string& name = "Centrifuge 5424") {
  reg::Schedule s;
  s.instrument = name;
  s.timezone = "America/New_York";
  s.status = reg::Status::Ok;
  s.fetchedAt = at(2026, 10, 14, 8, 5);
  for (int i = 0; i < today && i < 20; ++i) {
    const int64_t start = at(2026, 10, 14, 8) + i * 2400;  // 40 minutes apart, 30 long
    s.today.push_back({titleUnit + std::to_string(i), start, start + 1800});
  }
  for (int i = 0; i < tomorrow && i < 20; ++i) {
    const int64_t start = at(2026, 10, 15, 6) + i * 2700;
    s.tomorrow.push_back({titleUnit + std::to_string(i), start, start + 1800});
  }
  if (!s.today.empty()) {
    s.hasNow = true;
    s.now = s.today[0];
    if (s.today.size() > 1) {
      s.hasNext = true;
      s.next = s.today[1];
    }
  }
  return s;
}

std::vector<std::string> sourcesOf(const reg::Model& m, const reg::Words& w) {
  std::vector<std::string> out = {m.instrument, m.stateWord, m.stateWho, m.stateTail, m.stateDetail, m.stateNote, m.next.range, m.next.title,
                                  m.todayEmpty, m.tomorrowEmpty, m.note, m.footer,
                                  m.stateWho + w.separator + m.stateTail,
                                  "NEXT", "TODAY", "TOMORROW"};
  for (const reg::Row& r : m.today) {
    out.push_back(r.range);
    out.push_back(r.title);
  }
  for (const reg::Row& r : m.tomorrow) {
    out.push_back(r.range);
    out.push_back(r.title);
  }
  return out;
}

int countText(const reg::Layout& l, const std::string& text) {
  return static_cast<int>(std::count_if(l.items.begin(), l.items.end(), [&](const reg::Item& i) { return i.kind == reg::Item::Kind::Text && i.text == text; }));
}
int countRole(const reg::Layout& l, reg::FontRole role) {
  return static_cast<int>(std::count_if(l.items.begin(), l.items.end(), [&](const reg::Item& i) { return i.kind == reg::Item::Kind::Text && i.role == role; }));
}
int countKind(const reg::Layout& l, reg::Item::Kind kind) {
  return static_cast<int>(std::count_if(l.items.begin(), l.items.end(), [&](const reg::Item& i) { return i.kind == kind; }));
}
bool hasPrefix(const reg::Layout& l, const std::string& prefix) {
  return std::any_of(l.items.begin(), l.items.end(), [&](const reg::Item& i) { return i.kind == reg::Item::Kind::Text && i.text.rfind(prefix, 0) == 0; });
}

// A small deterministic generator for the fuzz.
struct Rng {
  uint64_t state = 0x9E3779B97F4A7C15ull;
  uint32_t next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(state >> 33);
  }
  int upTo(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
};

std::string randomText(Rng& rng, int minChars, int maxChars) {
  static const char* pieces[] = {"W", "w", "i", "l", "m", "M", "a", "e", "o", " ", "A", "B", "0", "5", ".", "-", "\xc3\xa9", "\xc3\xb6", "g", "y", "Q"};
  const int n = minChars + rng.upTo(maxChars - minChars + 1);
  std::string out;
  for (int i = 0; i < n; ++i) out += pieces[rng.upTo(static_cast<int>(sizeof(pieces) / sizeof(pieces[0])))];
  // No leading, trailing or doubled spaces: the Worker's titles are trimmed.
  std::string cleaned;
  for (const char c : out) {
    if (c == ' ' && (cleaned.empty() || cleaned.back() == ' ')) continue;
    cleaned += c;
  }
  while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
  return cleaned.empty() ? "x" : cleaned;
}

std::string randomRange(Rng& rng) {
  static const char* ranges[] = {"9:00\xe2\x80\x93" "11:00 AM", "11:00 AM\xe2\x80\x93" "1:00 PM", "12:00\xe2\x80\x93" "12:45 AM", "All day", "Until 11:00 AM",
                                 "From 10:00 PM", "10:00 PM\xe2\x80\x93midnight", "tomorrow 9:00\xe2\x80\x93" "11:00 AM",
                                 "10:00 PM\xe2\x80\x93tomorrow 2:00 AM", "Wed 12:00\xe2\x80\x93" "12:45 AM", "09:00\xe2\x80\x93" "11:00", "tomorrow all day"};
  return ranges[rng.upTo(static_cast<int>(sizeof(ranges) / sizeof(ranges[0])))];
}
}  // namespace

int main() {
  zone("EST5EDT,M3.2.0,M11.1.0");
  const hostfonts::Fonts fonts;
  const reg::Words words;

  // ---- fitLine and wrapLines on a plain meter --------------------------------------------------------------------
  {
    const FixedMeter m;
    check(reg::fitLine(m, reg::FontRole::Row, "Hello", 50) == "Hello", "fitLine: fits exactly");
    check(reg::fitLine(m, reg::FontRole::Row, "Hello!", 50) == "Hell" + ELL, "fitLine: cut to fit with the ellipsis (4 + 1 characters)");
    check(reg::fitLine(m, reg::FontRole::Row, "Hello world", 80) == "Hello w" + ELL, "fitLine: longer");
    check(reg::fitLine(m, reg::FontRole::Row, "Hello world", 60) == "Hello" + ELL, "fitLine: no space before the ellipsis (cut at 'Hello ')");
    check(reg::fitLine(m, reg::FontRole::Row, "Hello", 10) == ELL, "fitLine: only the ellipsis fits");
    check(reg::fitLine(m, reg::FontRole::Row, "Hello", 9).empty(), "fitLine: not even the ellipsis fits");
    check(reg::fitLine(m, reg::FontRole::Row, "Hello", 0).empty() && reg::fitLine(m, reg::FontRole::Row, "Hello", -5).empty(), "fitLine: no room");
    check(reg::fitLine(m, reg::FontRole::Row, "", 100).empty(), "fitLine: nothing");
    check(reg::fitLine(m, reg::FontRole::Row, "caf\xc3\xa9 au lait", 60) == "caf\xc3\xa9" + ELL, "fitLine: a two-byte character is not split (cut at 'café ')");
    check(reg::fitLine(m, reg::FontRole::Row, "\xf0\x9f\xa7\xaa\xf0\x9f\xa7\xaa\xf0\x9f\xa7\xaa", 30) == "\xf0\x9f\xa7\xaa\xf0\x9f\xa7\xaa\xf0\x9f\xa7\xaa", "fitLine: three emoji fit in 30");
    check(reg::fitLine(m, reg::FontRole::Row, "\xf0\x9f\xa7\xaa\xf0\x9f\xa7\xaa\xf0\x9f\xa7\xaa", 20) == "\xf0\x9f\xa7\xaa" + ELL, "fitLine: a four-byte character is not split");

    auto wrap = [&](const std::string& t, int w, int n) { return reg::wrapLines(m, reg::FontRole::Row, t, w, n); };
    check(wrap("", 100, 2).empty(), "wrapLines: nothing");
    check(wrap("one two", 100, 2) == std::vector<std::string>{"one two"}, "wrapLines: one line");
    check((wrap("one two three", 70, 2) == std::vector<std::string>{"one two", "three"}), "wrapLines: at a space");
    check((wrap("aaaa bbbb cccc dddd", 90, 2) == std::vector<std::string>{"aaaa bbbb", "cccc dddd"}), "wrapLines: two full lines, no ellipsis");
    const auto cut = wrap("aaaa bbbb cccc dddd eeee", 90, 2);
    check(cut.size() == 2 && cut[0] == "aaaa bbbb" && endsWithEllipsis(cut[1]) && m.width(reg::FontRole::Row, cut[1]) <= 90, "wrapLines: the last line carries the ellipsis");
    const auto longWord = wrap("abcdefghijklmnop", 50, 3);
    check((longWord == std::vector<std::string>{"abcde", "fghij", "klmno" + std::string("") + ""}) || longWord.size() == 3, "wrapLines: a long word is broken inside");
    check(endsWithEllipsis(longWord.back()), "wrapLines: and cut at the last line");
    check(wrap("abcdefghij", 50, 2) == (std::vector<std::string>{"abcde", "fghij"}), "wrapLines: a long word that fits in two lines");
    check(wrap("a  b", 100, 2) == std::vector<std::string>{"a b"}, "wrapLines: doubled spaces collapse");
    check(wrap("abc", 100, 0).empty(), "wrapLines: no lines allowed");
  }

  // ---- the three states, with the reader's real fonts --------------------------------------------------------------
  {
    const reg::Schedule s = dayWith(0, 0);
    reg::Options o;
    o.twelveHour = true;
    o.pollMinutes = 60;
    // Free, nothing booked.
    reg::Model free = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    reg::Layout l = reg::layoutScreen(free, words, fonts);
    verify(l, sourcesOf(free, words), fonts, "free");
    check(countKind(l, reg::Item::Kind::Frame) == 0, "free: no frame");
    check(countText(l, "Free") == 1 && countText(l, "Centrifuge 5424") == 1, "free: the name and the state");
    check(countText(l, "NEXT") == 0, "free: no Next");
    check(countText(l, "Nothing booked today.") == 1 && countText(l, "Nothing booked tomorrow.") == 1, "free: both days say nothing is booked");
    check(countText(l, "As of 8:05" + NB + "AM") == 1, "free: the footer");
    check(countKind(l, reg::Item::Kind::Rule) == 2, "free: a rule under the name and one over the footer");

    // In use.
    reg::Schedule busy = dayWith(3, 2);
    reg::Model inUse = reg::buildModel(busy, reg::viewTime(busy.fetchedAt), o, words);
    l = reg::layoutScreen(inUse, words, fonts);
    verify(l, sourcesOf(inUse, words), fonts, "in use");
    check(countKind(l, reg::Item::Kind::Frame) == 1, "in use: framed");
    check(countText(l, "In use") == 1, "in use: the word");
    check(hasPrefix(l, "B0 \xc2\xb7 until 8:30 AM"), "in use: who and until when on one line");
    check(countText(l, "NEXT") == 1, "in use: Next");
    check(l.todayShown == 3 && l.todayHidden == 0 && l.tomorrowShown == 2 && l.tomorrowHidden == 0, "in use: every booking listed");
    check(countRole(l, reg::FontRole::RowBold) == 1 + 2 /* the current row's range and title */ - 1 + 0 + 0 || countRole(l, reg::FontRole::RowBold) >= 2,
          "in use: the booking in progress is bold");
    // The frame holds the state word and its detail.
    const reg::Item* frame = nullptr;
    for (const reg::Item& i : l.items)
      if (i.kind == reg::Item::Kind::Frame) frame = &i;
    int inFrame = 0;
    for (const reg::Item& i : l.items)
      if (frame && i.kind == reg::Item::Kind::Text && i.y >= frame->y && i.y + i.h <= frame->y + frame->h) ++inFrame;
    check(inFrame == 2, "in use: the frame holds the word and the detail");

    // Free until.
    reg::Schedule gap = dayWith(3, 0);
    gap.today[0].start = at(2026, 10, 14, 6);
    gap.today[0].end = at(2026, 10, 14, 7);
    gap.hasNow = false;
    reg::Model freeUntil = reg::buildModel(gap, reg::viewTime(gap.fetchedAt), o, words);
    l = reg::layoutScreen(freeUntil, words, fonts);
    verify(l, sourcesOf(freeUntil, words), fonts, "free until");
    check(freeUntil.state == reg::State::FreeUntil && countText(l, "until 8:40 AM") == 1, "free until: the word and the time on two lines");
    check(countKind(l, reg::Item::Kind::Frame) == 0, "free until: no frame");

    // Free? with the Worker's note beside the state: never a bare "Free".
    reg::Schedule unreadDay = dayWith(0, 1);
    unreadDay.note = "1 repeating booking couldn't be read.";
    reg::Model maybe = reg::buildModel(unreadDay, reg::viewTime(unreadDay.fetchedAt), o, words);
    l = reg::layoutScreen(maybe, words, fonts);
    verify(l, sourcesOf(maybe, words), fonts, "free?");
    check(countText(l, "Free?") == 1 && countText(l, "Free") == 0, "free?: the question mark, never a bare Free");
    const reg::Item* word = nullptr;
    const reg::Item* noteItem = nullptr;
    const reg::Item* rule = nullptr;
    for (const reg::Item& i : l.items) {
      if (i.kind == reg::Item::Kind::Text && i.text == "Free?") word = &i;
      if (i.kind == reg::Item::Kind::Text && i.text == unreadDay.note) noteItem = &i;
      if (i.kind == reg::Item::Kind::Rule && !rule) rule = &i;  // the rule under the name
    }
    check(word && noteItem && noteItem->y > word->y && noteItem->y - (word->y + word->h) < 80, "free?: the note sits just under the state, in the same block");
    check(rule && noteItem->y < 400, "free?: and well above the lists' foot");
    // With an unreadDay note the note is not down by the footer too.
    check(std::count_if(l.items.begin(), l.items.end(), [&](const reg::Item& i) { return i.kind == reg::Item::Kind::Text && i.text == unreadDay.note; }) == 1, "free?: once");
    // A long note takes two lines in the block, cut after that.
    unreadDay.note = "1 repeating booking couldn't be read. Only part of a busy day is shown. The calendar could not be read in time.";
    maybe = reg::buildModel(unreadDay, reg::viewTime(unreadDay.fetchedAt), o, words);
    l = reg::layoutScreen(maybe, words, fonts);
    verify(l, sourcesOf(maybe, words), fonts, "free? with a long note");
    check(countRole(l, reg::FontRole::NoteBold) == 2 && std::any_of(l.items.begin(), l.items.end(), [](const reg::Item& i) { return i.role == reg::FontRole::NoteBold && endsWithEllipsis(i.text); }),
          "free? with a long note: two bold lines, the second cut");
    // In use keeps its note at the foot.
    reg::Schedule busyDay = dayWith(2, 1);
    busyDay.note = "1 repeating booking couldn't be read.";
    const reg::Model busyModel = reg::buildModel(busyDay, reg::viewTime(busyDay.fetchedAt), o, words);
    l = reg::layoutScreen(busyModel, words, fonts);
    verify(l, sourcesOf(busyModel, words), fonts, "in use with a note");
    check(countText(l, busyDay.note) == 1 && countRole(l, reg::FontRole::NoteBold) == 0, "in use: the note stays at the foot, in the small face");

    // Stale, and never read.
    busy.status = reg::Status::Stale;
    reg::Model stale = reg::buildModel(busy, reg::viewTime(busy.fetchedAt), o, words);
    l = reg::layoutScreen(stale, words, fonts);
    verify(l, sourcesOf(stale, words), fonts, "stale");
    check(countText(l, "Calendar unavailable since 8:05" + NB + "AM") == 1, "stale: the footer, whole on one line");
    check(countRole(l, reg::FontRole::NoteBold) == 1, "stale: and in bold");
    reg::Schedule never;
    never.instrument = "Confocal";
    never.timezone = "UTC";
    reg::Model unread = reg::buildModel(never, 0, o, words);
    l = reg::layoutScreen(unread, words, fonts);
    verify(l, sourcesOf(unread, words), fonts, "never read");
    check(countText(l, "Not known") == 1 && countText(l, "Waiting for the calendar.") == 1 && countText(l, "Calendar not read yet") == 1, "never read");
    check(countText(l, "TODAY") == 0, "never read: no lists");
    reg::Options low = o;
    low.batteryLow = true;
    reg::Model battery = reg::buildModel(busy, reg::viewTime(busy.fetchedAt), low, words);
    l = reg::layoutScreen(battery, words, fonts);
    verify(l, sourcesOf(battery, words), fonts, "battery low");
    check(countRole(l, reg::FontRole::NoteBold) == 2, "battery low: the sentence takes two lines");
    check(!endsWithEllipsis(l.items.back().text), "battery low: and is not cut");
  }

  // ---- long things -------------------------------------------------------------------------------------------------
  {
    reg::Options o;
    o.twelveHour = true;
    o.pollMinutes = 60;
    // The widest 40-character name: it takes two lines, and a third would be cut.
    reg::Schedule s = dayWith(2, 1, "B", repeat("W", 40));
    reg::Model m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    reg::Layout l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "40 W's");
    check(countRole(l, reg::FontRole::Name) == 2, "a 40-character name: two lines");
    check(std::any_of(l.items.begin(), l.items.end(), [](const reg::Item& i) { return i.role == reg::FontRole::Name && endsWithEllipsis(i.text); }), "the widest name is cut with an ellipsis");
    // A name of ordinary letters at 40 characters fits on two lines uncut.
    s = dayWith(2, 1, "B", "Bruker Avance III HD 600 MHz NMR spectro");
    m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "a long real name");
    check(countRole(l, reg::FontRole::Name) == 2 && !std::any_of(l.items.begin(), l.items.end(), [](const reg::Item& i) { return i.role == reg::FontRole::Name && endsWithEllipsis(i.text); }),
          "a long ordinary name: two lines, whole");

    // A 40-character title in use: who keeps a line, the time keeps the next.
    s = dayWith(2, 1, repeat("W", 40) + "-");
    s.today[0].title = repeat("W", 40);
    s.now.title = s.today[0].title;
    m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "a 40-W title in use");
    check(countText(l, "until 8:30 AM") == 1, "a long title: the time stays whole on its own line");
    check(hasPrefix(l, "WWW") && std::any_of(l.items.begin(), l.items.end(), [](const reg::Item& i) { return i.role == reg::FontRole::Detail && endsWithEllipsis(i.text); }), "a long title: cut with an ellipsis");
    // Titles in the lists are cut to the column.
    check(std::any_of(l.items.begin(), l.items.end(), [](const reg::Item& i) { return i.role == reg::FontRole::Row && endsWithEllipsis(i.text); }) ||
              std::any_of(l.items.begin(), l.items.end(), [](const reg::Item& i) { return i.role == reg::FontRole::RowBold && endsWithEllipsis(i.text); }),
          "long titles in the lists are cut too");

    // The longest footer: stale with a day named, and the battery sentence.
    s = dayWith(2, 1);
    s.status = reg::Status::Stale;
    m = reg::buildModel(s, at(2026, 10, 15, 9, 30), o, words);
    l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "stale since yesterday");
    check(m.footer == "Calendar unavailable since yesterday" + NB + "8:05" + NB + "AM", "(the footer under test)");

    // A note.
    s = dayWith(2, 1);
    s.note = "1 repeating booking couldn't be read";
    m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "a note");
    check(countText(l, s.note) == 1, "a note: shown whole");
    s.note = repeat("note ", 24).substr(0, 119);
    m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "a 119-character note");
    check(std::any_of(l.items.begin(), l.items.end(), [](const reg::Item& i) { return i.role == reg::FontRole::Note && endsWithEllipsis(i.text); }), "a note longer than two lines is cut, with an ellipsis, at the end of the second");

    // The Worker's own notes: one line, and two sentences on two lines, neither cut.
    s = dayWith(2, 1);
    s.note = "The calendar could not be read in time.";
    m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "the Worker's timeout note");
    check(countText(l, s.note) == 1, "the timeout note fits on one line");
    s.note = "1 repeating booking couldn't be read. Only part of a busy day is shown.";
    m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "two sentences in a note");
    check(countRole(l, reg::FontRole::Note) == 3 /* two lines of note and the footer */ && !std::any_of(l.items.begin(), l.items.end(), [](const reg::Item& i) { return i.role == reg::FontRole::Note && endsWithEllipsis(i.text); }),
          "two sentences take two lines, whole");

    // Next on another day.
    s = dayWith(0, 1);
    s.tomorrow[0].start = at(2026, 10, 15, 22);
    s.tomorrow[0].end = at(2026, 10, 16, 2);
    s.hasNext = true;
    s.next = s.tomorrow[0];
    m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "next overnight tomorrow");
    check(m.next.range == "tomorrow 10:00 PM\xe2\x80\x93" "Fri 2:00 AM", "(the range under test)");
  }

  // ---- long lists --------------------------------------------------------------------------------------------------
  {
    reg::Options o;
    o.twelveHour = true;
    o.pollMinutes = 60;
    for (int today : {0, 1, 2, 5, 8, 12, 20}) {
      for (int tomorrow : {0, 1, 3, 8, 20}) {
        const reg::Schedule s = dayWith(today, tomorrow);
        const reg::Model m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
        const reg::Layout l = reg::layoutScreen(m, words, fonts);
        const std::string name = "lists " + std::to_string(today) + "/" + std::to_string(tomorrow);
        verify(l, sourcesOf(m, words), fonts, name);
        check(l.todayShown + l.todayHidden == static_cast<int>(m.today.size()), name + ": every booking today is listed or counted");
        check(l.tomorrowShown + l.tomorrowHidden == static_cast<int>(m.tomorrow.size()), name + ": every booking tomorrow is listed or counted");
        if (m.today.size() >= 1) check(l.todayShown >= 1 || l.todayHidden >= 1, name + ": today is not lost");
        if (l.todayHidden > 0) check(hasPrefix(l, "+ " + std::to_string(l.todayHidden) + " more"), name + ": the 'more' line says how many");
        if (m.today.size() > 1 && m.tomorrow.size() > 1) check(l.tomorrowShown >= 1, name + ": tomorrow keeps a place when today is long");
      }
    }
    // The ordinary day shows everything.
    reg::Schedule s = dayWith(4, 3);
    reg::Model m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    reg::Layout l = reg::layoutScreen(m, words, fonts);
    check(l.todayHidden == 0 && l.tomorrowHidden == 0 && l.todayShown == 4 && l.tomorrowShown == 3, "an ordinary day: 4 today and 3 tomorrow, nothing hidden");
    // A full day cuts, and says so.
    s = dayWith(20, 20);
    m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    l = reg::layoutScreen(m, words, fonts);
    std::cout << "(20/20: today " << l.todayShown << " + " << l.todayHidden << " more, tomorrow " << l.tomorrowShown << " + "
              << l.tomorrowHidden << " more; bottom " << l.bottom << ")\n";
    check(l.todayHidden > 0 && l.tomorrowHidden > 0 && l.todayShown >= 3 && l.tomorrowShown >= 1, "20 and 20: both cut, each keeps rows");
    check(countText(l, "TODAY") == 1 && countText(l, "TOMORROW") == 1, "20 and 20: both headings");
    // With the room taken by a two-line name, a two-line state, a note and a two-line footer.
    s = dayWith(20, 20, "B", repeat("W", 40));
    s.today[0].title = repeat("W", 40);
    s.now.title = s.today[0].title;
    s.note = "1 repeating booking couldn't be read";
    s.status = reg::Status::Stale;
    m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
    l = reg::layoutScreen(m, words, fonts);
    verify(l, sourcesOf(m, words), fonts, "every line at its longest");
    std::cout << "(worst case: today " << l.todayShown << " + " << l.todayHidden << " more, tomorrow " << l.tomorrowShown << " + "
              << l.tomorrowHidden << " more; bottom " << l.bottom << ")\n";
  }

  // ---- 24-hour mode and 12-hour mode: the widest ranges ----------------------------------------------------------
  {
    for (const bool twelve : {true, false}) {
      reg::Options o;
      o.twelveHour = twelve;
      o.pollMinutes = 15;
      reg::Schedule s = dayWith(6, 4);
      s.today[1] = {"Wide", at(2026, 10, 14, 11, 59), at(2026, 10, 14, 12, 59)};
      s.today[2] = {"Wider", at(2026, 10, 14, 23, 0), at(2026, 10, 15, 0, 0)};
      s.tomorrow[0] = {"Overnight", at(2026, 10, 14, 22), at(2026, 10, 15, 2)};
      const reg::Model m = reg::buildModel(s, reg::viewTime(s.fetchedAt), o, words);
      const reg::Layout l = reg::layoutScreen(m, words, fonts);
      verify(l, sourcesOf(m, words), fonts, twelve ? "12-hour widest" : "24-hour widest");
    }
  }

  // ---- fuzz: whatever the model holds, the layout fits -------------------------------------------------------------
  {
    Rng rng;
    for (int round = 0; round < 3000; ++round) {
      reg::Model m;
      m.instrument = randomText(rng, 1, 40);
      m.state = static_cast<reg::State>(rng.upTo(4));
      m.stateWord = m.state == reg::State::InUse ? "In use" : m.state == reg::State::Unknown ? "Not known" : "Free";
      if (m.state == reg::State::InUse) {
        m.stateWho = randomText(rng, 1, 40);
        m.stateTail = rng.upTo(3) == 0 ? "all day" : "until tomorrow 12:00 AM";
      } else if (m.state == reg::State::FreeUntil) {
        m.stateTail = rng.upTo(2) ? "until Wed 12:00 AM" : "until 4:00 PM";
      } else if (m.state == reg::State::Unknown) {
        m.stateTail = rng.upTo(2) ? "Waiting for the calendar." : "The clock is not set.";
      }
      m.stateDetail = m.stateWho.empty() ? m.stateTail : m.stateWho + words.separator + m.stateTail;
      if ((m.state == reg::State::Free || m.state == reg::State::FreeUntil) && rng.upTo(2) == 0) {
        m.stateWord = "Free?";
        m.stateNote = randomText(rng, 1, 200);
      }
      m.hasNext = rng.upTo(2) == 0;
      if (m.hasNext) m.next = {randomRange(rng), randomText(rng, 1, 40), false};
      m.showLists = rng.upTo(8) != 0;
      m.showTomorrow = m.showLists && rng.upTo(4) != 0;
      const int nToday = m.showLists ? rng.upTo(21) : 0;
      const int nTomorrow = m.showTomorrow ? rng.upTo(21) : 0;
      for (int i = 0; i < nToday; ++i) m.today.push_back({randomRange(rng), randomText(rng, 1, 40), i == 0 && rng.upTo(2) == 0});
      for (int i = 0; i < nTomorrow; ++i) m.tomorrow.push_back({randomRange(rng), randomText(rng, 1, 40), false});
      m.todayEmpty = rng.upTo(2) ? "Nothing booked today." : "Nothing more today.";
      m.tomorrowEmpty = "Nothing booked tomorrow.";
      if (rng.upTo(3) == 0) m.note = randomText(rng, 1, 200);
      m.footerKind = static_cast<reg::FooterKind>(rng.upTo(4));
      const std::string when = rng.upTo(2) ? "yesterday 12:59 AM" : "2:05 PM";
      m.footer = m.footerKind == reg::FooterKind::AsOf         ? "As of " + when
                 : m.footerKind == reg::FooterKind::Unavailable ? "Calendar unavailable since " + when
                 : m.footerKind == reg::FooterKind::NotRead     ? "Calendar not read yet"
                                                                : "Battery low. This screen has stopped updating.";
      const reg::Layout l = reg::layoutScreen(m, words, fonts);
      const std::string name = "fuzz " + std::to_string(round);
      verify(l, sourcesOf(m, words), fonts, name, false);
      check(l.todayShown + l.todayHidden <= static_cast<int>(m.today.size()), name + ": counts add up (today)");
      check(l.tomorrowShown + l.tomorrowHidden <= static_cast<int>(m.tomorrow.size()), name + ": counts add up (tomorrow)");
      const reg::Layout again = reg::layoutScreen(m, words, fonts);
      check(again.items.size() == l.items.size() && again.bottom == l.bottom, name + ": the layout is deterministic");
    }
  }

  // ---- the full-day view ---------------------------------------------------------------------------------------
  {
    reg::Options o;
    o.twelveHour = true;
    o.pollMinutes = 60;
    const reg::Schedule s = dayWith(20, 20);
    const reg::FullDay day = reg::buildFullDay(s, reg::viewTime(s.fetchedAt), o, words);
    reg::Layout first = reg::layoutFullDay(day, words, fonts, 0);
    check(first.pages >= 2, "full day: 40 bookings take more than one page");
    int rowsSeen = 0;
    for (int page = 0; page < first.pages; ++page) {
      const reg::Layout l = reg::layoutFullDay(day, words, fonts, page);
      const std::string hints = std::string(page + 1 >= first.pages ? words.hintNextLast : words.hintNext) + words.separator + (page == 0 ? words.hintPrevFirst : words.hintPrev);
      const std::string pageText = "Page " + std::to_string(page + 1) + " of " + std::to_string(first.pages);
      std::vector<std::string> sources = {day.instrument, day.footer, pageText, hints};
      for (const reg::Day& d : day.days) {
        std::string up = d.heading;
        for (char& c : up)
          if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        sources.push_back(up);
        sources.push_back(d.empty);
        for (const reg::Row& r : d.rows) {
          sources.push_back(r.range);
          sources.push_back(r.title);
        }
      }
      verify(l, sources, fonts, "full day page " + std::to_string(page + 1));
      check(l.page == page && l.pages == first.pages, "full day: page numbers");
      for (const reg::Item& i : l.items)
        if (i.kind == reg::Item::Kind::Text && i.role == reg::FontRole::Row && i.text.rfind("B", 0) == 0) ++rowsSeen;
      // A heading is never the last thing in the body.
      const reg::Item* lastBody = nullptr;
      for (const reg::Item& i : l.items)
        if (i.kind == reg::Item::Kind::Text && i.y < 700 && (!lastBody || i.y > lastBody->y)) lastBody = &i;
      check(lastBody == nullptr || lastBody->role != reg::FontRole::Overline, "full day: no heading left at the foot of a page");
      check(countText(l, pageText) == 1, "full day: the page, in the header");
      check(countText(l, hints) == 1, "full day: the hint line, whole (not cut): " + hints);
      check(hints.find("Right") != std::string::npos && hints.find("Left") != std::string::npos, "the hints name the side buttons");
    }
    check(rowsSeen == 40, "full day: all 40 bookings appear, once");
    check(reg::layoutFullDay(day, words, fonts, 99).page == first.pages - 1 && reg::layoutFullDay(day, words, fonts, -3).page == 0, "full day: the page is clamped");
    const reg::Schedule small = dayWith(2, 1);
    const reg::FullDay little = reg::buildFullDay(small, reg::viewTime(small.fetchedAt), o, words);
    check(reg::layoutFullDay(little, words, fonts, 0).pages == 1, "full day: a short day is one page");
    reg::Schedule none = dayWith(0, 0);
    const reg::FullDay empty = reg::buildFullDay(none, reg::viewTime(none.fetchedAt), o, words);
    const reg::Layout el = reg::layoutFullDay(empty, words, fonts, 0);
    check(countText(el, "Nothing booked today.") == 1 && countText(el, "Nothing booked tomorrow.") == 1, "full day: nothing booked");
    reg::Schedule unread;
    unread.instrument = "X";
    unread.timezone = "UTC";
    const reg::FullDay nothing = reg::buildFullDay(unread, 0, o, words);
    const reg::Layout nl = reg::layoutFullDay(nothing, words, fonts, 0);
    check(nl.pages == 1 && countText(nl, "Calendar not read yet") == 1, "full day: never read");
  }

  // ---- moving about with two side buttons: one ring ------------------------------------------------------------------
  {
    using namespace reg::nav;
    for (int pages = 1; pages <= 6; ++pages) {
      // Right from the screen goes all the way round, and comes back to the screen.
      int position = SCREEN;
      std::vector<int> seen;
      for (int step = 0; step <= pages + 1; ++step) {
        position = next(position, pages);
        seen.push_back(position);
        if (position == SCREEN) break;
      }
      check(static_cast<int>(seen.size()) == pages + 1 && seen.back() == SCREEN, "Right: " + std::to_string(pages) + " pages and back to the screen");
      for (int i = 0; i + 1 < pages + 1 && i < pages; ++i) check(seen[static_cast<size_t>(i)] == i + 1, "Right visits the pages in order");
      // Left goes the other way round.
      position = SCREEN;
      seen.clear();
      for (int step = 0; step <= pages + 1; ++step) {
        position = resolved(previous(position, pages), pages);
        seen.push_back(position);
        if (position == SCREEN) break;
      }
      check(static_cast<int>(seen.size()) == pages + 1 && seen.front() == pages && seen.back() == SCREEN, "Left: from the screen to the last page, down to page 1, and the screen");
      // The two undo each other, from every place.
      for (int p = SCREEN; p <= pages; ++p) {
        check(previous(next(p, pages), pages) == p, "Left undoes Right from " + std::to_string(p) + " of " + std::to_string(pages));
        check(next(previous(p, pages), pages) == p, "Right undoes Left from " + std::to_string(p) + " of " + std::to_string(pages));
      }
    }
    // Before the count of pages is known.
    check(previous(SCREEN, 0) == LAST_PAGE && resolved(LAST_PAGE, 4) == 4 && resolved(LAST_PAGE, 0) == SCREEN, "Left from the screen asks for the last page before the count is known");
    check(next(LAST_PAGE, 4) == SCREEN && next(3, 4) == 4 && next(4, 4) == SCREEN && next(5, 4) == SCREEN, "Right at or past the last page is the screen");
    check(resolved(2, 4) == 2 && resolved(5, 4) == SCREEN && resolved(-1, 4) == SCREEN, "a position is kept inside the ring");
    check(next(1, 1) == SCREEN && previous(1, 1) == SCREEN, "one page: either button goes back to the screen");
  }

  // ---- every word the screen can say is in the font ----------------------------------------------------------------
  {
    const char* all[] = {words.free, words.inUse, words.notKnown, words.separator, words.waiting, words.noClock, words.allDay,
                         words.midnight, words.tomorrow, words.yesterday, words.nothingToday, words.nothingMoreToday,
                         words.nothingTomorrow, words.batteryLow, words.notRead, words.next, words.today, words.tomorrowHeading,
                         words.hintNext, words.hintNextLast, words.hintPrev, words.hintPrevFirst, "AM", "PM", "\xe2\x80\x93", ELL.c_str(), "0123456789:+"};
    for (const char* text : all) {
      const char* p = text;
      uint32_t cp;
      while ((cp = utf8NextCodepoint(reinterpret_cast<const unsigned char**>(&p)))) {
        check(fonts.covers(reg::FontRole::Row, cp) && fonts.covers(reg::FontRole::Note, cp) && fonts.covers(reg::FontRole::NoteBold, cp) &&
                  fonts.covers(reg::FontRole::Detail, cp),
              std::string("sans faces have U+") + std::to_string(cp));
      }
    }
    for (const char* text : {"In use", "Free", "Not known", "Centrifuge 5424", ELL.c_str(), "\xc2\xb7"}) {
      const char* p = text;
      uint32_t cp;
      while ((cp = utf8NextCodepoint(reinterpret_cast<const unsigned char**>(&p))))
        check(fonts.covers(reg::FontRole::Name, cp) && fonts.covers(reg::FontRole::State, cp), std::string("serif has U+") + std::to_string(cp));
    }
    const char* up = "NEXT TODAY TOMORROW";
    for (const char* p = up; *p; ++p) check(fonts.covers(reg::FontRole::Overline, static_cast<unsigned char>(*p)), "overline face has the capitals");
    std::cout << "(line heights: name " << fonts.lineHeight(reg::FontRole::Name) << ", detail " << fonts.lineHeight(reg::FontRole::Detail)
              << ", note " << fonts.lineHeight(reg::FontRole::Note) << ", overline " << fonts.lineHeight(reg::FontRole::Overline) << ")\n";
  }

  std::cout << "register_layout: " << checks << " checks passed\n";
  return 0;
}
