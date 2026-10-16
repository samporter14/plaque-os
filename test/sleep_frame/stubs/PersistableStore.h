#pragma once

// Host stand-in for the firmware's PersistableStore: the real one reaches for
// Arduino.h and the SD card. CrossPointState only needs the singleton accessor,
// so its JSON round trip can be tested without either.
template <typename T>
class PersistableStore {
 protected:
  PersistableStore() = default;

 public:
  static T& getInstance() {
    static T instance;
    return instance;
  }
};
