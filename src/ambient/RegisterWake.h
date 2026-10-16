#pragma once

#include <cstdint>

#include "RegisterModel.h"

// When the reader wakes for the Register screen, as one pure function. A sleeping Register reader has three reasons
// to wake, and the wake it arms is the soonest of them:
//
//  - the poll: ask the mailbox for the schedule again, at the configured interval;
//  - a boundary: a booking starts or ends, midnight comes, or the reading goes stale (nextBoundary()), so that the
//    panel changes on time with no network at all;
//  - a running step timer's end (Stage C), which the sleepframe wake hook adds on top of whatever is passed in.
//
// The first two are the "ambient" wake, which sleepframe::rearmMicros() combines with the timer's and drops under 15 %
// battery, as it does for the weather. A poll and a boundary both arrive as the same timer wake, so a wake tells them
// apart by the clock: it is a poll wake when the poll is due, a boundary wake otherwise (pollDue()). A boundary wake
// does no network. Pure logic with no Arduino, covered by test/register_wake.
namespace reg {

// A poll that is already due when the reader goes to sleep waits this long, so a reader put to sleep right after a
// poll does not wake again for it at once.
constexpr int64_t MIN_POLL_GAP_S = 60;
// A wake this much before the poll's time still counts as the poll's (the wake timer is not exact). The timer counts on
// the internal clock, which runs up to 2 % off (clockprov::INTERNAL_SLOW_PPM), so a fixed 15 s was enough for 15 minutes
// (18 s of error at 2 %) only by luck and not at all for the longer intervals the settings allow (up to 1440 minutes):
// a wake that early was taken for a boundary wake and the poll waited for the next one. The slack is 2 % of the interval
// plus the floor below.
constexpr int64_t POLL_SLACK_S = 15;
constexpr int64_t POLL_SLACK_PERCENT = 2;
constexpr int64_t pollSlackSeconds(const int64_t intervalS) {
  return POLL_SLACK_S + (intervalS > 0 ? intervalS * POLL_SLACK_PERCENT / 100 : 0);
}

struct WakeInputs {
  int64_t now = 0;              // the clock in seconds; 0 when it cannot be trusted
  bool pollEnabled = false;     // scheduled polling is on and the mailbox is configured
  int64_t pollIntervalS = 900;  // the configured interval
  int64_t pollDueAt = 0;        // when the last wake planned the next poll (kept in RTC memory); 0 for none
  bool boundaryWakes = false;   // the screen may be redrawn: Register is the sleep screen and no Method step is on the panel
  int64_t boundaryAt = 0;       // nextBoundary(); 0 for none
  uint16_t batteryPercent = 100;
};

// Is this wake the poll's? True when polling is on and either the poll is due (within POLL_SLACK_S), nothing says when
// it was planned, the clock cannot say, or the planned time is further off than one interval (the clock has moved).
bool pollDue(const WakeInputs& in);

enum class WakeFor : uint8_t { Nothing, Poll, Boundary, StepTimer };

struct WakePlan {
  uint64_t micros = 0;     // the wake timer to arm; 0 for none
  int64_t pollDueAt = 0;   // the poll's planned time, to keep for the next wake
  WakeFor reason = WakeFor::Nothing;  // which source set `micros` (the poll on a tie)
};

// The wake timer to arm on the way into sleep, from a sleep the user started or from a wake that drew or did nothing.
// The soonest of the poll and the boundary goes through sleepframe::rearmMicros(), which adds the step timer's end (the
// wake hook) and drops the ambient wake when the battery is low.
WakePlan planWake(const WakeInputs& in);

// The boundary to arm for. The screen was decided at `decidedView` (a viewTime(), 0 when nothing was decided this
// boot), a few seconds before the wake is armed (a draw, a poll and a redraw come in between), so a boundary can fall
// in that gap: asking for the one after `now` alone would skip it, and the panel would keep saying what it said until
// the next poll. The earlier of the two views is used, so a boundary the screen has not caught up with is still
// ahead, and planWake() turns one that is already past into the shortest wake, which redraws.
int64_t boundaryForArm(const Schedule& schedule, int64_t decidedView, int64_t now, const Options& options);

// A timer wake that cannot read the card (it would not mount, or the mailbox settings would not load) while Register had
// armed a wake: ending the chain there would leave the panel saying "In use" for ever with no stale footer. It tries
// again after CARD_RETRY_S, up to CARD_RETRY_LIMIT times in a row (an hour), then stops as before. `retries` is the
// count so far; the step timer's end and the battery rule apply as for every wake.
constexpr int64_t CARD_RETRY_S = 300;
constexpr uint8_t CARD_RETRY_LIMIT = 12;
struct CardRetry {
  uint64_t micros = 0;
  uint8_t retries = 0;  // to keep for the next wake
};
CardRetry planCardRetry(bool registerArmed, uint8_t retries, uint16_t batteryPercent);

// The planned time of the next poll once one has finished at `now`.
constexpr int64_t pollDueAfter(const int64_t now, const int64_t intervalS) { return now > 0 ? now + intervalS : 0; }

}  // namespace reg
