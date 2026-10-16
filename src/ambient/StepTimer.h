#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A step timer on the reader (Stage C): one timer at a time, started from a Method step page, that survives
// sleeping and ends with an alert. Pure logic: no Arduino, no storage, no clock of its own, so every decision the
// firmware acts on (what to show, when to refresh, when to wake, what a boot does about a running timer, how the
// light pulses, where things are drawn) is covered by the host tests in test/step_timer. The firmware side
// (StepTimerService, the reader, the alert screen, main.cpp) only gathers inputs and acts on the answers.
//
// Time. A timer is "remaining milliseconds until its end". The firmware counts that against the hardware timer while it
// is awake (esp_timer: it keeps running through an SNTP clock step, which can move the wall clock by hours the first
// time the reader syncs) and against the wall clock only across a sleep, where nothing runs that could step it.
// Record::endWallMs is that wall-clock end, rewritten every time the reader goes to sleep.
namespace steptimer {

// ---- Constants -------------------------------------------------------------------------------------------------

constexpr int64_t ALERT_LIMIT_MS = 10 * 60 * 1000;  // an alert pulses and holds the reader awake for 10 minutes at most
constexpr int64_t WAKE_LEAD_MS = 2000;              // the wake timer is set this early: booting takes about this long
constexpr int64_t WAKE_SLACK_MS = 5000;             // a timer wake this close to the end is the alert, not a poll
constexpr uint64_t MIN_WAKE_US = 1000000;           // never arm a wake sooner than this
constexpr uint16_t MIN_ALERT_BATTERY_PERCENT = 10;  // below this the light stays off (the panel alert still shows)

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  int right() const { return x + w; }
  int bottom() const { return y + h; }
  bool empty() const { return w <= 0 || h <= 0; }
  bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

constexpr int PAGE_W = 480;
constexpr int PAGE_H = 800;
// ---- The running timer, as persisted ----------------------------------------------------------------------------

enum class Phase : uint8_t {
  None = 0,
  Running = 1,  // counting down
  Alerted = 2,  // the end was shown; waits for the reader to be used again (the next wake clears it)
};

struct Record {
  Phase phase = Phase::None;
  std::string bookPath;  // the Method book that carries the timer: /Lamina/Method/<name>.xtc
  uint16_t page = 0;     // 1-based page of that book
  uint32_t seconds = 0;  // the full duration
  std::string label;     // as the book gave it, possibly empty
  uint16_t boxX = 0;     // the page's timer box, where the countdown is drawn
  uint16_t boxY = 0;
  uint16_t boxW = 0;
  uint16_t boxH = 0;
  int64_t endWallMs = 0;   // the end on the wall clock, as of the last sleep or start (see the note above)
  uint16_t panelPage = 0;  // 1-based page that was on the panel when the reader slept, 0 when not known
};

// True when a record read back from the card is one this reader could have written. Phase None is valid whatever
// else it holds (and is how a damaged record is reset).
bool recordValid(const Record& record);

// ---- Reading and showing the time -------------------------------------------------------------------------------

// Whole seconds still to go as the display shows them: rounded up (a countdown shows 0:00 only at the end), never
// below 0.
int64_t displaySeconds(int64_t remainingMs);
// "5:00", "0:45", "12:00", "1:30:00": the Mac prints a timer the same way on the page.
std::string clockText(int64_t seconds);

// The next value the display moves to after showing `shown` seconds: each minute while more than a minute is left
// (never skipping past 1:00), then every 10 seconds in the last minute, then 0:00. 0 stays 0.
int64_t nextShownSeconds(int64_t shown);
// The display must change now: the time left has reached the next value after the one shown.
bool tickDue(int64_t shownSeconds, int64_t remainingMs);

constexpr bool timerDue(int64_t remainingMs) { return remainingMs <= 0; }

// ---- Sleeping ---------------------------------------------------------------------------------------------------

// The wake timer to arm on the way to sleep: the sooner of the ambient poll (pollMicros, zero when polling is off or
// the battery is too low for it) and the running timer's end, a little early (WAKE_LEAD_MS) so the alert lands on the
// end, never sooner than MIN_WAKE_US. Zero means no timer at all.
uint64_t nextWakeMicros(uint64_t pollMicros, bool timerRunning, int64_t remainingMs);

// An ambient poll wake runs to the end before it sleeps again: the Wi-Fi join (10 s), the note check (two requests,
// each with a 5 s socket timeout and a 20 s transfer budget: up to 50 s) and the Method book check (its budget is 90
// s). A step timer that ends inside that would have its alert made late by the poll, so such a poll is skipped: the
// reader sleeps again for the timer's end (rearm), and the poll waits for the next sleep. Sleeping costs less than
// waiting out the 150 s awake.
//
// The join is 10 s only with no network profile installed. A profile has a budget of its own before the saved network's
// (10 s, or 25 s for EAP-TLS), so the poll's worst case is pollBudgetMs(netjoin::wakeJoinWorstMs(...)): 160 s with an
// open or PSK profile, 175 s with an EAP-TLS one. POLL_BUDGET_MS is the reader with no profile, to the millisecond.
//
// A Register poll (ambient/RegisterWake.h) is inside the same budget: the join, one bounded request for the schedule
// where the note makes two, and the book check. Its boundary wakes join nothing, but arrive as the same timer wake and
// are skipped by the same rule.
constexpr int64_t POLL_JOIN_MS = 10000;
constexpr int64_t POLL_BUDGET_MS = POLL_JOIN_MS + 50000 + 90000;
constexpr int64_t pollBudgetMs(int64_t joinMs) { return joinMs + 50000 + 90000; }
constexpr bool pollFitsBeforeEnd(bool timerRunning, int64_t remainingMs, int64_t budgetMs = POLL_BUDGET_MS) {
  return !timerRunning || remainingMs > budgetMs;
}

// ---- Booting with a timer on the card ---------------------------------------------------------------------------

enum class BootAction : uint8_t {
  None,         // nothing to do; a running timer carries on
  Acknowledge,  // an alerted timer found by someone using the reader: clear it
  AlertPulse,   // the timer ended while the reader slept: alert now, light pulsing
  AlertQuiet,   // the timer is over and nobody was told (a late wake, or the reader lost power): say so, no pulsing
};

struct BootFacts {
  Phase phase = Phase::None;
  bool powerLost = false;  // power was lost since the record was written, so its end cannot be trusted
  int64_t remainingMs = 0;
  bool timerWake = false;  // the reader woke by itself (a timer, not a button, not a cold start)
};
BootAction planBoot(const BootFacts& facts);

// ---- A boot where the card will not mount --------------------------------------------------------------------------

enum class NoCardPlan : uint8_t {
  ShowAlert,      // the timer is over (or about to be): say so without the card
  SleepForTimer,  // an ambient wake, the timer still ahead: sleep again, woken for its end
  SleepQuietly,   // an ambient wake with nothing to say: sleep again as the card-less wake always did
  ShowError,      // someone woke the reader: the "SD card error" screen
};
// What a boot does when the card will not mount (after it has been tried again). The alert does not need the card: it
// draws with the built-in fonts on a clean screen, from the record in NVS.
constexpr NoCardPlan noCardPlan(BootAction action, bool timerWake, bool timerRunning, int64_t remainingMs) {
  if (action == BootAction::AlertPulse || action == BootAction::AlertQuiet) return NoCardPlan::ShowAlert;
  if (!timerWake) return NoCardPlan::ShowError;
  return timerRunning && remainingMs > WAKE_SLACK_MS ? NoCardPlan::SleepForTimer : NoCardPlan::SleepQuietly;
}

// ---- The alert's light ------------------------------------------------------------------------------------------

struct LightFrame {
  bool on = false;
  bool ended = false;  // the alert has run its limit: light off, no more pulsing
};
// The frontlight `elapsedMs` after the alert began: three off/on flashes, then steady on, with the flashes repeated
// every 30 seconds so an alert nobody has seen yet is still noticed. `quiet` keeps it dark. After ALERT_LIMIT_MS the
// light is off and `ended` is set.
LightFrame alertLight(int64_t elapsedMs, bool quiet);
// The light's brightness during an alert: the user's own level, kept between 30 and 70 percent.
uint8_t alertBrightness(uint8_t userPercent);
constexpr bool lightAllowed(uint16_t batteryPercent) { return batteryPercent >= MIN_ALERT_BATTERY_PERCENT; }

// ---- The running timer ------------------------------------------------------------------------------------------

// One reading of the clocks, taken by the caller: the monotonic hardware counter in microseconds (it restarts at every
// boot and nothing sets it) and the wall clock in milliseconds (it can be stepped by a sync, and is whatever the reader
// last had when it was never synced).
struct Now {
  int64_t monoUs = 0;
  int64_t wallMs = 0;
};

// What a timer needs the clocks for, over a Record the caller owns (the one persisted with the rest of the sleep
// state). Holds one thing the Record cannot: the end on this boot's counter, which is why none of its answers need a
// clock of their own. Everything is a function of the Now it is given, so the sleep, wake and clock-step cases are
// covered by host tests with a fake clock.
class Clockwork {
 public:
  explicit Clockwork(Record& record) : rec_(record) {}

  const Record& record() const { return rec_; }
  bool running() const { return rec_.phase == Phase::Running; }

  // First thing after boot, with the record loaded: puts the persisted wall-clock end on this boot's counter.
  // `powerLost` is the reset reason saying power came back this boot (a cold start, a brownout): the wall clock
  // restarted from wherever it could, so the end means nothing. A wall clock that has gone back, so the end is further
  // away than the whole timer, is taken the same way.
  BootFacts boot(const Now& now, bool powerLost, bool timerWake);

  // Time left, in ms, as of `monoUs` (negative once past the end). Only meaningful while running.
  int64_t remainingMs(int64_t monoUs) const;

  // Starts a timer, replacing any other.
  void start(const std::string& bookPath, unsigned page, uint32_t seconds, const std::string& label, const Rect& box,
             const Now& now, unsigned panelPage);
  // No timer any more (stopped, or acknowledged).
  void clear();
  // The end has been shown.
  void markAlerted();

  // Going to sleep: rewrites the wall-clock end from the counter, because the wall clock may have been stepped by a
  // sync since the boot or the start, and records which page is on the panel (0 when not known). Nothing else runs
  // while the reader sleeps, so across the sleep the wall clock is exact.
  void beforeSleep(const Now& now, unsigned panelPage);

  // The wake timer to arm, given the ambient poll's (zero for none).
  uint64_t wakeMicros(uint64_t pollMicros, int64_t monoUs) const;

 private:
  Record& rec_;
  int64_t endMonoUs_ = 0;  // this boot's counter value at the end
};

// ---- What the panel says while the reader sleeps ----------------------------------------------------------------

// A countdown frozen on a sleeping panel would only mislead, so the panel says when the timer ends, or, when the clock
// cannot be trusted to say that, how long was left when the reader slept. Always in words with the time, never bare
// figures.
enum class SleepingKind : uint8_t {
  EndsAt,       // "Ends 2:32 PM": `value` is the time of day (as the reader writes the time, am/pm included)
  LeftAtSleep,  // "About 4:10 left at sleep": `value` is the time left when the reader slept
};
struct SleepingLine {
  SleepingKind kind = SleepingKind::EndsAt;
  std::string value;
};
// `endsAt` is the end as a time of day, empty when the clock cannot be trusted.
SleepingLine sleepingLine(const std::string& endsAt, int64_t remainingMs);

// Where the sleeping panel's timer is drawn, by what the frame holds (the reader's own page as the last full render
// drew it, which a page turn still waiting to be drawn has not caught up with, and a question open over it does not
// hide). Whenever a timer runs and the frame holds a page, the panel is about to keep a frame with its countdown in it,
// so the countdown is replaced: by the sleeping line in the timer's own box when the frame shows its page and the box
// fits, in the chip otherwise. Nothing is drawn only when there is no countdown on the frame to leave (no timer, or a
// frame that is another screen's).
enum class SleepOverlay : uint8_t { None, Box, Chip };
constexpr SleepOverlay sleepOverlay(bool timerRunning, bool childOpen, bool frameHasPage, bool frameShowsTimersPage,
                                    bool boxFits) {
  if (!timerRunning || childOpen || !frameHasPage) return SleepOverlay::None;
  return frameShowsTimersPage && boxFits ? SleepOverlay::Box : SleepOverlay::Chip;
}

// The end of a timer as a time of day is written to the minute, and a minute that is only partly over must not be
// the one promised: "Ends 2:32 PM" for an end at 2:32:45 would have the alert come 45 seconds after the panel's time.
// So the wall-clock end is first rounded UP to a whole minute (exactly on the minute stays), and that is what is
// written. (The alert can still be a second or so after the end itself: the wall clock reads whole seconds.)
int64_t roundUpToMinute(int64_t wallMs);

// ---- Starting the alert -----------------------------------------------------------------------------------------

// One alert per timer, and only once it is up does the timer count as alerted. A screen that restarts the reader as it
// is left (Wi-Fi screens, downloads) can do so between "the alert was requested" and "the alert is on screen"; if the
// timer had been marked alerted by then, the next boot would take the alert for one already shown and clear it. So the
// timer is marked alerted by the alert itself as it comes up, and this gate stops a second request in the meantime.
class AlertGate {
 public:
  bool pending() const { return pending_; }
  void request() { pending_ = true; }   // an alert has been created and will come up
  void release() { pending_ = false; }  // it is up, or it was dropped before it was
 private:
  bool pending_ = false;
};

// Should the main loop start an alert for the running timer now? Not while the screen on the panel is one that must not
// be replaced (`screenBusy`): USB Drive, which owns the card; the crash report, which holds the reason the reader
// restarted; the SD recovery and firmware picker, which may be writing the firmware. The timer stays running and over;
// the loop asks again on every pass, so the alert comes up as soon as that screen is left, and a sleep from it wakes
// for the over timer at once.
constexpr bool alertWanted(bool timerRunning, int64_t remainingMs, bool alertPending, bool screenBusy) {
  return timerRunning && timerDue(remainingMs) && !alertPending && !screenBusy;
}

// Is `stored` still the timer an alert was made for? The alert comes up in the main loop's next step, after the screen
// under it has had its turn, and a tap on the timer's box in that turn can stop the timer and start it again (the
// question's "Stop" and the next tap, or "Replace"). The alert must then leave the new timer alone: it marks alerted
// only a timer that is running, in the same book, on the same page, for the same time, ending where the alert's copy
// ends. A restart ends at least the timer's length later (a second at the least), and the end is kept in whole seconds
// of the wall clock, so "where it ends" allows the half second a rewrite of the end can move it.
constexpr int64_t SAME_END_TOLERANCE_MS = 500;
bool sameTimer(const Record& madeFor, const Record& stored);

// ---- Starting and stopping --------------------------------------------------------------------------------------

enum class StartDecision : uint8_t {
  Start,        // nothing runs: start it
  AlreadyHere,  // this very timer is the one running: a tap means stop
  AskReplace,   // another timer runs: ask before stopping it for this one
};
StartDecision decideStart(const Record& running, std::string_view bookPath, unsigned page);

// ---- Geometry ---------------------------------------------------------------------------------------------------

constexpr int MIN_TARGET_W = 120;  // 64 px rows and 120 px wide choices are the least that suits a gloved finger
constexpr int MIN_TARGET_H = 64;

// The small chip other pages show, over the free middle of the header row, and the larger banner an alert uses when
// the page on the panel is not the timer's own page.
Rect chipRect();
Rect bannerRect();
// A visual that may be as small as 24 x 24 gets a touch target of at least 120 x 64 around the same centre, kept on
// the page.
Rect touchTarget(const Rect& visual);

// How a box (or the banner) is divided: the 3 px frame, an optional label row on top, an optional hint row at the
// bottom, and the clock digits in what is left. Rows appear only where the box is tall enough to leave the digits
// room.
struct BoxLayout {
  Rect frame;
  int frameWidth = 0;
  Rect label;
  Rect digits;
  Rect hint;
};
BoxLayout layoutBox(const Rect& box);

// The chip: the clock digits on the left, the label after them.
struct ChipLayout {
  Rect frame;
  int frameWidth = 0;
  Rect digits;
  Rect label;
};
ChipLayout layoutChip(const Rect& chip);

// The chip's row when its time is words, not block digits ("Ends 2:32 PM", "About 4:10 left at sleep"): the text first,
// as wide as it is up to 85 % of the row, then the timer's label in what is left (empty when under 40 px remain).
struct ChipSplit {
  Rect time;
  Rect label;
};
ChipSplit splitChipRow(const Rect& row, int timeTextWidth);

// The first of `count` candidate sizes (widest first) whose text fits `areaW` x `areaH`; the last when none does.
int pickFont(const int* widths, const int* heights, int count, int areaW, int areaH);

// Clock figures drawn as blocks, large and the same width every digit ("mono, lining figures"): `text` holds digits and
// colons (anything else is a gap). The rectangles to fill are appended to `out`, as large as fits `area` and centred in
// it. Returns the figures' height, or 0 (and adds nothing) when even MIN_GLYPH_H would not fit the area.
constexpr int MIN_GLYPH_H = 14;
int clockGlyphs(std::string_view text, const Rect& area, std::vector<Rect>& out);
// True when `text` can be drawn readably in `area`. A page timer box too small for its own clock (the contract allows
// a box of 24 x 24) is not drawn over; the chip shows the countdown instead.
bool clockFits(std::string_view text, const Rect& area);

}  // namespace steptimer
