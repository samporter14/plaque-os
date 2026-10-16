#pragma once

// Just enough of the Arduino core for the SDK's BoardConfig.h to compile on a host: its inline helpers drive pins.
#include <cstddef>
#include <cstdint>
#include <initializer_list>

constexpr int LOW = 0;
constexpr int HIGH = 1;
constexpr int INPUT = 1;
constexpr int OUTPUT = 3;
constexpr int INPUT_PULLUP = 5;
constexpr int INPUT_PULLDOWN = 9;
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return HIGH; }

struct HardwareSerial {
  explicit operator bool() const { return false; }
};
inline HardwareSerial Serial;
inline HardwareSerial Serial0;
