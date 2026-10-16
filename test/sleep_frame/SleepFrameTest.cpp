#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "CrossPointState.h"
#include "SleepFrame.h"
#include "StepTimer.h"

namespace {
int checks = 0;
void check(bool condition, const char* name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

using sleepframe::Frame;
using sleepframe::Wake;

const std::string METHOD_BOOK = "/Lamina/Method/Example column clean-up 1a2b3c4d.xtc";
const std::string OTHER_BOOK = "/Books/Lab Handbook.epub";

sleepframe::SleepInputs in(bool method, bool fromReader, const std::string& path, bool quickResume, bool ambient) {
  return {method, fromReader, path, quickResume, ambient};
}

// The persisted pieces of one reader plus the panel, enough to walk a whole
// sleep -> timer wake -> re-sleep -> button wake sequence with the same
// decisions main.cpp makes. The ambient note is the "current" one the SD card
// holds; the panel is whatever is physically showing.
struct Device {
  std::string panel;  // what the e-ink retains
  std::string openEpubPath;
  bool lastSleepFromReader = false;
  bool methodStepOnPanel = false;
  bool sleepFrameFile = false;
  uint8_t readerLoadCount = 0;
  std::string committedNote;  // ambient state on the card
  std::string route;          // where the last button wake went
};

struct Settings {
  bool quickResume = false;  // Sleep Screen = Quick Resume
  bool ambientOn = true;     // ambient is the sleep screen
  bool methodEnabled = true;
};

// enterDeepSleep(): decide, persist the marker, paint the frame. `openInReader` is the book the open reader holds
// (ActivityManager::readerBookPath()); by default it is the remembered path, as it is once the first page is drawn.
void sleepFrom(Device& d, const Settings& s, bool inReader, const std::string& page,
               const std::optional<std::string>& openInReader = std::nullopt) {
  d.lastSleepFromReader = inReader;
  d.readerLoadCount = 0;  // the reader's onExit
  const std::string reader = !inReader ? std::string() : openInReader ? *openInReader : d.openEpubPath;
  const auto book = sleepframe::resolveSleepBook(s.methodEnabled, inReader, d.openEpubPath, reader);
  const bool ambientAvailable =
      !sleepframe::methodStepOpen(s.methodEnabled, inReader, book.path) && s.ambientOn && !d.committedNote.empty();
  const auto plan = sleepframe::planSleep(in(s.methodEnabled, inReader, book.path, s.quickResume, ambientAvailable));
  if (book.remember) d.openEpubPath = book.path;
  d.methodStepOnPanel = plan.methodStep;
  d.sleepFrameFile = plan.frame == Frame::QuickResume;
  switch (plan.frame) {
    case Frame::QuickResume:
      d.panel = page + "+moon";
      break;
    case Frame::Ambient:
      d.panel = "ambient:" + d.committedNote;
      break;
    case Frame::Normal:
      d.panel = "sleep-screen";
      break;
  }
}

// setup() on an ambient timer wake. serverNote is the newest note the mailbox holds.
void timerWake(Device& d, const std::string& serverNote) {
  const bool fetchedNew = serverNote != d.committedNote;
  const auto plan = sleepframe::planWake(Wake::Timer, d.methodStepOnPanel, fetchedNew);
  if (plan.redraw) {
    d.panel = "ambient:" + serverNote;
    d.committedNote = serverNote;
  } else if (plan.commitFetched) {
    d.committedNote = serverNote;  // saved, not shown
  }
  d.methodStepOnPanel = plan.markerAfter;
}

// setup() on a power-button wake: clear the marker, route, reopen the book.
void buttonWake(Device& d, bool backHeld = false) {
  d.methodStepOnPanel = sleepframe::planWake(Wake::Button, d.methodStepOnPanel, false).markerAfter;
  d.sleepFrameFile = false;  // loadSleepFrameBuffer() consumes it
  if (sleepframe::resumesBook(d.openEpubPath, d.lastSleepFromReader, backHeld, d.readerLoadCount)) {
    d.route = "reader:" + d.openEpubPath;
    // main.cpp clears the remembered path and counts the open; the reader sets it again after its first paint.
    const std::string path = d.openEpubPath;
    d.openEpubPath.clear();
    ++d.readerLoadCount;
    d.panel = "page";  // the reader repaints the page it saved
    d.openEpubPath = path;
  } else {
    d.route = "home";
    d.panel = "home";
  }
}

// ---- A step timer running while the reader sleeps in a Method book
// -------------------------------------------------------- The same order of decisions as main.cpp: the timer is looked
// at before the ambient poll; every re-sleep that draws nothing asks rearmMicros, whose hook wakes for the timer's end
// as well.
struct TimedReader {
  Device d;
  steptimer::Record rec;
  steptimer::Clockwork cw{rec};
  int64_t trueMs = 0;  // real time
  int64_t wallOffsetMs = 1'790'000'000'000LL;
  int64_t bootTrueMs = 0;
  uint64_t armedUs = 0;
  std::string alertOn;  // what the last alert drew over: "" none, "retained:<panel>" or "clean"
  int alerts = 0;
  int skippedPolls = 0;  // polls left for the next sleep because the timer ends inside them

  steptimer::Now now() const { return {(trueMs - bootTrueMs) * 1000 + 1'500'000, trueMs + wallOffsetMs}; }
};
TimedReader* g_reader = nullptr;
uint64_t timerHook(uint64_t poll) {
  g_reader->cw.beforeSleep(g_reader->now(), g_reader->rec.panelPage);
  return g_reader->cw.wakeMicros(poll, g_reader->now().monoUs);
}

// enterDeepSleep() / a re-sleep: the wake timer to arm.
void arm(TimedReader& r, uint64_t poll, bool afterAmbientWake) {
  sleepframe::setWakeHook(timerHook);
  // enterDeepSleep arms the poll as it is; a re-sleep after a wake that drew nothing goes through rearmMicros (battery
  // 80 %).
  r.armedUs = afterAmbientWake ? sleepframe::rearmMicros(poll, 80) : timerHook(poll);
}

// setup() on a wake after `sleptMs` of real time: the order main.cpp uses. `timer` is whether the wake timer woke it.
steptimer::BootAction wakeTimed(TimedReader& r, int64_t sleptMs, bool timer, const std::string& serverNote) {
  r.trueMs += sleptMs;
  r.bootTrueMs = r.trueMs;
  const steptimer::BootAction action = steptimer::planBoot(r.cw.boot(r.now(), false, timer));
  const bool alert = action == steptimer::BootAction::AlertPulse || action == steptimer::BootAction::AlertQuiet;
  if (alert) {
    // The ambient poll is skipped; the wake paints like a button wake (marker cleared), the alert goes over the
    // retained frame.
    r.d.methodStepOnPanel = sleepframe::planWake(Wake::Button, r.d.methodStepOnPanel, false).markerAfter;
    r.alertOn = r.d.sleepFrameFile ? "retained:" + r.d.panel : "clean";
    r.d.sleepFrameFile = false;
    ++r.alerts;
    r.cw.markAlerted();
  } else if (timer && !steptimer::pollFitsBeforeEnd(r.cw.running(), r.cw.remainingMs(r.now().monoUs))) {
    ++r.skippedPolls;  // main.cpp: sleep again for the timer's end; the poll waits
  } else if (timer) {
    timerWake(r.d, serverNote);
  }
  return action;
}
}  // namespace

int main() {
  // --- Which sleeps count as "a Method step is open" ---
  check(sleepframe::methodStepOpen(true, true, METHOD_BOOK), "method book in the reader");
  check(!sleepframe::methodStepOpen(true, false, METHOD_BOOK), "stale remembered path after leaving the book");
  check(!sleepframe::methodStepOpen(false, true, METHOD_BOOK), "no Method feature on this board");
  check(!sleepframe::methodStepOpen(true, true, OTHER_BOOK), "other book");
  check(!sleepframe::methodStepOpen(true, true, ""), "nothing remembered yet (sleep before the first paint)");
  for (const auto* bad : {"/Lamina/Method/.incoming.xtc", "/Lamina/Method/sub/book.xtc",
                          "/Lamina/Method/../Books/book.xtc", "/Lamina/Method/book.epub", "/Lamina/Methods/book.xtc",
                          "Lamina/Method/book.xtc", "/Lamina/Method/", "/Lamina/Method/a\xC2\xB5.xtc"})
    check(!sleepframe::methodStepOpen(true, true, bad), "path outside the Method library");
  // ownedBookPath caps paths below 96 bytes: a longer file is not treated as a Method book.
  check(sleepframe::methodStepOpen(true, true, std::string("/Lamina/Method/") + std::string(76, 'a') + ".xtc"),
        "longest owned path (95 bytes)");
  check(!sleepframe::methodStepOpen(true, true, std::string("/Lamina/Method/") + std::string(77, 'a') + ".xtc"),
        "96-byte path is not owned");

  // --- planSleep: a Method step always keeps the page; others keep today's rules ---
  for (const bool quickResume : {false, true}) {
    for (const bool ambient : {false, true}) {
      const auto plan = sleepframe::planSleep(in(true, true, METHOD_BOOK, quickResume, ambient));
      check(plan.frame == Frame::QuickResume && plan.methodStep, "method step overrides every setting and ambient");
    }
  }
  struct Row {
    bool quickResume, ambient;
    Frame expected;
  };
  for (const Row row : {Row{false, false, Frame::Normal}, Row{true, false, Frame::QuickResume},
                        Row{false, true, Frame::Ambient}, Row{true, true, Frame::Ambient}}) {
    for (const auto* path : {OTHER_BOOK.c_str(), "", METHOD_BOOK.c_str()}) {
      // Not in a reader (Home, shelf, settings): the remembered path, even a Method one, changes nothing.
      const bool fromReader = path == OTHER_BOOK;
      const auto plan = sleepframe::planSleep(in(true, fromReader, path, row.quickResume, row.ambient));
      check(plan.frame == row.expected && !plan.methodStep, "non-Method sleeps behave as before");
    }
  }
  check(sleepframe::planSleep(in(false, true, METHOD_BOOK, false, true)).frame == Frame::Ambient,
        "boards without Method keep the ambient override");

  // --- planWake ---
  {
    const auto held = sleepframe::planWake(Wake::Timer, true, true);
    check(!held.redraw && held.commitFetched && held.markerAfter,
          "timer wake over a step: save, don't draw, keep marker");
    const auto heldNothing = sleepframe::planWake(Wake::Timer, true, false);
    check(!heldNothing.redraw && !heldNothing.commitFetched && heldNothing.markerAfter,
          "timer wake over a step, nothing new: keep marker");
    const auto free = sleepframe::planWake(Wake::Timer, false, true);
    check(free.redraw && !free.commitFetched && !free.markerAfter, "timer wake with no step on the panel draws");
    const auto freeNothing = sleepframe::planWake(Wake::Timer, false, false);
    check(!freeNothing.redraw && !freeNothing.commitFetched && !freeNothing.markerAfter, "nothing new: nothing drawn");
    for (const bool marker : {false, true}) {
      for (const bool fetched : {false, true}) {
        const auto button = sleepframe::planWake(Wake::Button, marker, fetched);
        check(!button.markerAfter && !button.redraw && !button.commitFetched, "button wake clears the marker");
        const auto other = sleepframe::planWake(Wake::Other, marker, fetched);
        check(!other.markerAfter && !other.redraw && !other.commitFetched, "cold boot or restart clears the marker");
      }
    }
  }

  // --- resumesBook: the button-wake route ---
  check(sleepframe::resumesBook(METHOD_BOOK, true, false, 0), "wake reopens the book");
  check(!sleepframe::resumesBook("", true, false, 0), "no remembered book: Home");
  check(!sleepframe::resumesBook(METHOD_BOOK, false, false, 0), "last sleep not from a reader: Home");
  check(!sleepframe::resumesBook(METHOD_BOOK, true, true, 0), "Back held at wake: Home");
  check(!sleepframe::resumesBook(METHOD_BOOK, true, false, 1), "reader open never finished: Home (crash guard)");

  // --- The sequence that matters: sleep inside a Method book, a note arrives, the device wakes on its timer
  //     twice, then the user presses the power button. The step must stay and the book must come back. ---
  {
    Settings settings;  // ambient on, Sleep Screen not Quick Resume: today the weather would win
    Device d;
    d.openEpubPath = METHOD_BOOK;
    d.committedNote = "note-1";
    d.panel = "page";

    sleepFrom(d, settings, true, "page");
    check(d.panel == "page+moon", "sleeping in a Method book keeps the page, with the moon");
    check(d.methodStepOnPanel && d.sleepFrameFile, "marker set and the quick-resume frame saved");

    timerWake(d, "note-2");
    check(d.panel == "page+moon", "timer wake with a new note leaves the step on the panel");
    check(d.committedNote == "note-2", "the new note is still fetched and saved");
    check(d.methodStepOnPanel, "marker survives the timer wake");
    check(d.openEpubPath == METHOD_BOOK && d.lastSleepFromReader && d.sleepFrameFile,
          "timer wake leaves the quick-resume state alone");

    // Re-sleep after the timer wake: setup() goes straight back to deep sleep without touching the panel or state.
    timerWake(d, "note-2");
    check(d.panel == "page+moon" && d.methodStepOnPanel, "second timer wake, nothing new: step and marker stay");
    timerWake(d, "note-3");
    check(d.panel == "page+moon" && d.committedNote == "note-3" && d.methodStepOnPanel,
          "third timer wake with another note: still the step");

    buttonWake(d);
    check(d.route == "reader:" + METHOD_BOOK, "button wake reopens the same Method book");
    check(!d.methodStepOnPanel, "button wake clears the marker");
    check(!d.sleepFrameFile, "the quick-resume frame was consumed by the wake");

    // Normal ambient behaviour is back: sleep from the same book again and wake on the timer.
    sleepFrom(d, settings, true, "page");
    check(d.methodStepOnPanel && d.panel == "page+moon", "sleeping in the book again protects the step again");
    // Leave the book for Home: the remembered path is stale, so this sleep shows ambient and clears the marker.
    buttonWake(d);
    sleepFrom(d, settings, false, "home");
    check(d.panel == "ambient:note-3" && !d.methodStepOnPanel, "sleeping from Home shows the (newest) ambient note");
    timerWake(d, "note-4");
    check(d.panel == "ambient:note-4", "with no step on the panel a timer wake redraws as before");
  }

  // --- Control: a book outside /Lamina/Method keeps today's behaviour exactly ---
  {
    Settings settings;
    Device d;
    d.openEpubPath = OTHER_BOOK;
    d.committedNote = "note-1";
    sleepFrom(d, settings, true, "page");
    check(d.panel == "ambient:note-1" && !d.methodStepOnPanel && !d.sleepFrameFile,
          "other book: the ambient override still wins");
    timerWake(d, "note-2");
    check(d.panel == "ambient:note-2", "other book: timer wake redraws");
    buttonWake(d);
    check(d.route == "reader:" + OTHER_BOOK, "other book: button wake resumes as before");
  }
  {
    Settings settings;
    settings.ambientOn = false;
    settings.quickResume = true;
    Device d;
    d.openEpubPath = OTHER_BOOK;
    sleepFrom(d, settings, true, "page");
    check(d.panel == "page+moon" && !d.methodStepOnPanel && d.sleepFrameFile,
          "other book with Quick Resume and no ambient: quick resume, no marker");
  }

  // --- A Method book with the user's Sleep Screen set to Cover/Custom/Blank still keeps the step ---
  {
    Settings settings;
    settings.ambientOn = false;
    settings.quickResume = false;
    Device d;
    d.openEpubPath = METHOD_BOOK;
    sleepFrom(d, settings, true, "page");
    check(d.panel == "page+moon" && d.methodStepOnPanel && d.sleepFrameFile,
          "Method book overrides the sleep-screen setting without ambient");
  }

  // --- Wake without the button resuming the book (Back held) still clears the marker ---
  {
    Settings settings;
    Device d;
    d.openEpubPath = METHOD_BOOK;
    d.committedNote = "n";
    sleepFrom(d, settings, true, "page");
    buttonWake(d, /*backHeld=*/true);
    check(d.route == "home" && !d.methodStepOnPanel, "Back held: Home, marker cleared");
  }

  // --- Re-arming the ambient wake timer on the way back to sleep (timer re-sleep and ghost-wake re-sleep) ---
  {
    constexpr uint64_t POLL = 3600ull * 1000000ull;
    check(sleepframe::rearmMicros(POLL, 100) == POLL, "full battery keeps polling");
    check(sleepframe::rearmMicros(POLL, sleepframe::MIN_POLL_BATTERY_PERCENT) == POLL, "15 % still polls");
    check(sleepframe::rearmMicros(POLL, sleepframe::MIN_POLL_BATTERY_PERCENT - 1) == 0, "14 % stops polling");
    check(sleepframe::rearmMicros(0, 100) == 0, "polling off stays off");
    check(sleepframe::MIN_POLL_BATTERY_PERCENT == 15, "same threshold as the fetch guard in setup()");
  }

  // --- The SD card failing to mount on a timer wake must not paint over the step ---
  check(!sleepframe::showsSdError(Wake::Timer), "timer wake: straight back to sleep, panel untouched");
  check(sleepframe::showsSdError(Wake::Button) && sleepframe::showsSdError(Wake::Other),
        "button wake and cold boot still show the SD error");
  {
    Settings settings;
    Device d;
    d.openEpubPath = METHOD_BOOK;
    d.committedNote = "note-1";
    sleepFrom(d, settings, true, "page");
    const std::string before = d.panel;
    // setup(): Storage.begin() fails on a timer wake; showsSdError(Timer) is false, so nothing is drawn or saved.
    if (sleepframe::showsSdError(Wake::Timer)) d.panel = "SD card error";
    check(d.panel == before && d.methodStepOnPanel && d.sleepFrameFile, "step and marker survive an SD failure wake");
    buttonWake(d);
    check(d.route == "reader:" + METHOD_BOOK, "the book still resumes once the card is back");
  }

  // --- A sleep before the first page paints. ReaderActivity clears the remembered book on entry and sets it after the
  //     first paint (about a second), so for that long nothing remembered says "Method book"; the open reader's own
  //     path stands in for it, the sleep is a Method sleep, and the wake reopens the book. ---
  {
    check(!sleepframe::methodStepOpen(true, true, ""), "nothing remembered, no path given: not a Method sleep");
    check(sleepframe::resolveSleepBook(true, true, "", METHOD_BOOK).path == METHOD_BOOK &&
              sleepframe::resolveSleepBook(true, true, "", METHOD_BOOK).remember,
          "empty remembered path: the open reader's Method book stands in, and is remembered");
    const auto remembered = sleepframe::resolveSleepBook(true, true, OTHER_BOOK, METHOD_BOOK);
    check(remembered.path == OTHER_BOOK && !remembered.remember, "a remembered path always wins");
    const auto same = sleepframe::resolveSleepBook(true, true, METHOD_BOOK, METHOD_BOOK);
    check(same.path == METHOD_BOOK && !same.remember, "already remembered: nothing new to persist");
    const auto none = sleepframe::resolveSleepBook(true, false, "", "");
    check(none.path.empty() && !none.remember, "no reader, nothing remembered");
    const auto other = sleepframe::resolveSleepBook(true, true, "", OTHER_BOOK);
    check(other.path == OTHER_BOOK && !other.remember, "another book in that window is not remembered (as before)");
    check(!sleepframe::resolveSleepBook(false, true, "", METHOD_BOOK).remember, "no Method feature on this board");
    check(!sleepframe::resolveSleepBook(true, false, "", METHOD_BOOK).remember, "no reader on screen");
    for (const auto* bad : {"/Lamina/Method/.incoming.xtc", "/Lamina/Method/sub/book.xtc", "/Lamina/Method/book.epub"})
      check(!sleepframe::resolveSleepBook(true, true, "", bad).remember, "a path outside the Method library");

    Settings settings;  // ambient on, Sleep Screen not Quick Resume: today the weather would win
    Device d;
    d.openEpubPath.clear();  // ReaderActivity::onEnter just cleared it
    d.committedNote = "note-1";
    d.panel = "shelf";  // the first page has not reached the panel yet
    sleepFrom(d, settings, true, "shelf", METHOD_BOOK);
    check(d.panel == "shelf+moon" && d.methodStepOnPanel && d.sleepFrameFile,
          "quick sleep before the first paint: the panel keeps what it shows, with the moon, and the marker is set");
    check(d.openEpubPath == METHOD_BOOK && d.lastSleepFromReader, "the book is remembered for the wake");

    timerWake(d, "note-2");
    check(d.panel == "shelf+moon" && d.methodStepOnPanel && d.committedNote == "note-2",
          "a timer wake fetches the note but does not draw over it");
    buttonWake(d);
    check(d.route == "reader:" + METHOD_BOOK, "the button wake reopens the Method book");
    check(!d.methodStepOnPanel, "and clears the marker");
    check(d.openEpubPath == METHOD_BOOK && d.readerLoadCount == 1, "the reader remembers it again after its first page");

    // The crash guard holds: if that reader open never finishes (reboot, count still 1), the next wake lands on Home.
    Device crashed = d;
    crashed.openEpubPath.clear();  // main.cpp cleared it before starting the reader, which then crashed
    buttonWake(crashed);
    check(crashed.route == "home", "a resume that crashed lands on Home, not in a loop");

    // Back held at wake still means Home.
    Device held;
    held.committedNote = "note-1";
    sleepFrom(held, settings, true, "shelf", METHOD_BOOK);
    buttonWake(held, /*backHeld=*/true);
    check(held.route == "home" && !held.methodStepOnPanel, "Back held at wake: Home");

    // The same sleep with Quick Resume selected, or with ambient off, is the same sleep.
    for (const bool quickResume : {false, true}) {
      for (const bool ambientOn : {false, true}) {
        Settings other_settings;
        other_settings.quickResume = quickResume;
        other_settings.ambientOn = ambientOn;
        Device e;
        e.committedNote = "note-1";
        sleepFrom(e, other_settings, true, "shelf", METHOD_BOOK);
        check(e.panel == "shelf+moon" && e.methodStepOnPanel && e.sleepFrameFile && e.openEpubPath == METHOD_BOOK,
              "every setting: a Method book in that window keeps the panel");
      }
    }

    // Controls: other books, and boards without Method, keep today's behaviour exactly.
    Device o;
    o.committedNote = "note-1";
    sleepFrom(o, settings, true, "shelf", OTHER_BOOK);
    check(o.panel == "ambient:note-1" && !o.methodStepOnPanel && o.openEpubPath.empty(),
          "other book before its first paint: ambient, nothing remembered");
    buttonWake(o);
    check(o.route == "home", "other book before its first paint: the wake lands on Home, as before");
    Settings noMethod;
    noMethod.methodEnabled = false;
    Device n;
    n.committedNote = "note-1";
    sleepFrom(n, noMethod, true, "shelf", METHOD_BOOK);
    check(n.panel == "ambient:note-1" && !n.methodStepOnPanel && n.openEpubPath.empty(),
          "a board without Method keeps the ambient override");
    Device h;  // Home with a stale remembered path and no reader
    h.openEpubPath = METHOD_BOOK;
    h.committedNote = "note-1";
    sleepFrom(h, settings, false, "home");
    check(h.panel == "ambient:note-1" && !h.methodStepOnPanel, "not in a reader: ambient, as before");
  }

  // --- A step timer ends while the reader sleeps in a Method book, with ambient polls in between ---
  {
    Settings settings;  // ambient on and Sleep Screen not Quick Resume: today the weather would win
    TimedReader r;
    g_reader = &r;
    const uint64_t POLL = 5ull * 60 * 1000000;
    r.d.committedNote = "note-1";
    r.d.openEpubPath = METHOD_BOOK;
    r.d.panel = "step4";
    r.cw.start(METHOD_BOOK, 4, 20 * 60, "Incubate", steptimer::Rect{32, 540, 416, 156}, r.now(), 4);
    r.trueMs += 20'000;  // the user reads on, then presses Power
    sleepFrom(r.d, settings, true, "step4");
    arm(r, POLL, false);
    check(r.d.panel == "step4+moon" && r.d.methodStepOnPanel,
          "a Method sleep with a timer running keeps the step on the panel");
    check(r.armedUs == POLL, "20 minutes out: the first wake is the 5 minute poll");

    // Polls at 5, 10 and 15 minutes: each fetches the note, draws nothing, and re-sleeps for the next poll or the
    // timer's end.
    for (int poll = 1; poll <= 3; ++poll) {
      const std::string note = "note-" + std::to_string(poll + 1);
      check(wakeTimed(r, static_cast<int64_t>(r.armedUs / 1000), true, note) == steptimer::BootAction::None,
            "an ambient poll mid-timer");
      check(r.d.panel == "step4+moon" && r.d.methodStepOnPanel && r.d.committedNote == note && r.alerts == 0,
            "the poll saves the note but does not touch the step or the marker");
      r.trueMs += 25'000;  // Wi-Fi, a clock sync that steps the wall clock, the fetch
      if (poll == 1) r.wallOffsetMs += 2 * 3600 * 1000LL;
      arm(r, POLL, true);
    }
    check(r.armedUs < POLL && r.armedUs > 60ull * 1000000,
          "after the third poll the next wake is the timer's end, not a fourth poll");

    // The timer's wake. The server holds a newer note, but the alert comes first and the poll waits.
    check(wakeTimed(r, static_cast<int64_t>(r.armedUs / 1000), true, "note-9") == steptimer::BootAction::AlertPulse,
          "the wake at the end is the alert");
    check(r.alerts == 1 && r.alertOn == "retained:step4+moon", "drawn over the retained frame the sleep left");
    check(r.d.committedNote == "note-4", "the poll was skipped for this wake");
    check(!r.d.methodStepOnPanel, "the wake repaints the panel, so the marker is cleared");
    const int64_t lateMs = r.trueMs - 20 * 60 * 1000;  // the wake is set a little early; the alert waits out the rest
    check(lateMs > -5000 && lateMs < 1000, "and it comes within a second of the end whatever the clock steps did");

    // A press: the book, on the page it was on.
    buttonWake(r.d);  // the press ends the alert; the resume is a wake's
    check(r.d.route == "reader:" + METHOD_BOOK, "a press goes back into the book");
    check(!r.cw.running() && r.rec.phase == steptimer::Phase::Alerted, "the timer is over");
    // Sleeping again does not alert again, and the poll is back to being the only wake.
    sleepFrom(r.d, settings, true, "step4");
    arm(r, POLL, false);
    check(r.armedUs == POLL, "nothing but the poll now");
    check(
        wakeTimed(r, static_cast<int64_t>(POLL / 1000), true, "note-9") == steptimer::BootAction::None && r.alerts == 1,
        "an ambient poll over an alerted timer does not alert again");
    check(wakeTimed(r, 60'000, false, "") == steptimer::BootAction::Acknowledge,
          "whoever wakes the reader next clears it");
    sleepframe::setWakeHook(nullptr);
  }

  // --- A poll that would run into the timer's end is left for the next sleep, so the alert is on time ---
  {
    Settings settings;
    TimedReader r;
    g_reader = &r;
    const uint64_t POLL = 5ull * 60 * 1000000;
    r.d.committedNote = "note-1";
    r.d.openEpubPath = METHOD_BOOK;
    r.cw.start(METHOD_BOOK, 4, 10 * 60, "Incubate", steptimer::Rect{32, 540, 416, 156}, r.now(), 4);
    sleepFrom(r.d, settings, true, "step4");
    arm(r, POLL, false);
    check(wakeTimed(r, static_cast<int64_t>(r.armedUs / 1000), true, "note-2") == steptimer::BootAction::None &&
              r.skippedPolls == 0 && r.d.committedNote == "note-2",
          "a poll with 5 minutes to the end of a 10 minute timer fits, and runs");
    r.trueMs += 25'000;
    arm(r, POLL, true);
    check(r.armedUs < POLL, "the end is nearer than the next poll");
  }
  {
    Settings settings;
    TimedReader r;
    g_reader = &r;
    const uint64_t POLL = 5ull * 60 * 1000000;
    r.d.committedNote = "note-1";
    r.d.openEpubPath = METHOD_BOOK;
    r.cw.start(METHOD_BOOK, 4, 6 * 60, "Incubate", steptimer::Rect{32, 540, 416, 156}, r.now(),
               4);  // ends 60 s after the poll
    sleepFrom(r.d, settings, true, "step4");
    arm(r, POLL, false);
    check(wakeTimed(r, static_cast<int64_t>(r.armedUs / 1000), true, "note-2") == steptimer::BootAction::None,
          "the poll wake itself is no alert");
    check(r.skippedPolls == 1 && r.d.committedNote == "note-1" && r.d.panel == "step4+moon" && r.d.methodStepOnPanel,
          "its poll is skipped (60 s is inside the 150 s a poll can take), and the step and marker are as they were");
    arm(r, POLL,
        true);  // rearm(0) in main.cpp is the hook's answer for no poll; here the poll is armed and the end is sooner
    check(wakeTimed(r, static_cast<int64_t>(r.armedUs / 1000), true, "note-2") == steptimer::BootAction::AlertPulse &&
              r.alerts == 1,
          "the next wake is the alert");
    const int64_t late = r.trueMs - 6 * 60 * 1000;
    check(late > -5000 && late < 1000, "and it is on time instead of a poll's length late");
    sleepframe::setWakeHook(nullptr);
  }
  // --- The timer ends while the reader sleeps on Home: nothing retained to draw over ---
  {
    Settings settings;
    TimedReader r;
    g_reader = &r;
    r.d.committedNote = "note-1";
    r.d.openEpubPath = METHOD_BOOK;
    r.cw.start(METHOD_BOOK, 4, 120, "Incubate", steptimer::Rect{32, 540, 416, 156}, r.now(), 4);
    sleepFrom(r.d, settings, false, "home");
    arm(r, 0, false);
    check(r.d.panel == "ambient:note-1" && !r.d.methodStepOnPanel && !r.d.sleepFrameFile,
          "Home sleeps on the ambient screen");
    check(wakeTimed(r, static_cast<int64_t>(r.armedUs / 1000), true, "note-1") == steptimer::BootAction::AlertPulse,
          "the timer still wakes the reader");
    check(r.alertOn == "clean", "with no saved frame the alert is a clean screen, not a patch on the weather");
    buttonWake(r.d);
    check(r.d.route == "home", "and a press goes where a wake from Home goes");
    sleepframe::setWakeHook(nullptr);
  }

  // --- A wake by hand after the end, and a wake after power was lost ---
  {
    Settings settings;
    TimedReader r;
    g_reader = &r;
    r.d.committedNote = "note-1";
    r.d.openEpubPath = METHOD_BOOK;
    r.cw.start(METHOD_BOOK, 4, 60, "Incubate", steptimer::Rect{32, 540, 416, 156}, r.now(), 4);
    sleepFrom(r.d, settings, true, "step4");
    arm(r, 0, false);
    check(wakeTimed(r, 300'000, false, "") == steptimer::BootAction::AlertQuiet,
          "pressed 4 minutes after the end: said quietly");
    TimedReader lost;
    g_reader = &lost;
    lost.cw.start(METHOD_BOOK, 4, 3600, "Incubate", steptimer::Rect{32, 540, 416, 156}, lost.now(), 4);
    lost.cw.beforeSleep(lost.now(), 4);
    lost.trueMs += 1000;
    lost.bootTrueMs = lost.trueMs;
    check(steptimer::planBoot(lost.cw.boot(lost.now(), true, false)) == steptimer::BootAction::AlertQuiet,
          "power lost: quiet");
    sleepframe::setWakeHook(nullptr);
  }

  // --- The marker really persists in state.json, and an old state file (no key) means "no step on the panel" ---
  {
    auto& state = CrossPointState::getInstance();
    state.openEpubPath = METHOD_BOOK;
    state.lastSleepFromReader = true;
    state.showBootScreen = false;
    state.methodStepOnPanel = true;
    JsonDocument saved;
    state.toJson(saved);
    check(saved["methodStepOnPanel"].as<bool>(), "marker is written to state.json");
    std::string text;
    serializeJson(saved, text);

    state.methodStepOnPanel = false;
    state.openEpubPath.clear();
    JsonDocument loaded;
    check(deserializeJson(loaded, text) == DeserializationError::Ok, "state.json parses");
    check(state.fromJson(loaded.as<JsonVariantConst>()), "state.json loads");
    check(state.methodStepOnPanel && state.openEpubPath == METHOD_BOOK && state.lastSleepFromReader &&
              !state.showBootScreen,
          "marker and the rest of the quick-resume state round-trip together");

    JsonDocument legacy;
    check(deserializeJson(legacy, "{\"openEpubPath\":\"/Books/a.epub\",\"lastSleepFromReader\":true}") ==
              DeserializationError::Ok,
          "legacy state parses");
    state.methodStepOnPanel = true;
    check(state.fromJson(legacy.as<JsonVariantConst>()) && !state.methodStepOnPanel,
          "state.json from an older firmware means no Method step on the panel");
  }

  // --- rearmMicros: the ambient poll, gated by the battery, and whatever else the hook wants awake sooner ---
  {
    const uint64_t POLL = 300ull * 1000000;
    sleepframe::setWakeHook(nullptr);
    check(sleepframe::rearmMicros(POLL, 100) == POLL && sleepframe::rearmMicros(POLL, 15) == POLL &&
              sleepframe::rearmMicros(POLL, 14) == 0 && sleepframe::rearmMicros(0, 100) == 0,
          "no hook: the poll while the battery allows it");
    static uint64_t seenPoll = 12345;
    sleepframe::setWakeHook([](uint64_t poll) -> uint64_t {
      seenPoll = poll;
      return poll == 0 ? 90ull * 1000000 : std::min<uint64_t>(poll, 90ull * 1000000);
    });
    check(sleepframe::rearmMicros(POLL, 100) == 90ull * 1000000 && seenPoll == POLL,
          "the hook sees the poll and may wake sooner");
    check(sleepframe::rearmMicros(POLL, 5) == 90ull * 1000000 && seenPoll == 0,
          "a low battery stops the poll but not the hook: the step timer is the user's own");
    sleepframe::setWakeHook(nullptr);
    check(sleepframe::rearmMicros(POLL, 100) == POLL, "and with the hook removed it is the poll again");
  }

  std::cout << checks << " sleep-keeps-the-step checks passed\n";
}
