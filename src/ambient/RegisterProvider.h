#pragma once

#include <cstdint>
#include <mutex>

#include "AmbientProvider.h"
#include "RegisterData.h"
#include "RegisterLayout.h"
#include "RegisterModel.h"

class GfxRenderer;

// The device side of the Register screen (docs/SPEC-REGISTER.md section 2): the schedule on the card, the poll, and
// drawing. The decisions (what the screen says, when to redraw, when to wake) are in RegisterModel.h and
// RegisterWake.h, covered by the host tests; this file reads the card and the clock, asks the mailbox, and draws the
// layout RegisterLayout.h gives with the reader's fonts.
//
// What survives a deep sleep lives in RTC memory, not on the card: when the next poll is planned for, and what the
// last Register draw left on the panel (a signature, and the footer's time). A reset or a lost battery clears them,
// which means "poll now" and "draw again", the safe answers.
namespace ambient {

class RegisterProvider {
 public:
  static RegisterProvider& instance();

  // ---- the schedule ----------------------------------------------------------------------------------------------
  // /Lamina/register.json, read and validated on first use and held. False when there is none or the file is not one
  // this reader would have accepted from the mailbox (it is ignored, never trusted, and left where it is).
  bool loadSchedule();
  // Asks the mailbox for the schedule (GET /api/device/<id>/register, verified TLS, bounded) and keeps it on the card,
  // written whole to a temporary file and renamed. Updated: a valid reply was saved. Failed: nothing changed. The
  // caller owns Wi-Fi.
  FetchResult fetch();
  // A poll has finished (it worked or not): the next one is planned an interval from now.
  void notePolled();

  // ---- the wake --------------------------------------------------------------------------------------------------
  // Is this timer wake a poll's? False for a boundary wake, which does no network, and when polling is off or the
  // battery is too low for the radio.
  bool pollWake(uint16_t batteryPercent);
  // The wake timer to arm on the way into sleep: the soonest of the poll, the next boundary and a running step
  // timer's end, none under 15 % battery. `methodStepOnPanel`: a Method step is on the panel, so there is nothing to
  // redraw at a boundary. Keeps the plan for the poll in RTC memory. It reads the schedule from memory only, because it
  // is asked for after the card has been shut down: a caller that has not used the schedule this boot calls
  // loadSchedule() first, while the card is still up.
  uint64_t wakeMicros(uint16_t batteryPercent, bool methodStepOnPanel);

  // ---- the panel -------------------------------------------------------------------------------------------------
  // What the screen would say now, ready to draw (no fonts needed). False when there is no schedule to show.
  bool prepare(uint16_t batteryPercent);
  // The prepared model against what the panel holds. Draws are for NotOurs, Content and Footer (reg::shouldRedraw).
  reg::Why planRedraw(uint16_t batteryPercent, bool methodStepOnPanel);
  // Draws what prepare() or planRedraw() made, full refresh, and records what the panel now holds. Takes the render
  // lock. Needs the fonts: the caller has set up the display. False when nothing was prepared or the draw failed.
  bool drawPrepared(GfxRenderer& renderer);
  // The panel no longer holds a Register screen (another sleep screen, a Method step).
  void clearPanel();

  // ---- for the awake view ----------------------------------------------------------------------------------------
  // The schedule as it stands, copied (read from the card first if it has not been). False when there is none.
  bool snapshot(reg::Schedule& out);
  // The same, from memory only: never touches the card. `decidedView` (when given) is the view time the screen was last
  // prepared for in this boot, 0 when it was not.
  bool peek(reg::Schedule& out, int64_t* decidedView = nullptr);

  // ---- a wake the card does not answer -----------------------------------------------------------------------------
  // Kept in RTC memory: Register armed a wake when the reader last went to sleep. A timer wake that finds the card will
  // not mount, or the mailbox settings will not load, while this is set tries again soon instead of ending the chain
  // (see reg::planCardRetry).
  static bool armed();
  // The wake to arm for that retry (0 once the retries are spent, or under 15 % battery), counted in RTC memory.
  static uint64_t cardRetryMicros(uint16_t batteryPercent);
  // Register is not the sleep screen (any more): nothing is armed.
  static void disarm();

 private:
  RegisterProvider() = default;
  bool loadLocked();
  bool saveLocked(const reg::Schedule& schedule);

  std::mutex mutex_;
  bool loaded_ = false;
  bool valid_ = false;
  reg::Schedule schedule_;
  bool prepared_ = false;
  reg::Model model_;
  reg::Panel panel_;
  int64_t preparedView_ = 0;
};

// The screen's words, in the reader's language.
reg::Words registerWords();
// The reader's clock format and poll interval.
reg::Options registerOptions(bool batteryLow);
// The reader's clock, or 0 when it cannot be trusted.
int64_t registerNow();

// Draws a layout into the framebuffer in portrait, as the Register screen: white page, black text and rules. The caller
// refreshes the panel. Renderer-owned state (fonts, display) is touched: call on the render path or under RenderLock.
void drawRegisterLayout(GfxRenderer& renderer, const reg::Layout& layout);
// A text meter over the renderer's fonts, for building a layout.
class RendererMeter final : public reg::TextMeter {
 public:
  explicit RendererMeter(const GfxRenderer& renderer) : renderer(renderer) {}
  int width(reg::FontRole role, const std::string& text) const override;
  int lineHeight(reg::FontRole role) const override;

 private:
  const GfxRenderer& renderer;
};

}  // namespace ambient
