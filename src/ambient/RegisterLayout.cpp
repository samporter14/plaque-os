#include "RegisterLayout.h"

#include <algorithm>
#include <cstdio>

namespace reg {
namespace {
constexpr int TOP = 40;
constexpr int RULE = 2;
constexpr int FRAME = 3;
constexpr int STATE_PAD = 12;
constexpr int GAP_NAME = 8;
constexpr int GAP_AFTER_RULE = 18;
constexpr int GAP_SECTION = 18;
constexpr int GAP_OVERLINE = 4;
constexpr int COL_GAP = 14;
constexpr int ROW_PAD = 4;
constexpr int MAX_RANGE_COL = 230;  // the time column of a list is never wider than this
constexpr int FOOTER_GAP = 8;
constexpr int NOTE_GAP = 6;

// UTF-8 sequence length from its lead byte (continuation bytes count as 1 so a bad string still advances).
size_t charLength(const unsigned char lead) {
  if (lead >= 0xF0) return 4;
  if (lead >= 0xE0) return 3;
  if (lead >= 0xC0) return 2;
  return 1;
}

// Byte offsets at which each character ends.
std::vector<size_t> charEnds(const std::string& text) {
  std::vector<size_t> ends;
  ends.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    i = std::min(text.size(), i + charLength(static_cast<unsigned char>(text[i])));
    ends.push_back(i);
  }
  return ends;
}

std::string trimEnd(std::string text) {
  while (!text.empty() && text.back() == ' ') text.pop_back();
  return text;
}

std::string upper(std::string text) {
  for (char& c : text)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return text;
}

template <typename... Args>
std::string formatted(const char* format, Args... args) {
  char buffer[160];
  const int written = snprintf(buffer, sizeof(buffer), format, args...);
  return written < 0 ? std::string() : std::string(buffer);
}

class Builder {
 public:
  Builder(const TextMeter& meter, Layout& out) : meter_(meter), out_(out) {}

  int lh(const FontRole role) const { return meter_.lineHeight(role); }

  // A line of text at (x, y), cut to maxWidth. Returns its height.
  int text(const FontRole role, const int x, const int y, const std::string& wanted, const int maxWidth) {
    Item item;
    item.kind = Item::Kind::Text;
    item.role = role;
    item.x = x;
    item.y = y;
    item.text = fitLine(meter_, role, wanted, maxWidth);
    if (item.text.empty()) return 0;
    item.w = meter_.width(role, item.text);
    item.h = meter_.lineHeight(role);
    out_.items.push_back(std::move(item));
    return meter_.lineHeight(role);
  }

  // Text that has already been fitted by wrapLines (its last line may carry the ellipsis).
  void lines(const FontRole role, const int x, int& y, const std::vector<std::string>& wrapped) {
    for (const std::string& line : wrapped) {
      Item item;
      item.role = role;
      item.x = x;
      item.y = y;
      item.text = line;
      item.w = meter_.width(role, line);
      item.h = meter_.lineHeight(role);
      y += item.h;
      out_.items.push_back(std::move(item));
    }
  }

  void rule(const int x, const int y, const int w) {
    Item item;
    item.kind = Item::Kind::Rule;
    item.x = x;
    item.y = y;
    item.w = w;
    item.h = RULE;
    item.thickness = RULE;
    out_.items.push_back(std::move(item));
  }

  void frame(const int x, const int y, const int w, const int h) {
    Item item;
    item.kind = Item::Kind::Frame;
    item.x = x;
    item.y = y;
    item.w = w;
    item.h = h;
    item.thickness = FRAME;
    out_.items.push_back(std::move(item));
  }

 private:
  const TextMeter& meter_;
  Layout& out_;
};

// One line of a list: the time, then the booking. The column the times share is `rangeCol` wide.
struct ListRow {
  std::string range;
  std::string title;
  bool bold = false;
};

int rangeColumn(const TextMeter& meter, const std::vector<ListRow>& rows) {
  int widest = 0;
  for (const ListRow& row : rows) widest = std::max(widest, meter.width(row.bold ? FontRole::RowBold : FontRole::Row, row.range));
  return std::min(widest, MAX_RANGE_COL);
}

// Places one row at y and returns its height.
int placeRow(Builder& b, const ListRow& row, const int y, const int rangeCol) {
  const FontRole role = row.bold ? FontRole::RowBold : FontRole::Row;
  const int height = b.lh(role) + ROW_PAD;
  b.text(role, MARGIN_X, y, row.range, rangeCol);
  b.text(role, MARGIN_X + rangeCol + COL_GAP, y, row.title, CONTENT_W - rangeCol - COL_GAP);
  return height;
}

struct Section {
  std::string heading;
  std::vector<ListRow> rows;  // what is listed
  std::string empty;
  int total = 0;              // bookings the day has
  int hidden = 0;             // how many a "+ N more" line stands for
};

// The room the lists have, in the heights the layout uses.
struct Room {
  int heading;  // an overline and the gap under it
  int slot;     // one row
  int between;  // the gap between two sections, beyond the slot's own padding
};

// How many row slots each section gets from `avail` pixels. A section with more rows than it is given shows one row
// fewer and a "+ N more" line in the slot that frees. Today comes first; Tomorrow (nTomorrow < 0 when it is not
// listed) is kept to a third of the room at least two rows deep, and dropped altogether only when the room cannot hold
// two rows of each. Returns {slotsToday, slotsTomorrow}; a section with 0 slots is not drawn at all.
struct Slots {
  int today = 0;
  int tomorrow = 0;
};
Slots shareOut(const Room& room, const int avail, const int nToday, const int nTomorrow) {
  const int needToday = std::max(1, nToday);
  Slots s;
  const int alone = (avail - room.heading) / room.slot;
  if (nTomorrow < 0) {
    s.today = std::max(0, std::min(needToday, alone));
    return s;
  }
  const int needTomorrow = std::max(1, nTomorrow);
  const int both = (avail - 2 * room.heading - room.between) / room.slot;
  if (needToday + needTomorrow <= both) {
    s.today = needToday;
    s.tomorrow = needTomorrow;
  } else if (both >= std::min(needToday, 2) + std::min(needTomorrow, 2)) {
    s.tomorrow = std::min(needTomorrow, std::max(std::min(needTomorrow, 2), both / 3));
    s.today = std::min(needToday, both - s.tomorrow);
    int spare = both - s.today - s.tomorrow;
    const int toToday = std::min(spare, needToday - s.today);
    s.today += toToday;
    spare -= toToday;
    s.tomorrow += std::min(spare, needTomorrow - s.tomorrow);
  } else {
    s.today = std::max(0, std::min(needToday, alone));
  }
  return s;
}

// Cuts a section to `slots` slots.
void fit(Section& s, const int slots) {
  s.total = static_cast<int>(s.rows.size());
  s.hidden = 0;
  if (s.total == 0 || s.total <= slots) return;
  const int keep = std::max(slots - 1, 0);
  s.hidden = s.total - keep;
  s.rows.resize(static_cast<size_t>(keep));
}

}  // namespace

std::string fitLine(const TextMeter& meter, const FontRole role, const std::string& text, const int maxWidth) {
  if (maxWidth <= 0 || text.empty()) return {};
  if (meter.width(role, text) <= maxWidth) return text;
  const std::vector<size_t> ends = charEnds(text);
  const auto candidate = [&](const size_t count) {
    return trimEnd(text.substr(0, count == 0 ? 0 : ends[count - 1])) + ELLIPSIS;
  };
  size_t low = 0;
  size_t high = ends.size();
  while (low < high) {
    const size_t mid = low + (high - low + 1) / 2;
    if (meter.width(role, candidate(mid)) <= maxWidth) {
      low = mid;
    } else {
      high = mid - 1;
    }
  }
  const std::string fitted = candidate(low);
  return meter.width(role, fitted) <= maxWidth ? fitted : std::string();
}

std::vector<std::string> wrapLines(const TextMeter& meter, const FontRole role, const std::string& text,
                                   const int maxWidth, const int maxLines) {
  std::vector<std::string> all;
  if (maxWidth <= 0 || maxLines <= 0) return all;
  std::string current;
  const auto flush = [&] {
    if (!current.empty()) all.push_back(current);
    current.clear();
  };
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t space = text.find(' ', pos);
    const std::string word = text.substr(pos, space == std::string::npos ? std::string::npos : space - pos);
    pos = space == std::string::npos ? text.size() : space + 1;
    if (word.empty()) continue;
    const std::string joined = current.empty() ? word : current + " " + word;
    if (meter.width(role, joined) <= maxWidth) {
      current = joined;
      continue;
    }
    flush();
    if (meter.width(role, word) <= maxWidth) {
      current = word;
      continue;
    }
    // A word wider than a line: break it between characters.
    const std::vector<size_t> ends = charEnds(word);
    size_t from = 0;
    size_t taken = 0;
    while (taken < ends.size()) {
      size_t count = taken + 1;
      while (count < ends.size() && meter.width(role, word.substr(from, ends[count] - from)) <= maxWidth) ++count;
      all.push_back(word.substr(from, ends[count - 1] - from));
      from = ends[count - 1];
      taken = count;
    }
    current = all.back();
    all.pop_back();
  }
  flush();
  if (static_cast<int>(all.size()) <= maxLines) return all;
  // Too long: keep the first lines and cut the rest into the last one. What is left over is wider than a line (each
  // line stopped where the next word would not fit), so fitLine cuts it and ends it with the ellipsis.
  std::string rest;
  for (size_t i = static_cast<size_t>(maxLines) - 1; i < all.size(); ++i) rest += (rest.empty() ? "" : " ") + all[i];
  all.resize(static_cast<size_t>(maxLines) - 1);
  all.push_back(fitLine(meter, role, rest, maxWidth));
  return all;
}

Layout layoutScreen(const Model& m, const Words& w, const TextMeter& meter) {
  Layout out;
  Builder b(meter, out);
  int y = TOP;

  // The footer and the note sit at the bottom, so what is left for the lists is known before they are placed.
  const FontRole footerRole = m.footerKind == FooterKind::AsOf ? FontRole::Note : FontRole::NoteBold;
  const std::vector<std::string> footerLines = wrapLines(meter, footerRole, m.footer, CONTENT_W, 2);
  const int footerHeight = RULE + FOOTER_GAP + static_cast<int>(footerLines.size()) * meter.lineHeight(footerRole);
  const int footerTop = PANEL_H - MARGIN_BOTTOM - footerHeight;
  // The Worker's note (what it could not read, an error) takes up to two lines, cut with an ellipsis after that.
  const std::vector<std::string> noteLines = wrapLines(meter, FontRole::Note, m.note, CONTENT_W, 2);
  const int noteHeight =
      noteLines.empty() ? 0 : static_cast<int>(noteLines.size()) * meter.lineHeight(FontRole::Note) + NOTE_GAP;
  const int noteTop = footerTop - noteHeight;

  // The name.
  const std::vector<std::string> nameLines = wrapLines(meter, FontRole::Name, m.instrument, CONTENT_W, 2);
  b.lines(FontRole::Name, MARGIN_X, y, nameLines);
  y += GAP_NAME;
  b.rule(MARGIN_X, y, CONTENT_W);
  y += RULE + GAP_AFTER_RULE;

  // The state: framed when in use. Free and Not known keep the same inner margins so the words line up.
  {
    const int innerX = MARGIN_X + FRAME + STATE_PAD;
    const int innerW = CONTENT_W - 2 * (FRAME + STATE_PAD);
    std::vector<std::string> detail;
    if (!m.stateTail.empty()) {
      const std::string whole = m.stateWho.empty() ? m.stateTail : m.stateWho + w.separator + m.stateTail;
      if (meter.width(FontRole::Detail, whole) <= innerW) {
        detail.push_back(whole);
      } else if (m.stateWho.empty()) {
        detail = wrapLines(meter, FontRole::Detail, m.stateTail, innerW, 2);
      } else {
        // The title may be long; the time is the part that matters, so it keeps its own line whole.
        detail.push_back(fitLine(meter, FontRole::Detail, m.stateWho, innerW));
        detail.push_back(fitLine(meter, FontRole::Detail, m.stateTail, innerW));
      }
    }
    // "Free?" carries the Worker's note with it, so the doubt is read where the state is.
    const int blockTop = y;
    int inner = blockTop + FRAME + STATE_PAD;
    b.text(FontRole::State, innerX, inner, m.stateWord, innerW);
    inner += meter.lineHeight(FontRole::State);
    b.lines(FontRole::Detail, innerX, inner, detail);
    if (!m.stateNote.empty()) {
      inner += GAP_OVERLINE;
      b.lines(FontRole::NoteBold, innerX, inner, wrapLines(meter, FontRole::NoteBold, m.stateNote, innerW, 2));
    }
    const int blockBottom = inner + STATE_PAD + FRAME;
    if (m.state == State::InUse) b.frame(MARGIN_X, blockTop, CONTENT_W, blockBottom - blockTop);
    y = blockBottom + GAP_SECTION;
  }

  // Next: on one line with its label when all of it fits, else the label over the title over the time.
  if (m.hasNext) {
    const std::string label = upper(w.next);
    const int labelW = meter.width(FontRole::Overline, label);
    const int rangeW = meter.width(FontRole::Row, m.next.range);
    const int titleW = meter.width(FontRole::RowBold, m.next.title);
    const int rowH = meter.lineHeight(FontRole::Row) + ROW_PAD;
    const int labelX = MARGIN_X;
    const int titleX = MARGIN_X + labelW + COL_GAP;
    if (labelW + COL_GAP + titleW + COL_GAP + rangeW <= CONTENT_W) {
      // The label sits on the row's baseline: its line box is shorter, so it starts lower.
      const int labelY = y + meter.lineHeight(FontRole::Row) - meter.lineHeight(FontRole::Overline) - 5;
      b.text(FontRole::Overline, labelX, labelY, label, CONTENT_W);
      b.text(FontRole::RowBold, titleX, y, m.next.title, CONTENT_W);
      b.text(FontRole::Row, titleX + titleW + COL_GAP, y, m.next.range, CONTENT_W);
      y += rowH;
    } else {
      b.text(FontRole::Overline, MARGIN_X, y, label, CONTENT_W);
      y += meter.lineHeight(FontRole::Overline) + GAP_OVERLINE;
      b.text(FontRole::RowBold, MARGIN_X, y, m.next.title, CONTENT_W);
      y += meter.lineHeight(FontRole::RowBold);
      b.text(FontRole::Row, MARGIN_X, y, m.next.range, CONTENT_W);
      y += rowH;
    }
    y += GAP_SECTION - ROW_PAD;
  }

  // The lists: whatever room is left, shared out.
  if (m.showLists) {
    Section today;
    today.heading = upper(w.today);
    today.empty = m.todayEmpty;
    for (const Row& r : m.today) today.rows.push_back({r.range, r.title, r.current});
    Section tomorrow;
    tomorrow.heading = upper(w.tomorrowHeading);
    tomorrow.empty = m.tomorrowEmpty;
    for (const Row& r : m.tomorrow) tomorrow.rows.push_back({r.range, r.title, false});

    const Room room{meter.lineHeight(FontRole::Overline) + GAP_OVERLINE, meter.lineHeight(FontRole::Row) + ROW_PAD,
                    GAP_SECTION - ROW_PAD};
    const int available = noteTop - GAP_SECTION - y;
    const Slots slots = shareOut(room, available, static_cast<int>(today.rows.size()),
                                 m.showTomorrow ? static_cast<int>(tomorrow.rows.size()) : -1);
    fit(today, slots.today);
    fit(tomorrow, slots.tomorrow);
    // Both days share one time column, so the bookings' names line up from Today into Tomorrow.
    std::vector<ListRow> listed;
    if (slots.today > 0) listed.insert(listed.end(), today.rows.begin(), today.rows.end());
    if (slots.tomorrow > 0) listed.insert(listed.end(), tomorrow.rows.begin(), tomorrow.rows.end());
    const int rangeCol = rangeColumn(meter, listed);

    const auto place = [&](const Section& s) {
      b.text(FontRole::Overline, MARGIN_X, y, s.heading, CONTENT_W);
      y += room.heading;
      if (s.total == 0) {
        b.text(FontRole::Note, MARGIN_X, y, s.empty, CONTENT_W);
        y += room.slot;
        return;
      }
      for (const ListRow& row : s.rows) y += placeRow(b, row, y, rangeCol);
      if (s.hidden > 0) {
        b.text(FontRole::Note, MARGIN_X, y, formatted(w.more, s.hidden), CONTENT_W);
        y += room.slot;
      }
    };
    if (slots.today > 0) {
      place(today);
      out.todayShown = static_cast<int>(today.rows.size());
      out.todayHidden = today.hidden;
    } else {
      out.todayHidden = today.hidden;
    }
    if (m.showTomorrow) {
      if (slots.tomorrow > 0) {
        y += room.between;
        place(tomorrow);
        out.tomorrowShown = static_cast<int>(tomorrow.rows.size());
        out.tomorrowHidden = tomorrow.hidden;
      } else {
        out.tomorrowHidden = tomorrow.hidden;
      }
    }
  }

  // The note, then the footer.
  int ny = noteTop;
  b.lines(FontRole::Note, MARGIN_X, ny, noteLines);
  b.rule(MARGIN_X, footerTop, CONTENT_W);
  int fy = footerTop + RULE + FOOTER_GAP;
  b.lines(footerRole, MARGIN_X, fy, footerLines);
  out.bottom = fy;
  return out;
}

Layout layoutFullDay(const FullDay& d, const Words& w, const TextMeter& meter, const int wantedPage) {
  Layout out;
  Builder b(meter, out);
  int y = TOP;

  // Header: the name in the face the list uses and, at the right, which page this is; a rule under them. Placed once the
  // pages are known (below).
  const int headerTop = y;
  y += meter.lineHeight(FontRole::RowBold) + GAP_NAME + RULE + GAP_AFTER_RULE;
  const int bodyTop = y;

  // Footer: how fresh, then the page and how to go back.
  const FontRole footerRole = d.stale ? FontRole::NoteBold : FontRole::Note;
  const int footerHeight = RULE + FOOTER_GAP + 2 * meter.lineHeight(FontRole::Note);
  const int footerTop = PANEL_H - MARGIN_BOTTOM - footerHeight;
  const int bodyBottom = footerTop - GAP_SECTION;

  // Flatten the days into blocks of lines, then deal them out to pages.
  struct Line {
    enum class Kind : uint8_t { Heading, Row, Empty } kind;
    int day;
    int row;
  };
  std::vector<ListRow> rows;
  std::vector<Line> lines;
  for (size_t day = 0; day < d.days.size(); ++day) {
    lines.push_back({Line::Kind::Heading, static_cast<int>(day), -1});
    if (d.days[day].rows.empty()) {
      lines.push_back({Line::Kind::Empty, static_cast<int>(day), -1});
      continue;
    }
    for (size_t r = 0; r < d.days[day].rows.size(); ++r) {
      rows.push_back({d.days[day].rows[r].range, d.days[day].rows[r].title, d.days[day].rows[r].current});
      lines.push_back({Line::Kind::Row, static_cast<int>(day), static_cast<int>(rows.size()) - 1});
    }
  }
  const int rangeCol = rangeColumn(meter, rows);
  // A heading carries the gap above it, so a day's rows do not run into the next day's heading.
  const int headingGap = GAP_SECTION - ROW_PAD;
  const auto height = [&](const Line& l) {
    return l.kind == Line::Kind::Heading ? headingGap + meter.lineHeight(FontRole::Overline) + GAP_OVERLINE
                                         : meter.lineHeight(FontRole::Row) + ROW_PAD;
  };
  std::vector<int> pageOf(lines.size(), 0);
  int page = 0;
  int used = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    // A heading is kept with the line after it.
    int need = height(lines[i]);
    if (lines[i].kind == Line::Kind::Heading && i + 1 < lines.size()) need += height(lines[i + 1]);
    if (used > 0 && used + need > bodyBottom - bodyTop) {
      ++page;
      used = 0;
    }
    pageOf[i] = page;
    used += height(lines[i]);
  }
  out.pages = lines.empty() ? 1 : page + 1;
  out.page = std::clamp(wantedPage, 0, out.pages - 1);

  y = bodyTop;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (pageOf[i] != out.page) continue;
    const Line& l = lines[i];
    if (l.kind == Line::Kind::Heading) {
      b.text(FontRole::Overline, MARGIN_X, y + headingGap, upper(d.days[static_cast<size_t>(l.day)].heading), CONTENT_W);
    } else if (l.kind == Line::Kind::Empty) {
      b.text(FontRole::Note, MARGIN_X, y, d.days[static_cast<size_t>(l.day)].empty, CONTENT_W);
    } else {
      placeRow(b, rows[static_cast<size_t>(l.row)], y, rangeCol);
    }
    y += height(l);
  }

  {
    const std::string pageText = formatted(w.pageOf, out.page + 1, out.pages);
    const int pageW = meter.width(FontRole::Note, pageText);
    b.text(FontRole::RowBold, MARGIN_X, headerTop, d.instrument, CONTENT_W - pageW - COL_GAP);
    b.text(FontRole::Note, MARGIN_X + CONTENT_W - pageW, headerTop + (meter.lineHeight(FontRole::RowBold) - meter.lineHeight(FontRole::Note)) / 2 + 2,
           pageText, pageW);
    b.rule(MARGIN_X, headerTop + meter.lineHeight(FontRole::RowBold) + GAP_NAME, CONTENT_W);
  }

  // The footer: how fresh, then what the keys do. The two side buttons and a tap go round the pages and the screen
  // itself in one ring: screen, page 1, page 2, ... the last page, screen.
  b.rule(MARGIN_X, footerTop, CONTENT_W);
  int fy = footerTop + RULE + FOOTER_GAP;
  b.text(footerRole, MARGIN_X, fy, d.footer, CONTENT_W);
  fy += meter.lineHeight(FontRole::Note);
  const bool lastPage = out.page + 1 >= out.pages;
  const bool firstPage = out.page == 0;
  b.text(FontRole::Note, MARGIN_X, fy, std::string(lastPage ? w.hintNextLast : w.hintNext) + w.separator + (firstPage ? w.hintPrevFirst : w.hintPrev), CONTENT_W);
  out.bottom = fy + meter.lineHeight(FontRole::Note);
  return out;
}

}  // namespace reg
