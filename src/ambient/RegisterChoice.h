#pragma once

#include <cstdint>

// The sleep-screen choices of Settings > System > Messages, as plain rules (covered by test/register_wake): one sleep
// screen at a time, and a Register screen that is switched on can keep itself current. AmbientSettings applies them when
// it loads; the settings screen applies them when a row is toggled.
namespace reg {

constexpr uint16_t REGISTER_POLL_MINUTES = 15;  // the interval Register switches polling on with

struct SleepChoice {
  bool message = false;         // the latest message as the sleep screen
  bool registerScreen = false;  // the Register screen as the sleep screen
  bool polling = false;         // "Check while sleeping"
  uint16_t pollMinutes = 60;
};

struct Toggled {
  SleepChoice choice;
  bool pollingTurnedOn = false;  // Register was switched on with polling off, and polling was switched on with it
};

// Both set (a hand-edited file): Register is kept, the message screen cleared.
constexpr SleepChoice normalised(SleepChoice c) {
  if (c.registerScreen) c.message = false;
  return c;
}

// Register switched on clears the message screen. With polling off it would never refresh, so polling is switched on at
// REGISTER_POLL_MINUTES; an interval already chosen with polling on is left alone.
constexpr Toggled toggleRegister(SleepChoice c) {
  Toggled out;
  c.registerScreen = !c.registerScreen;
  if (c.registerScreen) {
    c.message = false;
    if (!c.polling) {
      c.polling = true;
      c.pollMinutes = REGISTER_POLL_MINUTES;
      out.pollingTurnedOn = true;
    }
  }
  out.choice = c;
  return out;
}

// The message screen switched on clears Register.
constexpr SleepChoice toggleMessage(SleepChoice c) {
  c.message = !c.message;
  if (c.message) c.registerScreen = false;
  return c;
}

}  // namespace reg
