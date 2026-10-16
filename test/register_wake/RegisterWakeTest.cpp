#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "RegisterChoice.h"
#include "RegisterWake.h"
#include "SleepFrame.h"
#include "StepTimer.h"

namespace {
int checks = 0;
void check(bool condition, const std::string& name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

void zone(const char* posix) {
  setenv("TZ", posix, 1);
  tzset();
}
constexpr char NEW_YORK[] = "EST5EDT,M3.2.0,M11.1.0";

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

constexpr uint64_t SEC = 1000000;

// The Stage C hook, standing in for STEP_TIMER: the real arithmetic over a timer the test sets.
struct FakeTimer {
  bool running = false;
  int64_t remainingMs = 0;
};
FakeTimer timer;
uint64_t timerHook(uint64_t pollMicros) { return steptimer::nextWakeMicros(pollMicros, timer.running, timer.remainingMs); }

reg::WakeInputs inputs() {
  reg::WakeInputs in;
  in.now = at(2026, 10, 14, 14, 0);
  in.pollEnabled = true;
  in.pollIntervalS = 15 * 60;
  in.pollDueAt = in.now + 10 * 60;
  in.boundaryWakes = true;
  in.boundaryAt = 0;
  in.batteryPercent = 80;
  return in;
}

reg::Entry book(const char* title, int64_t start, int64_t end) { return {title, start, end}; }
}  // namespace

int main() {
  zone(NEW_YORK);
  sleepframe::setWakeHook(timerHook);

  // ---- the poll alone --------------------------------------------------------------------------------------------
  {
    timer = {};
    reg::WakeInputs in = inputs();
    reg::WakePlan p = reg::planWake(in);
    check(p.micros == 600 * SEC && p.reason == reg::WakeFor::Poll && p.pollDueAt == in.now + 600, "poll due in 10 minutes");
    in.pollDueAt = in.now + 30;
    p = reg::planWake(in);
    check(p.micros == 60 * SEC && p.pollDueAt == in.now + 60, "due sooner than the minimum gap: waits the gap");
    in.pollDueAt = in.now - 3600;
    p = reg::planWake(in);
    check(p.micros == 60 * SEC && p.pollDueAt == in.now + 60, "overdue (the reader was awake): polls in a minute, not at once");
    in.pollDueAt = 0;
    p = reg::planWake(in);
    check(p.micros == 60 * SEC && p.pollDueAt == in.now + 60, "never planned: a minute");
    in.pollDueAt = in.now + 6 * 3600;
    p = reg::planWake(in);
    check(p.micros == (900 + 33) * SEC, "a planned time beyond one interval (the clock moved): one interval and the slack (15 s and 2 %)");
    in.now = 0;
    p = reg::planWake(in);
    check(p.micros == 900 * SEC && p.pollDueAt == 0, "no clock: one interval from now");
    in = inputs();
    in.pollEnabled = false;
    p = reg::planWake(in);
    check(p.micros == 0 && p.reason == reg::WakeFor::Nothing && p.pollDueAt == 0, "polling off, no boundary: no wake");
  }

  // ---- is this wake the poll's? ----------------------------------------------------------------------------------
  {
    reg::WakeInputs in = inputs();
    check(!reg::pollDue(in), "a poll 10 minutes off: a boundary wake");
    in.pollDueAt = in.now + 34;
    check(!reg::pollDue(in), "34 seconds early: not yet (a 15 minute interval allows 15 s and 2 %, 33 s)");
    in.pollDueAt = in.now + 33;
    check(reg::pollDue(in), "33 seconds early: the poll's");
    in.pollDueAt = in.now - 5;
    check(reg::pollDue(in), "late: the poll's");
    in.pollDueAt = 0;
    check(reg::pollDue(in), "nothing planned (a reset cleared it): poll");
    in.pollDueAt = inputs().now + 600;
    in.now = 0;
    check(reg::pollDue(in), "no clock: poll (it syncs one)");
    in = inputs();
    in.pollDueAt = in.now + 900 + 33 + 1;
    check(reg::pollDue(in), "planned further off than an interval: the clock has moved, poll");
    in.pollDueAt = in.now + 900 + 33;
    check(!reg::pollDue(in), "exactly an interval and the slack: fine");
    in = inputs();
    in.pollEnabled = false;
    in.pollDueAt = in.now - 100;
    check(!reg::pollDue(in), "polling off: never a poll wake");
    check(reg::pollDueAfter(1000, 900) == 1900 && reg::pollDueAfter(0, 900) == 0, "after a poll: one interval on, or nothing");

    // The slack scales with the interval: the wake timer counts on the internal clock, which runs up to 2 % off, so a
    // fixed 15 seconds missed the poll's own wake at the longer intervals the settings allow (up to 1440 minutes).
    check(reg::pollSlackSeconds(0) == 15 && reg::pollSlackSeconds(-5) == 15 && reg::pollSlackSeconds(300) == 21 &&
              reg::pollSlackSeconds(900) == 33 && reg::pollSlackSeconds(3600) == 87 && reg::pollSlackSeconds(86400) == 1743,
          "15 s and 2 % of the interval");
    for (const int64_t minutes : {5, 15, 30, 60, 240, 1440}) {
      reg::WakeInputs w = inputs();
      w.pollIntervalS = minutes * 60;
      w.pollDueAt = w.now + w.pollIntervalS;
      // The wake armed for the poll fires 2 % early on a fast internal clock: still the poll's.
      reg::WakeInputs early = w;
      early.now = w.now + w.pollIntervalS * 98 / 100;
      check(reg::pollDue(early), std::to_string(minutes) + " minutes: a wake 2 % early is the poll's");
      // And a wake far from the poll's time is a boundary wake (the slack is not the whole interval).
      reg::WakeInputs far = w;
      far.now = w.now + w.pollIntervalS / 2;
      check(!reg::pollDue(far), std::to_string(minutes) + " minutes: half way is not the poll's");
      // A planned time further off than an interval and its slack means the clock has moved.
      reg::WakeInputs moved = w;
      moved.pollDueAt = w.now + w.pollIntervalS + reg::pollSlackSeconds(w.pollIntervalS) + 1;
      check(reg::pollDue(moved), std::to_string(minutes) + " minutes: a poll planned beyond an interval and its slack: poll");
    }
  }

  // ---- the boundary ----------------------------------------------------------------------------------------------
  {
    timer = {};
    reg::WakeInputs in = inputs();
    in.boundaryAt = in.now + 180;
    reg::WakePlan p = reg::planWake(in);
    check(p.micros == (180 - reg::VIEW_LEAD_S) * SEC && p.reason == reg::WakeFor::Boundary, "boundary in 3 minutes: that, less the lead");
    in.pollEnabled = false;
    p = reg::planWake(in);
    check(p.micros == (180 - reg::VIEW_LEAD_S) * SEC && p.reason == reg::WakeFor::Boundary && p.pollDueAt == 0,
          "with polling off the boundary still wakes");
    in.boundaryAt = in.now + 3;
    p = reg::planWake(in);
    check(p.micros == 1 * SEC, "a boundary a moment away: never under a second");
    in.boundaryAt = in.now;
    p = reg::planWake(in);
    check(p.micros == 1 * SEC && p.reason == reg::WakeFor::Boundary, "a boundary already past (the screen has not caught up): the shortest wake");
    in.boundaryAt = in.now - 600;
    p = reg::planWake(in);
    check(p.micros == 1 * SEC, "however long ago it was");
    in.boundaryAt = 0;
    p = reg::planWake(in);
    check(p.micros == 0, "no boundary at all, polling off: no wake");
    in.boundaryAt = in.now + 600;
    in.boundaryWakes = false;
    p = reg::planWake(in);
    check(p.micros == 0 && p.reason == reg::WakeFor::Nothing, "a Method step on the panel: no boundary wake");
    in.now = 0;
    in.boundaryWakes = true;
    in.boundaryAt = at(2026, 10, 14, 15);
    p = reg::planWake(in);
    check(p.micros == 0, "no clock: no boundary wake (and polling is off here)");
  }

  // ---- the poll and the boundary together: the soonest -----------------------------------------------------------
  {
    timer = {};
    reg::WakeInputs in = inputs();
    in.pollDueAt = in.now + 600;
    in.boundaryAt = in.now + 300;
    reg::WakePlan p = reg::planWake(in);
    check(p.micros == (300 - reg::VIEW_LEAD_S) * SEC && p.reason == reg::WakeFor::Boundary, "boundary before the poll: the boundary");
    check(p.pollDueAt == in.now + 600, "and the poll keeps its place");
    in.boundaryAt = in.now + 900;
    p = reg::planWake(in);
    check(p.micros == 600 * SEC && p.reason == reg::WakeFor::Poll, "poll before the boundary: the poll");
    in.boundaryAt = in.now + 600 + reg::VIEW_LEAD_S;
    p = reg::planWake(in);
    check(p.micros == 600 * SEC && p.reason == reg::WakeFor::Poll, "a tie: reported as the poll");
    // The wake after a boundary one second before a poll is due is a poll wake, and does both.
    in = inputs();
    in.pollDueAt = in.now + 10;
    check(reg::pollDue(in), "10 seconds from the poll: it is the poll's");
  }

  // ---- with a running step timer ---------------------------------------------------------------------------------
  {
    reg::WakeInputs in = inputs();
    in.pollDueAt = in.now + 600;
    in.boundaryAt = in.now + 300;
    timer = {true, 120 * 1000};  // ends in 2 minutes
    reg::WakePlan p = reg::planWake(in);
    check(p.micros == (120 - 2) * SEC && p.reason == reg::WakeFor::StepTimer, "timer ends before both: the timer, less its 2 s lead");
    timer = {true, 400 * 1000};
    p = reg::planWake(in);
    check(p.micros == (300 - reg::VIEW_LEAD_S) * SEC && p.reason == reg::WakeFor::Boundary, "timer after the boundary: the boundary");
    timer = {true, 700 * 1000};
    in.boundaryAt = in.now + 900;
    p = reg::planWake(in);
    check(p.micros == 600 * SEC && p.reason == reg::WakeFor::Poll, "timer after the poll and the boundary: the poll");
    // Only the timer.
    in.pollEnabled = false;
    in.boundaryWakes = false;
    timer = {true, 90 * 1000};
    p = reg::planWake(in);
    check(p.micros == 88 * SEC && p.reason == reg::WakeFor::StepTimer, "polling off, no boundary: the timer alone");
    // Everything at once, three different times, each winning in turn.
    in = inputs();
    in.pollDueAt = in.now + 500;
    in.boundaryAt = in.now + 400 + reg::VIEW_LEAD_S;  // wakes in 400 s
    timer = {true, 300 * 1000 + 2000};                // wakes in 300 s
    p = reg::planWake(in);
    check(p.micros == 300 * SEC && p.reason == reg::WakeFor::StepTimer, "all three: the timer is soonest");
    timer = {true, 450 * 1000 + 2000};
    p = reg::planWake(in);
    check(p.micros == 400 * SEC && p.reason == reg::WakeFor::Boundary, "all three: the boundary is soonest");
    in.boundaryAt = in.now + 600 + reg::VIEW_LEAD_S;
    p = reg::planWake(in);
    check(p.micros == 450 * SEC && p.reason == reg::WakeFor::StepTimer, "all three: the timer again");
    timer = {true, 900 * 1000};
    p = reg::planWake(in);
    check(p.micros == 500 * SEC && p.reason == reg::WakeFor::Poll, "all three: the poll is soonest");
    // A timer that is over: the floor.
    timer = {true, 0};
    p = reg::planWake(in);
    check(p.micros == steptimer::MIN_WAKE_US && p.reason == reg::WakeFor::StepTimer, "a timer at its end: the minimum wake");
    timer = {};
  }

  // ---- battery ---------------------------------------------------------------------------------------------------
  {
    reg::WakeInputs in = inputs();
    in.boundaryAt = in.now + 300;
    in.batteryPercent = sleepframe::MIN_POLL_BATTERY_PERCENT;
    timer = {};
    check(reg::planWake(in).micros == (300 - reg::VIEW_LEAD_S) * SEC, "15 %: the ambient wake stays");
    in.batteryPercent = sleepframe::MIN_POLL_BATTERY_PERCENT - 1;
    reg::WakePlan p = reg::planWake(in);
    check(p.micros == 0 && p.reason == reg::WakeFor::Nothing, "14 %: no poll and no boundary wake");
    timer = {true, 200 * 1000};
    p = reg::planWake(in);
    check(p.micros == 198 * SEC && p.reason == reg::WakeFor::StepTimer, "14 % with a step timer: the timer still wakes");
    timer = {};
  }

  // ---- the model's boundaries through the wake: midnight, an all-day booking, a stale schedule, a clock change ----
  {
    timer = {};
    const reg::Options o = [] {
      reg::Options x;
      x.twelveHour = true;
      x.pollMinutes = 15;
      return x;
    }();
    reg::Schedule s;
    s.instrument = "X";
    s.timezone = "America/New_York";
    s.status = reg::Status::Ok;
    s.fetchedAt = at(2026, 10, 14, 23, 50);
    s.today.clear();
    reg::WakeInputs in = inputs();
    in.pollEnabled = false;
    in.now = at(2026, 10, 14, 23, 52);
    // Midnight rollover: nothing booked, the reading goes stale in 35 minutes, midnight comes first.
    in.boundaryAt = reg::nextBoundary(s, reg::viewTime(in.now), o);
    check(in.boundaryAt == at(2026, 10, 15, 0), "midnight is the next boundary");
    reg::WakePlan p = reg::planWake(in);
    check(p.micros == (8 * 60 - reg::VIEW_LEAD_S) * SEC, "midnight rollover: wakes 4 s before midnight");

    // An all-day booking: today it is in use; the boundaries are its end (midnight) and the stale time.
    reg::Schedule allDay = s;
    allDay.fetchedAt = at(2026, 10, 14, 8, 55);
    allDay.hasNow = true;
    allDay.now = book("Maintenance", at(2026, 10, 14, 0), at(2026, 10, 15, 0));
    allDay.now.allDay = true;
    allDay.today = {allDay.now};
    in.now = at(2026, 10, 14, 9);
    in.boundaryAt = reg::nextBoundary(allDay, reg::viewTime(in.now), o);
    check(in.boundaryAt == allDay.fetchedAt + reg::staleAfterSeconds(15) + 1, "all day: first the reading going stale");
    in.boundaryAt = reg::nextBoundary(allDay, reg::viewTime(in.now), reg::Options{true, 1440, false});
    check(in.boundaryAt == at(2026, 10, 15, 0), "all day, polled rarely: its end, at midnight (no wake in between)");

    // A stale schedule has no stale-time boundary left; midnight remains.
    reg::Schedule stale = s;
    stale.status = reg::Status::Stale;
    in.now = at(2026, 10, 14, 23, 52);
    in.boundaryAt = reg::nextBoundary(stale, reg::viewTime(in.now), o);
    check(in.boundaryAt == at(2026, 10, 15, 0), "a stale schedule: midnight only");

    // Daylight saving: at 01:30 EST on the spring-forward day, a booking starts at 03:00 EDT, half an hour of real time on.
    zone(NEW_YORK);
    reg::Schedule dst = s;
    dst.fetchedAt = at(2026, 3, 8, 1, 0);
    dst.today = {book("After the change", at(2026, 3, 8, 3), at(2026, 3, 8, 4))};
    in.now = at(2026, 3, 8, 1, 30);
    in.boundaryAt = reg::nextBoundary(dst, reg::viewTime(in.now), reg::Options{true, 1440, false});
    check(in.boundaryAt == at(2026, 3, 8, 3), "dst: the booking's start");
    p = reg::planWake(in);
    check(p.micros == (30 * 60 - reg::VIEW_LEAD_S) * SEC, "dst: 30 minutes of real time between 01:30 and 03:00");
    // Fall back: 01:30 EDT (first) to a booking at 02:00 EST is 90 real minutes.
    reg::Schedule fall = s;
    fall.fetchedAt = at(2026, 11, 1, 0, 30);
    fall.today = {book("After the repeat", at(2026, 11, 1, 2), at(2026, 11, 1, 3))};
    in.now = at(2026, 11, 1, 0, 45);
    in.boundaryAt = reg::nextBoundary(fall, reg::viewTime(in.now), reg::Options{true, 1440, false});
    check(in.boundaryAt == at(2026, 11, 1, 2), "fall back: the booking's start");
    p = reg::planWake(in);
    check(in.boundaryAt - in.now == 2 * 3600 + 15 * 60, "(00:45 EDT to 02:00 EST is 2 hours 15 minutes)");
    check(p.micros == (2 * 3600 + 15 * 60 - reg::VIEW_LEAD_S) * SEC, "fall back: the wake counts real seconds");
  }

  // ---- the wake after a wake: the sequence of a day --------------------------------------------------------------
  {
    // Ana 13:00-15:00, Sam 16:00-18:00, polling every 15 minutes, a 60-minute-limit schedule so it stays fresh.
    timer = {};
    reg::Schedule s;
    s.instrument = "X";
    s.timezone = "America/New_York";
    s.status = reg::Status::Ok;
    s.fetchedAt = at(2026, 10, 14, 14, 5);
    s.today = {book("Ana", at(2026, 10, 14, 13), at(2026, 10, 14, 15)), book("Sam", at(2026, 10, 14, 16), at(2026, 10, 14, 18))};
    s.hasNow = true;
    s.now = s.today[0];
    const reg::Options o{true, 60, false};
    reg::WakeInputs in = inputs();
    in.pollIntervalS = 60 * 60;
    in.now = at(2026, 10, 14, 14, 5);
    in.pollDueAt = at(2026, 10, 14, 15, 5);
    // Walk the wakes the plan asks for until the poll comes up.
    std::string seen;
    for (int step = 0; step < 8; ++step) {
      in.boundaryAt = reg::nextBoundary(s, reg::viewTime(in.now), o);
      const reg::WakePlan p = reg::planWake(in);
      if (p.micros == 0) break;
      in.now += static_cast<int64_t>(p.micros / SEC) + 2;  // the wake, and the boot
      if (reg::pollDue(in)) {
        seen += "P";
        break;
      }
      seen += "B";
    }
    // Boundaries at 15:00 (Ana ends), 16:00 (Sam starts); the stale time is 16:10; the poll is at 15:05.
    check(seen == "BP", "the day: a boundary wake at 15:00, then the 15:05 poll");
  }

  // ---- a boundary that falls between the decision and the arming (the reviewer's repro) --------------------------
  {
    timer = {};
    const int64_t B = at(2026, 10, 14, 15, 0);  // Ana's booking ends
    reg::Schedule sc;
    sc.instrument = "Scope";
    sc.timezone = "America/Detroit";
    sc.status = reg::Status::Ok;
    sc.fetchedAt = at(2026, 10, 14, 14, 40);
    const reg::Entry ana{"Ana", at(2026, 10, 14, 13), B, false, false};
    const reg::Entry sam{"Sam", at(2026, 10, 14, 17), at(2026, 10, 14, 18), false, false};
    sc.hasNow = true;
    sc.now = ana;
    sc.today = {ana, sam};
    sc.hasNext = true;
    sc.next = sam;
    const reg::Options o{true, 15, false};
    reg::Words w;
    // The screen is decided five seconds before the end: it still says In use.
    const int64_t decided = reg::viewTime(B - 5);
    check(reg::buildModel(sc, decided, o, w).state == reg::State::InUse, "decided at B-5 s: in use");
    for (int64_t armedAt : {B - 5, B - 4, B - 2, B + 3, B + 30}) {
      reg::WakeInputs in;
      in.now = armedAt;
      in.pollEnabled = true;
      in.pollIntervalS = 900;
      in.pollDueAt = armedAt + 850;
      in.boundaryWakes = true;
      in.batteryPercent = 80;
      // What main.cpp did before: ask for the boundary after `now` alone. At B-4 s that is already Sam's start.
      const int64_t naive = reg::nextBoundary(sc, reg::viewTime(armedAt), o);
      in.boundaryAt = reg::boundaryForArm(sc, decided, armedAt, o);
      const reg::WakePlan p = reg::planWake(in);
      check(in.boundaryAt == B, "armed at B" + std::string(armedAt < B ? "-" : "+") + std::to_string(std::llabs(armedAt - B)) + " s: the boundary is Ana's end");
      check(p.reason == reg::WakeFor::Boundary && p.micros <= 2 * SEC + (armedAt < B - 4 ? (B - armedAt) * SEC : 0), "and it wakes at once (or on time), not at the poll");
      if (armedAt >= B - 4) check(naive != B, "(the naive arm had lost it)");
      // The wake redraws: the screen at the wake says Free.
      const int64_t wakes = armedAt + static_cast<int64_t>(p.micros / SEC) + 2;
      check(reg::buildModel(sc, reg::viewTime(wakes), o, w).state != reg::State::InUse || wakes < B - 4, "the redraw at the wake is past Ana's booking");
    }
    // Nothing decided this boot (a ghost wake): only now's view.
    reg::WakeInputs g;
    g.now = B - 30;
    g.boundaryWakes = true;
    g.batteryPercent = 80;
    g.boundaryAt = reg::boundaryForArm(sc, 0, g.now, o);
    check(g.boundaryAt == B, "no earlier decision: the boundary after now");
    // A decision later than now (a clock that stepped back) is not used.
    check(reg::boundaryForArm(sc, reg::viewTime(B + 100), B - 30, o) == B, "a decision from the future is ignored");
    check(reg::boundaryForArm(sc, decided, 0, o) == 0, "no clock: no boundary");
  }

  // ---- the sleep-screen choices ------------------------------------------------------------------------------------
  {
    reg::SleepChoice c;
    reg::Toggled t = reg::toggleRegister(c);
    check(t.choice.registerScreen && t.choice.polling && t.choice.pollMinutes == 15 && t.pollingTurnedOn, "Register on with polling off: polling on, 15 minutes, and the screen can say so");
    c.polling = true;
    c.pollMinutes = 30;
    t = reg::toggleRegister(c);
    check(t.choice.registerScreen && t.choice.polling && t.choice.pollMinutes == 30 && !t.pollingTurnedOn, "polling already on: its interval is left alone");
    c.pollMinutes = 5;
    check(reg::toggleRegister(c).choice.pollMinutes == 5, "(5 minutes stays 5)");
    c = {};
    c.pollMinutes = 45;  // polling off, an interval from long ago
    check(reg::toggleRegister(c).choice.pollMinutes == 15, "polling off: 15 minutes, whatever interval was left");
    c = {};
    c.message = true;
    check(!reg::toggleRegister(c).choice.message && reg::toggleRegister(c).choice.registerScreen, "Register on clears the message screen");
    c = {};
    c.registerScreen = true;
    c.polling = true;
    c.pollMinutes = 30;
    t = reg::toggleRegister(c);
    check(!t.choice.registerScreen && t.choice.polling && t.choice.pollMinutes == 30 && !t.pollingTurnedOn, "Register off leaves polling as it is");
    check(!reg::toggleMessage(c).registerScreen && reg::toggleMessage(c).message, "the message screen on clears Register");
    reg::SleepChoice m;
    m.message = true;
    check(!reg::toggleMessage(m).message, "the message screen off");
    reg::SleepChoice both;
    both.message = true;
    both.registerScreen = true;
    check(reg::normalised(both).registerScreen && !reg::normalised(both).message, "both set in the file: Register is kept");
    both.registerScreen = false;
    check(reg::normalised(both).message, "only the message screen: unchanged");
    check(!reg::normalised(reg::SleepChoice{}).message && !reg::normalised(reg::SleepChoice{}).registerScreen, "neither: unchanged");
  }

  // ---- the card will not mount on a wake the Register armed ------------------------------------------------------
  {
    timer = {};
    reg::CardRetry r = reg::planCardRetry(true, 0, 80);
    check(r.micros == static_cast<uint64_t>(reg::CARD_RETRY_S) * SEC && r.retries == 1, "armed, first failure: try again in five minutes");
    check(reg::CARD_RETRY_S >= 300 && reg::CARD_RETRY_S <= 600, "(between five and ten minutes)");
    check(reg::CARD_RETRY_S == 300 && reg::CARD_RETRY_LIMIT == 12, "five minutes, twelve times: an hour (pinned)");
    r = reg::planCardRetry(true, 5, 80);
    check(r.micros == 300 * SEC && r.retries == 6, "and again");
    r = reg::planCardRetry(true, reg::CARD_RETRY_LIMIT - 1, 80);
    check(r.micros == 300 * SEC && r.retries == reg::CARD_RETRY_LIMIT, "the last try");
    r = reg::planCardRetry(true, reg::CARD_RETRY_LIMIT, 80);
    check(r.micros == 0 && r.retries == reg::CARD_RETRY_LIMIT, "after an hour of failures it stops, as before");
    r = reg::planCardRetry(false, 0, 80);
    check(r.micros == 0 && r.retries == 0, "Register did not arm this wake: nothing (the old behaviour)");
    r = reg::planCardRetry(true, 0, sleepframe::MIN_POLL_BATTERY_PERCENT - 1);
    check(r.micros == 0, "under 15 %: no retry, like every ambient wake");
    // A running step timer keeps its own wake.
    timer = {true, 120 * 1000};
    r = reg::planCardRetry(true, 0, 80);
    check(r.micros == 118 * SEC, "a step timer ending sooner than the retry: its end");
    r = reg::planCardRetry(false, 0, 80);
    check(r.micros == 118 * SEC, "and not armed: still the step timer's end");
    timer = {true, 900 * 1000};
    r = reg::planCardRetry(true, 0, 80);
    check(r.micros == 300 * SEC, "a step timer later than the retry: the retry");
    r = reg::planCardRetry(true, reg::CARD_RETRY_LIMIT, 80);
    check(r.micros == 898 * SEC, "retries spent: the step timer alone");
    timer = {};
  }

  // ---- a whole afternoon, wake by wake: when the panel is drawn ----------------------------------------------------
  {
    timer = {};
    // The reader is put to sleep at 12:00. The mailbox answers every poll with the same three bookings, "read" 30 s
    // before the poll. Polling every 15 minutes. The loop is what main.cpp does: wake, poll if due, redraw if the
    // screen would change, plan the next wake.
    const int64_t bookings[3][2] = {{at(2026, 10, 14, 13), at(2026, 10, 14, 15)},
                                    {at(2026, 10, 14, 16), at(2026, 10, 14, 18)},
                                    {at(2026, 10, 14, 18, 30), at(2026, 10, 14, 19)}};
    const char* names[3] = {"Ana", "Sam", "Lee"};
    const auto reply = [&](int64_t readAt) {
      reg::Schedule s;
      s.instrument = "Confocal";
      s.timezone = "America/New_York";
      s.status = reg::Status::Ok;
      s.fetchedAt = readAt;
      for (int i = 0; i < 3; ++i) s.today.push_back({names[i], bookings[i][0], bookings[i][1]});
      return s;
    };
    const reg::Options opt{true, 15, false};
    int64_t now = at(2026, 10, 14, 12, 0);
    reg::Schedule schedule = reply(now - 600);  // the last poll, ten minutes before bed
    reg::Panel panel;                           // nothing of ours until the sleep draws
    int64_t pollDueAt = now + 5 * 60;           // five minutes to the next poll
    struct Draw {
      int64_t at;
      reg::Why why;
    };
    std::vector<Draw> draws;
    int polls = 0, wakes = 0, boundaryWakes = 0;
    {
      const reg::Model m = reg::buildModel(schedule, reg::viewTime(now), opt, reg::Words());
      draws.push_back({now, reg::redrawReason(panel, m, false)});
      panel = reg::panelFor(m);
    }
    while (now < at(2026, 10, 14, 20, 0) && wakes < 200) {
      reg::WakeInputs in;
      in.now = now;
      in.pollEnabled = true;
      in.pollIntervalS = 15 * 60;
      in.pollDueAt = pollDueAt;
      in.boundaryWakes = true;
      in.boundaryAt = reg::nextBoundary(schedule, reg::viewTime(now), opt);
      in.batteryPercent = 80;
      const reg::WakePlan plan = reg::planWake(in);
      if (plan.micros == 0) break;
      pollDueAt = plan.pollDueAt;
      now += static_cast<int64_t>(plan.micros / SEC) + 2;  // the timer, and the boot
      ++wakes;
      reg::WakeInputs woke = in;
      woke.now = now;
      woke.pollDueAt = pollDueAt;
      if (reg::pollDue(woke)) {
        ++polls;
        schedule = reply(now - 30);
        pollDueAt = reg::pollDueAfter(now + 8, 15 * 60);  // a poll takes a few seconds
      } else {
        ++boundaryWakes;
      }
      const reg::Model m = reg::buildModel(schedule, reg::viewTime(now), opt, reg::Words());
      const reg::Why why = reg::redrawReason(panel, m, false);
      if (reg::shouldRedraw(why)) {
        draws.push_back({now + 5, why});  // the refresh takes a few seconds
        panel = reg::panelFor(m);
      }
    }
    // Content changes land within seconds of the boundary: Ana starts at 13:00, ends at 15:00, Sam 16:00 to 18:00,
    // Lee 18:30 to 19:00. Between them only a footer an hour behind draws.
    const int64_t expectedContent[] = {bookings[0][0], bookings[0][1], bookings[1][0], bookings[1][1], bookings[2][0], bookings[2][1]};
    size_t found = 0;
    int footerDraws = 0;
    for (const Draw& d : draws) {
      if (d.why == reg::Why::Footer) {
        ++footerDraws;
        continue;
      }
      if (d.why == reg::Why::NotOurs) continue;  // the sleep draw
      check(found < 6 && d.at >= expectedContent[found] && d.at <= expectedContent[found] + 12, "a content draw lands on its boundary (within 12 s)");
      ++found;
    }
    check(found == 6, "all six boundaries drew");
    check(draws.size() == static_cast<size_t>(1 + 6 + footerDraws), "and nothing else drew but the sleep and the hourly footer");
    // A footer that only moves forward draws about once an hour, never more often than that, however often it polls.
    check(footerDraws >= 4 && footerDraws <= 8, "an hour-old footer drew about hourly");
    for (size_t i = 1; i < draws.size(); ++i)
      if (draws[i].why == reg::Why::Footer) check(draws[i].at - draws[i - 1].at > 40 * 60, "a footer draw is at least 40 minutes after the draw before it");
    for (size_t i = 1; i < draws.size(); ++i) check(draws[i].at - draws[i - 1].at > 60, "no two draws within a minute");
    check(polls >= 30 && polls <= 34, "about a poll every 15 minutes over eight hours");
    check(boundaryWakes == 6, "and exactly one boundary wake for each boundary, with no network");
    std::cout << "(afternoon: " << wakes << " wakes, " << polls << " polls, " << boundaryWakes << " boundary wakes, " << draws.size() << " draws, " << footerDraws << " of them for the footer)\n";
  }

  sleepframe::setWakeHook(nullptr);
  std::cout << "register_wake: " << checks << " checks passed\n";
  return 0;
}
