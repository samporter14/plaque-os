#include "StepTimer.h"

#include <algorithm>
#include <cstdio>

#include "MethodProtocol.h"

namespace steptimer {

// ---- The record --------------------------------------------------------------------------------------------------

bool recordValid(const Record& r) {
  if (r.phase == Phase::None) return true;
  if (r.phase != Phase::Running && r.phase != Phase::Alerted) return false;
  if (!method::ownedBookPath(r.bookPath)) return false;
  if (r.page < 1 || r.seconds < 1 || r.seconds > method::MAX_TIMER_SECONDS) return false;
  if (r.label.size() > method::MAX_TIMER_LABEL_BYTES) return false;
  const int minBox = method::MIN_TIMER_BOX_PX;
  if (r.boxW < minBox || r.boxH < minBox || r.boxX + r.boxW > PAGE_W || r.boxY + r.boxH > PAGE_H) return false;
  if (r.phase == Phase::Running && r.endWallMs <= 0) return false;
  return true;
}

// ---- Showing the time --------------------------------------------------------------------------------------------

int64_t displaySeconds(int64_t remainingMs) { return remainingMs <= 0 ? 0 : (remainingMs + 999) / 1000; }

std::string clockText(int64_t seconds) {
  const int64_t s = std::max<int64_t>(0, seconds);
  const long hours = static_cast<long>(s / 3600), minutes = static_cast<long>((s % 3600) / 60),
             secs = static_cast<long>(s % 60);
  char out[24];
  if (hours > 0) {
    snprintf(out, sizeof(out), "%ld:%02ld:%02ld", hours, minutes, secs);
  } else {
    snprintf(out, sizeof(out), "%ld:%02ld", minutes, secs);
  }
  return out;
}

int64_t nextShownSeconds(int64_t shown) {
  if (shown <= 0) return 0;
  if (shown > 60) return std::max<int64_t>(shown - 60, 60);
  return ((shown - 1) / 10) * 10;
}

bool tickDue(int64_t shownSeconds, int64_t remainingMs) {
  if (shownSeconds <= 0) return false;
  return displaySeconds(remainingMs) <= nextShownSeconds(shownSeconds);
}

// ---- Sleeping ----------------------------------------------------------------------------------------------------

uint64_t nextWakeMicros(uint64_t pollMicros, bool timerRunning, int64_t remainingMs) {
  uint64_t wake = pollMicros;
  if (timerRunning) {
    const int64_t untilMs = remainingMs - WAKE_LEAD_MS;
    const uint64_t timer =
        untilMs <= 0 ? MIN_WAKE_US : std::max<uint64_t>(MIN_WAKE_US, static_cast<uint64_t>(untilMs) * 1000);
    wake = wake == 0 ? timer : std::min(wake, timer);
  }
  return wake;
}

// ---- Booting -----------------------------------------------------------------------------------------------------

BootAction planBoot(const BootFacts& facts) {
  switch (facts.phase) {
    case Phase::None:
      return BootAction::None;
    case Phase::Alerted:
      // The alert has been shown. An ambient poll finding it leaves it for whoever wakes the reader; a person doing
      // so has seen the end of the timer (or will see the book), and the record is cleared.
      return facts.timerWake ? BootAction::None : BootAction::Acknowledge;
    case Phase::Running:
      break;
  }
  if (facts.powerLost) return BootAction::AlertQuiet;
  if (facts.remainingMs <= 0) {
    // Late by the whole alert limit or more: nobody can be told in time, so no light for minutes at an empty room.
    if (!facts.timerWake || -facts.remainingMs >= ALERT_LIMIT_MS) return BootAction::AlertQuiet;
    return BootAction::AlertPulse;
  }
  if (facts.timerWake && facts.remainingMs <= WAKE_SLACK_MS) return BootAction::AlertPulse;
  return BootAction::None;
}

// ---- The alert's light -------------------------------------------------------------------------------------------

namespace {
constexpr int64_t FLASH_HALF_MS = 350;  // dark, then lit, 350 ms each
constexpr int64_t FLASHES = 3;
constexpr int64_t BURST_MS = FLASH_HALF_MS * 2 * FLASHES;  // 2.1 s
constexpr int64_t BURST_PERIOD_MS = 30000;
}  // namespace

LightFrame alertLight(int64_t elapsedMs, bool quiet) {
  if (elapsedMs >= ALERT_LIMIT_MS) return {false, true};
  if (quiet || elapsedMs < 0) return {false, false};
  const int64_t phase = elapsedMs % BURST_PERIOD_MS;
  if (phase >= BURST_MS) return {true, false};  // between bursts the light stays on
  return {(phase % (FLASH_HALF_MS * 2)) >= FLASH_HALF_MS, false};
}

uint8_t alertBrightness(uint8_t userPercent) { return static_cast<uint8_t>(std::clamp<int>(userPercent, 30, 70)); }

// ---- The running timer -------------------------------------------------------------------------------------------

BootFacts Clockwork::boot(const Now& now, bool powerLost, bool timerWake) {
  BootFacts facts;
  facts.phase = rec_.phase;
  facts.timerWake = timerWake;
  if (rec_.phase != Phase::Running) return facts;
  const int64_t remaining = rec_.endWallMs - now.wallMs;
  // More time left than the timer ever had (a minute of slack for rounding): the wall clock is behind where it was.
  const bool clockWentBack = remaining > static_cast<int64_t>(rec_.seconds) * 1000 + 60000;
  facts.powerLost = powerLost || clockWentBack;
  // A wall clock that cannot be trusted says nothing about when the timer ends (after a cold start it is near 1970, so
  // `remaining` is decades): the end is taken as now, and every later remainingMs() is zero or less. Nothing may wait
  // for it.
  endMonoUs_ = facts.powerLost ? now.monoUs : now.monoUs + remaining * 1000;
  facts.remainingMs = facts.powerLost ? -1 : remaining;
  return facts;
}

int64_t Clockwork::remainingMs(int64_t monoUs) const {
  // Floor division, so a microsecond past the end is already negative.
  const int64_t us = endMonoUs_ - monoUs;
  return us >= 0 ? us / 1000 : -((-us + 999) / 1000);
}

void Clockwork::start(const std::string& bookPath, unsigned page, uint32_t seconds, const std::string& label,
                      const Rect& box, const Now& now, unsigned panelPage) {
  Record r;
  r.phase = Phase::Running;
  r.bookPath = bookPath;
  r.page = static_cast<uint16_t>(page);
  r.seconds = seconds;
  r.label = label;
  r.boxX = static_cast<uint16_t>(box.x);
  r.boxY = static_cast<uint16_t>(box.y);
  r.boxW = static_cast<uint16_t>(box.w);
  r.boxH = static_cast<uint16_t>(box.h);
  r.endWallMs = now.wallMs + static_cast<int64_t>(seconds) * 1000;
  r.panelPage = static_cast<uint16_t>(panelPage);
  rec_ = std::move(r);
  endMonoUs_ = now.monoUs + static_cast<int64_t>(seconds) * 1000000;
}

void Clockwork::clear() { rec_ = Record(); }

void Clockwork::markAlerted() {
  if (rec_.phase == Phase::Running) rec_.phase = Phase::Alerted;
}

void Clockwork::beforeSleep(const Now& now, unsigned panelPage) {
  rec_.panelPage = static_cast<uint16_t>(panelPage);
  if (rec_.phase != Phase::Running) return;
  rec_.endWallMs = now.wallMs + remainingMs(now.monoUs);
}

uint64_t Clockwork::wakeMicros(uint64_t pollMicros, int64_t monoUs) const {
  return nextWakeMicros(pollMicros, running(), running() ? remainingMs(monoUs) : 0);
}

// ---- Starting ----------------------------------------------------------------------------------------------------

StartDecision decideStart(const Record& running, std::string_view bookPath, unsigned page) {
  if (running.phase != Phase::Running) return StartDecision::Start;
  return running.bookPath == bookPath && running.page == page ? StartDecision::AlreadyHere : StartDecision::AskReplace;
}

// ---- Geometry ----------------------------------------------------------------------------------------------------

Rect chipRect() { return {120, 6, 240, 44}; }
Rect bannerRect() { return {32, 6, 416, 76}; }

Rect touchTarget(const Rect& visual) {
  Rect r = visual;
  r.w = std::min(PAGE_W, std::max(visual.w, MIN_TARGET_W));
  r.h = std::min(PAGE_H, std::max(visual.h, MIN_TARGET_H));
  r.x = std::clamp(visual.x - (r.w - visual.w) / 2, 0, PAGE_W - r.w);
  r.y = std::clamp(visual.y - (r.h - visual.h) / 2, 0, PAGE_H - r.h);
  return r;
}

namespace {
Rect inset(const Rect& r, int by) {
  Rect out{r.x + by, r.y + by, r.w - 2 * by, r.h - 2 * by};
  if (out.w < 0) out.w = 0;
  if (out.h < 0) out.h = 0;
  return out;
}
}  // namespace

BoxLayout layoutBox(const Rect& box) {
  BoxLayout layout;
  layout.frame = box;
  layout.frameWidth = box.h >= 48 ? 3 : 2;
  const int pad = box.h >= 100 ? 8 : (box.h >= 48 ? 4 : 2);
  const Rect inner = inset(box, layout.frameWidth + pad);
  constexpr int ROW = 26;
  if (inner.h >= 110) {
    layout.label = {inner.x, inner.y, inner.w, ROW};
    layout.hint = {inner.x, inner.bottom() - ROW, inner.w, ROW};
    layout.digits = {inner.x, inner.y + ROW, inner.w, inner.h - 2 * ROW};
  } else if (inner.h >= 64) {
    layout.label = {inner.x, inner.y, inner.w, 24};
    layout.digits = {inner.x, inner.y + 24, inner.w, inner.h - 24};
  } else {
    layout.digits = inner;
  }
  return layout;
}

ChipLayout layoutChip(const Rect& chip) {
  ChipLayout layout;
  layout.frame = chip;
  layout.frameWidth = 2;
  const Rect inner = inset(chip, 8);
  const int digitsW = std::min(100, inner.w / 2);
  layout.digits = {inner.x, inner.y, digitsW, inner.h};
  const int gap = 10;
  layout.label = {inner.x + digitsW + gap, inner.y, std::max(0, inner.w - digitsW - gap), inner.h};
  return layout;
}

bool sameTimer(const Record& madeFor, const Record& stored) {
  if (stored.phase != Phase::Running) return false;
  const int64_t moved = stored.endWallMs - madeFor.endWallMs;
  return stored.bookPath == madeFor.bookPath && stored.page == madeFor.page && stored.seconds == madeFor.seconds &&
         moved >= -SAME_END_TOLERANCE_MS && moved <= SAME_END_TOLERANCE_MS;
}

SleepingLine sleepingLine(const std::string& endsAt, int64_t remainingMs) {
  if (!endsAt.empty()) return {SleepingKind::EndsAt, endsAt};
  return {SleepingKind::LeftAtSleep, clockText(displaySeconds(remainingMs))};
}

int64_t roundUpToMinute(int64_t wallMs) {
  constexpr int64_t MINUTE_MS = 60000;
  int64_t past = wallMs % MINUTE_MS;
  if (past < 0) past += MINUTE_MS;
  return past == 0 ? wallMs : wallMs + (MINUTE_MS - past);
}

ChipSplit splitChipRow(const Rect& row, int timeTextWidth) {
  constexpr int GAP = 8;
  constexpr int MIN_LABEL = 40;  // a label squeezed to fewer pixels than this says nothing, so it is left out
  ChipSplit split;
  const int timeW = std::clamp(timeTextWidth, 0, row.w * 85 / 100);
  split.time = {row.x, row.y, timeW, row.h};
  const int labelX = row.x + timeW + GAP;
  const int labelW = row.right() - labelX;
  if (labelW >= MIN_LABEL) split.label = {labelX, row.y, labelW, row.h};
  return split;
}

int pickFont(const int* widths, const int* heights, int count, int areaW, int areaH) {
  for (int i = 0; i < count; ++i)
    if (widths[i] <= areaW && heights[i] <= areaH) return i;
  return count > 0 ? count - 1 : 0;
}

// ---- Clock figures -----------------------------------------------------------------------------------------------

namespace {
// Which of the seven segments each digit lights: bit 0 a (top), 1 b (top right), 2 c (bottom right), 3 d (bottom),
// 4 e (bottom left), 5 f (top left), 6 g (middle).
constexpr uint8_t SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

struct Metrics {
  int h, t, cellW, gap, colonW;
};
Metrics metricsFor(int h) {
  Metrics m;
  m.h = h;
  m.t = std::max(2, (h + 4) / 9);
  m.cellW = std::max(2 * m.t + 2, h * 58 / 100);
  m.gap = std::max(2, m.t);
  m.colonW = m.t;
  return m;
}

int glyphWidth(char c, const Metrics& m) {
  if (c >= '0' && c <= '9') return m.cellW;
  if (c == ':') return m.colonW;
  return m.cellW / 2;  // any other character is a gap
}

int textWidth(std::string_view text, const Metrics& m) {
  int width = 0;
  for (const char c : text) width += glyphWidth(c, m);
  if (!text.empty()) width += static_cast<int>(text.size() - 1) * m.gap;
  return width;
}

void digitRects(int digit, int x, int y, const Metrics& m, std::vector<Rect>& out) {
  const uint8_t on = SEGMENTS[digit];
  const int w = m.cellW, h = m.h, t = m.t;
  const int half = (h + t) / 2;                // a vertical segment spans half the height plus the bar it joins
  if (on & 0x01) out.push_back({x, y, w, t});  // a
  if (on & 0x02) out.push_back({x + w - t, y, t, half});                // b
  if (on & 0x04) out.push_back({x + w - t, y + (h - t) / 2, t, half});  // c
  if (on & 0x08) out.push_back({x, y + h - t, w, t});                   // d
  if (on & 0x10) out.push_back({x, y + (h - t) / 2, t, half});          // e
  if (on & 0x20) out.push_back({x, y, t, half});                        // f
  if (on & 0x40) out.push_back({x, y + (h - t) / 2, w, t});             // g
}
}  // namespace

int clockGlyphs(std::string_view text, const Rect& area, std::vector<Rect>& out) {
  if (text.empty() || area.empty()) return 0;
  int h = std::min(area.h, 400);
  if (h < MIN_GLYPH_H) return 0;
  Metrics m = metricsFor(h);
  while (h > MIN_GLYPH_H && textWidth(text, m) > area.w) m = metricsFor(--h);
  const int width = textWidth(text, m);
  if (width > area.w) return 0;
  int x = area.x + std::max(0, (area.w - width) / 2);
  const int y = area.y + std::max(0, (area.h - m.h) / 2);
  for (const char c : text) {
    if (c >= '0' && c <= '9') {
      digitRects(c - '0', x, y, m, out);
    } else if (c == ':') {
      out.push_back({x, y + m.h / 3 - m.t / 2, m.t, m.t});
      out.push_back({x, y + 2 * m.h / 3 - m.t / 2, m.t, m.t});
    }
    x += glyphWidth(c, m) + m.gap;
  }
  return m.h;
}

bool clockFits(std::string_view text, const Rect& area) {
  std::vector<Rect> scratch;
  return clockGlyphs(text, area, scratch) > 0;
}

}  // namespace steptimer
