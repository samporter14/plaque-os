#include "StepTimerAlertActivity.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <PlaqueBoard.h>
#include <esp_timer.h>

#include <ctime>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "StepTimerView.h"
#include "ambient/SleepFrame.h"
#include "ambient/StepTimerService.h"
#include "fontIds.h"

// Defined in main.cpp: asks the main loop to put the reader to sleep, as an idle timeout would.
void requestDeepSleepSoon();

namespace {
// A release this soon after the alert appears is the end of a touch that began before it (a page turn, the lift of a
// finger): not an answer to it.
constexpr uint32_t DISMISS_GRACE_MS = 700;
}  // namespace

steptimer::AlertGate& StepTimerAlertActivity::gate() {
  static steptimer::AlertGate gate;
  return gate;
}

void StepTimerAlertActivity::onEnter() {
  Activity::onEnter();
  rec_ = params_.record;
  bool stillThatTimer = false;
  {
    // Now, before anything is drawn: from here the timer is no longer running, so nothing starts a second alert for it.
    // Only that timer, though: the screen under this one has had a turn since the alert was asked for, and a tap in it
    // may have stopped the timer, or started another in its place. Marking whatever is stored would end the new one.
    RenderLock lock(*this);
    stillThatTimer = steptimer::sameTimer(rec_, STEP_TIMER.record());
    if (stillThatTimer) STEP_TIMER.markAlerted();
  }
  gate().release();
  if (!stillThatTimer) {
    // Whoever stopped or restarted it has seen to it: no alert, and nothing of the stored timer is touched.
    LOG_INF("TIMER", "The timer changed before its alert came up: no alert (%s)", rec_.label.c_str());
    dismissed_ = true;
    leave();
    return;
  }
  if (params_.waitMs > 0) {
    waitUntilUs_ = esp_timer_get_time() + params_.waitMs * 1000;
    LOG_INF("TIMER", "Alert in %lld ms (%s)", static_cast<long long>(params_.waitMs), rec_.label.c_str());
  } else {
    begin();
  }
}

void StepTimerAlertActivity::begin() {
  started_ = true;
  startedMs_ = millis();
  // When it ended, for the note left behind: the wall clock now plus what the counter says is left (zero, or less if
  // the alert is late). Not when power was lost: nothing then says when it ended.
  if (!params_.lostPower)
    endedAt_ = steptimer::view::timeOfDay(static_cast<int64_t>(time(nullptr)) * 1000 + STEP_TIMER.remainingMs());
  LOG_INF("TIMER", "Alert: \"%s\" page %u of %s (quiet=%d, retained=%d, panel page %u)", rec_.label.c_str(), rec_.page,
          rec_.bookPath.c_str(), params_.quiet, params_.retainedFrame, params_.panelPage);
  // The light pulses only on a board with one Plaque alerts with (the X4 Pro). Elsewhere nothing here touches a light:
  // the inverted box is the alert, and the press that answers it is the same.
  alertLight_ = plaque::alertHasLight(plaque::board().caps, Frontlight.present());
  if (alertLight_) {
    baselineOn_ = Frontlight.isOn();
    baselineBrightness_ = Frontlight.brightness();
    lightTouched_ = true;
    Frontlight.setBrightness(steptimer::alertBrightness(static_cast<uint8_t>(SETTINGS.frontlightBrightness)));
  }
  requestUpdate();
}

void StepTimerAlertActivity::stepLight(const uint32_t elapsedMs) {
  // The battery is read only where there is a light to pulse.
  const bool lightAllowed =
      alertLight_ && !params_.quiet && steptimer::lightAllowed(powerManager.getBatteryPercentage());
  const steptimer::LightFrame frame = steptimer::alertLight(static_cast<int64_t>(elapsedMs), !lightAllowed);
  if (frame.on != lightOn_) {
    lightOn_ = frame.on;
    Frontlight.setOn(frame.on);
  }
  if (frame.ended) {
    ended_ = true;
    restoreLight();
    LOG_INF("TIMER", "Alert over after %lu ms with no answer; the finished timer stays on the panel",
            static_cast<unsigned long>(elapsedMs));
    // A reader that woke by itself for this goes back to sleep now, whatever its idle timeout is set to.
    if (params_.fromTimerWake) requestDeepSleepSoon();
  }
}

void StepTimerAlertActivity::restoreLight() {
  if (!lightTouched_) return;
  lightTouched_ = false;
  Frontlight.setBrightness(baselineBrightness_);
  Frontlight.setOn(baselineOn_);
  lightOn_ = baselineOn_;
}

void StepTimerAlertActivity::loop() {
  if (dismissed_) return;
  if (!started_) {
    if (esp_timer_get_time() >= waitUntilUs_) begin();
    return;
  }
  const uint32_t elapsed = millis() - startedMs_;
  if (!ended_) stepLight(elapsed);

  // Any press or touch answers the alert, on its release (so the release cannot also be a page turn in whatever comes
  // next), once it has been on the panel long enough to be seen.
  if (rendered_.load(std::memory_order_acquire) && millis() - renderedAtMs_.load() >= DISMISS_GRACE_MS &&
      (gpio.wasAnyReleased() || mappedInput.wasScreenTouchReleased())) {
    dismiss();
  }
}

bool StepTimerAlertActivity::handleHomeGesture() {
  if (params_.after != After::NoCard) return false;
  // Taken either way, so that Home is never built; it answers the alert only once the alert has been on the panel long
  // enough to have been seen, as a press does.
  if (!dismissed_ && started_ && rendered_.load(std::memory_order_acquire) &&
      millis() - renderedAtMs_.load() >= DISMISS_GRACE_MS)
    dismiss();
  return true;
}

void StepTimerAlertActivity::dismiss() {
  dismissed_ = true;
  restoreLight();
  {
    RenderLock lock(*this);
    STEP_TIMER.clear();
    // The timer's box keeps saying that it ended ("Done · 2:32 PM") until the page changes, whatever it was that
    // dismissed this.
    if (params_.after != After::NoCard) {
      steptimer::DoneNote note;
      note.bookPath = rec_.bookPath;
      note.page = rec_.page;
      note.box = steptimer::Rect{rec_.boxX, rec_.boxY, rec_.boxW, rec_.boxH};
      note.label = rec_.label;
      note.endedAt = endedAt_;
      STEP_TIMER.setDone(std::move(note));
    }
  }
  LOG_INF("TIMER", "Alert answered");
  leave();
}

// Where the alert goes when it is over: back to what it was over, or on into the book.
void StepTimerAlertActivity::leave() {
  switch (params_.after) {
    case After::Pop:
      finish();
      break;
    case After::OpenBook:
      if (Storage.exists(rec_.bookPath.c_str())) {
        activityManager.goToReader(rec_.bookPath);
      } else {
        onGoHome();
      }
      break;
    case After::NoCard:
      activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
      break;
    case After::ResumeWake:
      // What a power-button wake would have done: back into the book the reader slept in, unless that is not what this
      // wake can safely do (it slept from Home, or opening the book crashed last time).
      if (sleepframe::resumesBook(APP_STATE.openEpubPath, APP_STATE.lastSleepFromReader, false,
                                  APP_STATE.readerActivityLoadCount)) {
        const std::string path = APP_STATE.openEpubPath;
        LOG_INF("MAIN", "Wake: resuming %s", path.c_str());
        APP_STATE.openEpubPath = "";
        APP_STATE.readerActivityLoadCount++;
        APP_STATE.saveToFile();
        activityManager.goToReader(path);
      } else {
        onGoHome();
      }
      break;
  }
}

void StepTimerAlertActivity::onExit() {
  restoreLight();
  Activity::onExit();
}

void StepTimerAlertActivity::render(RenderLock&&) {
  if (!started_) return;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  using steptimer::Rect;
  using steptimer::view::Mood;

  steptimer::view::Content content = steptimer::view::contentFor(rec_, Mood::Due, 0, params_.lostPower);
  content.hint = content.hint.empty() ? std::string(tr(STR_METHOD_TIMER_DONE)) : content.hint;

  if (params_.retainedFrame) {
    // Over the frame that is on the panel: only the timer changes. Its own box when that page is the one showing,
    // otherwise a banner across the top.
    const Rect box{rec_.boxX, rec_.boxY, rec_.boxW, rec_.boxH};
    const bool onItsPage =
        params_.panelPage == rec_.page && screenW == steptimer::PAGE_W && steptimer::view::boxCanShow(rec_);
    if (onItsPage) {
      steptimer::view::drawBox(renderer, box, content, true);
    } else {
      Rect banner = steptimer::bannerRect();
      banner.w = screenW - 2 * banner.x;
      steptimer::view::drawBox(renderer, banner, content, true);
    }
    // Awake, the controller still holds the frame it last refreshed, so a fast refresh changes only the pixels that
    // differ: the box. After a deep sleep it does not: the panel's controller discards its memory in sleep and its
    // driver makes the first paint after a boot an absolute (half) refresh whatever is asked, so this is the whole
    // panel redrawn once with the box inverted. Asked for explicitly, so that it does not depend on the driver doing
    // so.
    renderer.displayBuffer(params_.after == After::ResumeWake ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  } else {
    // Nothing known is on the panel (a cold start, or the reader slept on a screen with no saved frame): a clean screen
    // of its own.
    renderer.clearScreen();
    const int w = screenW - 64;
    const Rect box{32, (screenH - 220) / 2, w, 220};
    content.hint = std::string(plaque::promptsTouch(plaque::board().caps) ? tr(STR_METHOD_TIMER_DISMISS)
                                                                          : tr(STR_X4C_TIMER_DISMISS));
    steptimer::view::drawBox(renderer, box, content, true);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
  renderedAtMs_.store(millis());
  rendered_.store(true, std::memory_order_release);
}
