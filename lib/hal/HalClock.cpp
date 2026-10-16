#include "HalClock.h"

#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <time.h>

HalClock halClock;  // Singleton instance

void HalClock::begin() {
  _available = _sdkRtc.begin();
  LOG_INF("CLK", _available ? "SDK RTC found" : "RTC not found");
}

namespace {
// UTC calendar date -> Unix epoch, no timezone involvement (newlib has no
// timegm). Days-from-civil per Howard Hinnant's algorithm.
time_t epochFromUtc(const Rtc::DateTime& dt) {
  int y = dt.year;
  const int m = dt.month;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * static_cast<unsigned>(m + (m > 2 ? -3 : 9)) + 2u) / 5u + dt.day - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long days = static_cast<long>(era) * 146097L + static_cast<long>(doe) - 719468L;
  return static_cast<time_t>(days) * 86400 + dt.hour * 3600L + dt.minute * 60L + dt.second;
}
}  // namespace

void HalClock::setTimezone(const char* posixTz) {
  setenv("TZ", posixTz && posixTz[0] != '\0' ? posixTz : "UTC0", 1);
  tzset();
  _lastPollMs = 0;  // re-derive local time under the new rule immediately
}

bool HalClock::localTime(struct tm& out) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs == 0 || (now - _lastPollMs) >= CLOCK_POLL_MS) {
    Rtc::DateTime dt;
    if (_sdkRtc.now(dt)) {
      _cachedUtc = epochFromUtc(dt);
      _hasCachedTime = true;
    } else if (!_hasCachedTime) {
      return false;
    }
    _lastPollMs = now != 0 ? now : 1;  // 0 doubles as the invalidation sentinel
  }
  localtime_r(&_cachedUtc, &out);
  return true;
}

bool HalClock::readUtc(int64_t& epoch) const {
  if (!_available) return false;
  Rtc::DateTime dt;
  if (!_sdkRtc.now(dt)) return false;  // I2C error, or VL set
  epoch = static_cast<int64_t>(epochFromUtc(dt));
  return true;
}

bool HalClock::writeUtc(const int64_t epoch) {
  if (!_available || epoch <= 0) return false;
  const time_t seconds = static_cast<time_t>(epoch);
  struct tm utc;
  if (!gmtime_r(&seconds, &utc)) return false;
  Rtc::DateTime dt;
  dt.year = static_cast<uint16_t>(utc.tm_year + 1900);
  dt.month = static_cast<uint8_t>(utc.tm_mon + 1);
  dt.day = static_cast<uint8_t>(utc.tm_mday);
  dt.hour = static_cast<uint8_t>(utc.tm_hour);
  dt.minute = static_cast<uint8_t>(utc.tm_min);
  dt.second = static_cast<uint8_t>(utc.tm_sec);
  dt.weekday = static_cast<uint8_t>(utc.tm_wday);
  if (!_sdkRtc.set(dt)) return false;
  _lastPollMs = 0;  // the next display read fetches the new time
  return true;
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  struct tm local;
  if (!localTime(local)) return false;
  hour = static_cast<uint8_t>(local.tm_hour);
  minute = static_cast<uint8_t>(local.tm_min);
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  struct tm local;
  if (!localTime(local)) return false;

  if (use12Hour) {
    const bool pm = local.tm_hour >= 12;
    int hour12 = local.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, local.tm_min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", local.tm_hour, local.tm_min);
  }
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  // The same sync every other caller makes: the installed profile's time servers (TrustedTime's prepare hook), the
  // display timezone left as it is, and the answer judged by the clock record before anything is written to the chip.
  const uint32_t token = trustedtime::beginSync();
  constexpr int maxAttempts = 50;  // up to 5 seconds
  for (int i = 0; i < maxAttempts; i++) {
    if (trustedtime::syncCompleted(token)) {
      const bool recorded = _recorder != nullptr && _recorder();
      int64_t chip = 0;
      if (recorded && readUtc(chip)) {
        _cachedUtc = static_cast<time_t>(chip);
        _hasCachedTime = true;
      }
      _lastPollMs = 0;
      LOG_INF("CLK", recorded ? "RTC set from the sync" : "The sync did not set the RTC");
      return recorded;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
