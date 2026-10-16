#include <gtest/gtest.h>

#include "network/CertificateDates.h"
#include "network/ClockProvenance.h"

using clockprov::boundSeconds;
using clockprov::errorBoundMs;
using clockprov::Estimate;
using clockprov::estimate;
using clockprov::Inputs;
using clockprov::Oscillator;
using clockprov::Source;
using clockprov::SyncRecord;
using clockprov::timerContinuousOverBoot;
using clockprov::trustworthy;

namespace {
constexpr int64_t SYNC_EPOCH = 1791028800;  // 2026-10-03T12:00:00Z
constexpr uint64_t SYNC_RTC_US = 123456789;
constexpr int64_t HOUR_MS = 3600LL * 1000;
constexpr int64_t DAY_MS = 24 * HOUR_MS;
// The internal slow clock reaches the 10 minute bound at this age: 2 s + age x 2 % = 600 s.
constexpr int64_t INTERNAL_LIMIT_MS = 29900000;

SyncRecord record(bool chipSet = false) {
  SyncRecord r;
  r.magic = SyncRecord::MAGIC;
  r.id = 0x5eed1234;
  r.epoch = SYNC_EPOCH;
  r.rtcMicros = SYNC_RTC_US;
  r.chipSet = chipSet ? 1 : 0;
  return r;
}

Inputs timerInputs(int64_t ageMs, int64_t windowMs = 0) {
  Inputs in;
  in.record = record();
  in.timerContinuous = true;
  in.rtcNowMicros = SYNC_RTC_US + static_cast<uint64_t>(ageMs) * 1000;
  in.windowMs = windowMs;
  return in;
}

Inputs chipInputs(int64_t ageSeconds, int64_t windowMs = 0) {
  Inputs in;
  in.record = record(true);
  in.chipValid = true;
  in.chipEpoch = SYNC_EPOCH + ageSeconds;
  in.windowMs = windowMs;
  return in;
}
}  // namespace

TEST(ClockProvenance, SourceNamesAreTheResultFileTokens) {
  EXPECT_STREQ(clockprov::sourceName(Source::Sntp), "sntp");
  EXPECT_STREQ(clockprov::sourceName(Source::Retained), "retained");
  EXPECT_STREQ(clockprov::sourceName(Source::None), "none");
}

TEST(ClockProvenance, DriftPerClockType) {
  EXPECT_EQ(clockprov::driftPpm(Oscillator::CrystalRtc), 50);
  EXPECT_EQ(clockprov::driftPpm(Oscillator::InternalSlow), 20000);
}

TEST(ClockProvenance, ErrorBoundIsTwoSecondsPlusAgeTimesDrift) {
  EXPECT_EQ(errorBoundMs(Oscillator::CrystalRtc, 0), 2000);
  EXPECT_EQ(errorBoundMs(Oscillator::InternalSlow, 0), 2000);
  EXPECT_EQ(errorBoundMs(Oscillator::CrystalRtc, DAY_MS), 2000 + 4320);       // 86 400 s x 50 ppm
  EXPECT_EQ(errorBoundMs(Oscillator::InternalSlow, HOUR_MS), 2000 + 72000);   // 3 600 s x 2 %
  EXPECT_EQ(errorBoundMs(Oscillator::CrystalRtc, 7 * DAY_MS), 2000 + 30240);  // the 7 day limit: 32 s
  EXPECT_EQ(errorBoundMs(Oscillator::CrystalRtc, 1), 2001);                   // rounded up, never down
  EXPECT_EQ(errorBoundMs(Oscillator::InternalSlow, INTERNAL_LIMIT_MS), 600000);
  EXPECT_EQ(errorBoundMs(Oscillator::InternalSlow, -5), 2000);
}

TEST(ClockProvenance, BoundInWholeSecondsRoundsUp) {
  EXPECT_EQ(boundSeconds(0), 0);
  EXPECT_EQ(boundSeconds(2000), 2);
  EXPECT_EQ(boundSeconds(2001), 3);
  EXPECT_EQ(boundSeconds(600000), 600);
}

TEST(ClockProvenance, InternalClockIsTrustedUntilItsBoundReachesTenMinutes) {
  EXPECT_TRUE(trustworthy(Oscillator::InternalSlow, 0));
  EXPECT_TRUE(trustworthy(Oscillator::InternalSlow, INTERNAL_LIMIT_MS));
  EXPECT_FALSE(trustworthy(Oscillator::InternalSlow, INTERNAL_LIMIT_MS + 1));
}

TEST(ClockProvenance, CrystalClockIsTrustedForSevenDays) {
  EXPECT_TRUE(trustworthy(Oscillator::CrystalRtc, 7 * DAY_MS));
  EXPECT_FALSE(trustworthy(Oscillator::CrystalRtc, 7 * DAY_MS + 1));
  EXPECT_FALSE(trustworthy(Oscillator::CrystalRtc, -1));
}

TEST(ClockProvenance, RecordValidity) {
  EXPECT_TRUE(clockprov::recordValid(record()));
  SyncRecord r = record();
  r.magic = 0;
  EXPECT_FALSE(clockprov::recordValid(r));
  r = record();
  r.epoch = 0;
  EXPECT_FALSE(clockprov::recordValid(r));
  r.epoch = message_tls::MIN_TLS_EPOCH - 1;
  EXPECT_FALSE(clockprov::recordValid(r));
  r.epoch = message_tls::MAX_TLS_EPOCH;
  EXPECT_FALSE(clockprov::recordValid(r));
  EXPECT_FALSE(clockprov::recordValid(SyncRecord{}));  // what RTC memory holds after a reset
}

TEST(ClockProvenance, DeepSleepWakeWithMatchingCopiesKeepsTheTimer) {
  EXPECT_TRUE(timerContinuousOverBoot(true, record(), record(), SYNC_RTC_US + 1000));
  EXPECT_TRUE(timerContinuousOverBoot(true, record(), record(), SYNC_RTC_US));
}

TEST(ClockProvenance, AResetOrPowerLossBreaksTheTimer) {
  // Not a deep-sleep wake: even a record that looks intact does not count.
  EXPECT_FALSE(timerContinuousOverBoot(false, record(), record(), SYNC_RTC_US + 1000));
  // A power-on clears RTC memory.
  EXPECT_FALSE(timerContinuousOverBoot(true, SyncRecord{}, record(), SYNC_RTC_US + 1000));
}

TEST(ClockProvenance, CopiesThatDisagreeBreakTheTimer) {
  SyncRecord other = record();
  other.id ^= 1;
  EXPECT_FALSE(timerContinuousOverBoot(true, other, record(), SYNC_RTC_US + 1000));
  other = record();
  other.epoch += 1;
  EXPECT_FALSE(timerContinuousOverBoot(true, other, record(), SYNC_RTC_US + 1000));
  other = record();
  other.rtcMicros += 1;
  EXPECT_FALSE(timerContinuousOverBoot(true, other, record(), SYNC_RTC_US + 1000));
  other = record(true);
  EXPECT_FALSE(timerContinuousOverBoot(true, other, record(false), SYNC_RTC_US + 1000));
  // No NVS record at all.
  EXPECT_FALSE(timerContinuousOverBoot(true, record(), SyncRecord{}, SYNC_RTC_US + 1000));
  // The timer went back: it was restarted.
  EXPECT_FALSE(timerContinuousOverBoot(true, record(), record(), SYNC_RTC_US - 1));
}

TEST(ClockProvenance, NoRecordIsNone) {
  const Estimate e = estimate(Inputs{});
  EXPECT_FALSE(e.trusted);
}

TEST(ClockProvenance, FloorAfterAResetIsNeverTrusted) {
  // After a reset the clock runs from the NVS floor: the record is there, but the timer restarted and the chip was not
  // set from the sync (or reports a stop), so nothing vouches for the time.
  Inputs in;
  in.record = record(false);
  in.timerContinuous = false;
  in.rtcNowMicros = 5000;
  in.chipValid = true;
  in.chipEpoch = SYNC_EPOCH + 60;
  EXPECT_FALSE(estimate(in).trusted);

  in.record = record(true);
  in.chipValid = false;  // VL flag set: the chip stopped
  EXPECT_FALSE(estimate(in).trusted);

  // A timer that restarted cannot vouch even with the copies matching.
  in = timerInputs(HOUR_MS);
  in.timerContinuous = false;
  EXPECT_FALSE(estimate(in).trusted);
}

TEST(ClockProvenance, InternalTimerEstimate) {
  const Estimate e = estimate(timerInputs(HOUR_MS));
  EXPECT_TRUE(e.trusted);
  EXPECT_EQ(e.oscillator, Oscillator::InternalSlow);
  EXPECT_EQ(e.nowEpoch, SYNC_EPOCH + 3600);
  EXPECT_EQ(e.ageMs, HOUR_MS);
  EXPECT_EQ(e.boundMs, 74000);
}

TEST(ClockProvenance, InternalTimerAtItsLimit) {
  EXPECT_TRUE(estimate(timerInputs(INTERNAL_LIMIT_MS)).trusted);
  EXPECT_FALSE(estimate(timerInputs(INTERNAL_LIMIT_MS + 1)).trusted);
  // Judged at the end of the window it will be used for.
  EXPECT_TRUE(estimate(timerInputs(INTERNAL_LIMIT_MS - 90000, 90000)).trusted);
  EXPECT_FALSE(estimate(timerInputs(INTERNAL_LIMIT_MS - 90000 + 1, 90000)).trusted);
  EXPECT_EQ(estimate(timerInputs(HOUR_MS, 90000)).boundMs, errorBoundMs(Oscillator::InternalSlow, HOUR_MS + 90000));
}

TEST(ClockProvenance, CrystalChipEstimate) {
  const Estimate e = estimate(chipInputs(86400));
  EXPECT_TRUE(e.trusted);
  EXPECT_EQ(e.oscillator, Oscillator::CrystalRtc);
  EXPECT_EQ(e.nowEpoch, SYNC_EPOCH + 86400);
  EXPECT_EQ(e.ageMs, DAY_MS);
  EXPECT_EQ(e.boundMs, 6320);
}

TEST(ClockProvenance, CrystalChipAtItsSevenDayLimit) {
  EXPECT_TRUE(estimate(chipInputs(7 * 86400)).trusted);
  EXPECT_FALSE(estimate(chipInputs(7 * 86400 + 1)).trusted);
  EXPECT_FALSE(estimate(chipInputs(7 * 86400 - 89, 90000)).trusted);
  EXPECT_TRUE(estimate(chipInputs(7 * 86400 - 90, 90000)).trusted);
}

TEST(ClockProvenance, ChipNotSetFromTheSyncOrSetBackIsNotTrusted) {
  Inputs in = chipInputs(60);
  in.record.chipSet = 0;
  EXPECT_FALSE(estimate(in).trusted);
  EXPECT_FALSE(estimate(chipInputs(-1)).trusted);
  in = chipInputs(60);
  in.chipValid = false;
  EXPECT_FALSE(estimate(in).trusted);
}

TEST(ClockProvenance, TheSmallerBoundWins) {
  Inputs in = chipInputs(3600);
  in.timerContinuous = true;
  in.rtcNowMicros = SYNC_RTC_US + static_cast<uint64_t>(HOUR_MS) * 1000;
  const Estimate e = estimate(in);
  EXPECT_TRUE(e.trusted);
  EXPECT_EQ(e.oscillator, Oscillator::CrystalRtc);
  EXPECT_EQ(e.boundMs, 2180);

  // The timer alone when the chip cannot be read.
  in.chipValid = false;
  EXPECT_EQ(estimate(in).oscillator, Oscillator::InternalSlow);

  // The chip alone when the internal clock is past its limit.
  in = chipInputs(10 * 3600);
  in.timerContinuous = true;
  in.rtcNowMicros = SYNC_RTC_US + static_cast<uint64_t>(10 * HOUR_MS) * 1000;
  EXPECT_TRUE(estimate(in).trusted);
  EXPECT_EQ(estimate(in).oscillator, Oscillator::CrystalRtc);
}

TEST(ClockProvenance, AnImplausibleTimeIsNeverTrusted) {
  Inputs in = chipInputs(3600);
  in.record.epoch = message_tls::MAX_TLS_EPOCH - 10;
  in.chipEpoch = message_tls::MAX_TLS_EPOCH + 5;
  EXPECT_FALSE(estimate(in).trusted);
}

TEST(ClockProvenance, UntrustedAnswerStillSaysHowOldTheSyncIs) {
  const Estimate e = estimate(timerInputs(10 * HOUR_MS));
  EXPECT_FALSE(e.trusted);
  EXPECT_EQ(e.ageMs, 10 * HOUR_MS);
}

TEST(ClockProvenance, DeepSleepScenario) {
  // Synced, slept two hours, woke: the copies match, the timer kept counting, the internal clock vouches.
  ASSERT_TRUE(timerContinuousOverBoot(true, record(), record(), SYNC_RTC_US + 2 * 3600ULL * 1000000));
  Estimate e = estimate(timerInputs(2 * HOUR_MS, 90000));
  EXPECT_TRUE(e.trusted);
  EXPECT_EQ(boundSeconds(e.boundMs), 148);  // 2 s + (7 200 s + 90 s) x 2 % = 147.8 s
  // Slept nine hours: past the internal clock's limit.
  e = estimate(timerInputs(9 * HOUR_MS));
  EXPECT_FALSE(e.trusted);
}

TEST(ClockProvenance, WidenedValidityWindowEdges) {
  using message_tls::checkDatesWithin;
  using message_tls::DateResult;
  using message_tls::UtcDate;
  const UtcDate from{2026, 10, 1, 0, 0, 0};  // 1790812800
  const UtcDate to{2027, 1, 1, 0, 0, 0};     // 1798761600
  constexpr int64_t FROM = 1790812800;
  constexpr int64_t TO = 1798761600;
  EXPECT_EQ(checkDatesWithin(from, to, FROM + 600, 600), DateResult::Valid);
  EXPECT_EQ(checkDatesWithin(from, to, FROM + 599, 600), DateResult::Future);
  EXPECT_EQ(checkDatesWithin(from, to, TO - 600, 600), DateResult::Valid);
  EXPECT_EQ(checkDatesWithin(from, to, TO - 599, 600), DateResult::Expired);
  EXPECT_EQ(checkDatesWithin(from, to, FROM, 0), DateResult::Valid);
  EXPECT_EQ(checkDatesWithin(from, to, TO, 0), DateResult::Valid);
  EXPECT_EQ(checkDatesWithin(from, to, FROM - 1, 0), DateResult::Future);
  EXPECT_EQ(checkDatesWithin(from, to, TO + 1, 0), DateResult::Expired);
  // A window wider than the whole validity period never fits.
  const UtcDate instant{2026, 10, 3, 12, 0, 0};
  EXPECT_EQ(checkDatesWithin(instant, instant, SYNC_EPOCH, 1), DateResult::Future);
  EXPECT_EQ(checkDatesWithin(instant, instant, SYNC_EPOCH, 0), DateResult::Valid);
  EXPECT_EQ(checkDatesWithin(from, to, SYNC_EPOCH, -1), DateResult::InvalidClock);
  EXPECT_EQ(checkDatesWithin(from, to, 0, 1), DateResult::InvalidClock);
  EXPECT_EQ(checkDatesWithin({2026, 2, 30, 0, 0, 0}, to, SYNC_EPOCH, 1), DateResult::Malformed);
}

// ---- the retained clock never goes behind the proven floor
// -------------------------------------------------------------

TEST(ClockProvenance, ARetainedTimeBehindTheFloorIsRaisedToIt) {
  // The internal clock says SYNC_EPOCH + 3600; the floor is 40 s past that.
  Inputs in = timerInputs(HOUR_MS);
  in.floorEpoch = SYNC_EPOCH + 3600 + 40;
  const Estimate e = estimate(in);
  EXPECT_TRUE(e.trusted);
  EXPECT_EQ(e.nowEpoch, SYNC_EPOCH + 3600 + 40);
  EXPECT_EQ(e.boundMs, 74000);  // the bound is the oscillator's; the floor only moves the estimate up
  // A floor at or behind the estimate changes nothing.
  in.floorEpoch = SYNC_EPOCH + 3600;
  EXPECT_EQ(estimate(in).nowEpoch, SYNC_EPOCH + 3600);
  in.floorEpoch = SYNC_EPOCH;
  EXPECT_EQ(estimate(in).nowEpoch, SYNC_EPOCH + 3600);
  in.floorEpoch = 0;
  EXPECT_EQ(estimate(in).nowEpoch, SYNC_EPOCH + 3600);
}

TEST(ClockProvenance, ARetainedTimeFarBehindTheFloorCannotBeRight) {
  // The tolerance, exactly: ten minutes behind the floor is still a clock the floor can have got ahead of; one second
  // more is not.
  Inputs in = timerInputs(HOUR_MS);
  in.floorEpoch = SYNC_EPOCH + 3600 + clockprov::FLOOR_TOLERANCE_S;
  EXPECT_TRUE(estimate(in).trusted);
  EXPECT_EQ(estimate(in).nowEpoch, in.floorEpoch);
  in.floorEpoch = SYNC_EPOCH + 3600 + clockprov::FLOOR_TOLERANCE_S + 1;
  EXPECT_FALSE(estimate(in).trusted);
  Inputs chip = chipInputs(3600);
  chip.floorEpoch = SYNC_EPOCH + 3600 + clockprov::FLOOR_TOLERANCE_S;
  EXPECT_TRUE(estimate(chip).trusted);
  chip.floorEpoch = SYNC_EPOCH + 3600 + clockprov::FLOOR_TOLERANCE_S + 1;
  EXPECT_FALSE(estimate(chip).trusted);
}

TEST(ClockProvenance, TheToleranceIsTenMinutesNotADay) {
  // A mutant that widened the tolerance to a day would let a chip set back by an hour, or by a day, pass as "behind the
  // floor by the clock's drift". It must be refused.
  static_assert(clockprov::FLOOR_TOLERANCE_S == 600, "the tolerance is the retained clock's 10 minute bound");
  for (const int64_t behind : {3600LL, 6 * 3600LL, 86400LL, 90 * 86400LL}) {
    Inputs chip = chipInputs(3600);
    chip.floorEpoch = SYNC_EPOCH + 3600 + behind;
    EXPECT_FALSE(estimate(chip).trusted) << behind << " s behind";
    Inputs timer = timerInputs(HOUR_MS);
    timer.floorEpoch = SYNC_EPOCH + 3600 + behind;
    EXPECT_FALSE(estimate(timer).trusted) << behind << " s behind";
  }
}

TEST(ClockProvenance, TheExactChipIsNotVetoedByAFloorThatIsOnlyTheProvenOne) {
  // The reader slept a day. The system clock ran 2 % fast and would read 28 minutes 48 s ahead; it is not the floor any
  // more. The floor is what a sync or the chip vouched for: here the chip's own reading from an hour after the sync.
  constexpr int64_t SLEPT = 24 * 3600;
  Inputs chip = chipInputs(SLEPT);
  chip.floorEpoch = SYNC_EPOCH + 3600;
  const Estimate e = estimate(chip);
  EXPECT_TRUE(e.trusted);
  EXPECT_EQ(e.oscillator, Oscillator::CrystalRtc);
  EXPECT_EQ(e.nowEpoch, SYNC_EPOCH + SLEPT);  // not raised: nothing is ahead of it
  // With the system clock's 2 % in the floor it would have been refused: this is what the monotonic floor did.
  chip.floorEpoch = SYNC_EPOCH + SLEPT + SLEPT / 50;
  EXPECT_FALSE(estimate(chip).trusted);
}

TEST(ClockProvenance, ASourceThatFailsTheFloorLeavesTheOtherOne) {
  // The internal clock ran 100 s fast an hour after the sync; the chip is exact. A floor just past the chip's tolerance
  // rules the chip out, and the timer, closer to it, is raised to the floor.
  Inputs in = chipInputs(3600);
  in.timerContinuous = true;
  in.rtcNowMicros = SYNC_RTC_US + static_cast<uint64_t>(HOUR_MS + 100000) * 1000;
  in.floorEpoch = SYNC_EPOCH + 3600 + clockprov::FLOOR_TOLERANCE_S + 1;
  const Estimate e = estimate(in);
  EXPECT_TRUE(e.trusted);
  EXPECT_EQ(e.oscillator, Oscillator::InternalSlow);
  EXPECT_EQ(e.nowEpoch, in.floorEpoch);
  // A floor further ahead than both tolerances rules out both.
  in.floorEpoch = SYNC_EPOCH + 3600 + 100 + clockprov::FLOOR_TOLERANCE_S + 1;
  EXPECT_FALSE(estimate(in).trusted);
}

TEST(ClockProvenance, AnUntrustedEstimateStaysUntrustedWhateverTheFloor) {
  Inputs in = timerInputs(10 * HOUR_MS);  // past the internal clock's limit
  in.floorEpoch = SYNC_EPOCH;
  EXPECT_FALSE(estimate(in).trusted);
  const Estimate none = clockprov::clampToFloor(Estimate{}, SYNC_EPOCH);
  EXPECT_FALSE(none.trusted);
}

// ---- judging a sync: the chip is the witness ---------------------------------------------------------------------

namespace {
using clockprov::judgeSync;
using clockprov::Reason;
using clockprov::SyncInputs;
using clockprov::SyncJudgment;
using clockprov::Verdict;

constexpr int64_t DAY_S = 86400;
constexpr int64_t TRUE_NOW = SYNC_EPOCH + 3600;  // an hour after the earlier sync

SyncRecord previousRecord(bool chipSet, bool confirmed) {
  SyncRecord r = record(chipSet);
  r.confirmed = confirmed ? 1 : 0;
  return r;
}

// A reader whose last sync was an hour ago, written to the chip, with the floor where that sync left it.
SyncInputs established(int64_t clockNow, bool confirmed = true) {
  SyncInputs in;
  in.clockNow = clockNow;
  in.floorEpoch = SYNC_EPOCH;
  in.previous = previousRecord(true, confirmed);
  in.chipValid = true;
  in.chipEpoch = TRUE_NOW;
  return in;
}
}  // namespace

TEST(ClockProvenance, ReasonNamesAreDistinct) {
  const Reason all[] = {Reason::Witnessed,     Reason::FirstSync,   Reason::NoWitness,  Reason::Replaced,
                        Reason::ChipDisagrees, Reason::BehindFloor, Reason::FloorAhead, Reason::Implausible};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
    EXPECT_NE(clockprov::reasonName(all[i])[0], '\0');
    for (size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j)
      EXPECT_STRNE(clockprov::reasonName(all[i]), clockprov::reasonName(all[j]));
  }
}

TEST(ClockProvenance, TheChipAgreeingMakesASyncConfirmedAndRaisesTheFloor) {
  const SyncJudgment j = judgeSync(established(TRUE_NOW + 2));
  EXPECT_EQ(j.verdict, Verdict::Record);
  EXPECT_EQ(j.reason, Reason::Witnessed);
  EXPECT_TRUE(j.confirmed);
  EXPECT_EQ(j.epoch, TRUE_NOW + 2);
  EXPECT_EQ(j.floorTo, TRUE_NOW + 2);
  EXPECT_EQ(j.chipOffsetS, -2);
  // An unconfirmed earlier sync is confirmed by the chip agreeing with the next one.
  const SyncJudgment first = judgeSync(established(TRUE_NOW, /*confirmed=*/false));
  EXPECT_EQ(first.reason, Reason::Witnessed);
  EXPECT_TRUE(first.confirmed);
  // A sync not past the floor does not move it.
  SyncInputs behind = established(TRUE_NOW);
  behind.floorEpoch = TRUE_NOW + 5;
  EXPECT_EQ(judgeSync(behind).floorTo, 0);
}

TEST(ClockProvenance, AgreementIsTenMinutesPlusTheCrystalsDrift) {
  EXPECT_EQ(clockprov::agreementSeconds(0), 600);
  EXPECT_EQ(clockprov::agreementSeconds(-5), 600);
  EXPECT_EQ(clockprov::agreementSeconds(DAY_S), 605);  // 86 400 s x 50 ppm = 4.32 s, rounded up
  EXPECT_EQ(clockprov::agreementSeconds(365 * DAY_S), 600 + 1577);
  const int64_t limit = clockprov::agreementSeconds(3600);
  SyncInputs in = established(TRUE_NOW + limit);
  EXPECT_EQ(judgeSync(in).reason, Reason::Witnessed);
  in = established(TRUE_NOW - limit);
  EXPECT_EQ(judgeSync(in).reason, Reason::Witnessed);
  EXPECT_EQ(judgeSync(established(TRUE_NOW + limit + 1)).reason, Reason::ChipDisagrees);
  EXPECT_EQ(judgeSync(established(TRUE_NOW - limit - 1)).reason, Reason::ChipDisagrees);
}

TEST(ClockProvenance, AForwardSpoofTheChipContradictsIsUsedForTheSessionOnly) {
  // +1 day, +90 days, +10 years: on a reader whose chip time was confirmed. Nothing is recorded, the floor does not
  // move (so a later true sync is not behind it), and the chip is not rewritten.
  for (const int64_t spoof : {DAY_S, 90 * DAY_S, 3650 * DAY_S}) {
    const SyncJudgment j = judgeSync(established(TRUE_NOW + spoof));
    EXPECT_EQ(j.verdict, Verdict::SessionOnly) << spoof;
    EXPECT_EQ(j.reason, Reason::ChipDisagrees);
    EXPECT_FALSE(j.confirmed);
    EXPECT_EQ(j.floorTo, 0);
  }
  // And after it a true sync is judged against the same floor and recorded.
  const SyncJudgment next = judgeSync(established(TRUE_NOW + 30));
  EXPECT_EQ(next.verdict, Verdict::Record);
}

TEST(ClockProvenance, ABackwardSpoofTheChipContradictsIsUsedForTheSessionOnlyToo) {
  // Not behind the floor, so the floor cannot refuse it; the chip, an hour past the sync, can tell.
  SyncJudgment j = judgeSync(established(TRUE_NOW - 3000));
  EXPECT_EQ(j.verdict, Verdict::SessionOnly);
  EXPECT_EQ(j.reason, Reason::ChipDisagrees);
  EXPECT_EQ(j.floorTo, 0);
  // Behind the floor but inside its tolerance: the same.
  j = judgeSync(established(SYNC_EPOCH - 1));
  EXPECT_EQ(j.verdict, Verdict::SessionOnly);
}

TEST(ClockProvenance, ASyncBeyondTheYear2100OrBeforeTheReaderExistedIsRefused) {
  SyncInputs in = established(message_tls::MAX_TLS_EPOCH);
  EXPECT_EQ(judgeSync(in).verdict, Verdict::Refuse);
  EXPECT_EQ(judgeSync(in).reason, Reason::Implausible);
  in = established(message_tls::MAX_TLS_EPOCH + 100 * 365 * DAY_S);
  EXPECT_EQ(judgeSync(in).verdict, Verdict::Refuse);
  in = established(0);
  EXPECT_EQ(judgeSync(in).reason, Reason::Implausible);
  in = established(message_tls::MIN_TLS_EPOCH - 1);
  EXPECT_EQ(judgeSync(in).reason, Reason::Implausible);
  EXPECT_EQ(judgeSync(in).floorTo, 0);
}

TEST(ClockProvenance, ASyncBehindTheFloorByMoreThanTheToleranceIsRefused) {
  SyncInputs in = established(SYNC_EPOCH);
  in.floorEpoch = SYNC_EPOCH + clockprov::FLOOR_TOLERANCE_S;
  in.chipEpoch = SYNC_EPOCH + 5;
  // Exactly the tolerance behind: recorded as the clock set it (never raised: the chip is written with it).
  SyncJudgment j = judgeSync(in);
  EXPECT_EQ(j.verdict, Verdict::Record);
  EXPECT_EQ(j.epoch, SYNC_EPOCH);
  // One more second: refused, with the reason.
  in.floorEpoch += 1;
  j = judgeSync(in);
  EXPECT_EQ(j.verdict, Verdict::Refuse);
  EXPECT_EQ(j.reason, Reason::BehindFloor);
  EXPECT_EQ(j.floorTo, 0);
  // An hour, a day, six months, a year, the start of the epoch: refused, with or without a witness.
  for (const int64_t back : {3600LL, DAY_S, 180 * DAY_S, 365 * DAY_S}) {
    SyncInputs spoof = established(TRUE_NOW - back);
    spoof.floorEpoch = TRUE_NOW;
    EXPECT_EQ(judgeSync(spoof).verdict, Verdict::Refuse) << back;
    spoof.previous = SyncRecord{};
    EXPECT_EQ(judgeSync(spoof).verdict, Verdict::Refuse) << back;
  }
  SyncInputs old = established(1735689600);
  old.floorEpoch = SYNC_EPOCH;
  EXPECT_EQ(judgeSync(old).verdict, Verdict::Refuse);
  // No floor: nothing to be behind.
  SyncInputs none = established(TRUE_NOW);
  none.floorEpoch = 0;
  EXPECT_EQ(judgeSync(none).verdict, Verdict::Record);
}

// A floor an earlier firmware left ahead of true time (it persisted the raw internal clock at every sleep, which runs
// up to 2 % fast): the first sync of a reader with no clock record is not refused for it, and lowers the floor, once.
namespace {
SyncInputs firstSyncUnderAFloorAhead(int64_t ahead) {
  SyncInputs in;
  in.clockNow = TRUE_NOW;
  in.floorEpoch = TRUE_NOW + ahead;
  return in;
}
}  // namespace

TEST(ClockProvenance, TheFirstSyncAfterAnOlderFirmwareLowersAFloorThatWasLeftAhead) {
  // The reviewer's numbers: 900 s ahead refused 2 honest syncs, 3600 s refused 17, six hours 117.
  for (const int64_t ahead : {601LL, 900LL, 3600LL, 6 * 3600LL, DAY_S}) {
    SyncInputs in = firstSyncUnderAFloorAhead(ahead);
    const SyncJudgment j = judgeSync(in);
    EXPECT_EQ(j.verdict, Verdict::Record) << ahead;
    EXPECT_EQ(j.reason, Reason::FloorAhead) << ahead;
    EXPECT_EQ(j.epoch, TRUE_NOW);
    EXPECT_EQ(j.lowerFloorTo, TRUE_NOW) << "the floor comes down to the sync, no further";
    EXPECT_EQ(j.floorTo, 0);
    EXPECT_FALSE(j.confirmed) << "nothing witnessed it by a record";
    // With the chip answering and agreeing, the same; with a chip that cannot answer, the same.
    in.chipValid = true;
    in.chipEpoch = TRUE_NOW + 3;
    EXPECT_EQ(judgeSync(in).reason, Reason::FloorAhead) << ahead;
    in.chipValid = false;
    EXPECT_EQ(judgeSync(in).reason, Reason::FloorAhead) << ahead;
  }
  // Within the tolerance it is no news: recorded as it always was, the floor left alone.
  for (const int64_t ahead : {1LL, 300LL, clockprov::FLOOR_TOLERANCE_S}) {
    const SyncJudgment j = judgeSync(firstSyncUnderAFloorAhead(ahead));
    EXPECT_EQ(j.verdict, Verdict::Record) << ahead;
    EXPECT_EQ(j.reason, Reason::FirstSync) << ahead;
    EXPECT_EQ(j.lowerFloorTo, 0) << ahead;
  }
}

TEST(ClockProvenance, ButNotByMoreThanADayAndNotAgainstTheChip) {
  // A day and a second behind is refused.
  EXPECT_EQ(judgeSync(firstSyncUnderAFloorAhead(DAY_S + 1)).verdict, Verdict::Refuse);
  EXPECT_EQ(judgeSync(firstSyncUnderAFloorAhead(DAY_S + 1)).reason, Reason::BehindFloor);
  EXPECT_EQ(judgeSync(firstSyncUnderAFloorAhead(180 * DAY_S)).verdict, Verdict::Refuse);
  // A chip that answers and sides with the floor says the network went backward: refused, as a backward spoof.
  for (const int64_t ahead : {3600LL, 6 * 3600LL}) {
    SyncInputs in = firstSyncUnderAFloorAhead(ahead);
    in.chipValid = true;
    in.chipEpoch = in.floorEpoch;
    EXPECT_EQ(judgeSync(in).verdict, Verdict::Refuse) << ahead;
    EXPECT_EQ(judgeSync(in).lowerFloorTo, 0);
    in.chipEpoch = TRUE_NOW + clockprov::FLOOR_TOLERANCE_S + 1;  // just outside the agreement
    EXPECT_EQ(judgeSync(in).verdict, Verdict::Refuse) << ahead;
    in.chipEpoch = TRUE_NOW + clockprov::FLOOR_TOLERANCE_S;  // just inside
    EXPECT_EQ(judgeSync(in).verdict, Verdict::Record) << ahead;
    in.chipEpoch = 0;  // an implausible chip answer is not agreement
    EXPECT_EQ(judgeSync(in).verdict, Verdict::Refuse) << ahead;
  }
  // The implausible and the far future are still not syncs.
  EXPECT_EQ(judgeSync(firstSyncUnderAFloorAhead(0)).reason, Reason::FirstSync);
}

TEST(ClockProvenance, WithARecordABackwardSyncIsRefusedWhateverTheAmount) {
  // The protection against a backward spoof is unchanged once a record exists, however small the step down, with the
  // chip vouching or not, confirmed or not: nothing here is forgiven.
  for (const int64_t back : {601LL, 900LL, 3600LL, 6 * 3600LL, DAY_S, 7 * DAY_S}) {
    for (const bool confirmed : {true, false}) {
      for (const bool chipSet : {true, false}) {
        SyncInputs in = established(TRUE_NOW - back, confirmed);
        in.floorEpoch = TRUE_NOW;
        in.previous.chipSet = chipSet ? 1 : 0;
        const SyncJudgment j = judgeSync(in);
        EXPECT_EQ(j.verdict, Verdict::Refuse) << back << " " << confirmed << " " << chipSet;
        EXPECT_EQ(j.reason, Reason::BehindFloor);
        EXPECT_EQ(j.lowerFloorTo, 0);
        // A chip that cannot answer makes no difference either.
        in.chipValid = false;
        EXPECT_EQ(judgeSync(in).verdict, Verdict::Refuse) << back;
      }
    }
  }
}

TEST(ClockProvenance, TheFirstSyncIsRecordedButMovesTheFloorByAMinuteAtMost) {
  // stampSync recorded the first forward spoof and raised the floor to it. A reader with no record has no witness:
  // the sync is recorded (so the chip can witness the next one), and the persisted floor steps by a minute.
  for (const int64_t spoof : {5LL, 59LL, 60LL, 61LL, DAY_S, 90 * DAY_S, 3650 * DAY_S}) {
    SyncInputs in;
    in.clockNow = SYNC_EPOCH + spoof;
    in.floorEpoch = SYNC_EPOCH;
    const SyncJudgment j = judgeSync(in);
    EXPECT_EQ(j.verdict, Verdict::Record) << spoof;
    EXPECT_EQ(j.reason, Reason::FirstSync);
    EXPECT_FALSE(j.confirmed);
    EXPECT_EQ(j.epoch, SYNC_EPOCH + spoof);
    EXPECT_EQ(j.floorTo, SYNC_EPOCH + (spoof < 60 ? spoof : 60)) << spoof;
  }
  // No floor yet: nothing to step from, so nothing persisted.
  SyncInputs fresh;
  fresh.clockNow = SYNC_EPOCH + 90 * DAY_S;
  EXPECT_EQ(judgeSync(fresh).floorTo, 0);
  EXPECT_EQ(judgeSync(fresh).verdict, Verdict::Record);
}

TEST(ClockProvenance, ASyncNoChipCanWitnessMovesTheFloorByAMinuteAtMost) {
  // An earlier record, but: the chip lost power (VL set), was never written, reads before the record, or is not
  // plausible.
  SyncInputs in = established(SYNC_EPOCH + 90 * DAY_S);
  in.chipValid = false;
  SyncJudgment j = judgeSync(in);
  EXPECT_EQ(j.reason, Reason::NoWitness);
  EXPECT_EQ(j.floorTo, SYNC_EPOCH + 60);
  EXPECT_FALSE(j.confirmed);
  in = established(SYNC_EPOCH + 90 * DAY_S);
  in.previous.chipSet = 0;
  EXPECT_EQ(judgeSync(in).reason, Reason::NoWitness);
  in = established(SYNC_EPOCH + 90 * DAY_S);
  in.chipEpoch = SYNC_EPOCH - 1;  // set back below the record
  EXPECT_EQ(judgeSync(in).reason, Reason::NoWitness);
  EXPECT_EQ(judgeSync(in).floorTo, SYNC_EPOCH + 60);
  in = established(SYNC_EPOCH + 90 * DAY_S);
  in.chipEpoch = message_tls::MAX_TLS_EPOCH;
  EXPECT_EQ(judgeSync(in).reason, Reason::NoWitness);
}

TEST(ClockProvenance, AnUnconfirmedChipTimeIsReplacedNotDefended) {
  // The first sync was the spoofed one (+90 days) and the chip holds it. The true sync that follows disagrees with the
  // chip; nobody confirmed the chip's time, so the new sync replaces it, with a minute's floor step: the reader heals.
  SyncInputs in = established(TRUE_NOW, /*confirmed=*/false);
  in.chipEpoch = TRUE_NOW + 90 * DAY_S;
  in.floorEpoch = SYNC_EPOCH + 60;  // the spoofed first sync's step
  const SyncJudgment j = judgeSync(in);
  EXPECT_EQ(j.verdict, Verdict::Record);
  EXPECT_EQ(j.reason, Reason::Replaced);
  EXPECT_FALSE(j.confirmed);
  EXPECT_EQ(j.epoch, TRUE_NOW);
  EXPECT_EQ(j.floorTo, SYNC_EPOCH + 120);
}

TEST(ClockProvenance, NoSpoofEverMovesTheFloorFurtherThanAWitnessAgrees) {
  // Property: over every earlier state and a range of replies, the floor is raised past a minute's step only for a
  // sync the chip agrees with, and then by no more than the chip is out by plus the agreement tolerance.
  const int64_t offsets[] = {-DAY_S, -3600, -601, -600, -60, -1,   0,     1,          59,
                             60,     61,    599,  600,  601, 3600, DAY_S, 90 * DAY_S, 3650 * DAY_S};
  for (const bool chipSet : {false, true})
    for (const bool confirmed : {false, true})
      for (const bool chipValid : {false, true})
        for (const int64_t chipOffset : {-3600LL, 0LL, 7LL, DAY_S, 90 * DAY_S})
          for (const int64_t offset : offsets) {
            SyncInputs in;
            in.previous = previousRecord(chipSet, confirmed);
            in.floorEpoch = SYNC_EPOCH;
            in.chipValid = chipValid;
            in.chipEpoch = TRUE_NOW + chipOffset;
            in.clockNow = TRUE_NOW + offset;
            const SyncJudgment j = judgeSync(in);
            if (j.verdict != Verdict::Record) {
              EXPECT_EQ(j.floorTo, 0);
              EXPECT_FALSE(j.confirmed);
              continue;
            }
            if (j.floorTo > in.floorEpoch + clockprov::UNWITNESSED_STEP_S) {
              ASSERT_TRUE(j.confirmed) << offset << " " << chipOffset;
              EXPECT_TRUE(chipSet && chipValid);
              const int64_t apart = offset > chipOffset ? offset - chipOffset : chipOffset - offset;
              EXPECT_LE(apart, clockprov::agreementSeconds(3600 + chipOffset));
            }
            EXPECT_LE(j.floorTo, in.clockNow);  // never past the time the sync set
            EXPECT_EQ(j.epoch, in.clockNow);    // and what is recorded is that time, never the floor
          }
}

TEST(ClockProvenance, TheChipVouchesForItsTimeLessItsOwnError) {
  SyncRecord r = previousRecord(true, true);
  // An hour after the sync: bound 2 s + 0.18 s, whole seconds 3.
  EXPECT_EQ(clockprov::vouchedTime(r, true, SYNC_EPOCH + 3600), SYNC_EPOCH + 3600 - 3);
  // Seven days: bound 32.24 s, rounded up to 33.
  EXPECT_EQ(clockprov::vouchedTime(r, true, SYNC_EPOCH + 7 * DAY_S), SYNC_EPOCH + 7 * DAY_S - 33);
  // Never more than the chip reads.
  EXPECT_LT(clockprov::vouchedTime(r, true, SYNC_EPOCH), SYNC_EPOCH);
  // Unconfirmed, not written to the chip, chip unreadable, chip behind the record, implausible: nothing.
  EXPECT_EQ(clockprov::vouchedTime(previousRecord(true, false), true, SYNC_EPOCH + 3600), 0);
  EXPECT_EQ(clockprov::vouchedTime(previousRecord(false, true), true, SYNC_EPOCH + 3600), 0);
  EXPECT_EQ(clockprov::vouchedTime(r, false, SYNC_EPOCH + 3600), 0);
  EXPECT_EQ(clockprov::vouchedTime(r, true, SYNC_EPOCH - 1), 0);
  EXPECT_EQ(clockprov::vouchedTime(r, true, message_tls::MAX_TLS_EPOCH), 0);
  EXPECT_EQ(clockprov::vouchedTime(SyncRecord{}, true, SYNC_EPOCH + 3600), 0);
}

TEST(ClockProvenance, TheConfirmedFlagIsPartOfTheRecordAndSurvivesTheCopyCheck) {
  SyncRecord a = previousRecord(true, true);
  SyncRecord b = a;
  EXPECT_TRUE(clockprov::sameRecord(a, b));
  b.confirmed = 0;
  EXPECT_FALSE(clockprov::sameRecord(a, b));
  EXPECT_FALSE(timerContinuousOverBoot(true, b, a, SYNC_RTC_US + 1000));
  // An old record, written before the flag existed, reads as unconfirmed.
  EXPECT_EQ(SyncRecord{}.confirmed, 0);
}
