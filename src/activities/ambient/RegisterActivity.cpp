#include "RegisterActivity.h"

#include <HalPowerManager.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include "activities/network/WifiSelectionActivity.h"
#include "ambient/AmbientSettings.h"
#include "ambient/LatestMessageProvider.h"
#include "ambient/RegisterNav.h"
#include "ambient/RegisterProvider.h"
#include "components/UITheme.h"
#include "network/NetworkJoinDevice.h"
#include "network/NetworkProfileDevice.h"
#include "util/ButtonNavigator.h"

void RegisterActivity::onEnter() {
  Activity::onEnter();
  auto& provider = ambient::RegisterProvider::instance();
  if (!checkNow) {
    state = provider.loadSchedule() ? State::Showing : State::Empty;
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
    // The same join Messages makes: the installed network profile first, then the saved networks. A reader whose only
    // network is a profile can check the register.
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

void RegisterActivity::joinSavedNetworks() {
  state = State::WaitingWifi;
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("REG", "OOM: WiFi selection");
    failWithSaved();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (!result.isCancelled && WiFi.status() == WL_CONNECTED) {
      state = State::Checking;
      return;
    }
    failWithSaved();
  });
}

// No check could be made (or it failed): the saved schedule is still shown, with the failure in its footer; only when
// there is none is there nothing but the message.
void RegisterActivity::failWithSaved() {
  reg::Schedule saved;
  checkFailed = ambient::RegisterProvider::instance().snapshot(saved);
  state = checkFailed ? State::Showing : State::Failed;
  position = reg::nav::SCREEN;
  halfRefresh = true;
}

void RegisterActivity::onExit() {
  if (ownsWifi) ambient::disconnectWifi();
  Activity::onExit();
}

void RegisterActivity::go(const int newPosition) {
  position = newPosition;
  halfRefresh = newPosition == reg::nav::SCREEN;  // a page turn is a quick refresh; the screen is drawn clean
  requestUpdate();
}

void RegisterActivity::loop() {
  auto& provider = ambient::RegisterProvider::instance();
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
    const auto result = provider.fetch();
    provider.notePolled();
    if (ownsWifi) ambient::disconnectWifi();
    // A failed check still shows the saved schedule, with the failure stated in its footer.
    if (result == ambient::FetchResult::Updated) {
      checkFailed = false;
      state = State::Showing;
      position = reg::nav::SCREEN;
      halfRefresh = true;
    } else {
      failWithSaved();
    }
    requestUpdate();
    return;
  }

  int x = 0;
  int y = 0;
  const bool back = mappedInput.wasReleased(MappedInputManager::Button::Back);
  const bool tap = mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y);
  if (state == State::Showing) {
    if (back) {
      if (position != reg::nav::SCREEN) {
        go(reg::nav::SCREEN);
      } else {
        finish();
      }
      return;
    }
    ButtonNavigator::onNextPress([this] { go(reg::nav::next(position, pages)); });
    ButtonNavigator::onPreviousPress([this] { go(reg::nav::previous(position, pages)); });
    if (tap) go(reg::nav::next(position, pages));
    return;
  }
  if (back || tap) finish();
}

void RegisterActivity::render(RenderLock&&) {
  const State now = state;
  if (now == State::Showing) {
    reg::Schedule schedule;
    if (ambient::RegisterProvider::instance().snapshot(schedule)) {
      const auto words = ambient::registerWords();
      const auto options = ambient::registerOptions(false);
      const int64_t at = reg::viewTime(ambient::registerNow());
      const ambient::RendererMeter meter(renderer);
      const auto orientation = renderer.getOrientation();
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      const char* failed = checkFailed ? tr(STR_REGISTER_CHECK_FAILED) : nullptr;
      reg::Layout layout;
      int shown = reg::nav::SCREEN;
      if (position != reg::nav::SCREEN) {
        reg::FullDay day = reg::buildFullDay(schedule, at, options, words);
        if (failed) {
          day.footer = failed;
          day.stale = true;
        }
        layout = reg::layoutFullDay(day, words, meter, 0);
        pages = layout.pages;
        // LAST_PAGE becomes the last page; a position past the end is the screen again.
        shown = reg::nav::resolved(position, layout.pages);
        if (shown != reg::nav::SCREEN && shown != 1) layout = reg::layoutFullDay(day, words, meter, shown - 1);
      }
      if (shown == reg::nav::SCREEN) {
        reg::Model model = reg::buildModel(schedule, at, options, words);
        if (failed) {
          model.footerKind = reg::FooterKind::Unavailable;
          model.footer = failed;
        }
        layout = reg::layoutScreen(model, words, meter);
      }
      position = shown;
      ambient::drawRegisterLayout(renderer, layout);
      renderer.setOrientation(orientation);
      renderer.displayBuffer(halfRefresh || shown == reg::nav::SCREEN ? HalDisplay::HALF_REFRESH
                                                                      : HalDisplay::FAST_REFRESH);
      return;
    }
  }
  renderer.clearScreen();
  const StrId text = now == State::Checking || now == State::WaitingWifi || now == State::JoiningProfile
                         ? StrId::STR_REGISTER_CHECKING
                     : now == State::NotConfigured ? StrId::STR_MESSAGE_CONFIGURE
                     : now == State::Failed        ? StrId::STR_REGISTER_FAILED
                                                   : StrId::STR_REGISTER_EMPTY;
  GUI.drawPopup(renderer, I18N.get(text));
}
