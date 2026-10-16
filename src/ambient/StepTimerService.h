#pragma once

#include <cstdint>
#include <string>

#include "StepTimer.h"

// The reader's one step timer: the pure Clockwork (StepTimer.h) over a record kept in NVS, fed with the real clocks.
//
// Why NVS and not state.json on the card. The timer has to be written down after the card has been shut down for
// sleep: an ambient poll that finishes while a timer runs re-sleeps through the Storage.prepareForDeepSleep() that
// powers the card off, and the timer's wall-clock end must be rewritten there when the poll's clock sync has stepped
// the clock. NVS stays writable; it survives a dead battery, it does not depend on the card being in at the wake, and
// it needs no 4 KB rewrite of state.json on every poll. (RTC memory was the other candidate: it does not survive power
// loss, and a timer that outlives a battery is the case worth handling.)
//
// Threading. Everything here runs on the main task. The reader's render task reads the record and remainingMs() while
// it draws, so the main task changes the timer (start, stop, markAlerted) only while holding the RenderLock, as it
// does for the reader's other state.
namespace steptimer {

// A timer that ended and was answered, kept in memory (not saved) so its box can say "Done · 2:32 PM" until the page
// changes: whatever dismissed the alert (a stray touch, a key meant for something else) must not erase the evidence
// that it ended.
struct DoneNote {
  std::string bookPath;
  uint16_t page = 0;  // 1-based; 0 means there is no note
  Rect box;
  std::string label;
  std::string endedAt;  // the time of day it ended, empty when that is not known
};

class Service {
 public:
  static Service& instance();

  const Record& record() const { return clock_.record(); }
  bool running() const { return clock_.running(); }

  // First thing after the clocks are up: reads the record from NVS and puts its end on this boot's counter.
  // `bootClocks` is the reading taken at the top of setup(), before anything could step the wall clock. Returns what
  // this boot must do.
  BootAction boot(const Now& bootClocks, bool timerWake);

  // The note of a timer that has ended and been answered (see DoneNote). Set under the RenderLock by the alert; read,
  // and cleared once the page it belongs to is left, by the render task, which holds the lock.
  const DoneNote& done() const { return done_; }
  void setDone(DoneNote note) { done_ = std::move(note); }
  void clearDone() { done_ = DoneNote(); }

  // True when the last boot() found the record from before a power loss.
  bool lostPower() const { return lostPower_; }

  // Time left, as of now. Only meaningful while running().
  int64_t remainingMs() const;
  Now now() const;

  // Each saves the record. The reader holds the RenderLock around them.
  void start(const std::string& bookPath, unsigned page, uint32_t seconds, const std::string& label, const Rect& box,
             unsigned panelPage);
  void clear();        // stopped, or acknowledged
  void markAlerted();  // the end has been shown

  // Writes the timer down as it is now: its wall-clock end is rewritten from the counter (the wall clock may have been
  // stepped by a sync since the boot), and the page on the panel is noted (0 = not known; -1 keeps what is recorded).
  // For the way into sleep, and before a restart that would lose the counter. Does nothing when no timer runs.
  void persist(int panelPage = -1);

  // On the way into sleep, from any path: persist(), and the wake timer to arm: the ambient poll's (zero for none) or
  // the timer's end, whichever is first.
  uint64_t rearm(uint64_t pollMicros, int panelPage = -1);

 private:
  Service() : clock_(record_) {}
  void save() const;
  void load();

  Record record_;
  Clockwork clock_;
  bool lostPower_ = false;
  DoneNote done_;
};

// The two clocks, read now.
Now readClocks();
// The reset reason says power came back this boot (a cold start or a brownout), so a wall clock and a timer from before
// mean nothing.
bool powerWasLost();

}  // namespace steptimer

#define STEP_TIMER (steptimer::Service::instance())
