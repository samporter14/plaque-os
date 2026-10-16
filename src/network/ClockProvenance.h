#pragma once

#include <cstddef>
#include <cstdint>

// Where the reader's idea of the time comes from, and how far it can be trusted for certificate dates
// (SPEC-UNIVERSITY-WIFI section 7).
//
//   sntp      an SNTP answer this session, used within 90 s (message_tls decides that part)
//   retained  the clock has run without a break since an SNTP sync recorded in NVS and in RTC memory:
//             the same boot, or a deep-sleep wake whose RTC-memory copy of the record matches NVS (the
//             ESP32-S3's internal slow clock kept the time), or the BM8563 RTC chip, set from that sync, still
//             reporting a clean oscillator (its VL flag clear)
//   none      anything else, including the time restored from the NVS floor after a reset or power loss
//
// A retained time is trusted only while its error bound, 2 s + age x drift, is at most 10 minutes and the sync is at
// most 7 days old. Drift is 50 ppm for the crystal RTC chip and 20 000 ppm (2 %) for the internal slow clock. A
// certificate checked against a retained time must be valid over the whole window [now - bound, now + bound].
//
// Under all of it lies the proven floor (trustedtime::provenFloor(): the persisted floor and what accepted syncs and
// the RTC chip raised it to; never the system clock, which runs 2 % fast through a sleep): a retained time is never
// earlier than the floor, and a sync that set the clock back from it is not recorded. A backward-spoofed SNTP reply
// would otherwise be recorded here and trusted for days.
//
// And the floor is only ever raised by a time something other than the network agrees with. Each sync is judged
// (judgeSync): the RTC chip, set from an earlier sync and counting on its own crystal, is the witness. A sync the chip
// agrees with is recorded, marked confirmed, and raises the persisted floor. A sync the chip contradicts, when the
// chip's own time was confirmed before, is used for the session and recorded nowhere. With no witness (the first sync
// ever, a chip that lost its battery) the sync is recorded so that the chip can witness the next one, and the persisted
// floor moves by at most a minute. So one forward-spoofed reply cannot keep the reader's floor in the future: at worst
// it is the first sync, it costs a lockout of at most the minute, and the person can clear it (Settings > System >
// Clock > Reset the clock check).
//
// The decisions are pure (test/clock_provenance). The device side, at the bottom, reads the reset reason, RTC memory,
// NVS and the RTC chip.
namespace clockprov {

enum class Source : uint8_t { None, Sntp, Retained };
const char* sourceName(Source source);  // "none", "sntp", "retained"

enum class Oscillator : uint8_t { InternalSlow, CrystalRtc };

constexpr int64_t BASE_ERROR_MS = 2000;
constexpr int64_t CRYSTAL_RTC_PPM = 50;
// An assumption, not a measurement: the ESP32-S3's internal slow clock is a 136 kHz RC oscillator, and 2 % is a
// generous guess at how far it drifts. The acceptance checklist (section 0b) has the reader measure it; if the real
// figure is higher, this is the number to raise, and the retained clock's reach (about 8.3 hours at 2 %: the bound
// is 2 s + age x 2 %, and 10 minutes allows 598 s / 2 %, so 29 900 s) shrinks with it.
constexpr int64_t INTERNAL_SLOW_PPM = 20000;
constexpr int64_t MAX_BOUND_MS = 10LL * 60 * 1000;
constexpr int64_t MAX_AGE_MS = 7LL * 24 * 60 * 60 * 1000;

int64_t driftPpm(Oscillator oscillator);
// 2 s + age x drift, rounded up to the next millisecond. Age must not be negative.
int64_t errorBoundMs(Oscillator oscillator, int64_t ageMs);
// The bound in whole seconds, rounded up: what a certificate's validity window is widened by.
int64_t boundSeconds(int64_t boundMs);
// Age between 0 and 7 days, and a bound of at most 10 minutes.
bool trustworthy(Oscillator oscillator, int64_t ageMs);

// One SNTP sync, as written to NVS and to RTC memory.
struct SyncRecord {
  static constexpr uint32_t MAGIC = 0x434c4b31;  // "CLK1"
  uint32_t magic = 0;
  uint32_t id = 0;         // random per sync: ties the RTC-memory copy to the NVS copy
  int64_t epoch = 0;       // the UTC second the sync set
  uint64_t rtcMicros = 0;  // the RTC timer at that moment (esp_rtc_get_time_us): counts through deep sleep
  uint8_t chipSet = 0;     // the RTC chip was written from this sync
  uint8_t confirmed = 0;   // the RTC chip agreed with this sync when it was made (a witness, not only the network)
  uint8_t reserved[6] = {};
};
static_assert(sizeof(SyncRecord) == 32, "SyncRecord is stored as a blob; its layout is part of the NVS format");

bool recordValid(const SyncRecord& record);
bool sameRecord(const SyncRecord& a, const SyncRecord& b);

// The most a persisted floor may be ahead of the true time, which makes it the most a trustworthy time may be behind
// the floor: ten minutes. Every way the floor reaches NVS keeps it inside that: a sync is persisted only when the chip
// agrees with it within this much (plus the chip's drift), the chip's own time is persisted less its error bound, and
// an unwitnessed sync moves it by one minute. So a sync that lands further behind the floor than this was spoofed
// backward, or an earlier forward error made the floor wrong and the person can reset it; one that lands closer is
// recorded as it is and the retained time is raised to the floor when it is read. (Not the system clock's 2 % drift
// through a sleep: the floor no longer includes the system clock.)
constexpr int64_t FLOOR_TOLERANCE_S = MAX_BOUND_MS / 1000;
// How far an unwitnessed sync may move the persisted floor.
constexpr int64_t UNWITNESSED_STEP_S = 60;
// A floor an earlier firmware left behind. Before the clock record existed the floor was the raw internal clock,
// persisted at every sleep, and that clock runs fast (up to 2 %): after a long sleep the floor can be minutes or hours
// ahead of true time, and every honest sync would be refused as behind it until real time caught up. So the first sync
// a reader makes with no clock record (right after it replaced such a firmware, or after the record was cleared) may be
// behind the floor by up to this much, and then lowers the floor to itself, once. If the RTC chip answers, it must
// agree with that sync. Once a record exists none of this applies: a sync behind the floor by more than
// FLOOR_TOLERANCE_S is refused as a backward spoof, as it always was.
constexpr int64_t FIRST_SYNC_FLOOR_FORGIVENESS_S = 24LL * 60 * 60;

enum class Verdict : uint8_t {
  Record,       // recorded: RTC memory, NVS, the RTC chip
  SessionOnly,  // the system clock was set and stays so for this session; nothing is recorded
  Refuse,       // the same, and the clock is behind a time the reader has already seen, or not a time at all
};
enum class Reason : uint8_t {
  Witnessed,      // the chip agreed
  FirstSync,      // no earlier record to be witnessed by
  NoWitness,      // an earlier record, but the chip cannot vouch (written by something else, lost power, set back)
  Replaced,       // the chip disagreed with an earlier sync nobody had confirmed: the newer sync replaces it
  ChipDisagrees,  // the chip disagreed with a sync it had confirmed
  BehindFloor,
  FloorAhead,  // the first sync with no record, behind a floor an earlier firmware left ahead of true time (see above)
  Implausible,
};
const char* reasonName(Reason reason);

// Everything judgeSync looks at: the second the system clock was just set to, the proven floor (0: none), the record in
// NVS before this sync, and what the RTC chip says now (read before it is written).
struct SyncInputs {
  int64_t clockNow = 0;
  int64_t floorEpoch = 0;
  SyncRecord previous;
  bool chipValid = false;
  int64_t chipEpoch = 0;
};
struct SyncJudgment {
  Verdict verdict = Verdict::Refuse;
  Reason reason = Reason::Implausible;
  int64_t epoch = 0;  // what to record: the clock as the sync set it (never raised to the floor: the chip is written
                      // with it, and a retained time is raised to the floor when it is read, inside its bound)
  bool confirmed = false;
  int64_t floorTo = 0;       // raise the proven floor (and persist it) to this; 0 leaves it alone
  int64_t lowerFloorTo = 0;  // lower the proven floor (and persist it) to this, once; 0 leaves it alone
  int64_t chipOffsetS = 0;   // chip minus clock when there was a witness, for the log
};
// How far the chip may differ from a sync it witnesses: FLOOR_TOLERANCE_S plus the crystal's drift over `ageSeconds`.
int64_t agreementSeconds(int64_t ageSeconds);
SyncJudgment judgeSync(const SyncInputs& in);

// What the chip can vouch for now, as a floor: its time less its own error bound, so that a chip running fast cannot
// lift the floor past the true time. 0 unless the record was confirmed, written to the chip, and the chip reads at or
// after it.
int64_t vouchedTime(const SyncRecord& record, bool chipValid, int64_t chipEpoch);

// At boot: has the RTC timer counted without a break since the NVS record was made? Only a deep-sleep wake keeps
// RTC memory and the timer; then the RTC-memory copy must match NVS exactly and the timer must not have gone back.
bool timerContinuousOverBoot(bool deepSleepWake, const SyncRecord& rtcCopy, const SyncRecord& nvsCopy,
                             uint64_t rtcNowMicros);

// What the reader may claim from what survived since the last recorded sync, now.
struct Inputs {
  SyncRecord record;             // the NVS record (invalid when there is none)
  bool timerContinuous = false;  // the RTC timer has counted since the record (same boot, or a matching deep sleep)
  uint64_t rtcNowMicros = 0;
  bool chipValid = false;  // the RTC chip answered with its VL flag clear
  int64_t chipEpoch = 0;   // its time, UTC seconds
  int64_t windowMs = 0;    // how long the answer will be used: the bound is taken at the end of it
  int64_t floorEpoch = 0;  // the proven floor, UTC seconds (0: none); a retained time is never behind it
};

struct Estimate {
  bool trusted = false;
  Oscillator oscillator = Oscillator::InternalSlow;
  int64_t nowEpoch = 0;  // UTC seconds
  int64_t ageMs = 0;     // since the sync
  int64_t boundMs = 0;   // at the end of the window
};

// Of the oscillators that kept the time since the sync, the one with the smaller bound that is still trustworthy.
// Each is held to the floor first (clampToFloor): one that cannot be right does not count, and the other may.
Estimate estimate(const Inputs& inputs);
// True time is at least the floor, so a trusted estimate behind it is raised to it, never left behind. An estimate
// behind the floor by more than FLOOR_TOLERANCE_S (or its own error bound, if that is more) cannot be right and is no
// longer trusted. An untrusted estimate, or no floor (0), changes nothing.
Estimate clampToFloor(Estimate estimate, int64_t floorEpoch);

// ---- device side (ClockProvenanceDevice.cpp) ----------------------------------------------------------------------

// Once per boot, after trustedtime::init(): reads the record from NVS and RTC memory and decides whether the RTC timer
// ran through since then, and installs the floor policy into trustedtime (the chip vouches; the system clock does not).
void begin();
// Every successful SNTP sync, on the main task, right after the clock was set: judges it (judgeSync), and when it is
// recorded writes the record to RTC memory and NVS and sets the RTC chip from it. Every path that sets the clock calls
// it (message_tls, the Clock settings sync, the Wi-Fi setup sync), so none writes the chip on its own. Returns what
// became of the sync.
Verdict recordSync();
// The person's "Reset the clock check": forgets the persisted floor, the RAM floor and the retained record (NVS and RTC
// memory), so that the next sync starts the chain again. The chip is left as it is: with no record it vouches for
// nothing.
void resetClockCheck();
// The retained time now, for a use lasting windowMs. Reads the RTC chip (a short I2C transfer) when the record says
// it was set; main task only.
Estimate retained(int64_t windowMs);

}  // namespace clockprov
