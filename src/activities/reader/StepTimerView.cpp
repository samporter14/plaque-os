#include "StepTimerView.h"

#include <I18n.h>
#include <PlaqueBoard.h>
#include <TrustedTime.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <vector>

#include "CrossPointSettings.h"
#include "ambient/MethodFormat.h"
#include "fontIds.h"

namespace steptimer::view {

namespace {
std::string format(const char* pattern, const std::string& a) {
  char out[160];
  snprintf(out, sizeof(out), pattern, a.c_str());
  return out;
}

void drawTextIn(const GfxRenderer& renderer, int fontId, const Rect& row, const std::string& text, bool black,
                EpdFontFamily::Style style) {
  if (text.empty() || row.empty()) return;
  const std::string fitted = renderer.truncatedText(fontId, text.c_str(), row.w, style);
  const int y = row.y + (row.h - renderer.getLineHeight(fontId)) / 2;
  renderer.drawText(fontId, row.x, y, fitted.c_str(), black, style);
}

void drawDigits(const GfxRenderer& renderer, const std::string& digits, const Rect& area, bool inverted) {
  std::vector<Rect> rects;
  clockGlyphs(digits, area, rects);
  for (const Rect& r : rects) renderer.fillRect(r.x, r.y, r.w, r.h, !inverted);
}

// Words with a time in them, in the largest of the built-in sizes that fits the area, centred; cut with an ellipsis
// only if even the smallest is too wide.
void drawFittedText(const GfxRenderer& renderer, const Rect& area, const std::string& text, bool black) {
  if (text.empty() || area.empty()) return;
  constexpr int FONTS[] = {NOTOSANS_18_FONT_ID, UI_12_FONT_ID, UI_10_FONT_ID};
  constexpr int COUNT = 3;
  int widths[COUNT], heights[COUNT];
  for (int i = 0; i < COUNT; ++i) {
    widths[i] = renderer.getTextWidth(FONTS[i], text.c_str(), EpdFontFamily::BOLD);
    heights[i] = renderer.getLineHeight(FONTS[i]);
  }
  const int pick = pickFont(widths, heights, COUNT, area.w, area.h);
  const std::string fitted =
      widths[pick] <= area.w ? text : renderer.truncatedText(FONTS[pick], text.c_str(), area.w, EpdFontFamily::BOLD);
  const int width = std::min(widths[pick], area.w);
  renderer.drawText(FONTS[pick], area.x + (area.w - width) / 2, area.y + (area.h - heights[pick]) / 2, fitted.c_str(),
                    black, EpdFontFamily::BOLD);
}
}  // namespace

std::string timeOfDay(int64_t wallMs) {
  if (trustedtime::trustedNow() == 0) return {};
  const time_t seconds = static_cast<time_t>((wallMs + 500) / 1000);
  struct tm parts = {};
  if (!localtime_r(&seconds, &parts)) return {};
  method::LocalTime local;
  local.year = parts.tm_year + 1900;
  local.month = parts.tm_mon + 1;
  local.day = parts.tm_mday;
  local.hour = parts.tm_hour;
  local.minute = parts.tm_min;
  local.weekday = parts.tm_wday;
  local.valid = true;
  return method::clockText(local, SETTINGS.clockFormat == 1);
}

std::string endTimeOfDay(int64_t endWallMs) { return timeOfDay(roundUpToMinute(endWallMs)); }

Content contentFor(const Record& record, Mood mood, int64_t remainingMs, bool lostPower) {
  Content c;
  c.label = record.label.empty() ? std::string(tr(STR_METHOD_TIMER_LABEL)) : record.label;
  switch (mood) {
    case Mood::Running: {
      c.digits = clockText(displaySeconds(remainingMs));
      // The end as a time of day, from the wall clock now plus what is left. A tap on the box stops it where there is a
      // touchscreen; on the keys, Confirm opens the menu that has Stop in it.
      const std::string ends = endTimeOfDay(static_cast<int64_t>(time(nullptr)) * 1000 + remainingMs);
      const bool touch = plaque::promptsTouch(plaque::board().caps);
      if (ends.empty()) {
        c.hint = touch ? tr(STR_METHOD_TIMER_HINT_STOP) : tr(STR_X4C_TIMER_HINT_STOP);
      } else {
        c.hint = format(touch ? tr(STR_METHOD_TIMER_HINT_STOP_ENDS) : tr(STR_X4C_TIMER_HINT_STOP_ENDS), ends);
      }
      break;
    }
    case Mood::Sleeping: {
      // Always words with the time: "Ends 2:32 PM", or, when the clock cannot say, "About 4:10 left at sleep". Bare
      // figures on a sleeping panel read as a countdown that has stopped.
      const SleepingLine line =
          sleepingLine(endTimeOfDay(static_cast<int64_t>(time(nullptr)) * 1000 + remainingMs), remainingMs);
      c.digits =
          format(line.kind == SleepingKind::EndsAt ? tr(STR_METHOD_TIMER_ENDS_TIME) : tr(STR_METHOD_TIMER_LEFT_ASLEEP),
                 line.value);
      c.asText = true;
      break;
    }
    case Mood::Due:
      c.digits = clockText(0);
      if (lostPower) {
        c.hint = tr(STR_METHOD_TIMER_LOST);
      } else if (record.page >= 2) {
        char out[80];
        snprintf(out, sizeof(out), tr(STR_METHOD_TIMER_DONE_STEP), static_cast<int>(record.page) - 1);
        c.hint = out;
      } else {
        c.hint = tr(STR_METHOD_TIMER_DONE);
      }
      break;
  }
  return c;
}

Content contentForDone(const std::string& label, const std::string& endedAt) {
  Content c;
  c.label = label.empty() ? std::string(tr(STR_METHOD_TIMER_LABEL)) : label;
  c.digits =
      endedAt.empty() ? std::string(tr(STR_METHOD_TIMER_FINISHED)) : format(tr(STR_METHOD_TIMER_FINISHED_AT), endedAt);
  c.asText = true;
  return c;
}

bool boxCanShow(const Record& record) {
  const Rect box{record.boxX, record.boxY, record.boxW, record.boxH};
  return clockFits(clockText(record.seconds), layoutBox(box).digits);
}

void drawBox(const GfxRenderer& renderer, const Rect& box, const Content& content, bool inverted) {
  const BoxLayout layout = layoutBox(box);
  // The whole box is overwritten, so a countdown drawn over the last one leaves nothing of it behind.
  renderer.fillRect(box.x, box.y, box.w, box.h, inverted);
  if (!inverted) renderer.drawRect(box.x, box.y, box.w, box.h, layout.frameWidth, true);
  if (content.asText) {
    drawFittedText(renderer, layout.digits, content.digits, !inverted);
  } else {
    drawDigits(renderer, content.digits, layout.digits, inverted);
  }
  drawTextIn(renderer, UI_10_FONT_ID, layout.label, content.label, !inverted, EpdFontFamily::BOLD);
  drawTextIn(renderer, UI_10_FONT_ID, layout.hint, content.hint, !inverted, EpdFontFamily::REGULAR);
}

void drawChip(const GfxRenderer& renderer, const Content& content, bool inverted) {
  const Rect chip = chipRect();
  const ChipLayout layout = layoutChip(chip);
  renderer.fillRect(chip.x, chip.y, chip.w, chip.h, inverted);
  if (!inverted) renderer.drawRect(chip.x, chip.y, chip.w, chip.h, layout.frameWidth, true);
  if (content.asText) {
    // Words with a time ("Ends 2:32 PM") take what they need of the row, up to 85 %, and the label gets the rest if it
    // is worth showing.
    const Rect row{layout.digits.x, layout.digits.y, layout.label.right() - layout.digits.x, layout.digits.h};
    const ChipSplit split =
        splitChipRow(row, renderer.getTextWidth(UI_10_FONT_ID, content.digits.c_str(), EpdFontFamily::BOLD));
    drawTextIn(renderer, UI_10_FONT_ID, split.time, content.digits, !inverted, EpdFontFamily::BOLD);
    drawTextIn(renderer, UI_10_FONT_ID, split.label, content.label, !inverted, EpdFontFamily::REGULAR);
  } else {
    drawDigits(renderer, content.digits, layout.digits, inverted);
    drawTextIn(renderer, UI_10_FONT_ID, layout.label, content.label, !inverted, EpdFontFamily::BOLD);
  }
}

}  // namespace steptimer::view
