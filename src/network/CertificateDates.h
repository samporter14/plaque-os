#pragma once

#include <cstdint>

namespace message_tls {
struct UtcDate {
  int year, month, day, hour, minute, second;
};
enum class DateResult { Valid, Expired, Future, InvalidClock, Malformed };
constexpr int64_t MIN_TLS_EPOCH = 1735689600LL;  // 2025-01-01 UTC
constexpr int64_t MAX_TLS_EPOCH = 4102444800LL;  // 2100-01-01 UTC
bool plausibleEpoch(int64_t epoch);
DateResult checkDates(const UtcDate& from, const UtcDate& to, int64_t now);
// For a clock known only to within boundSeconds (a retained clock, see ClockProvenance.h): valid only if the whole
// window [now - bound, now + bound] lies inside [from, to]. Future when the window starts before `from`, Expired when
// it ends after `to`. A bound of 0 is checkDates(); a negative bound is InvalidClock.
DateResult checkDatesWithin(const UtcDate& from, const UtcDate& to, int64_t now, int64_t boundSeconds);
}  // namespace message_tls
