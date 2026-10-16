#include "CertificateDates.h"

#include <ctime>

namespace message_tls {
namespace {
bool validDate(const UtcDate& date) {
  if (date.year < 1 || date.year > 9999 || date.month < 1 || date.month > 12 || date.day < 1 || date.hour < 0 ||
      date.hour > 23 || date.minute < 0 || date.minute > 59 || date.second < 0 || date.second > 59)
    return false;
  constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = date.year % 4 == 0 && (date.year % 100 != 0 || date.year % 400 == 0);
  return date.day <= days[date.month - 1] + (date.month == 2 && leap ? 1 : 0);
}
int compare(const UtcDate& a, const UtcDate& b) {
  if (a.year != b.year) return a.year < b.year ? -1 : 1;
  if (a.month != b.month) return a.month < b.month ? -1 : 1;
  if (a.day != b.day) return a.day < b.day ? -1 : 1;
  if (a.hour != b.hour) return a.hour < b.hour ? -1 : 1;
  if (a.minute != b.minute) return a.minute < b.minute ? -1 : 1;
  if (a.second != b.second) return a.second < b.second ? -1 : 1;
  return 0;
}
}  // namespace
bool plausibleEpoch(const int64_t epoch) { return epoch >= MIN_TLS_EPOCH && epoch < MAX_TLS_EPOCH; }
DateResult checkDates(const UtcDate& from, const UtcDate& to, const int64_t now) {
  if (!plausibleEpoch(now)) return DateResult::InvalidClock;
  if (!validDate(from) || !validDate(to) || compare(from, to) > 0) return DateResult::Malformed;
  const time_t epoch = static_cast<time_t>(now);
  struct tm utc;
  if (static_cast<int64_t>(epoch) != now || !gmtime_r(&epoch, &utc)) return DateResult::InvalidClock;
  const UtcDate current{utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec};
  if (compare(current, from) < 0) return DateResult::Future;
  if (compare(current, to) > 0) return DateResult::Expired;
  return DateResult::Valid;
}
DateResult checkDatesWithin(const UtcDate& from, const UtcDate& to, const int64_t now, const int64_t boundSeconds) {
  if (!plausibleEpoch(now) || boundSeconds < 0 || boundSeconds > MAX_TLS_EPOCH) return DateResult::InvalidClock;
  if (!validDate(from) || !validDate(to) || compare(from, to) > 0) return DateResult::Malformed;
  const auto utcDate = [](const int64_t epoch, UtcDate& out) {
    const time_t seconds = static_cast<time_t>(epoch);
    struct tm utc;
    if (static_cast<int64_t>(seconds) != epoch || !gmtime_r(&seconds, &utc)) return false;
    out = {utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec};
    return true;
  };
  UtcDate earliest{}, latest{};
  if (!utcDate(now - boundSeconds, earliest) || !utcDate(now + boundSeconds, latest)) return DateResult::InvalidClock;
  if (compare(earliest, from) < 0) return DateResult::Future;
  if (compare(latest, to) > 0) return DateResult::Expired;
  return DateResult::Valid;
}
}  // namespace message_tls
