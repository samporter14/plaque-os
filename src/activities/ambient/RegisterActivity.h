#pragma once

#include <atomic>

#include "activities/Activity.h"
#include "ambient/RegisterData.h"

// The Register screen while the reader is awake (Settings > System > Messages > View the register). It draws the same
// screen the sleeping reader holds. The reader's two side buttons (Right is next, Left is previous) and a tap, which is
// next, go round one ring: the screen, the pages of the full day, the screen (reg::nav). So every place has a way out
// with keys alone, and nothing needs touch. Back returns to the screen, and from the screen leaves.
class RegisterActivity final : public Activity {
  // JoiningProfile: the installed network profile is tried first, as Messages does; then the saved networks
  // (WaitingWifi).
  enum class State { JoiningProfile, WaitingWifi, Checking, Showing, Failed, NotConfigured, Empty };
  std::atomic<State> state{State::Showing};
  std::atomic<int> position{0};  // reg::nav: 0 the screen, 1..pages a page of the full day
  std::atomic<int> pages{0};     // how many, as the last render of the full day found
  std::atomic<bool> halfRefresh{true};
  std::atomic<bool> checkFailed{false};  // a check just failed: the saved schedule is shown, and the footer says so
  bool ownsWifi = false;
  bool checkNow;
  uint32_t profileDeadline = 0;

  void joinSavedNetworks();
  void go(int newPosition);
  void failWithSaved();

 public:
  RegisterActivity(GfxRenderer& renderer, MappedInputManager& input, bool checkNow)
      : Activity("Register", renderer, input), checkNow(checkNow) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == State::Checking || state == State::WaitingWifi || state == State::JoiningProfile;
  }
};
