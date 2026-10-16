#pragma once
// The battery-backed RTC chip, scripted (FakeWorld.h Chip).
#include <cstdint>

class HalClock {
 public:
  bool writeUtc(int64_t epoch);
  bool readUtc(int64_t& epoch);
};
extern HalClock halClock;
