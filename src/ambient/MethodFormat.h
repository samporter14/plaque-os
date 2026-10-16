#pragma once

#include <cstdint>
#include <string>

#include "MethodProtocol.h"

// Text for the Method shelf rows: step counts, progress and relative dates.
// Pure logic with no Arduino or I18n dependency (the words come in through
// Words), so the host tests in test/method_format cover every branch.
namespace method {

// A calendar time in the reader's time zone. The firmware fills it from
// localtime_r(); the tests build it by hand.
struct LocalTime {
  int year = 0;
  int month = 0;    // 1 to 12
  int day = 0;      // 1 to 31
  int hour = 0;     // 0 to 23
  int minute = 0;   // 0 to 59
  int weekday = 0;  // 0 = Sunday
  bool valid = false;
};

// Whole calendar days from `from` to `to` (positive when `to` is later).
int daysFromTo(const LocalTime& from, const LocalTime& to);

// The words and formats the row text is built from; the firmware fills them from I18n.
struct Words {
  const char* oneStep = "1 step";
  const char* steps = "%d steps";
  const char* received = "received %s";
  const char* today = "today";
  const char* notStarted = "not started";
  const char* stepOf = "step %d of %d";
  const char* finished = "finished";
  const char* onePage = "1 page";
  const char* pages = "%d pages";
  // Indexed by method::Kind: what a book is called on its row ("Table", "Plate map", "Board").
  const char* kinds[4] = {"Method", "Table", "Plate map", "Board"};
  const char* weekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const char* months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
};

// "21:04", or "9:04 PM" in 12-hour mode.
std::string clockText(const LocalTime& time, bool twelveHour);

// How long ago a method arrived: "today 21:00", a weekday name for the last six
// days ("Sat"), otherwise "28 Sep". Empty when `received` is not a valid time.
std::string receivedText(const LocalTime& received, const LocalTime& now, bool twelveHour, const Words& words);

enum class ProgressKind : uint8_t { NotStarted, InProgress, Finished };
struct Progress {
  ProgressKind kind = ProgressKind::NotStarted;
  uint32_t step = 0;   // 1-based, InProgress only
  uint32_t steps = 0;  // steps in the method (the cover page is not a step)
};

// Where a reader stands in a book of `pageCount` pages (page 0 is the cover, pages
// 1 to pageCount-1 are the steps). `savedPage` is the page index in progress.bin, or
// a negative number when the book has never been opened.
//   no file or the cover        -> not started
//   a step before the last      -> step k of N
//   the last page, or the end   -> finished
Progress progressFor(int64_t savedPage, uint32_t pageCount);

// Number of steps in a book (pages minus the cover).
uint32_t stepCount(uint32_t pageCount);

std::string stepsText(uint32_t pageCount, const Words& words);
std::string progressText(const Progress& progress, const Words& words);

// A method: "7 steps · received today 21:00 · not started". Any other kind has no steps and no progress:
// "Table · 2 pages · received today 21:02". `received` may be invalid, in which case that part is left out.
std::string rowDetail(Kind kind, uint32_t pageCount, const LocalTime& received, const LocalTime& now,
                      bool twelveHour, int64_t savedPage, const Words& words);

std::string pagesText(uint32_t pageCount, const Words& words);

// Size in decimal megabytes rounded up to a tenth: 1,300,000 -> "1.3".
std::string megabytesText(uint64_t bytes);

}  // namespace method
