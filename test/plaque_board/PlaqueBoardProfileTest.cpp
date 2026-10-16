#include <BoardConfig.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "PlaqueBoard.h"

// plaque::board() (lib/PlaqueBoard/PlaqueBoardDevice.cpp) against the SDK's real board profiles, built once per
// firmware environment (EXPECT_MODEL: x4pro, x4c, or none for a board Plaque does not run on). It checks the model and
// capabilities the reader derives from BoardConfig::ACTIVE, and, for the X4 Classic, that the SDK gives every logical
// key the screens use (Back, Confirm, Left, Right, Up, Down, Power) a GPIO of its own: the keys-only paths (Confirm
// for the timer menu, a long Confirm to remove a book, Back out of every screen) have a key to run on.

#ifndef EXPECT_MODEL
#error "EXPECT_MODEL must be x4pro, x4c or none"
#endif

namespace {
int checks = 0;

void check(const bool condition, const std::string& name) {
  if (!condition) {
    std::cerr << "FAIL [" << EXPECT_MODEL << "]: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

bool is(const char* model) { return std::strcmp(EXPECT_MODEL, model) == 0; }

// The network profile, its screens and its files are Plaque features: the two Plaque boards offer all of them, and a
// board Plaque does not run on (a C3 X4 or X3, a Sticky, a Paper Mono) none of them.
void checkNetworkFeatures(const plaque::Capabilities& caps, const bool offered) {
  check(plaque::offersNetworkProfile(caps) == offered, "the Network screen and an installed profile");
  check(plaque::writesProfileSummary(caps) == offered, "/Lamina/network-profile.json");
  check(plaque::offersCardImport(caps) == offered, "Import from card");
  check(plaque::offersConnectToMac(caps) == offered, "Connect to Mac");
  check(plaque::offersConnectionCheck(caps) == offered, "Check connection");
  for (const auto kind : {plaque::WifiKind::Open, plaque::WifiKind::Psk, plaque::WifiKind::EapTls})
    check(plaque::supportsWifi(caps, kind) == offered, "every kind of network a profile carries");
}
}  // namespace

int main() {
  const plaque::Board board = plaque::board();
  const plaque::Capabilities& caps = board.caps;
  const auto& input = BoardConfig::ACTIVE.input;

  if (is("x4pro")) {
    check(board.model == plaque::Model::X4Pro, "the X4 Pro profile is the X4 Pro");
    check(caps.touch && caps.homeKey && caps.frontlight && caps.alertLight, "touch, Home key and its light");
    check(caps.method && caps.stepTimers && caps.timedWake && caps.cardIdentity && caps.usbDrive, "Plaque on it");
    // The Pro has only the side keys and power as GPIOs: Back and Confirm come from touch and the Home key.
    check(input.back < 0 && input.confirm < 0 && input.up >= 0 && input.down >= 0 && input.power >= 0,
          "the Pro's own keys: side pair and power");
    check(plaque::promptsTouch(caps), "so its prompts name the tap");
    checkNetworkFeatures(caps, true);
  } else if (is("x4c")) {
    check(board.model == plaque::Model::X4Classic, "the X4 Classic profile is the X4 Classic");
    check(!caps.touch && !caps.homeKey && !caps.frontlight && !caps.alertLight, "no touch, no Home key, no light");
    check(caps.method && caps.mailbox && caps.notes && caps.almanac && caps.scheduledDownloads && caps.timedWake &&
              caps.stepTimers && caps.cardIdentity && caps.usbDrive,
          "every Plaque feature");
    check(BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalButtons, "discrete buttons");
    check(input.back >= 0 && input.confirm >= 0 && input.left >= 0 && input.right >= 0 && input.up >= 0 &&
              input.down >= 0 && input.power >= 0,
          "a key for Back, Confirm, Left, Right, Up, Down and Power");
    check(input.back == 9 && input.confirm == 8 && input.left == 5 && input.right == 2, "bottom keys 9/8/5/2");
    check(input.up == 0 && input.down == 7 && input.power == 3, "side page keys 0/7 and power 3");
    check(!plaque::promptsTouch(caps), "so its prompts name the key");
    // The frontlight's PWM pins are buttons here, and no light driver is compiled in.
    check(BoardConfig::ACTIVE.frontlight.gpio < 0 && !FREEINK_CAP_FRONTLIGHT, "no frontlight pins, no driver");
    check(!FREEINK_CAP_TOUCH && BoardConfig::ACTIVE.touch.controller == BoardConfig::TouchController::None,
          "no touch controller, no driver");
    check(BoardConfig::hasRtc(), "the BM8563 RTC is in the profile");
    checkNetworkFeatures(caps, true);
  } else {
    check(board.model == plaque::Model::Unsupported, "a board Plaque does not run on is unsupported");
    check(!caps.method && !caps.mailbox && !caps.stepTimers && !caps.timedWake && !caps.cardIdentity,
          "no Plaque feature on it: today's behaviour");
    check(caps.touch == (FREEINK_CAP_TOUCH && BoardConfig::hasTouch()), "its touch bit is the profile's");
    check(plaque::armedWakeMicros(caps, 1000000) == 0, "it never arms a wake timer");
    checkNetworkFeatures(caps, false);
  }

  std::cout << "plaque board profile (" << EXPECT_MODEL << "): " << checks << " checks passed\n";
  return 0;
}
