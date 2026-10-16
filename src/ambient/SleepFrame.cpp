#include "SleepFrame.h"

#include "MethodProtocol.h"

namespace sleepframe {

SleepBook resolveSleepBook(const bool methodEnabled, const bool fromReader, const std::string_view remembered,
                           const std::string_view openInReader) {
  if (!remembered.empty()) return {std::string(remembered), false};
  return {std::string(openInReader), methodStepOpen(methodEnabled, fromReader, openInReader)};
}

bool methodStepOpen(const bool methodEnabled, const bool fromReader, const std::string_view bookPath) {
  return methodEnabled && fromReader && method::ownedBookPath(bookPath);
}

SleepPlan planSleep(const SleepInputs& in) {
  if (methodStepOpen(in.methodEnabled, in.fromReader, in.bookPath)) return {Frame::QuickResume, true};
  if (in.ambientAvailable) return {Frame::Ambient, false};
  return {in.quickResumeSetting ? Frame::QuickResume : Frame::Normal, false};
}

WakePlan planWake(const Wake wake, const bool methodStepOnPanel, const bool fetchedNewFrame) {
  if (wake != Wake::Timer) return {false, false, false};
  return {fetchedNewFrame && !methodStepOnPanel, fetchedNewFrame && methodStepOnPanel, methodStepOnPanel};
}

namespace {
WakeHook wakeHook = nullptr;
}

void setWakeHook(const WakeHook hook) { wakeHook = hook; }

uint64_t rearmMicros(const uint64_t pollMicros, const uint16_t batteryPercent) {
  const uint64_t poll = batteryPercent >= MIN_POLL_BATTERY_PERCENT ? pollMicros : 0;
  return wakeHook ? wakeHook(poll) : poll;
}

bool showsSdError(const Wake wake) { return wake != Wake::Timer; }

bool resumesBook(const std::string_view bookPath, const bool lastSleepFromReader, const bool backHeld,
                 const uint8_t readerLoadCount) {
  return !bookPath.empty() && lastSleepFromReader && !backHeld && readerLoadCount == 0;
}

}  // namespace sleepframe
