#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "RegisterData.h"

// What the Register screen says, as text: the state of the instrument now, its next booking, today's and tomorrow's
// bookings, and how fresh all that is. Pure logic with no Arduino, no storage and no fonts, covered by
// test/register_model; RegisterLayout.h puts the text on the 480 x 800 panel and RegisterWake.h decides when the
// reader wakes to change it.
//
// The schedule is a list of bookings with absolute times, made at `fetched_at`. The model works out what is true at
// the time it is asked for, in the reader's own time zone, so it stays right between polls: a booking that has begun
// is "In use", one that has ended is gone from Today, and at midnight Tomorrow becomes Today. Nothing is said that the
// data cannot support:
//  - never read, or the reader's clock is not set (or is behind the data): the state is "Not known";
//  - past the end of what the reply covered (the end of the day after the one it was made on): "Not known", no lists;
//  - the day after that is not listed at all (Tomorrow is hidden rather than shown empty);
//  - a note from the Worker (some bookings could not be read, only part of a busy day is shown) means "Free" cannot be
//    promised: the state reads "Free?" and the note is shown with it, under the state, not down by the footer;
//  - data older than the reader's own limit, or a reply the Worker marked stale: the footer says the calendar has been
//    unavailable since the time it was last read, and the rest stays as it was.
namespace reg {

// The reader draws what will be true by the time the panel has refreshed: a wake arms for the boundary less this, and
// the screen it draws looks this far ahead, so that it lands on the boundary rather than behind it.
constexpr int64_t VIEW_LEAD_S = 4;
// A reply made this long after the reader's clock reads is taken to mean the clock is wrong.
constexpr int64_t CLOCK_SKEW_S = 300;
// A reply is stale when its reading is older than this, or older than twice the poll interval and a margin when the
// reader polls less often than the Worker's 30 minutes allow for.
constexpr int64_t MIN_STALE_S = 30 * 60;
int64_t staleAfterSeconds(int pollMinutes);

// ---- local time -----------------------------------------------------------------------------------------------------

// A calendar time in the reader's zone (localtime_r under the process TZ, which the reader sets from its clock
// settings). The tests set TZ to a POSIX rule to cover daylight saving.
struct Local {
  int year = 0;
  int month = 0;    // 1 to 12
  int day = 0;      // 1 to 31
  int hour = 0;     // 0 to 23
  int minute = 0;   // 0 to 59
  int weekday = 0;  // 0 = Sunday
  bool valid = false;
};
// With a zone (the instrument's, from the reply) the answer comes from the zone's own spans and the reader's clock
// settings are not consulted at all; without one it is the reader's zone.
const Zone* zoneOf(const Schedule& schedule);  // the schedule's zone, or null
bool localParts(int64_t epoch, Local& out, const Zone* zone = nullptr);
// 00:00 local on the day `epoch` falls on, plus dayOffset days (a day is 23, 24 or 25 hours across a clock change).
// 0 when the time cannot be converted.
int64_t localDayStart(int64_t epoch, int dayOffset = 0, const Zone* zone = nullptr);
// Calendar days from one local date to another (positive when `to` is later).
int daysBetween(const Local& from, const Local& to);

// ---- words ----------------------------------------------------------------------------------------------------------

// The text the screen is made of. The firmware fills it from I18n (the register strings); the defaults are the English text
// and what the host tests use. The formats take their arguments in the order shown.
struct Words {
  const char* free = "Free";
  const char* freeMaybe = "Free?";  // free as far as the calendar could be read: the note says what it could not
  const char* inUse = "In use";
  const char* notKnown = "Not known";
  const char* separator = " \xc2\xb7 ";  // between who has it and until when: "Ana · until 3:00 PM"
  const char* until = "until %s";         // the end or the next start: "until 4:00 PM"
  const char* allDayTail = "all day";     // "Maintenance · all day"
  const char* untilFurtherNotice = "until further notice";  // a booking whose end is not known
  const char* waiting = "Waiting for the calendar.";
  const char* noClock = "The clock is not set.";
  const char* allDay = "All day";
  const char* allDayOn = "%s all day";   // a day word: "tomorrow all day"
  const char* untilTime = "Until %s";    // a booking that began before the day: "Until 11:00 AM"
  const char* fromTime = "From %s";      // a booking that goes on past the day: "From 4:00 PM"
  const char* midnight = "midnight";
  const char* endOfDay = "end of %s";    // a weekday: "end of Wed"
  const char* tomorrow = "tomorrow";
  const char* yesterday = "yesterday";
  const char* dayTime = "%s %s";         // a day word and a time: "Wed 9:00 AM"
  const char* nothingToday = "Nothing booked today.";
  const char* nothingMoreToday = "Nothing more today.";
  const char* nothingTomorrow = "Nothing booked tomorrow.";
  const char* more = "+ %d more";
  const char* asOf = "As of %s";
  const char* unavailableSince = "Calendar unavailable since %s";
  const char* notRead = "Calendar not read yet";
  const char* batteryLow = "Battery low. This screen has stopped updating.";
  const char* heading = "%s \xc2\xb7 %s %d %s";  // the full-day view: "Today · Wed 14 Oct"
  const char* next = "Next";
  const char* today = "Today";
  const char* tomorrowHeading = "Tomorrow";
  const char* pageOf = "Page %d of %d";  // the full-day view
  // What the keys do in the full-day view, in the reader's own terms: its side buttons are Left and Right.
  const char* hintNext = "Right or tap: next";
  const char* hintNextLast = "Right or tap: the screen";
  const char* hintPrev = "Left: previous";
  const char* hintPrevFirst = "Left: the screen";
  const char* weekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const char* months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
};

// "15:00", or "3:00 PM" in 12-hour mode.
std::string clockText(const Local& time, bool twelveHour);
// A time of day, with a day word when it is not on the same local date as `reference`: "3:00 PM", "tomorrow 9:00 AM",
// "Wed 9:00 AM" (a weekday for anything else within a week either way), "14 Oct"-style dates are not needed because
// nothing on the screen reaches further than a week.
std::string dayTimeText(int64_t epoch, int64_t reference, bool twelveHour, const Words& words, const Zone* zone = nullptr);

// ---- the model -----------------------------------------------------------------------------------------------------

struct Options {
  bool twelveHour = false;
  int pollMinutes = 15;     // the configured interval, which sets how old a reading may be before it is stale
  bool batteryLow = false;  // the reader has stopped waking: the footer says so
};

struct Row {
  std::string range;  // "9:00–11:00 AM", "All day", "Until 11:00 AM", "tomorrow 9:00–11:00 AM"
  std::string title;
  bool current = false;  // the booking in progress
};

enum class State : uint8_t { Unknown, Free, FreeUntil, InUse };
enum class FooterKind : uint8_t { AsOf, Unavailable, NotRead, BatteryLow };

struct Model {
  std::string instrument;
  State state = State::Unknown;
  std::string stateWord;    // "Free", "In use", "Not known"
  std::string stateWho;     // who has it: "Ana" (only when in use)
  std::string stateTail;    // "until 3:00 PM", "all day", "Waiting for the calendar."; may be empty
  std::string stateDetail;  // who and tail together: "Ana · until 3:00 PM"; may be empty
  std::string stateNote;    // the Worker's note, shown with the state when "Free" could be wrong (see buildModel)
  bool hasNext = false;
  Row next;
  bool showLists = false;     // Today (and Tomorrow) are known
  bool showTomorrow = false;  // Tomorrow is covered by the reply
  std::vector<Row> today;     // what is still to come today, the booking in progress first
  std::vector<Row> tomorrow;
  std::string todayEmpty;     // the sentence for an empty Today
  std::string tomorrowEmpty;
  std::string note;           // the Worker's sentences about what it could not read; may be empty (see stateNote)
  FooterKind footerKind = FooterKind::NotRead;
  std::string footer;         // the whole footer line
  int64_t asOf = 0;           // when the calendar was read, for FooterKind::AsOf; 0 otherwise
  bool stale = false;
};

// The reader's clock as the Register screen may use it. The system clock alone is not enough: after a flash or a power
// loss it is the floor restored from NVS, which is no more than "not earlier than this", and a booking that ended since
// would still read "In use". So it counts only when something vouches for it: a sync set it this boot, or the retained
// clock (the RTC chip, or the timer that has run since a recorded sync) is trusted. Otherwise 0, which the screen
// reads as "Not known" (the clock is not set).
constexpr int64_t trustedReading(const int64_t systemNow, const bool syncedThisBoot, const bool retainedTrusted) {
  return systemNow > 0 && (syncedThisBoot || retainedTrusted) ? systemNow : 0;
}

// Looks `VIEW_LEAD_S` ahead of a clock reading; 0 stays 0 ("the clock is not set").
int64_t viewTime(int64_t now);

// The model at `at` (a viewTime(); 0 when the clock is not trustworthy).
Model buildModel(const Schedule& schedule, int64_t at, const Options& options, const Words& words);

// The whole of today and tomorrow, ended bookings included, for the full-day view a tap opens.
struct Day {
  std::string heading;  // "Today · Wed 14 Oct"
  std::vector<Row> rows;
  std::string empty;    // the sentence when there are no rows
};
struct FullDay {
  std::string instrument;
  std::vector<Day> days;  // Today, and Tomorrow when the reply covers it
  std::string footer;
  bool stale = false;
};
FullDay buildFullDay(const Schedule& schedule, int64_t at, const Options& options, const Words& words);

// A hash of everything the screen says except the time of a fresh footer ("As of 2:05 PM"), which has its own rule
// (see Panel). Never 0.
uint32_t contentSignature(const Model& model);

// ---- the panel -----------------------------------------------------------------------------------------------------

// What the last Register draw left on the panel, kept across the deep sleeps between wakes. `sig` 0 means the panel
// shows nothing of ours (a different sleep screen, a Method step, a reader's page).
struct Panel {
  uint32_t sig = 0;
  int64_t asOf = 0;
};

// A fresh footer's time moves with every poll. Redrawing for each one would flash the panel every few minutes to say
// "As of" a slightly later minute, so a footer that is still true, and only older, waits until it is this much behind.
constexpr int64_t FOOTER_REFRESH_S = 60 * 60;

enum class Why : uint8_t {
  Same,        // nothing on the panel would change
  MethodStep,  // a Method step is on the panel: it outranks this screen
  NotOurs,     // the panel shows something else
  Content,     // the state, a list, the note or the footer's words changed
  Footer,      // the footer's time is FOOTER_REFRESH_S or more behind
};
// Should the panel be redrawn, and why. The step outranks everything.
Why redrawReason(const Panel& shown, const Model& model, bool methodStepOnPanel);
inline bool shouldRedraw(Why why) { return why == Why::NotOurs || why == Why::Content || why == Why::Footer; }
// What the panel will hold once `model` is drawn.
Panel panelFor(const Model& model);

// ---- boundaries ----------------------------------------------------------------------------------------------------

// The first moment after `at` when the screen would say something different with no new reply: a booking starts or
// ends, the next local midnight, or the reading becomes stale. 0 when nothing can change (no schedule yet, the clock
// not set, or the reply already out of date).
int64_t nextBoundary(const Schedule& schedule, int64_t at, const Options& options);

}  // namespace reg
