#include "MethodWake.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace method::wake {
namespace {
uint32_t saturate(const uint64_t value) { return static_cast<uint32_t>(std::min<uint64_t>(value, UINT32_MAX)); }
}  // namespace

Rules rulesFor(const uint32_t remainingMs, std::string protectedPath, const uint32_t blockedBookId) {
  Rules rules;
  rules.active = true;
  rules.budgetMs = remainingMs;
  rules.protectedPath = std::move(protectedPath);
  rules.blockedBookId = blockedBookId;
  return rules;
}

uint32_t budgetLeft(const uint32_t elapsedMs, const bool timerRunning, const int64_t timerRemainingMs) {
  uint64_t left = elapsedMs < BUDGET_MS ? BUDGET_MS - elapsedMs : 0;
  if (timerRunning) {
    constexpr int64_t keep = steptimer::WAKE_LEAD_MS + TIMER_MARGIN_MS;
    const uint64_t room = timerRemainingMs > keep ? static_cast<uint64_t>(timerRemainingMs - keep) : 0;  // no overflow
    left = std::min(left, room);
  }
  return static_cast<uint32_t>(left);
}

uint32_t hardStopGraceMs(const bool timerRunning) {
  return timerRunning ? TIMER_HARD_STOP_GRACE_MS : HARD_STOP_GRACE_MS;
}

uint32_t transferMs(const Rules& rules, const uint32_t bytes) {
  if (rules.bytesPerSec == 0) return UINT32_MAX;
  const uint64_t perSecond = rules.bytesPerSec;
  return saturate((static_cast<uint64_t>(bytes) * 1000 + perSecond - 1) / perSecond);
}

uint32_t neededMs(const Rules& rules, const uint32_t bytes) {
  return saturate(static_cast<uint64_t>(rules.requestMs) + transferMs(rules, bytes) + rules.filingMs);
}

uint32_t remainingMs(const Rules& rules, const uint32_t elapsedMs) {
  return rules.budgetMs > elapsedMs ? rules.budgetMs - elapsedMs : 0;
}

Gate gate(const Rules& rules, const GateInputs& in) {
  if (in.battery < rules.minBatteryPercent) return Gate::Battery;
  // One request is the least a check can do: the list.
  if (in.remainingMs < rules.requestMs) return Gate::NoTime;
  if (in.https && !in.clockReady && in.noteFailed) return Gate::NoClock;
  return Gate::Run;
}

Decision judge(const Rules& rules, const Request& request) {
  Decision decision;
  decision.bookId = request.bookId;
  decision.bytes = request.bytes;
  decision.neededMs = neededMs(rules, request.bytes);
  decision.remainingMs = remainingMs(rules, request.elapsedMs);
  decision.battery = request.battery;
  decision.started = request.started;
  if (request.battery < rules.minBatteryPercent) {
    decision.verdict = Verdict::Battery;
  } else if (rules.blockedBookId != 0 && request.bookId == rules.blockedBookId) {
    decision.verdict = Verdict::Blocked;
  } else if (request.bytes > rules.maxBytes) {
    decision.verdict = Verdict::TooLarge;
  } else if (request.displacesProtected) {
    decision.verdict = Verdict::Protected;
  } else if (request.started >= rules.maxBooks) {
    decision.verdict = Verdict::Limit;
  } else if (decision.neededMs > decision.remainingMs) {
    decision.verdict = Verdict::NoTime;
  } else {
    decision.verdict = Verdict::Download;
  }
  return decision;
}

uint32_t blockedId(const Backoff& backoff) { return backoff.failures >= MAX_FAILED_WAKES ? backoff.bookId : 0; }

void noteFailure(Backoff& backoff, const uint32_t bookId) {
  if (bookId == 0) return;
  if (backoff.bookId != bookId) {
    backoff.bookId = bookId;
    backoff.failures = 1;
  } else if (backoff.failures < UINT8_MAX) {
    ++backoff.failures;
  }
}

void forgetHandled(Backoff& backoff, const uint32_t mark) {
  if (backoff.bookId != 0 && backoff.bookId <= mark) backoff = Backoff{};
}

uint32_t settleWaitMs(const uint32_t sincePollMs) {
  return sincePollMs < KEY_SETTLE_MS ? KEY_SETTLE_MS - sincePollMs : 0;
}

bool TouchWatch::observe(const uint32_t nowMs, const bool contact) {
  switch (state_) {
    case State::Start:
      state_ = contact ? State::Ignoring : State::Idle;
      break;
    case State::Ignoring:
      if (!contact) state_ = State::Idle;
      break;
    case State::Idle:
      if (contact) {
        state_ = State::Pending;
        since_ = nowMs;
      }
      break;
    case State::Pending:
      if (!contact) {
        state_ = State::Idle;  // a blip
      } else if (static_cast<uint32_t>(nowMs - since_) >= TOUCH_CONFIRM_MS) {
        state_ = State::Confirmed;
      }
      break;
    case State::Confirmed:
      break;
  }
  return state_ == State::Confirmed;
}

uint32_t TouchWatch::waitMs(const uint32_t nowMs) const {
  if (state_ != State::Pending) return 0;
  const uint32_t seen = static_cast<uint32_t>(nowMs - since_);
  return seen < TOUCH_CONFIRM_MS ? TOUCH_CONFIRM_MS - seen : 0;
}

uint32_t PressWatch::lastLookWaitMs(const uint32_t nowMs, const uint32_t sincePollMs) const {
  return std::max(settleWaitMs(sincePollMs), touch_.waitMs(nowMs));
}

bool PressWatch::observe(const uint32_t nowMs, const bool keyEdge, const bool keyHeld, const bool keyHeldAgain,
                         const bool touch) {
  const bool touched = touch_.observe(nowMs, touch);
  return keyEdge || (keyHeld && keyHeldAgain) || touched;
}

Leftover leftover(const bool fingerDown, const bool keyOtherThanPowerDown) {
  return {fingerDown, keyOtherThanPowerDown};
}

Ending ending(const bool userActed, const bool noteFetched) {
  if (!userActed) return {false, false, false, false};
  return {true, true, noteFetched, true};
}

const char* gateText(const Gate gate) {
  switch (gate) {
    case Gate::Run:
      return "checking";
    case Gate::Battery:
      return "battery is under the floor";
    case Gate::NoTime:
      return "no time left in the wake budget";
    case Gate::NoClock:
      return "the clock is not synced";
  }
  return "";
}

const char* verdictText(const Verdict verdict) {
  switch (verdict) {
    case Verdict::Download:
      return "download";
    case Verdict::Battery:
      return "battery";
    case Verdict::Blocked:
      return "blocked";
    case Verdict::TooLarge:
      return "too large";
    case Verdict::Protected:
      return "protected";
    case Verdict::Limit:
      return "limit";
    case Verdict::NoTime:
      return "no time";
  }
  return "";
}

size_t describe(const Rules& rules, const Decision& decision, char* out, const size_t size) {
  if (!out || size == 0) return 0;
  const unsigned id = decision.bookId;
  const unsigned bytes = decision.bytes;
  int written = 0;
  switch (decision.verdict) {
    case Verdict::Download:
      written = snprintf(out, size, "b%u: %u B, download (needs %u ms, %u ms left)", id, bytes,
                         static_cast<unsigned>(decision.neededMs), static_cast<unsigned>(decision.remainingMs));
      break;
    case Verdict::Battery:
      written = snprintf(out, size, "b%u: left for the shelf, battery %d%% is under %d%%", id, decision.battery,
                         rules.minBatteryPercent);
      break;
    case Verdict::Blocked:
      written = snprintf(out, size, "b%u: left for the shelf, its download failed on %u earlier wakes", id,
                         static_cast<unsigned>(MAX_FAILED_WAKES));
      break;
    case Verdict::TooLarge:
      written = snprintf(out, size, "b%u: left for the shelf, %u B is over the %u B limit", id, bytes,
                         static_cast<unsigned>(rules.maxBytes));
      break;
    case Verdict::Protected:
      written = snprintf(out, size, "b%u: left for the shelf, filing it would delete the book on the panel", id);
      break;
    case Verdict::Limit:
      written = snprintf(out, size, "b%u: left for the shelf, %u books already this wake", id,
                         static_cast<unsigned>(decision.started));
      break;
    case Verdict::NoTime:
      written = snprintf(out, size, "b%u: left for the shelf, needs %u ms and %u ms are left", id,
                         static_cast<unsigned>(decision.neededMs), static_cast<unsigned>(decision.remainingMs));
      break;
  }
  return written < 0 ? 0 : std::min(static_cast<size_t>(written), size - 1);
}

}  // namespace method::wake
