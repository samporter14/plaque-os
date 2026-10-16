#include "RegisterWake.h"

#include <algorithm>

#include "SleepFrame.h"

namespace reg {
namespace {
constexpr uint64_t MICROS = 1000000;
// The wake timer is never armed sooner than this, the same floor the step timer keeps.
constexpr int64_t MIN_WAKE_S = 1;

// The planned time of the poll as it stands, from where it was kept and what the clock says.
int64_t plannedPoll(const WakeInputs& in) {
  if (in.now <= 0) return 0;
  if (in.pollDueAt <= 0) return in.now + MIN_POLL_GAP_S;
  const int64_t soonest = in.now + MIN_POLL_GAP_S;
  const int64_t latest = in.now + in.pollIntervalS + pollSlackSeconds(in.pollIntervalS);
  return std::min(std::max(in.pollDueAt, soonest), latest);
}
}  // namespace

bool pollDue(const WakeInputs& in) {
  if (!in.pollEnabled) return false;
  if (in.now <= 0) return true;
  const int64_t slack = pollSlackSeconds(in.pollIntervalS);
  if (in.pollDueAt > in.now + in.pollIntervalS + slack) return true;  // the clock has stepped back
  return in.now + slack >= in.pollDueAt;
}

WakePlan planWake(const WakeInputs& in) {
  WakePlan plan;
  uint64_t pollMicros = 0;
  if (in.pollEnabled) {
    if (in.now > 0) {
      plan.pollDueAt = plannedPoll(in);
      pollMicros = static_cast<uint64_t>(plan.pollDueAt - in.now) * MICROS;
    } else {
      // No clock to anchor on: one interval from now, and the wake after it polls (pollDue() says so for a missing time).
      pollMicros = static_cast<uint64_t>(std::max<int64_t>(in.pollIntervalS, MIN_WAKE_S)) * MICROS;
    }
  }
  uint64_t boundaryMicros = 0;
  // A boundary at or before `now` is one the screen has not caught up with: the shortest wake.
  if (in.boundaryWakes && in.now > 0 && in.boundaryAt > 0) {
    boundaryMicros = static_cast<uint64_t>(std::max<int64_t>(in.boundaryAt - VIEW_LEAD_S - in.now, MIN_WAKE_S)) * MICROS;
  }

  uint64_t ambient = pollMicros;
  if (boundaryMicros != 0 && (ambient == 0 || boundaryMicros < ambient)) ambient = boundaryMicros;
  plan.micros = sleepframe::rearmMicros(ambient, in.batteryPercent);

  if (plan.micros == 0) {
    plan.reason = WakeFor::Nothing;
  } else if (in.batteryPercent < sleepframe::MIN_POLL_BATTERY_PERCENT || ambient == 0 || plan.micros < ambient) {
    plan.reason = WakeFor::StepTimer;
  } else {
    plan.reason = ambient == pollMicros ? WakeFor::Poll : WakeFor::Boundary;
  }
  return plan;
}

int64_t boundaryForArm(const Schedule& schedule, const int64_t decidedView, const int64_t now, const Options& options) {
  const int64_t nowView = viewTime(now);
  const int64_t view = decidedView > 0 && nowView > 0 ? std::min(decidedView, nowView) : nowView;
  return nextBoundary(schedule, view, options);
}

CardRetry planCardRetry(const bool registerArmed, const uint8_t retries, const uint16_t batteryPercent) {
  CardRetry plan;
  const bool again = registerArmed && retries < CARD_RETRY_LIMIT;
  plan.micros = sleepframe::rearmMicros(again ? static_cast<uint64_t>(CARD_RETRY_S) * MICROS : 0, batteryPercent);
  plan.retries = again ? static_cast<uint8_t>(retries + 1) : retries;
  return plan;
}

}  // namespace reg
