#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  // The RTC keeps UTC; local time comes from newlib's localtime_r under the
  // POSIX TZ rule set via setTimezone(), so zones with DST are correct
  // year-round. Cached as a UTC epoch to keep the RTC bus quiet.
  mutable time_t _cachedUtc = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Set the POSIX TZ rule (e.g. "CET-1CEST,M3.5.0,M10.5.0/3") applied to every
  // read. nullptr/empty falls back to UTC. Drops the read cache so the change
  // shows immediately.
  void setTimezone(const char* posixTz);

  // Current wall-clock time in the configured timezone.
  // Returns false if RTC is not available.
  bool localTime(struct tm& out) const;

  // Get current local hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format the local time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, bool use12Hour = false) const;

  // The chip's time as a UTC epoch, read now (not from the cache). False when there is no RTC, the read fails, or the
  // chip reports its oscillator stopped since it was last set (the BM8563's VL flag): its count is then not continuous.
  bool readUtc(int64_t& epoch) const;

  // Sets the chip to a UTC epoch, which also clears its VL flag. Only the clock record writes it, from a sync it has
  // judged (clockprov::recordSync): ClockProvenance relies on that, and test/test_clock_chip_writes.py holds the line.
  bool writeUtc(int64_t epoch);

  // What follows every SNTP answer syncFromNTP() gets: the firmware's clock record (clockprov::recordSync), which
  // judges the sync and, when it believes it, writes the chip. True when the chip was set from this sync. The HAL
  // cannot name it (it lives with the network code), so main() installs it. With none installed a sync sets nothing.
  using SyncRecorder = bool (*)();
  void setSyncRecorder(SyncRecorder recorder) { _recorder = recorder; }

  // Sync the RTC from SNTP (the installed network profile's time servers, as every sync uses). Requires WiFi to be
  // connected. Blocks for up to ~5s while waiting for SNTP response. Returns true if the RTC was updated: the sync
  // came, and the clock record believed it and wrote the chip.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

 private:
  SyncRecorder _recorder = nullptr;
};
