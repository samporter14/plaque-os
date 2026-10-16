#include <iostream>

#include "FakePorts.h"
#include "MethodFormat.h"

using fake::check;
using fake::checkEq;
using method::LocalTime;
using method::ProgressKind;

namespace {
LocalTime at(int year, int month, int day, int hour, int minute, int weekday) {
  LocalTime t;
  t.year = year;
  t.month = month;
  t.day = day;
  t.hour = hour;
  t.minute = minute;
  t.weekday = weekday;
  t.valid = true;
  return t;
}

// Monday 5 October 2026, 21:10 (the tests read like the spec's examples).
const LocalTime NOW = at(2026, 10, 5, 21, 10, 1);
const method::Words WORDS;

void testClockText() {
  checkEq(method::clockText(at(2026, 10, 5, 21, 4, 1), false), std::string("21:04"), "24-hour evening");
  checkEq(method::clockText(at(2026, 10, 5, 0, 5, 1), false), std::string("00:05"), "24-hour just after midnight");
  checkEq(method::clockText(at(2026, 10, 5, 9, 7, 1), false), std::string("09:07"), "24-hour pads the hour");
  checkEq(method::clockText(at(2026, 10, 5, 21, 4, 1), true), std::string("9:04 PM"), "12-hour evening");
  checkEq(method::clockText(at(2026, 10, 5, 0, 5, 1), true), std::string("12:05 AM"), "12-hour just after midnight");
  checkEq(method::clockText(at(2026, 10, 5, 12, 0, 1), true), std::string("12:00 PM"), "12-hour noon");
  checkEq(method::clockText(at(2026, 10, 5, 9, 7, 1), true), std::string("9:07 AM"), "12-hour morning");
}

void testDaysFromTo() {
  checkEq(method::daysFromTo(at(2026, 10, 5, 1, 0, 1), at(2026, 10, 5, 23, 59, 1)), 0, "same day");
  checkEq(method::daysFromTo(at(2026, 10, 4, 23, 59, 0), at(2026, 10, 5, 0, 1, 1)), 1, "a day boundary is a day, not 2 minutes");
  checkEq(method::daysFromTo(at(2026, 1, 31, 0, 0, 6), at(2026, 2, 1, 0, 0, 0)), 1, "month end");
  checkEq(method::daysFromTo(at(2026, 12, 31, 0, 0, 4), at(2027, 1, 1, 0, 0, 5)), 1, "year end");
  checkEq(method::daysFromTo(at(2028, 2, 28, 0, 0, 1), at(2028, 3, 1, 0, 0, 3)), 2, "leap year");
  checkEq(method::daysFromTo(at(2027, 2, 28, 0, 0, 0), at(2027, 3, 1, 0, 0, 1)), 1, "non-leap year");
  checkEq(method::daysFromTo(at(2026, 9, 28, 0, 0, 1), NOW), 7, "28 Sep to 5 Oct");
  checkEq(method::daysFromTo(NOW, at(2026, 10, 4, 0, 0, 0)), -1, "backwards");
  // Cross-check the calendar helper used by the other tests.
  const LocalTime epoch = fake::localFromEpoch(0, 0);
  check(epoch.year == 1970 && epoch.month == 1 && epoch.day == 1 && epoch.weekday == 4, "epoch is Thursday 1 Jan 1970");
  const LocalTime leap = fake::localFromEpoch(1709164800, 0);  // 2024-02-29 00:00 UTC
  check(leap.year == 2024 && leap.month == 2 && leap.day == 29 && leap.weekday == 4, "leap day is a Thursday");
  const LocalTime shifted = fake::localFromEpoch(1709164800, -5 * 3600);
  check(shifted.day == 28 && shifted.hour == 19, "time zone offset moves the day");
}

void testReceivedText() {
  checkEq(method::receivedText(at(2026, 10, 5, 21, 0, 1), NOW, false, WORDS), std::string("today 21:00"), "today shows the time");
  checkEq(method::receivedText(at(2026, 10, 5, 21, 0, 1), NOW, true, WORDS), std::string("today 9:00 PM"), "12-hour today");
  checkEq(method::receivedText(at(2026, 10, 3, 14, 0, 6), NOW, false, WORDS), std::string("Sat"), "two days ago is a weekday");
  checkEq(method::receivedText(at(2026, 10, 4, 23, 59, 0), at(2026, 10, 5, 0, 1, 1), false, WORDS), std::string("Sun"),
          "yesterday just before midnight is a weekday, not today");
  checkEq(method::receivedText(at(2026, 9, 29, 8, 0, 2), NOW, false, WORDS), std::string("Tue"), "six days ago is a weekday");
  checkEq(method::receivedText(at(2026, 9, 28, 8, 0, 1), NOW, false, WORDS), std::string("28 Sep"),
          "a week ago is a date, so Monday is not confused with today");
  checkEq(method::receivedText(at(2025, 1, 3, 8, 0, 5), NOW, false, WORDS), std::string("3 Jan"), "long ago is a date");
  checkEq(method::receivedText(at(2026, 10, 9, 8, 0, 5), NOW, false, WORDS), std::string("9 Oct"),
          "a time in the future (clock changed) is a date");
  checkEq(method::receivedText(LocalTime{}, NOW, false, WORDS), std::string(""), "an unknown time is empty");
  checkEq(method::receivedText(at(2026, 10, 5, 21, 0, 1), LocalTime{}, false, WORDS), std::string("5 Oct"),
          "no clock now: the date alone");
  LocalTime bad = at(2026, 13, 5, 21, 0, 1);
  checkEq(method::receivedText(bad, NOW, false, WORDS), std::string(""), "an impossible month is empty");
  bad = at(2026, 10, 5, 21, 0, 9);
  checkEq(method::receivedText(bad, NOW, false, WORDS), std::string(""), "an impossible weekday is empty");

  method::Words german;
  german.today = "heute";
  german.weekdays[6] = "Sa";
  german.months[9] = "Okt";
  checkEq(method::receivedText(at(2026, 10, 5, 21, 0, 1), NOW, false, german), std::string("heute 21:00"), "words come from the table");
  checkEq(method::receivedText(at(2026, 10, 3, 14, 0, 6), NOW, false, german), std::string("Sa"), "translated weekday");
  checkEq(method::receivedText(at(2026, 10, 3, 14, 0, 6), at(2026, 10, 20, 0, 0, 2), false, german), std::string("3 Okt"),
          "translated month");
}

void testProgress() {
  // 8 pages: the cover (0) and steps 1 to 7.
  const auto p = [](int64_t page, uint32_t pages = 8) { return method::progressFor(page, pages); };
  check(p(-1).kind == ProgressKind::NotStarted, "never opened");
  check(p(0).kind == ProgressKind::NotStarted, "on the cover");
  check(p(1).kind == ProgressKind::InProgress && p(1).step == 1 && p(1).steps == 7, "step 1 of 7");
  check(p(6).kind == ProgressKind::InProgress && p(6).step == 6, "step 6 of 7");
  check(p(7).kind == ProgressKind::Finished, "the last page is finished");
  check(p(8).kind == ProgressKind::Finished, "the end-of-book position is finished");
  check(p(900).kind == ProgressKind::Finished, "a position past the end is finished");

  check(p(0, 2).kind == ProgressKind::NotStarted && p(1, 2).kind == ProgressKind::Finished,
        "a one-step method: cover not started, step 1 finished");
  check(p(-1, 1).kind == ProgressKind::NotStarted && p(0, 1).kind == ProgressKind::Finished,
        "a cover-only book: opened means read");
  check(p(0, 0).kind == ProgressKind::NotStarted && p(5, 0).kind == ProgressKind::NotStarted, "unknown page count");

  checkEq(method::stepCount(8), 7u, "steps are pages minus the cover");
  checkEq(method::stepCount(0), 0u, "no pages, no steps");
  checkEq(method::stepsText(8, WORDS), std::string("7 steps"), "plural");
  checkEq(method::stepsText(2, WORDS), std::string("1 step"), "singular");
  checkEq(method::stepsText(1, WORDS), std::string("0 steps"), "none");
  checkEq(method::progressText(p(-1), WORDS), std::string("not started"), "text: not started");
  checkEq(method::progressText(p(5), WORDS), std::string("step 5 of 7"), "text: step k of N");
  checkEq(method::progressText(p(7), WORDS), std::string("finished"), "text: finished");
}

void testRowDetail() {
  // The three rows from the spec.
  checkEq(method::rowDetail(method::Kind::Method, 8, at(2026, 10, 5, 21, 0, 1), NOW, false, -1, WORDS),
          std::string("7 steps \xC2\xB7 received today 21:00 \xC2\xB7 not started"), "spec row 1");
  checkEq(method::rowDetail(method::Kind::Method, 13, at(2026, 10, 3, 9, 0, 6), NOW, false, 5, WORDS),
          std::string("12 steps \xC2\xB7 received Sat \xC2\xB7 step 5 of 12"), "spec row 2");
  checkEq(method::rowDetail(method::Kind::Method, 10, at(2026, 9, 28, 9, 0, 1), NOW, false, 9, WORDS),
          std::string("9 steps \xC2\xB7 received 28 Sep \xC2\xB7 finished"), "spec row 3");
  checkEq(method::rowDetail(method::Kind::Method, 8, LocalTime{}, NOW, false, 0, WORDS), std::string("7 steps \xC2\xB7 not started"),
          "an unknown received time is left out");
  checkEq(method::rowDetail(method::Kind::Method, 8, at(2026, 10, 5, 21, 0, 1), NOW, true, 3, WORDS),
          std::string("7 steps \xC2\xB7 received today 9:00 PM \xC2\xB7 step 3 of 7"), "12-hour clock");
}

void testOtherKinds() {
  // A table, plate map or board has no steps and no progress.
  checkEq(method::rowDetail(method::Kind::Table, 2, at(2026, 10, 5, 21, 2, 1), NOW, false, 1, WORDS),
          std::string("Table \xC2\xB7 2 pages \xC2\xB7 received today 21:02"), "spec table row");
  checkEq(method::rowDetail(method::Kind::Plate, 1, at(2026, 10, 3, 9, 0, 6), NOW, false, -1, WORDS),
          std::string("Plate map \xC2\xB7 1 page \xC2\xB7 received Sat"), "plate map row, one page");
  checkEq(method::rowDetail(method::Kind::Board, 3, LocalTime{}, NOW, false, 2, WORDS),
          std::string("Board \xC2\xB7 3 pages"), "board row without a received time");
  checkEq(method::rowDetail(method::Kind::Table, 2, at(2026, 9, 28, 9, 0, 1), NOW, false, 1, WORDS),
          std::string("Table \xC2\xB7 2 pages \xC2\xB7 received 28 Sep"), "progress never shows on a table, even with a saved page");
  checkEq(method::pagesText(1, WORDS), std::string("1 page"), "singular page");
  checkEq(method::pagesText(12, WORDS), std::string("12 pages"), "plural pages");
  method::Words other;
  other.kinds[1] = "Tabelle";
  other.pages = "%d Seiten";
  checkEq(method::rowDetail(method::Kind::Table, 2, LocalTime{}, NOW, false, 0, other), std::string("Tabelle \xC2\xB7 2 Seiten"),
          "kind names and formats come from the words table");
  // A method row is unchanged by the kind work.
  checkEq(method::rowDetail(method::Kind::Method, 8, LocalTime{}, NOW, false, 3, WORDS), std::string("7 steps \xC2\xB7 step 3 of 7"),
          "method rows keep their steps and progress");
}

void testMegabytes() {
  checkEq(method::megabytesText(0), std::string("0.0"), "zero");
  checkEq(method::megabytesText(1), std::string("0.1"), "one byte rounds up");
  checkEq(method::megabytesText(100000), std::string("0.1"), "exact tenth");
  checkEq(method::megabytesText(100001), std::string("0.2"), "just over a tenth");
  checkEq(method::megabytesText(1300000), std::string("1.3"), "1.3 MB");
  checkEq(method::megabytesText(1900000 + 262144), std::string("2.2"), "a full book plus the spare 256 KB");
  checkEq(method::megabytesText(12000000), std::string("12.0"), "two digits");
}

}  // namespace

int main() {
  testClockText();
  testDaysFromTo();
  testReceivedText();
  testProgress();
  testRowDetail();
  testOtherKinds();
  testMegabytes();
  std::cout << "method format: " << fake::checkCount() << " checks passed\n";
  return 0;
}
