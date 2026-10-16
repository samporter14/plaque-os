#include "MessageViewActivity.h"

#include <HalPowerManager.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include "activities/network/WifiSelectionActivity.h"
#include "ambient/AmbientSettings.h"
#include "ambient/LatestMessageProvider.h"
#include "components/UITheme.h"
#include "network/NetworkJoinDevice.h"
#include "network/NetworkProfileDevice.h"

void MessageViewActivity::onEnter() {
  Activity::onEnter();
  if (!checkNow) {
    requestUpdate();
    return;
  }
  if (!AMBIENT_SETTINGS.configured()) {
    state = State::NotConfigured;
    requestUpdate();
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    state = State::Checking;
  } else {
    ownsWifi = true;
    if (netprofile::installed() && netjoin::beginProfileJoin()) {
      state = State::JoiningProfile;
      profileDeadline =
          millis() + netjoin::interactiveProfileJoinMs(netprofile::summary().auth == netprofile::Auth::EapTls);
    } else {
      joinSavedNetworks();
    }
  }
  requestUpdate();
}

void MessageViewActivity::joinSavedNetworks() {
  state = State::WaitingWifi;
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("AMBIENT", "OOM: WiFi selection");
    state = State::Failed;
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    state = result.isCancelled || WiFi.status() != WL_CONNECTED ? State::Failed : State::Checking;
  });
}

void MessageViewActivity::onExit() {
  if (ownsWifi) ambient::disconnectWifi();
  Activity::onExit();
}

void MessageViewActivity::loop() {
  if (state == State::JoiningProfile) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      netjoin::abortProfileJoin();
      finish();
      return;
    }
    const netjoin::Poll poll = netjoin::pollProfileJoin();
    if (poll == netjoin::Poll::Connected) {
      state = State::Checking;
      requestUpdate();
    } else if (poll == netjoin::Poll::Failed || static_cast<int32_t>(millis() - profileDeadline) >= 0) {
      netjoin::abortProfileJoin();
      joinSavedNetworks();
      requestUpdate();
    }
    return;
  }
  if (state == State::Checking) {
    requestUpdateAndWait();
    HalPowerManager::Lock lock;
    const auto result = ambient::LatestMessageProvider::instance().fetchContent();
    if (ownsWifi) ambient::disconnectWifi();
    state = result == ambient::FetchResult::Failed ? State::Failed : State::Showing;
    requestUpdate();
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) finish();
}

void MessageViewActivity::render(RenderLock&&) {
  if (state == State::Showing && ambient::LatestMessageProvider::instance().render(renderer)) return;
  renderer.clearScreen();
  const StrId text = state == State::Checking || state == State::WaitingWifi || state == State::JoiningProfile
                         ? StrId::STR_MESSAGE_CHECKING
                     : state == State::NotConfigured ? StrId::STR_MESSAGE_CONFIGURE
                     : state == State::Failed        ? StrId::STR_MESSAGE_FAILED
                                                     : StrId::STR_MESSAGE_EMPTY;
  GUI.drawPopup(renderer, I18N.get(text));
}
