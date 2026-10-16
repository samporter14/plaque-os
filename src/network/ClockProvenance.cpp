#include "ClockProvenance.h"

#include "CertificateDates.h"

namespace clockprov {

const char* sourceName(const Source source) {
  switch (source) {
    case Source::Sntp:
      return "sntp";
    case Source::Retained:
      return "retained";
    case Source::None:
      break;
  }
  return "none";
}

int64_t driftPpm(const Oscillator oscillator) {
  return oscillator == Oscillator::CrystalRtc ? CRYSTAL_RTC_PPM : INTERNAL_SLOW_PPM;
}

int64_t errorBoundMs(const Oscillator oscillator, const int64_t ageMs) {
  const int64_t age = ageMs > 0 ? ageMs : 0;
  return BASE_ERROR_MS + (age * driftPpm(oscillator) + 999999) / 1000000;
}

int64_t boundSeconds(const int64_t boundMs) { return boundMs <= 0 ? 0 : (boundMs + 999) / 1000; }

bool trustworthy(const Oscillator oscillator, const int64_t ageMs) {
  return ageMs >= 0 && ageMs <= MAX_AGE_MS && errorBoundMs(oscillator, ageMs) <= MAX_BOUND_MS;
}

bool recordValid(const SyncRecord& record) {
  return record.magic == SyncRecord::MAGIC && message_tls::plausibleEpoch(record.epoch);
}

bool sameRecord(const SyncRecord& a, const SyncRecord& b) {
  return a.magic == b.magic && a.id == b.id && a.epoch == b.epoch && a.rtcMicros == b.rtcMicros &&
         a.chipSet == b.chipSet && a.confirmed == b.confirmed;
}

const char* reasonName(const Reason reason) {
  switch (reason) {
    case Reason::Witnessed:
      return "the RTC chip agrees";
    case Reason::FirstSync:
      return "first sync";
    case Reason::NoWitness:
      return "the RTC chip cannot vouch";
    case Reason::Replaced:
      return "replaces an unconfirmed sync the RTC chip disagreed with";
    case Reason::ChipDisagrees:
      return "the RTC chip disagrees";
    case Reason::BehindFloor:
      return "behind the clock floor";
    case Reason::FloorAhead:
      return "first sync: the clock floor an earlier firmware left was ahead";
    case Reason::Implausible:
      break;
  }
  return "not a plausible time";
}

int64_t agreementSeconds(const int64_t ageSeconds) {
  const int64_t age = ageSeconds > 0 ? ageSeconds : 0;
  return FLOOR_TOLERANCE_S + (age * CRYSTAL_RTC_PPM + 999999) / 1000000;
}

namespace {
// The record was written to the chip and the chip reads at or after it: it has not been set back or lost its count.
bool chipWitnesses(const SyncRecord& previous, const bool chipValid, const int64_t chipEpoch) {
  return recordValid(previous) && previous.chipSet && chipValid && chipEpoch >= previous.epoch &&
         message_tls::plausibleEpoch(chipEpoch);
}
}  // namespace

SyncJudgment judgeSync(const SyncInputs& in) {
  SyncJudgment out;
  if (!message_tls::plausibleEpoch(in.clockNow)) return out;  // Refuse, Implausible
  if (in.floorEpoch != 0 && in.clockNow < in.floorEpoch - FLOOR_TOLERANCE_S) {
    // Only a reader with no clock record may be forgiven a floor that is ahead of the truth, and only by a day. A chip
    // that answers must agree with the sync: a chip that sides with the floor says the network went backward.
    const int64_t chipApart = in.chipEpoch > in.clockNow ? in.chipEpoch - in.clockNow : in.clockNow - in.chipEpoch;
    const bool chipAgrees =
        !in.chipValid || (message_tls::plausibleEpoch(in.chipEpoch) && chipApart <= FLOOR_TOLERANCE_S);
    if (!recordValid(in.previous) && in.clockNow >= in.floorEpoch - FIRST_SYNC_FLOOR_FORGIVENESS_S && chipAgrees) {
      out.verdict = Verdict::Record;
      out.reason = Reason::FloorAhead;
      out.epoch = in.clockNow;
      out.lowerFloorTo = in.clockNow;
      return out;
    }
    out.reason = Reason::BehindFloor;
    return out;
  }
  out.epoch = in.clockNow;
  const bool witness = chipWitnesses(in.previous, in.chipValid, in.chipEpoch);
  if (witness) {
    out.chipOffsetS = in.chipEpoch - in.clockNow;
    const int64_t apart = out.chipOffsetS < 0 ? -out.chipOffsetS : out.chipOffsetS;
    if (apart <= agreementSeconds(in.chipEpoch - in.previous.epoch)) {
      out.verdict = Verdict::Record;
      out.reason = Reason::Witnessed;
      out.confirmed = true;
      if (in.clockNow > in.floorEpoch) out.floorTo = in.clockNow;
      return out;
    }
    if (in.previous.confirmed) {
      // The chip's time came from syncs that agreed with it before; this one does not. The network is the one that
      // changed: use it for the session, keep the chip's record, persist nothing.
      out.verdict = Verdict::SessionOnly;
      out.reason = Reason::ChipDisagrees;
      return out;
    }
    out.reason = Reason::Replaced;
  } else {
    out.reason = recordValid(in.previous) ? Reason::NoWitness : Reason::FirstSync;
  }
  // Nothing vouched for this sync: record it so that the chip can vouch for the next one, but let it move the persisted
  // floor by one minute at most. (With no floor at all there is nothing to step from.)
  out.verdict = Verdict::Record;
  if (in.floorEpoch != 0 && in.clockNow > in.floorEpoch) {
    const int64_t room = in.clockNow - in.floorEpoch;
    out.floorTo = in.floorEpoch + (room < UNWITNESSED_STEP_S ? room : UNWITNESSED_STEP_S);
  }
  return out;
}

int64_t vouchedTime(const SyncRecord& record, const bool chipValid, const int64_t chipEpoch) {
  if (!record.confirmed || !chipWitnesses(record, chipValid, chipEpoch)) return 0;
  const int64_t vouched =
      chipEpoch - boundSeconds(errorBoundMs(Oscillator::CrystalRtc, (chipEpoch - record.epoch) * 1000));
  return message_tls::plausibleEpoch(vouched) ? vouched : 0;
}

bool timerContinuousOverBoot(const bool deepSleepWake, const SyncRecord& rtcCopy, const SyncRecord& nvsCopy,
                             const uint64_t rtcNowMicros) {
  return deepSleepWake && recordValid(rtcCopy) && recordValid(nvsCopy) && sameRecord(rtcCopy, nvsCopy) &&
         rtcNowMicros >= nvsCopy.rtcMicros;
}

namespace {
// One way the time could have been kept, judged at the end of the window it will be used for.
bool judge(const Oscillator oscillator, const int64_t nowEpoch, const int64_t ageMs, const int64_t windowMs,
           Estimate& out) {
  const int64_t ageAtEnd = ageMs + (windowMs > 0 ? windowMs : 0);
  out.oscillator = oscillator;
  out.nowEpoch = nowEpoch;
  out.ageMs = ageMs;
  out.boundMs = errorBoundMs(oscillator, ageAtEnd);
  out.trusted = message_tls::plausibleEpoch(nowEpoch) && trustworthy(oscillator, ageAtEnd);
  return out.trusted;
}
}  // namespace

Estimate clampToFloor(Estimate estimate, const int64_t floorEpoch) {
  if (!estimate.trusted || floorEpoch <= estimate.nowEpoch) return estimate;
  const int64_t behind = floorEpoch - estimate.nowEpoch;
  const int64_t bound = boundSeconds(errorBoundMs(estimate.oscillator, estimate.ageMs));
  if (behind > (bound > FLOOR_TOLERANCE_S ? bound : FLOOR_TOLERANCE_S)) {
    estimate.trusted = false;
    return estimate;
  }
  estimate.nowEpoch = floorEpoch;
  return estimate;
}

namespace {
Estimate estimateFromSources(const Inputs& inputs) {
  Estimate best;
  if (!recordValid(inputs.record)) return best;
  const SyncRecord& record = inputs.record;

  Estimate timer;
  bool timerOk = false;
  bool timerJudged = false;
  if (inputs.timerContinuous && inputs.rtcNowMicros >= record.rtcMicros) {
    const int64_t ageMs = static_cast<int64_t>((inputs.rtcNowMicros - record.rtcMicros) / 1000);
    judge(Oscillator::InternalSlow, record.epoch + ageMs / 1000, ageMs, inputs.windowMs, timer);
    timer = clampToFloor(timer, inputs.floorEpoch);  // each source is held to the floor before one is chosen
    timerOk = timer.trusted;
    timerJudged = true;
  }

  Estimate chip;
  bool chipOk = false;
  bool chipJudged = false;
  // The chip counts whole seconds from the moment the sync set it; a reading before that moment means it was set
  // back or lost its count, whatever its flag says.
  if (record.chipSet && inputs.chipValid && inputs.chipEpoch >= record.epoch) {
    const int64_t ageMs = (inputs.chipEpoch - record.epoch) * 1000;
    judge(Oscillator::CrystalRtc, inputs.chipEpoch, ageMs, inputs.windowMs, chip);
    chip = clampToFloor(chip, inputs.floorEpoch);
    chipOk = chip.trusted;
    chipJudged = true;
  }

  if (timerOk && chipOk) return chip.boundMs <= timer.boundMs ? chip : timer;
  if (chipOk) return chip;
  if (timerOk) return timer;
  // Nothing to trust; what was judged still says why (its age), for the log.
  if (chipJudged) return chip;
  if (timerJudged) return timer;
  return best;
}
}  // namespace

Estimate estimate(const Inputs& inputs) { return estimateFromSources(inputs); }

}  // namespace clockprov
