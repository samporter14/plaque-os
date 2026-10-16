#pragma once

#include <atomic>

#include "activities/Activity.h"

class MessageViewActivity final : public Activity {
  // JoiningProfile: the installed network profile is tried first; then the saved networks as before (WaitingWifi).
  enum class State { JoiningProfile, WaitingWifi, Checking, Showing, Failed, NotConfigured };
  std::atomic<State> state{State::Showing};
  bool ownsWifi = false;
  bool checkNow;
  uint32_t profileDeadline = 0;

  void joinSavedNetworks();

 public:
  MessageViewActivity(GfxRenderer& renderer, MappedInputManager& input, bool checkNow)
      : Activity("MessageView", renderer, input), checkNow(checkNow) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == State::Checking || state == State::WaitingWifi || state == State::JoiningProfile;
  }
};
