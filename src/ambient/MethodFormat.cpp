#include "MethodFormat.h"

#include <cstdio>

namespace method {
namespace {
// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
int64_t daysFromCivil(int year, int month, int day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yearOfEra = year - era * 400;
  const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + dayOfEra - 719468;
}

bool plausibleDate(const LocalTime& time) {
  return time.valid && time.month >= 1 && time.month <= 12 && time.day >= 1 && time.day <= 31 && time.weekday >= 0 &&
         time.weekday <= 6;
}

// snprintf with a format that comes from a translation file. Output is always terminated.
template <typename... Args>
std::string formatted(const char* format, Args... args) {
  char buffer[96];
  const int written = snprintf(buffer, sizeof(buffer), format, args...);
  if (written < 0) return {};
  return std::string(buffer);
}
}  // namespace

int daysFromTo(const LocalTime& from, const LocalTime& to) {
  return static_cast<int>(daysFromCivil(to.year, to.month, to.day) - daysFromCivil(from.year, from.month, from.day));
}

std::string clockText(const LocalTime& time, const bool twelveHour) {
  char buffer[16];
  if (!twelveHour) {
    snprintf(buffer, sizeof(buffer), "%02d:%02d", time.hour, time.minute);
  } else {
    const int hour = time.hour % 12 == 0 ? 12 : time.hour % 12;
    snprintf(buffer, sizeof(buffer), "%d:%02d %s", hour, time.minute, time.hour < 12 ? "AM" : "PM");
  }
  return buffer;
}

std::string receivedText(const LocalTime& received, const LocalTime& now, const bool twelveHour, const Words& words) {
  if (!plausibleDate(received)) return {};
  const int daysAgo = plausibleDate(now) ? daysFromTo(received, now) : -1;
  if (daysAgo == 0) return std::string(words.today) + " " + clockText(received, twelveHour);
  if (daysAgo >= 1 && daysAgo <= 6) return words.weekdays[received.weekday];
  // A week or more ago, or a clock that has gone backwards: the date alone is the honest answer.
  return formatted("%d", received.day) + " " + words.months[received.month - 1];
}

uint32_t stepCount(const uint32_t pageCount) { return pageCount > 0 ? pageCount - 1 : 0; }

Progress progressFor(const int64_t savedPage, const uint32_t pageCount) {
  Progress progress;
  progress.steps = stepCount(pageCount);
  if (savedPage < 0 || pageCount == 0) return progress;
  const int64_t lastPage = static_cast<int64_t>(pageCount) - 1;
  if (lastPage == 0) {
    progress.kind = ProgressKind::Finished;  // a cover-only book that has been opened has been read
  } else if (savedPage >= lastPage) {
    progress.kind = ProgressKind::Finished;  // the last page, or the end-of-book screen after it
  } else if (savedPage > 0) {
    progress.kind = ProgressKind::InProgress;
    progress.step = static_cast<uint32_t>(savedPage);
  }
  return progress;
}

std::string stepsText(const uint32_t pageCount, const Words& words) {
  const uint32_t steps = stepCount(pageCount);
  if (steps == 1) return words.oneStep;
  return formatted(words.steps, static_cast<int>(steps));
}

std::string progressText(const Progress& progress, const Words& words) {
  switch (progress.kind) {
    case ProgressKind::NotStarted:
      return words.notStarted;
    case ProgressKind::InProgress:
      return formatted(words.stepOf, static_cast<int>(progress.step), static_cast<int>(progress.steps));
    case ProgressKind::Finished:
      return words.finished;
  }
  return {};
}

std::string pagesText(const uint32_t pageCount, const Words& words) {
  if (pageCount == 1) return words.onePage;
  return formatted(words.pages, static_cast<int>(pageCount));
}

std::string rowDetail(const Kind kind, const uint32_t pageCount, const LocalTime& received, const LocalTime& now,
                      const bool twelveHour, const int64_t savedPage, const Words& words) {
  constexpr char SEPARATOR[] = " \xC2\xB7 ";  // " · "
  const std::string when = receivedText(received, now, twelveHour, words);
  const std::string receivedPart = when.empty() ? std::string() : formatted(words.received, when.c_str());
  std::string detail;
  if (kindHasSteps(kind)) {
    detail = stepsText(pageCount, words);
    if (!receivedPart.empty()) detail += SEPARATOR + receivedPart;
    detail += SEPARATOR + progressText(progressFor(savedPage, pageCount), words);
    return detail;
  }
  // Tables, plate maps and boards are read straight through: no steps, no progress.
  detail = words.kinds[static_cast<size_t>(kind)];
  detail += SEPARATOR + pagesText(pageCount, words);
  if (!receivedPart.empty()) detail += SEPARATOR + receivedPart;
  return detail;
}

std::string megabytesText(const uint64_t bytes) {
  // Round up to a tenth of a megabyte (decimal): never promise less space than is needed.
  const uint64_t tenths = (bytes + 99999) / 100000;
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%u.%u", static_cast<unsigned>(tenths / 10), static_cast<unsigned>(tenths % 10));
  return buffer;
}

}  // namespace method
