#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "activities/Activity.h"
#include "ambient/StepTimer.h"

// The end of a step timer. Draws the inverted "0:00" over the page (its own timer box when that page is on the panel, a
// banner when it is not), pulses the frontlight where the board has one (plaque::Capabilities::alertLight) and holds
// the reader awake until a press or a touch, for at most ALERT_LIMIT_MS; then the light goes off, the panel keeps the
// finished timer, and the reader goes to sleep as it normally would. On a board without a frontlight (the X4 Classic)
// the inverted box is the whole alert: nothing waits on a light, and a key press answers it.
//
// One activity serves every way a timer can end:
//  - the reader slept and woke itself at the end (After::ResumeWake): main.cpp starts it instead of the reader, over
//    the frame the sleep left on the panel, and a press goes on into the book as any wake does;
//  - the reader is awake in a book (After::Pop): pushed over it, a press goes back to it;
//  - the reader is awake anywhere else (After::OpenBook): it replaces that screen, and a press opens the timer's book;
//  - the card would not mount at the end (After::NoCard): the alert needs nothing from it (a clean screen, the built-in
//    fonts, the record in NVS), and a press goes on to the "SD card error" screen.
// Whoever starts the alert hands over a copy of the timer's record. The timer is marked alerted here, as the alert
// comes up, not before: a screen that restarts the reader as it is left would otherwise leave an alerted timer behind,
// and the next boot would take the alert for one already shown. Until then gate() keeps a second request from starting.
//
// It counts as a reader: a sleep during the alert is a Method sleep, with the finished timer kept on the panel.
class StepTimerAlertActivity final : public Activity {
 public:
  enum class After : uint8_t { Pop, OpenBook, ResumeWake, NoCard };

  struct Params {
    steptimer::Record record;  // the timer that ended, as it was when it did
    After after = After::Pop;
    bool retainedFrame = true;   // the framebuffer holds what the panel shows, so only the timer is drawn over it
    bool quiet = false;          // the end is old news (a late wake, lost power): no light
    bool lostPower = false;      // say that power was lost rather than that time is up
    bool fromTimerWake = false;  // the reader woke by itself for this: when the alert is over it goes back to sleep
    unsigned panelPage = 0;      // 1-based page on the panel, 0 when not known
    int64_t waitMs = 0;          // the end is still this far away (a wake a little early): wait for it
  };

  StepTimerAlertActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Params& params)
      : Activity("StepTimerAlert", renderer, mappedInput), params_(params) {
    gate().request();
  }
  ~StepTimerAlertActivity() override { gate().release(); }

  // True from the moment an alert is created until it is on screen.
  static steptimer::AlertGate& gate();

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return !ended_; }
  // A Home swipe or key would build Home. On the card-less alert that is a screen made from state this boot never
  // read, so there it answers the alert instead and goes on to the card error, as any other press does. (The light
  // panel's swipe is taken by the activity manager before an activity sees it: it opens the panel over the alert, and
  // the panel's saves have no card to write to.)
  bool handleHomeGesture() override;
  bool isReaderActivity() const override { return true; }
  std::string_view openBookPath() const override { return rec_.bookPath; }

 private:
  void begin();
  void stepLight(uint32_t elapsedMs);
  void restoreLight();
  void dismiss();
  void leave();

  Params params_;
  steptimer::Record rec_;  // the timer as it was when the alert began; the stored one is already marked alerted

  int64_t waitUntilUs_ = 0;
  bool started_ = false;
  bool ended_ = false;  // the alert has run its limit
  uint32_t startedMs_ = 0;
  bool alertLight_ = false;  // this board pulses a frontlight for the alert
  bool lightTouched_ = false;
  bool lightOn_ = false;
  bool baselineOn_ = false;
  uint8_t baselineBrightness_ = 0;
  bool dismissed_ = false;
  std::string endedAt_;  // the time of day the timer ended, for the note left in its box

  std::atomic<bool> rendered_{false};
  std::atomic<uint32_t> renderedAtMs_{0};
};
