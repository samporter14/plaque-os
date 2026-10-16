#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include "ConnectionCheckDevice.h"
#include "activities/Activity.h"
#include "network/ConnectionCheck.h"

// Settings › System › Check connection: joins the configured network, then checks address, name lookup, clock, TLS
// and the mailbox, showing each stage as it goes (see src/network/ConnectionCheck.h), and saves the result to
// /Lamina/connection-check.json when it ends, a canceled check included.
//
// Keys and touch alike:
//   cancel the check   Back key, header back arrow or back-swipe, or the Cancel button (touch boards)
//   leave              Back again once it has ended; the Home key or swipe leaves at once (and cancels)
//   check again        Confirm, or the Check again button (touch boards)
//
// The check is a step machine stepped from loop(), so input is read on every pass; the only longer pass is the
// request's open, bounded by the 5 s socket timeout per part. Sleep, Home and a step timer's alert all cancel it
// first: the radio is off and the result saved before the next screen takes over. It never touches a book, the
// reader's place in it or a running step timer.
//
// Threading: the check lives on the main loop task; the render task only sees the View published under viewMutex.
class ConnectionCheckActivity final : public Activity {
  struct View {
    conncheck::Status status[conncheck::STAGE_COUNT] = {};
    uint32_t ms[conncheck::STAGE_COUNT] = {};
    int failedStage = -1;
    bool retainedTime = false;
    bool running = false;
    bool ended = false;
    bool saved = false;
    conncheck::Message message = conncheck::Message::None;
    char network[96] = {};
    char fact[192] = {};
  };

  // Declared in destruction order: the check goes before the ports it points at.
  std::unique_ptr<conncheck::DevicePorts> ports;
  std::unique_ptr<conncheck::Check> check;
  conncheck::Config config;

  // Main loop task only.
  uint32_t lastRevision = 0;
  uint32_t enteredAt = 0;
  bool checkWanted = false;  // start once the first frame is on the panel
  bool leaving = false;
  bool resultWritten = false;
  bool resultSaved = false;
  std::atomic<bool> firstPaintDone{false};

  mutable std::mutex viewMutex;
  View published;

  // Render task only.
  View view;

  void startCheck();
  void stepCheck();
  void stopCheck();
  void concludeCheck();
  void publish();
  void describeFailure(const conncheck::Report& report, View& out) const;
  // A step timer has ended and its alert is due or on its way: the check must give way to it.
  static bool stepTimerAlertDue();

  // The touch button under the stages (Cancel while running, Check again after): drawn and hit-tested from the same
  // rectangle. Touch boards only; the keys do the same everywhere.
  struct Button {
    int x = 0, y = 0, w = 0, h = 0;
  };
  Button buttonRect() const;
  bool buttonTapped();

 public:
  ConnectionCheckActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ConnectionCheck", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return check && check->running(); }
  // The power button sleeps the reader even mid-check: the radio and the connection are put away first.
  void prepareForSleep() override {
    leaving = true;
    stopCheck();
  }
  // The Home gesture is handled before loop(): stop the check now so it cannot step once more.
  bool handleHomeGesture() override {
    leaving = true;
    stopCheck();
    return false;
  }
};
