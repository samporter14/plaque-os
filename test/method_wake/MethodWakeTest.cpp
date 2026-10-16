#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "MethodWake.h"
#include "SleepFrame.h"

// Host tests for the rules of a timer wake's book check (src/ambient/MethodWake.*): the budget arithmetic, the
// per-book verdicts and their order, the gate in front of the whole check, the retry limit and the log words.
// The check that applies them is covered in test/method_check.

using namespace method::wake;

namespace {
int checks = 0;

void check(const bool condition, const char* name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, const char* name) {
  if (!(actual == expected)) {
    std::cerr << "FAIL: " << name << "\n  actual:   " << actual << "\n  expected: " << expected << '\n';
    std::exit(1);
  }
  ++checks;
}

template <typename E>
unsigned num(const E value) {
  return static_cast<unsigned>(value);
}

Rules wakeRules(const uint32_t budgetMs = BUDGET_MS) { return rulesFor(budgetMs, "", 0); }

// A book that is fine in every respect but the one a test changes.
Request fine() {
  Request request{};
  request.bookId = 12;
  request.bytes = 400000;
  request.battery = 80;
  request.elapsedMs = 20000;
  request.started = 0;
  request.displacesProtected = false;
  return request;
}

void testTheConstantsAreWhatTheSpecSays() {
  checkEq(BUDGET_MS, 90000u, "the wake budget is the 90 s clock window");
  checkEq(PLANNING_BYTES_PER_SEC, 12000u, "planned at 12 KB/s");
  checkEq(MIN_BATTERY_PERCENT, 30, "downloads need 30 % battery");
  checkEq(MAX_BYTES, 1000000u, "a wake takes books up to 1 MB");
  checkEq(MAX_BOOKS, 2u, "at most two books per wake");
  const Rules rules = wakeRules();
  check(!Rules{}.active, "rules are off unless a wake asks for them");
  check(rules.active, "rulesFor() turns them on");
  checkEq(rules.minBatteryPercent, MIN_BATTERY_PERCENT, "rules carry the battery floor");
  checkEq(rules.maxBytes, MAX_BYTES, "rules carry the size cap");
  checkEq(rules.maxBooks, MAX_BOOKS, "rules carry the book cap");
  checkEq(rules.bytesPerSec, PLANNING_BYTES_PER_SEC, "rules carry the planning rate");
  const Rules built = rulesFor(61234, "/Lamina/Method/A abcd1234.xtc", 17);
  checkEq(built.budgetMs, 61234u, "rulesFor() takes what is left of the budget");
  checkEq(built.protectedPath, std::string("/Lamina/Method/A abcd1234.xtc"), "rulesFor() takes the protected path");
  checkEq(built.blockedBookId, 17u, "rulesFor() takes the blocked book");
}

void testTransferArithmetic() {
  const Rules rules = wakeRules();
  checkEq(transferMs(rules, 0), 0u, "nothing to move takes no time");
  checkEq(transferMs(rules, 1), 1u, "one byte rounds up to a millisecond");
  checkEq(transferMs(rules, 12), 1u, "12 bytes is exactly 1 ms at 12 KB/s");
  checkEq(transferMs(rules, 13), 2u, "13 bytes rounds up");
  checkEq(transferMs(rules, 12000), 1000u, "12 KB takes a second");
  checkEq(transferMs(rules, 384000), 32000u, "an 8-page book (384 KB) takes 32 s");
  checkEq(transferMs(rules, 480000), 40000u, "480 KB takes 40 s");
  checkEq(transferMs(rules, 1000000), 83334u, "1 MB takes 83.3 s, rounded up");
  checkEq(transferMs(rules, 1000008), 83334u, "1,000,008 B is exactly 83,334 ms");
  checkEq(transferMs(rules, 1000009), 83335u, "and one byte more takes a millisecond more");
  checkEq(transferMs(rules, UINT32_MAX), 357913942u, "the longest announced length does not overflow");

  checkEq(neededMs(rules, 0), 11000u, "an empty book still costs the request and the filing");
  checkEq(neededMs(rules, 240000), 31000u, "240 KB needs 8 s + 20 s + 3 s");
  checkEq(neededMs(rules, 384000), 43000u, "an 8-page book needs 8 s + 32 s + 3 s");
  checkEq(neededMs(rules, 1000000), 94334u, "1 MB needs 8 s + 83.3 s + 3 s: more than a whole wake");

  Rules slow = wakeRules();
  slow.bytesPerSec = 1;
  checkEq(transferMs(slow, UINT32_MAX), UINT32_MAX, "an impossibly slow link saturates");
  checkEq(neededMs(slow, UINT32_MAX), UINT32_MAX, "and so does the total");
  Rules none = wakeRules();
  none.bytesPerSec = 0;
  checkEq(transferMs(none, 10), UINT32_MAX, "no rate at all never divides by zero");

  checkEq(remainingMs(rules, 0), BUDGET_MS, "the whole budget is left at the start");
  checkEq(remainingMs(rules, 20000), 70000u, "20 s in");
  checkEq(remainingMs(rules, BUDGET_MS), 0u, "none left at the end");
  checkEq(remainingMs(rules, BUDGET_MS + 1), 0u, "never negative");
  checkEq(remainingMs(wakeRules(1234), 234), 1000u, "counted from what rulesFor() was given");
}

void testTheBudgetBoundary() {
  const Rules rules = wakeRules();
  Request request = fine();
  request.bytes = 240000;  // needs 31 s
  request.elapsedMs = 59000;
  Decision d = judge(rules, request);
  checkEq(num(d.verdict), num(Verdict::Download), "a book that fits exactly is taken");
  checkEq(d.neededMs, 31000u, "needed is reported");
  checkEq(d.remainingMs, 31000u, "remaining is reported");
  request.elapsedMs = 59001;
  d = judge(rules, request);
  checkEq(num(d.verdict), num(Verdict::NoTime), "one millisecond short is not taken");
  checkEq(d.remainingMs, 30999u, "remaining after the extra millisecond");

  // An 8-page book (384 KB) needs 43 s, so it must start within the first 47 s of the wake.
  request.bytes = 384000;
  request.elapsedMs = 47000;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Download), "an 8-page book fits when 47 s have gone");
  request.elapsedMs = 47001;
  checkEq(num(judge(rules, request).verdict), num(Verdict::NoTime), "it does not fit a millisecond later");

  // The largest book that can start at all is 948,000 B: it takes the whole 90 s. A 1 MB book never fits a wake,
  // however quickly the note check went.
  request.bytes = 948000;
  request.elapsedMs = 0;
  check(judge(rules, request).verdict == Verdict::Download, "948,000 B fits an empty wake exactly");
  request.bytes = 948001;
  check(judge(rules, request).verdict == Verdict::NoTime, "one byte more does not");
  request.bytes = MAX_BYTES;
  check(judge(rules, request).verdict == Verdict::NoTime, "1 MB does not fit even an empty wake");

  // Two of the largest books never fit in one wake, whatever the note check cost.
  request.bytes = 948000;
  request.elapsedMs = neededMs(rules, 948000);
  check(judge(rules, request).verdict == Verdict::NoTime, "the second one does not fit behind the first");

  // The budget handed to a check is what is left of the wake: a slow note check leaves less.
  const Rules late = wakeRules(BUDGET_MS - 30000);  // the note check took 30 s
  request.elapsedMs = 0;
  request.bytes = 384000;
  checkEq(num(judge(late, request).verdict), num(Verdict::Download), "an 8-page book still fits 60 s");
  request.bytes = 588000;  // 8 + 49 + 3 = 60 s
  checkEq(num(judge(late, request).verdict), num(Verdict::Download), "588 KB fits 60 s exactly");
  request.elapsedMs = 1;
  checkEq(num(judge(late, request).verdict), num(Verdict::NoTime), "588 KB does not fit 59.999 s");
}

void testTheBatteryFloor() {
  const Rules rules = wakeRules();
  Request request = fine();
  request.battery = 29;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Battery), "29 % is under the floor");
  request.battery = 30;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Download), "30 % is enough");
  request.battery = 0;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Battery), "an empty gauge is under it");
  request.battery = -1;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Battery), "a gauge that failed is under it");
  Rules strict = wakeRules();
  strict.minBatteryPercent = 50;
  request.battery = 49;
  checkEq(num(judge(strict, request).verdict), num(Verdict::Battery), "the floor is the rule's");
}

void testTheSizeCap() {
  // The cap, apart from the budget: a budget so long that time is never the reason.
  const Rules rules = wakeRules(1000000);
  Request request = fine();
  request.bytes = MAX_BYTES;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Download), "exactly 1 MB is taken");
  request.bytes = MAX_BYTES + 1;
  checkEq(num(judge(rules, request).verdict), num(Verdict::TooLarge), "one byte over is not");
  request.bytes = 1900000;
  checkEq(num(judge(rules, request).verdict), num(Verdict::TooLarge), "the Worker's largest book is not");
  request.bytes = UINT32_MAX;
  checkEq(num(judge(rules, request).verdict), num(Verdict::TooLarge), "the largest length there is is not");
}

void testTheBookCap() {
  const Rules rules = wakeRules();
  Request request = fine();
  request.bytes = 100000;
  request.started = 0;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Download), "the first book");
  request.started = 1;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Download), "the second book");
  request.started = 2;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Limit), "not a third");
  checkEq(judge(rules, request).started, 2u, "the count is reported");
}

void testTheBookOnThePanel() {
  const Rules rules = wakeRules();
  Request request = fine();
  request.displacesProtected = true;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Protected), "a book that would delete the step is left");
}

void testTheRetryLimit() {
  Rules rules = wakeRules();
  rules.blockedBookId = 12;
  Request request = fine();
  checkEq(num(judge(rules, request).verdict), num(Verdict::Blocked), "the blocked book is left");
  request.bookId = 13;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Download), "another book is not");
  rules.blockedBookId = 0;
  request.bookId = 0;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Download), "no blocked book blocks nothing");
}

void testTheOrderOfTheReasons() {
  Rules rules = wakeRules();
  rules.blockedBookId = 12;
  Request request = fine();
  request.battery = 10;
  request.bytes = MAX_BYTES + 1;
  request.displacesProtected = true;
  request.started = 5;
  request.elapsedMs = BUDGET_MS;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Battery), "battery first");
  request.battery = 80;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Blocked), "then the retry limit");
  request.bookId = 13;
  checkEq(num(judge(rules, request).verdict), num(Verdict::TooLarge), "then the size");
  request.bytes = 1000;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Protected), "then the book on the panel");
  request.displacesProtected = false;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Limit), "then the count");
  request.started = 0;
  checkEq(num(judge(rules, request).verdict), num(Verdict::NoTime), "then the time");
  request.elapsedMs = 0;
  checkEq(num(judge(rules, request).verdict), num(Verdict::Download), "and then it is taken");
}

void testTheGate() {
  const Rules rules = wakeRules();
  GateInputs in{80, 70000, true, true, false};
  checkEq(num(gate(rules, in)), num(Gate::Run), "a healthy wake checks");
  in.battery = 29;
  checkEq(num(gate(rules, in)), num(Gate::Battery), "29 % does not even ask for the list");
  in.battery = 30;
  checkEq(num(gate(rules, in)), num(Gate::Run), "30 % does");
  in.remainingMs = REQUEST_MS;
  checkEq(num(gate(rules, in)), num(Gate::Run), "one request's time is enough to ask");
  in.remainingMs = REQUEST_MS - 1;
  checkEq(num(gate(rules, in)), num(Gate::NoTime), "a millisecond less is not");
  in.remainingMs = 0;
  checkEq(num(gate(rules, in)), num(Gate::NoTime), "nothing left is not");
  in.remainingMs = 70000;

  in.noteFailed = true;
  in.clockReady = false;
  checkEq(num(gate(rules, in)), num(Gate::NoClock), "a failed note check with no clock does not retry the sync");
  in.clockReady = true;
  checkEq(num(gate(rules, in)), num(Gate::Run), "a failed note check that left a clock still asks");
  in.clockReady = false;
  in.https = false;
  checkEq(num(gate(rules, in)), num(Gate::Run), "plain http needs no clock");
  in.https = true;
  in.noteFailed = false;
  checkEq(num(gate(rules, in)), num(Gate::Run), "a clock that lapsed is synced again by the check itself");

  in.battery = 5;
  in.remainingMs = 0;
  in.noteFailed = true;
  checkEq(num(gate(rules, in)), num(Gate::Battery), "the battery is the first reason");
  in.battery = 80;
  checkEq(num(gate(rules, in)), num(Gate::NoTime), "then the time");
}

// A step timer that is running must not be made late by a book.
void testAStepTimerCutsTheBudget() {
  checkEq(steptimer::WAKE_LEAD_MS, int64_t{2000}, "the alert's wake leads its end by 2 s");
  checkEq(TIMER_MARGIN_MS, int64_t{10000}, "and the book check leaves a margin of 10 s");

  // No timer: the plain budget.
  checkEq(budgetLeft(0, false, 0), BUDGET_MS, "no timer: the whole budget");
  checkEq(budgetLeft(20000, false, 0), 70000u, "no timer: what the wake has left");
  checkEq(budgetLeft(BUDGET_MS, false, 0), 0u, "no timer: none left at the end");
  checkEq(budgetLeft(BUDGET_MS + 5, false, 0), 0u, "no timer: never negative");
  checkEq(budgetLeft(20000, false, 1), 70000u, "a timer value is not read when no timer runs");

  // A timer far off does not matter.
  checkEq(budgetLeft(20000, true, 3600 * 1000), 70000u, "a timer an hour away: the plain budget");
  checkEq(budgetLeft(20000, true, 70000 + 12000), 70000u, "a timer exactly beyond the budget plus lead and margin");

  // A timer that ends within the budget cuts it: the time to its end less 2 s of lead and 10 s of margin.
  checkEq(budgetLeft(20000, true, 70000 + 12000 - 1), 69999u, "a millisecond sooner: a millisecond less");
  checkEq(budgetLeft(20000, true, 60000), 48000u, "a timer 60 s off: 48 s of budget");
  checkEq(budgetLeft(0, true, 30000), 18000u, "a timer 30 s off, at the start of the wake");
  checkEq(budgetLeft(20000, true, 30000), 18000u, "the cap is on the timer's remaining time, which is read now");
  // The cap leaves the check at least the 8 s a request needs only when the timer is far enough off.
  const Rules rules = wakeRules();
  GateInputs in{80, budgetLeft(20000, true, 20000), true, true, false};
  checkEq(in.remainingMs, 8000u, "a timer 20 s off leaves exactly one request's time");
  checkEq(num(gate(rules, in)), num(Gate::Run), "which still asks for the list");
  in.remainingMs = budgetLeft(20000, true, 19999);
  checkEq(in.remainingMs, 7999u, "a millisecond closer");
  checkEq(num(gate(rules, in)), num(Gate::NoTime), "and the book check does not start");

  // Close to the end, or already past it: nothing.
  checkEq(budgetLeft(20000, true, 12000), 0u, "a timer 12 s off: nothing left");
  checkEq(budgetLeft(20000, true, 11999), 0u, "a timer 11.999 s off: nothing, not negative");
  checkEq(budgetLeft(20000, true, 5000), 0u, "a timer 5 s off");
  checkEq(budgetLeft(20000, true, 0), 0u, "a timer due now");
  checkEq(budgetLeft(20000, true, -5000), 0u, "a timer already past its end");
  checkEq(budgetLeft(20000, true, INT64_MIN), 0u, "the most negative time left");
  checkEq(budgetLeft(20000, true, INT64_MAX), 70000u, "the largest time left");
  checkEq(budgetLeft(BUDGET_MS + 1000, true, 3600 * 1000), 0u, "and an expired budget stays expired");

  // The same cut decides a book: the timer leaves 18 s, so a book that needs 31 s is left for the shelf, one that
  // needs 11 s (an empty book) is not.
  const Rules cut = rulesFor(budgetLeft(0, true, 30000), "", 0);
  Request request = fine();
  request.elapsedMs = 0;
  request.bytes = 240000;
  checkEq(num(judge(cut, request).verdict), num(Verdict::NoTime), "a 31 s book does not fit 18 s");
  request.bytes = 84000;  // 8 + 7 + 3 = 18 s
  checkEq(num(judge(cut, request).verdict), num(Verdict::Download), "an 18 s book fits exactly");
  request.bytes = 84001;
  checkEq(num(judge(cut, request).verdict), num(Verdict::NoTime), "one byte more does not");

  // A check that runs on past its budget by the whole allowance (a handshake of 5 s, then 5 s to get to sleep) is
  // asleep with at least the alert's own lead left of the timer, whatever the timer's distance.
  for (const int64_t timerMs : {20000, 30000, 60000, 89000, 90000, 100000, 120000, 3600000}) {
    const int64_t left = budgetLeft(0, true, timerMs);
    const int64_t timerLeftAtBudgetEnd = timerMs - left;
    check(left <= BUDGET_MS, "never more than the budget");
    check(timerLeftAtBudgetEnd - 10000 >= steptimer::WAKE_LEAD_MS, "asleep with the alert's lead to spare");
  }

  // The loop's last-resort guard stays inside the margin when a timer runs.
  checkEq(hardStopGraceMs(false), HARD_STOP_GRACE_MS, "no timer: the usual grace");
  checkEq(hardStopGraceMs(true), TIMER_HARD_STOP_GRACE_MS, "a timer: the short one");
  check(static_cast<int64_t>(TIMER_HARD_STOP_GRACE_MS) <= TIMER_MARGIN_MS / 2, "which fits in half the margin");
  check(TIMER_HARD_STOP_GRACE_MS < HARD_STOP_GRACE_MS, "and is shorter than the usual one");
}

void testTheBackoff() {
  Backoff backoff;
  checkEq(blockedId(backoff), 0u, "nothing is blocked at first");
  noteFailure(backoff, 12);
  checkEq(backoff.failures, uint8_t{1}, "one failure");
  checkEq(blockedId(backoff), 0u, "is not enough to block");
  noteFailure(backoff, 12);
  checkEq(blockedId(backoff), 12u, "two failures of the same book block it");
  checkEq(MAX_FAILED_WAKES, uint8_t{2}, "the limit is two wakes");
  noteFailure(backoff, 12);
  checkEq(blockedId(backoff), 12u, "and it stays blocked");
  noteFailure(backoff, 14);
  checkEq(backoff.bookId, 14u, "a different book starts a new count");
  checkEq(backoff.failures, uint8_t{1}, "at one");
  checkEq(blockedId(backoff), 0u, "so nothing is blocked");
  noteFailure(backoff, 0);
  checkEq(backoff.bookId, 14u, "book 0 is no book");

  forgetHandled(backoff, 13);
  checkEq(backoff.bookId, 14u, "a mark below the book keeps the count");
  forgetHandled(backoff, 14);
  checkEq(backoff.bookId, 0u, "a mark at the book forgets it");
  checkEq(backoff.failures, uint8_t{0}, "and its failures");
  noteFailure(backoff, 20);
  noteFailure(backoff, 20);
  forgetHandled(backoff, 99);
  checkEq(blockedId(backoff), 0u, "a mark past a blocked book frees it");

  Backoff worn;
  worn.bookId = 7;
  worn.failures = UINT8_MAX;
  noteFailure(worn, 7);
  checkEq(worn.failures, UINT8_MAX, "the count saturates instead of wrapping to zero");
  checkEq(blockedId(worn), 7u, "so a worn-out book stays blocked");
}

// Polls the watch every `stepMs` from `from` to `to` with the contact given by `contactAt(t)`; the time of the first
// confirmation, or -1.
template <typename Contact>
int64_t firstConfirmed(TouchWatch& watch, const uint32_t from, const uint32_t to, const uint32_t stepMs,
                       Contact contactAt) {
  for (uint32_t now = from; now <= to; now += stepMs)
    if (watch.observe(now, contactAt(now))) return now;
  return -1;
}

void testATouchIsNotAPhantom() {
  checkEq(TOUCH_CONFIRM_MS, 50u, "a contact has to last 50 ms");

  // Nothing there: nothing happens.
  {
    TouchWatch watch;
    checkEq(firstConfirmed(watch, 0, 10000, 10, [](uint32_t) { return false; }), int64_t{-1}, "no contact, no press");
  }
  // A contact already there at the first look (a reader face-down on a bench) is ignored for as long as it stays.
  {
    TouchWatch watch;
    checkEq(firstConfirmed(watch, 0, 60000, 10, [](uint32_t) { return true; }), int64_t{-1},
            "a contact held since the first look is never a press");
    check(!watch.confirmed(), "and is not latched");
  }
  // ... until it lifts: the next contact is new.
  {
    TouchWatch watch;
    const auto contact = [](uint32_t t) { return t < 2000 || (t >= 3000 && t < 3500); };
    checkEq(firstConfirmed(watch, 0, 5000, 10, contact), int64_t{3050},
            "after a held contact lifts, a new one counts 50 ms in");
  }
  // A brief blip is not a finger.
  {
    TouchWatch watch;
    checkEq(firstConfirmed(watch, 0, 1000, 10, [](uint32_t t) { return t == 100; }), int64_t{-1},
            "one reading of contact is a blip");
    TouchWatch shorter;
    checkEq(firstConfirmed(shorter, 0, 1000, 10, [](uint32_t t) { return t >= 100 && t <= 140; }), int64_t{-1},
            "a contact of 40 ms is a blip");
    TouchWatch flicker;
    checkEq(firstConfirmed(flicker, 0, 10000, 10, [](uint32_t t) { return (t / 10) % 3 == 1; }), int64_t{-1},
            "a flicker that is never down for two readings is never a press");
    TouchWatch alternating;
    checkEq(firstConfirmed(alternating, 0, 10000, 10, [](uint32_t t) { return (t / 10) % 2 == 1; }), int64_t{-1},
            "a contact on and off at every reading is never a press");
  }
  // A real tap: a new contact seen at a reading and again 50 ms or more later.
  {
    TouchWatch watch;
    checkEq(firstConfirmed(watch, 0, 1000, 10, [](uint32_t t) { return t >= 100 && t < 300; }), int64_t{150},
            "a tap of 200 ms is a press at the reading 50 ms after the first");
    TouchWatch exact;
    checkEq(firstConfirmed(exact, 0, 1000, 10, [](uint32_t t) { return t >= 100 && t <= 150; }), int64_t{150},
            "a contact read at 100 and at 150 is just enough");
    TouchWatch justShort;
    checkEq(firstConfirmed(justShort, 0, 1000, 10, [](uint32_t t) { return t >= 100 && t <= 149; }), int64_t{-1},
            "one that is gone before the reading at 150 is not");
    TouchWatch slow;  // the polls are 30 ms apart while a step runs
    checkEq(firstConfirmed(slow, 0, 1000, 30, [](uint32_t t) { return t >= 100 && t < 400; }), int64_t{180},
            "polls 30 ms apart: seen at 120, confirmed at 180");
    TouchWatch late;  // a poll gap longer than the confirmation: the second reading confirms
    check(!late.observe(0, false) && !late.observe(1000, true) && late.observe(1200, true),
          "a slow poll also confirms");
    TouchWatch lifted;
    check(!lifted.observe(0, false) && !lifted.observe(1000, true) && !lifted.observe(1200, false),
          "a contact seen once and gone at the next, slow, reading is not confirmed");
  }
  // Confirmed stays confirmed.
  {
    TouchWatch watch;
    check(!watch.observe(0, false), "start");
    check(!watch.observe(10, true), "first sight of the new contact");
    check(watch.observe(60, true), "confirmed");
    check(watch.observe(70, false), "and it stays confirmed after the finger lifts");
    check(watch.confirmed(), "latched");
  }
  // A contact that appears after the first look and stays is a finger as far as the reader can tell.
  {
    TouchWatch watch;
    checkEq(firstConfirmed(watch, 0, 5000, 10, [](uint32_t t) { return t >= 1000; }), int64_t{1050},
            "a new contact that stays counts");
  }
  // The clock may wrap.
  {
    TouchWatch watch;
    check(!watch.observe(0xFFFFFFF0u, false), "before the wrap");
    check(!watch.observe(0xFFFFFFF8u, true), "contact seen just before the wrap");
    check(!watch.observe(0x00000020u, true), "40 ms later, across the wrap");
    check(watch.observe(0x0000003Au, true), "and 66 ms later: confirmed");
  }
}

void testAHeldKeyNeedsTheDebounceToCommit() {
  checkEq(KEY_SETTLE_MS, 8u, "the last look comes 8 ms after the one before it");
  checkEq(settleWaitMs(0), 8u, "a look straight after the first waits the whole 8 ms");
  checkEq(settleWaitMs(1), 7u, "1 ms after: 7 more");
  checkEq(settleWaitMs(5), 3u, "5 ms after (the debounce itself): 3 more, so it can commit");
  checkEq(settleWaitMs(7), 1u, "7 ms after: 1 more");
  checkEq(settleWaitMs(8), 0u, "8 ms after: no wait");
  checkEq(settleWaitMs(9), 0u, "later: no wait");
  checkEq(settleWaitMs(5000), 0u, "much later: no wait");
  checkEq(settleWaitMs(UINT32_MAX), 0u, "never wraps into a wait");
  check(KEY_SETTLE_MS > 5, "longer than the input debounce, which is 5 ms");
}

void testAFingerInTheLastWindowStillCounts() {
  // The watch knows how much of the confirmation window a new contact still has to run.
  {
    TouchWatch watch;
    checkEq(watch.waitMs(0), 0u, "nothing seen: no wait");
    watch.observe(0, false);
    checkEq(watch.waitMs(10), 0u, "no contact: no wait");
    watch.observe(1000, true);
    checkEq(watch.waitMs(1000), 50u, "a contact just seen: the whole window");
    checkEq(watch.waitMs(1020), 30u, "20 ms later: 30 ms more");
    checkEq(watch.waitMs(1049), 1u, "49 ms later: 1 ms more");
    checkEq(watch.waitMs(1050), 0u, "50 ms later: none");
    checkEq(watch.waitMs(5000), 0u, "long after: none");
    watch.observe(1060, true);
    check(watch.confirmed(), "confirmed");
    checkEq(watch.waitMs(1060), 0u, "a confirmed contact waits for nothing");
  }
  {
    TouchWatch held;  // a contact there at the first look is ignored, so there is nothing to wait for
    held.observe(0, true);
    checkEq(held.waitMs(0), 0u, "a contact held at the first look: no wait");
    held.observe(10, true);
    checkEq(held.waitMs(10), 0u, "still none");
  }
  {
    TouchWatch wrap;  // across the clock wrap
    wrap.observe(0xFFFFFFF0u, false);
    wrap.observe(0xFFFFFFF8u, true);
    checkEq(wrap.waitMs(0x00000008u), 34u, "16 ms after the first sight, across the wrap: 34 more");
    wrap.observe(0x00000008u, false);
    checkEq(wrap.waitMs(0x00000008u), 0u, "a blip: no wait");
  }

  // The wait for the last look is the longer of the key settle and the touch window.
  {
    PressWatch watch;
    watch.observe(0, false, false, false, false);
    checkEq(watch.lastLookWaitMs(1, 1), 7u, "only the key settle: 7 ms");
    checkEq(watch.lastLookWaitMs(100, 100), 0u, "long after, nothing pending: none");
    watch.observe(1000, false, false, false, true);  // a finger comes down
    checkEq(watch.lastLookWaitMs(1005, 5), 45u, "a finger 5 ms old: 45 ms, longer than the key settle's 3");
    checkEq(watch.lastLookWaitMs(1049, 49), 1u, "49 ms old: 1 ms");
    checkEq(watch.lastLookWaitMs(1050, 50), 0u, "50 ms old: none");
    checkEq(watch.lastLookWaitMs(1049, 0), 8u, "the key settle wins when it is longer");
  }

  // A finger that came down in the last 50 ms of the check: the wake waits it out, reads once more, and counts it.
  {
    PressWatch watch;
    bool pressed = false;
    for (uint32_t now = 0; now < 1000 && !pressed; now += 10) pressed = watch.observe(now, false, false, false, false);
    check(!pressed, "nothing during the check");
    check(!watch.observe(1000, false, false, false, true), "a finger comes down as the check ends");
    const uint32_t wait = watch.lastLookWaitMs(1004, 4);
    checkEq(wait, 46u, "the last look waits out the window");
    check(watch.observe(1004 + wait, false, false, false, true), "and the finger, still there, counts");
  }
  // ... and one that was only a blip does not.
  {
    PressWatch watch;
    watch.observe(0, false, false, false, false);
    watch.observe(1000, false, false, false, true);
    const uint32_t wait = watch.lastLookWaitMs(1004, 4);
    check(!watch.observe(1004 + wait, false, false, false, false), "lifted by the last look: a blip, not a press");
  }
}

void testWhatIsLeftDownAtTheCap() {
  checkEq(RELEASE_WAIT_MS, 700u, "the boot waits 700 ms for a press to end");
  auto left = leftover(false, false);
  check(!left.suppressTouch && !left.consumeKeyReleases, "nothing down: nothing to do");
  left = leftover(true, false);
  check(left.suppressTouch && !left.consumeKeyReleases, "a finger still down: the rest of its contact is suppressed");
  left = leftover(false, true);
  check(!left.suppressTouch && left.consumeKeyReleases,
        "a key still down: its release is swallowed as a consumed long-press");
  left = leftover(true, true);
  check(left.suppressTouch && left.consumeKeyReleases, "both");
}

void testTheHomeKeyLevel() {
  HomeKeyLevel key;
  check(!key.down() && !key.observe(false, false, false), "up at first");
  check(key.observe(true, false, false), "a press edge: down");
  check(key.observe(false, false, false) && key.down(), "stays down while held");
  check(!key.observe(false, true, false), "a tap is the release");
  check(!key.observe(false, false, false), "and it stays up");

  // Held past the long-press threshold: the release is not a tap, so the long press is the last thing seen.
  check(key.observe(true, false, false), "pressed again");
  check(!key.observe(false, false, true), "a long press ends the wait: its release is not a tap");

  // A whole tap between two polls.
  check(!key.observe(true, true, false), "press and tap in one poll: up");
  check(!key.observe(false, true, false), "a tap with no press seen: up");
  check(key.observe(true, false, false), "a new press");
  check(!key.observe(false, true, false), "released");
}

void testKeysAreStillTheLevelRule() {
  // A key press edge: a press at once, whatever the touch is doing.
  {
    PressWatch watch;
    check(watch.observe(0, true, false, false, false), "a key press edge is a press");
  }
  {
    PressWatch watch;
    check(watch.observe(0, true, false, false, true), "also at the first look, with a contact held");
  }
  // A key held since before the first sample has no edge: its level, read twice.
  {
    PressWatch watch;
    check(watch.observe(0, false, true, true, false), "a key held on two readings is a press");
  }
  {
    PressWatch watch;
    check(!watch.observe(0, false, true, false, false), "one noisy reading is not");
    check(!watch.observe(10, false, false, true, false), "the second reading alone is nothing either");
    check(!watch.observe(20, false, false, false, false), "nothing at all is nothing");
  }
  // A phantom contact never stops a key from counting, and a key never waits for the touch rule.
  {
    PressWatch watch;
    check(!watch.observe(0, false, false, false, true), "a contact at the first look: ignored");
    check(!watch.observe(10, false, false, false, true), "still ignored");
    check(watch.observe(20, false, true, true, true), "a key on top of it counts at once");
  }
  // The bench: a reader face-down reports a contact at every poll for ten minutes. Not once is it a press.
  {
    PressWatch watch;
    bool any = false;
    for (uint32_t now = 0; now < 600000; now += 10) any = any || watch.observe(now, false, false, false, true);
    check(!any, "a steady phantom contact is never a press");
  }
  // The pouch: contact in bursts of one or two readings.
  {
    PressWatch watch;
    bool any = false;
    for (uint32_t now = 0; now < 600000; now += 10) {
      const uint32_t phase = (now / 10) % 7;
      any = any || watch.observe(now, false, false, false, phase == 2 || phase == 3 || phase == 5);
    }
    check(!any, "bursts of one or two readings are never a press");
  }
  // And a real tap after a long phantom: the user's finger is a new contact only if the phantom lifted first.
  {
    PressWatch watch;
    bool touched = false;
    for (uint32_t now = 0; now < 1000; now += 10) touched = touched || watch.observe(now, false, false, false, true);
    check(!touched, "the phantom is ignored");
    check(!watch.observe(1000, false, false, false, false), "it lifts");
    check(!watch.observe(1010, false, false, false, true), "a finger lands");
    check(watch.observe(1060, false, false, false, true), "and counts 50 ms later");
  }
}

const std::string METHOD_BOOK = "/Lamina/Method/Example column clean-up 1a2b3c4d.xtc";
const std::string OTHER_BOOK = "/Books/Lab Handbook.epub";

// What the SD card holds when the reader wakes on a timer, and what main.cpp then does with it. The decisions are the
// ones setup() makes: planWake for the marker, resumesBook for the route, "a persisted sleep wake" for the splash.
struct Wake {
  std::string openEpubPath;
  bool lastSleepFromReader = false;
  bool methodStepOnPanel = false;
  bool showBootScreen = false;  // enterDeepSleep() clears it so the next wake keeps the panel
  uint8_t readerLoadCount = 0;
  bool backHeld = false;
};

struct Landing {
  bool slept = false;    // the wake ended by sleeping again
  bool splash = false;   // the boot shows the splash
  bool marker = false;   // the Method marker after the wake
  std::string route;     // "reader:<path>" or "home"; empty when it slept
  bool noteKept = false; // the fetched note was committed
  bool redraw = false;   // the wake drew the fetched note itself
};

Landing wakeUp(const Wake& card, const bool userActed, const bool noteFetched) {
  Landing out;
  out.marker = card.methodStepOnPanel;
  const method::wake::Ending end = method::wake::ending(userActed, noteFetched);
  if (!end.continueBoot) {
    // The old tail of the timer branch, unchanged: draw or keep the note, then sleep with the marker as found.
    const auto plan = sleepframe::planWake(sleepframe::Wake::Timer, card.methodStepOnPanel, noteFetched);
    out.redraw = plan.redraw;
    out.noteKept = plan.redraw || plan.commitFetched;
    out.marker = plan.markerAfter;
    out.slept = true;
    return out;
  }
  out.noteKept = end.commitNote;
  const bool persistedSleepWake = end.sleepWake && !card.showBootScreen;
  out.splash = !persistedSleepWake;
  const sleepframe::Wake kind = end.sleepWake ? sleepframe::Wake::Button : sleepframe::Wake::Other;
  out.marker = sleepframe::planWake(kind, card.methodStepOnPanel, false).markerAfter;
  out.route = sleepframe::resumesBook(card.openEpubPath, card.lastSleepFromReader, card.backHeld, card.readerLoadCount)
                  ? "reader:" + card.openEpubPath
                  : "home";
  return out;
}

void testWithoutAPressNothingChanges() {
  const auto none = method::wake::ending(false, true);
  check(!none.continueBoot && !none.sleepWake && !none.commitNote && !none.swallowRelease,
        "no press: the wake sleeps again and changes nothing");
  const auto quiet = method::wake::ending(false, false);
  check(!quiet.continueBoot && !quiet.sleepWake && !quiet.commitNote && !quiet.swallowRelease, "same without a note");

  // Exactly the old behaviour, with a Method step on the panel or not.
  Wake step;
  step.openEpubPath = METHOD_BOOK;
  step.lastSleepFromReader = true;
  step.methodStepOnPanel = true;
  Landing l = wakeUp(step, false, true);
  check(l.slept && !l.redraw && l.noteKept && l.marker,
        "a step on the panel: the note is kept, not drawn, the marker stays");
  Wake home;
  l = wakeUp(home, false, true);
  check(l.slept && l.redraw && l.noteKept && !l.marker, "nothing on the panel: the note is drawn as before");
  l = wakeUp(home, false, false);
  check(l.slept && !l.redraw && !l.noteKept, "no note, no change");
}

void testAPressMakesTheWakeAButtonWake() {
  const auto end = method::wake::ending(true, true);
  check(end.continueBoot, "a press carries on into the boot");
  check(end.sleepWake, "as a wake from sleep");
  check(end.commitNote, "keeping the note it fetched");
  check(end.swallowRelease, "and swallowing the release of the press");
  check(!method::wake::ending(true, false).commitNote, "nothing to keep without a note");
  check(method::wake::ending(true, false).continueBoot && method::wake::ending(true, false).sleepWake,
        "the press decides, not the note");

  // Sleeping in a Method book, then a press during a timer wake's book check: back on that page, no splash, the
  // marker cleared (the boot repaints the panel), the note kept but not drawn.
  Wake step;
  step.openEpubPath = METHOD_BOOK;
  step.lastSleepFromReader = true;
  step.methodStepOnPanel = true;
  Landing l = wakeUp(step, true, true);
  check(!l.slept, "it does not sleep again");
  check(!l.splash, "no splash: the panel keeps the step until the first paint, as on a button wake");
  checkEq(l.route, "reader:" + METHOD_BOOK, "back on the book on the panel");
  check(!l.marker, "the Method marker is cleared");
  check(l.noteKept && !l.redraw, "the note is kept and not drawn");

  // Without a note.
  l = wakeUp(step, true, false);
  checkEq(l.route, "reader:" + METHOD_BOOK, "back on the book without a note too");
  check(!l.noteKept, "nothing kept");

  // Sleeping from Home on the ambient screen: Home.
  Wake home;
  l = wakeUp(home, true, true);
  checkEq(l.route, std::string("home"), "no book open: Home");
  check(!l.splash && !l.marker && l.noteKept && !l.slept, "and the same button-wake boot");

  // A book that is not a Method: the same rules as a button wake, which reopen it.
  Wake epub;
  epub.openEpubPath = OTHER_BOOK;
  epub.lastSleepFromReader = true;
  l = wakeUp(epub, true, false);
  checkEq(l.route, "reader:" + OTHER_BOOK, "another book reopens too, as on a button wake");

  // Left the book for Home before sleeping: the stale path is not reopened.
  Wake left;
  left.openEpubPath = METHOD_BOOK;
  left.lastSleepFromReader = false;
  checkEq(wakeUp(left, true, false).route, std::string("home"), "a stale remembered book is not reopened");

  // Back held at the press: Home, the escape every wake has.
  Wake back = step;
  back.backHeld = true;
  checkEq(wakeUp(back, true, false).route, std::string("home"), "Back held lands on Home, as on a button wake");

  // A reader that never finished opening (the crash guard).
  Wake crashed = step;
  crashed.readerLoadCount = 1;
  checkEq(wakeUp(crashed, true, false).route, std::string("home"), "the crash guard still sends it Home");

  // The one-shot flag says the last boot was not a clean sleep: a splash, as on a button wake.
  Wake stale = step;
  stale.showBootScreen = true;
  check(wakeUp(stale, true, false).splash, "showBootScreen set: the splash, as on a button wake");
}

// A finished step timer is left for whoever wakes the reader; a press that ends a poll wake is that person. main.cpp
// asks the step timer's own boot rule with timerWake false, as any boot by someone using the reader does.
void testAPressAcknowledgesAFinishedStepTimer() {
  using steptimer::BootAction;
  using steptimer::BootFacts;
  using steptimer::Phase;
  const auto asked = [](Phase phase, bool timerWake) {
    return steptimer::planBoot(BootFacts{phase, false, 0, timerWake});
  };
  check(asked(Phase::Alerted, true) == BootAction::None, "an uninterrupted poll leaves a finished timer alone");
  check(asked(Phase::Alerted, false) == BootAction::Acknowledge, "a person using the reader acknowledges it");
  check(asked(Phase::None, false) == BootAction::None, "no timer: nothing to acknowledge");
  // A timer still running is never acknowledged, with the time left main.cpp gives it (the real remaining time) or
  // without (it passes 0 when none runs, and it only acts on Acknowledge).
  check(steptimer::planBoot(BootFacts{Phase::Running, false, 600000, false}) == BootAction::None,
        "a timer still running carries on");
  check(steptimer::planBoot(BootFacts{Phase::Running, false, 0, false}) != BootAction::Acknowledge,
        "a running timer is not acknowledged whatever time it is given");
  check(steptimer::planBoot(BootFacts{Phase::Running, false, -5000, false}) != BootAction::Acknowledge,
        "nor one that is past its end");
}

void testTheWordsForTheLog() {
  const Rules rules = wakeRules();
  char out[160];
  Decision d;
  d.bookId = 12;
  d.bytes = 512344;
  d.verdict = Verdict::Download;
  d.neededMs = 53696;
  d.remainingMs = 61000;
  describe(rules, d, out, sizeof(out));
  checkEq(std::string(out), std::string("b12: 512344 B, download (needs 53696 ms, 61000 ms left)"), "download");
  d.verdict = Verdict::Battery;
  d.battery = 27;
  describe(rules, d, out, sizeof(out));
  checkEq(std::string(out), std::string("b12: left for the shelf, battery 27% is under 30%"), "battery");
  d.verdict = Verdict::Blocked;
  describe(rules, d, out, sizeof(out));
  checkEq(std::string(out), std::string("b12: left for the shelf, its download failed on 2 earlier wakes"), "blocked");
  d.verdict = Verdict::TooLarge;
  d.bytes = 1400000;
  describe(rules, d, out, sizeof(out));
  checkEq(std::string(out), std::string("b12: left for the shelf, 1400000 B is over the 1000000 B limit"), "too large");
  d.verdict = Verdict::Protected;
  describe(rules, d, out, sizeof(out));
  checkEq(std::string(out), std::string("b12: left for the shelf, filing it would delete the book on the panel"),
          "protected");
  d.verdict = Verdict::Limit;
  d.started = 2;
  describe(rules, d, out, sizeof(out));
  checkEq(std::string(out), std::string("b12: left for the shelf, 2 books already this wake"), "limit");
  d.verdict = Verdict::NoTime;
  d.neededMs = 51000;
  d.remainingMs = 40000;
  describe(rules, d, out, sizeof(out));
  checkEq(std::string(out), std::string("b12: left for the shelf, needs 51000 ms and 40000 ms are left"), "no time");

  char small[12];
  const size_t length = describe(rules, d, small, sizeof(small));
  checkEq(length, strlen(small), "a short buffer reports what it holds");
  checkEq(strlen(small), size_t{11}, "and is cut, not overrun");
  checkEq(describe(rules, d, nullptr, 10), size_t{0}, "no buffer, nothing written");
  checkEq(describe(rules, d, out, 0), size_t{0}, "no room, nothing written");

  for (unsigned i = 0; i <= num(Verdict::NoTime); ++i)
    check(strlen(verdictText(static_cast<Verdict>(i))) > 0, "verdict text");
  for (unsigned i = 0; i <= num(Gate::NoClock); ++i) check(strlen(gateText(static_cast<Gate>(i))) > 0, "gate text");
  checkEq(std::string(gateText(Gate::Battery)), std::string("battery is under the floor"), "gate text, battery");
}
}  // namespace

int main() {
  testTheConstantsAreWhatTheSpecSays();
  testTransferArithmetic();
  testTheBudgetBoundary();
  testTheBatteryFloor();
  testTheSizeCap();
  testTheBookCap();
  testTheBookOnThePanel();
  testTheRetryLimit();
  testTheOrderOfTheReasons();
  testTheGate();
  testAStepTimerCutsTheBudget();
  testTheBackoff();
  testATouchIsNotAPhantom();
  testAHeldKeyNeedsTheDebounceToCommit();
  testAFingerInTheLastWindowStillCounts();
  testWhatIsLeftDownAtTheCap();
  testTheHomeKeyLevel();
  testKeysAreStillTheLevelRule();
  testWithoutAPressNothingChanges();
  testAPressMakesTheWakeAButtonWake();
  testAPressAcknowledgesAFinishedStepTimer();
  testTheWordsForTheLog();
  std::cout << "method wake: " << checks << " checks passed\n";
  return 0;
}
