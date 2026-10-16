#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>

#include "RegisterModel.h"

namespace {
int checks = 0;
void check(bool condition, const std::string& name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}
void checkText(const std::string& got, const std::string& want, const std::string& name) {
  if (got != want) {
    std::cerr << "FAIL: " << name << "\n  got:  [" << got << "]\n  want: [" << want << "]\n";
    std::exit(1);
  }
  ++checks;
}

void zone(const char* posix) {
  setenv("TZ", posix, 1);
  tzset();
}
constexpr char NEW_YORK[] = "EST5EDT,M3.2.0,M11.1.0";

// A local time in the zone set above.
int64_t at(int year, int month, int day, int hour, int minute = 0, int second = 0) {
  struct tm parts = {};
  parts.tm_year = year - 1900;
  parts.tm_mon = month - 1;
  parts.tm_mday = day;
  parts.tm_hour = hour;
  parts.tm_min = minute;
  parts.tm_sec = second;
  parts.tm_isdst = -1;
  return static_cast<int64_t>(mktime(&parts));
}


// UTC calendar time to epoch, with no time zone involved (the zone tables below are built from it).
int64_t utc(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
  int y = year - (month <= 2);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (era * 146097 + doe - 719468) * 86400 + hour * 3600 + minute * 60 + second;
}

// The instrument zones the Worker describes: from before today through the end of the next day and a margin.
reg::Zone detroitZone() {
  reg::Zone z;
  z.name = "America/Detroit";
  z.spans = {{utc(2026, 1, 1), -18000}, {utc(2026, 3, 8, 7), -14400}, {utc(2026, 11, 1, 6), -18000}};
  return z;
}
reg::Zone kolkataZone() {
  reg::Zone z;
  z.name = "Asia/Kolkata";
  z.spans = {{utc(2026, 1, 1), 19800}};
  return z;
}
// Lord Howe Island: UTC+10:30, and +11:00 in summer (a 30-minute clock change): summer begins 2:00 on the first Sunday of
// October and ends 2:00 on the first Sunday of April.
reg::Zone lordHoweZone() {
  reg::Zone z;
  z.name = "Australia/Lord_Howe";
  z.spans = {{utc(2026, 1, 1), 39600}, {utc(2026, 4, 4, 15), 37800}, {utc(2026, 10, 3, 15, 30), 39600}};
  return z;
}
// A time on the instrument's wall clock.
int64_t atIn(const reg::Zone& z, int year, int month, int day, int hour, int minute = 0) {
  const int64_t naive = utc(year, month, day, hour, minute);
  int32_t offset = z.spans.front().offset;
  for (const reg::ZoneSpan& span : z.spans)
    if (span.from <= naive - span.offset) offset = span.offset;
  return naive - offset;
}

reg::Entry book(const char* title, int64_t start, int64_t end) { return {title, start, end, false}; }
// The calendar's all-day event, as the Worker marks it.
reg::Entry allDayBook(const char* title, int64_t start, int64_t end) { return {title, start, end, true}; }

// Wednesday 14 October 2026, in New York (EDT, UTC-4). The reply was made at 14:05.
struct Day {
  int64_t fetched = at(2026, 10, 14, 14, 5);
  reg::Entry ana = book("Ana", at(2026, 10, 14, 13), at(2026, 10, 14, 15));
  reg::Entry sam = book("Sam", at(2026, 10, 14, 16), at(2026, 10, 14, 18));
  reg::Entry lee = book("Lee", at(2026, 10, 15, 9), at(2026, 10, 15, 11));

  reg::Schedule schedule() const {
    reg::Schedule s;
    s.instrument = "Centrifuge 5424";
    s.timezone = "America/New_York";
    s.fetchedAt = fetched;
    s.status = reg::Status::Ok;
    s.hasNow = true;
    s.now = ana;
    s.hasNext = true;
    s.next = sam;
    s.today = {ana, sam};
    s.tomorrow = {lee};
    return s;
  }
};

reg::Options opts(bool twelve = true, int poll = 60) {
  reg::Options o;
  o.twelveHour = twelve;
  o.pollMinutes = poll;
  return o;
}

reg::Model model(const reg::Schedule& s, int64_t when, const reg::Options& o = opts()) {
  return reg::buildModel(s, when, o, reg::Words());
}

const std::string DASH = "\xe2\x80\x93";
const std::string DOT = "\xc2\xb7";
const std::string NB = "\xc2\xa0";  // a footer's time keeps its spaces together
}  // namespace

int main() {
  zone(NEW_YORK);
  const Day day;
  const reg::Schedule base = day.schedule();

  // ---- time formats ----------------------------------------------------------------------------------------------
  {
    reg::Local l;
    check(reg::localParts(at(2026, 10, 14, 15, 0), l) && l.hour == 15 && l.weekday == 3, "local parts: Wednesday 15:00");
    checkText(reg::clockText(l, true), "3:00 PM", "12-hour afternoon");
    checkText(reg::clockText(l, false), "15:00", "24-hour afternoon");
    reg::localParts(at(2026, 10, 14, 0, 5), l);
    checkText(reg::clockText(l, true), "12:05 AM", "12-hour just after midnight");
    checkText(reg::clockText(l, false), "00:05", "24-hour just after midnight");
    reg::localParts(at(2026, 10, 14, 12, 0), l);
    checkText(reg::clockText(l, true), "12:00 PM", "12-hour noon");
    reg::localParts(at(2026, 10, 14, 9, 7), l);
    checkText(reg::clockText(l, true), "9:07 AM", "12-hour has no leading zero");
    checkText(reg::clockText(l, false), "09:07", "24-hour has one");
    const int64_t ref = at(2026, 10, 14, 12);
    checkText(reg::dayTimeText(at(2026, 10, 14, 15), ref, true, reg::Words()), "3:00 PM", "same day: the time alone");
    checkText(reg::dayTimeText(at(2026, 10, 15, 9), ref, true, reg::Words()), "tomorrow 9:00 AM", "tomorrow");
    checkText(reg::dayTimeText(at(2026, 10, 13, 9), ref, true, reg::Words()), "yesterday 9:00 AM", "yesterday");
    checkText(reg::dayTimeText(at(2026, 10, 16, 9), ref, false, reg::Words()), "Fri 09:00", "later in the week");
    check(reg::dayTimeText(0, ref, true, reg::Words()).empty(), "no time, no text");
  }

  // ---- the state -------------------------------------------------------------------------------------------------
  {
    const reg::Model m = model(base, reg::viewTime(day.fetched));
    check(m.state == reg::State::InUse && m.stateWord == "In use", "14:05: in use");
    checkText(m.stateDetail, "Ana " + DOT + " until 3:00 PM", "14:05: who and until when");
    checkText(m.stateWho, "Ana", "14:05: who");
    checkText(m.stateTail, "until 3:00 PM", "14:05: until when");
    check(m.hasNext, "14:05: there is a next");
    checkText(m.next.title, "Sam", "next: who");
    checkText(m.next.range, "4:00" + DASH + "6:00 PM", "next: when, one suffix for both");
    check(m.showLists && m.showTomorrow, "14:05: both lists are known");
    check(m.today.size() == 2 && m.today[0].current && !m.today[1].current, "the booking in progress leads Today");
    checkText(m.today[0].range, "1:00" + DASH + "3:00 PM", "today row");
    check(m.tomorrow.size() == 1, "tomorrow has Lee");
    checkText(m.tomorrow[0].range, "9:00" + DASH + "11:00 AM", "tomorrow row");
    checkText(m.footer, "As of 2:05" + NB + "PM", "fresh footer");
    check(m.footerKind == reg::FooterKind::AsOf && m.asOf == day.fetched && !m.stale, "fresh");
    check(m.instrument == "Centrifuge 5424", "the name");
  }
  {
    // 24-hour mode.
    const reg::Model m = model(base, reg::viewTime(day.fetched), opts(false));
    checkText(m.stateDetail, "Ana " + DOT + " until 15:00", "24-hour detail");
    checkText(m.next.range, "16:00" + DASH + "18:00", "24-hour range");
    checkText(m.footer, "As of 14:05", "24-hour footer");
    checkText(m.today[0].range, "13:00" + DASH + "15:00", "24-hour list row");
  }
  {
    // Free until the next booking.
    const reg::Model m = model(base, at(2026, 10, 14, 15, 30));
    check(m.state == reg::State::FreeUntil && m.stateWord == "Free", "15:30: free");
    checkText(m.stateDetail, "until 4:00 PM", "15:30: until the next booking");
    check(m.today.size() == 1 && m.today[0].title == "Sam" && !m.today[0].current, "Ana has gone from Today");
    checkText(m.footer, "As of 2:05" + NB + "PM", "the reply is still fresh at 15:30 on a 60-minute poll");
  }
  {
    // After the last booking of the day, with one tomorrow.
    const reg::Model m = model(base, at(2026, 10, 14, 18, 30));
    check(m.state == reg::State::FreeUntil, "18:30: free until tomorrow");
    checkText(m.stateDetail, "until tomorrow 9:00 AM", "18:30: the next one is tomorrow");
    check(m.today.empty(), "nothing more today");
    checkText(m.todayEmpty, "Nothing more today.", "and it says so");
    checkText(m.next.range, "tomorrow 9:00" + DASH + "11:00 AM", "a next booking on another day names the day");
  }
  {
    // Nothing booked at all.
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today.clear();
    s.tomorrow.clear();
    const reg::Model m = model(s, reg::viewTime(day.fetched));
    check(m.state == reg::State::Free && m.stateWord == "Free" && m.stateDetail.empty(), "free, no end");
    check(!m.hasNext && m.today.empty() && m.tomorrow.empty(), "no next, no rows");
    checkText(m.todayEmpty, "Nothing booked today.", "nothing booked today");
    checkText(m.tomorrowEmpty, "Nothing booked tomorrow.", "nothing booked tomorrow");
    check(m.showLists && m.showTomorrow, "the lists are still shown, empty");
  }
  {
    // A booking that starts at the very second the screen looks at: it is in use, not free.
    const reg::Model m = model(base, day.sam.start);
    check(m.state == reg::State::InUse && m.stateDetail == "Sam " + DOT + " until 6:00 PM", "a start is in use");
    const reg::Model e = model(base, day.ana.end);
    check(e.state == reg::State::FreeUntil, "an end is free (end is exclusive)");
  }
  {
    // Overlapping bookings: the one that began first is shown.
    reg::Schedule s = base;
    s.today.push_back(book("Kim", at(2026, 10, 14, 14), at(2026, 10, 14, 14, 30)));
    const reg::Model m = model(s, at(2026, 10, 14, 14, 10));
    check(m.state == reg::State::InUse && m.stateDetail.rfind("Ana", 0) == 0, "overlap: the earlier start leads");
    check(m.today.size() == 3, "overlap: all are listed");
  }
  {
    // The reply names a booking twice (as now, and in today's list): one row.
    const reg::Model m = model(base, reg::viewTime(day.fetched));
    check(m.today.size() == 2, "a booking named in now and in today is listed once");
  }
  {
    // A title with a dot and a long one are carried as they are; the layout truncates.
    reg::Schedule s = base;
    s.today[0].title = "Dr. A. Very-Long-Surname's overnight run";
    s.now.title = s.today[0].title;
    const reg::Model m = model(s, reg::viewTime(day.fetched));
    check(m.today[0].title == s.today[0].title, "titles are not shortened here");
  }

  // ---- all day, midnight, overnight ------------------------------------------------------------------------------
  {
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today = {allDayBook("Maintenance", at(2026, 10, 14, 0), at(2026, 10, 15, 0))};
    s.tomorrow.clear();
    const reg::Model m = model(s, reg::viewTime(day.fetched));
    check(m.state == reg::State::InUse, "all day: in use");
    checkText(m.stateDetail, "Maintenance " + DOT + " all day", "all day: says so rather than 'until 12:00 AM'");
    checkText(m.today[0].range, "All day", "all day: the row");
    check(!m.hasNext, "all day: nothing after");
    const reg::Model after = model(s, at(2026, 10, 15, 0, 1));
    check(after.state == reg::State::Free, "all day: after it ends, free");
  }
  {
    // An all-day booking tomorrow shows as Next with a day word.
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today.clear();
    s.tomorrow = {allDayBook("Calibration", at(2026, 10, 15, 0), at(2026, 10, 16, 0))};
    const reg::Model m = model(s, reg::viewTime(day.fetched));
    check(m.state == reg::State::FreeUntil, "all day tomorrow: free until it");
    checkText(m.stateDetail, "until tomorrow 12:00 AM", "all day tomorrow: starts at midnight");
    checkText(m.next.range, "tomorrow all day", "all day tomorrow: Next says so");
    checkText(m.tomorrow[0].range, "All day", "all day tomorrow: the row");
  }
  {
    // Three days Monday to Wednesday, seen on Tuesday: until the end of Wednesday.
    reg::Schedule s = base;
    s.fetchedAt = at(2026, 10, 13, 10);
    s.hasNow = s.hasNext = false;
    s.today = {allDayBook("Shutdown", at(2026, 10, 12, 0), at(2026, 10, 15, 0))};
    s.tomorrow.clear();
    const reg::Model m = model(s, at(2026, 10, 13, 10, 1));
    checkText(m.stateDetail, "Shutdown " + DOT + " until end of Wed", "multi-day all day");
    checkText(m.today[0].range, "All day", "multi-day: covers the whole of today");
  }
  {
    // A timed booking that happens to run from one midnight to another is not an all-day event: only the flag says so.
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today = {book("Long run", at(2026, 10, 14, 0), at(2026, 10, 15, 0))};
    s.tomorrow.clear();
    const reg::Model m = model(s, reg::viewTime(day.fetched));
    checkText(m.stateDetail, "Long run " + DOT + " until midnight", "unflagged midnight to midnight: a span of time");
    checkText(m.today[0].range, "All day", "(its list row still covers the whole day)");
    s.today = {book("Long run", at(2026, 10, 14, 6), at(2026, 10, 15, 0))};
    checkText(model(s, reg::viewTime(day.fetched)).today[0].range, "6:00 AM" + DASH + "midnight", "and a shorter one is its hours");
  }
  {
    // A flagged all-day booking over two days: all day in both lists, and in use until the end of the second.
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today = {allDayBook("Maintenance", at(2026, 10, 14, 0), at(2026, 10, 16, 0))};
    s.tomorrow = s.today;
    const reg::Model m = model(s, reg::viewTime(day.fetched));
    checkText(m.stateDetail, "Maintenance " + DOT + " until end of Thu", "two all-day days: until the end of the second");
    check(m.today.size() == 1 && m.tomorrow.size() == 1 && m.today[0].range == "All day" && m.tomorrow[0].range == "All day", "all day in both lists");
    s.fetchedAt = at(2026, 10, 14, 0, 0, 30);
    const reg::Model before = model(s, at(2026, 10, 14, 0, 1));
    check(before.state == reg::State::InUse, "(and in use from its first minute)");
  }
  {
    // The reader's zone is not the instrument's: the flagged booking is still one day, and still all day.
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today = {allDayBook("Maintenance", at(2026, 10, 14, 0), at(2026, 10, 15, 0))};  // New York midnights
    s.tomorrow.clear();
    zone("UTC0");
    const reg::Model m = model(s, at(2026, 10, 14, 19));  // 15:00 in New York
    checkText(m.stateDetail, "Maintenance " + DOT + " all day", "another zone: flagged all-day is still all day");
    checkText(m.today[0].range, "All day", "another zone: its row");
    zone(NEW_YORK);
  }
  {
    // Ends at midnight tonight.
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today = {book("Night run", at(2026, 10, 14, 20), at(2026, 10, 15, 0))};
    s.tomorrow.clear();
    const reg::Model m = model(s, at(2026, 10, 14, 21));
    checkText(m.stateDetail, "Night run " + DOT + " until midnight", "until midnight");
    checkText(m.today[0].range, "8:00 PM" + DASH + "midnight", "a range that ends at midnight");
    const reg::Model m24 = model(s, at(2026, 10, 14, 21), opts(false));
    checkText(m24.today[0].range, "20:00" + DASH + "midnight", "24-hour, ending at midnight");
  }
  {
    // Overnight: 22:00 to 02:00. Today says From, Tomorrow says Until.
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today = {book("Overnight", at(2026, 10, 14, 22), at(2026, 10, 15, 2))};
    s.tomorrow = {s.today[0]};
    const reg::Model m = model(s, at(2026, 10, 14, 22, 30));
    checkText(m.stateDetail, "Overnight " + DOT + " until tomorrow 2:00 AM", "overnight: until tomorrow");
    checkText(m.today[0].range, "From 10:00 PM", "overnight: today's row");
    checkText(m.tomorrow[0].range, "Until 2:00 AM", "overnight: tomorrow's row");
    const reg::Model before = model(s, at(2026, 10, 14, 20));
    checkText(before.next.range, "10:00 PM" + DASH + "tomorrow 2:00 AM", "overnight as Next");
  }
  {
    // Ranges across noon and midnight in 12-hour mode.
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.tomorrow.clear();
    s.today = {book("a", at(2026, 10, 14, 11), at(2026, 10, 14, 13)), book("b", at(2026, 10, 14, 12), at(2026, 10, 14, 13)),
               book("c", at(2026, 10, 14, 0), at(2026, 10, 14, 1)), book("d", at(2026, 10, 14, 23), at(2026, 10, 15, 0)),
               book("e", at(2026, 10, 14, 9, 30), at(2026, 10, 14, 10, 15))};
    s.fetchedAt = at(2026, 10, 14, 0, 1);
    const reg::Model m = model(s, at(2026, 10, 14, 0, 2));
    // Order of start: c, e, a, b, d
    checkText(m.today[0].range, "12:00" + DASH + "1:00 AM", "midnight start, one suffix");
    checkText(m.today[1].range, "9:30" + DASH + "10:15 AM", "minutes");
    checkText(m.today[2].range, "11:00 AM" + DASH + "1:00 PM", "across noon: two suffixes");
    checkText(m.today[3].range, "12:00" + DASH + "1:00 PM", "from noon");
    checkText(m.today[4].range, "11:00 PM" + DASH + "midnight", "to midnight");
  }

  // ---- never read, stale, clock, coverage ------------------------------------------------------------------------
  {
    reg::Schedule s;
    s.instrument = "Confocal";
    s.timezone = "America/New_York";
    s.status = reg::Status::Unavailable;
    const reg::Model m = model(s, at(2026, 10, 14, 14));
    check(m.state == reg::State::Unknown && m.stateWord == "Not known", "never read: not known");
    checkText(m.stateDetail, "Waiting for the calendar.", "never read: the detail");
    check(!m.showLists && !m.hasNext, "never read: no lists, no next");
    checkText(m.footer, "Calendar not read yet", "never read: the footer");
    check(m.footerKind == reg::FooterKind::NotRead && m.stale, "never read: kind");
  }
  {
    // The Worker says stale.
    reg::Schedule s = base;
    s.status = reg::Status::Stale;
    const reg::Model m = model(s, reg::viewTime(day.fetched));
    check(m.stale && m.footerKind == reg::FooterKind::Unavailable, "stale from the Worker");
    checkText(m.footer, "Calendar unavailable since 2:05" + NB + "PM", "stale footer names the last good reading");
    check(m.state == reg::State::InUse, "stale: the state stays as it was");
    check(m.showLists, "stale: the lists stay");
  }
  {
    // Stale by age, on the reader's own clock. 15-minute poll: 35 minutes; 60-minute poll: 125.
    check(reg::staleAfterSeconds(5) == 1800 && reg::staleAfterSeconds(15) == 2100 && reg::staleAfterSeconds(60) == 7500 &&
              reg::staleAfterSeconds(1440) == 2885 * 60,
          "stale limits: at least 30 minutes, else twice the poll and 5");
    const int64_t limit = day.fetched + reg::staleAfterSeconds(15);
    check(!model(base, limit, opts(true, 15)).stale, "35 minutes on a 15-minute poll: still fresh");
    check(model(base, limit + 1, opts(true, 15)).stale, "one second more: stale");
    check(!model(base, limit + 1, opts(true, 60)).stale, "the same moment on a 60-minute poll: fresh");
    const reg::Model m = model(base, limit + 1, opts(true, 15));
    checkText(m.footer, "Calendar unavailable since 2:05" + NB + "PM", "stale by age: the footer");
  }
  {
    // Stale since yesterday: the day is named.
    reg::Schedule s = base;
    s.status = reg::Status::Stale;
    const reg::Model m = model(s, at(2026, 10, 15, 8));
    checkText(m.footer, "Calendar unavailable since yesterday" + NB + "2:05" + NB + "PM", "stale: the day is named");
  }
  {
    // The clock is not set, or reads before the reply was made.
    const reg::Model unset = model(base, 0);
    check(unset.state == reg::State::Unknown && unset.stateDetail == "The clock is not set.", "clock unset: not known");
    check(unset.showLists && unset.today.size() == 2 && !unset.today[0].current, "clock unset: lists as they were");
    check(unset.footerKind == reg::FooterKind::AsOf, "clock unset: the footer is the reading's time");
    const reg::Model behind = model(base, day.fetched - 301);
    check(behind.state == reg::State::Unknown, "clock behind the reply by over 5 minutes: not known");
    const reg::Model skewed = model(base, day.fetched - 300);
    check(skewed.state == reg::State::InUse, "within 5 minutes: fine");
  }
  {
    // Past what the reply covers: the end of the day after it was made.
    const int64_t edge = at(2026, 10, 16, 0);
    const reg::Model before = model(base, edge - 1);
    check(before.state != reg::State::Unknown, "just before the edge: known");
    const reg::Model m = model(base, edge);
    check(m.state == reg::State::Unknown && !m.showLists && !m.hasNext, "at the edge: not known, no lists");
    check(m.stale && m.footerKind == reg::FooterKind::Unavailable, "at the edge: stale");
  }
  {
    // The day after the reply's day: Tomorrow is not covered, so it is hidden rather than shown empty.
    const reg::Model m = model(base, at(2026, 10, 15, 0, 5));
    check(m.showLists && !m.showTomorrow, "the next day: Tomorrow is hidden");
    check(m.today.size() == 1 && m.today[0].title == "Lee", "the next day: Today is what was Tomorrow");
  }

  // ---- "Free" is not promised when the Worker could not read everything -------------------------------------------
  {
    const std::string unread = "1 repeating booking couldn't be read.";
    // The Worker's reply for a calendar with an unreadable repeat: status ok, nothing in progress, a note.
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today.clear();
    s.tomorrow.clear();
    s.note = unread;
    const reg::Model m = model(s, reg::viewTime(day.fetched));
    check(m.state == reg::State::Free, "unread repeats, nothing booked: still the Free state");
    checkText(m.stateWord, "Free?", "but never a bare Free: a question mark");
    checkText(m.stateNote, unread, "and the note beside the state");
    check(m.note.empty(), "(not again down by the footer)");
    check(m.footerKind == reg::FooterKind::AsOf, "the footer is the reading's, as ever");
    // Free until the next booking: same.
    reg::Schedule u = base;
    u.note = unread;
    const reg::Model until = model(u, at(2026, 10, 14, 15, 30));
    check(until.state == reg::State::FreeUntil, "(free until 4:00 PM)");
    checkText(until.stateWord, "Free?", "free until the next booking: a question mark too");
    checkText(until.stateTail, "until 4:00 PM", "with its time");
    checkText(until.stateNote, unread, "and the note");
    // What is known stays known: a booking in progress is in use, and the note stays at the foot of the screen.
    const reg::Model busy = model(u, reg::viewTime(day.fetched));
    check(busy.state == reg::State::InUse && busy.stateWord == "In use" && busy.stateNote.empty(), "in use is in use");
    checkText(busy.note, unread, "and the note stays where it was");
    // Not known stays not known; the note stays at the foot.
    const reg::Model unknown = model(u, 0);
    check(unknown.state == reg::State::Unknown && unknown.stateWord == "Not known" && unknown.stateNote.empty() && unknown.note == unread,
          "no clock: not known, note at the foot");
    // The clock Register uses (RegisterProvider.cpp registerNow()): the system clock counts only when a sync set it this
    // boot or the retained clock is trusted. After a flash or a power loss it is a restored floor, and a booking that
    // ended since must not read "In use".
    {
      const int64_t restoredFloor = reg::viewTime(day.fetched) + 3600;  // what the system clock reads, untrusted
      check(reg::trustedReading(restoredFloor, false, false) == 0, "a restored floor and nothing vouching: no time");
      check(reg::trustedReading(restoredFloor, true, false) == restoredFloor, "a sync this boot vouches");
      check(reg::trustedReading(restoredFloor, false, true) == restoredFloor, "a trusted retained clock vouches");
      check(reg::trustedReading(0, true, true) == 0 && reg::trustedReading(-5, true, true) == 0, "no system time: none");
      const reg::Model afterFlash = model(u, reg::viewTime(reg::trustedReading(restoredFloor, false, false)));
      check(afterFlash.state == reg::State::Unknown && afterFlash.stateWord == "Not known",
            "with no trusted clock a booking in progress at the reply is not known, not \"In use\"");
      const reg::Model trusted = model(u, reg::viewTime(reg::trustedReading(reg::viewTime(day.fetched), true, false)));
      check(trusted.state == reg::State::InUse, "and with a sync this boot it is");
    }
    // Any note counts, whatever the sentence.
    u.note = "Only part of a busy day is shown.";
    checkText(model(u, at(2026, 10, 14, 15, 30)).stateWord, "Free?", "a cut day: a question mark");
    u.note = "The calendar could not be read in time.";
    checkText(model(u, at(2026, 10, 14, 15, 30)).stateWord, "Free?", "a timeout: a question mark");
    // No note, no question mark.
    u.note.clear();
    checkText(model(u, at(2026, 10, 14, 15, 30)).stateWord, "Free", "no note: Free");
    // The signature sees it.
    reg::Schedule w = base;
    w.hasNow = w.hasNext = false;
    w.today.clear();
    w.tomorrow.clear();
    const reg::Model plain = model(w, reg::viewTime(day.fetched));
    w.note = unread;
    check(reg::contentSignature(plain) != reg::contentSignature(model(w, reg::viewTime(day.fetched))), "signature: Free against Free?");
    reg::Model r = model(w, reg::viewTime(day.fetched));
    r.stateNote = "another";
    check(reg::contentSignature(r) != reg::contentSignature(model(w, reg::viewTime(day.fetched))), "signature: the state's note");
    // And the panel is redrawn when the note arrives.
    const reg::Panel shown = reg::panelFor(plain);
    check(reg::redrawReason(shown, model(w, reg::viewTime(day.fetched)), false) == reg::Why::Content, "a note appearing redraws");
  }

  // ---- the instrument's own zone -----------------------------------------------------------------------------------
  {
    // The zone arithmetic against the C library, for the same rules, across a whole year in quarter-hours.
    struct Case {
      reg::Zone zone;
      const char* posix;
    };
    const Case cases[] = {{detroitZone(), "EST5EDT,M3.2.0,M11.1.0"},
                          {kolkataZone(), "IST-5:30"},
                          {lordHoweZone(), "LHST-10:30LHDT-11,M10.1.0,M4.1.0"}};
    for (const Case& c : cases) {
      int64_t mismatches = 0;
      int64_t samples = 0;
      for (int64_t t = utc(2026, 1, 2); t < utc(2026, 12, 30); t += 900) {
        zone("UTC0");
        reg::Local mine;
        reg::localParts(t, mine, &c.zone);
        int64_t dayStarts[4];
        for (int k = -1; k <= 2; ++k) dayStarts[k + 1] = reg::localDayStart(t, k, &c.zone);
        zone(c.posix);
        reg::Local libc;
        reg::localParts(t, libc);
        bool same = mine.year == libc.year && mine.month == libc.month && mine.day == libc.day && mine.hour == libc.hour &&
                    mine.minute == libc.minute && mine.weekday == libc.weekday;
        for (int k = -1; k <= 2; ++k) same = same && dayStarts[k + 1] == reg::localDayStart(t, k);
        if (!same) {
          if (mismatches < 3) std::cerr << "zone " << c.zone.name << " differs from libc at " << t << "\n";
          ++mismatches;
        }
        ++samples;
      }
      check(mismatches == 0 && samples > 30000, std::string(c.zone.name) + ": parts and day starts match the C library all year");
    }
    zone(NEW_YORK);
    // A day's length on the instrument's clock.
    const reg::Zone detroit = detroitZone();
    const reg::Zone lordHowe = lordHoweZone();
    check(reg::localDayStart(atIn(detroit, 2026, 3, 8, 12), 1, &detroit) - reg::localDayStart(atIn(detroit, 2026, 3, 8, 12), 0, &detroit) == 23 * 3600, "Detroit: the spring day is 23 hours");
    check(reg::localDayStart(atIn(detroit, 2026, 11, 1, 12), 1, &detroit) - reg::localDayStart(atIn(detroit, 2026, 11, 1, 12), 0, &detroit) == 25 * 3600, "Detroit: the autumn day is 25 hours");
    const int64_t lhStart = atIn(lordHowe, 2026, 10, 4, 12);
    check(reg::localDayStart(lhStart, 1, &lordHowe) - reg::localDayStart(lhStart, 0, &lordHowe) == 23 * 3600 + 1800, "Lord Howe: the day summer begins is 23 hours 30 minutes");
    const int64_t lhEnd = atIn(lordHowe, 2026, 4, 5, 12);
    check(reg::localDayStart(lhEnd, 1, &lordHowe) - reg::localDayStart(lhEnd, 0, &lordHowe) == 24 * 3600 + 1800, "Lord Howe: the day summer ends is 24 hours 30 minutes");
    reg::Local l;
    reg::localParts(atIn(lordHowe, 2026, 10, 4, 3, 15), l, &lordHowe);
    check(l.hour == 3 && l.minute == 15 && l.day == 4 && l.weekday == 0, "Lord Howe: 03:15 on the Sunday summer begins, a Sunday");
    check(!reg::localParts(0, l, &lordHowe) && reg::localDayStart(0, 0, &lordHowe) == 0, "no time, no zone answer");
    const reg::Zone far;  // no spans: falls back to the reader's zone
    check(reg::localParts(utc(2026, 10, 14, 12), l, &far) && l.hour == 8, "a zone with no spans is the reader's own");
  }
  {
    // A reader set to UTC (a fresh one) showing a Detroit instrument: everything is on the instrument's clock.
    reg::Schedule detroit = base;  // built in New York: the same offsets as Detroit
    zone(NEW_YORK);
    const reg::Model reference = model(detroit, reg::viewTime(day.fetched));
    const reg::Model referenceLater = model(detroit, at(2026, 10, 14, 15, 30));
    const reg::Model referenceNight = model(detroit, at(2026, 10, 14, 18, 30));
    const reg::FullDay referenceDay = reg::buildFullDay(detroit, at(2026, 10, 14, 17), opts(), reg::Words());
    const int64_t boundaryReference = reg::nextBoundary(detroit, at(2026, 10, 14, 18, 30), opts(true, 60));
    const int64_t t1530 = at(2026, 10, 14, 15, 30), t1830 = at(2026, 10, 14, 18, 30), t1700 = at(2026, 10, 14, 17);  // Detroit's clock
    detroit.hasZone = true;
    detroit.zone = detroitZone();
    for (const char* tz : {"UTC0", "CST-8", "PST8PDT,M3.2.0,M11.1.0"}) {
      zone(tz);
      const std::string where = std::string("reader on ") + tz + ": ";
      const auto same = [&](const reg::Model& got, const reg::Model& want, const std::string& what) {
        check(got.stateWord == want.stateWord && got.stateDetail == want.stateDetail && got.footer == want.footer &&
                  got.next.range == want.next.range && got.next.title == want.next.title && got.today.size() == want.today.size() &&
                  got.tomorrow.size() == want.tomorrow.size() && got.showTomorrow == want.showTomorrow,
              where + what);
        for (size_t i = 0; i < want.today.size() && i < got.today.size(); ++i) check(got.today[i].range == want.today[i].range, where + what + " (today rows)");
        for (size_t i = 0; i < want.tomorrow.size() && i < got.tomorrow.size(); ++i) check(got.tomorrow[i].range == want.tomorrow[i].range, where + what + " (tomorrow rows)");
      };
      same(model(detroit, reg::viewTime(day.fetched)), reference, "in use, as Detroit's clock says");
      checkText(model(detroit, reg::viewTime(day.fetched)).stateDetail, "Ana " + DOT + " until 3:00 PM", where + "the time is Detroit's");
      same(model(detroit, t1530), referenceLater, "free until 4:00 PM");
      same(model(detroit, t1830), referenceNight, "tomorrow's booking is tomorrow");
      checkText(model(detroit, t1830).stateDetail, "until tomorrow 9:00 AM", where + "a day word on the instrument's calendar");
      const reg::FullDay got = reg::buildFullDay(detroit, t1700, opts(), reg::Words());
      check(got.days.size() == referenceDay.days.size() && got.days[0].heading == referenceDay.days[0].heading && got.days[1].heading == referenceDay.days[1].heading && got.days[0].rows.size() == 2,
            where + "the full day's headings and rows are Detroit's");
      check(reg::nextBoundary(detroit, t1830, opts(true, 60)) == boundaryReference, where + "the next boundary is Detroit's midnight, not the reader's");
    }
    zone(NEW_YORK);
    // The reader's own zone is used for a reply with no zone: the same schedule, no `zone`, on a UTC reader, is wrong.
    reg::Schedule old = base;
    zone("UTC0");
    const reg::Model wrong = model(old, utc(2026, 10, 14, 19, 0));  // 15:00 in Detroit
    check(wrong.stateDetail != "until 4:00 PM", "without a zone a UTC reader draws UTC (the fallback)");
    zone(NEW_YORK);
    // The midnight rollover and the covered-until edge follow the instrument too.
    detroit.hasZone = true;
    zone("UTC0");
    const int64_t detroitMidnight = atIn(detroitZone(), 2026, 10, 15, 0);
    check(reg::nextBoundary(detroit, detroitMidnight - 3600, opts(true, 60)) <= detroitMidnight, "a boundary at or before Detroit's midnight");
    const reg::Model before = model(detroit, detroitMidnight - 1);
    const reg::Model after = model(detroit, detroitMidnight + 5);
    check(before.showTomorrow && !after.showTomorrow, "tomorrow is hidden once it is Detroit's next day (the reply no longer reaches it)");
    check(after.today.size() == 1 && after.today[0].title == "Lee", "and Today is what Tomorrow was");
    const int64_t edge = atIn(detroitZone(), 2026, 10, 16, 0);
    check(model(detroit, edge - 1).state != reg::State::Unknown && model(detroit, edge).state == reg::State::Unknown, "the reply runs out at the end of Detroit's tomorrow");
    zone(NEW_YORK);
  }
  {
    // Kolkata (+5:30): a half-hour zone, on a reader in New York.
    const reg::Zone kolkata = kolkataZone();
    reg::Schedule s;
    s.instrument = "NMR";
    s.timezone = "Asia/Kolkata";
    s.status = reg::Status::Ok;
    s.hasZone = true;
    s.zone = kolkata;
    s.fetchedAt = atIn(kolkata, 2026, 10, 14, 14, 5);
    s.today = {book("Ana", atIn(kolkata, 2026, 10, 14, 13, 30), atIn(kolkata, 2026, 10, 14, 15, 15)), book("Sam", atIn(kolkata, 2026, 10, 14, 16), atIn(kolkata, 2026, 10, 14, 18))};
    s.tomorrow = {book("Lee", atIn(kolkata, 2026, 10, 15, 9, 45), atIn(kolkata, 2026, 10, 15, 11))};
    s.hasNow = true;
    s.now = s.today[0];
    s.hasNext = true;
    s.next = s.today[1];
    for (const char* tz : {NEW_YORK, "UTC0"}) {
      zone(tz);
      const reg::Model m = model(s, atIn(kolkata, 2026, 10, 14, 14, 10));
      checkText(m.stateDetail, "Ana " + DOT + " until 3:15 PM", std::string("Kolkata, reader on ") + tz + ": until 3:15 PM");
      checkText(m.today[0].range, "1:30" + DASH + "3:15 PM", "Kolkata: minutes of a half-hour zone");
      checkText(m.tomorrow[0].range, "9:45" + DASH + "11:00 AM", "Kolkata: tomorrow");
      checkText(m.footer, "As of 2:05" + NB + "PM", "Kolkata: footer");
      const reg::Model m24 = model(s, atIn(kolkata, 2026, 10, 14, 14, 10), opts(false));
      checkText(m24.stateDetail, "Ana " + DOT + " until 15:15", "Kolkata, 24 hour");
      check(reg::nextBoundary(s, atIn(kolkata, 2026, 10, 14, 17, 0), opts(true, 60)) == s.today[1].end, "Kolkata: the next boundary is Sam's end");
      check(reg::nextBoundary(s, s.today[1].end, opts(true, 60)) == atIn(kolkata, 2026, 10, 15, 0), "Kolkata: then local midnight");
    }
    zone(NEW_YORK);
  }
  {
    // Lord Howe: summer begins at 02:00 on Sunday 4 October 2026, and the clock jumps half an hour.
    const reg::Zone lh = lordHoweZone();
    reg::Schedule s;
    s.instrument = "Island";
    s.timezone = "Australia/Lord_Howe";
    s.status = reg::Status::Ok;
    s.hasZone = true;
    s.zone = lh;
    s.fetchedAt = atIn(lh, 2026, 10, 3, 20);
    s.today = {book("Late", atIn(lh, 2026, 10, 3, 22), atIn(lh, 2026, 10, 4, 4))};  // across the change
    s.tomorrow = {book("Sunday", atIn(lh, 2026, 10, 4, 9), atIn(lh, 2026, 10, 4, 10, 30))};
    s.hasNext = true;
    s.next = s.today[0];
    for (const char* tz : {"UTC0", NEW_YORK}) {
      zone(tz);
      const reg::Model m = model(s, atIn(lh, 2026, 10, 3, 20, 5));
      checkText(m.stateWord, "Free", std::string("Lord Howe, reader on ") + tz);
      checkText(m.stateTail, "until 10:00 PM", "Lord Howe: until 10:00 PM");
      checkText(m.tomorrow[0].range, "Until 4:00 AM", "Lord Howe: the late booking carries into Sunday");
      checkText(m.tomorrow[1].range, "9:00" + DASH + "10:30 AM", "Lord Howe: Sunday's booking");
      checkText(m.next.range, "10:00 PM" + DASH + "tomorrow 4:00 AM", "Lord Howe: across the half-hour change");
      check(s.today[0].end - s.today[0].start == 5 * 3600 + 1800, "(the booking is 5 hours 30 minutes of real time)");
      const reg::Model inUse = model(s, atIn(lh, 2026, 10, 4, 1, 45));
      checkText(inUse.stateDetail, "Late " + DOT + " until 4:00 AM", "Lord Howe: in use after midnight, until 4:00 the same morning");
      // The wall clock jumps from 01:59:59 to 02:30:00.
      reg::Local l;
      reg::localParts(atIn(lh, 2026, 10, 4, 3), l, &lh);
      check(l.hour == 3 && l.minute == 0, "Lord Howe: 03:00 summer time");
      reg::localParts(utc(2026, 10, 3, 15, 29, 59), l, &lh);
      check(l.hour == 1 && l.minute == 59, "Lord Howe: 01:59 just before the change");
      reg::localParts(utc(2026, 10, 3, 15, 30), l, &lh);
      check(l.hour == 2 && l.minute == 30, "Lord Howe: 02:30 the moment after");
    }
    zone(NEW_YORK);
  }
  {
    // A clamped booking: no end to state.
    reg::Schedule s = base;
    s.hasNow = true;
    s.now = day.ana;
    s.now.clamped = true;
    s.now.end = day.fetched + 400LL * 86400;
    s.today = {s.now, day.sam};
    const reg::Model m = model(s, reg::viewTime(day.fetched));
    check(m.state == reg::State::InUse, "a clamped booking is in use");
    checkText(m.stateDetail, "Ana " + DOT + " until further notice", "no end time for a clamped booking");
    checkText(m.today[0].range, "From 1:00 PM", "its row says when it began");
    s.now.start = at(2026, 10, 13, 9);
    s.today[0] = s.now;
    checkText(model(s, reg::viewTime(day.fetched)).today[0].range, "Until further notice", "one that began on an earlier day");
    // The Worker's cut times (2020-01-01 to 2021-02-04) are long past: the flag, not the times, decides.
    reg::Schedule old = base;
    old.hasNow = true;
    old.now = {"Out of service", utc(2020, 1, 1), utc(2021, 2, 4), false, true};
    old.today = {old.now, day.sam};
    const reg::Model om = model(old, reg::viewTime(day.fetched));
    check(om.state == reg::State::InUse, "a clamped booking with an end in the past is still in use");
    checkText(om.stateDetail, "Out of service " + DOT + " until further notice", "until further notice");
    check(om.today.size() == 2 && om.today[0].current && om.today[0].range == "Until further notice", "and it stays in Today, first");
    check(reg::nextBoundary(old, reg::viewTime(day.fetched), opts(true, 60)) != utc(2021, 2, 4), "its cut end is no boundary");
    const reg::FullDay fd = reg::buildFullDay(old, reg::viewTime(day.fetched), opts(), reg::Words());
    check(fd.days[0].rows.size() == 2, "nor does it vanish from the full day");
    // Next, clamped.
    reg::Schedule n = base;
    n.hasNow = false;
    n.today = {day.sam};
    n.today[0].clamped = true;
    n.next = n.today[0];
    const reg::Model nm = model(n, reg::viewTime(day.fetched) + 3600);
    checkText(nm.next.range, "From 4:00 PM", "Next: from, with no end");
    // And it is not a boundary at its stand-in end.
    check(reg::nextBoundary(n, at(2026, 10, 14, 16, 30), opts(true, 60)) != n.today[0].end, "a clamped end is not a boundary");
    s = base;
    s.today[1].clamped = true;
    check(reg::contentSignature(model(s, reg::viewTime(day.fetched))) != reg::contentSignature(model(base, reg::viewTime(day.fetched))), "signature: the row of a clamped booking differs");
  }

  // ---- low battery -----------------------------------------------------------------------------------------------
  {
    reg::Options o = opts();
    o.batteryLow = true;
    const reg::Model m = model(base, reg::viewTime(day.fetched), o);
    check(m.footerKind == reg::FooterKind::BatteryLow, "low battery: the footer says so");
    checkText(m.footer, "Battery low. This screen has stopped updating.", "low battery: the words");
    check(m.state == reg::State::InUse, "low battery: the rest is unchanged");
    reg::Schedule never;
    never.instrument = "X";
    never.timezone = "UTC";
    check(model(never, 0, o).footerKind == reg::FooterKind::BatteryLow, "low battery on a never-read schedule");
  }

  // ---- daylight saving and the day's length ----------------------------------------------------------------------
  {
    // Sunday 8 March 2026: clocks go forward at 02:00. The day is 23 hours.
    const int64_t noon = at(2026, 3, 8, 12);
    check(reg::localDayStart(noon, 1) - reg::localDayStart(noon, 0) == 23 * 3600, "spring forward: a 23-hour day");
    // Sunday 1 November 2026: clocks go back at 02:00. The day is 25 hours.
    const int64_t noonFall = at(2026, 11, 1, 12);
    check(reg::localDayStart(noonFall, 1) - reg::localDayStart(noonFall, 0) == 25 * 3600, "fall back: a 25-hour day");
    check(reg::localDayStart(noonFall, 0) == at(2026, 11, 1, 0), "the day starts at local midnight");
    check(reg::localDayStart(at(2026, 10, 14, 0), 0) == at(2026, 10, 14, 0), "midnight is its own day's start");
    check(reg::localDayStart(at(2026, 10, 14, 23, 59, 59), 0) == at(2026, 10, 14, 0), "the last second is still that day");
    check(reg::localDayStart(at(2026, 10, 14, 12), -1) == at(2026, 10, 13, 0), "yesterday");
    check(reg::localDayStart(0, 1) == 0, "no time, no day");
    // An all-day booking on the short day is still all day.
    reg::Schedule s;
    s.instrument = "X";
    s.timezone = "America/New_York";
    s.status = reg::Status::Ok;
    s.fetchedAt = at(2026, 3, 8, 8);
    s.today = {allDayBook("Short day", at(2026, 3, 8, 0), at(2026, 3, 9, 0))};
    const reg::Model m = model(s, at(2026, 3, 8, 8, 1));
    checkText(m.stateDetail, "Short day " + DOT + " all day", "all day on a 23-hour day");
    checkText(m.today[0].range, "All day", "its row");
    // A booking across the clock change: 01:00 to 04:00 is two hours of wall time but reads as it is.
    s.today = {book("Across", at(2026, 3, 8, 1), at(2026, 3, 8, 4))};
    s.fetchedAt = at(2026, 3, 8, 0, 30);
    const reg::Model across = model(s, at(2026, 3, 8, 0, 31));
    checkText(across.next.range, "1:00" + DASH + "4:00 AM", "a booking across the change reads in local time");
    // A day later in the same zone: the model works in the zone it is given.
    zone("UTC0");
    reg::Local l;
    reg::localParts(at(2026, 10, 14, 15), l);
    check(l.hour == 15, "(UTC: at() and localParts agree)");
    zone("CST-8");
    reg::localParts(at(2026, 10, 14, 15), l);
    check(l.hour == 15, "(UTC+8: at() and localParts agree)");
    check(reg::localDayStart(at(2026, 10, 14, 15), 1) - reg::localDayStart(at(2026, 10, 14, 15), 0) == 24 * 3600,
          "a zone without daylight saving: 24 hours");
    zone(NEW_YORK);
  }

  // ---- the full day ----------------------------------------------------------------------------------------------
  {
    const reg::FullDay full = reg::buildFullDay(base, at(2026, 10, 14, 17), opts(), reg::Words());
    check(full.days.size() == 2, "full day: Today and Tomorrow");
    checkText(full.days[0].heading, "Today " + DOT + " Wed 14 Oct", "full day: heading");
    checkText(full.days[1].heading, "Tomorrow " + DOT + " Thu 15 Oct", "full day: tomorrow's heading");
    check(full.days[0].rows.size() == 2, "full day: ended bookings stay");
    check(full.days[1].rows.size() == 1, "full day: tomorrow's rows");
    checkText(full.instrument, "Centrifuge 5424", "full day: the name");
    reg::Schedule s = base;
    s.hasNow = s.hasNext = false;
    s.today.clear();
    s.tomorrow.clear();
    const reg::FullDay none = reg::buildFullDay(s, at(2026, 10, 14, 17), opts(), reg::Words());
    check(none.days[0].rows.empty() && none.days[0].empty == "Nothing booked today.", "full day: nothing booked");
    const reg::FullDay nextDay = reg::buildFullDay(base, at(2026, 10, 15, 8), opts(), reg::Words());
    check(nextDay.days.size() == 1, "full day: Tomorrow is not listed when it is not covered");
    reg::Schedule unread;
    unread.instrument = "X";
    unread.timezone = "UTC";
    check(reg::buildFullDay(unread, 0, opts(), reg::Words()).days.empty(), "full day: never read, no days");
  }

  // ---- the signature ---------------------------------------------------------------------------------------------
  {
    const reg::Model a = model(base, reg::viewTime(day.fetched));
    check(reg::contentSignature(a) == reg::contentSignature(model(base, reg::viewTime(day.fetched))), "same model, same signature");
    check(reg::contentSignature(a) != 0, "never 0");
    const reg::Model later = model(base, reg::viewTime(day.fetched) + 120);
    check(reg::contentSignature(a) == reg::contentSignature(later), "two minutes on, nothing said differently");
    // Each thing the screen says changes the signature.
    reg::Model b = a;
    b.instrument = "Other";
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: the name");
    b = a;
    b.stateTail = "until 4:00 PM";
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: the state's tail");
    b = a;
    b.stateWho = "Ann";
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: who has it");
    b = a;
    b.state = reg::State::Free;
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: the state");
    b = a;
    b.next.title = "Sim";
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: the next booking");
    b = a;
    b.hasNext = false;
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: no next");
    b = a;
    b.today[1].range = "4:00" + DASH + "7:00 PM";
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: a row's time");
    b = a;
    b.today[0].current = false;
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: which row is current");
    b = a;
    b.tomorrow.clear();
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: tomorrow's rows");
    b = a;
    b.showTomorrow = false;
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: Tomorrow hidden");
    b = a;
    b.note = "1 repeating booking couldn't be read";
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: the note");
    b = a;
    b.footerKind = reg::FooterKind::Unavailable;
    check(reg::contentSignature(b) != reg::contentSignature(a), "signature: the kind of footer");
    // The time of a fresh footer is the exception.
    b = a;
    b.footer = "As of 2:09" + NB + "PM";
    b.asOf += 240;
    check(reg::contentSignature(b) == reg::contentSignature(a), "signature: a fresh footer's time is not in it");
    // Words that run together differently differ.
    reg::Model p = a, q = a;
    p.today = {{"ab", "c", false}};
    q.today = {{"a", "bc", false}};
    check(reg::contentSignature(p) != reg::contentSignature(q), "signature: boundaries between texts");
    // A stale footer's text is in it.
    reg::Model s1 = model(base, reg::viewTime(day.fetched) + 99999);
    reg::Schedule other = base;
    other.fetchedAt += 60;
    reg::Model s2 = model(other, reg::viewTime(day.fetched) + 99999);
    check(s1.footerKind == reg::FooterKind::Unavailable && s2.footerKind == reg::FooterKind::Unavailable &&
              reg::contentSignature(s1) != reg::contentSignature(s2),
          "signature: a stale footer's 'since' time is in it");
  }

  // ---- redraw only when the panel would change -------------------------------------------------------------------
  {
    const int64_t t0 = reg::viewTime(day.fetched);
    const reg::Model m = model(base, t0);
    const reg::Panel none;
    check(reg::redrawReason(none, m, false) == reg::Why::NotOurs, "nothing of ours on the panel: draw");
    check(reg::shouldRedraw(reg::Why::NotOurs), "NotOurs draws");
    const reg::Panel drawn = reg::panelFor(m);
    check(drawn.sig == reg::contentSignature(m) && drawn.asOf == day.fetched, "what a draw leaves on the panel");
    check(reg::redrawReason(drawn, m, false) == reg::Why::Same, "the same model: leave it");
    check(!reg::shouldRedraw(reg::Why::Same), "Same does not draw");
    check(reg::redrawReason(drawn, model(base, t0 + 600), false) == reg::Why::Same, "ten minutes on, nothing changed");
    check(reg::redrawReason(drawn, model(base, day.ana.end), false) == reg::Why::Content, "Ana's booking ends: draw");
    check(reg::shouldRedraw(reg::Why::Content), "Content draws");
    // A Method step outranks every reason.
    check(reg::redrawReason(drawn, model(base, day.ana.end), true) == reg::Why::MethodStep, "a step on the panel: never");
    check(reg::redrawReason(none, m, true) == reg::Why::MethodStep, "a step on the panel, nothing of ours: still never");
    check(!reg::shouldRedraw(reg::Why::MethodStep), "MethodStep does not draw");
    // The footer's time, on a day with nothing booked so that only the footer can differ.
    reg::Schedule quiet = base;
    quiet.hasNow = quiet.hasNext = false;
    quiet.today.clear();
    quiet.tomorrow.clear();
    const reg::Panel quietShown = reg::panelFor(model(quiet, t0));
    reg::Schedule newer = quiet;
    newer.fetchedAt = day.fetched + 5 * 60;
    check(reg::redrawReason(quietShown, model(newer, newer.fetchedAt + 5), false) == reg::Why::Same, "five minutes newer: leave it");
    newer.fetchedAt = day.fetched + reg::FOOTER_REFRESH_S - 1;
    check(reg::redrawReason(quietShown, model(newer, newer.fetchedAt + 5), false) == reg::Why::Same, "just under the limit: leave it");
    newer.fetchedAt = day.fetched + reg::FOOTER_REFRESH_S;
    check(reg::redrawReason(quietShown, model(newer, newer.fetchedAt + 5), false) == reg::Why::Footer, "at the limit: draw");
    check(reg::shouldRedraw(reg::Why::Footer), "Footer draws");
    // Going stale changes the footer's words: always drawn.
    check(reg::redrawReason(drawn, model(base, day.fetched + reg::staleAfterSeconds(60) + 5), false) == reg::Why::Content,
          "going stale: draw");
    reg::Schedule stale = base;
    stale.status = reg::Status::Stale;
    check(reg::redrawReason(drawn, model(stale, t0), false) == reg::Why::Content, "the Worker says stale: draw");
    // And recovering draws too.
    const reg::Panel staleShown = reg::panelFor(model(stale, t0));
    check(reg::redrawReason(staleShown, m, false) == reg::Why::Content, "recovering: draw");
    check(reg::redrawReason(staleShown, model(stale, t0 + 30), false) == reg::Why::Same, "still stale, same words: leave it");
    check(staleShown.asOf == 0, "a stale footer has no time to compare");
  }

  // ---- boundaries ------------------------------------------------------------------------------------------------
  {
    const reg::Options o = opts(true, 60);
    const int64_t t0 = reg::viewTime(day.fetched);
    check(reg::nextBoundary(base, t0, o) == day.ana.end, "next boundary: Ana's end");
    check(reg::nextBoundary(base, day.ana.end, o) == day.sam.start, "then Sam's start");
    const int64_t staleAt = day.fetched + reg::staleAfterSeconds(60) + 1;  // 16:10:01
    check(reg::nextBoundary(base, day.sam.start, o) == staleAt, "then the reading going stale, before Sam's end");
    check(reg::nextBoundary(base, staleAt, o) == day.sam.end, "then Sam's end");
    check(reg::nextBoundary(base, day.sam.end, o) == at(2026, 10, 15, 0), "then midnight");
    check(reg::nextBoundary(base, at(2026, 10, 15, 0), o) == day.lee.start, "then Lee's start");
    check(reg::nextBoundary(base, day.lee.start, o) == day.lee.end, "then Lee's end");
    check(reg::nextBoundary(base, day.lee.end, o) == at(2026, 10, 16, 0), "then the edge of what the reply covers");
    check(reg::nextBoundary(base, at(2026, 10, 16, 0), o) == 0, "past the edge: nothing");
    // Two bookings that meet.
    reg::Schedule s = base;
    s.today = {book("A", at(2026, 10, 14, 13), at(2026, 10, 14, 15)), book("B", at(2026, 10, 14, 15), at(2026, 10, 14, 16))};
    s.hasNext = false;
    check(reg::nextBoundary(s, t0, o) == at(2026, 10, 14, 15), "back to back: one boundary");
    // The reading goes stale before anything else happens.
    reg::Schedule quiet = base;
    quiet.hasNow = quiet.hasNext = false;
    quiet.today.clear();
    quiet.tomorrow.clear();
    check(reg::nextBoundary(quiet, t0, o) == day.fetched + reg::staleAfterSeconds(60) + 1, "quiet: the reading going stale");
    check(reg::nextBoundary(quiet, t0, opts(true, 5)) == day.fetched + 1800 + 1, "quiet, 5-minute poll: 30 minutes");
    // A schedule the Worker already calls stale: the stale time has no meaning, midnight does.
    quiet.status = reg::Status::Stale;
    check(reg::nextBoundary(quiet, t0, o) == at(2026, 10, 15, 0), "stale: midnight");
    // Nothing to wake for.
    reg::Schedule never;
    never.instrument = "X";
    never.timezone = "UTC";
    check(reg::nextBoundary(never, t0, o) == 0, "never read: nothing");
    check(reg::nextBoundary(base, 0, o) == 0, "clock unset: nothing");
    check(reg::nextBoundary(base, day.fetched - 301, o) == 0, "clock behind the reply: nothing");
    // Midnight is the next local midnight on a short and a long day.
    reg::Schedule dst = base;
    dst.fetchedAt = at(2026, 3, 8, 8);
    dst.hasNow = dst.hasNext = false;
    dst.today.clear();
    dst.tomorrow.clear();
    check(reg::nextBoundary(dst, at(2026, 3, 8, 8, 1), opts(true, 1440)) == at(2026, 3, 9, 0), "midnight after a spring-forward day");
    check(at(2026, 3, 9, 0) - at(2026, 3, 8, 0) == 23 * 3600, "(which is 23 hours after the day began)");
    dst.fetchedAt = at(2026, 11, 1, 8);
    check(reg::nextBoundary(dst, at(2026, 11, 1, 8, 1), opts(true, 1440)) == at(2026, 11, 2, 0), "midnight after a fall-back day");
    // A moment inside the repeated hour.
    reg::Schedule rep = dst;
    rep.today = {book("Repeat", at(2026, 11, 1, 1, 30), at(2026, 11, 1, 3))};
    check(reg::nextBoundary(rep, at(2026, 11, 1, 8), opts(true, 1440)) == at(2026, 11, 2, 0), "after the repeated hour: midnight");
  }

  std::cout << "register_model: " << checks << " checks passed\n";
  return 0;
}
