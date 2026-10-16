#include <ArduinoJson.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "StepTimer.h"
#include "StepTimerStore.h"

using namespace steptimer;

namespace {
int checks = 0;
void check(bool condition, const char* name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}
std::ostream& operator<<(std::ostream& out, const std::vector<int64_t>& list) {
  out << '[';
  for (size_t i = 0; i < list.size(); ++i) out << (i ? ", " : "") << list[i];
  return out << ']';
}
template <typename A, typename B>
void checkEq(const A& actual, const B& expected, const char* name) {
  if (!(actual == expected)) {
    std::cerr << "FAIL: " << name << "\n  actual:   " << actual << "\n  expected: " << expected << '\n';
    std::exit(1);
  }
  ++checks;
}

Record running(const std::string& path = "/Lamina/Method/Extract 11111111.xtc", unsigned page = 4) {
  Record r;
  r.phase = Phase::Running;
  r.bookPath = path;
  r.page = static_cast<uint16_t>(page);
  r.seconds = 300;
  r.label = "Incubate";
  r.boxX = 32;
  r.boxY = 540;
  r.boxW = 416;
  r.boxH = 156;
  r.endWallMs = 1'800'000'000'000LL;
  return r;
}

// ---- Showing the time -----------------------------------------------------------------------------------------

void testClockText() {
  checkEq(clockText(300), std::string("5:00"), "five minutes");
  checkEq(clockText(45), std::string("0:45"), "forty-five seconds");
  checkEq(clockText(0), std::string("0:00"), "zero");
  checkEq(clockText(-3), std::string("0:00"), "never negative");
  checkEq(clockText(59), std::string("0:59"), "a second under a minute");
  checkEq(clockText(60), std::string("1:00"), "a minute");
  checkEq(clockText(720), std::string("12:00"), "twelve minutes");
  checkEq(clockText(3599), std::string("59:59"), "a second under an hour");
  checkEq(clockText(3600), std::string("1:00:00"), "an hour");
  checkEq(clockText(5400), std::string("1:30:00"), "ninety minutes (the Mac prints it the same way)");
  checkEq(clockText(359999), std::string("99:59:59"), "the longest timer");
}

void testDisplaySeconds() {
  checkEq(displaySeconds(0), int64_t{0}, "none left");
  checkEq(displaySeconds(-5000), int64_t{0}, "past the end shows zero");
  checkEq(displaySeconds(1), int64_t{1}, "a millisecond left still shows a second");
  checkEq(displaySeconds(1000), int64_t{1}, "exactly a second");
  checkEq(displaySeconds(1001), int64_t{2}, "a millisecond over rounds up");
  checkEq(displaySeconds(299001), int64_t{300}, "just under five minutes shows 5:00 until it is under");
  checkEq(displaySeconds(300000), int64_t{300}, "five minutes");
}

void testCadence() {
  const struct {
    int64_t shown, next;
  } table[] = {{300, 240}, {240, 180}, {180, 120}, {120, 60}, {100, 60},    {90, 60}, {61, 60}, {60, 50},
               {59, 50},   {51, 50},   {50, 40},   {41, 40},  {40, 30},     {30, 20}, {20, 10}, {11, 10},
               {10, 0},    {9, 0},     {1, 0},     {0, 0},    {3600, 3540}, {-4, 0}};
  for (const auto& row : table) checkEq(nextShownSeconds(row.shown), row.next, "the next value on the display");

  // A whole 5:00 timer, watched every 100 ms: the display changes at exactly these values and no others.
  const auto watch = [](int64_t seconds) {
    std::vector<int64_t> shown;
    int64_t current = seconds;
    shown.push_back(current);
    for (int64_t remaining = seconds * 1000; remaining >= 0; remaining -= 100) {
      if (tickDue(current, remaining)) {
        current = displaySeconds(remaining);
        shown.push_back(current);
      }
    }
    return shown;
  };
  checkEq(watch(300), (std::vector<int64_t>{300, 240, 180, 120, 60, 50, 40, 30, 20, 10, 0}),
          "five minutes: each minute, then every ten seconds in the last, then zero");
  checkEq(watch(330), (std::vector<int64_t>{330, 270, 210, 150, 90, 60, 50, 40, 30, 20, 10, 0}),
          "5:30: minutes keep the half, and 1:00 is never skipped");
  checkEq(watch(95), (std::vector<int64_t>{95, 60, 50, 40, 30, 20, 10, 0}), "1:35 goes straight to 1:00");
  checkEq(watch(45), (std::vector<int64_t>{45, 40, 30, 20, 10, 0}), "45 s: tens from the start");
  checkEq(watch(7), (std::vector<int64_t>{7, 0}), "7 s: only the end");
  checkEq(watch(1), (std::vector<int64_t>{1, 0}), "1 s");
  check(watch(3600).size() == 1 + 59 + 6 + 0 || watch(3600).size() == 1 + 58 + 1 + 6,
        "an hour: a refresh a minute, not a second");

  check(!tickDue(300, 300000) && !tickDue(300, 240001) && tickDue(300, 240000) && tickDue(300, 239999),
        "due the moment the time left reaches the next value");
  check(tickDue(300, 100000), "a long stall still catches up in one refresh");
  check(!tickDue(0, 0) && !tickDue(0, -1000), "once at zero nothing more is due");
  check(!tickDue(10, 10000) && tickDue(10, 0) && tickDue(10, -1) && !tickDue(10, 1), "the last ten seconds");
  check(timerDue(0) && timerDue(-1) && !timerDue(1), "the timer is due at zero");
}

// ---- Sleeping -------------------------------------------------------------------------------------------------

void testNextWake() {
  const uint64_t POLL = 5ull * 60 * 1000000;
  checkEq(nextWakeMicros(0, false, 0), uint64_t{0}, "nothing to wake for");
  checkEq(nextWakeMicros(POLL, false, 0), POLL, "only the poll");
  checkEq(nextWakeMicros(POLL, false, 1000), POLL, "a timer that is not running is ignored");
  checkEq(nextWakeMicros(0, true, 600000), uint64_t{598} * 1000000, "only the timer: its end less the lead");
  checkEq(nextWakeMicros(POLL, true, 27 * 60 * 1000), POLL,
          "a 27 minute timer under a 5 minute poll wakes for the poll first");
  checkEq(nextWakeMicros(POLL, true, 90000), uint64_t{88} * 1000000, "the timer ends before the next poll");
  checkEq(nextWakeMicros(POLL, true, 302000), POLL,
          "the poll and the timer 2 s apart: the poll is not later than the timer");
  checkEq(nextWakeMicros(POLL, true, 301999), uint64_t{299999} * 1000, "just inside the poll");
  checkEq(nextWakeMicros(0, true, 2000), MIN_WAKE_US, "an end within the lead wakes at the minimum");
  checkEq(nextWakeMicros(0, true, 0), MIN_WAKE_US, "a timer already due wakes at once");
  checkEq(nextWakeMicros(POLL, true, -50000), MIN_WAKE_US, "a timer long past does too");
  checkEq(nextWakeMicros(0, true, 3000), MIN_WAKE_US, "an end 3 s away: not sooner than the minimum");
  checkEq(nextWakeMicros(0, true, 2500), MIN_WAKE_US, "an end 2.5 s away would be 0.5 s: the minimum instead");
  checkEq(nextWakeMicros(0, true, 2001), MIN_WAKE_US, "a millisecond past the lead: the minimum");
  checkEq(nextWakeMicros(0, true, 3001), uint64_t{1001} * 1000, "and a ms later is just over it");
  checkEq(nextWakeMicros(0, true, int64_t{359999} * 1000), uint64_t{359997} * 1000000, "the longest timer");
}

// ---- Booting --------------------------------------------------------------------------------------------------

void testPlanBoot() {
  const auto plan = [](Phase phase, int64_t remaining, bool timerWake, bool lost = false) {
    BootFacts f;
    f.phase = phase;
    f.remainingMs = remaining;
    f.timerWake = timerWake;
    f.powerLost = lost;
    return planBoot(f);
  };
  using A = BootAction;
  check(plan(Phase::None, 0, true) == A::None && plan(Phase::None, -99999, false) == A::None,
        "no timer, nothing to do");

  // Alerted: waits for a person.
  check(plan(Phase::Alerted, 0, false) == A::Acknowledge, "a person finds an alerted timer: it is cleared");
  check(plan(Phase::Alerted, -100000, false, true) == A::Acknowledge, "also after power was lost");
  check(plan(Phase::Alerted, 0, true) == A::None, "an ambient poll leaves it for the next person");

  // Running, the timer ended while the reader slept.
  check(plan(Phase::Running, 0, true) == A::AlertPulse, "woke exactly at the end: alert with the light");
  check(plan(Phase::Running, -3000, true) == A::AlertPulse, "woke 3 s late: alert");
  check(plan(Phase::Running, -(ALERT_LIMIT_MS - 1), true) == A::AlertPulse,
        "a millisecond inside the limit: still pulses");
  check(plan(Phase::Running, -ALERT_LIMIT_MS, true) == A::AlertQuiet, "at the limit: nobody is waiting, no light");
  check(plan(Phase::Running, -3600000, true) == A::AlertQuiet, "an hour late: quiet");
  check(plan(Phase::Running, 0, false) == A::AlertQuiet, "a person woke the reader after the end: quiet");
  check(plan(Phase::Running, -1, false) == A::AlertQuiet, "a millisecond after the end, by hand: quiet");

  // Running, the end is close (the wake timer is set a little early).
  check(plan(Phase::Running, 1, true) == A::AlertPulse && plan(Phase::Running, WAKE_SLACK_MS, true) == A::AlertPulse,
        "woke just before the end: the alert will fire at zero");
  check(plan(Phase::Running, WAKE_SLACK_MS + 1, true) == A::None, "a poll wake a little further out carries on");
  check(plan(Phase::Running, 1, false) == A::None && plan(Phase::Running, 600000, false) == A::None,
        "a person wakes it while it runs: it carries on");
  check(plan(Phase::Running, 600000, true) == A::None, "an ambient poll mid-timer: it carries on");

  // Power lost: the end cannot be known.
  check(plan(Phase::Running, 600000, false, true) == A::AlertQuiet &&
            plan(Phase::Running, 600000, true, true) == A::AlertQuiet,
        "power was lost while it ran: say so quietly, whatever the clock claims");
  check(plan(Phase::Running, -5, false, true) == A::AlertQuiet, "power lost and overdue");
}

// ---- The alert's light ----------------------------------------------------------------------------------------

void testAlertLight() {
  check(!alertLight(0, false).on && !alertLight(349, false).on, "first, a dark half flash");
  check(alertLight(350, false).on && alertLight(699, false).on, "then a lit one");
  check(!alertLight(700, false).on && alertLight(1050, false).on && !alertLight(1400, false).on &&
            alertLight(1750, false).on,
        "three flashes");
  check(alertLight(2100, false).on && alertLight(10000, false).on && alertLight(29999, false).on, "then steady on");
  check(!alertLight(30000, false).on && alertLight(30350, false).on && !alertLight(30700, false).on,
        "the flashes come back every 30 s");
  check(alertLight(32100, false).on && alertLight(59999, false).on, "and the light is steady again after them");
  check(!alertLight(60000, false).on, "the next burst");
  check(!alertLight(ALERT_LIMIT_MS - 1, false).ended && alertLight(ALERT_LIMIT_MS, false).ended, "ends at the limit");
  check(!alertLight(ALERT_LIMIT_MS, false).on && !alertLight(ALERT_LIMIT_MS + 100000, false).on,
        "dark after the limit");
  for (int64_t t = 0; t < ALERT_LIMIT_MS; t += 337)
    check(!alertLight(t, true).on && !alertLight(t, true).ended, "quiet is dark throughout");
  check(alertLight(ALERT_LIMIT_MS, true).ended, "and a quiet alert ends at the limit too");
  check(!alertLight(-5, false).on, "before it begins: dark");
  // A steady light would be 10 minutes at full draw; the flashes make up a small part of it.
  int64_t lit = 0;
  for (int64_t t = 0; t < ALERT_LIMIT_MS; t += 50) lit += alertLight(t, false).on ? 50 : 0;
  check(lit > ALERT_LIMIT_MS * 9 / 10 && lit < ALERT_LIMIT_MS, "mostly steady on");

  checkEq(int{alertBrightness(0)}, 30, "never darker than 30 percent");
  checkEq(int{alertBrightness(29)}, 30, "29 rounds up");
  checkEq(int{alertBrightness(30)}, 30, "30 stays");
  checkEq(int{alertBrightness(50)}, 50, "the user's level in range");
  checkEq(int{alertBrightness(70)}, 70, "70 stays");
  checkEq(int{alertBrightness(71)}, 70, "never brighter than 70 percent");
  checkEq(int{alertBrightness(100)}, 70, "full is 70");
  check(lightAllowed(10) && lightAllowed(100) && !lightAllowed(9) && !lightAllowed(0), "no light below 10 percent");
}

// ---- Starting -------------------------------------------------------------------------------------------------

void testDecideStart() {
  Record none;
  check(decideStart(none, "/Lamina/Method/A aaaaaaaa.xtc", 3) == StartDecision::Start, "nothing runs: start");
  Record alerted = running();
  alerted.phase = Phase::Alerted;
  check(decideStart(alerted, "/Lamina/Method/A aaaaaaaa.xtc", 3) == StartDecision::Start,
        "an alerted record is not running");
  const Record r = running();
  check(decideStart(r, r.bookPath, 4) == StartDecision::AlreadyHere, "this timer is the one running");
  check(decideStart(r, r.bookPath, 5) == StartDecision::AskReplace, "another page of the same book: ask");
  check(decideStart(r, "/Lamina/Method/Other aaaaaaaa.xtc", 4) == StartDecision::AskReplace,
        "the same page of another book: ask");
}

// ---- The record -----------------------------------------------------------------------------------------------

void testRecordValid() {
  check(recordValid(Record()), "an empty record is valid");
  Record junk;
  junk.bookPath = "../../etc";
  junk.seconds = 0;
  check(recordValid(junk), "phase None ignores the rest");
  check(recordValid(running()), "a running record");
  const auto bad = [](auto change, const char* name) {
    Record r = running();
    change(r);
    check(!recordValid(r), name);
  };
  bad([](Record& r) { r.bookPath = "/Books/x.xtc"; }, "a book outside the Method folder");
  bad([](Record& r) { r.bookPath = "/Lamina/Method/../x.xtc"; }, "a path that climbs out");
  bad([](Record& r) { r.bookPath.clear(); }, "no book");
  bad([](Record& r) { r.page = 0; }, "page 0");
  bad([](Record& r) { r.seconds = 0; }, "zero seconds");
  bad([](Record& r) { r.seconds = 360000; }, "past 99:59:59");
  bad([](Record& r) { r.label = std::string(161, 'x'); }, "a label over 160 bytes");
  bad([](Record& r) { r.boxW = 23; }, "a box under 24 wide");
  bad([](Record& r) { r.boxH = 23; }, "a box under 24 tall");
  bad([](Record& r) { r.boxX = 100; }, "a box past the right edge (100 + 416)");
  bad([](Record& r) { r.boxY = 700; }, "a box past the bottom edge");
  bad([](Record& r) { r.endWallMs = 0; }, "a running timer with no end");
  bad([](Record& r) { r.phase = static_cast<Phase>(9); }, "an unknown phase");
  Record alerted = running();
  alerted.phase = Phase::Alerted;
  alerted.endWallMs = 0;
  check(recordValid(alerted), "an alerted record needs no end");
  Record edge = running();
  edge.boxX = 480 - 24;
  edge.boxY = 800 - 24;
  edge.boxW = 24;
  edge.boxH = 24;
  check(recordValid(edge), "the smallest box in the last corner");
}

// ---- Geometry -------------------------------------------------------------------------------------------------

bool inside(const Rect& inner, const Rect& outer) {
  return inner.empty() || (inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() &&
                           inner.bottom() <= outer.bottom());
}
bool overlap(const Rect& a, const Rect& b) {
  return !a.empty() && !b.empty() && a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

void testTouchTargets() {
  const Rect big{32, 540, 416, 156};
  check(touchTarget(big) == big, "a box already big enough is its own target");
  const Rect small{200, 400, 24, 24};
  const Rect t = touchTarget(small);
  check(t.w == MIN_TARGET_W && t.h == MIN_TARGET_H && t.contains(200, 400) && t.contains(223, 423),
        "a 24 px box gets a 120 x 64 target around it");
  check(t.x + t.w / 2 >= 210 && t.x + t.w / 2 <= 214 && t.y + t.h / 2 >= 410 && t.y + t.h / 2 <= 414,
        "centred on the box");
  for (const Rect corner : {Rect{0, 0, 24, 24}, Rect{456, 0, 24, 24}, Rect{0, 776, 24, 24}, Rect{456, 776, 24, 24}}) {
    const Rect c = touchTarget(corner);
    check(inside(c, Rect{0, 0, PAGE_W, PAGE_H}) && c.contains(corner.x, corner.y) &&
              c.contains(corner.x + 23, corner.y + 23),
          "a target at a page corner stays on the page and covers the box");
  }
  const Rect wideThin{32, 100, 416, 30};
  const Rect wt = touchTarget(wideThin);
  check(wt.w == 416 && wt.h == 64 && wt.contains(32, 100) && wt.contains(447, 129),
        "a wide thin box grows only in height");
  const Rect chip = chipRect();
  check(touchTarget(chip).h == 64 && touchTarget(chip).y == 0 && touchTarget(chip).contains(chip.x, chip.y),
        "the chip's target reaches the top edge");
  check(inside(chipRect(), Rect{0, 0, PAGE_W, PAGE_H}) && inside(bannerRect(), Rect{0, 0, PAGE_W, PAGE_H}),
        "chip and banner are on the page");
  check(chipRect().x >= 120 && chipRect().right() <= 360, "the chip sits in the free middle of the header row");
}

void testLayouts() {
  for (const Rect box :
       {Rect{32, 540, 416, 156}, Rect{32, 610, 416, 96}, Rect{32, 400, 416, 64}, Rect{32, 400, 200, 40},
        Rect{0, 0, 24, 24}, Rect{100, 100, 60, 30}, Rect{32, 6, 416, 76}, Rect{10, 10, 300, 120}}) {
    const BoxLayout l = layoutBox(box);
    check(inside(l.label, box) && inside(l.digits, box) && inside(l.hint, box), "every part is inside the box");
    check(!overlap(l.label, l.digits) && !overlap(l.digits, l.hint) && !overlap(l.label, l.hint), "and no two overlap");
    check(l.digits.w > 0 && l.digits.h > 0, "the digits always have room");
    check(l.digits.x >= box.x + l.frameWidth && l.digits.right() <= box.right() - l.frameWidth &&
              l.digits.y >= box.y + l.frameWidth && l.digits.bottom() <= box.bottom() - l.frameWidth,
          "the digits clear the frame");
  }
  const BoxLayout full = layoutBox({32, 540, 416, 156});
  check(!full.label.empty() && !full.hint.empty() && full.digits.h >= 70,
        "the Mac's 156 px box: label, hint and 70 px or more of digits");
  check(full.frameWidth == 3, "the same 3 px frame as the page");
  const BoxLayout mid = layoutBox({32, 610, 416, 96});
  check(!mid.label.empty() && mid.hint.empty() && mid.digits.h >= 50, "a 96 px box: a label and 50 px digits");
  const BoxLayout tiny = layoutBox({0, 0, 24, 24});
  check(tiny.label.empty() && tiny.hint.empty() && !tiny.digits.empty(), "a 24 px box: digits only");

  const ChipLayout chip = layoutChip(chipRect());
  check(inside(chip.digits, chipRect()) && inside(chip.label, chipRect()) && !overlap(chip.digits, chip.label),
        "chip parts fit and do not overlap");
  check(chip.digits.h >= 24 && chip.label.w >= 100, "the chip's digits are 24 px or more and its label has room");
}

void testClockGlyphs() {
  const Rect area{50, 100, 380, 84};
  const int expectedSegments[10] = {6, 2, 5, 5, 4, 5, 6, 3, 7, 6};
  for (int d = 0; d < 10; ++d) {
    std::vector<Rect> rects;
    const int h = clockGlyphs(std::string(1, static_cast<char>('0' + d)), area, rects);
    checkEq(static_cast<int>(rects.size()), expectedSegments[d], "a digit lights the right number of segments");
    check(h == 84, "a single digit is as tall as the area");
    for (const Rect& r : rects) check(inside(r, area) && !r.empty(), "every segment is inside the area");
  }
  std::vector<Rect> colon;
  clockGlyphs(":", area, colon);
  checkEq(static_cast<int>(colon.size()), 2, "a colon is two dots");
  std::vector<Rect> blank;
  checkEq(clockGlyphs("", area, blank), 0, "nothing to draw");
  checkEq(clockGlyphs("5:00", Rect{0, 0, 0, 0}, blank), 0, "no room");
  check(blank.empty(), "and nothing is added");

  // As large as fits, and always fitting: either nothing is drawn, or all of it is inside the area.
  for (const std::string text : {"5:00", "12:00", "59:59", "1:05:00", "99:59:59", "0:00", "14:32"}) {
    for (const Rect a : {Rect{0, 0, 380, 84}, Rect{0, 0, 100, 26}, Rect{0, 0, 52, 20}, Rect{0, 0, 400, 400},
                         Rect{10, 20, 150, 60}, Rect{0, 0, 16, 16}, Rect{5, 5, 30, 13}}) {
      std::vector<Rect> rects;
      const int h = clockGlyphs(text, a, rects);
      check(clockFits(text, a) == (h > 0), "clockFits says what clockGlyphs did");
      if (h == 0) {
        check(rects.empty(), "when it does not fit nothing is drawn");
        continue;
      }
      check(h >= MIN_GLYPH_H && h <= a.h, "the figures fit the height");
      int left = 9999, right = -1;
      for (const Rect& r : rects) {
        check(inside(r, a), "every segment stays inside the area");
        left = std::min(left, r.x);
        right = std::max(right, r.right());
      }
      check(right - left <= a.w, "and the width");
      // A leading 1 has its ink on the right of its cell, as on any seven-segment display.
      check(text[0] == '1' || std::abs((left - a.x) - (a.right() - right)) <= 4, "centred, give or take a gap");
    }
  }
  check(!clockFits("5:00", Rect{0, 0, 16, 16}), "a 24 px page box has no room for its clock");
  check(clockFits("5:00", Rect{0, 0, 100, 26}) && clockFits("1:05:00", Rect{0, 0, 104, 26}),
        "the chip's digits hold any timer's clock");
  check(clockFits("99:59:59", Rect{0, 0, 100, 26}), "even the longest, smaller");
  std::vector<Rect> big;
  const int bigH = clockGlyphs("5:00", Rect{0, 0, 380, 84}, big);
  std::vector<Rect> longer;
  const int longH = clockGlyphs("99:59:59", Rect{0, 0, 300, 84}, longer);
  check(bigH == 84 && longH > 40 && longH < 84, "a long time is drawn smaller to fit, not cut");

  // Monospaced: every digit takes the same width, so the clock does not jitter as it counts down. The colons, which
  // follow digits, land in the same place whatever those digits are.
  const auto colons = [&](const std::string& text) {
    std::vector<Rect> rects, found;
    clockGlyphs(text, Rect{0, 0, 380, 84}, rects);
    for (const Rect& r : rects)
      if (r.w == r.h && r.w < 20) found.push_back(r);  // a colon dot is a square no bigger than the segment thickness
    return found;
  };
  check(colons("11:11") == colons("88:88") && colons("00:00") == colons("59:59") && colons("1:11") == colons("8:88") &&
            colons("11:11:11") == colons("00:00:00") && colons("11:11").size() == 2,
        "the same width whatever the digits");
  check(!(colons("1:11") == colons("11:11")), "while a longer time is wider");
  // Lining: the digits share one top and one bottom.
  for (const std::string text : {"8", "1", "0", "7"}) {
    std::vector<Rect> rects;
    clockGlyphs(text, Rect{0, 0, 380, 84}, rects);
    int top = 9999, bottom = -1;
    for (const Rect& r : rects) {
      top = std::min(top, r.y);
      bottom = std::max(bottom, r.bottom());
    }
    // 1 and 7 do not use the lower or upper bars everywhere, but the full height is spanned by the right-hand bars.
    check(bottom - top <= 84 && (text == "8" || text == "0" ? bottom - top == 84 : bottom - top >= 40),
          "digits are drawn at the same height");
  }
}

// ---- The whole life of a timer, with a fake clock --------------------------------------------------------------

// A reader with the two clocks the firmware has. The counter (mono) restarts at every boot, as esp_timer does; the wall
// clock runs through sleep and can be stepped by a sync, or have started anywhere because it was never set.
struct Sim {
  Record record;
  Clockwork clock{record};
  int64_t trueMs = 0;      // real elapsed milliseconds since the scenario began
  int64_t wallOffsetMs;    // wall = true + offset (a sync changes the offset, nothing else)
  int64_t bootTrueMs = 0;  // when this boot began
  uint64_t armedUs = 0;    // the wake timer set on the way to sleep (0 = none)
  bool asleep = false;

  explicit Sim(int64_t wallStartMs) : wallOffsetMs(wallStartMs) {}

  Now now() const { return {(trueMs - bootTrueMs) * 1000 + 1'500'000, trueMs + wallOffsetMs}; }
  void awake(int64_t ms) { trueMs += ms; }
  void syncClock(int64_t stepMs) { wallOffsetMs += stepMs; }  // SNTP: the wall clock jumps, real time does not
  int64_t remaining() const { return clock.remainingMs(now().monoUs); }

  void start(unsigned seconds, unsigned page = 4, unsigned panelPage = 4) {
    clock.start("/Lamina/Method/Extract 11111111.xtc", page, seconds, "Incubate", Rect{32, 540, 416, 156}, now(),
                panelPage);
  }
  // The way into sleep: the wall-clock end is rewritten and the wake timer chosen.
  void sleep(uint64_t pollUs, unsigned panelPage = 4) {
    clock.beforeSleep(now(), panelPage);
    armedUs = clock.wakeMicros(pollUs, now().monoUs);
    asleep = true;
  }
  // The wake timer fires (or the poll does): the reader boots. Returns what the boot would do about the timer.
  BootAction timerWake() {
    trueMs += static_cast<int64_t>(armedUs / 1000);
    return boot(true);
  }
  BootAction buttonWake(int64_t afterMs) {
    trueMs += afterMs;
    return boot(false);
  }
  BootAction boot(bool timer, bool powerLost = false) {
    asleep = false;
    bootTrueMs = trueMs;
    return planBoot(clock.boot(now(), powerLost, timer));
  }
};

void testScenarioShortTimerAsleepAllTheWay() {
  Sim d(1'790'000'000'000LL);
  d.start(300);
  d.awake(2000);  // the user turns the page, then the reader sleeps
  d.sleep(0);
  check(d.armedUs == (300000 - 2000 - 2000) * 1000ull, "armed for the end less the lead, no poll");
  const BootAction action = d.timerWake();
  check(action == BootAction::AlertPulse, "the wake at the end is the alert");
  check(d.remaining() <= WAKE_SLACK_MS && d.remaining() >= -1000, "it lands within a second of zero");
  d.clock.markAlerted();
  check(d.record.phase == Phase::Alerted && !d.clock.running(), "alerted: no longer running");
  d.sleep(5ull * 60 * 1000000);
  check(d.armedUs == 5ull * 60 * 1000000, "an alerted timer asks for no wake of its own");
  check(d.buttonWake(3'600'000) == BootAction::Acknowledge, "whoever wakes the reader next clears it");
  d.clock.clear();
  check(d.record.phase == Phase::None && d.record.bookPath.empty(), "and it is gone");
}

void testScenarioPollsInBetween() {
  const uint64_t POLL = 5ull * 60 * 1000000;
  Sim d(1'790'000'000'000LL);
  d.start(27 * 60);
  d.sleep(POLL);
  check(d.armedUs == POLL, "the first wake is the poll");
  int polls = 0;
  BootAction action = BootAction::None;
  while (true) {
    action = d.timerWake();
    if (action != BootAction::None) break;
    ++polls;
    check(d.remaining() > WAKE_SLACK_MS, "a poll wake leaves the timer running");
    d.awake(22000);  // Wi-Fi, a fetch, maybe the first sync in days: the wall clock jumps three hours
    if (polls == 1) d.syncClock(3 * 3600 * 1000LL);
    if (polls == 2) d.syncClock(-1500);  // a small correction the other way
    d.sleep(POLL);
    check(polls < 10, "the polls end");
  }
  check(polls == 5, "five polls fit in 27 minutes, then the timer's own wake");
  check(action == BootAction::AlertPulse, "the wake after them is the alert");
  // True end: 27 min after the start. Elapsed true time now vs that: within a second, whatever the syncs did.
  const int64_t late = d.trueMs - 27 * 60 * 1000;
  check(late >= -WAKE_SLACK_MS && late <= 1000, "and it is on time, not moved by the clock steps");
}

void testScenarioAwakeThroughASync() {
  Sim d(1'000'000);  // never synced: the wall clock holds a few minutes
  d.start(600);
  d.awake(100'000);
  d.syncClock(1'790'000'000'000LL - 1'000'000);  // the first sync ever: the wall clock jumps to 2026
  check(d.remaining() == 500'000, "the countdown does not notice the clock step");
  d.awake(200'000);
  d.sleep(0);
  check(d.record.endWallMs == 1'790'000'000'000LL + 600'000,
        "the wall end was rewritten on the way to sleep, in the new clock");
  check(d.armedUs == (300'000 - 2000) * 1000ull, "and the wake is 5 minutes (less the lead) away");
  check(d.timerWake() == BootAction::AlertPulse, "alert at the end");
}

void testScenarioWallClockNeverSet() {
  Sim d(5000);  // a clock that has never been set: seconds since power-on
  d.start(120);
  d.sleep(0);
  check(d.timerWake() == BootAction::AlertPulse, "the arithmetic does not need a real date");
}

void testScenarioOtherWakes() {
  {  // The user wakes the reader mid-timer: it carries on.
    Sim d(1'790'000'000'000LL);
    d.start(600);
    d.sleep(0);
    check(d.buttonWake(60'000) == BootAction::None && d.clock.running() && d.remaining() > 530'000 &&
              d.remaining() < 541'000,
          "a button wake a minute in: still running, about nine minutes left");
    d.awake(10'000);
    d.sleep(0);
    check(d.armedUs > 520ull * 1000000 && d.armedUs < 530ull * 1000000, "and it sleeps again with the right wake");
  }
  {  // The user wakes it after the end: quiet.
    Sim d(1'790'000'000'000LL);
    d.start(60);
    d.sleep(0);
    check(d.buttonWake(90'000) == BootAction::AlertQuiet, "woken by hand 30 s late: said quietly");
  }
  {  // The wake timer is late (held button, slow boot): still the alert.
    Sim d(1'790'000'000'000LL);
    d.start(60);
    d.sleep(0);
    d.awake(4000);  // the wake runs 4 s late
    check(d.timerWake() == BootAction::AlertPulse, "4 s late is still a pulse");
  }
  {  // The wake never came (SD error on the timer wake slept without one); a person comes back 20 minutes late.
    Sim d(1'790'000'000'000LL);
    d.start(60);
    d.sleep(0);
    check(d.buttonWake(20 * 60'000) == BootAction::AlertQuiet, "twenty minutes late: quiet");
    Sim e(1'790'000'000'000LL);
    e.start(60);
    e.sleep(0);
    e.trueMs += 60'000 + ALERT_LIMIT_MS;  // a timer wake that is a whole limit late
    check(e.boot(true) == BootAction::AlertQuiet, "a timer wake a whole alert limit late: quiet");
    Sim f(1'790'000'000'000LL);
    f.start(60);
    f.sleep(0);
    f.trueMs += 60'000 + ALERT_LIMIT_MS - 1000;
    check(f.boot(true) == BootAction::AlertPulse, "a second inside the limit: pulses");
  }
  {  // Power lost while it ran.
    Sim d(1'790'000'000'000LL);
    d.start(3600);
    d.sleep(0);
    d.trueMs += 30 * 60'000;
    check(d.boot(false, true) == BootAction::AlertQuiet, "power came back: said quietly, whatever the clock says");
    Sim e(1'790'000'000'000LL);
    e.start(3600);
    e.sleep(0);
    e.trueMs += 20 * 60'000;
    e.syncClock(-3 * 3600 * 1000LL);  // the clock restarted behind where it was
    check(e.boot(false) == BootAction::AlertQuiet, "a wall clock gone back by hours is taken as lost power too");
    Sim f(1'790'000'000'000LL);
    f.start(3600);
    f.sleep(0);
    f.trueMs += 20 * 60'000;
    f.syncClock(-30'000);  // a modest correction is not a power loss
    check(f.boot(false) == BootAction::None, "while a modest correction is not");
  }
  {  // Stopped before sleeping: nothing wakes the reader.
    Sim d(1'790'000'000'000LL);
    d.start(600);
    d.clock.clear();
    d.sleep(5ull * 60 * 1000000);
    check(d.armedUs == 5ull * 60 * 1000000 && !d.clock.running(), "a stopped timer asks for nothing");
    d.sleep(0);
    check(d.armedUs == 0, "and with no poll the reader sleeps without a wake timer");
  }
  {  // Starting another replaces the first.
    Sim d(1'790'000'000'000LL);
    d.start(600);
    d.awake(60'000);
    d.start(120, 6, 6);
    check(d.record.page == 6 && d.record.seconds == 120 && d.remaining() == 120'000,
          "the new timer runs from its own start");
  }
  {  // Many short sleeps in a row do not walk the end.
    Sim d(1'790'000'000'000LL);
    d.start(1800);
    for (int i = 0; i < 100; ++i) {
      d.awake(800);
      d.sleep(0);
      d.trueMs += 7000;  // asleep seven seconds
      d.boot(false);
    }
    const int64_t expected = 1'800'000 - 100 * (800 + 7000);
    check(std::abs(d.remaining() - (expected - 1500)) <= 1500,
          "a hundred sleep/wake cycles keep the end within a second or two");
    check(std::abs(d.record.endWallMs - (1'790'000'000'000LL + 1'800'000)) <= 1, "the wall end does not drift");
  }
  {  // The panel page is recorded for the wake.
    Sim d(1'790'000'000'000LL);
    d.start(60, 4, 4);
    d.sleep(0, 7);
    check(d.record.panelPage == 7, "the page on the panel is remembered");
    d.sleep(0, 0);
    check(d.record.panelPage == 0, "or that it is unknown");
  }
}

void testRecordRoundTripKeepsTheTimer() {
  Sim d(1'790'000'000'000LL);
  d.start(900);
  d.sleep(0);
  // The record as the card holds it is all the next boot has: a fresh Clockwork over a copy of it.
  Record onCard = d.record;
  Clockwork next(onCard);
  const Now later{1'500'000, d.now().wallMs + 600'000};  // booted 10 minutes later
  const BootFacts facts = next.boot(later, false, false);
  check(facts.phase == Phase::Running && !facts.powerLost && facts.remainingMs > 295'000 && facts.remainingMs < 305'000,
        "a boot over the card's copy finds the right time left");
}

// ---- The record as written down -----------------------------------------------------------------------------------

void testStore() {
  Record r = running();
  r.label = "Incubate \"hard\" \xC2\xB5L";
  r.endWallMs = 1'790'123'456'789LL;  // past 32 bits: it goes through the text as a string
  r.panelPage = 5;
  const std::string text = recordToJson(r);
  check(!text.empty() && text.size() <= MAX_RECORD_JSON_BYTES, "a running record is written");
  const Record back = recordFromJson(text);
  check(back.phase == Phase::Running && back.bookPath == r.bookPath && back.page == 4 && back.seconds == 300 &&
            back.label == r.label && back.boxX == 32 && back.boxY == 540 && back.boxW == 416 && back.boxH == 156 &&
            back.endWallMs == r.endWallMs && back.panelPage == 5,
        "every field round-trips, a 64-bit end included");
  check(recordToJson(Record()).empty() && recordFromJson("").phase == Phase::None, "no timer is an empty string");
  Record alerted = r;
  alerted.phase = Phase::Alerted;
  check(recordFromJson(recordToJson(alerted)).phase == Phase::Alerted, "an alerted record round-trips");

  // The worst case fits the bound: the longest path and a label of 40 characters that all need escaping.
  Record worst = running(std::string("/Lamina/Method/") + std::string(76, 'a') + ".xtc");
  worst.label = std::string(40, '"');
  worst.endWallMs = INT64_MAX;
  worst.seconds = 359999;
  check(recordToJson(worst).size() <= MAX_RECORD_JSON_BYTES && recordFromJson(recordToJson(worst)).label == worst.label,
        "the worst record fits");

  // Whatever is on the card, a damaged record is no record.
  const std::string ok =
      "{\"v\":1,\"phase\":1,\"book\":\"/Lamina/Method/A "
      "aaaaaaaa.xtc\",\"page\":4,\"seconds\":60,\"box\":[32,540,416,156],\"end\":\"1790000000000\"}";
  check(recordFromJson(ok).phase == Phase::Running, "a minimal record reads");
  for (const char* bad :
       {"", "{", "[]", "5", "null", "{}", "{\"phase\":0}", "{\"phase\":3}", "{\"phase\":\"1\"}",
        "{\"phase\":1,\"book\":\"/Books/x.xtc\",\"page\":4,\"seconds\":60,\"box\":[32,540,416,156],\"end\":\"1\"}",
        "{\"phase\":1,\"book\":\"/Lamina/Method/A "
        "aaaaaaaa.xtc\",\"page\":0,\"seconds\":60,\"box\":[32,540,416,156],\"end\":\"1\"}",
        "{\"phase\":1,\"book\":\"/Lamina/Method/A "
        "aaaaaaaa.xtc\",\"page\":4,\"seconds\":0,\"box\":[32,540,416,156],\"end\":\"1\"}",
        "{\"phase\":1,\"book\":\"/Lamina/Method/A "
        "aaaaaaaa.xtc\",\"page\":4,\"seconds\":60,\"box\":[32,540,416],\"end\":\"1\"}",
        "{\"phase\":1,\"book\":\"/Lamina/Method/A "
        "aaaaaaaa.xtc\",\"page\":4,\"seconds\":60,\"box\":[32,540,416,156],\"end\":\"0\"}",
        "{\"phase\":1,\"book\":\"/Lamina/Method/A "
        "aaaaaaaa.xtc\",\"page\":4,\"seconds\":60,\"box\":[400,540,416,156],\"end\":\"1\"}"})
    check(recordFromJson(bad).phase == Phase::None, "a record this reader did not write reads as no timer");
  check(recordFromJson(std::string(MAX_RECORD_JSON_BYTES + 1, ' ')).phase == Phase::None,
        "an oversized record is no record");
  std::string deep = ok;
  deep.insert(deep.size() - 1, ",\"x\":[[[[1]]]]");
  check(recordFromJson(deep).phase == Phase::None, "nesting past the limit is refused");
}

void testClockworkGuards() {
  {  // Only a running timer is touched by sleeping or alerting; an empty or finished record stays as it is.
    Record empty;
    Clockwork none(empty);
    none.markAlerted();
    none.beforeSleep(Now{5'000'000, 1'790'000'000'000LL}, 7);
    check(empty.phase == Phase::None, "an empty record stays empty through alert and sleep: no phase");
    check(empty.endWallMs == 0, "an empty record stays empty through alert and sleep: no end");
    check(empty.bookPath.empty(), "an empty record stays empty through alert and sleep: no book");
    check(empty.panelPage == 7, "though the page on the panel is noted");
    Record alerted = running();
    alerted.phase = Phase::Alerted;
    alerted.endWallMs = 1'777'000'000'000LL;
    Clockwork done(alerted);
    done.beforeSleep(Now{5'000'000, 1'790'000'000'000LL}, 3);
    check(alerted.endWallMs == 1'777'000'000'000LL && alerted.phase == Phase::Alerted,
          "an alerted record is not given a new end");
  }
  {  // A modest correction the other way is not a power loss: up to a minute more than the timer's whole length is
     // allowed.
    Sim d(1'790'000'000'000LL);
    d.start(3600);
    d.sleep(0);
    d.trueMs += 10'000;
    d.syncClock(-30'000);  // remaining now reads 3620 s on a 3600 s timer
    const BootFacts facts = d.clock.boot(d.now(), false, false);
    check(!facts.powerLost && facts.remainingMs > 3'600'000,
          "20 s over the timer's length is a clock correction, not lost power");
    Sim e(1'790'000'000'000LL);
    e.start(3600);
    e.sleep(0);
    e.syncClock(-61'000);
    check(e.clock.boot(e.now(), false, false).powerLost, "more than a minute over is not");
    check(e.clock.boot(e.now(), false, false).remainingMs <= 0,
          "and a lost timer reports no time left, so nothing waits for it");
  }

  {  // The counter's reading of the time left, to the millisecond, negative once past the end.
    Record r;
    Clockwork c(r);
    c.start("/Lamina/Method/A aaaaaaaa.xtc", 2, 1, "", Rect{0, 0, 100, 100}, Now{0, 1'790'000'000'000LL}, 2);
    checkEq(c.remainingMs(0), int64_t{1000}, "a second on the clock");
    checkEq(c.remainingMs(1'000'000), int64_t{0}, "exactly at the end");
    checkEq(c.remainingMs(999'000), int64_t{1}, "a millisecond before");
    checkEq(c.remainingMs(999'999), int64_t{0}, "less than a millisecond before reads as none left");
    checkEq(c.remainingMs(1'000'001), int64_t{-1}, "a microsecond past the end is already negative");
    checkEq(c.remainingMs(1'001'000), int64_t{-1}, "a millisecond past");
    checkEq(c.remainingMs(1'001'001), int64_t{-2}, "just over a millisecond past");
  }
  {  // The record written at the start is complete: a reboot that is not a sleep (a crash, a restart) finds the right
     // end.
    Sim d(1'790'000'000'000LL);
    d.start(600);
    Record onCard = d.record;
    Clockwork next(onCard);
    const BootFacts facts = next.boot(Now{1'500'000, d.now().wallMs + 30'000}, false, false);
    check(!facts.powerLost && facts.remainingMs == 570'000,
          "a timer started 30 s ago has 570 s left on the card's copy, with no sleep in between");
    check(planBoot(facts) == BootAction::None, "and a reboot mid-timer carries on");
  }
  {  // Power lost: nothing may wait for a timer whose end cannot be known, however far off the stale end looks.
    Sim d(1'790'000'000'000LL);
    d.start(3600);
    d.sleep(0);
    const BootFacts facts = d.clock.boot(d.now(), true, false);
    check(facts.powerLost && facts.remainingMs <= 0, "power lost: no time left to wait for");
    check(d.clock.remainingMs(d.now().monoUs) <= 0,
          "and the clock agrees, so main.cpp's wait and the SD-fail check see none");
    check(d.clock.remainingMs(d.now().monoUs + 1'000'000) < 0, "and it stays over");
    // The case that did it: a cold start leaves the wall clock near 1970, so the stale end is decades away.
    Sim cold(1'790'000'000'000LL);
    cold.start(3600);
    cold.sleep(0);
    cold.wallOffsetMs = 3000;  // the clock restarted from nothing
    const BootFacts c = cold.clock.boot(cold.now(), true, false);
    check(c.powerLost && cold.clock.remainingMs(cold.now().monoUs) <= 0,
          "a clock restarted near 1970: no wait for an end that is decades off");
    check(planBoot(c) == BootAction::AlertQuiet, "and the boot says so quietly");
    // The same when only the clock says so: gone back, power not reported lost.
    Sim back(1'790'000'000'000LL);
    back.start(3600);
    back.sleep(0);
    back.wallOffsetMs -= 5 * 3600 * 1000LL;
    const BootFacts b = back.clock.boot(back.now(), false, false);
    check(b.powerLost && back.clock.remainingMs(back.now().monoUs) <= 0,
          "a wall clock gone back by hours is not waited out either");
  }
}

// ---- Starting the alert ---------------------------------------------------------------------------------------------

void testAlertGate() {
  check(alertWanted(true, 0, false, false) && alertWanted(true, -1, false, false) &&
            alertWanted(true, -3'600'000, false, false),
        "a running timer that is over gets its alert");
  check(!alertWanted(true, 1, false, false) && !alertWanted(true, 60'000, false, false), "not before the end");
  check(!alertWanted(false, -1, false, false) && !alertWanted(false, 0, false, false),
        "a timer that is not running has no alert to start");
  check(!alertWanted(true, 0, true, false) && !alertWanted(true, -5000, true, false),
        "and not a second one while one is on its way up");
  check(!alertWanted(true, 0, false, true) && !alertWanted(true, -60'000, false, true),
        "nor over a screen that must not be replaced (USB Drive, the crash report, the firmware picker)");
  check(alertWanted(true, 0, false, false), "and the moment that screen is left, the alert is wanted");
  AlertGate gate;
  check(!gate.pending(), "no alert pending to begin with");
  gate.request();
  check(gate.pending(), "requested");
  gate.release();
  check(!gate.pending() && alertWanted(true, 0, gate.pending(), false),
        "an alert that was dropped before it came up lets the next request through");
}

// A tap in the same pass of the loop that stops the timer the alert was made for, or restarts it, must not make the
// alert mark the new one.
void testAlertIsForThatTimer() {
  const Record made = running();
  check(sameTimer(made, made), "the timer the alert was made for is that timer");
  Record moved = made;
  moved.endWallMs += 400;
  check(sameTimer(made, moved), "its end rewritten by a fraction of a second (a sleep, a persist) is still it");
  moved.endWallMs = made.endWallMs - 500;
  Record later = made;
  later.endWallMs = made.endWallMs + 500;
  check(sameTimer(made, moved) && sameTimer(made, later), "up to half a second either way");
  moved.endWallMs = made.endWallMs + 501;
  Record earlier = made;
  earlier.endWallMs = made.endWallMs - 501;
  check(!sameTimer(made, moved) && !sameTimer(made, earlier), "a half second and a millisecond is a different end");

  // What a stop and start in the same pass does: the same book, page and length, ending a whole length later.
  Record restarted = made;
  restarted.endWallMs = made.endWallMs + 300'000 + 40;
  check(!sameTimer(made, restarted), "stopped and started again on the same page: a new timer");
  Record shortest = made;
  shortest.seconds = 2;
  Record shortestAgain = shortest;
  shortestAgain.endWallMs = shortest.endWallMs + 2000 - 999;  // the wall clock reads whole seconds: up to 999 ms less
  check(!sameTimer(shortest, shortestAgain), "even a two second timer restarted at once ends over a second later");

  Record stopped = made;
  stopped.phase = Phase::None;
  check(!sameTimer(made, stopped), "stopped: nothing to mark");
  Record done = made;
  done.phase = Phase::Alerted;
  check(!sameTimer(made, done), "already alerted: nothing to mark");

  Record other = made;
  other.bookPath = "/Lamina/Method/Spin 22222222.xtc";
  check(!sameTimer(made, other), "another book");
  other = made;
  other.page = 5;
  check(!sameTimer(made, other), "another page");
  other = made;
  other.seconds = 600;
  check(!sameTimer(made, other), "another length");
  other = made;
  other.label = "Other";
  other.panelPage = 9;
  check(sameTimer(made, other), "the label and the page on the panel are not what makes a timer that timer");
}

// The alert marks the timer alerted as it comes up. A screen that restarts the reader as it is left can do so in
// between; the order matters, and this walks both.
void testRestartBeforeTheAlertIsUp() {
  Sim d(1'790'000'000'000LL);
  d.start(600);
  d.awake(600'000 + 500);  // the timer ends while the reader is awake on a screen that restarts as it is left
  check(timerDue(d.remaining()), "the timer is over");
  // The restart persists the timer as it is (running, over) and boots again, not by timer and with the power on.
  d.clock.beforeSleep(d.now(), 4);
  d.trueMs += 1500;
  check(d.boot(false) == BootAction::AlertQuiet,
        "the boot after the restart alerts for it, quietly: the alert is not lost");

  // What would have happened had the timer been marked alerted before the alert was up.
  Sim e(1'790'000'000'000LL);
  e.start(600);
  e.awake(600'500);
  e.clock.markAlerted();
  e.clock.beforeSleep(e.now(), 4);
  e.trueMs += 1500;
  check(e.boot(false) == BootAction::Acknowledge,
        "marked alerted first, the same restart clears it unseen: why the order is as it is");
}

void testPollBeforeTheEnd() {
  check(POLL_BUDGET_MS == 150'000, "the poll's worst case: 10 s of Wi-Fi, 50 s of note, 90 s of books");
  check(pollFitsBeforeEnd(false, 0) && pollFitsBeforeEnd(false, -5000), "no timer, the poll always runs");
  check(pollFitsBeforeEnd(true, 150'001) && pollFitsBeforeEnd(true, 600'000),
        "a timer further off than the poll can last: it runs");
  check(!pollFitsBeforeEnd(true, 150'000) && !pollFitsBeforeEnd(true, 100'000) && !pollFitsBeforeEnd(true, 6'000),
        "a timer inside it: skipped");
  // S4: a network profile lengthens the join the poll can start with (a profile and then the saved network, an
  // EAP-TLS profile with a longer budget of its own), and with it the budget; with no profile it is the one above.
  check(pollBudgetMs(10'000) == POLL_BUDGET_MS, "no profile: the join is 10 s and the budget is the one above");
  check(pollBudgetMs(20'000) == 160'000, "a profile and the saved network: 20 s of Wi-Fi");
  check(pollBudgetMs(35'000) == 175'000, "an EAP-TLS profile and the saved network: 35 s of Wi-Fi");
  check(pollFitsBeforeEnd(true, 160'001) && !pollFitsBeforeEnd(true, 160'000, pollBudgetMs(20'000)) &&
            pollFitsBeforeEnd(true, 160'001, pollBudgetMs(20'000)),
        "the budget a caller passes decides");
  check(
      pollFitsBeforeEnd(true, 170'000, pollBudgetMs(20'000)) && !pollFitsBeforeEnd(true, 170'000, pollBudgetMs(35'000)),
      "a timer 170 s off: a poll with a profile fits, one with an EAP-TLS profile waits");
  check(pollFitsBeforeEnd(false, 0, pollBudgetMs(35'000)), "no timer, the poll always runs");

  // The wake the skipped poll leaves is the timer's: the alert lands at the end, not after the poll.
  Sim d(1'790'000'000'000LL);
  d.start(300);
  d.sleep(300ull * 1000000);  // a 5 minute poll and a 5 minute timer: both fall due together
  d.armedUs = 200ull * 1000000;
  d.trueMs += 200'000;  // a poll wake with 100 s left
  d.bootTrueMs = d.trueMs;
  const BootFacts facts = d.clock.boot(d.now(), false, true);
  check(planBoot(facts) == BootAction::None && !pollFitsBeforeEnd(true, facts.remainingMs),
        "a poll wake 100 s from the end is not an alert yet, and its poll is skipped");
  d.sleep(0);  // rearm(0): no poll
  check(d.armedUs == 98ull * 1000000, "and the reader sleeps for the end less the lead");
  check(d.timerWake() == BootAction::AlertPulse && d.trueMs <= 300'000 && d.trueMs >= 300'000 - 3000,
        "the next wake is the alert, on time");
}

void testNoCard() {
  using P = NoCardPlan;
  check(noCardPlan(BootAction::AlertPulse, true, true, 0) == P::ShowAlert &&
            noCardPlan(BootAction::AlertPulse, true, true, 3000) == P::ShowAlert,
        "a timer wake at the end with no card: the alert is shown without it");
  check(noCardPlan(BootAction::AlertQuiet, false, true, -60000) == P::ShowAlert &&
            noCardPlan(BootAction::AlertQuiet, true, false, 0) == P::ShowAlert,
        "also one found over by hand, or after lost power");
  check(noCardPlan(BootAction::None, true, true, WAKE_SLACK_MS + 1) == P::SleepForTimer,
        "an ambient wake with the timer still ahead sleeps for its end");
  check(noCardPlan(BootAction::None, true, true, 600000) == P::SleepForTimer, "however far off");
  check(noCardPlan(BootAction::None, true, true, WAKE_SLACK_MS) == P::SleepQuietly,
        "a timer within the slack of its end is not slept for: it would be woken for at once");
  check(noCardPlan(BootAction::None, true, false, 0) == P::SleepQuietly &&
            noCardPlan(BootAction::Acknowledge, true, false, 0) == P::SleepQuietly,
        "an ambient wake with no timer sleeps as it always did");
  check(noCardPlan(BootAction::None, true, false, 600000) == P::SleepQuietly,
        "time left on a record that is not running is not a timer to sleep for");
  check(noCardPlan(BootAction::None, false, true, 600000) == P::ShowError &&
            noCardPlan(BootAction::None, false, false, 0) == P::ShowError &&
            noCardPlan(BootAction::Acknowledge, false, false, 0) == P::ShowError,
        "a person who wakes the reader sees the card error");
}

// ---- What the sleeping panel says
// -------------------------------------------------------------------------------------

void testSleepingLine() {
  SleepingLine trusted = sleepingLine("2:32 PM", 252'000);
  check(trusted.kind == SleepingKind::EndsAt && trusted.value == "2:32 PM",
        "a trusted clock: the end as a time of day, am/pm kept");
  trusted = sleepingLine("14:32", 252'000);
  check(trusted.kind == SleepingKind::EndsAt && trusted.value == "14:32", "in 24-hour mode as written");
  SleepingLine untrusted = sleepingLine("", 250'500);
  check(untrusted.kind == SleepingKind::LeftAtSleep && untrusted.value == "4:11",
        "no clock to trust: what was left when the reader slept, rounded up");
  untrusted = sleepingLine("", 3'660'000);
  check(untrusted.kind == SleepingKind::LeftAtSleep && untrusted.value == "1:01:00",
        "an hour and more keeps its hours");
  untrusted = sleepingLine("", -4000);
  check(untrusted.kind == SleepingKind::LeftAtSleep && untrusted.value == "0:00", "never negative");
}

// A panel that sleeps with a countdown on it keeps that countdown, stopped, for hours. Over every state the frame can
// be in, the countdown is replaced whenever there is one.
void testSleepOverlay() {
  using O = SleepOverlay;
  for (int bits = 0; bits < 32; ++bits) {
    const bool running = bits & 1, child = bits & 2, framePage = bits & 4, ownPage = bits & 8, fits = bits & 16;
    const O plan = sleepOverlay(running, child, framePage, ownPage, fits);
    if (!running || child || !framePage) {
      check(plan == O::None, "no timer, another screen's frame, or no page on it: nothing of the timer to replace");
    } else if (ownPage && fits) {
      check(plan == O::Box, "the frame shows the timer's own page and its box fits: the sleeping line goes in the box");
    } else {
      check(plan == O::Chip, "any other page, or a box too small: in the chip");
    }
    // The point of it: a running timer on a frame that holds a page never leaves its countdown as it was.
    check(!(running && !child && framePage) || plan != O::None, "a countdown on a page frame is always replaced");
  }
}

void testRoundUpToMinute() {
  constexpr int64_t MIN = 60'000;
  const int64_t base = 1'800'000'000'000LL / MIN * MIN;  // a whole minute of the wall clock
  checkEq(roundUpToMinute(base), base, "exactly on the minute stays");
  checkEq(roundUpToMinute(base + 1), base + MIN, "a millisecond past it reads as the next minute");
  checkEq(roundUpToMinute(base + 1000), base + MIN, "a second past it too");
  checkEq(roundUpToMinute(base + 45'000), base + MIN, "an end at :45 is written as the minute after");
  checkEq(roundUpToMinute(base + MIN - 1), base + MIN, "and one millisecond short of the next minute");
  checkEq(roundUpToMinute(base + MIN), base + MIN, "the next minute, exactly, stays");
  checkEq(roundUpToMinute(0), int64_t{0}, "zero");
  checkEq(roundUpToMinute(-1), int64_t{0}, "before the epoch rounds up too");
  checkEq(roundUpToMinute(-MIN - 1), -MIN, "and in whole minutes");
  // The promise: what is written is never earlier than the end, and never a minute or more later.
  for (int64_t offset = 0; offset <= 3 * MIN; offset += 997) {
    const int64_t end = base + offset;
    const int64_t written = roundUpToMinute(end);
    check(written >= end && written - end < MIN && written % MIN == 0,
          "never earlier than the end, under a minute later");
  }
}

void testChipSplit() {
  const ChipLayout chip = steptimer::layoutChip(steptimer::chipRect());
  const Rect whole{chip.digits.x, chip.digits.y, chip.label.right() - chip.digits.x, chip.digits.h};
  for (int textWidth = 0; textWidth <= 400; textWidth += 7) {
    const ChipSplit split = splitChipRow(whole, textWidth);
    check(inside(split.time, whole) && inside(split.label, whole), "both parts are inside the row");
    check(!overlap(split.time, split.label), "and apart");
    check(split.time.w <= whole.w * 85 / 100, "the time is never more than 85 % of the row");
    check(split.label.empty() || split.label.w >= 40, "a label is shown only if it has 40 px or more");
    check(split.time.w == std::min(textWidth, whole.w * 85 / 100), "the time gets its width, up to the cap");
  }
  const ChipSplit roomy = splitChipRow(whole, 80);
  check(!roomy.label.empty() && roomy.label.x >= roomy.time.right() + 8,
        "a short time leaves the label room, a gap apart");
  check(splitChipRow(whole, 185).label.empty(),
        "a long line ('About 4:10 left at sleep' can be 180 px) takes the chip's row and the label gives way");
  check(splitChipRow(whole, 400).time.w == whole.w * 85 / 100,
        "text wider than the row is capped, to be cut with an ellipsis");
  // The label is shown from 40 px up: the time that leaves exactly 40 px (after the 8 px gap) keeps it, one pixel more
  // does not.
  const ChipSplit forty = splitChipRow(whole, whole.w - 48);
  check(!forty.label.empty() && forty.label.w == 40, "a label with exactly 40 px is shown");
  check(splitChipRow(whole, whole.w - 47).label.empty(), "one with 39 px is left out");
  check(splitChipRow(whole, -5).time.w == 0, "a width that makes no sense takes no room");
}

void testPickFont() {
  const int widths[3] = {200, 140, 100};
  const int heights[3] = {40, 28, 20};
  check(pickFont(widths, heights, 3, 380, 84) == 0, "the largest that fits");
  check(pickFont(widths, heights, 3, 150, 84) == 1, "the next down when the first is too wide");
  check(pickFont(widths, heights, 3, 380, 30) == 1, "or too tall");
  check(pickFont(widths, heights, 3, 100, 20) == 2, "the smallest when it is the one that fits");
  check(pickFont(widths, heights, 3, 50, 10) == 2, "and still the smallest when nothing does (the text is cut)");
  check(pickFont(widths, heights, 0, 380, 84) == 0, "no candidates: the first");
  check(pickFont(widths, heights, 3, 200, 40) == 0, "a text exactly as wide and as tall as the area fits");
  check(pickFont(widths, heights, 3, 199, 40) == 1 && pickFont(widths, heights, 3, 200, 39) == 1,
        "and one pixel less in either does not");
  // A 96 px box has 50-60 px for its text; a chip row is 28 px tall.
  const BoxLayout mid = steptimer::layoutBox({32, 610, 416, 96});
  const int mw[3] = {230, 170, 120};
  const int mh[3] = {42, 30, 22};
  check(pickFont(mw, mh, 3, mid.digits.w, mid.digits.h) == 0, "'Ends 2:32 PM' in the largest size fits a 96 px box");
  const ChipLayout chip = steptimer::layoutChip(steptimer::chipRect());
  check(pickFont(mw, mh, 3, chip.digits.w + 10 + chip.label.w, chip.digits.h) == 2,
        "while a chip's 28 px row takes only the smallest");
}

}  // namespace

int main() {
  testClockText();
  testDisplaySeconds();
  testCadence();
  testNextWake();
  testPlanBoot();
  testAlertLight();
  testDecideStart();
  testRecordValid();
  testTouchTargets();
  testLayouts();
  testClockGlyphs();
  testScenarioShortTimerAsleepAllTheWay();
  testScenarioPollsInBetween();
  testScenarioAwakeThroughASync();
  testScenarioWallClockNeverSet();
  testScenarioOtherWakes();
  testRecordRoundTripKeepsTheTimer();
  testStore();
  testClockworkGuards();
  testAlertGate();
  testAlertIsForThatTimer();
  testNoCard();
  testSleepingLine();
  testRoundUpToMinute();
  testSleepOverlay();
  testChipSplit();
  testPickFont();
  testPollBeforeTheEnd();
  testRestartBeforeTheAlertIsUp();
  std::cout << "step timer: " << checks << " checks passed\n";
  return 0;
}
