#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// What stays on the panel while the reader sleeps, and what a wake may redraw.
// Pure logic: no Arduino, no storage, so the whole sleep -> timer wake ->
// re-sleep -> button wake sequence is covered by the host tests in
// test/sleep_frame. main.cpp and SleepActivity only gather the inputs and act
// on the answers.
//
// The rule for a Method book (a protocol the reader is showing at the bench):
// the step outranks everything. Sleeping inside one is always a Quick Resume
// sleep, whatever Settings > Sleep Screen says and even when the ambient
// (Messages/Weather/Agenda) screen is on, and nothing that runs while it sleeps
// (timer wakes, a failed SD mount, sleep-time plugin popups) may paint over the
// reader's current screen: the step, or the Steps list when that is what was
// open. Every other book keeps today's behaviour.
namespace sleepframe {

// What the panel shows while the device sleeps.
enum class Frame : uint8_t {
  Normal,       // whatever Settings > Sleep Screen selects (cover, custom, blank, ...)
  QuickResume,  // the current page stays on the panel, plus the moon mark
  Ambient,      // the Messages / Weather / Agenda frame
};

struct SleepInputs {
  bool methodEnabled;         // the Method feature exists on this board (plaque::Capabilities::method: Pro and Classic)
  bool fromReader;            // a reader was on screen when the sleep began
  std::string_view bookPath;  // the remembered open book; stale once the user has left it
  bool quickResumeSetting;    // Quick Resume selected, or "after timeout" and this sleep is a timeout
  bool ambientAvailable;      // ambient is the sleep screen and a cached frame exists
};

// The book a sleep is about, resolved once from the remembered path and the open reader's own.
struct SleepBook {
  std::string path;  // what methodStepOpen() and planSleep() look at (a copy, so nothing that runs later can move it)
  bool remember;     // persist `path` as the remembered book: the wake must reopen it
};

// A reader clears the remembered book while it opens one (a crash guard) and sets it again once the first page is on
// the panel, so for about a second after a book opens the remembered path is empty although a reader is already
// showing it. A sleep in that window would not be a Method sleep: the ambient frame would go over the page and the
// wake would land on Home. So an empty remembered path falls back to the reader's own, `openInReader` (empty when no
// reader is open). A remembered path always wins, so every other case is decided exactly as before.
//
// `remember` is true only for a Method book found that way, so that the button wake can reopen it. That is safe with
// the crash guard: a device that reaches sleep with the reader open did not crash loading it, and the wake itself
// re-arms the guard (main.cpp clears the path and counts the open before it starts the reader, so a book that
// crashes on resume lands on Home next boot). Other books in that window keep today's behaviour, Home on wake.
SleepBook resolveSleepBook(bool methodEnabled, bool fromReader, std::string_view remembered,
                           std::string_view openInReader);

// True when a Method book is what the user was reading: a reader is on screen
// and the remembered book is one of ours. The path alone is not enough, because
// it outlives the reader: leaving a book for Home keeps it.
bool methodStepOpen(bool methodEnabled, bool fromReader, std::string_view bookPath);

struct SleepPlan {
  Frame frame;
  // Persist as "the Method book's current screen stays on the panel": the marker
  // the timer-wake path reads before it draws anything. Assigned on every sleep,
  // never set-only.
  bool methodStep;
};

// Callers should not pay for the ambient frame check (a 48 KB read and hash)
// when methodStepOpen() already answers the question; pass ambientAvailable as
// false in that case, the result is the same.
SleepPlan planSleep(const SleepInputs& in);

// What woke the device. Timer wakes are the ambient polls and reboot through
// setup() like any other wake; Other covers cold boots, flashes and restarts.
enum class Wake : uint8_t { Timer, Button, Other };

struct WakePlan {
  bool redraw;         // the fetched frame may be drawn on the panel
  bool commitFetched;  // keep the fetched frame as the current one without drawing it
  bool markerAfter;    // what the persisted Method marker must hold afterwards
};

// A timer wake may always fetch, but while a Method step is on the panel the
// result is saved, not shown, and the marker is left exactly as found so the
// re-sleep (and every later timer wake) protects the step too. Any other wake
// repaints the panel itself, so the marker is cleared.
WakePlan planWake(Wake wake, bool methodStepOnPanel, bool fetchedNewFrame);

// A wake timer is only worth arming while the battery can afford the poll.
constexpr uint16_t MIN_POLL_BATTERY_PERCENT = 15;

// What else wants the reader awake at a given time. The step timer's end is the only such thing: it sets this once at
// boot, and the answer is the wake timer to arm given the ambient poll's (zero for none), which it also keeps the
// timer's own record up to date for. A function pointer, not a call into the timer, so this file stays free of the
// firmware and the host tests can stand in for the timer.
using WakeHook = uint64_t (*)(uint64_t pollMicros);
void setWakeHook(WakeHook hook);

// The wake timer to arm on the way back to sleep, from any wake: the ambient poll
// interval (zero when polling is off), or none when the battery is low, and sooner if
// the hook says something else is due first. The timer re-sleep and the ghost-wake
// re-sleep both use it, so a brief power bounce does not silently end scheduled
// polling, and a running step timer is woken for however the reader goes back to sleep.
uint64_t rearmMicros(uint64_t pollMicros, uint16_t batteryPercent);

// The SD card did not mount at boot. Only a timer wake stays quiet (straight back
// to sleep, no timer: the settings are unreadable) so the retained frame, which
// may be a Method step, is not painted over; a button wake shows the error.
bool showsSdError(Wake wake);

// Wake from sleep: reopen the remembered book (true) or land on Home (false).
// Home wins when no book is remembered, the last sleep was not from a reader,
// Back is held, or a previous reader open never finished (a crash guard).
bool resumesBook(std::string_view bookPath, bool lastSleepFromReader, bool backHeld, uint8_t readerLoadCount);

}  // namespace sleepframe
