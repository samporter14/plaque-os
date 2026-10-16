#include "RegisterModel.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace reg {
namespace {
constexpr char EN_DASH[] = "\xe2\x80\x93";

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int year, const int month, const int day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yearOfEra = year - era * 400;
  const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + dayOfEra - 719468;
}

// The text with its spaces made non-breaking, so a time such as "yesterday 2:05 PM" stays on one line when a footer wraps.
std::string unbreakable(const std::string& text) {
  std::string out;
  for (const char c : text) {
    if (c == ' ') {
      out += "\xc2\xa0";
    } else {
      out += c;
    }
  }
  return out;
}

// snprintf with a format that comes from a translation file. Output is always terminated.
template <typename... Args>
std::string formatted(const char* format, Args... args) {
  char buffer[480];
  const int written = snprintf(buffer, sizeof(buffer), format, args...);
  if (written < 0) return {};
  return std::string(buffer);
}

bool sameDate(const Local& a, const Local& b) { return a.year == b.year && a.month == b.month && a.day == b.day; }

bool atMidnight(const int64_t epoch, const Zone* z) {
  Local parts;
  return localParts(epoch, parts, z) && parts.hour == 0 && parts.minute == 0;
}

// The end of a booking as a clock reading: "midnight" when it ends at local midnight.
std::string endClock(const int64_t end, const bool twelveHour, const Words& words, const Zone* z) {
  Local parts;
  if (!localParts(end, parts, z)) return {};
  if (parts.hour == 0 && parts.minute == 0) return words.midnight;
  return clockText(parts, twelveHour);
}

// "9:00–11:00 AM" (one suffix when both ends share it), "9:00 AM–1:00 PM", "09:00–11:00", "4:00 PM–midnight".
std::string compactRange(const int64_t start, const int64_t end, const bool twelveHour, const Words& words,
                         const Zone* z) {
  Local a, b;
  if (!localParts(start, a, z) || !localParts(end, b, z)) return {};
  const std::string tail = endClock(end, twelveHour, words, z);
  if (!twelveHour) return clockText(a, false) + EN_DASH + tail;
  const bool endsAtMidnight = b.hour == 0 && b.minute == 0;
  if (!endsAtMidnight && (a.hour >= 12) == (b.hour >= 12)) {
    const int ha = a.hour % 12 == 0 ? 12 : a.hour % 12;
    const int hb = b.hour % 12 == 0 ? 12 : b.hour % 12;
    char text[32];
    snprintf(text, sizeof(text), "%d:%02d%s%d:%02d %s", ha, a.minute, EN_DASH, hb, b.minute, b.hour < 12 ? "AM" : "PM");
    return text;
  }
  return clockText(a, true) + EN_DASH + tail;
}

// The calendar's all-day event, as the Worker marks it. A booking that merely runs from one midnight to another is
// drawn as the span of time it is.
bool isAllDay(const Entry& e) { return e.allDay; }
// When a booking stops mattering: a clamped one has no end the reader can use (its stand-in is a cut time, possibly in the
// past), so it runs on.
int64_t endOf(const Entry& e) { return e.clamped ? INT64_MAX / 4 : e.end; }
// An all-day booking of one day (23 to 25 hours across a clock change), against one of several.
bool oneDay(const Entry& e) { return e.end - e.start <= 25 * 3600; }

// "tomorrow", "Wed" or nothing, for a time on another date than `reference`.
std::string dayWord(const int64_t epoch, const int64_t reference, const Words& words, const Zone* z) {
  Local when, ref;
  if (!localParts(epoch, when, z) || !localParts(reference, ref, z) || sameDate(when, ref)) return {};
  const int diff = daysBetween(ref, when);
  if (diff == 1) return words.tomorrow;
  if (diff == -1) return words.yesterday;
  return words.weekdays[when.weekday];
}

// The end of a booking, for "until ...": a time, "tomorrow 9:00 AM", or at midnight "midnight" / "end of Wed".
std::string endText(const int64_t end, const int64_t at, const bool twelveHour, const Words& words, const Zone* z) {
  if (atMidnight(end, z)) {
    Local lastDay, ref;
    if (localParts(end - 1, lastDay, z) && localParts(at, ref, z)) {
      if (sameDate(lastDay, ref)) return words.midnight;
      return formatted(words.endOfDay, words.weekdays[lastDay.weekday]);
    }
  }
  return dayTimeText(end, at, twelveHour, words, z);
}

// How a booking reads on its own: for the Next line, which may be on another day.
std::string fullRange(const Entry& e, const int64_t at, const bool twelveHour, const Words& words, const Zone* z) {
  Local first, last;
  if (!localParts(e.start, first, z) || !localParts(e.end - 1, last, z)) return {};
  const std::string day = dayWord(e.start, at, words, z);
  if (e.clamped) return formatted(words.fromTime, dayTimeText(e.start, at, twelveHour, words, z).c_str());
  if (isAllDay(e)) {
    if (oneDay(e)) return day.empty() ? words.allDay : formatted(words.allDayOn, day.c_str());
    return std::string(words.weekdays[first.weekday]) + EN_DASH + words.weekdays[last.weekday];
  }
  if (sameDate(first, last)) {
    const std::string range = compactRange(e.start, e.end, twelveHour, words, z);
    return day.empty() ? range : day + " " + range;
  }
  return dayTimeText(e.start, at, twelveHour, words, z) + EN_DASH + endText(e.end, at, twelveHour, words, z);
}

// How a booking reads in a list for the day [dayStart, dayEnd): clipped to the day.
std::string listRange(const Entry& e, const int64_t dayStart, const int64_t dayEnd, const bool twelveHour,
                      const Words& words, const Zone* z) {
  if (e.clamped) {
    // No end to state: "From 4:00 PM", or "Until further notice" when it began before the day.
    if (e.start < dayStart) {
      std::string text = words.untilFurtherNotice;
      if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') text[0] = static_cast<char>(text[0] - 'a' + 'A');
      return text;
    }
    Local from;
    return localParts(e.start, from, z) ? formatted(words.fromTime, clockText(from, twelveHour).c_str()) : std::string();
  }
  if (isAllDay(e) || (e.start <= dayStart && e.end >= dayEnd)) return words.allDay;
  if (e.start < dayStart) return formatted(words.untilTime, endClock(e.end, twelveHour, words, z).c_str());
  if (e.end > dayEnd) {
    Local from;
    return localParts(e.start, from, z) ? formatted(words.fromTime, clockText(from, twelveHour).c_str()) : std::string();
  }
  return compactRange(e.start, e.end, twelveHour, words, z);
}

// Every booking the reply names, once each, in order of start.
std::vector<Entry> pool(const Schedule& s) {
  std::vector<Entry> all;
  all.reserve(s.today.size() + s.tomorrow.size() + 2);
  const auto add = [&all](const Entry& e) {
    if (std::find(all.begin(), all.end(), e) == all.end()) all.push_back(e);
  };
  if (s.hasNow) add(s.now);
  if (s.hasNext) add(s.next);
  for (const Entry& e : s.today) add(e);
  for (const Entry& e : s.tomorrow) add(e);
  std::stable_sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) {
    return a.start != b.start ? a.start < b.start : a.end < b.end;
  });
  return all;
}

void fnv(uint32_t& hash, const std::string& text) {
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  hash ^= 0xFFu;  // a separator no UTF-8 text contains, so "ab" + "c" differs from "a" + "bc"
  hash *= 16777619u;
}

void fnv(uint32_t& hash, const Row& row) {
  fnv(hash, row.range);
  fnv(hash, row.title);
  fnv(hash, row.current ? "1" : "0");
}
}  // namespace

const Zone* zoneOf(const Schedule& s) { return s.hasZone && !s.zone.spans.empty() ? &s.zone : nullptr; }

int64_t staleAfterSeconds(const int pollMinutes) {
  return std::max<int64_t>(MIN_STALE_S, (2 * static_cast<int64_t>(pollMinutes) + 5) * 60);
}

namespace {
// The offset in force at `epoch`: the last span that has begun, or the first one for anything before it.
int32_t offsetAt(const Zone& zone, const int64_t epoch) {
  int32_t offset = zone.spans.front().offset;
  for (const ZoneSpan& span : zone.spans) {
    if (span.from > epoch) break;
    offset = span.offset;
  }
  return offset;
}

int64_t floorDiv(const int64_t a, const int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

// The calendar date of a count of days since 1970-01-01 (Howard Hinnant's algorithm).
void civilFromDays(int64_t z, int& year, int& month, int& day) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<int>(yoe + era * 400 + (month <= 2));
}
}  // namespace

bool localParts(const int64_t epoch, Local& out, const Zone* zone) {
  if (epoch <= 0) return false;
  if (zone && !zone->spans.empty()) {
    const int64_t local = epoch + offsetAt(*zone, epoch);
    const int64_t days = floorDiv(local, 86400);
    const int64_t seconds = local - days * 86400;
    civilFromDays(days, out.year, out.month, out.day);
    out.hour = static_cast<int>(seconds / 3600);
    out.minute = static_cast<int>(seconds % 3600 / 60);
    out.weekday = static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
    out.valid = true;
    return true;
  }
  const time_t seconds = static_cast<time_t>(epoch);
  struct tm parts = {};
  if (!localtime_r(&seconds, &parts)) return false;
  out.year = parts.tm_year + 1900;
  out.month = parts.tm_mon + 1;
  out.day = parts.tm_mday;
  out.hour = parts.tm_hour;
  out.minute = parts.tm_min;
  out.weekday = parts.tm_wday;
  out.valid = true;
  return true;
}

int64_t localDayStart(const int64_t epoch, const int dayOffset, const Zone* zone) {
  if (epoch <= 0) return 0;
  if (zone && !zone->spans.empty()) {
    // The earliest moment whose local time has reached that day's 00:00. Within one span that is max(span start,
    // 00:00 less the span's offset); across spans the earliest wins, so a midnight that a clock change skips begins the
    // day at the change, and one that happens twice begins it the first time.
    const int64_t days = floorDiv(epoch + offsetAt(*zone, epoch), 86400) + dayOffset;
    const int64_t midnight = days * 86400;
    int64_t best = 0;
    const size_t count = zone->spans.size();
    for (size_t i = 0; i < count; ++i) {
      const bool first = i == 0;
      const bool last = i + 1 == count;
      const int64_t lo = first ? INT64_MIN : zone->spans[i].from;
      const int64_t hi = last ? INT64_MAX : zone->spans[i + 1].from;
      const int64_t t = std::max(lo, midnight - zone->spans[i].offset);
      if (t < hi && (best == 0 || t < best)) best = t;
    }
    return best > 0 ? best : 0;
  }
  const time_t seconds = static_cast<time_t>(epoch);
  struct tm parts = {};
  if (!localtime_r(&seconds, &parts)) return 0;
  parts.tm_hour = 0;
  parts.tm_min = 0;
  parts.tm_sec = 0;
  parts.tm_mday += dayOffset;
  parts.tm_isdst = -1;
  const time_t start = mktime(&parts);
  return start == static_cast<time_t>(-1) ? 0 : static_cast<int64_t>(start);
}

int daysBetween(const Local& from, const Local& to) {
  return static_cast<int>(daysFromCivil(to.year, to.month, to.day) - daysFromCivil(from.year, from.month, from.day));
}

std::string clockText(const Local& time, const bool twelveHour) {
  char buffer[16];
  if (!twelveHour) {
    snprintf(buffer, sizeof(buffer), "%02d:%02d", time.hour, time.minute);
  } else {
    const int hour = time.hour % 12 == 0 ? 12 : time.hour % 12;
    snprintf(buffer, sizeof(buffer), "%d:%02d %s", hour, time.minute, time.hour < 12 ? "AM" : "PM");
  }
  return buffer;
}

std::string dayTimeText(const int64_t epoch, const int64_t reference, const bool twelveHour, const Words& words,
                        const Zone* z) {
  Local when;
  if (!localParts(epoch, when, z)) return {};
  const std::string clock = clockText(when, twelveHour);
  const std::string day = dayWord(epoch, reference, words, z);
  return day.empty() ? clock : formatted(words.dayTime, day.c_str(), clock.c_str());
}

int64_t viewTime(const int64_t now) { return now > 0 ? now + VIEW_LEAD_S : 0; }

Model buildModel(const Schedule& s, const int64_t at, const Options& o, const Words& w) {
  const Zone* z = zoneOf(s);
  Model m;
  m.instrument = s.instrument;
  m.note = s.note;

  const bool neverRead = s.status == Status::Unavailable || s.fetchedAt <= 0;
  if (neverRead) {
    m.stateWord = w.notKnown;
    m.stateTail = w.waiting;
    m.stateDetail = m.stateTail;
    m.stale = true;
    m.footerKind = o.batteryLow ? FooterKind::BatteryLow : FooterKind::NotRead;
    m.footer = o.batteryLow ? w.batteryLow : w.notRead;
    return m;
  }

  // A clock that is not set, or that reads before the reply was made, cannot say what is true now. The lists are shown
  // as they were when the reply was made.
  const bool clockOk = at > 0 && at + CLOCK_SKEW_S >= s.fetchedAt;
  const int64_t t = clockOk ? std::max(at, s.fetchedAt) : s.fetchedAt;
  const int64_t todayStart = localDayStart(t, 0, z);
  const int64_t todayEnd = localDayStart(t, 1, z);
  const int64_t tomorrowEnd = localDayStart(t, 2, z);
  const int64_t coverEnd = localDayStart(s.fetchedAt, 2, z);
  const bool daysOk = todayStart > 0 && todayEnd > todayStart && tomorrowEnd > todayEnd && coverEnd > 0;
  // The reply covers the day it was made on and the next; past that it knows nothing.
  const bool beyond = !daysOk || t >= coverEnd;
  m.stale = s.status != Status::Ok || (clockOk && t > s.fetchedAt + staleAfterSeconds(o.pollMinutes)) || beyond;

  if (o.batteryLow) {
    m.footerKind = FooterKind::BatteryLow;
    m.footer = w.batteryLow;
  } else if (m.stale) {
    m.footerKind = FooterKind::Unavailable;
    m.footer = formatted(w.unavailableSince, unbreakable(dayTimeText(s.fetchedAt, t, o.twelveHour, w, z)).c_str());
  } else {
    m.footerKind = FooterKind::AsOf;
    m.footer = formatted(w.asOf, unbreakable(dayTimeText(s.fetchedAt, t, o.twelveHour, w, z)).c_str());
    m.asOf = s.fetchedAt;
  }

  if (beyond) {
    m.stateWord = w.notKnown;
    m.stateTail = w.waiting;
    m.stateDetail = m.stateTail;
    return m;
  }

  const std::vector<Entry> all = pool(s);
  const Entry* current = nullptr;
  const Entry* upcoming = nullptr;
  for (const Entry& e : all) {
    if (e.start <= t && t < endOf(e) && (!current || e.start < current->start)) current = &e;
    if (e.start > t && !upcoming) upcoming = &e;
  }

  if (!clockOk) {
    current = nullptr;
    m.stateWord = w.notKnown;
    m.stateTail = w.noClock;
  } else if (current) {
    m.state = State::InUse;
    m.stateWord = w.inUse;
    m.stateWho = current->title;
    if (current->clamped) {
      m.stateTail = w.untilFurtherNotice;
    } else if (isAllDay(*current) && oneDay(*current)) {
      m.stateTail = w.allDayTail;
    } else {
      m.stateTail = formatted(w.until, endText(current->end, t, o.twelveHour, w, z).c_str());
    }
  } else if (upcoming) {
    m.state = State::FreeUntil;
    m.stateWord = w.free;
    m.stateTail = formatted(w.until, dayTimeText(upcoming->start, t, o.twelveHour, w, z).c_str());
  } else {
    m.state = State::Free;
    m.stateWord = w.free;
  }

  m.stateDetail = m.stateWho.empty() ? m.stateTail : m.stateWho + w.separator + m.stateTail;
  // Nobody has it as far as the calendar could be read, and the Worker says it could not read all of it: a bare "Free"
  // would promise what is not known. The question mark, and the note beside the state.
  if (!m.note.empty() && (m.state == State::Free || m.state == State::FreeUntil)) {
    m.stateWord = w.freeMaybe;
    m.stateNote = std::move(m.note);
    m.note.clear();
  }

  if (upcoming) {
    m.hasNext = true;
    m.next.title = upcoming->title;
    m.next.range = fullRange(*upcoming, t, o.twelveHour, w, z);
  }

  m.showLists = true;
  m.showTomorrow = tomorrowEnd <= coverEnd;
  bool endedToday = false;
  for (const Entry& e : all) {
    if (endOf(e) > todayStart && e.start < todayEnd) {
      if (endOf(e) > t) {
        Row row;
        row.range = listRange(e, todayStart, todayEnd, o.twelveHour, w, z);
        row.title = e.title;
        row.current = clockOk && &e == current;
        m.today.push_back(std::move(row));
      } else {
        endedToday = true;
      }
    }
    if (m.showTomorrow && endOf(e) > todayEnd && e.start < tomorrowEnd) {
      Row row;
      row.range = listRange(e, todayEnd, tomorrowEnd, o.twelveHour, w, z);
      row.title = e.title;
      m.tomorrow.push_back(std::move(row));
    }
  }
  m.todayEmpty = endedToday ? w.nothingMoreToday : w.nothingToday;
  m.tomorrowEmpty = w.nothingTomorrow;
  return m;
}

FullDay buildFullDay(const Schedule& s, const int64_t at, const Options& o, const Words& w) {
  const Zone* z = zoneOf(s);
  FullDay full;
  const Model overview = buildModel(s, at, o, w);
  full.instrument = s.instrument;
  full.footer = overview.footer;
  full.stale = overview.stale;
  if (!overview.showLists) return full;

  const bool clockOk = at > 0 && at + CLOCK_SKEW_S >= s.fetchedAt;
  const int64_t t = clockOk ? std::max(at, s.fetchedAt) : s.fetchedAt;
  const int64_t starts[3] = {localDayStart(t, 0, z), localDayStart(t, 1, z), localDayStart(t, 2, z)};
  const std::vector<Entry> all = pool(s);
  for (int day = 0; day < (overview.showTomorrow ? 2 : 1); ++day) {
    Day d;
    Local date;
    localParts(starts[day], date, z);
    d.heading = formatted(w.heading, day == 0 ? w.today : w.tomorrowHeading, w.weekdays[date.weekday], date.day,
                          w.months[date.month - 1]);
    for (const Entry& e : all) {
      if (endOf(e) > starts[day] && e.start < starts[day + 1])
        d.rows.push_back({listRange(e, starts[day], starts[day + 1], o.twelveHour, w, z), e.title, false});
    }
    d.empty = day == 0 ? w.nothingToday : w.nothingTomorrow;
    full.days.push_back(std::move(d));
  }
  return full;
}

uint32_t contentSignature(const Model& m) {
  uint32_t hash = 2166136261u;
  fnv(hash, m.instrument);
  fnv(hash, std::to_string(static_cast<int>(m.state)));
  fnv(hash, m.stateWord);
  fnv(hash, m.stateWho);
  fnv(hash, m.stateTail);
  fnv(hash, m.stateNote);
  fnv(hash, m.hasNext ? "1" : "0");
  if (m.hasNext) fnv(hash, m.next);
  fnv(hash, m.showLists ? "1" : "0");
  fnv(hash, m.showTomorrow ? "1" : "0");
  for (const Row& row : m.today) fnv(hash, row);
  fnv(hash, m.todayEmpty);
  for (const Row& row : m.tomorrow) fnv(hash, row);
  fnv(hash, m.tomorrowEmpty);
  fnv(hash, m.note);
  fnv(hash, std::to_string(static_cast<int>(m.footerKind)));
  // A fresh footer's time is the one thing left out: Panel::asOf and FOOTER_REFRESH_S decide when it is worth a redraw.
  if (m.footerKind != FooterKind::AsOf) fnv(hash, m.footer);
  return hash == 0 ? 1 : hash;
}

Panel panelFor(const Model& model) {
  return {contentSignature(model), model.footerKind == FooterKind::AsOf ? model.asOf : 0};
}

Why redrawReason(const Panel& shown, const Model& model, const bool methodStepOnPanel) {
  if (methodStepOnPanel) return Why::MethodStep;
  if (shown.sig == 0) return Why::NotOurs;
  if (contentSignature(model) != shown.sig) return Why::Content;
  if (model.footerKind == FooterKind::AsOf && model.asOf >= shown.asOf + FOOTER_REFRESH_S) return Why::Footer;
  return Why::Same;
}

int64_t nextBoundary(const Schedule& s, const int64_t at, const Options& o) {
  const Zone* z = zoneOf(s);
  if (s.status == Status::Unavailable || s.fetchedAt <= 0 || at <= 0 || at + CLOCK_SKEW_S < s.fetchedAt) return 0;
  const int64_t t = std::max(at, s.fetchedAt);
  const int64_t coverEnd = localDayStart(s.fetchedAt, 2, z);
  if (coverEnd <= 0 || t >= coverEnd) return 0;  // already out of what the reply covers: nothing more to say
  int64_t soonest = 0;
  const auto consider = [&](const int64_t when) {
    if (when > t && (soonest == 0 || when < soonest)) soonest = when;
  };
  for (const Entry& e : pool(s)) {
    consider(e.start);
    if (!e.clamped) consider(e.end);  // a stand-in end is not a moment anything happens
  }
  consider(localDayStart(t, 1, z));
  if (s.status == Status::Ok) consider(s.fetchedAt + staleAfterSeconds(o.pollMinutes) + 1);
  return soonest;
}

}  // namespace reg
