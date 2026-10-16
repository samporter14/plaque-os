#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <string>

#include "ambient/StepTimer.h"

// Drawing the step timer: the countdown over a page's timer box, the small chip other pages carry, and the inverted
// banner of the alert. Everything goes onto the renderer's framebuffer; the caller refreshes. Geometry comes from
// steptimer::layoutBox / layoutChip / clockGlyphs (host-tested); the figures are drawn as blocks, large and the same
// width each, because the built-in fonts stop well short of the page's 56 px digits.
namespace steptimer::view {

// Running: outlined, counting down. Sleeping: outlined, what the panel keeps while the reader sleeps (the end as a time
// of day, because a frozen countdown would only mislead). Due: inverted, the end.
enum class Mood : uint8_t { Running, Sleeping, Due };

struct Content {
  std::string digits;  // "4:12": drawn as large block figures; or, when asText, words with a time ("Ends 2:32 PM"),
                       // drawn in a font
  std::string label;
  std::string hint;
  bool asText = false;
};

// A finished timer left in its box after the alert has been answered: "Done · 2:32 PM" and the label, until the page
// changes.
Content contentForDone(const std::string& label, const std::string& endedAt);

// The text for `record` as of `remainingMs`. `lostPower` marks a timer found over after the reader lost power.
Content contentFor(const Record& record, Mood mood, int64_t remainingMs, bool lostPower = false);

// The end (a wall-clock time in ms) as a time of day in the reader's zone, "14:32" or "2:32 PM"; empty when the clock
// cannot be trusted.
std::string timeOfDay(int64_t wallMs);
// The same for a time something is going to END ("ends 2:32 PM"): rounded up to the next whole minute first, so that
// the time written is never earlier than the end (steptimer::roundUpToMinute). A time something DID end stays
// timeOfDay.
std::string endTimeOfDay(int64_t endWallMs);

// The box (or banner) filled and framed, with its digits, label and hint.
void drawBox(const GfxRenderer& renderer, const Rect& box, const Content& content, bool inverted);
// The chip: digits and label.
void drawChip(const GfxRenderer& renderer, const Content& content, bool inverted);

// True when a page's timer box is big enough for its own countdown; if not, the chip is shown on that page too.
bool boxCanShow(const Record& record);

}  // namespace steptimer::view
