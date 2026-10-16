#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "StepTimer.h"

// What a timer wake may do about Method books (Stage B), as plain decisions. The wake checks the mailbox's book
// list after the note check, in the same Wi-Fi session and clock window, and files what is new exactly as the shelf
// does (MethodCheck, MethodLibrary, the same high-water mark). This file holds the rules that make that safe to do
// unattended: the battery floor, the size cap, the time budget, the book on the panel, and the retry limit. Pure
// logic with no Arduino and no storage, covered by test/method_wake and test/method_check.
//
// A book is declined by stopping the run at it, never by skipping it: the mark only moves up, so filing a newer book
// past a declined one would leave the declined one behind the mark, and the shelf would never fetch it either. A
// declined book, and everything newer, waits for the shelf's next check.
namespace method::wake {

// ---- the wake budget -------------------------------------------------------------------
//
// A timer wake may stay awake for books for BUDGET_MS, counted from the wake itself (millis() restarts at every
// wake), so the time the note check took is already spent. The figure is the TLS clock window (90 s): the note check
// syncs the clock after the wake began, so the window outlasts the budget and a wake never needs a second sync.
//
// A book is started only when it fits in what is left at the planning rate:
//   needed = REQUEST_MS + ceil(bytes * 1000 / PLANNING_BYTES_PER_SEC) + FILING_MS
//   start  = elapsed + needed <= BUDGET_MS
// The budget is also the hard stop: a download still running when it ends is abandoned and its partial file removed.
//
// At 12 KB/s an 8-page book (about 384 KB) needs 8 + 32 + 3 = 43 s, so it starts while 47 s or more of the wake are
// left; the largest book that can start at all, in an empty wake, is 948,000 B (8 + 79 + 3 = 90 s); and a 1 MB book
// (94.3 s) never fits a 90 s wake, so the size cap below is the ceiling the budget would otherwise have set: after a
// note check of 20 s the largest book is about 700 KB.
constexpr uint32_t BUDGET_MS = 90000;
// A planning guess, not a measurement: no device has timed a download on a wake yet (Stage A's 10.5 KB/s was only an
// assumption written in a comment). A wake that turns out slower than the plan is stopped by the budget and by the
// retry limit; one that is faster just finishes early. To be calibrated from the first wake's `phase download` line
// in the serial log (the book's bytes over that phase's milliseconds), and changed here and in the tests.
constexpr uint32_t PLANNING_BYTES_PER_SEC = 12000;
constexpr uint32_t REQUEST_MS = 8000;  // name lookup, TLS handshake and headers of one request
constexpr uint32_t FILING_MS = 3000;   // after the last byte: digest, XTC check, rename, index write

constexpr int MIN_BATTERY_PERCENT = 30;   // below this a wake does not touch the books at all
constexpr uint32_t MAX_BYTES = 1000000;  // "1 MB": 20 pages of a book, decimal like the Worker's 1.9 MB cap (see above)
constexpr uint32_t MAX_BOOKS = 2;         // downloads per wake
constexpr uint8_t MAX_FAILED_WAKES = 2;   // a book whose download failed on this many wakes is left for the shelf

// A step timer that runs while the reader sleeps must still sound on time, and a poll wake that is busy with a book is
// the one thing that can make it late. So with a timer running, the budget is also cut to what leaves the timer its
// lead and a margin (below): the existing "no time" rules then stop the book check, or never start it, before the
// timer is due.
//
//   left = min(BUDGET_MS - elapsed, timerRemaining - WAKE_LEAD_MS - TIMER_MARGIN_MS)
//
// WAKE_LEAD_MS (2 s, StepTimer.h) is how early the alert's own wake is armed, because booting takes about that long.
// The margin is what a check can run on past its budget and what the way back to sleep takes:
//   - up to 5 s: a request that began just before the budget ended is inside its TLS handshake, which only the 5 s
//     socket timeout ends (a book that has just arrived is filed in 1 to 2 s instead, inside the same allowance);
//   - about 5 s: tearing down, the note redraw if there is one (display init and a full refresh, about 4 s), and
//     arming the next wake.
// 10 s covers both. In the worst case the reader is asleep with 2 s of the timer left, the alert's wake is armed for
// the minimum 1 s, and the alert sounds about a second late; with the usual overrun of well under a second it is
// on time.
constexpr int64_t TIMER_MARGIN_MS = 10000;

// What is left of the wake's budget `elapsedMs` into the wake, cut for a running step timer as above (timerRemainingMs
// is only read when timerRunning). 0 once nothing is left.
uint32_t budgetLeft(uint32_t elapsedMs, bool timerRunning, int64_t timerRemainingMs);

// How long past its budget the loop lets a check run before cancelling it. The check ends itself at the budget, bar the
// verify of a book that has just arrived (limited to 10 s); this only guards against a check that never ends. With a
// step timer running it is shorter, so even that guard stays inside the timer's margin.
constexpr uint32_t HARD_STOP_GRACE_MS = 15000;
constexpr uint32_t TIMER_HARD_STOP_GRACE_MS = 5000;
uint32_t hardStopGraceMs(bool timerRunning);

// What a check needs to know to behave as a wake check. The shelf passes none: `active` is false and nothing here
// applies.
struct Rules {
  bool active = false;
  uint32_t budgetMs = BUDGET_MS;  // for the whole check, counted from Check::start()
  int minBatteryPercent = MIN_BATTERY_PERCENT;
  uint32_t maxBytes = MAX_BYTES;
  uint32_t maxBooks = MAX_BOOKS;
  uint32_t bytesPerSec = PLANNING_BYTES_PER_SEC;
  uint32_t requestMs = REQUEST_MS;
  uint32_t filingMs = FILING_MS;
  uint32_t blockedBookId = 0;  // a book that failed on MAX_FAILED_WAKES wakes: the shelf's to fetch; 0 for none
  // The book a button wake would reopen. Filing a book never deletes it, whatever it displaces: the user's place in
  // the protocol on the panel is worth more than a newer version, which the shelf will fetch. Empty for none.
  std::string protectedPath;
};

// Wake rules for a check that has `remainingMs` of the wake budget left.
Rules rulesFor(uint32_t remainingMs, std::string protectedPath, uint32_t blockedBookId);

// ceil(bytes * 1000 / bytesPerSec), saturating.
uint32_t transferMs(const Rules& rules, uint32_t bytes);
// The time one download is planned to take, from the request to the filed book.
uint32_t neededMs(const Rules& rules, uint32_t bytes);
// What is left of the budget after `elapsedMs`; 0 once it is spent.
uint32_t remainingMs(const Rules& rules, uint32_t elapsedMs);

// ---- before anything is built ----------------------------------------------------------

// Is a book check worth starting at all? Decided before the library is read or a request is made, so that a wake that
// cannot use the books costs nothing, and the log says why.
enum class Gate : uint8_t {
  Run,
  Battery,  // under MIN_BATTERY_PERCENT: the list alone costs a TLS handshake
  NoTime,   // not enough budget left for one request
  NoClock,  // the note check failed and left no synced clock: HTTPS cannot start, and a second sync would fail too
};

struct GateInputs {
  int battery;
  uint32_t remainingMs;  // what budgetLeft() says is left of the wake
  bool https;
  bool clockReady;   // a clock synced within the last 90 s
  bool noteFailed;   // the note check ended Failed (it stops at a failed clock sync)
};

Gate gate(const Rules& rules, const GateInputs& in);

// ---- per book --------------------------------------------------------------------------

enum class Verdict : uint8_t {
  Download,
  Battery,    // under the floor now (the gauge was read again)
  Blocked,    // failed on MAX_FAILED_WAKES earlier wakes
  TooLarge,
  Protected,  // filing it would delete the book a button wake reopens
  Limit,      // MAX_BOOKS already started this wake
  NoTime,     // it would not fit in what is left of the budget
};

struct Request {
  uint32_t bookId;     // the number in "b12"
  uint32_t bytes;      // announced length
  int battery;
  uint32_t elapsedMs;  // since the check started
  uint32_t started;    // downloads already started this wake
  bool displacesProtected;
};

struct Decision {
  uint32_t bookId = 0;
  uint32_t bytes = 0;
  Verdict verdict = Verdict::Download;
  uint32_t neededMs = 0;
  uint32_t remainingMs = 0;
  int battery = 0;
  uint32_t started = 0;
};

// The rules in this order, so that the reason given is the most basic one: the battery, then what is wrong with the
// book (retried out, too large, would delete the page on the panel), then what is wrong with the wake (the count,
// the time).
Decision judge(const Rules& rules, const Request& request);

// ---- the retry limit -------------------------------------------------------------------

// A download that fails mid-way is attempted again on every wake, and each attempt costs the radio. Remember, across
// the deep sleeps between wakes, which book failed and how often; after MAX_FAILED_WAKES the wake leaves it to the
// shelf. Plain values so a device can keep them in RTC memory.
struct Backoff {
  uint32_t bookId = 0;
  uint8_t failures = 0;
};

uint32_t blockedId(const Backoff& backoff);                  // the book to leave alone, or 0
void noteFailure(Backoff& backoff, uint32_t bookId);         // a download of this book failed on a wake
void forgetHandled(Backoff& backoff, uint32_t mark);         // the mark has reached the book: nothing left to retry

// ---- when the user acts ----------------------------------------------------------------

// A wake is a poll the user did not ask for, and the panel may be showing the step they are working through. The
// book check looks at the hardware between its steps; a key, a touch or the Home key during it ends the wake as a
// button wake would, at once: the check is cancelled through its normal teardown (partial file removed, Wi-Fi off)
// and the boot carries on instead of sleeping again, so the user is back on the book, or Home, without pressing
// again. The one thing that can delay it is the TLS handshake inside a request, bounded by the 5 s socket timeout,
// as on the shelf.
//
// How long the boot waits for the press to end before it carries on (a tap is over in well under a second), so that
// its release is not read as an action by the screen the boot opens.
constexpr uint32_t RELEASE_WAIT_MS = 700;

// A reader face-down on a bench or in a pouch can report a touch at every poll, and a wake that took each of those for
// the user would boot fully and stay awake until it timed out. So a keypress is the hardware's level (as before: the
// debounced edge, or the ladder and power line read twice a few milliseconds apart), but a touch counts only when it is
// a new contact: one that began after the first reading (a contact already there at the first look is a phantom or a
// hand that was on the screen before the check began, and is ignored until it lifts), and that is still there at a
// reading at least TOUCH_CONFIRM_MS after the one that first saw it (a single-frame blip is not a finger).
//
// "Touch" is the screen and the capacitive Home key together: the caller reports whether any contact is down now.
constexpr uint32_t TOUCH_CONFIRM_MS = 50;

class TouchWatch {
 public:
  // One reading of the touch contact, `nowMs` on any steady clock. True from the reading that confirms a new contact.
  bool observe(uint32_t nowMs, bool contact);
  bool confirmed() const { return state_ == State::Confirmed; }
  // A new contact is waiting to be confirmed: how many more milliseconds until a reading can confirm it (0 when none
  // is waiting, or the window has already passed). For the last look before a wake ends, so that a finger that came
  // down in the last TOUCH_CONFIRM_MS is not lost.
  uint32_t waitMs(uint32_t nowMs) const;

 private:
  enum class State : uint8_t {
    Start,      // no reading yet
    Ignoring,   // a contact was there at the first reading: ignored until it lifts
    Idle,       // no contact
    Pending,    // a new contact, first seen at since_
    Confirmed,  // a new contact, seen again TOUCH_CONFIRM_MS or more after the first
  };
  State state_ = State::Start;
  uint32_t since_ = 0;
};

// The capacitive Home key's level. The SDK reports the key as edges only: a press, then a tap when it is released
// short, or a long press once it has been held past the threshold (after which its release is not a tap). So the key
// counts as down from the press edge to either of those. Call once after every input update with that update's edges.
class HomeKeyLevel {
 public:
  bool observe(bool pressedEdge, bool tapped, bool longPressed) {
    if (pressedEdge) down_ = true;
    if (tapped || longPressed) down_ = false;
    return down_;
  }
  bool down() const { return down_; }

 private:
  bool down_ = false;
};

// A key that has been held since the first look has no press edge until the input debounce (5 ms) has passed, and
// the debounce only commits at a later update. So the last look before a wake ends without a press comes at least
// KEY_SETTLE_MS after the look before it: a final look microseconds after the first would find nothing.
constexpr uint32_t KEY_SETTLE_MS = 8;
// How long to wait before a look that follows the previous look by `sincePollMs`.
uint32_t settleWaitMs(uint32_t sincePollMs);

class PressWatch {
 public:
  // One poll. keyEdge: the debounced press edge of a key. keyHeld, keyHeldAgain: the level of the button ladder and
  // power line, then read again a few milliseconds later (keyHeldAgain is only meaningful when keyHeld). touch: any
  // contact down now. True when the user pressed a key or touched the reader.
  bool observe(uint32_t nowMs, bool keyEdge, bool keyHeld, bool keyHeldAgain, bool touch);
  // How long to wait before the last look before the wake ends without a press: until a key held since the first look
  // can commit (settleWaitMs, `sincePollMs` after the previous look) and until a contact that has just come down can
  // be confirmed, whichever is longer.
  uint32_t lastLookWaitMs(uint32_t nowMs, uint32_t sincePollMs) const;

 private:
  TouchWatch touch_;
};

// A press that is still down when RELEASE_WAIT_MS has run out (a long hold) must not act when it is released, by the
// screen the boot opens. A finger still on the screen is suppressed for the rest of its contact, as the app does for
// a consumed touch long-press; a key still down is treated as a consumed long-press, whose release the app swallows.
// The power key is not one of these: its release is the main loop's own (wakePowerReleasePending).
struct Leftover {
  bool suppressTouch;
  bool consumeKeyReleases;
};
Leftover leftover(bool fingerDown, bool keyOtherThanPowerDown);

struct Ending {
  bool continueBoot;    // do not sleep again: carry on into the normal boot
  bool sleepWake;       // that boot is a wake from sleep: no splash, the book on the panel is reopened
  bool commitNote;      // keep the note this wake fetched without drawing it: the boot repaints the panel
  bool swallowRelease;  // the press that did this may still be down: its release is not an action
};

// What the rest of the wake does. Without a press: nothing changes, the wake draws or leaves the panel and sleeps
// again exactly as before books existed. With one: it is a button wake, and a note fetched on the way is kept (so the
// next poll does not fetch it again) but not drawn.
Ending ending(bool userActed, bool noteFetched);

// ---- words for the serial log ----------------------------------------------------------

const char* gateText(Gate gate);
const char* verdictText(Verdict verdict);
// "b12: 512344 B, download (needs 53696 ms, 61000 ms left)" and the like. Returns the length written.
size_t describe(const Rules& rules, const Decision& decision, char* out, size_t size);

}  // namespace method::wake
